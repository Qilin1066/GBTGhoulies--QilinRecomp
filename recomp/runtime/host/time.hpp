#pragma once
// Arithmetic shared by NT waits and timers. Guest intervals are 100 ns ticks;
// positive values are system time since 1601, negative ones are relative.
#include <chrono>
#include <cstdint>
#include <limits>

namespace qilin::host::time {
using Clock = std::chrono::steady_clock;
inline constexpr uint64_t epoch = 116444736000000000ULL;
inline uint64_t system_ticks() {
    using Ticks = std::chrono::duration<int64_t, std::ratio<1, 10000000>>;
    return uint64_t(std::chrono::duration_cast<Ticks>(std::chrono::system_clock::now().time_since_epoch()).count()) + epoch;
}
inline uint64_t interval_ticks(int64_t due, uint64_t system_now) {
    if (due < 0) return uint64_t{0} - uint64_t(due); // also INT64_MIN
    return uint64_t(due) > system_now ? uint64_t(due) - system_now : 0;
}
inline uint64_t add(uint64_t at, uint64_t ticks) {
    return ticks > UINT64_MAX - at ? UINT64_MAX : at + ticks;
}
inline Clock::time_point deadline(Clock::time_point now, uint64_t ticks) {
    if (ticks > uint64_t(INT64_MAX) / 100) return Clock::time_point::max();
    const auto delta = std::chrono::duration_cast<Clock::duration>(std::chrono::nanoseconds(int64_t(ticks * 100)));
    return now > Clock::time_point::max() - delta ? Clock::time_point::max() : now + delta;
}
// Keep a periodic timer's phase, but coalesce missed expirations after a stall.
inline uint64_t next_period(uint64_t due, uint64_t period, uint64_t now) {
    if (!period || due > now) return due;
    return add(now, period - (now - due) % period);
}
} // namespace qilin::host::time
