#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Formats/CasRefSnapshotFormat.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Formats/CasTextFormat.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Primitives/CasCodecUtil.h>
#include <Common/Exception.h>
#include <IO/ReadBufferFromMemory.h>
#include <IO/WriteBufferFromString.h>
#include <IO/WriteHelpers.h>
#include <optional>
#include <tuple>

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

std::string_view lifecycleToWord(RefLifecycle l)
{
    switch (l)
    {
        case RefLifecycle::Live:    return "live";
        case RefLifecycle::Removed: return "removed";
    }
    throw Exception(ErrorCodes::CORRUPTED_DATA, "RefTableSnapshot: unknown lifecycle {}", static_cast<int>(l));
}

RefLifecycle lifecycleFromWord(std::string_view w)
{
    if (w == "live")    return RefLifecycle::Live;
    if (w == "removed") return RefLifecycle::Removed;
    throw Exception(ErrorCodes::CORRUPTED_DATA, "RefTableSnapshot: unknown lifecycle '{}'", w);
}

void checkTxnIdNonzero(const RefTxnId & id, std::string_view what)
{
    if (id.writer_epoch == 0 || id.ref_sequence == 0)
        throw Exception(ErrorCodes::CORRUPTED_DATA,
            "RefTableSnapshot: {} fields must both be nonzero, got {}-{}", what, id.writer_epoch, id.ref_sequence);
}

void checkCommittedSorted(const std::vector<RefCommittedRow> & rows)
{
    for (size_t i = 1; i < rows.size(); ++i)
        if (!(rows[i - 1].ref_name < rows[i].ref_name))
            throw Exception(ErrorCodes::CORRUPTED_DATA,
                "RefTableSnapshot: committed rows are not strictly ascending by ref_name at '{}' -> '{}'",
                rows[i - 1].ref_name, rows[i].ref_name);
}

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

/// Whole-object validation: transaction IDs must be nonzero, lifecycle determines whether removal
/// metadata and rows are allowed, `sealed_from` must not be later than `snapshot_id`, and both row
/// vectors must be strictly sorted. Applying the same checks before encoding and after decoding
/// keeps malformed caller state and malformed stored data subject to the same contract.
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

    if (snapshot.sealed_from)
    {
        checkTxnIdNonzero(*snapshot.sealed_from, "sealed_from");
        if (snapshot.snapshot_id < *snapshot.sealed_from)
            throw Exception(ErrorCodes::LOGICAL_ERROR,
                "RefTableSnapshot: sealed_from {}-{} exceeds snapshot_id {}-{}",
                snapshot.sealed_from->writer_epoch, snapshot.sealed_from->ref_sequence,
                snapshot.snapshot_id.writer_epoch, snapshot.snapshot_id.ref_sequence);
    }

    checkCommittedSorted(snapshot.committed);
    checkPrecommitsSorted(snapshot.precommits);
}

void writeIdFields(WriteBuffer & out, bool & first, std::string_view epoch_key, std::string_view seq_key, const RefTxnId & id)
{
    /// Both fields decimal STRINGS: ref_sequence reaches UINT64_MAX for a recovery seal.
    writeKey(out, epoch_key, first);
    writeU64StringValue(out, id.writer_epoch);
    writeKey(out, seq_key, first);
    writeU64StringValue(out, id.ref_sequence);
}

void writeCommittedRow(WriteBuffer & out, const RefCommittedRow & row)
{
    checkCanonicalRefName(row.ref_name, "RefTableSnapshot", "committed ref_name");
    checkManifestRef(row.manifest_ref, "RefTableSnapshot", "committed");
    bool first = true;
    writeKey(out, "k", first);
    writeStringValue(out, "c");
    writeKey(out, "rn", first);
    writeStringValue(out, row.ref_name);
    writeManifestRefFields(out, first, "", row.manifest_ref);
    writeKey(out, "pl", first);
    writeStringValue(out, row.payload);
    writeKey(out, "ts", first);
    writeIntText(row.published_at_ms, out);
    closeObject(out, first);
    writeChar('\n', out);
}

void writePrecommitRow(WriteBuffer & out, const RefOwnerBinding & row)
{
    if (row.kind != RefOwnerKind::Precommit)
        throw Exception(ErrorCodes::CORRUPTED_DATA,
            "RefTableSnapshot: precommits entry '{}' has kind {}, expected Precommit",
            row.ref_name, static_cast<uint8_t>(row.kind));
    checkCanonicalRefName(row.ref_name, "RefTableSnapshot", "precommit ref_name");
    checkManifestRef(row.manifest_ref, "RefTableSnapshot", "precommit");
    bool first = true;
    writeKey(out, "k", first);
    writeStringValue(out, "p");
    writeKey(out, "rn", first);
    writeStringValue(out, row.ref_name);
    writeManifestRefFields(out, first, "", row.manifest_ref);
    closeObject(out, first);
    writeChar('\n', out);
}

