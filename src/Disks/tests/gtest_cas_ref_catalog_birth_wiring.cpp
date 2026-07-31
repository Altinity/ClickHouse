#include <gtest/gtest.h>

#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Backend/CasInMemoryBackend.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Formats/CasLayout.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasPool.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasRefCatalog.h>
#include <Disks/tests/cas_test_helpers.h>
#include <Common/Exception.h>

#include <limits>
#include <memory>
#include <vector>

namespace DB::ErrorCodes
{
extern const int NETWORK_ERROR;
}

/// Stage B Task 4-C: production birth wiring. `CasRefLedger::resolveNamespaceLife`, called from
/// `ensureRefTableRecovered`, resolves a namespace's real catalog life ONCE per table-open --
/// create-if-absent, adopt an existing `Live`/`Removing` entry, or reconcile a stale `Creating` one via
/// `CasRefCatalog::reconcileStaleCreator` + `isCreatorFenceTerminal` -- so every ref-layer object a
/// mounted writer produces is keyed at a real, catalog-proven incarnation (spec INV-3), never the
/// Stage-A sentinel.
///
/// OBLIGATION 3 (carried from Task 3's review, closed here): Task 3 could only enforce "`Creating`
/// forbids publication" (`CasRefCatalog::checkPublicationAdmittedOrThrow`) AT THE CATALOG LEVEL, because
/// nothing on the production ref-write path consulted the catalog at all. The refusal this suite pins
/// below rests on CONSTRUCTION, not a check: there is no `if (state == Creating) throw` anywhere in
/// `appendRefOps`'s path. `ensureRefTableRecovered` simply cannot make a table's runtime usable
/// (`rt.recovered` never becomes `true`, `rt.life` never gets set) while the catalog entry is `Creating`
/// under a fence that is not provably dead -- so no append can reach `commitRefChunk` for such a
/// namespace, by construction, stronger than any per-write check could prove. Stated here so nobody
/// later greps for a check and concludes the gap Task 3's review flagged is still open.
///
/// The suite name is prefixed `Cas` so it is covered by the `Cas*` unit-test gate filter.

using namespace DB::Cas;
using namespace DB::Cas::tests;

namespace
{

PoolPtr openPoolForBirthTest(const BackendPtr & backend, const String & server_root_id = "test")
{
    seedPoolMetaForRestart(*backend);
    return Pool::open(backend, PoolConfig{.pool_prefix = "p", .server_root_id = server_root_id});
}

const CatalogEntry * findEntry(const RefCatalog & catalog, const RootNamespace & ns)
{
    for (const CatalogEntry & e : catalog.entries)
        if (e.ns.string() == ns.string())
            return &e;
    return nullptr;
}

/// The same one-transaction publish `gtest_cas_ref_ckpt.cpp`'s `publishRef` drives: a namespace's first
/// append through the REAL append lane, which is also what triggers `resolveNamespaceLife`.
RefTxnId publishBirth(const PoolPtr & store, const RootNamespace & ns, const String & ref)
{
    return store->appendRefOps(ns, MutationScope::ref(ref),
        [&ref](const RefTableState & state)
        {
            std::vector<RefOp> ops;
            if (state.getLifecycle() != RefLifecycle::Live)
                ops.push_back(namespaceBirthOp());
            for (const RefOp & op : publishCommittedOps(ref, ManifestRef{1, 1, 1}))
                ops.push_back(op);
            return ops;
        },
        RootMutationOrigin::Writer, RootMutationKind::Publish);
}

}

