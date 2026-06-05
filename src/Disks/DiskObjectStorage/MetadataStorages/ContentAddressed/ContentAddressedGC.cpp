#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/ContentAddressedGC.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/ObjectIO.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/RefPayload.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/GcCompaction.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/GcLogWriter.h>
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
#include <Common/ProfileEvents.h>
#include <Common/logger_useful.h>

#include <set>
#include <vector>

namespace ProfileEvents
{
    extern const Event ContentAddressedTombstonesTotal;
    extern const Event ContentAddressedGenerationsObserved;
    extern const Event ContentAddressedHashesObserved;
    extern const Event ContentAddressedOrphanBytesEstimate;
}

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

/// Read + deserialize the session at `key`, returning nullopt if it VANISHED between the caller's LIST and
/// this READ (its owner committed/aborted — on commit the blobs are reachable via the published ref; on
/// abort they were never referenced, so a missing session is simply skipped). Only the missing-object
/// errors are swallowed; a malformed/truncated session still fails closed via deserialize (never
/// best-effort on corruption). Shared by every sessions/ scan so they cannot diverge on this race.
std::optional<WriteSession> tryReadSession(const ObjectStoragePtr & object_storage, const std::string & key)
{
    std::string raw;
    try
    {
        raw = readSmallObject(object_storage, key);
    }
    catch (const Exception & e)
    {
        if (e.code() == ErrorCodes::CANNOT_OPEN_FILE || e.code() == ErrorCodes::FILE_DOESNT_EXIST)
            return std::nullopt;
        throw;
    }
    return WriteSession::deserialize(raw);
}

}

/// ==== Pool enumeration (was PoolScan) ====

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
    /// Live part ids are collected from TWO families of GC roots, both under a `/refs/` segment whose ref
    /// objects each carry a live part id as their payload:
    ///
    /// 1. refsRootPrefix (store/.../refs/): active refs. The root also holds verbatim table-level files
    ///    (store/server/uuid/files/), so we keep only keys with a /refs/ segment. Per-ref sidecars (.meta)
    ///    also live under /refs/ but are ref-scoped, not GC roots (their payload is a RefSidecar, not a
    ///    part id) — skip them.
    ///
    /// 2. shadowRefsRootPrefix (shadow/<backup>/.../refs/): FREEZE/BACKUP snapshot refs, same /refs/ + .meta
    ///    conventions. Scanning this root keeps a frozen snapshot's blobs reachable until its shadow ref is
    ///    explicitly removed; without it they would be reclaimed once the live part is merged or dropped,
    ///    defeating FREEZE.
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
        const std::optional<WriteSession> session = tryReadSession(object_storage, key);
        if (!session)
            continue; /// vanished between LIST and READ — see tryReadSession.
        /// An EXPIRED session is itself reclaimable (a crashed writer must not pin blobs forever): the lease
        /// is a liveness HINT only, never the basis of a positive "still pinned" decision. Compare in a
        /// common signed domain (the lease is unsigned). CA GC S4 (#2): a sticky session (its `+` flush
        /// failed and is not yet durably re-logged) is EXEMPT from lease-expiry reaping — its reference is
        /// covered by neither the log nor a fresh session, so dropping the pin would be the forbidden
        /// under-count. A crashed-writer leak is acceptable (reclaimed once the reaper re-logs + folds, or by
        /// reconciliation); a dropped pin is not.
        if (!session->deltas_failed && static_cast<int64_t>(session->lease_deadline_unix) < now)
            continue;
        /// Project each BARE content hash to the FULL blob object key with the SAME `blobKey` fan-out
        /// `markReachableBlobs` uses, so the pinned set is directly comparable to the reachable set.
        for (const auto & hash : session->pending)
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
        const std::optional<WriteSession> session = tryReadSession(object_storage, key);
        if (!session)
            continue; /// vanished between LIST and READ — see tryReadSession.
        /// CA GC S4 (#2): a sticky session is EXEMPT from lease-expiry reaping (same under-count rationale as
        /// sessionPinnedBlobs). Otherwise an expired session is itself reclaimable, not a root.
        if (!session->deltas_failed && static_cast<int64_t>(session->lease_deadline_unix) < now)
            continue;
        /// Pin the manifest the session names ONLY if it resolves to a real parts/ object. The write path's
        /// session carries a part NAME (no manifest at that key), so its key does not exist and contributes
        /// nothing; the relink path carries a real committed part_id, so its manifest is kept reachable
        /// across the source-ref-drop window.
        const PartObjectKey manifest_key = partKey(key_prefix, session->part_id);
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

