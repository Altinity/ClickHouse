#include <gtest/gtest.h>
#include "cas_test_helpers.h"

#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/ContentAddressedMetadataStorage.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/ContentAddressedTransaction.h>
#include <Disks/DiskObjectStorage/ObjectStorages/Local/LocalObjectStorage.h>

#include <fmt/ranges.h>

#include <algorithm>
#include <atomic>
#include <filesystem>
#include <mutex>
#include <string>
#include <unistd.h>
#include <vector>

namespace DB::ErrorCodes
{
    extern const int CORRUPTED_DATA;
}

/// Per-TU declarations of the settings this file overrides, the pattern `cas_test_helpers.h`
/// documents: defined once in `ContentAddressedSettings.cpp`, declared by each consumer.
namespace DB::ContentAddressedSetting
{
    extern const ContentAddressedSettingsBool gc_enabled;
    extern const ContentAddressedSettingsUInt64 part_folder_cache_bytes;
    extern const ContentAddressedSettingsUInt64 manifest_decode_cache_bytes;
}

/// Directory probes on a path INSIDE a part (`<table>/<part>/<file>`) must be answered from the
/// part-folder view, never by a LIST of the table's `_files/` prefix. The metadata storage builds its
/// own backend from an `ObjectStoragePtr`, so the instrument sits at the `IObjectStorage` layer: a
/// `LocalObjectStorage` that counts every operation it is asked, by kind and key. Every method
/// `CasObjectStorageBackend` reaches is overridden, so "zero LISTs" is satisfied by the behaviour, not
/// by an unrecorded path.

using namespace DB::Cas::tests;

namespace
{

class CountingObjectStorage : public DB::LocalObjectStorage
{
public:
    using DB::LocalObjectStorage::LocalObjectStorage;

    enum class Kind { List, Get, Head, Put, Delete };

    bool exists(const DB::StoredObject & object) const override
    {
        record(Kind::Head, object.remote_path);
        return DB::LocalObjectStorage::exists(object);
    }

    std::unique_ptr<DB::ReadBufferFromFileBase> readObject(
        const DB::StoredObject & object, const DB::ReadSettings & read_settings,
        std::optional<size_t> read_hint, bool use_external_buffer,
        bool restrict_seek) const override
    {
        record(Kind::Get, object.remote_path);
        failIfArmed(object.remote_path);
        return DB::LocalObjectStorage::readObject(object, read_settings, read_hint, use_external_buffer, restrict_seek);
    }

    std::unique_ptr<DB::WriteBufferFromFileBase> writeObject(
        const DB::StoredObject & object, DB::WriteMode mode,
        std::optional<DB::ObjectAttributes> attributes,
        size_t buf_size,
        const DB::WriteSettings & write_settings) override
    {
        record(Kind::Put, object.remote_path);
        return DB::LocalObjectStorage::writeObject(object, mode, attributes, buf_size, write_settings);
    }

    void removeObjectIfExists(const DB::StoredObject & object) override
    {
        record(Kind::Delete, object.remote_path);
        DB::LocalObjectStorage::removeObjectIfExists(object);
    }

    void removeObjectsIfExist(const DB::StoredObjects & objects) override
    {
        for (const DB::StoredObject & object : objects)
            record(Kind::Delete, object.remote_path);
        DB::LocalObjectStorage::removeObjectsIfExist(objects);
    }

    DB::ObjectMetadata getObjectMetadata(const std::string & path, bool with_tags) const override
    {
        record(Kind::Head, path);
        return DB::LocalObjectStorage::getObjectMetadata(path, with_tags);
    }

    std::optional<DB::ObjectMetadata> tryGetObjectMetadata(const std::string & path, bool with_tags) const override
    {
        record(Kind::Head, path);
        return DB::LocalObjectStorage::tryGetObjectMetadata(path, with_tags);
    }

    void listObjects(const std::string & path, DB::RelativePathsWithMetadata & children, size_t max_keys) const override
    {
        record(Kind::List, path);
        DB::LocalObjectStorage::listObjects(path, children, max_keys);
    }

    bool existsOrHasAnyChild(const std::string & path) const override
    {
        record(Kind::List, path);
        return DB::LocalObjectStorage::existsOrHasAnyChild(path);
    }

