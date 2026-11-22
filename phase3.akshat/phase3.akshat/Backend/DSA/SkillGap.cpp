#include "SkillGap.h"

#include <algorithm>
#include <cstdlib>

#include "../Util/Text.h"
#include "MinHeap.h"

namespace cx {
namespace learn {

namespace {

// True if the words of `phrase` appear next to each other in `words` ("machine learning" in a title).
bool containsPhrase(const std::vector<std::string>& words, const std::vector<std::string>& phrase) {
    if (phrase.empty() || phrase.size() > words.size()) return false;
    for (std::size_t i = 0; i + phrase.size() <= words.size(); ++i) {
        if (std::equal(phrase.begin(), phrase.end(), words.begin() + static_cast<long>(i))) return true;
    }
    return false;
}

// Jaccard similarity of two word sets: |A ∩ B| / |A ∪ B|. Titles at 0.7 or more count as the same video
// ("NumPy for Beginners" vs "Pandas for Beginners" is only 0.5).
double similarity(const std::unordered_set<std::string>& a, const std::unordered_set<std::string>& b) {
    if (a.empty() && b.empty()) return 1.0;
    std::size_t common = 0;
    for (const auto& w : a) common += b.count(w);
    return static_cast<double>(common) / static_cast<double>(a.size() + b.size() - common);
}

const char* PRACTICAL_WORDS[] = {"project", "projects", "hands", "practical", "example", "examples", "build",
                                 "building", "exercises", "implement", "real", "practice"};

}  // namespace

GapReport analyse(const std::vector<RequiredSkill>& required, const std::unordered_map<std::string, int>& current) {
    GapReport r;
    double weightedGap = 0, weightedTarget = 0;
    for (const RequiredSkill& req : required) {
        SkillGapRow row;
        row.name = req.name;
        auto it = current.find(toLowerAscii(req.name));
        row.current = it == current.end() ? 0 : it->second;  // 0 = not in the profile, not "knows nothing"
        row.target = req.target;
        row.weight = req.weight;
        row.gap = std::max(0, row.target - row.current);
        row.priority = row.gap * row.weight;
        row.status = row.gap == 0 ? "sufficient" : row.current == 0 ? "missing" : "improve";
        weightedGap += row.weight * row.gap;
        weightedTarget += row.weight * row.target;
        r.rows.push_back(row);
        if (row.gap > 0) r.focus.push_back(row);
    }
    r.gapPercent = weightedTarget > 0 ? 100.0 * weightedGap / weightedTarget : 0.0;
    // Highest priority first; ties: bigger gap, then name (stable output).
    std::sort(r.focus.begin(), r.focus.end(), [](const SkillGapRow& a, const SkillGapRow& b) {
        if (a.priority != b.priority) return a.priority > b.priority;
        if (a.gap != b.gap) return a.gap > b.gap;
        return a.name < b.name;
    });
    return r;
}

int levelFromLabel(const std::string& label) {
    std::string l = toLowerAscii(trim(label));
    if (l.rfind("beginner", 0) == 0 || l == "basic") return 1;
    if (l.rfind("intermediate", 0) == 0) return 2;
    if (l.rfind("advanced", 0) == 0 || l == "expert") return 3;
    return 0;
}

std::string titleKey(const std::string& title) {
    std::string key;
    for (const auto& w : tokenize(title)) {
        if (!key.empty()) key += ' ';
        key += w;
    }
    return key;
}

RankResult rankCandidates(const std::vector<Candidate>& candidates, const GapReport& gaps,
                          const std::unordered_set<std::string>& skip, std::size_t limit, std::size_t perSkill) {
    RankResult out;
    out.received = static_cast<int>(candidates.size());
    if (gaps.focus.empty()) return out;

    int maxPriority = gaps.focus.front().priority;  // focus is sorted, so the first is the largest
    std::unordered_map<std::string, const SkillGapRow*> bySkill;  // lower-cased name -> gap row
    std::vector<std::pair<const SkillGapRow*, std::vector<std::string>>> skillWords;
    for (const SkillGapRow& g : gaps.focus) {
        bySkill[toLowerAscii(g.name)] = &g;
        skillWords.push_back({&g, tokenize(g.name)});
    }

    std::unordered_set<std::string> seenKeys;
    std::vector<std::unordered_set<std::string>> keptWords;  // word sets of titles kept so far
    auto better = [](const Scored& a, const Scored& b) {
        if (a.total != b.total) return a.total > b.total;
        if (a.goalScore != b.goalScore) return a.goalScore > b.goalScore;
        return a.c.title < b.c.title;  // consistent tie-break
    };
    MinHeap<Scored, decltype(better)> heap(better);  // root = best, so it behaves as a max-heap

    for (const Candidate& c : candidates) {
        std::string title = trim(c.title);
        if (title.empty() || title.size() > 160) { ++out.empty; continue; }

        // Which gap skill is this about? Trust the label only if it names a skill we asked for,
        // otherwise look for a gap skill's name in the title.
        std::vector<std::string> titleWords = tokenize(title);
        const SkillGapRow* gap = nullptr;
        auto it = bySkill.find(toLowerAscii(trim(c.skill)));
        bool labelled = it != bySkill.end();
        if (labelled) gap = it->second;
        for (std::size_t i = 0; !gap && i < skillWords.size(); ++i) {
            if (containsPhrase(titleWords, skillWords[i].second)) gap = skillWords[i].first;
        }
        if (!gap) { ++out.unrelated; continue; }

        std::vector<std::string> nameWords = tokenize(gap->name);
        std::string extra = c.description;
        for (const auto& t : c.topics) extra += " " + t;
        std::vector<std::string> extraWords = tokenize(extra);

        Scored s;
        s.c = c;
        s.c.title = title;
        s.skill = gap->name;
        // Skill named in the title: 100; in the description or topics: 70; only in Gemini's label: 50
        // (e.g. a "Hypothesis Testing" video labelled Statistics).
        s.skillScore = containsPhrase(titleWords, nameWords) ? 100 : containsPhrase(extraWords, nameWords) ? 70 : labelled ? 50 : 0;
        if (s.skillScore == 0) { ++out.unrelated; continue; }

        int level = levelFromLabel(c.level);
        if (level > gap->target) { ++out.tooAdvanced; continue; }
        int next = std::min(gap->target, gap->current + 1);  // the level the student should study next
        int diff = level == 0 ? -1 : std::abs(level - next);
        s.levelScore = diff < 0 ? 50 : diff == 0 ? 100 : diff == 1 ? 60 : 20;

        std::string key = titleKey(title);
        if (skip.count(key)) { ++out.alreadySaved; continue; }
        std::unordered_set<std::string> words(titleWords.begin(), titleWords.end());
        bool duplicate = !seenKeys.insert(key).second;
        for (std::size_t i = 0; !duplicate && i < keptWords.size(); ++i) duplicate = similarity(words, keptWords[i]) >= 0.7;
        if (duplicate) { ++out.duplicates; continue; }
        keptWords.push_back(std::move(words));

        s.goalScore = maxPriority > 0 ? 100.0 * gap->priority / maxPriority : 0;
        std::size_t topics = 0;
        for (const auto& t : c.topics) topics += !trim(t).empty();
        s.topicScore = static_cast<double>(std::min<std::size_t>(4, topics)) * 25.0;
        std::vector<std::string> allWords = titleWords;
        allWords.insert(allWords.end(), extraWords.begin(), extraWords.end());
        bool practical = false;
        for (const char* w : PRACTICAL_WORDS) practical = practical || std::find(allWords.begin(), allWords.end(), w) != allWords.end();
        s.practicalScore = practical ? 100 : 50;

        s.total = 0.35 * s.skillScore + 0.25 * s.levelScore + 0.20 * s.goalScore + 0.15 * s.topicScore + 0.05 * s.practicalScore;
        heap.push(std::move(s));
    }

    // Pop best first; cap each skill so one skill can't take all five places.
    std::unordered_map<std::string, std::size_t> perSkillCount;
    while (!heap.empty() && out.top.size() < limit) {
        Scored s = heap.pop();
        if (perSkillCount[s.skill]++ >= perSkill) continue;
        out.top.push_back(std::move(s));
    }
    return out;
}

}  // namespace learn
}  // namespace cx
