#pragma once

#include "config.h"

#if USE_AWS_S3

#include <Disks/DiskObjectStorage/ObjectStorages/Local/LocalObjectStorage.h>
#include <Disks/tests/cas_scoped_test_directory.h>
#include <IO/ReadBufferFromMemory.h>
#include <IO/S3Common.h>
#include <IO/WriteBufferFromFileBase.h>
#include <IO/WriteSettings.h>
#include <Common/Exception.h>
#include <Common/SipHash.h>
#include <base/hex.h>

#include <aws/s3/S3Errors.h>

#include <atomic>
#include <exception>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include <unistd.h>

namespace DB::ErrorCodes
{
extern const int BAD_ARGUMENTS;
}

namespace DB::Cas::tests
{

/// A `LocalObjectStorage` that reports the GCS generation dialect
/// (`conditionalOpsUseGenerationTokens() == true`) and a non-`Local` `getType()`, so
/// `ContentAddressedMetadataStorage::openPoolView` builds its backend in `Mode::Native` with
/// `native_token_type == Dialect::Generation`. The fake also advertises native copy so generation
/// token mode can exercise explicit S3 staging without endpoint/provider heuristics.
///
/// Holds every object entirely in memory, keyed by the BARE CAS key exactly as `Backend` hands it to
/// `object_storage` (e.g. `"pool/_probe/<hex>/token"`). Native mode never asks `object_storage` to
/// resolve that key against anything (`ContentAddressedMetadataStorage::physicalKey` is a documented
/// no-op for Native, since a real S3 client resolves a bucket-relative key against its own bucket/prefix
/// configuration internally) -- so this fake never needs a notion of "resolve a key to a location" at
/// all, unlike a real filesystem-backed object storage would. That sidesteps the class of bug a
/// resolve-then-strip round trip through the real `LocalObjectStorage` file/list implementation is prone
/// to (a key is a key, with no round trip to get wrong), and it never touches the real filesystem, so it
/// cannot leak files into the test process's working directory either.
///
/// A writable Native-mode mount always runs the mandatory capability battery (`CasProbe.cpp`), which
/// requires REAL conditional-write enforcement: the precondition is evaluated when a write completes,
/// signaled by an `S3Exception` carrying the canonical `PreconditionFailed` name (see
/// `ObjectStorageBackend::finalizeConditionalWrite`). `writeObject` therefore buffers bytes in memory and
/// defers both the precondition check and the commit to `finalize`, mirroring how a real object store
/// only commits -- and only then can reject -- on PUT completion.
class FakeGenerationObjectStorage final : public DB::LocalObjectStorage
{
public:
    using DB::LocalObjectStorage::LocalObjectStorage;

    /// Report the S3 ETag dialect instead: a token is a digest of the object's bytes, so the same bytes
    /// written or copied again carry the same token, as on S3 without KMS. Set before `startup`.
    bool content_derived_etags = false;

    DB::ObjectStorageType getType() const override { return DB::ObjectStorageType::S3; }
    bool conditionalOpsUseGenerationTokens() const override { return !content_derived_etags; }
    std::optional<bool> isBucketVersioningEnabled(const DB::ObjectStorageControlRequest &) const override { return false; }
    bool supportsRetryProfile(DB::ObjectStorageRetryProfile) const override { return true; }
    bool supportsCopyMode(DB::ObjectStorageCopyMode mode) const override
    {
        return mode == DB::ObjectStorageCopyMode::Default || mode == DB::ObjectStorageCopyMode::NativeOnly;
    }

    /// One `writeObject` or `copyObject` as the store received it.
    struct WriteRecord
    {
        std::string key;
        DB::WriteSettings settings;
        bool copy = false;
        bool throttled = false;
        std::exception_ptr throttle_error;
    };

    /// Keys whose write calls its `remote_throttler->throttle(32768, 0)` once before the store takes the bytes, as a
    /// socket does at a send-budget refill. What that call throws fails the write. Set before the writes start.
    std::function<bool(const std::string & key)> throttle_while_writing;
    /// Runs on the writing thread right before that call.
    std::function<void(const std::string & key)> before_throttle;

    /// Every write and copy so far, in the order the store received them.
    std::vector<std::shared_ptr<const WriteRecord>> records() const
    {
        std::lock_guard lock(mutex);
        return {recorded.begin(), recorded.end()};
    }

