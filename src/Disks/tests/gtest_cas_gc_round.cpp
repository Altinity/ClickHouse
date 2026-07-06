#include <gtest/gtest.h>

#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasBlobInDegree.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasGc.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasGcCursorKey.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasInMemoryBackend.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasStore.h>
#include "cas_test_helpers.h"

namespace DB::ErrorCodes
{
extern const int BAD_ARGUMENTS;
extern const int CORRUPTED_DATA;
}

using namespace DB::Cas;
using namespace DB::Cas::tests;

/// ROUND-LEVEL end-to-end GC tests over the root-local part-manifest model (one-pass ack-floor round:
/// heartbeat floor -> fold with the three-cursor merge -> pre-CAS exact-token deletes -> single CAS -> trim).
///
/// This file is the survivor of the old snap/cascade-based `gtest_cas_gc_round.cpp`. The per-STEP
/// behaviours it used to cover have moved to the dedicated GC-core suites and are intentionally NOT
/// re-tested here:
///   - fold edge dispatch (committed/precommit/promote/removal +/-1, 404 clamp/anomaly, fold barrier,
///     ref-mismatch fail-closed) -> gtest_cas_gc_fold.cpp
///   - condemn/graduate/delete + spare (manifest body deferred delete, publish racing the pass is spared,
///     unreferenced blob exact-token delete) -> gtest_cas_gc_ack_floor.cpp
///   - trim of folded owner events + idempotent crash replay -> gtest_cas_gc_resume.cpp
/// What remains here is what those step suites do NOT cover: the LEASE/leadership protocol (the round's
/// only stateful concurrency), the cursor-key codec, and the multi-round END-TO-END reclaim scenarios
/// driven to fixpoint (publish->drop->reclaim, multi-ref sharing, spare-on-recheck race, idempotent
/// fixpoint, split-brain duplicate-work-only). Every kept test keeps STRONG no-loss / no-dangle / no-leak
/// assertions. No test sleeps or reads a clock — "time" is the order of `runRegularRound` calls.

namespace
{

const UInt128 kGc = hexToU128("00000000000000000000000000000001");
const UInt128 kGcA = hexToU128("0000000000000000000000000000000a");
const UInt128 kGcB = hexToU128("0000000000000000000000000000000b");

ManifestRef ref(uint64_t seq, uint64_t inst)
{
    return ManifestRef{.writer_epoch = 1, .build_sequence = seq, .manifest_ordinal = static_cast<uint32_t>(inst)};
}

bool blobExists(InMemoryBackend & b, const Layout & layout, const UInt128 & hash)
{
    return b.head(layout.blobKey(BlobId(u128ToHex(hash)))).exists;
}

bool manifestExists(InMemoryBackend & b, const Layout & layout, const ManifestId & id)
{
    return b.head(layout.manifestKey(id)).exists;
}

StorePtr openTestStore(std::shared_ptr<InMemoryBackend> & out_backend)
{
    out_backend = std::make_shared<InMemoryBackend>();
    return Store::open(out_backend, PoolConfig{.pool_prefix = "p", .server_root_id = "test"});
}

StorePtr openTestStoreWithConfig(std::shared_ptr<InMemoryBackend> & out_backend, PoolConfig config)
{
    out_backend = std::make_shared<InMemoryBackend>();
    return Store::open(out_backend, std::move(config));
}

GcState readState(InMemoryBackend & b, const Store & s)
{
    const auto got = b.get(s.layout().gcStateKey());
    if (!got)
    {
        ADD_FAILURE() << "gc/state absent";
        return {};
    }
    return decodeGcState(got->bytes);
}

/// Whether the CURRENT retired list (any gc-shard, dereferenced through gc/state.retired_refs) holds any
/// entry — the ack-floor deletion pipeline is still in flight while this is true.
bool anyRetiredPending(InMemoryBackend & b, const Store & s)
{
    const auto st = b.get(s.layout().gcStateKey());
    if (!st)
        return false;
    const GcState gc_state = decodeGcState(st->bytes);
    for (const auto & [shard, key] : gc_state.retired_refs)
    {
        const auto got = b.get(key);
        if (got && !decodeRetiredSet(got->bytes).entries.empty())
            return true;
    }
    return false;
}

/// Drive a Gc to fixpoint over the ACK-FLOOR round: run rounds, advancing the store's own mount ack after
/// each (`renewWatermarkOnce` runs the beat so `observed_gc_round` follows the committed round and the
/// heartbeat floor advances). A condemned blob traverses the multi-round condemn -> graduate -> delete
/// pipeline, so "fixpoint" is reached only when a round did NO work AND the current retired list is empty
/// (nothing still in flight). Returns the number of rounds that held the lease and did work. Bounded so a
/// non-converging core fails downstream assertions rather than hanging.
size_t driveToFixpoint(InMemoryBackend & backend, const StorePtr & store, Gc & gc)
{
    size_t working_rounds = 0;
    for (size_t r = 0; r < 64; ++r)
    {
        const RoundReport rep = gc.runRegularRound();
        if (!rep.acquired_lease)
            continue;
        store->renewWatermarkOnce();
        const bool no_work = rep.candidates == 0 && rep.deleted == 0 && rep.absent == 0
            && rep.replaced == 0 && rep.spared == 0;
        if (no_work && !anyRetiredPending(backend, *store))
            break;
        if (!no_work)
            ++working_rounds;
    }
    return working_rounds;
}

}

/// ---- cursor-key codec (not snap-specific: cursorKey/parseCursorKey survive the redesign) ----

/// `cursorKey`/`parseCursorKey` must round-trip and match the legacy inline expressions in CasGc.cpp.
/// The namespace "srv1/tbl" deliberately contains a '/' to exercise the rfind-based last-slash split.
TEST(CasGcCursorKey, RoundTripsAndMatchesLegacyFormat)
{
    const RootNamespace ns{"srv1/tbl"};

    EXPECT_EQ(cursorKey(ns, 7), "srv1/tbl/7");
    EXPECT_EQ(cursorKey(ns, 0), "srv1/tbl/0");
    EXPECT_EQ(cursorKey(RootNamespace{"simple"}, 42), "simple/42");

    {
        const auto [pns, pshard] = parseCursorKey("srv1/tbl/7");
        EXPECT_EQ(pns.string(), "srv1/tbl");
        EXPECT_EQ(pshard, 7u);
    }
    {
        const auto [pns, pshard] = parseCursorKey("simple/42");
        EXPECT_EQ(pns.string(), "simple");
        EXPECT_EQ(pshard, 42u);
    }
    {
        const auto key = cursorKey(ns, 7);
        const auto [pns, pshard] = parseCursorKey(key);
        EXPECT_EQ(pns.string(), ns.string());
        EXPECT_EQ(pshard, 7u);
    }
}

/// ---- LEASE / leadership protocol (the round's only stateful concurrency) ----
///
/// The lease steal window is observation-based and deterministic (see CasGc.h): a contender becomes
/// steal-eligible when it observes the SAME (owner, seq) across two of its own consecutive round
/// attempts. The new model keeps gc/state {round, fence_seq, snap_generation, lease}, so these tests
/// are model-agnostic and were ported verbatim from the pre-redesign suite.

TEST(CasGcLease, FreshPoolAcquiresAndRenews)
{
    std::shared_ptr<InMemoryBackend> b;
    auto s = openTestStore(b);
    Gc gc(s, kGc);

    EXPECT_TRUE(gc.runRegularRound().acquired_lease);
    const GcState st1 = readState(*b, *s);
    EXPECT_EQ(st1.lease.owner, kGc);
    const uint64_t seq1 = st1.lease.seq;
    EXPECT_GE(seq1, 1u);

    EXPECT_TRUE(gc.runRegularRound().acquired_lease);            /// renew
    const GcState st2 = readState(*b, *s);
    EXPECT_EQ(st2.lease.owner, kGc);
    EXPECT_GT(st2.lease.seq, seq1);                              /// seq strictly advanced
    EXPECT_EQ(st2.fence_seq, st1.fence_seq);                     /// renewal is NOT a new epoch
}

TEST(CasGcLease, ContenderBacksOffWhileIncumbentRenews)
{
    std::shared_ptr<InMemoryBackend> b;
    auto s = openTestStore(b);
    Gc gc1(s, kGcA);
    Gc gc2(s, kGcB);

    ASSERT_TRUE(gc1.runRegularRound().acquired_lease);
    EXPECT_FALSE(gc2.runRegularRound().acquired_lease);          /// first sight: record observation
    ASSERT_TRUE(gc1.runRegularRound().acquired_lease);           /// incumbent renews (seq advances)
    EXPECT_FALSE(gc2.runRegularRound().acquired_lease);          /// gc2 sees a NEW seq => incumbent alive
    ASSERT_TRUE(gc1.runRegularRound().acquired_lease);
    EXPECT_FALSE(gc2.runRegularRound().acquired_lease);          /// alive again - never steals while renewing
    EXPECT_EQ(readState(*b, *s).lease.owner, kGcA);
}

TEST(CasGcLease, StealAfterObservedNonRenewalBumpsEpoch)
{
    std::shared_ptr<InMemoryBackend> b;
    auto s = openTestStore(b);
    Gc gc1(s, kGcA);
    Gc gc2(s, kGcB);

    ASSERT_TRUE(gc1.runRegularRound().acquired_lease);
    const GcState st0 = readState(*b, *s);
    EXPECT_FALSE(gc2.runRegularRound().acquired_lease);          /// observation recorded; gc1 then DIES
    EXPECT_TRUE(gc2.runRegularRound().acquired_lease);           /// same (owner, seq) observed twice => steal
    const GcState st = readState(*b, *s);
    EXPECT_EQ(st.lease.owner, kGcB);
    EXPECT_GT(st.lease.seq, st0.lease.seq);
    EXPECT_EQ(st.fence_seq, st0.fence_seq + 1);                  /// steal bumps the leadership epoch
}

TEST(CasGcLease, HeartbeatBlocksFalseStealOfAliveLeader)
{
    /// B160: a slow-but-alive incumbent whose lease.seq is frozen for its (long) round must NOT be
    /// stolen from, because its advisory heartbeat keeps advancing.
    std::shared_ptr<InMemoryBackend> b;
    auto s = openTestStore(b);
    Gc gc1(s, kGcA);
    Gc gc2(s, kGcB);

    ASSERT_TRUE(gc1.runRegularRound().acquired_lease);          /// gc1 leads (seq frozen for its round)
    EXPECT_FALSE(gc2.runRegularRound().acquired_lease);         /// gc2 observes (gc/hb absent yet)

    Gc::pulseHeartbeat(*s, kGcA);                              /// gc1 mid-round but heartbeating (hb 0->1)
    EXPECT_FALSE(gc2.runRegularRound().acquired_lease);         /// hb advanced => alive => NO steal
    Gc::pulseHeartbeat(*s, kGcA);                              /// hb 1->2
    EXPECT_FALSE(gc2.runRegularRound().acquired_lease);         /// still no steal while heartbeating
    EXPECT_EQ(readState(*b, *s).lease.owner, kGcA);            /// gc1 still owns the lease
}