    void copyObject(
        const DB::StoredObject & object_from, const DB::StoredObject & object_to,
        const DB::ReadSettings & read_settings, const DB::WriteSettings & write_settings,
        std::optional<DB::ObjectAttributes> object_to_attributes) override
    {
        record(Kind::Get, object_from.remote_path);
        record(Kind::Put, object_to.remote_path);
        DB::LocalObjectStorage::copyObject(object_from, object_to, read_settings, write_settings, object_to_attributes);
    }

    size_t listCount(std::string_view prefix) const { return count(Kind::List, prefix); }
    size_t getCount(std::string_view prefix) const { return count(Kind::Get, prefix); }
    [[maybe_unused]] size_t headCount(std::string_view prefix) const { return count(Kind::Head, prefix); }

    /// Every recorded key of `kind` containing `needle`, so a failure names the offender.
    std::vector<String> keys(Kind kind, std::string_view needle) const
    {
        std::lock_guard lock(mutex);
        std::vector<String> out;
        for (const auto & [k, key] : records)
            if (k == kind && key.find(needle) != String::npos)
                out.push_back(key);
        return out;
    }

    void reset()
    {
        std::lock_guard lock(mutex);
        records.clear();
    }

    /// Every GET of a key containing `needle` throws, so a resolved ref whose manifest cannot be
    /// read is an error, never a fall-through. `CORRUPTED_DATA` is a deterministic local failure
    /// (`isDeterministicLocalFailure`), so the read engine propagates it on the first attempt
    /// instead of retrying it to the lease budget like a transport fault.
    void failReadsContaining(String needle)
    {
        std::lock_guard lock(mutex);
        fail_reads_containing = std::move(needle);
    }

private:
    void record(Kind kind, const std::string & key) const
    {
        std::lock_guard lock(mutex);
        records.emplace_back(kind, key);
    }

    void failIfArmed(const std::string & key) const
    {
        std::lock_guard lock(mutex);
        if (!fail_reads_containing.empty() && key.find(fail_reads_containing) != String::npos)
            throw DB::Exception(DB::ErrorCodes::CORRUPTED_DATA, "CountingObjectStorage: injected read failure on '{}'", key);
    }

    size_t count(Kind kind, std::string_view prefix) const
    {
        std::lock_guard lock(mutex);
        size_t n = 0;
        for (const auto & [k, key] : records)
            if (k == kind && key.starts_with(prefix))
                ++n;
        return n;
    }

    mutable std::mutex mutex;
    mutable std::vector<std::pair<Kind, String>> records;
    String fail_reads_containing;
};

const std::string kTbl = "a11/a11a11a1-1111-4111-8111-111111111111";
const std::string kNonAtomicTbl = "data/db/tbl";

/// Owns the pool's two on-disk temp directories (object-storage root and metadata scratch dir) and
/// removes both once the metadata storage they back is gone, so a test run does not leak two
/// directories under the temp dir per call to `openCountingStorage`. `storage->`/`*storage` forward
/// to the held metadata storage, so callers use it exactly like the `shared_ptr` it replaces.
class CountingStoragePool
{
public:
    /// Owns the two directories from construction, before a metadata storage exists: a throwing
    /// `startup()` still unwinds through this object's destructor and removes them.
    CountingStoragePool(std::string root_, std::string scratch_)
        : root(std::move(root_)), scratch(std::move(scratch_))
    {
    }

    CountingStoragePool(const CountingStoragePool &) = delete;
    CountingStoragePool & operator=(const CountingStoragePool &) = delete;
    /// Needed for the by-value return from `openCountingStorage`: NRVO is not guaranteed for a
    /// named local, and the deleted copy constructor above suppresses the implicit move
    /// constructor the language would otherwise synthesize.
    CountingStoragePool(CountingStoragePool &&) = default;

    ~CountingStoragePool()
    {
        storage.reset();
        std::error_code ec;
        std::filesystem::remove_all(root, ec);
        std::filesystem::remove_all(scratch, ec);
    }

    void attach(std::shared_ptr<DB::ContentAddressedMetadataStorage> storage_) { storage = std::move(storage_); }

