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
/// gate is gone; a write is now the four-step flow (EDGE-BEFORE-OBSERVE order):
///   stageManifest(entries) -> precommitAdd(ns, ref, id) -> putBlob(...) -> promote(ns, ref, build_id, id)
/// The fail-closed publish gate that those scenarios exercise now lives in TWO places (Phase A of spec
/// 2026-07-09-cas-writer-gc-simplification):
///   • putBlob: INV-1 condemned-dedup re-upload from the writer's OWN source bytes (never GETs the
///     dying object);
///   • promote: TOKENED leaves (this build putBlob'd them) are EDGE-PROTECTED and NOT re-validated — the
///     precommit closure named them before putBlob observed them, so a condemnation in the
///     putBlob→promote window is doomed (the next fold spares it). promote commits with the tokened
///     blob's token UNCHANGED. Only NON-tokened leaves get the single mandatory presence observation: a
///     tokenless W-EVIDENCE adopt that is condemned-but-present is displaced by a verified copy-forward
///     (the committed ref names a FRESH incarnation); absent, or condemned + no-dep, fails closed
///     (ABORTED). promote refreshes the retire view when the fence is ahead.
/// These scenarios assert the no-dangle / no-loss / fail-closed protocol properties faithfully on that
/// flow. The strong safety assertions are preserved.
///
/// DELETED (Phase A): `RevalidateAbsentTokenedBlobResurrectsFromSource`. Its premise — a putBlob'd
/// (tokened) blob body hand-deleted before the gate, then resurrected — is protocol-unreachable under
/// EDGE-BEFORE-OBSERVE: a tokened leaf under a durable precommit closure cannot be GC-deleted in the
/// putBlob→promote window, and promote no longer re-validates tokened leaves at all. Deleting a
/// putBlob'd body out-of-band is corruption, which is `ca-fsck`'s domain, not the promote gate's.

namespace DB::ErrorCodes
{
extern const int ABORTED;
extern const int FILE_DOESNT_EXIST;
extern const int LOGICAL_ERROR;
}

using namespace DB::Cas;
using DB::Cas::tests::blobEntryFor;
using DB::Cas::tests::condemnMeta;
using DB::Cas::tests::displaceBlobToken;
using DB::Cas::tests::expectThrowsCode;
using DB::Cas::tests::fenceNamespace;
using DB::Cas::tests::idOf;
using DB::Cas::tests::injectRetire;
using DB::Cas::tests::loadMetaForTest;
using DB::Cas::tests::streamingHexOf;
using DB::Cas::tests::u128Of;
using DB::Cas::tests::writeBlobRaw;

namespace
{

StorePtr openStore(const std::shared_ptr<InMemoryBackend> & b)
{
    return Store::open(b, PoolConfig{.pool_prefix = "p", .server_root_id = "test"});
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
    /// Wiring order (EDGE-BEFORE-OBSERVE): stageManifest -> precommitAdd -> putBlob -> promote.
    const ManifestId id = build->stageManifest({blobEntry(path, payload)});
    build->precommitAdd(ns, ref, id);
    build->putBlob(idOf(payload), BlobSource::fromString(payload));
    build->promote(ns, ref, build->buildId(), id);
    return id;
}

/// Read the part's blob back through the full read stack (resolveRef → readManifest → findEntry →
/// locate → ranged GET) and assert it returns `payload`. This is the INV-NO-DANGLE check: every named
/// object resolves and reads.
void assertPartReads(
    const std::shared_ptr<InMemoryBackend> & b, const StorePtr & s,
    const RootNamespace & ns, const String & ref, const String & path, const String & payload)
{
    auto r = s->resolveRef(ns, ref);
    ASSERT_TRUE(r.has_value());

    const PartManifest manifest = s->readManifest(r->manifest_id);
    const auto * entry = findEntry(manifest.entries, path);
    ASSERT_TRUE(entry != nullptr);
    auto loc = s->locate(*entry);
    auto got = b->get(loc.key, Range{loc.offset, loc.length});
    ASSERT_TRUE(got.has_value());
    EXPECT_EQ(got->bytes, payload);
}

}