/// The happy path: nothing to reconcile, no pre-existing entry. The first append mints a fresh `Live`
/// catalog entry and keys the birth transaction at it -- not at the Stage-A sentinel.
TEST(CasRefCatalogBirthWiring, FirstOpenMintsALiveCatalogEntryAndKeysTheBirthAtIt)
{
    auto backend = std::make_shared<InMemoryBackend>();
    auto store = openPoolForBirthTest(backend);
    const RootNamespace ns{"srv1/birth_wiring"};

    const RefTxnId id = publishBirth(store, ns, "a");
    EXPECT_EQ(id, (RefTxnId{store->writerEpoch(), 1}));

    const CasRefCatalog::Snapshot snap = CasRefCatalog::read(*backend, store->layout());
    const CatalogEntry * entry = findEntry(snap.catalog, ns);
    ASSERT_NE(entry, nullptr) << "the first open must mint a catalog entry";
    EXPECT_EQ(entry->state, NsState::Live);
    EXPECT_NE(entry->incarnation, UInt128(0));
    EXPECT_EQ(entry->creator, std::nullopt) << "creator is forbidden outside Creating (strict grammar)";

    const NamespaceLifeId life = NamespaceLifeId::fromCatalogEntry(entry->ns, entry->incarnation);
    EXPECT_TRUE(backend->head(store->layout().refLogKey(life, id)).exists)
        << "the birth transaction must be keyed at the REAL minted incarnation, not the Stage-A sentinel";
    EXPECT_FALSE(backend->head(store->layout().refLogKey(NamespaceLifeId::stageATransition(ns), id)).exists)
        << "and must NOT be keyed at the sentinel any more";
}

TEST(CasRefCatalogBirthWiring, CatalogLossAfterMountCannotRecreateAOneRowAuthority)
{
    auto backend = std::make_shared<CountingBackend>();
    auto store = openPoolForBirthTest(backend);
    const Layout & layout = store->layout();

    publishBirth(store, RootNamespace{"srv1/existing"}, "old");
    const auto catalog = backend->get(layout.refCatalogKey());
    ASSERT_TRUE(catalog);
    ASSERT_EQ(backend->deleteExact(layout.refCatalogKey(), catalog->token).kind,
              DeleteOutcome::Kind::Deleted);
    backend->resetCounts();

    EXPECT_THROW(publishBirth(store, RootNamespace{"srv1/new"}, "new"), DB::Exception);
    EXPECT_FALSE(backend->head(layout.refCatalogKey()).exists)
        << "runtime loss must not be repaired with a one-row replacement authority";
    EXPECT_EQ(backend->casPutTotal(), 0u);
    EXPECT_EQ(backend->putTotal(), 0u)
        << "the failed birth must not publish a checkpoint or ref-log body";
    EXPECT_EQ(backend->putOverwriteTotal(), 0u);
}

/// A namespace whose catalog entry is ALREADY `Live` (e.g. admitted by an earlier mount that this
/// runtime never cached) must be ADOPTED, never re-minted: `CasRefCatalog::createNamespace` refuses
/// outright once any entry exists, so `resolveNamespaceLife` has no create branch left to take here --
/// only the adopt branch can succeed.
TEST(CasRefCatalogBirthWiring, AnExistingLiveEntryIsAdoptedRatherThanReminted)
{
    auto backend = std::make_shared<InMemoryBackend>();
    auto store = openPoolForBirthTest(backend);
    const Layout & layout = store->layout();
    const RootNamespace ns{"srv1/adopt_live"};

    const CatalogEntry entry{.ns = ns, .state = NsState::Live, .incarnation = UInt128(0xcafe),
                             .creator = std::nullopt};
    CasRefCatalog::casAdmitEntry(*backend, layout, entry);

    const RefTxnId id = publishBirth(store, ns, "a");
    EXPECT_EQ(id, (RefTxnId{store->writerEpoch(), 1}));

    const CatalogEntry * after = findEntry(CasRefCatalog::read(*backend, layout).catalog, ns);
    ASSERT_NE(after, nullptr);
    EXPECT_EQ(after->incarnation, UInt128(0xcafe)) << "adopted, not re-minted";
    EXPECT_EQ(after->state, NsState::Live);

    const NamespaceLifeId life = NamespaceLifeId::fromCatalogEntry(ns, UInt128(0xcafe));
    EXPECT_TRUE(backend->head(layout.refLogKey(life, id)).exists);
}

