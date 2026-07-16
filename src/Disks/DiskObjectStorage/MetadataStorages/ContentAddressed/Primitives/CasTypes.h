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
/// RootNamespace (the class of bug that caused live-blob data loss: mixed identifier
/// types made mismatched comparisons silently compile). Construction from String is explicit, there
/// is NO implicit conversion to String, and the underlying string is reached only through the
/// explicit string accessor — at the object-storage boundary.
///
/// (`BlobId` — a bare hex string — was deleted in the mixed-algo-pools refactor: a blob's identity is
/// now ONLY the `BlobRef` pair, `CasBlobRef.h`. `TreeId` was part of the standalone-tree layer
/// excised in 2026-07-03 (rev. 15 `PartManifest` redesign) and is no longer defined.)
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

CAS_STRONG_STRING_HASH(RootNamespace)

#undef CAS_STRONG_STRING_HASH

// ===== Merged from CasBlobDigest.h (merge #1) =====
#include <base/defines.h>
#include <base/extended_types.h>
#include <base/hex.h>
#include <base/types.h>
#include <Common/Exception.h>
#include <array>
#include <compare>
#include <cstdint>
#include <cstring>
#include <string>
#include <string_view>

namespace DB
{
namespace ErrorCodes
{
    extern const int BAD_ARGUMENTS;
}
}

namespace DB::Cas
{

/// The blob CONTENT digest (CAS pluggable-blob-hash Phase 2, design §12): an algo-scoped
/// variable-length digest that generalizes the fixed 128-bit `UInt128` blob identity so a
/// 256-bit hash (`sha256`) is admissible alongside the existing 128-bit hashes (`cityHash128`,
/// `xxh3-128`). Always 32 bytes wide, big-endian; only the FIRST `blobHashLenFor(algo)` bytes
/// (the ALGO's digest width, 16 or 32 -- Phase 3 T4 deleted the pool-wide width, a mixed-algo pool
/// has no single one) are meaningful -- bytes beyond that are always zero. A fixed-size
/// `std::array` (not a `String`): design §12 rejected a variable `String`
/// because a 32-byte `sha256` digest exceeds libc++'s 22-byte SSO, forcing one heap allocation
/// per `ManifestEntry` on the manifest-decode READ path (part-folder validate-on-hit) and costing
/// MORE memory than the fixed array even for the 32-byte case.
///
/// This is the CONTENT digest ONLY (design §12 "widen ONLY the content digest"): internal
/// 128-bit ids -- `payload_digest`, `RunRef::checksum`, `sourceEdgeId`, `GcLease::owner`,
/// `GcHeartbeat::owner`, `manifestCleanupShard` -- stay `UInt128` and do NOT become a
/// `BlobDigest`. This task (Phase 2 Task 1) introduces the type and its codec ONLY; no existing
/// `UInt128 blob_hash` field is migrated yet (later tasks do that one structure at a time).
struct BlobDigest
{
    std::array<uint8_t, 32> bytes{};

    auto operator<=>(const BlobDigest &) const = default;
    bool operator==(const BlobDigest &) const = default;

    /// For 128-bit pools (`cityHash128`, `xxh3-128`): the existing `UInt128` blob hash, written
    /// big-endian into `bytes[0:16]`, tail zero. Lets later tasks migrate `UInt128 blob_hash`
    /// fields to `BlobDigest` incrementally, one structure at a time, without a flag day.
    static BlobDigest fromU128(const UInt128 & v)
    {
        BlobDigest d;
        for (int i = 0; i < 16; ++i)
            d.bytes[static_cast<size_t>(i)] = static_cast<uint8_t>(static_cast<UInt8>(v >> (8 * (15 - i))));
        return d;
    }

