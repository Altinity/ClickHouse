#include "cas_test_helpers.h"
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Gc/CasNamespaceJanitor.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Gc/CasGcMaintenanceState.h>

using namespace DB::Cas;
using namespace DB::Cas::tests;

namespace
{

class OrderedJanitorBackend : public CountingBackend
{
public:
    using CountingBackend::get;
    std::vector<String> events;

    ListPage list(const String & prefix, const String & cursor, size_t limit) override
    {
        if (prefix.ends_with("/cas/ns/"))
            events.push_back("list");
        return CountingBackend::list(prefix, cursor, limit);
    }

    std::optional<GetResult> get(const String & key, Range range) override
    {
        if (key.ends_with("/cas/ref_catalog"))
            events.push_back("catalog");
        return CountingBackend::get(key, range);
    }
};

class OmitFirstNamespacePageBackend : public CountingBackend
{
public:
    ListPage list(const String & prefix, const String & cursor, size_t limit) override
    {
        if (omit && prefix.ends_with("/cas/ns/"))
        {
            omit = false;
            return {};
        }
        return CountingBackend::list(prefix, cursor, limit);
    }
private:
    bool omit = true;
};

class ReplaceBeforeJanitorDeleteBackend : public CountingBackend
{
public:
    DeleteOutcome deleteExact(const String & key, const Token & token) override
    {
        if (!replaced)
        {
            replaced = true;
            const auto current = InMemoryBackend::get(key);
            if (current)
                (void)InMemoryBackend::casPut(key, "winner", current->token);
        }
        return CountingBackend::deleteExact(key, token);
    }
private:
    bool replaced = false;
};

class CatalogAfterListBackend : public CountingBackend
{
public:
    explicit CatalogAfterListBackend(NamespaceLifeId life_) : protected_life(std::move(life_)) {}

    ListPage list(const String & prefix, const String & cursor, size_t limit) override
    {
        ListPage page = CountingBackend::list(prefix, cursor, limit);
        if (!published && prefix.ends_with("/cas/ns/"))
        {
            published = true;
            const String catalog_key = "p/cas/ref_catalog";
            const auto current = InMemoryBackend::get(catalog_key, {});
            if (current)
            {
                RefCatalog catalog;
                catalog.entries.push_back(CatalogEntry{.ns = protected_life.ns, .state = NsState::Live,
                    .incarnation = protected_life.incarnation});
                (void)InMemoryBackend::casPut(catalog_key, encodeRefCatalog(catalog), current->token);
            }
        }
        return page;
    }
private:
    NamespaceLifeId protected_life;
    bool published = false;
};

class RejectCursorBackend : public CountingBackend
{
public:
    ListPage list(const String & prefix, const String & cursor, size_t limit) override
    {
        if (prefix.ends_with("/cas/ns/") && !cursor.empty())
            throw std::runtime_error("backend rejected cursor");
        return CountingBackend::list(prefix, cursor, limit);
    }
};

class FailMaintenancePublicationBackend : public CountingBackend
{
public:
    CasResult casPut(const String & key, const String & bytes, const std::optional<Token> & expected,
        const ObjectMeta & meta) override
    {
        if (fail_publication && key.ends_with("/gc/maintenance_state"))
            throw std::runtime_error("maintenance publication failed");
        return CountingBackend::casPut(key, bytes, expected, meta);
    }
    bool fail_publication = false;
};

void seedCatalog(CountingBackend & backend, const Layout & layout, RefCatalog catalog = {})
{
    ASSERT_EQ(backend.putIfAbsent(layout.refCatalogKey(), encodeRefCatalog(catalog)).outcome, PutOutcome::Done);
}

NamespaceLifeId life(const char * name, uint64_t id)
{
    const RootNamespace ns{name};
    return NamespaceLifeId::fromCatalogEntry(ns, UInt128{id});
}

}

