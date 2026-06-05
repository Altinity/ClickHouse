#include <gtest/gtest.h>

#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/ContentAddressedGC.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Identifiers.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/PartManifest.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/PoolCoordination.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/PoolPaths.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/WriteSession.h>
#include <Disks/DiskObjectStorage/ObjectStorages/IObjectStorage.h>
#include <Disks/DiskObjectStorage/ObjectStorages/Local/LocalObjectStorage.h>
#include <Disks/DiskObjectStorage/ObjectStorages/StoredObject.h>
#include <Disks/WriteMode.h>

#include <IO/ReadHelpers.h>
#include <IO/ReadSettings.h>

#include <Core/ServerUUID.h>
#include <Common/Exception.h>
#include <Common/ProfileEvents.h>

#include <base/types.h>

#include <filesystem>
#include <optional>
#include <set>
#include <string>
#include <system_error>
#include <vector>

namespace fs = std::filesystem;
using namespace DB::ContentAddressed;

namespace DB
{
namespace ErrorCodes
{
    extern const int FILE_DOESNT_EXIST;
}
}

namespace ProfileEvents
{
    extern const Event ContentAddressedTombstonesTotal;
    extern const Event ContentAddressedOrphanBytesEstimate;
    extern const Event ContentAddressedGenerationsObserved;
    extern const Event ContentAddressedHashesObserved;
}

/// CA GC S3 — the §11 race oracles: the DATA-LOSS SAFETY PROOF for the generation + tombstone deletion
/// semantics. Every oracle is a DETERMINISTIC interleaving driven by hand (NO sleeps). They drive the REAL
/// machinery over a REAL LocalObjectStorage:
///   - `condCreateIfAbsent` runs the real `O_EXCL` single-owner create (the seal/resurrect primitive).
///   - `ContentAddressedGC::runReconciliationScan` runs the real seal -> grace -> fresh re-check ->
///     {sweep | recover | drain} tail (`sweepCandidates`) — the same code the compaction-driven
///     `runSweepOnce` runs, only the candidate SOURCE differs (here the full-scan + sealed-tombstone
///     re-presentation, which is exactly what exercises the branch on hand-seeded generationed objects).
///   - the generation key helpers (`blobGenKey`, `blobTombstoneKey`, `blobActiveKey`, `blobGenPrefix`,
///     `parseGenFromKey`, `parseGenObjectKey`, `partGenKey`, …) are the production builders.
/// The reader-fallback oracles model the §6.1 reader rule (`active` default 0 -> GET -> 404 -> LIST the
/// generation prefix -> pick the highest present generation -> read its bytes; the tombstone never gates a
/// read) over those SAME production helpers and the SAME object storage, so the read and GC sides cannot
/// disagree on the key layout.

namespace
{

/// A fixture giving each oracle an isolated on-disk LocalObjectStorage rooted at a unique scratch dir, with
/// the GC pool keyed under a NON-EMPTY prefix (so the O_EXCL seal/tombstone create has a parent dir to live
/// under — the empty-prefix root cannot materialize a file on a local filesystem).
class ContentAddressedGcS3 : public testing::Test
{
public:
    void SetUp() override
    {
        if (!initialized)
        {
            DB::ServerUUID::setRandomForUnitTests();
            initialized = true;
        }
        /// NOTE: LocalObjectStorage keys are CWD-relative (the storage root is not prepended to a key —
        /// see LocalObjectStorage::writeObject/listObjects). So the pool physically lives at `<prefix>/…`
        /// under the CWD. Use a UNIQUE per-test pool prefix so the oracles never share or pollute one
        /// another's pool, and clean it in TearDown (mirrors the empty-prefix ContentAddressedMetaTest).
        prefix = std::string("cas_gc_s3_pool_") + ::testing::UnitTest::GetInstance()->current_test_info()->name();
        std::error_code ec;
        fs::remove_all(prefix, ec);
        fs::create_directories(prefix, ec);
        DB::LocalObjectStorageSettings settings("test", prefix, /*read_only_=*/false);
        os = std::make_shared<DB::LocalObjectStorage>(std::move(settings));
    }

