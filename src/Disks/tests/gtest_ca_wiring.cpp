#include <gtest/gtest.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/PartPathParser.h>

/// M-W wiring tier (design 2026-06-11 section 7 tier 3): the ClickHouse-facing translation layer
/// tested through its own seams. Task 1: PartPathParser — the path-classification rows ported from
/// the PoC's gtest_content_addressed_metadata.cpp (the behavior that survived the SQL suites) plus
/// the shadow/detached/mutable rows the later tasks route on.

using namespace DB::ContentAddressed;

TEST(CaPartPathParser, ParsePartFilePathAtomic)
{
    auto file = parsePartFilePath("uui/uuid-1/all_1_1_0/columns.txt");
    ASSERT_TRUE(file.has_value());
    EXPECT_EQ(file->table_uuid, "uuid-1");
    EXPECT_EQ(file->part_name, "all_1_1_0");
    EXPECT_EQ(file->file, "columns.txt");
    EXPECT_TRUE(file->backup_name.empty());
    EXPECT_TRUE(file->shadow_table_dir.empty());

    auto part_dir = parsePartFilePath("uui/uuid-1/all_1_1_0/"); // trailing slash, no file
    ASSERT_TRUE(part_dir.has_value());
    EXPECT_EQ(part_dir->part_name, "all_1_1_0");
    EXPECT_TRUE(part_dir->file.empty());

    EXPECT_FALSE(parsePartFilePath("uui/uuid-1").has_value()); // table dir, not a part
    EXPECT_FALSE(parsePartFilePath("123").has_value());        // shallower

    // The real-server shape carries a leading store/; the uuid-pair anchor makes it equivalent.
    auto atomic = parsePartFilePath("store/uui/uuid-1/all_1_1_0/data.bin");
    ASSERT_TRUE(atomic.has_value());
    EXPECT_EQ(atomic->table_uuid, "uuid-1");
    EXPECT_EQ(atomic->part_name, "all_1_1_0");
    EXPECT_EQ(atomic->file, "data.bin");
}

TEST(CaPartPathParser, ParsePartFilePathProjectionSubPath)
{
    // A projection file keeps its FULL in-part relative path as the file (the tree entry name).
    auto proj = parsePartFilePath("uui/uuid-1/all_1_1_0/p.proj/data.bin");
    ASSERT_TRUE(proj.has_value());
    EXPECT_EQ(proj->part_name, "all_1_1_0");
    EXPECT_EQ(proj->file, "p.proj/data.bin");
}

TEST(CaPartPathParser, ParsePartFilePathNonAtomic)
{
    // Non-Atomic (Ordinary/Memory/Lazy) layout: data/<db>/<table>/<part>/<file> — no uuid anchor;
    // the part dir is recognized by its block-range suffix (B40).
    auto file = parsePartFilePath("data/memory_01069/mt/all_1_1_0/data.cmrk4");
    ASSERT_TRUE(file.has_value());
    EXPECT_EQ(file->table_uuid, "data/memory_01069/mt");
    EXPECT_EQ(file->part_name, "all_1_1_0");
    EXPECT_EQ(file->file, "data.cmrk4");

    // Temporary/operation prefixes keep the suffix and stay part dirs.
    auto tmp = parsePartFilePath("data/memory_01069/mt/tmp_insert_all_1_1_0/data.cmrk4");
    ASSERT_TRUE(tmp.has_value());
    EXPECT_EQ(tmp->part_name, "tmp_insert_all_1_1_0");

    // Mutation-level form <partition>_<min>_<max>_<level>_<mutation>.
    auto mut = parsePartFilePath("data/db/tbl/20200101_1_1_0_5/data.bin");
    ASSERT_TRUE(mut.has_value());
    EXPECT_EQ(mut->part_name, "20200101_1_1_0_5");

    // A non-Atomic table-level file is NOT a part file.
    EXPECT_FALSE(isPartFilePath("data/memory_01069/mt/format_version.txt"));
    auto tf = parseTableFilePath("data/memory_01069/mt/format_version.txt");
    ASSERT_TRUE(tf.has_value());
    EXPECT_EQ(tf->table_uuid, "data/memory_01069/mt");
    EXPECT_EQ(tf->tail, "format_version.txt");

    EXPECT_EQ(parseTableUuid("data/memory_01069/mt"), std::optional<std::string>("data/memory_01069/mt"));

    // Generic disk-root files classify as nothing (verbatim passthrough).
    EXPECT_FALSE(isPartFilePath("clickhouse_access_check_xyz"));
    EXPECT_FALSE(parseTableFilePath("clickhouse_access_check_xyz").has_value());
    EXPECT_FALSE(parseTableUuid("clickhouse_access_check_xyz").has_value());
}

TEST(CaPartPathParser, ParseTableUuid)
{
    EXPECT_EQ(parseTableUuid("uui/uuid-1/"), std::optional<std::string>("uuid-1"));
    EXPECT_EQ(parseTableUuid("uui/uuid-1"), std::optional<std::string>("uuid-1"));
    EXPECT_FALSE(parseTableUuid("uui/uuid-1/all_1_1_0").has_value()); // part dir, not table dir

    EXPECT_TRUE(endsWithTableUuidPair("store/uui/uuid-1"));
    EXPECT_FALSE(endsWithTableUuidPair("store/uui/uuid-1/all_1_1_0"));
    EXPECT_FALSE(endsWithTableUuidPair("shadow/bk1/store"));
}

TEST(CaPartPathParser, ParseTableFilePathNested)
{
    // The reserved deduplication_logs/ subdir is a table-level namespace, never a part dir.
    EXPECT_FALSE(isPartFilePath("uui/uuid-1/deduplication_logs/deduplication_log_1.txt"));
    auto tf = parseTableFilePath("uui/uuid-1/deduplication_logs/deduplication_log_1.txt");
    ASSERT_TRUE(tf.has_value());
    EXPECT_EQ(tf->table_uuid, "uuid-1");
    EXPECT_EQ(tf->tail, "deduplication_logs/deduplication_log_1.txt");

    auto flat = parseTableFilePath("uui/uuid-1/format_version.txt");
    ASSERT_TRUE(flat.has_value());
    EXPECT_EQ(flat->tail, "format_version.txt");

    EXPECT_FALSE(parseTableFilePath("uui/uuid-1").has_value());
    EXPECT_FALSE(parseTableFilePath("uui/uuid-1/").has_value());

    EXPECT_TRUE(isPartFilePath("uui/uuid-1/all_1_1_0/data.bin"));
}

TEST(CaPartPathParser, ShadowFreezePaths)
{
    EXPECT_TRUE(isShadowPath("shadow/bk1/store/uui/uuid-1/all_1_1_0/data.bin"));
    EXPECT_TRUE(isShadowPath("/shadow/bk1"));
    EXPECT_FALSE(isShadowPath("store/uui/uuid-1/all_1_1_0/data.bin"));
    EXPECT_FALSE(isShadowPath("shadowy/bk1"));

    auto s = parsePartFilePath("shadow/bk1/store/uui/uuid-1/all_1_1_0/data.bin");
    ASSERT_TRUE(s.has_value());
    EXPECT_EQ(s->table_uuid, "uuid-1");
    EXPECT_EQ(s->part_name, "all_1_1_0");
    EXPECT_EQ(s->file, "data.bin");
    EXPECT_EQ(s->backup_name, "bk1");
    EXPECT_EQ(s->shadow_table_dir, "shadow/bk1/store/uui/uuid-1");
}

TEST(CaPartPathParser, DetachedPathsReportTheSharedDetachedComponent)
{
    // The PoC contract (B36): "detached" parses as the part_name; the real detached part dir is
    // the first component of `file`. The transaction/read routing re-splits on this shape.
    auto d = parsePartFilePath("uui/uuid-1/detached/attaching_all_0_0_0/metadata_version.txt");
    ASSERT_TRUE(d.has_value());
    EXPECT_EQ(d->table_uuid, "uuid-1");
    EXPECT_EQ(d->part_name, std::string(kDetachedDirName));
    EXPECT_EQ(d->file, "attaching_all_0_0_0/metadata_version.txt");
}

TEST(CaPartPathParser, MutablePerPartFiles)
{
    EXPECT_TRUE(isMutablePerPartFile("uuid.txt"));
    EXPECT_TRUE(isMutablePerPartFile("txn_version.txt"));
    EXPECT_TRUE(isMutablePerPartFile("metadata_version.txt"));
    // Atomic-write sibling tmp (VersionMetadataOnDisk) stages as mutable too.
    EXPECT_TRUE(isMutablePerPartFile("txn_version.txt.tmp"));
    // Detached form: prefixed by the detached part dir, matched on the basename (B62).
    EXPECT_TRUE(isMutablePerPartFile("attaching_all_0_0_0/metadata_version.txt"));
    EXPECT_FALSE(isMutablePerPartFile("columns.txt"));
    EXPECT_FALSE(isMutablePerPartFile("data.bin"));
    EXPECT_FALSE(isMutablePerPartFile("uuid.txt2"));
}

