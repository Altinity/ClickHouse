#pragma once
#include <cstdint>
#include <limits>
#include <optional>

namespace DB::Cas
{

/// The window of the default write policy.
inline constexpr uint64_t kStandardWriteWindowMs = 90'000;

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
    /// When set, a reissue in `CasOperation`'s write and read loops waits until this long (drawn in
    /// [0.8, 1.2] of it) has passed since the failed request started, instead of the engine's backoff.
    /// A request that took longer is reissued at once. The first-attempt fuse is reissued at once either way.
    std::optional<uint64_t> attempt_spacing_ms = std::nullopt;

    /// Full jitter: uniform(0, min(5000, 200 << (attempt-1))) milliseconds. `attempt` is 1-based;
    /// `attempt == 0` returns 0.
    static uint64_t backoff(uint32_t attempt);

    /// The flat pause after a clean lost race: `backoff(1)`, uniform over [0, 200] ms. It does not grow
    /// with the writer's loss count, because a settled conflict has nothing to wait for but
    /// desynchronisation from its competitors, and growing it with the loss count made the oldest
    /// loser the slowest and the likeliest to lose again. A conflict that settled a transport fault
    /// keeps `backoff(attempt)`: the fault is what must pace the loop.
    static uint64_t conflictBackoff() { return backoff(1); }

    /// A policy with `ms` milliseconds of its own budget and no lease bound.
    static Retry within(uint64_t ms) { return {.window_ms = ms, .lease_deadline_ms = std::nullopt, .single_attempt = false}; }
    /// `within(kStandardWriteWindowMs)` -- the default write policy.
    static Retry standard() { return within(kStandardWriteWindowMs); }
    /// A policy bound by the mount lease: `lease_deadline_ms` minus `margin`, clamped at 0 -- never
    /// risk a write landing after this node's fence may already be gone. `window_ms` defaults to the
    /// default write window, `kStandardWriteWindowMs`; a caller whose own budget is deliberately much smaller (the
    /// graceful-shutdown farewell, whose window is derived from what ONE write costs, not from the
    /// standard policy) passes its own window explicitly, and `bind` still takes whichever of the two
    /// bounds is smaller.
    static Retry untilLeaseSafe(uint64_t lease_deadline_ms, uint64_t margin, uint64_t window_ms = kStandardWriteWindowMs)
    {
        return {.window_ms = window_ms,
                .lease_deadline_ms = lease_deadline_ms > margin ? lease_deadline_ms - margin : 0,
                .single_attempt = false};
    }
    /// The standard policy, but at most one attempt is ever sent.
    static Retry once() { return {.window_ms = kStandardWriteWindowMs, .lease_deadline_ms = std::nullopt, .single_attempt = true}; }

    /// No window and no lease bound: retried until a definitive answer, or until the fence or the
    /// caller's liveness ends it. `bind` saturates, so the only horizon is the range of the clock.
    static Retry untilDefinitive(uint64_t attempt_spacing_ms_)
    {
        return {.window_ms = std::numeric_limits<uint64_t>::max(), .lease_deadline_ms = std::nullopt,
                .single_attempt = false, .attempt_spacing_ms = attempt_spacing_ms_};
    }

    /// The pause before a retry under `attempt_spacing_ms`: `spacing_draw_ms` minus what the failed
    /// request took, not below 0. A clock sample before the start counts as no time taken.
    static uint64_t spacedPause(uint64_t spacing_draw_ms, uint64_t request_started_ms, uint64_t now_ms);
    /// A uniform draw in [0.8, 1.2] of `attempt_spacing_ms_`, saturating at the top of the range.
    static uint64_t drawSpacing(uint64_t attempt_spacing_ms_);

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
