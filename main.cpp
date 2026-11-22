// CampusX HTTP server (cpp-httplib).
// This file only translates HTTP <-> service calls. All logic lives in
// services/ and dsa/, which are tested without a network (see tests/).
//
// Requests: query string or application/x-www-form-urlencoded body.
// Responses: JSON. Errors: {"error": "message"} with a 4xx/5xx status.
#include <httplib.h>

#include <cstdlib>
#include <functional>
#include <iostream>
#include <string>
#include <unordered_map>

#include "backend/database/Database.h"
#include "backend/dsa/RateLimiter.h"
#include "backend/seed/Seed.h"
#include "backend/services/AuthService.h"
#include "backend/services/GeminiClient.h"
#include "backend/services/LearningService.h"
#include "backend/services.ved/FeedService.h"
#include "backend/Services.rit/NavService.h"
#include "backend/services.ved/PartnerService.h"
#include "backend/services/SearchService.h"
#include "backend/util/HttpError.h"
#include "backend/util/Json.h"
#include "backend/util/Text.h"

using namespace cx;

namespace {

struct Config {
    int port = 8080;
    std::string dbPath = "campusx.db";
    std::string publicDir = "Frontend";
    std::string emailDomain;  // e.g. geu.ac.in to restrict sign-ups
    std::string seedPassword = "ChangeMe@123";
    int pbkdf2Iterations = 120000;
    bool reseed = false;
};

std::string env(const char* key, const std::string& fallback) {
    const char* v = std::getenv(key);
    return v && *v ? std::string(v) : fallback;
}

Config loadConfig(int argc, char** argv) {
    Config c;
    c.port = static_cast<int>(toInt(env("CAMPUSX_PORT", "8080"), 8080));
    c.dbPath = env("CAMPUSX_DB", c.dbPath);
    c.publicDir = env("CAMPUSX_PUBLIC", c.publicDir);
    c.emailDomain = env("CAMPUSX_EMAIL_DOMAIN", "");
    c.seedPassword = env("CAMPUSX_SEED_PASSWORD", c.seedPassword);
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        auto value = [&]() -> std::string {
            if (i + 1 >= argc) throw std::invalid_argument(a + " needs a value");
            return argv[++i];
        };
        if (a == "--seed") c.reseed = true;
        else if (a == "--port") c.port = static_cast<int>(toInt(value(), 8080));
        else if (a == "--db") c.dbPath = value();
        else if (a == "--public") c.publicDir = value();
        else if (a == "--help") {
            std::cout << "Usage: campusx [--seed] [--port 8080] [--db campusx.db] [--public Frontend]\n"
                         "  --seed   wipe the database and load the SAMPLE campus (development only)\n";
            std::exit(0);
        } else {
            throw std::invalid_argument("Unknown option " + a + " (try --help)");
        }
    }
    return c;
}

const char* JSON_TYPE = "application/json; charset=utf-8";

void sendRaw(httplib::Response& res, int status, const std::string& json) {
    res.status = status;
    res.set_header("Cache-Control", "no-store");
    res.set_header("X-Content-Type-Options", "nosniff");
    res.set_content(json, JSON_TYPE);
}
void sendJson(httplib::Response& res, int status, const Json& body) { sendRaw(res, status, body.dump()); }
void sendError(httplib::Response& res, int status, const std::string& message) {
    sendJson(res, status, Json::object({{"error", message}}));
}
void sendNoContent(httplib::Response& res) { res.status = 204; }

// Query string + form body merged into one lookup (body wins).
class Params {
public:
    explicit Params(const httplib::Request& req) {
        for (const auto& [k, v] : req.params) values_[k] = v;
        if (req.get_header_value("Content-Type").rfind("application/x-www-form-urlencoded", 0) == 0) {
            for (const auto& [k, v] : parseForm(req.body)) values_[k] = v;
        }
    }
    std::string get(const std::string& key, const std::string& fallback = "") const {
        auto it = values_.find(key);
        return it == values_.end() ? fallback : it->second;
    }
    bool flag(const std::string& key) const {
        std::string v = get(key);
        return v == "1" || v == "true" || v == "on";
    }
    double number(const std::string& key) const {
        try {
            std::size_t used = 0;
            std::string s = get(key);
            double d = std::stod(s, &used);
            if (used != s.size()) throw std::invalid_argument("trailing");
            return d;
        } catch (const std::exception&) {
            throw HttpError(400, key + " must be a number.");
        }
    }

private:
    std::unordered_map<std::string, std::string> values_;
};

