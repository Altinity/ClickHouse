#include <gtest/gtest.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Backend/CasRequestBudget.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/ContentAddressedMetadataStorage.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/ContentAddressedSettings.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasPool.h>
#include <Common/Exception.h>
#include "cas_test_helpers.h"
#include <Common/logger_useful.h>
#include <Poco/AutoPtr.h>
#include <Poco/StreamChannel.h>
#include <Poco/Util/XMLConfiguration.h>
#include <filesystem>
#include <limits>
#include <sstream>
#include <unistd.h>

using namespace DB;

namespace DB::ErrorCodes
{
    extern const int NO_ELEMENTS_IN_CONFIG;
    extern const int BAD_ARGUMENTS;
    extern const int UNKNOWN_SETTING;
}

/// Per-TU extern declarations for the `ContentAddressedSetting` entries this file uses -- the
/// established pattern for `BaseSettings`-derived classes in this codebase (see e.g.
/// `RegisterDiskCache.cpp`'s `namespace FileCacheSetting` block): the entries are DEFINED once in
/// `ContentAddressedSettings.cpp`, and each consumer TU declares only the ones it references.
namespace DB::ContentAddressedSetting
{
    extern const ContentAddressedSettingsBool gc_enabled;
    extern const ContentAddressedSettingsUInt64 gc_shards;
    extern const ContentAddressedSettingsUInt64 gc_interval_sec;
    extern const ContentAddressedSettingsUInt64 gc_bulk_delete_chunk_keys;
    extern const ContentAddressedSettingsUInt64 gc_round_graduation_budget;
    extern const ContentAddressedSettingsUInt64 gc_meta_pool_size;
    extern const ContentAddressedSettingsUInt64 gc_round_intent_create_budget;
    extern const ContentAddressedSettingsUInt64 gc_round_redelete_budget;
    extern const ContentAddressedSettingsUInt64 gc_round_ref_cleanup_budget;
    extern const ContentAddressedSettingsUInt64 gc_round_handoff_prefix_wholesale_budget;
    extern const ContentAddressedSettingsUInt64 gc_round_outcome_entry_budget;
    extern const ContentAddressedSettingsUInt64 gc_round_sweep_namespace_budget;
    extern const ContentAddressedSettingsUInt64 gc_round_sweep_recovery_op_budget;
    extern const ContentAddressedSettingsUInt64 gc_round_prefix_wholesale_budget;
    extern const ContentAddressedSettingsUInt64 intent_sweep_list_budget_keys;
    extern const ContentAddressedSettingsUInt64 mount_lease_ttl_ms;
    extern const ContentAddressedSettingsUInt64 mount_renew_period_ms;
    extern const ContentAddressedSettingsUInt64 request_window_ms;
    extern const ContentAddressedSettingsString scratch_path;
    extern const ContentAddressedSettingsBool unsafe_remount_no_delay;
    extern const ContentAddressedSettingsBool allow_packed_parts;
}

namespace
{
Poco::AutoPtr<Poco::Util::XMLConfiguration> makeConfig(const std::string & inner)
{
    std::istringstream iss("<clickhouse><disk>" + inner + "</disk></clickhouse>");
    return new Poco::Util::XMLConfiguration(iss);
}

const auto identity_macros = [](const std::string & s) { return s; };

/// Every capture of this file reads the settings logger at `warning`.
struct ScopedCasSettingsLogCapture : Cas::tests::ScopedLogCapture
{
    ScopedCasSettingsLogCapture() : ScopedLogCapture("ContentAddressedSettings", "warning") {}
};

size_t countOccurrences(const String & haystack, const String & needle)
{
    size_t n = 0;
    for (size_t at = haystack.find(needle); at != String::npos; at = haystack.find(needle, at + 1))
        ++n;
    return n;
}

void expectLoadFailureWithExactMessage(const String & config, int code, const String & message)
{
    auto cfg = makeConfig(config);
    ContentAddressedSettings settings;
    try
    {
        settings.loadFromConfig(*cfg, "disk", "/scratch", "/scratch", identity_macros);
        FAIL() << "expected settings load to fail";
    }
    catch (const Exception & e)
    {
        EXPECT_EQ(e.code(), code);
        EXPECT_EQ(e.message(), message);
    }
}
}

TEST(CASContentAddressedSettings, DefaultsAndOverridesLand)
{
    auto cfg = makeConfig("<cas_server_root_id>srv1</cas_server_root_id><cas_gc_shards>4</cas_gc_shards>");
    ContentAddressedSettings s;
    s.loadFromConfig(*cfg, "disk", "/data", "/data/default_scratch", identity_macros);
    EXPECT_EQ(s[ContentAddressedSetting::gc_shards].value, 4u);
    EXPECT_EQ(s[ContentAddressedSetting::gc_interval_sec].value, 60u);          /// table default
    /// Absent key -> the verbatim default (never touches the anchor).
    EXPECT_EQ(s[ContentAddressedSetting::scratch_path].value, "/data/default_scratch");
}

