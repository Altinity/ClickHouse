#pragma once

#include <Compression/CompressionInfo.h>
#include <base/types.h>
#include <base/unaligned.h>
#include <string_view>

namespace DB::Cas
{

/// ClickHouse compressed files are a concatenation of independent blocks:
///   CityHash128(payload)  (16 bytes)
///   method + compressed_size + uncompressed_size  (9 bytes)
///   compressed payload
/// `compressed_size` includes the 9-byte header, not the checksum.
///
/// Used so CAS can publish each compressed block as its own blob when MergeTree cuts those
/// blocks at content-defined uncompressed windows. FastCDC on the ciphertext would cut inside
/// a block and throw away identical recompressed windows.
inline bool tryPeekClickHouseCompressedBlockSize(std::string_view data, size_t & block_bytes)
{
    constexpr size_t checksum_size = sizeof(UInt64) * 2;
    constexpr size_t header_size = COMPRESSED_BLOCK_HEADER_SIZE;
    if (data.size() < checksum_size + header_size)
        return false;

    const char * header = data.data() + checksum_size;
    const UInt32 compressed_incl_header = unalignedLoadLittleEndian<UInt32>(header + 1);
    const UInt32 uncompressed_size = unalignedLoadLittleEndian<UInt32>(header + 5);
    if (compressed_incl_header < header_size || compressed_incl_header > DBMS_MAX_COMPRESSED_SIZE)
        return false;
    if (uncompressed_size == 0 || uncompressed_size > DBMS_MAX_DECOMPRESSED_SIZE)
        return false;

    block_bytes = checksum_size + compressed_incl_header;
    return true;
}

}
