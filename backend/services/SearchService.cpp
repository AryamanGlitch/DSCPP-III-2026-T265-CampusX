#include "services/SearchService.h"

#include <algorithm>
#include <chrono>
#include <unordered_map>
#include <unordered_set>

#include "dsa/TopK.h"
#include "util/HttpError.h"
#include "util/Text.h"

namespace cx {

namespace {
constexpr long long REBUILD_EVERY_MS = 5 * 60 * 1000;
constexpr std::size_t MAX_RANKED = 200;

// How much a word counts depending on where it appears.
constexpr float W_TITLE = 3.0f, W_KEYWORD = 2.0f, W_SUBTITLE = 1.0f;

// How much a match counts depending on its quality.
constexpr double EXACT = 1.0, PREFIX = 0.75, TYPO_1 = 0.6, TYPO_2 = 0.45;

std::string joinNonEmpty(std::initializer_list<std::string> parts, const std::string& sep) {
    std::string out;
    for (const auto& p : parts) {
        if (p.empty()) continue;
        if (!out.empty()) out += sep;
        out += p;
    }
    return out;
}
}  // namespace

SearchService::SearchService(Database& db) : db_(db) {}

void SearchService::rebuild() {
    std::vector<SearchDoc> docs;
    std::vector<std::string> keywords;  // parallel to docs
    {
        auto lock = db_.lock();
        // Self-join shows "Academic Block A" instead of the raw building code "ABA".
        auto p = db_.prepare(
            "SELECT p.id, p.name, p.type, p.code, p.description, p.tags, COALESCE(b.name, p.building), p.floor, p.nav_node_id "
            "FROM places p LEFT JOIN places b ON b.code = p.building AND b.type = 'building'");
        while (p.step()) {
            std::string where = p.isNull(6) ? "" : p.getText(6);
            if (!p.isNull(7)) {
                std::string floor = p.getInt(7) == 0 ? "ground floor" : "floor " + std::to_string(p.getInt(7));
                where += (where.empty() ? "" : ", ") + floor;
            }
            docs.push_back({"place", p.getInt(0), p.getText(1), joinNonEmpty({p.getText(2), where}, ", "),
                            p.getText(4), p.getText(8)});
            keywords.push_back(p.getText(3) + " " + p.getText(2) + " " + p.getText(5));
        }
        auto f = db_.prepare("SELECT id, name, designation, department, email, cabin, nav_node_id, research_areas FROM faculty");
        while (f.step()) {
            docs.push_back({"faculty", f.getInt(0), f.getText(1), joinNonEmpty({f.getText(2), f.getText(3)}, ", "),
                            joinNonEmpty({f.getText(5), f.getText(4)}, " | "), f.getText(6)});
            keywords.push_back(f.getText(3) + " " + f.getText(7));
        }
        auto e = db_.prepare("SELECT id, title, description, venue, nav_node_id, starts_at, ends_at, organizer, tags FROM events");
        while (e.step()) {
            SearchDoc d{"event", e.getInt(0), e.getText(1), joinNonEmpty({e.getText(3), e.getText(7)}, ", "),
                        e.getText(2), e.getText(4)};
            d.startsAt = e.getInt(5);
            d.endsAt = e.getInt(6);
            docs.push_back(std::move(d));
            keywords.push_back(e.getText(8) + " " + e.getText(7));
        }
    }

    // Build the new trie without holding the search lock, then swap it in.
    Trie trie;
    for (std::size_t i = 0; i < docs.size(); ++i) {
        int id = static_cast<int>(i);
        for (const auto& w : tokenize(docs[i].title)) trie.insert(w, id, W_TITLE);
        for (const auto& w : tokenize(keywords[i])) trie.insert(w, id, W_KEYWORD);
        for (const auto& w : tokenize(docs[i].subtitle)) trie.insert(w, id, W_SUBTITLE);
    }
    std::unique_lock<std::shared_mutex> lock(mu_);
    trie_ = std::move(trie);
    docs_ = std::move(docs);
    builtAt_ = nowMs();
    std::lock_guard<std::mutex> cacheLock(cacheMu_);
    cache_.clear();  // cached rankings refer to old doc indices
}

void SearchService::rebuildIfStale(long long now) {
    bool stale;
    {
        std::shared_lock<std::shared_mutex> lock(mu_);
        stale = now - builtAt_ > REBUILD_EVERY_MS;
    }
    if (stale) rebuild();
}

SearchService::Ranked SearchService::rank(const std::vector<std::string>& tokens) const {
    // For each query word, the best score each document earns for it.
    std::vector<std::unordered_map<int, double>> perToken(tokens.size());
    std::vector<Trie::Match> matches;

    for (std::size_t t = 0; t < tokens.size(); ++t) {
        const std::string& word = tokens[t];
        auto& best = perToken[t];
        auto credit = [&](const Trie::Match& m, double quality) {
            for (const Posting& p : *m.postings) {
                double s = p.weight * quality;
                auto it = best.find(p.docId);
                if (it == best.end() || it->second < s) best[p.docId] = s;
            }
        };

        matches.clear();
        trie_.collectPrefix(word, 100, matches);  // "lib" -> library, librarian ...
        for (const auto& m : matches) credit(m, m.word == word ? EXACT : PREFIX);

        if (word.size() >= 4) {  // typo tolerance only for longer words; short ones match too much
            int maxDist = word.size() >= 8 ? 2 : 1;
            matches.clear();
            trie_.fuzzy(word, maxDist, matches);
            for (const auto& m : matches) {
                if (m.distance > 0) credit(m, m.distance == 1 ? TYPO_1 : TYPO_2);
            }
        }
    }

    // AND semantics first (every query word must match). If that finds
    // nothing for a multi-word query, fall back to OR.
    auto combine = [&](bool requireAll) {
        std::unordered_map<int, std::pair<double, std::size_t>> total;  // doc -> (score, words matched)
        for (const auto& m : perToken) {
            for (const auto& [doc, s] : m) {
                auto& entry = total[doc];
                entry.first += s;
                entry.second += 1;
            }
        }
        TopK<int> top(MAX_RANKED);
        for (const auto& [doc, entry] : total) {
            if (requireAll && entry.second < tokens.size()) continue;
            top.offer(entry.first, doc, doc);
        }
        return top.takeSorted();
    };

    Ranked ranked = combine(true);
    if (ranked.empty() && tokens.size() > 1) ranked = combine(false);
    return ranked;
}

Json SearchService::search(const std::string& queryIn, const std::string& kind, int limit, long long now) {
    auto started = std::chrono::steady_clock::now();
    std::string query = trim(queryIn);
    if (query.size() > 100) throw HttpError(400, "Search text is too long (max 100 characters).");
    if (kind != "all" && kind != "place" && kind != "faculty" && kind != "event") {
        throw HttpError(400, "kind must be all, place, faculty or event.");
    }
    limit = std::clamp(limit, 1, 50);

    Json results = Json::array();
    bool cacheHit = false;
    std::vector<std::string> tokens = tokenize(query);
    // Remove duplicate words ("lab lab") so they don't double the score.
    std::unordered_set<std::string> seen;
    tokens.erase(std::remove_if(tokens.begin(), tokens.end(), [&](const std::string& t) { return !seen.insert(t).second; }),
                 tokens.end());

    if (query.size() >= 2 && !tokens.empty()) {
        rebuildIfStale(now);
        std::string key;
        for (const auto& t : tokens) key += t + ' ';

        // Hold the index lock across cache lookup AND use, so a rebuild can't
        // swap docs_ between reading a cached ranking and applying it.
        // Lock order is always mu_ then cacheMu_ (rebuild uses the same order).
        std::shared_lock<std::shared_mutex> lock(mu_);
        Ranked ranked;
        {
            std::lock_guard<std::mutex> cacheLock(cacheMu_);
            if (auto hit = cache_.get(key)) { ranked = std::move(*hit); cacheHit = true; }
        }
        if (!cacheHit) {
            ranked = rank(tokens);
            std::lock_guard<std::mutex> cacheLock(cacheMu_);
            cache_.put(key, ranked);
        }
        // Filters run on every request (not cached) so ended events disappear on time.
        for (const auto& [idx, score] : ranked) {
            if (static_cast<int>(results.size()) >= limit) break;
            if (idx < 0 || idx >= static_cast<int>(docs_.size())) continue;
            const SearchDoc& d = docs_[idx];
            if (kind != "all" && d.kind != kind) continue;
            if (d.kind == "event" && d.endsAt < now) continue;
            Json item = Json::object({{"kind", d.kind}, {"id", d.dbId}, {"title", d.title}, {"subtitle", d.subtitle},
                                      {"detail", d.detail}, {"score", score}});
            item.set("navNodeId", d.navNodeId.empty() ? Json() : Json(d.navNodeId));
            if (d.kind == "event") item.set("startsAt", d.startsAt).set("endsAt", d.endsAt);
            results.push(std::move(item));
        }
    }

    long long micros = std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - started).count();
    return Json::object({{"query", query}, {"results", results}, {"cached", cacheHit}, {"tookMicros", micros}});
}

Json SearchService::stats() const {
    std::shared_lock<std::shared_mutex> lock(mu_);
    return Json::object({{"documents", docs_.size()}, {"uniqueWords", trie_.wordCount()}, {"trieNodes", trie_.nodeCount()}});
}

}  // namespace cx
