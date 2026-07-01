#include <gtest/gtest.h>

#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasBlobInDegree.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasGc.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasGcFormats.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasStore.h>
#include "cas_test_helpers.h"

#include <functional>

/// PHASE-1 ROOT-CAUSE REPRO for the soak S04 failure:
///   Code: 246 CORRUPTED_DATA: CAS blob in-degree: merged in-degree -1 < 0 for a blob ...
///   (undercount — fail closed rather than over-delete)
///
/// The guard lives in CasBlobInDegree.cpp `foldDeltasIntoGeneration` (merged = prior_count + delta_sum;
/// if merged < 0 throw). It fires when the FOLDED owner-REMOVE deltas for a blob edge exceed the FOLDED
/// owner-ADD deltas. This file constructs the minimal deterministic event sequences for the two
/// competing hypotheses and captures the exact failing assertion.
///
/// These tests are INVESTIGATION-ONLY. They deliberately encode an incorrect / hazardous event stream to
/// prove which stream drives a blob's merged in-degree below zero. They are NOT a regression fixture yet
/// (a fix would change what these assert), so they are quarantined under the CasGcUndercount suite.

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
    return b.head(layout.blobKey(BlobId(u128ToHex(hash)))).exists;
}

/// Append a RAW owner event to a shard journal WITHOUT going through the semantic helpers, so a test can
/// stage an event whose old_binding does not correspond to the shard's current owner (the "drop THEN
/// repoint-from-r1" hazard the fold has no way to reject at fold time — it dispatches purely on the
/// event's own old/new manifest_ref pair). Mirrors appendOwnerEvent's CAS loop but takes the bindings
/// verbatim and does not maintain `refs` (the fold reads the journal, not refs).
uint64_t appendRawOwnerEvent(
    InMemoryBackend & backend, const Layout & layout, const RootNamespace & ns, uint64_t shard,
    std::optional<OwnerBinding> old_binding, std::optional<OwnerBinding> new_binding)
{
    registerNamespaceRaw(backend, layout, ns);
    const String key = layout.rootShardKey(ns, shard);
    while (true)
    {
        const auto got = backend.get(key);
        RootShard root;
        std::optional<Token> expected;
        if (got)
        {
            root = decodeRootShard(got->bytes);
            expected = got->token;
        }
        const uint64_t version = root.shard_version + 1;
        root.shard_version = version;
        root.journal.push_back(RootOwnerEvent{
            .transition_version = version, .old_binding = old_binding, .new_binding = new_binding});
        if (backend.casPut(key, encodeRootShard(root), expected).outcome == CasOutcome::Committed)
            return version;
    }
}

OwnerBinding committed(const String & ref_name, const ManifestRef & r)
{
    return OwnerBinding{.owner_kind = OwnerKind::Committed, .ref_name = ref_name, .build_id = {}, .manifest_ref = r};
}

}

/// ============================ H2: SOURCE DOUBLE-COUNT OF -1 ============================
///
/// Hypothesis H2: two DISTINCT journal events each carry `old = committed(r1)`, so r1's blobs receive a
/// `-1` twice in ONE fold round while r1's body is still present. A blob that r1 pins but the re-pointing
/// event's new body does NOT re-reference is driven to prior(1) + (-1) + (-1) = -1 => the fold throws.
///
/// This is the hazard documented in gtest_cas_gc_round.cpp:550-554
/// (RepublishDuringFenceWindowSparesOnlyReReferencedBlob): "A separate drop THEN repoint would
/// double-count the -1 on r1's blobs and drive blob 2 to -1 — an undercount the in-degree fold fails
/// closed on."
TEST(CasGcUndercount, H2_DropThenRepointFromSameOldDoubleCountsMinusOne)
{
    auto backend = std::make_shared<InMemoryBackend>();
    auto store = openStoreForTest(backend);
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

    /// Now stage TWO distinct removal-carrying events for r1 in ONE fold window (r1's body is NOT deleted
    /// until recheck, so BOTH events resolve r1's edges at fold time):
    ///   v2: DROP r1        {old=committed(r1), new=none}          => -1 on {1,2}
    ///   v3: REPOINT r1->r2 {old=committed(r1), new=committed(r2)} => -1 on {1,2}, +1 on {1}
    /// Blob 2 nets prior(1) + (-1) + (-1) = -1  =>  the in-degree fold fails closed.
    appendRawOwnerEvent(*backend, store->layout(), ns, 0, committed("tbl", r1), std::nullopt);
    appendRawOwnerEvent(*backend, store->layout(), ns, 0, committed("tbl", r1), committed("tbl", r2));

    /// The next fold round streams v2 and v3 and folds the doubled -1 on blob 2 to -1.
    bool threw_undercount = false;
    try
    {
        for (int i = 0; i < 8; ++i)
            gc.runRegularRound();
    }
    catch (const DB::Exception & e)
    {
        threw_undercount = (e.code() == DB::ErrorCodes::CORRUPTED_DATA
            && e.message().find("merged in-degree -1 < 0") != String::npos);
        if (!threw_undercount)
            throw;
        std::cerr << "H2 captured exception: " << e.message() << "\n";
    }
    EXPECT_TRUE(threw_undercount)
        << "H2: a drop THEN repoint-from-r1 must double-count the -1 on r1's blobs and drive blob 2 to -1";
    (void)blobExists;
}

