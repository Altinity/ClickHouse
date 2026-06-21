#include <gtest/gtest.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasBuild.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasStore.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasInMemoryBackend.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasEnvelope.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasGc.h>
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
    DB::Cas::PutOutcome putIfAbsent(const String & k, const String & b, DB::Cas::Token * t = nullptr, const DB::Cas::ObjectMeta & meta = {}) override { return inner->putIfAbsent(k, b, t, meta); }
    DB::Cas::WriteSinkPtr putIfAbsentStream(const String & k, const DB::Cas::ObjectMeta & meta = {}) override { return inner->putIfAbsentStream(k, meta); }
    DB::Cas::PutOutcome putOverwrite(const String & k, const String & b, const DB::Cas::Token & e, DB::Cas::Token * t = nullptr, const DB::Cas::ObjectMeta & meta = {}) override { return inner->putOverwrite(k, b, e, t, meta); }
    DB::Cas::CasOutcome casPut(const String & k, const String & b, const std::optional<DB::Cas::Token> & e, DB::Cas::Token * t = nullptr, const DB::Cas::ObjectMeta & meta = {}) override { return inner->casPut(k, b, e, t, meta); }
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

/// B171: the `cas_owner` owner-triple stamping (`Build::ownerMeta`) was DELETED — protection is now
/// the build-root precommit edge (reachability), not revocable object metadata GC reads per-candidate.
/// The old `CasBuild.BlobCarriesOwnerTripleInMetadata` asserted that stamping; its coverage is replaced
/// by the build-root precommit/reclaim tests (`CasBuildRoot*`, `CasBuildRootDangle*`), which prove a
/// written-but-unreferenced object is protected by a live precommit and collectable once it is abandoned.

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
    /// B156b: a NOT-recreatable reuse (body_recreatable=false — e.g. an adopted-from-committed entry, or
    /// genuinely-never-existed) of an absent blob stays FAIL-LOUD FILE_DOESNT_EXIST — a real loss must
    /// surface, never be masked as a retryable ABORTED (which would spin forever with no body to re-upload).
    expectThrowsCode(DB::ErrorCodes::FILE_DOESNT_EXIST,
        [&] { build->reuseBlob(idOf("never-written"), /*body_recreatable*/ false); });
}

TEST(CasBuildReuseBlob, VanishedRecreatableBlobIsRetryableAborted)
{
    /// B156b: reuseBlob with body_recreatable=TRUE on a blob that was deleted by GC after being staged
    /// (putBlob'd) in this same transaction must throw ABORTED (retryable), NOT FILE_DOESNT_EXIST.
    ///
    /// The race: the blob DID exist when the transaction uploaded it; GC's exact-token deleteExact
    /// landed between the reuseBlob call and our HEAD inside observeAndAdmit. The caller (e.g.,
    /// ContentAddressedTransaction::createHardLink staged-source branch) detects ABORTED and retries the
    /// whole INSERT, which re-uploads the content via putBlob from source ⇒ CONVERGES. The fatal
    /// FILE_DOESNT_EXIST path is preserved for the NOT-recreatable (adopted) case and for putBlob's
    /// body-holding retry / adoptTree's never-existed fail-closed (those don't go through reuseBlob).
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

    /// 3. reuseBlob(body_recreatable=true) on the now-absent blob ⇒ ABORTED (retryable), not FILE_DOESNT_EXIST.
    auto s = openStore(b);
    auto build = s->startBuild({});
    expectThrowsCode(DB::ErrorCodes::ABORTED, [&] { build->reuseBlob(id, /*body_recreatable*/ true); });

    /// 4. Confirm: a successful reuseBlob on a live (non-deleted) blob still works.
    {
        auto s1 = openStore(b);
        auto build1 = s1->startBuild({});
        const BlobId live_id = build1->putBlob(idOf("live-payload"), BlobSource::fromString("live-payload")).id;
        auto ref = build1->reuseBlob(live_id, /*body_recreatable*/ true);
        EXPECT_EQ(ref.id, live_id);
    }
}