TEST(CASContentAddressedSettings, GcRoundBudgetsDefaultToUnbounded)
{
    auto cfg = makeConfig("<cas_server_root_id>srv1</cas_server_root_id>");
    ContentAddressedSettings s;
    s.loadFromConfig(*cfg, "disk", "/data", "/data/default_scratch", identity_macros);
    EXPECT_EQ(s[ContentAddressedSetting::gc_round_graduation_budget].value, 0u);
    EXPECT_EQ(s[ContentAddressedSetting::gc_round_redelete_budget].value, 0u);
    EXPECT_EQ(s[ContentAddressedSetting::gc_round_ref_cleanup_budget].value, 0u);
    EXPECT_EQ(s[ContentAddressedSetting::gc_round_handoff_prefix_wholesale_budget].value, 0u);
    EXPECT_EQ(s[ContentAddressedSetting::gc_round_outcome_entry_budget].value, 0u);

    const Cas::PoolConfig pool_config;
    EXPECT_EQ(pool_config.gc_round_intent_create_budget, 0u);
    EXPECT_EQ(pool_config.gc_round_redelete_budget, 0u);
    EXPECT_EQ(pool_config.gc_round_ref_cleanup_budget, 0u);
    EXPECT_EQ(pool_config.gc_round_handoff_prefix_wholesale_budget, 0u);
    EXPECT_EQ(pool_config.gc_round_outcome_entry_budget, 0u);

    EXPECT_EQ(s[ContentAddressedSetting::gc_round_sweep_namespace_budget].value, 20u);
    EXPECT_EQ(s[ContentAddressedSetting::gc_round_sweep_recovery_op_budget].value, 5000u);
    EXPECT_EQ(s[ContentAddressedSetting::gc_round_prefix_wholesale_budget].value, 20000u);
}

TEST(CASContentAddressedSettings, IntentCreateBudgetDefaultsToUnbounded)
{
    auto cfg = makeConfig("<cas_server_root_id>srv1</cas_server_root_id>");
    ContentAddressedSettings s;
    s.loadFromConfig(*cfg, "disk", "/data", "/data/default_scratch", identity_macros);
    EXPECT_EQ(s[ContentAddressedSetting::gc_round_intent_create_budget].value, 0u);
    EXPECT_EQ(Cas::PoolConfig{}.gc_round_intent_create_budget, 0u);
}

TEST(CASContentAddressedSettings, IntentCreateBudgetReachesThePool)
{
    const auto scratch = std::filesystem::temp_directory_path() / ("ca_intent_budget_scratch_" + std::to_string(::getpid()));
    auto cfg = makeConfig("<cas_server_root_id>srv1</cas_server_root_id><cas_gc_round_intent_create_budget>7</cas_gc_round_intent_create_budget>");
    ContentAddressedSettings settings;
    settings.loadFromConfig(*cfg, "disk", scratch.string(), scratch.string(), identity_macros);
    ASSERT_EQ(settings[ContentAddressedSetting::gc_round_intent_create_budget].value, 7u) << "the external key loads";

    auto storage = std::make_shared<ContentAddressedMetadataStorage>(
        Cas::tests::makeLocalObjectStorageForTest(), "pool", "srv1", "", nullptr, settings);
    storage->startup();
    const auto pool = storage->poolForTest();
    ASSERT_NE(pool, nullptr);
    EXPECT_EQ(pool->poolConfig().gc_round_intent_create_budget, 7u);
}

namespace
{

/// The pool config a disk configured with `extra` opens with.
Cas::PoolConfig poolConfigOf(const String & extra, const String & scratch_name)
{
    const auto scratch = std::filesystem::temp_directory_path() / (scratch_name + "_" + std::to_string(::getpid()));
    auto cfg = makeConfig("<cas_server_root_id>srv1</cas_server_root_id>" + extra);
    ContentAddressedSettings settings;
    settings.loadFromConfig(*cfg, "disk", scratch.string(), scratch.string(), identity_macros);
    auto storage = std::make_shared<ContentAddressedMetadataStorage>(
        Cas::tests::makeLocalObjectStorageForTest(), "pool", "srv1", "", nullptr, settings);
    storage->startup();
    const auto pool = storage->poolForTest();
    if (!pool)
        throw std::runtime_error("the disk opened no pool");
    return pool->poolConfig();
}

}

TEST(CASContentAddressedSettings, RetiredGcSettingsStillLoad)
{
    const Cas::PoolConfig defaults;
    const Cas::PoolConfig configured = poolConfigOf("<cas_gc_meta_pool_size>9</cas_gc_meta_pool_size>", "ca_retired_settings_scratch");
    EXPECT_EQ(configured.gc_round_intent_create_budget, defaults.gc_round_intent_create_budget);
    EXPECT_EQ(configured.gc_round_redelete_budget, defaults.gc_round_redelete_budget);
    EXPECT_EQ(configured.gc_io_concurrency, defaults.gc_io_concurrency);
}

TEST(CASContentAddressedSettings, RetiredCreateBudgetAppliesWhenTheNewOneIsNotSet)
{
    EXPECT_EQ(poolConfigOf("<cas_gc_round_graduation_budget>11</cas_gc_round_graduation_budget>", "ca_retired_budget_scratch")
                  .gc_round_intent_create_budget, 11u);
    EXPECT_EQ(poolConfigOf("<cas_gc_round_graduation_budget>11</cas_gc_round_graduation_budget>"
                           "<cas_gc_round_intent_create_budget>5</cas_gc_round_intent_create_budget>", "ca_both_budgets_scratch")
                  .gc_round_intent_create_budget, 5u) << "the new setting wins";
}

TEST(CASContentAddressedSettings, IntentSweepPageReachesThePool)
{
    const auto scratch = std::filesystem::temp_directory_path() / ("ca_intent_sweep_scratch_" + std::to_string(::getpid()));
    auto cfg = makeConfig("<cas_server_root_id>srv1</cas_server_root_id><cas_intent_sweep_list_budget_keys>7</cas_intent_sweep_list_budget_keys>");
    ContentAddressedSettings settings;
    settings.loadFromConfig(*cfg, "disk", scratch.string(), scratch.string(), identity_macros);
    ASSERT_EQ(settings[ContentAddressedSetting::intent_sweep_list_budget_keys].value, 7u) << "the external key loads";
    EXPECT_NE(Cas::PoolConfig{}.intent_sweep_list_budget_keys, 7u) << "the default would hide a lost value";

    auto storage = std::make_shared<ContentAddressedMetadataStorage>(
        Cas::tests::makeLocalObjectStorageForTest(), "pool", "srv1", "", nullptr, settings);
    storage->startup();
    const auto pool = storage->poolForTest();
    ASSERT_NE(pool, nullptr);
    EXPECT_EQ(pool->poolConfig().intent_sweep_list_budget_keys, 7u);
}

