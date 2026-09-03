#include "cas_test_helpers.h"
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Gc/CasNamespaceJanitor.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Gc/CasGcMaintenanceState.h>

using namespace DB::Cas;
using namespace DB::Cas::tests;

namespace
{

/// `readGcMaintenanceState` now takes an admitted `CasOperation`, which cannot bind to an rvalue: every
/// call site below goes through this helper rather than materializing its own throwaway operation.
GcMaintenanceReadResult readState(CasRequests & requests, const Layout & layout)
{
    auto op = requests.admit();
    return readGcMaintenanceState(op, layout);
}

class OrderedJanitorBackend : public CountingBackend
{
public:
    std::vector<String> events;

    DB::Cas::Backend::RawListPage list(const String & prefix, const String & cursor, size_t limit,
                                       DB::Cas::TransportAccess & access) override
    {
        if (prefix.ends_with("/cas/ns/"))
            events.push_back("list");
        return CountingBackend::list(prefix, cursor, limit, access);
    }

    std::optional<DB::Cas::Backend::Raw> read(const String & key, DB::Cas::TransportAccess & access) override
    {
        if (key.ends_with("/cas/ref_catalog"))
            events.push_back("catalog");
        return CountingBackend::read(key, access);
    }
};

class OmitFirstNamespacePageBackend : public CountingBackend
{
public:
    DB::Cas::Backend::RawListPage list(const String & prefix, const String & cursor, size_t limit,
                                       DB::Cas::TransportAccess & access) override
    {
        if (omit && prefix.ends_with("/cas/ns/"))
        {
            omit = false;
            return {};
        }
        return CountingBackend::list(prefix, cursor, limit, access);
    }
private:
    bool omit = true;
};

/// Adds per-key REMOVE counting on top of `CountingBackend`, which only totals it: no test here needs
/// more than one key distinguished, but `TokenlessListHeadsDeadKeysAndRetainsConcurrentReplacement`
/// needs that one.
class RemoveCountingBackend : public CountingBackend
{
public:
    DB::Cas::Backend::RawRemoval remove(const String & key, const String & expected_value,
                                        DB::Cas::TransportAccess & access) override
    {
        ++remove_counts[key];
        return CountingBackend::remove(key, expected_value, access);
    }
    uint64_t removeCount(const String & key) const
    {
        const auto it = remove_counts.find(key);
        return it == remove_counts.end() ? 0 : it->second;
    }
private:
    std::map<String, uint64_t> remove_counts;
};

class ReplaceBeforeJanitorDeleteBackend : public CountingBackend
{
public:
    DB::Cas::Backend::RawRemoval remove(const String & key, const String & expected_value,
                                        DB::Cas::TransportAccess & access) override
    {
        if (!replaced)
        {
            replaced = true;
            const auto current = InMemoryBackend::get(key);
            if (current)
                (void)InMemoryBackend::casPut(key, "winner", current->token);
        }
        return CountingBackend::remove(key, expected_value, access);
    }
private:
    bool replaced = false;
};

class TokenlessListBackend : public RemoveCountingBackend
{
public:
    DB::Cas::Backend::RawListPage list(const String & prefix, const String & cursor, size_t limit,
                                       DB::Cas::TransportAccess & access) override
    {
        DB::Cas::Backend::RawListPage page = RemoveCountingBackend::list(prefix, cursor, limit, access);
        for (auto & key : page.keys)
            key.value.reset();
        return page;
    }

    bool supportsListTokens() const override { return false; }

    std::optional<DB::Cas::Backend::RawMeta> head(const String & key, DB::Cas::TransportAccess & access) override
    {
        std::optional<DB::Cas::Backend::RawMeta> result = RemoveCountingBackend::head(key, access);
        if (!replaced && result && key == replace_on_head)
        {
            replaced = true;
            const HeadResult current = InMemoryBackend::head(key);
            if (current.exists)
                (void)InMemoryBackend::casPut(key, "winner", current.token);
        }
        return result;
    }

