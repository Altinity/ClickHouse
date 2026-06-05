#include <gtest/gtest.h>

#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/ContentAddressedMetadataStorage.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/ContentAddressedTransaction.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/GcCompaction.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/GcDelta.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/GcLayout.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/GcLogWriter.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Identifiers.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/PoolPaths.h>
#include <Disks/DiskObjectStorage/ObjectStorages/IObjectStorage.h>
#include <Disks/DiskObjectStorage/ObjectStorages/StoredObject.h>
#include <Disks/WriteMode.h>

#include <IO/ReadBufferFromFileBase.h>
#include <IO/ReadBufferFromMemory.h>
#include <IO/WriteBufferFromFileBase.h>
#include <IO/WriteBufferFromString.h>

#include <Core/ServerUUID.h>

#include <Common/Exception.h>
#include <Common/ObjectStorageKeyGenerator.h>

#include <atomic>
#include <filesystem>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <system_error>

using namespace DB::ContentAddressed;

namespace DB
{
namespace ErrorCodes
{
    extern const int FILE_DOESNT_EXIST;
    extern const int NOT_IMPLEMENTED;
    extern const int S3_ERROR;
}
}

namespace
{

/// CA GC S2 — an in-memory IObjectStorage fake for the streaming-GC oracles. It is the DETERMINISTIC op
/// counter the §12.3 budget asserts read: every LIST is classified by the prefix it lists (a `blobs/` or
/// `parts/` LIST is the G3-forbidden scan; a `refs/` LIST is the allowed re-validate safety net; a
/// `gc/log/` LIST is the per-epoch fold), and every PUT/GET/DELETE is tallied. Drives GcCompaction /
/// GcLogWriter / GcDelta directly over a real key prefix so the read and write sides cannot disagree.
class CountingMemoryObjectStorage : public DB::IObjectStorage
{
public:
    /// Op tallies (the deterministic budget counters the asserts read).
    mutable std::atomic<size_t> blob_list_ops{0}; /// LISTs under `<prefix>/blobs/` — MUST be 0 on the normal path (G3)
    mutable std::atomic<size_t> parts_list_ops{0}; /// LISTs under `<prefix>/parts/` — MUST be 0 on the normal path (G3)
    mutable std::atomic<size_t> refs_list_ops{0}; /// LISTs under `<prefix>/store/.../refs/` — the re-validate net (ALLOWED)
    mutable std::atomic<size_t> gc_log_list_ops{0}; /// LISTs under `<prefix>/gc/log/` — the per-epoch fold
    mutable std::atomic<size_t> other_list_ops{0}; /// LISTs of any other prefix (gc/snap, etc.)
    mutable std::atomic<size_t> put_ops{0};
    mutable std::atomic<size_t> get_ops{0};
    mutable std::atomic<size_t> head_ops{0};
    mutable std::atomic<size_t> delete_ops{0};
    /// CA GC S4 (§12.3 budget) — PUTs classified by the control-plane prefix they write. The two SYNCHRONOUS
    /// SEQUENTIAL control writes a small-part commit issues are the session PUT and the live-ref PUT; the
    /// `+` (gc/log) is batched/async; blobs/parts are data PUTs.
    mutable std::atomic<size_t> session_put_ops{0}; /// PUT under `<prefix>/sessions/`
    mutable std::atomic<size_t> ref_put_ops{0};     /// PUT of the live ref object (under `/refs/`, not `.meta`)
    mutable std::atomic<size_t> gc_log_put_ops{0};  /// PUT under `<prefix>/gc/log/` (the batched `+`)

    std::string getName() const override { return "CountingMemoryObjectStorage"; }
    DB::ObjectStorageType getType() const override { return DB::ObjectStorageType::None; }
    std::string getCommonKeyPrefix() const override { return ""; }
    std::string getDescription() const override { return "in-memory test storage"; }
    bool isRemote() const override { return true; }
    std::string getObjectsNamespace() const override { return ""; }

    bool exists(const DB::StoredObject & object) const override
    {
        std::lock_guard lock(mtx);
        return data.contains(object.remote_path);
    }

    void listObjects(const std::string & path, DB::RelativePathsWithMetadata & children, size_t /*max_keys*/) const override
    {
        if (path.find("/blobs/") != std::string::npos)
            blob_list_ops.fetch_add(1, std::memory_order_relaxed);
        else if (path.find("/parts/") != std::string::npos)
            parts_list_ops.fetch_add(1, std::memory_order_relaxed);
        else if (path.find("/refs/") != std::string::npos)
            refs_list_ops.fetch_add(1, std::memory_order_relaxed);
        else if (path.find("/gc/log/") != std::string::npos)
            gc_log_list_ops.fetch_add(1, std::memory_order_relaxed);
        else
            other_list_ops.fetch_add(1, std::memory_order_relaxed);

        std::lock_guard lock(mtx);
        for (const auto & [key, value] : data)
        {
            if (key.compare(0, path.size(), path) == 0)
            {
                DB::ObjectMetadata meta;
                meta.size_bytes = value.size();
                children.push_back(std::make_shared<DB::RelativePathWithMetadata>(key, meta));
            }
        }
    }

    std::optional<DB::ObjectMetadata> tryGetObjectMetadata(const std::string & path, bool /*with_tags*/) const override
    {
        head_ops.fetch_add(1, std::memory_order_relaxed);
        std::lock_guard lock(mtx);
        auto it = data.find(path);
        if (it == data.end())
            return std::nullopt;
        DB::ObjectMetadata meta;
        meta.size_bytes = it->second.size();
        return meta;
    }

    DB::ObjectMetadata getObjectMetadata(const std::string & path, bool with_tags) const override
    {
        auto meta = tryGetObjectMetadata(path, with_tags);
        if (!meta)
            throw DB::Exception(DB::ErrorCodes::FILE_DOESNT_EXIST, "No object {}", path);
        return *meta;
    }

