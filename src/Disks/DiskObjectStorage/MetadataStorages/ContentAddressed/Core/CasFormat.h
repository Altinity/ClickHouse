#pragma once
#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>

namespace DB::Cas
{

/// The highest pool-format generation this build understands. A build keeps every decoder for
/// generations 1..G_BUILD (new code always reads old); an object is readable iff its
/// min_reader_version <= G_BUILD. Bump this (and append a change-point in CasFormat.cpp) when a new
/// format generation is introduced.
constexpr uint16_t G_BUILD = 1;

/// The registry of every self-describing persisted object class. For hashed binary objects the 4-byte
/// magic doubles as the on-disk identifier; this enum is what the version tables key on.
enum class FormatId : uint16_t
{
    Blob = 1,
    Tree = 2,
    Manifest = 3,
    GcSnap = 4,
    GcState = 5,
    RetiredSet = 6,
    Watermark = 7,
    PoolMeta = 8,
    Roster = 9,
};

/// One entry of a class's format history: at global generation `generation` the class's serialization
/// changed, and a reader must understand at least `min_reader` to read an object written at it.
/// Additive change => append {gen, <prior min_reader>}; breaking change => append {gen, gen}.
struct FormatChangePoint
{
    uint16_t generation;
    uint16_t min_reader;
};

/// The append-only change-point history for `id`, oldest first. Generation 1 is the frozen baseline
/// ({1,1} for every class today).
std::span<const FormatChangePoint> changePoints(FormatId id);

/// What a writer stamps onto an object when writing class `id` at write-floor `floor`:
/// writer_version = newest generation <= floor that this class has a format for; min_reader_version =
/// that change-point's min_reader. With one generation defined, always {1,1}.
struct WriterStamp
{
    uint16_t writer_version;
    uint16_t min_reader_version;
};
WriterStamp currentWriterVersion(FormatId id, uint16_t floor = G_BUILD);

/// THE reader rule. `min_reader_version` is read from an object's header; if it exceeds what this
/// build understands (G_BUILD), fail closed with UNKNOWN_FORMAT_VERSION — never misread a future
/// object. `what` names the object in the message.
void gateOnRead(uint16_t min_reader_version, std::string_view what);

}
