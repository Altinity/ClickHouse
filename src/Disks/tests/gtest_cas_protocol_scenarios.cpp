#include <gtest/gtest.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasBuild.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasStore.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasInMemoryBackend.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasEnvelope.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasGc.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasGcFormats.h>
#include <Disks/tests/cas_test_helpers.h>
#include <Common/Exception.h>

/// Task 13 — the adversarial publish gate (the protocol climax of M-C2). Every scenario injects GC
/// state through the on-storage GC surface ONLY (injectRetire writes gc/state + a retired set;
/// fenceNamespace raises fence_round — exactly the writes a GC leader performs), then publishes. The
/// Store refreshes its retire view at open; Build::publish refreshes again on a fence-advanced conflict
/// and then runs `checkAndResolveDeps` — the merged W-REVALIDATE + W-EVIDENCE + condemned-token pass
/// (B190 Task 3; replaces the former separate `revalidateDeps` + `gateCheckDeps` calls).
///
/// The standard pattern: build + upload (records tokens at view round 0), THEN inject retire + fence,
/// THEN publish. The first CAS attempt reads the fenced manifest, sees view.round() < fence_round,
/// refreshes, and runs `checkAndResolveDeps`.

namespace DB::ErrorCodes
{
extern const int ABORTED;
extern const int FILE_DOESNT_EXIST;
extern const int LOGICAL_ERROR;
}

using namespace DB::Cas;
using DB::Cas::tests::displaceBlobToken;
using DB::Cas::tests::expectThrowsCode;
using DB::Cas::tests::fenceNamespace;
using DB::Cas::tests::idOf;
using DB::Cas::tests::injectRetire;
using DB::Cas::tests::u128Of;
using DB::Cas::tests::writeBlobRaw;
using DB::Cas::tests::writeTreeRaw;

namespace
{

StorePtr openStore(const std::shared_ptr<InMemoryBackend> & b)
{
    return Store::open(b, PoolConfig{.pool_prefix = "p"});
}

/// A single-blob tree entry naming `payload` at `name` (the entry the part's tree carries).
TreeEntry blobEntry(const String & name, const String & payload)
{
    TreeEntry e;
    e.name = name;
    e.placement = Placement::Blob;
    e.file_hash = u128Of(payload);
    e.file_size = payload.size();
    return e;
}

/// Read the part's blob back through the full read stack (resolve → readTree → locate → ranged GET)
/// and assert it returns `payload`. This is the INV-NO-DANGLE check: every named object resolves.
void assertPartReads(
    const std::shared_ptr<InMemoryBackend> & b, const StorePtr & s,
    const RootNamespace & ns, const String & ref, const TreeId & tree, const String & payload)
{
    auto r = s->resolveRef(ns, ref);
    ASSERT_TRUE(r.has_value());
    EXPECT_EQ(r->tree_id, tree);

    auto entries = s->readTree(tree);
    ASSERT_EQ(entries.size(), 1u);
    auto loc = s->locate(entries[0]);
    auto got = b->get(loc.key, Range{loc.offset, loc.length});
    ASSERT_TRUE(got.has_value());
    EXPECT_EQ(got->bytes, payload);
}

}

TEST(CasProtocol, FenceConflictCondemnedBlobDepAbortsRetryable)
{
    /// B190 INV-1: a blob dep whose OWN token is condemned at the gate has no source bytes available
    /// (they were not retained after putBlob returned). The gate MUST throw ABORTED (retryable), never
    /// GET the dying object. Resurrection-by-GET was the old behavior; INV-1 forbids it.
    ///
    /// Spec §5: "blob or adopted-tree with no source → retryable ABORTED". The caller retries the full
    /// build from scratch (re-putBlob from its own source bytes).
    auto b = std::make_shared<InMemoryBackend>();
    auto s = openStore(b);
    const RootNamespace ns{"srv1/tbl"};

    /// Build: own upload of X (records token t0 at view round 0), then tree T.
    auto build = s->startBuild({});
    build->putBlob(idOf("payload-X"), BlobSource::fromString("payload-X"));
    const TreeId tree = build->putTree({blobEntry("data.bin", "payload-X")});

    const String blob_key = s->layout().blobKey(idOf("payload-X"));
    const Token t0 = b->head(blob_key).token;

    /// GC condemns X at t0 in round 1 and fences the namespace to round 1.
    injectRetire(*b, s->layout(), /*round*/ 1, /*fence_seq*/ 0, /*shard*/ 0,
        {RetiredEntry{.kind = ObjectKind::Blob, .hash = u128Of("payload-X"), .token = t0, .size = 9}});
    fenceNamespace(*b, s->layout(), ns, s->poolMeta().root_shards, /*round*/ 1);

    /// First CAS reads fence_round=1 > view round 0 ⇒ refresh ⇒ `checkAndResolveDeps`: X condemned at t0
    /// ⇒ Case(a): dep's own token condemned, no retained payload ⇒ ABORTED (INV-1; caller must retry).
    expectThrowsCode(DB::ErrorCodes::ABORTED, [&] { build->publish(ns, "part_1", tree, RefPayload{}); });

    /// The condemned object is still at t0 — nothing was written.
    EXPECT_EQ(b->head(blob_key).token, t0);
    /// No ref was published.
    EXPECT_FALSE(s->resolveRef(ns, "part_1").has_value());
}

