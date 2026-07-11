#pragma once
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasManifestId.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasIds.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasBlobDigest.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasBlobRef.h>
#include <base/types.h>
#include <base/extended_types.h>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace DB::Cas
{

/// Where a manifest entry's file bytes live (spec §Object Identity And Ownership). No `Subtree`:
/// there are no nested tree objects in this design; a directory is a path prefix, not a placement.
enum class EntryPlacement : uint8_t
{
    Inline = 1,   /// bytes embedded in `inline_bytes`
    Blob = 2,     /// bytes stored as a content-addressed blob at blobKey(blob_hash)
};

/// One file entry inside a part manifest. `ref`/`blob_size` are meaningful only for `Blob`;
/// `inline_bytes` only for `Inline`. `ref` (mixed-algo pools, Phase 3 T2) is the FULL blob identity —
/// the algo travels WITH the digest, per-entry, so a manifest may carry entries minted under
/// different hash algos (additive algo switching, no migration): a bare digest is never the identity.
struct ManifestEntry
{
    String path;
    EntryPlacement placement = EntryPlacement::Inline;
    BlobRef ref{};
    uint64_t blob_size = 0;
    String inline_bytes;
    bool operator==(const ManifestEntry &) const = default;
};

/// The immutable body of a single root-local part manifest (spec §Part Manifest Reference And
/// Identity, OQ1). It repeats its own `ref` and `root_namespace_id` for fail-closed validation only -
/// never as a second identity. `payload_digest` is integrity/debug only: NEVER a key, NEVER dedup,
/// NEVER in-degree. No mutable per-ref payload here (that stays in the root RefRecord). No directory
/// index in Phase 1a (OQ3-deferred). No manifest-wide `blob_hash_len` (Phase 3 T2 deleted it): every
/// entry's digest width follows its OWN `ref.algo`, so one manifest may mix 16- and 32-byte digests.
struct PartManifest
{
    ManifestRef ref;
    RootNamespace root_namespace_id;
    UInt128 payload_digest{};
    std::vector<ManifestEntry> entries;
    bool operator==(const PartManifest &) const = default;
};

/// Deterministic, streaming-capable encode. Entries are written in canonical path order (the encoder
/// sorts them); a duplicate path throws CORRUPTED_DATA. Byte output is reproducible for identical
/// input (no timestamps, no nondeterministic compression).
String encodePartManifest(const PartManifest & m);

/// Decode. Throws CORRUPTED_DATA on bad magic, future format, unknown placement, duplicate path, or a
/// truncated buffer.
PartManifest decodePartManifest(std::string_view data);

/// Content digest of the canonical encoded body, using the CAS content-hash primitive
/// (`CityHash_v1_0_2::CityHash128`, the same one blob/tree hashing uses - NOT a second hash
/// primitive). Callers (Phase 1b `stageManifest`) set `PartManifest.payload_digest` from this. It is
/// integrity/debug ONLY - never a key, never dedup, never in-degree. Stable for identical bodies;
/// changes when any byte of the canonical encoding does, and is independent of the `payload_digest`
/// field itself (computed with it zeroed).
UInt128 computePayloadDigest(const PartManifest & m);

/// Fail-closed read/fold checks (spec §Object Identity And Ownership). Tested in Task 6.
/// The journal `ManifestRef` must equal the `ref` inside the decoded body.
bool refMatchesBody(const ManifestRef & journal_ref, const PartManifest & body);
/// The body `root_namespace_id` must equal the owning root namespace.
bool manifestNamespaceMatches(const RootNamespace & owning, const PartManifest & body);

/// Ordered-entry lookup primitives (spec 2026-07-08-cas-part-folder-cache §Shared Decodes): pure
/// functions of a DECODED manifest, whose entries the decoder guarantees strictly ascending by
/// `path`. `PartFolderView` composes these with wiring policy; Core tests use them directly.

/// Binary search. Returns nullptr when absent. The pointer aliases `entries` — do not outlive it.
const ManifestEntry * findEntry(const std::vector<ManifestEntry> & entries, std::string_view path);

/// The contiguous [first, last) sub-span of entries whose path starts with `dir_prefix` (canonical
/// order makes matches contiguous). Empty prefix = the whole span.
std::pair<const ManifestEntry *, const ManifestEntry *>
entryRange(const std::vector<ManifestEntry> & entries, std::string_view dir_prefix);

}
