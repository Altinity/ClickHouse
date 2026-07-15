#pragma once

#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasBackend.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasBlobDigest.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasBlobRef.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasLayout.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasToken.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/Formats/CasBlobMetaFormat.h>
#include <base/types.h>
#include <Core/Types.h>

#include <optional>
#include <string_view>

namespace DB::Cas
{

/// A loaded meta plus its backend etag — the etag is the conditional token for the next CAS/delete.
struct LoadedMeta
{
    BlobMeta meta;
    Token etag;
};

/// The shared meta-ops layer used by BOTH Build (writer) and Gc. Key-agnostic across all backends.
/// Requires strong read-after-write consistency for the 1-GET adopt (S3 since 2020, RustFS yes).
///
/// Phase 3 T2/T3 (mixed-algo pools): keyed by the full `BlobRef` pair -- the codec is derived INSIDE
/// via `codecFor(ref.algo)` (never a pool-wide width), so callers never thread a `DigestCodec`.
std::optional<LoadedMeta> loadMeta(Backend & backend, const Layout & layout, const BlobRef & ref);
CasResult putMetaIfAbsent(Backend & backend, const Layout & layout, const BlobRef & ref, const BlobMeta & meta);
CasResult casMeta(Backend & backend, const Layout & layout, const BlobRef & ref, const Token & expected, const BlobMeta & meta);
DeleteOutcome deleteMetaExact(Backend & backend, const Layout & layout, const BlobRef & ref, const Token & expected);

}
