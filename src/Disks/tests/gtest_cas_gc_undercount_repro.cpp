#include <gtest/gtest.h>

#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Gc/CasBlobInDegree.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Gc/CasGc.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Formats/CasGcStateFormat.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasPool.h>
#include "cas_test_helpers.h"

#include <functional>

/// Regression suite for the soak S04 / S04b undercount failures:
///   Code: 246 CORRUPTED_DATA: CAS blob in-degree: merged in-degree -1 < 0 for a blob ...
///
/// H1 (DeposedFoldAdopt) and H1b (FenceWindowReRemoval) guard against the fence-window re-fold
/// undercount. Fixed STRUCTURALLY by replacing the persisted integer in-degree with an idempotent
/// source-edge SET: re-folding a fence-window removal across generations is a set-difference no-op,
/// so the underflow cannot occur (NOT by patching the sealed cursor — that approach was rejected).
///
/// H2 (DropThenRepointIdempotent) guards against the duplicate-remove undercount that existed when
/// in-degree was a persisted integer: two events both carrying `old=committed(r1)` subtracted -1
/// twice from blob 2's count, driving it to -1. Same fix — the second removal of an already-absent
/// edge is a no-op.

using namespace DB::Cas;
using namespace DB::Cas::tests;

namespace DB::ErrorCodes
{
extern const int CORRUPTED_DATA;
extern const int ABORTED;
}

namespace
{

const UInt128 kGc = hexToU128("00000000000000000000000000000001");
const UInt128 kGcA = hexToU128("0000000000000000000000000000000a");
const UInt128 kGcB = hexToU128("0000000000000000000000000000000b");

ManifestRef ref(uint64_t seq, uint64_t inst)
{
    return ManifestRef{.writer_epoch = 1, .build_sequence = seq, .manifest_ordinal = static_cast<uint32_t>(inst)};
}

bool blobExists(InMemoryBackend & b, const Layout & layout, const UInt128 & hash)
{
    return b.head(layout.blobKey(BlobRef{BlobHashAlgo::CityHash128, BlobDigest::fromU128(hash)})).exists;
}

/// A committed `RefOwnerBinding` for a raw `owner_transition` op. The raw appender is now
/// `tests::appendOwnerEvent`, which writes ONE `owner_transition` ref-log transaction via
/// `writeRefLogTxnRaw` at the next `RefTxnId` -- the GC fold EXTRACTS edges from each log
/// (`manifestEdgesOfTxn`) and never replays them, so an `old_binding` that does not correspond to the
/// table's current committed owner is folded verbatim (exactly the "drop THEN repoint-from-r1" hazard).
RefOwnerBinding committed(const String & ref_name, const ManifestRef & r)
{
    return RefOwnerBinding{RefOwnerKind::Committed, ref_name, r};
}

}

