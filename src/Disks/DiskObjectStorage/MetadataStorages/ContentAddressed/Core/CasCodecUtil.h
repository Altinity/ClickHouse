#pragma once
#include <base/types.h>
#include <base/extended_types.h>
#include <Common/Exception.h>
#include <cstdint>
#include <cstring>
#include <string>
#include <string_view>

namespace DB
{
namespace ErrorCodes
{
    extern const int CORRUPTED_DATA;
}
}

namespace DB::Cas
{

/// Shared little-endian byte writers/readers for the three on-disk codecs.
///
/// The on-disk format is byte-exact (it IS pool format v2), so we never rely on struct layout or
/// memcpy of whole structs: every multi-byte field is written and read one byte at a time in
/// little-endian order. UInt128 is serialized as its 16 little-endian bytes.

inline void writeU8(String & out, uint8_t v)
{
    out.push_back(static_cast<char>(v));
}

inline void writeLE16(String & out, uint16_t v)
{
    out.push_back(static_cast<char>(v & 0xff));
    out.push_back(static_cast<char>((v >> 8) & 0xff));
}

inline void writeLE32(String & out, uint32_t v)
{
    for (int i = 0; i < 4; ++i)
        out.push_back(static_cast<char>((v >> (8 * i)) & 0xff));
}

inline void writeLE64(String & out, uint64_t v)
{
    for (int i = 0; i < 8; ++i)
        out.push_back(static_cast<char>((v >> (8 * i)) & 0xff));
}

inline void writeU128LE(String & out, const UInt128 & v)
{
    /// wide::integer stores 64-bit limbs; extract low and high 64 bits explicitly so we never
    /// depend on the in-memory limb order. Low 64 bits first (little-endian overall).
    const uint64_t low = static_cast<uint64_t>(v);
    const uint64_t high = static_cast<uint64_t>(v >> 64);
    writeLE64(out, low);
    writeLE64(out, high);
}

/// Sequential reader over a byte buffer; every out-of-bounds read throws CORRUPTED_DATA.
class ByteReader
{
public:
    explicit ByteReader(std::string_view data_) : data(data_) {}

    size_t position() const { return pos; }
    size_t remaining() const { return data.size() - pos; }
    bool eof() const { return pos >= data.size(); }

    void require(size_t n) const
    {
        if (pos + n > data.size())
            throw DB::Exception(DB::ErrorCodes::CORRUPTED_DATA,
                "CAS codec: truncated buffer (need {} bytes at offset {}, have {})", n, pos, data.size());
    }

    uint8_t readU8()
    {
        require(1);
        return static_cast<uint8_t>(data[pos++]);
    }

    uint16_t readLE16()
    {
        require(2);
        const uint16_t v = static_cast<uint16_t>(
            static_cast<uint16_t>(static_cast<uint8_t>(data[pos]))
            | (static_cast<uint16_t>(static_cast<uint8_t>(data[pos + 1])) << 8));
        pos += 2;
        return v;
    }

    uint32_t readLE32()
    {
        require(4);
        uint32_t v = 0;
        for (int i = 0; i < 4; ++i)
            v |= static_cast<uint32_t>(static_cast<uint8_t>(data[pos + i])) << (8 * i);
        pos += 4;
        return v;
    }

    uint64_t readLE64()
    {
        require(8);
        uint64_t v = 0;
        for (int i = 0; i < 8; ++i)
            v |= static_cast<uint64_t>(static_cast<uint8_t>(data[pos + i])) << (8 * i);
        pos += 8;
        return v;
    }

    UInt128 readU128LE()
    {
        const uint64_t low = readLE64();
        const uint64_t high = readLE64();
        return (UInt128(high) << 64) | UInt128(low);
    }

    String readBytes(size_t n)
    {
        require(n);
        String s(data.substr(pos, n));
        pos += n;
        return s;
    }

private:
    std::string_view data;
    size_t pos = 0;
};

}