TEST(CasProtocol, RevalidateReObservesStaleTokenKeepsWhenUnchanged)
{
    /// The F1 IN-FLIGHT DISJUNCTION keep-branch.
    auto b = std::make_shared<InMemoryBackend>();
    auto s = openStore(b);
    const RootNamespace ns{"srv1/tbl"};

    /// X pre-exists out-of-band; the build dedup-adopts it via putBlob (records the current token t0).
    writeBlobRaw(*b, s->layout(), "payload-X", s->poolMeta().blob_header_len, s->poolMeta().pool_id);
    const String blob_key = s->layout().blobKey(idOf("payload-X"));
    const Token t0 = b->head(blob_key).token;

    auto build = s->startBuild({});
    build->putBlob(idOf("payload-X"), BlobSource::fromString("payload-X"));   /// dedup → adopts t0 (TOKENED dep)
    const TreeId tree = build->putTree({blobEntry("data.bin", "payload-X")});

    /// GC advanced the round to 1 but dropped all entries on outcomes ⇒ EMPTY retired set; fence to 1.
    /// X is NOT condemned in the refreshed view and its token is unchanged.
    injectRetire(*b, s->layout(), /*round*/ 1, /*fence_seq*/ 0, /*shard*/ 0, {});
    fenceNamespace(*b, s->layout(), ns, s->poolMeta().root_shards, /*round*/ 1);

    /// refresh ⇒ X stale (observed round 0 < 1), no hit ⇒ re-observe ⇒ current == t0 ⇒ KEEP.
    build->publish(ns, "part_1", tree, RefPayload{});

    assertPartReads(b, s, ns, "part_1", tree, "payload-X");
    /// No rewrite happened — the keep-branch left the object's token at t0.
    EXPECT_EQ(b->head(blob_key).token, t0);
}

TEST(CasProtocol, RevalidateReObservesStaleTokenAdoptsWhenDisplaced)
{
    auto b = std::make_shared<InMemoryBackend>();
    auto s = openStore(b);
    const RootNamespace ns{"srv1/tbl"};

    writeBlobRaw(*b, s->layout(), "payload-X", s->poolMeta().blob_header_len, s->poolMeta().pool_id);
    const String blob_key = s->layout().blobKey(idOf("payload-X"));
    const Token t0 = b->head(blob_key).token;

    auto build = s->startBuild({});
    build->putBlob(idOf("payload-X"), BlobSource::fromString("payload-X"));   /// dedup → adopts t0 at round 0
    const TreeId tree = build->putTree({blobEntry("data.bin", "payload-X")});

    /// Another writer displaces X out-of-band ⇒ a new current token t1 (same payload, fresh tag).
    const Token t1 = displaceBlobToken(*b, s->layout(), idOf("payload-X"));
    EXPECT_NE(t1, t0);

    /// GC advanced to round 1 with an EMPTY retired set; fence to 1.
    injectRetire(*b, s->layout(), /*round*/ 1, /*fence_seq*/ 0, /*shard*/ 0, {});
    fenceNamespace(*b, s->layout(), ns, s->poolMeta().root_shards, /*round*/ 1);

    /// refresh ⇒ X stale, no hit ⇒ re-observe ⇒ current t1 != observed t0 ⇒ observeAndAdmit ⇒ adopt t1.
    build->publish(ns, "part_1", tree, RefPayload{});
    assertPartReads(b, s, ns, "part_1", tree, "payload-X");
    EXPECT_EQ(b->head(blob_key).token, t1);

    /// Black-box proof the dep now rides t1 (not the stale t0): re-publish the same tree into a SECOND
    /// namespace with NO new GC injection — no fence advance, so no refresh and no gate displacement.
    /// The dep is already token-bearing at t1; nothing is re-uploaded and the object stays at t1.
    /// (Condemning hashX at ANY token now forces a resurrect on a fence-advanced publish — view hits
    /// are by HASH, not by our exact token — so the dep's token cannot be probed via a hash-condemn.)
    build->publish(RootNamespace{"srv1/tbl/copy"}, "part_2", tree, RefPayload{});
    EXPECT_EQ(b->head(blob_key).token, t1);
    assertPartReads(b, s, RootNamespace{"srv1/tbl/copy"}, "part_2", tree, "payload-X");

    /// Independent discriminator that the dep rides t1, not the stale t0: t0 is DEAD. A deleteExact
    /// against t0 must TokenMismatch (INV-NO-RETURN — t0 was displaced and can never be current
    /// again), proving the adoption was to t1 and did not silently retain t0.
    EXPECT_EQ(b->deleteExact(blob_key, t0).kind, DeleteOutcome::Kind::TokenMismatch);
}

