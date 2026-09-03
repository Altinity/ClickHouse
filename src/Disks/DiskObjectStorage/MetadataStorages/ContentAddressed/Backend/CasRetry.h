#pragma once
#include <cstdint>

namespace DB::Cas
{

/// A retry policy for one logical CAS write: an absolute deadline on the boot-time monotonic clock
/// (`CasMountRuntime::bootMs()`) plus whether the caller wants at most one attempt regardless of the
/// deadline.
struct Retry
{
    uint64_t deadline_ms;      /// absolute, CasMountRuntime::bootMs() clock
    bool single_attempt;

    /// Full jitter: uniform(0, min(5000, 200 << (attempt-1))) milliseconds. `attempt` is 1-based;
    /// `attempt == 0` returns 0.
    static uint64_t backoff(uint32_t attempt);
    /// A policy expiring `ms` milliseconds from now.
    static Retry within(uint64_t ms);
    /// `within(90'000)` -- the default write policy.
    static Retry standard();
    /// The standard deadline, clamped to `safety_margin_ms` before the mount lease itself expires --
    /// never risk a write landing after this node's fence may already be gone.
    static Retry untilLeaseSafe(uint64_t lease_deadline_ms, uint64_t safety_margin_ms);
    /// The standard deadline, but at most one attempt is ever sent.
    static Retry once();
};

}
