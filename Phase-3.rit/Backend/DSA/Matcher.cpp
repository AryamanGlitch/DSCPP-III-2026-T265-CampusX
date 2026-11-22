#include "Matcher.h"

#include <algorithm>
#include <deque>

#include "../dsa/MinHeap.h"

namespace cx {
namespace match {

int countBits(unsigned v) {
    int n = 0;
    while (v) {
        v &= v - 1;  // clears the lowest set bit
        ++n;
    }
    return n;
}

double skillScore(const ProjectReq& p, const Profile& s, std::vector<int>* matched) {
    if (p.skills.empty()) return 100.0;
    double total = 0, earned = 0;
    for (std::size_t i = 0; i < p.skills.size(); ++i) {
        const SkillReq& r = p.skills[i];
        total += r.importance;
        auto it = s.skills.find(r.skillId);  // hash map: O(1) per required skill
        if (it == s.skills.end()) continue;
        if (matched) matched->push_back(static_cast<int>(i));
        earned += r.importance * std::min(1.0, static_cast<double>(it->second) / r.level);
    }
    return total > 0 ? 100.0 * earned / total : 100.0;
}

double availabilityScore(const ProjectReq& p, const Profile& s) {
    int wanted = countBits(p.availability);
    if (wanted == 0) return 100.0;  // project has no preferred times
    return 100.0 * countBits(p.availability & s.availability) / wanted;
}

double experienceScore(int wanted, int has) {
    int gap = wanted - has;
    if (gap <= 0) return 100.0;
    return gap == 1 ? 50.0 : 0.0;
}

double interestScore(const ProjectReq& p, const Profile& s) { return (p.domainBit & s.interests) ? 100.0 : 0.0; }

double combine(double skill, double availability, double experience, double interest, const Weights& w) {
    return w.skill * skill + w.availability * availability + w.experience * experience + w.interest * interest;
}

Scores score(const ProjectReq& p, const Profile& s, const Weights& w) {
    Scores r;
    r.skill = skillScore(p, s, &r.matched);
    r.availability = availabilityScore(p, s);
    r.experience = experienceScore(p.experience, s.experience);
    r.interest = interestScore(p, s);
    r.total = combine(r.skill, r.availability, r.experience, r.interest, w);
    r.sharedSlots = p.availability & s.availability;
    return r;
}

Exclusion eligibility(const ProjectReq& p, const Profile& s, int maxActiveProjects) {
    for (const SkillReq& r : p.skills) {
        if (r.mandatory && !s.skills.count(r.skillId)) return Exclusion::MissingMandatory;
    }
    if (p.scheduleRequired && p.availability && !(p.availability & s.availability)) return Exclusion::NoSharedTime;
    if (maxActiveProjects > 0 && s.activeProjects >= maxActiveProjects) return Exclusion::TooManyProjects;
    return Exclusion::None;
}

bool ranksHigher(const Candidate& a, const Candidate& b, const std::vector<Profile>& profiles) {
    if (a.scores.total != b.scores.total) return a.scores.total > b.scores.total;
    const Profile& pa = profiles[a.profile];
    const Profile& pb = profiles[b.profile];
    if (pa.activeProjects != pb.activeProjects) return pa.activeProjects < pb.activeProjects;
    return pa.userId < pb.userId;
}

RankResult rank(const ProjectReq& p, const std::vector<Profile>& profiles, const std::unordered_set<long long>& excludeIds,
                int maxActiveProjects, const Weights& w) {
    RankResult out;

    // Candidate generation as a graph walk: project -> skill nodes -> student nodes.
    // Edges skill -> student come from an adjacency list built once per call.
    std::unordered_map<int, std::vector<std::size_t>> studentsWithSkill;
    for (std::size_t i = 0; i < profiles.size(); ++i) {
        for (const auto& [skill, level] : profiles[i].skills) {
            (void)level;
            studentsWithSkill[skill].push_back(i);
        }
    }
    std::vector<std::size_t> reached;
    std::unordered_set<std::size_t> seen;  // a student with 3 matching skills is reached 3 times; keep once
    std::deque<int> frontier;
    for (const SkillReq& r : p.skills) frontier.push_back(r.skillId);
    while (!frontier.empty()) {
        int skill = frontier.front();
        frontier.pop_front();
        auto it = studentsWithSkill.find(skill);
        if (it == studentsWithSkill.end()) continue;
        for (std::size_t i : it->second) {
            if (seen.insert(i).second) reached.push_back(i);
        }
    }
    if (p.skills.empty()) {  // nothing to match on: everyone is a candidate
        for (std::size_t i = 0; i < profiles.size(); ++i) reached.push_back(i);
    }

    int eligibleTotal = 0;
    for (std::size_t i = 0; i < profiles.size(); ++i) {
        if (!excludeIds.count(profiles[i].userId)) ++eligibleTotal;
    }

    auto better = [&profiles](const Candidate& a, const Candidate& b) { return ranksHigher(a, b, profiles); };
    MinHeap<Candidate, decltype(better)> heap(better);  // root = best, so it behaves as a max-heap
    for (std::size_t i : reached) {
        const Profile& s = profiles[i];
        if (excludeIds.count(s.userId)) continue;
        ++out.considered;
        switch (eligibility(p, s, maxActiveProjects)) {
            case Exclusion::MissingMandatory: ++out.missingMandatory; continue;
            case Exclusion::NoSharedTime: ++out.noSharedTime; continue;
            case Exclusion::TooManyProjects: ++out.tooManyProjects; continue;
            case Exclusion::None: break;
        }
        heap.push(Candidate{i, score(p, s, w)});
    }
    out.noSkillOverlap = eligibleTotal - out.considered;
    while (!heap.empty()) out.ranked.push_back(heap.pop());  // O(n log n) heap sort, best first
    return out;
}

std::vector<TeamPick> formTeam(const ProjectReq& p, const std::vector<Profile>& profiles,
                               const std::vector<Candidate>& ranked, std::vector<bool>& covered, int slots) {
    std::vector<TeamPick> team;
    covered.resize(p.skills.size(), false);
    std::vector<bool> used(ranked.size(), false);

    for (int round = 0; round < slots; ++round) {
        // Gain is measured against what is STILL missing (as in greedy set cover), not the
        // total: otherwise compatibility, which varies little between students, drowns it out.
        double missingImportance = 0;
        for (std::size_t i = 0; i < p.skills.size(); ++i) {
            if (!covered[i]) missingImportance += p.skills[i].importance;
        }
        int best = -1;
        double bestScore = -1;
        std::vector<int> bestNew;
        for (std::size_t c = 0; c < ranked.size(); ++c) {
            if (used[c]) continue;
            const Profile& s = profiles[ranked[c].profile];
            std::vector<int> fresh;
            double gain = 0;
            for (std::size_t i = 0; i < p.skills.size(); ++i) {
                if (!covered[i] && s.skills.count(p.skills[i].skillId)) {
                    fresh.push_back(static_cast<int>(i));
                    gain += p.skills[i].importance;
                }
            }
            double gainPct = missingImportance > 0 ? 100.0 * gain / missingImportance : 0.0;
            double pick = 0.5 * ranked[c].scores.total + 0.5 * gainPct;
            // Strictly greater: on a tie the earlier (higher-ranked) candidate stays.
            if (pick > bestScore) {
                best = static_cast<int>(c);
                bestScore = pick;
                bestNew = std::move(fresh);
            }
        }
        if (best < 0) break;  // ran out of candidates
        used[best] = true;
        for (int i : bestNew) covered[i] = true;
        team.push_back(TeamPick{static_cast<std::size_t>(best), std::move(bestNew), bestScore});
    }
    return team;
}

}  // namespace match
}  // namespace cx