TEST(CasProtocol, RevalidateAdoptsLiveTokenWhenOnlyPhantomCondemnedAtDifferentToken)
{
    /// B190 INV-1 Case(b): a token-bearing dep whose OWN token t0 is LIVE (not condemned), but a
    /// DIFFERENT phantom token t_other for the same hash IS condemned. `checkAndResolveDeps`: hash hit,
    /// but isCondemnedToken(t0)=false ⇒ Case(b): dep's own token is live ⇒ re-observe (observeAndAdmit,
    /// HEAD-only, no GET) ⇒ current still t0 ⇒ adopt t0 (keep in place, no upload). The old resurrect
    /// path would have re-uploaded from GET(condemned_object), which is forbidden by INV-1.
    ///
    /// Discriminating contract: publish SUCCEEDS and the object STAYS at t0 (no displacement).
    /// The old test expected EXPECT_NE(t_after, t0) because the old resurrect replaced the object;
    /// the B190 Case(b) path does NOT re-upload — it merely re-adopts the still-live t0.
    auto b = std::make_shared<InMemoryBackend>();
    const RootNamespace ns{"srv1/tbl"};

    /// X pre-exists at its current token t0; another incarnation token t_other is condemned by hash at
    /// round 1 (a different incarnation's entry — the GC surface only needs the (hash, token) pair).
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
    /// round 1 already populated — and the build's dep is then recorded at round 1 (non-stale).
    fenceNamespace(*b, layout, ns, /*n_shards*/ 8, /*round*/ 1);

    auto s = Store::open(b, PoolConfig{.pool_prefix = "p"});   /// open-time refresh ⇒ view round 1
    auto build = s->startBuild({});
    /// putBlob dedup-adopts t0: current t0 is NOT in the condemned set {t_other} ⇒ ADOPT t0, dep
    /// recorded at observed_view_round = 1 (the current view round) ⇒ NON-STALE at publish time.
    build->putBlob(idOf("payload-X"), BlobSource::fromString("payload-X"));   /// dedup → adopts t0
    const TreeId tree = build->putTree({blobEntry("data.bin", "payload-X")});

    /// publish: view round 1 == fence_round 1 ⇒ NO fence advance ⇒ no refresh before the gate.
    /// `checkAndResolveDeps`: hash hit (t_other condemned) but dep's own t0 is NOT condemned ⇒ Case(b):
    /// observeAndAdmit (HEAD-only) ⇒ current t0 is live ⇒ re-adopt t0. Publish lands.
    build->publish(ns, "part_1", tree, RefPayload{});

    assertPartReads(b, s, ns, "part_1", tree, "payload-X");

    /// B190 Case(b): the object was NOT displaced — it STAYS at t0 (no re-upload, just re-adopted).
    /// This is the discriminator vs the old resurrect: the old code replaced the object with a fresh
    /// incarnation (EXPECT_NE); the new path merely re-adopts the live t0 (EXPECT_EQ).
    const Token t_after = b->head(blob_key).token;
    EXPECT_EQ(t_after, t0);
}

TEST(CasProtocol, RevalidateAbsentBlobDepAbortsRetryable)
{
    auto b = std::make_shared<InMemoryBackend>();
    auto s = openStore(b);
    const RootNamespace ns{"srv1/tbl"};

    writeBlobRaw(*b, s->layout(), "payload-X", s->poolMeta().blob_header_len, s->poolMeta().pool_id);
    const String blob_key = s->layout().blobKey(idOf("payload-X"));

    auto build = s->startBuild({});
    build->putBlob(idOf("payload-X"), BlobSource::fromString("payload-X"));   /// dedup → adopts token at round 0
    const TreeId tree = build->putTree({blobEntry("data.bin", "payload-X")});

    /// A landed GC delete whose retire entry already dropped: delete X raw by its current token.
    const Token cur = b->head(blob_key).token;
    ASSERT_EQ(b->deleteExact(blob_key, cur).kind, DeleteOutcome::Kind::Deleted);

    injectRetire(*b, s->layout(), /*round*/ 1, /*fence_seq*/ 0, /*shard*/ 0, {});
    fenceNamespace(*b, s->layout(), ns, s->poolMeta().root_shards, /*round*/ 1);

    /// refresh ⇒ X stale, no hit ⇒ re-observe ⇒ absent ⇒ blob payload not retained ⇒ ABORTED (retryable).
    expectThrowsCode(DB::ErrorCodes::ABORTED, [&] { build->publish(ns, "part_1", tree, RefPayload{}); });
}

