#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasGcFormats.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasCodecUtil.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasEnumStrings.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasFormat.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasIds.h>
#include <cas_root_shard.pb.h>
#include <IO/ReadBufferFromMemory.h>
#include <IO/WriteBufferFromString.h>
#include <IO/WriteHelpers.h>
#include <Common/Exception.h>
#include <base/defines.h>
#include <algorithm>

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

namespace
{

constexpr std::string_view GC_STATE_MAGIC  = "CAGT";
constexpr std::string_view RETIRED_SET_MAGIC = "CART";

/// ObjectKind <-> uint32 for the RetiredEntryProto.kind field (mirrors the enum values).
uint32_t objectKindToProto(ObjectKind kind)
{
    return static_cast<uint32_t>(kind);
}

ObjectKind objectKindFromProto(uint32_t v, std::string_view what)
{
    switch (v)
    {
        case static_cast<uint32_t>(ObjectKind::Blob): return ObjectKind::Blob;
        case static_cast<uint32_t>(ObjectKind::Tree): return ObjectKind::Tree;
        default:
            throw Exception(ErrorCodes::CORRUPTED_DATA,
                "CAS {}: invalid object kind {} in retired entry", what, v);
    }
}

/// TokenType <-> uint32 for the RetiredEntryProto.token_type field (mirrors the enum values).
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
                "CAS {}: invalid token type {} in retired entry", what, v);
    }
}

}

String encodeGcState(const GcState & state)
{
    chassert(state.snap_shards >= 1);   /// catch a zeroed GC constant at the write site

    Cas::Proto::GcStateProto msg;
    msg.set_round(state.round);
    msg.set_fence_seq(state.fence_seq);
    msg.set_snap_shards(state.snap_shards);
    msg.set_snap_generation(state.snap_generation);
    msg.set_snap_pruned_through(state.snap_pruned_through);

    auto * lease = msg.mutable_lease();
    lease->set_owner(u128ToBytesBE(state.lease.owner));
    lease->set_seq(state.lease.seq);

    auto & fv = *msg.mutable_fence_version();
    for (const auto & [round, inner] : state.fence_version)
    {
        Cas::Proto::FenceVersionInnerProto inner_proto;
        auto & shards = *inner_proto.mutable_shards();
        for (const auto & [shard, version] : inner)
            shards[shard] = version;
        fv[round] = std::move(inner_proto);
    }

    std::string body;
    if (!msg.SerializeToString(&body))
        throw Exception(ErrorCodes::LOGICAL_ERROR, "CAS gc/state: protobuf serialization failed");

    WriteBufferFromOwnString out;
    Cas::writeFramingHeader(out, GC_STATE_MAGIC, Cas::currentWriterVersion(Cas::FormatId::GcState));
    writeString(body, out);
    return std::move(out.str());
}

GcState decodeGcState(std::string_view data)
{
    if (data.empty())
        throw Exception(ErrorCodes::CORRUPTED_DATA, "CAS gc/state: empty object");

    ReadBufferFromMemory in(data.data(), data.size());
    Cas::readFramingHeader(in, GC_STATE_MAGIC, "gc/state");
    const std::string_view body = data.substr(Cas::FRAMING_HEADER_SIZE);

    Cas::Proto::GcStateProto msg;
    if (!msg.ParseFromArray(body.data(), static_cast<int>(body.size())))
        throw Exception(ErrorCodes::CORRUPTED_DATA, "CAS gc/state: protobuf parse failed");

    GcState state;
    state.round = msg.round();
    state.fence_seq = msg.fence_seq();
    state.snap_shards = msg.snap_shards();
    if (state.snap_shards == 0)
        throw Exception(ErrorCodes::CORRUPTED_DATA, "CAS gc/state: snap_shards must be >= 1");
    state.snap_generation = msg.snap_generation();
    state.snap_pruned_through = msg.snap_pruned_through();

    state.lease.owner = u128FromBytesBE(msg.lease().owner(), "gc/state lease owner");
    state.lease.seq = msg.lease().seq();

    for (const auto & [round, inner_proto] : msg.fence_version())
    {
        std::map<String, uint64_t> inner;
        for (const auto & [shard, version] : inner_proto.shards())
            inner[shard] = version;
        state.fence_version[round] = std::move(inner);
    }

    return state;
}

