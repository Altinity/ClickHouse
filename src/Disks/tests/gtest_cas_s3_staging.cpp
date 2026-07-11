#include <gtest/gtest.h>
#include "cas_test_helpers.h"
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/ContentAddressedMetadataStorage.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/ContentAddressedWriteBuffers.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasProbe.h>
#include <Disks/DiskObjectStorage/ObjectStorages/Local/LocalObjectStorage.h>

#include <Poco/AutoPtr.h>
#include <Poco/Util/XMLConfiguration.h>

#include <atomic>
#include <filesystem>
#include <sstream>
#include <string>

/// Task 0 of the S3-native staging plan (docs/superpowers/plans/2026-07-11-cas-s3-native-staging.md):
/// pure config plumbing, ZERO behavior change. `cas_staging_backend` (default `local`) and
/// `cas_s3_staging_min_bytes` (default 64 MiB) are parsed from the CAS disk config; the parsed
/// `StagingBackend` is exposed via `ContentAddressedMetadataStorage::stagingBackend()` /
/// `::s3StagingMinBytes()`. `::conditionalCopySupported()` is a stored bool, defaulting to `false`
/// until a later task wires the mount-time capability probe.
///
/// The global constraint (OFF BY DEFAULT) is the DEFAULT arm below: absent config keys must parse to
/// `StagingBackend::Local` with the default min-bytes threshold and `conditionalCopySupported()==false`.

namespace
{

/// Build a `Poco::Util::XMLConfiguration` with `inner_xml` nested under a `<disk>` element (mirrors
/// the shape a real CAS disk config has under `storage_configuration.disks.<name>`, so
/// `config_prefix = "disk"` reads exactly like the disk factory's `config_prefix`).
Poco::AutoPtr<Poco::Util::XMLConfiguration> configWithDiskSection(const std::string & inner_xml)
{
    std::istringstream xml_stream( // STYLE_CHECK_ALLOW_STD_STRING_STREAM
        "<clickhouse><disk>" + inner_xml + "</disk></clickhouse>");
    return new Poco::Util::XMLConfiguration(xml_stream);
}

/// A test-only `LocalObjectStorage` subclass whose `copyObjectConditional` is configurable, so the
/// Task 3 selection logic (`DB::Cas::probeConditionalCopy`) can be exercised without a live S3/RustFS
/// backend (live enforcement is Task 7). `LocalObjectStorage` already implements every OTHER pure
/// virtual (`writeObject`, `removeObjectIfExists`, `exists`, `copyObject`, ...) against real files
/// under a fresh temp root, so overriding just `copyObjectConditional` is enough to fake either an
/// ENFORCING or a NON-ENFORCING backend; a THROWING (default `NOT_IMPLEMENTED`) backend needs no
/// fake at all — a plain `LocalObjectStorage` already exercises that path (see
/// `DefaultCopyObjectConditionalThrowsNotImplemented` above).
class FakeConditionalCopyObjectStorage : public DB::LocalObjectStorage
{
public:
    enum class Mode
    {
        /// Real write-once semantics: creates the destination iff it was absent; a destination that
        /// already exists is REJECTED (created=false), no bytes touched.
        Enforcing,
        /// A backend that silently ignores `If-None-Match`: every call overwrites the destination
        /// and reports created=true, even when the destination already existed.
        NonEnforcing,
    };

    FakeConditionalCopyObjectStorage(DB::LocalObjectStorageSettings settings_, Mode mode_)
        : DB::LocalObjectStorage(std::move(settings_)), mode(mode_)
    {
    }

    DB::ConditionalCopyResult copyObjectConditional(
        const DB::StoredObject & object_from,
        const DB::StoredObject & object_to,
        const DB::ReadSettings & read_settings,
        const DB::WriteSettings & write_settings,
        std::optional<DB::ObjectAttributes> object_to_attributes = {}) override
    {
        ++call_count;
        if (mode == Mode::Enforcing && exists(object_to))
            return {.created = false, .dest_etag = {}};

        copyObject(object_from, object_to, read_settings, write_settings, object_to_attributes);
        return {.created = true, .dest_etag = "fake-etag"};
    }