TEST(CasProtocol, RevalidateAbsentTreeDepRecreates)
{
    auto b = std::make_shared<InMemoryBackend>();
    auto s = openStore(b);
    const RootNamespace ns{"srv1/tbl"};

    auto build = s->startBuild({});
    build->putBlob(idOf("payload-X"), BlobSource::fromString("payload-X"));
    const TreeId tree = build->putTree({blobEntry("data.bin", "payload-X")});   /// retained payload

    /// Delete the tree object raw (its retire entry already dropped). The blob X is untouched.
    const String tree_key = s->layout().treeKey(tree);
    const Token tree_tok = b->head(tree_key).token;
    ASSERT_EQ(b->deleteExact(tree_key, tree_tok).kind, DeleteOutcome::Kind::Deleted);

    injectRetire(*b, s->layout(), /*round*/ 1, /*fence_seq*/ 0, /*shard*/ 0, {});
    fenceNamespace(*b, s->layout(), ns, s->poolMeta().root_shards, /*round*/ 1);

    /// refresh ⇒ T stale, no hit ⇒ re-observe ⇒ absent ⇒ re-create from the retained payload ⇒ lands.
    build->publish(ns, "part_1", tree, RefPayload{});

    /// The tree object is back (fresh incarnation) and the part reads.
    EXPECT_TRUE(b->head(tree_key).exists);
    EXPECT_NE(b->head(tree_key).token, tree_tok);
    assertPartReads(b, s, ns, "part_1", tree, "payload-X");
}

TEST(CasProtocol, EvidenceHitCondemnedBlobAbortsRetryable)
{
    /// B190 INV-1: W-EVIDENCE (tokenless dep) on blob X — `checkAndResolveDeps`: view hit by hash ⇒
    /// observeAndAdmit ⇒ HEAD ⇒ current t0 condemned ⇒ ABORTED (blob has no retained source bytes;
    /// INV-1 forbids GET). The old behavior was resurrect-by-GET, which is now forbidden.
    ///
    /// Spec §5: "blob or adopted-tree with no source → retryable ABORTED". The caller retries the full
    /// build from scratch (re-uploads from its own source).
    auto b = std::make_shared<InMemoryBackend>();
    auto s = openStore(b);
    const RootNamespace ns{"srv1/tbl"};

    /// Source tree S names blob X; X pre-exists with token t0.
    writeBlobRaw(*b, s->layout(), "payload-X", s->poolMeta().blob_header_len, s->poolMeta().pool_id);
    const String blob_key = s->layout().blobKey(idOf("payload-X"));
    const Token t0 = b->head(blob_key).token;
    const TreeId source = writeTreeRaw(*b, s->layout(), {blobEntry("data.bin", "payload-X")}, s->poolMeta().pool_id);

    auto build = s->startBuild({});
    const TreeEntry adopted = build->adoptFromTree(source, "data.bin");   /// tokenless dep on X
    const TreeId tree = build->putTree({adopted});

    /// GC condemns X at t0 in round 1 + fence.
    injectRetire(*b, s->layout(), /*round*/ 1, /*fence_seq*/ 0, /*shard*/ 0,
        {RetiredEntry{.kind = ObjectKind::Blob, .hash = u128Of("payload-X"), .token = t0, .size = 9}});
    fenceNamespace(*b, s->layout(), ns, s->poolMeta().root_shards, /*round*/ 1);

    /// refresh ⇒ `checkAndResolveDeps`: tokenless X has a view hit (condemned at t0) ⇒
    /// observeAndAdmit ⇒ HEAD ⇒ current t0 condemned ⇒ ABORTED (INV-1; no source bytes to re-upload).
    expectThrowsCode(DB::ErrorCodes::ABORTED, [&] { build->publish(ns, "part_1", tree, RefPayload{}); });

    /// The condemned object stays at t0 — nothing was written.
    EXPECT_EQ(b->head(blob_key).token, t0);
    /// No ref was published.
    EXPECT_FALSE(s->resolveRef(ns, "part_1").has_value());
}

