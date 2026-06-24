#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasGcSnap.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasCodecUtil.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasIds.h>
#include <IO/ReadBufferFromMemory.h>
#include <IO/ReadBufferFromString.h>
#include <IO/ReadHelpers.h>
#include <IO/WriteBufferFromString.h>
#include <IO/ZstdDeflatingWriteBuffer.h>
#include <IO/ZstdInflatingReadBuffer.h>
#include <Common/Exception.h>
#include <base/defines.h>
#include <limits>

namespace DB
{
namespace ErrorCodes
{
    extern const int CORRUPTED_DATA;
    extern const int UNKNOWN_FORMAT_VERSION;
    extern const int LOGICAL_ERROR;
    extern const int ZSTD_DECODER_FAILED;
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
/// truncated => CORRUPTED_DATA; future version => UNKNOWN_FORMAT_VERSION).
constexpr char GC_SNAP_MAGIC[4] = {'C', 'A', 'G', 'S'};
/// v1 of the cursor-carrying snap (B140-dangle fix); no migration, no back-compat.
/// Frame layout: magic(4) + version(1) + codec(1) + <compressed-or-raw body>.
/// Old readers (version <= 0) never see version 1 — they reject it with UNKNOWN_FORMAT_VERSION.
constexpr uint8_t GC_SNAP_VERSION = 1;   /// v1 of the cursor-carrying snap (B140-dangle fix); no migration, no back-compat.
constexpr uint8_t GC_SNAP_CODEC_RAW = 0;   /// body is the raw field bytes (reserved; not emitted)
constexpr uint8_t GC_SNAP_CODEC_ZSTD = 1;  /// body is zstd-compressed field bytes

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
    }
    throw Exception(ErrorCodes::CORRUPTED_DATA, "CAS gc/snap: invalid edge kind byte {}", static_cast<int>(b));
}

ObjectKind objectKindFromByte(uint8_t b)
{
    switch (b)
    {
        case static_cast<uint8_t>(ObjectKind::Blob): return ObjectKind::Blob;
        case static_cast<uint8_t>(ObjectKind::Tree): return ObjectKind::Tree;
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
    }
    throw Exception(ErrorCodes::CORRUPTED_DATA,
        "CAS gc/snap: invalid edge kind {}", static_cast<int>(rec.edge_kind));
}

