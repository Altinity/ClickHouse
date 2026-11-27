#pragma once
#include <cstdint>
#include <limits>
#include <optional>

namespace DB::Cas
{

/// Caps on a hand-written loop's own iterations, independent of any clock: a loop that keeps
/// deciding again on fresh reads must end even when every iteration is quick.
/// An update of a shared object that keeps losing races fails at this count with `NETWORK_ERROR` "did not converge";
/// there is no total time budget, and each request has its own window.
inline constexpr uint32_t kCasMaxDecisions = 100;
/// The walk that fails at this count propagates its own error; a restart at it fails as "did not converge".
inline constexpr uint32_t kCasMaxRecoveryWalks = 8;

/// A retry policy for one logical CAS request, expressed WITHOUT touching a clock.
///
/// Every request gets its OWN window, measured from the entry of the engine call that sends it: `window_ms`
/// if set, else the default window of the plane it runs on. A window bounds one request or one local wait
/// (the hot-key queue wait), so `cas_request_window_ms` means one step.
///
/// A loop that runs too long gets a smaller count, an owner's stop, or a time budget that decides only whether its
/// next pass starts (the copy-source confirm poll, the GC janitor's pages, the GC ref intake), never a deadline of its
/// own shared between its requests: a shared one leaves a later request no time after a slow early one and can refuse
/// the read that settles a write already sent. The only shared deadlines are absolute bounds from outside the loop,
/// below. Accepted cost: a conflict loop on a slow store can run about 100 windows. Two have neither: a paged listing
/// (`CasOperation::forEachListedKey`) runs until its callback stops it, and a request under `untilDefinitive` has no
/// window.
///
/// Two things can end a request earlier, and `bind` takes the minimum of all three:
///   - `lease_deadline_ms`: an absolute bound (already reduced by the caller's safety margin) on the clock
///     the mount lease is tracked against; `GaveUp` reports it as the lease's, and an expired window or cap as the
///     policy's.
///   - `outside_cap_ms`: an absolute bound from a caller that has a deadline of its own (an offline tool's
///     `--timeout`). Successive requests under it share it. Set it only through `cappedAt`, never by hand, and
///     never to carry a loop's own budget: a cap never lengthens a window.
/// Binding is deferred to `bind`, which takes the caller's `now_ms`: `CasRequests` runs on an injected clock
/// in tests, and `GaveUp` must tell the lease's bound from the others.
struct Retry
{
    std::optional<uint64_t> window_ms;
    std::optional<uint64_t> lease_deadline_ms;
    bool single_attempt;
    /// Physical attempts that earlier submissions of the same logical operation already sent. Only the
    /// write path's transport attempt number continues from here (so a re-decided write keeps the
    /// configured, not the adaptive first-attempt, timeouts); result and observer counts stay per call.
    uint32_t attempts_already_sent = 0;
    /// False: after a write attempt whose fate is unknown, never send the same bytes again; return
    /// `GaveUp::Unresolved` so the caller decides new bytes. For namespace-create step 1, whose catalog
    /// row must differ per attempt. A clean conflict and a credential reissue are unaffected. Read
    /// paths ignore it. Clear it only in a loop that decides again on a fresh read (`casUpdateImpl`
    /// with `CasOperation::pauseBeforeRedecide`); any other caller gets a bare `GaveUp::Unresolved`.
    bool resend_unresolved = true;
    /// An absolute bound on the caller's clock that can only shorten the request. See `cappedAt`.
    std::optional<uint64_t> outside_cap_ms = std::nullopt;
    /// When set, a reissue in `CasOperation`'s write and read loops waits until a draw from [0.8, 1.2]
    /// of this has passed since the failed attempt started, instead of the growing backoff. A
    /// first-attempt fuse timeout is still reissued at once.
    std::optional<uint64_t> attempt_spacing_ms = std::nullopt;

    static constexpr uint64_t kDefinitiveAttemptSpacingMs = 1'000;

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
    /// The default write policy: the window of the request plane it runs on.
    static Retry standard() { return {.window_ms = std::nullopt, .lease_deadline_ms = std::nullopt, .single_attempt = false}; }
    /// A policy bound by the mount lease: an attempt starts only if its reserved time ends before
    /// `lease_deadline_ms` minus `margin`, clamped at 0. That bounds when attempts start, not when they
    /// land. `window_ms` defaults to the window of the request plane; a caller whose own budget is
    /// deliberately much smaller (the graceful-shutdown farewell, whose window is derived from what ONE
    /// write costs, not from the standard policy) passes its own window explicitly, and `bind` still
    /// takes whichever of the two bounds is smaller.
    static Retry untilLeaseSafe(uint64_t lease_deadline_ms, uint64_t margin,
                                std::optional<uint64_t> window_ms = std::nullopt)
    {
        return {.window_ms = window_ms,
                .lease_deadline_ms = lease_deadline_ms > margin ? lease_deadline_ms - margin : 0,
                .single_attempt = false};
    }
    /// The standard policy, but at most one attempt is ever sent.
    static Retry once() { return {.window_ms = std::nullopt, .lease_deadline_ms = std::nullopt, .single_attempt = true}; }
    /// No window and no lease bound: the operation ends on a definitive answer, or when the fence or the
    /// caller's liveness refuses. Reissues are spaced by `kDefinitiveAttemptSpacingMs`.
    static Retry untilDefinitive()
    {
        return {.window_ms = std::numeric_limits<uint64_t>::max(), .lease_deadline_ms = std::nullopt,
                .single_attempt = false, .attempt_spacing_ms = kDefinitiveAttemptSpacingMs};
    }
    /// `spacing_draw_ms` minus what the failed attempt took, not below 0. A sample before the start
    /// counts as no time taken.
    static uint64_t spacedPause(uint64_t spacing_draw_ms, uint64_t attempt_started_ms, uint64_t now_ms);
    /// A uniform draw in [0.8, 1.2] of `attempt_spacing_ms_`, saturating at the top of the range.
    static uint64_t drawSpacing(uint64_t attempt_spacing_ms_);
    /// The pause before the next attempt under `attempt_spacing_ms`: `spacedPause` of a fresh draw.
    static uint64_t pauseAfterAttempt(uint64_t attempt_spacing_ms_, uint64_t attempt_started_ms, uint64_t now_ms)
    {
        return spacedPause(drawSpacing(attempt_spacing_ms_), attempt_started_ms, now_ms);
    }

    /// This policy ending no later than `deadline_ms` on the caller's clock; the window and the lease bound
    /// still apply, and an earlier cap stays.
    Retry cappedAt(uint64_t deadline_ms) const
    {
        Retry copy = *this;
        copy.outside_cap_ms = outside_cap_ms && *outside_cap_ms < deadline_ms ? *outside_cap_ms : deadline_ms;
        return copy;
    }

    /// A policy bound to an absolute deadline on the caller's own clock, plus which bound produced it.
    struct Bound
    {
        enum class Source : uint8_t { RequestWindow, OutsideCap, Lease };
        uint64_t deadline_ms;
        Source source;
    };
    /// `deadline_ms = min(now_ms + (window_ms ?: default_window_ms), outside_cap_ms, lease_deadline_ms)`. A bound
    /// is the `source` only when strictly smaller than the ones before it in that order.
    Bound bind(uint64_t now_ms, uint64_t default_window_ms) const;
};

}
