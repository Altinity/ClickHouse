#include <gtest/gtest.h>

#include "config.h"

#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Backend/CasInMemoryBackend.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Backend/CasRequestControl.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Formats/CasRefLogFormat.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Formats/CasTextFormat.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasPartWriteTxn.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasPool.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasRefProtocol.h>
#include <Disks/tests/cas_test_helpers.h>
#include <Common/Exception.h>
#include <Common/MemoryTracker.h>
#include <Common/ProfileEvents.h>

#include <Poco/Exception.h>

#include <atomic>
#include <condition_variable>
#include <functional>
#include <mutex>
#include <optional>
#include <thread>

/// ================================================================================================
/// Task 4 (2026-07-28 CAS ref-chain Stage A streams, spec INV-1's every-attempt rule + INV-2's seal):
/// the writer wedge.
///
/// An id is freed only when NOTHING WAS SENT, or when every sent attempt has its own CONCLUSIVE
/// rejection. That is what these tests are about, and the second half is the part that changed: an
/// ambiguous attempt used to be resolved by a bare exact GET, which can only ever report "absent" --
/// and absent is not a rejection, because the ambiguous attempt may still land afterwards. The lane
/// therefore stayed wedged FOREVER over a key nothing had written. The rule now runs one bounded
/// `slotOccupy` per later caller's flush: the ref-log key is write-once, so a conditional CREATE of
/// the SAME bytes either makes the transaction durable (adopt it) or conflicts with whatever is
/// there, which the follow-up read then names -- our own earlier write (adopt), a successor's
/// `EpochSeal` (the operation is conclusively rejected and never was acked), or a foreign object
/// (impossible under mount-lease exclusivity: fail loud).
///
/// Two cross-cutting rules are exercised throughout rather than in one place:
///   - the ADMISSION FENCE: a wedge carries the mount-fence generation it was admitted under, every
///     retry is gated on THAT generation (never the current one), and every result is re-checked
///     under `state_mutex` before anything acts on it. A result that returns after a fence
///     bump/re-arm, or after the wedge it belonged to was replaced, must be INERT.
///   - `prev_epoch_seal`: a seal observed at the wedged key IS this namespace's epoch-closing record,
///     so it becomes the `prev_epoch_seal` the next sequence-1 append carries. `nullopt` means
///     genesis, and means it exactly.
/// ================================================================================================

namespace ProfileEvents
{
extern const Event CasRefAppendSealRejected;
extern const Event CasRefAppendOccupantUnreadable;
extern const Event CasRefAppendWedged;
extern const Event CasRefAppendDefiniteFailure;
}

namespace DB::ErrorCodes
{
extern const int BAD_ARGUMENTS;
extern const int CORRUPTED_DATA;
extern const int INVALID_STATE;
extern const int MEMORY_LIMIT_EXCEEDED;
extern const int NETWORK_ERROR;
}

using namespace DB::Cas;
using DB::Cas::tests::CountingBackend;
using DB::Cas::tests::LandedButAckLostOnceBackend;
using DB::Cas::tests::expectThrowsCode;

namespace
{

PoolPtr openPool(const BackendPtr & backend, CasRequestBudget budget = {})
{
    DB::Cas::tests::seedPoolMetaForRestart(*backend);
    return Pool::open(backend, PoolConfig{.pool_prefix = "p", .server_root_id = "test", .cas_request_budget = budget});
}

/// The budget every wedge test uses: ONE attempt, so a single injected ambiguity is the whole
/// operation and the lane wedges deterministically instead of retrying its way out.
CasRequestBudget singleAttemptBudget()
{
    CasRequestBudget budget;
    budget.max_attempts = 1;
    budget.attempt_timeout_ms = 100;
    budget.operation_deadline_ms = 5000;   /// strictly above attempt_timeout_ms: equality is a wall-clock race (validateCasRequestBudget)
    budget.lease_safety_margin_ms = 100;
    return budget;
}

/// TWO attempts of one logical operation, with the inter-attempt backoff disabled. Everything about the
/// call-level verdict rule lives BETWEEN two attempts of a single call, so it cannot be reached with the
/// one-attempt budget the tests above use; the backoff is switched off because the schedule is
/// `gtest_cas_request_control.cpp`'s subject and a real sleep here would only slow the suite.
///
/// `[[maybe_unused]]`: both of its callers need a real S3-classified rejection to script their second
/// attempt, so they compile away entirely in a build without S3.
[[maybe_unused]] CasRequestBudget twoAttemptBudget()
{
    CasRequestBudget budget = singleAttemptBudget();
    budget.max_attempts = 2;
    budget.retry_initial_backoff_ms = 0;
    budget.retry_max_backoff_ms = 0;
    return budget;
}

PartWriteTxnPtr startBuildFor(const PoolPtr & s, const RootNamespace & ns, const String & ref)
{
    PartWriteInfo info;
    info.intended_namespace = ns;
    info.intended_ref = ns.string() + "/" + ref;
    return s->beginPartWrite(info);
}

void publishEmptyPart(const PoolPtr & s, const RootNamespace & ns, const String & ref)
{
    auto build = startBuildFor(s, ns, ref);
    const ManifestId id = build->stageManifest({});
    build->precommitAdd(ns, ref, id);
    build->promote(ns, ref, build->buildId(), id);
}

/// A `CountingBackend` with the exact seams these tests need, all keyed by substring so a whole Pool's
/// bootstrap traffic never consumes a fault meant for a `_log/` PUT.
class WedgeTestBackend : public CountingBackend
{
public:
    using CountingBackend::putIfAbsent;
    using CountingBackend::get;

    /// One-shot ambiguity that writes NOTHING: the response is lost and the key stays absent, which is
    /// the input that makes a later `slotOccupy` report `Created`.
    String ambiguous_substr;
    int ambiguous_count = 0;

    /// One-shot DETERMINISTIC LOCAL failure (`BAD_ARGUMENTS`, in `isDeterministicLocalFailure`'s set),
    /// which `slotOccupy` rethrows unchanged -- a definite refusal of THIS attempt. Portable stand-in
    /// for the S3 `DefiniteFailure` shape, which needs `USE_AWS_S3`; both are "proven never applied".
    String definite_substr;
    int definite_count = 0;

    /// One-shot WHITELISTED SYNCHRONOUS REJECTION: the ONLY shape `classifyConditionalWriteResult`
    /// answers `DefiniteFailure` for, and therefore the only way to drive the append lane's definite
    /// arm. Distinct from `definite_substr` above on purpose -- that one is a deterministic LOCAL
    /// failure, which `slotOccupy` rethrows but `putIfAbsentControlled` (no such special case) merely
    /// classifies Unresolved, so it cannot script this arm at all.
    String s3_definite_substr;
    int s3_definite_count = 0;

    /// A SUCCESSOR lands `conflict_bytes` at the key and only then is our response lost, so the
    /// controller's resolve-before-reissue reads a different object and proves the conflict. This is how
    /// the ordinary append site meets an occupant at the id it derived.
    String conflict_substr;
    int conflict_count = 0;
    String conflict_bytes;

    /// Fail GETs of matching keys after skipping the first `fail_get_skip` of them -- the resolve read
    /// that PROVES the conflict must succeed, so only the adjudication read that follows it is faulted.
    String fail_get_substr;
    int fail_get_skip = 0;
    int fail_get_count = 0;

    std::optional<GetResult> get(const String & key, Range range) override
    {
        if (fail_get_count > 0 && !fail_get_substr.empty() && key.find(fail_get_substr) != String::npos)
        {
            if (fail_get_skip > 0)
                --fail_get_skip;
            else
            {
                --fail_get_count;
                throw Poco::TimeoutException("WedgeTestBackend: simulated lost GET (read response never arrived)");
            }
        }
        return CountingBackend::get(key, range);
    }

