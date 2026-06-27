#include <gtest/gtest.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasBuild.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasFsck.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasGc.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasInMemoryBackend.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasStore.h>
#include <Disks/tests/cas_test_helpers.h>

#include <iostream>
#include <string>
#include <vector>

namespace DB::ErrorCodes
{
extern const int FILE_DOESNT_EXIST;
extern const int ABORTED;
}

/// NO-LEAK property suite (C++ verification of the R0 INV-NO-LEAK invariant for the root-local
/// part-manifest model). Every dropped/abandoned closure must be FULLY reclaimed: after GC reaches a
/// fixpoint, NO blob or manifest object may remain for the reclaimed part, and the in-degree generation
/// must hold no stranded positive counter for a now-unreferenced blob.
///
/// The model has changed since the tree/snap era: a part is one immutable single-owner `ManifestId`
/// (only blobs stay content-addressed; manifests are NEVER shared across instances — backlog item B7).
/// The leak scenarios below therefore drive the REAL write flow (`stageManifest -> precommitAdd ->
/// putBlob -> promote`) and the real drop/abandon paths, then assert the reclaimed closure leaves no
/// debris. The old "adopt-by-tree relink" leak cases are OBSOLETE under B7 (no shared content id, no
/// subtree placement, `getPartTreeId` returns nullopt and `adoptPart` throws `NOT_IMPLEMENTED`); they
/// are `GTEST_SKIP`ped with a one-line reason rather than weakened.

using namespace DB::Cas;
using DB::Cas::tests::idOf;
using DB::Cas::tests::u128Of;
using DB::Cas::tests::inDegreeOf;

