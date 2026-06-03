#pragma once
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Identifiers.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/PartManifest.h>
#include <Disks/DiskObjectStorage/ObjectStorages/IObjectStorage_fwd.h>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace DB::ContentAddressed
{

/// The whole garbage-collection concern for a content-addressed pool lives here: the pure
/// reachability/sweep algorithms, the pool enumeration primitives, the un-wired refcount seam, and
/// the sweep driver `ContentAddressedGC`. The threading/lifecycle driver is `ContentAddressedGCThread`.

/// ==== Pool enumeration (was PoolScan) ====

/// The ref-object payload is a versioned struct on the shared codec (B28): `MAGIC(4) + version(1) +
/// part_id` (the part id as a length-prefixed string). The magic is `CARF` ("Content-Addressed ReF").
/// Version 1 holds only the part id; later versions append additive fields (B1's
/// `ReplicatedMergeTreePartHeader`, the per-ref mutable fields) WITHOUT breaking older readers, which
/// stop after the part id. The format is reserved for that growth on purpose.
constexpr FormatMagic kRefPayloadMagic = makeMagic("CARF");
constexpr uint8_t kRefPayloadVersion = 1;

/// Serialize a ref payload naming `part_id` (the publishing write path's payload). The SINGLE writer
/// paired with the SINGLE parser `partIdFromRefPayload` below.
std::string serializeRefPayload(const PartId & part_id);

/// Resolve a ref object payload into the part id it names. Parses the versioned `serializeRefPayload`
/// format EXACTLY: a wrong magic is `CORRUPTED_DATA` and a version newer than this build understands
/// is `NOT_IMPLEMENTED` (both fail-close — a published ref must name a part, and an unknown version
/// must never be misinterpreted). This is the SINGLE ref-payload parser: both the GC live-set scan
/// (`listLivePartIds`) and the read path (`ContentAddressedMetadataStorage::readRefPartId`) resolve a
/// ref through it, so they cannot disagree on the part id by construction (B28).
PartId partIdFromRefPayload(const std::string & payload);

/// Enumerate the full set of LIVE part ids in a content-addressed pool: every published ref under
/// the pool's refs root (the store/server/uuid/refs/part layout) names a part id (its payload). These
/// are the GC roots — a part id is live iff at least one ref points at it. Any list or read error
/// PROPAGATES so the caller aborts the sweep: a partial scan must never drive deletion (fail-close).
std::set<PartId> listLivePartIds(const ObjectStoragePtr & object_storage, const std::string & key_prefix);

/// List the object keys of every object under a pool root prefix (e.g. partsPrefix / blobsPrefix).
/// Thin wrapper over object_storage->listObjects; errors propagate. The returned strings are the raw
/// object keys; the GC wraps them into the right typed key (BlobObjectKey / PartObjectKey) at the one
/// well-marked boundary in runSweepOnce so reachable-vs-listed can only be compared in one key space.
std::vector<std::string> listKeysUnder(const ObjectStoragePtr & object_storage, const std::string & prefix);

/// ==== Pure reachability/sweep algorithms (was Reachability) ====

using PartManifestResolver = std::function<PartManifest(const PartId & part_id)>;

/// Reachable blob object-key set from the live roots (refs -> part manifests -> blob object keys).
/// A manifest stores the BARE content hash in each `BlobEntry.key` (the production write path records
/// `BlobEntry{blob_hash, size, blob_hash}`), while the GC sweep enumerates FULL object keys under
/// `blobsPrefix(key_prefix)`. To make the two comparable the reachable set is built with the SAME
/// `blobKey(key_prefix, bare_hash)` fan-out the read path uses, so reachable == full blob object key.
/// The return type is `std::set<BlobObjectKey>`: the sweep can ONLY compare it against listed blobs
/// after wrapping those into `BlobObjectKey` too, so a bare-hash-vs-object-key mismatch cannot compile.
std::set<BlobObjectKey> markReachableBlobs(
    const std::string & key_prefix, const std::set<PartId> & live_part_ids, const PartManifestResolver & resolve);

struct SweepResult
{
    std::vector<std::string> to_delete;
    std::unordered_map<std::string, int64_t> first_unreachable; /// updated timers (cleared for reachable-again)
};

/// `grace` is measured from first loss of reachability (NOT object age). Reachable-again clears the timer.
/// Operates on raw object-key strings — the GC has already reduced both blob and part keys to the
/// same unreferenced-object-key space before calling this, so no typed distinction is needed here.
SweepResult selectForSweep(const std::set<std::string> & unreferenced,
                           const std::unordered_map<std::string, int64_t> & first_unreachable,
                           int64_t now, int64_t grace);

/// ==== Un-wired refcount seam (was BlobRefIndex) ====

/// NOTE: BlobRefIndex is an un-wired future seam (B9), not on the M1 GC path.
/// Seam (B9): the delta refcount over content-addressed blob hashes.
/// M1 ships InMemoryBlobRefIndex; a RocksDB-backed impl plugs in here unchanged.
class IBlobRefIndex
{
public:
    virtual ~IBlobRefIndex() = default;
    virtual void addPart(const PartId & part_id, const PartManifest & manifest) = 0;
    virtual void removePart(const PartId & part_id, const PartManifest & manifest) = 0;
    virtual int64_t refcount(const BlobHash & blob_hash) const = 0;
    virtual std::set<BlobHash> unreferenced() const = 0;
};

class InMemoryBlobRefIndex : public IBlobRefIndex
{
public:
    void addPart(const PartId & part_id, const PartManifest & manifest) override;
    void removePart(const PartId & part_id, const PartManifest & manifest) override;
    int64_t refcount(const BlobHash & blob_hash) const override;
    std::set<BlobHash> unreferenced() const override;

private:
    std::unordered_map<BlobHash, int64_t> counts;
    std::unordered_set<PartId> applied_parts; /// idempotency guard for add/remove
};

/// ==== Sweep driver ====

struct SweepStats
{
    size_t deleted_blobs = 0;
    size_t deleted_parts = 0;
};

/// Reachability garbage collector for one content-addressed pool (single process — see the M1 GC
/// safety invariants). It enumerates the live part ids (the published refs), computes the reachable
/// blob set from their manifests, and deletes ONLY manifests under parts/ and blobs under blobs/ that
/// have been continuously unreferenced for at least `grace` seconds. Refs, table-level files and
/// generic disk files are owned by the table and never touched here.
///
/// `first_unreachable` is the across-sweeps timer state (grace is measured from the first sweep that
/// found an object unreferenced, NOT from object age); reachable-again clears an object's timer.
/// It is held in memory for the process lifetime (M1): on restart it resets, which only makes grace
/// conservative (never premature deletion).
class ContentAddressedGC
{
public:
    /// `gc_lock_` is the per-pool in-process GC mutex shared with the transaction commit path (B49). The
    /// sweep holds it across the live-set computation + delete so a delete can never interleave with a
    /// commit's blob re-validate + ref publish, and a commit fails closed if a blob it references was
    /// reclaimed in the finalize -> commit window. When null (legacy direct construction) a private
    /// mutex is created so the sweep is still internally consistent; production and the B49 regression
    /// tests pass the metadata storage's shared lock so the sweep and commit truly exclude each other.
    ContentAddressedGC(ObjectStoragePtr object_storage_, std::string key_prefix_, std::shared_ptr<std::mutex> gc_lock_ = nullptr);

    /// Run one sweep. Deletes nothing if any step before the removal throws (fail-close): a missing
    /// manifest for a live ref (B18), or any list/read error, aborts the sweep with the pool intact.
    /// The whole sweep is run under the per-pool GC lock (B49); holding it for the entire sweep rather
    /// than only mark+delete is acceptable for the PoC — the contention is a B9/B32 optimization.
    SweepStats runSweepOnce(int64_t now, int64_t grace);

private:
    const ObjectStoragePtr object_storage;
    const std::string key_prefix;
    /// Per-pool in-process GC lock, shared with ContentAddressedTransaction::commit (B49). Never null
    /// after construction.
    const std::shared_ptr<std::mutex> gc_lock;
    std::unordered_map<std::string, int64_t> first_unreachable;
};

}
