#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Backend/CasRequests.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Backend/CasOwner.h>

#include <Common/ErrorCodes.h>
#include <Common/ErrnoException.h>
#include <Common/Exception.h>
#include <IO/ReadBuffer.h>
#include <Common/LoggingHelpers.h>
#include <Common/ProfileEvents.h>
#include <Common/Stopwatch.h>
#include <Common/logger_useful.h>
#include <base/sleep.h>

#include "config.h"

#if USE_AWS_S3
#include <IO/S3Common.h>
#endif

#include <Poco/Exception.h>

#include <fmt/format.h>

#include <array>
#include <cerrno>
#include <ctime>
#include <limits>
#include <type_traits>
#include <utility>

namespace ProfileEvents
{
    extern const Event CASRequestAttempt;
    extern const Event CASRequestReissue;
    extern const Event CASRequestConflictPause;
    extern const Event CASRequestResolveRead;
    extern const Event CASRequestGaveUp;
    extern const Event CASRequestRefused;
    extern const Event CASRequestFenceLostPostWrite;
    extern const Event CASRequestConnectFailureHint;
    extern const Event CASRequestFirstAttemptFuse;
}

namespace DB::ErrorCodes
{
    extern const int BAD_ARGUMENTS;
    extern const int CAS_DELETE_MARKER;
    extern const int CORRUPTED_DATA;
    extern const int LOGICAL_ERROR;
    extern const int MEMORY_LIMIT_EXCEEDED;
    extern const int NETWORK_ERROR;
    extern const int NOT_IMPLEMENTED;
    extern const int READONLY;
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

void recordConflictPause()
{
    ProfileEvents::increment(ProfileEvents::CASRequestConflictPause);
}

void recordFirstAttemptFuse()
{
    ProfileEvents::increment(ProfileEvents::CASRequestFirstAttemptFuse);
}

}

namespace
{
/// Shared by the two entry points below so the log line and the exception's message text can never
/// drift apart. Rate-limited (not per-distinct-`why` -- `LogSeriesLimiter` keys on the LOGGER NAME
/// only, so under a sustained outage where `why` keeps changing slightly, only the first message in
/// each window prints; this is the intended throttle, not a bug). Warning-level visibility is
/// intentional: this condition is expected to self-heal (the caller retries), but an operator watching
/// CAS logs directly should see it without having to know to look at system.replication_queue.
void logCasWriteRetryLater(const String & why)
{
    LogSeriesLimiter log(getLogger("CasWriteRetryLater"), /*allowed_count=*/1, /*interval_s=*/30);
    LOG_WARNING(log, "CAS write could not be committed ({}); retrying later", why);
}
}

[[noreturn]] void throwCasWriteRetryLater(const String & why)
{
    logCasWriteRetryLater(why);
    throw Exception(ErrorCodes::NETWORK_ERROR, "CAS write could not be committed ({}); retrying later", why);
}

[[noreturn]] void throwDidNotConverge(std::string_view what, uint32_t count, std::string_view unit)
{
    throwCasWriteRetryLater(fmt::format("{}: did not converge after {} {}", what, count, unit));
}

[[noreturn]] void throwCasRequestRetryLater(const String & why)
{
    LogSeriesLimiter log(getLogger("CasRequestRetryLater"), /*allowed_count=*/1, /*interval_s=*/30);
    LOG_WARNING(log, "CAS request did not complete ({}); retrying later", why);
    throw Exception(ErrorCodes::NETWORK_ERROR, "CAS request did not complete ({}); retrying later", why);
}

Exception makeCasWriteRetryLaterException(const String & why)
{
    logCasWriteRetryLater(why);
    return Exception(ErrorCodes::NETWORK_ERROR, "CAS write could not be committed ({}); retrying later", why);
}

std::exception_ptr makeCasWriteRetryLaterExceptionPtr(const String & why)
{
    return std::make_exception_ptr(makeCasWriteRetryLaterException(why));
}

[[noreturn]] void throwCasTransientUnavailable(const String & subject, const String & condition)
{
    /// The code is coarse (it shares a `system.errors` row with socket failures), so the MESSAGE must
    /// carry the whole truth: which CA condition refused, and that the refusal is a state rather than
    /// damage. Consumers key on the code; operators read this line.
    ///
    /// The shared suffix carries ONLY the classification, because that is the one claim true at every
    /// site: retry-later is right even where the condition may turn out terminal, since the next attempt
    /// re-decides against fresh state. Any promise about HOW the condition clears belongs in `condition`,
    /// where the site that can actually prove it makes it -- `checkFenceOrThrow` provably cannot.
    throw Exception(ErrorCodes::NETWORK_ERROR,
        "{} -- {}; TRANSIENT unavailability, not damage", subject, condition);
}

namespace
{

/// `CLOCK_BOOTTIME` milliseconds -- the clock a mount lease deadline is expressed on, reproduced here
/// rather than shared because `Backend/` does not depend on the mount plane.
uint64_t bootClockMs()
{
#if defined(OS_DARWIN)
    constexpr clockid_t clock_id = CLOCK_MONOTONIC;
#else
    constexpr clockid_t clock_id = CLOCK_BOOTTIME;
#endif
    return clock_gettime_ns(clock_id) / 1000000;
}

uint64_t saturatingAdd(uint64_t lhs, uint64_t rhs)
{
    return lhs > std::numeric_limits<uint64_t>::max() - rhs ? std::numeric_limits<uint64_t>::max() : lhs + rhs;
}

}

/// The failure class a fresh credential could fix, named here rather than taken from
/// `S3Exception::isAccessTokenExpiredError`, which also fires on `S3Errors::UNKNOWN` -- the SDK's code
/// for EVERY error it does not model. Borrowing it would put throttling codes an S3-compatible store
/// reports under a non-AWS name into the credential class, and would carve a real access denial out of
/// `isDefinitelyRefusedWrite` so the write wedges its caller instead of being refused. `UNKNOWN` is
/// therefore never matched by code alone; a store that spells the error out by name still matches.
///
/// Declared in the header: a caller whose OWN loop makes the next physical attempt has to tell this
/// class apart from the rest of `isDefinitelyRefusedWrite`.
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

