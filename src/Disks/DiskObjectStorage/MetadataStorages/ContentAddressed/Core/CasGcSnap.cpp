#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasGcSnap.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasCodecUtil.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasIds.h>
#include <IO/ReadBufferFromMemory.h>
#include <IO/WriteBufferFromString.h>
#include <Common/Exception.h>
#include <base/defines.h>

namespace DB
{
namespace ErrorCodes
{
    extern const int CORRUPTED_DATA;
    extern const int NOT_IMPLEMENTED;
    extern const int LOGICAL_ERROR;
}
}

namespace DB::Cas
{

namespace
{

/// gc/snap is a non-hashed metadata object, but unlike the human-inspected operational metadata it
/// is the GC round's serialization hot path: it holds the WHOLE pool's reachability state and is
/// parsed/serialized every round. So it is encoded BINARY (the codebase's standard IO-helper idiom,
/// same as the hashed tree/envelope codecs) rather than JSON — the parse/serialize CPU dominated
/// the GC round. Magic + version byte at the front; fail-closed decode (bad magic / bad enum byte /
/// truncated => CORRUPTED_DATA; future version => NOT_IMPLEMENTED).
constexpr char GC_SNAP_MAGIC[4] = {'C', 'A', 'G', 'S'};
constexpr uint8_t GC_SNAP_VERSION = 2;

/// `EdgeKind` <-> byte. Unknown byte on decode is corruption (fail closed).
uint8_t edgeKindToByte(EdgeKind kind)
{
    return static_cast<uint8_t>(kind);
}

EdgeKind edgeKindFromByte(uint8_t b)
{
    switch (b)
    {
        case static_cast<uint8_t>(EdgeKind::Root): return EdgeKind::Root;
        case static_cast<uint8_t>(EdgeKind::Tree): return EdgeKind::Tree;
        case static_cast<uint8_t>(EdgeKind::Pack): return EdgeKind::Pack;
    }
    throw Exception(ErrorCodes::CORRUPTED_DATA, "CAS gc/snap: invalid edge kind byte {}", static_cast<int>(b));
}

ObjectKind objectKindFromByte(uint8_t b)
{
    switch (b)
    {
        case static_cast<uint8_t>(ObjectKind::Blob): return ObjectKind::Blob;
        case static_cast<uint8_t>(ObjectKind::Tree): return ObjectKind::Tree;
        case static_cast<uint8_t>(ObjectKind::Pack): return ObjectKind::Pack;
    }
    throw Exception(ErrorCodes::CORRUPTED_DATA, "CAS gc/snap: invalid object kind byte {}", static_cast<int>(b));
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
    out.write(GC_SNAP_MAGIC, sizeof(GC_SNAP_MAGIC));
    writeBinaryLittleEndian(GC_SNAP_VERSION, out);
    writeBinaryLittleEndian(snap.snap_shard, out);
    writeBinaryLittleEndian(snap.generation, out);

    writeBinaryLittleEndian(static_cast<uint32_t>(snap.edges.size()), out);
    for (const auto & [id, rec] : snap.edges)                    /// canonical-edge-id order
    {
        writeBinaryLittleEndian(edgeKindToByte(rec.edge_kind), out);
        if (rec.edge_kind == EdgeKind::Root)
        {
            writeBinaryLittleEndian(static_cast<uint16_t>(rec.root_shard.size()), out);
            writeString(rec.root_shard, out);
            writeBinaryLittleEndian(static_cast<uint16_t>(rec.part_name.size()), out);
            writeString(rec.part_name, out);
        }
        else
        {
            writeBinaryLittleEndian(rec.parent_tree, out);
        }
        writeBinaryLittleEndian(static_cast<uint8_t>(rec.target_kind), out);
        writeBinaryLittleEndian(rec.target_hash, out);
    }

    writeBinaryLittleEndian(static_cast<uint32_t>(snap.expanded.size()), out);
    for (const auto & tree : snap.expanded)                      /// set order (sorted)
        writeBinaryLittleEndian(tree, out);

    writeBinaryLittleEndian(static_cast<uint32_t>(snap.known.size()), out);
    for (const auto & [kind, hash] : snap.known)                 /// set order (sorted by (kind, hash))
    {
        writeBinaryLittleEndian(kind, out);
        writeBinaryLittleEndian(hash, out);
    }

    return std::move(out.str());
}

GcSnap decodeGcSnap(std::string_view data)
{
    return decodeGuarded("gc/snap", [&]
    {
        ReadBufferFromMemory in(data.data(), data.size());

        if (readFixedBytes(in, sizeof(GC_SNAP_MAGIC)) != std::string_view(GC_SNAP_MAGIC, sizeof(GC_SNAP_MAGIC)))
            throw Exception(ErrorCodes::CORRUPTED_DATA, "CAS gc/snap: bad magic");

        uint8_t version = 0;
        readBinaryLittleEndian(version, in);
        if (version > GC_SNAP_VERSION)
            throw Exception(ErrorCodes::NOT_IMPLEMENTED, "CAS gc/snap: unsupported version {}", version);
        if (version != GC_SNAP_VERSION)
            throw Exception(ErrorCodes::CORRUPTED_DATA, "CAS gc/snap: invalid version {}", version);

        GcSnap snap;
        readBinaryLittleEndian(snap.snap_shard, in);
        readBinaryLittleEndian(snap.generation, in);

        uint32_t edge_count = 0;
        readBinaryLittleEndian(edge_count, in);
        for (uint32_t i = 0; i < edge_count; ++i)
        {
            GcSnap::EdgeRec rec;
            uint8_t edge_kind_byte = 0;
            readBinaryLittleEndian(edge_kind_byte, in);
            rec.edge_kind = edgeKindFromByte(edge_kind_byte);
            if (rec.edge_kind == EdgeKind::Root)
            {
                uint16_t shard_len = 0;
                readBinaryLittleEndian(shard_len, in);
                rec.root_shard = readFixedBytes(in, shard_len);
                uint16_t part_len = 0;
                readBinaryLittleEndian(part_len, in);
                rec.part_name = readFixedBytes(in, part_len);
            }
            else
            {
                readBinaryLittleEndian(rec.parent_tree, in);
            }
            uint8_t target_kind_byte = 0;
            readBinaryLittleEndian(target_kind_byte, in);
            rec.target_kind = objectKindFromByte(target_kind_byte);
            readBinaryLittleEndian(rec.target_hash, in);

            if (rec.edge_kind == EdgeKind::Pack && rec.target_kind != ObjectKind::Pack)
                throw Exception(ErrorCodes::CORRUPTED_DATA,
                    "CAS gc/snap: pack edge target_kind must be 'pack', got {}",
                    static_cast<int>(rec.target_kind));

            /// The canonical encoder iterates the edge map, so it can never emit two edges with
            /// the same canonical id — ANY duplicate in a persisted document is corruption (and
            /// silently deduplicating a root-edge duplicate would pick an arbitrary target).
            const String id = GcSnap::edgeIdFor(rec);
            if (snap.edges.contains(id))
                throw Exception(ErrorCodes::CORRUPTED_DATA, "CAS gc/snap: duplicate edge id '{}'", id);
            snap.addEdge(std::move(rec));                        /// rebuilds indeg (and seeds known)
        }

        uint32_t expanded_count = 0;
        readBinaryLittleEndian(expanded_count, in);
        for (uint32_t i = 0; i < expanded_count; ++i)
        {
            UInt128 tree{};
            readBinaryLittleEndian(tree, in);
            snap.expanded.insert(tree);
        }

        /// The explicit known set is authoritative; the edge-derived inserts from addEdge above
        /// are a subset of it for any document we encoded (a known node may have zero edges — that
        /// is exactly the zero-in-degree candidate case). Union both (simple and over-count-safe).
        uint32_t known_count = 0;
        readBinaryLittleEndian(known_count, in);
        for (uint32_t i = 0; i < known_count; ++i)
        {
            uint8_t kind_byte = 0;
            readBinaryLittleEndian(kind_byte, in);
            const ObjectKind kind = objectKindFromByte(kind_byte);
            UInt128 hash{};
            readBinaryLittleEndian(hash, in);
            snap.known.insert(GcSnap::NodeKey{static_cast<uint8_t>(kind), hash});
        }

        return snap;
    });
}

}
