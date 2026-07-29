#include <gtest/gtest.h>

#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Primitives/CasEvent.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Gc/CasGcShardPlan.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Gc/CasOrphanManifestSweep.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasPool.h>
#include "cas_test_helpers.h"
#include <limits>
#include <vector>

using namespace DB::Cas;
using namespace DB::Cas::tests;

namespace
{
constexpr uint64_t kWriterEpoch = 7;
const String kServerRoot = "00";
ManifestRef ref(uint64_t seq, uint64_t inst)
{
    return ManifestRef{.writer_epoch = kWriterEpoch, .build_sequence = seq, .manifest_ordinal = static_cast<uint32_t>(inst)};
}

/// The §6 deletion premise (`manifestDeletionPremise`) is a SECOND precondition on every deletion below,
/// alongside the watermark eligibility these tests are about: a manifest of an epoch-`E` build is
/// deletable only once the namespace's sealed fold cursor sits in an epoch strictly above `E`. Tests
/// whose subject is the eligibility or ownership rule therefore have to establish it, or they would
/// assert a deletion the premise (not the rule under test) prevented. Tests whose subject is RETENTION
/// deliberately do NOT call this — see `CasSweepDeletionPremise` for the premise's own coverage.
void seedConsumedSealCursor(InMemoryBackend & backend, const Layout & layout, const RootNamespace & ns)
{
    seedFoldCursorForTest(backend, layout, ns, RefTxnId{kWriterEpoch + 1, 1});
}
}

/// A staged-but-unowned body in an ELIGIBLE prefix, absent from the owner view, is deleted (#7).
TEST(CasOrphanManifestSweep, EligibleAndUnownedIsDeleted)
{
    auto backend = std::make_shared<InMemoryBackend>();
    auto store = openPoolForTest(backend);
    const RootNamespace ns{"00/aa@cas@"};
    registerNamespaceRaw(*backend, store->layout(), ns);
    const ManifestRef r = ref(5, 0xAB);
    writeManifestRaw(*backend, store->layout(), ns, r, {blobEntryFor("a", DB::UInt128(1))});   // body, no owner
    setWatermarkMinActive(*backend, store->layout(), kServerRoot, kWriterEpoch, /*min_active*/6);   // 6 > 5 => eligible
    seedConsumedSealCursor(*backend, store->layout(), ns);

    sweepNamespace(*store, ns, BuildPrefix{.writer_epoch = kWriterEpoch, .build_sequence = 5});
    EXPECT_FALSE(backend->head(store->layout().manifestKey(ManifestId{ns, r})).exists);
}

/// A body that IS in the owner view (committed) is NEVER swept (#8).
TEST(CasOrphanManifestSweep, OwnedBodyIsSkipped)
{
    auto backend = std::make_shared<InMemoryBackend>();
    auto store = openPoolForTest(backend);
    const RootNamespace ns{"00/aa@cas@"};
    const ManifestRef r = ref(5, 0xAB);
    writeManifestRaw(*backend, store->layout(), ns, r, {blobEntryFor("a", DB::UInt128(1))});
    publishCommittedTransition(*backend, store->layout(), ns, "tbl", std::nullopt, r);  // now owned
    setWatermarkMinActive(*backend, store->layout(), kServerRoot, kWriterEpoch, 6);

    sweepNamespace(*store, ns, BuildPrefix{.writer_epoch = kWriterEpoch, .build_sequence = 5});
    EXPECT_TRUE(backend->head(store->layout().manifestKey(ManifestId{ns, r})).exists);
}