    std::unique_ptr<DB::WriteBufferFromFileBase> writeObject(
        const DB::StoredObject & object,
        DB::WriteMode mode,
        std::optional<DB::ObjectAttributes> /*attributes*/,
        size_t /*buf_size*/,
        const DB::WriteSettings & write_settings) override
    {
        if (mode != DB::WriteMode::Rewrite)
            throw DB::Exception(DB::ErrorCodes::BAD_ARGUMENTS, "FakeGenerationObjectStorage only supports Rewrite");
        auto record = std::make_shared<WriteRecord>();
        record->key = object.remote_path;
        record->settings = write_settings;
        {
            std::lock_guard lock(mutex);
            recorded.push_back(record);
        }
        return std::make_unique<ConditionalWriteBuffer>(
            *this, object.remote_path, write_settings.object_storage_write_if_none_match,
            write_settings.object_storage_write_if_match, std::move(record));
    }

    bool exists(const DB::StoredObject & object) const override
    {
        std::lock_guard lock(mutex);
        return objects.contains(object.remote_path);
    }

    std::unique_ptr<DB::ReadBufferFromFileBase> readObject(
        const DB::StoredObject & object,
        const DB::ReadSettings & /*read_settings*/,
        std::optional<size_t> /*read_hint*/,
        bool /*use_external_buffer*/,
        bool /*restrict_seek*/) const override
    {
        std::lock_guard lock(mutex);
        auto it = objects.find(object.remote_path);
        if (it == objects.end())
            /// `RESOURCE_NOT_FOUND`, not a plain `DB::Exception`: `Backend::probeSentinelRaw`'s Native
            /// path (`CasObjectStorageBackend.cpp`) classifies absence ONLY from a caught `S3Exception`
            /// carrying `NO_SUCH_KEY`/`RESOURCE_NOT_FOUND` (or the matching exception name) -- anything
            /// else, including an unrecognized exception TYPE, falls through to `ProbeOutcome::
            /// Indeterminate` (fail-closed). A bodyless HEAD on a real absent S3 key throws exactly this
            /// code, since the SDK cannot parse a `<Code>` from a body that was never sent.
            throw DB::S3Exception("FakeGenerationObjectStorage: object does not exist",
                                   Aws::S3::S3Errors::RESOURCE_NOT_FOUND);
        /// Copies the bytes, matching a real remote read (no shared ownership with the stored entry, so
        /// a later overwrite of this key cannot mutate bytes a caller is still reading).
        return std::make_unique<DB::ReadBufferFromOwnMemoryFile>(object.remote_path, it->second.bytes);
    }

    DB::ObjectMetadata getObjectMetadata(const std::string & path, bool with_tags) const override
    {
        auto metadata = tryGetObjectMetadata(path, with_tags);
        if (!metadata)
            throw DB::S3Exception("FakeGenerationObjectStorage: object does not exist",
                                   Aws::S3::S3Errors::RESOURCE_NOT_FOUND);
        return *metadata;
    }

    std::optional<DB::ObjectMetadata> tryGetObjectMetadata(const std::string & path, bool /*with_tags*/) const override
    {
        std::lock_guard lock(mutex);
        auto it = objects.find(path);
        if (it == objects.end())
            return std::nullopt;
        DB::ObjectMetadata metadata;
        metadata.size_bytes = it->second.bytes.size();
        metadata.etag = tokenOf(it->second);
        return metadata;
    }

    std::optional<DB::ObjectMetadata> tryGetObjectMetadataWithNativeToken(const std::string & path, bool with_tags) const override
    {
        return tryGetObjectMetadata(path, with_tags);
    }

    /// This fake advertises every retry profile, and an in-memory store has no retry behaviour to
    /// vary, so the profile-aware overloads simply forward. A storage that claimed the capability
    /// without implementing them would refuse every control-plane request of a writable mount.
    std::optional<DB::ObjectMetadata> tryGetObjectMetadataWithNativeToken(
        const std::string & path, bool with_tags, const DB::ObjectStorageControlRequest &) const override
    {
        return tryGetObjectMetadata(path, with_tags);
    }

    DB::ObjectStorageIteratorPtr iterate(
        const std::string & path_prefix, size_t max_keys, bool with_tags, const std::optional<std::string> & start_after,
        const DB::ObjectStorageControlRequest &) const override
    {
        return DB::LocalObjectStorage::iterate(path_prefix, max_keys, with_tags, start_after);
    }

    DB::ConditionalRemoveResult removeObjectIfTokenMatches(
        const DB::StoredObject & object, const std::string & etag, const DB::ObjectStorageControlRequest &) override
    {
        return removeObjectIfTokenMatches(object, etag);
    }
    using DB::LocalObjectStorage::removeObjectIfTokenMatches;