    int callCount() const { return call_count; }

private:
    Mode mode;
    int call_count = 0;
};

/// Build a `FakeConditionalCopyObjectStorage` rooted at a fresh, unique temp directory (mirrors
/// `DB::Cas::tests::makeLocalObjectStorageForTest`).
std::shared_ptr<FakeConditionalCopyObjectStorage> makeFakeConditionalCopyStorage(FakeConditionalCopyObjectStorage::Mode mode)
{
    static std::atomic<uint64_t> counter{0};
    const auto unique = std::to_string(::getpid()) + "_" + std::to_string(counter.fetch_add(1));
    const auto root = (std::filesystem::temp_directory_path() / ("cas_s3_staging_probe_" + unique)).string();

    std::error_code ec;
    std::filesystem::remove_all(root, ec);
    std::filesystem::create_directories(root, ec);

    DB::LocalObjectStorageSettings settings("test", root, /*read_only_=*/false);
    return std::make_shared<FakeConditionalCopyObjectStorage>(std::move(settings), mode);
}

/// A fake object-store sink for Task 4 of the S3-native staging plan (`DB::ContentAddressed::CaContentWriteBuffer`'s
/// S3-staging constructor): an in-memory `WriteBufferFromFileBase` that records every byte written to
/// it, plus whether `cancelImpl`/`finalizeImpl` ran. This is enough to prove the S3-staging mode
/// streams to the SINK (not to a local temp file) while hashing, without needing a real object storage
/// — the end-to-end wiring (`writeFile` choosing this mode, the promote path) lands in later tasks.
class FakeStagingSink : public DB::WriteBufferFromFileBase
{
public:
    explicit FakeStagingSink(std::string key_)
        : DB::WriteBufferFromFileBase(/*buf_size=*/8192, nullptr, 0), key(std::move(key_))
    {
    }

    void sync() override {}
    std::string getFileName() const override { return key; }

    const std::string & writtenBytes() const { return written; }
    bool wasCancelled() const { return cancelled; }
    bool wasFinalizedForTest() const { return did_finalize; }

protected:
    void nextImpl() override
    {
        if (!offset())
            return;
        written.append(working_buffer.begin(), offset());
    }

    void finalizeImpl() override
    {
        next();
        did_finalize = true;
    }

    void cancelImpl() noexcept override
    {
        cancelled = true;
    }

private:
    std::string key;
    std::string written;
    bool cancelled = false;
    bool did_finalize = false;
};

}

TEST(CasS3Staging, ParsesS3BackendAndMinBytesFromConfig)
{
    auto config = configWithDiskSection(
        "<cas_staging_backend>s3</cas_staging_backend>"
        "<cas_s3_staging_min_bytes>67108864</cas_s3_staging_min_bytes>");

    EXPECT_EQ(DB::ContentAddressedMetadataStorage::parseStagingBackend(*config, "disk"), DB::StagingBackend::S3);
    EXPECT_EQ(DB::ContentAddressedMetadataStorage::parseS3StagingMinBytes(*config, "disk"), 67108864ULL);
}

TEST(CasS3Staging, DefaultConfigParsesToLocalBackendAndDefaultMinBytes)
{
    /// No `cas_staging_backend` / `cas_s3_staging_min_bytes` keys at all — the OFF BY DEFAULT arm.
    auto config = configWithDiskSection("<scratch_path>/tmp/whatever</scratch_path>");

    EXPECT_EQ(DB::ContentAddressedMetadataStorage::parseStagingBackend(*config, "disk"), DB::StagingBackend::Local);
    EXPECT_EQ(DB::ContentAddressedMetadataStorage::parseS3StagingMinBytes(*config, "disk"), 64ULL << 20);
}

TEST(CasS3Staging, UnknownBackendValueThrows)
{
    auto config = configWithDiskSection("<cas_staging_backend>nfs</cas_staging_backend>");
    EXPECT_THROW(DB::ContentAddressedMetadataStorage::parseStagingBackend(*config, "disk"), DB::Exception);
}

TEST(CasS3Staging, DefaultConstructedStorageReportsLocalAndNoConditionalCopy)
{
    /// Constructed with no staging-related args at all (mirrors the existing gtest call sites, e.g.
    /// gtest_ca_wiring.cpp, which stop at `context_`): the accessors must reflect the same
    /// byte-for-byte-current-behavior defaults the config parser produces above.
    auto storage = std::make_shared<DB::ContentAddressedMetadataStorage>(
        DB::Cas::tests::makeLocalObjectStorageForTest(), "pool", "srv1", "test",
        std::filesystem::temp_directory_path() / "cas_s3_staging_default_scratch", nullptr);

    EXPECT_EQ(storage->stagingBackend(), DB::StagingBackend::Local);
    EXPECT_EQ(storage->s3StagingMinBytes(), 64ULL << 20);
    EXPECT_FALSE(storage->conditionalCopySupported());
}