TEST(CASContentAddressedSettings, ZeroIntentSweepPageIsRefused)
{
    expectLoadFailureWithExactMessage(
        "<cas_server_root_id>srv1</cas_server_root_id><cas_intent_sweep_list_budget_keys>0</cas_intent_sweep_list_budget_keys>",
        ErrorCodes::BAD_ARGUMENTS,
        "content_addressed disk: cas_intent_sweep_list_budget_keys must be >= 1 (got 0)");
}

TEST(CASContentAddressedSettings, PackedPartsDefaultOffAndOptIn)
{
    auto default_cfg = makeConfig("<cas_server_root_id>srv1</cas_server_root_id>");
    ContentAddressedSettings defaults;
    defaults.loadFromConfig(*default_cfg, "disk", "/data", "/data/default_scratch", identity_macros);
    EXPECT_FALSE(defaults[ContentAddressedSetting::allow_packed_parts].value);

    auto enabled_cfg = makeConfig(
        "<cas_server_root_id>srv1</cas_server_root_id>"
        "<cas_allow_packed_parts>true</cas_allow_packed_parts>");
    ContentAddressedSettings enabled;
    enabled.loadFromConfig(*enabled_cfg, "disk", "/data", "/data/default_scratch", identity_macros);
    EXPECT_TRUE(enabled[ContentAddressedSetting::allow_packed_parts].value);
}

TEST(CASContentAddressedSettings, RemovedCacheSettingsAreRejected)
{
    for (const std::string & suffix : {"cache_bytes", "head_first_min_bytes"})
    {
        const std::string setting = "cas_deduplication_" + suffix;
        SCOPED_TRACE(setting);
        auto cfg = makeConfig(
            "<cas_server_root_id>srv1</cas_server_root_id><" + setting + ">4096</" + setting + ">");
        ContentAddressedSettings settings;
        try
        {
            settings.loadFromConfig(*cfg, "disk", "/data", "/data/scratch", identity_macros);
            FAIL() << "expected removed setting " << setting << " to be rejected as unknown";
        }
        catch (const Exception & e)
        {
            EXPECT_EQ(e.code(), ErrorCodes::UNKNOWN_SETTING);
        }
    }
}

TEST(CASContentAddressedSettings, RemovedGcReadConcurrencyIsRejected)
{
    auto cfg = makeConfig(
        "<cas_server_root_id>srv1</cas_server_root_id>"
        "<cas_gc_read_concurrency>1</cas_gc_read_concurrency>");
    ContentAddressedSettings settings;
    try
    {
        settings.loadFromConfig(*cfg, "disk", "/data", "/data/scratch", identity_macros);
        FAIL() << "expected the removed setting cas_gc_read_concurrency to be rejected as unknown";
    }
    catch (const Exception & e)
    {
        EXPECT_EQ(e.code(), ErrorCodes::UNKNOWN_SETTING);
    }
}

/// `cas_part_folder_validate` paced a manifest `HEAD` that no longer exists. A config still asking
/// for it must fail the disk open, not be quietly accepted and ignored.
TEST(CASContentAddressedSettings, RetiredPartFolderValidateIsRejected)
{
    for (const std::string & value : {"always", "never", "age 5"})
    {
        SCOPED_TRACE(value);
        auto cfg = makeConfig(
            "<cas_server_root_id>srv1</cas_server_root_id>"
            "<cas_part_folder_validate>" + value + "</cas_part_folder_validate>");
        ContentAddressedSettings settings;
        try
        {
            settings.loadFromConfig(*cfg, "disk", "/data", "/data/scratch", identity_macros);
            FAIL() << "expected the retired setting cas_part_folder_validate to be rejected as unknown";
        }
        catch (const Exception & e)
        {
            EXPECT_EQ(e.code(), ErrorCodes::UNKNOWN_SETTING);
        }
    }
}

TEST(CASContentAddressedSettings, UnknownKeyRejected)
{
    expectLoadFailureWithExactMessage(
        "<cas_server_root_id>srv1</cas_server_root_id><cas_gc_shardz>4</cas_gc_shardz>",
        ErrorCodes::UNKNOWN_SETTING,
        "Unknown setting 'cas_gc_shardz'");
}

TEST(CASContentAddressedSettings, MissingRequiredSettingNamesExternalConfigKey)
{
    expectLoadFailureWithExactMessage(
        "<cas_gc_shards>1</cas_gc_shards>",
        ErrorCodes::NO_ELEMENTS_IN_CONFIG,
        "Expected `cas_server_root_id` in config for a content-addressed disk");
}

