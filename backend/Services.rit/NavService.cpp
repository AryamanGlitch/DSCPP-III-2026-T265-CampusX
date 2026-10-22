#include "services/NavService.h"

#include <cmath>
#include <mutex>

#include "util/HttpError.h"
#include "util/Text.h"

namespace cx {

namespace {
constexpr double WALKING_SPEED_MPS = 1.2;  // only for the ETA estimate

double haversine(double lat1, double lng1, double lat2, double lng2) {
    constexpr double R = 6371000, PI = 3.14159265358979323846;
    auto rad = [](double d) { return d * PI / 180.0; };
    double dLat = rad(lat2 - lat1), dLng = rad(lng2 - lng1);
    double a = std::sin(dLat / 2) * std::sin(dLat / 2) +
               std::cos(rad(lat1)) * std::cos(rad(lat2)) * std::sin(dLng / 2) * std::sin(dLng / 2);
    return 2 * R * std::asin(std::sqrt(a));
}

std::string floorLabel(int floor) { return floor == 0 ? "Ground floor" : "Floor " + std::to_string(floor); }

bool isLandmark(const std::string& type) {
    return type == "exit" || type == "entrance" || type == "stairs" || type == "elevator" || type == "assembly" ||
           type == "medical" || type == "refuge" || type == "room";
}

Json nodeJson(const GNode& n) {
    Json j = Json::object({{"id", n.id}, {"name", n.name}, {"type", n.type}});
    if (n.indoor()) {
        j.set("building", n.building).set("floor", n.floor).set("x", n.x).set("y", n.y);
    } else {
        j.set("building", nullptr).set("floor", nullptr).set("lat", n.lat).set("lng", n.lng);
    }
    return j;
}
}  // namespace

NavService::NavService(Database& db) : db_(db) {}

std::string NavService::modeKey(const std::string& type, bool accessible) {
    std::string base;
    if (type == "fire" || type == "earthquake" || type == "evacuation") base = "evacuate";
    else if (type == "medical") base = "medical";
    else throw HttpError(400, "type must be fire, earthquake, evacuation or medical.");
    return accessible ? base + "_accessible" : base;
}

std::vector<NavService::Mode> NavService::buildModes(const RouteOptions& base) {
    // Fire / earthquake: lifts are unsafe. Someone who also can't use stairs
    // may have no way out, so a refuge area (where rescuers collect people)
    // counts as a safe destination for them.
    // Medical: lifts are fine; wheelchair users need them.
    std::vector<Mode> modes;
    auto make = [&](std::string key, std::unordered_set<std::string> targets, bool avoidLifts, bool avoidStairs) {
        Mode m{std::move(key), std::move(targets), base};
        m.options.avoidElevators = avoidLifts;
        m.options.avoidStairs = avoidStairs;
        modes.push_back(std::move(m));
    };
    make("evacuate", {"assembly"}, true, false);
    make("evacuate_accessible", {"assembly", "refuge"}, true, true);
    make("medical", {"medical"}, false, false);
    make("medical_accessible", {"medical"}, false, true);
    return modes;
}

void NavService::reload() {
    Graph graph;
    RouteOptions base;
    std::vector<HazardInfo> hazards;
    Json buildings = Json::array();
    Json contacts = Json::array();

    {
        auto lock = db_.lock();
        auto n = db_.prepare("SELECT id, name, type, building, floor, x, y, lat, lng FROM nav_nodes ORDER BY id");
        while (n.step()) {
            GNode node;
            node.id = n.getText(0);
            node.name = n.getText(1);
            node.type = n.getText(2);
            node.building = n.isNull(3) ? "" : n.getText(3);
            node.floor = n.isNull(4) ? 0 : static_cast<int>(n.getInt(4));
            node.x = n.getDouble(5);
            node.y = n.getDouble(6);
            node.lat = n.getDouble(7);
            node.lng = n.getDouble(8);
            graph.addNode(std::move(node));
        }
        auto e = db_.prepare("SELECT from_id, to_id, weight, kind, one_way FROM nav_edges");
        while (e.step()) {
            graph.addEdge(e.getText(0), e.getText(1), e.getDouble(2), parseEdgeKind(e.getText(3)), e.getInt(4) != 0);
        }
        auto h = db_.prepare(
            "SELECT id, COALESCE(node_id,''), COALESCE(edge_from,''), COALESCE(edge_to,''), description, created_at "
            "FROM hazards WHERE active = 1 ORDER BY created_at");
        while (h.step()) {
            HazardInfo hz{h.getInt(0), h.getText(1), h.getText(2), h.getText(3), h.getText(4), h.getInt(5)};
            if (int idx = graph.indexOf(hz.nodeId); idx >= 0) base.blockedNodes.insert(idx);
            int a = graph.indexOf(hz.edgeFrom), b = graph.indexOf(hz.edgeTo);
            if (a >= 0 && b >= 0) base.blockedEdges.insert(Graph::edgeKey(a, b));
            hazards.push_back(std::move(hz));
        }
        auto bq = db_.prepare("SELECT code, name FROM places WHERE type = 'building' AND code IS NOT NULL ORDER BY name");
        while (bq.step()) {
            Json floors = Json::array();
            auto fq = db_.prepare("SELECT level, name, width, height FROM building_floors WHERE building = ? ORDER BY level");
            fq.bind(1, bq.getText(0));
            while (fq.step()) {
                floors.push(Json::object({{"level", fq.getInt(0)}, {"name", fq.getText(1)},
                                          {"width", fq.getDouble(2)}, {"height", fq.getDouble(3)}}));
            }
            buildings.push(Json::object({{"code", bq.getText(0)}, {"name", bq.getText(1)}, {"floors", floors}}));
        }
        auto cq = db_.prepare("SELECT name, phone, category, available_24x7 FROM contacts ORDER BY priority, id");
        while (cq.step()) {
            contacts.push(Json::object({{"name", cq.getText(0)}, {"phone", cq.getText(1)}, {"category", cq.getText(2)},
                                        {"available24x7", cq.getInt(3) != 0}}));
        }
    }

    // One multi-source Dijkstra per mode.
    std::map<std::string, RoutingTable> tables;
    for (const Mode& m : buildModes(base)) tables[m.key] = graph.buildRoutingTable(m.targets, m.options);

    // Pre-serialise for clients.
    Json nodes = Json::array(), edges = Json::array(), hz = Json::array(), tablesJson = Json::object();
    for (std::size_t i = 0; i < graph.size(); ++i) nodes.push(nodeJson(graph.node(static_cast<int>(i))));
    for (const GEdge& e : graph.edges()) {
        edges.push(Json::object({{"from", graph.node(e.from).id}, {"to", graph.node(e.to).id},
                                 {"weight", e.weight}, {"kind", edgeKindName(e.kind)}, {"oneWay", e.oneWay}}));
    }
    for (const auto& h : hazards) {
        hz.push(Json::object({{"id", h.id}, {"nodeId", h.nodeId.empty() ? Json() : Json(h.nodeId)},
                              {"edgeFrom", h.edgeFrom.empty() ? Json() : Json(h.edgeFrom)},
                              {"edgeTo", h.edgeTo.empty() ? Json() : Json(h.edgeTo)},
                              {"description", h.description}, {"createdAt", h.createdAt}}));
    }
    for (const auto& [key, t] : tables) {
        Json next = Json::object(), dist = Json::object();
        for (std::size_t i = 0; i < graph.size(); ++i) {
            const std::string& id = graph.node(static_cast<int>(i)).id;
            next.set(id, t.next[i] < 0 ? Json() : Json(graph.node(t.next[i]).id));
            dist.set(id, std::isinf(t.dist[i]) ? Json() : Json(std::round(t.dist[i])));
        }
        tablesJson.set(key, Json::object({{"next", next}, {"dist", dist}}));
    }

    std::unique_lock<std::shared_mutex> lock(mu_);
    ++version_;
    Json out = Json::object({{"version", version_}, {"generatedAt", nowMs()}, {"nodes", nodes}, {"edges", edges},
                             {"hazards", hz}, {"buildings", buildings}, {"contacts", contacts}, {"tables", tablesJson}});
    graphJson_ = out.dump();
    graph_ = std::move(graph);
    base_ = std::move(base);
    hazards_ = std::move(hazards);
    tables_ = std::move(tables);
}

std::string NavService::graphJson() const {
    std::shared_lock<std::shared_mutex> lock(mu_);
    return graphJson_;
}

Json NavService::directions(const Route& r) const {
    Json steps = Json::array();
    if (!r.found) return steps;
    auto name = [&](int i) { return graph_.node(i).name; };
    auto step = [&](const char* kind, std::string text, int node) {
        steps.push(Json::object({{"kind", kind}, {"text", std::move(text)}, {"nodeId", graph_.node(node).id}}));
    };
    if (r.segments.empty()) {
        step("arrive", "You are already at " + name(r.target) + ".", r.target);
        return steps;
    }

    double walk = 0;
    auto flush = [&](int node) {
        if (walk > 0) {
            step("walk", "Walk " + std::to_string(static_cast<long long>(std::lround(walk))) + " m to " + name(node), node);
            walk = 0;
        }
    };
    for (std::size_t i = 0; i < r.segments.size(); ++i) {
        const Segment& s = r.segments[i];
        const GNode& a = graph_.node(s.from);
        const GNode& b = graph_.node(s.to);
        bool last = i + 1 == r.segments.size();

        if (s.kind == EdgeKind::Stairs || s.kind == EdgeKind::Elevator) {
            flush(s.from);
            std::string via = s.kind == EdgeKind::Stairs ? "stairs" : "lift";
            std::string dir = b.floor > a.floor ? "up" : "down";
            step(s.kind == EdgeKind::Stairs ? "stairs" : "elevator", "Take the " + via + " " + dir + " to " + floorLabel(b.floor), s.to);
            continue;
        }
        if (a.indoor() && !b.indoor()) {
            flush(s.from);
            step("exit", "Leave the building through " + a.name, s.from);
        }
        if (!a.indoor() && b.indoor()) {
            flush(s.from);
            step("enter", "Enter the building at " + b.name, s.to);
            continue;
        }
        walk += s.weight;
        if (last || isLandmark(b.type)) flush(s.to);
    }
    return steps;
}

Json NavService::routeJson(const Route& r) const {
    Json path = Json::array(), segments = Json::array();
    for (int v : r.path) path.push(graph_.node(v).id);
    for (const auto& s : r.segments) {
        segments.push(Json::object({{"from", graph_.node(s.from).id}, {"to", graph_.node(s.to).id},
                                    {"weight", s.weight}, {"kind", edgeKindName(s.kind)}}));
    }
    const GNode& t = graph_.node(r.target);
    return Json::object({
        {"found", true},
        {"target", Json::object({{"id", t.id}, {"name", t.name}, {"type", t.type}})},
        {"distance", std::round(r.distance)},
        {"etaSeconds", std::lround(r.distance / WALKING_SPEED_MPS)},
        {"path", path},
        {"segments", segments},
        {"steps", directions(r)},
    });
}

Json NavService::route(const std::string& fromId, const std::string& toId, const std::string& type, bool accessible) const {
    std::shared_lock<std::shared_mutex> lock(mu_);
    int from = graph_.indexOf(fromId);
    if (from < 0) throw HttpError(400, "Unknown start location: " + fromId);

    if (!toId.empty()) {
        int to = graph_.indexOf(toId);
        if (to < 0) throw HttpError(400, "Unknown destination: " + toId);
        RouteOptions opt = base_;
        opt.avoidElevators = false;  // everyday directions
        opt.avoidStairs = accessible;
        Route r = graph_.shortestPath(from, to, opt);
        if (!r.found) {
            return Json::object({{"found", false}, {"reason", "No route to that destination with current hazards and settings."}});
        }
        return routeJson(r);
    }

    auto it = tables_.find(modeKey(type, accessible));
    Route r = graph_.followTable(it->second, from);
    if (!r.found) {
        return Json::object({{"found", false},
                             {"reason", "No safe route found. Stay where you are, close doors between you and the hazard, "
                                        "and call an emergency contact."}});
    }
    return routeJson(r);
}

Json NavService::nearest(double lat, double lng) const {
    std::shared_lock<std::shared_mutex> lock(mu_);
    int best = -1;
    double bestDist = 0;
    for (std::size_t i = 0; i < graph_.size(); ++i) {
        const GNode& n = graph_.node(static_cast<int>(i));
        if (n.indoor()) continue;  // GPS can't tell which floor you're on
        double d = haversine(lat, lng, n.lat, n.lng);
        if (best < 0 || d < bestDist) { best = static_cast<int>(i); bestDist = d; }
    }
    if (best < 0) throw HttpError(404, "No outdoor locations are mapped yet.");
    return Json::object({{"node", nodeJson(graph_.node(best))}, {"distance", std::round(bestDist)}});
}

Json NavService::addHazard(const User& by, const std::string& nodeId, const std::string& edgeFrom,
                           const std::string& edgeTo, const std::string& descIn) {
    std::string description = trim(descIn);
    if (description.size() < 3 || description.size() > 200) throw HttpError(400, "Describe the hazard in 3 to 200 characters.");
    bool isNode = !nodeId.empty();
    bool isEdge = !edgeFrom.empty() && !edgeTo.empty();
    if (isNode == isEdge) throw HttpError(400, "Give either nodeId, or both edgeFrom and edgeTo.");
    {
        std::shared_lock<std::shared_mutex> lock(mu_);
        if (isNode && graph_.indexOf(nodeId) < 0) throw HttpError(400, "Unknown location: " + nodeId);
        if (isEdge) {
            int a = graph_.indexOf(edgeFrom), b = graph_.indexOf(edgeTo);
            bool exists = false;
            for (const GEdge& e : graph_.edges()) {
                if ((e.from == a && e.to == b) || (e.from == b && e.to == a)) { exists = true; break; }
            }
            if (a < 0 || b < 0 || !exists) throw HttpError(400, "No path segment between those two locations.");
        }
    }
    long long now = nowMs();
    long long id;
    {
        auto lock = db_.lock();
        db_.prepare("INSERT INTO hazards (node_id, edge_from, edge_to, description, reported_by, created_at) VALUES (?, ?, ?, ?, ?, ?)")
            .bindOptional(1, nodeId).bindOptional(2, isEdge ? edgeFrom : "").bindOptional(3, isEdge ? edgeTo : "")
            .bind(4, description).bind(5, by.id).bind(6, now).run();
        id = db_.lastInsertId();
    }
    reload();
    return Json::object({{"id", id}, {"nodeId", isNode ? Json(nodeId) : Json()}, {"description", description}, {"createdAt", now}});
}

void NavService::resolveHazard(long long id) {
    {
        auto lock = db_.lock();
        db_.prepare("UPDATE hazards SET active = 0, resolved_at = ? WHERE id = ? AND active = 1").bind(1, nowMs()).bind(2, id).run();
        if (db_.changes() == 0) throw HttpError(404, "Active hazard not found.");
    }
    reload();
}

Json NavService::health() const {
    std::shared_lock<std::shared_mutex> lock(mu_);
    auto components = graph_.connectedComponents();
    Json comps = Json::array();
    for (const auto& c : components) {
        Json ids = Json::array();
        for (std::size_t i = 0; i < c.size() && i < 10; ++i) ids.push(graph_.node(c[i]).id);
        comps.push(Json::object({{"size", c.size()}, {"sample", ids}}));
    }
    Json unreachable = Json::object();
    for (const auto& [key, t] : tables_) {
        Json ids = Json::array();
        for (std::size_t i = 0; i < graph_.size(); ++i) {
            if (std::isinf(t.dist[i])) ids.push(graph_.node(static_cast<int>(i)).id);
        }
        unreachable.set(key, ids);
    }
    return Json::object({{"nodes", graph_.size()}, {"edges", graph_.edges().size()},
                         {"connectedComponents", components.size()}, {"components", comps},
                         {"unreachable", unreachable}, {"activeHazards", hazards_.size()}, {"version", version_}});
}

}  // namespace cx
