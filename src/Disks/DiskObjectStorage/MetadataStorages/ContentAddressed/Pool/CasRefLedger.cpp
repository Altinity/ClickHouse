#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasRefLedger.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Formats/CasGcStateFormat.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Backend/CasRequests.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasKeyReader.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasRefCatalog.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasServerRoot.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Primitives/CasCodecUtil.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Formats/CasTextFormat.h>
#include <IO/ReadBufferFromMemory.h>
#include <IO/ReadHelpers.h>
#include <IO/WriteBufferFromString.h>
#include <IO/WriteHelpers.h>
#include <Common/Exception.h>
#include <Common/logger_useful.h>
#include <Common/MemoryTracker.h>
#include <Common/ProfileEvents.h>
#include <Common/setThreadName.h>
#include <Common/ThreadPool.h>
#include <base/defines.h>
#include <base/sleep.h>
#include <fmt/format.h>
#include <algorithm>
#include <chrono>
#include <thread>
#include <type_traits>
#include <unordered_set>

namespace DB
{
namespace ErrorCodes
{
    extern const int ABORTED;
    extern const int CORRUPTED_DATA;
    extern const int FILE_DOESNT_EXIST;
    extern const int INVALID_STATE;
    extern const int LIMIT_EXCEEDED;
    extern const int LOGICAL_ERROR;
    extern const int NETWORK_ERROR;
    extern const int S3_ERROR;
    extern const int POCO_EXCEPTION;
    extern const int UNKNOWN_FORMAT_VERSION;
    extern const int SOCKET_TIMEOUT;
    extern const int CANNOT_READ_FROM_SOCKET;
    extern const int TIMEOUT_EXCEEDED;
}
}

namespace ProfileEvents
{
    extern const Event CASRefAppendTombstoneWritten;
    extern const Event CASRefBatchFlushes;
    extern const Event CASRefBatchedMutations;
    extern const Event CASRefBatchScopeCuts;
    extern const Event CASRefQueueWaitMicroseconds;
    extern const Event CASRefRecoveryRestarts;
    extern const Event CASRefRecoveryRetries;
    extern const Event CASRefAppendWedged;
    extern const Event CASRefAppendPreAttemptRefused;
    extern const Event CASRefAppendUnwedged;
    extern const Event CASRefAppendDefiniteFailure;
    extern const Event CASRefAppendSealRejected;
    extern const Event CASRefAppendOccupantUnreadable;
    extern const Event CASRelinkConfirmRefusedRefMutationInFlight;
    extern const Event CASRelinkConfirmRefusedLaneWedged;
    extern const Event CASRelinkConfirmRefusedLaneBroken;
    extern const Event CASRelinkConfirmRefusedStateLockBusy;
    extern const Event CASRelinkConfirmRefusedMountCannotSpeak;
    extern const Event CASRefNeedsRecovery;
    extern const Event CASRefSweepDeferred;
    extern const Event CASRefSweepRearmed;
    extern const Event CASRefStalePrecommitsReclaimed;
    extern const Event CASRefTableEvictions;
    extern const Event CASRefSnapshotPutBytes;
    extern const Event CASRefSnapshotTailLogs;
    extern const Event CASRefSnapshotPublishDispatched;
    extern const Event CASRefSnapshotPublishBackoff;
    extern const Event CASRefCheckpointPublished;
    extern const Event CASRefCheckpointIdenticalSkip;
    extern const Event CASRefCheckpointNotAdvanced;
    extern const Event CASRefRecoveryEpochSealed;
    extern const Event CASRefRecoveryEpochSealAdopted;
    extern const Event CASRefRecoveryStragglerAdopted;
    extern const Event CASRefRecoveryCancelled;
    extern const Event CASRefRecoveryStreamHole;
}

namespace DB::Cas
{

namespace
{
/// Closing clauses for a write that was not attempted because an earlier transaction of the same
/// table is unresolved. Each carries the outcome phrase the earlier transaction's own writer saw.
constexpr std::string_view outcome_unknown_tail = "it may or may not take effect, and retrying the earlier write may duplicate it; this write was not attempted";
constexpr std::string_view durable_unconfirmed_tail
    = "the next ref-log recovery installs it, so it may still take effect, and retrying the earlier write "
      "may duplicate it; this write was not attempted";

/// The relink confirm's logger, resolved once. `LOG_IMPL` evaluates its logger argument BEFORE testing
/// the level, so `getLogger` at the call site would take the global logger-registry lock on every
/// refusal even with tracing off -- and `confirmExactRef` refuses while holding pool-wide append
/// admission, on a path a remote peer drives.
const LoggerPtr & confirmLogger()
{
    static const LoggerPtr logger = getLogger("CasRefLedger");
    return logger;
}

/// Classifies whether an exception thrown out of a ref-table recovery attempt (checkpoint/snapshot/log
/// reads, or the seal write) is a TRANSIENT object-store transport failure worth retrying, vs. a
/// terminal condition (corruption, decode failure, logic error, resource limit) that must fail fast.
/// A read that exhausts its policy surfaces as `NETWORK_ERROR`, but a store's own transport failure
/// can also reach here unclassified -- `S3_ERROR` for the S3 backend, or a socket/timeout/Poco code --
/// so the set covers both rather than only the one the engine re-mints.
bool isTransientRecoveryError(int code)
{
    return code == ErrorCodes::NETWORK_ERROR
        || code == ErrorCodes::S3_ERROR
        || code == ErrorCodes::POCO_EXCEPTION
        || code == ErrorCodes::SOCKET_TIMEOUT
        || code == ErrorCodes::CANNOT_READ_FROM_SOCKET
        || code == ErrorCodes::TIMEOUT_EXCEEDED;
}

String recoveredLifeRemovedMessage(const RootNamespace & ns, const NamespaceLifeId & life)
{
    return fmt::format(
        "CAS namespace '{}': the catalog no longer names life {} after its ref recovery walk; "
        "retry from a fresh catalog observation", ns.string(), renderIncarnation(life.incarnation));
}

/// What sits at a ref-log key that our own conditional create just lost to.
enum class Occupant : uint8_t
{
    NotOccupied,     /// the create won, or the outcome is unresolved -- nothing was read
    Ours,            /// byte-for-byte the transaction this attempt intended: an earlier attempt landed
    Tombstone,       /// a tombstone (`EpochSeal`) of this namespace, written by a successor
    Foreign,         /// none of the above -- impossible under mount-lease exclusivity
};

/// A tombstone at `id` that names `next_epoch`. Its backlink is `open->seal_id` iff `id` is at sequence 1
/// and `open` is set: the same rule `nextRefSlot` gives an ordinary record at that slot.
RefLogTxn makeEpochTombstoneTxn(const RootNamespace & ns, const RefTxnId & id, uint64_t next_epoch,
                                const std::optional<RefEpochTransition> & open)
{
    RefLogTxn tombstone;
    tombstone.ns = ns.string();
    tombstone.txn_id = id;
    RefOp op;
    op.kind = RefOpKind::EpochSeal;
    tombstone.ops.push_back(op);
    if (open && id.ref_sequence == 1)
        tombstone.prev_epoch_seal = open->seal_id;
    tombstone.next_epoch = next_epoch;
    return tombstone;
}

/// The `mine | successor's seal | foreign` adjudication both write sites owe (the primitive that read
/// the occupant deliberately does none of it). "Mine" means BYTE EQUALITY -- never a shape or
/// generation match, which is the aliasing the phase-0 model rejected.
///
/// The `catch` is narrow ON PURPOSE, and it is the whole reason this is a function rather than three
/// lines inline. A blanket `catch (...)` here would launder a TRANSIENT failure -- an allocation
/// failure or a memory-limit hit while decoding -- into a `Foreign` verdict, and `Foreign` fences the
/// mount closed and raises a foreign-interference alarm. A perfectly ordinary successor handover plus a
/// memory blip would then read as corruption. Only the two codes that actually mean "these bytes are
/// not a well-formed ref-log transaction of this namespace at this id" are absorbed: the decode layer
/// normalises every malformed-input class to `CORRUPTED_DATA` and passes `UNKNOWN_FORMAT_VERSION`
/// through (an object this build cannot read is still not a seal it can consume). Everything else
/// propagates, leaving the caller's lane exactly as it was.
struct ClassifiedOccupant
{
    Occupant kind;
    /// Engaged iff `kind` is `Tombstone`: the epoch the tombstone names.
    std::optional<RefEpochTransition> transition;
    /// False iff this build could not decode the occupant (`CORRUPTED_DATA` or `UNKNOWN_FORMAT_VERSION`).
    bool decoded = true;
};

ClassifiedOccupant classifyRefLogOccupant(const RootNamespace & ns, const RefTxnId & id, const String & occupant_bytes,
                                          const String & expected_bytes)
{
    if (occupant_bytes == expected_bytes)
        return {Occupant::Ours, std::nullopt, true};
    try
    {
        const RefLogTxn occupant = decodeRefLogTxn(openObject(FormatId::RefLog, occupant_bytes), ns.string(), id);
        std::optional<RefEpochTransition> transition = refEpochTransitionOf(occupant);
        if (transition)
            return {Occupant::Tombstone, transition, true};
        return {Occupant::Foreign, std::nullopt, true};
    }
    catch (const Exception & e)
    {
        if (e.code() != ErrorCodes::CORRUPTED_DATA && e.code() != ErrorCodes::UNKNOWN_FORMAT_VERSION)
            throw;
        return {Occupant::Foreign, std::nullopt, false};
    }
}

/// Reads like `InlineKeyReader` and keeps the bytes it read at one key: the retained-attempt check
/// compares stored bytes, and a chain visitor receives only the decoded record.
class CapturingKeyReader final : public KeyReader
{
public:
    CapturingKeyReader(CasOperation & op_, std::optional<String> watched_key_)
        : op(op_), watched_key(std::move(watched_key_))
    {
    }

    void hint(const String &) override {}
    std::optional<Object> take(const String & key) override
    {
        std::optional<Object> got = op.read(key, Retry::standard());
        if (watched_key && key == *watched_key)
            captured = got ? std::optional<String>(got->bytes) : std::nullopt;
        return got;
    }
    void discard(const String &) override {}
    size_t pending() const override { return 0; }
    size_t window() const override { return 0; }