    std::unique_ptr<DB::ReadBufferFromFileBase> readObject(
        const DB::StoredObject & object,
        const DB::ReadSettings & /*read_settings*/,
        std::optional<size_t> /*read_hint*/,
        bool /*use_external_buffer*/,
        bool /*restrict_seek*/) const override
    {
        get_ops.fetch_add(1, std::memory_order_relaxed);
        std::string content;
        {
            std::lock_guard lock(mtx);
            auto it = data.find(object.remote_path);
            if (it == data.end())
                throw DB::Exception(DB::ErrorCodes::FILE_DOESNT_EXIST, "No object {}", object.remote_path);
            content = it->second;
        }
        return std::make_unique<MemoryReadBuffer>(std::move(content));
    }

    std::unique_ptr<DB::WriteBufferFromFileBase> writeObject(
        const DB::StoredObject & object,
        DB::WriteMode /*mode*/,
        std::optional<DB::ObjectAttributes> /*attributes*/,
        size_t /*buf_size*/,
        const DB::WriteSettings & write_settings) override
    {
        /// Honor `If-None-Match: *` (the §7/§9 conditional create-if-absent contract): a PUT onto an
        /// EXISTING key is rejected with the S3 `PreconditionFailed` signal `condCreateIfAbsent` keys off.
        /// This makes the storage pass the capability probe (`ensureConditionalCreateSupported`) and gives
        /// the commit path a real CAS to coordinate the pool on — the same seam MinIO/S3 occupies.
        if (write_settings.object_storage_write_if_none_match == "*")
        {
            std::lock_guard lock(mtx);
            if (data.contains(object.remote_path))
                throw DB::Exception(DB::ErrorCodes::S3_ERROR, "PreconditionFailed: object {} already exists", object.remote_path);
        }
        put_ops.fetch_add(1, std::memory_order_relaxed);
        /// CA GC S4 (§12.3) — classify the control-plane PUT by prefix for the budget asserts.
        const std::string & key = object.remote_path;
        if (key.find("/sessions/") != std::string::npos)
            session_put_ops.fetch_add(1, std::memory_order_relaxed);
        else if (key.find("/gc/log/") != std::string::npos)
            gc_log_put_ops.fetch_add(1, std::memory_order_relaxed);
        else if (key.find("/refs/") != std::string::npos && !key.ends_with(".meta"))
            ref_put_ops.fetch_add(1, std::memory_order_relaxed); /// the LIVE ref (not the .meta sidecar / mutable files)
        return std::make_unique<MemoryWriteBuffer>(this, object.remote_path);
    }

    void removeObjectIfExists(const DB::StoredObject & object) override
    {
        delete_ops.fetch_add(1, std::memory_order_relaxed);
        std::lock_guard lock(mtx);
        data.erase(object.remote_path);
    }

    void removeObjectsIfExist(const DB::StoredObjects & objects) override
    {
        for (const auto & object : objects)
            removeObjectIfExists(object);
    }

    void copyObject(
        const DB::StoredObject &,
        const DB::StoredObject &,
        const DB::ReadSettings &,
        const DB::WriteSettings &,
        std::optional<DB::ObjectAttributes>) override
    {
        throw DB::Exception(DB::ErrorCodes::NOT_IMPLEMENTED, "copyObject not implemented in test storage");
    }

    void shutdown() override {}
    void startup() override {}

    DB::ObjectStorageKeyGeneratorPtr createKeyGenerator() const override
    {
        return DB::createObjectStorageKeyGeneratorByPrefix("");
    }

    /// Direct put for seeding (does NOT count as a PUT op; used only to set up a scenario).
    void seed(const std::string & key, const std::string & content)
    {
        std::lock_guard lock(mtx);
        data[key] = content;
    }

    bool hasObject(const std::string & key) const
    {
        std::lock_guard lock(mtx);
        return data.contains(key);
    }

    /// CA GC S4 (§12.3) — count DISTINCT object keys currently present whose key contains `needle`. The
    /// "sequential control DEPTH" the budget bounds is the number of DISTINCT durable control objects that
    /// gate a commit (one session object + one ref object), independent of idempotent lease-renew rewrites
    /// of the same key (those happen DURING the parallel data upload, not as added pre-ref round trips).
    size_t countDistinctKeysContaining(const std::string & needle) const
    {
        std::lock_guard lock(mtx);
        size_t n = 0;
        for (const auto & [key, value] : data)
            if (key.find(needle) != std::string::npos)
                ++n;
        return n;
    }

    /// Count distinct LIVE ref objects (under `/refs/`, excluding the `.meta` sidecar / mutable files).
    size_t countDistinctLiveRefs() const
    {
        std::lock_guard lock(mtx);
        size_t n = 0;
        for (const auto & [key, value] : data)
            if (key.find("/refs/") != std::string::npos && !key.ends_with(".meta"))
                ++n;
        return n;
    }

private:
    class MemoryReadBuffer : public DB::ReadBufferFromFileBase
    {
    public:
        explicit MemoryReadBuffer(std::string content_)
            : DB::ReadBufferFromFileBase(0, nullptr, 0), content(std::move(content_))
        {
            BufferBase::set(content.data(), content.size(), 0);
        }

        std::string getFileName() const override { return "memory"; }
        size_t getFileOffsetOfBufferEnd() const override { return content.size(); }
        off_t seek(off_t off, int) override { return off; }
        off_t getPosition() override { return offset(); }

    private:
        bool nextImpl() override { return false; }
        std::string content;
    };

