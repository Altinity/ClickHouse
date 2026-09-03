#pragma once
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Backend/CasIncarnation.h>
/// `throwCasWriteRetryLater` / `throwCasTransientUnavailable` are declared here today; the lock moves
/// them into `CasRequests.h` alongside the rest of this contract.
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Backend/CasRequestControl.h>
#include <Common/Exception.h>
#include <base/defines.h>

#include <fmt/format.h>

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <variant>

namespace DB::ErrorCodes
{
    extern const int ABORTED;
}

namespace DB::Cas
{

struct Object { String bytes; Incarnation incarnation; };
struct Meta   { uint64_t size; Incarnation incarnation; };
enum class Removal : uint8_t { Removed, Gone, Mismatch };

/// What a write attempt observed of the key's current state before giving up, so a caller (or the
/// message built by `orThrow`) can report exactly what was seen instead of just that something failed.
struct NotObserved {};
struct ProvenAbsent {};
using Observation = std::variant<NotObserved, ProvenAbsent, Meta, Object>;

/// A durable write landed: `incarnation` names the incarnation it created (or, for a retried write
/// resolved by a read, the incarnation already present), `attempts_sent` counts the HTTP attempts this
/// call made, and `resolved_by_read` is true when the commit was proven by a read rather than by the
/// attempt's own response.
struct Committed { Incarnation incarnation; uint32_t attempts_sent; bool resolved_by_read; };
/// The write was never attempted or never needed -- e.g. `putIfAbsent` finding the key already
/// present under the caller's intended content. `seen` is whatever the resolve read observed.
struct Declined  { Observation seen; };
/// A competing write won: the key's current state does not match what this call expected.
struct Conflict  { Observation seen; };
/// The store itself refused the request (not a lost precondition) -- `store_error` is a ClickHouse
/// error code and `message` explains it.
struct Refused   { int store_error; String message; };
/// No attempt landed and none can be proven safe to keep making.
struct GaveUp
{
    enum class Why : uint8_t { Deadline, FenceLost, Unresolved };
    enum class Source : uint8_t { Policy, Lease };
    Why why; Source deadline_source; bool sent_any; Observation last_seen;
    /// The HTTP attempts this call made, the same count `Committed` carries. Operator counters -- the
    /// mount renewal's attempt and retry counters among them -- have to count the attempts of a write
    /// that GAVE UP as well as of one that committed, and `sent_any` cannot say how many. It stays
    /// beside this because the readers that only branch on "was anything sent" branch on it by name.
    uint32_t attempts_sent = 0;
};
using WriteResult = std::variant<Committed, Declined, Conflict, Refused, GaveUp>;

namespace detail
{

/// Helper for std::visit with multiple lambdas; no shared one exists in the tree yet.
template <typename... Ts>
struct Overload : Ts...
{
    using Ts::operator()...;
};
template <typename... Ts>
Overload(Ts...) -> Overload<Ts...>;

inline String renderObservation(const Observation & seen)
{
    return std::visit(Overload{
        [](const NotObserved &) -> String { return "nothing observed"; },
        [](const ProvenAbsent &) -> String { return "absent"; },
        [](const Meta &) -> String { return "present (meta)"; },
        [](const Object & o) -> String { return "present (" + o.incarnation.render() + ")"; }}, seen);
}

}

/// Collapse a `WriteResult` into the incarnation a caller can act on: `nullopt` for a declined write
/// (nothing changed, nothing to report), the committed incarnation otherwise -- or throw, mapping
/// every non-success alternative to the error class its meaning already implies. `what` names the
/// call for the thrown message.
inline std::optional<Incarnation> orThrow(WriteResult && result, std::string_view what)
{
    using detail::Overload;
    using detail::renderObservation;
    return std::visit(Overload{
        [](Committed & c) -> std::optional<Incarnation> { return std::move(c.incarnation); },
        [](Declined &) -> std::optional<Incarnation> { return std::nullopt; },
        [&](Conflict & c) -> std::optional<Incarnation>
        {
            throw Exception(ErrorCodes::ABORTED, "{}: conflict, observed {}", what, renderObservation(c.seen));
        },
        [&](Refused & r) -> std::optional<Incarnation>
        {
            throw Exception(r.store_error, "{}: the store refused the write: {}", what, r.message);
        },
        [&](GaveUp & g) -> std::optional<Incarnation>
        {
            switch (g.why)
            {
                case GaveUp::Why::FenceLost:
                    throwCasTransientUnavailable(String(what), "mount fence tripped: the durable write is refused because this node no longer holds the mount incarnation it was admitted under");
                case GaveUp::Why::Deadline:
                    throwCasWriteRetryLater(fmt::format("{}: gave up at the {} deadline after {} attempt(s)", what, g.deadline_source == GaveUp::Source::Lease ? "lease" : "policy", g.sent_any ? "one or more" : "zero"));
                case GaveUp::Why::Unresolved:
                    throwCasWriteRetryLater(fmt::format("{}: the write is unresolved (sent, resolve read found {})", what, renderObservation(g.last_seen)));
            }
            UNREACHABLE();
        }}, result);
}

}