/// ==== M-W Task 2: the read side over Cas::Store ====
/// Fixture: publish parts through the CORE API, then read through the IMetadataStorage surface of
/// the rewritten ContentAddressedMetadataStorage (real ctor over a Local object storage; the
/// backend self-selects EmulatedSingleProcess token semantics).

#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/ContentAddressedMetadataStorage.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasBuild.h>
#include <Disks/tests/cas_test_helpers.h>
#include <IO/ReadHelpers.h>
#include <IO/ReadPipeline.h>

using DB::Cas::tests::idOf;
using DB::Cas::tests::u128Of;

namespace
{

DB::Cas::TreeEntry wiringBlobEntry(const String & name, const String & payload)
{
    DB::Cas::TreeEntry e;
    e.name = name;
    e.placement = DB::Cas::Placement::Blob;
    e.file_hash = u128Of(payload);
    e.file_size = payload.size();
    return e;
}

std::shared_ptr<DB::ContentAddressedMetadataStorage> openWiringStorage()
{
    auto storage = std::make_shared<DB::ContentAddressedMetadataStorage>(
        DB::Cas::tests::makeLocalObjectStorageForTest(), "pool", "srv1",
        std::filesystem::temp_directory_path() / "ca_wiring_scratch", nullptr);
    storage->startup();
    return storage;
}

/// One part with a content blob, a projection file, and the mutable per-part files, published
/// through the real Build into `ns` under `ref`.
void publishWiredPart(
    DB::ContentAddressedMetadataStorage & storage, const DB::Cas::RootNamespace & ns, const String & ref)
{
    auto build = storage.store()->startBuild({});
    build->putBlob(idOf("payload-A"), DB::Cas::BlobSource::fromString("payload-A"));
    build->putBlob(idOf("payload-B"), DB::Cas::BlobSource::fromString("payload-B"));
    auto tree = build->putTree({wiringBlobEntry("data.bin", "payload-A"), wiringBlobEntry("p.proj/data.bin", "payload-B")});
    DB::Cas::RefPayload payload;
    payload.mutable_files = {{"uuid.txt", "u-123"}, {"metadata_version.txt", "5"}, {".ca_mtime", "1700000000"}};
    build->publish(ns, ref, tree, std::move(payload));
}

}

TEST(CaWiringRead, ResolvesPublishedPart)
{
    auto storage = openWiringStorage();
    publishWiredPart(*storage, storage->liveNamespace("uuid-1"), "all_1_1_0");

    EXPECT_TRUE(storage->existsFile("uui/uuid-1/all_1_1_0/data.bin"));
    EXPECT_FALSE(storage->existsFile("uui/uuid-1/all_1_1_0/missing.bin"));
    EXPECT_EQ(storage->getFileSize("uui/uuid-1/all_1_1_0/data.bin"), 9u);

    EXPECT_TRUE(storage->existsDirectory("uui/uuid-1"));
    EXPECT_TRUE(storage->existsDirectory("uui/uuid-1/all_1_1_0"));
    EXPECT_FALSE(storage->existsDirectory("uui/uuid-1/all_9_9_9"));
    EXPECT_FALSE(storage->existsDirectory("uui/uuid-2"));

    /// Part dir listing: nested keys collapse to their first component; reserved payload keys
    /// (.ca_mtime) never surface; mutable files DO surface.
    auto names = storage->listDirectory("uui/uuid-1/all_1_1_0");
    std::sort(names.begin(), names.end());
    EXPECT_EQ(names, (std::vector<std::string>{"data.bin", "metadata_version.txt", "p.proj", "uuid.txt"}));

    auto parts = storage->listDirectory("uui/uuid-1");
    EXPECT_EQ(parts, (std::vector<std::string>{"all_1_1_0"}));

    /// The part dir reports EMPTY (virtual files; B45) so removeDirectory goes straight to the
    /// ref-unlink; the table dir keeps listing-based emptiness.
    EXPECT_TRUE(storage->isDirectoryEmpty("uui/uuid-1/all_1_1_0"));
    EXPECT_FALSE(storage->isDirectoryEmpty("uui/uuid-1"));

    /// Blob-backed file: a real key, PAYLOAD-sized (the envelope header is a read-path concern).
    auto objects = storage->getStorageObjects("uui/uuid-1/all_1_1_0/data.bin");
    ASSERT_EQ(objects.size(), 1u);
    EXPECT_FALSE(objects[0].remote_path.empty());
    EXPECT_EQ(objects[0].bytes_size, 9u);

    /// Mutable per-part file: bytes live in the shard manifest.
    EXPECT_TRUE(storage->existsFile("uui/uuid-1/all_1_1_0/uuid.txt"));
    EXPECT_EQ(storage->getFileSize("uui/uuid-1/all_1_1_0/uuid.txt"), 5u);
    EXPECT_EQ(storage->tryGetInManifestBytes("uui/uuid-1/all_1_1_0/uuid.txt"), std::optional<String>("u-123"));
    auto mobj = storage->getStorageObjects("uui/uuid-1/all_1_1_0/uuid.txt");
    ASSERT_EQ(mobj.size(), 1u);
    EXPECT_TRUE(mobj[0].remote_path.empty());   /// sized placeholder; bytes ride prepareInManifestRead

    /// The publish stamp (.ca_mtime) backs getLastModified for the part dir and its files.
    EXPECT_EQ(storage->getLastModified("uui/uuid-1/all_1_1_0").epochTime(), 1700000000);
    EXPECT_EQ(storage->getLastModified("uui/uuid-1/all_1_1_0/data.bin").epochTime(), 1700000000);
}

TEST(CaWiringRead, BlobViewPlanRidesTheStandardPipeline)
{
    /// The committed read path (B116): an in-manifest file is served from memory via
    /// prepareInManifestRead; a blob-backed file translates to its physical blob object +
    /// payload window (getBlobViewPlan) and rides the STANDARD object-storage pipeline,
    /// bounded by the FileView stage — composed here the way DiskObjectStorage::prepareRead
    /// composes it.
    auto object_storage = DB::Cas::tests::makeLocalObjectStorageForTest();
    auto storage = std::make_shared<DB::ContentAddressedMetadataStorage>(
        object_storage, "pool", "srv1",
        std::filesystem::temp_directory_path() / "ca_wiring_scratch", nullptr);
    storage->startup();
    publishWiredPart(*storage, storage->liveNamespace("uuid-1"), "all_1_1_0");

    /// In-manifest file: memory source, no blob plan.
    DB::ReadPipeline manifest_pipeline;
    ASSERT_TRUE(storage->prepareInManifestRead("uui/uuid-1/all_1_1_0/uuid.txt", DB::ReadSettings{}, manifest_pipeline));
    String manifest_bytes;
    {
        auto buf = manifest_pipeline.build();
        DB::readStringUntilEOF(manifest_bytes, *buf);
    }
    EXPECT_EQ(manifest_bytes, "u-123");
    EXPECT_FALSE(storage->getBlobViewPlan("uui/uuid-1/all_1_1_0/uuid.txt").has_value());

    /// Blob-backed file: a real physical key and a payload-sized window whose extent equals
    /// the object's readable size (a right-bounded read never overshoots the window).
    const std::string path = "uui/uuid-1/all_1_1_0/data.bin";
    auto plan = storage->getBlobViewPlan(path);
    ASSERT_TRUE(plan.has_value());
    EXPECT_FALSE(plan->object.remote_path.empty());
    EXPECT_EQ(plan->object.local_path, path);
    EXPECT_EQ(plan->payload_end - plan->payload_offset, 9u);
    EXPECT_EQ(plan->object.bytes_size, plan->payload_end);
    EXPECT_FALSE(storage->prepareInManifestRead(path, DB::ReadSettings{}, manifest_pipeline = {}));

    auto make_pipeline = [&]
    {
        DB::ReadPipeline pipeline;
        pipeline.setSource(object_storage, {plan->object}, DB::ReadSettings{});
        pipeline.needGather();
        pipeline.needFileView(path, plan->payload_offset, plan->payload_end);
        return pipeline;
    };
    EXPECT_EQ(make_pipeline().describe(), "Source(ObjectStorage) -> Gather -> FileView");

    {
        auto buf = make_pipeline().build();
        EXPECT_EQ(buf->getFileName(), path);
        EXPECT_EQ(buf->tryGetFileSize(), std::optional<size_t>(9));
        String bytes;
        DB::readStringUntilEOF(bytes, *buf);
        EXPECT_EQ(bytes, "payload-A");
    }

    /// Right-bounded read through the view (the MergeTreeReaderStream::adjustRightMark shape):
    /// the bound is window-relative and forwarded down the chain.
    {
        auto buf = make_pipeline().build();
        buf->setReadUntilPosition(7);
        String head(7, '\0');
        buf->readStrict(head.data(), 7);
        EXPECT_EQ(head, "payload");
        EXPECT_TRUE(buf->eof());
        buf->setReadUntilEnd();
        String tail;
        DB::readStringUntilEOF(tail, *buf);
        EXPECT_EQ(tail, "-A");
    }

    /// Seek inside the window.
    {
        auto buf = make_pipeline().build();
        buf->seek(8, SEEK_SET);
        String last;
        DB::readStringUntilEOF(last, *buf);
        EXPECT_EQ(last, "A");
    }
}