PartManifest ContentAddressedGC::resolveManifestAtAnyGeneration(const PartId & part_id) const
{
    /// Try the common-case g=0 manifest first (one HEAD/GET). On a 404 the manifest was resurrected to a
    /// higher generation (the GC sealed mg=0 and a contended commit re-created it at mg+1); LIST the per-id
    /// generation prefix and read the HIGHEST present generation. A live ref with NO present manifest
    /// generation is a genuine dangling ref -> fail-close (CORRUPTED_DATA), exactly as the g=0-only resolver
    /// did, so the safety property is unchanged — only the resurrected-manifest case is now resolved instead
    /// of mis-reported as missing.
    std::string manifest_key = partKey(key_prefix, part_id).string();
    if (!object_storage->tryGetObjectMetadata(manifest_key, /*with_tags=*/false))
    {
        std::optional<uint64_t> best;
        for (const auto & key : listKeysUnder(object_storage, partGenPrefix(key_prefix, part_id)))
        {
            bool is_tombstone = false;
            if (auto gen = parseGenFromKey(key, is_tombstone); gen && !is_tombstone)
                if (!best || *gen > *best)
                    best = *gen;
        }
        if (!best)
            throw Exception(
                ErrorCodes::CORRUPTED_DATA,
                "ContentAddressed GC: live ref points at missing manifest {} (no present generation)",
                partKey(key_prefix, part_id).string());
        manifest_key = partGenKey(key_prefix, part_id, *best).string();
    }
    StoredObject object(manifest_key);
    auto buf = object_storage->readObject(object, getReadSettings(), /*read_hint=*/std::nullopt);
    String bytes;
    readStringUntilEOF(bytes, *buf);
    return PartManifest::deserialize(bytes);
}

ContentAddressedGC::Reachability ContentAddressedGC::computeReachability(int64_t now) const
{
    /// CA GC S3: resolve a live part's manifest at ANY present generation (a resurrected manifest lives at
    /// mg>0; the steady g=0 key can be absent for a live part — see resolveManifestAtAnyGeneration). B18
    /// fail-close is preserved: a live ref with NO present manifest generation throws CORRUPTED_DATA.
    PartManifestResolver resolve = [this](const PartId & part_id) -> PartManifest
    { return resolveManifestAtAnyGeneration(part_id); };

    const std::set<PartId> live = listLivePartIds(object_storage, key_prefix);
    Reachability r;
    for (const auto & part_id : live)
        r.live_part_keys.insert(partKey(key_prefix, part_id));
    r.reachable_blobs = markReachableBlobs(key_prefix, live, resolve);
    for (const auto & pinned : sessionPinnedBlobs(object_storage, key_prefix, now))
        r.reachable_blobs.insert(pinned);
    for (const auto & pinned : sessionPinnedPartKeys(object_storage, key_prefix, now))
        r.live_part_keys.insert(pinned);
    return r;
}

