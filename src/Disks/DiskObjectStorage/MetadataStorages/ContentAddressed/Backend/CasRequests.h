#pragma once
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Backend/CasBackend.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Backend/CasFence.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Backend/CasIncarnation.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Backend/CasRetry.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Backend/CasWriteResult.h>
#include <IO/ReadBuffer.h>
#include <base/types.h>

#include <cstdint>
#include <exception>
#include <functional>
#include <memory>
#include <optional>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

namespace DB::Cas
{

/// TRUE when the store's own answer proves this write never applied: a malformed request, an entity
/// too large, an access denial, or a credential failure. Whether a STALE CREDENTIAL explains it is not
/// asked here -- the engine asks the backend for fresh credentials first, and refuses only when no
/// refresh HELPED: the error was outside the credential class, the one refresh this call is allowed
/// installed nothing, or there is no reissue left to sign with what it did install. A refresh that
/// helps re-sends the attempt, which is known not to have applied. A non-S3 exception is never a
/// refusal: an unmodeled error may have landed.
bool isDefinitelyRefusedWrite(const std::exception & e);

/// Deterministic caller/local bugs, surfaced unchanged by every loop here: reissuing only replays the
/// same failure and buries the root cause behind a retryable exception. The set is `LOGICAL_ERROR`,
/// `NOT_IMPLEMENTED`, `BAD_ARGUMENTS` and `CORRUPTED_DATA`.
bool isDeterministicLocalFailure(int code);

/// Facts the fence cannot see, sampled by the caller. Non-throwing; FALSE ends the operation exactly
/// like a lost fence, because the engine does not need to know which of the two refused.
using Liveness       = std::function<bool()>;
/// `decide` sees the current object (or absence) and returns the bytes to write, or nullopt for
/// "nothing to do". It may throw: the exception is the caller's control flow and propagates unchanged.
using DecideOnObject = std::function<std::optional<String>(const std::optional<Object> &)>;
using DecideOnMeta   = std::function<std::optional<String>(const std::optional<Meta> &)>;

/// One key returned by `list`. `incarnation` is present only on a backend that surfaces per-key
/// incarnations through LIST -- see `Backend::supportsListTokens`.
struct KeyEntry
{
    String key;
    uint64_t size;
    std::optional<Incarnation> incarnation;
};
/// One page of an enumeration. `next_cursor` resumes strictly after the last returned key; empty
/// marks the end.
struct KeyPage
{
    std::vector<KeyEntry> keys;
    String next_cursor;
};
/// The walk's callback: FALSE stops the walk.
using KeyEntryFn = std::function<bool(const KeyEntry &)>;

namespace detail
{
/// The engine's per-attempt counters, behind functions so the header need not declare the events.
void recordAttempt();
void recordReissue();
}

class CasOperation;

/// The only caller of `Backend`. It owns the three things a physical request must be measured
/// against -- the transport, the mount fence, and the clock -- and it is the sole minter of
/// `Incarnation`, so a caller can hold one only by way of a request this class admitted.
///
/// It is constructed with a fence because a fence is a property of whoever holds the lease, not of a
/// call: the mount plane passes the mount fence, the GC plane and the offline tools an open one. A
/// verb is never called here; verbs live on `CasOperation`, which carries the generation the caller
/// was admitted under.
class CasRequests
{
public:
    /// `now_ms` defaults to `CLOCK_BOOTTIME` milliseconds -- the same clock a mount lease deadline is
    /// expressed on, so `Retry::untilLeaseSafe` and this engine compare like with like. `sleep_ms`
    /// defaults to a real sleep. `attempt_reservation_ms` is taken from the backend's own attempt
    /// timeout: it is what the engine reserves before it starts anything.
    CasRequests(BackendPtr backend_, Fence fence_,
                std::function<uint64_t()> now_ms_ = {}, std::function<void(uint64_t)> sleep_ms_ = {});

    /// Admitted now, under the fence's current generation.
    CasOperation admit(Liveness liveness = {});
    /// Admitted earlier: the generation came from a persisted runtime record, and an operation that
    /// resumes under a generation the fence has since moved past gives up rather than writing.
    CasOperation resume(uint64_t admitted_generation, Liveness liveness = {});