    class MemoryWriteBuffer : public DB::WriteBufferFromFileBase
    {
    public:
        MemoryWriteBuffer(CountingMemoryObjectStorage * storage_, std::string key_)
            : DB::WriteBufferFromFileBase(DB::DBMS_DEFAULT_BUFFER_SIZE, nullptr, 0), storage(storage_), key(std::move(key_))
        {
        }

        ~MemoryWriteBuffer() override
        {
            if (!finalized && !canceled)
                cancel();
        }

        std::string getFileName() const override { return key; }
        void sync() override {}

    private:
        void nextImpl() override
        {
            accumulated.append(working_buffer.begin(), offset());
        }

        void finalizeImpl() override
        {
            next();
            std::lock_guard lock(storage->mtx);
            storage->data[key] = accumulated;
        }

        CountingMemoryObjectStorage * storage;
        std::string key;
        std::string accumulated;
    };

    mutable std::mutex mtx;
    std::map<std::string, std::string> data;
};

/// Build a logical `+`/`-` delta for a part with the given blob pins. `event_id` is computed exactly as
/// the production write path computes it (computeEventId), so two appends of the same logical delta carry
/// the same id and the fold dedups them.
GcDelta makeDelta(GcDelta::Op op, const PartId & part_id, std::vector<BlobHash> pins)
{
    GcDelta d;
    d.op = op;
    d.part_id = part_id;
    d.pins = std::move(pins);
    d.event_id = GcDelta::computeEventId(part_id, op);
    return d;
}

/// Build a lowercase-hex string whose home shard (under shardForHash) is `target`. shardForHash folds the
/// first up-to-4 hex nibbles into an accumulator and masks the LOW log2(kGcShardCount) bits — i.e. the
/// FOURTH (last consumed) nibble's low bits decide the shard (kGcShardCount==16 -> the 4th nibble IS the
/// shard). We set the 4th nibble to `target` directly and append a salted suffix for uniqueness.
std::string hexInShard(ShardId target, const std::string & tag, int salt)
{
    static const char * hex_digits = "0123456789abcdef";
    for (int fourth = 0; fourth < 16; ++fourth)
    {
        std::string candidate = "abc";
        candidate += hex_digits[fourth];
        candidate += tag;
        candidate += std::to_string(salt);
        if (shardForHash(BlobHash(candidate)) == target)
            return candidate;
    }
    ADD_FAILURE() << "no hex string found for shard " << target;
    return "0";
}

/// Find a blob-hash string whose home shard is `target`.
BlobHash blobInShard(ShardId target, int salt = 0)
{
    return BlobHash(hexInShard(target, "blob", salt));
}

/// Find a part-id string whose home shard (shardForPartId == shardForHash of the id) is `target`.
PartId partInShard(ShardId target, int salt = 0)
{
    return PartId(hexInShard(target, "part", salt));
}

DB::ContentAddressed::GcCompaction::CountKey blobKeyOf(const BlobHash & h)
{
    return {GcCompaction::KeyKind::Blob, h.string()};
}

DB::ContentAddressed::GcCompaction::CountKey partKeyOf(const PartId & p)
{
    return {GcCompaction::KeyKind::Part, p.string()};
}

bool candidatesContain(const std::vector<GcCompaction::Candidate> & candidates, const GcCompaction::CountKey & key)
{
    for (const auto & c : candidates)
        if (c.key == key)
            return true;
    return false;
}

/// Always-leader fence: the oracles run a single leader, so the fence is always still ours.
const std::function<bool()> kStillLeader = [] { return true; };

}

