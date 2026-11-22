#include "LearningService.h"

#include <algorithm>
#include <cmath>

#include "../Util/HttpError.h"
#include "../Util/JsonParse.h"
#include "../Util/Text.h"

namespace cx {

namespace {
const char* LEVEL_NAMES[] = {"Not in profile", "Beginner", "Intermediate", "Advanced"};
constexpr int HIGH = 3, MEDIUM = 2, LOW = 1;
constexpr std::size_t SKILLS_PER_PROMPT = 3, CANDIDATES_PER_PROMPT = 10;

double round1(double x) { return std::round(x * 10) / 10; }

Json rowJson(const learn::SkillGapRow& r) {
    return Json::object({{"skill", r.name}, {"current", r.current}, {"currentLabel", LEVEL_NAMES[r.current]},
                         {"target", r.target}, {"targetLabel", LEVEL_NAMES[r.target]}, {"gap", r.gap},
                         {"weight", r.weight}, {"priority", r.priority}, {"status", r.status}});
}
}  // namespace

// Predefined goals. Skill names match the ones students pick in their profile, so they line up.
// Targets use the app's levels (1 Beginner, 2 Intermediate, 3 Advanced); weights: high 3, medium 2, low 1.
const std::vector<LearningGoal>& LearningService::goals() {
    static const std::vector<LearningGoal> all = {
        {"ml_project", "Build a Machine Learning project", "Train and evaluate a model on a real dataset.",
         {{"Python", 2, HIGH}, {"NumPy", 2, HIGH}, {"Statistics", 2, HIGH}, {"Pandas", 2, MEDIUM},
          {"Scikit-learn", 2, MEDIUM}, {"Data Visualization", 1, LOW}}},
        {"web_dev", "Learn Web Development", "Build and deploy a full website with a backend.",
         {{"HTML", 2, HIGH}, {"CSS", 2, HIGH}, {"JavaScript", 2, HIGH}, {"Backend Development", 1, MEDIUM},
          {"SQL", 1, MEDIUM}, {"Git", 1, LOW}}},
        {"ds_internship", "Prepare for a Data Science internship", "Skills interviewers expect from a data science intern.",
         {{"Python", 3, HIGH}, {"SQL", 2, HIGH}, {"Statistics", 2, HIGH}, {"Pandas", 2, MEDIUM},
          {"Data Visualization", 2, MEDIUM}, {"Machine Learning", 1, MEDIUM}}},
        {"cybersecurity", "Explore Cybersecurity", "Foundations for security work and CTFs.",
         {{"Networking", 2, HIGH}, {"Linux", 2, HIGH}, {"Network Security", 2, HIGH}, {"Operating Systems", 1, MEDIUM},
          {"Cryptography", 1, MEDIUM}, {"Python", 1, LOW}}},
        {"android_app", "Build an Android app", "Ship a working app to your phone.",
         {{"Java", 2, HIGH}, {"Android", 2, HIGH}, {"SQL", 1, MEDIUM}, {"UI/UX Design", 1, LOW}, {"Git", 1, LOW}}},
        {"coding_interviews", "Prepare for coding interviews", "Placement-style data structures and algorithms rounds.",
         {{"Data Structures", 3, HIGH}, {"Algorithms", 3, HIGH}, {"C++", 2, MEDIUM}, {"Problem Solving", 2, MEDIUM},
          {"SQL", 1, LOW}}},
    };
    return all;
}

const LearningGoal* LearningService::findGoal(const std::string& key) {
    for (const LearningGoal& g : goals()) {
        if (g.key == key) return &g;
    }
    return nullptr;
}

LearningService::LearningService(Database& db, AskGemini ask, bool geminiConfigured)
    : db_(db), ask_(std::move(ask)), geminiConfigured_(geminiConfigured) {}

const LearningGoal& LearningService::goalOr400(const std::string& key) {
    const LearningGoal* g = findGoal(trim(key));
    if (!g) throw HttpError(400, "Choose a learning goal.");
    return *g;
}

std::unordered_map<std::string, int> LearningService::currentSkills(long long userId, Json* list) {
    std::unordered_map<std::string, int> skills;  // hash map: lower-cased name -> level
    auto lock = db_.lock();
    auto st = db_.prepare("SELECT s.name, ss.level FROM student_skills ss JOIN skills s ON s.id = ss.skill_id "
                          "WHERE ss.user_id = ? ORDER BY ss.level DESC, s.name");
    st.bind(1, userId);
    while (st.step()) {
        int level = static_cast<int>(std::clamp<long long>(st.getInt(1), 1, 3));
        skills[toLowerAscii(st.getText(0))] = level;
        if (list) list->push(Json::object({{"name", st.getText(0)}, {"level", level}, {"label", LEVEL_NAMES[level]}}));
    }
    return skills;
}

Json LearningService::goalsJson() {
    Json out = Json::array();
    for (const LearningGoal& g : goals()) {
        Json skills = Json::array();
        for (const auto& s : g.skills) skills.push(s.name);
        out.push(Json::object({{"key", g.key}, {"title", g.title}, {"description", g.description}, {"skills", skills}}));
    }
    return Json::object({{"goals", out}, {"geminiConfigured", geminiConfigured_}});
}

/* ---------- Analysis ---------- */

Json LearningService::analysis(const User& u, const std::string& goalKey) {
    const LearningGoal& goal = goalOr400(goalKey);
    Json mySkills = Json::array();
    auto current = currentSkills(u.id, &mySkills);
    learn::GapReport gaps = learn::analyse(goal.skills, current);

    Json rows = Json::array(), focus = Json::array();
    int sufficient = 0;
    for (const auto& r : gaps.rows) {
        rows.push(rowJson(r));
        sufficient += r.gap == 0;
    }
    for (const auto& r : gaps.focus) focus.push(rowJson(r));

    Json recs = Json::array();
    int saved = 0, completed = 0;
    std::unordered_map<std::string, int> doneBySkill;
    {
        auto lock = db_.lock();
        auto st = db_.prepare(
            "SELECT id, skill, title, description, level, topics, score, source, status, created_at FROM learning_recommendations "
            "WHERE user_id = ? AND goal_key = ? ORDER BY CASE status WHEN 'new' THEN 0 WHEN 'saved' THEN 1 ELSE 2 END, score DESC, id");
        st.bind(1, u.id).bind(2, goal.key);
        while (st.step()) {
            std::string status = st.getText(8);
            saved += status == "saved";
            if (status == "completed") {
                ++completed;
                ++doneBySkill[st.getText(1)];
            }
            Json topics = Json::array();  // stored as "a|b|c"
            std::string raw = st.getText(5), cur;
            for (char c : raw + "|") {
                if (c == '|') { if (!cur.empty()) topics.push(cur); cur.clear(); }
                else cur += c;
            }
            recs.push(Json::object({{"id", st.getInt(0)}, {"skill", st.getText(1)}, {"title", st.getText(2)},
                                    {"description", st.getText(3)}, {"level", st.getText(4)}, {"topics", topics},
                                    {"score", round1(st.getDouble(6))}, {"source", st.getText(7)}, {"status", status},
                                    {"createdAt", st.getInt(9)}}));
        }
    }
    Json done = Json::object();
    for (const auto& [skill, n] : doneBySkill) done.set(skill, n);

    return Json::object({
        {"goal", Json::object({{"key", goal.key}, {"title", goal.title}, {"description", goal.description}})},
        {"mySkills", mySkills}, {"hasSkills", !current.empty()},
        {"rows", rows}, {"focus", focus}, {"gapPercent", round1(gaps.gapPercent)},
        {"recommendations", recs},
        {"progress", Json::object({{"completed", completed}, {"saved", saved}, {"skillsAtTarget", sufficient},
                                   {"skillsTotal", gaps.rows.size()}, {"remainingGaps", gaps.focus.size()},
                                   {"completedBySkill", done}})},
        {"geminiConfigured", geminiConfigured_},
    });
}

/* ---------- Gemini prompt + parsing ---------- */

std::string LearningService::buildPrompt(const LearningGoal& goal, const learn::GapReport& gaps) {
    std::string skills;
    for (std::size_t i = 0; i < gaps.focus.size() && i < SKILLS_PER_PROMPT; ++i) {
        const auto& g = gaps.focus[i];
        skills += "- " + g.name + ": current " + LEVEL_NAMES[g.current] + ", target " + LEVEL_NAMES[g.target] + "\n";
    }
    // Only the goal and skill levels are sent: no name, email or other personal data.
    return "You are an educational assistant that suggests YouTube video titles for a BTech student.\n"
           "Learning goal: " + goal.title + "\n"
           "Skills to learn, most important first:\n" + skills +
           "Suggest " + std::to_string(CANDIDATES_PER_PROMPT) + " different YouTube video titles across these skills, "
           "with more for the first skills. Prefer practical tutorials with examples, fundamentals before advanced topics, "
           "and nothing above each skill's target level. No duplicate or near-identical titles. "
           "Each \"skill\" must be exactly one of the skill names above.\n"
           "Answer with JSON only, in this shape:\n"
           "{\"recommendations\":[{\"skill\":\"NumPy\",\"title\":\"...\",\"description\":\"one sentence on what it teaches\","
           "\"topics\":[\"...\",\"...\"],\"level\":\"Beginner|Intermediate|Advanced\"}]}";
}

std::vector<learn::Candidate> LearningService::parseCandidates(const std::string& text) {
    // Models sometimes wrap JSON in ```json fences or add a sentence: keep the outermost {...} or [...].
    std::size_t start = text.find_first_of("{[");
    std::size_t end = text.find_last_of("}]");
    if (start == std::string::npos || end == std::string::npos || end < start) throw std::runtime_error("no JSON in answer");
    JsonValue doc = parseJson(text.substr(start, end - start + 1));
    const JsonValue* list = doc.isArray() ? &doc : doc.get("recommendations");
    if (!list || !list->isArray()) throw std::runtime_error("no recommendations array");

    std::vector<learn::Candidate> out;
    for (const JsonValue& item : list->items) {
        if (!item.isObject()) continue;
        learn::Candidate c;
        c.skill = item.str("skill");
        c.title = item.str("title");
        c.description = item.str("description");
        c.level = item.str("level");
        if (const JsonValue* topics = item.get("topics"); topics && topics->isArray()) {
            for (const JsonValue& t : topics->items) {
                if (t.type == JsonValue::Type::String) c.topics.push_back(t.string);
            }
        }
        out.push_back(std::move(c));
    }
    return out;
}

std::vector<learn::Candidate> LearningService::fallbackCandidates(const LearningGoal& goal, const learn::GapReport& gaps) {
    // Used when Gemini can't answer. These are search phrases, not specific videos.
    std::vector<learn::Candidate> out;
    for (std::size_t i = 0; i < gaps.focus.size() && i < SKILLS_PER_PROMPT; ++i) {
        const auto& g = gaps.focus[i];
        std::string next = LEVEL_NAMES[std::min(g.target, g.current + 1)];
        out.push_back({g.name, g.name + " full course for beginners", "A complete introduction to " + g.name + " from the basics.",
                       "Beginner", {g.name + " basics", "setup", "first examples"}});
        out.push_back({g.name, g.name + " tutorial with practical examples", "Hands-on " + g.name + " examples you can follow along.",
                       next, {g.name, "examples", "practice"}});
        out.push_back({g.name, g.name + " project tutorial step by step", "Use " + g.name + " in a small project (goal: " + goal.title + ").",
                       next, {g.name, "project"}});
    }
    return out;
}

/* ---------- Recommend ---------- */

Json LearningService::recommend(const User& u, const std::string& goalKey) {
    const LearningGoal& goal = goalOr400(goalKey);
    auto current = currentSkills(u.id, nullptr);
    if (current.empty()) {
        throw HttpError(400, "Add your skills first (Find Project Partner → My profile), so the gap can be calculated.");
    }
    learn::GapReport gaps = learn::analyse(goal.skills, current);
    if (gaps.focus.empty()) {
        Json out = analysis(u, goal.key);
        out.set("run", Json::object({{"source", "none"}, {"warning", "No skill gaps for this goal: you already meet every target."}}));
        return out;
    }

    // Titles already saved or completed for this goal are not suggested again.
    std::unordered_set<std::string> skip;
    {
        auto lock = db_.lock();
        auto st = db_.prepare("SELECT title_key FROM learning_recommendations WHERE user_id = ? AND goal_key = ? AND status <> 'new'");
        st.bind(1, u.id).bind(2, goal.key);
        while (st.step()) skip.insert(st.getText(0));
    }

    std::string source = "gemini", model, warning;
    learn::RankResult ranked;
    GeminiResult answer = ask_ ? ask_(buildPrompt(goal, gaps)) : GeminiResult{false, "", "", "Gemini isn't set up."};
    if (answer.ok) {
        try {
            ranked = learn::rankCandidates(parseCandidates(answer.text), gaps, skip);
            model = answer.model;
            if (ranked.top.empty()) warning = "None of Gemini's suggestions matched your skill gaps, so these are standard search suggestions.";
        } catch (const std::exception&) {
            warning = "Gemini's answer wasn't in the expected format, so these are standard search suggestions.";
        }
    } else {
        warning = answer.error + " Showing standard search suggestions instead.";
    }
    if (ranked.top.empty()) {
        source = "fallback";
        learn::RankResult fb = learn::rankCandidates(fallbackCandidates(goal, gaps), gaps, skip);
        fb.received = ranked.received;  // report what Gemini sent, not the fallback list
        fb.unrelated += ranked.unrelated;
        fb.duplicates += ranked.duplicates;
        fb.tooAdvanced += ranked.tooAdvanced;
        ranked = std::move(fb);
    }

    long long now = nowMs();
    {
        auto lock = db_.lock();
        Transaction tx(db_);
        // Unsaved suggestions are replaced; saved and completed ones stay.
        db_.prepare("DELETE FROM learning_recommendations WHERE user_id = ? AND goal_key = ? AND status = 'new'")
            .bind(1, u.id).bind(2, goal.key).run();
        auto st = db_.prepare(
            "INSERT OR IGNORE INTO learning_recommendations (user_id, goal_key, skill, title, title_key, description, level, "
            "topics, score, source, created_at, updated_at) VALUES (?,?,?,?,?,?,?,?,?,?,?,?)");
        for (const auto& s : ranked.top) {
            std::string topics;
            for (const auto& t : s.c.topics) {
                std::string clean;
                for (char c : trim(t)) clean += c == '|' ? '/' : c;
                if (clean.empty()) continue;
                if (!topics.empty()) topics += '|';
                topics += clean.substr(0, 60);
            }
            st.reset();
            st.bind(1, u.id).bind(2, goal.key).bind(3, s.skill).bind(4, s.c.title).bind(5, learn::titleKey(s.c.title))
                .bind(6, s.c.description.substr(0, 400)).bind(7, s.c.level.substr(0, 20)).bind(8, topics)
                .bind(9, s.total).bind(10, source).bind(11, now).bind(12, now).run();
        }
        tx.commit();
    }

    Json out = analysis(u, goal.key);
    out.set("run", Json::object({
        {"source", source}, {"model", model}, {"warning", warning},
        {"received", ranked.received}, {"shown", ranked.top.size()},
        {"filtered", Json::object({{"empty", ranked.empty}, {"unrelated", ranked.unrelated}, {"tooAdvanced", ranked.tooAdvanced},
                                   {"duplicates", ranked.duplicates}, {"alreadySaved", ranked.alreadySaved}})},
    }));
    return out;
}

Json LearningService::setStatus(const User& u, long long id, const std::string& status) {
    if (status != "new" && status != "saved" && status != "completed") throw HttpError(400, "status must be new, saved or completed.");
    auto lock = db_.lock();
    db_.prepare("UPDATE learning_recommendations SET status = ?, updated_at = ? WHERE id = ? AND user_id = ?")
        .bind(1, status).bind(2, nowMs()).bind(3, id).bind(4, u.id).run();
    if (db_.changes() == 0) throw HttpError(404, "Recommendation not found.");
    return Json::object({{"id", id}, {"status", status}});
}

}  // namespace cx
