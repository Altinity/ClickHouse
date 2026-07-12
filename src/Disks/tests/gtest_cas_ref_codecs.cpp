#include <gtest/gtest.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasRefIds.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasRefLogCodec.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasRefSnapshotCodec.h>
#include <Disks/tests/cas_test_helpers.h>
#include <Common/Exception.h>
#include <random>
#include <vector>

namespace DB::ErrorCodes
{
extern const int CORRUPTED_DATA;
extern const int UNKNOWN_FORMAT_VERSION;
}

using namespace DB::Cas;
using DB::Cas::tests::expectThrowsCode;

namespace
{

ManifestRef manifestRef(uint64_t epoch, uint64_t seq, uint32_t ordinal)
{
    return ManifestRef{epoch, seq, ordinal};
}

}

/// ===================================================================================
/// RefTxnId: render / parse
/// ===================================================================================

TEST(CasRefCodec, RenderCanonicalForm)
{
    EXPECT_EQ(renderRefTxnId(RefTxnId{7, 0x8e}), "0000000000000007-000000000000008e");
    EXPECT_EQ(renderRefTxnId(RefTxnId{1, 1}), "0000000000000001-0000000000000001");
    EXPECT_EQ(renderRefTxnId(RefTxnId{0xffffffffffffffffULL, 0xffffffffffffffffULL}),
        "ffffffffffffffff-ffffffffffffffff");
}

TEST(CasRefCodec, RenderRejectsZeroComponent)
{
    EXPECT_THROW(renderRefTxnId(RefTxnId{0, 1}), DB::Exception);
    EXPECT_THROW(renderRefTxnId(RefTxnId{1, 0}), DB::Exception);
    EXPECT_THROW(renderRefTxnId(RefTxnId{0, 0}), DB::Exception);
}

TEST(CasRefCodec, ParseRoundTrip)
{
    for (const RefTxnId id : {RefTxnId{7, 0x8e}, RefTxnId{1, 1}, RefTxnId{255, 2}, RefTxnId{0x100000000ULL, 3},
                               RefTxnId{0x8000000000000000ULL, 0x8000000000000000ULL},
                               RefTxnId{0xffffffffffffffffULL, 0xffffffffffffffffULL}})
    {
        const String rendered = renderRefTxnId(id);
        const auto parsed = parseRefTxnId(rendered);
        ASSERT_TRUE(parsed.has_value());
        EXPECT_EQ(*parsed, id);
    }
}

TEST(CasRefCodec, ParseRejectsShort)
{
    EXPECT_FALSE(parseRefTxnId("000000000000007-000000000000008e").has_value());   /// 32 chars, one short
    EXPECT_FALSE(parseRefTxnId("7-8e").has_value());
    EXPECT_FALSE(parseRefTxnId("").has_value());
}

TEST(CasRefCodec, ParseRejectsLong)
{
    EXPECT_FALSE(parseRefTxnId("00000000000000007-000000000000008e").has_value());  /// 34 chars, one long
    EXPECT_FALSE(parseRefTxnId("0000000000000007-000000000000008e0").has_value());
}

TEST(CasRefCodec, ParseRejectsUppercase)
{
    EXPECT_FALSE(parseRefTxnId("0000000000000007-00000000000000AE").has_value());
    EXPECT_FALSE(parseRefTxnId("0000000000000007-000000000000008E").has_value());
    EXPECT_FALSE(parseRefTxnId("0000000000000007-00000000000000Ae").has_value());  /// mixed case
}

TEST(CasRefCodec, ParseRejectsZeroComponent)
{
    EXPECT_FALSE(parseRefTxnId("0000000000000000-000000000000008e").has_value());
    EXPECT_FALSE(parseRefTxnId("0000000000000007-0000000000000000").has_value());
    EXPECT_FALSE(parseRefTxnId("0000000000000000-0000000000000000").has_value());
}

TEST(CasRefCodec, ParseRejectsNonHexGarbage)
{
    EXPECT_FALSE(parseRefTxnId("000000000000000g-000000000000008e").has_value());
    EXPECT_FALSE(parseRefTxnId("!!!!!!!!!!!!!!!!-000000000000008e").has_value());
    EXPECT_FALSE(parseRefTxnId("0000000000000007_000000000000008e").has_value());  /// wrong separator
}

TEST(CasRefCodec, ParseRejectsMisplacedSeparator)
{
    /// 17 hex digits then '-' then 15: same total length (33), dash at the wrong index -- the kind of
    /// shape that, read naively without a fixed dash position, could be mistaken for an in-range but
    /// overflowing first component.
    EXPECT_FALSE(parseRefTxnId("00000000000000078-00000000000000e").has_value());
}

TEST(CasRefCodec, OrderMatchesLexicalOrderOfRender)
{
    const std::vector<uint64_t> values{1, 2, 255, 1ULL << 32, 1ULL << 63};
    std::vector<RefTxnId> ids;
    for (uint64_t epoch : values)
        for (uint64_t seq : values)
            ids.push_back(RefTxnId{epoch, seq});

    std::mt19937 rng(42);
    for (int iter = 0; iter < 200; ++iter)
    {
        const RefTxnId & a = ids[rng() % ids.size()];
        const RefTxnId & b = ids[rng() % ids.size()];
        const String ra = renderRefTxnId(a);
        const String rb = renderRefTxnId(b);
        EXPECT_EQ(a < b, ra < rb) << ra << " vs " << rb;
        EXPECT_EQ(a == b, ra == rb);
    }
}

/// ===================================================================================
/// RefLogTxn: round trip
/// ===================================================================================

TEST(CasRefCodec, RoundTripNamespaceBirth)
{
    RefLogTxn txn;
    txn.ns = "srv1/db/table@cas@";
    txn.txn_id = RefTxnId{1, 1};
    RefOp op;
    op.kind = RefOpKind::NamespaceBirth;
    txn.ops.push_back(op);

    const String bytes = encodeRefLogTxn(txn);
    const RefLogTxn decoded = decodeRefLogTxn(bytes, txn.ns, txn.txn_id);
    EXPECT_EQ(decoded, txn);
}