    /// The capability predicates and `dialect()`.
    Backend & backendForCapabilityPredicates() { return *backend; }

    void setNowFnForTest(std::function<uint64_t()> now_ms_);
    void setSleepFnForTest(std::function<void(uint64_t)> sleep_ms_);
    void setAttemptReservationForTest(uint64_t ms) { attempt_reservation_ms = ms; }

private:
    friend class CasOperation;

    /// The one place a transport key is created. Every verb reaches the store through this, so no
    /// engine code -- and nothing outside it -- can name the key's type, let alone construct one.
    template <typename Fn>
    auto withTransportAccess(Fn && fn)
    {
        TransportAccess access;
        return std::forward<Fn>(fn)(access);
    }

    /// The store's answer for `key`, as an incarnation. Throws `CORRUPTED_DATA` naming the key when
    /// the value fails this backend's dialect grammar.
    Incarnation mint(const String & key, String value) const;
    /// `mint` without the verdict, for the one caller that must treat a malformed value as an
    /// ambiguity to settle by reading rather than as corruption: a write's own 2xx response.
    std::optional<Incarnation> tryMint(const String & key, String value) const;
    /// The transport value to send as a precondition. Throws `LOGICAL_ERROR` when the incarnation
    /// names another key or another backend -- a precondition built from it would silently mean
    /// something else.
    const String & valueFor(const String & key, const Incarnation & inc) const;

    BackendPtr backend;
    Fence fence;
    std::function<uint64_t()> now_ms;
    std::function<void(uint64_t)> sleep_ms;
    uint64_t attempt_reservation_ms;
};

/// One admitted operation: the unit a policy, a fence generation and a liveness predicate apply to.
/// Move-only, and every request it makes re-checks its admission -- before each attempt, before each
/// sleep, and once more after a proven commit, so a write whose fence was lost while it was in flight
/// is never reported as committed.
class CasOperation
{
public:
    CasOperation(CasOperation &&) = default;
    CasOperation(const CasOperation &) = delete;
    CasOperation & operator=(const CasOperation &) = delete;

    uint64_t generation() const { return admitted_generation; }
    /// The verdict point: is this operation still admitted? For the sites that guard a decision rather
    /// than a request.
    bool admitted() const { return gate(0) == Gate::Ok; }

    std::optional<Object> read(const String & key, const Retry & policy);
    std::optional<Meta>   head(const String & key, const Retry & policy);
    KeyPage               list(const String & prefix, const String & cursor, size_t limit, const Retry & policy);
    /// Walks every key under `prefix` exactly once. The policy governs EACH PAGE, not the walk: a walk
    /// is an unbounded number of requests, and a silently truncated enumeration is the error a
    /// coverage record exists to prevent. `on_page_fetched` fires once per page DELIVERED; a page that
    /// took several reissues still fires once, and `CASRequestAttempt` is the physical count.
    void forEachListedKey(const String & prefix, const KeyEntryFn & fn, const Retry & per_page,
                          size_t page_limit = 1000, const std::function<void()> & on_page_fetched = {});
    Removal remove(const String & key, const Incarnation & seen, const Retry & policy);
    /// `head` then `remove` of what it saw, repeating on `Mismatch`. `Gone` when the key is already
    /// absent; never returns `Mismatch` -- under `once`, where there is no reissue to resolve one, a
    /// `Mismatch` is the retry-later throw the read verbs use when their policy is exhausted.
    Removal removeCurrent(const String & key, const Retry & policy);
    /// The one primitive that reports failure as a value, so the policy reissues on the OUTCOME:
    /// `Indeterminate` is retried, the four authoritative outcomes return at once, and an
    /// `Indeterminate` that outlives the bound is returned rather than thrown. Admission refused before
    /// the first attempt still throws -- nothing was probed.
    SentinelProbeResult probeSentinel(const String & key, const Retry & policy);
    /// The OPEN is under the policy; the body is the SDK's.
    std::unique_ptr<ReadBuffer> stream(const String & key, const Retry & policy);
    /// The INITIATION is under the policy; the transfer is the SDK's.
    void publish(const BlobPublishRequest & request, const Retry & policy);