TEST(CaWiringRead, ProjectionDirectory)
{
    auto storage = openWiringStorage();
    publishWiredPart(*storage, storage->liveNamespace("uuid-1"), "all_1_1_0");

    EXPECT_TRUE(storage->existsDirectory("uui/uuid-1/all_1_1_0/p.proj"));
    EXPECT_FALSE(storage->existsDirectory("uui/uuid-1/all_1_1_0/q.proj"));
    EXPECT_EQ(storage->listDirectory("uui/uuid-1/all_1_1_0/p.proj"), (std::vector<std::string>{"data.bin"}));
    EXPECT_TRUE(storage->isDirectoryEmpty("uui/uuid-1/all_1_1_0/p.proj"));   /// B60
    EXPECT_TRUE(storage->existsFile("uui/uuid-1/all_1_1_0/p.proj/data.bin"));
    EXPECT_EQ(storage->getFileSize("uui/uuid-1/all_1_1_0/p.proj/data.bin"), 9u);
}

TEST(CaWiringRead, DetachedFoldedIntoTableNamespace)
{
    auto storage = openWiringStorage();
    /// B181: a detached part is a `detached/`-prefixed ref INSIDE the table's own archive namespace,
    /// not a separate sibling namespace. Publish it that way through the core, and ALSO a live part
    /// that shares the same base name to prove the live↔detached collision is impossible (the ref
    /// names `all_1_1_0` and `detached/all_1_1_0` differ — one namespace, no re-split needed).
    publishWiredPart(*storage, storage->liveNamespace("uuid-1"), "detached/broken_all_1_1_0");
    publishWiredPart(*storage, storage->liveNamespace("uuid-1"), "broken_all_1_1_0");

    /// The TABLE dir collapses the `detached/<part>` refs to the single `detached` subdir entry
    /// alongside the live part name.
    auto top = storage->listDirectory("uui/uuid-1");
    std::sort(top.begin(), top.end());
    EXPECT_EQ(top, (std::vector<std::string>{"broken_all_1_1_0", "detached"}));

    /// The detached CONTAINER lists the detached part DIRECTORY names (B36's intent), prefix-stripped
    /// — and NOT the live part of the same base name.
    EXPECT_TRUE(storage->existsDirectory("uui/uuid-1/detached"));
    EXPECT_EQ(storage->listDirectory("uui/uuid-1/detached"), (std::vector<std::string>{"broken_all_1_1_0"}));
    /// A single detached part dir + its files (the detached part is its own `detached/`-prefixed ref).
    EXPECT_TRUE(storage->existsDirectory("uui/uuid-1/detached/broken_all_1_1_0"));
    auto names = storage->listDirectory("uui/uuid-1/detached/broken_all_1_1_0");
    std::sort(names.begin(), names.end());
    EXPECT_EQ(names, (std::vector<std::string>{"data.bin", "metadata_version.txt", "p.proj", "uuid.txt"}));
    /// The B62 shape: a detached part's mutable file resolves through the `detached/`-prefixed ref.
    EXPECT_TRUE(storage->existsFile("uui/uuid-1/detached/broken_all_1_1_0/metadata_version.txt"));
    EXPECT_EQ(storage->tryGetInManifestBytes("uui/uuid-1/detached/broken_all_1_1_0/metadata_version.txt"),
              std::optional<String>("5"));
    EXPECT_TRUE(storage->existsFile("uui/uuid-1/detached/broken_all_1_1_0/data.bin"));
}

TEST(CaWiringRoute, DetachedFoldsIntoTableNamespaceWithPrefixedRef)
{
    /// B181: a detached part file routes to the table's OWN archive namespace under a
    /// `detached/`-prefixed ref — NOT a separate sibling namespace.
    auto storage = openWiringStorage();
    auto p = parsePartFilePath("store/uui/uuid-1/detached/broken_all_1_1_0/data.bin");
    ASSERT_TRUE(p.has_value());
    auto r = storage->route(*p);
    ASSERT_TRUE(r.has_value());
    EXPECT_EQ(r->ns.string(), storage->liveNamespace("uuid-1").string());
    EXPECT_EQ(r->ref, "detached/broken_all_1_1_0");
    EXPECT_EQ(r->file, "data.bin");

    /// The detached CONTAINER dir routes to the table ns with an empty ref (filtered listing).
    auto pc = parsePartFilePath("store/uui/uuid-1/detached/broken_all_1_1_0");
    ASSERT_TRUE(pc.has_value());
    auto rc = storage->route(*pc);
    ASSERT_TRUE(rc.has_value());
    EXPECT_EQ(rc->ns.string(), storage->liveNamespace("uuid-1").string());
    EXPECT_EQ(rc->ref, "detached/broken_all_1_1_0");
    EXPECT_TRUE(rc->file.empty());
}

TEST(CaWiringRead, ShadowFreezeTree)
{
    auto storage = openWiringStorage();
    publishWiredPart(*storage, DB::ContentAddressedMetadataStorage::shadowNamespace("shadow/bk1/store/uui/uuid-1"), "all_1_1_0");

    /// Intermediate dirs derive from the registered shadow namespaces.
    EXPECT_TRUE(storage->existsDirectory("shadow/bk1"));
    EXPECT_TRUE(storage->existsDirectory("shadow/bk1/store"));
    EXPECT_FALSE(storage->existsDirectory("shadow/bk2"));
    EXPECT_EQ(storage->listDirectory("shadow"), (std::vector<std::string>{"bk1"}));
    EXPECT_EQ(storage->listDirectory("shadow/bk1"), (std::vector<std::string>{"store"}));
    EXPECT_EQ(storage->listDirectory("shadow/bk1/store"), (std::vector<std::string>{"uui"}));
    EXPECT_EQ(storage->listDirectory("shadow/bk1/store/uui"), (std::vector<std::string>{"uuid-1"}));
    /// Shadow TABLE dir (strict uuid-pair anchor) and PART dir.
    EXPECT_TRUE(storage->existsDirectory("shadow/bk1/store/uui/uuid-1"));
    EXPECT_EQ(storage->listDirectory("shadow/bk1/store/uui/uuid-1"), (std::vector<std::string>{"all_1_1_0"}));
    EXPECT_TRUE(storage->existsDirectory("shadow/bk1/store/uui/uuid-1/all_1_1_0"));
    auto names = storage->listDirectory("shadow/bk1/store/uui/uuid-1/all_1_1_0");
    std::sort(names.begin(), names.end());
    EXPECT_EQ(names, (std::vector<std::string>{"data.bin", "metadata_version.txt", "p.proj", "uuid.txt"}));
    EXPECT_TRUE(storage->existsFile("shadow/bk1/store/uui/uuid-1/all_1_1_0/data.bin"));
    EXPECT_EQ(storage->getFileSize("shadow/bk1/store/uui/uuid-1/all_1_1_0/data.bin"), 9u);
}

