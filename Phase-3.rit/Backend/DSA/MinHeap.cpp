#include "MinHeap.h"

#include <stdexcept>
#include <string>
#include <utility>

#include "Graph.h"  // QItem (Dijkstra's heap entry)
#include "TopK.h"   // TopK<T>::Entry / TopK<T>::Worse

namespace cx {

template <typename T, typename Less>
MinHeap<T, Less>::MinHeap(Less less) : less_(less) {}

template <typename T, typename Less>
bool MinHeap<T, Less>::empty() const { return data_.empty(); }

template <typename T, typename Less>
std::size_t MinHeap<T, Less>::size() const { return data_.size(); }

template <typename T, typename Less>
const T& MinHeap<T, Less>::top() const {
    if (data_.empty()) throw std::out_of_range("MinHeap::top on empty heap");
    return data_.front();
}

template <typename T, typename Less>
void MinHeap<T, Less>::push(T value) {
    data_.push_back(std::move(value));
    siftUp(data_.size() - 1);
}

template <typename T, typename Less>
T MinHeap<T, Less>::pop() {
    if (data_.empty()) throw std::out_of_range("MinHeap::pop on empty heap");
    T result = std::move(data_.front());
    data_.front() = std::move(data_.back());
    data_.pop_back();
    if (!data_.empty()) siftDown(0);
    return result;
}

template <typename T, typename Less>
void MinHeap<T, Less>::siftUp(std::size_t i) {
    while (i > 0) {
        std::size_t parent = (i - 1) / 2;
        if (!less_(data_[i], data_[parent])) break;
        std::swap(data_[i], data_[parent]);
        i = parent;
    }
}

template <typename T, typename Less>
void MinHeap<T, Less>::siftDown(std::size_t i) {
    const std::size_t n = data_.size();
    for (;;) {
        std::size_t smallest = i;
        std::size_t l = 2 * i + 1, r = 2 * i + 2;
        if (l < n && less_(data_[l], data_[smallest])) smallest = l;
        if (r < n && less_(data_[r], data_[smallest])) smallest = r;
        if (smallest == i) return;
        std::swap(data_[i], data_[smallest]);
        i = smallest;
    }
}

// ---- Explicit instantiations: every element type the project uses ----
// Naming TopK's private Entry/Worse here is allowed: access checks do not
// apply to explicit instantiations.
template class MinHeap<int>;     // tests: min_heap_sorts_like_std_sort
template class MinHeap<QItem>;   // Graph: dijkstra, buildRoutingTable
template class MinHeap<TopK<int>::Entry, TopK<int>::Worse>;
template class MinHeap<TopK<std::size_t>::Entry, TopK<std::size_t>::Worse>;
template class MinHeap<TopK<std::string>::Entry, TopK<std::string>::Worse>;

}  // namespace cx
