#include <gtest/gtest.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasBuild.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasStore.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasInMemoryBackend.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasEnvelope.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasGcFormats.h>
#include <Disks/tests/cas_test_helpers.h>
#include <IO/WriteBuffer.h>
#include <IO/WriteHelpers.h>
#include <Common/Exception.h>

namespace DB::ErrorCodes
{
extern const int BAD_ARGUMENTS;
extern const int FILE_DOESNT_EXIST;
extern const int LOGICAL_ERROR;
extern const int NOT_IMPLEMENTED;
extern const int ABORTED;
}

using namespace DB::Cas;
using DB::Cas::tests::expectThrowsCode;
using DB::Cas::tests::idOf;
using DB::Cas::tests::injectRetire;
using DB::Cas::tests::shardOfForTest;
using DB::Cas::tests::u128Of;
using DB::Cas::tests::writeTreeRaw;

namespace
{

StorePtr openStore(const std::shared_ptr<InMemoryBackend> & b)
{
    return Store::open(b, PoolConfig{.pool_prefix = "p"});
}

/// A one-shot backend hook (mirrors the WriteCountingBackend delegation pattern in gtest_cas_store.cpp):
/// it delegates every op to a wrapped Backend, but the FIRST time head(target_key) is called it fires a
/// deleteExact(target_key, condemned_token) AFTER computing the (present) HEAD result and BEFORE returning
/// it — simulating GC's exact-token content delete landing in the writer's HEAD->GET window (B136).
class HeadThenDeleteOnceBackend final : public DB::Cas::Backend
{
public:
    HeadThenDeleteOnceBackend(BackendPtr inner_, String target_key_, DB::Cas::Token condemned_)
        : inner(std::move(inner_)), target_key(std::move(target_key_)), condemned(condemned_) {}

    DB::Cas::HeadResult head(const String & k) override
    {
        const DB::Cas::HeadResult hr = inner->head(k);
        if (k == target_key && !fired)
        {
            fired = true;
            /// GC's single content-delete site, landing in the HEAD->GET window.
            inner->deleteExact(target_key, condemned);
        }
        return hr;
    }

    std::optional<DB::Cas::GetResult> get(const String & k, DB::Cas::Range r = {}) override { return inner->get(k, r); }
    DB::Cas::ListPage list(const String & p, const String & c, size_t l) override { return inner->list(p, c, l); }
    DB::Cas::PutOutcome putIfAbsent(const String & k, const String & b, DB::Cas::Token * t = nullptr) override { return inner->putIfAbsent(k, b, t); }
    DB::Cas::WriteSinkPtr putIfAbsentStream(const String & k) override { return inner->putIfAbsentStream(k); }
    DB::Cas::PutOutcome putOverwrite(const String & k, const String & b, const DB::Cas::Token & e, DB::Cas::Token * t = nullptr) override { return inner->putOverwrite(k, b, e, t); }
    DB::Cas::CasOutcome casPut(const String & k, const String & b, const std::optional<DB::Cas::Token> & e, DB::Cas::Token * t = nullptr) override { return inner->casPut(k, b, e, t); }
    DB::Cas::DeleteOutcome deleteExact(const String & k, const DB::Cas::Token & t) override { return inner->deleteExact(k, t); }

private:
    BackendPtr inner;
    String target_key;
    DB::Cas::Token condemned;
    bool fired = false;
};

}

TEST(CasBuild, StartBuildHeartbeatDurableFirst)
{
    auto b = std::make_shared<InMemoryBackend>();
    auto s = openStore(b);
    auto build = s->startBuild({});
    /// W-HEARTBEAT: the heartbeat object is durable when startBuild returns.
    EXPECT_TRUE(b->head(s->layout().buildHeartbeatKey(u128ToHex(build->buildId()))).exists);
}

