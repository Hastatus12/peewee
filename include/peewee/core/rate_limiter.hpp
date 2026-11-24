#pragma once
// Token bucket request limiter.
//
// The bucket holds at most `burst` tokens and refills at
// `rate_per_sec` tokens per second. Each request consumes one token;
// when empty, requests must wait. Exchanges enforce limits of
// this kind and reject requests or block clients that exceed them.

#include <algorithm>
#include <chrono>

#include "peewee/core/types.hpp"

namespace peewee {

class TokenBucket {
public:
    TokenBucket(double rate_per_sec, double burst)
        : rate_(rate_per_sec), burst_(burst), tokens_(burst) {}

    // Consume n tokens if available.
    bool try_acquire(Timestamp now, double n = 1.0) {
        refill(now);
        if (tokens_ < n) return false;
        tokens_ -= n;
        return true;
    }

    // Are n tokens available? (Does not consume them.)
    bool available(Timestamp now, double n = 1.0) const { return tokens_at(now) >= n; }

private:
    double tokens_at(Timestamp now) const {
        if (!started_) return tokens_;
        const double dt = std::chrono::duration<double>(now - last_).count();
        return std::min(burst_, tokens_ + dt * rate_);
    }

    void refill(Timestamp now) {
        tokens_ = tokens_at(now);
        last_ = now;
        started_ = true;
    }

    double rate_;
    double burst_;
    double tokens_;
    Timestamp last_{};
    bool started_ = false;
};

}  // namespace peewee
