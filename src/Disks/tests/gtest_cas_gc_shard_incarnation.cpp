#include <gtest/gtest.h>

#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasBuild.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasFsck.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasGc.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasGcFormats.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasInMemoryBackend.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasStore.h>
#include "cas_test_helpers.h"

namespace DB::ErrorCodes
{
extern const int ABORTED;
}

using namespace DB::Cas;
using namespace DB::Cas::tests;
using DB::Cas::tests::injectRetire;
using DB::Cas::tests::writeBlobRaw;
using DB::Cas::tests::idOf;
using DB::Cas::tests::u128Of;

namespace
{

StorePtr makeStoreWithShards(std::shared_ptr<InMemoryBackend> & out_backend, uint64_t gc_shards = 1)
{
    out_backend = std::make_shared<InMemoryBackend>();
    return Store::open(out_backend,
        PoolConfig{.pool_prefix = "p", .server_root_id = "test", .gc_shards = gc_shards});
}

ManifestRef testRef(uint64_t seq)
{
    return ManifestRef{.writer_epoch = 1, .build_sequence = seq, .manifest_ordinal = 1};
}

/// Whether a blob's body object is present in the backend.
bool blobPresent(InMemoryBackend & b, const Layout & layout, const String & payload)
{
    return b.head(layout.blobKey(BlobId(u128ToHex(u128Of(payload))))).exists;
}

}

/// Task 4: LIST-based discovery. Publishing a ref into ns A shard 0 makes (A,0) discoverable;
/// ns B with no shard object is NOT discovered.
TEST(CasGcShardIncarnation, DiscoveryEqualsPresentShards)
{
    for (const uint64_t gc_shards : {1u, 4u})
    {
        std::shared_ptr<InMemoryBackend> backend;
        auto store = makeStoreWithShards(backend, gc_shards);
        Gc gc(store, hexToU128("0000000000000000000000000000000a"));

        const RootNamespace ns_a{"srv1/tblA"};
        const RootNamespace ns_b{"srv1/tblB"};

        /// Write a ref shard for ns A shard 0 only.
        writeManifestRaw(*backend, store->layout(), ns_a, testRef(1), {});
        publishCommittedTransition(*backend, store->layout(), ns_a, "part_1", std::nullopt, testRef(1), /*shard=*/0);

        /// ns B: no shard object written at all.

        const auto universe = gc.discoverUniverseForTest();

        /// Must contain (A, 0).
        bool found_a = false;
        for (const auto & [ns, shard] : universe)
        {
            if (ns.string() == "srv1/tblA" && shard == 0)
                found_a = true;
            EXPECT_NE(ns.string(), "srv1/tblB") << "ns B should not appear in universe (no shard written)";
        }
        EXPECT_TRUE(found_a) << "ns A shard 0 must be in the universe (shard object present)";
    }
}

/// Task 4: listNamespaces is LIST-based; no registry involved.
/// Publishing into ns A makes it appear in listNamespaces(""); ns B absent.
TEST(CasGcShardIncarnation, ListNamespacesFromRefsNotRegistry)
{
    for (const uint64_t gc_shards : {1u, 4u})
    {
        std::shared_ptr<InMemoryBackend> backend;
        auto store = makeStoreWithShards(backend, gc_shards);

        const RootNamespace ns_a{"srv1/tblA"};

        EXPECT_TRUE(store->listNamespaces("").empty());

        /// Write a ref shard for ns A — no registry write.
        writeManifestRaw(*backend, store->layout(), ns_a, testRef(1), {});
        publishCommittedTransition(*backend, store->layout(), ns_a, "part_1", std::nullopt, testRef(1), /*shard=*/0);

        const auto nss = store->listNamespaces("");
        ASSERT_EQ(nss.size(), 1u);
        EXPECT_EQ(nss[0], "srv1/tblA");

        /// Prefix filter: no match.
        EXPECT_TRUE(store->listNamespaces("srv2/").empty());
        /// Prefix filter: match.
        const auto filtered = store->listNamespaces("srv1/");
        ASSERT_EQ(filtered.size(), 1u);
        EXPECT_EQ(filtered[0], "srv1/tblA");
    }
}