TEST(CasBuild, PutBlobWritesEnvelopeWithFixedHeader)
{
    auto b = std::make_shared<InMemoryBackend>();
    auto s = openStore(b);
    auto build = s->startBuild({});
    auto ref = build->putBlob(idOf("hello world"), BlobSource::fromString("hello world"));
    EXPECT_EQ(ref.size, 11u);

    auto raw = b->get(s->layout().blobKey(ref.id));
    ASSERT_TRUE(raw.has_value());
    auto h = decodeEnvelopeHeader(raw->bytes, raw->bytes.size(), ObjectKind::Blob);
    EXPECT_EQ(h.header_len, s->poolMeta().blob_header_len);   /// 256
    EXPECT_EQ(h.logical_size, 11u);
    EXPECT_EQ(u128ToHex(h.logical_hash), ref.id.string());
    EXPECT_EQ(h.domain_id, s->poolMeta().pool_id);
    EXPECT_EQ(h.build_id, build->buildId());
    EXPECT_NE(h.incarnation_tag, UInt128{});
    EXPECT_EQ(raw->bytes.substr(h.header_len), "hello world");
}

TEST(CasBuild, PutBlobDedupSecondWriterAdopts)
{
    auto b = std::make_shared<InMemoryBackend>();
    auto s = openStore(b);

    auto build_a = s->startBuild({});
    auto ref_a = build_a->putBlob(idOf("dup"), BlobSource::fromString("dup"));
    const Token token_a = b->head(s->layout().blobKey(ref_a.id)).token;

    auto build_b = s->startBuild({});
    auto ref_b = build_b->putBlob(idOf("dup"), BlobSource::fromString("dup"));

    EXPECT_EQ(ref_b.id, ref_a.id);
    /// A's incarnation survives — the second writer adopts, nothing was overwritten.
    EXPECT_EQ(b->head(s->layout().blobKey(ref_a.id)).token, token_a);
}

TEST(CasBuild, PutBlobWrongSizeFailsClosed)
{
    auto b = std::make_shared<InMemoryBackend>();
    auto s = openStore(b);
    auto build = s->startBuild({});

    BlobSource lying;
    lying.size = 11;   /// declares 11 but writes 5
    lying.write_payload = [](DB::WriteBuffer & out) { DB::writeString(std::string_view("short"), out); };

    const BlobId id = idOf("does-not-matter");
    expectThrowsCode(DB::ErrorCodes::LOGICAL_ERROR, [&] { build->putBlob(id, std::move(lying)); });
    /// The cancelled stream created nothing.
    EXPECT_FALSE(b->head(s->layout().blobKey(id)).exists);
}

TEST(CasBuild, ReuseBlobAbsentThrows)
{
    auto b = std::make_shared<InMemoryBackend>();
    auto s = openStore(b);
    auto build = s->startBuild({});
    /// B156: reuseBlob treats absent-on-reuse as a retryable race (caller only reuses known-staged blobs);
    /// genuinely-never-existed is caught by adoptTree/putBlob fail-closed paths.
    expectThrowsCode(DB::ErrorCodes::ABORTED, [&] { build->reuseBlob(idOf("never-written")); });
}

TEST(CasBuildReuseBlob, VanishedBlobIsRetryableAborted)
{
    /// B156: reuseBlob on a blob that was deleted by GC after being staged (putBlob'd) in this same
    /// transaction must throw ABORTED (retryable), NOT FILE_DOESNT_EXIST (fatal).
    ///
    /// The race: the blob DID exist when the transaction uploaded it; GC's exact-token deleteExact
    /// landed between the reuseBlob call and our HEAD inside observeAndAdmit. The caller (e.g.,
    /// ContentAddressedTransaction::createHardLink / moveDirectory / moveFile) detects ABORTED and
    /// retries, which re-uploads the content via putBlob from the held source. The fatal
    /// FILE_DOESNT_EXIST path is intentionally preserved for putBlob's body-holding retry and
    /// adoptTree's never-existed fail-closed semantics — those callers are NOT going through reuseBlob.
    auto b = std::make_shared<InMemoryBackend>();

    /// 1. Upload a blob so it exists, capture its token t0.
    BlobId id;
    Token t0;
    {
        auto s0 = openStore(b);
        auto build0 = s0->startBuild({});
        id = build0->putBlob(idOf("reuse-payload"), BlobSource::fromString("reuse-payload")).id;
        t0 = b->head(s0->layout().blobKey(id)).token;
    }

    /// 2. Delete it from the backend to simulate GC's exact-token deleteExact.
    DB::Cas::Layout layout("p");
    b->deleteExact(layout.blobKey(id), t0);
    ASSERT_FALSE(b->head(layout.blobKey(id)).exists);

    /// 3. reuseBlob on the now-absent blob must throw ABORTED (retryable), not FILE_DOESNT_EXIST (fatal).
    auto s = openStore(b);
    auto build = s->startBuild({});
    expectThrowsCode(DB::ErrorCodes::ABORTED, [&] { build->reuseBlob(id); });

    /// 4. Confirm: a successful reuseBlob on a live (non-deleted) blob still works.
    {
        auto s1 = openStore(b);
        auto build1 = s1->startBuild({});
        const BlobId live_id = build1->putBlob(idOf("live-payload"), BlobSource::fromString("live-payload")).id;
        auto ref = build1->reuseBlob(live_id);
        EXPECT_EQ(ref.id, live_id);
    }
}