    /// Inverse of `fromU128`: reads `bytes[0:16]` as big-endian back into a `UInt128`. Only
    /// meaningful for a digest actually produced at a 128-bit pool's width -- `bytes[16:32]` are
    /// ignored (they are zero at a 128-bit pool's codec boundary).
    UInt128 toU128() const
    {
        UInt128 v = 0;
        for (int i = 0; i < 16; ++i)
            v = (v << 8) | static_cast<UInt8>(bytes[static_cast<size_t>(i)]);
        return v;
    }
};

/// Hasher for `BlobDigest` as an `unordered_map`/`unordered_set` key. This is an in-process hash
/// table key, not a content address, so a cheap FNV-1a mix over the raw bytes is sufficient -- no
/// cryptographic property is needed here.
struct BlobDigestHash
{
    size_t operator()(const BlobDigest & d) const noexcept
    {
        size_t h = 1469598103934665603ull; /// FNV-1a 64-bit offset basis
        for (uint8_t b : d.bytes)
        {
            h ^= b;
            h *= 1099511628211ull; /// FNV-1a 64-bit prime
        }
        return h;
    }
};

/// The ONE algo-scoped digest<->hex/bytes codec (design §12 "len-drift" mitigation, the biggest
/// Phase 2 risk; Phase 3 T4 relocated its width source from the deleted pool-wide `blob_hash_len`
/// to the per-`BlobRef` algo): every digest<->hex or digest<->bytes conversion routes through an
/// object constructed from a digest width (16 for `cityHash128`/`xxh3-128`, 32 for `sha256`) --
/// NEVER a free function taking a bare length. Construct via `Cas::codecFor(algo)`
/// (`CasBlobRef.h`), the ONE way to obtain one; never wire a pool-wide value in again (a mixed-algo
/// pool has no single width). `toHex`/`fromHex` render/parse exactly `2 * digestLen()` lowercase
/// hex chars; `toBytesBE`/`fromBytesBE` (de)serialize exactly `digestLen()` raw bytes; `shardOf`
/// reads the first 8 bytes big-endian -- bit-identical to today's `blob_hash >> 64` for every
/// 128-bit digest, the gate this whole refactor must never break (design §12).
class DigestCodec
{
public:
    explicit DigestCodec(uint64_t blob_hash_len_) : len(blob_hash_len_)
    {
        chassert(len == 16 || len == 32, "DigestCodec: digest length must be 16 or 32 bytes");
    }

    uint64_t digestLen() const { return len; }

    /// Renders exactly `2 * digestLen()` lowercase hex chars.
    String toHex(const BlobDigest & d) const
    {
        checkZeroTail(d, "toHex");
        return hexString(d.bytes.data(), len);
    }

    /// Requires exactly `2 * digestLen()` hex chars; throws `BAD_ARGUMENTS` otherwise (wrong
    /// width or a non-hex character). Zero-fills the tail beyond `digestLen()`.
    BlobDigest fromHex(std::string_view hex) const
    {
        if (hex.size() != 2 * len)
            throw Exception(ErrorCodes::BAD_ARGUMENTS,
                "DigestCodec::fromHex: expected {} hex chars for a {}-byte digest, got {}",
                2 * len, len, hex.size());

        for (char c : hex)
        {
            if (unhex(c) == 0xff)
                throw Exception(ErrorCodes::BAD_ARGUMENTS, "DigestCodec::fromHex: invalid hex character '{}'", c);
        }

        BlobDigest d;
        for (uint64_t i = 0; i < len; ++i)
            d.bytes[i] = unhex2(hex.data() + i * 2);
        return d;
    }

    /// Serializes exactly `digestLen()` raw bytes, big-endian (i.e. `bytes[0:digestLen()]`).
    String toBytesBE(const BlobDigest & d) const
    {
        checkZeroTail(d, "toBytesBE");
        return String(reinterpret_cast<const char *>(d.bytes.data()), len);
    }

    /// Requires exactly `digestLen()` bytes; throws `BAD_ARGUMENTS` otherwise. Zero-fills the
    /// tail beyond `digestLen()`.
    BlobDigest fromBytesBE(std::string_view b) const
    {
        if (b.size() != len)
            throw Exception(ErrorCodes::BAD_ARGUMENTS,
                "DigestCodec::fromBytesBE: expected {} bytes for the pool's digest width, got {}", len, b.size());

        BlobDigest d;
        memcpy(d.bytes.data(), b.data(), len);
        return d;
    }

    /// BE-u64 of `bytes[0:8]` -- an EXPLICIT big-endian read, bit-identical to today's
    /// `static_cast<uint64_t>(blob_hash >> 64)` for every 128-bit digest (design §12). This must
    /// NEVER become a native-endian `memcpy` read: that would silently reshard on a
    /// little-endian host (the `ShardReducer` misroute hazard the design calls out).
    uint64_t shardOf(const BlobDigest & d) const
    {
        uint64_t v = 0;
        for (int i = 0; i < 8; ++i)
            v = (v << 8) | d.bytes[static_cast<size_t>(i)];
        return v;
    }

private:
    uint64_t len;

