#include <gtest/gtest.h>

#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasBlobInDegree.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasGc.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasStore.h>
#include "cas_test_helpers.h"

using namespace DB::Cas;
using namespace DB::Cas::tests;

namespace DB::ErrorCodes
{
    extern const int CORRUPTED_DATA;
}

namespace
{
const UInt128 kGc = hexToU128("00000000000000000000000000000001");
ManifestRef ref(const String &, uint64_t seq, uint64_t inst)
{
    return ManifestRef{.writer_epoch = 1, .build_sequence = seq, .manifest_ordinal = static_cast<uint32_t>(inst)};
}
}

/// Committed new_manifest => +1 per blob entry (BlobInDegreeMatchesActiveManifests).
/// After a fold, gc/state records snap_attempt == the folding leader's lease.seq, and the fold seal
/// lives under (snap_generation, snap_attempt).
TEST(CasGcFold, FoldAdoptsAttemptEqualsLeaseSeq)
{
    auto backend = std::make_shared<InMemoryBackend>();
    auto store = openStoreForTest(backend);
    const RootNamespace ns{"00/aa@cas@"};
    const ManifestRef r = ref("srv-a:1", 1, 0xAA);
    writeManifestRaw(*backend, store->layout(), ns, r, {blobEntryFor("a", DB::UInt128(1))});
    publishCommittedTransition(*backend, store->layout(), ns, "tbl", std::nullopt, r);

    Gc gc(store, kGc);
    gc.runRegularRound();

    const auto st = decodeGcState(backend->get(store->layout().gcStateKey())->bytes);
    EXPECT_EQ(st.snap_attempt, st.lease.seq);
    EXPECT_GT(st.snap_generation, 0u);
    /// The round's seal is durable under (snap_generation, snap_attempt) — a completed round leaves the
    /// completion seal at snap_generation; a fold-only round leaves the fold seal there. Either way the
    /// adopted attempt locates it (a seal under any other attempt would be unadopted debris).
    EXPECT_TRUE(backend->head(store->layout().completionSealKey(st.snap_generation, st.snap_attempt)).exists
                || backend->head(store->layout().foldSealKey(st.snap_generation, st.snap_attempt)).exists);
    /// The fold seal specifically lives under the adopted attempt at the fold generation (G_f = G_c - 1
    /// after completion; = snap_generation for a fold-only round).
    const uint64_t fold_gen = backend->head(store->layout().foldSealKey(st.snap_generation, st.snap_attempt)).exists
        ? st.snap_generation : st.snap_generation - 1;
    EXPECT_TRUE(backend->head(store->layout().foldSealKey(fold_gen, st.snap_attempt)).exists);
}

TEST(CasGcFold, CommittedAddEmitsPlusOnePerBlob)
{
    auto backend = std::make_shared<InMemoryBackend>();
    auto store = openStoreForTest(backend);
    const RootNamespace ns{"00/aa@cas@"};
    const ManifestRef r = ref("srv-a:1", 1, 0xAA);
    writeManifestRaw(*backend, store->layout(), ns, r,
        {blobEntryFor("a", DB::UInt128(1)), blobEntryFor("b", DB::UInt128(2))});
    publishCommittedTransition(*backend, store->layout(), ns, "tbl", std::nullopt, r);

    Gc gc(store, kGc);
    gc.runRegularRound();

    EXPECT_EQ(inDegreeOf(*backend, store->layout(), DB::UInt128(1)), 1);
    EXPECT_EQ(inDegreeOf(*backend, store->layout(), DB::UInt128(2)), 1);
}