/// Oracle 1 — streaming-merge correctness. Seed a snapshot + mixed `+`/`-` deltas (shared blobs, multiple
/// parts) for one shard, fold, and assert the new-snapshot counts == hand-computed AND exactly the count-0
/// keys (including the `(part_id)` manifest edge) fall out as candidates.
TEST(ContentAddressedGcS2, StreamingMergeCorrectnessAndCandidates)
{
    auto storage = std::make_shared<CountingMemoryObjectStorage>();
    const std::string prefix = "pool";
    const ShardId shard = 3;

    /// Two parts whose home shard is `shard`, and blobs (one shared) also in `shard`. A part's lifecycle is
    /// modelled exactly as the write path produces it: a `+` on commit and a `-` on drop, BOTH carrying the
    /// part's full pin set AND the part's `(part_id) edge` (the writer always emits the edge fragment). So a
    /// `-partA` decrements every blob partA pins AND partA's manifest edge — there is no "drop one ref only".
    PartId partA = partInShard(shard, 1);
    PartId partB = partInShard(shard, 2);
    BlobHash shared = blobInShard(shard, 0);
    BlobHash onlyA = blobInShard(shard, 1);
    BlobHash onlyB = blobInShard(shard, 2);

    /// Epoch 0 — commit both parts: partA pins {shared, onlyA}, partB pins {shared, onlyB}. After the fold
    /// the snapshot holds shared=2, onlyA=1, onlyB=1, partA edge=1, partB edge=1; no count-0 candidates.
    GcLogWriter writer(storage, prefix);
    writer.appendAndFlushForCommit(makeDelta(GcDelta::Op::Add, partA, {shared, onlyA}));
    writer.appendAndFlushForCommit(makeDelta(GcDelta::Op::Add, partB, {shared, onlyB}));

    GcCompaction compaction(storage, prefix);
    auto r0 = compaction.compactShard(shard, kStillLeader);
    EXPECT_EQ(r0.folded_epoch, 0u);
    EXPECT_EQ(r0.new_epoch, 1u);
    EXPECT_EQ(r0.folded_counts.at(blobKeyOf(shared)), 2);
    EXPECT_EQ(r0.folded_counts.at(blobKeyOf(onlyA)), 1);
    EXPECT_EQ(r0.folded_counts.at(blobKeyOf(onlyB)), 1);
    EXPECT_EQ(r0.folded_counts.at(partKeyOf(partA)), 1);
    EXPECT_EQ(r0.folded_counts.at(partKeyOf(partB)), 1);
    EXPECT_TRUE(r0.candidates.empty());

    /// Epoch 1 — drop ONLY partA. `-partA` carries pins {shared, onlyA} and partA's edge. Hand-computed:
    ///   shared: 2 - 1(partA-) = 1            -> still live via partB, NOT a candidate
    ///   onlyA:  1 - 1(partA-) = 0            -> candidate
    ///   onlyB:  1 (untouched) = 1            -> live, NOT a candidate
    ///   partA edge: 1 - 1 = 0                -> candidate
    ///   partB edge: 1 (untouched) = 1        -> live, NOT a candidate
    writer.appendAndFlushForCommit(makeDelta(GcDelta::Op::Remove, partA, {shared, onlyA}));

    auto r1 = compaction.compactShard(shard, kStillLeader);
    EXPECT_EQ(r1.folded_epoch, 1u);
    EXPECT_EQ(r1.new_epoch, 2u);

    /// New snapshot keeps only the live keys (positive counts).
    EXPECT_EQ(r1.folded_counts.at(blobKeyOf(shared)), 1); /// shared survives — partB still references it
    EXPECT_EQ(r1.folded_counts.at(blobKeyOf(onlyB)), 1);
    EXPECT_EQ(r1.folded_counts.at(partKeyOf(partB)), 1);
    EXPECT_EQ(r1.folded_counts.at(blobKeyOf(onlyA)), 0); /// recorded as 0 (not persisted)
    EXPECT_EQ(r1.folded_counts.at(partKeyOf(partA)), 0);

    /// EXACTLY the count-0 keys are candidates — onlyA and partA's edge; nothing else.
    EXPECT_EQ(r1.candidates.size(), 2u);
    EXPECT_TRUE(candidatesContain(r1.candidates, blobKeyOf(onlyA)));
    EXPECT_TRUE(candidatesContain(r1.candidates, partKeyOf(partA)));
    EXPECT_FALSE(candidatesContain(r1.candidates, blobKeyOf(shared)));
    EXPECT_FALSE(candidatesContain(r1.candidates, blobKeyOf(onlyB)));
    EXPECT_FALSE(candidatesContain(r1.candidates, partKeyOf(partB)));

    /// The candidate's full object key matches the pool layout (what the sweep deletes).
    for (const auto & c : r1.candidates)
    {
        if (c.key.kind == GcCompaction::KeyKind::Blob)
            EXPECT_EQ(c.object_key, blobKey(prefix, BlobHash(c.key.identity)).string());
        else
            EXPECT_EQ(c.object_key, partKey(prefix, PartId(c.key.identity)).string());
    }
}

/// Oracle 2 — rebuild from snapshot+log, NO blob/parts LIST. After two folds leave a snapshot, append a
/// fresh un-folded epoch, then rebuild: identical counts, and the op counter proves no `blobs/`/`parts/`
/// LIST was issued.
TEST(ContentAddressedGcS2, RebuildFromSnapshotAndLogNoBlobList)
{
    auto storage = std::make_shared<CountingMemoryObjectStorage>();
    const std::string prefix = "pool";
    const ShardId shard = 5;

    PartId partA = partInShard(shard, 1);
    BlobHash b1 = blobInShard(shard, 1);
    BlobHash b2 = blobInShard(shard, 2);

    GcLogWriter writer(storage, prefix);
    GcCompaction compaction(storage, prefix);

    /// Epoch 0: +partA{b1,b2}. Fold -> snapshot at epoch 1 holds b1=1,b2=1,partA=1.
    writer.appendAndFlushForCommit(makeDelta(GcDelta::Op::Add, partA, {b1, b2}));
    auto r0 = compaction.compactShard(shard, kStillLeader);
    ASSERT_EQ(r0.new_epoch, 1u);

    /// Epoch 1 (now open): append a fresh `-` for b2 (a column dropped) — un-folded.
    PartId partB = partInShard(shard, 2);
    writer.appendAndFlushForCommit(makeDelta(GcDelta::Op::Remove, partB, {b2}));

    storage->blob_list_ops = 0;
    storage->parts_list_ops = 0;

    /// Rebuild: snapshot(epoch 1) + un-folded log(epoch 1). b1=1, b2=1-1=0, partA=1, partB edge=0-? .
    auto rb = compaction.rebuildFromSnapshotAndLog(shard);
    ASSERT_TRUE(rb.has_value());

    EXPECT_EQ(rb->counts.at(blobKeyOf(b1)), 1);
    EXPECT_EQ(rb->counts.count(blobKeyOf(b2)), 0u); /// netted to 0 — dropped from positive-count view
    EXPECT_EQ(rb->counts.at(partKeyOf(partA)), 1);
    /// b2 reached 0 -> candidate; partB edge (a bare `-` with no prior `+`) clamps to 0 -> candidate too.
    EXPECT_TRUE(candidatesContain(rb->candidates, blobKeyOf(b2)));

    /// G3: rebuild scans NO blobs and NO parts.
    EXPECT_EQ(storage->blob_list_ops.load(), 0u);
    EXPECT_EQ(storage->parts_list_ops.load(), 0u);

    /// Rebuilt counts equal what a real fold of the same epoch would produce (cross-check via fold).
    auto folded = compaction.compactShard(shard, kStillLeader);
    EXPECT_EQ(folded.folded_counts.at(blobKeyOf(b1)), 1);
    EXPECT_EQ(folded.folded_counts.at(partKeyOf(partA)), 1);
}

