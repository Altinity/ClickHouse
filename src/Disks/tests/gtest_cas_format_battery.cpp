#include "cas_format_test_battery.h"
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasIds.h>
#include <IO/ReadBufferFromMemory.h>
#include <IO/WriteBufferFromString.h>
#include <IO/WriteHelpers.h>

using namespace DB::Cas;

namespace DB { namespace ErrorCodes { extern const int CORRUPTED_DATA; } }

namespace
{
String toyEncode()
{
    DB::WriteBufferFromOwnString out;
    writeHeaderLine(out, FormatId::PoolMeta);
    bool first = true;
    writeKey(out, "pid", first);
    writeHex128Value(out, hexToU128("00112233445566778899aabbccddeeff"));
    writeKey(out, "gen", first);
    writeU64StringValue(out, 3);
    closeObject(out, first);
    DB::writeChar('\n', out);
    out.finalize();
    return sealObject(FormatId::PoolMeta, out.str());
}

void toyDecode(std::string_view stored)
{
    const String text = openObject(FormatId::PoolMeta, stored);
    DB::ReadBufferFromMemory in(text.data(), text.size());
    expectHeaderLine(in, FormatId::PoolMeta);
    const String body = readLine(in, traitsFor(FormatId::PoolMeta).line_cap, "toy");
    DB::ReadBufferFromMemory body_in(body.data(), body.size());
    JsonObjectReader r(body_in, KeyStrictness::Tolerant, "toy");
    String key;
    bool saw_pid = false;
    while (r.nextKey(key))
    {
        if (key == "pid") { r.readHex128(); saw_pid = true; }
        else if (key == "gen") { r.readU64String(); }
        else r.skipUnknown(key);
    }
    if (!saw_pid)
        throw DB::Exception(DB::ErrorCodes::CORRUPTED_DATA, "toy: missing pid");
    if (!in.eof())
        throw DB::Exception(DB::ErrorCodes::CORRUPTED_DATA, "toy: junk after body");
}
}

TEST(CasFormatBattery, ProvingInstance)
{
    runFormatBattery(FormatBatteryCase{
        .id = FormatId::PoolMeta,
        .encode = toyEncode,
        .decode = toyDecode,
        .golden = "{\"type\":\"cas_pool_meta\",\"v\":3}\n"
                  "{\"pid\":\"00112233445566778899aabbccddeeff\",\"gen\":\"3\"}\n"});
}