namespace
{

/// The store's authoritative answer that there is nothing at the key -- a fact about the OBJECT, which
/// reissuing only replays. Deliberately narrower than "not retryable": a missing bucket or an
/// unmodeled name is an answer about reaching the store, which an S3-compatible store gives
/// transiently when it misroutes a request, so it stays in the ambiguous class and is reissued until
/// the deadline. The credential codes never reach here -- `refreshAndClassifyReadFault` consumes them
/// first.
bool isAuthoritativeAbsence([[maybe_unused]] const std::exception & e)
{
#if USE_AWS_S3
    if (const auto * s3 = dynamic_cast<const S3Exception *>(&e))
    {
        const Aws::S3::S3Errors code = s3->getS3ErrorCode();
        return code == Aws::S3::S3Errors::NO_SUCH_KEY || code == Aws::S3::S3Errors::NO_SUCH_UPLOAD;
    }
#endif
    return false;
}

GaveUp::Source sourceFor(const Retry::Bound & bound)
{
    return bound.source == Retry::Bound::Source::Lease ? GaveUp::Source::Lease : GaveUp::Source::Policy;
}

/// Could the precondition this write was built with still be met by what the resolve read saw? A
/// create needs the key absent; a replace needs the incarnation it named to still be current.
bool preconditionStillSatisfiable(const Observation & seen, const std::optional<Etag> & expected)
{
    return std::visit(detail::Overload{
        /// The read itself failed, so it proved nothing either way and an ambiguous attempt may still
        /// be alive. Reporting a conflict on it would name an occupant nobody observed.
        [](const NotObserved &) { return true; },
        [&](const ProvenAbsent &) { return !expected.has_value(); },
        [&](const Meta & m) { return expected.has_value() && sameVersion(m.etag, *expected); },
        [&](const Object & o) { return expected.has_value() && sameVersion(o.etag, *expected); }},
        seen);
}

/// Drop the body from an observation the write engine had to fetch to prove whose bytes were at the
/// key. The presence-only loop is defined by what it reports, so the demotion happens on its results
/// rather than being trusted to every branch that builds one.
Observation withoutBody(Observation seen)
{
    if (const auto * obj = std::get_if<Object>(&seen))
        return Meta{obj->bytes.size(), obj->etag};
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

bool isMemoryLimitExceeded(const std::exception & e)
{
    const auto * db = dynamic_cast<const Exception *>(&e);
    return db && db->code() == ErrorCodes::MEMORY_LIMIT_EXCEEDED;
}

bool isDeterministicLocalFailure(int code)
{
    return code == ErrorCodes::LOGICAL_ERROR || code == ErrorCodes::NOT_IMPLEMENTED
        || code == ErrorCodes::BAD_ARGUMENTS || code == ErrorCodes::CORRUPTED_DATA;
}

bool isDefinitelyRefusedWrite([[maybe_unused]] const std::exception & e)
{
    /// A read-only object storage and a file the process may not create both refuse before anything is
    /// written, and say so the same way on every reissue.
    if (const auto * db_e = dynamic_cast<const Exception *>(&e); db_e && db_e->code() == ErrorCodes::READONLY)
        return true;
    if (const auto * errno_e = dynamic_cast<const ErrnoException *>(&e))
    {
        const int error = errno_e->getErrno();
        if (error == EACCES || error == EPERM || error == EROFS)
            return true;
    }
#if USE_AWS_S3
    if (const auto * s3 = dynamic_cast<const S3Exception *>(&e))
        /// The refresh class is included rather than left to overlap: the two name lists agree, but
        /// `InvalidSignature` carries a code `isAccessDeniedError` does not match, and an error that is
        /// refreshable without being refusable would spend the whole deadline whenever no refresh is
        /// available -- which is every CAS disk today.
        return S3::isMalformedRequestError(*s3) || S3::isEntityTooLargeError(*s3)
            || S3::isAccessDeniedError(*s3) || isRefreshableCredentialError(e);
#endif
    return false;
}

bool isConnectFailureHint([[maybe_unused]] const std::exception & e)
{
#if USE_AWS_S3
    const auto * s3 = dynamic_cast<const S3Exception *>(&e);
    if (!s3 || s3->getS3ErrorCode() != Aws::S3::S3Errors::NETWORK_CONNECTION)
        return false;
    /// This repository's Poco (`SocketImpl::error`, `SocketImpl::connect`) is the source of every text.
    static constexpr std::array<std::string_view, 5> texts{
        "Cannot assign requested address", "Connection refused", "No route to host",
        "Network is unreachable", "connect timed out"};
    const std::string_view message = s3->message();
    for (std::string_view text : texts)
        if (message.find(text) != std::string_view::npos)
            return true;
#endif
    return false;
}

bool isFirstAttemptFuseTimeout([[maybe_unused]] const std::exception & e, [[maybe_unused]] size_t attempt_no)
{
#if USE_AWS_S3
    /// The connect-failure hint is checked FIRST: `connect timed out` belongs to that classifier, and
    /// an attempt whose text matches both stays a hint, reissued without a preceding settle read.
    if (attempt_no != 1 || isConnectFailureHint(e))
        return false;
    const auto * s3 = dynamic_cast<const S3Exception *>(&e);
    if (!s3 || s3->getS3ErrorCode() != Aws::S3::S3Errors::NETWORK_CONNECTION)
        return false;
    const std::string_view message = s3->message();
    return message.find("Timeout") != std::string_view::npos;
#else
    return false;
#endif
}

bool isStoreAnswer(const std::exception & e)
{
    const auto * db = dynamic_cast<const Exception *>(&e);
    return dynamic_cast<const Poco::Exception *>(&e) && !(db && isDeterministicLocalFailure(db->code()));
}

bool isLocalFault(const std::exception_ptr & e)
{
    if (isQueryStoredCause(e))
        return false;
    try
    {
        std::rethrow_exception(e);
    }
    catch (const std::exception & ex)
    {
        return !isStoreAnswer(ex);
    }
    catch (...)
    {
        return true;
    }
}

String describeRequestFailure(std::string_view verb, const String & key, const std::exception & e)
{
    const auto & poco = dynamic_cast<const Poco::Exception &>(e);
    String code;
#if USE_AWS_S3
    if (const auto * s3 = dynamic_cast<const S3Exception *>(&e))
        code = s3->getExceptionName();
#endif
    if (const auto * db = dynamic_cast<const Exception *>(&e); code.empty() && db)
        code = String(ErrorCodes::getName(db->code()));
    if (code.empty())
        code = poco.name();
    return fmt::format("{} {}: {} {} {}", verb, key, isDefinitelyRefusedWrite(e) ? "refused" : "transport", code, poco.message());
}

void detail::throwVetoedCredentialRetry(std::string_view verb, const String & subject, const std::exception & e)
{
    throwCasRequestRetryLater(fmt::format("the query stopped before the credential retry; {}",
                                          describeRequestFailure(verb, subject, e)));
}

CasRequests::CasRequests(BackendPtr backend_, Fence fence_,
                         std::function<uint64_t()> now_ms_, std::function<void(uint64_t)> sleep_ms_,
                         CasHotKeys * hot_keys_, uint64_t default_window_ms_)
    : backend(std::move(backend_))
    , fence(std::move(fence_))
    , now_ms(now_ms_ ? std::move(now_ms_) : std::function<uint64_t()>(bootClockMs))
    , sleep_ms(sleep_ms_ ? std::move(sleep_ms_) : std::function<void(uint64_t)>(sleepForMilliseconds))
    , attempt_reservation_ms(backend->attemptEnvelopeMs())
    , default_window_ms(default_window_ms_)
    , own_hot_keys(hot_keys_ ? nullptr : std::make_unique<CasHotKeys>(0))
    , hot_keys(hot_keys_ ? hot_keys_ : own_hot_keys.get())
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

CasOperation CasRequests::resume(uint64_t admitted_generation, Liveness liveness, std::function<void(uint64_t)> sleep_ms_)
{
    return CasOperation(*this, admitted_generation, std::move(liveness), std::move(sleep_ms_));
}

Dialect CasOperation::backendDialect() const
{
    return owner.backend->dialect();
}

std::optional<Etag> CasRequests::tryMint(const String & key, String value) const
{
    if (!isIncarnationValue(backend->dialect(), value))
        return std::nullopt;
    return Etag(backend->backendId(), key, backend->dialect(), std::move(value));
}

Etag CasRequests::mint(const String & key, String value) const
{
    if (auto minted = tryMint(key, value))
        return std::move(*minted);
    throw Exception(ErrorCodes::CORRUPTED_DATA,
        "CAS: the store answered for '{}' with a value '{}' that is not a valid incarnation", key, value);
}

const String & CasRequests::valueFor(const String & key, const Etag & inc) const
{
    if (inc.key() != key || inc.backendId() != backend->backendId())
        throw Exception(ErrorCodes::LOGICAL_ERROR,
            "CAS: an incarnation of '{}' observed on backend {} cannot be the precondition for '{}' on backend {}",
            inc.key(), inc.backendId(), key, backend->backendId());
    return inc.value();
}

namespace
{

/// The one mapping from a refused admission to the exception a read-class caller sees. The request
/// gate and the streamed body below both refuse through it, so a body refused mid-transfer reads
/// exactly like an open refused before it: `NoBudget` is the retry-later class, everything else the
/// tripped-fence class. Free, not a member: the body's copy must not reference an operation.
[[noreturn]] void throwReadRefused(Fence::Admit admit, std::string_view verb, const String & subject,
                                   std::string_view when)
{
    if (admit == Fence::Admit::NoBudget)
        throwCasRequestRetryLater(fmt::format("{} of '{}': no lease budget {}", verb, subject, when));
    throwCasTransientUnavailable(fmt::format("CAS {} of '{}'", verb, subject),
                                 fmt::format("mount fence tripped {}", when));
}

/// The body of a streamed object, re-admitted at every refill. `Backend::stream` bounds only the
/// open; the SDK reads the body at the consumer's pace, under the storage's ordinary settings, long
/// after the attempt that opened it returned -- so a fold parked in a multi-gigabyte run body would
/// outlive the fence that refused every other request of its operation. The check is the operation's
/// own admission, asked once per SDK buffer, and its refusal is the exception a refused open produces.
///
/// The predicate is held BY VALUE -- the fence's `admit` closure, the admitted generation, the
/// caller's liveness -- never through the operation: `CasOperation::stream` returns a buffer that can
/// outlive the operation object (S3 staging opens its stream under a local mount-plane operation and
/// hands the buffer to the backend). Modelled on `LimitReadBuffer`: no byte is copied, the SDK's
/// window is exposed as this buffer's own.
class AdmittedBodyReadBuffer : public ReadBuffer
{
public:
    AdmittedBodyReadBuffer(std::unique_ptr<ReadBuffer> in_, String key_,
                           std::function<Fence::Admit(uint64_t, uint64_t)> admit_,
                           uint64_t admitted_generation_, Liveness liveness_)
        /// The open already loaded a window (`Backend::stream` forces the first GET): adopt it, so
        /// the first refill this buffer asks for is the SECOND window and the SDK buffer is never
        /// asked to advance over pending data.
        : ReadBuffer(in_->position(), in_->available(), 0)
        , in(std::move(in_))
        , key(std::move(key_))
        , admit(std::move(admit_))
        , admitted_generation(admitted_generation_)
        , liveness(std::move(liveness_))
    {
    }

private:
    bool nextImpl() override
    {
        /// Let the SDK buffer account the bytes the consumer took from the shared window.
        in->position() = position();

        Fence::Admit verdict = admit(admitted_generation, 0);
        if (verdict == Fence::Admit::Ok && liveness && !liveness())
            verdict = Fence::Admit::LostOrRearmed;
        if (verdict != Fence::Admit::Ok)
            throwReadRefused(verdict, "stream body", key, "mid-body");

        if (!in->next())
        {
            BufferBase::set(in->position(), 0, 0);
            return false;
        }
        BufferBase::set(in->position(), in->available(), 0);
        return true;
    }

    std::unique_ptr<ReadBuffer> in;
    String key;
    std::function<Fence::Admit(uint64_t, uint64_t)> admit;
    uint64_t admitted_generation;
    Liveness liveness;
};

}

CasOperation::Gate CasOperation::gate(uint64_t needed_ms) const
{
    switch (owner.fence.admit(admitted_generation, needed_ms))
    {
        case Fence::Admit::LostOrRearmed: return Gate::FenceLost;
        case Fence::Admit::NoBudget:      return Gate::NoBudget;
        case Fence::Admit::Ok: break;
    }
    /// The caller's authority facts and pool teardown are the second half of admission, and the engine
    /// does not need to know which of the two refused. A query's or a background owner's stop never feeds it.
    if (liveness && !liveness())
        return Gate::FenceLost;
    return Gate::Ok;
}

std::optional<CasOperation::ReadStop> CasOperation::refusedReadAttempt(const Retry::Bound & bound) const
{
    const uint64_t reservation = reservedFor(0, 1);
    switch (gate(reservation))
    {
        case Gate::FenceLost: return ReadStop::FenceLost;
        case Gate::NoBudget:  return ReadStop::NoBudgetLease;
        case Gate::Ok: break;
    }
    if (!fits(reservation, bound))
        return ReadStop::PolicyExhausted;
    return std::nullopt;
}

uint64_t CasOperation::reservedFor(uint64_t sleep_ms, uint32_t envelopes) const
{
    uint64_t total = sleep_ms;
    for (uint32_t i = 0; i < envelopes; ++i)
        total = saturatingAdd(total, owner.attempt_reservation_ms);
    return total;
}

bool CasOperation::oneAttemptFits(const Retry & policy) const
{
    return fits(reservedFor(0, 1), owner.bindPolicy(policy));
}

bool CasOperation::oneAttemptFitsBefore(uint64_t deadline_ms) const
{
    return fits(reservedFor(0, 1), Retry::Bound{.deadline_ms = deadline_ms, .source = Retry::Bound::Source::OutsideCap});
}

std::optional<GaveUp> CasOperation::pauseAfterUnresolvedWrite(uint64_t ms, const GaveUp & unresolved, const Retry & policy)
{
    WriteState state;
    state.attempts_sent = unresolved.attempts_sent;
    state.sent_any = unresolved.sent_any;
    state.last_seen = unresolved.last_seen;
    return pauseBetweenDecisions(ms, state, policy, PauseAfter::Failure);
}

std::optional<CasOperation::PauseStop> CasOperation::pauseUnderPolicy(uint64_t ms, const Retry & policy, PauseAfter after,
                                                                      void (*record)(), bool zero_is_no_pause)
{
    /// Admission for `needed_ms` of pause, so a pause never outlives the mount lease. A window belongs to
    /// one request and the next request binds its own when it is sent, so only the policy's absolute limits
    /// can refuse a pause. The owner is checked last, so a lost fence or lease stays the typed outcome.
    auto check_pause = [&](uint64_t needed_ms) -> std::optional<PauseStop>
    {
        switch (gate(needed_ms))
        {
            case Gate::FenceLost: return PauseStop::FenceLost;
            case Gate::NoBudget:  return PauseStop::NoBudget;
            case Gate::Ok: break;
        }
        if (policy.lease_deadline_ms && !fits(needed_ms, {.deadline_ms = *policy.lease_deadline_ms, .source = Retry::Bound::Source::Lease}))
            return PauseStop::Lease;
        if (policy.outside_cap_ms && !fits(needed_ms, {.deadline_ms = *policy.outside_cap_ms, .source = Retry::Bound::Source::OutsideCap}))
            return PauseStop::OutsideCap;
        throwIfBackgroundStopped();
        if (after == PauseAfter::Failure && !insideOwed() && queryStopRequested())
            return PauseStop::QueryStopped;
        return std::nullopt;
    };
    if (auto stop = check_pause(ms))
        return stop;
    if (ms == 0 && zero_is_no_pause)
        return std::nullopt;
    record();
    /// A query cut short here is reported by the check below, after the typed bounds.
    if (after == PauseAfter::Failure)
        sleepUnlessQueryStops(ms, [this](uint64_t slice_ms) { sleep(slice_ms); });
    else
        sleep(ms);
    /// A sleep can end on or past a limit it fitted before, and `decide` must not run after it.
    return check_pause(0);
}

std::optional<GaveUp> CasOperation::pauseBetweenDecisions(uint64_t ms, WriteState & state, const Retry & policy,
                                                               PauseAfter after, void (*record)(), bool zero_is_no_pause)
{
    const std::optional<PauseStop> stop = pauseUnderPolicy(ms, policy, after, record, zero_is_no_pause);
    if (!stop)
        return std::nullopt;
    switch (*stop)
    {
        /// Named the way every request names a lost fence: by the bound the policy would get now.
        case PauseStop::FenceLost:  return gaveUp(GaveUp::Why::FenceLost, sourceFor(owner.bindPolicy(policy)), state);
        case PauseStop::NoBudget:   return gaveUp(GaveUp::Why::Deadline, GaveUp::Source::Lease, state);
        case PauseStop::Lease:      return gaveUp(GaveUp::Why::Deadline, GaveUp::Source::Lease, state);
        case PauseStop::OutsideCap: return gaveUp(GaveUp::Why::Deadline, GaveUp::Source::Policy, state);
        /// The failure the pause followed stays unresolved; no decision follows it.
        case PauseStop::QueryStopped: return gaveUp(GaveUp::Why::Unresolved, sourceFor(owner.bindPolicy(policy)), state);
    }
    UNREACHABLE();
}

bool CasOperation::fits(uint64_t needed_ms, const Retry::Bound & bound) const
{
    /// Strict at the boundary. A backend with no attempt timeout reserves 0 and full jitter can draw a
    /// 0 sleep, so a `needed <= remaining` test would keep issuing requests at and past the deadline --
    /// the one thing "no request after the boundary" promises never happens.
    const uint64_t now = owner.now_ms();
    if (now >= bound.deadline_ms)
        return false;
    return needed_ms <= bound.deadline_ms - now;
}

bool CasOperation::refreshAndClassifyReadFault(const std::exception & e, bool & refresh_attempted, bool & refreshed)
{
    /// A local failure -- a bad allocation, a logic error raised inside the attempt -- is not a
    /// transport fault, and reissuing it would spend the whole deadline replaying the same bug.
    if (!isStoreAnswer(e))
        return true;
    /// A credential failure gets ONE refresh per call -- the storage hands back a fresh client every
    /// time it is asked, so refreshing per attempt would reissue a permanent denial to the deadline.
    /// Without new credentials nothing would sign differently, so a read propagates rather than
    /// spending its policy on a request that cannot start succeeding.
    if (isRefreshableCredentialError(e))
    {
        if (refresh_attempted)
            return true;
        refresh_attempted = true;
        refreshed = owner.backend->refreshCredentials();
        return !refreshed;
    }
    /// The store's own answer decides. A refusal it proved never applied, and an authoritative absence,
    /// both replay identically; everything else -- a throttle, a 5xx, a missing bucket, an unmodeled
    /// name an S3-compatible store reports -- may still be transient.
    return isDefinitelyRefusedWrite(e) || isAuthoritativeAbsence(e);
}

void CasOperation::giveUpReadFenceLost(std::string_view verb, const String & subject, std::string_view when)
{
    last_read_stop = ReadStop::FenceLost;
    throwReadRefused(Fence::Admit::LostOrRearmed, verb, subject, when);
}

void CasOperation::giveUpReadNoBudget(std::string_view verb, const String & subject, std::string_view what)
{
    last_read_stop = ReadStop::NoBudgetLease;
    throwReadRefused(Fence::Admit::NoBudget, verb, subject, what);
}

void CasOperation::giveUpReadDeadline(std::string_view verb, const String & subject,
                                      GaveUp::Source source, uint32_t attempts_made)
{
    last_read_stop = ReadStop::PolicyExhausted;
    throwCasRequestRetryLater(fmt::format("{} of '{}': gave up at the {} deadline after {} attempt(s)",
                                          verb, subject, source == GaveUp::Source::Lease ? "lease" : "policy", attempts_made));
}

std::optional<Object> CasOperation::readUnder(const String & key, const Retry & policy, OnMemoryLimit on_memory_limit)
{
    return readLoop("read", key, policy, [&](auto & access) -> std::optional<Object>
    {
        auto raw = owner.backend->read(key, access);
        if (!raw)
            return std::nullopt;
        return Object{std::move(raw->bytes), owner.mint(key, std::move(raw->value))};
    }, on_memory_limit);
}

std::optional<Meta> CasOperation::headUnder(const String & key, const Retry & policy)
{
    return readLoop("head", key, policy, [&](auto & access) -> std::optional<Meta>
    {
        auto raw = owner.backend->head(key, access);
        if (!raw)
            return std::nullopt;
        return Meta{raw->size, owner.mint(key, std::move(raw->value))};
    });
}

ListPage CasOperation::listUnder(const String & prefix, const String & cursor, size_t limit, const Retry & policy)
{
    return readLoop("list", prefix, policy, [&](auto & access)
    {
        Backend::RawListPage raw = owner.backend->list(prefix, cursor, limit, access);
        ListPage page;
        page.next_cursor = std::move(raw.next_cursor);
        page.keys.reserve(raw.keys.size());
        for (auto & listed : raw.keys)
        {
            ListedKey entry{std::move(listed.key), listed.size, std::nullopt};
            if (listed.value)
                entry.etag = owner.mint(entry.key, std::move(*listed.value));
            page.keys.push_back(std::move(entry));
        }
        return page;
    });
}

Removal CasOperation::removeUnder(const String & key, const String & expected_value, const Retry & policy)
{
    /// A mutation, so unlike a read it does not start for a stopped background owner.
    throwIfBackgroundStopped();
    const Backend::RawRemoval raw = readLoop("remove", key, policy, [&](auto & access)
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

std::optional<Object> CasOperation::read(const String & key, const Retry & policy, OnMemoryLimit on_memory_limit)
{
    return readUnder(key, policy, on_memory_limit);
}

std::optional<Meta> CasOperation::head(const String & key, const Retry & policy)
{
    return headUnder(key, policy);
}

ListPage CasOperation::list(const String & prefix, const String & cursor, size_t limit, const Retry & policy)
{
    return listUnder(prefix, cursor, limit, policy);
}

void CasOperation::forEachListedKey(const String & prefix, const ListedKeyFn & fn, const Retry & per_page,
                                    size_t page_limit, const std::function<void()> & on_page_fetched)
{
    String cursor;
    for (;;)
    {
        ListPage page = list(prefix, cursor, page_limit, per_page);
        if (on_page_fetched)
            on_page_fetched();
        for (const ListedKey & entry : page.keys)
            if (!fn(entry))
                return;
        if (page.next_cursor.empty())
            return;
        cursor = std::move(page.next_cursor);
    }
}

void CasOperation::removeManyWriteOnce(const std::vector<WriteOnceKey> & keys, const Retry & policy)
{
    if (keys.size() > kBulkDeleteMaxKeys)
        throw Exception(ErrorCodes::LOGICAL_ERROR,
            "CAS removeManyWriteOnce: {} keys in one chunk, the limit is {}; the consumer chunks its input",
            keys.size(), kBulkDeleteMaxKeys);
    if (keys.empty())
        return;
    throwIfBackgroundStopped();
    const String subject = fmt::format("{} (+{} keys)", keys.front().str(), keys.size() - 1);
    readLoop("removeManyWriteOnce", subject, policy, [&](auto & access)
    {
        owner.backend->removeManyWriteOnce(keys, access);
        return true;
    });
}

Removal CasOperation::remove(const String & key, const Etag & seen, const Retry & policy)
{
    return removeUnder(key, owner.valueFor(key, seen), policy);
}

Removal CasOperation::removeCurrent(const String & key, const Retry & policy)
{
    for (uint32_t attempt = 1;; ++attempt)
    {
        const std::optional<Meta> seen = headUnder(key, policy);
        if (!seen)
            return Removal::Gone;
        const Removal removed = removeUnder(key, owner.valueFor(key, seen->etag), policy);
        if (removed != Removal::Mismatch)
            return removed;

        /// A `Mismatch` is a VALUE, so it never reaches the fault check inside the two loops above: this
        /// verb has to honour `once` itself, and its contract is that it never hands a `Mismatch` back.
        /// The message says what actually stopped it, which is the policy and not the deadline.
        if (policy.single_attempt)
            throwCasRequestRetryLater(fmt::format(
                "removeCurrent of '{}': the observed incarnation was replaced and the policy allows no reissue", key));

        if (attempt == kCasMaxDecisions)
        {
            if (const std::optional<AdmissionLoss> lost = lostAdmission())
            {
                if (*lost == AdmissionLoss::Fence)
                    giveUpReadFenceLost("removeCurrent", key, "at the decision cap");
                giveUpReadNoBudget("removeCurrent", key, "at the decision cap");
            }
            throwDidNotConverge(fmt::format("CAS remove of '{}'", key), kCasMaxDecisions, "decisions");
        }

        /// Another incarnation became current between the observation and the delete: a lost race. Re-observe
        /// after the conflict pause; the next `head` and `remove` bind their own windows.
        if (const std::optional<PauseStop> stop
            = pauseUnderPolicy(Retry::conflictBackoff(attempt), policy, PauseAfter::Conflict, detail::recordReissue,
                               /*zero_is_no_pause=*/true))
        {
            switch (*stop)
            {
                case PauseStop::FenceLost:  giveUpReadFenceLost("removeCurrent", key, "before the reissue");
                case PauseStop::NoBudget:   giveUpReadNoBudget("removeCurrent", key, "for the reissue");
                case PauseStop::Lease:      giveUpReadDeadline("removeCurrent", key, GaveUp::Source::Lease, attempt);
                case PauseStop::OutsideCap: giveUpReadDeadline("removeCurrent", key, GaveUp::Source::Policy, attempt);
                /// Only a pause after a failure reports a stopped query, and this one follows a conflict.
                case PauseStop::QueryStopped: UNREACHABLE();
            }
        }
    }
}

SentinelProbeResult CasOperation::probeSentinel(const String & key, const Retry & policy)
{
    const Retry::Bound bound = owner.bindPolicy(policy);
    bool refresh_attempted = false;
    std::optional<SentinelProbeResult> last_probe;
    for (uint32_t attempt = 1;; ++attempt)
    {
        /// Not before the first probe: a healthy probe is never refused.
        if (attempt > 1)
            throwIfBackgroundStopped();
        const uint64_t reservation = reservedFor(0, 1);
        switch (gate(reservation))
        {
            case Gate::FenceLost: giveUpReadFenceLost("probeSentinel", key, "before the request");
            case Gate::NoBudget:  giveUpReadNoBudget("probeSentinel", key, "for one more request");
            case Gate::Ok: break;
        }
        if (!fits(reservation, bound))
        {
            /// A pause that fit can still end on the boundary. Once a probe has run, its outcome is the
            /// answer; before the first one the caller is told no request happened.
            if (last_probe)
                return *last_probe;
            giveUpReadDeadline("probeSentinel", key, sourceFor(bound), attempt - 1);
        }

        detail::recordAttempt();
        SentinelProbeResult result{ProbeOutcome::Indeterminate, std::nullopt};
        std::exception_ptr cause;
        try
        {
            result = owner.withTransportAccess(attempt, [&](auto & access)
            {
                return owner.backend->probeSentinelRaw(key, access);
            });
        }
        catch (const std::exception & e)
        {
            /// A local fault surfaces unchanged, as in the write loop; a query's stored cause can carry any code.
            if (!isStoreAnswer(e) && !isQueryStoredCause(std::current_exception()))
                throw;
            /// A stopped owner's failure is no store answer: neither reissued nor a credential refresh. A stopped
            /// query keeps the failure.
            rethrowIfStoppedOwnersFailure();
            /// The probe never counts a fuse, so whether this fault was a credential reissue is not
            /// interesting here -- only the write and read loops guard a counter with it.
            bool refreshed = false;
            if (refreshAndClassifyReadFault(e, refresh_attempted, refreshed))
                throw;
            /// The probe reports every transport failure as `Indeterminate` rather than by throwing, so
            /// a decorator that does throw is folded onto the same inconclusive outcome.
            cause = std::current_exception();
        }

        /// The native and default backends fold a stopped owner's exception into `Indeterminate`, so the
        /// background verdict is recovered here. A stopped query gets the inconclusive outcome itself, or its cause,
        /// and no reissue. An authoritative outcome stays a store answer.
        if (result.outcome == ProbeOutcome::Indeterminate)
        {
            if (auto stop = backgroundStopRequested())
                std::rethrow_exception(stop);
            if (queryStopRequested())
            {
                if (cause)
                    std::rethrow_exception(cause);
                return result;
            }
        }

        /// The reissue is driven by the OUTCOME: this is the one primitive whose failures never reach a
        /// `catch`, and `Indeterminate` means "inconclusive", which is exactly what a retry resolves.
        /// The other four outcomes are authoritative answers and return on the first attempt.
        if (result.outcome != ProbeOutcome::Indeterminate || policy.single_attempt)
            return result;

        const uint64_t pause_ms = Retry::backoff(attempt);
        const uint64_t needed = reservedFor(pause_ms, 1);
        /// A lost fence and a spent lease budget are not things the store said, so they are reported the
        /// way every other read verb reports them rather than being dressed up as an outcome.
        switch (gate(needed))
        {
            case Gate::FenceLost: giveUpReadFenceLost("probeSentinel", key, "before the reissue");
            case Gate::NoBudget:  giveUpReadNoBudget("probeSentinel", key, "for the reissue");
            case Gate::Ok: break;
        }
        /// Only the policy's own bound ends the loop with a value: a probe DID run, and what it saw is
        /// more than a give-up exception could say. Every consumer treats `Indeterminate` fail-closed.
        if (!fits(needed, bound))
            return result;
        last_probe = std::move(result);
        detail::recordReissue();
        if (!sleepUnlessQueryStops(pause_ms, [this](uint64_t slice_ms) { sleep(slice_ms); }))
        {
            if (cause)
                std::rethrow_exception(cause);
            return *last_probe;
        }
    }
}

std::unique_ptr<ReadBuffer> CasOperation::stream(const String & key, const Retry & policy)
{
    std::unique_ptr<ReadBuffer> body = readLoop("stream", key, policy, [&](auto & access)
    {
        return owner.backend->stream(key, access);
    });
    if (!body)
        return nullptr;   /// absent: the open already answered
    /// By value, deliberately: nothing the buffer holds may reference this operation or its owner.
    return std::make_unique<AdmittedBodyReadBuffer>(std::move(body), key, owner.fence.admit,
                                                    admitted_generation, liveness);
}

void CasOperation::publish(const BlobPublishRequest & request, const Retry & policy)
{
    throwIfBackgroundStopped();
    readLoop("publish", request.destination_key, policy, [&](auto & access)
    {
        owner.backend->publish(request, access);
    });
}

CasOperation::Resolved CasOperation::observe(const String & key, const Retry & policy, ResolveWith what)
{
    last_read_stop.reset();
    try
    {
        if (what == ResolveWith::Presence)
        {
            if (auto meta = headUnder(key, policy))
                return {std::move(*meta), std::nullopt};
        }
        else if (auto object = readUnder(key, policy))
            return {std::move(*object), std::nullopt};
        return {ProvenAbsent{}, std::nullopt};
    }
    catch (const std::exception & e)
    {
        /// A local bug replays identically on every reissue, so it is never swallowed into "nothing
        /// observed". A stopped owner's failure is not a store answer: it is reported only when a bound
        /// refused the read or the read is owed. `last_read_stop` says which bound refused it.
        bool observed_stop = false;
        if (!isStoreAnswer(e) && !isQueryStoredCause(std::current_exception()))
            throw;
        if (auto stop = backgroundStopRequested())
        {
            /// The verdict, not `e`: the stop can land after the read's own check classified `e`.
            if (!last_read_stop && !insideOwed())
                std::rethrow_exception(stop);
        }
        else if (queryStopRequested())
        {
            observed_stop = true;
            if (!last_read_stop && !insideOwed())
                throw;
        }
        else if (!isStoreAnswer(e))
            throw;
        return {NotObserved{}, last_read_stop, last_read_source, observed_stop};
    }
}

GaveUp CasOperation::gaveUp(GaveUp::Why why, GaveUp::Source source, WriteState & state) const
{
    ProfileEvents::increment(ProfileEvents::CASRequestGaveUp);
    return GaveUp{why, source, state.sent_any, state.last_seen, state.attempts_sent};
}

WriteResult CasOperation::gaveUpForReadStop(ReadStop stop, GaveUp::Source source, WriteState & state) const
{
    switch (stop)
    {
        case ReadStop::FenceLost:
            return gaveUp(GaveUp::Why::FenceLost, source, state);
        case ReadStop::NoBudgetLease:
            /// The fence's budget IS the mount lease, so the lease is the bound that ended this call.
            return gaveUp(GaveUp::Why::Deadline, GaveUp::Source::Lease, state);
        case ReadStop::PolicyExhausted:
            return gaveUp(GaveUp::Why::Deadline, source, state);
    }
    UNREACHABLE();
}

WriteResult CasOperation::gaveUpAfterFailedObservation(const Resolved & resolved, WriteState & state) const
{
    if (resolved.stop)
        return gaveUpForReadStop(*resolved.stop, resolved.source, state);
    /// No bound refused; the read itself failed. That is what `Unresolved` names, and it is the honest
    /// answer -- claiming a deadline the clock never reached would misreport which bound to widen.
    return gaveUp(GaveUp::Why::Unresolved, resolved.source, state);
}

std::optional<CasOperation::AdmissionLoss> CasOperation::lostAdmission() const
{
    switch (gate(0))
    {
        case Gate::FenceLost: return AdmissionLoss::Fence;
        case Gate::NoBudget:  return AdmissionLoss::Lease;
        case Gate::Ok:        return std::nullopt;
    }
    UNREACHABLE();
}

WriteResult CasOperation::gaveUpAfterDecisionCap(WriteState & state, const Retry & policy) const
{
    if (const std::optional<AdmissionLoss> lost = lostAdmission())
        return *lost == AdmissionLoss::Fence
            ? gaveUp(GaveUp::Why::FenceLost, sourceFor(owner.bindPolicy(policy)), state)
            : gaveUp(GaveUp::Why::Deadline, GaveUp::Source::Lease, state);
    return gaveUp(GaveUp::Why::Attempts, GaveUp::Source::Policy, state);
}

CasOperation::OnCleanRefusal CasOperation::onCleanRefusalBefore(uint32_t decisions, const Retry & policy)
{
    /// A decision that another follows after a conflict pause reads again after that pause; the last
    /// one, and a single attempt, report what the settling read saw.
    return policy.single_attempt || decisions == kCasMaxDecisions ? OnCleanRefusal::Settle : OnCleanRefusal::ReturnUnobserved;
}

std::optional<WriteResult> CasOperation::pauseAfterConflict(uint32_t decisions, WriteState & state, const Retry & policy)
{
    if (decisions == kCasMaxDecisions)
        return gaveUpAfterDecisionCap(state, policy);
    /// A clean conflict is paced by the call's run of consecutive clean ones, which a conflict that settled a
    /// transport fault ends; that one is paced by the fault schedule. Either way the conflict is an answer, so the
    /// pause does not consult the query. The first clean conflict of a run is retried at once, after the same checks.
    if (state.any_ambiguous)
    {
        state.clean_conflicts = 0;
        return pauseBetweenDecisions(reissuePause(state, Retry::backoff(++state.reissues)), state, policy, PauseAfter::Conflict);
    }
    return pauseBetweenDecisions(reissuePause(state, Retry::conflictBackoff(++state.clean_conflicts)), state, policy,
                                 PauseAfter::Conflict, detail::recordConflictPause, /*zero_is_no_pause=*/true);
}

WriteResult CasOperation::postCommit(Etag inc, bool resolved_by_read, WriteState & state, const Retry::Bound & bound)
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

CasOperation::PauseOutcome CasOperation::gatedPause(uint64_t pause_ms, uint32_t envelopes, WriteState & state,
                                                    const Retry::Bound & bound, void (*record)(), bool should_sleep)
{
    const uint64_t needed = reservedFor(pause_ms, envelopes);
    switch (gate(needed))
    {
        case Gate::FenceLost: return gaveUp(GaveUp::Why::FenceLost, sourceFor(bound), state);
        case Gate::NoBudget:  return gaveUp(GaveUp::Why::Deadline, GaveUp::Source::Lease, state);
        case Gate::Ok: break;
    }
    if (!fits(needed, bound))
        return gaveUp(GaveUp::Why::Deadline, sourceFor(bound), state);
    record();
    /// `should_sleep` is false ONLY for the fuse's zero-pause reissue: that pause is not "zero
    /// milliseconds", it is NO PAUSE AT ALL, so it must not call `sleep_ms` even with a zero argument.
    /// The two ordinary callers always sleep, even when a drawn jitter happens to be exactly zero --
    /// `Retry::backoff`'s full jitter includes zero.
    const bool query_stopped = should_sleep
        ? !sleepUnlessQueryStops(pause_ms, [this](uint64_t slice_ms) { sleep(slice_ms); })
        : !insideOwed() && queryStopRequested();
    if (!query_stopped)
        return ResumeAfterPause{};
    state.observed_stop = true;
    return QueryVetoedResend{};
}

uint64_t CasOperation::reissuePause(const WriteState & state, uint64_t unspaced_ms) const
{
    if (!state.spacing)
        return unspaced_ms;
    return Retry::pauseAfterAttempt(state.spacing->spacing_ms, state.spacing->attempt_started_ms, owner.now_ms());
}

CasOperation::PauseOutcome CasOperation::pauseAndReissue(WriteState & state, const Retry::Bound & bound)
{
    const uint64_t pause_ms = reissuePause(state, Retry::backoff(++state.reissues));
    return gatedPause(pause_ms, 2, state, bound, detail::recordReissue, /*should_sleep=*/true);
}

/// A flat pause before reissuing an attempt whose failure text named a failed connection.
static constexpr uint64_t kConnectHintPauseMs = 50;

CasOperation::PauseOutcome CasOperation::pauseFlat(WriteState & state, const Retry::Bound & bound)
{
    return gatedPause(reissuePause(state, kConnectHintPauseMs), 2, state, bound, detail::recordReissue, /*should_sleep=*/true);
}

CasOperation::PauseOutcome CasOperation::reissueAtOnce(WriteState & state, const Retry::Bound & bound)
{
    return gatedPause(0, 2, state, bound, detail::recordReissue, /*should_sleep=*/false);
}

CasOperation::Resolved CasOperation::settlingRead(const String & key, const Retry & policy, const WriteState & state,
                                                  ResolveWith resolve_refusal_with)
{
    ProfileEvents::increment(ProfileEvents::CASRequestResolveRead);
    /// Owed to the write: a stopped owner must not lose the proof of what its attempt did.
    const auto owed = OwnerScope::owed();
    return observe(key, policy, state.any_ambiguous ? ResolveWith::Body : resolve_refusal_with);
}

std::optional<WriteResult> CasOperation::adjudicate(const Resolved & resolved, const String & bytes,
                                                    const std::optional<Etag> & expected, WriteState & state,
                                                    const Retry::Bound & write_bound)
{
    state.last_seen = resolved.seen;
    state.observed_stop = state.observed_stop || resolved.observed_stop;
    /// A bound of the resolve read refused it, so say WHICH. Erasing it here is what let a lost
    /// fence be reported as an ordinary conflict and a lease refusal as a policy deadline.
    if (resolved.stop)
        return gaveUpForReadStop(*resolved.stop, resolved.source, state);

    /// No attempt of this inner write is unresolved, so the refused precondition IS the answer.
    /// Identical bytes here are somebody else's object, and the caller that owns the key's meaning
    /// decides what that means.
    if (!state.any_ambiguous)
        return Conflict{state.last_seen, state.attempts_sent, state.any_ambiguous, state.observed_stop};

    if (!preconditionStillSatisfiable(state.last_seen, expected))
    {
        /// The precondition has MOVED, and THAT is what makes byte equality a proof: an attempt
        /// that never applied leaves the key exactly as it found it, so under an incarnation that
        /// did not move our own bytes cannot be told from the bytes already there. A create reaches
        /// here for every object it sees -- an occupied key never satisfies its precondition.
        if (const auto * obj = std::get_if<Object>(&state.last_seen); obj && obj->bytes == bytes)
            return postCommit(obj->etag, /*resolved_by_read=*/true, state, write_bound);
        /// Nothing of this inner write's is at the key, and a reissue would be refused too.
        return Conflict{state.last_seen, state.attempts_sent, state.any_ambiguous, state.observed_stop};
    }
    return std::nullopt;
}

namespace
{

/// What one conditional write attempt proved, from its reply alone.
enum class AttemptKind : uint8_t
{
    Committed,
    /// The precondition was refused: this attempt wrote nothing.
    Conflict,
    /// Refused before anything was written, the same way on every resend.
    Refused,
    /// Refused before anything was written; fresh credentials may let a resend through.
    CredentialRefused,
    /// No proof either way: the attempt may have landed.
    Ambiguous,
    /// The query's stored verdict replaced the store's answer, which may have landed.
    QueryStoredCause,
    /// Not a store answer: surfaced unchanged.
    LocalFault,
};

struct WriteAttempt
{
    explicit WriteAttempt(AttemptKind kind_) : kind(kind_) {}

    AttemptKind kind;
    /// For `Committed`.
    std::optional<Etag> landed;
    /// For every thrown kind.
    std::exception_ptr error;
    /// For `Refused` and `CredentialRefused`.
    int refusal_code = 0;
    String refusal_message;
    /// An owner had asked to stop when the attempt failed.
    bool failed_after_stop = false;
    /// For a thrown store answer: the text named a failed connection.
    bool connect_hint = false;
    /// For `Ambiguous`: the first attempt met the adaptive timeout.
    bool fuse = false;
};

/// Only the transport can have landed anything, and every exception it raises is a `Poco::Exception`; a local fault
/// settled by a read would bury the bug behind an outcome the store never gave.
WriteAttempt classifyWriteFailure(const std::exception & e, size_t attempt_no)
{
    WriteAttempt attempt(AttemptKind::Ambiguous);
    attempt.error = std::current_exception();
    attempt.failed_after_stop = ownerStopRequested() != nullptr;
    if (isQueryStoredCause(attempt.error))
        attempt.kind = AttemptKind::QueryStoredCause;
    else if (!isStoreAnswer(e))
        attempt.kind = AttemptKind::LocalFault;
    else
    {
        attempt.connect_hint = isConnectFailureHint(e);
        if (isDefinitelyRefusedWrite(e))
        {
            attempt.kind = isRefreshableCredentialError(e) ? AttemptKind::CredentialRefused : AttemptKind::Refused;
            const auto & refusal = dynamic_cast<const Exception &>(e);
            attempt.refusal_code = refusal.code();
            attempt.refusal_message = refusal.message();
        }
        else
        {
            /// False when the text is also a hint: such an attempt stays a hint.
            attempt.fuse = isFirstAttemptFuseTimeout(e, attempt_no);
        }
    }
    return attempt;
}

/// `on_failure` takes the exception object, which only a rethrow recovers from an `exception_ptr`.
void reportWriteFailure(const RequestObserver & observer, const String & key, const std::exception_ptr & error)
{
    if (!observer.on_failure)
        return;
    try
    {
        std::rethrow_exception(error);
    }
    catch (const std::exception & e)
    {
        observer.on_failure("PUT", key, e);
    }
}

[[noreturn]] void throwVetoedCredentialRetryOfWrite(const String & key, const std::exception_ptr & refusal)
{
    try
    {
        std::rethrow_exception(refusal);
    }
    catch (const std::exception & e)
    {
        detail::throwVetoedCredentialRetry("PUT", key, e);
    }
}

}

WriteResult CasOperation::writeLoop(const String & key, const String & bytes, const std::optional<Etag> & expected,
                                    const Retry & policy, WriteState & state, ResolveWith resolve_refusal_with,
                                    OnCleanRefusal on_clean_refusal)
{
    const Retry::Bound bound = owner.bindPolicy(policy);
    std::optional<String> expected_value;
    if (expected)
        expected_value = owner.valueFor(key, *expected);

    /// This inner write's own bytes are what an ambiguity of it could have landed. A previous inner
    /// write of the same call sent DIFFERENT bytes and ended in a conflict, which proved its attempts
    /// dead -- carrying its ambiguity forward is how a competitor's identical object gets claimed.
    state.any_ambiguous = false;
    /// False while an ambiguous attempt awaits its settling read: a connect hint resends before reading.
    bool latest_attempt_settled = true;
    /// A stopped owner gets its latest attempt settled instead of a reissue.
    const auto settle_for_stopped_owner = [&]() -> WriteResult
    {
        if (!latest_attempt_settled)
            if (auto settled = adjudicate(settlingRead(key, policy, state, ResolveWith::Body), bytes, expected, state, bound))
                return *settled;
        return gaveUp(GaveUp::Why::Unresolved, sourceFor(bound), state);
    };

    const auto send_attempt = [&]() -> WriteAttempt
    {
        std::optional<std::expected<String, Backend::RawConflict>> reply;
        try
        {
            reply = owner.withTransportAccess(state.attempts_sent + policy.attempts_already_sent, [&](auto & access)
            {
                return owner.backend->write(key, bytes, expected_value, access);
            });
        }
        catch (const std::exception & e)
        {
            return classifyWriteFailure(e, state.attempts_sent + policy.attempts_already_sent);
        }
        if (!reply->has_value())
            return WriteAttempt(AttemptKind::Conflict);
        /// A 2xx carrying a value no grammar accepts may well have landed: an ambiguity, never a corruption verdict.
        std::optional<Etag> landed = owner.tryMint(key, std::move(**reply));
        WriteAttempt attempt(landed ? AttemptKind::Committed : AttemptKind::Ambiguous);
        attempt.landed = std::move(landed);
        return attempt;
    };

    for (;;)
    {
        /// A query that stopped during a failure pause gets the latest attempt settled instead of a reissue.
        if (state.observed_stop)
            return settle_for_stopped_owner();
        /// Once an attempt may have landed, a stopped background owner gets that attempt settled instead of its
        /// verdict. A stopped query is consulted only on the failure path below, which records what it saw.
        if (!state.any_ambiguous)
            throwIfBackgroundStopped();
        else if (!insideOwed() && backgroundStopRequested())
            return settle_for_stopped_owner();

        /// An attempt starts only with room for TWO envelopes: itself and the read that settles it. This
        /// bounds when the attempt starts, not when it lands or whether its settling read completes.
        const uint64_t reservation = reservedFor(0, 2);
        switch (gate(reservation))
        {
            case Gate::FenceLost: return gaveUp(GaveUp::Why::FenceLost, sourceFor(bound), state);
            case Gate::NoBudget:  return gaveUp(GaveUp::Why::Deadline, GaveUp::Source::Lease, state);
            case Gate::Ok: break;
        }
        if (!fits(reservation, bound))
            return gaveUp(GaveUp::Why::Deadline, sourceFor(bound), state);

        if (policy.attempt_spacing_ms)
            state.spacing = WriteState::Spacing{*policy.attempt_spacing_ms, owner.now_ms()};
        detail::recordAttempt();
        ++state.attempts_sent;
        state.sent_any = true;
        if (observer.on_write_attempt)
            observer.on_write_attempt(state.attempts_sent);

        /// "This attempt did not land" says nothing about an earlier attempt of this write whose reply was lost.
        const bool earlier_attempt_unanswered = state.any_ambiguous;
        WriteAttempt attempt = send_attempt();
        const bool refused = attempt.kind == AttemptKind::Refused || attempt.kind == AttemptKind::CredentialRefused;

        if (attempt.kind == AttemptKind::LocalFault)
            std::rethrow_exception(attempt.error);
        if (attempt.kind == AttemptKind::Committed)
            return postCommit(std::move(*attempt.landed), /*resolved_by_read=*/false, state, bound);

        /// Nothing is resent after the owner stopped: a failure that may have landed, or a refusal that cannot answer
        /// for an earlier unanswered attempt, is settled and ends the call. A refusal with nothing unanswered is kept.
        if (attempt.failed_after_stop && !(refused && !earlier_attempt_unanswered))
        {
            state.any_ambiguous = true;
            latest_attempt_settled = false;
            state.observed_stop = state.observed_stop || queryStopRequested();
            return settle_for_stopped_owner();
        }

        if (attempt.error)
            reportWriteFailure(observer, key, attempt.error);
        bool refreshed = false;
        if (refused)
        {
            /// One refresh per call, and only when a resend could sign with what it installs.
            if (attempt.kind == AttemptKind::CredentialRefused && !attempt.failed_after_stop && !state.refresh_attempted
                && !policy.single_attempt)
            {
                state.refresh_attempted = true;
                refreshed = owner.backend->refreshCredentials();
            }
            /// With an earlier attempt unanswered the refusal proves nothing about it, so the read below settles it.
            if (!earlier_attempt_unanswered)
            {
                if (refreshed)
                {
                    PauseOutcome paused = pauseAndReissue(state, bound);
                    if (auto * ended = std::get_if<WriteResult>(&paused))
                        return std::move(*ended);
                    /// The store refused this attempt, so a vetoed resend leaves no outcome unknown.
                    if (std::holds_alternative<QueryVetoedResend>(paused))
                        throwVetoedCredentialRetryOfWrite(key, attempt.error);
                    continue;
                }
                ProfileEvents::increment(ProfileEvents::CASRequestRefused);
                return Refused{attempt.refusal_code, std::move(attempt.refusal_message), state.attempts_sent};
            }
        }
        else if (attempt.kind != AttemptKind::Conflict)
        {
            state.any_ambiguous = true;
            latest_attempt_settled = false;
        }

        /// A refusal that fresh credentials may cure is resent like an ambiguity. Counted whether or not the policy
        /// or the gates let the resend happen.
        const bool connect_hint = attempt.connect_hint && (attempt.kind == AttemptKind::Ambiguous || refreshed);
        if (connect_hint)
            ProfileEvents::increment(ProfileEvents::CASRequestConnectFailureHint);
        if (attempt.fuse)
            ProfileEvents::increment(ProfileEvents::CASRequestFirstAttemptFuse);
        /// A read now would meet the same broken connection, so the resend is the cheaper probe; a refused
        /// precondition it meets is settled by the read below.
        if (connect_hint && !policy.single_attempt && policy.resend_unresolved)
        {
            PauseOutcome paused = pauseFlat(state, bound);
            if (auto * ended = std::get_if<WriteResult>(&paused))
                return std::move(*ended);
            if (std::holds_alternative<QueryVetoedResend>(paused))
                return settle_for_stopped_owner();
            continue;
        }

        if (attempt.kind == AttemptKind::Conflict && !earlier_attempt_unanswered
            && on_clean_refusal == OnCleanRefusal::ReturnUnobserved)
        {
            state.last_seen = NotObserved{};
            /// The skipped read would have bound its own window and been refused by these, before any pause the
            /// caller takes; they are reported as that read reports them.
            const Retry::Bound read_bound = owner.bindPolicy(policy);
            if (const std::optional<ReadStop> read_refused = refusedReadAttempt(read_bound))
                return gaveUpForReadStop(*read_refused, sourceFor(read_bound), state);
            return Conflict{state.last_seen, state.attempts_sent, state.any_ambiguous, state.observed_stop};
        }

        /// One exact read settles every other refused precondition and every ambiguity: a refused precondition does
        /// not say WHO holds the key. A presence-only caller settles a refusal with a HEAD; proving
        /// an ambiguous attempt landed needs the bytes.
        if (auto settled = adjudicate(settlingRead(key, policy, state, resolve_refusal_with), bytes, expected, state, bound))
            return *settled;
        latest_attempt_settled = true;

        /// Unresolved: the precondition would still be met, or the read observed nothing. Re-sending the same bytes
        /// under the same precondition is safe: it ends at the store's own answer.
        if (policy.single_attempt || !policy.resend_unresolved)
            return gaveUp(GaveUp::Why::Unresolved, sourceFor(bound), state);
        /// A stop during the settling read ends the call here instead of after a pause.
        if (state.observed_stop || (!insideOwed() && ownerStopRequested()))
            return gaveUp(GaveUp::Why::Unresolved, sourceFor(bound), state);
        /// A first-attempt timeout is a connection-quality answer, not a store fault: resend at once.
        PauseOutcome paused = attempt.fuse ? reissueAtOnce(state, bound) : pauseAndReissue(state, bound);
        if (auto * ended = std::get_if<WriteResult>(&paused))
            return std::move(*ended);
        if (std::holds_alternative<QueryVetoedResend>(paused))
            return settle_for_stopped_owner();
    }
}

WriteResult CasOperation::create(const String & key, const String & bytes, const Retry & policy)
{
    WriteState state;
    return writeLoop(key, bytes, std::nullopt, policy, state, ResolveWith::Body);
}

WriteResult CasOperation::replace(const String & key, const String & bytes, const Etag & seen, const Retry & policy)
{
    WriteState state;
    return writeLoop(key, bytes, seen, policy, state, ResolveWith::Body);
}

template <typename Seen>
WriteResult CasOperation::readModifyWriteImpl(
    const String & key, const std::function<std::optional<String>(const std::optional<Seen> &)> & decide, const Retry & policy)
{
    constexpr bool presence_only = std::is_same_v<Seen, Meta>;
    constexpr ResolveWith observe_with = presence_only ? ResolveWith::Presence : ResolveWith::Body;
    /// Proving an ambiguous attempt landed needs the bytes even here; a presence-only verb reports metadata only.
    const auto reported = [](auto value)
    {
        if constexpr (presence_only)
            return withoutBody(std::move(value));
        else
            return value;
    };

    WriteState state;
    for (uint32_t decisions = 1;; ++decisions)
    {
        /// A decision after the first is reached only through the conflict pause, and reads after it: a write
        /// landing during the pause would refuse a decision taken on any read before it.
        if (decisions > 1)
            if (const std::optional<WriteResult> given_up = pauseAfterConflict(decisions - 1, state, policy))
                return *given_up;

        const Resolved resolved = observe(key, policy, observe_with);
        state.last_seen = resolved.seen;
        std::optional<Seen> current;
        if (const auto * seen = std::get_if<Seen>(&state.last_seen))
            current = *seen;
        else if (!std::holds_alternative<ProvenAbsent>(state.last_seen))
            return gaveUpAfterFailedObservation(resolved, state);

        throwIfBackgroundStopped();
        /// Outside every classification: `decide` is the caller's own control flow, and an exception
        /// from it is theirs to see unchanged.
        const std::optional<String> next = decide(current);
        if (!next)
            return Declined{state.last_seen};

        WriteResult result = writeLoop(key, *next, current ? std::optional<Etag>(current->etag) : std::nullopt, policy, state,
            observe_with, onCleanRefusalBefore(decisions, policy));
        state.last_seen = reported(std::move(state.last_seen));
        if (!std::holds_alternative<Conflict>(result) || policy.single_attempt || state.observed_stop)
            return reported(std::move(result));
    }
}

WriteResult CasOperation::readModifyWrite(const String & key, const DecideOnObject & decide, const Retry & policy)
{
    return readModifyWriteImpl<Object>(key, decide, policy);
}

WriteResult CasOperation::readModifyWriteOnPresence(const String & key, const DecideOnMeta & decide, const Retry & policy)
{
    return readModifyWriteImpl<Meta>(key, decide, policy);
}

}