    WriteResult create(const String & key, const String & bytes, const Retry & policy);
    WriteResult replace(const String & key, const String & bytes, const Incarnation & seen, const Retry & policy);
    /// Read, decide, write, and re-decide on conflict against what the write's own resolve read
    /// already observed. `decide` returning nullopt is `Declined`.
    WriteResult readModifyWrite(const String & key, const DecideOnObject & decide, const Retry & policy);
    /// The same loop over `head`, settling a refused precondition with a `head` too. Proving that an
    /// ambiguous attempt landed needs the bytes, so that one path does read a body; either way the verb
    /// reports a `Meta` and never an `Object`.
    WriteResult readModifyWriteOnPresence(const String & key, const DecideOnMeta & decide, const Retry & policy);

private:
    friend class CasRequests;

    CasOperation(CasRequests & owner_, uint64_t admitted_generation_, Liveness liveness_)
        : owner(owner_), admitted_generation(admitted_generation_), liveness(std::move(liveness_))
    {
    }

    enum class Gate : uint8_t { Ok, FenceLost, NoBudget };
    /// The admission point: the fence for `needed_ms` from now, then the caller's own facts.
    Gate gate(uint64_t needed_ms) const;

    /// Everything ONE logical write call accumulates. It outlives each attempt, and for
    /// `readModifyWrite` it outlives each inner write, so a `GaveUp` reports what the whole call did
    /// rather than what its last attempt did.
    struct WriteState
    {
        uint32_t attempts_sent = 0;
        bool sent_any = false;
        /// Did ANY attempt of this call end without proof of whether it applied? A credential answer
        /// does not qualify: the store gives it before applying anything.
        bool any_ambiguous = false;
        Observation last_seen = NotObserved{};
        uint32_t reissues = 0;
        bool refresh_attempted = false;
    };

    /// Why a read-class request stopped without an answer. Every give-up below throws the same
    /// `NETWORK_ERROR`, so a caller that SWALLOWS the exception -- only the resolve read does -- cannot
    /// recover from it which bound refused, and reporting a lease refusal as a policy deadline is the
    /// confusion `GaveUp::Source` exists to prevent. `PolicyExhausted` names the `Retry` bound, whose
    /// own source is `Bound::lease_bound`.
    enum class ReadStop : uint8_t { FenceLost, NoBudgetLease, PolicyExhausted };

    /// What the resolve read saw, and why it stopped when it saw nothing. `stop` is set ONLY when a
    /// bound refused; a read that failed at the transport leaves it empty, and that is the one case
    /// `NotObserved` is still the whole story.
    struct Resolved
    {
        Observation seen;
        std::optional<ReadStop> stop;
    };

    /// One read-class request under the policy: admission, attempt, classification, jittered reissue.
    /// Returns whatever `once` returns, or throws -- the read surface reports failure by exception.
    template <typename Fn>
    auto readLoop(std::string_view verb, const String & subject, const Retry & policy,
                  const Retry::Bound & bound, Fn && once);

    std::optional<Object> readUnder(const String & key, const Retry & policy, const Retry::Bound & bound);
    std::optional<Meta>   headUnder(const String & key, const Retry & policy, const Retry::Bound & bound);
    KeyPage               listUnder(const String & prefix, const String & cursor, size_t limit,
                                    const Retry & policy, const Retry::Bound & bound);
    Removal               removeUnder(const String & key, const String & expected_value,
                                      const Retry & policy, const Retry::Bound & bound);

    /// How a write settles a REFUSED PRECONDITION, which needs only to know what is at the key. An
    /// ambiguous attempt always reads the body, whichever this says, because only the bytes can prove
    /// the attempt landed.
    enum class ResolveWith : uint8_t { Body, Presence };

    /// The write engine: one call, any policy. Settles every refused precondition and every ambiguity
    /// by an exact read before it reports anything.
    WriteResult writeLoop(const String & key, const String & bytes, const std::optional<Incarnation> & expected,
                          const Retry & policy, const Retry::Bound & bound, WriteState & state,
                          ResolveWith resolve_refusal_with);
    /// The resolve read: an exact read under the same policy and deadline, reporting what it saw and,
    /// when it saw nothing, which bound stopped it.
    Resolved observe(const String & key, const Retry & policy, const Retry::Bound & bound);
    /// The presence-only sibling, for the one loop that must not fetch a body.
    Resolved observePresence(const String & key, const Retry & policy, const Retry::Bound & bound);

