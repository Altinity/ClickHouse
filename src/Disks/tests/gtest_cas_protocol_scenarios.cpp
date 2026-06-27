#include <gtest/gtest.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasBuild.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasStore.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasInMemoryBackend.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasEnvelope.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasGc.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasGcFormats.h>
#include <Disks/tests/cas_test_helpers.h>
#include <Common/Exception.h>

/// Multi-actor protocol scenarios for the root-local part-manifest model (CA GC redesign rev. 15).
/// Ported from the removed tree/closure model. The single-call `publish(ns, ref, tree, RefPayload{})`
/// gate is gone; a write is now the four-step flow:
///   stageManifest(entries) -> precommitAdd(ns, ref, id) -> putBlob(...) -> promote(ns, ref, build_id, id)
/// The fail-closed publish gate that those scenarios exercise now lives in TWO places:
///   • putBlob: INV-1 condemned-dedup re-upload from the writer's OWN source bytes (never GETs the
///     dying object);
///   • promote: a fail-closed HEAD over EVERY blob leaf of the staged manifest body — a blob that is
///     ABSENT or condemned-at-its-CURRENT-token ⇒ ABORTED (the committed ref never names a missing or
///     dying blob). promote refreshes the retire view when the shard/registry fence is ahead of it.
/// These scenarios assert the no-dangle / no-loss / fail-closed protocol properties faithfully on that
/// flow. The strong safety assertions are preserved.

namespace DB::ErrorCodes
{
extern const int ABORTED;
extern const int FILE_DOESNT_EXIST;
extern const int LOGICAL_ERROR;
}

using namespace DB::Cas;
using DB::Cas::tests::blobEntryFor;
using DB::Cas::tests::displaceBlobToken;
using DB::Cas::tests::expectThrowsCode;
using DB::Cas::tests::fenceNamespace;
using DB::Cas::tests::idOf;
using DB::Cas::tests::injectRetire;
using DB::Cas::tests::u128Of;
using DB::Cas::tests::writeBlobRaw;

namespace
{

StorePtr openStore(const std::shared_ptr<InMemoryBackend> & b)
{
    return Store::open(b, PoolConfig{.pool_prefix = "p"});
}

/// A single-blob manifest entry naming `payload` at `path` (the entry the part's manifest carries).
ManifestEntry blobEntry(const String & path, const String & payload)
{
    return blobEntryFor(path, u128Of(payload), payload.size());
}

/// Start a build whose `intended_ref` is "ns/ref" — REQUIRED: stageManifest derives the manifest's
/// owning namespace by splitting intended_ref on the LAST '/'. (See Build::manifestNamespace.)
BuildPtr startBuildFor(const StorePtr & s, const RootNamespace & ns, const String & ref)
{
    BuildInfo info;
    info.intended_ref = ns.string() + "/" + ref;
    return s->startBuild(info);
}

/// The full write flow for a part whose only file is `payload` at `path` (blob placement). Uploads the
/// blob via putBlob, stages the manifest, precommits, then promotes. Returns the committed ManifestId.
/// Mirrors what the old single-call `publish` did on the tree model.
ManifestId publishBlobPart(
    const StorePtr & s, const RootNamespace & ns, const String & ref, const String & path, const String & payload)
{
    auto build = startBuildFor(s, ns, ref);
    build->putBlob(idOf(payload), BlobSource::fromString(payload));
    const ManifestId id = build->stageManifest({blobEntry(path, payload)});
    build->precommitAdd(ns, ref, id);
    build->promote(ns, ref, build->buildId(), id);
    return id;
}

/// Read the part's blob back through the full read stack (resolveRef → readManifest → lookupPath →
/// locate → ranged GET) and assert it returns `payload`. This is the INV-NO-DANGLE check: every named
/// object resolves and reads.
void assertPartReads(
    const std::shared_ptr<InMemoryBackend> & b, const StorePtr & s,
    const RootNamespace & ns, const String & ref, const String & path, const String & payload)
{
    auto r = s->resolveRef(ns, ref);
    ASSERT_TRUE(r.has_value());

    const PartManifest manifest = s->readManifest(r->manifest_id);
    auto entry = s->lookupPath(manifest, path);
    ASSERT_TRUE(entry.has_value());
    auto loc = s->locate(*entry);
    auto got = b->get(loc.key, Range{loc.offset, loc.length});
    ASSERT_TRUE(got.has_value());
    EXPECT_EQ(got->bytes, payload);
}

}