/// ============================ H2: DROP-THEN-REPOINT FROM SAME OLD IS IDEMPOTENT (REGRESSION GUARD) ====
///
/// Two DISTINCT journal events each carry `old = committed(r1)`:
///   v2: DROP r1        {old=committed(r1), new=none}          => removes r1's source-edge to {1,2}
///   v3: REPOINT r1->r2 {old=committed(r1), new=committed(r2)} => removes r1's source-edge to {1,2} (again),
///                                                                  adds r2's source-edge to {1}
///
/// Under the OLD integer in-degree model this drove blob 2 to prior(1) + (-1) + (-1) = -1 and threw
/// CORRUPTED_DATA. Under the FIXED idempotent source-edge SET model the second removal of r1's edge to
/// blob 2 is a no-op: each source edge is present or absent, and removing an already-absent edge is silent.
///
/// Correct post-fix behaviour:
///   - GC must NOT throw.
///   - Blob 2 is unreferenced (r1 dropped, r2 never references it)  => collected (in-degree 0, key gone).
///   - Blob 1 is re-referenced by r2                                 => spared  (in-degree 1, key present).
TEST(CasGcUndercount, H2DropThenRepointFromSameOldIsIdempotentNoUnderflow)
{
    auto backend = std::make_shared<InMemoryBackend>();
    auto store = openPoolForTest(backend);
    const RootNamespace ns{"00/aa@cas@"};
    const ManifestRef r1 = ref(1, 0xB1);
    const ManifestRef r2 = ref(2, 0xB2);

    /// r1 pins blobs {1,2}; r2 (staged, present) pins only blob 1.
    writeBlobBody(*backend, store->layout(), DB::UInt128(1));
    writeBlobBody(*backend, store->layout(), DB::UInt128(2));
    writeManifestRaw(*backend, store->layout(), ns, r1,
        {blobEntryFor("a", DB::UInt128(1)), blobEntryFor("b", DB::UInt128(2))});
    writeManifestRaw(*backend, store->layout(), ns, r2, {blobEntryFor("a", DB::UInt128(1))});

    /// v1: publish r1 (owner: none -> committed(r1)). Fold it so blobs 1 and 2 are each pinned at 1.
    publishCommittedTransition(*backend, store->layout(), ns, "tbl", std::nullopt, r1);
    Gc gc(store, kGc);
    ASSERT_TRUE(gc.runRegularRound().acquired_lease);
    ASSERT_EQ(inDegreeOf(*backend, store->layout(), DB::UInt128(1)), 1);
    ASSERT_EQ(inDegreeOf(*backend, store->layout(), DB::UInt128(2)), 1);

    /// Stage TWO distinct removal-carrying events for r1 in ONE fold window (r1's body is NOT deleted
    /// until recheck, so both events are resolved at fold time):
    ///   v2: DROP r1        {old=committed(r1), new=none}
    ///   v3: REPOINT r1->r2 {old=committed(r1), new=committed(r2)}
    /// The second removal of r1's edge to blob 2 is a no-op under the idempotent set model.
    appendOwnerEvent(*backend, store->layout(), ns, 0, committed("tbl", r1), std::nullopt);
    appendOwnerEvent(*backend, store->layout(), ns, 0, committed("tbl", r1), committed("tbl", r2));

    /// Drive GC to fixpoint (advancing the mount ack each round so the ack floor graduates the condemned
    /// blob 2): must complete without throwing, collect blob 2, spare blob 1.
    ASSERT_NO_THROW({
        for (int i = 0; i < 12; ++i)
        {
            gc.runRegularRound();
            store->renewWatermarkOnce();
        }
    }) << "H2 regression: drop-then-repoint-from-same-old must NOT underflow; idempotent edge set absorbs the duplicate removal";

    EXPECT_EQ(inDegreeOf(*backend, store->layout(), DB::UInt128(2)), 0)
        << "blob 2 is unreferenced (r1 dropped, r2 does not reference it) and must be collected";
    EXPECT_FALSE(blobExists(*backend, store->layout(), DB::UInt128(2)))
        << "blob 2 must be physically removed from the store";

    EXPECT_EQ(inDegreeOf(*backend, store->layout(), DB::UInt128(1)), 1)
        << "blob 1 is re-referenced by r2 and must be spared";
    EXPECT_TRUE(blobExists(*backend, store->layout(), DB::UInt128(1)))
        << "blob 1 must remain present in the store";
}

/// ============================ H1: CURSOR RE-FOLD UNDER ABORT ============================
///
/// Hypothesis H1: a removal `-1` is folded, but the SINGLE round-commit CAS that would durably advance the
/// cursor past that removal LOSES to a concurrent leader (ABORTED). The cursor is NOT advanced, so a later
/// honest round RE-FOLDS the same removal against a parent generation whose in-degree for that blob has
/// already reached 0 => -1.
///
/// We reproduce the deposed-round-commit injection from gtest_cas_gc_attempt.cpp
/// (DeposedFoldAttemptDoesNotWedge): deny the SINGLE round-commit gc/state CAS (the one that advances
/// snap_generation) of the round that folds the drop's -1. The deposed round left only never-adopted
/// attempt-scoped debris, so the retry re-folds the -1 against the still-adopted parent (in-degree 1),
/// producing a clean 0 — never a double-applied -1.
class InterruptRoundCasBackend : public InMemoryBackend
{
public:
    CasResult casPut(const String & key, const String & bytes, const std::optional<Token> & expected,
                     const ObjectMeta & meta) override
    {
        if (arm_interrupt && key == gc_state_key)
        {
            const auto stored = get(key);
            const uint64_t stored_gen = stored ? decodeGcState(stored->bytes).snap_generation : 0;
            const uint64_t next_gen = decodeGcState(bytes).snap_generation;
            if (next_gen > stored_gen)
            {
                arm_interrupt = false;
                throw DB::Exception(DB::ErrorCodes::ABORTED,
                    "test-injected: round-commit gc/state CAS denied (leader deposed mid-round; lease lost)");
            }
        }
        return InMemoryBackend::casPut(key, bytes, expected, meta);
    }

