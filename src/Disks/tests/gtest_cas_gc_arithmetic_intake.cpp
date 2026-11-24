#include <gtest/gtest.h>

#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Formats/CasFoldSealFormat.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Formats/CasRefLogFormat.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Gc/CasGc.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Gc/CasGcScheduler.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasPool.h>
#include "cas_test_helpers.h"

#include <base/scope_guard.h>

#include <bit>
#include <chrono>
#include <condition_variable>
#include <limits>
#include <map>
#include <mutex>
#include <set>
#include <thread>

/// ARITHMETIC REF INTAKE (spec 2026-07-27 "ref chain complete cut" §5).
///
/// The GC fold used to walk the ids the round's LIST returned. That made the LIST a source of TRUTH
/// about which records exist, and an object store that omits a durable key from a listing -- observed
/// in production as the `0x1430c`/`0x1430d` shape -- silently skipped those records' owner edges and
/// then sealed a cursor ABOVE them, so their blobs looked unreferenced forever after.
///
/// Under INV-1 (per-namespace contiguous ids) the ids within one `(namespace, writer_epoch)` are dense
/// `1..T`, so the next record's id is COMPUTABLE: `cursor + 1`. The fold therefore steps by arithmetic
/// and reads each expected id by EXACT key (the per-record GET was always owed -- the round read every
/// record's body anyway). The listing is demoted to a HINT with two jobs: it says which namespaces
/// exist, and it supplies the witnesses that make an absent expected-next decidable:
///
///   * absent at `expected`, no listed id above it  => the namespace's frontier this round (normal end)
///   * absent at `expected`, a listed id above it   => IMPOSSIBLE under contiguity: the store is lying
///                                                     or a durable record was lost. Hold the namespace
///                                                     (classification `Clamped`), cursor unmoved.
///
/// Epochs are crossed ONLY by consuming the `EpochSeal` that closes an epoch (INV-2). The seal folds as
/// an applied no-op (probe B2: `produced=false`), and the next epoch's start is `{E', 1}` -- reached
/// through the `prev_epoch_seal` back-chain, never guessed from the hint, so an epoch the hint omits
/// entirely is still walked.
///
/// These tests drive REAL rounds over an in-memory pool whose LIST omits keys that are genuinely
/// present (`HintHoleBackend`) -- the production shape, reproduced.

using namespace DB::Cas;
using namespace DB::Cas::tests;

namespace
{

const UInt128 kGc = hexToU128("00000000000000000000000000000001");

/// The lying store is `HintHoleBackend` from `cas_test_helpers.h`: LIST permanently omits keys that
/// stay readable by exact key, so these tests exercise the intake's arithmetic walk rather than any
/// one `list` call.

/// The `RefCoverage` the newest fold seal recorded for `ns`'s opaque catalog life, or nullopt when the
/// seal has no entry for it. Scans downward from the adopted generation for the most recent fold seal,
/// mirroring `foldCursorOf`'s reasoning (a completed round's `gc/state` points at the recheck generation).
std::optional<RefCoverage> coverageOf(Backend & backend, const Layout & layout, const RootNamespace & ns)
{
    const uint64_t gen = currentGenerationOf(backend, layout);
    const uint64_t attempt = currentAttemptOf(backend, layout);
    const UInt128 life_id = catalogLifeIdForTest(backend, layout, ns);
    for (uint64_t g = gen; ; --g)
    {
        OperationForTest op(backend);
        if (const auto got = (*op).read(layout.foldSealKey(g, attempt), Retry::once()))
        {
            const CasFoldSeal seal = decodeFoldSeal(got->bytes);
            const auto it = seal.ref_lives.find(life_id);
            if (it == seal.ref_lives.end())
                return std::nullopt;
            return it->second.coverage;
        }
        if (g == 0)
            return std::nullopt;
    }
}

RefTxnId cursorOf(Backend & backend, const Layout & layout, const RootNamespace & ns)
{
    const auto cov = coverageOf(backend, layout, ns);
    return cov ? cov->last_folded_ref_id : RefTxnId{};
}

CoverageClass classificationOf(Backend & backend, const Layout & layout, const RootNamespace & ns)
{
    const auto cov = coverageOf(backend, layout, ns);
    return cov ? cov->classification : CoverageClass::Absent;
}

/// The `fold_ref_intake` phase metrics of the round `sched` runs -- the only place probe B1's two
/// numbers are observable.
std::map<String, UInt64> runRoundAndReadIntakeMetrics(const PoolPtr & store)
{
    std::vector<GcRoundLogRecord> rows;
    CasGcScheduler sched(store, std::chrono::seconds(1), "test::gc", "ca",
                         [&](const GcRoundLogRecord & r) { rows.push_back(r); });
    EXPECT_TRUE(sched.runOneRoundNow(GcRoundLogRecord::Trigger::Manual).acquired_lease);
    for (const GcRoundLogRecord & r : rows)
        if (r.event_type == GcRoundLogRecord::EventType::Phase && r.phase == "fold_ref_intake")
            return r.phase_metrics;
    return {};
}

}

/// ===================== THE BLOCKER, AS A UNIT TEST =====================
///
/// Five records, all durable and all readable by exact key; the hint omits the two in the MIDDLE.
/// Arithmetic intake never consults the hint for what to read next, so the omission is a non-event:
/// every record folds, every blob keeps its owner edge, and the cursor lands on the true tail.
///
/// Under listing-driven intake this test fails on the blobs, not on the cursor: the cursor still
/// reached `{1, 5}` (the last LISTED id) while the two hidden records' edges were never folded -- the
/// exact damage shape the production blocker caused, since a cursor sealed above an unfolded record can
/// never be re-read.
TEST(CASGCArithmeticIntake, HintOmittingMiddleRecordsFoldsThroughUnnoticed)
{
    auto backend = std::make_shared<HintHoleBackend>();
    auto store = openPoolForTest(backend);
    const Layout & layout = store->layout();
    const RootNamespace ns{"00/aa@cas@"};
    fixture::admitLive(*backend, store->layout(), ns);   /// Stage B (Task 4-C): pin to the sentinel before the first real touch

    for (uint64_t i = 1; i <= 5; ++i)
        publishAt(*backend, layout, ns, RefTxnId{1, i}, "ref_" + std::to_string(i), i,
                  DB::UInt128(i), /*birth=*/i == 1);
    writeRecoverableCkptForRawFixture(*backend, layout, ns, RefCkpt{
        .life_epoch = 1,
        .committed_through = RefTxnId{1, 5},
        .checkpoint_snapshot_id = std::nullopt,
        .last_epoch_seal = std::nullopt,
    });

    backend->hide(layout.refLogKey(fixture::fixtureLife(ns), RefTxnId{1, 3}));
    backend->hide(layout.refLogKey(fixture::fixtureLife(ns), RefTxnId{1, 4}));

    Gc gc(store, kGc);
    ASSERT_TRUE(gc.runRegularRound().acquired_lease);
    ASSERT_GT(backend->holesServed(), 0u) << "the hint hole was never actually served";

    EXPECT_EQ(cursorOf(*backend, layout, ns), (RefTxnId{1, 5}));
    EXPECT_EQ(classificationOf(*backend, layout, ns), CoverageClass::Folded) << "a folded namespace is `changed`";
    for (uint64_t i = 1; i <= 5; ++i)
        EXPECT_EQ(inDegreeOf(*backend, layout, DB::UInt128(i)), 1)
            << "blob " << i << " lost its owner edge: its record was skipped because the hint omitted it";
}

/// A clean, hole-free namespace still ends its walk exactly where it should: at the first absent id,
/// with no witness above it and therefore no hold.
TEST(CASGCArithmeticIntake, WalkEndsAtFrontierWithoutHold)
{
    auto backend = std::make_shared<InMemoryBackend>();
    /// Fold every round: the default defer window would skip the second round entirely and leave the
    /// first round's seal in place, so the assertion below would read a stale coverage record.
    auto store = openPoolForTest(backend, /*gc_fold_max_defer_rounds=*/0);
    const Layout & layout = store->layout();
    const RootNamespace ns{"00/aa@cas@"};

    for (uint64_t i = 1; i <= 3; ++i)
        publishAt(*backend, layout, ns, RefTxnId{1, i}, "ref_" + std::to_string(i), i,
                  DB::UInt128(i), /*birth=*/i == 1);
    writeRecoverableCkptForRawFixture(*backend, layout, ns, RefCkpt{
        .life_epoch = 1,
        .committed_through = RefTxnId{1, 3},
        .checkpoint_snapshot_id = std::nullopt,
        .last_epoch_seal = std::nullopt,
    });

    Gc gc(store, kGc);
    ASSERT_TRUE(gc.runRegularRound().acquired_lease);

    EXPECT_EQ(cursorOf(*backend, layout, ns), (RefTxnId{1, 3}));
    EXPECT_EQ(classificationOf(*backend, layout, ns), CoverageClass::Folded);

    /// A second round over an unchanged namespace pays exactly one exact GET, finds the same frontier,
    /// and neither advances nor holds.
    ASSERT_TRUE(gc.runRegularRound().acquired_lease);
    EXPECT_EQ(cursorOf(*backend, layout, ns), (RefTxnId{1, 3}));
    EXPECT_EQ(classificationOf(*backend, layout, ns), CoverageClass::Unchanged) << "an unchanged namespace is `carried`";
}

/// ===================== EPOCHS ARE CROSSED ONLY BY CONSUMING A SEAL =====================
///
/// `{1,1} {1,2} seal{1,3} | {2,1} {2,2}`: the seal is applied as a table no-op, counted applied, and
/// the walk continues at `{2, 1}` -- whose `prev_epoch_seal` names the seal just consumed, which is what
/// makes the crossing provable rather than guessed.
TEST(CASGCArithmeticIntake, SealCrossesEpochAndIsAppliedAsNoOp)
{
    auto backend = std::make_shared<HintHoleBackend>();
    auto store = openPoolForTest(backend);
    const Layout & layout = store->layout();
    const RootNamespace ns{"00/aa@cas@"};
    fixture::admitLive(*backend, store->layout(), ns);   /// Stage B (Task 4-C): pin to the sentinel before the first real touch

    publishAt(*backend, layout, ns, RefTxnId{1, 1}, "ref_1", 1, DB::UInt128(1), /*birth=*/true);
    publishAt(*backend, layout, ns, RefTxnId{1, 2}, "ref_2", 2, DB::UInt128(2));
    writeSealAt(*backend, layout, ns, RefTxnId{1, 3});
    publishAt(*backend, layout, ns, RefTxnId{2, 1}, "ref_3", 3, DB::UInt128(3),
              /*birth=*/false, /*prev_epoch_seal=*/RefTxnId{1, 3});
    publishAt(*backend, layout, ns, RefTxnId{2, 2}, "ref_4", 4, DB::UInt128(4));
    writeRecoverableCkptForRawFixture(*backend, layout, ns, RefCkpt{
        .life_epoch = 1,
        .committed_through = RefTxnId{2, 2},
        .checkpoint_snapshot_id = std::nullopt,
        .last_epoch_seal = RefTxnId{1, 3},
    });

    /// The hint hides the seal AND the new epoch's first record: neither the epoch boundary nor its
    /// start may depend on the listing.
    backend->hide(layout.refLogKey(fixture::fixtureLife(ns), RefTxnId{1, 3}));
    backend->hide(layout.refLogKey(fixture::fixtureLife(ns), RefTxnId{2, 1}));

    Gc gc(store, kGc);
    ASSERT_TRUE(gc.runRegularRound().acquired_lease);
    ASSERT_GT(backend->holesServed(), 0u);

    EXPECT_EQ(cursorOf(*backend, layout, ns), (RefTxnId{2, 2}));
    EXPECT_EQ(classificationOf(*backend, layout, ns), CoverageClass::Folded);
    for (uint64_t i = 1; i <= 4; ++i)
        EXPECT_EQ(inDegreeOf(*backend, layout, DB::UInt128(i)), 1) << "blob " << i;
}