String encodeRetiredSet(const RetiredSet & set)
{
    Cas::Proto::RetiredSetProto msg;

    /// Sort entries deterministically (kind, hash-BE, token_value, token_type, size) so the
    /// encoded bytes are stable across encodes — mirrors the JSON encoder's ordered iteration.
    std::vector<const RetiredEntry *> sorted;
    sorted.reserve(set.entries.size());
    for (const auto & e : set.entries)
        sorted.push_back(&e);

    std::sort(sorted.begin(), sorted.end(), [](const RetiredEntry * a, const RetiredEntry * b)
    {
        if (a->kind != b->kind)
            return static_cast<uint8_t>(a->kind) < static_cast<uint8_t>(b->kind);
        const std::string ha = u128ToBytesBE(a->hash);
        const std::string hb = u128ToBytesBE(b->hash);
        if (ha != hb)
            return ha < hb;
        if (a->token.value != b->token.value)
            return a->token.value < b->token.value;
        if (a->token.type != b->token.type)
            return static_cast<uint8_t>(a->token.type) < static_cast<uint8_t>(b->token.type);
        return a->size < b->size;
    });

    for (const auto * ep : sorted)
    {
        auto * pe = msg.add_entries();
        pe->set_kind(objectKindToProto(ep->kind));
        pe->set_hash(u128ToBytesBE(ep->hash));
        pe->set_token_value(ep->token.value);
        pe->set_token_type(tokenTypeToProto(ep->token.type));
        pe->set_size(ep->size);
    }

    std::string body;
    if (!msg.SerializeToString(&body))
        throw Exception(ErrorCodes::LOGICAL_ERROR, "CAS retired set: protobuf serialization failed");

    WriteBufferFromOwnString out;
    Cas::writeFramingHeader(out, RETIRED_SET_MAGIC, Cas::currentWriterVersion(Cas::FormatId::RetiredSet));
    writeString(body, out);
    return std::move(out.str());
}

RetiredSet decodeRetiredSet(std::string_view data)
{
    if (data.empty())
        throw Exception(ErrorCodes::CORRUPTED_DATA, "CAS retired set: empty object");

    ReadBufferFromMemory in(data.data(), data.size());
    Cas::readFramingHeader(in, RETIRED_SET_MAGIC, "retired set");
    const std::string_view body = data.substr(Cas::FRAMING_HEADER_SIZE);

    Cas::Proto::RetiredSetProto msg;
    if (!msg.ParseFromArray(body.data(), static_cast<int>(body.size())))
        throw Exception(ErrorCodes::CORRUPTED_DATA, "CAS retired set: protobuf parse failed");

    RetiredSet set;
    set.entries.reserve(static_cast<size_t>(msg.entries_size()));
    for (const auto & pe : msg.entries())
    {
        RetiredEntry entry;
        entry.kind = objectKindFromProto(pe.kind(), "retired set");
        entry.hash = u128FromBytesBE(pe.hash(), "retired set hash");
        entry.token.value = pe.token_value();
        entry.token.type = tokenTypeFromProto(pe.token_type(), "retired set");
        entry.size = pe.size();
        set.entries.push_back(std::move(entry));
    }
    return set;
}

String encodeGcHeartbeat(const GcHeartbeat & hb)
{
    String out(24, '\0');
    for (int i = 0; i < 16; ++i)
        out[i] = static_cast<char>(static_cast<UInt8>(hb.owner >> (8 * (15 - i))));
    for (int i = 0; i < 8; ++i)
        out[16 + i] = static_cast<char>(static_cast<UInt8>(hb.hb_seq >> (8 * (7 - i))));
    return out;
}

GcHeartbeat decodeGcHeartbeat(std::string_view data)
{
    if (data.size() != 24)
        throw Exception(ErrorCodes::CORRUPTED_DATA, "CAS gc heartbeat: expected 24 bytes, got {}", data.size());
    GcHeartbeat hb;
    for (int i = 0; i < 16; ++i)
        hb.owner = (hb.owner << 8) | static_cast<UInt8>(data[i]);
    for (int i = 0; i < 8; ++i)
        hb.hb_seq = (hb.hb_seq << 8) | static_cast<UInt8>(static_cast<unsigned char>(data[16 + i]));
    return hb;
}

}