namespace
{

StorePtr openTestStore(std::shared_ptr<InMemoryBackend> & out_backend)
{
    out_backend = std::make_shared<InMemoryBackend>();
    /// One root shard so the journal of a ref lives in a single, predictable shard (cursor keys "ns/0").
    return Store::open(out_backend, PoolConfig{.pool_prefix = "p", .root_shards = 1});
}

/// Drive regular GC to a fixpoint. A round that only retired/fenced without deleting has not reached a
/// fixpoint: the delete-cascade for a just-retired blob lands in a SUBSEQUENT round, so the loop stays
/// alive while ANY of the work counters is nonzero (candidates/deleted/absent/replaced/spared).
size_t runGcToFixpoint(Gc & gc, size_t max_rounds = 64)
{
    size_t rounds = 0;
    for (; rounds < max_rounds; ++rounds)
    {
        const RoundReport rep = gc.runRegularRound();
        if (!rep.acquired_lease)
            continue;
        if (rep.candidates == 0 && rep.deleted == 0 && rep.absent == 0
            && rep.replaced == 0 && rep.spared == 0)
            break;
    }
    return rounds;
}

/// A `ManifestEntry` for a Blob leaf at `path` referencing `payload`'s content hash.
ManifestEntry blobEntry(const String & path, const String & payload)
{
    ManifestEntry e;
    e.path = path;
    e.placement = EntryPlacement::Blob;
    e.blob_hash = u128Of(payload);
    e.blob_size = payload.size();
    return e;
}

/// Publish ONE ref naming a two-blob part through the REAL writer transaction sequence — the exact order
/// the wiring drives: `startBuild -> putBlob(each body) -> stageManifest(entries) -> precommitAdd ->
/// promote`. Every byte of the closure is born under this build before the owner move commits. Returns
/// the published `ManifestId` so a caller can later HEAD its body / assert reclaim.
ManifestId publishTwoBlobPart(
    const StorePtr & s, const RootNamespace & ns, const String & ref,
    const String & payload_a, const String & payload_b)
{
    BuildInfo info;
    info.intended_ref = ns.string() + "/" + ref;
    auto build = s->startBuild(info);

    build->putBlob(idOf(payload_a), BlobSource::fromString(payload_a));
    build->putBlob(idOf(payload_b), BlobSource::fromString(payload_b));

    const ManifestId id = build->stageManifest({blobEntry("data.bin", payload_a),
                                                blobEntry("data.cmrk3", payload_b)});
    build->precommitAdd(ns, ref, id);
    build->promote(ns, ref, build->buildId(), id);
    return id;
}

/// Publish ONE ref naming a single-blob part through the real writer sequence. Returns its ManifestId.
ManifestId publishOneBlobPart(
    const StorePtr & s, const RootNamespace & ns, const String & ref, const String & payload)
{
    BuildInfo info;
    info.intended_ref = ns.string() + "/" + ref;
    auto build = s->startBuild(info);
    build->putBlob(idOf(payload), BlobSource::fromString(payload));
    const ManifestId id = build->stageManifest({blobEntry("data.bin", payload)});
    build->precommitAdd(ns, ref, id);
    build->promote(ns, ref, build->buildId(), id);
    return id;
}

/// Whether a blob's body object is present in the backend (HEADs blobKey directly — the GC retire path
/// HEADs the object key, never the Store's manifest decode cache).
bool blobPresent(const std::shared_ptr<InMemoryBackend> & b, const Layout & layout, const String & payload)
{
    return b->head(layout.blobKey(BlobId(u128ToHex(u128Of(payload))))).exists;
}

/// Whether a manifest body object is present in the backend.
bool manifestPresent(const std::shared_ptr<InMemoryBackend> & b, const Layout & layout, const ManifestId & id)
{
    return b->head(layout.manifestKey(id)).exists;
}

/// Reproduce displacement + a competing delete on the SAME (s, ns, ref) and run GC to fixpoint. partB's
/// distinct blobs displace partA's via a second promote for the same ref name (last-owner-wins). partA's
/// manifest body is then deleted by exact token exactly as a competing GC round's landed delete would,
/// so GC must reclaim partA's blobs from the journal-recorded edges WITHOUT reading the vanished body.
/// Returns the fsck report so the caller can assert the no-leak end state.
FsckReport displaceDeleteAndGc(
    const StorePtr & s, const std::shared_ptr<InMemoryBackend> & b,
    const RootNamespace & ns, const String & ref, const ManifestId & part_a)
{
    /// Re-publish the SAME ref to partB (distinct blobs): last-owner-wins displacement of partA's owner.
    publishTwoBlobPart(s, ns, ref, "data-B", "mark-B");

    /// A competing GC round already deleted the displaced partA body: delete it at the BACKEND by exact
    /// token so GC's fold reads it absent (the removal -1 then comes from the recorded owner edge, never
    /// from the missing body).
    {
        const String key = s->layout().manifestKey(part_a);
        const auto head = b->head(key);
        EXPECT_TRUE(head.exists) << "partA manifest body must exist before we delete it";
        EXPECT_EQ(b->deleteExact(key, head.token).kind, DeleteOutcome::Kind::Deleted);
        EXPECT_FALSE(b->head(key).exists) << "partA manifest body must be gone after the delete";
    }

    /// The displacing publish dropped partA's owner; advance the watermark floor so partA's now-orphaned
    /// blobs are not spared as in-flight, then run GC to a fixpoint.
    s->renewWatermarkOnce();
    Gc gc(s, hexToU128("00000000000000000000000000000001"));
    runGcToFixpoint(gc);
    return runFsck(*s, /*detail=*/false);
}

}

/// ---- OBSOLETE under B7: adopt-by-tree relink leak cases (no shared content id, no subtree) ----
///
/// The relink/republish/staged-subtree leak cases targeted the removed "adopt a tree by its shared
/// content id" path. In the part-manifest model every part is a per-instance `ManifestId` (manifests are
/// NEVER shared), there is no `Placement::Subtree`, and the cross-server relink is gone: `getPartTreeId`
/// returns nullopt and `adoptPart` throws `NOT_IMPLEMENTED` (backlog item B7 — the sender always streams
/// bytes; a relink would be a byte-stream-fallback publish, which is just an ordinary `publishTwoBlobPart`
/// already covered by the no-leak repros below). These have no manifest analog, so they are skipped, not
/// weakened.

TEST(CasGcLeak, RelinkAdoptedRootFoldNoLeak_ObsoleteB7)
{
    GTEST_SKIP() << "B7: adopt-by-tree relink removed (per-instance manifests, no shared content id; "
                    "getPartTreeId=nullopt, adoptPart throws NOT_IMPLEMENTED). The byte-stream-fallback "
                    "relink is an ordinary publish — covered by the no-leak displacement repros.";
}

TEST(CasGcLeak, RepublishAdoptedRootFoldNoLeak_ObsoleteB7)
{
    GTEST_SKIP() << "B7: rename/move via adopt-by-subtree-evidence removed (no Placement::Subtree in the "
                    "manifest model). Rename is an ordinary republish — covered by the no-leak repros.";
}