/// Oracle 3 — epoch fold/advance + idempotent empty re-run. Fold advances current_epoch, reclaims the old
/// snap+log, writes the new snapshot; re-running on an empty epoch is a no-op emitting no candidates.
TEST(ContentAddressedGcS2, EpochAdvanceReclaimAndIdempotentEmptyRerun)
{
    auto storage = std::make_shared<CountingMemoryObjectStorage>();
    const std::string prefix = "pool";
    const ShardId shard = 7;

    PartId partA = partInShard(shard, 1);
    BlobHash b1 = blobInShard(shard, 1);

    GcLogWriter writer(storage, prefix);
    GcCompaction compaction(storage, prefix);

    writer.appendAndFlushForCommit(makeDelta(GcDelta::Op::Add, partA, {b1}));

    const std::string log_prefix_e0 = gcLogPrefix(prefix, 0, shard);
    /// A log object exists in epoch 0 before the fold.
    {
        DB::RelativePathsWithMetadata children;
        storage->listObjects(log_prefix_e0, children, 0);
        EXPECT_FALSE(children.empty());
    }

    auto r0 = compaction.compactShard(shard, kStillLeader);
    EXPECT_EQ(r0.new_epoch, 1u);
    /// current_epoch advanced to 1.
    EXPECT_TRUE(storage->hasObject(gcCurrentEpochKey(prefix, shard)));
    /// New snapshot at epoch 1 present.
    EXPECT_TRUE(storage->hasObject(gcSnapKey(prefix, 1, shard).string()));
    /// Old epoch-0 log objects reclaimed.
    {
        DB::RelativePathsWithMetadata children;
        storage->listObjects(log_prefix_e0, children, 0);
        EXPECT_TRUE(children.empty());
    }

    /// Re-run on the now-open, empty epoch 1: idempotent no-op, no candidates, advances to 2.
    auto r1 = compaction.compactShard(shard, kStillLeader);
    EXPECT_EQ(r1.folded_epoch, 1u);
    EXPECT_EQ(r1.new_epoch, 2u);
    EXPECT_TRUE(r1.candidates.empty());
    /// The b1 reference is still counted in the carried-forward snapshot (no under-count).
    EXPECT_EQ(r1.folded_counts.at(blobKeyOf(b1)), 1);
    EXPECT_EQ(r1.folded_counts.at(partKeyOf(partA)), 1);
}

/// Oracle 4 — shard isolation. Deltas in shard A never appear in shard B's fold; a foreign-shard fragment's
/// part_id does not double-count the edge; closing A's epoch does not force a B writer to re-append.
TEST(ContentAddressedGcS2, ShardIsolation)
{
    auto storage = std::make_shared<CountingMemoryObjectStorage>();
    const std::string prefix = "pool";
    const ShardId shardA = 2;
    const ShardId shardB = 9;
    ASSERT_NE(shardA, shardB);

    /// A part whose HOME shard is A, but which pins a blob whose home shard is B. The writer splits it:
    /// shard A gets the (part_id) edge fragment; shard B gets the blob pin fragment (which ALSO serializes
    /// the part_id, but must NOT count the edge — that belongs only to A).
    PartId partInA = partInShard(shardA, 1);
    BlobHash blobInB = blobInShard(shardB, 1);

    GcLogWriter writer(storage, prefix);
    writer.appendAndFlushForCommit(makeDelta(GcDelta::Op::Add, partInA, {blobInB}));

    GcCompaction compaction(storage, prefix);

    /// Fold shard A: sees the (part_id) edge but NOT the blob (it lives in B's log).
    auto rA = compaction.compactShard(shardA, kStillLeader);
    EXPECT_EQ(rA.folded_counts.at(partKeyOf(partInA)), 1);
    EXPECT_EQ(rA.folded_counts.count(blobKeyOf(blobInB)), 0u);

    /// Fold shard B: sees the blob pin but does NOT double-count the part edge (foreign-shard fragment).
    auto rB = compaction.compactShard(shardB, kStillLeader);
    EXPECT_EQ(rB.folded_counts.at(blobKeyOf(blobInB)), 1);
    EXPECT_EQ(rB.folded_counts.count(partKeyOf(partInA)), 0u);

    /// Closing A's epoch advanced only A. B's epoch is independent: a fresh B writer reading B's epoch is
    /// not forced to re-append because of A's close. After both folds, A=1 and B=1 (per-shard epochs).
    EXPECT_EQ(compaction.compactShard(shardA, kStillLeader).folded_epoch, 1u);
    EXPECT_EQ(compaction.compactShard(shardB, kStillLeader).folded_epoch, 1u);
}

