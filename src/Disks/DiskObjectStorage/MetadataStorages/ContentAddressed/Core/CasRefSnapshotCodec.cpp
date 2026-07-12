#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasRefSnapshotCodec.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasCodecUtil.h>
#include <IO/WriteBufferFromString.h>
#include <IO/WriteHelpers.h>
#include <IO/ReadBufferFromMemory.h>
#include <IO/ReadHelpers.h>
#include <Common/Exception.h>
#include <algorithm>
#include <limits>
#include <tuple>

namespace DB
{
namespace ErrorCodes
{
    extern const int CORRUPTED_DATA;
    extern const int UNKNOWN_FORMAT_VERSION;
}
}

namespace DB::Cas
{

namespace
{

constexpr uint32_t kRefTableSnapshotFormatVersion = 1;

/// Canonical-clean-relative-path check (spec §One Log Encoding, reused for snapshot ref names): shared
/// with `CasRefLogCodec` via `CasCodecUtil.h`'s `isCanonicalRefName`/`checkCanonicalRefName` rather
/// than duplicated per codec.
void checkRefName(std::string_view name, std::string_view what)
{
    checkCanonicalRefName(name, "RefTableSnapshot", what);
}

/// `ManifestRef` field validity: shared with `CasRefLogCodec` via `CasCodecUtil.h`'s
/// `checkManifestRef` rather than duplicated per codec.
void checkManifestRef(const ManifestRef & ref, std::string_view what)
{
    DB::Cas::checkManifestRef(ref, "RefTableSnapshot", what);
}

void checkTxnIdNonzero(const RefTxnId & id, std::string_view what)
{
    if (id.writer_epoch == 0 || id.ref_sequence == 0)
        throw Exception(ErrorCodes::CORRUPTED_DATA,
            "RefTableSnapshot: {} fields must both be nonzero, got {}-{}", what, id.writer_epoch, id.ref_sequence);
}

void writeTxnId(WriteBuffer & out, const RefTxnId & id)
{
    writeBinaryLittleEndian(id.writer_epoch, out);
    writeBinaryLittleEndian(id.ref_sequence, out);
}

RefTxnId readTxnId(ReadBuffer & in)
{
    RefTxnId id;
    readBinaryLittleEndian(id.writer_epoch, in);
    readBinaryLittleEndian(id.ref_sequence, in);
    return id;
}

void writeManifestRef(WriteBuffer & out, const ManifestRef & ref)
{
    writeBinaryLittleEndian(ref.writer_epoch, out);
    writeBinaryLittleEndian(ref.build_sequence, out);
    writeBinaryLittleEndian(ref.manifest_ordinal, out);
}

ManifestRef readManifestRef(ReadBuffer & in)
{
    ManifestRef ref;
    readBinaryLittleEndian(ref.writer_epoch, in);
    readBinaryLittleEndian(ref.build_sequence, in);
    readBinaryLittleEndian(ref.manifest_ordinal, in);
    return ref;
}

/// `writeLenPrefixed`/`readLenPrefixed` (u32-length-prefixed byte strings, with the >UInt32 guard):
/// shared with `CasRefLogCodec` via `CasCodecUtil.h` rather than duplicated per codec.

void writeCommittedRow(WriteBuffer & out, const RefCommittedRow & row)
{
    checkRefName(row.ref_name, "committed ref_name");
    checkManifestRef(row.manifest_ref, "committed");
    writeLenPrefixed(out, row.ref_name);
    writeManifestRef(out, row.manifest_ref);
    writeLenPrefixed(out, row.payload);
    writeBinaryLittleEndian(row.published_at_ms, out);
}

RefCommittedRow readCommittedRow(ReadBuffer & in)
{
    RefCommittedRow row;
    row.ref_name = readLenPrefixed(in);
    checkRefName(row.ref_name, "committed ref_name");
    row.manifest_ref = readManifestRef(in);
    checkManifestRef(row.manifest_ref, "committed");
    row.payload = readLenPrefixed(in);
    readBinaryLittleEndian(row.published_at_ms, in);
    return row;
}

/// Every `precommits` entry must carry `kind == Precommit`: the list's membership already says so, so
/// this is validated rather than left to encode arbitrary data under the wrong tag.
void writePrecommitRow(WriteBuffer & out, const RefOwnerBinding & row)
{
    if (row.kind != RefOwnerKind::Precommit)
        throw Exception(ErrorCodes::CORRUPTED_DATA,
            "RefTableSnapshot: precommits entry '{}' has kind {}, expected Precommit",
            row.ref_name, static_cast<uint8_t>(row.kind));
    checkRefName(row.ref_name, "precommit ref_name");
    checkManifestRef(row.manifest_ref, "precommit");
    writeLenPrefixed(out, row.ref_name);
    writeManifestRef(out, row.manifest_ref);
}

RefOwnerBinding readPrecommitRow(ReadBuffer & in)
{
    RefOwnerBinding row;
    row.kind = RefOwnerKind::Precommit;
    row.ref_name = readLenPrefixed(in);
    checkRefName(row.ref_name, "precommit ref_name");
    row.manifest_ref = readManifestRef(in);
    checkManifestRef(row.manifest_ref, "precommit");
    return row;
}

/// Strictly ascending by canonical bytewise `ref_name`, so this single check also rejects a duplicate
/// (there is exactly one committed manifest per name).
void checkCommittedSorted(const std::vector<RefCommittedRow> & rows)
{
    for (size_t i = 1; i < rows.size(); ++i)
        if (!(rows[i - 1].ref_name < rows[i].ref_name))
            throw Exception(ErrorCodes::CORRUPTED_DATA,
                "RefTableSnapshot: committed rows are not strictly ascending by ref_name at '{}' -> '{}'",
                rows[i - 1].ref_name, rows[i].ref_name);
}

/// Strictly ascending by `(ref_name, manifest_ref)`, so this also rejects an exact duplicate binding
/// (same name, same manifest_ref) while still allowing several builds to race for the same name.
void checkPrecommitsSorted(const std::vector<RefOwnerBinding> & rows)
{
    for (size_t i = 1; i < rows.size(); ++i)
    {
        const auto prev_key = std::tie(rows[i - 1].ref_name, rows[i - 1].manifest_ref);
        const auto cur_key = std::tie(rows[i].ref_name, rows[i].manifest_ref);
        if (!(prev_key < cur_key))
            throw Exception(ErrorCodes::CORRUPTED_DATA,
                "RefTableSnapshot: precommit rows are not strictly ascending by (ref_name, manifest_ref) at '{}' -> '{}'",
                rows[i - 1].ref_name, rows[i].ref_name);
    }
}

/// Whole-object validation: txn-id nonzero-ness, `Live`/`Removed` shape, and sortedness. Individual
/// row validity (canonical `ref_name`, valid `manifest_ref`, precommit `kind`) is instead checked once
/// per row inside `write*Row`/`read*Row` -- applied to both the object about to be encoded and the
/// object just decoded (mirrors `CasRefLogCodec`'s symmetric encode/decode validation).
void checkSnapshotInvariants(const RefTableSnapshot & snapshot)
{
    checkTxnIdNonzero(snapshot.snapshot_id, "snapshot_id");

    if (snapshot.lifecycle == RefLifecycle::Removed)
    {
        if (!snapshot.remove_txn_id)
            throw Exception(ErrorCodes::CORRUPTED_DATA, "RefTableSnapshot: Removed snapshot is missing remove_txn_id");
        checkTxnIdNonzero(*snapshot.remove_txn_id, "remove_txn_id");
        if (!snapshot.committed.empty() || !snapshot.precommits.empty())
            throw Exception(ErrorCodes::CORRUPTED_DATA,
                "RefTableSnapshot: Removed snapshot must have zero committed/precommit rows, got {}/{}",
                snapshot.committed.size(), snapshot.precommits.size());
    }
    else if (snapshot.remove_txn_id)
        throw Exception(ErrorCodes::CORRUPTED_DATA, "RefTableSnapshot: Live snapshot must not carry remove_txn_id");

    checkCommittedSorted(snapshot.committed);
    checkPrecommitsSorted(snapshot.precommits);
}

}

String encodeRefTableSnapshot(const RefTableSnapshot & snapshot)
{
    checkSnapshotInvariants(snapshot);

    WriteBufferFromOwnString out;
    writeBinaryLittleEndian(kRefTableSnapshotFormatVersion, out);
    writeLenPrefixed(out, snapshot.ns);
    writeTxnId(out, snapshot.snapshot_id);
    writeBinaryLittleEndian(static_cast<uint8_t>(snapshot.lifecycle), out);
    if (snapshot.lifecycle == RefLifecycle::Removed)
        writeTxnId(out, *snapshot.remove_txn_id);

    writeBinaryLittleEndian(static_cast<uint32_t>(snapshot.committed.size()), out);
    for (const RefCommittedRow & row : snapshot.committed)
        writeCommittedRow(out, row);

    writeBinaryLittleEndian(static_cast<uint32_t>(snapshot.precommits.size()), out);
    for (const RefOwnerBinding & row : snapshot.precommits)
        writePrecommitRow(out, row);

    const String bytes = out.str();
    if (bytes.size() > ref_snapshot_max_bytes)
        throw Exception(ErrorCodes::CORRUPTED_DATA,
            "RefTableSnapshot: encoded size {} exceeds the snapshot byte limit {}", bytes.size(), ref_snapshot_max_bytes);
    return bytes;
}

RefTableSnapshot decodeRefTableSnapshot(
    std::string_view data, const String & expected_ns, const RefTxnId & expected_snapshot_id)
{
    if (data.size() > ref_snapshot_max_bytes)
        throw Exception(ErrorCodes::CORRUPTED_DATA,
            "RefTableSnapshot: encoded size {} exceeds the snapshot byte limit {}", data.size(), ref_snapshot_max_bytes);

    ReadBufferFromMemory in(data.data(), data.size());
    return decodeGuarded("RefTableSnapshot", [&]
    {
        uint32_t format_version = 0;
        readBinaryLittleEndian(format_version, in);
        if (format_version != kRefTableSnapshotFormatVersion)
            throw Exception(ErrorCodes::UNKNOWN_FORMAT_VERSION,
                "RefTableSnapshot: unknown format_version {}", format_version);

        RefTableSnapshot snapshot;
        snapshot.ns = readLenPrefixed(in);
        snapshot.snapshot_id = readTxnId(in);

        uint8_t lifecycle_raw = 0;
        readBinaryLittleEndian(lifecycle_raw, in);
        if (lifecycle_raw != static_cast<uint8_t>(RefLifecycle::Live)
            && lifecycle_raw != static_cast<uint8_t>(RefLifecycle::Removed))
            throw Exception(ErrorCodes::CORRUPTED_DATA, "RefTableSnapshot: unknown lifecycle {}", lifecycle_raw);
        snapshot.lifecycle = static_cast<RefLifecycle>(lifecycle_raw);
        if (snapshot.lifecycle == RefLifecycle::Removed)
            snapshot.remove_txn_id = readTxnId(in);

        uint32_t n_committed = 0;
        readBinaryLittleEndian(n_committed, in);
        snapshot.committed.reserve(std::min<uint32_t>(n_committed, 1024));
        for (uint32_t i = 0; i < n_committed; ++i)
            snapshot.committed.push_back(readCommittedRow(in));

        uint32_t n_precommits = 0;
        readBinaryLittleEndian(n_precommits, in);
        snapshot.precommits.reserve(std::min<uint32_t>(n_precommits, 1024));
        for (uint32_t i = 0; i < n_precommits; ++i)
            snapshot.precommits.push_back(readPrecommitRow(in));

        if (snapshot.ns != expected_ns || snapshot.snapshot_id != expected_snapshot_id)
            throw Exception(ErrorCodes::CORRUPTED_DATA,
                "RefTableSnapshot: body (ns='{}', snapshot_id={}-{}) does not match the key it was read from "
                "(ns='{}', snapshot_id={}-{})",
                snapshot.ns, snapshot.snapshot_id.writer_epoch, snapshot.snapshot_id.ref_sequence,
                expected_ns, expected_snapshot_id.writer_epoch, expected_snapshot_id.ref_sequence);

        checkSnapshotInvariants(snapshot);
        return snapshot;
    });
}

}
