#include "cas_test_helpers.h"

#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Gc/CasGc.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasServerRoot.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Tools/CasDecommission.h>

#include <algorithm>
#include <atomic>
#include <functional>
#include <optional>

namespace DB::ErrorCodes
{
    extern const int CORRUPTED_DATA;
}

using namespace DB;
using namespace DB::Cas;
using namespace DB::Cas::tests;

namespace
{

PoolPtr openVictim(const std::shared_ptr<InMemoryBackend> & backend)
{
    return Pool::open(backend, PoolConfig{.pool_prefix = "p", .server_root_id = "victim"});
}

CatalogEntry catalogEntry(CasOperation & op, const Layout & layout, const RootNamespace & ns)
{
    const RefCatalog catalog = CasRefCatalog::read(op, layout).catalog;
    const auto it = std::find_if(catalog.entries.begin(), catalog.entries.end(),
        [&](const CatalogEntry & entry) { return entry.ns == ns; });
    if (it == catalog.entries.end())
        throw Exception(ErrorCodes::CORRUPTED_DATA, "fixture catalog entry '{}' is absent", ns.string());
    return *it;
}

void makeRemoving(CasOperation & op, const Layout & layout, const CatalogEntry & live)
{
    CasRefCatalog::casUpdate(op, layout, [&](const RefCatalog & current)
    {
        RefCatalog next = current;
        const auto it = std::find(next.entries.begin(), next.entries.end(), live);
        if (it == next.entries.end())
            throw Exception(ErrorCodes::CORRUPTED_DATA, "fixture exact Live row changed");
        it->state = NsState::Removing;
        it->removal_started_round = 0;
        return next;
    });
}

bool slotObjectExists(Backend & backend, const String & leaf)
{
    DB::Cas::tests::OperationForTest op(backend);
    return (*op).head("p/gc/server-roots/victim/" + leaf, Retry::standard()).has_value();
}

OwnerObject readOwnerDuty(Backend & backend)
{
    DB::Cas::tests::OperationForTest op(backend);
    const auto current = (*op).read("p/gc/server-roots/victim/owner", Retry::standard());
    if (!current)
        throw Exception(ErrorCodes::CORRUPTED_DATA, "fixture owner is absent");
    return decodeOwner(current->bytes);
}

class AddVictimEntryDuringRootDrainBackend final : public InMemoryBackend
{
public:
    /// Unhide the legacy `list` overloads the primitive override below would otherwise hide.
    using Backend::list;
    void arm() { armed = true; }
    bool fired() const { return added; }

    /// Intercepted at the PRIMITIVE, which every legacy forwarder reaches too, so the injection fires
    /// whichever surface issued the enumeration.
    RawListPage list(const String & prefix, const String & cursor, size_t limit, TransportAccess & access) override
    {
        RawListPage page = InMemoryBackend::list(prefix, cursor, limit, access);
        if (armed && !added && prefix == "p/roots/victim/" && cursor.empty())
        {
            added = true;
            CasRequests requests = DB::Cas::tests::openRequestsForTest(*this);
            CasOperation op = requests.admit();
            CasRefCatalog::casAdmitEntry(
                op, Layout("p"), 1,
                CatalogEntry{
                    .ns = RootNamespace("victim/db/late"),
                    .state = NsState::Live,
                    .incarnation = UInt128{707}});
        }
        return page;
    }

private:
    bool armed = false;
    bool added = false;
};

/// Runs `on_tombstone` once, right after the first write of the victim's owner object: the decommission's
/// tombstone, written after its claim raised the victim's epoch and before its drain.
class AfterTombstoneBackend final : public InMemoryBackend
{
public:
    std::expected<String, RawConflict> write(
        const String & key, const String & bytes, const std::optional<String> & expected_value,
        TransportAccess & access) override
    {
        auto result = InMemoryBackend::write(key, bytes, expected_value, access);
        if (on_tombstone && key == "p/gc/server-roots/victim/owner")
        {
            std::function<void()> hook;
            std::swap(hook, on_tombstone);
            hook();
        }
        return result;
    }

    std::function<void()> on_tombstone;
};

/// After the victim's tombstone, runs `hook` once, right after the `n`-th read of the ref catalog.
class AtCatalogReadAfterTombstoneBackend final : public InMemoryBackend
{
public:
    void arm(size_t n, std::function<void()> hook_)
    {
        reads_to_wait = n;
        hook = std::move(hook_);
    }

    std::expected<String, RawConflict> write(
        const String & key, const String & bytes, const std::optional<String> & expected_value,
        TransportAccess & access) override
    {
        auto result = InMemoryBackend::write(key, bytes, expected_value, access);
        if (key == "p/gc/server-roots/victim/owner")
            counting = true;
        return result;
    }