TEST(CasRefCodec, RoundTripRemoveNamespace)
{
    RefLogTxn txn;
    txn.ns = "srv1/db/table@cas@";
    txn.txn_id = RefTxnId{1, 2};
    RefOp op;
    op.kind = RefOpKind::RemoveNamespace;
    txn.ops.push_back(op);

    const String bytes = encodeRefLogTxn(txn);
    const RefLogTxn decoded = decodeRefLogTxn(bytes, txn.ns, txn.txn_id);
    EXPECT_EQ(decoded, txn);
}

TEST(CasRefCodec, RoundTripSetPayload)
{
    RefLogTxn txn;
    txn.ns = "srv1/db/table@cas@";
    txn.txn_id = RefTxnId{3, 5};
    RefOp op;
    op.kind = RefOpKind::SetPayload;
    op.ref_name = "all_1_1_0";
    op.expected_manifest_ref = manifestRef(3, 4, 1);
    op.payload = "mutable-ref-payload-bytes";
    op.published_at_ms = 1717000000000ULL;
    txn.ops.push_back(op);

    const String bytes = encodeRefLogTxn(txn);
    const RefLogTxn decoded = decodeRefLogTxn(bytes, txn.ns, txn.txn_id);
    EXPECT_EQ(decoded, txn);
}

TEST(CasRefCodec, RoundTripSetPayloadEmptyPayload)
{
    RefLogTxn txn;
    txn.ns = "ns";
    txn.txn_id = RefTxnId{1, 1};
    RefOp op;
    op.kind = RefOpKind::SetPayload;
    op.ref_name = "r";
    op.expected_manifest_ref = manifestRef(1, 1, 1);
    op.payload = "";
    op.published_at_ms = 0;
    txn.ops.push_back(op);

    const String bytes = encodeRefLogTxn(txn);
    const RefLogTxn decoded = decodeRefLogTxn(bytes, txn.ns, txn.txn_id);
    EXPECT_EQ(decoded, txn);
}

TEST(CasRefCodec, RoundTripOwnerTransitionAdd)
{
    /// new-only = add: no old_binding, a fresh new_binding.
    RefLogTxn txn;
    txn.ns = "ns";
    txn.txn_id = RefTxnId{1, 1};
    RefOp op;
    op.kind = RefOpKind::OwnerTransition;
    op.new_binding = RefOwnerBinding{RefOwnerKind::Precommit, "all_1_1_0", manifestRef(1, 1, 1)};
    txn.ops.push_back(op);

    const String bytes = encodeRefLogTxn(txn);
    const RefLogTxn decoded = decodeRefLogTxn(bytes, txn.ns, txn.txn_id);
    EXPECT_EQ(decoded, txn);
    ASSERT_TRUE(decoded.ops[0].new_binding.has_value());
    EXPECT_FALSE(decoded.ops[0].old_binding.has_value());
}

TEST(CasRefCodec, RoundTripOwnerTransitionRemoval)
{
    /// old-only = removal: an old_binding, no new_binding.
    RefLogTxn txn;
    txn.ns = "ns";
    txn.txn_id = RefTxnId{1, 1};
    RefOp op;
    op.kind = RefOpKind::OwnerTransition;
    op.old_binding = RefOwnerBinding{RefOwnerKind::Precommit, "all_1_1_0", manifestRef(1, 1, 1)};
    txn.ops.push_back(op);

    const String bytes = encodeRefLogTxn(txn);
    const RefLogTxn decoded = decodeRefLogTxn(bytes, txn.ns, txn.txn_id);
    EXPECT_EQ(decoded, txn);
    EXPECT_FALSE(decoded.ops[0].new_binding.has_value());
    ASSERT_TRUE(decoded.ops[0].old_binding.has_value());
}

TEST(CasRefCodec, RoundTripOwnerTransitionReplace)
{
    /// both present = replace.
    RefLogTxn txn;
    txn.ns = "ns";
    txn.txn_id = RefTxnId{1, 1};
    RefOp op;
    op.kind = RefOpKind::OwnerTransition;
    op.old_binding = RefOwnerBinding{RefOwnerKind::Precommit, "all_1_1_0", manifestRef(1, 1, 1)};
    op.new_binding = RefOwnerBinding{RefOwnerKind::Committed, "all_1_1_0", manifestRef(1, 1, 1)};
    txn.ops.push_back(op);

    const String bytes = encodeRefLogTxn(txn);
    const RefLogTxn decoded = decodeRefLogTxn(bytes, txn.ns, txn.txn_id);
    EXPECT_EQ(decoded, txn);
    ASSERT_TRUE(decoded.ops[0].old_binding.has_value());
    ASSERT_TRUE(decoded.ops[0].new_binding.has_value());
}

TEST(CasRefCodec, RoundTripMultipleOpsInOneTransaction)
{
    RefLogTxn txn;
    txn.ns = "ns";
    txn.txn_id = RefTxnId{9, 100};

    RefOp birth;
    birth.kind = RefOpKind::NamespaceBirth;
    txn.ops.push_back(birth);

    RefOp add;
    add.kind = RefOpKind::OwnerTransition;
    add.new_binding = RefOwnerBinding{RefOwnerKind::Precommit, "a/b/c", manifestRef(9, 1, 1)};
    txn.ops.push_back(add);

    RefOp payload;
    payload.kind = RefOpKind::SetPayload;
    payload.ref_name = "a/b/c";
    payload.expected_manifest_ref = manifestRef(9, 1, 1);
    payload.payload = "x";
    payload.published_at_ms = 42;
    txn.ops.push_back(payload);

    const String bytes = encodeRefLogTxn(txn);
    const RefLogTxn decoded = decodeRefLogTxn(bytes, txn.ns, txn.txn_id);
    EXPECT_EQ(decoded, txn);
    EXPECT_EQ(decoded.ops.size(), 3u);
}