SweepStats ContentAddressedGC::sweepCandidates(
    const std::set<std::string> & candidate_object_keys,
    const std::set<std::string> & pinned_snapshot,
    int64_t now,
    int64_t grace,
    const std::optional<GcLock> & held)
{
    /// The SHARED re-validate + grace + fence + delete tail (UNCHANGED semantics from the pre-S2 sweep —
    /// only the candidate SOURCE differs between the compaction-driven normal path and the reconciliation
    /// fallback).
    /// CA GC S4 (G1): runs LOCK-FREE — the gc_lock is no longer held across the sweep. Reads in-flight pins
    /// only from the supplied lock-free pinned_snapshot (never the shared set).
    const Reachability snapshot = computeReachability(now);

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

    /// CA GC S3 — `unreferenced` is the set of VALID generationed candidates that flow through
    /// seal/grace/recheck/branch. A non-generation key (a stray tombstone/active shape) is dropped here so
    /// it is never sealed or swept; everything else is kept, because even an already-sealed,
    /// now-identity-reachable candidate must reach the branch so RECOVER can un-seal it. The "do we create a
    /// NEW tombstone?" decision is made per-candidate in the seal loop below (it skips the RECOVER-would-fire
    /// case), so this set is deliberately NOT pre-filtered by reachability.
    std::set<std::string> unreferenced;
    for (const auto & key : candidate_object_keys)
    {
        if (parseGenObjectKey(key_prefix, key).has_value())
            unreferenced.insert(key);
    }

    /// Helper: identity-level reachability for a candidate against the supplied snapshot (the S3 safety
    /// net keys reachability at the g=0 identity key — the manifest pins bare `H` — plus B52 in-flight pins).
    auto identity_reachable_in = [&](const Reachability & r, const GenObjectKeyParts & parts) -> bool
    {
        if (parts.is_blob)
        {
            const BlobObjectKey identity_key = blobKey(key_prefix, BlobHash(parts.identity));
            /// CA GC S4 (G1): read the B52 in-flight pin from the lock-free SNAPSHOT taken at the start of
            /// the sweep, not from the shared set a concurrent commit is mutating.
            return r.reachable_blobs.contains(identity_key)
                || pinned_snapshot.contains(identity_key.string());
        }
        return r.live_part_keys.contains(partKey(key_prefix, PartId(parts.identity)));
    };

    /// Fence-ownership guard (the safety backstop): if this caller holds the GC-leader lock, re-confirm
    /// before every seal/delete that `gc.lock` STILL carries our fence token. A paused holder must NOT
    /// seal or delete after a successor took a higher fence. When no lock was supplied (single-owner /
    /// unit tests) it is skipped.
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

    /// CA GC S3 step (a) — SEAL every still-unreferenced candidate that is not already sealed. The seal is
    /// a DURABLE, single-owner condemnation: `condCreateIfAbsent(<g>.tombstone)`. It is the persistent
    /// condemned-state — once written, the candidate is re-discovered as a sealed-tombstone candidate every
    /// later round (closing the S2 grace>0 gap), and the writer's tomb re-check routes any reuse of the
    /// identity to `g+1` (so no new attach can land on a sealed `g`, I4). Sealing is fence-gated and
    /// idempotent (a lost CAS just means it was already sealed). We seal here, BEFORE grace + the fresh
    /// re-check, so the condemnation outlives this process even if the round is interrupted before sweep.
    if (leadership_lost())
        return {}; /// a successor took a higher fence; seal/delete NOTHING.

    for (const auto & key : unreferenced)
    {
        const auto parts = parseGenObjectKey(key_prefix, key);
        if (!parts)
            continue; /// not a generationed object key (e.g. a reconciliation orphan in an unexpected shape) — skip.
        const std::string tombstone = parts->is_blob
            ? blobTombstoneKey(key_prefix, BlobHash(parts->identity), parts->generation).string()
            : partTombstoneKey(key_prefix, PartId(parts->identity), parts->generation).string();
        /// Idempotent seal: if the tombstone already exists the CAS is lost and we simply proceed; the
        /// candidate stays condemned. (We never re-create a tombstone we already swept under — a swept
        /// gravestone persists, so condCreateIfAbsent there is a harmless no-op too.)
        if (object_storage->tryGetObjectMetadata(tombstone, /*with_tags=*/false).has_value())
            continue; /// already sealed — leave it; the branch decides its fate (sweep / drain / recover).
        /// Do NOT create a NEW tombstone for the actively-attachable, still-referenced generation (identity
        /// reachable AND no successor — the RECOVER case): condemning the live attach target each round is
        /// pointless churn. An ALREADY-sealed such candidate still flows to the branch (un-sealed by RECOVER).
        if (identity_reachable_in(snapshot, *parts) && !successorGenerationExists(parts->is_blob, parts->identity, parts->generation))
            continue;
        if (leadership_lost())
            return {};
        /// §13 observability: count a DURABLE condemnation actually sealed by this leader (the CAS winner).
        /// A swept generation keeps its gravestone forever, so this is the count of distinct condemnations.
        if (condCreateIfAbsent(*object_storage, tombstone, /*bytes=*/std::string()))
        {
            ProfileEvents::increment(ProfileEvents::ContentAddressedTombstonesTotal);
            /// CA GC S4 (#4, G3, Scan A): BEST-EFFORT record this open tombstone in the compact per-shard
            /// gc/sealed/<shard> index, so the next round re-discovers the condemnation by LISTing only that
            /// tiny index instead of the whole blobs/+parts/ tree. The durable `<g>.tombstone` is the source
            /// of truth; the index is only an accelerator — a missed write merely falls back to the
            /// reconciliation full-scan cross-check, never loses safety. So a failure here is swallowed.
            try
            {
                const ShardId shard = parts->is_blob
                    ? shardForHash(BlobHash(parts->identity))
                    : shardForPartId(PartId(parts->identity));
                condCreateIfAbsent(
                    *object_storage,
                    gcSealedKey(key_prefix, shard, parts->identity, parts->generation, parts->is_blob),
                    /*bytes=*/std::string());
            }
            catch (...)
            {
                tryLogCurrentException(
                    getLogger("ContentAddressedGC"),
                    "CA GC S4 (#4): best-effort gc/sealed index write failed (the durable tombstone is the "
                    "truth; reconciliation re-discovers it)");
            }
        }
    }

    /// Step (b) — GRACE: liveness ageing measured from the seal (the first round we condemned the object),
    /// carried in `first_unreachable`. This is a delay knob ONLY — the fresh re-check below, NOT the timer,
    /// is the safety gate. Objects no longer unreferenced drop out of `first_unreachable` (timer cleared).
    SweepResult res = selectForSweep(unreferenced, first_unreachable, now, grace);
    first_unreachable = std::move(res.first_unreachable);

    if (res.to_delete.empty())
        return {};

    /// Step (c) — FRESH authoritative re-check (§6.2): a fresh reachability pass AFTER the seal, dropping
    /// any candidate now reachable. In S3 this is the existing refs -> manifests + live-sessions pass (the
    /// §6.2 sessions+compaction-only form is S4). It is the SAFETY GATE: a generation whose identity is
    /// reachable is never blindly swept.
    const Reachability revalidated = computeReachability(now);

    if (leadership_lost())
        return {}; /// a successor took a higher fence; delete NOTHING further.

    /// Step (d) — BRANCH per candidate into {recover | drain | sweep}. The branch is generation-aware but
    /// gates on the IDENTITY-level safety net (S3): a candidate `(id, g)` whose identity is unreachable is
    /// SWEPT; a candidate whose identity is still reachable is RECOVERED (un-sealed) when no successor
    /// generation exists, or DRAINED (kept, tombstone kept, not re-opened) when a successor `g+1…` exists.
    /// SWEEP keeps the `<g>.tombstone` gravestone forever; only RECOVER deletes a tombstone, and only the
    /// fenced leader does, and only when no successor exists.
    SweepStats stats;
    StoredObjects to_remove; /// generation objects to SWEEP this round
    StoredObjects tombstones_to_remove; /// tombstones to delete on RECOVER (un-seal)
    for (const auto & key : res.to_delete)
    {
        const auto parts = parseGenObjectKey(key_prefix, key);
        if (!parts)
            continue;
        const bool is_part = !parts->is_blob;

        /// Identity-level reachability against the FRESH re-check snapshot (markReachableBlobs +
        /// sessionPinnedBlobs project to the g=0 `blobKey`; sessionPinnedPartKeys + live manifests to the
        /// g=0 `partKey`; the manifest pins bare `H`, so the safety net is identity-level — see the S3
        /// re-check note).
        const bool identity_reachable = identity_reachable_in(revalidated, *parts);

        const std::string tombstone = parts->is_blob
            ? blobTombstoneKey(key_prefix, BlobHash(parts->identity), parts->generation).string()
            : partTombstoneKey(key_prefix, PartId(parts->identity), parts->generation).string();

        /// CA GC S4 (#4, G3, Scan A): the matching gc/sealed index entry written at the seal site. It is
        /// removed on BOTH the SWEEP and the RECOVER terminal of this generation's lifecycle — on SWEEP so an
        /// already-swept generation (whose gravestone is KEPT forever) is not re-presented from the index every
        /// round, and on RECOVER alongside the un-seal of the tombstone. The shard is the same canonical
        /// assignment the seal used. `removeObjectsIfExist` is a no-op if the entry was never written
        /// (best-effort seal-side write, or a directly-written tombstone with no index entry).
        const ShardId shard = parts->is_blob
            ? shardForHash(BlobHash(parts->identity))
            : shardForPartId(PartId(parts->identity));
        const std::string sealed_index_key = gcSealedKey(key_prefix, shard, parts->identity, parts->generation, parts->is_blob);

        if (!identity_reachable)
        {
            /// SWEEP — no ref/session for the identity: delete the generation object, best-effort reset
            /// `active` off `g` (§6.1), KEEP the gravestone `<g>.tombstone` forever. The delete is
            /// unconditional + ABA-proof: `g` is sealed, so no new attach can land on it (I4); a re-created
            /// identity routes to `g+1` (a different key), so re-creation can never resurrect THIS object.
            resetActiveOffGeneration(parts->is_blob, parts->identity, parts->generation);
            if (is_part)
                ++stats.deleted_parts;
            else
                ++stats.deleted_blobs;
            /// §13 observability: estimate the reclaimed orphan bytes (one HEAD; 0 if already gone — a
            /// swept-twice gravestone). A guardrail for orphan drift / leaked uploads.
            if (auto meta = object_storage->tryGetObjectMetadata(key, /*with_tags=*/false))
                ProfileEvents::increment(ProfileEvents::ContentAddressedOrphanBytesEstimate, meta->size_bytes);
            to_remove.emplace_back(key);
            /// Remove the index entry on SWEEP: the generation object is gone (the gravestone remains), so the
            /// candidate must no longer be re-listed from the index. This is what stops a swept generation from
            /// being re-presented forever.
            to_remove.emplace_back(sealed_index_key);
        }
        else if (successorGenerationExists(parts->is_blob, parts->identity, parts->generation))
        {
            /// DRAIN — the identity is still reachable AND a successor `g+1…` exists: KEEP the tombstone AND
            /// the generation object (do NOT delete — it may still be the generation a live reference
            /// resolves to), and do NOT re-open `g`. New attaches go to the successor. `g` is reclaimed in a
            /// later round once its identity is fully unreachable (the S3 identity-level net cannot prove an
            /// individual generation drained while a successor still lives — see the S4 precision note).
        }
        else
        {
            /// RECOVER — the identity is still reachable and NO successor exists: the live reference must
            /// resolve to `g` itself, so un-seal it. Delete the `<g>.tombstone` (only the fenced leader does
            /// this, only when no successor exists) and re-open `g` as the attachable generation. Only
            /// queue the un-seal if a tombstone actually exists (a candidate may reach here un-sealed when
            /// it was filtered out of the seal loop as the live attach target).
            if (object_storage->tryGetObjectMetadata(tombstone, /*with_tags=*/false).has_value())
            {
                tombstones_to_remove.emplace_back(tombstone);
                /// Un-seal also drops the gc/sealed index entry: the condemnation is lifted, so the candidate
                /// must no longer be re-presented from the index.
                tombstones_to_remove.emplace_back(sealed_index_key);
            }
        }
    }

    /// Final fence re-check immediately before issuing any removal (seal/delete are all fence-gated).
    if (leadership_lost())
        return {};

    if (!to_remove.empty())
        object_storage->removeObjectsIfExist(to_remove);
    if (!tombstones_to_remove.empty())
        object_storage->removeObjectsIfExist(tombstones_to_remove); /// RECOVER: un-seal
    return stats;
}