    std::optional<Raw> read(const String & key, TransportAccess & access) override
    {
        auto result = InMemoryBackend::read(key, access);
        if (counting && hook && key == Layout("p").refCatalogKey() && --reads_to_wait == 0)
        {
            std::function<void()> fire;
            std::swap(fire, hook);
            fire();
        }
        return result;
    }

private:
    bool counting = false;
    size_t reads_to_wait = 0;
    std::function<void()> hook;
};

CatalogEntry seedVictimCreating(CasOperation & op, const PoolPtr & victim, const RootNamespace & ns)
{
    const CatalogEntry entry{
        .ns = ns,
        .state = NsState::Creating,
        .incarnation = UInt128{721},
        .creator = CreatorFence{.server_root_id = "victim", .writer_epoch = victim->writerEpoch(), .fence_generation = 1}};
    CasRefCatalog::casAdmitEntry(op, victim->layout(), victim->poolConfig().gc_shards, entry);
    return entry;
}

TEST(CASDecommissionCatalogDuties, RemovingWithoutCheckpointIsCorruptionAndLeavesAnUnverifiedTombstone)
{
    auto backend = std::make_shared<InMemoryBackend>();
    CasRequests catalog_requests = DB::Cas::tests::openRequestsForTest(backend);
    CasOperation catalog_op = catalog_requests.admit();
    {
        auto victim = openVictim(backend);
        const CatalogEntry live{
            .ns = RootNamespace("victim/db/missing_ckpt"),
            .state = NsState::Live,
            .incarnation = UInt128{701}};
        CasRefCatalog::casAdmitEntry(
            catalog_op, victim->layout(), victim->poolConfig().gc_shards,
            live);
        makeRemoving(catalog_op, victim->layout(), live);
    }

    expectThrowsCode(ErrorCodes::CORRUPTED_DATA, [&]
    {
        (void)decommissionPoolMember(
            backend, PoolConfig{.pool_prefix = "p", .server_root_id = "admin"}, "victim");
    });

    EXPECT_TRUE(slotObjectExists(*backend, "owner"));
    EXPECT_TRUE(slotObjectExists(*backend, "epoch"));
    EXPECT_TRUE(slotObjectExists(*backend, "mount"));
    EXPECT_TRUE(readOwnerDuty(*backend).retired_at_ms.has_value());
    EXPECT_FALSE(readOwnerDuty(*backend).retirement_verified);
    EXPECT_EQ(catalogEntry(catalog_op, Layout("p"), RootNamespace("victim/db/missing_ckpt")).state,
        NsState::Removing);
}

TEST(CASDecommissionCatalogDuties, RemovingWithCheckpointResumesTerminalAndIsVerifiedWithTheRowPendingGc)
{
    auto backend = std::make_shared<InMemoryBackend>();
    CasRequests catalog_requests = DB::Cas::tests::openRequestsForTest(backend);
    CasOperation catalog_op = catalog_requests.admit();
    const RootNamespace ns("victim/db/pending_terminal");
    std::optional<NamespaceLifeId> life;
    {
        auto victim = openVictim(backend);
        life = victim->namespaceLife(ns);
        const CatalogEntry live = catalogEntry(catalog_op, victim->layout(), ns);
        makeRemoving(catalog_op, victim->layout(), live);
        ASSERT_TRUE(catalog_op.head(victim->layout().refCkptKey(*life), Retry::standard()).has_value());
        ASSERT_TRUE(catalog_op.list(victim->layout().namespaceStreamPrefix(*life), "", 100, Retry::standard()).keys.empty());
    }

    std::atomic<uint64_t> wake_requests{0};
    const DecommissionReport report = decommissionPoolMember(
        backend, PoolConfig{.pool_prefix = "p", .server_root_id = "admin"}, "victim", {},
        [&] { wake_requests.fetch_add(1); });

    EXPECT_EQ(report.namespaces_already_removed, 1u);
    EXPECT_EQ(wake_requests.load(), 1u);
    EXPECT_TRUE(report.member_retired);
    EXPECT_TRUE(report.warnings.empty());
    EXPECT_EQ(report.namespaces_pending_gc, 1u);
    EXPECT_TRUE(slotObjectExists(*backend, "owner"));

    /// The admin mount writes in a later epoch than the creation, so the never-born stream first gets a
    /// tombstone at its creation slot, then the terminal record.
    const ListPage stream = catalog_op.list(Layout("p").namespaceStreamPrefix(*life), "", 100, Retry::standard());
    ASSERT_EQ(stream.keys.size(), 2u);
    const auto decodeAt = [&](size_t index)
    {
        const auto parsed = Layout("p").parseRefObjectKey(stream.keys[index].key);
        EXPECT_TRUE(parsed);
        const auto body = catalog_op.read(stream.keys[index].key, Retry::standard());
        EXPECT_TRUE(body);
        return decodeRefLogTxn(openObject(FormatId::RefLog, body->bytes), ns.string(), parsed->txn_id);
    };
    const RefLogTxn genesis = decodeAt(0);
    EXPECT_TRUE(refLogTxnIsEpochSeal(genesis));
    const RefLogTxn terminal = decodeAt(1);
    EXPECT_EQ(terminal.prev_epoch_seal, std::make_optional(genesis.txn_id));
    ASSERT_EQ(terminal.ops.size(), 2u);
    EXPECT_EQ(terminal.ops.front().kind, RefOpKind::NamespaceBirth);
    EXPECT_EQ(terminal.ops.back().kind, RefOpKind::RemoveNamespace);
}

TEST(CASDecommissionCatalogDuties, PartialRemovalProgressStillWakesGcWhenLaterNamespaceFails)
{
    auto backend = std::make_shared<InMemoryBackend>();
    CasRequests catalog_requests = DB::Cas::tests::openRequestsForTest(backend);
    CasOperation catalog_op = catalog_requests.admit();
    const RootNamespace progressed_ns("victim/db/a_progressed");
    const RootNamespace broken_ns("victim/db/z_missing_ckpt");
    std::optional<NamespaceLifeId> progressed_life;
    {
        auto victim = openVictim(backend);
        progressed_life = victim->namespaceLife(progressed_ns);
        const CatalogEntry progressed_live = catalogEntry(catalog_op, victim->layout(), progressed_ns);
        makeRemoving(catalog_op, victim->layout(), progressed_live);

        const CatalogEntry broken_live{
            .ns = broken_ns,
            .state = NsState::Live,
            .incarnation = UInt128{713}};
        CasRefCatalog::casAdmitEntry(
            catalog_op, victim->layout(), victim->poolConfig().gc_shards, broken_live);
        makeRemoving(catalog_op, victim->layout(), broken_live);
        ASSERT_FALSE(catalog_op.head(victim->layout().refCkptKey(
            NamespaceLifeId::fromCatalogEntry(broken_ns, broken_live.incarnation)), Retry::standard()).has_value());
    }

    std::atomic<uint64_t> wake_requests{0};
    expectThrowsCode(ErrorCodes::CORRUPTED_DATA, [&]
    {
        (void)decommissionPoolMember(
            backend, PoolConfig{.pool_prefix = "p", .server_root_id = "admin"}, "victim", {},
            [&] { wake_requests.fetch_add(1); });
    });

    EXPECT_EQ(wake_requests.load(), 1u)
        << "progress already made for an earlier life must wake GC even when a later life fails closed";
    EXPECT_TRUE(slotObjectExists(*backend, "owner"));
    EXPECT_TRUE(readOwnerDuty(*backend).retired_at_ms.has_value());
    EXPECT_FALSE(readOwnerDuty(*backend).retirement_verified);
    const ListPage progressed_stream
        = catalog_op.list(Layout("p").namespaceStreamPrefix(*progressed_life), "", 100, Retry::standard());
    ASSERT_EQ(progressed_stream.keys.size(), 2u) << "the creation slot's tombstone and the terminal record";
}

TEST(CASDecommissionCatalogDuties, VictimEntryAppearingAfterTheCutBlocksTheMark)
{
    auto backend = std::make_shared<AddVictimEntryDuringRootDrainBackend>();
    CasRequests catalog_requests = DB::Cas::tests::openRequestsForTest(backend);
    CasOperation catalog_op = catalog_requests.admit();
    { auto victim = openVictim(backend); }
    backend->arm();

    const DecommissionReport report = decommissionPoolMember(
        backend, PoolConfig{.pool_prefix = "p", .server_root_id = "admin"}, "victim");

    EXPECT_TRUE(backend->fired());
    EXPECT_FALSE(report.member_retired);
    ASSERT_FALSE(report.warnings.empty());
    EXPECT_NE(report.warnings.front().find("appeared after"), String::npos) << report.warnings.front();
    EXPECT_TRUE(readOwnerDuty(*backend).retired_at_ms.has_value());
    EXPECT_FALSE(readOwnerDuty(*backend).retirement_verified);
    EXPECT_EQ(catalogEntry(catalog_op, Layout("p"), RootNamespace("victim/db/late")).state, NsState::Live);
}

TEST(CASDecommissionCatalogDuties, FoldedTerminalRemainsGcOwnedAndOnlyRequestsAnotherRound)
{
    auto backend = std::make_shared<InMemoryBackend>();
    CasRequests catalog_requests = DB::Cas::tests::openRequestsForTest(backend);
    CasOperation catalog_op = catalog_requests.admit();
    const RootNamespace ns("victim/db/folded_terminal");
    std::optional<NamespaceLifeId> life;
    std::vector<String> stream_before;
    {
        PoolConfig config{
            .pool_prefix = "p",
            .server_root_id = "victim",
            .gc_fold_threshold = 1,
            .gc_fold_max_defer_rounds = 0};
        auto victim = Pool::open(backend, config);
        life = victim->namespaceLife(ns);
        victim->putNamespaceFile(*life, "format_version.txt", "1\n");
        victim->dropNamespace(ns);

        Gc gc(victim, UInt128{811});
        ASSERT_FALSE(runRegularRoundReclaiming(gc).deferred);
        ASSERT_EQ(catalogEntry(catalog_op, victim->layout(), ns).state, NsState::Removing);
        for (const ListedKey & key : catalog_op.list(victim->layout().namespaceStreamPrefix(*life), "", 100, Retry::standard()).keys)
            stream_before.push_back(key.key);
        ASSERT_FALSE(stream_before.empty());
    }

    std::atomic<uint64_t> wake_requests{0};
    const DecommissionReport report = decommissionPoolMember(
        backend, PoolConfig{.pool_prefix = "p", .server_root_id = "admin"}, "victim", {},
        [&] { wake_requests.fetch_add(1); });

    EXPECT_EQ(wake_requests.load(), 1u);
    EXPECT_EQ(report.namespaces_already_removed, 1u);
    EXPECT_TRUE(report.member_retired);
    EXPECT_TRUE(report.warnings.empty());
    EXPECT_EQ(report.namespaces_pending_gc, 1u);
    EXPECT_EQ(catalogEntry(catalog_op, Layout("p"), ns).state, NsState::Removing);
    std::vector<String> stream_after;
    for (const ListedKey & key : catalog_op.list(Layout("p").namespaceStreamPrefix(*life), "", 100, Retry::standard()).keys)
        stream_after.push_back(key.key);
    EXPECT_EQ(stream_after, stream_before)
        << "decommission must not append a second terminal or become a catalog deletion driver";
}

TEST(CASDecommissionCatalogDuties, OpaqueLifeDebrisWithoutCatalogOwnershipDoesNotBlockRetirement)
{
    auto backend = std::make_shared<InMemoryBackend>();
    CasRequests catalog_requests = DB::Cas::tests::openRequestsForTest(backend);
    CasOperation catalog_op = catalog_requests.admit();
    { auto victim = openVictim(backend); }
    const Layout layout("p");
    const NamespaceLifeId dead_life
        = NamespaceLifeId::fromCatalogEntry(RootNamespace("historical/name"), UInt128{709});
    const String debris_key = layout.refCkptKey(dead_life);
    ASSERT_TRUE(std::holds_alternative<Committed>(catalog_op.create(debris_key, "debris", Retry::standard())));

    const DecommissionReport report = decommissionPoolMember(
        backend, PoolConfig{.pool_prefix = "p", .server_root_id = "admin"}, "victim");

    EXPECT_TRUE(report.warnings.empty());
    EXPECT_TRUE(report.member_retired);
    EXPECT_TRUE(catalog_op.head(debris_key, Retry::standard()).has_value());
}

}

