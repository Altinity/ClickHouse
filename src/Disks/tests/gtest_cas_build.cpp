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
}

using namespace DB::Cas;
using DB::Cas::tests::expectThrowsCode;
using DB::Cas::tests::idOf;
using DB::Cas::tests::injectRetire;
using DB::Cas::tests::u128Of;
using DB::Cas::tests::writeTreeRaw;

namespace
{

StorePtr openStore(const std::shared_ptr<InMemoryBackend> & b)
{
    return Store::open(b, PoolConfig{.pool_prefix = "p"});
}

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
    expectThrowsCode(DB::ErrorCodes::FILE_DOESNT_EXIST, [&] { build->reuseBlob(idOf("never-written")); });
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
