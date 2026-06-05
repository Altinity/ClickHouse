#include <gtest/gtest.h>

#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/ContentAddressedGC.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/RefPayload.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/GcCompaction.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/GcDelta.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/GcLayout.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/GcLogWriter.h>
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

/// CA GC S4 — the LOCKLESS-HANDSHAKE safety PROOF (spec §7/§7.1, §5.1, §11). The in-process `gc_lock` that
/// used to serialize a transaction commit against the whole GC sweep is GONE (G1); the SOLE cross-process
/// commit-vs-sweep gate is now the durable two-flag §7 handshake:
///   - the writer raises its SESSION pin (the durable, GC-visible flag `A`) BEFORE its tomb re-check and the
///     `+`, and keeps it until folded;
///   - the GC seals `.tombstone` (flag `M`) BEFORE its FRESH authoritative §6.2 re-check, which re-reads the
///     LIVE session set (a current `LIST sessions/`) + refs + manifests.
/// The interleaving chain end(raise A) ≤ start(writer-recheck) < end(seal M) ≤ start(L reads A) < end(raise A)
/// is unsatisfiable, so "writer commits to (H,g)" and "GC deletes (H,g)" cannot BOTH hold.
///
/// Every oracle is a DETERMINISTIC interleaving driven BY HAND (NO sleeps): we drive the writer's durable
/// steps (raise the session object, publish the ref) and the GC's durable steps (seal a `.tombstone`, run the
/// real `ContentAddressedGC` seal -> grace -> fresh §6.2 re-check -> {sweep|recover|drain} tail) in the
/// chosen racy order over a REAL `LocalObjectStorage`, and assert the safety property. The GC re-check reads
/// the session set from the bucket (`sessionPinnedBlobs`), so a live session written before the re-check is
/// observed exactly as it would be by a concurrent production sweep — this exercises the EXACT race the lock
/// used to prevent.