TEST(CasProtocol, FenceConflictCondemnedBlobDepAbortsRetryable)
{
    /// INV-1: a blob leaf whose CURRENT token is condemned at the promote gate has no source bytes
    /// available (a precommitted build's promote holds no BlobSource). The gate MUST throw ABORTED
    /// (retryable), never GET the dying object. The caller retries the whole build from scratch.
    auto b = std::make_shared<InMemoryBackend>();
    auto s = openStore(b);
    const RootNamespace ns{"srv1/tbl"};

    /// Build: upload X (records token t0 at view round 0), stage the manifest, precommit.
    auto build = startBuildFor(s, ns, "part_1");
    build->putBlob(idOf("payload-X"), BlobSource::fromString("payload-X"));
    const ManifestId id = build->stageManifest({blobEntry("data.bin", "payload-X")});
    build->precommitAdd(ns, "part_1", id);

    const String blob_key = s->layout().blobKey(idOf("payload-X"));
    const Token t0 = b->head(blob_key).token;

    /// GC condemns X at t0 in round 1 and fences the namespace to round 1.
    injectRetire(*b, s->layout(), /*round*/ 1, /*fence_seq*/ 0, /*shard*/ 0,
        {RetiredEntry{.kind = ObjectKind::Blob, .hash = u128Of("payload-X"), .token = t0, .size = 9}});
    fenceNamespace(*b, s->layout(), ns, s->poolMeta().root_shards, /*round*/ 1);

    /// promote: mutateShard refreshes the view (fence_round 1 > view round 0), then revalidates X:
    /// HEAD t0 is condemned ⇒ ABORTED (INV-1; caller must retry from scratch).
    expectThrowsCode(DB::ErrorCodes::ABORTED, [&] { build->promote(ns, "part_1", build->buildId(), id); });

    /// The condemned object is still at t0 — nothing was written.
    EXPECT_EQ(b->head(blob_key).token, t0);
    /// No ref was published.
    EXPECT_FALSE(s->resolveRef(ns, "part_1").has_value());
}

TEST(CasProtocol, RevalidateReObservesStaleTokenKeepsWhenUnchanged)
{
    /// The keep-branch: a blob dedup-adopted at round 0, an EMPTY retire set at round 1, the blob's
    /// token unchanged. promote re-HEADs at the gate, finds t0 live (not condemned) ⇒ commits in place,
    /// no rewrite.
    auto b = std::make_shared<InMemoryBackend>();
    auto s = openStore(b);
    const RootNamespace ns{"srv1/tbl"};

    /// X pre-exists out-of-band; the build dedup-adopts it via putBlob (records the current token t0).
    writeBlobRaw(*b, s->layout(), "payload-X", s->poolMeta().blob_header_len, s->poolMeta().pool_id);
    const String blob_key = s->layout().blobKey(idOf("payload-X"));
    const Token t0 = b->head(blob_key).token;

    auto build = startBuildFor(s, ns, "part_1");
    build->putBlob(idOf("payload-X"), BlobSource::fromString("payload-X"));   /// dedup → adopts t0
    const ManifestId id = build->stageManifest({blobEntry("data.bin", "payload-X")});
    build->precommitAdd(ns, "part_1", id);

    /// GC advanced the round to 1 with an EMPTY retired set; fence to 1. X is NOT condemned and its
    /// token is unchanged.
    injectRetire(*b, s->layout(), /*round*/ 1, /*fence_seq*/ 0, /*shard*/ 0, {});
    fenceNamespace(*b, s->layout(), ns, s->poolMeta().root_shards, /*round*/ 1);

    /// promote refreshes ⇒ revalidate X ⇒ HEAD t0 not condemned ⇒ commit (KEEP).
    build->promote(ns, "part_1", build->buildId(), id);

    assertPartReads(b, s, ns, "part_1", "data.bin", "payload-X");
    /// No rewrite happened — the keep-branch left the object's token at t0.
    EXPECT_EQ(b->head(blob_key).token, t0);
}