TEST(CasProtocol, WedgedHeartbeatCondemnedBlobDepAbortsRetryable)
{
    /// B190 INV-1: a build whose heartbeat never renews finds its OWN uploads condemned by full GC;
    /// its publish refreshes, sees them condemned, but has NO retained source bytes for the blob ⇒
    /// ABORTED (INV-1 forbids GET on the condemned object). The caller must retry from scratch,
    /// re-uploading from its own source bytes. The old "self-heal resurrect" path used GET, which
    /// is now forbidden.
    ///
    /// Spec §5: "blob or adopted-tree with no source → retryable ABORTED". The precommit/two-phase
    /// commit redesign (B171+) removes this window for normal builds: the precommit fence ensures GC
    /// cannot condemn the build's uploads while the build is in-flight. This test validates the
    /// correct ABORTED behavior for the degenerate case where a build skips precommit.
    auto b = std::make_shared<InMemoryBackend>();
    auto s = openStore(b);
    const RootNamespace ns{"srv1/tbl"};

    auto build = s->startBuild({});   /// background_heartbeats=false by config; never renewed
    build->putBlob(idOf("payload-X"), BlobSource::fromString("payload-X"));
    const TreeId tree = build->putTree({blobEntry("data.bin", "payload-X")});

    const String blob_key = s->layout().blobKey(idOf("payload-X"));
    const Token t0 = b->head(blob_key).token;

    /// Full GC condemned the build's OWN upload.
    injectRetire(*b, s->layout(), /*round*/ 1, /*fence_seq*/ 0, /*shard*/ 0,
        {RetiredEntry{.kind = ObjectKind::Blob, .hash = u128Of("payload-X"), .token = t0, .size = 9}});
    fenceNamespace(*b, s->layout(), ns, s->poolMeta().root_shards, /*round*/ 1);

    /// refresh ⇒ `checkAndResolveDeps`: X condemned at dep's own t0 ⇒ Case(a): no retained payload ⇒ ABORTED.
    expectThrowsCode(DB::ErrorCodes::ABORTED, [&] { build->publish(ns, "part_1", tree, RefPayload{}); });

    /// The condemned object stays at t0 — nothing was written.
    EXPECT_EQ(b->head(blob_key).token, t0);
    /// No ref was published.
    EXPECT_FALSE(s->resolveRef(ns, "part_1").has_value());
}

TEST(CasProtocol, AbandonLeavesDebrisAndDropsHeartbeat)
{
    auto b = std::make_shared<InMemoryBackend>();
    auto s = openStore(b);
    const RootNamespace ns{"srv1/tbl"};

    auto build = s->startBuild({});
    auto blob = build->putBlob(idOf("payload-X"), BlobSource::fromString("payload-X"));
    const TreeId tree = build->putTree({blobEntry("data.bin", "payload-X")});
    const String hb_key = s->layout().buildHeartbeatKey(u128ToHex(build->buildId()));
    ASSERT_TRUE(b->head(hb_key).exists);

    build->abandon();

    /// Heartbeat gone; the uploaded objects remain as debris; no manifest was touched.
    EXPECT_FALSE(b->head(hb_key).exists);
    EXPECT_TRUE(b->head(s->layout().blobKey(blob.id)).exists);
    EXPECT_TRUE(b->head(s->layout().treeKey(tree)).exists);
    EXPECT_TRUE(s->listRefs(ns).empty());

    /// Further build ops ⇒ LOGICAL_ERROR (requireAlive).
    expectThrowsCode(DB::ErrorCodes::LOGICAL_ERROR,
        [&] { build->publish(ns, "part_1", tree, RefPayload{}); });
}

TEST(CasProtocol, DropReattachThroughDetachedNamespace)
{
    /// ATTACH choreography (design §4): publish part_1 in ns; adopt the tree and publish into
    /// ns/detached + drop part_1 from ns; then adopt + re-publish part_1 back in ns + drop from detached.
    /// The tree id never changes and the objects are never re-uploaded.
    auto b = std::make_shared<InMemoryBackend>();
    auto s = openStore(b);
    const RootNamespace ns{"srv1/tbl"};
    const RootNamespace detached{"srv1/tbl/detached"};

    auto build1 = s->startBuild({});
    auto blob = build1->putBlob(idOf("payload-X"), BlobSource::fromString("payload-X"));
    const TreeId tree = build1->putTree({blobEntry("data.bin", "payload-X")});
    build1->publish(ns, "part_1", tree, RefPayload{});

    const String blob_key = s->layout().blobKey(blob.id);
    const String tree_key = s->layout().treeKey(tree);
    const Token blob_tok = b->head(blob_key).token;
    const Token tree_tok = b->head(tree_key).token;

    EXPECT_TRUE(s->listRefs(ns).contains("part_1"));
    EXPECT_TRUE(s->listRefs(detached).empty());

    /// Move to detached: adopt the live tree, publish into detached, drop from ns.
    auto build2 = s->startBuild({});
    build2->adoptTree(tree);
    build2->publish(detached, "part_1", tree, RefPayload{});
    s->dropRef(ns, "part_1");

    EXPECT_TRUE(s->listRefs(ns).empty());
    ASSERT_TRUE(s->listRefs(detached).contains("part_1"));
    EXPECT_EQ(s->listRefs(detached).at("part_1").tree_id, tree);

    /// Re-attach: adopt the tree, re-publish part_1 back in ns, drop from detached.
    auto build3 = s->startBuild({});
    build3->adoptTree(tree);
    build3->publish(ns, "part_1", tree, RefPayload{});
    s->dropRef(detached, "part_1");

    ASSERT_TRUE(s->listRefs(ns).contains("part_1"));
    EXPECT_EQ(s->listRefs(ns).at("part_1").tree_id, tree);
    EXPECT_TRUE(s->listRefs(detached).empty());

    /// The tree id never changed and neither object was ever re-uploaded (tokens stable throughout).
    EXPECT_EQ(b->head(blob_key).token, blob_tok);
    EXPECT_EQ(b->head(tree_key).token, tree_tok);
}

