#include "cas_test_helpers.h"
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Formats/CasRefCatalogFormat.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasRefCatalog.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasRefCkpt.h>
/// Explicit rather than relying on a transitive path: `DEBUG_OR_SANITIZER_BUILD` (used below to gate
/// the `*DeathTest` split) must resolve in THIS translation unit.
#include <base/defines.h>
#include <fmt/format.h>

using namespace DB::Cas;

namespace DB::ErrorCodes
{
    extern const int LOGICAL_ERROR;
    extern const int NETWORK_ERROR;
}

/// Stage B Task 3 (spec §3, the ref-chain catalog's creation lifecycle): the three-conditional-write
/// sequence that carries a namespace from nothing to `Live` --
///   1. catalog CAS: insert `{ns, Creating, fresh incarnation, creator}` (`CasRefCatalog::createNamespace`,
///      built on Task 2's `casAdmitEntry`);
///   2. `_ckpt` create (`CasRefCatalog::completeCreation`, step 2 -- Stage A's `publishCkpt` unchanged);
///   3. catalog CAS: `Creating -> Live`, re-presenting the creator's admission GENERATION and
///      value-CASing the OBSERVED entry (`completeCreation`, step 3 -- the "ZombieGoLive" guard) --
/// plus stale-`Creating` reconciliation (`CasRefCatalog::reconcileStaleCreator`) and the publication
/// gate (`checkPublicationAdmittedOrThrow`).
///
/// The suite name is prefixed `Cas` so it is covered by the `Cas*` unit-test gate filter.

namespace
{

/// A fence that never refuses, for tests whose subject is not the fence -- same helper, same intent,
/// as `gtest_cas_ref_ckpt.cpp`'s identically-named constant (not shared: each `_ckpt`/catalog test file
/// defines its own copy, matching that file's own precedent).
const std::function<void(uint64_t)> ALWAYS_ADMITTED = [](uint64_t) {};

/// A deadline far enough out that only the test's own contention decides the outcome -- mirrors
/// `gtest_cas_ref_ckpt.cpp`'s `generousDeadline`.
CkptDeadline generousDeadline()
{
    return CkptDeadline{[] { return uint64_t{1000}; }, 60000};
}

CreatorFence creatorFence(const String & srid, uint64_t writer_epoch, uint64_t fence_generation = 1)
{
    return CreatorFence{.server_root_id = srid, .writer_epoch = writer_epoch, .fence_generation = fence_generation};
}

/// A `is_creator_fence_terminal` stub that answers the same fixed verdict for every fence -- for tests
/// whose subject is not terminality itself (that predicate's own tests live in `gtest_cas_mount.cpp`,
/// next to `isCreatorFenceTerminal`, the real implementation this stub stands in for).
std::function<bool(const CreatorFence &)> fixedTerminality(bool terminal)
{
    return [terminal](const CreatorFence &) { return terminal; };
}

const CatalogEntry * findEntryForTest(const RefCatalog & catalog, const RootNamespace & ns)
{
    for (const CatalogEntry & e : catalog.entries)
        if (e.ns.string() == ns.string())
            return &e;
    return nullptr;
}

/// Raw lifecycle tests operate below `Pool::open`, so model an already-bootstrapped pool explicitly.
class InitializedCatalogBackend : public InMemoryBackend
{
public:
    InitializedCatalogBackend()
    {
        CasRefCatalog::initializeEmptyForNewPool(*this, Layout("p"));
    }
};

}

/// ---------------------------------------------------------------------------------------------
/// Happy path: all three writes land
/// ---------------------------------------------------------------------------------------------