    /// The bytes of the last read of the watched key; `nullopt` when it was absent or never read.
    const std::optional<String> & capturedBytes() const { return captured; }

private:
    CasOperation & op;
    const std::optional<String> watched_key;
    std::optional<String> captured;
};

/// One line when a lane enters a state that stops its appends: the counters behind it are server-wide and
/// name no table.
void logLaneStopped(const NamespaceLifeId & life, RefLaneState state, uint64_t writer_epoch, std::string_view key,
                    std::string_view cause) noexcept
{
    try
    {
        LOG_WARNING(getLogger("CasPool"),
            "CAS ref table '{}' (life {}): the append lane of writer epoch {} is {} at '{}': {}",
            life.ns.string(), renderIncarnation(life.incarnation), writer_epoch, refLaneStateName(state), key, cause);
    }
    catch (...)   // NOLINT(bugprone-empty-catch)
    {
        /// The state transition is already made; logging must not replace the caller's error.
    }
}
}

CasRefLedger::CasRefLedger(
    CasRequests & mount_requests_,
    const Layout & layout_,
    RefLedgerConfig config_,
    const CasEventSink & event_sink_,
    CasRequestBudget cas_request_budget_,
    String server_root_id_,
    std::function<uint64_t()> live_epoch_fn_,
    std::function<LiveEpochPair()> live_epoch_pair_fn_,
    std::function<bool()> fence_ok_fn_,
    std::function<uint64_t()> fence_generation_fn_,
    std::function<uint64_t()> boot_ms_now_fn_,
    std::function<bool()> may_mutate_,
    std::function<void(const String &, const String &, const std::optional<String> &)> on_impossible_interference_,
    std::function<bool(std::function<void(DetachedStopToken)>)> dispatch_detached_,
    std::function<void()> publish_error_hook_,
    std::function<void(const RootNamespace &)> cancel_inflight_builds_,
    std::function<void()> recovery_pre_first_request_hook_for_test_)
    : mount_requests(mount_requests_)
    , layout(layout_)
    , config(std::move(config_))
    , event_sink(event_sink_)
    , cas_request_budget(cas_request_budget_)
    , server_root_id(std::move(server_root_id_))
    , live_epoch_fn(std::move(live_epoch_fn_))
    , live_epoch_pair_fn(std::move(live_epoch_pair_fn_))
    , fence_ok_fn(std::move(fence_ok_fn_))
    , fence_generation_fn(std::move(fence_generation_fn_))
    , boot_ms_now_fn(std::move(boot_ms_now_fn_))
    , may_mutate(std::move(may_mutate_))
    , on_impossible_interference(std::move(on_impossible_interference_))
    , dispatch_detached(std::move(dispatch_detached_))
    , publish_error_hook(std::move(publish_error_hook_))
    , cancel_inflight_builds(std::move(cancel_inflight_builds_))
    , recovery_pre_first_request_hook_for_test(std::move(recovery_pre_first_request_hook_for_test_))
{
    /// Default backoff sleep for the recovery retry loop (`ensureRefTableRecovered`): sleep in short
    /// slices and stop early if the mount fence drops (shutdown / lease loss), so teardown never waits
    /// out a full 30s backoff. This is deliberate, bounded backoff against external object-store I/O
    /// failure -- NOT masking a race -- exactly like the request engine's own inter-attempt sleep; the
    /// slice loop additionally makes it interruptible, which that one is not.
    recovery_retry_sleep_fn = [this](uint64_t total_ms, const std::optional<DetachedStopToken> & token)
    {
        constexpr uint64_t slice_ms = 200;
        uint64_t slept = 0;
        while (slept < total_ms && fence_ok_fn() && !(token && token->stopping()))
        {
            const uint64_t chunk = std::min(slice_ms, total_ms - slept);
            sleepForMilliseconds(chunk);
            slept += chunk;
        }
    };
}

WriteResult CasRefLedger::stagingPutIfAbsent(const String & key, const String & bytes)
{
    /// Admitted under the mount fence's CURRENT generation: a staged write belongs to the caller in
    /// front of it, not to a transaction admitted earlier, so there is nothing to resume under.
    CasOperation op = mount_requests.admit();
    return op.create(key, bytes, Retry::standard());
}

void CasRefLedger::refuseUnlessAdmitted(const CasOperation & op, std::string_view what) const
{
    if (op.admitted())
        return;
    throwNotAdmitted(what);
}

void CasRefLedger::throwNotAdmitted(std::string_view what) const
{
    throwCasTransientUnavailable(
        fmt::format("content-addressed pool '{}'", server_root_id),
        fmt::format("{}: the operation is no longer admitted -- the mount lease has too little time left "
                    "for another request (writes are paused and resume once the lease renewal is confirmed), "
                    "or this table's runtime was detached", what));
}

void CasRefLedger::setCasRetrySleepForTest(std::function<void(uint64_t)> sleep_fn)
{
    mount_requests.setSleepFnForTest(sleep_fn);
    recovery_retry_sleep_fn = [recovery_sleep_fn = std::move(sleep_fn)](
        uint64_t total_ms, const std::optional<DetachedStopToken> &)
    {
        recovery_sleep_fn(total_ms);
    };
}

void CasRefLedger::setRefRecoveryRetrySleepForTest(
    std::function<void(uint64_t, const std::optional<DetachedStopToken> &)> sleep_fn)
{
    recovery_retry_sleep_fn = std::move(sleep_fn);
}


std::optional<Resolved> CasRefLedger::resolveRef(const RootNamespace & ns, const String & ref_name, bool /*allow_stale*/,
                                                 ResolveAudit audit)
{
    /// The read side of the snapshot+log protocol has one authoritative cached table for this mounted
    /// writer. The `allow_stale` staleness-tolerance knob no longer selects anything: this mounted writer is the
    /// ONLY writer of `ns`'s ref state (no external CAS token to go stale against, unlike the old
    /// per-shard decode cache), so the recovered-and-cached `RefTableState` is always this process's
    /// authoritative view. Kept as a parameter so existing callers compile unchanged.
    const auto rt = acquireReadableRefTableRuntime(ns);
    /// A namespace the catalog does not name resolves nothing and is not born by being asked: a ref that
    /// could exist would have needed a write to put it there, and that write would have birthed the
    /// namespace first. The two maintenance calls below are skipped with it, and provably lose nothing --
    /// see the note in `listRefs`.
    if (!rt)
        return std::nullopt;
    if (ensureRefTableRecovered(ns, *rt) == RefRecoveryOutcome::LifeRemoved)
        return std::nullopt;
    /// A table this mount only ever READS (never mutates) would otherwise
    /// never have its just-replayed tail/precommits checked -- `appendRefOps`'s own hoisted checks only
    /// fire for a table this mount WRITES to. Both are cheap (lock + comparison) on the warm path (the
    /// flag/threshold is already false after the table's first touch this mount); the sweep, if it DOES
    /// fire, runs synchronously here (safe: this call is not nested inside any queue leader's stack).
    /// Insulated (unlike appendRefOps's own hoisted call): a READ must not fail because a piggybacked
    /// maintenance action hit an uncertain PUT -- see `sweepStalePrecommitsForRead`.
    sweepStalePrecommitsForRead(ns, rt);
    maybeScheduleSnapshotPublish(ns, rt);

    if (read_before_state_lock_hook_for_test)
        read_before_state_lock_hook_for_test();

    /// Capture the resolved edge under `state_mutex`, but emit AFTER releasing it: the audit
    /// sink may re-enter a ledger read that itself takes `state_mutex` (e.g. `resolveRef`), so emitting
    /// while holding the lock self-deadlocks that reentrant read on the same thread. The reentrancy-safe
    /// dispatcher additionally serializes delivery, but the same-thread relock is prevented here by the
    /// lock discipline, not the dispatcher.
    ManifestRef resolved_ref;
    uint64_t resolved_published_at_ms = 0;
    std::optional<CasEvent> pending_event;
    {
        std::lock_guard lock(rt->state_mutex);
        const auto it = rt->state.getCommitted().find(ref_name);
        if (it == rt->state.getCommitted().end())
            return std::nullopt;

        const RefCommittedRow & row = it->second;
        resolved_ref = row.manifest_ref;
        resolved_published_at_ms = row.published_at_ms;
        /// A resolved ref points to its manifest (the read-path entry point). `object_hash` is the manifest
        /// instance id the ref names; pairs with a later readManifest ReadMissing if that body is gone.
        /// `Deferred` (used only by `CachedPartFolderAccess::resolve` on the `getView` call path) skips this
        /// emit; the caller decides, once it knows whether the access as a whole did real resolve work,
        /// whether to emit the identical event itself — see `ResolveAudit`'s doc comment.
        if (audit == ResolveAudit::Emit && hasEventSink())
        {
            CasEvent _ev0;
            _ev0.type = CasEventType::RefResolve;
            _ev0.namespace_ = ns.string();
            _ev0.ref_name = ref_name;
            _ev0.object_kind = CasEventObjectKind::Manifest;
            _ev0.object_hash = manifestRefDebugString(row.manifest_ref);
            _ev0.outcome = "resolved";
            _ev0.reason = "read-side resolve of a ref to its part manifest";
            pending_event = std::move(_ev0);
        }
    }
    if (pending_event)
        emitEvent(std::move(*pending_event));
    return Resolved{
        .manifest_id = ManifestId{.root_namespace = ns, .ref = resolved_ref},
        .manifest_size = 0,
        .published_at_ms = resolved_published_at_ms,
    };
}

std::map<String, Resolved> CasRefLedger::listRefs(const RootNamespace & ns)
{
    /// The whole ref set is a map iteration over this namespace's recovered-and-cached `RefTableState`:
    /// an empty but existing namespace still pays one exact recovery pass and zero further requests;
    /// a warm namespace costs nothing at all (replacing the old per-shard LIST-then-HEAD-present-shards
    /// dance, since there is no longer a shard fan-out to rediscover on every call). A namespace that was
    /// never born costs one catalog GET and stops there.
    const auto rt = acquireReadableRefTableRuntime(ns);
    /// A namespace the catalog does not name has no refs to list, and listing them is the wrong event to
    /// bring one into existence on.
    ///
    /// Returning here also skips the two maintenance calls below, and that is not a lost obligation:
    /// neither would do anything. The stale-precommit sweep runs only when `needs_stale_precommit_sweep`
    /// is armed, which recovery and commit are the only things that arm; the snapshot publish is admitted
    /// only for a `Live` lifecycle over a non-empty tail, and an unrecovered runtime is neither.
    if (!rt)
        return {};
    if (ensureRefTableRecovered(ns, *rt) == RefRecoveryOutcome::LifeRemoved)
        return {};
    /// Apply the same read-side maintenance policy as `resolveRef`; see `sweepStalePrecommitsForRead`.
    sweepStalePrecommitsForRead(ns, rt);
    maybeScheduleSnapshotPublish(ns, rt);

    std::map<String, Resolved> result;
    std::lock_guard lock(rt->state_mutex);
    for (const auto [ref_name, row] : rt->state.getCommitted())
        result.emplace(ref_name, Resolved{
            .manifest_id = ManifestId{.root_namespace = ns, .ref = row.manifest_ref},
            .manifest_size = 0,
            .published_at_ms = row.published_at_ms,
        });
    return result;
}

bool CasRefLedger::hasAnyRefWithPrefix(const RootNamespace & ns, std::string_view prefix)
{
    /// Same non-minting recovery/maintenance preamble as `listRefs`; see there for what each shape of
    /// namespace -- never born, empty, warm -- costs.
    const auto rt = acquireReadableRefTableRuntime(ns);
    if (!rt)
        return false;
    if (ensureRefTableRecovered(ns, *rt) == RefRecoveryOutcome::LifeRemoved)
        return false;
    sweepStalePrecommitsForRead(ns, rt);
    maybeScheduleSnapshotPublish(ns, rt);

    std::lock_guard lock(rt->state_mutex);
    for (const auto [ref_name, row] : rt->state.getCommitted())
        if (prefix.empty() || std::string_view(ref_name).starts_with(prefix))
            return true;
    return false;
}


ConfirmAnswer CasRefLedger::confirmExactRef(const RootNamespace & ns, const String & ref_name,
                                           const ManifestRef & manifest_ref, ConfirmLockMode lock_mode) const
{
    const uint64_t started_ms = boot_ms_now_fn();
    while (true)
    {
        if (const auto answer = confirmExactRefIfStateLockFree(ns, ref_name, manifest_ref))
            return *answer;
        if (lock_mode == ConfirmLockMode::TryLock || boot_ms_now_fn() - started_ms >= cas_request_budget.request_window_ms)
        {
            ProfileEvents::increment(ProfileEvents::CASRelinkConfirmRefusedStateLockBusy);
            LOG_TRACE(confirmLogger(), "Relink confirm for ref '{}' in namespace '{}' is unknown: the table's state lock is held",
                      ref_name, ns.string());
            return ConfirmAnswer::Unknown;
        }
        if (confirm_before_state_lock_wait_hook_for_test)
            confirm_before_state_lock_wait_hook_for_test();
        /// Waits holding no other ledger lock, so append admission of every table goes on meanwhile.
        if (const auto rt = lookupRefTableRuntime(ns))
        {
            std::lock_guard<std::mutex> wait_for_holder(rt->state_mutex);
        }
    }
}

std::optional<ConfirmAnswer> CasRefLedger::confirmExactRefIfStateLockFree(const RootNamespace & ns, const String & ref_name,
                                                                         const ManifestRef & manifest_ref) const
{
    /// Gate 1 of the relink confirm. A `Yes` authorizes a remote relink receiver, or an in-process copy,
    /// to promote a manifest whose blobs are protected only by this writer's committed binding of that
    /// exact manifest, so a `Yes` is an assertion about the durable table, not about this cache. Every
    /// rule below exists to make that assertion true; the answer to anything a rule cannot establish is
    /// `Unknown`, which costs the caller a retry or a refused copy and costs correctness nothing.
    ///
    /// Two structural properties, both load-bearing:
    ///
    ///   ZERO object-store I/O. The relink sender runs this on an interserver request, so anything it
    ///   could be made to do is something a remote peer can make this writer do. It therefore reads only
    ///   what is already resident, never recovers, never resolves a wedge, and -- see the `find` below -- never even
    ///   materializes a runtime. Deliberately absent for the same reason: `ensureRefTableRecovered`,
    ///   `sweepStalePrecommitsForRead` and `maybeScheduleSnapshotPublish`, the three maintenance calls
    ///   `resolveRef` performs and all three of which can do I/O.
    ///
    ///   ONE snapshot across BOTH lane mutexes. `pending`/`carved` live under
    ///   `ref_queue_mutex`, the rows and the wedge under `state_mutex`, and the whole point of the
    ///   rules is their CONJUNCTION -- read at different instants they would prove nothing. The lock
    ///   ORDER is the one the rest of this file already establishes (`enforceRefTableCacheBudget`
    ///   nests `state_mutex` under `ref_queue_mutex`, and nothing anywhere takes them the other way
    ///   round). Because admission (`appendRefOps`' `pending.push_back`) happens under
    ///   `ref_queue_mutex`, an append is either entirely before this snapshot -- and then visible in
    ///   `pending` or, once carved, in `carved` -- or entirely after it. There is no interleaving in
    ///   which a mutation of the asked-about ref is admitted and this function still answers `Yes`; a
    ///   mutation of another ref may be admitted, and the answer is still right, because it cannot move
    ///   this ref's row.
    ///
    /// What a `Yes` does NOT prove, stated so nobody has to rediscover it: that this runtime's
    /// recovered view is a COMPLETE replay of the durable log. Completeness is recovery's contract, not
    /// this function's, and it cannot be re-established here without I/O. Rules 2-4 exclude every way
    /// this MOUNT can have fallen behind its own durable writes; a recovery that silently observed less
    /// than it should have is a different defect, in a different component.
    std::lock_guard<std::mutex> qlock(ref_queue_mutex);

    /// Rule 2 (residency). Direct slot lookup, never a catalog observation or exact-runtime acquisition:
    /// a read-only query must not let a peer grow this writer's cache or make the next reader pay for a
    /// recovery it invented. A cold or evicted table is simply unknown here.
    ///
    /// These two arms are the ONLY refusals in this function that are deliberately not counted: a table
    /// this mount has never touched, or has dropped under cache-budget pressure, is ordinary cache
    /// behaviour rather than the lane, mount or load condition each counter below separates. Counting
    /// it would put a number that moves with cache size next to numbers that describe this writer's
    /// health. There is also no runtime here to attribute the refusal to.
    const auto it = ref_name_slots.find(ns.string());
    if (it == ref_name_slots.end())
        return ConfirmAnswer::Unknown;
    if (!it->second.current)
        return ConfirmAnswer::Unknown;
    RefTableRuntime & rt = *it->second.current;

    /// Every refusal below is attributed here, on the node that computed it: `ConfirmAnswer` crosses
    /// two interfaces as a three-value enum and stays that way, so the counters and this trace line are
    /// the only way a live gate can tell load (`RefMutationInFlight`) from a fault (`LaneWedged`,
    /// `LaneBroken`), from contention (`StateLockBusy`), or from this mount losing its claim to the
    /// namespace (`MountCannotSpeak`). The invariant to keep when editing below: every
    /// `return ConfirmAnswer::Unknown` past this point goes through `refuse`, and the only uncounted
    /// refusals in this function are the two residency arms above, which say why.
    const auto refuse = [&](ProfileEvents::Event reason, std::string_view why)
    {
        ProfileEvents::increment(reason);
        LOG_TRACE(confirmLogger(), "Relink confirm for ref '{}' in namespace '{}' is unknown: {}",
                  ref_name, ns.string(), why);
        return ConfirmAnswer::Unknown;
    };

    /// Only a try-lock: this function holds `ref_queue_mutex`, which is pool-wide append admission, and waiting
    /// here would make every table's lane wait on this table's state lock.
    std::unique_lock<std::mutex> slock(rt.state_mutex, std::try_to_lock);
    if (!slock.owns_lock())
        return std::nullopt;

    /// Rule 2 (warm). An unrecovered or mid-recovery runtime has an EMPTY `state`, which would read as
    /// "the ref does not exist" -- knowledge it does not have. `superseded_by_remount` is the same
    /// class: this runtime was detached by a self-remount and its view belongs to a dead incarnation.
    /// Recovery publishes atomically (`installRecoveryResult` sets `recovered` LAST under this mutex),
    /// so there is no half-recovered view to catch in between.
    if (!rt.recovered || rt.recovery_in_progress
        || rt.catalog_life_invalidated.load(std::memory_order_acquire)
        || rt.superseded_by_remount.load(std::memory_order_acquire))
        return refuse(ProfileEvents::CASRelinkConfirmRefusedMountCannotSpeak,
                      "the table is unrecovered, recovering, retired or superseded by a remount");

    /// Rule 3 (no admitted mutation of THIS ref). The hazard is a committed row that lags a transaction
    /// of the asked-about ref: the leader does not hold `state_mutex` across the `PUT`, so between
    /// "durable" and "installed" that ref's row is stale, and a `Yes` read off it would authorize a
    /// receiver to promote over a blob the transaction may already have retired. A mutation of ANOTHER
    /// ref cannot change this ref's binding or the blobs its manifest protects, so its row is exactly as
    /// authoritative as on an idle lane; refusing for it is what starved two replicas of each other on a
    /// slow control plane. Every admitted mutation names its scope (`MutationScope`, recorded at
    /// admission under `ref_queue_mutex` and validated against its ops at flush), and it is visible in
    /// `pending` from admission to carve and in `carved` from carve to the tenure's exit guard, so "a
    /// change of this ref is queued or in flight" is read from those two. The lane states other than
    /// `Ready`/`Writing` refuse table-wide: `Wedged` holds a transaction that may be durable, and once
    /// its tenure exits nothing but the attempt and the lane state records WHICH ref it touched -- the
    /// exit guard clears the carved mirror, and the chunk's items were completed with an error before
    /// that -- and `NeedsRecovery`, `Closed`, `Faulted` are fences on the whole view.
    /// `Writing` with nothing carved cannot happen; it fails closed.
    if (rt.lane_state == RefLaneState::Wedged)
        return refuse(ProfileEvents::CASRelinkConfirmRefusedLaneWedged, "the lane holds an unresolved append");
    if (rt.lane_state != RefLaneState::Ready && rt.lane_state != RefLaneState::Writing)
        return refuse(ProfileEvents::CASRelinkConfirmRefusedLaneBroken, "the lane is neither Ready nor Writing");
    if (rt.lane_state == RefLaneState::Writing && rt.carved.empty())
        return refuse(ProfileEvents::CASRelinkConfirmRefusedLaneBroken, "the lane is Writing with nothing carved");
    const auto covers = [&](const MutationScope & scope)
    {
        return scope.kind == MutationScope::Kind::WholeShard || scope.names(ref_name);
    };
    for (const auto & item : rt.pending)
        if (covers(item->scope))
            return refuse(ProfileEvents::CASRelinkConfirmRefusedRefMutationInFlight, "a queued mutation names this ref");
    for (const auto & item : rt.carved)
        if (covers(item->scope))
            return refuse(ProfileEvents::CASRelinkConfirmRefusedRefMutationInFlight, "a carved mutation names this ref");

    /// Rule 5 (exact row equality) -- the only rule that can answer `No` at all. On a table that passed
    /// rules 2-4 the committed map is this writer's view, so a missing row or a different `ManifestRef`
    /// is a real disagreement rather than an ambiguity about this cache. It is NOT a proof about the
    /// DURABLE table: the fence has not been checked yet (rule 6, below, states why that order is
    /// deliberate and why it is sound). Equality is exact and total:
    /// mint-tightening (spec §A3) guarantees a repoint or a recreation mints a fresh `ManifestRef`, so
    /// there is no ABA to defend against here.
    const auto & committed = rt.state.getCommitted();
    const auto row = committed.find(ref_name);
    if (row == committed.end() || !(row->second.manifest_ref == manifest_ref))
        return ConfirmAnswer::No;

    /// Rule 6 (mount fence), LAST and still under both locks -- the order the spec fixes. Everything
    /// above describes what this process believes; this is the check that it is still entitled to
    /// believe it: a fenced-out mount is no longer the namespace's single writer, so another writer may
    /// already have repointed the ref. Being last means a token that does not match is reported as `No`
    /// even under a lost fence: `No` and `Unknown` are the same outcome for the caller (both are
    /// `SourceProofFailed`, spec §failure-taxonomy), and only `Yes` is gated on the fence.
    /// Both monotone runtime-invalidations are folded in exactly as the mutation gates fold them: a
    /// retired or remount-superseded runtime can never authorize a remote promotion.
    if (!fence_ok_fn()
        || rt.catalog_life_invalidated.load(std::memory_order_acquire)
        || rt.superseded_by_remount.load(std::memory_order_acquire))
        return refuse(ProfileEvents::CASRelinkConfirmRefusedMountCannotSpeak,
                      "this mount no longer holds the namespace's write fence");

    return ConfirmAnswer::Yes;
}

std::shared_ptr<CasRefLedger::RefTableRuntime> CasRefLedger::lookupRefTableRuntime(const RootNamespace & ns) const
{
    std::lock_guard lock(ref_queue_mutex);
    const auto it = ref_name_slots.find(ns.string());
    return it == ref_name_slots.end() ? nullptr : it->second.current;
}

CasRefLedger::ColdAdmission::ColdAdmission(
    CasRefLedger & ledger_, String name_, uint64_t retirements_seen_, uint64_t epoch_) noexcept
    : ledger(&ledger_)
    , name(std::move(name_))
    , retirements_seen(retirements_seen_)
    , epoch(epoch_)
{
}

CasRefLedger::ColdAdmission::ColdAdmission(ColdAdmission && other) noexcept
    : ledger(std::exchange(other.ledger, nullptr))
    , name(std::move(other.name))
    , retirements_seen(other.retirements_seen)
    , epoch(other.epoch)
{
}

CasRefLedger::ColdAdmission::~ColdAdmission()
{
    if (!ledger)
        return;
    std::lock_guard lock(ledger->ref_queue_mutex);
    ledger->releaseColdAdmissionLocked(*this);
}

CasRefLedger::ColdAdmission CasRefLedger::beginColdAdmission(const RootNamespace & ns)
{
    String name = ns.string();
    std::lock_guard lock(ref_queue_mutex);
    RefNameSlot & slot = ref_name_slots[name];
    ++slot.admissions_in_flight;
    return ColdAdmission(*this, std::move(name), slot.retirements, ref_name_slots_epoch);
}

void CasRefLedger::releaseColdAdmissionLocked(ColdAdmission & admission)
{
    admission.ledger = nullptr;
    /// A remount cleared the slot this admission counted in; a slot of the same name now belongs to
    /// admissions captured after the remount.
    if (admission.epoch != ref_name_slots_epoch)
        return;
    const auto it = ref_name_slots.find(admission.name);
    chassert(it != ref_name_slots.end() && it->second.admissions_in_flight > 0);
    if (it == ref_name_slots.end())
        return;
    --it->second.admissions_in_flight;
    eraseNameSlotIfUnusedLocked(it);
}

void CasRefLedger::eraseNameSlotIfUnusedLocked(std::map<String, RefNameSlot>::iterator it)
{
    if (!it->second.current && it->second.admissions_in_flight == 0)
        ref_name_slots.erase(it);
}

bool CasRefLedger::latchRemovingLocked(RefTableRuntime & rt)
{
    const bool was_open = !rt.removal_admission_closed;
    rt.removal_admission_closed = true;
    rt.removing_observed = true;
    /// The row already reads `Removing`, so no removal write of this runtime can still change it.
    rt.removal_write_may_land = false;
    return was_open;
}

void CasRefLedger::logRemovingLatched(const NamespaceLifeId & life)
{
    LOG_WARNING(getLogger("CasRefLedger"),
        "CAS namespace '{}' life {}: its catalog row is Removing, so this server closes it: writes are refused "
        "and reads see no data", life.ns.string(), renderIncarnation(life.incarnation));
}

std::shared_ptr<CasRefLedger::RefTableRuntime> CasRefLedger::acquireRefTableRuntime(
    const NamespaceLifeId & life, bool observed_removing, uint64_t admitted_generation,
    ColdAdmission & admission, RuntimeAdmission kind)
{
    chassert(admission.ledger == this && admission.name == life.ns.string());
    refuseUnlessAdmitted(mount_requests.resume(admitted_generation), "ref-table runtime install");

    std::shared_ptr<RefTableRuntime> result;
    bool generation_moved = false;
    bool slots_cleared_by_remount = false;
    bool retired_since_observation = false;
    bool identity_conflict = false;
    bool latched_now = false;
    {
        std::lock_guard lock(ref_queue_mutex);
        generation_moved = fence_generation_fn() != admitted_generation;
        slots_cleared_by_remount = admission.epoch != ref_name_slots_epoch;
        if (!generation_moved && !slots_cleared_by_remount)
        {
            /// The admission keeps its slot alive, so a missing one is a bug; as in
            /// `releaseColdAdmissionLocked`, a release build takes the safe exit and publishes nothing.
            const auto it = ref_name_slots.find(life.ns.string());
            chassert(it != ref_name_slots.end());
            /// The catalog row this life came from may have been removed and drained after the caller read
            /// it; inserting then would publish a runtime for a life the janitor sweeps.
            retired_since_observation = it == ref_name_slots.end() || it->second.retirements != admission.retirements_seen;
            if (!retired_since_observation)
            {
                RefNameSlot & slot = it->second;
                if (slot.current)
                {
                    const auto & current = slot.current;
                    if (current->life == life
                        && current->admitted_fence_generation == admitted_generation
                        && !current->catalog_life_invalidated.load(std::memory_order_acquire)
                        && !current->superseded_by_remount.load(std::memory_order_acquire))
                        result = current;
                    else
                        identity_conflict = true;
                }
                else
                {
                    result = std::make_shared<RefTableRuntime>(
                        next_ref_runtime_id.fetch_add(1, std::memory_order_relaxed) + 1,
                        life,
                        admitted_generation);
                    slot.current = result;
                }
            }
        }
        /// A resident runtime too: it may have been admitted as `Live` before the row turned.
        if (result && observed_removing)
            latched_now = latchRemovingLocked(*result);
        releaseColdAdmissionLocked(admission);
    }
    if (latched_now)
        logRemovingLatched(life);

    if (generation_moved)
        refuseUnlessAdmitted(mount_requests.resume(admitted_generation), "ref-table runtime install");
    if (slots_cleared_by_remount)
        throwNotAdmitted("ref-table runtime install");
    /// Same retryable class on both planes; a read must not claim a write was attempted.
    const auto refuseStaleObservation = [&](const String & why)
    {
        if (kind != RuntimeAdmission::Write)
            throwCasTransientUnavailable(fmt::format("CAS namespace '{}'", life.ns.string()), why);
        throwCasWriteRetryLater(fmt::format("CAS namespace '{}': {}", life.ns.string(), why));
    };
    if (retired_since_observation && kind == RuntimeAdmission::PresenceProbe)
        return nullptr;
    if (retired_since_observation)
        refuseStaleObservation(fmt::format(
            "a life of this name was retired while life {} was being admitted; "
            "retry from a fresh catalog observation",
            renderIncarnation(life.incarnation)));
    if (identity_conflict)
        refuseStaleObservation(fmt::format(
            "the cached runtime identity changed while publishing catalog life {}; "
            "retry from a fresh catalog observation",
            renderIncarnation(life.incarnation)));
    return result;
}

std::shared_ptr<CasRefLedger::RefTableRuntime> CasRefLedger::acquireReadableRefTableRuntime(
    const RootNamespace & ns)
{
    /// A resident runtime is the process's already-held immutable life handle. Hot readers deliberately
    /// pay no catalog request here: after rebirth the handle may return predecessor bytes or NotFound,
    /// but its exact physical id can never alias successor bytes. A genuinely fresh logical-name
    /// admission is the cold path below and resolves the current catalog life before publishing a
    /// runtime.
    if (auto current = lookupRefTableRuntime(ns))
    {
        refuseUnlessAdmitted(mount_requests.resume(current->admitted_fence_generation), "resident readable runtime");
        {
            std::lock_guard queue_lock(ref_queue_mutex);
            if (current->removal_admission_closed)
                return nullptr;
        }
        if (current->catalog_life_invalidated.load(std::memory_order_acquire)
            || current->superseded_by_remount.load(std::memory_order_acquire))
            throwCasTransientUnavailable(
                fmt::format("CAS namespace '{}'", ns.string()),
                "its cached life was detached; retry against a fresh observation");
        return current;
    }

    const uint64_t admitted_generation = fence_generation_fn();
    ColdAdmission admission = beginColdAdmission(ns);
    CasOperation op = mount_requests.resume(admitted_generation);
    const CasRefCatalog::Snapshot first_catalog = CasRefCatalog::read(op, layout);
    refuseUnlessAdmitted(op, "cold readable runtime admission");
    first_catalog.life_index.throwIfAmbiguous("CAS cold readable runtime admission");
    const auto it = std::find_if(first_catalog.catalog.entries.begin(), first_catalog.catalog.entries.end(),
        [&](const CatalogEntry & entry) { return entry.ns == ns; });
    if (it == first_catalog.catalog.entries.end() || it->state != NsState::Live)
        return nullptr;
    const NamespaceLifeId life = NamespaceLifeId::fromCatalogEntry(it->ns, it->incarnation);

    if (readable_catalog_after_observation_hook_for_test)
        readable_catalog_after_observation_hook_for_test();

    /// Revalidate THIS namespace's row by value before the life derived from the first cut becomes the
    /// first resident runtime. Comparing the whole catalog (token + content) here starves under
    /// unrelated-namespace churn -- every cold admission would retry whenever ANY other namespace
    /// mutates -- while our row's identity is what the published runtime actually depends on. A row
    /// that changed state or incarnation, or vanished, still forces a fresh observation; transitions
    /// after this read are caught by `invalidateRemovedCatalogLife` exactly as before. This second GET
    /// is deliberately immediately before the queue-locked fence/slot recheck in
    /// `acquireRefTableRuntime`; the held-handle warm path above pays none.
    const CasRefCatalog::Snapshot second_catalog = CasRefCatalog::read(op, layout);
    refuseUnlessAdmitted(op, "cold readable runtime admission");
    /// The first read's ambiguity validation does not cover an aliasing incarnation admitted BETWEEN
    /// the reads; physical life-owned keys use only the incarnation, so an ambiguous second cut must
    /// refuse admission even when this namespace's own row is untouched.
    second_catalog.life_index.throwIfAmbiguous("CAS cold readable runtime admission");
    const auto second_it = std::find_if(second_catalog.catalog.entries.begin(), second_catalog.catalog.entries.end(),
        [&](const CatalogEntry & entry) { return entry.ns == ns; });
    const bool row_changed = second_it == second_catalog.catalog.entries.end() || !(*second_it == *it);
    if (row_changed)
        throwCasTransientUnavailable(
            fmt::format("CAS namespace '{}'", ns.string()),
            fmt::format("its catalog row changed while a cold reader observed life {}; "
                        "retry from a fresh catalog observation", renderIncarnation(life.incarnation)));

    return acquireRefTableRuntime(life, /*observed_removing=*/false, admitted_generation, admission, RuntimeAdmission::Read);
}

std::shared_ptr<CasRefLedger::RefTableRuntime> CasRefLedger::acquireMutableRefTableRuntime(
    const RootNamespace & ns)
{
    /// Captured before `namespaceLife`, whose answer may come from a runtime retired right after.
    const uint64_t admitted_generation = fence_generation_fn();
    ColdAdmission admission = beginColdAdmission(ns);
    const NamespaceLifeId life = namespaceLife(ns);
    if (const auto current = lookupRefTableRuntime(ns))
    {
        if (current->life == life)
            return current;
    }
    return acquireRefTableRuntime(life, /*observed_removing=*/false, admitted_generation, admission, RuntimeAdmission::Write);
}

void CasRefLedger::invalidateRemovedCatalogLife(const NamespaceLifeId & life)
{
    std::shared_ptr<RefTableRuntime> rt;
    {
        std::lock_guard lock(ref_queue_mutex);
        /// No slot: no admission of the name is in flight. After an observation of the removal a later
        /// admission reads the catalog after it. Before a GC erase the row is still `Removing`, so a later
        /// presence probe or drop may admit it; the drain calls this again once the erase resolved.
        const auto it = ref_name_slots.find(life.ns.string());
        if (it == ref_name_slots.end())
            return;
        /// Recorded whatever is resident, a successor included: a cold admission of `life` may be
        /// between its catalog read and its insertion, and the successor may be evicted before it.
        ++it->second.retirements;
        rt = it->second.current;
        if (!rt)
            return;
    }

    /// Invalidation must neither materialize a cache entry nor perform catalog I/O on GC's thread.
    /// Re-check the exact resident life while holding its state lock: a delayed reconciliation for a
    /// predecessor must not invalidate a successor. Publishing this bit makes existing holders inert;
    /// the slot is then detached and a later name-based caller may publish a distinct runtime.
    {
        std::lock_guard state_lock(rt->state_mutex);
        if (rt->life != life)
            return;
        rt->catalog_life_invalidated.store(true, std::memory_order_release);
    }
    {
        /// Detach by POINTER identity, not by logical name. A concurrent fresh lookup may already
        /// have installed a successor for the same name; a delayed predecessor invalidation must never
        /// erase that successor's cache slot.
        std::lock_guard queue_lock(ref_queue_mutex);
        const auto it = ref_name_slots.find(life.ns.string());
        if (it != ref_name_slots.end() && it->second.current == rt)
        {
            it->second.current.reset();
            eraseNameSlotIfUnusedLocked(it);
        }
    }
    rt->cv.notify_all();
    rt->recovery_cv.notify_all();
    rt->publish_settle_cv.notify_all();
}

void CasRefLedger::reconcileCatalogCut(const CasRefCatalog::Snapshot & catalog_cut)
{
    catalog_cut.life_index.throwIfAmbiguous("CAS resident ref-runtime reconciliation");

    /// `Live -> Removing` is one-way, so a `Removing` row read in an older cut is still `Removing`; the
    /// exact life keeps a successor of the same name open.
    std::vector<NamespaceLifeId> removing_lives;
    for (const CatalogEntry & entry : catalog_cut.catalog.entries)
        if (entry.state == NsState::Removing)
            removing_lives.push_back(NamespaceLifeId::fromCatalogEntry(entry.ns, entry.incarnation));

    std::vector<std::shared_ptr<RefTableRuntime>> closed_runtimes;
    std::vector<NamespaceLifeId> latched_now;
    {
        std::lock_guard queue_lock(ref_queue_mutex);
        for (const auto & [_, slot] : ref_name_slots)
        {
            if (!slot.current)
                continue;
            RefTableRuntime & rt = *slot.current;
            if (std::find(removing_lives.begin(), removing_lives.end(), rt.life) != removing_lives.end()
                && latchRemovingLocked(rt))
                latched_now.push_back(rt.life);
            if (rt.removal_admission_closed)
                closed_runtimes.push_back(slot.current);
        }
    }
    for (const NamespaceLifeId & life : latched_now)
        logRemovingLatched(life);

    for (const auto & rt : closed_runtimes)
    {
        const NamespaceLifeId & resident_life = rt->life;
        const std::optional<NamespaceLifeId> catalog_life
            = catalog_cut.life_index.resolve(resident_life.incarnation);
        if (!catalog_life || *catalog_life != resident_life)
            invalidateRemovedCatalogLife(resident_life);
    }
}

void CasRefLedger::checkRecoveryStillAdmitted(const RootNamespace & ns, RefTableRuntime & rt,
                                              bool & cancelled,
                                              const std::optional<DetachedStopToken> & token) const
{
    /// Polled at EVERY I/O boundary of the walk, because every one of them is a point at which this
    /// recovery may already have lost the right to continue -- and the walk WRITES, so "continue" is not
    /// a read-only proposition.
    ///
    /// Detached teardown is terminal for this publisher attempt. It is independent of the runtime's
    /// remount-cancellation latch and uses `ABORTED` so the retry envelope cannot mistake it for a
    /// transient object-store failure.
    if (token && token->stopping())
        throw Exception(ErrorCodes::ABORTED,
            "CAS ref recovery: teardown began while recovery was in progress");

    /// Runtime cancellation is checked next and reported as its own outcome: a self-remount asking
    /// recovery to stop is an orderly hand-off, not a failure of the store, and the caller must not
    /// re-drive it through the transient-retry loop the way it would an S3 blip.
    if (rt.recovery_cancel_requested.load(std::memory_order_acquire))
    {
        cancelled = true;
        ProfileEvents::increment(ProfileEvents::CASRefRecoveryCancelled);
        throwCasWriteRetryLater(fmt::format(
            "CAS ref-table recovery for namespace '{}' was cancelled by a self-remount before the mount "
            "fence was re-armed; the last attempt's fate is unresolved and nothing is installed — the next "
            "touch recovers under the fresh incarnation", ns.string()));
    }

    if (rt.catalog_life_invalidated.load(std::memory_order_acquire))
        throwCasWriteRetryLater(fmt::format(
            "CAS ref-table recovery for namespace '{}': catalog retirement invalidated life {} "
            "while recovery I/O was in flight — nothing further is written and nothing is installed",
            ns.string(), renderIncarnation(rt.life.incarnation)));

    /// The remount's OTHER publication, ordered before the fence re-arm: this runtime is detached, so
    /// whatever it recovers belongs to a dead incarnation's cache. Checked separately from the
    /// cancellation because the two are independent facts, exactly as `resolveWedgeOnce` checks them.
    if (rt.superseded_by_remount.load(std::memory_order_acquire))
        throwCasWriteRetryLater(fmt::format(
            "CAS ref-table recovery for namespace '{}': this cached table was superseded by a self-remount "
            "mid-recovery — retry against the fresh mount incarnation", ns.string()));

    /// The FENCE is deliberately NOT checked here: the walk's own `CasOperation` carries the admitted
    /// generation and refuses every request under a generation the fence has moved past, so a second
    /// check would only report the same fact from a different sample.
}

std::optional<RecoveryResult> CasRefLedger::runRecoveryWalkOnce(
    const RootNamespace & ns, RefTableRuntime & rt, uint64_t admitted_generation, const LiveEpochPair & live,
    const std::optional<RefAppendAttempt> & retained_attempt, std::optional<String> & hole_detail,
    bool & cancelled, const std::optional<DetachedStopToken> & token)
{
    /// One attempt. Runs with NO lock held: everything below is either read-only I/O or a conditional
    /// write at a key this namespace owns, and the replayed candidate is PRIVATE until the caller
    /// installs it. `recovery_in_progress` (not `state_mutex`) is what keeps a second caller for this
    /// same table from racing an independent walk -- see its doc comment.
    ///
    /// Runtime construction already fixed the exact catalog life, so every key this walk builds remains
    /// under the predecessor even if the same logical name is concurrently rebound.
    const NamespaceLifeId life = rt.life;

    /// ONE operation for the whole walk, resumed under the generation this recovery was admitted at:
    /// its reads, its stamp and its tombstone creates are all measured against that admission, and a
    /// result returning after a fence bump can install nothing.
    ///
    /// The liveness carries EVERY term `checkRecoveryStillAdmitted` polls except the generation, which
    /// is the fence's. The predicate is a bool where the poll throws, so the poll still runs at the
    /// boundaries: it turns each of these facts into the right exception and LATCHES a cancellation for
    /// the caller's retry classification.
    CasOperation op = mount_requests.resume(admitted_generation, [&rt, &token]
    {
        return !(token && token->stopping())
            && !rt.recovery_cancel_requested.load(std::memory_order_acquire)
            && !rt.catalog_life_invalidated.load(std::memory_order_acquire)
            && !rt.superseded_by_remount.load(std::memory_order_acquire);
    });

    /// ---- The checkpoint and the base ----
    /// The runtime was admitted for this exact life before entering recovery, so this walk takes no
    /// catalog cut of its own; the caller confirms the life after the walk.
    const CatalogEntry catalog_entry{
        .ns = life.ns,
        .state = NsState::Live,
        .incarnation = life.incarnation};
    if (recovery_pre_first_request_hook_for_test)
        recovery_pre_first_request_hook_for_test();
    checkRecoveryStillAdmitted(ns, rt, cancelled, token);
    const std::optional<CkptSample> sampled_ckpt = readCkpt(op, layout, life);
    checkRecoveryStillAdmitted(ns, rt, cancelled, token);

    /// `chooseRecoveryGrounding` is the single policy boundary: it requires a readable `_ckpt` with
    /// `life_epoch` for a recoverable life, and names the base and the first key of the walk.
    const RecoveryGrounding grounding = chooseRecoveryGrounding(
        catalog_entry,
        sampled_ckpt ? std::optional<RefCkpt>{sampled_ckpt->ckpt} : std::nullopt);
    const uint64_t life_epoch = *sampled_ckpt->ckpt.life_epoch;
    /// The `_ckpt` this recovery rests on: the sample, or the stamp that replaced it.
    CkptSample accepted = *sampled_ckpt;

    std::optional<RefTableSnapshot> base_snapshot;
    uint64_t base_snapshot_bytes = 0;
    if (grounding.base)
    {
        try
        {
            checkRecoveryStillAdmitted(ns, rt, cancelled, token);
            CheckpointSnapshotBase base = readCheckpointSnapshotBase(op, layout, life, sampled_ckpt->ckpt);
            base_snapshot = std::move(base.snapshot);
            base_snapshot_bytes = base.bytes;
        }
        catch (const Exception & e)
        {
            if (!failureJudgedAgainstCkpt(e.code()))
                throw;

            /// A newer checkpoint may have re-anchored recovery while cleanup retired this old anchor's
            /// snapshot or witness log. Restart from that newer sample; an unchanged checkpoint turns
            /// every helper failure (missing, malformed, or seal) into the corruption it describes.
            checkRecoveryStillAdmitted(ns, rt, cancelled, token);
            const std::optional<CkptSample> current = readCkpt(op, layout, life);
            if (classifyMissingSampledBase(sampled_ckpt->etag,
                                           current ? std::optional<Etag>(current->etag) : std::nullopt)
                == MissingBaseVerdict::RestartRecovery)
                return std::nullopt;
            throw;
        }
    }
    checkRecoveryStillAdmitted(ns, rt, cancelled, token);

    RefReplayBuilder builder(std::move(base_snapshot), base_snapshot_bytes);

    /// Applies one decoded transaction to the private candidate, accounting its resident footprint to
    /// the streaming-recovery memory probe for exactly the span it is held (no-op in production).
    const auto apply_one = [&](RefLogTxn && txn, uint64_t encoded_bytes)
    {
        const int64_t footprint = static_cast<int64_t>(decodedRefLogTxnFootprint(txn));
        reportReplayMemoryDelta(footprint);
        SCOPE_EXIT({ reportReplayMemoryDelta(-footprint); });
        builder.applyOne(std::move(txn), encoded_bytes);
    };

    CapturingKeyReader reader(op, retained_attempt
        ? std::optional<String>(layout.refLogKey(life, retained_attempt->txn_id)) : std::nullopt);

    const auto adjudicate_retained_attempt = [&](const RefTxnId & id, const std::optional<String> & stored)
    {
        if (retained_attempt && retained_attempt->txn_id == id)
        {
            /// `NeedsRecovery` retains the writer's exact bytes for this adjudication. A storage
            /// violation can replace a write-once log object between the failed install and recovery;
            /// installing another ordinary transaction here would acknowledge history the admitted
            /// writer never created. A successor's tombstone is the conclusive loss case: it is
            /// replayed as the durable record.
            chassert(stored);
            if (classifyRefLogOccupant(ns, id, *stored, retained_attempt->bytes).kind == Occupant::Foreign)
                throw Exception(ErrorCodes::CORRUPTED_DATA,
                    "CAS ref-table recovery for namespace '{}': a DIFFERENT object occupies retained "
                    "writer txn {}-{}; recovery must not install it as that writer's history",
                    ns.string(), id.writer_epoch, id.ref_sequence);
        }
    };
    /// False asks for a restart: the record may not apply because the base moved under the walk.
    const auto applied_under_unmoved_base = [&](RefLogTxn && txn, uint64_t encoded_bytes)
    {
        try
        {
            apply_one(std::move(txn), encoded_bytes);
        }
        catch (const Exception & e)
        {
            if (restartsOnMovedBase(e.code(), op, layout, life, accepted.etag))
                return false;
            throw;
        }
        return true;
    };
    bool base_moved_under_apply = false;
    const RefChainVisitor visit = [&](RefLogTxn && txn, uint64_t stored_bytes)
    {
        adjudicate_retained_attempt(txn.txn_id, reader.capturedBytes());
        if (!applied_under_unmoved_base(std::move(txn), stored_bytes))
        {
            base_moved_under_apply = true;
            return false;
        }
        checkRecoveryStillAdmitted(ns, rt, cancelled, token);
        return true;
    };

    /// The walk never re-reads `_ckpt` itself: the re-read after the last read or write of this attempt
    /// covers its end and every tombstone create.
    RefChainOrigin origin{
        .first_key = *grounding.walk_from,
        .open_transition = std::nullopt,
        .ckpt_token = accepted.etag,
        .confirm_base_after_end = false,
        .old_frontier = grounding.committed_through};

    size_t slot_attempts_this_epoch = 0;
    for (;;)
    {
        checkRecoveryStillAdmitted(ns, rt, cancelled, token);
        const RefChainWalk walk = walkRefChain(op, layout, life, origin, visit, &reader);
        if (walk.end == RefChainEnd::BaseMoved || base_moved_under_apply)
            return std::nullopt;
        if (walk.end == RefChainEnd::Terminal)
            break;
        chassert(walk.end == RefChainEnd::AbsentKey);

        const RefTxnId id = walk.next_key;
        const uint64_t chain_epoch = id.writer_epoch;

        /// Only a generation 1 or 2 checkpoint records a seal; one in the epoch of the absent key and
        /// above it makes that key a hole, not the end of the chain.
        const bool sampled_seal_is_after_hole = sampled_ckpt->ckpt.last_epoch_seal
            && sampled_ckpt->ckpt.last_epoch_seal->writer_epoch == chain_epoch
            && id.ref_sequence < sampled_ckpt->ckpt.last_epoch_seal->ref_sequence;
        if (sampled_seal_is_after_hole)
        {
            checkRecoveryStillAdmitted(ns, rt, cancelled, token);
            if (op.read(layout.refLogKey(life, *sampled_ckpt->ckpt.last_epoch_seal), Retry::standard()))
            {
                ProfileEvents::increment(ProfileEvents::CASRefRecoveryStreamHole);
                hole_detail = fmt::format(
                    "id {}-{} is absent while the exact checkpoint records same-epoch seal {}-{} — the "
                    "ref-log stream is dense by construction, so this is a hole, not the end of "
                    "the stream",
                    id.writer_epoch, id.ref_sequence,
                    sampled_ckpt->ckpt.last_epoch_seal->writer_epoch,
                    sampled_ckpt->ckpt.last_epoch_seal->ref_sequence);
                return std::nullopt;
            }
        }

        /// Only a chain whose epoch may have ended uncleanly is sealed here; the lane writes the tombstone of
        /// any other chain before its first append. A cleanly ended epoch drained its lanes, and a recovery
        /// tombstone of a later epoch reaches its slot only if that epoch's marker covers it: markers never
        /// decrease, so the live marker covers it too.
        if (chain_epoch >= live.writer_epoch || chain_epoch > live.last_unclean_epoch)
            break;

        /// Close the chain with a tombstone naming the live epoch, so a late create of the chain's epoch
        /// meets the tombstone at its slot. A generation 3 `_ckpt` precedes it.
        if (accepted.ckpt.generation < G_REF_CHAIN)
        {
            checkRecoveryStillAdmitted(ns, rt, cancelled, token);
            CkptStampResult stamp = stampCkptGeneration3(op, layout, life, accepted);
            if (stamp.outcome == CkptStampOutcome::TokenMismatch)
                return std::nullopt;
            if (stamp.outcome == CkptStampOutcome::Unresolved)
                throwCasWriteRetryLater(fmt::format(
                    "CAS ref-table recovery for namespace '{}': the generation 3 stamp of its `_ckpt` is "
                    "unresolved; nothing is installed", ns.string()));
            accepted = std::move(*stamp.stamped);
        }

        if (++slot_attempts_this_epoch > kRefRecoveryMaxSlotAttemptsPerEpoch)
            throw Exception(ErrorCodes::CORRUPTED_DATA,
                "CAS ref-table recovery for namespace '{}': the tombstone slot of epoch {} was taken by a "
                "straggler {} times running. At most one append per table and writer is in flight at a "
                "time, so a store that keeps materializing objects underneath this walk is not a race "
                "this recovery may keep chasing",
                ns.string(), chain_epoch, slot_attempts_this_epoch - 1);

        /// Everything that can throw is prepared BEFORE the conditional create, so a failure here
        /// happens while the slot is provably untouched.
        RefLogTxn tombstone = makeEpochTombstoneTxn(ns, id, live.writer_epoch, walk.open_transition);
        validateEpochSealGrammarContextual(tombstone, life_epoch);
        const String tombstone_bytes = sealObject(FormatId::RefLog, encodeRefLogTxn(tombstone));

        /// One bounded attempt under the generation this recovery was admitted at: a tombstone written
        /// by an incarnation that no longer owns the namespace is a write from a dead mount.
        checkRecoveryStillAdmitted(ns, rt, cancelled, token);
        const WriteResult created = op.create(layout.refLogKey(life, id), tombstone_bytes, Retry::once());

        if (std::holds_alternative<Committed>(created))
        {
            apply_one(std::move(tombstone), tombstone_bytes.size());
            ProfileEvents::increment(ProfileEvents::CASRefRecoveryEpochSealed);
            break;
        }

        if (const auto * conflict = std::get_if<Conflict>(&created))
        {
            const auto * occupant_object = std::get_if<Object>(&conflict->seen);
            if (!occupant_object)
                throwCasWriteRetryLater(fmt::format(
                    "CAS ref-table recovery for namespace '{}': the tombstone at {}-{} lost its slot "
                    "to an occupant the settling read could not observe; the table stays unrecovered",
                    ns.string(), id.writer_epoch, id.ref_sequence));

            std::optional<RefLogTxn> occupant;
            try
            {
                occupant = decodeRefLogTxn(openObject(FormatId::RefLog, occupant_object->bytes), ns.string(), id);
            }
            catch (const Exception & e)
            {
                if (restartsOnMovedBase(e.code(), op, layout, life, accepted.etag))
                    return std::nullopt;
                throw;
            }
            adjudicate_retained_attempt(id, occupant_object->bytes);
            ProfileEvents::increment(refLogTxnIsEpochSeal(*occupant)
                ? ProfileEvents::CASRefRecoveryEpochSealAdopted : ProfileEvents::CASRefRecoveryStragglerAdopted);
            const std::optional<RefTxnId> successor = refChainSuccessor(*occupant);
            const std::optional<RefEpochTransition> transition = refEpochTransitionOf(*occupant);
            if (!applied_under_unmoved_base(std::move(*occupant), occupant_object->bytes.size()))
                return std::nullopt;
            checkRecoveryStillAdmitted(ns, rt, cancelled, token);
            if (!successor)
                break;
            if (successor->writer_epoch != chain_epoch)
                slot_attempts_this_epoch = 0;
            origin = RefChainOrigin{
                .first_key = *successor,
                .open_transition = transition,
                .ckpt_token = accepted.etag,
                .confirm_base_after_end = false,
                .old_frontier = std::nullopt};
            continue;
        }

        if (const auto * gave_up = std::get_if<GaveUp>(&created))
            throwCasWriteRetryLater(fmt::format(
                "CAS ref-table recovery for namespace '{}': the tombstone at {}-{} is UNRESOLVED ({}); the "
                "table stays unrecovered", ns.string(), id.writer_epoch, id.ref_sequence,
                gave_up->sent_any ? "the outcome of the attempt is unknown" : "nothing was sent"));

        const auto & refused = std::get<Refused>(created);
        throwCasWriteRetryLater(fmt::format(
            "CAS ref-table recovery for namespace '{}': the store refused the tombstone at {}-{} ({}); "
            "the table stays unrecovered", ns.string(), id.writer_epoch, id.ref_sequence, refused.message));
    }

    /// The linearization point. A create into another epoch's slot, or an end read as absent, proves the
    /// end of the chain only if cleanup had not moved the base: cleanup deletes only below a snapshot id
    /// it read from `_ckpt`, so an unchanged token proves none of those keys was emptied under the walk.
    checkRecoveryStillAdmitted(ns, rt, cancelled, token);
    const std::optional<CkptSample> final_ckpt = readCkpt(op, layout, life);
    if (!final_ckpt || final_ckpt->etag != accepted.etag || final_ckpt->ckpt != accepted.ckpt)
        return std::nullopt;
    checkRecoveryStillAdmitted(ns, rt, cancelled, token);

    RecoveryResult result = std::move(builder).finish();

    /// `finish` returns the candidate WITHOUT materializing: `stateFromSnapshot` loads every committed
    /// row and owned-manifest entry into the COW OVERLAY, and no tail transaction materializes either.
    /// This state is the table's long-lived working state, so fold both `committed` and `owned_manifests`
    /// into fresh shared bases ONCE here -- rather than making the first flush's scratch copy (and every
    /// per-item/shape-check copy on it) deep-copy an N-row overlay. The O(N) fold rides inside recovery,
    /// which is already O(N).
    result.state.materializeCommitted();
    /// Stale-precommit cleanup is dispatched once, from `appendRefOps`'s top level (never from here --
    /// this call may itself be nested inside a queue leader's flush stack).
    result.needs_stale_precommit_sweep = true;
    result.chain_epoch = result.state.isTerminal() ? 0 : result.state.chainEpoch(life_epoch);
    result.accepted_ckpt = std::move(accepted);
    /// Per-table admission budgets pre-subtract this table's own wire overhead (`4 + ns.size()`, once in
    /// a snapshot body and once in a removal txn body) plus a fixed safety margin from the raw hard
    /// limits, once, here.
    const uint64_t overhead = 4 + ns.string().size() + kRefAdmissionSafetyMargin;
    result.snapshot_budget = overhead < ref_snapshot_max_bytes ? ref_snapshot_max_bytes - overhead : 0;
    result.removal_budget = overhead < ref_removal_max_bytes ? ref_removal_max_bytes - overhead : 0;

    return result;
}

NamespaceLifeId CasRefLedger::resolveNamespaceLife(
    const RootNamespace & ns, uint64_t admitted_generation, uint64_t live_epoch,
    bool * lifecycle_refusal)
{
    /// The SECONDARY bound. Wall-clock time is already bounded by the frozen policy below, which
    /// every verb of every iteration shares; this cap exists so a sequence that somehow converges on
    /// neither a resolution nor the deadline still ends. A duel between two openers (one creating, one
    /// reconciling a stale creator) converges in a handful of rounds.
    static constexpr size_t kMaxResolveAttempts = 32;
    const CreatorFence our_fence{server_root_id, live_epoch, admitted_generation};
    CasOperation op = mount_requests.resume(admitted_generation);

    /// ONE bound for the whole loop, frozen before the first iteration: every read and every protocol
    /// call below shares this deadline, so a namespace whose catalog entry keeps moving gives up
    /// retry-later within one standard window rather than spending a fresh window per verb per
    /// iteration. The paced re-read below is a bare sleep that does not consult the deadline, so the
    /// loop can sleep one backoff (at most 5 s) past it before the next read refuses to start.
    const Retry policy = op.freeze(Retry::standard());

    for (size_t attempt = 0; attempt < kMaxResolveAttempts; ++attempt)
    {
        /// Paces ONLY the re-reads a competing actor forced. A `Live` outcome is this open's own
        /// success and its single confirming re-read is not contention, so it is never delayed. The
        /// argument is the number of collisions so far, so the first one waits the shortest draw.
        const auto paceReRead = [&] { op.pause(Retry::backoff(static_cast<uint32_t>(attempt) + 1)); };

        const CasRefCatalog::Snapshot snap = CasRefCatalog::read(op, layout, policy);
        const auto it = std::find_if(snap.catalog.entries.begin(), snap.catalog.entries.end(),
            [&](const CatalogEntry & e) { return e.ns.string() == ns.string(); });

        if (it == snap.catalog.entries.end())
        {
            /// No entry at all: this open is the namespace's first-ever opener. `createNamespace`
            /// mints a fresh incarnation and carries it all the way to `Live` (or reports why it could
            /// not); either way it does not hand the incarnation back, so a `Live` outcome re-reads the
            /// catalog on the next loop iteration to learn it -- one extra GET, paid once per birth,
            /// never per write.
            const auto outcome = CasRefCatalog::createNamespace(
                op, layout, config.gc_shards, ns, our_fence, policy);
            if (outcome == CasRefCatalog::NamespaceCreationOutcome::FencedOut)
                throwCasWriteRetryLater(fmt::format(
                    "CAS ref-table recovery for namespace '{}': the mount incarnation moved while "
                    "birthing its catalog entry; the last attempt's fate is unresolved and nothing is "
                    "installed", ns.string()));
            if (outcome == CasRefCatalog::NamespaceCreationOutcome::Superseded)
                paceReRead();
            continue;   /// Live or Superseded: re-read (Superseded means a DIFFERENT actor won birth)
        }

        if (it->state == NsState::Live)
            return NamespaceLifeId::fromCatalogEntry(it->ns, it->incarnation);

        if (it->state == NsState::Removing)
        {
            if (lifecycle_refusal)
                *lifecycle_refusal = true;
            throwCasWriteRetryLater(fmt::format(
                "CAS namespace '{}' is Removing: creation waits for its terminal fold and catalog "
                "removal to complete; retry later", ns.string()));
        }

        /// `Creating`, with a strict-grammar creator fence (`CatalogEntry`'s own invariant guarantees
        /// `it->creator` is set whenever `state == Creating`). If it names THIS mount's own currently
        /// live fence, an earlier attempt of this SAME open landed step 1 (`casAdmitEntry`) but not
        /// steps 2/3 -- e.g. a transient failure inside `completeCreation`'s own `_ckpt` create --
        /// and resuming is simply re-running steps 2/3 over the entry as observed just now. Reasoning
        /// about fence terminality for our OWN live fence would never terminate (we are, by definition,
        /// not dead), so this case is checked FIRST and unconditionally, before any terminality probe.
        if (it->creator->server_root_id == server_root_id && it->creator->writer_epoch == live_epoch)
        {
            const auto outcome = CasRefCatalog::completeCreation(op, layout, *it, policy);
            if (outcome == CasRefCatalog::NamespaceCreationOutcome::FencedOut)
                throwCasWriteRetryLater(fmt::format(
                    "CAS ref-table recovery for namespace '{}': the mount incarnation moved while "
                    "resuming its own stalled creation; the last attempt's fate is unresolved and nothing "
                    "is installed",
                    ns.string()));
            if (outcome == CasRefCatalog::NamespaceCreationOutcome::Superseded)
                paceReRead();
            continue;   /// Live or Superseded: re-read either way
        }

        /// A DIFFERENT actor's `Creating` entry. It may still be mid-flight (retry later, against a
        /// fresh read -- never busy-loop this instant) or provably dead, in which case reconciliation
        /// steals it onto our own fence and this open resumes `completeCreation` itself.
        const auto reconcile_outcome = CasRefCatalog::reconcileStaleCreator(
            op, layout, *it, our_fence,
            [&](const CreatorFence & f) { return isCreatorFenceTerminal(op, layout, f.server_root_id, f.writer_epoch, policy); },
            policy);
        switch (reconcile_outcome)
        {
            case CasRefCatalog::ReconcileCreatorOutcome::FencedOut:
                /// Our OWN mount fence moved before the steal CAS -- the CAS's fate is unresolved, and
                /// this mount is the wrong actor to retry (its incarnation is gone).
                throwCasWriteRetryLater(fmt::format(
                    "CAS ref-table recovery for namespace '{}': the mount incarnation moved while "
                    "reconciling a stalled foreign creator; the last attempt's fate is unresolved and "
                    "nothing is installed",
                    ns.string()));
            case CasRefCatalog::ReconcileCreatorOutcome::Reconciled:
            {
                CatalogEntry resumed = *it;
                resumed.creator = our_fence;
                const auto outcome = CasRefCatalog::completeCreation(op, layout, resumed, policy);
                if (outcome == CasRefCatalog::NamespaceCreationOutcome::FencedOut)
                    throwCasWriteRetryLater(fmt::format(
                        "CAS ref-table recovery for namespace '{}': the mount incarnation moved while "
                        "completing a reconciled creation; the last attempt's fate is unresolved and "
                        "nothing is installed",
                        ns.string()));
                if (outcome == CasRefCatalog::NamespaceCreationOutcome::Superseded)
                    paceReRead();
                continue;   /// Live or Superseded: re-read either way
            }
            case CasRefCatalog::ReconcileCreatorOutcome::CreatorFenceStillLive:
                if (lifecycle_refusal)
                    *lifecycle_refusal = true;
                throwCasWriteRetryLater(fmt::format(
                    "CAS ref-table recovery for namespace '{}': its catalog entry is still Creating "
                    "under a creator fence that is not yet provably dead; retry later", ns.string()));
            case CasRefCatalog::ReconcileCreatorOutcome::EntryChanged:
                /// Token-exactness failed: someone else already moved this entry. Pace before re-reading.
                paceReRead();
                continue;
        }
    }

    throwCasWriteRetryLater(fmt::format(
        "CAS ref-table recovery for namespace '{}': its catalog entry did not converge to a resolvable "
        "incarnation after {} attempts", ns.string(), kMaxResolveAttempts));
}

RefRecoveryOutcome CasRefLedger::ensureRefTableRecovered(
    const RootNamespace & ns, RefTableRuntime & rt, const std::optional<DetachedStopToken> & token)
{
    bool life_removed = false;
    {
    std::unique_lock lock(rt.state_mutex);
    /// Every touch, warm or cold,
    /// marks this table most-recently-used so `enforceRefTableCacheBudget` evicts idle tables first.
    rt.last_touch_tick = ref_table_access_tick.fetch_add(1, std::memory_order_relaxed) + 1;

    /// `NeedsRecovery` is a hard lane fence: this cache may lack a durable transaction, or its chain or
    /// `_ckpt` moved under it. Replaying the durable stream is the only transition back to `Ready`.
    const auto needs_rerecovery = [&rt]
    {
        return rt.lane_state == RefLaneState::NeedsRecovery;
    };
    if (rt.recovered && !needs_rerecovery())
        return RefRecoveryOutcome::Recovered;

    /// A concurrent second caller waits here rather than racing an independent walk against the first
    /// caller's unlocked I/O.
    while (rt.recovery_in_progress)
    {
        if (token && token->stopping())
            throw Exception(ErrorCodes::ABORTED,
                "CAS ref recovery: teardown began while waiting for a concurrent recovery");
        ++rt.recovery_waiters_for_test;
        rt.recovery_cv.wait_for(lock, std::chrono::milliseconds(200));
        --rt.recovery_waiters_for_test;
    }
    if (rt.recovered && !needs_rerecovery())
        return RefRecoveryOutcome::Recovered;   /// the caller we waited on already finished it
    /// Set under this lock before `recovery_in_progress` falls, so no recovery starts on a removed life.
    if (rt.recovery_ended_life_removed)
        return RefRecoveryOutcome::LifeRemoved;

    rt.recovery_in_progress = true;
    /// Cleared + broadcast on every exit from here, success or exception -- so a parked waiter is never
    /// left hanging, and so the self-remount barrier's JOIN completes on a failed attempt as surely as
    /// on a successful one.
    SCOPE_EXIT({
        if (life_removed)
            rt.recovery_ended_life_removed = true;
        rt.recovery_in_progress = false;
        rt.recovery_cv.notify_all();
    });

    /// ---- Step 1: capture the admitted generation, ONCE ----
    /// This ONE value admits the walk's operation, so every
    /// request it makes is measured against it, and the install below presents it one final time. One
    /// capture point, no re-derivation -- a value re-read midway would let a recovery that lost the
    /// mount "recover" its right to write by observing a fresh incarnation it was never admitted under.
    ///
    /// Captured for the WHOLE call, not per attempt, for the same reason: the transient-retry loop below
    /// exists for object-store blips, and a generation that moved is not one. The loop refuses to
    /// re-drive under a moved generation (below), so the budget is never burned on a doomed retry.
    const uint64_t admitted_generation = rt.admitted_fence_generation;
    refuseUnlessAdmitted(mount_requests.resume(admitted_generation), "ref recovery walk admission");
    /// Preserve this runtime's exact writer identity across the unlocked walk. The runtime stays in
    /// `NeedsRecovery` until the same lock installs a result, so no later append can replace it here.
    const std::optional<RefAppendAttempt> retained_attempt = rt.append_attempt;
    /// The live writer epoch and its `last_unclean_epoch`, likewise captured once and as one pair: they
    /// decide whether the walk writes a tombstone. Re-reading them mid-walk would let the boundary move
    /// under the decision.
    const LiveEpochPair live = live_epoch_pair_fn();

    /// Outer transient-retry loop, so a blip does not fail a table load for good: a whole recovery attempt that
    /// fails with a TRANSIENT object-store transport error (`isTransientRecoveryError` -- `S3_ERROR`/
    /// socket/timeout from the direct LIST/GET backend calls, or a controller `NETWORK_ERROR`) is retried
    /// with capped-exponential backoff until `recovery_retry_budget_ms` is spent, instead of propagating
    /// and failing this table's async load permanently. Non-transient errors (corruption, decode, a moved
    /// fence, logic, resource limits) fail fast; so do the latched terminal cases below.
    const uint64_t recovery_start_ms = boot_ms_now_fn();
    uint64_t recovery_retry_num = 0;
    bool vanish_brake_tripped = false;
    bool cancelled = false;
    for (;;)
    {
        try
        {
            std::optional<String> hole_detail;
            for (uint64_t attempt = 0; ; ++attempt)
            {
                if (attempt > 0)
                {
                    if (attempt > kRefRecoveryMaxRestarts)
                    {
                        /// Terminal, NOT a transient object-store outage: latch so the outer retry loop
                        /// rethrows immediately instead of re-driving this brake for the whole budget.
                        vanish_brake_tripped = true;
                        if (hole_detail)
                            throw Exception(ErrorCodes::CORRUPTED_DATA,
                                "CAS ref-table recovery for namespace '{}' found a hole in the durable ref-log "
                                "stream that persisted across {} re-reads: {}",
                                ns.string(), attempt - 1, *hole_detail);
                        throwCasWriteRetryLater(fmt::format(
                            "CAS ref-table recovery for namespace '{}' restarted {} times (a selected snapshot "
                            "kept vanishing between the checkpoint that named it and its GET) — giving up; this "
                            "bound is a runaway brake against a pathological cleanup race, not an expected "
                            "steady state", ns.string(), attempt - 1));
                    }
                    ++rt.recovery_restarts;
                    ProfileEvents::increment(ProfileEvents::CASRefRecoveryRestarts);
                }

                /// Each attempt derives its own restart reason, so a prior hole is not misreported as a
                /// selected-base vanish after the next attempt takes a different path.
                hole_detail.reset();

                /// The whole walk runs UNLOCKED. Nothing it touches is shared: the candidate is private
                /// and `rt` is read only through atomics the poll consults. Unlocking is what keeps a
                /// recovery's full I/O envelope from stalling every reader of this table -- and what lets
                /// the self-remount barrier take the lock promptly to JOIN us.
                lock.unlock();
                std::optional<RecoveryResult> walked;
                try
                {
                    walked = runRecoveryWalkOnce(
                        ns, rt, admitted_generation, live, retained_attempt, hole_detail, cancelled, token);
                    /// The admission's catalog read precedes the walk, and another server's GC may have
                    /// erased the life and deleted its objects under it.
                    if (walked)
                        life_removed = !confirmRecoveredLifeOrDetach(rt, admitted_generation);
                }
                catch (...)
                {
                    /// Re-acquire BEFORE letting anything propagate: the SCOPE_EXIT above mutates
                    /// `recovery_in_progress` and notifies `recovery_cv`, and it MUST run with
                    /// `state_mutex` held -- unwinding through it unlocked would be a data race on the
                    /// plain bool and an unlocked notify.
                    lock.lock();
                    throw;
                }
                lock.lock();

                if (!walked)
                    continue;   /// restart requested (vanished base, or a hole worth one more reading)
                if (life_removed)
                    break;

                /// ---- Step 8: the install recheck, the LAST member of the trio ----
                /// Under the re-acquired lock and IMMEDIATELY before the install, present the admitted
                /// generation one final time. Everything above ran on I/O that took an unbounded amount
                /// of time to come back, and a recovery whose window straddled a fence bump describes a
                /// mount incarnation that no longer owns this namespace. It must publish NOTHING: the
                /// table stays unrecovered and the next touch recovers it properly.
                if (recovery_install_probe_for_test)
                    recovery_install_probe_for_test();
                refuseUnlessAdmitted(mount_requests.resume(admitted_generation), "ref recovery install");
                if (rt.catalog_life_invalidated.load(std::memory_order_acquire))
                    throwCasWriteRetryLater(fmt::format(
                        "CAS ref-table recovery for namespace '{}': catalog retirement invalidated life {} "
                        "before recovery install — nothing is installed",
                        ns.string(), renderIncarnation(rt.life.incarnation)));
                if (rt.superseded_by_remount.load(std::memory_order_acquire))
                    throwCasWriteRetryLater(fmt::format(
                        "CAS ref-table recovery for namespace '{}': this cached table was superseded by a "
                        "self-remount while the walk was in flight — nothing is installed",
                        ns.string()));
                if (token && token->stopping())
                    throw Exception(ErrorCodes::ABORTED,
                        "CAS ref recovery: teardown began before recovery install");

                /// One atomic publication under `state_mutex`: `installRecoveryResult` copies every seeded
                /// field from the result and sets `recovered` LAST, so no waiter (woken only by the
                /// function-scope SCOPE_EXIT's `recovery_cv` notify, which runs after this returns) ever
                /// observes a partially-installed table.
                installRecoveryResult(rt, std::move(*walked), live.writer_epoch);
                recovery_install_count_for_test.fetch_add(1, std::memory_order_relaxed);
                break;
            }
            break;   /// recovery succeeded -> exit the outer retry loop
        }
        catch (...)
        {
            /// `catch (...)`, not `catch (const Exception &)`: recovery exact-GET failures can surface as
            /// a raw object-storage transport exception (even a non-`DB::Exception` Poco timeout), which
            /// a `catch (const Exception &)` would not even see. `getCurrentExceptionCode()` normalises
            /// every exception (DB, Poco, std) to a code so the transient classifier can decide.
            const int code = getCurrentExceptionCode();
            /// Corruption of a life the catalog no longer names is the janitor's work, not damage.
            if (code == ErrorCodes::CORRUPTED_DATA)
            {
                lock.unlock();
                bool confirmed = true;
                try
                {
                    confirmed = confirmRecoveredLifeOrDetach(rt, admitted_generation);
                }
                catch (...)  // NOLINT(bugprone-empty-catch)
                {
                    /// Without a confirmation the walk's own error is the answer.
                }
                lock.lock();
                if (!confirmed)
                {
                    life_removed = true;
                    break;
                }
            }
            /// `cancelled` is latched by the poll itself: a self-remount's cancellation uses the
            /// retry-later class (the caller should retry, against the FRESH incarnation), so without the
            /// latch this loop would read it as a transient blip and re-drive the very work the remount
            /// just stopped. Detached teardown is independently terminal even when the in-flight
            /// exception happened to carry a transient transport code.
            if (vanish_brake_tripped || cancelled || (token && token->stopping())
                || !isTransientRecoveryError(code))
                throw;   /// a latched terminal case, or a non-transient failure -- fail fast

            /// A cancellation now ends the walk through the operation's liveness, which refuses
            /// silently -- so the exception that arrives here carries a transport code and `cancelled`
            /// is not yet latched. Take the latch before the backoff rather than one whole attempt
            /// later: the self-remount barrier is waiting for this recovery to stop.
            if (rt.recovery_cancel_requested.load(std::memory_order_acquire))
                checkRecoveryStillAdmitted(ns, rt, cancelled, token);

            const uint64_t elapsed_ms = boot_ms_now_fn() - recovery_start_ms;
            /// Fail closed BEFORE sleeping: budget spent, mount fence lost, this runtime superseded by a
            /// self-remount, or the incarnation that admitted this recovery has moved (retrying under a
            /// generation that is already stale can only ever be refused at the install).
            if (elapsed_ms >= cas_request_budget.recovery_retry_budget_ms
                || (token && token->stopping())
                || !fence_ok_fn()
                || rt.catalog_life_invalidated.load(std::memory_order_acquire)
                || rt.superseded_by_remount.load(std::memory_order_acquire)
                || fence_generation_fn() != admitted_generation)
                throw;

            /// Saturating `initial << recovery_retry_num`: `initial > cap >> n` implies the unshifted
            /// product already exceeds the cap, so return the cap without ever computing an
            /// overflowing/UB shift for large retry counts.
            const uint64_t init_backoff = cas_request_budget.recovery_retry_initial_backoff_ms;
            const uint64_t cap_backoff = cas_request_budget.recovery_retry_max_backoff_ms;
            const uint64_t backoff_ms = (recovery_retry_num >= 63 || init_backoff > (cap_backoff >> recovery_retry_num))
                ? cap_backoff
                : std::min(cap_backoff, init_backoff << recovery_retry_num);
            ++recovery_retry_num;
            ProfileEvents::increment(ProfileEvents::CASRefRecoveryRetries);
            LOG_WARNING(getLogger("CasRefLedger"),
                "CAS ref-table recovery for namespace '{}' hit a transient object-store error "
                "(code {}: {}); retry #{} after {}ms backoff (elapsed {}ms / budget {}ms)",
                ns.string(), code, getCurrentExceptionMessage(/*with_stacktrace=*/false),
                recovery_retry_num, backoff_ms, elapsed_ms, cas_request_budget.recovery_retry_budget_ms);

            lock.unlock();
            /// Re-acquire the lock before letting any exception from the sleep unwind, so the SCOPE_EXIT
            /// (which mutates `recovery_in_progress` + notifies `recovery_cv` and MUST run under
            /// `state_mutex`) never runs unlocked -- same obligation as the walk's window above.
            try
            {
                recovery_retry_sleep_fn(backoff_ms, token);
            }
            catch (...)
            {
                lock.lock();
                throw;
            }
            lock.lock();
            /// The fence/supersession/budget can all change during the unlocked sleep -- re-check before
            /// starting the next full attempt so we never re-drive recovery on an orphaned runtime, past
            /// the budget, or under a lost or moved fence (the sliced sleep may have woken early on fence
            /// loss).
            if (boot_ms_now_fn() - recovery_start_ms >= cas_request_budget.recovery_retry_budget_ms
                || (token && token->stopping())
                || !fence_ok_fn()
                || rt.catalog_life_invalidated.load(std::memory_order_acquire)
                || rt.superseded_by_remount.load(std::memory_order_acquire)
                || fence_generation_fn() != admitted_generation)
                throw;
            /// loop: re-run recovery from a fresh checkpoint read, exact replay and walk
        }
    }
    }

    if (life_removed)
        return RefRecoveryOutcome::LifeRemoved;
    /// A NEW table was just materialized; enforce the whole-table cache budget, protecting this one.
    /// The pass runs OUTSIDE `rt.state_mutex` (that scope closed above) so
    /// the pass -- which acquires `ref_queue_mutex` and try-locks other tables' `state_mutex` -- never
    /// nests this table's `state_mutex` under `ref_queue_mutex`.
    enforceRefTableCacheBudget(ns);
    return RefRecoveryOutcome::Recovered;
}

bool CasRefLedger::confirmRecoveredLifeOrDetach(RefTableRuntime & rt, uint64_t admitted_generation)
{
    CasOperation op = mount_requests.resume(admitted_generation);
    const CasRefCatalog::Snapshot snapshot = CasRefCatalog::read(op, layout);
    refuseUnlessAdmitted(op, "ref recovery catalog confirmation");
    if (!catalogStillNamesLife(snapshot.catalog, rt.life))
    {
        invalidateRemovedCatalogLife(rt.life);
        return false;
    }
    const bool removing = std::any_of(snapshot.catalog.entries.begin(), snapshot.catalog.entries.end(),
        [&](const CatalogEntry & entry)
        {
            return entry.ns == rt.life.ns && entry.incarnation == rt.life.incarnation && entry.state == NsState::Removing;
        });
    /// A row that turned after admission; `appendRefOpsOnRuntime` re-reads the flag after recovery.
    bool latched_now = false;
    if (removing)
    {
        std::lock_guard queue_lock(ref_queue_mutex);
        latched_now = latchRemovingLocked(rt);
    }
    if (latched_now)
        logRemovingLatched(rt.life);
    return true;
}

void CasRefLedger::installRecoveryResult(RefTableRuntime & rt, RecoveryResult && result, uint64_t live_epoch)
{
    /// One place that seeds a recovered table's runtime, copying EVERY `RecoveryResult` field so the
    /// publication cannot drift from the struct. `recovered` is set LAST: the caller holds `state_mutex`
    /// throughout and the function-scope SCOPE_EXIT notifies `recovery_cv` only after this returns, so a
    /// parked waiter re-checking `recovered` under the same lock sees a fully-installed table or none.
    rt.state = std::move(result.state);
    rt.newest_snapshot_id = result.newest_snapshot_id;
    rt.accepted_ckpt = std::move(result.accepted_ckpt);
    rt.tail_count_since_snapshot.store(result.tail_count, std::memory_order_relaxed);
    rt.tail_bytes_since_snapshot.store(result.tail_bytes, std::memory_order_relaxed);
    rt.base_snapshot_bytes.store(result.base_snapshot_bytes, std::memory_order_relaxed);
    rt.snapshot_budget = result.snapshot_budget;
    rt.removal_budget = result.removal_budget;
    rt.needs_stale_precommit_sweep = result.needs_stale_precommit_sweep;
    rt.append_attempt.reset();
    /// A chain in a later epoch was closed by a successor: this mount reads the table and appends nothing.
    /// A read-only pool holds no lease and has no live epoch (0), so no successor can depose it.
    rt.lane_state = live_epoch != 0 && result.chain_epoch > live_epoch ? RefLaneState::Closed : RefLaneState::Ready;
    if (rt.lane_state == RefLaneState::Closed)
    {
        try
        {
            const auto & open = rt.state.getOpenTransition();
            logLaneStopped(rt.life, RefLaneState::Closed, live_epoch,
                open ? fmt::format("{}-{}", open->seal_id.writer_epoch, open->seal_id.ref_sequence) : String(),
                fmt::format("recovery found the chain in epoch {}, after this writer's", result.chain_epoch));
        }
        catch (...)   // NOLINT(bugprone-empty-catch)
        {
            /// The install must complete; a failed log line changes nothing.
        }
    }
    rt.recovered = true;   /// set LAST
}

void CasRefLedger::cancelRecoveriesAndAwaitQuiescence()
{
    /// Snapshot the runtimes (the copies keep them alive across the wait, exactly as
    /// `quiesceRefTablesForRemount` does).
    std::vector<std::shared_ptr<RefTableRuntime>> tables;
    {
        std::lock_guard<std::mutex> qlock(ref_queue_mutex);
        tables.reserve(ref_name_slots.size());
        for (auto & [name, slot] : ref_name_slots)
            if (slot.current)
                tables.push_back(slot.current);
    }

    /// Publish the request to EVERY table first, then wait -- never table-by-table. Requesting and
    /// waiting in one pass would let a recovery start on table B while we are still parked on table A,
    /// and we would then join work that began after the cancellation was already under way.
    for (auto & rt : tables)
        rt->recovery_cancel_requested.store(true, std::memory_order_release);

    for (auto & rt : tables)
    {
        std::unique_lock<std::mutex> slock(rt->state_mutex);
        rt->recovery_cv.wait(slock, [&] { return !rt->recovery_in_progress; });
    }

    /// Released once nothing is in flight. Clearing here rather than after the fence re-arm keeps this a
    /// self-contained barrier with no obligation on the caller to unwind: the window it opens (a recovery
    /// starting between here and the re-arm) is closed twice over by the re-arm's own generation bump and
    /// by `quiesceRefTablesForRemount`'s `superseded_by_remount`, both of which the walk polls.
    for (auto & rt : tables)
        rt->recovery_cancel_requested.store(false, std::memory_order_release);
}


void CasRefLedger::enforceRefTableCacheBudget(const RootNamespace & keep_ns)
{
    if (config.ref_table_cache_bytes == 0)
        return;   /// 0 = unbounded: eviction disabled

    /// Evicted runtimes are held alive here until AFTER every lock is released, so a runtime whose sole
    /// owner is its map slot is never destroyed while we still hold its `state_mutex` (that would destroy
    /// a locked mutex).
    std::vector<std::shared_ptr<RefTableRuntime>> evicted;
    {
        std::lock_guard<std::mutex> qlock(ref_queue_mutex);

        /// Relaxed atomic reads: the `total` loop below reads this for EVERY table, including hot ones a
        /// concurrent append lane is mutating under `state_mutex` only (a cross-lock read). The gated
        /// candidate loop reads it too, but only for `use_count()==1` tables (no concurrent writer).
        const auto weightOf = [](const RefTableRuntime & rt)
        {
            return rt.base_snapshot_bytes.load(std::memory_order_relaxed)
                 + rt.tail_bytes_since_snapshot.load(std::memory_order_relaxed);
        };

        uint64_t total = 0;
        for (const auto & [name, slot] : ref_name_slots)
            if (slot.current)
                total += weightOf(*slot.current);
        if (total <= config.ref_table_cache_bytes)
            return;

        /// Idle candidates, least-recently-touched first. Idle == the map holds the SOLE `shared_ptr`
        /// (`use_count() == 1`: no in-flight caller, queued append, leader, or background publish holds a
        /// copy), no active queue leader, an empty pending queue, and not the just-recovered `keep_ns`.
        /// The `use_count() == 1` gate is what makes append-lane split-brain impossible: any thread that
        /// fetched this runtime keeps it non-evictable for as long as it holds the copy.
        struct Cand { String name; uint64_t tick; uint64_t weight; };
        std::vector<Cand> cands;
        for (const auto & [name, slot] : ref_name_slots)
        {
            if (name == keep_ns.string())
                continue;
            const auto & rt = slot.current;
            if (!rt)
                continue;
            /// A fresh runtime would reopen a lane that a possibly landing removal write keeps closed.
            if (rt.use_count() != 1 || rt->leader_active || !rt->pending.empty() || rt->removal_write_may_land)
                continue;
            cands.push_back(Cand{name, rt->last_touch_tick, weightOf(*rt)});
        }
        std::sort(cands.begin(), cands.end(),
                  [](const Cand & a, const Cand & b) { return a.tick < b.tick; });

        for (const Cand & c : cands)
        {
            if (total <= config.ref_table_cache_bytes)
                break;
            auto it = ref_name_slots.find(c.name);
            if (it == ref_name_slots.end())
                continue;
            std::shared_ptr<RefTableRuntime> & rt = it->second.current;
            if (!rt)
                continue;
            bool removal_applied = false;
            {
                /// `use_count() == 1` guarantees no other thread holds the runtime, so this try_lock
                /// cannot fail; the RAII scope releases `state_mutex` before `rt` is moved out. A wedged
                /// append lane is never evicted -- its uncertain in-flight PUT is not reconstructable from
                /// the durable objects, and re-recovery could re-allocate an id:
                /// Linearization forbids this).
                std::unique_lock<std::mutex> slock(rt->state_mutex, std::try_to_lock);
                if (!slock.owns_lock() || rt->lane_state != RefLaneState::Ready)
                    continue;
                removal_applied = rt->state.getLifecycle() != RefLifecycle::Live && rt->state.getRemoveTxnId().has_value();
            }
            if (rt.use_count() != 1 || rt->leader_active || !rt->pending.empty())
                continue;   /// re-check under the still-held ref_queue_mutex
            total -= c.weight;
            /// A removed life may be drained by another server's GC leader, which never calls
            /// `invalidateRemovedCatalogLife` here. Record the removal at eviction so a cold admission
            /// that read the row earlier is still refused.
            if (rt->removal_admission_closed || removal_applied)
                ++it->second.retirements;
            evicted.push_back(std::move(rt));   /// keep alive past the erase and lock release
            eraseNameSlotIfUnusedLocked(it);
            ProfileEvents::increment(ProfileEvents::CASRefTableEvictions);
        }
    }
    /// `evicted` destructs the dropped runtimes here, with no lock held.
}


void CasRefLedger::quiesceRefTablesForRemount()
{
    /// Snapshot the current runtimes (copies keep them alive across the drain). New dispatches are
    /// already suppressed while the fence is lost (`maybeScheduleSnapshotPublish`'s fence guard), so the
    /// only publishers to drain are those dispatched before the fence dropped.
    std::vector<std::shared_ptr<RefTableRuntime>> tables;
    {
        std::lock_guard<std::mutex> qlock(ref_queue_mutex);
        tables.reserve(ref_name_slots.size());
        for (auto & [name, slot] : ref_name_slots)
            if (slot.current)
                tables.push_back(slot.current);
    }

    /// Wait for every in-flight background publisher to finish so none is mid-PUT when its runtime is
    /// detached. A publisher observes the lost fence (`fence_ok` false) and returns without committing,
    /// then decrements `pending_snapshot_publishes` under `state_mutex` and signals `publish_settle_cv`.
    for (auto & rt : tables)
    {
        std::unique_lock<std::mutex> slock(rt->state_mutex);
        rt->publish_settle_cv.wait(slock,
            [&] { return rt->pending_snapshot_publishes.load(std::memory_order_relaxed) == 0; });
    }

    /// Detach every cached table. Mark it superseded FIRST (release, and before the caller re-arms the
    /// fence): a leader that raced in and holds one of these orphaned runtimes then fails closed at the
    /// `flushRefBatch` gate rather than allocating an id against a stale cache under the re-armed fence.
    /// Queued callers self-drain -- each `flushRefBatch` for a superseded runtime completes its whole
    /// carved batch with a retry error, so no caller hangs; the next touch creates a fresh runtime that
    /// re-recovers from the durable snapshot+log objects under `live_writer_epoch`. Dropping the map slot
    /// discards each runtime's in-memory wedge, and nothing about that drop needs to be certified here:
    /// the undecided `PUT` the wedge describes is settled by the durable protocol rather than by
    /// bookkeeping this process carries across the boundary. Recovery closes the chain with a tombstone
    /// written as a conditional create, so the wedged write either already landed (and the walk reads
    /// it) or loses its own create to the tombstone.
    std::vector<std::shared_ptr<RefTableRuntime>> detached;
    {
        std::lock_guard<std::mutex> qlock(ref_queue_mutex);
        detached.reserve(ref_name_slots.size());
        for (auto & [name, slot] : ref_name_slots)
        {
            auto & rt = slot.current;
            if (!rt)
                continue;
            rt->superseded_by_remount.store(true, std::memory_order_release);
            rt->cv.notify_all();   /// wake any waiter so it re-leads and fails closed against the flag
            detached.push_back(rt);
        }
        ref_name_slots.clear();
        ++ref_name_slots_epoch;
    }
    /// `detached` releases the map's references here (with no lock held); each runtime lives on only as
    /// long as an in-flight leader/caller still holds it.
}


uint64_t CasRefLedger::refRecoveryRestartsForTest(const RootNamespace & ns)
{
    const auto rt = lookupRefTableRuntime(ns);
    if (!rt)
        return 0;
    std::lock_guard lock(rt->state_mutex);
    return rt->recovery_restarts;
}

bool CasRefLedger::refLaneWedgedForTest(const RootNamespace & ns)
{
    const auto rt = lookupRefTableRuntime(ns);
    if (!rt)
        return false;
    std::lock_guard lock(rt->state_mutex);
    return rt->lane_state == RefLaneState::Wedged;
}

String CasRefLedger::wedgedKeyForTest(const RootNamespace & ns)
{
    const auto rt = lookupRefTableRuntime(ns);
    if (!rt)
        return {};
    std::lock_guard lock(rt->state_mutex);
    return rt->append_attempt ? rt->append_attempt->key : String{};
}

uint64_t CasRefLedger::wedgedAdmittedGenerationForTest(const RootNamespace & ns)
{
    const auto rt = lookupRefTableRuntime(ns);
    if (!rt)
        return 0;
    std::lock_guard lock(rt->state_mutex);
    return rt->append_attempt ? rt->append_attempt->admitted_fence_generation : 0;
}

std::optional<RefTxnId> CasRefLedger::lastEpochSealForTest(const RootNamespace & ns)
{
    const auto rt = lookupRefTableRuntime(ns);
    if (!rt)
        return std::nullopt;
    std::lock_guard lock(rt->state_mutex);
    const auto & open = rt->state.getOpenTransition();
    return open ? std::optional<RefTxnId>{open->seal_id} : std::nullopt;
}

void CasRefLedger::forceWedgeForTest(const RootNamespace & ns, uint64_t writer_epoch, uint64_t ref_sequence,
                              const String & key, const String & bytes,
                              std::optional<uint64_t> admitted_generation)
{
    const auto rt = acquireMutableRefTableRuntime(ns);
    if (ensureRefTableRecovered(ns, *rt) == RefRecoveryOutcome::LifeRemoved)
        throwCasWriteRetryLater(recoveredLifeRemovedMessage(ns, rt->life));
    /// Read outside `state_mutex`: it is an atomic load on the mount runtime, and taking it here keeps
    /// the seam's default identical to what a wedge born at this instant would carry.
    const uint64_t generation = admitted_generation.value_or(fence_generation_fn());
    std::lock_guard lock(rt->state_mutex);
    rt->append_attempt = RefAppendAttempt{RefTxnId{writer_epoch, ref_sequence}, key, bytes, generation};
    rt->lane_state = RefLaneState::Wedged;
}

RefLaneState CasRefLedger::laneStateForTest(const RootNamespace & ns)
{
    const auto rt = lookupRefTableRuntime(ns);
    if (!rt)
        return RefLaneState::Closed;
    std::lock_guard lock(rt->state_mutex);
    return rt->lane_state;
}

bool CasRefLedger::needsStalePrecommitSweepForTest(const RootNamespace & ns)
{
    const auto rt = lookupRefTableRuntime(ns);
    if (!rt)
        return false;
    std::lock_guard lock(rt->state_mutex);
    return rt->needs_stale_precommit_sweep;
}


size_t CasRefLedger::wedgedRefLaneCount()
{
    std::vector<std::shared_ptr<RefTableRuntime>> runtimes;
    {
        std::lock_guard<std::mutex> g(ref_queue_mutex);
        runtimes.reserve(ref_name_slots.size());
        for (const auto & [_, slot] : ref_name_slots)
            if (slot.current)
                runtimes.push_back(slot.current);
    }
    size_t wedged = 0;
    for (const auto & rt : runtimes)
    {
        std::lock_guard lock(rt->state_mutex);
        if (rt->lane_state == RefLaneState::Wedged)
            ++wedged;
    }
    return wedged;
}


bool CasRefLedger::drainRefLanesForShutdown(uint64_t wait_budget_ms)
{
    /// Latch FIRST, then snapshot under `ref_queue_mutex` (see the `shutting_down` member comment): this
    /// ordering is what makes the check in `appendRefOps` -- performed inside the SAME critical section
    /// as its `pending.push_back` -- race-free against the snapshot below, for both an already-cached
    /// table and one whose very first touch races this call.
    shutting_down.store(true, std::memory_order_release);

    std::vector<std::shared_ptr<RefTableRuntime>> runtimes;
    {
        std::lock_guard<std::mutex> g(ref_queue_mutex);
        runtimes.reserve(ref_name_slots.size());
        for (const auto & [_, slot] : ref_name_slots)
            if (slot.current)
                runtimes.push_back(slot.current);
    }

    /// Wait for every table's queue to go idle (no pending item, no active leader), bounded overall by
    /// `wait_budget_ms` -- `cv.wait_until` slices against one shared deadline, never a sleep. All the
    /// runtimes share the one `ref_queue_mutex` that guards `pending`/`leader_active` (see the
    /// `RefTableRuntime` field comments), so a single `lk` covers every table in the loop below; each
    /// table's OWN `cv` is what its leader/appendRefOps notifies on a state change, so the wait must
    /// target that specific `cv`, one table at a time.
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(wait_budget_ms);
    bool timed_out = false;
    {
        std::unique_lock<std::mutex> lk(ref_queue_mutex);
        for (const auto & rt : runtimes)
        {
            while (!(rt->pending.empty() && !rt->leader_active))
            {
                if (rt->cv.wait_until(lk, deadline) == std::cv_status::timeout
                    && !(rt->pending.empty() && !rt->leader_active))
                {
                    timed_out = true;
                    break;
                }
            }
            if (timed_out)
                break;
        }
    }

    /// A queue going idle does NOT by itself prove no PUT is in flight: a wedge is recorded (under
    /// `state_mutex`) strictly BEFORE the wedged item's caller is completed and the leader bookkeeping
    /// reset (see `flushRefBatch`'s `Unresolved` case), so this check -- performed AFTER the wait above
    /// -- observes it whenever the queue-idle wait itself raced a wedge. Every table is checked
    /// regardless of `timed_out`, purely for a complete diagnostic; the return value already fails
    /// closed on either condition alone.
    bool any_wedge = false;
    for (const auto & rt : runtimes)
    {
        std::lock_guard<std::mutex> lock(rt->state_mutex);
        if (rt->lane_state == RefLaneState::Writing || rt->lane_state == RefLaneState::Wedged)
            any_wedge = true;
    }

    return !timed_out && !any_wedge;
}


RefTxnId CasRefLedger::appendRefOps(const RootNamespace & ns, MutationScope scope,
                             std::function<std::vector<RefOp>(const RefTableState &)> build_ops,
                             RootMutationOrigin origin, RootMutationKind kind,
                             bool skip_stale_precommit_sweep)
{
    if (kind == RootMutationKind::DropNamespace)
        throw Exception(ErrorCodes::CORRUPTED_DATA,
            "CAS namespace '{}': the generic append surface cannot acquire removal ownership; "
            "use the exact `dropNamespace` lifecycle operation",
            ns.string());
    const auto rt = acquireMutableRefTableRuntime(ns);
    if (append_after_runtime_capture_hook_for_test)
        append_after_runtime_capture_hook_for_test();
    return appendRefOpsOnRuntime(
        ns, rt, std::move(scope), std::move(build_ops), origin, kind, skip_stale_precommit_sweep,
        /*terminal_removal_authorized=*/false);
}


namespace
{

/// The row may still be `Live` here, so the text must not claim `Removing`.
[[noreturn]] void throwRemovalWriteMayLand(const RootNamespace & ns)
{
    throwCasWriteRetryLater(fmt::format(
        "CAS namespace '{}': a removal write may still land, so positive ref mutation admission is "
        "closed until a retried removal completes or the disk is remounted; retry later", ns.string()));
}

/// True whether the removal is still running, done and waiting for GC to erase the row, or stuck; the
/// remedy for a stuck one depends on the operation that started it, so the text names none.
[[noreturn]] void throwRemovingObserved(const RootNamespace & ns)
{
    throwCasWriteRetryLater(fmt::format(
        "CAS namespace '{}' is Removing in the catalog: writes are refused and reads see no data until its removal "
        "completes and GC erases the row; retry later, and if it stays Removing see the CAS troubleshooting page",
        ns.string()));
}

/// Flags read under `ref_queue_mutex`.
[[noreturn]] void throwPositiveAdmissionClosed(const RootNamespace & ns, bool removing_observed, bool write_may_land)
{
    if (write_may_land)
        throwRemovalWriteMayLand(ns);
    if (removing_observed)
        throwRemovingObserved(ns);
    throwCasWriteRetryLater(fmt::format(
        "CAS namespace '{}' is Removing: positive ref mutation admission is closed while its "
        "terminal fold and catalog removal complete; retry later", ns.string()));
}

}

RefTxnId CasRefLedger::appendRefOpsOnRuntime(
    const RootNamespace & ns, const std::shared_ptr<RefTableRuntime> & rt, MutationScope scope,
    std::function<std::vector<RefOp>(const RefTableState &)> build_ops,
    RootMutationOrigin origin, RootMutationKind kind, bool skip_stale_precommit_sweep,
    bool terminal_removal_authorized)
{
    const auto refuse_if_removing = [&]
    {
        std::lock_guard<std::mutex> lock(ref_queue_mutex);
        if (rt->removal_admission_closed && !terminal_removal_authorized)
            throwPositiveAdmissionClosed(ns, rt->removing_observed, rt->removal_write_may_land);
    };
    /// Check before recovery/maintenance so a known-Removing runtime cannot spend an object-store
    /// mutation on behalf of an operation it is already required to refuse.
    refuse_if_removing();
    /// Hoisted here (rather than left to `flushRefBatch`'s own idempotent call) so both
    /// triggers below run on the CALLING thread, strictly BEFORE this call enqueues its own item or
    /// becomes a queue leader -- `maybeSweepStalePrecommits`'s own nested `appendRefOps` calls are
    /// therefore always a fresh top-level invocation, never nested inside a leader's flush stack
    /// (which would deadlock the leader against itself).
    if (ensureRefTableRecovered(ns, *rt) == RefRecoveryOutcome::LifeRemoved)
        throwCasWriteRetryLater(recoveredLifeRemovedMessage(ns, rt->life));
    if (!skip_stale_precommit_sweep)
        maybeSweepStalePrecommits(ns, rt);
    maybeScheduleSnapshotPublish(ns, rt);

    auto item = std::make_shared<RefMutationItem>();
    item->scope = std::move(scope);
    item->build_ops = std::move(build_ops);
    item->origin = origin;
    item->kind = kind;
    item->terminal_removal_authorized = terminal_removal_authorized;

    const auto enqueued_at = std::chrono::steady_clock::now();
    std::unique_lock<std::mutex> lk(ref_queue_mutex);
    /// Refuse admission once a clean-release drain has begun (`drainRefLanesForShutdown`).
    /// Checked in the SAME critical section as the `pending.push_back` below -- the pairing that makes
    /// this race-free against the drain's snapshot-and-wait (see the `shutting_down` member comment).
    if (shutting_down.load(std::memory_order_acquire))
        throwCasWriteRetryLater(fmt::format(
            "CAS store is shutting down — refusing to append ref-log transactions for server_root '{}'",
            config.server_root_id));
    if (rt->removal_admission_closed && !terminal_removal_authorized)
        throwPositiveAdmissionClosed(ns, rt->removing_observed, rt->removal_write_may_land);
    rt->pending.push_back(item);

    while (!item->done)
    {
        if (!rt->leader_active)
        {
            /// The set of items THIS leader is responsible for: its own enqueued `item` plus every item a
            /// flush carves out of `pending` (recorded by `flushRefBatch` as it carves). Whatever the
            /// leader loop does below -- return normally, or throw at ANY point including BEFORE it ever
            /// carves -- every one of these items must leave here `done` (its waiter woken), never left
            /// stranded in `pending` for a future leader to carve after this caller's stack (and its
            /// `build_ops` closure) is gone: a use-after-free that concurrent per-part commit makes
            /// immediate. `completeOwnedItemsAndReleaseLeadership` enforces that on EVERY exit and folds in
            /// the `leader_active` release the old catch used to own. Items NOT owned by this leader (other
            /// callers' still-queued items) are untouched -- they stay validly owned by their blocked
            /// callers.
            ///
            /// Build the responsibility set (its own `item`) BEFORE publishing the baton, so becoming
            /// leader contains NO throwing operation once `leader_active` is set: the only allocation is
            /// this first `push_back`, done here while still holding `lk` and NOT yet leader. If it throws
            /// (a `bad_alloc` at the pre-tenure point), the baton is never
            /// taken -- but `item` is already in `pending` (pushed above), so it must be un-enqueued before
            /// propagating, else a future leader would carve an item whose `build_ops` closure died with
            /// this unwinding caller (the same use-after-free the exit guard prevents post-publication).
            /// Publishing the baton and reaching the exit guard is then a pure no-throw sequence.
            std::vector<std::shared_ptr<RefMutationItem>> owned_items;
            try
            {
                if (ref_pre_tenure_hook_for_test)
                    ref_pre_tenure_hook_for_test();
                owned_items.push_back(item);
            }
            catch (...)
            {
                std::erase(rt->pending, item);
                throw;
            }

            rt->leader_active = true;
            lk.unlock();
            std::exception_ptr flush_exception;
            try
            {
                runRefQueueLeader(ns, rt, item, owned_items);
            }
            catch (...)
            {
                flush_exception = std::current_exception();
            }
            /// Single exit authority (normal AND exceptional): complete every still-incomplete owned
            /// item with `flush_exception` (nullptr on the normal path -> a fail-closed LOGICAL_ERROR)
            /// and release leadership. This does NOT rethrow. Under chunked flush the leader's OWN item
            /// may already have succeeded in an earlier committed chunk, and a later exception -- from a
            /// subsequent chunk, the reseed, or chunk-N processing -- must NOT be handed to this caller
            /// whose mutation is already durable (tenure exception containment, spec §3): the guard
            /// leaves such an item `done` with no error, and the loop re-check + tail below return its
            /// `committed_id`. An item that genuinely failed carries `item->error` and the tail rethrows
            /// it, exactly as the old unconditional rethrow did for the single-chunk case.
            completeOwnedItemsAndReleaseLeadership(ns, rt, owned_items, flush_exception);
            lk.lock();
        }
        else
        {
            rt->cv.wait(lk);
        }
    }
    lk.unlock();

    ProfileEvents::increment(ProfileEvents::CASRefQueueWaitMicroseconds,
        std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now() - enqueued_at).count());
    if (item->error)
        std::rethrow_exception(item->error);
    return item->committed_id;
}