TEST(CASContentAddressedSettings, InvalidBoundsDiagnosticNamesExternalConfigKeys)
{
    expectLoadFailureWithExactMessage(
        "<cas_server_root_id>srv1</cas_server_root_id><cas_gc_shards>0</cas_gc_shards>",
        ErrorCodes::BAD_ARGUMENTS,
        "content_addressed disk: cas_gc_interval_sec, cas_gc_shards and cas_gc_io_concurrency must be >= 1 "
        "(got 60, 0, 16)");
    /// The GC I/O pool is refused at zero for the same reason the shard count is: a zero would be a
    /// silently disabled subsystem rather than a configuration the pool can honour. One is the way to
    /// run the covered GC I/O sequentially.
    expectLoadFailureWithExactMessage(
        "<cas_server_root_id>srv1</cas_server_root_id><cas_gc_io_concurrency>0</cas_gc_io_concurrency>",
        ErrorCodes::BAD_ARGUMENTS,
        "content_addressed disk: cas_gc_interval_sec, cas_gc_shards and cas_gc_io_concurrency must be >= 1 "
        "(got 60, 1, 0)");
}

TEST(CASSettings, BulkDeleteChunkKeysBoundsAreEnforced)
{
    expectLoadFailureWithExactMessage(
        "<cas_server_root_id>srv1</cas_server_root_id>"
        "<cas_gc_bulk_delete_chunk_keys>1001</cas_gc_bulk_delete_chunk_keys>",
        ErrorCodes::BAD_ARGUMENTS,
        "content_addressed disk: gc_bulk_delete_chunk_keys must be between 1 and 1000 (got 1001)");
    expectLoadFailureWithExactMessage(
        "<cas_server_root_id>srv1</cas_server_root_id>"
        "<cas_gc_bulk_delete_chunk_keys>0</cas_gc_bulk_delete_chunk_keys>",
        ErrorCodes::BAD_ARGUMENTS,
        "content_addressed disk: gc_bulk_delete_chunk_keys must be between 1 and 1000 (got 0)");

    auto cfg = makeConfig(
        "<cas_server_root_id>srv1</cas_server_root_id>"
        "<cas_gc_bulk_delete_chunk_keys>1</cas_gc_bulk_delete_chunk_keys>");
    ContentAddressedSettings s;
    EXPECT_NO_THROW(s.loadFromConfig(*cfg, "disk", "/scratch", "/scratch", identity_macros));
    EXPECT_EQ(s[ContentAddressedSetting::gc_bulk_delete_chunk_keys].value, 1u);
}

TEST(CASContentAddressedSettings, InvalidEnumDiagnosticsNameExternalConfigKeys)
{
    expectLoadFailureWithExactMessage(
        "<cas_server_root_id>srv1</cas_server_root_id><cas_blob_hash>md5</cas_blob_hash>",
        ErrorCodes::BAD_ARGUMENTS,
        "parseBlobHashAlgo: unknown cas_blob_hash config value 'md5' (expected one of cityhash128|xxh3-128|sha256)");
    expectLoadFailureWithExactMessage(
        "<cas_server_root_id>srv1</cas_server_root_id><cas_staging_backend>remote</cas_staging_backend>",
        ErrorCodes::BAD_ARGUMENTS,
        "Unknown cas_staging_backend value 'remote' (expected 'local' or 's3')");
}

/// The point of this test is that none of these names appears anywhere in CAS code. It is not an
/// enumeration to be extended when a backend adds a setting; it samples the classes that a
/// name-based skip-list provably cannot cover.
TEST(CASContentAddressedSettings, ForeignKeysAreNeverInspected)
{
    auto cfg = makeConfig(
        "<cas_server_root_id>srv1</cas_server_root_id>"
        "<type>object_storage</type><object_storage_type>s3</object_storage_type>"
        "<metadata_type>cas</metadata_type><endpoint>http://x/y</endpoint>"
        "<path>cas_pool/</path><name>cas_test_disk</name><use_fake_transaction>1</use_fake_transaction>"
        "<http_keep_alive_timeout>60</http_keep_alive_timeout>"
        "<http_keep_alive_max_requests>100</http_keep_alive_max_requests>"
        "<connect_timeout_ms>1000</connect_timeout_ms><session_token>t</session_token>"
        "<s3_retry_attempts>7</s3_retry_attempts><s3_max_put_rps>100</s3_max_put_rps>"
        "<header>X-A: 1</header><header>X-B: 2</header>"
        "<access_header>X-C: 3</access_header><user_alice>alice</user_alice>"
        "<proxy><uri>http://proxy:8080</uri></proxy>"
        "<server_side_encryption_kms_config><key_id>k</key_id></server_side_encryption_kms_config>"
        "<account_name>acct</account_name><container_name>c</container_name>"
        "<connection_string>DefaultEndpointsProtocol=http;</connection_string>");
    ContentAddressedSettings s;
    EXPECT_NO_THROW(s.loadFromConfig(*cfg, "disk", "/scratch", "/scratch", identity_macros));
}

TEST(CASContentAddressedSettings, LegacySpellingStillLoadsDuringMigrationWindow)
{
    auto cfg = makeConfig("<server_root_id>srv1</server_root_id><gc_shards>4</gc_shards>");
    ContentAddressedSettings s;
    s.loadFromConfig(*cfg, "disk", "/scratch", "/scratch", identity_macros);
    EXPECT_EQ(s[ContentAddressedSetting::gc_shards].value, 4u);
}