    /// len-drift guard (design §12): bytes beyond the pool's digest width must always be zero at
    /// a codec boundary. Debug-only (`chassert`) -- this is an internal invariant check, not a
    /// release fail-close; a genuinely wrong-width INPUT is already rejected loudly by
    /// `fromHex`/`fromBytesBE` above.
    void checkZeroTail(const BlobDigest & d, [[maybe_unused]] const char * what) const
    {
        for (uint64_t i = len; i < d.bytes.size(); ++i)
            chassert(d.bytes[i] == 0, fmt::format("DigestCodec::{}: non-zero byte at tail position {} (len={})", what, i, len));
    }
};

}

// ===== Merged from CasBlobRef.h =====
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Primitives/CasBlobHasher.h>

namespace DB::Cas
{

/// THE blob identity (mixed-algo pools, design 2026-07-11 §2): the PAIR of the hash algo and the
/// digest. A bare digest is NOT a blob identity anywhere -- `ch128` and `xxh3` digests are both
/// 16-byte, so the same digest value under two algos names two DIFFERENT objects. BlobRef is
/// constructed ONLY where algo and digest are born together (the write mint / the hasher) or read
/// together (a durable form: settlement key, blob path, manifest entry, envelope). Every other
/// site COPIES BlobRefs -- never assemble one from an algo and a digest obtained separately.
struct BlobRef
{
    BlobHashAlgo algo = BlobHashAlgo::CityHash128;
    BlobDigest digest{};

    auto operator<=>(const BlobRef &) const = default;
    bool operator==(const BlobRef &) const = default;
};

/// Hasher for unordered_map/unordered_set keys (in-process only, not a content address).
struct BlobRefHash
{
    size_t operator()(const BlobRef & r) const noexcept
    {
        size_t h = BlobDigestHash{}(r.digest);
        h ^= static_cast<size_t>(r.algo) + 0x9e3779b97f4a7c15ULL + (h << 6) + (h >> 2);
        return h;
    }
};

/// The ONE way to obtain a digest codec for an algo (replaces the deleted pool-wide
/// `DigestCodec(PoolMeta)`): width follows the algo, never a pool-level assumption.
inline DigestCodec codecFor(BlobHashAlgo algo)
{
    return DigestCodec(blobHashLenFor(algo));
}

/// Bare lowercase hex of the digest at the algo's width -- for OBJECT KEY construction only
/// (the algo lives in the key's path segment `blobs/<algo>/...`).
inline String blobHexOf(const BlobRef & r)
{
    return codecFor(r.algo).toHex(r.digest);
}

/// Human/log identity: "<algoName>:<hex>", e.g. "sha256:ab12...". Rendered ids must never be a
/// bare hex (ambiguous across algos) -- events, inspect JSON and error messages use this.
inline String blobIdOf(const BlobRef & r)
{
    return String(blobHashAlgoName(r.algo)) + ":" + blobHexOf(r);
}

}

// ===== Merged from CasManifestId.h =====
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Formats/CasFormat.h>
#include <base/types.h>
#include <base/extended_types.h>
#include <fmt/format.h>
#include <cstdint>
#include <tuple>

namespace DB::Cas
{

/// The compact reference a root journal stores for a part manifest (spec §Part Manifest Reference
/// And Identity). It is NOT a string key: `CasLayout::manifestKey` derives the object key from this
/// ref plus the owning root namespace. `root_namespace_id` is deliberately NOT a field here - it
/// comes from the owning root context and must never be serialized into the journal ref.
///
///   writer_epoch         - durable monotone writer epoch allocated under the mounted `server_root_id`;
///                          never reused for that server root.
///   build_sequence       - monotone build sequence inside one writer incarnation; part of identity
///                          and of the build-scoped debris prefix.
///   manifest_ordinal     - monotone ordinal inside one build, rendered as `000001.zst` in the key.
struct ManifestRef
{
    uint64_t writer_epoch = 0;
    uint64_t build_sequence = 0;
    uint32_t manifest_ordinal = 0;

    bool operator==(const ManifestRef & o) const = default;

