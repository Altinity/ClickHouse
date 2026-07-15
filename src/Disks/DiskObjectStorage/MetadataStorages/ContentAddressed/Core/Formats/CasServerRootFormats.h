#pragma once
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/Formats/CasFormat.h>
#include <base/types.h>
#include <base/extended_types.h>
#include <cstdint>
#include <string_view>

namespace DB::Cas
{

/// Per-server-root control singletons under gc/server-roots/<srid>/ (mount safety, Phase 0). v3 text:
/// header line + one JSON body object. Owner binds srid -> server UUID (write-once); epoch is the
/// monotone next writer epoch; mount lease is the current live holder (liveness + merged min_active).

struct OwnerObject
{
    UInt128 server_uuid{};
};

struct ServerEpoch
{
    uint64_t next_writer_epoch = 0;
};

struct MountLease
{
    UInt128 server_uuid{};
    uint64_t writer_epoch = 0;
    String hostname;
    uint64_t pid = 0;
    uint64_t started_at_ms = 0;
    uint64_t seq = 0;
    uint64_t expires_at_ms = 0;
    uint64_t min_active = 0;   /// UINT64_MAX = retired (farewell)
    bool gc_fenced = false;    /// GC fence-out of an expired lease; terminal
};

String encodeOwner(const OwnerObject & o);
OwnerObject decodeOwner(std::string_view data);

String encodeServerEpoch(const ServerEpoch & e);
ServerEpoch decodeServerEpoch(std::string_view data);

String encodeMountLease(const MountLease & m);
MountLease decodeMountLease(std::string_view data);

}
