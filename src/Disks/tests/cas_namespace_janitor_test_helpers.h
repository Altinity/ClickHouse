#pragma once

#include "cas_test_helpers.h"
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Gc/CasGcMaintenanceState.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Gc/CasNamespaceJanitor.h>
#include <Common/CurrentMetrics.h>
#include <Common/ThreadPool.h>
#include <base/scope_guard.h>
#include <algorithm>
#include <atomic>
#include <mutex>
#include <thread>

namespace CurrentMetrics
{
    extern const Metric LocalThread;
    extern const Metric LocalThreadActive;
    extern const Metric LocalThreadScheduled;
}

namespace DB::Cas::tests::janitor
{

inline std::unique_ptr<ThreadPool> makeJobPool(size_t threads)
{
    return std::make_unique<ThreadPool>(
        CurrentMetrics::LocalThread, CurrentMetrics::LocalThreadActive, CurrentMetrics::LocalThreadScheduled,
        /*max_threads*/ threads, /*max_free_threads*/ threads, /*queue_size*/ 0);
}

inline void createObj(Backend & backend, const String & key, const String & bytes)
{
    OperationForTest op(backend);
    ASSERT_TRUE(std::holds_alternative<Committed>((*op).create(key, bytes, Retry::once()))) << key;
}

inline std::optional<Object> readObj(Backend & backend, const String & key)
{
    OperationForTest op(backend);
    return (*op).read(key, Retry::standard());
}

inline bool present(Backend & backend, const String & key)
{
    return readObj(backend, key).has_value();
}

inline void seedCatalog(Backend & backend, const Layout & layout, RefCatalog catalog = {})
{
    createObj(backend, layout.refCatalogKey(), encodeRefCatalog(catalog));
}

inline NamespaceLifeId life(const char * name, uint64_t id)
{
    return NamespaceLifeId::fromCatalogEntry(RootNamespace{name}, UInt128{id});
}

/// `readGcMaintenanceState` takes an admitted operation, which cannot bind to an rvalue.
inline GcMaintenanceReadResult readState(CasRequests & requests, const Layout & layout)
{
    auto op = requests.admit();
    return readGcMaintenanceState(op, layout);
}

/// The published janitor cursor; `std::nullopt` while nothing was ever published.
inline std::optional<String> publishedCursor(CasRequests & requests, const Layout & layout)
{
    const GcMaintenanceReadResult state = readState(requests, layout);
    if (state.status != GcMaintenanceReadStatus::Valid || !state.state)
        return std::nullopt;
    return state.state->janitor_cursor;
}

/// `count` `_log` keys of `owner`, in listing order.
inline std::vector<String> seedLogs(Backend & backend, const Layout & layout, const NamespaceLifeId & owner, uint64_t count)
{
    std::vector<String> keys;
    keys.reserve(count);
    for (uint64_t i = 1; i <= count; ++i)
        keys.push_back(layout.refLogKey(owner, RefTxnId{1, i}));
    std::sort(keys.begin(), keys.end());
    for (const String & key : keys)
        createObj(backend, key, "log");
    return keys;
}

/// A live namespace with a checkpoint, the shape a GC round's fold expects; returns its life.
inline NamespaceLifeId seedLiveNamespace(Backend & backend, const Layout & layout)
{
    const RootNamespace live_namespace{"00/live@cas@"};
    fixture::admitLive(backend, layout, live_namespace);
    const NamespaceLifeId live = fixture::fixtureLife(live_namespace);
    createObj(backend, layout.refCkptKey(live),
        encodeRefCkpt(RefCkpt{.life_epoch = std::optional<uint64_t>{1}, .checkpoint_snapshot_id = std::nullopt, .last_epoch_seal = std::nullopt}));
    return live;
}

inline NamespaceJanitorResult runPhase(CasRequests & requests, const Layout & layout, const JanitorRunContext & context,
                                       bool suppress_deletes = false, size_t page_keys = 1000)
{
    NamespaceJanitorResult result;
    NamespaceJanitor(requests, layout, page_keys).run(suppress_deletes, context, result);
    return result;
}

/// One page, no refresh, sequential: the contract the single-page tests were written against.
inline NamespaceJanitorResult runOnePage(NamespaceJanitor janitor, bool suppress_deletes, Liveness liveness)
{
    NamespaceJanitorResult result;
    JanitorRunContext context;
    context.liveness = std::move(liveness);
    context.budget_ms = 0;
    janitor.run(suppress_deletes, context, result);
    return result;
}

/// The batch-capability store with the janitor's requests counted apart and hooks at fixed points. Hooks
/// run with no backend lock held; their own store access goes through the uncounted primitives.
class JanitorBackend : public BatchCapabilityBackend
{
public:
    using Access = TransportAccess;
    using Keys = std::vector<WriteOnceKey>;

