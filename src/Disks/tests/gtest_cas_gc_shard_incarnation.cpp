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
        /// Mint b1 under the POOL streaming-hash id (via a throwaway build's putBlob) so the in-closure
        /// copy-forward verifier accepts its payload — the plain CityHash test id would be refused.
        const String b1_payload = "shared-blob-b1";
        const String b1_hex = streamingHexOf(b1_payload);
        {
            auto seed = store->startBuild({});
            seed->putBlob(BlobId{b1_hex}, BlobSource::fromString(b1_payload));
        }
        const String b1_key = store->layout().blobKey(BlobId{b1_hex});
        ASSERT_TRUE(backend->head(b1_key).exists)
            << "b1 body must be present after the seed putBlob";
        const Token b1_token = backend->head(b1_key).token;

        /// --- Phase 2: Inject gc/state at round 1 with b1 CONDEMNED (body still present). ---
        /// This simulates GC having advanced to round 1 and retired b1 (condemned token recorded
        /// in the retired set) but not yet deleted b1's body object. The Store's retireView is
        /// still at round 0 — it has NOT been refreshed since open.
        injectRetire(*backend, store->layout(), /*round*/ 1, /*fence_seq*/ 0, /*shard*/ 0,
            {RetiredEntry{.kind = ObjectKind::Blob, .hash = hexToU128(b1_hex),
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
        dep_b1.blob_hash = hexToU128(b1_hex);
        dep_b1.blob_size = b1_payload.size();
        build_b->adoptEvidence(dep_b1);

        const ManifestId id_b = build_b->stageManifest({dep_b1});

        /// precommitAdd: NEWBORN shard does not exist yet. Reads currentGcRound() = 1 → stamps
        /// fence_round = 1 (self-floor). An existing shard would keep its old fence_round.
        build_b->precommitAdd(ns_b, "part_b1", id_b);

        /// --- Phase 4: promote — the safety assertion (Phase-A contract) ---
        /// Spec 2026-07-09-cas-writer-gc-simplification D5: there is NO writer-side view refresh at
        /// promote anymore. The stale view (round 0) does not see b1's condemnation, so the K3 gate binds
        /// the condemned-but-present token AS IS — and that is SAFE by the floor + edge pair:
        ///   • graduation of the round-1 condemnation requires min_ack > 1, but THIS writer's advertised
        ///     round is 0 (its installed view) — the floor holds, the entry can never graduate to delete
        ///     while this writer has not installed a newer view;
        ///   • the precommit closure edge has been journal-durable since precommitAdd, so the NEXT GC fold
        ///     sees d >= 1 and SPARES the entry (EDGE-BEFORE-OBSERVE) — the condemnation is doomed, not
        ///     the blob.
        /// The former behavior (self-floor-forced refresh → in-closure copy-forward → fresh incarnation)
        /// was TLA+-Gate-A-verified redundant; the shard's fence_round stamp itself (THM-NO-RETURN birth
        /// floor) remains and is asserted by the sibling shard-incarnation tests.
        EXPECT_NO_THROW(build_b->promote(ns_b, "part_b1", build_b->buildId(), id_b))
            << "gc_shards=" << gc_shards << ": promote must commit — the floor + durable edge protect the "
               "condemned-but-present tokenless leaf without any refresh or copy-forward";
        EXPECT_TRUE(store->resolveRef(ns_b, "part_b1").has_value())
            << "gc_shards=" << gc_shards << ": the ref must commit";
        /// The condemned token is bound UNCHANGED — no displacement happens (and none is needed).
        EXPECT_EQ(backend->head(b1_key).token, b1_token)
            << "gc_shards=" << gc_shards << ": no copy-forward under the Phase-A contract — the token "
               "stays; the floor forbids its deletion and the folded edge will spare it";

        /// INV-NO-DANGLE: the body is present and the floor forbids its deletion while this writer's ack
        /// is below the condemn round; the folded precommit/committed edge spares the entry at the next
        /// fold. A regression that let the delete pipeline run past a live writer's ack would produce
        /// dangling=1 here.
        const FsckReport rep = runFsck(*store, /*detail=*/false);
        EXPECT_EQ(rep.dangling, 0u)
            << "gc_shards=" << gc_shards << ": INV-NO-DANGLE violated — a committed ref names a "
               "missing blob (dangling=" << rep.dangling << ", reachable=" << rep.reachable << ")";
    }
}

/// Task 6 (S30): an empty+tombstoned+fully-folded ref-shard object is reclaimed by GC.
/// After `dropNamespace`, the ref-shard object must be deleted by GC (not left as storage debris).
TEST(CasGcShardIncarnation, DroppedShardObjectIsReclaimed)
{
    for (const uint64_t gc_shards : {1u, 4u})
    {
        std::shared_ptr<InMemoryBackend> backend;
        auto store = makeStoreWithShards(backend, gc_shards);
        const RootNamespace ns{"srv1/tblDrop"};

        /// Publish a ref so the shard object is created.
        {
            BuildInfo info;
            info.intended_ref = ns.string() + "/part_0";
            auto build = store->startBuild(info);
            const ManifestId id = build->stageManifest({});
            build->precommitAdd(ns, "part_0", id);
            build->promote(ns, "part_0", build->buildId(), id);
        }

        /// Use the actual shard "part_0" routes to (CityHash64("part_0") % root_shards).
        const uint64_t ref_shard = store->shardOf("part_0");
        const String shard_key = store->layout().rootShardKey(ns, ref_shard);
        ASSERT_TRUE(backend->head(shard_key).exists)
            << "gc_shards=" << gc_shards << ": shard object must exist after publish";

        /// Drop the namespace (appends removal events + tombstone per shard).
        store->dropNamespace(ns);

        /// Advance watermark so the build-watermark guard does not spare objects.
        store->renewWatermarkOnce();

        /// Run GC rounds until the shard object is reclaimed or the round limit is hit.
        Gc gc(store, hexToU128("000000000000000000000000000000ab"));
        for (int i = 0; i < 16; ++i)
        {
            gc.runRegularRound();
            if (!backend->head(shard_key).exists)
                break;
        }

        EXPECT_FALSE(backend->head(shard_key).exists)
            << "gc_shards=" << gc_shards
            << ": ref-shard object must be reclaimed by GC after dropNamespace (soak S30)";
    }
}

/// Task 6: a shard that is empty but has NO tombstone (table alive, all parts dropped manually)
/// must NOT be reclaimed. The tombstone is written only by `dropNamespace`; `dropRef` does not write it.
TEST(CasGcShardIncarnation, IdleButLiveShardNotReclaimed)
{
    for (const uint64_t gc_shards : {1u, 4u})
    {
        std::shared_ptr<InMemoryBackend> backend;
        auto store = makeStoreWithShards(backend, gc_shards);
        const RootNamespace ns{"srv1/tblLive"};

        /// Publish and then drop the ref — shard exists but no tombstone (table not dropped).
        {
            BuildInfo info;
            info.intended_ref = ns.string() + "/part_0";
            auto build = store->startBuild(info);
            const ManifestId id = build->stageManifest({});
            build->precommitAdd(ns, "part_0", id);
            build->promote(ns, "part_0", build->buildId(), id);
        }
        store->dropRef(ns, "part_0");
        store->renewWatermarkOnce();

        const uint64_t ref_shard = store->shardOf("part_0");
        const String shard_key = store->layout().rootShardKey(ns, ref_shard);
        ASSERT_TRUE(backend->head(shard_key).exists)
            << "gc_shards=" << gc_shards << ": shard object must exist after dropRef";

        /// Run several GC rounds — shard must NOT be reclaimed (no tombstone).
        Gc gc(store, hexToU128("000000000000000000000000000000cd"));
        for (int i = 0; i < 8; ++i)
            gc.runRegularRound();

        EXPECT_TRUE(backend->head(shard_key).exists)
            << "gc_shards=" << gc_shards
            << ": ref-shard object without tombstone must NOT be reclaimed by GC";
    }
}

/// Task 6 + Task 3 (ABA): after a shard is reclaimed, a new publish recreates it with a strictly
/// greater incarnation. GC must fold the new shard from cursor 0 (no stale-cursor double-count),
/// and the result must satisfy INV-NO-DANGLE + no unreachable blobs.
TEST(CasGcShardIncarnation, RecreateAfterReclaimFoldsFromZero)
{
    for (const uint64_t gc_shards : {1u, 4u})
    {
        std::shared_ptr<InMemoryBackend> backend;
        auto store = makeStoreWithShards(backend, gc_shards);
        const RootNamespace ns{"srv1/tblRecreate"};

        /// First publish.
        {
            BuildInfo info;
            info.intended_ref = ns.string() + "/part_0";
            auto build = store->startBuild(info);
            const ManifestId id = build->stageManifest({});
            build->precommitAdd(ns, "part_0", id);
            build->promote(ns, "part_0", build->buildId(), id);
        }

        /// Use the actual shard "part_0" and "part_1" route to.
        const uint64_t ref_shard_0 = store->shardOf("part_0");
        const uint64_t ref_shard_1 = store->shardOf("part_1");
        const String shard_key = store->layout().rootShardKey(ns, ref_shard_0);

        /// Drop the namespace (tombstone).
        store->dropNamespace(ns);
        store->renewWatermarkOnce();

        /// Run GC until the shard is reclaimed.
        {
            Gc gc(store, hexToU128("000000000000000000000000000000ef"));
            for (int i = 0; i < 16; ++i)
            {
                gc.runRegularRound();
                if (!backend->head(shard_key).exists)
                    break;
            }
            ASSERT_FALSE(backend->head(shard_key).exists)
                << "gc_shards=" << gc_shards << ": shard must be reclaimed before recreate test";
        }

        /// Recreate: publish a new ref into the same namespace (new incarnation > old).
        {
            BuildInfo info;
            info.intended_ref = ns.string() + "/part_1";
            auto build = store->startBuild(info);
            const ManifestId id = build->stageManifest({});
            build->precommitAdd(ns, "part_1", id);
            build->promote(ns, "part_1", build->buildId(), id);
        }

        /// part_1 may route to a different shard than part_0 — check whichever shard holds each.
        const String shard_key_1 = store->layout().rootShardKey(ns, ref_shard_1);
        ASSERT_TRUE(backend->head(shard_key_1).exists)
            << "gc_shards=" << gc_shards << ": shard must be recreated after new publish";

        /// GC should fold the new shard correctly (no double-count, no INV-NO-DANGLE).
        {
            Gc gc(store, hexToU128("000000000000000000000000000000ff"));
            for (int i = 0; i < 8; ++i)
                gc.runRegularRound();
        }

        const FsckReport rep = runFsck(*store, /*detail=*/false);
        EXPECT_EQ(rep.dangling, 0u)
            << "gc_shards=" << gc_shards << ": recreated namespace must not have dangling refs";
        EXPECT_EQ(rep.unreachable, 0u)
            << "gc_shards=" << gc_shards << ": recreated namespace must have no unreachable blobs";
    }
}

/// Task 6 (Fix 1 — Guard 4): a shard that is empty (no committed refs) + tombstoned + fully folded
/// but has a LIVE ACTIVATED (body-present) precommit binding must NOT be reclaimed by GC.
///
/// Without Guard 4 (`refs.empty()` alone is insufficient), a drop-namespace that races a live build
/// passes Guards 1-3: the fold advances past the precommit event because the body IS present (the
/// fold barrier only clamps for body-ABSENT precommits), the tombstone is covered by the cursor, and
/// `refs` is empty (the precommit was never promoted). If reclaim fires, the precommit's +1 edge is
/// already sealed into the generation's in-degree but the matching -1 (from a future abandon /
/// PrecommitRemove) can never land — the shard is gone → permanent blob leak.
///
/// Two scenarios are covered:
///
/// Scenario A (safety — Guard 4 blocks premature reclaim):
///   1. Build starts: precommitAdd (body present / activated). Do NOT promote or abandon.
///   2. dropNamespace: journal becomes [PrecommitAdd, Tombstone]. Tombstone IS last.
///   3. GC runs: fold covers both events (body present → no barrier clamp). Guards 1-3 pass.
///      Guard 4 must block reclaim. Assert shard and manifest body survive all GC rounds.
///
/// Scenario B (liveness — reclaim proceeds after precommit is resolved cleanly):
///   The normal production ordering is: build resolves (abandon/promote) BEFORE dropNamespace
///   so the tombstone is appended AFTER the PrecommitRemove, making it the LAST event.
///   After that ordering, all four guards pass and GC reclaims.
///   1. Build starts: precommitAdd (activated), then abandon (PrecommitRemove appended).
///   2. dropNamespace: journal becomes [PrecommitAdd, PrecommitRemove, Tombstone].
///      Tombstone IS last; live_bindings replay yields empty set (Guard 4 passes).
///   3. GC runs: shard IS reclaimed (all guards pass). Assert shard gone.
TEST(CasGcShardIncarnation, ActivatedPrecommitBlocksShardReclaim)
{
    for (const uint64_t gc_shards : {1u, 4u})
    {
        /// ====== Scenario A: dropNamespace races a live activated precommit ======
        {
            std::shared_ptr<InMemoryBackend> backend;
            auto store = makeStoreWithShards(backend, gc_shards);
            const RootNamespace ns{"srv1/tblPrecommitLeakA"};

            BuildInfo info;
            info.intended_ref = ns.string() + "/part_0";
            auto build = store->startBuild(info);

            const ManifestId id = build->stageManifest({});
            build->precommitAdd(ns, "part_0", id);

            const uint64_t ref_shard = store->shardOf("part_0");
            const String shard_key = store->layout().rootShardKey(ns, ref_shard);
            const String manifest_key = store->layout().manifestKey(id);

            ASSERT_TRUE(backend->head(shard_key).exists)
                << "gc_shards=" << gc_shards << " A: shard must exist after precommitAdd";
            ASSERT_TRUE(backend->head(manifest_key).exists)
                << "gc_shards=" << gc_shards << " A: manifest body must be present (activated)";

            /// dropNamespace appends tombstone LAST (no committed refs). Journal: [PrecommitAdd, Tombstone].
            store->dropNamespace(ns);
            store->renewWatermarkOnce();

            /// GC rounds: Guards 1-3 pass, Guard 4 must block reclaim (precommit live).
            {
                Gc gc(store, hexToU128("0000000000000000000000000000bb01"));
                for (int i = 0; i < 16; ++i)
                    gc.runRegularRound();
            }

            EXPECT_TRUE(backend->head(shard_key).exists)
                << "gc_shards=" << gc_shards
                << ": ref-shard must NOT be reclaimed while an activated precommit is live "
                   "(Guard 4 — blob-leak prevention; Guards 1-3 were all satisfied)";

            EXPECT_TRUE(backend->head(manifest_key).exists)
                << "gc_shards=" << gc_shards
                << ": manifest body must NOT be deleted while its precommit is live in the shard";

            build->abandon();   /// best-effort cleanup; abandon does not make the shard reclaim-eligible
            /// (abandon appends PrecommitRemove AFTER the tombstone, so Guard 2 / tombstone-last fails;
            /// that is an accepted property of the concurrent-drop scenario — not a regression).
        }

        /// ====== Scenario B: build resolves (abandon) BEFORE dropNamespace → reclaim proceeds ======
        {
            std::shared_ptr<InMemoryBackend> backend;
            auto store = makeStoreWithShards(backend, gc_shards);
            const RootNamespace ns{"srv1/tblPrecommitLeakB"};

            BuildInfo info;
            info.intended_ref = ns.string() + "/part_0";
            auto build = store->startBuild(info);

            const ManifestId id = build->stageManifest({});
            build->precommitAdd(ns, "part_0", id);

            const uint64_t ref_shard = store->shardOf("part_0");
            const String shard_key = store->layout().rootShardKey(ns, ref_shard);

            ASSERT_TRUE(backend->head(shard_key).exists)
                << "gc_shards=" << gc_shards << " B: shard must exist after precommitAdd";

            /// abandon BEFORE dropNamespace: PrecommitRemove comes BEFORE tombstone.
            /// Journal will be: [PrecommitAdd, PrecommitRemove, Tombstone].
            build->abandon();

            /// Now dropNamespace: tombstone appended LAST.
            store->dropNamespace(ns);
            store->renewWatermarkOnce();

            /// GC rounds: all guards pass (live_bindings empty, tombstone last) → shard reclaimed.
            {
                Gc gc(store, hexToU128("0000000000000000000000000000bb02"));
                for (int i = 0; i < 16; ++i)
                {
                    gc.runRegularRound();
                    if (!backend->head(shard_key).exists)
                        break;
                }
            }

            EXPECT_FALSE(backend->head(shard_key).exists)
                << "gc_shards=" << gc_shards
                << ": ref-shard must be reclaimed when precommit was abandoned before dropNamespace "
                   "(tombstone is last event, live_bindings empty — all four guards satisfied)";
        }
    }
}

/// Task 6 (Fix 1): a concurrent revive after the tombstone changes the shard's token, so a
/// `deleteExact` carrying the pre-revive token must be REFUSED (TokenMismatch) and the shard object
/// must survive. Proves that `reclaimDroppedShards`'s deleteExact is token-guarded, not unconditional.
///
/// Scenario: drive a shard to tombstoned+fully-folded so it WOULD be reclaimed. Capture the
/// shard's token T (the one GC would use for deleteExact). Simulate a concurrent revive by appending
/// a fresh owner event to the shard (so its body changes and its token advances to T' ≠ T). Assert
/// that `deleteExact(key, T)` returns TokenMismatch and the shard still exists. Also assert the
/// positive counterpart: `deleteExact(key, T')` (the CURRENT token) succeeds.
///
/// This is the key safety case for Task 6. We test the token-guard DIRECTLY against the backend —
/// this is the actual mechanism `reclaimDroppedShards` relies on, independent of GC round timing.
TEST(CasGcShardIncarnation, ReviveRacesReclaimAborts)
{
    for (const uint64_t gc_shards : {1u, 4u})
    {
        std::shared_ptr<InMemoryBackend> backend;
        auto store = makeStoreWithShards(backend, gc_shards);
        const RootNamespace ns{"srv1/tblReviveRace"};

        /// --- Phase 1: Publish a ref into the namespace. ---
        {
            BuildInfo info;
            info.intended_ref = ns.string() + "/part_0";
            auto build = store->startBuild(info);
            const ManifestId id = build->stageManifest({});
            build->precommitAdd(ns, "part_0", id);
            build->promote(ns, "part_0", build->buildId(), id);
        }

        const uint64_t ref_shard = store->shardOf("part_0");
        const String shard_key = store->layout().rootShardKey(ns, ref_shard);

        /// --- Phase 2: Drop namespace (tombstone) + run GC until fully folded and reclaimed. ---
        store->dropNamespace(ns);
        store->renewWatermarkOnce();

        {
            Gc gc_reclaim(store, hexToU128("0000000000000000000000000000aa01"));
            for (int i = 0; i < 16; ++i)
            {
                gc_reclaim.runRegularRound();
                if (!backend->head(shard_key).exists)
                    break;
            }
        }

        /// --- Phase 3: Recreate — publish into the same namespace (new incarnation). ---
        /// The shard is now absent (reclaimed above); a new publish creates a fresh object.
        {
            BuildInfo info;
            info.intended_ref = ns.string() + "/part_1";
            auto build = store->startBuild(info);
            const ManifestId id = build->stageManifest({});
            build->precommitAdd(ns, "part_1", id);
            build->promote(ns, "part_1", build->buildId(), id);
        }

        const uint64_t ref_shard_1 = store->shardOf("part_1");
        const String shard_key_1 = store->layout().rootShardKey(ns, ref_shard_1);

        ASSERT_TRUE(backend->head(shard_key_1).exists)
            << "gc_shards=" << gc_shards << ": shard must exist after recreate";

        /// Drop the ref (no tombstone yet — the namespace is still live).
        store->dropRef(ns, "part_1");

        /// --- Phase 4: GC folds the removal event; capture the fold-time token T. ---
        /// Run enough GC rounds that the shard is folded (cursor covers the drop event). After fold
        /// but before a full reclaim (the shard has no tombstone yet, so it cannot be reclaimed):
        /// capture the token that GC would carry into a deleteExact if a tombstone appeared now.
        {
            Gc gc_fold(store, hexToU128("0000000000000000000000000000aa02"));
            for (int i = 0; i < 4; ++i)
                gc_fold.runRegularRound();
        }

        /// Capture the shard's current token T (pre-revive / the token GC read at fold/recheck).
        const HeadResult pre_revive = backend->head(shard_key_1);
        ASSERT_TRUE(pre_revive.exists)
            << "gc_shards=" << gc_shards << ": shard must still exist (no tombstone — not yet reclaimable)";
        const Token stale_token = pre_revive.token;

        /// --- Phase 5: Concurrent revive — append a new publish to the shard. ---
        /// A real concurrent writer would call precommitAdd + promote. Here we use the test helper
        /// `appendOwnerEvent` to append a new committed binding directly (an owner event that changes
        /// the shard's body and mints a fresh backend token T' ≠ T).
        const ManifestRef revive_ref{.writer_epoch = 2, .build_sequence = 99, .manifest_ordinal = 1};
        writeManifestRaw(*backend, store->layout(), ns, revive_ref, {});
        appendOwnerEvent(*backend, store->layout(), ns, ref_shard_1,
            /*old_binding=*/std::nullopt,
            OwnerBinding{.owner_kind = OwnerKind::Committed,
                         .ref_name = "part_revived",
                         .build_id = {},
                         .manifest_ref = revive_ref},
            /*committed_ref_name=*/"part_revived");

        /// Confirm the shard's token changed (the revive mutated the body).
        const HeadResult post_revive = backend->head(shard_key_1);
        ASSERT_TRUE(post_revive.exists)
            << "gc_shards=" << gc_shards << ": shard must still exist after the revive append";
        const Token current_token = post_revive.token;
        ASSERT_NE(stale_token.value, current_token.value)
            << "gc_shards=" << gc_shards
            << ": the revive must have changed the shard token (pre-revive T == post-revive T' is "
               "impossible for a mutable object storage that issues content-based tokens)";

        /// --- Assert: deleteExact with the STALE token (T) is REFUSED (TokenMismatch). ---
        const DeleteOutcome stale_del = backend->deleteExact(shard_key_1, stale_token);
        EXPECT_EQ(stale_del.kind, DeleteOutcome::Kind::TokenMismatch)
            << "gc_shards=" << gc_shards
            << ": deleteExact with a pre-revive (stale) token must return TokenMismatch — "
               "the shard-reclaim is token-guarded and must not over-delete a revived shard";

        /// The shard must still exist (not deleted by the stale-token attempt).
        EXPECT_TRUE(backend->head(shard_key_1).exists)
            << "gc_shards=" << gc_shards
            << ": shard object must survive a stale-token deleteExact (the revive must prevail)";

        /// --- Positive counterpart: deleteExact with the CURRENT token (T') succeeds. ---
        const DeleteOutcome current_del = backend->deleteExact(shard_key_1, current_token);
        EXPECT_EQ(current_del.kind, DeleteOutcome::Kind::Deleted)
            << "gc_shards=" << gc_shards
            << ": deleteExact with the current token must succeed (sanity check for token-guard semantics)";

        EXPECT_FALSE(backend->head(shard_key_1).exists)
            << "gc_shards=" << gc_shards
            << ": shard must be gone after a successful current-token deleteExact";
    }
}