/// Two chained seals in ONE round, the middle epoch entirely EMPTY (its seal is its only record, at
/// sequence 1, carrying `prev_epoch_seal`). The hint omits that whole epoch, so the only way to reach it
/// is the back-chain: the record the hint DOES show names the seal that must be consumed first.
TEST(CASGCArithmeticIntake, ChainedEmptyEpochSealsBothConsumedInOneRound)
{
    auto backend = std::make_shared<HintHoleBackend>();
    auto store = openPoolForTest(backend, /*gc_fold_max_defer_rounds=*/0);
    const Layout & layout = store->layout();
    const RootNamespace ns{"00/aa@cas@"};
    fixture::admitLive(*backend, store->layout(), ns);   /// Stage B (Task 4-C): pin to the sentinel before the first real touch

    publishAt(*backend, layout, ns, RefTxnId{1, 1}, "ref_1", 1, DB::UInt128(1), /*birth=*/true);
    writeSealAt(*backend, layout, ns, RefTxnId{1, 2});
    writeSealAt(*backend, layout, ns, RefTxnId{2, 1}, /*prev_epoch_seal=*/RefTxnId{1, 2});
    publishAt(*backend, layout, ns, RefTxnId{3, 1}, "ref_2", 2, DB::UInt128(2),
              /*birth=*/false, /*prev_epoch_seal=*/RefTxnId{2, 1});
    writeRecoverableCkptForRawFixture(*backend, layout, ns, RefCkpt{
        .life_epoch = 1,
        .committed_through = RefTxnId{3, 1},
        .checkpoint_snapshot_id = std::nullopt,
        .last_epoch_seal = RefTxnId{2, 1},
    });

    backend->hide(layout.refLogKey(fixture::fixtureLife(ns), RefTxnId{2, 1}));

    const auto intake = runRoundAndReadIntakeMetrics(store);
    ASSERT_FALSE(intake.empty()) << "no fold_ref_intake row";
    ASSERT_GT(backend->holesServed(), 0u);

    EXPECT_EQ(cursorOf(*backend, layout, ns), (RefTxnId{3, 1}));
    EXPECT_EQ(inDegreeOf(*backend, layout, DB::UInt128(1)), 1);
    EXPECT_EQ(inDegreeOf(*backend, layout, DB::UInt128(2)), 1);

    /// THE ASSERTION THAT MAKES THIS TEST ABOUT THE CHAIN. Every check above is also satisfied by an
    /// id-ordered walk over the listed keys. Two crossings can only happen by consuming `seal{1,2}` and
    /// then `seal{2,1}`, and the hint never mentions `{2,1}`: only the seal's successor leads there.
    EXPECT_EQ(intake.at("epoch_crossings"), 2u)
        << "the walk must cross TWICE, through a hidden epoch it can only reach by the seal chain";
    EXPECT_EQ(intake.at("logs_accounted"), intake.at("logs_applied"));
    EXPECT_EQ(intake.at("logs_applied"), 4u) << "two records and two seals, all applied";
    /// One absent probe, past `{3,1}`, proves the frontier.
    EXPECT_EQ(intake.at("absent_probes"), 1u);
}

/// A round that ends ON a seal (nothing above it yet) leaves the cursor there. The NEXT round must
/// still cross into the epoch that appears later -- the cursor sitting on a closed epoch's seal is the
/// ordinary steady state after a writer-epoch change, not a wedge.
TEST(CASGCArithmeticIntake, CursorRestingOnSealCrossesInALaterRound)
{
    auto backend = std::make_shared<InMemoryBackend>();
    auto store = openPoolForTest(backend, /*gc_fold_max_defer_rounds=*/0);
    const Layout & layout = store->layout();
    const RootNamespace ns{"00/aa@cas@"};

    publishAt(*backend, layout, ns, RefTxnId{1, 1}, "ref_1", 1, DB::UInt128(1), /*birth=*/true);
    writeSealAt(*backend, layout, ns, RefTxnId{1, 2});
    writeRecoverableCkptForRawFixture(*backend, layout, ns, RefCkpt{
        .life_epoch = 1,
        .committed_through = RefTxnId{1, 2},
        .checkpoint_snapshot_id = std::nullopt,
        .last_epoch_seal = RefTxnId{1, 2},
    });

    Gc gc(store, kGc);
    ASSERT_TRUE(gc.runRegularRound().acquired_lease);
    ASSERT_EQ(cursorOf(*backend, layout, ns), (RefTxnId{1, 2})) << "the round consumed the seal";

    publishAt(*backend, layout, ns, RefTxnId{2, 1}, "ref_2", 2, DB::UInt128(2),
              /*birth=*/false, /*prev_epoch_seal=*/RefTxnId{1, 2});
    advanceRecoverableCkptForRawFixture(*backend, layout, ns, RefTxnId{2, 1});

    ASSERT_TRUE(gc.runRegularRound().acquired_lease);
    EXPECT_EQ(cursorOf(*backend, layout, ns), (RefTxnId{2, 1}));
    EXPECT_EQ(classificationOf(*backend, layout, ns), CoverageClass::Folded);
    EXPECT_EQ(inDegreeOf(*backend, layout, DB::UInt128(2)), 1);
}

/// ===================== IMPOSSIBLE SHAPES HOLD THE NAMESPACE =====================
///
/// `{1,3}` is genuinely absent while `{1,4}` is present AND listed. Contiguity says that cannot happen,
/// so whatever sits behind the gap may be an acked `+1`: the namespace is held at classification
/// `Clamped` with its cursor UNMOVED, rather than sealing past the gap.
///
/// Listing-driven intake folded `{1,4}` and sealed the cursor at it -- permanently, since a record below
/// the cursor is never re-read.
TEST(CASGCArithmeticIntake, GapBelowWitnessHoldsNamespaceAtClampedClassification)
{
    auto backend = std::make_shared<InMemoryBackend>();
    auto store = openPoolForTest(backend);
    const Layout & layout = store->layout();
    const RootNamespace ns{"00/aa@cas@"};

    publishAt(*backend, layout, ns, RefTxnId{1, 1}, "ref_1", 1, DB::UInt128(1), /*birth=*/true);
    publishAt(*backend, layout, ns, RefTxnId{1, 2}, "ref_2", 2, DB::UInt128(2));
    /// {1,3} is never written -- the record that vanished.
    publishAt(*backend, layout, ns, RefTxnId{1, 4}, "ref_4", 4, DB::UInt128(4));
    writeRecoverableCkptForRawFixture(*backend, layout, ns, RefCkpt{
        .life_epoch = 1,
        .committed_through = RefTxnId{1, 4},
        .checkpoint_snapshot_id = std::nullopt,
        .last_epoch_seal = std::nullopt,
    });

    Gc gc(store, kGc);
    ASSERT_TRUE(gc.runRegularRound().acquired_lease);

    EXPECT_EQ(cursorOf(*backend, layout, ns), (RefTxnId{1, 2}))
        << "the cursor must not advance past a gap";
    EXPECT_EQ(classificationOf(*backend, layout, ns), CoverageClass::Clamped);
    EXPECT_EQ(inDegreeOf(*backend, layout, DB::UInt128(4)), 0)
        << "the record above the gap was not folded";
}

/// A record of a LATER epoch reachable while the current epoch's seal was never consumed: the crossing
/// has no proof (the later epoch's `prev_epoch_seal` names a seal this cursor never reached), so the
/// namespace holds instead of jumping the boundary.
TEST(CASGCArithmeticIntake, UnconsumedSealCrossingHoldsNamespace)
{
    auto backend = std::make_shared<InMemoryBackend>();
    auto store = openPoolForTest(backend);
    const Layout & layout = store->layout();
    const RootNamespace ns{"00/aa@cas@"};

    publishAt(*backend, layout, ns, RefTxnId{1, 1}, "ref_1", 1, DB::UInt128(1), /*birth=*/true);
    /// Epoch 1's missing `{1,2}` seal is the exact position epoch 2 claims to chain from. With no
    /// same-epoch witness above it, the later epoch is the only witness and the crossing must hold.
    publishAt(*backend, layout, ns, RefTxnId{2, 1}, "ref_2", 2, DB::UInt128(2),
              /*birth=*/false, /*prev_epoch_seal=*/RefTxnId{1, 2});
    writeRecoverableCkptForRawFixture(*backend, layout, ns, RefCkpt{
        .life_epoch = 1,
        .committed_through = RefTxnId{2, 1},
        .checkpoint_snapshot_id = std::nullopt,
        .last_epoch_seal = RefTxnId{1, 2},
    });

    Gc gc(store, kGc);
    ASSERT_TRUE(gc.runRegularRound().acquired_lease);

    EXPECT_EQ(cursorOf(*backend, layout, ns), (RefTxnId{1, 1}));
    EXPECT_EQ(classificationOf(*backend, layout, ns), CoverageClass::Clamped);
    const auto coverage = coverageOf(*backend, layout, ns);
    ASSERT_TRUE(coverage && coverage->hold.has_value());
    EXPECT_EQ(coverage->hold->reason, HoldReason::UnconsumedSealCrossing);
    EXPECT_EQ(coverage->hold->offending_position, (RefTxnId{1, 2}));
    EXPECT_EQ(inDegreeOf(*backend, layout, DB::UInt128(2)), 0);
}