TEST(CasProtocol, FenceConflictCondemnedTokenedBlobCommitsWithTokenUnchanged)
{
    /// EDGE-BEFORE-OBSERVE (spec 2026-07-09-cas-writer-gc-simplification, Phase A): a blob leaf whose
    /// CURRENT token is condemned at the promote gate, but which THIS build putBlob'd (tokened dep under
    /// the durable precommit closure), is EDGE-PROTECTED — the condemnation is doomed (the next fold spares
    /// it) and promote does NOT re-validate or re-upload the tokened leaf. promote COMMITS with the blob's
    /// token UNCHANGED; the premature condemn is invisible to the client.
    auto b = std::make_shared<InMemoryBackend>();
    auto s = openStore(b);
    const RootNamespace ns{"srv1/tbl"};

    /// Wiring order: stage + precommit (durable edge) BEFORE putBlob observes X (records token t0).
    auto build = startBuildFor(s, ns, "part_1");
    const ManifestId id = build->stageManifest({blobEntry("data.bin", "payload-X")});
    build->precommitAdd(ns, "part_1", id);
    build->putBlob(idOf("payload-X"), BlobSource::fromString("payload-X"));

    const String blob_key = s->layout().blobKey(idOf("payload-X"));
    const Token t0 = b->head(blob_key).token;

    /// GC condemns X at t0 in round 1 and fences the namespace to round 1.
    injectRetire(*b, s->layout(), /*round*/ 1, /*fence_seq*/ 0, /*shard*/ 0,
        {RetiredEntry{.kind = ObjectKind::Blob, .hash = DB::Cas::BlobDigest::fromU128(u128Of("payload-X")), .token = t0, .size = 9}});
    fenceNamespace(*b, s->layout(), ns, s->poolMeta().root_shards, /*round*/ 1);

    /// promote: mutateShard refreshes the view (fence_round 1 > view round 0), but the tokened leaf is
    /// edge-protected — skipped, not re-validated ⇒ commit, token unchanged.
    build->promote(ns, "part_1", build->buildId(), id);

    /// The ref is committed and reads back; the blob still rides t0 (no re-upload).
    assertPartReads(b, s, ns, "part_1", "data.bin", "payload-X");
    EXPECT_EQ(b->head(blob_key).token, t0);
}

TEST(CasProtocol, RevalidateReObservesStaleTokenKeepsWhenUnchanged)
{
    /// A blob dedup-adopted (tokened dep) under the precommit closure; an EMPTY retire set at round 1.
    /// Under EDGE-BEFORE-OBSERVE the tokened leaf is NOT re-observed at the promote gate at all — it is
    /// edge-protected — so promote commits in place with the token UNCHANGED (no HEAD, no rewrite).
    auto b = std::make_shared<InMemoryBackend>();
    auto s = openStore(b);
    const RootNamespace ns{"srv1/tbl"};

    /// X pre-exists out-of-band; the build dedup-adopts it via putBlob (records the current token t0).
    writeBlobRaw(*b, s->layout(), "payload-X", s->poolMeta().blob_header_len, s->poolMeta().pool_id);
    const String blob_key = s->layout().blobKey(idOf("payload-X"));
    const Token t0 = b->head(blob_key).token;

    /// Wiring order: stage + precommit (durable edge) BEFORE the adopting putBlob.
    auto build = startBuildFor(s, ns, "part_1");
    const ManifestId id = build->stageManifest({blobEntry("data.bin", "payload-X")});
    build->precommitAdd(ns, "part_1", id);
    build->putBlob(idOf("payload-X"), BlobSource::fromString("payload-X"));   /// dedup → adopts t0

    /// GC advanced the round to 1 with an EMPTY retired set; fence to 1. X is NOT condemned and its
    /// token is unchanged.
    injectRetire(*b, s->layout(), /*round*/ 1, /*fence_seq*/ 0, /*shard*/ 0, {});
    fenceNamespace(*b, s->layout(), ns, s->poolMeta().root_shards, /*round*/ 1);

    /// promote: the tokened leaf is edge-protected (not re-observed) ⇒ commit in place (KEEP).
    build->promote(ns, "part_1", build->buildId(), id);

    assertPartReads(b, s, ns, "part_1", "data.bin", "payload-X");
    /// No rewrite happened — the tokened leaf was never touched, so its token stays at t0.
    EXPECT_EQ(b->head(blob_key).token, t0);
}