TEST(CasNsCreationLifecycle, HappyPathReachesLiveWithADurableCkptAndAStableIncarnation)
{
    InitializedCatalogBackend backend;
    Layout layout("p");
    const RootNamespace ns{"a"};
    const CreatorFence creator = creatorFence("srv1", /*writer_epoch=*/5);

    const auto outcome = CasRefCatalog::createNamespace(
        backend, layout, ns, creator, /*admitted_generation=*/1, ALWAYS_ADMITTED, generousDeadline());
    EXPECT_EQ(outcome, CasRefCatalog::NamespaceCreationOutcome::Live);

    const CasRefCatalog::Snapshot snap = CasRefCatalog::read(backend, layout);
    const CatalogEntry * entry = findEntryForTest(snap.catalog, ns);
    ASSERT_NE(entry, nullptr);
    EXPECT_EQ(entry->state, NsState::Live);
    EXPECT_EQ(entry->creator, std::nullopt) << "creator is forbidden outside Creating (strict grammar)";
    const UInt128 incarnation = entry->incarnation;
    EXPECT_NE(incarnation, UInt128(0));

    const std::optional<CkptSample> ckpt = readCkpt(backend, layout, NamespaceLifeId::fromCatalogEntry(entry->ns, incarnation));
    ASSERT_TRUE(ckpt.has_value()) << "step 2's _ckpt must be durable";
    EXPECT_EQ(ckpt->ckpt.life_epoch, 5u) << "INV-4's genesis epoch is the creator's writer_epoch";

    /// Re-reading the catalog again must show the SAME incarnation -- nothing mints a second one.
    EXPECT_EQ(CasRefCatalog::read(backend, layout).catalog.entries.at(0).incarnation, incarnation);
}

/// ---------------------------------------------------------------------------------------------
/// `createNamespace` refuses a namespace that already has an entry (Task 2 review's own note: this
/// is Task 3's job, not `casAdmitEntry`'s duplicate-namespace grammar refusal).
/// ---------------------------------------------------------------------------------------------

#ifndef DEBUG_OR_SANITIZER_BUILD
TEST(CasNsCreationLifecycle, CreateNamespaceRejectsAnAlreadyExistingEntry)
{
    InitializedCatalogBackend backend;
    Layout layout("p");
    const RootNamespace ns{"a"};
    const CreatorFence creator = creatorFence("srv1", 1);
    ASSERT_EQ(CasRefCatalog::createNamespace(backend, layout, ns, creator, 1, ALWAYS_ADMITTED, generousDeadline()),
               CasRefCatalog::NamespaceCreationOutcome::Live);

    DB::Cas::tests::expectThrowsCode(DB::ErrorCodes::LOGICAL_ERROR, [&]
    {
        CasRefCatalog::createNamespace(backend, layout, ns, creatorFence("srv2", 2), 1, ALWAYS_ADMITTED, generousDeadline());
    });
}
#endif

#if defined(DEBUG_OR_SANITIZER_BUILD)
TEST(CasNsCreationLifecycleDeathTest, CreateNamespaceRejectsAnAlreadyExistingEntryAborts)
{
    InitializedCatalogBackend backend;
    Layout layout("p");
    const RootNamespace ns{"a"};
    const CreatorFence creator = creatorFence("srv1", 1);
    ASSERT_EQ(CasRefCatalog::createNamespace(backend, layout, ns, creator, 1, ALWAYS_ADMITTED, generousDeadline()),
               CasRefCatalog::NamespaceCreationOutcome::Live);

    EXPECT_DEATH(
        {
            CasRefCatalog::createNamespace(backend, layout, ns, creatorFence("srv2", 2), 1, ALWAYS_ADMITTED, generousDeadline());
        },
        "already carries a catalog entry");
}
#endif

/// ---------------------------------------------------------------------------------------------
/// `Creating` forbids publication
/// ---------------------------------------------------------------------------------------------

TEST(CasNsCreationLifecycle, CreatingForbidsPublication)
{
    RefCatalog catalog;
    catalog.entries.push_back(CatalogEntry{.ns = RootNamespace{"a"}, .state = NsState::Creating,
        .incarnation = UInt128(1), .creator = creatorFence("srv1", 1)});
    DB::Cas::tests::expectThrowsCode(DB::ErrorCodes::NETWORK_ERROR,
        [&] { CasRefCatalog::checkPublicationAdmittedOrThrow(catalog, RootNamespace{"a"}); });
}

TEST(CasNsCreationLifecycle, LiveAndRemovingAndAbsentAllAdmitPublication)
{
    RefCatalog catalog;
    catalog.entries.push_back(CatalogEntry{.ns = RootNamespace{"live"}, .state = NsState::Live, .incarnation = UInt128(1)});
    catalog.entries.push_back(CatalogEntry{.ns = RootNamespace{"removing"}, .state = NsState::Removing, .incarnation = UInt128(2)});
    EXPECT_NO_THROW(CasRefCatalog::checkPublicationAdmittedOrThrow(catalog, RootNamespace{"live"}));
    EXPECT_NO_THROW(CasRefCatalog::checkPublicationAdmittedOrThrow(catalog, RootNamespace{"removing"}));
    EXPECT_NO_THROW(CasRefCatalog::checkPublicationAdmittedOrThrow(catalog, RootNamespace{"never-heard-of"}));
}

