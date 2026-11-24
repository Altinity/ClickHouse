#include <gtest/gtest.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Tools/CasInspect.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Formats/CasBlobMetaFormat.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Formats/CasFoldSealFormat.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Formats/CasLayout.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Formats/CasRefCkptFormat.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Formats/CasRefLogFormat.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Formats/CasRefSnapshotFormat.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Formats/CasTextFormat.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Gc/CasBlobInDegree.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Primitives/CasTypes.h>
#include <IO/WriteBufferFromString.h>
#include "cas_test_helpers.h"

#include <algorithm>
#include <sstream>

using namespace DB::Cas;

namespace
{

ManifestRef manifestRef(uint64_t epoch, uint64_t seq, uint32_t ordinal)
{
    return ManifestRef{epoch, seq, ordinal};
}

BlobRef bh(uint64_t n)
{
    return BlobRef{BlobHashAlgo::CityHash128, BlobDigest::fromU128(UInt128(n))};
}

}

/// Stage-1 T12 (spec §4 "RefOp payload removal"): `cas-inspect` renders the renamed `SetPublishedAt`
/// op kind, and neither the ref-log nor the ref-snapshot rendering carries a `payload_size` key --
/// `RefOp`/`RefCommittedRow` no longer have a `payload` field to size.

TEST(CASInspect, RendersSetPublishedAtOpWithNoPayloadSizeKey)
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

    const String key = layout.refLogKey(DB::Cas::tests::fixture::fixtureLife(ns), id);
    const String bytes = sealObject(FormatId::RefLog, encodeRefLogTxn(txn));

    const String json = caInspectToJson(layout, key, bytes, DB::Cas::tests::fixture::fixtureLife(ns));
    EXPECT_NE(json.find(R"("kind":"set_published_at")"), String::npos) << json;
    EXPECT_EQ(json.find("payload"), String::npos) << json;
}

/// Task-1 review finding M5: `cas inspect` renders the new `EpochSeal` op kind and the txn-level
/// `prev_epoch_seal` chain field, needed to debug INV-2 seal chains without a raw byte dump.
TEST(CASInspect, RendersEpochSealTxnWithPrevEpochSeal)
{
    const Layout layout("p");
    const RootNamespace ns{"srv1/db/tbl"};
    const RefTxnId id{3, 1};

    RefLogTxn txn;
    txn.ns = ns.string();
    txn.txn_id = id;
    txn.prev_epoch_seal = RefTxnId{2, 9};
    RefOp op;
    op.kind = RefOpKind::EpochSeal;
    txn.ops.push_back(op);

    const String key = layout.refLogKey(DB::Cas::tests::fixture::fixtureLife(ns), id);
    /// An old-format seal: only a header below 3 carries one without `next_epoch`.
    const String bytes = sealObject(FormatId::RefLog, encodeRefLogTxnAtGeneration(txn, 1));

    const String json = caInspectToJson(layout, key, bytes, DB::Cas::tests::fixture::fixtureLife(ns));
    EXPECT_NE(json.find(R"("kind":"epoch_seal")"), String::npos) << json;
    EXPECT_NE(json.find(R"("prev_epoch_seal":{"writer_epoch":2,"ref_sequence":9})"), String::npos) << json;
}