TEST(CasGcLease, FailoverStealOnceHeartbeatStops)
{
    /// B160: once the incumbent stops heartbeating (it died), a follower observing the now-frozen
    /// heartbeat steals — automatic failover is preserved.
    std::shared_ptr<InMemoryBackend> b;
    auto s = openTestStore(b);
    Gc gc1(s, kGcA);
    Gc gc2(s, kGcB);

    ASSERT_TRUE(gc1.runRegularRound().acquired_lease);
    EXPECT_FALSE(gc2.runRegularRound().acquired_lease);         /// obs #1
    Gc::pulseHeartbeat(*s, kGcA);                              /// one last pulse (hb 0->1)
    EXPECT_FALSE(gc2.runRegularRound().acquired_lease);         /// hb advanced => no steal; records hb=1
    /// gc1 now DEAD: no renew, no further pulse. hb stays at 1 == gc2's last observation.
    EXPECT_TRUE(gc2.runRegularRound().acquired_lease);          /// hb frozen + seq frozen => STEAL
    EXPECT_EQ(readState(*b, *s).lease.owner, kGcB);
}

TEST(CasGcLease, DeadIncumbentThenRevivedIncumbentWinsRace)
{
    /// A stalled incumbent that revives and renews BEFORE the contender's second look resets the
    /// contender's window: gc2's second observation sees a NEW seq => NOT steal-eligible => backs off.
    std::shared_ptr<InMemoryBackend> b;
    auto s = openTestStore(b);
    Gc gc1(s, kGcA);
    Gc gc2(s, kGcB);

    ASSERT_TRUE(gc1.runRegularRound().acquired_lease);
    EXPECT_FALSE(gc2.runRegularRound().acquired_lease);          /// obs #1
    ASSERT_TRUE(gc1.runRegularRound().acquired_lease);           /// gc1 revives and renews
    EXPECT_FALSE(gc2.runRegularRound().acquired_lease);          /// new seq seen => window resets
    EXPECT_EQ(readState(*b, *s).lease.owner, kGcA);
}

TEST(CasGcLease, ConcurrentStealLosesCas)
{
    /// The CAS-race horn: gc2 is steal-eligible and goes for the CAS, but gc/state moved under it
    /// (injected one-shot conflict). It must back off (never acquired=true off a lost CAS) and the
    /// owner on storage must be unperturbed. The injected conflict left the object unchanged, so gc2's
    /// NEXT round is steal-eligible again and succeeds.
    std::shared_ptr<InMemoryBackend> b;
    auto s = openTestStore(b);
    Gc gc1(s, kGcA);
    Gc gc2(s, kGcB);

    ASSERT_TRUE(gc1.runRegularRound().acquired_lease);
    const GcState st0 = readState(*b, *s);
    EXPECT_FALSE(gc2.runRegularRound().acquired_lease);          /// obs #1; gc1 stalls now
    b->failNextCasPut(s->layout().gcStateKey());                 /// inject: gc2's steal CAS conflicts
    EXPECT_FALSE(gc2.runRegularRound().acquired_lease);          /// steal attempt loses the CAS => back off
    const GcState st1 = readState(*b, *s);
    EXPECT_EQ(st1.lease.owner, kGcA);                            /// unchanged
    EXPECT_EQ(st1.lease.seq, st0.lease.seq);                     /// nothing clobbered
    EXPECT_TRUE(gc2.runRegularRound().acquired_lease);           /// still steal-eligible => succeeds now
    EXPECT_EQ(readState(*b, *s).lease.owner, kGcB);
}

TEST(CasGcLease, CreateConflictReReadsWithinTheBound)
{
    /// The create-Conflict branch: a fresh pool where the create-if-absent CAS conflicts (one-shot).
    /// The contender re-reads and falls through within its bounded (2) CAS attempts — the re-read still
    /// finds the key absent, so the second attempt creates and acquires.
    std::shared_ptr<InMemoryBackend> b;
    auto s = openTestStore(b);
    Gc gc(s, hexToU128("0000000000000000000000000000000c"));

    b->failNextCasPut(s->layout().gcStateKey());
    EXPECT_TRUE(gc.runRegularRound().acquired_lease);
    const GcState st = readState(*b, *s);
    EXPECT_EQ(st.lease.owner, hexToU128("0000000000000000000000000000000c"));
    EXPECT_EQ(st.lease.seq, 1u);
}

TEST(CasGcLease, CtorFailsClosedOnBadArguments)
{
    /// Guards: a null store and gc_id == 0 (reserved for "lease never held") are caller bugs.
    std::shared_ptr<InMemoryBackend> b;
    auto s = openTestStore(b);
    expectThrowsCode(DB::ErrorCodes::BAD_ARGUMENTS, [&] { Gc(nullptr, kGc); });
    expectThrowsCode(DB::ErrorCodes::BAD_ARGUMENTS, [&] { Gc(s, DB::UInt128(0)); });
}

TEST(CasGcLease, IncumbentRenewConflictRetriesOnceAndAcquires)
{
    /// The incumbent's own renew CAS conflicts (one-shot). Re-read sees our own ownership => the renew
    /// is retried ONCE within the bounded (2) CAS attempts => acquired. Never acquired=true without a
    /// Committed CAS — storage must carry the seq the SECOND (committed) attempt wrote.
    std::shared_ptr<InMemoryBackend> b;
    auto s = openTestStore(b);
    Gc gc(s, hexToU128("0000000000000000000000000000000d"));

    ASSERT_TRUE(gc.runRegularRound().acquired_lease);            /// create: seq 1
    b->failNextCasPut(s->layout().gcStateKey());                 /// inject: the renew CAS conflicts
    EXPECT_TRUE(gc.runRegularRound().acquired_lease);            /// re-read (still us) => retried once
    const GcState st = readState(*b, *s);
    EXPECT_EQ(st.lease.owner, hexToU128("0000000000000000000000000000000d"));
    EXPECT_EQ(st.lease.seq, 2u);                                 /// the committed retry's seq
}

TEST(CasGcLease, VanishedStateAfterObservationFailsClosed)
{
    /// gc/state is never legally deleted - absent AFTER a recorded observation proves an out-of-model
    /// deletion. Recreating a default state would reset round/fence_seq/cursors; the lease protocol
    /// must fail closed (CORRUPTED_DATA) instead.
    std::shared_ptr<InMemoryBackend> b;
    auto s = openTestStore(b);
    Gc gc1(s, kGcA);
    Gc gc2(s, kGcB);

    ASSERT_TRUE(gc1.runRegularRound().acquired_lease);
    EXPECT_FALSE(gc2.runRegularRound().acquired_lease);          /// gc2 records an observation

    const auto head = b->head(s->layout().gcStateKey());         /// out-of-model wipe (raw delete)
    ASSERT_TRUE(head.exists);
    ASSERT_EQ(b->deleteExact(s->layout().gcStateKey(), head.token).kind, DeleteOutcome::Kind::Deleted);

    expectThrowsCode(DB::ErrorCodes::CORRUPTED_DATA, [&] { gc2.runRegularRound(); });
}

/// ---- END-TO-END round scenarios driven to fixpoint (the headline value of this file) ----

/// publish -> drop -> GC-to-fixpoint reclaim: a committed ref names a blob; after the ref is dropped,
/// the round protocol collects the blob (exact-token delete) AND the owner-removed manifest body, and a
/// further round is a clean no-op. The strongest no-loss/no-leak oracle: while the ref is live the blob
/// is NEVER touched; once dropped, BOTH the blob and the manifest are gone and nothing dangles.
TEST(CasGcRound, PublishDropReclaimsBlobAndManifestToFixpoint)
{
    auto backend = std::make_shared<InMemoryBackend>();
    auto store = openStoreForTest(backend);
    const RootNamespace ns{"00/aa@cas@"};
    const ManifestRef r = ref(1, 0xAA);
    const ManifestId id{ns, r};

    writeBlobBody(*backend, store->layout(), DB::UInt128(1));
    writeManifestRaw(*backend, store->layout(), ns, r, {blobEntryFor("a", DB::UInt128(1))});
    publishCommittedTransition(*backend, store->layout(), ns, "tbl", std::nullopt, r);

    Gc gc(store, kGc);
    driveToFixpoint(*backend, store, gc);
    /// While live: the blob's in-degree is 1 and NOTHING is collected (no-loss).
    EXPECT_EQ(inDegreeOf(*backend, store->layout(), DB::UInt128(1)), 1);
    EXPECT_TRUE(blobExists(*backend, store->layout(), DB::UInt128(1)));
    EXPECT_TRUE(manifestExists(*backend, store->layout(), id));

    dropRefTransition(*backend, store->layout(), ns, "tbl", r);
    driveToFixpoint(*backend, store, gc);
    /// After drop + fixpoint: the blob's only edge is gone, the blob is collected, the owner-removed
    /// manifest body is collected, and the in-degree generation reflects zero (no-leak / no-dangle).
    EXPECT_EQ(inDegreeOf(*backend, store->layout(), DB::UInt128(1)), 0);
    EXPECT_FALSE(blobExists(*backend, store->layout(), DB::UInt128(1)));
    EXPECT_FALSE(manifestExists(*backend, store->layout(), id));

    /// Idempotent: re-running to fixpoint changes nothing and never throws.
    EXPECT_NO_THROW(driveToFixpoint(*backend, store, gc));
    EXPECT_FALSE(blobExists(*backend, store->layout(), DB::UInt128(1)));
}

/// Attempt-scoping (B2): a fold seal planted under a NON-adopted attempt at the adopted generation
/// must be INVISIBLE to every reader. A deposed leader writes its fold seal under its own (unadopted)
/// `lease.seq`; that artifact lives at `foldSealKey(snap_generation, snap_attempt + k)` and no decision
/// path may resolve it. `previewDeletes` reads the in-degree generation strictly at the adopted
/// `(snap_generation, snap_attempt)`, so the decoy must not change its output and must not throw. This
/// is the implementation-level complement to the TLA+ `INV_ONLY_ADOPTED_VIEWABLE` gate.
TEST(CasGcRound, NonAdoptedAttemptSealIgnored)
{
    auto backend = std::make_shared<InMemoryBackend>();
    auto store = openStoreForTest(backend);
    const RootNamespace ns{"00/aa@cas@"};
    const ManifestRef r = ref(1, 0xAA);
    writeBlobBody(*backend, store->layout(), DB::UInt128(1));
    writeManifestRaw(*backend, store->layout(), ns, r, {blobEntryFor("a", DB::UInt128(1))});
    publishCommittedTransition(*backend, store->layout(), ns, "tbl", std::nullopt, r);

    Gc gc(store, kGc);
    gc.runRegularRound();
    const GcState st = readState(*backend, *store);
    ASSERT_GT(st.snap_generation, 0u);

    /// Control preview BEFORE the decoy (previewDeletes is write-free, so the result is deterministic).
    const auto control = gc.previewDeletes();

    /// Plant a decoy fold seal under a DIFFERENT attempt at the SAME generation (a deposed leader's
    /// unadopted artifact). It must be invisible to the adopted-attempt readers.
    backend->putIfAbsent(store->layout().foldSealKey(st.snap_generation, st.snap_attempt + 999),
                         "decoy-seal-bytes");

    /// No reader resolves the non-adopted attempt: no throw, and the preview is unchanged by the decoy.
    std::vector<Gc::PreviewEntry> after;
    EXPECT_NO_THROW(after = gc.previewDeletes());
    EXPECT_EQ(after.size(), control.size())
        << "a non-adopted attempt's fold seal must not influence previewDeletes";

    /// A further full round must still proceed without throwing and without the decoy wedging it.
    EXPECT_NO_THROW(gc.runRegularRound());
}