void GcSnap::addEdge(EdgeRec rec)
{
    /// Set-semantics insert for edges whose canonical id INCLUDES the target (Tree): a
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

void GcSnap::forget(ObjectKind kind, const UInt128 & hash)
{
    /// indeg holds only nonzero counts; a zero-in-degree node has no indeg entry, so erasing from
    /// `known` is sufficient. Erasing a key not present is a no-op (idempotent).
    known.erase(NodeKey{static_cast<uint8_t>(kind), hash});
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

/// Encodes the snap fields (snap_shard, generation, edges, expanded, known, folded_cursor) into a
/// binary body string using the standard IO helpers.  The frame header (magic + version + codec) is
/// NOT included here — only the raw field bytes.  This is the v1 cursor-carrying layout, paired with
/// `decodeSnapFields` which reconstructs the snap from any decompressed body.
String GcSnap::encodeSnapFields(const GcSnap & snap)
{
    WriteBufferFromOwnString body;
    writeBinaryLittleEndian(snap.snap_shard, body);
    writeBinaryLittleEndian(snap.generation, body);

    writeBinaryLittleEndian(static_cast<uint32_t>(snap.edges.size()), body);
    for (const auto & [id, rec] : snap.edges)                    /// canonical-edge-id order
    {
        writeBinaryLittleEndian(edgeKindToByte(rec.edge_kind), body);
        if (rec.edge_kind == EdgeKind::Root)
        {
            writeBinaryLittleEndian(static_cast<uint16_t>(rec.root_shard.size()), body);
            writeString(rec.root_shard, body);
            writeBinaryLittleEndian(static_cast<uint16_t>(rec.part_name.size()), body);
            writeString(rec.part_name, body);
        }
        else
        {
            writeU128LE(body, rec.parent_tree);
        }
        writeBinaryLittleEndian(static_cast<uint8_t>(rec.target_kind), body);
        writeU128LE(body, rec.target_hash);
    }

    writeBinaryLittleEndian(static_cast<uint32_t>(snap.expanded.size()), body);
    for (const auto & tree : snap.expanded)                      /// set order (sorted)
        writeU128LE(body, tree);

    writeBinaryLittleEndian(static_cast<uint32_t>(snap.known.size()), body);
    for (const auto & [kind, hash] : snap.known)                 /// set order (sorted by (kind, hash))
    {
        writeBinaryLittleEndian(kind, body);
        writeU128LE(body, hash);
    }

    writeBinaryLittleEndian(static_cast<uint32_t>(snap.folded_cursor.size()), body);
    for (const auto & [key, version] : snap.folded_cursor)        /// std::map => sorted key order (canonical)
    {
        writeBinaryLittleEndian(static_cast<uint16_t>(key.size()), body);
        writeString(key, body);
        writeBinaryLittleEndian(version, body);
    }

    return std::move(body.str());
}

/// Parses the snap fields from a ReadBuffer that is positioned at the start of the (possibly
/// decompressed) body — i.e. right after the 6-byte frame header has been consumed.
GcSnap GcSnap::decodeSnapFields(ReadBuffer & body)
{
    GcSnap snap;
    readBinaryLittleEndian(snap.snap_shard, body);
    readBinaryLittleEndian(snap.generation, body);

    uint32_t edge_count = 0;
    readBinaryLittleEndian(edge_count, body);
    for (uint32_t i = 0; i < edge_count; ++i)
    {
        GcSnap::EdgeRec rec;
        uint8_t edge_kind_byte = 0;
        readBinaryLittleEndian(edge_kind_byte, body);
        rec.edge_kind = edgeKindFromByte(edge_kind_byte);
        if (rec.edge_kind == EdgeKind::Root)
        {
            uint16_t shard_len = 0;
            readBinaryLittleEndian(shard_len, body);
            rec.root_shard = readFixedBytes(body, shard_len);
            uint16_t part_len = 0;
            readBinaryLittleEndian(part_len, body);
            rec.part_name = readFixedBytes(body, part_len);
        }
        else
        {
            rec.parent_tree = readU128LE(body);
        }
        uint8_t target_kind_byte = 0;
        readBinaryLittleEndian(target_kind_byte, body);
        rec.target_kind = objectKindFromByte(target_kind_byte);
        rec.target_hash = readU128LE(body);

        const String id = GcSnap::edgeIdFor(rec);
        if (snap.edges.contains(id))
            throw Exception(ErrorCodes::CORRUPTED_DATA, "CAS gc/snap: duplicate edge id '{}'", id);
        snap.addEdge(std::move(rec));
    }

    uint32_t expanded_count = 0;
    readBinaryLittleEndian(expanded_count, body);
    for (uint32_t i = 0; i < expanded_count; ++i)
    {
        const UInt128 tree = readU128LE(body);
        snap.expanded.insert(tree);
    }

    uint32_t known_count = 0;
    readBinaryLittleEndian(known_count, body);
    for (uint32_t i = 0; i < known_count; ++i)
    {
        uint8_t kind_byte = 0;
        readBinaryLittleEndian(kind_byte, body);
        const ObjectKind kind = objectKindFromByte(kind_byte);
        const UInt128 hash = readU128LE(body);
        snap.known.insert(GcSnap::NodeKey{static_cast<uint8_t>(kind), hash});
    }

    uint32_t cursor_count = 0;
    readBinaryLittleEndian(cursor_count, body);
    for (uint32_t i = 0; i < cursor_count; ++i)
    {
        uint16_t key_len = 0;
        readBinaryLittleEndian(key_len, body);
        const String key = readFixedBytes(body, key_len);
        uint64_t version = 0;
        readBinaryLittleEndian(version, body);
        snap.folded_cursor[key] = version;
    }

    return snap;
}

String encodeGcSnap(const GcSnap & snap)
{
    /// 1. Encode the field body (the v1 cursor-carrying layout, without the frame header).
    const String raw_body = GcSnap::encodeSnapFields(snap);

    /// 2. Compress the body with zstd.
    String compressed;
    {
        auto sink = std::make_unique<WriteBufferFromString>(compressed);
        ZstdDeflatingWriteBuffer zstd(std::move(sink), ZSTD_defaultCLevel());
        zstd.write(raw_body.data(), raw_body.size());
        zstd.finalize();
    }

    /// 3. Prepend the 10-byte frame: magic(4) + version(1=1) + codec(1=ZSTD) + compressed_len(4).
    /// The explicit compressed_len lets the decoder detect truncation (including truncated checksum
    /// bytes) independently of the zstd stream framing.
    /// The 4-byte length field bounds the compressed body at 4 GiB; a reachability snap will never
    /// approach this even uncompressed, but make the assumption explicit.
    chassert(compressed.size() <= std::numeric_limits<uint32_t>::max());
    const auto compressed_len = static_cast<uint32_t>(compressed.size());
    String out;
    out.reserve(10 + compressed.size());
    out.append(GC_SNAP_MAGIC, sizeof(GC_SNAP_MAGIC));
    out.push_back(static_cast<char>(GC_SNAP_VERSION));
    out.push_back(static_cast<char>(GC_SNAP_CODEC_ZSTD));
    /// out is a String here (not a WriteBuffer), so write the u32 length LE by hand.
    for (int i = 0; i < 4; ++i)
        out.push_back(static_cast<char>((compressed_len >> (8 * i)) & 0xFF));
    out.append(compressed);
    return out;
}

GcSnap decodeGcSnap(std::string_view data)
{
    return decodeGuarded("gc/snap", [&]
    {
        ReadBufferFromMemory in(data.data(), data.size());

        /// --- 6-byte frame header ---
        if (readFixedBytes(in, sizeof(GC_SNAP_MAGIC)) != std::string_view(GC_SNAP_MAGIC, sizeof(GC_SNAP_MAGIC)))
            throw Exception(ErrorCodes::CORRUPTED_DATA, "CAS gc/snap: bad magic");

        uint8_t version = 0;
        readBinaryLittleEndian(version, in);
        checkVersion(GC_SNAP_VERSION, version, "gc/snap");
        if (version != GC_SNAP_VERSION)
            throw Exception(ErrorCodes::CORRUPTED_DATA, "CAS gc/snap: invalid version {}", version);

        uint8_t codec = 0;
        readBinaryLittleEndian(codec, in);

        if (codec == GC_SNAP_CODEC_ZSTD)
        {
            /// Read the 4-byte compressed_len stored in the frame (allows reliable truncation
            /// detection that is independent of the zstd stream's own framing / checksum bytes).
            uint32_t compressed_len = 0;
            readBinaryLittleEndian(compressed_len, in);
            /// Bounds-check BEFORE allocation (in.available() is exact for ReadBufferFromMemory).
            if (compressed_len > in.available())
                throw Exception(ErrorCodes::CORRUPTED_DATA,
                    "CAS gc/snap: truncated compressed body (expected {} bytes, {} available)",
                    compressed_len, in.available());
            const std::string_view compressed_view(in.position(), compressed_len);
            String body;
            try
            {
                auto compressed_in = std::make_unique<ReadBufferFromString>(compressed_view);
                ZstdInflatingReadBuffer zstd(std::move(compressed_in));
                readStringUntilEOF(body, zstd);
            }
            catch (const Exception & e)
            {
                if (e.code() == ErrorCodes::ZSTD_DECODER_FAILED)
                    throw Exception(ErrorCodes::CORRUPTED_DATA,
                        "CAS gc/snap: zstd decompression failed (corrupt compressed body): {}", e.message());
                throw;
            }
            ReadBufferFromOwnString body_buf(std::move(body));
            return GcSnap::decodeSnapFields(body_buf);
        }
        else if (codec == GC_SNAP_CODEC_RAW)
        {
            /// For RAW, the remainder is the unframed field bytes directly.
            const std::string_view remainder(in.position(), in.available());
            ReadBufferFromMemory body_buf(remainder.data(), remainder.size());
            return GcSnap::decodeSnapFields(body_buf);
        }
        else
        {
            throw Exception(ErrorCodes::CORRUPTED_DATA, "CAS gc/snap: unknown codec byte {}", static_cast<int>(codec));
        }
    });
}

}