    /// A real S3 GET answers with the object's incarnation, which is what the backend reads its
    /// bytes AND its generation from in one request. Quoted, the way the SDK's ETag field carries a
    /// generation across the HTTP boundary.
    DB::SmallObjectDataWithMetadata readSmallObjectAndGetObjectMetadata(
        const DB::StoredObject & object, const DB::ReadSettings &, size_t, std::optional<size_t>) const override
    {
        std::lock_guard lock(mutex);
        auto it = objects.find(object.remote_path);
        if (it == objects.end())
            throw DB::S3Exception("FakeGenerationObjectStorage: object does not exist",
                                   Aws::S3::S3Errors::RESOURCE_NOT_FOUND);
        DB::SmallObjectDataWithMetadata result;
        result.data = it->second.bytes;
        result.metadata.size_bytes = it->second.bytes.size();
        result.metadata.etag = transportTokenOf(it->second);
        return result;
    }

    void removeObjectIfExists(const DB::StoredObject & object) override
    {
        std::lock_guard lock(mutex);
        objects.erase(object.remote_path);
    }

    void removeObjectsIfExist(const DB::StoredObjects & objects_to_remove) override
    {
        std::lock_guard lock(mutex);
        for (const auto & object : objects_to_remove)
            objects.erase(object.remote_path);
    }

    /// S3 sends an unconditional delete under every retry profile; the intent probe and GC use it.
    void removeObjectsIfExistUnderProfile(const DB::StoredObjects & objects_to_remove, const DB::ObjectStorageControlRequest &) override
    {
        removeObjectsIfExist(objects_to_remove);
    }

    /// `path` is a bare CAS-relative prefix (e.g. `"pool/"` or `"pool/_probe/<hex>"`) -- exactly what
    /// `Backend::list`'s Native-mode path passes through unchanged (it applies no prefix stripping of
    /// its own there) and expects back on every listed key. A plain string-prefix scan over the
    /// in-memory keys already IS that key space, so there is no separate "physical" representation to
    /// resolve to or strip back off.
    void listObjects(const std::string & path, DB::RelativePathsWithMetadata & children, size_t max_keys) const override
    {
        std::lock_guard lock(mutex);
        for (const auto & [key, entry] : objects)
        {
            if (!key.starts_with(path))
                continue;
            DB::ObjectMetadata metadata;
            metadata.size_bytes = entry.bytes.size();
            metadata.etag = tokenOf(entry);
            children.push_back(std::make_shared<DB::RelativePathWithMetadata>(key, std::move(metadata)));
            if (max_keys != 0 && children.size() >= max_keys)
                break;
        }
    }

    DB::ConditionalRemoveResult removeObjectIfTokenMatches(const DB::StoredObject & object, const std::string & etag) override
    {
        std::lock_guard lock(mutex);
        DB::ConditionalRemoveResult result;
        auto it = objects.find(object.remote_path);
        if (it == objects.end())
        {
            result.outcome = DB::ConditionalRemoveOutcome::NotFound;
            return result;
        }
        if (tokenOf(it->second) != etag)
        {
            result.outcome = DB::ConditionalRemoveOutcome::TokenMismatch;
            return result;
        }
        objects.erase(it);
        result.outcome = DB::ConditionalRemoveOutcome::Removed;
        return result;
    }

    /// Checks the write-once/exact-token precondition against the current generation and, on success,
    /// stores `bytes` and mints the next generation. Throws an `S3Exception` naming `PreconditionFailed`
    /// on a lost condition -- the one signal `finalizeConditionalWrite` classifies as
    /// `ConditionalWriteOutcome::PreconditionLost` rather than an ordinary failure.
    /// Returns the new object's token as a real store returns it in the write response: the backend
    /// attributes the write to that token and nothing reads it back.
    std::string commitConditionalWrite(const std::string & key, const std::string & bytes,
                                 const std::string & if_none_match, const std::string & if_match)
    {
        std::lock_guard lock(mutex);
        auto it = objects.find(key);
        const bool exists_now = it != objects.end();
        if (!if_none_match.empty() && exists_now)
            throw DB::S3Exception("FakeGenerationObjectStorage: if-none-match precondition failed",
                                   Aws::S3::S3Errors::UNKNOWN, "PreconditionFailed");
        if (!if_match.empty() && (!exists_now || tokenOf(it->second) != if_match))
            throw DB::S3Exception("FakeGenerationObjectStorage: if-match precondition failed",
                                   Aws::S3::S3Errors::UNKNOWN, "PreconditionFailed");

        Entry & entry = objects[key];
        entry = Entry{bytes, next_generation++};
        return transportTokenOf(entry);
    }