void CasRefLedger::runRefQueueLeader(const RootNamespace & ns, const std::shared_ptr<RefTableRuntime> & rt,
                              const std::shared_ptr<RefMutationItem> & own,
                              std::vector<std::shared_ptr<RefMutationItem>> & owned_items)
{
    /// Fairness baton pass: serve flushes only until the caller's OWN item is done, then hand off to a
    /// woken waiter.
    while (true)
    {
        {
            std::lock_guard<std::mutex> g(ref_queue_mutex);
            if (own->done)
                return;
        }
        flushRefBatch(ns, rt, owned_items);
    }
}

void CasRefLedger::completeOwnedItemsAndReleaseLeadership(
    const RootNamespace & ns, const std::shared_ptr<RefTableRuntime> & rt,
    const std::vector<std::shared_ptr<RefMutationItem>> & owned_items,
    std::exception_ptr flush_exception)
{
    std::lock_guard<std::mutex> g(ref_queue_mutex);
    for (const auto & owned : owned_items)
    {
        if (!owned->done)
        {
            owned->error = flush_exception
                ? flush_exception
                : std::make_exception_ptr(Exception(ErrorCodes::LOGICAL_ERROR,
                    "CAS ref-log append for namespace '{}': the append-lane leader exited without "
                    "completing an owned queue item -- failing it closed rather than leaving it stranded "
                    "in the pending queue for a future leader to carve", ns.string()));
            owned->done = true;
        }
        /// Never leave an owned item in `pending`: a stranded item would be carved by a future leader
        /// which would then invoke its (now dangling) `build_ops` closure -- the use-after-free this
        /// guard exists to prevent. Carved items were already popped during the carve, so this is a
        /// no-op for them; it only matters for an item the leader owned but never got to carve.
        std::erase(rt->pending, owned);
    }
    /// The tenure is over: every carved item is completed (above, or by its chunk's commit) and its
    /// effect is either installed, or its failure is recorded by the lane state (`Wedged` for an
    /// ambiguous `PUT`, `NeedsRecovery` for a durable-but-not-installed chunk), so the confirm no
    /// longer needs to see it.
    rt->carved.clear();
    rt->leader_active = false;
    rt->cv.notify_all();
}