    DB::ContentAddressedMetadataStorage * operator->() const { return storage.get(); }
    DB::ContentAddressedMetadataStorage & operator*() const { return *storage; }

private:
    std::shared_ptr<DB::ContentAddressedMetadataStorage> storage;
    std::string root;
    std::string scratch;
};

CountingStoragePool openCountingStorage(
    std::shared_ptr<CountingObjectStorage> & out_object_storage, bool disable_caches)
{
    static std::atomic<uint64_t> counter{0};
    const String unique = std::to_string(::getpid()) + "_" + std::to_string(counter.fetch_add(1));
    const auto root = (std::filesystem::temp_directory_path() / ("cas_dir_probes_" + unique)).string();
    const auto scratch = (std::filesystem::temp_directory_path() / ("cas_dir_probes_scratch_" + unique)).string();

    /// Take directory ownership BEFORE creating anything under `root`, so a throwing `startup()`
    /// below still leaves cleanup to this object's destructor.
    CountingStoragePool pool(root, scratch);

    std::error_code ec;
    std::filesystem::remove_all(root, ec);
    std::filesystem::create_directories(root, ec);
    if (ec)
        throw std::runtime_error("openCountingStorage: create_directories(" + root + ") failed: " + ec.message());

    out_object_storage = std::make_shared<CountingObjectStorage>(
        DB::LocalObjectStorageSettings("test", root, /*read_only_=*/false));

    auto settings = makeSettingsForTest("test", scratch);
    /// A GC round LISTs on its own schedule; a timer is not a fence, so keep it off.
    settings[DB::ContentAddressedSetting::gc_enabled] = false;
    if (disable_caches)
    {
        settings[DB::ContentAddressedSetting::part_folder_cache_bytes] = 0;
        settings[DB::ContentAddressedSetting::manifest_decode_cache_bytes] = 0;
    }
    auto storage = std::make_shared<DB::ContentAddressedMetadataStorage>(
        out_object_storage, "pool", "srv1", "", nullptr, settings);
    storage->startup();
    pool.attach(std::move(storage));
    return pool;
}

/// Publishes one part through the real transaction path: every (relative file, bytes) pair is written
/// and the transaction is committed, exactly as a MergeTree part write does.
void publishPart(DB::ContentAddressedMetadataStorage & storage, const std::string & part_path,
                 const std::vector<std::pair<std::string, std::string>> & files)
{
    auto tx = storage.createTransaction();
    auto & ca_tx = dynamic_cast<DB::ContentAddressedTransaction &>(*tx);
    for (const auto & [file, bytes] : files)
    {
        auto buf = ca_tx.writeFile(part_path + "/" + file, 65536, DB::WriteMode::Rewrite, {});
        buf->write(bytes.data(), bytes.size());
        buf->finalize();
    }
    tx->commit(DB::NoCommitOptions{});
}

/// The table's readable namespace-files life, the handle every verbatim (non-part) table file is
/// keyed under. A null life means the test set the table up wrong (no live life to name), so this
/// throws rather than falling back to a literal that would silently match no recorded key and pass
/// a LIST-count assertion vacuously.
DB::Cas::NamespaceLifeId namespaceFilesLifeOf(DB::ContentAddressedMetadataStorage & storage, const std::string & table_path)
{
    const auto uuid = table_path.substr(table_path.find_last_of('/') + 1);
    const auto life = storage.readableNamespaceFilesLife(storage.liveNamespace(uuid));
    if (!life)
        throw std::runtime_error("namespaceFilesLifeOf: no readable namespace-files life for " + table_path);
    return *life;
}

std::string filesPrefixOf(DB::ContentAddressedMetadataStorage & storage, const std::string & table_path)
{
    /// The LIST that must not happen: the life's `_files/` prefix. Resolved through the storage so a
    /// layout change moves the assertion instead of silently matching nothing.
    return storage.store()->layout().namespaceFilesPrefix(namespaceFilesLifeOf(storage, table_path));
}

}

/// A file of a published part is not a directory; a non-projection nested directory is, lists its
/// children and is not empty; a projection directory keeps its deliberate empty answer. None of it
/// LISTs the table's `_files/` prefix, with or without the view caches.
class CASDirectoryProbes : public ::testing::TestWithParam<bool> {};

