#pragma once

#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Backend/CasBackend.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Primitives/CasTypes.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Formats/CasLayout.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Formats/CasBlobMetaFormat.h>

#include <optional>

namespace DB::Cas
{

/// A decoded blob meta record together with the backend token observed for the same incarnation.
/// The token is returned with the decoded record because the next conditional update or exact delete
/// must be guarded by the version that was actually read; comparing encoded meta bytes would not
/// provide that protection.
struct LoadedMeta
{
    BlobMeta meta;
    Token etag;
};

/// Shared lifecycle operations for the blob freshness marker used by the writer and GC. The key is
/// built from the complete `BlobRef`, so each algorithm uses its own digest representation and no
/// pool-wide digest width is threaded through these functions. The marker is a point-read hint rather
/// than the blob lifetime's linearization point: the blob body's incarnation tag and exact-token body
/// deletion provide the safety guarantee, while a stale marker can at most make a writer re-upload.
///
/// `loadMeta` is used in the adopt path, so its backend must provide strong read-after-write
/// consistency: after a successful meta write, the one subsequent GET must observe that write.
/// Conditional updates and deletion use the backend token, not the encoded meta bytes.
///
/// Returns the current decoded marker and its conditional token, or nullopt when the meta key is
/// absent. Decoding errors propagate as exceptions.
std::optional<LoadedMeta> loadMeta(Backend & backend, const Layout & layout, const BlobRef & ref);

/// Creates the marker only when its key is absent. A precondition failure means another writer has
/// already created the marker and is returned as a normal `CasResult`; the backend's error handling
/// is otherwise allowed to propagate exceptions.
CasResult putMetaIfAbsent(Backend & backend, const Layout & layout, const BlobRef & ref, const BlobMeta & meta);

/// Replaces the marker only when its current backend token equals `expected`. A conflict leaves the
/// existing marker unchanged, allowing the caller to reload it and make a decision from the newer
/// state. The operation is conditional on the backend token even though the marker's encoded bytes
/// are format-specific and are never compared directly.
CasResult casMeta(Backend & backend, const Layout & layout, const BlobRef & ref, const Token & expected, const BlobMeta & meta);

/// Deletes only the marker incarnation identified by `expected`. A token mismatch leaves the current
/// marker untouched; `NotFound` is distinct from that case so callers can tell absence from a raced
/// replacement. The backend's complete `DeleteOutcome` is returned, including any storage-specific
/// delete-marker status.
DeleteOutcome deleteMetaExact(Backend & backend, const Layout & layout, const BlobRef & ref, const Token & expected);

}