/// The back-chain proves the IDENTITY of the position an epoch chains from; it does not, by itself,
/// prove that position is a SEAL. Here a writer names an ordinary record (`{1,2}`, epoch 1's last
/// record, never sealed) as `{2,1}`'s `prev_epoch_seal`. Identity matches, so a chain-only check would
/// grant the crossing and declare epoch 1 closed while its writer may still be appending -- any later
/// `{1,k}` would then land permanently below the cursor, which is exactly the damage the seal exists to
/// prevent. The walk applied that record itself this round, so it knows its kind for free: refuse.
TEST(CASGCArithmeticIntake, CrossingFromANonSealRecordIsRefusedEvenWhenTheChainMatches)
{
    auto backend = std::make_shared<InMemoryBackend>();
    auto store = openPoolForTest(backend, /*gc_fold_max_defer_rounds=*/0);
    const Layout & layout = store->layout();
    const RootNamespace ns{"00/aa@cas@"};

    publishAt(*backend, layout, ns, RefTxnId{1, 1}, "ref_1", 1, DB::UInt128(1), /*birth=*/true);
    publishAt(*backend, layout, ns, RefTxnId{1, 2}, "ref_2", 2, DB::UInt128(2));
    /// `{1,2}` is an ordinary published record, NOT an `EpochSeal` -- and epoch 2 chains to it anyway.
    publishAt(*backend, layout, ns, RefTxnId{2, 1}, "ref_3", 3, DB::UInt128(3),
              /*birth=*/false, /*prev_epoch_seal=*/RefTxnId{1, 2});
    writeRecoverableCkptForRawFixture(*backend, layout, ns, RefCkpt{
        .life_epoch = 1,
        .committed_through = RefTxnId{2, 1},
        .checkpoint_snapshot_id = std::nullopt,
        .last_epoch_seal = RefTxnId{1, 2},
    });

    Gc gc(store, kGc);
    ASSERT_TRUE(gc.runRegularRound().acquired_lease);

    EXPECT_EQ(cursorOf(*backend, layout, ns), (RefTxnId{1, 2}))
        << "epoch 1 was never sealed, so the cursor may not leave it";
    EXPECT_EQ(classificationOf(*backend, layout, ns), CoverageClass::Clamped);
    EXPECT_EQ(inDegreeOf(*backend, layout, DB::UInt128(1)), 1) << "epoch 1's records still fold";
    EXPECT_EQ(inDegreeOf(*backend, layout, DB::UInt128(2)), 1);
    EXPECT_EQ(inDegreeOf(*backend, layout, DB::UInt128(3)), 0)
        << "the record beyond the unsealed boundary must NOT be folded";
}

/// The walk reads each key once and takes that answer as final. An epoch-start record that answers its
/// first read and not the next is folded on the first answer; a walk that read it again would stop at it.
TEST(CASGCArithmeticIntake, EpochStartThatAnswersOnlyEveryOtherReadIsFoldedOnItsFirstAnswer)
{
    /// Answers `flaky` on odd-numbered reads and 404s on even ones. Nothing else is disturbed.
    class AlternatingGetBackend : public InMemoryBackend
    {
    public:
        String flaky;
        size_t reads = 0;

        std::optional<Raw> read(const String & key, TransportAccess & access) override
        {
            if (key == flaky && ++reads % 2 == 0)
                return std::nullopt;
            return InMemoryBackend::read(key, access);
        }
    };

    auto backend = std::make_shared<AlternatingGetBackend>();
    auto store = openPoolForTest(backend, /*gc_fold_max_defer_rounds=*/0);
    const Layout & layout = store->layout();
    const RootNamespace ns{"00/aa@cas@"};
    fixture::admitLive(*backend, store->layout(), ns);   /// Stage B (Task 4-C): pin to the sentinel before the first real touch

    publishAt(*backend, layout, ns, RefTxnId{1, 1}, "ref_1", 1, DB::UInt128(1), /*birth=*/true);
    writeSealAt(*backend, layout, ns, RefTxnId{1, 2});
    publishAt(*backend, layout, ns, RefTxnId{2, 1}, "ref_2", 2, DB::UInt128(2),
              /*birth=*/false, /*prev_epoch_seal=*/RefTxnId{1, 2});
    writeSealAt(*backend, layout, ns, RefTxnId{2, 2});
    publishAt(*backend, layout, ns, RefTxnId{3, 1}, "ref_3", 3, DB::UInt128(3),
              /*birth=*/false, /*prev_epoch_seal=*/RefTxnId{2, 2});
    writeRecoverableCkptForRawFixture(*backend, layout, ns, RefCkpt{
        .life_epoch = 1,
        .committed_through = RefTxnId{3, 1},
        .checkpoint_snapshot_id = std::nullopt,
        .last_epoch_seal = RefTxnId{2, 2},
    });

    /// Arm only after seeding, so the fixture's own writes are undisturbed and the read counter starts
    /// at the round's first read of this key.
    backend->flaky = layout.refLogKey(fixture::fixtureLife(ns), RefTxnId{2, 1});

    Gc gc(store, kGc);
    ASSERT_TRUE(gc.runRegularRound().acquired_lease);
    EXPECT_EQ(backend->reads, 1u) << "the round reads the unstable record exactly once";

    EXPECT_EQ(cursorOf(*backend, layout, ns), (RefTxnId{3, 1}));
    EXPECT_NE(classificationOf(*backend, layout, ns), CoverageClass::Clamped);
    EXPECT_EQ(inDegreeOf(*backend, layout, DB::UInt128(2)), 1) << "the record was folded on its first answer";
    EXPECT_EQ(inDegreeOf(*backend, layout, DB::UInt128(3)), 1);
}

/// ===================== A PER-NAMESPACE FAILURE IS NOT A ROUND FAILURE =====================
///
/// Spec §5 narrows the whole-round abort to a key that cannot be attributed to any namespace. An
/// undecodable BODY belongs to exactly one namespace, so it clamps that namespace and nothing else:
/// `ns_a` holds at its last good record while `ns_b` folds and seals normally in the same round.
TEST(CASGCArithmeticIntake, CorruptBodyClampsOneNamespaceWhileAnotherFolds)
{
    auto backend = std::make_shared<InMemoryBackend>();
    auto store = openPoolForTest(backend);
    const Layout & layout = store->layout();
    const RootNamespace ns_a{"00/aa@cas@"};
    fixture::admitLive(*backend, store->layout(), ns_a);   /// Stage B (Task 4-C): pin to the sentinel before the first real touch
    const RootNamespace ns_b{"00/bb@cas@"};

    publishAt(*backend, layout, ns_a, RefTxnId{1, 1}, "ref_1", 1, DB::UInt128(1), /*birth=*/true);
    {
        OperationForTest op(*backend);
        (*op).create(layout.refLogKey(fixture::fixtureLife(ns_a), RefTxnId{1, 2}), "this is not a cas_ref_log object", Retry::once());
    }
    writeRecoverableCkptForRawFixture(*backend, layout, ns_a, RefCkpt{
        .life_epoch = 1,
        .committed_through = RefTxnId{1, 2},
        .checkpoint_snapshot_id = std::nullopt,
        .last_epoch_seal = std::nullopt,
    });

    publishAt(*backend, layout, ns_b, RefTxnId{1, 1}, "ref_1", 11, DB::UInt128(11), /*birth=*/true);
    publishAt(*backend, layout, ns_b, RefTxnId{1, 2}, "ref_2", 12, DB::UInt128(12));
    writeSealAt(*backend, layout, ns_b, RefTxnId{1, 3});
    writeRecoverableCkptForRawFixture(*backend, layout, ns_b, RefCkpt{
        .life_epoch = 1,
        .committed_through = RefTxnId{1, 3},
        .checkpoint_snapshot_id = std::nullopt,
        .last_epoch_seal = RefTxnId{1, 3},
    });

    Gc gc(store, kGc);
    ASSERT_TRUE(gc.runRegularRound().acquired_lease);

    EXPECT_EQ(cursorOf(*backend, layout, ns_a), (RefTxnId{1, 1}));
    EXPECT_EQ(classificationOf(*backend, layout, ns_a), CoverageClass::Clamped);

    EXPECT_EQ(cursorOf(*backend, layout, ns_b), (RefTxnId{1, 3}))
        << "a sibling namespace's corrupt body must not stop this one";
    EXPECT_EQ(classificationOf(*backend, layout, ns_b), CoverageClass::Folded);
    EXPECT_EQ(inDegreeOf(*backend, layout, DB::UInt128(11)), 1);
    EXPECT_EQ(inDegreeOf(*backend, layout, DB::UInt128(12)), 1);
}

/// ===================== PROBE B1 OVER AN ARITHMETIC CUT =====================
///
/// `logs_accounted` is recomputed from the SEALED cut -- the arithmetic distance the round's cursors
/// claim to cover -- and compared with the count the walk incremented once per applied record. The two
/// can only differ if a cursor moved over a position nothing applied, which is precisely the damage
/// listing-driven intake used to do silently. The identity must survive both a hint hole (positions
/// applied that the listing never mentioned) and a seal crossing (an applied no-op, and a cut that
/// spans two epochs).
TEST(CASGCArithmeticIntake, B1IdentityHoldsOverAHoleyCutThatCrossesASeal)
{
    auto backend = std::make_shared<HintHoleBackend>();
    auto store = openPoolForTest(backend, /*gc_fold_max_defer_rounds=*/0);
    const Layout & layout = store->layout();
    const RootNamespace ns{"00/aa@cas@"};
    fixture::admitLive(*backend, store->layout(), ns);   /// Stage B (Task 4-C): pin to the sentinel before the first real touch

    publishAt(*backend, layout, ns, RefTxnId{1, 1}, "ref_1", 1, DB::UInt128(1), /*birth=*/true);
    publishAt(*backend, layout, ns, RefTxnId{1, 2}, "ref_2", 2, DB::UInt128(2));
    writeSealAt(*backend, layout, ns, RefTxnId{1, 3});
    publishAt(*backend, layout, ns, RefTxnId{2, 1}, "ref_3", 3, DB::UInt128(3),
              /*birth=*/false, /*prev_epoch_seal=*/RefTxnId{1, 3});
    publishAt(*backend, layout, ns, RefTxnId{2, 2}, "ref_4", 4, DB::UInt128(4));
    writeRecoverableCkptForRawFixture(*backend, layout, ns, RefCkpt{
        .life_epoch = 1,
        .committed_through = RefTxnId{2, 2},
        .checkpoint_snapshot_id = std::nullopt,
        .last_epoch_seal = RefTxnId{1, 3},
    });

    backend->hide(layout.refLogKey(fixture::fixtureLife(ns), RefTxnId{1, 2}));

    const auto intake = runRoundAndReadIntakeMetrics(store);
    ASSERT_FALSE(intake.empty()) << "no fold_ref_intake row";
    ASSERT_GT(backend->holesServed(), 0u);

    EXPECT_EQ(intake.at("logs_accounted"), intake.at("logs_applied"));
    EXPECT_EQ(intake.at("logs_applied"), 5u)
        << "four records and the seal: the seal is APPLIED, as a no-op";
    EXPECT_EQ(cursorOf(*backend, layout, ns), (RefTxnId{2, 2}));
}