TEST_P(CASDirectoryProbes, PartFileAnswersFromTheViewWithoutAList)
{
    const bool disable_caches = GetParam();
    std::shared_ptr<CountingObjectStorage> os;
    auto storage = openCountingStorage(os, disable_caches);

    const std::string part = kTbl + "/all_1_1_0";
    publishPart(*storage, part, {
        {"columns.txt", "cols"}, {"data.bin", "data-bytes"},
        {"sub/inner.bin", "inner"}, {"p.proj/data.bin", "proj-bytes"}});
    /// The load has happened; from here on every probe must be answered from what it retained.
    ASSERT_TRUE(storage->existsDirectory(part));
    /// `existsDirectory(part)` above resolves the PartDir shape via `existsRef`, which never
    /// touches the part-folder view; prime the view/manifest-decode cache explicitly through a
    /// PartFile-shaped probe so the warm-cache assertions below measure the REPEAT probes, not the
    /// one-time cold decode a commit's cache invalidation leaves behind.
    ASSERT_FALSE(storage->existsDirectory(part + "/columns.txt"));
    const std::string files_prefix = filesPrefixOf(*storage, kTbl);
    os->reset();

    EXPECT_FALSE(storage->existsDirectory(part + "/columns.txt"));
    EXPECT_FALSE(storage->existsDirectory(part + "/data.bin"));
    EXPECT_TRUE(storage->existsDirectory(part + "/sub"));
    EXPECT_TRUE(storage->existsDirectory(part + "/sub/"));            /// trailing slash
    EXPECT_EQ(storage->listDirectory(part + "/sub"), (std::vector<std::string>{"inner.bin"}));
    EXPECT_TRUE(storage->listDirectory(part + "/columns.txt").empty());   /// a plain file lists empty
    EXPECT_FALSE(storage->isDirectoryEmpty(part + "/sub"));
    EXPECT_TRUE(storage->isDirectoryEmpty(part + "/p.proj"));          /// ProjectionDir keeps its answer
    EXPECT_TRUE(storage->existsDirectory(part + "/p.proj"));
    EXPECT_FALSE(storage->existsDirectory(part + "/absent"));

    /// A LOCAL-backed pool answers every emulated operation through `StoredObject(emu_root + "/" +
    /// key)` (`ObjectStorageBackend::emuPath`), so a recorded key carries the object storage's own
    /// key prefix ahead of the logical CAS key that `casManifestsPrefix`/the `_files` prefix name.
    const std::string emu_root = os->getCommonKeyPrefix() + "/";
    EXPECT_EQ(os->listCount(emu_root + files_prefix), 0u) << "keys: " << fmt::to_string(fmt::join(os->keys(CountingObjectStorage::Kind::List, "_files"), ", "));
    EXPECT_EQ(os->listCount(""), 0u) << "no LIST of any prefix for probes inside a resolved part";
    if (!disable_caches)
        EXPECT_EQ(os->getCount(""), 0u) << "warm view: no GET at all";
    else
    {
        /// Nine of the ten probes above reach `getView` (all but `isDirectoryEmpty(p.proj)`, which
        /// short-circuits on the `ProjectionDir` prefix without touching the view). With both caches
        /// disabled, every `getView` call issues exactly one manifest `GET` and nothing else, so both
        /// counts must be exactly nine, not merely positive.
        const std::string manifests_prefix = emu_root + storage->store()->layout().casManifestsPrefix();
        EXPECT_EQ(os->getCount(manifests_prefix), 9u) << "cold caches: one manifest GET per probe reaching the view";
        EXPECT_EQ(os->getCount(""), 9u) << "cold caches: nothing else is GET";
    }
}

INSTANTIATE_TEST_SUITE_P(CASCaches, CASDirectoryProbes, ::testing::Bool(),
    [](const ::testing::TestParamInfo<bool> & param_info) { return param_info.param ? "Disabled" : "Default"; });