/// B11: the round summary must count manifest-body (tree) deletes separately from blob deletes. A drop
/// that reclaims one manifest body must report manifests_deleted >= 1 in the RoundReport of the
/// reclaiming round, while blobs and manifests remain separately countable.
TEST(CasGcRound, RoundSummaryCountsManifestBodyDeletes)
{
    auto backend = std::make_shared<InMemoryBackend>();
    auto store = openStoreForTest(backend);
    const RootNamespace ns{"00/aa@cas@"};
    const ManifestRef r = ref(1, 0xCC);
    const ManifestId id{ns, r};

    writeBlobBody(*backend, store->layout(), DB::UInt128(3));
    writeManifestRaw(*backend, store->layout(), ns, r, {blobEntryFor("a", DB::UInt128(3))});
    publishCommittedTransition(*backend, store->layout(), ns, "tbl", std::nullopt, r);

    Gc gc(store, kGc);
    driveToFixpoint(*backend, store, gc);   /// fold the publish; no delete yet

    dropRefTransition(*backend, store->layout(), ns, "tbl", r);

    /// Ack-floor drift: the owner-removed manifest body is deleted in the CONDEMNING round (post-CAS,
    /// after its -1 is adopted), while the blob's exact-token delete happens a few rounds later once the
    /// ack floor graduates its retired entry. So the two deletes fall in DIFFERENT reports now — accumulate
    /// across the pipeline (renewing the ack each round so the floor advances) and assert both were counted.
    uint64_t total_manifests_deleted = 0;
    uint64_t total_blob_deleted = 0;
    for (size_t i = 0; i < 64; ++i)
    {
        const RoundReport rep = gc.runRegularRound();
        if (!rep.acquired_lease)
            continue;
        store->renewWatermarkOnce();
        total_manifests_deleted += rep.manifests_deleted;
        total_blob_deleted += rep.deleted;
        if (total_manifests_deleted > 0 && total_blob_deleted > 0)
            break;
    }

    /// B11: the manifest-body delete must be counted separately from the blob delete.
    EXPECT_GE(total_manifests_deleted, 1u)
        << "round summary must count the owner-removed manifest body delete (B11 — manifests_deleted)";
    /// Blobs and manifests are separately countable: the blob delete (deleted >= 1) is independent.
    EXPECT_GE(total_blob_deleted, 1u)
        << "the blob exact-token delete must still be counted in deleted";
    /// The manifest body is gone and the blob is gone — no-leak / no-dangle.
    EXPECT_FALSE(manifestExists(*backend, store->layout(), id));
    EXPECT_FALSE(blobExists(*backend, store->layout(), DB::UInt128(3)));
}

/// M1 REGRESSION (cross-round fold cursor must survive independent of trim): a folded-but-untrimmed owner
/// event must NOT be re-folded by the next round. With eager trim the folded event is removed so the bug
/// (sealedCursorOf resetting to 0 after a completed round, because snap_generation points at the COMPLETION
/// generation whose fold_seal lives at the parent) is MASKED. Disable trim to expose it: the publish event
/// stays in the journal, so a round that re-folds from 0 emits a SECOND +1 and drives the blob's in-degree
/// to 2 (a silent over-pin => leak). The fix carries the per-shard fold cursor into the completion seal so
/// the next round recovers the exact cursor. Asserts in-degree stays EXACTLY 1 across >= 2 re-folds.
TEST(CasGcRound, FoldCursorSurvivesAcrossRoundsWithoutTrim)
{
    auto backend = std::make_shared<InMemoryBackend>();
    auto store = openStoreForTest(backend);
    const RootNamespace ns{"00/aa@cas@"};
    const ManifestRef r = ref(1, 0xAA);

    writeBlobBody(*backend, store->layout(), DB::UInt128(1));
    writeManifestRaw(*backend, store->layout(), ns, r, {blobEntryFor("a", DB::UInt128(1))});
    publishCommittedTransition(*backend, store->layout(), ns, "tbl", std::nullopt, r);

    Gc gc(store, kGc);
    gc.setTrimEnabledForTest(false);   /// keep the folded publish event in the journal across rounds

    /// Round 1 folds the +1 edge: in-degree 1, blob pinned.
    ASSERT_TRUE(gc.runRegularRound().acquired_lease);
    EXPECT_EQ(inDegreeOf(*backend, store->layout(), DB::UInt128(1)), 1);

    /// Several more rounds. The publish event is STILL in the journal (trim off). Each round must
    /// recover the exact sealed cursor and re-fold NOTHING for this shard — in-degree stays exactly 1.
    for (int round = 0; round < 3; ++round)
    {
        ASSERT_TRUE(gc.runRegularRound().acquired_lease);
        EXPECT_EQ(inDegreeOf(*backend, store->layout(), DB::UInt128(1)), 1)
            << "round " << round << ": a folded-but-untrimmed event was re-folded => blob in-degree double-counted";
    }

    /// No-loss throughout: the live blob and its owner body are intact.
    EXPECT_TRUE(blobExists(*backend, store->layout(), DB::UInt128(1)));
    EXPECT_TRUE(manifestExists(*backend, store->layout(), ManifestId{ns, r}));
}

/// Multi-ref sharing (INV-NO-LOSS): one blob referenced by TWO committed refs is spared until BOTH
/// drop. Dropping the first ref must NOT collect the blob (the second ref still pins it); only after the
/// second ref drops does the round collect it.
TEST(CasGcRound, SharedBlobSparedUntilBothRefsDrop)
{
    auto backend = std::make_shared<InMemoryBackend>();
    auto store = openStoreForTest(backend);
    const RootNamespace ns{"00/aa@cas@"};
    const ManifestRef r1 = ref(1, 0xA1);
    const ManifestRef r2 = ref(2, 0xA2);

    writeBlobBody(*backend, store->layout(), DB::UInt128(1));
    /// Two distinct manifests at two distinct refs, BOTH referencing the same shared blob 1.
    writeManifestRaw(*backend, store->layout(), ns, r1, {blobEntryFor("a", DB::UInt128(1))});
    writeManifestRaw(*backend, store->layout(), ns, r2, {blobEntryFor("a", DB::UInt128(1))});
    publishCommittedTransition(*backend, store->layout(), ns, "tbl1", std::nullopt, r1);
    publishCommittedTransition(*backend, store->layout(), ns, "tbl2", std::nullopt, r2);

    Gc gc(store, kGc);
    driveToFixpoint(*backend, store, gc);
    EXPECT_EQ(inDegreeOf(*backend, store->layout(), DB::UInt128(1)), 2);   /// two source edges
    EXPECT_TRUE(blobExists(*backend, store->layout(), DB::UInt128(1)));

    /// Drop the FIRST ref: in-degree falls to 1, blob STILL pinned by tbl2 (spared).
    dropRefTransition(*backend, store->layout(), ns, "tbl1", r1);
    driveToFixpoint(*backend, store, gc);
    EXPECT_EQ(inDegreeOf(*backend, store->layout(), DB::UInt128(1)), 1);
    EXPECT_TRUE(blobExists(*backend, store->layout(), DB::UInt128(1)))
        << "shared blob must survive while a second ref still names it";

    /// Drop the SECOND ref: in-degree reaches 0, blob is finally collected.
    dropRefTransition(*backend, store->layout(), ns, "tbl2", r2);
    driveToFixpoint(*backend, store, gc);
    EXPECT_EQ(inDegreeOf(*backend, store->layout(), DB::UInt128(1)), 0);
    EXPECT_FALSE(blobExists(*backend, store->layout(), DB::UInt128(1)));
}

/// Spare-during-the-pass, multi-blob discrimination: a drop condemns two blobs; in the SAME window
/// between rounds (before the next pass folds), one of them is re-referenced under a fresh ref. The pass
/// folds the racing publish and SPARES the re-referenced blob (recovery wins in the pass merge, dropping
/// its retired entry), while the genuinely-unreferenced blob proceeds through the condemn -> graduate ->
/// delete pipeline. The discriminating assertion: at fixpoint, one is spared (kept) and the other gone.
TEST(CasGcRound, RepublishDuringFenceWindowSparesOnlyReReferencedBlob)
{
    auto backend = std::make_shared<InMemoryBackend>();
    auto store = openStoreForTest(backend);
    const RootNamespace ns{"00/aa@cas@"};
    const ManifestRef r1 = ref(1, 0xB1);
    const ManifestRef r2 = ref(2, 0xB2);

    writeBlobBody(*backend, store->layout(), DB::UInt128(1));   /// kept (will be re-referenced)
    writeBlobBody(*backend, store->layout(), DB::UInt128(2));   /// genuinely dropped
    writeManifestRaw(*backend, store->layout(), ns, r1,
        {blobEntryFor("a", DB::UInt128(1)), blobEntryFor("b", DB::UInt128(2))});
    publishCommittedTransition(*backend, store->layout(), ns, "tbl", std::nullopt, r1);

    Gc gc(store, kGc);
    driveToFixpoint(*backend, store, gc);
    EXPECT_EQ(inDegreeOf(*backend, store->layout(), DB::UInt128(1)), 1);
    EXPECT_EQ(inDegreeOf(*backend, store->layout(), DB::UInt128(2)), 1);

    /// Repoint the ref from r1 to r2 between rounds: ONE event {old=committed(r1), new=committed(r2)}.
    /// The -1 (r1's body: blobs 1 AND 2) and +1 (r2's body: blob 1 only) net to in-degree 1 for blob 1
    /// (re-referenced => SPARED in the pass merge) and 0 for blob 2 (genuinely unreferenced => condemned,
    /// then reclaimed by the ack-floor pipeline). (A separate drop THEN repoint would double-count the -1
    /// on r1's blobs and drive blob 2 to -1 — an undercount the in-degree fold fails closed on.)
    writeManifestRaw(*backend, store->layout(), ns, r2, {blobEntryFor("a", DB::UInt128(1))});
    publishCommittedTransition(*backend, store->layout(), ns, "tbl", r1, r2);

    driveToFixpoint(*backend, store, gc);
    /// Blob 1 is re-referenced (net in-degree 1) => SPARED; blob 2 is genuinely unreferenced => GONE.
    EXPECT_TRUE(blobExists(*backend, store->layout(), DB::UInt128(1)))
        << "the racing republish must spare blob 1";
    EXPECT_EQ(inDegreeOf(*backend, store->layout(), DB::UInt128(1)), 1);
    EXPECT_FALSE(blobExists(*backend, store->layout(), DB::UInt128(2)))
        << "the genuinely-unreferenced blob 2 must be collected";
    EXPECT_EQ(inDegreeOf(*backend, store->layout(), DB::UInt128(2)), 0);
}