TEST(CasProtocol, FreezeIntoShadowNamespace)
{
    /// FREEZE survives the table's part lifecycle (design §4): a shadow ref is a reachability root that
    /// outlives the dropped live ref.
    auto b = std::make_shared<InMemoryBackend>();
    auto s = openStore(b);
    const RootNamespace ns{"srv1/tbl"};
    const RootNamespace shadow{"shadow/backup1/tbl"};

    auto build1 = s->startBuild({});
    build1->putBlob(idOf("payload-X"), BlobSource::fromString("payload-X"));
    const TreeId tree = build1->putTree({blobEntry("data.bin", "payload-X")});
    build1->publish(ns, "part_1", tree, RefPayload{});

    /// Freeze into the shadow namespace, then drop the live ref.
    auto build2 = s->startBuild({});
    build2->adoptTree(tree);
    build2->publish(shadow, "part_1", tree, RefPayload{});
    s->dropRef(ns, "part_1");

    EXPECT_TRUE(s->listRefs(ns).empty());
    /// The shadow ref still resolves and reads after the live ref is gone.
    assertPartReads(b, s, shadow, "part_1", tree, "payload-X");
}

TEST(CasProtocol, ResurrectLosesRaceCondemnedDepAbortsRetryable)
{
    /// B190 INV-1: a tokened dep whose OWN token t0 is condemned. Even if the object was
    /// concurrently displaced to t1 (live), the dep's own token t0 is still condemned in the view ⇒
    /// `checkAndResolveDeps` Case(a): dep's own token condemned, blob has no retained source bytes ⇒ ABORTED.
    ///
    /// The old resurrect path would have GET'd the current incarnation (t1, uncondemned) and
    /// rewritten it with putOverwrite. INV-1 forbids GET on any dying object and forbids GET
    /// even on a live object when the dep's own token is condemned. ABORTED is the correct outcome.
    ///
    /// The model's `W_Resurrect` racing `W_Publish` interleaving (displacement between GET and
    /// putOverwrite) was the old test's focus. Under B190 the entire resurrect-by-GET path is
    /// removed; the caller retries from scratch with their own source bytes.
    auto b = std::make_shared<InMemoryBackend>();
    auto s = openStore(b);
    const RootNamespace ns{"srv1/tbl"};

    writeBlobRaw(*b, s->layout(), "payload-X", s->poolMeta().blob_header_len, s->poolMeta().pool_id);
    const String blob_key = s->layout().blobKey(idOf("payload-X"));
    const Token t0 = b->head(blob_key).token;

    auto build = s->startBuild({});
    build->putBlob(idOf("payload-X"), BlobSource::fromString("payload-X"));   /// dedup → adopts t0 at round 0
    const TreeId tree = build->putTree({blobEntry("data.bin", "payload-X")});

    /// Another writer displaces X to t1 (uncondemned) before our gate runs.
    const Token t1 = displaceBlobToken(*b, s->layout(), idOf("payload-X"));
    ASSERT_NE(t1, t0);

    /// The view still condemns the OLD t0 at round 1, fenced.
    injectRetire(*b, s->layout(), /*round*/ 1, /*fence_seq*/ 0, /*shard*/ 0,
        {RetiredEntry{.kind = ObjectKind::Blob, .hash = u128Of("payload-X"), .token = t0, .size = 9}});
    fenceNamespace(*b, s->layout(), ns, s->poolMeta().root_shards, /*round*/ 1);

    /// refresh ⇒ `checkAndResolveDeps`: hash hit (t0), dep's own t0 IS condemned ⇒ Case(a): no retained
    /// source bytes ⇒ ABORTED (INV-1; caller must retry from scratch with its own source bytes).
    expectThrowsCode(DB::ErrorCodes::ABORTED, [&] { build->publish(ns, "part_1", tree, RefPayload{}); });

    /// The object is at t1 (the displacing writer's incarnation, live and untouched by us).
    EXPECT_EQ(b->head(blob_key).token, t1);
    /// No ref was published.
    EXPECT_FALSE(s->resolveRef(ns, "part_1").has_value());
}

