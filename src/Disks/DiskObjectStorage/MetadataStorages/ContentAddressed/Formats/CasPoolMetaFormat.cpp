#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Formats/CasPoolMetaFormat.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Formats/CasTextFormat.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Formats/CasWireVocab.h>
#include <Common/Exception.h>
#include <IO/ReadBufferFromMemory.h>

namespace DB
{
namespace ErrorCodes
{
    extern const int CORRUPTED_DATA;
    extern const int UNKNOWN_FORMAT_VERSION;
}
}

namespace DB::Cas
{

/// Minimum `blob_header_len` that provably fits the v3 `cas_blob` JSON envelope's mandatory (always-
/// written) non-ref fields, computed at type maxima from `encodeEnvelopeHeader` (CasBlobEnvelopeFormat.cpp):
///   {"type":"cas_blob"                                        18
///   ,"version":<u32>                         11 + 10            21
///   ,"incarnation_tag":"<32 hex>"           19 + 34            53
///   ,"build_id":"<32 hex>"                  12 + 34            46
///   ,"created_at_ms":<u64>                  17 + 20            37
///   ,"creator_server_id":"<32 hex>"         21 + 34            55
///   ,"operation":"<word>"                   13 + 10            23
///                                      (`mutation`: 8 + 2 quotes)
///   ,"clickhouse_version":<u32>             22 + 10            32
///                                                  non-ref JSON = 285 bytes
/// The encoder then always frames the ref: `,"intended_ref":` (16) + `""` (2) + `}` (1), and
/// reserves byte blob_header_len-1 for '\n' (1) = 20 bytes. The mandatory content therefore needs
/// 285 + 20 = 305 bytes. Below that, `encodeEnvelopeHeader` throws `LOGICAL_ERROR` on the first blob
/// write (the old drop-and-retry that used to mask this is gone). We floor at 320, a multiple of 8
/// comfortably above 305. This leaves exactly 15 bytes for the diagnostic ref at the floor (and more
/// above it) and is well under the 384 default, so a misconfigured pool fails at creation with
/// `BAD_ARGUMENTS`, not at the first write with `LOGICAL_ERROR`.
static constexpr uint64_t kMinBlobHeaderLen = 320;

void validatePoolBlobHeaderLen(uint64_t blob_header_len, int error_code, std::string_view what)
{
    if (blob_header_len < kMinBlobHeaderLen)
        throw Exception(error_code, "CAS {}: blob_header_len must be >= {} (v3 envelope minimum), got {}",
            what, kMinBlobHeaderLen, blob_header_len);
    if (blob_header_len % 8 != 0)
        throw Exception(error_code, "CAS {}: blob_header_len must be a multiple of 8, got {}", what, blob_header_len);
    if (blob_header_len > 16 * 1024)
        throw Exception(error_code, "CAS {}: blob_header_len must be <= 16384, got {}", what, blob_header_len);
}

void validatePoolAlgosUsed(const std::vector<uint8_t> & algos_used, int error_code, std::string_view what)
{
    if (algos_used.empty())
        throw Exception(error_code, "CAS {}: algos_used must be non-empty", what);
    for (size_t i = 0; i < algos_used.size(); ++i)
    {
        try
        {
            blobHashAlgoName(static_cast<BlobHashAlgo>(algos_used[i]));
        }
        catch (const Exception &)
        {
            throw Exception(error_code, "CAS {}: algos_used contains an unknown algo {}", what, algos_used[i]);
        }
        if (i > 0 && algos_used[i] <= algos_used[i - 1])
            throw Exception(error_code,
                "CAS {}: algos_used must be strictly sorted with no duplicates, got {} at index {} not after {}",
                what, algos_used[i], i, algos_used[i - 1]);
    }
}

String encodePoolMeta(const PoolMeta & pm)
{
    CasJsonWriter out(256);
    writeHeaderLine(out, FormatId::PoolMeta);

    bool first = true;
    writeKey(out, "pool_id", first);
    writeHex128Value(out, pm.pool_id);
    writeKey(out, "blob_header_len", first);
    writeIntText(pm.blob_header_len, out);
    writeKey(out, "gc_shards", first);
    writeIntText(pm.gc_shards, out);
    writeKey(out, "min_reader_generation", first);
    writeIntText(pm.min_reader_generation, out);
    writeKey(out, "algorithms_used", first);
    {
        /// Comma-joined algo words (tiny list, <=3): "ch128" or "ch128,sha256".
        String joined;
        for (size_t i = 0; i < pm.algos_used.size(); ++i)
        {
            if (i != 0)
                joined += ',';
            joined += blobHashAlgoName(static_cast<BlobHashAlgo>(pm.algos_used[i]));
        }
        writeStringValue(out, joined);
    }
    closeObject(out, first);
    writeChar('\n', out);

    return std::move(out).take();
}

PoolMeta decodePoolMeta(std::string_view data)
{
    ReadBufferFromMemory in(data.data(), data.size());
    const TextHeader header = expectHeaderLine(in, FormatId::PoolMeta);

    /// An older pool predates a breaking ref-layer change this build cannot reconcile, so
    /// reject it before reading the metadata body. Writers always emit the current generation, while
    /// `expectHeaderLine` separately rejects a future generation that this build cannot understand.
    /// Generation 10 is the latest recreate-only authority floor and rejects old pools before any
    /// mount lease body lacking its durable write-attempt identity can be interpreted.
    if (header.v < kMountWriteAttemptIdGeneration)
        throw Exception(ErrorCodes::UNKNOWN_FORMAT_VERSION,
            "CAS pool format {} predates generation-10 mount-attempt-identity floor; recreate the pool. "
            "This build requires the durable mount write attempt identity "
            "in the generation-10 format "
            "(generation {}+), and CAS is pre-release: there is no in-place migration.",
            header.v, kMountWriteAttemptIdGeneration);

    const String body = readLine(in, traitsFor(FormatId::PoolMeta).line_cap, "pool meta");
    ReadBufferFromMemory body_in(body.data(), body.size());
    JsonObjectReader r(body_in, KeyStrictness::Tolerant, "pool meta");

    PoolMeta pm;
    bool saw_pid = false;
    bool saw_gc_shards = false;
    String key;
    while (r.nextKey(key))
    {
        if (key == "pool_id")
        {
            pm.pool_id = r.readHex128();
            saw_pid = true;
        }
        else if (key == "blob_header_len")
            pm.blob_header_len = r.readU64Number();
        else if (key == "gc_shards")
        {
            pm.gc_shards = r.readU64Number();
            saw_gc_shards = true;
        }
        else if (key == "min_reader_generation")
            pm.min_reader_generation = r.readU64Number();
        else if (key == "algorithms_used")
        {
            const String joined = r.readString();
            size_t start = 0;
            while (start <= joined.size())
            {
                const size_t comma = joined.find(',', start);
                const String word = joined.substr(start, comma == String::npos ? String::npos : comma - start);
                if (word.empty())
                    throw Exception(ErrorCodes::CORRUPTED_DATA, "CAS pool meta: empty algo word in '{}'", joined);
                pm.algos_used.push_back(static_cast<uint8_t>(blobHashAlgoFromWord(word, "pool meta algo")));
                if (comma == String::npos)
                    break;
                start = comma + 1;
            }
        }
        else
            r.skipUnknown(key);
    }
    if (!saw_pid)
        throw Exception(ErrorCodes::CORRUPTED_DATA, "CAS pool meta: missing pool_id");
    if (!saw_gc_shards || pm.gc_shards == 0)
        throw Exception(ErrorCodes::CORRUPTED_DATA, "CAS pool meta: missing or zero gc_shards");
    if (!body_in.eof())
        throw Exception(ErrorCodes::CORRUPTED_DATA, "CAS pool meta: junk after body object");
    if (!in.eof())
        throw Exception(ErrorCodes::CORRUPTED_DATA, "CAS pool meta: trailing bytes after body line");

    validatePoolBlobHeaderLen(pm.blob_header_len, ErrorCodes::CORRUPTED_DATA, "pool meta");
    validatePoolAlgosUsed(pm.algos_used, ErrorCodes::CORRUPTED_DATA, "pool meta");

    if (G_BUILD < pm.min_reader_generation)
        throw Exception(ErrorCodes::UNKNOWN_FORMAT_VERSION,
            "CAS pool meta: pool requires reader generation {} but this build supports at most {}",
            pm.min_reader_generation, G_BUILD);

    return pm;
}

}