TEST(CasProtocol, RevalidateReObservesStaleTokenAdoptsWhenDisplaced)
{
    /// A blob displaced out-of-band to a fresh live token t1 before the gate. promote re-HEADs the
    /// CURRENT token (t1), finds it live ⇒ commits; the part rides t1.
    auto b = std::make_shared<InMemoryBackend>();
    auto s = openStore(b);
    const RootNamespace ns{"srv1/tbl"};

    writeBlobRaw(*b, s->layout(), "payload-X", s->poolMeta().blob_header_len, s->poolMeta().pool_id);
    const String blob_key = s->layout().blobKey(idOf("payload-X"));
    const Token t0 = b->head(blob_key).token;

    auto build = startBuildFor(s, ns, "part_1");
    build->putBlob(idOf("payload-X"), BlobSource::fromString("payload-X"));   /// dedup → adopts t0
    const ManifestId id = build->stageManifest({blobEntry("data.bin", "payload-X")});
    build->precommitAdd(ns, "part_1", id);

    /// Another writer displaces X out-of-band ⇒ a new current token t1 (same payload, fresh tag).
    const Token t1 = displaceBlobToken(*b, s->layout(), idOf("payload-X"));
    EXPECT_NE(t1, t0);

    /// GC advanced to round 1 with an EMPTY retired set; fence to 1.
    injectRetire(*b, s->layout(), /*round*/ 1, /*fence_seq*/ 0, /*shard*/ 0, {});
    fenceNamespace(*b, s->layout(), ns, s->poolMeta().root_shards, /*round*/ 1);

    /// promote refreshes ⇒ revalidate X ⇒ HEAD current t1 not condemned ⇒ commit. The dep rides t1.
    build->promote(ns, "part_1", build->buildId(), id);
    assertPartReads(b, s, ns, "part_1", "data.bin", "payload-X");
    EXPECT_EQ(b->head(blob_key).token, t1);

    /// Black-box proof the part reads the t1 incarnation: re-publish the same blob into a SECOND
    /// namespace with NO new GC injection. The blob is already present at t1; nothing is re-uploaded.
    publishBlobPart(s, RootNamespace{"srv1/tbl/copy"}, "part_2", "data.bin", "payload-X");
    EXPECT_EQ(b->head(blob_key).token, t1);
    assertPartReads(b, s, RootNamespace{"srv1/tbl/copy"}, "part_2", "data.bin", "payload-X");

    /// Independent discriminator that the blob rides t1, not the stale t0: t0 is DEAD. A deleteExact
    /// against t0 must TokenMismatch (INV-NO-RETURN — t0 was displaced and can never be current again).
    EXPECT_EQ(b->deleteExact(blob_key, t0).kind, DeleteOutcome::Kind::TokenMismatch);
}

TEST(CasProtocol, RevalidateAdoptsLiveTokenWhenOnlyPhantomCondemnedAtDifferentToken)
{
    /// A blob whose OWN current token t0 is LIVE, but a DIFFERENT phantom token t_other for the same
    /// hash IS condemned. promote HEADs t0; isCondemnedToken(t0)=false ⇒ commit in place (no upload,
    /// no displacement). The condemnation is for a different incarnation and does not touch t0.
    auto b = std::make_shared<InMemoryBackend>();
    const RootNamespace ns{"srv1/tbl"};

    DB::Cas::Layout layout("p");
    {
        auto s0 = Store::open(b, PoolConfig{.pool_prefix = "p"});
        writeBlobRaw(*b, s0->layout(), "payload-X", s0->poolMeta().blob_header_len, s0->poolMeta().pool_id);
    }
    const String blob_key = layout.blobKey(idOf("payload-X"));
    const Token t0 = b->head(blob_key).token;
    const Token t_other{"emulated-phantom", DB::Cas::TokenType::Emulated};
    ASSERT_NE(t_other, t0);

    injectRetire(*b, layout, /*round*/ 1, /*fence_seq*/ 0, /*shard*/ 0,
        {RetiredEntry{.kind = ObjectKind::Blob, .hash = u128Of("payload-X"), .token = t_other, .size = 9}});
    /// Fence to round 1 BEFORE opening the store, so the store's open-time refresh lands the view at
    /// round 1 already populated.
    fenceNamespace(*b, layout, ns, /*n_shards*/ 8, /*round*/ 1);

    auto s = Store::open(b, PoolConfig{.pool_prefix = "p"});   /// open-time refresh ⇒ view round 1
    auto build = startBuildFor(s, ns, "part_1");
    build->putBlob(idOf("payload-X"), BlobSource::fromString("payload-X"));   /// dedup → adopts t0
    const ManifestId id = build->stageManifest({blobEntry("data.bin", "payload-X")});
    build->precommitAdd(ns, "part_1", id);

    /// promote: revalidate X ⇒ HEAD t0; t0 is NOT in the condemned set {t_other} ⇒ commit. Lands.
    build->promote(ns, "part_1", build->buildId(), id);

    assertPartReads(b, s, ns, "part_1", "data.bin", "payload-X");

    /// The object was NOT displaced — it STAYS at t0 (no re-upload, only re-validated).
    EXPECT_EQ(b->head(blob_key).token, t0);
}