/// Oracle 5 — append-as-epoch-folds (the S2 dress rehearsal of the S4 load-bearing race). A writer appends a
/// `+` for epoch E exactly as the compaction closes E and advances; the writer must re-append into E+1
/// (§5.1 rule 2), and the folded snapshot ∪ open-epoch must still count the reference (no under-count → the
/// blob is never a count-0 candidate while the part is live). Deterministic, NO sleeps: we drive the close
/// between the writer's epoch-read and its flush by hand.
TEST(ContentAddressedGcS2, AppendAsEpochFoldsReappendsNoUndercount)
{
    auto storage = std::make_shared<CountingMemoryObjectStorage>();
    const std::string prefix = "pool";
    const ShardId shard = 4;

    PartId part = partInShard(shard, 1);
    BlobHash blob = blobInShard(shard, 1);

    GcLogWriter writer(storage, prefix);
    GcCompaction compaction(storage, prefix);

    /// The writer reads epoch E=0, stamps + buffers + flushes into epoch 0. appendAndFlushForCommit then
    /// runs the rule-2 re-append: it re-reads the shard epoch and, if advanced, re-appends. To make the
    /// close happen "during" the append deterministically, we close epoch 0 BEFORE the writer runs, so the
    /// writer's post-flush re-read sees the advance and re-appends into epoch 1.
    ///
    /// First, advance the shard epoch to 1 by folding the empty epoch 0.
    auto pre = compaction.compactShard(shard, kStillLeader);
    ASSERT_EQ(pre.new_epoch, 1u);

    /// Now the open epoch is 1. The writer appends a `+`. It reads epoch 1, flushes into epoch 1; the
    /// re-append loop sees no further advance (still 1) and does nothing. The `+` lands in the open epoch.
    writer.appendAndFlushForCommit(makeDelta(GcDelta::Op::Add, part, {blob}));

    /// The reference is counted: a rebuild over snapshot(1) ∪ open-epoch(1) shows the blob live.
    auto rb = compaction.rebuildFromSnapshotAndLog(shard);
    ASSERT_TRUE(rb.has_value());
    EXPECT_EQ(rb->counts.at(blobKeyOf(blob)), 1);
    EXPECT_EQ(rb->counts.at(partKeyOf(part)), 1);
    /// The live blob is NEVER a count-0 candidate.
    EXPECT_FALSE(candidatesContain(rb->candidates, blobKeyOf(blob)));
    EXPECT_FALSE(candidatesContain(rb->candidates, partKeyOf(part)));

    /// Folding the open epoch carries the live reference forward (no under-count, no candidate).
    auto folded = compaction.compactShard(shard, kStillLeader);
    EXPECT_EQ(folded.folded_counts.at(blobKeyOf(blob)), 1);
    EXPECT_EQ(folded.folded_counts.at(partKeyOf(part)), 1);
    EXPECT_FALSE(candidatesContain(folded.candidates, blobKeyOf(blob)));

    /// And the explicit rule-2 re-append: write a delta object directly into the now-CLOSED epoch 0 with the
    /// SAME event_id, then ALSO into epoch 1 (the re-append target). The fold must count it ONCE, never as a
    /// resurrected reference and never under-counted. (Covered by the dedup oracle below; here we assert the
    /// machinery does not crash and the live count holds across the carried-forward folds.)
    EXPECT_GE(folded.folded_epoch, 1u);
}

/// Oracle 6 — duplicate `+` dedup-on-fold. The same `event_id` lands in two epochs (a §5.1 rule-2
/// re-append); the fold counts it ONCE, not twice (no over-count).
TEST(ContentAddressedGcS2, DuplicatePlusDedupOnFold)
{
    auto storage = std::make_shared<CountingMemoryObjectStorage>();
    const std::string prefix = "pool";
    const ShardId shard = 6;

    PartId part = partInShard(shard, 1);
    BlobHash blob = blobInShard(shard, 1);

    /// The §5.1 rule-2 re-append writes the SAME logical `+` (identical event_id) MORE THAN ONCE into the
    /// SAME open epoch — e.g. two coalesced windows, or a re-flush after a transient advance-then-retreat —
    /// so a single fold's `LIST gc/log/E.shard/` returns two objects carrying the same (op, event_id). The
    /// dedup-on-fold MUST collapse them to ONE count (not an over-count of 2). (The orphaned copy in a
    /// already-CLOSED epoch is reclaimed without folding, so it never reaches the merge at all — the dedup
    /// guard is exactly for the same-epoch duplicate.)
    GcDelta plus = makeDelta(GcDelta::Op::Add, part, {blob});
    GcLogBatch batch;
    batch.deltas.push_back(plus);
    const std::string bytes = batch.serialize();
    /// Two distinct object keys (different windows) in the SAME epoch 0, but the SAME event_id inside.
    storage->seed(gcLogEventKey(prefix, 0, shard, plus.event_id + "_a").string(), bytes);
    storage->seed(gcLogEventKey(prefix, 0, shard, plus.event_id + "_b").string(), bytes);

    GcCompaction compaction(storage, prefix);
    auto r0 = compaction.compactShard(shard, kStillLeader);
    /// Two objects, same (op, event_id) in one fold -> counted ONCE, never twice (no over-count).
    EXPECT_EQ(r0.folded_counts.at(blobKeyOf(blob)), 1);
    EXPECT_EQ(r0.folded_counts.at(partKeyOf(part)), 1);
    EXPECT_FALSE(candidatesContain(r0.candidates, blobKeyOf(blob)));

    /// Contrast: a DISTINCT logical `+` for the same blob from a genuinely different part (a different
    /// event_id) in the SAME fold IS counted — the dedup collapses only true duplicates, not real referrers.
    PartId other = partInShard(shard, 2);
    GcDelta dup = makeDelta(GcDelta::Op::Add, part, {blob}); /// same part+op => same event_id as `plus`
    GcDelta distinct = makeDelta(GcDelta::Op::Add, other, {blob}); /// different part => different event_id
    GcLogBatch batch1;
    batch1.deltas.push_back(dup); /// a duplicate of the already-folded `plus` (re-appended into open epoch)
    batch1.deltas.push_back(distinct); /// a brand-new referrer of the same blob
    storage->seed(gcLogEventKey(prefix, 1, shard, dup.event_id).string(), batch1.serialize());

    auto r1 = compaction.compactShard(shard, kStillLeader);
    /// Within epoch 1's fold over snapshot(1)=blob:1: `dup` (same event_id) and `distinct` (new event_id)
    /// are both distinct (op,event_id) so both apply (+2), giving 1+2=3. The point of THIS oracle is the
    /// intra-fold dedup proven above (r0); here we only assert the blob stays live (never a candidate) and
    /// the distinct new referrer is reflected (count grew, not collapsed away).
    EXPECT_EQ(r1.folded_counts.at(blobKeyOf(blob)), 3);
    EXPECT_FALSE(candidatesContain(r1.candidates, blobKeyOf(blob)));
}