TEST(CasBuild, ReuseBlobCondemnedResurrects)
{
    auto b = std::make_shared<InMemoryBackend>();

    /// Write X via a throwaway build; capture its token t0.
    BlobId id;
    Token t0;
    {
        auto s0 = openStore(b);
        auto build0 = s0->startBuild({});
        id = build0->putBlob(idOf("payload-X"), BlobSource::fromString("payload-X")).id;
        t0 = b->head(s0->layout().blobKey(id)).token;
    }

    /// Condemn (Blob, X, t0) in the retire view, then open a FRESH Store so its retireView.refresh
    /// (at open) sees the injection.
    DB::Cas::Layout layout("p");
    injectRetire(*b, layout, /*round*/ 1, /*fence_seq*/ 0, /*shard*/ 0,
        {RetiredEntry{.kind = ObjectKind::Blob, .hash = u128Of("payload-X"), .token = t0, .size = 9}});

    auto s = openStore(b);
    auto build = s->startBuild({});
    auto ref = build->reuseBlob(id);
    EXPECT_EQ(ref.id, id);

    /// The object was rewritten: a NEW token, a NEW incarnation_tag, the SAME payload + header_len.
    auto raw = b->get(s->layout().blobKey(id));
    ASSERT_TRUE(raw.has_value());
    EXPECT_NE(raw->token, t0);
    auto h = decodeEnvelopeHeader(raw->bytes, raw->bytes.size(), ObjectKind::Blob);
    EXPECT_EQ(h.header_len, s->poolMeta().blob_header_len);
    EXPECT_EQ(raw->bytes.substr(h.header_len), "payload-X");

    /// INV-NO-RETURN: the condemned incarnation is unreachable by its old token.
    EXPECT_EQ(b->deleteExact(s->layout().blobKey(id), t0).kind, DeleteOutcome::Kind::TokenMismatch);
}