/// A namespace the hint omits ENTIRELY still folds through the checkpoint's authoritative frontier.
/// Every log key remains readable by exact key, and the cursor reaches `{1,3}` despite the empty hint.
/// When the hint reappears, it changes neither the cursor nor the owner edges.
TEST(CASGCArithmeticIntake, WhollyOmittedNamespaceFoldsThroughAuthoritativeCheckpoint)
{
    auto backend = std::make_shared<HintHoleBackend>();
    auto store = openPoolForTest(backend, /*gc_fold_max_defer_rounds=*/0);
    const Layout & layout = store->layout();
    const RootNamespace ns{"00/aa@cas@"};

    for (uint64_t i = 1; i <= 3; ++i)
        publishAt(*backend, layout, ns, RefTxnId{1, i}, "ref_" + std::to_string(i), i,
                  DB::UInt128(i), /*birth=*/i == 1);
    writeRecoverableCkptForRawFixture(*backend, layout, ns, RefCkpt{
        .life_epoch = 1,
        .committed_through = RefTxnId{1, 3},
        .checkpoint_snapshot_id = std::nullopt,
        .last_epoch_seal = std::nullopt,
    });
    for (uint64_t i = 1; i <= 3; ++i)
        backend->hide(layout.refLogKey(fixture::fixtureLife(ns), RefTxnId{1, i}));

    Gc gc(store, kGc);
    ASSERT_TRUE(gc.runRegularRound().acquired_lease);
    const auto hidden_cov = coverageOf(*backend, layout, ns);
    ASSERT_TRUE(hidden_cov.has_value())
        << "the namespace is `Live` in the catalog, so it stays in the universe even fully hidden";
    EXPECT_EQ(hidden_cov->classification, CoverageClass::Folded) << "the checkpoint's frontier is folded by exact key";
    EXPECT_EQ(hidden_cov->last_folded_ref_id, (RefTxnId{1, 3}));

    /// The store stops lying: the already folded namespace reappears.
    backend->revealAll();
    ASSERT_TRUE(gc.runRegularRound().acquired_lease);

    EXPECT_EQ(cursorOf(*backend, layout, ns), (RefTxnId{1, 3}));
    for (uint64_t i = 1; i <= 3; ++i)
        EXPECT_EQ(inDegreeOf(*backend, layout, DB::UInt128(i)), 1) << "blob " << i;
}

/// ===================== EACH LIFE'S STOP IS PLANNED BEFORE THE WALK =====================

namespace
{

/// One round of `gc` and the metrics of its `fold_ref_intake` row.
std::map<String, UInt64> runRoundReadingIntake(Gc & gc)
{
    std::map<String, UInt64> metrics;
    gc.setPhaseSink([&](const GcPhaseRecord & record)
    {
        if (record.phase == "fold_ref_intake")
            metrics = record.metrics;
    });
    SCOPE_EXIT({ gc.setPhaseSink({}); });
    EXPECT_TRUE(gc.runRegularRound().acquired_lease);
    return metrics;
}

/// The journal entries since it was cleared that name `key`, of `verb`.
size_t requestsOn(const CountingBackend & backend, JournalVerb verb, const String & key)
{
    size_t n = 0;
    for (const JournalEntry & entry : backend.journal())
        n += entry.verb == verb && std::find(entry.keys.begin(), entry.keys.end(), key) != entry.keys.end();
    return n;
}

size_t deletesSent(const CountingBackend & backend)
{
    size_t n = 0;
    for (const JournalEntry & entry : backend.journal())
        n += entry.verb == JournalVerb::Remove || entry.verb == JournalVerb::RemoveMany;
    return n;
}

/// A seal at `id` whose body names `next_epoch` as the epoch the chain continues in.
void writeSealToEpoch(Backend & backend, const Layout & layout, const RootNamespace & ns, const RefTxnId & id,
                      uint64_t next_epoch)
{
    RefLogTxn txn;
    txn.ns = ns.string();
    txn.txn_id = id;
    txn.ops = {epochSealOp()};
    txn.next_epoch = next_epoch;
    writeRefLogTxnRaw(backend, layout, txn);
}

}

/// A seal names the epoch the chain continues in. Planning follows it to that epoch's head and never reads past
/// the seal in the sealed epoch, even when the listing omits the whole new epoch.
TEST(CASGCArithmeticIntake, ASealSendsThePlannerToTheHeadItsBodyNames)
{
    const RootNamespace ns{"00/aa@cas@"};
    const auto sealedPool = [&](auto & backend, const Layout & layout, Gc & gc)
    {
        fixture::admitLive(*backend, layout, ns);
        publishAt(*backend, layout, ns, RefTxnId{1, 1}, "ref_1", 1, DB::UInt128(1), /*birth=*/true);
        publishAt(*backend, layout, ns, RefTxnId{1, 2}, "ref_2", 2, DB::UInt128(2));
        writeRecoverableCkptForRawFixture(*backend, layout, ns, RefCkpt{
            .life_epoch = 1,
            .committed_through = RefTxnId{1, 2},
            .checkpoint_snapshot_id = std::nullopt,
            .last_epoch_seal = std::nullopt,
        });
        ASSERT_TRUE(gc.runRegularRound().acquired_lease);
        ASSERT_EQ(cursorOf(*backend, layout, ns), (RefTxnId{1, 2}));
        writeSealToEpoch(*backend, layout, ns, RefTxnId{1, 3}, /*next_epoch=*/5);
    };

    {
        SCOPED_TRACE("the head is present");
        auto backend = std::make_shared<HintHoleBackendOn<CountingBackend>>();
        auto store = openPoolForTest(backend, /*gc_fold_max_defer_rounds=*/0);
        const Layout & layout = store->layout();
        Gc gc(store, kGc);
        ASSERT_NO_FATAL_FAILURE(sealedPool(backend, layout, gc));
        publishAt(*backend, layout, ns, RefTxnId{5, 1}, "ref_51", 51, DB::UInt128(51),
                  /*birth=*/false, /*prev_epoch_seal=*/RefTxnId{1, 3});
        publishAt(*backend, layout, ns, RefTxnId{5, 2}, "ref_52", 52, DB::UInt128(52));
        backend->hide(layout.refLogKey(fixture::fixtureLife(ns), RefTxnId{5, 1}));
        backend->hide(layout.refLogKey(fixture::fixtureLife(ns), RefTxnId{5, 2}));

        backend->clearJournal();
        const auto intake = runRoundReadingIntake(gc);
        ASSERT_FALSE(intake.empty());
        EXPECT_GT(backend->holesServed(), 0u);
        EXPECT_EQ(requestsOn(*backend, JournalVerb::Read, layout.refLogKey(fixture::fixtureLife(ns), RefTxnId{1, 4})), 0u)
            << "a key past the seal in the sealed epoch was read";
        EXPECT_EQ(cursorOf(*backend, layout, ns), (RefTxnId{5, 2}));
        EXPECT_EQ(intake.at("frontier_proven"), intake.at("frontier_namespaces"));
        EXPECT_EQ(intake.at("absent_probes"), 1u);
        EXPECT_EQ(inDegreeOf(*backend, layout, DB::UInt128(51)), 1) << "the head's owner edge was not folded";
        EXPECT_EQ(inDegreeOf(*backend, layout, DB::UInt128(52)), 1);
    }
    {
        SCOPED_TRACE("the head is absent");
        auto backend = std::make_shared<HintHoleBackendOn<CountingBackend>>();
        auto store = openPoolForTest(backend, /*gc_fold_max_defer_rounds=*/0);
        const Layout & layout = store->layout();
        Gc gc(store, kGc);
        ASSERT_NO_FATAL_FAILURE(sealedPool(backend, layout, gc));

        backend->clearJournal();
        const auto intake = runRoundReadingIntake(gc);
        ASSERT_FALSE(intake.empty());
        EXPECT_EQ(requestsOn(*backend, JournalVerb::Read, layout.refLogKey(fixture::fixtureLife(ns), RefTxnId{1, 4})), 0u)
            << "a key past the seal in the sealed epoch was read";
        EXPECT_EQ(requestsOn(*backend, JournalVerb::Read, layout.refLogKey(fixture::fixtureLife(ns), RefTxnId{5, 1})), 1u);
        EXPECT_EQ(cursorOf(*backend, layout, ns), (RefTxnId{1, 3}));
        EXPECT_EQ(intake.at("frontier_proven"), intake.at("frontier_namespaces"));
        EXPECT_EQ(intake.at("absent_probes"), 1u);
    }
}

/// A chain of seals longer than planning follows: the walk applies the planned prefix and stops at the first epoch
/// planning did not reach, unproven, without a read there and without a hold. The next round goes on from there.
TEST(CASGCArithmeticIntake, MoreSealCrossingsThanThePlannerFollowsLeaveTheLifeUnproven)
{
    constexpr uint64_t seal_only_epochs = 66;
    auto backend = std::make_shared<CountingBackend>();
    auto store = openPoolForTest(backend, /*gc_fold_max_defer_rounds=*/0);
    const Layout & layout = store->layout();
    const RootNamespace ns{"00/aa@cas@"};
    const NamespaceLifeId life = fixture::fixtureLife(ns);
    fixture::admitLive(*backend, layout, ns);
    publishAt(*backend, layout, ns, RefTxnId{1, 1}, "ref_1", 1, DB::UInt128(1), /*birth=*/true);
    writeRecoverableCkptForRawFixture(*backend, layout, ns, RefCkpt{
        .life_epoch = 1,
        .committed_through = RefTxnId{1, 1},
        .checkpoint_snapshot_id = std::nullopt,
        .last_epoch_seal = std::nullopt,
    });
    Gc gc(store, kGc);
    ASSERT_TRUE(gc.runRegularRound().acquired_lease);
    ASSERT_EQ(cursorOf(*backend, layout, ns), (RefTxnId{1, 1}));

    /// `{1,2}` seals epoch 1; each later epoch holds only its own seal; epoch 67 holds a record.
    RefTxnId seal{1, 2};
    writeSealAt(*backend, layout, ns, seal);
    for (uint64_t epoch = 2; epoch <= seal_only_epochs; ++epoch)
    {
        writeSealAt(*backend, layout, ns, RefTxnId{epoch, 1}, seal);
        seal = RefTxnId{epoch, 1};
    }
    publishAt(*backend, layout, ns, RefTxnId{seal_only_epochs + 1, 1}, "ref_last", 2, DB::UInt128(2),
              /*birth=*/false, /*prev_epoch_seal=*/seal);

    backend->clearJournal();
    std::map<String, UInt64> intake;
    {
        ScopedLogCapture capture("CasGc", "information");
        intake = runRoundReadingIntake(gc);
        EXPECT_EQ(capture.count("discovery_incomplete=1"), 1u) << capture.captured();
        const String text = capture.captured();
        const size_t at = text.find("past what planning observed");
        ASSERT_NE(at, String::npos) << "no line names the life planning did not cover: " << text;
        const size_t line_begin = text.rfind('\n', at) == String::npos ? 0 : text.rfind('\n', at) + 1;
        const String line = text.substr(line_begin, text.find('\n', at) - line_begin);
        EXPECT_NE(line.find(ns.string()), String::npos) << line;
        EXPECT_NE(line.find(renderRefTxnId(RefTxnId{seal_only_epochs, 1})), String::npos) << line;
        EXPECT_GT(capture.count("destructive work SUPPRESSED"), 0u);
    }
    ASSERT_FALSE(intake.empty());
    EXPECT_LT(intake.at("frontier_proven"), intake.at("frontier_namespaces"));
    EXPECT_EQ(deletesSent(*backend), 0u);
    /// Planning crossed 64 seals: from `{1,2}` to the head of epoch 65, which is the 65th seal.
    EXPECT_EQ(cursorOf(*backend, layout, ns), (RefTxnId{65, 1})) << "the planned prefix was not kept";
    EXPECT_EQ(requestsOn(*backend, JournalVerb::Read, layout.refLogKey(life, RefTxnId{66, 1})), 0u)
        << "the walk read in an epoch planning did not reach";
    const auto coverage = coverageOf(*backend, layout, ns);
    ASSERT_TRUE(coverage.has_value());
    EXPECT_FALSE(coverage->hold.has_value()) << "an unplanned epoch minted a hold";

    const auto next = runRoundReadingIntake(gc);
    ASSERT_FALSE(next.empty());
    EXPECT_EQ(next.at("frontier_proven"), next.at("frontier_namespaces"));
    EXPECT_EQ(cursorOf(*backend, layout, ns), (RefTxnId{seal_only_epochs + 1, 1}));
    EXPECT_EQ(inDegreeOf(*backend, layout, DB::UInt128(2)), 1);
}

