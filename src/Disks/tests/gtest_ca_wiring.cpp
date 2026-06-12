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
    EXPECT_TRUE(mobj[0].remote_path.empty());   /// sized placeholder; bytes ride prepareReadPipeline

    /// The publish stamp (.ca_mtime) backs getLastModified for the part dir and its files.
    EXPECT_EQ(storage->getLastModified("uui/uuid-1/all_1_1_0").epochTime(), 1700000000);
    EXPECT_EQ(storage->getLastModified("uui/uuid-1/all_1_1_0/data.bin").epochTime(), 1700000000);
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

TEST(CaWiringRead, DetachedNamespace)
{
    auto storage = openWiringStorage();
    publishWiredPart(*storage, storage->detachedNamespace("uuid-1"), "broken_all_1_1_0");

    /// The detached CONTAINER lists the detached part DIRECTORY names (B36's intent).
    EXPECT_TRUE(storage->existsDirectory("uui/uuid-1/detached"));
    EXPECT_EQ(storage->listDirectory("uui/uuid-1/detached"), (std::vector<std::string>{"broken_all_1_1_0"}));
    /// A single detached part dir + its files (per-part refs in the detached namespace — the new
    /// layout; the PoC's shared-ref re-keying is gone).
    EXPECT_TRUE(storage->existsDirectory("uui/uuid-1/detached/broken_all_1_1_0"));
    auto names = storage->listDirectory("uui/uuid-1/detached/broken_all_1_1_0");
    std::sort(names.begin(), names.end());
    EXPECT_EQ(names, (std::vector<std::string>{"data.bin", "metadata_version.txt", "p.proj", "uuid.txt"}));
    /// The B62 shape: a detached part's mutable file resolves through the detached namespace.
    EXPECT_TRUE(storage->existsFile("uui/uuid-1/detached/broken_all_1_1_0/metadata_version.txt"));
    EXPECT_EQ(storage->tryGetInManifestBytes("uui/uuid-1/detached/broken_all_1_1_0/metadata_version.txt"),
              std::optional<String>("5"));
    EXPECT_TRUE(storage->existsFile("uui/uuid-1/detached/broken_all_1_1_0/data.bin"));
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
    storage->store()->putNamespaceFile(storage->genericNamespace(), "clickhouse_access_check_xyz", "ok");

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

    /// Generic disk-root files route to the reserved generic namespace.
    EXPECT_TRUE(storage->existsFile("clickhouse_access_check_xyz"));
    EXPECT_EQ(storage->tryGetInManifestBytes("clickhouse_access_check_xyz"), std::optional<String>("ok"));
    EXPECT_FALSE(storage->existsFile("clickhouse_access_check_other"));
}
