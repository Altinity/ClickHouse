#include <gtest/gtest.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasGc.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasGenerationSeal.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasInMemoryBackend.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasStore.h>
#include <Disks/tests/cas_test_helpers.h>

using namespace DB::Cas;
using DB::Cas::tests::writeManifestRaw;
using DB::Cas::tests::addPrecommitTransition;
using DB::Cas::tests::publishCommittedTransition;
using DB::Cas::tests::appendOwnerEvent;
using DB::Cas::tests::setWatermarkMinActive;
using DB::Cas::tests::cursorKeyForTest;

namespace
{

/// Same opener convention as the sibling GC gtests (`gtest_cas_gc_leak.cpp` and friends): one root
/// shard so cursor keys are predictable ("ns/0"), server_root_id "test" for the Store's OWN writer
/// bootstrap (irrelevant to this repro -- the watermark that governs `ns` below is resolved purely
/// from `ns`'s own string prefix via `floorForNamespace`, independent of this Store's server_root_id).
StorePtr openTestStore(std::shared_ptr<InMemoryBackend> & out_backend)
{
    out_backend = std::make_shared<InMemoryBackend>();
    return Store::open(out_backend, PoolConfig{.pool_prefix = "p", .server_root_id = "test", .root_shards = 1});
}

}

/// An abandoned precommit (never promoted, never removed) on a content-static ref-shard is parked by the
/// fold Skip optimization; once the watermark advances past its build_sequence it is PROVABLY dead, yet
/// `reclaimAbandonedPrecommit` never re-runs for the parked shard, so its manifest orphans. Fixed in Task 4.
TEST(CasDanglingPrecommit, AbandonedPrecommitOrphansManifestUntilFix)
{
    std::shared_ptr<InMemoryBackend> backend;
    auto store = openTestStore(backend);
    ASSERT_TRUE(backend->supportsListTokens()) << "repro needs the token-diff Skip to engage";
    const Layout & layout = store->layout();

    const String server_root_id = "srv";
    const RootNamespace ns{"srv/tbl@cas@"};
    const uint64_t shard = 0;
    const uint64_t dead_epoch = 1;
    const uint64_t precommit_seq = 5;   /// build_sequence of the abandoned precommit

    /// A precommit manifest whose body is PRESENT (so it activates and folds), then NEVER promoted/removed.
    const ManifestId id = writeManifestRaw(*backend, layout, ns,
        ManifestRef{.writer_epoch = dead_epoch, .build_sequence = precommit_seq, .manifest_ordinal = 1}, {});
    addPrecommitTransition(*backend, layout, ns, /*build_id*/ hexToU128("000000000000000000000000000000aa"),
        /*final_ref_name*/ "all_0_0_0", /*old_ref*/ std::nullopt, id.ref, shard);

    /// Precommit still ALIVE (min_active <= precommit_seq): fold it in, seal coverage, do NOT reclaim.
    setWatermarkMinActive(*backend, layout, server_root_id, dead_epoch, /*min_active*/ precommit_seq);
    Gc gc(store, hexToU128("00000000000000000000000000000101"));
    gc.runRegularRound();
    ASSERT_TRUE(backend->head(layout.manifestKey(id)).exists) << "body present while precommit alive";

    /// Watermark advances PAST the precommit (other builds retired): it is now provably dead.
    setWatermarkMinActive(*backend, layout, server_root_id, dead_epoch, /*min_active*/ precommit_seq + 1);

    /// Drive several rounds. The shard is token-stable => Skipped => reclaim never runs.
    for (int i = 0; i < 5; ++i)
        gc.runRegularRound();

    /// FIXED: the watermark-dead precommit forces a re-fold => reclaimAbandonedPrecommit emits the removal
    /// => the fold folds the -1 => R6 deletes the owner-removed manifest body.
    EXPECT_FALSE(backend->head(layout.manifestKey(id)).exists)
        << "POST-FIX: dangling precommit manifest is reclaimed once the watermark proves it dead";
}

/// Scaffold (Task 3): ShardCoverage.has_live_precommit / min_live_precommit_* round-trip through the
/// fold seal codec. Nothing consumes these fields yet (Task 4 wires them into computeDiscoverDecisions).
TEST(CasDanglingPrecommit, ShardCoverageRoundTripsMinLivePrecommit)
{
    CasFoldSeal seal;
    seal.generation = 3;
    seal.parent_generation = 2;
    ShardCoverage cov;
    cov.classification = 1;
    cov.folded_cursor = 7;
    cov.has_live_precommit = true;
    cov.min_live_precommit_writer_epoch = 1;
    cov.min_live_precommit_build_sequence = 5;
    seal.per_ns_shard["srv/tbl@cas@/0"] = cov;

    const CasFoldSeal back = decodeFoldSeal(encodeFoldSeal(seal));
    const ShardCoverage & r = back.per_ns_shard.at("srv/tbl@cas@/0");
    EXPECT_TRUE(r.has_live_precommit);
    EXPECT_EQ(r.min_live_precommit_writer_epoch, 1u);
    EXPECT_EQ(r.min_live_precommit_build_sequence, 5u);

    /// Default (no live precommit) round-trips as absent.
    CasFoldSeal empty_seal;
    empty_seal.per_ns_shard["srv/tbl@cas@/1"] = ShardCoverage{};
    const CasFoldSeal e_back = decodeFoldSeal(encodeFoldSeal(empty_seal));
    EXPECT_FALSE(e_back.per_ns_shard.at("srv/tbl@cas@/1").has_live_precommit);
}