TEST(CaWiringRead, VerbatimNamespaceFiles)
{
    auto storage = openWiringStorage();
    publishWiredPart(*storage, storage->liveNamespace("uuid-1"), "all_1_1_0");
    storage->store()->putNamespaceFile(storage->liveNamespace("uuid-1"), "format_version.txt", "1\n");
    storage->store()->putNamespaceFile(
        storage->liveNamespace("uuid-1"), "deduplication_logs/deduplication_log_1.txt", "log-bytes");
    /// Loose disk-root files are plain mountpoint objects (design §5.2), not namespace files.
    storage->store()->putMountpointObject(storage->serverId() + "/" + "clickhouse_access_check_xyz", "ok");

    EXPECT_TRUE(storage->existsFile("uui/uuid-1/format_version.txt"));
    EXPECT_EQ(storage->getFileSize("uui/uuid-1/format_version.txt"), 2u);
    EXPECT_EQ(storage->tryGetInManifestBytes("uui/uuid-1/format_version.txt"), std::optional<String>("1\n"));

    /// Table dir listing merges part names + verbatim file first components.
    auto names = storage->listDirectory("uui/uuid-1");
    std::sort(names.begin(), names.end());
    EXPECT_EQ(names, (std::vector<std::string>{"all_1_1_0", "deduplication_logs", "format_version.txt"}));

    /// The reserved table-level subdir.
    EXPECT_TRUE(storage->existsDirectory("uui/uuid-1/deduplication_logs"));
    EXPECT_EQ(storage->listDirectory("uui/uuid-1/deduplication_logs"),
              (std::vector<std::string>{"deduplication_log_1.txt"}));
    EXPECT_TRUE(storage->existsFile("uui/uuid-1/deduplication_logs/deduplication_log_1.txt"));

    /// Loose disk-root files are plain objects — existsFile checks the mountpoint object, not a namespace file.
    EXPECT_TRUE(storage->existsFile("clickhouse_access_check_xyz"));
    /// Loose files are real objects — tryGetInManifestBytes returns nullopt (not in-manifest bytes).
    EXPECT_EQ(storage->tryGetInManifestBytes("clickhouse_access_check_xyz"), std::nullopt);
    EXPECT_EQ(storage->getFileSize("clickhouse_access_check_xyz"), 2u);
    EXPECT_FALSE(storage->existsFile("clickhouse_access_check_other"));
}

/// ==== M-W Task 3: the write path through IMetadataTransaction ====

#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/ContentAddressedTransaction.h>
#include <IO/ReadHelpers.h>

namespace
{

void writeThroughTransaction(DB::IMetadataTransaction & tx, const String & path, const String & bytes)
{
    auto & ca_tx = dynamic_cast<DB::ContentAddressedTransaction &>(tx);
    auto buf = ca_tx.writeFile(path, 65536, DB::WriteMode::Rewrite, {});
    buf->write(bytes.data(), bytes.size());
    buf->finalize();
}

}

TEST(CaWiringWrite, ContentRoundTripThroughTransaction)
{
    auto storage = openWiringStorage();

    auto tx = storage->createTransaction();
    writeThroughTransaction(*tx, "uui/uuid-1/all_1_1_0/data.bin", "content-A");
    writeThroughTransaction(*tx, "uui/uuid-1/all_1_1_0/checksums.txt", "sums");
    writeThroughTransaction(*tx, "uui/uuid-1/all_1_1_0/uuid.txt", "u-42");
    /// Nothing visible before commit.
    EXPECT_FALSE(storage->existsDirectory("uui/uuid-1/all_1_1_0"));
    tx->commit(DB::NoCommitOptions{});

    EXPECT_TRUE(storage->existsDirectory("uui/uuid-1/all_1_1_0"));
    EXPECT_TRUE(storage->existsFile("uui/uuid-1/all_1_1_0/data.bin"));
    EXPECT_EQ(storage->getFileSize("uui/uuid-1/all_1_1_0/data.bin"), 9u);
    EXPECT_EQ(storage->tryGetInManifestBytes("uui/uuid-1/all_1_1_0/uuid.txt"), std::optional<String>("u-42"));
    auto names = storage->listDirectory("uui/uuid-1/all_1_1_0");
    std::sort(names.begin(), names.end());
    EXPECT_EQ(names, (std::vector<std::string>{"checksums.txt", "data.bin", "uuid.txt"}));
    /// The publish stamp was added automatically and is filtered from listings.
    EXPECT_GT(storage->getLastModified("uui/uuid-1/all_1_1_0").epochTime(), 1700000000);
}

TEST(CaWiringWrite, IdenticalContentDedupsToOneBlob)
{
    auto storage = openWiringStorage();

    auto tx = storage->createTransaction();
    writeThroughTransaction(*tx, "uui/uuid-1/all_1_1_0/data.bin", "same-bytes");
    tx->commit(DB::NoCommitOptions{});
    auto tx2 = storage->createTransaction();
    writeThroughTransaction(*tx2, "uui/uuid-1/all_2_2_0/data.bin", "same-bytes");
    tx2->commit(DB::NoCommitOptions{});

    /// Identical content => the SAME blob object (the key is the content hash).
    auto a = storage->getStorageObjects("uui/uuid-1/all_1_1_0/data.bin");
    auto b = storage->getStorageObjects("uui/uuid-1/all_2_2_0/data.bin");
    ASSERT_EQ(a.size(), 1u);
    ASSERT_EQ(b.size(), 1u);
    EXPECT_EQ(a[0].remote_path, b[0].remote_path);
}

TEST(CaWiringWrite, UncommittedTransactionPublishesNothing)
{
    auto storage = openWiringStorage();
    {
        auto tx = storage->createTransaction();
        writeThroughTransaction(*tx, "uui/uuid-1/all_1_1_0/data.bin", "doomed");
        /// destroyed without commit => Build abandoned (uploads are heartbeat-gated debris)
    }
    EXPECT_FALSE(storage->existsDirectory("uui/uuid-1/all_1_1_0"));
    EXPECT_FALSE(storage->existsFile("uui/uuid-1/all_1_1_0/data.bin"));
}

TEST(CaWiringWrite, MutableOnlyUpdateOnCommittedPart)
{
    auto storage = openWiringStorage();
    auto tx = storage->createTransaction();
    writeThroughTransaction(*tx, "uui/uuid-1/all_1_1_0/data.bin", "content-A");
    writeThroughTransaction(*tx, "uui/uuid-1/all_1_1_0/txn_version.txt", "v1");
    tx->commit(DB::NoCommitOptions{});

    /// The MVCC autocommit one-shot shape: a fresh transaction rewriting ONLY a mutable file of a
    /// COMMITTED part goes through updateRefPayload (no tree rebuild, no journal record).
    auto tx2 = storage->createTransaction();
    writeThroughTransaction(*tx2, "uui/uuid-1/all_1_1_0/txn_version.txt", "v2");
    tx2->commit(DB::NoCommitOptions{});

    EXPECT_EQ(storage->tryGetInManifestBytes("uui/uuid-1/all_1_1_0/txn_version.txt"), std::optional<String>("v2"));
    EXPECT_TRUE(storage->existsFile("uui/uuid-1/all_1_1_0/data.bin"));   /// the tree is untouched
}

TEST(CaWiringWrite, VerbatimFilesDurableOnFinalizeAndAppendable)
{
    auto storage = openWiringStorage();
    auto tx = storage->createTransaction();
    /// Verbatim files are durable on FINALIZE, with no commit (the disk layer's autocommit
    /// contract for table-level files).
    writeThroughTransaction(*tx, "uui/uuid-1/mutation_5.txt", "commands\n");
    EXPECT_TRUE(storage->existsFile("uui/uuid-1/mutation_5.txt"));

    /// Append = read-modify-rewrite (the MVCC mutation-entry CSN append).
    {
        auto & ca_tx = dynamic_cast<DB::ContentAddressedTransaction &>(*tx);
        auto buf = ca_tx.writeFile("uui/uuid-1/mutation_5.txt", 65536, DB::WriteMode::Append, {});
        buf->write("csn 42\n", 7);
        buf->finalize();
    }
    EXPECT_EQ(storage->tryGetInManifestBytes("uui/uuid-1/mutation_5.txt"),
              std::optional<String>("commands\ncsn 42\n"));
}

/// ==== M-W Tasks 5-7: carry-forward, renames, removals, detached/ATTACH/FREEZE ====

TEST(CaWiringOps, HardLinkCarriesForwardWithoutReupload)
{
    auto storage = openWiringStorage();
    auto tx = storage->createTransaction();
    writeThroughTransaction(*tx, "uui/uuid-1/all_1_1_0/data.bin", "shared-payload");
    writeThroughTransaction(*tx, "uui/uuid-1/all_1_1_0/uuid.txt", "u-1");
    tx->commit(DB::NoCommitOptions{});

    /// A mutation/merge carries unchanged files into the new part by hardlink.
    auto tx2 = storage->createTransaction();
    tx2->createHardLink("uui/uuid-1/all_1_1_0/data.bin", "uui/uuid-1/all_1_1_5/data.bin");
    tx2->createHardLink("uui/uuid-1/all_1_1_0/uuid.txt", "uui/uuid-1/all_1_1_5/uuid.txt");
    tx2->commit(DB::NoCommitOptions{});

    EXPECT_TRUE(storage->existsFile("uui/uuid-1/all_1_1_5/data.bin"));
    EXPECT_EQ(storage->getStorageObjects("uui/uuid-1/all_1_1_0/data.bin")[0].remote_path,
              storage->getStorageObjects("uui/uuid-1/all_1_1_5/data.bin")[0].remote_path);
    EXPECT_EQ(storage->tryGetInManifestBytes("uui/uuid-1/all_1_1_5/uuid.txt"), std::optional<String>("u-1"));
}