    bool arm_interrupt = false;
    String gc_state_key = "p/gc/state";
};

TEST(CasGcUndercount, H1DrainAfterDeposedRemovalFoldDoesNotUnderflow)
{
    auto backend = std::make_shared<InterruptRoundCasBackend>();
    auto store = openPoolForTest(backend);
    ASSERT_EQ(store->layout().gcStateKey(), "p/gc/state");

    const RootNamespace ns{"00/aa@cas@"};
    const ManifestRef r = ref(1, 0xAA);
    writeBlobBody(*backend, store->layout(), DB::UInt128(1));
    writeManifestRaw(*backend, store->layout(), ns, r, {blobEntryFor("a", DB::UInt128(1))});
    publishCommittedTransition(*backend, store->layout(), ns, "tbl", std::nullopt, r);

    Gc gc(store, kGc);
    /// Round 1 (honest): fold +1, pin blob 1.
    ASSERT_TRUE(gc.runRegularRound().acquired_lease);
    store->renewWatermarkOnce();
    ASSERT_EQ(inDegreeOf(*backend, store->layout(), DB::UInt128(1)), 1);

    /// Drop the only ref and advance the watermark floor.
    dropRefTransition(*backend, store->layout(), ns, "tbl", r);
    store->renewWatermarkOnce();

    /// Round 2 (DEPOSED): fold the -1, then the round-commit CAS is denied (ABORTED). The adopted
    /// (snap_generation, snap_attempt) must NOT advance.
    backend->arm_interrupt = true;
    EXPECT_ANY_THROW(gc.runRegularRound());
    backend->arm_interrupt = false;

    /// Honest drive to fixpoint (advancing the mount ack each round). H1 predicts the re-fold of the -1
    /// underflows; the current code predicts a clean drain. Capture whichever happens.
    bool threw_undercount = false;
    try
    {
        for (int i = 0; i < 32; ++i)
        {
            gc.runRegularRound();
            store->renewWatermarkOnce();
        }
    }
    catch (const DB::Exception & e)
    {
        threw_undercount = (e.code() == DB::ErrorCodes::CORRUPTED_DATA
            && e.message().find("merged in-degree -1 < 0") != String::npos);
        if (!threw_undercount)
            throw;
        std::cerr << "H1 captured exception: " << e.message() << "\n";
    }

    if (threw_undercount)
    {
        FAIL() << "H1 REPRODUCED: the deposed removal fold underflowed on re-fold";
    }
    else
    {
        /// H1 did NOT reproduce with a single deposed round: the drain is clean.
        EXPECT_FALSE(blobExists(*backend, store->layout(), DB::UInt128(1)))
            << "H1-not-reproduced: the pool drained cleanly (single deposed round is idempotent)";
        EXPECT_EQ(inDegreeOf(*backend, store->layout(), DB::UInt128(1)), 0);
    }
}

