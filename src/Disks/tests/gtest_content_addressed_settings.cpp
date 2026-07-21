#include <gtest/gtest.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/ContentAddressedSettings.h>
#include <Common/Exception.h>
#include <Poco/Util/XMLConfiguration.h>
#include <Poco/AutoPtr.h>
#include <sstream>

using namespace DB;

namespace DB::ErrorCodes
{
    extern const int NO_ELEMENTS_IN_CONFIG;
    extern const int BAD_ARGUMENTS;
}

/// Per-TU extern declarations for the `ContentAddressedSetting` entries this file uses -- the
/// established pattern for `BaseSettings`-derived classes in this codebase (see e.g.
/// `RegisterDiskCache.cpp`'s `namespace FileCacheSetting` block): the entries are DEFINED once in
/// `ContentAddressedSettings.cpp`, and each consumer TU declares only the ones it references.
namespace DB::ContentAddressedSetting
{
    extern const ContentAddressedSettingsUInt64 gc_shards;
    extern const ContentAddressedSettingsUInt64 gc_interval_sec;
    extern const ContentAddressedSettingsUInt64 dedup_cache_bytes;
    extern const ContentAddressedSettingsString scratch_path;
}

namespace
{
Poco::AutoPtr<Poco::Util::XMLConfiguration> makeConfig(const std::string & inner)
{
    std::istringstream iss("<clickhouse><disk>" + inner + "</disk></clickhouse>");
    return new Poco::Util::XMLConfiguration(iss);
}
const auto identity_macros = [](const std::string & s) { return s; };
}

TEST(ContentAddressedSettings, DefaultsAndOverridesLand)
{
    auto cfg = makeConfig("<server_root_id>srv1</server_root_id><gc_shards>4</gc_shards>");
    ContentAddressedSettings s;
    s.loadFromConfig(*cfg, "disk", "/data/default_scratch", identity_macros);
    EXPECT_EQ(s[ContentAddressedSetting::gc_shards].value, 4u);
    EXPECT_EQ(s[ContentAddressedSetting::gc_interval_sec].value, 60u);          /// table default
    EXPECT_EQ(s[ContentAddressedSetting::dedup_cache_bytes].value, 64ULL << 20); /// table default
    EXPECT_EQ(s[ContentAddressedSetting::scratch_path].value, "/data/default_scratch");
}

TEST(ContentAddressedSettings, UnknownKeyRejected)
{
    auto cfg = makeConfig("<server_root_id>srv1</server_root_id><gc_shardz>4</gc_shardz>");
    ContentAddressedSettings s;
    EXPECT_THROW(s.loadFromConfig(*cfg, "disk", "/scratch", identity_macros), Exception);
}

TEST(ContentAddressedSettings, ObjectStorageKeysSkipped)
{
    auto cfg = makeConfig(
        "<metadata_type>content_addressed</metadata_type><type>object_storage</type>"
        "<object_storage_type>s3</object_storage_type><endpoint>http://x/y</endpoint>"
        "<access_key_id>k</access_key_id><secret_access_key>s</secret_access_key>"
        "<server_root_id>srv1</server_root_id>");
    ContentAddressedSettings s;
    EXPECT_NO_THROW(s.loadFromConfig(*cfg, "disk", "/scratch", identity_macros));
}

TEST(ContentAddressedSettings, ValidateFailsClosed)
{
    {   /// missing server_root_id: ABSENT key -> typed NO_ELEMENTS_IN_CONFIG (distinct from a
        /// present-but-invalid value, checked below), mirroring the pre-F4b factory behavior.
        auto cfg = makeConfig("<gc_shards>1</gc_shards>");
        ContentAddressedSettings s;
        try
        {
            s.loadFromConfig(*cfg, "disk", "/scratch", identity_macros);
            FAIL() << "expected an exception";
        }
        catch (const Exception & e)
        {
            EXPECT_EQ(e.code(), ErrorCodes::NO_ELEMENTS_IN_CONFIG);
        }
    }
    {   /// present but invalid (empty) server_root_id -> BAD_ARGUMENTS from
        /// `Cas::validateServerRootId`, not NO_ELEMENTS_IN_CONFIG.
        auto cfg = makeConfig("<server_root_id></server_root_id>");
        ContentAddressedSettings s;
        try
        {
            s.loadFromConfig(*cfg, "disk", "/scratch", identity_macros);
            FAIL() << "expected an exception";
        }
        catch (const Exception & e)
        {
            EXPECT_EQ(e.code(), ErrorCodes::BAD_ARGUMENTS);
        }
    }
    {   /// zero gc_shards
        auto cfg = makeConfig("<server_root_id>srv1</server_root_id><gc_shards>0</gc_shards>");
        ContentAddressedSettings s;
        EXPECT_THROW(s.loadFromConfig(*cfg, "disk", "/scratch", identity_macros), Exception);
    }
    {   /// unknown blob_hash spelling
        auto cfg = makeConfig("<server_root_id>srv1</server_root_id><blob_hash>md5</blob_hash>");
        ContentAddressedSettings s;
        EXPECT_THROW(s.loadFromConfig(*cfg, "disk", "/scratch", identity_macros), Exception);
    }
}

TEST(ContentAddressedSettings, RelativeScratchPathAnchored)
{
    auto cfg = makeConfig("<server_root_id>srv1</server_root_id><scratch_path>rel/dir</scratch_path>");
    ContentAddressedSettings s;
    s.loadFromConfig(*cfg, "disk", "/data/default_scratch", identity_macros);
    /// Relative override anchored to the server data path prefix passed by the caller, never CWD.
    EXPECT_TRUE(s[ContentAddressedSetting::scratch_path].value.starts_with("/"));
}
