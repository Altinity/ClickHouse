#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasRootShardCodec.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasCodecUtil.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasFormat.h>
/// Included by basename via clickhouse_cas_proto's SYSTEM include dir so the generated header's
/// reserved identifiers don't trip -Weverything -Werror (same idiom as clickhouse_grpc_protos).
#include <cas_root_shard.pb.h>
#include <Common/Exception.h>
#include <IO/ReadBufferFromMemory.h>
#include <IO/WriteBufferFromString.h>
#include <IO/WriteHelpers.h>
#include <google/protobuf/io/coded_stream.h>
#include <google/protobuf/io/zero_copy_stream_impl_lite.h>
#include <cstdint>

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

/// The CARS (CA Root Shard) framing magic: prepended before the protobuf body so the framing header
/// (version + fail-closed gating) is read BEFORE the body is parsed.
constexpr std::string_view MANIFEST_MAGIC = "CARS";

/// UInt128 <-> 16 raw bytes, big-endian: routed through the named, FROZEN-byte-order helpers
/// `u128ToBytesBE` / `u128FromBytesBE` in `CasCodecUtil.h` (the bodies were moved there verbatim).

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
        case static_cast<uint32_t>(Placement::Subtree):   return Placement::Subtree;
        default:
            throw Exception(ErrorCodes::CORRUPTED_DATA,
                "CAS {}: unknown placement value {} in closure entry", what, v);
    }
}

Cas::Proto::ClosureNodeProto encodeClosureNode(const ClosureNode & node)
{
    Cas::Proto::ClosureNodeProto pn;
    pn.set_tree_hash(u128ToBytesBE(node.tree_hash));
    for (const auto & e : node.entries)
    {
        auto * pe = pn.add_entries();
        pe->set_placement(placementToProto(e.placement));
        pe->set_file_hash(u128ToBytesBE(e.file_hash));
        pe->set_file_size(e.file_size);
    }
    return pn;
}

ClosureNode decodeClosureNode(const Cas::Proto::ClosureNodeProto & pn)
{
    ClosureNode node;
    node.tree_hash = u128FromBytesBE(pn.tree_hash(), "closure node tree_hash");
    node.entries.reserve(static_cast<size_t>(pn.entries_size()));
    for (const auto & pe : pn.entries())
    {
        TreeEntry e;
        e.placement = placementFromProto(pe.placement(), "closure node entry");
        e.file_hash = u128FromBytesBE(pe.file_hash(), "closure node entry file_hash");
        e.file_size = pe.file_size();
        node.entries.push_back(std::move(e));
    }
    return node;
}

}

String encodeRootShard(const RootShard & root)
{
    Cas::Proto::RootShardManifest msg;
    msg.set_shard_version(root.shard_version);
    msg.set_fence_round(root.fence_round);

    auto & refs = *msg.mutable_refs();
    for (const auto & [name, payload] : root.refs)
    {
        Cas::Proto::RefPayload p;
        p.set_tree_id(u128ToBytesBE(payload.tree_id));
        p.set_tree_size(payload.tree_size);
        auto & mf = *p.mutable_mutable_files();
        for (const auto & [k, v] : payload.mutable_files)
            mf[k] = v;
        p.set_published_at_ms(payload.published_at_ms);
        refs[name] = std::move(p);
    }

    for (const auto & rec : root.journal)
    {
        auto * r = msg.add_journal();
        r->set_op(journalOpToProto(rec.op));
        r->set_ref_name(rec.ref_name);
        r->set_tree_id(u128ToBytesBE(rec.tree_id));
        r->set_at_version(rec.at_version);
        for (const auto & node : rec.closure)
            *r->add_closure() = encodeClosureNode(node);
    }

    /// Deterministic serialization (sorts map<> entries) so golden tests are stable. Correctness
    /// does not need it - the manifest is CAS-by-token, not content-addressed.
    std::string body;
    {
        google::protobuf::io::StringOutputStream zos(&body);
        google::protobuf::io::CodedOutputStream cos(&zos);
        cos.SetSerializationDeterministic(true);
        if (!msg.SerializeToCodedStream(&cos))
            throw Exception(ErrorCodes::LOGICAL_ERROR, "CAS root shard: protobuf serialization failed");
    }

    /// Prepend the framing header so the version is checked BEFORE the body is parsed.
    WriteBufferFromOwnString out;
    Cas::writeFramingHeader(out, MANIFEST_MAGIC, Cas::currentWriterVersion(Cas::FormatId::Manifest));
    writeString(body, out);
    return std::move(out.str());
}

RootShard decodeRootShard(std::string_view data)
{
    if (data.empty())
        throw Exception(ErrorCodes::CORRUPTED_DATA, "CAS root shard: empty manifest");

    /// Read and gate the framing header BEFORE parsing the protobuf body: validates magic and
    /// gateOnRead(min_reader_version) so a future object from a newer writer fails closed.
    ReadBufferFromMemory in(data.data(), data.size());
    Cas::readFramingHeader(in, MANIFEST_MAGIC, "root shard");
    const std::string_view body = data.substr(Cas::FRAMING_HEADER_SIZE);

    Cas::Proto::RootShardManifest msg;
    if (!msg.ParseFromArray(body.data(), static_cast<int>(body.size())))
        throw Exception(ErrorCodes::CORRUPTED_DATA, "CAS root shard: protobuf parse failed");

    RootShard root;
    root.shard_version = msg.shard_version();
    root.fence_round = msg.fence_round();

    for (const auto & [name, p] : msg.refs())
    {
        RefPayload payload;
        payload.tree_id = u128FromBytesBE(p.tree_id(), "root shard ref");
        payload.tree_size = p.tree_size();
        for (const auto & [k, v] : p.mutable_files())
            payload.mutable_files[k] = v;
        payload.published_at_ms = p.published_at_ms();
        root.refs[name] = std::move(payload);
    }

    for (const auto & r : msg.journal())
    {
        JournalRecord rec;
        rec.op = journalOpFromProto(r.op(), "root shard journal");
        rec.ref_name = r.ref_name();
        rec.tree_id = u128FromBytesBE(r.tree_id(), "root shard journal");
        rec.at_version = r.at_version();
        rec.closure.reserve(static_cast<size_t>(r.closure_size()));
        for (const auto & pn : r.closure())
            rec.closure.push_back(decodeClosureNode(pn));

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