/// ---------------------------------------------------------------------------------------------
/// ZombieGoLive: fenced-out between the `_ckpt` publish and the `Creating -> Live` CAS
/// ---------------------------------------------------------------------------------------------

/// A fence callback that admits its FIRST call (spent by step 2's `publishCkpt`) and refuses every
/// call after (spent by step 3's `mutate`) -- deterministically reproducing "fenced out between the
/// `_ckpt` create and the `Creating -> Live` CAS" without a second thread or fault injection.
namespace
{
std::function<void(uint64_t)> admittedOnceThenFenced()
{
    auto calls = std::make_shared<int>(0);
    return [calls](uint64_t admitted)
    {
        if (++*calls > 1)
            throw DB::Exception(DB::ErrorCodes::NETWORK_ERROR, "fence generation moved since admission ({})", admitted);
    };
}
}

TEST(CasNsCreationLifecycle, FencedOutBetweenTheCkptPublishAndGoLiveRefusesAndLeavesEntryCreating)
{
    InitializedCatalogBackend backend;
    Layout layout("p");
    const RootNamespace ns{"a"};
    const CreatorFence creator = creatorFence("srv1", 5);

    const auto outcome = CasRefCatalog::createNamespace(
        backend, layout, ns, creator, /*admitted_generation=*/1, admittedOnceThenFenced(), generousDeadline());
    EXPECT_EQ(outcome, CasRefCatalog::NamespaceCreationOutcome::FencedOut);

    const CasRefCatalog::Snapshot snap = CasRefCatalog::read(backend, layout);
    const CatalogEntry * entry = findEntryForTest(snap.catalog, ns);
    ASSERT_NE(entry, nullptr);
    EXPECT_EQ(entry->state, NsState::Creating) << "step 3 never ran its CAS -- ZombieGoLive refuses before sending it";
    ASSERT_TRUE(entry->creator.has_value());
    EXPECT_EQ(*entry->creator, creator);

    /// Step 2's _ckpt DID land (it is not what the fence check gates) -- CKPT-FAILED-BIRTH-DEBRIS is a
    /// different mechanism (the OLD `RefOpKind::NamespaceBirth` writer, `Pool/CasRefLedger.cpp`); this
    /// driver's own `_ckpt` is simply left in place for whichever actor next reconciles this entry.
    EXPECT_TRUE(readCkpt(backend, layout, NamespaceLifeId::fromCatalogEntry(entry->ns, entry->incarnation)).has_value());
}

/// ---------------------------------------------------------------------------------------------
/// Token-stale: the observed entry no longer matches at the `Creating -> Live` CAS
/// ---------------------------------------------------------------------------------------------

