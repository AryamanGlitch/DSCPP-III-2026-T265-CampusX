// Tests: Campus feed: likes, pagination, roles, trending.
#include "TestHarness.h"

TEST(feed_like_toggle_is_idempotent) {
    Fixture f;
    User student = f.login("student@campusx.local");
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
    User u = f.login("student@campusx.local");
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
    User student = f.login("student@campusx.local");
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