void CasRefLedger::requireRecoveryForTakenTombstoneSlot(
    RefTableRuntime & rt, const RootNamespace & ns, const RefTxnId & id) noexcept
{
    const bool entering = rt.lane_state != RefLaneState::NeedsRecovery;
    rt.lane_state = RefLaneState::NeedsRecovery;
    if (!entering)
        return;
    ProfileEvents::increment(ProfileEvents::CASRefNeedsRecovery);
    try
    {
        LOG_WARNING(getLogger("CasPool"),
            "CAS ref table '{}' NEEDS RECOVERY: another record holds slot {}-{} of this lane's tombstone, so the "
            "tombstone did not land. New writes, snapshots, and confirmations are fenced until recovery adopts "
            "that record.",
            ns.string(), id.writer_epoch, id.ref_sequence);
    }
    catch (...)   // NOLINT(bugprone-empty-catch)
    {
        /// The state transition above is the safety mechanism; logging must not replace it.
    }
}

void CasRefLedger::requireRecovery(RefTableRuntime & rt, const RootNamespace & ns, std::string_view region) noexcept
{
    const bool entering = rt.lane_state != RefLaneState::NeedsRecovery;
    rt.lane_state = RefLaneState::NeedsRecovery;
    if (!entering)
        return;
    ProfileEvents::increment(ProfileEvents::CASRefNeedsRecovery);
    try
    {
        LOG_ERROR(getLogger("CasPool"),
            "CAS ref table '{}' NEEDS RECOVERY at {}: a transaction is known durable but could not be "
            "installed in this cached table. New writes, snapshots, and confirmations are fenced until "
            "recovery replays the durable log.",
            ns.string(), region);
    }
    catch (...)   // NOLINT(bugprone-empty-catch)
    {
        /// The state transition above is the safety mechanism; logging must not replace the original
        /// post-durable exception.
    }
}

CasRefLedger::WedgeResolutionResult
CasRefLedger::resolveWedgeOnce(const RootNamespace & ns, const std::shared_ptr<RefTableRuntime> & rt)
{
    WedgeResolutionResult result;

    /// WHY the lane is not proceeding, decided under `state_mutex` and rendered into a message AFTER it
    /// is released. Building an exception is `fmt::format` plus a stack-trace capture, neither of which
    /// belongs under a lock that readers and the snapshot publisher contend for.
    enum class Reason : uint8_t
    {
        None,
        FenceMoved,             /// the mount incarnation moved since this attempt was admitted
        CatalogLifeRetired,     /// exact catalog retirement detached this immutable life
        Superseded,             /// a self-remount detached this runtime
        WedgeReplaced,          /// the result belongs to a wedge that is no longer installed
        RefusedPreAttempt,      /// the bounded attempt sent nothing
        ResolveFoundNothing,    /// it sent an attempt and the follow-up read came up empty
        StaleState,             /// the table advanced under a proven-durable object we cannot install
        TombstoneSlotTaken,     /// another record holds the wedged tombstone's slot; recovery adopts it
    };
    Reason reason = Reason::None;
    std::optional<RefLaneState> invalid_lane_state;

    /// ---- Read the wedge and prepare EVERYTHING that can throw, before any I/O (spec §A1, site 2) ----
    RefAppendAttempt wedge;
    std::optional<RefTableState> candidate;
    RefTxnId candidate_base_id;
    {
        std::lock_guard lock(rt->state_mutex);
        if (rt->lane_state == RefLaneState::Ready)
            return result;   /// NoWedge -- the ordinary flush pays nothing for any of this
        if (rt->lane_state != RefLaneState::Wedged || !rt->append_attempt)
        {
            invalid_lane_state = rt->lane_state;
        }
        else
        {
            wedge = *rt->append_attempt;
            candidate.emplace(rt->state);
            candidate_base_id = rt->state.getGreatestApplied();
        }
    }

    if (invalid_lane_state)
    {
        result.kind = WedgeResolution::StillWedged;
        if (*invalid_lane_state == RefLaneState::NeedsRecovery)
        {
            result.survivor_error = makeCasWriteRetryLaterExceptionPtr(fmt::format(
                "CAS ref-log append for namespace '{}': lane recovery is still in progress; an earlier "
                "transaction may be durable and was not acknowledged: {}",
                ns.string(), durable_unconfirmed_tail));
        }
        else
        {
            result.survivor_error = std::make_exception_ptr(Exception(
                ErrorCodes::INVALID_STATE,
                "CAS ref-log append for namespace '{}': terminal lane state {} does not permit wedge "
                "resolution; this write was not attempted",
                ns.string(), static_cast<uint8_t>(*invalid_lane_state)));
        }
        return result;
    }

    /// Decode and apply BEFORE the I/O. The wedge carries the encoded body, so this costs no extra
    /// round trip -- only the decode and the overlay build, both of which can throw (allocation) and
    /// both of which MUST NOT run after the object is proven durable. A throw here happens while the
    /// outcome is still unknown and the wedge is still set, i.e. it is indistinguishable from "the
    /// resolution has not been attempted yet": the lane stays wedged and a later flush retries the whole
    /// thing. It propagates to `appendRefOps`' catch, which completes every survivor.
    ///
    /// The candidate is deliberately NOT cached in the wedge across attempts: a wedge can live until a
    /// remount, and retaining a full state copy for that long is a real memory cost on a path that is
    /// rare by construction. Recomputing it per attempt is the cheaper trade.
    const RefLogTxn wedged_txn = decodeRefLogTxn(
        openObject(FormatId::RefLog, wedge.bytes), ns.string(), wedge.txn_id);
    applyRefLogTxn(*candidate, wedged_txn);
    /// A lane tombstone, as opposed to an ordinary append: it loses its slot conclusively only to a
    /// tombstone naming a later epoch, and any other record there is history recovery adopts.
    const std::optional<RefEpochTransition> wedged_transition = refEpochTransitionOf(wedged_txn);

    /// ---- ONE bounded attempt, admitted under the wedge's ORIGINAL generation ----
    /// Never the CURRENT generation: a retry that "passes" because the mount was re-armed under a new
    /// lease incarnation is a write from an incarnation that never admitted this transaction. Refusing
    /// pre-attempt leaves the key provably untouched, which is the only state a later recovery can
    /// reason about.
    CasOperation op = mount_requests.resume(wedge.admitted_fence_generation, [&rt]
    {
        return !rt->catalog_life_invalidated.load(std::memory_order_acquire)
            && !rt->superseded_by_remount.load(std::memory_order_acquire);
    });

    std::optional<WriteResult> attempted;
    try
    {
        if (wedge_before_slot_occupy_hook_for_test)
            wedge_before_slot_occupy_hook_for_test();
        attempted = op.create(wedge.key, wedge.bytes, Retry::once());
    }
    catch (...)
    {
        /// `ambiguous-then-definite`, the model-proven control. A deterministic local failure surfaces
        /// unchanged, and it proves nothing whatsoever about the EARLIER ambiguous attempt, which may
        /// still be in flight or may already have landed. So the lane stays wedged: unwedging here is
        /// exactly how an acked-then-lost transaction gets written around. The id is not consumed
        /// either, so the next attempt re-derives the SAME one.
        result.kind = WedgeResolution::StillWedged;
        result.survivor_error = makeCasWriteRetryLaterExceptionPtr(fmt::format(
            "CAS ref-log append for namespace '{}': the bounded retry of wedged txn {}-{} failed ({}), "
            "which says nothing about the earlier ambiguous attempt — the lane stays wedged; wedged txn {}-{} "
            "outcome is unknown: {}",
            ns.string(), wedge.txn_id.writer_epoch, wedge.txn_id.ref_sequence,
            getCurrentExceptionMessage(/*with_stacktrace*/ false),
            wedge.txn_id.writer_epoch, wedge.txn_id.ref_sequence, outcome_unknown_tail));
        return result;
    }

    /// A store refusal is the same control as the throw above: it proves only its OWN attempt never
    /// applied, and the earlier ambiguous one is what the wedge is about.
    if (const auto * refused = std::get_if<Refused>(&*attempted))
    {
        result.kind = WedgeResolution::StillWedged;
        result.survivor_error = makeCasWriteRetryLaterExceptionPtr(fmt::format(
            "CAS ref-log append for namespace '{}': the bounded retry of wedged txn {}-{} was definitively "
            "refused ({}), which says nothing about the earlier ambiguous attempt — the lane stays wedged; "
            "wedged txn {}-{} outcome is unknown: {}",
            ns.string(), wedge.txn_id.writer_epoch, wedge.txn_id.ref_sequence, refused->message,
            wedge.txn_id.writer_epoch, wedge.txn_id.ref_sequence, outcome_unknown_tail));
        return result;
    }

    /// ---- Classify the occupant OFF the lock: pure, and the decode allocates ----
    /// The three-way `mine | successor's seal | foreign` adjudication is the CALLER's job by
    /// construction (the engine never compares bytes for meaning), and "mine" means BYTE EQUALITY --
    /// never a shape or generation match, which is the aliasing the phase-0 model rejected.
    const auto * conflict = std::get_if<Conflict>(&*attempted);
    const auto * occupant_object = conflict ? std::get_if<Object>(&conflict->seen) : nullptr;
    const auto * gave_up = std::get_if<GaveUp>(&*attempted);
    /// A conflict whose settling read observed nothing is as unresolved as a give-up: the key holds
    /// something this call could not name, so nothing may be adopted or unwedged from it.
    const bool unresolved = gave_up || (conflict && !occupant_object);
    const ClassifiedOccupant occupant = occupant_object
        ? classifyRefLogOccupant(ns, wedge.txn_id, occupant_object->bytes, wedge.bytes)
        : ClassifiedOccupant{Occupant::NotOccupied, std::nullopt};
    const bool exact_attempt_is_durable
        = std::holds_alternative<Committed>(*attempted) || occupant.kind == Occupant::Ours;
    /// Caller holds `state_mutex`. Keeping the identity predicate in one place is part of the safety
    /// rule: there must be one notion of "same attempt".
    const auto same_wedge_under_lock = [&]
    {
        return rt->append_attempt
            && rt->lane_state == RefLaneState::Wedged
            && rt->append_attempt->txn_id == wedge.txn_id
            && rt->append_attempt->bytes == wedge.bytes
            && rt->append_attempt->admitted_fence_generation == wedge.admitted_fence_generation;
    };

    /// A successor's tombstone at our key is conclusive evidence that OUR transaction did not land. For a
    /// wedged tombstone only one naming a later epoch than ours is: this writer is then deposed.
    const bool closed_by_successor_seal = occupant.kind == Occupant::Tombstone
        && (!wedged_transition || occupant.transition->next_epoch > wedged_transition->next_epoch);
    /// An occupant that does not decode is no record recovery could adopt; it takes the `Foreign` arm.
    const bool tombstone_slot_taken = wedged_transition && occupant_object && occupant.decoded
        && !exact_attempt_is_durable && !closed_by_successor_seal;

    /// ---- POST-I/O RECHECK, then act, in ONE hold of `state_mutex` ----
    /// Everything above ran on an I/O result that took an unbounded amount of time to come back. Before
    /// ANY consequence follows from it -- adopting, acknowledging, unwedging, failing the survivors --
    /// this runtime must still be the one the attempt was admitted for. Two independent things can have
    /// made it not: the mount fence moved (a loss, or a re-arm under a fresh lease incarnation), or the
    /// wedge itself was replaced. Both are checked; neither implies the other.
    {
        std::lock_guard lock(rt->state_mutex);

        /// The generation this attempt was admitted under, presented back. A moved incarnation is a
        /// verdict here, not an exception: the caller's retry classification keys on the retry-later
        /// error class and a routine lease blip must not reach it as a hard failure. Nothing is
        /// installed and nothing is unwedged either way, which is the whole meaning of INERT here.
        const bool fence_moved = !op.admitted();

        /// The remount half of the same question, checked separately because the two are independent
        /// facts even though today's ordering makes one imply the other: `quiesceRefTablesForRemount`
        /// detaches this runtime BEFORE the fence is re-armed, so a detached runtime always has a moved
        /// generation and the check above would already have caught it. Relying on that ordering
        /// silently is how a future edit to the remount sequence turns into a stale install.
        const bool superseded = rt->superseded_by_remount.load(std::memory_order_acquire);
        const bool catalog_life_retired = rt->catalog_life_invalidated.load(std::memory_order_acquire);

        /// All three components of the identity, because none of them alone identifies the attempt: two
        /// attempts of one table can share an id and a generation and describe DIFFERENT bytes, and
        /// installing one attempt's candidate because the other's key resolved is the acked-then-lost
        /// class itself. One leader per table makes this unreachable today; it is checked, not assumed,
        /// because the cost is a comparison and the failure mode is silent data loss.
        const bool same_wedge = same_wedge_under_lock();

        /// A durable attempt is history; a lease lost under it leaves it to recovery.
        const bool installable = exact_attempt_is_durable && !closed_by_successor_seal;
        if ((fence_moved || superseded) && same_wedge && installable)
        {
            requireRecovery(*rt, ns, "wedged attempt resolved after its fence moved");
            reason = Reason::StaleState;
            result.kind = WedgeResolution::StillWedged;
        }
        else if (fence_moved || catalog_life_retired || superseded || !same_wedge)
        {
            reason = fence_moved ? Reason::FenceMoved
                : (catalog_life_retired ? Reason::CatalogLifeRetired
                : (superseded ? Reason::Superseded : Reason::WedgeReplaced));
            result.kind = WedgeResolution::StillWedged;
        }
        else if (unresolved)
        {
            /// Still uncertain. Do NOT clear the wedge: it describes an object that may well be durable,
            /// and clearing it on a failed read is the one thing this path must never do. There is no
            /// deadline reset and no background loop -- the next caller of this namespace, or a remount,
            /// retries. That is ACCEPTED behaviour: a permanently quiet wedged namespace
            /// waits, which costs nothing, because the wedged operation was never acknowledged.
            reason = gave_up && !gave_up->sent_any
                ? Reason::RefusedPreAttempt : Reason::ResolveFoundNothing;
            result.kind = WedgeResolution::StillWedged;
        }
        else if (closed_by_successor_seal)
        {
            /// THE conclusive rejection: the ref-log key holds a successor's tombstone, so our bytes never
            /// landed and never can. The operation was never acknowledged, so nothing is lost by failing it
            /// permanently -- and it must be permanent, not "retry later": no later attempt in this epoch
            /// can ever succeed.
            rt->append_attempt.reset();
            rt->lane_state = RefLaneState::Closed;
            result.kind = WedgeResolution::Rejected;
        }
        else if (tombstone_slot_taken)
        {
            /// A late record of the chain's epoch, or an earlier build's recovery tombstone. The attempt is
            /// cleared so recovery does not adjudicate it as this writer's retained bytes; its walk adopts
            /// the occupant and the next flush writes the tombstone after it.
            rt->append_attempt.reset();
            requireRecoveryForTakenTombstoneSlot(*rt, ns, wedge.txn_id);
            reason = Reason::TombstoneSlotTaken;
            result.kind = WedgeResolution::StillWedged;
        }
        else if (occupant.kind == Occupant::Foreign)
        {
            /// Impossible under mount-lease exclusivity. The terminal state carries the verdict; no
            /// uncertain attempt remains to be retried.
            rt->append_attempt.reset();
            rt->lane_state = RefLaneState::Faulted;
            result.kind = WedgeResolution::Corrupted;
        }
        else if (!(rt->state.getGreatestApplied() == candidate_base_id))
        {
            /// ADOPTION, refused. Only this leader mutates `rt->state`, and the attempt above ran
            /// without the lock, so this compares the state against the snapshot the candidate was built
            /// from. A debug build also `chassert`s it inside the region below; this is the RELEASE-mode
            /// counterpart, because the window it guards is a full network round trip and a silent swap
            /// would discard whatever advanced the table.
            ///
            /// The object is proven durable and this runtime cannot record it. No later id may be
            /// allocated from this cache; recovery is the only legal successor.
            requireRecovery(*rt, ns, "wedge-resolution install");
            reason = Reason::StaleState;
            result.kind = WedgeResolution::StillWedged;
        }
        else
        {
            /// ---- ADOPTION: `Created`, or `Occupied` with our own bytes ----
            /// Receives the resolved wedge so it is destroyed OUTSIDE the region: clearing `rt->append_attempt`
            /// in place would free its two `String` bodies there, and the region's contract is that it
            /// touches no allocator at all.
            std::optional<RefAppendAttempt> displaced_wedge;
            static_assert(std::is_nothrow_swappable_v<std::optional<RefAppendAttempt>>,
                "the wedge hand-off below must be non-throwing: it runs after the wedged object is proven "
                "durable, where a throw would re-apply the transaction on the next resolution");
            /// One of the two post-durable install regions (spec §A2; the other is `commitRefChunk`'s
            /// own, further below, sharing this same probe). The `catch` cannot fire while §A1 holds --
            /// the body below allocates nothing -- and is what makes a violation of §A1 VISIBLE rather
            /// than silent: the attempt proved the object durable, so an install that does not complete
            /// leaves this table's cached state missing it. It rethrows unchanged, so the lane's error
            /// handling is unchanged; the explicit lane state makes the missing install visible.
            try
            {
                DENY_ALLOCATIONS_IN_SCOPE;
                /// The negative control (`setInstallRegionProbeForTest`), fired with the guard already
                /// armed, exactly as in `commitRefChunk`'s region.
                if (install_region_probe_for_test)
                    install_region_probe_for_test();
                /// The debug-build twin of the refusal above. `chassert` stringifies its condition, so
                /// it reads a short local rather than the comparison itself: a long condition would heap
                /// allocate ON FAILURE inside the very region that must not allocate.
                chassert(same_wedge);
                /// The install, allocation-free by construction: a member-wise swap of pointers and
                /// PODs, two atomic increments, and a second swap of pointers. The object is durable, so
                /// the transaction MUST be recorded -- and recording it MUST be inseparable from clearing
                /// the wedge, or a failure between them leaves the transaction applied with the wedge
                /// still set and the next resolution re-applies it. A wedge-resolved transaction is a
                /// commit like any other: it joins the applied-above-newest-snapshot tail counters
                /// exactly as the ordinary commit arm's does, or the snapshot-publish threshold and the
                /// resident-weight estimate undercount by one transaction per resolved wedge until the
                /// next recovery reseeds.
                rt->state.swap(*candidate);
                rt->tail_count_since_snapshot.fetch_add(1, std::memory_order_relaxed);
                rt->tail_bytes_since_snapshot.fetch_add(wedge.bytes.size(), std::memory_order_relaxed);
                rt->append_attempt.swap(displaced_wedge);
                rt->lane_state = RefLaneState::Ready;
            }
            catch (...)
            {
                requireRecovery(*rt, ns, "wedge-resolution install");
                throw;
            }
            /// `candidate` now holds the DISPLACED state, which still shares the COW bases `rt->state`
            /// uses; destroying it here restores unique base ownership so the fold below keeps its
            /// O(overlay) in-place path instead of rebuilding the whole base. Both `reset`s only destroy:
            /// they allocate nothing and cannot throw.
            candidate.reset();
            displaced_wedge.reset();
            /// Fold the just-installed overlay back into the base right here, exactly as the ordinary
            /// commit arm does at its install point, so `rt->state` returns to "base + empty overlay" and
            /// the next flush's trial copies stay cheap. Cheap: no scratch copy shares the base at this
            /// point in the flush (`working` is not taken until later), so this is the O(overlay) in-place
            /// fold. Coherent-on-throw (see `CasRefCowMap.cpp`), and SWALLOWING, symmetrically with the
            /// ordinary commit arm: the transaction is durable, installed and unwedged before this runs,
            /// so a mid-fold allocation failure merely defers the fold to the next flush -- it must not
            /// unwind past a completed install.
            try
            {
                rt->state.materializeCommitted();
            }
            catch (...)
            {
                tryLogCurrentException(getLogger("CasPool"), fmt::format(
                    "CAS ref-log append for namespace '{}': wedged txn {}-{} resolved durable and was "
                    "installed, but the post-install overlay fold failed and was retained coherently for "
                    "the next flush",
                    ns.string(), wedge.txn_id.writer_epoch, wedge.txn_id.ref_sequence));
            }
            result.kind = WedgeResolution::Adopted;
        }
    }

    /// ---- Everything that allocates or reacts, now that `state_mutex` is released ----
    switch (result.kind)
    {
        case WedgeResolution::Adopted:
            ProfileEvents::increment(ProfileEvents::CASRefAppendUnwedged);
            if (wedged_transition)
                ProfileEvents::increment(ProfileEvents::CASRefAppendTombstoneWritten);
            break;
        case WedgeResolution::Rejected:
            logLaneStopped(rt->life, RefLaneState::Closed, wedge.txn_id.writer_epoch, wedge.key,
                "a successor's tombstone holds the slot of the wedged transaction");
            /// A wedged tombstone sits in the chain epoch's slot and names this mount's epoch.
            result.survivor_error = std::make_exception_ptr(Exception(ErrorCodes::INVALID_STATE,
                "CAS ref-log append for namespace '{}': writer epoch {} was CLOSED by a successor's tombstone "
                "at {}-{}, which conclusively rejects the wedged transaction (it was never "
                "acknowledged): nothing is durable, so a retry cannot duplicate this transaction. This mount's "
                "append lane resumes only under a later epoch",
                ns.string(), wedged_transition ? wedged_transition->next_epoch : wedge.txn_id.writer_epoch,
                wedge.txn_id.writer_epoch, wedge.txn_id.ref_sequence));
            break;
        case WedgeResolution::Corrupted:
            logLaneStopped(rt->life, RefLaneState::Faulted, wedge.txn_id.writer_epoch, wedge.key,
                "a foreign object holds the slot of the wedged transaction");
            on_impossible_interference(wedge.key,
                fmt::format("ref-log wedge resolution for namespace '{}' txn {}-{} observed a foreign object "
                    "at the wedged slot: neither this attempt's own bytes nor a tombstone of this namespace",
                    ns.string(), wedge.txn_id.writer_epoch, wedge.txn_id.ref_sequence),
                ns.string());
            result.survivor_error = std::make_exception_ptr(Exception(ErrorCodes::CORRUPTED_DATA,
                "CAS ref-log append for namespace '{}': impossible foreign interference observed at the "
                "wedged slot '{}' — the mount is fenced closed and a remount is scheduled; the lane is "
                "left faulted for inspection; wedged txn {}-{} outcome is unknown: {}. See the anomaly "
                "diagnostics log",
                ns.string(), wedge.key, wedge.txn_id.writer_epoch, wedge.txn_id.ref_sequence,
                outcome_unknown_tail));
            break;
        case WedgeResolution::StillWedged:
        {
            const String txn = fmt::format("{}-{}", wedge.txn_id.writer_epoch, wedge.txn_id.ref_sequence);
            String why;
            switch (reason)
            {
                case Reason::FenceMoved:
                    why = fmt::format(
                        "CAS ref-log append for namespace '{}': the resolution of txn {} returned under a "
                        "DIFFERENT mount incarnation than the one that admitted it — the result is inert "
                        "and the lane keeps its wedge for whoever recovers it under the live incarnation",
                        ns.string(), txn);
                    break;
                case Reason::CatalogLifeRetired:
                    why = fmt::format(
                        "CAS ref-log append for namespace '{}': catalog retirement detached life {} "
                        "before the bounded retry could be adopted — the result is inert and the "
                        "successor life is untouched",
                        ns.string(), renderIncarnation(rt->life.incarnation));
                    break;
                case Reason::Superseded:
                    why = fmt::format(
                        "CAS ref-log append for namespace '{}': the resolution of txn {} returned for a "
                        "table that a self-remount had already detached — the result is inert; the fresh "
                        "incarnation re-derives this table from the durable log",
                        ns.string(), txn);
                    break;
                case Reason::WedgeReplaced:
                    why = fmt::format(
                        "CAS ref-log append for namespace '{}': the resolution of txn {} returned for a "
                        "wedge that is no longer installed — the result is inert and the lane keeps "
                        "whatever wedge it has",
                        ns.string(), txn);
                    break;
                case Reason::RefusedPreAttempt:
                    /// The admission-generation half of a pre-attempt refusal never reaches this message:
                    /// the recheck above presents the same generation and reports `FenceMoved` first.
                    /// What is left are the two causes that leave the generation intact.
                    why = fmt::format(
                        "CAS ref-log append for namespace '{}': the bounded retry of wedged txn {} was "
                        "refused BEFORE any request was sent — the mount lease is not healthy enough to "
                        "start a write, or the operation deadline is exhausted — so the slot '{}' is "
                        "provably untouched by this attempt and the lane stays wedged",
                        ns.string(), txn, wedge.key);
                    break;
                case Reason::ResolveFoundNothing:
                    /// The two observations behind this one verdict are named rather than collapsed into
                    /// a generic "unresolved": they call for very different investigations.
                    why = fmt::format(
                        "CAS ref-log append for namespace '{}': the bounded retry of wedged txn {} was sent "
                        "and the resolve read found NOTHING at slot '{}' — either the read itself failed, "
                        "or the occupant that rejected the create was DELETED under a live epoch, which is "
                        "a GC invariant alarm rather than routine contention. The lane stays wedged until "
                        "the SAME slot resolves durable or a conclusive rejection is observed",
                        ns.string(), txn, wedge.key);
                    break;
                case Reason::StaleState:
                    why = fmt::format(
                        "CAS ref-log append for namespace '{}': wedged txn {} is DURABLE but this table "
                        "advanced under it during the resolution, so it cannot be installed without "
                        "discarding that advance — the lane NEEDS RECOVERY and refuses later writes until "
                        "replay re-derives the cache from the durable log",
                        ns.string(), txn);
                    break;
                case Reason::TombstoneSlotTaken:
                    why = fmt::format(
                        "CAS ref-log append for namespace '{}': the slot of wedged tombstone {} holds another "
                        "record, so the tombstone did not land; the lane NEEDS RECOVERY, which adopts that "
                        "record, and this write was not attempted",
                        ns.string(), txn);
                    break;
                case Reason::None:
                    why = fmt::format("CAS ref-log append for namespace '{}': txn {} stays wedged",
                                      ns.string(), txn);
                    break;
            }
            if (reason == Reason::StaleState)
                why += fmt::format("; wedged txn {} is durable but was not acknowledged: {}", txn, durable_unconfirmed_tail);
            else if (reason != Reason::TombstoneSlotTaken)
                why += fmt::format("; wedged txn {} outcome is unknown: {}", txn, outcome_unknown_tail);
            result.survivor_error = makeCasWriteRetryLaterExceptionPtr(why);
            break;
        }
        case WedgeResolution::NoWedge:
            break;
    }
    return result;
}

namespace
{
/// The first ref name `op` mutates that `scope` does not name, or nullptr when every name it
/// carries is named by `scope` or it carries none (`NamespaceBirth`, `RemoveNamespace` and `EpochSeal` are
/// namespace-level and name no ref). Naming no ref is not the same as moving no ref: a
/// `RemoveNamespace` moves every row of the namespace, so its caller rejects it under a `Ref` scope
/// on its own rather than reading an answer out of this function. Both bindings of an
/// `OwnerTransition` count -- a promotion names the ref twice -- because this check runs before the
/// transition's shape is validated, so either binding may still carry any name at this point.
const String * refNamedOutsideScope(const RefOp & op, const MutationScope & scope)
{
    if (op.kind == RefOpKind::OwnerTransition)
    {
        if (op.old_binding && !scope.names(op.old_binding->ref_name))
            return &op.old_binding->ref_name;
        if (op.new_binding && !scope.names(op.new_binding->ref_name))
            return &op.new_binding->ref_name;
        return nullptr;
    }
    if (op.kind == RefOpKind::SetPublishedAt && !scope.names(op.ref_name))
        return &op.ref_name;
    return nullptr;
}
}