/// Idempotent fixpoint: once a pool is quiescent (all live refs folded, nothing to collect), repeated
/// rounds are pure no-ops — no blob is collected, no manifest disappears, the in-degree generation is
/// stable, and no round throws. The split-brain-safety bedrock: every step is idempotent.
TEST(CasGcRound, IdempotentRerunAtFixpointIsNoOp)
{
    auto backend = std::make_shared<InMemoryBackend>();
    auto store = openStoreForTest(backend);
    const RootNamespace ns{"00/aa@cas@"};
    const ManifestRef r = ref(1, 0xAA);
    const ManifestId id{ns, r};

    writeBlobBody(*backend, store->layout(), DB::UInt128(1));
    writeManifestRaw(*backend, store->layout(), ns, r, {blobEntryFor("a", DB::UInt128(1))});
    publishCommittedTransition(*backend, store->layout(), ns, "tbl", std::nullopt, r);

    Gc gc(store, kGc);
    driveToFixpoint(*backend, store, gc);
    const uint64_t gen0 = currentGenerationOf(*backend, store->layout());

    /// At quiescence: a fresh round does NO work (no candidates/deletes/spares) and changes nothing.
    const RoundReport quiescent = gc.runRegularRound();
    EXPECT_TRUE(quiescent.acquired_lease);
    EXPECT_EQ(quiescent.candidates, 0u);
    EXPECT_EQ(quiescent.deleted, 0u);
    EXPECT_EQ(quiescent.spared, 0u);

    EXPECT_NO_THROW(driveToFixpoint(*backend, store, gc));
    EXPECT_TRUE(blobExists(*backend, store->layout(), DB::UInt128(1)));   /// no-loss
    EXPECT_TRUE(manifestExists(*backend, store->layout(), id));          /// no-loss
    /// The CONTENT no-op invariant: the live blob's durable in-degree is unchanged (still pinned). The
    /// generation POINTER advances every round by design (each fold seals a fresh generation for durable
    /// cursor coverage, and recheck seals the completion generation), even when no edges change — so the
    /// quiescence guarantee is "no candidates/deletes/spares + nothing lost", NOT a frozen generation.
    EXPECT_EQ(inDegreeOf(*backend, store->layout(), DB::UInt128(1)), 1);
    EXPECT_GE(currentGenerationOf(*backend, store->layout()), gen0)
        << "the generation pointer is monotone; a quiescent round never moves it backward";
}

/// Split-brain: two leaders racing the same pool only DUPLICATE WORK, never double-delete or lose data.
/// gc1 leads and folds the live publish; the ref is then dropped; gc2 steals the lease (stale leader)
/// and both contend to collect the now-unreferenced blob. The exact-token delete is the only destructive
/// authority, so the blob is removed exactly once and a losing/duplicate attempt is a harmless 404/412 —
/// no exception escapes, and the blob ends up gone exactly once with no dangling owner.
TEST(CasGcRound, SplitBrainLeadersOnlyDuplicateWork)
{
    auto backend = std::make_shared<InMemoryBackend>();
    auto store = openStoreForTest(backend);
    const RootNamespace ns{"00/aa@cas@"};
    const ManifestRef r = ref(1, 0xAA);
    const ManifestId id{ns, r};

    writeBlobBody(*backend, store->layout(), DB::UInt128(1));
    writeManifestRaw(*backend, store->layout(), ns, r, {blobEntryFor("a", DB::UInt128(1))});
    publishCommittedTransition(*backend, store->layout(), ns, "tbl", std::nullopt, r);

    Gc gc1(store, kGcA);
    Gc gc2(store, kGcB);

    /// gc1 leads; fold the publish edge.
    ASSERT_TRUE(gc1.runRegularRound().acquired_lease);
    EXPECT_EQ(inDegreeOf(*backend, store->layout(), DB::UInt128(1)), 1);

    /// The ref is dropped; gc1 stalls. gc2 observes the frozen lease twice and STEALS (new epoch).
    dropRefTransition(*backend, store->layout(), ns, "tbl", r);
    EXPECT_FALSE(gc2.runRegularRound().acquired_lease);   /// obs #1
    ASSERT_TRUE(gc2.runRegularRound().acquired_lease);    /// obs #2 => steal
    EXPECT_GT(readState(*backend, *store).fence_seq, 0u);

    /// Both leaders now drive rounds. The blob is collected exactly once; duplicate attempts are
    /// harmless. No round throws.
    EXPECT_NO_THROW(driveToFixpoint(*backend, store, gc2));
    EXPECT_NO_THROW(driveToFixpoint(*backend, store, gc1));   /// the revived stale leader backs off / duplicates harmlessly

    EXPECT_FALSE(blobExists(*backend, store->layout(), DB::UInt128(1)))
        << "the dropped blob must be collected exactly once across both leaders";
    EXPECT_FALSE(manifestExists(*backend, store->layout(), id));
    EXPECT_EQ(inDegreeOf(*backend, store->layout(), DB::UInt128(1)), 0);
}

/// INV-JOURNAL-COVERAGE: trim only removes journal records at or below the SEALED fold cursor
/// (CasFoldSeal::per_ns_shard[ck].folded_cursor). Records with transition_version strictly above
/// the sealed cursor survive trim, even when the journal still holds them. This test uses a
/// fold-barrier scenario (a live precommit with no body) to produce a clamped cursor < shard_version,
/// so the journal has both trimmed events (<= cursor) and retained events (> cursor) after one round.
///
/// A second assertion confirms that a shard whose coverage cursor is carried forward across rounds
/// trims correctly on the second round — trim only removes records at or below the carried cursor.
TEST(CasGcRound, TrimOnlyBelowSealedCoverage)
{
    auto backend = std::make_shared<InMemoryBackend>();
    auto store = openStoreForTest(backend);
    const RootNamespace ns{"00/aa@cas@"};

    /// Publish a committed ref at version 1: has a body, folds cleanly.
    const ManifestRef r_committed = ref(1, 0xA1);
    writeBlobBody(*backend, store->layout(), DB::UInt128(1));
    writeManifestRaw(*backend, store->layout(), ns, r_committed, {blobEntryFor("a", DB::UInt128(1))});
    const uint64_t v1 = publishCommittedTransition(*backend, store->layout(), ns, "tbl1", std::nullopt, r_committed);

    /// Add a live precommit with NO body at version 2: this triggers the fold barrier, clamping the
    /// fold cursor at v1 (= v2 - 1). The fold seals cursor = v1; trim must remove the event at v1
    /// but RETAIN the precommit event at v2 (above the sealed cursor).
    const ManifestRef r_precommit = ref(2, 0xB2);
    /// No writeManifestRaw for r_precommit: body is intentionally absent (fold barrier).
    const uint64_t v2 = addPrecommitTransition(*backend, store->layout(), ns, DB::UInt128(9), "tbl2",
                                               std::nullopt, r_precommit);
    ASSERT_EQ(v2, v1 + 1);   /// ensure v2 = v1+1 so the cursor split is clear

    Gc gc(store, kGc);
    gc.runRegularRound();

    /// The fold cursor must be v1 (barrier halted at v2; resolved_through = v2-1 = v1).
    const uint64_t cursor = foldCursorOf(*backend, store->layout(), ns, 0);
    EXPECT_EQ(cursor, v1)
        << "sealed fold cursor must equal the last clean-folded version (v1); barrier halted at v2";

    /// Read the live shard journal and check the invariant:
    ///   - event at v1 (transition_version <= cursor): removed by trim
    ///   - event at v2 (transition_version > cursor): retained by trim
    const auto shard_bytes = backend->get(store->layout().rootShardKey(ns, 0));
    ASSERT_TRUE(shard_bytes.has_value()) << "root shard must exist after the round";
    const RootShard root = decodeRootShard(shard_bytes->bytes);
    for (const RootOwnerEvent & e : root.journal)
    {
        EXPECT_GT(e.transition_version, cursor)
            << "INV-JOURNAL-COVERAGE: event at version " << e.transition_version
            << " must be ABOVE the sealed cursor " << cursor << " (trim must not remove it)";
    }
    /// The precommit event (v2 > cursor) must survive trim.
    const bool precommit_retained = std::any_of(root.journal.begin(), root.journal.end(),
        [v2](const RootOwnerEvent & e) { return e.transition_version == v2; });
    EXPECT_TRUE(precommit_retained)
        << "the fold-barrier precommit event (v2=" << v2 << ") must be retained above the sealed cursor";
    /// The committed event (v1 <= cursor) must be removed by trim.
    const bool committed_removed = std::none_of(root.journal.begin(), root.journal.end(),
        [v1](const RootOwnerEvent & e) { return e.transition_version == v1; });
    EXPECT_TRUE(committed_removed)
        << "the committed publish event (v1=" << v1 << " <= cursor=" << cursor << ") must be trimmed";

    /// Second assertion: the cursor at v1 is carried forward into the next round. Run a second round
    /// (the barrier is still present — the precommit body is still absent). The new fold cursor is
    /// again v1 (barrier re-fires at v2, resolved_through = v1 again). Trim again removes events
    /// <= v1 (none remain) and retains v2. The journal must still contain exactly the precommit event.
    gc.runRegularRound();
    const uint64_t cursor2 = foldCursorOf(*backend, store->layout(), ns, 0);
    EXPECT_EQ(cursor2, v1)
        << "carried-forward cursor must remain at v1 while the barrier is unresolved";
    const auto shard2 = backend->get(store->layout().rootShardKey(ns, 0));
    ASSERT_TRUE(shard2.has_value());
    const RootShard root2 = decodeRootShard(shard2->bytes);
    for (const RootOwnerEvent & e : root2.journal)
    {
        EXPECT_GT(e.transition_version, cursor2)
            << "second round: event at version " << e.transition_version
            << " must be ABOVE the carried-forward cursor " << cursor2;
    }
    const bool precommit_still_retained = std::any_of(root2.journal.begin(), root2.journal.end(),
        [v2](const RootOwnerEvent & e) { return e.transition_version == v2; });
    EXPECT_TRUE(precommit_still_retained)
        << "second round: the precommit event (v2=" << v2 << ") must still be retained above the cursor";
}

/// ---- INTENTIONALLY NOT PORTED (covered elsewhere or obsolete in the manifest model) ----
///
/// The removed snap/cascade/tree cases and where their behaviour now lives:
///   - CasGcFold.{FreshUploadsAreNeverCandidates, DropZeroesTreeButChildStaysPinned,
///     RepublishSameRefIsLastOpWins, ExpansionIsOncePerTree, IncrementalSecondFoldOnlyNewRecords,
///     DurableSnapBeforeCursorAdvance, ForeignDivergentGenerationIsProbedPast,
///     GenerationProbeRecoversAfterLostCursorCas, SnapShardsOtherThanOneIsNotImplemented,
///     AbsentTree*, NoChurnRound*} — fold-step behaviour now in gtest_cas_gc_fold.cpp
///     (CommittedAdd/Removal/Precommit/FoldBarrier/Clamp+anomaly/RefMismatch).
///   - CasGcCorruptCommittedTree.MissingTreeOfLiveRefDoesNotHaltGc — now
///     CasGcFold.CommittedMissingBodyClampsCursorAndRecordsAnomaly.
///   - CasGcRetire.{Observes*, AbsentCandidate*, DeletedCandidate*, DeleteTimePrune*, BlobOnlyPrune*,
///     RetireForgets*, RetireSetsDurable*, Diverged*, BlobHeaderUnderflow*, RetireUsesFoldCommitted*,
///     RetireReplayAdoptsOwnCrashedAttempt} — retire-step behaviour now split between
///     gtest_cas_gc_ack_floor.cpp and the retire-view suite.
///   - CasGcRecheck.{SparedWhenPublishRacesTheFence, ReplacedWhenResurrectionWins, AbsentWhenAlreadyGone}
///     — now CasGcRecheck.{PublishRacingFenceSparesBlob, UnreferencedBlobDeletedExactToken}.
///   - CasGcFence.* / CasGcDiscovery.UsesRegistryNotList — the fence machinery is retired; the equivalent
///     no-op-round-does-not-mutate-ref-shards property is gtest_cas_gc_ack_floor.cpp::
///     CasGcAckFloor.NoOpRoundDoesNotMutateRefShards (+ helper registerNamespaceRaw discovery is
///     exercised by every fold test).
///   - CasGcCascade.* — the cascade/closure model is REMOVED; in-degree is per-blob, so a shared
///     child surviving one parent's deletion is now CasGcRound.SharedBlobSparedUntilBothRefsDrop above,
///     and "never cascades on replaced" is CasGcRound.RepublishDuringFenceWindowSparesOnlyReReferencedBlob.
///   - CasGcTrim.* — now gtest_cas_gc_resume.cpp::CasGcRound.TrimDropsFoldedOwnerEvents.
///   - CasGcResume.{CompletesRoundAfterCrashBeforeFencePersist, AdoptsOutcomesAfterCrashBeforeCascadePersist}
///     — now gtest_cas_gc_resume.cpp::CasGcResume.ResumeFromDurableFoldSealCompletesRound.
///   - CasGcScenario.ZombieDeleteAfterResurrectIs412 — relied on the snap/tree publish path + held
///     in-flight deletes; the in-degree-spare equivalent is RepublishDuringFenceWindowSparesOnly...
///     above (exact-token delete is the sole authority; a zombie carrying a stale token 412s).
///   - CasGcRound.PreviewDeletesIsWriteFreeAndSubsetOfUnreachable — previewDeletes survives, but it is
///     covered by the fsck/preview suite; not duplicated here.
///   - CasGcWatermark.LiveBuildPrecommitHonoredAcrossGcRounds /
///     CasGcRetire.ReclaimsAbandonedPrecommitWhenFloorPasses — precommit reclaim is exercised by the
///     orphan-manifest-sweep / build-root suites against the new reclaimAbandonedPrecommit path.

