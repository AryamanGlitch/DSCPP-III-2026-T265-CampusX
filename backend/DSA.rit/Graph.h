#pragma once
// Campus navigation graph: one weighted graph for indoors and outdoors.
//   Nodes: rooms, corridors, stairs, lifts, exits, outdoor junctions,
//          assembly points, medical centre, refuge areas.
//   Edges: walkable links with a cost in metres (stairs cost extra).
// Stored as adjacency lists (edge indices per node), so memory is O(V + E).
#include <limits>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace cx {

enum class EdgeKind { Walk, Stairs, Elevator, Ramp };

const char* edgeKindName(EdgeKind k);
EdgeKind parseEdgeKind(const std::string& s);

struct GNode {
    std::string id, name, type;
    std::string building;  // empty = outdoors
    int floor = 0;
    double x = 0, y = 0;      // indoor: metres on the floor plan
    double lat = 0, lng = 0;  // outdoor
    bool indoor() const { return !building.empty(); }
};

struct GEdge {
    int from, to;
    double weight;
    EdgeKind kind;
    bool oneWay;
};

struct RouteOptions {
    bool avoidElevators = true;  // lifts are unsafe in a fire or earthquake
    bool avoidStairs = false;    // wheelchair users
    std::unordered_set<int> blockedNodes;
    std::unordered_set<long long> blockedEdges;  // Graph::edgeKey(a, b)
};

struct Segment {
    int from, to;
    double weight;
    EdgeKind kind;
};

struct Route {
    bool found = false;
    int target = -1;
    double distance = 0;
    std::vector<int> path;
    std::vector<Segment> segments;
};

// Result of one multi-source Dijkstra: for EVERY node, the distance to its
// nearest safe point and the next node to walk to. Following `next` from
// any room traces the shortest evacuation route.
struct RoutingTable {
    static constexpr double INF = std::numeric_limits<double>::infinity();
    std::vector<double> dist;
    std::vector<int> next;  // -1 at a target or when unreachable
    std::vector<int> target;
    std::vector<EdgeKind> nextKind;
    std::vector<double> nextWeight;
};

class Graph {
public:
    int addNode(GNode node);  // throws on duplicate id
    void addEdge(const std::string& from, const std::string& to, double weight, EdgeKind kind, bool oneWay = false);

    int indexOf(const std::string& id) const;  // -1 if unknown
    const GNode& node(int i) const { return nodes_.at(i); }
    std::size_t size() const { return nodes_.size(); }
    const std::vector<GEdge>& edges() const { return edges_; }
    static long long edgeKey(int a, int b);

    // Dijkstra from `from` to one specific node (everyday directions).
    Route shortestPath(int from, int to, const RouteOptions& opt) const;

    // Dijkstra that stops at the first node whose type is in `types`:
    // nearest by walking distance, not straight-line distance.
    Route nearestOfType(int from, const std::unordered_set<std::string>& types, const RouteOptions& opt) const;

    // Multi-source Dijkstra on the REVERSED graph, starting from all targets
    // at once. One O((V+E) log V) run answers "nearest exit" for every node.
    RoutingTable buildRoutingTable(const std::unordered_set<std::string>& types, const RouteOptions& opt) const;
    Route followTable(const RoutingTable& table, int from) const;

    // BFS over the undirected structure: groups of nodes that are connected.
    // More than one component usually means a missing edge in the map data.
    std::vector<std::vector<int>> connectedComponents() const;

private:
    bool usable(const GEdge& e, const RouteOptions& opt) const;
    static int otherEnd(const GEdge& e, int from) { return e.from == from ? e.to : e.from; }
    template <typename IsGoal>
    Route dijkstra(int from, IsGoal isGoal, const RouteOptions& opt) const;

    std::vector<GNode> nodes_;
    std::vector<GEdge> edges_;
    std::unordered_map<std::string, int> index_;
    std::vector<std::vector<int>> out_;  // edge ids you can leave a node by
    std::vector<std::vector<int>> in_;   // edge ids you can arrive at a node by
};

}  // namespace cx