TEST(CASContentAddressedSettings, RetiredGcSettingsWarnOnlyWhenSetToAnotherValue)
{
    const auto warningOf = [](const String & extra)
    {
        auto cfg = makeConfig("<cas_server_root_id>srv1</cas_server_root_id>" + extra);
        ContentAddressedSettings s;
        ScopedCasSettingsLogCapture capture;
        s.loadFromConfig(*cfg, "disk", "/scratch", "/scratch", identity_macros);
        return capture.captured();
    };

    const String meta = warningOf("<cas_gc_meta_pool_size>9</cas_gc_meta_pool_size>");
    EXPECT_NE(meta.find("`cas_gc_meta_pool_size` is ignored"), String::npos) << meta;
    EXPECT_EQ(meta.find("cas_gc_round_graduation_budget"), String::npos) << meta;

    const String carried = warningOf("<cas_gc_round_graduation_budget>11</cas_gc_round_graduation_budget>");
    EXPECT_NE(carried.find("its value 11 applies to `cas_gc_round_intent_create_budget`"), String::npos) << carried;
    EXPECT_EQ(carried.find("cas_gc_meta_pool_size"), String::npos) << carried;

    const String both = warningOf("<cas_gc_round_graduation_budget>11</cas_gc_round_graduation_budget>"
                                  "<cas_gc_round_intent_create_budget>5</cas_gc_round_intent_create_budget>");
    EXPECT_NE(both.find("`cas_gc_round_graduation_budget` is retired and ignored, because "
                        "`cas_gc_round_intent_create_budget` is set"), String::npos) << both;

    const String defaults = warningOf("<cas_gc_meta_pool_size>16</cas_gc_meta_pool_size>"
                                      "<cas_gc_round_graduation_budget>0</cas_gc_round_graduation_budget>");
    EXPECT_EQ(defaults.find("cas_gc_meta_pool_size"), String::npos) << "the default values warn: " << defaults;
    EXPECT_EQ(defaults.find("cas_gc_round_graduation_budget"), String::npos) << "the default values warn: " << defaults;
    EXPECT_EQ(warningOf("").find("cas_gc_"), String::npos);
}

TEST(CASContentAddressedSettings, PartialMigrationLoadsAndReportsEveryLegacyKey)
{
    auto cfg = makeConfig(
        "<cas_server_root_id>srv1</cas_server_root_id>"
        "<gc_shards>4</gc_shards><gc_interval_sec>7</gc_interval_sec>");
    ContentAddressedSettings s;
    String captured;
    {
        ScopedCasSettingsLogCapture capture;
        s.loadFromConfig(*cfg, "disk", "/scratch", "/scratch", identity_macros);
        captured = capture.captured();
    }
    EXPECT_EQ(s[ContentAddressedSetting::gc_shards].value, 4u);
    EXPECT_EQ(s[ContentAddressedSetting::gc_interval_sec].value, 7u);
    EXPECT_EQ(countOccurrences(captured, "superseded unprefixed spelling"), 1u);
    EXPECT_NE(captured.find("gc_shards"), String::npos);
    EXPECT_NE(captured.find("gc_interval_sec"), String::npos);
}

TEST(CASContentAddressedSettings, FullyMigratedBlockWarnsAboutNothing)
{
    auto cfg = makeConfig("<cas_server_root_id>srv1</cas_server_root_id><cas_gc_shards>4</cas_gc_shards>");
    ContentAddressedSettings s;
    ScopedCasSettingsLogCapture capture;
    s.loadFromConfig(*cfg, "disk", "/scratch", "/scratch", identity_macros);
    EXPECT_EQ(capture.captured().find("superseded"), String::npos);
}

TEST(CASContentAddressedSettings, BothSpellingsOfOneSettingRejected)
{
    auto cfg = makeConfig(
        "<cas_server_root_id>srv1</cas_server_root_id>"
        "<cas_gc_shards>4</cas_gc_shards><gc_shards>8</gc_shards>");
    ContentAddressedSettings s;
    try
    {
        s.loadFromConfig(*cfg, "disk", "/scratch", "/scratch", identity_macros);
        FAIL() << "expected the ambiguous pair to be rejected";
    }
    catch (const Exception & e)
    {
        EXPECT_EQ(e.code(), ErrorCodes::BAD_ARGUMENTS);
    }
}

TEST(CASContentAddressedSettings, MalformedRepeatedPrefixedKeyIsRejectedBeforeParsing)
{
    auto cfg = makeConfig(
        "<cas_server_root_id>srv1</cas_server_root_id>"
        "<cas_gc_shards>not-a-number</cas_gc_shards><cas_gc_shards>8</cas_gc_shards>");
    ContentAddressedSettings s;
    try
    {
        s.loadFromConfig(*cfg, "disk", "/scratch", "/scratch", identity_macros);
        FAIL() << "expected the repeated key to be rejected before parsing its value";
    }
    catch (const Exception & e)
    {
        EXPECT_EQ(e.code(), ErrorCodes::BAD_ARGUMENTS);
        EXPECT_NE(String(e.message()).find("set more than once"), String::npos);
    }
}

TEST(CASContentAddressedSettings, MalformedPrefixedValueCannotMaskBothSpellingsConflict)
{
    auto cfg = makeConfig(
        "<cas_server_root_id>srv1</cas_server_root_id>"
        "<cas_gc_shards>not-a-number</cas_gc_shards><gc_shards>8</gc_shards>");
    ContentAddressedSettings s;
    try
    {
        s.loadFromConfig(*cfg, "disk", "/scratch", "/scratch", identity_macros);
        FAIL() << "expected the ambiguous pair to be rejected before parsing its values";
    }
    catch (const Exception & e)
    {
        EXPECT_EQ(e.code(), ErrorCodes::BAD_ARGUMENTS);
        EXPECT_NE(String(e.message()).find("both"), String::npos);
    }
}

TEST(CASContentAddressedSettings, AmbiguousConfigDoesNotWarnOrPartiallyApplySettings)
{
    auto cfg = makeConfig(
        "<cas_server_root_id>srv1</cas_server_root_id>"
        "<cas_gc_shards>4</cas_gc_shards><gc_shards>8</gc_shards>");
    ContentAddressedSettings s;
    String captured;
    {
        ScopedCasSettingsLogCapture capture;
        EXPECT_THROW(s.loadFromConfig(*cfg, "disk", "/scratch", "/scratch", identity_macros), Exception);
        captured = capture.captured();
    }
    EXPECT_EQ(captured.find("are applied"), String::npos);
    EXPECT_FALSE(s[ContentAddressedSetting::gc_shards].changed);
}