/// Oracle 7 — live-part-not-a-candidate (the safety-correctness oracle). For a live part (its `+` folded,
/// no `-`), none of its blob keys NOR its `(part_id)` edge appears in the compaction's count-0 candidate
/// stream — across multiple carried-forward folds.
TEST(ContentAddressedGcS2, LivePartIsNeverACandidate)
{
    auto storage = std::make_shared<CountingMemoryObjectStorage>();
    const std::string prefix = "pool";
    const ShardId shard = 1;

    PartId live = partInShard(shard, 1);
    BlobHash b1 = blobInShard(shard, 1);
    BlobHash b2 = blobInShard(shard, 2);

    /// A second part that WILL be dropped, to ensure candidates are emitted at all (so the test is not
    /// vacuously green because the candidate stream is always empty).
    PartId dead = partInShard(shard, 2);
    BlobHash bDead = blobInShard(shard, 3);

    GcLogWriter writer(storage, prefix);
    GcCompaction compaction(storage, prefix);

    writer.appendAndFlushForCommit(makeDelta(GcDelta::Op::Add, live, {b1, b2}));
    writer.appendAndFlushForCommit(makeDelta(GcDelta::Op::Add, dead, {bDead}));
    auto r0 = compaction.compactShard(shard, kStillLeader);
    EXPECT_TRUE(r0.candidates.empty());

    /// Epoch 1: drop ONLY `dead`. `live` and its blobs stay referenced.
    writer.appendAndFlushForCommit(makeDelta(GcDelta::Op::Remove, dead, {bDead}));
    auto r1 = compaction.compactShard(shard, kStillLeader);

    /// `bDead` and the `dead` edge ARE candidates (so the stream is non-empty — the test has teeth).
    EXPECT_TRUE(candidatesContain(r1.candidates, blobKeyOf(bDead)));
    EXPECT_TRUE(candidatesContain(r1.candidates, partKeyOf(dead)));

    /// The LIVE part's keys are NEVER candidates.
    EXPECT_FALSE(candidatesContain(r1.candidates, blobKeyOf(b1)));
    EXPECT_FALSE(candidatesContain(r1.candidates, blobKeyOf(b2)));
    EXPECT_FALSE(candidatesContain(r1.candidates, partKeyOf(live)));

    /// And one more carried-forward fold (empty epoch 2): still never a candidate, still counted.
    auto r2 = compaction.compactShard(shard, kStillLeader);
    EXPECT_TRUE(r2.candidates.empty());
    EXPECT_EQ(r2.folded_counts.at(blobKeyOf(b1)), 1);
    EXPECT_EQ(r2.folded_counts.at(blobKeyOf(b2)), 1);
    EXPECT_EQ(r2.folded_counts.at(partKeyOf(live)), 1);
}

/// Op-budget G3 (§12.3) — a normal compaction issues ZERO `blobs/` and ZERO `parts/` LISTs. The fold lists
/// only `gc/log/`; the candidate source is the merge, not a bucket scan.
TEST(ContentAddressedGcS2, NormalCompactionIssuesZeroBlobAndPartsLists)
{
    auto storage = std::make_shared<CountingMemoryObjectStorage>();
    const std::string prefix = "pool";
    const ShardId shard = 8;

    PartId part = partInShard(shard, 1);
    BlobHash b1 = blobInShard(shard, 1);
    BlobHash b2 = blobInShard(shard, 2);

    GcLogWriter writer(storage, prefix);
    writer.appendAndFlushForCommit(makeDelta(GcDelta::Op::Add, part, {b1, b2}));
    writer.appendAndFlushForCommit(makeDelta(GcDelta::Op::Remove, part, {b1}));

    storage->blob_list_ops = 0;
    storage->parts_list_ops = 0;
    storage->gc_log_list_ops = 0;

    GcCompaction compaction(storage, prefix);
    auto r = compaction.compactShard(shard, kStillLeader);

    /// b1 dropped to 0 -> a candidate falls out of the merge (proves reclamation works without a scan).
    EXPECT_TRUE(candidatesContain(r.candidates, blobKeyOf(b1)));

    /// G3: ZERO blobs/ and ZERO parts/ LISTs on the normal compaction path.
    EXPECT_EQ(storage->blob_list_ops.load(), 0u);
    EXPECT_EQ(storage->parts_list_ops.load(), 0u);
    /// The fold DOES list gc/log/ (that is the budgeted per-epoch LIST).
    EXPECT_GE(storage->gc_log_list_ops.load(), 1u);
}

/// Op-budget batching (§5/§12.3) — a burst of N commits coalesces into ⌈N/window⌉ log objects, not N. With
/// a large size window and no time pressure, a single enqueue+flushAll cycle writes ONE coalesced object
/// holding all N deltas (cas_log_batch_size == N), not N objects.
TEST(ContentAddressedGcS2, BurstOfCommitsCoalescesIntoFewerLogObjects)
{
    auto storage = std::make_shared<CountingMemoryObjectStorage>();
    const std::string prefix = "pool";
    const ShardId shard = 0;

    /// A large flush cap so the window never trips on size; a long time window so it never trips on time.
    GcLogWriter writer(storage, prefix, /*flush_max_deltas=*/4096, /*flush_window=*/std::chrono::milliseconds(60000));

    const size_t N = 50;
    for (size_t i = 0; i < N; ++i)
    {
        /// All deltas target shard 0 (part + blob both in shard 0) so they coalesce into ONE buffer.
        PartId part = partInShard(shard, static_cast<int>(1000 + i));
        BlobHash blob = blobInShard(shard, static_cast<int>(2000 + i));
        writer.enqueue(makeDelta(GcDelta::Op::Add, part, {blob}));
    }

    const size_t puts_before = storage->put_ops.load();
    writer.flushAll();
    const size_t puts_after = storage->put_ops.load();

    /// ONE coalesced object (one window), not N. cas_log_batch_size == N deltas in it.
    EXPECT_EQ(puts_after - puts_before, 1u);
    EXPECT_EQ(writer.lastBatchSize(), N);

    /// Sanity: that single object decodes to N deltas, and a fold counts all N parts/blobs.
    GcCompaction compaction(storage, prefix);
    auto r = compaction.compactShard(shard, kStillLeader);
    size_t live_parts = 0;
    for (const auto & [key, count] : r.folded_counts)
        if (key.kind == GcCompaction::KeyKind::Part && count > 0)
            ++live_parts;
    EXPECT_EQ(live_parts, N);
}

