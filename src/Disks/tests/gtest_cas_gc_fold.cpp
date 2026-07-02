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
    /// The one-pass round's fold seal is durable under (snap_generation, snap_attempt) — the adopted
    /// attempt locates it (a seal under any other attempt would be unadopted debris).
    EXPECT_TRUE(backend->head(store->layout().foldSealKey(st.snap_generation, st.snap_attempt)).exists);
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

namespace
{
/// Inject a gc/state with the given lease owner+seq and snap coordinates into `backend`.
/// round=0 ensures `tryResumeIncompleteRound` returns early (round == 0 => no resume).
void injectGcStateForFoldTest(
    DB::Cas::Backend & backend, const DB::Cas::Layout & layout,
    DB::UInt128 lease_owner, uint64_t lease_seq,
    uint64_t snap_generation, uint64_t snap_attempt, uint64_t gc_shards = 1)
{
    DB::Cas::GcState state;
    state.round = 0;
    state.gc_shards = gc_shards;
    state.snap_generation = snap_generation;
    state.snap_attempt = snap_attempt;
    state.lease.owner = lease_owner;
    state.lease.seq = lease_seq;
    const DB::Cas::HeadResult h = backend.head(layout.gcStateKey());
    if (h.exists)
        backend.putOverwrite(layout.gcStateKey(), DB::Cas::encodeGcState(state), h.token);
    else
        backend.putIfAbsent(layout.gcStateKey(), DB::Cas::encodeGcState(state));
}

/// Inject a fold seal at `(generation, attempt)` carrying a stale coverage for `(ns, shard)`:
/// `folded_cursor` is the stale cursor and `stale_incarnation` is the old incarnation that no
/// longer matches the live shard.
void injectStaleFoldSeal(
    DB::Cas::Backend & backend, const DB::Cas::Layout & layout,
    const DB::Cas::RootNamespace & ns, uint64_t shard,
    uint64_t generation, uint64_t attempt,
    uint64_t folded_cursor, DB::Cas::ShardIncarnation stale_incarnation)
{
    DB::Cas::CasFoldSeal seal;
    seal.generation = generation;
    DB::Cas::ShardCoverage cov;
    cov.folded_cursor = folded_cursor;
    cov.incarnation = stale_incarnation;
    cov.classification = 2;
    seal.per_ns_shard[cursorKeyForTest(ns, shard)] = cov;
    backend.putIfAbsent(layout.foldSealKey(generation, attempt), DB::Cas::encodeFoldSeal(seal));
}
}

/// ABA hazard, gc_shards=1: a shard sealed at incarnation I1 with folded_cursor=5 is recreated at
/// incarnation I2 with a fresh journal starting at version 1. Without the incarnation-keyed reset,
/// the fold keeps cursor=5 and skips the new event (1 <= 5). With the fix, the cursor is reset to 0
/// on mismatch and the event is applied => blob b1 gets in-degree 1.
TEST(CasGcFold, IncarnationMismatchRestartsFoldAtZero)
{
    const ShardIncarnation I1{1, 1};   /// old incarnation (stale sealed)
    const ShardIncarnation I2{1, 2};   /// new incarnation (live shard)

    auto backend = std::make_shared<InMemoryBackend>();
    auto store = openStoreForTest(backend);
    const RootNamespace ns{"00/aa@cas@"};
    const ManifestRef r = ref("srv-a:1", 1, 0xBB);

    /// 1. Create the shard with live incarnation I2 (write raw with I2 set, then add one event).
    ///    `publishRaw` creates the shard if absent; `publishCommittedTransition` preserves incarnation
    ///    because `appendOwnerEvent` does a read-modify-CAS that carries all existing fields.
    {
        RootShard root;
        root.incarnation = I2;
        publishRaw(*backend, store->layout(), ns, 0, root);
    }
    writeManifestRaw(*backend, store->layout(), ns, r, {blobEntryFor("a", DB::UInt128(1))});
    publishCommittedTransition(*backend, store->layout(), ns, "tbl", std::nullopt, r);
    /// Shard is now at version=1, incarnation=I2.

    /// 2. Inject a stale fold seal at gen=1, attempt=1: cursor=5 (beyond version 1), incarnation=I1.
    injectStaleFoldSeal(*backend, store->layout(), ns, 0, 1, 1, 5, I1);

    /// 3. Inject gc/state pointing to snap_generation=1, snap_attempt=1 so the fold reads the stale seal.
    injectGcStateForFoldTest(*backend, store->layout(), kGc, 1, 1, 1, 1);

    /// 4. Run the round; the fold must reset cursor to 0 on mismatch and apply the add event.
    Gc gc(store, kGc);
    EXPECT_NO_THROW(gc.runRegularRound());

    /// 5. If cursor reset => event (transition_version=1) is applied => in-degree 1.
    ///    If cursor kept at 5 => event skipped (1 <= 5) => in-degree 0 (failing test).
    EXPECT_EQ(inDegreeOf(*backend, store->layout(), DB::UInt128(1)), 1)
        << "incarnation mismatch must reset fold cursor to 0 so the new shard's events are applied";
}

/// Same ABA hazard scenario with gc_shards=2: the root shard is visited exactly once by the fold
/// loop; deltas are bucketed by blob hash before being passed to the per-target-shard reducers.
/// Blob UInt128(1) has high-64=0, so blobShard(UInt128(1), 2) == 0 => the edge lands in target
/// shard 0. `inDegreeOf` reads shard 0, so the same EXPECT is correct.
TEST(CasGcFold, IncarnationMismatchRestartsFoldAtZeroMultiShard)
{
    const ShardIncarnation I1{2, 1};
    const ShardIncarnation I2{2, 2};

    auto backend = std::make_shared<InMemoryBackend>();
    /// gc_shards=2 exercises the sharded bucket path in fold().
    auto store = DB::Cas::Store::open(
        backend,
        DB::Cas::PoolConfig{.pool_prefix = "p", .server_root_id = "test", .root_shards = 1,
                            .gc_shards = 2, .gc_trim_min_events = 0});

    const RootNamespace ns{"00/bb@cas@"};
    const ManifestRef r = ref("srv-b:1", 1, 0xCC);

    {
        RootShard root;
        root.incarnation = I2;
        publishRaw(*backend, store->layout(), ns, 0, root);
    }
    writeManifestRaw(*backend, store->layout(), ns, r, {blobEntryFor("a", DB::UInt128(1))});
    publishCommittedTransition(*backend, store->layout(), ns, "tbl", std::nullopt, r);

    injectStaleFoldSeal(*backend, store->layout(), ns, 0, 1, 1, 5, I1);
    /// gc_shards=2 must be in the injected state so the fold picks the sharded path.
    injectGcStateForFoldTest(*backend, store->layout(), kGc, 1, 1, 1, 2);

    Gc gc(store, kGc);
    EXPECT_NO_THROW(gc.runRegularRound());

    EXPECT_EQ(inDegreeOf(*backend, store->layout(), DB::UInt128(1)), 1)
        << "gc_shards=2: incarnation mismatch must reset cursor to 0 and apply the new shard's events";
}
