#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/GcCompaction.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/GcLogWriter.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/PoolPaths.h>
#include <Disks/DiskObjectStorage/ObjectStorages/IObjectStorage.h>
#include <Disks/DiskObjectStorage/ObjectStorages/StoredObject.h>

#include <IO/ReadBufferFromString.h>
#include <IO/ReadHelpers.h>
#include <IO/ReadSettings.h>
#include <IO/VarInt.h>
#include <IO/WriteBufferFromString.h>
#include <IO/WriteHelpers.h>

#include <Common/Exception.h>
#include <Common/logger_useful.h>

#include <algorithm>
#include <set>
#include <unordered_set>
#include <utility>

namespace DB
{
namespace ErrorCodes
{
    extern const int CANNOT_OPEN_FILE;
    extern const int FILE_DOESNT_EXIST;
    extern const int CORRUPTED_DATA;
}
}

namespace DB::ContentAddressed
{

namespace
{

/// The snapshot run codec. MAGIC `CAGN` ("Content-Addressed Gc sNapshot") + version, on the shared
/// LE/varint codec (cross-arch determinism, fail-closed on bad magic / unknown version — B19/B28). The
/// body is a varint count of entries, then per entry: the kind byte, the identity (length-prefixed hex
/// string), the resolved GENERATION (varint), and a varint count. The entries are written in sorted
/// (kind, identity, generation) order so the run is a sorted run by construction (the merge-join walks it
/// in lockstep with the sorted deltas).
///
/// CA GC S3 (version 2) appends the per-entry resolved generation, so a g>0 (resurrected) key's running
/// count survives across epochs INDEPENDENTLY of its g=0 sibling. A v3 pool is created fresh (PoolMeta v3,
/// no back-compat), so no v1 snapshot can exist in it — reading only v2 is correct and fail-closed.
constexpr FormatMagic kSnapMagic = makeMagic("CAGN");
constexpr uint8_t kSnapVersion = 2;

std::string serializeSnapshot(const std::map<GcCompaction::CountKey, int64_t> & counts)
{
    std::string out;
    WriteBufferFromString buf(out);
    FormatHeader{kSnapMagic, kSnapVersion}.write(buf);
    /// Only POSITIVE counts are persisted (a 0/negative count carries no live reference — see writeSnapshot).
    size_t live = 0;
    for (const auto & [key, count] : counts)
        if (count > 0)
            ++live;
    writeVarUInt(live, buf);
    for (const auto & [key, count] : counts)
    {
        if (count <= 0)
            continue;
        writeBinaryLittleEndian(static_cast<uint8_t>(key.kind), buf);
        writeStringBinary(key.identity, buf);
        writeVarUInt(key.generation, buf);
        writeVarUInt(static_cast<uint64_t>(count), buf);
    }
    buf.finalize();
    return out;
}

std::map<GcCompaction::CountKey, int64_t> deserializeSnapshot(const std::string & bytes)
{
    ReadBufferFromString buf(bytes);
    FormatHeader::readAndValidate(buf, kSnapMagic, kSnapVersion, "gc snapshot run");
    std::map<GcCompaction::CountKey, int64_t> counts;
    uint64_t n = 0;
    readVarUInt(n, buf);
    for (uint64_t i = 0; i < n; ++i)
    {
        uint8_t kind_raw = 0;
        readBinaryLittleEndian(kind_raw, buf);
        if (kind_raw != static_cast<uint8_t>(GcCompaction::KeyKind::Blob)
            && kind_raw != static_cast<uint8_t>(GcCompaction::KeyKind::Part))
            throw Exception(ErrorCodes::CORRUPTED_DATA, "ContentAddressed gc snapshot run: bad key kind {}", static_cast<uint32_t>(kind_raw));
        GcCompaction::CountKey key;
        key.kind = static_cast<GcCompaction::KeyKind>(kind_raw);
        readStringBinary(key.identity, buf);
        readVarUInt(key.generation, buf);
        uint64_t count = 0;
        readVarUInt(count, buf);
        counts.emplace(std::move(key), static_cast<int64_t>(count));
    }
    return counts;
}

/// Parse the `<padded-epoch>.<shard>` tail of a gc/snap object key into (epoch, shard). The key under
/// gcSnapPrefix is `<padded-epoch>.<shard>` (no trailing slash); a key that does not match the (epoch,
/// shard) shape is ignored (nullopt) rather than misparsed.
std::optional<std::pair<uint64_t, ShardId>> parseSnapTail(const std::string & key, const std::string & snap_prefix)
{
    if (key.rfind(snap_prefix, 0) != 0)
        return std::nullopt;
    const std::string tail = key.substr(snap_prefix.size());
    const auto dot = tail.find('.');
    if (dot == std::string::npos)
        return std::nullopt;
    try
    {
        const uint64_t epoch = std::stoull(tail.substr(0, dot));
        const ShardId shard = static_cast<ShardId>(std::stoul(tail.substr(dot + 1)));
        return std::make_pair(epoch, shard);
    }
    catch (...)
    {
        return std::nullopt;
    }
}

/// Parse the `<epoch>.<shard>` component of a gc/log object key (`<root>/<epoch>.<shard>/<event_id>`) into
/// (epoch, shard). nullopt for any key that does not match the shape.
std::optional<std::pair<uint64_t, ShardId>> parseLogTail(const std::string & key, const std::string & log_root)
{
    if (key.rfind(log_root, 0) != 0)
        return std::nullopt;
    const std::string tail = key.substr(log_root.size());
    const auto slash = tail.find('/');
    if (slash == std::string::npos)
        return std::nullopt;
    const std::string component = tail.substr(0, slash);
    const auto dot = component.find('.');
    if (dot == std::string::npos)
        return std::nullopt;
    try
    {
        const uint64_t epoch = std::stoull(component.substr(0, dot));
        const ShardId shard = static_cast<ShardId>(std::stoul(component.substr(dot + 1)));
        return std::make_pair(epoch, shard);
    }
    catch (...)
    {
        return std::nullopt;
    }
}

std::string readSmallObject(const ObjectStoragePtr & object_storage, const std::string & key)
{
    StoredObject object(key);
    auto buf = object_storage->readObject(object, getReadSettings(), /*read_hint=*/std::nullopt);
    String content;
    readStringUntilEOF(content, *buf);
    return content;
}

}

GcCompaction::GcCompaction(ObjectStoragePtr object_storage_, std::string key_prefix_)
    : object_storage(std::move(object_storage_))
    , key_prefix(std::move(key_prefix_))
{
}

uint64_t GcCompaction::readShardEpoch(ShardId shard) const
{
    /// Mirrors GcLogWriter::readShardEpoch: absent / empty / malformed degrades to 0 (the open epoch since
    /// the beginning of time), which only WIDENS the window the fold covers — never an under-count.
    const std::string key = gcCurrentEpochKey(key_prefix, shard);
    if (!object_storage->tryGetObjectMetadata(key, /*with_tags=*/false))
        return 0;
    const std::string content = readSmallObject(object_storage, key);
    if (content.empty())
        return 0;
    try
    {
        return std::stoull(content);
    }
    catch (...)
    {
        return 0;
    }
}

void GcCompaction::writeShardEpoch(ShardId shard, uint64_t epoch)
{
    /// §5.1 rule 1: a PLAIN fenced PUT (the per-shard epoch has a single writer, the shard's GC leader, so
    /// the fence lease serializes it — no CAS). The decimal value is read back by readShardEpoch and by
    /// every writer's GcLogWriter::readShardEpoch, so it is a bare decimal string (no codec header — it is
    /// not a content-addressed object, just a tiny counter).
    const std::string key = gcCurrentEpochKey(key_prefix, shard);
    const std::string bytes = std::to_string(epoch);
    auto out = object_storage->writeObject(StoredObject(key), WriteMode::Rewrite);
    out->write(bytes.data(), bytes.size());
    out->finalize();
}

std::map<GcCompaction::CountKey, int64_t> GcCompaction::readSnapshot(uint64_t epoch, ShardId shard) const
{
    const GcSnapObjectKey snap_key = gcSnapKey(key_prefix, epoch, shard);
    if (!object_storage->tryGetObjectMetadata(snap_key.string(), /*with_tags=*/false))
        return {}; /// no predecessor snapshot (e.g. epoch 0) -> empty count base.
    return deserializeSnapshot(readSmallObject(object_storage, snap_key.string()));
}

void GcCompaction::writeSnapshot(uint64_t epoch, ShardId shard, const std::map<CountKey, int64_t> & counts)
{
    const GcSnapObjectKey snap_key = gcSnapKey(key_prefix, epoch, shard);
    const std::string bytes = serializeSnapshot(counts);
    auto out = object_storage->writeObject(StoredObject(snap_key.string()), WriteMode::Rewrite);
    out->write(bytes.data(), bytes.size());
    out->finalize();
}

std::vector<GcDelta> GcCompaction::listAndDecodeDeltas(uint64_t epoch, ShardId shard) const
{
    std::vector<GcDelta> deltas;
    const std::string prefix = gcLogPrefix(key_prefix, epoch, shard);
    RelativePathsWithMetadata children;
    object_storage->listObjects(prefix, children, /*max_keys=*/0);
    for (const auto & child : children)
    {
        /// A log object can vanish between the LIST and the READ only via the reclaim of an already-folded
        /// epoch by THIS same single-leader; within one fold the set is stable. A truncated/forged object
        /// still fails closed via GcLogBatch::deserialize (never best-effort on corruption).
        std::string raw;
        try
        {
            raw = readSmallObject(object_storage, child->relative_path);
        }
        catch (const Exception & e)
        {
            if (e.code() == ErrorCodes::CANNOT_OPEN_FILE || e.code() == ErrorCodes::FILE_DOESNT_EXIST)
                continue;
            throw;
        }
        GcLogBatch batch = GcLogBatch::deserialize(raw);
        for (auto & d : batch.deltas)
            deltas.push_back(std::move(d));
    }
    return deltas;
}

void GcCompaction::reclaimFoldedEpoch(uint64_t epoch, ShardId shard)
{
    /// The epoch was already CLOSED (current_epoch advanced past it) before the fold, so no writer is
    /// appending into it; reclaiming its objects is safe. Idempotent: a missing object is a no-op
    /// (removeObjectsIfExist), so a crashed-then-re-run reclaim is harmless.
    StoredObjects to_remove;
    const std::string prefix = gcLogPrefix(key_prefix, epoch, shard);
    RelativePathsWithMetadata children;
    object_storage->listObjects(prefix, children, /*max_keys=*/0);
    to_remove.reserve(children.size() + 1);
    for (const auto & child : children)
        to_remove.emplace_back(child->relative_path);
    to_remove.emplace_back(gcSnapKey(key_prefix, epoch, shard).string());
    if (!to_remove.empty())
        object_storage->removeObjectsIfExist(to_remove);
}

GcCompaction::CompactionResult GcCompaction::compactShard(ShardId shard, const std::function<bool()> & fence_still_mine)
{
    CompactionResult result;
    const uint64_t E = readShardEpoch(shard);
    result.folded_epoch = E;
    result.new_epoch = E + 1;

    /// (a) CLOSE the epoch by a plain fenced PUT BEFORE any LIST/fold (§5.1 rule 1). Gate on the fence: a
    /// paused leader whose successor took a higher fence must not advance the epoch. If leadership was
    /// lost, return an empty result (no candidates) — the caller deletes nothing.
    if (fence_still_mine && !fence_still_mine())
        return {};
    writeShardEpoch(shard, E + 1);

    /// (b) Only AFTER the close: LIST + decode E's deltas and the predecessor snapshot run.
    std::vector<GcDelta> deltas = listAndDecodeDeltas(E, shard);
    std::map<CountKey, int64_t> base = readSnapshot(E, shard);

    /// External-sort the deltas by the unified count key. We expand each delta into per-(key, event_id, op)
    /// contributions; the per-key dedup-on-fold collapses a re-appended duplicate (same event_id, same op)
    /// to a single count. The std::map keying gives the sorted (kind, identity) order the merge-join needs;
    /// memory is bounded by the merge frontier (the distinct keys + their per-key event_id sets), not the
    /// number of log objects.
    ///
    /// Per key we accumulate the set of (op, event_id) already applied (the dedup guard) and the running
    /// delta to that key's count. A `+` for a part's home shard counts the (part_id) edge; pins count their
    /// (H) keys. The part edge is counted ONLY in the part's home shard (shardForPartId) — mirroring the
    /// writer's split, so a manifest reference is counted in exactly one shard.
    struct KeyFold
    {
        int64_t delta = 0;
        std::set<std::pair<uint8_t, std::string>> applied; /// (op, event_id) dedup guard
    };
    std::map<CountKey, KeyFold> folds;

    auto apply = [&](const CountKey & key, GcDelta::Op op, const std::string & event_id)
    {
        auto & fold = folds[key];
        if (!fold.applied.emplace(static_cast<uint8_t>(op), event_id).second)
            return; /// §5.1 dedup-on-fold: this exact (op, event_id) already folded into this key.
        fold.delta += (op == GcDelta::Op::Add) ? 1 : -1;
    };

    for (const auto & d : deltas)
    {
        /// CA GC S3: each pin is counted under its RESOLVED generation `g` (`pin_generations[i]`, parallel
        /// to `pins`). An S2 delta (or any delta whose `pin_generations` is empty) takes every pin as g=0
        /// — the common path. The generation is part of the count key, so `(H, g)` and `(H, g+1)` net
        /// independently (a resurrected blob never sweeps its byte-identical predecessor and vice versa).
        for (size_t i = 0; i < d.pins.size(); ++i)
        {
            const uint64_t g = (i < d.pin_generations.size()) ? d.pin_generations[i] : 0;
            apply(CountKey{KeyKind::Blob, d.pins[i].string(), g}, d.op, d.event_id);
        }
        /// The (part_id) edge — counted only in the part's home shard (the writer put it there). We are
        /// folding `shard`, so count it iff this is the part's home shard. (A foreign-shard delta fragment
        /// still serializes part_id, so this guard — not the absence of the field — is what scopes it.)
        /// The manifest's resolved generation `mg` (`manifest_generation`, default 0) keys the part edge.
        if (GcLogWriter::shardForPartId(d.part_id) == shard)
            apply(CountKey{KeyKind::Part, d.part_id.string(), d.manifest_generation}, d.op, d.event_id);
    }

    /// Streaming MERGE-JOIN: the snapshot base (sorted) and the folds (sorted) are both std::maps keyed by
    /// CountKey, so a key-ordered union walk sums per key, streams the new snapshot, and emits count-0
    /// candidates in one pass. (std::map already yields sorted order; the union is the merge frontier.)
    std::set<CountKey> all_keys;
    for (const auto & [key, count] : base)
        all_keys.insert(key);
    for (const auto & [key, fold] : folds)
        all_keys.insert(key);

    std::map<CountKey, int64_t> folded;
    for (const auto & key : all_keys)
    {
        int64_t count = 0;
        if (auto it = base.find(key); it != base.end())
            count = it->second;
        if (auto it = folds.find(key); it != folds.end())
            count += it->second.delta;

        /// A count <= 0 is a key whose every reference has been netted away: emit it as a candidate. Clamp
        /// to 0 in the folded view (counts never go negative durably — an over-decrement reflects a `-`
        /// whose matching `+` was already folded in an earlier epoch; reconciliation corrects any drift).
        if (count <= 0)
        {
            Candidate candidate;
            candidate.key = key;
            /// CA GC S3: the candidate addresses the GENERATIONED physical object whose references netted
            /// to zero — `blobGenKey(H, g)` / `partGenKey(part_id, mg)` — so the seal/sweep targets exactly
            /// that object, never a sibling generation. For g=0 this equals the old `blobKey` / `partKey`.
            candidate.object_key = (key.kind == KeyKind::Blob)
                ? blobGenKey(key_prefix, BlobHash(key.identity), key.generation).string()
                : partGenKey(key_prefix, PartId(key.identity), key.generation).string();
            result.candidates.push_back(std::move(candidate));
            folded[key] = 0; /// recorded for the drift cross-check; NOT persisted (writeSnapshot drops <=0)
        }
        else
        {
            folded[key] = count;
        }
    }

    /// Stream the new snapshot (only positive counts persist). Then advance: reclaim E's log + snapshot.
    writeSnapshot(E + 1, shard, folded);

    /// Re-check the fence before reclaiming the old epoch (the reclaim is a destructive op; a leader that
    /// lost leadership mid-fold must not delete). The new snapshot + closed epoch are already durable, so a
    /// skipped reclaim only leaks the old objects (reconciled later) — never an under-count.
    if (fence_still_mine && !fence_still_mine())
    {
        result.folded_counts = std::move(folded);
        return result;
    }
    reclaimFoldedEpoch(E, shard);

    result.folded_counts = std::move(folded);
    return result;
}

std::optional<uint64_t> GcCompaction::latestSnapshotEpoch(ShardId shard) const
{
    const std::string prefix = gcSnapPrefix(key_prefix);
    RelativePathsWithMetadata children;
    object_storage->listObjects(prefix, children, /*max_keys=*/0);
    std::optional<uint64_t> latest;
    for (const auto & child : children)
    {
        const auto parsed = parseSnapTail(child->relative_path, prefix);
        if (!parsed || parsed->second != shard)
            continue;
        if (!latest || parsed->first > *latest)
            latest = parsed->first;
    }
    return latest;
}

bool GcCompaction::isEpochFolded(ShardId shard, uint64_t epoch) const
{
    /// CA GC S4 (§5.1 rule 3): a `+` enqueued into `(shard, epoch)` is folded once a durable snapshot
    /// incorporates it. The fold of epoch `E` writes `gc/snap/<E+1>.<shard>`, so a snapshot at epoch `S`
    /// has folded every delta of epochs `< S`. Therefore `epoch` is folded iff the latest snapshot's epoch
    /// is STRICTLY GREATER than `epoch`. Absent snapshot => nothing folded yet => false (conservative: the
    /// session lingers, never reaped early). This is a pure read over `gc/snap/` (the §7.3 reaper rule (b)).
    const std::optional<uint64_t> latest = latestSnapshotEpoch(shard);
    return latest.has_value() && *latest > epoch;
}

std::vector<uint64_t> GcCompaction::logEpochs(ShardId shard) const
{
    const std::string root = gcLogRootPrefix(key_prefix);
    RelativePathsWithMetadata children;
    object_storage->listObjects(root, children, /*max_keys=*/0);
    std::set<uint64_t> epochs;
    for (const auto & child : children)
    {
        const auto parsed = parseLogTail(child->relative_path, root);
        if (!parsed || parsed->second != shard)
            continue;
        epochs.insert(parsed->first);
    }
    return {epochs.begin(), epochs.end()};
}

std::optional<GcCompaction::RebuildResult> GcCompaction::rebuildFromSnapshotAndLog(ShardId shard)
{
    /// §9 rebuild / catch-up: recompute counts from the LATEST snapshot run + every un-folded log epoch for
    /// the shard, with NO blob LIST. Used on leader startup. The result is the reverse index the next fold
    /// would have produced WITHOUT reclaiming or advancing anything (read-only).
    const std::optional<uint64_t> snap_epoch = latestSnapshotEpoch(shard);
    const std::vector<uint64_t> epochs = logEpochs(shard);

    /// Total loss: neither a snapshot nor any log object exists for the shard. The caller falls back to the
    /// heavy reconciliation scan (which rebinds reachability from live refs + manifests).
    if (!snap_epoch && epochs.empty())
        return std::nullopt;

    std::map<CountKey, int64_t> counts = snap_epoch ? readSnapshot(*snap_epoch, shard) : std::map<CountKey, int64_t>{};

    /// Fold every log epoch STRICTLY AFTER the snapshot's epoch (those whose deltas the snapshot does not
    /// yet incorporate). A snapshot at epoch S already folded the deltas of epochs <= S-1 (the snapshot for
    /// E+1 incorporates E), so the un-folded tail is epochs >= S. Folding epoch == S over the snapshot
    /// named S is correct: that snapshot was the PREDECESSOR base of S's fold, so S's deltas are not yet in
    /// it. Use a per-key (op, event_id) dedup guard exactly as compactShard does.
    struct KeyFold
    {
        int64_t delta = 0;
        std::set<std::pair<uint8_t, std::string>> applied;
    };
    std::map<CountKey, KeyFold> folds;
    auto apply = [&](const CountKey & key, GcDelta::Op op, const std::string & event_id)
    {
        auto & fold = folds[key];
        if (!fold.applied.emplace(static_cast<uint8_t>(op), event_id).second)
            return;
        fold.delta += (op == GcDelta::Op::Add) ? 1 : -1;
    };

    for (const uint64_t epoch : epochs)
    {
        if (snap_epoch && epoch < *snap_epoch)
            continue; /// already folded into the snapshot.
        for (const auto & d : listAndDecodeDeltas(epoch, shard))
        {
            /// Generation-aware fold, identical to compactShard: each pin under its resolved generation
            /// (default 0), the part edge under the resolved manifest generation (default 0).
            for (size_t i = 0; i < d.pins.size(); ++i)
            {
                const uint64_t g = (i < d.pin_generations.size()) ? d.pin_generations[i] : 0;
                apply(CountKey{KeyKind::Blob, d.pins[i].string(), g}, d.op, d.event_id);
            }
            if (GcLogWriter::shardForPartId(d.part_id) == shard)
                apply(CountKey{KeyKind::Part, d.part_id.string(), d.manifest_generation}, d.op, d.event_id);
        }
    }

    for (const auto & [key, fold] : folds)
        counts[key] += fold.delta;

    /// Emit every net-zero / negative key as a candidate (the catch-up leader hands these to the grace +
    /// re-validate + delete tail without re-folding), then drop them from the rebuilt count view — symmetric
    /// with the snapshot persistence rule, so the rebuilt counts are byte-equivalent to a fresh fold's
    /// folded_counts.
    RebuildResult result;
    for (auto it = counts.begin(); it != counts.end();)
    {
        if (it->second <= 0)
        {
            Candidate candidate;
            candidate.key = it->first;
            candidate.object_key = (it->first.kind == KeyKind::Blob)
                ? blobGenKey(key_prefix, BlobHash(it->first.identity), it->first.generation).string()
                : partGenKey(key_prefix, PartId(it->first.identity), it->first.generation).string();
            result.candidates.push_back(std::move(candidate));
            it = counts.erase(it);
        }
        else
        {
            ++it;
        }
    }
    result.counts = std::move(counts);
    return result;
}

}
