#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Backend/CasRequests.h>

#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Backend/CasRequestControl.h>
#include <Common/Exception.h>
#include <Common/ProfileEvents.h>
#include <Common/Stopwatch.h>
#include <base/sleep.h>

#include "config.h"

#if USE_AWS_S3
#include <IO/S3Common.h>
#endif

#include <Poco/Exception.h>

#include <fmt/format.h>

#include <ctime>
#include <limits>
#include <utility>

namespace ProfileEvents
{
    extern const Event CASRequestAttempt;
    extern const Event CASRequestReissue;
    extern const Event CASRequestResolveRead;
    extern const Event CASRequestGaveUp;
    extern const Event CASRequestRefused;
    extern const Event CASRequestFenceLostPostWrite;
}

namespace DB::ErrorCodes
{
    extern const int BAD_ARGUMENTS;
    extern const int CAS_DELETE_MARKER;
    extern const int CORRUPTED_DATA;
    extern const int LOGICAL_ERROR;
    extern const int NOT_IMPLEMENTED;
}

namespace DB::Cas
{

namespace detail
{

void recordAttempt()
{
    ProfileEvents::increment(ProfileEvents::CASRequestAttempt);
}

void recordReissue()
{
    ProfileEvents::increment(ProfileEvents::CASRequestReissue);
}

}

namespace
{

/// `CLOCK_BOOTTIME` milliseconds -- the clock a mount lease deadline is expressed on, reproduced here
/// rather than shared because `Backend/` does not depend on the mount plane.
uint64_t bootClockMs()
{
    return clock_gettime_ns(CLOCK_BOOTTIME) / 1000000;
}

uint64_t saturatingAdd(uint64_t lhs, uint64_t rhs)
{
    return lhs > std::numeric_limits<uint64_t>::max() - rhs ? std::numeric_limits<uint64_t>::max() : lhs + rhs;
}

/// The failure class a fresh credential could fix, named here rather than taken from
/// `S3Exception::isAccessTokenExpiredError`, which also fires on `S3Errors::UNKNOWN` -- the SDK's code
/// for EVERY error it does not model. Borrowing it would put throttling codes an S3-compatible store
/// reports under a non-AWS name into the credential class, and would carve a real access denial out of
/// `isDefinitelyRefusedWrite` so the write wedges its caller instead of being refused. `UNKNOWN` is
/// therefore never matched by code alone; a store that spells the error out by name still matches.
bool isRefreshableCredentialError([[maybe_unused]] const std::exception & e)
{
#if USE_AWS_S3
    const auto * s3 = dynamic_cast<const S3Exception *>(&e);
    if (!s3)
        return false;
    const Aws::S3::S3Errors code = s3->getS3ErrorCode();
    if (code == Aws::S3::S3Errors::INVALID_ACCESS_KEY_ID || code == Aws::S3::S3Errors::ACCESS_DENIED
        || code == Aws::S3::S3Errors::INVALID_SIGNATURE || code == Aws::S3::S3Errors::INVALID_CLIENT_TOKEN_ID)
        return true;
    const String & name = s3->getExceptionName();
    return name == "ExpiredToken" || name == "InvalidToken" || name == "InvalidAccessKeyId"
        || name == "SignatureDoesNotMatch" || name == "AccessDenied" || name == "AccountProblem";
#else
    return false;
#endif
}

/// An answer from the store rather than a fault in reaching it: reissuing replays it unchanged.
bool isDefiniteStoreRefusal([[maybe_unused]] const std::exception & e)
{
#if USE_AWS_S3
    if (const auto * s3 = dynamic_cast<const S3Exception *>(&e))
        return !s3->isRetryableError();
#endif
    return false;
}

GaveUp::Source sourceFor(const Retry::Bound & bound)
{
    return bound.lease_bound ? GaveUp::Source::Lease : GaveUp::Source::Policy;
}

/// Drop the body from an observation the write engine had to fetch to prove whose bytes were at the
/// key. The presence-only loop is defined by what it reports, so the demotion happens on its results
/// rather than being trusted to every branch that builds one.
Observation withoutBody(Observation seen)
{
    if (const auto * obj = std::get_if<Object>(&seen))
        return Meta{obj->bytes.size(), obj->incarnation};
    return seen;
}

WriteResult withoutBody(WriteResult result)
{
    if (auto * conflict = std::get_if<Conflict>(&result))
        conflict->seen = withoutBody(std::move(conflict->seen));
    else if (auto * declined = std::get_if<Declined>(&result))
        declined->seen = withoutBody(std::move(declined->seen));
    else if (auto * gave_up = std::get_if<GaveUp>(&result))
        gave_up->last_seen = withoutBody(std::move(gave_up->last_seen));
    return result;
}

}

bool isDeterministicLocalFailure(int code)
{
    return code == ErrorCodes::LOGICAL_ERROR || code == ErrorCodes::NOT_IMPLEMENTED
        || code == ErrorCodes::BAD_ARGUMENTS || code == ErrorCodes::CORRUPTED_DATA;
}

bool isDefinitelyRefusedWrite([[maybe_unused]] const std::exception & e)
{
#if USE_AWS_S3
    if (const auto * s3 = dynamic_cast<const S3Exception *>(&e))
        return S3::isMalformedRequestError(*s3) || S3::isEntityTooLargeError(*s3) || S3::isAccessDeniedError(*s3);
#endif
    return false;
}

CasRequests::CasRequests(BackendPtr backend_, Fence fence_,
                         std::function<uint64_t()> now_ms_, std::function<void(uint64_t)> sleep_ms_)
    : backend(std::move(backend_))
    , fence(std::move(fence_))
    , now_ms(now_ms_ ? std::move(now_ms_) : std::function<uint64_t()>(bootClockMs))
    , sleep_ms(sleep_ms_ ? std::move(sleep_ms_) : std::function<void(uint64_t)>(sleepForMilliseconds))
    , attempt_reservation_ms(backend->attemptTimeoutMs())
{
}

void CasRequests::setNowFnForTest(std::function<uint64_t()> now_ms_)
{
    now_ms = now_ms_ ? std::move(now_ms_) : std::function<uint64_t()>(bootClockMs);
}

void CasRequests::setSleepFnForTest(std::function<void(uint64_t)> sleep_ms_)
{
    sleep_ms = sleep_ms_ ? std::move(sleep_ms_) : std::function<void(uint64_t)>(sleepForMilliseconds);
}

CasOperation CasRequests::admit(Liveness liveness)
{
    return CasOperation(*this, fence.generation(), std::move(liveness));
}

CasOperation CasRequests::resume(uint64_t admitted_generation, Liveness liveness)
{
    return CasOperation(*this, admitted_generation, std::move(liveness));
}

std::optional<Incarnation> CasRequests::tryMint(const String & key, String value) const
{
    if (!isIncarnationValue(backend->dialect(), value))
        return std::nullopt;
    return Incarnation(backend->backendId(), key, backend->dialect(), std::move(value));
}

Incarnation CasRequests::mint(const String & key, String value) const
{
    if (auto minted = tryMint(key, value))
        return std::move(*minted);
    throw Exception(ErrorCodes::CORRUPTED_DATA,
        "CAS: the store answered for '{}' with a value '{}' that is not a valid incarnation", key, value);
}

const String & CasRequests::valueFor(const String & key, const Incarnation & inc) const
{
    if (inc.key() != key || inc.backendId() != backend->backendId())
        throw Exception(ErrorCodes::LOGICAL_ERROR,
            "CAS: an incarnation of '{}' observed on backend {} cannot be the precondition for '{}' on backend {}",
            inc.key(), inc.backendId(), key, backend->backendId());
    return inc.value();
}

CasOperation::Gate CasOperation::gate(uint64_t needed_ms) const
{
    switch (owner.fence.admit(admitted_generation, needed_ms))
    {
        case Fence::Admit::LostOrRearmed: return Gate::FenceLost;
        case Fence::Admit::NoBudget:      return Gate::NoBudget;
        case Fence::Admit::Ok: break;
    }
    /// The caller's own facts are the second half of admission, and the engine does not need to know
    /// which of the two refused: a stopping task and a lost lease end the operation the same way.
    if (liveness && !liveness())
        return Gate::FenceLost;
    return Gate::Ok;
}

uint64_t CasOperation::reservedFor(uint64_t sleep_ms, uint32_t envelopes) const
{
    uint64_t total = sleep_ms;
    for (uint32_t i = 0; i < envelopes; ++i)
        total = saturatingAdd(total, owner.attempt_reservation_ms);
    return total;
}

bool CasOperation::fits(uint64_t needed_ms, const Retry::Bound & bound) const
{
    const uint64_t now = owner.now_ms();
    const uint64_t remaining = now >= bound.deadline_ms ? 0 : bound.deadline_ms - now;
    return needed_ms <= remaining;
}

bool CasOperation::refreshAndClassifyReadFault(const std::exception & e)
{
    if (const auto * db_e = dynamic_cast<const Exception *>(&e); db_e && isDeterministicLocalFailure(db_e->code()))
        return true;
    /// A local failure -- a bad allocation, a logic error raised inside the attempt -- is not a
    /// transport fault, and reissuing it would spend the whole deadline replaying the same bug. Every
    /// exception the transport raises is a `Poco::Exception`, `DB::Exception` included.
    if (!dynamic_cast<const Poco::Exception *>(&e))
        return true;
    if (isRefreshableCredentialError(e) && owner.backend->refreshCredentials())
        return false;
    /// No credentials were installed, so the store's own answer decides: a definite one would replay
    /// identically, and everything else -- an unmodeled name an S3-compatible store reports, a
    /// throttle, a 5xx -- may still be transient and is reissued.
    return isDefiniteStoreRefusal(e);
}

void CasOperation::giveUpReadFenceLost(std::string_view verb, const String & subject, std::string_view when) const
{
    throwCasTransientUnavailable(fmt::format("CAS {} of '{}'", verb, subject),
                                 fmt::format("mount fence tripped {}", when));
}

void CasOperation::giveUpReadNoBudget(std::string_view verb, const String & subject, std::string_view what) const
{
    throwCasWriteRetryLater(fmt::format("{} of '{}': no lease budget {}", verb, subject, what));
}

void CasOperation::giveUpReadDeadline(std::string_view verb, const String & subject,
                                      const Retry::Bound & bound, uint32_t attempts_made) const
{
    throwCasWriteRetryLater(fmt::format("{} of '{}': gave up at the {} deadline after {} attempt(s)",
                                        verb, subject, bound.lease_bound ? "lease" : "policy", attempts_made));
}

std::optional<Object> CasOperation::readUnder(const String & key, const Retry & policy, const Retry::Bound & bound)
{
    return readLoop("read", key, policy, bound, [&](auto & access) -> std::optional<Object>
    {
        auto raw = owner.backend->read(key, access);
        if (!raw)
            return std::nullopt;
        return Object{std::move(raw->bytes), owner.mint(key, std::move(raw->value))};
    });
}

std::optional<Meta> CasOperation::headUnder(const String & key, const Retry & policy, const Retry::Bound & bound)
{
    return readLoop("head", key, policy, bound, [&](auto & access) -> std::optional<Meta>
    {
        auto raw = owner.backend->head(key, access);
        if (!raw)
            return std::nullopt;
        return Meta{raw->size, owner.mint(key, std::move(raw->value))};
    });
}

KeyPage CasOperation::listUnder(const String & prefix, const String & cursor, size_t limit,
                                const Retry & policy, const Retry::Bound & bound)
{
    return readLoop("list", prefix, policy, bound, [&](auto & access)
    {
        Backend::RawListPage raw = owner.backend->list(prefix, cursor, limit, access);
        KeyPage page;
        page.next_cursor = std::move(raw.next_cursor);
        page.keys.reserve(raw.keys.size());
        for (auto & listed : raw.keys)
        {
            KeyEntry entry{std::move(listed.key), listed.size, std::nullopt};
            if (listed.value)
                entry.incarnation = owner.mint(entry.key, std::move(*listed.value));
            page.keys.push_back(std::move(entry));
        }
        return page;
    });
}

Removal CasOperation::removeUnder(const String & key, const String & expected_value,
                                  const Retry & policy, const Retry::Bound & bound)
{
    const Backend::RawRemoval raw = readLoop("remove", key, policy, bound, [&](auto & access)
    {
        return owner.backend->remove(key, expected_value, access);
    });
    switch (raw)
    {
        case Backend::RawRemoval::Removed:  return Removal::Removed;
        case Backend::RawRemoval::Gone:     return Removal::Gone;
        case Backend::RawRemoval::Mismatch: return Removal::Mismatch;
        case Backend::RawRemoval::DeleteMarker: break;
    }
    /// Thrown outside the attempt loop: a versioned bucket answers this way every time, so reissuing
    /// would spend the whole deadline to be told the same thing.
    throw Exception(ErrorCodes::CAS_DELETE_MARKER,
        "CAS remove of '{}' archived a noncurrent version instead of reclaiming the object: "
        "the bucket has object versioning enabled", key);
}

std::optional<Object> CasOperation::read(const String & key, const Retry & policy)
{
    return readUnder(key, policy, policy.bind(owner.now_ms()));
}

std::optional<Meta> CasOperation::head(const String & key, const Retry & policy)
{
    return headUnder(key, policy, policy.bind(owner.now_ms()));
}

KeyPage CasOperation::list(const String & prefix, const String & cursor, size_t limit, const Retry & policy)
{
    return listUnder(prefix, cursor, limit, policy, policy.bind(owner.now_ms()));
}

void CasOperation::forEachListedKey(const String & prefix, const KeyEntryFn & fn, const Retry & per_page,
                                    size_t page_limit, const std::function<void()> & on_page_fetched)
{
    String cursor;
    for (;;)
    {
        KeyPage page = list(prefix, cursor, page_limit, per_page);
        if (on_page_fetched)
            on_page_fetched();
        for (const KeyEntry & entry : page.keys)
            if (!fn(entry))
                return;
        if (page.next_cursor.empty())
            return;
        cursor = std::move(page.next_cursor);
    }
}

Removal CasOperation::remove(const String & key, const Incarnation & seen, const Retry & policy)
{
    return removeUnder(key, owner.valueFor(key, seen), policy, policy.bind(owner.now_ms()));
}

Removal CasOperation::removeCurrent(const String & key, const Retry & policy)
{
    const Retry::Bound bound = policy.bind(owner.now_ms());
    for (uint32_t attempt = 1;; ++attempt)
    {
        const std::optional<Meta> seen = headUnder(key, policy, bound);
        if (!seen)
            return Removal::Gone;
        const Removal removed = removeUnder(key, owner.valueFor(key, seen->incarnation), policy, bound);
        if (removed != Removal::Mismatch)
            return removed;

        /// A `Mismatch` is a VALUE, so it never reaches the fault check inside the two loops above: this
        /// verb has to honour `once` itself, and its contract is that it never hands a `Mismatch` back.
        /// The message says what actually stopped it, which is the policy and not the deadline.
        if (policy.single_attempt)
            throwCasWriteRetryLater(fmt::format(
                "removeCurrent of '{}': the observed incarnation was replaced and the policy allows no reissue", key));

        /// Another incarnation became current between the observation and the delete. Re-observe, paced
        /// like every other reissue: the reservation covers the next `head` and the `remove` after it.
        const uint64_t pause_ms = Retry::backoff(attempt);
        const uint64_t needed = reservedFor(pause_ms, 2);
        switch (gate(needed))
        {
            case Gate::FenceLost: giveUpReadFenceLost("removeCurrent", key, "before the reissue");
            case Gate::NoBudget:  giveUpReadNoBudget("removeCurrent", key, "for the reissue");
            case Gate::Ok: break;
        }
        if (!fits(needed, bound))
            giveUpReadDeadline("removeCurrent", key, bound, attempt);
        detail::recordReissue();
        owner.sleep_ms(pause_ms);
    }
}

SentinelProbeResult CasOperation::probeSentinel(const String & key, const Retry & policy)
{
    const Retry::Bound bound = policy.bind(owner.now_ms());
    for (uint32_t attempt = 1;; ++attempt)
    {
        const uint64_t reservation = reservedFor(0, 1);
        switch (gate(reservation))
        {
            case Gate::FenceLost: giveUpReadFenceLost("probeSentinel", key, "before the request");
            case Gate::NoBudget:  giveUpReadNoBudget("probeSentinel", key, "for one more request");
            case Gate::Ok: break;
        }
        /// Before the first attempt nothing has been probed, so the caller is told the request never
        /// happened rather than being handed an outcome no request produced.
        if (!fits(reservation, bound))
            giveUpReadDeadline("probeSentinel", key, bound, attempt - 1);

        detail::recordAttempt();
        SentinelProbeResult result{ProbeOutcome::Indeterminate, std::nullopt};
        try
        {
            result = owner.withTransportAccess([&](auto & access)
            {
                return owner.backend->probeSentinelRaw(key, access);
            });
        }
        catch (const std::exception & e)
        {
            if (refreshAndClassifyReadFault(e))
                throw;
            /// The probe reports every transport failure as `Indeterminate` rather than by throwing, so
            /// a decorator that does throw is folded onto the same inconclusive outcome.
        }

        /// The reissue is driven by the OUTCOME: this is the one primitive whose failures never reach a
        /// `catch`, and `Indeterminate` means "inconclusive", which is exactly what a retry resolves.
        /// The other four outcomes are authoritative answers and return on the first attempt.
        if (result.outcome != ProbeOutcome::Indeterminate || policy.single_attempt)
            return result;

        const uint64_t pause_ms = Retry::backoff(attempt);
        const uint64_t needed = reservedFor(pause_ms, 1);
        /// At the bound the inconclusive outcome IS the answer: a probe did run, and reporting what it
        /// saw is more than a give-up exception could say.
        if (gate(needed) != Gate::Ok || !fits(needed, bound))
            return result;
        detail::recordReissue();
        owner.sleep_ms(pause_ms);
    }
}

std::unique_ptr<ReadBuffer> CasOperation::stream(const String & key, const Retry & policy)
{
    const Retry::Bound bound = policy.bind(owner.now_ms());
    return readLoop("stream", key, policy, bound, [&](auto & access)
    {
        return owner.backend->stream(key, access);
    });
}

void CasOperation::publish(const BlobPublishRequest & request, const Retry & policy)
{
    const Retry::Bound bound = policy.bind(owner.now_ms());
    readLoop("publish", request.destination_key, policy, bound, [&](auto & access)
    {
        owner.backend->publish(request, access);
    });
}

Observation CasOperation::observe(const String & key, const Retry & policy, const Retry::Bound & bound)
{
    try
    {
        auto got = readUnder(key, policy, bound);
        if (!got)
            return ProvenAbsent{};
        return std::move(*got);
    }
    catch (const Exception & e)
    {
        /// A local bug replays identically on every reissue, so it is never swallowed into "nothing
        /// observed". Everything else -- including the read giving up at the deadline -- is exactly
        /// that: the resolve settled nothing.
        if (isDeterministicLocalFailure(e.code()))
            throw;
        return NotObserved{};
    }
    catch (const std::exception &)
    {
        return NotObserved{};
    }
}

Observation CasOperation::observePresence(const String & key, const Retry & policy, const Retry::Bound & bound)
{
    try
    {
        auto got = headUnder(key, policy, bound);
        if (!got)
            return ProvenAbsent{};
        return std::move(*got);
    }
    catch (const Exception & e)
    {
        if (isDeterministicLocalFailure(e.code()))
            throw;
        return NotObserved{};
    }
    catch (const std::exception &)
    {
        return NotObserved{};
    }
}

WriteResult CasOperation::gaveUp(GaveUp::Why why, GaveUp::Source source, WriteState & state) const
{
    ProfileEvents::increment(ProfileEvents::CASRequestGaveUp);
    return GaveUp{why, source, state.sent_any, state.last_seen};
}

WriteResult CasOperation::gaveUpAfterFailedObservation(WriteState & state, const Retry::Bound & bound)
{
    /// A lost generation never comes back, so if the fence still admits us the read ran out of time.
    if (gate(0) == Gate::FenceLost)
        return gaveUp(GaveUp::Why::FenceLost, sourceFor(bound), state);
    return gaveUp(GaveUp::Why::Deadline, sourceFor(bound), state);
}

WriteResult CasOperation::postCommit(Incarnation inc, bool resolved_by_read, WriteState & state, const Retry::Bound & bound)
{
    /// Admission once more, now that the write is proven durable: a fence lost here means the object
    /// may well exist, but this call must never claim it -- the caller has to resolve the key instead.
    switch (gate(0))
    {
        case Gate::FenceLost:
            ProfileEvents::increment(ProfileEvents::CASRequestFenceLostPostWrite);
            return gaveUp(GaveUp::Why::FenceLost, sourceFor(bound), state);
        case Gate::NoBudget:
            /// The fence's budget IS the mount lease, so its refusal names the lease as the bound that
            /// ended this call, whichever bound produced the policy's own deadline.
            return gaveUp(GaveUp::Why::Deadline, GaveUp::Source::Lease, state);
        case Gate::Ok: break;
    }
    return Committed{std::move(inc), state.attempts_sent, resolved_by_read};
}

std::optional<WriteResult> CasOperation::pauseAndReissue(WriteState & state, const Retry::Bound & bound)
{
    const uint64_t pause_ms = Retry::backoff(++state.reissues);
    const uint64_t needed = reservedFor(pause_ms, 2);
    switch (gate(needed))
    {
        case Gate::FenceLost: return gaveUp(GaveUp::Why::FenceLost, sourceFor(bound), state);
        case Gate::NoBudget:  return gaveUp(GaveUp::Why::Deadline, GaveUp::Source::Lease, state);
        case Gate::Ok: break;
    }
    if (!fits(needed, bound))
        return gaveUp(GaveUp::Why::Deadline, sourceFor(bound), state);
    detail::recordReissue();
    owner.sleep_ms(pause_ms);
    return std::nullopt;
}

WriteResult CasOperation::writeLoop(const String & key, const String & bytes, const std::optional<Incarnation> & expected,
                                    const Retry & policy, const Retry::Bound & bound, WriteState & state,
                                    ResolveWith resolve_refusal_with)
{
    std::optional<String> expected_value;
    if (expected)
        expected_value = owner.valueFor(key, *expected);

    for (;;)
    {
        /// A write reserves TWO envelopes: the attempt, and the exact read that settles it. That is
        /// what keeps "every conflict is settled by one read" true at the deadline edge.
        const uint64_t reservation = reservedFor(0, 2);
        switch (gate(reservation))
        {
            case Gate::FenceLost: return gaveUp(GaveUp::Why::FenceLost, sourceFor(bound), state);
            case Gate::NoBudget:  return gaveUp(GaveUp::Why::Deadline, GaveUp::Source::Lease, state);
            case Gate::Ok: break;
        }
        if (!fits(reservation, bound))
            return gaveUp(GaveUp::Why::Deadline, sourceFor(bound), state);

        detail::recordAttempt();
        ++state.attempts_sent;
        state.sent_any = true;

        /// Disengaged means the attempt threw: its fate is unproven, and nothing may be read out of it.
        std::optional<std::expected<String, Backend::RawConflict>> outcome;
        try
        {
            outcome = owner.withTransportAccess([&](auto & access)
            {
                return owner.backend->write(key, bytes, expected_value, access);
            });
        }
        catch (const Exception & e)
        {
            if (isDeterministicLocalFailure(e.code()))
                throw;
            /// One refresh, and only for the class a credential could explain -- so an oversized entity
            /// or a malformed request never triggers a re-acquisition. Fresh credentials make the
            /// attempt ambiguous rather than refused: it may have been signed correctly and landed, and
            /// the reissue signs with the new client. Nothing installed means nothing would sign
            /// differently, so the store's answer stands.
            const bool refreshed = isRefreshableCredentialError(e) && owner.backend->refreshCredentials();
            /// A refusal that FOLLOWS an ambiguous attempt of this call proves nothing about that
            /// attempt, so it is settled by the read below instead of ending the call here.
            if (!refreshed && isDefinitelyRefusedWrite(e) && !state.any_ambiguous)
            {
                ProfileEvents::increment(ProfileEvents::CASRequestRefused);
                return Refused{e.code(), e.message()};
            }
        }
        catch (const std::exception &)
        {
            /// An unmodeled failure may still have landed: leave `outcome` disengaged and settle it by
            /// reading, never by reporting a refusal the store never gave.
        }

        if (outcome && outcome->has_value())
        {
            if (auto inc = owner.tryMint(key, std::move(**outcome)))
                return postCommit(std::move(*inc), /*resolved_by_read=*/false, state, bound);
            /// A 2xx carrying a value no grammar accepts: the write may well have landed, so this is an
            /// ambiguity to settle by reading, never a corruption verdict about the object.
            outcome.reset();
        }
        if (!outcome)
            state.any_ambiguous = true;

        /// Every refused precondition and every ambiguous attempt is settled by ONE exact read, under
        /// every policy: a refused precondition does not say WHO holds the key, and a 404 and a 412
        /// reach here as the same answer. A refused precondition needs only to know WHAT is there, so a
        /// presence-only caller settles it with a HEAD; proving an ambiguous attempt landed needs the
        /// bytes, and there the body read is unavoidable.
        ProfileEvents::increment(ProfileEvents::CASRequestResolveRead);
        state.last_seen = resolve_refusal_with == ResolveWith::Presence && !state.any_ambiguous
            ? observePresence(key, policy, bound)
            : observe(key, policy, bound);
        if (const auto * obj = std::get_if<Object>(&state.last_seen))
        {
            /// Our own bytes prove an earlier ambiguous attempt landed. Without an ambiguity there was
            /// nothing of ours to land, so identical bytes are somebody else's object and the caller
            /// that owns the key's meaning decides what that means.
            if (state.any_ambiguous && obj->bytes == bytes)
                return postCommit(obj->incarnation, /*resolved_by_read=*/true, state, bound);
            return Conflict{state.last_seen};
        }
        if (!state.any_ambiguous)
            return Conflict{state.last_seen};
        if (policy.single_attempt)
            return gaveUp(GaveUp::Why::Unresolved, sourceFor(bound), state);
        if (auto given_up = pauseAndReissue(state, bound))
            return *given_up;
    }
}

WriteResult CasOperation::create(const String & key, const String & bytes, const Retry & policy)
{
    WriteState state;
    return writeLoop(key, bytes, std::nullopt, policy, policy.bind(owner.now_ms()), state, ResolveWith::Body);
}

WriteResult CasOperation::replace(const String & key, const String & bytes, const Incarnation & seen, const Retry & policy)
{
    WriteState state;
    return writeLoop(key, bytes, seen, policy, policy.bind(owner.now_ms()), state, ResolveWith::Body);
}

WriteResult CasOperation::readModifyWrite(const String & key, const DecideOnObject & decide, const Retry & policy)
{
    const Retry::Bound bound = policy.bind(owner.now_ms());
    WriteState state;

    state.last_seen = observe(key, policy, bound);
    std::optional<Object> current;
    if (const auto * obj = std::get_if<Object>(&state.last_seen))
        current = *obj;
    else if (!std::holds_alternative<ProvenAbsent>(state.last_seen))
        return gaveUpAfterFailedObservation(state, bound);

    for (;;)
    {
        /// Outside every classification: `decide` is the caller's own control flow, and an exception
        /// from it is theirs to see unchanged.
        const std::optional<String> next = decide(current);
        if (!next)
            return Declined{state.last_seen};

        WriteResult result = writeLoop(key, *next,
            current ? std::optional<Incarnation>(current->incarnation) : std::nullopt, policy, bound, state,
            ResolveWith::Body);
        if (!std::holds_alternative<Conflict>(result))
            return result;

        /// The write's own resolve read IS this iteration's read: a hot key never costs a second GET
        /// per conflict.
        if (const auto * obj = std::get_if<Object>(&state.last_seen))
            current = *obj;
        else if (std::holds_alternative<ProvenAbsent>(state.last_seen))
            current.reset();

        if (policy.single_attempt)
            return result;
        if (auto given_up = pauseAndReissue(state, bound))
            return *given_up;

        /// Only when the resolve settled nothing is a fresh read owed; otherwise `current` already is
        /// what the store held.
        if (std::holds_alternative<NotObserved>(state.last_seen))
        {
            state.last_seen = observe(key, policy, bound);
            if (const auto * obj = std::get_if<Object>(&state.last_seen))
                current = *obj;
            else if (std::holds_alternative<ProvenAbsent>(state.last_seen))
                current.reset();
            else
                return gaveUpAfterFailedObservation(state, bound);
        }
    }
}

WriteResult CasOperation::readModifyWriteOnPresence(const String & key, const DecideOnMeta & decide, const Retry & policy)
{
    const Retry::Bound bound = policy.bind(owner.now_ms());
    WriteState state;

    state.last_seen = observePresence(key, policy, bound);
    std::optional<Meta> current;
    if (const auto * meta = std::get_if<Meta>(&state.last_seen))
        current = *meta;
    else if (!std::holds_alternative<ProvenAbsent>(state.last_seen))
        return gaveUpAfterFailedObservation(state, bound);

    for (;;)
    {
        const std::optional<String> next = decide(current);
        if (!next)
            return Declined{state.last_seen};

        WriteResult result = writeLoop(key, *next,
            current ? std::optional<Incarnation>(current->incarnation) : std::nullopt, policy, bound, state,
            ResolveWith::Presence);
        /// A refused precondition was settled by a HEAD, but proving an ambiguous attempt landed needs
        /// the bytes; this loop is presence-only by contract, so that body stops here.
        state.last_seen = withoutBody(std::move(state.last_seen));
        if (!std::holds_alternative<Conflict>(result))
            return withoutBody(std::move(result));

        if (const auto * meta = std::get_if<Meta>(&state.last_seen))
            current = *meta;
        else if (std::holds_alternative<ProvenAbsent>(state.last_seen))
            current.reset();

        if (policy.single_attempt)
            return Conflict{state.last_seen};
        if (auto given_up = pauseAndReissue(state, bound))
            return *given_up;

        if (std::holds_alternative<NotObserved>(state.last_seen))
        {
            state.last_seen = observePresence(key, policy, bound);
            if (const auto * meta = std::get_if<Meta>(&state.last_seen))
                current = *meta;
            else if (std::holds_alternative<ProvenAbsent>(state.last_seen))
                current.reset();
            else
                return gaveUpAfterFailedObservation(state, bound);
        }
    }
}

}
