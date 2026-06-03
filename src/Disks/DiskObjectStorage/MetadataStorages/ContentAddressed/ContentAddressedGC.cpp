#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/ContentAddressedGC.h>
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

#include <set>
#include <vector>

namespace DB
{

namespace ErrorCodes
{
    extern const int CORRUPTED_DATA;
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
    /// Every server's/table's refs live under refsRootPrefix. The same root also holds verbatim
    /// table-level files under the store/server/uuid/files/ layout, so we keep only keys whose path has a
    /// /refs/ segment (the ref objects); their payload is the live part id. Per-ref sidecars (.meta)
    /// also live under /refs/ but are NOT refs (their payload is a RefSidecar, not a part id), so skip
    /// them: a sidecar is ref-scoped, removed with the ref, and never a GC root.
    static const std::string refs_segment = "/refs/";
    std::set<PartId> live;
    for (const auto & key : listKeysUnder(object_storage, refsRootPrefix(key_prefix)))
    {
        if (key.find(refs_segment) == std::string::npos)
            continue;
        if (isRefMetaKey(key))
            continue;
        live.insert(partIdFromRefPayload(readSmallObject(object_storage, key)));
    }
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
        const WriteSession session = WriteSession::deserialize(readSmallObject(object_storage, key));
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
    if (!applied_parts.insert(part_id).second)
        return; /// idempotent: this part's refs are already counted

    for (const auto & blob : manifest.blobs)
        counts[blob.second.key] += 1;
}

void InMemoryBlobRefIndex::removePart(const PartId & part_id, const PartManifest & manifest)
{
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
    auto it = counts.find(blob_hash);
    return it == counts.end() ? 0 : it->second;
}

std::set<BlobHash> InMemoryBlobRefIndex::unreferenced() const
{
    std::set<BlobHash> result;
    for (const auto & item : counts)
        if (item.second <= 0)
            result.insert(item.first);
    return result;
}

/// ==== Sweep driver ====

ContentAddressedGC::ContentAddressedGC(
    ObjectStoragePtr object_storage_,
    std::string key_prefix_,
    std::shared_ptr<std::mutex> gc_lock_,
    std::shared_ptr<const std::set<std::string>> in_flight_pinned_blobs_)
    : object_storage(std::move(object_storage_))
    , key_prefix(std::move(key_prefix_))
    , gc_lock(gc_lock_ ? std::move(gc_lock_) : std::make_shared<std::mutex>())
    , in_flight_pinned_blobs(std::move(in_flight_pinned_blobs_))
{
}

SweepStats ContentAddressedGC::runSweepOnce(int64_t now, int64_t grace, std::optional<GcLock> held)
{
    /// Hold the per-pool GC lock for the whole sweep (B49): the live-set computation, the
    /// reachable-blob mark and the delete must not interleave with a transaction commit's
    /// re-validate + ref publish, or a blob just deemed unreferenced could be deleted in the window
    /// between a dedup commit's blob HEAD and its ref publish (dangling ref -> data loss). The commit
    /// path takes the SAME lock and re-HEADs every referenced blob before publishing, failing closed
    /// if any is gone, so the two are mutually exclusive and a reused blob is either kept reachable
    /// (commit published its ref first) or the commit retries (sweep reclaimed it first).
    std::lock_guard<std::mutex> gc_guard(*gc_lock);

    /// The reachable-blob computation, factored so it can be re-run at the snapshot AND again
    /// immediately before deletion (the re-validate-under-lock step). B18 fail-close: a live ref whose
    /// manifest is missing throws CORRUPTED_DATA so the sweep aborts WITHOUT deleting anything (a
    /// partial reachable set must never drive deletion — it would drop a live part's blobs).
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

    /// One reachability snapshot: the live part ids (every part named by a published ref), the live
    /// part manifest keys, and the reachable blob set = (refs -> manifests -> blob keys) UNION every
    /// LIVE write-session's pending blobs (M8 session-pin roots). All blob keys are FULL object keys
    /// (BlobObjectKey) so they are directly comparable to the listed blobs/ keys — a bare-hash mismatch
    /// (the historical data-loss bug class) cannot compile.
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
        /// Session-pin roots: a blob uploaded but not yet referenced by a published ref is pinned by a
        /// live write session in the bucket. Treat it as reachable so a remote sweep cannot reclaim it
        /// out from under the writer. An expired session is not a root (it is itself reclaimable).
        for (const auto & pinned : sessionPinnedBlobs(object_storage, key_prefix, now))
            r.reachable_blobs.insert(pinned);
        return r;
    };

