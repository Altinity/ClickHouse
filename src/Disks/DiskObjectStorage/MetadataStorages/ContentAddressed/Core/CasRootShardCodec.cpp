#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasRootShardCodec.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasCodecUtil.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasFormat.h>
/// Included by basename via clickhouse_cas_proto's SYSTEM include dir so the generated header's
/// reserved identifiers don't trip -Weverything -Werror (same idiom as clickhouse_grpc_protos).
#include <cas_format.pb.h>
#include <Common/Exception.h>
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

namespace Proto = ::clickhouse::cas::format;

namespace
{

Cas::Proto::OwnerKindProto ownerKindToProto(OwnerKind kind)
{
    switch (kind)
    {
        case OwnerKind::Committed: return Cas::Proto::OWNER_KIND_COMMITTED;
        case OwnerKind::Precommit: return Cas::Proto::OWNER_KIND_PRECOMMIT;
    }
    throw Exception(ErrorCodes::CORRUPTED_DATA, "CAS root shard: invalid owner kind {}", static_cast<int>(kind));
}

OwnerKind ownerKindFromProto(Cas::Proto::OwnerKindProto kind, std::string_view what)
{
    switch (kind)
    {
        case Cas::Proto::OWNER_KIND_COMMITTED: return OwnerKind::Committed;
        case Cas::Proto::OWNER_KIND_PRECOMMIT: return OwnerKind::Precommit;
        default:
            throw Exception(ErrorCodes::CORRUPTED_DATA, "CAS {}: invalid owner kind {}", what, static_cast<int>(kind));
    }
}

void encodeManifestRef(Cas::Proto::ManifestRefProto * p, const ManifestRef & ref)
{
    p->set_writer_epoch(ref.writer_epoch);
    p->set_build_sequence(ref.build_sequence);
    p->set_manifest_ordinal(ref.manifest_ordinal);
}

ManifestRef decodeManifestRef(const Cas::Proto::ManifestRefProto & p, std::string_view what)
{
    ManifestRef ref;
    ref.writer_epoch = p.writer_epoch();
    ref.build_sequence = p.build_sequence();
    ref.manifest_ordinal = p.manifest_ordinal();
    if (ref.manifest_ordinal == 0 || ref.manifest_ordinal > kMaxManifestOrdinal)
        throw Exception(ErrorCodes::CORRUPTED_DATA,
            "CAS {}: manifest ordinal {} out of range", what, ref.manifest_ordinal);
    return ref;
}

void encodeOwnerBinding(Cas::Proto::OwnerBindingProto * p, const OwnerBinding & b)
{
    p->set_owner_kind(ownerKindToProto(b.owner_kind));
    p->set_ref_name(b.ref_name);
    p->set_build_id(u128ToBytesBE(b.build_id));
    encodeManifestRef(p->mutable_manifest_ref(), b.manifest_ref);
}

OwnerBinding decodeOwnerBinding(const Cas::Proto::OwnerBindingProto & p, std::string_view what)
{
    OwnerBinding b;
    b.owner_kind = ownerKindFromProto(p.owner_kind(), what);
    b.ref_name = p.ref_name();
    b.build_id = u128FromBytesBE(p.build_id(), what);
    b.manifest_ref = decodeManifestRef(p.manifest_ref(), what);

    /// Fail-closed decode: enforce the owner_kind<->build_id invariant the proto only documents
    /// (OwnerBindingProto: "Committed: build_id = 0. Precommit: build_id set."). A binding that violates
    /// it is corruption — a Committed owner with a build_id, or a Precommit owner without one, would
    /// mis-drive the fold's precommit-vs-committed dispatch (the barrier / reclaim decisions).
    if (b.owner_kind == OwnerKind::Committed && b.build_id != UInt128(0))
        throw Exception(ErrorCodes::CORRUPTED_DATA,
            "CAS {}: committed owner binding carries a non-zero build_id (owner_kind<->build_id invariant)", what);
    if (b.owner_kind == OwnerKind::Precommit && b.build_id == UInt128(0))
        throw Exception(ErrorCodes::CORRUPTED_DATA,
            "CAS {}: precommit owner binding has a zero build_id (owner_kind<->build_id invariant)", what);
    return b;
}

}

