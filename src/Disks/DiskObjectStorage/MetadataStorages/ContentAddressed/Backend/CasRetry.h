#pragma once
#include <cstdint>
#include <optional>

namespace DB::Cas
{

/// A retry policy for one logical CAS write, expressed WITHOUT touching a clock: `window_ms` is the
/// policy's own budget measured from the call's start, and `lease_deadline_ms` (already reduced by
/// the caller's safety margin) is an absolute bound on whatever clock the caller's mount lease is
/// tracked against. Binding a policy to an absolute deadline is deferred to `bind`, which takes the
/// caller's own `now_ms` -- `CasRequests` runs on an injected clock in tests, so a `Retry` built at
/// construction time from the real clock could never be exercised deterministically, and `GaveUp`
/// could not tell a lease-caused deadline from a policy-caused one without recording which bound won.
struct Retry
{
    uint64_t window_ms;
    std::optional<uint64_t> lease_deadline_ms;
    bool single_attempt;
    /// An ABSOLUTE bound on the caller's own clock, replacing `window_ms` in `bind`. A hand-written
    /// loop freezes one before it starts (`CasOperation::freeze`) and shares it across every call it
    /// makes, so the loop as a whole ends when that deadline does instead of granting each of its
    /// iterations a fresh window. Empty for a single verb, which gets its window from where it is
    /// called.
    std::optional<uint64_t> policy_deadline_ms = std::nullopt;

    /// Full jitter: uniform(0, min(5000, 200 << (attempt-1))) milliseconds. `attempt` is 1-based;
    /// `attempt == 0` returns 0.
    static uint64_t backoff(uint32_t attempt);

    /// A policy with `ms` milliseconds of its own budget and no lease bound.
    static Retry within(uint64_t ms) { return {.window_ms = ms, .lease_deadline_ms = std::nullopt, .single_attempt = false}; }
    /// `within(90'000)` -- the default write policy.
    static Retry standard() { return within(90'000); }
    /// The standard policy, additionally bound by the mount lease: `lease_deadline_ms` minus
    /// `margin`, clamped at 0 -- never risk a write landing after this node's fence may already be
    /// gone.
    static Retry untilLeaseSafe(uint64_t lease_deadline_ms, uint64_t margin)
    {
        return {.window_ms = 90'000,
                .lease_deadline_ms = lease_deadline_ms > margin ? lease_deadline_ms - margin : 0,
                .single_attempt = false};
    }
    /// The standard policy, but at most one attempt is ever sent.
    static Retry once() { return {.window_ms = 90'000, .lease_deadline_ms = std::nullopt, .single_attempt = true}; }

    /// This policy, made single-attempt. A frozen loop policy keeps its absolute deadline through it,
    /// which is what lets a loop send an unrepeatable request under the same bound as the rest.
    Retry asSingleAttempt() const
    {
        Retry copy = *this;
        copy.single_attempt = true;
        return copy;
    }

    /// A policy bound to an absolute deadline on the caller's own clock, plus which bound produced
    /// it -- the caller's own budget, or the (smaller) lease bound.
    struct Bound
    {
        uint64_t deadline_ms;
        bool lease_bound;
    };
    /// Bind this policy to `now_ms`: `deadline_ms = min(policy_deadline_ms ?: now_ms + window_ms,
    /// lease_deadline_ms)`, `lease_bound` true exactly when the lease bound was the smaller of the
    /// two. Called once at call entry with the owner's `now_ms()`. A frozen policy therefore keeps
    /// the lease bound honest: the smaller of the two still wins, and still says so.
    Bound bind(uint64_t now_ms) const;
};

}