/// B9 snap-generation retention, reimplemented over the run/generation model: after a generation is
/// adopted the GC prunes the per-generation seal/run/cleanup objects of generations at or below the
/// retention floor (snap_generation - gc_snap_generations_to_keep), advancing snap_pruned_through. This
/// test drives enough rounds to accumulate several generations, then asserts that everything at or below
/// the floor is GONE while the last `keep` generations (and the live current one) remain.
TEST(CasGcSnapRetention, PrunesOldGenerationsKeepingLastThree)
{
    auto backend = std::make_shared<InMemoryBackend>();
    /// keep the default 3 generations; one root shard so cursor keys are "ns/0".
    auto store = Store::open(backend, PoolConfig{.pool_prefix = "p", .server_root_id = "test", .root_shards = 1, .gc_snap_generations_to_keep = 3, .gc_fold_max_defer_rounds = 0});
    const RootNamespace ns{"00/aa@cas@"};
    const ManifestRef r = ref(1, 0xAA);

    writeBlobBody(*backend, store->layout(), DB::UInt128(1));
    writeManifestRaw(*backend, store->layout(), ns, r, {blobEntryFor("a", DB::UInt128(1))});
    publishCommittedTransition(*backend, store->layout(), ns, "tbl", std::nullopt, r);

    Gc gc(store, kGc);
    /// Several quiescent rounds, each advancing the generation pointer (fold + completion). Enough to
    /// push generations below the floor.
    for (int i = 0; i < 8; ++i)
        ASSERT_TRUE(gc.runRegularRound().acquired_lease);

    const GcState st = readState(*backend, *store);
    const uint64_t keep = 3;
    ASSERT_GT(st.snap_generation, keep);
    const uint64_t floor = st.snap_generation - keep;

    /// snap_pruned_through reached the floor (bounded burst is large enough for this generation count).
    EXPECT_EQ(st.snap_pruned_through, floor)
        << "retention cursor must reach the floor (snap_generation - keep)";

    /// Every generation at or below the floor is fully gone (fold seal absent).
    for (uint64_t g = 1; g <= floor; ++g)
    {
        EXPECT_FALSE(backend->head(store->layout().foldSealKey(g, st.snap_attempt)).exists)
            << "fold seal of pruned generation " << g << " must be gone";
        EXPECT_FALSE(backend->head(store->layout().blobTargetRunKey(g, st.snap_attempt, /*shard*/0, /*seq*/0)).exists)
            << "blob-target run of pruned generation " << g << " must be gone";
    }

    /// The fold seal at the current generation survives (the live in-degree view).
    EXPECT_TRUE(backend->head(store->layout().foldSealKey(st.snap_generation, st.snap_attempt)).exists)
        << "the current generation's seal must NOT be pruned";

    /// No-loss: the live blob and owner body are intact throughout retention pruning.
    EXPECT_TRUE(blobExists(*backend, store->layout(), DB::UInt128(1)));
    EXPECT_TRUE(manifestExists(*backend, store->layout(), ManifestId{ns, r}));
}

/// Task 9 (wholesale generation-retention): a generation may hold artifacts under MULTIPLE attempts
/// (each round mints a fresh `lease.seq`, and a deposed leader may have written debris under its own
/// unadopted attempt). When a generation ages past the retention floor it must be reclaimed WHOLESALE
/// — every attempt's artifacts (incl. the attempt-scoped `retired/` and `outcomes/` sets that now live
/// under `gc/gen/<g>/attempt/<a>/`), not just the final adopted attempt's. This test plants a retired
/// set AND a decoy fold seal under a NON-adopted attempt at an old generation, ages that generation out,
/// and asserts the whole `gc/gen/<g>/` subtree is gone (the per-key single-attempt prune leaked it).
TEST(CasGcSnapRetention, WholesalePruneReclaimsAllAttemptsIncludingRetiredOutcomes)
{
    auto backend = std::make_shared<InMemoryBackend>();
    auto store = Store::open(backend, PoolConfig{.pool_prefix = "p", .server_root_id = "test", .root_shards = 1, .gc_snap_generations_to_keep = 3, .gc_fold_max_defer_rounds = 0});
    const RootNamespace ns{"00/aa@cas@"};
    const ManifestRef r = ref(1, 0xAA);

    writeBlobBody(*backend, store->layout(), DB::UInt128(1));
    writeManifestRaw(*backend, store->layout(), ns, r, {blobEntryFor("a", DB::UInt128(1))});
    publishCommittedTransition(*backend, store->layout(), ns, "tbl", std::nullopt, r);

    Gc gc(store, kGc);
    /// One round to establish the first completed generation and learn its adopted attempt (derive both
    /// from gc/state — never hardcode a generation; the round folds and completes, so it is > 1).
    ASSERT_TRUE(gc.runRegularRound().acquired_lease);
    const GcState st1 = readState(*backend, *store);
    ASSERT_GT(st1.snap_generation, 0u);
    const uint64_t old_gen = st1.snap_generation;
    const uint64_t adopted_attempt_g1 = st1.snap_attempt;

    /// Plant debris under a NON-adopted attempt of generation 1: a retired set, an outcomes log, a fold
    /// seal, and a blob-target run — exactly the families a deposed leader would have written before its
    /// CAS failed. The per-key single-attempt prune (keyed on the FINAL snap_attempt) never touches them.
    const uint64_t decoy_attempt = adopted_attempt_g1 + 777;
    const String decoy_retired = store->layout().retiredKey(old_gen, decoy_attempt, /*round*/0, /*shard*/0);
    const String decoy_outcomes = store->layout().outcomesKey(old_gen, decoy_attempt, /*round*/0, /*shard*/0);
    const String decoy_seal = store->layout().foldSealKey(old_gen, decoy_attempt);
    const String decoy_run = store->layout().blobTargetRunKey(old_gen, decoy_attempt, /*shard*/0, /*seq*/0);
    backend->putIfAbsent(decoy_retired, "decoy-retired");
    backend->putIfAbsent(decoy_outcomes, "decoy-outcomes");
    backend->putIfAbsent(decoy_seal, "decoy-seal");
    backend->putIfAbsent(decoy_run, "decoy-run");

    /// Drop the ref so the next fold writes a FRESH run under a newer generation and the adopted seal's
    /// blob_target ref moves OFF `old_gen`. Under T0 reference-parent carry, a still-referenced generation
    /// is deliberately retained (its run is live), so `old_gen` can only age out once nothing references
    /// its run anymore — which the drop guarantees.
    dropRefTransition(*backend, store->layout(), ns, "tbl", r);

    /// Age generation 1 well past the retention floor (keep=3): several more quiescent rounds.
    for (int i = 0; i < 8; ++i)
        ASSERT_TRUE(gc.runRegularRound().acquired_lease);

    const GcState st = readState(*backend, *store);
    ASSERT_GT(st.snap_generation, old_gen + 3) << "generation 1 must be below the retention floor";

    /// The ENTIRE gc/gen/<old_gen>/ subtree — across ALL attempts — must be reclaimed.
    EXPECT_FALSE(backend->head(decoy_retired).exists) << "non-adopted retired set leaked past retention";
    EXPECT_FALSE(backend->head(decoy_outcomes).exists) << "non-adopted outcomes log leaked past retention";
    EXPECT_FALSE(backend->head(decoy_seal).exists) << "non-adopted fold seal leaked past retention";
    EXPECT_FALSE(backend->head(decoy_run).exists) << "non-adopted blob-target run leaked past retention";

    /// Nothing remains under the old generation prefix at all.
    const ListPage residue = backend->list(store->layout().gcGenPrefix(old_gen), "", 1000);
    EXPECT_TRUE(residue.keys.empty()) << "old generation prefix must be fully reclaimed; left "
                                      << residue.keys.size() << " objects";

    /// The drop was necessary to move the seal's blob_target ref off `old_gen` so it could age out (under
    /// T0 a still-referenced generation is deliberately retained — see the comment above). The blob is
    /// condemned by the drop but stays present: these rounds never advance the mount ack floor
    /// (`renewWatermarkOnce`), so it never graduates to a delete — retire drain is not the property under
    /// test here.
    EXPECT_TRUE(blobExists(*backend, store->layout(), DB::UInt128(1)));
    /// The owner-removed manifest body IS reclaimed by the part-manifest cleanup pass over the aging rounds.
    EXPECT_FALSE(manifestExists(*backend, store->layout(), ManifestId{ns, r}));
}

