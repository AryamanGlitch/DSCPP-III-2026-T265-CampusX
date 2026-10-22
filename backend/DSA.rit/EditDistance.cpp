#pragma once
// Levenshtein distance (insert / delete / replace = 1) with an upper bound.
// Dynamic programming over two rows: O(n*m) time, O(m) memory.
// Stops early once every cell in a row exceeds maxDist, because the
// distance can only grow from there.
// The search engine uses the trie-based version in Trie.cpp; this one is
// the simple reference implementation the tests compare it against.
#include <algorithm>
#include <cstdlib>
#include <string>
#include <vector>

namespace cx {

inline int boundedEditDistance(const std::string& a, const std::string& b, int maxDist) {
    const int n = static_cast<int>(a.size()), m = static_cast<int>(b.size());
    if (std::abs(n - m) > maxDist) return maxDist + 1;

    std::vector<int> prev(m + 1), cur(m + 1);
    for (int j = 0; j <= m; ++j) prev[j] = j;

    for (int i = 1; i <= n; ++i) {
        cur[0] = i;
        int rowMin = cur[0];
        for (int j = 1; j <= m; ++j) {
            int replace = prev[j - 1] + (a[i - 1] != b[j - 1] ? 1 : 0);
            int del = prev[j] + 1;
            int ins = cur[j - 1] + 1;
            cur[j] = std::min({replace, del, ins});
            rowMin = std::min(rowMin, cur[j]);
        }
        if (rowMin > maxDist) return maxDist + 1;
        std::swap(prev, cur);
    }
    return std::min(prev[m], maxDist + 1);
}

}  // namespace cx
