// CampusX test suite. No framework needed: build target `campusx_tests`, run it,
// exit code 0 = all passed. Uses an in-memory SQLite database.
#include <algorithm>
#include <cmath>
#include <functional>
#include <iostream>
#include <random>
#include <set>
#include <string>
#include <vector>

#include "../backend/database/Database.h"
#include "../backend/DSA.rit/EditDistance.h"
#include "../backend/DSA.rit/Graph.h"
#include "../backend/dsa/LRUCache.h"
#include "../backend/DSA.rit/Matcher.h"
#include "../backend/dsa/SkillGap.h"
#include "../backend/dsa/MinHeap.h"
#include "../backend/dsa/RateLimiter.h"
#include "../backend/dsa/TopK.h"
#include "../backend/dsa.ved/Trie.h"
#include "../backend/seed/Seed.h"
#include "../backend/services/AuthService.h"
#include "../backend/services.ved/FeedService.h"
#include "../backend/services/GeminiClient.h"
#include "../backend/services/LearningService.h"
#include "../backend/Services.rit/NavService.h"
#include "../backend/services.ved/PartnerService.h"
#include "../backend/services/SearchService.h"
#include "../backend/util/Crypto.h"
#include "../backend/util/HttpError.h"
#include "../backend/util/JsonParse.h"
#include "../backend/util/Text.h"

using namespace cx;

static int g_checks = 0, g_failures = 0;
static std::vector<std::pair<const char*, void (*)()>>& registry() {
    static std::vector<std::pair<const char*, void (*)()>> r;
    return r;
}
struct Register {
    Register(const char* n, void (*f)()) { registry().push_back({n, f}); }
};
#define TEST(name)                               \
    static void name();                          \
    static Register reg_##name(#name, name);     \
    static void name()
#define CHECK(cond)                                                                       \
    do {                                                                                  \
        ++g_checks;                                                                       \
        if (!(cond)) {                                                                    \
            ++g_failures;                                                                 \
            std::cerr << "  FAILED " << __FILE__ << ":" << __LINE__ << "  " #cond "\n"; \
        }                                                                                 \
    } while (0)
#define CHECK_THROWS_STATUS(expr, code)                                       \
    do {                                                                      \
        int got_ = 0;                                                         \
        try { expr; } catch (const HttpError& e) { got_ = e.status; }         \
        CHECK(got_ == (code));                                                \
    } while (0)

static GNode makeNode(const std::string& id, const std::string& type) {
    GNode n;
    n.id = id;
    n.name = id;
    n.type = type;
    return n;
}

static bool contains(const std::string& hay, const std::string& needle) { return hay.find(needle) != std::string::npos; }

// A fully seeded in-memory campus shared by the service tests.
struct Fixture {
    Database db{":memory:"};
    AuthService auth{db, 1000};  // low PBKDF2 cost keeps tests fast
    SearchService search{db};
    FeedService feed{db};
    NavService nav{db};
    PartnerService partners{db};
    Fixture() {
        createSchema(db);
        seedDatabase(db, auth, "TestPass@123");
        search.rebuild();
        nav.reload();
    }
    User login(const std::string& email) { return auth.login(email, "TestPass@123").user; }
    User aryaman() { return auth.login(DEMO_STUDENT_EMAIL, DEMO_STUDENT_PASSWORD).user; }
    long long projectId(const std::string& title) {
        auto st = db.prepare("SELECT id FROM projects WHERE title = ?");
        st.bind(1, title);
        return st.step() ? st.getInt(0) : 0;
    }
    long long userId(const std::string& email) {
        auto st = db.prepare("SELECT id FROM users WHERE email = ?");
        st.bind(1, email);
        return st.step() ? st.getInt(0) : 0;
    }
    long long pendingInvitation(long long project, long long student) {
        auto st = db.prepare("SELECT id FROM invitations WHERE project_id = ? AND student_id = ? AND status = 'pending'");
        st.bind(1, project).bind(2, student);
        return st.step() ? st.getInt(0) : 0;
    }
};

// The part of a JSON dump between two keys (good enough to scope a contains() check).
static std::string between(const std::string& json, const std::string& from, const std::string& to) {
    auto a = json.find(from);
    if (a == std::string::npos) return "";
    auto b = json.find(to, a);
    return json.substr(a, b == std::string::npos ? std::string::npos : b - a);
}

/* ------------------------------ DSA ------------------------------ */

TEST(min_heap_sorts_like_std_sort) {
    std::mt19937 rng(42);
    std::vector<int> values(1000);
    for (int& v : values) v = static_cast<int>(rng() % 10000);
    MinHeap<int> heap;
    for (int v : values) heap.push(v);
    std::vector<int> out;
    while (!heap.empty()) out.push_back(heap.pop());
    std::sort(values.begin(), values.end());
    CHECK(out == values);
}

TEST(topk_matches_full_sort) {
    std::mt19937 rng(7);
    std::vector<std::pair<double, int>> all;
    TopK<int> top(10);
    for (int i = 0; i < 500; ++i) {
        double s = static_cast<double>(rng() % 1000);
        all.push_back({s, i});
        top.offer(s, i, i);
    }
    std::sort(all.begin(), all.end(), [](auto& a, auto& b) { return a.first != b.first ? a.first > b.first : a.second < b.second; });
    auto got = top.takeSorted();
    CHECK(got.size() == 10);
    for (std::size_t i = 0; i < got.size(); ++i) CHECK(got[i].first == all[i].second);
}

TEST(edit_distance_known_pairs) {
    CHECK(boundedEditDistance("library", "libary", 2) == 1);
    CHECK(boundedEditDistance("kitten", "sitting", 5) == 3);
    CHECK(boundedEditDistance("abc", "abc", 1) == 0);
    CHECK(boundedEditDistance("short", "muchlongerword", 2) == 3);  // capped at maxDist + 1
}

TEST(trie_prefix_and_fuzzy_match_brute_force) {
    std::vector<std::string> words = {"library", "librarian", "lab", "labs", "lecture", "seminar", "semester",
                                      "canteen", "cafeteria", "computer", "compute", "hostel", "hospital"};
    Trie trie;
    for (std::size_t i = 0; i < words.size(); ++i) trie.insert(words[i], static_cast<int>(i), 1.0f);
    CHECK(trie.wordCount() == words.size());

    std::vector<Trie::Match> m;
    trie.collectPrefix("lib", 100, m);
    std::set<std::string> got;
    for (auto& x : m) got.insert(x.word);
    CHECK((got == std::set<std::string>{"library", "librarian"}));

    // Trie-pruned fuzzy search must return exactly what a brute-force scan returns.
    for (std::string q : {"libary", "hostal", "semnar", "lap", "compter", "xyzzy"}) {
        for (int maxDist = 1; maxDist <= 2; ++maxDist) {
            m.clear();
            trie.fuzzy(q, maxDist, m);
            std::set<std::string> fuzzy, brute;
            for (auto& x : m) fuzzy.insert(x.word);
            for (auto& w : words) if (boundedEditDistance(q, w, maxDist) <= maxDist) brute.insert(w);
            CHECK(fuzzy == brute);
        }
    }
}

