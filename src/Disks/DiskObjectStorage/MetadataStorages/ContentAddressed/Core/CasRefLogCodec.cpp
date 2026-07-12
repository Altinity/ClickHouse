#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasRefLogCodec.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasCodecUtil.h>
#include <IO/WriteBufferFromString.h>
#include <IO/WriteHelpers.h>
#include <IO/ReadBufferFromMemory.h>
#include <IO/ReadHelpers.h>
#include <Common/Exception.h>
#include <algorithm>
#include <limits>

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

constexpr uint32_t kRefLogTxnFormatVersion = 1;

/// Canonical-clean-relative-path check (spec §One Log Encoding): shared with `CasRefSnapshotCodec` via
/// `CasCodecUtil.h`'s `isCanonicalRefName`/`checkCanonicalRefName` rather than duplicated per codec.
void checkRefName(std::string_view name, std::string_view what)
{
    checkCanonicalRefName(name, "RefLogTxn", what);
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

void writeLenPrefixed(WriteBuffer & out, const String & s)
{
    /// Explicit guard rather than relying on the op/byte budgets alone to keep every string short:
    /// this is the point where a length silently truncated by the u32 cast would corrupt the wire.
    if (s.size() > std::numeric_limits<uint32_t>::max())
        throw Exception(ErrorCodes::CORRUPTED_DATA, "RefLogTxn: string field of {} bytes exceeds UInt32 length prefix", s.size());
    writeBinaryLittleEndian(static_cast<uint32_t>(s.size()), out);
    out.write(s.data(), s.size());
}

String readLenPrefixed(ReadBuffer & in)
{
    uint32_t len = 0;
    readBinaryLittleEndian(len, in);
    return readFixedBytes(in, len);
}

void writeBinding(WriteBuffer & out, const RefOwnerBinding & b)
{
    checkRefName(b.ref_name, "owner binding ref_name");
    writeBinaryLittleEndian(static_cast<uint8_t>(b.kind), out);
    writeLenPrefixed(out, b.ref_name);
    writeManifestRef(out, b.manifest_ref);
}

RefOwnerBinding readBinding(ReadBuffer & in)
{
    RefOwnerBinding b;
    uint8_t kind_raw = 0;
    readBinaryLittleEndian(kind_raw, in);
    if (kind_raw != static_cast<uint8_t>(RefOwnerKind::Committed)
        && kind_raw != static_cast<uint8_t>(RefOwnerKind::Precommit))
        throw Exception(ErrorCodes::CORRUPTED_DATA, "RefLogTxn: unknown owner kind {}", kind_raw);
    b.kind = static_cast<RefOwnerKind>(kind_raw);
    b.ref_name = readLenPrefixed(in);
    checkRefName(b.ref_name, "owner binding ref_name");
    b.manifest_ref = readManifestRef(in);
    return b;
}

void writeOp(WriteBuffer & out, const RefOp & op)
{
    writeBinaryLittleEndian(static_cast<uint8_t>(op.kind), out);
    switch (op.kind)
    {
        case RefOpKind::NamespaceBirth:
        case RefOpKind::RemoveNamespace:
            return;
        case RefOpKind::OwnerTransition:
            writeBinaryLittleEndian(static_cast<uint8_t>(op.old_binding.has_value()), out);
            if (op.old_binding)
                writeBinding(out, *op.old_binding);
            writeBinaryLittleEndian(static_cast<uint8_t>(op.new_binding.has_value()), out);
            if (op.new_binding)
                writeBinding(out, *op.new_binding);
            return;
        case RefOpKind::SetPayload:
            checkRefName(op.ref_name, "set_payload ref_name");
            writeLenPrefixed(out, op.ref_name);
            writeManifestRef(out, op.expected_manifest_ref);
            writeLenPrefixed(out, op.payload);
            writeBinaryLittleEndian(op.published_at_ms, out);
            return;
    }
    /// Reachable only through a hand-corrupted `RefOpKind` value (e.g. an out-of-range static_cast);
    /// every named enumerator returns above. Mirrors CasFormat.cpp's exhaustive-switch-then-throw shape.
    throw Exception(ErrorCodes::CORRUPTED_DATA, "RefLogTxn: unknown op kind {}", static_cast<uint8_t>(op.kind));
}

RefOp readOp(ReadBuffer & in)
{
    RefOp op;
    uint8_t kind_raw = 0;
    readBinaryLittleEndian(kind_raw, in);
    switch (kind_raw)
    {
        case static_cast<uint8_t>(RefOpKind::NamespaceBirth):
            op.kind = RefOpKind::NamespaceBirth;
            break;
        case static_cast<uint8_t>(RefOpKind::RemoveNamespace):
            op.kind = RefOpKind::RemoveNamespace;
            break;
        case static_cast<uint8_t>(RefOpKind::OwnerTransition):
        {
            op.kind = RefOpKind::OwnerTransition;
            uint8_t has_old = 0;
            readBinaryLittleEndian(has_old, in);
            if (has_old)
                op.old_binding = readBinding(in);
            uint8_t has_new = 0;
            readBinaryLittleEndian(has_new, in);
            if (has_new)
                op.new_binding = readBinding(in);
            break;
        }
        case static_cast<uint8_t>(RefOpKind::SetPayload):
            op.kind = RefOpKind::SetPayload;
            op.ref_name = readLenPrefixed(in);
            checkRefName(op.ref_name, "set_payload ref_name");
            op.expected_manifest_ref = readManifestRef(in);
            op.payload = readLenPrefixed(in);
            readBinaryLittleEndian(op.published_at_ms, in);
            break;
        default:
            throw Exception(ErrorCodes::CORRUPTED_DATA, "RefLogTxn: unknown op kind {}", kind_raw);
    }
    return op;
}

bool isRemovalClass(const std::vector<RefOp> & ops)
{
    return std::any_of(ops.begin(), ops.end(), [](const RefOp & op) { return op.kind == RefOpKind::RemoveNamespace; });
}

/// A transaction containing `RemoveNamespace` shares the larger complete-table byte budget (spec
/// §Transaction Log Format) and is not separately capped on operation count.
void checkBudget(const std::vector<RefOp> & ops, size_t encoded_bytes)
{
    const bool removal = isRemovalClass(ops);
    const size_t byte_limit = removal ? ref_removal_max_bytes : ref_txn_max_bytes;
    if (encoded_bytes > byte_limit)
        throw Exception(ErrorCodes::CORRUPTED_DATA,
            "RefLogTxn: encoded size {} exceeds the {}-class byte limit {}",
            encoded_bytes, removal ? "removal" : "normal", byte_limit);
    if (!removal && ops.size() > ref_txn_max_ops)
        throw Exception(ErrorCodes::CORRUPTED_DATA,
            "RefLogTxn: {} operations exceeds the normal-class op-count limit {}", ops.size(), ref_txn_max_ops);
}

void checkTxnIdNonzero(const RefTxnId & id)
{
    if (id.writer_epoch == 0 || id.ref_sequence == 0)
        throw Exception(ErrorCodes::CORRUPTED_DATA,
            "RefLogTxn: txn_id fields must both be nonzero, got {}-{}", id.writer_epoch, id.ref_sequence);
}

}