TEST(CasGcLeak, StagedParentWithAbsentAdoptedSubtreeNoLeak_ObsoleteB7)
{
    GTEST_SKIP() << "B7: nested staged-parent + adopted-subtree closures removed (manifests are flat, "
                    "single-owner, no subtree placement). The own-blob no-leak property it asserted is "
                    "covered by the abandon repro AbandonedPrecommitReclaimsOwnBlobs below.";
}

/// NO-LEAK (S1, fold interleaved): partA is published and folded ONCE (its body present, +1 per blob),
/// then partB displaces it on the same ref and partA's body is deleted (a competing GC round's landed
/// delete). GC must reclaim partA's blobs to a fixpoint: no blob/manifest object remains for partA and
/// the in-degree generation holds no stranded positive counter for partA's blobs.
TEST(CasGcLeak, DisplacedPartBlobsReclaimedFoldBetween)
{
    std::shared_ptr<InMemoryBackend> b;
    auto s = openTestStore(b);
    const RootNamespace ns{"srv1/tbl"};
    const String ref = "all_0_0_0";

    const ManifestId part_a = publishTwoBlobPart(s, ns, ref, "data-A", "mark-A");

    /// A GC fold runs HERE, before any displacement — partA's body is present, so the fold records +1 for
    /// each of partA's blobs into the durable in-degree generation.
    {
        Gc gc(s, hexToU128("00000000000000000000000000000001"));
        runGcToFixpoint(gc);
    }
    EXPECT_EQ(inDegreeOf(*b, s->layout(), u128Of("data-A")), 1) << "partA's data blob is pinned (+1)";
    EXPECT_EQ(inDegreeOf(*b, s->layout(), u128Of("mark-A")), 1) << "partA's mark blob is pinned (+1)";

    const FsckReport after = displaceDeleteAndGc(s, b, ns, ref, part_a);

    EXPECT_EQ(after.dangling, 0u) << "S1 INV-NO-LOSS: displacement must never lose a reachable object";
    EXPECT_GT(after.reachable, 0u) << "S1: the live ref points at partB; partB's closure is reachable";
    EXPECT_EQ(after.unreachable, 0u)
        << "S1 INV-NO-LEAK: an interleaved fold recorded partA's edges; the removal -1 + retire must "
           "reclaim partA's blobs (unreachable=" << after.unreachable << ")";

    /// Backend-level no-debris: partA's blobs and body object are gone; the in-degree counters are 0.
    EXPECT_FALSE(blobPresent(b, s->layout(), "data-A")) << "S1: partA data blob object must be deleted";
    EXPECT_FALSE(blobPresent(b, s->layout(), "mark-A")) << "S1: partA mark blob object must be deleted";
    EXPECT_FALSE(manifestPresent(b, s->layout(), part_a)) << "S1: partA manifest body must be gone";
    EXPECT_EQ(inDegreeOf(*b, s->layout(), u128Of("data-A")), 0) << "S1: no stranded positive in-degree";
    EXPECT_EQ(inDegreeOf(*b, s->layout(), u128Of("mark-A")), 0) << "S1: no stranded positive in-degree";
}

/// NO-LEAK (S2, NO fold interleaved — the decisive worst case): partA is published, then IMMEDIATELY
/// displaced by partB and its body deleted before ANY GC fold runs. The single fold therefore folds
/// partA's activation (+1, body present at activation-fold) and its removal (-1) and the body-delete all
/// at once. The removal -1 must come from the recorded owner edge even though partA's body is gone, and
/// the retire must reclaim partA's blobs. No debris may remain.
TEST(CasGcLeak, DisplacedPartBlobsReclaimedNoFoldBetween)
{
    std::shared_ptr<InMemoryBackend> b;
    auto s = openTestStore(b);
    const RootNamespace ns{"srv1/tbl"};
    const String ref = "all_0_0_0";

    const ManifestId part_a = publishTwoBlobPart(s, ns, ref, "data-A", "mark-A");

    const FsckReport after = displaceDeleteAndGc(s, b, ns, ref, part_a);

    EXPECT_EQ(after.dangling, 0u) << "S2 INV-NO-LOSS: displacement must never lose a reachable object";
    EXPECT_GT(after.reachable, 0u) << "S2: the live ref points at partB; partB's closure is reachable";
    EXPECT_EQ(after.unreachable, 0u)
        << "S2 INV-NO-LEAK: partA's blobs must be reclaimed even with no interleaved fold — the recorded "
           "owner edges drive the removal -1 + retire (unreachable=" << after.unreachable << ")";

    EXPECT_FALSE(blobPresent(b, s->layout(), "data-A")) << "S2: partA data blob object must be deleted";
    EXPECT_FALSE(blobPresent(b, s->layout(), "mark-A")) << "S2: partA mark blob object must be deleted";
    EXPECT_FALSE(manifestPresent(b, s->layout(), part_a)) << "S2: partA manifest body must be gone";
    EXPECT_EQ(inDegreeOf(*b, s->layout(), u128Of("data-A")), 0) << "S2: no stranded positive in-degree";
    EXPECT_EQ(inDegreeOf(*b, s->layout(), u128Of("mark-A")), 0) << "S2: no stranded positive in-degree";
}