/// ===================================================================================
/// RefLogTxn: validation rejections
/// ===================================================================================

TEST(CasRefCodec, EncodeRejectsZeroTxnId)
{
    RefLogTxn txn;
    txn.ns = "ns";
    txn.txn_id = RefTxnId{0, 1};
    expectThrowsCode(DB::ErrorCodes::CORRUPTED_DATA, [&] { encodeRefLogTxn(txn); });
}

TEST(CasRefCodec, DecodeRejectsFutureFormatVersion)
{
    RefLogTxn txn;
    txn.ns = "ns";
    txn.txn_id = RefTxnId{1, 1};
    RefOp op;
    op.kind = RefOpKind::NamespaceBirth;
    txn.ops.push_back(op);
    String bytes = encodeRefLogTxn(txn);

    /// format_version is the first little-endian u32.
    bytes[0] = 2;
    expectThrowsCode(DB::ErrorCodes::UNKNOWN_FORMAT_VERSION,
        [&] { decodeRefLogTxn(bytes, txn.ns, txn.txn_id); });
}

TEST(CasRefCodec, DecodeRejectsTruncatedBuffer)
{
    RefLogTxn txn;
    txn.ns = "ns";
    txn.txn_id = RefTxnId{1, 1};
    RefOp op;
    op.kind = RefOpKind::SetPayload;
    op.ref_name = "r";
    op.expected_manifest_ref = manifestRef(1, 1, 1);
    op.payload = "some payload";
    txn.ops.push_back(op);
    const String bytes = encodeRefLogTxn(txn);

    expectThrowsCode(DB::ErrorCodes::CORRUPTED_DATA,
        [&] { decodeRefLogTxn(bytes.substr(0, bytes.size() - 3), txn.ns, txn.txn_id); });
}

TEST(CasRefCodec, DecodeRejectsUnknownOpKind)
{
    RefLogTxn txn;
    txn.ns = "ns";
    txn.txn_id = RefTxnId{1, 1};
    RefOp op;
    op.kind = RefOpKind::NamespaceBirth;
    txn.ops.push_back(op);
    String bytes = encodeRefLogTxn(txn);

    /// Layout: u32 ver | u32 ns_len | ns bytes | u64 epoch | u64 seq | u32 op_count | u8 kind ...
    const size_t kind_byte_offset = 4 + 4 + txn.ns.size() + 8 + 8 + 4;
    ASSERT_EQ(static_cast<uint8_t>(bytes[kind_byte_offset]), static_cast<uint8_t>(RefOpKind::NamespaceBirth));
    bytes[kind_byte_offset] = 99;
    expectThrowsCode(DB::ErrorCodes::CORRUPTED_DATA,
        [&] { decodeRefLogTxn(bytes, txn.ns, txn.txn_id); });
}

TEST(CasRefCodec, DecodeRejectsBodyNamespaceMismatch)
{
    RefLogTxn txn;
    txn.ns = "ns-a";
    txn.txn_id = RefTxnId{1, 1};
    RefOp op;
    op.kind = RefOpKind::NamespaceBirth;
    txn.ops.push_back(op);
    const String bytes = encodeRefLogTxn(txn);

    expectThrowsCode(DB::ErrorCodes::CORRUPTED_DATA,
        [&] { decodeRefLogTxn(bytes, "ns-b", txn.txn_id); });
}

TEST(CasRefCodec, DecodeRejectsBodyTxnIdMismatch)
{
    RefLogTxn txn;
    txn.ns = "ns";
    txn.txn_id = RefTxnId{1, 1};
    RefOp op;
    op.kind = RefOpKind::NamespaceBirth;
    txn.ops.push_back(op);
    const String bytes = encodeRefLogTxn(txn);

    expectThrowsCode(DB::ErrorCodes::CORRUPTED_DATA,
        [&] { decodeRefLogTxn(bytes, txn.ns, RefTxnId{1, 2}); });
}

TEST(CasRefCodec, EncodeRejectsEmptyRefName)
{
    RefLogTxn txn;
    txn.ns = "ns";
    txn.txn_id = RefTxnId{1, 1};
    RefOp op;
    op.kind = RefOpKind::SetPayload;
    op.ref_name = "";
    op.expected_manifest_ref = manifestRef(1, 1, 1);
    txn.ops.push_back(op);
    expectThrowsCode(DB::ErrorCodes::CORRUPTED_DATA, [&] { encodeRefLogTxn(txn); });
}

TEST(CasRefCodec, EncodeRejectsDotRefName)
{
    RefLogTxn txn;
    txn.ns = "ns";
    txn.txn_id = RefTxnId{1, 1};
    RefOp op;
    op.kind = RefOpKind::SetPayload;
    op.ref_name = ".";
    op.expected_manifest_ref = manifestRef(1, 1, 1);
    txn.ops.push_back(op);
    expectThrowsCode(DB::ErrorCodes::CORRUPTED_DATA, [&] { encodeRefLogTxn(txn); });
}

TEST(CasRefCodec, EncodeRejectsDotDotSegment)
{
    RefLogTxn txn;
    txn.ns = "ns";
    txn.txn_id = RefTxnId{1, 1};
    RefOp op;
    op.kind = RefOpKind::SetPayload;
    op.ref_name = "a/../b";
    op.expected_manifest_ref = manifestRef(1, 1, 1);
    txn.ops.push_back(op);
    expectThrowsCode(DB::ErrorCodes::CORRUPTED_DATA, [&] { encodeRefLogTxn(txn); });
}

TEST(CasRefCodec, EncodeRejectsRepeatedSeparator)
{
    RefLogTxn txn;
    txn.ns = "ns";
    txn.txn_id = RefTxnId{1, 1};
    RefOp op;
    op.kind = RefOpKind::SetPayload;
    op.ref_name = "a//b";
    op.expected_manifest_ref = manifestRef(1, 1, 1);
    txn.ops.push_back(op);
    expectThrowsCode(DB::ErrorCodes::CORRUPTED_DATA, [&] { encodeRefLogTxn(txn); });
}