/// GC-WEDGE regression (2026-07-10): a COMMITTED ref that has been DROPPED but whose removal `-1` is NOT
/// yet sealed (transition_version above the sealed fold cursor, which is 0 for this fresh pool) must
/// SURVIVE the sweep — the GC fold still needs the body to emit the `-1` (delete-after-sealed-decrements).
/// A promoted build retires its build_seq, so the prefix is watermark-eligible; before the fix the sweep
/// deleted the body in the dropRef→fold window → the removal-fold then clamped FOREVER on the missing
/// committed body → pool-wide GC stop. The pending-removal protection now covers COMMITTED (not only
/// PRECOMMIT) removals.
///
/// SINCE THE §6 PREMISE, this shape is held by TWO independent facts: the tail-removal protection this
/// test is named for, and the premise's rule (1) — the fixture seals no fold cursor, so epoch
/// `kWriterEpoch`'s closing seal is not consumed either. They cannot be separated HERE: the removal log
/// sits in a lower epoch than the build, so any cursor high enough to satisfy rule (1) would also sit
/// above the log and stop the tail scan from reading it at all. The case where the tail-removal
/// protection is the ONLY thing standing — a removal in a LATER epoch, which is the direction removals
/// actually cross — is `CasSweepDeletionPremise.AnUnconsumedTailRemovalRetainsItsTarget`.
TEST(CasOrphanManifestSweep, PendingCommittedRemovalBodyIsSkipped)
{
    auto backend = std::make_shared<InMemoryBackend>();
    auto store = openPoolForTest(backend);
    const RootNamespace ns{"00/aa@cas@"};
    const ManifestRef r = ref(5, 0xAB);
    writeManifestRaw(*backend, store->layout(), ns, r, {blobEntryFor("a", DB::UInt128(1))});
    publishCommittedTransition(*backend, store->layout(), ns, "tbl", std::nullopt, r);   // committed owner
    dropRefTransition(*backend, store->layout(), ns, "tbl", r);   // dropped: pending committed removal, -1 unsealed
    setWatermarkMinActive(*backend, store->layout(), kServerRoot, kWriterEpoch, 6);   // 6 > 5 => prefix eligible

    sweepNamespace(*store, ns, BuildPrefix{.writer_epoch = kWriterEpoch, .build_sequence = 5});
    EXPECT_TRUE(backend->head(store->layout().manifestKey(ManifestId{ns, r})).exists)
        << "a dropped-but-unsealed committed manifest body must survive the sweep (delete-after-sealed-"
           "decrements) — else the removal-fold clamps forever on the missing body (GC-WEDGE-2026-07-10)";
}

/// The sweep emits NO blob deltas: the in-degree generation is unchanged.
TEST(CasOrphanManifestSweep, EmitsNoBlobDeltas)
{
    auto backend = std::make_shared<InMemoryBackend>();
    auto store = openPoolForTest(backend);
    const RootNamespace ns{"00/aa@cas@"};
    const ManifestRef r = ref(5, 0xAB);
    writeManifestRaw(*backend, store->layout(), ns, r, {blobEntryFor("a", DB::UInt128(1))});
    setWatermarkMinActive(*backend, store->layout(), kServerRoot, kWriterEpoch, 6);
    seedConsumedSealCursor(*backend, store->layout(), ns);
    // The sweep must not advance the in-degree generation: capture it AFTER the fixture's own seal.
    const uint64_t gen_before = currentGenerationOf(*backend, store->layout());

    sweepNamespace(*store, ns, BuildPrefix{.writer_epoch = kWriterEpoch, .build_sequence = 5});
    EXPECT_EQ(currentGenerationOf(*backend, store->layout()), gen_before);
    EXPECT_EQ(inDegreeOf(*backend, store->layout(), DB::UInt128(1)), 0);
}

TEST(CasOrphanManifestSweep, CursorPageAdvancesAndWrapsWithListBudget)
{
    auto backend = std::make_shared<InMemoryBackend>();
    auto store = openPoolForTest(backend);
    const RootNamespace ns{"00/aa@cas@"};
    registerNamespaceRaw(*backend, store->layout(), ns);
    const ManifestRef r1 = ref(5, 0xE1);
    const ManifestRef r2 = ref(5, 0xE2);
    writeManifestRaw(*backend, store->layout(), ns, r1, {blobEntryFor("a", DB::UInt128(1))});
    writeManifestRaw(*backend, store->layout(), ns, r2, {blobEntryFor("b", DB::UInt128(2))});
    setWatermarkMinActive(*backend, store->layout(), kServerRoot, kWriterEpoch, /*min_active*/6);

    const ManifestSweepResult first = sweepManifestCursorPage(*store, "", /*list_budget*/1, /*delete_budget*/0);
    EXPECT_EQ(first.listed, 1u);
    EXPECT_FALSE(first.wrapped);
    EXPECT_FALSE(first.next_cursor.empty());

    const ManifestSweepResult second = sweepManifestCursorPage(*store, first.next_cursor, /*list_budget*/100, /*delete_budget*/0);
    EXPECT_GE(second.listed, 1u);
    EXPECT_TRUE(second.wrapped);
    EXPECT_TRUE(second.next_cursor.empty());
}

/// A NON-eligible prefix (no watermark fact) deletes NOTHING (#9: frozen-seq is not authority).
TEST(CasOrphanManifestSweep, NoWatermarkIsNotAuthority)
{
    auto backend = std::make_shared<InMemoryBackend>();
    auto store = openPoolForTest(backend);
    const RootNamespace ns{"00/aa@cas@"};
    const ManifestRef r = ref(5, 0xAB);
    writeManifestRaw(*backend, store->layout(), ns, r, {blobEntryFor("a", DB::UInt128(1))});
    // No setWatermarkMinActive — no durable fact => not eligible.
    sweepNamespace(*store, ns, BuildPrefix{.writer_epoch = kWriterEpoch, .build_sequence = 5});
    EXPECT_TRUE(backend->head(store->layout().manifestKey(ManifestId{ns, r})).exists);
}

