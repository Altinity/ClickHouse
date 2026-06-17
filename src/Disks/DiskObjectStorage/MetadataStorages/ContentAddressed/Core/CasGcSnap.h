#pragma once
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasEnvelope.h>
#include <base/types.h>
#include <base/extended_types.h>
#include <map>
#include <optional>
#include <set>
#include <string_view>
#include <vector>

namespace DB
{
class ReadBuffer;
}

namespace DB::Cas
{

/// The GC in-degree snapshot (spec §4/§7): present-edge sets + tree expansion markers + the
/// journal-known set, per (generation, snap_shard). Sharded by the TARGET content-hash prefix —
/// every edge targeting a node lands in the node's own shard, so in-degree is intra-shard.
/// Model mapping (CaIncarnationCore.tla): edges ↔ rootEdges/treeEdges, expanded ↔ marker,
/// known ↔ everEdged, inDegree ↔ InDeg, zeroInDegreeKnown ↔ the GRetire guard's candidate set
/// (STATELESS: derived from durable state, never from in-memory fold transitions — crash-replay).
/// Set semantics throughout: duplicate adds and missing removes are no-ops (idempotent replay;
/// GC state may over-count, must never under-count — INV-OVER-COUNT-ONLY). Root edges are
/// last-op-wins (spec §7): an add over an existing (root_shard, part_name) edge re-points it.

enum class EdgeKind : uint8_t
{
    Root = 1,
    Tree = 2,
    Pack = 3,
};

struct Candidate
{
    ObjectKind kind = ObjectKind::Blob;
    UInt128 hash{};
};

/// snap_shard of a node = low 64 bits of its hash, mod snap_shards (snap_shards >= 1).
uint64_t hashPrefixShard(const UInt128 & hash, uint64_t snap_shards);

class GcSnap
{
public:
    uint64_t snap_shard = 0;
    uint64_t generation = 0;

    /// Last-op-wins (spec §7): the canonical root-edge id is (root_shard, part_name) — the target
    /// is NOT part of the id — and `Build::publish` may legally re-publish an existing ref with a
    /// NEW tree (consecutive journal `Add`s, no `Remove` between). Adding over an existing edge
    /// with a different target re-points it; returns the displaced OLD target as a candidate if
    /// its in-degree transitioned to 0. A same-target duplicate add is a no-op (empty result).
    std::vector<Candidate> addRootEdge(const String & root_shard, const String & part_name, const UInt128 & tree);
    void addTreeEdge(const UInt128 & parent_tree, ObjectKind child_kind, const UInt128 & child_hash);
    void addPackEdge(const UInt128 & parent_tree, const UInt128 & pack_hash);

    /// Removes the (root_shard, part_name) edge if present; returns the target as a candidate if its
    /// in-degree transitioned to 0. Missing edge => no-op (idempotent replay).
    std::vector<Candidate> removeRootEdge(const String & root_shard, const String & part_name);

    /// Strips ALL edges sourced from parent_tree (cascade) and clears its expansion marker;
    /// returns the children whose in-degree transitioned to 0. Idempotent.
    std::vector<Candidate> stripTree(const UInt128 & parent_tree);

    /// Remove a node from `known` (the inverse of addEdge's known.insert). Set semantics: forgetting
    /// a node not in `known` is a no-op (idempotent crash-replay). Edges/markers are untouched — a
    /// node is only forgotten when its in-degree is already 0, so it has no incoming edge; a later
    /// folded Add re-inserts it via addEdge. P9: keeps `known` from growing past live nodes so the
    /// retire observe loop stops re-HEAD-404ing already-deleted candidates (model action GForget).
    void forget(ObjectKind kind, const UInt128 & hash);

    void markExpanded(const UInt128 & tree);
    bool isExpanded(const UInt128 & tree) const;

    uint64_t inDegree(ObjectKind kind, const UInt128 & hash) const;
    bool isKnown(ObjectKind kind, const UInt128 & hash) const;

    /// ALL known nodes with zero in-degree — the model's stateless GRetire candidate set, derived
    /// from durable state (a freshly decoded snap reports the same as the one that mutated).
    std::vector<Candidate> zeroInDegreeKnown() const;

private:
    struct EdgeRec
    {
        EdgeKind edge_kind = EdgeKind::Root;
        ObjectKind target_kind = ObjectKind::Blob;
        UInt128 target_hash{};
        String root_shard;        /// Root only
        String part_name;         /// Root only
        UInt128 parent_tree{};    /// Tree/Pack only
    };
    using NodeKey = std::pair<uint8_t, UInt128>;

    static String edgeIdFor(const EdgeRec & rec);
    static String rootEdgeId(const String & root_shard, const String & part_name);
    void addEdge(EdgeRec rec);                                   /// shared set-semantics insert

    /// Decrements the in-degree of rec's target (the edge itself is erased by the caller);
    /// returns the target as a candidate iff its in-degree transitioned to 0.
    std::optional<Candidate> dropEdgeTarget(const EdgeRec & rec);

    std::map<String, EdgeRec> edges;                             /// canonical edge id -> record
    std::set<UInt128> expanded;
    std::set<NodeKey> known;
    /// Derived from edges, kept in sync. INVARIANT: indeg never stores a zero count — an entry is
    /// erased the moment it reaches 0, so zeroInDegreeKnown is "known minus indeg keys".
    std::map<NodeKey, uint64_t> indeg;

    friend String encodeGcSnap(const GcSnap &);
    friend GcSnap decodeGcSnap(std::string_view);

    /// Codec helpers: encode/decode the raw field bytes (without the frame header).
    static String encodeSnapFields(const GcSnap & snap);
    static GcSnap decodeSnapFields(ReadBuffer & body);
};

String encodeGcSnap(const GcSnap & snap);
GcSnap decodeGcSnap(std::string_view data);

}