TEST(CasProtocol, NewNamespacePublishSeesRegistryFenceFloor)
{
    /// THE ABSENT-SHARD ORDERING HOLE's regression test (spec section 5 W-REGISTER, decision
    /// 2026-06-12). A publish into a BRAND-NEW namespace creates its manifest with fence_round 0,
    /// so the manifest alone can never trigger the gate's fence-advanced refresh - without the
    /// registry fence floor a stale-view writer would republish a condemned hash invisibly to the
    /// recheck, and the exact-token delete would then dangle a live ref. With W-REGISTER, the
    /// registration observes the registry's fence_round (raised by the GC round) and the gate
    /// refreshes + revalidates BEFORE the first publish commits.
    ///
    /// Discriminating asserts: WITHOUT the floor (max(root.fence_round /*0*/, 0)) the stale view
    /// has no entry for T, no resurrect happens, and deleteExact(T, t0) would return Deleted -
    /// the dangle made concrete. With the floor it returns TokenMismatch.
    auto b = std::make_shared<InMemoryBackend>();
    auto s = openStore(b);

    /// 1. part_1 -> tree T in namespace A, through the real Build (registers A).
    auto build_a = s->startBuild({});
    build_a->putBlob(idOf("floor-payload"), BlobSource::fromString("floor-payload"));
    auto tree = build_a->putTree({blobEntry("data.bin", "floor-payload")});
    build_a->publish(RootNamespace{"srv1/tbl"}, "part_1", tree, RefPayload{});
    const String tree_key = s->layout().treeKey(tree);
    const Token t0 = b->head(tree_key).token;

    /// 2. build B adopts T while the view is still at round 0 (a token-bearing cold-reuse dep
    /// since 2026-06-12 - observed at adopt time, stamped with view round 0).
    auto build_b = s->startBuild({});
    build_b->adoptTree(tree);

    /// 3. the last ref to T drops; a REAL GC round retires T at t0 and fences the registry
    /// (fence_round 1). No delete exists yet in this milestone - the retire set is the barrier.
    s->dropRef(RootNamespace{"srv1/tbl"}, "part_1");
    /// build_a (T's owner) finished; advance the durable watermark floor past its seq so the Task 10
    /// build-watermark guard condemns T. build_b is still in-flight, but it does NOT own T (T was
    /// written by build_a), so its still-active seq does not protect T.
    build_a.reset();
    s->renewWatermarkOnce();
    Gc gc(s, hexToU128("00000000000000000000000000000001"));
    ASSERT_TRUE(gc.runRegularRound().acquired_lease);

    /// The full round COMPLETED: T (unreachable) was deleted at t0 and its retired entry dropped
    /// on the confirmed outcome - the view alone can no longer condemn it.
    EXPECT_FALSE(b->head(tree_key).exists);

    /// 4. build B publishes T into a BRAND-NEW namespace: ensureRegistered returns the registry's
    /// fence_round (1) > view round (0) => the gate refreshes => B's STALE dep on T (observed at
    /// view round 0 < 1, no view hit - the entry dropped) is RE-OBSERVED (one HEAD,
    /// W-REVALIDATE): T is ABSENT and adoptTree retains no payload => the publish ABORTS
    /// retryably. It must NEVER land - landing would write a manifest naming a deleted tree (the
    /// dangle this whole ordering machinery exists to prevent).
    expectThrowsCode(DB::ErrorCodes::ABORTED, [&]
    {
        build_b->publish(RootNamespace{"srv2/new"}, "part_x", tree, RefPayload{});
    });

    /// NO DANGLE: nothing in the new namespace names the deleted tree.
    EXPECT_FALSE(s->resolveRef(RootNamespace{"srv2/new"}, "part_x").has_value());
    EXPECT_EQ(b->deleteExact(tree_key, t0).kind, DeleteOutcome::Kind::NotFound);   /// long gone

    /// WITHOUT the registry gate floor the publish LANDS on the stale round-0 view (no refresh, no
    /// re-observation) and resolveRef would return a ref whose readTree throws - the dangle made
    /// concrete. (Verified red with the floor disabled.)
}