/// NO-LEAK (drop): a fully-committed part is published, folded (+1 per blob), then its ref is dropped.
/// GC must reclaim the WHOLE closure — both blobs and the manifest body — leaving no debris and no
/// stranded positive in-degree.
TEST(CasGcLeak, DroppedPartFullyReclaimed)
{
    std::shared_ptr<InMemoryBackend> b;
    auto s = openTestStore(b);
    const RootNamespace ns{"srv1/tbl"};
    const String ref = "all_1_1_0";

    const ManifestId id = publishTwoBlobPart(s, ns, ref, "drop-data", "drop-mark");
    {
        Gc gc(s, hexToU128("00000000000000000000000000000002"));
        runGcToFixpoint(gc);
    }
    EXPECT_EQ(inDegreeOf(*b, s->layout(), u128Of("drop-data")), 1);
    EXPECT_EQ(inDegreeOf(*b, s->layout(), u128Of("drop-mark")), 1);

    s->dropRef(ns, ref);
    s->renewWatermarkOnce();   /// advance the floor so the now-unreferenced closure is not spared

    Gc gc(s, hexToU128("00000000000000000000000000000002"));
    runGcToFixpoint(gc);

    const FsckReport after = runFsck(*s, /*detail=*/false);
    EXPECT_EQ(after.dangling, 0u) << "drop INV-NO-LOSS: nothing reachable was lost";
    EXPECT_EQ(after.unreachable, 0u)
        << "drop INV-NO-LEAK: the dropped closure's blobs + body must be fully reclaimed "
           "(unreachable=" << after.unreachable << ")";
    EXPECT_FALSE(blobPresent(b, s->layout(), "drop-data")) << "dropped data blob must be deleted";
    EXPECT_FALSE(blobPresent(b, s->layout(), "drop-mark")) << "dropped mark blob must be deleted";
    EXPECT_FALSE(manifestPresent(b, s->layout(), id)) << "dropped manifest body must be gone";
    EXPECT_EQ(inDegreeOf(*b, s->layout(), u128Of("drop-data")), 0) << "no stranded positive in-degree";
    EXPECT_EQ(inDegreeOf(*b, s->layout(), u128Of("drop-mark")), 0) << "no stranded positive in-degree";
}

/// NO-LEAK (abandon): a build stages a manifest, precommitAdds it, and uploads its OWN unique blob, then
/// ABANDONS (never promotes). The orphaned precommit owner intent must be reclaimed and the build's own
/// unique blob freed — no debris for the abandoned closure, no stranded positive in-degree.
///
/// (This replaces the obsolete staged-parent/adopted-subtree own-blob no-leak case — there is no subtree
/// in the manifest model, so the property is exercised through a flat staged-then-abandoned part.)
TEST(CasGcLeak, AbandonedPrecommitReclaimsOwnBlobs)
{
    std::shared_ptr<InMemoryBackend> b;
    auto s = openTestStore(b);
    const RootNamespace ns{"srv1/tbl"};
    const String ref = "all_2_2_0";
    const String payload = "abandon-unique-blob";

    BuildInfo info;
    info.intended_ref = ns.string() + "/" + ref;
    auto build = s->startBuild(info);
    build->putBlob(idOf(payload), BlobSource::fromString(payload));   /// the build's OWN unique blob
    const ManifestId id = build->stageManifest({blobEntry("data.bin", payload)});
    build->precommitAdd(ns, ref, id);
    build->abandon();                                                 /// no promote — abandoned

    /// Build finished (abandoned) → advance the floor past its seq so GC reclaims the orphaned precommit.
    s->renewWatermarkOnce();

    Gc gc(s, hexToU128("00000000000000000000000000000003"));
    EXPECT_NO_THROW(runGcToFixpoint(gc));

    const FsckReport after = runFsck(*s, /*detail=*/false);
    EXPECT_EQ(after.dangling, 0u) << "abandon INV-NO-LOSS: nothing reachable was lost";
    EXPECT_EQ(after.unreachable, 0u)
        << "abandon INV-NO-LEAK: the abandoned precommit's own unique blob must be reclaimed "
           "(unreachable=" << after.unreachable << ")";
    EXPECT_FALSE(blobPresent(b, s->layout(), payload)) << "abandoned build's own blob must be deleted";
    EXPECT_EQ(inDegreeOf(*b, s->layout(), u128Of(payload)), 0) << "no stranded positive in-degree";
}