/// Probing above a record near the top of the sequence range saturates at the greatest sequence instead of
/// wrapping to a small one, with read-ahead on. The record at the greatest sequence has no successor, so the walk holds
/// there.
TEST(CASGCArithmeticIntake, ProbingSaturatesAtTheTopOfTheSequenceRange)
{
    constexpr uint64_t top = std::numeric_limits<uint64_t>::max();
    auto backend = std::make_shared<HintHoleBackendOn<CountingBackend>>();
    auto store = Pool::open(backend, PoolConfig{.pool_prefix = "p", .server_root_id = "test",
                                                .gc_fold_max_defer_rounds = 0, .gc_io_concurrency = 16});
    const Layout & layout = store->layout();
    const RootNamespace ns{"00/aa@cas@"};
    const NamespaceLifeId life = fixture::fixtureLife(ns);
    fixture::admitLive(*backend, layout, ns);
    publishAt(*backend, layout, ns, RefTxnId{1, 1}, "ref_1", 1, DB::UInt128(1), /*birth=*/true);
    writeRecoverableCkptForRawFixture(*backend, layout, ns, RefCkpt{
        .life_epoch = 1,
        .committed_through = RefTxnId{1, 1},
        .checkpoint_snapshot_id = std::nullopt,
        .last_epoch_seal = std::nullopt,
    });
    Gc gc(store, kGc);
    ASSERT_TRUE(gc.runRegularRound().acquired_lease);

    /// Records `top-4 .. top`; the cursor sits on the first, the listing shows none above it. Doubling from
    /// `top-3` probes `top-2`, `top-1` and then `top-3 + 4`, which only saturation keeps in range.
    for (uint64_t back = 4;; --back)
    {
        publishAt(*backend, layout, ns, RefTxnId{1, top - back}, fmt::format("ref_top_{}", back), 100 + back,
                  DB::UInt128(100 + back));
        if (back < 4)
            backend->hide(layout.refLogKey(life, RefTxnId{1, top - back}));
        if (back == 0)
            break;
    }
    seedFoldCursorForTest(*backend, layout, ns, RefTxnId{1, top - 4}, std::nullopt,
                          currentGenerationOf(*backend, layout), currentAttemptOf(*backend, layout));

    backend->clearJournal();
    const auto intake = runRoundReadingIntake(gc);
    ASSERT_FALSE(intake.empty());
    EXPECT_GT(backend->holesServed(), 0u);
    EXPECT_GT(requestsOn(*backend, JournalVerb::Read, layout.refLogKey(life, RefTxnId{1, top})), 0u)
        << "positive control: the probe reached the greatest sequence";
    for (const JournalEntry & entry : backend->journal())
        for (const String & key : entry.keys)
            if (const auto parsed = layout.parseRefObjectKey(key); parsed && parsed->txn_id.writer_epoch == 1)
                EXPECT_GE(parsed->txn_id.ref_sequence, top - 4) << "a wrapped key was requested: " << key;
    const auto coverage = coverageOf(*backend, layout, ns);
    ASSERT_TRUE(coverage && coverage->hold);
    EXPECT_EQ(coverage->hold->reason, HoldReason::BodyUndecodable);
    EXPECT_EQ(coverage->hold->offending_position, (RefTxnId{1, top}));
    EXPECT_EQ(coverage->last_folded_ref_id, (RefTxnId{1, top - 1}));
    EXPECT_EQ(deletesSent(*backend), 0u);
}

/// A quiet life with a warm cursor costs its `_ckpt` read and one read of its next key, answered absent, and
/// nothing else on its `_log` keys. Planning adds no `LIST`.
TEST(CASGCArithmeticIntake, AQuietLifeCostsOneCheckpointReadAndOneAbsentFirstKey)
{
    /// The `LIST` requests of this round on the commit before planning was added.
    constexpr size_t base_round_lists = 5;
    auto backend = std::make_shared<CountingBackend>();
    auto store = openPoolForTest(backend, /*gc_fold_max_defer_rounds=*/0);
    const Layout & layout = store->layout();
    const std::vector<RootNamespace> quiet{RootNamespace{"00/aa@cas@"}, RootNamespace{"00/bb@cas@"},
                                           RootNamespace{"00/cc@cas@"}};
    uint64_t blob = 1;
    for (const RootNamespace & ns : quiet)
    {
        fixture::admitLive(*backend, layout, ns);
        publishAt(*backend, layout, ns, RefTxnId{1, 1}, "ref_1", blob, DB::UInt128(blob), /*birth=*/true);
        ++blob;
        publishAt(*backend, layout, ns, RefTxnId{1, 2}, "ref_2", blob, DB::UInt128(blob));
        ++blob;
        writeRecoverableCkptForRawFixture(*backend, layout, ns, RefCkpt{
            .life_epoch = 1,
            .committed_through = RefTxnId{1, 2},
            .checkpoint_snapshot_id = std::nullopt,
            .last_epoch_seal = std::nullopt,
        });
    }
    Gc gc(store, kGc);
    ASSERT_TRUE(gc.runRegularRound().acquired_lease);

    backend->clearJournal();
    const auto intake = runRoundReadingIntake(gc);
    ASSERT_FALSE(intake.empty());
    const auto journal = backend->journal();
    for (const RootNamespace & ns : quiet)
    {
        SCOPED_TRACE(ns.string());
        const NamespaceLifeId life = fixture::fixtureLife(ns);
        const String checkpoint = layout.refCkptKey(life);
        const String stop = layout.refLogKey(life, RefTxnId{1, 3});
        const String logs = stop.substr(0, stop.find("/_log/") + 6);
        size_t checkpoint_reads = 0;
        size_t stop_reads = 0;
        size_t other_log_requests = 0;
        for (const JournalEntry & entry : journal)
            for (const String & key : entry.keys)
            {
                if (key == checkpoint && entry.verb == JournalVerb::Read)
                    ++checkpoint_reads;
                else if (key == stop && entry.verb == JournalVerb::Read)
                    ++stop_reads;
                else if (key.starts_with(logs))
                    ++other_log_requests;
            }
        EXPECT_EQ(checkpoint_reads, 1u);
        EXPECT_EQ(stop_reads, 1u);
        EXPECT_EQ(other_log_requests, 0u);
        EXPECT_EQ(cursorOf(*backend, layout, ns), (RefTxnId{1, 2}));
    }
    EXPECT_EQ(intake.at("frontier_proven"), intake.at("frontier_namespaces"));
    EXPECT_EQ(intake.at("absent_probes"), quiet.size());
    const size_t lists = std::count_if(journal.begin(), journal.end(),
                                       [](const JournalEntry & entry) { return entry.verb == JournalVerb::List; });
    EXPECT_EQ(lists, base_round_lists);
}

/// ===================== READ-AHEAD OF REF LOGS =====================

namespace
{

/// Records the threads that read each ref log, and runs a one-shot callback right after one chosen key is read absent.
/// A hinted read runs on a read-ahead worker; a take nobody hinted runs on the round's thread. It can also hold
/// worker reads until enough are outstanding at once, and fail every read of one key.
class ReadAheadObservingBackend : public CountingBackend
{
public:
    void armAfterAbsentRead(const String & key, std::function<void()> callback)
    {
        std::lock_guard lock(observe_mutex);
        after_absent_key = key;
        after_absent_callback = std::move(callback);
    }

    std::set<String> logsReadOffThread(std::thread::id round_thread) const
    {
        std::lock_guard lock(observe_mutex);
        std::set<String> out;
        for (const auto & [key, readers] : log_readers)
            if (std::any_of(readers.begin(), readers.end(), [&](std::thread::id reader) { return reader != round_thread; }))
                out.insert(key);
        return out;
    }

    void clearReaders()
    {
        std::lock_guard lock(observe_mutex);
        log_readers.clear();
    }

    /// Holds every ref-log read made off `round_thread`, except of `except_key`, until `width` distinct keys are held
    /// at once. The wait is bounded and is abandoned for good once it expires, so a missing overlap fails
    /// `fillWitnessReached` instead of hanging.
    void armFillWitness(std::thread::id round_thread, size_t width, String except_key)
    {
        std::lock_guard lock(witness_mutex);
        witness_round_thread = round_thread;
        witness_width = width;
        witness_except_key = std::move(except_key);
    }

    bool fillWitnessReached() const
    {
        std::lock_guard lock(witness_mutex);
        return witness_filled;
    }

    /// Every read of `key`, on any thread, throws.
    void failReadsOf(String key)
    {
        std::lock_guard lock(observe_mutex);
        fault_key = std::move(key);
    }

    size_t faultsServed() const
    {
        std::lock_guard lock(observe_mutex);
        return faults;
    }