/// The remaining two `RefOpKind` words this file's other tests do not exercise: a namespace's birth
/// record and its removal terminator.
TEST(CASInspect, RendersNamespaceBirthAndRemoveNamespaceOpKinds)
{
    const Layout layout("p");
    const RootNamespace ns{"srv1/db/tbl"};

    RefLogTxn birth_txn;
    birth_txn.ns = ns.string();
    birth_txn.txn_id = RefTxnId{1, 1};
    RefOp birth;
    birth.kind = RefOpKind::NamespaceBirth;
    birth_txn.ops.push_back(birth);
    const String birth_key = layout.refLogKey(DB::Cas::tests::fixture::fixtureLife(ns), birth_txn.txn_id);
    const String birth_bytes = sealObject(FormatId::RefLog, encodeRefLogTxn(birth_txn));
    const String birth_json = caInspectToJson(
        layout, birth_key, birth_bytes, DB::Cas::tests::fixture::fixtureLife(ns));
    EXPECT_NE(birth_json.find(R"("kind":"namespace_birth")"), String::npos) << birth_json;

    RefLogTxn remove_txn;
    remove_txn.ns = ns.string();
    remove_txn.txn_id = RefTxnId{1, 2};
    RefOp remove;
    remove.kind = RefOpKind::RemoveNamespace;
    remove_txn.ops.push_back(remove);
    const String remove_key = layout.refLogKey(DB::Cas::tests::fixture::fixtureLife(ns), remove_txn.txn_id);
    const String remove_bytes = sealObject(FormatId::RefLog, encodeRefLogTxn(remove_txn));
    const String remove_json = caInspectToJson(
        layout, remove_key, remove_bytes, DB::Cas::tests::fixture::fixtureLife(ns));
    EXPECT_NE(remove_json.find(R"("kind":"remove_namespace")"), String::npos) << remove_json;
}

/// `RefOwnerKind` renders as its full wire word (`committed`/`precommit`), not the enumerator spelling,
/// at both binding slots an `owner_transition` op carries.
TEST(CASInspect, RendersRefOwnerKindWireWords)
{
    const Layout layout("p");
    const RootNamespace ns{"srv1/db/tbl"};
    const RefTxnId id{1, 3};

    RefLogTxn txn;
    txn.ns = ns.string();
    txn.txn_id = id;
    RefOp op;
    op.kind = RefOpKind::OwnerTransition;
    op.old_binding = RefOwnerBinding{RefOwnerKind::Committed, "all_1_1_0", manifestRef(1, 1, 1)};
    op.new_binding = RefOwnerBinding{RefOwnerKind::Precommit, "all_1_1_0", manifestRef(1, 1, 1)};
    txn.ops.push_back(op);

    const String key = layout.refLogKey(DB::Cas::tests::fixture::fixtureLife(ns), id);
    const String bytes = sealObject(FormatId::RefLog, encodeRefLogTxn(txn));

    const String json = caInspectToJson(layout, key, bytes, DB::Cas::tests::fixture::fixtureLife(ns));
    EXPECT_NE(json.find(R"("old_binding":{"kind":"committed")"), String::npos) << json;
    EXPECT_NE(json.find(R"("new_binding":{"kind":"precommit")"), String::npos) << json;
}

/// A recorded incarnation's dialect renders as its full wire word; the blob-target-run test below
/// covers `emulated`, so this pins the other two (`etag`/`generation`) via a second condemned-row-only run.
TEST(CASInspect, RendersTokenTypeWireWordsEtagAndGeneration)
{
    const Layout layout("p");

    SourceEdgeRecord etag_rec;
    etag_rec.ref = bh(1);
    etag_rec.source_id = UInt128{0};
    etag_rec.marker = RunMarker::Condemned;
    etag_rec.token = PersistedEtag{"etag", "v-etag"};

    SourceEdgeRecord gen_rec;
    gen_rec.ref = bh(1);
    gen_rec.source_id = UInt128{1};
    gen_rec.marker = RunMarker::Condemned;
    gen_rec.token = PersistedEtag{"generation", "v-gen"};

    DB::WriteBufferFromOwnString out;
    SourceEdgeRunWriter writer(out);
    writer.append(etag_rec);
    writer.append(gen_rec);
    writer.finish();
    out.finalize();
    const String bytes = out.str();

    const String key = layout.blobTargetRunKey(/*generation*/3, /*attempt*/0, /*shard*/0, /*seq*/0);
    const String json = caInspectToJson(layout, key, bytes);
    EXPECT_NE(json.find(R"("type":"etag")"), String::npos) << json;
    EXPECT_NE(json.find(R"("type":"generation")"), String::npos) << json;
}

