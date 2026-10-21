#pragma once
#include <mutex>
#include <shared_mutex>
#include <string>
#include <vector>

#include "db/Database.h"
#include "dsa/LRUCache.h"
#include "dsa/Trie.h"
#include "util/Json.h"

namespace cx {

struct SearchDoc {
    std::string kind;  // place | faculty | event
    long long dbId = 0;
    std::string title, subtitle, detail, navNodeId;
    long long startsAt = 0, endsAt = 0;  // events only
};

class SearchService {
public:
    explicit SearchService(Database& db);

    void rebuild();  // reload places/faculty/events from SQLite into the trie
    Json search(const std::string& query, const std::string& kind, int limit, long long now);
    Json stats() const;

private:
    using Ranked = std::vector<std::pair<int, double>>;  // (doc index, score), best first
    Ranked rank(const std::vector<std::string>& tokens) const;
    void rebuildIfStale(long long now);

    Database& db_;
    mutable std::shared_mutex mu_;  // many concurrent searches, exclusive rebuilds
    Trie trie_;
    std::vector<SearchDoc> docs_;
    long long builtAt_ = 0;

    std::mutex cacheMu_;  // separate lock: LRU get() reorders its list
    LRUCache<std::string, Ranked> cache_{256};
};

}  // namespace cx