TEST(CasProtocol, RevalidateAbsentBlobDepAbortsRetryable)
{
    /// A blob deleted (a landed GC delete) before the gate. promote HEADs it ⇒ absent ⇒ ABORTED.
    auto b = std::make_shared<InMemoryBackend>();
    auto s = openStore(b);
    const RootNamespace ns{"srv1/tbl"};

    writeBlobRaw(*b, s->layout(), "payload-X", s->poolMeta().blob_header_len, s->poolMeta().pool_id);
    const String blob_key = s->layout().blobKey(idOf("payload-X"));

    auto build = startBuildFor(s, ns, "part_1");
    build->putBlob(idOf("payload-X"), BlobSource::fromString("payload-X"));   /// dedup → adopts current token
    const ManifestId id = build->stageManifest({blobEntry("data.bin", "payload-X")});
    build->precommitAdd(ns, "part_1", id);

    /// A landed GC delete: delete X raw by its current token.
    const Token cur = b->head(blob_key).token;
    ASSERT_EQ(b->deleteExact(blob_key, cur).kind, DeleteOutcome::Kind::Deleted);

    injectRetire(*b, s->layout(), /*round*/ 1, /*fence_seq*/ 0, /*shard*/ 0, {});
    fenceNamespace(*b, s->layout(), ns, s->poolMeta().root_shards, /*round*/ 1);

    /// promote: revalidate X ⇒ HEAD absent ⇒ ABORTED (retryable). No ref published.
    expectThrowsCode(DB::ErrorCodes::ABORTED, [&] { build->promote(ns, "part_1", build->buildId(), id); });
    EXPECT_FALSE(s->resolveRef(ns, "part_1").has_value());
}

TEST(CasProtocol, EvidenceHitCondemnedBlobAbortsRetryable)
{
    /// W-EVIDENCE (tokenless adopted dep) on a blob X that is condemned at the gate. promote does not
    /// consult the dep set — it HEADs every blob leaf of the manifest body directly. X condemned at its
    /// current token t0 ⇒ ABORTED (no source bytes to re-upload; INV-1 forbids GET).
    auto b = std::make_shared<InMemoryBackend>();
    auto s = openStore(b);
    const RootNamespace ns{"srv1/tbl"};

    /// X pre-exists with token t0; the manifest names it (a tokenless adopted leaf).
    writeBlobRaw(*b, s->layout(), "payload-X", s->poolMeta().blob_header_len, s->poolMeta().pool_id);
    const String blob_key = s->layout().blobKey(idOf("payload-X"));
    const Token t0 = b->head(blob_key).token;

    auto build = startBuildFor(s, ns, "part_1");
    const ManifestEntry entry = blobEntry("data.bin", "payload-X");
    build->adoptEvidence(entry);   /// tokenless W-EVIDENCE dep on X (no HEAD, no upload)
    const ManifestId id = build->stageManifest({entry});
    build->precommitAdd(ns, "part_1", id);

    /// GC condemns X at t0 in round 1 + fence.
    injectRetire(*b, s->layout(), /*round*/ 1, /*fence_seq*/ 0, /*shard*/ 0,
        {RetiredEntry{.kind = ObjectKind::Blob, .hash = u128Of("payload-X"), .token = t0, .size = 9}});
    fenceNamespace(*b, s->layout(), ns, s->poolMeta().root_shards, /*round*/ 1);

    /// promote: revalidate X ⇒ HEAD t0 condemned ⇒ ABORTED (INV-1; no source bytes).
    expectThrowsCode(DB::ErrorCodes::ABORTED, [&] { build->promote(ns, "part_1", build->buildId(), id); });

    EXPECT_EQ(b->head(blob_key).token, t0);
    EXPECT_FALSE(s->resolveRef(ns, "part_1").has_value());
}