/// ============================ H1: CURSOR RE-FOLD UNDER ABORT ============================
///
/// Hypothesis H1: a removal `-1` is folded, but the fold-adopt CAS that would durably advance the cursor
/// past that removal LOSES to a concurrent leader (ABORTED). The cursor is NOT advanced, so a later
/// honest round RE-FOLDS the same removal against a parent generation whose in-degree for that blob has
/// already reached 0 => -1.
///
/// We reproduce the deposed-fold-adopt injection from gtest_cas_gc_attempt.cpp
/// (DeposedFoldAttemptDoesNotWedge): deny exactly the fold-adopt gc/state CAS #1 of the round that folds
/// the drop's -1. If the durable in-degree parent used by the retry has ALREADY absorbed the -1 (i.e. the
/// deposed round left a partial durable in-degree run adopted by a later reader), the retry re-applies the
/// -1 and underflows.
class InterruptFoldAdoptBackend : public InMemoryBackend
{
public:
    CasResult casPut(const String & key, const String & bytes, const std::optional<Token> & expected,
                     const ObjectMeta & meta = {}) override
    {
        if (arm_interrupt && key == gc_state_key)
        {
            const auto stored = get(key);
            const uint64_t stored_gen = stored ? decodeGcState(stored->bytes).snap_generation : 0;
            const uint64_t next_gen = decodeGcState(bytes).snap_generation;
            if (next_gen > stored_gen && !hasDurableRetiredSet())
            {
                arm_interrupt = false;
                throw DB::Exception(DB::ErrorCodes::ABORTED,
                    "test-injected: fold-adopt CAS #1 denied (leader deposed mid-fold; lease lost)");
            }
        }
        return InMemoryBackend::casPut(key, bytes, expected, meta);
    }

    bool arm_interrupt = false;
    String gc_state_key = "p/gc/state";

private:
    bool hasDurableRetiredSet()
    {
        String cursor;
        for (;;)
        {
            const ListPage page = list("", cursor, 1024);
            for (const auto & item : page.keys)
                if (item.key.find("/retired/") != String::npos)
                    return true;
            if (page.next_cursor.empty())
                return false;
            cursor = page.next_cursor;
        }
    }
};