/// Reclaim-VIA-RETENTION of a non-adopted current-generation attempt orphan (KISS prune model). A
/// deposed leader can write its fold seal under an attempt that lost CAS #1 to a higher-seq adopter —
/// debris at the FOLD generation under a NON-adopted attempt. There is NO per-round current-generation
/// attempt-sweep anymore (it cost a per-round LIST for a rare collision); the wholesale
/// generation-retention prune is the SOLE reclaimer. So such an orphan is NOT reclaimed within one
/// round; instead it waits until its generation ages past `keep` and the wholesale prefix-delete
/// reclaims the whole `gc/gen/<g>/` subtree — every attempt at once, including this orphan. This test
/// plants the orphan at a fold generation, ages that generation out, and asserts retention reclaims it.
TEST(CasGcSnapRetention, ReclaimsNonAdoptedCurrentGenAttemptViaRetention)
{
    auto backend = std::make_shared<InMemoryBackend>();
    /// keep=3 retention floor (matches WholesalePruneReclaimsAllAttemptsIncludingRetiredOutcomes).
    auto store = Store::open(backend, PoolConfig{.pool_prefix = "p", .server_root_id = "test", .root_shards = 1, .gc_snap_generations_to_keep = 3, .gc_fold_max_defer_rounds = 0});
    const RootNamespace ns{"00/aa@cas@"};
    const ManifestRef r = ref(1, 0xAA);
    writeBlobBody(*backend, store->layout(), DB::UInt128(1));
    writeManifestRaw(*backend, store->layout(), ns, r, {blobEntryFor("a", DB::UInt128(1))});
    publishCommittedTransition(*backend, store->layout(), ns, "tbl", std::nullopt, r);

    Gc gc(store, kGc);
    /// Drive a couple of rounds so snap_attempt is comfortably above 0 (a low orphan seq exists below it).
    ASSERT_TRUE(gc.runRegularRound().acquired_lease);
    ASSERT_TRUE(gc.runRegularRound().acquired_lease);
    const GcState st = readState(*backend, *store);
    ASSERT_GT(st.snap_attempt, 0u) << "need snap_attempt > 0 so a strictly-older orphan attempt exists";

    /// Plant a deposed competitor's debris at the next round's FOLD generation under an attempt strictly
    /// older than that round's adopted attempt — exactly the orphan the old per-round sweep targeted.
    const uint64_t orphan_gen = st.snap_generation + 1;
    const uint64_t orphan_attempt = st.snap_attempt - 1;
    const String orphan_seal = store->layout().foldSealKey(orphan_gen, orphan_attempt);
    const String orphan_run = store->layout().blobTargetRunKey(orphan_gen, orphan_attempt, 0, 0);
    backend->putIfAbsent(orphan_seal, "orphan-seal");
    backend->putIfAbsent(orphan_run, "orphan-run");

    /// One more round folds into `orphan_gen` and completes. The orphan must SURVIVE this round — there
    /// is no current-generation sweep; retention has not yet reached `orphan_gen`.
    ASSERT_TRUE(gc.runRegularRound().acquired_lease);
    EXPECT_TRUE(backend->head(orphan_seal).exists)
        << "orphan must survive its own round — there is no per-round current-gen sweep";

    /// Age `orphan_gen` well past the retention floor (keep=3): several more quiescent rounds. The
    /// wholesale generation-retention prune then reclaims the WHOLE `gc/gen/<orphan_gen>/` subtree,
    /// including this non-adopted attempt's debris.
    for (int i = 0; i < 8; ++i)
        ASSERT_TRUE(gc.runRegularRound().acquired_lease);

    const GcState st_after = readState(*backend, *store);
    ASSERT_GT(st_after.snap_generation, orphan_gen + 3) << "orphan_gen must be below the retention floor";

    EXPECT_FALSE(backend->head(orphan_seal).exists)
        << "non-adopted attempt orphan must be reclaimed by wholesale retention once its generation ages out";
    EXPECT_FALSE(backend->head(orphan_run).exists)
        << "the whole orphan subtree must be reclaimed by wholesale retention";

    /// No-loss: the live data is intact throughout.
    EXPECT_TRUE(blobExists(*backend, store->layout(), DB::UInt128(1)));
    EXPECT_TRUE(manifestExists(*backend, store->layout(), ManifestId{ns, r}));
}

/// ---- Task 7 (2026-07-02 snapshot-streaming): ref-aware retention + post-CAS hand-off delete ----

/// Retention must NOT reclaim a generation whose run the live seal still references, EVEN once the
/// retention cursor (`snap_pruned_through`) has advanced past that generation. With `keep=1` and a live
/// ref that idle-carries across generations, `pruneSupersededGenerations` SKIPS gen-1's prefix every
/// round while advancing the cursor over it. The gen-1 run object (physically holding the seal's ref)
/// must survive, and folding/in-degree resolution THROUGH the carried ref must keep working.
TEST(CasGcRetention, PruneRetainsLiveReferencedRun)
{
    auto backend = std::make_shared<InMemoryBackend>();
    /// keep=1: the retention floor is aggressive so the cursor reaches gen-1's neighbourhood fast.
    auto store = Store::open(backend, PoolConfig{.pool_prefix = "p", .server_root_id = "test", .root_shards = 1, .gc_snap_generations_to_keep = 1, .gc_fold_max_defer_rounds = 0});
    const RootNamespace ns{"00/aa@cas@"};
    const ManifestRef r = ref(1, 0xAA);

    writeBlobBody(*backend, store->layout(), DB::UInt128(1));
    writeManifestRaw(*backend, store->layout(), ns, r, {blobEntryFor("a", DB::UInt128(1))});
    publishCommittedTransition(*backend, store->layout(), ns, "tbl", std::nullopt, r);

    Gc gc(store, kGc);
    gc.runRegularRound();   // gen 1: the blob's run is sealed under gen-1's key namespace
    const GcState st1 = readState(*backend, *store);
    const uint64_t ref_gen = st1.snap_generation;

    /// The gen-1 seal's ref names gen-1's physical run key — capture it so we can assert the OBJECT
    /// (not just the generation number) survives retention.
    const auto seal1 = decodeFoldSeal(
        backend->get(store->layout().foldSealKey(st1.snap_generation, st1.snap_attempt))->bytes);
    ASSERT_EQ(seal1.blob_target_runs.size(), 1u);
    const String referenced_run_key = seal1.blob_target_runs.front().key;
    ASSERT_EQ(seal1.blob_target_runs.front().generation, ref_gen);
    ASSERT_TRUE(backend->head(referenced_run_key).exists);

    /// Several idle rounds: no delta, no retired => pure ref-carry. Each round advances the generation
    /// and, once adopted_generation > keep, drives the retention prune forward. gen-1 is referenced every
    /// round, so it is SKIPPED (retained) even as `snap_pruned_through` climbs past it.
    for (int i = 0; i < 6; ++i)
        ASSERT_TRUE(gc.runRegularRound().acquired_lease);

    const GcState st = readState(*backend, *store);
    /// The cursor has advanced strictly past the referenced generation (the retention prune SKIPPED it
    /// but still moved the high-water cursor forward) — this is the exact window Task 7 guards.
    ASSERT_GT(st.snap_pruned_through, ref_gen)
        << "the retention cursor must have advanced past the still-referenced generation";

    /// The referenced run object is STILL ALIVE despite the cursor passing its generation.
    EXPECT_TRUE(backend->head(referenced_run_key).exists)
        << "a run referenced by the live seal must be retained even after the cursor passes its generation";

    /// The current seal still references that same physical gen-1 object (carried, not reconstructed),
    /// and in-degree resolution THROUGH the carried ref still works.
    const auto seal_now = decodeFoldSeal(
        backend->get(store->layout().foldSealKey(st.snap_generation, st.snap_attempt))->bytes);
    ASSERT_EQ(seal_now.blob_target_runs.size(), 1u);
    EXPECT_EQ(seal_now.blob_target_runs.front().key, referenced_run_key);
    EXPECT_EQ(seal_now.blob_target_runs.front().generation, ref_gen);
    EXPECT_EQ(inDegreeOf(*backend, store->layout(), DB::UInt128(1)), 1)
        << "folding still resolves in-degree through the retained, carried parent ref";

    /// No-loss end-to-end: the live blob is intact.
    EXPECT_TRUE(blobExists(*backend, store->layout(), DB::UInt128(1)));
}

/// When a later delta finally REPLACES the carried ref with a fresh run, the superseded old-generation
/// run — whose generation the retention cursor already passed while it was retained — is reclaimed by the
/// post-CAS HAND-OFF delete in `runRegularRound` (the wholesale prune never revisits a generation behind
/// its cursor, so the ordinary prune would leak it). The whole `gc/gen/<old>/` prefix must be gone.
TEST(CasGcRetention, HandOffDeletesSupersededRef)
{
    auto backend = std::make_shared<InMemoryBackend>();
    auto store = Store::open(backend, PoolConfig{.pool_prefix = "p", .server_root_id = "test", .root_shards = 1, .gc_snap_generations_to_keep = 1, .gc_fold_max_defer_rounds = 0});
    const RootNamespace ns{"00/aa@cas@"};
    const ManifestRef r1 = ref(1, 0xAA);

    writeBlobBody(*backend, store->layout(), DB::UInt128(1));
    writeManifestRaw(*backend, store->layout(), ns, r1, {blobEntryFor("a", DB::UInt128(1))});
    publishCommittedTransition(*backend, store->layout(), ns, "tbl", std::nullopt, r1);

    Gc gc(store, kGc);
    gc.runRegularRound();   // gen 1: run sealed under gen-1
    const GcState st1 = readState(*backend, *store);
    const uint64_t old_gen = st1.snap_generation;
    const String old_prefix = store->layout().gcGenPrefix(old_gen);
    ASSERT_FALSE(backend->list(old_prefix, "", 1000).keys.empty()) << "gen-1 prefix must be populated";

    /// Idle-carry the gen-1 ref until the retention cursor has advanced strictly PAST gen-1. Until it
    /// does, a normal prune could still reclaim gen-1 when the ref moves — the hand-off is only load-
    /// bearing once gen-1 is BEHIND the cursor.
    for (int i = 0; i < 6; ++i)
        ASSERT_TRUE(gc.runRegularRound().acquired_lease);
    ASSERT_GT(readState(*backend, *store).snap_pruned_through, old_gen)
        << "gen-1 must be behind the retention cursor before the hand-off is exercised";
    /// gen-1 is retained (referenced) even though the cursor passed it.
    ASSERT_FALSE(backend->list(old_prefix, "", 1000).keys.empty())
        << "the referenced gen-1 prefix must still exist before the ref moves off it";

    /// A real delta: swap the ref to a new manifest naming a different blob. The next fold writes a FRESH
    /// run under the new generation and the seal's shard-0 ref moves OFF gen-1.
    const ManifestRef r2 = ref(2, 0xBB);
    writeBlobBody(*backend, store->layout(), DB::UInt128(2));
    writeManifestRaw(*backend, store->layout(), ns, r2, {blobEntryFor("b", DB::UInt128(2))});
    publishCommittedTransition(*backend, store->layout(), ns, "tbl", r1, r2);

    ASSERT_TRUE(gc.runRegularRound().acquired_lease);   // folds through the carried ref; ref leaves gen-1

    /// The seal no longer references gen-1 ...
    const GcState st_after = readState(*backend, *store);
    const auto seal_after = decodeFoldSeal(
        backend->get(store->layout().foldSealKey(st_after.snap_generation, st_after.snap_attempt))->bytes);
    for (const RunRef & rr : seal_after.blob_target_runs)
        EXPECT_NE(rr.generation, old_gen) << "the live seal must have moved its ref off gen-1";

    /// ... and the post-CAS hand-off delete reclaimed gen-1's WHOLE prefix (not just the single run
    /// object): seal, attempt subtree, run — all gone. The ordinary prune would have leaked it because its
    /// cursor is already past gen-1.
    const ListPage residue = backend->list(old_prefix, "", 1000);
    EXPECT_TRUE(residue.keys.empty())
        << "the superseded gen-1 prefix must be hand-off deleted; left " << residue.keys.size() << " objects";

    /// The now-referenced blob 2 is intact; folding through the fresh run resolves it.
    EXPECT_TRUE(blobExists(*backend, store->layout(), DB::UInt128(2)));
    EXPECT_EQ(inDegreeOf(*backend, store->layout(), DB::UInt128(2)), 1);
}

/// ---- B12 lazy/batched trim tests ----

