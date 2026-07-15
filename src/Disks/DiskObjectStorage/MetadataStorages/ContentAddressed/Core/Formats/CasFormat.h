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
///
/// Raised 1 -> 2 (CAS mixed-algo pools, Phase 3 T5 controller-review extension): the schema-3
/// algo-prefixed settlement key (`SourceEdgeKeyCodec`, `Core/CasBlobInDegree.h`) landed in Task 3 with
/// NO accompanying reader-generation bump, leaving the gate a no-op constant -- a generation-1 build
/// could still open a pool whose GC state it cannot actually decode. `PoolMeta::createOrValidate`
/// (`CasPoolMeta.cpp`) already CAS-raises `min_reader_generation` to `G_BUILD` on every successful
/// open/admission; bumping this constant to 2 makes that raise land at 2 and makes a persisted
/// `min_reader_generation > 2` (e.g. a future generation-3 pool) correctly fail-closed here.
///
/// Raised 2 -> 3 (Task 12, ref snapshot+log format): the table ref state moved from the mutable
/// pre-generation-3 ref-shard objects to immutable `_log`/`_snap` objects -- a BREAKING ref-format
/// change. The forward gate above is not sufficient (a generation-2 pool's `compatibility_version` still
/// passes `<= G_BUILD`), so `decodePoolMeta` adds an explicit BACKWARD floor: a pool whose pool-meta
/// `compatibility_version < 3` holds unreadable pre-generation-3 ref-shard-format refs and fails closed
/// at open. CAS is pre-release, so no migration exists -- such a pool must be recreated.
constexpr uint32_t G_BUILD = 3;

/// The pool-format generation at which the ref state became immutable snapshot+log objects (Task 12).
/// A pool-meta `compatibility_version` below this cannot be opened by this build (its refs are the
/// removed pre-generation-3 mutable ref-shard format). See the backward floor in
/// `CasPoolMeta.cpp::decodePoolMeta`.
constexpr uint32_t kRefSnapshotLogGeneration = 3;

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
    /// v3 text-format cutover (2026-07-15 design): ids for persisted objects that predate the
    /// registry — refsnaplog, the blob-meta sidecar, and the GC heartbeat (formerly the 24-byte
    /// unversioned exception). Values are frozen; never reuse.
    RefLog = 19,          /// cas_ref_log     — ref transaction log object
    RefSnapshot = 20,     /// cas_ref_snap    — complete per-namespace ref table
    BlobMeta = 21,        /// cas_blob_meta   — per-blob freshness sidecar
    GcHeartbeat = 22,     /// cas_gc_hb       — GC leader heartbeat
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

/// ---- v3 text-format registry -----------------------------------------------------------------
/// One row per persisted object class (spec 2026-07-15 §corrected-object-inventory). The row is
/// the single source of the header-line `type`, the family, the strictness of unknown keys, the
/// compression policy, and the fail-closed size caps. A format missing here cannot be decoded.

enum class TextFamily : uint8_t { Control = 1, RecordStream = 2, PayloadHybrid = 3 };
enum class KeyStrictness : uint8_t { Tolerant = 1, Strict = 2 };
enum class CompressionPolicy : uint8_t { Never = 1, Always = 2, PinnedRaw = 3 };

struct FormatTraits
{
    FormatId id;
    std::string_view type;      /// header-line "type" value
    TextFamily family;
    KeyStrictness strictness;
    CompressionPolicy compression;
    uint64_t object_cap;        /// max DECOMPRESSED object bytes; 0 = uncapped (streamed)
    uint64_t line_cap;          /// max bytes of one text line
};

/// Traits for `id`. LOGICAL_ERROR for `FormatId::Roster` (reserved, unbuilt — has no traits row).
const FormatTraits & traitsFor(FormatId id);
/// Traits for a header-line `type` string, or nullptr when `type` is not a registered format.
const FormatTraits * traitsForType(std::string_view type);
/// The key-suffix a stored object of `id` is written under: ".zst" when its compression policy is
/// `Always`, "" otherwise. Callers building a key never inspect the body to decide the suffix.
std::string_view storedSuffix(FormatId id);

}