/// Task 5: THM-NO-RETURN create-race. A NEWBORN ref-shard is born fenced to the current GC round
/// (self-floor), so the `promote` gate is forced to refresh its retire view before committing any
/// blob. Without the self-floor, a writer with a stale retire view could promote a committed ref
/// to a blob that GC has already condemned in the current round — a dangling committed manifest
/// (INV-NO-DANGLE violation). With the self-floor, the refresh exposes the condemnation and
/// promote fails closed (ABORTED), preventing the dangle.
///
/// Scenario (registry-free create-race):
///   1. Open a Store (retireView refreshed at open — gc/state absent, so view is at round 0).
///   2. Write blob b1's body directly to the backend (present, not yet condemned).
///   3. Inject gc/state at round 1 with b1 condemned (its current token in the retired set).
///      b1's body is still PRESENT — this simulates GC having fenced+retired b1 but not yet
///      deleted it (the retired-but-body-present window).
///   4. The Store's retireView is NOT refreshed — it is still at round 0 (empty condemned set).
///      `currentGcRound()` reads gc/state and returns 1.
///   5. A writer for NEWBORN ns B calls `precommitAdd` → reads `currentGcRound() = 1` →
///      the NEWBORN shard is born with `fence_round = 1` (self-floor).
///   6. `promote`:
///      - With self-floor (fixed): `retireView.round() = 0 < shard.fence_round = 1` → refresh
///        → b1 condemned → ABORTED. No dangle.
///      - Without self-floor (broken): `shard.fence_round = 0`, no refresh → stale view says
///        b1 not condemned → promote succeeds → committed ref names a condemned (soon-deleted)
///        blob → INV-NO-DANGLE violated (dangling=1 in fsck).
///
/// Both gc_shards=1 and gc_shards>1 are exercised. The self-floor and promote gate are independent
/// of the blob-hash-prefix sharding axis (fence_round lives in the ROOT shard).
TEST(CasGcShardIncarnation, NewbornPrecommitProtectsDedupBlobAgainstConcurrentDrop)
{
    for (const uint64_t gc_shards : {1u, 4u})
    {
        std::shared_ptr<InMemoryBackend> backend;
        auto store = makeStoreWithShards(backend, gc_shards);
        const RootNamespace ns_b{"srv1/tblB"};

        /// --- Phase 1: Write b1's body directly (before any GC). retireView = round 0. ---
        const String b1_payload = "shared-blob-b1";
        writeBlobRaw(*backend, store->layout(), b1_payload,
            store->poolMeta().blob_header_len, store->poolMeta().pool_id);
        ASSERT_TRUE(blobPresent(*backend, store->layout(), b1_payload))
            << "b1 body must be present after direct write";
        const String b1_key = store->layout().blobKey(idOf(b1_payload));
        const Token b1_token = backend->head(b1_key).token;

        /// --- Phase 2: Inject gc/state at round 1 with b1 CONDEMNED (body still present). ---
        /// This simulates GC having advanced to round 1 and retired b1 (condemned token recorded
        /// in the retired set) but not yet deleted b1's body object. The Store's retireView is
        /// still at round 0 — it has NOT been refreshed since open.
        injectRetire(*backend, store->layout(), /*round*/ 1, /*fence_seq*/ 0, /*shard*/ 0,
            {RetiredEntry{.kind = ObjectKind::Blob, .hash = u128Of(b1_payload),
                          .token = b1_token, .size = static_cast<uint64_t>(b1_payload.size())}});

        /// Sanity: currentGcRound() reads gc/state fresh and returns 1.
        ASSERT_EQ(store->currentGcRound(), 1u)
            << "currentGcRound() must return the injected round";
        /// Sanity: retireView is still stale (not refreshed since open).
        ASSERT_EQ(store->retireView().round(), 0u)
            << "retireView must still be at round 0 (not refreshed after injection)";

        /// --- Phase 3: Writer for NEWBORN ns B — stale retireView, b1 condemned but body present ---
        BuildInfo info_b;
        info_b.intended_ref = ns_b.string() + "/part_b1";
        auto build_b = store->startBuild(info_b);

        /// Adopt b1 by tokenless evidence (simulating the dedup case: the writer observed b1
        /// present BEFORE the GC round — no HEAD here, just evidence).
        ManifestEntry dep_b1;
        dep_b1.path = "data.bin";
        dep_b1.placement = EntryPlacement::Blob;
        dep_b1.blob_hash = u128Of(b1_payload);
        dep_b1.blob_size = b1_payload.size();
        build_b->adoptEvidence(dep_b1);

        const ManifestId id_b = build_b->stageManifest({dep_b1});

        /// precommitAdd: NEWBORN shard does not exist yet. Reads currentGcRound() = 1 → stamps
        /// fence_round = 1 (self-floor). An existing shard would keep its old fence_round.
        build_b->precommitAdd(ns_b, "part_b1", id_b);

        /// --- Phase 4: promote — the safety assertion ---
        /// With self-floor (fence_round = 1): retireView.round() = 0 < 1 → refresh → b1
        /// condemned → ABORTED. No dangle.
        /// Without self-floor (fence_round = 0): no refresh → stale view says b1 ok → promote
        /// succeeds → committed ref naming a condemned (soon-deleted) blob → dangle.
        bool promote_threw_aborted = false;
        try
        {
            build_b->promote(ns_b, "part_b1", build_b->buildId(), id_b);
        }
        catch (const DB::Exception & e)
        {
            if (e.code() == DB::ErrorCodes::ABORTED)
                promote_threw_aborted = true;
            else
                throw;
        }

        EXPECT_TRUE(promote_threw_aborted)
            << "gc_shards=" << gc_shards << ": promote must throw ABORTED because the NEWBORN "
               "shard's self-floor (fence_round=1) forced a retireView refresh that exposed b1 "
               "as condemned — a silent promote would commit a dangling ref (INV-NO-DANGLE)";

        /// INV-NO-DANGLE: whether promote threw or silently succeeded, no committed ref must name
        /// a missing or condemned blob. A silent promote (promote_threw_aborted == false) produces
        /// dangling=1 here, confirming the test would catch the regression.
        const FsckReport rep = runFsck(*store, /*detail=*/false);
        EXPECT_EQ(rep.dangling, 0u)
            << "gc_shards=" << gc_shards << ": INV-NO-DANGLE violated — a committed ref names a "
               "missing blob (dangling=" << rep.dangling << ", reachable=" << rep.reachable << ")";
    }
}
