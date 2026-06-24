#pragma once
#include <base/types.h>
#include <base/extended_types.h>
#include <base/hex.h>
#include <Common/Exception.h>
#include <compare>
#include <cstddef>
#include <functional>
#include <string>
#include <utility>

namespace DB
{
namespace ErrorCodes
{
    extern const int BAD_ARGUMENTS;
}
}

namespace DB::Cas
{

/// Strong-typed id strings for the content-addressed core.
///
/// Each wraps a String but carries a distinct C++ type, so the compiler refuses to mix e.g. a
/// BlobId with a TreeId (the class of bug that caused live-blob data loss: mixed identifier types
/// made mismatched comparisons silently compile). Construction from String is explicit, there is NO
/// implicit conversion to String, and the underlying string is reached only through the explicit
/// string accessor — at the object-storage boundary.
///
/// All of them expose operator<=> and operator== (so they live in std::set / std::map) and a
/// std::hash specialization (so they live in std::unordered_* containers).

#define CAS_STRONG_STRING(NAME) \
    class NAME \
    { \
    public: \
        NAME() = default; \
        explicit NAME(String value_) : value(std::move(value_)) {} \
        const String & string() const { return value; } \
        auto operator<=>(const NAME &) const = default; \
        bool operator==(const NAME &) const = default; \
    private: \
        String value; \
    };

/// Ids are 32-char lowercase hex of a 128-bit hash. Envelope/codecs store the raw 16 bytes (LE).
CAS_STRONG_STRING(BlobId)
CAS_STRONG_STRING(TreeId)

/// Opaque namespace under which root manifests live. The core never interprets its contents:
/// the wiring composes strings like "srv1/<table_uuid>" or "shadow/<backup>/<table_uuid>".
/// Layout only validates its shape (non-empty, no leading/trailing or empty segments,
/// no segment equal to the reserved "_files").
CAS_STRONG_STRING(RootNamespace)

#undef CAS_STRONG_STRING

/// Returns 32 lowercase hex chars encoding `v`.
inline String u128ToHex(const UInt128 & v)
{
    return getHexUIntLowercase(v);
}

/// Parses 32-char lowercase (or uppercase) hex string to UInt128.
/// Throws BAD_ARGUMENTS on wrong length or non-hex characters.
inline UInt128 hexToU128(const String & hex)
{
    if (hex.size() != 32)
        throw DB::Exception(DB::ErrorCodes::BAD_ARGUMENTS,
            "hexToU128: expected 32 hex chars, got {}", hex.size());

    // Validate each character is a valid hex digit (unhex(c) returns 0xff for invalid chars).
    for (char c : hex)
    {
        if (::unhex(c) == 0xff)
            throw DB::Exception(DB::ErrorCodes::BAD_ARGUMENTS,
                "hexToU128: invalid hex character '{}'", c);
    }

    return unhexUInt<UInt128>(hex.data());
}

}

#define CAS_STRONG_STRING_HASH(NAME) \
    template <> \
    struct std::hash<DB::Cas::NAME> \
    { \
        size_t operator()(const DB::Cas::NAME & v) const noexcept \
        { \
            return std::hash<std::string>{}(v.string()); \
        } \
    };

CAS_STRONG_STRING_HASH(BlobId)
CAS_STRONG_STRING_HASH(TreeId)
CAS_STRONG_STRING_HASH(RootNamespace)

#undef CAS_STRONG_STRING_HASH
