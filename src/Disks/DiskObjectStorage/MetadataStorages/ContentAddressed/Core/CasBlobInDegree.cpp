#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasBlobInDegree.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasRunFile.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasCodecUtil.h>
#include <IO/WriteBufferFromString.h>
#include <base/defines.h>
#include <Common/Exception.h>
#include <Common/logger_useful.h>
#include <city.h>
#include <algorithm>
#include <cstring>
#include <memory>
#include <optional>

namespace DB
{
namespace ErrorCodes
{
    extern const int CORRUPTED_DATA;
    extern const int LOGICAL_ERROR;
    extern const int NOT_IMPLEMENTED;
}
}

namespace DB::Cas
{

namespace
{

const UInt128 kZeroSourceId{0};

UInt128 cityHash128(const String & bytes)
{
    const auto h = CityHash_v1_0_2::CityHash128(bytes.data(), bytes.size());
    return (static_cast<UInt128>(h.high64) << 64) | static_cast<UInt128>(h.low64);
}

/// Streams a shard's prior source-edge run at O(one block) resident memory: chains the run SEGMENTS the
/// caller resolved from the parent seal (`blob_target_runs` filtered to one shard) and exposes a one-row
/// lookahead the fold merge consumes. Two-cursor / retired-in-snapshot (spec §2.1): the prior run carries
/// BOTH surviving edges (`kEdgeActive`) AND the retired `kCondemned` sentinel rows at the zero source id,
/// so the cursor stops at edges AND at condemned rows (exposing the type via `rowType`), while zero-marker
/// sentinels are dropped on carry (per-generation, never carried forward). Row/key invariants are enforced
/// while streaming (spec §2.1): `kEdgeActive` never at `source_id = 0`; sentinel rows (`kZeroMarker` /
/// `kCondemned`) ONLY at `source_id = 0`; at most one sentinel per blob; an unknown value byte or an empty
/// payload is CORRUPTED_DATA. Resolution is BY REF (2026-07-02 T0): the caller passes the exact object
/// keys, so a run sealed for generation G that physically lives under an older generation's key is reached
/// without key construction. An empty `segments` is the fresh-pool / empty baseline. The row stream is
/// globally sorted by (blob_hash, source_id), so `key()` values are non-decreasing.
class PriorEdgeCursor
{
public:
    /// `out_codec` is the FOLD'S OUTPUT codec (Task 4): the load-bearing coherence gate. Every prior
    /// run segment's own `key_schema` must equal `out_codec.keySchema()`, checked the moment the
    /// segment is opened — BEFORE any row of that segment is parsed or compared. Cross-width raw-byte
    /// comparison is unsafe (a schema-1 key puts `source_id` where a schema-2 key puts digest-suffix
    /// bytes), so a mismatch throws `CORRUPTED_DATA` here rather than silently mis-merging.
    PriorEdgeCursor(Backend & backend_, const std::vector<RunRef> & segments_, const SourceEdgeKeyCodec & out_codec_)
        : backend(backend_), segments(segments_), out_codec(out_codec_)
    {
        advance();
    }

    bool valid() const { return has_current; }
    const String & key() const { return current_key; }
    /// The value byte of the current row: `kEdgeActive` (a surviving edge) or `kCondemned` (a retired
    /// sentinel row). Zero markers are never surfaced (dropped on carry).
    char rowType() const { return current_type; }
    /// The decoded retired sentinel for the current row (only valid when `rowType() == kCondemned`).
    const CondemnedRow & condemnedRow() const { return current_condemned; }

