#pragma once
// Skill gap analysis and learning-resource ranking. Pure algorithms (no
// database, no network) so each step is testable on its own.
//
//   1. Gap per required skill:  G = max(0, target - current)
//      Priority:                P = G * weight   (weight: high 3, medium 2, low 1)
//      Overall gap:             Gap% = sum(w * G) / sum(w * T) * 100
//   2. Candidates (from Gemini) are filtered: empty, unrelated (label and title name no gap skill), above the
//      target level, duplicate or nearly identical titles.
//   3. Each survivor is scored 0..100:
//      R = 0.35 skill + 0.25 level + 0.20 goal + 0.15 topics + 0.05 practical
//   4. A max-heap returns the best first; at most 3 per skill, top 5 overall.
// Levels follow the rest of CampusX: 0 = not in profile, 1 Beginner,
// 2 Intermediate, 3 Advanced.
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace cx {
namespace learn {

struct RequiredSkill {
    std::string name;
    int target = 2;  // level wanted (1..3)
    int weight = 2;  // high 3, medium 2, low 1
};

struct SkillGapRow {
    std::string name;
    int current = 0, target = 0, gap = 0, weight = 0, priority = 0;
    const char* status = "sufficient";  // sufficient | improve | missing
};

struct GapReport {
    std::vector<SkillGapRow> rows;  // in the goal's order
    std::vector<SkillGapRow> focus;  // gap > 0 only, highest priority first
    double gapPercent = 0;
};

// `current` maps lower-cased skill name -> level (hash map lookup per required skill).
GapReport analyse(const std::vector<RequiredSkill>& required, const std::unordered_map<std::string, int>& current);

struct Candidate {
    std::string skill, title, description, level;
    std::vector<std::string> topics;
};

struct Scored {
    Candidate c;
    std::string skill;  // the gap skill it was matched to (canonical name)
    double skillScore = 0, levelScore = 0, goalScore = 0, topicScore = 0, practicalScore = 0, total = 0;
};

struct RankResult {
    std::vector<Scored> top;  // best first
    int received = 0, empty = 0, unrelated = 0, tooAdvanced = 0, duplicates = 0, alreadySaved = 0;
};

int levelFromLabel(const std::string& label);  // "Beginner" -> 1 ... unknown -> 0
std::string titleKey(const std::string& title);  // lower-cased words joined by one space

// `skip` holds titleKey()s the student already saved or completed for this goal.
RankResult rankCandidates(const std::vector<Candidate>& candidates, const GapReport& gaps,
                          const std::unordered_set<std::string>& skip, std::size_t limit = 5, std::size_t perSkill = 3);

}  // namespace learn
}  // namespace cx
