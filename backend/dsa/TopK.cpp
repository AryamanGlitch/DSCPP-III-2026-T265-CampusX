#pragma once
// Top-K selection with a MIN-heap of size K.
// The heap root is the WORST of the current best K, so a new item only
// enters if it beats the root. Cost: O(n log k) instead of O(n log n)
// for a full sort. Used for search ranking and trending posts/hashtags.
#include <algorithm>
#include <vector>

#include "MinHeap.h"

namespace cx {

template <typename T>
class TopK {
public:
    explicit TopK(std::size_t k) : k_(k) {}

    // `tie` breaks equal scores: the smaller tie value wins.
    void offer(double score, long long tie, T item) {
        if (k_ == 0) return;
        Entry e{score, tie, std::move(item)};
        if (heap_.size() < k_) {
            heap_.push(std::move(e));
        } else if (Worse()(heap_.top(), e)) {
            heap_.pop();
            heap_.push(std::move(e));
        }
    }

    std::size_t size() const { return heap_.size(); }

    // Best first. Empties the structure.
    std::vector<std::pair<T, double>> takeSorted() {
        std::vector<std::pair<T, double>> out;
        out.reserve(heap_.size());
        while (!heap_.empty()) {
            Entry e = heap_.pop();
            out.emplace_back(std::move(e.item), e.score);
        }
        std::reverse(out.begin(), out.end());  // heap pops worst first
        return out;
    }

private:
    struct Entry {
        double score;
        long long tie;
        T item;
    };
    // "a is worse than b": lower score, or same score with a larger tie value.
    struct Worse {
        bool operator()(const Entry& a, const Entry& b) const {
            if (a.score != b.score) return a.score < b.score;
            return a.tie > b.tie;
        }
    };

    std::size_t k_;
    MinHeap<Entry, Worse> heap_;
};

}  // namespace cx