    /// Park a matching PUT until `releaseBlock()`, notifying `awaitBlockEntered()` on arrival, so a
    /// test can drive a fence bump or a successor's write into the exact I/O window.
    void armBlock(const String & substr)
    {
        std::lock_guard g(block_mutex);
        block_substr = substr;
        block_armed = true;
        block_entered = false;
    }
    void awaitBlockEntered()
    {
        std::unique_lock lk(block_mutex);
        block_cv.wait(lk, [&] { return block_entered; });
    }
    void releaseBlock()
    {
        {
            std::lock_guard g(block_mutex);
            block_armed = false;
        }
        block_cv.notify_all();
    }

    /// Write straight through, bypassing every fault and block seam above -- how a test models what a
    /// SUCCESSOR (another process entirely) put at a key. Using the faulting entry point instead would
    /// park the test's own write on the very gate it is trying to drive a scenario through. The
    /// qualification must name the THREE-argument overload: `Backend`'s two-argument convenience
    /// forwards to the VIRTUAL one, so `CountingBackend::putIfAbsent(key, bytes)` would dispatch right
    /// back into the override above and deadlock the test against its own block.
    PutResult putAsSuccessor(const String & key, const String & bytes)
    {
        return CountingBackend::putIfAbsent(key, bytes, ObjectMeta{});
    }

    PutResult putIfAbsent(const String & key, const String & bytes, const ObjectMeta & meta) override
    {
        if (ambiguous_count > 0 && !ambiguous_substr.empty() && key.find(ambiguous_substr) != String::npos)
        {
            --ambiguous_count;
            throw Poco::TimeoutException("WedgeTestBackend: simulated ambiguous PUT (response lost, nothing landed)");
        }
        if (definite_count > 0 && !definite_substr.empty() && key.find(definite_substr) != String::npos)
        {
            --definite_count;
            throw DB::Exception(DB::ErrorCodes::BAD_ARGUMENTS, "WedgeTestBackend: scripted deterministic local failure");
        }
        /// AFTER the ambiguity seam, so arming both scripts one call's attempts in order: the first
        /// attempt goes ambiguous, the reissue is definitively refused.
        if (s3_definite_count > 0 && !s3_definite_substr.empty() && key.find(s3_definite_substr) != String::npos)
        {
            --s3_definite_count;
#if USE_AWS_S3
            throw DB::S3Exception("WedgeTestBackend: simulated malformed request",
                                  Aws::S3::S3Errors::UNKNOWN, "MalformedXML");
#else
            throw DB::Exception(DB::ErrorCodes::CORRUPTED_DATA,
                "WedgeTestBackend: DefiniteFailure requires S3 error classification (USE_AWS_S3 off)");
#endif
        }
        if (conflict_count > 0 && !conflict_substr.empty() && key.find(conflict_substr) != String::npos)
        {
            --conflict_count;
            CountingBackend::putIfAbsent(key, conflict_bytes, meta);
            throw Poco::TimeoutException("WedgeTestBackend: a successor's object landed; our response was lost");
        }
        {
            std::unique_lock lk(block_mutex);
            if (block_armed && !block_substr.empty() && key.find(block_substr) != String::npos)
            {
                block_entered = true;
                block_cv.notify_all();
                /// Bounded so a wiring bug bounds the wait instead of hanging the suite.
                block_cv.wait_for(lk, std::chrono::seconds(20), [&] { return !block_armed; });
            }
        }
        return CountingBackend::putIfAbsent(key, bytes, meta);
    }

private:
    std::mutex block_mutex;
    std::condition_variable block_cv;
    String block_substr;
    bool block_armed = false;
    bool block_entered = false;
};

/// The `_log/` key prefix of one namespace -- what every fault seam here matches on.
String logPrefix(const PoolPtr & store, const RootNamespace & ns)
{
    return store->layout().refsNamespacePrefix(NamespaceLifeId::stageATransition(ns)) + "_log/";
}

/// Decode the ref-log object at `id`, through the SAME codec the writer's recovery uses (never a
/// hand-rolled parse), so an assertion about `prev_epoch_seal` is an assertion about the WIRE.
RefLogTxn readRefLogTxn(Backend & backend, const Layout & layout, const RootNamespace & ns, const RefTxnId & id)
{
    const auto got = backend.get(layout.refLogKey(NamespaceLifeId::stageATransition(ns), id));
    if (!got)
        throw DB::Exception(DB::ErrorCodes::CORRUPTED_DATA, "no ref-log object at {}-{}", id.writer_epoch, id.ref_sequence);
    return decodeRefLogTxn(openObject(FormatId::RefLog, got->bytes), ns.string(), id);
}

/// The bytes of a real `EpochSeal` transaction closing `id.writer_epoch` at `id` -- what a SUCCESSOR
/// writes into the dead epoch's next slot (spec INV-2). Grammar: exactly one `EpochSeal` op, and
/// `prev_epoch_seal` on sequence 1 only, so callers pass it exactly when `id.ref_sequence == 1`.
String epochSealBytes(const RootNamespace & ns, const RefTxnId & id, std::optional<RefTxnId> prev_epoch_seal = std::nullopt)
{
    RefOp op;
    op.kind = RefOpKind::EpochSeal;
    const RefLogTxn txn{ns.string(), id, {op}, prev_epoch_seal};
    return sealObject(FormatId::RefLog, encodeRefLogTxn(txn));
}

/// Re-arm the mount fence WITHOUT a self-remount: the generation moves (twice -- trip, then re-arm)
/// while the cached runtime survives, which is what isolates the generation check as the sole
/// detector. A real self-remount also quiesces the runtimes; that path is Task 6's.
void bumpFenceGeneration(const PoolPtr & store, uint64_t writer_epoch)
{
    store->tripMountLost();
    store->armMountFence(DB::UInt128{0, 1}, writer_epoch, store->bootMsNow() + 600000);
    /// The fence re-arm alone moves the GENERATION; the live incarnation's writer epoch is a separate
    /// publication (`tryRemountOnce` does both), and the append lane derives its ids from that one.
    store->setLiveWriterEpochForTest(writer_epoch);
}

/// Arm a one-shot throw inside the post-durable install regions. The exception is built OUTSIDE the
/// region (building it inside would trip `DENY_ALLOCATIONS_IN_SCOPE` and test the guard instead), and
/// `MEMORY_LIMIT_EXCEEDED` is what a real tracked allocation failure raises. Same shape as
/// `gtest_cas_ref_install_safety.cpp`'s helper.
void armOneShotInstallFailure(const PoolPtr & store)
{
    auto planned = std::make_exception_ptr(DB::Exception(DB::ErrorCodes::MEMORY_LIMIT_EXCEEDED,
        "simulated allocation failure inside the post-durable install region"));
    auto fired = std::make_shared<std::atomic<bool>>(false);
    store->setInstallRegionProbeForTest([planned, fired]
    {
        if (fired->exchange(true))
            return;
        ALLOW_ALLOCATIONS_IN_SCOPE;
        std::rethrow_exception(planned);
    });
}

}

/// ===================================================================================
/// The every-attempt rule: an ambiguous attempt is resolved by a bounded CREATE, not a read
/// ===================================================================================

