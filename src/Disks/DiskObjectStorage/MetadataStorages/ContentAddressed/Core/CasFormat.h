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
    /// 2 (Tree) and 4 (GcSnap) retired in the rev. 15 root-local part-manifest redesign — no on-disk
    /// compat to honor (CA pre-release). The survivors keep their numeric values; no two share a value.
    Manifest = 3,
    GcState = 5,
    /// 6 (RetiredSet, magic CART) retired 2026-07-10 with the retired-in-snapshot refactor — condemned
    /// state rides the source-edge runs + fold-seal condemned_summary. Id 6 and magic CART (0x54524143)
    /// are freed; never reuse.
    /// 7 (Watermark) retired with the ack-floor merge — the build-watermark floor rides the mount-lease
    /// beat (MountLease), there is no standalone watermark object anymore.
    PoolMeta = 8,
    Roster = 9,
    /// 10 (RootsRegistry) deleted in Task 4 — discovery authority moved to LIST(cas/refs/).
    GcOutcomes = 11,
    /// Phase 1a (CA GC root-local part-manifest redesign):
    PartManifest = 12,    /// immutable root-local part manifest body; magic "CAPT" (see plan note: "CAPM" is taken by PoolMeta)
    RunFile = 13,         /// dense block-framed sorted binary data-plane run; magic "CARN"
    FoldSeal = 14,        /// write-once gc/gen/<gen>/fold_seal (coverage + blob_target/cleanup runs); magic "CAFS"
    /// 15 (CompletionSeal) retired with the one-pass ack-floor round (fence/recheck/delete/trim phases
    /// removed) — the fold seal is now the sole per-generation coverage record.
    /// Phase 0 (mount safety): per-server-root control objects under gc/server-roots/<server_root_id>/.
    Owner = 16,           /// owner anchor (server_root_id -> server UUID); magic "CAOW"
    ServerEpoch = 17,     /// writer-epoch fence (next_writer_epoch); magic "CAEP"
    MountLease = 18,      /// live mount lease; magic "CAML"
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