TEST(lru_evicts_least_recently_used) {
    LRUCache<std::string, int> c(2);
    c.put("a", 1);
    c.put("b", 2);
    CHECK(c.get("a").value_or(-1) == 1);  // a is now most recent
    c.put("c", 3);                        // evicts b
    CHECK(!c.get("b").has_value());
    CHECK(c.get("a").value_or(-1) == 1);
    CHECK(c.get("c").value_or(-1) == 3);
    c.put("a", 10);
    CHECK(c.get("a").value_or(-1) == 10);
    CHECK(c.size() == 2);
}

TEST(rate_limiter_sliding_window) {
    RateLimiter rl(3, 1000);
    CHECK(rl.allow("ip", 0));
    CHECK(rl.allow("ip", 100));
    CHECK(rl.allow("ip", 200));
    CHECK(!rl.allow("ip", 300));    // 4th within a second
    CHECK(rl.allow("other", 300));  // keys are independent
    CHECK(rl.allow("ip", 1001));    // the request at t=0 has left the window
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

/* ----------------------------- Crypto ---------------------------- */

TEST(sha256_and_pbkdf2_match_reference_vectors) {
    CHECK(crypto::sha256Hex("abc") == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
    CHECK(crypto::sha256Hex("") == "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
    CHECK(crypto::sha256Hex(std::string(1000, 'a')) == "41edece42d63e8d9bf515a9ba6932e1c20cbc9f5a5d134645adb5db1b9737ea3");
    // Generated with Python: hashlib.pbkdf2_hmac('sha256', ...)
    CHECK(crypto::toHex(crypto::pbkdf2Sha256("passwd", {'s', 'a', 'l', 't'}, 1, 64)) ==
          "55ac046e56e3089fec1691c22544b605f94185216dde0465e68b9d57c20dacbc"
          "49ca9cccf179b645991664b39d77ef317c71b845b1e30bd509112041d3a19783");
    CHECK(crypto::toHex(crypto::pbkdf2Sha256("ChangeMe@123", crypto::fromHex("00112233445566778899aabbccddeeff"), 1000, 32)) ==
          "048aa15d07352d553512dad8e51b466071eaa43092f917b57719569b5f112f56");
    std::string stored = crypto::hashPassword("secret123", 500);
    CHECK(crypto::verifyPassword("secret123", stored));
    CHECK(!crypto::verifyPassword("secret124", stored));
    CHECK(!crypto::verifyPassword("secret123", "garbage"));
}

/* ------------------------------ Auth ----------------------------- */

TEST(auth_register_login_logout) {
    Fixture f;
    auto r = f.auth.registerUser("Test Student", "  New@Campus.Local ", "password123");
    CHECK(r.user.role == "student");
    CHECK(r.user.email == "new@campus.local");
    CHECK(r.token.size() == 64);
    CHECK(f.auth.userForToken(r.token).has_value());

    CHECK_THROWS_STATUS(f.auth.registerUser("Dup", "new@campus.local", "password123"), 409);
    CHECK_THROWS_STATUS(f.auth.registerUser("X", "x@campus.local", "password123"), 400);       // name too short
    CHECK_THROWS_STATUS(f.auth.registerUser("Valid Name", "not-an-email", "password123"), 400);
    CHECK_THROWS_STATUS(f.auth.registerUser("Valid Name", "a@b.co", "short"), 400);
    CHECK_THROWS_STATUS(f.auth.login("new@campus.local", "wrongpass"), 401);
    CHECK_THROWS_STATUS(f.auth.login("nobody@campus.local", "password123"), 401);

    auto again = f.auth.login("NEW@campus.local", "password123");
    f.auth.logout(again.token);
    CHECK(!f.auth.userForToken(again.token).has_value());
    CHECK(f.auth.userForToken(r.token).has_value());  // other session unaffected
    CHECK(!f.auth.userForToken("not-a-token").has_value());
}

TEST(demo_student_only_logs_in_with_its_own_credentials) {
    Fixture f;
    User a = f.aryaman();
    CHECK(a.name == "Aryaman");
    CHECK(a.email == "aryaman@gmail.com");
    CHECK_THROWS_STATUS(f.auth.login("student@campusx.local", "TestPass@123"), 401);  // old demo login is gone
    CHECK_THROWS_STATUS(f.auth.login("aryaman@gmail.com", "TestPass@123"), 401);      // shared demo password doesn't work
    CHECK_THROWS_STATUS(f.auth.login("aryaman@gmail.com", "ChangeMe@123"), 401);
    CHECK(!upgradeLegacyDemoStudent(f.db, f.auth));  // nothing to move in a new database

    // An older sample database: student@campusx.local with the shared password and a live session.
    Database db(":memory:");
    createSchema(db);
    AuthService auth(db, 1000);
    long long id = auth.createUser("Riya Student", "student@campusx.local", "TestPass@123", "student");
    std::string oldToken = auth.login("student@campusx.local", "TestPass@123").token;
    CHECK(upgradeLegacyDemoStudent(db, auth));
    CHECK_THROWS_STATUS(auth.login("student@campusx.local", "TestPass@123"), 401);
    CHECK_THROWS_STATUS(auth.login("aryaman@gmail.com", "TestPass@123"), 401);
    CHECK(!auth.userForToken(oldToken).has_value());  // signed out everywhere
    AuthResult r = auth.login("aryaman@gmail.com", "qwerty123");
    CHECK(r.user.id == id);  // same account, so its projects and invitations stay
    CHECK(r.user.name == "Aryaman");
    CHECK(!upgradeLegacyDemoStudent(db, auth));  // runs once
}

TEST(auth_email_domain_restriction) {
    Database db(":memory:");
    createSchema(db);
    AuthService auth(db, 100, "geu.ac.in");
    CHECK_THROWS_STATUS(auth.registerUser("Some One", "someone@gmail.com", "password123"), 400);
    CHECK(auth.registerUser("Some One", "someone@geu.ac.in", "password123").user.id > 0);
}

/* ----------------------------- Search ---------------------------- */

TEST(search_handles_typos_prefixes_and_filters) {
    Fixture f;
    long long now = nowMs();
    auto typo = f.search.search("libary", "all", 10, now).dump();
    CHECK(contains(typo, "\"title\":\"Central Library\""));

    auto prefix = f.search.search("semi", "all", 10, now).dump();
    CHECK(contains(prefix, "Seminar Hall"));

    auto person = f.search.search("rao", "faculty", 10, now).dump();
    CHECK(contains(person, "Ananya Rao"));
    CHECK(!contains(person, "\"kind\":\"place\""));

    auto past = f.search.search("orientation", "all", 10, now).dump();
    CHECK(!contains(past, "Library orientation"));  // event already ended

    auto multi = f.search.search("computer lab", "all", 10, now).dump();
    CHECK(contains(multi, "Computer Lab 1"));

    auto none = f.search.search("zzzzqqq", "all", 10, now).dump();
    CHECK(contains(none, "\"results\":[]"));

    auto cached = f.search.search("libary", "all", 10, now).dump();
    CHECK(contains(cached, "\"cached\":true"));

    CHECK_THROWS_STATUS(f.search.search("lab", "bogus", 10, now), 400);
    CHECK_THROWS_STATUS(f.search.search(std::string(101, 'a'), "all", 10, now), 400);
}

TEST(search_ranks_title_matches_first) {
    Fixture f;
    auto out = f.search.search("library", "all", 10, nowMs()).dump();
    // The library itself must come before the librarian whose cabin is in it.
    CHECK(out.find("Central Library") < out.find("Priya Bisht"));
}

/* ------------------------------ Feed ----------------------------- */

TEST(feed_like_toggle_is_idempotent) {
    Fixture f;
    User student = f.aryaman();
    User prof = f.login("faculty@campusx.local");
    auto post = f.feed.create(student, "Testing likes", "general").dump();
    long long id = std::stoll(post.substr(post.find("\"id\":") + 5));

    CHECK(contains(f.feed.toggleLike(student, id).dump(), "\"liked\":true,\"likeCount\":1"));
    CHECK(contains(f.feed.toggleLike(prof, id).dump(), "\"likeCount\":2"));
    CHECK(contains(f.feed.toggleLike(student, id).dump(), "\"liked\":false,\"likeCount\":1"));
    CHECK_THROWS_STATUS(f.feed.toggleLike(student, 999999), 404);
}

TEST(feed_pagination_has_no_duplicates_or_gaps) {
    Fixture f;
    User u = f.aryaman();
    for (int i = 0; i < 23; ++i) f.feed.create(u, "Post number " + std::to_string(i), "general");

    std::set<long long> seen;
    std::string cursor;
    int pages = 0, total = 0;
    do {
        std::string page = f.feed.list(u, cursor, 5, "").dump();
        ++pages;
        for (std::size_t pos = page.find("{\"id\":"); pos != std::string::npos; pos = page.find("{\"id\":", pos + 1)) {
            long long id = std::stoll(page.substr(pos + 6));
            if (page.compare(pos - 9, 9, "\"author\":") == 0) continue;  // skip nested author ids
            CHECK(seen.insert(id).second);
            ++total;
        }
        auto c = page.find("\"nextCursor\":\"");
        cursor = c == std::string::npos ? "" : page.substr(c + 14, page.find('"', c + 14) - (c + 14));
    } while (!cursor.empty() && pages < 20);
    CHECK(total == 27);  // 23 new + 4 seeded
    CHECK_THROWS_STATUS(f.feed.list(u, "garbage", 5, ""), 400);
}

TEST(feed_roles_and_ownership) {
    Fixture f;
    User student = f.aryaman();
    User prof = f.login("faculty@campusx.local");
    User admin = f.login("admin@campusx.local");
    CHECK_THROWS_STATUS(f.feed.create(student, "Official?", "announcement"), 403);
    CHECK(contains(f.feed.create(prof, "Exam schedule out", "announcement").dump(), "\"isOfficial\":true"));
    CHECK_THROWS_STATUS(f.feed.create(student, "   ", "general"), 400);
    CHECK_THROWS_STATUS(f.feed.create(student, std::string(2001, 'x'), "general"), 400);
    CHECK_THROWS_STATUS(f.feed.create(student, "hi", "memes"), 400);

    auto post = f.feed.create(student, "Mine", "general").dump();
    long long id = std::stoll(post.substr(post.find("\"id\":") + 5));
    f.feed.addComment(prof, id, "Nice");
    CHECK(contains(f.feed.listComments(id).dump(), "Nice"));
    CHECK_THROWS_STATUS(f.feed.remove(prof, id), 403);
    f.feed.remove(admin, id);
    CHECK(contains(f.feed.listComments(id).dump(), "\"comments\":[]"));  // cascade deleted
    CHECK_THROWS_STATUS(f.feed.addComment(prof, id, "Too late"), 404);
}

TEST(feed_trending_counts_hashtags) {
    Fixture f;
    auto out = f.feed.trending(5, nowMs()).dump();
    CHECK(contains(out, "{\"tag\":\"safety\",\"count\":2}"));  // seeded twice
    CHECK(contains(out, "\"posts\":[{"));
}

/* --------------------------- Navigation -------------------------- */

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

/* ------------------------- Partner matching ---------------------- */

static match::Profile makeProfile(long long id, std::initializer_list<std::pair<int, int>> skills, int exp = 2,
                                  unsigned interests = 0, match::SlotMask slots = 0, int active = 0) {
    match::Profile p;
    p.userId = id;
    for (auto [skill, level] : skills) p.skills[skill] = level;
    p.experience = exp;
    p.interests = interests;
    p.availability = slots;
    p.activeProjects = active;
    return p;
}

TEST(partner_scores_match_spec_examples) {
    // Spec section 9: 0.5*90 + 0.25*80 + 0.15*70 + 0.1*90 = 84.5
    CHECK(std::fabs(match::combine(90, 80, 70, 90) - 84.5) < 1e-9);

    enum { PY = 1, ML = 2, SQL = 3, DA = 4, NUMPY = 5 };
    match::ProjectReq p;
    p.skills = {{PY, 1, 1, false}, {ML, 1, 1, false}, {SQL, 1, 1, false}, {DA, 1, 1, false}};
    match::Profile a = makeProfile(1, {{PY, 2}, {ML, 2}, {DA, 1}, {NUMPY, 3}});
    std::vector<int> matched;
    CHECK(std::fabs(match::skillScore(p, a, &matched) - 75.0) < 1e-9);  // 3 of 4, equal weights
    CHECK(matched.size() == 3);

    // Weighted: ML 40, Python 30, SQL 20, Data Analysis 10 -> missing SQL costs 20.
    p.skills = {{ML, 1, 4, false}, {PY, 1, 3, false}, {SQL, 1, 2, false}, {DA, 1, 1, false}};
    CHECK(std::fabs(match::skillScore(p, a) - 80.0) < 1e-9);

    // Lower level than wanted earns partial credit.
    match::ProjectReq hard;
    hard.skills = {{PY, 3, 1, false}};
    CHECK(std::fabs(match::skillScore(hard, makeProfile(2, {{PY, 1}})) - 100.0 / 3) < 1e-9);
    CHECK(std::fabs(match::skillScore(hard, makeProfile(3, {{PY, 3}})) - 100.0) < 1e-9);

    // Spec section 7: project Wed evening + Sat + Sun; student has Wed evening + Sat -> 2/3.
    match::ProjectReq sched;
    sched.availability = (1u << 8) | (1u << 16) | (1u << 19);
    CHECK(std::fabs(match::availabilityScore(sched, makeProfile(4, {}, 2, 0, (1u << 8) | (1u << 16))) - 200.0 / 3) < 1e-9);
    CHECK(match::availabilityScore(match::ProjectReq{}, makeProfile(5, {})) == 100.0);  // no preferred times

    CHECK(match::experienceScore(2, 3) == 100.0);
    CHECK(match::experienceScore(2, 1) == 50.0);
    CHECK(match::experienceScore(3, 1) == 0.0);

    std::mt19937 rng(11);
    for (int i = 0; i < 1000; ++i) {
        unsigned v = rng();
        int brute = 0;
        for (int b = 0; b < 32; ++b) brute += (v >> b) & 1u;
        CHECK(match::countBits(v) == brute);
    }
}

TEST(partner_rank_matches_brute_force) {
    std::mt19937 rng(2024);
    for (int trial = 0; trial < 20; ++trial) {
        match::ProjectReq p;
        int nSkills = 1 + static_cast<int>(rng() % 5);
        for (int i = 0; i < nSkills; ++i) {
            p.skills.push_back({static_cast<int>(i * 3 + rng() % 3), 1 + static_cast<int>(rng() % 3),
                                1 + static_cast<int>(rng() % 5), rng() % 6 == 0});
        }
        p.experience = 1 + static_cast<int>(rng() % 3);
        p.domainBit = 1u << (rng() % 4);
        p.availability = rng() & ((1u << match::SLOTS) - 1);
        p.scheduleRequired = rng() % 2 == 0;

        std::vector<match::Profile> profiles;
        for (int i = 0; i < 150; ++i) {
            match::Profile s;
            s.userId = 1000 + i;
            int k = static_cast<int>(rng() % 5);
            for (int j = 0; j < k; ++j) s.skills[static_cast<int>(rng() % 15)] = 1 + static_cast<int>(rng() % 3);
            s.experience = 1 + static_cast<int>(rng() % 3);
            s.interests = rng() & 15u;
            s.availability = rng() & ((1u << match::SLOTS) - 1);
            s.activeProjects = static_cast<int>(rng() % 5);
            profiles.push_back(std::move(s));
        }
        std::unordered_set<long long> exclude = {1000, 1001, 1002};
        match::RankResult r = match::rank(p, profiles, exclude, 3);

        // Brute force: score everyone, keep the eligible ones that share a skill, sort.
        std::vector<match::Candidate> expected;
        int notExcluded = 0, overlapping = 0;
        for (std::size_t i = 0; i < profiles.size(); ++i) {
            if (exclude.count(profiles[i].userId)) continue;
            ++notExcluded;
            bool shares = false;
            for (const auto& req : p.skills) shares = shares || profiles[i].skills.count(req.skillId);
            if (!shares) continue;
            ++overlapping;
            if (match::eligibility(p, profiles[i], 3) != match::Exclusion::None) continue;
            expected.push_back({i, match::score(p, profiles[i])});
        }
        std::sort(expected.begin(), expected.end(),
                  [&](const match::Candidate& a, const match::Candidate& b) { return match::ranksHigher(a, b, profiles); });
        CHECK(r.ranked.size() == expected.size());
        bool sameOrder = r.ranked.size() == expected.size();
        for (std::size_t i = 0; sameOrder && i < expected.size(); ++i) sameOrder = r.ranked[i].profile == expected[i].profile;
        CHECK(sameOrder);
        CHECK(r.considered == overlapping);
        CHECK(r.noSkillOverlap == notExcluded - overlapping);
        CHECK(r.missingMandatory + r.noSharedTime + r.tooManyProjects + static_cast<int>(r.ranked.size()) == r.considered);
        for (const auto& c : r.ranked) {
            CHECK(c.scores.total >= 0 && c.scores.total <= 100);
            CHECK(profiles[c.profile].activeProjects < 3);
        }
    }
}

TEST(partner_greedy_team_covers_missing_skills) {
    // Spec section 11: the top 3 by score all know Python + ML; the team still needs SQL and frontend.
    enum { PY = 1, ML = 2, SQL = 3, FE = 4 };
    match::ProjectReq p;
    p.skills = {{PY, 2, 3, false}, {ML, 2, 3, false}, {SQL, 2, 2, false}, {FE, 2, 2, false}};
    std::vector<match::Profile> profiles = {
        makeProfile(1, {{PY, 3}, {ML, 3}}), makeProfile(2, {{PY, 3}, {ML, 2}}), makeProfile(3, {{PY, 2}, {ML, 2}}),
        makeProfile(4, {{SQL, 2}}), makeProfile(5, {{FE, 2}}),
    };
    match::RankResult r = match::rank(p, profiles, {}, 0);
    CHECK(r.ranked.size() == 5);
    CHECK(profiles[r.ranked[0].profile].userId == 1);
    CHECK(profiles[r.ranked[3].profile].skills.size() == 1);  // SQL and frontend people rank last

    std::vector<bool> covered(p.skills.size(), false);
    covered[1] = true;  // the creator already knows ML
    auto team = match::formTeam(p, profiles, r.ranked, covered, 3);
    std::set<long long> picked;
    for (const auto& t : team) picked.insert(profiles[r.ranked[t.candidate].profile].userId);
    CHECK(team.size() == 3);
    CHECK(picked.size() == 3);  // no one picked twice
    CHECK(picked.count(4) && picked.count(5));  // SQL + frontend make the team
    CHECK(picked.count(1));                     // and the strongest Python/ML student
    CHECK(std::all_of(covered.begin(), covered.end(), [](bool b) { return b; }));

    // Nothing left to cover: falls back to plain rank order. More slots than people: everyone, once.
    std::vector<bool> all(p.skills.size(), true);
    auto byRank = match::formTeam(p, profiles, r.ranked, all, 2);
    CHECK(byRank.size() == 2 && byRank[0].candidate == 0 && byRank[1].candidate == 1);
    std::vector<bool> none;
    CHECK(match::formTeam(p, profiles, r.ranked, none, 10).size() == 5);
    CHECK(match::formTeam(p, profiles, {}, none, 3).empty());
}

TEST(partner_profile_validation) {
    Fixture f;
    User aryaman = f.aryaman();
    User prof = f.login("faculty@campusx.local");
    ProfileInput ok{"CSE", "5", "2", "ai_ml,web", "0,8,20", "Python:3,  Machine   Learning :2", "hi"};
    CHECK_THROWS_STATUS(f.partners.saveProfile(prof, ok), 403);  // profiles are for students

    auto bad = [&](ProfileInput in) { return [&f, &aryaman, in]() { f.partners.saveProfile(aryaman, in); }; };
    ProfileInput x = ok; x.semester = "11";             CHECK_THROWS_STATUS(bad(x)(), 400);
    x = ok; x.experience = "abc";                       CHECK_THROWS_STATUS(bad(x)(), 400);
    x = ok; x.interests = "astrology";                  CHECK_THROWS_STATUS(bad(x)(), 400);
    x = ok; x.availability = "21";                      CHECK_THROWS_STATUS(bad(x)(), 400);
    x = ok; x.skills = "";                              CHECK_THROWS_STATUS(bad(x)(), 400);
    x = ok; x.skills = "Python:3,python:1";             CHECK_THROWS_STATUS(bad(x)(), 400);  // same skill twice
    x = ok; x.skills = "Python";                        CHECK_THROWS_STATUS(bad(x)(), 400);
    x = ok; x.skills = "Python:4";                      CHECK_THROWS_STATUS(bad(x)(), 400);
    x = ok; x.department = "C";                         CHECK_THROWS_STATUS(bad(x)(), 400);

    auto saved = f.partners.saveProfile(aryaman, ok).dump();
    CHECK(contains(saved, "\"name\":\"Machine Learning\""));  // whitespace tidied, matches the seeded skill
    CHECK(contains(saved, "\"availability\":[0,8,20]"));
    CHECK(contains(saved, "\"interests\":[\"ai_ml\",\"web\"]"));
    auto st = f.db.prepare("SELECT COUNT(*) FROM skills WHERE name_key IN ('python', 'machine learning')");
    st.step();
    CHECK(st.getInt(0) == 2);  // "python" was not created a second time

    f.partners.deleteProfile(aryaman);
    CHECK(contains(f.partners.myProfile(aryaman).dump(), "\"profile\":null"));
    CHECK_THROWS_STATUS(f.partners.studentProfile(aryaman.id), 404);
}

TEST(partner_seed_example_builds_balanced_team) {
    Fixture f;
    User aryaman = f.aryaman();
    long long crop = f.projectId("AI-Based Crop Recommendation System");
    CHECK(crop > 0);
    auto m = f.partners.matches(aryaman, crop, 10).dump();

    std::string team = between(m, "\"suggestedTeam\":", "\"spotsLeft\"");
    CHECK(contains(team, "Neha Joshi"));   // SQL
    CHECK(contains(team, "Rohan Das"));    // frontend
    CHECK(contains(team, "Data Analysis"));
    CHECK(contains(m, "\"spotsLeft\":3"));
    CHECK(contains(between(m, "\"coverage\":", "\"weights\""), "\"missing\":[]"));

    // The plain top 3 by score are Python/ML people: greedy is doing real work.
    std::string cands = between(m, "\"candidates\":", "\"eligible\"");
    auto first = cands.find("Aarav Sharma");
    CHECK(first != std::string::npos && first < cands.find("Neha Joshi") && first < cands.find("Rohan Das"));
    CHECK(!contains(cands, "\"name\":\"Aryaman\""));  // never recommend the creator
    CHECK(contains(cands, "Recommended because this student has"));
    CHECK(!contains(cands, "Meera Iyer") || contains(cands, "no shared time slots"));
    CHECK(contains(m, "\"warnings\":[]"));

    User neha = f.login("neha.sample@campusx.local");
    CHECK_THROWS_STATUS(f.partners.matches(neha, crop, 10), 403);  // creator only
}

TEST(partner_invitation_flow) {
    Fixture f;
    User aryaman = f.aryaman();
    User neha = f.login("neha.sample@campusx.local");
    User rohan = f.login("rohan.sample@campusx.local");
    User kabir = f.login("kabir.sample@campusx.local");
    User aarav = f.login("aarav.sample@campusx.local");
    User sneha = f.login("sneha.sample@campusx.local");
    long long crop = f.projectId("AI-Based Crop Recommendation System");

    auto inv = f.partners.invite(aryaman, crop, neha.id, "Join us?").dump();
    CHECK(contains(inv, "\"status\":\"pending\""));
    CHECK_THROWS_STATUS(f.partners.invite(aryaman, crop, neha.id, ""), 409);       // duplicate
    CHECK_THROWS_STATUS(f.partners.invite(neha, crop, rohan.id, ""), 403);      // not the creator
    CHECK_THROWS_STATUS(f.partners.invite(aryaman, crop, aryaman.id, ""), 400);       // self
    CHECK_THROWS_STATUS(f.partners.invite(aryaman, crop, f.userId("admin@campusx.local"), ""), 404);  // no profile
    CHECK_THROWS_STATUS(f.partners.requestToJoin(neha, crop, ""), 409);         // already invited

    long long nehaInv = f.pendingInvitation(crop, neha.id);
    CHECK_THROWS_STATUS(f.partners.respond(aryaman, nehaInv, "accept"), 403);      // sender can't accept
    CHECK_THROWS_STATUS(f.partners.respond(rohan, nehaInv, "accept"), 403);     // stranger can't either
    CHECK(contains(f.partners.respond(neha, nehaInv, "accept").dump(), "\"status\":\"accepted\""));
    CHECK_THROWS_STATUS(f.partners.respond(neha, nehaInv, "reject"), 409);      // already answered
    CHECK_THROWS_STATUS(f.partners.invite(aryaman, crop, neha.id, ""), 409);       // already on the team
    CHECK(!contains(between(f.partners.matches(aryaman, crop, 20).dump(), "\"candidates\":", "\"eligible\""), "Neha Joshi"));

    // A "no" is respected: no second invitation.
    f.partners.invite(aryaman, crop, aarav.id, "");
    f.partners.respond(aarav, f.pendingInvitation(crop, aarav.id), "reject");
    CHECK_THROWS_STATUS(f.partners.invite(aryaman, crop, aarav.id, ""), 409);
    CHECK(contains(f.partners.matches(aryaman, crop, 20).dump(), "\"declined\":true"));

    // Withdraw by the sender only.
    f.partners.invite(aryaman, crop, sneha.id, "");
    long long snehaInv = f.pendingInvitation(crop, sneha.id);
    CHECK_THROWS_STATUS(f.partners.respond(sneha, snehaInv, "withdraw"), 403);
    f.partners.respond(aryaman, snehaInv, "withdraw");
    f.partners.invite(aryaman, crop, sneha.id, "");  // a withdrawn invite can be sent again

    // Fill the team (size 4): Aryaman + Neha + Rohan + Kabir. Sneha's pending invite closes.
    f.partners.invite(aryaman, crop, rohan.id, "");
    f.partners.requestToJoin(kabir, crop, "I know data analysis");
    f.partners.respond(rohan, f.pendingInvitation(crop, rohan.id), "accept");
    auto inbox = f.partners.invitations(aryaman).dump();
    CHECK(contains(between(inbox, "\"received\":", "\"sent\""), "I know data analysis"));
    f.partners.respond(aryaman, f.pendingInvitation(crop, kabir.id), "accept");
    auto proj = f.partners.project(aryaman, crop).dump();
    CHECK(contains(proj, "\"status\":\"team_complete\""));
    CHECK(contains(proj, "\"spotsLeft\":0"));
    CHECK(f.pendingInvitation(crop, sneha.id) == 0);
    CHECK_THROWS_STATUS(f.partners.invite(aryaman, crop, sneha.id, ""), 409);
    CHECK_THROWS_STATUS(f.partners.matches(aryaman, crop, 10), 409);  // not recruiting any more

    // Leaving reopens the team; the creator can't leave.
    f.partners.leave(kabir, crop);
    CHECK(contains(f.partners.project(aryaman, crop).dump(), "\"status\":\"recruiting\""));
    CHECK_THROWS_STATUS(f.partners.leave(kabir, crop), 404);
    CHECK_THROWS_STATUS(f.partners.leave(aryaman, crop), 400);

    // Status changes: creator only, valid transitions only.
    CHECK_THROWS_STATUS(f.partners.setStatus(neha, crop, "in_progress"), 403);
    CHECK_THROWS_STATUS(f.partners.setStatus(aryaman, crop, "completed"), 409);
    CHECK_THROWS_STATUS(f.partners.setStatus(aryaman, crop, "party"), 400);
    f.partners.setStatus(aryaman, crop, "in_progress");
    f.partners.setStatus(aryaman, crop, "completed");
    CHECK_THROWS_STATUS(f.partners.setStatus(aryaman, crop, "cancelled"), 409);
}

TEST(partner_limits_mandatory_and_schedule) {
    Fixture f;
    User aryaman = f.aryaman();
    User rohan = f.login("rohan.sample@campusx.local");
    User arjun = f.login("arjun.sample@campusx.local");
    long long events = f.projectId("Campus Events Website");
    long long phishing = f.projectId("Phishing Email Detector");

    // Aryaman: creator of 1, accepts the seeded phishing invite (2), joins events (3) -> at the limit.
    f.partners.respond(aryaman, f.pendingInvitation(phishing, aryaman.id), "accept");
    f.partners.requestToJoin(aryaman, events, "");
    f.partners.respond(rohan, f.pendingInvitation(events, aryaman.id), "accept");
    ProjectInput extra{"Fourth project", "One project too many for one student.", "web", "3", "4", "1", "", "0", "SQL:1:3:0", ""};
    CHECK_THROWS_STATUS(f.partners.createProject(aryaman, extra), 409);

    // Arjun's project has spots left, but Aryaman isn't recommended to anyone else now.
    ProjectInput ml{"Another ML idea", "Needs Python and machine learning people.", "ai_ml", "3", "4", "1", "", "0",
                    "Python:1:3:0,Machine Learning:1:3:0", ""};
    long long another = std::stoll(between(f.partners.createProject(arjun, ml).dump(), "\"id\":", ",").substr(5));
    auto m = f.partners.matches(arjun, another, 20).dump();
    CHECK(!contains(between(m, "\"candidates\":", "\"eligible\""), "Aryaman"));
    CHECK(contains(m, "\"tooManyProjects\":1"));

    // A mandatory skill nobody has: no candidates, and a warning says why.
    ProjectInput rare{"Quantum thing", "Needs a skill no student has listed.", "research", "2", "4", "1", "", "0",
                      "Quantum Computing:1:5:1,Python:1:2:0", ""};
    long long q = std::stoll(between(f.partners.createProject(rohan, rare).dump(), "\"id\":", ",").substr(5));
    auto mq = f.partners.matches(rohan, q, 20).dump();
    CHECK(contains(mq, "\"candidates\":[]"));
    CHECK(contains(mq, "mandatory skill Quantum Computing"));

    // Mandatory schedule: Meera (Mon/Tue mornings only) is filtered out, not just scored low.
    ProjectInput design{"Design sprint", "Weekend UI design sprint for a club app.", "design", "3", "2", "1", "15,16", "1",
                        "UI/UX Design:1:5:0,Figma:1:3:0", ""};
    long long d = std::stoll(between(f.partners.createProject(arjun, design).dump(), "\"id\":", ",").substr(5));
    auto md = f.partners.matches(arjun, d, 20).dump();
    CHECK(!contains(between(md, "\"candidates\":", "\"eligible\""), "Meera Iyer"));
    CHECK(contains(md, "\"noSharedTime\":1"));
    ProjectInput noSlots = design;
    noSlots.availability = "";
    CHECK_THROWS_STATUS(f.partners.createProject(arjun, noSlots), 400);  // mandatory schedule needs slots

    // Bad project input.
    ProjectInput badDomain = ml;
    badDomain.domain = "astrology";
    CHECK_THROWS_STATUS(f.partners.createProject(rohan, badDomain), 400);
    ProjectInput badSize = ml;
    badSize.teamSize = "9";
    CHECK_THROWS_STATUS(f.partners.createProject(rohan, badSize), 400);
    ProjectInput noSkills = ml;
    noSkills.skills = "";
    CHECK_THROWS_STATUS(f.partners.createProject(rohan, noSkills), 400);
    CHECK_THROWS_STATUS(f.partners.matches(rohan, 999999, 10), 404);
}

/* ---------------- Skill gap + learning recommendations ------------ */

static const learn::SkillGapRow* rowFor(const learn::GapReport& r, const std::string& name) {
    for (const auto& row : r.rows) if (row.name == name) return &row;
    return nullptr;
}

TEST(learning_gap_formulas) {
    // Spec example (on CampusX's 1-3 scale): ML project needs Python, NumPy, Pandas, Statistics, Scikit-learn, Data Viz.
    std::vector<learn::RequiredSkill> req = {{"Python", 2, 3}, {"NumPy", 2, 3}, {"Pandas", 2, 2},
                                             {"Statistics", 2, 3}, {"Scikit-learn", 2, 2}, {"Data Visualization", 1, 1}};
    auto r = learn::analyse(req, {{"python", 3}, {"pandas", 1}, {"sql", 2}});
    CHECK(std::string(rowFor(r, "Python")->status) == "sufficient");  // above target is fine
    CHECK(rowFor(r, "Python")->gap == 0);
    CHECK(std::string(rowFor(r, "Pandas")->status) == "improve");      // has it, not enough
    CHECK(rowFor(r, "Pandas")->gap == 1 && rowFor(r, "Pandas")->priority == 2);
    CHECK(std::string(rowFor(r, "NumPy")->status) == "missing");
    CHECK(rowFor(r, "NumPy")->priority == 6);                          // gap 2 * weight 3
    // Gap% = sum(w*G) / sum(w*T) = (0+6+2+6+4+1) / (6+6+4+6+4+1) = 19/27
    CHECK(std::fabs(r.gapPercent - 100.0 * 19 / 27) < 1e-9);
    // Focus: priority desc, then gap, then name -> NumPy(6), Statistics(6), Scikit-learn(4), Pandas(2), Data Viz(1)
    std::vector<std::string> order;
    for (const auto& f : r.focus) order.push_back(f.name);
    CHECK((order == std::vector<std::string>{"NumPy", "Statistics", "Scikit-learn", "Pandas", "Data Visualization"}));

    auto none = learn::analyse(req, {});
    CHECK(std::fabs(none.gapPercent - 100.0) < 1e-9 && none.focus.size() == req.size());
    auto all = learn::analyse(req, {{"python", 3}, {"numpy", 3}, {"pandas", 2}, {"statistics", 2}, {"scikit-learn", 2}, {"data visualization", 1}});
    CHECK(all.focus.empty() && all.gapPercent == 0.0);
    CHECK(learn::levelFromLabel(" intermediate ") == 2 && learn::levelFromLabel("Expert") == 3 && learn::levelFromLabel("?") == 0);
    CHECK(learn::titleKey("  NumPy -- for BEGINNERS!! ") == "numpy for beginners");
}

TEST(learning_ranking_filters_and_scores) {
    std::vector<learn::RequiredSkill> req = {{"NumPy", 2, 3}, {"Statistics", 2, 3}, {"Data Visualization", 1, 1}};
    auto gaps = learn::analyse(req, {{"numpy", 1}});  // NumPy: improve to 2; others missing
    std::vector<learn::Candidate> c = {
        {"NumPy", "NumPy Arrays for Intermediate Learners with Projects", "Practical array work", "Intermediate", {"arrays", "indexing", "broadcasting"}},
        {"NumPy", "numpy arrays for intermediate learners with projects!", "same title, different case", "Intermediate", {}},  // duplicate
        {"NumPy", "NumPy Arrays for Intermediate Learners and Projects", "near duplicate", "Intermediate", {}},           // near-duplicate
        {"NumPy", "Introduction to Web Development", "HTML and CSS", "Beginner", {"html"}},  // labelled NumPy, but stays (label trusted, low score)
        {"React", "React Hooks Crash Course", "Not a gap skill", "Beginner", {"react"}},    // unrelated
        {"Statistics", "Advanced Bayesian Statistics", "Too hard", "Advanced", {"bayes"}},  // above target
        {"Statistics", "   ", "empty title", "Beginner", {}},                               // empty
        {"Statistics", "Statistics Basics for Machine Learning", "Mean, variance", "Beginner", {"mean", "variance"}},
        {"", "Data Visualization with Matplotlib for Beginners", "Plots", "Beginner", {"plots"}},  // no label: found via title
        {"Statistics", "Hypothesis Testing Explained", "statistical tests", "Beginner", {"p-values"}},  // label only
    };
    auto r = learn::rankCandidates(c, gaps, {});
    CHECK(r.received == 10);
    CHECK(r.duplicates == 2);
    CHECK(r.unrelated == 1);
    CHECK(r.tooAdvanced == 1);
    CHECK(r.empty == 1);
    CHECK(r.top.size() == 5);  // 5 survivors
    // Best first: Statistics is missing entirely (priority 2*3 = 6) while NumPy needs one more level (1*3 = 3).
    CHECK(r.top[0].c.title == "Statistics Basics for Machine Learning");
    CHECK(r.top[1].c.title == "NumPy Arrays for Intermediate Learners with Projects");
    for (std::size_t i = 1; i < r.top.size(); ++i) CHECK(r.top[i - 1].total >= r.top[i].total);
    for (const auto& s : r.top) CHECK(s.total >= 0 && s.total <= 100);
    auto byTitle = [&](const std::string& t) -> const learn::Scored* {
        for (const auto& s : r.top) if (s.c.title == t) return &s;
        return nullptr;
    };
    CHECK(byTitle("Introduction to Web Development")->skillScore == 50);   // label only, ranks low
    CHECK(byTitle("Hypothesis Testing Explained")->skillScore == 50);
    CHECK(byTitle("Data Visualization with Matplotlib for Beginners")->skill == "Data Visualization");
    CHECK(r.top.back().c.title == "Introduction to Web Development");  // weakest match is last

    // Already saved titles are skipped; per-skill cap; fewer than 5 valid -> fewer shown (nothing invented).
    auto skipped = learn::rankCandidates(c, gaps, {learn::titleKey(c[0].title)});
    CHECK(skipped.alreadySaved == 2);  // c[0] and c[1] are the same title once normalised
    auto capped = learn::rankCandidates(c, gaps, {}, 5, 1);
    std::set<std::string> skills;
    for (const auto& s : capped.top) skills.insert(s.skill);
    CHECK(skills.size() == capped.top.size());  // one per skill
    CHECK(learn::rankCandidates({c[7]}, gaps, {}).top.size() == 1);
    CHECK(learn::rankCandidates(c, learn::analyse(req, {{"numpy", 3}, {"statistics", 3}, {"data visualization", 3}}), {}).top.empty());
}

TEST(json_parser_reads_gemini_style_json) {
    JsonValue v = parseJson(R"( {"a": [1, -2.5e1, true, false, null], "s": "q\"\\\/\né😀", "o": {}} )");
    CHECK(v.isObject());
    CHECK(v.get("a")->items.size() == 5);
    CHECK(v.get("a")->items[1].number == -25.0);
    CHECK(v.get("a")->items[2].boolean);
    CHECK(v.str("s") == "q\"\\/\n\xC3\xA9\xF0\x9F\x98\x80");  // é and an emoji as UTF-8
    CHECK(v.get("missing") == nullptr && v.str("a").empty());
    for (const char* bad : {"", "{", "[1,]", "{\"a\" 1}", "\"open", "tru", "[1] x", "{\"a\":\"\x01\"}"}) {
        bool threw = false;
        try { parseJson(bad); } catch (const std::runtime_error&) { threw = true; }
        CHECK(threw);
    }
    std::string deep(100, '[');
    bool threw = false;
    try { parseJson(deep); } catch (const std::runtime_error&) { threw = true; }
    CHECK(threw);  // depth limit, no stack overflow
}

TEST(gemini_request_and_response_handling) {
    std::string body = GeminiClient::requestBody("say \"hi\"", "gemini-3.1-flash-lite");
    CHECK(contains(body, "\"responseMimeType\":\"application/json\""));
    CHECK(contains(body, "\"thinkingLevel\":\"low\""));
    CHECK(contains(body, "say \\\"hi\\\""));  // prompt is escaped
    CHECK(!contains(GeminiClient::requestBody("x", "gemini-flash-lite-latest"), "thinkingLevel"));

    auto ok = GeminiClient::parseResponse(200,
        R"({"candidates":[{"content":{"parts":[{"text":"thinking...","thought":true},{"text":"{\"recommendations\":[]}"}]}}]})");
    CHECK(ok.ok && ok.text == "{\"recommendations\":[]}");  // the model's reasoning part is skipped
    CHECK(!GeminiClient::parseResponse(429, "").ok);
    CHECK(contains(GeminiClient::parseResponse(429, "").error, "limit"));
    CHECK(contains(GeminiClient::parseResponse(503, "{}").error, "busy"));
    CHECK(contains(GeminiClient::parseResponse(404, "").error, "isn't available"));
    CHECK(contains(GeminiClient::parseResponse(403, "").error, "API key"));
    CHECK(contains(GeminiClient::parseResponse(200, "not json").error, "valid JSON"));
    CHECK(contains(GeminiClient::parseResponse(200, R"({"promptFeedback":{"blockReason":"SAFETY"}})").error, "blocked"));
    CHECK(contains(GeminiClient::parseResponse(200, R"({"candidates":[{"content":{"parts":[{"text":"  "}]}}]})").error, "empty"));

    GeminiClient noKey("", {"m"});
    CHECK(!noKey.configured());
    CHECK(!noKey.generateJson("x").ok);  // no network call without a key

    auto cands = LearningService::parseCandidates("Here you go:\n```json\n{\"recommendations\":[{\"skill\":\"NumPy\",\"title\":\"T\",\"topics\":[\"a\",1,\"b\"],\"level\":\"Beginner\"},5]}\n```");
    CHECK(cands.size() == 1 && cands[0].topics.size() == 2 && cands[0].description.empty());
    CHECK(LearningService::parseCandidates("[{\"title\":\"x\"}]").size() == 1);  // bare array also accepted
    bool threw = false;
    try { LearningService::parseCandidates("{\"other\":1}"); } catch (const std::exception&) { threw = true; }
    CHECK(threw);
}

