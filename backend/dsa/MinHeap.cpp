#pragma once
// Binary min-heap stored in an array.
//   parent(i) = (i-1)/2, left(i) = 2i+1, right(i) = 2i+2
//   push / pop: O(log n), top: O(1)
// Used by Dijkstra (closest unvisited node) and TopK (keep best K results).
#include <functional>
#include <stdexcept>
#include <utility>
#include <vector>

namespace cx {

template <typename T, typename Less = std::less<T>>
class MinHeap {
public:
    explicit MinHeap(Less less = Less()) : less_(less) {}

    bool empty() const { return data_.empty(); }
    std::size_t size() const { return data_.size(); }

    const T& top() const {
        if (data_.empty()) throw std::out_of_range("MinHeap::top on empty heap");
        return data_.front();
    }

    void push(T value) {
        data_.push_back(std::move(value));
        siftUp(data_.size() - 1);
    }

    T pop() {
        if (data_.empty()) throw std::out_of_range("MinHeap::pop on empty heap");
        T result = std::move(data_.front());
        data_.front() = std::move(data_.back());
        data_.pop_back();
        if (!data_.empty()) siftDown(0);
        return result;
    }

private:
    void siftUp(std::size_t i) {
        while (i > 0) {
            std::size_t parent = (i - 1) / 2;
            if (!less_(data_[i], data_[parent])) break;
            std::swap(data_[i], data_[parent]);
            i = parent;
        }
    }

    void siftDown(std::size_t i) {
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

    std::vector<T> data_;
    Less less_;
};

}  // namespace cx