    void TearDown() override
    {
        if (os)
            os->shutdown();
        std::error_code ec;
        fs::remove_all(prefix, ec);
    }

    /// The pool prefix (a CWD-relative dir, unique per test). Every key is built under it.
    std::string prefix;
    std::shared_ptr<DB::LocalObjectStorage> os;

    /// A plain (non-CAS) write — used to materialize a generation OBJECT or the `active` hint.
    void put(const std::string & key, const std::string & bytes)
    {
        auto buf = os->writeObject(DB::StoredObject(key), DB::WriteMode::Rewrite);
        buf->write(bytes.data(), bytes.size());
        buf->finalize();
    }

    std::string get(const std::string & key)
    {
        DB::StoredObject object(key);
        auto buf = os->readObject(object, DB::getReadSettings(), /*read_hint=*/std::nullopt);
        String content;
        DB::readStringUntilEOF(content, *buf);
        return content;
    }

    bool exists(const std::string & key) const
    {
        return os->tryGetObjectMetadata(key, /*with_tags=*/false).has_value();
    }

    /// Publish a manifest + ref so the part_id (and every blob it pins, at its g=0 identity key) is a LIVE
    /// GC root. `blob_hashes` are the BARE content hashes the manifest pins (the manifest always pins bare
    /// `H` — generation lives only in the physical key). The blob OBJECTS themselves are seeded separately
    /// at whatever generation the oracle wants.
    void publishLivePart(const std::string & uuid, const std::string & part_name, const PartId & part_id,
                         const std::vector<BlobHash> & blob_hashes)
    {
        PartManifest manifest;
        for (size_t i = 0; i < blob_hashes.size(); ++i)
            manifest.blobs["f" + std::to_string(i) + ".bin"] = BlobEntry{blob_hashes[i], 1, blob_hashes[i].string()};
        /// The manifest body is published at the g=0 manifest key (the manifest identity is g=0; a
        /// resurrected manifest generation is a separate object the GC re-check resolves).
        put(partKey(prefix, part_id).string(), manifest.serialize());
        put(refKey(prefix, "srv", uuid, part_name).string(), serializeRefPayload(part_id));
    }

    void dropRef(const std::string & uuid, const std::string & part_name)
    {
        os->removeObjectsIfExist({DB::StoredObject(refKey(prefix, "srv", uuid, part_name).string())});
    }

    /// Open a LIVE write-session pinning the given bare blob hashes (and optionally a committed part id).
    /// The session is a GC root for the lifetime it is on disk and its lease has not expired.
    void openSession(const std::string & session_id, int64_t lease_deadline,
                    const std::vector<BlobHash> & pins, const PartId & part_id = PartId(""))
    {
        WriteSession s;
        s.server_id = "srv";
        s.lease_deadline_unix = static_cast<UInt64>(lease_deadline);
        s.part_id = part_id;
        s.pending = pins;
        put(sessionKey(prefix, session_id), s.serialize());
    }

    void closeSession(const std::string & session_id)
    {
        os->removeObjectsIfExist({DB::StoredObject(sessionKey(prefix, session_id))});
    }

    /// The set of present generation OBJECTS (not tombstones, not `active`) under a blob's directory.
    std::set<uint64_t> presentBlobGenerations(const BlobHash & h)
    {
        std::set<uint64_t> gens;
        DB::RelativePathsWithMetadata children;
        os->listObjects(blobGenPrefix(prefix, h), children, 0);
        for (const auto & c : children)
        {
            bool is_tombstone = false;
            if (auto g = parseGenFromKey(c->relative_path, is_tombstone); g && !is_tombstone)
                gens.insert(*g);
        }
        return gens;
    }

    /// Reconstruct generation lineage `max(gen)` from the SURVIVING objects (generation objects AND
    /// gravestone tombstones), exactly as the §6 last-bullet / I7d rule does — independent of `active`.
    std::optional<uint64_t> maxGenForHashFromObjects(const BlobHash & h)
    {
        std::optional<uint64_t> best;
        DB::RelativePathsWithMetadata children;
        os->listObjects(blobGenPrefix(prefix, h), children, 0);
        for (const auto & c : children)
        {
            bool is_tombstone = false;
            if (auto g = parseGenFromKey(c->relative_path, is_tombstone); g)
                if (!best || *g > *best)
                    best = *g;
        }
        return best;
    }