/// REUSE-vs-GC race (no-LOSS half of the no-leak family): a build ADOPTS a committed blob B by tokenless
/// evidence (B present, not yet condemned), the committed ref pinning B is DROPPED, GC retires+deletes B
/// AND completes the round, and only THEN does the build try to publish a manifest naming B.
///
/// The promote gate re-observes the loss (it re-HEADs every blob leaf and fails closed on a deleted dep,
/// throwing a retryable ABORTED) — it must NEVER silently commit a dangling ref. The assertion is the
/// no-LOSS guarantee: `dangling==0`. (A tokenless adopt has no body to re-upload, so a real caller would
/// re-derive B from source on retry; here we only confirm the gate fails closed.)
TEST(CasReuseGcRace, ReuseOfBlobDeletedBeforePublish)
{
    std::shared_ptr<InMemoryBackend> b;
    auto s = openTestStore(b);
    const RootNamespace ns{"srv1/tbl"};
    const String B = "shared-blob-payload";
    const String U = "build2-unique-blob";

    /// build1: commit part_1 -> manifest -> blob B.
    publishOneBlobPart(s, ns, "part_1", B);

    /// build2: adopt B by tokenless evidence (no HEAD), upload its OWN unique blob U, stage a manifest
    /// naming both, precommitAdd — but do NOT promote yet.
    BuildInfo info;
    info.intended_ref = ns.string() + "/part_2";
    auto build2 = s->startBuild(info);

    ManifestEntry eb;
    eb.path = "data.bin";
    eb.placement = EntryPlacement::Blob;
    eb.blob_hash = u128Of(B);
    eb.blob_size = B.size();
    build2->adoptEvidence(eb);                                   /// tokenless dep (no HEAD)
    build2->putBlob(idOf(U), BlobSource::fromString(U));         /// build2's own unique, protected blob

    const ManifestId id2 = build2->stageManifest({eb, blobEntry("uniq.bin", U)});
    build2->precommitAdd(ns, "part_2", id2);

    /// Drop the committed pin on B and advance the watermark so B (owned by the finished build1) is not
    /// spared. build2 has NOT promoted, so the precommit body names B but no committed owner pins it; GC
    /// folds B to in-degree 0 once part_1 is dropped.
    s->dropRef(ns, "part_1");
    s->renewWatermarkOnce();

    /// GC reclaims build1's manifest and B to a fixpoint, completing the rounds.
    {
        Gc gc(s, u128Of("gc-reuse-race"));
        runGcToFixpoint(gc);
    }
    ASSERT_FALSE(blobPresent(b, s->layout(), B))
        << "GC must have deleted the now-unreferenced reused blob B";

    /// build2 promotes part_2 -> id2 -> {B, U}. The gate must NOT commit a ref to the deleted B. promote
    /// may throw a retryable ABORTED if the gate re-observes the loss — that is CORRECT (no dangle); only
    /// a SILENT commit of a dangling ref is the bug.
    bool published = false;
    for (int attempt = 0; attempt < 8 && !published; ++attempt)
    {
        try
        {
            build2->promote(ns, "part_2", build2->buildId(), id2);
            published = true;
        }
        catch (const DB::Exception & e)
        {
            if (e.code() != DB::ErrorCodes::ABORTED)
                throw;
            break;   /// fail-closed (no dangle) is the safe outcome; stop.
        }
    }

    /// THE ASSERTION: no reachable object is missing. A dangling B == an INV-NO-LOSS violation.
    const FsckReport rep = runFsck(*s, /*detail=*/true);
    EXPECT_EQ(rep.dangling, 0u)
        << "reuse adopted a blob GC later deleted; the promote gate must re-observe B (or fail closed), "
           "never commit a dangling ref (dangling=" << rep.dangling
        << ", reachable=" << rep.reachable << ")";
}