TEST(CASInspect, RendersCommittedRowWithNoPayloadSizeKey)
{
    const Layout layout("p");
    const RootNamespace ns{"srv1/db/tbl"};
    const RefTxnId id{7, 9};

    RefTableSnapshot snap;
    snap.ns = ns.string();
    snap.snapshot_id = id;
    RefCommittedRow row;
    row.ref_name = "all_1_1_0";
    row.manifest_ref = manifestRef(1, 1, 1);
    row.published_at_ms = 42;
    snap.committed.push_back(row);

    const String key = layout.refSnapshotKey(DB::Cas::tests::fixture::fixtureLife(ns), id);
    const String bytes = sealObject(FormatId::RefSnapshot, encodeRefTableSnapshot(snap));

    const String json = caInspectToJson(layout, key, bytes, DB::Cas::tests::fixture::fixtureLife(ns));
    EXPECT_EQ(json.find("payload"), String::npos) << json;
    EXPECT_EQ(json.find("lifecycle"), String::npos) << json;
    EXPECT_EQ(json.find("remove_txn_id"), String::npos) << json;
    EXPECT_NE(json.find(R"("published_at_ms":42)"), String::npos) << json;
}

/// A blob-target source-edge run segment (`Layout::blobTargetRunKey`) is the ground truth for every
/// in-degree question; `cas-inspect` decodes it with the typed `SourceEdgeRunView` reader (not by hand)
/// and must distinguish an active edge from a condemned sentinel row, decoding the latter's fields.
TEST(CASInspect, RendersBlobTargetRunEdgeAndCondemnedRows)
{
    const Layout layout("p");

    /// `SourceEdgeRunWriter::append` requires non-decreasing `(ref, source_id)` order; the condemned
    /// sentinel sorts first for its blob (source_id 0), and `bh(1) < bh(2)`, so appending in this
    /// order already satisfies it.
    SourceEdgeRecord condemned_rec;
    condemned_rec.ref = bh(1);
    condemned_rec.source_id = UInt128{0};
    condemned_rec.marker = RunMarker::Condemned;
    condemned_rec.delete_pending = true;
    condemned_rec.token = PersistedEtag{"emulated", "etag-1"};
    condemned_rec.size = 123;
    condemned_rec.condemn_round = 7;
    condemned_rec.marker_confirmed = true;
    condemned_rec.standing = true;

    SourceEdgeRecord edge_rec;
    edge_rec.ref = bh(2);
    edge_rec.source_id = UInt128(9);
    edge_rec.marker = RunMarker::Edge;

    DB::WriteBufferFromOwnString out;
    SourceEdgeRunWriter writer(out);
    writer.append(condemned_rec);
    writer.append(edge_rec);
    writer.finish();
    out.finalize();
    const String bytes = out.str();

    const String key = layout.blobTargetRunKey(/*generation*/2, /*attempt*/0, /*shard*/0, /*seq*/0);

    const String json = caInspectToJson(layout, key, bytes);
    EXPECT_NE(json.find(R"("object":"blob_target_run")"), String::npos) << json;
    EXPECT_NE(json.find(R"("generation":2)"), String::npos) << json;
    EXPECT_NE(json.find(R"("kind":"edge")"), String::npos) << json;
    EXPECT_NE(json.find(R"("kind":"condemned")"), String::npos) << json;
    EXPECT_NE(json.find(R"("delete_pending":true)"), String::npos) << json;
    EXPECT_NE(json.find(R"("standing":true)"), String::npos) << json;
    EXPECT_NE(json.find(R"("condemn_round":7)"), String::npos) << json;
    EXPECT_NE(json.find(R"("value":"etag-1")"), String::npos) << json;
    EXPECT_NE(json.find(R"("type":"emulated")"), String::npos) << json;
    EXPECT_NE(json.find(R"("rows":2)"), String::npos) << json;
    EXPECT_NE(json.find(R"("distinct_blobs":2)"), String::npos) << json;
    EXPECT_NE(json.find(R"("edges":1)"), String::npos) << json;
    EXPECT_NE(json.find(R"("condemned":1)"), String::npos) << json;
    EXPECT_NE(json.find(R"("zero_markers":0)"), String::npos) << json;
}

