#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasGcOutcomes.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasCodecUtil.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasFormat.h>
#include <cas_format.pb.h>
#include <Common/Exception.h>
#include <cstring>
#include <tuple>

namespace DB
{
namespace ErrorCodes
{
    extern const int CORRUPTED_DATA;
    extern const int LOGICAL_ERROR;
}
}

namespace DB::Cas
{

namespace Proto = ::clickhouse::cas::format;

namespace
{

/// `OutcomeKind` <-> uint32. Unknown value on decode is corruption (fail closed). The proto field
/// carries the enum value directly (mirrors the C++ enum uint8_t backing).
uint32_t outcomeKindToProto(OutcomeKind outcome)
{
    return static_cast<uint32_t>(outcome);
}

OutcomeKind outcomeKindFromProto(uint32_t v, std::string_view what)
{
    switch (v)
    {
        case static_cast<uint32_t>(OutcomeKind::Deleted):  return OutcomeKind::Deleted;
        case static_cast<uint32_t>(OutcomeKind::Absent):   return OutcomeKind::Absent;
        case static_cast<uint32_t>(OutcomeKind::Replaced): return OutcomeKind::Replaced;
        case static_cast<uint32_t>(OutcomeKind::Spared):   return OutcomeKind::Spared;
        default:
            throw Exception(ErrorCodes::CORRUPTED_DATA,
                "CAS {}: invalid outcome kind {} in outcome entry", what, v);
    }
}

/// `ObjectKind` <-> uint32 (mirrors CasGcFormats.cpp — reuse same enum values).
uint32_t objectKindToProto(ObjectKind kind)
{
    return static_cast<uint32_t>(kind);
}

ObjectKind objectKindFromProto(uint32_t v, std::string_view what)
{
    switch (v)
    {
        case static_cast<uint32_t>(ObjectKind::Blob): return ObjectKind::Blob;
        default:
            throw Exception(ErrorCodes::CORRUPTED_DATA,
                "CAS {}: invalid object kind {} in outcome entry", what, v);
    }
}

/// `TokenType` <-> uint32 (mirrors CasGcFormats.cpp — reuse same enum values).
uint32_t tokenTypeToProto(TokenType t)
{
    return static_cast<uint32_t>(t);
}

TokenType tokenTypeFromProto(uint32_t v, std::string_view what)
{
    switch (v)
    {
        case static_cast<uint32_t>(TokenType::ETag):       return TokenType::ETag;
        case static_cast<uint32_t>(TokenType::Generation): return TokenType::Generation;
        case static_cast<uint32_t>(TokenType::Emulated):   return TokenType::Emulated;
        default:
            throw Exception(ErrorCodes::CORRUPTED_DATA,
                "CAS {}: invalid token type {} in outcome entry", what, v);
    }
}

/// CAS pluggable-blob-hash Phase 2 Task 5: `OutcomeEntry::hash` is a `BlobDigest` (always 32 bytes,
/// tail zero beyond the pool's width) — serialize the WHOLE fixed-size array unconditionally rather
/// than a pool-width slice. This keeps the codec self-describing (encode/decode need no `PoolMeta`/
/// `DigestCodec` in scope) at the cost of a fixed 16 spare zero bytes per entry for a 128-bit pool —
/// outcome logs are ephemeral per-round bookkeeping, not the storage-cost driver.
String digestToBytesFull(const BlobDigest & d)
{
    return String(reinterpret_cast<const char *>(d.bytes.data()), d.bytes.size());
}

BlobDigest digestFromBytesFull(const std::string & b, std::string_view what)
{
    if (b.size() != std::tuple_size_v<decltype(BlobDigest::bytes)>)
        throw Exception(ErrorCodes::CORRUPTED_DATA, "CAS {}: hash must be {} bytes, got {}",
            what, std::tuple_size_v<decltype(BlobDigest::bytes)>, b.size());
    BlobDigest d;
    memcpy(d.bytes.data(), b.data(), b.size());
    return d;
}

}

String encodeOutcomeLog(const OutcomeLog & log)
{
    Cas::Proto::GcOutcomeLogProto msg;

    /// Set CasHeader as field 1 (pure protobuf — no binary prefix).
    auto * hdr = msg.mutable_header();
    hdr->set_magic(magicFor(FormatId::GcOutcomes));
    hdr->set_writer_version(currentWriterVersion());
    hdr->set_compatibility_version(currentCompatibilityVersion());

    for (const auto & entry : log.entries)
    {
        auto * pe = msg.add_entries();
        pe->set_kind(objectKindToProto(entry.kind));
        pe->set_hash(digestToBytesFull(entry.hash));
        pe->set_token_value(entry.token.value);
        pe->set_token_type(tokenTypeToProto(entry.token.type));
        pe->set_outcome(outcomeKindToProto(entry.outcome));
    }

    std::string out;
    if (!msg.SerializeToString(&out))
        throw Exception(ErrorCodes::LOGICAL_ERROR, "CAS outcome log: protobuf serialization failed");
    return out;
}

OutcomeLog decodeOutcomeLog(std::string_view data)
{
    return decodeGuarded("outcome log", [&]
    {
        if (data.empty())
            throw Exception(ErrorCodes::CORRUPTED_DATA, "CAS outcome log: empty object");

        /// Parse the whole message directly (pure protobuf, no binary prefix).
        Cas::Proto::GcOutcomeLogProto msg;
        if (!msg.ParseFromArray(data.data(), static_cast<int>(data.size())))
            throw Exception(ErrorCodes::CORRUPTED_DATA, "CAS outcome log: protobuf parse failed");

        /// Check magic then compatibility_version BEFORE reading any other fields.
        if (msg.header().magic() != magicFor(FormatId::GcOutcomes))
            throw Exception(ErrorCodes::CORRUPTED_DATA,
                "CAS outcome log: bad magic (got 0x{:08x}, expected 0x{:08x})",
                msg.header().magic(), magicFor(FormatId::GcOutcomes));
        checkCompatibility(msg.header().compatibility_version(), "outcome log");

        OutcomeLog log;
        log.entries.reserve(static_cast<size_t>(msg.entries_size()));
        for (const auto & pe : msg.entries())
        {
            OutcomeEntry entry;
            entry.kind = objectKindFromProto(pe.kind(), "outcome log");
            entry.hash = digestFromBytesFull(pe.hash(), "outcome log hash");
            entry.token.value = pe.token_value();
            entry.token.type = tokenTypeFromProto(pe.token_type(), "outcome log");
            entry.outcome = outcomeKindFromProto(pe.outcome(), "outcome log");
            log.entries.push_back(std::move(entry));
        }
        return log;
    });
}

}