TEST(CasBuild, PutBlobResurrectVanishedReUploadsHeldBody)
{
    auto b = std::make_shared<InMemoryBackend>();

    /// 1. Write payload-X via a throwaway build to create the blob; capture its token t0.
    BlobId id;
    Token t0;
    {
        auto s0 = openStore(b);
        auto build0 = s0->startBuild({});
        id = build0->putBlob(idOf("payload-X"), BlobSource::fromString("payload-X")).id;
        t0 = b->head(s0->layout().blobKey(id)).token;
    }

    /// 2. Condemn (Blob, hash(X), t0) in the retire view.
    DB::Cas::Layout layout("p");
    const String blob_key = layout.blobKey(id);
    injectRetire(*b, layout, /*round*/ 1, /*fence_seq*/ 0, /*shard*/ 0,
        {RetiredEntry{.kind = ObjectKind::Blob, .hash = u128Of("payload-X"), .token = t0, .size = 9}});

    /// 3. Wrap the backend so the NEXT head(blob_key) returns the (present) result and THEN fires
    ///    deleteExact(blob_key, t0) exactly once — GC's delete in the HEAD->GET window. Open a FRESH
    ///    Store over the hook so its retire view (refreshed at open) sees the condemnation.
    auto hook = std::make_shared<HeadThenDeleteOnceBackend>(b, blob_key, t0);
    auto s = Store::open(hook, PoolConfig{.pool_prefix = "p"});
    auto build = s->startBuild({});

    /// 4. putBlob with a re-invokable body.
    ///    BEFORE fix: putIfAbsent -> PreconditionFailed -> observeAndAdmit HEAD (present, condemned)
    ///                -> resurrect GET (vanished, deleted in the window) -> throws FILE_DOESNT_EXIST.
    ///    AFTER fix:  the FILE_DOESNT_EXIST is caught; putBlob re-runs the fresh-upload path with the
    ///                still-held body; the object is recreated under a FRESH token.
    auto ref = build->putBlob(idOf("payload-X"), BlobSource::fromString("payload-X"));
    EXPECT_EQ(ref.id, id);

    /// 5. The blob is present again under a FRESH token, with the same payload; and the condemned token
    ///    never returns (INV-NO-RETURN).
    const HeadResult hr = b->head(blob_key);
    ASSERT_TRUE(hr.exists);
    EXPECT_NE(hr.token, t0);

    auto raw = b->get(blob_key);
    ASSERT_TRUE(raw.has_value());
    auto h = decodeEnvelopeHeader(raw->bytes, raw->bytes.size(), ObjectKind::Blob);
    EXPECT_EQ(h.header_len, s->poolMeta().blob_header_len);
    EXPECT_EQ(raw->bytes.substr(h.header_len), "payload-X");

    EXPECT_EQ(b->deleteExact(blob_key, t0).kind, DeleteOutcome::Kind::TokenMismatch);
}

TEST(CasBuild, PublishResurrectVanishedThrowsAbortedRetryable)
{
    /// B137: the resurrect HEAD->GET window also races GC's exact-token delete on the BODYLESS publish
    /// path (gateCheckDeps -> observeAndAdmit -> resurrect), where no writer holds the body to re-upload.
    /// That vanish must surface as ABORTED ("retry the operation") — a retryable transient, matching the
    /// sibling lost-dependency branches — NOT a hard FILE_DOESNT_EXIST (which became an HTTP-500 INSERT
    /// failure).
    auto b = std::make_shared<InMemoryBackend>();

    /// 1. Write payload-X via a throwaway build to create the blob object; capture its token t0.
    BlobId id;
    Token t0;
    {
        auto s0 = openStore(b);
        auto build0 = s0->startBuild({});
        id = build0->putBlob(idOf("payload-X"), BlobSource::fromString("payload-X")).id;
        t0 = b->head(s0->layout().blobKey(id)).token;
    }

    /// 2. A source tree referencing blob hash(X) by name. adoptFromTree records a TOKENLESS (evidence)
    ///    Blob dep on hash(X) WITHOUT holding the body — exactly the bodyless dependency B137 is about.
    DB::Cas::Layout layout("p");
    const String blob_key = layout.blobKey(id);
    TreeEntry src_entry;
    src_entry.name = "data.bin";
    src_entry.placement = Placement::Blob;
    src_entry.file_hash = u128Of("payload-X");
    src_entry.file_size = 9;
    const TreeId source = writeTreeRaw(*b, layout, {src_entry}, openStore(b)->poolMeta().pool_id);

    /// 3. Condemn (Blob, hash(X), t0) in the retire view, so the publish gate sees the evidence dep as a
    ///    condemned hit and must resolve it (observeAndAdmit -> resurrect).
    injectRetire(*b, layout, /*round*/ 1, /*fence_seq*/ 0, /*shard*/ 0,
        {RetiredEntry{.kind = ObjectKind::Blob, .hash = u128Of("payload-X"), .token = t0, .size = 9}});

    /// 4. Wrap the backend so the NEXT head(blob_key) returns the (present) result and THEN fires
    ///    deleteExact(blob_key, t0) exactly once — GC's exact-token delete landing in the resurrect
    ///    HEAD->GET window. Open a FRESH Store over the hook so its retire view (refreshed at open) sees
    ///    the condemnation. The FIRST head(blob_key) in this build is the one inside the gate's
    ///    observeAndAdmit (adoptFromTree only reads the source tree; putTree of the new tree does not
    ///    head the blob), so the one-shot fires precisely in the window before resurrect's GET.
    auto hook = std::make_shared<HeadThenDeleteOnceBackend>(b, blob_key, t0);
    auto s = Store::open(hook, PoolConfig{.pool_prefix = "p"});
    auto build = s->startBuild({});

    /// 5. Adopt the blob as a tokenless evidence dep, then build a NEW tree referencing it. No body in hand.
    const TreeEntry adopted = build->adoptFromTree(source, "data.bin");
    const TreeId tree = build->putTree({adopted});

    /// 6. Publish drives the bodyless gate: tokenless+condemned -> observeAndAdmit head(blob_key) [hook
    ///    fires deleteExact(blob_key, t0)] -> present&condemned -> resurrect GET (vanished) -> throws.
    ///    BEFORE fix: FILE_DOESNT_EXIST (hard). AFTER fix: ABORTED "retry the operation" (retryable).
    expectThrowsCode(DB::ErrorCodes::ABORTED,
        [&] { build->publish(RootNamespace{"srv1/tbl"}, "part_1", tree, RefPayload{}); });
}