/// Collector for a ManifestRef's three flat fields (bare "me"/"mb"/"mo").
struct ManifestFields
{
    std::optional<uint64_t> me;
    std::optional<uint64_t> mb;
    std::optional<uint64_t> mo;

    /// Reconstruct a manifest reference after the tolerant reader has collected all three flat
    /// fields. Missing fields are malformed input; `manifestRefFromFields` performs the remaining
    /// range checks and reports the same corruption context as the row decoder.
    ManifestRef build(std::string_view what) const
    {
        if (!me || !mb || !mo)
            throw Exception(ErrorCodes::CORRUPTED_DATA, "RefTableSnapshot: {} manifest_ref missing me/mb/mo", what);
        return manifestRefFromFields(*me, *mb, *mo, "RefTableSnapshot", what);
    }
};

}

String encodeRefTableSnapshot(const RefTableSnapshot & snapshot)
{
    checkSnapshotInvariants(snapshot);

    WriteBufferFromOwnString out;
    writeHeaderLine(out, FormatId::RefSnapshot);

    {
        bool first = true;
        writeKey(out, "ns", first);
        writeStringValue(out, snapshot.ns);
        writeIdFields(out, first, "we", "rs", snapshot.snapshot_id);
        writeKey(out, "lc", first);
        writeStringValue(out, lifecycleToWord(snapshot.lifecycle));
        if (snapshot.lifecycle == RefLifecycle::Removed)
            writeIdFields(out, first, "rte", "rts", *snapshot.remove_txn_id);
        if (snapshot.sealed_from)
            writeIdFields(out, first, "sfe", "sfs", *snapshot.sealed_from);
        closeObject(out, first);
        writeChar('\n', out);
    }

    for (const RefCommittedRow & row : snapshot.committed)
        writeCommittedRow(out, row);
    for (const RefOwnerBinding & row : snapshot.precommits)
        writePrecommitRow(out, row);

    writeTrailerLine(out, snapshot.committed.size() + snapshot.precommits.size());
    out.finalize();
    const String text = out.str();
    if (text.size() > ref_snapshot_max_bytes)
        throw Exception(ErrorCodes::CORRUPTED_DATA,
            "RefTableSnapshot: encoded size {} exceeds the snapshot byte limit {}", text.size(), ref_snapshot_max_bytes);
    return text;
}