TEST(CasRefCodec, EncodeRejectsLeadingSlash)
{
    RefLogTxn txn;
    txn.ns = "ns";
    txn.txn_id = RefTxnId{1, 1};
    RefOp op;
    op.kind = RefOpKind::SetPayload;
    op.ref_name = "/a";
    op.expected_manifest_ref = manifestRef(1, 1, 1);
    txn.ops.push_back(op);
    expectThrowsCode(DB::ErrorCodes::CORRUPTED_DATA, [&] { encodeRefLogTxn(txn); });
}

TEST(CasRefCodec, EncodeRejectsTrailingSlash)
{
    RefLogTxn txn;
    txn.ns = "ns";
    txn.txn_id = RefTxnId{1, 1};
    RefOp op;
    op.kind = RefOpKind::SetPayload;
    op.ref_name = "a/";
    op.expected_manifest_ref = manifestRef(1, 1, 1);
    txn.ops.push_back(op);
    expectThrowsCode(DB::ErrorCodes::CORRUPTED_DATA, [&] { encodeRefLogTxn(txn); });
}

TEST(CasRefCodec, EncodeRejectsBackslash)
{
    RefLogTxn txn;
    txn.ns = "ns";
    txn.txn_id = RefTxnId{1, 1};
    RefOp op;
    op.kind = RefOpKind::SetPayload;
    op.ref_name = "a\\b";
    op.expected_manifest_ref = manifestRef(1, 1, 1);
    txn.ops.push_back(op);
    expectThrowsCode(DB::ErrorCodes::CORRUPTED_DATA, [&] { encodeRefLogTxn(txn); });
}

TEST(CasRefCodec, EncodeRejectsNonCanonicalOwnerBindingRefName)
{
    RefLogTxn txn;
    txn.ns = "ns";
    txn.txn_id = RefTxnId{1, 1};
    RefOp op;
    op.kind = RefOpKind::OwnerTransition;
    op.new_binding = RefOwnerBinding{RefOwnerKind::Precommit, "..", manifestRef(1, 1, 1)};
    txn.ops.push_back(op);
    expectThrowsCode(DB::ErrorCodes::CORRUPTED_DATA, [&] { encodeRefLogTxn(txn); });
}

TEST(CasRefCodec, EncodeRejectsTooManyOps)
{
    RefLogTxn txn;
    txn.ns = "ns";
    txn.txn_id = RefTxnId{1, 1};
    for (size_t i = 0; i < ref_txn_max_ops + 1; ++i)
    {
        RefOp op;
        op.kind = RefOpKind::NamespaceBirth;
        txn.ops.push_back(op);
    }
    expectThrowsCode(DB::ErrorCodes::CORRUPTED_DATA, [&] { encodeRefLogTxn(txn); });
}

TEST(CasRefCodec, EncodeAllowsExactlyMaxOps)
{
    RefLogTxn txn;
    txn.ns = "ns";
    txn.txn_id = RefTxnId{1, 1};
    for (size_t i = 0; i < ref_txn_max_ops; ++i)
    {
        RefOp op;
        op.kind = RefOpKind::NamespaceBirth;
        txn.ops.push_back(op);
    }
    const String bytes = encodeRefLogTxn(txn);
    const RefLogTxn decoded = decodeRefLogTxn(bytes, txn.ns, txn.txn_id);
    EXPECT_EQ(decoded.ops.size(), ref_txn_max_ops);
}

TEST(CasRefCodec, EncodeRejectsOversizedNormalTransaction)
{
    RefLogTxn txn;
    txn.ns = "ns";
    txn.txn_id = RefTxnId{1, 1};
    RefOp op;
    op.kind = RefOpKind::SetPayload;
    op.ref_name = "r";
    op.expected_manifest_ref = manifestRef(1, 1, 1);
    op.payload = String(ref_txn_max_bytes + 1, 'x');
    txn.ops.push_back(op);
    expectThrowsCode(DB::ErrorCodes::CORRUPTED_DATA, [&] { encodeRefLogTxn(txn); });
}

TEST(CasRefCodec, RemovalClassTransactionLiftsByteBudgetAboveNormalLimit)
{
    /// A RemoveNamespace transaction carrying a payload bigger than the NORMAL limit but within the
    /// REMOVAL limit must succeed -- proving the removal-class flag actually lifts the byte budget
    /// rather than merely being ignored.
    RefLogTxn txn;
    txn.ns = "ns";
    txn.txn_id = RefTxnId{1, 1};

    RefOp remove;
    remove.kind = RefOpKind::RemoveNamespace;
    txn.ops.push_back(remove);

    RefOp payload;
    payload.kind = RefOpKind::SetPayload;
    payload.ref_name = "r";
    payload.expected_manifest_ref = manifestRef(1, 1, 1);
    payload.payload = String(ref_txn_max_bytes + 1024, 'x');
    txn.ops.push_back(payload);

    const String bytes = encodeRefLogTxn(txn);
    EXPECT_GT(bytes.size(), ref_txn_max_bytes);
    const RefLogTxn decoded = decodeRefLogTxn(bytes, txn.ns, txn.txn_id);
    EXPECT_EQ(decoded, txn);
}

TEST(CasRefCodec, RemovalClassTransactionStillRejectsBeyondRemovalLimit)
{
    RefLogTxn txn;
    txn.ns = "ns";
    txn.txn_id = RefTxnId{1, 1};

    RefOp remove;
    remove.kind = RefOpKind::RemoveNamespace;
    txn.ops.push_back(remove);

    RefOp payload;
    payload.kind = RefOpKind::SetPayload;
    payload.ref_name = "r";
    payload.expected_manifest_ref = manifestRef(1, 1, 1);
    payload.payload = String(ref_removal_max_bytes + 1, 'x');
    txn.ops.push_back(payload);

    expectThrowsCode(DB::ErrorCodes::CORRUPTED_DATA, [&] { encodeRefLogTxn(txn); });
}

