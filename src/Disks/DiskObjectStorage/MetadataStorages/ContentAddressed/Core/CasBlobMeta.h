#pragma once

#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasBackend.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasLayout.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasToken.h>
#include <base/types.h>
#include <Core/Types.h>

#include <optional>
#include <string_view>

namespace DB::Cas
{

/// The per-hash meta descriptor lifecycle (spec 2026-07-09 §raw-body-refinement, v3). The meta is a
/// 2-state FRESHNESS MARKER, not a linearization point: the body's in-body incarnation_tag plus
/// exact-token delete is the safety core, and the meta is only a point-read for the writer's dedup gate.
enum class MetaState : uint8_t
{
    Clean = 0,       /// referenceable; body present (INV-META-BODY)
    Condemned = 1,   /// GC marked in-degree 0; body STILL present (a writer may resurrect by CAS)
};

/// The durable meta body (fixed codec, ~a couple dozen bytes). `version` guards codec evolution; `size`
/// is the raw body size (introspection/fsck/GC accounting — reads never consult the meta).
struct BlobMeta
{
    uint8_t version = 1;
    MetaState state = MetaState::Clean;
    uint64_t condemn_round = 0;   /// the GC round that condemned this blob (M4: guards a
                                  /// condemned-etag ABA after spare->re-condemn)
    uint64_t size = 0;
};

/// Fixed codec. encode is total; decode fails closed (CORRUPTED_DATA) on bad magic/version/length.
String encodeBlobMeta(const BlobMeta & meta);
BlobMeta decodeBlobMeta(std::string_view bytes);

/// A loaded meta plus its backend etag — the etag is the conditional token for the next CAS/delete.
struct LoadedMeta
{
    BlobMeta meta;
    Token etag;
};

/// The shared meta-ops layer used by BOTH Build (writer) and Gc. Key-agnostic across all backends.
/// Requires strong read-after-write consistency for the 1-GET adopt (S3 since 2020, RustFS yes).
std::optional<LoadedMeta> loadMeta(Backend & backend, const Layout & layout, const UInt128 & hash);
CasResult putMetaIfAbsent(Backend & backend, const Layout & layout, const UInt128 & hash, const BlobMeta & meta);
CasResult casMeta(Backend & backend, const Layout & layout, const UInt128 & hash, const Token & expected, const BlobMeta & meta);
DeleteOutcome deleteMetaExact(Backend & backend, const Layout & layout, const UInt128 & hash, const Token & expected);

}