/// The claim raises the victim's epoch, which certifies its `Creating` row terminal: the decommission
/// cancels the row itself and retires the member.
TEST(CASDecommissionCatalogDuties, AVictimCreatingRowIsCancelledAndTheMemberRetires)
{
    auto backend = std::make_shared<InMemoryBackend>();
    CasRequests catalog_requests = DB::Cas::tests::openRequestsForTest(backend);
    CasOperation catalog_op = catalog_requests.admit();
    const RootNamespace ns("victim/db/stalled");
    {
        auto victim = openVictim(backend);
        seedVictimCreating(catalog_op, victim, ns);
    }

    const DecommissionReport report = decommissionPoolMember(
        backend, PoolConfig{.pool_prefix = "p", .server_root_id = "admin"}, "victim");

    EXPECT_TRUE(report.member_retired) << (report.warnings.empty() ? String{} : report.warnings.front());
    EXPECT_EQ(report.namespaces_removed, 1u);
    EXPECT_TRUE(CasRefCatalog::read(catalog_op, Layout("p")).catalog.entries.empty());
}

/// The GC leader cancels the victim's `Creating` row between the decommission's ownership cut and its
/// drain. The drain skips the row, which this run did not remove and does not count, and the member retires.
TEST(CASDecommissionCatalogDuties, AVictimCreatingRowCancelledByGcDuringTheRunIsSkippedAndNotCounted)
{
    auto backend = std::make_shared<AfterTombstoneBackend>();
    CasRequests catalog_requests = DB::Cas::tests::openRequestsForTest(backend);
    CasOperation catalog_op = catalog_requests.admit();
    const RootNamespace ns("victim/db/stalled");
    CatalogEntry stalled;
    {
        auto victim = openVictim(backend);
        stalled = seedVictimCreating(catalog_op, victim, ns);
    }
    bool cancelled = false;
    backend->on_tombstone = [&]
    {
        CasRequests gc_requests = DB::Cas::tests::openRequestsForTest(*backend);
        CasOperation gc_op = gc_requests.admit();
        const Layout layout("p");
        cancelled = CasRefCatalog::cancelStalledCreating(gc_op, layout, stalled,
            [&](const CreatorFence & creator)
            {
                return isCreatorFenceTerminal(gc_op, layout, creator.server_root_id, creator.writer_epoch);
            },
            [](const Retry &) {}) == CasRefCatalog::StalledCreatingCancelOutcome::Cancelled;
    };

    DecommissionReport report;
    ASSERT_NO_THROW(report = decommissionPoolMember(
        backend, PoolConfig{.pool_prefix = "p", .server_root_id = "admin"}, "victim"));

    EXPECT_TRUE(cancelled) << "positive control: the claim made the row cancellable and GC cancelled it";
    EXPECT_TRUE(report.member_retired) << (report.warnings.empty() ? String{} : report.warnings.front());
    EXPECT_EQ(report.namespaces_removed, 0u);
    EXPECT_EQ(report.namespaces_already_removed, 0u);
    EXPECT_TRUE(CasRefCatalog::read(catalog_op, Layout("p")).catalog.entries.empty());
}