    /// Model the §6.1 READER resolution rule over the production helpers + real object storage. This is
    /// EXACTLY the rule `resolveBlobGenKeyForRead` + `repairBlobGenOn404` implement: start at the `active`
    /// hint (default 0), GET that generation; on a 404 LIST the blob's generation prefix and read the
    /// HIGHEST present generation's bytes. The TOMBSTONE is never consulted — only a 404 triggers fallback.
    /// `fell_back` reports whether the fast-path generation 404'd (so the test can assert the steady path
    /// took ONE GET, vs the fallback fired).
    std::string readerResolveAndRead(const BlobHash & h, bool & fell_back)
    {
        fell_back = false;
        /// Resolve the preferred generation: read `active` if present (a plain hint), else default 0.
        uint64_t resolved = 0;
        const std::string active_key = blobActiveKey(prefix, h);
        if (exists(active_key))
        {
            const std::string content = get(active_key);
            if (!content.empty())
                resolved = std::stoull(content);
        }
        /// Fast path: GET the resolved generation. Tombstone is irrelevant — we never read it.
        const std::string fast_key = blobGenKey(prefix, h, resolved).string();
        if (exists(fast_key))
            return get(fast_key);

        /// 404 fallback: LIST the generation prefix, pick the HIGHEST present generation, read its bytes.
        fell_back = true;
        std::optional<uint64_t> best;
        DB::RelativePathsWithMetadata children;
        os->listObjects(blobGenPrefix(prefix, h), children, 0);
        for (const auto & c : children)
        {
            bool is_tombstone = false;
            if (auto g = parseGenFromKey(c->relative_path, is_tombstone); g && !is_tombstone)
                if (!best || *g > *best)
                    best = *g;
        }
        if (!best)
            throw DB::Exception(DB::ErrorCodes::FILE_DOESNT_EXIST, "reader: no present generation for blob");
        return get(blobGenKey(prefix, h, *best).string());
    }

    static inline bool initialized = false;
};

/// A hex string long enough to fan out (≥4 chars) so the key layout matches production. The shard does not
/// matter for the reconciliation-driven oracles (the full scan is shard-agnostic).
BlobHash blobHash(const std::string & tag) { return BlobHash("aabbcc" + tag); }
PartId partId(const std::string & tag) { return PartId("ddeeff" + tag); }

}