TEST(CasNsCreationLifecycle, EntryStolenByAConcurrentReconcilerRefusesGoLiveAndLeavesTheStolenEntryAlone)
{
    InitializedCatalogBackend backend;
    Layout layout("p");
    const RootNamespace ns{"a"};
    const CreatorFence original_creator = creatorFence("srv1", 5);
    const CreatorFence thief = creatorFence("srv2", 9);

    /// Write 1 only -- models "crash after write 1": no _ckpt yet, entry still Creating.
    const CatalogEntry entry{.ns = ns, .state = NsState::Creating, .incarnation = UInt128(42), .creator = original_creator};
    CasRefCatalog::casAdmitEntry(backend, layout, entry);

    /// `check_fence_or_throw` is the seam this driver calls on EVERY attempt -- once inside step 2's
    /// `publishCkpt`, once more inside step 3's own `mutate` -- so smuggling a REAL concurrent write
    /// into it (rather than faking the outcome) has to land on the SECOND call specifically, or the
    /// steal itself would run twice (and the second run would see its own first result and refuse).
    /// This reproduces "stolen between the creator's _ckpt publish and its Creating -> Live CAS"
    /// without a second thread. The steal itself must succeed (asserted), so the mismatch
    /// `completeCreation` sees below is the entry ACTUALLY changing, not a contrived stub.
    auto calls = std::make_shared<int>(0);
    const std::function<void(uint64_t)> steal_before_the_go_live_cas = [&, calls](uint64_t)
    {
        if (++*calls == 2)
            ASSERT_EQ(CasRefCatalog::reconcileStaleCreator(backend, layout, entry, thief, fixedTerminality(true), /*admitted_generation=*/1, ALWAYS_ADMITTED),
                       CasRefCatalog::ReconcileCreatorOutcome::Reconciled);
    };

    const auto outcome = CasRefCatalog::completeCreation(
        backend, layout, entry, /*admitted_generation=*/1, steal_before_the_go_live_cas, generousDeadline());
    EXPECT_EQ(outcome, CasRefCatalog::NamespaceCreationOutcome::Superseded);

    /// `read`'s `Snapshot` is bound to a name here, not chained through a temporary: a `const
    /// CatalogEntry *` taken from `.catalog` of an unbound temporary dangles the instant the full
    /// expression ends, which every other site in this file (and the copy/paste that spread it) got
    /// wrong until ASan caught it.
    const CasRefCatalog::Snapshot snap_after = CasRefCatalog::read(backend, layout);
    const CatalogEntry * after = findEntryForTest(snap_after.catalog, ns);
    ASSERT_NE(after, nullptr);
    EXPECT_EQ(after->state, NsState::Creating) << "the ORIGINAL creator's attempt wrote nothing -- only the thief's CAS did";
    ASSERT_TRUE(after->creator.has_value());
    EXPECT_EQ(*after->creator, thief) << "the entry is exactly what the thief left it as, untouched by our refused attempt";
    EXPECT_EQ(after->incarnation, entry.incarnation) << "reconciliation never mints a fresh incarnation";
}

/// ---------------------------------------------------------------------------------------------
/// Both stale at once: fence moved AND the entry was stolen -- refused (fence checked first).
/// ---------------------------------------------------------------------------------------------

TEST(CasNsCreationLifecycle, BothFenceAndEntryStaleRefusesGoLiveViaTheFenceCheckWhichRunsFirst)
{
    InitializedCatalogBackend backend;
    Layout layout("p");
    const RootNamespace ns{"a"};
    const CreatorFence original_creator = creatorFence("srv1", 5);
    const CreatorFence thief = creatorFence("srv2", 9);

    const CatalogEntry entry{.ns = ns, .state = NsState::Creating, .incarnation = UInt128(42), .creator = original_creator};
    CasRefCatalog::casAdmitEntry(backend, layout, entry);

    /// Same steal as the test above, landing on the SECOND `check_fence_or_throw` call (step 3's own
    /// `mutate`, not step 2's `publishCkpt`) -- but this one ALSO throws on that same second call, so
    /// both axes go stale in the SAME `mutate` invocation. `completeCreation`'s fence check runs before
    /// its entry check (documented ordering), so this is reported `FencedOut`; the assertions below
    /// confirm the entry ALSO changed, so the test is not merely re-proving the fence-only case above.
    auto calls = std::make_shared<int>(0);
    const std::function<void(uint64_t)> steal_and_fence_before_the_go_live_cas = [&, calls](uint64_t admitted)
    {
        if (++*calls == 2)
        {
            ASSERT_EQ(CasRefCatalog::reconcileStaleCreator(backend, layout, entry, thief, fixedTerminality(true), /*admitted_generation=*/1, ALWAYS_ADMITTED),
                       CasRefCatalog::ReconcileCreatorOutcome::Reconciled);
            throw DB::Exception(DB::ErrorCodes::NETWORK_ERROR, "fence generation moved since admission ({})", admitted);
        }
    };

    const auto outcome = CasRefCatalog::completeCreation(
        backend, layout, entry, /*admitted_generation=*/1, steal_and_fence_before_the_go_live_cas, generousDeadline());
    EXPECT_EQ(outcome, CasRefCatalog::NamespaceCreationOutcome::FencedOut)
        << "both checks would refuse; the fence check speaks first by this driver's fixed ordering";

    const CasRefCatalog::Snapshot snap_after = CasRefCatalog::read(backend, layout);
    const CatalogEntry * after = findEntryForTest(snap_after.catalog, ns);
    ASSERT_NE(after, nullptr);
    ASSERT_TRUE(after->creator.has_value());
    EXPECT_EQ(*after->creator, thief) << "confirms the entry axis really did go stale too, not just the fence";
}