TEST(CasBuild, PutTreeEnforcesBottomUp)
{
    auto b = std::make_shared<InMemoryBackend>();
    auto s = openStore(b);
    auto build = s->startBuild({});

    /// A blob entry referencing an unknown hash → W-TREE-BUILD violation.
    TreeEntry blob_entry;
    blob_entry.name = "data.bin";
    blob_entry.placement = Placement::Blob;
    blob_entry.file_hash = u128Of("blob-content");
    blob_entry.file_size = 12;
    expectThrowsCode(DB::ErrorCodes::LOGICAL_ERROR, [&] { build->putTree({blob_entry}); });

    /// After putBlob of that hash → putTree succeeds.
    build->putBlob(idOf("blob-content"), BlobSource::fromString("blob-content"));
    const TreeId child = build->putTree({blob_entry});

    /// A Subtree entry referencing an unknown child tree → violation.
    TreeEntry subtree_entry;
    subtree_entry.name = "sub";
    subtree_entry.placement = Placement::Subtree;
    subtree_entry.file_hash = u128Of("unknown-child-tree");
    subtree_entry.file_size = 1;
    expectThrowsCode(DB::ErrorCodes::LOGICAL_ERROR, [&] { build->putTree({subtree_entry}); });

    /// adoptTree(child) records the dep; then a Subtree pointing at it succeeds.
    build->adoptTree(child);
    TreeEntry good_subtree;
    good_subtree.name = "sub";
    good_subtree.placement = Placement::Subtree;
    good_subtree.file_hash = DB::Cas::hexToU128(child.string());
    good_subtree.file_size = 1;
    EXPECT_NO_THROW(build->putTree({good_subtree}));

    /// An Inline-only tree needs no deps.
    TreeEntry inline_entry;
    inline_entry.name = "small";
    inline_entry.placement = Placement::Inline;
    inline_entry.inline_bytes = "abc";
    inline_entry.file_size = 3;
    EXPECT_NO_THROW(build->putTree({inline_entry}));
}

TEST(CasBuild, AdoptFromTreeRecordsEvidence)
{
    auto b = std::make_shared<InMemoryBackend>();
    auto s = openStore(b);

    /// Build a source tree with a Blob entry "data.bin".
    DB::Cas::Layout layout("p");
    TreeEntry src_entry;
    src_entry.name = "data.bin";
    src_entry.placement = Placement::Blob;
    src_entry.file_hash = u128Of("source-blob");
    src_entry.file_size = 11;
    const TreeId source = writeTreeRaw(*b, layout, {src_entry}, s->poolMeta().pool_id);

    auto build = s->startBuild({});

    /// Unknown name → BAD_ARGUMENTS.
    expectThrowsCode(DB::ErrorCodes::BAD_ARGUMENTS, [&] { build->adoptFromTree(source, "missing"); });

    /// The entry is returned and a tokenless dep recorded — observed indirectly: a subsequent putTree
    /// referencing that blob hash must NOT throw W-TREE-BUILD.
    const TreeEntry adopted = build->adoptFromTree(source, "data.bin");
    EXPECT_EQ(adopted.name, "data.bin");
    EXPECT_EQ(adopted.file_hash, u128Of("source-blob"));

    TreeEntry reuse_entry = adopted;
    EXPECT_NO_THROW(build->putTree({reuse_entry}));
}