TEST(CASContentAddressedSettings, UnknownPrefixedKeyDoesNotWarnOrPartiallyApplySettings)
{
    auto cfg = makeConfig(
        "<cas_gc_shards>3</cas_gc_shards><gc_enabled>0</gc_enabled>"
        "<cas_gc_shardz>8</cas_gc_shardz>");
    ContentAddressedSettings s;
    String captured;
    {
        ScopedCasSettingsLogCapture capture;
        try
        {
            s.loadFromConfig(*cfg, "disk", "/scratch", "/scratch", identity_macros);
            FAIL() << "expected the unknown prefixed key to be rejected";
        }
        catch (const Exception & e)
        {
            EXPECT_EQ(e.code(), ErrorCodes::UNKNOWN_SETTING);
        }
        captured = capture.captured();
    }
    EXPECT_EQ(captured.find("are applied"), String::npos);
    EXPECT_FALSE(s[ContentAddressedSetting::gc_shards].changed);
    EXPECT_FALSE(s[ContentAddressedSetting::gc_enabled].changed);
}

TEST(CASContentAddressedSettings, MalformedPrefixedKeyDoesNotWarnOrPartiallyApplySettings)
{
    auto cfg = makeConfig(
        "<cas_gc_shards>3</cas_gc_shards><gc_enabled>0</gc_enabled>"
        "<cas_gc_interval_sec>not-a-number</cas_gc_interval_sec>");
    ContentAddressedSettings s;
    String captured;
    {
        ScopedCasSettingsLogCapture capture;
        try
        {
            s.loadFromConfig(*cfg, "disk", "/scratch", "/scratch", identity_macros);
            FAIL() << "expected the malformed prefixed key to be rejected";
        }
        catch (const Exception &)
        {
        }
        captured = capture.captured();
    }
    EXPECT_EQ(captured.find("are applied"), String::npos);
    EXPECT_FALSE(s[ContentAddressedSetting::gc_shards].changed);
    EXPECT_FALSE(s[ContentAddressedSetting::gc_enabled].changed);
}

TEST(CASContentAddressedSettings, ValidMixedConfigCommitsAfterAllValuesValidate)
{
    auto cfg = makeConfig(
        "<cas_server_root_id>srv1</cas_server_root_id><cas_gc_shards>3</cas_gc_shards>"
        "<gc_enabled>0</gc_enabled>");
    ContentAddressedSettings s;
    s.loadFromConfig(*cfg, "disk", "/scratch", "/scratch", identity_macros);
    EXPECT_EQ(s[ContentAddressedSetting::gc_shards].value, 3u);
    EXPECT_FALSE(s[ContentAddressedSetting::gc_enabled].value);
}

TEST(CASContentAddressedSettings, SemanticInvalidPrefixedKeyDoesNotWarnOrPartiallyApplySettings)
{
    auto cfg = makeConfig(
        "<gc_enabled>0</gc_enabled><cas_gc_shards>0</cas_gc_shards>");
    ContentAddressedSettings s;
    String captured;
    {
        ScopedCasSettingsLogCapture capture;
        try
        {
            s.loadFromConfig(*cfg, "disk", "/scratch", "/scratch", identity_macros);
            FAIL() << "expected the semantically invalid prefixed key to be rejected";
        }
        catch (const Exception & e)
        {
            EXPECT_EQ(e.code(), ErrorCodes::BAD_ARGUMENTS);
        }
        captured = capture.captured();
    }
    EXPECT_EQ(captured.find("are applied"), String::npos);
    EXPECT_FALSE(s[ContentAddressedSetting::gc_enabled].changed);
    EXPECT_FALSE(s[ContentAddressedSetting::gc_shards].changed);
}

TEST(CASContentAddressedSettings, InvalidEnumDoesNotWarnOrPartiallyApplySettings)
{
    auto cfg = makeConfig(
        "<cas_server_root_id>srv1</cas_server_root_id><gc_enabled>0</gc_enabled>"
        "<cas_blob_hash>md5</cas_blob_hash>");
    ContentAddressedSettings s;
    String captured;
    {
        ScopedCasSettingsLogCapture capture;
        try
        {
            s.loadFromConfig(*cfg, "disk", "/scratch", "/scratch", identity_macros);
            FAIL() << "expected the invalid hash algorithm to be rejected";
        }
        catch (const Exception & e)
        {
            EXPECT_EQ(e.code(), ErrorCodes::BAD_ARGUMENTS);
        }
        captured = capture.captured();
    }
    EXPECT_EQ(captured.find("are applied"), String::npos);
    EXPECT_FALSE(s[ContentAddressedSetting::gc_enabled].changed);
}

/// Poco renders a repeated element as `name`, `name[1]`. A key of ours that appears twice must be
/// recognized by its base name rather than passed over as foreign, or the first value would silently win.
TEST(CASContentAddressedSettings, RepeatedKeyRejectedInEitherSpelling)
{
    for (const std::string & spelling : {std::string("gc_shards"), std::string("cas_gc_shards")})
    {
        SCOPED_TRACE(spelling);
        auto cfg = makeConfig(
            "<cas_server_root_id>srv1</cas_server_root_id>"
            "<" + spelling + ">4</" + spelling + ">"
            "<" + spelling + ">8</" + spelling + ">");
        ContentAddressedSettings s;
        try
        {
            s.loadFromConfig(*cfg, "disk", "/scratch", "/scratch", identity_macros);
            FAIL() << "expected a repeated key to be rejected";
        }
        catch (const Exception & e)
        {
            EXPECT_EQ(e.code(), ErrorCodes::BAD_ARGUMENTS);
        }
    }
}