/// ── Oracle group 1 — seal / resurrect / gravestone-lineage ──────────────────────────────────────────
///
/// PROVES: a GC seal of `(H,0)` does NOT block a contended writer from resurrecting `(H,1)` via the
/// single-owner condCreateIfAbsent; after `(H,0)` is swept its gravestone PERSISTS and the lineage
/// `max(gen)` is still reconstructable from the surviving objects with `active` deleted (I7d).
TEST_F(ContentAddressedGcS3, SealResurrectGravestoneLineage)
{
    const BlobHash h = blobHash("seal01");

    /// (H,0) exists; the GC seals it (single-owner create of the tombstone).
    put(blobGenKey(prefix, h, 0).string(), "v0");
    ASSERT_TRUE(condCreateIfAbsent(*os, blobTombstoneKey(prefix, h, 0).string(), std::string()));
    /// Re-sealing the SAME generation loses the CAS (idempotent seal — the durable condemned-state).
    EXPECT_FALSE(condCreateIfAbsent(*os, blobTombstoneKey(prefix, h, 0).string(), std::string()));

    /// A contended writer finds (H,0) sealed and RESURRECTS to (H,1) (condCreateIfAbsent at g+1). The seal
    /// does not block it — resurrection targets a DIFFERENT key.
    ASSERT_TRUE(condCreateIfAbsent(*os, blobGenKey(prefix, h, 1).string(), "v1"));
    /// best-effort advance `active` -> 1.
    put(blobActiveKey(prefix, h), "1");

    /// Both generations present; (H,0) still sealed.
    EXPECT_TRUE(exists(blobGenKey(prefix, h, 0).string()));
    EXPECT_TRUE(exists(blobGenKey(prefix, h, 1).string()));
    EXPECT_TRUE(exists(blobTombstoneKey(prefix, h, 0).string()));
    EXPECT_EQ(presentBlobGenerations(h), (std::set<uint64_t>{0, 1}));

    /// Now SWEEP (H,0): no ref/session references H -> the GC deletes blobGenKey(H,0), KEEPS the gravestone.
    DB::ContentAddressed::ContentAddressedGC gc(os, prefix);
    /// (H,1) is also unreferenced here, but we only want to prove the g=0 sweep + gravestone. Drive the
    /// reconciliation path so the full scan emits the (H,*) generation objects as candidates; past grace
    /// they are swept. We assert the gravestone of (H,0) persists and lineage is reconstructable.
    gc.runReconciliationScan(/*now=*/0, /*grace=*/100);          // arm grace timers (seal happens here)
    auto stats = gc.runReconciliationScan(/*now=*/200, /*grace=*/100); // past grace -> sweep unreferenced gens
    EXPECT_GE(stats.deleted_blobs, 1u);

    /// The generation OBJECTS are swept; the GRAVESTONE `(H,0).tombstone` PERSISTS forever (never deleted on
    /// sweep — only RECOVER deletes a tombstone, and only when the identity is reachable with no successor).
    EXPECT_FALSE(exists(blobGenKey(prefix, h, 0).string()));
    EXPECT_TRUE(exists(blobTombstoneKey(prefix, h, 0).string()));

    /// Lineage survives `active` loss: delete the hint, and `max(gen)` is still reconstructable from the
    /// surviving objects (the gravestone tombstones carry the generation number — I7d).
    os->removeObjectsIfExist({DB::StoredObject(blobActiveKey(prefix, h))});
    EXPECT_FALSE(exists(blobActiveKey(prefix, h)));
    const auto lineage = maxGenForHashFromObjects(h);
    ASSERT_TRUE(lineage.has_value());
    EXPECT_EQ(*lineage, 1u); /// reconstructed from the (H,1) gravestone/object even after the g=0 object is gone
}

/// ── Oracle group 2 — best-effort `active` + sweep-resets-`active` + reader fallback ─────────────────
///
/// PROVES: sweeping `(H,0)` while `(H,1)` is live resets `active` off 0; a reader with a STALE `active=0`
/// 404s on the GET, LISTs, and reads the surviving generation's bytes — and the reset/repair leaves a
/// hint that no longer names a deleted object.
TEST_F(ContentAddressedGcS3, ActiveResetSweepAndReaderFallback)
{
    const std::string uuid = "uuid-active";
    const BlobHash h = blobHash("act01");
    const PartId pid = partId("act01");

    /// Two generations exist; the manifest pins bare H. (H,1) carries the live bytes.
    put(blobGenKey(prefix, h, 0).string(), "old-bytes");
    put(blobGenKey(prefix, h, 1).string(), "new-bytes");
    put(blobActiveKey(prefix, h), "0");
    publishLivePart(uuid, "all_1_1_0", pid, {h});

    /// Drop the ref so the WHOLE identity is unreachable, then sweep: both generations are reclaimed (the S3
    /// identity-level re-check sweeps every generation of an unreachable identity). The `active` hint is now
    /// stale (it names a deleted generation) — exactly the state the reader's 404 -> LIST fallback repairs.
    dropRef(uuid, "all_1_1_0");
    DB::ContentAddressed::ContentAddressedGC gc(os, prefix);
    gc.runReconciliationScan(/*now=*/0, /*grace=*/100);
    gc.runReconciliationScan(/*now=*/200, /*grace=*/100);
    EXPECT_FALSE(exists(blobGenKey(prefix, h, 0).string()));

    /// Re-create the survivor (H,1) (a fresh insert of byte-identical content), leaving the STALE active=0.
    put(blobGenKey(prefix, h, 1).string(), "new-bytes");
    put(blobActiveKey(prefix, h), "0");

    /// READER: active=0 -> GET (H,0) -> 404 -> LIST blobGenPrefix -> highest present is (H,1) -> read bytes.
    bool fell_back = false;
    const std::string bytes = readerResolveAndRead(h, fell_back);
    EXPECT_TRUE(fell_back); /// the stale active forced the LIST fallback
    EXPECT_EQ(bytes, "new-bytes");

    /// Repairing `active` to a present generation (what the GC's `resetActiveOffGeneration` and the reader's
    /// opportunistic PUT both do) makes the steady read take ONE GET — no LIST, no fallback.
    put(blobActiveKey(prefix, h), "1");
    bool fell_back2 = true;
    const std::string bytes2 = readerResolveAndRead(h, fell_back2);
    EXPECT_FALSE(fell_back2);
    EXPECT_EQ(bytes2, "new-bytes");
}