TEST(CasProtocol, RevalidateReObservesStaleTokenAdoptsWhenDisplaced)
{
    /// A blob displaced out-of-band to a fresh live token t1 before promote. Phase-A contract: the leaf is
    /// TOKENED (putBlob-adopted), so promote SKIPS it entirely (edge-protected — EDGE-BEFORE-OBSERVE); no
    /// re-HEAD happens. The commit still rides the displaced object correctly because the manifest names
    /// the HASH, not a token — this is the black-box "displaced object still reads by content key" check.
    auto b = std::make_shared<InMemoryBackend>();
    auto s = openStore(b);
    const RootNamespace ns{"srv1/tbl"};

    writeBlobRaw(*b, s->layout(), "payload-X", s->poolMeta().blob_header_len, s->poolMeta().pool_id);
    const String blob_key = s->layout().blobKey(idOf("payload-X"));
    const Token t0 = b->head(blob_key).token;

    auto build = startBuildFor(s, ns, "part_1");
    /// Wiring order (EDGE-BEFORE-OBSERVE): stageManifest -> precommitAdd -> putBlob.
    const ManifestId id = build->stageManifest({blobEntry("data.bin", "payload-X")});
    build->precommitAdd(ns, "part_1", id);
    build->putBlob(idOf("payload-X"), BlobSource::fromString("payload-X"));   /// dedup → adopts t0

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
    /// hash IS condemned. The build putBlob-adopts t0 (tokened dep), so promote does not re-observe it
    /// (edge-protected) and commits in place: the blob keeps t0 (no upload, no displacement). The phantom
    /// condemnation is for a different incarnation and never touches t0.
    auto b = std::make_shared<InMemoryBackend>();
    const RootNamespace ns{"srv1/tbl"};

    DB::Cas::Layout layout("p");
    {
        auto s0 = Store::open(b, PoolConfig{.pool_prefix = "p", .server_root_id = "test"});
        writeBlobRaw(*b, s0->layout(), "payload-X", s0->poolMeta().blob_header_len, s0->poolMeta().pool_id);
    }
    const String blob_key = layout.blobKey(idOf("payload-X"));
    const Token t0 = b->head(blob_key).token;
    const Token t_other{"emulated-phantom", DB::Cas::TokenType::Emulated};
    ASSERT_NE(t_other, t0);

    injectRetire(*b, layout, /*round*/ 1, /*fence_seq*/ 0, /*shard*/ 0,
        {RetiredEntry{.kind = ObjectKind::Blob, .hash = DB::Cas::BlobDigest::fromU128(u128Of("payload-X")), .token = t_other, .size = 9}});
    /// Fence to round 1 BEFORE opening the store, so the store's open-time refresh lands the view at
    /// round 1 already populated.
    fenceNamespace(*b, layout, ns, /*n_shards*/ 8, /*round*/ 1);

    auto s = Store::open(b, PoolConfig{.pool_prefix = "p", .server_root_id = "test"});   /// open-time refresh ⇒ view round 1
    /// Wiring order: stage + precommit (durable edge) BEFORE the adopting putBlob.
    auto build = startBuildFor(s, ns, "part_1");
    const ManifestId id = build->stageManifest({blobEntry("data.bin", "payload-X")});
    build->precommitAdd(ns, "part_1", id);
    build->putBlob(idOf("payload-X"), BlobSource::fromString("payload-X"));   /// dedup → adopts t0

    /// promote: the tokened leaf is edge-protected (not re-observed) ⇒ commit. Lands. t0 untouched.
    build->promote(ns, "part_1", build->buildId(), id);

    assertPartReads(b, s, ns, "part_1", "data.bin", "payload-X");

    /// The object was NOT displaced — it STAYS at t0 (no re-upload, only re-validated).
    EXPECT_EQ(b->head(blob_key).token, t0);
}

/// (DELETED, Phase A) RevalidateAbsentTokenedBlobResurrectsFromSource — see the file-header note: a
/// hand-deleted putBlob'd (tokened) body is protocol-unreachable under EDGE-BEFORE-OBSERVE (a tokened
/// leaf under a durable precommit closure cannot be GC-deleted in the putBlob→promote window, and promote
/// no longer re-validates tokened leaves). Out-of-band body deletion is `ca-fsck`'s domain.