bool CasRefLedger::ensureEpochOpenForAppend(const RootNamespace & ns, const std::shared_ptr<RefTableRuntime> & rt,
                                            std::vector<std::shared_ptr<RefMutationItem>> & owned_items)
{
    const auto fail_pending = [&](std::exception_ptr e)
    {
        std::lock_guard<std::mutex> g(ref_queue_mutex);
        owned_items.reserve(owned_items.size() + rt->pending.size());
        for (const auto & item : rt->pending)
        {
            item->error = e;
            item->done = true;
            owned_items.push_back(item);
        }
        rt->pending.clear();
        rt->cv.notify_all();
    };
    const auto runtime_live = [&rt]
    {
        return !rt->catalog_life_invalidated.load(std::memory_order_acquire)
            && !rt->superseded_by_remount.load(std::memory_order_acquire);
    };

    /// The epoch first, then the flags: a self-remount publishes the new epoch before it detaches this
    /// runtime, so only the checks after the read keep a detached runtime from naming the new epoch.
    const uint64_t live_epoch = live_epoch_fn();
    if (!may_mutate())
    {
        fail_pending(makeCasWriteRetryLaterExceptionPtr(fmt::format(
            "CAS mount lost / lease expired — refusing to append ref-log transactions for server_root '{}'",
            config.server_root_id)));
        return false;
    }
    if (rt->superseded_by_remount.load(std::memory_order_acquire))
    {
        fail_pending(makeCasWriteRetryLaterExceptionPtr(fmt::format(
            "CAS ref-log append for server_root '{}': this cached table was superseded by a self-remount — "
            "retry against the fresh mount incarnation",
            config.server_root_id)));
        return false;
    }

    bool terminal = false;
    uint64_t chain_epoch = 0;
    bool stamped = false;
    {
        std::lock_guard lock(rt->state_mutex);
        terminal = rt->state.isTerminal();
        chain_epoch = rt->state.chainEpoch(*rt->accepted_ckpt->ckpt.life_epoch);
        stamped = rt->accepted_ckpt->ckpt.generation >= G_REF_CHAIN;
    }
    if (terminal || chain_epoch == live_epoch)
        return true;

    if (chain_epoch > live_epoch)
    {
        /// A successor's tombstone already named a later epoch: this mount is deposed. Permanent, because no
        /// later attempt in this epoch can succeed.
        {
            std::lock_guard lock(rt->state_mutex);
            rt->lane_state = RefLaneState::Closed;
        }
        logLaneStopped(rt->life, RefLaneState::Closed, live_epoch, "",
            fmt::format("the chain is already in epoch {}", chain_epoch));
        ProfileEvents::increment(ProfileEvents::CASRefAppendSealRejected);
        fail_pending(std::make_exception_ptr(Exception(ErrorCodes::INVALID_STATE,
            "CAS ref-log append for namespace '{}': writer epoch {} was CLOSED by a successor's tombstone "
            "naming epoch {}; nothing was sent. This mount's append lane resumes only under a later epoch",
            ns.string(), live_epoch, chain_epoch)));
        return false;
    }

    /// The chain ends in an earlier epoch. This tombstone needs no `_ckpt` re-read: recovery seals before
    /// install every chain whose epoch may have a request in flight, so no mount of that epoch or of a later
    /// one below the live epoch appends here, and cleanup deletes only what exists.
    if (!stamped)
    {
        CasOperation stamp_op = mount_requests.resume(rt->admitted_fence_generation, runtime_live);
        bool stamp_done = false;
        try
        {
            stamp_done = stampBeforeFirstWrite(ns, *rt, stamp_op);
        }
        catch (...)
        {
            fail_pending(std::current_exception());
            return false;
        }
        if (!stamp_done)
        {
            fail_pending(makeCasWriteRetryLaterExceptionPtr(fmt::format(
                "CAS ref-log append for namespace '{}': the generation 3 stamp of its `_ckpt` met another "
                "token or did not resolve; the lane needs recovery and nothing was appended", ns.string())));
            return false;
        }
    }

    /// Everything that can throw is prepared before the attempt is armed, so a throw leaves the slot
    /// untouched; the install after the create then allocates nothing.
    std::optional<RefTableState> candidate;
    RefTxnId candidate_base_id;
    RefAppendAttempt attempt;
    try
    {
        uint64_t life_epoch = 0;
        RefTxnId id;
        std::optional<RefEpochTransition> open;
        {
            std::lock_guard lock(rt->state_mutex);
            candidate.emplace(rt->state);
            life_epoch = *rt->accepted_ckpt->ckpt.life_epoch;
            id = allocateRefTxnId(*rt).id;
            open = rt->state.getOpenTransition();
        }
        candidate_base_id = candidate->getGreatestApplied();
        RefLogTxn tombstone = makeEpochTombstoneTxn(ns, id, live_epoch, open);
        validateEpochSealGrammarContextual(tombstone, life_epoch);
        applyRefLogTxn(*candidate, tombstone);
        attempt.txn_id = id;
        attempt.key = layout.refLogKey(rt->life, id);
        attempt.admitted_fence_generation = rt->admitted_fence_generation;
        attempt.bytes = sealObject(FormatId::RefLog, encodeRefLogTxn(tombstone));
    }
    catch (...)
    {
        fail_pending(std::current_exception());
        return false;
    }
    const RefTxnId id = attempt.txn_id;

    bool armed = false;
    {
        std::lock_guard lock(rt->state_mutex);
        if (rt->lane_state == RefLaneState::Ready && !rt->append_attempt
            && rt->state.getGreatestApplied() == candidate_base_id)
        {
            rt->append_attempt = attempt;
            rt->lane_state = RefLaneState::Writing;
            armed = true;
        }
    }
    if (!armed)
    {
        /// No lane transition: the lane is not what this attempt expected, so another append may own it.
        fail_pending(std::make_exception_ptr(Exception(ErrorCodes::LOGICAL_ERROR,
            "CAS ref-log append for namespace '{}': lane changed before tombstone {}-{} could be armed",
            ns.string(), id.writer_epoch, id.ref_sequence)));
        return false;
    }

    /// Caller holds `state_mutex`.
    const auto attempt_is_installed = [&]
    {
        return rt->append_attempt && rt->append_attempt->txn_id == id && rt->append_attempt->bytes == attempt.bytes;
    };
    const auto wedge = [&](const String & cause)
    {
        bool wedged = false;
        {
            std::lock_guard lock(rt->state_mutex);
            if (rt->lane_state == RefLaneState::Writing && attempt_is_installed())
            {
                rt->lane_state = RefLaneState::Wedged;
                wedged = true;
            }
        }
        if (wedged)
            logLaneStopped(rt->life, RefLaneState::Wedged, live_epoch, attempt.key, "tombstone outcome unknown: " + cause);
        ProfileEvents::increment(ProfileEvents::CASRefAppendWedged);
        fail_pending(makeCasWriteRetryLaterExceptionPtr(fmt::format(
            "CAS ref-log tombstone {}-{} for namespace '{}' outcome is unknown ({}); the append lane is wedged "
            "until the same key resolves, and this write was not attempted",
            id.writer_epoch, id.ref_sequence, ns.string(), cause)));
    };
    const auto release_unsent = [&](const String & cause)
    {
        {
            std::lock_guard lock(rt->state_mutex);
            if (rt->lane_state == RefLaneState::Writing && attempt_is_installed())
            {
                rt->append_attempt.reset();
                rt->lane_state = RefLaneState::Ready;
            }
        }
        fail_pending(makeCasWriteRetryLaterExceptionPtr(fmt::format(
            "CAS ref-log tombstone {}-{} for namespace '{}' was not written ({}); nothing is durable and this "
            "write was not attempted", id.writer_epoch, id.ref_sequence, ns.string(), cause)));
    };

    CasOperation op = mount_requests.resume(attempt.admitted_fence_generation, runtime_live);
    std::optional<WriteResult> written;
    try
    {
        written = op.create(attempt.key, attempt.bytes, Retry::standard());
    }
    catch (...)
    {
        wedge(getCurrentExceptionMessage(false, false, false, false));
        return false;
    }

    if (const auto * conflict = std::get_if<Conflict>(&*written))
    {
        const auto * occupant_object = std::get_if<Object>(&conflict->seen);
        std::optional<ClassifiedOccupant> occupant;
        if (occupant_object)
        {
            try
            {
                occupant = classifyRefLogOccupant(ns, id, occupant_object->bytes, attempt.bytes);
            }
            catch (...)   // NOLINT(bugprone-empty-catch)
            {
                /// Left unclassified: an occupant this call cannot name is decided by a later read.
            }
        }
        if (!occupant)
        {
            ProfileEvents::increment(ProfileEvents::CASRefAppendOccupantUnreadable);
            wedge(fmt::format("the store refused the create and its occupant could not be named (observed {})",
                detail::renderObservation(conflict->seen)));
            return false;
        }
        if (occupant->kind == Occupant::Tombstone && occupant->transition->next_epoch > live_epoch)
        {
            {
                std::lock_guard lock(rt->state_mutex);
                rt->append_attempt.reset();
                rt->lane_state = RefLaneState::Closed;
            }
            logLaneStopped(rt->life, RefLaneState::Closed, live_epoch, attempt.key,
                fmt::format("a successor's tombstone naming epoch {} holds the slot", occupant->transition->next_epoch));
            ProfileEvents::increment(ProfileEvents::CASRefAppendSealRejected);
            fail_pending(std::make_exception_ptr(Exception(ErrorCodes::INVALID_STATE,
                "CAS ref-log append for namespace '{}': writer epoch {} was CLOSED by a successor's tombstone at "
                "{}-{} naming epoch {}; nothing was sent. This mount's append lane resumes only under a later epoch",
                ns.string(), live_epoch, id.writer_epoch, id.ref_sequence, occupant->transition->next_epoch)));
            return false;
        }
        if (!occupant->decoded)
        {
            /// Not a record recovery could adopt: the anomaly policy, exactly as for an ordinary append.
            {
                std::lock_guard lock(rt->state_mutex);
                rt->append_attempt.reset();
                rt->lane_state = RefLaneState::Faulted;
            }
            logLaneStopped(rt->life, RefLaneState::Faulted, live_epoch, attempt.key,
                "the slot holds an object that does not decode as this tombstone's record");
            const String interference_detail = fmt::format(
                "ref-log tombstone for namespace '{}' txn {}-{} observed an object at its slot that is not a "
                "ref-log transaction of this namespace at that id (observed {})",
                ns.string(), id.writer_epoch, id.ref_sequence, detail::renderObservation(conflict->seen));
            on_impossible_interference(attempt.key, interference_detail, ns.string());
            fail_pending(std::make_exception_ptr(Exception(ErrorCodes::CORRUPTED_DATA, "CAS {}", interference_detail)));
            return false;
        }
        if (occupant->kind != Occupant::Ours)
        {
            /// A late record of the chain's epoch, or an earlier build's recovery tombstone. The attempt is
            /// cleared so recovery does not adjudicate it as this writer's retained bytes; its walk adopts
            /// the occupant and the next flush writes the tombstone after it.
            {
                std::lock_guard lock(rt->state_mutex);
                if (attempt_is_installed())
                    rt->append_attempt.reset();
                requireRecoveryForTakenTombstoneSlot(*rt, ns, id);
            }
            fail_pending(makeCasWriteRetryLaterExceptionPtr(fmt::format(
                "CAS ref-log append for namespace '{}': the slot of tombstone {}-{} holds another record; the "
                "lane needs recovery, which adopts it, and this write was not attempted",
                ns.string(), id.writer_epoch, id.ref_sequence)));
            return false;
        }
    }
    else if (const auto * refused = std::get_if<Refused>(&*written))
    {
        ProfileEvents::increment(ProfileEvents::CASRefAppendDefiniteFailure);
        release_unsent(fmt::format("the store refused it: {}", refused->message));
        return false;
    }
    else if (const auto * gave_up = std::get_if<GaveUp>(&*written))
    {
        if (!gave_up->sent_any)
        {
            ProfileEvents::increment(ProfileEvents::CASRefAppendPreAttemptRefused);
            release_unsent("it was refused before any request was sent");
            return false;
        }
        wedge(fmt::format("gave up after a request was sent, last observed {}",
            detail::renderObservation(gave_up->last_seen)));
        return false;
    }

    /// The tombstone is durable. It is history, so it is installed only if this admission still holds.
    ProfileEvents::increment(ProfileEvents::CASRefAppendTombstoneWritten);
    if (carve_hook_for_test)
        carve_hook_for_test(CarvePhaseForTest::PostDurableTombstoneInstall);
    std::optional<String> install_refusal;
    {
        std::lock_guard lock(rt->state_mutex);
        try
        {
            refuseUnlessAdmitted(op, "ref-log tombstone install");
        }
        catch (...)
        {
            requireRecovery(*rt, ns, "post-create tombstone install admission");
            install_refusal = getCurrentExceptionMessage(false, false, false, false);
        }
        if (!install_refusal)
        {
            const bool state_unchanged = rt->lane_state == RefLaneState::Writing && attempt_is_installed()
                && rt->state.getGreatestApplied() == candidate_base_id;
            if (!state_unchanged)
            {
                requireRecovery(*rt, ns, "lane tombstone install");
                install_refusal = "the table changed before installation";
            }
            else
            {
                std::optional<RefAppendAttempt> completed_attempt;
                try
                {
                    DENY_ALLOCATIONS_IN_SCOPE;
                    rt->state.swap(*candidate);
                    rt->tail_count_since_snapshot.fetch_add(1, std::memory_order_relaxed);
                    rt->tail_bytes_since_snapshot.fetch_add(attempt.bytes.size(), std::memory_order_relaxed);
                    rt->append_attempt.swap(completed_attempt);
                    rt->lane_state = RefLaneState::Ready;
                }
                catch (...)
                {
                    requireRecovery(*rt, ns, "lane tombstone install");
                    throw;
                }
                candidate.reset();
                completed_attempt.reset();
                try
                {
                    rt->state.materializeCommitted();
                }
                catch (...)
                {
                    tryLogCurrentException(getLogger("CasPool"), fmt::format(
                        "CAS ref-log append for namespace '{}': tombstone {}-{} was installed, but the overlay fold "
                        "failed and was retained coherently for the next flush",
                        ns.string(), id.writer_epoch, id.ref_sequence));
                }
            }
        }
    }
    if (install_refusal)
    {
        fail_pending(makeCasWriteRetryLaterExceptionPtr(fmt::format(
            "CAS ref-log append for namespace '{}': tombstone {}-{} is durable but was not installed ({}); the lane "
            "needs recovery, which replays it, and this write was not attempted",
            ns.string(), id.writer_epoch, id.ref_sequence, *install_refusal)));
        return false;
    }
    return true;
}

void CasRefLedger::flushRefBatch(const RootNamespace & ns, const std::shared_ptr<RefTableRuntime> & rt,
                                 std::vector<std::shared_ptr<RefMutationItem>> & owned_items)
{
    /// One flush = one carved batch through one attempted append. Contract: every ORDINARY outcome
    /// (validation reject, DefiniteFailure, Unresolved/wedge, Committed) lands in the affected items so
    /// waiters always wake, and this does NOT throw for any of them. Neither `commitRefChunk` nor
    /// `resolveWedgeOnce` throws past the point where its object is proven durable -- both installs are
    /// allocation-free by construction -- and `resolveWedgeOnce` reports every ORDINARY
    /// outcome, including a fence that moved under it, through its own result rather than by throwing.
    /// The paths that can still throw are the wedge-resolution candidate build, which runs BEFORE the
    /// bounded retry and therefore before anything is proven; a TRANSIENT failure while decoding a
    /// foreign occupant (deliberately not laundered into a verdict); and an allocation failure in this
    /// function's own bookkeeping (e.g. the chunk-boundary reseed). All are contained by
    /// `appendRefOps`' catch, which completes every still-unfinished survivor and restores the leader
    /// bookkeeping, so no caller hangs.
    auto complete_error = [&](const std::vector<std::shared_ptr<RefMutationItem>> & items, std::exception_ptr e)
    {
        std::lock_guard<std::mutex> g(ref_queue_mutex);
        for (const auto & it : items)
        {
            it->error = e;
            it->done = true;
        }
        rt->cv.notify_all();
    };
    auto carve_all_pending = [&]() -> std::vector<std::shared_ptr<RefMutationItem>>
    {
        std::lock_guard<std::mutex> g(ref_queue_mutex);
        std::vector<std::shared_ptr<RefMutationItem>> all(rt->pending.begin(), rt->pending.end());
        rt->pending.clear();
        return all;
    };

    RefRecoveryOutcome recovery_outcome = RefRecoveryOutcome::Recovered;
    try
    {
        recovery_outcome = ensureRefTableRecovered(ns, *rt);
    }
    catch (...)
    {
        complete_error(carve_all_pending(), std::current_exception());
        return;
    }
    if (recovery_outcome == RefRecoveryOutcome::LifeRemoved)
    {
        complete_error(carve_all_pending(),
            makeCasWriteRetryLaterExceptionPtr(recoveredLifeRemovedMessage(ns, rt->life)));
        return;
    }

    /// The local write fence ensures that a superseded or paused writer cannot race the live one.
    /// Fails the WHOLE queue -- every caller would have gotten the same refusal alone.
    if (!may_mutate())
    {
        complete_error(carve_all_pending(), makeCasWriteRetryLaterExceptionPtr(fmt::format(
            "CAS mount lost / lease expired — refusing to append ref-log transactions for server_root '{}'",
            config.server_root_id)));
        return;
    }

    /// Self-remount re-incarnation: this runtime was detached by a
    /// `quiesceRefTablesForRemount` swap, so its cache is a stale (pre-remount) view. Fail the whole
    /// carved batch closed -- allocating an id / applying against this orphaned runtime under the
    /// re-armed fence would split-brain against the fresh runtime the next touch re-recovers. The
    /// superseded flag is ordered before the fence re-arm (release/acquire through `mayMutate`), so
    /// reaching this AFTER passing `mayMutate` above proves the swap happened.
    if (rt->superseded_by_remount.load(std::memory_order_acquire))
    {
        complete_error(carve_all_pending(), makeCasWriteRetryLaterExceptionPtr(fmt::format(
            "CAS ref-log append for server_root '{}': this cached table was superseded by a self-remount — "
            "retry against the fresh mount incarnation",
            config.server_root_id)));
        return;
    }

    /// Resolve an outstanding wedge FIRST (spec INV-1): "It does not start a later ref-log PUT for
    /// that table until the earlier result is resolved." One bounded attempt per flush, and the only
    /// outcome that lets this flush continue is an adoption -- everything else either leaves the lane
    /// uncertain or closes it deliberately, and in both cases every queued caller is told so.
    {
        const WedgeResolutionResult resolution = resolveWedgeOnce(ns, rt);
        switch (resolution.kind)
        {
            case WedgeResolution::NoWedge:
                break;
            case WedgeResolution::Adopted:
                /// The adoption's tail bump may have crossed the snapshot-publish threshold, and this
                /// flush can still return early below WITHOUT reaching the post-commit scheduler -- an
                /// empty carve, or an all-no-op survivor batch, both return before it. Trigger it HERE
                /// so a resolved wedge never leaves the table over-threshold until some later unrelated
                /// mutation happens to arrive. Idempotent with the post-commit call below (the
                /// single-in-flight gate), and off-lock as that call requires.
                maybeScheduleSnapshotPublish(ns, rt);
                break;
            case WedgeResolution::Rejected:
            case WedgeResolution::StillWedged:
            case WedgeResolution::Corrupted:
                complete_error(carve_all_pending(), resolution.survivor_error);
                return;
        }
    }

    /// Test-only (see `setRefPreCarveHookForTest`): a no-op in production.
    if (ref_pre_carve_hook_for_test)
        ref_pre_carve_hook_for_test();

    if (!ensureEpochOpenForAppend(ns, rt, owned_items))
        return;

    /// Carve a compatible batch. `lifecycle != Live` forces a solo carve:
    /// `namespace_birth` must run alone, and the flush already KNOWS the table's current lifecycle
    /// before carving (unlike a per-item property, which would need speculative undo).
    RefTableState working;
    bool table_live = false;
    /// Captured in the SAME hold as `working`: every preview names its record by `nextRefSlot` of the
    /// state it applies to, exactly as the real append does, because replay rejects a record whose id or
    /// backlink differs from that rule.
    uint64_t life_epoch = 0;
    {
        std::lock_guard lock(rt->state_mutex);
        working = rt->state;
        table_live = rt->state.getLifecycle() == RefLifecycle::Live;
        life_epoch = *rt->accepted_ckpt->ckpt.life_epoch;
    }

    /// The supersession gate again, and this is NOT the same check as the one at the top of the flush.
    /// A self-remount does not wait for leaders, so it can land in the window between them -- and after
    /// it does, this runtime's cached view belongs to a dead incarnation while `live_epoch_fn` already
    /// reports the NEW epoch. Everything derived below is then a transaction of an epoch this state has
    /// no chain link for, which INV-2's read side (correctly) calls corruption.
    ///
    /// Checking here is what keeps that from being how an ORDINARY remount is reported. The condition is
    /// a routine, retryable fact about the world -- the fresh runtime the next touch recovers has both
    /// the epoch and the link -- so it must surface as the retry-safe supersession error, not as a
    /// `CORRUPTED_DATA` about a stale premise this lane was never entitled to use.
    if (rt->superseded_by_remount.load(std::memory_order_acquire))
    {
        complete_error(carve_all_pending(), makeCasWriteRetryLaterExceptionPtr(fmt::format(
            "CAS ref-log append for server_root '{}': this cached table was superseded by a self-remount "
            "before its batch was carved — retry against the fresh mount incarnation",
            config.server_root_id)));
        return;
    }

    /// Two-phase carve (spec §2). The old carve popped from `pending` while interleaving the allocating
    /// `seen_refs`/`batch` growth and only recorded the batch into `owned_items` afterwards, so any throw
    /// after the first pop stranded already-popped items -- neither in `pending` nor in `owned_items` --
    /// and their waiters hung forever. Instead:
    ///   PLAN (may throw, mutates no CONTENT): under `ref_queue_mutex`, scan `pending` WITHOUT popping
    ///   and build the selection count, reserving CAPACITY in every container (`batch`, `owned_items`,
    ///   `rt->carved`) that the publish below grows -- a capacity change, never a size or content
    ///   change. A throw here leaves `pending`/`owned_items`/`carved` byte-for-byte unchanged in
    ///   content, so the leadership-exit guard completes only the leader's own item and the untouched
    ///   followers stay queued for a later leader.
    ///   PUBLISH (no-throw): still under the SAME continuous `ref_queue_mutex` hold (no TOCTOU by
    ///   construction), pop the selected front items and append them to `batch` and `owned_items` using
    ///   only non-throwing operations (capacity pre-reserved; `shared_ptr` copies and `deque::pop_front`
    ///   never throw). ProfileEvents increments are deferred past the plan so the plan performs no
    ///   observable mutation beyond the reserved capacity above. The same items are appended to
    ///   `rt->carved`, the confirm-visible mirror the exit guard clears.
    std::vector<std::shared_ptr<RefMutationItem>> batch;
    {
        std::lock_guard<std::mutex> g(ref_queue_mutex);
        const size_t cap = table_live ? kMaxRefBatch : 1;

        /// --- PLAN ---
        std::set<String> seen_refs;
        size_t selected = 0;         /// contiguous front items to carve
        bool scope_cut = false;      /// a duplicate ref name ended the selection early
        for (const auto & candidate : rt->pending)
        {
            if (selected >= cap)
                break;
            if (candidate->scope.kind == MutationScope::Kind::WholeShard)
            {
                /// A whole-shard mutation carves solo -- it may only be the FIRST (and then only) item.
                if (selected != 0)
                    break;
                if (carve_hook_for_test)
                    carve_hook_for_test(CarvePhaseForTest::PlanBatchGrow);
                batch.reserve(1);
                ++selected;
                break;
            }
            if (carve_hook_for_test)
                carve_hook_for_test(CarvePhaseForTest::PlanSeenRefs);
            const auto & cand_scope = candidate->scope;
            bool already_seen = false;
            cand_scope.forEachName([&](const String & name) { already_seen = already_seen || seen_refs.contains(name); });
            if (already_seen)
            {
                scope_cut = true;
                break;
            }
            cand_scope.forEachName([&](const String & name) { seen_refs.insert(name); });
            if (carve_hook_for_test)
                carve_hook_for_test(CarvePhaseForTest::PlanBatchGrow);
            batch.reserve(selected + 1);
            ++selected;
        }
        /// Reserve the leader's responsibility set for the whole selection BEFORE any pop, so the publish
        /// append below cannot throw. `owned_items` already holds the leader's own item (recorded by
        /// `appendRefOps`), which the carve re-adds as it re-appears at the front of `pending` -- a
        /// harmless idempotent double-listing the guard tolerates, matching the pre-fix behavior.
        if (carve_hook_for_test)
            carve_hook_for_test(CarvePhaseForTest::PlanReserveOwned);
        owned_items.reserve(owned_items.size() + selected);
        /// Reserve the confirm-visible mirror too, for the same reason: the publish appends into it and
        /// must not throw.
        rt->carved.reserve(rt->carved.size() + selected);

        /// --- PUBLISH (no-throw) ---
        if (carve_hook_for_test)
            carve_hook_for_test(CarvePhaseForTest::PublishPop);
        for (size_t i = 0; i < selected; ++i)
        {
            batch.push_back(rt->pending.front());        /// shared_ptr copy, capacity reserved
            owned_items.push_back(rt->pending.front());  /// same item into the responsibility set
            rt->carved.push_back(rt->pending.front());   /// and into the confirm-visible mirror
            rt->pending.pop_front();
        }

        /// Deferred past the plan (spec §2) so the plan phase performs no observable mutation.
        if (scope_cut)
            ProfileEvents::increment(ProfileEvents::CASRefBatchScopeCuts);
    }
    if (batch.empty())
        return;   /// raced: everything was carved by a previous flush of this leader

    /// Per-item validation, in order, against `working` (per-request undo via `item_scratch`):
    /// business preconditions (thrown by `build_ops` itself) and the pre-encode admission budget both
    /// fail ONLY the offending item; survivors' ops accumulate into `final_ops` for ONE CHUNK. When
    /// admitting the next item's ops would exceed `ref_txn_max_ops`, the accumulated chunk is committed
    /// as a complete ref-log transaction and validation continues into a fresh chunk against the
    /// reseeded live state (spec §3 chunked flush): one tenure may emit several transactions.
    std::vector<RefOp> final_ops;
    std::vector<std::shared_ptr<RefMutationItem>> survivors;
    /// Every preview below stamps its throwaway transaction with `nextRefSlot` of the state it is about
    /// to be applied to -- the SAME rule `allocateRefTxnId` uses for the real id and backlink, so a
    /// preview can never be rejected for an id shape the persisted transaction would have been given.
    /// Deriving each preview id from ITS OWN state, rather than carrying a running counter across the
    /// loop, is what makes failure isolation hold under
    /// INV-1: an item that throws part-way through its per-op previews leaves `working` untouched, and
    /// the next item's preview is still the successor of `working` rather than of the abandoned item's
    /// last trial id. These ids are never persisted or compared outside this loop.
    for (size_t item_index = 0; item_index < batch.size(); ++item_index)
    {
        const auto & it = batch[item_index];

        /// Step 1: build this item's ops and apply the counts-only per-item caps. `build_ops` runs at
        /// most once per item, HERE -- the overflowing item's ops are built once and reused in the fresh
        /// chunk it lands in (the at-most-once contract holds across a chunk boundary). A failure here
        /// (a business precondition thrown by `build_ops`, or an over-cap item/op) fails ONLY this item;
        /// the chunk in progress and the remaining items are untouched.
        std::vector<RefOp> item_ops;
        bool removal_class = false;
        try
        {
            item_ops = it->build_ops(working);

            /// Counts-only admission caps (spec §3), checked before any op is touched further so an
            /// oversized item or op never reaches `working` or the state-machine preview below and
            /// fails ALONE -- neighbors in this same batch are unaffected. Removal-class items are
            /// exempt from both: they share the larger `ref_removal_max_bytes` byte budget instead
            /// (`checkBudget`, `CasRefLogFormat.cpp`) and are already carved as singletons (`WholeShard`
            /// scope forces a solo carve above). `refLogTxnIsRemovalClass` is the ONE canonical
            /// discriminator (built ops contain `RemoveNamespace`) shared with the codec's own
            /// `checkBudget` -- `WholeShard` scope alone is NOT a substitute (the stale-precommit
            /// reclaim sweep is also `WholeShard`-scoped but is not removal-class).
            removal_class = refLogTxnIsRemovalClass(item_ops);
            if (!removal_class)
            {
                if (item_ops.size() > ref_txn_max_ops)
                    throw Exception(ErrorCodes::LIMIT_EXCEEDED,
                        "ref mutation on namespace '{}' has {} operations, exceeding the normal-class "
                        "per-item op-count cap {} — refusing before any object is created",
                        ns.string(), item_ops.size(), ref_txn_max_ops);
                for (const RefOp & op : item_ops)
                {
                    const size_t op_bytes = encodedOpSize(op);
                    if (op_bytes > ref_op_max_bytes)
                        throw Exception(ErrorCodes::LIMIT_EXCEEDED,
                            "ref mutation on namespace '{}' contains an op of encoded size {}, exceeding "
                            "the normal-class per-op cap {} — refusing before any object is created",
                            ns.string(), op_bytes, ref_op_max_bytes);
                }
            }
        }
        catch (...)
        {
            complete_error({it}, std::current_exception());
            continue;
        }

        /// Step 2: chunk boundary (spec §3). If admitting this item's ops would push the current
        /// (non-empty) chunk over `ref_txn_max_ops`, COMMIT the accumulated chunk now as a COMPLETE
        /// ref-log transaction and start a fresh one. Removal-class items are always solo-carved
        /// (`WholeShard` scope), so `final_ops` is empty when one is processed and this branch never
        /// fires for them.
        if (!removal_class && !final_ops.empty()
            && final_ops.size() + item_ops.size() > ref_txn_max_ops)
        {
            /// Release the scratch `working` so `commitRefChunk`'s post-commit overlay fold is in place
            /// (the E5 fast path), exactly as the single-chunk path does before its commit arm.
            working = RefTableState{};
            const bool committed = commitRefChunk(ns, rt, final_ops, survivors);
            if (!committed)
            {
                /// Failure isolation (spec §3): chunk N's survivors were already failed inside
                /// `commitRefChunk`. Fail THIS item and the entire not-yet-attempted remainder too, so no
                /// owned item is left stranded (its waiter would hang and its `build_ops` closure become
                /// unsafe). Earlier chunks that already committed keep their callers' success -- an
                /// unresolved wedge from `commitRefChunk` therefore contains ONLY this chunk.
                std::vector<std::shared_ptr<RefMutationItem>> remainder(batch.begin() + item_index, batch.end());
                complete_error(remainder, makeCasWriteRetryLaterExceptionPtr(fmt::format(
                    "CAS ref-log append for namespace '{}': a preceding chunk of this multi-transaction "
                    "flush did not commit — this item was not attempted and can be retried", ns.string())));
                return;
            }
            /// Reseed `working` from the now-live state: the speculative `working` with trial ids from
            /// the just-committed chunk is discarded, so a later zero-op item is completed against the
            /// REAL committed id, never a trial id that never persisted. The preview ids need no reseed
            /// of their own -- each is derived from the state it is applied to, so re-seating `working`
            /// re-seats them. A throw at the boundary -- the injected `ChunkReseed` fault, or a genuine
            /// reseed allocation failure -- propagates to `appendRefOps`' tenure-containment catch, which
            /// preserves the already-committed chunk's callers' success.
            if (carve_hook_for_test)
                carve_hook_for_test(CarvePhaseForTest::ChunkReseed);
            {
                std::lock_guard lock(rt->state_mutex);
                working = rt->state;
                life_epoch = *rt->accepted_ckpt->ckpt.life_epoch;
            }
            final_ops.clear();
            survivors.clear();
        }

        /// Step 3: validate this item into the current (possibly fresh) chunk and, past all throwing
        /// points, publish its effects into `working`/`final_ops`/`survivors`. A failure here fails ONLY
        /// this item. `item_ops` was built in step 1 against the pre-boundary state; the carve
        /// deduplicates ref names within a batch, so the overflowing item operates on a ref distinct
        /// from the just-committed chunk's and re-validating it against the reseeded `working` is
        /// consistent. The scope is validated here as well, because the confirm relies on it (see
        /// `confirmExactRef`, rule 3).
        RefTableState item_scratch = working;
        try
        {
            /// Scope validation. `MutationScope` is what `confirmExactRef` reads to decide whether a
            /// queued or in-flight mutation may change the ref it is asked about, so a `Ref{name}` item
            /// whose ops mutate ANOTHER ref would let the confirm answer `Yes` off a row this very item is
            /// about to change. Checked before anything durable and failing only this item: every
            /// production caller names the exact ref its ops mutate, so a mismatch is a programming error.
            if (it->scope.kind == MutationScope::Kind::Ref)
            {
                for (const RefOp & op : item_ops)
                {
                    /// A namespace removal names no ref and moves every row, so it is outside every
                    /// `Ref` scope. The confirm's answer about every OTHER ref rests on this scope
                    /// check alone, so it is rejected here rather than left to any later one.
                    if (op.kind == RefOpKind::RemoveNamespace)
                        throw Exception(ErrorCodes::LOGICAL_ERROR,
                            "ref mutation on namespace '{}' is scoped to ref {} but its {} op moves every ref",
                            ns.string(), it->scope.namesForLog(), refOpKindToWireWord(op.kind));
                    if (const String * other = refNamedOutsideScope(op, it->scope))
                        throw Exception(ErrorCodes::LOGICAL_ERROR,
                            "ref mutation on namespace '{}' is scoped to ref {} but its {} op names ref '{}'",
                            ns.string(), it->scope.namesForLog(), refOpKindToWireWord(op.kind), *other);
                }
            }

            /// Whole-item shape validation (prerequisite to `dropNamespace`): the
            /// per-op loop below previews each op as its OWN single-op trial transaction, so a
            /// whole-transaction-shape rule like "remove_namespace must be the FINAL op" trivially
            /// passes on every singleton slice regardless of this item's REAL combined shape -- a
            /// malformed item (e.g. remove_namespace not last) would otherwise only be caught by
            /// `commitRefChunk`'s candidate apply -- which fails the whole chunk, taking every innocent
            /// co-batched item with it, and (before the candidate moved ahead of the `PUT`) did so only
            /// after the object was already durable. Validate the item's COMPLETE
            /// ops array as ONE combined transaction, against a throwaway copy of the pre-item state,
            /// before doing any other per-op work -- exactly what the real persisted transaction will
            /// contain, using only the public two-phase `applyRefLogTxn` entry point (no need to reach
            /// into the state machine's private per-op helpers).
            if (!item_ops.empty())
            {
                RefTableState shape_check = working;
                const RefNextSlot shape_slot = nextRefSlot(shape_check, life_epoch);
                applyRefLogTxn(shape_check, RefLogTxn{ns.string(), shape_slot.id, item_ops, shape_slot.backlink});
            }
            if (removal_class && !it->terminal_removal_authorized)
                throw Exception(ErrorCodes::CORRUPTED_DATA,
                    "ref mutation on namespace '{}' attempted `RemoveNamespace` through the generic append "
                    "surface; only exact catalog removal ownership may append the terminal transaction",
                    ns.string());

            for (const RefOp & op : item_ops)
            {
                /// Admission budget: only STATE-GROWING ops need the check --
                /// an `owner_transition` installing a binding (add or promote) and `set_published_at`.
                /// `namespace_birth` is exempt (it grows nothing, and a never-born state's preview has
                /// no meaningful "current snapshot" to encode); `remove_namespace` and a pure
                /// owner_transition removal shrink state and can never violate the budget.
                const bool state_growing = (op.kind == RefOpKind::OwnerTransition && op.new_binding.has_value())
                    || op.kind == RefOpKind::SetPublishedAt;
                if (state_growing && !admits(item_scratch, op, rt->snapshot_budget, rt->removal_budget))
                    throw Exception(ErrorCodes::LIMIT_EXCEEDED,
                        "ref mutation on namespace '{}' would exceed the table's admission budget "
                        "(snapshot_budget={} removal_budget={}) — refusing before any object is created",
                        ns.string(), rt->snapshot_budget, rt->removal_budget);
                /// Apply THIS op to item_scratch now (a single-op trial transaction) so a LATER op of
                /// the SAME item (e.g. namespace_birth immediately followed by its first
                /// owner_transition) is validated -- both here and by admits's own preview -- against
                /// a state that already reflects it, exactly as the real combined transaction will.
                const RefNextSlot trial_slot = nextRefSlot(item_scratch, life_epoch);
                applyRefLogTxn(item_scratch, RefLogTxn{ns.string(), trial_slot.id, {op}, trial_slot.backlink});
            }
            /// Reserve the growth of BOTH accumulators BEFORE this item's effects are published. These
            /// reservations are the ONLY remaining throwing steps; once they succeed the publish below is
            /// no-throw -- `working`'s move-assignment is `noexcept`, and the `RefOp` moves and the
            /// `shared_ptr` copy land in pre-reserved capacity. Before the fix, `working` was moved and
            /// `final_ops` appended before these allocations, so a failure here left a failed item applied
            /// to `working` (corrupting later items' validation) and -- when the throw fell between the
            /// two accumulator writes -- its ops already in the durably-committed transaction while its
            /// own caller was told the append failed.
            final_ops.reserve(final_ops.size() + item_ops.size());
            survivors.reserve(survivors.size() + 1);
            if (carve_hook_for_test)
                carve_hook_for_test(CarvePhaseForTest::ValidateFinalOps);
            working = std::move(item_scratch);
            for (RefOp & op : item_ops)
                final_ops.push_back(std::move(op));
            survivors.push_back(it);
        }
        catch (...)
        {
            complete_error({it}, std::current_exception());
        }
    }
    if (final_ops.empty())
    {
        /// Either every item failed validation (already completed via complete_error above, nothing
        /// left to do), or every survivor of the LAST chunk contributed ZERO ops (an idempotent no-op,
        /// e.g. precommitAdd/promote re-targeting a manifest already exactly committed). Survivors of the
        /// latter kind still need marking done -- with no new object created, `committed_id` is the
        /// table's current high-water mark. After an earlier committed chunk, `working` was reseeded from
        /// the live state, so that mark is the REAL id the earlier chunk persisted, never a discarded
        /// trial id.
        if (!survivors.empty())
        {
            std::lock_guard<std::mutex> g(ref_queue_mutex);
            for (const auto & it : survivors)
            {
                it->committed_id = working.getGreatestApplied();
                it->done = true;
            }
            rt->cv.notify_all();
        }
        return;
    }

    /// Commit the FINAL chunk of this tenure (spec §3): the remaining accumulated ops form the last --
    /// possibly only -- ref-log transaction. Release the scratch `working` first so `commitRefChunk`'s
    /// post-commit overlay fold is in place (the E5 fast path), then run the full committed arm. Its
    /// survivors are completed (success or failure) inside it, so nothing is owed here on any outcome,
    /// and it no longer throws past its durable `PUT` at all (spec §A1).
    working = RefTableState{};
    commitRefChunk(ns, rt, final_ops, survivors);
}

