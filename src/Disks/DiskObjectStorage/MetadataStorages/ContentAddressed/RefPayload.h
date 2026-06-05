#pragma once
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Codec.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Identifiers.h>
#include <string>

namespace DB::ContentAddressed
{

/// The ref-object payload is a versioned struct on the shared codec (B28): `MAGIC(4) + version(1) +
/// part_id` (the part id as a length-prefixed string). The magic is `CARF` ("Content-Addressed ReF").
/// Version 1 holds only the part id; later versions append additive fields (B1's
/// `ReplicatedMergeTreePartHeader`, the per-ref mutable fields) WITHOUT breaking older readers, which
/// stop after the part id. The format is reserved for that growth on purpose.
constexpr FormatMagic kRefPayloadMagic = makeMagic("CARF");
constexpr uint8_t kRefPayloadVersion = 1;

/// Serialize a ref payload naming `part_id` (the publishing write path's payload). The SINGLE writer
/// paired with the SINGLE parser `partIdFromRefPayload` below.
std::string serializeRefPayload(const PartId & part_id);

/// Resolve a ref object payload into the part id it names. Parses the versioned `serializeRefPayload`
/// format EXACTLY: a wrong magic is `CORRUPTED_DATA` and a version newer than this build understands
/// is `NOT_IMPLEMENTED` (both fail-close — a published ref must name a part, and an unknown version
/// must never be misinterpreted). This is the SINGLE ref-payload parser: both the GC live-set scan
/// (`listLivePartIds`) and the read path (`ContentAddressedMetadataStorage::readRefPartId`) resolve a
/// ref through it, so they cannot disagree on the part id by construction (B28).
PartId partIdFromRefPayload(const std::string & payload);

}
