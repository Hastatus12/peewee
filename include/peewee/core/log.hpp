#pragma once
// Minimal stderr logging. In production, replace it with an asynchronous
// logger (e.g. Quill): writing to files/console on the engine thread
// introduces unpredictable latency.
//
// Every line starts with the local wall-clock time, to the millisecond:
// [peewee 22:42:31.512] hyperliquid: connection lost ...

#include <chrono>
#include <cstdio>
#include <ctime>

namespace peewee::detail {

struct LogTime {
    char text[16];  // "HH:MM:SS.mmm"
};

inline LogTime log_time() {
    const auto now = std::chrono::system_clock::now();
    const std::time_t seconds = std::chrono::system_clock::to_time_t(now);
    const auto ms =
        std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count() % 1000;
    std::tm local{};
    localtime_r(&seconds, &local);
    LogTime t{};
    std::snprintf(t.text, sizeof(t.text), "%02d:%02d:%02d.%03d", local.tm_hour, local.tm_min, local.tm_sec,
                  static_cast<int>(ms));
    return t;
}

}  // namespace peewee::detail

// One fprintf per line, so lines from different threads do not mix.
#define PEEWEE_LOG(fmt, ...) \
    std::fprintf(stderr, "[peewee %s] " fmt "\n", ::peewee::detail::log_time().text __VA_OPT__(,) __VA_ARGS__)