String encodeRootShard(const RootShard & root)
{
    Cas::Proto::RootShardManifest msg;

    /// Set CasHeader as field 1 (pure protobuf — no binary prefix).
    auto * hdr = msg.mutable_header();
    hdr->set_magic(magicFor(FormatId::Manifest));
    hdr->set_writer_version(currentWriterVersion());
    hdr->set_compatibility_version(currentCompatibilityVersion());

    msg.set_shard_version(root.shard_version);
    msg.set_fence_round(root.fence_round);
    msg.set_incarnation_writer_epoch(root.incarnation.writer_epoch);
    msg.set_incarnation_build_sequence(root.incarnation.build_sequence);

    auto & refs = *msg.mutable_refs();
    for (const auto & [name, rr] : root.refs)
    {
        Cas::Proto::RootRefProto p;
        p.set_ref_name(rr.ref_name);
        encodeManifestRef(p.mutable_manifest_ref(), rr.manifest_ref);
        auto & mf = *p.mutable_mutable_files();
        for (const auto & [k, v] : rr.mutable_files)
            mf[k] = v;
        p.set_published_at_ms(rr.published_at_ms);
        refs[name] = std::move(p);
    }

    for (const auto & ev : root.journal)
    {
        auto * e = msg.add_journal();
        e->set_transition_version(ev.transition_version);
        if (ev.old_binding)
            encodeOwnerBinding(e->mutable_old_binding(), *ev.old_binding);
        if (ev.new_binding)
            encodeOwnerBinding(e->mutable_new_binding(), *ev.new_binding);
    }

    /// Deterministic serialization (sorts map<> entries) so golden tests are stable. Correctness
    /// does not need it - the manifest is CAS-by-token, not content-addressed.
    std::string out;
    {
        google::protobuf::io::StringOutputStream zos(&out);
        google::protobuf::io::CodedOutputStream cos(&zos);
        cos.SetSerializationDeterministic(true);
        if (!msg.SerializeToCodedStream(&cos))
            throw Exception(ErrorCodes::LOGICAL_ERROR, "CAS root shard: protobuf serialization failed");
    }
    return out;
}

RootShard decodeRootShard(std::string_view data)
{
    if (data.empty())
        throw Exception(ErrorCodes::CORRUPTED_DATA, "CAS root shard: empty manifest");

    /// Parse the whole message directly (pure protobuf, no binary prefix).
    Cas::Proto::RootShardManifest msg;
    if (!msg.ParseFromArray(data.data(), static_cast<int>(data.size())))
        throw Exception(ErrorCodes::CORRUPTED_DATA, "CAS root shard: protobuf parse failed");

    /// Check magic then compatibility_version BEFORE reading any other fields.
    if (msg.header().magic() != magicFor(FormatId::Manifest))
        throw Exception(ErrorCodes::CORRUPTED_DATA,
            "CAS root shard: bad magic (got 0x{:08x}, expected 0x{:08x})",
            msg.header().magic(), magicFor(FormatId::Manifest));
    checkCompatibility(msg.header().compatibility_version(), "root shard");

    RootShard root;
    root.shard_version = msg.shard_version();
    root.fence_round = msg.fence_round();
    root.incarnation.writer_epoch = msg.incarnation_writer_epoch();
    root.incarnation.build_sequence = msg.incarnation_build_sequence();

    for (const auto & [name, p] : msg.refs())
    {
        RootRef rr;
        rr.ref_name = p.ref_name();
        rr.manifest_ref = decodeManifestRef(p.manifest_ref(), "root shard ref manifest_ref");
        for (const auto & [k, v] : p.mutable_files())
            rr.mutable_files[k] = v;
        rr.published_at_ms = p.published_at_ms();
        root.refs[name] = std::move(rr);
    }

    for (const auto & e : msg.journal())
    {
        RootOwnerEvent ev;
        ev.transition_version = e.transition_version();
        if (e.has_old_binding())
            ev.old_binding = decodeOwnerBinding(e.old_binding(), "root shard journal old_binding");
        if (e.has_new_binding())
            ev.new_binding = decodeOwnerBinding(e.new_binding(), "root shard journal new_binding");

        /// A RootOwnerEvent must remove or add at least one binding (spec §Root Journal Format): an
        /// event with neither is meaningless and would fold to a no-op — fail closed.
        if (!ev.old_binding && !ev.new_binding)
            throw Exception(ErrorCodes::CORRUPTED_DATA,
                "CAS root shard: journal event at transition_version {} has neither old_binding nor new_binding",
                ev.transition_version);

        /// The GC fold replays the journal in transition_version order under a cursor bound: an
        /// out-of-order or beyond-shard_version transition_version would fold silently in vector order
        /// (a corruption-induced mis-count = a wrong delete later). NON-DECREASING, not strict.
        if (ev.transition_version > root.shard_version)
            throw Exception(ErrorCodes::CORRUPTED_DATA,
                "CAS root shard: journal transition_version {} exceeds shard_version {}",
                ev.transition_version, root.shard_version);
        if (!root.journal.empty() && ev.transition_version < root.journal.back().transition_version)
            throw Exception(ErrorCodes::CORRUPTED_DATA,
                "CAS root shard: journal transition_version {} after {} - the journal must be non-decreasing",
                ev.transition_version, root.journal.back().transition_version);

        root.journal.push_back(std::move(ev));
    }
    return root;
}

}