/// Owner removal => -1 per blob entry; in-degree returns to 0.
TEST(CasGcFold, RemovalEmitsMinusOne)
{
    auto backend = std::make_shared<InMemoryBackend>();
    auto store = openStoreForTest(backend);
    const RootNamespace ns{"00/aa@cas@"};
    const ManifestRef r = ref("srv-a:1", 1, 0xAA);
    writeManifestRaw(*backend, store->layout(), ns, r, {blobEntryFor("a", DB::UInt128(1))});
    publishCommittedTransition(*backend, store->layout(), ns, "tbl", std::nullopt, r);
    Gc gc(store, kGc);
    gc.runRegularRound();
    EXPECT_EQ(inDegreeOf(*backend, store->layout(), DB::UInt128(1)), 1);
    dropRefTransition(*backend, store->layout(), ns, "tbl", r);
    gc.runRegularRound();
    EXPECT_EQ(inDegreeOf(*backend, store->layout(), DB::UInt128(1)), 0);
}

/// Precommit with a PRESENT, valid body => +1.
TEST(CasGcFold, PrecommitBodyPresentEmitsPlusOne)
{
    auto backend = std::make_shared<InMemoryBackend>();
    auto store = openStoreForTest(backend);
    const RootNamespace ns{"00/aa@cas@"};
    const ManifestRef r = ref("srv-a:1", 1, 0xAA);
    writeManifestRaw(*backend, store->layout(), ns, r, {blobEntryFor("a", DB::UInt128(1))});
    addPrecommitTransition(*backend, store->layout(), ns, DB::UInt128(7), "tbl", std::nullopt, r);
    Gc gc(store, kGc);
    gc.runRegularRound();
    EXPECT_EQ(inDegreeOf(*backend, store->layout(), DB::UInt128(1)), 1);
}

/// Precommit whose body is ABSENT => NO delta (control #4); the 404 must NOT throw.
TEST(CasGcFold, PrecommitMissingBodyEmitsNoDelta)
{
    auto backend = std::make_shared<InMemoryBackend>();
    auto store = openStoreForTest(backend);
    const RootNamespace ns{"00/aa@cas@"};
    const ManifestRef r = ref("srv-a:1", 1, 0xAA);
    addPrecommitTransition(*backend, store->layout(), ns, DB::UInt128(7), "tbl", std::nullopt, r);
    Gc gc(store, kGc);
    EXPECT_NO_THROW(gc.runRegularRound());
    EXPECT_EQ(inDegreeOf(*backend, store->layout(), DB::UInt128(1)), 0);
}

/// FOLD BARRIER (control #23): a LIVE precommit binding whose body is missing does NOT advance the
/// durable fold cursor past its activation event; when the body appears the cursor advances.
TEST(CasGcFold, FoldBarrierHaltsCursorAtLiveMissingBodyPrecommit)
{
    auto backend = std::make_shared<InMemoryBackend>();
    auto store = openStoreForTest(backend);
    const RootNamespace ns{"00/aa@cas@"};
    const ManifestRef r = ref("srv-a:1", 1, 0xAA);
    const uint64_t v = addPrecommitTransition(*backend, store->layout(), ns, DB::UInt128(7), "tbl", std::nullopt, r);
    Gc gc(store, kGc);
    EXPECT_NO_THROW(gc.runRegularRound());
    EXPECT_LT(foldCursorOf(*backend, store->layout(), ns, 0), v);   // barrier: halted at the activation

    writeManifestRaw(*backend, store->layout(), ns, r, {blobEntryFor("a", DB::UInt128(1))});
    gc.runRegularRound();
    EXPECT_EQ(inDegreeOf(*backend, store->layout(), DB::UInt128(1)), 1);
    EXPECT_GE(foldCursorOf(*backend, store->layout(), ns, 0), v);   // barrier lifted by activation
}

/// Promote of an already-activated precommit is a PURE OWNER MOVE: NO delta, body not condemned.
TEST(CasGcFold, PromoteOfActivatedPrecommitEmitsNoDelta)
{
    auto backend = std::make_shared<InMemoryBackend>();
    auto store = openStoreForTest(backend);
    const RootNamespace ns{"00/aa@cas@"};
    const ManifestRef r = ref("srv-a:1", 1, 0xAA);
    writeManifestRaw(*backend, store->layout(), ns, r, {blobEntryFor("a", DB::UInt128(1))});
    addPrecommitTransition(*backend, store->layout(), ns, DB::UInt128(7), "tbl", std::nullopt, r);
    Gc gc(store, kGc);
    gc.runRegularRound();
    EXPECT_EQ(inDegreeOf(*backend, store->layout(), DB::UInt128(1)), 1);

    promoteTransition(*backend, store->layout(), ns, DB::UInt128(7), "tbl", r);
    gc.runRegularRound();
    EXPECT_EQ(inDegreeOf(*backend, store->layout(), DB::UInt128(1)), 1);   // unchanged, still pinned
    EXPECT_TRUE(backend->head(store->layout().manifestKey(ManifestId{ns, r})).exists);   // not condemned
}