/// Local re-declaration of the counter (same convention `gtest_cas_store.cpp` uses for `CasManifest*`
/// events): the enumerator is DEFINED in `ProfileEvents.cpp` and forward-declared per-TU, never exported
/// via a header.
namespace ProfileEvents
{
extern const Event CasGcPrecommitRevisitForced;
}

/// Task 5 (a): once the fix reclaims a watermark-dead precommit, the force-Read guard must not keep
/// firing forever -- the reclaim's removal drops `has_live_precommit` from the NEXT fold's coverage
/// (see the `minLivePrecommit` stamp in `Gc::fold`), so `computeDiscoverDecisions` has nothing left to
/// force and the shard settles back to `Skip`. Self-termination is observed via the SAME accessor
/// `gtest_cas_store.cpp` uses for `CasGc*`/`CasManifest*` ProfileEvents
/// (`ProfileEvents::global_counters[...]`, see `HardLimitBlocksPromoteBeforeCommit`): the
/// `CasGcPrecommitRevisitForced` counter must stop incrementing once the shard is clean.
TEST(CasDanglingPrecommit, ReclaimIsIdempotentAndSelfTerminating)
{
    std::shared_ptr<InMemoryBackend> backend;
    auto store = openTestStore(backend);
    const Layout & layout = store->layout();

    const String server_root_id = "srv";
    const RootNamespace ns{"srv/tbl@cas@"};
    const ManifestId id = writeManifestRaw(*backend, layout, ns,
        ManifestRef{.writer_epoch = 1, .build_sequence = 5, .manifest_ordinal = 1}, {});
    addPrecommitTransition(*backend, layout, ns, hexToU128("000000000000000000000000000000aa"),
        "all_0_0_0", std::nullopt, id.ref, /*shard*/ 0);
    setWatermarkMinActive(*backend, layout, server_root_id, 1, 5);   /// alive: fold it in, seal coverage.
    Gc gc(store, hexToU128("00000000000000000000000000000102"));
    gc.runRegularRound();
    setWatermarkMinActive(*backend, layout, server_root_id, 1, 6);   /// now provably dead.
    for (int i = 0; i < 3; ++i)
        gc.runRegularRound();
    ASSERT_FALSE(backend->head(layout.manifestKey(id)).exists) << "reclaimed";

    /// Extra rounds after reclaim: no forced revisit is left to make (nothing to reclaim), no exception,
    /// no re-delete / churn.
    const uint64_t forced_before = ProfileEvents::global_counters[ProfileEvents::CasGcPrecommitRevisitForced].load();
    for (int i = 0; i < 3; ++i)
        gc.runRegularRound();
    const uint64_t forced_after = ProfileEvents::global_counters[ProfileEvents::CasGcPrecommitRevisitForced].load();
    EXPECT_EQ(forced_after, forced_before) << "no further forced revisits once the shard is clean";

    /// Belt-and-suspenders: the write-free decision probe agrees the shard settled back to Skip.
    const auto decisions = gc.discoverDecisionsForTest();
    EXPECT_EQ(decisions.at(cursorKeyForTest(ns, 0)), Gc::DiscoverDecision::Skip)
        << "self-terminated: the now-clean shard is Skipped again";
}

