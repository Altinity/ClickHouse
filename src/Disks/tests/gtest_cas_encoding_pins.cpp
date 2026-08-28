#include <gtest/gtest.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Formats/CasRefLogFormat.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Formats/CasRefSnapshotFormat.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Formats/CasRecordStreamFormat.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Primitives/CasTypes.h>
#include <IO/WriteBufferFromString.h>
#include <fmt/format.h>

using namespace DB;
using namespace DB::Cas;

/// These literals pin the CANONICAL BYTES of the CAS text encoders. Canonical text is byte-compared
/// on retries and deterministic adoption, and the incremental ref budget counters assume these
/// exact sizes. Update an expected string only for an intentional format change.

TEST(CASEncodingPins, RefLogTxnAllOpKinds)
{
    RefLogTxn txn;
    txn.ns = "roots/pin";
    txn.txn_id = RefTxnId{7, 9};

    RefOp birth;
    birth.kind = RefOpKind::NamespaceBirth;
    txn.ops.push_back(birth);

    RefOp transition;
    transition.kind = RefOpKind::OwnerTransition;
    transition.old_binding = RefOwnerBinding{RefOwnerKind::Precommit, "20260101_0_1_1_1", ManifestRef{1, 2, 3}};
    transition.new_binding = RefOwnerBinding{RefOwnerKind::Committed, "20260101_0_1_1_1", ManifestRef{1, 2, 3}};
    txn.ops.push_back(transition);

    RefOp set_published_at;
    set_published_at.kind = RefOpKind::SetPublishedAt;
    /// NOTE the split literals: "\x01" "e" (else the hex escape would swallow the 'e') and
    /// "\xA8" "f" (else it would swallow the 'f'). `checkCanonicalRefName` forbids '\\' and NUL but
    /// not quote/newline/control bytes/U+2028, so `ref_name` -- the only free-form string `RefOp`
    /// still carries now that `payload` is gone -- exercises quote, newline, a bare control byte,
    /// and the three-byte U+2028 sequence. Backslash escaping is pinned separately, over an
    /// unrestricted string, by `gtest_cas_json_writer.cpp`'s `CASJsonWriterEscaping` suite.
    set_published_at.ref_name = String("20260101_0_1_1_1\"c\nd") + "\x01" "e" + "\xE2\x80\xA8" "f";
    set_published_at.expected_manifest_ref = ManifestRef{1, 2, 3};
    set_published_at.published_at_ms = 1234;
    txn.ops.push_back(set_published_at);

    RefOp removal;
    removal.kind = RefOpKind::RemoveNamespace;
    txn.ops.push_back(removal);

    const String expected = fmt::format("{{\"type\":\"cas_ref_log\",\"version\":{}}}\n", currentCompatibilityVersion()) +
        "{\"namespace\":\"roots/pin\",\"writer_epoch\":\"7\",\"ref_sequence\":\"9\"}\n"
        "{\"operation\":\"namespace_birth\"}\n"
        "{\"operation\":\"owner_transition\",\"old_binding_kind\":\"precommit\",\"old_ref_name\":\"20260101_0_1_1_1\","
        "\"old_writer_epoch\":\"1\",\"old_build_sequence\":\"2\",\"old_manifest_ordinal\":3,\"new_binding_kind\":\"committed\",\"new_ref_name\":\"20260101_0_1_1_1\","
        "\"new_writer_epoch\":\"1\",\"new_build_sequence\":\"2\",\"new_manifest_ordinal\":3}\n"
        "{\"operation\":\"set_published_at\",\"ref_name\":\"20260101_0_1_1_1\\\"c\\nd\\u0001e\\u2028f\","
        "\"writer_epoch\":\"1\",\"build_sequence\":\"2\",\"manifest_ordinal\":3,\"published_at_ms\":1234}\n"
        "{\"operation\":\"remove_namespace\"}\n"
        "{\"record_count\":4}\n";
    EXPECT_EQ(encodeRefLogTxn(txn), expected);
}

TEST(CASEncodingPins, RefSnapshotLive)
{
    RefTableSnapshot snap;
    snap.ns = "roots/pin";
    snap.snapshot_id = RefTxnId{7, 9};

    RefCommittedRow row;
    row.ref_name = "20260101_0_1_1_1";
    row.manifest_ref = ManifestRef{1, 2, 3};
    row.published_at_ms = 5;
    snap.committed.push_back(row);

    snap.precommits.push_back(RefOwnerBinding{RefOwnerKind::Precommit, "20260102_0_2_2_2", ManifestRef{4, 5, 6}});

    const String expected = fmt::format("{{\"type\":\"cas_ref_snap\",\"version\":{}}}\n", currentCompatibilityVersion()) +
        "{\"namespace\":\"roots/pin\",\"writer_epoch\":\"7\",\"ref_sequence\":\"9\",\"lifecycle\":\"live\"}\n"
        "{\"kind\":\"c\",\"ref_name\":\"20260101_0_1_1_1\",\"writer_epoch\":\"1\",\"build_sequence\":\"2\",\"manifest_ordinal\":3,\"published_at_ms\":5}\n"
        "{\"kind\":\"p\",\"ref_name\":\"20260102_0_2_2_2\",\"writer_epoch\":\"4\",\"build_sequence\":\"5\",\"manifest_ordinal\":6}\n"
        "{\"record_count\":2}\n";
    EXPECT_EQ(encodeRefTableSnapshot(snap), expected);
}

TEST(CASEncodingPins, SourceEdgeRunLines)
{
    WriteBufferFromOwnString out;
    SourceEdgeRunWriter writer(out);

    SourceEdgeRecord active;
    active.ref = BlobRef{BlobHashAlgo::CityHash128, BlobDigest::fromU128(UInt128(2))};
    active.source_id = UInt128(5);
    active.marker = kEdgeActive;
    writer.append(active);

    writer.finish();
    out.finalize();

    /// The exact `blob` rendering (algo byte + digest hex) is pinned as a whole line; the point is
    /// that Task 8's line-scratch rewrite must reproduce it byte-for-byte.
    const String text = out.str();
    const String header = fmt::format("{{\"type\":\"cas_run\",\"version\":{},\"kind\":\"source_edge\"}}\n", currentCompatibilityVersion());
    const String expected_record =
        "{\"blob_ref\":\"0100000000000000000000000000000002\",\"source_id\":\"00000000000000000000000000000005\",\"marker\":\"edge\"}\n";
    const String trailer = "{\"record_count\":1}\n";
    /// There is exactly one record, so the whole buffer must be byte-identical to header + record + trailer.
    const String expected_full = header + expected_record + trailer;
    EXPECT_EQ(text, expected_full) << text;
}