/// ---------------------------------------------------------------------------------------------
/// Stale-`Creating` reconciliation
/// ---------------------------------------------------------------------------------------------

TEST(CasNsCreationLifecycle, ReconcileRefusedWhileTheOriginalCreatorFenceIsStillLive)
{
    InitializedCatalogBackend backend;
    Layout layout("p");
    const RootNamespace ns{"a"};
    const CatalogEntry entry{.ns = ns, .state = NsState::Creating, .incarnation = UInt128(7),
                              .creator = creatorFence("srv1", 5)};
    CasRefCatalog::casAdmitEntry(backend, layout, entry);

    const auto outcome = CasRefCatalog::reconcileStaleCreator(
        backend, layout, entry, creatorFence("srv2", 9), fixedTerminality(false), /*admitted_generation=*/1, ALWAYS_ADMITTED);
    EXPECT_EQ(outcome, CasRefCatalog::ReconcileCreatorOutcome::CreatorFenceStillLive);

    const CasRefCatalog::Snapshot snap_after = CasRefCatalog::read(backend, layout);
    const CatalogEntry * after = findEntryForTest(snap_after.catalog, ns);
    ASSERT_NE(after, nullptr);
    EXPECT_EQ(*after, entry) << "refused -- nothing written";
}

TEST(CasNsCreationLifecycle, ReconcileSucceedsTokenExactlyAfterTheOriginalCreatorFenceIsTerminalThenResumesToLive)
{
    InitializedCatalogBackend backend;
    Layout layout("p");
    const RootNamespace ns{"a"};
    const CreatorFence original_creator = creatorFence("srv1", 5);
    const CreatorFence new_creator = creatorFence("srv2", 9);
    const CatalogEntry entry{.ns = ns, .state = NsState::Creating, .incarnation = UInt128(7), .creator = original_creator};
    CasRefCatalog::casAdmitEntry(backend, layout, entry);   /// "crash after write 1" -- no _ckpt yet

    ASSERT_EQ(CasRefCatalog::reconcileStaleCreator(backend, layout, entry, new_creator, fixedTerminality(true), /*admitted_generation=*/1, ALWAYS_ADMITTED),
               CasRefCatalog::ReconcileCreatorOutcome::Reconciled);

    CatalogEntry taken_over = entry;
    taken_over.creator = new_creator;
    const CasRefCatalog::Snapshot snap_mid = CasRefCatalog::read(backend, layout);
    const CatalogEntry * mid = findEntryForTest(snap_mid.catalog, ns);
    ASSERT_NE(mid, nullptr);
    EXPECT_EQ(*mid, taken_over) << "creator moved to the new actor; state and incarnation unchanged";

    const auto outcome = CasRefCatalog::completeCreation(
        backend, layout, taken_over, /*admitted_generation=*/1, ALWAYS_ADMITTED, generousDeadline());
    EXPECT_EQ(outcome, CasRefCatalog::NamespaceCreationOutcome::Live);

    const CasRefCatalog::Snapshot snap_final = CasRefCatalog::read(backend, layout);
    const CatalogEntry * final_entry = findEntryForTest(snap_final.catalog, ns);
    ASSERT_NE(final_entry, nullptr);
    EXPECT_EQ(final_entry->state, NsState::Live);
    EXPECT_EQ(final_entry->incarnation, entry.incarnation) << "the SAME incarnation throughout -- resumption, not rebirth";
    const std::optional<CkptSample> ckpt = readCkpt(backend, layout, NamespaceLifeId::fromCatalogEntry(final_entry->ns, final_entry->incarnation));
    ASSERT_TRUE(ckpt.has_value());
    EXPECT_EQ(ckpt->ckpt.life_epoch, new_creator.writer_epoch)
        << "the RESUMING actor's writer_epoch is the genesis epoch that actually landed";
}

