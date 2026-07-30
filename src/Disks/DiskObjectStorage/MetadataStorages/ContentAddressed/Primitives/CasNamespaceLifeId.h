#pragma once
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Primitives/CasTypes.h>
#include <Common/Exception.h>
#include <base/types.h>
#include <optional>
#include <string_view>
#include <utility>

namespace DB
{
namespace ErrorCodes
{
    extern const int LOGICAL_ERROR;
}
}

namespace DB::Cas
{

/// Builds the key segment for `incarnation`: 32 fixed-width lower-case hex digits, so the segment has
/// one canonical spelling and `<ns>/<inc>/` sorts stably. Throws LOGICAL_ERROR on a zero incarnation
/// for the same reason `renderRefTxnId` does: this render becomes an object key, and an invalid
/// identity must never silently produce a well-formed-looking one.
inline String renderIncarnation(const UInt128 & incarnation)
{
    if (incarnation == 0)
        throw DB::Exception(DB::ErrorCodes::LOGICAL_ERROR,
            "NamespaceLifeId: incarnation must be nonzero -- 0 never names a life");
    return u128ToHex(incarnation);
}

/// Inverse of `renderIncarnation` for ONE listed path segment. Accepts the canonical form only:
/// exactly 32 lower-case hex digits encoding a nonzero value. Upper case, a short or long segment,
/// non-hex characters and an all-zero segment all return `std::nullopt`; the CALLER decides whether
/// that is "not one of our keys" or corruption, because only the caller knows whether the rest of the
/// key already identified the object as ours.
inline std::optional<UInt128> parseIncarnation(std::string_view s)
{
    constexpr size_t kHexLen = 32;
    if (s.size() != kHexLen)
        return std::nullopt;

    /// `unhexUInt` also accepts upper case, which the canonical form must reject, so the digits are
    /// validated by hand first and only then handed to it.
    for (char c : s)
    {
        const bool canonical_digit = (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
        if (!canonical_digit)
            return std::nullopt;
    }

    const UInt128 value = unhexUInt<UInt128>(s.data());
    if (value == 0)
        return std::nullopt;
    return value;
}

class Layout;

/// The identity of ONE LIFE of a namespace's ref layer: the opaque namespace name plus the
/// incarnation minted for that life in the `ref_catalog` (spec INV-3). Every ref-layer key is built
/// from the PAIR -- `<prefix>/cas/refs/<ns>/<inc>/{_log,_snap,_cleanup,_ckpt}` -- so dropping and
/// recreating a namespace under the same name lands under a different prefix, and the previous life's
/// surviving objects are structurally inert to the new life instead of being aliased by it.
///
/// There is deliberately NO conversion from a bare `RootNamespace` and no default construction: code
/// holding only the name cannot name a ref object at all, so losing the incarnation is a compile
/// error rather than a runtime aliasing bug (spec §2 r9-3).
///
/// Exactly three places may mint one, and each has a name that says where its incarnation came from:
/// `fromCatalogEntry` (every discovery path -- recovery, fold, fsck, the sweeps), the reader-handle
/// factory Stage B Task 6 adds, and `Layout`'s key parsers (befriended below), which reconstruct the
/// pair a listed key spells. A parsed id is UNTRUSTED: a key can name any incarnation, including one
/// no longer in the catalog, which is precisely how a dead life is recognized and left alone.
struct NamespaceLifeId
{
    RootNamespace ns;
    UInt128 incarnation;   /// 0 is INVALID -- never a wildcard, never "any life"

    bool operator==(const NamespaceLifeId &) const = default;

    /// The catalog is the universe authority (INV-3): a discovery path learns of a namespace ONLY
    /// from a catalog entry, and takes BOTH fields from that same entry. Pairing a namespace with an
    /// incarnation obtained from anywhere else re-opens the rebirth alias this type exists to close.
    static NamespaceLifeId fromCatalogEntry(RootNamespace ns, const UInt128 & incarnation)
    {
        return NamespaceLifeId{std::move(ns), incarnation};
    }

    /// Stage B Task 6 adds the second permanent factory, `fromLiveHandle`: a live reader carries the
    /// id its table was opened under and never re-derives it, so the read side pays no catalog
    /// request (spec §2). It is absent here because the handle type does not exist yet.

    /// TRANSITIONAL -- Stage B Task 1 ONLY, and the ONLY way to reach a ref key from a bare
    /// namespace. Task 1 migrates every ref-layer key helper to this type before the catalog that
    /// mints real incarnations exists (Task 2) and before the reader paths carry handles (Task 6), so
    /// until then the whole tree mints keys at this one fixed incarnation and the layer stays
    /// self-consistent. Task 6 DELETES it, gated on a tree-wide grep for `stageATransition` finding
    /// no build inputs. Do not call it from new code.
    static NamespaceLifeId stageATransition(RootNamespace ns)
    {
        /// A fixed, obviously synthetic sentinel rather than a plausible-looking small number: its
        /// hex render spells `__STAGE_A_TRANS`, so a key minted during the transition is recognizable
        /// on sight in a bucket listing and cannot be mistaken for a minted incarnation.
        static constexpr UInt128 kTransitionIncarnation
            = (static_cast<UInt128>(0x5f5f'5354'4147'455fULL) << 64) | static_cast<UInt128>(0x415f'5452'414e'5301ULL);
        return NamespaceLifeId{std::move(ns), kTransitionIncarnation};
    }

private:
    /// `Layout`'s key parsers reconstruct the pair a listed key spells; they are the third minting
    /// site and the reason this constructor is reachable at all from outside the factories above.
    friend class Layout;

    NamespaceLifeId(RootNamespace ns_, const UInt128 & incarnation_)
        : ns(std::move(ns_)), incarnation(incarnation_)
    {
        if (incarnation_ == 0)
            throw DB::Exception(DB::ErrorCodes::LOGICAL_ERROR,
                "NamespaceLifeId: incarnation must be nonzero for namespace '{}' -- 0 never names a life",
                ns.string());
    }
};

}
