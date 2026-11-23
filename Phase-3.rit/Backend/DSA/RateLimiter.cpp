#include "RateLimiter.h"

#include <iterator>

namespace cx {

RateLimiter::RateLimiter(std::size_t limit, long long windowMs)
    : limit_(limit), windowMs_(windowMs) {}

bool RateLimiter::allow(const std::string& key, long long nowMs) {
    std::lock_guard<std::mutex> lock(mu_);
    auto& q = hits_[key];
    while (!q.empty() && q.front() <= nowMs - windowMs_) q.pop_front();
    if (q.size() >= limit_) return false;
    q.push_back(nowMs);
    if (hits_.size() > 10000) sweep(nowMs);  // stop the map growing forever
    return true;
}

// Caller must hold mu_.
void RateLimiter::sweep(long long nowMs) {
    for (auto it = hits_.begin(); it != hits_.end();) {
        auto& q = it->second;
        while (!q.empty() && q.front() <= nowMs - windowMs_) q.pop_front();
        it = q.empty() ? hits_.erase(it) : std::next(it);
    }
}

}  // namespace cx
