#include <gtest/gtest.h>
#include "cas_test_helpers.h"
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/ContentAddressedMetadataStorage.h>

#include <Poco/AutoPtr.h>
#include <Poco/Util/XMLConfiguration.h>

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