TEST(CasRefCodec, RemovalClassTransactionNotCappedOnOpCount)
{
    /// A removal-class transaction may exceed `ref_txn_max_ops` -- only the (much larger) byte budget
    /// bounds it, per spec ("its operation count is bounded by that byte limit").
    RefLogTxn txn;
    txn.ns = "ns";
    txn.txn_id = RefTxnId{1, 1};

    RefOp remove;
    remove.kind = RefOpKind::RemoveNamespace;
    txn.ops.push_back(remove);
    for (size_t i = 0; i < ref_txn_max_ops + 10; ++i)
    {
        RefOp op;
        op.kind = RefOpKind::NamespaceBirth;
        txn.ops.push_back(op);
    }

    const String bytes = encodeRefLogTxn(txn);
    const RefLogTxn decoded = decodeRefLogTxn(bytes, txn.ns, txn.txn_id);
    EXPECT_EQ(decoded.ops.size(), txn.ops.size());
}

/// ===================================================================================
/// RefTableSnapshot: round trip
/// ===================================================================================

namespace
{

RefTableSnapshot makeLiveSnapshot()
{
    RefTableSnapshot s;
    s.ns = "srv1/db/table@cas@";
    s.snapshot_id = RefTxnId{5, 200};
    s.lifecycle = RefLifecycle::Live;

    RefCommittedRow c1;
    c1.ref_name = "all_1_1_0";
    c1.manifest_ref = manifestRef(5, 10, 1);
    c1.payload = "mutable-ref-bytes-1";
    c1.published_at_ms = 1717000000000ULL;
    s.committed.push_back(c1);

    RefCommittedRow c2;
    c2.ref_name = "all_2_2_0";
    c2.manifest_ref = manifestRef(5, 11, 1);
    c2.payload = "mutable-ref-bytes-2";
    c2.published_at_ms = 1717000000001ULL;
    s.committed.push_back(c2);

    RefOwnerBinding p1{RefOwnerKind::Precommit, "all_3_3_0", manifestRef(5, 12, 1)};
    s.precommits.push_back(p1);

    return s;
}

}

TEST(CasRefSnapshotCodec, RoundTripLive)
{
    const RefTableSnapshot s = makeLiveSnapshot();
    const String bytes = encodeRefTableSnapshot(s);
    const RefTableSnapshot decoded = decodeRefTableSnapshot(bytes, s.ns, s.snapshot_id);
    EXPECT_EQ(decoded, s);
}

TEST(CasRefSnapshotCodec, RoundTripLiveEmpty)
{
    RefTableSnapshot s;
    s.ns = "ns";
    s.snapshot_id = RefTxnId{1, 1};
    s.lifecycle = RefLifecycle::Live;

    const String bytes = encodeRefTableSnapshot(s);
    const RefTableSnapshot decoded = decodeRefTableSnapshot(bytes, s.ns, s.snapshot_id);
    EXPECT_EQ(decoded, s);
    EXPECT_TRUE(decoded.committed.empty());
    EXPECT_TRUE(decoded.precommits.empty());
    EXPECT_FALSE(decoded.remove_txn_id.has_value());
}

TEST(CasRefSnapshotCodec, RoundTripRemoved)
{
    RefTableSnapshot s;
    s.ns = "ns";
    s.snapshot_id = RefTxnId{7, 500};
    s.lifecycle = RefLifecycle::Removed;
    s.remove_txn_id = RefTxnId{7, 500};

    const String bytes = encodeRefTableSnapshot(s);
    const RefTableSnapshot decoded = decodeRefTableSnapshot(bytes, s.ns, s.snapshot_id);
    EXPECT_EQ(decoded, s);
    ASSERT_TRUE(decoded.remove_txn_id.has_value());
    EXPECT_EQ(*decoded.remove_txn_id, (RefTxnId{7, 500}));
}

TEST(CasRefSnapshotCodec, ByteIdenticalReencode)
{
    const RefTableSnapshot s = makeLiveSnapshot();
    const String bytes1 = encodeRefTableSnapshot(s);
    const RefTableSnapshot decoded = decodeRefTableSnapshot(bytes1, s.ns, s.snapshot_id);
    const String bytes2 = encodeRefTableSnapshot(decoded);
    EXPECT_EQ(bytes1, bytes2);
}

TEST(CasRefSnapshotCodec, RoundTripPrecommitsSameNameDifferentManifest)
{
    /// Two builds racing for the same final ref name: same ref_name, different manifest_ref, sorted
    /// by manifest_ref as the tiebreak.
    RefTableSnapshot s;
    s.ns = "ns";
    s.snapshot_id = RefTxnId{1, 1};
    s.precommits.push_back(RefOwnerBinding{RefOwnerKind::Precommit, "same", manifestRef(1, 1, 1)});
    s.precommits.push_back(RefOwnerBinding{RefOwnerKind::Precommit, "same", manifestRef(1, 2, 1)});

    const String bytes = encodeRefTableSnapshot(s);
    const RefTableSnapshot decoded = decodeRefTableSnapshot(bytes, s.ns, s.snapshot_id);
    EXPECT_EQ(decoded, s);
    EXPECT_EQ(decoded.precommits.size(), 2u);
}

/// ===================================================================================
/// RefTableSnapshot: validation rejections
/// ===================================================================================

TEST(CasRefSnapshotCodec, EncodeRejectsZeroSnapshotId)
{
    RefTableSnapshot s;
    s.ns = "ns";
    s.snapshot_id = RefTxnId{0, 1};
    expectThrowsCode(DB::ErrorCodes::CORRUPTED_DATA, [&] { encodeRefTableSnapshot(s); });
}

TEST(CasRefSnapshotCodec, EncodeRejectsRemovedWithoutRemoveTxnId)
{
    RefTableSnapshot s;
    s.ns = "ns";
    s.snapshot_id = RefTxnId{1, 1};
    s.lifecycle = RefLifecycle::Removed;
    expectThrowsCode(DB::ErrorCodes::CORRUPTED_DATA, [&] { encodeRefTableSnapshot(s); });
}

