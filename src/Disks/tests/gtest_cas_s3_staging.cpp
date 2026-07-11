#include <gtest/gtest.h>
#include "cas_test_helpers.h"
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/ContentAddressedMetadataStorage.h>
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