TEST(CASContentAddressedSettings, SkipAccessCheckKeepsItsBareSpelling)
{
    auto with = makeConfig(
        "<cas_server_root_id>srv1</cas_server_root_id>"
        "<skip_access_check>1</skip_access_check>");
    ContentAddressedSettings s;
    s.loadFromConfig(*with, "disk", "/scratch", "/scratch", identity_macros);
    EXPECT_TRUE(s.skipAccessCheck());

    auto prefixed = makeConfig(
        "<cas_server_root_id>srv1</cas_server_root_id>"
        "<cas_skip_access_check>1</cas_skip_access_check>");
    ContentAddressedSettings rejected;
    try
    {
        rejected.loadFromConfig(*prefixed, "disk", "/scratch", "/scratch", identity_macros);
        FAIL() << "expected `cas_skip_access_check` to be unknown";
    }
    catch (const Exception & e)
    {
        EXPECT_EQ(e.code(), ErrorCodes::UNKNOWN_SETTING);
    }
}

TEST(CASContentAddressedSettings, PrefixedGcsCapIsNotACasSetting)
{
    auto cfg = makeConfig(
        "<cas_server_root_id>srv1</cas_server_root_id>"
        "<cas_gcs_max_conditional_put_bytes>4096</cas_gcs_max_conditional_put_bytes>");
    ContentAddressedSettings s;
    try
    {
        s.loadFromConfig(*cfg, "disk", "/scratch", "/scratch", identity_macros);
        FAIL() << "expected the prefixed cap name to be unknown";
    }
    catch (const Exception & e)
    {
        EXPECT_EQ(e.code(), ErrorCodes::UNKNOWN_SETTING);
    }
}

TEST(CASContentAddressedSettings, ValidateFailsClosed)
{
    {
        auto cfg = makeConfig("<cas_gc_shards>1</cas_gc_shards>");
        ContentAddressedSettings s;
        try
        {
            s.loadFromConfig(*cfg, "disk", "/scratch", "/scratch", identity_macros);
            FAIL() << "expected an exception";
        }
        catch (const Exception & e)
        {
            EXPECT_EQ(e.code(), ErrorCodes::NO_ELEMENTS_IN_CONFIG);
        }
    }
    {
        auto cfg = makeConfig("<cas_server_root_id></cas_server_root_id>");
        ContentAddressedSettings s;
        try
        {
            s.loadFromConfig(*cfg, "disk", "/scratch", "/scratch", identity_macros);
            FAIL() << "expected an exception";
        }
        catch (const Exception & e)
        {
            EXPECT_EQ(e.code(), ErrorCodes::BAD_ARGUMENTS);
        }
    }
    {
        auto cfg = makeConfig("<cas_server_root_id>srv1</cas_server_root_id><cas_gc_shards>0</cas_gc_shards>");
        ContentAddressedSettings s;
        EXPECT_THROW(s.loadFromConfig(*cfg, "disk", "/scratch", "/scratch", identity_macros), Exception);
    }
    {
        auto cfg = makeConfig("<cas_server_root_id>srv1</cas_server_root_id><cas_blob_hash>md5</cas_blob_hash>");
        ContentAddressedSettings s;
        EXPECT_THROW(s.loadFromConfig(*cfg, "disk", "/scratch", "/scratch", identity_macros), Exception);
    }
}

TEST(CASContentAddressedSettings, RelativeScratchPathAnchored)
{
    auto cfg = makeConfig("<cas_server_root_id>srv1</cas_server_root_id><cas_scratch_path>rel/dir</cas_scratch_path>");
    ContentAddressedSettings s;
    s.loadFromConfig(*cfg, "disk", "/data", "/data/disks/x/cas_scratch", identity_macros);
    EXPECT_EQ(s[ContentAddressedSetting::scratch_path].value, "/data/rel/dir");
}

TEST(CASContentAddressedSettings, AbsentScratchPathUsesDefaultVerbatim)
{
    auto cfg = makeConfig("<cas_server_root_id>srv1</cas_server_root_id>");
    ContentAddressedSettings s;
    s.loadFromConfig(*cfg, "disk", "/data", "/data/disks/x/cas_scratch", identity_macros);
    EXPECT_EQ(s[ContentAddressedSetting::scratch_path].value, "/data/disks/x/cas_scratch");
}

TEST(CASContentAddressedSettings, UnsafeRemountNoDelayIsOffByDefault)
{
    {
        auto cfg = makeConfig("<cas_server_root_id>srv1</cas_server_root_id>");
        ContentAddressedSettings s;
        s.loadFromConfig(*cfg, "disk", "/data", "/data/scratch", identity_macros);
        EXPECT_FALSE(s[ContentAddressedSetting::unsafe_remount_no_delay].value);
    }
    {
        auto cfg = makeConfig("<cas_server_root_id>srv1</cas_server_root_id><cas_unsafe_remount_no_delay>1</cas_unsafe_remount_no_delay>");
        ContentAddressedSettings s;
        s.loadFromConfig(*cfg, "disk", "/data", "/data/scratch", identity_macros);
        EXPECT_TRUE(s[ContentAddressedSetting::unsafe_remount_no_delay].value);
    }
}

