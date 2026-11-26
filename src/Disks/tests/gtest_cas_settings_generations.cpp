#include <gtest/gtest.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/ContentAddressedSettings.h>
#include <Poco/AutoPtr.h>
#include <Poco/Util/XMLConfiguration.h>
#include <sstream>
#include "cas_test_helpers.h"

using namespace DB;

namespace DB::ErrorCodes
{
    extern const int BAD_ARGUMENTS;
}

namespace DB::ContentAddressedSetting
{
    extern const ContentAddressedSettingsUInt64 gc_snapshot_generations_to_keep;
}

namespace
{
struct LoadedGenerations
{
    UInt64 value = 0;
    String warnings;
};

LoadedGenerations loadGenerations(const String & value)
{
    std::istringstream iss(
        "<clickhouse><disk><cas_server_root_id>srv1</cas_server_root_id>"
        "<cas_gc_snapshot_generations_to_keep>" + value + "</cas_gc_snapshot_generations_to_keep></disk></clickhouse>");
    Poco::AutoPtr<Poco::Util::XMLConfiguration> cfg(new Poco::Util::XMLConfiguration(iss));
    ContentAddressedSettings settings;
    Cas::tests::ScopedLogCapture capture("ContentAddressedSettings", "warning");
    settings.loadFromConfig(*cfg, "disk", "/scratch", "/scratch", [](const std::string & s) { return s; });
    return {settings[ContentAddressedSetting::gc_snapshot_generations_to_keep].value, capture.captured()};
}
}

TEST(CASSettingsGenerations, OneLoadsAsTwoWithAWarning)
{
    const auto loaded = loadGenerations("1");
    EXPECT_EQ(loaded.value, 2u);
    EXPECT_NE(loaded.warnings.find("cas_gc_snapshot_generations_to_keep"), String::npos) << loaded.warnings;
}

TEST(CASSettingsGenerations, ZeroIsRefusedWithAnUpgradeMessage)
{
    Cas::tests::expectThrowsCodeWithMessage(ErrorCodes::BAD_ARGUMENTS, "large number", [] { loadGenerations("0"); });
}

TEST(CASSettingsGenerations, TwoAndThreeLoadUnchanged)
{
    for (const UInt64 value : {2u, 3u})
    {
        const auto loaded = loadGenerations(std::to_string(value));
        EXPECT_EQ(loaded.value, value);
        EXPECT_EQ(loaded.warnings.find("cas_gc_snapshot_generations_to_keep"), String::npos) << loaded.warnings;
    }
}
