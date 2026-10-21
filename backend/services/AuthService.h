#pragma once
#include <optional>
#include <string>
#include <utility>

#include "db/Database.h"
#include "util/Json.h"

namespace cx {

struct User {
    long long id = 0;
    std::string name, email, role;
    bool isAdmin() const { return role == "admin"; }
    bool canPostOfficial() const { return role == "faculty" || role == "admin"; }
};

struct AuthResult {
    std::string token;  // give to the client; only its hash is stored
    User user;
};

class AuthService {
public:
    // iterations: PBKDF2 work factor. ~100k+ in production, lower in tests for speed.
    AuthService(Database& db, int iterations, std::string allowedEmailDomain = "");

    AuthResult registerUser(const std::string& name, const std::string& email, const std::string& password);
    AuthResult login(const std::string& email, const std::string& password);
    void logout(const std::string& token);
    std::optional<User> userForToken(const std::string& token);

    // Creates a user with any role. For seeding/admin tools only: never exposed over HTTP.
    long long createUser(const std::string& name, const std::string& email, const std::string& password,
                         const std::string& role);

    static Json toJson(const User& u);

private:
    AuthResult startSession(const User& u);

    Database& db_;
    int iterations_;
    std::string allowedDomain_;
    std::string dummyHash_;  // used to equalise timing for unknown emails
};

}  // namespace cx
