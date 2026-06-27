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
using DB::Cas::tests::publishCommittedTransition;
using DB::Cas::tests::appendOwnerEvent;

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

/// Stage partB's full closure (its two distinct blob bodies + its manifest body) through the REAL
/// writer primitives WITHOUT publishing an owner — `startBuild -> putBlob(each) -> stageManifest`. The
/// bytes are durable in the backend but no journal owner names them yet; the caller installs partB as
/// the new owner via a REPOINT (see displaceAndGc). Returns partB's ManifestId.
ManifestId stagePartBClosure(
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
    /// No precommitAdd / promote: the repoint below installs partB committed in ONE owner-move event.
    return id;
}

/// Reproduce displacement on the SAME (s, ns, ref) and run GC to a fixpoint. partB's distinct blobs
/// displace partA's via a REPOINT of the ref (one RootOwnerEvent old={Committed,ref,partA}/
/// new={Committed,ref,partB}) — the real production shape of last-owner-wins, NOT a body delete.
///
/// Crucially the test does NOT delete partA's manifest body. In the part-manifest model a true removal
/// (the repoint's -1) is derived by GC READING partA's body at removal-fold time; only GC may delete a
/// committed owner's body, and only AFTER the -1 is sealed (recheck cleanup, control #11). So GC folds
/// the repoint: -1 for partA's blobs (body present), +1 for partB's blobs, retires + deletes partA's
/// now-zero-in-degree blobs, and recheck cleanup deletes partA's owner-removed body. Returns the fsck
/// report so the caller can assert the no-leak end state (partA's blobs AND body gone, unreachable==0).
FsckReport displaceAndGc(
    const StorePtr & s, const std::shared_ptr<InMemoryBackend> & b,
    const RootNamespace & ns, const String & ref, const ManifestId & part_a)
{
    /// Stage partB's full closure (blobs + body present), then repoint the ref from partA to partB.
    const ManifestId part_b = stagePartBClosure(s, ns, ref, "data-B", "mark-B");

    EXPECT_TRUE(b->head(s->layout().manifestKey(part_a)).exists)
        << "partA manifest body must still be present so GC can read its -1 edges at removal-fold";

    /// REPOINT: old={Committed,ref,partA} / new={Committed,ref,partB} in the single ordered journal.
    /// (root_shards=1, so the ref's shard is 0 — matches publishCommittedTransition's default.)
    publishCommittedTransition(*b, s->layout(), ns, ref, part_a.ref, part_b.ref);

    /// The repoint dropped partA's owner; advance the watermark floor so partA's now-orphaned blobs are
    /// not spared as in-flight, then run GC to a fixpoint.
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
/// then partB REPOINTS the ref away from partA (partA's body stays present so GC reads its -1 edges at
/// removal-fold; only GC deletes the owner-removed body, after the -1 is sealed). GC must reclaim partA's
/// blobs to a fixpoint: no blob/manifest object remains for partA and the in-degree generation holds no
/// stranded positive counter for partA's blobs.
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

    const FsckReport after = displaceAndGc(s, b, ns, ref, part_a);

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
/// repointed to partB before ANY GC fold runs. The single fold therefore folds partA's activation (+1)
/// and its removal (-1, read from partA's still-present body) in one pass; the retire reclaims partA's
/// blobs and recheck cleanup deletes partA's owner-removed body. No debris may remain.
TEST(CasGcLeak, DisplacedPartBlobsReclaimedNoFoldBetween)
{
    std::shared_ptr<InMemoryBackend> b;
    auto s = openTestStore(b);
    const RootNamespace ns{"srv1/tbl"};
    const String ref = "all_0_0_0";

    const ManifestId part_a = publishTwoBlobPart(s, ns, ref, "data-A", "mark-A");

    const FsckReport after = displaceAndGc(s, b, ns, ref, part_a);

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

/// NO-LEAK (abandon): a build stages a manifest, precommitAdds it (activating +1 for its OWN unique
/// blob, body present), uploads that blob, then is ABANDONED (never promoted). Its create-precommit
/// owner is then REMOVED (a PrecommitRemove RootOwnerEvent old={Precommit,ref,build,id}/new=none on the
/// SAME table shard). GC folds that removal as a true removal: -1 for the blob, retire + delete — no
/// debris for the abandoned closure, no stranded positive in-degree.
///
/// NOTE (B8): GC's automatic precommit-reclaim (`reclaimAbandonedPrecommit`) only runs for `_precommits`
/// namespaces, but `precommitAdd` writes the create-precommit into the TARGET TABLE shard. So an
/// abandoned precommit's edges are NOT auto-released by the current core; this test drives the removal
/// itself (the production reclaim of a table-shard precommit is the same removal event), keeping the
/// no-leak assertion strong rather than skipping.
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
    build->precommitAdd(ns, ref, id);                                 /// activating +1 for the blob (body present)
    const UInt128 build_id = build->buildId();

    /// Drive the precommit removal (B8: not auto-reclaimed on a table shard). old = the create-precommit
    /// binding precommitAdd wrote {Precommit, ref, build_id, id.ref}; new = none. The manifest body is
    /// STILL PRESENT, so the removal-fold reads its -1 edge for the blob (control #11 fail-close path not
    /// taken). root_shards=1 ⇒ shard 0. We do NOT call `abandon()` — abandon out-of-band deletes the
    /// staged body, which would make the removal-fold read it absent and (faithfully) emit NO -1 (a
    /// removed precommit with absent body emitted no edges to mirror); only GC may delete the body, after
    /// the -1 is sealed. The build's dtor (build.reset() below) retires the build_seq — the crash path —
    /// without touching the body.
    {
        OwnerBinding precommit_b{.owner_kind = OwnerKind::Precommit,
            .ref_name = ref, .build_id = build_id, .manifest_ref = id.ref};
        appendOwnerEvent(*b, s->layout(), ns, /*shard=*/0, precommit_b, std::nullopt);
    }
    build.reset();   /// dtor retires the build_seq (crash semantics); does NOT delete the staged body

    /// Build finished → advance the floor past its seq so GC reclaims the orphaned precommit's blob.
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

    /// build2: adopt B by tokenless evidence (no HEAD) and upload its OWN unique blob U. It does NOT yet
    /// stage a manifest or precommit — the scenario is that GC deletes B BEFORE build2 publishes a manifest
    /// naming it. (Staging+precommitting BEFORE the drop would make the precommit's activating +1 PIN B —
    /// B would never reach in-degree 0 and GC could not delete it, so the race could not be reproduced.)
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

    /// Drop the committed pin on B and advance the watermark so B (owned by the finished build1) is not
    /// spared. No owner names B now (build2 has not staged/precommitted), so GC folds B to in-degree 0
    /// once part_1 is dropped.
    s->dropRef(ns, "part_1");
    s->renewWatermarkOnce();

    /// GC reclaims build1's manifest and the now-unreferenced B to a fixpoint, completing the rounds.
    {
        Gc gc(s, u128Of("gc-reuse-race"));
        runGcToFixpoint(gc);
    }
    ASSERT_FALSE(blobPresent(b, s->layout(), B))
        << "GC must have deleted the now-unreferenced reused blob B";

    /// Only NOW does build2 publish a manifest naming the (just-deleted) B: stage the body + precommit.
    const ManifestId id2 = build2->stageManifest({eb, blobEntry("uniq.bin", U)});
    build2->precommitAdd(ns, "part_2", id2);

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
