#include "dsa/Graph.h"
#include <algorithm>
#include <stdexcept>
#include "dsa/MinHeap.h"

namespace cx {

const char* edgeKindName(EdgeKind k) {
    switch (k) {
        case EdgeKind::Stairs: return "stairs";
        case EdgeKind::Elevator: return "elevator";
        case EdgeKind::Ramp: return "ramp";
        default: return "walk";
    }
}

EdgeKind parseEdgeKind(const std::string& s) {
    if (s == "stairs") return EdgeKind::Stairs;
    if (s == "elevator") return EdgeKind::Elevator;
    if (s == "ramp") return EdgeKind::Ramp;
    return EdgeKind::Walk;
}

long long Graph::edgeKey(int a, int b) {
    if (a > b) std::swap(a, b);
    return (static_cast<long long>(a) << 32) | static_cast<unsigned int>(b);
}

int Graph::addNode(GNode node) {
    if (index_.count(node.id)) throw std::invalid_argument("duplicate node id: " + node.id);
    int idx = static_cast<int>(nodes_.size());
    index_[node.id] = idx;
    nodes_.push_back(std::move(node));
    out_.emplace_back();
    in_.emplace_back();
    return idx;
}

void Graph::addEdge(const std::string& from, const std::string& to, double weight, EdgeKind kind, bool oneWay) {
    int a = indexOf(from), b = indexOf(to);
    if (a < 0 || b < 0) throw std::invalid_argument("edge references unknown node: " + from + " -> " + to);
    if (!(weight >= 0)) throw std::invalid_argument("edge weight must be >= 0: " + from + " -> " + to);
    int id = static_cast<int>(edges_.size());
    edges_.push_back({a, b, weight, kind, oneWay});
    out_[a].push_back(id);
    in_[b].push_back(id);
    if (!oneWay) {
        out_[b].push_back(id);
        in_[a].push_back(id);
    }
}

int Graph::indexOf(const std::string& id) const {
    auto it = index_.find(id);
    return it == index_.end() ? -1 : it->second;
}

bool Graph::usable(const GEdge& e, const RouteOptions& opt) const {
    if (opt.avoidElevators && e.kind == EdgeKind::Elevator) return false;
    if (opt.avoidStairs && e.kind == EdgeKind::Stairs) return false;
    return opt.blockedEdges.count(edgeKey(e.from, e.to)) == 0;
}

// Heap entry: (distance so far, node). Lazy deletion: a node can be pushed
// several times; stale entries are skipped when popped.
struct QItem {
    double dist;
    int node;
    bool operator<(const QItem& o) const { return dist < o.dist || (dist == o.dist && node < o.node); }
};

template <typename IsGoal>
Route Graph::dijkstra(int from, IsGoal isGoal, const RouteOptions& opt) const {
    Route route;
    if (from < 0 || from >= static_cast<int>(nodes_.size())) return route;

    const std::size_t n = nodes_.size();
    std::vector<double> dist(n, RoutingTable::INF);
    std::vector<int> prevEdge(n, -1);
    std::vector<bool> done(n, false);
    MinHeap<QItem> heap;

    dist[from] = 0;
    heap.push({0, from});
    while (!heap.empty()) {
        QItem cur = heap.pop();
        if (done[cur.node]) continue;
        done[cur.node] = true;

        if (isGoal(cur.node)) {
            route.found = true;
            route.target = cur.node;
            route.distance = cur.dist;
            for (int v = cur.node; v != from;) {  // walk the predecessor edges back to the start
                const GEdge& e = edges_[prevEdge[v]];
                int u = otherEnd(e, v);
                route.segments.push_back({u, v, e.weight, e.kind});
                v = u;
            }
            std::reverse(route.segments.begin(), route.segments.end());
            route.path.push_back(from);
            for (const auto& s : route.segments) route.path.push_back(s.to);
            return route;
        }

        for (int edgeId : out_[cur.node]) {
            const GEdge& e = edges_[edgeId];
            if (!usable(e, opt)) continue;
            int next = otherEnd(e, cur.node);
            // You may START on a blocked node (you are already there) but never walk into one.
            if (done[next] || opt.blockedNodes.count(next)) continue;
            double nd = cur.dist + e.weight;
            if (nd < dist[next]) {
                dist[next] = nd;
                prevEdge[next] = edgeId;
                heap.push({nd, next});
            }
        }
    }
    return route;  // found = false
}

Route Graph::shortestPath(int from, int to, const RouteOptions& opt) const {
    if (to < 0 || opt.blockedNodes.count(to)) return Route{};
    return dijkstra(from, [to](int v) { return v == to; }, opt);
}

Route Graph::nearestOfType(int from, const std::unordered_set<std::string>& types, const RouteOptions& opt) const {
    return dijkstra(from, [&](int v) { return types.count(nodes_[v].type) > 0; }, opt);
}

RoutingTable Graph::buildRoutingTable(const std::unordered_set<std::string>& types, const RouteOptions& opt) const {
    const std::size_t n = nodes_.size();
    RoutingTable t;
    t.dist.assign(n, RoutingTable::INF);
    t.next.assign(n, -1);
    t.target.assign(n, -1);
    t.nextKind.assign(n, EdgeKind::Walk);
    t.nextWeight.assign(n, 0);

    std::vector<bool> done(n, false);
    MinHeap<QItem> heap;
    for (std::size_t i = 0; i < n; ++i) {
        if (types.count(nodes_[i].type) && !opt.blockedNodes.count(static_cast<int>(i))) {
            t.dist[i] = 0;
            t.target[i] = static_cast<int>(i);
            heap.push({0, static_cast<int>(i)});
        }
    }

    // Walk edges BACKWARDS: arriving at `x` from `y` means the real move is y -> x.
    while (!heap.empty()) {
        QItem cur = heap.pop();
        if (done[cur.node]) continue;
        done[cur.node] = true;
        for (int edgeId : in_[cur.node]) {
            const GEdge& e = edges_[edgeId];
            if (!usable(e, opt)) continue;
            int y = otherEnd(e, cur.node);
            if (done[y] || opt.blockedNodes.count(y)) continue;
            double nd = cur.dist + e.weight;
            if (nd < t.dist[y]) {
                t.dist[y] = nd;
                t.next[y] = cur.node;
                t.target[y] = t.target[cur.node];
                t.nextKind[y] = e.kind;
                t.nextWeight[y] = e.weight;
                heap.push({nd, y});
            }
        }
    }

    // Someone standing ON a blocked node still needs a way out: step to the
    // best unblocked neighbour.
    for (int b : opt.blockedNodes) {
        if (b < 0 || b >= static_cast<int>(n)) continue;
        for (int edgeId : out_[b]) {
            const GEdge& e = edges_[edgeId];
            if (!usable(e, opt)) continue;
            int x = otherEnd(e, b);
            if (opt.blockedNodes.count(x) || t.dist[x] == RoutingTable::INF) continue;
            double cand = e.weight + t.dist[x];
            if (cand < t.dist[b]) {
                t.dist[b] = cand;
                t.next[b] = x;
                t.target[b] = t.target[x];
                t.nextKind[b] = e.kind;
                t.nextWeight[b] = e.weight;
            }
        }
    }
    return t;
}

Route Graph::followTable(const RoutingTable& t, int from) const {
    Route route;
    if (from < 0 || from >= static_cast<int>(nodes_.size()) || t.dist[from] == RoutingTable::INF) return route;
    route.found = true;
    route.distance = t.dist[from];
    route.target = t.target[from];
    route.path.push_back(from);
    int cur = from;
    for (std::size_t guard = 0; t.next[cur] != -1; ++guard) {
        if (guard > nodes_.size()) return Route{};  // defensive: a cycle means corrupt data
        route.segments.push_back({cur, t.next[cur], t.nextWeight[cur], t.nextKind[cur]});
        cur = t.next[cur];
        route.path.push_back(cur);
    }
    return route;
}

std::vector<std::vector<int>> Graph::connectedComponents() const {
    const std::size_t n = nodes_.size();
    std::vector<bool> seen(n, false);
    std::vector<std::vector<int>> components;
    std::vector<int> queue;  // vector + head index = a simple FIFO queue
    for (std::size_t s = 0; s < n; ++s) {
        if (seen[s]) continue;
        components.emplace_back();
        queue.assign(1, static_cast<int>(s));
        seen[s] = true;
        for (std::size_t head = 0; head < queue.size(); ++head) {
            int u = queue[head];
            components.back().push_back(u);
            for (int edgeId : out_[u]) {
                int v = otherEnd(edges_[edgeId], u);
                if (!seen[v]) { seen[v] = true; queue.push_back(v); }
            }
            for (int edgeId : in_[u]) {  // ignore direction for connectivity
                int v = otherEnd(edges_[edgeId], u);
                if (!seen[v]) { seen[v] = true; queue.push_back(v); }
            }
        }
    }
    return components;
}

}  // namespace cx
