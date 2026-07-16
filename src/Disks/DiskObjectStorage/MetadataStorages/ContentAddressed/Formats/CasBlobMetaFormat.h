#pragma once
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/Formats/CasFormat.h>
#include <base/types.h>
#include <cstdint>
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

/// The durable meta body. v3 text form: header line + {"st":"<state word>","cr":"<condemn_round>",
/// "sz":"<size>"}. `size` is the raw body size (introspection/fsck/GC accounting — reads never consult
/// the meta). The meta is CAS-swapped by its backend etag; its on-disk bytes are never compared.
struct BlobMeta
{
    /// Vestigial: the header line `v` is the authoritative format version. Kept (default 1) because
    /// CasInspect renders it; NOT serialized to the body, and decode leaves it at 1 (was always 1).
    uint8_t version = 1;
    MetaState state = MetaState::Clean;
    uint64_t condemn_round = 0;   /// the GC round that condemned this blob (M4: guards a
                                  /// condemned-etag ABA after spare->re-condemn)
    uint64_t size = 0;
};

/// Text codec. encode is total; decode fails closed (CORRUPTED_DATA) on bad header/type/unknown state.
String encodeBlobMeta(const BlobMeta & meta);
BlobMeta decodeBlobMeta(std::string_view bytes);

}