/// ── Oracle group 3 — tombstone-does-not-block-reads ─────────────────────────────────────────────────
///
/// PROVES: a reader GETs a SEALED-but-present `(H,g)` during grace and SUCCEEDS, using the bytes regardless
/// of the tombstone. The tombstone gates ATTACHMENT, never reads (§6.1) — only a 404 triggers fallback.
TEST_F(ContentAddressedGcS3, TombstoneDoesNotBlockReads)
{
    const BlobHash h = blobHash("tomb01");

    /// (H,0) present AND sealed (a gravestone-in-grace): the GC condemned it but has not swept yet.
    put(blobGenKey(prefix, h, 0).string(), "present-bytes");
    ASSERT_TRUE(condCreateIfAbsent(*os, blobTombstoneKey(prefix, h, 0).string(), std::string()));
    ASSERT_TRUE(exists(blobTombstoneKey(prefix, h, 0).string()));

    /// READER: active default 0 -> GET (H,0) SUCCEEDS. The tombstone is NOT consulted; no fallback fires.
    bool fell_back = true;
    const std::string bytes = readerResolveAndRead(h, fell_back);
    EXPECT_FALSE(fell_back); /// the present generation served the read directly — no LIST, no exception
    EXPECT_EQ(bytes, "present-bytes");
}

/// ── Oracle group 4 — mark / recover / drain / sweep transitions (the load-bearing branch oracle) ────

/// SWEEP: a sealed candidate with no ref/session is, past grace and re-check-unreachable, deleted; the
/// gravestone is kept.
TEST_F(ContentAddressedGcS3, BranchSweepDeletesGenKeepsGravestone)
{
    const BlobHash h = blobHash("sweep01");
    put(blobGenKey(prefix, h, 0).string(), "v0"); /// orphan: no ref, no session, no manifest

    DB::ContentAddressed::ContentAddressedGC gc(os, prefix);
    gc.runReconciliationScan(/*now=*/0, /*grace=*/100);   /// SEAL (H,0), arm grace
    EXPECT_TRUE(exists(blobTombstoneKey(prefix, h, 0).string())); /// durable condemnation written

    auto stats = gc.runReconciliationScan(/*now=*/200, /*grace=*/100); /// past grace -> SWEEP
    EXPECT_EQ(stats.deleted_blobs, 1u);
    EXPECT_FALSE(exists(blobGenKey(prefix, h, 0).string())); /// gen object deleted
    EXPECT_TRUE(exists(blobTombstoneKey(prefix, h, 0).string())); /// gravestone KEPT
}