std::set<std::string> ContentAddressedGC::collectSealedTombstoneCandidates()
{
    /// CA GC S4 (#4, G3, Scan A): discover sealed-but-unswept tombstones from the compact per-shard
    /// gc/sealed/<shard> index instead of LISTing the entire blobs/+parts/ tree every round. Each index
    /// entry maps back to its generation object key (the re-presented candidate). The durable .tombstone
    /// object remains the source of truth; the index is the accelerator (a missed entry is bounded by the
    /// reconciliation cross-check + Scan B still gates the delete). Runs lock-free (CA GC S4 G1).
    ///
    /// The generation object key produced here is byte-identical to what the OLD full-tree Scan A inserted
    /// (it stripped `.tombstone` off the tombstone key; `blobTombstoneKey == blobGenKey + .tombstone`, so
    /// `blobGenKey(...,g).string()` IS that exact key) — the sweep's candidate handling sees the same shape.
    /// A gravestone of an ALREADY-swept generation no longer re-appears here (its index entry is removed on
    /// sweep), so the swept generation is not re-presented forever.
    std::set<std::string> candidates;
    for (ShardId shard = 0; shard < kGcShardCount; ++shard)
    {
        for (const auto & key : listKeysUnder(object_storage, gcSealedPrefix(key_prefix, shard)))
        {
            const auto entry = parseSealedIndexKey(key_prefix, key);
            if (!entry)
                continue;
            const std::string gen_object_key = entry->is_blob
                ? blobGenKey(key_prefix, BlobHash(entry->identity), entry->generation).string()
                : partGenKey(key_prefix, PartId(entry->identity), entry->generation).string();
            candidates.insert(gen_object_key);
        }
    }
    return candidates;
}