TEST(CasProtocol, WedgedHeartbeatCondemnedBlobDepAbortsRetryable)
{
    /// A build whose watermark never renews finds its OWN upload condemned by full GC; its promote
    /// revalidates, sees the blob condemned, and has no retained source bytes at promote time ⇒ ABORTED.
    /// The precommit fence normally removes this window; this validates the degenerate case.
    auto b = std::make_shared<InMemoryBackend>();
    auto s = openStore(b);
    const RootNamespace ns{"srv1/tbl"};

    auto build = startBuildFor(s, ns, "part_1");
    build->putBlob(idOf("payload-X"), BlobSource::fromString("payload-X"));
    const ManifestId id = build->stageManifest({blobEntry("data.bin", "payload-X")});
    build->precommitAdd(ns, "part_1", id);

    const String blob_key = s->layout().blobKey(idOf("payload-X"));
    const Token t0 = b->head(blob_key).token;

    /// Full GC condemned the build's OWN upload.
    injectRetire(*b, s->layout(), /*round*/ 1, /*fence_seq*/ 0, /*shard*/ 0,
        {RetiredEntry{.kind = ObjectKind::Blob, .hash = u128Of("payload-X"), .token = t0, .size = 9}});
    fenceNamespace(*b, s->layout(), ns, s->poolMeta().root_shards, /*round*/ 1);

    /// promote: revalidate X ⇒ HEAD t0 condemned ⇒ ABORTED.
    expectThrowsCode(DB::ErrorCodes::ABORTED, [&] { build->promote(ns, "part_1", build->buildId(), id); });

    EXPECT_EQ(b->head(blob_key).token, t0);
    EXPECT_FALSE(s->resolveRef(ns, "part_1").has_value());
}

TEST(CasProtocol, AbandonLeavesDebrisAndDisables)
{
    /// abandon leaves the uploaded blob + staged manifest body as debris (reaped by the orphan sweep);
    /// no owner transition is touched, and further build ops fail LOGICAL_ERROR (requireAlive).
    auto b = std::make_shared<InMemoryBackend>();
    auto s = openStore(b);
    const RootNamespace ns{"srv1/tbl"};

    auto build = startBuildFor(s, ns, "part_1");
    auto blob = build->putBlob(idOf("payload-X"), BlobSource::fromString("payload-X"));
    const ManifestId id = build->stageManifest({blobEntry("data.bin", "payload-X")});

    build->abandon();

    /// The uploaded blob remains as debris; the staged manifest body is best-effort deleted by abandon.
    EXPECT_TRUE(b->head(s->layout().blobKey(blob.id)).exists);
    EXPECT_FALSE(b->head(s->layout().manifestKey(id)).exists);   /// best-effort cleanup ran
    EXPECT_TRUE(s->listRefs(ns).empty());

    /// Further build ops ⇒ LOGICAL_ERROR (requireAlive).
    expectThrowsCode(DB::ErrorCodes::LOGICAL_ERROR,
        [&] { build->stageManifest({blobEntry("data.bin", "payload-X")}); });
}

TEST(CasProtocol, DropReattachThroughDetachedNamespace)
{
    /// ATTACH choreography (design §4): publish part_1 in ns; re-publish into ns/detached + drop part_1
    /// from ns; then re-publish part_1 back in ns + drop from detached. The BLOB is never re-uploaded
    /// (its token is stable throughout); each namespace gets its own single-owner manifest.
    auto b = std::make_shared<InMemoryBackend>();
    auto s = openStore(b);
    const RootNamespace ns{"srv1/tbl"};
    const RootNamespace detached{"srv1/tbl/detached"};

    publishBlobPart(s, ns, "part_1", "data.bin", "payload-X");

    const String blob_key = s->layout().blobKey(idOf("payload-X"));
    const Token blob_tok = b->head(blob_key).token;

    EXPECT_TRUE(s->listRefs(ns).contains("part_1"));
    EXPECT_TRUE(s->listRefs(detached).empty());

    /// Move to detached: re-publish into detached (adopting the live blob), drop from ns.
    publishBlobPart(s, detached, "part_1", "data.bin", "payload-X");
    s->dropRef(ns, "part_1");

    EXPECT_TRUE(s->listRefs(ns).empty());
    ASSERT_TRUE(s->listRefs(detached).contains("part_1"));
    assertPartReads(b, s, detached, "part_1", "data.bin", "payload-X");

    /// Re-attach: re-publish part_1 back in ns, drop from detached.
    publishBlobPart(s, ns, "part_1", "data.bin", "payload-X");
    s->dropRef(detached, "part_1");

    ASSERT_TRUE(s->listRefs(ns).contains("part_1"));
    assertPartReads(b, s, ns, "part_1", "data.bin", "payload-X");
    EXPECT_TRUE(s->listRefs(detached).empty());

    /// The blob was never re-uploaded (token stable throughout — every publish dedup-adopted it).
    EXPECT_EQ(b->head(blob_key).token, blob_tok);
}