    /// Advance to the next surviving edge OR retired sentinel, dropping zero markers, enforcing the
    /// row/key invariants, and crossing segment boundaries.
    void advance()
    {
        while (true)
        {
            /// Pull rows from the open segment until a surviving edge / retired sentinel or the segment ends.
            if (reader)
            {
                String k;
                String p;
                while (reader->next(k, p))
                {
                    BlobDigest bh;
                    UInt128 sid;
                    /// `out_codec` is the fold's output codec; the segment-open check above already
                    /// proved this segment's key_schema matches it, so every row is this codec's width.
                    /// `parse` throws CORRUPTED_DATA on a malformed size (fail-closed).
                    out_codec.parse(k, bh, sid);
                    if (p.empty())
                        throw Exception(ErrorCodes::CORRUPTED_DATA, "CAS source-edge run: empty row payload");
                    const char v = p[0];
                    const bool sentinel_key = (sid == kZeroSourceId);

                    if (sentinel_key)
                    {
                        /// A sentinel key carries exactly one row per blob and never an edge.
                        if (v == kEdgeActive)
                            throw Exception(ErrorCodes::CORRUPTED_DATA,
                                "CAS source-edge run: active edge at the reserved sentinel source_id 0");
                        if (v != kZeroMarker && v != kCondemned)
                            throw Exception(ErrorCodes::CORRUPTED_DATA,
                                "CAS source-edge run: unknown sentinel row type 0x{:02x}", static_cast<uint8_t>(v));
                        if (have_sentinel_blob && sentinel_blob == bh)
                            throw Exception(ErrorCodes::CORRUPTED_DATA,
                                "CAS source-edge run: duplicate sentinel row for one blob");
                        have_sentinel_blob = true;
                        sentinel_blob = bh;
                        if (v == kZeroMarker)
                            continue;   // per-generation zero marker: dropped, not carried forward
                        /// A retired sentinel: decode and surface it (settled at close-out, not an edge).
                        current_condemned = decodeCondemnedRow(p);
                        current_key = k;
                        current_type = kCondemned;
                        has_current = true;
                        return;
                    }

                    /// A non-sentinel key must carry a surviving edge and nothing else.
                    if (v != kEdgeActive)
                        throw Exception(ErrorCodes::CORRUPTED_DATA,
                            "CAS source-edge run: sentinel row type 0x{:02x} at a non-sentinel key",
                            static_cast<uint8_t>(v));
                    current_key = k;
                    current_type = kEdgeActive;
                    has_current = true;
                    return;
                }
                reader.reset();
                ++seg_idx;
            }

            /// Open the next resolved segment; the segment list is exhausted => the chain is done.
            if (seg_idx >= segments.size())
            {
                has_current = false;
                return;
            }
            /// Typed open (spec §2.1): every source-edge run reader goes through openSourceEdgeRun.
            reader = std::make_unique<RunFileReader>(openSourceEdgeRun(backend, segments[seg_idx].key));
            /// THE LOAD-BEARING COHERENCE GATE (Task 4, consult finding 1 & 4): this segment's own
            /// key_schema must match the fold's output schema, checked BEFORE any row of the segment
            /// is read/parsed/compared. A schema-1 segment folded into a schema-2 output (or the
            /// reverse) is refused here, not discovered later via a corrupted merge.
            if (reader->keySchema() != out_codec.keySchema())
                throw Exception(ErrorCodes::CORRUPTED_DATA,
                    "CAS gc fold: prior source-edge run schema {} != output schema {} (cross-width "
                    "merge refused)", reader->keySchema(), out_codec.keySchema());
        }
    }

private:
    Backend & backend;
    const std::vector<RunRef> & segments;
    const SourceEdgeKeyCodec & out_codec;

    size_t seg_idx = 0;
    std::unique_ptr<RunFileReader> reader;
    String current_key;
    char current_type = kEdgeActive;
    CondemnedRow current_condemned;
    bool has_current = false;

