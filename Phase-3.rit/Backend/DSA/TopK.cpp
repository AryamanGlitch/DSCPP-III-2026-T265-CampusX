#include "TopK.h"

#include <algorithm>
#include <string>

namespace cx {

template <typename T>
TopK<T>::TopK(std::size_t k) : k_(k) {}

template <typename T>
void TopK<T>::offer(double score, long long tie, T item) {
    if (k_ == 0) return;
    Entry e{score, tie, std::move(item)};
    if (heap_.size() < k_) {
        heap_.push(std::move(e));
    } else if (Worse()(heap_.top(), e)) {
        heap_.pop();
        heap_.push(std::move(e));
    }
}

template <typename T>
std::size_t TopK<T>::size() const { return heap_.size(); }

template <typename T>
std::vector<std::pair<T, double>> TopK<T>::takeSorted() {
    std::vector<std::pair<T, double>> out;
    out.reserve(heap_.size());
    while (!heap_.empty()) {
        Entry e = heap_.pop();
        out.emplace_back(std::move(e.item), e.score);
    }
    std::reverse(out.begin(), out.end());  // heap pops worst first
    return out;
}

// ---- Explicit instantiations (each also needs its heap in MinHeap.cpp) ----
template class TopK<int>;          // SearchService::rank, tests
template class TopK<std::size_t>;  // FeedService: hot posts
template class TopK<std::string>;  // FeedService: trending hashtags

}  // namespace cx