TEST(learning_service_flow) {
    Fixture f;
    User aryaman = f.aryaman();  // seed profile: Machine Learning (Intermediate), Python (Beginner)
    std::string reply = R"({"recommendations":[
        {"skill":"NumPy","title":"NumPy for Beginners","description":"Arrays","topics":["arrays","indexing"],"level":"Beginner"},
        {"skill":"NumPy","title":"NumPy Projects for Machine Learning","description":"Practice","topics":["vectors"],"level":"Intermediate"},
        {"skill":"Statistics","title":"Statistics Fundamentals","description":"Mean and variance","topics":["mean"],"level":"Beginner"},
        {"skill":"Python","title":"Python Intermediate Tutorial with Examples","description":"Functions","topics":["functions","files"],"level":"Intermediate"},
        {"skill":"Statistics","title":"Statistics Fundamentals!","description":"duplicate","topics":[],"level":"Beginner"},
        {"skill":"React","title":"React Crash Course","description":"unrelated","topics":[],"level":"Beginner"}]})";
    int calls = 0;
    std::string lastPrompt;
    bool fail = false;
    LearningService learning(f.db, [&](const std::string& prompt) {
        ++calls;
        lastPrompt = prompt;
        GeminiResult r;
        if (fail) { r.error = "Gemini is busy right now."; return r; }
        r.ok = true; r.text = reply; r.model = "fake";
        return r;
    }, true);

    CHECK(contains(learning.goalsJson().dump(), "\"ml_project\""));
    CHECK_THROWS_STATUS(learning.analysis(aryaman, "flying"), 400);
    auto a = learning.analysis(aryaman, "ml_project").dump();
    CHECK(contains(a, "\"hasSkills\":true"));
    CHECK(contains(a, "\"skill\":\"Python\",\"current\":1,"));   // read from the partner profile
    CHECK(contains(a, "\"status\":\"improve\""));
    CHECK(contains(a, "\"recommendations\":[]"));

    auto run = learning.recommend(aryaman, "ml_project").dump();
    CHECK(calls == 1);
    CHECK(!contains(lastPrompt, "Aryaman") && !contains(lastPrompt, "@"));  // no personal data sent
    CHECK(contains(lastPrompt, "NumPy"));
    CHECK(contains(run, "\"source\":\"gemini\""));
    CHECK(contains(run, "\"received\":6"));
    CHECK(contains(run, "\"duplicates\":1"));
    CHECK(contains(run, "\"unrelated\":1"));
    CHECK(contains(run, "\"shown\":4"));  // only 4 valid: none invented
    auto count = [&](const char* status) {
        auto st = f.db.prepare("SELECT COUNT(*) FROM learning_recommendations WHERE user_id = ? AND status = ?");
        st.bind(1, aryaman.id).bind(2, status);
        st.step();
        return st.getInt(0);
    };
    CHECK(count("new") == 4);

    // Save one, complete one; a new run replaces only the unsaved ones and doesn't repeat saved titles.
    long long first = 0, second = 0;
    {
        auto st = f.db.prepare("SELECT id FROM learning_recommendations WHERE user_id = ? ORDER BY score DESC, id LIMIT 2");
        st.bind(1, aryaman.id);
        st.step(); first = st.getInt(0);
        st.step(); second = st.getInt(0);
    }
    learning.setStatus(aryaman, first, "saved");
    learning.setStatus(aryaman, second, "completed");
    CHECK_THROWS_STATUS(learning.setStatus(aryaman, first, "watched"), 400);
    CHECK_THROWS_STATUS(learning.setStatus(f.login("neha.sample@campusx.local"), first, "completed"), 404);  // not theirs
    auto again = learning.recommend(aryaman, "ml_project").dump();
    // 3 skipped: the saved and completed titles, plus "Statistics Fundamentals!" which is the same title as one of them.
    CHECK(contains(again, "\"alreadySaved\":3"));
    CHECK(contains(again, "\"shown\":2"));
    CHECK(count("new") == 2 && count("saved") == 1 && count("completed") == 1);
    CHECK(contains(again, "\"completed\":1,\"saved\":1"));

    // Gemini down: clearly-labelled standard suggestions, ranked by the same engine.
    fail = true;
    auto fb = learning.recommend(aryaman, "ml_project").dump();
    CHECK(contains(fb, "\"source\":\"fallback\""));
    CHECK(contains(fb, "Gemini is busy"));
    CHECK(contains(fb, "full course for beginners"));
    CHECK(count("saved") == 1 && count("completed") == 1);  // progress is never lost

    // No skills at all -> asked to add them first; no Gemini call.
    int before = calls;
    CHECK_THROWS_STATUS(learning.recommend(f.login("faculty@campusx.local"), "ml_project"), 400);
    CHECK(calls == before);
    CHECK(contains(learning.analysis(f.login("faculty@campusx.local"), "ml_project").dump(), "\"hasSkills\":false"));
}

int main() {
    for (auto& [name, fn] : registry()) {
        int before = g_failures;
        try {
            fn();
        } catch (const std::exception& e) {
            ++g_failures;
            std::cerr << "  EXCEPTION in " << name << ": " << e.what() << "\n";
        }
        std::cout << (g_failures == before ? "PASS  " : "FAIL  ") << name << "\n";
    }
    std::cout << "\n" << g_checks << " checks, " << g_failures << " failures\n";
    return g_failures == 0 ? 0 : 1;
}