    /// Duplicate-sentinel guard: the last blob for which a sentinel row was seen (across skipped zero
    /// markers too). Rows are globally sorted, so two sentinels for one blob are adjacent.
    bool have_sentinel_blob = false;
    BlobDigest sentinel_blob{};
};

}

UInt128 sourceEdgeId(const ManifestId & id, const String & path)
{
    String canon;
    canon += id.root_namespace.string();
    canon += '\0';
    auto beU64 = [&](uint64_t v) { for (int i = 7; i >= 0; --i) canon += static_cast<char>((v >> (8 * i)) & 0xFF); };
    auto beU32 = [&](uint32_t v) { for (int i = 3; i >= 0; --i) canon += static_cast<char>((v >> (8 * i)) & 0xFF); };
    beU64(id.ref.writer_epoch); beU64(id.ref.build_sequence); beU32(id.ref.manifest_ordinal);
    canon += '\0';
    canon += path;
    const auto h = CityHash_v1_0_2::CityHash128(canon.data(), canon.size());
    const UInt128 result = (static_cast<UInt128>(h.high64) << 64) | static_cast<UInt128>(h.low64);
    if (result == UInt128{0})
        throw Exception(ErrorCodes::LOGICAL_ERROR,
            "CAS source edge: hash collided with the reserved sentinel id 0");
    return result;
}

void assertValidSourceEdgeId(const UInt128 & source_id)
{
    if (source_id == UInt128{0})
        throw Exception(ErrorCodes::LOGICAL_ERROR,
            "CAS source edge: source_id 0 is the reserved sentinel key (spec §2.1)");
}

String encodeCondemnedRow(const CondemnedRow & row)
{
    String out;
    out.push_back(kCondemned);
    out.push_back(static_cast<char>(row.delete_pending ? 1 : 0));
    out.push_back(static_cast<char>(row.token.type));
    auto beU64 = [&](uint64_t v) { for (int i = 7; i >= 0; --i) out += static_cast<char>((v >> (8 * i)) & 0xFF); };
    beU64(row.condemn_round);
    beU64(row.size);
    if (row.token.value.size() > 0xFFFF)
        throw Exception(ErrorCodes::CORRUPTED_DATA, "CAS condemned row: token too long ({})", row.token.value.size());
    out += static_cast<char>((row.token.value.size() >> 8) & 0xFF);
    out += static_cast<char>(row.token.value.size() & 0xFF);
    out += row.token.value;
    return out;
}

CondemnedRow decodeCondemnedRow(std::string_view p)
{
    /// [0]=0x02 [1]=flags [2]=token_type [3..10]=round [11..18]=size [19..20]=len [21..]=value
    constexpr size_t kFixed = 21;
    if (p.size() < kFixed || p[0] != kCondemned)
        throw Exception(ErrorCodes::CORRUPTED_DATA, "CAS condemned row: malformed header");
    CondemnedRow row;
    const uint8_t flags = static_cast<uint8_t>(p[1]);
    if (flags > 1)
        throw Exception(ErrorCodes::CORRUPTED_DATA, "CAS condemned row: unknown flags 0x{:02x}", flags);
    row.delete_pending = flags & 1;
    const uint8_t type = static_cast<uint8_t>(p[2]);
    if (type < 1 || type > 3)
        throw Exception(ErrorCodes::CORRUPTED_DATA, "CAS condemned row: unknown token_type {}", type);
    row.token.type = static_cast<TokenType>(type);
    auto beU64 = [&](size_t off) { uint64_t v = 0; for (int i = 0; i < 8; ++i) v = (v << 8) | static_cast<uint8_t>(p[off + i]); return v; };
    row.condemn_round = beU64(3);
    row.size = beU64(11);
    const size_t len = (static_cast<uint8_t>(p[19]) << 8) | static_cast<uint8_t>(p[20]);
    if (p.size() != kFixed + len)
        throw Exception(ErrorCodes::CORRUPTED_DATA, "CAS condemned row: declared token_len {} vs payload {}", len, p.size() - kFixed);
    row.token.value = String(p.substr(kFixed, len));
    return row;
}

namespace
{
void assertSourceEdgeRunHeader(const RunFileReader & reader)
{
    if (reader.kind() != RunKind::SourceEdge)
        throw Exception(ErrorCodes::CORRUPTED_DATA, "CAS source-edge run: wrong kind {}", static_cast<int>(reader.kind()));
    /// Widened set (Task 4): key_schema ∈ {kSourceEdgeKeySchema128, kSourceEdgeKeySchemaSha256}; any
    /// other value (including the pre-Task-4 unset 0) is NOT_IMPLEMENTED — sourceEdgeDigestLen throws.
    sourceEdgeDigestLen(reader.keySchema());
}
}

RunFileReader openSourceEdgeRun(std::string_view bytes)
{
    RunFileReader reader(bytes);
    assertSourceEdgeRunHeader(reader);
    return reader;
}

RunFileReader openSourceEdgeRun(Backend & backend, const String & key)
{
    RunFileReader reader(backend, key);
    assertSourceEdgeRunHeader(reader);
    return reader;
}

uint8_t sourceEdgeDigestLen(uint8_t key_schema)
{
    switch (key_schema)
    {
        case kSourceEdgeKeySchema128:
            return 16;
        case kSourceEdgeKeySchemaSha256:
            return 32;
        default:
            throw Exception(ErrorCodes::NOT_IMPLEMENTED,
                "CAS source-edge run: key_schema {} (this build supports {} and {})",
                key_schema, kSourceEdgeKeySchema128, kSourceEdgeKeySchemaSha256);
    }
}

uint8_t sourceEdgeKeySchemaFor(uint8_t digest_len)
{
    if (digest_len == 16)
        return kSourceEdgeKeySchema128;
    if (digest_len == 32)
        return kSourceEdgeKeySchemaSha256;
    throw Exception(ErrorCodes::NOT_IMPLEMENTED,
        "CAS source-edge key codec: digest length {} bytes is not a supported source-edge key width "
        "(16 or 32)", digest_len);
}

SourceEdgeKeyCodec::SourceEdgeKeyCodec(uint8_t digest_len_) : digest_len(digest_len_)
{
    /// Validates via sourceEdgeKeySchemaFor (throws NOT_IMPLEMENTED on an unsupported width) rather
    /// than duplicating the 16/32 check — one authority for "is this a supported digest width".
    sourceEdgeKeySchemaFor(digest_len);
}

SourceEdgeKeyCodec SourceEdgeKeyCodec::forSchema(uint8_t key_schema)
{
    return SourceEdgeKeyCodec(sourceEdgeDigestLen(key_schema));
}

namespace
{
/// len-drift guard (mirrors DigestCodec::checkZeroTail, CasBlobDigest.h): a caller passing a digest
/// wider than this codec's width is a programming bug, not corrupted on-disk data — chassert, not throw.
void checkZeroTailForCodec(const BlobDigest & d, uint8_t digest_len, [[maybe_unused]] const char * what)
{
    for (size_t i = digest_len; i < d.bytes.size(); ++i)
        chassert(d.bytes[i] == 0, fmt::format("SourceEdgeKeyCodec::{}: non-zero byte at tail position {} (digest_len={})", what, i, digest_len));
}
}

String SourceEdgeKeyCodec::key(const BlobDigest & blob_hash, const UInt128 & source_id) const
{
    checkZeroTailForCodec(blob_hash, digest_len, "key");
    return String(reinterpret_cast<const char *>(blob_hash.bytes.data()), digest_len) + u128ToBytesBE(source_id);
}

void SourceEdgeKeyCodec::parse(std::string_view key, BlobDigest & blob_hash, UInt128 & source_id) const
{
    if (key.size() != static_cast<size_t>(digest_len) + 16)
        throw Exception(ErrorCodes::CORRUPTED_DATA,
            "CAS source-edge run: malformed key ({} bytes, expected {})", key.size(), digest_len + 16);
    blob_hash = BlobDigest{};
    memcpy(blob_hash.bytes.data(), key.data(), digest_len);
    source_id = u128FromBytesBE(String(key.substr(digest_len, 16)), "src-edge run key source_id");
}

String SourceEdgeKeyCodec::seekPrefix(const BlobDigest & blob_hash) const
{
    checkZeroTailForCodec(blob_hash, digest_len, "seekPrefix");
    return String(reinterpret_cast<const char *>(blob_hash.bytes.data()), digest_len);
}

void putDeterministicArtifact(Backend & backend, const String & key, const String & bytes)
{
    if (backend.putIfAbsent(key, bytes).outcome == PutOutcome::PreconditionFailed)
    {
        const auto existing = backend.get(key);
        if (!existing || existing->bytes != bytes)
            throw Exception(ErrorCodes::CORRUPTED_DATA,
                "CAS gc: deterministic artifact at {} occupied by divergent bytes (impossible under "
                "correct operation; refusing to proceed)", key);
        /// byte-equal => our own deterministic replay; adopt (no-op).
    }
}

void foldDeltasIntoGeneration(Backend & backend, const Layout & layout,
                              const std::vector<RunRef> & prior_runs,
                              uint64_t new_generation, uint64_t attempt,
                              uint64_t shard,
                              std::vector<BlobDelta> scattered, std::vector<RunRef> & out_runs,
                              uint64_t current_round, uint64_t condemn_round,
                              const std::function<std::optional<HeadResult>(const BlobDigest &)> & head_blob,
                              const std::function<std::optional<HeadResult>(const BlobDigest &)> & peek_head,
                              RetiredMergeResult * out_retired,
                              bool suppress_destructive,
                              uint8_t digest_len)
{
    RetiredMergeResult sink;
    RetiredMergeResult & rmr = out_retired ? *out_retired : sink;

    /// ONE write codec for this fold, built from the OUTPUT width (Task 4). Every key build/parse
    /// below — including the prior-run coherence gate inside `PriorEdgeCursor` — goes through it.
    const SourceEdgeKeyCodec out_codec(digest_len);

    // Deterministic input ordering => byte-reproducible run (OQ5 resume/adoption).
    // MUST be stable: for the same (blob_hash, source_id) the journal ordering is
    // activation-before-removal; "last wins" then correctly resolves to removal (edge absent).
    // An unstable sort can put removal before activation => last=activation => false positive.
    // BlobDigest's defaulted operator<=> is lexicographic over the 32-byte array, which for a
    // WIDTH-HOMOGENEOUS run (every digest zero-tailed beyond digest_len) equals numeric magnitude
    // order — bit-identical to the old UInt128 `<` for every 128-bit digest (design consult).
    std::stable_sort(scattered.begin(), scattered.end(),
        [](const BlobDelta & a, const BlobDelta & b)
        {
            if (a.blob_hash != b.blob_hash) return a.blob_hash < b.blob_hash;
            return a.source_id < b.source_id;
        });

    PriorEdgeCursor cursor(backend, prior_runs, out_codec);

    DB::WriteBufferFromOwnString out;
    RunHeader header;
    header.kind = RunKind::SourceEdge;
    header.key_schema = out_codec.keySchema();   // (blob_hash, source_id) fixed-width + zero-sentinel rows
    RunFileWriter writer(out, header);

    // Streaming two-cursor merge over the prior run (surviving edges by 32-byte key AND retired kCondemned
    // sentinel rows at the zero source id) and this round's edge deltas (by (blob_hash, source_id)). All
    // rows for one blob are adjacent in both inputs; the sentinel key (source_id 0) sorts first. We resolve
    // final presence per edge locally (idempotent: prior present + activate => present; any remove =>
    // absent), settle each blob's carried retired row against its post-merge in-degree at close-out, and
    // re-emit the surviving retired rows / zero-transition markers. O(block) IO + O(1) per current blob.
    size_t di = 0;
    BlobDigest cur_blob{};
    bool have_blob = false;
    uint64_t cur_edges = 0;              // surviving edges of cur_blob so far
    bool cur_touched = false;            // cur_blob had prior edges or deltas this generation
    std::optional<CondemnedRow> cur_condemned;   // the retired sentinel carried on the prior run for cur_blob

    auto toRetiredEntry = [](const BlobDigest & hash, const CondemnedRow & r) -> RetiredEntry
    {
        RetiredEntry e;
        e.kind = ObjectKind::Blob;
        e.hash = hash;
        e.token = r.token;
        e.size = r.size;
        e.condemn_round = r.condemn_round;
        e.delete_pending = r.delete_pending;
        return e;
    };
    auto toCondemnedRow = [](const RetiredEntry & e) -> CondemnedRow
    {
        return CondemnedRow{.delete_pending = e.delete_pending, .token = e.token,
                            .size = e.size, .condemn_round = e.condemn_round};
    };

    auto settleEntry = [&](const RetiredEntry & e, uint64_t indeg)
    {
        chassert(e.kind == ObjectKind::Blob);   /// the in-degree merge settles Blob entries only
        if (indeg > 0)
        {
            /// A delete_pending entry recovering in-degree is structurally impossible (a published pending
            /// blob should never be re-referenced) but IS reachable under real races (T1 TLA finding).
            /// Spare it LOUDLY — never a fail-closed abort and never a delete of a re-referenced blob.
            if (e.delete_pending)
                LOG_WARNING(getLogger("CasGcFold"),
                    "CAS gc fold: a delete_pending retired entry recovered in-degree {} — structurally "
                    "impossible but reachable under races; sparing (never a fail-closed delete)", indeg);
            rmr.spared.push_back(e);            /// recovery wins, even past the floor
        }
        else if (e.delete_pending)
        {
            if (suppress_destructive)
                rmr.still_retired.push_back(e); /// clamp-suppressed pass: carry pending UNCHANGED
            else
                rmr.redelete.push_back(e);      /// published pending by a PRIOR pass — execute + drop
        }
        else if (!suppress_destructive && e.condemn_round < current_round)
        {
            RetiredEntry pending = e;           /// newly floor-passed: publish pending; delete NEXT pass
            pending.delete_pending = true;
            rmr.graduated.push_back(pending);
            rmr.still_retired.push_back(std::move(pending));
        }
        else
            rmr.still_retired.push_back(e);     /// carried unchanged until the floor passes it
    };

    auto closeBlob = [&]()
    {
        if (!have_blob)
            return;
        const size_t retired_before = rmr.still_retired.size();

        /// Settle the retired row carried on the prior run for the blob being closed, against its
        /// post-merge in-degree...
        if (cur_condemned)
        {
            /// `RetiredEntry.hash` is a native `BlobDigest` (Phase 2 Task 5) — `cur_blob` passes straight
            /// through, no narrowing shim.
            const RetiredEntry stale = toRetiredEntry(cur_blob, *cur_condemned);
            /// RESURRECT-REUPLOAD-ORPHAN: on a re-reference cycle (touched this window, net in-degree 0),
            /// re-observe the CURRENT token. If it differs from the retired row's token, a resurrect
            /// replaced the incarnation at this key — supersede the stale entry with a fresh condemn of the
            /// current token so the replacement enters the pipeline (the stale token's exact-token delete
            /// would only find the new token and no-op). Keyed on (hash, current token), matching GRetire.
            /// `peek_head` (audit fix, 2026-07-08): a SIDE-EFFECT-FREE peek, deliberately NOT `head_blob` —
            /// `head_blob` is the fresh-condemn hook (emits `BlobRetire` + increments
            /// `CasGcRetiredCondemned`); calling it here would double-emit `blob_retire` alongside the
            /// `blob_retire_replaced` this supersede already produces below, and double-count the
            /// condemned counter for one physical condemnation.
            bool superseded = false;
            if (cur_edges == 0 && cur_touched && peek_head)
            {
                if (const auto hr = peek_head(cur_blob);
                    hr && hr->exists && hr->token != stale.token)
                {
                    RetiredEntry fresh;
                    fresh.kind = ObjectKind::Blob;
                    fresh.hash = cur_blob;
                    fresh.token = hr->token;
                    fresh.size = hr->size;
                    fresh.condemn_round = condemn_round;
                    ReplacedEntry re;
                    re.old_token = stale.token;                 /// the stale token this supersede replaces
                    re.fresh = fresh;
                    rmr.replaced.push_back(std::move(re));       /// caller emits blob_retire_replaced
                    rmr.still_retired.push_back(std::move(fresh));
                    superseded = true;
                }
            }
            if (!superseded)
                settleEntry(stale, cur_edges);
        }
        /// ...or condemn a fresh transition-to-zero (no carried row). `head_blob` captures the exact
        /// incarnation token for the later exact-token delete; an absent object needs no entry.
        else if (cur_edges == 0 && cur_touched && head_blob)
        {
            if (const auto hr = head_blob(cur_blob); hr && hr->exists)
            {
                RetiredEntry fresh;
                fresh.kind = ObjectKind::Blob;
                fresh.hash = cur_blob;
                fresh.token = hr->token;
                fresh.size = hr->size;
                fresh.condemn_round = condemn_round;
                rmr.still_retired.push_back(std::move(fresh));
            }
        }

        /// Emit AT MOST ONE sentinel row per blob (spec §2.1, invariant 4): the `kCondemned` row when the
        /// blob is condemned/carried/graduated this pass (still_retired grew for it), else a per-generation
        /// `kZeroMarker` when it transitioned to zero this pass but was not condemned (redelete-dropped or
        /// absent-at-condemn). A blob with surviving edges (cur_edges > 0) emits neither — its edge rows
        /// were appended inline, and a condemned/zeroed blob has NO surviving edges, so appending the
        /// sentinel now (its key sorts first for the blob, and no edge rows precede it) keeps the run
        /// sorted. `still_retired` therefore mirrors exactly the emitted `kCondemned` rows, in order.
        if (rmr.still_retired.size() > retired_before)
            writer.append(out_codec.key(cur_blob, kZeroSourceId),
                          encodeCondemnedRow(toCondemnedRow(rmr.still_retired.back())));
        else if (cur_edges == 0 && cur_touched)
            writer.append(out_codec.key(cur_blob, kZeroSourceId), String(1, kZeroMarker));
    };
    auto openBlobIfNeeded = [&](const BlobDigest & b)
    {
        if (!have_blob || b != cur_blob)
        {
            closeBlob();
            cur_blob = b; have_blob = true; cur_edges = 0; cur_touched = false; cur_condemned.reset();
        }
    };

    while (cursor.valid() || di < scattered.size())
    {
        // Pick the smallest row key across the prior-run cursor and this round's deltas.
        String key;
        bool from_prior = false;
        if (cursor.valid()) { key = cursor.key(); from_prior = true; }
        if (di < scattered.size())
        {
            const String dk = out_codec.key(scattered[di].blob_hash, scattered[di].source_id);
            if (!from_prior || dk < key) { key = dk; from_prior = false; }
        }

        BlobDigest blob_hash;
        UInt128 source_id;
        out_codec.parse(key, blob_hash, source_id);   // throws CORRUPTED_DATA on a malformed key (fail-closed)
        openBlobIfNeeded(blob_hash);

        /// A retired sentinel row from the prior run: stash it for close-out settlement. It is NOT an edge
        /// and NEVER a touch — a carried kCondemned row must not force a zero-marker or a peek_head HEAD
        /// (spec §2.1, invariant 2). Deltas never key the zero source id, so no delta merges at this key.
        if (from_prior && cursor.rowType() == kCondemned)
        {
            cur_condemned = cursor.condemnedRow();
            cursor.advance();
            continue;
        }

        bool present = false;
        if (from_prior && cursor.key() == key) { present = true; cursor.advance(); cur_touched = true; }
        while (di < scattered.size()
               && scattered[di].blob_hash == blob_hash && scattered[di].source_id == source_id)
        {
            present = scattered[di].remove ? false : true;   // apply in order; last wins
            cur_touched = true;
            ++di;
        }

        if (present)
        {
            writer.append(key, String(1, kEdgeActive));
            ++cur_edges;
        }
    }
    closeBlob();

    writer.finish();
    const String run_bytes = out.str();
    const String run_key = layout.blobTargetRunKey(new_generation, attempt, shard, 0);
    putDeterministicArtifact(backend, run_key, run_bytes);
    out_runs.push_back(RunRef{.key = run_key, .checksum = cityHash128(run_bytes),
                              .shard = shard, .generation = new_generation});
}

std::vector<BlobCandidate> zeroInDegree(Backend & backend, const std::vector<RunRef> & runs)
{
    std::vector<BlobCandidate> result;
    for (const RunRef & run : runs)
    {
        /// Resolution is by ref (2026-07-02 T0): the caller passed the exact object key, so a run sealed
        /// for a later generation but physically living under an older key is reached directly. The run is
        /// streamed at O(one block) resident memory, never materialized whole. `openSourceEdgeRun` enforces
        /// the run kind + key schema; `kCondemned` sentinel rows are skipped (only `kZeroMarker` counts).
        RunFileReader r = openSourceEdgeRun(backend, run.key);
        /// Readers derive width from the run's OWN key_schema — never from pool meta (Task 4): a run
        /// is decoded by its own header.
        const SourceEdgeKeyCodec codec = SourceEdgeKeyCodec::forSchema(r.keySchema());
        String k, p;
        while (r.next(k, p))
            if (!p.empty() && p[0] == kZeroMarker)
            {
                BlobDigest bh;
                UInt128 sid;
                /// `parse` throws CORRUPTED_DATA on a malformed key — fail-closed (Task 4 consult
                /// finding 5): the pre-Task-4 code silently skipped a malformed row here instead.
                codec.parse(k, bh, sid);
                result.push_back(BlobCandidate{.hash = bh});
            }
    }
    return result;
}

int64_t inDegreeInGeneration(Backend & backend, const std::vector<RunRef> & runs, const BlobDigest & blob_hash)
{
    int64_t count = 0;
    for (const RunRef & run : runs)
    {
        /// Stream the resolved run at O(block). The `seek` below is the ranged-get path: it lands the
        /// cursor on the target blob's block via the sparse footer index rather than scanning a resident
        /// whole-run buffer. `openSourceEdgeRun` enforces the run kind + key schema; only `kEdgeActive`
        /// rows count (the blob's `kCondemned` / `kZeroMarker` sentinel rows are not in-degree).
        RunFileReader r = openSourceEdgeRun(backend, run.key);
        /// Readers derive width from the run's OWN key_schema — never from pool meta (Task 4).
        const SourceEdgeKeyCodec codec = SourceEdgeKeyCodec::forSchema(r.keySchema());
        r.seek(codec.seekPrefix(blob_hash));   // sparse-index skip to this blob's edges
        String k, p;
        while (r.next(k, p))
        {
            BlobDigest bh;
            UInt128 sid;
            codec.parse(k, bh, sid);   // throws CORRUPTED_DATA on a malformed key (fail-closed)
            if (bh != blob_hash)
                break;   // past this blob
            if (!p.empty() && p[0] == kEdgeActive) ++count;
        }
    }
    return count;
}

}
