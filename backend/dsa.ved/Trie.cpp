#include "dsa/Trie.h"
#include <algorithm>
#include <utility>

namespace cx {

Trie::Trie() { clear(); }

void Trie::clear() {
    nodes_.assign(1, Node{});
    words_ = 0;
}

void Trie::insert(const std::string& word, int docId, float weight) {
    if (word.empty()) return;
    int cur = 0;
    for (unsigned char ch : word) {
        auto it = nodes_[cur].children.find(ch);
        if (it == nodes_[cur].children.end()) {
            nodes_.push_back(Node{});  // may reallocate: index, never hold references across this
            int idx = static_cast<int>(nodes_.size()) - 1;
            nodes_[cur].children[ch] = idx;
            cur = idx;
        } else {
            cur = it->second;
        }
    }
    Node& end = nodes_[cur];
    if (!end.terminal) {
        end.terminal = true;
        ++words_;
    }
    // Documents are indexed one at a time, so a repeat of the same doc is at the back.
    if (!end.postings.empty() && end.postings.back().docId == docId) {
        end.postings.back().weight = std::max(end.postings.back().weight, weight);
    } else {
        end.postings.push_back({docId, weight});
    }
}

void Trie::collectPrefix(const std::string& prefix, std::size_t maxWords, std::vector<Match>& out) const {
    int cur = 0;
    for (unsigned char ch : prefix) {
        auto it = nodes_[cur].children.find(ch);
        if (it == nodes_[cur].children.end()) return;
        cur = it->second;
    }
    // Iterative DFS so a long word can't overflow the call stack.
    std::vector<std::pair<int, std::string>> stack{{cur, prefix}};
    std::size_t found = 0;
    while (!stack.empty() && found < maxWords) {
        auto [node, word] = std::move(stack.back());
        stack.pop_back();
        const Node& n = nodes_[node];
        if (n.terminal) {
            out.push_back({word, 0, &n.postings});
            ++found;
        }
        // Push in reverse so the smallest character is explored first.
        for (auto it = n.children.rbegin(); it != n.children.rend(); ++it) {
            stack.emplace_back(it->second, word + static_cast<char>(it->first));
        }
    }
}

void Trie::fuzzy(const std::string& word, int maxDist, std::vector<Match>& out) const {
    // Row 0 of the DP table: distance from "" to each prefix of `word`.
    std::vector<int> firstRow(word.size() + 1);
    for (std::size_t i = 0; i <= word.size(); ++i) firstRow[i] = static_cast<int>(i);
    std::string path;
    for (const auto& [ch, child] : nodes_[0].children) {
        fuzzyVisit(child, ch, firstRow, word, maxDist, path, out);
    }
}

void Trie::fuzzyVisit(int node, unsigned char ch, const std::vector<int>& prevRow, const std::string& word,
                      int maxDist, std::string& path, std::vector<Match>& out) const {
    const std::size_t cols = word.size() + 1;
    std::vector<int> row(cols);
    row[0] = prevRow[0] + 1;
    int rowMin = row[0];
    for (std::size_t i = 1; i < cols; ++i) {
        int insertCost = row[i - 1] + 1;
        int deleteCost = prevRow[i] + 1;
        int replaceCost = prevRow[i - 1] + (static_cast<unsigned char>(word[i - 1]) != ch ? 1 : 0);
        row[i] = std::min({insertCost, deleteCost, replaceCost});
        rowMin = std::min(rowMin, row[i]);
    }

    path.push_back(static_cast<char>(ch));
    const Node& n = nodes_[node];
    if (n.terminal && row[cols - 1] <= maxDist) {
        out.push_back({path, row[cols - 1], &n.postings});
    }
    if (rowMin <= maxDist) {  // pruning: otherwise no word below this node can match
        for (const auto& [nextCh, child] : n.children) {
            fuzzyVisit(child, nextCh, row, word, maxDist, path, out);
        }
    }
    path.pop_back();
}

}  // namespace cx