/// The headline change. Nothing landed, so the old bare-GET resolution reported "absent" forever and
/// the lane never recovered without a remount. One conditional create of the SAME bytes settles it:
/// the object becomes durable and the wedged transaction is adopted -- applied EXACTLY once, before
/// the flush that resolved it allocates any new id.
TEST(CasRefWedgeEveryAttempt, AmbiguousPutWedgesTheLaneAndTheNextFlushsCreateAdoptsItExactlyOnce)
{
    auto backend = std::make_shared<WedgeTestBackend>();
    auto store = openPool(backend, singleAttemptBudget());
    const RootNamespace ns{"srv1/wedge_created"};
    publishEmptyPart(store, ns, "x");
    publishEmptyPart(store, ns, "y");

    backend->ambiguous_substr = logPrefix(store, ns);
    backend->ambiguous_count = 1;

    expectThrowsCode(DB::ErrorCodes::NETWORK_ERROR, [&] { store->dropRef(ns, "x"); });
    ASSERT_TRUE(store->refLaneWedgedForTest(ns));
    ASSERT_TRUE(store->resolveRef(ns, "x").has_value()) << "a wedged transaction is not applied";
    const String wedged_key = store->wedgedKeyForTest(ns);
    ASSERT_FALSE(backend->get(wedged_key).has_value()) << "the ambiguous attempt wrote nothing";
    const size_t tail_before = store->tailSinceSnapshotCountForTest(ns);

    /// The next caller's flush resolves the wedge with ONE create, adopts it, and only then carves and
    /// commits its own transaction.
    EXPECT_NO_THROW(store->dropRef(ns, "y"));

    EXPECT_FALSE(store->refLaneWedgedForTest(ns));
    EXPECT_FALSE(store->resolveRef(ns, "x").has_value()) << "the adopted wedge applied its drop";
    EXPECT_FALSE(store->resolveRef(ns, "y").has_value()) << "the resolving flush committed its own drop";
    EXPECT_TRUE(backend->get(wedged_key).has_value()) << "the wedged transaction is durable at its own key";
    EXPECT_EQ(store->tailSinceSnapshotCountForTest(ns), tail_before + 2)
        << "the adopted wedge and the ordinary commit must each join the tail exactly once";
    EXPECT_EQ(store->laneStateForTest(ns), RefLaneState::Ready);
}

/// The other adoption input, and the one that proves the identity rule is about BYTES: our own
/// earlier attempt DID land (only its ack, and the controller's own resolve read, were lost). The
/// retry's create conflicts with our own object, the follow-up read returns bytes equal to the
/// wedge's, and the transaction is adopted -- ONCE, not once per attempt.
TEST(CasRefWedgeEveryAttempt, OwnLandedAttemptIsAdoptedFromOccupiedWithoutDoubleApply)
{
    auto backend = std::make_shared<LandedButAckLostOnceBackend>();
    /// Disarmed while the fixture is built: the one-shot fault matches ANY key until a substring is
    /// set, and the pool's own bootstrap PUT would otherwise consume it.
    backend->fired = true;
    auto store = openPool(backend, singleAttemptBudget());
    const RootNamespace ns{"srv1/wedge_occupied_mine"};
    publishEmptyPart(store, ns, "x");
    publishEmptyPart(store, ns, "y");

    backend->key_substr = logPrefix(store, ns);
    backend->lose_resolve_read = true;
    backend->fired = false;   /// armed: the next `_log/` PUT lands and loses its ack

    expectThrowsCode(DB::ErrorCodes::NETWORK_ERROR, [&] { store->dropRef(ns, "x"); });
    ASSERT_TRUE(store->refLaneWedgedForTest(ns));
    const String wedged_key = store->wedgedKeyForTest(ns);
    ASSERT_TRUE(backend->get(wedged_key).has_value()) << "this fault LANDS the write; only the ack was lost";
    ASSERT_TRUE(store->resolveRef(ns, "x").has_value()) << "durable, but not applied while wedged";
    const size_t tail_before = store->tailSinceSnapshotCountForTest(ns);
    const uint64_t puts_before = backend->putCount(wedged_key);

    EXPECT_NO_THROW(store->dropRef(ns, "y"));

    EXPECT_FALSE(store->refLaneWedgedForTest(ns));
    EXPECT_FALSE(store->resolveRef(ns, "x").has_value()) << "the landed transaction is adopted on resolution";
    EXPECT_EQ(store->tailSinceSnapshotCountForTest(ns), tail_before + 2)
        << "adopted exactly once: a double-apply would bump the tail twice for one transaction";
    EXPECT_EQ(backend->putCount(wedged_key), puts_before + 1)
        << "the resolution costs exactly ONE conditional create at the wedged key";
}

/// `ambiguous-then-definite`, the control the phase-0 model singles out: a definite refusal of a LATER
/// attempt says nothing about the EARLIER ambiguous one, which may still be in flight. The lane must
/// stay wedged -- unwedging here is how an acked-then-lost transaction gets written around.
TEST(CasRefWedgeEveryAttempt, DefiniteRefusalOfARetryAttemptKeepsTheLaneWedged)
{
    auto backend = std::make_shared<WedgeTestBackend>();
    auto store = openPool(backend, singleAttemptBudget());
    const RootNamespace ns{"srv1/wedge_ambiguous_then_definite"};
    publishEmptyPart(store, ns, "x");
    publishEmptyPart(store, ns, "y");

    backend->ambiguous_substr = logPrefix(store, ns);
    backend->ambiguous_count = 1;
    expectThrowsCode(DB::ErrorCodes::NETWORK_ERROR, [&] { store->dropRef(ns, "x"); });
    ASSERT_TRUE(store->refLaneWedgedForTest(ns));
    const String wedged_key = store->wedgedKeyForTest(ns);
    const RefTxnId wedged_id = store->layout().parseRefObjectKey(wedged_key)->txn_id;

    /// The retry's own create is definitively refused.
    backend->definite_substr = logPrefix(store, ns);
    backend->definite_count = 1;
    EXPECT_ANY_THROW(store->dropRef(ns, "y"));

    EXPECT_TRUE(store->refLaneWedgedForTest(ns)) << "a definite refusal AFTER an ambiguous attempt must not unwedge";
    EXPECT_EQ(store->wedgedKeyForTest(ns), wedged_key) << "the SAME wedge, not a fresh one";
    EXPECT_TRUE(store->resolveRef(ns, "x").has_value()) << "nothing was adopted";
    EXPECT_FALSE(backend->get(wedged_key).has_value()) << "and nothing became durable";
    EXPECT_EQ(store->laneStateForTest(ns), RefLaneState::Wedged)
        << "a wedged lane's steady state is 'may be durable, not applied'";

    /// Still the same id afterwards: the definite refusal consumed nothing.
    backend->definite_count = 0;
    EXPECT_NO_THROW(store->dropRef(ns, "y"));
    EXPECT_EQ(store->layout().parseRefObjectKey(
        store->layout().refLogKey(NamespaceLifeId::stageATransition(ns), wedged_id))->txn_id, wedged_id);
    EXPECT_FALSE(store->refLaneWedgedForTest(ns)) << "the create-based resolution still settles it afterwards";
}

/// The SAME rule one level down, and the level where it was actually broken. The test above splits the
/// two attempts across two CALLS, which the wedge already handles. Inside ONE call the controller used
/// to report the LAST attempt's outcome: an ambiguous attempt followed by a definitively refused reissue
/// came back `DefiniteFailure` -- the verdict that means "the key is provably unwritten". It is not. The
/// refusal proves only that the SECOND request never applied; the first may still be in flight and may
/// still land, and `unresolvedProvesNothingWasSent` is false for exactly that reason. So the CALL is
/// unresolved, and a definite verdict is only ever the whole call's.
///
/// The new reason lands on the fail-close side of the predicate the ledger acts on. Asserted at compile
/// time, beside the behaviour, because a member added to the enum without classifying it is precisely
/// how the wedge would silently stop happening.
static_assert(!unresolvedProvesNothingWasSent(CasUnresolvedReason::DefiniteFailureAfterAmbiguity));