String encodeRefLogTxn(const RefLogTxn & txn)
{
    checkTxnIdNonzero(txn.txn_id);

    WriteBufferFromOwnString out;
    writeBinaryLittleEndian(kRefLogTxnFormatVersion, out);
    writeLenPrefixed(out, txn.ns);
    writeBinaryLittleEndian(txn.txn_id.writer_epoch, out);
    writeBinaryLittleEndian(txn.txn_id.ref_sequence, out);
    writeBinaryLittleEndian(static_cast<uint32_t>(txn.ops.size()), out);
    for (const RefOp & op : txn.ops)
        writeOp(out, op);

    const String bytes = out.str();
    checkBudget(txn.ops, bytes.size());
    return bytes;
}

RefLogTxn decodeRefLogTxn(std::string_view data, const String & expected_ns, const RefTxnId & expected_txn_id)
{
    ReadBufferFromMemory in(data.data(), data.size());
    return decodeGuarded("RefLogTxn", [&]
    {
        uint32_t format_version = 0;
        readBinaryLittleEndian(format_version, in);
        if (format_version != kRefLogTxnFormatVersion)
            throw Exception(ErrorCodes::UNKNOWN_FORMAT_VERSION,
                "RefLogTxn: unknown format_version {}", format_version);

        RefLogTxn txn;
        txn.ns = readLenPrefixed(in);
        readBinaryLittleEndian(txn.txn_id.writer_epoch, in);
        readBinaryLittleEndian(txn.txn_id.ref_sequence, in);
        checkTxnIdNonzero(txn.txn_id);

        if (txn.ns != expected_ns || txn.txn_id != expected_txn_id)
            throw Exception(ErrorCodes::CORRUPTED_DATA,
                "RefLogTxn: body (ns='{}', txn_id={}-{}) does not match the key it was read from "
                "(ns='{}', txn_id={}-{})",
                txn.ns, txn.txn_id.writer_epoch, txn.txn_id.ref_sequence,
                expected_ns, expected_txn_id.writer_epoch, expected_txn_id.ref_sequence);

        uint32_t op_count = 0;
        readBinaryLittleEndian(op_count, in);
        /// Never `reserve(op_count)` directly: it is an untrusted wire field and could demand a huge
        /// up-front allocation before a single byte of it is validated against the actual buffer.
        txn.ops.reserve(std::min<uint32_t>(op_count, 1024));
        for (uint32_t i = 0; i < op_count; ++i)
            txn.ops.push_back(readOp(in));

        checkBudget(txn.ops, data.size());
        return txn;
    });
}

}