    /// Total order for std::map / std::set keys. Field order is arbitrary but stable.
    bool operator<(const ManifestRef & o) const
    {
        return std::tie(writer_epoch, build_sequence, manifest_ordinal)
             < std::tie(o.writer_epoch, o.build_sequence, o.manifest_ordinal);
    }
};

/// The protocol identity GC uses (spec §Object Identity And Ownership): namespace-qualified
/// `ManifestId = (root_namespace_id, ManifestRef)`. It keys source edges / blob deltas / cleanup work
/// and addressing. `ManifestId` is the protocol identity; the TLA+ model abstracts it to
/// `ManifestSafetyId = (root_namespace, manifest ref)`, a Phase-0 model-only term that never
/// appears in this code.
/// Two namespaces may legally carry the same `ManifestRef` tuple without addressing the same object;
/// keying source edges / blob deltas / cleanup work by `ManifestRef` alone is the modeled
/// `SabotageKeyByRefNotId` hazard. Always key by `ManifestId`.
struct ManifestId
{
    RootNamespace root_namespace;   /// owning namespace; NOT part of the journal ref
    ManifestRef ref;

    bool operator==(const ManifestId & o) const = default;

    bool operator<(const ManifestId & o) const
    {
        if (root_namespace.string() != o.root_namespace.string())
            return root_namespace.string() < o.root_namespace.string();
        return ref < o.ref;
    }
};

/// A `{writer_epoch, build_sequence}` pair identifying the incarnation of a ref-shard (legacy
/// mutable-shard model). `{0, 0}` is the unstamped sentinel -- a valid value, not an error. Relocated
/// here from the removed legacy ref-shard codec (dropped with the snapshot+log ref model): it survives as
/// the type of `ShardCoverage::incarnation` in the fold seal, which the snapshot+log model leaves unstamped.
struct ShardIncarnation
{
    uint64_t writer_epoch = 0;
    uint64_t build_sequence = 0;
    bool operator==(const ShardIncarnation &) const = default;
    bool operator<(const ShardIncarnation & o) const
    {
        return std::tie(writer_epoch, build_sequence) < std::tie(o.writer_epoch, o.build_sequence);
    }
};

inline constexpr uint32_t kMaxManifestOrdinal = 999999;

/// Six-digit filename for a per-build part-manifest ordinal: `000001.zst` through `999999.zst`
/// (the registered v3 stored suffix for `FormatId::PartManifest`). `0` is reserved as an invalid
/// sentinel and is never emitted.
inline String manifestOrdinalFileName(uint32_t manifest_ordinal)
{
    if (manifest_ordinal == 0 || manifest_ordinal > kMaxManifestOrdinal)
        throw DB::Exception(DB::ErrorCodes::BAD_ARGUMENTS,
            "Manifest ordinal must be in [1, {}], got {}", kMaxManifestOrdinal, manifest_ordinal);
    return fmt::format("{:06}{}", manifest_ordinal, storedSuffix(FormatId::PartManifest));
}

inline String manifestRefDebugString(const ManifestRef & ref)
{
    return fmt::format("{}:{}:{}", ref.writer_epoch, ref.build_sequence, ref.manifest_ordinal);
}

}

/// std::hash specializations so ManifestRef / ManifestId can key unordered containers (the read-path
/// `(ManifestId, Token)` cache in Phase 1c, plus any GC-side unordered map). Equality is the
/// `operator==` above; these hashes must agree with it (equal values => equal hash).
namespace std
{

template <>
struct hash<DB::Cas::ManifestRef>
{
    size_t operator()(const DB::Cas::ManifestRef & r) const
    {
        const size_t h1 = std::hash<uint64_t>{}(r.writer_epoch);
        const size_t h2 = std::hash<uint64_t>{}(r.build_sequence);
        const size_t h3 = std::hash<uint32_t>{}(r.manifest_ordinal);
        size_t h = h1;
        h ^= h2 + 0x9e3779b97f4a7c15ULL + (h << 6) + (h >> 2);
        h ^= h3 + 0x9e3779b97f4a7c15ULL + (h << 6) + (h >> 2);
        return h;
    }
};

template <>
struct hash<DB::Cas::ManifestId>
{
    size_t operator()(const DB::Cas::ManifestId & id) const
    {
        const size_t h1 = std::hash<::String>{}(id.root_namespace.string());
        const size_t h2 = std::hash<DB::Cas::ManifestRef>{}(id.ref);
        return h1 ^ (h2 + 0x9e3779b97f4a7c15ULL + (h1 << 6) + (h1 >> 2));
    }
};

}

