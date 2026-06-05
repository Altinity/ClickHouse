#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/GcDelta.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Codec.h>
#include <IO/ReadBufferFromString.h>
#include <IO/ReadHelpers.h>
#include <IO/VarInt.h>
#include <IO/WriteBufferFromString.h>
#include <IO/WriteHelpers.h>
#include <Common/Exception.h>
#include <Common/SipHash.h>
#include <base/hex.h>

namespace DB
{
namespace ErrorCodes
{
    extern const int CORRUPTED_DATA;
}
}

namespace DB::ContentAddressed
{

std::string GcDelta::computeEventId(const PartId & part_id, Op op, uint64_t generation)
{
    /// SipHash-128 (lowercase hex) over (op, generation, part_id) — stable and reproducible without any
    /// shared state, so the §5.1 rule-2 re-append of the SAME logical delta into a later epoch produces
    /// an identical id and the compaction dedups it on fold. The op byte and generation are folded in via
    /// fixed-width updates; the part id via its string. (No host-byte-order int is exposed: the id is the
    /// hex digest, so the log object key it forms is cross-arch stable.)
    SipHash hash;
    hash.update(static_cast<uint8_t>(op));
    hash.update(generation);
    hash.update(part_id.string());
    return getHexUIntLowercase(hash.get128());
}

std::string GcLogBatch::serialize() const
{
    /// MAGIC(4) + version(1) + body, on the shared codec. The body is a varint count of deltas, then per
    /// delta: the op (1 byte little-endian), the event_id (length-prefixed string — the stable hex id),
    /// the part_id (length-prefixed string), a varint count of pins and that many bare blob hashes (each
    /// length-prefixed). All explicitly little-endian so the object is byte-identical regardless of the
    /// writer's architecture (cross-arch determinism — the log is read by whichever mounter compacts it).
    std::string out;
    DB::WriteBufferFromString buf(out);
    FormatHeader{MAGIC, VERSION}.write(buf);
    DB::writeVarUInt(deltas.size(), buf);
    for (const auto & d : deltas)
    {
        DB::writeBinaryLittleEndian(static_cast<uint8_t>(d.op), buf);
        DB::writeStringBinary(d.event_id, buf);
        DB::writeStringBinary(d.part_id.string(), buf);
        DB::writeVarUInt(d.pins.size(), buf);
        for (const auto & pin : d.pins)
            DB::writeStringBinary(pin.string(), buf);
        /// CA GC S3 (version 2): the resolved manifest generation, then a parallel vector of the resolved
        /// per-pin generations. The pin-generation count is written explicitly (rather than assumed equal
        /// to pins.size()) so a defensive size mismatch fails closed on read.
        DB::writeVarUInt(d.manifest_generation, buf);
        DB::writeVarUInt(d.pin_generations.size(), buf);
        for (uint64_t g : d.pin_generations)
            DB::writeVarUInt(g, buf);
    }
    buf.finalize();
    return out;
}

GcLogBatch GcLogBatch::deserialize(const std::string & bytes)
{
    DB::ReadBufferFromString buf(bytes);
    FormatHeader::readAndValidate(buf, MAGIC, VERSION, "gc delta log");

    GcLogBatch batch;
    uint64_t nd = 0;
    DB::readVarUInt(nd, buf);
    batch.deltas.reserve(nd);
    for (uint64_t i = 0; i < nd; ++i)
    {
        GcDelta d;
        uint8_t op_raw = 0;
        DB::readBinaryLittleEndian(op_raw, buf);
        if (op_raw != static_cast<uint8_t>(GcDelta::Op::Add) && op_raw != static_cast<uint8_t>(GcDelta::Op::Remove))
            throw Exception(ErrorCodes::CORRUPTED_DATA, "ContentAddressed gc delta log: bad op {}", static_cast<uint32_t>(op_raw));
        d.op = static_cast<GcDelta::Op>(op_raw);
        DB::readStringBinary(d.event_id, buf);
        std::string part_id;
        DB::readStringBinary(part_id, buf);
        d.part_id = PartId(std::move(part_id));
        uint64_t np = 0;
        DB::readVarUInt(np, buf);
        d.pins.reserve(np);
        for (uint64_t j = 0; j < np; ++j)
        {
            std::string pin;
            DB::readStringBinary(pin, buf);
            d.pins.emplace_back(std::move(pin));
        }
        /// CA GC S3 (version 2): the resolved manifest generation and the parallel per-pin generations.
        /// The count must either be 0 (no resolved generations recorded — every pin is g=0) or exactly
        /// match the pin count; any other size is a corrupt log object (fail closed).
        DB::readVarUInt(d.manifest_generation, buf);
        uint64_t ng = 0;
        DB::readVarUInt(ng, buf);
        if (ng != 0 && ng != np)
            throw Exception(
                ErrorCodes::CORRUPTED_DATA,
                "ContentAddressed gc delta log: pin_generations count {} does not match pins count {}",
                ng, np);
        d.pin_generations.reserve(ng);
        for (uint64_t j = 0; j < ng; ++j)
        {
            uint64_t g = 0;
            DB::readVarUInt(g, buf);
            d.pin_generations.push_back(g);
        }
        batch.deltas.push_back(std::move(d));
    }
    return batch;
}

}