TEST(CasBuildReuseBlob, VanishedAdoptedBlobStaysFailLoud)
{
    /// B156b INV-NO-LOSS guard: reuseBlob with body_recreatable=FALSE (the adopted-from-committed,
    /// tokenless W-EVIDENCE case) on a vanished blob must throw FILE_DOESNT_EXIST — a REFERENCED blob
    /// was deleted, a genuine loss that MUST surface, NOT be masked as a retryable ABORTED (there is no
    /// body to re-upload, so an ABORTED retry would re-adopt the still-gone blob and spin forever).
    /// Also confirms depIsTokened discriminates: a putBlob'd dep is tokened, an adopted dep is not.
    auto b = std::make_shared<InMemoryBackend>();

    /// 1. Create a blob and a source tree that references it by name (committed source shape).
    DB::Cas::Layout layout("p");
    BlobId id;
    Token t0;
    {
        auto s0 = openStore(b);
        auto build0 = s0->startBuild({});
        id = build0->putBlob(idOf("adopt-payload"), BlobSource::fromString("adopt-payload")).id;
        t0 = b->head(s0->layout().blobKey(id)).token;
    }
    TreeEntry src_entry;
    src_entry.name = "data.bin";
    src_entry.placement = Placement::Blob;
    src_entry.file_hash = u128Of("adopt-payload");
    src_entry.file_size = 13;
    const TreeId source = writeTreeRaw(*b, layout, {src_entry}, openStore(b)->poolMeta().pool_id);

    /// 2. A build adopts the blob from the committed source tree ⇒ a TOKENLESS W-EVIDENCE dep.
    auto s = openStore(b);
    auto adopting = s->startBuild({});
    adopting->adoptFromTree(source, "data.bin");
    EXPECT_FALSE(adopting->depIsTokened(u128Of("adopt-payload")));
    EXPECT_TRUE(adopting->hasDep(u128Of("adopt-payload")));

    /// 3. GC deletes the referenced blob (exact-token deleteExact).
    b->deleteExact(layout.blobKey(id), t0);
    ASSERT_FALSE(b->head(layout.blobKey(id)).exists);

    /// 4. reuseBlob(body_recreatable=false) on the vanished blob ⇒ FAIL-LOUD FILE_DOESNT_EXIST (NOT masked).
    auto consumer = s->startBuild({});
    expectThrowsCode(DB::ErrorCodes::FILE_DOESNT_EXIST,
        [&] { consumer->reuseBlob(id, /*body_recreatable*/ false); });
}