    /// A same-store copy of the stored bytes, counted per destination key.
    void copyObject( /// NOLINT
        const DB::StoredObject & object_from,
        const DB::StoredObject & object_to,
        const DB::ReadSettings & /*read_settings*/,
        const DB::WriteSettings & write_settings,
        std::optional<DB::ObjectAttributes> /*object_to_attributes*/ = {}) override
    {
        std::lock_guard lock(mutex);
        auto record = std::make_shared<WriteRecord>();
        record->key = object_to.remote_path;
        record->settings = write_settings;
        record->copy = true;
        recorded.push_back(std::move(record));
        auto it = objects.find(object_from.remote_path);
        if (it == objects.end())
            throw DB::S3Exception("FakeGenerationObjectStorage: copy source does not exist",
                                   Aws::S3::S3Errors::RESOURCE_NOT_FOUND);
        Entry copy{it->second.bytes, next_generation++};
        objects[object_to.remote_path] = std::move(copy);
        ++copies_to[object_to.remote_path];
    }

    std::map<std::string, size_t> copiesByDestination() const
    {
        std::lock_guard lock(mutex);
        return copies_to;
    }

private:
    struct Entry
    {
        std::string bytes;
        uint64_t generation;
    };

    std::string tokenOf(const Entry & entry) const
    {
        if (content_derived_etags)
            return getHexUIntLowercase(sipHash128(entry.bytes.data(), entry.bytes.size()));
        return std::to_string(entry.generation);
    }

    /// The token as the SDK's ETag field carries it: quoted for a GCS generation.
    std::string transportTokenOf(const Entry & entry) const
    {
        return content_derived_etags ? tokenOf(entry) : "\"" + tokenOf(entry) + "\"";
    }

    /// Buffers the whole body in memory so the entry is committed exactly once, at `finalize`, and only after the
    /// precondition has been checked.
    class ConditionalWriteBuffer final : public DB::WriteBufferFromFileBase
    {
    public:
        ConditionalWriteBuffer(FakeGenerationObjectStorage & storage_, std::string key_,
                                std::string if_none_match_, std::string if_match_, std::shared_ptr<WriteRecord> record_)
            : DB::WriteBufferFromFileBase(/*buf_size=*/8192, nullptr, 0)
            , storage(storage_), key(std::move(key_))
            , if_none_match(std::move(if_none_match_)), if_match(std::move(if_match_))
            , record(std::move(record_))
        {
        }

        void sync() override {}
        std::string getFileName() const override { return key; }

        /// The write response's own incarnation, quoted the way the SDK's ETag field carries a GCS
        /// generation across the HTTP boundary -- the backend is what strips that transport syntax.
        std::optional<std::string> getResultObjectETag() const override { return committed_generation; }

    protected:
        void nextImpl() override
        {
            if (!offset())
                return;
            buffered.append(working_buffer.begin(), offset());
        }

        void finalizeImpl() override
        {
            next();
            const auto & throttler = record->settings.remote_throttler;
            if (throttler && storage.throttle_while_writing && storage.throttle_while_writing(key))
                throttleOnce(*throttler);
            committed_generation = storage.commitConditionalWrite(key, buffered, if_none_match, if_match);
        }

        void throttleOnce(DB::IThrottler & throttler)
        {
            if (storage.before_throttle)
                storage.before_throttle(key);
            record->throttled = true;
            try
            {
                throttler.throttle(32768, 0);
            }
            catch (...)
            {
                record->throttle_error = std::current_exception();
                throw;
            }
        }

    private:
        FakeGenerationObjectStorage & storage;
        std::string key;
        std::string if_none_match;
        std::string if_match;
        std::string buffered;
        std::optional<std::string> committed_generation;
        std::shared_ptr<WriteRecord> record;
    };

    mutable std::mutex mutex;
    std::map<std::string, Entry> objects;
    std::map<std::string, size_t> copies_to;
    std::vector<std::shared_ptr<WriteRecord>> recorded;
    uint64_t next_generation = 1;
};

inline std::shared_ptr<FakeGenerationObjectStorage> makeFakeGenerationObjectStorageForTest()
{
    static std::atomic<uint64_t> counter{0};
    const auto unique = std::to_string(::getpid()) + "_" + std::to_string(counter.fetch_add(1));
    const auto root = (std::filesystem::temp_directory_path() / ("cas_s3_staging_generation_" + unique)).string();

    std::error_code ec;
    std::filesystem::remove_all(root, ec);
    std::filesystem::create_directories(root, ec);

    DB::LocalObjectStorageSettings settings("test", root, /*read_only_=*/false);
    return ownedByDirectory(std::make_shared<FakeGenerationObjectStorage>(std::move(settings)), root);
}

}

#endif
