#include "services/FeedService.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <unordered_map>
#include <unordered_set>

#include "dsa/TopK.h"
#include "util/HttpError.h"
#include "util/Text.h"

namespace cx {

namespace {
const char* CATEGORIES[] = {"general", "announcement", "academic", "event", "lost_found"};
constexpr std::size_t MAX_POST = 2000, MAX_COMMENT = 500;
constexpr long long WEEK_MS = 7LL * 24 * 60 * 60 * 1000;

Json authorJson(const Statement& st, int idCol) {
    if (st.isNull(idCol)) return Json::object({{"id", nullptr}, {"name", "Deleted user"}, {"role", nullptr}});
    return Json::object({{"id", st.getInt(idCol)}, {"name", st.getText(idCol + 1)}, {"role", st.getText(idCol + 2)}});
}

// Extract #hashtags (letters, digits, underscore), lower-cased, once per post.
std::unordered_set<std::string> hashtags(const std::string& text) {
    std::unordered_set<std::string> tags;
    for (std::size_t i = 0; i < text.size(); ++i) {
        if (text[i] != '#') continue;
        std::size_t j = i + 1;
        while (j < text.size() && (std::isalnum(static_cast<unsigned char>(text[j])) || text[j] == '_')) ++j;
        if (j - i > 2 && j - i <= 41) tags.insert(toLowerAscii(text.substr(i + 1, j - i - 1)));
        i = j - 1;
    }
    return tags;
}
}  // namespace

FeedService::FeedService(Database& db) : db_(db) {}

bool FeedService::validCategory(const std::string& c) {
    return std::any_of(std::begin(CATEGORIES), std::end(CATEGORIES), [&](const char* x) { return c == x; });
}

Json FeedService::list(const User& viewer, const std::string& cursor, int limit, const std::string& category) {
    limit = std::clamp(limit, 1, 50);
    if (!category.empty() && !validCategory(category)) throw HttpError(400, "Unknown category.");

    long long cursorTime = 0, cursorId = 0;
    if (!cursor.empty()) {
        auto sep = cursor.find('_');
        if (sep == std::string::npos || !isDigits(cursor.substr(0, sep)) || !isDigits(cursor.substr(sep + 1))) {
            throw HttpError(400, "Invalid cursor.");
        }
        cursorTime = std::stoll(cursor.substr(0, sep));
        cursorId = std::stoll(cursor.substr(sep + 1));
    }

    // Keyset pagination: "older than the last post I saw". Unlike OFFSET,
    // new posts arriving mid-scroll can't cause duplicates or skipped posts,
    // and the index makes every page equally fast.
    std::string sql =
        "SELECT p.id, p.content, p.category, p.is_official, p.like_count, p.comment_count, p.created_at, "
        "       u.id, u.name, u.role, "
        "       EXISTS (SELECT 1 FROM likes l WHERE l.post_id = p.id AND l.user_id = ?1) "
        "FROM posts p LEFT JOIN users u ON u.id = p.author_id WHERE 1 = 1";
    if (!category.empty()) sql += " AND p.category = ?2";
    if (!cursor.empty()) sql += " AND (p.created_at < ?3 OR (p.created_at = ?3 AND p.id < ?4))";
    sql += " ORDER BY p.created_at DESC, p.id DESC LIMIT ?5";

    auto lock = db_.lock();
    auto st = db_.prepare(sql);
    st.bind(1, viewer.id);
    if (!category.empty()) st.bind(2, category);
    if (!cursor.empty()) st.bind(3, cursorTime).bind(4, cursorId);
    st.bind(5, limit + 1);  // one extra row tells us whether another page exists

    Json posts = Json::array();
    int count = 0;
    long long lastTime = 0, lastId = 0;
    bool hasMore = false;
    while (st.step()) {
        if (count == limit) { hasMore = true; break; }
        ++count;
        lastId = st.getInt(0);
        lastTime = st.getInt(6);
        bool mine = !st.isNull(7) && st.getInt(7) == viewer.id;
        posts.push(Json::object({
            {"id", lastId},
            {"content", st.getText(1)},
            {"category", st.getText(2)},
            {"isOfficial", st.getInt(3) != 0},
            {"likeCount", st.getInt(4)},
            {"commentCount", st.getInt(5)},
            {"createdAt", lastTime},
            {"author", authorJson(st, 7)},
            {"likedByMe", st.getInt(10) != 0},
            {"canDelete", mine || viewer.isAdmin()},
        }));
    }
    Json next = hasMore ? Json(std::to_string(lastTime) + "_" + std::to_string(lastId)) : Json();
    return Json::object({{"posts", posts}, {"nextCursor", next}});
}

Json FeedService::create(const User& author, const std::string& contentIn, const std::string& categoryIn) {
    std::string content = trim(contentIn);
    std::string category = categoryIn.empty() ? "general" : categoryIn;
    if (content.empty()) throw HttpError(400, "Write something first.");
    if (content.size() > MAX_POST) throw HttpError(400, "Posts can be at most 2000 characters.");
    if (!validCategory(category)) throw HttpError(400, "Unknown category.");
    if (category == "announcement" && !author.canPostOfficial()) {
        throw HttpError(403, "Only faculty and admins can post announcements.");
    }

    long long now = nowMs();
    auto lock = db_.lock();
    db_.prepare("INSERT INTO posts (author_id, content, category, is_official, created_at) VALUES (?, ?, ?, ?, ?)")
        .bind(1, author.id).bind(2, content).bind(3, category).bind(4, author.canPostOfficial() ? 1 : 0).bind(5, now)
        .run();
    long long id = db_.lastInsertId();
    return Json::object({
        {"id", id}, {"content", content}, {"category", category}, {"isOfficial", author.canPostOfficial()},
        {"likeCount", 0}, {"commentCount", 0}, {"createdAt", now},
        {"author", Json::object({{"id", author.id}, {"name", author.name}, {"role", author.role}})},
        {"likedByMe", false}, {"canDelete", true},
    });
}

void FeedService::remove(const User& requester, long long postId) {
    auto lock = db_.lock();
    auto st = db_.prepare("SELECT author_id FROM posts WHERE id = ?");
    st.bind(1, postId);
    if (!st.step()) throw HttpError(404, "Post not found.");
    bool owner = !st.isNull(0) && st.getInt(0) == requester.id;
    if (!owner && !requester.isAdmin()) throw HttpError(403, "You can only delete your own posts.");
    db_.prepare("DELETE FROM posts WHERE id = ?").bind(1, postId).run();  // comments + likes cascade
}

Json FeedService::toggleLike(const User& user, long long postId) {
    auto lock = db_.lock();
    Transaction tx(db_);
    {
        auto st = db_.prepare("SELECT 1 FROM posts WHERE id = ?");
        st.bind(1, postId);
        if (!st.step()) throw HttpError(404, "Post not found.");
    }
    // The (post_id, user_id) primary key makes a duplicate like impossible.
    // INSERT OR IGNORE changes 1 row if it was new, 0 if already liked.
    db_.prepare("INSERT OR IGNORE INTO likes (post_id, user_id) VALUES (?, ?)").bind(1, postId).bind(2, user.id).run();
    bool liked = db_.changes() == 1;
    if (liked) {
        db_.prepare("UPDATE posts SET like_count = like_count + 1 WHERE id = ?").bind(1, postId).run();
    } else {
        db_.prepare("DELETE FROM likes WHERE post_id = ? AND user_id = ?").bind(1, postId).bind(2, user.id).run();
        db_.prepare("UPDATE posts SET like_count = MAX(like_count - 1, 0) WHERE id = ?").bind(1, postId).run();
    }
    auto st = db_.prepare("SELECT like_count FROM posts WHERE id = ?");
    st.bind(1, postId);
    st.step();
    long long count = st.getInt(0);
    tx.commit();
    return Json::object({{"liked", liked}, {"likeCount", count}});
}

Json FeedService::listComments(long long postId) {
    auto lock = db_.lock();
    auto st = db_.prepare(
        "SELECT c.id, c.content, c.created_at, u.id, u.name, u.role FROM comments c "
        "LEFT JOIN users u ON u.id = c.author_id WHERE c.post_id = ? ORDER BY c.created_at, c.id LIMIT 200");
    st.bind(1, postId);
    Json comments = Json::array();
    while (st.step()) {
        comments.push(Json::object({{"id", st.getInt(0)}, {"content", st.getText(1)}, {"createdAt", st.getInt(2)},
                                    {"author", authorJson(st, 3)}}));
    }
    return Json::object({{"comments", comments}});
}

Json FeedService::addComment(const User& user, long long postId, const std::string& contentIn) {
    std::string content = trim(contentIn);
    if (content.empty()) throw HttpError(400, "Write a comment first.");
    if (content.size() > MAX_COMMENT) throw HttpError(400, "Comments can be at most 500 characters.");
    long long now = nowMs();
    auto lock = db_.lock();
    Transaction tx(db_);
    db_.prepare("UPDATE posts SET comment_count = comment_count + 1 WHERE id = ?").bind(1, postId).run();
    if (db_.changes() == 0) throw HttpError(404, "Post not found.");
    db_.prepare("INSERT INTO comments (post_id, author_id, content, created_at) VALUES (?, ?, ?, ?)")
        .bind(1, postId).bind(2, user.id).bind(3, content).bind(4, now).run();
    long long id = db_.lastInsertId();
    tx.commit();
    return Json::object({{"id", id}, {"content", content}, {"createdAt", now},
                         {"author", Json::object({{"id", user.id}, {"name", user.name}, {"role", user.role}})}});
}

Json FeedService::trending(int k, long long now) {
    k = std::clamp(k, 1, 20);
    struct Row { long long id; std::string content, author; long long likes, comments, createdAt; };
    std::vector<Row> rows;
    {
        auto lock = db_.lock();
        auto st = db_.prepare(
            "SELECT p.id, p.content, COALESCE(u.name, 'Deleted user'), p.like_count, p.comment_count, p.created_at "
            "FROM posts p LEFT JOIN users u ON u.id = p.author_id WHERE p.created_at >= ?");
        st.bind(1, now - WEEK_MS);
        while (st.step()) {
            rows.push_back({st.getInt(0), st.getText(1), st.getText(2), st.getInt(3), st.getInt(4), st.getInt(5)});
        }
    }

    // "Hot" score with time decay (Hacker News style): engagement divided by
    // (age + 2)^1.5, so a new post with a few likes can beat an old popular one.
    TopK<std::size_t> hotPosts(static_cast<std::size_t>(k));
    std::unordered_map<std::string, long long> tagCounts;  // hash map: tag -> number of posts
    for (std::size_t i = 0; i < rows.size(); ++i) {
        const Row& r = rows[i];
        double ageHours = std::max(0.0, (now - r.createdAt) / 3'600'000.0);
        double score = (r.likes + 2.0 * r.comments + 1.0) / std::pow(ageHours + 2.0, 1.5);
        hotPosts.offer(score, -r.createdAt, i);  // tie: newer wins
        for (const auto& tag : hashtags(r.content)) ++tagCounts[tag];
    }

    Json posts = Json::array();
    for (const auto& [i, score] : hotPosts.takeSorted()) {
        const Row& r = rows[i];
        std::string snippet = r.content.size() > 140 ? r.content.substr(0, 137) + "..." : r.content;
        posts.push(Json::object({{"id", r.id}, {"snippet", snippet}, {"author", r.author}, {"likeCount", r.likes},
                                 {"commentCount", r.comments}, {"score", score}}));
    }

    TopK<std::string> topTags(static_cast<std::size_t>(k));
    for (const auto& [tag, count] : tagCounts) {
        // Tie-break alphabetically so results are stable between requests.
        long long tie = 0;
        for (std::size_t c = 0; c < std::min<std::size_t>(tag.size(), 7); ++c) tie = tie * 256 + static_cast<unsigned char>(tag[c]);
        topTags.offer(static_cast<double>(count), tie, tag);
    }
    Json tags = Json::array();
    for (const auto& [tag, count] : topTags.takeSorted()) {
        tags.push(Json::object({{"tag", tag}, {"count", static_cast<long long>(count)}}));
    }
    return Json::object({{"posts", posts}, {"hashtags", tags}});
}

}  // namespace cx