/// B12: a shard with FEW (< gc_trim_min_events) trimmable events at/below the sealed fold cursor must
/// NOT be compacted by a regular round. Its journal retains those events after the round (they are
/// inert — at/below the sealed cursor; the next fold skips them via the per-shard cursor carried in the
/// completion seal). As a consequence the shard's root-shard token is UNCHANGED after the round (no
/// mutateShard bumped it). The next round therefore SKIPs the shard in the discover phase (the listed
/// token equals the sealed post-fence token from the prior round = no settling re-read is needed).
///
/// Setup: gc_trim_min_events=4 (the batch threshold); publish exactly 3 events, all below the sealed
/// fold cursor after the first round. Assert: (a) the 3 events survive the round in the journal,
/// (b) the shard token is NOT bumped by trim (the token after the round == the post-fence token stored
/// in the completion seal), and (c) discoverDecisionsForTest() returns Skip for the shard.
TEST(CasGcRound, LazyTrimSkipsSmallJournalAndKeepsTokenStable)
{
    auto backend = std::make_shared<InMemoryBackend>();
    /// gc_trim_min_events=4 and maintenance disabled; 3 events must not trigger trim.
    auto store = Store::open(backend, PoolConfig{.pool_prefix = "p", .server_root_id = "test", .root_shards = 1,
                                                 .gc_trim_min_events = 4});
    const RootNamespace ns{"00/aa@cas@"};

    /// Publish 3 distinct committed events so the journal has exactly 3 entries.
    /// All 3 have bodies so the fold advances the cursor past all of them (no barrier).
    const ManifestRef r1 = ref(1, 0xA1);
    const ManifestRef r2 = ref(2, 0xA2);
    const ManifestRef r3 = ref(3, 0xA3);
    writeBlobBody(*backend, store->layout(), DB::UInt128(1));
    writeBlobBody(*backend, store->layout(), DB::UInt128(2));
    writeBlobBody(*backend, store->layout(), DB::UInt128(3));
    writeManifestRaw(*backend, store->layout(), ns, r1, {blobEntryFor("a", DB::UInt128(1))});
    writeManifestRaw(*backend, store->layout(), ns, r2, {blobEntryFor("b", DB::UInt128(2))});
    writeManifestRaw(*backend, store->layout(), ns, r3, {blobEntryFor("c", DB::UInt128(3))});
    /// v1: publish ref1; v2: move ref1->ref2 (drop ref1, add ref2); v3: move ref2->ref3.
    publishCommittedTransition(*backend, store->layout(), ns, "tbl", std::nullopt, r1);
    publishCommittedTransition(*backend, store->layout(), ns, "tbl", r1, r2);
    publishCommittedTransition(*backend, store->layout(), ns, "tbl", r2, r3);

    Gc gc(store, kGc);
    /// One round: folds all 3 events (cursor = v3), then trim sees 3 trimmable events < 4 => SKIP.
    ASSERT_TRUE(gc.runRegularRound().acquired_lease);

    /// (a) All 3 events must remain in the journal (lazy trim did NOT compact them).
    const auto shard_bytes = backend->get(store->layout().rootShardKey(ns, 0));
    ASSERT_TRUE(shard_bytes.has_value()) << "root shard must exist after the round";
    const RootShard root_after = decodeRootShard(shard_bytes->bytes);
    EXPECT_EQ(root_after.journal.size(), 3u)
        << "lazy trim must NOT compact a journal with fewer than gc_trim_min_events trimmable events";

    /// (b) The shard token recorded in the FOLD seal (fold-time token — completion seals are a retired
    /// concept in the one-pass round) equals the CURRENT token of the shard object — i.e., trim did NOT
    /// mutate the shard and bump its token. Read the fold seal's per-shard folded_token for the shard.
    const GcState st = readState(*backend, *store);
    const auto fold_seal_obj = backend->get(store->layout().foldSealKey(st.snap_generation, st.snap_attempt));
    ASSERT_TRUE(fold_seal_obj.has_value()) << "fold seal must exist after a successful round";
    const CasFoldSeal seal = decodeFoldSeal(fold_seal_obj->bytes);
    const String ck = cursorKeyForTest(ns, 0);
    const auto cov_it = seal.per_ns_shard.find(ck);
    ASSERT_NE(cov_it, seal.per_ns_shard.end()) << "fold seal must carry coverage for ns/0";
    /// The stored fold-time token must equal the current shard token (no trim mutation).
    const auto current_head = backend->head(store->layout().rootShardKey(ns, 0));
    ASSERT_TRUE(current_head.exists);
    EXPECT_EQ(cov_it->second.folded_token, current_head.token)
        << "lazy trim must not bump the shard token; the fold-seal fold-time token "
           "must equal the live shard token (no settling re-read needed next round)";

    /// (c) The next round's discover step must Skip the shard (no settling re-read): the listed token
    /// equals the sealed post-fence token because trim did NOT mutate it.
    const auto decisions = gc.discoverDecisionsForTest();
    const auto dec_it = decisions.find(ck);
    ASSERT_NE(dec_it, decisions.end()) << "discover must produce a decision for ns/0";
    EXPECT_EQ(dec_it->second, Gc::DiscoverDecision::Skip)
        << "B12: lazy trim avoids bumping the shard token, so the next round's discover "
           "must Skip the shard (no settling re-read)";
}

/// B12: a shard with ENOUGH (>= gc_trim_min_events) trimmable events IS compacted by a regular round.
/// Also verifies the body-size soft-limit path: a shard whose encoded body is >= gc_trim_body_soft_limit
/// is always compacted even if it has fewer than gc_trim_min_events events.
/// The safety invariant (INV-JOURNAL-COVERAGE) is unchanged: trim only removes events AT OR BELOW
/// the sealed cursor — events above it survive (same guarantee as the pre-B12 eager trim).
TEST(CasGcRound, LazyTrimCompactsAtThresholdOrSoftLimit)
{
    auto backend = std::make_shared<InMemoryBackend>();
    /// Use gc_trim_min_events=3 and a tiny body-size soft limit (1 byte) to test both gates separately.
    /// For the event-count gate: publish 3 events so the count exactly meets the threshold.
    auto store = Store::open(backend, PoolConfig{.pool_prefix = "p", .server_root_id = "test", .root_shards = 1,
                                                 .gc_trim_min_events = 3,
                                                 .gc_trim_body_soft_limit = 1024ULL * 1024 * 1024}); /// 1 GiB: only event-count fires
    const RootNamespace ns{"00/aa@cas@"};

    /// Publish 3 committed events — exactly at the threshold.
    const ManifestRef r1 = ref(1, 0xC1);
    const ManifestRef r2 = ref(2, 0xC2);
    const ManifestRef r3 = ref(3, 0xC3);
    writeBlobBody(*backend, store->layout(), DB::UInt128(0x11));
    writeBlobBody(*backend, store->layout(), DB::UInt128(0x22));
    writeBlobBody(*backend, store->layout(), DB::UInt128(0x33));
    writeManifestRaw(*backend, store->layout(), ns, r1, {blobEntryFor("a", DB::UInt128(0x11))});
    writeManifestRaw(*backend, store->layout(), ns, r2, {blobEntryFor("b", DB::UInt128(0x22))});
    writeManifestRaw(*backend, store->layout(), ns, r3, {blobEntryFor("c", DB::UInt128(0x33))});
    [[maybe_unused]] const uint64_t v1 = publishCommittedTransition(*backend, store->layout(), ns, "t1", std::nullopt, r1);
    [[maybe_unused]] const uint64_t v2 = publishCommittedTransition(*backend, store->layout(), ns, "t2", std::nullopt, r2);
    const uint64_t v3 = publishCommittedTransition(*backend, store->layout(), ns, "t3", std::nullopt, r3);

    Gc gc(store, kGc);
    /// One round: folds all 3 events (cursor = v3), then trim sees 3 trimmable events >= 3 => COMPACT.
    ASSERT_TRUE(gc.runRegularRound().acquired_lease);

    /// The fold cursor must cover all 3 events (no barrier).
    const uint64_t cursor = foldCursorOf(*backend, store->layout(), ns, 0);
    EXPECT_EQ(cursor, v3) << "cursor must be sealed at v3 (all 3 events cleanly folded)";

    /// After the round, the journal must be EMPTY (all 3 events <= cursor were trimmed).
    const auto shard_bytes = backend->get(store->layout().rootShardKey(ns, 0));
    ASSERT_TRUE(shard_bytes.has_value());
    const RootShard root_after = decodeRootShard(shard_bytes->bytes);
    EXPECT_TRUE(root_after.journal.empty())
        << "B12: at threshold (>= gc_trim_min_events) the journal must be compacted";

    /// INV-JOURNAL-COVERAGE unchanged: now add a pre-cursor event above the cursor and verify a
    /// second round does NOT trim it. Publish a 4th event at v4 (above v3 = cursor after first round).
    const ManifestRef r4 = ref(4, 0xD4);
    writeBlobBody(*backend, store->layout(), DB::UInt128(0x44));
    writeManifestRaw(*backend, store->layout(), ns, r4, {blobEntryFor("d", DB::UInt128(0x44))});
    const uint64_t v4 = publishCommittedTransition(*backend, store->layout(), ns, "t4", std::nullopt, r4);
    ASSERT_GT(v4, v3);

    /// Second round: folds the new event (v4); trim now has 1 trimmable event (< 3) => SKIP per lazy rule.
    /// The v4 event (above the sealed cursor from round 1) must survive after round 2.
    ASSERT_TRUE(gc.runRegularRound().acquired_lease);

    const uint64_t cursor2 = foldCursorOf(*backend, store->layout(), ns, 0);
    EXPECT_EQ(cursor2, v4) << "cursor must advance to v4 after the second round";

    /// The journal must still contain the v4 event (now below cursor2, but count = 1 < threshold).
    const auto shard2 = backend->get(store->layout().rootShardKey(ns, 0));
    ASSERT_TRUE(shard2.has_value());
    const RootShard root2 = decodeRootShard(shard2->bytes);
    EXPECT_EQ(root2.journal.size(), 1u)
        << "B12: 1 trimmable event < gc_trim_min_events=3 must not be compacted (lazy skip)";

    /// INV-JOURNAL-COVERAGE: that surviving event must be AT OR BELOW the current cursor (it was just
    /// folded, so cursor2 = v4; the event at v4 is <= cursor2). No event above cursor2 exists.
    for (const RootOwnerEvent & e : root2.journal)
        EXPECT_LE(e.transition_version, cursor2)
            << "lazy trim: surviving event at v" << e.transition_version
            << " must be <= cursor2=" << cursor2 << " (below the sealed cursor — safe to leave)";

    /// Now test the body-size soft-limit gate: open a second store with a tiny soft-limit (1 byte)
    /// so the body size always triggers trim regardless of event count.
    auto backend2 = std::make_shared<InMemoryBackend>();
    auto store2 = Store::open(backend2, PoolConfig{.pool_prefix = "p", .server_root_id = "test", .root_shards = 1,
                                                    .gc_trim_min_events = 256,     /// large batch threshold — event count won't fire
                                                    .gc_trim_body_soft_limit = 1}); /// 1 byte: always over limit
    const RootNamespace ns2{"00/bb@cas@"};
    const ManifestRef s1 = ref(10, 0xE1);
    writeBlobBody(*backend2, store2->layout(), DB::UInt128(0x55));
    writeManifestRaw(*backend2, store2->layout(), ns2, s1, {blobEntryFor("e", DB::UInt128(0x55))});
    const uint64_t u1 = publishCommittedTransition(*backend2, store2->layout(), ns2, "tE", std::nullopt, s1);

    Gc gc2(store2, kGc);
    /// One round: 1 trimmable event < 256 (batch gate skips), but body >= 1 byte (soft-limit fires).
    ASSERT_TRUE(gc2.runRegularRound().acquired_lease);

    const auto shard3 = backend2->get(store2->layout().rootShardKey(ns2, 0));
    ASSERT_TRUE(shard3.has_value());
    const RootShard root3 = decodeRootShard(shard3->bytes);
    EXPECT_TRUE(root3.journal.empty())
        << "B12 soft-limit gate: body >= gc_trim_body_soft_limit must force compaction "
           "even when event count < gc_trim_min_events";
    (void)u1;
}

