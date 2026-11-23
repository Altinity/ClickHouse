#include "cas_format_test_battery.h"
#include "cas_test_helpers.h"
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Formats/CasBlobIntentFormat.h>
#include <IO/ReadBufferFromMemory.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Formats/CasPoolMetaFormat.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Primitives/CasTypes.h>

using namespace DB::Cas;

namespace
{
template <typename F>
void expectThrowsCode(int expected_code, F && fn)
{
    try
    {
        fn();
        FAIL() << "expected exception " << expected_code;
    }
    catch (const DB::Exception & e)
    {
        EXPECT_EQ(e.code(), expected_code) << e.message();
    }
}
}

/// The real cas_pool_meta case replaces the phase-1 toy proving instance. Every other control-plane
/// format registers its own battery row in its own gtest_cas_<object>_format.cpp file (Tasks 3-6).

CAS_BATTERY_COVERS(PoolMeta);

TEST(CASFormatBattery, PoolMeta)
{
    PoolMeta pm;
    pm.pool_id = hexToU128("00112233445566778899aabbccddeeff");
    pm.blob_header_len = 256;
    pm.min_reader_generation = 1;
    pm.algos_used = {static_cast<uint8_t>(BlobHashAlgo::CityHash128)};
    runFormatBattery(FormatBatteryCase{
        .id = FormatId::PoolMeta,
        .encode = [&] { return sealObject(FormatId::PoolMeta, encodePoolMeta(pm)); },
        .decode = [](std::string_view s) { decodePoolMeta(std::string(openObject(FormatId::PoolMeta, s))); },
        .golden = currentFormatHeader("cas_pool_meta") +
                  "{\"pool_id\":\"00112233445566778899aabbccddeeff\",\"blob_header_len\":256,\"gc_shards\":1,\"min_reader_generation\":1,\"algos_used\":[\"ch128\"]}\n"});
}

TEST(CASPoolMeta, RejectsInvalidAlgoArrays)
{
    const auto decode = [](std::string_view algos_used)
    {
        return decodePoolMeta("{\"type\":\"cas_pool_meta\",\"v\":1}\n"
            "{\"pool_id\":\"00112233445566778899aabbccddeeff\",\"blob_header_len\":256,\"gc_shards\":1,\"min_reader_generation\":1,\"algos_used\":" + String(algos_used) + "}\n");
    };

    /// The first value is the field's PREVIOUS encoding -- a comma-joined string inside one JSON
    /// value. It must fail closed rather than round-trip; the rest are malformed arrays.
    for (const std::string_view bad : {"\"ch128,sha256\"", "[\"ch128\",1]", "[]", "[\"sha256\",\"ch128\"]", "[\"ch128\",\"ch128\"]", "[\"unknown\"]"})
        expectThrowsCode(DB::ErrorCodes::CORRUPTED_DATA, [&] { decode(bad); });
}

TEST(CASPoolMeta, ValidateAlgosUsedRejectsUnknownByte)
{
    expectThrowsCode(DB::ErrorCodes::CORRUPTED_DATA,
        [] { validatePoolAlgosUsed({7}, DB::ErrorCodes::CORRUPTED_DATA, "t"); });
}

CAS_BATTERY_COVERS(BlobIntent);

/// The record has no codec of its own (consumers compare bytes), so the battery decodes it the way a
/// header-gated reader would: header line, then a body line that must be exactly `{}`.
namespace
{

void decodeBlobIntent(std::string_view stored)
{
    const String text(openObject(FormatId::BlobIntent, stored));
    DB::ReadBufferFromMemory in(text.data(), text.size());
    expectHeaderLine(in, FormatId::BlobIntent);
    const String body = readLine(in, traitsFor(FormatId::BlobIntent).line_cap, "cas_blob_intent");
    if (body != "{}" || !in.eof())
        throw DB::Exception(DB::ErrorCodes::CORRUPTED_DATA, "cas_blob_intent: unexpected body '{}'", body);
}

}

TEST(CASFormatBattery, BlobIntent)
{
    const DB::Cas::tests::ScopedReadCeiling ceiling(G_BLOB_INTENTS);
    runFormatBattery(FormatBatteryCase{
        .id = FormatId::BlobIntent,
        .encode = [] { return sealObject(FormatId::BlobIntent, blobIntentRecord()); },
        .decode = decodeBlobIntent,
        .golden = "{\"type\":\"cas_blob_intent\",\"v\":4}\n{}\n",
        .make_future_version = [](std::string_view text)
        {
            String future(text);
            const String now = "\"v\":4";
            future.replace(future.find(now), now.size(), "\"v\":5");
            return future;
        }});
}

TEST(CASFormatBattery, BlobIntentTraitsAreThePinnedRecordClass)
{
    const FormatTraits & t = traitsFor(FormatId::BlobIntent);
    EXPECT_EQ(t.type, "cas_blob_intent");
    EXPECT_EQ(t.family, TextFamily::Control);
    EXPECT_EQ(t.compression, CompressionPolicy::PinnedRaw);
    EXPECT_EQ(t.object_cap, 4u * 1024);
    EXPECT_EQ(t.line_cap, 1u * 1024);
    EXPECT_EQ(storedSuffix(FormatId::BlobIntent), "");
}

TEST(CASFormatBattery, BlobIntentIsRefusedByAnOlderReader)
{
    {
        const DB::Cas::tests::ScopedReadCeiling ceiling(G_BLOB_INTENTS);
        decodeBlobIntent(blobIntentRecord());   /// positive control: the record decodes at its own generation
    }
    const DB::Cas::tests::ScopedReadCeiling ceiling(3);
    expectThrowsCode(DB::ErrorCodes::UNKNOWN_FORMAT_VERSION, [] { decodeBlobIntent(blobIntentRecord()); });
}