using Handler = std::function<void(const httplib::Request&, httplib::Response&)>;

// Every route goes through this: service errors become clean JSON responses.
Handler safe(Handler h) {
    return [h](const httplib::Request& req, httplib::Response& res) {
        try {
            h(req, res);
        } catch (const HttpError& e) {
            sendError(res, e.status, e.what());
        } catch (const std::exception& e) {
            std::cerr << "[500] " << req.method << " " << req.path << ": " << e.what() << "\n";
            sendError(res, 500, "Server error. Try again in a moment.");
        }
    };
}

std::string bearerToken(const httplib::Request& req) {
    std::string h = req.get_header_value("Authorization");
    return h.rfind("Bearer ", 0) == 0 ? h.substr(7) : "";
}

long long pathId(const httplib::Request& req) { return std::stoll(req.matches[1].str()); }

}  // namespace

int main(int argc, char** argv) {
    Config cfg;
    try {
        cfg = loadConfig(argc, argv);
    } catch (const std::exception& e) {
        std::cerr << e.what() << "\n";
        return 2;
    }

    try {
        Database db(cfg.dbPath);
        createSchema(db);
        AuthService auth(db, cfg.pbkdf2Iterations, cfg.emailDomain);

        if (cfg.reseed || isDatabaseEmpty(db)) {
            std::cout << "Loading SAMPLE campus data (placeholders, not real)...\n";
            seedDatabase(db, auth, cfg.seedPassword);
            std::cout << "Demo logins, password \"" << cfg.seedPassword << "\":\n"
                      << "  admin@campusx.local  faculty@campusx.local\n"
                      << "Demo student: " << DEMO_STUDENT_EMAIL << " (own password, see README)\n";
        } else if (upgradeLegacyDemoStudent(db, auth)) {
            std::cout << "Moved the old demo student (student@campusx.local) to " << DEMO_STUDENT_EMAIL << ".\n";
        }
        if (partnerSampleMissing(db)) {
            std::cout << "Adding SAMPLE student profiles for project partner matching...\n";
            seedPartnerSample(db, auth, cfg.seedPassword);
        }

        SearchService search(db);
        FeedService feed(db);
        NavService nav(db);
        PartnerService partners(db);
        GeminiClient gemini(GeminiClient::loadKey("gemini.key"), GeminiClient::loadModels(), 20);
        LearningService learning(db, [&gemini](const std::string& prompt) { return gemini.generateJson(prompt); },
                                 gemini.configured());
        std::cout << "Gemini: " << (gemini.configured() ? "on" : GeminiClient::available() ? "off (no API key)" : "off (built without libcurl)")
                  << "\n";
        search.rebuild();
        nav.reload();

        RateLimiter authLimiter(20, 15 * 60 * 1000);   // 20 login/sign-up attempts per 15 min per IP
        RateLimiter writeLimiter(30, 10 * 60 * 1000);  // 30 posts/comments per 10 min per IP
        RateLimiter geminiLimiter(5, 5 * 60 * 1000);   // 5 Gemini runs per 5 min per IP (free-tier quota is small)

        auto rateLimit = [](RateLimiter& rl, const httplib::Request& req) {
            // Behind a reverse proxy, remote_addr is the proxy; use X-Forwarded-For there instead.
            if (!rl.allow(req.remote_addr, nowMs())) throw HttpError(429, "Too many requests. Wait a few minutes and try again.");
        };
        auto requireUser = [&](const httplib::Request& req) -> User {
            auto user = auth.userForToken(bearerToken(req));
            if (!user) throw HttpError(401, "Log in to continue.");
            return *user;
        };
        auto requireAdmin = [&](const httplib::Request& req) -> User {
            User u = requireUser(req);
            if (!u.isAdmin()) throw HttpError(403, "Only admins can do this.");
            return u;
        };

        httplib::Server svr;
        svr.set_payload_max_length(64 * 1024);
        // Frontend/ is split like the repo: HTML pages at /, and Js, CSS, img folders at /js, /css, /img.
        if (!svr.set_mount_point("/", cfg.publicDir + "/HTML") || !svr.set_mount_point("/js", cfg.publicDir + "/Js") ||
            !svr.set_mount_point("/css", cfg.publicDir + "/CSS") || !svr.set_mount_point("/img", cfg.publicDir + "/img")) {
            std::cerr << "Frontend folder not found: " << cfg.publicDir
                      << "\nRun from the project root, or pass --public <path>.\n";
            return 1;
        }
        svr.set_logger([](const httplib::Request& req, const httplib::Response& res) {
            if (req.path.rfind("/api/", 0) == 0) std::cout << req.method << " " << req.path << " -> " << res.status << "\n";
        });

        svr.Get("/api/health", safe([&](const httplib::Request&, httplib::Response& res) {
            sendJson(res, 200, Json::object({{"ok", true}, {"time", nowMs()}, {"search", search.stats()}}));
        }));

        /* ---------- Auth ---------- */
        svr.Post("/api/auth/register", safe([&](const httplib::Request& req, httplib::Response& res) {
            rateLimit(authLimiter, req);
            Params p(req);
            AuthResult r = auth.registerUser(p.get("name"), p.get("email"), p.get("password"));
            sendJson(res, 201, Json::object({{"token", r.token}, {"user", AuthService::toJson(r.user)}}));
        }));
        svr.Post("/api/auth/login", safe([&](const httplib::Request& req, httplib::Response& res) {
            rateLimit(authLimiter, req);
            Params p(req);
            AuthResult r = auth.login(p.get("email"), p.get("password"));
            sendJson(res, 200, Json::object({{"token", r.token}, {"user", AuthService::toJson(r.user)}}));
        }));
        svr.Post("/api/auth/logout", safe([&](const httplib::Request& req, httplib::Response& res) {
            auth.logout(bearerToken(req));
            sendNoContent(res);
        }));
        svr.Get("/api/auth/me", safe([&](const httplib::Request& req, httplib::Response& res) {
            sendJson(res, 200, Json::object({{"user", AuthService::toJson(requireUser(req))}}));
        }));

        /* ---------- Search (public: visitors need it too) ---------- */
        svr.Get("/api/search", safe([&](const httplib::Request& req, httplib::Response& res) {
            Params p(req);
            sendJson(res, 200, search.search(p.get("q"), p.get("kind", "all"), static_cast<int>(toInt(p.get("limit"), 20)), nowMs()));
        }));
        svr.Post("/api/search/reindex", safe([&](const httplib::Request& req, httplib::Response& res) {
            requireAdmin(req);
            search.rebuild();
            sendJson(res, 200, search.stats());
        }));

        /* ---------- Feed (campus members only) ---------- */
        svr.Get("/api/feed", safe([&](const httplib::Request& req, httplib::Response& res) {
            User u = requireUser(req);
            Params p(req);
            sendJson(res, 200, feed.list(u, p.get("cursor"), static_cast<int>(toInt(p.get("limit"), 10)), p.get("category")));
        }));
        svr.Get("/api/feed/trending", safe([&](const httplib::Request& req, httplib::Response& res) {
            requireUser(req);
            sendJson(res, 200, feed.trending(static_cast<int>(toInt(Params(req).get("k"), 5)), nowMs()));
        }));
        svr.Post("/api/feed", safe([&](const httplib::Request& req, httplib::Response& res) {
            User u = requireUser(req);
            rateLimit(writeLimiter, req);
            Params p(req);
            sendJson(res, 201, feed.create(u, p.get("content"), p.get("category", "general")));
        }));
        svr.Delete(R"(/api/feed/(\d{1,15}))", safe([&](const httplib::Request& req, httplib::Response& res) {
            feed.remove(requireUser(req), pathId(req));
            sendNoContent(res);
        }));
        svr.Post(R"(/api/feed/(\d{1,15})/like)", safe([&](const httplib::Request& req, httplib::Response& res) {
            sendJson(res, 200, feed.toggleLike(requireUser(req), pathId(req)));
        }));
        svr.Get(R"(/api/feed/(\d{1,15})/comments)", safe([&](const httplib::Request& req, httplib::Response& res) {
            requireUser(req);
            sendJson(res, 200, feed.listComments(pathId(req)));
        }));
        svr.Post(R"(/api/feed/(\d{1,15})/comments)", safe([&](const httplib::Request& req, httplib::Response& res) {
            User u = requireUser(req);
            rateLimit(writeLimiter, req);
            sendJson(res, 201, feed.addComment(u, pathId(req), Params(req).get("content")));
        }));

        /* ---------- Project partner matching (campus members only) ---------- */
        svr.Get("/api/partners/meta", safe([&](const httplib::Request& req, httplib::Response& res) {
            requireUser(req);
            sendJson(res, 200, partners.meta());
        }));
        svr.Get("/api/partners/profile", safe([&](const httplib::Request& req, httplib::Response& res) {
            sendJson(res, 200, partners.myProfile(requireUser(req)));
        }));
        svr.Post("/api/partners/profile", safe([&](const httplib::Request& req, httplib::Response& res) {
            User u = requireUser(req);
            Params p(req);
            sendJson(res, 200, partners.saveProfile(u, ProfileInput{p.get("department"), p.get("semester"), p.get("experience"),
                                                                    p.get("interests"), p.get("availability"), p.get("skills"), p.get("bio")}));
        }));
        svr.Delete("/api/partners/profile", safe([&](const httplib::Request& req, httplib::Response& res) {
            partners.deleteProfile(requireUser(req));
            sendNoContent(res);
        }));
        svr.Get(R"(/api/partners/students/(\d{1,15}))", safe([&](const httplib::Request& req, httplib::Response& res) {
            requireUser(req);
            sendJson(res, 200, partners.studentProfile(pathId(req)));
        }));
        svr.Get("/api/partners/projects", safe([&](const httplib::Request& req, httplib::Response& res) {
            User u = requireUser(req);
            sendJson(res, 200, partners.listProjects(u, Params(req).get("scope", "mine")));
        }));
        svr.Post("/api/partners/projects", safe([&](const httplib::Request& req, httplib::Response& res) {
            User u = requireUser(req);
            rateLimit(writeLimiter, req);
            Params p(req);
            sendJson(res, 201, partners.createProject(u, ProjectInput{p.get("title"), p.get("description"), p.get("domain"),
                                                                      p.get("teamSize"), p.get("durationWeeks"), p.get("experience"),
                                                                      p.get("availability"), p.get("scheduleRequired"), p.get("skills"),
                                                                      p.get("extra")}));
        }));
        svr.Get(R"(/api/partners/projects/(\d{1,15}))", safe([&](const httplib::Request& req, httplib::Response& res) {
            sendJson(res, 200, partners.project(requireUser(req), pathId(req)));
        }));
        svr.Patch(R"(/api/partners/projects/(\d{1,15})/status)", safe([&](const httplib::Request& req, httplib::Response& res) {
            User u = requireUser(req);
            sendJson(res, 200, partners.setStatus(u, pathId(req), Params(req).get("status")));
        }));
        svr.Get(R"(/api/partners/projects/(\d{1,15})/matches)", safe([&](const httplib::Request& req, httplib::Response& res) {
            User u = requireUser(req);
            sendJson(res, 200, partners.matches(u, pathId(req), static_cast<int>(toInt(Params(req).get("limit"), 10))));
        }));
        svr.Post(R"(/api/partners/projects/(\d{1,15})/invite)", safe([&](const httplib::Request& req, httplib::Response& res) {
            User u = requireUser(req);
            rateLimit(writeLimiter, req);
            Params p(req);
            long long studentId = toInt(p.get("studentId"), 0);
            if (studentId <= 0) throw HttpError(400, "studentId is required.");
            sendJson(res, 201, partners.invite(u, pathId(req), studentId, p.get("message")));
        }));
        svr.Post(R"(/api/partners/projects/(\d{1,15})/request)", safe([&](const httplib::Request& req, httplib::Response& res) {
            User u = requireUser(req);
            rateLimit(writeLimiter, req);
            sendJson(res, 201, partners.requestToJoin(u, pathId(req), Params(req).get("message")));
        }));
        svr.Post(R"(/api/partners/projects/(\d{1,15})/leave)", safe([&](const httplib::Request& req, httplib::Response& res) {
            partners.leave(requireUser(req), pathId(req));
            sendNoContent(res);
        }));
        svr.Get("/api/partners/invitations", safe([&](const httplib::Request& req, httplib::Response& res) {
            sendJson(res, 200, partners.invitations(requireUser(req)));
        }));
        svr.Post(R"(/api/partners/invitations/(\d{1,15})/respond)", safe([&](const httplib::Request& req, httplib::Response& res) {
            User u = requireUser(req);
            sendJson(res, 200, partners.respond(u, pathId(req), Params(req).get("action")));
        }));

        /* ---------- Skill gap + learning recommendations (campus members only) ---------- */
        svr.Get("/api/learning/goals", safe([&](const httplib::Request& req, httplib::Response& res) {
            requireUser(req);
            sendJson(res, 200, learning.goalsJson());
        }));
        svr.Get("/api/learning/analysis", safe([&](const httplib::Request& req, httplib::Response& res) {
            User u = requireUser(req);
            sendJson(res, 200, learning.analysis(u, Params(req).get("goal")));
        }));
        svr.Post("/api/learning/recommendations", safe([&](const httplib::Request& req, httplib::Response& res) {
            User u = requireUser(req);
            rateLimit(geminiLimiter, req);
            sendJson(res, 200, learning.recommend(u, Params(req).get("goal")));
        }));
        svr.Post(R"(/api/learning/recommendations/(\d{1,15})/status)", safe([&](const httplib::Request& req, httplib::Response& res) {
            User u = requireUser(req);
            sendJson(res, 200, learning.setStatus(u, pathId(req), Params(req).get("status")));
        }));

        /* ---------- Emergency (reads are public: no login wall in an emergency) ---------- */
        svr.Get("/api/emergency/graph", safe([&](const httplib::Request&, httplib::Response& res) {
            sendRaw(res, 200, nav.graphJson());
        }));
        svr.Get("/api/emergency/route", safe([&](const httplib::Request& req, httplib::Response& res) {
            Params p(req);
            sendJson(res, 200, nav.route(p.get("from"), p.get("to"), p.get("type", "fire"), p.flag("accessible")));
        }));
        svr.Get("/api/emergency/nearest", safe([&](const httplib::Request& req, httplib::Response& res) {
            Params p(req);
            sendJson(res, 200, nav.nearest(p.number("lat"), p.number("lng")));
        }));
        svr.Get("/api/emergency/health", safe([&](const httplib::Request& req, httplib::Response& res) {
            requireAdmin(req);
            sendJson(res, 200, nav.health());
        }));
        svr.Post("/api/emergency/hazards", safe([&](const httplib::Request& req, httplib::Response& res) {
            User admin = requireAdmin(req);
            Params p(req);
            sendJson(res, 201, nav.addHazard(admin, p.get("nodeId"), p.get("edgeFrom"), p.get("edgeTo"), p.get("description")));
        }));
        svr.Patch(R"(/api/emergency/hazards/(\d{1,15})/resolve)", safe([&](const httplib::Request& req, httplib::Response& res) {
            requireAdmin(req);
            nav.resolveHazard(pathId(req));
            sendJson(res, 200, Json::object({{"id", pathId(req)}, {"active", false}}));
        }));

        // Unknown API routes get JSON, not the static-file 404 page.
        Handler apiNotFound = [](const httplib::Request& req, httplib::Response& res) {
            sendError(res, 404, "No route for " + req.method + " " + req.path);
        };
        svr.Get(R"(/api/.*)", apiNotFound);
        svr.Post(R"(/api/.*)", apiNotFound);
        svr.Delete(R"(/api/.*)", apiNotFound);
        svr.Patch(R"(/api/.*)", apiNotFound);

        std::cout << "CampusX running at http://localhost:" << cfg.port << "  (Ctrl+C to stop)\n";
        if (!svr.listen("0.0.0.0", cfg.port)) {
            std::cerr << "Could not listen on port " << cfg.port << ". Is it already in use? Try --port 8081\n";
            return 1;
        }
    } catch (const std::exception& e) {
        std::cerr << "Startup failed: " << e.what() << "\n";
        return 1;
    }
    return 0;
}