/// ==================== H1b: A CONCURRENT-DROP REMOVAL IS FOLDED ONCE (IDEMPOTENCE) ====================
///
/// The idempotence claim that survives the redesign, without the (retired) fence-window framing: a removal
/// that lands AFTER a round's fold sealed its cursor but BEFORE that round's single commit CAS must be
/// folded EXACTLY ONCE by a later round — never re-folded to drive the blob in-degree below zero.
///
/// In the one-pass round there is a single gc/state CAS (fold -> publish -> commit). We inject the drop
/// just before that commit CAS lands, so the event (v2) is above the fold's sealed cursor (v1) this round.
/// The committed round adopts the fold seal at cursor v1; the next round folds (v1, v2] as an ordinary -1
/// against the still-live parent (blob 1 at in-degree 1 => 0). The source-edge SET model makes a re-fold
/// of the same removal a set-difference no-op, so the in-degree never underflows.
class DropAtCommitBackend : public InMemoryBackend
{
public:
    CasResult casPut(const String & key, const String & bytes, const std::optional<Token> & expected,
                     const ObjectMeta & meta) override
    {
        /// The one-pass round has a SINGLE gc/state CAS that advances snap_generation. Fire the injected
        /// drop ONCE, just before that CAS commits — so the drop event is above this round's sealed cursor.
        if (arm_drop && key == gc_state_key)
        {
            const auto stored = get(key);
            if (stored)
            {
                const GcState prev = decodeGcState(stored->bytes);
                const GcState next = decodeGcState(bytes);
                if (next.snap_generation > prev.snap_generation)
                {
                    arm_drop = false;
                    if (on_commit)
                        on_commit();
                }
            }
        }
        return InMemoryBackend::casPut(key, bytes, expected, meta);
    }

    bool arm_drop = false;
    String gc_state_key = "p/gc/state";
    std::function<void()> on_commit;
};

TEST(CasGcUndercount, H1bFenceWindowRemovalReFoldedNextRoundUnderflows)
{
    auto backend = std::make_shared<DropAtCommitBackend>();
    /// gc_fold_max_defer_rounds=0 forces fold-every-round: the injected drop fires from `on_commit`,
    /// which only runs on the round-commit CAS that ADVANCES snap_generation. With immutable logs an idle
    /// round DEFERS (never advancing the generation), so a default store would never fire the injection --
    /// forcing a fold each round keeps the fence-window injection point (and its re-fold) reachable.
    auto store = openPoolForTest(backend, /*gc_fold_max_defer_rounds*/ 0);
    ASSERT_EQ(store->layout().gcStateKey(), "p/gc/state");

    const RootNamespace ns{"00/aa@cas@"};
    const ManifestRef r = ref(1, 0xAA);
    writeBlobBody(*backend, store->layout(), DB::UInt128(1));
    writeManifestRaw(*backend, store->layout(), ns, r, {blobEntryFor("a", DB::UInt128(1))});
    publishCommittedTransition(*backend, store->layout(), ns, "tbl", std::nullopt, r);

    Gc gc(store, kGc);
    /// Round 1 (honest): fold +1, pin blob 1 at in-degree 1. Cursor sealed at v1.
    ASSERT_TRUE(gc.runRegularRound().acquired_lease);
    store->renewWatermarkOnce();
    ASSERT_EQ(inDegreeOf(*backend, store->layout(), DB::UInt128(1)), 1);

    /// Round 2: just before the round-commit CAS lands (after the fold sealed its cursor at v1), inject the
    /// DROP as v2. The fold this round saw only up to v1 (no change), so the sealed cursor stays v1; the
    /// drop event v2 is above it and survives trim. The NEXT round folds (v1, v2] => -1 on blob 1 against
    /// the still-live parent (in-degree 1 => 0). It must NOT be re-folded a second time.
    backend->arm_drop = true;
    backend->on_commit = [&]
    {
        dropRefTransition(*backend, store->layout(), ns, "tbl", r);
    };
    ASSERT_TRUE(gc.runRegularRound().acquired_lease);
    backend->arm_drop = false;

    /// CORRECT behaviour: the concurrently-dropped blob is reclaimed exactly once and GC stays quiescent —
    /// the removal folds ONCE (idempotent source-edge set), NEVER driving the in-degree below zero. Advance
    /// the mount ack each round so the ack floor graduates and deletes the condemned blob.
    EXPECT_NO_THROW({
        for (int i = 0; i < 12; ++i)
        {
            gc.runRegularRound();
            store->renewWatermarkOnce();
        }
    }) << "undercount: a concurrent-drop removal was re-folded and drove the blob in-degree < 0";

    EXPECT_FALSE(blobExists(*backend, store->layout(), DB::UInt128(1)))
        << "the concurrently-dropped blob must be reclaimed";
    EXPECT_EQ(inDegreeOf(*backend, store->layout(), DB::UInt128(1)), 0);
}