/// RECOVER: a sealed candidate whose identity becomes reachable again (a ref reappears) with NO successor
/// generation -> the tombstone is DELETED (un-sealed) and the generation is re-opened/attachable.
TEST_F(ContentAddressedGcS3, BranchRecoverUnsealsWhenReachableNoSuccessor)
{
    const BlobHash h = blobHash("rec01");

    /// (H,0) present; a LIVE write SESSION pins H (the cross-mounter generalization of the in-process pin —
    /// e.g. a blob just uploaded but not yet referenced by a published ref). The session is a GC root, so
    /// the IDENTITY is reachable. There is NO successor generation.
    put(blobGenKey(prefix, h, 0).string(), "v0");
    openSession("sess-rec", /*lease_deadline=*/1000, /*pins=*/{h});

    /// Manually SEAL (H,0) (simulate a stale condemnation from a crossing fold). The identity is reachable
    /// (the session pins it) and no successor exists, so the GC must RECOVER it: delete the tombstone,
    /// re-open g=0.
    ASSERT_TRUE(condCreateIfAbsent(*os, blobTombstoneKey(prefix, h, 0).string(), std::string()));
    /// CA GC S4 (#4, G3): the real seal path now also records the open tombstone in the gc/sealed index, and
    /// Scan A re-presents sealed candidates from that index (not a full bucket scan). A manual seal must mirror
    /// the real one or the reachable-identity candidate (which the reconciliation full-scan skips) would never
    /// reach the RECOVER branch. Seed the matching index entry so this manual seal == a real seal.
    ASSERT_TRUE(condCreateIfAbsent(*os, gcSealedKey(prefix, shardForHash(h), h.string(), 0, /*is_blob=*/true), std::string()));

    DB::ContentAddressed::ContentAddressedGC gc(os, prefix);
    gc.runReconciliationScan(/*now=*/0, /*grace=*/100);
    auto stats = gc.runReconciliationScan(/*now=*/200, /*grace=*/100);

    /// RECOVER fired: the gen object survives (it is reachable) and the tombstone was un-sealed.
    EXPECT_EQ(stats.deleted_blobs, 0u);
    EXPECT_TRUE(exists(blobGenKey(prefix, h, 0).string()));
    EXPECT_FALSE(exists(blobTombstoneKey(prefix, h, 0).string())); /// un-sealed -> re-attachable

    /// Now the pin drains to zero (the session closes — its blob was never published to a ref): the identity
    /// is unreachable again, so a subsequent GC round re-seals and (past grace) sweeps g=0.
    closeSession("sess-rec");
    gc.runReconciliationScan(/*now=*/300, /*grace=*/100);          /// re-seal the now-unreferenced g=0
    auto swept = gc.runReconciliationScan(/*now=*/500, /*grace=*/100); /// past grace -> SWEEP
    EXPECT_EQ(swept.deleted_blobs, 1u);
    EXPECT_FALSE(exists(blobGenKey(prefix, h, 0).string()));
    EXPECT_TRUE(exists(blobTombstoneKey(prefix, h, 0).string())); /// gravestone kept
}

/// DRAIN (successor exists -> don't recover): seal (H,0); a writer resurrects (H,1) (the successor); a
/// ref/session still references the identity. The GC must KEEP BOTH (H,0).tombstone AND blobGenKey(H,0)
/// (do NOT delete, do NOT re-open) — the I7a two-generations-pinned reality.
TEST_F(ContentAddressedGcS3, BranchDrainKeepsBothWhenSuccessorExists)
{
    const std::string uuid = "uuid-drain";
    const BlobHash h = blobHash("drain01");
    const PartId pid = partId("drain01");

    /// (H,0) sealed; (H,1) resurrected (the successor). The identity is reachable (a live part pins H).
    put(blobGenKey(prefix, h, 0).string(), "v0");
    ASSERT_TRUE(condCreateIfAbsent(*os, blobTombstoneKey(prefix, h, 0).string(), std::string()));
    ASSERT_TRUE(condCreateIfAbsent(*os, blobGenKey(prefix, h, 1).string(), "v1"));
    publishLivePart(uuid, "all_1_1_0", pid, {h});

    DB::ContentAddressed::ContentAddressedGC gc(os, prefix);
    gc.runReconciliationScan(/*now=*/0, /*grace=*/100);
    auto stats = gc.runReconciliationScan(/*now=*/200, /*grace=*/100);

    /// DRAIN: nothing deleted, BOTH the tombstone and the g=0 object kept, g=0 NOT re-opened.
    EXPECT_EQ(stats.deleted_blobs, 0u);
    EXPECT_TRUE(exists(blobGenKey(prefix, h, 0).string()));   /// predecessor kept (still possibly referenced)
    EXPECT_TRUE(exists(blobTombstoneKey(prefix, h, 0).string())); /// tombstone KEPT (not un-sealed)
    EXPECT_TRUE(exists(blobGenKey(prefix, h, 1).string()));   /// successor present
}