    String replace_on_head;

private:
    bool replaced = false;
};

class FenceLossDuringHeadBackend : public TokenlessListBackend
{
public:
    std::optional<DB::Cas::Backend::RawMeta> head(const String & key, DB::Cas::TransportAccess & access) override
    {
        std::optional<DB::Cas::Backend::RawMeta> result = TokenlessListBackend::head(key, access);
        fence_held = false;
        return result;
    }

    bool fence_held = true;
};

class CatalogAfterListBackend : public CountingBackend
{
public:
    explicit CatalogAfterListBackend(NamespaceLifeId life_) : protected_life(std::move(life_)) {}

    DB::Cas::Backend::RawListPage list(const String & prefix, const String & cursor, size_t limit,
                                       DB::Cas::TransportAccess & access) override
    {
        DB::Cas::Backend::RawListPage page = CountingBackend::list(prefix, cursor, limit, access);
        if (!published && prefix.ends_with("/cas/ns/"))
        {
            published = true;
            const String catalog_key = "p/cas/ref_catalog";
            /// This models a CONCURRENT actor's read, not the janitor's own -- counting it here would
            /// make `PostListCatalogCutProtectsConcurrentCreationWithOneGet`'s "exactly one get" assertion
            /// count this simulated actor's read as the janitor's, defeating the point of that assertion.
            const auto current = InMemoryBackend::get(catalog_key, {}); // NOLINT(bugprone-parent-virtual-call)
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
    DB::Cas::Backend::RawListPage list(const String & prefix, const String & cursor, size_t limit,
                                       DB::Cas::TransportAccess & access) override
    {
        if (prefix.ends_with("/cas/ns/") && !cursor.empty())
            throw std::runtime_error("backend rejected cursor");
        return CountingBackend::list(prefix, cursor, limit, access);
    }
};

class FailMaintenancePublicationBackend : public CountingBackend
{
public:
    std::expected<String, DB::Cas::Backend::RawConflict> write(const String & key, const String & bytes,
                                                               const std::optional<String> & expected_value,
                                                               DB::Cas::TransportAccess & access) override
    {
        if (fail_publication && key.ends_with("/gc/maintenance_state"))
            throw std::runtime_error("maintenance publication failed");
        return CountingBackend::write(key, bytes, expected_value, access);
    }
    bool fail_publication = false;
};

/// The catch-path reset in `NamespaceJanitor::runOnePage` fires on any LIST failure; this backend
/// additionally makes its own write unresolvable (an unmodeled exception, then a resolve read that
/// proves nothing), so `once` vs `standard` is actually distinguishable by attempt count.
class ThrowingListAndAmbiguousWriteBackend : public CountingBackend
{
public:
    DB::Cas::Backend::RawListPage list(const String &, const String &, size_t,
                                       DB::Cas::TransportAccess &) override
    {
        throw std::runtime_error("list failed");
    }
    std::expected<String, DB::Cas::Backend::RawConflict> write(const String &, const String &,
                                                               const std::optional<String> &,
                                                               DB::Cas::TransportAccess &) override
    {
        throw std::runtime_error("ambiguous write");
    }
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

TEST(CASNamespaceJanitor, DeletesDeadFilesAndCheckpointFromOnePostListCatalogCut)
{
    auto backend = std::make_shared<CountingBackend>();
    CasRequests requests(backend, Fence::open());
    const Layout layout("p");
    seedCatalog(*backend, layout);
    const auto dead = life("dead", 41);
    const String file = layout.namespaceFilesPrefix(dead) + "part/data.bin";
    const String ckpt = layout.refCkptKey(dead);
    ASSERT_EQ(backend->putIfAbsent(file, "file-bytes").outcome, PutOutcome::Done);
    ASSERT_EQ(backend->putIfAbsent(ckpt, "ckpt-bytes").outcome, PutOutcome::Done);
    backend->resetCounts();

    NamespaceJanitor janitor(requests, layout, 100);
    const NamespaceJanitorResult result = janitor.runOnePage(false, [] { return true; });

    EXPECT_EQ(result.pages, 1u);
    EXPECT_EQ(result.keys, 2u);
    EXPECT_EQ(result.deleted, 2u);
    EXPECT_FALSE(backend->get(file));
    EXPECT_FALSE(backend->get(ckpt));
    EXPECT_EQ(backend->listRequestCount(layout.namespaceRootPrefix()), 1u);
    EXPECT_EQ(backend->readRequestCount(layout.refCatalogKey()), 1u);
    EXPECT_EQ(readState(requests, layout).state, GcMaintenanceState{});
}

TEST(CASNamespaceJanitor, RetainsEveryCurrentLifecycleAndSuppressesAmbiguousCut)
{
    auto backend = std::make_shared<CountingBackend>();
    CasRequests requests(backend, Fence::open());
    const Layout layout("p");
    RefCatalog catalog;
    CatalogEntry creating{.ns = RootNamespace{"creating"}, .state = NsState::Creating, .incarnation = UInt128{51},
        .creator = CreatorFence{.server_root_id = "srv", .writer_epoch = 1, .fence_generation = 1}};
    CatalogEntry live{.ns = RootNamespace{"live"}, .state = NsState::Live, .incarnation = UInt128{52}};
    CatalogEntry removing{.ns = RootNamespace{"removing"}, .state = NsState::Removing, .incarnation = UInt128{53},
        .removal_started_round = 1};
    catalog.entries = {creating, live, removing};
    seedCatalog(*backend, layout, catalog);
    for (const auto & entry : catalog.entries)
        ASSERT_EQ(backend->putIfAbsent(layout.refCkptKey(
            NamespaceLifeId::fromCatalogEntry(entry.ns, entry.incarnation)), "keep").outcome, PutOutcome::Done);

    NamespaceJanitor janitor(requests, layout, 100);
    const auto result = janitor.runOnePage(false, [] { return true; });
    EXPECT_EQ(result.deleted, 0u);
    EXPECT_EQ(backend->removeRequests(), 0u);
}

TEST(CASNamespaceJanitor, CatalogFirstCreatingRetainsEveryObjectOfTheNewLife)
{
    auto backend = std::make_shared<CountingBackend>();
    CasRequests requests(backend, Fence::open());
    const Layout layout("p");
    const CatalogEntry creating{
        .ns = RootNamespace{"catalog-first"},
        .state = NsState::Creating,
        .incarnation = UInt128{54},
        .creator = CreatorFence{.server_root_id = "srv", .writer_epoch = 2, .fence_generation = 3}};
    seedCatalog(*backend, layout, RefCatalog{.entries = {creating}});

    /// The production creation order is the point: the catalog row is durable before either object.
    const NamespaceLifeId creating_life
        = NamespaceLifeId::fromCatalogEntry(creating.ns, creating.incarnation);
    const String ckpt = layout.refCkptKey(creating_life);
    const String file = layout.namespaceFilesPrefix(creating_life) + "data";
    ASSERT_EQ(backend->putIfAbsent(ckpt, "checkpoint").outcome, PutOutcome::Done);
    ASSERT_EQ(backend->putIfAbsent(file, "file").outcome, PutOutcome::Done);
    backend->resetCounts();

    const NamespaceJanitorResult result
        = NamespaceJanitor(requests, layout, 100).runOnePage(false, [] { return true; });

    EXPECT_EQ(result.deleted, 0u);
    EXPECT_EQ(backend->removeRequests(), 0u);
    EXPECT_EQ(backend->readRequestCount(layout.refCatalogKey()), 1u);
    EXPECT_TRUE(backend->get(ckpt));
    EXPECT_TRUE(backend->get(file));
}

TEST(CASNamespaceJanitor, CancelledCreatingCheckpointIsReclaimedThroughPublicLifecycle)
{
    auto backend = std::make_shared<CountingBackend>();
    CasRequests requests(backend, Fence::open());
    const Layout layout("p");
    const CatalogEntry creating{
        .ns = RootNamespace{"cancelled"},
        .state = NsState::Creating,
        .incarnation = UInt128{55},
        .creator = CreatorFence{.server_root_id = "dead-srv", .writer_epoch = 4, .fence_generation = 5}};
    seedCatalog(*backend, layout, RefCatalog{.entries = {creating}});
    const String ckpt = layout.refCkptKey(
        NamespaceLifeId::fromCatalogEntry(creating.ns, creating.incarnation));
    ASSERT_EQ(backend->putIfAbsent(ckpt, "cancelled-checkpoint").outcome, PutOutcome::Done);

    auto cancel_op = requests.admit();
    ASSERT_EQ(CasRefCatalog::cancelStalledCreating(
        cancel_op, layout, creating, [](const CreatorFence &) { return true; }),
        CasRefCatalog::StalledCreatingCancelOutcome::Cancelled);
    auto read_op = requests.admit();
    EXPECT_TRUE(CasRefCatalog::read(read_op, layout).catalog.entries.empty());

    const NamespaceJanitorResult result
        = NamespaceJanitor(requests, layout, 100).runOnePage(false, [] { return true; });
    EXPECT_EQ(result.deleted, 1u);
    EXPECT_FALSE(backend->get(ckpt));
}

TEST(CASNamespaceJanitor, SuppressionAndFenceLossDeleteNothing)
{
    auto backend = std::make_shared<CountingBackend>();
    CasRequests requests(backend, Fence::open());
    const Layout layout("p");
    seedCatalog(*backend, layout);
    const String first = layout.refCkptKey(life("dead-a", 61));
    const String second = layout.refCkptKey(life("dead-b", 62));
    ASSERT_EQ(backend->putIfAbsent(first, "first").outcome, PutOutcome::Done);
    ASSERT_EQ(backend->putIfAbsent(second, "second").outcome, PutOutcome::Done);

    NamespaceJanitor janitor(requests, layout, 1);
    EXPECT_EQ(janitor.runOnePage(true, [] { return true; }).deleted, 0u);
    EXPECT_EQ(readState(requests, layout).status, GcMaintenanceReadStatus::Absent)
        << "a globally suppressed page is undecided and must not mint cleanup progress";
    EXPECT_EQ(backend->writeRequests(), 0u);
    EXPECT_EQ(janitor.runOnePage(false, [] { return false; }).deleted, 0u);
    EXPECT_EQ(readState(requests, layout).status, GcMaintenanceReadStatus::Absent)
        << "fence loss must not mint progress past a page whose deletion was not authorized";
    EXPECT_TRUE(backend->get(first));
    EXPECT_TRUE(backend->get(second));
    EXPECT_EQ(backend->removeRequests(), 0u);
}

TEST(CASNamespaceJanitor, FenceLossOnRetainedOnlyPageDoesNotAdvanceCursor)
{
    auto backend = std::make_shared<CountingBackend>();
    CasRequests requests(backend, Fence::open());
    const Layout layout("p");
    const CatalogEntry current{
        .ns = RootNamespace{"current"}, .state = NsState::Live, .incarnation = UInt128{63}};
    seedCatalog(*backend, layout, RefCatalog{.entries = {current}});
    const NamespaceLifeId current_life
        = NamespaceLifeId::fromCatalogEntry(current.ns, current.incarnation);
    const String ckpt = layout.refCkptKey(current_life);
    const String file = layout.namespaceFilesPrefix(current_life) + "data";
    ASSERT_EQ(backend->putIfAbsent(ckpt, "checkpoint").outcome, PutOutcome::Done);
    ASSERT_EQ(backend->putIfAbsent(file, "file").outcome, PutOutcome::Done);

    const NamespaceJanitorResult result
        = NamespaceJanitor(requests, layout, 1).runOnePage(false, [] { return false; });

    EXPECT_EQ(result.deleted, 0u);
    EXPECT_EQ(backend->removeRequests(), 0u);
    EXPECT_TRUE(backend->get(ckpt));
    EXPECT_TRUE(backend->get(file));
    EXPECT_EQ(readState(requests, layout).status, GcMaintenanceReadStatus::Absent)
        << "a tenure that observes fence loss cannot publish progress even when every object was retained";
}

TEST(CASNamespaceJanitor, FenceLossAfterLastDeleteRetainsCursorWithoutRollingBackDelete)
{
    auto backend = std::make_shared<CountingBackend>();
    CasRequests requests(backend, Fence::open());
    const Layout layout("p");
    seedCatalog(*backend, layout);
    const String dead = layout.refCkptKey(life("dead-after-delete", 64));
    ASSERT_EQ(backend->putIfAbsent(dead, "dead").outcome, PutOutcome::Done);
    uint64_t fence_checks = 0;

    const NamespaceJanitorResult result
        = NamespaceJanitor(requests, layout, 1).runOnePage(false, [&] { return fence_checks++ == 0; });

    EXPECT_EQ(result.deleted, 1u);
    EXPECT_FALSE(backend->get(dead))
        << "the exact delete completed under the fence and is never rolled back";
    EXPECT_EQ(fence_checks, 2u)
        << "the fence must be checked before deletion and again immediately before cursor publication";
    EXPECT_EQ(readState(requests, layout).status, GcMaintenanceReadStatus::Absent)
        << "losing the fence after the delete keeps this page selected for an idempotent retry";
}

TEST(CASNamespaceJanitor, CursorResumesThenResetsAtEnd)
{
    auto backend = std::make_shared<CountingBackend>();
    CasRequests requests(backend, Fence::open());
    const Layout layout("p");
    seedCatalog(*backend, layout);
    const auto dead = life("dead", 71);
    ASSERT_EQ(backend->putIfAbsent(layout.namespaceFilesPrefix(dead) + "a", "a").outcome, PutOutcome::Done);
    ASSERT_EQ(backend->putIfAbsent(layout.namespaceFilesPrefix(dead) + "b", "b").outcome, PutOutcome::Done);

    NamespaceJanitor first_process(requests, layout, 1);
    EXPECT_EQ(first_process.runOnePage(false, [] { return true; }).deleted, 1u);
    const auto mid = readState(requests, layout);
    ASSERT_EQ(mid.status, GcMaintenanceReadStatus::Valid);
    ASSERT_TRUE(mid.state);
    EXPECT_FALSE(mid.state->janitor_cursor.empty());
    NamespaceJanitor restarted_process(requests, layout, 1);
    EXPECT_EQ(restarted_process.runOnePage(false, [] { return true; }).deleted, 1u);
    EXPECT_TRUE(readState(requests, layout).state->janitor_cursor.empty());
}

TEST(CASNamespaceJanitor, TakesOneCatalogCutAfterListingAndContinuesPastMalformedKey)
{
    auto backend = std::make_shared<OrderedJanitorBackend>();
    CasRequests requests(backend, Fence::open());
    const Layout layout("p");
    seedCatalog(*backend, layout);
    const auto dead = life("dead", 81);
    const String valid = layout.namespaceFilesPrefix(dead) + "data";
    const String malformed = layout.namespaceStreamRootPrefix() + "not-a-life/_log/1-1.zst";
    const String malformed_state = layout.namespaceStateRootPrefix() + "not-a-life/_ckpt";
    ASSERT_EQ(backend->putIfAbsent(valid, "v").outcome, PutOutcome::Done);
    ASSERT_EQ(backend->putIfAbsent(malformed, "bad").outcome, PutOutcome::Done);
    ASSERT_EQ(backend->putIfAbsent(malformed_state, "bad-state").outcome, PutOutcome::Done);
    backend->resetCounts();
    backend->events.clear();

    const auto result = NamespaceJanitor(requests, layout, 100).runOnePage(false, [] { return true; });
    EXPECT_EQ(result.deleted, 1u);
    EXPECT_FALSE(result.anomalies.empty());
    EXPECT_TRUE(backend->get(malformed));
    EXPECT_TRUE(backend->get(malformed_state));
    ASSERT_EQ(backend->events.size(), 2u);
    EXPECT_EQ(backend->events[0], "list");
    EXPECT_EQ(backend->events[1], "catalog");
    EXPECT_EQ(backend->readRequestCount(layout.refCatalogKey()), 1u);
}

TEST(CASNamespaceJanitor, MalformedKeyIsFinalAndAdvancesCursor)
{
    auto backend = std::make_shared<CountingBackend>();
    CasRequests requests(backend, Fence::open());
    const Layout layout("p");
    seedCatalog(*backend, layout);
    const String first = layout.namespaceStreamRootPrefix() + "bad-a/_log/1-1.zst";
    const String second = layout.namespaceStreamRootPrefix() + "bad-b/_log/1-1.zst";
    ASSERT_EQ(backend->putIfAbsent(first, "first").outcome, PutOutcome::Done);
    ASSERT_EQ(backend->putIfAbsent(second, "second").outcome, PutOutcome::Done);

    const NamespaceJanitorResult result
        = NamespaceJanitor(requests, layout, 1).runOnePage(false, [] { return true; });

    EXPECT_EQ(result.deleted, 0u);
    EXPECT_FALSE(result.anomalies.empty());
    EXPECT_TRUE(backend->get(first));
    EXPECT_TRUE(backend->get(second));
    const GcMaintenanceReadResult progress = readState(requests, layout);
    ASSERT_EQ(progress.status, GcMaintenanceReadStatus::Valid);
    ASSERT_TRUE(progress.state);
    EXPECT_FALSE(progress.state->janitor_cursor.empty())
        << "malformed keys are surfaced and skipped, but do not pin the cleanup cycle";
}

TEST(CASNamespaceJanitor, DuplicateCurrentLifeSuppressesWholePage)
{
    auto backend = std::make_shared<CountingBackend>();
    CasRequests requests(backend, Fence::open());
    const Layout layout("p");
    RefCatalog catalog;
    catalog.entries = {
        CatalogEntry{.ns = RootNamespace{"a"}, .state = NsState::Live, .incarnation = UInt128{91}},
        CatalogEntry{.ns = RootNamespace{"b"}, .state = NsState::Live, .incarnation = UInt128{91}}};
    seedCatalog(*backend, layout, catalog);
    const String dead_a = layout.refCkptKey(life("dead-a", 92));
    const String dead_b = layout.refCkptKey(life("dead-b", 93));
    ASSERT_EQ(backend->putIfAbsent(dead_a, "a").outcome, PutOutcome::Done);
    ASSERT_EQ(backend->putIfAbsent(dead_b, "b").outcome, PutOutcome::Done);
    const auto result = NamespaceJanitor(requests, layout, 1).runOnePage(false, [] { return true; });
    EXPECT_EQ(result.deleted, 0u);
    EXPECT_EQ(backend->removeRequests(), 0u);
    EXPECT_TRUE(backend->get(dead_a));
    EXPECT_TRUE(backend->get(dead_b));
    EXPECT_EQ(readState(requests, layout).status, GcMaintenanceReadStatus::Absent)
        << "an ambiguous catalog cut leaves the selected page undecided for an authoritative retry";
}

TEST(CASNamespaceJanitor, CorruptProgressResetsWithoutDeletingAndFilesOnlyOmittedCycleRetries)
{
    auto backend = std::make_shared<OmitFirstNamespacePageBackend>();
    CasRequests requests(backend, Fence::open());
    const Layout layout("p");
    seedCatalog(*backend, layout);
    const String dead = layout.namespaceFilesPrefix(life("dead", 101)) + "only-residue";
    ASSERT_EQ(backend->putIfAbsent(dead, "bytes").outcome, PutOutcome::Done);
    ASSERT_EQ(backend->putIfAbsent(layout.gcMaintenanceStateKey(), "corrupt").outcome, PutOutcome::Done);
    EXPECT_EQ(NamespaceJanitor(requests, layout, 100).runOnePage(false, [] { return true; }).deleted, 0u);
    EXPECT_TRUE(backend->get(dead));
    EXPECT_EQ(readState(requests, layout).status, GcMaintenanceReadStatus::Valid);
    EXPECT_EQ(NamespaceJanitor(requests, layout, 100).runOnePage(false, [] { return true; }).deleted, 0u);
    EXPECT_TRUE(backend->get(dead));
    EXPECT_EQ(NamespaceJanitor(requests, layout, 100).runOnePage(false, [] { return true; }).deleted, 1u);
    EXPECT_FALSE(backend->get(dead));
}

TEST(CASNamespaceJanitor, ExactTokenMismatchRetainsConcurrentReplacement)
{
    auto backend = std::make_shared<ReplaceBeforeJanitorDeleteBackend>();
    CasRequests requests(backend, Fence::open());
    const Layout layout("p");
    seedCatalog(*backend, layout);
    const String dead = layout.refCkptKey(life("dead-a", 111));
    const String later = layout.refCkptKey(life("dead-b", 112));
    ASSERT_EQ(backend->putIfAbsent(dead, "old").outcome, PutOutcome::Done);
    ASSERT_EQ(backend->putIfAbsent(later, "later").outcome, PutOutcome::Done);
    const auto result = NamespaceJanitor(requests, layout, 1).runOnePage(false, [] { return true; });
    EXPECT_EQ(result.deleted, 0u);
    ASSERT_TRUE(backend->get(dead));
    EXPECT_EQ(backend->get(dead)->bytes, "winner");
    EXPECT_TRUE(backend->get(later));
    const GcMaintenanceReadResult progress = readState(requests, layout);
    ASSERT_EQ(progress.status, GcMaintenanceReadStatus::Valid);
    ASSERT_TRUE(progress.state);
    EXPECT_FALSE(progress.state->janitor_cursor.empty())
        << "an exact-token mismatch retains the rewrite but completes this page's decision";
}

TEST(CASNamespaceJanitor, TokenlessListHeadsDeadKeysAndRetainsConcurrentReplacement)
{
    auto backend = std::make_shared<TokenlessListBackend>();
    CasRequests requests(backend, Fence::open());
    const Layout layout("p");
    const CatalogEntry current{
        .ns = RootNamespace{"current"}, .state = NsState::Live, .incarnation = UInt128{161}};
    seedCatalog(*backend, layout, RefCatalog{.entries = {current}});
    const String live_key = layout.refCkptKey(NamespaceLifeId::fromCatalogEntry(current.ns, current.incarnation));
    const String dead_key = layout.refCkptKey(life("dead", 162));
    const String raced_key = layout.namespaceFilesPrefix(life("raced", 163)) + "data";
    ASSERT_EQ(backend->putIfAbsent(live_key, "live").outcome, PutOutcome::Done);
    ASSERT_EQ(backend->putIfAbsent(dead_key, "dead").outcome, PutOutcome::Done);
    ASSERT_EQ(backend->putIfAbsent(raced_key, "old").outcome, PutOutcome::Done);
    backend->replace_on_head = raced_key;
    backend->resetCounts();

    const auto result = NamespaceJanitor(requests, layout, 100).runOnePage(false, [] { return true; });

    EXPECT_EQ(result.deleted, 1u);
    EXPECT_TRUE(result.anomalies.empty());
    EXPECT_TRUE(backend->get(live_key));
    EXPECT_FALSE(backend->get(dead_key));
    ASSERT_TRUE(backend->get(raced_key));
    EXPECT_EQ(backend->get(raced_key)->bytes, "winner");
    EXPECT_EQ(backend->headRequestCount(live_key), 0u);
    EXPECT_EQ(backend->headRequestCount(dead_key), 1u);
    EXPECT_EQ(backend->headRequestCount(raced_key), 1u);
    EXPECT_EQ(backend->removeCount(dead_key), 1u);
    EXPECT_EQ(backend->removeCount(raced_key), 1u);
}

TEST(CASNamespaceJanitor, TokenlessListRechecksFenceAfterHeadBeforeDelete)
{
    auto backend = std::make_shared<FenceLossDuringHeadBackend>();
    CasRequests requests(backend, Fence::open());
    const Layout layout("p");
    seedCatalog(*backend, layout);
    const String dead_key = layout.refCkptKey(life("dead", 164));
    ASSERT_EQ(backend->putIfAbsent(dead_key, "dead").outcome, PutOutcome::Done);
    backend->resetCounts();

    const auto result = NamespaceJanitor(requests, layout, 100).runOnePage(
        false, [&] { return backend->fence_held; });

    EXPECT_EQ(result.deleted, 0u);
    EXPECT_EQ(backend->headRequestCount(dead_key), 1u);
    EXPECT_EQ(backend->removeCount(dead_key), 0u);
    EXPECT_TRUE(backend->get(dead_key));
}

TEST(CASNamespaceJanitor, PostListCatalogCutProtectsConcurrentCreationWithOneGet)
{
    const auto created = life("created", 121);
    auto backend = std::make_shared<CatalogAfterListBackend>(created);
    CasRequests requests(backend, Fence::open());
    const Layout layout("p");
    seedCatalog(*backend, layout);
    const String first = layout.refCkptKey(created);
    const String second = layout.namespaceFilesPrefix(created) + "data";
    ASSERT_EQ(backend->putIfAbsent(first, "ckpt").outcome, PutOutcome::Done);
    ASSERT_EQ(backend->putIfAbsent(second, "file").outcome, PutOutcome::Done);
    backend->resetCounts();
    const auto result = NamespaceJanitor(requests, layout, 100).runOnePage(false, [] { return true; });
    EXPECT_EQ(result.deleted, 0u);
    EXPECT_EQ(backend->removeRequests(), 0u);
    EXPECT_EQ(backend->readRequestCount(layout.refCatalogKey()), 1u);
    EXPECT_TRUE(backend->get(first));
    EXPECT_TRUE(backend->get(second));
}

TEST(CASNamespaceJanitor, BackendRejectedCursorResetsExactlyAndDeletesNothing)
{
    auto backend = std::make_shared<RejectCursorBackend>();
    CasRequests requests(backend, Fence::open());
    const Layout layout("p");
    seedCatalog(*backend, layout);
    const String dead = layout.refCkptKey(life("dead", 131));
    ASSERT_EQ(backend->putIfAbsent(dead, "bytes").outcome, PutOutcome::Done);
    ASSERT_EQ(backend->putIfAbsent(layout.gcMaintenanceStateKey(),
        encodeGcMaintenanceState({.janitor_cursor = "rejected"})).outcome, PutOutcome::Done);
    EXPECT_THROW(NamespaceJanitor(requests, layout, 100).runOnePage(false, [] { return true; }), std::runtime_error);
    EXPECT_EQ(backend->removeRequests(), 0u);
    EXPECT_TRUE(backend->get(dead));
    EXPECT_TRUE(readState(requests, layout).state->janitor_cursor.empty());
}

TEST(CASNamespaceJanitor, CursorPublicationFailureIsLeakOnly)
{
    auto backend = std::make_shared<FailMaintenancePublicationBackend>();
    CasRequests requests(backend, Fence::open());
    const Layout layout("p");
    seedCatalog(*backend, layout);
    const String dead = layout.refCkptKey(life("dead", 141));
    ASSERT_EQ(backend->putIfAbsent(dead, "bytes").outcome, PutOutcome::Done);
    backend->fail_publication = true;
    const auto result = NamespaceJanitor(requests, layout, 100).runOnePage(false, [] { return true; });
    EXPECT_EQ(result.deleted, 1u);
    EXPECT_FALSE(result.anomalies.empty());
    EXPECT_FALSE(backend->get(dead));
}

/// The write inside `catch (...)` (the reset after a LIST failure) is admitted `once`: an unmodeled,
/// unresolvable write attempt must give up after its one exact resolve read rather than looping through
/// `Retry::standard()`'s backoff -- proven here by both the physical write count and that no sleep, real
/// or fake, was ever asked for.
TEST(CASGcMaintenanceState, CatchPathWriteIsOnce)
{
    auto backend = std::make_shared<ThrowingListAndAmbiguousWriteBackend>();
    FakeClock clock;
    CasRequests requests(backend, Fence::open(), clock.nowFn(), clock.sleepFn());
    const Layout layout("p");

    EXPECT_THROW(
        (void)NamespaceJanitor(requests, layout, 100).runOnePage(false, [] { return true; }),
        std::runtime_error);
    EXPECT_EQ(backend->writeRequests(), 1u)
        << "the catch-path reset settles by its one resolve read and gives up rather than reissuing";
    EXPECT_TRUE(clock.sleeps.empty())
        << "Retry::once() never sleeps to reissue; Retry::standard() would have";
}

TEST(CASNamespaceJanitorIntegration, RegularGcRoundDeletesDeadNamespaceBytes)
{
    auto backend = std::make_shared<CountingBackend>();
    auto store = openPoolForTest(backend, /*gc_fold_max_defer_rounds=*/0);
    const Layout & layout = store->layout();
    const RootNamespace live_namespace{"00/live@cas@"};
    fixture::admitLive(*backend, layout, live_namespace);
    ASSERT_EQ(backend->putIfAbsent(layout.refCkptKey(fixture::fixtureLife(live_namespace)),
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
