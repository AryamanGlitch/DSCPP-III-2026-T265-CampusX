#pragma once
#include <string>

#include "db/Database.h"
#include "util/Json.h"
#include "services/AuthService.h"

namespace cx {

class FeedService {
public:
    explicit FeedService(Database& db);

    // Newest first. cursor = "<createdAt>_<id>" of the last post already shown.
    Json list(const User& viewer, const std::string& cursor, int limit, const std::string& category);
    Json create(const User& author, const std::string& content, const std::string& category);
    void remove(const User& requester, long long postId);
    Json toggleLike(const User& user, long long postId);
    Json listComments(long long postId);
    Json addComment(const User& user, long long postId, const std::string& content);

    // Top-K "hot" posts and hashtags from the last 7 days.
    Json trending(int k, long long now);

    static bool validCategory(const std::string& c);

private:
    Database& db_;
};

}  // namespace cx