    WriteResult postCommit(Incarnation inc, bool resolved_by_read, WriteState & state, const Retry::Bound & bound);
    WriteResult gaveUp(GaveUp::Why why, GaveUp::Source source, WriteState & state) const;
    /// The bound that refused the resolve read, reported as the outcome it actually is.
    WriteResult gaveUpForReadStop(ReadStop stop, WriteState & state, const Retry::Bound & bound) const;
    /// A resolve read that produced nothing. The fence is NOT resampled: a second sample reports a
    /// state the read never saw, which is how a lease refusal used to be reported as a policy deadline.
    WriteResult gaveUpAfterFailedObservation(std::optional<ReadStop> stop, WriteState & state,
                                             const Retry::Bound & bound) const;
    /// Admission, then the jittered sleep. A value means the call ended during it; nullopt means the
    /// caller may send another attempt.
    std::optional<WriteResult> pauseAndReissue(WriteState & state, const Retry::Bound & bound);

    /// `sleep_ms` plus `envelopes` attempt reservations, saturating.
    uint64_t reservedFor(uint64_t sleep_ms, uint32_t envelopes) const;
    /// Is there room to START something needing `needed_ms` before the bound? The guarantee is on the
    /// start side: nothing is begun that could not finish inside it.
    bool fits(uint64_t needed_ms, const Retry::Bound & bound) const;

    /// One failed read-class attempt, classified. A credential failure is refreshed HERE so the reissue
    /// signs with the new client, at most once per call -- `refresh_attempted` is the caller's, and a
    /// second credential failure under the same call is classified as if no refresh were available.
    /// TRUE means the failure must surface unchanged.
    bool refreshAndClassifyReadFault(const std::exception & e, bool & refresh_attempted);

    /// Each records its cause in `last_read_stop` before throwing, so the resolve read can report it.
    [[noreturn]] void giveUpReadFenceLost(std::string_view verb, const String & subject, std::string_view when);
    [[noreturn]] void giveUpReadNoBudget(std::string_view verb, const String & subject, std::string_view what);
    [[noreturn]] void giveUpReadDeadline(std::string_view verb, const String & subject,
                                         const Retry::Bound & bound, uint32_t attempts_made);

    CasRequests & owner;
    uint64_t admitted_generation;
    Liveness liveness;
    /// Written immediately before a read-class give-up throws, cleared and read only by the resolve
    /// read that swallows it. Every other caller lets the exception carry the verdict.
    std::optional<ReadStop> last_read_stop;
};

template <typename Fn>
auto CasOperation::readLoop(std::string_view verb, const String & subject, const Retry & policy,
                            const Retry::Bound & bound, Fn && once)
{
    bool refresh_attempted = false;
    for (uint32_t attempt = 1;; ++attempt)
    {
        const uint64_t reservation = reservedFor(0, 1);
        switch (gate(reservation))
        {
            case Gate::FenceLost: giveUpReadFenceLost(verb, subject, "before the request");
            case Gate::NoBudget:  giveUpReadNoBudget(verb, subject, "for one more request");
            case Gate::Ok: break;
        }
        if (!fits(reservation, bound))
            giveUpReadDeadline(verb, subject, bound, attempt - 1);

        detail::recordAttempt();
        try
        {
            return owner.withTransportAccess([&](auto & access) { return once(access); });
        }
        catch (const std::exception & e)
        {
            if (refreshAndClassifyReadFault(e, refresh_attempted) || policy.single_attempt)
                throw;
        }

        const uint64_t pause_ms = Retry::backoff(attempt);
        const uint64_t needed = reservedFor(pause_ms, 1);
        switch (gate(needed))
        {
            case Gate::FenceLost: giveUpReadFenceLost(verb, subject, "before the reissue");
            case Gate::NoBudget:  giveUpReadNoBudget(verb, subject, "for the reissue");
            case Gate::Ok: break;
        }
        if (!fits(needed, bound))
            giveUpReadDeadline(verb, subject, bound, attempt);
        detail::recordReissue();
        owner.sleep_ms(pause_ms);
    }
}

}