bool ContentAddressedGC::successorGenerationExists(bool is_blob, const std::string & identity, uint64_t generation) const
{
    /// True iff a present GENERATION OBJECT (not a tombstone, not `active`) with a generation strictly
    /// greater than `generation` exists under the identity's directory. A resurrection always lands at
    /// max+1, so the presence of any higher generation object is the "a successor exists" signal that
    /// distinguishes DRAIN (keep `g`) from RECOVER (un-seal `g`). LISTs the per-identity prefix only.
    const std::string prefix = is_blob
        ? blobGenPrefix(key_prefix, BlobHash(identity))
        : partGenPrefix(key_prefix, PartId(identity));
    for (const auto & key : listKeysUnder(object_storage, prefix))
    {
        bool is_tombstone = false;
        const std::optional<uint64_t> g = parseGenFromKey(key, is_tombstone);
        if (!g || is_tombstone)
            continue; /// skip tombstones and the `active` hint — only present generation OBJECTS count.
        if (*g > generation)
            return true;
    }
    return false;
}

void ContentAddressedGC::resetActiveOffGeneration(bool is_blob, const std::string & identity, uint64_t swept_generation)
{
    /// Best-effort §6.1 reset: if `active` currently points at the generation we are sweeping, repair it to
    /// the highest surviving generation OBJECT (so a reader's default/active hint no longer names a deleted
    /// object). A plain PUT, NOT a CAS (G4/I7d) — `active` is a hint; a stale value is corrected by the
    /// reader's 404 -> LIST fallback, so this reset never gates safety and a failure is non-fatal. If no
    /// surviving generation remains, leave `active` as-is (it will 404 -> LIST -> repair, or the identity is
    /// gone entirely). NOTE: the active key is read once to avoid a needless PUT when it does not point here.
    const std::string active_key = is_blob
        ? blobActiveKey(key_prefix, BlobHash(identity))
        : partActiveKey(key_prefix, PartId(identity));

    /// Read the current active hint (absent -> default 0).
    uint64_t current_active = 0;
    if (object_storage->tryGetObjectMetadata(active_key, /*with_tags=*/false).has_value())
    {
        try
        {
            const std::string content = readSmallObject(object_storage, active_key);
            if (!content.empty())
                current_active = std::stoull(content);
        }
        catch (...)
        {
            current_active = 0; /// a malformed hint is treated as default-0; the reader fallback repairs it.
        }
    }
    if (current_active != swept_generation)
        return; /// `active` does not name the swept generation — nothing to repair.

    /// Find the highest SURVIVING generation object other than the one being swept.
    const std::string prefix = is_blob
        ? blobGenPrefix(key_prefix, BlobHash(identity))
        : partGenPrefix(key_prefix, PartId(identity));
    std::optional<uint64_t> best;
    for (const auto & key : listKeysUnder(object_storage, prefix))
    {
        bool is_tombstone = false;
        const std::optional<uint64_t> g = parseGenFromKey(key, is_tombstone);
        if (!g || is_tombstone || *g == swept_generation)
            continue;
        if (!best || *g > *best)
            best = *g;
    }
    if (!best)
        return; /// no surviving generation — leave `active` (reader fallback / identity gone handles it).

    try
    {
        const std::string bytes = std::to_string(*best);
        auto out = object_storage->writeObject(StoredObject(active_key), WriteMode::Rewrite);
        out->write(bytes.data(), bytes.size());
        out->finalize();
    }
    catch (...)
    {
        /// Best-effort: a failed `active` repair is non-fatal (the reader's 404 -> LIST fallback repairs it).
    }
}

