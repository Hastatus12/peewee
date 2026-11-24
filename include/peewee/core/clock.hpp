#pragma once
// Abstract clock: real time in production; tests and the simulator use
// a "manual" clock that you advance. Algorithms obtain time ONLY from
// this object, never directly from the system, making tests reproducible.
//
// now() is MONOTONIC: it never jumps backward, so it is safe for intervals,
// timeouts and deadlines. Use wall() only to label things with calendar time
// (logs, comparing against exchange timestamps).

#include <chrono>

#include "peewee/core/types.hpp"

namespace peewee {

class IClock {
public:
    virtual ~IClock() = default;
    // Monotonic time: use for intervals and deadlines.
    virtual Timestamp now() const = 0;
    // Wall-clock time since the Unix epoch: may jump (NTP corrections).
    virtual Timestamp wall() const = 0;
};

class RealClock final : public IClock {
public:
    Timestamp now() const override {
        return std::chrono::duration_cast<Timestamp>(
            std::chrono::steady_clock::now().time_since_epoch());
    }
    Timestamp wall() const override {
        return std::chrono::duration_cast<Timestamp>(
            std::chrono::system_clock::now().time_since_epoch());
    }
};

class ManualClock final : public IClock {
public:
    Timestamp now() const override { return now_; }
    Timestamp wall() const override { return now_; }
    void set(Timestamp t) { now_ = t; }
    void advance(Duration d) { now_ += d; }

private:
    Timestamp now_{};
};

}  // namespace peewee