/// Task 2 of the S3-native staging plan: `IObjectStorage::copyObjectConditional` (write-once
/// conditional server-side copy) — the interface-level contract. Backends without an enforced,
/// native conditional copy MUST NOT override the default: it fail-closes with `NOT_IMPLEMENTED`,
/// exactly like the existing `IObjectStorage::removeObjectIfTokenMatches` default (never silently
/// falls back to an unconditional overwrite). `LocalObjectStorage` (used by
/// `makeLocalObjectStorageForTest`) does not override `copyObjectConditional`, so it exercises the
/// base-class default directly. Live 412-vs-created S3 semantics are covered by the Task 7
/// integration test (with_rustfs); this is deliberately just the fail-closed contract test.
TEST(CasS3Staging, DefaultCopyObjectConditionalThrowsNotImplemented)
{
    auto storage = DB::Cas::tests::makeLocalObjectStorageForTest();

    const DB::StoredObject from{"cas_s3_staging_conditional_copy_from"};
    const DB::StoredObject to{"cas_s3_staging_conditional_copy_to"};

    DB::Cas::tests::expectThrowsCode(DB::ErrorCodes::NOT_IMPLEMENTED, [&]
    {
        storage->copyObjectConditional(from, to, DB::ReadSettings{}, DB::WriteSettings{});
    });
}

/// Task 3 of the S3-native staging plan: the mount-time capability probe (`DB::Cas::probeConditionalCopy`)
/// for the OPTIONAL conditional-copy capability (distinct from the mandatory `runCapabilityProbe`
/// battery). These three tests cover the fail-close SELECTION logic with fakes — live 412-vs-created
/// enforcement against a real backend is Task 7 (with_rustfs integration test).

TEST(CasS3Staging, ProbeConditionalCopyReturnsTrueForEnforcingBackend)
{
    auto storage = makeFakeConditionalCopyStorage(FakeConditionalCopyObjectStorage::Mode::Enforcing);

    EXPECT_TRUE(DB::Cas::probeConditionalCopy(*storage, "probe_prefix"));
    /// Both the "fresh destination" and the "already-existing destination" conditional copies ran.
    EXPECT_EQ(storage->callCount(), 2);
}

TEST(CasS3Staging, ProbeConditionalCopyReturnsFalseForNonEnforcingBackend)
{
    auto storage = makeFakeConditionalCopyStorage(FakeConditionalCopyObjectStorage::Mode::NonEnforcing);

    /// The backend silently overwrites the destination on the second call (created=true again) —
    /// it does not enforce If-None-Match, so the probe must fail closed.
    EXPECT_FALSE(DB::Cas::probeConditionalCopy(*storage, "probe_prefix"));
}

TEST(CasS3Staging, ProbeConditionalCopyReturnsFalseWhenCopyObjectConditionalThrows)
{
    /// A plain `LocalObjectStorage` does not override `copyObjectConditional` at all — it falls
    /// through to the base-class default, which throws NOT_IMPLEMENTED (exactly what a real backend
    /// without conditional-copy support does). The probe must never propagate this: it fails closed.
    auto storage = DB::Cas::tests::makeLocalObjectStorageForTest();

    EXPECT_FALSE(DB::Cas::probeConditionalCopy(*storage, "probe_prefix"));
}

/// Task 4 of the S3-native staging plan: `CaContentWriteBuffer`'s S3-staging constructor streams
/// directly to an already-opened object-store sink while hashing, instead of spilling to a local temp
/// file (see the constructor's doc comment in ContentAddressedWriteBuffers.h). These two tests
/// exercise the buffer directly over a `FakeStagingSink` — no real object storage, disk, or
/// `ContentAddressedTransaction` needed; `writeFile` choosing this mode is exercised together with the
/// promote path in later tasks (S3 mode is off by default and not enabled by any existing test).