/// The GC leader cancels the victim's `Creating` row after the drain checked it, at each catalog read up to the
/// drop's own cancel. The drop removed nothing, so the run neither counts nor announces the namespace.
TEST(CASDecommissionCatalogDuties, AVictimCreatingRowCancelledByGcDuringItsDropIsNotCounted)
{
    /// Earlier reads come before the drain's check; by the sixth, the drop has cancelled the row itself.
    for (const size_t reads_before_cancel : {3, 4, 5})
    {
        SCOPED_TRACE(fmt::format("cancel after catalog read {} after the tombstone", reads_before_cancel));
        auto backend = std::make_shared<AtCatalogReadAfterTombstoneBackend>();
        CasRequests catalog_requests = DB::Cas::tests::openRequestsForTest(backend);
        CasOperation catalog_op = catalog_requests.admit();
        const RootNamespace ns("victim/db/stalled");
        CatalogEntry stalled;
        {
            auto victim = openVictim(backend);
            stalled = seedVictimCreating(catalog_op, victim, ns);
        }
        bool cancelled = false;
        backend->arm(reads_before_cancel, [&]
        {
            CasRequests gc_requests = DB::Cas::tests::openRequestsForTest(*backend);
            CasOperation gc_op = gc_requests.admit();
            const Layout layout("p");
            cancelled = CasRefCatalog::cancelStalledCreating(gc_op, layout, stalled,
                [&](const CreatorFence & creator)
                {
                    return isCreatorFenceTerminal(gc_op, layout, creator.server_root_id, creator.writer_epoch);
                },
                [](const Retry &) {}) == CasRefCatalog::StalledCreatingCancelOutcome::Cancelled;
        });
        std::vector<String> outcomes;
        const CasEventSink sink = [&](CasEvent e)
        {
            if (e.type == CasEventType::MemberDecommission)
                outcomes.push_back(e.outcome);
        };

        DecommissionReport report;
        ASSERT_NO_THROW(report = decommissionPoolMember(
            backend, PoolConfig{.pool_prefix = "p", .server_root_id = "admin"}, "victim", sink));

        EXPECT_TRUE(cancelled) << "positive control: GC cancelled the row during the run";
        EXPECT_TRUE(report.member_retired) << (report.warnings.empty() ? String{} : report.warnings.front());
        EXPECT_EQ(report.namespaces_removed, 0u);
        EXPECT_EQ(std::count(outcomes.begin(), outcomes.end(), "namespace_removed"), 0);
        EXPECT_TRUE(CasRefCatalog::read(catalog_op, Layout("p")).catalog.entries.empty());
    }
}