TEST(CasGcUndercount, H1_DrainAfterDeposedRemovalFoldDoesNotUnderflow)
{
    auto backend = std::make_shared<InterruptFoldAdoptBackend>();
    auto store = openStoreForTest(backend);
    ASSERT_EQ(store->layout().gcStateKey(), "p/gc/state");

    const RootNamespace ns{"00/aa@cas@"};
    const ManifestRef r = ref(1, 0xAA);
    writeBlobBody(*backend, store->layout(), DB::UInt128(1));
    writeManifestRaw(*backend, store->layout(), ns, r, {blobEntryFor("a", DB::UInt128(1))});
    publishCommittedTransition(*backend, store->layout(), ns, "tbl", std::nullopt, r);

    Gc gc(store, kGc);
    /// Round 1 (honest): fold +1, pin blob 1.
    ASSERT_TRUE(gc.runRegularRound().acquired_lease);
    ASSERT_EQ(inDegreeOf(*backend, store->layout(), DB::UInt128(1)), 1);

    /// Drop the only ref and advance the watermark floor.
    dropRefTransition(*backend, store->layout(), ns, "tbl", r);
    store->renewWatermarkOnce();

    /// Round 2 (DEPOSED): fold the -1, then the fold-adopt CAS #1 is denied (ABORTED). The adopted
    /// (snap_generation, snap_attempt) must NOT advance.
    backend->arm_interrupt = true;
    EXPECT_ANY_THROW(gc.runRegularRound());
    backend->arm_interrupt = false;

    /// Honest drive to fixpoint. H1 predicts the re-fold of the -1 underflows; the current code predicts a
    /// clean drain. Capture whichever happens.
    bool threw_undercount = false;
    try
    {
        for (int i = 0; i < 32; ++i)
            gc.runRegularRound();
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

/// ==================== H1b: FENCE-WINDOW REMOVAL RE-FOLDED NEXT ROUND ====================
///
/// A NATURAL (write-path-legal) mechanism that produces the SAME doubled `-1` as H2 without any
/// hand-crafted "old=r1 twice" event. The round order is fold -> retire -> fence -> recheck -> trim.
///   * fold seals the per-shard cursor at `folded_cursor` (= shard_version at fold time).
///   * recheck RE-STREAMS the window (folded_cursor, fence_version] and folds its deltas into the
///     COMPLETION generation (CasGc.cpp:717-730).
///   * BUT the completion seal carries `folded_cursors = fold_seal.per_ns_shard` (CasGc.cpp:963) — the
///     PRE-window fold cursor. It does NOT advance past the events recheck just folded.
///   * trim removes only events <= folded_cursor, so a window event (> folded_cursor) SURVIVES.
/// => The NEXT round's fold re-reads `folded_cursor` and RE-FOLDS the window's `-1` on top of the
///    completion generation that already absorbed it. Blob dropped to 0 in the completion gen goes to -1.
///
/// To land a removal in the fence window we drop the ref DURING the round, after the fold sealed its
/// cursor but before the fence observes shard_version. We inject at the retire gc/state CAS (which runs
/// after fold, before fence): the injection appends the drop event to the shard journal exactly once.
class DropAtRetireBackend : public InMemoryBackend
{
public:
    CasResult casPut(const String & key, const String & bytes, const std::optional<Token> & expected,
                     const ObjectMeta & meta = {}) override
    {
        /// The retire CAS advances gc/state.round without advancing snap_generation (fold already did
        /// that). Detect it: gc/state key, snap_generation unchanged, round increased. Fire the injected
        /// drop ONCE, just before that CAS commits — so the event is > folded_cursor and <= fence_version.
        if (arm_drop && key == gc_state_key)
        {
            const auto stored = get(key);
            if (stored)
            {
                const GcState prev = decodeGcState(stored->bytes);
                const GcState next = decodeGcState(bytes);
                if (next.snap_generation == prev.snap_generation && next.round > prev.round)
                {
                    arm_drop = false;
                    if (on_retire)
                        on_retire();
                }
            }
        }
        return InMemoryBackend::casPut(key, bytes, expected, meta);
    }

    bool arm_drop = false;
    String gc_state_key = "p/gc/state";
    std::function<void()> on_retire;
};

TEST(CasGcUndercount, H1b_FenceWindowRemovalReFoldedNextRoundUnderflows)
{
    auto backend = std::make_shared<DropAtRetireBackend>();
    auto store = openStoreForTest(backend);
    ASSERT_EQ(store->layout().gcStateKey(), "p/gc/state");

    const RootNamespace ns{"00/aa@cas@"};
    const ManifestRef r = ref(1, 0xAA);
    writeBlobBody(*backend, store->layout(), DB::UInt128(1));
    writeManifestRaw(*backend, store->layout(), ns, r, {blobEntryFor("a", DB::UInt128(1))});
    publishCommittedTransition(*backend, store->layout(), ns, "tbl", std::nullopt, r);

    Gc gc(store, kGc);
    /// Round 1 (honest): fold +1, pin blob 1 at in-degree 1. Cursor sealed at v1.
    ASSERT_TRUE(gc.runRegularRound().acquired_lease);
    ASSERT_EQ(inDegreeOf(*backend, store->layout(), DB::UInt128(1)), 1);

    store->renewWatermarkOnce();

    /// Round 2: at the retire CAS (after fold sealed its cursor at v1, before the fence observes the
    /// journal) inject the DROP as v2. The fold this round saw only up to v1 (no change), so folded_cursor
    /// stays v1; the fence then observes shard_version = v2; recheck folds (v1, v2] => -1 on blob 1 into
    /// the completion generation, driving it to 0. The completion seal keeps folded_cursor = v1.
    backend->arm_drop = true;
    backend->on_retire = [&]
    {
        dropRefTransition(*backend, store->layout(), ns, "tbl", r);
    };
    ASSERT_TRUE(gc.runRegularRound().acquired_lease);
    backend->arm_drop = false;

    /// After round 2: blob 1's in-degree in the completion generation is 0 (recheck folded the -1).
    /// The drop event v2 is STILL in the journal (trim only removes <= folded_cursor = v1).
    /// Round 3's fold re-reads folded_cursor = v1 and re-folds (v1, v2] => a SECOND -1 on top of the
    /// completion generation that already has blob 1 at 0 => -1 => the in-degree fold fails closed.
    ///
    /// CORRECT behaviour (what this test asserts, so it fails RED on the buggy tree): a concurrent drop
    /// landing in the fence window must reclaim blob 1 exactly once and leave GC quiescent — NEVER throw
    /// the undercount. The throw below is the BUG (S04): a fence-window removal folded by recheck is
    /// re-folded by the next round because the sealed cursor was not advanced past it.
    EXPECT_NO_THROW({
        for (int i = 0; i < 8; ++i)
            gc.runRegularRound();
    }) << "S04 undercount: a fence-window removal was re-folded next round and drove the blob in-degree < 0";

    EXPECT_FALSE(blobExists(*backend, store->layout(), DB::UInt128(1)))
        << "the concurrently-dropped blob must be reclaimed";
    EXPECT_EQ(inDegreeOf(*backend, store->layout(), DB::UInt128(1)), 0);
}