TEST(CasRefWedgeEveryAttempt, ADefiniteRefusalCannotSpeakForAnEarlierAmbiguousAttemptOfTheSameCall)
{
#if !USE_AWS_S3
    GTEST_SKIP() << "DefiniteFailure classification requires S3 error types (USE_AWS_S3 off)";
#else
    auto backend = std::make_shared<WedgeTestBackend>();
    CasRequestController controller(backend, twoAttemptBudget());
    const std::function<bool()> fence_ok = [] { return true; };

    /// One call, two attempts: ambiguous, then definitively refused.
    backend->ambiguous_substr = "key/";
    backend->ambiguous_count = 1;
    backend->s3_definite_substr = "key/";
    backend->s3_definite_count = 1;

    CasUnresolvedReason reason = CasUnresolvedReason::NotUnresolved;
    const CasWriteOutcome outcome =
        controller.putIfAbsentControlled("key/haunted", "bytes", fence_ok, /*out_token=*/nullptr, &reason);

    EXPECT_EQ(outcome, CasWriteOutcome::Unresolved)
        << "a definite refusal of the SECOND attempt cannot retire the first attempt's ambiguity";
    EXPECT_EQ(reason, CasUnresolvedReason::DefiniteFailureAfterAmbiguity);
    EXPECT_FALSE(unresolvedProvesNothingWasSent(reason))
        << "the caller must keep protecting itself: an earlier attempt was sent and may yet land";
    EXPECT_FALSE(backend->get("key/haunted").has_value())
        << "and the key is still empty -- which is exactly why an absent read settles nothing";

    /// THE CONTROL. Aggregation must not soften a definite refusal that speaks for the whole call: with
    /// no ambiguous predecessor, the first attempt's whitelisted rejection is still `DefiniteFailure`,
    /// and the ledger may still free the id on it.
    backend->s3_definite_count = 1;
    CasUnresolvedReason clean_reason = CasUnresolvedReason::NotUnresolved;
    EXPECT_EQ(controller.putIfAbsentControlled("key/clean", "bytes", fence_ok, /*out_token=*/nullptr, &clean_reason),
              CasWriteOutcome::DefiniteFailure);
    EXPECT_EQ(clean_reason, CasUnresolvedReason::NotUnresolved);
#endif
}

/// The ledger-side twin of the same call: what the append lane does with that verdict. On
/// `DefiniteFailure` it returns the lane to `Ready` and tells callers the txn id was never used,
/// so the next append re-derives that id -- which, with an earlier attempt still possibly in flight, is
/// how an acked-then-lost transaction gets written around. The lane must wedge instead and stay pending
/// until the key itself resolves.
TEST(CasRefWedgeEveryAttempt, ADefiniteRefusalAfterAnAmbiguousAttemptOfTheSameCallStillWedgesTheLane)
{
#if !USE_AWS_S3
    GTEST_SKIP() << "DefiniteFailure classification requires S3 error types (USE_AWS_S3 off)";
#else
    auto backend = std::make_shared<WedgeTestBackend>();
    auto store = openPool(backend, twoAttemptBudget());
    const RootNamespace ns{"srv1/wedge_one_call_ambiguous_then_definite"};
    publishEmptyPart(store, ns, "x");
    publishEmptyPart(store, ns, "y");

    backend->ambiguous_substr = logPrefix(store, ns);
    backend->ambiguous_count = 1;
    backend->s3_definite_substr = logPrefix(store, ns);
    backend->s3_definite_count = 1;

    const uint64_t wedged_before = ProfileEvents::global_counters[ProfileEvents::CasRefAppendWedged].load();
    const uint64_t definite_before = ProfileEvents::global_counters[ProfileEvents::CasRefAppendDefiniteFailure].load();

    expectThrowsCode(DB::ErrorCodes::NETWORK_ERROR, [&] { store->dropRef(ns, "x"); });

    EXPECT_TRUE(store->refLaneWedgedForTest(ns))
        << "one call whose first attempt is unresolved leaves an object that may become durable";
    EXPECT_EQ(store->laneStateForTest(ns), RefLaneState::Wedged)
        << "the id must NOT be declared never-used: the marker stands until the key itself resolves";
    EXPECT_EQ(ProfileEvents::global_counters[ProfileEvents::CasRefAppendDefiniteFailure].load(), definite_before)
        << "this append was never definitively rejected -- only one of its attempts was";
    EXPECT_EQ(ProfileEvents::global_counters[ProfileEvents::CasRefAppendWedged].load(), wedged_before + 1);
    EXPECT_TRUE(store->resolveRef(ns, "x").has_value()) << "nothing is applied while the lane is wedged";
    const String wedged_key = store->wedgedKeyForTest(ns);
    EXPECT_FALSE(backend->get(wedged_key).has_value()) << "and nothing became durable";

    /// And it still recovers by the ordinary route: the next flush's bounded create lands the wedged
    /// transaction and adopts it, so wedging costs availability only until the next caller arrives.
    EXPECT_NO_THROW(store->dropRef(ns, "y"));
    EXPECT_FALSE(store->refLaneWedgedForTest(ns));
    EXPECT_FALSE(store->resolveRef(ns, "x").has_value()) << "the adopted wedge applied its drop";
    EXPECT_FALSE(store->resolveRef(ns, "y").has_value());
    EXPECT_EQ(store->laneStateForTest(ns), RefLaneState::Ready);
#endif
}

/// ===================================================================================
/// A successor's `EpochSeal` is the conclusive rejection (spec INV-2)
/// ===================================================================================

/// The seal is the ONLY thing that can prove our transaction will never be durable: the key is
/// write-once and a successor put its epoch-closing record there. The operation was never acked, so
/// its callers get a permanent error; the wedge is cleared; and the seal becomes this namespace's
/// `prev_epoch_seal`, which the first append of the NEXT epoch carries on the wire.
TEST(CasRefWedgeEveryAttempt, SuccessorSealAtTheWedgedKeyRejectsConclusivelyAndSourcesPrevEpochSeal)
{
    auto backend = std::make_shared<WedgeTestBackend>();
    auto store = openPool(backend, singleAttemptBudget());
    const Layout & layout = store->layout();
    const RootNamespace ns{"srv1/wedge_sealed"};
    publishEmptyPart(store, ns, "x");
    publishEmptyPart(store, ns, "y");

    const uint64_t epoch = store->liveWriterEpoch();
    backend->ambiguous_substr = logPrefix(store, ns);
    backend->ambiguous_count = 1;
    expectThrowsCode(DB::ErrorCodes::NETWORK_ERROR, [&] { store->dropRef(ns, "x"); });
    ASSERT_TRUE(store->refLaneWedgedForTest(ns));
    const String wedged_key = store->wedgedKeyForTest(ns);
    const RefTxnId seal_id = layout.parseRefObjectKey(wedged_key)->txn_id;
    ASSERT_EQ(seal_id.writer_epoch, epoch);
    ASSERT_GT(seal_id.ref_sequence, 1u) << "this namespace already has records, so its seal is not at sequence 1";
    ASSERT_EQ(store->lastEpochSealForTest(ns), std::nullopt) << "nothing has closed an epoch for this namespace yet";
    const size_t tail_before = store->tailSinceSnapshotCountForTest(ns);

    /// A successor closes our epoch at exactly the slot our attempt was aiming at.
    ASSERT_EQ(backend->putAsSuccessor(wedged_key, epochSealBytes(ns, seal_id)).outcome, PutOutcome::Done);

    /// The next caller's resolution meets the seal. Its own items fail -- permanently, not "retry
    /// later": nothing about this lane's epoch will ever accept a write again.
    expectThrowsCode(DB::ErrorCodes::INVALID_STATE, [&] { store->dropRef(ns, "y"); });

    EXPECT_FALSE(store->refLaneWedgedForTest(ns)) << "a conclusive rejection clears the wedge";
    EXPECT_TRUE(store->resolveRef(ns, "x").has_value()) << "the rejected transaction was never applied";
    EXPECT_EQ(store->tailSinceSnapshotCountForTest(ns), tail_before) << "and never joined the tail";
    EXPECT_EQ(store->laneStateForTest(ns), RefLaneState::Closed)
        << "the successor seal closes this epoch's lane";
    ASSERT_EQ(store->lastEpochSealForTest(ns), std::make_optional(seal_id))
        << "the observed seal is this namespace's epoch-closing record";

    /// INV-2's fence, stated as behaviour: a dying lane that observed the seal keeps deriving the SAME
    /// `T+1` and keeps colliding with it -- it never mints `T+2` and writes its stream past the record
    /// that closed its epoch. Ids are state-derived, so this falls out rather than being enforced.
    ///
    /// And the collision is adjudicated as the CONCLUSIVE REJECTION it is, not as foreign interference:
    /// this is the designed path, so it must not fence the mount or raise an anomaly. The append site
    /// reads the occupant and tells a seal of this namespace from a genuine breach, exactly as the
    /// wedge-resolve site does.
    const uint64_t remounts_before = store->scheduleRemountCallCountForTest();
    expectThrowsCode(DB::ErrorCodes::INVALID_STATE, [&] { store->dropRef(ns, "y"); });
    EXPECT_EQ(backend->get(layout.refLogKey(NamespaceLifeId::stageATransition(ns), RefTxnId{epoch, seal_id.ref_sequence + 1})), std::nullopt)
        << "nothing of ours may exist above the seal in the closed epoch";
    EXPECT_TRUE(store->mayMutate()) << "meeting a successor's seal is the protocol working, not an anomaly";
    EXPECT_EQ(store->scheduleRemountCallCountForTest(), remounts_before)
        << "and must not schedule a remount";

    /// Merely changing the epoch counters does not reopen a cached runtime. Production reaches a new
    /// epoch through remount, which replaces the runtime and recovers its chain link.
    bumpFenceGeneration(store, epoch + 1);
    expectThrowsCode(DB::ErrorCodes::INVALID_STATE, [&] { store->dropRef(ns, "y"); });
    EXPECT_EQ(store->laneStateForTest(ns), RefLaneState::Closed);
}