TEST(CasProtocol, FreezeIntoShadowNamespace)
{
    /// FREEZE survives the table's part lifecycle (design §4): a shadow ref is a reachability root that
    /// outlives the dropped live ref.
    auto b = std::make_shared<InMemoryBackend>();
    auto s = openStore(b);
    const RootNamespace ns{"srv1/tbl"};
    const RootNamespace shadow{"shadow/backup1/tbl"};

    publishBlobPart(s, ns, "part_1", "data.bin", "payload-X");

    /// Freeze into the shadow namespace (adopting the live blob), then drop the live ref.
    publishBlobPart(s, shadow, "part_1", "data.bin", "payload-X");
    s->dropRef(ns, "part_1");

    EXPECT_TRUE(s->listRefs(ns).empty());
    /// The shadow ref still resolves and reads after the live ref is gone.
    assertPartReads(b, s, shadow, "part_1", "data.bin", "payload-X");
}

TEST(CasProtocol, DisplacedToLiveTokenCommitsAtCurrentIncarnation)
{
    /// (Ported from the former ResurrectLosesRace scenario.) The old tree-model gate condemned the dep's
    /// OWN recorded token and ABORTED even when the object had been displaced to a live t1. The new model
    /// re-HEADs at promote and checks the CURRENT token: a blob displaced to a LIVE t1 (while its old t0
    /// is condemned for a now-defunct incarnation) is SAFE to commit — the committed manifest names a
    /// blob HASH, the live t1 incarnation backs it, and GC's exact-token delete of t0 only TokenMismatches.
    /// So promote COMMITS. This is the correct no-loss / no-dangle outcome under the re-HEAD gate; the
    /// old conservative ABORTED has no manifest-model analog.
    auto b = std::make_shared<InMemoryBackend>();
    auto s = openStore(b);
    const RootNamespace ns{"srv1/tbl"};

    writeBlobRaw(*b, s->layout(), "payload-X", s->poolMeta().blob_header_len, s->poolMeta().pool_id);
    const String blob_key = s->layout().blobKey(idOf("payload-X"));
    const Token t0 = b->head(blob_key).token;

    auto build = startBuildFor(s, ns, "part_1");
    build->putBlob(idOf("payload-X"), BlobSource::fromString("payload-X"));   /// dedup → adopts t0
    const ManifestId id = build->stageManifest({blobEntry("data.bin", "payload-X")});
    build->precommitAdd(ns, "part_1", id);

    /// Another writer displaces X to t1 (uncondemned) before our gate runs.
    const Token t1 = displaceBlobToken(*b, s->layout(), idOf("payload-X"));
    ASSERT_NE(t1, t0);

    /// The view still condemns the OLD t0 at round 1, fenced.
    injectRetire(*b, s->layout(), /*round*/ 1, /*fence_seq*/ 0, /*shard*/ 0,
        {RetiredEntry{.kind = ObjectKind::Blob, .hash = u128Of("payload-X"), .token = t0, .size = 9}});
    fenceNamespace(*b, s->layout(), ns, s->poolMeta().root_shards, /*round*/ 1);

    /// promote: revalidate X ⇒ HEAD current t1 (NOT condemned; only the defunct t0 is) ⇒ commit.
    build->promote(ns, "part_1", build->buildId(), id);

    /// The blob lives at t1 (the displacing writer's incarnation) and the part reads.
    EXPECT_EQ(b->head(blob_key).token, t1);
    assertPartReads(b, s, ns, "part_1", "data.bin", "payload-X");

    /// NO-LOSS / NO-RETURN: t0 is dead — a deleteExact against it TokenMismatches (the GC delete of the
    /// condemned t0 spares the live t1).
    EXPECT_EQ(b->deleteExact(blob_key, t0).kind, DeleteOutcome::Kind::TokenMismatch);
}