TEST(CaWiringOps, TmpToFinalRenamePublishesUnderFinalName)
{
    auto storage = openWiringStorage();
    auto tx = storage->createTransaction();
    writeThroughTransaction(*tx, "uui/uuid-1/tmp_insert_all_1_1_0/data.bin", "fresh");
    tx->moveDirectory("uui/uuid-1/tmp_insert_all_1_1_0", "uui/uuid-1/all_1_1_0");
    tx->commit(DB::NoCommitOptions{});

    EXPECT_TRUE(storage->existsDirectory("uui/uuid-1/all_1_1_0"));
    EXPECT_FALSE(storage->existsDirectory("uui/uuid-1/tmp_insert_all_1_1_0"));
    EXPECT_TRUE(storage->existsFile("uui/uuid-1/all_1_1_0/data.bin"));
}

TEST(CaWiringOps, CommittedPartRenameMovesTheRef)
{
    auto storage = openWiringStorage();
    auto tx = storage->createTransaction();
    writeThroughTransaction(*tx, "uui/uuid-1/all_1_1_0/data.bin", "bytes");
    tx->commit(DB::NoCommitOptions{});

    /// MergeTree renames a part to delete_tmp_<part> before removing it.
    auto tx2 = storage->createTransaction();
    tx2->moveDirectory("uui/uuid-1/all_1_1_0", "uui/uuid-1/delete_tmp_all_1_1_0");
    tx2->commit(DB::NoCommitOptions{});

    EXPECT_FALSE(storage->existsDirectory("uui/uuid-1/all_1_1_0"));
    EXPECT_TRUE(storage->existsDirectory("uui/uuid-1/delete_tmp_all_1_1_0"));
    EXPECT_TRUE(storage->existsFile("uui/uuid-1/delete_tmp_all_1_1_0/data.bin"));
}

TEST(CaWiringOps, ProjectionTmpRenameRekeysStagedEntries)
{
    auto storage = openWiringStorage();
    auto tx = storage->createTransaction();
    writeThroughTransaction(*tx, "uui/uuid-1/all_1_1_0/data.bin", "main");
    writeThroughTransaction(*tx, "uui/uuid-1/all_1_1_0/p_1.tmp_proj/data.bin", "proj");
    auto & ca_tx = dynamic_cast<DB::ContentAddressedTransaction &>(*tx);
    ca_tx.moveDirectory("uui/uuid-1/all_1_1_0/p_1.tmp_proj", "uui/uuid-1/all_1_1_0/p.proj");
    tx->commit(DB::NoCommitOptions{});

    EXPECT_TRUE(storage->existsFile("uui/uuid-1/all_1_1_0/p.proj/data.bin"));
    EXPECT_FALSE(storage->existsFile("uui/uuid-1/all_1_1_0/p_1.tmp_proj/data.bin"));
    EXPECT_EQ(storage->listDirectory("uui/uuid-1/all_1_1_0/p.proj"), (std::vector<std::string>{"data.bin"}));
}

TEST(CaWiringOps, DetachAttachRoundTrip)
{
    auto storage = openWiringStorage();
    auto tx = storage->createTransaction();
    writeThroughTransaction(*tx, "uui/uuid-1/all_1_1_0/data.bin", "detachable");
    writeThroughTransaction(*tx, "uui/uuid-1/all_1_1_0/metadata_version.txt", "3");
    tx->commit(DB::NoCommitOptions{});

    /// DETACH: a committed part moves into the detached namespace - pure ref ops.
    auto tx2 = storage->createTransaction();
    tx2->moveDirectory("uui/uuid-1/all_1_1_0", "uui/uuid-1/detached/all_1_1_0");
    tx2->commit(DB::NoCommitOptions{});
    EXPECT_FALSE(storage->existsDirectory("uui/uuid-1/all_1_1_0"));
    EXPECT_TRUE(storage->existsDirectory("uui/uuid-1/detached/all_1_1_0"));
    EXPECT_EQ(storage->tryGetInManifestBytes("uui/uuid-1/detached/all_1_1_0/metadata_version.txt"),
              std::optional<String>("3"));

    /// ATTACH: stage-rename within detached, then publish back into the live namespace.
    auto tx3 = storage->createTransaction();
    tx3->moveDirectory("uui/uuid-1/detached/all_1_1_0", "uui/uuid-1/detached/attaching_all_1_1_0");
    tx3->commit(DB::NoCommitOptions{});
    auto tx4 = storage->createTransaction();
    tx4->moveDirectory("uui/uuid-1/detached/attaching_all_1_1_0", "uui/uuid-1/all_2_2_0");
    tx4->commit(DB::NoCommitOptions{});

    EXPECT_TRUE(storage->existsDirectory("uui/uuid-1/all_2_2_0"));
    EXPECT_TRUE(storage->existsFile("uui/uuid-1/all_2_2_0/data.bin"));
    EXPECT_FALSE(storage->existsDirectory("uui/uuid-1/detached/attaching_all_1_1_0"));
    EXPECT_EQ(storage->listDirectory("uui/uuid-1/detached"), (std::vector<std::string>{}));
}

TEST(CaWiringOps, RemovalsDropRefsAndNamespaces)
{
    auto storage = openWiringStorage();
    auto tx = storage->createTransaction();
    writeThroughTransaction(*tx, "uui/uuid-1/all_1_1_0/data.bin", "gone-soon");
    writeThroughTransaction(*tx, "uui/uuid-1/all_2_2_0/data.bin", "stays");
    tx->commit(DB::NoCommitOptions{});
    storage->store()->putNamespaceFile(storage->liveNamespace("uuid-1"), "format_version.txt", "1\n");

    /// The fast-removal path: per-file unlinks are no-ops; removeDirectory(<part>) drops the ref.
    auto tx2 = storage->createTransaction();
    tx2->unlinkFile("uui/uuid-1/all_1_1_0/data.bin", false, false);
    EXPECT_TRUE(storage->existsFile("uui/uuid-1/all_1_1_0/data.bin"));   /// still committed
    tx2->removeDirectory("uui/uuid-1/all_1_1_0");
    EXPECT_FALSE(storage->existsDirectory("uui/uuid-1/all_1_1_0"));
    EXPECT_TRUE(storage->existsDirectory("uui/uuid-1/all_2_2_0"));

    /// DROP TABLE: removeRecursive on the table dir drops the live + detached namespaces.
    tx2->removeRecursive("uui/uuid-1", {});
    EXPECT_FALSE(storage->existsDirectory("uui/uuid-1"));
    EXPECT_FALSE(storage->existsFile("uui/uuid-1/format_version.txt"));
}

TEST(CaWiringOps, MutableTmpMoveOnCommittedPart)
{
    auto storage = openWiringStorage();
    auto tx = storage->createTransaction();
    writeThroughTransaction(*tx, "uui/uuid-1/all_1_1_0/data.bin", "x");
    tx->commit(DB::NoCommitOptions{});

    /// VersionMetadataOnDisk's atomic write: autocommit txn_version.txt.tmp, then a standalone
    /// one-shot moveFile(.tmp -> txn_version.txt).
    auto tx2 = storage->createTransaction();
    writeThroughTransaction(*tx2, "uui/uuid-1/all_1_1_0/txn_version.txt.tmp", "tid-7");
    tx2->commit(DB::NoCommitOptions{});
    auto tx3 = storage->createTransaction();
    tx3->moveFile("uui/uuid-1/all_1_1_0/txn_version.txt.tmp", "uui/uuid-1/all_1_1_0/txn_version.txt");
    tx3->commit(DB::NoCommitOptions{});

    EXPECT_EQ(storage->tryGetInManifestBytes("uui/uuid-1/all_1_1_0/txn_version.txt"), std::optional<String>("tid-7"));
    EXPECT_FALSE(storage->existsFile("uui/uuid-1/all_1_1_0/txn_version.txt.tmp"));

    /// removeTmpMetadataFile: unlink of a committed mutable file publishes its deletion.
    auto tx4 = storage->createTransaction();
    tx4->unlinkFile("uui/uuid-1/all_1_1_0/txn_version.txt", false, false);
    tx4->commit(DB::NoCommitOptions{});
    EXPECT_FALSE(storage->existsFile("uui/uuid-1/all_1_1_0/txn_version.txt"));
}

TEST(CaWiringOps, VerbatimMoveAndUnlink)
{
    auto storage = openWiringStorage();
    auto tx = storage->createTransaction();
    writeThroughTransaction(*tx, "uui/uuid-1/tmp_mutation_5.txt", "cmds");
    auto & ca_tx = dynamic_cast<DB::ContentAddressedTransaction &>(*tx);
    ca_tx.moveFile("uui/uuid-1/tmp_mutation_5.txt", "uui/uuid-1/mutation_5.txt");
    EXPECT_TRUE(storage->existsFile("uui/uuid-1/mutation_5.txt"));
    EXPECT_FALSE(storage->existsFile("uui/uuid-1/tmp_mutation_5.txt"));
    ca_tx.unlinkFile("uui/uuid-1/mutation_5.txt", false, false);
    EXPECT_FALSE(storage->existsFile("uui/uuid-1/mutation_5.txt"));
}

