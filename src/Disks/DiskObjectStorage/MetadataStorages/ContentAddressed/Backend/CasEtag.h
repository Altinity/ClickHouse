#pragma once
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Primitives/CasTypes.h>
#include <base/defines.h>
#include <base/types.h>
#include <cstdint>
#include <optional>
#include <string_view>

namespace DB::Cas
{

/// Two grammar levels. `isIncarnationValue` is the control-key grammar: what a response value must meet to
/// be usable as a precondition at all (a weak ETag passes). `canonicalIdentity` is the stricter blob-identity
/// grammar: what may stand for one incarnation of a blob body, spelled one way.
///
/// `isIncarnationValue` per dialect. Generation: canonical
/// positive decimal (no leading zero, not "0" -- zero is the dialect's absence sentinel). ETag:
/// non-empty, not "*" after trimming whitespace, no comma (a list matches any member). Emulated:
/// non-empty. `ObjectStorageBackend::isValidTokenValue` forwards here.
bool isIncarnationValue(Dialect dialect, const String & value);

/// An incarnation in one spelling: the dialect and the opaque tag in lowercase hex. A quoted and a bare
/// ETag of the same tag are equal; no case folding.
struct CanonicalIdentity
{
    Dialect dialect;
    String tag_hex;
    bool operator==(const CanonicalIdentity &) const = default;
};

std::string_view dialectWord(Dialect dialect);   /// "etag" | "generation" | "emulated"
std::optional<Dialect> dialectFromWord(std::string_view word);

/// Nothing for a value that cannot name a blob incarnation (weak, empty, wildcard, list, control bytes,
/// over 128 bytes; a generation outside `[1-9][0-9]{0,19}`).
std::optional<CanonicalIdentity> canonicalIdentity(Dialect dialect, std::string_view raw_value);

/// Inverse of the `tag_hex` spelling, for key parsing: the opaque tag, or nothing when `tag_hex` is not
/// lowercase even-length hex of an opaque tag. It checks the tag, not a wire spelling: `"W/x"` is a strong
/// ETag whose tag `W/x` must parse back.
std::optional<String> opaqueTagFromHex(Dialect dialect, std::string_view tag_hex);

/// One backend-observed incarnation of an object: the transport's own value naming that incarnation,
/// together with the backend and key it was observed against. "Etag" names the ROLE this value plays
/// -- whatever the backend hands back as the object's identity for a conditional write -- not the wire
/// field: it is a literal ETag on S3-compatible stores, a generation number on GCS's JSON dialect, and
/// a minted sequence value on the emulated/in-memory backends. Not default-constructible, not
/// constructible from a bare `String`, and minted ONLY by `CasRequests` -- a caller can hold one only
/// by way of an admitted read or write, so an `Etag` is always traceable to the request that produced
/// it.
class Etag
{
public:
    Etag() = delete;
    /// A write's answer and a read's answer may spell one version differently: compare with `sameVersion`, or with
    /// `identicalToken` when the spelling cannot differ.
    bool operator==(const Etag &) const = delete;
    bool operator!=(const Etag &) const = delete;

    /// "etag:<value>" | "generation:<value>" | "emulated:<value>"
    String render() const;
    const String & key() const { return key_; }
    Dialect dialect() const { return dialect_; }
    uint64_t backendId() const { return backend_id_; }
    std::optional<CanonicalIdentity> canonical() const;

private:
    friend class CasRequests;
    friend bool identicalToken(const Etag & a, const Etag & b);

    Etag(uint64_t backend_id, String key, Dialect dialect, String value)
        : backend_id_(backend_id), key_(std::move(key)), dialect_(dialect), value_(std::move(value))
    {
    }

    /// The transport's own text; CasRequests reads it to build the next conditional request.
    const String & value() const { return value_; }

    uint64_t backend_id_;
    String key_;
    Dialect dialect_;
    String value_;
};

inline String Etag::render() const
{
    return String(dialectWord(dialect_)) + ":" + value_;
}

/// An incarnation as recorded in a persisted manifest/ref: the dialect word and value, without any
/// live backend to check them against. Forward-only: a `PersistedEtag` is captured FROM a live
/// `Etag`, never the reverse -- a persisted record must never be trusted to mint a live one.
/// `matches` re-derives the same rendering the live incarnation would produce and compares it
/// textually, so the two representations can never drift apart.
struct PersistedEtag
{
    String dialect;    /// "etag" | "generation" | "emulated"
    String value;

    static PersistedEtag capture(const Etag & live);
    bool matches(const Etag & live) const;
    /// Nothing for an unknown dialect word or a value that is not an identity.
    std::optional<CanonicalIdentity> canonical() const;
};

/// True when both spellings canonicalise and are equal.
bool sameIncarnation(const PersistedEtag & row, const Etag & live);

/// Whether `read`, from a read of an object, names the version a conditional write of it answered with as `written`:
/// equal tokens, or equal canonical identities, because the two responses may quote one ETag differently. A token
/// with no canonical form matches only exactly. Throws `LOGICAL_ERROR` for tokens of two keys or two backends: equal
/// bytes under two keys share an S3 ETag.
bool sameVersion(const Etag & read, const Etag & written);

/// The same backend, key and transport text. For two tokens from responses of one kind, or a token and its own copy,
/// which spell one version the same way.
bool identicalToken(const Etag & a, const Etag & b);
/// Both absent, or both present and identical.
bool identicalToken(const std::optional<Etag> & a, const std::optional<Etag> & b);

/// A token whose `canonical` is `identity`, in wire spelling: a quoted tag except for `generation`. Nothing when
/// `tag_hex` names no opaque tag.
std::optional<PersistedEtag> persistedTokenOf(const CanonicalIdentity & identity);

inline PersistedEtag PersistedEtag::capture(const Etag & live)
{
    const String rendered = live.render();
    const auto colon = rendered.find(':');
    return PersistedEtag{rendered.substr(0, colon), rendered.substr(colon + 1)};
}

inline bool PersistedEtag::matches(const Etag & live) const
{
    return live.render() == dialect + ":" + value;
}

}