TEST(CasProtocol, FreshEvidenceDepWithViewHitIsResolvedByGate)
{
    /// B190 Task-3 coverage: a TOKENLESS (W-EVIDENCE) dep recorded at the CURRENT view round (fresh —
    /// observed_view_round == current round, so the stale branch does NOT fire) that has a
    /// view hit by hash MUST still be resolved by `checkAndResolveDeps` (observeAndAdmit → ABORTED
    /// when condemned).
    ///
    /// In the merged `checkAndResolveDeps` pass: a fresh tokenless dep with a view hit goes
    /// directly to observeAndAdmit (the stale-refresh branch is skipped because
    /// observed_view_round == current round). This test verifies that path is preserved:
    /// "fresh tokenless + hit → observeAndAdmit."
    ///
    /// Discriminating assertion: publish throws ABORTED (condemned blob, no source bytes) — NOT
    /// FILE_DOESNT_EXIST and NOT success (which would publish a dangle).
    auto b = std::make_shared<InMemoryBackend>();

    /// Pre-inject retire state at round=1 BEFORE opening the store — the store's open-time refresh
    /// lands at round=1, so any dep recorded thereafter is non-stale (observed_view_round == 1).
    DB::Cas::Layout layout("p");
    writeBlobRaw(*b, layout, "payload-fresh-ev", 256 /*blob_header_len*/, UInt128{} /*pool_id*/);
    const String blob_key = layout.blobKey(idOf("payload-fresh-ev"));
    const Token t0 = b->head(blob_key).token;
    injectRetire(*b, layout, /*round*/ 1, /*fence_seq*/ 0, /*shard*/ 0,
        {RetiredEntry{.kind = ObjectKind::Blob, .hash = u128Of("payload-fresh-ev"), .token = t0, .size = 16}});

    /// Open the store AFTER injecting round=1 — retire view refreshes at round=1.
    auto s = openStore(b);
    ASSERT_EQ(s->retireView().round(), 1u);

    const RootNamespace ns{"srv1/tbl"};
    const TreeId source = writeTreeRaw(*b, s->layout(), {blobEntry("data.bin", "payload-fresh-ev")}, s->poolMeta().pool_id);

    auto build = s->startBuild({});
    /// adoptFromTree records a TOKENLESS dep at observed_view_round = 1 (the current round) — FRESH.
    const TreeEntry adopted = build->adoptFromTree(source, "data.bin");
    const TreeId tree = build->putTree({adopted});

    /// No fence advance (round stays 1): `checkAndResolveDeps` will see observed_view_round=1 >= round=1
    /// (fresh) — skips the stale-refresh branch. The hash hit still triggers observeAndAdmit.
    ///
    /// Publish: no fence advance ⇒ no refresh ⇒ `checkAndResolveDeps`: dep is fresh (skips stale
    /// branch); hits.has_value() → tokenless + hit ⇒ observeAndAdmit ⇒ HEAD ⇒ current t0 condemned
    /// ⇒ ABORTED (INV-1; blob has no retained source bytes).
    expectThrowsCode(DB::ErrorCodes::ABORTED,
        [&] { build->publish(ns, "part_1", tree, RefPayload{}); });

    /// The condemned blob stays at t0 — nothing was written by the gate.
    EXPECT_EQ(b->head(blob_key).token, t0);
    /// No ref was published.
    EXPECT_FALSE(s->resolveRef(ns, "part_1").has_value());
}

TEST(CasProtocol, AdoptTreeOfReclaimedTreeFailsClosedAtAdoptTime)
{
    /// The SAME-ROUND companion of the scenario above (found refreshing the TLA+ model, B91,
    /// 2026-06-12): adoptTree must OBSERVE the object (cold-reuse semantics, one HEAD), never
    /// record blind tokenless evidence. A detached tree reclaimed by a COMPLETED round has no
    /// view hit (entries drop on confirmed outcomes), and a view already refreshed AT the current
    /// round skips both the publish-time re-observation (the observed_view_round >= round keep
    /// branch) AND the fence-advanced refresh (view round == fence round). With a blind adopt
    /// nothing is left to catch it and the publish lands a manifest naming a deleted tree - the
    /// dangle. The live-source argument that justifies tokenless evidence for adoptFromTree
    /// children does not apply here: a detached tree is exactly NOT pinned by anything.
    auto b = std::make_shared<InMemoryBackend>();
    auto s = openStore(b);

    auto build_a = s->startBuild({});
    build_a->putBlob(idOf("wr-payload"), BlobSource::fromString("wr-payload"));
    auto tree = build_a->putTree({blobEntry("data.bin", "wr-payload")});
    build_a->publish(RootNamespace{"srv1/tbl"}, "part_1", tree, RefPayload{});

    /// Detach, then a FULL GC round: T deleted at its observed token, entry dropped on the
    /// confirmed outcome, namespace fenced at round 1.
    s->dropRef(RootNamespace{"srv1/tbl"}, "part_1");
    /// build_a (T's owner) finished; advance the durable watermark floor past its seq so the Task 10
    /// build-watermark guard condemns T (the background renewer is off in this test).
    build_a.reset();
    s->renewWatermarkOnce();
    Gc gc(s, hexToU128("00000000000000000000000000000001"));
    ASSERT_TRUE(gc.runRegularRound().acquired_lease);
    ASSERT_FALSE(b->head(s->layout().treeKey(tree)).exists);

    /// The process view refreshes AFTER the round completed (any unrelated publish does this in
    /// production): round 1, NO entry for T - the view alone can never condemn it again.
    s->retireView().refresh();

    /// Re-attach attempt: the adopt itself must fail closed at observation time - by the time of
    /// the publish gate there is nothing left to catch it (no hit, fresh round, no refresh).
    auto build_b = s->startBuild({});
    expectThrowsCode(DB::ErrorCodes::FILE_DOESNT_EXIST, [&] { build_b->adoptTree(tree); });

    /// NOTHING may name the deleted tree.
    EXPECT_FALSE(s->resolveRef(RootNamespace{"srv1/tbl"}, "part_2").has_value());
}