/// The wire round trip of the same rule, driven from the OTHER producer of `last_epoch_seal`:
/// recovery's CAS-walk (Task 6), stood in for here by its test seam. The point is the encode call
/// site, which is this task's.
TEST(CasRefWedgeEveryAttempt, OrdinaryFirstAppendAfterASealedTransitionCarriesTheExactPrevEpochSeal)
{
    auto backend = std::make_shared<CountingBackend>();
    auto store = openPool(backend);
    const Layout & layout = store->layout();
    const RootNamespace ns{"srv1/prev_epoch_seal_roundtrip"};

    const uint64_t epoch = store->liveWriterEpoch();
    publishEmptyPart(store, ns, "x");
    const RefTxnId seal_id{epoch, 42};

    /// A recovery that walked the dead epoch installs the seal it wrote; the mount then lives at E+1.
    store->setLastEpochSealForTest(ns, seal_id);
    bumpFenceGeneration(store, epoch + 1);

    EXPECT_NO_THROW(store->dropRef(ns, "x"));

    const RefLogTxn written = readRefLogTxn(*backend, layout, ns, RefTxnId{epoch + 1, 1});
    EXPECT_EQ(written.prev_epoch_seal, std::make_optional(seal_id));

    /// And it is carried on sequence 1 ONLY: the next transaction of the same epoch must not repeat it.
    publishEmptyPart(store, ns, "z");
    const RefLogTxn second = readRefLogTxn(*backend, layout, ns, RefTxnId{epoch + 1, 2});
    EXPECT_EQ(second.prev_epoch_seal, std::nullopt)
        << "prev_epoch_seal is required on sequence 1 of a non-genesis epoch and forbidden everywhere else";
}

/// GENESIS: `last_epoch_seal` is `nullopt` exactly for a namespace whose stream starts here, and a
/// genesis birth carries NO `prev_epoch_seal` even though its epoch is far above 1. Nothing about the
/// global epoch number makes a namespace non-genesis -- only a transition of its OWN stream does.
TEST(CasRefWedgeEveryAttempt, GenesisBirthAtAHighEpochCarriesNoPrevEpochSeal)
{
    auto backend = std::make_shared<CountingBackend>();
    auto store = openPool(backend);
    const Layout & layout = store->layout();
    const RootNamespace ns{"srv1/genesis_at_five"};

    bumpFenceGeneration(store, 5);
    ASSERT_EQ(store->liveWriterEpoch(), 5u);

    publishEmptyPart(store, ns, "x");

    EXPECT_EQ(store->lastEpochSealForTest(ns), std::nullopt)
        << "a namespace with no recovered seal, whose greatest applied id is at its own life epoch, is genesis";
    const RefLogTxn birth = readRefLogTxn(*backend, layout, ns, RefTxnId{5, 1});
    EXPECT_EQ(birth.prev_epoch_seal, std::nullopt) << "a genesis stream opens; it does not continue one";
}

/// ===================================================================================
/// A foreign occupant is impossible, so it is loud -- and the mount self-heals
/// ===================================================================================

/// Under mount-lease exclusivity the wedged key is exclusively ours, so a foreign non-seal object at
/// it is corruption or a protocol breach. Fail closed with `CORRUPTED_DATA`, KEEP the wedge for
/// inspection, and route the anomaly so the mount remounts itself rather than staying stuck until
/// someone notices.
TEST(CasRefWedgeEveryAttempt, ForeignNonSealOccupantIsCorruptedDataAndSchedulesARemount)
{
    auto backend = std::make_shared<WedgeTestBackend>();
    auto store = openPool(backend, singleAttemptBudget());
    const RootNamespace ns{"srv1/wedge_foreign"};
    publishEmptyPart(store, ns, "x");
    publishEmptyPart(store, ns, "y");

    backend->ambiguous_substr = logPrefix(store, ns);
    backend->ambiguous_count = 1;
    expectThrowsCode(DB::ErrorCodes::NETWORK_ERROR, [&] { store->dropRef(ns, "x"); });
    ASSERT_TRUE(store->refLaneWedgedForTest(ns));
    const String wedged_key = store->wedgedKeyForTest(ns);
    const uint64_t remounts_before = store->scheduleRemountCallCountForTest();

    /// Something that is neither our bytes nor a seal occupies the slot.
    ASSERT_EQ(backend->putAsSuccessor(wedged_key, "not a ref-log object at all").outcome, PutOutcome::Done);

    expectThrowsCode(DB::ErrorCodes::CORRUPTED_DATA, [&] { store->dropRef(ns, "y"); });

    EXPECT_FALSE(store->refLaneWedgedForTest(ns));
    EXPECT_EQ(store->laneStateForTest(ns), RefLaneState::Faulted);
    EXPECT_GT(store->scheduleRemountCallCountForTest(), remounts_before)
        << "the impossible-interference route must schedule a remount";
}

/// I5, the OTHER site with the same shape: the ordinary append's own conditional create can prove a
/// different object sits at the id it derived. Task 3 made that fail closed -- correctly -- but it
/// left the mount stuck there until a manual remount, unlike the wedge-resolution site. Both are the
/// same impossibility and both must self-heal by remount.
TEST(CasRefWedgeEveryAttempt, AppendSiteProvenDifferentObjectAlsoSchedulesARemount)
{
    auto backend = std::make_shared<CountingBackend>();
    auto store = openPool(backend);
    const Layout & layout = store->layout();
    const RootNamespace ns{"srv1/append_site_foreign"};
    publishEmptyPart(store, ns, "x");

    /// Occupy the id the next append will derive with a foreign object, so its create conflicts and
    /// the controller's resolve-before-reissue proves the occupant is not ours.
    const RefTxnId next{store->liveWriterEpoch(), 3};
    ASSERT_EQ(backend->putIfAbsent(layout.refLogKey(NamespaceLifeId::stageATransition(ns), next),
                                   "a different object entirely").outcome, PutOutcome::Done);
    const uint64_t remounts_before = store->scheduleRemountCallCountForTest();

    expectThrowsCode(DB::ErrorCodes::CORRUPTED_DATA, [&] { store->dropRef(ns, "x"); });

    EXPECT_FALSE(store->refLaneWedgedForTest(ns)) << "a proven different object is conclusive, never a wedge";
    EXPECT_GT(store->scheduleRemountCallCountForTest(), remounts_before)
        << "the append site must route through the same impossible-interference reaction as the wedge site";
}