CasRefLedger::PreparedRefChunk CasRefLedger::prepareRefChunk(
    const Layout & layout, const NamespaceLifeId & life, RefTableState state, const RefTxnId & id,
    const std::optional<RefTxnId> & chain_link, std::span<const RefOp> ops, uint64_t admitted_generation)
{
    PreparedRefChunk prepared{
        .candidate = std::move(state),
        .candidate_base_id = {},
        .chunk_txn = RefLogTxn{life.ns.string(), id, std::vector<RefOp>(ops.begin(), ops.end()), chain_link},
        .prepared_attempt = {},
    };
    prepared.candidate_base_id = prepared.candidate.getGreatestApplied();

    /// A throw here is a clean PRE-durability failure -- the same class as an ordinary validation
    /// reject: no object exists yet, the cache is untouched, and the id is simply never used (the next
    /// attempt re-derives it from this same unchanged state).
    applyRefLogTxn(prepared.candidate, prepared.chunk_txn);

    /// The COMPLETE attempt (spec §A1 site 3), so the request can read its key and body straight out of
    /// it and the `Unresolved` arm only has to MOVE it into the runtime. Building it here rather than
    /// after the `PUT` is what keeps an allocation failure from leaving a possibly-DURABLE object with
    /// NEITHER the transaction nor the attempt recorded -- strictly worse than a wedge, because the next
    /// append would then mint a fresh id against a state missing a landed transaction, which is the
    /// divergence the candidate exists to prevent.
    ///
    /// A rename, not an extra copy: the seal writes its result directly into the attempt's body and the
    /// key is computed directly into the attempt's key, so nothing is copied twice on the committed path
    /// either.
    ///
    /// FAULT CLASS, stated because the extraction NARROWED it, and this is the whole of the change.
    /// `RefLogTxn`'s construction above and the key computation below used to sit outside any local
    /// `try` in `commitRefChunk`, so an allocation failure in either escaped `commitRefChunk` and
    /// `flushRefBatch` entirely and was caught by `appendRefOps`' tenure catch, which completes every
    /// still-INCOMPLETE owned item with that exception and releases leadership, ending the tenure (an
    /// item already made durable by an earlier committed chunk keeps its success -- the guard leaves it
    /// `done` with no error, deliberately). Inside this function both are covered by the caller's single
    /// `catch`, so the same failure now completes `chunk_survivors` with that exception and returns
    /// false, and the tenure continues. That is exactly what an apply failure at this same pre-durability
    /// stage already did, which is the point: an allocation failure here is no longer the one fault whose
    /// blast radius differs from its immediate neighbours'.
    ///
    /// Nothing is stranded either way, and there is no universal remainder handler to appeal to -- the
    /// two call sites dispose of their own. On a chunk boundary `flushRefBatch` fails this item plus the
    /// entire not-yet-attempted remainder retry-later and returns at once. On the FINAL chunk the tail
    /// call ignores the return value because no remainder is left: every batch item is by then either
    /// already completed (it failed its own validation, or it belonged to an earlier chunk that
    /// `commitRefChunk` completed) or is in THIS chunk's `chunk_survivors`, which this path completes.
    /// `chunk_survivors` is emphatically NOT the whole batch on that path -- `survivors.clear()` at each
    /// chunk boundary leaves it holding only the last chunk's items.
    ///
    /// One reported error class does change on the boundary path: the not-yet-attempted remainder now
    /// gets a retry-later refusal instead of the raw escaping exception. Those items were provably never
    /// attempted, so retry-later is the truthful class, and it is the class they already got whenever the
    /// preceding chunk failed for any other reason.
    prepared.prepared_attempt.txn_id = id;
    prepared.prepared_attempt.key = layout.refLogKey(life, id);
    prepared.prepared_attempt.admitted_fence_generation = admitted_generation;
    prepared.prepared_attempt.bytes = sealObject(FormatId::RefLog, encodeRefLogTxn(prepared.chunk_txn));
    return prepared;
}

bool CasRefLedger::stampBeforeFirstWrite(const RootNamespace & ns, RefTableRuntime & rt, CasOperation & op)
{
    std::optional<CkptSample> accepted;
    {
        std::lock_guard lock(rt.state_mutex);
        accepted = rt.accepted_ckpt;
    }
    CkptStampResult stamp = stampCkptGeneration3(op, layout, rt.life, *accepted);
    std::lock_guard lock(rt.state_mutex);
    if (stamp.outcome == CkptStampOutcome::Stamped)
    {
        rt.accepted_ckpt = std::move(stamp.stamped);
        return true;
    }
    /// Another token means another writer changed `_ckpt` since recovery read it; recovery re-reads it.
    LOG_WARNING(getLogger("CasPool"),
        "CAS ref table '{}': the generation 3 stamp of `_ckpt` {}; the lane needs recovery before it appends",
        ns.string(), stamp.outcome == CkptStampOutcome::TokenMismatch ? "met another token" : "did not resolve");
    rt.lane_state = RefLaneState::NeedsRecovery;
    return false;
}

bool CasRefLedger::commitRefChunk(const RootNamespace & ns, const std::shared_ptr<RefTableRuntime> & rt,
                                  const std::vector<RefOp> & chunk_ops,
                                  const std::vector<std::shared_ptr<RefMutationItem>> & chunk_survivors)
{
    /// Reconstructed locally so this arm has the SAME completion + fence semantics as when it lived
    /// inline in `flushRefBatch`: `complete_error` wakes a chunk's waiters under `ref_queue_mutex`, and
    /// `runtime_live` folds `superseded_by_remount` into the append operation's liveness so a
    /// self-remount landing between a leader's pre-allocate re-check and its write ends the operation
    /// rather than committing against a stale cache.
    auto complete_error = [&](const std::vector<std::shared_ptr<RefMutationItem>> & items, std::exception_ptr e)
    {
        std::lock_guard<std::mutex> g(ref_queue_mutex);
        for (const auto & it : items)
        {
            it->error = e;
            it->done = true;
        }
        rt->cv.notify_all();
    };
    /// Everything the append fence cannot see. The generation term is the operation's own, so it is
    /// deliberately absent here.
    const auto runtime_live = [&rt]
    {
        return !rt->catalog_life_invalidated.load(std::memory_order_acquire)
            && !rt->superseded_by_remount.load(std::memory_order_acquire);
    };

    const bool removal_append = refLogTxnIsRemovalClass(chunk_ops);

    /// The local capability prevents callers from reaching this point through the generic API; the
    /// durable row is the independent final authority. Re-read it immediately before id allocation so
    /// a stale exact runtime cannot append a terminal after the catalog life changed, and require the
    /// positive lane to still be closed under the same queue lock that guards admission.
    if (removal_append)
    {
        try
        {
            if (!std::all_of(chunk_survivors.begin(), chunk_survivors.end(),
                    [](const auto & item) { return item->terminal_removal_authorized; }))
                throw Exception(ErrorCodes::CORRUPTED_DATA,
                    "CAS namespace removal '{}': terminal chunk lacks exact removal ownership",
                    ns.string());

            {
                std::lock_guard queue_lock(ref_queue_mutex);
                if (!rt->removal_admission_closed)
                    throw Exception(ErrorCodes::CORRUPTED_DATA,
                        "CAS namespace removal '{}': terminal append reached an open positive lane",
                        ns.string());
            }

            const uint64_t admitted_generation = rt->admitted_fence_generation;
            CasOperation catalog_op = mount_requests.resume(admitted_generation, runtime_live);
            const CasRefCatalog::Snapshot catalog = CasRefCatalog::read(catalog_op, layout);
            refuseUnlessAdmitted(catalog_op, "terminal removal append");
            catalog.life_index.throwIfAmbiguous("CAS terminal removal append");
            const auto entry_it = std::find_if(
                catalog.catalog.entries.begin(), catalog.catalog.entries.end(),
                [&](const CatalogEntry & entry)
                {
                    return entry.ns == ns && entry.incarnation == rt->life.incarnation;
                });
            if (entry_it == catalog.catalog.entries.end() || entry_it->state != NsState::Removing)
                throw Exception(ErrorCodes::CORRUPTED_DATA,
                    "CAS namespace removal '{}': terminal append requires its exact catalog life to be Removing",
                    ns.string());
        }
        catch (...)
        {
            complete_error(chunk_survivors, std::current_exception());
            return false;
        }
    }

    /// Self-remount re-check BEFORE allocating an id: the top-of-flush gate
    /// is passed once, but a leader can stall between it and here -- in `build_ops`' caller I/O -- across
    /// the whole fence-loss + remount window, then resume after `armMountFence`. Allocating {new_epoch,
    /// seq} now and PUTting it (its live `fence_ok` would pass) would persist a transaction validated
    /// against this orphaned runtime's STALE cache and lose data. `superseded_by_remount` is
    /// published before the fence re-arm, so failing closed here (no id, no PUT, no wedge, cache
    /// unchanged) keeps the durable log free of any stale-view transaction. The append `fence_ok`
    /// (which also checks the flag) is the airtight backstop for the narrow window past this point.
    if (rt->superseded_by_remount.load(std::memory_order_acquire))
    {
        complete_error(chunk_survivors, makeCasWriteRetryLaterExceptionPtr(fmt::format(
            "CAS ref-log append for server_root '{}': this cached table was superseded by a self-remount "
            "before id allocation — retry against the fresh mount incarnation",
            config.server_root_id)));
        return false;
    }

    /// `Ready` is the sole new-id admission state. The resolver either restores it or returns the whole
    /// batch closed, so any other state here is an internal lifecycle violation. It can be reached only
    /// by a bug or a test injection after the top-of-flush resolver gate; make that contradiction an
    /// explicit `Faulted` state and route it through the same anomaly policy as foreign interference.
    {
        RefLaneState lane_state = RefLaneState::Faulted;
        std::optional<String> attempt_key;
        {
            std::lock_guard lock(rt->state_mutex);
            lane_state = rt->lane_state;
            if (rt->append_attempt)
                attempt_key = rt->append_attempt->key;
        }
        if (lane_state != RefLaneState::Ready)
        {
            {
                std::lock_guard lock(rt->state_mutex);
                rt->append_attempt.reset();
                rt->lane_state = RefLaneState::Faulted;
            }
            logLaneStopped(rt->life, RefLaneState::Faulted, live_epoch_fn(), attempt_key.value_or(""),
                fmt::format("new-id allocation found the lane {}, not Ready", refLaneStateName(lane_state)));
            on_impossible_interference(attempt_key.value_or(""), fmt::format(
                "ref-log append reached new-id allocation while the lane was in state {} instead of Ready",
                static_cast<uint8_t>(lane_state)), ns.string());
            /// `Faulted` is a TERMINAL lane state (same as the two other `Faulted` arms further down this
            /// function, which both report `CORRUPTED_DATA`) -- reporting it via
            /// `makeCasWriteRetryLaterExceptionPtr` would tell the caller a state the lane can never leave
            /// on its own is transient. The arm is self-limiting today (the next flush's
            /// `resolveWedgeOnce` takes the `invalid_lane_state` `Reason` arm and re-reports it the same
            /// way), but a terminal state must never be reported as retryable from ANY arm --
            /// `gtest_cas_ref_writer.cpp`'s `CasAnomalyPolicy.NonReadyAtNewIdAllocationFaultsAndFailsClosed`
            /// pins this arm specifically (it already drives this exact seam; only its expected error
            /// class needed to change).
            complete_error(chunk_survivors, std::make_exception_ptr(Exception(ErrorCodes::CORRUPTED_DATA,
                "CAS ref-log append for namespace '{}': refusing to allocate a new ref-log id while the "
                "lane is not Ready (state {}, attempt key '{}'); the lane is faulted until remount",
                ns.string(), static_cast<uint8_t>(lane_state), attempt_key.value_or(""))));
            return false;
        }
    }

    /// A `_ckpt` below generation 3 is replaced before the first log this lane writes, so no reader that
    /// takes `committed_through` as the frontier ever sees a log above it written under this rule.
    {
        bool stamped_already = false;
        {
            std::lock_guard lock(rt->state_mutex);
            stamped_already = rt->accepted_ckpt->ckpt.generation >= G_REF_CHAIN;
        }
        if (!stamped_already)
        {
            CasOperation stamp_op = mount_requests.resume(rt->admitted_fence_generation, runtime_live);
            bool stamped = false;
            try
            {
                stamped = stampBeforeFirstWrite(ns, *rt, stamp_op);
            }
            catch (...)
            {
                complete_error(chunk_survivors, std::current_exception());
                return false;
            }
            if (!stamped)
            {
                complete_error(chunk_survivors, makeCasWriteRetryLaterExceptionPtr(fmt::format(
                    "CAS ref-log append for namespace '{}': the generation 3 stamp of its `_ckpt` met another "
                    "token or did not resolve; the lane needs recovery and nothing was appended", ns.string())));
                return false;
            }
        }
    }

    /// ONE atomic reading of everything this transaction is derived from that the RUNTIME owns: the
    /// state it will be applied to, its id and backlink, and the mount incarnation that admitted it.
    /// `prepareRefChunk` below is a pure function of its arguments, and every argument is either one of
    /// those readings or an immutable input the runtime does not own at all (the layout, the namespace,
    /// this chunk's ops).
    ///
    /// The candidate is built from this snapshot BEFORE the PUT (spec §A1), so that the region between
    /// "this chunk's object is durable" and "the runtime records it" is allocation-free and therefore
    /// cannot throw. It used to be the other way round -- PUT, then `applyRefLogTxn(rt->state, ...)` -- and
    /// that apply CAN throw on an allocation failure (the COW containers allocate their overlays), which
    /// left the transaction durable but invisible to the writer: the apply check of the day admitted any
    /// strictly greater id, so a later transaction sailed over the hole and a snapshot published
    /// afterwards was labelled with THAT id -- recovery then skips the stranded transaction forever
    /// while GC, which folds the ref LOGS, still applies it. That divergence is a data-loss class, not a
    /// stale cache. INV-1 now refuses the same hole from the read side too, so the accident this
    /// ordering prevents would also have to survive the density check to do any damage.
    ///
    /// A throw during PREPARATION, by contrast, is a clean PRE-durability failure -- the same class as an
    /// ordinary validation reject: no object exists yet, the cache is untouched, and the id is simply
    /// never used (the next attempt re-derives it from this same unchanged state).
    ///
    /// The snapshot copy taken here is cheap because it SHARES the live state's COW bases; the apply
    /// inside preparation allocates only the overlay. It is CONSUMED by preparation -- moved, not copied
    /// again -- so this remains the single copy of `rt->state` the commit path makes, exactly as it was.
    /// The candidate is deliberately NOT materialized: a state that shares its base cannot fold in place,
    /// so folding now would rebuild the whole base (O(table) per chunk). The install below restores unique
    /// base ownership before the existing post-install fold, which therefore stays O(overlay).
    ///
    /// The id and backlink are derived in the SAME critical section that snapshots the state:
    /// they are a function of the state, so reading them at a different instant than the state the
    /// transaction is applied to would be deriving this chunk's id from a different stream. Only this
    /// leader mutates `rt->state`, so the two reads cannot disagree today -- taking them together is what
    /// keeps that from being an invariant a future edit has to rediscover. This is also why the id, the
    /// backlink and the generation are INPUTS to `prepareRefChunk` rather than things it derives: it has
    /// no lock to read them under.
    ///
    /// The fence GENERATION is read in the same hold for the same reason: it is this transaction's
    /// ADMISSION token, one atomic reading of "which mount incarnation allowed this attempt", which the
    /// attempt then carries and every later retry and install presents back.
    std::optional<RefTableState> state_snapshot;
    RefTxnId id;
    std::optional<RefTxnId> chain_link;
    uint64_t admitted_fence_generation = 0;
    try
    {
        std::lock_guard lock(rt->state_mutex);
        state_snapshot.emplace(rt->state);
        const RefNextSlot slot = allocateRefTxnId(*rt);
        id = slot.id;
        chain_link = slot.backlink;
        admitted_fence_generation = rt->admitted_fence_generation;
    }
    catch (...)
    {
        complete_error(chunk_survivors, std::current_exception());
        return false;
    }
    /// Preparation is ONE pure call, and it is everything decided before this chunk can have any durable
    /// effect: the candidate, the transaction with its chain link and the complete attempt (key + sealed
    /// bytes). The chunk's only durable effect is the ref-log create below.
    ///
    /// Everything it can throw is a pre-durability rejection with the identical handler, which is why ONE
    /// catch replaces the two that used to sit around those steps separately (see the FAULT CLASS note on
    /// `prepareRefChunk` for the two statements whose catcher changed). The reachable ones are a rejected
    /// apply and a failed seal; an allocation failure anywhere inside is the same class, and so is the
    /// `BAD_ARGUMENTS` that `layout.refLogKey` -> `namespaceStreamPrefix` -> `checkNamespace` would raise
    /// for a malformed namespace -- unreachable here, since a mounted table's namespace already passed
    /// that check, but the list is not meant to read closed.
    std::optional<PreparedRefChunk> prepared;
    try
    {
        prepared.emplace(prepareRefChunk(layout, rt->life, std::move(*state_snapshot), id, chain_link,
                                         chunk_ops, admitted_fence_generation));
    }
    catch (...)
    {
        complete_error(chunk_survivors, std::current_exception());
        return false;
    }
    const RefTxnId candidate_base_id = prepared->candidate_base_id;
    /// The candidate moves back into its own optional so the post-durable install region below is
    /// textually untouched by this extraction: it still swaps `*candidate` in and still `reset()`s it in
    /// the same place, for the same reason (releasing bases whose `use_count()` the fold then reads). The
    /// move is COW-pointer-only and happens here, while nothing is durable.
    std::optional<RefTableState> candidate{std::move(prepared->candidate)};

    /// ONE operation for this chunk's durable phase -- the log create and the install recheck -- resumed
    /// under the generation the transaction was admitted at. The engine refuses every request once that
    /// generation moves, so a chunk admitted by an incarnation this mount no longer holds makes nothing
    /// durable.
    CasOperation op = mount_requests.resume(admitted_fence_generation, runtime_live);

    /// Install the exact attempt before the first possible send. This is NOT preparation -- it mutates
    /// `RefTableRuntime` and takes `state_mutex` -- so it stays here, between preparation and the first
    /// send. From this point every exit must make one explicit lane transition; there is no independent
    /// marker to update or reconstruct later.
    bool attempt_armed = false;
    {
        std::lock_guard lock(rt->state_mutex);
        const bool same_base = rt->state.getGreatestApplied() == candidate_base_id;
        if (rt->lane_state == RefLaneState::Ready && !rt->append_attempt && same_base)
        {
            static_assert(std::is_nothrow_move_constructible_v<RefAppendAttempt>);
            rt->append_attempt = std::move(prepared->prepared_attempt);
            rt->lane_state = RefLaneState::Writing;
            attempt_armed = true;
        }
    }
    if (!attempt_armed)
    {
        /// No lane transition: the lane is not what THIS attempt expected, so another append may own it.
        complete_error(chunk_survivors, std::make_exception_ptr(Exception(
            ErrorCodes::LOGICAL_ERROR,
            "CAS ref-log append for namespace '{}': lane changed before attempt {}-{} could be armed",
            ns.string(), id.writer_epoch, id.ref_sequence)));
        return false;
    }
    const RefAppendAttempt & active_attempt = *rt->append_attempt;

    /// Callers must learn whether a retry can duplicate this transaction, so the two outcomes that
    /// leave that open say so, and keep the original code and cause.
    struct DurableUnconfirmed
    {
        int code;
        String cause;
    };
    /// A precommit binding is never published as a part. What a durable promotion means to the table
    /// depends on the engine (`MergeTree` activates it at restart, `ReplicatedMergeTree` detaches it as
    /// unexpected), which this layer cannot see, so the text claims only that the write may take effect.
    /// The chunk can mix kinds, so every waiter gets the text of its own step.
    const auto fail_durable_unconfirmed = [&](const DurableUnconfirmed & failure)
    {
        for (const auto & item : chunk_survivors)
        {
            const std::vector<std::shared_ptr<RefMutationItem>> one{item};
            if (item->kind == RootMutationKind::Precommit)
            {
                complete_error(one, std::make_exception_ptr(Exception(failure.code,
                    "CAS ref-log txn {}-{} for namespace '{}' is durable but was not acknowledged ({}); the "
                    "next ref-log recovery installs the precommit binding, but the part is never published, "
                    "so its rows will not appear",
                    id.writer_epoch, id.ref_sequence, ns.string(), failure.cause)));
            }
            else
            {
                complete_error(one, std::make_exception_ptr(Exception(failure.code,
                    "CAS ref-log txn {}-{} for namespace '{}' is durable but was not acknowledged ({}); the "
                    "next ref-log recovery installs it, so the write may still take effect and a retry may "
                    "duplicate it",
                    id.writer_epoch, id.ref_sequence, ns.string(), failure.cause)));
            }
        }
    };
    const auto outcome_unknown = [&](int code, const String & cause)
    {
        return std::make_exception_ptr(Exception(code,
            "CAS ref-log txn {}-{} for namespace '{}' outcome is unknown ({}): it may or may not take "
            "effect; a retry may duplicate it",
            id.writer_epoch, id.ref_sequence, ns.string(), cause));
    };

    /// An attempt that may have been sent moves the lane from `Writing` to `Wedged`; true when it did.
    const auto wedge_lane = [&]
    {
        std::lock_guard lock(rt->state_mutex);
        if (rt->lane_state != RefLaneState::Writing || !rt->append_attempt || rt->append_attempt->txn_id != id)
            return false;
        rt->lane_state = RefLaneState::Wedged;
        return true;
    };

    /// A successor's tombstone occupies the id this append derived, so the append is conclusively
    /// rejected and the lane resumes only under a later epoch.
    const auto reject_append_at_successor_seal = [&]
    {
        /// Counted: this writer was deposed, and a lane that keeps landing here is a mount that lost its
        /// lease and does not know it yet.
        ProfileEvents::increment(ProfileEvents::CASRefAppendSealRejected);
        {
            std::lock_guard lock(rt->state_mutex);
            rt->append_attempt.reset();
            rt->lane_state = RefLaneState::Closed;
        }
        logLaneStopped(rt->life, RefLaneState::Closed, id.writer_epoch, layout.refLogKey(rt->life, id),
            "a successor's tombstone holds the slot");
        complete_error(chunk_survivors, std::make_exception_ptr(Exception(ErrorCodes::INVALID_STATE,
            "CAS ref-log append for namespace '{}': writer epoch {} was CLOSED by a successor's tombstone "
            "at {}-{}, which conclusively rejects this transaction (it was never acknowledged). "
            "This mount's append lane resumes only under a later epoch",
            ns.string(), id.writer_epoch, id.writer_epoch, id.ref_sequence)));
    };

    std::optional<WriteResult> written;
    try
    {
        written = op.create(active_attempt.key, active_attempt.bytes, Retry::standard());
    }
    catch (...)
    {
        /// Every classified outcome comes back as a value, so an exception here is a deterministic
        /// local failure raised at a point where an attempt may already have been sent. That is
        /// ambiguous, and it therefore transfers ownership from `Writing` to `Wedged`; the exact
        /// attempt remains installed.
        if (wedge_lane())
            logLaneStopped(rt->life, RefLaneState::Wedged, id.writer_epoch, active_attempt.key,
                "the create threw: " + getCurrentExceptionMessage(false, false, false, false));
        complete_error(chunk_survivors, outcome_unknown(getCurrentExceptionCode(), getCurrentExceptionMessage(false, false, false, false)));
        return false;
    }

    if (const auto * conflict = std::get_if<Conflict>(&*written))
    {
        /// THREE-WAY ADJUDICATION, the same one the wedge resolution owes. "A different
        /// object at our derived key" is not one situation but two, and they call for opposite
        /// reactions. One of them is EXPECTED: a successor that sealed our epoch put its epoch-closing
        /// record at exactly the id we keep re-deriving, and INV-2 says we must keep re-deriving it
        /// ("a dying lane that observes the seal retries T+1, never mints T+2"). Treating that as
        /// foreign interference would fence the mount and raise an anomaly alarm on the designed path.
        /// The other is a genuine breach of write-exclusivity and must be exactly as loud as before.
        ///
        /// The occupant arrives WITH the conflict: the write's own settling read already observed the
        /// key, so no second request is issued here.
        const auto * occupant_object = std::get_if<Object>(&conflict->seen);
        if (!occupant_object)
        {
            /// The settling read named NO occupant: it either failed, or it proved the key absent after
            /// the store had already refused our create. Neither observation can be adjudicated, and
            /// neither is terminal -- `resolveWedgeOnce` meets the identical observation and keeps the
            /// lane WEDGED, and this site owes the same answer. The wedge is what makes the next flush
            /// re-create at this exact key and adjudicate whatever it then finds; faulting instead would
            /// spend the table's write availability until a remount on a read that may well succeed on
            /// the next attempt.
            ///
            /// It must be COUNTED, because this arm is quiet by construction: the loud interference
            /// report is only reached once the occupant can be NAMED, so a real breach whose occupant
            /// keeps failing to be read would otherwise show up as nothing but a throttled log line
            /// under load. Sustained growth on this counter is the signal that the loud path is starved.
            ProfileEvents::increment(ProfileEvents::CASRefAppendOccupantUnreadable);
            if (wedge_lane())
                logLaneStopped(rt->life, RefLaneState::Wedged, id.writer_epoch, active_attempt.key,
                    "the store refused the create and the settling read named no occupant");
            ProfileEvents::increment(ProfileEvents::CASRefAppendWedged);
            complete_error(chunk_survivors, outcome_unknown(ErrorCodes::NETWORK_ERROR, fmt::format(
                "the store refused the create and the settling read named no occupant (observed {}); "
                "the append lane is wedged until the same key resolves",
                detail::renderObservation(conflict->seen))));
            return false;
        }

        Occupant occupant = Occupant::Foreign;
        bool classified = false;
        try
        {
            occupant = classifyRefLogOccupant(ns, id, occupant_object->bytes, active_attempt.bytes).kind;
            classified = true;
        }
        catch (...)   // NOLINT(bugprone-empty-catch)
        {
            /// Left unclassified deliberately -- see below. The conflict itself is what the survivors
            /// are told about; the adjudication's own failure is not their business.
        }
        if (!classified)
        {
            /// An occupant WAS observed, and naming it raised something other than the malformed-object
            /// codes `classifyRefLogOccupant` answers `Foreign` for. We could not learn WHICH of the two
            /// situations this is, so we decide NEITHER: reporting foreign interference would fence the
            /// mount on a guess, and reporting a conclusive rejection would acknowledge a deposition we
            /// did not observe.
            ///
            /// It must be COUNTED, for the same reason the arm above is.
            ProfileEvents::increment(ProfileEvents::CASRefAppendOccupantUnreadable);
            const String attempt_key = active_attempt.key;
            {
                std::lock_guard lock(rt->state_mutex);
                rt->append_attempt.reset();
                rt->lane_state = RefLaneState::Faulted;
            }
            logLaneStopped(rt->life, RefLaneState::Faulted, id.writer_epoch, attempt_key,
                "the occupant of the slot could not be classified");
            complete_error(chunk_survivors, std::make_exception_ptr(Exception(
                ErrorCodes::CORRUPTED_DATA,
                "CAS ref-log append for namespace '{}': a DIFFERENT object occupies the id {}-{} this table "
                "derived, and telling a successor's tombstone from foreign interference did not succeed "
                "(observed {}) — the lane is faulted until remount recovery adjudicates durable state",
                ns.string(), id.writer_epoch, id.ref_sequence, detail::renderObservation(conflict->seen))));
            return false;
        }
        if (occupant == Occupant::Tombstone)
        {
            reject_append_at_successor_seal();
            return false;
        }
        if (occupant != Occupant::Ours)
        {
            /// A genuine breach. This table's appends are now BLOCKED, and that is the intended contract:
            /// under mount-lease exclusivity this key is exclusively ours, so a foreign object at it is
            /// corruption or a protocol breach, not a race. The id is not consumed, so the next attempt
            /// derives the SAME id and hits the SAME conflict, loudly, until a remount-level recovery (a
            /// fresh writer epoch is a fresh key namespace) clears it. Advancing past the occupant, which is
            /// what the pool-wide allocator did, would have written this table's stream around a foreign
            /// object and hidden the violation -- and produced a hole in a stream that must stay dense.
            ///
            /// Route it through the anomaly policy, exactly as the wedge-resolution site does for the
            /// identical observation. Failing closed is right, but failing closed FOREVER is
            /// not: without this the mount stays blocked on this table until somebody notices and remounts
            /// by hand. One impossibility, one reaction. The report is deliberately BEFORE the survivors are
            /// completed, so the fence is closed by the time any caller wakes and can retry.
            const String attempt_key = active_attempt.key;
            {
                std::lock_guard lock(rt->state_mutex);
                rt->append_attempt.reset();
                rt->lane_state = RefLaneState::Faulted;
            }
            logLaneStopped(rt->life, RefLaneState::Faulted, id.writer_epoch, attempt_key,
                "a foreign object holds the slot");
            const String interference_detail = fmt::format(
                "ref-log append for namespace '{}' txn {}-{} observed a DIFFERENT object already at the id it "
                "derived, and it is not a tombstone of this namespace (observed {})",
                ns.string(), id.writer_epoch, id.ref_sequence, detail::renderObservation(conflict->seen));
            on_impossible_interference(attempt_key, interference_detail, ns.string());
            complete_error(chunk_survivors, std::make_exception_ptr(Exception(
                ErrorCodes::CORRUPTED_DATA, "CAS {}", interference_detail)));
            return false;
        }
        /// `Occupant::Ours`: our OWN bytes at the id we derived, so an earlier attempt of this exact
        /// transaction is already durable and byte equality is the proof (never a shape or generation
        /// match). This lane's state machine does not produce it -- an attempt that may have landed
        /// leaves the lane `Wedged`, and resolving that wedge advances the id -- but it is decided here
        /// rather than left to the arm above, because reporting this table's own content as foreign
        /// interference would fence the mount. The commit path below installs it, exactly as the wedge
        /// site's identical observation does.
    }
    else if (const auto * refused = std::get_if<Refused>(&*written))
    {
        /// Proof that nothing became durable returns the exact attempt to `Ready`.
        {
            std::lock_guard lock(rt->state_mutex);
            if (rt->lane_state == RefLaneState::Writing && rt->append_attempt
                && rt->append_attempt->txn_id == id)
            {
                rt->append_attempt.reset();
                rt->lane_state = RefLaneState::Ready;
            }
        }
        ProfileEvents::increment(ProfileEvents::CASRefAppendDefiniteFailure);
        complete_error(chunk_survivors, makeCasWriteRetryLaterExceptionPtr(fmt::format(
            "CAS ref-log append for namespace '{}' definitively failed ({}); "
            "cached state is unchanged and txn id {}-{} was never used (a retry re-derives it); "
            "nothing is durable, so a retry cannot duplicate this transaction",
            ns.string(), refused->message, id.writer_epoch, id.ref_sequence)));
        return false;
    }
    else if (const auto * gave_up = std::get_if<GaveUp>(&*written))
    {
        /// The ONE give-up shape that must NOT wedge. The wedge exists because an unresolved write MAY
        /// HAVE LANDED: the durable log may or may not contain this transaction, only a read of that
        /// exact key can settle it, and until it does, minting a later id would build on a state that
        /// may be missing a landed transaction. All of that presupposes an attempt was SENT.
        ///
        /// `sent_any` is the whole call's, not its last attempt's: it is false only when every gate
        /// refused before the first request reached the network, so the key is provably unwritten,
        /// there is nothing for a wedge to resolve, and wedging is pointless.
        ///
        /// It is no longer HARMFUL, and the difference is worth stating because the old comment here
        /// rested on it: a wedge over a never-written key used to be unclearable, because resolution
        /// was a bare read and a read can only ever report absent. The every-attempt rule replaced
        /// that with a conditional CREATE, so such a wedge now clears on the next caller's flush by
        /// landing the transaction. What remains is that this lane would be blocked until then for no
        /// reason at all -- a transient fence blip in the pre-attempt gate would cost the table its
        /// write availability, and buy nothing, since there is provably nothing to resolve.
        ///
        /// The counterexample this argument deliberately excludes: a fence lost or a deadline reached
        /// AFTER at least one attempt, and an attempt that COMMITTED but returned under a dropped
        /// fence, both report `sent_any` true. Each may have left a durable object, so each keeps
        /// wedging.
        if (!gave_up->sent_any)
        {
            {
                std::lock_guard lock(rt->state_mutex);
                if (rt->lane_state == RefLaneState::Writing && rt->append_attempt
                    && rt->append_attempt->txn_id == id)
                {
                    rt->append_attempt.reset();
                    rt->lane_state = RefLaneState::Ready;
                }
            }
            /// Count it. Before this arm existed these refusals bumped `CASRefAppendWedged`, so
            /// removing the wedge also removed the only signal they were happening at all -- and a
            /// soak oracle watching that counter fall could not tell "the fix works" from "nothing
            /// happened". A separate event keeps both readings available: the wedge counter now means
            /// only genuinely ambiguous appends, and this one means availability preserved.
            ProfileEvents::increment(ProfileEvents::CASRefAppendPreAttemptRefused);
            /// The id is not consumed: it was derived from `greatest_applied`, which this
            /// refusal leaves exactly as it was, so the next caller on this table derives the SAME id
            /// and the durable stream keeps no trace of the refusal. That is the free half of the
            /// every-attempt rule -- an attempt that provably sent nothing owes nothing.
            complete_error(chunk_survivors, makeCasWriteRetryLaterExceptionPtr(fmt::format(
                "CAS ref-log append for namespace '{}' txn {}-{} was refused BEFORE any request was "
                "sent — the append lane is NOT wedged (nothing can be durable, so there is "
                "nothing to resolve) and the txn id is not consumed (a retry re-derives it); "
                "nothing is durable, so a retry cannot duplicate this transaction",
                ns.string(), id.writer_epoch, id.ref_sequence)));
            return false;
        }
        if (wedge_lane())
            logLaneStopped(rt->life, RefLaneState::Wedged, id.writer_epoch, active_attempt.key,
                fmt::format("gave up after a request was sent, last observed {}", detail::renderObservation(gave_up->last_seen)));
        ProfileEvents::increment(ProfileEvents::CASRefAppendWedged);
        complete_error(chunk_survivors, outcome_unknown(ErrorCodes::NETWORK_ERROR, fmt::format(
            "gave up after a request was sent, last observed {}; the append lane is wedged until the "
            "same key resolves", detail::renderObservation(gave_up->last_seen))));
        return false;
    }

    {
        /// The log object is durable -- this call's own attempt committed it, or the conflict above
        /// proved an earlier attempt of this exact transaction already had. A durable log on the chain is
        /// history, so it is installed and acknowledged only if this admission still holds.
        if (carve_hook_for_test)
            carve_hook_for_test(CarvePhaseForTest::PostDurableInstall);
        bool install_refused = false;
        std::optional<DurableUnconfirmed> install_admission_error;
        {
            std::lock_guard lock(rt->state_mutex);
            /// The create can succeed and the fence can move before the state lock is reached.
            /// Re-present the same admission INSIDE the install hold, immediately before inspecting and
            /// swapping the candidate. A stale runtime may leave the log durable, but it must neither
            /// install nor acknowledge it.
            try
            {
                refuseUnlessAdmitted(op, "ref-log install");
            }
            catch (...)
            {
                requireRecovery(*rt, ns, "post-create install admission");
                install_admission_error = DurableUnconfirmed{getCurrentExceptionCode(),
                    "its install was not admitted after the log was created: "
                    + getCurrentExceptionMessage(false, false, false, false)};
            }
            /// Only this leader mutates `rt->state`, so the candidate's base snapshot is still the
            /// current one: there is one append-lane leader per table at a time (the `leader_active`
            /// baton), the wedge-resolution apply ran earlier in this same flush on this same thread,
            /// recovery installs a state exactly once per runtime and has already completed for this
            /// table, and every other consumer (readers, the snapshot publisher) only COPIES the
            /// state under this mutex. Evaluated here, one statement before the install, and
            /// asserted inside it: the comparison allocates nothing, and the identifier is short
            /// enough that even the failure path's message is inline-buffered rather than heap
            /// allocated, so no build can turn the assert itself into an allocation in the region.
            const bool state_unchanged
                = !install_admission_error
                && rt->lane_state == RefLaneState::Writing
                && rt->append_attempt
                && rt->append_attempt->txn_id == id
                && rt->append_attempt->bytes == active_attempt.bytes
                && rt->state.getGreatestApplied() == candidate_base_id;
            if (!install_admission_error && !state_unchanged)
            {
                /// RELEASE-mode counterpart of the `chassert` inside the region below, which is a
                /// no-op in a release build and therefore no guard at all for a window that spans a
                /// full network round trip. Swapping the candidate in anyway would DISCARD whatever
                /// advanced the table. The object is durable and this runtime cannot record it,
                /// `LOGICAL_ERROR` here, where the wedge site's identical refusal reports the
                /// retry-later class, and the asymmetry is deliberate: THIS one is reachable only by
                /// a second writer inside one process -- a bug in this build, which a debug build
                /// should abort on and shout about. The wedge site's is reachable by an ordinary
                /// remount racing a slow resolution, which is a retryable fact about the world, not a
                /// bug. Same refusal, different provenance, so different loudness.
                requireRecovery(*rt, ns, "commitRefChunk install");
                install_refused = true;
            }
            else if (!install_admission_error)
            {
                std::optional<RefAppendAttempt> completed_attempt;
                try
                {
                    DENY_ALLOCATIONS_IN_SCOPE;
                    if (install_region_probe_for_test)
                        install_region_probe_for_test();
                    chassert(state_unchanged);
                    rt->state.swap(*candidate);
                    rt->tail_count_since_snapshot.fetch_add(1, std::memory_order_relaxed);
                    rt->tail_bytes_since_snapshot.fetch_add(active_attempt.bytes.size(), std::memory_order_relaxed);
                    rt->append_attempt.swap(completed_attempt);
                    rt->lane_state = RefLaneState::Ready;
                }
                catch (...)
                {
                    requireRecovery(*rt, ns, "commitRefChunk install");
                    throw;
                }
                candidate.reset();
                completed_attempt.reset();
                try
                {
                    rt->state.materializeCommitted();
                }
                catch (...)
                {
                    tryLogCurrentException(getLogger("CasPool"), fmt::format(
                        "CAS ref-log append for namespace '{}': committed txn {}-{} was applied durably, but "
                        "the post-commit overlay fold failed and was retained coherently for the next flush",
                        ns.string(), id.writer_epoch, id.ref_sequence));
                }
            }
        }
        if (install_admission_error)
        {
            fail_durable_unconfirmed(*install_admission_error);
            return false;
        }
        if (install_refused)
        {
            complete_error(chunk_survivors, std::make_exception_ptr(Exception(
                ErrorCodes::LOGICAL_ERROR,
                "CAS ref-log append for namespace '{}': txn {}-{} is durable but was not acknowledged "
                "(this table changed before installation); the lane needs recovery and refuses later "
                "writes until replay",
                ns.string(), id.writer_epoch, id.ref_sequence)));
            return false;
        }
        if (carve_hook_for_test)
            carve_hook_for_test(CarvePhaseForTest::PostInstallPreAck);
        ProfileEvents::increment(ProfileEvents::CASRefBatchFlushes);
        ProfileEvents::increment(ProfileEvents::CASRefBatchedMutations, chunk_survivors.size());
        {
            std::lock_guard<std::mutex> g(ref_queue_mutex);
            for (const auto & it : chunk_survivors)
            {
                it->committed_id = id;
                it->done = true;
            }
            rt->cv.notify_all();
        }
        /// The threshold trigger -- off the lane,
        /// dispatched AFTER waking every waiter above so this commit's own callers are never
        /// delayed by it. Per chunk: each committed chunk schedules its own publication,
        /// and settlement coalesces the triggers so a mid-tenure publisher never suppresses a later
        /// chunk (`settleSnapshotPublish`).
        maybeScheduleSnapshotPublish(ns, rt);
        return true;
    }
}

