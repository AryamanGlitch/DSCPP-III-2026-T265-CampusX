// Tests: Search (Trie, edit distance, Top-K, LRU), heap, rate limiter and auth.
#include "TestHarness.h"

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

TEST(auth_email_domain_restriction) {
    Database db(":memory:");
    createSchema(db);
    AuthService auth(db, 100, "geu.ac.in");
    CHECK_THROWS_STATUS(auth.registerUser("Some One", "someone@gmail.com", "password123"), 400);
    CHECK(auth.registerUser("Some One", "someone@geu.ac.in", "password123").user.id > 0);
}

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