TEST(CaWiringOps, TableRenameMovesRefsFilesAndDetached)
{
    auto storage = openWiringStorage();
    auto tx = storage->createTransaction();
    writeThroughTransaction(*tx, "uui/uuid-1/all_1_1_0/data.bin", "live");
    tx->commit(DB::NoCommitOptions{});
    auto tx2 = storage->createTransaction();
    tx2->moveDirectory("uui/uuid-1/all_1_1_0", "uui/uuid-1/detached/all_1_1_0");   /// one detached part
    tx2->commit(DB::NoCommitOptions{});
    auto tx3 = storage->createTransaction();
    writeThroughTransaction(*tx3, "uui/uuid-1/all_2_2_0/data.bin", "live2");
    tx3->commit(DB::NoCommitOptions{});
    storage->store()->putNamespaceFile(storage->liveNamespace("uuid-1"), "format_version.txt", "1\n");

    auto tx4 = storage->createTransaction();
    tx4->moveDirectory("uui/uuid-1", "uui/uuid-2");
    tx4->commit(DB::NoCommitOptions{});

    EXPECT_FALSE(storage->existsDirectory("uui/uuid-1"));
    EXPECT_TRUE(storage->existsDirectory("uui/uuid-2"));
    EXPECT_TRUE(storage->existsFile("uui/uuid-2/all_2_2_0/data.bin"));
    EXPECT_TRUE(storage->existsFile("uui/uuid-2/format_version.txt"));
    EXPECT_TRUE(storage->existsDirectory("uui/uuid-2/detached/all_1_1_0"));
}

/// B126: RENAME TABLE move_namespace is idempotent — re-driving the SAME rename after it completed is a
/// clean no-op (the source namespace is already gone), so a partial-failure re-drive is safe.
TEST(CaWiringOps, TableRenameIsIdempotentOnRedrive)
{
    auto storage = openWiringStorage();
    auto tx = storage->createTransaction();
    writeThroughTransaction(*tx, "uui/uuid-1/all_1_1_0/data.bin", "live");
    tx->commit(DB::NoCommitOptions{});
    storage->store()->putNamespaceFile(storage->liveNamespace("uuid-1"), "format_version.txt", "1\n");

    auto tx2 = storage->createTransaction();
    tx2->moveDirectory("uui/uuid-1", "uui/uuid-2");
    tx2->commit(DB::NoCommitOptions{});

    /// Re-drive the identical rename: uuid-1 is empty/gone, so every step no-ops; must not throw.
    auto tx3 = storage->createTransaction();
    EXPECT_NO_THROW(tx3->moveDirectory("uui/uuid-1", "uui/uuid-2"));
    tx3->commit(DB::NoCommitOptions{});

    EXPECT_TRUE(storage->existsFile("uui/uuid-2/all_1_1_0/data.bin"));
    EXPECT_TRUE(storage->existsFile("uui/uuid-2/format_version.txt"));
    EXPECT_FALSE(storage->existsDirectory("uui/uuid-1"));
}

/// B123: a verbatim-file move (get->put->remove, no native rename) is idempotent on re-drive — once the
/// source is gone but the destination is present, a re-driven move is a no-op, not a FILE_DOESNT_EXIST.
TEST(CaWiringOps, VerbatimMoveIsIdempotentOnRedrive)
{
    auto storage = openWiringStorage();
    auto tx = storage->createTransaction();
    writeThroughTransaction(*tx, "uui/uuid-1/tmp_mutation_7.txt", "cmds");
    auto & ca_tx = dynamic_cast<DB::ContentAddressedTransaction &>(*tx);
    ca_tx.moveFile("uui/uuid-1/tmp_mutation_7.txt", "uui/uuid-1/mutation_7.txt");
    EXPECT_TRUE(storage->existsFile("uui/uuid-1/mutation_7.txt"));
    /// Re-drive: source gone, destination present → no-op (no throw).
    EXPECT_NO_THROW(ca_tx.moveFile("uui/uuid-1/tmp_mutation_7.txt", "uui/uuid-1/mutation_7.txt"));
    EXPECT_TRUE(storage->existsFile("uui/uuid-1/mutation_7.txt"));
    /// Both source and destination absent → genuine missing source still throws.
    EXPECT_ANY_THROW(ca_tx.moveFile("uui/uuid-1/tmp_mutation_8.txt", "uui/uuid-1/mutation_8.txt"));
}

/// B124: moveDirectory's staged-merge is source-wins, and a genuine collision (the same mutable file
/// staged under BOTH the source and destination part keys with DIFFERING bytes) fails loud instead of
/// silently dropping a just-written file. Identical bytes are a benign idempotent re-key.
TEST(CaWiringOps, MoveDirectoryMutableCollisionPolicy)
{
    /// Differing bytes → fail loud.
    {
        auto storage = openWiringStorage();
        auto tx = storage->createTransaction();
        writeThroughTransaction(*tx, "uui/uuid-1/tmp_x/txn_version.txt", "A");
        writeThroughTransaction(*tx, "uui/uuid-1/all_9_9_9/txn_version.txt", "B");
        auto & ca_tx = dynamic_cast<DB::ContentAddressedTransaction &>(*tx);
        EXPECT_ANY_THROW(ca_tx.moveDirectory("uui/uuid-1/tmp_x", "uui/uuid-1/all_9_9_9"));
    }
    /// Identical bytes → benign, no throw (source-wins, idempotent). Both parts carry real content so
    /// the eager publish-at-rename builds a proper ref (a mutable-only staging would instead hit
    /// updateRefPayload on a not-yet-committed ref — unrelated to the collision policy under test).
    {
        auto storage = openWiringStorage();
        auto tx = storage->createTransaction();
        writeThroughTransaction(*tx, "uui/uuid-1/tmp_y/data.bin", "d1");
        writeThroughTransaction(*tx, "uui/uuid-1/tmp_y/txn_version.txt", "SAME");
        writeThroughTransaction(*tx, "uui/uuid-1/all_8_8_8/data.bin", "d2");
        writeThroughTransaction(*tx, "uui/uuid-1/all_8_8_8/txn_version.txt", "SAME");
        auto & ca_tx = dynamic_cast<DB::ContentAddressedTransaction &>(*tx);
        EXPECT_NO_THROW(ca_tx.moveDirectory("uui/uuid-1/tmp_y", "uui/uuid-1/all_8_8_8"));
    }
}

TEST(CaWiringOps, FreezeViaHardLinksIntoShadow)
{
    auto storage = openWiringStorage();
    auto tx = storage->createTransaction();
    writeThroughTransaction(*tx, "uui/uuid-1/all_1_1_0/data.bin", "frozen-bytes");
    writeThroughTransaction(*tx, "uui/uuid-1/all_1_1_0/metadata_version.txt", "7");
    tx->commit(DB::NoCommitOptions{});

    /// FREEZE clones a committed part file-by-file into the shadow tree via hardlinks; the staged
    /// shadow part publishes at commit (pool-global - any replica reads the backup).
    auto tx2 = storage->createTransaction();
    tx2->createHardLink("uui/uuid-1/all_1_1_0/data.bin", "shadow/bk1/store/uui/uuid-1/all_1_1_0/data.bin");
    tx2->createHardLink("uui/uuid-1/all_1_1_0/metadata_version.txt",
                        "shadow/bk1/store/uui/uuid-1/all_1_1_0/metadata_version.txt");
    tx2->commit(DB::NoCommitOptions{});

    EXPECT_TRUE(storage->existsDirectory("shadow/bk1/store/uui/uuid-1/all_1_1_0"));
    EXPECT_TRUE(storage->existsFile("shadow/bk1/store/uui/uuid-1/all_1_1_0/data.bin"));
    EXPECT_EQ(storage->tryGetInManifestBytes("shadow/bk1/store/uui/uuid-1/all_1_1_0/metadata_version.txt"),
              std::optional<String>("7"));

    /// UNFREEZE: removeRecursive of the backup root drops every shadow namespace under it.
    auto tx3 = storage->createTransaction();
    tx3->removeRecursive("shadow/bk1", {});
    EXPECT_FALSE(storage->existsDirectory("shadow/bk1/store/uui/uuid-1/all_1_1_0"));
    EXPECT_FALSE(storage->existsDirectory("shadow/bk1"));
}

/// ==== M-W Task 8: in-flight read-your-writes (B59) ====