TEST(CasNamespaceJanitor, DeletesDeadFilesAndCheckpointFromOnePostListCatalogCut)
{
    CountingBackend backend;
    const Layout layout("p");
    seedCatalog(backend, layout);
    const auto dead = life("dead", 41);
    const String file = layout.namespaceFilesPrefix(dead) + "part/data.bin";
    const String ckpt = layout.refCkptKey(dead);
    ASSERT_EQ(backend.putIfAbsent(file, "file-bytes").outcome, PutOutcome::Done);
    ASSERT_EQ(backend.putIfAbsent(ckpt, "ckpt-bytes").outcome, PutOutcome::Done);
    backend.resetCounts();

    NamespaceJanitor janitor(backend, layout, 100);
    const NamespaceJanitorResult result = janitor.runOnePage(false, [] { return true; });

    EXPECT_EQ(result.pages, 1u);
    EXPECT_EQ(result.keys, 2u);
    EXPECT_EQ(result.deleted, 2u);
    EXPECT_FALSE(backend.get(file));
    EXPECT_FALSE(backend.get(ckpt));
    EXPECT_EQ(backend.listCount(layout.namespaceRootPrefix()), 1u);
    EXPECT_EQ(backend.getCount(layout.refCatalogKey()), 1u);
    EXPECT_EQ(readGcMaintenanceState(backend, layout).state, GcMaintenanceState{});
}

TEST(CasNamespaceJanitor, RetainsEveryCurrentLifecycleAndSuppressesAmbiguousCut)
{
    CountingBackend backend;
    const Layout layout("p");
    RefCatalog catalog;
    CatalogEntry creating{.ns = RootNamespace{"creating"}, .state = NsState::Creating, .incarnation = UInt128{51},
        .creator = CreatorFence{.server_root_id = "srv", .writer_epoch = 1, .fence_generation = 1}};
    CatalogEntry live{.ns = RootNamespace{"live"}, .state = NsState::Live, .incarnation = UInt128{52}};
    CatalogEntry removing{.ns = RootNamespace{"removing"}, .state = NsState::Removing, .incarnation = UInt128{53},
        .removal_started_round = 1};
    catalog.entries = {creating, live, removing};
    seedCatalog(backend, layout, catalog);
    for (const auto & entry : catalog.entries)
        ASSERT_EQ(backend.putIfAbsent(layout.refCkptKey(
            NamespaceLifeId::fromCatalogEntry(entry.ns, entry.incarnation)), "keep").outcome, PutOutcome::Done);

    NamespaceJanitor janitor(backend, layout, 100);
    const auto result = janitor.runOnePage(false, [] { return true; });
    EXPECT_EQ(result.deleted, 0u);
    EXPECT_EQ(backend.deleteTotal(), 0u);
}

TEST(CasNamespaceJanitor, SuppressionAndFenceLossDeleteNothing)
{
    CountingBackend backend;
    const Layout layout("p");
    seedCatalog(backend, layout);
    const auto dead = life("dead", 61);
    const String key = layout.refCkptKey(dead);
    ASSERT_EQ(backend.putIfAbsent(key, "bytes").outcome, PutOutcome::Done);

    NamespaceJanitor janitor(backend, layout, 100);
    EXPECT_EQ(janitor.runOnePage(true, [] { return true; }).deleted, 0u);
    EXPECT_EQ(janitor.runOnePage(false, [] { return false; }).deleted, 0u);
    EXPECT_TRUE(backend.get(key));
    EXPECT_EQ(backend.deleteTotal(), 0u);
}

TEST(CasNamespaceJanitor, CursorResumesThenResetsAtEnd)
{
    CountingBackend backend;
    const Layout layout("p");
    seedCatalog(backend, layout);
    const auto dead = life("dead", 71);
    ASSERT_EQ(backend.putIfAbsent(layout.namespaceFilesPrefix(dead) + "a", "a").outcome, PutOutcome::Done);
    ASSERT_EQ(backend.putIfAbsent(layout.namespaceFilesPrefix(dead) + "b", "b").outcome, PutOutcome::Done);

    NamespaceJanitor janitor(backend, layout, 1);
    EXPECT_EQ(janitor.runOnePage(false, [] { return true; }).deleted, 1u);
    const auto mid = readGcMaintenanceState(backend, layout);
    ASSERT_EQ(mid.status, GcMaintenanceReadStatus::Valid);
    ASSERT_TRUE(mid.state);
    EXPECT_FALSE(mid.state->janitor_cursor.empty());
    EXPECT_EQ(janitor.runOnePage(false, [] { return true; }).deleted, 1u);
    EXPECT_TRUE(readGcMaintenanceState(backend, layout).state->janitor_cursor.empty());
}