TEST(CasRefSnapshotCodec, EncodeRejectsRemovedWithZeroRemoveTxnId)
{
    RefTableSnapshot s;
    s.ns = "ns";
    s.snapshot_id = RefTxnId{1, 1};
    s.lifecycle = RefLifecycle::Removed;
    s.remove_txn_id = RefTxnId{0, 0};
    expectThrowsCode(DB::ErrorCodes::CORRUPTED_DATA, [&] { encodeRefTableSnapshot(s); });
}

TEST(CasRefSnapshotCodec, EncodeRejectsRemovedWithNonEmptyCommitted)
{
    RefTableSnapshot s;
    s.ns = "ns";
    s.snapshot_id = RefTxnId{1, 1};
    s.lifecycle = RefLifecycle::Removed;
    s.remove_txn_id = RefTxnId{1, 1};
    RefCommittedRow row;
    row.ref_name = "r";
    row.manifest_ref = manifestRef(1, 1, 1);
    s.committed.push_back(row);
    expectThrowsCode(DB::ErrorCodes::CORRUPTED_DATA, [&] { encodeRefTableSnapshot(s); });
}

TEST(CasRefSnapshotCodec, EncodeRejectsRemovedWithNonEmptyPrecommits)
{
    RefTableSnapshot s;
    s.ns = "ns";
    s.snapshot_id = RefTxnId{1, 1};
    s.lifecycle = RefLifecycle::Removed;
    s.remove_txn_id = RefTxnId{1, 1};
    s.precommits.push_back(RefOwnerBinding{RefOwnerKind::Precommit, "r", manifestRef(1, 1, 1)});
    expectThrowsCode(DB::ErrorCodes::CORRUPTED_DATA, [&] { encodeRefTableSnapshot(s); });
}

TEST(CasRefSnapshotCodec, EncodeRejectsLiveWithRemoveTxnIdSet)
{
    RefTableSnapshot s;
    s.ns = "ns";
    s.snapshot_id = RefTxnId{1, 1};
    s.lifecycle = RefLifecycle::Live;
    s.remove_txn_id = RefTxnId{1, 1};
    expectThrowsCode(DB::ErrorCodes::CORRUPTED_DATA, [&] { encodeRefTableSnapshot(s); });
}

TEST(CasRefSnapshotCodec, EncodeRejectsUnsortedCommitted)
{
    RefTableSnapshot s;
    s.ns = "ns";
    s.snapshot_id = RefTxnId{1, 1};
    RefCommittedRow a;
    a.ref_name = "b";
    a.manifest_ref = manifestRef(1, 1, 1);
    RefCommittedRow b;
    b.ref_name = "a";
    b.manifest_ref = manifestRef(1, 2, 1);
    s.committed.push_back(a);
    s.committed.push_back(b);
    expectThrowsCode(DB::ErrorCodes::CORRUPTED_DATA, [&] { encodeRefTableSnapshot(s); });
}

TEST(CasRefSnapshotCodec, EncodeRejectsDuplicateCommittedRefName)
{
    RefTableSnapshot s;
    s.ns = "ns";
    s.snapshot_id = RefTxnId{1, 1};
    RefCommittedRow a;
    a.ref_name = "same";
    a.manifest_ref = manifestRef(1, 1, 1);
    RefCommittedRow b;
    b.ref_name = "same";
    b.manifest_ref = manifestRef(1, 2, 1);
    s.committed.push_back(a);
    s.committed.push_back(b);
    expectThrowsCode(DB::ErrorCodes::CORRUPTED_DATA, [&] { encodeRefTableSnapshot(s); });
}

TEST(CasRefSnapshotCodec, EncodeRejectsUnsortedPrecommits)
{
    RefTableSnapshot s;
    s.ns = "ns";
    s.snapshot_id = RefTxnId{1, 1};
    s.precommits.push_back(RefOwnerBinding{RefOwnerKind::Precommit, "b", manifestRef(1, 1, 1)});
    s.precommits.push_back(RefOwnerBinding{RefOwnerKind::Precommit, "a", manifestRef(1, 2, 1)});
    expectThrowsCode(DB::ErrorCodes::CORRUPTED_DATA, [&] { encodeRefTableSnapshot(s); });
}

TEST(CasRefSnapshotCodec, EncodeRejectsPrecommitsSameNameWrongManifestOrder)
{
    /// Same ref_name but the manifest_ref tiebreak is descending -- must be rejected even though the
    /// names alone look sorted.
    RefTableSnapshot s;
    s.ns = "ns";
    s.snapshot_id = RefTxnId{1, 1};
    s.precommits.push_back(RefOwnerBinding{RefOwnerKind::Precommit, "same", manifestRef(1, 2, 1)});
    s.precommits.push_back(RefOwnerBinding{RefOwnerKind::Precommit, "same", manifestRef(1, 1, 1)});
    expectThrowsCode(DB::ErrorCodes::CORRUPTED_DATA, [&] { encodeRefTableSnapshot(s); });
}

TEST(CasRefSnapshotCodec, EncodeRejectsDuplicatePrecommitBinding)
{
    RefTableSnapshot s;
    s.ns = "ns";
    s.snapshot_id = RefTxnId{1, 1};
    s.precommits.push_back(RefOwnerBinding{RefOwnerKind::Precommit, "same", manifestRef(1, 1, 1)});
    s.precommits.push_back(RefOwnerBinding{RefOwnerKind::Precommit, "same", manifestRef(1, 1, 1)});
    expectThrowsCode(DB::ErrorCodes::CORRUPTED_DATA, [&] { encodeRefTableSnapshot(s); });
}

TEST(CasRefSnapshotCodec, EncodeRejectsNonCanonicalCommittedRefName)
{
    RefTableSnapshot s;
    s.ns = "ns";
    s.snapshot_id = RefTxnId{1, 1};
    RefCommittedRow row;
    row.ref_name = "a/../b";
    row.manifest_ref = manifestRef(1, 1, 1);
    s.committed.push_back(row);
    expectThrowsCode(DB::ErrorCodes::CORRUPTED_DATA, [&] { encodeRefTableSnapshot(s); });
}