/// ===================================================================================
/// The admission fence
/// ===================================================================================

/// The old-generation-retry-inert rule. A wedge admitted under one mount incarnation may not send an
/// attempt under another: the retry is refused BEFORE anything reaches the store, so the key is
/// provably untouched and the wedge is intact for whoever recovers the lane properly.
TEST(CasRefWedgeEveryAttempt, RetryUnderAnOlderAdmissionGenerationSendsNothing)
{
    auto backend = std::make_shared<WedgeTestBackend>();
    auto store = openPool(backend, singleAttemptBudget());
    const RootNamespace ns{"srv1/wedge_old_generation"};
    publishEmptyPart(store, ns, "x");
    publishEmptyPart(store, ns, "y");

    const uint64_t epoch = store->liveWriterEpoch();
    backend->ambiguous_substr = logPrefix(store, ns);
    backend->ambiguous_count = 1;
    expectThrowsCode(DB::ErrorCodes::NETWORK_ERROR, [&] { store->dropRef(ns, "x"); });
    ASSERT_TRUE(store->refLaneWedgedForTest(ns));
    const String wedged_key = store->wedgedKeyForTest(ns);
    ASSERT_EQ(store->wedgedAdmittedGenerationForTest(ns), store->fenceGeneration())
        << "the wedge records the generation it was admitted under";
    const uint64_t puts_before = backend->putCount(wedged_key);

    /// The lease incarnation moves under the wedge; the mount is writable again, but not the same one.
    bumpFenceGeneration(store, epoch);
    ASSERT_NE(store->wedgedAdmittedGenerationForTest(ns), store->fenceGeneration());

    EXPECT_ANY_THROW(store->dropRef(ns, "y"));

    EXPECT_EQ(backend->putCount(wedged_key), puts_before)
        << "the retry must be refused pre-attempt: nothing may reach the store under a foreign generation";
    EXPECT_TRUE(store->refLaneWedgedForTest(ns)) << "and the wedge is untouched";
    EXPECT_FALSE(backend->get(wedged_key).has_value());
}

/// The post-I/O recheck, deterministically. The retry's create is parked mid-flight; while it is
/// parked the fence is lost and re-armed AND a successor seals the slot. The released result is a
/// perfectly real `Occupied`(seal) -- but it belongs to an incarnation that no longer exists, so this
/// runtime must act on NOTHING: no acknowledgement, no unwedge, no install, and no adoption of the
/// seal it just read.
TEST(CasRefWedgeEveryAttempt, ResultReleasedAfterAFenceBumpAndSuccessorSealIsInert)
{
    auto backend = std::make_shared<WedgeTestBackend>();
    auto store = openPool(backend, singleAttemptBudget());
    const Layout & layout = store->layout();
    const RootNamespace ns{"srv1/wedge_blocked_io"};
    publishEmptyPart(store, ns, "x");
    publishEmptyPart(store, ns, "y");

    const uint64_t epoch = store->liveWriterEpoch();
    backend->ambiguous_substr = logPrefix(store, ns);
    backend->ambiguous_count = 1;
    expectThrowsCode(DB::ErrorCodes::NETWORK_ERROR, [&] { store->dropRef(ns, "x"); });
    ASSERT_TRUE(store->refLaneWedgedForTest(ns));
    const String wedged_key = store->wedgedKeyForTest(ns);
    const RefTxnId seal_id = layout.parseRefObjectKey(wedged_key)->txn_id;
    const size_t tail_before = store->tailSinceSnapshotCountForTest(ns);

    backend->armBlock(logPrefix(store, ns));
    std::exception_ptr caller_error;
    std::thread resolver([&]
    {
        try { store->dropRef(ns, "y"); }
        catch (...) { caller_error = std::current_exception(); }
    });
    backend->awaitBlockEntered();

    /// Everything that makes this runtime superseded happens INSIDE the I/O window.
    ASSERT_EQ(backend->putAsSuccessor(wedged_key, epochSealBytes(ns, seal_id)).outcome, PutOutcome::Done);
    bumpFenceGeneration(store, epoch + 1);
    backend->releaseBlock();
    resolver.join();

    ASSERT_TRUE(caller_error != nullptr) << "no acknowledgement: the caller must not be told this succeeded";
    /// And it must be the RETRY-SAFE class. A moved incarnation is usually a routine lease blip, and the
    /// storage layer classifies retry-safety on exactly `ABORTED || NETWORK_ERROR` — surfacing the fence
    /// check's own `INVALID_STATE` here would turn every blip into a hard failure for the caller.
    expectThrowsCode(DB::ErrorCodes::NETWORK_ERROR, [&] { std::rethrow_exception(caller_error); });
    EXPECT_TRUE(store->refLaneWedgedForTest(ns)) << "no unwedge";
    EXPECT_EQ(store->tailSinceSnapshotCountForTest(ns), tail_before) << "no install";
    EXPECT_TRUE(store->resolveRef(ns, "x").has_value()) << "no install: the wedged drop is still unapplied";
    EXPECT_EQ(store->lastEpochSealForTest(ns), std::nullopt)
        << "and no adoption of the seal a superseded runtime happened to read";
}

/// The same recheck, on the identity leg rather than the generation leg. The fence never moves; only
/// the installed wedge's BYTES change while the create is parked. Generation-equality alone would let
/// the released result install a candidate built from the OTHER attempt's transaction -- the aliasing
/// bug the phase-0 model found, which is why identity is (generation, id, bytes) and not any one of
/// them. Production cannot reach this (one leader per table mutates a lane), so this is a white-box
/// guard on the rule, driven through the force-wedge seam.
TEST(CasRefWedgeEveryAttempt, ResultReleasedAfterTheWedgeIdentityChangedIsInert)
{
    auto backend = std::make_shared<WedgeTestBackend>();
    auto store = openPool(backend, singleAttemptBudget());
    const RootNamespace ns{"srv1/wedge_identity_changed"};
    publishEmptyPart(store, ns, "x");
    publishEmptyPart(store, ns, "y");

    backend->ambiguous_substr = logPrefix(store, ns);
    backend->ambiguous_count = 1;
    expectThrowsCode(DB::ErrorCodes::NETWORK_ERROR, [&] { store->dropRef(ns, "x"); });
    ASSERT_TRUE(store->refLaneWedgedForTest(ns));
    const String wedged_key = store->wedgedKeyForTest(ns);
    const RefTxnId wedged_id = store->layout().parseRefObjectKey(wedged_key)->txn_id;
    const size_t tail_before = store->tailSinceSnapshotCountForTest(ns);

    backend->armBlock(logPrefix(store, ns));
    std::exception_ptr caller_error;
    std::thread resolver([&]
    {
        try { store->dropRef(ns, "y"); }
        catch (...) { caller_error = std::current_exception(); }
    });
    backend->awaitBlockEntered();

    /// Same id, same generation, DIFFERENT bytes.
    store->forceWedgeForTest(ns, wedged_id.writer_epoch, wedged_id.ref_sequence, wedged_key, "different attempt bytes");
    backend->releaseBlock();
    resolver.join();

    ASSERT_TRUE(caller_error != nullptr);
    EXPECT_TRUE(store->refLaneWedgedForTest(ns)) << "no unwedge";
    EXPECT_EQ(store->tailSinceSnapshotCountForTest(ns), tail_before)
        << "no install: the released result described a wedge that is no longer installed";
    EXPECT_TRUE(store->resolveRef(ns, "x").has_value());
}