TEST(CasBuild, AbandonDropsHeartbeatAndDisables)
{
    auto b = std::make_shared<InMemoryBackend>();
    auto s = openStore(b);
    auto build = s->startBuild({});

    auto blob_ref = build->putBlob(idOf("kept"), BlobSource::fromString("kept"));
    TreeEntry entry;
    entry.name = "f";
    entry.placement = Placement::Blob;
    entry.file_hash = u128Of("kept");
    entry.file_size = 4;
    const TreeId tree = build->putTree({entry});

    const String heartbeat_key = s->layout().buildHeartbeatKey(u128ToHex(build->buildId()));
    ASSERT_TRUE(b->head(heartbeat_key).exists);

    build->abandon();

    /// Heartbeat key gone; objects still present (debris — full GC's job).
    EXPECT_FALSE(b->head(heartbeat_key).exists);
    EXPECT_TRUE(b->head(s->layout().blobKey(blob_ref.id)).exists);
    EXPECT_TRUE(b->head(s->layout().treeKey(tree)).exists);

    /// Further operations throw via requireAlive.
    expectThrowsCode(DB::ErrorCodes::LOGICAL_ERROR,
        [&] { build->putBlob(idOf("after"), BlobSource::fromString("after")); });
    expectThrowsCode(DB::ErrorCodes::LOGICAL_ERROR, [&] { build->putTree({entry}); });
    expectThrowsCode(DB::ErrorCodes::LOGICAL_ERROR,
        [&] { build->publish(RootNamespace("ns"), "ref", tree, RefPayload{}); });
}

TEST(CasBuild, PublishHappyPathRoundTrip)
{
    auto b = std::make_shared<InMemoryBackend>();
    auto s = openStore(b);
    auto build = s->startBuild({});
    auto blob = build->putBlob(idOf("hello world"), BlobSource::fromString("hello world"));
    EXPECT_EQ(blob.size, 11u);

    std::vector<TreeEntry> entries;
    TreeEntry e;
    e.name = "data.bin";
    e.placement = Placement::Blob;
    e.file_hash = u128Of("hello world");
    e.file_size = 11;
    entries.push_back(e);
    auto tree = build->putTree(entries);

    RefPayload payload;
    payload.mutable_files["txn_version.txt"] = "1";
    build->publish(RootNamespace{"srv1/tbl"}, "part_1", tree, payload);

    auto r = s->resolveRef(RootNamespace{"srv1/tbl"}, "part_1");
    ASSERT_TRUE(r.has_value());
    EXPECT_EQ(r->tree_id, tree);
    EXPECT_EQ(r->mutable_files.at("txn_version.txt"), "1");

    auto read = s->readTree(tree);
    ASSERT_EQ(read.size(), 1u);
    auto loc = s->locate(read[0]);
    auto got = b->get(loc.key, Range{loc.offset, loc.length});
    ASSERT_TRUE(got.has_value());
    EXPECT_EQ(got->bytes, "hello world");

    /// journal: read the shard manifest raw, assert journal.back() == {Add, "part_1", tree, at_version}
    /// with at_version == shard_version.
    const uint64_t shard = shardOfForTest("part_1", s->poolMeta().root_shards);
    auto manifest = b->get(s->layout().rootShardKey(RootNamespace{"srv1/tbl"}, shard));
    ASSERT_TRUE(manifest.has_value());
    const RootShard root = decodeRootShard(manifest->bytes);
    ASSERT_FALSE(root.journal.empty());
    const JournalRecord & last = root.journal.back();
    EXPECT_EQ(last.op, JournalRecord::Op::Add);
    EXPECT_EQ(last.ref_name, "part_1");
    EXPECT_EQ(u128ToHex(last.tree_id), tree.string());
    EXPECT_EQ(last.at_version, root.shard_version);
}