SweepStats ContentAddressedGC::runSweepOnce(int64_t now, int64_t grace, std::optional<GcLock> held)
{
    /// CA GC S4 (G1) — the per-pool GC lock is NO LONGER held across the sweep. We take a CONSISTENT
    /// snapshot of the in-flight pin set ONCE under the narrow container guard, then run the entire sweep
    /// lock-free against the copy. The cross-process commit-vs-sweep safety the held-across-the-sweep lock
    /// used to provide is now the durable two-flag §7 handshake: the GC seals `.tombstone` BEFORE its fresh
    /// authoritative §6.2 re-check (`compute_reachability`, which re-reads the LIVE session set), and the
    /// writer raises its session pin BEFORE its own tomb re-check and the `+`. The §5.1 epoch-close
    /// (fenced PUT) before the fold makes the compaction's `LIST gc/log/E.shard/` see a stable, complete
    /// epoch WITHOUT the lock (the close is the barrier, not the mutex); a writer racing the close
    /// re-appends into the open epoch (rule 2) and its session covers the reference until folded (rule 3).
    const std::set<std::string> pinned_snapshot = snapshotInFlightPinnedBlobs();

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

    /// CA GC S4 (§5.1 rule 3, §7.3) — reap every COMMITTED write session whose `+` deltas are now folded
    /// into a durable snapshot. We run this AFTER the per-shard fold above, so the folded watermark each
    /// session is checked against is maximally advanced this round. A session lingers until folded; once
    /// folded, the snapshot covers the reference and the pin is dropped (the §6.2 gate's
    /// `sessions ∪ folded-snapshot` premise holds across the delete). This is the lockless-handshake's
    /// session lifetime extension — purely additive in S2 (the lock still serializes commit vs sweep).
    try
    {
        const size_t reaped = reapFoldedSessions(compaction);
        if (reaped != 0)
            LOG_TEST(getLogger("ContentAddressedGC"), "CA GC S4: reaped {} folded write session(s)", reaped);
    }
    catch (...)
    {
        /// A reaper failure only OVER-RETAINS a folded session (conservative — the snapshot already covers
        /// the reference, so the lingering pin is harmless and reclaimed next round). Never abort the sweep.
        tryLogCurrentException(getLogger("ContentAddressedGC"), "CA GC S4: folded-session reaper failed (sessions over-retained; harmless)");
    }

    /// CA GC S3 — re-present every sealed-but-unswept tombstone as a candidate. The compaction emits a
    /// count-0 candidate ONLY in the crossing fold (the epoch where the count first hits 0); on later rounds
    /// the key is gone from the folded counts, so without this the condemnation would be forgotten before
    /// grace expired (the S2 grace>0 gap). The DURABLE `<g>.tombstone` is the persistent condemned-state:
    /// re-discovering it here keeps the candidate flowing through seal(no-op)/grace/recheck/branch every
    /// round until it is swept or recovered.
    for (const auto & key : collectSealedTombstoneCandidates())
        candidate_object_keys.insert(key);

    /// §9 orphan-drift bound: occasionally fold in the heavy reconciliation scan's candidates too, so
    /// over-counts / abandoned uploads the compaction cannot see (orphan drift) are bounded. This only
    /// WIDENS the candidate set; the same grace + re-validate + delete tail (and its safety net) applies.
    if (reconciliationDue())
    {
        for (const auto & key : collectReconciliationCandidates(now, pinned_snapshot))
            candidate_object_keys.insert(key);
    }

    /// The unchanged grace + re-validate + fence + delete tail. The candidate SOURCE changed; everything
    /// downstream (grace ageing, the re-validate-under-lock safety net, the fence guard, the delete
    /// primitive) is identical to the pre-S2 sweep.
    return sweepCandidates(candidate_object_keys, pinned_snapshot, now, grace, held);
}

std::set<std::string> ContentAddressedGC::snapshotInFlightPinnedBlobs() const
{
    /// CA GC S4 (G1) — copy the shared in-flight pin set under the narrow container guard so the lock-free
    /// remainder of the sweep reads a stable set, not a `std::set` a concurrent commit is mutating. If no
    /// pin set was wired (legacy / unit tests with no concurrent inserts) the snapshot is empty.
    if (!in_flight_pinned_blobs)
        return {};
    std::lock_guard<std::mutex> guard(*gc_lock);
    return *in_flight_pinned_blobs;
}