/// A resolution that proves the exact attempt durable but cannot install it has one successor:
/// `NeedsRecovery`. It drops the attempt and forbids another write until replay catches the cache up.
TEST(CasRefWedgeEveryAttempt, KnownDurableInstallFailureMovesDirectlyToRecovery)
{
    auto backend = std::make_shared<LandedButAckLostOnceBackend>();
    backend->fired = true;   /// disarmed while the fixture is built (see the adoption test above)
    auto store = openPool(backend, singleAttemptBudget());
    const RootNamespace ns{"srv1/wedge_floor"};
    publishEmptyPart(store, ns, "x");
    publishEmptyPart(store, ns, "y");

    backend->key_substr = logPrefix(store, ns);
    backend->lose_resolve_read = true;
    backend->fired = false;
    expectThrowsCode(DB::ErrorCodes::NETWORK_ERROR, [&] { store->dropRef(ns, "x"); });
    ASSERT_TRUE(store->refLaneWedgedForTest(ns));
    const String wedged_key = store->wedgedKeyForTest(ns);
    ASSERT_TRUE(backend->get(wedged_key).has_value()) << "the wedged transaction is durable";
    /// The adoption reaches its install region and the install throws.
    armOneShotInstallFailure(store);
    expectThrowsCode(DB::ErrorCodes::MEMORY_LIMIT_EXCEEDED, [&] { store->dropRef(ns, "y"); });
    store->setInstallRegionProbeForTest(nullptr);
    ASSERT_EQ(store->laneStateForTest(ns), RefLaneState::NeedsRecovery);
    ASSERT_FALSE(store->refLaneWedgedForTest(ns))
        << "known durability transfers ownership to recovery; no uncertain attempt remains";
    /// Do not call the tail-count seam here: it intentionally forces recovery, which is the transition
    /// this assertion is proving has not happened yet.

    /// The next flush first replays the durable drop of `x`, then admits the drop of `y`.
    EXPECT_NO_THROW(store->dropRef(ns, "y"));
    EXPECT_FALSE(store->resolveRef(ns, "x").has_value())
        << "replay must install the already-durable drop of `x` before the next write";
    EXPECT_FALSE(store->resolveRef(ns, "y").has_value());
    EXPECT_EQ(store->laneStateForTest(ns), RefLaneState::Ready)
        << "only completed replay returns the lane to Ready";
}

/// ===================================================================================
/// The append site owes the SAME three-way adjudication as the wedge site
/// ===================================================================================

/// No wedge is involved here at all: an ordinary append derives its next id and finds a successor's
/// epoch seal sitting on it. That is not interference — it is INV-2's designed outcome for a lane that
/// has been deposed without being told, and the lane will keep re-deriving that same id forever. So it
/// must be adjudicated as the conclusive rejection it is: a permanent error for the callers, the seal
/// recorded as this namespace's epoch-closing record, and NO fence and NO remount.
TEST(CasRefWedgeEveryAttempt, AppendSiteMeetingASuccessorSealIsAConclusiveRejectionNotInterference)
{
    auto backend = std::make_shared<WedgeTestBackend>();
    auto store = openPool(backend);
    const Layout & layout = store->layout();
    const RootNamespace ns{"srv1/append_site_seal"};
    publishEmptyPart(store, ns, "x");

    const uint64_t epoch = store->liveWriterEpoch();
    const RefTxnId next{epoch, 3};
    ASSERT_EQ(store->lastEpochSealForTest(ns), std::nullopt);
    const uint64_t remounts_before = store->scheduleRemountCallCountForTest();
    const uint64_t sealed_before = ProfileEvents::global_counters[ProfileEvents::CasRefAppendSealRejected].load();

    /// The successor's seal lands at exactly the id this table's next append derives.
    backend->conflict_substr = layout.refLogKey(NamespaceLifeId::stageATransition(ns), next);
    backend->conflict_bytes = epochSealBytes(ns, next);
    backend->conflict_count = 1;

    expectThrowsCode(DB::ErrorCodes::INVALID_STATE, [&] { store->dropRef(ns, "x"); });

    EXPECT_FALSE(store->refLaneWedgedForTest(ns)) << "a conclusive rejection is not an uncertain outcome";
    EXPECT_TRUE(store->resolveRef(ns, "x").has_value()) << "the rejected transaction never applied";
    EXPECT_EQ(store->lastEpochSealForTest(ns), std::make_optional(next))
        << "the observed seal IS this namespace's epoch-closing record, whichever site observed it";
    EXPECT_TRUE(store->mayMutate()) << "the designed path must not fence the mount";
    EXPECT_EQ(store->scheduleRemountCallCountForTest(), remounts_before) << "nor schedule a remount";
    EXPECT_EQ(ProfileEvents::global_counters[ProfileEvents::CasRefAppendSealRejected].load(), sealed_before + 1)
        << "a deposed writer must still be COUNTED: this is the protocol working, and also the signal "
           "that this mount has lost its lease and does not know it";
}

/// [CKPT-FAILED-BIRTH-DEBRIS] (BACKLOG `{#ckpt-failed-birth-debris}`, closed by Stage B Task 3): a
/// GENESIS birth (never-born `ns`, so `precommitAdd` prepends `namespace_birth`) whose `_ckpt` publish
/// lands but whose ref-log PUT is then conclusively rejected by a successor's epoch seal at the exact
/// id it derived -- the identical scenario as the test above, but at sequence 1 of a namespace that has
/// never had a `_ckpt` before, so `commitRefChunk` is its FIRST writer. Before this task, that `_ckpt`
/// was permanent debris: namespace cleanup runs only for `Removed` namespaces, and this one never
/// reaches `Live`, so nothing else ever reclaims it (`Gc/CasGc.cpp`'s own `_ckpt` backstop is the
/// REMOVED-namespace case; this is the never-born one it explicitly cannot reach).
TEST(CasRefWedgeEveryAttempt, BirthCkptIsReclaimedWhenTheGenesisTransactionIsConclusivelyRejected)
{
    auto backend = std::make_shared<WedgeTestBackend>();
    auto store = openPool(backend);
    const Layout & layout = store->layout();
    const RootNamespace ns{"srv1/birth_ckpt_debris"};

    const RefTxnId genesis{store->liveWriterEpoch(), 1};
    const String ckpt_key = layout.refCkptKey(NamespaceLifeId::stageATransition(ns));
    ASSERT_FALSE(backend->get(ckpt_key).has_value()) << "nothing has ever been written for this namespace yet";

    /// A successor's epoch seal lands at exactly the id this never-born table's genesis birth derives.
    backend->conflict_substr = layout.refLogKey(NamespaceLifeId::stageATransition(ns), genesis);
    backend->conflict_bytes = epochSealBytes(ns, genesis);
    backend->conflict_count = 1;

    expectThrowsCode(DB::ErrorCodes::INVALID_STATE, [&] { publishEmptyPart(store, ns, "x"); });

    EXPECT_FALSE(backend->get(ckpt_key).has_value())
        << "the birth's own _ckpt was published (step 2 of the ordering spec §3 requires), but this "
           "attempt's ref-log PUT was conclusively rejected (step below it never sent) -- the orphaned "
           "_ckpt must be reclaimed here, not left as permanent debris under a namespace that never "
           "reached Live";
}

