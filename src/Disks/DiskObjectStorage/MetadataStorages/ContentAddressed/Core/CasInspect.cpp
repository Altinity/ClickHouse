#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasInspect.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasBlobMeta.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasEnvelope.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasGcFormats.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasGenerationSeal.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasIds.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasManifestCodec.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasRootShardCodec.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasServerRoot.h>
#include <Common/Exception.h>
#include <fmt/format.h>
#include <cstdint>
#include <vector>

namespace DB::Cas
{

namespace
{

/// Escapes `s` as a JSON string LITERAL (including the surrounding quotes). Handles the standard
/// two-char escapes plus a `\uXXXX` fallback for any other control byte; everything else (including
/// raw multi-byte UTF-8) passes through unchanged. This is a debug/inspection rendering, not a wire
/// format, so it deliberately does not attempt full Unicode validation.
String jsonEscape(std::string_view s)
{
    String out;
    out.reserve(s.size() + 2);
    out += '"';
    for (unsigned char c : s)
    {
        switch (c)
        {
            case '"':  out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\b': out += "\\b"; break;
            case '\f': out += "\\f"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if (c < 0x20)
                    out += fmt::format("\\u{:04x}", c);
                else
                    out += static_cast<char>(c);
        }
    }
    out += '"';
    return out;
}

/// u128 fields (hashes, ids, tokens-as-u128) render as a lowercase-hex JSON string, matching
/// `u128ToHex` — never as a nested {high,low} object or a decimal number.
String jsonHex(const UInt128 & v) { return jsonEscape(u128ToHex(v)); }
String jsonUInt(uint64_t v) { return std::to_string(v); }
String jsonBool(bool b) { return b ? "true" : "false"; }

/// A minimal JSON object builder: each `add` takes a key and an already-rendered JSON fragment
/// (a quoted string, a number, `true`/`false`/`null`, or a nested `{...}`/`[...]`) and joins them
/// with commas. No pretty-printing — this is a debug/inspection tool, not a wire format.
class JsonObj
{
public:
    JsonObj & add(std::string_view key, const String & raw_value)
    {
        if (!first)
            out += ",";
        first = false;
        out += jsonEscape(key);
        out += ":";
        out += raw_value;
        return *this;
    }