TEST(CasProtocol, NewNamespacePublishSeesRegistryFenceFloor)
{
    /// THE ABSENT-SHARD ORDERING HOLE's regression test (spec §5 W-REGISTER). A publish into a BRAND-NEW
    /// namespace creates its shard with fence_round 0, so the shard alone can never trigger the gate's
    /// fence-advanced refresh. W-REGISTER makes promote's ensureRegistered observe the REGISTRY's
    /// fence_round (raised by the GC round) and refresh the view BEFORE revalidating. Re-expressed on the
    /// manifest model with a blob leaf (the tree object is gone): build B adopts a blob, a GC round
    /// retires + deletes it, then B publishes into a fresh namespace — promote refreshes (registry floor)
    /// and revalidates the now-ABSENT blob ⇒ ABORTED. It must NEVER land a manifest naming a deleted blob.
    auto b = std::make_shared<InMemoryBackend>();
    auto s = openStore(b);

    /// 1. part_1 → a blob in namespace A, through the real Build (registers A).
    const RootNamespace ns_a{"srv1/tbl"};
    auto build_a = startBuildFor(s, ns_a, "part_1");
    build_a->putBlob(idOf("floor-payload"), BlobSource::fromString("floor-payload"));
    const ManifestId id_a = build_a->stageManifest({blobEntry("data.bin", "floor-payload")});
    build_a->precommitAdd(ns_a, "part_1", id_a);
    build_a->promote(ns_a, "part_1", build_a->buildId(), id_a);
    const String blob_key = s->layout().blobKey(idOf("floor-payload"));
    const Token t0 = b->head(blob_key).token;

    /// 2. build B adopts the blob (tokenless W-EVIDENCE) while the view is still at round 0.
    auto build_b = startBuildFor(s, RootNamespace{"srv2/new"}, "part_x");
    build_b->adoptEvidence(blobEntry("data.bin", "floor-payload"));

    /// 3. drop part_1 from A; a REAL GC round retires the blob at t0, deletes it, and fences the
    /// registry (fence_round 1). build_a finished, so advancing the watermark floor condemns the blob.
    s->dropRef(ns_a, "part_1");
    build_a.reset();
    s->renewWatermarkOnce();
    Gc gc(s, hexToU128("00000000000000000000000000000001"));
    for (size_t r = 0; r < 8; ++r)
    {
        const RoundReport rep = gc.runRegularRound();
        if (rep.acquired_lease && rep.candidates == 0 && rep.deleted == 0 && rep.absent == 0
            && rep.replaced == 0 && rep.spared == 0)
            break;
    }
    /// The blob (unreachable) was deleted at t0.
    EXPECT_FALSE(b->head(blob_key).exists);

    /// 4. build B publishes into a BRAND-NEW namespace: ensureRegistered returns the registry's
    /// fence_round (1) > view round (0) ⇒ promote refreshes ⇒ revalidate the blob ⇒ ABSENT ⇒ ABORTED.
    /// It must NEVER land — landing would write a manifest naming a deleted blob (the dangle).
    const ManifestId id_b = build_b->stageManifest({blobEntry("data.bin", "floor-payload")});
    build_b->precommitAdd(RootNamespace{"srv2/new"}, "part_x", id_b);
    expectThrowsCode(DB::ErrorCodes::ABORTED, [&]
    {
        build_b->promote(RootNamespace{"srv2/new"}, "part_x", build_b->buildId(), id_b);
    });

    /// NO DANGLE: nothing in the new namespace names the deleted blob.
    EXPECT_FALSE(s->resolveRef(RootNamespace{"srv2/new"}, "part_x").has_value());
    EXPECT_EQ(b->deleteExact(blob_key, t0).kind, DeleteOutcome::Kind::NotFound);   /// long gone
}

TEST(CasProtocol, FreshEvidenceDepWithViewHitIsResolvedByGate)
{
    /// A TOKENLESS (W-EVIDENCE) leaf recorded at the CURRENT view round (fresh — no stale-refresh) whose
    /// blob is condemned by hash MUST still be caught by promote's unconditional blob revalidation.
    /// Discriminating assertion: promote throws ABORTED (condemned blob, no source) — NOT a dangle.
    auto b = std::make_shared<InMemoryBackend>();

    /// Pre-inject retire state at round=1 BEFORE opening the store — the store's open-time refresh lands
    /// at round=1, so any dep recorded thereafter is non-stale (observed_view_round == 1).
    DB::Cas::Layout layout("p");
    writeBlobRaw(*b, layout, "payload-fresh-ev", 256 /*blob_header_len*/, UInt128{} /*pool_id*/);
    const String blob_key = layout.blobKey(idOf("payload-fresh-ev"));
    const Token t0 = b->head(blob_key).token;
    injectRetire(*b, layout, /*round*/ 1, /*fence_seq*/ 0, /*shard*/ 0,
        {RetiredEntry{.kind = ObjectKind::Blob, .hash = u128Of("payload-fresh-ev"), .token = t0, .size = 16}});

    auto s = openStore(b);
    ASSERT_EQ(s->retireView().round(), 1u);

    const RootNamespace ns{"srv1/tbl"};
    auto build = startBuildFor(s, ns, "part_1");
    /// adoptEvidence records a TOKENLESS dep at observed_view_round = 1 (the current round) — FRESH.
    const ManifestEntry entry = blobEntry("data.bin", "payload-fresh-ev");
    build->adoptEvidence(entry);
    const ManifestId id = build->stageManifest({entry});
    build->precommitAdd(ns, "part_1", id);

    /// No fence advance (round stays 1): promote does NOT refresh, but its blob revalidation is
    /// unconditional — HEAD t0 condemned ⇒ ABORTED.
    expectThrowsCode(DB::ErrorCodes::ABORTED,
        [&] { build->promote(ns, "part_1", build->buildId(), id); });

    EXPECT_EQ(b->head(blob_key).token, t0);
    EXPECT_FALSE(s->resolveRef(ns, "part_1").has_value());
}