RefTableSnapshot decodeRefTableSnapshot(
    std::string_view data, const String & expected_ns, const RefTxnId & expected_snapshot_id)
{
    ReadBufferFromMemory in(data.data(), data.size());
    expectHeaderLine(in, FormatId::RefSnapshot);
    const uint64_t line_cap = traitsFor(FormatId::RefSnapshot).line_cap;

    RefTableSnapshot snapshot;

    {
        const String line = readLine(in, line_cap, "cas_ref_snap");
        ReadBufferFromMemory m(line.data(), line.size());
        JsonObjectReader r(m, KeyStrictness::Tolerant, "cas_ref_snap");
        bool saw_ns = false, saw_we = false, saw_rs = false, saw_lc = false;
        std::optional<uint64_t> rte, rts, sfe, sfs;
        String key;
        while (r.nextKey(key))
        {
            if (key == "ns") { snapshot.ns = r.readString(); saw_ns = true; }
            else if (key == "we") { snapshot.snapshot_id.writer_epoch = r.readU64String(); saw_we = true; }
            else if (key == "rs") { snapshot.snapshot_id.ref_sequence = r.readU64String(); saw_rs = true; }
            else if (key == "lc") { snapshot.lifecycle = lifecycleFromWord(r.readString()); saw_lc = true; }
            else if (key == "rte") rte = r.readU64String();
            else if (key == "rts") rts = r.readU64String();
            else if (key == "sfe") sfe = r.readU64String();
            else if (key == "sfs") sfs = r.readU64String();
            else r.skipUnknown(key);
        }
        if (!saw_ns || !saw_we || !saw_rs || !saw_lc)
            throw Exception(ErrorCodes::CORRUPTED_DATA, "RefTableSnapshot: meta line missing ns/we/rs/lc");
        if (rte || rts)
        {
            if (!rte || !rts)
                throw Exception(ErrorCodes::CORRUPTED_DATA, "RefTableSnapshot: remove_txn_id needs both rte and rts");
            snapshot.remove_txn_id = RefTxnId{*rte, *rts};
        }
        if (sfe || sfs)
        {
            if (!sfe || !sfs)
                throw Exception(ErrorCodes::CORRUPTED_DATA, "RefTableSnapshot: sealed_from needs both sfe and sfs");
            snapshot.sealed_from = RefTxnId{*sfe, *sfs};
        }
        if (!m.eof())
            throw Exception(ErrorCodes::CORRUPTED_DATA, "RefTableSnapshot: junk after meta line");
    }

    /// record lines (committed then precommit), until the trailer
    while (true)
    {
        const String line = readLine(in, line_cap, "cas_ref_snap");
        ReadBufferFromMemory l(line.data(), line.size());
        JsonObjectReader r(l, KeyStrictness::Tolerant, "cas_ref_snap");
        String key;
        if (!r.nextKey(key))
            throw Exception(ErrorCodes::CORRUPTED_DATA, "RefTableSnapshot: empty line");

        if (key == "n")
        {
            const uint64_t n = r.readU64Number();
            while (r.nextKey(key))
                r.skipUnknown(key);
            if (!l.eof() || !in.eof())
                throw Exception(ErrorCodes::CORRUPTED_DATA, "RefTableSnapshot: bytes after trailer");
            if (n != snapshot.committed.size() + snapshot.precommits.size())
                throw Exception(ErrorCodes::CORRUPTED_DATA,
                    "RefTableSnapshot: trailer count {} != {} rows", n, snapshot.committed.size() + snapshot.precommits.size());
            break;
        }
        if (key != "k")
            throw Exception(ErrorCodes::CORRUPTED_DATA, "RefTableSnapshot: record must start with \"k\"");
        const String k = r.readString();

        std::optional<String> rn;
        ManifestFields mf;
        std::optional<String> pl;
        std::optional<uint64_t> ts;
        while (r.nextKey(key))
        {
            if (key == "rn") rn = r.readString();
            else if (key == "me") mf.me = r.readU64String();
            else if (key == "mb") mf.mb = r.readU64String();
            else if (key == "mo") mf.mo = r.readU64Number();
            else if (key == "pl") pl = r.readString();
            else if (key == "ts") ts = r.readU64Number();
            else r.skipUnknown(key);
        }
        if (!l.eof())
            throw Exception(ErrorCodes::CORRUPTED_DATA, "RefTableSnapshot: junk after record");

        if (k == "c")
        {
            if (!rn || !pl || !ts)
                throw Exception(ErrorCodes::CORRUPTED_DATA, "RefTableSnapshot: committed row missing rn/pl/ts");
            RefCommittedRow row;
            row.ref_name = *rn;
            checkCanonicalRefName(row.ref_name, "RefTableSnapshot", "committed ref_name");
            row.manifest_ref = mf.build("committed");
            row.payload = *pl;
            row.published_at_ms = *ts;
            snapshot.committed.push_back(std::move(row));
        }
        else if (k == "p")
        {
            if (!rn)
                throw Exception(ErrorCodes::CORRUPTED_DATA, "RefTableSnapshot: precommit row missing rn");
            RefOwnerBinding row;
            row.kind = RefOwnerKind::Precommit;
            row.ref_name = *rn;
            checkCanonicalRefName(row.ref_name, "RefTableSnapshot", "precommit ref_name");
            row.manifest_ref = mf.build("precommit");
            snapshot.precommits.push_back(std::move(row));
        }
        else
            throw Exception(ErrorCodes::CORRUPTED_DATA, "RefTableSnapshot: unknown row kind '{}'", k);
    }

    /// The object key is supplied separately from the body. Check the binding before accepting any
    /// decoded state so bytes stored under one namespace or snapshot ID cannot be interpreted as
    /// another object.
    if (snapshot.ns != expected_ns || snapshot.snapshot_id != expected_snapshot_id)
        throw Exception(ErrorCodes::CORRUPTED_DATA,
            "RefTableSnapshot: body (ns='{}', snapshot_id={}-{}) does not match the key it was read from "
            "(ns='{}', snapshot_id={}-{})",
            snapshot.ns, snapshot.snapshot_id.writer_epoch, snapshot.snapshot_id.ref_sequence,
            expected_ns, expected_snapshot_id.writer_epoch, expected_snapshot_id.ref_sequence);

    checkSnapshotInvariants(snapshot);
    return snapshot;
}

}