    const Reachability snapshot = compute_reachability();

    /// The full unreferenced object-key set: every manifest not backing a live part, plus every blob
    /// not reachable from a live manifest or a live session. Sweep scope is strictly parts/ and blobs/.
    /// This is the ONE well-marked boundary where the raw listed strings are wrapped into typed keys.
    std::set<std::string> unreferenced;
    for (const auto & key : listKeysUnder(object_storage, partsPrefix(key_prefix)))
    {
        if (!snapshot.live_part_keys.contains(PartObjectKey(key)))
            unreferenced.insert(key);
    }
    for (const auto & key : listKeysUnder(object_storage, blobsPrefix(key_prefix)))
    {
        if (snapshot.reachable_blobs.contains(BlobObjectKey(key)))
            continue;
        /// B52: a blob staged by an in-flight (uncommitted) transaction is not yet named by any
        /// published ref, so it would look unreferenced — but a dedup-skipping insert decided to reuse
        /// it (and did NOT re-upload it), and is about to publish a ref to it. Deleting it here would
        /// leave that ref dangling. We hold gc_lock for the whole sweep and the insert holds the SAME
        /// lock while it pins the key and makes its skip decision, so the pin set we read here is a
        /// consistent snapshot: treat every pinned key as reachable.
        if (in_flight_pinned_blobs && in_flight_pinned_blobs->contains(key))
            continue;
        unreferenced.insert(key);
    }

    /// Apply the grace-from-unreachability policy and carry the updated timer state forward.
    SweepResult res = selectForSweep(unreferenced, first_unreachable, now, grace);
    first_unreachable = std::move(res.first_unreachable);

    if (res.to_delete.empty())
        return {};

    /// Re-validate-under-lock: re-read the live roots (refs + live sessions) immediately before
    /// deletion. The snapshot above listed objects AFTER reading refs, so a ref published in that
    /// enumeration window would have been missed; recomputing reachability here and dropping any
    /// candidate now reachable closes that read-refs-then-list-objects race. Keep it correct over
    /// clever: a fresh full reachability pass, then filter the candidates. B18 fail-close still holds.
    const Reachability revalidated = compute_reachability();

    /// Fence-ownership guard (the safety backstop): if this caller holds the GC-leader lock, re-confirm
    /// before deleting that `gc.lock` STILL carries our fence token. A peer may have stolen leadership
    /// (a higher fence on disk) while we were paused; a paused holder must NOT delete after a successor
    /// took a higher fence. Mirror `renewGcLock`'s ownership read-check. When no lock was supplied
    /// (single-owner / unit tests) the guard is skipped.
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

    /// Delete only the candidates that are STILL unreferenced after re-validation. NOTHING above can
    /// have deleted anything: a throw in any list/read step aborts before this point with the pool
    /// intact. The fence guard is also re-checked within the loop so a leadership loss observed
    /// mid-delete stops further deletions (a paused holder must not keep deleting).
    const std::string parts_root = partsPrefix(key_prefix);
    SweepStats stats;
    StoredObjects to_remove;
    to_remove.reserve(res.to_delete.size());
    for (const auto & key : res.to_delete)
    {
        const bool is_part = key.rfind(parts_root, 0) == 0;
        /// Re-validate this specific candidate against the freshly recomputed reachability.
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

    /// Final fence re-check immediately before issuing the removal, in case leadership was lost while
    /// re-validating (which itself does object-store reads).
    if (leadership_lost())
        return {};

    object_storage->removeObjectsIfExist(to_remove);
    return stats;
}

}

}