std::set<std::string> ContentAddressedGC::collectReconciliationCandidates(
    int64_t now, const std::set<std::string> & pinned_snapshot)
{
    /// The full bucket scan's unreferenced complement (spec §9): every parts/ manifest not backing a live
    /// part, plus every blobs/ object not reachable from a live manifest or a live session. This is the
    /// PRE-S2 candidate source, kept ONLY as the reconciliation fallback.
    /// CA GC S4 (G1/#5): runs lock-free; reads in-flight pins only from the supplied pinned_snapshot.
    const Reachability reach = computeReachability(now);
    const std::set<PartObjectKey> & live_part_keys = reach.live_part_keys;
    const std::set<BlobObjectKey> & reachable_blobs = reach.reachable_blobs;

    /// CA GC S3 (concern 2) — the `parts/`/`blobs/` directories now hold a generation FAMILY per identity:
    /// `<g>` (generation objects), `<g>.tombstone` (gravestones), and `active` (the hint). Fold each
    /// directory generation-aware:
    ///   - NEVER flag a `.tombstone` (a gravestone is KEPT forever) or the `active` hint as an orphan — they
    ///     are not generation objects (parseGenObjectKey returns nullopt for both), so they are skipped.
    ///   - A generation OBJECT `<g>` is a candidate only if its IDENTITY is unreachable. The reachable set
    ///     is keyed at the g=0 identity key (`blobKey`/`partKey`), and the manifest pins bare `H`, so a
    ///     reachable identity keeps ALL its present generations off the orphan list (a referenced `<g>` is
    ///     never an orphan). An unreachable identity's generation objects are fed to the shared
    ///     seal/grace/recheck/branch tail, which re-confirms unreachability before sweeping.
    /// §13 observability (CA GC S4 #4): tally the generations-per-hash proxy HERE, where we already walk the
    /// whole parts/+blobs/ listing — present generation OBJECTS (ContentAddressedGenerationsObserved) and the
    /// distinct identities carrying at least one (ContentAddressedHashesObserved). The compaction-driven path
    /// no longer LISTs the full tree (Scan A reads only the gc/sealed index), so the tally must live here to
    /// see the true distribution; reconciliation's own cadence means this never double-counts. Their ratio is
    /// the mean generations-per-hash — the hot-content-cycling guardrail (gravestones are safe but not free).
    std::set<std::string> identities_with_generation;
    size_t generation_objects = 0;

    std::set<std::string> candidate_object_keys;
    for (const auto & key : listKeysUnder(object_storage, partsPrefix(key_prefix)))
    {
        const auto parts = parseGenObjectKey(key_prefix, key);
        if (!parts || parts->is_blob)
            continue; /// a tombstone, the `active` hint, or a malformed shape — never an orphan candidate.
        ++generation_objects;
        identities_with_generation.insert(parts->identity);
        if (live_part_keys.contains(partKey(key_prefix, PartId(parts->identity))))
            continue; /// the identity is reachable — keep ALL its generations.
        candidate_object_keys.insert(key);
    }
    for (const auto & key : listKeysUnder(object_storage, blobsPrefix(key_prefix)))
    {
        const auto parts = parseGenObjectKey(key_prefix, key);
        if (!parts || !parts->is_blob)
            continue; /// a tombstone, the `active` hint, or a malformed shape — never an orphan candidate.
        ++generation_objects;
        identities_with_generation.insert(parts->identity);
        const BlobObjectKey identity_key = blobKey(key_prefix, BlobHash(parts->identity));
        if (reachable_blobs.contains(identity_key))
            continue; /// the identity is reachable — keep ALL its generations.
        /// CA GC S4 (#5): read the B52 in-flight pin from the lock-free SNAPSHOT taken at the start of the
        /// sweep, not from the shared std::set a concurrent commit is mutating (the data race this fixes).
        if (pinned_snapshot.contains(identity_key.string()))
            continue;
        candidate_object_keys.insert(key);
    }

    if (generation_objects != 0)
        ProfileEvents::increment(ProfileEvents::ContentAddressedGenerationsObserved, generation_objects);
    if (!identities_with_generation.empty())
        ProfileEvents::increment(ProfileEvents::ContentAddressedHashesObserved, identities_with_generation.size());

    return candidate_object_keys;
}

void ContentAddressedGC::rewriteSession(const std::string & session_key, const WriteSession & session)
{
    const std::string bytes = session.serialize();
    auto out = object_storage->writeObject(StoredObject(session_key), WriteMode::Rewrite);
    out->write(bytes.data(), bytes.size());
    out->finalize();
}