TEST(CasNamespaceJanitor, TakesOneCatalogCutAfterListingAndContinuesPastMalformedKey)
{
    OrderedJanitorBackend backend;
    const Layout layout("p");
    seedCatalog(backend, layout);
    const auto dead = life("dead", 81);
    const String valid = layout.namespaceFilesPrefix(dead) + "data";
    const String malformed = layout.namespaceStreamRootPrefix() + "not-a-life/_log/1-1.zst";
    const String malformed_state = layout.namespaceStateRootPrefix() + "not-a-life/_ckpt";
    ASSERT_EQ(backend.putIfAbsent(valid, "v").outcome, PutOutcome::Done);
    ASSERT_EQ(backend.putIfAbsent(malformed, "bad").outcome, PutOutcome::Done);
    ASSERT_EQ(backend.putIfAbsent(malformed_state, "bad-state").outcome, PutOutcome::Done);
    backend.resetCounts();
    backend.events.clear();

    const auto result = NamespaceJanitor(backend, layout, 100).runOnePage(false, [] { return true; });
    EXPECT_EQ(result.deleted, 1u);
    EXPECT_FALSE(result.anomalies.empty());
    EXPECT_TRUE(backend.get(malformed));
    EXPECT_TRUE(backend.get(malformed_state));
    ASSERT_EQ(backend.events.size(), 2u);
    EXPECT_EQ(backend.events[0], "list");
    EXPECT_EQ(backend.events[1], "catalog");
    EXPECT_EQ(backend.getCount(layout.refCatalogKey()), 1u);
}

TEST(CasNamespaceJanitor, DuplicateCurrentLifeSuppressesWholePage)
{
    CountingBackend backend;
    const Layout layout("p");
    RefCatalog catalog;
    catalog.entries = {
        CatalogEntry{.ns = RootNamespace{"a"}, .state = NsState::Live, .incarnation = UInt128{91}},
        CatalogEntry{.ns = RootNamespace{"b"}, .state = NsState::Live, .incarnation = UInt128{91}}};
    seedCatalog(backend, layout, catalog);
    const String dead = layout.refCkptKey(life("dead", 92));
    ASSERT_EQ(backend.putIfAbsent(dead, "bytes").outcome, PutOutcome::Done);
    const auto result = NamespaceJanitor(backend, layout, 100).runOnePage(false, [] { return true; });
    EXPECT_EQ(result.deleted, 0u);
    EXPECT_EQ(backend.deleteTotal(), 0u);
    EXPECT_TRUE(backend.get(dead));
}

TEST(CasNamespaceJanitor, CorruptProgressResetsWithoutDeletingAndOmittedCycleRetries)
{
    OmitFirstNamespacePageBackend backend;
    const Layout layout("p");
    seedCatalog(backend, layout);
    const String dead = layout.refCkptKey(life("dead", 101));
    ASSERT_EQ(backend.putIfAbsent(dead, "bytes").outcome, PutOutcome::Done);
    ASSERT_EQ(backend.putIfAbsent(layout.gcMaintenanceStateKey(), "corrupt").outcome, PutOutcome::Done);
    NamespaceJanitor janitor(backend, layout, 100);
    EXPECT_EQ(janitor.runOnePage(false, [] { return true; }).deleted, 0u);
    EXPECT_TRUE(backend.get(dead));
    EXPECT_EQ(readGcMaintenanceState(backend, layout).status, GcMaintenanceReadStatus::Valid);
    EXPECT_EQ(janitor.runOnePage(false, [] { return true; }).deleted, 0u);
    EXPECT_TRUE(backend.get(dead));
    EXPECT_EQ(janitor.runOnePage(false, [] { return true; }).deleted, 1u);
    EXPECT_FALSE(backend.get(dead));
}

TEST(CasNamespaceJanitor, ExactTokenMismatchRetainsConcurrentReplacement)
{
    ReplaceBeforeJanitorDeleteBackend backend;
    const Layout layout("p");
    seedCatalog(backend, layout);
    const String dead = layout.refCkptKey(life("dead", 111));
    ASSERT_EQ(backend.putIfAbsent(dead, "old").outcome, PutOutcome::Done);
    const auto result = NamespaceJanitor(backend, layout, 100).runOnePage(false, [] { return true; });
    EXPECT_EQ(result.deleted, 0u);
    ASSERT_TRUE(backend.get(dead));
    EXPECT_EQ(backend.get(dead)->bytes, "winner");
}

