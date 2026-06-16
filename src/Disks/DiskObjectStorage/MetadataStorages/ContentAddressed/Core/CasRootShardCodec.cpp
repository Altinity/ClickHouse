#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasRootShardCodec.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasCodecUtil.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasIds.h>
/// Included by basename via clickhouse_cas_proto's SYSTEM include dir so the generated header's
/// reserved identifiers don't trip -Weverything -Werror (same idiom as clickhouse_grpc_protos).
#include <cas_root_shard.pb.h>
#include <IO/WriteBufferFromString.h>
#include <Common/Exception.h>
#include <google/protobuf/io/coded_stream.h>
#include <google/protobuf/io/zero_copy_stream_impl_lite.h>

namespace DB
{
namespace ErrorCodes
{
    extern const int CORRUPTED_DATA;
    extern const int NOT_IMPLEMENTED;
}
}

namespace DB::Cas
{

namespace
{

constexpr uint64_t ROOT_VERSION = 1;             /// the JSON format is the implicit codec v1
constexpr uint32_t CODEC_VERSION_PROTOBUF = 2;   /// the protobuf format

/// UInt128 <-> 16 raw bytes, big-endian (mirrors the u128ToHex byte order).
std::string u128ToBytes(const UInt128 & v)
{
    std::string out(16, '\0');
    for (int i = 0; i < 16; ++i)
        out[i] = static_cast<char>(static_cast<UInt8>(v >> (8 * (15 - i))));
    return out;
}

UInt128 u128FromBytes(const std::string & b, std::string_view what)
{
    if (b.size() != 16)
        throw Exception(ErrorCodes::CORRUPTED_DATA, "CAS {}: tree_id must be 16 bytes, got {}", what, b.size());
    UInt128 v = 0;
    for (int i = 0; i < 16; ++i)
        v = (v << 8) | static_cast<UInt8>(b[i]);
    return v;
}

Cas::Proto::JournalOp journalOpToProto(JournalRecord::Op op)
{
    switch (op)
    {
        case JournalRecord::Op::Add: return Cas::Proto::JOURNAL_OP_ADD;
        case JournalRecord::Op::Remove: return Cas::Proto::JOURNAL_OP_REMOVE;
    }
    throw Exception(ErrorCodes::CORRUPTED_DATA, "CAS root shard: invalid journal op {}", static_cast<int>(op));
}

JournalRecord::Op journalOpFromProto(Cas::Proto::JournalOp op, std::string_view what)
{
    switch (op)
    {
        case Cas::Proto::JOURNAL_OP_ADD: return JournalRecord::Op::Add;
        case Cas::Proto::JOURNAL_OP_REMOVE: return JournalRecord::Op::Remove;
        default: throw Exception(ErrorCodes::CORRUPTED_DATA, "CAS {}: invalid journal op {}", what, static_cast<int>(op));
    }
}

/// `JournalRecord::Op` <- string. Unknown string on decode is corruption (fail closed). Used by the
/// legacy JSON decoder only (the encoder is now protobuf).
JournalRecord::Op journalOpFromString(std::string_view s, std::string_view what)
{
    if (s == "add")
        return JournalRecord::Op::Add;
    if (s == "remove")
        return JournalRecord::Op::Remove;
    throw Exception(ErrorCodes::CORRUPTED_DATA, "CAS {}: invalid journal op '{}'", what, s);
}

}

String encodeRootShard(const RootShard & root)
{
    Cas::Proto::RootShardManifest msg;
    msg.set_codec_version(CODEC_VERSION_PROTOBUF);
    msg.set_shard_version(root.shard_version);
    msg.set_fence_round(root.fence_round);

    auto & refs = *msg.mutable_refs();
    for (const auto & [name, payload] : root.refs)
    {
        Cas::Proto::RefPayload p;
        p.set_tree_id(u128ToBytes(payload.tree_id));
        p.set_tree_size(payload.tree_size);
        auto & mf = *p.mutable_mutable_files();
        for (const auto & [k, v] : payload.mutable_files)
            mf[k] = v;
        refs[name] = std::move(p);
    }

    for (const auto & rec : root.journal)
    {
        auto * r = msg.add_journal();
        r->set_op(journalOpToProto(rec.op));
        r->set_ref_name(rec.ref_name);
        r->set_tree_id(u128ToBytes(rec.tree_id));
        r->set_at_version(rec.at_version);
    }

    /// Deterministic serialization (sorts map<> entries) so golden tests are stable. Correctness
    /// does not need it - the manifest is CAS-by-token, not content-addressed.
    std::string out;
    {
        google::protobuf::io::StringOutputStream zos(&out);
        google::protobuf::io::CodedOutputStream cos(&zos);
        cos.SetSerializationDeterministic(true);
        if (!msg.SerializeToCodedStream(&cos))
            throw Exception(ErrorCodes::CORRUPTED_DATA, "CAS root shard: protobuf serialize failed");
    }
    return out;
}

static RootShard decodeRootShardProto(std::string_view data)
{
    Cas::Proto::RootShardManifest msg;
    if (!msg.ParseFromArray(data.data(), static_cast<int>(data.size())))
        throw Exception(ErrorCodes::CORRUPTED_DATA, "CAS root shard: protobuf parse failed");
    if (msg.codec_version() > CODEC_VERSION_PROTOBUF)
        throw Exception(ErrorCodes::NOT_IMPLEMENTED,
            "CAS root shard: codec_version {} is from a newer writer", msg.codec_version());

    RootShard root;
    root.shard_version = msg.shard_version();
    root.fence_round = msg.fence_round();

    for (const auto & [name, p] : msg.refs())
    {
        RefPayload payload;
        payload.tree_id = u128FromBytes(p.tree_id(), "root shard ref");
        payload.tree_size = p.tree_size();
        for (const auto & [k, v] : p.mutable_files())
            payload.mutable_files[k] = v;
        root.refs[name] = std::move(payload);
    }

    for (const auto & r : msg.journal())
    {
        JournalRecord rec;
        rec.op = journalOpFromProto(r.op(), "root shard journal");
        rec.ref_name = r.ref_name();
        rec.tree_id = u128FromBytes(r.tree_id(), "root shard journal");
        rec.at_version = r.at_version();

        /// Same semantic invariants the JSON decoder enforces (corruption -> fail closed).
        if (rec.at_version > root.shard_version)
            throw Exception(ErrorCodes::CORRUPTED_DATA,
                "CAS root shard: journal at_version {} exceeds shard_version {}",
                rec.at_version, root.shard_version);
        if (!root.journal.empty() && rec.at_version < root.journal.back().at_version)
            throw Exception(ErrorCodes::CORRUPTED_DATA,
                "CAS root shard: journal at_version {} after {} - the journal must be non-decreasing",
                rec.at_version, root.journal.back().at_version);

        root.journal.push_back(std::move(rec));
    }
    return root;
}

static RootShard decodeRootShardJson(std::string_view data)
{
    return decodeJsonGuarded("root shard", [&]
    {
        auto obj = parseJsonDocument(data, "cas_root_shard", ROOT_VERSION, "root shard");
        checkNoUnknownKeys(*obj, {"format", "version", "shard_version", "fence_round", "refs", "journal"}, "root shard");

        RootShard root;
        root.shard_version = requireU64(*obj, "shard_version", "root shard");
        root.fence_round = requireU64(*obj, "fence_round", "root shard");

        auto refs = requireObject(*obj, "refs", "root shard");
        for (const auto & [name, value] : *refs)
        {
            if (value.type() != typeid(Poco::JSON::Object::Ptr))
                throw Exception(ErrorCodes::CORRUPTED_DATA, "CAS root shard: ref '{}' must be an object", name);
            auto ref_obj = value.extract<Poco::JSON::Object::Ptr>();
            checkNoUnknownKeys(*ref_obj, {"tree", "tree_size", "mutable_files"}, "root shard ref");

            RefPayload payload;
            payload.tree_id = requireHash(*ref_obj, "tree", "root shard ref");
            payload.tree_size = requireU64(*ref_obj, "tree_size", "root shard ref");
            payload.mutable_files = requireStringMap(*ref_obj, "mutable_files", "root shard ref");
            root.refs[name] = std::move(payload);
        }

        auto journal = requireArray(*obj, "journal", "root shard");
        for (size_t i = 0; i < journal->size(); ++i)
        {
            auto rec_obj = requireObjectAt(*journal, i, "root shard journal");
            checkNoUnknownKeys(*rec_obj, {"op", "ref", "tree", "at_version"}, "root shard journal record");

            JournalRecord rec;
            rec.op = journalOpFromString(requireString(*rec_obj, "op", "root shard journal"), "root shard journal");
            rec.ref_name = requireString(*rec_obj, "ref", "root shard journal");
            rec.tree_id = requireHash(*rec_obj, "tree", "root shard journal");
            rec.at_version = requireU64(*rec_obj, "at_version", "root shard journal");

            /// The GC fold replays the journal in order under a cursor bound: an out-of-order
            /// at_version would fold silently in vector order (a corruption-induced UNDER-count =
            /// a wrong delete later), and a record beyond shard_version would be silently skipped
            /// by the cursor window. Both are corruption - fail closed. NON-DECREASING, not
            /// strict: dropNamespace legally appends N Removes at the same committed version.
            if (rec.at_version > root.shard_version)
                throw Exception(ErrorCodes::CORRUPTED_DATA,
                    "CAS root shard: journal at_version {} exceeds shard_version {}",
                    rec.at_version, root.shard_version);
            if (!root.journal.empty() && rec.at_version < root.journal.back().at_version)
                throw Exception(ErrorCodes::CORRUPTED_DATA,
                    "CAS root shard: journal at_version {} after {} - the journal must be non-decreasing",
                    rec.at_version, root.journal.back().at_version);

            root.journal.push_back(std::move(rec));
        }

        return root;
    });
}

RootShard decodeRootShard(std::string_view data)
{
    if (data.empty())
        throw Exception(ErrorCodes::CORRUPTED_DATA, "CAS root shard: empty manifest");
    /// Dispatch: a legacy JSON manifest starts with '{' (0x7B); a protobuf RootShardManifest starts
    /// with field-1's tag byte 0x08 (varint codec_version). They never collide.
    if (data.front() == '{')
        return decodeRootShardJson(data);
    return decodeRootShardProto(data);
}

}