/// Detached and non-Atomic parts route through the same shape and the same view.
TEST(CASDirectoryProbes, DetachedAndNonAtomicPartFilesAnswerWithoutAList)
{
    std::shared_ptr<CountingObjectStorage> os;
    auto storage = openCountingStorage(os, /*disable_caches=*/false);

    const std::string part = kTbl + "/all_2_2_0";
    publishPart(*storage, part, {{"columns.txt", "cols"}, {"sub/x.bin", "x"}});
    {
        auto tx = storage->createTransaction();
        tx->moveDirectory(part, kTbl + "/detached/all_2_2_0");
        tx->commit(DB::NoCommitOptions{});
    }
    const std::string detached = kTbl + "/detached/all_2_2_0";
    ASSERT_TRUE(storage->existsDirectory(detached));

    const std::string na_part = kNonAtomicTbl + "/all_1_1_0";
    publishPart(*storage, na_part, {{"columns.txt", "cols"}, {"sub/y.bin", "y"}});
    ASSERT_TRUE(storage->existsDirectory(na_part));

    os->reset();
    EXPECT_FALSE(storage->existsDirectory(detached + "/columns.txt"));
    EXPECT_TRUE(storage->existsDirectory(detached + "/sub"));
    EXPECT_FALSE(storage->existsDirectory(na_part + "/columns.txt"));
    EXPECT_TRUE(storage->existsDirectory(na_part + "/sub"));
    EXPECT_EQ(os->listCount(""), 0u) << "keys: " << fmt::to_string(fmt::join(os->keys(CountingObjectStorage::Kind::List, ""), ", "));
}

/// An unresolved ref is not a part: the old branch answers, with today's one LIST. Exact oracle:
/// answer and LIST count per probe.
TEST(CASDirectoryProbes, UnresolvedRefKeepsTheTableSubdirBranchAndItsOneList)
{
    std::shared_ptr<CountingObjectStorage> os;
    auto storage = openCountingStorage(os, /*disable_caches=*/false);
    /// A real part so the table has a live life and a resident ref table.
    publishPart(*storage, kTbl + "/all_1_1_0", {{"columns.txt", "cols"}});
    /// "custom" sits directly under the table's UUID, so the path parser anchors it exactly like a
    /// real part component; the disk write path (`tryCreateWriteBuffer`) therefore cannot write
    /// through it as a verbatim file (it always routes a part-shaped path into the part-write
    /// machinery). Writing straight through the namespace-file primitive `tableSubdirExists` itself
    /// reads is what makes "custom" content on disk without ever publishing it as a part ref.
    storage->store()->putNamespaceFile(namespaceFilesLifeOf(*storage, kTbl), "custom/sub/x", "x");
    const std::string emu_root = os->getCommonKeyPrefix() + "/";
    const std::string files_prefix = emu_root + filesPrefixOf(*storage, kTbl);

    os->reset();
    EXPECT_TRUE(storage->existsDirectory(kTbl + "/custom/sub"));
    EXPECT_EQ(os->listCount(files_prefix), 1u);

    os->reset();
    EXPECT_FALSE(storage->existsDirectory(kTbl + "/custom/nothere"));
    EXPECT_EQ(os->listCount(files_prefix), 1u);

    os->reset();   /// a part-shaped name with no such part
    EXPECT_FALSE(storage->existsDirectory(kTbl + "/all_9_9_0/columns.txt"));
    EXPECT_EQ(os->listCount(files_prefix), 1u);

    os->reset();   /// non-Atomic, unresolved: the generic live-tree probe, not the `_files/` prefix
    EXPECT_FALSE(storage->existsDirectory(kNonAtomicTbl + "/all_9_9_0/f"));
    EXPECT_EQ(os->listCount(files_prefix), 0u);
    EXPECT_EQ(os->listCount(""), 1u) << "keys: " << fmt::to_string(fmt::join(os->keys(CountingObjectStorage::Kind::List, ""), ", "));
}

