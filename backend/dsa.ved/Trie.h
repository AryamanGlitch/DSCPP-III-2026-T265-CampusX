#pragma once
// Trie (prefix tree) over the words of every searchable document.
// Each word-ending node stores a posting list: which documents contain the
// word, and how important it is there (title > keywords > subtitle).
//
//   insert(word)            O(L)
//   prefix("lib")           O(L + size of subtree)  -> "library", "librarian"
//   fuzzy("libary", 1)      walks the trie with one Levenshtein DP row per
//                           node and prunes any branch whose row minimum
//                           already exceeds the allowed distance. Shared
//                           prefixes are computed once, which is why this
//                           beats comparing the query with every word.
#include <map>
#include <string>
#include <vector>

namespace cx {

struct Posting {
    int docId;
    float weight;
};

class Trie {
public:
    struct Match {
        std::string word;
        int distance;  // 0 for prefix matches
        const std::vector<Posting>* postings;
    };

    Trie();
    void insert(const std::string& word, int docId, float weight);
    void collectPrefix(const std::string& prefix, std::size_t maxWords, std::vector<Match>& out) const;
    void fuzzy(const std::string& word, int maxDist, std::vector<Match>& out) const;
    void clear();

    std::size_t nodeCount() const { return nodes_.size(); }
    std::size_t wordCount() const { return words_; }

private:
    struct Node {
        std::map<unsigned char, int> children;  // ordered => deterministic results
        std::vector<Posting> postings;
        bool terminal = false;
    };

    void fuzzyVisit(int node, unsigned char ch, const std::vector<int>& prevRow, const std::string& word,
                    int maxDist, std::string& path, std::vector<Match>& out) const;

    std::vector<Node> nodes_;  // node pool; index 0 is the root
    std::size_t words_ = 0;
};

}  // namespace cx