TEST(CasProtocol, EvidenceHitCondemnedPresentBlobCopiesForwardInClosure)
{
    /// W-EVIDENCE (tokenless adopted dep) on a blob X whose hash is condemned-but-PRESENT: promote copies
    /// X forward in-closure to a fresh live incarnation and SUCCEEDS. X here has NO independent committed
    /// owner (a lone raw blob) — the accepted revive-for-the-live-about-to-commit-owner window. A
    /// non-tokened leaf is the ONLY leaf promote still observes (tokened leaves are edge-protected).
    /// v3 (Task 4, spec §meta-protocols): the condemned decision is a per-hash META POINT-READ, not the
    /// writer's RetireView — condemning the meta is immediately visible to K3, no view install needed.
    /// Before the copy-forward fix this aborted (INV-1 fail-closed), a liveness brick on the DETACH/freeze
    /// adopt path.
    auto b = std::make_shared<InMemoryBackend>();
    auto s = openStore(b);
    const RootNamespace ns{"srv1/tbl"};

    /// X pre-exists with token t0 — minted with the POOL streaming-hash id so the copy-forward verifier
    /// accepts its payload; the manifest names it as a tokenless adopted leaf.
    const String hex = streamingHexOf("payload-X");
    {
        auto seed = s->startBuild({});
        seed->putBlob(BlobId{hex}, BlobSource::fromString("payload-X"));
    }
    const String blob_key = s->layout().blobKey(BlobId{hex});
    const Token t0 = b->head(blob_key).token;

    auto build = startBuildFor(s, ns, "part_1");
    ManifestEntry entry = blobEntry("data.bin", "payload-X");
    entry.ref = DB::Cas::BlobRef{DB::Cas::BlobHashAlgo::CityHash128, DB::Cas::BlobDigest::fromU128(hexToU128(hex))};   /// streaming-convention id (matches the minted blob)
    build->adoptEvidence(entry);   /// tokenless W-EVIDENCE dep on X (no HEAD, no upload)
    const ManifestId id = build->stageManifest({entry});
    build->precommitAdd(ns, "part_1", id);

    /// GC condemns X's hash in round 1 via the meta.
    condemnMeta(*b, s->layout(), hexToU128(hex), /*condemn_round*/ 1);

    /// promote: the K3 gate sees X condemned (meta point-read) ⇒ copy-forward ⇒ commit.
    EXPECT_NO_THROW(build->promote(ns, "part_1", build->buildId(), id));

    /// The committed ref stands over a FRESH incarnation; the condemned token t0 is never bound.
    EXPECT_TRUE(s->resolveRef(ns, "part_1").has_value());
    EXPECT_NE(b->head(blob_key).token, t0);

    /// The copy-forward flips the meta back to Clean (Task 4 step 4).
    const auto lm_after = loadMetaForTest(*b, s->layout(), hexToU128(hex));
    ASSERT_TRUE(lm_after.has_value());
    EXPECT_EQ(lm_after->meta.state, MetaState::Clean);
}

