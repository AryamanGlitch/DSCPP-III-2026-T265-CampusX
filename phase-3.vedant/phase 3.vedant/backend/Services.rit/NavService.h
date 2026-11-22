#pragma once
#include <map>
#include <shared_mutex>
#include <string>
#include <unordered_set>
#include <vector>

#include "../database/Database.h"
#include "../DSA.rit/Graph.h"
#include "../util/Json.h"
#include "../services/AuthService.h"

namespace cx {

struct HazardInfo {
    long long id = 0;
    std::string nodeId, edgeFrom, edgeTo, description;
    long long createdAt = 0;
};

class NavService {
public:
    explicit NavService(Database& db);

    // Rebuild graph + routing tables from SQLite. Called at startup and after
    // every hazard change, so routes always reflect current conditions.
    void reload();

    // Everything a phone needs to keep navigating offline, pre-serialised once
    // per reload (clients poll it, so it shouldn't be rebuilt per request).
    std::string graphJson() const;

    // to empty  -> emergency mode: nearest safe point via the routing table.
    // to given  -> everyday directions to that node (lifts allowed).
    Json route(const std::string& from, const std::string& to, const std::string& emergencyType, bool accessible) const;
    Json nearest(double lat, double lng) const;

    Json addHazard(const User& by, const std::string& nodeId, const std::string& edgeFrom, const std::string& edgeTo,
                   const std::string& description);
    void resolveHazard(long long id);

    Json health() const;  // BFS connectivity + nodes that can't reach safety

    // Exposed for tests.
    static std::string modeKey(const std::string& emergencyType, bool accessible);

private:
    struct Mode {
        std::string key;
        std::unordered_set<std::string> targets;
        RouteOptions options;
    };
    static std::vector<Mode> buildModes(const RouteOptions& base);
    Json routeJson(const Route& r) const;
    Json directions(const Route& r) const;

    Database& db_;
    mutable std::shared_mutex mu_;
    Graph graph_;
    RouteOptions base_;  // blocked nodes/edges from active hazards
    std::vector<HazardInfo> hazards_;
    std::map<std::string, RoutingTable> tables_;
    std::string graphJson_;
    long long version_ = 0;
};

}  // namespace cx