/// Task 5 (b): the Skip optimization must be untouched for the two cases the fix must NOT force-Read --
/// a shard whose only precommit is still LIVE (the watermark has not yet proven it dead), and a shard
/// that never held a precommit at all (a plain committed ref). Both settle to `Skip` after one fold,
/// exactly as before Task 4.
TEST(CasDanglingPrecommit, SkipPreservedForLivePrecommitAndForNoPrecommit)
{
    /// NOTE: two DISTINCT shards are exercised below, so open with `root_shards = 2` (not the shared
    /// `openTestStore` helper's `root_shards = 1`) -- the orphan-manifest-sweep's `activeManifestKeys`
    /// bounds its per-namespace shard scan by `poolMeta().root_shards`, so a shard beyond that bound
    /// would look unowned to the sweep even while a committed ref legitimately owns it there.
    auto backend = std::make_shared<InMemoryBackend>();
    auto store = Store::open(backend, PoolConfig{.pool_prefix = "p", .server_root_id = "test", .root_shards = 2});
    const Layout & layout = store->layout();

    const String server_root_id = "srv";
    const RootNamespace ns{"srv/tbl@cas@"};

    /// Shard 0: a LIVE (not-yet-dead) precommit. build_sequence 5, min_active 5 (5 is NOT below the
    /// floor => alive).
    const ManifestId live_id = writeManifestRaw(*backend, layout, ns,
        ManifestRef{.writer_epoch = 1, .build_sequence = 5, .manifest_ordinal = 1}, {});
    addPrecommitTransition(*backend, layout, ns, hexToU128("000000000000000000000000000000aa"),
        "all_0_0_0", std::nullopt, live_id.ref, /*shard*/ 0);

    /// Shard 1: no precommit at all -- a plain committed ref.
    const ManifestId committed_id = writeManifestRaw(*backend, layout, ns,
        ManifestRef{.writer_epoch = 1, .build_sequence = 1, .manifest_ordinal = 1}, {});
    publishCommittedTransition(*backend, layout, ns, "committed_0_0_0", std::nullopt, committed_id.ref, /*shard*/ 1);

    setWatermarkMinActive(*backend, layout, server_root_id, 1, 5);
    Gc gc(store, hexToU128("00000000000000000000000000000103"));
    gc.runRegularRound();   /// seals coverage for both shards.

    /// The token-stable shards must still be Skip (optimization preserved).
    const auto decisions = gc.discoverDecisionsForTest();
    EXPECT_EQ(decisions.at(cursorKeyForTest(ns, 0)), Gc::DiscoverDecision::Skip)
        << "a live (not-yet-dead) precommit must NOT force a re-fold";
    EXPECT_EQ(decisions.at(cursorKeyForTest(ns, 1)), Gc::DiscoverDecision::Skip)
        << "a shard with no precommit at all is Skip, exactly as before Task 4";

    /// Neither manifest is reclaimed: the live-precommit manifest is not yet dead, the committed
    /// manifest was never a reclaim candidate.
    EXPECT_TRUE(backend->head(layout.manifestKey(live_id)).exists);
    EXPECT_TRUE(backend->head(layout.manifestKey(committed_id)).exists);
}

/// Task 5 (c): the fix makes `reclaimAbandonedPrecommit` run more often (every forced revisit), which
/// raises the odds of a REDUNDANT removal landing on the journal for a precommit already reclaimed (e.g.
/// a race with a would-be `Build::abandon` that fires after the GC's own reclaim already folded). Append
/// a SECOND removal of the EXACT SAME precommit binding directly via the raw journal helper
/// (`appendOwnerEvent`, the same primitive `addPrecommitTransition`/`dropRefTransition` build on) and
/// fold it: `foldManifestEdges`'s "removed precommit whose body is absent emits no edges" branch
/// (`CasGc.cpp`, the `was_precommit` no-op path) makes this idempotent -- no exception, no
/// double-decrement, the manifest stays reclaimed, and the shard is not left wedged open.
TEST(CasDanglingPrecommit, DoubleRemovalOfReclaimedPrecommitIsIdempotent)
{
    std::shared_ptr<InMemoryBackend> backend;
    auto store = openTestStore(backend);
    const Layout & layout = store->layout();

    const String server_root_id = "srv";
    const RootNamespace ns{"srv/tbl@cas@"};
    const uint64_t shard = 0;
    const DB::UInt128 build_id = hexToU128("000000000000000000000000000000aa");
    const String final_ref_name = "all_0_0_0";
    const ManifestRef precommit_ref{.writer_epoch = 1, .build_sequence = 5, .manifest_ordinal = 1};

    const ManifestId id = writeManifestRaw(*backend, layout, ns, precommit_ref, {});
    addPrecommitTransition(*backend, layout, ns, build_id, final_ref_name, std::nullopt, precommit_ref, shard);

    setWatermarkMinActive(*backend, layout, server_root_id, 1, 5);   /// alive: fold it in, seal coverage.
    Gc gc(store, hexToU128("00000000000000000000000000000104"));
    gc.runRegularRound();
    ASSERT_TRUE(backend->head(layout.manifestKey(id)).exists) << "body present while precommit alive";

    setWatermarkMinActive(*backend, layout, server_root_id, 1, 6);   /// now provably dead.
    for (int i = 0; i < 3; ++i)
        gc.runRegularRound();
    ASSERT_FALSE(backend->head(layout.manifestKey(id)).exists) << "reclaimed by the fix";

    /// Append a SECOND, redundant removal of the EXACT SAME precommit binding directly (bypassing
    /// `reclaimAbandonedPrecommit` -- simulating a race where the removal lands twice).
    const OwnerBinding redundant_old{.owner_kind = OwnerKind::Precommit,
        .ref_name = final_ref_name, .build_id = build_id, .manifest_ref = precommit_ref};
    appendOwnerEvent(*backend, layout, ns, shard, redundant_old, std::nullopt);

    /// Fold it: no exception, manifest stays reclaimed (no double-decrement / cleanup churn).
    ASSERT_NO_THROW(gc.runRegularRound());
    EXPECT_FALSE(backend->head(layout.manifestKey(id)).exists) << "stays reclaimed after the redundant removal";

    /// A further round settles the shard back to Skip -- the redundant event did not leave it wedged.
    gc.runRegularRound();
    const auto decisions = gc.discoverDecisionsForTest();
    EXPECT_EQ(decisions.at(cursorKeyForTest(ns, shard)), Gc::DiscoverDecision::Skip)
        << "redundant removal does not wedge the shard open";
}
