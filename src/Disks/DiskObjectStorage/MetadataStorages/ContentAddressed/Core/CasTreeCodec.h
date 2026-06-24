#pragma once
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasIds.h>
#include <base/types.h>
#include <base/extended_types.h>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace DB::Cas
{

/// Canonical tree codec — protocol spec §3. A tree is the directory listing of one logical folder
/// (a ref). Entries are sorted by name byte-wise so that two identical folders produce a
/// byte-identical payload (and therefore hash identically). Magic "CATR", version 1.

enum class Placement : uint8_t
{
    Inline = 1,     /// file bytes embedded in the tree payload
    Blob = 2,       /// file stored as a standalone blob at blobKey(file_hash)
    Subtree = 3,    /// child tree; file_hash is the child tree id, file_size its payload size
};

struct TreeEntry
{
    String name;
    Placement placement = Placement::Inline;
    UInt128 file_hash{};       /// blob file hash; for subtree: child tree id
    uint64_t file_size = 0;    /// logical file size; for subtree: tree payload size
    String inline_bytes;       /// Inline only
    bool operator==(const TreeEntry &) const = default;
};

/// Encodes entries into the canonical payload. SORTS entries by name byte-wise; throws BAD_ARGUMENTS
/// on a duplicate name.
String encodeTree(std::vector<TreeEntry> entries);

/// Decodes a tree payload. Throws CORRUPTED_DATA on bad magic, future version, unknown placement, or
/// a truncated buffer.
std::vector<TreeEntry> decodeTree(std::string_view data);

/// The tree's logical id = cityHash128 of the encoded payload (same hashing the PoC uses for content).
TreeId treeIdFor(const String & encoded);

}