/// A part being written in an open transaction is not published: its ref does not resolve through
/// the storage, so the probe takes today's branch and today's answer.
TEST(CASDirectoryProbes, UnpublishedPartFallsThroughLikeToday)
{
    std::shared_ptr<CountingObjectStorage> os;
    auto storage = openCountingStorage(os, /*disable_caches=*/false);
    publishPart(*storage, kTbl + "/all_1_1_0", {{"columns.txt", "cols"}});

    auto tx = storage->createTransaction();
    auto & ca_tx = dynamic_cast<DB::ContentAddressedTransaction &>(*tx);
    const std::string staged = kTbl + "/tmp_insert_all_2_2_0";
    auto buf = ca_tx.writeFile(staged + "/sub/data.bin", 65536, DB::WriteMode::Rewrite, {});
    buf->write("d", 1);
    buf->finalize();

    const std::string emu_root = os->getCommonKeyPrefix() + "/";
    const std::string files_prefix = emu_root + filesPrefixOf(*storage, kTbl);
    os->reset();
    EXPECT_NO_THROW(EXPECT_FALSE(storage->existsDirectory(staged + "/sub")));
    EXPECT_EQ(os->listCount(files_prefix), 1u) << "unpublished: the old branch and its LIST, unchanged";
    tx->commit(DB::NoCommitOptions{});
}

/// Failure is not absence: a resolved ref whose manifest cannot be read throws; the old LIST branch
/// is never entered on a failed request.
TEST(CASDirectoryProbes, FailedManifestReadPropagatesAndDoesNotList)
{
    std::shared_ptr<CountingObjectStorage> os;
    auto storage = openCountingStorage(os, /*disable_caches=*/true);
    const std::string part = kTbl + "/all_1_1_0";
    publishPart(*storage, part, {{"columns.txt", "cols"}});
    const std::string emu_root = os->getCommonKeyPrefix() + "/";
    const std::string files_prefix = emu_root + filesPrefixOf(*storage, kTbl);
    const std::string manifests_prefix = emu_root + storage->store()->layout().casManifestsPrefix();

    os->failReadsContaining(manifests_prefix);
    os->reset();
    try
    {
        storage->existsDirectory(part + "/columns.txt");
        FAIL() << "expected the injected manifest-read failure to propagate";
    }
    catch (const DB::Exception & e)
    {
        EXPECT_EQ(e.code(), DB::ErrorCodes::CORRUPTED_DATA);
        EXPECT_NE(e.message().find(manifests_prefix), std::string::npos) << e.message();
    }
    /// One attempted manifest GET, no reissue: `CORRUPTED_DATA` is a deterministic local failure, so
    /// the read engine never retries it to the lease budget.
    EXPECT_EQ(os->getCount(manifests_prefix), 1u);
    EXPECT_EQ(os->listCount(files_prefix), 0u);
    EXPECT_EQ(os->listCount(""), 0u);
}

/// The load profile: after the one legitimate table-directory enumeration, probing every file of
/// every part adds no LIST.
TEST(CASDirectoryProbes, CheckSizeProbesOfFiftyPartsAddNoList)
{
    std::shared_ptr<CountingObjectStorage> os;
    auto storage = openCountingStorage(os, /*disable_caches=*/false);
    const std::vector<std::string> files = {"columns.txt", "checksums.txt", "count.txt", "data.bin", "data.cmrk3", "primary.cidx"};
    for (int i = 1; i <= 50; ++i)
    {
        std::vector<std::pair<std::string, std::string>> contents;
        for (const auto & f : files)
            contents.emplace_back(f, "bytes-" + std::to_string(i));
        publishPart(*storage, kTbl + "/all_" + std::to_string(i) + "_" + std::to_string(i) + "_0", contents);
    }
    const std::string emu_root = os->getCommonKeyPrefix() + "/";
    const std::string files_prefix = emu_root + filesPrefixOf(*storage, kTbl);

    /// The table directory enumeration a load does once (its one `_files/` LIST is legitimate).
    auto names = storage->listDirectory(kTbl);
    ASSERT_EQ(std::count_if(names.begin(), names.end(), [](const auto & n) { return n.starts_with("all_"); }), 50);
    os->reset();

    for (const auto & name : names)
        if (name.starts_with("all_"))
            for (const auto & f : files)
                EXPECT_FALSE(storage->existsDirectory(kTbl + "/" + name + "/" + f));

    EXPECT_EQ(os->listCount(files_prefix), 0u) << "keys: " << fmt::to_string(fmt::join(os->keys(CountingObjectStorage::Kind::List, "_files"), ", "));
    EXPECT_EQ(os->listCount(""), 0u);
}
