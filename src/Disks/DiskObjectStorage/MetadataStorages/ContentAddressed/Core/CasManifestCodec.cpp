#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasManifestCodec.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasRunFile.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasCodecUtil.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/Formats/CasFormat.h>
#include <IO/WriteBufferFromString.h>
#include <IO/WriteHelpers.h>
#include <IO/ReadBufferFromMemory.h>
#include <IO/ReadHelpers.h>
#include <Common/Exception.h>
#include <city.h>
#include <algorithm>
#include <cstring>

namespace DB
{
namespace ErrorCodes
{
    extern const int CORRUPTED_DATA;
}
}

namespace DB::Cas
{

namespace
{

constexpr uint16_t kPartManifestFormatVersion = 1;

/// One entry's RunFile payload (Phase 3 T2 — per-entry `BlobRef`): placement u8, algo u8,
/// digest[blobHashLenFor(algo)] raw BE, blob_size u64 LE, inline_len u32, inline. The algo travels
/// WITH the digest so entries minted under different algos coexist in one manifest (mixed-algo pools).
String encodeEntryPayload(const ManifestEntry & e)
{
    WriteBufferFromOwnString buf;
    writeBinaryLittleEndian(static_cast<uint8_t>(e.placement), buf);
    writeBinaryLittleEndian(static_cast<uint8_t>(e.ref.algo), buf);
    const uint64_t digest_len = blobHashLenFor(e.ref.algo);
    buf.write(reinterpret_cast<const char *>(e.ref.digest.bytes.data()), digest_len);
    writeBinaryLittleEndian(e.blob_size, buf);
    writeBinaryLittleEndian(static_cast<uint32_t>(e.inline_bytes.size()), buf);
    buf.write(e.inline_bytes.data(), e.inline_bytes.size());
    return buf.str();
}

ManifestEntry decodeEntryPayload(const String & path, std::string_view payload)
{
    ReadBufferFromMemory in(payload.data(), payload.size());
    return decodeGuarded("PartManifest entry", [&]
    {
        ManifestEntry e;
        e.path = path;
        uint8_t placement_raw = 0;
        readBinaryLittleEndian(placement_raw, in);
        if (placement_raw != static_cast<uint8_t>(EntryPlacement::Inline)
            && placement_raw != static_cast<uint8_t>(EntryPlacement::Blob))
            throw Exception(ErrorCodes::CORRUPTED_DATA, "PartManifest: unknown placement {}", placement_raw);
        e.placement = static_cast<EntryPlacement>(placement_raw);

        uint8_t algo_raw = 0;
        readBinaryLittleEndian(algo_raw, in);
        const auto algo = static_cast<BlobHashAlgo>(algo_raw);
        try
        {
            /// Validates the enum value (throws BAD_ARGUMENTS for anything `blobHashAlgoName` rejects);
            /// the rendered name itself is unused here.
            blobHashAlgoName(algo);
        }
        catch (const Exception &)
        {
            throw Exception(ErrorCodes::CORRUPTED_DATA, "PartManifest: unknown blob hash algo {}", algo_raw);
        }
        e.ref.algo = algo;
        const uint64_t digest_len = blobHashLenFor(algo);
        const String hash_bytes = readFixedBytes(in, digest_len);
        e.ref.digest = BlobDigest{};
        memcpy(e.ref.digest.bytes.data(), hash_bytes.data(), digest_len);

        readBinaryLittleEndian(e.blob_size, in);
        uint32_t inline_len = 0;
        readBinaryLittleEndian(inline_len, in);
        e.inline_bytes = readFixedBytes(in, inline_len);
        return e;
    });
}

}

String encodePartManifest(const PartManifest & m)
{
    /// Canonical path order + duplicate-path rejection.
    std::vector<const ManifestEntry *> sorted;
    sorted.reserve(m.entries.size());
    for (const auto & e : m.entries)
        sorted.push_back(&e);
    std::sort(sorted.begin(), sorted.end(),
              [](const ManifestEntry * a, const ManifestEntry * b) { return a->path < b->path; });
    for (size_t i = 1; i < sorted.size(); ++i)
        if (sorted[i]->path == sorted[i - 1]->path)
            throw Exception(ErrorCodes::CORRUPTED_DATA, "PartManifest: duplicate path '{}'", sorted[i]->path);

    WriteBufferFromOwnString out;
    /// header: magic "CAPT", format_version u16, writer_version u16
    const uint32_t magic = magicFor(FormatId::PartManifest);
    writeBinaryLittleEndian(magic, out);
    writeBinaryLittleEndian(kPartManifestFormatVersion, out);
    writeBinaryLittleEndian(static_cast<uint16_t>(currentWriterVersion()), out);
    /// ref: durable writer_epoch + build sequence + per-build manifest ordinal.
    writeBinaryLittleEndian(m.ref.writer_epoch, out);
    writeBinaryLittleEndian(m.ref.build_sequence, out);
    writeBinaryLittleEndian(m.ref.manifest_ordinal, out);
    /// root_namespace_id (len-prefixed)
    writeBinaryLittleEndian(static_cast<uint32_t>(m.root_namespace_id.string().size()), out);
    out.write(m.root_namespace_id.string().data(), m.root_namespace_id.string().size());
    /// payload_digest
    writeU128LE(out, m.payload_digest);
    /// entries as an embedded RunFile (kind = ManifestEntries), key = path
    WriteBufferFromOwnString entries_buf;
    {
        RunHeader h;
        h.kind = RunKind::ManifestEntries;
        h.key_schema = 0;
        RunFileWriter w(entries_buf, h);
        for (const auto * e : sorted)
            w.append(e->path, encodeEntryPayload(*e));
        w.finish();
    }
    const String entries_bytes = entries_buf.str();
    writeBinaryLittleEndian(static_cast<uint64_t>(entries_bytes.size()), out);
    out.write(entries_bytes.data(), entries_bytes.size());
    return out.str();
}

PartManifest decodePartManifest(std::string_view data)
{
    ReadBufferFromMemory in(data.data(), data.size());
    return decodeGuarded("PartManifest", [&]
    {
        uint32_t magic = 0;
        readBinaryLittleEndian(magic, in);
        if (magic != magicFor(FormatId::PartManifest))
            throw Exception(ErrorCodes::CORRUPTED_DATA, "PartManifest: bad magic");
        uint16_t format_version = 0;
        readBinaryLittleEndian(format_version, in);
        checkCompatibility(format_version, "PartManifest");
        uint16_t writer_version = 0;
        readBinaryLittleEndian(writer_version, in);

        PartManifest m;
        readBinaryLittleEndian(m.ref.writer_epoch, in);
        readBinaryLittleEndian(m.ref.build_sequence, in);
        readBinaryLittleEndian(m.ref.manifest_ordinal, in);
        if (m.ref.manifest_ordinal == 0 || m.ref.manifest_ordinal > kMaxManifestOrdinal)
            throw Exception(ErrorCodes::CORRUPTED_DATA,
                "PartManifest: manifest ordinal {} out of range", m.ref.manifest_ordinal);
        uint32_t ns_len = 0;
        readBinaryLittleEndian(ns_len, in);
        m.root_namespace_id = RootNamespace(readFixedBytes(in, ns_len));
        m.payload_digest = readU128LE(in);

        uint64_t entries_len = 0;
        readBinaryLittleEndian(entries_len, in);
        const String entries_bytes = readFixedBytes(in, entries_len);
        /// Borrowed-memory reader over the decoded body (alive through this loop) — no extra copy.
        RunFileReader r{std::string_view(entries_bytes)};
        String path, payload;
        String prev_path;
        bool have_prev = false;
        while (r.next(path, payload))
        {
            if (have_prev && path == prev_path)
                throw Exception(ErrorCodes::CORRUPTED_DATA, "PartManifest: duplicate path '{}'", path);
            /// Strict ascending canonical order (spec 2026-07-08-cas-part-folder-cache): the encoder
            /// has always written sorted entries; enforcing it here makes duplicate detection sound
            /// for non-adjacent duplicates AND establishes the ordering invariant `findEntry` /
            /// `PartFolderView` binary search rely on.
            if (have_prev && path < prev_path)
                throw Exception(ErrorCodes::CORRUPTED_DATA,
                    "PartManifest: entries out of canonical order ('{}' after '{}')", path, prev_path);
            m.entries.push_back(decodeEntryPayload(path, payload));
            prev_path = path;
            have_prev = true;
        }
        return m;
    });
}

UInt128 computePayloadDigest(const PartManifest & m)
{
    /// Digest the canonical encoding with payload_digest zeroed, so the digest does not depend on
    /// itself (it would be circular otherwise) and is stable for identical bodies, changing whenever
    /// ref / namespace / entries change. Uses the CAS content-hash primitive (CityHash128) over the
    /// deterministic encodePartManifest bytes - the same primitive blob/tree hashing uses.
    PartManifest probe = m;
    probe.payload_digest = UInt128{};
    const String bytes = encodePartManifest(probe);
    const auto h = CityHash_v1_0_2::CityHash128(bytes.data(), bytes.size());
    return (static_cast<UInt128>(h.high64) << 64) | static_cast<UInt128>(h.low64);
}

bool refMatchesBody(const ManifestRef & journal_ref, const PartManifest & body)
{
    return journal_ref == body.ref;
}

bool manifestNamespaceMatches(const RootNamespace & owning, const PartManifest & body)
{
    return owning == body.root_namespace_id;
}

const ManifestEntry * findEntry(const std::vector<ManifestEntry> & entries, std::string_view path)
{
    const auto it = std::lower_bound(entries.begin(), entries.end(), path,
        [](const ManifestEntry & e, std::string_view p) { return std::string_view(e.path) < p; });
    if (it == entries.end() || std::string_view(it->path) != path)
        return nullptr;
    return &*it;
}

std::pair<const ManifestEntry *, const ManifestEntry *>
entryRange(const std::vector<ManifestEntry> & entries, std::string_view dir_prefix)
{
    if (dir_prefix.empty())
        return {entries.data(), entries.data() + entries.size()};
    /// Every path starting with `dir_prefix` compares >= `dir_prefix`, and prefixed paths form a
    /// contiguous run from the first such position.
    const auto first = std::lower_bound(entries.begin(), entries.end(), dir_prefix,
        [](const ManifestEntry & e, std::string_view p) { return std::string_view(e.path) < p; });
    auto last = first;
    while (last != entries.end() && std::string_view(last->path).starts_with(dir_prefix))
        ++last;
    return {entries.data() + (first - entries.begin()), entries.data() + (last - entries.begin())};
}

}