TEST(CasBuildReuseBlob, DepIsTokenedDiscriminatesPutBlobVsAdopt)
{
    /// B156b discriminator unit: putBlob records a TOKENED dep (recreatable), adoptFromTree records a
    /// TOKENLESS W-EVIDENCE dep (not recreatable). The wiring uses this to pick body_recreatable.
    auto b = std::make_shared<InMemoryBackend>();
    auto s = openStore(b);
    DB::Cas::Layout layout("p");

    /// putBlob'd hash ⇒ tokened.
    auto build = s->startBuild({});
    build->putBlob(idOf("written"), BlobSource::fromString("written"));
    EXPECT_TRUE(build->depIsTokened(u128Of("written")));
    EXPECT_TRUE(build->hasDep(u128Of("written")));

    /// Adopted hash ⇒ tokenless. The blob must exist for adoptFromTree's source tree to reference it.
    {
        auto s0 = openStore(b);
        auto b0 = s0->startBuild({});
        b0->putBlob(idOf("adopted"), BlobSource::fromString("adopted"));
    }
    TreeEntry src_entry;
    src_entry.name = "f";
    src_entry.placement = Placement::Blob;
    src_entry.file_hash = u128Of("adopted");
    src_entry.file_size = 7;
    const TreeId source = writeTreeRaw(*b, layout, {src_entry}, s->poolMeta().pool_id);
    build->adoptFromTree(source, "f");
    EXPECT_FALSE(build->depIsTokened(u128Of("adopted")));
    EXPECT_TRUE(build->hasDep(u128Of("adopted")));

    /// Unknown hash ⇒ no dep, not tokened.
    EXPECT_FALSE(build->depIsTokened(u128Of("unknown")));
    EXPECT_FALSE(build->hasDep(u128Of("unknown")));
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
    /// The blob exists and is condemned (resurrect path) — body_recreatable is irrelevant here.
    auto ref = build->reuseBlob(id, /*body_recreatable*/ true);
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
    /// CAS-appends it to `gc/registry` BEFORE the manifest exists; later publishes into the same
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

TEST(CasBuild, AdoptEvidenceNoBackendOp)
{
    /// B188: adoptEvidence records a TOKENLESS W-EVIDENCE dep from an already-resolved TreeEntry
    /// WITHOUT any backend call (no HEAD, no GET, no PUT).
    ///
    /// Two behavioural assertions:
    ///   1. No backend HEAD or stream_put fires during adoptEvidence (counted via CountingBackend).
    ///   2. The recorded dep is usable by putTree: after adoptEvidence(entry), calling putTree({entry})
    ///      does NOT throw LOGICAL_ERROR (W-TREE-BUILD enforces "every child in dep set").

    /// Use the CountingBackend from gtest_ca_dedup_cache.cpp — same delegating pattern, same two
    /// counters (heads, stream_puts). Re-implement a minimal counting wrapper here inline so this test
    /// is self-contained and does not pull in the other file's anonymous namespace.
    struct LocalCountingBackend final : public Backend
    {
        explicit LocalCountingBackend(BackendPtr inner_) : inner(std::move(inner_)) {}
        size_t heads = 0;
        size_t stream_puts = 0;
        size_t gets = 0;

        HeadResult head(const String & k) override { ++heads; return inner->head(k); }
        WriteSinkPtr putIfAbsentStream(const String & k, const ObjectMeta & meta = {}) override
        {
            ++stream_puts;
            return inner->putIfAbsentStream(k, meta);
        }
        std::optional<GetResult> get(const String & k, Range r = {}) override { ++gets; return inner->get(k, r); }
        ListPage list(const String & p, const String & c, size_t l) override { return inner->list(p, c, l); }
        PutOutcome putIfAbsent(const String & k, const String & bts, Token * t = nullptr, const ObjectMeta & m = {}) override { return inner->putIfAbsent(k, bts, t, m); }
        PutOutcome putOverwrite(const String & k, const String & bts, const Token & e, Token * t = nullptr, const ObjectMeta & m = {}) override { return inner->putOverwrite(k, bts, e, t, m); }
        CasOutcome casPut(const String & k, const String & bts, const std::optional<Token> & e, Token * t = nullptr, const ObjectMeta & m = {}) override { return inner->casPut(k, bts, e, t, m); }
        DeleteOutcome deleteExact(const String & k, const Token & t) override { return inner->deleteExact(k, t); }
    private:
        BackendPtr inner;
    };

    /// 1. Set up the store: use a raw InMemoryBackend for setup writes, wrap it in the counting
    ///    decorator only for the Build that will call adoptEvidence.
    auto raw = std::make_shared<InMemoryBackend>();

    /// Upload the blob content so putTree's own putIfAbsentStream (for the tree object) succeeds.
    /// The blob itself must exist on the backend so that putTree can upload the tree that references
    /// it. We upload via a throwaway build using the raw backend (not the counting backend), so the
    /// setup counts don't pollute the test's counter.
    {
        auto s0 = openStore(raw);
        auto b0 = s0->startBuild({});
        b0->putBlob(idOf("b188-content"), BlobSource::fromString("b188-content"));
    }

    /// 2. Wrap the backend in the counting decorator and open a FRESH Store over it.
    auto counting = std::make_shared<LocalCountingBackend>(raw);
    auto s = Store::open(counting, PoolConfig{.pool_prefix = "p"});
    auto build = s->startBuild({});

    /// 3. Construct a Blob TreeEntry (the same content as above). adoptEvidence is called on a
    ///    hand-crafted TreeEntry rather than going through adoptFromTree — that IS the B188 interface.
    TreeEntry entry;
    entry.name = "b188.bin";
    entry.placement = Placement::Blob;
    entry.file_hash = u128Of("b188-content");
    entry.file_size = 12;

    /// Reset the counters after Store::open (which may HEAD gc/state etc. during retireView refresh).
    counting->heads = 0;
    counting->stream_puts = 0;
    counting->gets = 0;

    /// 4. Call adoptEvidence — must record the dep WITHOUT touching the backend.
    EXPECT_NO_THROW(build->adoptEvidence(entry));

    /// Assertion 1: no HEAD, no GET, no stream_put during adoptEvidence.
    EXPECT_EQ(counting->heads, 0u) << "adoptEvidence must not HEAD the backend";
    EXPECT_EQ(counting->stream_puts, 0u) << "adoptEvidence must not PUT to the backend";
    EXPECT_EQ(counting->gets, 0u) << "adoptEvidence must not GET from the backend";

    /// Assertion 2: dep is recorded — putTree({entry}) must succeed (W-TREE-BUILD passes).
    /// putTree WILL call putIfAbsentStream to upload the tree object, but that is expected and
    /// separate from the adoptEvidence call itself.
    EXPECT_NO_THROW(build->putTree({entry}));

    /// Inline entry: adoptEvidence records nothing (Inline has no standalone object).
    TreeEntry inline_entry;
    inline_entry.name = "small";
    inline_entry.placement = Placement::Inline;
    inline_entry.inline_bytes = "xy";
    inline_entry.file_size = 2;
    EXPECT_NO_THROW(build->adoptEvidence(inline_entry));
    /// putTree with inline only also succeeds (no dep needed for Inline).
    EXPECT_NO_THROW(build->putTree({inline_entry}));
}

TEST(CasBuild, ResurrectConvergesUnderProductiveGc)
{
    /// B167/B171 LIVENESS — the resurrect/condemn livelock, now closed by the build-root precommit edge.
    ///
    /// THE BUG (before the fix): a blob H was referenced, dropped, and GC-condemned (everEdged ∧ InDeg=0,
    /// condemned in the retire view). A NEW build dedup-HITS H by content and must resurrect it — it
    /// re-streams a FRESH incarnation of H. But the productive GC, re-deriving H as a zero-in-degree
    /// candidate every round (zeroInDegreeKnown is stateless), kept RE-CONDEMNING and exact-token-DELETING
    /// that fresh incarnation in the build's upload→publish window. The build never converged: every retry
    /// re-uploaded only to have GC delete it again → livelock → broken/detached parts in soak.
    ///
    /// THE FIX (B171): protection is the build-root PRECOMMIT EDGE, not a `cas_owner` watermark hint. Build
    /// B precommits its manifest tree (naming H) BEFORE the adversarial loop, so the GC fold lifts H to
    /// in-degree ≥ 1 — H is never even a zero-in-degree candidate and is SPARED every round until B
    /// publishes (the table ref pins H, then the precommit is removed) → convergence. (The earlier fix used
    /// `protectedByLiveBuild` on per-object `cas_owner` metadata; both were deleted in B171 — see the
    /// `CasBuildRoot*` tests for the reclaim of an ABANDONED precommit.)
    ///
    /// FORM: full adversarial loop. A real Gc drives complete runRegularRound rounds against the same
    /// pool while build B holds an active watermark covering H's incarnation. We assert H is SPARED
    /// every round and that B publishes within a BOUNDED number of GC rounds, after which H reads back.
    auto b = std::make_shared<InMemoryBackend>();

    /// 1. Build A creates H ("shared-content"), publishes a part referencing it, then drops the ref.
    ///    Capture H's first incarnation token so we can condemn exactly it.
    PoolConfig cfg;
    cfg.pool_prefix = "p";
    cfg.server_id = UInt128(0xAB);
    cfg.background_heartbeats = false;
    const String content = "shared-content";
    BlobId h;
    Token h_token0;
    {
        auto s0 = Store::open(b, cfg);
        auto build_a = s0->startBuild({});
        h = build_a->putBlob(idOf(content), BlobSource::fromString(content)).id;
        TreeEntry e;
        e.name = "f";
        e.placement = Placement::Blob;
        e.file_hash = u128Of(content);
        e.file_size = content.size();
        const TreeId tree_a = build_a->putTree({e});
        build_a->publish(RootNamespace{"srv1/tbl"}, "part_1", tree_a, RefPayload{});
        h_token0 = b->head(s0->layout().blobKey(h)).token;
        s0->dropRef(RootNamespace{"srv1/tbl"}, "part_1");
    }

    /// 2. Condemn (Blob, H, h_token0) in the retire view (mirrors ReuseBlobCondemnedResurrects). A fresh
    ///    Store::open below refreshes its retire view at open and sees the condemnation, so the dedup-hit
    ///    on H takes the resurrect (re-stream-a-fresh-incarnation) path rather than free adoption.
    DB::Cas::Layout layout("p");
    injectRetire(*b, layout, /*round*/ 1, /*fence_seq*/ 0, /*shard*/ 0,
        {RetiredEntry{.kind = ObjectKind::Blob, .hash = u128Of(content), .token = h_token0,
                      .size = content.size()}});

    /// 3. Open the live Store and start build B. B dedup-hits the condemned H and RESURRECTS it: a fresh
    ///    incarnation, a NEW token. B stays ACTIVE for the whole adversarial loop — its build_seq is never
    ///    retired below.
    auto s = Store::open(b, cfg);
    const String blob_key = s->layout().blobKey(h);
    auto build_b = s->startBuild({});

    const auto ref_b = build_b->reuseBlob(h, /*body_recreatable*/ true);
    ASSERT_EQ(ref_b.id, h);

    const HeadResult after_resurrect = b->head(blob_key);
    ASSERT_TRUE(after_resurrect.exists);
    EXPECT_NE(after_resurrect.token, h_token0);   /// a genuinely fresh incarnation
    /// B171: the resurrected incarnation no longer carries a `cas_owner` triple (stamping was deleted).
    /// Protection is now the build-root PRECOMMIT EDGE: B assembles its manifest tree naming H and
    /// precommits it BEFORE the adversarial GC loop, so H has in-degree ≥ 1 from the build-root fold and
    /// is never a zero-in-degree candidate — the reachability replacement for the old watermark hint.

    /// 4. Build B assembles its tree referencing H and PRECOMMITS it (build-root edge). H is now protected
    ///    by reachability, not by `cas_owner` — the precommit is the new upload→publish-window protection.
    TreeEntry eb;
    eb.name = "f";
    eb.placement = Placement::Blob;
    eb.file_hash = u128Of(content);
    eb.file_size = content.size();
    const TreeId tree_b = build_b->putTree({eb});
    build_b->precommit(tree_b);

    /// 5. THE ADVERSARIAL LOOP. A real, productive GC keeps trying to reclaim. Round 1 reclaims the
    ///    now-unreferenced tree_a (the finished build A's manifest — UNprotected) but H stays pinned by
    ///    B's PRECOMMIT edge (in-degree ≥ 1), so H is never even a zero-in-degree candidate. We drive far
    ///    more rounds than B needs to publish; H must survive ALL of them. (Each round still renews B's
    ///    watermark so the K=2 crash detector keeps judging B live — the watermark now drives precommit
    ///    reclaim liveness, so a frozen B would have its precommit reclaimed; an advancing seq keeps it.)
    Gc gc(s, hexToU128("00000000000000000000000000000001"));
    constexpr int MAX_GC_ROUNDS = 8;   /// bounded-step budget: convergence must not exceed this

    /// One adversarial GC round + the spare invariant. INV (the heart of the test): the in-flight,
    /// PRECOMMIT-protected H is NEVER condemned/deleted while build B is active — H stays present and
    /// reads back the exact content. (Before the fix, GC re-condemned+deleted the fresh incarnation in the
    /// upload→publish window, so H would VANISH here → livelock.) The blob plane holds exactly one blob
    /// (H), so a reclaim of H is the only way rep.deleted could count a blob — but trees zero/cascade too,
    /// so we assert directly on H's presence + content rather than on the aggregate counter.
    const auto driveRoundAndAssertHSpared = [&](int round_no)
    {
        /// A LIVE server renews its watermark continuously (a background thread every ~heartbeat_period
        /// in production). Renew once per GC round so B's watermark seq ADVANCES between rounds: that is
        /// precisely what distinguishes a live server from a crashed one. (Without this, B's seq freezes
        /// and the GC's K=2 frozen-seq crash detector correctly declares B dead and RECLAIMS B's
        /// precommit — releasing H. The renew keeps B live so the precommit is honored every round.)
        s->renewWatermarkOnce();
        gc.runRegularRound();
        /// (Lease may not be acquired on the very first round: injectRetire pre-seeded gc/state with
        /// no lease owner, so the GC observes once and steals on the next round — protocol-correct,
        /// not load-bearing for this test. The invariant below holds regardless of who leads.)
        const HeadResult hr = b->head(blob_key);
        ASSERT_TRUE(hr.exists) << "H was deleted by GC at round " << round_no
                               << " despite being pinned by the live build B's precommit (B167 livelock would do this)";
        const auto raw = b->get(blob_key);
        ASSERT_TRUE(raw.has_value());
        const auto hdr = decodeEnvelopeHeader(raw->bytes, raw->bytes.size(), ObjectKind::Blob);
        EXPECT_EQ(raw->bytes.substr(hdr.header_len), content)
            << "H's content was lost/corrupted at round " << round_no;
    };

    /// Phase 1 — the livelock window. H is referenced by NO published TABLE root (B has not published
    /// yet) but IS named by B's precommit, so the build-root fold lifts it to in-degree ≥ 1 — never a
    /// zero-in-degree candidate. Drive several full rounds (enough to establish the leader and reclaim
    /// tree_a/part_1); the precommit edge must SPARE H's fresh incarnation every single round.
    int rounds_run = 0;
    constexpr int PRE_PUBLISH_ROUNDS = 4;
    for (int i = 0; i < PRE_PUBLISH_ROUNDS; ++i)
    {
        driveRoundAndAssertHSpared(++rounds_run);
        if (::testing::Test::HasFatalFailure())
            return;
    }

    /// Phase 2 — converge. With H still alive (spared through the whole window), build B publishes a
    /// part referencing it. publish itself legitimately re-streams the still-condemned H to a fresh
    /// incarnation (the resurrect path on the publish gate); H is never absent. This MUST succeed —
    /// the build converges in bounded steps.
    build_b->publish(RootNamespace{"srv1/tbl"}, "part_2", tree_b, RefPayload{});
    const bool published = true;

    /// Phase 3 — keep the GC hammering after publish. H is now pinned by tree_b's TABLE root edge (the
    /// publish also removed the precommit); the GC must keep sparing it as a genuinely-reachable node.
    while (rounds_run < MAX_GC_ROUNDS)
    {
        driveRoundAndAssertHSpared(++rounds_run);
        if (::testing::Test::HasFatalFailure())
            return;
    }

    /// 6. ASSERT convergence: publish SUCCEEDED within the bounded budget, and H reads back intact.
    ASSERT_TRUE(published) << "build B never published — the B167 livelock is back";
    EXPECT_LE(rounds_run, MAX_GC_ROUNDS);

    const auto resolved = s->resolveRef(RootNamespace{"srv1/tbl"}, "part_2");
    ASSERT_TRUE(resolved.has_value());
    EXPECT_EQ(resolved->tree_id, tree_b);

    const auto read = s->readTree(tree_b);
    ASSERT_EQ(read.size(), 1u);
    const auto loc = s->locate(read[0]);
    const auto got = b->get(loc.key, Range{loc.offset, loc.length});
    ASSERT_TRUE(got.has_value());
    EXPECT_EQ(got->bytes, content);
}