/// "Stale token at reconciliation -> fail closed": a SECOND reconciler racing the first, both reading
/// the SAME stale `observed` before either writes.
TEST(CasNsCreationLifecycle, ReconcileFailsClosedWhenTheEntryAlreadyChanged)
{
    InitializedCatalogBackend backend;
    Layout layout("p");
    const RootNamespace ns{"a"};
    const CatalogEntry entry{.ns = ns, .state = NsState::Creating, .incarnation = UInt128(7),
                              .creator = creatorFence("srv1", 5)};
    CasRefCatalog::casAdmitEntry(backend, layout, entry);

    const CreatorFence first_reconciler = creatorFence("srv2", 9);
    const CreatorFence second_reconciler = creatorFence("srv3", 11);

    ASSERT_EQ(CasRefCatalog::reconcileStaleCreator(backend, layout, entry, first_reconciler, fixedTerminality(true), /*admitted_generation=*/1, ALWAYS_ADMITTED),
               CasRefCatalog::ReconcileCreatorOutcome::Reconciled);

    /// The second reconciler still holds the ORIGINAL `entry` it read before either of them wrote --
    /// token-exactness must refuse it even though the terminality predicate would still say yes.
    const auto outcome = CasRefCatalog::reconcileStaleCreator(
        backend, layout, entry, second_reconciler, fixedTerminality(true), /*admitted_generation=*/1, ALWAYS_ADMITTED);
    EXPECT_EQ(outcome, CasRefCatalog::ReconcileCreatorOutcome::EntryChanged);

    const CasRefCatalog::Snapshot snap_after = CasRefCatalog::read(backend, layout);
    const CatalogEntry * after = findEntryForTest(snap_after.catalog, ns);
    ASSERT_NE(after, nullptr);
    ASSERT_TRUE(after->creator.has_value());
    EXPECT_EQ(*after->creator, first_reconciler) << "the second reconciler's refused attempt changed nothing";
}

/// ---------------------------------------------------------------------------------------------
/// Preconditions: `completeCreation`/`reconcileStaleCreator` refuse anything but a `Creating` entry
/// with a creator fence -- a caller bug, not a race, hence `LOGICAL_ERROR`.
/// ---------------------------------------------------------------------------------------------

#ifndef DEBUG_OR_SANITIZER_BUILD
TEST(CasNsCreationLifecycle, CompleteCreationRejectsANonCreatingEntry)
{
    InitializedCatalogBackend backend;
    Layout layout("p");
    const CatalogEntry live{.ns = RootNamespace{"a"}, .state = NsState::Live, .incarnation = UInt128(1)};
    DB::Cas::tests::expectThrowsCode(DB::ErrorCodes::LOGICAL_ERROR, [&]
    {
        CasRefCatalog::completeCreation(backend, layout, live, 1, ALWAYS_ADMITTED, generousDeadline());
    });
}

TEST(CasNsCreationLifecycle, ReconcileStaleCreatorRejectsANonCreatingEntry)
{
    InitializedCatalogBackend backend;
    Layout layout("p");
    const CatalogEntry live{.ns = RootNamespace{"a"}, .state = NsState::Live, .incarnation = UInt128(1)};
    DB::Cas::tests::expectThrowsCode(DB::ErrorCodes::LOGICAL_ERROR, [&]
    {
        CasRefCatalog::reconcileStaleCreator(backend, layout, live, creatorFence("srv2", 2), fixedTerminality(true), /*admitted_generation=*/1, ALWAYS_ADMITTED);
    });
}
#endif

#if defined(DEBUG_OR_SANITIZER_BUILD)
TEST(CasNsCreationLifecycleDeathTest, CompleteCreationRejectsANonCreatingEntryAborts)
{
    InitializedCatalogBackend backend;
    Layout layout("p");
    const CatalogEntry live{.ns = RootNamespace{"a"}, .state = NsState::Live, .incarnation = UInt128(1)};
    EXPECT_DEATH(
        { CasRefCatalog::completeCreation(backend, layout, live, 1, ALWAYS_ADMITTED, generousDeadline()); },
        "not a Creating entry");
}

TEST(CasNsCreationLifecycleDeathTest, ReconcileStaleCreatorRejectsANonCreatingEntryAborts)
{
    InitializedCatalogBackend backend;
    Layout layout("p");
    const CatalogEntry live{.ns = RootNamespace{"a"}, .state = NsState::Live, .incarnation = UInt128(1)};
    EXPECT_DEATH(
        {
            CasRefCatalog::reconcileStaleCreator(backend, layout, live, creatorFence("srv2", 2), fixedTerminality(true), /*admitted_generation=*/1, ALWAYS_ADMITTED);
        },
        "not a Creating entry");
}
#endif