/// Stage A task 5 (spec INV-4): the `_ckpt` renders as its own object kind. It is point-addressed in
/// `cas/ns/state/` with no transaction id, so it has a separate dispatch from stream objects and once
/// fell through to
/// `BAD_ARGUMENTS` for it -- and it is precisely the object an operator reaches for when asking "what
/// is recovery's base" or "why is cleanup not reclaiming anything".
TEST(CASInspect, RendersRefCkptWithEveryFieldPresent)
{
    const Layout layout("p");
    const RootNamespace ns{"srv1/db/tbl"};

    const RefCkpt ckpt{.life_epoch = std::optional<uint64_t>{7},
                       .committed_through = RefTxnId{7, 9},
                       .checkpoint_snapshot_id = RefTxnId{7, 9},
                       .last_epoch_seal = RefTxnId{6, 4}};

    const String json = caInspectToJson(
        layout, layout.refCkptKey(DB::Cas::tests::fixture::fixtureLife(ns)), encodeRefCkptAtGeneration(ckpt, 1),
        DB::Cas::tests::fixture::fixtureLife(ns));
    EXPECT_NE(json.find(R"("object":"ref_ckpt")"), String::npos) << json;
    /// The namespace comes from the KEY: a `_ckpt` body does not name it.
    EXPECT_NE(json.find(R"("namespace":"srv1/db/tbl")"), String::npos) << json;
    EXPECT_NE(json.find(R"("life_epoch":7)"), String::npos) << json;
    EXPECT_NE(json.find(R"("committed_through":{"writer_epoch":7,"ref_sequence":9})"), String::npos) << json;
    EXPECT_NE(json.find(R"("writer_epoch":7,"ref_sequence":9)"), String::npos) << json;
    EXPECT_NE(json.find(R"("writer_epoch":6,"ref_sequence":4)"), String::npos) << json;
}

/// The absences are the interesting readings, so they render as explicit `null`s rather than missing
/// keys: no checkpoint means recovery has no base AND nothing is deletable, which is a very different
/// report from "the key is there and I could not tell you what is in it".
TEST(CASInspect, RendersRefCkptAbsencesAsExplicitNulls)
{
    const Layout layout("p");
    const RootNamespace ns{"srv1/db/fresh"};

    const String json = caInspectToJson(
        layout, layout.refCkptKey(DB::Cas::tests::fixture::fixtureLife(ns)), encodeRefCkptAtGeneration(RefCkpt{}, 1),
        DB::Cas::tests::fixture::fixtureLife(ns));
    EXPECT_NE(json.find(R"("object":"ref_ckpt")"), String::npos) << json;
    EXPECT_NE(json.find(R"("life_epoch":null)"), String::npos) << json;
    EXPECT_NE(json.find(R"("checkpoint_snapshot_id":null)"), String::npos) << json;
    EXPECT_NE(json.find(R"("last_epoch_seal":null)"), String::npos) << json;
}

/// `CoverageClass` renders as its full wire word, not the enumerator's numeric value: `cas-inspect` is
/// exactly the tool an operator reaches for to read a fold seal directly, so a coverage row that still
/// printed a bare integer would send them back to this file's comment to decode it.
TEST(CASInspect, RendersCoverageClassificationWireWords)
{
    const Layout layout("p");
    CasFoldSeal seal;
    seal.generation = 3;
    seal.parent_generation = 2;
    seal.ref_lives[UInt128{1}].coverage = RefCoverage{.classification = CoverageClass::Absent};
    seal.ref_lives[UInt128{2}].coverage = RefCoverage{.classification = CoverageClass::Unchanged};
    seal.ref_lives[UInt128{3}].coverage
        = RefCoverage{.classification = CoverageClass::Folded, .last_folded_ref_id = RefTxnId{1, 1}};
    seal.ref_lives[UInt128{4}].coverage = RefCoverage{
        .classification = CoverageClass::Clamped,
        .hold = RefHold{.reason = HoldReason::GapBelowWitness, .offending_position = RefTxnId{1, 2},
                        .retry_count = 0, .next_retry_round = 1}};

    const String key = layout.foldSealKey(/*generation*/3, /*attempt*/0);
    const String json = caInspectToJson(layout, key, encodeFoldSeal(seal));
    EXPECT_NE(json.find(R"("classification":"absent")"), String::npos) << json;
    EXPECT_NE(json.find(R"("classification":"unchanged")"), String::npos) << json;
    EXPECT_NE(json.find(R"("classification":"folded")"), String::npos) << json;
    EXPECT_NE(json.find(R"("classification":"clamped")"), String::npos) << json;
}