/// The same conflict, but the read that would tell a seal from a breach fails. We must then decide
/// NEITHER: fencing the mount would be a guess, and reporting a conclusive rejection would acknowledge
/// a deposition nobody observed. The id is not consumed, so the next attempt re-derives it and
/// classifies again — deferring costs one round trip and decides nothing wrongly.
TEST(CasRefWedgeEveryAttempt, AppendSiteFaultsWhenTheOccupantCannotBeRead)
{
    auto backend = std::make_shared<WedgeTestBackend>();
    auto store = openPool(backend);
    const Layout & layout = store->layout();
    const RootNamespace ns{"srv1/append_site_unreadable"};
    publishEmptyPart(store, ns, "x");

    const RefTxnId next{store->liveWriterEpoch(), 3};
    const uint64_t remounts_before = store->scheduleRemountCallCountForTest();

    backend->conflict_substr = layout.refLogKey(NamespaceLifeId::stageATransition(ns), next);
    backend->conflict_bytes = epochSealBytes(ns, next);
    backend->conflict_count = 1;
    /// Skip the resolve read that PROVES the conflict; fail only the adjudication read after it.
    backend->fail_get_substr = layout.refLogKey(NamespaceLifeId::stageATransition(ns), next);
    backend->fail_get_skip = 1;
    backend->fail_get_count = 1;

    const uint64_t deferred_before = ProfileEvents::global_counters[ProfileEvents::CasRefAppendOccupantUnreadable].load();
    expectThrowsCode(DB::ErrorCodes::CORRUPTED_DATA, [&] { store->dropRef(ns, "x"); });

    EXPECT_EQ(ProfileEvents::global_counters[ProfileEvents::CasRefAppendOccupantUnreadable].load(), deferred_before + 1)
        << "the deferral is the one quiet arm here -- it must be counted or a starved loud path is invisible";
    EXPECT_TRUE(store->mayMutate()) << "the table faults without guessing that the whole mount is corrupt";
    EXPECT_EQ(store->scheduleRemountCallCountForTest(), remounts_before) << "nor schedule a remount";
    EXPECT_EQ(store->lastEpochSealForTest(ns), std::nullopt)
        << "nor record a deposition that was never actually observed";
    EXPECT_FALSE(store->refLaneWedgedForTest(ns)) << "nothing of ours became durable, so nothing is wedged";
    EXPECT_EQ(store->laneStateForTest(ns), RefLaneState::Faulted);
    expectThrowsCode(DB::ErrorCodes::INVALID_STATE, [&] { store->dropRef(ns, "x"); });
}

/// A WELL-FORMED ref-log transaction of this namespace at this id, which simply is not a seal, must be
/// adjudicated `Foreign` on CONTENT — not because it failed to decode. The sibling test above reaches
/// the same verdict through an undecodable body, so without this one the classifier could be deciding
/// "foreign" purely from decode failures and nothing would notice.
TEST(CasRefWedgeEveryAttempt, WellFormedNonSealOccupantIsStillForeign)
{
    auto backend = std::make_shared<WedgeTestBackend>();
    auto store = openPool(backend);
    const Layout & layout = store->layout();
    const RootNamespace ns{"srv1/append_site_wellformed_foreign"};
    publishEmptyPart(store, ns, "x");

    const RefTxnId next{store->liveWriterEpoch(), 3};
    const uint64_t remounts_before = store->scheduleRemountCallCountForTest();

    /// A perfectly decodable transaction for this exact namespace and id — just not an epoch seal.
    RefOp birth;
    birth.kind = RefOpKind::NamespaceBirth;
    const RefLogTxn foreign_txn{ns.string(), next, {birth}, std::nullopt};
    backend->conflict_substr = layout.refLogKey(NamespaceLifeId::stageATransition(ns), next);
    backend->conflict_bytes = sealObject(FormatId::RefLog, encodeRefLogTxn(foreign_txn));
    backend->conflict_count = 1;

    expectThrowsCode(DB::ErrorCodes::CORRUPTED_DATA, [&] { store->dropRef(ns, "x"); });

    EXPECT_GT(store->scheduleRemountCallCountForTest(), remounts_before)
        << "a well-formed non-seal occupant is still a breach of write-exclusivity";
    EXPECT_EQ(store->lastEpochSealForTest(ns), std::nullopt) << "and is emphatically not an epoch seal";
}

/// The deposed-lane self-pointer. A successor that seals an EMPTY epoch writes its record at sequence 1
/// of that epoch, and a lane still live there re-derives exactly that id. Stamping the seal as its own
/// `prev_epoch_seal` would be a self-pointer, which the structural grammar (strictly-less by
/// construction) refuses at ENCODE — so the lane would fail with a self-inflicted `CORRUPTED_DATA` on
/// every attempt and never reach the seal collision that is supposed to fence it. The stamp is
/// therefore conditioned on the seal's epoch being strictly BELOW the id's.
TEST(CasRefWedgeEveryAttempt, ALiveEpochSealIsNeverStampedAsItsOwnPrevEpochSeal)
{
    auto backend = std::make_shared<CountingBackend>();
    auto store = openPool(backend);
    const Layout & layout = store->layout();
    const RootNamespace ns{"srv1/live_epoch_seal"};
    publishEmptyPart(store, ns, "x");

    const uint64_t epoch = store->liveWriterEpoch();
    bumpFenceGeneration(store, epoch + 1);
    /// A seal of the LIVE epoch — the deposed-lane shape the wedge rejection arm can record.
    store->setLastEpochSealForTest(ns, RefTxnId{epoch + 1, 7});

    /// The lane now holds NOTHING it can legally write. Its next id is sequence 1 of the new epoch, which
    /// owes a link to the seal that closed the epoch BELOW -- and the only seal it has is of the epoch it
    /// is trying to open. Stamping that one would be a self-pointer the ENCODER refuses; stamping nothing
    /// leaves a crossing the READER refuses. So the append fails closed, locally, before anything is sent.
    ///
    /// That is a strictly better outcome than the one this test originally pinned (stamp nothing, send,
    /// and let the successor's seal reject the attempt at the key): the deposed lane spends no request to
    /// learn what it can already prove about itself. The property the test exists for is unchanged and is
    /// asserted below in its strongest form -- NO object is written at that id at all, so no self-pointer
    /// can have been stamped anywhere.
    /// The refusal must reach the SAME TERMINAL OUTCOME the collision produced, not merely "an error".
    /// Skipping the request must not skip the conclusion, so all four halves are pinned:
    ///
    ///   1. the class is INVALID_STATE -- the conclusive-rejection class the successor-seal arm uses,
    ///      NOT the retry-later class. This is the one that matters most: a retryable error here would
    ///      have every caller re-derive the same impossible transaction forever, and the deposition
    ///      would never surface anywhere;
    ///   2. the message says the lane resumes only under a later epoch -- that IS the deposition,
    ///      reported to the caller and the operator in the same words the collision reported it;
    ///   3. NOTHING is written, so no self-pointer can have been stamped and no request was spent;
    ///   4. a SECOND flush behaves identically instead of looping or degrading.
    const uint64_t remounts_before = store->scheduleRemountCallCountForTest();
    const size_t puts_before = backend->putCount(layout.refLogKey(NamespaceLifeId::stageATransition(ns), RefTxnId{epoch + 1, 1}));
    try
    {
        store->dropRef(ns, "x");
        FAIL() << "the deposed lane must reject conclusively, not succeed";
    }
    catch (const DB::Exception & e)
    {
        EXPECT_EQ(e.code(), DB::ErrorCodes::INVALID_STATE) << "got: " << e.message();
        EXPECT_NE(e.message().find("resumes only under a later epoch"), String::npos)
            << "the deposition must be surfaced, not just the failure: " << e.message();
    }
    EXPECT_FALSE(backend->get(layout.refLogKey(NamespaceLifeId::stageATransition(ns), RefTxnId{epoch + 1, 1})).has_value())
        << "nothing may be written: the lane could not construct a legal transaction, so it sent none";
    EXPECT_EQ(backend->putCount(layout.refLogKey(NamespaceLifeId::stageATransition(ns), RefTxnId{epoch + 1, 1})), puts_before)
        << "and no request was spent learning what the lane could already prove about itself";

    /// The second flush: same conclusive answer, still no traffic. A lane that re-derived and re-sent
    /// here would be exactly the spin this arm exists to prevent.
    expectThrowsCode(DB::ErrorCodes::INVALID_STATE, [&] { store->dropRef(ns, "x"); });
    EXPECT_EQ(backend->putCount(layout.refLogKey(NamespaceLifeId::stageATransition(ns), RefTxnId{epoch + 1, 1})), puts_before);

    /// NO remount is scheduled, matching the collision arm exactly. A successor closing our epoch is a
    /// legitimate handover, not an anomaly to react to: the mount lease is what resolves it, and
    /// scheduling a remount from here would turn every ordinary deposition into a self-inflicted
    /// re-claim storm.
    EXPECT_EQ(store->scheduleRemountCallCountForTest(), remounts_before);
}
