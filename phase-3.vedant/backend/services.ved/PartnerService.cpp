#include "PartnerService.h"

#include <algorithm>
#include <cmath>
#include <unordered_map>
#include <unordered_set>

#include "../util/HttpError.h"
#include "../util/Text.h"

namespace cx {

namespace {

struct Domain { const char *key, *label; };
// A domain's bit is its index here: append new domains at the end, never reorder.
const Domain DOMAINS[] = {
    {"ai_ml", "AI / Machine Learning"}, {"web", "Web Development"}, {"mobile", "Mobile Apps"},
    {"cybersecurity", "Cybersecurity"}, {"data_science", "Data Science"}, {"iot", "IoT & Embedded"},
    {"cloud", "Cloud & DevOps"}, {"design", "UI/UX Design"}, {"game_dev", "Game Development"},
    {"research", "Research"},
};
constexpr int DOMAIN_COUNT = static_cast<int>(sizeof(DOMAINS) / sizeof(DOMAINS[0]));
const char* DAY_NAMES[] = {"Mon", "Tue", "Wed", "Thu", "Fri", "Sat", "Sun"};
const char* PERIOD_NAMES[] = {"morning", "afternoon", "evening"};
const char* LEVEL_NAMES[] = {"", "Beginner", "Intermediate", "Advanced"};
// Statuses in which a project still takes up a member's time.
const char* ACTIVE_STATUSES = "('recruiting','team_complete','in_progress')";
constexpr std::size_t MAX_SKILLS_PROFILE = 25, MAX_SKILLS_PROJECT = 12, MAX_SKILL_NAME = 40;

int domainIndex(const std::string& key) {
    for (int i = 0; i < DOMAIN_COUNT; ++i) {
        if (key == DOMAINS[i].key) return i;
    }
    return -1;
}

double round1(double x) { return std::round(x * 10) / 10; }

std::vector<std::string> splitOn(const std::string& s, char sep) {
    std::vector<std::string> out;
    std::string cur;
    for (char c : s) {
        if (c == sep) { out.push_back(cur); cur.clear(); }
        else cur += c;
    }
    out.push_back(cur);
    return out;
}

int intInRange(const std::string& raw, int lo, int hi, const std::string& what) {
    std::string s = trim(raw);
    if (!isDigits(s) || s.size() > 4) throw HttpError(400, what + " must be a whole number.");
    int v = std::stoi(s);
    if (v < lo || v > hi) throw HttpError(400, what + " must be between " + std::to_string(lo) + " and " + std::to_string(hi) + ".");
    return v;
}

std::string boundedText(const std::string& raw, std::size_t lo, std::size_t hi, const std::string& what) {
    std::string s = trim(raw);
    if (s.size() < lo || s.size() > hi) {
        if (lo == 0) throw HttpError(400, what + " can be at most " + std::to_string(hi) + " characters.");
        throw HttpError(400, what + " must be " + std::to_string(lo) + " to " + std::to_string(hi) + " characters.");
    }
    return s;
}

match::SlotMask parseSlots(const std::string& raw) {
    match::SlotMask mask = 0;
    if (trim(raw).empty()) return mask;
    for (const auto& part : splitOn(raw, ',')) {
        int slot = intInRange(part, 0, match::SLOTS - 1, "Time slot");
        mask |= 1u << slot;
    }
    return mask;
}

unsigned parseDomains(const std::string& raw) {
    unsigned bits = 0;
    if (trim(raw).empty()) return bits;
    for (const auto& part : splitOn(raw, ',')) {
        int i = domainIndex(trim(part));
        if (i < 0) throw HttpError(400, "Unknown domain: " + trim(part));
        bits |= 1u << i;
    }
    return bits;
}

// "  machine   learning " -> "machine learning". ':' and ',' are field separators, so they're not allowed.
std::string cleanSkillName(const std::string& raw) {
    std::string out;
    for (char c : trim(raw)) {
        if (static_cast<unsigned char>(c) < 0x20) throw HttpError(400, "Skill names can't contain control characters.");
        if (c == ' ' && !out.empty() && out.back() == ' ') continue;
        out += c;
    }
    if (out.empty()) throw HttpError(400, "A skill name is empty.");
    if (out.size() > MAX_SKILL_NAME) throw HttpError(400, "Skill names can be at most 40 characters.");
    return out;
}

Json slotList(match::SlotMask mask) {
    Json a = Json::array();
    for (int i = 0; i < match::SLOTS; ++i) {
        if (mask & (1u << i)) a.push(i);
    }
    return a;
}

std::string slotLabel(int slot) { return std::string(DAY_NAMES[slot / 3]) + " " + PERIOD_NAMES[slot % 3]; }

Json domainList(unsigned bits) {
    Json a = Json::array();
    for (int i = 0; i < DOMAIN_COUNT; ++i) {
        if (bits & (1u << i)) a.push(DOMAINS[i].key);
    }
    return a;
}

// "a", "a and b", "a, b and c"
std::string joinAnd(const std::vector<std::string>& items) {
    std::string out;
    for (std::size_t i = 0; i < items.size(); ++i) {
        if (i > 0) out += (i + 1 == items.size()) ? " and " : ", ";
        out += items[i];
    }
    return out;
}

const char* statusLabel(const std::string& s) {
    if (s == "recruiting") return "recruiting";
    if (s == "team_complete") return "team complete";
    if (s == "in_progress") return "in progress";
    if (s == "completed") return "completed";
    return "cancelled";
}

}  // namespace

PartnerService::PartnerService(Database& db) : db_(db) {}

/* ---------- Helpers ---------- */

int PartnerService::skillIdFor(const std::string& name) {
    std::string key = toLowerAscii(name);
    auto lock = db_.lock();
    auto st = db_.prepare("SELECT id FROM skills WHERE name_key = ?");
    st.bind(1, key);
    if (st.step()) return static_cast<int>(st.getInt(0));
    db_.prepare("INSERT INTO skills (name, name_key) VALUES (?, ?)").bind(1, name).bind(2, key).run();
    return static_cast<int>(db_.lastInsertId());
}

bool PartnerService::hasProfile(long long userId) {
    auto lock = db_.lock();
    auto st = db_.prepare("SELECT 1 FROM partner_profiles WHERE user_id = ?");
    st.bind(1, userId);
    return st.step();
}

int PartnerService::activeProjectCount(long long userId) {
    auto lock = db_.lock();
    auto st = db_.prepare(std::string("SELECT COUNT(*) FROM project_members m JOIN projects p ON p.id = m.project_id "
                                      "WHERE m.user_id = ? AND p.status IN ") + ACTIVE_STATUSES);
    st.bind(1, userId);
    st.step();
    return static_cast<int>(st.getInt(0));
}

std::vector<long long> PartnerService::memberIds(long long projectId) {
    auto lock = db_.lock();
    auto st = db_.prepare("SELECT user_id FROM project_members WHERE project_id = ? ORDER BY joined_at, user_id");
    st.bind(1, projectId);
    std::vector<long long> ids;
    while (st.step()) ids.push_back(st.getInt(0));
    return ids;
}

std::vector<match::Profile> PartnerService::loadProfiles(std::vector<StudentInfo>& info, long long userId) {
    std::vector<match::Profile> profiles;
    info.clear();
    std::unordered_map<long long, std::size_t> index;  // user id -> position
    auto lock = db_.lock();
    {
        auto st = db_.prepare(
            "SELECT p.user_id, u.name, p.department, p.semester, p.experience, p.interests, p.availability, p.bio "
            "FROM partner_profiles p JOIN users u ON u.id = p.user_id WHERE (?1 = 0 OR p.user_id = ?1) ORDER BY p.user_id");
        st.bind(1, userId);
        while (st.step()) {
            match::Profile p;
            p.userId = st.getInt(0);
            p.experience = static_cast<int>(st.getInt(4));
            p.interests = static_cast<unsigned>(st.getInt(5));
            p.availability = static_cast<match::SlotMask>(st.getInt(6));
            index[p.userId] = profiles.size();
            profiles.push_back(std::move(p));
            info.push_back(StudentInfo{st.getText(1), st.getText(2), st.getText(7), static_cast<int>(st.getInt(3)), {}});
        }
    }
    {
        auto st = db_.prepare(
            "SELECT ss.user_id, ss.skill_id, ss.level, s.name FROM student_skills ss JOIN skills s ON s.id = ss.skill_id "
            "WHERE (?1 = 0 OR ss.user_id = ?1) ORDER BY ss.level DESC, s.name");
        st.bind(1, userId);
        while (st.step()) {
            auto it = index.find(st.getInt(0));
            if (it == index.end()) continue;
            int level = static_cast<int>(st.getInt(2));
            profiles[it->second].skills[static_cast<int>(st.getInt(1))] = level;
            info[it->second].skills.emplace_back(st.getText(3), level);
        }
    }
    {
        auto st = db_.prepare(std::string("SELECT m.user_id, COUNT(*) FROM project_members m JOIN projects p ON p.id = m.project_id "
                                          "WHERE p.status IN ") + ACTIVE_STATUSES + " GROUP BY m.user_id");
        while (st.step()) {
            auto it = index.find(st.getInt(0));
            if (it != index.end()) profiles[it->second].activeProjects = static_cast<int>(st.getInt(1));
        }
    }
    return profiles;
}

static Json studentCard(const match::Profile& p, const PartnerService::StudentInfo& s) {
    Json skills = Json::array();
    for (const auto& [name, level] : s.skills) skills.push(Json::object({{"name", name}, {"level", level}}));
    return Json::object({
        {"id", p.userId}, {"name", s.name}, {"department", s.department}, {"semester", s.semester},
        {"experience", p.experience}, {"experienceLabel", LEVEL_NAMES[p.experience]},
        {"interests", domainList(p.interests)}, {"availability", slotList(p.availability)},
        {"skills", skills}, {"activeProjects", p.activeProjects},
    });
}

PartnerService::ProjectRow PartnerService::loadProject(long long id) {
    auto lock = db_.lock();
    ProjectRow p;
    {
        auto st = db_.prepare(
            "SELECT p.id, p.creator_id, u.name, p.title, p.description, p.domain, p.team_size, p.duration_weeks, "
            "p.experience, p.availability, p.schedule_required, p.extra, p.status, p.created_at "
            "FROM projects p JOIN users u ON u.id = p.creator_id WHERE p.id = ?");
        st.bind(1, id);
        if (!st.step()) throw HttpError(404, "Project not found.");
        p.id = st.getInt(0);
        p.creatorId = st.getInt(1);
        p.creatorName = st.getText(2);
        p.title = st.getText(3);
        p.description = st.getText(4);
        p.domain = st.getText(5);
        p.teamSize = static_cast<int>(st.getInt(6));
        p.durationWeeks = static_cast<int>(st.getInt(7));
        p.experience = static_cast<int>(st.getInt(8));
        p.availability = static_cast<unsigned>(st.getInt(9));
        p.scheduleRequired = st.getInt(10) != 0;
        p.extra = st.getText(11);
        p.status = st.getText(12);
        p.createdAt = st.getInt(13);
    }
    auto st = db_.prepare(
        "SELECT ps.skill_id, s.name, ps.required_level, ps.importance, ps.mandatory FROM project_skills ps "
        "JOIN skills s ON s.id = ps.skill_id WHERE ps.project_id = ? ORDER BY ps.importance DESC, s.name");
    st.bind(1, id);
    while (st.step()) {
        p.skills.push_back(match::SkillReq{static_cast<int>(st.getInt(0)), static_cast<int>(st.getInt(2)),
                                           static_cast<int>(st.getInt(3)), st.getInt(4) != 0});
        p.skillNames.push_back(st.getText(1));
    }
    return p;
}

static match::ProjectReq toRequirements(const PartnerService::ProjectRow& p) {
    match::ProjectReq r;
    r.skills = p.skills;
    r.experience = p.experience;
    int d = domainIndex(p.domain);
    r.domainBit = d >= 0 ? 1u << d : 0;
    r.availability = p.availability;
    r.scheduleRequired = p.scheduleRequired;
    return r;
}

Json PartnerService::membersJson(const ProjectRow& p, std::vector<bool>* covered) {
    auto lock = db_.lock();
    std::vector<StudentInfo> info;
    std::vector<match::Profile> profiles = loadProfiles(info);
    std::unordered_map<long long, std::size_t> byId;
    for (std::size_t i = 0; i < profiles.size(); ++i) byId[profiles[i].userId] = i;

    if (covered) covered->assign(p.skills.size(), false);
    Json out = Json::array();
    auto st = db_.prepare(
        "SELECT m.user_id, u.name, m.role, m.joined_at FROM project_members m JOIN users u ON u.id = m.user_id "
        "WHERE m.project_id = ? ORDER BY m.role = 'member', m.joined_at, m.user_id");
    st.bind(1, p.id);
    while (st.step()) {
        long long uid = st.getInt(0);
        Json skills = Json::array();
        auto it = byId.find(uid);
        if (it != byId.end()) {
            for (const auto& [name, level] : info[it->second].skills) skills.push(Json::object({{"name", name}, {"level", level}}));
            if (covered) {
                for (std::size_t i = 0; i < p.skills.size(); ++i) {
                    if (profiles[it->second].skills.count(p.skills[i].skillId)) (*covered)[i] = true;
                }
            }
        }
        out.push(Json::object({{"id", uid}, {"name", st.getText(1)}, {"role", st.getText(2)},
                               {"joinedAt", st.getInt(3)}, {"skills", skills}, {"hasProfile", it != byId.end()}}));
    }
    return out;
}

Json PartnerService::projectJson(const ProjectRow& p, long long viewerId) {
    std::vector<bool> covered;
    Json members = membersJson(p, &covered);
    Json skills = Json::array();
    for (std::size_t i = 0; i < p.skills.size(); ++i) {
        const auto& s = p.skills[i];
        skills.push(Json::object({{"name", p.skillNames[i]}, {"level", s.level}, {"levelLabel", LEVEL_NAMES[s.level]},
                                  {"importance", s.importance}, {"mandatory", s.mandatory}, {"covered", static_cast<bool>(covered[i])}}));
    }
    std::vector<long long> ids = memberIds(p.id);
    bool isMember = std::find(ids.begin(), ids.end(), viewerId) != ids.end();
    int d = domainIndex(p.domain);
    return Json::object({
        {"id", p.id}, {"title", p.title}, {"description", p.description}, {"domain", p.domain},
        {"domainLabel", d >= 0 ? DOMAINS[d].label : p.domain}, {"teamSize", p.teamSize}, {"durationWeeks", p.durationWeeks},
        {"experience", p.experience}, {"experienceLabel", LEVEL_NAMES[p.experience]},
        {"availability", slotList(p.availability)}, {"scheduleRequired", p.scheduleRequired}, {"extra", p.extra},
        {"status", p.status}, {"createdAt", p.createdAt},
        {"creator", Json::object({{"id", p.creatorId}, {"name", p.creatorName}})},
        {"skills", skills}, {"members", members}, {"memberCount", ids.size()},
        {"spotsLeft", std::max(0, p.teamSize - static_cast<int>(ids.size()))},
        {"isCreator", p.creatorId == viewerId}, {"isMember", isMember},
    });
}

Json PartnerService::invitationJson(long long id) {
    auto lock = db_.lock();
    auto st = db_.prepare(
        "SELECT i.id, i.kind, i.status, i.message, i.created_at, i.responded_at, p.id, p.title, p.status, "
        "p.creator_id, cu.name, i.student_id, su.name FROM invitations i JOIN projects p ON p.id = i.project_id "
        "JOIN users cu ON cu.id = p.creator_id JOIN users su ON su.id = i.student_id WHERE i.id = ?");
    st.bind(1, id);
    if (!st.step()) throw HttpError(404, "Invitation not found.");
    return Json::object({
        {"id", st.getInt(0)}, {"kind", st.getText(1)}, {"status", st.getText(2)}, {"message", st.getText(3)},
        {"createdAt", st.getInt(4)}, {"respondedAt", st.isNull(5) ? Json() : Json(st.getInt(5))},
        {"project", Json::object({{"id", st.getInt(6)}, {"title", st.getText(7)}, {"status", st.getText(8)}})},
        {"creator", Json::object({{"id", st.getInt(9)}, {"name", st.getText(10)}})},
        {"student", Json::object({{"id", st.getInt(11)}, {"name", st.getText(12)}})},
    });
}

void PartnerService::closePendingIfNotRecruiting(long long projectId) {
    auto lock = db_.lock();
    db_.prepare("UPDATE invitations SET status = 'withdrawn', responded_at = ?1 WHERE project_id = ?2 AND status = 'pending' "
                "AND (SELECT status FROM projects WHERE id = ?2) <> 'recruiting'")
        .bind(1, nowMs()).bind(2, projectId).run();
}

/* ---------- Meta + profiles ---------- */

Json PartnerService::meta() {
    Json domains = Json::array();
    for (const Domain& d : DOMAINS) domains.push(Json::object({{"key", d.key}, {"label", d.label}}));
    Json days = Json::array(), periods = Json::array(), levels = Json::array();
    for (const char* d : DAY_NAMES) days.push(d);
    for (const char* p : PERIOD_NAMES) periods.push(p);
    for (int i = 1; i <= 3; ++i) levels.push(Json::object({{"value", i}, {"label", LEVEL_NAMES[i]}}));
    Json skills = Json::array();
    {
        auto lock = db_.lock();
        auto st = db_.prepare("SELECT name FROM skills ORDER BY name_key LIMIT 500");
        while (st.step()) skills.push(st.getText(0));
    }
    match::Weights w;
    return Json::object({
        {"domains", domains}, {"days", days}, {"periods", periods}, {"levels", levels}, {"skills", skills},
        {"maxActiveProjects", MAX_ACTIVE_PROJECTS},
        {"weights", Json::object({{"skill", w.skill}, {"availability", w.availability}, {"experience", w.experience}, {"interest", w.interest}})},
    });
}

Json PartnerService::myProfile(const User& u) {
    std::vector<StudentInfo> info;
    auto profiles = loadProfiles(info, u.id);
    if (profiles.empty()) return Json::object({{"profile", nullptr}, {"canHaveProfile", u.role == "student"}});
    Json card = studentCard(profiles[0], info[0]);
    card.set("bio", info[0].bio);
    return Json::object({{"profile", card}, {"canHaveProfile", true}});
}

Json PartnerService::saveProfile(const User& u, const ProfileInput& in) {
    if (u.role != "student") throw HttpError(403, "Partner profiles are for students.");
    std::string department = boundedText(in.department, 2, 60, "Department");
    int semester = intInRange(in.semester, 1, 10, "Semester");
    int experience = intInRange(in.experience, 1, 3, "Experience level");
    unsigned interests = parseDomains(in.interests);
    match::SlotMask availability = parseSlots(in.availability);
    std::string bio = boundedText(in.bio, 0, 500, "About you");

    std::vector<std::pair<std::string, int>> skills;
    std::unordered_set<std::string> seen;  // case-insensitive duplicate check
    if (!trim(in.skills).empty()) {
        for (const auto& part : splitOn(in.skills, ',')) {
            auto fields = splitOn(part, ':');
            if (fields.size() != 2) throw HttpError(400, "Each skill needs a name and a level.");
            std::string name = cleanSkillName(fields[0]);
            if (!seen.insert(toLowerAscii(name)).second) throw HttpError(400, name + " is listed twice.");
            skills.emplace_back(name, intInRange(fields[1], 1, 3, "Skill level"));
        }
    }
    if (skills.empty()) throw HttpError(400, "Add at least one skill, so projects can find you.");
    if (skills.size() > MAX_SKILLS_PROFILE) throw HttpError(400, "List at most 25 skills.");

    auto lock = db_.lock();
    Transaction tx(db_);
    db_.prepare(
        "INSERT INTO partner_profiles (user_id, department, semester, experience, interests, availability, bio, updated_at) "
        "VALUES (?1, ?2, ?3, ?4, ?5, ?6, ?7, ?8) ON CONFLICT (user_id) DO UPDATE SET department = ?2, semester = ?3, "
        "experience = ?4, interests = ?5, availability = ?6, bio = ?7, updated_at = ?8")
        .bind(1, u.id).bind(2, department).bind(3, semester).bind(4, experience)
        .bind(5, static_cast<long long>(interests)).bind(6, static_cast<long long>(availability)).bind(7, bio).bind(8, nowMs())
        .run();
    db_.prepare("DELETE FROM student_skills WHERE user_id = ?").bind(1, u.id).run();
    auto st = db_.prepare("INSERT INTO student_skills (user_id, skill_id, level) VALUES (?, ?, ?)");
    for (const auto& [name, level] : skills) {
        int sid = skillIdFor(name);
        st.reset();
        st.bind(1, u.id).bind(2, sid).bind(3, level).run();
    }
    tx.commit();
    return myProfile(u);
}

void PartnerService::deleteProfile(const User& u) {
    auto lock = db_.lock();
    Transaction tx(db_);
    db_.prepare("DELETE FROM student_skills WHERE user_id = ?").bind(1, u.id).run();
    db_.prepare("DELETE FROM partner_profiles WHERE user_id = ?").bind(1, u.id).run();
    // Nobody can accept on their behalf any more.
    db_.prepare("UPDATE invitations SET status = 'withdrawn', responded_at = ? WHERE student_id = ? AND status = 'pending'")
        .bind(1, nowMs()).bind(2, u.id).run();
    tx.commit();
}

Json PartnerService::studentProfile(long long userId) {
    std::vector<StudentInfo> info;
    auto profiles = loadProfiles(info, userId);
    if (profiles.empty()) throw HttpError(404, "This student has no partner profile.");
    Json card = studentCard(profiles[0], info[0]);
    card.set("bio", info[0].bio);
    return Json::object({{"profile", card}});
}

/* ---------- Projects ---------- */

Json PartnerService::createProject(const User& creator, const ProjectInput& in) {
    std::string title = boundedText(in.title, 4, 100, "Project name");
    std::string description = boundedText(in.description, 10, 1000, "Description");
    std::string domain = trim(in.domain);
    if (domainIndex(domain) < 0) throw HttpError(400, "Choose a project domain.");
    int teamSize = intInRange(in.teamSize, 2, 8, "Team size");
    int weeks = intInRange(in.durationWeeks, 1, 52, "Duration (weeks)");
    int experience = intInRange(in.experience, 1, 3, "Experience level");
    match::SlotMask availability = parseSlots(in.availability);
    std::string req = trim(in.scheduleRequired);
    bool scheduleRequired = req == "1" || req == "true" || req == "on";
    if (scheduleRequired && !availability) throw HttpError(400, "Pick at least one time slot to make the schedule mandatory.");
    std::string extra = boundedText(in.extra, 0, 500, "Additional requirements");

    struct Req { std::string name; int level, importance; bool mandatory; };
    std::vector<Req> skills;
    std::unordered_set<std::string> seen;
    if (!trim(in.skills).empty()) {
        for (const auto& part : splitOn(in.skills, ',')) {
            auto f = splitOn(part, ':');
            if (f.size() != 4) throw HttpError(400, "Each required skill needs a name, level, importance and mandatory flag.");
            std::string name = cleanSkillName(f[0]);
            if (!seen.insert(toLowerAscii(name)).second) throw HttpError(400, name + " is listed twice.");
            skills.push_back({name, intInRange(f[1], 1, 3, "Skill level"), intInRange(f[2], 1, 5, "Importance"),
                              intInRange(f[3], 0, 1, "Mandatory flag") == 1});
        }
    }
    if (skills.empty()) throw HttpError(400, "Add at least one required skill.");
    if (skills.size() > MAX_SKILLS_PROJECT) throw HttpError(400, "List at most 12 required skills.");
    if (activeProjectCount(creator.id) >= MAX_ACTIVE_PROJECTS) {
        throw HttpError(409, "You're already in " + std::to_string(MAX_ACTIVE_PROJECTS) + " active projects. Finish or leave one first.");
    }

    long long now = nowMs();
    auto lock = db_.lock();
    Transaction tx(db_);
    db_.prepare(
        "INSERT INTO projects (creator_id, title, description, domain, team_size, duration_weeks, experience, availability, "
        "schedule_required, extra, created_at) VALUES (?,?,?,?,?,?,?,?,?,?,?)")
        .bind(1, creator.id).bind(2, title).bind(3, description).bind(4, domain).bind(5, teamSize).bind(6, weeks)
        .bind(7, experience).bind(8, static_cast<long long>(availability)).bind(9, scheduleRequired ? 1 : 0)
        .bind(10, extra).bind(11, now).run();
    long long id = db_.lastInsertId();
    auto st = db_.prepare("INSERT INTO project_skills (project_id, skill_id, required_level, importance, mandatory) VALUES (?,?,?,?,?)");
    for (const Req& r : skills) {
        int sid = skillIdFor(r.name);
        st.reset();
        st.bind(1, id).bind(2, sid).bind(3, r.level).bind(4, r.importance).bind(5, r.mandatory ? 1 : 0).run();
    }
    db_.prepare("INSERT INTO project_members (project_id, user_id, role, joined_at) VALUES (?, ?, 'creator', ?)")
        .bind(1, id).bind(2, creator.id).bind(3, now).run();
    tx.commit();
    return projectJson(loadProject(id), creator.id);
}

Json PartnerService::listProjects(const User& u, const std::string& scope) {
    if (scope != "mine" && scope != "open") throw HttpError(400, "scope must be mine or open.");
    std::vector<long long> ids;
    {
        auto lock = db_.lock();
        std::string sql = scope == "mine"
            ? "SELECT p.id FROM projects p JOIN project_members m ON m.project_id = p.id AND m.user_id = ?1 "
              "ORDER BY p.status IN ('completed','cancelled'), p.created_at DESC LIMIT 50"
            : "SELECT p.id FROM projects p WHERE p.status = 'recruiting' AND NOT EXISTS "
              "(SELECT 1 FROM project_members m WHERE m.project_id = p.id AND m.user_id = ?1) ORDER BY p.created_at DESC LIMIT 50";
        auto st = db_.prepare(sql);
        st.bind(1, u.id);
        while (st.step()) ids.push_back(st.getInt(0));
    }

    // For open projects, show how well each one fits ME (the same scorer, run the other way round).
    std::vector<StudentInfo> info;
    std::vector<match::Profile> mine = scope == "open" ? loadProfiles(info, u.id) : std::vector<match::Profile>();
    std::unordered_map<long long, std::pair<std::string, std::string>> myInvites;  // project -> kind, status
    if (scope == "open") {
        auto lock = db_.lock();
        auto st = db_.prepare("SELECT project_id, kind, status FROM invitations WHERE student_id = ? ORDER BY created_at");
        st.bind(1, u.id);
        while (st.step()) myInvites[st.getInt(0)] = {st.getText(1), st.getText(2)};  // latest wins
    }

    struct Item { Json json; double fit; long long createdAt; };
    std::vector<Item> items;
    for (long long id : ids) {
        ProjectRow p = loadProject(id);
        Json j = projectJson(p, u.id);
        double fit = -1;
        if (!mine.empty()) {
            match::Scores s = match::score(toRequirements(p), mine[0]);
            fit = round1(s.total);
            j.set("myFit", fit);
        }
        auto inv = myInvites.find(id);
        j.set("myInvitation", inv == myInvites.end() ? Json()
                              : Json::object({{"kind", inv->second.first}, {"status", inv->second.second}}));
        items.push_back({j, fit, p.createdAt});
    }
    if (!mine.empty()) {
        std::stable_sort(items.begin(), items.end(), [](const Item& a, const Item& b) { return a.fit > b.fit; });
    }
    Json out = Json::array();
    for (auto& it : items) out.push(it.json);
    return Json::object({{"projects", out}, {"hasProfile", scope == "open" ? Json(!mine.empty()) : Json(hasProfile(u.id))}});
}

Json PartnerService::project(const User& u, long long projectId) {
    ProjectRow p = loadProject(projectId);
    Json j = projectJson(p, u.id);
    Json pending = Json::array();
    if (p.creatorId == u.id) {
        auto lock = db_.lock();
        auto st = db_.prepare("SELECT id FROM invitations WHERE project_id = ? ORDER BY created_at DESC LIMIT 100");
        st.bind(1, projectId);
        std::vector<long long> invIds;
        while (st.step()) invIds.push_back(st.getInt(0));
        for (long long id : invIds) pending.push(invitationJson(id));
    }
    j.set("invitations", pending);
    return Json::object({{"project", j}});
}

Json PartnerService::setStatus(const User& u, long long projectId, const std::string& status) {
    ProjectRow p = loadProject(projectId);
    if (p.creatorId != u.id) throw HttpError(403, "Only the project creator can change its status.");
    if (status != "in_progress" && status != "completed" && status != "cancelled" && status != "recruiting") {
        throw HttpError(400, "status must be in_progress, completed, cancelled or recruiting.");
    }
    bool ok = (status == "in_progress" && (p.status == "recruiting" || p.status == "team_complete")) ||
              (status == "completed" && p.status == "in_progress") ||
              (status == "cancelled" && p.status != "completed" && p.status != "cancelled") ||
              (status == "recruiting" && p.status == "team_complete" &&
               static_cast<int>(memberIds(projectId).size()) < p.teamSize);
    if (!ok) throw HttpError(409, std::string("A project that is ") + statusLabel(p.status) + " can't become " + statusLabel(status) + ".");
    {
        auto lock = db_.lock();
        db_.prepare("UPDATE projects SET status = ? WHERE id = ?").bind(1, status).bind(2, projectId).run();
    }
    closePendingIfNotRecruiting(projectId);
    return projectJson(loadProject(projectId), u.id);
}

/* ---------- Matching ---------- */

Json PartnerService::matches(const User& u, long long projectId, int limit) {
    limit = std::clamp(limit, 1, 50);
    ProjectRow p = loadProject(projectId);
    if (p.creatorId != u.id) throw HttpError(403, "Only the project creator can see recommendations.");
    if (p.status != "recruiting") throw HttpError(409, std::string("This project is ") + statusLabel(p.status) + ", so it isn't recruiting.");

    match::ProjectReq req = toRequirements(p);
    std::vector<StudentInfo> info;
    std::vector<match::Profile> profiles = loadProfiles(info);
    std::vector<long long> members = memberIds(projectId);
    std::unordered_set<long long> exclude(members.begin(), members.end());
    exclude.insert(p.creatorId);

    // Latest invitation per student for this project: shown on the card, and a "no" is respected.
    std::unordered_map<long long, Json> invites;
    std::unordered_set<long long> declined;
    {
        auto lock = db_.lock();
        auto st = db_.prepare("SELECT id, student_id, kind, status FROM invitations WHERE project_id = ? ORDER BY created_at, id");
        st.bind(1, projectId);
        while (st.step()) {
            long long sid = st.getInt(1);
            std::string kind = st.getText(2), status = st.getText(3);
            invites[sid] = Json::object({{"id", st.getInt(0)}, {"kind", kind}, {"status", status}});
            if (kind == "invite" && status == "rejected") declined.insert(sid);
            else declined.erase(sid);
        }
    }

    match::RankResult ranked = match::rank(req, profiles, exclude, MAX_ACTIVE_PROJECTS);

    // Skills the team already has (creator + accepted members).
    std::vector<bool> covered;
    Json memberList = membersJson(p, &covered);
    std::vector<bool> coveredBefore = covered;

    auto names = [&](const std::vector<int>& idx) {
        Json a = Json::array();
        for (int i : idx) a.push(p.skillNames[i]);
        return a;
    };

    auto reasonFor = [&](const match::Candidate& c) {
        const match::Profile& s = profiles[c.profile];
        std::vector<std::string> parts, skillNames;
        for (int i : c.scores.matched) skillNames.push_back(p.skillNames[i]);
        if (!skillNames.empty()) parts.push_back("has " + joinAnd(skillNames));
        int wanted = match::countBits(req.availability), shared = match::countBits(c.scores.sharedSlots);
        if (wanted > 0) {
            std::vector<std::string> slots;
            for (int i = 0; i < match::SLOTS && slots.size() < 3; ++i) {
                if (c.scores.sharedSlots & (1u << i)) slots.push_back(slotLabel(i));
            }
            if (shared == wanted) parts.push_back("is free at all your preferred times");
            else if (shared > 0) parts.push_back("shares " + std::to_string(shared) + " of your " + std::to_string(wanted) +
                                                 " time slots (" + joinAnd(slots) + (shared > 3 ? ", …" : "") + ")");
        }
        if (c.scores.experience >= 100) parts.push_back(std::string("has ") + LEVEL_NAMES[s.experience] + " experience");
        if (c.scores.interest >= 100) parts.push_back("is interested in " + std::string(DOMAINS[std::max(0, domainIndex(p.domain))].label));
        std::string text = "Recommended because this student " + joinAnd(parts) + ".";
        std::vector<std::string> gaps;
        if (wanted > 0 && shared == 0) gaps.push_back("no shared time slots");
        if (c.scores.experience < 100) gaps.push_back(std::string("experience is ") + LEVEL_NAMES[s.experience] +
                                                      " (you asked for " + LEVEL_NAMES[p.experience] + ")");
        if (!gaps.empty()) text += " Note: " + joinAnd(gaps) + ".";
        return text;
    };

    auto candidateJson = [&](const match::Candidate& c) {
        const match::Profile& s = profiles[c.profile];
        std::vector<int> missing;
        for (std::size_t i = 0; i < p.skills.size(); ++i) {
            if (std::find(c.scores.matched.begin(), c.scores.matched.end(), static_cast<int>(i)) == c.scores.matched.end()) {
                missing.push_back(static_cast<int>(i));
            }
        }
        auto inv = invites.find(s.userId);
        return Json::object({
            {"student", studentCard(s, info[c.profile])},
            {"score", Json::object({{"total", round1(c.scores.total)}, {"skill", round1(c.scores.skill)},
                                    {"availability", round1(c.scores.availability)}, {"experience", round1(c.scores.experience)},
                                    {"interest", round1(c.scores.interest)}})},
            {"matchedSkills", names(c.scores.matched)}, {"missingSkills", names(missing)},
            {"sharedSlots", slotList(c.scores.sharedSlots)}, {"reason", reasonFor(c)},
            {"invitation", inv == invites.end() ? Json() : inv->second},
            {"declined", declined.count(s.userId) > 0},
        });
    };

    Json candidates = Json::array();
    for (std::size_t i = 0; i < ranked.ranked.size() && static_cast<int>(i) < limit; ++i) {
        candidates.push(candidateJson(ranked.ranked[i]));
    }

    // Greedy balanced team over everyone eligible who hasn't declined.
    std::vector<match::Candidate> pool;
    for (const auto& c : ranked.ranked) {
        if (!declined.count(profiles[c.profile].userId)) pool.push_back(c);
    }
    int slots = std::max(0, p.teamSize - static_cast<int>(members.size()));
    std::vector<match::TeamPick> picks = match::formTeam(req, profiles, pool, covered, slots);
    Json team = Json::array();
    for (const auto& pick : picks) {
        Json j = candidateJson(pool[pick.candidate]);
        j.set("covers", names(pick.newlyCovered));
        team.push(j);
    }

    std::vector<int> before, after, missing;
    for (std::size_t i = 0; i < p.skills.size(); ++i) {
        if (coveredBefore[i]) before.push_back(static_cast<int>(i));
        if (covered[i]) after.push_back(static_cast<int>(i));
        else missing.push_back(static_cast<int>(i));
    }

    // Mandatory skills that nobody outside the team has: no amount of ranking can fix that.
    Json warnings = Json::array();
    for (std::size_t i = 0; i < p.skills.size(); ++i) {
        if (!p.skills[i].mandatory) continue;
        bool anyone = false;
        for (const auto& prof : profiles) {
            if (!exclude.count(prof.userId) && prof.skills.count(p.skills[i].skillId)) { anyone = true; break; }
        }
        if (!anyone) warnings.push("No student with a profile has the mandatory skill " + p.skillNames[i] +
                                   ". Make it optional, or ask around and have them create a profile.");
    }
    if (ranked.ranked.empty() && warnings.size() == 0) warnings.push("No suitable students found yet. Try fewer mandatory skills or a flexible schedule.");

    match::Weights w;
    return Json::object({
        {"project", projectJson(p, u.id)},
        {"candidates", candidates}, {"eligible", ranked.ranked.size()}, {"considered", ranked.considered},
        {"totalProfiles", profiles.size()},
        {"excluded", Json::object({{"noSkillOverlap", ranked.noSkillOverlap}, {"missingMandatory", ranked.missingMandatory},
                                   {"noSharedTime", ranked.noSharedTime}, {"tooManyProjects", ranked.tooManyProjects}})},
        {"suggestedTeam", team}, {"spotsLeft", slots},
        {"coverage", Json::object({{"before", names(before)}, {"after", names(after)}, {"missing", names(missing)}})},
        {"weights", Json::object({{"skill", w.skill}, {"availability", w.availability}, {"experience", w.experience}, {"interest", w.interest}})},
        {"warnings", warnings},
    });
}

/* ---------- Invitations ---------- */

Json PartnerService::invite(const User& creator, long long projectId, long long studentId, const std::string& messageIn) {
    std::string message = boundedText(messageIn, 0, 300, "Message");
    ProjectRow p = loadProject(projectId);
    if (p.creatorId != creator.id) throw HttpError(403, "Only the project creator can send invitations.");
    if (p.status != "recruiting") throw HttpError(409, "This project isn't recruiting.");
    if (studentId == creator.id) throw HttpError(400, "You're already on this team.");
    if (!hasProfile(studentId)) throw HttpError(404, "This student has no partner profile.");

    auto lock = db_.lock();
    Transaction tx(db_);
    std::vector<long long> members = memberIds(projectId);
    if (std::find(members.begin(), members.end(), studentId) != members.end()) throw HttpError(409, "This student is already on the team.");
    if (static_cast<int>(members.size()) >= p.teamSize) throw HttpError(409, "The team is full.");
    {
        auto st = db_.prepare("SELECT kind, status FROM invitations WHERE project_id = ? AND student_id = ? ORDER BY created_at DESC, id DESC LIMIT 1");
        st.bind(1, projectId).bind(2, studentId);
        if (st.step()) {
            std::string kind = st.getText(0), status = st.getText(1);
            if (status == "pending" && kind == "request") throw HttpError(409, "This student already asked to join. Accept their request instead.");
            if (status == "pending") throw HttpError(409, "You already invited this student.");
            if (kind == "invite" && status == "rejected") throw HttpError(409, "This student declined your invitation.");
        }
    }
    {
        auto st = db_.prepare("SELECT COUNT(*) FROM invitations WHERE project_id = ? AND status = 'pending' AND kind = 'invite'");
        st.bind(1, projectId);
        st.step();
        if (st.getInt(0) >= MAX_PENDING_INVITES) {
            throw HttpError(409, "You have " + std::to_string(MAX_PENDING_INVITES) + " invitations waiting. Wait for replies or withdraw some.");
        }
    }
    db_.prepare("INSERT INTO invitations (project_id, student_id, kind, message, created_at) VALUES (?, ?, 'invite', ?, ?)")
        .bind(1, projectId).bind(2, studentId).bind(3, message).bind(4, nowMs()).run();
    long long id = db_.lastInsertId();
    tx.commit();
    return invitationJson(id);
}

Json PartnerService::requestToJoin(const User& student, long long projectId, const std::string& messageIn) {
    std::string message = boundedText(messageIn, 0, 300, "Message");
    ProjectRow p = loadProject(projectId);
    if (p.creatorId == student.id) throw HttpError(400, "This is your own project.");
    if (!hasProfile(student.id)) throw HttpError(400, "Create your partner profile first, so the creator can see your skills.");
    if (p.status != "recruiting") throw HttpError(409, "This project isn't recruiting.");
    if (activeProjectCount(student.id) >= MAX_ACTIVE_PROJECTS) {
        throw HttpError(409, "You're already in " + std::to_string(MAX_ACTIVE_PROJECTS) + " active projects.");
    }

    auto lock = db_.lock();
    Transaction tx(db_);
    std::vector<long long> members = memberIds(projectId);
    if (std::find(members.begin(), members.end(), student.id) != members.end()) throw HttpError(409, "You're already on this team.");
    if (static_cast<int>(members.size()) >= p.teamSize) throw HttpError(409, "The team is full.");
    {
        auto st = db_.prepare("SELECT kind, status FROM invitations WHERE project_id = ? AND student_id = ? ORDER BY created_at DESC, id DESC LIMIT 1");
        st.bind(1, projectId).bind(2, student.id);
        if (st.step()) {
            std::string kind = st.getText(0), status = st.getText(1);
            if (status == "pending" && kind == "invite") throw HttpError(409, "You're already invited. Accept it under Invitations.");
            if (status == "pending") throw HttpError(409, "You already asked to join.");
            if (kind == "request" && status == "rejected") throw HttpError(409, "The creator declined your request for this project.");
        }
    }
    db_.prepare("INSERT INTO invitations (project_id, student_id, kind, message, created_at) VALUES (?, ?, 'request', ?, ?)")
        .bind(1, projectId).bind(2, student.id).bind(3, message).bind(4, nowMs()).run();
    long long id = db_.lastInsertId();
    tx.commit();
    return invitationJson(id);
}

Json PartnerService::respond(const User& u, long long invitationId, const std::string& action) {
    if (action != "accept" && action != "reject" && action != "withdraw") throw HttpError(400, "action must be accept, reject or withdraw.");
    long long projectId = 0, studentId = 0, creatorId = 0;
    std::string kind, status;
    {
        auto lock = db_.lock();
        auto st = db_.prepare("SELECT i.project_id, i.student_id, i.kind, i.status, p.creator_id FROM invitations i "
                              "JOIN projects p ON p.id = i.project_id WHERE i.id = ?");
        st.bind(1, invitationId);
        if (!st.step()) throw HttpError(404, "Invitation not found.");
        projectId = st.getInt(0);
        studentId = st.getInt(1);
        kind = st.getText(2);
        status = st.getText(3);
        creatorId = st.getInt(4);
    }
    // The receiver answers; the sender may withdraw.
    long long receiver = kind == "invite" ? studentId : creatorId;
    long long sender = kind == "invite" ? creatorId : studentId;
    if (action == "withdraw" ? u.id != sender : u.id != receiver) {
        throw HttpError(403, action == "withdraw" ? "Only the sender can withdraw this." : "Only the receiver can answer this.");
    }
    if (status != "pending") throw HttpError(409, "This invitation is already " + status + ".");

    auto lock = db_.lock();
    Transaction tx(db_);
    long long now = nowMs();
    if (action == "accept") {
        ProjectRow p = loadProject(projectId);
        if (p.status != "recruiting") throw HttpError(409, "This project isn't recruiting any more.");
        std::vector<long long> members = memberIds(projectId);
        if (static_cast<int>(members.size()) >= p.teamSize) throw HttpError(409, "The team is already full.");
        if (activeProjectCount(studentId) >= MAX_ACTIVE_PROJECTS) {
            throw HttpError(409, kind == "invite" ? "You're already in " + std::to_string(MAX_ACTIVE_PROJECTS) + " active projects. Leave one first."
                                                  : "This student is already in " + std::to_string(MAX_ACTIVE_PROJECTS) + " active projects.");
        }
        db_.prepare("INSERT INTO project_members (project_id, user_id, role, joined_at) VALUES (?, ?, 'member', ?)")
            .bind(1, projectId).bind(2, studentId).bind(3, now).run();
        if (static_cast<int>(members.size()) + 1 >= p.teamSize) {
            db_.prepare("UPDATE projects SET status = 'team_complete' WHERE id = ?").bind(1, projectId).run();
        }
    }
    const char* next = action == "accept" ? "accepted" : action == "reject" ? "rejected" : "withdrawn";
    db_.prepare("UPDATE invitations SET status = ?, responded_at = ? WHERE id = ?").bind(1, next).bind(2, now).bind(3, invitationId).run();
    closePendingIfNotRecruiting(projectId);  // team just filled up: nobody else can join
    tx.commit();
    return invitationJson(invitationId);
}

Json PartnerService::invitations(const User& u) {
    std::vector<long long> ids;
    {
        auto lock = db_.lock();
        auto st = db_.prepare("SELECT i.id FROM invitations i JOIN projects p ON p.id = i.project_id "
                              "WHERE i.student_id = ?1 OR p.creator_id = ?1 ORDER BY i.status <> 'pending', i.created_at DESC LIMIT 100");
        st.bind(1, u.id);
        while (st.step()) ids.push_back(st.getInt(0));
    }
    Json received = Json::array(), sent = Json::array();
    int pending = 0;
    for (long long id : ids) {
        Json j = invitationJson(id);
        auto lock = db_.lock();
        auto st = db_.prepare("SELECT i.kind, i.student_id, i.status FROM invitations i WHERE i.id = ?");
        st.bind(1, id);
        st.step();
        std::string kind = st.getText(0);
        bool toMe = (kind == "invite") == (st.getInt(1) == u.id);  // invite to me, or a request to my project
        bool isPending = st.getText(2) == "pending";
        j.set("canRespond", toMe && isPending);
        j.set("canWithdraw", !toMe && isPending);
        if (toMe) {
            if (isPending) ++pending;
            received.push(j);
        } else {
            sent.push(j);
        }
    }
    return Json::object({{"received", received}, {"sent", sent}, {"pendingReceived", pending}});
}

void PartnerService::leave(const User& u, long long projectId) {
    ProjectRow p = loadProject(projectId);
    if (p.creatorId == u.id) throw HttpError(400, "Creators can't leave their own project. Cancel it instead.");
    if (p.status == "completed" || p.status == "cancelled") throw HttpError(409, "This project has already ended.");
    auto lock = db_.lock();
    Transaction tx(db_);
    db_.prepare("DELETE FROM project_members WHERE project_id = ? AND user_id = ? AND role = 'member'").bind(1, projectId).bind(2, u.id).run();
    if (db_.changes() == 0) throw HttpError(404, "You're not on this team.");
    // A spot opened up: a full team goes back to recruiting.
    db_.prepare("UPDATE projects SET status = 'recruiting' WHERE id = ? AND status = 'team_complete'").bind(1, projectId).run();
    tx.commit();
}

}  // namespace cx
