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

/// ROUND-LEVEL end-to-end GC tests over the root-local part-manifest model (spec rev. 15 §Round
/// Protocol: fold -> retire -> fence -> recheck -> exact-token delete -> trim).
///
/// This file is the survivor of the old snap/cascade-based `gtest_cas_gc_round.cpp`. The per-STEP
/// behaviours it used to cover have moved to the dedicated GC-core suites and are intentionally NOT
/// re-tested here:
///   - fold edge dispatch (committed/precommit/promote/removal +/-1, 404 clamp/anomaly, fold barrier,
///     ref-mismatch fail-closed) -> gtest_cas_gc_fold.cpp
///   - retire/fence/recheck (manifest body deferred delete, fence raises shard+registry, publish racing
///     the fence is spared, unreferenced blob exact-token delete) -> gtest_cas_gc_fence_recheck.cpp
///   - trim of folded owner events + idempotent resume -> gtest_cas_gc_resume.cpp
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
    return ManifestRef{.writer_instance_id = "srv-a:1", .build_sequence = seq, .manifest_instance_id = DB::UInt128(inst)};
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
    return Store::open(out_backend, PoolConfig{.pool_prefix = "p"});
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

/// Drive a Gc to fixpoint (no candidates/deletes/etc. produced by a round, OR no lease this round).
/// Returns the number of rounds that actually held the lease and did work. Bounded so a non-converging
/// core never hangs the test (it would fail downstream assertions instead).
size_t driveToFixpoint(Gc & gc)
{
    size_t working_rounds = 0;
    for (size_t r = 0; r < 64; ++r)
    {
        const RoundReport rep = gc.runRegularRound();
        if (!rep.acquired_lease)
            continue;
        if (rep.candidates == 0 && rep.deleted == 0 && rep.absent == 0
            && rep.replaced == 0 && rep.spared == 0)
            break;
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
    driveToFixpoint(gc);
    /// While live: the blob's in-degree is 1 and NOTHING is collected (no-loss).
    EXPECT_EQ(inDegreeOf(*backend, store->layout(), DB::UInt128(1)), 1);
    EXPECT_TRUE(blobExists(*backend, store->layout(), DB::UInt128(1)));
    EXPECT_TRUE(manifestExists(*backend, store->layout(), id));

    dropRefTransition(*backend, store->layout(), ns, "tbl", r);
    driveToFixpoint(gc);
    /// After drop + fixpoint: the blob's only edge is gone, the blob is collected, the owner-removed
    /// manifest body is collected, and the in-degree generation reflects zero (no-leak / no-dangle).
    EXPECT_EQ(inDegreeOf(*backend, store->layout(), DB::UInt128(1)), 0);
    EXPECT_FALSE(blobExists(*backend, store->layout(), DB::UInt128(1)));
    EXPECT_FALSE(manifestExists(*backend, store->layout(), id));

    /// Idempotent: re-running to fixpoint changes nothing and never throws.
    EXPECT_NO_THROW(driveToFixpoint(gc));
    EXPECT_FALSE(blobExists(*backend, store->layout(), DB::UInt128(1)));
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
    driveToFixpoint(gc);
    EXPECT_EQ(inDegreeOf(*backend, store->layout(), DB::UInt128(1)), 2);   /// two source edges
    EXPECT_TRUE(blobExists(*backend, store->layout(), DB::UInt128(1)));

    /// Drop the FIRST ref: in-degree falls to 1, blob STILL pinned by tbl2 (spared).
    dropRefTransition(*backend, store->layout(), ns, "tbl1", r1);
    driveToFixpoint(gc);
    EXPECT_EQ(inDegreeOf(*backend, store->layout(), DB::UInt128(1)), 1);
    EXPECT_TRUE(blobExists(*backend, store->layout(), DB::UInt128(1)))
        << "shared blob must survive while a second ref still names it";

    /// Drop the SECOND ref: in-degree reaches 0, blob is finally collected.
    dropRefTransition(*backend, store->layout(), ns, "tbl2", r2);
    driveToFixpoint(gc);
    EXPECT_EQ(inDegreeOf(*backend, store->layout(), DB::UInt128(1)), 0);
    EXPECT_FALSE(blobExists(*backend, store->layout(), DB::UInt128(1)));
}

/// Spare-on-recheck race, multi-blob discrimination: a drop condemns two blobs; in the SAME window
/// before the next round folds, one of them is re-referenced under a fresh ref. The round must collect
/// ONLY the genuinely-unreferenced blob and SPARE the re-referenced one (#14, recheck folds the racing
/// publish on top of the fold generation before deleting). The discriminating assertion: gone vs. spared
/// in one round.
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
    driveToFixpoint(gc);
    EXPECT_EQ(inDegreeOf(*backend, store->layout(), DB::UInt128(1)), 1);
    EXPECT_EQ(inDegreeOf(*backend, store->layout(), DB::UInt128(2)), 1);

    /// Repoint the ref from r1 to r2 in the same window: ONE event {old=committed(r1), new=committed(r2)}.
    /// The -1 (r1's body: blobs 1 AND 2) and +1 (r2's body: blob 1 only) net to in-degree 1 for blob 1
    /// (re-referenced => SPARED) and 0 for blob 2 (genuinely unreferenced => collected). (A separate drop
    /// THEN repoint would double-count the -1 on r1's blobs and drive blob 2 to -1 — an undercount the
    /// in-degree fold fails closed on.)
    writeManifestRaw(*backend, store->layout(), ns, r2, {blobEntryFor("a", DB::UInt128(1))});
    publishCommittedTransition(*backend, store->layout(), ns, "tbl", r1, r2);

    driveToFixpoint(gc);
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
    driveToFixpoint(gc);
    const uint64_t gen0 = currentGenerationOf(*backend, store->layout());

    /// At quiescence: a fresh round does NO work (no candidates/deletes/spares) and changes nothing.
    const RoundReport quiescent = gc.runRegularRound();
    EXPECT_TRUE(quiescent.acquired_lease);
    EXPECT_EQ(quiescent.candidates, 0u);
    EXPECT_EQ(quiescent.deleted, 0u);
    EXPECT_EQ(quiescent.spared, 0u);

    EXPECT_NO_THROW(driveToFixpoint(gc));
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
    EXPECT_NO_THROW(driveToFixpoint(gc2));
    EXPECT_NO_THROW(driveToFixpoint(gc1));   /// the revived stale leader backs off / duplicates harmlessly

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
///     gtest_cas_gc_fence_recheck.cpp and the retire-view suite.
///   - CasGcRecheck.{SparedWhenPublishRacesTheFence, ReplacedWhenResurrectionWins, AbsentWhenAlreadyGone}
///     — now CasGcRecheck.{PublishRacingFenceSparesBlob, UnreferencedBlobDeletedExactToken}.
///   - CasGcFence.* / CasGcDiscovery.UsesRegistryNotList — now
///     gtest_cas_gc_fence_recheck.cpp::CasGcFence.RaisesAllShardAndRegistryFence (+ helper
///     registerNamespaceRaw discovery is exercised by every fold test).
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
    auto store = Store::open(backend, PoolConfig{.pool_prefix = "p", .root_shards = 1, .gc_snap_generations_to_keep = 3});
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

    /// Every generation at or below the floor is fully gone (fold seal + completion seal absent).
    for (uint64_t g = 1; g <= floor; ++g)
    {
        EXPECT_FALSE(backend->head(store->layout().foldSealKey(g)).exists)
            << "fold seal of pruned generation " << g << " must be gone";
        EXPECT_FALSE(backend->head(store->layout().completionSealKey(g)).exists)
            << "completion seal of pruned generation " << g << " must be gone";
        EXPECT_FALSE(backend->head(store->layout().blobTargetRunKey(g, /*shard*/0, /*seq*/0)).exists)
            << "blob-target run of pruned generation " << g << " must be gone";
    }

    /// The latest seal at the current generation survives (the live in-degree view).
    EXPECT_TRUE(backend->head(store->layout().completionSealKey(st.snap_generation)).exists
                || backend->head(store->layout().foldSealKey(st.snap_generation)).exists)
        << "the current generation's seal must NOT be pruned";

    /// No-loss: the live blob and owner body are intact throughout retention pruning.
    EXPECT_TRUE(blobExists(*backend, store->layout(), DB::UInt128(1)));
    EXPECT_TRUE(manifestExists(*backend, store->layout(), ManifestId{ns, r}));
}

/// keep == 0 is the forensics "keep ALL" mode: NO generation is pruned, snap_pruned_through stays 0.
TEST(CasGcSnapRetention, KeepZeroPrunesNothing)
{
    auto backend = std::make_shared<InMemoryBackend>();
    auto store = Store::open(backend, PoolConfig{.pool_prefix = "p", .root_shards = 1, .gc_snap_generations_to_keep = 0});
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

    /// Every fold seal from generation 1 up to the current one remains.
    for (uint64_t g = 1; g <= st.snap_generation; ++g)
        EXPECT_TRUE(backend->head(store->layout().foldSealKey(g)).exists
                    || backend->head(store->layout().completionSealKey(g)).exists)
            << "keep==0: seal of generation " << g << " must remain";
}
