#pragma once
#include <functional>
#include <string>
#include <vector>

#include "../database/Database.h"
#include "../DSA/SkillGap.h"
#include "../Util/Json.h"
#include "AuthService.h"
#include "GeminiClient.h"

namespace cx {

struct LearningGoal {
    std::string key, title, description;
    std::vector<learn::RequiredSkill> skills;
};

// Skill gap analysis + learning recommendations.
// The student's skills are the ones in their partner profile (one skill list
// for the whole app). Gaps and ranking are computed in C++; Gemini only
// suggests candidate video titles.
class LearningService {
public:
    using AskGemini = std::function<GeminiResult(const std::string& prompt)>;

    LearningService(Database& db, AskGemini ask, bool geminiConfigured);

    static const std::vector<LearningGoal>& goals();
    static const LearningGoal* findGoal(const std::string& key);

    Json goalsJson();
    Json analysis(const User& u, const std::string& goalKey);  // gap report + saved recommendations + progress
    Json recommend(const User& u, const std::string& goalKey);  // asks Gemini, ranks, stores the top 5
    Json setStatus(const User& u, long long recommendationId, const std::string& status);

    // Exposed for tests.
    static std::string buildPrompt(const LearningGoal& goal, const learn::GapReport& gaps);
    static std::vector<learn::Candidate> parseCandidates(const std::string& geminiText);  // throws on bad JSON
    static std::vector<learn::Candidate> fallbackCandidates(const LearningGoal& goal, const learn::GapReport& gaps);

private:
    std::unordered_map<std::string, int> currentSkills(long long userId, Json* list);
    const LearningGoal& goalOr400(const std::string& key);

    Database& db_;
    AskGemini ask_;
    bool geminiConfigured_;
};

}  // namespace cx