    std::optional<DB::Cas::Backend::Raw> read(const String & key, DB::Cas::TransportAccess & access) override
    {
        if (key.find("/_log/") != String::npos)
        {
            {
                std::lock_guard lock(observe_mutex);
                log_readers[key].insert(std::this_thread::get_id());
                if (key == fault_key)
                {
                    ++faults;
                    throw std::runtime_error("injected ref log read fault");
                }
            }
            holdForFillWitness(key);
        }
        std::optional<DB::Cas::Backend::Raw> got = CountingBackend::read(key, access);
        std::function<void()> fire;
        {
            std::lock_guard lock(observe_mutex);
            if (!got && after_absent_callback && key == after_absent_key)
                fire = std::exchange(after_absent_callback, nullptr);
        }
        if (fire)
            fire();
        return got;
    }

private:
    void holdForFillWitness(const String & key)
    {
        std::unique_lock lock(witness_mutex);
        if (witness_width == 0 || witness_filled || witness_abandoned || std::this_thread::get_id() == witness_round_thread
            || key == witness_except_key)
            return;
        held.insert(key);
        if (held.size() >= witness_width)
        {
            witness_filled = true;
            witness_cv.notify_all();
            return;
        }
        if (!witness_cv.wait_for(lock, std::chrono::seconds(10), [&] { return witness_filled; }))
            witness_abandoned = true;
    }

    mutable std::mutex observe_mutex;
    std::map<String, std::set<std::thread::id>> log_readers;
    String fault_key;
    size_t faults = 0;

    mutable std::mutex witness_mutex;
    std::condition_variable witness_cv;
    std::thread::id witness_round_thread;
    size_t witness_width = 0;
    String witness_except_key;
    std::set<String> held;
    bool witness_filled = false;
    bool witness_abandoned = false;
    String after_absent_key;
    std::function<void()> after_absent_callback;
};

using ObservedHintHoleBackend = HintHoleBackendOn<ReadAheadObservingBackend>;

/// Publishes `{1,first} .. {1,last}` of `ns`; record `{1,i}` names blob `blob_base + i`.
void publishRange(Backend & backend, const Layout & layout, const RootNamespace & ns, uint64_t first, uint64_t last,
                  uint64_t blob_base = 0)
{
    for (uint64_t i = first; i <= last; ++i)
        publishAt(backend, layout, ns, RefTxnId{1, i}, fmt::format("ref_{}", i), blob_base + i,
                  DB::UInt128(blob_base + i), /*birth=*/i == 1);
}

/// A live `ns` whose records `{1,1}` and `{1,2}` a first round of `gc` folds.
void foldTwoRecords(Backend & backend, const Layout & layout, const RootNamespace & ns, Gc & gc)
{
    fixture::admitLive(backend, layout, ns);
    publishRange(backend, layout, ns, 1, 2);
    writeRecoverableCkptForRawFixture(backend, layout, ns, RefCkpt{
        .life_epoch = 1,
        .committed_through = RefTxnId{1, 2},
        .checkpoint_snapshot_id = std::nullopt,
        .last_epoch_seal = std::nullopt,
    });
    ASSERT_TRUE(gc.runRegularRound().acquired_lease);
    ASSERT_EQ(cursorOf(backend, layout, ns), (RefTxnId{1, 2}));
}

}

/// Records the listing omits are read ahead too, up to the last one planning read present: the walk has `width`
/// ref-log reads outstanding at once, and nothing at or above the stop is read by a worker.
TEST(CASGCArithmeticIntake, UnlistedRecordsBelowTheStopAreReadAhead)
{
    constexpr uint64_t hidden = 300;
    constexpr uint64_t width = 16;
    auto backend = std::make_shared<ObservedHintHoleBackend>();
    auto store = Pool::open(backend, PoolConfig{.pool_prefix = "p", .server_root_id = "test",
                                                .gc_fold_max_defer_rounds = 0, .gc_io_concurrency = width});
    const Layout & layout = store->layout();
    const RootNamespace ns{"00/aa@cas@"};
    const NamespaceLifeId life = fixture::fixtureLife(ns);
    Gc gc(store, kGc);
    ASSERT_NO_FATAL_FAILURE(foldTwoRecords(*backend, layout, ns, gc));

    publishRange(*backend, layout, ns, 3, 2 + hidden);
    for (uint64_t i = 3; i <= 2 + hidden; ++i)
        backend->hide(layout.refLogKey(life, RefTxnId{1, i}));

    backend->clearJournal();
    backend->clearReaders();
    const std::thread::id round_thread = std::this_thread::get_id();
    ScopedRoundCallback arm(gc, RoundInterleavePoint::AfterDiscovery, {}, [&]
    {
        backend->armFillWitness(round_thread, width, layout.refLogKey(life, RefTxnId{1, 3}));
    });
    const auto intake = runRoundReadingIntake(gc);
    ASSERT_EQ(arm.fired(), 1u);
    ASSERT_FALSE(intake.empty());
    EXPECT_GT(backend->holesServed(), 0u);
    EXPECT_TRUE(backend->fillWitnessReached()) << "the walk never had " << width << " distinct ref logs outstanding";
    EXPECT_EQ(cursorOf(*backend, layout, ns), (RefTxnId{1, 2 + hidden}));
    EXPECT_EQ(intake.at("frontier_proven"), intake.at("frontier_namespaces"));
    EXPECT_EQ(intake.at("reflog_reads_issued"), intake.at("reflog_takes")) << "a ref log was hinted and never taken";

    const RefTxnId stop{1, 3 + hidden};
    EXPECT_EQ(requestsOn(*backend, JournalVerb::Read, layout.refLogKey(life, stop)), 1u)
        << "the stop was not read exactly once, by planning";
    for (const String & key : backend->logsReadOffThread(round_thread))
    {
        const auto parsed = layout.parseRefObjectKey(key);
        ASSERT_TRUE(parsed.has_value()) << key;
        EXPECT_LT(parsed->txn_id.ref_sequence, stop.ref_sequence) << "a worker read at or above the stop: " << key;
    }
    for (uint64_t i = 3; i <= 2 + hidden; ++i)
        ASSERT_EQ(inDegreeOf(*backend, layout, DB::UInt128(i)), 1) << "record " << i << " was not applied";
}

/// The listing ends `hidden` records below the tail. Planning reads the listing's last id, doubles past it to its
/// first miss, and a writer fills that slot right after the miss. Bisection still finds the first absent key above
/// the tail: the walk adopts the record below it and stops there, the late record is not folded, and no worker reads
/// at or above the stop.
TEST(CASGCArithmeticIntake, AHintBehindTheTailStopsAtTheObservedMiss)
{
    constexpr uint64_t listed_tail = 52;
    constexpr uint64_t hidden = 200;
    constexpr uint64_t tail = listed_tail + hidden;
    constexpr uint64_t late_blob = 9000;
    auto backend = std::make_shared<ObservedHintHoleBackend>();
    auto store = openPoolForTest(backend, /*gc_fold_max_defer_rounds=*/0);
    const Layout & layout = store->layout();
    const RootNamespace ns{"00/aa@cas@"};
    const NamespaceLifeId life = fixture::fixtureLife(ns);
    Gc gc(store, kGc);
    ASSERT_NO_FATAL_FAILURE(foldTwoRecords(*backend, layout, ns, gc));

    publishRange(*backend, layout, ns, 3, tail);
    for (uint64_t i = listed_tail + 1; i <= tail; ++i)
        backend->hide(layout.refLogKey(life, RefTxnId{1, i}));

    /// Doubling from the listing's last id: `listed_tail + 1, + 2, + 4, ...`; the first of them above the tail misses.
    uint64_t step = 1;
    while (listed_tail + step <= tail)
        step *= 2;
    const RefTxnId late{1, listed_tail + step};
    bool written = false;
    backend->armAfterAbsentRead(layout.refLogKey(life, late), [&]
    {
        publishAt(*backend, layout, ns, late, "ref_late", late_blob, DB::UInt128(late_blob));
        written = true;
    });

    backend->clearJournal();
    backend->clearReaders();
    const auto intake = runRoundReadingIntake(gc);
    ASSERT_FALSE(intake.empty());
    ASSERT_TRUE(written) << "planning never read " << late.ref_sequence;
    EXPECT_GT(backend->holesServed(), 0u);
    EXPECT_EQ(cursorOf(*backend, layout, ns), (RefTxnId{1, tail}));
    EXPECT_EQ(requestsOn(*backend, JournalVerb::Read, layout.refLogKey(life, RefTxnId{1, tail + 1})), 1u)
        << "the stop was not read exactly once, by planning";
    EXPECT_EQ(inDegreeOf(*backend, layout, DB::UInt128(late_blob)), 0) << "the record written after its miss was folded";
    EXPECT_EQ(intake.at("frontier_proven"), intake.at("frontier_namespaces"));
    /// `2 * ceil(log2(hidden + 1)) + 2`.
    EXPECT_LE(intake.at("absent_probes"), 2 * std::bit_width(hidden) + 2);
    EXPECT_GT(intake.at("absent_probes"), 0u);

    size_t read_ahead_unlisted = 0;
    for (const String & key : backend->logsReadOffThread(std::this_thread::get_id()))
    {
        const auto parsed = layout.parseRefObjectKey(key);
        ASSERT_TRUE(parsed.has_value()) << key;
        EXPECT_LE(parsed->txn_id.ref_sequence, tail) << "a worker read at or above the stop: " << key;
        read_ahead_unlisted += parsed->txn_id.ref_sequence > listed_tail;
    }
    EXPECT_GT(read_ahead_unlisted, 0u) << "positive control: no unlisted id was read ahead";
}

