#include <gtest/gtest.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Tools/CasInspect.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Formats/CasLayout.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Formats/CasRefLogFormat.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Formats/CasRefSnapshotFormat.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Formats/CasTextFormat.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Primitives/CasTypes.h>

using namespace DB::Cas;

namespace
{

ManifestRef manifestRef(uint64_t epoch, uint64_t seq, uint32_t ordinal)
{
    return ManifestRef{epoch, seq, ordinal};
}

}

/// Stage-1 T12 (spec §4 "RefOp payload removal"): `ca-inspect` renders the renamed `SetPublishedAt`
/// op kind, and neither the ref-log nor the ref-snapshot rendering carries a `payload_size` key --
/// `RefOp`/`RefCommittedRow` no longer have a `payload` field to size.

TEST(CasInspect, RendersSetPublishedAtOpWithNoPayloadSizeKey)
{
    const Layout layout("p");
    const RootNamespace ns{"srv1/db/tbl"};
    const RefTxnId id{7, 9};

    RefLogTxn txn;
    txn.ns = ns.string();
    txn.txn_id = id;
    RefOp op;
    op.kind = RefOpKind::SetPublishedAt;
    op.ref_name = "all_1_1_0";
    op.expected_manifest_ref = manifestRef(1, 1, 1);
    op.published_at_ms = 42;
    txn.ops.push_back(op);

    const String key = layout.refLogKey(ns, id);
    const String bytes = sealObject(FormatId::RefLog, encodeRefLogTxn(txn));

    const String json = caInspectToJson(layout, key, bytes);
    EXPECT_NE(json.find(R"("kind":"SetPublishedAt")"), String::npos) << json;
    EXPECT_EQ(json.find("payload"), String::npos) << json;
}

TEST(CasInspect, RendersCommittedRowWithNoPayloadSizeKey)
{
    const Layout layout("p");
    const RootNamespace ns{"srv1/db/tbl"};
    const RefTxnId id{7, 9};

    RefTableSnapshot snap;
    snap.ns = ns.string();
    snap.snapshot_id = id;
    snap.lifecycle = RefLifecycle::Live;
    RefCommittedRow row;
    row.ref_name = "all_1_1_0";
    row.manifest_ref = manifestRef(1, 1, 1);
    row.published_at_ms = 42;
    snap.committed.push_back(row);

    const String key = layout.refSnapshotKey(ns, id);
    const String bytes = sealObject(FormatId::RefSnapshot, encodeRefTableSnapshot(snap));

    const String json = caInspectToJson(layout, key, bytes);
    EXPECT_EQ(json.find("payload"), String::npos) << json;
    EXPECT_NE(json.find(R"("published_at_ms":42)"), String::npos) << json;
}