/// A listed physical id cannot supply a namespace. Inspect must receive the unique catalog join, and
/// a different logical spelling at the same id is rejected by the decoded object's own namespace.
TEST(CASInspect, RefObjectRequiresTheExactCatalogResolution)
{
    const Layout layout("p");
    const RootNamespace ns{"srv1/db/tbl"};
    const NamespaceLifeId life = NamespaceLifeId::fromCatalogEntry(ns, UInt128{91});
    RefLogTxn txn;
    txn.ns = ns.string();
    txn.txn_id = RefTxnId{1, 1};
    RefOp birth;
    birth.kind = RefOpKind::NamespaceBirth;
    txn.ops = {birth};
    const String key = layout.refLogKey(life, txn.txn_id);
    const String bytes = sealObject(FormatId::RefLog, encodeRefLogTxn(txn));

    EXPECT_THROW(caInspectToJson(layout, key, bytes), DB::Exception);
    EXPECT_THROW(caInspectToJson(
        layout, key, bytes, NamespaceLifeId::fromCatalogEntry(RootNamespace{"redirected"}, life.incarnation)),
        DB::Exception);
    EXPECT_NO_THROW(caInspectToJson(layout, key, bytes, life));
}

TEST(CASInspect, PrintsGenerationNextEpochAndTheStrayNote)
{
    DB::Cas::tests::ScopedReadCeiling c(3);
    const Layout layout("p");
    const RootNamespace ns{"srv1/db/tbl"};
    const NamespaceLifeId life = DB::Cas::tests::fixture::fixtureLife(ns);

    RefCkpt ckpt = DB::Cas::tests::refCkptOf(1, std::nullopt, RefTxnId{5, 1});
    const String ckpt_json = caInspectToJson(layout, layout.refCkptKey(life), encodeRefCkptAtGeneration(ckpt, 3), life);
    EXPECT_NE(ckpt_json.find("\"generation\":3"), String::npos) << ckpt_json;

    const RefLogTxn tombstone = DB::Cas::tests::tombstoneTxn(ns.string(), RefTxnId{1, 2}, 5);
    const String tombstone_json = caInspectToJson(
        layout, layout.refLogKey(life, tombstone.txn_id), sealObject(FormatId::RefLog, encodeRefLogTxn(tombstone)), life);
    EXPECT_NE(tombstone_json.find("\"next_epoch\":5"), String::npos) << tombstone_json;

    RefLogTxn below;
    below.ns = ns.string();
    below.txn_id = RefTxnId{1, 3};
    below.ops.push_back(DB::Cas::tests::namespaceBirthOp());
    const String below_key = layout.refLogKey(life, below.txn_id);
    const String below_bytes = sealObject(FormatId::RefLog, encodeRefLogTxn(below));
    const String stray_json = caInspectToJson(layout, below_key, below_bytes, life, RefTxnId{5, 1});
    EXPECT_NE(stray_json.find("may be a stray"), String::npos) << stray_json;
    EXPECT_EQ(caInspectToJson(layout, below_key, below_bytes, life, RefTxnId{1, 2}).find("may be a stray"), String::npos);
}

TEST(CASInspect, RendersTheMountLeaseLastUncleanEpoch)
{
    const Layout layout("p");
    MountLease lease;
    lease.server_uuid = DB::UInt128(1);
    lease.write_attempt_id = DB::UInt128(2);
    lease.writer_epoch = 7;
    lease.last_unclean_epoch = 3;
    const String json = caInspectToJson(layout, layout.mountKey("srv1"), encodeMountLease(lease));
    EXPECT_NE(json.find("\"last_unclean_epoch\":3"), String::npos) << json;

    lease.last_unclean_epoch.reset();
    const String older = caInspectToJson(layout, layout.mountKey("srv1"), encodeMountLease(lease));
    EXPECT_NE(older.find("\"last_unclean_epoch\":null"), String::npos) << older;
}