size_t ContentAddressedGC::reapFoldedSessions(GcCompaction & compaction)
{
    /// CA GC S4 (§5.1 rule 3, §7.3) — delete every COMMITTED write session whose `+` deltas are all folded
    /// into a durable snapshot. The session is the durable handshake flag (§7.1) that covers the
    /// `+`-before-fold gap; once folded, the folded snapshot itself covers the reference, so the pin is no
    /// longer needed (the §6.2 gate's `sessions ∪ folded-snapshot` premise still holds after the delete).
    ///
    /// Mechanical rule (§7.3): reap iff EVERY `(shard, epoch)` in `delta_epochs` is folded. A session that
    /// is not yet committed (the writer is still uploading, or aborted) is NOT reaped here — its OWNER drops
    /// it at commit/abort, and a crashed owner's session is reclaimed by the lease (in
    /// `sessionPinnedBlobs`/`sessionPinnedPartKeys`, an expired session is already treated as reclaimable).
    /// We never reap a committed-but-unfolded session on a timer — the folded watermark is the only gate.
    ///
    /// CA GC S4 (G1) — runs WITHOUT the `gc_lock`. Race-safe against a concurrent commit raising a new
    /// session: a session that vanished between the LIST and the READ is skipped (rule a, swallowed as a
    /// missing object); an uncommitted (still-uploading) session is skipped (rule a, `committed == false`);
    /// a committed session is deleted ONLY when every delta epoch is provably folded (rule b). A session
    /// raised AFTER this LIST is simply not enumerated this round, so it is never mistakenly reaped.
    size_t reaped = 0;
    for (const auto & key : listKeysUnder(object_storage, sessionsPrefix(key_prefix)))
    {
        std::optional<WriteSession> read = tryReadSession(object_storage, key);
        if (!read)
            continue; /// vanished between LIST and READ — see tryReadSession.
        WriteSession session = std::move(*read);

        /// CA GC S4 (#2): a sticky session's `+` flush failed at commit — the ref is published but no `+` is
        /// durable. Re-log the stored `+` delta(s) (idempotent by event_id), record their settled epochs, and
        /// clear the sticky flag. The session then converts to a normal committed-until-folded session and is
        /// reaped by the folded gate below on a later round. NEVER reap it while still sticky.
        if (session.deltas_failed)
        {
            try
            {
                const std::vector<GcDelta> adds = deserializeGcDeltasFromSession(session.pending_add_delta);
                if (!adds.empty())
                {
                    GcLogWriter relog_writer(object_storage, key_prefix);
                    std::vector<std::pair<ShardId, uint64_t>> settled;
                    for (const auto & add : adds)
                    {
                        const auto part_settled = relog_writer.appendAndFlushForCommit(add);
                        settled.insert(settled.end(), part_settled.begin(), part_settled.end());
                    }
                    relog_writer.flushAll();
                    session.delta_epochs = std::move(settled);
                }
                /// The re-log landed durably: drop sticky. Persist the converted session so a crash after this
                /// point sees a normal committed session, not a sticky one.
                session.deltas_failed = false;
                session.pending_add_delta.clear();
                rewriteSession(key, session);
            }
            catch (...)
            {
                /// The re-log failed again (still-throttled S3): keep the session sticky for the next round.
                tryLogCurrentException(getLogger("ContentAddressedGC"),
                    "CA GC S4 (#2): sticky-session + re-log failed; session kept for the next round");
            }
            continue; /// never reap a session on the same round it converts — re-check foldedness next round.
        }

        /// Rule (a): not committed -> NOT reaped here (the owner/lease handles it). The pin must outlive an
        /// in-flight upload.
        if (!session.committed)
            continue;

        /// Rule (b): reap only when EVERY delta epoch is folded. An empty `delta_epochs` (a committed but
        /// delta-less commit — a FREEZE/shadow/detached publish out of the count model) is trivially folded
        /// (no `+` to cover), so the session is reapable immediately.
        bool all_folded = true;
        for (const auto & [shard, epoch] : session.delta_epochs)
        {
            if (!compaction.isEpochFolded(shard, epoch))
            {
                all_folded = false;
                break;
            }
        }
        if (!all_folded)
            continue;

        /// Folded: the durable snapshot now covers the reference, so the pin is safe to drop. Idempotent
        /// best-effort delete (a concurrent owner-delete is a harmless no-op).
        object_storage->removeObjectIfExists(StoredObject(key));
        ++reaped;
    }
    return reaped;
}

SweepStats ContentAddressedGC::runReconciliationScan(int64_t now, int64_t grace, std::optional<GcLock> held)
{
    /// The EXPLICIT reconciliation fallback (spec §9): the rare full `parts/`+`blobs/` scan, retired from
    /// the normal path. Same gc_lock + grace + re-validate + fence + delete tail; only the candidate source
    /// is the full bucket scan, so it reclaims orphans the compaction cannot see (over-counts, abandoned
    /// uploads) and recovers from a lost log+snapshot. CA GC S4 (G1): no `gc_lock` across the scan; the
    /// in-flight pin set is snapshotted once (the §7 handshake + fence lease carry the safety, as in the
    /// normal path).
    const std::set<std::string> pinned_snapshot = snapshotInFlightPinnedBlobs();
    std::set<std::string> candidate_object_keys = collectReconciliationCandidates(now, pinned_snapshot);
    /// CA GC S3 — also re-present every sealed-but-unswept tombstone, so the reconciliation path drives the
    /// same seal/grace/recheck/branch tail (sweep / drain / recover) on already-condemned generations, not
    /// only on freshly-discovered orphans.
    for (const auto & key : collectSealedTombstoneCandidates())
        candidate_object_keys.insert(key);
    return sweepCandidates(candidate_object_keys, pinned_snapshot, now, grace, held);
}

}

}