TEST(CasProtocol, AdoptedLeafCarriesRealBlobSize)
{
    /// B92 round-trip (re-expressed on the manifest model): an adopted leaf must carry its real
    /// blob_size, NOT 0. Build A publishes a blob; build B adopts that leaf into a second ref. The
    /// adopted manifest's entry must report the same non-zero blob_size as the original.
    auto b = std::make_shared<InMemoryBackend>();
    auto s = openStore(b);
    const RootNamespace ns{"srv1/tbl"};

    /// Build A: a blob with a real payload so blob_size > 0.
    const ManifestId id_a = publishBlobPart(s, ns, "ref_a", "data.bin", "payload-B92");

    const PartManifest manifest_a = s->readManifest(id_a);
    auto entry_a = s->lookupPath(manifest_a, "data.bin");
    ASSERT_TRUE(entry_a.has_value());
    const uint64_t size_a = entry_a->blob_size;
    EXPECT_NE(size_a, 0u) << "ref A blob_size must be non-zero";
    EXPECT_EQ(size_a, String("payload-B92").size());

    /// Build B: adopt the same leaf, publish as ref_b (no re-upload).
    auto build_b = startBuildFor(s, ns, "ref_b");
    ASSERT_TRUE(entry_a.has_value());
    build_b->adoptEvidence(*entry_a);
    const ManifestId id_b = build_b->stageManifest({*entry_a});
    build_b->precommitAdd(ns, "ref_b", id_b);
    build_b->promote(ns, "ref_b", build_b->buildId(), id_b);

    /// Resolve ref B: the adopted leaf's blob_size must match ref A (round-trip invariant for B92).
    const PartManifest manifest_b = s->readManifest(s->resolveRef(ns, "ref_b")->manifest_id);
    auto entry_b = s->lookupPath(manifest_b, "data.bin");
    ASSERT_TRUE(entry_b.has_value());
    EXPECT_NE(entry_b->blob_size, 0u) << "adopted leaf blob_size must not be 0 (B92)";
    EXPECT_EQ(entry_b->blob_size, size_a) << "adopted-leaf blob_size mismatch (B92 round-trip)";
}

/// ---- Genuinely-obsolete pure-tree-model scenarios (no manifest analog) ----

TEST(CasProtocol, DISABLED_RevalidateAbsentTreeDepRecreates)
{
    GTEST_SKIP() << "Obsolete (tree model). The gate's 'absent tree dep recreated from retained "
                    "payload' behavior has no manifest analog: a part manifest body is staged ONCE by "
                    "stageManifest and promote never re-creates it — an absent/invalid body at promote "
                    "fails closed (ABORTED). The blob-leaf absent-recreate case is covered by putBlob's "
                    "INV-1 re-upload-from-source path, not by the publish gate.";
}

TEST(CasProtocol, DISABLED_AdoptTreeOfReclaimedTreeFailsClosedAtAdoptTime)
{
    GTEST_SKIP() << "Obsolete (tree model). adoptTree's fail-closed observe-at-adopt-time (one HEAD, "
                    "FILE_DOESNT_EXIST on an absent detached tree) has no manifest analog: the manifest "
                    "model's adoptEvidence is deliberately TOKENLESS and performs NO backend call — the "
                    "no-dangle guarantee for an adopted-but-reclaimed leaf is enforced at the promote "
                    "gate (unconditional blob revalidation ⇒ ABORTED), covered by "
                    "NewNamespacePublishSeesRegistryFenceFloor and FreshEvidenceDepWithViewHitIsResolvedByGate.";
}
