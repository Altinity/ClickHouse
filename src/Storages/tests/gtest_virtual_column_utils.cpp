#include <gtest/gtest.h>
#include <Common/tests/gtest_global_context.h>
#include <Storages/VirtualColumnUtils.h>
#include <DataTypes/DataTypeString.h>

using namespace DB;

static std::vector<std::string> test_paths = {
    "/some/folder/key1=val1/key2=val2/file1.txt",
    "/data/keyA=valA/keyB=valB/keyC=valC/file2.txt",
    "/another/dir/x=1/y=2/z=3/file3.txt",
    "/tiny/path/a=b/file4.txt",
    "/yet/another/path/k1=v1/k2=v2/k3=v3/k4=v4/k5=v5/"
};

TEST(VirtualColumnUtils, BenchmarkRegexParser)
{
    static constexpr int iterations = 1000000;

    auto start_extractkv = std::chrono::steady_clock::now();

    for (int i = 0; i < iterations; ++i)
    {
        // Pick from 5 different paths
        const auto & path = test_paths[i % 5];
        auto result = VirtualColumnUtils::parseHivePartitioningKeysAndValues(path);
        ASSERT_TRUE(!result.empty());
    }

    auto end_extractkv = std::chrono::steady_clock::now();
    auto duration_ms_extractkv = std::chrono::duration_cast<std::chrono::milliseconds>(end_extractkv - start_extractkv).count();

    std::cout << "[BenchmarkExtractkvParser] "
              << iterations << " iterations across 5 paths took "
              << duration_ms_extractkv << " ms\n";

    auto start = std::chrono::steady_clock::now();

    for (int i = 0; i < iterations; ++i)
    {
        // Pick from 5 different paths
        const auto & path = test_paths[i % 5];
        auto result = VirtualColumnUtils::parseHivePartitioningKeysAndValuesRegex(path);
        ASSERT_TRUE(!result.empty());
    }

    auto end = std::chrono::steady_clock::now();
    auto duration_ms = std::chrono::duration_cast<std::chrono::milliseconds>(end - start).count();

    std::cout << "[BenchmarkRegexParser] "
              << iterations << " iterations across 5 paths took "
              << duration_ms << " ms\n";
}