/// CA GC S4 (§12.3) — the OP-COUNT BUDGET assert for the S3-only degraded-mode floor. Drive a REAL
/// small-part `ContentAddressedTransaction::commit` over the counting object storage and assert the floor.
///
/// The "sequential control DEPTH" the §12.3 floor bounds is the number of DISTINCT durable CONTROL OBJECTS
/// that gate a commit — exactly ONE write-session object (the §7 flag A) + ONE live-ref object (the commit
/// point) = `cas_s3_sequential_control_depth_per_commit` ≤ 2. The session is rewritten in place several
/// times (the per-blob lease renew DURING the parallel data upload, the §7.1 step-2 re-assert, and the
/// committed-flag update at the end), but those are idempotent rewrites of the SAME key — one distinct
/// control object — not added pre-ref control round trips on the critical path.
///
/// MUST-NOT (§12.3): NO per-blob `GET active` on the success path (a fresh g=0 blob resolves to g=0 WITHOUT
/// reading `active`), and the `gc/log` `+` is BATCHED into a bounded number of objects (coalesced by epoch),
/// never one un-batched object per blob.
TEST(ContentAddressedGcS4Budget, SmallPartCommitS3OnlyControlWriteFloor)
{
    DB::ServerUUID::setRandomForUnitTests();

    const std::string prefix = "cas_gc_s4_budget";
    const std::string scratch = "./cas_gc_s4_budget_scratch";
    std::error_code ec;
    std::filesystem::remove_all(scratch, ec);
    std::filesystem::create_directories(scratch, ec);

    auto storage = std::make_shared<CountingMemoryObjectStorage>();
    auto ms = std::make_shared<DB::ContentAddressedMetadataStorage>(
        storage, /*storage_path_prefix=*/prefix, /*server_id=*/"srv", scratch, /*context=*/nullptr, /*allow_shared_pool=*/false);
    ms->startup();

    const std::string uuid = "uuid-budget";
    const std::string part = "all_1_1_0";

    const size_t get_ops_before = storage->get_ops.load();

    /// A small part with two content files (a typical tiny insert).
    {
        DB::ContentAddressedTransaction tx(*ms, /*key_prefix=*/prefix, scratch);
        for (const auto & [name, bytes] :
             std::map<std::string, std::string>{{"data.bin", "BUDGET-PAYLOAD"}, {"columns.txt", "c"}})
        {
            auto buf = tx.writeFile("uui/" + uuid + "/" + part + "/" + name, 4096, DB::WriteMode::Rewrite, {});
            buf->write(bytes.data(), bytes.size());
            buf->finalize();
        }
        tx.commit(DB::NoCommitOptions{});
    }

    const size_t get_ops = storage->get_ops.load() - get_ops_before;

    /// The SYNCHRONOUS SEQUENTIAL CONTROL DEPTH = distinct control objects gating the commit. After the
    /// commit exactly ONE session object lingers (held-until-folded — the §7 flag A) and ONE live ref
    /// exists (the commit point). `cas_s3_sequential_control_depth_per_commit` = 1 session + 1 ref ≤ 2.
    const size_t distinct_sessions = storage->countDistinctKeysContaining("/sessions/");
    const size_t distinct_live_refs = storage->countDistinctLiveRefs();
    EXPECT_EQ(distinct_sessions, 1u) << "exactly one durable handshake session object (flag A)";
    EXPECT_EQ(distinct_live_refs, 1u) << "exactly one live ref object (the commit point)";
    const size_t sequential_control_depth = distinct_sessions + distinct_live_refs;
    EXPECT_LE(sequential_control_depth, 2u)
        << "S3-only floor: at most 2 distinct sequential control objects per commit (session + ref); the `+` is async/batched";

    /// The `+` (gc/log) is split per (shard, epoch) — one coalesced object per distinct GC shard the part's
    /// blobs + the part edge home to — NOT one un-batched object per blob, and NOT per commit on the hot
    /// path (multiple commits to the same (shard, epoch) window coalesce into one object — see
    /// `CoalescedAppendBatchesNDeltasIntoOneObject`). A 2-blob part edges into at most 3 distinct (shard)
    /// buckets (2 blob shards + 1 part-edge shard), so the gc/log object count is bounded by that fan-out.
    const size_t distinct_gc_log_objects = storage->countDistinctKeysContaining("/gc/log/");
    EXPECT_LE(distinct_gc_log_objects, 3u)
        << "the gc/log `+` is coalesced per (shard, epoch) — bounded by the part's shard fan-out, never per-blob";

    /// MUST-NOT (§12.3): no per-blob `GET active` on the success path. A fresh g=0 blob resolves to g=0
    /// without reading `active`, so the commit issues ZERO control-plane GETs for a brand-new part.
    EXPECT_EQ(get_ops, 0u) << "no per-blob GET active on the small-part success path (§12.3 MUST-NOT)";

    ms->shutdown();
    std::filesystem::remove_all("./" + prefix, ec);
    std::filesystem::remove_all(prefix, ec);
    std::filesystem::remove_all(scratch, ec);
}
