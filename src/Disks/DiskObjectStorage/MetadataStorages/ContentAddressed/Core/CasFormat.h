#pragma once
#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>

namespace DB::Cas
{

/// The highest pool-format generation this build understands. A build keeps every decoder for
/// generations 1..G_BUILD (new code always reads old); an object is readable iff its
/// compatibility_version <= G_BUILD. Bump this (and append a change-point in CasFormat.cpp) when a new
/// format generation is introduced.
constexpr uint32_t G_BUILD = 1;

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
    RootsRegistry = 10,
    GcOutcomes = 11,
};

/// Per-type magic: the 4 ASCII bytes of each object class encoded as a little-endian uint32. Used in
/// CasHeader.magic. Throws LOGICAL_ERROR for an unexpected id.
uint32_t magicFor(FormatId id);

/// What this build stamps as writer_version on every object it writes. Pre-roster: always G_BUILD.
uint32_t currentWriterVersion();

/// What this build stamps as compatibility_version on every object it writes. Pre-roster: always
/// G_BUILD (the write-down-to-floor branch defers until a roster is available). Readers check
/// compatibility_version > G_BUILD and fail closed.
uint32_t currentCompatibilityVersion();

/// THE reader rule. `compatibility_version` is read from an object's CasHeader; if it exceeds what
/// this build understands (G_BUILD), fail closed with UNKNOWN_FORMAT_VERSION — never misread a
/// future object. `what` names the object class in the error message.
void checkCompatibility(uint32_t compatibility_version, std::string_view what);

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

/// THE reader rule for the hashed binary envelope (CasEnvelope). `compatibility_version` is the
/// slot in the binary envelope core that was previously named `min_reader_version`; the semantics
/// are unchanged: > G_BUILD → fail-closed. `what` names the object in the error message.
/// (Used by CasEnvelope.cpp; kept here to avoid a circular dependency.)
void gateOnRead(uint32_t compatibility_version, std::string_view what);

}