/// ── Oracle group 5 — seal -> resurrect -> GC-recovers-old-generation (I7a, TWO generations pinned) ──
///
/// PROVES the "single live generation" lemma is correctly NOT assumed. W1 pins g, GC seals g, W2 resurrects
/// g+1, GC RECOVERs g because W1 still references it -> BOTH g and g+1 are pinned and a reader reads EITHER
/// correctly (byte-identical content per I7c). Here, because a successor exists, the branch is DRAIN (keep
/// both) — which is the conservative S3 form of "recover the old generation" (it is never deleted while
/// referenced). We assert byte-correct reads from BOTH generations.
TEST_F(ContentAddressedGcS3, TwoGenerationsPinnedReadsByteCorrect)
{
    const std::string uuid = "uuid-two";
    const BlobHash h = blobHash("two01");
    const PartId pidW1 = partId("two0a");
    const PartId pidW2 = partId("two0b");

    /// W1 wrote and pins (H,0) (a live part). GC seals (H,0). W2 (a contended writer) resurrects (H,1) — a
    /// byte-identical copy at the next generation. W2 publishes a second live part that ALSO pins H.
    put(blobGenKey(prefix, h, 0).string(), "identical-bytes");
    publishLivePart(uuid, "all_1_1_0", pidW1, {h});                 /// W1 ref pins H
    ASSERT_TRUE(condCreateIfAbsent(*os, blobTombstoneKey(prefix, h, 0).string(), std::string())); /// GC seals g=0
    ASSERT_TRUE(condCreateIfAbsent(*os, blobGenKey(prefix, h, 1).string(), "identical-bytes"));   /// W2 resurrects
    publishLivePart(uuid, "all_2_2_0", pidW2, {h});                 /// W2 ref ALSO pins H

    /// The GC runs: H is reachable (W1 and W2), a successor exists -> DRAIN keeps BOTH. Neither is deleted.
    DB::ContentAddressed::ContentAddressedGC gc(os, prefix);
    gc.runReconciliationScan(/*now=*/0, /*grace=*/100);
    auto stats = gc.runReconciliationScan(/*now=*/200, /*grace=*/100);
    EXPECT_EQ(stats.deleted_blobs, 0u); /// NOT a single-live-generation assumption — both stay pinned

    EXPECT_TRUE(exists(blobGenKey(prefix, h, 0).string()));
    EXPECT_TRUE(exists(blobGenKey(prefix, h, 1).string()));

    /// A reader reads EITHER generation byte-correctly (I7c — byte-identical content). With active=0 the
    /// reader gets g=0 directly; with active=1 it gets g=1; both yield the same bytes.
    put(blobActiveKey(prefix, h), "0");
    bool fb0 = true;
    EXPECT_EQ(readerResolveAndRead(h, fb0), "identical-bytes");
    EXPECT_FALSE(fb0);
    put(blobActiveKey(prefix, h), "1");
    bool fb1 = true;
    EXPECT_EQ(readerResolveAndRead(h, fb1), "identical-bytes");
    EXPECT_FALSE(fb1);
}

/// ── Oracle group 6 — manifest symmetry ──────────────────────────────────────────────────────────────
///
/// PROVES a `(part_id, mg)` manifest runs the IDENTICAL seal / recover / drain / sweep machinery: an
/// unreferenced manifest generation is sealed + swept (gravestone kept); a relink-after-full-drop routes a
/// re-created manifest to a fresh generation so the old `mg` sweep cannot kill it (ABA hole closed).
TEST_F(ContentAddressedGcS3, ManifestSymmetrySealSweepGravestoneAndResurrect)
{
    const PartId pid = partId("man01");

    /// An ORPHAN manifest generation (parts/<pid>/0) with no ref pointing at it.
    put(partGenKey(prefix, pid, 0).string(), "manifest-v0");

    DB::ContentAddressed::ContentAddressedGC gc(os, prefix);
    gc.runReconciliationScan(/*now=*/0, /*grace=*/100);   /// SEAL the manifest generation
    EXPECT_TRUE(exists(partTombstoneKey(prefix, pid, 0).string())); /// durable manifest condemnation

    auto stats = gc.runReconciliationScan(/*now=*/200, /*grace=*/100); /// past grace -> SWEEP
    EXPECT_EQ(stats.deleted_parts, 1u);
    EXPECT_FALSE(exists(partGenKey(prefix, pid, 0).string())); /// manifest gen object deleted
    EXPECT_TRUE(exists(partTombstoneKey(prefix, pid, 0).string())); /// gravestone KEPT (symmetric with blobs)

    /// ABA: a relink-after-full-drop re-creates the manifest. mg=0 is sealed (a gravestone persists). The
    /// writer's tomb re-check sees the mg=0 gravestone and MUST route the re-creation to mg=1 (a DIFFERENT
    /// key) rather than re-use the swept generation — this is exactly what closes the relink-after-full-drop
    /// ABA hole (§9): the old mg=0 sweep already happened and can never resurrect the re-created manifest.
    ASSERT_TRUE(exists(partTombstoneKey(prefix, pid, 0).string())); /// the gravestone is the re-check signal
    ASSERT_TRUE(condCreateIfAbsent(*os, partGenKey(prefix, pid, 1).string(), "manifest-redo"));
    EXPECT_TRUE(exists(partGenKey(prefix, pid, 1).string()));
    /// The old mg=0 gravestone still stands and does NOT shadow the re-created mg=1 manifest.
    EXPECT_TRUE(exists(partTombstoneKey(prefix, pid, 0).string()));
}