/// Committed add naming a MISSING body (404) => clamp + anomaly, never a guessed +1, never a throw.
TEST(CasGcFold, CommittedMissingBodyClampsCursorAndRecordsAnomaly)
{
    auto backend = std::make_shared<InMemoryBackend>();
    auto store = openStoreForTest(backend);
    const RootNamespace ns{"00/aa@cas@"};
    const ManifestRef r = ref("srv-a:1", 1, 0xAA);
    const uint64_t v = publishCommittedTransition(*backend, store->layout(), ns, "tbl", std::nullopt, r);  // no body
    Gc gc(store, kGc);
    RoundReport report;
    EXPECT_NO_THROW(report = gc.runRegularRound());
    EXPECT_TRUE(report.hasAnomaly(ns, /*shard*/0));
    EXPECT_EQ(inDegreeOf(*backend, store->layout(), DB::UInt128(1)), 0);
    EXPECT_LT(foldCursorOf(*backend, store->layout(), ns, 0), v);
}

/// A body whose self-ref disagrees (PRESENT but INVALID) => hard fail closed (controls #19/#20).
TEST(CasGcFold, RefMismatchFailsClosed)
{
    auto backend = std::make_shared<InMemoryBackend>();
    auto store = openStoreForTest(backend);
    const RootNamespace ns{"00/aa@cas@"};
    const ManifestRef r = ref("srv-a:1", 1, 0xAA);
    PartManifest bad;
    bad.ref = ref("srv-a:1", 1, 0xBB);   // != r
    bad.root_namespace_id = ns;
    bad.entries = {blobEntryFor("a", DB::UInt128(1))};
    bad.payload_digest = computePayloadDigest(bad);
    backend->putIfAbsent(store->layout().manifestKey(ManifestId{ns, r}), encodePartManifest(bad));
    publishCommittedTransition(*backend, store->layout(), ns, "tbl", std::nullopt, r);
    Gc gc(store, kGc);
    expectThrowsCode(DB::ErrorCodes::CORRUPTED_DATA, [&]{ gc.runRegularRound(); });
}

/// Owner-removal whose OLD committed body is gone at removal-fold => clamp + anomaly, no partial -1.
TEST(CasGcFold, RemovalWithMissingOldBodyClampsAndRecordsAnomaly)
{
    auto backend = std::make_shared<InMemoryBackend>();
    auto store = openStoreForTest(backend);
    const RootNamespace ns{"00/aa@cas@"};
    const ManifestRef r = ref("srv-a:1", 1, 0xAA);
    writeManifestRaw(*backend, store->layout(), ns, r, {blobEntryFor("a", DB::UInt128(1))});
    publishCommittedTransition(*backend, store->layout(), ns, "tbl", std::nullopt, r);
    Gc gc(store, kGc);
    gc.runRegularRound();   // +1; blob 1 in-degree 1

    const uint64_t removal_version = dropRefTransition(*backend, store->layout(), ns, "tbl", r);
    deleteManifestBody(*backend, store->layout(), ManifestId{ns, r});   // body gone before its decrement

    RoundReport report;
    EXPECT_NO_THROW(report = gc.runRegularRound());
    EXPECT_EQ(inDegreeOf(*backend, store->layout(), DB::UInt128(1)), 1);   // unchanged: no silent -1
    EXPECT_TRUE(report.hasAnomaly(ns, /*shard*/0));
    EXPECT_LT(foldCursorOf(*backend, store->layout(), ns, 0), removal_version);
}
