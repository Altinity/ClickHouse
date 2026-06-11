#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasGcSnap.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasCodecUtil.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasEnumStrings.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasIds.h>
#include <IO/WriteBufferFromString.h>
#include <Common/Exception.h>
#include <base/defines.h>

namespace DB
{
namespace ErrorCodes
{
    extern const int CORRUPTED_DATA;
    extern const int LOGICAL_ERROR;
}
}

namespace DB::Cas
{

namespace
{

constexpr uint64_t GC_SNAP_VERSION = 1;

/// `EdgeKind` <-> string. Unknown string on decode is corruption (fail closed).
std::string_view edgeKindToString(EdgeKind kind)
{
    switch (kind)
    {
        case EdgeKind::Root: return "root";
        case EdgeKind::Tree: return "tree";
        case EdgeKind::Pack: return "pack";
    }
    throw Exception(ErrorCodes::CORRUPTED_DATA, "CAS gc/snap: invalid edge kind {}", static_cast<int>(kind));
}

EdgeKind edgeKindFromString(std::string_view s, std::string_view what)
{
    if (s == "root")
        return EdgeKind::Root;
    if (s == "tree")
        return EdgeKind::Tree;
    if (s == "pack")
        return EdgeKind::Pack;
    throw Exception(ErrorCodes::CORRUPTED_DATA, "CAS {}: invalid edge kind '{}'", what, s);
}

/// A 32-hex hash from an already-extracted string (array elements, where requireHash's
/// object-key form does not apply); junk hex inside a persisted object is corruption.
UInt128 hashFromHex(const String & hex, std::string_view what)
{
    try
    {
        return hexToU128(hex);
    }
    catch (const Exception & e)
    {
        throw Exception(ErrorCodes::CORRUPTED_DATA, "CAS {}: not a valid hash ({})", what, e.message());
    }
}

}

uint64_t hashPrefixShard(const UInt128 & hash, uint64_t snap_shards)
{
    chassert(snap_shards >= 1);
    return static_cast<uint64_t>(hash) % snap_shards;            /// low 64 bits of the wide int
}

String GcSnap::rootEdgeId(const String & root_shard, const String & part_name)
{
    return "R|" + root_shard + "|" + part_name;
}

String GcSnap::edgeIdFor(const EdgeRec & rec)
{
    switch (rec.edge_kind)
    {
        case EdgeKind::Root:
            /// The root id deliberately omits the target: a (root_shard, part_name) names exactly
            /// one tree at a time. Re-pointing is removeRootEdge + addRootEdge OR a direct
            /// add-over-add (last-op-wins, handled in addRootEdge).
            return rootEdgeId(rec.root_shard, rec.part_name);
        case EdgeKind::Tree:
            return "T|" + u128ToHex(rec.parent_tree) + "|" + std::to_string(static_cast<int>(rec.target_kind))
                + "|" + u128ToHex(rec.target_hash);
        case EdgeKind::Pack:
            return "P|" + u128ToHex(rec.parent_tree) + "|" + u128ToHex(rec.target_hash);
    }
    throw Exception(ErrorCodes::CORRUPTED_DATA,
        "CAS gc/snap: invalid edge kind {}", static_cast<int>(rec.edge_kind));
}

void GcSnap::addEdge(EdgeRec rec)
{
    /// Set-semantics insert for edges whose canonical id INCLUDES the target (Tree/Pack): a
    /// duplicate id always carries the SAME target, so a different-target duplicate is impossible
    /// by construction and a duplicate add is a pure no-op. Root edges (target NOT in the id)
    /// have last-op-wins semantics per spec §7 and are handled in addRootEdge, not here.
    const String id = edgeIdFor(rec);
    const NodeKey target{static_cast<uint8_t>(rec.target_kind), rec.target_hash};
    const auto [it, inserted] = edges.try_emplace(id, std::move(rec));
    known.insert(target);                                        /// known even on duplicate add
    if (inserted)
        ++indeg[target];
}

std::vector<Candidate> GcSnap::addRootEdge(const String & root_shard, const String & part_name, const UInt128 & tree)
{
    /// Last-op-wins (spec §7): `Build::publish` legally re-publishes an existing ref with a NEW
    /// tree (consecutive journal `Add`s for the same ref, no `Remove` between). Keeping the FIRST
    /// record would leave the LIVE tree with in-degree 0 — an under-count that zeroInDegreeKnown
    /// would surface as a retire candidate (INV-NO-LOSS violation). So an add over an existing
    /// (root_shard, part_name) edge with a different target re-points it: the old target drops
    /// one in-degree (and may become a candidate), the new target gains one.
    std::vector<Candidate> result;
    const NodeKey target{static_cast<uint8_t>(ObjectKind::Tree), tree};
    known.insert(target);                                        /// known even on duplicate add
    const auto it = edges.find(rootEdgeId(root_shard, part_name));
    if (it != edges.end())
    {
        if (it->second.target_hash == tree)
            return result;                                       /// same-target duplicate: no-op
        if (const auto candidate = dropEdgeTarget(it->second))
            result.push_back(*candidate);
        it->second.target_hash = tree;
        ++indeg[target];
        return result;
    }
    EdgeRec rec;
    rec.edge_kind = EdgeKind::Root;
    rec.target_kind = ObjectKind::Tree;
    rec.target_hash = tree;
    rec.root_shard = root_shard;
    rec.part_name = part_name;
    edges.emplace(rootEdgeId(root_shard, part_name), std::move(rec));
    ++indeg[target];
    return result;
}

void GcSnap::addTreeEdge(const UInt128 & parent_tree, ObjectKind child_kind, const UInt128 & child_hash)
{
    EdgeRec rec;
    rec.edge_kind = EdgeKind::Tree;
    rec.target_kind = child_kind;
    rec.target_hash = child_hash;
    rec.parent_tree = parent_tree;
    addEdge(std::move(rec));
}

void GcSnap::addPackEdge(const UInt128 & parent_tree, const UInt128 & pack_hash)
{
    EdgeRec rec;
    rec.edge_kind = EdgeKind::Pack;
    rec.target_kind = ObjectKind::Pack;
    rec.target_hash = pack_hash;
    rec.parent_tree = parent_tree;
    addEdge(std::move(rec));
}

std::optional<Candidate> GcSnap::dropEdgeTarget(const EdgeRec & rec)
{
    const NodeKey target{static_cast<uint8_t>(rec.target_kind), rec.target_hash};
    const auto it = indeg.find(target);
    /// edges and indeg are kept in sync; this guards a delete decision, so a desync must fail
    /// loud in release builds too — never proceed on inconsistent GC state (fail closed).
    if (it == indeg.end() || it->second == 0)
        throw Exception(ErrorCodes::LOGICAL_ERROR,
            "CAS gc/snap: in-degree desync for edge target {} (kind {})",
            u128ToHex(rec.target_hash), static_cast<int>(rec.target_kind));
    if (--it->second > 0)
        return std::nullopt;
    indeg.erase(it);                                             /// indeg never stores a zero count
    /// Every edge target is in `known` (addEdge inserts it), so the transition to 0 always yields
    /// a candidate; the check is belt-and-braces against a future known-trimming operation.
    if (!known.contains(target))
        return std::nullopt;
    return Candidate{rec.target_kind, rec.target_hash};
}

std::vector<Candidate> GcSnap::removeRootEdge(const String & root_shard, const String & part_name)
{
    std::vector<Candidate> result;
    const auto it = edges.find(rootEdgeId(root_shard, part_name));
    if (it == edges.end())
        return result;                                           /// missing remove: no-op (replay)
    if (const auto candidate = dropEdgeTarget(it->second))
        result.push_back(*candidate);
    edges.erase(it);
    return result;
}

std::vector<Candidate> GcSnap::stripTree(const UInt128 & parent_tree)
{
    std::vector<Candidate> result;
    for (auto it = edges.begin(); it != edges.end();)
    {
        if (it->second.edge_kind != EdgeKind::Root && it->second.parent_tree == parent_tree)
        {
            if (const auto candidate = dropEdgeTarget(it->second))
                result.push_back(*candidate);
            it = edges.erase(it);
        }
        else
            ++it;
    }
    expanded.erase(parent_tree);
    return result;
}

void GcSnap::markExpanded(const UInt128 & tree)
{
    expanded.insert(tree);
}

bool GcSnap::isExpanded(const UInt128 & tree) const
{
    return expanded.contains(tree);
}

uint64_t GcSnap::inDegree(ObjectKind kind, const UInt128 & hash) const
{
    const auto it = indeg.find(NodeKey{static_cast<uint8_t>(kind), hash});
    return it == indeg.end() ? 0 : it->second;
}

bool GcSnap::isKnown(ObjectKind kind, const UInt128 & hash) const
{
    return known.contains(NodeKey{static_cast<uint8_t>(kind), hash});
}

std::vector<Candidate> GcSnap::zeroInDegreeKnown() const
{
    /// indeg holds only nonzero counts, so "zero in-degree" == "no indeg entry".
    std::vector<Candidate> result;
    for (const auto & [kind, hash] : known)
        if (!indeg.contains(NodeKey{kind, hash}))
            result.push_back(Candidate{static_cast<ObjectKind>(kind), hash});
    return result;
}

String encodeGcSnap(const GcSnap & snap)
{
    WriteBufferFromOwnString out;
    writeCString("{", out);
    writeJsonKey(out, "format");
    writeJsonString("cas_gc_snap", out);
    writeChar(',', out);
    writeJsonKey(out, "version");
    writeIntText(GC_SNAP_VERSION, out);
    writeChar(',', out);
    writeJsonKey(out, "snap_shard");
    writeIntText(snap.snap_shard, out);
    writeChar(',', out);
    writeJsonKey(out, "generation");
    writeIntText(snap.generation, out);
    writeChar(',', out);
    writeJsonKey(out, "edges");
    writeChar('[', out);
    bool first = true;
    for (const auto & [id, rec] : snap.edges)                    /// canonical-edge-id order
    {
        if (!first)
            writeChar(',', out);
        first = false;

        writeChar('{', out);
        writeJsonKey(out, "edge_kind");
        writeJsonString(edgeKindToString(rec.edge_kind), out);
        writeChar(',', out);
        if (rec.edge_kind == EdgeKind::Root)
        {
            writeJsonKey(out, "root_shard");
            writeJsonString(rec.root_shard, out);
            writeChar(',', out);
            writeJsonKey(out, "part_name");
            writeJsonString(rec.part_name, out);
        }
        else
        {
            writeJsonKey(out, "parent_tree");
            writeJsonString(u128ToHex(rec.parent_tree), out);
        }
        writeChar(',', out);
        writeJsonKey(out, "target_kind");
        writeJsonString(objectKindToString(rec.target_kind), out);
        writeChar(',', out);
        writeJsonKey(out, "target_hash");
        writeJsonString(u128ToHex(rec.target_hash), out);
        writeChar('}', out);
    }
    writeChar(']', out);
    writeChar(',', out);
    writeJsonKey(out, "expanded");
    writeChar('[', out);
    first = true;
    for (const auto & tree : snap.expanded)                      /// sorted hex (set order)
    {
        if (!first)
            writeChar(',', out);
        first = false;
        writeJsonString(u128ToHex(tree), out);
    }
    writeChar(']', out);
    writeChar(',', out);
    writeJsonKey(out, "known");
    writeChar('[', out);
    first = true;
    for (const auto & [kind, hash] : snap.known)                 /// sorted by (kind, hash)
    {
        if (!first)
            writeChar(',', out);
        first = false;
        writeChar('{', out);
        writeJsonKey(out, "kind");
        writeJsonString(objectKindToString(static_cast<ObjectKind>(kind)), out);
        writeChar(',', out);
        writeJsonKey(out, "hash");
        writeJsonString(u128ToHex(hash), out);
        writeChar('}', out);
    }
    writeChar(']', out);
    writeChar('}', out);
    return std::move(out.str());
}

GcSnap decodeGcSnap(std::string_view data)
{
    return decodeJsonGuarded("gc/snap", [&]
    {
        auto obj = parseJsonDocument(data, "cas_gc_snap", GC_SNAP_VERSION, "gc/snap");
        checkNoUnknownKeys(*obj,
            {"format", "version", "snap_shard", "generation", "edges", "expanded", "known"}, "gc/snap");

        GcSnap snap;
        snap.snap_shard = requireU64(*obj, "snap_shard", "gc/snap");
        snap.generation = requireU64(*obj, "generation", "gc/snap");

        auto edges = requireArray(*obj, "edges", "gc/snap");
        for (size_t i = 0; i < edges->size(); ++i)
        {
            auto edge_obj = requireObjectAt(*edges, i, "gc/snap edges");

            GcSnap::EdgeRec rec;
            rec.edge_kind = edgeKindFromString(requireString(*edge_obj, "edge_kind", "gc/snap edge"), "gc/snap edge");
            switch (rec.edge_kind)
            {
                case EdgeKind::Root:
                {
                    checkNoUnknownKeys(*edge_obj,
                        {"edge_kind", "root_shard", "part_name", "target_kind", "target_hash"}, "gc/snap root edge");
                    rec.root_shard = requireString(*edge_obj, "root_shard", "gc/snap root edge");
                    rec.part_name = requireString(*edge_obj, "part_name", "gc/snap root edge");
                    break;
                }
                case EdgeKind::Tree:
                {
                    checkNoUnknownKeys(*edge_obj,
                        {"edge_kind", "parent_tree", "target_kind", "target_hash"}, "gc/snap tree edge");
                    rec.parent_tree = requireHash(*edge_obj, "parent_tree", "gc/snap tree edge");
                    break;
                }
                case EdgeKind::Pack:
                {
                    checkNoUnknownKeys(*edge_obj,
                        {"edge_kind", "parent_tree", "target_kind", "target_hash"}, "gc/snap pack edge");
                    rec.parent_tree = requireHash(*edge_obj, "parent_tree", "gc/snap pack edge");
                    break;
                }
            }
            rec.target_kind = objectKindFromString(requireString(*edge_obj, "target_kind", "gc/snap edge"), "gc/snap edge");
            rec.target_hash = requireHash(*edge_obj, "target_hash", "gc/snap edge");
            if (rec.edge_kind == EdgeKind::Pack && rec.target_kind != ObjectKind::Pack)
                throw Exception(ErrorCodes::CORRUPTED_DATA,
                    "CAS gc/snap: pack edge target_kind must be 'pack', got '{}'",
                    objectKindToString(rec.target_kind));

            /// The canonical encoder iterates the edge map, so it can never emit two edges with
            /// the same canonical id — ANY duplicate in a persisted document is corruption (and
            /// silently deduplicating a root-edge duplicate would pick an arbitrary target).
            const String id = GcSnap::edgeIdFor(rec);
            if (snap.edges.contains(id))
                throw Exception(ErrorCodes::CORRUPTED_DATA, "CAS gc/snap: duplicate edge id '{}'", id);
            snap.addEdge(std::move(rec));                        /// rebuilds indeg (and seeds known)
        }

        auto expanded = requireArray(*obj, "expanded", "gc/snap");
        for (size_t i = 0; i < expanded->size(); ++i)
        {
            const auto var = expanded->get(static_cast<unsigned int>(i));
            if (var.type() != typeid(String))
                throw Exception(ErrorCodes::CORRUPTED_DATA,
                    "CAS gc/snap: expanded element {} must be a string", i);
            snap.expanded.insert(hashFromHex(var.extract<String>(), "gc/snap expanded"));
        }

        /// The explicit known array is authoritative; the edge-derived inserts from addEdge above
        /// are a subset of it for any document we encoded (a known node may have zero edges — that
        /// is exactly the zero-in-degree candidate case). Union both (simple and over-count-safe).
        auto known = requireArray(*obj, "known", "gc/snap");
        for (size_t i = 0; i < known->size(); ++i)
        {
            auto known_obj = requireObjectAt(*known, i, "gc/snap known");
            checkNoUnknownKeys(*known_obj, {"kind", "hash"}, "gc/snap known entry");
            const ObjectKind kind
                = objectKindFromString(requireString(*known_obj, "kind", "gc/snap known"), "gc/snap known");
            snap.known.insert(GcSnap::NodeKey{
                static_cast<uint8_t>(kind), requireHash(*known_obj, "hash", "gc/snap known")});
        }

        return snap;
    });
}

}
