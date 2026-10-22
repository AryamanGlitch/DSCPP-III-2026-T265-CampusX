// Shared test harness for the CampusX suite (split out of the original
// tests/test_main.cpp so each owner keeps their tests in their own folder).
// No framework needed: TEST() registers a function, CHECK() counts a check.
// Uses an in-memory SQLite database.
#pragma once

#include <algorithm>
#include <cmath>
#include <functional>
#include <iostream>
#include <random>
#include <set>
#include <string>
#include <vector>

#include "db/Database.h"
#include "dsa/EditDistance.h"
#include "dsa/Graph.h"
#include "dsa/LRUCache.h"
#include "dsa/MinHeap.h"
#include "dsa/RateLimiter.h"
#include "dsa/TopK.h"
#include "dsa/Trie.h"
#include "seed/Seed.h"
#include "services/AuthService.h"
#include "services/FeedService.h"
#include "services/NavService.h"
#include "services/SearchService.h"
#include "util/Crypto.h"
#include "util/HttpError.h"
#include "util/Text.h"

using namespace cx;

inline int g_checks = 0, g_failures = 0;
inline std::vector<std::pair<const char*, void (*)()>>& registry() {
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

inline bool contains(const std::string& hay, const std::string& needle) { return hay.find(needle) != std::string::npos; }

// A fully seeded in-memory campus shared by the service tests.
struct Fixture {
    Database db{":memory:"};
    AuthService auth{db, 1000};  // low PBKDF2 cost keeps tests fast
    SearchService search{db};
    FeedService feed{db};
    NavService nav{db};
    Fixture() {
        createSchema(db);
        seedDatabase(db, auth, "TestPass@123");
        search.rebuild();
        nav.reload();
    }
    User login(const std::string& email) { return auth.login(email, "TestPass@123").user; }
};