TEST(CasBuild, PublishRequiresTreeInDepSet)
{
    auto b = std::make_shared<InMemoryBackend>();
    auto s = openStore(b);
    auto build = s->startBuild({});

    /// A TreeId this build never built or adopted → LOGICAL_ERROR (root not in the W-DEP-SET).
    const TreeId stranger{u128ToHex(u128Of("nope"))};
    expectThrowsCode(DB::ErrorCodes::LOGICAL_ERROR,
        [&] { build->publish(RootNamespace{"srv1/tbl"}, "part_1", stranger, RefPayload{}); });
}

TEST(CasBuild, PublishOwnThreadConflictRetries)
{
    auto b = std::make_shared<InMemoryBackend>();
    auto s = openStore(b);
    auto build = s->startBuild({});

    build->putBlob(idOf("hello world"), BlobSource::fromString("hello world"));
    TreeEntry e;
    e.name = "data.bin";
    e.placement = Placement::Blob;
    e.file_hash = u128Of("hello world");
    e.file_size = 11;
    auto tree = build->putTree({e});

    /// One artificial Conflict on the shard's first casPut (no fence advance; the view is untouched).
    /// mutateShard re-reads + re-runs the lambda and lands on the retry.
    const uint64_t shard = shardOfForTest("part_1", s->poolMeta().root_shards);
    b->failNextCasPut(s->layout().rootShardKey(RootNamespace{"srv1/tbl"}, shard));

    build->publish(RootNamespace{"srv1/tbl"}, "part_1", tree, RefPayload{});

    auto r = s->resolveRef(RootNamespace{"srv1/tbl"}, "part_1");
    ASSERT_TRUE(r.has_value());
    EXPECT_EQ(r->tree_id, tree);
}

TEST(CasBuild, PublishIntoSecondNamespaceSameTree)
{
    auto b = std::make_shared<InMemoryBackend>();
    auto s = openStore(b);
    auto build = s->startBuild({});

    auto blob = build->putBlob(idOf("hello world"), BlobSource::fromString("hello world"));
    TreeEntry e;
    e.name = "data.bin";
    e.placement = Placement::Blob;
    e.file_hash = u128Of("hello world");
    e.file_size = 11;
    auto tree = build->putTree({e});

    const String blob_key = s->layout().blobKey(blob.id);
    const String tree_key = s->layout().treeKey(tree);
    const Token blob_token = b->head(blob_key).token;
    const Token tree_token = b->head(tree_key).token;

    /// The SAME tree published as "part_1" in two namespaces — the second publish must NOT re-upload
    /// (the tree dep is already present); both refs resolve to the same tree, a single object set.
    build->publish(RootNamespace{"srv1/tbl"}, "part_1", tree, RefPayload{});
    build->publish(RootNamespace{"srv1/tbl/detached"}, "part_1", tree, RefPayload{});

    auto r1 = s->resolveRef(RootNamespace{"srv1/tbl"}, "part_1");
    auto r2 = s->resolveRef(RootNamespace{"srv1/tbl/detached"}, "part_1");
    ASSERT_TRUE(r1.has_value());
    ASSERT_TRUE(r2.has_value());
    EXPECT_EQ(r1->tree_id, tree);
    EXPECT_EQ(r2->tree_id, tree);

    /// The blob/tree objects were uploaded once: their tokens are unchanged after both publishes.
    EXPECT_EQ(b->head(blob_key).token, blob_token);
    EXPECT_EQ(b->head(tree_key).token, tree_token);
}