/// OBLIGATION 3, pinned through the PRODUCTION path: a `Creating` entry left by a DIFFERENT, still-live
/// (or at least not provably dead) actor refuses every append -- no test-only seam, no direct call to
/// `resolveNamespaceLife`/`reconcileStaleCreator`, just an ordinary `appendRefOps`.
TEST(CasRefCatalogBirthWiring, ANamespaceStuckCreatingUnderALiveForeignFenceRefusesProductionPublicationByConstruction)
{
    auto backend = std::make_shared<InMemoryBackend>();
    auto store = openPoolForBirthTest(backend);
    const Layout & layout = store->layout();
    const RootNamespace ns{"srv1/stuck_creating"};

    /// A DIFFERENT actor's `Creating` entry naming a server root that never mounted at all --
    /// `isCreatorFenceTerminal`'s own doc: an ABSENT mount slot answers nothing about liveness, so it
    /// is treated as NOT terminal (fail closed), never as proof of death.
    const CreatorFence foreign_creator{.server_root_id = "ghost-server", .writer_epoch = 9, .fence_generation = 1};
    const CatalogEntry entry{.ns = ns, .state = NsState::Creating, .incarnation = UInt128(0xdead),
                             .creator = foreign_creator};
    CasRefCatalog::casAdmitEntry(*backend, layout, entry);

    expectThrowsCode(DB::ErrorCodes::NETWORK_ERROR, [&] { publishBirth(store, ns, "a"); });

    /// Nothing was written: the entry is exactly as observed, still Creating, still the foreign fence.
    const CatalogEntry * still = findEntry(CasRefCatalog::read(*backend, layout).catalog, ns);
    ASSERT_NE(still, nullptr);
    EXPECT_EQ(*still, entry) << "a refused resolution must write nothing";
}

/// The mirror image, and Task 3's own deferred obligation ("wire `reconcileStaleCreator` and pin it
/// with a test that drives reconciliation through the discovery path rather than by calling the
/// primitive directly"): a dead predecessor's `Creating` entry is reconciled onto THIS mount and
/// completed to `Live`, over the SAME incarnation -- resumption, not rebirth.
TEST(CasRefCatalogBirthWiring, AStaleCreatingEntryFromATerminatedForeignFenceIsReconciledThroughTheProductionPath)
{
    auto backend = std::make_shared<InMemoryBackend>();
    auto store = openPoolForBirthTest(backend, "this-server");
    const Layout & layout = store->layout();
    const RootNamespace ns{"srv1/reconciled"};

    /// A dead predecessor's `Creating` entry: its mount lease carries the clean-farewell sentinel
    /// (`min_active == UINT64_MAX`), one of `isCreatorFenceTerminal`'s three certificates of death.
    const CreatorFence dead_creator{.server_root_id = "dead-server", .writer_epoch = 3, .fence_generation = 1};
    const CatalogEntry entry{.ns = ns, .state = NsState::Creating, .incarnation = UInt128(0xbeef),
                             .creator = dead_creator};
    CasRefCatalog::casAdmitEntry(*backend, layout, entry);
    setWatermarkMinActive(*backend, layout, "dead-server", /*writer_epoch=*/3,
                          /*min_active=*/std::numeric_limits<uint64_t>::max());

    /// The production path resumes creation itself: reconciles the stale entry onto THIS mount's own
    /// fence and completes it to `Live`, over the SAME incarnation the dead creator minted.
    const RefTxnId id = publishBirth(store, ns, "a");
    EXPECT_EQ(id, (RefTxnId{store->writerEpoch(), 1}));

    const CatalogEntry * live = findEntry(CasRefCatalog::read(*backend, layout).catalog, ns);
    ASSERT_NE(live, nullptr);
    EXPECT_EQ(live->state, NsState::Live);
    EXPECT_EQ(live->incarnation, UInt128(0xbeef)) << "the SAME incarnation throughout -- resumption, not rebirth";
    EXPECT_EQ(live->creator, std::nullopt);

    const NamespaceLifeId life = NamespaceLifeId::fromCatalogEntry(ns, UInt128(0xbeef));
    EXPECT_TRUE(backend->head(layout.refLogKey(life, id)).exists);
}