namespace
{

class ContentAddressedGcS4 : public testing::Test
{
public:
    void SetUp() override
    {
        if (!initialized)
        {
            DB::ServerUUID::setRandomForUnitTests();
            initialized = true;
        }
        prefix = std::string("cas_gc_s4_pool_") + ::testing::UnitTest::GetInstance()->current_test_info()->name();
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

    std::string prefix;
    std::shared_ptr<DB::LocalObjectStorage> os;

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

    bool exists(const std::string & key) const { return os->tryGetObjectMetadata(key, /*with_tags=*/false).has_value(); }

    /// STORE flag M: the GC seal — a durable single-owner `condCreateIfAbsent(<g>.tombstone)`. The real seal
    /// site (CA GC S4 #4, G3) ALSO records the open tombstone in the compact per-shard gc/sealed index, and
    /// Scan A now re-presents sealed candidates from that index instead of LISTing the whole tree. A faithful
    /// manual seal must write BOTH so the candidate is re-discovered exactly as a real condemnation would be.
    bool seal(const BlobHash & h, uint64_t g)
    {
        const bool created = condCreateIfAbsent(*os, blobTombstoneKey(prefix, h, g).string(), std::string());
        condCreateIfAbsent(*os, gcSealedKey(prefix, shardForHash(h), h.string(), g, /*is_blob=*/true), std::string());
        return created;
    }

    /// STORE flag A: the writer raises a durable, GC-visible session pinning bare `H` (identity-level — the
    /// §7.1 conservative pin). `committed` distinguishes a still-uploading pin from a committed-until-folded
    /// one. `delta_epochs` records the `+`'s settled (shard, epoch) so the reaper can check foldedness.
    void raiseSession(const std::string & sid, bool committed, const std::vector<BlobHash> & pins,
                      const std::vector<std::pair<ShardId, uint64_t>> & delta_epochs = {}, int64_t lease_deadline = 1'000'000)
    {
        WriteSession s;
        s.server_id = "srv";
        s.lease_deadline_unix = static_cast<UInt64>(lease_deadline);
        s.committed = committed;
        s.pending = pins;
        s.delta_epochs = delta_epochs;
        put(sessionKey(prefix, sid), s.serialize());
    }

    /// Publish a manifest (pins bare H) + ref so the part_id and the blobs it names become a LIVE GC root.
    void publishLivePart(const std::string & uuid, const std::string & part_name, const PartId & pid, const std::vector<BlobHash> & hashes)
    {
        PartManifest m;
        for (size_t i = 0; i < hashes.size(); ++i)
            m.blobs["f" + std::to_string(i) + ".bin"] = BlobEntry{hashes[i], 1, hashes[i].string()};
        put(partKey(prefix, pid).string(), m.serialize());
        put(refKey(prefix, "srv", uuid, part_name).string(), serializeRefPayload(pid));
    }

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

    size_t countSessions(std::optional<bool> committed_filter = std::nullopt)
    {
        size_t n = 0;
        for (const auto & sk : listKeysUnder(os, sessionsPrefix(prefix)))
        {
            const WriteSession s = WriteSession::deserialize(get(sk));
            if (!committed_filter || s.committed == *committed_filter)
                ++n;
        }
        return n;
    }

    BlobHash blobHash(const std::string & tag) { return BlobHash("aabbcc" + tag); }
    PartId partId(const std::string & tag) { return PartId("ddeeff" + tag); }

    static inline bool initialized = false;
};

}

/// ── Oracle 1a — the LOAD-BEARING §7 oracle: writer publishes (raises the session) BEFORE the GC's recheck.
///
/// Interleaving: the writer raises the session for (H,0) and uploads the blob; THEN the GC seals (H,0) and
/// runs its fresh re-check. The re-check reads the LIVE session set (flag A), sees the pin, and does NOT
/// delete — it RECOVERs (un-seals) the still-pinned generation. (H,0) survives and the writer commits.
TEST_F(ContentAddressedGcS4, Sec7_WriterPublishesBeforeGcRecheck_BlobSurvives)
{
    const BlobHash h = blobHash("a01");

    /// Writer: raise the session (flag A) FIRST, then upload the blob (the §7.1 order: session before upload).
    raiseSession("writer-a01", /*committed=*/false, /*pins=*/{h});
    put(blobGenKey(prefix, h, 0).string(), "PAYLOAD-A01");

    /// GC: seal (H,0) (flag M) then run the fresh §6.2 re-check + delete tail. The re-check LISTs sessions and
    /// finds the live pin -> (H,0) is identity-reachable -> the GC RECOVERs (un-seals) it, never sweeps it.
    DB::ContentAddressed::ContentAddressedGC gc(os, prefix);
    gc.setReconciliationCadenceRounds(1);
    gc.runSweepOnce(/*now=*/0, /*grace=*/0);   // seal + arm grace; re-check sees the session -> recover
    gc.runSweepOnce(/*now=*/100, /*grace=*/0); // even past grace, the live session keeps it reachable

    EXPECT_TRUE(exists(blobGenKey(prefix, h, 0).string())) << "the session raised before the GC recheck must save (H,0)";
    EXPECT_FALSE(exists(blobTombstoneKey(prefix, h, 0).string())) << "RECOVER un-seals the still-pinned generation";

    /// The writer now commits: publish the manifest+ref to (H). The committed ref reads (H,0) byte-correct.
    const PartId pid = partId("a01");
    publishLivePart("uuid-a01", "all_1_1_0", pid, {h});
    EXPECT_EQ(get(blobGenKey(prefix, h, 0).string()), "PAYLOAD-A01");
}

/// ── Oracle 1b — the dual: the GC's refcheck runs BEFORE the writer raises the session.
///
/// Interleaving: the GC seals (H,0) and re-checks WHILE no session and no ref exist -> it SWEEPS (H,0)
/// (gravestone kept). THEN the (paused) writer resumes, re-checks the tomb, sees the seal, and RESURRECTS to
/// (H,1). The committed ref reads (H,1) — never a dangling ref. NO interleaving leaves a committed ref to
/// content whose every generation was deleted.
TEST_F(ContentAddressedGcS4, Sec7_GcRecheckBeforeWriterRaisesSession_WriterResurrects)
{
    const BlobHash h = blobHash("b01");

    /// A stale (H,0) object exists (e.g. an abandoned upload) with NO session and NO ref naming it.
    put(blobGenKey(prefix, h, 0).string(), "STALE-B01");

    /// GC: seal + re-check (no session, no ref) -> SWEEP (H,0); the gravestone (H,0).tombstone persists.
    /// With grace=0 the seal and the sweep collapse into the SAME round (the seal arms grace at `since=now`
    /// and `now - since >= 0` immediately gates the delete), so the actual deletion is counted in round 1.
    /// Round 2 is the idempotent "stays swept" check: now that a swept generation's gc/sealed index entry is
    /// removed on sweep (CA GC S4 #4), the gravestone is NOT re-presented forever — round 2 deletes nothing.
    /// Sum the rounds: the orphan is swept EXACTLY once, total deleted_blobs >= 1.
    DB::ContentAddressed::ContentAddressedGC gc(os, prefix);
    gc.setReconciliationCadenceRounds(1);
    auto stats = gc.runSweepOnce(/*now=*/0, /*grace=*/0);    // seal + (grace=0) sweep
    stats.deleted_blobs += gc.runSweepOnce(/*now=*/100, /*grace=*/0).deleted_blobs; // stays swept (no re-present)
    EXPECT_GE(stats.deleted_blobs, 1u);
    EXPECT_FALSE(exists(blobGenKey(prefix, h, 0).string()));
    EXPECT_TRUE(exists(blobTombstoneKey(prefix, h, 0).string())) << "the gravestone persists (ABA-proof: reuse routes to g+1)";

    /// Writer resumes: its §7.1 tomb re-check sees (H,0) sealed and RESURRECTS to (H,1) (a DIFFERENT physical
    /// key — condCreateIfAbsent at g+1). It raises its session over the bare H (now backed by (H,1)) and
    /// publishes the ref. The committed ref reads (H,1): NO dangling ref to a deleted generation.
    ASSERT_TRUE(condCreateIfAbsent(*os, blobGenKey(prefix, h, 1).string(), "PAYLOAD-B01"));
    put(blobActiveKey(prefix, h), "1");
    raiseSession("writer-b01", /*committed=*/true, /*pins=*/{h});
    const PartId pid = partId("b01");
    publishLivePart("uuid-b01", "all_1_1_0", pid, {h});

    /// SAFETY ASSERTION: at least one generation of H is present (the committed ref resolves), and it is the
    /// resurrected (H,1). A re-check sweep now leaves (H,1) alone (it is referenced).
    EXPECT_EQ(presentBlobGenerations(h), (std::set<uint64_t>{1}));
    EXPECT_EQ(get(blobGenKey(prefix, h, 1).string()), "PAYLOAD-B01");
    gc.runSweepOnce(/*now=*/200, /*grace=*/0);
    EXPECT_TRUE(exists(blobGenKey(prefix, h, 1).string())) << "the referenced resurrected generation is never swept";
}

/// ── Oracle 1c — the SEAL-WINS-RACE corner: the seal lands AFTER the writer's recheck but the writer's
/// session was raised FIRST. This is the precise unsatisfiable-chain corner: A (session) is up before the
/// recheck; the GC seals; the GC's later refcheck reads A and SKIPS. The blob survives AND the writer commits
/// — both cannot fail.
TEST_F(ContentAddressedGcS4, Sec7_SealAfterWriterRecheckButSessionFirst_NoDanglingRef)
{
    const BlobHash h = blobHash("c01");

    /// (1) Writer raises the session (flag A). (2) Writer uploads (H,0). (3) Writer re-checks the tomb: NONE
    /// present, so it does NOT resurrect — it intends to commit (H,0).
    raiseSession("writer-c01", /*committed=*/false, /*pins=*/{h});
    put(blobGenKey(prefix, h, 0).string(), "PAYLOAD-C01");
    EXPECT_FALSE(exists(blobTombstoneKey(prefix, h, 0).string())) << "writer recheck saw NO tomb";

    /// (4) NOW the GC seals (H,0) — AFTER the writer's recheck. Per the chain, the GC's subsequent refcheck
    /// MUST read flag A (the session raised in step 1) and skip the delete.
    ASSERT_TRUE(seal(h, 0));
    DB::ContentAddressed::ContentAddressedGC gc(os, prefix);
    gc.setReconciliationCadenceRounds(1);
    /// The candidate (H,0) is already sealed; the GC re-presents it and runs the re-check. The live session
    /// makes it identity-reachable -> RECOVER (un-seal), NOT sweep.
    gc.runSweepOnce(/*now=*/0, /*grace=*/0);
    gc.runSweepOnce(/*now=*/100, /*grace=*/0);

    EXPECT_TRUE(exists(blobGenKey(prefix, h, 0).string())) << "the session-before-recheck pin saves (H,0) from the later seal";
    EXPECT_FALSE(exists(blobTombstoneKey(prefix, h, 0).string())) << "RECOVER un-sealed it (the writer commits to (H,0))";

    /// (5) Writer commits to (H,0): publish ref; reads byte-correct.
    const PartId pid = partId("c01");
    publishLivePart("uuid-c01", "all_1_1_0", pid, {h});
    EXPECT_EQ(get(blobGenKey(prefix, h, 0).string()), "PAYLOAD-C01");
}

/// ── Oracle 2 — §5.1 append-as-epoch-folds, NOW LOCK-FREE. A writer's `+` lands as its epoch is closed; the
/// writer re-appends into the open epoch (rule 2), and folded-snapshot ∪ open-epoch covers the reference, so
/// the blob is NEVER a count-0 candidate while the part is live. This is the SAME machinery S2 exercised
/// under the lock — it must hold with the lock GONE (the §5.1 close-before-fold is the barrier, not the
/// mutex). Driven directly over `GcLogWriter` / `GcCompaction` (whose own `mtx` never was `gc_lock`).
TEST_F(ContentAddressedGcS4, Sec5_1_AppendAsEpochFolds_ReappendNoUndercount_LockFree)
{
    GcLogWriter writer(os, prefix);
    GcCompaction compaction(os, prefix);
    const auto kStillLeader = [] { return true; };

    const BlobHash b = blobHash("e01");
    const PartId part = partId("e01");

    /// Pre-advance EVERY shard by folding the empty epoch 0, so the open epoch is 1 everywhere (the `+`'s
    /// home shard is derived from the part_id/pins, not chosen — fold all so it lands in a fresh open epoch).
    for (ShardId s = 0; s < kGcShardCount; ++s)
        ASSERT_EQ(compaction.compactShard(s, kStillLeader).new_epoch, 1u);

    /// Append the `+`. The writer reads the open epoch of each fragment's home shard (1), flushes into 1, and
    /// runs the rule-2 re-append (no further advance -> the `+` settles in epoch 1 of its home shard(s)).
    GcDelta delta;
    delta.op = GcDelta::Op::Add;
    delta.part_id = part;
    delta.pins = {b};
    delta.event_id = GcDelta::computeEventId(part, GcDelta::Op::Add);
    const auto settled = writer.appendAndFlushForCommit(delta);
    ASSERT_FALSE(settled.empty());

    /// Now CLOSE each settled shard's epoch mid-flight: fold it. The reference is carried into the snapshot —
    /// b is counted, NOT a count-0 candidate. (No under-count: the §5.1 rule-2 carrier + close-before-fold
    /// made the log complete WITHOUT the lock.)
    for (const auto & [s, e] : settled)
    {
        const auto folded = compaction.compactShard(s, kStillLeader);
        EXPECT_GT(folded.new_epoch, e);
        for (const auto & c : folded.candidates)
        {
            EXPECT_NE(c.key.identity, b.string()) << "a live blob must NEVER fall out as a count-0 candidate";
            EXPECT_NE(c.key.identity, part.string()) << "a live part edge must NEVER fall out as a count-0 candidate";
        }
    }

    /// The settled epoch is now folded — the folded watermark advanced (the reaper would now release a
    /// session covering this `+`).
    for (const auto & [s, e] : settled)
        EXPECT_TRUE(compaction.isEpochFolded(s, e)) << "the `+`'s settled epoch must be folded after the close";
}

/// ── Oracle 3 — seal -> resurrect -> GC recovers the OLD generation (I7a): both g and g+1 pinned, reads stay
/// byte-correct. A seal of (H,0) + a resurrected (H,1) both backing a live ref: the read path resolves SOME
/// present generation byte-correctly, and the GC keeps both while the identity is reachable.
TEST_F(ContentAddressedGcS4, TwoGenerationsPinned_ReadsByteCorrect)
{
    const BlobHash h = blobHash("d01");
    const PartId pid = partId("d01");

    put(blobGenKey(prefix, h, 0).string(), "GEN0-BYTES");
    ASSERT_TRUE(seal(h, 0));                                  // (H,0) sealed
    ASSERT_TRUE(condCreateIfAbsent(*os, blobGenKey(prefix, h, 1).string(), "GEN1-BYTES")); // resurrected (H,1)
    put(blobActiveKey(prefix, h), "1");
    publishLivePart("uuid-d01", "all_1_1_0", pid, {h});      // a LIVE ref pins bare H

    /// A sweep with the identity reachable: (H,0) is sealed-with-a-successor -> DRAIN (kept, not re-opened);
    /// (H,1) is the live attach target -> kept. Neither is deleted while the ref lives.
    DB::ContentAddressed::ContentAddressedGC gc(os, prefix);
    gc.setReconciliationCadenceRounds(1);
    gc.runSweepOnce(/*now=*/0, /*grace=*/0);
    gc.runSweepOnce(/*now=*/100, /*grace=*/0);

    EXPECT_TRUE(exists(blobGenKey(prefix, h, 0).string())) << "(H,0) DRAINed (successor exists, identity reachable) — kept";
    EXPECT_TRUE(exists(blobGenKey(prefix, h, 1).string())) << "(H,1) is the live generation — kept";
    EXPECT_EQ(get(blobGenKey(prefix, h, 0).string()), "GEN0-BYTES");
    EXPECT_EQ(get(blobGenKey(prefix, h, 1).string()), "GEN1-BYTES");
    EXPECT_EQ(presentBlobGenerations(h), (std::set<uint64_t>{0, 1}));
}

/// ── Oracle 4 — reaper race-safety (the lockless reaper). The session reaper runs CONCURRENTLY (interleaved)
/// with a commit raising a NEW session. It must NEVER delete (a) a just-raised session whose deltas are not
/// folded, nor (b) any session whose epochs are unfolded — and must reap exactly the committed-AND-folded
/// ones. Driven by hand: seed sessions in distinct fold states, run the real sweep (which runs the reaper),
/// then raise a brand-new uncommitted session in the same round and re-run.
TEST_F(ContentAddressedGcS4, Reaper_RaceSafe_NeverReapsUnfoldedOrJustRaised)
{
    GcCompaction compaction(os, prefix);
    const auto kStillLeader = [] { return true; };

    /// Seed the durable sessions in distinct fold states. We hand-craft `delta_epochs` so the foldedness is
    /// deterministic and INDEPENDENT of how many shards the sweep happens to fold:
    ///   - committed + EMPTY delta_epochs (a delta-less commit — trivially folded) -> MUST be reaped.
    ///   - committed + an epoch FAR in the future (never folded) -> MUST be kept (the folded watermark gates
    ///     the reap, NOT a timer; the sweep advancing the live shards never reaches this epoch).
    ///   - uncommitted (still uploading) -> MUST be kept (the owner drops it at commit/abort).
    /// `unfolded_epoch` is a small epoch the initial sweeps will NOT have folded (each sweep advances shard 0
    /// by one fold; the snapshot stays below this for the first rounds) but which we can deterministically
    /// fold later by a few explicit `compactShard` calls.
    const uint64_t unfolded_epoch = 8;
    raiseSession("sess-folded", /*committed=*/true, /*pins=*/{blobHash("rf")}, /*delta_epochs=*/{});
    raiseSession("sess-unfolded", /*committed=*/true, /*pins=*/{blobHash("ru")}, {{0, unfolded_epoch}});
    raiseSession("sess-uncommitted", /*committed=*/false, /*pins=*/{blobHash("rc")}, {});
    ASSERT_EQ(countSessions(), 3u);
    ASSERT_FALSE(compaction.isEpochFolded(0, unfolded_epoch)) << "the epoch must start UNFOLDED";

    /// Run the real sweep (it runs reapFoldedSessions). It must reap ONLY the committed + trivially-folded
    /// session.
    DB::ContentAddressed::ContentAddressedGC gc(os, prefix);
    gc.runSweepOnce(/*now=*/0, /*grace=*/0);

    EXPECT_FALSE(exists(sessionKey(prefix, "sess-folded"))) << "committed + every-epoch-folded -> reaped";
    EXPECT_TRUE(exists(sessionKey(prefix, "sess-unfolded"))) << "committed but UNFOLDED -> never reaped on a timer";
    EXPECT_TRUE(exists(sessionKey(prefix, "sess-uncommitted"))) << "uncommitted (still uploading) -> never reaped by the GC";

    /// Interleave: a commit raises a BRAND-NEW uncommitted session in the same window, then the reaper runs
    /// again. The just-raised session must survive (it is not committed; its deltas are not folded). This is
    /// the race the lockless reaper must be safe against — a session created concurrently with the LIST/READ.
    raiseSession("sess-just-raised", /*committed=*/false, /*pins=*/{blobHash("rj")}, {});
    gc.runSweepOnce(/*now=*/50, /*grace=*/0);
    EXPECT_TRUE(exists(sessionKey(prefix, "sess-just-raised"))) << "a session raised concurrently with the reaper is never mistakenly reaped";
    EXPECT_TRUE(exists(sessionKey(prefix, "sess-unfolded"))) << "the still-unfolded session is still kept across rounds";

    /// Now make the unfolded session's epoch folded by advancing shard 0's snapshot PAST it (each
    /// `compactShard` folds the open epoch and advances the snapshot by one). The next reaper round reaps the
    /// session — proving the reap gate is the folded watermark, nothing else.
    for (int i = 0; i < 64 && !compaction.isEpochFolded(0, unfolded_epoch); ++i)
        compaction.compactShard(0, kStillLeader);
    ASSERT_TRUE(compaction.isEpochFolded(0, unfolded_epoch)) << "the epoch is now folded (snapshot advanced past it)";
    gc.runSweepOnce(/*now=*/100, /*grace=*/0);
    EXPECT_FALSE(exists(sessionKey(prefix, "sess-unfolded"))) << "once folded, the committed session is reaped";
}

/// ── Oracle 5 (FAILING / blocker #1 proof) — generations must survive the real writer→log→compaction path.
///
/// The blocker: `GcLogWriter::splitDeltaByShard` drops the resolved `pin_generations` when splitting a
/// logical delta across shards, so every `gc/log` fragment is serialised with an EMPTY `pin_generations`
/// vector.  When the compaction folds that fragment it treats every pin as g=0 (the fallback).  This means
/// a delta carrying (b, g=1) lands in the log as (b, g=0), collapsing distinct generations into the SAME
/// count key — the generation-aware `CountKey{Blob, identity, generation}` is inert.
///
/// Proof: thread a g>0 delta through the REAL writer→log→compaction path and assert
///   (a) the +/-  for (b, g=0) net to zero → (b,0) is a count-0 candidate, AND
///   (b) (b, g=1) remains alive (still pinned by p1) → (b,1) is NOT a candidate.
///
/// With the bug both (b,0) and (b,1) collapse to (b,0) in the log, giving count(b,0) = +1+1-1 = +1
/// → NOT a candidate → EXPECT_TRUE(b_g0_is_candidate) FAILS, catching the blocker.
TEST_F(ContentAddressedGcS4, Sec6_GenerationsSurviveTheRealWriterPath_WouldHaveCaughtBlocker1)
{
    GcLogWriter writer(os, prefix);
    GcCompaction compaction(os, prefix);
    const auto kStillLeader = [] { return true; };

    /// Pre-advance every shard so the open epoch is 1 (mirrors oracle 2).
    for (ShardId s = 0; s < kGcShardCount; ++s)
        ASSERT_EQ(compaction.compactShard(s, kStillLeader).new_epoch, 1u);

    const BlobHash b = blobHash("g01");
    const PartId p0 = partId("g0");   /// pins b at generation 0
    const PartId p1 = partId("g1");   /// pins b at generation 1 (a resurrected blob)

    /// + for (b, g=0) under part p0 (manifest mg=0).
    {
        GcDelta d;
        d.op = GcDelta::Op::Add;
        d.part_id = p0;
        d.manifest_generation = 0;
        d.pins = {b};
        d.pin_generations = {0};
        d.event_id = GcDelta::computeEventId(p0, GcDelta::Op::Add, 0);
        writer.appendAndFlushForCommit(d);
    }
    /// + for (b, g=1) under part p1 (manifest mg=1) — the resurrected generation.
    {
        GcDelta d;
        d.op = GcDelta::Op::Add;
        d.part_id = p1;
        d.manifest_generation = 1;
        d.pins = {b};
        d.pin_generations = {1};
        d.event_id = GcDelta::computeEventId(p1, GcDelta::Op::Add, 1);
        writer.appendAndFlushForCommit(d);
    }
    /// - for (b, g=0): drop p0. b@g0 now nets to zero; b@g1 is still pinned by p1.
    {
        GcDelta d;
        d.op = GcDelta::Op::Remove;
        d.part_id = p0;
        d.manifest_generation = 0;
        d.pins = {b};
        d.pin_generations = {0};
        d.event_id = GcDelta::computeEventId(p0, GcDelta::Op::Remove, 0);
        writer.enqueue(d);
    }
    writer.flushAll();

    /// Fold every shard and collect the count-0 candidates with their generation.
    bool b_g0_is_candidate = false;
    bool b_g1_is_candidate = false;
    for (ShardId s = 0; s < kGcShardCount; ++s)
    {
        const auto folded = compaction.compactShard(s, kStillLeader);
        for (const auto & c : folded.candidates)
        {
            if (c.key.identity == b.string() && c.key.generation == 0)
                b_g0_is_candidate = true;
            if (c.key.identity == b.string() && c.key.generation == 1)
                b_g1_is_candidate = true;
        }
    }

    /// With #1 fixed: (b,0) nets +1(p0) -1(dropP0) = 0 -> a count-0 candidate; (b,1) is +1(p1) -> NOT a
    /// candidate (the resurrected generation survives, keyed independently at g=1).
    EXPECT_TRUE(b_g0_is_candidate) << "g=0 must net to zero and become a candidate";
    EXPECT_FALSE(b_g1_is_candidate) << "g=1 is still pinned and must NOT be swept — proves the generation survived splitDeltaByShard";
    /// With the #1 bug everything collapses to g=0: count(b,0)=+1+1-1=+1 -> b_g0_is_candidate is FALSE and
    /// no (b,1) key exists -> this test fails, exactly catching the blocker.
}

/// CA GC S3 (#6): RefSidecar v2 serialize/deserialize round-trips the settled generations.
/// Proves that `manifest_generation` and `pin_generations` survive the codec so the DROP path
/// can reliably read back what the `+` path wrote.
TEST_F(ContentAddressedGcS4, Sec6_RefSidecarRoundTripsSettledGenerations)
{
    const BlobHash b = blobHash("s01");
    RefSidecar in;
    in.files["uuid.txt"] = "deadbeef";
    in.manifest_generation = 1;
    in.pin_generations[b.string()] = 1;

    const RefSidecar out = RefSidecar::deserialize(in.serialize());
    EXPECT_EQ(out.files.at("uuid.txt"), "deadbeef");
    EXPECT_EQ(out.manifest_generation, 1u);
    ASSERT_TRUE(out.pin_generations.contains(b.string()));
    EXPECT_EQ(out.pin_generations.at(b.string()), 1u);
}

TEST_F(ContentAddressedGcS4, Sec2_StickySession_NotReapedUntilRelogged_ThenReleasedOnFold)
{
    const BlobHash b = blobHash("k01");
    const PartId p = partId("k01");

    /// A committed sticky session: its `+` flush "failed", so it carries the serialized `+` and deltas_failed.
    /// Its lease is already in the PAST — a non-sticky session would be lease-reclaimed; a sticky one is not.
    GcDelta add;
    add.op = GcDelta::Op::Add;
    add.part_id = p;
    add.pins = {b};
    add.pin_generations = {0};
    add.event_id = GcDelta::computeEventId(p, GcDelta::Op::Add, 0);

    WriteSession s;
    s.server_id = "srv";
    s.lease_deadline_unix = 1; /// far in the past relative to the sweep clock below
    s.committed = true;
    s.deltas_failed = true;
    s.pending = {b};
    s.pending_add_delta = serializeGcDeltasForSession({add});
    put(sessionKey(prefix, "sess-sticky"), s.serialize());

    DB::ContentAddressed::ContentAddressedGC gc(os, prefix);

    /// Round 1: the reaper re-logs the `+`, clears sticky, and does NOT reap (foldedness rechecked next round).
    gc.runSweepOnce(/*now=*/1'000'000, /*grace=*/0);
    EXPECT_TRUE(exists(sessionKey(prefix, "sess-sticky"))) << "a sticky session is never reaped on the round it converts";
    {
        const WriteSession after = WriteSession::deserialize(get(sessionKey(prefix, "sess-sticky")));
        EXPECT_FALSE(after.deltas_failed) << "the reaper cleared sticky after the bounded re-log landed";
        EXPECT_FALSE(after.delta_epochs.empty()) << "the re-log recorded the settled (shard, epoch)";
    }

    /// Fold every recorded epoch, then run the reaper again: the now-normal committed session is reaped.
    GcCompaction compaction(os, prefix);
    const auto kStillLeader = [] { return true; };
    const WriteSession converted = WriteSession::deserialize(get(sessionKey(prefix, "sess-sticky")));
    for (const auto & [shard, epoch] : converted.delta_epochs)
        for (int i = 0; i < 64 && !compaction.isEpochFolded(shard, epoch); ++i)
            compaction.compactShard(shard, kStillLeader);
    gc.runSweepOnce(/*now=*/2'000'000, /*grace=*/0);
    EXPECT_FALSE(exists(sessionKey(prefix, "sess-sticky"))) << "once re-logged + folded, the converted session is reaped";
}

TEST_F(ContentAddressedGcS4, Sec5_1_ReappendIfAdvancedActuallyFires)
{
    GcLogWriter writer(os, prefix);
    GcCompaction compaction(os, prefix);
    const auto kStillLeader = [] { return true; };

    const BlobHash b = blobHash("r01");
    const PartId p = partId("r01");

    /// Determine the home shard of the part so we can advance exactly it.
    const ShardId home = GcLogWriter::shardForPartId(p);

    /// Buffer a `+` into the CURRENT open epoch of the home shard (enqueue buffers; no flush yet).
    GcDelta add;
    add.op = GcDelta::Op::Add;
    add.part_id = p;
    add.pins = {b};
    add.pin_generations = {0};
    add.event_id = GcDelta::computeEventId(p, GcDelta::Op::Add, 0);
    writer.enqueue(add);

    /// Externally CLOSE the home shard's epoch (advance it) WHILE the `+` sits buffered in the old epoch.
    const auto first_fold = compaction.compactShard(home, kStillLeader);
    EXPECT_GE(first_fold.new_epoch, 1u);

    /// flushAll: the buffered `+` flushes into the now-stale epoch, then reappendIfAdvanced detects the
    /// advance and re-logs it into the open epoch. The next fold of the home shard must then COUNT b (not
    /// drop it as a count-0 candidate) — proving the re-append carried the straggler forward.
    writer.flushAll();

    bool b_dropped_as_candidate = false;
    for (int i = 0; i < 4; ++i)
    {
        const auto folded = compaction.compactShard(home, kStillLeader);
        for (const auto & c : folded.candidates)
            if (c.key.identity == b.string())
                b_dropped_as_candidate = true;
    }
    EXPECT_FALSE(b_dropped_as_candidate) << "the re-appended + must keep b counted across the epoch advance";
}

/// ── Oracle 2 (new, Task 16) — real lockless interleaving: a `+` lands as its epoch folds → the blob survives.
///
/// Unlike the pre-existing `Sec5_1_AppendAsEpochFolds` oracle which pre-advances all shards and has
/// reconciliation ON, this oracle runs with reconciliation OFF (the GcLogWriter / GcCompaction path only —
/// exactly the lockless regime the spec §7 says the old lock used to cover). The `+` is appended and
/// IMMEDIATELY each settled epoch is folded; the blob must NEVER appear as a count-0 candidate.
TEST_F(ContentAddressedGcS4, Sec7_RealLocklessInterleaving_PlusLandsAsEpochFolds_BlobSurvives)
{
    GcLogWriter writer(os, prefix);
    GcCompaction compaction(os, prefix);
    const auto kStillLeader = [] { return true; };

    const BlobHash b = blobHash("i01");
    const PartId p = partId("i01");

    /// reconciliationCadence stays at its default (off): the candidate source is the compaction/lockless
    /// path only — exactly the regime the spec says the old oracles never exercised.
    GcDelta add;
    add.op = GcDelta::Op::Add;
    add.part_id = p;
    add.pins = {b};
    add.pin_generations = {0};
    add.event_id = GcDelta::computeEventId(p, GcDelta::Op::Add, 0);
    const auto settled = writer.appendAndFlushForCommit(add);
    ASSERT_FALSE(settled.empty());

    /// Close each settled epoch (fold) — the `+` is carried into the snapshot, b is COUNTED, never a
    /// count-0 candidate (the lock-free re-append + dedup made the log complete).
    for (const auto & [s, e] : settled)
    {
        const auto folded = compaction.compactShard(s, kStillLeader);
        EXPECT_GT(folded.new_epoch, e);
        for (const auto & c : folded.candidates)
            EXPECT_NE(c.key.identity, b.string()) << "a live blob must never fall out as a count-0 candidate under the lockless fold";
    }
}

/// ── Oracle 5 (new, Task 16) — negative codec for the session delta batch (fail-closed).
///
/// `serializeGcDeltasForSession` / `deserializeGcDeltasFromSession` (the PLURAL helpers, CA GC S4 #2)
/// must round-trip a single delta exactly, and must throw (fail-closed) on both magic corruption and
/// truncation. This closes the negative-path gap in the codec test coverage.
TEST_F(ContentAddressedGcS4, Sec5_NegativeCodec_GcDeltasSession_FailsClosed)
{
    const PartId p = partId("n01");
    GcDelta d;
    d.op = GcDelta::Op::Add;
    d.part_id = p;
    d.pins = {blobHash("n01")};
    d.pin_generations = {0};
    d.event_id = GcDelta::computeEventId(p, GcDelta::Op::Add, 0);

    const std::string good = serializeGcDeltasForSession({d});
    EXPECT_NO_THROW(deserializeGcDeltasFromSession(good));
    /// round-trip preserves the single delta
    {
        const auto parsed = deserializeGcDeltasFromSession(good);
        ASSERT_EQ(parsed.size(), 1u);
        EXPECT_EQ(parsed.front().part_id.string(), p.string());
    }

    /// Corrupt the magic (first byte) -> must throw (fail-closed), not misparse.
    std::string bad_magic = good;
    bad_magic[0] = static_cast<char>(bad_magic[0] ^ 0xFF);
    EXPECT_ANY_THROW(deserializeGcDeltasFromSession(bad_magic));

    /// Truncate the body -> must throw.
    EXPECT_ANY_THROW(deserializeGcDeltasFromSession(good.substr(0, good.size() / 2)));
}

/// CA GC S4 (#4): round-trip unit test for the gc/sealed/<shard> index path builders + parser.
/// Verifies gcSealedKey/parseSealedIndexKey round-trip for both blob and part entries at multiple
/// generations, and that parseSealedIndexKey returns nullopt for malformed inputs.
TEST(ContentAddressedSealedIndex, RoundTrip)
{
    const std::string pool_prefix = "mypool/prefix";

    /// Realistic 32-char lowercase-hex digests (only [0-9a-f] — no dots).
    const BlobHash blob_hash("aabbccddeeff00112233445566778899");
    const PartId part_id("deadbeef01234567890abcdef1234567");

    const ShardId blob_shard = shardForHash(blob_hash);
    const ShardId part_shard = shardForPartId(part_id);

    /// -- Blob entry, generation 0 --
    {
        const std::string key = gcSealedKey(pool_prefix, blob_shard, blob_hash.string(), 0, /*is_blob=*/true);
        const auto parsed = parseSealedIndexKey(pool_prefix, key);
        ASSERT_TRUE(parsed.has_value()) << "blob gen=0 must parse";
        EXPECT_EQ(parsed->identity, blob_hash.string());
        EXPECT_EQ(parsed->generation, 0u);
        EXPECT_TRUE(parsed->is_blob);
    }

    /// -- Blob entry, generation 5 --
    {
        const std::string key = gcSealedKey(pool_prefix, blob_shard, blob_hash.string(), 5, /*is_blob=*/true);
        const auto parsed = parseSealedIndexKey(pool_prefix, key);
        ASSERT_TRUE(parsed.has_value()) << "blob gen=5 must parse";
        EXPECT_EQ(parsed->identity, blob_hash.string());
        EXPECT_EQ(parsed->generation, 5u);
        EXPECT_TRUE(parsed->is_blob);
    }

    /// -- Part (manifest) entry, generation 0 --
    {
        const std::string key = gcSealedKey(pool_prefix, part_shard, part_id.string(), 0, /*is_blob=*/false);
        const auto parsed = parseSealedIndexKey(pool_prefix, key);
        ASSERT_TRUE(parsed.has_value()) << "part gen=0 must parse";
        EXPECT_EQ(parsed->identity, part_id.string());
        EXPECT_EQ(parsed->generation, 0u);
        EXPECT_FALSE(parsed->is_blob);
    }

    /// -- Part entry, generation 5 --
    {
        const std::string key = gcSealedKey(pool_prefix, part_shard, part_id.string(), 5, /*is_blob=*/false);
        const auto parsed = parseSealedIndexKey(pool_prefix, key);
        ASSERT_TRUE(parsed.has_value()) << "part gen=5 must parse";
        EXPECT_EQ(parsed->identity, part_id.string());
        EXPECT_EQ(parsed->generation, 5u);
        EXPECT_FALSE(parsed->is_blob);
    }

    /// -- shardForPartId in PoolPaths agrees with GcLogWriter::shardForPartId (single source of truth) --
    EXPECT_EQ(shardForPartId(part_id), GcLogWriter::shardForPartId(part_id));

    /// -- Reject: bare garbage string --
    EXPECT_FALSE(parseSealedIndexKey(pool_prefix, "garbage").has_value());

    /// -- Reject: key under a different prefix --
    EXPECT_FALSE(parseSealedIndexKey(pool_prefix, "other/gc/sealed/0/aabb.0.b").has_value());

    /// -- Reject: missing generation segment (only 2 dot-fields in basename) --
    {
        const std::string bad = gcSealedPrefix(pool_prefix, blob_shard) + blob_hash.string() + ".b";
        EXPECT_FALSE(parseSealedIndexKey(pool_prefix, bad).has_value());
    }

    /// -- Reject: bad type char --
    {
        const std::string bad = gcSealedPrefix(pool_prefix, blob_shard) + blob_hash.string() + ".0.x";
        EXPECT_FALSE(parseSealedIndexKey(pool_prefix, bad).has_value());
    }

    /// -- Reject: non-numeric generation --
    {
        const std::string bad = gcSealedPrefix(pool_prefix, blob_shard) + blob_hash.string() + ".abc.b";
        EXPECT_FALSE(parseSealedIndexKey(pool_prefix, bad).has_value());
    }

    /// -- Reject: empty identity --
    {
        const std::string bad = gcSealedPrefix(pool_prefix, blob_shard) + ".0.b";
        EXPECT_FALSE(parseSealedIndexKey(pool_prefix, bad).has_value());
    }
}

/// ── Oracle 6 — §4 Scan A: the gc/sealed/<shard> index re-presents a sealed candidate ACROSS rounds with NO
/// full bucket scan. Round 1 discovers the orphan via the reconciliation full-scan and SEALS it (which must
/// create the gc/sealed index entry). Round 2 runs with reconciliation OFF — so the candidate can ONLY be
/// re-presented from the index — and (grace satisfied) sweeps it. After the sweep the gen object is gone and
/// the index entry is removed (a swept generation is not re-presented forever), while the gravestone remains.
TEST_F(ContentAddressedGcS4, Sec4_SealedIndex_RePresentsAcrossRounds_NoBucketScan)
{
    const BlobHash b = blobHash("x01");

    /// Put an unreferenced generation object (no ref pins it) so the sweep seals it.
    put(blobGenKey(prefix, b, 0).string(), "ORPHAN");

    DB::ContentAddressed::ContentAddressedGC gc(os, prefix);
    gc.setReconciliationCadenceRounds(1); /// first round discovers via reconciliation (full scan) -> seals

    /// Round 1: discover (reconciliation), seal (H,0), arm grace. The seal must create a gc/sealed entry.
    gc.runSweepOnce(/*now=*/0, /*grace=*/100);
    const ShardId shard = shardForHash(b);
    EXPECT_TRUE(exists(gcSealedKey(prefix, shard, b.string(), 0, /*is_blob=*/true)))
        << "seal must record the open tombstone in the gc/sealed index";

    /// Round 2 with reconciliation OFF: the candidate must still be re-presented purely from the index, and
    /// (grace satisfied) swept. The generation object is gone; the index entry is removed on sweep.
    gc.setReconciliationCadenceRounds(0);
    gc.runSweepOnce(/*now=*/1000, /*grace=*/100);
    EXPECT_FALSE(exists(blobGenKey(prefix, b, 0).string())) << "the index re-presented the candidate and it was swept";
    EXPECT_FALSE(exists(gcSealedKey(prefix, shard, b.string(), 0, /*is_blob=*/true)))
        << "a swept generation's index entry is removed (not re-presented forever)";
}