TEST(CasBuild, TwoBuildsPublishToSameShardSerialize)
{
    auto b = std::make_shared<InMemoryBackend>();
    auto s = openStore(b);
    const uint64_t root_shards = s->poolMeta().root_shards;

    /// Find two distinct ref names that map to the SAME shard.
    String ref1;
    String ref2;
    {
        std::map<uint64_t, String> seen;
        for (char c = 'a'; c <= 'z' && ref2.empty(); ++c)
        {
            const String name(1, c);
            const uint64_t sh = shardOfForTest(name, root_shards);
            auto it = seen.find(sh);
            if (it != seen.end())
            {
                ref1 = it->second;
                ref2 = name;
            }
            else
            {
                seen.emplace(sh, name);
            }
        }
    }
    ASSERT_FALSE(ref1.empty());
    ASSERT_FALSE(ref2.empty());
    ASSERT_EQ(shardOfForTest(ref1, root_shards), shardOfForTest(ref2, root_shards));

    const RootNamespace ns{"srv1/tbl"};

    /// Build A publishes ref1.
    auto build_a = s->startBuild({});
    build_a->putBlob(idOf("content-a"), BlobSource::fromString("content-a"));
    TreeEntry ea;
    ea.name = "data.bin";
    ea.placement = Placement::Blob;
    ea.file_hash = u128Of("content-a");
    ea.file_size = 9;
    auto tree_a = build_a->putTree({ea});
    build_a->publish(ns, ref1, tree_a, RefPayload{});

    /// Build B publishes ref2 into the same shard: its mutateShard sees A's manifest (shard_version
    /// advanced past 0), re-reads, and lands. The single artificial conflict forces B to genuinely
    /// re-read after the first attempt.
    auto build_b = s->startBuild({});
    build_b->putBlob(idOf("content-b"), BlobSource::fromString("content-b"));
    TreeEntry eb;
    eb.name = "data.bin";
    eb.placement = Placement::Blob;
    eb.file_hash = u128Of("content-b");
    eb.file_size = 9;
    auto tree_b = build_b->putTree({eb});

    const uint64_t shard = shardOfForTest(ref2, root_shards);
    b->failNextCasPut(s->layout().rootShardKey(ns, shard));
    build_b->publish(ns, ref2, tree_b, RefPayload{});

    /// Both refs resolve.
    auto r1 = s->resolveRef(ns, ref1);
    auto r2 = s->resolveRef(ns, ref2);
    ASSERT_TRUE(r1.has_value());
    ASSERT_TRUE(r2.has_value());
    EXPECT_EQ(r1->tree_id, tree_a);
    EXPECT_EQ(r2->tree_id, tree_b);

    /// The shared manifest holds both refs and both Add journal records.
    auto manifest = b->get(s->layout().rootShardKey(ns, shard));
    ASSERT_TRUE(manifest.has_value());
    const RootShard root = decodeRootShard(manifest->bytes);
    EXPECT_TRUE(root.refs.contains(ref1));
    EXPECT_TRUE(root.refs.contains(ref2));
    size_t adds = 0;
    for (const JournalRecord & rec : root.journal)
        if (rec.op == JournalRecord::Op::Add)
            ++adds;
    EXPECT_EQ(adds, 2u);
}

TEST(CasBuild, FirstPublishRegistersNamespace)
{
    /// W-REGISTER (spec section 5, decision 2026-06-12): the first publish into a namespace
    /// CAS-appends it to roots/_registry BEFORE the manifest exists; later publishes into the same
    /// namespace hit the Store's monotone cache and leave the registry untouched.
    auto b = std::make_shared<InMemoryBackend>();
    auto s = Store::open(b, PoolConfig{.pool_prefix = "p"});
    auto build = s->startBuild({});
    auto blob = build->putBlob(idOf("reg-payload"), BlobSource::fromString("reg-payload"));
    std::vector<TreeEntry> entries;
    TreeEntry e;
    e.name = "f";
    e.placement = Placement::Blob;
    e.file_hash = u128Of("reg-payload");
    e.file_size = 11;
    entries.push_back(e);
    auto tree = build->putTree(entries);

    EXPECT_FALSE(b->get(s->layout().rootsRegistryKey()).has_value());
    build->publish(RootNamespace{"srv9/fresh"}, "part_1", tree, RefPayload{});

    const auto got = b->get(s->layout().rootsRegistryKey());
    ASSERT_TRUE(got.has_value());
    const RootsRegistry registry = decodeRootsRegistry(got->bytes);
    EXPECT_TRUE(registry.namespaces.contains("srv9/fresh"));
    const uint64_t version_after_first = registry.registry_version;

    /// second publish into the same namespace: cache hit, registry untouched
    build->publish(RootNamespace{"srv9/fresh"}, "part_2", tree, RefPayload{});
    const RootsRegistry again = decodeRootsRegistry(b->get(s->layout().rootsRegistryKey())->bytes);
    EXPECT_EQ(again.registry_version, version_after_first);
}
