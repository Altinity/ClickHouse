#pragma once
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Primitives/CasTypes.h>
#include <base/types.h>
#include <cstdint>

namespace DB::Cas
{

using Dialect = TokenType;

/// The per-dialect grammar a response value must meet to be an incarnation. Generation: canonical
/// positive decimal (no leading zero, not "0" -- zero is the dialect's absence sentinel). ETag:
/// non-empty, not "*" after trimming whitespace, no comma (a list matches any member). Emulated:
/// non-empty. `ObjectStorageBackend::isValidTokenValue` forwards here.
bool isIncarnationValue(Dialect dialect, const String & value);

/// One backend-observed incarnation of an object: the transport's own token value together with the
/// backend and key it was observed against. Not default-constructible and not constructible from a
/// bare `String` -- a caller must go through a backend (or, in tests, `CasIncarnationTestAccess`) to
/// hold one, so an `Incarnation` is always traceable to the read or write that minted it.
class Incarnation
{
public:
    Incarnation() = delete;
    bool operator==(const Incarnation &) const = default;

    /// "etag:<value>" | "generation:<value>" | "emulated:<value>"
    String render() const;
    const String & key() const { return key_; }
    Dialect dialect() const { return dialect_; }
    uint64_t backendId() const { return backend_id_; }

private:
    friend class CasRequests;
    friend class CasIncarnationTestAccess;         /// tests only: gtest_cas_requests.cpp

    Incarnation(uint64_t backend_id, String key, Dialect dialect, String value)
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

inline String Incarnation::render() const
{
    switch (dialect_)
    {
        case Dialect::ETag: return "etag:" + value_;
        case Dialect::Generation: return "generation:" + value_;
        case Dialect::Emulated: return "emulated:" + value_;
    }
    return "unknown:" + value_;
}

/// An incarnation as recorded in a persisted manifest/ref: the dialect word and value, without any
/// live backend to check them against. `matches` re-derives the same rendering the live incarnation
/// would produce and compares it textually, so the two representations can never drift apart.
struct PersistedIncarnation
{
    String dialect;    /// "etag" | "generation" | "emulated"
    String value;

    bool matches(const Incarnation & live) const;
};

inline bool PersistedIncarnation::matches(const Incarnation & live) const
{
    return live.render() == dialect + ":" + value;
}

}