bool CasRefLedger::hasStateBearingSnapshotCandidateUnderStateLock(const RefTableRuntime & rt) const
{
    /// The newest snapshot must be strictly older than the candidate. A tombstone advances epoch
    /// geometry but carries no table state, so a state that ends in one is never a snapshot candidate.
    /// A chain in an earlier epoch is not this mount's to publish.
    return rt.state.getLifecycle() == RefLifecycle::Live
        && (!rt.newest_snapshot_id || *rt.newest_snapshot_id < rt.state.getGreatestApplied())
        && !rt.state.getOpenTransition()
        && rt.accepted_ckpt
        && rt.state.chainEpoch(*rt.accepted_ckpt->ckpt.life_epoch) == live_epoch_fn();
}

bool CasRefLedger::admitSnapshotPublishUnderStateLock(RefTableRuntime & rt)
{
    /// Caller holds `rt.state_mutex` (the `may_mutate` fence check is the caller's responsibility, since
    /// it is not held under `state_mutex`). The whole decision -- the threshold trigger, the
    /// single-in-flight gate, the backoff deadline -- and the `pending_snapshot_publishes` increment all
    /// happen under that ONE hold, so two racing dispatchers can never both admit a publish for this
    /// table, and the settlement re-evaluation can decrement-and-re-admit without the count transiently
    /// reaching zero.
    const uint64_t now = boot_ms_now_fn();
    if (!rt.catalog_life_invalidated.load(std::memory_order_acquire)
        && !rt.superseded_by_remount.load(std::memory_order_acquire)
        /// Use the execution predicate at admission too, so settlement cannot redispatch a recovered seal.
        && hasStateBearingSnapshotCandidateUnderStateLock(rt)
        /// Single-in-flight gate: at most one background publish per table.
        && rt.pending_snapshot_publishes.load(std::memory_order_relaxed) == 0
        /// Backoff deadline: after a non-Committed publish, a saturated backend is not re-dispatched
        /// until the bounded backoff elapses (the read-triggered PUT-storm latch).
        && now >= rt.publish_backoff_until_ms)
    {
        /// The threshold trigger reads the tail counters directly -- no walk, no age filter.
        /// `tail_count_since_snapshot`/`tail_bytes_since_snapshot` count ONLY applied txns strictly above
        /// `newest_snapshot_id` (maintained incrementally by every commit in `commitRefChunk` and by the
        /// wedge-resolution apply in `flushRefBatch`), so `over_threshold` here is never true without a
        /// real, immediately-coverable candidate.
        const uint64_t publishable_count = rt.tail_count_since_snapshot.load(std::memory_order_relaxed);
        const uint64_t publishable_bytes = rt.tail_bytes_since_snapshot.load(std::memory_order_relaxed);
        const bool over_threshold = publishable_count > config.snapshot_log_count_threshold
            || publishable_bytes > config.snapshot_log_bytes_threshold;
        if (over_threshold)
        {
            rt.pending_snapshot_publishes.fetch_add(1, std::memory_order_relaxed);
            return true;
        }
    }
    return false;
}

void CasRefLedger::dispatchSnapshotPublisher(const RootNamespace & ns, const std::shared_ptr<RefTableRuntime> & rt)
{
    /// `admitSnapshotPublishUnderStateLock` already incremented `pending_snapshot_publishes` for THIS
    /// dispatch. Off the mutation hot path: `tryPublishSnapshotAndAdvanceCheckpointOnce` never touches
    /// the append queue, so dispatching it onto an unrelated global-pool thread can never deadlock a flush leader.
    ProfileEvents::increment(ProfileEvents::CASRefSnapshotPublishDispatched);

    /// The reservation was taken under `state_mutex` before this call, and only a LAUNCHED task's
    /// settlement retires it. This guard therefore covers exactly the paths on which no task runs --
    /// including an allocation failure while constructing the task, which happens in the expression
    /// below, before the dispatcher's own body is entered.
    bool launched = false;
    SCOPE_EXIT({
        if (launched)
            return;
        {
            std::lock_guard lock(rt->state_mutex);
            rt->pending_snapshot_publishes.fetch_sub(1, std::memory_order_relaxed);
        }
        rt->publish_settle_cv.notify_all();
    });

    try
    {
        launched = dispatch_detached([this, ns, rt](DetachedStopToken token)
        {
            /// Settlement runs on EVERY exit. A handler that throws must not be able to strand the
            /// reservation -- nothing would ever report that, and quiescence would wait forever.
            SCOPE_EXIT({ settleSnapshotPublish(ns, rt, token); });
            setThreadName(ThreadName::CAS_REF_SNAPSHOT_PUBLISH);
            try
            {
                tryPublishSnapshotAndAdvanceCheckpointOnceOnRuntime(ns, rt, token);
            }
            catch (...)
            {
                {
                    /// Pace the exception exactly like an ordinary non-Committed publish. Every ordinary
                    /// failure arm inside the attempt arms this backoff before returning; an exception
                    /// thrown before any of them reaches here with the deadline unarmed, and settlement's
                    /// ONLY pacing gate is that deadline -- so without this the publisher redispatches at
                    /// full speed for as long as the fault persists.
                    std::lock_guard lock(rt->state_mutex);
                    advancePublishBackoff(*rt);
                }
                if (publish_error_hook)
                    publish_error_hook();
                try
                {
                    tryLogCurrentException(getLogger("CasPool"), "CAS background snapshot publish attempt failed");
                }
                catch (...) // NOLINT(bugprone-empty-catch)
                {
                }
            }
        });
    }
    catch (...)
    {
        try
        {
            tryLogCurrentException(getLogger("CasPool"), "CAS background snapshot-publish dispatch failed");
        }
        catch (...) // NOLINT(bugprone-empty-catch)
        {
        }
    }
}

void CasRefLedger::settleSnapshotPublish(
    const RootNamespace & ns, const std::shared_ptr<RefTableRuntime> & rt, const DetachedStopToken & token)
{
    /// Fence re-checked outside `state_mutex` (as in `maybeScheduleSnapshotPublish`): a fence lost
    /// between this publish's dispatch and its settlement must suppress a follow-up.
    const bool live_mount = !token.stopping() && may_mutate();
    bool redispatch = false;
    {
        std::lock_guard lock(rt->state_mutex);
        /// Drop THIS publish's in-flight count and, under the SAME hold, re-evaluate the accumulated
        /// tail. A chunked tenure (or any concurrent mutation) that raised more log above the newest
        /// snapshot while this publish was capturing an earlier prefix had its trigger discarded by the
        /// single-flight gate; settlement re-fires it here so chunks 2..N are not suppressed until an
        /// unrelated later trigger (spec §3 snapshot coalescing). Re-admitting under the SAME lock as the
        /// decrement means `pending_snapshot_publishes` never transiently reaches 0 across the handoff, so
        /// `waitForSnapshotPublishSettleForTest` never observes a false "settled". A durable publish
        /// already subtracted its captured tail, so this self-terminates once the tail is back at/under
        /// threshold; a non-durable one armed the backoff, which `admit...` respects -- no PUT storm. A
        /// `Superseded` one is not durable, arms no backoff and adopts nothing: the next candidate is at
        /// least the newer durable snapshot, so it publishes or skips as identical.
        rt->pending_snapshot_publishes.fetch_sub(1, std::memory_order_relaxed);
        if (live_mount)
            redispatch = admitSnapshotPublishUnderStateLock(*rt);
    }
    if (redispatch)
        dispatchSnapshotPublisher(ns, rt);
    else
        rt->publish_settle_cv.notify_all();
}

void CasRefLedger::maybeScheduleSnapshotPublish(const RootNamespace & ns, const std::shared_ptr<RefTableRuntime> & rt)
{
    /// Never dispatch a publisher while the fence is lost: a publish is a
    /// conditional PUT that would fail `fence_ok` and return non-Committed anyway, and dispatching one
    /// during the self-remount window is exactly the stale-cache-publish race the remount quiesce closes
    /// -- with no dispatch here, `quiesceRefTablesForRemount` only has to drain publishers already in
    /// flight before the fence dropped, never a moving target.
    if (!may_mutate())
        return;

    bool dispatch = false;
    {
        std::lock_guard lock(rt->state_mutex);
        dispatch = admitSnapshotPublishUnderStateLock(*rt);
    }
    if (dispatch)
        dispatchSnapshotPublisher(ns, rt);
}


std::string_view refLaneStateName(RefLaneState state)
{
    switch (state)
    {
        case RefLaneState::Ready:
            return "Ready";
        case RefLaneState::Writing:
            return "Writing";
        case RefLaneState::Wedged:
            return "Wedged";
        case RefLaneState::NeedsRecovery:
            return "NeedsRecovery";
        case RefLaneState::Closed:
            return "Closed";
        case RefLaneState::Faulted:
            return "Faulted";
    }
    UNREACHABLE();
}

void CasRefLedger::advancePublishBackoff(RefTableRuntime & rt)
{
    /// Caller holds `rt.state_mutex`. Double the interval from `initial` up to `max` per consecutive
    /// non-Committed publish outcome; arm the deadline off the boottime clock (`bootMsNow`), so an
    /// injected test clock drives it deterministically and a VM-suspend cannot shorten it.
    rt.publish_backoff_ms = rt.publish_backoff_ms == 0
        ? config.snapshot_publish_backoff_initial_ms
        : std::min<uint64_t>(rt.publish_backoff_ms * 2, config.snapshot_publish_backoff_max_ms);
    rt.publish_backoff_until_ms = boot_ms_now_fn() + rt.publish_backoff_ms;
    ProfileEvents::increment(ProfileEvents::CASRefSnapshotPublishBackoff);
}

void CasRefLedger::resetPublishBackoff(RefTableRuntime & rt)
{
    /// Caller holds `rt.state_mutex`. A durable publish clears the cooldown.
    rt.publish_backoff_ms = 0;
    rt.publish_backoff_until_ms = 0;
}

void CasRefLedger::waitForSnapshotPublishSettleForTest(const RootNamespace & ns)
{
    const auto rt = lookupRefTableRuntime(ns);
    if (!rt)
        return;
    std::unique_lock lock(rt->state_mutex);
    rt->publish_settle_cv.wait(lock, [&] { return rt->pending_snapshot_publishes.load(std::memory_order_relaxed) == 0; });
}

int CasRefLedger::pendingSnapshotPublishesForTest(const RootNamespace & ns)
{
    const auto rt = lookupRefTableRuntime(ns);
    if (!rt)
        return 0;
    std::lock_guard lock(rt->state_mutex);
    return rt->pending_snapshot_publishes.load(std::memory_order_relaxed);
}

std::optional<RefTxnId> CasRefLedger::newestPublishedSnapshotIdForTest(const RootNamespace & ns)
{
    const auto rt = lookupRefTableRuntime(ns);
    if (!rt)
        return std::nullopt;
    std::lock_guard lock(rt->state_mutex);
    return rt->newest_snapshot_id;
}

bool CasRefLedger::refRecoveryCancelRequestedForTest(const RootNamespace & ns)
{
    std::lock_guard<std::mutex> qlock(ref_queue_mutex);
    const auto it = ref_name_slots.find(ns.string());
    return it != ref_name_slots.end() && it->second.current
        && it->second.current->recovery_cancel_requested.load(std::memory_order_acquire);
}

bool CasRefLedger::refTableRecoveredForTest(const RootNamespace & ns)
{
    /// Like every observational seam, deliberately does not recover: the fail-closed tests ask "did that
    /// refused recovery install anything", and an observer that recovered on demand would answer its own
    /// question with a yes.
    std::lock_guard<std::mutex> qlock(ref_queue_mutex);
    const auto it = ref_name_slots.find(ns.string());
    if (it == ref_name_slots.end() || !it->second.current)
        return false;
    std::lock_guard lock(it->second.current->state_mutex);
    return it->second.current->recovered;
}

size_t CasRefLedger::tailSinceSnapshotCountForTest(const RootNamespace & ns)
{
    const auto rt = lookupRefTableRuntime(ns);
    if (!rt)
        return 0;
    std::lock_guard lock(rt->state_mutex);
    return rt->tail_count_since_snapshot.load(std::memory_order_relaxed);
}

size_t CasRefLedger::committedOverlayEntriesForTest(const RootNamespace & ns)
{
    const auto rt = lookupRefTableRuntime(ns);
    if (!rt)
        return 0;
    std::lock_guard lock(rt->state_mutex);
    return rt->state.getCommitted().overlayEntriesForTest();
}

std::set<std::pair<String, ManifestRef>> CasRefLedger::livePrecommitsForTest(const RootNamespace & ns)
{
    const auto rt = lookupRefTableRuntime(ns);
    if (!rt)
        return {};
    std::lock_guard lock(rt->state_mutex);
    return rt->state.getPrecommits();
}

namespace
{
/// A clamped-to-zero fetch-subtract for the tail counters.
/// `tryPublishSnapshotAndAdvanceCheckpointOnce` is public and NOT serialized against itself (two
/// overlapping attempts can finish out of order), so the monotonic guard
/// below
/// skips a stale (superseded) adoption's subtraction outright, but it cannot see a SMALLER-candidate
/// attempt that lands its adoption BEFORE a larger-candidate one already in flight: that ordering would
/// have the larger attempt's `captured_count`/`captured_bytes` double-count the smaller one's
/// already-subtracted region. A plain `fetch_sub` would then underflow the unsigned counter, wrapping it
/// to near `UINT64_MAX` and permanently re-latching the read-triggered PUT-storm trigger on every
/// subsequent read -- a release-build regression of the exact bug this guard prevents. Clamping to
/// zero instead settles for a
/// benign, self-healing under-count (a delayed next dispatch; the NEXT publish always captures the true
/// live state fresh, so snapshot CONTENT is never affected) over an unsafe wraparound.
void clampedCounterSub(std::atomic<uint64_t> & counter, uint64_t amount)
{
    uint64_t old_value = counter.load(std::memory_order_relaxed);
    while (!counter.compare_exchange_weak(old_value, old_value > amount ? old_value - amount : 0,
        std::memory_order_relaxed))
    {
    }
}
}


bool CasRefLedger::tryPublishSnapshotAndAdvanceCheckpointOnce(const RootNamespace & ns)
{
    const auto rt = acquireMutableRefTableRuntime(ns);
    return tryPublishSnapshotAndAdvanceCheckpointOnceOnRuntime(ns, rt);
}


bool CasRefLedger::tryPublishSnapshotAndAdvanceCheckpointOnceOnRuntime(
    const RootNamespace & ns, const std::shared_ptr<RefTableRuntime> & rt)
{
    return tryPublishSnapshotAndAdvanceCheckpointOnceOnRuntimeImpl(ns, rt, std::nullopt);
}


bool CasRefLedger::tryPublishSnapshotAndAdvanceCheckpointOnceOnRuntimeImpl(
    const RootNamespace & ns, const std::shared_ptr<RefTableRuntime> & rt,
    const std::optional<DetachedStopToken> & token)
{
    if (ensureRefTableRecovered(ns, *rt, token) == RefRecoveryOutcome::LifeRemoved)
        return false;
    if (token && token->stopping())
        return false;

    /// This attempt's ADMISSION token, captured once, before any of its I/O: which mount incarnation
    /// allowed this publish. It is presented back on every `_ckpt` CAS attempt below (spec §3's
    /// recheck discipline -- the same value at every site), so a publish admitted under an incarnation
    /// that has since been replaced can advance nothing.
    const uint64_t admitted_generation = rt->admitted_fence_generation;
    CasOperation op = mount_requests.resume(admitted_generation, [&rt]
    {
        return !rt->catalog_life_invalidated.load(std::memory_order_acquire)
            && !rt->superseded_by_remount.load(std::memory_order_acquire);
    });
    const auto runtime_still_admitted = [&op] { return op.admitted(); };

    /// ONE copy of the live state, at a transaction boundary -- no
    /// replay, no per-entry retention. The tail counters are captured in the SAME critical section so
    /// adoption below subtracts exactly what this attempt's candidate actually covers.
    RefTableState candidate_state;
    RefTxnId candidate_x;
    uint64_t captured_count = 0;
    uint64_t captured_bytes = 0;
    std::optional<RefLaneState> blocked_lane;
    {
        std::lock_guard lock(rt->state_mutex);
        /// Snapshot certification is a `Ready`-only operation. This read and the state copy are one
        /// critical section, so no transition to `Writing`, `Wedged`, or `NeedsRecovery` can interleave
        /// between certification and capture.
        if (rt->lane_state != RefLaneState::Ready)
        {
            blocked_lane = rt->lane_state;
            advancePublishBackoff(*rt);
        }
        else if (!hasStateBearingSnapshotCandidateUnderStateLock(*rt))
            return false;   /// shares admission: terminal, covered, and seal candidates are all inert
        else
        {
            candidate_state = rt->state;
            candidate_x = rt->state.getGreatestApplied();
            captured_count = rt->tail_count_since_snapshot.load(std::memory_order_relaxed);
            captured_bytes = rt->tail_bytes_since_snapshot.load(std::memory_order_relaxed);
        }
    }

    if (blocked_lane)
    {
        /// `Writing` is the ordinary race with an appender, so only the other states warrant a warning.
        if (*blocked_lane == RefLaneState::Writing)
            LOG_DEBUG(getLogger("CasPool"),
                "CAS ref table '{}': refusing snapshot publication while the append lane is not Ready (state {})",
                ns.string(), refLaneStateName(*blocked_lane));
        else
            LOG_WARNING(getLogger("CasPool"),
                "CAS ref table '{}': refusing snapshot publication while the append lane is not Ready (state {})",
                ns.string(), refLaneStateName(*blocked_lane));
        return false;
    }

    if (snapshot_after_capture_hook_for_test)
        snapshot_after_capture_hook_for_test();

    /// The candidate holds shared COW bases, so even this stale-runtime exit must release it under the
    /// same mutex that protects materialization. More importantly, this is the last test-only pause
    /// before the first durable effect: retirement/remount can invalidate the captured runtime while it
    /// is paused, and the old holder then becomes inert instead of resolving the name to a successor.
    if (!runtime_still_admitted())
    {
        std::lock_guard lock(rt->state_mutex);
        candidate_state = RefTableState{};
        return false;
    }

    const RefTableSnapshot snap = snapshotOf(candidate_state, ns.string());

    /// `candidate_state` is a COW copy that SHARES `rt->state`'s committed/owned-manifest bases. It is
    /// dead past `snapshotOf`. Destroy it HERE, under `state_mutex` -- not at function return outside any
    /// lock. Its destruction is a `shared_ptr` release-DECREMENT of those shared bases; the flush thread's
    /// in-place `materializeCommitted()` reads their `use_count()` (relaxed) under this same mutex. Doing
    /// the release off-lock would leave that load racing this atomic decrement with no happens-before
    /// (TSan-reportable) and could momentarily let a flush observe a `use_count()` of 1 while this
    /// decrement is in flight. Under the lock the two are serialized. Every subsequent exit path (encode
    /// failure, non-Committed PUT, the monotonic-guard early return, success) then destroys an already
    /// empty `candidate_state`, which touches no shared base. See both COW headers' materialize safety
    /// argument, which relies on exactly this: every cross-thread copy is created AND destroyed under the
    /// state lock.
    {
        std::lock_guard lock(rt->state_mutex);
        candidate_state = RefTableState{};
    }

    String bytes;
    try
    {
        bytes = sealObject(FormatId::RefSnapshot, encodeRefTableSnapshot(snap));
    }
    catch (...)
    {
        /// Failure Handling: "Snapshot create fails: keep all logs; writer recovery remains unchanged."
        /// Treat like any other non-Committed outcome: arm the backoff so a persistent encode failure
        /// does not re-dispatch on every read.
        std::lock_guard lock(rt->state_mutex);
        advancePublishBackoff(*rt);
        return false;
    }
    const String key = layout.refSnapshotKey(rt->life, candidate_x);
    const WriteResult put = op.create(key, bytes, Retry::standard());
    if (const auto * conflict = std::get_if<Conflict>(&put))
    {
        /// A snapshot key names the exact state it encodes, so a re-run of this publish -- and only a
        /// re-run -- can find its own identical body already there. DIFFERENT bytes under this mount's
        /// exclusive lease are corruption, and the publisher must not paper over them with a backoff.
        const auto * occupant = std::get_if<Object>(&conflict->seen);
        if (!occupant || occupant->bytes != bytes)
            throw Exception(ErrorCodes::CORRUPTED_DATA,
                "CAS ref table '{}': a DIFFERENT object occupies snapshot {}-{} (observed {})",
                ns.string(), candidate_x.writer_epoch, candidate_x.ref_sequence,
                detail::renderObservation(conflict->seen));
    }
    else if (!std::holds_alternative<Committed>(put))
    {
        /// Refused or unresolved: DO NOT prune (no durable covering snapshot -- pruning the tail
        /// without one is data loss). Arm the bounded per-table backoff so the read path does not
        /// re-dispatch this full-snapshot encode+PUT until it elapses -- the read-triggered PUT-storm
        /// latch breaker. A later trigger past the deadline retries.
        std::lock_guard lock(rt->state_mutex);
        advancePublishBackoff(*rt);
        return false;
    }
    ProfileEvents::increment(ProfileEvents::CASRefSnapshotPutBytes, bytes.size());   /// account published bytes

    /// The snapshot body is durable, and it becomes CLEANUP-AUTHORITATIVE only once `_ckpt` names it.
    /// Ordering the two this way is what makes the intervening race harmless -- cleanup planned between
    /// the body PUT and this CAS still reads the OLD checkpoint, and the deletion gate is "strictly
    /// below" it, so it cannot delete the snapshot just published.
    ///
    /// A checkpoint that does NOT advance leaves the attempt unadopted: the backoff is armed and this
    /// returns false, so a later trigger re-runs the whole publish. The re-run's body create meets its
    /// own identical bytes as a conflict, which the occupant compare above accepts, so retrying costs
    /// one conditional PUT and not a second snapshot. Adopting instead would mark this snapshot as the
    /// newest -- suppressing every later publish for it -- while the checkpoint still pointed below it,
    /// leaving recovery replaying from an older base with nothing scheduled to fix it.
    bool ckpt_advanced = false;
    bool superseded = false;
    if (!runtime_still_admitted())
        return false;
    if (snapshot_before_ckpt_cas_hook_for_test)
        snapshot_before_ckpt_cas_hook_for_test();
    try
    {
        const CkptPublishOutcome outcome = publishCkptSnapshot(op, layout, rt->life, candidate_x);
        if (outcome == CkptPublishOutcome::Published)
            ProfileEvents::increment(ProfileEvents::CASRefCheckpointPublished);
        else if (outcome == CkptPublishOutcome::IdenticalSkip)
            ProfileEvents::increment(ProfileEvents::CASRefCheckpointIdenticalSkip);
        ckpt_advanced = ckptPublicationDurable(outcome);
        superseded = outcome == CkptPublishOutcome::Superseded;
    }
    catch (...)
    {
        /// Swallowed deliberately, and ONLY here: the snapshot body is already durable, so there is
        /// nothing to undo and nothing for a caller to decide --
        /// `tryPublishSnapshotAndAdvanceCheckpointOnce` is one best-effort attempt whose every other
        /// failure arm also returns false with a backoff. The
        /// counter and the log line are what keep it from being silent.
        tryLogCurrentException(getLogger("CasPool"),
            "CAS ref table '" + ns.string() + "': the snapshot body is durable but its _ckpt checkpoint "
            "could not be advanced; the snapshot is not yet cleanup-authoritative and the publish will "
            "be retried");
    }
    /// `_ckpt` already names a snapshot newer than this candidate: nothing to publish, so no adoption and no backoff.
    if (superseded)
        return false;
    if (!ckpt_advanced)
    {
        ProfileEvents::increment(ProfileEvents::CASRefCheckpointNotAdvanced);
        std::lock_guard lock(rt->state_mutex);
        advancePublishBackoff(*rt);
        return false;
    }

    {
        std::lock_guard lock(rt->state_mutex);
        if (!runtime_still_admitted())
            return false;
        /// A durable publish clears any backoff: progress was made this attempt (even if the
        /// monotonic guard below skips the in-memory adoption because a newer snapshot already won).
        resetPublishBackoff(*rt);
        /// Monotonic adoption guard (CRITICAL): publishes are NOT serialized, so two
        /// overlapping attempts can finish out of order (this OLDER-candidate attempt landing its PUT
        /// after a NEWER one already adopted). Adopting the older `candidate_x` here would REGRESS
        /// `newest_snapshot_id` below what a newer attempt already advanced it to -- the next published
        /// snapshot would then omit committed transactions and recovery would lose refs. Skip the
        /// in-memory adoption (and the counter subtraction below) whenever a newer-or-equal snapshot is
        /// already adopted; the already-durable `_snap/<candidate_x>` object is harmless (readers pick
        /// the greatest snapshot, GC reclaims covered ones).
        if (rt->newest_snapshot_id && !(*rt->newest_snapshot_id < candidate_x))
            return true;
        /// Subtract exactly the counters captured at copy time
        /// -- more appends (or even another publish's own commits) may have landed on the LIVE counters
        /// since, and only those should remain uncovered. Clamped (see `clampedCounterSub`): an
        /// out-of-order adoption ordering the guard above does not catch (a SMALLER candidate that
        /// adopts before a LARGER one already in flight) could otherwise subtract an already-subtracted
        /// region and underflow the unsigned counter.
        clampedCounterSub(rt->tail_count_since_snapshot, captured_count);
        clampedCounterSub(rt->tail_bytes_since_snapshot, captured_bytes);
        /// logs-per-table-after-snapshot: the tail this publish compacted.
        ProfileEvents::increment(ProfileEvents::CASRefSnapshotTailLogs, captured_count);
        rt->newest_snapshot_id = candidate_x;
        /// The new cache-weight base is exactly the snapshot
        /// we just encoded and PUT, so its body size is the fresh base weight -- no re-encode needed.
        rt->base_snapshot_bytes.store(bytes.size(), std::memory_order_relaxed);
    }
    return true;
}

bool CasRefLedger::tryPublishSnapshotAndAdvanceCheckpointOnceOnRuntime(
    const RootNamespace & ns, const std::shared_ptr<RefTableRuntime> & rt, const DetachedStopToken & token)
{
    if (token.stopping())
        return false;
    return tryPublishSnapshotAndAdvanceCheckpointOnceOnRuntimeImpl(ns, rt, token);
}


void CasRefLedger::sweepStalePrecommitsForRead(const RootNamespace & ns, const std::shared_ptr<RefTableRuntime> & rt)
{
    /// A read-only caller (resolveRef/listRefs) must not fail its OWN
    /// otherwise-successful read because a piggybacked maintenance action (the stale-precommit sweep)
    /// hit an uncertain PUT -- the read asked for none of that; a mutation path (appendRefOps's own
    /// top-level hoisted call, which calls `maybeSweepStalePrecommits` directly, uncaught) keeps
    /// propagating instead, since it must not proceed past a wedged lane anyway. Swallowing here does
    /// Do NOT drop the sweep: the failed
    /// attempt already re-armed `needs_stale_precommit_sweep` (with a bounded cooldown) inside
    /// `maybeSweepStalePrecommits`, so a later read/mutation trigger on THIS mount retries until a
    /// sweep completes verified clean -- the old drop-the-shot behavior left a dead incarnation's
    /// precommit bindings (and the manifests they protect from the GC orphan sweep) live forever on a
    /// long-lived mount whenever the single attempt burned in the post-restart error window.
    try
    {
        maybeSweepStalePrecommits(ns, rt);
    }
    catch (...)
    {
        ProfileEvents::increment(ProfileEvents::CASRefSweepDeferred);
        tryLogCurrentException(getLogger("CasPool"),
            "CAS stale-precommit sweep deferred for namespace '" + ns.string()
                + "' (a read-only caller observed the failure and is proceeding with its own read)");
    }
}

void CasRefLedger::maybeSweepStalePrecommits(const RootNamespace & ns, const std::shared_ptr<RefTableRuntime> & rt)
{
    {
        std::lock_guard lock(rt->state_mutex);
        if (!rt->needs_stale_precommit_sweep)
            return;
        /// A failed attempt armed a cooldown; do not re-attempt (and do not touch the flag)
        /// until it elapses -- the bounded-backoff storm latch, same shape as `publish_backoff_until_ms`.
        /// The boottime clock is injectable (`boot_ms_fn`), so tests drive this deterministically.
        if (boot_ms_now_fn() < rt->precommit_sweep_backoff_until_ms)
            return;
        /// Cleared FIRST: `sweepStalePrecommitsNow`'s own `appendRefOps` calls re-enter this same
        /// top-level check (via `appendRefOps`'s hoisted call), and must see it already cleared. This
        /// clear is for RE-ENTRANCY only, never consumption: any non-clean outcome re-arms below.
        rt->needs_stale_precommit_sweep = false;
    }
    try
    {
        sweepStalePrecommitsNow(ns, rt);
    }
    catch (...)
    {
        /// A failed or partial sweep
        /// failed or partial sweep must NOT consume the shot. Under kill-chaos the single attempt lands
        /// exactly inside the post-restart error window (an uncertain PUT, a fence blip), and with no
        /// retry the dead incarnation's durable precommit bindings -- and the manifests
        /// `activeManifestKeys` protects for them -- leaked forever on a long-lived mount (GC has no
        /// backstop). Re-arm with a bounded backoff and rethrow: the
        /// read path insulates the caller (`sweepStalePrecommitsForRead`), the mutation path propagates
        /// as before.
        {
            std::lock_guard lock(rt->state_mutex);
            rt->needs_stale_precommit_sweep = true;
            advancePrecommitSweepBackoff(*rt);
        }
        throw;
    }
    /// Verified clean: `sweepStalePrecommitsNow` returns only after a full pass over the live state
    /// found zero stale bindings, so the flag stays cleared for the rest of this mount; reset the
    /// failure cooldown too.
    std::lock_guard lock(rt->state_mutex);
    resetPrecommitSweepBackoff(*rt);
}

