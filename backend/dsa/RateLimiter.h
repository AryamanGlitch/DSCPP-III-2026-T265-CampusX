#pragma once
// Sliding-window rate limiter: for each key (client IP) keep a queue of
// request timestamps; drop timestamps older than the window, then allow the
// request only if fewer than `limit` remain. Exact, unlike fixed windows,
// which allow up to 2x the limit at a window boundary.
#include <deque>
#include <mutex>
#include <string>
#include <unordered_map>

namespace cx {

class RateLimiter {
public:
    RateLimiter(std::size_t limit, long long windowMs) : limit_(limit), windowMs_(windowMs) {}

    bool allow(const std::string& key, long long nowMs) {
        std::lock_guard<std::mutex> lock(mu_);
        auto& q = hits_[key];
        while (!q.empty() && q.front() <= nowMs - windowMs_) q.pop_front();
        if (q.size() >= limit_) return false;
        q.push_back(nowMs);
        if (hits_.size() > 10000) sweep(nowMs);  // stop the map growing forever
        return true;
    }

private:
    void sweep(long long nowMs) {
        for (auto it = hits_.begin(); it != hits_.end();) {
            auto& q = it->second;
            while (!q.empty() && q.front() <= nowMs - windowMs_) q.pop_front();
            it = q.empty() ? hits_.erase(it) : std::next(it);
        }
    }

    std::size_t limit_;
    long long windowMs_;
    std::mutex mu_;
    std::unordered_map<std::string, std::deque<long long>> hits_;
};

}  // namespace cx