TEST(CasNamespaceJanitor, PostListCatalogCutProtectsConcurrentCreationWithOneGet)
{
    const auto created = life("created", 121);
    CatalogAfterListBackend backend(created);
    const Layout layout("p");
    seedCatalog(backend, layout);
    const String first = layout.refCkptKey(created);
    const String second = layout.namespaceFilesPrefix(created) + "data";
    ASSERT_EQ(backend.putIfAbsent(first, "ckpt").outcome, PutOutcome::Done);
    ASSERT_EQ(backend.putIfAbsent(second, "file").outcome, PutOutcome::Done);
    backend.resetCounts();
    const auto result = NamespaceJanitor(backend, layout, 100).runOnePage(false, [] { return true; });
    EXPECT_EQ(result.deleted, 0u);
    EXPECT_EQ(backend.deleteTotal(), 0u);
    EXPECT_EQ(backend.getCount(layout.refCatalogKey()), 1u);
    EXPECT_TRUE(backend.get(first));
    EXPECT_TRUE(backend.get(second));
}

TEST(CasNamespaceJanitor, BackendRejectedCursorResetsExactlyAndDeletesNothing)
{
    RejectCursorBackend backend;
    const Layout layout("p");
    seedCatalog(backend, layout);
    const String dead = layout.refCkptKey(life("dead", 131));
    ASSERT_EQ(backend.putIfAbsent(dead, "bytes").outcome, PutOutcome::Done);
    ASSERT_EQ(backend.putIfAbsent(layout.gcMaintenanceStateKey(),
        encodeGcMaintenanceState({.janitor_cursor = "rejected"})).outcome, PutOutcome::Done);
    EXPECT_THROW(NamespaceJanitor(backend, layout, 100).runOnePage(false, [] { return true; }), std::runtime_error);
    EXPECT_EQ(backend.deleteTotal(), 0u);
    EXPECT_TRUE(backend.get(dead));
    EXPECT_TRUE(readGcMaintenanceState(backend, layout).state->janitor_cursor.empty());
}

TEST(CasNamespaceJanitor, CursorPublicationFailureIsLeakOnly)
{
    FailMaintenancePublicationBackend backend;
    const Layout layout("p");
    seedCatalog(backend, layout);
    const String dead = layout.refCkptKey(life("dead", 141));
    ASSERT_EQ(backend.putIfAbsent(dead, "bytes").outcome, PutOutcome::Done);
    backend.fail_publication = true;
    const auto result = NamespaceJanitor(backend, layout, 100).runOnePage(false, [] { return true; });
    EXPECT_EQ(result.deleted, 1u);
    EXPECT_FALSE(result.anomalies.empty());
    EXPECT_FALSE(backend.get(dead));
}

TEST(CasNamespaceJanitorIntegration, RegularGcRoundDeletesDeadNamespaceBytes)
{
    auto backend = std::make_shared<CountingBackend>();
    auto store = openPoolForTest(backend, /*gc_fold_max_defer_rounds=*/0);
    const Layout & layout = store->layout();
    const RootNamespace live_namespace{"00/live@cas@"};
    casAdmitEntry(*backend, layout, live_namespace);
    ASSERT_EQ(backend->putIfAbsent(layout.refCkptKey(NamespaceLifeId::stageATransition(live_namespace)),
        encodeRefCkpt(RefCkpt{.life_epoch = std::optional<uint64_t>{1},
                              .checkpoint_snapshot_id = std::nullopt, .last_epoch_seal = std::nullopt})).outcome,
        PutOutcome::Done);
    const String dead = layout.refCkptKey(life("dead", 151));
    ASSERT_EQ(backend->putIfAbsent(dead, "checkpoint").outcome, PutOutcome::Done);

    std::map<String, UInt64> namespace_cleanup;
    Gc gc(store, UInt128{152});
    gc.setPhaseSink([&](const GcPhaseRecord & record)
    {
        if (record.phase == "namespace_cleanup")
            namespace_cleanup = record.metrics;
    });
    const RoundReport report = runRegularRoundReclaiming(gc);
    gc.setPhaseSink({});

    ASSERT_TRUE(report.acquired_lease);
    EXPECT_FALSE(backend->get(dead));
    ASSERT_FALSE(namespace_cleanup.empty());
    EXPECT_EQ(namespace_cleanup["janitor_pages"], 1u);
    EXPECT_GE(namespace_cleanup["janitor_keys"], 1u);
    EXPECT_EQ(namespace_cleanup["janitor_deleted"], 1u);
}