    String str() const { return "{" + out + "}"; }

private:
    String out;
    bool first = true;
};

String jsonArray(const std::vector<String> & items)
{
    String out = "[";
    for (size_t i = 0; i < items.size(); ++i)
    {
        if (i)
            out += ",";
        out += items[i];
    }
    out += "]";
    return out;
}

String renderManifestRef(const ManifestRef & r)
{
    return JsonObj()
        .add("writer_epoch", jsonUInt(r.writer_epoch))
        .add("build_sequence", jsonUInt(r.build_sequence))
        .add("manifest_ordinal", jsonUInt(r.manifest_ordinal))
        .str();
}

String renderShardIncarnation(const ShardIncarnation & i)
{
    return JsonObj()
        .add("writer_epoch", jsonUInt(i.writer_epoch))
        .add("build_sequence", jsonUInt(i.build_sequence))
        .str();
}

String ownerKindName(OwnerKind k)
{
    switch (k)
    {
        case OwnerKind::Committed: return "Committed";
        case OwnerKind::Precommit: return "Precommit";
    }
    return "Unknown";
}

String renderOwnerBinding(const OwnerBinding & b)
{
    return JsonObj()
        .add("owner_kind", jsonEscape(ownerKindName(b.owner_kind)))
        .add("ref_name", jsonEscape(b.ref_name))
        .add("build_id", jsonHex(b.build_id))
        .add("manifest_ref", renderManifestRef(b.manifest_ref))
        .str();
}

String renderRootOwnerEvent(const RootOwnerEvent & e)
{
    return JsonObj()
        .add("transition_version", jsonUInt(e.transition_version))
        .add("old_binding", e.old_binding ? renderOwnerBinding(*e.old_binding) : "null")
        .add("new_binding", e.new_binding ? renderOwnerBinding(*e.new_binding) : "null")
        .add("is_tombstone", jsonBool(e.is_tombstone))
        .str();
}

String renderRootRef(const RootRef & r)
{
    JsonObj files;
    for (const auto & [k, v] : r.mutable_files)
        files.add(k, jsonEscape(v));

    return JsonObj()
        .add("ref_name", jsonEscape(r.ref_name))
        .add("manifest_ref", renderManifestRef(r.manifest_ref))
        .add("mutable_files", files.str())
        .add("published_at_ms", jsonUInt(r.published_at_ms))
        .str();
}

String renderRootShard(const RootShard & root)
{
    JsonObj refs;
    for (const auto & [name, ref] : root.refs)
        refs.add(name, renderRootRef(ref));

    std::vector<String> journal;
    journal.reserve(root.journal.size());
    for (const auto & e : root.journal)
        journal.push_back(renderRootOwnerEvent(e));

    return JsonObj()
        .add("shard_version", jsonUInt(root.shard_version))
        .add("fence_round", jsonUInt(root.fence_round))
        .add("incarnation", renderShardIncarnation(root.incarnation))
        .add("refs", refs.str())
        .add("journal", jsonArray(journal))
        .str();
}

String placementName(EntryPlacement p)
{
    switch (p)
    {
        case EntryPlacement::Inline: return "Inline";
        case EntryPlacement::Blob: return "Blob";
    }
    return "Unknown";
}

/// `inline_bytes` renders as its LENGTH only, not its content — an inline file's bytes are payload
/// data, not part-manifest identity, and may be arbitrarily large / non-UTF8.
String renderManifestEntry(const ManifestEntry & e)
{
    return JsonObj()
        .add("path", jsonEscape(e.path))
        .add("placement", jsonEscape(placementName(e.placement)))
        .add("blob_hash", jsonHex(e.blob_hash))
        .add("blob_size", jsonUInt(e.blob_size))
        .add("inline_bytes_size", jsonUInt(e.inline_bytes.size()))
        .str();
}

String renderPartManifest(const PartManifest & m)
{
    std::vector<String> entries;
    entries.reserve(m.entries.size());
    for (const auto & e : m.entries)
        entries.push_back(renderManifestEntry(e));

    return JsonObj()
        .add("ref", renderManifestRef(m.ref))
        .add("root_namespace_id", jsonEscape(m.root_namespace_id.string()))
        .add("payload_digest", jsonHex(m.payload_digest))
        .add("entries", jsonArray(entries))
        .str();
}

String renderMountLease(const MountLease & m)
{
    return JsonObj()
        .add("server_uuid", jsonHex(m.server_uuid))
        .add("writer_epoch", jsonUInt(m.writer_epoch))
        .add("hostname", jsonEscape(m.hostname))
        .add("pid", jsonUInt(m.pid))
        .add("started_at_ms", jsonUInt(m.started_at_ms))
        .add("seq", jsonUInt(m.seq))
        .add("expires_at_ms", jsonUInt(m.expires_at_ms))
        .add("min_active", jsonUInt(m.min_active))
        .add("gc_fenced", jsonBool(m.gc_fenced))
        .str();
}

String renderGcLease(const GcLease & l)
{
    return JsonObj()
        .add("owner", jsonHex(l.owner))
        .add("seq", jsonUInt(l.seq))
        .str();
}

String renderGcState(const GcState & s)
{
    JsonObj retired_refs;
    for (const auto & [shard, key] : s.retired_refs)
        retired_refs.add(std::to_string(shard), jsonEscape(key));

    return JsonObj()
        .add("round", jsonUInt(s.round))
        .add("fence_seq", jsonUInt(s.fence_seq))
        .add("gc_shards", jsonUInt(s.gc_shards))
        .add("snap_generation", jsonUInt(s.snap_generation))
        .add("snap_pruned_through", jsonUInt(s.snap_pruned_through))
        .add("snap_attempt", jsonUInt(s.snap_attempt))
        .add("manifest_sweep_cursor", jsonEscape(s.manifest_sweep_cursor))
        .add("lease", renderGcLease(s.lease))
        .add("retired_refs", retired_refs.str())
        .str();
}

String tokenTypeName(TokenType t)
{
    switch (t)
    {
        case TokenType::ETag:       return "ETag";
        case TokenType::Generation: return "Generation";
        case TokenType::Emulated:   return "Emulated";
    }
    return "Unknown";
}

/// `Token::value` is an opaque backend-native string (e.g. an S3 ETag) — NOT a 128-bit hash — so it
/// renders verbatim (escaped), not hex-converted; `type` names which backend family minted it.
String renderToken(const Token & t)
{
    return JsonObj()
        .add("value", jsonEscape(t.value))
        .add("type", jsonEscape(tokenTypeName(t.type)))
        .str();
}

String objectKindName(ObjectKind k)
{
    switch (k)
    {
        case ObjectKind::Blob: return "Blob";
    }
    return "Unknown";
}

String renderRunRef(const RunRef & r)
{
    return JsonObj()
        .add("key", jsonEscape(r.key))
        .add("checksum", jsonHex(r.checksum))
        .add("shard", jsonUInt(r.shard))
        .add("generation", jsonUInt(r.generation))
        .str();
}

String renderShardCoverage(const ShardCoverage & c)
{
    return JsonObj()
        .add("classification", jsonUInt(c.classification))
        .add("folded_token", renderToken(c.folded_token))
        .add("folded_cursor", jsonUInt(c.folded_cursor))
        .add("incarnation", renderShardIncarnation(c.incarnation))
        .add("has_live_precommit", jsonBool(c.has_live_precommit))
        .add("min_live_precommit_writer_epoch", jsonUInt(c.min_live_precommit_writer_epoch))
        .add("min_live_precommit_build_sequence", jsonUInt(c.min_live_precommit_build_sequence))
        .str();
}

String renderFoldSeal(const CasFoldSeal & seal)
{
    JsonObj per_ns_shard;
    for (const auto & [k, v] : seal.per_ns_shard)
        per_ns_shard.add(k, renderShardCoverage(v));

    std::vector<String> blob_target_runs;
    blob_target_runs.reserve(seal.blob_target_runs.size());
    for (const auto & r : seal.blob_target_runs)
        blob_target_runs.push_back(renderRunRef(r));

    std::vector<String> part_manifest_cleanup;
    part_manifest_cleanup.reserve(seal.part_manifest_cleanup.size());
    for (const auto & r : seal.part_manifest_cleanup)
        part_manifest_cleanup.push_back(renderRunRef(r));

    return JsonObj()
        .add("generation", jsonUInt(seal.generation))
        .add("parent_generation", jsonUInt(seal.parent_generation))
        .add("per_ns_shard", per_ns_shard.str())
        .add("blob_target_runs", jsonArray(blob_target_runs))
        .add("part_manifest_cleanup", jsonArray(part_manifest_cleanup))
        .str();
}

String renderRetiredEntry(const RetiredEntry & e)
{
    return JsonObj()
        .add("kind", jsonEscape(objectKindName(e.kind)))
        .add("hash", jsonHex(e.hash))
        .add("token", renderToken(e.token))
        .add("size", jsonUInt(e.size))
        .add("condemn_round", jsonUInt(e.condemn_round))
        .add("delete_pending", jsonBool(e.delete_pending))
        .str();
}

String renderRetiredSet(const RetiredSet & set)
{
    std::vector<String> entries;
    entries.reserve(set.entries.size());
    for (const auto & e : set.entries)
        entries.push_back(renderRetiredEntry(e));
    return JsonObj().add("entries", jsonArray(entries)).str();
}

String provenanceOpName(ProvenanceOp op)
{
    switch (op)
    {
        case ProvenanceOp::Other:    return "Other";
        case ProvenanceOp::Insert:   return "Insert";
        case ProvenanceOp::Merge:    return "Merge";
        case ProvenanceOp::Mutation: return "Mutation";
        case ProvenanceOp::Attach:   return "Attach";
        case ProvenanceOp::Repack:   return "Repack";
    }
    return "Unknown";
}

String renderProvenance(const Provenance & p)
{
    return JsonObj()
        .add("created_at_ms", jsonUInt(p.created_at_ms))
        .add("creator_server_id", jsonHex(p.creator_server_id))
        .add("ch_version", jsonUInt(p.ch_version))
        .add("op", jsonEscape(provenanceOpName(p.op)))
        .str();
}

String metaStateName(MetaState s)
{
    switch (s)
    {
        case MetaState::Clean:     return "clean";
        case MetaState::Condemned: return "condemned";
    }
    return "unknown";
}

/// The per-hash `.meta` descriptor sibling of a blob body (spec §raw-body-refinement, v3): a
/// FRESHNESS MARKER (Clean/Condemned), not the blob's payload — rendered separately from
/// `renderEnvelopeHeader`, which still decodes the (unchanged, still-enveloped) body itself.
String renderBlobMeta(const BlobMeta & m)
{
    return JsonObj()
        .add("object", jsonEscape("blob_meta"))
        .add("version", jsonUInt(m.version))
        .add("state", jsonEscape(metaStateName(m.state)))
        .add("condemn_round", jsonUInt(m.condemn_round))
        .add("size", jsonUInt(m.size))
        .str();
}

String renderEnvelopeHeader(const EnvelopeHeader & h)
{
    return JsonObj()
        .add("kind", jsonEscape(objectKindName(h.kind)))
        .add("hash_algo", jsonUInt(h.hash_algo))
        .add("writer_version", jsonUInt(h.writer_version))
        .add("compatibility_version", jsonUInt(h.compatibility_version))
        .add("logical_size", jsonUInt(h.logical_size))
        .add("logical_hash", jsonHex(h.logical_hash))
        .add("domain_id", jsonHex(h.domain_id))
        .add("incarnation_tag", jsonHex(h.incarnation_tag))
        .add("build_id", jsonHex(h.build_id))
        .add("header_len", jsonUInt(h.header_len))
        .add("provenance", h.provenance ? renderProvenance(*h.provenance) : "null")
        .add("intended_ref", h.intended_ref ? jsonEscape(*h.intended_ref) : "null")
        .str();
}

}

String caInspectToJson(const Layout & layout, const String & key, std::string_view bytes)
{
    /// Most-specific first: `cas/manifests/.../NNNNNN.proto` before the pool-wide `cas/refs/`
    /// prefix, the `/mount` and `/fold_seal` suffixes before the pool-wide `gc/state` exact match,
    /// the `/retired/` segment before the pool-wide `blobs/` prefix, and (v3) the `.meta` sibling
    /// suffix before the bare `blobs/` prefix it also matches.
    if (key.starts_with(layout.casManifestsPrefix()) && key.ends_with(".proto"))
        return renderPartManifest(decodePartManifest(bytes));

    if (key.starts_with(layout.casRefsPrefix()))
        return renderRootShard(decodeRootShard(bytes));

    if (key == layout.gcStateKey())
        return renderGcState(decodeGcState(bytes));

    if (key.ends_with("/mount"))
        return renderMountLease(decodeMountLease(bytes));

    if (key.ends_with("/fold_seal"))
        return renderFoldSeal(decodeFoldSeal(bytes));

    if (key.find("/retired/") != String::npos)
        return renderRetiredSet(decodeRetiredSet(bytes));

    /// The per-hash meta descriptor (v3): `blobMetaKey(id) == blobKey(id) + ".meta"`, so it ALSO
    /// matches `blobsPrefix()` below — must be checked first or it would wrongly decode as an
    /// envelope. The body itself (non-`.meta`) is unchanged: it still carries its envelope.
    if (key.starts_with(layout.blobsPrefix()) && key.ends_with(".meta"))
        return renderBlobMeta(decodeBlobMeta(bytes));

    if (key.starts_with(layout.blobsPrefix()))
        return renderEnvelopeHeader(decodeEnvelopeHeader(bytes, bytes.size(), ObjectKind::Blob));

    throw DB::Exception(DB::ErrorCodes::BAD_ARGUMENTS,
        "ca-inspect: unrecognized key layout '{}' (recognized: cas/refs, cas/manifests, "
        "gc/server-roots/*/mount, gc/state, gc/gen/*/fold_seal, retired, blobs, blobs/*.meta)", key);
}

}