TEST(CASContentAddressedSettings, MountLeaseTtlAboveTheMaximumIsClampedWithOneWarning)
{
    ScopedCasSettingsLogCapture capture;
    auto cfg = makeConfig("<cas_server_root_id>srv1</cas_server_root_id><cas_mount_lease_ttl_ms>3600000</cas_mount_lease_ttl_ms>");
    ContentAddressedSettings s;
    s.loadFromConfig(*cfg, "disk", "/data", "/data/scratch", identity_macros);
    EXPECT_EQ(s[ContentAddressedSetting::mount_lease_ttl_ms].value, 600'000u);
    const auto log = capture.captured();
    EXPECT_EQ(countOccurrences(log, "cas_mount_lease_ttl_ms=3600000 exceeds the maximum of 600000 ms; using 600000 ms"), 1u) << log;
}

TEST(CASContentAddressedSettings, MountLeaseTtlAtTheMaximumIsKeptSilently)
{
    ScopedCasSettingsLogCapture capture;
    auto cfg = makeConfig("<cas_server_root_id>srv1</cas_server_root_id><cas_mount_lease_ttl_ms>600000</cas_mount_lease_ttl_ms>");
    ContentAddressedSettings s;
    s.loadFromConfig(*cfg, "disk", "/data", "/data/scratch", identity_macros);
    EXPECT_EQ(s[ContentAddressedSetting::mount_lease_ttl_ms].value, 600'000u);
    EXPECT_EQ(countOccurrences(capture.captured(), "exceeds the maximum"), 0u) << capture.captured();
}

TEST(CASContentAddressedSettings, MountLeaseDefaultsSatisfyRequestBudget)
{
    ContentAddressedSettings s;
    EXPECT_EQ(s[ContentAddressedSetting::mount_lease_ttl_ms].value, 40000u);
    EXPECT_EQ(s[ContentAddressedSetting::mount_renew_period_ms].value, 10000u);

    const Cas::CasRequestBudget budget;
    const uint64_t ttl = s[ContentAddressedSetting::mount_lease_ttl_ms].value;
    const uint64_t period = s[ContentAddressedSetting::mount_renew_period_ms].value;
    EXPECT_NO_THROW(Cas::validateCasRequestBudget(budget, ttl, period, /*background_renewal=*/true));

    const uint64_t write_reserve = 2 * budget.attemptEnvelopeMs() + budget.lease_safety_margin_ms;
    EXPECT_EQ(ttl - period - write_reserve, 14000u);
}

TEST(CASContentAddressedSettings, RequestWindowDefaultAndValidation)
{
    auto cfg = makeConfig("<cas_server_root_id>srv1</cas_server_root_id>");
    ContentAddressedSettings s;
    s.loadFromConfig(*cfg, "disk", "/data", "/data/scratch", identity_macros);
    EXPECT_EQ(s[ContentAddressedSetting::request_window_ms].value, 120'000u);

    /// A writable open needs room for a write's two envelopes, its attempt and its settlement read.
    DB::Cas::CasRequestBudget budget;
    budget.request_window_ms = 2 * budget.attemptEnvelopeMs();
    DB::Cas::tests::expectThrowsCodeWithMessage(DB::ErrorCodes::BAD_ARGUMENTS, "twice the attempt envelope", [&]
    {
        DB::Cas::validateCasRequestBudget(budget, /*mount_lease_ttl_ms=*/120'000, /*mount_renew_period_ms=*/10'000,
                                          /*background_renewal=*/true);
    });
    budget.request_window_ms = 2 * budget.attemptEnvelopeMs() + 1;
    EXPECT_NO_THROW(DB::Cas::validateCasRequestBudget(budget, 120'000, 10'000, /*background_renewal=*/true));

    /// A read-only open sends no write, so one envelope is its bound.
    budget.request_window_ms = budget.attemptEnvelopeMs();
    DB::Cas::tests::expectThrowsCodeWithMessage(DB::ErrorCodes::BAD_ARGUMENTS, "larger than the attempt envelope",
        [&] { DB::Cas::validateCasRequestWindow(budget); });
    budget.request_window_ms = budget.attemptEnvelopeMs() + 1;
    EXPECT_NO_THROW(DB::Cas::validateCasRequestWindow(budget));

    EXPECT_EQ(DB::Cas::CasRequestBudget{}.recovery_retry_budget_ms, 240'000u);
}

TEST(CASContentAddressedSettings, RequestWindowSettingSetsTheWindowAndTwiceTheRecoveryBudget)
{
    auto cfg = makeConfig("<cas_server_root_id>srv1</cas_server_root_id><cas_request_window_ms>90000</cas_request_window_ms>");
    ContentAddressedSettings s;
    s.loadFromConfig(*cfg, "disk", "/data", "/data/scratch", identity_macros);

    DB::Cas::PoolConfig pool_config;
    DB::Cas::setCasRequestWindow(pool_config.cas_request_budget, s[ContentAddressedSetting::request_window_ms].value);
    EXPECT_EQ(pool_config.cas_request_budget.request_window_ms, 90'000u);
    EXPECT_EQ(pool_config.cas_request_budget.recovery_retry_budget_ms, 180'000u);

    constexpr uint64_t max = std::numeric_limits<uint64_t>::max();
    DB::Cas::setCasRequestWindow(pool_config.cas_request_budget, max / 2);
    EXPECT_EQ(pool_config.cas_request_budget.recovery_retry_budget_ms, max - 1);
    DB::Cas::setCasRequestWindow(pool_config.cas_request_budget, max / 2 + 1);
    EXPECT_EQ(pool_config.cas_request_budget.recovery_retry_budget_ms, max);
    EXPECT_EQ(pool_config.cas_request_budget.request_window_ms, max / 2 + 1);
}
