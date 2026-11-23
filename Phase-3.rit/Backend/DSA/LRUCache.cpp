#include "LRUCache.h"

#include <string>
#include <utility>
#include <vector>

namespace cx {

template <typename K, typename V>
LRUCache<K, V>::LRUCache(std::size_t capacity) : capacity_(capacity) {}

template <typename K, typename V>
LRUCache<K, V>::~LRUCache() { clear(); }

template <typename K, typename V>
std::optional<V> LRUCache<K, V>::get(const K& key) {
    auto it = map_.find(key);
    if (it == map_.end()) return std::nullopt;
    moveToFront(it->second);
    return it->second->value;
}

template <typename K, typename V>
void LRUCache<K, V>::put(const K& key, V value) {
    if (capacity_ == 0) return;
    auto it = map_.find(key);
    if (it != map_.end()) {
        it->second->value = std::move(value);
        moveToFront(it->second);
        return;
    }
    if (map_.size() == capacity_) evictTail();
    Node* node = new Node{key, std::move(value), nullptr, head_};
    if (head_) head_->prev = node;
    head_ = node;
    if (!tail_) tail_ = node;
    map_[key] = node;
}

template <typename K, typename V>
void LRUCache<K, V>::clear() {
    Node* n = head_;
    while (n) {
        Node* next = n->next;
        delete n;
        n = next;
    }
    head_ = tail_ = nullptr;
    map_.clear();
}

template <typename K, typename V>
std::size_t LRUCache<K, V>::size() const { return map_.size(); }

template <typename K, typename V>
void LRUCache<K, V>::unlink(Node* n) {
    if (n->prev) n->prev->next = n->next; else head_ = n->next;
    if (n->next) n->next->prev = n->prev; else tail_ = n->prev;
    n->prev = n->next = nullptr;
}

template <typename K, typename V>
void LRUCache<K, V>::moveToFront(Node* n) {
    if (n == head_) return;
    unlink(n);
    n->next = head_;
    if (head_) head_->prev = n;
    head_ = n;
    if (!tail_) tail_ = n;
}

template <typename K, typename V>
void LRUCache<K, V>::evictTail() {
    Node* victim = tail_;
    if (!victim) return;
    unlink(victim);
    map_.erase(victim->key);
    delete victim;
}

// ---- Explicit instantiations: every <K, V> the project uses ----
// SearchService's cache (its private `Ranked` alias spelled out).
template class LRUCache<std::string, std::vector<std::pair<int, double>>>;

}  // namespace cx
