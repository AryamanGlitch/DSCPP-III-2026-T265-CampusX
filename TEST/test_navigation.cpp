// Tests: Graph algorithms and emergency navigation.
#include "TestHarness.h"

static GNode makeNode(const std::string& id, const std::string& type) {
    GNode n;
    n.id = id;
    n.name = id;
    n.type = type;
    return n;
}

TEST(graph_bfs_finds_components) {
    Graph g;
    for (std::string id : {"a", "b", "c", "d", "e"}) g.addNode(makeNode(id, "room"));
    g.addEdge("a", "b", 1, EdgeKind::Walk);
    g.addEdge("b", "c", 1, EdgeKind::Walk);
    g.addEdge("d", "e", 1, EdgeKind::Walk, true);
    CHECK(g.connectedComponents().size() == 2);
}

TEST(graph_one_way_edges_respected) {
    Graph g;
    for (std::string id : {"a", "b", "exit"}) g.addNode(makeNode(id, id == "exit" ? "assembly" : "room"));
    g.addEdge("a", "exit", 1, EdgeKind::Walk, true);  // a -> exit only
    g.addEdge("b", "a", 1, EdgeKind::Walk, true);     // b -> a only
    RouteOptions opt;
    auto table = g.buildRoutingTable({"assembly"}, opt);
    CHECK(table.dist[g.indexOf("b")] == 2);
    auto reverse = g.shortestPath(g.indexOf("exit"), g.indexOf("b"), opt);
    CHECK(!reverse.found);
}

TEST(routing_table_equals_single_source_dijkstra_for_every_node) {
    Fixture f;
    // Rebuild the graph directly so we can call the low-level algorithms.
    Graph g;
    {
        auto st = f.db.prepare("SELECT id, name, type, building, floor FROM nav_nodes");
        while (st.step()) {
            GNode n{st.getText(0), st.getText(1), st.getText(2), st.isNull(3) ? "" : st.getText(3),
                    static_cast<int>(st.getInt(4))};
            g.addNode(n);
        }
        auto e = f.db.prepare("SELECT from_id, to_id, weight, kind FROM nav_edges");
        while (e.step()) g.addEdge(e.getText(0), e.getText(1), e.getDouble(2), parseEdgeKind(e.getText(3)));
    }
    std::vector<RouteOptions> variants(3);
    variants[1].blockedNodes.insert(g.indexOf("ABA-1-ST"));
    variants[2].avoidStairs = true;
    for (auto& opt : variants) {
        std::unordered_set<std::string> targets = {"assembly"};
        if (opt.avoidStairs) targets.insert("refuge");
        RoutingTable t = g.buildRoutingTable(targets, opt);
        for (std::size_t i = 0; i < g.size(); ++i) {
            Route single = g.nearestOfType(static_cast<int>(i), targets, opt);
            bool tableFound = !std::isinf(t.dist[i]);
            CHECK(single.found == tableFound);
            if (single.found && tableFound) CHECK(std::abs(single.distance - t.dist[i]) < 1e-6);
            Route followed = g.followTable(t, static_cast<int>(i));
            if (followed.found) {
                double sum = 0;
                for (auto& s : followed.segments) sum += s.weight;
                CHECK(std::abs(sum - t.dist[i]) < 1e-6);  // following next pointers adds up
            }
        }
    }
}

TEST(nav_fire_route_uses_stairs_not_lift) {
    Fixture f;
    auto r = f.nav.route("ABA-1-R102", "", "fire", false).dump();
    CHECK(contains(r, "\"found\":true"));
    CHECK(contains(r, "\"type\":\"assembly\""));
    CHECK(!contains(r, "\"kind\":\"elevator\""));
    CHECK(contains(r, "Take the stairs down to Ground floor"));
    CHECK(contains(r, "Leave the building through"));
}

TEST(nav_hazard_reroutes_and_resolves) {
    Fixture f;
    User admin = f.login("admin@campusx.local");
    CHECK(contains(f.nav.route("ABA-1-R102", "", "fire", false).dump(), "East Stairwell"));

    auto hz = f.nav.addHazard(admin, "ABA-1-ST", "", "", "Smoke in stairwell").dump();
    auto rerouted = f.nav.route("ABA-1-R102", "", "fire", false).dump();
    CHECK(contains(rerouted, "West Stairwell"));
    CHECK(!contains(rerouted, "\"ABA-1-ST\""));

    long long id = std::stoll(hz.substr(hz.find("\"id\":") + 5));
    f.nav.resolveHazard(id);
    CHECK(contains(f.nav.route("ABA-1-R102", "", "fire", false).dump(), "East Stairwell"));
    CHECK_THROWS_STATUS(f.nav.resolveHazard(id), 404);
    CHECK_THROWS_STATUS(f.nav.addHazard(admin, "NOPE", "", "", "Bad id"), 400);
    CHECK_THROWS_STATUS(f.nav.addHazard(admin, "", "ABA-1-C1", "OUT-GATE", "Not adjacent"), 400);
}

TEST(nav_accessible_modes) {
    Fixture f;
    auto fire = f.nav.route("ABA-1-R102", "", "fire", true).dump();
    CHECK(contains(fire, "\"type\":\"refuge\""));  // no stairs, no lift -> refuge area
    auto medical = f.nav.route("ABA-1-R102", "", "medical", true).dump();
    CHECK(contains(medical, "\"type\":\"medical\""));
    CHECK(contains(medical, "Take the lift down"));  // lifts are fine in a medical emergency
}

TEST(nav_trapped_and_bad_input) {
    Fixture f;
    User admin = f.login("admin@campusx.local");
    f.nav.addHazard(admin, "ABA-1-C3", "", "", "Fire");
    f.nav.addHazard(admin, "ABA-1-REFUGE", "", "", "Fire");
    auto r = f.nav.route("ABA-1-R102", "", "fire", false).dump();
    CHECK(contains(r, "\"found\":false"));
    CHECK(contains(r, "Stay where you are"));
    CHECK_THROWS_STATUS(f.nav.route("NOPE", "", "fire", false), 400);
    CHECK_THROWS_STATUS(f.nav.route("OUT-GATE", "", "flood", false), 400);
}

TEST(nav_navigation_mode_and_gps) {
    Fixture f;
    auto r = f.nav.route("OUT-GATE", "ABA-1-R101", "fire", false).dump();
    CHECK(contains(r, "\"id\":\"ABA-1-R101\""));
    CHECK(contains(r, "Enter the building at Main Entrance"));
    auto near = f.nav.nearest(30.2685, 77.9945).dump();
    CHECK(contains(near, "\"id\":\"OUT-GATE\""));
}

TEST(nav_seed_graph_is_healthy) {
    Fixture f;
    auto h = f.nav.health().dump();
    CHECK(contains(h, "\"connectedComponents\":1"));
    CHECK(contains(h, "\"evacuate\":[]"));            // every node can reach an assembly point
    CHECK(contains(h, "\"evacuate_accessible\":[]"));  // or a refuge area
    auto g = f.nav.graphJson();
    CHECK(contains(g, "\"tables\":{"));
    CHECK(contains(g, "\"National Emergency (India)\""));
}