TEST(CaWiringInFlight, StagedFilesVisibleBeforeCommit)
{
    auto storage = openWiringStorage();
    auto tx = storage->createTransaction();
    writeThroughTransaction(*tx, "uui/uuid-1/tmp_mut_all_1_1_0/p.proj/data.bin", "proj-bytes");
    writeThroughTransaction(*tx, "uui/uuid-1/tmp_mut_all_1_1_0/uuid.txt", "u-9");

    /// The file trio.
    auto objects = tx->tryGetInFlightStorageObjects("uui/uuid-1/tmp_mut_all_1_1_0/p.proj/data.bin");
    ASSERT_TRUE(objects.has_value());
    ASSERT_EQ(objects->size(), 1u);
    EXPECT_FALSE((*objects)[0].remote_path.empty());
    EXPECT_EQ((*objects)[0].bytes_size, 10u);
    EXPECT_EQ(tx->tryGetInFlightFileSize("uui/uuid-1/tmp_mut_all_1_1_0/p.proj/data.bin"), std::optional<uint64_t>(10));
    EXPECT_EQ(tx->tryGetInFlightFileSize("uui/uuid-1/tmp_mut_all_1_1_0/uuid.txt"), std::optional<uint64_t>(3));
    EXPECT_FALSE(tx->tryGetInFlightFileSize("uui/uuid-1/tmp_mut_all_1_1_0/missing.bin").has_value());

    /// Bytes read back: a staged blob through the payload view; staged mutable bytes from memory.
    {
        auto buf = tx->tryReadFileInFlight("uui/uuid-1/tmp_mut_all_1_1_0/p.proj/data.bin", {}, std::nullopt);
        ASSERT_TRUE(buf);
        String read;
        readStringUntilEOF(read, *buf);
        EXPECT_EQ(read, "proj-bytes");
    }
    {
        auto buf = tx->tryReadFileInFlight("uui/uuid-1/tmp_mut_all_1_1_0/uuid.txt", {}, std::nullopt);
        ASSERT_TRUE(buf);
        String read;
        readStringUntilEOF(read, *buf);
        EXPECT_EQ(read, "u-9");
    }

    /// The directory overlay answers for INNER dirs only (the PoC contract): the part dir itself
    /// is FALSE so a rejected temporary part's removeIfNeeded takes the clean early-return path.
    EXPECT_FALSE(tx->hasInFlightDirectory("uui/uuid-1/tmp_mut_all_1_1_0"));
    EXPECT_TRUE(tx->hasInFlightDirectory("uui/uuid-1/tmp_mut_all_1_1_0/p.proj"));
    EXPECT_FALSE(tx->hasInFlightDirectory("uui/uuid-1/tmp_mut_all_1_1_0/q.proj"));
    auto top = tx->listInFlightDirectory("uui/uuid-1/tmp_mut_all_1_1_0");
    EXPECT_EQ(top, (std::vector<std::string>{"p.proj", "uuid.txt"}));
    EXPECT_EQ(tx->listInFlightDirectory("uui/uuid-1/tmp_mut_all_1_1_0/p.proj"),
              (std::vector<std::string>{"data.bin"}));
}

/// ==== M-W Task 10: the GC scheduler end-to-end through the wiring ====

TEST(CaWiringGc, DroppedPartIsReclaimedByRounds)
{
    auto storage = openWiringStorage();
    auto tx = storage->createTransaction();
    writeThroughTransaction(*tx, "uui/uuid-1/all_1_1_0/data.bin", "reclaim-me");
    tx->commit(DB::NoCommitOptions{});

    auto * exchange = dynamic_cast<DB::IContentAddressedExchange *>(storage.get());
    const auto tree_id = exchange->getPartTreeId("uui/uuid-1/all_1_1_0");
    ASSERT_TRUE(tree_id.has_value());
    const auto blob_key = storage->getStorageObjects("uui/uuid-1/all_1_1_0/data.bin")[0].remote_path;

    auto tx2 = storage->createTransaction();
    tx2->removeDirectory("uui/uuid-1/all_1_1_0");   /// dropRef - the part is unreachable now

    /// Round 1 folds the drop and retires+deletes the TREE; the cascade frees the blob, which a
    /// FOLLOWING round retires+deletes (next-round reclamation, M-C3). The steal needs one extra
    /// observation window between rounds (the pacing scheduler is stable across these calls -
    /// each call after the first re-acquires via renewal).
    storage->runOneGcRoundForTest();
    storage->runOneGcRoundForTest();
    storage->runOneGcRoundForTest();

    EXPECT_FALSE(storage->existsDirectory("uui/uuid-1/all_1_1_0"));
    /// The DELETION WITNESS: adopting the old tree id must fail (the tree object is GONE) - the
    /// same probe the relink race uses. (Key equality of a re-written identical part would be
    /// vacuous: content addressing reproduces the key whether or not the old object survived.)
    EXPECT_FALSE(exchange->adoptPart("uuid-1", "tmp-fetch_back", *tree_id, {}));
    EXPECT_FALSE(storage->existsDirectory("uui/uuid-1/tmp-fetch_back"));

    /// A fresh identical write re-CREATES the content at the same key and reads back fine.
    auto tx3 = storage->createTransaction();
    writeThroughTransaction(*tx3, "uui/uuid-1/all_9_9_0/data.bin", "reclaim-me");
    tx3->commit(DB::NoCommitOptions{});
    EXPECT_EQ(storage->getStorageObjects("uui/uuid-1/all_9_9_0/data.bin")[0].remote_path, blob_key);
    EXPECT_TRUE(storage->existsFile("uui/uuid-1/all_9_9_0/data.bin"));
}

/// ==== M-W Task 11: the DataPartsExchange facade (relink) ====

TEST(CaWiringExchange, AdoptPartPublishesOwnRef)
{
    /// Sender and receiver share one pool: model both as two metadata storages over the SAME
    /// backend is not constructible here (Local storages differ), so use one storage as both ends
    /// - the facade only touches pool state.
    auto storage = openWiringStorage();
    auto tx = storage->createTransaction();
    writeThroughTransaction(*tx, "uui/uuid-1/all_1_1_0/data.bin", "replicate-me");
    tx->commit(DB::NoCommitOptions{});

    auto * exchange = dynamic_cast<DB::IContentAddressedExchange *>(storage.get());
    ASSERT_NE(exchange, nullptr);
    EXPECT_FALSE(exchange->getPoolUUID().empty());

    auto tree_id = exchange->getPartTreeId("uui/uuid-1/all_1_1_0");
    ASSERT_TRUE(tree_id.has_value());
    EXPECT_FALSE(exchange->getPartTreeId("uui/uuid-1/all_9_9_9").has_value());

    /// The receiver's adoption: a new ref to the SAME tree under the fetched part name, carrying
    /// the transferred mutable header.
    ASSERT_TRUE(exchange->adoptPart("uuid-1", "tmp-fetch_all_1_1_0", *tree_id, {{"metadata_version.txt", "4"}}));
    EXPECT_TRUE(storage->existsFile("uui/uuid-1/tmp-fetch_all_1_1_0/data.bin"));
    EXPECT_EQ(storage->tryGetInManifestBytes("uui/uuid-1/tmp-fetch_all_1_1_0/metadata_version.txt"),
              std::optional<String>("4"));
    EXPECT_EQ(storage->getStorageObjects("uui/uuid-1/all_1_1_0/data.bin")[0].remote_path,
              storage->getStorageObjects("uui/uuid-1/tmp-fetch_all_1_1_0/data.bin")[0].remote_path);
}

TEST(CaWiringExchange, AdoptOfReclaimedTreeFallsBack)
{
    auto storage = openWiringStorage();
    auto tx = storage->createTransaction();
    writeThroughTransaction(*tx, "uui/uuid-1/all_1_1_0/data.bin", "soon-gone");
    tx->commit(DB::NoCommitOptions{});
    auto * exchange = dynamic_cast<DB::IContentAddressedExchange *>(storage.get());
    auto tree_id = exchange->getPartTreeId("uui/uuid-1/all_1_1_0");
    ASSERT_TRUE(tree_id.has_value());

    /// The part is dropped and reclaimed before the receiver adopts (the relink race).
    auto tx2 = storage->createTransaction();
    tx2->removeDirectory("uui/uuid-1/all_1_1_0");
    storage->runOneGcRoundForTest();
    storage->runOneGcRoundForTest();

    /// adoptPart returns false (byte-fetch fallback) and publishes NOTHING.
    EXPECT_FALSE(exchange->adoptPart("uuid-1", "tmp-fetch_all_1_1_0", *tree_id, {}));
    EXPECT_FALSE(storage->existsDirectory("uui/uuid-1/tmp-fetch_all_1_1_0"));
}