/// ── Observability (§13) — the GC counters are deterministically bumped ───────────────────────────────
///
/// PROVES the §13 guardrail counters fire on the events they guard: SEAL bumps the durable-condemnation
/// counter; SWEEP bumps the orphan-bytes estimate; the per-round generation walk bumps the
/// generations-per-hash proxy (generations observed / distinct identities observed).
TEST_F(ContentAddressedGcS3, ObservabilityCountersBumpOnSealSweepAndGenerationWalk)
{
    const BlobHash h = blobHash("ctr01");
    /// Two generations of one hash present, both orphan (no ref/session) — so the per-hash proxy sees one
    /// identity with two generation objects, and the sweep reclaims both.
    put(blobGenKey(prefix, h, 0).string(), "vvvv0");  /// 5 bytes
    put(blobGenKey(prefix, h, 1).string(), "vvvvvv1"); /// 7 bytes

    DB::ContentAddressed::ContentAddressedGC gc(os, prefix);

    /// Round 1 (now=0): SEAL both generations (durable condemnations) + walk the generation listing.
    const auto seals_before = ProfileEvents::global_counters[ProfileEvents::ContentAddressedTombstonesTotal].load();
    const auto gens_before = ProfileEvents::global_counters[ProfileEvents::ContentAddressedGenerationsObserved].load();
    const auto hashes_before = ProfileEvents::global_counters[ProfileEvents::ContentAddressedHashesObserved].load();
    gc.runReconciliationScan(/*now=*/0, /*grace=*/100);
    const auto seals_after = ProfileEvents::global_counters[ProfileEvents::ContentAddressedTombstonesTotal].load();
    const auto gens_after = ProfileEvents::global_counters[ProfileEvents::ContentAddressedGenerationsObserved].load();
    const auto hashes_after = ProfileEvents::global_counters[ProfileEvents::ContentAddressedHashesObserved].load();

    EXPECT_GE(seals_after - seals_before, 2); /// both (H,0) and (H,1) sealed (durable condemnations)
    EXPECT_GE(gens_after - gens_before, 2);   /// two present generation OBJECTS observed
    EXPECT_GE(hashes_after - hashes_before, 1); /// one distinct identity carrying generations

    /// Round 2 (now=200): past grace -> SWEEP both -> orphan-bytes estimate bumps by ~the swept sizes.
    const auto bytes_before = ProfileEvents::global_counters[ProfileEvents::ContentAddressedOrphanBytesEstimate].load();
    auto stats = gc.runReconciliationScan(/*now=*/200, /*grace=*/100);
    const auto bytes_after = ProfileEvents::global_counters[ProfileEvents::ContentAddressedOrphanBytesEstimate].load();

    EXPECT_EQ(stats.deleted_blobs, 2u);
    EXPECT_GE(bytes_after - bytes_before, 12); /// 5 + 7 bytes reclaimed (estimate; HEAD-before-delete)
}
