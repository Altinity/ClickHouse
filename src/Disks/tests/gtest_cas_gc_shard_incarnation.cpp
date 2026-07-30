#include <gtest/gtest.h>

#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasPartWriteTxn.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Gc/CasBlobInDegree.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Tools/CasFsck.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Gc/CasGc.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Formats/CasGcStateFormat.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Backend/CasInMemoryBackend.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasPool.h>
#include "cas_test_helpers.h"

namespace DB::ErrorCodes
{
extern const int ABORTED;
}

using namespace DB::Cas;
using namespace DB::Cas::tests;
using DB::Cas::tests::injectRetire;

namespace
{

PoolPtr makePoolWithShards(std::shared_ptr<InMemoryBackend> & out_backend, uint64_t gc_shards = 1)
{
    out_backend = std::make_shared<InMemoryBackend>();
    return Pool::open(out_backend,
        PoolConfig{.pool_prefix = "p", .server_root_id = "test", .gc_shards = gc_shards});
}

ManifestRef testRef(uint64_t seq)
{
    return ManifestRef{.writer_epoch = 1, .build_sequence = seq, .manifest_ordinal = 1};
}

}

/// Task 4: LIST-based discovery. Publishing a ref into ns A shard 0 makes (A,0) discoverable;
/// ns B with no shard object is NOT discovered.
TEST(CasGcShardIncarnation, DiscoveryEqualsPresentShards)
{
    for (const uint64_t gc_shards : {1u, 4u})
    {
        std::shared_ptr<InMemoryBackend> backend;
        auto store = makePoolWithShards(backend, gc_shards);
        Gc gc(store, hexToU128("0000000000000000000000000000000a"));

        const RootNamespace ns_a{"srv1/tblA"};
        const RootNamespace ns_b{"srv1/tblB"};

        /// Write a ref shard for ns A shard 0 only.
        writeManifestRaw(*backend, store->layout(), ns_a, testRef(1), {});
        publishCommittedTransition(*backend, store->layout(), ns_a, "part_1", std::nullopt, testRef(1), /*shard=*/0);

        /// ns B: no shard object written at all.

        const auto universe = gc.discoverUniverseForTest();

        /// Stage B (Task 4-C): the universe is catalog-authoritative now, so it is a life per namespace
        /// (there are no numeric shards to destructure -- see `NamespaceLifeId`), never a
        /// `(namespace, shard)` pair.
        bool found_a = false;
        for (const NamespaceLifeId & life : universe)
        {
            if (life.ns.string() == "srv1/tblA")
                found_a = true;
            EXPECT_NE(life.ns.string(), "srv1/tblB") << "ns B should not appear in universe (no shard written)";
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
        auto store = makePoolWithShards(backend, gc_shards);

        const RootNamespace ns_a{"srv1/tblA"};

        EXPECT_TRUE(store->listNamespaces("").namespaces.empty());

        /// Write a ref shard for ns A — no registry write.
        writeManifestRaw(*backend, store->layout(), ns_a, testRef(1), {});
        publishCommittedTransition(*backend, store->layout(), ns_a, "part_1", std::nullopt, testRef(1), /*shard=*/0);

        const auto nss = store->listNamespaces("").namespaces;
        ASSERT_EQ(nss.size(), 1u);
        EXPECT_EQ(nss[0], "srv1/tblA");

        /// Prefix filter: no match.
        EXPECT_TRUE(store->listNamespaces("srv2/").namespaces.empty());
        /// Prefix filter: match.
        const auto filtered = store->listNamespaces("srv1/").namespaces;
        ASSERT_EQ(filtered.size(), 1u);
        EXPECT_EQ(filtered[0], "srv1/tblA");
    }
}

/// Task 5: THM-NO-RETURN create-race. A NEWBORN ref-shard is born fenced to the current GC round
/// (self-floor: `fence_round` self-floors to `currentGcRound()` on the create-if-absent branch).
///
/// Scenario (registry-free create-race):
///   1. Open a Pool (gc/state absent).
///   2. Write blob b1's body directly to the backend (present, not yet condemned).
///   3. Inject gc/state at round 1 with b1 condemned (its current token in the retired set).
///      b1's body is still PRESENT — this simulates GC having fenced+retired b1 but not yet
///      deleted it (the retired-but-body-present window).
///   4. A writer for NEWBORN ns B calls `precommitAdd` → reads `currentGcRound() = 1` →
///      the NEWBORN shard is born with `fence_round = 1` (self-floor).
///   5. `promote` binds the condemned-but-present tokenless leaf AS IS (spec
///      2026-07-09-cas-writer-gc-simplification D5: there is no writer-side view refresh at promote
///      any more). This is safe because the precommit closure's edge is journal-durable BEFORE
///      promote returns (EDGE-BEFORE-OBSERVE): the NEXT GC fold sees net in-degree >= 1 for b1 and
///      SPARES the entry, regardless of when it would otherwise graduate — the condemnation is
///      doomed, never the blob. INV-NO-DANGLE holds (dangling=0 in fsck).
///
/// Both gc_shards=1 and gc_shards>1 are exercised. The self-floor and promote gate are independent
/// of the blob-hash-prefix sharding axis (fence_round lives in the ROOT shard).
TEST(CasGcShardIncarnation, NewbornPrecommitProtectsDedupBlobAgainstConcurrentDrop)
{
    for (const uint64_t gc_shards : {1u, 4u})
    {
        std::shared_ptr<InMemoryBackend> backend;
        auto store = makePoolWithShards(backend, gc_shards);
        const RootNamespace ns_b{"srv1/tblB"};

        /// --- Phase 1: Write b1's body directly (before any GC). ---
        /// Mint b1 under the POOL streaming-hash id (via a throwaway build's putBlob) so the in-closure
        /// copy-forward verifier accepts its payload — the plain CityHash test id would be refused.
        const String b1_payload = "shared-blob-b1";
        const String b1_hex = streamingHexOf(b1_payload);
        const BlobRef b1_ref{BlobHashAlgo::CityHash128, BlobDigest::fromU128(hexToU128(b1_hex))};
        {
            auto seed = store->beginPartWrite({});
            seed->putBlob(b1_ref, BlobSource::fromString(b1_payload));
        }
        const String b1_key = store->layout().blobKey(b1_ref);
        ASSERT_TRUE(backend->head(b1_key).exists)
            << "b1 body must be present after the seed putBlob";
        const Token b1_token = backend->head(b1_key).token;

        /// --- Phase 2: Inject gc/state at round 1 with b1 CONDEMNED (body still present). ---
        /// This simulates GC having advanced to round 1 and retired b1 (condemned token recorded
        /// in the retired set) but not yet deleted b1's body object.
        injectRetire(*backend, store->layout(), /*round*/ 1, /*shard*/ 0,
            {RetiredEntry{.kind = ObjectKind::Blob, .ref = b1_ref,
                          .token = b1_token, .size = static_cast<uint64_t>(b1_payload.size())}});

        /// Sanity: currentGcRound() reads gc/state fresh and returns 1.
        ASSERT_EQ(store->currentGcRound(), 1u)
            << "currentGcRound() must return the injected round";

        /// --- Phase 3: Writer for NEWBORN ns B — b1 condemned but body present ---
        PartWriteInfo info_b;
        info_b.intended_ref = ns_b.string() + "/part_b1";
        auto build_b = store->beginPartWrite(info_b);

        /// Adopt b1 by tokenless evidence (simulating the dedup case: the writer observed b1
        /// present BEFORE the GC round — no HEAD here, just evidence).
        ManifestEntry dep_b1;
        dep_b1.path = "data.bin";
        dep_b1.placement = EntryPlacement::Blob;
        dep_b1.ref = DB::Cas::BlobRef{DB::Cas::BlobHashAlgo::CityHash128, DB::Cas::BlobDigest::fromU128(hexToU128(b1_hex))};

        dep_b1.blob_size = b1_payload.size();
        build_b->adoptEvidence(dep_b1);

        const ManifestId id_b = build_b->stageManifest({dep_b1});

        /// precommitAdd: NEWBORN shard does not exist yet. Reads currentGcRound() = 1 → stamps
        /// fence_round = 1 (self-floor). An existing shard would keep its old fence_round.
        build_b->precommitAdd(ns_b, "part_b1", id_b);

        /// --- Phase 4: promote — the safety assertion (Phase-A contract) ---
        /// Spec 2026-07-09-cas-writer-gc-simplification D5: there is NO writer-side view refresh at
        /// promote any more — the K3 gate binds the condemned-but-present token AS IS. This is SAFE
        /// because the precommit closure's edge has been journal-durable since precommitAdd (BEFORE
        /// promote returns), so the NEXT GC fold sees net in-degree >= 1 for b1 and SPARES the entry
        /// (EDGE-BEFORE-OBSERVE) regardless of round-paced graduation timing — the condemnation is
        /// doomed, never the blob. (No GC round runs in this test at all; the argument is what makes
        /// deferring the round safe, not something this test drives to completion.)
        /// The former behavior (self-floor-forced refresh → in-closure copy-forward → fresh incarnation)
        /// was TLA+-Gate-A-verified redundant; the shard's fence_round stamp itself (THM-NO-RETURN birth
        /// floor) remains and is asserted by the sibling shard-incarnation tests.
        EXPECT_NO_THROW(build_b->promote(ns_b, "part_b1", build_b->buildId(), id_b))
            << "gc_shards=" << gc_shards << ": promote must commit — the durable edge protects the "
               "condemned-but-present tokenless leaf without any refresh or copy-forward";
        EXPECT_TRUE(store->resolveRef(ns_b, "part_b1").has_value())
            << "gc_shards=" << gc_shards << ": the ref must commit";
        /// The condemned token is bound UNCHANGED — no displacement happens (and none is needed).
        EXPECT_EQ(backend->head(b1_key).token, b1_token)
            << "gc_shards=" << gc_shards << ": no copy-forward under the Phase-A contract — the token "
               "stays; the folded edge will spare it at the next fold (no round runs here to delete it)";

        /// INV-NO-DANGLE: the body is present and no GC round ever runs in this test to fold the
        /// precommit/committed edge; a real deployment's next fold would see net in-degree >= 1 and
        /// spare the entry. A regression that let the delete pipeline race a live durable edge would
        /// produce dangling=1 here.
        const FsckReport rep = runFsck(*store, /*detail=*/false);
        EXPECT_EQ(rep.dangling, 0u)
            << "gc_shards=" << gc_shards << ": INV-NO-DANGLE violated — a committed ref names a "
               "missing blob (dangling=" << rep.dangling << ", reachable=" << rep.reachable << ")";
    }
}

/// The five shard-OBJECT-reclaim tests that used to follow (`DroppedShardObjectIsReclaimed`,
/// `IdleButLiveShardNotReclaimed`, `RecreateAfterReclaimFoldsFromZero`, `ActivatedPrecommitBlocksShardReclaim`,
/// `ReviveRacesReclaimAborts`) were removed with the snapshot+log ref model. They asserted GC reclaims /
/// token-guards a MUTABLE per-namespace ref-shard object at `rootShardKey(ns, shard)`. There is no such
/// mutable object anymore: a namespace's ref state is its immutable `_log`/`_snap` objects, physical
/// reclamation is the namespace-cleanup item (`remove_namespace` -> Pending -> Completed), and ABA safety
/// is structural -- a recreated namespace uses a strictly-greater `RefTxnId`. The still-meaningful ABA
/// case (`remove_namespace` then a later `namespace_birth` with a greater id folds normally, no
/// stale-cursor double-count) is covered by `gtest_cas_ref_gc.cpp`; the namespace-cleanup reclaim path is
/// covered there and in `gtest_cas_gc_round.cpp`.