/// ==== Commit atomicity (B122): a publish failing mid-loop must not leave a PARTIAL commit ====

#include <Disks/DiskObjectStorage/ObjectStorages/Local/LocalObjectStorage.h>
#include <Common/Exception.h>
#include <algorithm>

namespace DB::ErrorCodes
{
    extern const int LOGICAL_ERROR;
    extern const int CORRUPTED_DATA;
    extern const int READONLY;
}

namespace
{

/// A LocalObjectStorage whose writeObject can be armed to throw — the single seam needed to drive a
/// backend write failure at a chosen point. The hook runs BEFORE the write is created; throwing from
/// it fails the put exactly as a real backend error would. Everything else delegates to the base.
class FaultyLocalObjectStorage : public DB::LocalObjectStorage
{
public:
    using DB::LocalObjectStorage::LocalObjectStorage;

    std::function<void(const std::string &)> on_write;

    std::unique_ptr<DB::WriteBufferFromFileBase> writeObject(
        const DB::StoredObject & object,
        DB::WriteMode mode,
        std::optional<DB::ObjectAttributes> attributes,
        size_t buf_size,
        const DB::WriteSettings & write_settings) override
    {
        if (on_write)
            on_write(object.remote_path);
        return DB::LocalObjectStorage::writeObject(object, mode, attributes, buf_size, write_settings);
    }
};

/// True for a root-shard manifest key (<...>/roots/<namespace...>/<shard_number>) — the object a
/// single publish CASes. Tree blobs (blobs/ prefix), the registry (`gc/registry`) and verbatim
/// files (roots/<ns>/_files/...) are excluded, so counting these isolates publishes one-for-one.
bool isShardManifestPath(const std::string & path)
{
    if (path.find("/roots/") == std::string::npos)
        return false;
    const auto slash = path.rfind('/');
    if (slash == std::string::npos || slash + 1 >= path.size())
        return false;
    const std::string last = path.substr(slash + 1);
    return std::all_of(last.begin(), last.end(), [](unsigned char c) { return c >= '0' && c <= '9'; });
}

std::shared_ptr<FaultyLocalObjectStorage> makeFaultyStorageForTest()
{
    static std::atomic<uint64_t> counter{0};
    const auto unique = std::to_string(::getpid()) + "_" + std::to_string(counter.fetch_add(1));
    const auto root = (std::filesystem::temp_directory_path() / ("ca_b122_" + unique)).string();
    std::error_code ec;
    std::filesystem::remove_all(root, ec);
    std::filesystem::create_directories(root, ec);
    return std::make_shared<FaultyLocalObjectStorage>(DB::LocalObjectStorageSettings("test", root, /*read_only_=*/false));
}

}

TEST(CaWiringWrite, PartialCommitRollsBackPublishedParts)
{
    auto faulty = makeFaultyStorageForTest();
    auto storage = std::make_shared<DB::ContentAddressedMetadataStorage>(
        faulty, "pool", "srv1", std::filesystem::temp_directory_path() / "ca_b122_scratch", nullptr);
    storage->startup();

    /// Two parts in ONE transaction. The staging map is keyed by (ns, ref), so all_1_1_0 publishes
    /// before all_2_2_0 — the blob uploads happen now (before the fault is armed).
    auto tx = storage->createTransaction();
    writeThroughTransaction(*tx, "uui/uuid-1/all_1_1_0/data.bin", "content-A");
    writeThroughTransaction(*tx, "uui/uuid-1/all_2_2_0/data.bin", "content-B");

    /// Fail the SECOND shard-manifest publish (part all_2_2_0) — all_1_1_0 has already published by
    /// then. A pre-fix commit() leaves all_1_1_0 durably visible: a partial commit.
    int manifest_writes = 0;
    faulty->on_write = [&](const std::string & path)
    {
        if (isShardManifestPath(path) && ++manifest_writes == 2)
            throw DB::Exception(DB::ErrorCodes::LOGICAL_ERROR, "injected publish failure (B122)");
    };

    EXPECT_THROW(tx->commit(DB::NoCommitOptions{}), DB::Exception);

    /// All-or-nothing: the part that DID publish must have been rolled back. Disarm first so the
    /// rollback's own (3rd) manifest write and these read-back assertions run clean.
    faulty->on_write = nullptr;
    EXPECT_FALSE(storage->existsDirectory("uui/uuid-1/all_1_1_0"));
    EXPECT_FALSE(storage->existsDirectory("uui/uuid-1/all_2_2_0"));
}

TEST(CaWiringReadOnly, ObserveOnlyOpenReadsButRejectsWrites)
{
    /// 1. Writable storage publishes a part into a fixed root.
    const auto root = (std::filesystem::temp_directory_path()
                       / ("ca_ro_" + std::to_string(::getpid()))).string();
    std::error_code ec; std::filesystem::remove_all(root, ec); std::filesystem::create_directories(root, ec);
    auto writable_os = std::make_shared<DB::LocalObjectStorage>(
        DB::LocalObjectStorageSettings("test", root, /*read_only_=*/false));
    {
        auto w = std::make_shared<DB::ContentAddressedMetadataStorage>(
            writable_os, "pool", "srv1", std::filesystem::temp_directory_path() / "ca_ro_scratch", nullptr);
        w->startup();
        auto tx = w->createTransaction();
        writeThroughTransaction(*tx, "uui/uuid-1/all_1_1_0/data.bin", "ro-bytes");
        tx->commit(DB::NoCommitOptions{});
    }

    /// 2. Read-only object storage over the SAME root => observe-only metadata storage.
    auto ro_os = std::make_shared<DB::LocalObjectStorage>(
        DB::LocalObjectStorageSettings("test", root, /*read_only_=*/true));
    /// Same server_id as the writer: live namespaces are server-scoped (liveNamespace prepends
    /// server_id), so an observe-only mount reads the same server's data — the WORM scenario.
    auto ro = std::make_shared<DB::ContentAddressedMetadataStorage>(
        ro_os, "pool", "srv1", std::filesystem::temp_directory_path() / "ca_ro_scratch2", nullptr);
    ro->startup();   /// must NOT throw (probe skipped — a probe write would fail on a read-only os)

    EXPECT_TRUE(ro->isReadOnly());
    /// Reads work:
    EXPECT_TRUE(ro->existsFile("uui/uuid-1/all_1_1_0/data.bin"));
    EXPECT_EQ(ro->getFileSize("uui/uuid-1/all_1_1_0/data.bin"), 8u);
    /// Writes fail closed:
    DB::Cas::tests::expectThrowsCode(DB::ErrorCodes::READONLY,
        [&] { ro->createTransaction(); });
    DB::Cas::tests::expectThrowsCode(DB::ErrorCodes::READONLY,
        [&] { ro->adoptPart("uuid-1", "tmp-fetch", std::string(32, '0'), {}); });
}

TEST(CaWiringRead, CorruptCaMtimeStampFailsClosed)
{
    auto storage = openWiringStorage();
    auto tx = storage->createTransaction();
    writeThroughTransaction(*tx, "uui/uuid-1/all_1_1_0/data.bin", "x");
    tx->commit(DB::NoCommitOptions{});

    /// Corrupt the reserved publish stamp. getLastModified must surface a typed CORRUPTED_DATA rather
    /// than leaking a bare std::invalid_argument/out_of_range out of the underlying integer parse.
    storage->store()->updateRefPayload(storage->liveNamespace("uuid-1"), "all_1_1_0",
        [](DB::Cas::RefPayload & payload) { payload.mutable_files[".ca_mtime"] = "not-a-number"; });

    DB::Cas::tests::expectThrowsCode(DB::ErrorCodes::CORRUPTED_DATA,
        [&] { storage->getLastModified("uui/uuid-1/all_1_1_0"); });
}

// #4: content_addressed_root_shards is plumbed through the ctor to pool creation (creation-time
// fanout); default stays 8.
TEST(CaWiring, RootShardsConfigurable)
{
    auto widened = std::make_shared<DB::ContentAddressedMetadataStorage>(
        DB::Cas::tests::makeLocalObjectStorageForTest(), "pool", "srv1",
        std::filesystem::temp_directory_path() / "ca_wiring_rootshards", nullptr,
        /*gc_enabled=*/false, std::chrono::seconds(60), /*root_shards=*/4);
    widened->startup();
    EXPECT_EQ(widened->store()->poolMeta().root_shards, 4u);

    auto defaulted = std::make_shared<DB::ContentAddressedMetadataStorage>(
        DB::Cas::tests::makeLocalObjectStorageForTest(), "pool", "srv1",
        std::filesystem::temp_directory_path() / "ca_wiring_rootshards_def", nullptr);
    defaulted->startup();
    EXPECT_EQ(defaulted->store()->poolMeta().root_shards, 8u);
}
