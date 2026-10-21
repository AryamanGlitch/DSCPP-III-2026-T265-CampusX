#include "services/AuthService.h"

#include "util/Crypto.h"
#include "util/HttpError.h"
#include "util/Text.h"

namespace cx {

namespace {
constexpr long long SESSION_MS = 7LL * 24 * 60 * 60 * 1000;  // 7 days

std::string normaliseEmail(const std::string& raw) { return toLowerAscii(trim(raw)); }

bool looksLikeEmail(const std::string& e) {
    auto at = e.find('@');
    return e.size() <= 254 && at != std::string::npos && at > 0 && e.find('.', at) != std::string::npos &&
           e.find(' ') == std::string::npos && e.back() != '.';
}
}  // namespace

AuthService::AuthService(Database& db, int iterations, std::string allowedEmailDomain)
    : db_(db), iterations_(iterations), allowedDomain_(toLowerAscii(trim(allowedEmailDomain))) {
    dummyHash_ = crypto::hashPassword("timing-equaliser", iterations_);
}

Json AuthService::toJson(const User& u) {
    return Json::object({{"id", u.id}, {"name", u.name}, {"email", u.email}, {"role", u.role}});
}

long long AuthService::createUser(const std::string& name, const std::string& email, const std::string& password,
                                  const std::string& role) {
    std::string hash = crypto::hashPassword(password, iterations_);  // slow: do it before taking the lock
    auto lock = db_.lock();
    db_.prepare("INSERT INTO users (name, email, password_hash, role, created_at) VALUES (?, ?, ?, ?, ?)")
        .bind(1, trim(name)).bind(2, normaliseEmail(email)).bind(3, hash).bind(4, role).bind(5, nowMs())
        .run();
    return db_.lastInsertId();
}

AuthResult AuthService::registerUser(const std::string& nameIn, const std::string& emailIn, const std::string& password) {
    std::string name = trim(nameIn);
    std::string email = normaliseEmail(emailIn);
    if (name.size() < 2 || name.size() > 80) throw HttpError(400, "Name must be 2 to 80 characters.");
    if (!looksLikeEmail(email)) throw HttpError(400, "Enter a valid email address.");
    if (!allowedDomain_.empty()) {
        std::string suffix = "@" + allowedDomain_;
        if (email.size() <= suffix.size() || email.compare(email.size() - suffix.size(), suffix.size(), suffix) != 0) {
            throw HttpError(400, "Use your " + suffix + " email address.");
        }
    }
    if (password.size() < 8 || password.size() > 128) throw HttpError(400, "Password must be 8 to 128 characters.");

    long long id;
    try {
        id = createUser(name, email, password, "student");  // role is never taken from the request
    } catch (const HttpError& e) {
        if (e.status == 409) throw HttpError(409, "An account with this email already exists.");
        throw;
    }
    return startSession(User{id, name, email, "student"});
}

AuthResult AuthService::login(const std::string& emailIn, const std::string& password) {
    std::string email = normaliseEmail(emailIn);
    std::optional<User> user;
    std::string storedHash;
    {
        auto lock = db_.lock();
        auto st = db_.prepare("SELECT id, name, email, role, password_hash FROM users WHERE email = ?");
        st.bind(1, email);
        if (st.step()) {
            user = User{st.getInt(0), st.getText(1), st.getText(2), st.getText(3)};
            storedHash = st.getText(4);
        }
    }
    // Always run PBKDF2, even for unknown emails, so response time doesn't reveal which emails exist.
    bool ok = crypto::verifyPassword(password, user ? storedHash : dummyHash_);
    if (!user || !ok) throw HttpError(401, "Email or password is incorrect.");
    return startSession(*user);
}

AuthResult AuthService::startSession(const User& u) {
    std::string token = crypto::toHex(crypto::randomBytes(32));
    long long now = nowMs();
    auto lock = db_.lock();
    db_.prepare("DELETE FROM sessions WHERE expires_at < ?").bind(1, now).run();
    db_.prepare("INSERT INTO sessions (token_hash, user_id, expires_at) VALUES (?, ?, ?)")
        .bind(1, crypto::sha256Hex(token)).bind(2, u.id).bind(3, now + SESSION_MS).run();
    return {token, u};
}

void AuthService::logout(const std::string& token) {
    auto lock = db_.lock();
    db_.prepare("DELETE FROM sessions WHERE token_hash = ?").bind(1, crypto::sha256Hex(token)).run();
}

std::optional<User> AuthService::userForToken(const std::string& token) {
    if (token.size() != 64) return std::nullopt;
    auto lock = db_.lock();
    auto st = db_.prepare(
        "SELECT u.id, u.name, u.email, u.role FROM sessions s JOIN users u ON u.id = s.user_id "
        "WHERE s.token_hash = ? AND s.expires_at > ?");
    st.bind(1, crypto::sha256Hex(token)).bind(2, nowMs());
    if (!st.step()) return std::nullopt;
    return User{st.getInt(0), st.getText(1), st.getText(2), st.getText(3)};
}

}  // namespace cx