TEST(CasS3Staging, ContentWriteBufferS3ModeStreamsToSinkAndFinalizes)
{
    const std::string staging_key = "staging/mount1/abc123.tmp";
    auto * sink_ptr = new FakeStagingSink(staging_key);
    std::unique_ptr<DB::WriteBufferFromFileBase> sink(sink_ptr);

    std::string got_hash_hex;
    size_t got_size = 0;
    std::string got_key;
    int on_finalized_calls = 0;

    auto buf = std::make_unique<DB::ContentAddressed::CaContentWriteBuffer>(
        std::move(sink),
        staging_key,
        /*buf_size=*/8192,
        /*use_adaptive_buffer_size=*/false,
        /*adaptive_buffer_initial_size=*/0,
        [&](const std::string & hash_hex, size_t size, const std::string & key)
        {
            ++on_finalized_calls;
            got_hash_hex = hash_hex;
            got_size = size;
            got_key = key;
        });

    /// Write in two chunks (exercises more than one nextImpl flush) and finalize.
    const std::string payload_part1(4000, 'x');
    const std::string payload_part2(1234, 'y');
    buf->write(payload_part1.data(), payload_part1.size());
    buf->write(payload_part2.data(), payload_part2.size());
    buf->finalize();

    const std::string payload = payload_part1 + payload_part2;

    /// (a) the sink received EXACTLY the bytes written — no local temp file was ever touched.
    EXPECT_EQ(sink_ptr->writtenBytes(), payload);
    EXPECT_TRUE(sink_ptr->wasFinalizedForTest());
    EXPECT_FALSE(sink_ptr->wasCancelled());

    /// (b) on_finalized fired exactly once with the correct cityHash128 hex, size, and staging key.
    /// The pool-wide content hash is the STREAMING `HashingWriteBuffer` convention (chunked
    /// cityHash128, block = 2048 B), which diverges from a one-shot `CityHash_v1_0_2::CityHash128`
    /// call for a payload spanning more than one block (see `gtest_cas_build.cpp`'s
    /// `CopyForwardMultiBlockPayloadVerifies`, which documents and exercises the same divergence).
    /// This payload (5234 bytes) spans multiple 2048-byte blocks, so the expected hash must be
    /// recomputed with the SAME streaming convention via `HashingReadBuffer`, not a one-shot call.
    DB::ReadBufferFromMemory expected_in(payload.data(), payload.size());
    DB::HashingReadBuffer expected_hashing(expected_in);
    expected_hashing.ignoreAll();
    const std::string expected_hash_hex = getHexUIntLowercase(expected_hashing.getHash());
    EXPECT_EQ(on_finalized_calls, 1);
    EXPECT_EQ(got_hash_hex, expected_hash_hex);
    EXPECT_EQ(got_size, payload.size());
    EXPECT_EQ(got_key, staging_key);
    EXPECT_EQ(buf->getFileName(), staging_key);
}

TEST(CasS3Staging, ContentWriteBufferS3ModeCancelCancelsSinkAndSkipsFinalize)
{
    const std::string staging_key = "staging/mount1/cancelled.tmp";
    auto * sink_ptr = new FakeStagingSink(staging_key);
    std::unique_ptr<DB::WriteBufferFromFileBase> sink(sink_ptr);

    bool on_finalized_called = false;

    auto buf = std::make_unique<DB::ContentAddressed::CaContentWriteBuffer>(
        std::move(sink),
        staging_key,
        /*buf_size=*/8192,
        /*use_adaptive_buffer_size=*/false,
        /*adaptive_buffer_initial_size=*/0,
        [&](const std::string &, size_t, const std::string &)
        {
            on_finalized_called = true;
        });

    const std::string payload = "some bytes that must never be promoted";
    buf->write(payload.data(), payload.size());
    buf->cancel();

    /// (c) cancel() before finalize cancels the sink and on_finalized is NEVER called — no partial
    /// finalize (no promote-worthy hash/size is ever handed to the transaction for cancelled bytes).
    EXPECT_TRUE(sink_ptr->wasCancelled());
    EXPECT_FALSE(sink_ptr->wasFinalizedForTest());
    EXPECT_FALSE(on_finalized_called);

    /// The buffer's destructor calls cancel() again (defensive backstop) — already-cancelled, so this
    /// must stay a no-op: still no on_finalized call, and no attempt to fs::remove a remote key.
    buf.reset();
    EXPECT_FALSE(on_finalized_called);
}