/// B12 maintenance mode: setMaintenanceTrimForTest(true) arms full compaction for one round,
/// bypassing both the event-count and soft-limit thresholds. After the round the flag resets to false
/// so subsequent rounds are lazy again.
TEST(CasGcRound, MaintenanceTrimCompactsEverythingOnce)
{
    auto backend = std::make_shared<InMemoryBackend>();
    /// Large thresholds so lazy trim never fires on its own.
    auto store = Store::open(backend, PoolConfig{.pool_prefix = "p", .server_root_id = "test", .root_shards = 1,
                                                 .gc_trim_min_events = 256,
                                                 .gc_trim_body_soft_limit = 1024ULL * 1024 * 1024,
                                                 .gc_fold_max_defer_rounds = 0});
    const RootNamespace ns{"00/cc@cas@"};
    const ManifestRef r1 = ref(1, 0xF1);
    writeBlobBody(*backend, store->layout(), DB::UInt128(0xAA));
    writeManifestRaw(*backend, store->layout(), ns, r1, {blobEntryFor("f", DB::UInt128(0xAA))});
    publishCommittedTransition(*backend, store->layout(), ns, "tF", std::nullopt, r1);

    Gc gc(store, kGc);
    /// Normal round: 1 event < 256 (lazy skip). Journal stays intact.
    ASSERT_TRUE(gc.runRegularRound().acquired_lease);

    {
        const auto shard = backend->get(store->layout().rootShardKey(ns, 0));
        ASSERT_TRUE(shard.has_value());
        EXPECT_EQ(decodeRootShard(shard->bytes).journal.size(), 1u)
            << "before maintenance: lazy skip leaves 1 trimmable event";
    }

    /// Arm maintenance mode and run one more round. Journal must be fully compacted.
    gc.setMaintenanceTrimForTest(true);
    ASSERT_TRUE(gc.runRegularRound().acquired_lease);

    {
        const auto shard = backend->get(store->layout().rootShardKey(ns, 0));
        ASSERT_TRUE(shard.has_value());
        EXPECT_TRUE(decodeRootShard(shard->bytes).journal.empty())
            << "maintenance round must fully compact the journal (bypass all lazy-trim thresholds)";
    }

    /// After the maintenance round the flag resets automatically: another round is lazy again.
    const ManifestRef r2 = ref(2, 0xF2);
    writeBlobBody(*backend, store->layout(), DB::UInt128(0xBB));
    writeManifestRaw(*backend, store->layout(), ns, r2, {blobEntryFor("g", DB::UInt128(0xBB))});
    publishCommittedTransition(*backend, store->layout(), ns, "tF", r1, r2);

    ASSERT_TRUE(gc.runRegularRound().acquired_lease);
    {
        const auto shard = backend->get(store->layout().rootShardKey(ns, 0));
        ASSERT_TRUE(shard.has_value());
        EXPECT_EQ(decodeRootShard(shard->bytes).journal.size(), 1u)
            << "after maintenance round flag auto-reset: next round is lazy (1 event < 256 => skip)";
    }
}

/// keep == 0 is the forensics "keep ALL" mode: NO generation is pruned, snap_pruned_through stays 0.
TEST(CasGcSnapRetention, KeepZeroPrunesNothing)
{
    auto backend = std::make_shared<InMemoryBackend>();
    auto store = Store::open(backend, PoolConfig{.pool_prefix = "p", .server_root_id = "test", .root_shards = 1, .gc_snap_generations_to_keep = 0});
    const RootNamespace ns{"00/aa@cas@"};
    const ManifestRef r = ref(1, 0xAA);

    writeBlobBody(*backend, store->layout(), DB::UInt128(1));
    writeManifestRaw(*backend, store->layout(), ns, r, {blobEntryFor("a", DB::UInt128(1))});
    publishCommittedTransition(*backend, store->layout(), ns, "tbl", std::nullopt, r);

    Gc gc(store, kGc);
    for (int i = 0; i < 6; ++i)
        ASSERT_TRUE(gc.runRegularRound().acquired_lease);

    const GcState st = readState(*backend, *store);
    EXPECT_EQ(st.snap_pruned_through, 0u) << "keep==0 must prune nothing";

    /// Every seal from generation 1 up to the current one remains. Each generation was sealed under the
    /// attempt of the round that produced it (attempt == that round's lease.seq, which bumps every round),
    /// so a historical generation's seal lives under an earlier attempt than the final snap_attempt — scan
    /// all attempts up to snap_attempt and require the seal to survive under one of them.
    for (uint64_t g = 1; g <= st.snap_generation; ++g)
    {
        bool seal_present = false;
        for (uint64_t a = 0; a <= st.snap_attempt && !seal_present; ++a)
            seal_present = backend->head(store->layout().foldSealKey(g, a)).exists;
        EXPECT_TRUE(seal_present) << "keep==0: seal of generation " << g << " must remain";
    }
}

TEST(CasGcRound, OrphanManifestCursorSweepDeletesAndPersistsCursor)
{
    std::shared_ptr<InMemoryBackend> backend;
    PoolConfig config;
    config.pool_prefix = "p";
    config.server_root_id = "test";
    config.manifest_sweep_list_budget_keys = 1;
    config.manifest_sweep_delete_budget_keys = 1;
    /// This test drives MANY consecutive rounds expecting each to sweep + persist the cursor; force
    /// fold-every-round (Phase-4 Lever A would otherwise defer once the pool quiesces).
    config.gc_fold_max_defer_rounds = 0;
    auto store = openTestStoreWithConfig(backend, config);

    const RootNamespace ns{"test/aa@cas@"};
    registerNamespaceRaw(*backend, store->layout(), ns);
    const ManifestRef r1 = ref(5, 0xCA01);
    const ManifestRef r2 = ref(5, 0xCA02);
    writeManifestRaw(*backend, store->layout(), ns, r1, {blobEntryFor("a", DB::UInt128(1))});
    writeManifestRaw(*backend, store->layout(), ns, r2, {blobEntryFor("b", DB::UInt128(2))});
    setWatermarkMinActive(*backend, store->layout(), "test", r1.writer_epoch, /*min_active*/6);

    Gc gc(store, kGc);
    ASSERT_TRUE(gc.runRegularRound().acquired_lease);
    const GcState after_first = readState(*backend, *store);
    EXPECT_FALSE(after_first.manifest_sweep_cursor.empty());

    const bool first_exists = manifestExists(*backend, store->layout(), ManifestId{ns, r1});
    const bool second_exists = manifestExists(*backend, store->layout(), ManifestId{ns, r2});
    EXPECT_NE(first_exists, second_exists);

    ASSERT_TRUE(gc.runRegularRound().acquired_lease);
    const GcState after_second = readState(*backend, *store);
    EXPECT_TRUE(after_second.manifest_sweep_cursor.empty());
    EXPECT_FALSE(manifestExists(*backend, store->layout(), ManifestId{ns, r1}));
    EXPECT_FALSE(manifestExists(*backend, store->layout(), ManifestId{ns, r2}));
}

/// Source-edge idempotency: re-folding the same blob activation does not double-count.
/// A blob activated twice from the SAME source edge (same ManifestId + path) has in-degree 1, not 2.
TEST(CasGcRound, FoldManifestEdgesEmitsOnePlusEdgePerBlob)
{
    auto backend = std::make_shared<InMemoryBackend>();
    auto store = openStoreForTest(backend);
    const RootNamespace ns{"00/aa@cas@"};

    const ManifestRef r = ref(1, 0xAA);
    writeBlobBody(*backend, store->layout(), DB::UInt128(1));
    writeManifestRaw(*backend, store->layout(), ns, r, {blobEntryFor("a", DB::UInt128(1))});
    publishCommittedTransition(*backend, store->layout(), ns, "tbl", std::nullopt, r);

    Gc gc(store, kGc);
    driveToFixpoint(*backend, store, gc);

    EXPECT_EQ(inDegreeOf(*backend, store->layout(), DB::UInt128(1)), 1)
        << "a single published manifest must contribute exactly one source edge per blob";
    EXPECT_TRUE(blobExists(*backend, store->layout(), DB::UInt128(1)))
        << "the blob must still exist (in-degree > 0)";
}

/// Re-fold of a removal is idempotent: the fold barrier + source-edge set model ensure that
/// folding the same removal twice (the H1b scenario) does NOT drive the in-degree below zero.
TEST(CasGcRound, ReFoldOfRemovalIsIdempotent)
{
    auto backend = std::make_shared<InMemoryBackend>();
    auto store = openStoreForTest(backend);
    const RootNamespace ns{"00/aa@cas@"};

    const ManifestRef r = ref(1, 0xAA);
    writeBlobBody(*backend, store->layout(), DB::UInt128(1));
    writeManifestRaw(*backend, store->layout(), ns, r, {blobEntryFor("a", DB::UInt128(1))});
    publishCommittedTransition(*backend, store->layout(), ns, "tbl", std::nullopt, r);

    Gc gc(store, kGc);
    driveToFixpoint(*backend, store, gc);
    EXPECT_EQ(inDegreeOf(*backend, store->layout(), DB::UInt128(1)), 1);

    /// Drop the ref and run to fixpoint. The blob should be reclaimed.
    dropRefTransition(*backend, store->layout(), ns, "tbl", r);
    EXPECT_NO_THROW(driveToFixpoint(*backend, store, gc))
        << "re-fold of a removal must be idempotent (source-edge set, never underflows)";

    EXPECT_FALSE(blobExists(*backend, store->layout(), DB::UInt128(1)))
        << "the blob must be reclaimed after the only reference is dropped";
    EXPECT_EQ(inDegreeOf(*backend, store->layout(), DB::UInt128(1)), 0);
}

/// Two distinct manifests referencing the same blob contribute TWO independent source edges.
/// Dropping one manifest leaves the other's edge intact (in-degree stays 1, blob is spared).
TEST(CasGcRound, TwoManifestsTwoSourceEdgesDropOneSpares)
{
    auto backend = std::make_shared<InMemoryBackend>();
    auto store = openStoreForTest(backend);
    const RootNamespace ns{"00/aa@cas@"};

    const ManifestRef r1 = ref(1, 0xAA);
    const ManifestRef r2 = ref(2, 0xBB);
    writeBlobBody(*backend, store->layout(), DB::UInt128(1));
    writeManifestRaw(*backend, store->layout(), ns, r1, {blobEntryFor("a", DB::UInt128(1))});
    writeManifestRaw(*backend, store->layout(), ns, r2, {blobEntryFor("a", DB::UInt128(1))});
    publishCommittedTransition(*backend, store->layout(), ns, "tbl1", std::nullopt, r1);
    publishCommittedTransition(*backend, store->layout(), ns, "tbl2", std::nullopt, r2);

    Gc gc(store, kGc);
    driveToFixpoint(*backend, store, gc);
    EXPECT_EQ(inDegreeOf(*backend, store->layout(), DB::UInt128(1)), 2)
        << "two distinct manifests referencing the same blob must each contribute one source edge";
    EXPECT_TRUE(blobExists(*backend, store->layout(), DB::UInt128(1)));

    /// Drop one of the two references; the other still pins the blob.
    dropRefTransition(*backend, store->layout(), ns, "tbl1", r1);
    driveToFixpoint(*backend, store, gc);
    EXPECT_EQ(inDegreeOf(*backend, store->layout(), DB::UInt128(1)), 1)
        << "after dropping one of two references the in-degree must be 1";
    EXPECT_TRUE(blobExists(*backend, store->layout(), DB::UInt128(1)))
        << "the blob must survive — the second reference still pins it";
}
