#include <gtest/gtest.h>

#include <Core/ServerUUID.h>
#include <IO/ReadHelpers.h>
#include <IO/WriteHelpers.h>
#include <Common/Logger.h>

#include <filesystem>
#include <fstream>

namespace fs = std::filesystem;
using namespace DB;

namespace
{
fs::path tempUUIDPath(const std::string & suffix)
{
    return fs::temp_directory_path() / ("clickhouse_gtest_server_uuid_" + suffix);
}
}

TEST(ServerUUID, LoadsExact36ByteAndNewlineTerminatedUUID)
{
    const std::string expected_str = "12345678-1234-5234-8234-123456789012";
    const UUID expected = parse<UUID>(expected_str);
    auto * log = &Poco::Logger::root();

    for (const auto & [suffix, payload] : std::vector<std::pair<std::string, std::string>>{
             {"exact", expected_str},
             {"lf", expected_str + "\n"},
             {"crlf", expected_str + "\r\n"},
         })
    {
        fs::path path = tempUUIDPath(suffix);
        {
            std::ofstream out(path, std::ios::binary | std::ios::trunc);
            out << payload;
        }
        SCOPE_EXIT({
            std::error_code ec;
            fs::remove(path, ec);
        });

        const UUID loaded = loadServerUUID(path, log);
        EXPECT_EQ(loaded, expected) << "failed for suffix: " << suffix;
    }
}

TEST(ServerUUID, OverwritesCorruptedTrailingPayloadWithFreshUUID)
{
    const std::string base_str = "12345678-1234-5234-8234-123456789012";
    const UUID base_uuid = parse<UUID>(base_str);
    auto * log = &Poco::Logger::root();

    fs::path path = tempUUIDPath("corrupt");
    {
        std::ofstream out(path, std::ios::binary | std::ios::trunc);
        out << base_str << " trailing-garbage";
    }
    SCOPE_EXIT({
        std::error_code ec;
        fs::remove(path, ec);
    });

    const UUID loaded = loadServerUUID(path, log);
    EXPECT_NE(loaded, UUIDHelpers::Nil);
    EXPECT_NE(loaded, base_uuid);
}
