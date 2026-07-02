#pragma once
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasBackend.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasEnvelope.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasLayout.h>
#include <base/extended_types.h>
#include <base/types.h>
#include <cstdint>
#include <map>
#include <optional>
#include <shared_mutex>
#include <utility>
#include <vector>

namespace DB::Cas
{

/// The writer-side retire view (spec §5, ack-floor redesign): the set of condemned (kind, hash, token)
/// incarnations published by GC, plus the round through which the view is current. On storage this is
/// gc/state (ABSENT object => round 0 — a pool GC never touched) and the per-gc-shard retired-list
/// objects it references via `retired_refs`. The refs and the round come out of the SAME gc/state body,
/// so the retired list can never be older than the round it is installed for. GC drops entries by
/// REWRITING those objects, so reading current storage state IS the protocol's view.
///
/// The publish gate (rules W-EVIDENCE / W-PUBLISH-GATE / W-REVALIDATE) consults this view to
/// decide whether a reused object is condemned.
///
/// TOKEN IDENTITY (pinned): token equality is Token::operator== — value AND type must both match.
/// The same value under a different TokenType is a DIFFERENT token.
///
/// Thread-safe: Store shares ONE view across its builds; publish gates read it concurrently,
/// fence conflicts refresh it.
class RetireView
{
public:
    RetireView(BackendPtr backend_, Layout layout_);

    /// GET gc/state, then GET each retired-list object it references via `retired_refs`. Rare by
    /// construction: called at Store::open, on each heartbeat beat that observes a newer round, and on
    /// fence-advanced publish conflicts only — never on per-object hot paths.
    void refresh();

    /// The GC round through which this view is current (0 = pool GC never touched).
    uint64_t round() const;

    /// All condemned tokens for (kind, hash) — multiple rounds may each hold one.
    std::optional<std::vector<Token>> findCondemned(ObjectKind kind, const UInt128 & hash) const;

    bool isCondemnedToken(ObjectKind kind, const UInt128 & hash, const Token & token) const;

private:
    const BackendPtr backend;
    const Layout layout;

    mutable std::shared_mutex mutex;
    uint64_t view_round = 0;
    std::map<std::pair<uint8_t, UInt128>, std::vector<Token>> condemned;   /// (kind, hash) -> tokens
};

}