TEST(CASInspect, UndecodableCheckpointCostsOnlyTheStrayLabel)
{
    auto backend = std::make_shared<InMemoryBackend>();
    const Layout layout("p");
    const NamespaceLifeId life = DB::Cas::tests::fixture::fixtureLife(RootNamespace{"srv1/db/tbl"});
    CasRequests requests = DB::Cas::tests::openRequestsForTest(backend);
    CasOperation op = requests.admit();

    std::ostringstream absent_diagnostics;
    EXPECT_EQ(readInspectBaseSnapshotId(op, layout, life, absent_diagnostics), std::nullopt);
    EXPECT_TRUE(absent_diagnostics.str().empty());

    ASSERT_TRUE(std::holds_alternative<Committed>(op.create(layout.refCkptKey(life), "not a checkpoint", Retry::standard())));
    std::ostringstream diagnostics;
    std::optional<RefTxnId> base;
    EXPECT_NO_THROW(base = readInspectBaseSnapshotId(op, layout, life, diagnostics));
    EXPECT_EQ(base, std::nullopt);
    const String printed = diagnostics.str();
    EXPECT_NE(printed.find(layout.refCkptKey(life)), String::npos) << printed;
    EXPECT_EQ(std::count(printed.begin(), printed.end(), '\n'), 1) << printed;
    EXPECT_EQ(std::count(printed.begin(), printed.end(), '\n'), 1) << printed;

    /// A required (`!`-prefixed) key this build does not know raises `UNKNOWN_FORMAT_VERSION`, not `CORRUPTED_DATA`.
    const NamespaceLifeId critical_life = DB::Cas::tests::fixture::fixtureLife(RootNamespace{"srv1/db/critical"});
    String with_critical = encodeRefCkptAtGeneration(RefCkpt{.life_epoch = std::optional<uint64_t>{1}, .committed_through = RefTxnId{1, 1},
                                                             .checkpoint_snapshot_id = RefTxnId{1, 1}, .last_epoch_seal = std::nullopt}, 1);
    with_critical.replace(with_critical.rfind('}'), 1, R"(,"!zz":"1"})");
    ASSERT_TRUE(std::holds_alternative<Committed>(op.create(layout.refCkptKey(critical_life), with_critical, Retry::standard())));
    std::ostringstream critical_diagnostics;
    std::optional<RefTxnId> critical_base;
    EXPECT_NO_THROW(critical_base = readInspectBaseSnapshotId(op, layout, critical_life, critical_diagnostics));
    EXPECT_EQ(critical_base, std::nullopt);
    const String critical_printed = critical_diagnostics.str();
    EXPECT_NE(critical_printed.find(layout.refCkptKey(critical_life)), String::npos) << critical_printed;
    EXPECT_EQ(std::count(critical_printed.begin(), critical_printed.end(), '\n'), 1) << critical_printed;
}

/// A `.meta` key decodes as a legacy blob marker, not as a body: it shares the `blobs/` prefix with the body key.
TEST(CASInspect, RendersCondemnedMarker)
{
    const Layout layout("p");
    const BlobRef ref{BlobHashAlgo::CityHash128, BlobDigest::fromU128(DB::Cas::tests::u128Of("hash-inspect"))};
    const BlobMeta m{.version = 1, .state = MetaState::Condemned, .condemn_round = 9, .size = 123};

    const String json = caInspectToJson(layout, layout.blobMetaKey(ref), encodeBlobMetaForTest(m));
    EXPECT_NE(json.find("\"object\":\"blob_meta\""), String::npos);
    EXPECT_NE(json.find("\"condemned\""), String::npos);
    EXPECT_NE(json.find("\"condemn_round\":9"), String::npos);
    EXPECT_NE(json.find("\"size\":123"), String::npos);
}

TEST(CASInspect, RendersCleanMarker)
{
    const Layout layout("p");
    const BlobRef ref{BlobHashAlgo::CityHash128, BlobDigest::fromU128(DB::Cas::tests::u128Of("hash-inspect-clean"))};
    const BlobMeta m{.version = 1, .state = MetaState::Clean, .condemn_round = 0, .size = 7};

    const String json = caInspectToJson(layout, layout.blobMetaKey(ref), encodeBlobMetaForTest(m));
    EXPECT_NE(json.find("\"clean\""), String::npos);
}