/// Read-ahead moves only the moment bytes are fetched. The same pool folded at `gc_io_concurrency` 1 and 16 adopts the
/// same cursors, holds at the same positions and deletes the same keys; at 1 nothing is read ahead. A hold suppresses
/// deletes, so the gap it holds at is filled after the round that holds. After the first round two lives get records
/// while the round remembers their first keys, which width 16 reads ahead: one finds records past the listed tail,
/// the other a hole below listed records.
TEST(CASGCArithmeticIntake, ConcurrencyOneIsSequentialAndDecidesAsSixteen)
{
    constexpr size_t rounds = 8;
    struct Outcome
    {
        std::map<String, RefTxnId> cursors;
        /// After each of the first two rounds.
        std::vector<std::map<String, std::optional<std::pair<HoldReason, RefTxnId>>>> holds;
        std::set<String> deleted;
        std::vector<std::map<String, UInt64>> intakes;
        /// Ref logs some thread other than the round's read, and the first keys of the second round that should be among
        /// them at width 16.
        std::set<String> read_ahead;
        String present_first_key;
        String hole_key;
    };
    const std::vector<RootNamespace> lives{RootNamespace{"00/aa@cas@"}, RootNamespace{"00/bb@cas@"},
                                           RootNamespace{"00/cc@cas@"}, RootNamespace{"00/dd@cas@"},
                                           RootNamespace{"00/ee@cas@"}, RootNamespace{"00/ff@cas@"}};
    const auto fold = [&](uint64_t concurrency)
    {
        auto backend = std::make_shared<ObservedHintHoleBackend>();
        auto store = Pool::open(backend, PoolConfig{.pool_prefix = "p", .server_root_id = "test",
                                                    .gc_fold_max_defer_rounds = 0, .gc_io_concurrency = concurrency});
        const Layout & layout = store->layout();
        const auto seed = [&](const RootNamespace & ns, uint64_t blob_base, uint64_t last)
        {
            fixture::admitLive(*backend, layout, ns);
            publishRange(*backend, layout, ns, 1, last, blob_base);
            writeRecoverableCkptForRawFixture(*backend, layout, ns, RefCkpt{
                .life_epoch = 1,
                .committed_through = RefTxnId{1, last},
                .checkpoint_snapshot_id = std::nullopt,
                .last_epoch_seal = std::nullopt,
            });
        };
        const auto hide = [&](const RootNamespace & ns, uint64_t first, uint64_t last)
        {
            for (uint64_t i = first; i <= last; ++i)
                backend->hide(layout.refLogKey(fixture::fixtureLife(ns), RefTxnId{1, i}));
        };
        /// All listed; then a record that drops `ref_1`, so its blob is reclaimed.
        seed(lives[0], 0, 40);
        writeTxnAt(*backend, layout, lives[0], RefTxnId{1, 41},
                   {ownerTransitionOp(RefOwnerBinding{RefOwnerKind::Committed, "ref_1",
                                                      ManifestRef{.writer_epoch = 1, .build_sequence = 1, .manifest_ordinal = 1}},
                                      std::nullopt)});
        /// The top half hidden.
        seed(lives[1], 100, 40);
        hide(lives[1], 21, 40);
        /// A hidden run in the middle.
        seed(lives[2], 200, 40);
        hide(lives[2], 11, 20);
        /// `{1,11}` missing below listed records: the walk holds there until it is written.
        seed(lives[3], 300, 10);
        for (uint64_t i = 12; i <= 15; ++i)
            publishAt(*backend, layout, lives[3], RefTxnId{1, i}, fmt::format("ref_{}", i), 300 + i, DB::UInt128(300 + i));
        /// Quiet in the first round; the next two rounds start at the first key the first round read absent.
        seed(lives[4], 400, 20);
        seed(lives[5], 500, 10);

        Gc gc(store, kGc);
        Outcome out;
        for (size_t i = 0; i < rounds; ++i)
        {
            std::map<String, UInt64> metrics;
            gc.setPhaseSink([&](const GcPhaseRecord & record)
            {
                if (record.phase == "fold_ref_intake")
                    metrics = record.metrics;
            });
            runRegularRoundReclaiming(gc);
            store->renewWatermarkOnce();
            gc.setPhaseSink({});
            out.intakes.push_back(std::move(metrics));
            if (i > 1)
                continue;
            auto & holds = out.holds.emplace_back();
            for (const RootNamespace & ns : lives)
            {
                const auto coverage = coverageOf(*backend, layout, ns);
                if (coverage && coverage->hold)
                    holds[ns.string()] = std::make_pair(coverage->hold->reason, coverage->hold->offending_position);
                else
                    holds[ns.string()] = std::nullopt;
            }
            if (i == 0)
            {
                publishAt(*backend, layout, lives[3], RefTxnId{1, 11}, "ref_11", 311, DB::UInt128(311));
                /// Records past the listed tail.
                publishRange(*backend, layout, lives[4], 21, 40, 400);
                hide(lives[4], 31, 40);
                /// `{1,11}` missing below listed records.
                for (uint64_t j = 12; j <= 15; ++j)
                    publishAt(*backend, layout, lives[5], RefTxnId{1, j}, fmt::format("ref_{}", j), 500 + j, DB::UInt128(500 + j));
            }
            else
                publishAt(*backend, layout, lives[5], RefTxnId{1, 11}, "ref_11", 511, DB::UInt128(511));
        }
        for (const RootNamespace & ns : lives)
            out.cursors[ns.string()] = cursorOf(*backend, layout, ns);
        /// Probe keys carry a random name.
        for (const JournalEntry & entry : backend->journal())
            if (entry.verb == JournalVerb::Remove || entry.verb == JournalVerb::RemoveMany)
                for (const String & key : entry.keys)
                    if (key.find("/_probe/") == String::npos)
                        out.deleted.insert(key);
        out.read_ahead = backend->logsReadOffThread(std::this_thread::get_id());
        out.present_first_key = layout.refLogKey(fixture::fixtureLife(lives[4]), RefTxnId{1, 21});
        out.hole_key = layout.refLogKey(fixture::fixtureLife(lives[5]), RefTxnId{1, 11});
        EXPECT_FALSE(blobAbsent(*backend, layout, DB::UInt128(2))) << "positive control: a named blob is kept";
        EXPECT_TRUE(blobAbsent(*backend, layout, DB::UInt128(1))) << "positive control: the dropped blob is reclaimed";
        return out;
    };

    const Outcome sequential = fold(1);
    const Outcome parallel = fold(16);

    for (const auto & intake : sequential.intakes)
    {
        ASSERT_FALSE(intake.empty());
        EXPECT_EQ(intake.at("reflog_reads_issued"), intake.at("reflog_takes"));
    }
    EXPECT_EQ(sequential.cursors, parallel.cursors);
    EXPECT_EQ(sequential.cursors.at("00/aa@cas@"), (RefTxnId{1, 41})) << "positive control: the drop was folded";
    EXPECT_EQ(sequential.cursors.at("00/dd@cas@"), (RefTxnId{1, 15})) << "positive control: the filled gap was folded";
    EXPECT_EQ(sequential.cursors.at("00/ee@cas@"), (RefTxnId{1, 40})) << "positive control: the unlisted records were folded";
    EXPECT_EQ(sequential.cursors.at("00/ff@cas@"), (RefTxnId{1, 15})) << "positive control: the filled hole was folded";
    EXPECT_EQ(sequential.holds, parallel.holds);
    ASSERT_EQ(sequential.holds.size(), 2u);
    ASSERT_TRUE(sequential.holds[0].at("00/dd@cas@").has_value()) << "positive control: the gap holds";
    EXPECT_EQ(sequential.holds[0].at("00/dd@cas@")->first, HoldReason::GapBelowWitness);
    EXPECT_FALSE(sequential.holds[0].at("00/ff@cas@").has_value());
    EXPECT_FALSE(sequential.holds[1].at("00/ee@cas@").has_value()) << "records past the listed tail are no gap";
    EXPECT_EQ(sequential.holds[1].at("00/ff@cas@"), std::make_pair(HoldReason::GapBelowWitness, RefTxnId{1, 11}))
        << "positive control: the hole holds";
    EXPECT_EQ(sequential.deleted, parallel.deleted);
    EXPECT_FALSE(sequential.deleted.empty()) << "positive control: the rounds deleted something";

    EXPECT_TRUE(sequential.read_ahead.empty()) << sequential.read_ahead.size() << " ref log(s) read ahead at width 1";
    EXPECT_TRUE(parallel.read_ahead.contains(parallel.present_first_key)) << "positive control: a present first key was read ahead";
    EXPECT_TRUE(parallel.read_ahead.contains(parallel.hole_key)) << "positive control: the hole was read ahead";
}

namespace
{

/// One round of `gc`: its report and the metrics of its `fold_ref_intake` row.
std::pair<RoundReport, std::map<String, UInt64>> runRoundReadingReportAndIntake(Gc & gc)
{
    std::map<String, UInt64> metrics;
    gc.setPhaseSink([&](const GcPhaseRecord & record)
    {
        if (record.phase == "fold_ref_intake")
            metrics = record.metrics;
    });
    SCOPE_EXIT({ gc.setPhaseSink({}); });
    RoundReport report = gc.runRegularRound();
    EXPECT_TRUE(report.acquired_lease);
    return {std::move(report), std::move(metrics)};
}

void removeStoredKey(Backend & backend, const String & key)
{
    OperationForTest op(backend);
    const auto meta = (*op).head(key, Retry::standard());
    ASSERT_TRUE(meta.has_value()) << key;
    ASSERT_EQ((*op).remove(key, meta->etag, Retry::standard()), Removal::Removed) << key;
}

bool anyAnomalySays(const RoundReport & report, std::string_view text)
{
    return std::any_of(report.anomalies.begin(), report.anomalies.end(),
                       [&](const RoundAnomaly & a) { return std::string_view(a.reason).find(text) != std::string_view::npos; });
}

}

/// A probe can jump a seal to a stray record in the sealed epoch. Read-ahead then hints the absent keys between them:
/// at most the window of reads, never taken, and the round ends as it does without read-ahead.
TEST(CASGCArithmeticIntake, ASealBelowAStrayWastesAtMostTheWindow)
{
    const RootNamespace ns{"00/aa@cas@"};
    const NamespaceLifeId life = fixture::fixtureLife(ns);
    struct Outcome
    {
        RefTxnId cursor;
        bool held = false;
        UInt64 proven = 0;
        size_t absent_worker_reads = 0;
    };
    const auto run = [&](uint64_t width)
    {
        auto backend = std::make_shared<ObservedHintHoleBackend>();
        auto store = Pool::open(backend, PoolConfig{.pool_prefix = "p", .server_root_id = "test",
                                                    .gc_fold_max_defer_rounds = 0, .gc_io_concurrency = width});
        const Layout & layout = store->layout();
        Gc gc(store, kGc);
        foldTwoRecords(*backend, layout, ns, gc);
        /// Planning probes 4, 5, 7, 11, 19 and 35, all present, and never reads the seal at 6 between them.
        const std::set<uint64_t> ordinary{3, 4, 5, 7, 11, 19, 35};
        for (const uint64_t i : ordinary)
            publishRange(*backend, layout, ns, i, i, /*blob_base=*/100);
        writeSealToEpoch(*backend, layout, ns, RefTxnId{1, 6}, /*next_epoch=*/5);
        publishAt(*backend, layout, ns, RefTxnId{5, 1}, "ref_51", 51, DB::UInt128(51),
                  /*birth=*/false, /*prev_epoch_seal=*/RefTxnId{1, 6});
        for (uint64_t i = 3; i <= 35; ++i)
            backend->hide(layout.refLogKey(life, RefTxnId{1, i}));
        backend->hide(layout.refLogKey(life, RefTxnId{5, 1}));
        backend->clearReaders();
        const auto intake = runRoundReadingIntake(gc);
        EXPECT_FALSE(intake.empty());
        Outcome out;
        out.cursor = cursorOf(*backend, layout, ns);
        const auto coverage = coverageOf(*backend, layout, ns);
        out.held = coverage && coverage->hold;
        out.proven = intake.at("frontier_proven");
        for (const String & key : backend->logsReadOffThread(std::this_thread::get_id()))
        {
            const auto parsed = layout.parseRefObjectKey(key);
            if (parsed && parsed->txn_id.writer_epoch == 1 && parsed->txn_id.ref_sequence > 6
                && !ordinary.contains(parsed->txn_id.ref_sequence))
                ++out.absent_worker_reads;
        }
        return out;
    };
    const Outcome sequential = run(1);
    const Outcome wide = run(16);
    EXPECT_EQ(sequential.absent_worker_reads, 0u);
    EXPECT_GT(wide.absent_worker_reads, 0u) << "positive control: no absent key between the seal and the stray was hinted";
    EXPECT_LE(wide.absent_worker_reads, 4 * 16u);
    EXPECT_FALSE(wide.held);
    EXPECT_EQ(wide.held, sequential.held);
    EXPECT_EQ(wide.cursor, sequential.cursor);
    EXPECT_EQ(wide.proven, sequential.proven);
}