TEST(CasProtocol, WedgedHeartbeatCondemnedTokenedBlobCommitsWithTokenUnchanged)
{
    /// A build whose watermark never renews finds its OWN putBlob'd upload condemned by full GC while its
    /// precommit is STILL the live owner (this setup injects only the retire set + fence, no owner-removal
    /// — the false-positive-freeze window BEFORE any GC reclaim). The tokened leaf is EDGE-PROTECTED: the
    /// precommit closure named it before putBlob observed it, so the condemnation is doomed and promote
    /// does NOT re-validate it — promote COMMITS with the token UNCHANGED, closing the window invisibly.
    /// The genuine dead-build case (precommit reclaimed ⇒ owner check aborts, NO re-upload) is covered
    /// separately by CaWiringResurrect.PromoteAbandonedPrecommitAbortsWithoutResurrect.
    auto b = std::make_shared<InMemoryBackend>();
    auto s = openStore(b);
    const RootNamespace ns{"srv1/tbl"};

    /// Wiring order: stage + precommit (durable edge) BEFORE putBlob observes X.
    auto build = startBuildFor(s, ns, "part_1");
    const ManifestId id = build->stageManifest({blobEntry("data.bin", "payload-X")});
    build->precommitAdd(ns, "part_1", id);
    build->putBlob(idOf("payload-X"), BlobSource::fromString("payload-X"));

    const String blob_key = s->layout().blobKey(idOf("payload-X"));
    const Token t0 = b->head(blob_key).token;

    /// Full GC condemned the build's OWN upload.
    injectRetire(*b, s->layout(), /*round*/ 1, /*fence_seq*/ 0, /*shard*/ 0,
        {RetiredEntry{.kind = ObjectKind::Blob, .hash = DB::Cas::BlobDigest::fromU128(u128Of("payload-X")), .token = t0, .size = 9}});
    fenceNamespace(*b, s->layout(), ns, s->poolMeta().root_shards, /*round*/ 1);

    /// promote: the tokened leaf is edge-protected — skipped, not re-validated ⇒ commit, token unchanged.
    build->promote(ns, "part_1", build->buildId(), id);
    assertPartReads(b, s, ns, "part_1", "data.bin", "payload-X");
    EXPECT_EQ(b->head(blob_key).token, t0);
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
    /// (Ported from the former ResurrectLosesRace scenario.) A blob displaced to a LIVE t1 (while its old
    /// t0 is condemned for a now-defunct incarnation) is SAFE to commit: the committed manifest names a
    /// blob HASH, the live t1 incarnation backs it, and GC's exact-token delete of t0 only TokenMismatches.
    /// Phase-A contract: the leaf is TOKENED, so promote does not re-HEAD it at all (edge-protected —
    /// EDGE-BEFORE-OBSERVE); the commit is correct by content addressing, not by revalidation. The old
    /// conservative ABORTED has no manifest-model analog.
    auto b = std::make_shared<InMemoryBackend>();
    auto s = openStore(b);
    const RootNamespace ns{"srv1/tbl"};

    writeBlobRaw(*b, s->layout(), "payload-X", s->poolMeta().blob_header_len, s->poolMeta().pool_id);
    const String blob_key = s->layout().blobKey(idOf("payload-X"));
    const Token t0 = b->head(blob_key).token;

    auto build = startBuildFor(s, ns, "part_1");
    /// Wiring order (EDGE-BEFORE-OBSERVE): stageManifest -> precommitAdd -> putBlob.
    const ManifestId id = build->stageManifest({blobEntry("data.bin", "payload-X")});
    build->precommitAdd(ns, "part_1", id);
    build->putBlob(idOf("payload-X"), BlobSource::fromString("payload-X"));   /// dedup → adopts t0

    /// Another writer displaces X to t1 (uncondemned) before our gate runs.
    const Token t1 = displaceBlobToken(*b, s->layout(), idOf("payload-X"));
    ASSERT_NE(t1, t0);

    /// The view still condemns the OLD t0 at round 1, fenced.
    injectRetire(*b, s->layout(), /*round*/ 1, /*fence_seq*/ 0, /*shard*/ 0,
        {RetiredEntry{.kind = ObjectKind::Blob, .hash = DB::Cas::BlobDigest::fromU128(u128Of("payload-X")), .token = t0, .size = 9}});
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

TEST(CasProtocol, NewNamespacePublishGatedByShardFenceFloor)
{
    /// Regression test: build B adopts a blob, the ack-floor GC pipeline retires + deletes it, then B
    /// publishes into a fresh namespace. The fence machinery is gone; the dangle is prevented by promote's
    /// UNCONDITIONAL blob revalidation (step 3): it re-HEADs every blob leaf and, seeing the adopted blob
    /// absent (GC deleted it) — or its condemned token in the ack-fresh retire view — fails closed (ABORTED).
    /// The gate does not depend on any registry/shard fence floor; it is the revalidation path.
    auto b = std::make_shared<InMemoryBackend>();
    auto s = openStore(b);

    /// 1. part_1 → a blob in namespace A, through the real Build.
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

    /// 3. drop part_1 from A; the ack-floor GC pipeline retires the blob at t0 and deletes it. build_a
    /// finished, so advancing the watermark floor condemns the blob. Drive rounds advancing the store's
    /// own mount ack after each (so the floor graduates the condemned entry and the delete lands).
    s->dropRef(ns_a, "part_1");
    build_a.reset();
    s->renewWatermarkOnce();
    Gc gc(s, hexToU128("00000000000000000000000000000001"));
    for (size_t r = 0; r < 16; ++r)
    {
        const RoundReport rep = gc.runRegularRound();
        s->renewWatermarkOnce();
        if (!b->head(blob_key).exists)
            break;
    }
    /// The blob (unreachable) was deleted at t0.
    EXPECT_FALSE(b->head(blob_key).exists);

    /// 4. build B publishes into a BRAND-NEW namespace. The unconditional blob revalidation catches the
    /// now-absent (condemned) blob ⇒ ABORTED. It must NEVER land — landing would write a manifest naming a
    /// deleted blob (the dangle).
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
    /// A TOKENLESS (W-EVIDENCE) leaf whose blob is condemned by hash MUST still be caught by promote's
    /// unconditional blob revalidation. Since the copy-forward pre-pass
    /// (spec 2026-07-02-cas-copy-forward-condemned-evidence.md) "caught" no longer means ABORTED: a
    /// condemned-but-PRESENT incarnation is displaced by a verified copy-forward and promote SUCCEEDS.
    /// The underlying invariant is unchanged and asserted here: the listed token t0 is never bound — the
    /// committed ref stands over a FRESH incarnation.
    /// v3 (Task 4, spec §meta-protocols): the condemned decision is a per-hash META POINT-READ, not the
    /// writer's RetireView, so there is no "view round"/"fence advance" to track any more (the test name
    /// is legacy) — condemning the meta is immediately, unconditionally visible to K3.
    auto b = std::make_shared<InMemoryBackend>();

    DB::Cas::Layout layout("p");
    const String hex = streamingHexOf("payload-fresh-ev");
    {
        auto s0 = openStore(b);
        auto build0 = s0->startBuild({});
        build0->putBlob(BlobId{hex}, BlobSource::fromString("payload-fresh-ev"));
    }
    const String blob_key = layout.blobKey(BlobId{hex});
    const Token t0 = b->head(blob_key).token;
    condemnMeta(*b, layout, hexToU128(hex), /*condemn_round*/ 1);

    auto s = openStore(b);

    const RootNamespace ns{"srv1/tbl"};
    auto build = startBuildFor(s, ns, "part_1");
    /// adoptEvidence records a TOKENLESS dep.
    ManifestEntry entry = blobEntry("data.bin", "payload-fresh-ev");
    entry.ref = DB::Cas::BlobRef{DB::Cas::BlobHashAlgo::CityHash128, DB::Cas::BlobDigest::fromU128(hexToU128(hex))};   /// streaming-convention id (matches the minted blob)
    build->adoptEvidence(entry);
    const ManifestId id = build->stageManifest({entry});
    build->precommitAdd(ns, "part_1", id);

    /// promote's blob revalidation is unconditional — HEAD t0 condemned (meta point-read) ⇒ verified
    /// copy-forward displaces it; promote succeeds.
    EXPECT_NO_THROW(build->promote(ns, "part_1", build->buildId(), id));

    EXPECT_NE(b->head(blob_key).token, t0) << "the listed token must never be bound — fresh incarnation only";
    EXPECT_TRUE(s->resolveRef(ns, "part_1").has_value());
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
    const auto * entry_a = findEntry(manifest_a.entries, "data.bin");
    ASSERT_TRUE(entry_a != nullptr);
    const uint64_t size_a = entry_a->blob_size;
    EXPECT_NE(size_a, 0u) << "ref A blob_size must be non-zero";
    EXPECT_EQ(size_a, String("payload-B92").size());

    /// Build B: adopt the same leaf, publish as ref_b (no re-upload).
    auto build_b = startBuildFor(s, ns, "ref_b");
    ASSERT_TRUE(entry_a != nullptr);
    build_b->adoptEvidence(*entry_a);
    const ManifestId id_b = build_b->stageManifest({*entry_a});
    build_b->precommitAdd(ns, "ref_b", id_b);
    build_b->promote(ns, "ref_b", build_b->buildId(), id_b);

    /// Resolve ref B: the adopted leaf's blob_size must match ref A (round-trip invariant for B92).
    const PartManifest manifest_b = s->readManifest(s->resolveRef(ns, "ref_b")->manifest_id);
    const auto * entry_b = findEntry(manifest_b.entries, "data.bin");
    ASSERT_TRUE(entry_b != nullptr);
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
                    "NewNamespacePublishGatedByShardFenceFloor and FreshEvidenceDepWithViewHitIsResolvedByGate.";
}