TEST(CasOrphanManifestSweep, CursorPageDeletesEligibleUnownedBody)
{
    auto backend = std::make_shared<InMemoryBackend>();
    auto store = openPoolForTest(backend);
    const RootNamespace ns{"00/aa@cas@"};
    registerNamespaceRaw(*backend, store->layout(), ns);
    const ManifestRef r = ref(5, 0xAC);
    writeManifestRaw(*backend, store->layout(), ns, r, {blobEntryFor("a", DB::UInt128(1))});
    setWatermarkMinActive(*backend, store->layout(), kServerRoot, kWriterEpoch, 6);
    seedConsumedSealCursor(*backend, store->layout(), ns);

    const ManifestSweepResult result = sweepManifestCursorPage(*store, "", /*list_budget*/100, /*delete_budget*/10);
    EXPECT_GE(result.listed, 1u);
    EXPECT_EQ(result.deleted, 1u);
    EXPECT_FALSE(backend->head(store->layout().manifestKey(ManifestId{ns, r})).exists);
}

TEST(CasOrphanManifestSweep, CursorPageRespectsDeleteBudget)
{
    auto backend = std::make_shared<InMemoryBackend>();
    auto store = openPoolForTest(backend);
    const RootNamespace ns{"00/aa@cas@"};
    registerNamespaceRaw(*backend, store->layout(), ns);
    const ManifestRef r1 = ref(5, 0xAD);
    const ManifestRef r2 = ref(5, 0xAE);
    writeManifestRaw(*backend, store->layout(), ns, r1, {blobEntryFor("a", DB::UInt128(1))});
    writeManifestRaw(*backend, store->layout(), ns, r2, {blobEntryFor("b", DB::UInt128(2))});
    setWatermarkMinActive(*backend, store->layout(), kServerRoot, kWriterEpoch, 6);
    seedConsumedSealCursor(*backend, store->layout(), ns);

    const ManifestSweepResult result = sweepManifestCursorPage(*store, "", /*list_budget*/100, /*delete_budget*/1);
    EXPECT_EQ(result.deleted, 1u);
    const bool first_exists = backend->head(store->layout().manifestKey(ManifestId{ns, r1})).exists;
    const bool second_exists = backend->head(store->layout().manifestKey(ManifestId{ns, r2})).exists;
    EXPECT_NE(first_exists, second_exists);
}

TEST(CasOrphanManifestSweep, CursorPageSkipsOwnedBody)
{
    auto backend = std::make_shared<InMemoryBackend>();
    auto store = openPoolForTest(backend);
    const RootNamespace ns{"00/aa@cas@"};
    const ManifestRef r = ref(5, 0xAF);
    writeManifestRaw(*backend, store->layout(), ns, r, {blobEntryFor("a", DB::UInt128(1))});
    publishCommittedTransition(*backend, store->layout(), ns, "tbl", std::nullopt, r);
    setWatermarkMinActive(*backend, store->layout(), kServerRoot, kWriterEpoch, 6);

    const ManifestSweepResult result = sweepManifestCursorPage(*store, "", /*list_budget*/100, /*delete_budget*/10);
    EXPECT_EQ(result.deleted, 0u);
    EXPECT_TRUE(backend->head(store->layout().manifestKey(ManifestId{ns, r})).exists);
}

/// The LIST-based late-log detector that lived here is RETIRED with the sentinel seal, and it is worth
/// recording why rather than leaving a hole in this file's coverage story.
///
/// It existed because the old seal was a SNAPSHOT at a synthetic `{E-1, UINT64_MAX}` id: that object
/// occupied no `_log` key, so a dying predecessor's in-flight PUT could still land in the dead epoch and
/// the only possible response was to notice it afterwards and report it. INV-2's seal is a
/// TRANSACTION at exactly `{E, T+1}` -- the key that ghost would take -- so the store's own write-once
/// create refuses it. There is nothing left to detect at that shape: an id above the seal cannot be
/// minted either, because ids are state-derived and a writer that could derive `{E, T+2}` would have had
/// to observe the seal first.
///
/// `CasEventType::RefLateLogDetected` is retired WITH the detector -- pre-release, so a vocabulary entry
/// nothing can emit is just dead surface. Soak scenario S38 (`s38_late_put_injection.py`) keeps its
/// injection and FLIPS its assertion: from "the detection fired" to "the fence held" -- the late PUT's
/// conditional create must LOSE to the occupied slot, with zero data loss and the namespace folding
/// normally.

