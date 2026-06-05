#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/ContentAddressedGC.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/GcCompaction.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Identifiers.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/PoolCoordination.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/PoolPaths.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/PartManifest.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/WriteSession.h>
#include <Disks/DiskObjectStorage/ObjectStorages/IObjectStorage.h>
#include <Disks/DiskObjectStorage/ObjectStorages/StoredObject.h>

#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Codec.h>

#include <IO/ReadBufferFromString.h>
#include <IO/ReadHelpers.h>
#include <IO/ReadSettings.h>
#include <IO/WriteBufferFromString.h>
#include <IO/WriteHelpers.h>

#include <Common/Exception.h>
#include <Common/logger_useful.h>

#include <set>
#include <vector>

namespace DB
{

namespace ErrorCodes
{
    extern const int CORRUPTED_DATA;
    extern const int CANNOT_OPEN_FILE;
    extern const int FILE_DOESNT_EXIST;
}

namespace ContentAddressed
{

namespace
{

std::string readSmallObject(const ObjectStoragePtr & object_storage, const std::string & key)
{
    StoredObject object(key);
    auto buf = object_storage->readObject(object, getReadSettings(), /*read_hint=*/std::nullopt);
    String content;
    readStringUntilEOF(content, *buf);
    return content;
}

}

/// ==== Pool enumeration (was PoolScan) ====

std::string serializeRefPayload(const PartId & part_id)
{
    /// MAGIC(4) + version(1) + part_id (length-prefixed). Explicit little-endian via the shared codec.
    std::string out;
    WriteBufferFromString buf(out);
    FormatHeader{kRefPayloadMagic, kRefPayloadVersion}.write(buf);
    writeStringBinary(part_id.string(), buf);
    buf.finalize();
    return out;
}

PartId partIdFromRefPayload(const std::string & payload)
{
    ReadBufferFromString buf(payload);
    FormatHeader::readAndValidate(buf, kRefPayloadMagic, kRefPayloadVersion, "ref payload");
    std::string part_id;
    readStringBinary(part_id, buf);
    if (part_id.empty())
        throw Exception(ErrorCodes::CORRUPTED_DATA, "ContentAddressed: ref payload has no part id");
    /// v1 holds only the part id. A future version that appends fields (B1's part header, per-ref
    /// mutable state) bumps the version byte, which `readAndValidate` rejects fail-closed in this
    /// build; that is the intended forward-compat gate (never misinterpret a newer payload).
    return PartId(std::move(part_id));
}

std::vector<std::string> listKeysUnder(const ObjectStoragePtr & object_storage, const std::string & prefix)
{
    RelativePathsWithMetadata children;
    object_storage->listObjects(prefix, children, /*max_keys=*/0);
    std::vector<std::string> keys;
    keys.reserve(children.size());
    for (const auto & child : children)
        keys.push_back(child->relative_path);
    return keys;
}

std::set<PartId> listLivePartIds(const ObjectStoragePtr & object_storage, const std::string & key_prefix)
{
    /// Live part ids are collected from TWO families of GC roots:
    ///
    /// 1. refsRootPrefix (store/.../refs/): every server's/table's active refs. The root also holds
    ///    verbatim table-level files under the store/server/uuid/files/ layout, so we keep only keys
    ///    whose path has a /refs/ segment (the ref objects); their payload is the live part id.
    ///    Per-ref sidecars (.meta) also live under /refs/ but are NOT refs (their payload is a
    ///    RefSidecar, not a part id), so skip them: a sidecar is ref-scoped, removed with the ref,
    ///    and never a GC root.
    ///
    /// 2. shadowRefsRootPrefix (shadow/<backup>/.../refs/): FREEZE snapshot refs. When a part is
    ///    frozen via BACKUP / FREEZE, its files are copied into the shadow/ namespace and a shadow ref
    ///    is published at shadowRefKey(...). These refs use the same /refs/ + .meta conventions as
    ///    store/ refs. Without scanning this root, a frozen snapshot's blobs would be reclaimed once
    ///    the live part is merged or dropped — defeating FREEZE. Treating shadow/ as an additional GC
    ///    root keeps the frozen blobs reachable until the shadow ref is explicitly removed.
    static const std::string refs_segment = "/refs/";
    std::set<PartId> live;
    auto scan_root = [&](const std::string & root)
    {
        for (const auto & key : listKeysUnder(object_storage, root))
        {
            if (key.find(refs_segment) == std::string::npos)
                continue;
            if (isRefMetaKey(key))
                continue;
            live.insert(partIdFromRefPayload(readSmallObject(object_storage, key)));
        }
    };
    scan_root(refsRootPrefix(key_prefix));        /// live active parts (store/.../refs/)
    scan_root(shadowRefsRootPrefix(key_prefix));  /// FREEZE snapshots (shadow/<backup>/.../refs/) — keep
                                                  /// frozen blobs reachable even after the live part is gone
    return live;
}

/// ==== Pure reachability/sweep algorithms (was Reachability) ====

std::set<BlobObjectKey> markReachableBlobs(
    const std::string & key_prefix, const std::set<PartId> & live_part_ids, const PartManifestResolver & resolve)
{
    std::set<BlobObjectKey> reachable;
    for (const auto & id : live_part_ids)
    {
        PartManifest manifest = resolve(id);
        for (const auto & blob : manifest.blobs)
            /// `blob.second.key` is the BARE content hash (BlobHash); project it to the FULL blob
            /// object key (`blobKey` fan-out) so it matches the keys listed under `blobsPrefix`.
            reachable.insert(blobKey(key_prefix, blob.second.key));
    }
    return reachable;
}

std::set<BlobObjectKey> sessionPinnedBlobs(
    const ObjectStoragePtr & object_storage, const std::string & key_prefix, int64_t now)
{
    std::set<BlobObjectKey> pinned;
    for (const auto & key : listKeysUnder(object_storage, sessionsPrefix(key_prefix)))
    {
        /// A session listed here can be removed by its owner (commit/abort -> releaseSession) between
        /// this LIST and the READ. A vanished session is simply gone: on commit its blobs are now
        /// reachable via the published ref; on abort they were never referenced — so skip it instead of
        /// throwing. Only the missing-object errors are swallowed; a malformed/truncated session still
        /// fails closed via deserialize (never best-effort on corruption).
        std::string raw;
        try
        {
            raw = readSmallObject(object_storage, key);
        }
        catch (const Exception & e)
        {
            if (e.code() == ErrorCodes::CANNOT_OPEN_FILE || e.code() == ErrorCodes::FILE_DOESNT_EXIST)
                continue;
            throw;
        }
        const WriteSession session = WriteSession::deserialize(raw);
        /// An EXPIRED session is itself reclaimable (a crashed writer must not pin blobs forever): the
        /// lease is a liveness HINT only, never the basis of a positive "still pinned" decision. `now`
        /// is the sweep clock; the lease is unsigned, so compare in a common signed domain (a negative
        /// `now` — never produced in practice — would simply treat every session as expired).
        if (static_cast<int64_t>(session.lease_deadline_unix) < now)
            continue;
        for (const auto & hash : session.pending)
            /// Project the BARE content hash to the FULL blob object key with the SAME `blobKey` fan-out
            /// `markReachableBlobs` uses, so the pinned set is directly comparable to the reachable set.
            pinned.insert(blobKey(key_prefix, hash));
    }
    return pinned;
}

std::set<PartObjectKey> sessionPinnedPartKeys(
    const ObjectStoragePtr & object_storage, const std::string & key_prefix, int64_t now)
{
    std::set<PartObjectKey> pinned;
    for (const auto & key : listKeysUnder(object_storage, sessionsPrefix(key_prefix)))
    {
        /// A vanished session (its owner committed/aborted between this LIST and the READ) is simply
        /// gone — skip it (same rationale as sessionPinnedBlobs). Only the missing-object errors are
        /// swallowed; a malformed session still fails closed via deserialize.
        std::string raw;
        try
        {
            raw = readSmallObject(object_storage, key);
        }
        catch (const Exception & e)
        {
            if (e.code() == ErrorCodes::CANNOT_OPEN_FILE || e.code() == ErrorCodes::FILE_DOESNT_EXIST)
                continue;
            throw;
        }
        const WriteSession session = WriteSession::deserialize(raw);
        if (static_cast<int64_t>(session.lease_deadline_unix) < now)
            continue; /// expired -> itself reclaimable, not a root.
        /// Pin the manifest the session names ONLY if it resolves to a real parts/ object. The write
        /// path's session carries a part NAME (no manifest at that key), so its key simply does not exist
        /// and contributes nothing; the relink path carries a real committed part_id, so its manifest is
        /// kept reachable across the source-ref-drop window.
        const PartObjectKey manifest_key = partKey(key_prefix, session.part_id);
        if (object_storage->tryGetObjectMetadata(manifest_key.string(), /*with_tags=*/false).has_value())
            pinned.insert(manifest_key);
    }
    return pinned;
}

SweepResult selectForSweep(const std::set<std::string> & unreferenced,
                           const std::unordered_map<std::string, int64_t> & first_unreachable,
                           int64_t now, int64_t grace)
{
    SweepResult res;
    for (const auto & key : unreferenced)
    {
        auto it = first_unreachable.find(key);
        int64_t since = (it == first_unreachable.end()) ? now : it->second;
        if (now - since >= grace)
            res.to_delete.push_back(key);
        else
            res.first_unreachable[key] = since; /// keep ageing
    }
    return res; /// objects no longer unreferenced are dropped from first_unreachable (timer cleared)
}

/// ==== Un-wired refcount seam (was BlobRefIndex) ====
///
/// NOTE: BlobRefIndex is an un-wired future seam (B9), not on the M1 GC path.

void InMemoryBlobRefIndex::addPart(const PartId & part_id, const PartManifest & manifest)
{
    std::lock_guard<std::mutex> lock(mtx);
    if (!applied_parts.insert(part_id).second)
        return; /// idempotent: this part's refs are already counted

    for (const auto & blob : manifest.blobs)
        counts[blob.second.key] += 1;
}

void InMemoryBlobRefIndex::removePart(const PartId & part_id, const PartManifest & manifest)
{
    std::lock_guard<std::mutex> lock(mtx);
    if (applied_parts.erase(part_id) == 0)
        return; /// this part was not applied

    for (const auto & blob : manifest.blobs)
    {
        auto it = counts.find(blob.second.key);
        if (it != counts.end() && --it->second <= 0)
            it->second = 0;
    }
}

int64_t InMemoryBlobRefIndex::refcount(const BlobHash & blob_hash) const
{
    std::lock_guard<std::mutex> lock(mtx);
    auto it = counts.find(blob_hash);
    return it == counts.end() ? 0 : it->second;
}

std::set<BlobHash> InMemoryBlobRefIndex::unreferenced() const
{
    std::lock_guard<std::mutex> lock(mtx);
    std::set<BlobHash> result;
    for (const auto & item : counts)
        if (item.second <= 0)
            result.insert(item.first);
    return result;
}

std::set<BlobHash> InMemoryBlobRefIndex::referenced() const
{
    std::lock_guard<std::mutex> lock(mtx);
    std::set<BlobHash> result;
    for (const auto & item : counts)
        if (item.second > 0)
            result.insert(item.first);
    return result;
}

/// ==== Sweep driver ====

ContentAddressedGC::ContentAddressedGC(
    ObjectStoragePtr object_storage_,
    std::string key_prefix_,
    std::shared_ptr<std::mutex> gc_lock_,
    std::shared_ptr<const std::set<std::string>> in_flight_pinned_blobs_,
    std::shared_ptr<InMemoryBlobRefIndex> blob_ref_index_)
    : object_storage(std::move(object_storage_))
    , key_prefix(std::move(key_prefix_))
    , gc_lock(gc_lock_ ? std::move(gc_lock_) : std::make_shared<std::mutex>())
    , in_flight_pinned_blobs(std::move(in_flight_pinned_blobs_))
    , blob_ref_index(std::move(blob_ref_index_))
{
}

bool ContentAddressedGC::reconciliationDue()
{
    /// §9 orphan-drift bound: run the heavy reconciliation scan every Nth round. 0 disables the scheduled
    /// scan (then reconciliation is only manual / the total-loss fallback). The counter advances each
    /// normal round; when it reaches the cadence it resets and reports due.
    if (reconciliation_cadence_rounds <= 0)
        return false;
    if (++rounds_since_reconciliation >= reconciliation_cadence_rounds)
    {
        rounds_since_reconciliation = 0;
        return true;
    }
    return false;
}

SweepStats ContentAddressedGC::sweepCandidatesLocked(
    const std::set<std::string> & candidate_object_keys, int64_t now, int64_t grace, const std::optional<GcLock> & held)
{
    /// The SHARED re-validate + grace + fence + delete tail (UNCHANGED semantics from the pre-S2 sweep —
    /// only the candidate SOURCE differs between the compaction-driven normal path and the reconciliation
    /// fallback). MUST be called with gc_lock held by the caller.
    ///
    /// The reachable-blob computation, factored so it can be re-run immediately before deletion (the
    /// re-validate-under-lock step) AND used by the S1 drift validator. B18 fail-close: a live ref whose
    /// manifest is missing throws CORRUPTED_DATA so the sweep aborts WITHOUT deleting anything.
    PartManifestResolver resolve = [this](const PartId & part_id) -> PartManifest
    {
        const PartObjectKey manifest_key = partKey(key_prefix, part_id);
        if (!object_storage->tryGetObjectMetadata(manifest_key.string(), /*with_tags=*/false))
            throw Exception(ErrorCodes::CORRUPTED_DATA, "ContentAddressed GC: live ref points at missing manifest {}", manifest_key.string());
        StoredObject object(manifest_key.string());
        auto buf = object_storage->readObject(object, getReadSettings(), /*read_hint=*/std::nullopt);
        String bytes;
        readStringUntilEOF(bytes, *buf);
        return PartManifest::deserialize(bytes);
    };

    /// One reachability snapshot: the live part ids, the live part manifest keys, and the reachable blob
    /// set = (refs -> manifests -> blob keys) UNION every LIVE write-session's pending blobs (M8). NOTE
    /// (G3): this computes reachability from refs -> manifests + sessions; it does NOT `LIST blobs/`. It is
    /// the SAFETY NET that drops any candidate still reachable (so a live blob wrongly emitted as a
    /// count-0 candidate by the compaction is never deleted), NOT the candidate source.
    struct Reachability
    {
        std::set<PartObjectKey> live_part_keys;
        std::set<BlobObjectKey> reachable_blobs;
    };
    auto compute_reachability = [&]() -> Reachability
    {
        std::set<PartId> live = listLivePartIds(object_storage, key_prefix);
        Reachability r;
        for (const auto & part_id : live)
            r.live_part_keys.insert(partKey(key_prefix, part_id));
        r.reachable_blobs = markReachableBlobs(key_prefix, live, resolve);
        for (const auto & pinned : sessionPinnedBlobs(object_storage, key_prefix, now))
            r.reachable_blobs.insert(pinned);
        for (const auto & pinned : sessionPinnedPartKeys(object_storage, key_prefix, now))
            r.live_part_keys.insert(pinned);
        return r;
    };

    const Reachability snapshot = compute_reachability();

    /// CA GC S1 (B9): VALIDATE the incremental reverse index against this authoritative reachable-blob
    /// snapshot and LOG any disagreement. INSTRUMENTATION ONLY — the deletion decision below never gates
    /// on it. Kept running against the compaction-driven path for one more stage (the cheap cross-check
    /// that the in-memory pre-filter agrees with the folded reverse index / scan). See CA GC S1.
    if (blob_ref_index)
    {
        std::set<BlobObjectKey> index_reachable;
        for (const auto & bare_hash : blob_ref_index->referenced())
            index_reachable.insert(blobKey(key_prefix, bare_hash));

        size_t missing_in_index = 0; /// reachable by the scan but not pinned by the index (under-count)
        for (const auto & key : snapshot.reachable_blobs)
            if (!index_reachable.contains(key))
                ++missing_in_index;

        size_t extra_in_index = 0; /// pinned by the index but not reachable by the scan (over-count)
        for (const auto & key : index_reachable)
            if (!snapshot.reachable_blobs.contains(key))
                ++extra_in_index;

        if (missing_in_index != 0 || extra_in_index != 0)
            LOG_INFO(
                getLogger("ContentAddressedGC"),
                "cas_gc_index_drift{{missing_in_index={}, extra_in_index={}}} (informational: the per-process "
                "reverse index disagrees with the authoritative reachability; the scan/compaction drives "
                "deletion, the index is observational — see CA GC S1)",
                missing_in_index,
                extra_in_index);
    }

    /// Filter the candidates to the genuinely-unreferenced set against this snapshot, then apply grace.
    /// A candidate still reachable from a live manifest or a live session is DROPPED here (the safety net):
    /// the compaction may emit a count-0 key, but if reachability still reaches it the part is live and we
    /// keep it. A B52 in-flight pin keeps a blob too.
    std::set<std::string> unreferenced;
    const std::string parts_root_for_filter = partsPrefix(key_prefix);
    for (const auto & key : candidate_object_keys)
    {
        const bool is_part = key.rfind(parts_root_for_filter, 0) == 0;
        if (is_part)
        {
            if (snapshot.live_part_keys.contains(PartObjectKey(key)))
                continue;
        }
        else
        {
            if (snapshot.reachable_blobs.contains(BlobObjectKey(key)))
                continue;
            if (in_flight_pinned_blobs && in_flight_pinned_blobs->contains(key))
                continue;
        }
        unreferenced.insert(key);
    }

    /// Apply the grace-from-unreachability policy and carry the updated timer state forward.
    SweepResult res = selectForSweep(unreferenced, first_unreachable, now, grace);
    first_unreachable = std::move(res.first_unreachable);

    if (res.to_delete.empty())
        return {};

    /// Re-validate-under-lock: a fresh full reachability pass immediately before deletion, dropping any
    /// candidate now reachable (closes the read-refs-then-list race). B18 fail-close still holds.
    const Reachability revalidated = compute_reachability();

    /// Fence-ownership guard (the safety backstop): if this caller holds the GC-leader lock, re-confirm
    /// before deleting that `gc.lock` STILL carries our fence token. A paused holder must NOT delete after
    /// a successor took a higher fence. When no lock was supplied (single-owner / unit tests) it is skipped.
    auto leadership_lost = [&]() -> bool
    {
        if (!held)
            return false;
        const StoredObject lock_object(gcLockKey(key_prefix));
        if (!object_storage->exists(lock_object))
            return true; /// the lock is gone -> we no longer hold leadership.
        auto buf = object_storage->readObject(lock_object, getReadSettings(), /*read_hint=*/std::nullopt);
        String bytes;
        readStringUntilEOF(bytes, *buf);
        return GcLock::deserialize(bytes).fence_token != held->fence_token;
    };

    if (leadership_lost())
        return {}; /// a successor took a higher fence; delete NOTHING further.

    const std::string parts_root = partsPrefix(key_prefix);
    SweepStats stats;
    StoredObjects to_remove;
    to_remove.reserve(res.to_delete.size());
    for (const auto & key : res.to_delete)
    {
        const bool is_part = key.rfind(parts_root, 0) == 0;
        if (is_part)
        {
            if (revalidated.live_part_keys.contains(PartObjectKey(key)))
                continue; /// a ref to this part appeared after the snapshot -> keep it.
        }
        else
        {
            if (revalidated.reachable_blobs.contains(BlobObjectKey(key)))
                continue; /// reachable again (new ref or live session) -> keep it.
            if (in_flight_pinned_blobs && in_flight_pinned_blobs->contains(key))
                continue;
        }

        if (is_part)
            ++stats.deleted_parts;
        else
            ++stats.deleted_blobs;
        to_remove.emplace_back(key);
    }

    /// Final fence re-check immediately before issuing the removal.
    if (leadership_lost())
        return {};

    object_storage->removeObjectsIfExist(to_remove);
    return stats;
}

SweepStats ContentAddressedGC::runSweepOnce(int64_t now, int64_t grace, std::optional<GcLock> held)
{
    /// Hold the per-pool GC lock for the whole sweep (B49): the candidate computation and the delete must
    /// not interleave with a transaction commit's re-validate + ref publish. In S2 the lock does a SECOND
    /// job (§5.1): while it is held no commit is appending, so the compaction's `LIST gc/log/E.shard/` sees
    /// a STABLE, complete epoch (the close-before-fold + the held lock together make the fold authoritative).
    std::lock_guard<std::mutex> gc_guard(*gc_lock);

    /// CA GC S2 — the NORMAL path is COMPACTION-DRIVEN (G3 retired): the candidate source is the per-shard
    /// streaming compaction's count-0 stream, NOT `listLivePartIds` / `markReachableBlobs` / `LIST blobs/`.
    /// For each shard we close its epoch (fenced PUT, no CAS) before folding, fold `gc/snap` + `gc/log`,
    /// and collect every count-0 key's FULL object key as a deletion candidate. The fence guard is the same
    /// `leadership_lost` read-check the delete tail uses, passed into the compaction so a paused leader
    /// never advances an epoch.
    GcCompaction compaction(object_storage, key_prefix);

    auto fence_still_mine = [&]() -> bool
    {
        if (!held)
            return true; /// single-owner / unit tests: no fence to lose.
        const StoredObject lock_object(gcLockKey(key_prefix));
        if (!object_storage->exists(lock_object))
            return false;
        auto buf = object_storage->readObject(lock_object, getReadSettings(), /*read_hint=*/std::nullopt);
        String bytes;
        readStringUntilEOF(bytes, *buf);
        return GcLock::deserialize(bytes).fence_token == held->fence_token;
    };

    std::set<std::string> candidate_object_keys;
    for (ShardId shard = 0; shard < kGcShardCount; ++shard)
    {
        const GcCompaction::CompactionResult fold = compaction.compactShard(shard, fence_still_mine);
        for (const auto & candidate : fold.candidates)
            candidate_object_keys.insert(candidate.object_key);
    }

    /// §9 orphan-drift bound: occasionally fold in the heavy reconciliation scan's candidates too, so
    /// over-counts / abandoned uploads the compaction cannot see (orphan drift) are bounded. This only
    /// WIDENS the candidate set; the same grace + re-validate + delete tail (and its safety net) applies.
    if (reconciliationDue())
    {
        for (const auto & key : collectReconciliationCandidatesLocked(now))
            candidate_object_keys.insert(key);
    }

    /// The unchanged grace + re-validate + fence + delete tail. The candidate SOURCE changed; everything
    /// downstream (grace ageing, the re-validate-under-lock safety net, the fence guard, the delete
    /// primitive) is identical to the pre-S2 sweep.
    return sweepCandidatesLocked(candidate_object_keys, now, grace, held);
}

std::set<std::string> ContentAddressedGC::collectReconciliationCandidatesLocked(int64_t now)
{
    /// The full bucket scan's unreferenced complement (spec §9): every parts/ manifest not backing a live
    /// part, plus every blobs/ object not reachable from a live manifest or a live session. This is the
    /// PRE-S2 candidate source, kept ONLY as the reconciliation fallback. MUST be called with gc_lock held.
    PartManifestResolver resolve = [this](const PartId & part_id) -> PartManifest
    {
        const PartObjectKey manifest_key = partKey(key_prefix, part_id);
        if (!object_storage->tryGetObjectMetadata(manifest_key.string(), /*with_tags=*/false))
            throw Exception(ErrorCodes::CORRUPTED_DATA, "ContentAddressed GC: live ref points at missing manifest {}", manifest_key.string());
        StoredObject object(manifest_key.string());
        auto buf = object_storage->readObject(object, getReadSettings(), /*read_hint=*/std::nullopt);
        String bytes;
        readStringUntilEOF(bytes, *buf);
        return PartManifest::deserialize(bytes);
    };

    std::set<PartId> live = listLivePartIds(object_storage, key_prefix);
    std::set<PartObjectKey> live_part_keys;
    for (const auto & part_id : live)
        live_part_keys.insert(partKey(key_prefix, part_id));
    std::set<BlobObjectKey> reachable_blobs = markReachableBlobs(key_prefix, live, resolve);
    for (const auto & pinned : sessionPinnedBlobs(object_storage, key_prefix, now))
        reachable_blobs.insert(pinned);
    for (const auto & pinned : sessionPinnedPartKeys(object_storage, key_prefix, now))
        live_part_keys.insert(pinned);

    std::set<std::string> candidate_object_keys;
    for (const auto & key : listKeysUnder(object_storage, partsPrefix(key_prefix)))
    {
        if (!live_part_keys.contains(PartObjectKey(key)))
            candidate_object_keys.insert(key);
    }
    for (const auto & key : listKeysUnder(object_storage, blobsPrefix(key_prefix)))
    {
        if (reachable_blobs.contains(BlobObjectKey(key)))
            continue;
        if (in_flight_pinned_blobs && in_flight_pinned_blobs->contains(key))
            continue;
        candidate_object_keys.insert(key);
    }
    return candidate_object_keys;
}

SweepStats ContentAddressedGC::runReconciliationScan(int64_t now, int64_t grace, std::optional<GcLock> held)
{
    /// The EXPLICIT reconciliation fallback (spec §9): the rare full `parts/`+`blobs/` scan, retired from
    /// the normal path. Same gc_lock + grace + re-validate + fence + delete tail; only the candidate source
    /// is the full bucket scan, so it reclaims orphans the compaction cannot see (over-counts, abandoned
    /// uploads) and recovers from a lost log+snapshot.
    std::lock_guard<std::mutex> gc_guard(*gc_lock);
    const std::set<std::string> candidate_object_keys = collectReconciliationCandidatesLocked(now);
    return sweepCandidatesLocked(candidate_object_keys, now, grace, held);
}

}

}
