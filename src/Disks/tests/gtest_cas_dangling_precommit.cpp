#include <gtest/gtest.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasGc.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasInMemoryBackend.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasStore.h>
#include <Disks/tests/cas_test_helpers.h>

using namespace DB::Cas;
using DB::Cas::tests::writeManifestRaw;
using DB::Cas::tests::addPrecommitTransition;
using DB::Cas::tests::setWatermarkMinActive;

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

    /// BUG (pre-fix): the manifest body is orphaned -- still present, never reclaimed.
    /// After Task 4 this assertion is INVERTED (see Task 4 Step 4).
    EXPECT_TRUE(backend->head(layout.manifestKey(id)).exists)
        << "PRE-FIX: dangling precommit manifest is orphaned (Skip parks the shard, reclaim never runs)";
}