    struct BulkCall
    {
        std::vector<String> keys;
        std::thread::id thread;
    };

    RawListPage list(const String & prefix, const String & cursor, size_t limit, Access & access) override
    {
        RawListPage page = BatchCapabilityBackend::list(prefix, cursor, limit, access);
        if (!prefix.ends_with("/cas/ns/"))
            return page;
        if (tokenless)
            for (auto & key : page.keys)
                key.value.reset();
        const size_t index = namespace_lists.fetch_add(1);
        if (after_namespace_list)
            after_namespace_list(index, access);
        return page;
    }

    bool supportsListTokens() const override { return !tokenless; }

    RawRemoval remove(const String & key, const String & expected_value, Access & access) override
    {
        {
            std::lock_guard lock(calls_mutex);
            exact_removes.push_back(key);
        }
        if (before_exact_remove)
            before_exact_remove(key, access);
        return BatchCapabilityBackend::remove(key, expected_value, access);
    }

    void removeManyWriteOnce(const Keys & keys, Access & access) override
    {
        {
            std::lock_guard lock(calls_mutex);
            BulkCall call{.keys = {}, .thread = std::this_thread::get_id()};
            for (const WriteOnceKey & key : keys)
                call.keys.push_back(key.str());
            bulk_calls.push_back(std::move(call));
        }
        if (before_bulk)
            before_bulk(keys, access);
        BatchCapabilityBackend::removeManyWriteOnce(keys, access);
        if (after_bulk)
            after_bulk(keys);
    }

    std::vector<BulkCall> bulkCalls() const
    {
        std::lock_guard lock(calls_mutex);
        return bulk_calls;
    }

    std::vector<String> exactRemoves() const
    {
        std::lock_guard lock(calls_mutex);
        return exact_removes;
    }

    size_t namespaceLists() const { return namespace_lists.load(); }

    /// A concurrent actor's delete of whatever `key` holds now.
    void removeUncounted(const String & key, Access & access)
    {
        if (const auto current = InMemoryBackend::read(key, access)) // NOLINT(bugprone-parent-virtual-call)
            (void)InMemoryBackend::remove(key, current->value, access); // NOLINT(bugprone-parent-virtual-call)
    }

    /// A concurrent actor's write: a create when `key` is absent, else a replacement.
    void writeUncounted(const String & key, const String & bytes, Access & access)
    {
        const auto current = InMemoryBackend::read(key, access); // NOLINT(bugprone-parent-virtual-call)
        const std::optional<String> expected = current ? std::optional<String>(current->value) : std::nullopt;
        ASSERT_TRUE(InMemoryBackend::write(key, bytes, expected, access).has_value()); // NOLINT(bugprone-parent-virtual-call)
    }

    bool tokenless = false;
    std::function<void(size_t index, Access &)> after_namespace_list;
    std::function<void(const String &, Access &)> before_exact_remove;
    std::function<void(const Keys &, Access &)> before_bulk;
    std::function<void(const Keys &)> after_bulk;

private:
    mutable std::mutex calls_mutex;
    std::vector<BulkCall> bulk_calls;
    std::vector<String> exact_removes;
    std::atomic<size_t> namespace_lists{0};
};

inline const Layout & janitorLayout()
{
    static const Layout layout("p");
    return layout;
}

/// A pool-less janitor: an open fence, a catalog, and a request and phase clock the test drives.
struct JanitorFixture
{
    std::shared_ptr<JanitorBackend> backend = std::make_shared<JanitorBackend>();
    FakeClock clock;
    CasRequests requests{backend, Fence::open(), clock.nowFn(), clock.sleepFn()};

    explicit JanitorFixture(RefCatalog catalog = {}) { seedCatalog(*backend, janitorLayout(), std::move(catalog)); }

    JanitorRunContext context(size_t batch_keys = kBulkDeleteMaxKeys)
    {
        JanitorRunContext result;
        result.batch_keys = batch_keys;
        result.now_ms = clock.nowFn();
        return result;
    }

    NamespaceJanitorResult run(const JanitorRunContext & context, bool suppress_deletes = false)
    {
        return runPhase(requests, janitorLayout(), context, suppress_deletes);
    }

    std::optional<String> cursor() { return publishedCursor(requests, janitorLayout()); }
};

}