void CasRefLedger::advancePrecommitSweepBackoff(RefTableRuntime & rt)
{
    /// Caller holds `rt.state_mutex`. Double the interval
    /// from `initial` up to `max` per consecutive failed sweep attempt; arm the deadline off the
    /// boottime clock (`bootMsNow`), so an injected test clock drives it deterministically and a
    /// VM-suspend cannot shorten it.
    rt.precommit_sweep_backoff_ms = rt.precommit_sweep_backoff_ms == 0
        ? config.precommit_sweep_backoff_initial_ms
        : std::min<uint64_t>(rt.precommit_sweep_backoff_ms * 2, config.precommit_sweep_backoff_max_ms);
    rt.precommit_sweep_backoff_until_ms = boot_ms_now_fn() + rt.precommit_sweep_backoff_ms;
    ProfileEvents::increment(ProfileEvents::CASRefSweepRearmed);
}

void CasRefLedger::resetPrecommitSweepBackoff(RefTableRuntime & rt)
{
    /// Caller holds `rt.state_mutex`. A verified-clean sweep clears the cooldown.
    rt.precommit_sweep_backoff_ms = 0;
    rt.precommit_sweep_backoff_until_ms = 0;
}


void CasRefLedger::sweepStalePrecommitsNow(const RootNamespace & ns, const std::shared_ptr<RefTableRuntime> & rt)
{
    /// After a fresh mount fence and recovery, this writer
    /// knows the exact stale precommit bindings -- their `manifest_ref.writer_epoch` predates this
    /// incarnation's live writer_epoch, i.e. they belong to a build from a superseded incarnation that
    /// can never be promoted. Removed with ordinary exact `owner_transition(old_binding, none)`
    /// operations, chunked to `ref_txn_max_ops` per transaction. Interruption is harmless: each chunk
    /// re-reads the LIVE state, so a partial sweep just leaves fewer stale bindings for the next chunk
    /// (a later retry on this mount, or the next mount's recovery) to find; nothing here can loop
    /// forever since only OLDER-epoch bindings ever qualify, and this writer's own new work always uses
    /// `live_epoch_fn()` -- which a self-remount bumps in lockstep with the threshold below, so a
    /// remount's fresh precommits survive.
    ///
    /// A GC-side backstop stays deliberately OUT: the responsibility boundary assigns
    /// precommit-binding cleanup to the WRITER -- GC never mutates another writer's ref-table state, so
    /// a leader-side reclaim would be a new protocol capability (a question about GC-authored
    /// ref-log transactions and their fencing), not a bugfix. The retry-until-clean loop above is the
    /// writer-side answer; the follow-up (a GC visibility counter for "live precommit binding below the
    /// mount-lease epoch" would require a separate protocol decision.
    const uint64_t live_epoch = live_epoch_fn();
    while (true)
    {
        std::vector<std::pair<String, ManifestRef>> chunk;
        {
            std::lock_guard lock(rt->state_mutex);
            for (const auto & [ref_name, mref] : rt->state.getPrecommits())
            {
                if (mref.writer_epoch >= live_epoch)
                    continue;
                chunk.emplace_back(ref_name, mref);
                if (chunk.size() >= ref_txn_max_ops)
                    break;
            }
        }
        if (chunk.empty())
            return;

        appendRefOps(ns, MutationScope::wholeShard(),
            [chunk](const RefTableState & state) -> std::vector<RefOp>
            {
                std::vector<RefOp> ops;
                for (const auto & [ref_name, mref] : chunk)
                    if (state.getPrecommits().contains({ref_name, mref}))
                    {
                        RefOp op;
                        op.kind = RefOpKind::OwnerTransition;
                        op.old_binding = RefOwnerBinding{RefOwnerKind::Precommit, ref_name, mref};
                        ops.push_back(op);
                    }
                return ops;
            },
            RootMutationOrigin::Writer, RootMutationKind::ReclaimPrecommit);

        /// Audit each binding this sweep reclaimed, so
        /// `system.cas_log` records the reclaim and the "abandoned precommits
        /// reclaimed" counter is falsifiable (it had ZERO emit sites before). A binding gathered above
        /// that is GONE from the live state after the committed append was reclaimed by this sweep's
        /// work -- either this chunk's own ops or this lane's just-resolved wedged predecessor txn (a
        /// PRIOR attempt of this same sweep whose ack was lost); one still present was skipped by the
        /// builder (raced by another owner transition) and will be gathered again next iteration.
        /// Collected under the lock, emitted outside it (the sink forwards to the SystemLog).
        std::vector<std::pair<String, ManifestRef>> reclaimed;
        {
            std::lock_guard lock(rt->state_mutex);
            for (const auto & [ref_name, mref] : chunk)
                if (!rt->state.getPrecommits().contains({ref_name, mref}))
                    reclaimed.emplace_back(ref_name, mref);
        }
        ProfileEvents::increment(ProfileEvents::CASRefStalePrecommitsReclaimed, reclaimed.size());
        for (const auto & [ref_name, mref] : reclaimed)
        {
            EventEmitter{*this}.emit([&](CasEvent & e)
            {
                e.type = CasEventType::PrecommitReclaim;
                e.namespace_ = ns.string();
                e.ref_name = ref_name;
                e.object_kind = CasEventObjectKind::Root;
                e.object_hash = manifestRefDebugString(mref);
                e.reason = "stale-precommit sweep: dangling precommit of a superseded writer incarnation "
                           "reclaimed by the successor's fenced sweep";
                e.detail = {{"stale_writer_epoch", std::to_string(mref.writer_epoch)},
                            {"live_writer_epoch", std::to_string(live_epoch)}};
            });
        }
    }
}


void CasRefLedger::dropRef(const RootNamespace & ns, const String & ref_name)
{
    /// One `owner_transition` removal ref-log transaction. The
    /// exact committed binding must exist; `build_ops` reads it off the CURRENT batch-validation state,
    /// so a concurrently-co-batched publish/drop of a DIFFERENT ref sees a consistent view.
    ManifestRef dropped_ref;
    const RefTxnId txn_id = appendRefOps(ns, MutationScope::ref(ref_name),
        [&](const RefTableState & state) -> std::vector<RefOp>
        {
            const auto it = state.getCommitted().find(ref_name);
            if (it == state.getCommitted().end())
                /// Fail-closed (no silent no-op): this item's own exception, the batch survives.
                throw Exception(ErrorCodes::FILE_DOESNT_EXIST,
                    "dropRef: no such ref {} in namespace {}", ref_name, ns.string());

            dropped_ref = it->second.manifest_ref;
            RefOp op;
            op.kind = RefOpKind::OwnerTransition;
            op.old_binding = RefOwnerBinding{RefOwnerKind::Committed, ref_name, dropped_ref};
            return {op};
        },
        RootMutationOrigin::Writer, RootMutationKind::Drop);

    /// The ref was dropped (a removal operation GC folds as a true removal). `object_hash` is the
    /// manifest the ref named, so a part's "publish -> drop" life is reconstructable from the rows.
    if (hasEventSink())
    {
        CasEvent _ev3;
        _ev3.type = CasEventType::RefDrop;
        _ev3.namespace_ = ns.string();
        _ev3.ref_name = ref_name;
        _ev3.object_kind = CasEventObjectKind::Manifest;
        _ev3.object_hash = manifestRefDebugString(dropped_ref);
        _ev3.at_version = txn_id.ref_sequence;
        _ev3.outcome = "ok";
        _ev3.reason = "dropRef: appended an owner_transition removal ref-log transaction";
        emitEvent(std::move(_ev3));
    }
}


void CasRefLedger::updateRefPublishedAt(const RootNamespace & ns, const String & ref_name,
                             std::function<void(RefPublishedAtUpdate &)> mutator)
{
    /// One `set_published_at` ref-log transaction. EVERY change (even timestamp-only) is an explicit
    /// logged operation -- the immutable append-only log has no other way to record it.
    /// `published_at_ms` is the only metadata this op carries (the mutable-file map is gone; every
    /// per-part file is an ordinary manifest tree entry now, republished via `repointRef`, never
    /// through this side channel).
    appendRefOps(ns, MutationScope::ref(ref_name),
        [&](const RefTableState & state) -> std::vector<RefOp>
        {
            const auto it = state.getCommitted().find(ref_name);
            if (it == state.getCommitted().end())
                throw Exception(ErrorCodes::FILE_DOESNT_EXIST,
                    "updateRefPublishedAt: no such ref {} in namespace {}", ref_name, ns.string());

            /// The mutator edits only `published_at_ms`; the carrier deliberately carries no
            /// `manifest_ref`, so a reachability change is structurally impossible here (it goes through
            /// publish/drop/repoint instead).
            RefPublishedAtUpdate update;
            update.published_at_ms = it->second.published_at_ms;

            mutator(update);

            return {makeSetPublishedAtOp(ref_name, it->second.manifest_ref, update.published_at_ms)};
        },
        RootMutationOrigin::Writer, RootMutationKind::UpdateRefPublishedAt);
}


NamespaceLifeId CasRefLedger::namespaceLife(const RootNamespace & ns)
{
    auto rt = lookupRefTableRuntime(ns);
    if (rt)
    {
        refuseUnlessAdmitted(mount_requests.resume(rt->admitted_fence_generation), "resident namespace life");
        bool removal_closed = false;
        bool removing_observed = false;
        bool write_may_land = false;
        {
            std::lock_guard queue_lock(ref_queue_mutex);
            removal_closed = rt->removal_admission_closed;
            removing_observed = rt->removing_observed;
            write_may_land = rt->removal_write_may_land;
        }
        if (removal_closed)
        {
            /// A lost erase response can leave only the detached predecessor's close bit. Reconcile
            /// before refusing so an absent/replaced row frees the logical name without rebinding it.
            CasOperation reconcile_op = mount_requests.resume(rt->admitted_fence_generation);
            reconcileCatalogCut(CasRefCatalog::read(reconcile_op, layout));
            const auto refreshed = lookupRefTableRuntime(ns);
            if (refreshed == rt)
            {
                /// Re-read: the reconcile may have latched the runtime and resolved a pending removal write.
                {
                    std::lock_guard queue_lock(ref_queue_mutex);
                    removing_observed = rt->removing_observed;
                    write_may_land = rt->removal_write_may_land;
                }
                if (write_may_land)
                    throwRemovalWriteMayLand(ns);
                if (removing_observed)
                    throwRemovingObserved(ns);
                throwCasWriteRetryLater(fmt::format(
                    "CAS namespace '{}' is Removing: creation waits for its terminal fold and catalog "
                    "removal to complete; retry later", ns.string()));
            }
            rt = refreshed;
        }
        if (rt)
        {
            std::exception_ptr recovery_error;
            /// A removed life is already detached by the recovery; the cold path below takes over.
            bool life_removed = false;
            try
            {
                life_removed = ensureRefTableRecovered(ns, *rt) == RefRecoveryOutcome::LifeRemoved;
            }
            catch (...)
            {
                recovery_error = std::current_exception();
            }
            bool terminal = false;
            if (!recovery_error && !life_removed)
            {
                std::lock_guard state_lock(rt->state_mutex);
                terminal = rt->state.getLifecycle() != RefLifecycle::Live && rt->state.getRemoveTxnId().has_value();
            }
            if (!recovery_error && !life_removed && !terminal)
                return rt->life;

            if (!life_removed)
            {
                /// Only the removal-closed branch above reconciles, and a GC on another server never
                /// invalidates here: a terminal life stays resident. Once the catalog no longer names
                /// it, detach it and go cold.
                bool row_gone = false;
                try
                {
                    CasOperation catalog_op = mount_requests.resume(rt->admitted_fence_generation);
                    const CasRefCatalog::Snapshot catalog = CasRefCatalog::read(catalog_op, layout);
                    refuseUnlessAdmitted(catalog_op, "resident namespace life, catalog check");
                    const std::optional<NamespaceLifeId> catalog_life = catalog.life_index.resolve(rt->life.incarnation);
                    row_gone = !catalog_life || *catalog_life != rt->life;
                }
                catch (...)
                {
                    if (recovery_error)
                        std::rethrow_exception(recovery_error);
                    throw;
                }
                if (!row_gone)
                {
                    if (recovery_error)
                        std::rethrow_exception(recovery_error);
                    refuseTerminalLife(ns, *rt);
                    return rt->life;
                }
                invalidateRemovedCatalogLife(rt->life);
            }
        }
    }

    /// A cold mutation observes or births the durable identity before allocating any local state.
    const uint64_t admitted_generation = fence_generation_fn();
    ColdAdmission admission = beginColdAdmission(ns);
    CasOperation op = mount_requests.resume(admitted_generation);
    const CasRefCatalog::Snapshot catalog = CasRefCatalog::read(op, layout);
    refuseUnlessAdmitted(op, "cold mutable namespace admission");
    const auto entry_it = std::find_if(catalog.catalog.entries.begin(), catalog.catalog.entries.end(),
        [&](const CatalogEntry & entry) { return entry.ns == ns; });
    if (entry_it != catalog.catalog.entries.end() && entry_it->state == NsState::Removing)
        throwRemovingObserved(ns);

    const NamespaceLifeId life
        = entry_it != catalog.catalog.entries.end() && entry_it->state == NsState::Live
        ? NamespaceLifeId::fromCatalogEntry(entry_it->ns, entry_it->incarnation)
        : resolveNamespaceLife(ns, admitted_generation, live_epoch_fn());
    refuseUnlessAdmitted(op, "cold mutable namespace admission, after life resolution");
    rt = acquireRefTableRuntime(life, /*observed_removing=*/false, admitted_generation, admission, RuntimeAdmission::Write);
    if (ensureRefTableRecovered(ns, *rt) == RefRecoveryOutcome::LifeRemoved)
        throwCasWriteRetryLater(recoveredLifeRemovedMessage(ns, rt->life));
    refuseTerminalLife(ns, *rt);
    return rt->life;
}

void CasRefLedger::refuseTerminalLife(const RootNamespace & ns, RefTableRuntime & rt)
{
    /// Bytes written under a removed life are swept by the dead-life janitor, so the caller must not be
    /// handed one to write into. A never-born stream is also `Removed` but has no `remove_txn_id`.
    std::lock_guard lock(rt.state_mutex);
    if (rt.state.getLifecycle() != RefLifecycle::Live && rt.state.getRemoveTxnId().has_value())
        throwCasWriteRetryLater(fmt::format(
            "CAS namespace '{}': its resident life {} is already removed; retry after the removal completes",
            ns.string(), renderIncarnation(rt.life.incarnation)));
}

std::optional<NamespaceLifeId> CasRefLedger::namespaceFilesLifeIfReadable(const RootNamespace & ns)
{
    const auto rt = acquireReadableRefTableRuntime(ns);

    /// A namespace the catalog does not name has no files to read, and a read or an unlink is the wrong
    /// event to bring one into existence on. Fresh resolution admits only a catalog `Live` row;
    /// `Creating`, `Removing` and absent all answer absent without recovery or mutation.
    if (!rt)
        return std::nullopt;
    if (ensureRefTableRecovered(ns, *rt) == RefRecoveryOutcome::LifeRemoved)
        return std::nullopt;

    std::lock_guard<std::mutex> lock(rt->state_mutex);
    /// A stale already-held runtime may have applied the terminal before catalog invalidation reaches
    /// it. Preserve the stated stale-or-not-found contract by hiding that terminal view.
    if (rt->state.getLifecycle() != RefLifecycle::Live && rt->state.getRemoveTxnId().has_value())
        return std::nullopt;
    return rt->life;
}

bool CasRefLedger::namespaceStillLogicallyPresent(const RootNamespace & ns)
{
    /// O(1) fast path: a resident runtime already proven `Live` under an unbroken fence answers without
    /// any catalog fetch -- the common case (`existsDirectory` on a warm, ordinary table). Anything
    /// short of that falls through to the exact cold-path observation below.
    if (const auto current = lookupRefTableRuntime(ns))
    {
        refuseUnlessAdmitted(mount_requests.resume(current->admitted_fence_generation), "resident namespace presence");
        bool closed = false;
        {
            std::lock_guard<std::mutex> queue_lock(ref_queue_mutex);
            closed = current->removal_admission_closed;
        }
        if (!closed
            && !current->catalog_life_invalidated.load(std::memory_order_acquire)
            && !current->superseded_by_remount.load(std::memory_order_acquire))
        {
            std::lock_guard<std::mutex> state_lock(current->state_mutex);
            if (current->recovered && current->state.getLifecycle() == RefLifecycle::Live)
                return true;
        }
    }

    /// Cold path: an exact catalog observation. `Creating` and `Live` both answer present immediately --
    /// `true` is always the safe direction, so no revalidation is needed for either. A missing row is
    /// the one answer that must never be manufactured by a race, so it alone is re-confirmed by a
    /// second read of this namespace's row before being trusted.
    const uint64_t admitted_generation = fence_generation_fn();
    ColdAdmission admission = beginColdAdmission(ns);
    CasOperation op = mount_requests.resume(admitted_generation);
    const CasRefCatalog::Snapshot first_catalog = CasRefCatalog::read(op, layout);
    refuseUnlessAdmitted(op, "namespace presence probe");
    if (namespace_presence_probe_after_first_read_hook_for_test)
        namespace_presence_probe_after_first_read_hook_for_test();
    const auto find_entry = [&ns](const CasRefCatalog::Snapshot & snap) -> const CatalogEntry *
    {
        const auto it = std::find_if(snap.catalog.entries.begin(), snap.catalog.entries.end(),
            [&](const CatalogEntry & entry) { return entry.ns == ns; });
        return it == snap.catalog.entries.end() ? nullptr : &*it;
    };

    const CatalogEntry * entry = find_entry(first_catalog);
    if (!entry)
    {
        /// The second read corroborates absence of THIS row only. Each read is one token-CAS'd full
        /// catalog value, so absence in two independent reads is a valid linearization at the second
        /// one. Requiring the WHOLE catalog to hold still between the reads starves under
        /// unrelated-namespace churn (the catalog is pool-global and a parallel workload mutates it
        /// continuously) while adding nothing to this row's proof. A row that appears in between
        /// answers present: `true` is always the safe direction, and the caller's next poll runs the
        /// full state dispatch against a fresh observation.
        const CasRefCatalog::Snapshot second_catalog = CasRefCatalog::read(op, layout);
        refuseUnlessAdmitted(op, "namespace presence probe");
        if (find_entry(second_catalog))
            return true;
        return false;   /// no catalog row in two atomic observations: proven absent
    }

    if (entry->state == NsState::Creating || entry->state == NsState::Live)
        return true;

    /// `Removing`: only the exact incarnation's own durable ref-log proves the terminal
    /// `remove_namespace` transaction actually landed -- the catalog row alone cannot distinguish a
    /// completed removal from one whose terminal append is still outstanding after a crash. This is the
    /// same load-bearing distinction `dropNamespaceImpl` makes before returning early. The recovery below
    /// is unbounded I/O, and GC may erase this incarnation's row and a same-name rebirth may land inside
    /// it. Neither a proven terminal nor a removed life therefore proves the name `ns` absent; the catalog
    /// read after the recovery decides for the name.
    const NamespaceLifeId life = NamespaceLifeId::fromCatalogEntry(entry->ns, entry->incarnation);
    const auto rt = acquireRefTableRuntime(
        life, entry->state == NsState::Removing, admitted_generation, admission, RuntimeAdmission::PresenceProbe);
    /// GC announced the erase of this row during the admission. Throwing would fail
    /// `DatabaseOnDisk::dropTable`, which calls `existsDirectory` after its rollback scope; `true` is safe.
    if (!rt)
        return true;
    /// A removed life is not an error here: the catalog read below decides for the name.
    const bool life_removed = ensureRefTableRecovered(ns, *rt) == RefRecoveryOutcome::LifeRemoved;
    if (!life_removed)
    {
        bool this_incarnation_terminal = false;
        {
            std::lock_guard<std::mutex> lock(rt->state_mutex);
            this_incarnation_terminal = rt->state.getLifecycle() != RefLifecycle::Live && rt->state.getRemoveTxnId().has_value();
        }
        if (!this_incarnation_terminal)
            return true;   /// removal admitted but not yet terminal: cleanup work remains

        if (namespace_presence_probe_after_terminal_proven_hook_for_test)
            namespace_presence_probe_after_terminal_proven_hook_for_test();
    }

    const CasRefCatalog::Snapshot post_terminal_catalog = CasRefCatalog::read(op, layout);
    refuseUnlessAdmitted(op, "namespace presence probe");
    const CatalogEntry * post_terminal_entry = find_entry(post_terminal_catalog);
    if (life_removed)
        return post_terminal_entry != nullptr;
    if (!post_terminal_entry || post_terminal_entry->incarnation == entry->incarnation)
        return false;   /// terminal durably proven and nothing has since occupied `ns` under a new life
    /// A successor incarnation now occupies `ns` -- `Creating`, `Live`, or a fresh `Removing` all mean
    /// the name is not the proven-absent predecessor this call observed. `true` is always the safe
    /// direction; a caller that acts on it retries against the successor's own (correct) state rather
    /// than being told the namespace is gone while something already occupies its name.
    return true;
}


DropNamespaceStats CasRefLedger::dropNamespace(const RootNamespace & ns)
{
    return dropNamespaceImpl(ns, std::nullopt);
}

DropNamespaceStats CasRefLedger::dropNamespaceImpl(
    const RootNamespace & ns, const std::optional<UInt128> & expected_incarnation)
{
    /// One body transaction naming an exact `owner_transition`
    /// removal for every committed ref and precommit, followed by `remove_namespace` -- the removal
    /// class shares the bigger complete-table byte budget (encodeRefLogTxn's own `checkBudget`, keyed
    /// off the presence of a `RemoveNamespace` op) and is exempt from the ordinary per-op admission
    /// check (it only ever shrinks state; see `flushRefBatch`'s `state_growing` filter).
    ColdAdmission admission = beginColdAdmission(ns);
    CasOperation initial_op = mount_requests.admit();
    /// One deadline for every request that leads to the removal decision. The wait for the lane to
    /// drain and for another removal attempt counts against it but is never cut short.
    const Retry policy = initial_op.freeze(Retry::standard());
    const CasRefCatalog::Snapshot initial_catalog = CasRefCatalog::read(initial_op, layout, policy);
    const auto initial_it = std::find_if(initial_catalog.catalog.entries.begin(), initial_catalog.catalog.entries.end(),
        [&](const CatalogEntry & entry) { return entry.ns == ns; });
    if (initial_it == initial_catalog.catalog.entries.end())
        return {};
    if (expected_incarnation && initial_it->incarnation != *expected_incarnation)
        throwCasWriteRetryLater(fmt::format(
            "CAS namespace '{}': exact removal life {} differs from current catalog life {}",
            ns.string(), renderIncarnation(*expected_incarnation), renderIncarnation(initial_it->incarnation)));

    if (initial_it->state == NsState::Creating)
    {
        const CatalogEntry & observed = *initial_it;
        CasOperation cancel_op = mount_requests.resume(fence_generation_fn());
        switch (CasRefCatalog::cancelStalledCreating(
            cancel_op, layout, observed,
            [&](const CreatorFence & creator)
            {
                return isCreatorFenceTerminal(
                    cancel_op, layout, creator.server_root_id, creator.writer_epoch, policy);
            },
            policy))
        {
            case CasRefCatalog::StalledCreatingCancelOutcome::Cancelled:
                invalidateRemovedCatalogLife(NamespaceLifeId::fromCatalogEntry(observed.ns, observed.incarnation));
                return {};
            case CasRefCatalog::StalledCreatingCancelOutcome::CreatorFenceStillLive:
                throwCasWriteRetryLater(fmt::format(
                    "CAS namespace removal '{}': its catalog entry is still Creating under a creator "
                    "fence that is not yet provably terminal; retry later", ns.string()));
            case CasRefCatalog::StalledCreatingCancelOutcome::EntryChanged:
                throwCasWriteRetryLater(fmt::format(
                    "CAS namespace removal '{}': exact Creating row changed before cancellation; retry later",
                    ns.string()));
            case CasRefCatalog::StalledCreatingCancelOutcome::FencedOut:
                throwCasWriteRetryLater(fmt::format(
                    "CAS namespace removal '{}': mount fence moved before stalled creation cancellation",
                    ns.string()));
        }
    }

    const NamespaceLifeId observed_life
        = NamespaceLifeId::fromCatalogEntry(initial_it->ns, initial_it->incarnation);
    const uint64_t runtime_generation = fence_generation_fn();
    const auto rt = acquireRefTableRuntime(
        observed_life, initial_it->state == NsState::Removing, runtime_generation, admission, RuntimeAdmission::Write);
    if (removal_runtime_acquired_hook_for_test)
        removal_runtime_acquired_hook_for_test();
    if (ensureRefTableRecovered(ns, *rt) == RefRecoveryOutcome::LifeRemoved)
        return {};   /// as for a name without a row
    {
        std::unique_lock<std::mutex> queue_lock(ref_queue_mutex);
        rt->cv.wait(queue_lock, [&]
        {
            return !rt->removal_attempt_active;
        });
        rt->removal_attempt_active = true;
    }
    SCOPE_EXIT({
        std::lock_guard<std::mutex> queue_lock(ref_queue_mutex);
        rt->removal_attempt_active = false;
        rt->cv.notify_all();
    });
    {
        /// A real terminal transaction is idempotent. The representation also calls an empty, never-born
        /// stream `Removed`, but its absent `remove_txn_id` distinguishes that state: a cataloged life may
        /// already own `_ckpt` or `_files`, so it still needs durable birth+terminal evidence before GC
        /// may delete its catalog row.
        std::lock_guard lock(rt->state_mutex);
        if (rt->state.getLifecycle() != RefLifecycle::Live && rt->state.getRemoveTxnId().has_value())
            return {};
    }

    /// Close the local positive lane BEFORE publishing `Removing`. Calls already admitted ahead of
    /// this point drain first; later positive callers observe the flag in the same queue critical
    /// section as enqueue and receive the typed retry-later refusal. The terminal item below is the one
    /// deliberate exception.
    {
        std::unique_lock<std::mutex> queue_lock(ref_queue_mutex);
        rt->removal_admission_closed = true;
        rt->cv.wait(queue_lock, [&]
        {
            return !rt->leader_active && rt->pending.empty();
        });
    }

    if (removal_attempt_hook_for_test)
        removal_attempt_hook_for_test();

    /// Admitted under the runtime this drop closed and drained, not under the current generation: after a
    /// remount another runtime of this name may be open, and `Removing` must not land under it.
    const auto runtime_live = [&rt]
    {
        return !rt->catalog_life_invalidated.load(std::memory_order_acquire)
            && !rt->superseded_by_remount.load(std::memory_order_acquire);
    };
    CasOperation removal_op = mount_requests.resume(rt->admitted_fence_generation, runtime_live);
    std::optional<CatalogEntry> observed_live;
    if (initial_it->state == NsState::Live)
        observed_live = *initial_it;
    bool removing_durable = false;
    bool removal_write_outcome_unknown = false;
    try
    {
        refuseUnlessAdmitted(removal_op, "beginning removal");
        const CasRefCatalog::Snapshot snapshot = CasRefCatalog::read(removal_op, layout, policy);
        const auto entry_it = std::find_if(snapshot.catalog.entries.begin(), snapshot.catalog.entries.end(),
            [&](const CatalogEntry & entry) { return entry.ns == ns; });
        if (entry_it == snapshot.catalog.entries.end())
            throwCasWriteRetryLater(fmt::format(
                "CAS namespace '{}': its catalog row disappeared before removal admission closed",
                ns.string()));

        const NamespaceLifeId & life = rt->life;
        if (entry_it->incarnation != life.incarnation)
            throwCasWriteRetryLater(fmt::format(
                "CAS namespace '{}': cached life {} differs from catalog life {} while beginning removal",
                ns.string(), renderIncarnation(life.incarnation), renderIncarnation(entry_it->incarnation)));

        if (entry_it->state == NsState::Removing)
        {
            removing_durable = true;   /// retry after an earlier terminal append failure/ambiguous CAS
        }
        else if (entry_it->state == NsState::Live)
        {
            observed_live = *entry_it;
            uint64_t removal_started_round = 0;
            if (const auto got = removal_op.read(layout.gcStateKey(), policy))
                removal_started_round = decodeGcState(got->bytes).round;

            /// The flag turns on at the first sent attempt of the `Live -> Removing` write. Before it nothing
            /// can land, so a failure may reopen the lane. After it every exception, `FencedOut` and
            /// `EntryChanged` may follow an attempt that can still land.
            removal_op.observeRequests(RequestObserver{
                .on_write_attempt = [&removal_write_outcome_unknown](uint32_t) { removal_write_outcome_unknown = true; },
                .on_failure = {},
            });
            switch (CasRefCatalog::beginRemoving(
                removal_op, layout, *observed_live, removal_started_round, policy))
            {
                case CasRefCatalog::BeginRemovingOutcome::Transitioned:
                case CasRefCatalog::BeginRemovingOutcome::AlreadyRemoving:
                    removal_write_outcome_unknown = false;
                    removing_durable = true;
                    break;
                case CasRefCatalog::BeginRemovingOutcome::EntryChanged:
                    throwCasWriteRetryLater(fmt::format(
                        "CAS namespace '{}': its exact catalog row changed while beginning removal",
                        ns.string()));
                case CasRefCatalog::BeginRemovingOutcome::FencedOut:
                    throwCasWriteRetryLater(fmt::format(
                        "CAS namespace '{}': the mount fence moved while beginning removal",
                        ns.string()));
            }
        }
        else
        {
            throwCasWriteRetryLater(fmt::format(
                "CAS namespace '{}': its catalog row is Creating while removal owns a recovered life",
                ns.string()));
        }
    }
    catch (...)
    {
        /// Resolve an ambiguous transition before deciding whether this lane may reopen. `Removing`
        /// under the same life is conclusive success. Reopening also needs a fresh exact `Live` row, the
        /// same mount fence and no `Removing` observation of this runtime, which may land after the read;
        /// every other case stays closed and propagates the original error.
        try
        {
            /// A fresh standard window, not `policy`: an expired policy would fail this read at once and
            /// leave a healthy table closed.
            const CasRefCatalog::Snapshot fresh = CasRefCatalog::read(removal_op, layout);
            if (removal_reopen_read_hook_for_test)
                removal_reopen_read_hook_for_test();
            const auto fresh_it = std::find_if(fresh.catalog.entries.begin(), fresh.catalog.entries.end(),
                [&](const CatalogEntry & entry) { return entry.ns == ns; });
            if (observed_live
                && fresh_it != fresh.catalog.entries.end()
                && fresh_it->incarnation == observed_live->incarnation
                && fresh_it->state == NsState::Removing)
            {
                removing_durable = true;
            }
            else if (observed_live && fresh_it != fresh.catalog.entries.end() && *fresh_it == *observed_live)
            {
                refuseUnlessAdmitted(removal_op, "reopening the removal lane");
                std::lock_guard<std::mutex> queue_lock(ref_queue_mutex);
                if (!removal_write_outcome_unknown && !rt->removal_write_may_land && !rt->removing_observed)
                {
                    rt->removal_admission_closed = false;
                    rt->cv.notify_all();
                }
            }
        }
        catch (...) // NOLINT(bugprone-empty-catch)
        {
            /// The original failure remains the caller-visible one. Failure to prove an exact fresh
            /// `Live` row deliberately leaves admission closed.
        }
        if (!removing_durable)
        {
            /// One read showing `Live` does not prove that a sent write never lands, so after an unknown
            /// outcome the lane stays closed until GC erases the row or a remount replaces the runtime.
            /// A retried `DROP` completes the removal.
            if (removal_write_outcome_unknown)
            {
                {
                    std::lock_guard<std::mutex> queue_lock(ref_queue_mutex);
                    rt->removal_write_may_land = true;
                }
                LOG_WARNING(getLogger("CasRefLedger"),
                    "CAS namespace '{}': the outcome of its Live -> Removing catalog write is unknown; writes "
                    "stay refused until a retried removal completes or the disk is remounted", ns.string());
            }
            throw;
        }
    }

    chassert(removing_durable);

    {
        std::lock_guard<std::mutex> queue_lock(ref_queue_mutex);
        rt->removal_write_may_land = false;
    }

    /// This call's own removal
    /// transaction named, filled from the SAME `state` the ops below are built from -- a retried
    /// `build_ops` (a wedge resolving under a resumed leader) simply overwrites it with the final
    /// durable transaction's true counts.
    DropNamespaceStats stats;
    appendRefOpsOnRuntime(ns, rt, MutationScope::wholeShard(),
        [&](const RefTableState & state) -> std::vector<RefOp>
        {
            if (state.getLifecycle() != RefLifecycle::Live && state.getRemoveTxnId().has_value())
                return {};   /// raced: another caller already removed it since our check above

            std::vector<RefOp> ops;
            if (state.getLifecycle() != RefLifecycle::Live)
            {
                RefOp birth;
                birth.kind = RefOpKind::NamespaceBirth;
                ops.push_back(std::move(birth));
            }
            for (const auto [ref_name, row] : state.getCommitted())
            {
                RefOp op;
                op.kind = RefOpKind::OwnerTransition;
                op.old_binding = RefOwnerBinding{RefOwnerKind::Committed, ref_name, row.manifest_ref};
                ops.push_back(op);
            }
            for (const auto & [ref_name, mref] : state.getPrecommits())
            {
                RefOp op;
                op.kind = RefOpKind::OwnerTransition;
                op.old_binding = RefOwnerBinding{RefOwnerKind::Precommit, ref_name, mref};
                ops.push_back(op);
            }
            RefOp remove;
            remove.kind = RefOpKind::RemoveNamespace;
            ops.push_back(remove);

            stats.committed_refs = state.getCommitted().size();
            stats.precommits = state.getPrecommits().size();
            return ops;
        },
        RootMutationOrigin::Writer, RootMutationKind::DropNamespace,
        /// The operations above already name (and remove) every current precommit
        /// binding regardless of epoch, making the ordinary stale-precommit maintenance sweep redundant
        /// for THIS call -- and, left enabled, a race: the hoisted sweep runs first and would reclaim an
        /// epoch-stale binding in its OWN transaction, so `state.getPrecommits()` above would already be
        /// missing it and undercount `stats.precommits`. See `appendRefOps`'s doc comment.
        /*skip_stale_precommit_sweep=*/true,
        /*terminal_removal_authorized=*/true);

    /// "After the transaction is durable, it applies the same
    /// operations to memory, cancels local builds, and rejects further ordinary mutations." Reaching here
    /// means the removal is durable (this call's, or a concurrent caller's whose durable result the append
    /// lane observed) -- a FAILED append would have thrown above, so cancellation is only ever reached
    /// after durability (a failed append leaves the namespace `Removing`, not `Live`, and propagates;
    /// the catalog CAS above already made that transition durable before this append was attempted).
    /// Cancel
    /// every in-flight build TARGETING this namespace so its next op fails closed (`requireAlive`),
    /// preventing it from promoting/precommitting a fresh owner into (or staging more debris in) the
    /// just-removed namespace. The append lane is the real linearization authority (an `owner_transition`
    /// on a non-Live namespace is rejected by the state machine regardless); this stops wasted work early
    /// and surfaces a clear error. Builds in OTHER namespaces self-filter (no-op). The build registry
    /// (`inflight_builds`) lives on the owning Pool, so the cancellation runs through the injected
    /// `cancel_inflight_builds` callback (which collects the live shared_ptrs under `builds_mutex` and
    /// cancels OUTSIDE it -- see `Pool::cancelInflightBuildsForNamespace`).
    cancel_inflight_builds(ns);

    /// No background publisher may carry the old runtime across the later catalog deletion and its
    /// in-place reset. A removal has already succeeded here, so the cached lifecycle is no longer Live
    /// and settlement cannot re-dispatch another publisher.
    {
        std::unique_lock state_lock(rt->state_mutex);
        rt->publish_settle_cv.wait(state_lock, [&]
        {
            return rt->pending_snapshot_publishes.load(std::memory_order_acquire) == 0;
        });
    }

    /// The writer performs no physical deletion of ref-log/snapshot objects or namespace files. Once
    /// the catalog row is drained, those objects are dead-life debris for the perpetual janitor.
    return stats;
}

DropNamespaceStats CasRefLedger::dropNamespace(const NamespaceLifeId & life)
{
    return dropNamespaceImpl(life.ns, life.incarnation);
}


}
