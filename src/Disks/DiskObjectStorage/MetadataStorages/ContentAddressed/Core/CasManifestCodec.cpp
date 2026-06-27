#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasManifestCodec.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasRunFile.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasCodecUtil.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasFormat.h>
#include <IO/WriteBufferFromString.h>
#include <IO/WriteHelpers.h>
#include <IO/ReadBufferFromMemory.h>
#include <IO/ReadHelpers.h>
#include <Common/Exception.h>
#include <city.h>
#include <algorithm>

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

/// One entry's RunFile payload: placement u8, blob_hash 16B LE, blob_size u64 LE, inline_len u32, inline.
String encodeEntryPayload(const ManifestEntry & e)
{
    WriteBufferFromOwnString buf;
    writeBinaryLittleEndian(static_cast<uint8_t>(e.placement), buf);
    writeU128LE(buf, e.blob_hash);
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
        e.blob_hash = readU128LE(in);
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
    /// ref: writer_instance_id is a String token ("<server_id_hex>:<process_epoch>"), len-prefixed
    writeBinaryLittleEndian(static_cast<uint32_t>(m.ref.writer_instance_id.size()), out);
    out.write(m.ref.writer_instance_id.data(), m.ref.writer_instance_id.size());
    writeBinaryLittleEndian(m.ref.build_sequence, out);
    writeU128LE(out, m.ref.manifest_instance_id);
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
        uint32_t writer_len = 0;
        readBinaryLittleEndian(writer_len, in);
        m.ref.writer_instance_id = readFixedBytes(in, writer_len);
        readBinaryLittleEndian(m.ref.build_sequence, in);
        m.ref.manifest_instance_id = readU128LE(in);
        uint32_t ns_len = 0;
        readBinaryLittleEndian(ns_len, in);
        m.root_namespace_id = RootNamespace(readFixedBytes(in, ns_len));
        m.payload_digest = readU128LE(in);

        uint64_t entries_len = 0;
        readBinaryLittleEndian(entries_len, in);
        const String entries_bytes = readFixedBytes(in, entries_len);
        ReadBufferFromMemory entries_in(entries_bytes.data(), entries_bytes.size());
        RunFileReader r(entries_in);
        String path, payload;
        String prev_path;
        bool have_prev = false;
        while (r.next(path, payload))
        {
            if (have_prev && path == prev_path)
                throw Exception(ErrorCodes::CORRUPTED_DATA, "PartManifest: duplicate path '{}'", path);
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

}
