#pragma once
// Least-Recently-Used cache: hash map + hand-written doubly linked list.
//   get / put: O(1)
//   Most recently used item sits at the head; the tail is evicted when full.
// Not thread-safe on its own: the owner (SearchService) guards it with a mutex,
// because even get() reorders the list.
#include <optional>
#include <unordered_map>

namespace cx {

template <typename K, typename V>
class LRUCache {
public:
    explicit LRUCache(std::size_t capacity) : capacity_(capacity) {}
    ~LRUCache() { clear(); }
    LRUCache(const LRUCache&) = delete;
    LRUCache& operator=(const LRUCache&) = delete;

    std::optional<V> get(const K& key) {
        auto it = map_.find(key);
        if (it == map_.end()) return std::nullopt;
        moveToFront(it->second);
        return it->second->value;
    }

    void put(const K& key, V value) {
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

    void clear() {
        Node* n = head_;
        while (n) {
            Node* next = n->next;
            delete n;
            n = next;
        }
        head_ = tail_ = nullptr;
        map_.clear();
    }

    std::size_t size() const { return map_.size(); }

private:
    struct Node {
        K key;
        V value;
        Node* prev;
        Node* next;
    };

    void unlink(Node* n) {
        if (n->prev) n->prev->next = n->next; else head_ = n->next;
        if (n->next) n->next->prev = n->prev; else tail_ = n->prev;
        n->prev = n->next = nullptr;
    }

    void moveToFront(Node* n) {
        if (n == head_) return;
        unlink(n);
        n->next = head_;
        if (head_) head_->prev = n;
        head_ = n;
        if (!tail_) tail_ = n;
    }

    void evictTail() {
        Node* victim = tail_;
        if (!victim) return;
        unlink(victim);
        map_.erase(victim->key);
        delete victim;
    }

    std::size_t capacity_;
    std::unordered_map<K, Node*> map_;
    Node* head_ = nullptr;
    Node* tail_ = nullptr;
};

}  // namespace cx
