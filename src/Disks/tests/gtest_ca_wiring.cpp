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
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/ContentAddressedExchange.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/ContentAddressedTransaction.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasBuild.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasBackend.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasFsck.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasLayout.h>
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

    /// B188 precommit-first: content blobs are PENDING (staged locally, not yet uploaded). So
    /// tryGetInFlightStorageObjects returns {} — the pool object does not exist yet. The caller
    /// (DataPartStorageOnDiskFull::prepareRead) falls back to tryGetInFlightFileSize to get the size
    /// and then serves the content via tryReadFileInFlight (local temp file). File sizes and directory
    /// overlay still work because they are driven by the staged tree entry, not the pool.
    auto objects = tx->tryGetInFlightStorageObjects("uui/uuid-1/tmp_mut_all_1_1_0/p.proj/data.bin");
    EXPECT_FALSE(objects.has_value());
    EXPECT_EQ(tx->tryGetInFlightFileSize("uui/uuid-1/tmp_mut_all_1_1_0/p.proj/data.bin"), std::optional<uint64_t>(10));
    EXPECT_EQ(tx->tryGetInFlightFileSize("uui/uuid-1/tmp_mut_all_1_1_0/uuid.txt"), std::optional<uint64_t>(3));
    EXPECT_FALSE(tx->tryGetInFlightFileSize("uui/uuid-1/tmp_mut_all_1_1_0/missing.bin").has_value());

    /// Bytes read back: a pending blob from the local temp file (B188); staged mutable bytes from memory.
    {
        auto buf = tx->tryReadFileInFlight("uui/uuid-1/tmp_mut_all_1_1_0/p.proj/data.bin", {}, std::nullopt);
        ASSERT_TRUE(buf);
        String read;
        readStringUntilEOF(read, *buf);
        EXPECT_EQ(read, "proj-bytes");   /// B188: served from local temp file (pending upload)
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

/// B199 (real-path leak repro, replaces the former Build-level `CasGcLeak.DisplacedUnexpandedTreeBlobsLeak`):
/// the original repro drove `build->publish(...)` DIRECTLY with NO precommit — a path production NEVER
/// takes (`ContentAddressedTransaction` always goes precommit-first). This test reproduces the SAME
/// scenario through the GENUINE transaction/wiring entry point and asserts it is reclaimed:
///   - commit a part to treeA (unique content),
///   - DISPLACE the part's ref to treeB (distinct content) by RE-WRITING the same part path in a fresh
///     transaction: `publishStaging` republishes the existing ref via `build->publish` — a second `Add`
///     for the same ref name with NO Remove between (last-op-wins displacement),
///   - delete treeA's TREE OBJECT at the backend BEFORE any GC fold (a competing GC's landed delete,
///     `deleteExact`-by-token — the same primitive GC uses),
///   - run GC to a fixpoint.
/// GREEN because the real path is precommit-first: treeA was born under its own unique precommit ref
/// whose `Add` carries treeA's closure INLINE on the journal record (B199-S2), so the fold records
/// treeA→{its blobs} from the recorded closure WITHOUT reading the vanished tree object, and the
/// B199-S1 retire absent-tree strip then releases those edges. treeA's unique blobs are reclaimed
/// (`unreachable==0`); `dangling==0` throughout (treeB and its closure are never at risk — no data loss).
TEST(CaWiringGc, DisplacedTreeBlobsReclaimedThroughRealPath)
{
    auto storage = openWiringStorage();
    auto * exchange = dynamic_cast<DB::IContentAddressedExchange *>(storage.get());
    ASSERT_NE(exchange, nullptr);

    /// Commit treeA with unique content (data-A / mark-A), through the real precommit-first transaction.
    {
        auto tx = storage->createTransaction();
        writeThroughTransaction(*tx, "uui/uuid-1/all_0_0_0/data.bin", "data-A");
        writeThroughTransaction(*tx, "uui/uuid-1/all_0_0_0/data.cmrk3", "mark-A");
        tx->commit(DB::NoCommitOptions{});
    }
    const auto tree_a_hex = exchange->getPartTreeId("uui/uuid-1/all_0_0_0");
    ASSERT_TRUE(tree_a_hex.has_value());

    /// DISPLACE: re-write the SAME part path with DISTINCT content (data-B / mark-B) in a fresh
    /// transaction. publishStaging republishes ref `all_0_0_0` → treeB via build->publish, appending a
    /// second Add for the ref with no intervening Remove (last-op-wins). Confirm the displacement is real:
    /// the ref now resolves to a DIFFERENT tree.
    {
        auto tx = storage->createTransaction();
        writeThroughTransaction(*tx, "uui/uuid-1/all_0_0_0/data.bin", "data-B");
        writeThroughTransaction(*tx, "uui/uuid-1/all_0_0_0/data.cmrk3", "mark-B");
        tx->commit(DB::NoCommitOptions{});
    }
    const auto tree_b_hex = exchange->getPartTreeId("uui/uuid-1/all_0_0_0");
    ASSERT_TRUE(tree_b_hex.has_value());
    ASSERT_NE(*tree_a_hex, *tree_b_hex)
        << "the second write must displace the ref to a distinct tree (last-op-wins Add, no Remove)";

    /// A competing GC round already deleted the displaced treeA OBJECT: remove it at the backend by
    /// token, BEFORE any fold here, so the fold would 404 a readTree of treeA.
    {
        auto & backend = storage->store()->backend();
        const String tree_a_key = storage->store()->layout().treeKey(DB::Cas::TreeId(*tree_a_hex));
        const auto head = backend.head(tree_a_key);
        ASSERT_TRUE(head.exists) << "treeA object must exist before we delete it";
        ASSERT_EQ(backend.deleteExact(tree_a_key, head.token).kind, DB::Cas::DeleteOutcome::Kind::Deleted);
        ASSERT_FALSE(backend.head(tree_a_key).exists) << "treeA backend object must be gone after the delete";
    }

    /// Drive GC to a fixpoint. The dropped-part test needs ~3 rounds (next-round cascade reclamation);
    /// give a generous bound so the displaced-tree closure fully drains.
    for (int i = 0; i < 8; ++i)
        storage->runOneGcRoundForTest();

    const DB::Cas::FsckReport after = DB::Cas::runFsck(*storage->store(), /*detail=*/false);
    EXPECT_EQ(after.dangling, 0u) << "displacement must never lose a reachable object (treeB stays live)";
    EXPECT_GT(after.reachable, 0u) << "the live ref points at treeB; treeB's closure is reachable";
    EXPECT_EQ(after.unreachable, 0u)
        << "B199 real-path: treeA's unique blobs (data-A / mark-A) must be reclaimed even though treeA's "
        << "object vanished before the fold — the precommit Add's INLINE closure records treeA's edges "
        << "without a readTree (B199-S2) and the retire absent-tree strip releases them (B199-S1); "
        << "unreachable=" << after.unreachable;
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
#include <set>

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

/// ==== B188 precommit-first order invariant (Task 6) ====
///
/// A RecordingLocalObjectStorage records the four IObjectStorage methods the CA emulated-mode backend
/// uses on the commit path — writeObject (PUT), exists + getObjectMetadata (the HEAD), and readObject
/// (the GET) — as (op_name, logical_key). "Logical" means the bare pool key (without the emu_root
/// prefix) — the same string the Layout functions produce, so the `/blobs/`, `/trees/`, and
/// `/_precommits/` substring tests are unambiguous.
///
/// After commit the test asserts: the FIRST write to a key containing "/_precommits/" happened before
/// ALL ops (read OR write) on keys containing "/blobs/" or "/trees/". The precommit ref is what pins
/// the in-flight build-root closure so GC cannot reclaim the not-yet-uploaded content objects;
/// therefore every pool op touching a content blob or the manifest tree must be AFTER the precommit
/// ref is durably written. The READ gating is the heart of the B188 fix: the original bug was an
/// EAGER HEAD on a content blob during staging, before any precommit protection existed — a
/// write-only assertion would not catch its reintroduction.

namespace
{

/// Records the four IObjectStorage methods the CA emulated-mode backend uses on the commit path
/// (writeObject/exists/getObjectMetadata/readObject). listObjects/copyObject are deliberately NOT
/// overridden — they are not on the commit path the order invariant gates.
class RecordingLocalObjectStorage final : public DB::LocalObjectStorage
{
public:
    using DB::LocalObjectStorage::LocalObjectStorage;

    struct Record
    {
        std::string op;    /// "writeObject" | "exists" | "getObjectMetadata" | "readObject"
        std::string key;   /// logical (emu_root stripped)
    };

    /// Append-only; mutable so the const read methods (exists/readObject/tryGetObjectMetadata) can
    /// record. No mutex — these tests are single-threaded.
    mutable std::vector<Record> ops;

    /// Strip the common-key-prefix (emu_root) to recover the logical key. The emu_root is returned by
    /// getCommonKeyPrefix() and always ends with a path separator in LocalObjectStorage.
    std::string toLogical(const std::string & physical) const
    {
        const std::string root = getCommonKeyPrefix();
        std::string logical;
        if (!root.empty() && physical.starts_with(root))
            logical = physical.substr(root.size());
        else
            logical = physical;
        /// Strip any leading slash left after prefix removal.
        if (!logical.empty() && logical.front() == '/')
            logical = logical.substr(1);
        return logical;
    }

    std::unique_ptr<DB::WriteBufferFromFileBase> writeObject(
        const DB::StoredObject & object,
        DB::WriteMode mode,
        std::optional<DB::ObjectAttributes> attributes,
        size_t buf_size,
        const DB::WriteSettings & write_settings) override
    {
        ops.push_back({"writeObject", toLogical(object.remote_path)});
        return DB::LocalObjectStorage::writeObject(object, mode, attributes, buf_size, write_settings);
    }

    /// Backs the CA backend's `head` (emuExists) and gates its `get` (emuExists before emuRead).
    bool exists(const DB::StoredObject & object) const override
    {
        ops.push_back({"exists", toLogical(object.remote_path)});
        return DB::LocalObjectStorage::exists(object);
    }

    /// Backs the CA backend's `head` size/attributes lookup (emuPath stat).
    std::optional<DB::ObjectMetadata> tryGetObjectMetadata(const std::string & path, bool with_tags) const override
    {
        ops.push_back({"getObjectMetadata", toLogical(path)});
        return DB::LocalObjectStorage::tryGetObjectMetadata(path, with_tags);
    }

    /// Backs the CA backend's `get` body read (readObjectRanged).
    std::unique_ptr<DB::ReadBufferFromFileBase> readObject(
        const DB::StoredObject & object,
        const DB::ReadSettings & read_settings,
        std::optional<size_t> read_hint,
        bool use_external_buffer,
        bool restrict_seek) const override
    {
        ops.push_back({"readObject", toLogical(object.remote_path)});
        return DB::LocalObjectStorage::readObject(object, read_settings, read_hint, use_external_buffer, restrict_seek);
    }
};

std::shared_ptr<RecordingLocalObjectStorage> makeRecordingStorageForTest(const std::string & tag)
{
    static std::atomic<uint64_t> counter{0};
    const auto unique = std::to_string(::getpid()) + "_" + std::to_string(counter.fetch_add(1));
    const auto root = (std::filesystem::temp_directory_path() / ("ca_b188_" + tag + "_" + unique)).string();
    std::error_code ec;
    std::filesystem::remove_all(root, ec);
    std::filesystem::create_directories(root, ec);
    return std::make_shared<RecordingLocalObjectStorage>(
        DB::LocalObjectStorageSettings("test", root, /*read_only_=*/false));
}

/// Index of the first writeObject to a /_precommits/ key (the durable precommit), or -1. Anchors on
/// the WRITE, not on any op: mutateShard(precommitNs) reads the precommit-shard key (exists/readObject)
/// before its casPut, so an any-op scan would anchor on that READ rather than the durable write.
int firstPrecommitWriteIdx(const std::vector<RecordingLocalObjectStorage::Record> & log)
{
    for (int i = 0; i < static_cast<int>(log.size()); ++i)
        if (log[i].op == "writeObject" && log[i].key.find("/_precommits/") != std::string::npos)
            return i;
    return -1;
}

}

/// B188: every pool op (read OR write) on /blobs/ or /trees/ must come AFTER the first write to a
/// /_precommits/ key — including HEAD (exists/getObjectMetadata) and GET (readObject), since the
/// exact bug was an eager HEAD on a content blob during staging. The transaction writes a fresh
/// content file (pending blob) AND adopts an existing committed blob via hardlink — both paths must
/// satisfy the invariant.
TEST(CaWiringPrecommitOrder, NoContentPoolOpBeforePrecommit)
{
    auto recording = makeRecordingStorageForTest("order");
    auto storage = std::make_shared<DB::ContentAddressedMetadataStorage>(
        recording, "pool", "srv1",
        std::filesystem::temp_directory_path() / "ca_b188_order_scratch", nullptr);
    storage->startup();

    /// Phase 1: publish a committed source part — this gives us a committed blob to adopt in Phase 2.
    {
        auto tx = storage->createTransaction();
        writeThroughTransaction(*tx, "uui/uuid-1/all_0_0_0/data.bin", "source-blob");
        tx->commit(DB::NoCommitOptions{});
    }

    /// Phase 2: a new transaction that BOTH writes a fresh content blob (all_1_1_0/data.bin, pending)
    /// AND carries forward that PENDING blob via hardlink into a second fresh part (all_2_2_0/extra.bin,
    /// the cross-part pending-source adopt path). We clear the op log after Phase 1 so only Phase 2's
    /// ops are analysed.
    recording->ops.clear();

    auto tx = storage->createTransaction();
    writeThroughTransaction(*tx, "uui/uuid-1/all_1_1_0/data.bin", "fresh-content");
    /// Adopt by hardlinking a PENDING blob (the file just written above) into a SECOND fresh part
    /// (all_2_2_0). This is the B188-relevant adopt: the cross-part pending-source branch copies the
    /// PendingBlob into the dst build (NO eager pool op — the blob is not durable yet, so a HEAD/GET on
    /// it before precommit would be the exact bug). We deliberately do NOT adopt from the committed
    /// source part here: adoptFromTree(committed source) legitimately READS that source's
    /// already-durable, ref-pinned tree during staging — a foreign-tree read that is NOT a B188
    /// violation (the invariant is about THIS build's own not-yet-uploaded content, never a committed
    /// object owned by a live part). Gating it would be a false positive; see the committed-source
    /// adopt coverage in CaWiringOps.HardLinkCarriesForwardWithoutReupload.
    tx->createHardLink("uui/uuid-1/all_1_1_0/data.bin", "uui/uuid-1/all_2_2_0/extra.bin");
    tx->commit(DB::NoCommitOptions{});

    const auto & log = recording->ops;

    /// The content objects THIS transaction publishes are exactly the keys it WRITES under /blobs/
    /// or /trees/ (the staged tree + the fresh/pending blobs). The B188 invariant is that the build
    /// must not touch ITS OWN staged content before precommit. Reads of foreign committed objects
    /// (another part's tree) are legitimate and must not be gated — so we restrict the gate to the
    /// set of content keys this transaction itself wrote.
    std::set<std::string> own_content_keys;
    for (const auto & r : log)
        if (r.op == "writeObject"
            && (r.key.find("/blobs/") != std::string::npos || r.key.find("/trees/") != std::string::npos))
            own_content_keys.insert(r.key);

    /// Anchor on the first precommit WRITE (the durable casPut), not on any precommit-key op.
    const int first_precommit_idx = firstPrecommitWriteIdx(log);
    ASSERT_GE(first_precommit_idx, 0)
        << "No write to a /_precommits/ key was recorded — precommit step did not fire";

    /// Every op (read OR write) on one of THIS build's own content keys must have an index AFTER
    /// first_precommit_idx. This gates HEAD (exists/getObjectMetadata) and GET (readObject), not just
    /// PUT (writeObject) — an eager HEAD/GET on the build's own pending blob or staged tree before
    /// precommit is the exact B188 regression this guards against.
    for (int i = 0; i < static_cast<int>(log.size()); ++i)
    {
        if (!own_content_keys.contains(log[i].key))
            continue;
        EXPECT_GT(i, first_precommit_idx)
            << "Own-content pool op '" << log[i].op << "' on '" << log[i].key << "' at index " << i
            << " came BEFORE the first precommit write at index " << first_precommit_idx
            << " — violates B188 precommit-first invariant (no HEAD/GET/PUT on this build's content before precommit)";
    }

    /// Sanity: both parts are readable after commit, with the SAME underlying blob (content identity).
    EXPECT_TRUE(storage->existsFile("uui/uuid-1/all_1_1_0/data.bin"));
    EXPECT_TRUE(storage->existsFile("uui/uuid-1/all_2_2_0/extra.bin"));
    EXPECT_EQ(storage->getFileSize("uui/uuid-1/all_1_1_0/data.bin"), 13u);   /// "fresh-content"
    EXPECT_EQ(storage->getStorageObjects("uui/uuid-1/all_1_1_0/data.bin")[0].remote_path,
              storage->getStorageObjects("uui/uuid-1/all_2_2_0/extra.bin")[0].remote_path);

    /// Confirm at least one blob and one tree WRITE was recorded (both upload paths were exercised),
    /// so the gate above actually had content keys to check.
    const bool has_blob_write = std::any_of(log.begin(), log.end(),
        [](const RecordingLocalObjectStorage::Record & r)
        { return r.op == "writeObject" && r.key.find("/blobs/") != std::string::npos; });
    const bool has_tree_write = std::any_of(log.begin(), log.end(),
        [](const RecordingLocalObjectStorage::Record & r)
        { return r.op == "writeObject" && r.key.find("/trees/") != std::string::npos; });
    EXPECT_TRUE(has_blob_write) << "No /blobs/ write recorded — fresh blob path not exercised";
    EXPECT_TRUE(has_tree_write) << "No /trees/ write recorded — tree upload path not exercised";
    EXPECT_FALSE(own_content_keys.empty()) << "No own content keys collected — gate would be vacuous";
}

/// B188 committed-source adopt (the LITERAL bug path): when createHardLink carries forward a blob
/// from a COMMITTED source part (the source is NOT staged in this transaction), it takes the
/// adoptFromTree -> adoptEvidence branch — a TOKENLESS W-EVIDENCE dep with NO eager HEAD on the
/// adopted blob. The regression this guards is reverting adoptEvidence to a reuseBlob(false) (or any
/// observeAndAdmit) that HEADs the adopted blob during staging, before any precommit protection
/// exists. The own-content gate in NoContentPoolOpBeforePrecommit CANNOT catch this: the adopted blob
/// is FOREIGN (owned by the live source part, never written by this transaction), so it is absent from
/// own_content_keys. This test asserts a TARGETED invariant on that exact foreign blob key: no
/// exists/getObjectMetadata/readObject/writeObject on it before first_precommit_idx.
///
/// adoptFromTree legitimately READS the source TREE during staging (to find the entry) — that is fine
/// and is NOT asserted here; the assertion is scoped to the adopted BLOB key alone.
TEST(CaWiringPrecommitOrder, CommittedSourceAdoptNoHeadBeforePrecommit)
{
    auto recording = makeRecordingStorageForTest("committed_adopt");
    auto storage = std::make_shared<DB::ContentAddressedMetadataStorage>(
        recording, "pool", "srv1",
        std::filesystem::temp_directory_path() / "ca_b188_committed_adopt_scratch", nullptr);
    storage->startup();

    /// Phase 1: commit a source part with a content blob. Capture the source blob's logical key from
    /// the recorded /blobs/ write (the SAME key derivation the recorder uses, so the substring/index
    /// comparisons in Phase 2 line up exactly).
    recording->ops.clear();
    {
        auto tx = storage->createTransaction();
        writeThroughTransaction(*tx, "uui/uuid-1/all_0_0_0/data.bin", "committed-source-blob");
        tx->commit(DB::NoCommitOptions{});
    }
    std::string source_blob_key;
    for (const auto & r : recording->ops)
    {
        if (r.op == "writeObject" && r.key.find("/blobs/") != std::string::npos)
        {
            source_blob_key = r.key;
            break;
        }
    }
    ASSERT_FALSE(source_blob_key.empty())
        << "Phase 1 recorded no /blobs/ write — could not capture the committed-source blob key";

    /// Phase 2: a FRESH transaction that hardlinks the COMMITTED source blob into a NEW part. The
    /// source part (all_0_0_0) is not staged here, so createHardLink takes the committed-source branch
    /// (adoptFromTree -> adoptEvidence). Clear the log so only Phase 2's ops are analysed.
    recording->ops.clear();
    {
        auto tx = storage->createTransaction();
        tx->createHardLink("uui/uuid-1/all_0_0_0/data.bin", "uui/uuid-1/all_5_5_0/data.bin");
        tx->commit(DB::NoCommitOptions{});
    }

    const auto & log = recording->ops;

    /// Anchor on the first precommit WRITE (the durable casPut), not on any precommit-key op.
    const int first_precommit_idx = firstPrecommitWriteIdx(log);
    ASSERT_GE(first_precommit_idx, 0)
        << "No write to a /_precommits/ key was recorded — precommit step did not fire";

    /// TARGETED assertion: the adopted (foreign, committed) blob key must NOT be touched by ANY op
    /// (HEAD via exists/getObjectMetadata, GET via readObject, or PUT via writeObject) before the
    /// precommit write. With the bug reintroduced, adoptEvidence -> reuseBlob -> observeAndAdmit would
    /// HEAD this exact key during staging at an index < first_precommit_idx, failing here.
    bool adopted_blob_touched_before_precommit = false;
    for (int i = 0; i < first_precommit_idx; ++i)
    {
        if (log[i].key == source_blob_key)
        {
            adopted_blob_touched_before_precommit = true;
            ADD_FAILURE()
                << "Adopted committed-source blob op '" << log[i].op << "' on '" << log[i].key
                << "' at index " << i << " came BEFORE the first precommit write at index "
                << first_precommit_idx << " — violates B188 (committed-source adopt must not HEAD/GET/"
                << "PUT the adopted blob before precommit; expected a tokenless adoptEvidence dep)";
        }
    }
    EXPECT_FALSE(adopted_blob_touched_before_precommit);

    /// The committed-source adopt also must NOT re-upload the blob at all (content carried forward by
    /// reference): no writeObject on the source blob key in Phase 2.
    const bool reuploaded = std::any_of(log.begin(), log.end(),
        [&](const RecordingLocalObjectStorage::Record & r)
        { return r.op == "writeObject" && r.key == source_blob_key; });
    EXPECT_FALSE(reuploaded) << "Committed-source adopt re-uploaded the blob — should carry by reference";

    /// Sanity: the new part reads back and shares the source blob object.
    EXPECT_TRUE(storage->existsFile("uui/uuid-1/all_5_5_0/data.bin"));
    EXPECT_EQ(storage->getStorageObjects("uui/uuid-1/all_0_0_0/data.bin")[0].remote_path,
              storage->getStorageObjects("uui/uuid-1/all_5_5_0/data.bin")[0].remote_path);
}

/// B188 pending-blob hardlink (Task 6 Test 2): within a SINGLE transaction, write a content file
/// into part X (pending blob, not yet uploaded), then createHardLink that SAME file into part Y
/// (the cross-part pending-source branch: `&dst_st != src_st`, copies the PendingBlob record so
/// publishStaging uploads it for the dst part too). After commit both parts must read back the
/// identical content.
TEST(CaWiringPending, HardlinkOfPendingBlobCommitsAndReadsBack)
{
    auto storage = openWiringStorage();

    auto tx = storage->createTransaction();

    /// Write fresh content into part X — the blob is PENDING (not uploaded yet, temp-file only).
    writeThroughTransaction(*tx, "uui/uuid-1/all_10_10_0/data.bin", "pending-payload");

    /// Before commit, hardlink part X's file into part Y. At this point:
    ///   - src_st = staging for all_10_10_0 (exists: contains the pending blob)
    ///   - dst_st = staging for all_11_11_0 (created fresh here)
    ///   - &dst_st != src_st => PendingBlob is COPIED into dst_st.pending_blobs
    ///   - Both builds get recordPendingBlobDep (tokenless dep — no pool op until post-precommit)
    tx->createHardLink("uui/uuid-1/all_10_10_0/data.bin", "uui/uuid-1/all_11_11_0/data.bin");

    /// Nothing visible yet (B188: no uploads before precommit).
    EXPECT_FALSE(storage->existsFile("uui/uuid-1/all_10_10_0/data.bin"));
    EXPECT_FALSE(storage->existsFile("uui/uuid-1/all_11_11_0/data.bin"));

    tx->commit(DB::NoCommitOptions{});

    /// Both parts must be visible and carry the same content.
    ASSERT_TRUE(storage->existsFile("uui/uuid-1/all_10_10_0/data.bin"));
    ASSERT_TRUE(storage->existsFile("uui/uuid-1/all_11_11_0/data.bin"));

    EXPECT_EQ(storage->getFileSize("uui/uuid-1/all_10_10_0/data.bin"), 15u);   /// "pending-payload"
    EXPECT_EQ(storage->getFileSize("uui/uuid-1/all_11_11_0/data.bin"), 15u);

    /// Both parts must point to the SAME underlying blob object (content-addressed identity).
    auto objs_x = storage->getStorageObjects("uui/uuid-1/all_10_10_0/data.bin");
    auto objs_y = storage->getStorageObjects("uui/uuid-1/all_11_11_0/data.bin");
    ASSERT_EQ(objs_x.size(), 1u);
    ASSERT_EQ(objs_y.size(), 1u);
    EXPECT_EQ(objs_x[0].remote_path, objs_y[0].remote_path)
        << "Hardlinked pending blob must map to the SAME pool object in both parts";
}

/// ==== B190 Task 4: precommit-first for republishRef and committed-source createHardLink ====
///
/// B190-A: republishRef (called by moveDirectory for a COMMITTED part rename — RENAME TABLE, DETACH,
/// ATTACH, delete_tmp_ rename) must NOT call adoptTree (which HEADs the source tree) before precommit.
/// It should record a TOKENLESS tree-evidence dep and precommit FIRST. The source part's tree key
/// must not be accessed (exists / getObjectMetadata / readObject) before the first precommit write.
TEST(CaWiringPrecommitOrder, RepublishRefNoTreeHeadBeforePrecommit)
{
    auto recording = makeRecordingStorageForTest("republish");
    auto storage = std::make_shared<DB::ContentAddressedMetadataStorage>(
        recording, "pool", "srv1",
        std::filesystem::temp_directory_path() / "ca_b190_republish_scratch", nullptr);
    storage->startup();

    /// Phase 1: commit a source part. Capture its tree key from the /trees/ write.
    recording->ops.clear();
    {
        auto tx = storage->createTransaction();
        writeThroughTransaction(*tx, "uui/uuid-1/all_1_1_0/data.bin", "republish-source");
        tx->commit(DB::NoCommitOptions{});
    }
    std::string source_tree_key;
    for (const auto & r : recording->ops)
    {
        if (r.op == "writeObject" && r.key.find("/trees/") != std::string::npos)
        {
            source_tree_key = r.key;
            break;
        }
    }
    ASSERT_FALSE(source_tree_key.empty())
        << "Phase 1 recorded no /trees/ write — could not capture the source tree key";

    /// Phase 2: a COMMITTED rename (delete_tmp_ pattern) that triggers republishRef. Clear the log
    /// so only Phase 2's ops are analysed.
    recording->ops.clear();
    {
        auto tx = storage->createTransaction();
        tx->moveDirectory("uui/uuid-1/all_1_1_0", "uui/uuid-1/delete_tmp_all_1_1_0");
        tx->commit(DB::NoCommitOptions{});
    }

    const auto & log = recording->ops;

    const int first_precommit_idx = firstPrecommitWriteIdx(log);
    ASSERT_GE(first_precommit_idx, 0)
        << "No write to a /_precommits/ key was recorded — precommit step did not fire";

    /// The source tree key must NOT be accessed (HEAD via exists/getObjectMetadata, GET via readObject,
    /// or PUT via writeObject) before the precommit write. With adoptTree before precommit (the old
    /// code), observeAndAdmit HEADs the tree key at an index < first_precommit_idx, failing here.
    bool tree_touched_before_precommit = false;
    for (int i = 0; i < first_precommit_idx; ++i)
    {
        if (log[i].key == source_tree_key)
        {
            tree_touched_before_precommit = true;
            ADD_FAILURE()
                << "republishRef tree op '" << log[i].op << "' on '" << log[i].key
                << "' at index " << i << " came BEFORE the first precommit write at index "
                << first_precommit_idx << " — violates B190 precommit-first: republishRef must not "
                << "HEAD/GET/PUT the source tree before precommit (use tokenless adoptEvidence)";
        }
    }
    EXPECT_FALSE(tree_touched_before_precommit);

    /// Sanity: the renamed part is visible under the new name and NOT under the old name.
    EXPECT_FALSE(storage->existsDirectory("uui/uuid-1/all_1_1_0"));
    EXPECT_TRUE(storage->existsDirectory("uui/uuid-1/delete_tmp_all_1_1_0"));
    EXPECT_TRUE(storage->existsFile("uui/uuid-1/delete_tmp_all_1_1_0/data.bin"));
}

/// B190-B: the adoptStagedBlob helper unifies the 6 inline pending/uploaded adopt blocks from
/// createHardLink / moveFile / moveDirectory. The observable invariant: after refactoring, ALL
/// six sites still produce the same result as before — pending blobs are copied (hardlink) or
/// moved (moveFile/moveDirectory), and uploaded blobs are adopted by tokenless evidence. This test
/// exercises the non-trivial CROSS-PART pending path (createHardLink copies; moveFile moves) and
/// verifies both a copy and a move of the SAME pending source produce the correct committed state.
TEST(CaWiringPrecommitOrder, AdoptStagedBlobHelperUnifiesSixSites)
{
    /// Use a recording storage so we can verify no pre-precommit pool ops on own content.
    auto recording = makeRecordingStorageForTest("adopt_helper");
    auto storage = std::make_shared<DB::ContentAddressedMetadataStorage>(
        recording, "pool", "srv1",
        std::filesystem::temp_directory_path() / "ca_b190_adopt_scratch", nullptr);
    storage->startup();

    recording->ops.clear();

    /// One transaction: write a pending blob into part A, hardlink (COPY pending) into part B,
    /// and moveFile (MOVE pending) of a DIFFERENT pending blob from part A into part C.
    auto tx = storage->createTransaction();
    writeThroughTransaction(*tx, "uui/uuid-1/all_A_A_0/data.bin", "blob-for-copy");
    writeThroughTransaction(*tx, "uui/uuid-1/all_A_A_0/extra.bin", "blob-for-move");

    /// createHardLink = COPY semantics: both src and dst should see the blob after commit.
    tx->createHardLink("uui/uuid-1/all_A_A_0/data.bin", "uui/uuid-1/all_B_B_0/data.bin");

    /// moveFile cross-part = MOVE semantics: src loses the blob, dst gains it.
    {
        auto & ca_tx = dynamic_cast<DB::ContentAddressedTransaction &>(*tx);
        ca_tx.moveFile("uui/uuid-1/all_A_A_0/extra.bin", "uui/uuid-1/all_C_C_0/extra.bin");
    }

    tx->commit(DB::NoCommitOptions{});

    const auto & log = recording->ops;
    const int first_precommit_idx = firstPrecommitWriteIdx(log);
    ASSERT_GE(first_precommit_idx, 0)
        << "No precommit write recorded";

    /// Collect own content keys (blobs/trees this transaction wrote).
    std::set<std::string> own_content_keys;
    for (const auto & r : log)
        if (r.op == "writeObject"
            && (r.key.find("/blobs/") != std::string::npos || r.key.find("/trees/") != std::string::npos))
            own_content_keys.insert(r.key);

    /// No own-content pool op before precommit (B188 invariant extends to all adopt sites).
    for (int i = 0; i < static_cast<int>(log.size()); ++i)
    {
        if (!own_content_keys.contains(log[i].key))
            continue;
        EXPECT_GT(i, first_precommit_idx)
            << "Own-content op '" << log[i].op << "' on '" << log[i].key << "' at index " << i
            << " before precommit at " << first_precommit_idx;
    }

    /// COPY semantics: both A and B see the copied blob.
    EXPECT_TRUE(storage->existsFile("uui/uuid-1/all_A_A_0/data.bin"));
    EXPECT_TRUE(storage->existsFile("uui/uuid-1/all_B_B_0/data.bin"));
    EXPECT_EQ(storage->getFileSize("uui/uuid-1/all_A_A_0/data.bin"), 13u);   /// "blob-for-copy" (13 bytes)
    EXPECT_EQ(storage->getFileSize("uui/uuid-1/all_B_B_0/data.bin"), 13u);
    EXPECT_EQ(storage->getStorageObjects("uui/uuid-1/all_A_A_0/data.bin")[0].remote_path,
              storage->getStorageObjects("uui/uuid-1/all_B_B_0/data.bin")[0].remote_path)
        << "COPY (hardlink): both parts must share the same blob object";

    /// MOVE semantics: A loses extra.bin, C gains it.
    EXPECT_FALSE(storage->existsFile("uui/uuid-1/all_A_A_0/extra.bin"));
    EXPECT_TRUE(storage->existsFile("uui/uuid-1/all_C_C_0/extra.bin"));
    EXPECT_EQ(storage->getFileSize("uui/uuid-1/all_C_C_0/extra.bin"), 13u);   /// "blob-for-move" (13 bytes)
}

/// ==== B189: orphaned pending blob must NOT be uploaded after unlinkFile / replaceFile ====
///
/// When a file is written (pending blob X) and then unlinked (or replaced) within the same
/// transaction, X's tree entry is removed — so X is NOT referenced by the staged tree. Before the
/// B189 fix, publishStaging iterated pending_blobs unconditionally and uploaded X anyway (a wasted
/// PUT of an unreferenced blob). After the fix, publishStaging builds the set of blob hashes
/// referenced by the staged tree entries and uploads ONLY those — orphaned blobs are skipped.
///
/// The test uses RecordingLocalObjectStorage to capture every writeObject call. After commit it
/// checks that the orphaned blob's pool key received NO writeObject, while a kept blob (written and
/// NOT removed in the same transaction) IS uploaded.
TEST(CaWiringOps, OrphanedPendingBlobNotUploadedAfterUnlink)
{
    auto recording = makeRecordingStorageForTest("b189_unlink");
    auto storage = std::make_shared<DB::ContentAddressedMetadataStorage>(
        recording, "pool", "srv1",
        std::filesystem::temp_directory_path() / "ca_b189_unlink_scratch", nullptr);
    storage->startup();

    recording->ops.clear();

    auto tx = storage->createTransaction();

    /// Write blob X — this will be unlinked (orphaned) before commit.
    writeThroughTransaction(*tx, "uui/uuid-1/all_1_1_0/orphan.bin", "orphan-bytes");

    /// Write blob Y — this is kept (its tree entry survives to the staged tree).
    writeThroughTransaction(*tx, "uui/uuid-1/all_1_1_0/kept.bin", "kept-bytes");

    /// Unlink blob X — removes its tree entry; the pending_blobs record remains but is now orphaned.
    tx->unlinkFile("uui/uuid-1/all_1_1_0/orphan.bin", false, false);

    /// Sanity: the unlinked file is no longer staged (in-flight should not report it).
    EXPECT_FALSE(tx->tryGetInFlightFileSize("uui/uuid-1/all_1_1_0/orphan.bin").has_value());
    EXPECT_EQ(tx->tryGetInFlightFileSize("uui/uuid-1/all_1_1_0/kept.bin"), std::optional<uint64_t>(10));

    tx->commit(DB::NoCommitOptions{});

    const auto & log = recording->ops;

    /// Collect blob keys written by this transaction (only /blobs/ writeObjects).
    std::vector<std::string> blob_writes;
    for (const auto & r : log)
        if (r.op == "writeObject" && r.key.find("/blobs/") != std::string::npos)
            blob_writes.push_back(r.key);

    /// Exactly ONE blob must have been uploaded (the kept one). The orphaned blob's pool key must
    /// NOT appear in any writeObject — B189: orphan is filtered out of the publish upload.
    EXPECT_EQ(blob_writes.size(), 1u)
        << "Expected exactly 1 blob upload (the kept blob); got " << blob_writes.size()
        << ". If 2, the orphaned pending blob was uploaded — B189 regression.";

    /// The kept file is visible after commit; the orphaned file is not.
    EXPECT_TRUE(storage->existsFile("uui/uuid-1/all_1_1_0/kept.bin"));
    EXPECT_EQ(storage->getFileSize("uui/uuid-1/all_1_1_0/kept.bin"), 10u);   /// "kept-bytes"
    EXPECT_FALSE(storage->existsFile("uui/uuid-1/all_1_1_0/orphan.bin"));
}

/// B189 companion: the same orphan-filter applies when the tree entry is removed by replaceFile
/// (the destination entry erased before the move). Write blob X to dst, then replaceFile src->dst
/// (erases X's entry, moves src's entry to dst). The orphaned X must not be uploaded.
TEST(CaWiringOps, OrphanedPendingBlobNotUploadedAfterReplace)
{
    auto recording = makeRecordingStorageForTest("b189_replace");
    auto storage = std::make_shared<DB::ContentAddressedMetadataStorage>(
        recording, "pool", "srv1",
        std::filesystem::temp_directory_path() / "ca_b189_replace_scratch", nullptr);
    storage->startup();

    recording->ops.clear();

    auto tx = storage->createTransaction();

    /// Write blob X into the destination slot — it will be erased by replaceFile.
    writeThroughTransaction(*tx, "uui/uuid-1/all_1_1_0/data.bin", "original-bytes");

    /// Write blob Y into the source slot — it will replace the destination.
    writeThroughTransaction(*tx, "uui/uuid-1/all_1_1_0/new.bin", "replacement-bytes");

    /// replaceFile: erases the dst entry (X orphaned), then moves src->dst.
    {
        auto & ca_tx = dynamic_cast<DB::ContentAddressedTransaction &>(*tx);
        ca_tx.replaceFile("uui/uuid-1/all_1_1_0/new.bin", "uui/uuid-1/all_1_1_0/data.bin");
    }

    tx->commit(DB::NoCommitOptions{});

    const auto & log = recording->ops;

    /// Exactly ONE blob must have been uploaded (the replacement blob Y).
    std::vector<std::string> blob_writes;
    for (const auto & r : log)
        if (r.op == "writeObject" && r.key.find("/blobs/") != std::string::npos)
            blob_writes.push_back(r.key);

    EXPECT_EQ(blob_writes.size(), 1u)
        << "Expected exactly 1 blob upload (the replacement blob); got " << blob_writes.size()
        << ". If 2, the orphaned original blob was uploaded — B189 regression.";

    /// After commit the destination slot carries the replacement content.
    EXPECT_TRUE(storage->existsFile("uui/uuid-1/all_1_1_0/data.bin"));
    EXPECT_EQ(storage->getFileSize("uui/uuid-1/all_1_1_0/data.bin"), 17u);   /// "replacement-bytes"
    EXPECT_FALSE(storage->existsFile("uui/uuid-1/all_1_1_0/new.bin"));
}
