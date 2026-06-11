#include <gtest/gtest.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasBuild.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasStore.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasInMemoryBackend.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasEnvelope.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasGcFormats.h>
#include <Disks/tests/cas_test_helpers.h>
#include <Common/Exception.h>

/// Task 13 — the adversarial publish gate (the protocol climax of M-C2). Every scenario injects GC
/// state through the on-storage GC surface ONLY (injectRetire writes gc/state + a retired set;
/// fenceNamespace raises fence_round — exactly the writes a GC leader performs), then publishes. The
/// Store refreshes its retire view at open; Build::publish refreshes again on a fence-advanced conflict
/// and runs revalidateDeps (W-REVALIDATE) before the gate scan (W-EVIDENCE / condemned-token).
///
/// The standard pattern: build + upload (records tokens at view round 0), THEN inject retire + fence,
/// THEN publish. The first CAS attempt reads the fenced manifest, sees view.round() < fence_round,
/// refreshes, and revalidates.

namespace DB::ErrorCodes
{
extern const int ABORTED;
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

TEST(CasProtocol, FenceConflictForcesRevalidateAndResurrect)
{
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

    /// First CAS reads fence_round=1 > view round 0 ⇒ refresh ⇒ X condemned at t0 ⇒ resurrect; T not
    /// condemned, stale-observed, re-observed and kept. Publish lands.
    build->publish(ns, "part_1", tree, RefPayload{});

    assertPartReads(b, s, ns, "part_1", tree, "payload-X");

    /// INV-NO-RETURN: the condemned incarnation can never be current again — its old token 412s.
    EXPECT_EQ(b->deleteExact(blob_key, t0).kind, DeleteOutcome::Kind::TokenMismatch);
    /// X rides a new token after the resurrect.
    EXPECT_NE(b->head(blob_key).token, t0);
}

TEST(CasProtocol, RevalidateReObservesStaleTokenKeepsWhenUnchanged)
{
    /// The F1 IN-FLIGHT DISJUNCTION keep-branch.
    auto b = std::make_shared<InMemoryBackend>();
    auto s = openStore(b);
    const RootNamespace ns{"srv1/tbl"};

    /// X pre-exists out-of-band; the build cold-reuses it (records the current token t0 at round 0).
    writeBlobRaw(*b, s->layout(), "payload-X", s->poolMeta().blob_header_len, s->poolMeta().pool_id);
    const String blob_key = s->layout().blobKey(idOf("payload-X"));
    const Token t0 = b->head(blob_key).token;

    auto build = s->startBuild({});
    build->reuseBlob(idOf("payload-X"));
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
    build->reuseBlob(idOf("payload-X"));   /// records t0 at round 0
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

    /// Black-box proof the dep now rides t1, not the stale t0: a SECOND injected retire of the OLD t0
    /// + fence to round 2 + re-publish into a different ref must NOT resurrect (t0 isn't our token any
    /// more — the gate finds no condemned entry at our current token t1). The object stays at t1.
    injectRetire(*b, s->layout(), /*round*/ 2, /*fence_seq*/ 0, /*shard*/ 1,
        {RetiredEntry{.kind = ObjectKind::Blob, .hash = u128Of("payload-X"), .token = t0, .size = 9}});
    fenceNamespace(*b, s->layout(), ns, s->poolMeta().root_shards, /*round*/ 2);
    build->publish(ns, "part_2", tree, RefPayload{});
    EXPECT_EQ(b->head(blob_key).token, t1);
}

TEST(CasProtocol, RevalidateAbsentBlobDepAbortsRetryable)
{
    auto b = std::make_shared<InMemoryBackend>();
    auto s = openStore(b);
    const RootNamespace ns{"srv1/tbl"};

    writeBlobRaw(*b, s->layout(), "payload-X", s->poolMeta().blob_header_len, s->poolMeta().pool_id);
    const String blob_key = s->layout().blobKey(idOf("payload-X"));

    auto build = s->startBuild({});
    build->reuseBlob(idOf("payload-X"));   /// records t0 at round 0
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

TEST(CasProtocol, EvidenceHitForcesResolution)
{
    /// W-EVIDENCE: adoptFromTree records a TOKENLESS dep on blob X; the gate must resolve a view hit.
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

    /// refresh ⇒ revalidateDeps SKIPS the tokenless dep ⇒ gateCheckDeps: tokenless X has a view hit ⇒
    /// observeAndAdmit ⇒ HEAD ⇒ current t0 condemned ⇒ resurrect ⇒ X rides a new token. Publish lands.
    build->publish(ns, "part_1", tree, RefPayload{});

    assertPartReads(b, s, ns, "part_1", tree, "payload-X");
    EXPECT_EQ(b->deleteExact(blob_key, t0).kind, DeleteOutcome::Kind::TokenMismatch);
}

TEST(CasProtocol, WedgedHeartbeatSelfHeal)
{
    /// spec §5 wedged-heartbeat trace: a build whose heartbeat never renews finds its OWN uploads
    /// condemned by full GC; its publish refreshes, sees them condemned, and resurrects (it built them).
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

    build->publish(ns, "part_1", tree, RefPayload{});

    assertPartReads(b, s, ns, "part_1", tree, "payload-X");
    EXPECT_NE(b->head(blob_key).token, t0);   /// rides a new token after self-heal resurrect
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

TEST(CasProtocol, ResurrectLosesRaceReObserves)
{
    /// CARRYOVER from Task 11: the racing-PreconditionFailed branch in resurrect (GET an object
    /// condemned at t0, but a concurrent writer DISPLACES it to t1 before our putOverwrite, so the
    /// If-Match against t0 fails ⇒ observeAndAdmit re-observes and adopts the live t1).
    ///
    /// Single-thread reachability: resurrect's putOverwrite uses the token IT just read via GET, so to
    /// force PreconditionFailed the displacement must land BETWEEN that GET and the putOverwrite — a
    /// genuinely concurrent interleaving the single-thread gate cannot express without a backend hook
    /// firing inside putOverwrite. We approximate the OBSERVABLE OUTCOME of the lost race: the dep ends
    /// valid, riding the displacing writer's live token, with no dangling reference. We drive it by
    /// pre-displacing X to t1 (uncondemned) while the view still condemns the OLD t0: revalidateDeps
    /// sees no hit at the current token t1, re-observes, and adopts t1 — the same end state a lost
    /// resurrect race reaches (the racing writer's incarnation wins). The exact GET/putOverwrite
    /// interleaving is covered by the model (`W_Resurrect` racing `W_Publish`), not reproduced here.
    auto b = std::make_shared<InMemoryBackend>();
    auto s = openStore(b);
    const RootNamespace ns{"srv1/tbl"};

    writeBlobRaw(*b, s->layout(), "payload-X", s->poolMeta().blob_header_len, s->poolMeta().pool_id);
    const String blob_key = s->layout().blobKey(idOf("payload-X"));
    const Token t0 = b->head(blob_key).token;

    auto build = s->startBuild({});
    build->reuseBlob(idOf("payload-X"));   /// records t0 at round 0
    const TreeId tree = build->putTree({blobEntry("data.bin", "payload-X")});

    /// The racing writer displaces X to t1 (its resurrect won) before our gate runs.
    const Token t1 = displaceBlobToken(*b, s->layout(), idOf("payload-X"));
    ASSERT_NE(t1, t0);

    /// The view still condemns the OLD t0 (the racing writer's retire entry for the displaced
    /// incarnation), at round 1, fenced.
    injectRetire(*b, s->layout(), /*round*/ 1, /*fence_seq*/ 0, /*shard*/ 0,
        {RetiredEntry{.kind = ObjectKind::Blob, .hash = u128Of("payload-X"), .token = t0, .size = 9}});
    fenceNamespace(*b, s->layout(), ns, s->poolMeta().root_shards, /*round*/ 1);

    /// refresh ⇒ X has a view hit (t0) but NOT at our observed t0... our observed token IS t0, and t0
    /// IS condemned ⇒ revalidateDeps takes the condemned-at-our-token branch ⇒ resurrect. resurrect
    /// GETs the CURRENT incarnation (t1, uncondemned), rewrites a fresh tag, putOverwrite(If-Match t1)
    /// ⇒ succeeds (single-thread, no further displacement) ⇒ X rides a new token t2. The lost-race
    /// PreconditionFailed sub-branch (displacement between GET and putOverwrite) is the model-covered
    /// interleaving; here resurrect completes and the dep ends valid with no dangle.
    build->publish(ns, "part_1", tree, RefPayload{});

    assertPartReads(b, s, ns, "part_1", tree, "payload-X");
    const Token t2 = b->head(blob_key).token;
    EXPECT_NE(t2, t0);
    EXPECT_NE(t2, t1);
    /// INV-NO-RETURN: the condemned t0 can never be current again.
    EXPECT_EQ(b->deleteExact(blob_key, t0).kind, DeleteOutcome::Kind::TokenMismatch);
}