/// A key missing below the last record planning read present is read once and holds the walk. At width 16 the read
/// is a read-ahead and no more than the window of ids above the hole are read.
TEST(CASGCArithmeticIntake, AHoleBelowTheStopIsReadOnceAndHolds)
{
    for (const uint64_t width : {1u, 16u})
    {
        SCOPED_TRACE(fmt::format("width {}", width));
        auto backend = std::make_shared<ObservedHintHoleBackend>();
        auto store = Pool::open(backend, PoolConfig{.pool_prefix = "p", .server_root_id = "test",
                                                    .gc_fold_max_defer_rounds = 0, .gc_io_concurrency = width});
        const Layout & layout = store->layout();
        const RootNamespace ns{"00/aa@cas@"};
        const NamespaceLifeId life = fixture::fixtureLife(ns);
        Gc gc(store, kGc);
        ASSERT_NO_FATAL_FAILURE(foldTwoRecords(*backend, layout, ns, gc));

        /// `{1,11}` is never written; `{1,12} .. {1,120}` lie above it.
        publishRange(*backend, layout, ns, 3, 10);
        publishRange(*backend, layout, ns, 12, 120);
        const String hole = layout.refLogKey(life, RefTxnId{1, 11});

        backend->clearJournal();
        backend->clearReaders();
        const auto [report, intake] = runRoundReadingReportAndIntake(gc);
        ASSERT_FALSE(intake.empty());
        EXPECT_EQ(requestsOn(*backend, JournalVerb::Read, hole), 1u);
        const auto coverage = coverageOf(*backend, layout, ns);
        ASSERT_TRUE(coverage && coverage->hold);
        EXPECT_EQ(coverage->hold->reason, HoldReason::GapBelowWitness);
        EXPECT_EQ(coverage->hold->offending_position, (RefTxnId{1, 11}));
        EXPECT_EQ(coverage->last_folded_ref_id, (RefTxnId{1, 10}));
        EXPECT_TRUE(anyAnomalySays(report, "probed present, read absent"));

        const std::set<String> read_ahead = backend->logsReadOffThread(std::this_thread::get_id());
        if (width == 1)
        {
            EXPECT_TRUE(read_ahead.empty()) << "nothing is read ahead at width 1";
            continue;
        }
        EXPECT_TRUE(read_ahead.contains(hole)) << "the hole was not read on a worker";
        size_t above = 0;
        for (const String & key : read_ahead)
            if (const auto parsed = layout.parseRefObjectKey(key); parsed && parsed->txn_id.ref_sequence > 11)
                ++above;
        EXPECT_GT(above, 0u) << "positive control: ids above the hole were read ahead";
        EXPECT_LE(above, 4 * width) << "more ids above the hole than the window were read";
    }
}

/// A worker fault on a hinted record fails the round when the walk takes that record. Behind an earlier hold the
/// record is never taken, and the round ends with that hold.
TEST(CASGCArithmeticIntake, AFaultOnAHintedRecordFailsOnlyWhenTaken)
{
    constexpr uint64_t tail = 40;
    const RootNamespace ns{"00/aa@cas@"};
    const NamespaceLifeId life = fixture::fixtureLife(ns);
    const auto setup = [&](ObservedHintHoleBackend & backend, const Layout & layout, Gc & gc, std::optional<uint64_t> hole)
    {
        ASSERT_NO_FATAL_FAILURE(foldTwoRecords(backend, layout, ns, gc));
        for (uint64_t i = 3; i <= tail; ++i)
            if (i != hole)
                publishRange(backend, layout, ns, i, i);
        for (uint64_t i = 3; i <= tail; ++i)
            backend.hide(layout.refLogKey(life, RefTxnId{1, i}));
        backend.failReadsOf(layout.refLogKey(life, RefTxnId{1, 13}));
    };
    const auto open = [](const std::shared_ptr<ObservedHintHoleBackend> & backend)
    {
        return Pool::open(backend, PoolConfig{.pool_prefix = "p", .server_root_id = "test",
                                              .gc_fold_max_defer_rounds = 0, .gc_io_concurrency = 16});
    };
    {
        SCOPED_TRACE("taken");
        auto backend = std::make_shared<ObservedHintHoleBackend>();
        auto store = open(backend);
        Gc gc(store, kGc);
        ASSERT_NO_FATAL_FAILURE(setup(*backend, store->layout(), gc, std::nullopt));
        const std::thread::id round_thread = std::this_thread::get_id();
        EXPECT_THROW(gc.runRegularRound(), std::runtime_error);
        EXPECT_GT(backend->faultsServed(), 0u) << "the fault was never served";
        EXPECT_TRUE(backend->logsReadOffThread(round_thread).contains(store->layout().refLogKey(life, RefTxnId{1, 13})))
            << "the faulty record was not read on a worker";
    }
    {
        SCOPED_TRACE("behind a hold");
        auto backend = std::make_shared<ObservedHintHoleBackend>();
        auto store = open(backend);
        const Layout & layout = store->layout();
        Gc gc(store, kGc);
        ASSERT_NO_FATAL_FAILURE(setup(*backend, layout, gc, 10));
        const RoundReport report = gc.runRegularRound();
        EXPECT_TRUE(report.acquired_lease);
        EXPECT_GT(backend->faultsServed(), 0u) << "the fault was never served";
        const auto coverage = coverageOf(*backend, layout, ns);
        ASSERT_TRUE(coverage && coverage->hold);
        EXPECT_EQ(coverage->hold->reason, HoldReason::GapBelowWitness);
        EXPECT_EQ(coverage->hold->offending_position, (RefTxnId{1, 10}));
        EXPECT_EQ(coverage->last_folded_ref_id, (RefTxnId{1, 9}));
    }
}

/// A listed key that disappears before the walk reads it. At or below the last record planning read present, the walk
/// holds at that key and keeps the cursor below it. Above it, here the listed tail removed between the listing and
/// planning, the key is the stop: planning reads it absent, the walk adopts the record below, and the key is never
/// read ahead.
TEST(CASGCArithmeticIntake, AListedKeyRemovedBeforeTheWalkHoldsOrBecomesTheStop)
{
    constexpr uint64_t tail = 40;
    const RootNamespace ns{"00/aa@cas@"};
    const NamespaceLifeId life = fixture::fixtureLife(ns);
    const auto readAheadAbove = [&](const ObservedHintHoleBackend & backend, const Layout & layout, uint64_t sequence)
    {
        size_t n = 0;
        for (const String & key : backend.logsReadOffThread(std::this_thread::get_id()))
            if (const auto parsed = layout.parseRefObjectKey(key); parsed && parsed->txn_id.ref_sequence > sequence)
                ++n;
        return n;
    };
    {
        SCOPED_TRACE("removed after planning, below the last record read present");
        auto backend = std::make_shared<ObservedHintHoleBackend>();
        auto store = openPoolForTest(backend, /*gc_fold_max_defer_rounds=*/0);
        const Layout & layout = store->layout();
        Gc gc(store, kGc);
        ASSERT_NO_FATAL_FAILURE(foldTwoRecords(*backend, layout, ns, gc));
        publishRange(*backend, layout, ns, 3, tail);
        const String lost = layout.refLogKey(life, RefTxnId{1, 20});

        backend->clearJournal();
        backend->clearReaders();
        ScopedRoundCallback removal(gc, RoundInterleavePoint::AfterDiscovery, {}, [&] { removeStoredKey(*backend, lost); });
        const auto [report, intake] = runRoundReadingReportAndIntake(gc);
        ASSERT_EQ(removal.fired(), 1u);
        ASSERT_FALSE(intake.empty());
        const auto coverage = coverageOf(*backend, layout, ns);
        ASSERT_TRUE(coverage && coverage->hold);
        EXPECT_EQ(coverage->hold->reason, HoldReason::GapBelowWitness);
        EXPECT_EQ(coverage->hold->offending_position, (RefTxnId{1, 20}));
        EXPECT_EQ(coverage->last_folded_ref_id, (RefTxnId{1, 19}));
        EXPECT_TRUE(anyAnomalySays(report, "probed present, read absent"));
    }
    {
        SCOPED_TRACE("the listed tail removed between the listing and planning");
        auto backend = std::make_shared<ObservedHintHoleBackend>();
        auto store = openPoolForTest(backend, /*gc_fold_max_defer_rounds=*/0);
        const Layout & layout = store->layout();
        Gc gc(store, kGc);
        ASSERT_NO_FATAL_FAILURE(foldTwoRecords(*backend, layout, ns, gc));
        publishRange(*backend, layout, ns, 3, tail);
        const String lost = layout.refLogKey(life, RefTxnId{1, tail});

        backend->clearJournal();
        backend->clearReaders();
        ScopedRoundCallback removal(gc, RoundInterleavePoint::AfterCatalogRead, {}, [&] { removeStoredKey(*backend, lost); });
        const auto [report, intake] = runRoundReadingReportAndIntake(gc);
        ASSERT_EQ(removal.fired(), 1u);
        ASSERT_FALSE(intake.empty());
        EXPECT_EQ(cursorOf(*backend, layout, ns), (RefTxnId{1, tail - 1}));
        EXPECT_EQ(intake.at("frontier_proven"), intake.at("frontier_namespaces"));
        EXPECT_EQ(requestsOn(*backend, JournalVerb::Read, lost), 1u) << "the stop was not read exactly once, by planning";
        EXPECT_EQ(readAheadAbove(*backend, layout, tail - 1), 0u) << "the removed listed key was read ahead";
        EXPECT_EQ(intake.at("reflog_reads_issued"), intake.at("reflog_takes")) << "a ref log was hinted and never taken";
    }
}