TEST(CasRefSnapshotCodec, EncodeRejectsNonCanonicalPrecommitRefName)
{
    RefTableSnapshot s;
    s.ns = "ns";
    s.snapshot_id = RefTxnId{1, 1};
    s.precommits.push_back(RefOwnerBinding{RefOwnerKind::Precommit, "", manifestRef(1, 1, 1)});
    expectThrowsCode(DB::ErrorCodes::CORRUPTED_DATA, [&] { encodeRefTableSnapshot(s); });
}

TEST(CasRefSnapshotCodec, EncodeRejectsPrecommitWrongKind)
{
    RefTableSnapshot s;
    s.ns = "ns";
    s.snapshot_id = RefTxnId{1, 1};
    s.precommits.push_back(RefOwnerBinding{RefOwnerKind::Committed, "r", manifestRef(1, 1, 1)});
    expectThrowsCode(DB::ErrorCodes::CORRUPTED_DATA, [&] { encodeRefTableSnapshot(s); });
}

TEST(CasRefSnapshotCodec, EncodeRejectsZeroManifestRefFields)
{
    {
        RefTableSnapshot s;
        s.ns = "ns";
        s.snapshot_id = RefTxnId{1, 1};
        RefCommittedRow row;
        row.ref_name = "r";
        row.manifest_ref = manifestRef(0, 1, 1);
        s.committed.push_back(row);
        expectThrowsCode(DB::ErrorCodes::CORRUPTED_DATA, [&] { encodeRefTableSnapshot(s); });
    }
    {
        RefTableSnapshot s;
        s.ns = "ns";
        s.snapshot_id = RefTxnId{1, 1};
        RefCommittedRow row;
        row.ref_name = "r";
        row.manifest_ref = manifestRef(1, 1, 0);   /// ordinal 0 is out of range
        s.committed.push_back(row);
        expectThrowsCode(DB::ErrorCodes::CORRUPTED_DATA, [&] { encodeRefTableSnapshot(s); });
    }
}

TEST(CasRefSnapshotCodec, EncodeRejectsOversizedSnapshot)
{
    RefTableSnapshot s;
    s.ns = "ns";
    s.snapshot_id = RefTxnId{1, 1};
    RefCommittedRow row;
    row.ref_name = "r";
    row.manifest_ref = manifestRef(1, 1, 1);
    row.payload = String(ref_snapshot_max_bytes + 1, 'x');
    s.committed.push_back(row);
    expectThrowsCode(DB::ErrorCodes::CORRUPTED_DATA, [&] { encodeRefTableSnapshot(s); });
}

TEST(CasRefSnapshotCodec, DecodeRejectsFutureFormatVersion)
{
    RefTableSnapshot s;
    s.ns = "ns";
    s.snapshot_id = RefTxnId{1, 1};
    String bytes = encodeRefTableSnapshot(s);
    bytes[0] = 2;
    expectThrowsCode(DB::ErrorCodes::UNKNOWN_FORMAT_VERSION,
        [&] { decodeRefTableSnapshot(bytes, s.ns, s.snapshot_id); });
}

TEST(CasRefSnapshotCodec, DecodeRejectsTruncatedBuffer)
{
    const RefTableSnapshot s = makeLiveSnapshot();
    const String bytes = encodeRefTableSnapshot(s);
    expectThrowsCode(DB::ErrorCodes::CORRUPTED_DATA,
        [&] { decodeRefTableSnapshot(bytes.substr(0, bytes.size() - 3), s.ns, s.snapshot_id); });
}

TEST(CasRefSnapshotCodec, DecodeRejectsNamespaceMismatch)
{
    RefTableSnapshot s;
    s.ns = "ns-a";
    s.snapshot_id = RefTxnId{1, 1};
    const String bytes = encodeRefTableSnapshot(s);
    expectThrowsCode(DB::ErrorCodes::CORRUPTED_DATA,
        [&] { decodeRefTableSnapshot(bytes, "ns-b", s.snapshot_id); });
}

TEST(CasRefSnapshotCodec, DecodeRejectsSnapshotIdMismatch)
{
    RefTableSnapshot s;
    s.ns = "ns";
    s.snapshot_id = RefTxnId{1, 1};
    const String bytes = encodeRefTableSnapshot(s);
    expectThrowsCode(DB::ErrorCodes::CORRUPTED_DATA,
        [&] { decodeRefTableSnapshot(bytes, s.ns, RefTxnId{1, 2}); });
}

TEST(CasRefSnapshotCodec, DecodeRejectsUnknownLifecycleByte)
{
    RefTableSnapshot s;
    s.ns = "ns";
    s.snapshot_id = RefTxnId{1, 1};
    String bytes = encodeRefTableSnapshot(s);

    /// Layout: u32 ver | u32 ns_len | ns bytes | u64 epoch | u64 seq | u8 lifecycle
    const size_t lifecycle_offset = 4 + 4 + s.ns.size() + 8 + 8;
    ASSERT_EQ(static_cast<uint8_t>(bytes[lifecycle_offset]), static_cast<uint8_t>(RefLifecycle::Live));
    bytes[lifecycle_offset] = 99;
    expectThrowsCode(DB::ErrorCodes::CORRUPTED_DATA,
        [&] { decodeRefTableSnapshot(bytes, s.ns, s.snapshot_id); });
}

/// ===================================================================================
/// Review follow-ups (T6 review): decode-side name validation, NUL rejection, exact-boundary accepts
/// ===================================================================================

