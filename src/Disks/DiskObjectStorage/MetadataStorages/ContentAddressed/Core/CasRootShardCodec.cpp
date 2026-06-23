#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasRootShardCodec.h>
/// Included by basename via clickhouse_cas_proto's SYSTEM include dir so the generated header's
/// reserved identifiers don't trip -Weverything -Werror (same idiom as clickhouse_grpc_protos).
#include <cas_root_shard.pb.h>
#include <Common/Exception.h>
#include <google/protobuf/io/coded_stream.h>
#include <google/protobuf/io/zero_copy_stream_impl_lite.h>
#include <cstdint>

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

/// The only on-disk manifest format is protobuf (B164a). There is no JSON back-compat: a fresh pool
/// is always protobuf, and `codec_version` is the fail-closed gate for future breaking changes (see
/// the schema-evolution rules in cas_root_shard.proto).
constexpr uint32_t CODEC_VERSION = 1;

/// UInt128 <-> 16 raw bytes, big-endian.
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

uint32_t placementToProto(Placement p)
{
    return static_cast<uint32_t>(p);
}

Placement placementFromProto(uint32_t v, std::string_view what)
{
    switch (v)
    {
        case static_cast<uint32_t>(Placement::Inline):    return Placement::Inline;
        case static_cast<uint32_t>(Placement::Blob):      return Placement::Blob;
        case static_cast<uint32_t>(Placement::PackSlice): return Placement::PackSlice;
        case static_cast<uint32_t>(Placement::Subtree):   return Placement::Subtree;
        default:
            throw Exception(ErrorCodes::CORRUPTED_DATA,
                "CAS {}: unknown placement value {} in closure entry", what, v);
    }
}

Cas::Proto::ClosureNodeProto encodeClosureNode(const ClosureNode & node)
{
    Cas::Proto::ClosureNodeProto pn;
    pn.set_tree_hash(u128ToBytes(node.tree_hash));
    for (const auto & e : node.entries)
    {
        auto * pe = pn.add_entries();
        pe->set_placement(placementToProto(e.placement));
        pe->set_file_hash(u128ToBytes(e.file_hash));
        pe->set_file_size(e.file_size);
        if (e.placement == Placement::PackSlice)
            pe->set_pack_hash(u128ToBytes(e.pack_hash));
    }
    return pn;
}

ClosureNode decodeClosureNode(const Cas::Proto::ClosureNodeProto & pn)
{
    ClosureNode node;
    node.tree_hash = u128FromBytes(pn.tree_hash(), "closure node tree_hash");
    node.entries.reserve(static_cast<size_t>(pn.entries_size()));
    for (const auto & pe : pn.entries())
    {
        TreeEntry e;
        e.placement = placementFromProto(pe.placement(), "closure node entry");
        e.file_hash = u128FromBytes(pe.file_hash(), "closure node entry file_hash");
        e.file_size = pe.file_size();
        /// Symmetric with encode (which always writes pack_hash for PackSlice): decode it
        /// unconditionally so u128FromBytes fails closed (CORRUPTED_DATA) on an absent/short
        /// field, rather than silently zero-filling and losing a GC pack-edge.
        if (e.placement == Placement::PackSlice)
            e.pack_hash = u128FromBytes(pe.pack_hash(), "closure node entry pack_hash");
        node.entries.push_back(std::move(e));
    }
    return node;
}

}

String encodeRootShard(const RootShard & root)
{
    Cas::Proto::RootShardManifest msg;
    msg.set_codec_version(CODEC_VERSION);
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
        for (const auto & node : payload.closure)
            *p.add_closure() = encodeClosureNode(node);
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

RootShard decodeRootShard(std::string_view data)
{
    if (data.empty())
        throw Exception(ErrorCodes::CORRUPTED_DATA, "CAS root shard: empty manifest");

    Cas::Proto::RootShardManifest msg;
    if (!msg.ParseFromArray(data.data(), static_cast<int>(data.size())))
        throw Exception(ErrorCodes::CORRUPTED_DATA, "CAS root shard: protobuf parse failed");
    /// A valid encoder always sets codec_version >= 1; a zero value means the bytes are not a
    /// conforming manifest (e.g. random bytes that happened to protobuf-parse). Fail closed.
    if (msg.codec_version() == 0)
        throw Exception(ErrorCodes::CORRUPTED_DATA, "CAS root shard: missing or zero codec_version");
    if (msg.codec_version() > CODEC_VERSION)
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
        payload.closure.reserve(static_cast<size_t>(p.closure_size()));
        for (const auto & pn : p.closure())
            payload.closure.push_back(decodeClosureNode(pn));
        root.refs[name] = std::move(payload);
    }

    for (const auto & r : msg.journal())
    {
        JournalRecord rec;
        rec.op = journalOpFromProto(r.op(), "root shard journal");
        rec.ref_name = r.ref_name();
        rec.tree_id = u128FromBytes(r.tree_id(), "root shard journal");
        rec.at_version = r.at_version();

        /// The GC fold replays the journal in order under a cursor bound: an out-of-order at_version
        /// would fold silently in vector order (a corruption-induced UNDER-count = a wrong delete
        /// later), and a record beyond shard_version would be silently skipped by the cursor window.
        /// Both are corruption - fail closed. NON-DECREASING, not strict: dropNamespace legally
        /// appends N Removes at the same committed version.
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

}