// ===== Merged from CasToken.h =====
#include <base/types.h>

namespace DB::Cas
{

/// How the backend identifies one physical incarnation of a key (protocol spec §2).
enum class TokenType : uint8_t
{
    ETag = 1,        /// S3-family / Azure
    Generation = 2,  /// GCS (binding deferred; fail-closed until probed)
    Emulated = 3,    /// test backends (in-memory fake, Local emulation)
};

/// A backend-native incarnation token. Opaque; sent back to the backend EXACTLY as observed.
struct Token
{
    String value;
    TokenType type = TokenType::ETag;

    bool empty() const { return value.empty(); }
    bool operator==(const Token &) const = default;
};

}

// ===== Merged from CasRefIds.h =====
#include <base/types.h>
#include <base/hex.h>
#include <Common/Exception.h>
#include <compare>
#include <cstdint>
#include <optional>
#include <string_view>

namespace DB
{
namespace ErrorCodes
{
    extern const int LOGICAL_ERROR;
}
}

namespace DB::Cas
{

/// The ordered ref-transaction identifier (spec §Ordered Ref Transaction Identifier). A successful
/// writer mount establishes a strictly newer `writer_epoch`; within an epoch the mounted writer
/// allocates `ref_sequence` from a Store-wide strictly increasing counter at append time. Both fields
/// are nonzero for a valid id -- {0, 0} is never a real transaction. `writer_epoch` is the primary
/// ordering component, so tuple order matches the intended timeline even across an epoch restart that
/// resets `ref_sequence` back to one.
struct RefTxnId
{
    uint64_t writer_epoch = 0;
    uint64_t ref_sequence = 0;

    auto operator<=>(const RefTxnId &) const = default;
};

/// Renders the canonical form: two fixed-width, lower-case, 16-digit hexadecimal numbers joined by
/// '-' (e.g. "0000000000000007-000000000000008e"). Lexical order of the render equals tuple order of
/// `id`, because '-' (0x2d) sorts below every hex digit character and both fields are fixed-width.
/// Throws LOGICAL_ERROR if either field is zero: this render becomes an object key, and an invalid id
/// must never silently produce a well-formed-looking one.
inline String renderRefTxnId(const RefTxnId & id)
{
    if (id.writer_epoch == 0 || id.ref_sequence == 0)
        throw DB::Exception(DB::ErrorCodes::LOGICAL_ERROR,
            "RefTxnId: writer_epoch and ref_sequence must both be nonzero, got {}-{}",
            id.writer_epoch, id.ref_sequence);
    return getHexUIntLowercase(id.writer_epoch) + "-" + getHexUIntLowercase(id.ref_sequence);
}

/// Parses the canonical form only: exactly 33 characters, '-' at index 16, exactly 16 lower-case hex
/// digits ('0'-'9', 'a'-'f') either side, and both parsed fields nonzero. Any other shape -- short,
/// long, upper-case, non-hex, misplaced separator, or a zero component -- returns nullopt rather than
/// throwing, since parsing an untrusted listed key is an ordinary "is this ours" question.
inline std::optional<RefTxnId> parseRefTxnId(std::string_view s)
{
    constexpr size_t kFieldLen = 16;
    constexpr size_t kTotalLen = kFieldLen * 2 + 1;
    if (s.size() != kTotalLen || s[kFieldLen] != '-')
        return std::nullopt;

    /// Strict lower-case-hex-only parse: `unhexUInt` (base/hex.h) also accepts upper-case, which the
    /// canonical form must reject, so digits are validated and accumulated by hand here.
    const auto parseField = [](std::string_view field) -> std::optional<uint64_t>
    {
        uint64_t value = 0;
        for (char c : field)
        {
            uint64_t digit;
            if (c >= '0' && c <= '9')
                digit = static_cast<uint64_t>(c - '0');
            else if (c >= 'a' && c <= 'f')
                digit = static_cast<uint64_t>(c - 'a' + 10);
            else
                return std::nullopt;
            value = (value << 4) | digit;
        }
        return value;
    };

    const auto epoch = parseField(s.substr(0, kFieldLen));
    const auto seq = parseField(s.substr(kFieldLen + 1, kFieldLen));
    if (!epoch || !seq || *epoch == 0 || *seq == 0)
        return std::nullopt;
    return RefTxnId{*epoch, *seq};
}

}