TEST(CasRefCodec, DecodeRejectsNonCanonicalSetPayloadRefName)
{
    /// Encode with a canonical placeholder of the SAME byte length as the patched-in value, then
    /// byte-patch only the name bytes in place (leaving every length prefix and every other field
    /// untouched) so this exercises checkRefName on the DECODE path specifically -- every
    /// EncodeRejects*RefName test above only drives it through the encoder.
    RefLogTxn txn;
    txn.ns = "ns";
    txn.txn_id = RefTxnId{1, 1};
    RefOp op;
    op.kind = RefOpKind::SetPayload;
    op.ref_name = "zz";
    op.expected_manifest_ref = manifestRef(1, 1, 1);
    op.payload = "p";
    txn.ops.push_back(op);
    String bytes = encodeRefLogTxn(txn);

    /// Layout up to the name: u32 ver | u32 ns_len | ns bytes | u64 epoch | u64 seq | u32 op_count |
    /// u8 kind | u32 name_len | name bytes.
    const size_t name_bytes_offset = 4 + 4 + txn.ns.size() + 8 + 8 + 4 + 1 + 4;
    ASSERT_EQ(bytes.substr(name_bytes_offset, 2), "zz");
    bytes[name_bytes_offset] = '.';
    bytes[name_bytes_offset + 1] = '.';
    expectThrowsCode(DB::ErrorCodes::CORRUPTED_DATA, [&] { decodeRefLogTxn(bytes, txn.ns, txn.txn_id); });
}

TEST(CasRefCodec, EncodeRejectsEmbeddedNulRefName)
{
    RefLogTxn txn;
    txn.ns = "ns";
    txn.txn_id = RefTxnId{1, 1};
    RefOp op;
    op.kind = RefOpKind::SetPayload;
    op.ref_name = String("a\0b", 3);   /// embedded NUL byte -- never legitimate in a ref name
    op.expected_manifest_ref = manifestRef(1, 1, 1);
    txn.ops.push_back(op);
    expectThrowsCode(DB::ErrorCodes::CORRUPTED_DATA, [&] { encodeRefLogTxn(txn); });
}

TEST(CasRefCodec, EncodeAllowsExactlyMaxBytesNormalTransaction)
{
    RefLogTxn txn;
    txn.ns = "ns";
    txn.txn_id = RefTxnId{1, 1};
    RefOp op;
    op.kind = RefOpKind::SetPayload;
    op.ref_name = "r";
    op.expected_manifest_ref = manifestRef(1, 1, 1);
    op.payload = "";
    txn.ops.push_back(op);

    const size_t base_size = encodeRefLogTxn(txn).size();
    ASSERT_LE(base_size, ref_txn_max_bytes);
    txn.ops[0].payload = String(ref_txn_max_bytes - base_size, 'x');

    const String bytes = encodeRefLogTxn(txn);
    EXPECT_EQ(bytes.size(), ref_txn_max_bytes);
    const RefLogTxn decoded = decodeRefLogTxn(bytes, txn.ns, txn.txn_id);
    EXPECT_EQ(decoded, txn);
}

TEST(CasRefCodec, EncodeAllowsExactlyMaxRemovalBytes)
{
    RefLogTxn txn;
    txn.ns = "ns";
    txn.txn_id = RefTxnId{1, 1};
    RefOp remove;
    remove.kind = RefOpKind::RemoveNamespace;
    txn.ops.push_back(remove);
    RefOp payload_op;
    payload_op.kind = RefOpKind::SetPayload;
    payload_op.ref_name = "r";
    payload_op.expected_manifest_ref = manifestRef(1, 1, 1);
    payload_op.payload = "";
    txn.ops.push_back(payload_op);

    const size_t base_size = encodeRefLogTxn(txn).size();
    ASSERT_LE(base_size, ref_removal_max_bytes);
    txn.ops[1].payload = String(ref_removal_max_bytes - base_size, 'x');

    const String bytes = encodeRefLogTxn(txn);
    EXPECT_EQ(bytes.size(), ref_removal_max_bytes);
    const RefLogTxn decoded = decodeRefLogTxn(bytes, txn.ns, txn.txn_id);
    EXPECT_EQ(decoded, txn);
}

TEST(CasRefSnapshotCodec, DecodeRejectsNonCanonicalCommittedRefName)
{
    RefTableSnapshot s;
    s.ns = "ns";
    s.snapshot_id = RefTxnId{1, 1};
    RefCommittedRow row;
    row.ref_name = "zz";
    row.manifest_ref = manifestRef(1, 1, 1);
    s.committed.push_back(row);
    String bytes = encodeRefTableSnapshot(s);

    /// Layout up to the name: u32 ver | u32 ns_len | ns bytes | u64 epoch | u64 seq | u8 lifecycle |
    /// u32 n_committed | u32 name_len | name bytes (first committed row).
    const size_t name_bytes_offset = 4 + 4 + s.ns.size() + 8 + 8 + 1 + 4 + 4;
    ASSERT_EQ(bytes.substr(name_bytes_offset, 2), "zz");
    bytes[name_bytes_offset] = '.';
    bytes[name_bytes_offset + 1] = '.';
    expectThrowsCode(DB::ErrorCodes::CORRUPTED_DATA,
        [&] { decodeRefTableSnapshot(bytes, s.ns, s.snapshot_id); });
}

TEST(CasRefSnapshotCodec, EncodeAllowsExactlySnapshotMaxBytes)
{
    RefTableSnapshot s;
    s.ns = "ns";
    s.snapshot_id = RefTxnId{1, 1};
    RefCommittedRow row;
    row.ref_name = "r";
    row.manifest_ref = manifestRef(1, 1, 1);
    row.payload = "";
    s.committed.push_back(row);

    const size_t base_size = encodeRefTableSnapshot(s).size();
    ASSERT_LE(base_size, ref_snapshot_max_bytes);
    s.committed[0].payload = String(ref_snapshot_max_bytes - base_size, 'x');

    const String bytes = encodeRefTableSnapshot(s);
    EXPECT_EQ(bytes.size(), ref_snapshot_max_bytes);
    const RefTableSnapshot decoded = decodeRefTableSnapshot(bytes, s.ns, s.snapshot_id);
    EXPECT_EQ(decoded, s);
}
