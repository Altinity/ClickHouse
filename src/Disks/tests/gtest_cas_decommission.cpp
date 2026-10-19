#include "cas_test_helpers.h"
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Tools/CasDecommission.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Backend/CasRequests.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Gc/CasGc.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasPartWriteTxn.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasServerRoot.h>
#include <IO/ReadHelpers.h>
#include <IO/WriteHelpers.h>
#include <Common/ProfileEvents.h>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <functional>
#include <mutex>
#include <limits>
#include <stdexcept>
#include <thread>
#include <tuple>
#include <utility>
#include <unistd.h>
#include <fmt/ranges.h>

namespace ProfileEvents
{
    extern const Event CASRemountFailed;
}

namespace DB::ErrorCodes
{
    extern const int ABORTED;
    extern const int BAD_ARGUMENTS;
    extern const int CORRUPTED_DATA;
}

using namespace DB;
using namespace DB::Cas;
using namespace DB::Cas::tests;

namespace
{

/// Open a store for the VICTIM srid over `backend` (the pool's future dead member).
PoolPtr openVictim(BackendPtr backend)
{
    return Pool::open(std::move(backend), PoolConfig{.pool_prefix = "p", .server_root_id = "victim"});
}

OwnerObject readOwner(Backend & backend, const Layout & layout, const String & srid)
{
    OperationForTest op(backend);
    const auto current = (*op).read(layout.ownerKey(srid), Retry::once());
    if (!current)
        throw std::runtime_error("owner absent: " + layout.ownerKey(srid));
    return decodeOwner(current->bytes);
}

MountLease readSlot(Backend & backend, const Layout & layout, const String & srid)
{
    OperationForTest op(backend);
    const auto current = (*op).read(layout.mountKey(srid), Retry::once());
    if (!current)
        throw std::runtime_error("mount absent: " + layout.mountKey(srid));
    return decodeMountLease(current->bytes);
}

void drainCompletedNamespaceRemovals(const std::shared_ptr<InMemoryBackend> & backend)
{
    PoolConfig config{
        .pool_prefix = "p",
        .server_root_id = "gc",
        .gc_fold_threshold = 1,
        .gc_fold_max_defer_rounds = 0};
    auto store = Pool::open(backend, config);
    Gc gc(store, UInt128{991});
    ASSERT_FALSE(runRegularRoundReclaiming(gc).deferred);
    ASSERT_FALSE(runRegularRoundReclaiming(gc).deferred);
}

/// Fails the delete of one key with a transient `Poco::TimeoutException` (the engine reissues it), and of
/// another with `Mismatch`. `latch` keeps the throw armed across every reissue, so the engine gives up.
class FailingDeleteBackend : public InMemoryBackend
{
public:
    void failWithThrow(const String & key) { throw_key = key; }
    void failWithTokenMismatch(const String & key) { mismatch_key = key; }
    void latch() { latched = true; }
    /// Clears every injected failure.
    void disarm() { throw_key.clear(); mismatch_key.clear(); latched = false; }

    RawRemoval remove(const String & key, const String & expected_value, TransportAccess & access) override
    {
        if (key == throw_key)
        {
            if (!latched)
                throw_key.clear();
            throw Poco::TimeoutException("injected transient delete failure for " + key);
        }
        if (key == mismatch_key)
            return RawRemoval::Mismatch;
        return InMemoryBackend::remove(key, expected_value, access);
    }

private:
    String throw_key;
    String mismatch_key;
    bool latched = false;
};

/// Fails a read of one designated key a fixed number of times with a transient `Poco::TimeoutException`
/// -- the class the request engine classifies as a transport failure and reissues -- before delegating
/// to the base `InMemoryBackend`. Drives the owner-object read `Pool::openForDecommission` itself issues
/// on the OPEN plane (`claimOwnerOrThrow` -> `readOwnerObject`, CasServerRoot.cpp) through a bounded
/// number of paced retries DURING opening, before `decommissionPoolMember` gets a chance to install
/// anything on the already-open `Pool`.
class FlakyReadBackend : public InMemoryBackend
{
public:
    void failReadNTimes(const String & key, int times) { flaky_key = key; remaining = times; }

    std::optional<Raw> read(const String & key, DB::Cas::TransportAccess & access) override
    {
        if (key == flaky_key && remaining > 0)
        {
            --remaining;
            throw Poco::TimeoutException("injected transient read failure for " + key);
        }
        return InMemoryBackend::read(key, access);
    }

private:
    String flaky_key;
    int remaining = 0;
};

/// Replaces the durable catalog immediately after returning the first armed catalog read. This
/// distinguishes the immutable cut validated before decommission impersonation from a later mount
/// safety observation without assuming those two decisions share one GET.
class CatalogChangesAfterFirstReadBackend : public InMemoryBackend
{
public:
    void armCatalogReplacement(
        const String & key, RefCatalog replacement_, size_t completed_reads_before_replacement = 0)
    {
        catalog_key = key;
        replacement = std::move(replacement_);
        reads_to_skip = completed_reads_before_replacement;
        armed = true;
    }

    bool fired() const { return replacement_fired; }

    /// Injects on the `read` PRIMITIVE: every caller reaches this key through `CasOperation::read`,
    /// which funnels through here.
    std::optional<Raw> read(const String & key, TransportAccess & access) override
    {
        auto got = InMemoryBackend::read(key, access);
        if (!armed || replacement_fired || key != catalog_key)
            return got;
        if (reads_to_skip > 0)
        {
            --reads_to_skip;
            return got;
        }
        if (!got)
            throw std::runtime_error("catalog replacement fixture: catalog is absent");

        replacement_fired = true;
        const auto put = InMemoryBackend::write(key, encodeRefCatalog(replacement), got->value, access);
        if (!put.has_value())
            throw std::runtime_error("catalog replacement fixture: rewrite conflicted");
        return got;
    }

private:
    String catalog_key;
    RefCatalog replacement;
    size_t reads_to_skip = 0;
    bool armed = false;
    bool replacement_fired = false;
};

std::vector<std::tuple<String, String, Etag>> snapshotPrefixObjects(
    InMemoryBackend & backend, const String & prefix)
{
    std::vector<std::tuple<String, String, Etag>> objects;
    auto requests = openRequestsForTest(backend);
    auto op = requests.admit();
    String cursor;
    while (true)
    {
        const ListPage page = op.list(prefix, cursor, 1000, Retry::once());
        for (const ListedKey & listed : page.keys)
        {
            const auto got = op.read(listed.key, Retry::once());
            if (!got)
                throw std::runtime_error("prefix snapshot fixture: listed object disappeared");
            objects.emplace_back(listed.key, got->bytes, got->etag);
        }
        if (page.next_cursor.empty())
            return objects;
        cursor = page.next_cursor;
    }
}

/// Once armed, swaps the owner to another uuid on the second owner read after the admin's slot claim
/// landed: the first is the claim recheck, the second is the tombstone's. Arm it after the victim closed,
/// or the victim's own lease writes count as the claim.
class SwapOwnerBeforeTombstoneBackend : public InMemoryBackend
{
public:
    void arm() { armed = true; }
    bool fired() const { return swapped; }

    std::expected<String, RawConflict> write(const String & key, const String & bytes,
                                             const std::optional<String> & expected_value, TransportAccess & access) override
    {
        const auto result = InMemoryBackend::write(key, bytes, expected_value, access);
        if (armed && result && key == mount_key && expected_value && !decodeMountLease(bytes).gc_fenced
            && !decodeMountLease(bytes).isFarewell())
            claimed = true;
        return result;
    }

    std::optional<Raw> read(const String & key, TransportAccess & access) override
    {
        if (claimed && !swapped && key == owner_key && ++owner_reads == 2)
        {
            const auto current = InMemoryBackend::read(key, access);
            OwnerObject foreign = nextOwnerLife(decodeOwner(current->bytes), std::nullopt, false);
            foreign.server_uuid = UInt128(0xBEEF);
            if (!InMemoryBackend::write(key, encodeOwner(foreign), current->value, access))
                throw std::runtime_error("owner swap fixture: the rewrite conflicted");
            swapped = true;
        }
        return InMemoryBackend::read(key, access);
    }

private:
    inline static const String mount_key = "p/gc/server-roots/victim/mount";
    inline static const String owner_key = "p/gc/server-roots/victim/owner";
    bool armed = false;
    bool claimed = false;
    bool swapped = false;
    int owner_reads = 0;
};

/// Once armed, fences the admin's body right before its farewell lands, so the farewell conflicts and
/// `MountLeaseRenewer::terminate` returns without an outcome. Arm it after the victim closed, or the
/// victim's own farewell is the one fenced.
class FenceBeforeFarewellBackend : public InMemoryBackend
{
public:
    void arm() { armed = true; }
    bool fired() const { return fenced; }

    std::expected<String, RawConflict> write(const String & key, const String & bytes,
                                             const std::optional<String> & expected_value, TransportAccess & access) override
    {
        if (armed && !fenced && key == mount_key && expected_value && decodeMountLease(bytes).isFarewell())
        {
            const auto current = InMemoryBackend::read(key, access);
            if (!InMemoryBackend::write(key, encodeMountLease(decodeMountLease(current->bytes).fencedCopy()), current->value, access))
                throw std::runtime_error("fence-before-farewell fixture: the fence conflicted");
            fenced = true;
        }
        return InMemoryBackend::write(key, bytes, expected_value, access);
    }

private:
    inline static const String mount_key = "p/gc/server-roots/victim/mount";
    bool armed = false;
    bool fenced = false;
};


/// Seed one victim table with `committed` committed refs and `precommits` dangling precommit bindings through
/// the raw ref-log helpers: only the ref-table shape `dropNamespace` erases, not a real build. Precommits
/// carry a high `writer_epoch`, so the writer's stale-precommit sweep never reclaims them in its own
/// transaction ahead of `dropNamespace`.
void makeTableWithRefs(Pool & victim, const String & ns_str, uint64_t committed, uint64_t precommits)
{
    const RootNamespace ns(ns_str);
    Backend & backend = *victim.poolBackendPtr();
    const Layout & layout = victim.layout();

    /// A throwaway open-fence operation, for the two `CasRefCatalog` calls below only: this fixture
    /// writes everything else directly against `backend` via the raw-write helpers, unrelated to any
    /// mount fence.
    CasRequests requests(victim.poolBackendPtr(), Fence::open());
    CasOperation op = requests.admit();

    /// Final physical ids are pool-wide. The generic raw-write helper intentionally uses one shared
    /// transition sentinel, so this multi-namespace fixture admits a distinct deterministic test life
    /// before invoking it; the helper then resolves and preserves that existing catalog identity.
    const CasRefCatalog::Snapshot catalog = CasRefCatalog::read(op, layout);
    const auto existing = std::find_if(catalog.catalog.entries.begin(), catalog.catalog.entries.end(),
        [&](const CatalogEntry & entry) { return entry.ns.string() == ns.string(); });
    if (existing == catalog.catalog.entries.end())
    {
        static std::atomic<uint64_t> next_test_life{1000};
        CatalogEntry entry;
        entry.ns = ns;
        entry.state = NsState::Live;
        entry.incarnation = UInt128{next_test_life.fetch_add(1)};
        CasRefCatalog::casAdmitEntry(op, layout, 1, entry);
    }

    uint64_t last_ref_sequence = 0;
    for (uint64_t i = 0; i < committed; ++i)
    {
        const ManifestRef ref{.writer_epoch = 1, .build_sequence = i + 1, .manifest_ordinal = 1};
        writeManifestRaw(backend, layout, ns, ref, {});
        last_ref_sequence = publishCommittedTransition(backend, layout, ns, "committed_" + std::to_string(i), std::nullopt, ref);
    }
    for (uint64_t i = 0; i < precommits; ++i)
    {
        const ManifestRef ref{.writer_epoch = 999999, .build_sequence = i + 1, .manifest_ordinal = 1};
        writeManifestRaw(backend, layout, ns, ref, {});
        last_ref_sequence = addPrecommitTransition(backend, layout, ns, UInt128(1), "precommit_" + std::to_string(i), std::nullopt, ref);
    }

    /// Semantic transition helpers already publish `_ckpt`; replace their final checkpoint through
    /// the exact token-CAS fixture helper to make this fixture's complete intended state explicit.
    replaceRecoverableCkptForRawFixture(backend, layout, ns, RefCkpt{
        .life_epoch = 1,
        .committed_through = last_ref_sequence ? std::optional<RefTxnId>{RefTxnId{1, last_ref_sequence}} : std::nullopt,
        .checkpoint_snapshot_id = std::nullopt,
        .last_epoch_seal = std::nullopt,
    });

    /// Self-checking: `listRefs` must observe exactly `committed` committed refs before returning.
    ASSERT_EQ(victim.listRefs(ns).size(), committed);
}

/// Manifest debris: a staged manifest body under `ns_str` at the store's own `writer_epoch` that no owner
/// event names, a build staged and never finished. `build_sequence = 99` stays clear of `makeTableWithRefs`.
ManifestId seedOrphanManifestBody(Pool & victim, const String & ns_str)
{
    const RootNamespace ns(ns_str);
    const ManifestRef ref{.writer_epoch = victim.writerEpoch(), .build_sequence = 99, .manifest_ordinal = 1};
    const ManifestId id = writeManifestRaw(*victim.poolBackendPtr(), victim.layout(), ns, ref, {});
    OperationForTest op(*victim.poolBackendPtr());
    EXPECT_TRUE((*op).head(victim.layout().manifestKey(id), Retry::once()).has_value());
    return id;
}

/// The manifest-debris drain deletes an epoch-`E` manifest only once the namespace's sealed fold cursor sits
/// above `E`. In these single-epoch fixtures no cursor can clear the debris without also clearing the removal
/// record, so the tests assert retention.

}


/// The pool "p" with the member "victim" every test decommissions; the helpers work on `victim_backend`.
class CASDecommission : public ::testing::Test
{
protected:
    const Layout layout{"p"};
    BackendPtr victim_backend;

    /// Opens the victim over `backend`, runs `prepare` on it and closes it; `backend` then backs every helper
    /// below.
    template <typename B = InMemoryBackend>
    std::shared_ptr<B> startVictim(std::shared_ptr<B> backend = std::make_shared<B>(), const std::function<void(Pool &)> & prepare = {})
    {
        victim_backend = backend;
        auto victim = openVictim(backend);
        if (prepare)
            prepare(*victim);
        return backend;
    }

    static void oneTable(Pool & victim) { makeTableWithRefs(victim, "victim/db/t1", 1, 0); }
    /// `oneTable` plus a staged manifest no ref names; returns the manifest's key.
    static String oneTableWithDebris(Pool & victim)
    {
        oneTable(victim);
        return victim.layout().manifestKey(seedOrphanManifestBody(victim, "victim/db/t1"));
    }

    /// The owner, epoch and mount objects, bytes and incarnation.
    std::vector<std::optional<std::pair<String, Etag>>> controlObjects() const
    {
        OperationForTest op(*victim_backend);
        std::vector<std::optional<std::pair<String, Etag>>> objects;
        for (const String & key : {layout.ownerKey("victim"), layout.epochKey("victim"), layout.mountKey("victim")})
        {
            const auto got = (*op).read(key, Retry::once());
            objects.push_back(got ? std::optional(std::pair(got->bytes, got->etag)) : std::nullopt);
        }
        return objects;
    }

    DecommissionReport run(const String & admin, PoolConfig config = {})
    {
        config.pool_prefix = "p";
        config.server_root_id = admin;
        return decommissionPoolMember(victim_backend, std::move(config), "victim");
    }

    /// The run's clock and retry sleep, and the admin's lease, on `clock`. The closures own the clock: the
    /// admin's renewer may outlive the call that installed them.
    DecommissionReport runOnClock(const String & admin, const std::shared_ptr<FakeClock> & clock, PoolConfig config = {})
    {
        config.pool_prefix = "p";
        config.server_root_id = admin;
        return decommissionPoolMember(victim_backend, std::move(config), "victim", /*sink=*/{}, /*request_gc_round=*/{},
            [clock] { return clock->nowFn()(); }, [clock](uint64_t ms) { clock->sleepFn()(ms); });
    }

    OwnerObject owner() const { return readOwner(*victim_backend, layout, "victim"); }
    MountLease slot() const { return readSlot(*victim_backend, layout, "victim"); }

    /// The owner fields a decommission decides: its life, whether it is retired, whether the retirement is verified.
    void expectOwner(uint64_t life, bool retired, bool verified) const
    {
        const OwnerObject current = owner();
        EXPECT_EQ(current.owner_life, life);
        EXPECT_EQ(current.retired_at_ms.has_value(), retired);
        EXPECT_EQ(current.retirement_verified, verified);
    }

    void seed(const String & key) const
    {
        OperationForTest op(*victim_backend);
        ASSERT_TRUE(std::holds_alternative<Committed>((*op).create(key, "x", Retry::once())));
    }

    bool exists(const String & key) const
    {
        OperationForTest op(*victim_backend);
        return (*op).head(key, Retry::once()).has_value();
    }

    /// A fake clock far past any real host's boot time, so a split between it and the real boot clock fails.
    static std::shared_ptr<FakeClock> farClock()
    {
        auto clock = std::make_shared<FakeClock>();
        clock->now = 1'000'000'000'000'000ULL;
        return clock;
    }

    /// A pool on real time with a 1 s lease renewed every 200 ms.
    static PoolConfig realTimeConfig()
    {
        return PoolConfig{
            .pool_prefix = "p",
            .server_root_id = "",
            .mount_lease_ttl_ms = std::chrono::milliseconds(1000),
            .mount_renew_period = std::chrono::milliseconds(200),
            .cas_request_budget = CasRequestBudget{
                .attempt_timeout_ms = 100, .lease_safety_margin_ms = 100, .connect_timeout_cap_ms = std::nullopt}};
    }

    void fenceSlot() const { fenceOutMountForTest(*victim_backend, layout.mountKey("victim")); }
    void tombstoneOwner() const { tombstoneOwnerForTest(*victim_backend, layout.ownerKey("victim")); }
};

TEST_F(CASDecommission, RefusesLiveMember)
{
    auto backend = std::make_shared<InMemoryBackend>();
    auto victim = openVictim(backend);   /// keeps its mount lease unexpired — the member is alive

    expectThrowsCode(ErrorCodes::ABORTED, [&]
    {
        Pool::openForDecommission(backend, PoolConfig{.pool_prefix = "p", .server_root_id = "admin"}, "victim");
    });
}

/// The refusal names the state a hard-killed member has to reach. `expired` in `system.cas_mounts` is
/// wall-clock arithmetic; the claim trusts only a terminal slot (`terminated` or `fenced`), and an
/// expired, unfenced lease takes exactly the branch an unexpired one takes here.
TEST_F(CASDecommission, NoWaitRefusalNamesTheTerminalStates)
{
    auto backend = std::make_shared<InMemoryBackend>();
    auto victim = openVictim(backend);

    try
    {
        Pool::openForDecommission(backend, PoolConfig{.pool_prefix = "p", .server_root_id = "admin"}, "victim");
        FAIL() << "expected the live member to be refused";
    }
    catch (const DB::Exception & e)
    {
        EXPECT_EQ(e.code(), ErrorCodes::ABORTED);
        EXPECT_NE(e.message().find("alive or contended"), String::npos) << e.message();
        EXPECT_NE(e.message().find("state = 'terminated'"), String::npos) << e.message();
        EXPECT_NE(e.message().find("state = 'fenced'"), String::npos) << e.message();
        EXPECT_NE(e.message().find("GC round"), String::npos) << e.message();
    }
}

TEST_F(CASDecommission, ClaimsDeadMemberAndBumpsEpoch)
{
    auto backend = std::make_shared<InMemoryBackend>();
    uint64_t victim_epoch = 0;
    {
        auto victim = openVictim(backend);
        victim_epoch = victim->writerEpoch();
    }   /// graceful close: lease stamped already-expired + farewell — the slot is claimable

    auto admin = Pool::openForDecommission(backend, PoolConfig{.pool_prefix = "p", .server_root_id = "admin"}, "victim");
    ASSERT_TRUE(admin != nullptr);
    EXPECT_GT(admin->writerEpoch(), victim_epoch);
    /// The admin store IS the victim server root now (impersonation).
    EXPECT_EQ(admin->poolConfig().server_root_id, "victim");
}

TEST_F(CASDecommission, AlwaysRenewsAdminClaimEvenWhenHostDiskIsObserveOnly)
{
    auto backend = std::make_shared<InMemoryBackend>();
    {
        auto victim = openVictim(backend);
    }   /// graceful close: lease stamped already-expired + farewell — the slot is claimable

    /// The calling (host) disk may be observe-only, i.e. its own PoolConfig carries
    /// background_watermark = false. The decommission admin claim must renew its lease
    /// regardless -- a long drain must not expire midway just because the host mount doesn't
    /// run a background renewer for its OWN mount.
    auto admin = Pool::openForDecommission(
        backend, PoolConfig{.pool_prefix = "p", .server_root_id = "admin", .background_watermark = false}, "victim");
    ASSERT_TRUE(admin != nullptr);
    EXPECT_TRUE(admin->poolConfig().background_watermark);
    EXPECT_TRUE(admin->poolConfig().bounded_background_renewal);
}

TEST_F(CASDecommission, OpensOverARetiredOwner)
{
    auto backend = startVictim();
    tombstoneOwner();
    auto admin = Pool::openForDecommission(backend, PoolConfig{.pool_prefix = "p", .server_root_id = "admin"}, "victim");
    ASSERT_TRUE(admin != nullptr);
    /// The claim only reads: the owner stays retired at the life it had, and the admin holds the slot.
    EXPECT_TRUE(owner().retired_at_ms.has_value());
    EXPECT_EQ(owner().owner_life, 2u);
    EXPECT_EQ(slot().writer_epoch, admin->liveWriterEpoch());
}

TEST_F(CASDecommission, RefusesUnknownMember)
{
    auto backend = std::make_shared<InMemoryBackend>();
    expectThrowsCode(ErrorCodes::BAD_ARGUMENTS, [&]
    {
        Pool::openForDecommission(backend, PoolConfig{.pool_prefix = "p", .server_root_id = "admin"}, "never_existed");
    });
}

/// `SYSTEM CAS DROP POOL MEMBER 'a/b'` is refused before it touches the member, even when `a/b` owns its
/// owner object: the drain selects objects by key prefix, so it is defined only for ids without `/`.
TEST_F(CASDecommission, RefusesIdWithSlash)
{
    auto backend = std::make_shared<InMemoryBackend>();
    const Layout layout("p");
    DB::Cas::tests::seedPoolMetaForRestart(*backend);
    OperationForTest raw_op(*backend);
    ASSERT_TRUE(std::holds_alternative<Committed>((*raw_op).create(layout.ownerKey("shard-01/replica-a"),
        encodeOwner(OwnerObject{.server_uuid = UInt128(1), .retired_at_ms = std::nullopt}), Retry::once())));
    const auto owner_before = (*raw_op).read(layout.ownerKey("shard-01/replica-a"), Retry::once());
    ASSERT_TRUE(owner_before.has_value());

    DB::Cas::tests::expectThrowsCodeWithMessage(ErrorCodes::BAD_ARGUMENTS, "contains '/'", [&]
    {
        decommissionPoolMember(backend, PoolConfig{.pool_prefix = "p", .server_root_id = "admin"}, "shard-01/replica-a");
    });

    const auto owner_after = (*raw_op).read(layout.ownerKey("shard-01/replica-a"), Retry::once());
    ASSERT_TRUE(owner_after.has_value());
    EXPECT_EQ(owner_after->etag, owner_before->etag);
    EXPECT_FALSE((*raw_op).head(layout.mountKey("shard-01/replica-a"), Retry::once()).has_value());
    EXPECT_FALSE((*raw_op).head(layout.epochKey("shard-01/replica-a"), Retry::once()).has_value());
}

TEST_F(CASDecommission, SecondConcurrentDecommissionRefused)
{
    auto backend = startVictim();

    auto first = Pool::openForDecommission(backend, PoolConfig{.pool_prefix = "p", .server_root_id = "admin"}, "victim");
    expectThrowsCode(ErrorCodes::ABORTED, [&]
    {
        Pool::openForDecommission(backend, PoolConfig{.pool_prefix = "p", .server_root_id = "admin2"}, "victim");
    });
}

TEST_F(CASDecommission, DuplicateLifeIdRefusesBeforeAnyNamespaceOrSlotMutation)
{
    auto backend = startVictim();
    RefCatalog catalog;
    catalog.entries = {
        CatalogEntry{.ns = RootNamespace{"victim/a"}, .state = NsState::Live, .incarnation = UInt128{77}},
        CatalogEntry{
            .ns = RootNamespace{"victim/b"},
            .state = NsState::Removing,
            .incarnation = UInt128{77},
            .removal_started_round = 1},
    };
    OperationForTest raw_op(*backend);
    const auto empty_catalog = (*raw_op).read(layout.refCatalogKey(), Retry::once());
    ASSERT_TRUE(empty_catalog);
    ASSERT_TRUE(std::holds_alternative<Committed>((*raw_op).replace(
        layout.refCatalogKey(), encodeRefCatalog(catalog), empty_catalog->etag, Retry::once())));
    const auto before = controlObjects();

    EXPECT_THROW(run("admin"), DB::Exception);
    EXPECT_EQ(controlObjects(), before);
}

TEST_F(CASDecommission, CatalogCutIsValidatedBeforeImpersonationAndReusedForSelection)
{
    auto backend = std::make_shared<CatalogChangesAfterFirstReadBackend>();
    victim_backend = backend;
    { auto victim = Pool::open(backend, PoolConfig{.pool_prefix = "p", .server_root_id = "victim"}); }

    RefCatalog ambiguous;
    ambiguous.entries = {
        CatalogEntry{.ns = RootNamespace{"other/a"}, .state = NsState::Live, .incarnation = UInt128{77}},
        CatalogEntry{
            .ns = RootNamespace{"other/b"},
            .state = NsState::Removing,
            .incarnation = UInt128{77},
            .removal_started_round = 1},
    };

    const auto before = controlObjects();
    backend->armCatalogReplacement(layout.refCatalogKey(), std::move(ambiguous));

    EXPECT_THROW(run("admin"), DB::Exception);
    ASSERT_TRUE(backend->fired());

    EXPECT_EQ(controlObjects(), before);
}

TEST_F(CASDecommission, NamespaceSelectionUsesThePreImpersonationCut)
{
    auto backend = std::make_shared<CatalogChangesAfterFirstReadBackend>();
    victim_backend = backend;
    { auto victim = Pool::open(backend, PoolConfig{.pool_prefix = "p", .server_root_id = "victim"}); }

    RefCatalog later;
    later.entries = {
        CatalogEntry{.ns = RootNamespace{"victim/late"}, .state = NsState::Live, .incarnation = UInt128{88}},
    };
    backend->armCatalogReplacement(layout.refCatalogKey(), std::move(later));

    const DecommissionReport report = run("admin");
    ASSERT_TRUE(backend->fired());
    EXPECT_EQ(report.namespaces_removed, 0u)
        << "a namespace visible only to mount safety's later observation is outside the validated cut";
    EXPECT_EQ(report.namespaces_already_removed, 0u);
}

TEST_F(CASDecommission, SameNameRebirthAfterTheCutIsRefusedWithoutTouchingTheNewLife)
{
    auto backend = std::make_shared<CatalogChangesAfterFirstReadBackend>();
    victim_backend = backend;
    { auto victim = Pool::open(backend, PoolConfig{.pool_prefix = "p", .server_root_id = "victim"}); }
    const RootNamespace ns{"victim/same"};
    const NamespaceLifeId old_life = NamespaceLifeId::fromCatalogEntry(ns, UInt128{70});
    const NamespaceLifeId new_life = NamespaceLifeId::fromCatalogEntry(ns, UInt128{71});

    RefCatalog old_catalog;
    old_catalog.entries = {
        CatalogEntry{.ns = ns, .state = NsState::Live, .incarnation = old_life.incarnation},
    };
    OperationForTest raw_op(*backend);
    const auto empty_catalog = (*raw_op).read(layout.refCatalogKey(), Retry::once());
    ASSERT_TRUE(empty_catalog);
    ASSERT_TRUE(std::holds_alternative<Committed>((*raw_op).replace(
        layout.refCatalogKey(), encodeRefCatalog(old_catalog), empty_catalog->etag, Retry::once())));

    RefLogTxn new_birth;
    new_birth.ns = ns.string();
    new_birth.txn_id = RefTxnId{1, 1};
    new_birth.ops = {namespaceBirthOp()};
    ASSERT_TRUE(std::holds_alternative<Committed>((*raw_op).create(
        layout.refLogKey(new_life, new_birth.txn_id),
        sealObject(FormatId::RefLog, encodeRefLogTxn(new_birth)), Retry::once())));
    RefLogTxn new_seal;
    new_seal.ns = ns.string();
    new_seal.txn_id = RefTxnId{1, 2};
    new_seal.ops = {epochSealOp()};
    ASSERT_TRUE(std::holds_alternative<Committed>((*raw_op).create(
        layout.refLogKey(new_life, new_seal.txn_id),
        sealObject(FormatId::RefLog, encodeRefLogTxn(new_seal)), Retry::once())));
    const auto new_life_before = snapshotPrefixObjects(*backend, layout.namespaceStreamPrefix(new_life));

    RefCatalog replacement;
    replacement.entries = {
        CatalogEntry{.ns = ns, .state = NsState::Live, .incarnation = new_life.incarnation},
    };
    /// Read 1 captures the immutable selection cut. Read 2 is mount safety; replace immediately
    /// after returning that old observation, so the name-only call is the first consumer of the
    /// same-name new incarnation.
    backend->armCatalogReplacement(
        layout.refCatalogKey(), std::move(replacement), /*completed_reads_before_replacement=*/1);

    String refusal;
    try
    {
        (void)run("admin");
    }
    catch (const DB::Exception & e)
    {
        refusal = e.message();
    }
    EXPECT_NE(refusal.find("changed incarnation after the validated catalog cut"), String::npos)
        << refusal;
    ASSERT_TRUE(backend->fired());
    EXPECT_EQ(snapshotPrefixObjects(*backend, layout.namespaceStreamPrefix(new_life)), new_life_before)
        << "decommission must not append a removal transaction to the post-cut incarnation";
}

TEST_F(CASDecommission, VictimNameMatchesOneCanonicalPathComponent)
{
    auto backend = startVictim();

    const RootNamespace neighbor_ns{"victim2/db/t1"};
    {
        auto neighbor = Pool::open(backend, PoolConfig{.pool_prefix = "p", .server_root_id = "victim2"});
        makeTableWithRefs(*neighbor, neighbor_ns.string(), /*committed=*/1, /*precommits=*/0);
    }

    const auto report = run("admin");
    EXPECT_EQ(report.namespaces_removed, 0u);

    auto neighbor = Pool::open(backend, PoolConfig{.pool_prefix = "p", .server_root_id = "victim2"});
    EXPECT_EQ(neighbor->listRefs(neighbor_ns).size(), 1u)
        << "decommissioning victim must not select victim2 by raw string prefix";
}

TEST_F(CASDecommission, ErasesAllVictimNamespaces)
{
    auto backend = std::make_shared<InMemoryBackend>();
    victim_backend = backend;
    {
        auto victim = openVictim(backend);
        /// Two tables: ns "victim/db/t1" with 2 committed refs, ns "victim/db/t2" with 1 committed
        /// ref + 1 stale precommit (fixture idiom of gtest_cas_ref_writer.cpp).
        makeTableWithRefs(*victim, "victim/db/t1", /*committed=*/2, /*precommits=*/0);
        makeTableWithRefs(*victim, "victim/db/t2", /*committed=*/1, /*precommits=*/1);
    }

    const auto report = run("admin");

    EXPECT_EQ(report.srid, "victim");
    EXPECT_EQ(report.namespaces_removed, 2u);
    EXPECT_EQ(report.namespaces_already_removed, 0u);
    EXPECT_EQ(report.committed_refs_removed, 3u);
    EXPECT_EQ(report.precommits_removed, 1u);
    EXPECT_EQ(report.edge_deltas_emitted, 4u);

    /// Terminal publication is writer work; exact catalog-row deletion remains GC work, reported as
    /// pending rather than as a failure.
    EXPECT_TRUE(report.warnings.empty());
    EXPECT_TRUE(report.member_retired);
    EXPECT_EQ(report.namespaces_pending_gc, 2u);
    drainCompletedNamespaceRemovals(backend);
    const auto rerun = run("admin2");
    EXPECT_TRUE(rerun.warnings.empty());
    EXPECT_TRUE(rerun.member_retired);
    EXPECT_EQ(rerun.namespaces_pending_gc, 0u);
    EXPECT_EQ(rerun.namespaces_already_removed, 0u);
}

/// A real precommit carries the victim's own epoch, below the admin's. The writer's stale-precommit sweep
/// must not reclaim it in its own transaction before the namespace drop counts it.
TEST_F(CASDecommission, CountsRealisticEpochPrecommit)
{
    startVictim(std::make_shared<InMemoryBackend>(), [](Pool & victim)
    {
        oneTable(victim);
        const RootNamespace ns("victim/db/t1");
        /// `build_sequence = 2`: a real build's `ManifestRef` is unique; the table's ref uses 1.
        const ManifestRef ref{.writer_epoch = victim.writerEpoch(), .build_sequence = 2, .manifest_ordinal = 1};
        writeManifestRaw(*victim.poolBackendPtr(), victim.layout(), ns, ref, {});
        addPrecommitTransition(*victim.poolBackendPtr(), victim.layout(), ns, UInt128(1), "precommit_0", std::nullopt, ref);
    });

    const auto report = run("admin");

    EXPECT_EQ(report.namespaces_removed, 1u);
    EXPECT_EQ(report.committed_refs_removed, 1u);
    EXPECT_EQ(report.precommits_removed, 1u);
    EXPECT_EQ(report.edge_deltas_emitted, 2u);
}

TEST_F(CASDecommission, EmitsMemberDecommissionEvents)
{
    auto backend = startVictim(std::make_shared<InMemoryBackend>(), oneTable);

    std::vector<CasEvent> seen;
    (void)decommissionPoolMember(backend, PoolConfig{.pool_prefix = "p", .server_root_id = "admin"}, "victim",
        [&](const CasEvent & e) { seen.push_back(e); });

    std::vector<CasEvent> member_events;
    for (const auto & e : seen)
        if (e.type == CasEventType::MemberDecommission)
            member_events.push_back(e);

    ASSERT_EQ(member_events.size(), 3u);
    EXPECT_EQ(member_events[0].outcome, "begin");
    EXPECT_EQ(member_events[1].outcome, "namespace_removed");
    EXPECT_EQ(member_events[1].detail.at("namespace"), "victim/db/t1");
    EXPECT_EQ(member_events[1].detail.at("committed"), "1");
    EXPECT_EQ(member_events[1].detail.at("precommits"), "0");
    EXPECT_EQ(member_events[2].outcome, "end");
    EXPECT_EQ(member_events[2].detail.at("namespaces_removed"), "1");
    EXPECT_EQ(member_events[2].detail.at("member_retired"), "1");
    EXPECT_EQ(member_events[2].detail.at("owner_life"), "2");
}

/// The staging and mountpoint drains empty their prefixes even when the manifest-debris drain retains.
TEST_F(CASDecommission, DrainsDebrisStagingAndRoots)
{
    auto backend = startVictim(std::make_shared<InMemoryBackend>(), [](Pool & victim) { oneTableWithDebris(victim); });
    seed("p/staging/victim/upload1.tmp");
    seed("p/staging/victim/upload2.tmp");
    seed("p/roots/victim/clickhouse_access_check_abc");

    const auto report = run("admin");

    /// The manifest-debris retention is `RetainsDebrisWhoseEpochSealIsUnconsumed`'s subject.
    EXPECT_EQ(report.manifest_debris_removed, 0u);
    EXPECT_EQ(report.staging_objects_removed, 2u);
    EXPECT_EQ(report.mountpoint_objects_removed, 1u);
    EXPECT_FALSE(report.warnings.empty());
    OperationForTest raw_op(*backend);
    EXPECT_TRUE((*raw_op).list("p/staging/victim/", "", 10, Retry::once()).keys.empty());
    EXPECT_TRUE((*raw_op).list("p/roots/victim/", "", 10, Retry::once()).keys.empty());
}

/// With no sealed fold cursor past the victim's epoch the drain cannot show the debris is unreferenced: it
/// retains it, names it in a warning, and leaves the unverified tombstone for a rerun after GC catches up.
TEST_F(CASDecommission, RetainsDebrisWhoseEpochSealIsUnconsumed)
{
    String debris_key;
    startVictim(std::make_shared<InMemoryBackend>(), [&](Pool & victim) { debris_key = oneTableWithDebris(victim); });

    const auto report = run("admin");

    EXPECT_EQ(report.manifest_debris_removed, 0u);
    EXPECT_TRUE(exists(debris_key));
    ASSERT_EQ(report.warnings.size(), 1u) << fmt::format("{}", fmt::join(report.warnings, "; "));
    EXPECT_NE(report.warnings.front().find(debris_key), String::npos) << report.warnings.front();
    EXPECT_FALSE(report.member_retired);
    expectOwner(2, true, false);
}

/// A per-object failure in the staging or roots drain, a throw or a `Mismatch`, is a warning and the rest of
/// the drain proceeds.
TEST_F(CASDecommission, PerObjectFailureWarnsAndContinuesDrain)
{
    auto backend = startVictim(std::make_shared<FailingDeleteBackend>(), oneTable);
    seed("p/staging/victim/upload_ok.tmp");
    seed("p/staging/victim/upload_throws.tmp");
    seed("p/roots/victim/clickhouse_access_check_abc");
    backend->failWithThrow("p/staging/victim/upload_throws.tmp");
    backend->latch();
    backend->failWithTokenMismatch("p/roots/victim/clickhouse_access_check_abc");

    /// The latched fault reaches the engine's give-up on the fake clock, with no real time passing.
    auto clock = std::make_shared<FakeClock>();
    const auto report = runOnClock("admin", clock);

    EXPECT_EQ(report.staging_objects_removed, 1u);
    EXPECT_EQ(report.mountpoint_objects_removed, 0u);
    EXPECT_EQ(report.warnings.size(), 2u);
    EXPECT_FALSE(exists("p/staging/victim/upload_ok.tmp"));
    EXPECT_TRUE(exists("p/staging/victim/upload_throws.tmp"));
    EXPECT_TRUE(exists("p/roots/victim/clickhouse_access_check_abc"));
}

/// Opaque physical debris carries no logical owner, so it cannot widen or redirect the catalog-derived victim
/// set; the namespace janitor owns it.
TEST_F(CASDecommission, LifelessPhysicalKeyCannotRedirectCatalogOwnedDecommission)
{
    startVictim(std::make_shared<InMemoryBackend>(), oneTable);
    /// Hand-built: no helper can mint the un-incarnated shape any more.
    const String lifeless = layout.casRefsPrefix() + String("victim/db/t1/_log/") + renderRefTxnId(RefTxnId{1, 1}) + ".zst";
    seed(lifeless);

    const auto report = run("admin");
    EXPECT_EQ(report.namespaces_removed, 1u);
    EXPECT_TRUE(exists(lifeless));
}

/// A per-key failure in the manifest-debris drain is a warning; the namespace drop and the staging drain go on.
TEST_F(CASDecommission, ManifestDebrisDeleteFailureWarnsAndContinues)
{
    String debris_key;
    auto backend = startVictim(std::make_shared<FailingDeleteBackend>(), [&](Pool & victim) { debris_key = oneTableWithDebris(victim); });
    backend->failWithThrow(debris_key);
    backend->latch();
    seed("p/staging/victim/upload_ok.tmp");

    /// The latched fault reaches the engine's give-up on the fake clock, with no real time passing.
    auto clock = std::make_shared<FakeClock>();
    const auto report = runOnClock("admin", clock);

    EXPECT_EQ(report.namespaces_removed, 1u);
    EXPECT_EQ(report.manifest_debris_removed, 0u);
    EXPECT_EQ(report.warnings.size(), 1u);
    EXPECT_EQ(report.staging_objects_removed, 1u);
    EXPECT_TRUE(exists(debris_key));
}

/// A rerun over a verified retirement writes a fresh tombstone and mark of its own each time; the control
/// objects stay.
TEST_F(CASDecommission, RerunOverAVerifiedRetirementRetiresAgain)
{
    auto backend = startVictim(std::make_shared<InMemoryBackend>(), oneTable);
    const auto first = run("admin");
    EXPECT_TRUE(first.member_retired);
    EXPECT_EQ(first.namespaces_pending_gc, 1u);
    drainCompletedNamespaceRemovals(backend);

    const auto second = run("admin2");
    EXPECT_TRUE(second.member_retired);
    EXPECT_TRUE(second.warnings.empty());
    EXPECT_EQ(second.namespaces_pending_gc, 0u);
    EXPECT_TRUE(exists(layout.mountKey("victim")));
    EXPECT_TRUE(exists(layout.epochKey("victim")));
    expectOwner(5, true, true);

    const auto third = run("a2");
    EXPECT_TRUE(third.member_retired);
    EXPECT_TRUE(third.warnings.empty());
    expectOwner(7, true, true);
}

/// A resolve read settles an ambiguous tombstone write: the bytes already match, so the tombstone is committed.
TEST_F(CASDecommission, OwnerTombstoneAmbiguousSuccessResolvesToCommitted)
{
    auto backend = startVictim();
    backend->injectAmbiguousLandedWrite(layout.ownerKey("victim"));

    const auto report = run("admin");

    EXPECT_TRUE(report.member_retired);
    EXPECT_TRUE(report.warnings.empty());
    expectOwner(3, true, true);
}

/// Delegates every op to `inner`, except `remove`: while armed, a key under `fail_prefix` throws a transient
/// failure the engine reissues until it gives up. `disarm()` clears it.
class FailDeletesUnderPrefixBackend : public Backend
{
public:
    FailDeletesUnderPrefixBackend(std::shared_ptr<InMemoryBackend> inner_, String fail_prefix_)
        : inner(std::move(inner_)), fail_prefix(std::move(fail_prefix_))
    {
    }

    void disarm() { armed = false; }

    bool supportsListTokens() const override { return inner->supportsListTokens(); }

    std::optional<Raw> read(const String & key, TransportAccess & access) override { return inner->read(key, access); }
    std::optional<RawMeta> head(const String & key, TransportAccess & access) override { return inner->head(key, access); }
    RawListPage list(const String & prefix, const String & cursor, size_t limit, TransportAccess & access) override { return inner->list(prefix, cursor, limit, access); }
    RawRemoval remove(const String & key, const String & expected_value, TransportAccess & access) override
    {
        if (armed && key.starts_with(fail_prefix))
            throw Poco::TimeoutException("injected transient delete failure for " + key);
        return inner->remove(key, expected_value, access);
    }
    void removeManyWriteOnce(const std::vector<WriteOnceKey> & keys, TransportAccess & access) override { inner->removeManyWriteOnce(keys, access); }
    size_t bulkDeleteKeyLimit() const override { return inner->bulkDeleteKeyLimit(); }
    std::expected<String, RawConflict> write(const String & key, const String & bytes,
                                             const std::optional<String> & expected_value, TransportAccess & access) override
    {
        return inner->write(key, bytes, expected_value, access);
    }
    std::unique_ptr<DB::ReadBuffer> stream(const String & key, TransportAccess & access) override { return inner->stream(key, access); }
    void publish(const BlobPublishRequest & request, TransportAccess & access) override { inner->publish(request, access); }
    DB::Cas::Dialect dialect() const override { return inner->dialect(); }

private:
    std::shared_ptr<InMemoryBackend> inner;
    String fail_prefix;
    bool armed = true;
};

/// A drain failure under the roots prefix leaves this run's unverified tombstone. Once the fault is
/// cleared, a rerun resumes over it: the already-erased namespace is counted as
/// `namespaces_already_removed`, the leftover roots object is swept and the retirement is verified.
TEST_F(CASDecommission, FailedDrainLeavesAnUnverifiedTombstoneThenResumes)
{
    auto inner = startVictim(std::make_shared<InMemoryBackend>(), oneTable);
    seed("p/roots/victim/loose_file");
    auto failing = std::make_shared<FailDeletesUnderPrefixBackend>(inner, "p/roots/victim/");
    victim_backend = failing;

    /// The admin's boot clock is the run's clock, so its lease must outlast the retries' fast-forward.
    auto clock = std::make_shared<FakeClock>();
    const auto first = runOnClock(
        "a1", clock, PoolConfig{.pool_prefix = "p", .server_root_id = "", .mount_lease_ttl_ms = std::chrono::milliseconds(300'000)});
    EXPECT_FALSE(first.warnings.empty());
    EXPECT_FALSE(first.member_retired);
    expectOwner(2, true, false);
    EXPECT_TRUE(exists(layout.mountKey("victim")));

    failing->disarm();
    const auto second = run("a2");
    EXPECT_TRUE(second.warnings.empty());
    EXPECT_TRUE(second.member_retired);
    EXPECT_EQ(second.mountpoint_objects_removed, 1u);
    EXPECT_EQ(second.namespaces_already_removed, 1u);
    EXPECT_EQ(second.namespaces_pending_gc, 1u);

    drainCompletedNamespaceRemovals(inner);
    const auto third = run("a3");
    EXPECT_TRUE(third.warnings.empty());
    EXPECT_TRUE(third.member_retired);
    EXPECT_EQ(third.namespaces_pending_gc, 0u);
}

/// A per-key throw inside the manifest-debris drain leaves the unverified tombstone too. The debris stays
/// retained once the fault is cleared (no sealed fold cursor), so the rerun is not verified either.
TEST_F(CASDecommission, ManifestDebrisFailureLeavesAnUnverifiedTombstone)
{
    String debris_key;
    auto backend = startVictim(std::make_shared<FailingDeleteBackend>(), [&](Pool & victim) { debris_key = oneTableWithDebris(victim); });
    backend->failWithThrow(debris_key);
    backend->latch();

    auto clock = std::make_shared<FakeClock>();
    const auto first = runOnClock("a1", clock);
    EXPECT_FALSE(first.warnings.empty());
    EXPECT_FALSE(first.member_retired);
    EXPECT_EQ(first.manifest_debris_removed, 0u);
    expectOwner(2, true, false);
    EXPECT_TRUE(exists(debris_key));

    backend->disarm();
    const auto second = run("a2");
    EXPECT_EQ(second.namespaces_already_removed, 1u);
    EXPECT_EQ(second.manifest_debris_removed, 0u);
    EXPECT_FALSE(second.member_retired);
    expectOwner(3, true, false);
    EXPECT_TRUE(exists(debris_key));
    EXPECT_TRUE(exists(layout.mountKey("victim")));
}

/// The farewell deadline is bound to the admin's boot clock, which the run's fake clock must drive too. A fake
/// clock far beyond any real host's boot time makes a split between the two clocks fail on every machine.
TEST_F(CASDecommission, DrainClockUnifiesWithTheFarewellBootClockRegardlessOfHostUptime)
{
    startVictim();
    auto clock = farClock();
    const auto report = runOnClock("a1", clock);

    EXPECT_TRUE(report.warnings.empty());
    EXPECT_TRUE(report.member_retired);
    EXPECT_TRUE(slot().isFarewell());
}

/// The planes `Pool::openForDecommission` builds retry during opening, before the run installs its clock on
/// the pool; they must pace on the fake sleep from construction, or a retry deadline on the frozen clock
/// never elapses.
TEST_F(CASDecommission, OpeningRetriesPaceOnTheSameFakeClockAndSleepAsTheDrain)
{
    auto backend = startVictim(std::make_shared<FlakyReadBackend>());
    backend->failReadNTimes(layout.ownerKey("victim"), /*times=*/5);

    auto clock = farClock();

    const auto started = std::chrono::steady_clock::now();
    const auto report = runOnClock("a1", clock);
    const auto wall_elapsed = std::chrono::steady_clock::now() - started;

    EXPECT_TRUE(report.warnings.empty());
    EXPECT_TRUE(report.member_retired);
    EXPECT_TRUE(slot().isFarewell());
    EXPECT_FALSE(clock->sleeps.empty());
    EXPECT_LT(wall_elapsed, std::chrono::seconds(5));
}

/// The victim restarts during its own decommission: its owner check passes before the tombstone, its mount
/// claim waits on the admin's live lease, the admin tombstones and farewells during that wait, the victim
/// reclaims the farewell and its recheck sees the tombstone.
TEST_F(CASDecommission, VictimClaimingAfterTheTombstoneFencesItselfAndFails)
{
    auto backend = startVictim();
    const UInt128 victim_uuid = owner().server_uuid;
    auto admin = Pool::openForDecommission(backend, PoolConfig{.pool_prefix = "p", .server_root_id = "admin"}, "victim");
    const uint64_t admin_epoch = admin->writerEpoch();

    auto fake_boot = std::make_shared<std::atomic<uint64_t>>(0);
    bool decommissioned = false;
    PoolConfig victim_config{
        .pool_prefix = "p", .server_root_id = "victim",
        .boot_ms_fn = [fake_boot] { return fake_boot->load(); },
        .wait_sleep_fn = [&, fake_boot](uint64_t ms)
        {
            *fake_boot += ms;
            if (std::exchange(decommissioned, true))
                return;
            tombstoneOwner();
            admin.reset();   /// the clean farewell the victim's next poll reclaims
        }};
    expectThrowsCodeWithMessage(ErrorCodes::CORRUPTED_DATA, "decommissioned", [&] { Pool::open(backend, victim_config); });
    ASSERT_TRUE(decommissioned);
    const MountLease body = slot();
    EXPECT_EQ(body.server_uuid, victim_uuid);
    EXPECT_GT(body.writer_epoch, admin_epoch);
    EXPECT_TRUE(body.gc_fenced);
    EXPECT_TRUE(owner().retired_at_ms.has_value());
}


namespace
{
/// While armed, `_pool_meta` cannot be probed: an admin self-remount stops at its identity probe and retries
/// on a backoff the teardown interrupts.
class PoolMetaUnreadableBackend : public InMemoryBackend
{
public:
    std::atomic<bool> armed = false;

    std::optional<RawMeta> head(const String & key, TransportAccess & access) override
    {
        if (armed && key == Layout("p").poolMetaKey())
            throw Poco::TimeoutException("pool meta fixture: unreadable");
        return InMemoryBackend::head(key, access);
    }
};
}

/// The tombstone's first attempt goes unanswered and the admin's lease deadline passes before the retry.
/// The retry is refused locally, so a victim restarted after the deadline runs under the life it reads.
TEST_F(CASDecommission, TombstoneRetryAfterTheLeaseDeadlineIsNotSent)
{
    auto backend = startVictim(std::make_shared<PoolMetaUnreadableBackend>());
    auto clock = farClock();
    bool stalled = false;
    backend->onBeforeWrite(layout.ownerKey("victim"), [&]
    {
        if (std::exchange(stalled, true))
            return;
        /// The lapsed lease sends the admin into a self-remount; keep it from racing the teardown.
        backend->armed = true;
        clock->now += 31'000;   /// past the default 30 s TTL
        backend->injectAmbiguousWrite(layout.ownerKey("victim"));
    });

    EXPECT_THROW(runOnClock("a1", clock), DB::Exception);

    ASSERT_TRUE(stalled);
    backend->armed = false;
    expectOwner(1, false, false);
    auto restarted = Pool::open(backend, PoolConfig{.pool_prefix = "p", .server_root_id = "victim", .unsafe_remount_no_delay = true});
    EXPECT_TRUE(restarted->mayMutate());
    expectOwner(1, false, false);
}

/// A local fault after the tombstone went out leaves its fate unknown, and the read-back finds the old owner:
/// the store may still apply the tombstone. The admin leaves its live body instead of a clean farewell, so a
/// restarted victim waits for a fence before it runs.
TEST_F(CASDecommission, UnresolvedTombstoneLeavesNoFarewell)
{
    auto backend = startVictim();
    const UInt128 victim_uuid = owner().server_uuid;
    bool sent = false;
    backend->onBeforeWrite(layout.ownerKey("victim"), [&]
    {
        if (!std::exchange(sent, true))
            throw std::runtime_error("fixture: a local fault after the tombstone was sent");
    });

    EXPECT_THROW(runOnClock("a1", farClock()), std::runtime_error);

    ASSERT_TRUE(sent);
    expectOwner(1, false, false);
    const MountLease left = slot();
    EXPECT_EQ(left.server_uuid, victim_uuid);
    EXPECT_FALSE(left.isFarewell());
    EXPECT_FALSE(left.gc_fenced);

    auto fake_boot = std::make_shared<std::atomic<uint64_t>>(0);
    bool fenced = false;
    PoolConfig victim_config{
        .pool_prefix = "p", .server_root_id = "victim",
        .boot_ms_fn = [fake_boot] { return fake_boot->load(); },
        .wait_sleep_fn = [&, fake_boot](uint64_t ms)
        {
            *fake_boot += ms;
            if (!std::exchange(fenced, true))
                fenceSlot();
        }};
    auto restarted = Pool::open(backend, victim_config);
    EXPECT_TRUE(fenced) << "the victim reclaimed the slot without waiting";
    EXPECT_TRUE(restarted->mayMutate());
    EXPECT_GT(restarted->writerEpoch(), left.writer_epoch);
}

/// The owner is rewritten between the claim and the tombstone: the tombstone's precondition fails, the run
/// aborts, the rewrite stays and nothing is drained.
TEST_F(CASDecommission, OwnerRewrittenBeforeTheTombstoneAbortsTheRun)
{
    auto backend = startVictim();
    seed("p/roots/victim/loose_file");
    String rewritten;
    backend->onBeforeWrite(layout.ownerKey("victim"), [&]
    {
        if (!rewritten.empty())
            return;
        OperationForTest op(*backend);
        const auto current = (*op).read(layout.ownerKey("victim"), Retry::once());
        OwnerObject owner = decodeOwner(current->bytes);
        owner.owner_life += 1;
        rewritten = encodeOwner(owner);
        ASSERT_TRUE(std::holds_alternative<Committed>(
            (*op).replace(layout.ownerKey("victim"), rewritten, current->etag, Retry::once())));
    });

    expectThrowsCode(ErrorCodes::ABORTED, [&] { run("a1"); });

    ASSERT_FALSE(rewritten.empty());
    expectBytes(*backend, layout.ownerKey("victim"), rewritten);
    EXPECT_TRUE(exists("p/roots/victim/loose_file"));
}

/// A member last written by 26.6.4: owner without a life, counter without a nonce, its clean farewell. A
/// decommission that loses its claim rolls back to life 2, and the member restarts under it.
TEST_F(CASDecommission, A2664MemberRollsBackAndRestarts)
{
    auto backend = startVictim();
    const String hex = u128ToHex(owner().server_uuid);
    {
        OperationForTest op(*backend);
        const auto put = [&](const String & key, const String & bytes)
        {
            const auto current = (*op).read(key, Retry::once());
            ASSERT_TRUE(current.has_value());
            ASSERT_TRUE(std::holds_alternative<Committed>((*op).replace(key, bytes, current->etag, Retry::once())));
        };
        put(layout.ownerKey("victim"), "{\"type\":\"cas_owner\",\"v\":1}\n{\"server_uuid\":\"" + hex + "\"}\n");
        put(layout.epochKey("victim"), "{\"type\":\"cas_epoch\",\"v\":1}\n{\"next_writer_epoch\":\"5\"}\n");
        put(layout.mountKey("victim"), "{\"type\":\"cas_mount_lease\",\"v\":1}\n{\"server_uuid\":\"" + hex
            + "\",\"writer_epoch\":\"4\",\"hostname\":\"h\",\"pid\":1,\"started_at_ms\":1,\"seq\":\"3\",\"expires_at_ms\":1,"
              "\"min_active_build_sequence\":\"18446744073709551615\",\"gc_fenced\":false,"
              "\"write_attempt_id\":\"0000000000000000000000000000000a\"}\n");
    }
    bool fenced = false;
    backend->onWriteCommitted(layout.ownerKey("victim"), [&]
    {
        if (!std::exchange(fenced, true))
            fenceSlot();
    });

    const auto report = run("a1");

    ASSERT_TRUE(fenced);
    EXPECT_FALSE(report.member_retired);
    expectOwner(2, false, false);   /// 26.6.4 life 0, tombstone 1, rollback 2
    auto restarted = Pool::open(backend, PoolConfig{.pool_prefix = "p", .server_root_id = "victim", .unsafe_remount_no_delay = true});
    EXPECT_GT(restarted->writerEpoch(), 5u);
    expectOwner(2, false, false);
}

TEST_F(CASDecommission, KeepsEpochAndFarewellMount)
{
    uint64_t victim_epoch = 0;
    auto backend = startVictim(std::make_shared<InMemoryBackend>(), [&](Pool & victim) { victim_epoch = victim.writerEpoch(); });
    const UInt128 victim_uuid = owner().server_uuid;

    const auto report = run("admin");

    EXPECT_TRUE(report.warnings.empty());
    EXPECT_TRUE(report.member_retired);
    const MountLease body = slot();
    EXPECT_EQ(body.writer_epoch, victim_epoch + 1) << "the admin's own clean farewell stays in the slot";
    EXPECT_TRUE(body.isFarewell());
    EXPECT_FALSE(body.gc_fenced);
    OperationForTest raw_op(*backend);
    const auto epoch = (*raw_op).read(layout.epochKey("victim"), Retry::once());
    ASSERT_TRUE(epoch.has_value());
    EXPECT_EQ(decodeServerEpoch(epoch->bytes).next_writer_epoch, victim_epoch + 2) << "the counter is never deleted";
    expectOwner(3, true, true);   /// claim 1, tombstone 2, verified mark 3
    EXPECT_EQ(owner().server_uuid, victim_uuid);
}

/// The admin is GC-fenced between its claim and its tombstone: the confirm sees the fence and the
/// tombstone is rolled back to a fresh, unretired life.
TEST_F(CASDecommission, FenceBeforeTombstoneUntombstonesAndFails)
{
    auto backend = startVictim();
    seed("p/roots/victim/loose_file");
    bool fired = false;
    backend->onBeforeWrite(layout.ownerKey("victim"), [&]
    {
        if (std::exchange(fired, true))
            return;
        fenceSlot();
    });

    const auto report = run("admin");

    ASSERT_TRUE(fired);
    EXPECT_FALSE(report.member_retired);
    EXPECT_EQ(report.warnings.size(), 1u) << fmt::format("{}", fmt::join(report.warnings, "; "));
    expectOwner(3, false, false);   /// claim 1, tombstone 2, rollback 3
    EXPECT_TRUE(exists("p/roots/victim/loose_file")) << "nothing was drained";
}

/// A renewal between the claim and the confirm changes the ETag and the attempt id, and the confirm must not
/// care; a fence, a farewell, another epoch or another uuid is a loss.
TEST_F(CASDecommission, ConfirmAcceptsARenewalBetweenClaimAndConfirm)
{
    auto backend = std::make_shared<InMemoryBackend>();
    OperationForTest op(*backend);
    MountLease body{.server_uuid = UInt128(7), .writer_epoch = 5, .hostname = "h", .pid = 1,
                    .started_at_ms = 1, .seq = 1, .expires_at_ms = 2, .min_active_build_sequence = 0,
                    .gc_fenced = false, .write_attempt_id = UInt128(11)};
    ASSERT_TRUE(std::holds_alternative<Committed>((*op).create(layout.mountKey("v"), encodeMountLease(body), Retry::once())));
    EXPECT_TRUE(adminClaimStillHeld(*op, layout, "v", UInt128(7), 5));

    MountLease renewed = body;
    renewed.seq = 2;
    renewed.write_attempt_id = UInt128(12);
    ASSERT_TRUE(std::holds_alternative<Committed>((*op).replace(layout.mountKey("v"), encodeMountLease(renewed),
        (*op).read(layout.mountKey("v"), Retry::once())->etag, Retry::once())));
    EXPECT_TRUE(adminClaimStillHeld(*op, layout, "v", UInt128(7), 5));

    EXPECT_FALSE(adminClaimStillHeld(*op, layout, "v", UInt128(7), 6)) << "another epoch";
    EXPECT_FALSE(adminClaimStillHeld(*op, layout, "v", UInt128(8), 5)) << "another uuid";
    ASSERT_TRUE(std::holds_alternative<Committed>((*op).replace(layout.mountKey("v"), encodeMountLease(renewed.fencedCopy()),
        (*op).read(layout.mountKey("v"), Retry::once())->etag, Retry::once())));
    EXPECT_FALSE(adminClaimStillHeld(*op, layout, "v", UInt128(7), 5)) << "fenced";
    MountLease farewell = renewed;
    farewell.min_active_build_sequence = std::numeric_limits<uint64_t>::max();
    ASSERT_TRUE(std::holds_alternative<Committed>((*op).replace(layout.mountKey("v"), encodeMountLease(farewell),
        (*op).read(layout.mountKey("v"), Retry::once())->etag, Retry::once())));
    EXPECT_FALSE(adminClaimStillHeld(*op, layout, "v", UInt128(7), 5)) << "farewelled";
    EXPECT_FALSE(adminClaimStillHeld(*op, layout, "absent", UInt128(7), 5));
}

/// Another server took the server_root_id between the admin's claim and its tombstone (a rejoin). The
/// tombstone stops before it writes anything.
TEST_F(CASDecommission, StopsWhenTheOwnerUuidChanged)
{
    auto backend = startVictim(std::make_shared<SwapOwnerBeforeTombstoneBackend>());
    backend->arm();
    seed("p/roots/victim/loose_file");

    expectThrowsCode(ErrorCodes::ABORTED, [&] { run("admin"); });

    ASSERT_TRUE(backend->fired());
    EXPECT_EQ(owner().server_uuid, UInt128(0xBEEF));
    EXPECT_FALSE(owner().retired_at_ms.has_value()) << "the new owner was not tombstoned";
    EXPECT_TRUE(exists("p/roots/victim/loose_file")) << "nothing was drained";
}

/// An absent counter is refused, whether or not an owner is left, and never re-created; nothing is written.
TEST_F(CASDecommission, AbsentCounterIsRefused)
{
    for (const bool owner_left : {true, false})
    {
        SCOPED_TRACE(owner_left);
        startVictim();
        OperationForTest raw_op(*victim_backend);
        ASSERT_EQ((*raw_op).removeCurrent(layout.epochKey("victim"), Retry::once()), Removal::Removed);
        if (!owner_left)
            ASSERT_EQ((*raw_op).removeCurrent(layout.ownerKey("victim"), Retry::once()), Removal::Removed);
        const auto before = controlObjects();

        expectThrowsCodeWithMessage(ErrorCodes::CORRUPTED_DATA, "no epoch counter", [&] { run("admin"); });

        EXPECT_EQ(controlObjects(), before);
    }
}

/// The owner was deleted by hand while the counter survived: the run recovers the uuid from the lease,
/// re-creates the owner over the empty subtree and retires it.
TEST_F(CASDecommission, OwnerDeletedByHandIsRecreatedFromTheLease)
{
    auto backend = startVictim();
    OperationForTest raw_op(*backend);
    ASSERT_EQ((*raw_op).removeCurrent(layout.ownerKey("victim"), Retry::once()), Removal::Removed);

    const auto report = run("a2");

    EXPECT_TRUE(report.warnings.empty());
    EXPECT_TRUE(report.member_retired);
    expectOwner(3, true, true);
    EXPECT_TRUE(exists(layout.epochKey("victim")));
}

TEST_F(CASDecommission, UnconfirmedFarewellIsNotRetired)
{
    auto backend = startVictim(std::make_shared<FenceBeforeFarewellBackend>());
    backend->arm();
    DB::Cas::tests::ScopedLoggerCapture capture("CasDecommission", "warning");

    const auto report = run("admin");

    ASSERT_TRUE(backend->fired());
    EXPECT_FALSE(report.member_retired);
    ASSERT_FALSE(report.warnings.empty());
    for (const String & warning : report.warnings)
        EXPECT_NE(capture.captured().find(warning), String::npos) << "not logged: " << warning;
    expectOwner(2, true, false);
    EXPECT_TRUE(slot().gc_fenced);
}

/// The mark is written on the control plane after the admin pool is gone, and does not wait for GC to delete
/// the victim's catalog rows.
TEST_F(CASDecommission, VerifiedMarkDoesNotWaitForCatalogGc)
{
    auto backend = startVictim(std::make_shared<InMemoryBackend>(), oneTable);
    const auto report = run("admin");
    EXPECT_EQ(report.namespaces_removed, 1u);
    EXPECT_EQ(report.namespaces_pending_gc, 1u);
    EXPECT_TRUE(report.warnings.empty());
    EXPECT_TRUE(report.member_retired);
    expectOwner(3, true, true);
}

/// A rerun over an earlier retirement, verified or not, loses its claim after its tombstone: nothing is rolled
/// back and the owner stays retired. An unverified tombstone landed while its run held the claim, so no
/// member can be running under it.
TEST_F(CASDecommission, RollbackKeepsAPreviousRetirement)
{
    for (const bool verified : {true, false})
    {
        SCOPED_TRACE(verified);
        auto backend = startVictim();
        if (verified)
            ASSERT_TRUE(run("a1").member_retired);
        else
            tombstoneOwner();
        const uint64_t life_before = owner().owner_life;

        bool fired = false;
        backend->onWriteCommitted(layout.ownerKey("victim"), [&]
        {
            if (std::exchange(fired, true))
                return;
            fenceSlot();   /// the rerun's tombstone landed; its claim is lost now
        });
        const auto rerun = run("a2");

        ASSERT_TRUE(fired);
        EXPECT_FALSE(rerun.member_retired);
        EXPECT_EQ(rerun.warnings.size(), 1u) << fmt::format("{}", fmt::join(rerun.warnings, "; "));
        expectOwner(life_before + 1, true, verified);   /// the rerun's tombstone, kept
    }
}

/// Three runs: one verifies, one refreshes the tombstone and dies, one reruns and is fenced before its
/// confirm. The mark survives all three.
TEST_F(CASDecommission, RefreshKeepsTheVerifiedMark)
{
    auto backend = startVictim();
    ASSERT_TRUE(run("a1").member_retired);
    ASSERT_TRUE(owner().retirement_verified);

    tombstoneOwner();   /// a second run's tombstone, then death: life 4, mark kept
    ASSERT_TRUE(owner().retirement_verified);

    bool fired = false;
    backend->onWriteCommitted(layout.ownerKey("victim"), [&]
    {
        if (std::exchange(fired, true))
            return;
        fenceSlot();
    });
    const auto third = run("a3");
    ASSERT_TRUE(fired);
    EXPECT_FALSE(third.member_retired);
    expectOwner(5, true, true);
}

namespace
{

/// What an operator leaves after clearing a verified tombstone by hand to restart the member: the mark of the
/// retired life stays.
void clearTombstoneByHand(Backend & backend, const Layout & layout, const String & srid)
{
    OperationForTest op(backend);
    const auto current = (*op).read(layout.ownerKey(srid), Retry::once());
    ASSERT_TRUE(current.has_value());
    OwnerObject owner = decodeOwner(current->bytes);
    ASSERT_TRUE(owner.retirement_verified);
    owner.retired_at_ms.reset();
    ASSERT_TRUE(std::holds_alternative<Committed>(
        (*op).replace(layout.ownerKey(srid), encodeOwner(owner), current->etag, Retry::once())));
}

}

/// The member restarted over a hand-cleared tombstone. The next decommission loses its claim after its
/// tombstone: the mark of the earlier life must not stop the rollback.
TEST_F(CASDecommission, HandClearedTombstoneStillRollsBackALostClaim)
{
    auto backend = startVictim();
    ASSERT_TRUE(run("a1").member_retired);
    clearTombstoneByHand(*backend, layout, "victim");
    { auto victim = openVictim(backend); }

    bool fired = false;
    backend->onWriteCommitted(layout.ownerKey("victim"), [&]
    {
        if (std::exchange(fired, true))
            return;
        fenceSlot();
    });
    const auto second = run("a2");

    ASSERT_TRUE(fired);
    EXPECT_FALSE(second.member_retired);
    EXPECT_EQ(second.warnings.size(), 1u) << fmt::format("{}", fmt::join(second.warnings, "; "));
    expectOwner(5, false, false);   /// first run 3, second tombstone 4, rollback 5
}

/// The member restarted over a hand-cleared tombstone. The next decommission's farewell is fenced: without a
/// mark of its own life the run reports rerun.
TEST_F(CASDecommission, HandClearedTombstoneIsNotARetirementOfTheNextRun)
{
    auto backend = startVictim(std::make_shared<FenceBeforeFarewellBackend>());
    ASSERT_TRUE(run("a1").member_retired);
    clearTombstoneByHand(*backend, layout, "victim");
    { auto victim = openVictim(backend); }
    backend->arm();

    const auto second = run("a2");

    ASSERT_TRUE(backend->fired());
    EXPECT_FALSE(second.member_retired);
    expectOwner(4, true, false);
}

/// A run dies after each step; a rerun completes over the run's own tombstone.
TEST_F(CASDecommission, ResumesOverItsOwnTombstone)
{
    enum class DieAt { AfterTombstone, AfterFarewell, AfterMark };
    for (const DieAt die_at : {DieAt::AfterTombstone, DieAt::AfterFarewell, DieAt::AfterMark})
    {
        SCOPED_TRACE(static_cast<int>(die_at));
        auto backend = startVictim();
        seed("p/roots/victim/loose_file");
        /// `BAD_ARGUMENTS` is a deterministic local failure to the request engine: the attempt is not
        /// reissued and not resolved by a read, so the landed write surfaces as a refusal -- the shape of a
        /// process that died right after its write landed.
        const auto died_with = [](const char * what)
        {
            return std::make_exception_ptr(DB::Exception(DB::ErrorCodes::BAD_ARGUMENTS, "{}", what));
        };
        bool died = false;
        switch (die_at)
        {
            case DieAt::AfterTombstone:
                backend->onWriteCommitted(layout.ownerKey("victim"), [&]
                {
                    if (!std::exchange(died, true))
                        std::rethrow_exception(died_with("died after the tombstone"));
                });
                break;
            case DieAt::AfterFarewell:
                /// The farewell is written by `~Pool`, which swallows a throw; die on the verify read instead.
                backend->onWriteCommitted(layout.mountKey("victim"), [&]
                {
                    if (slot().isFarewell() && !std::exchange(died, true))
                        backend->failNextReadWith(layout.ownerKey("victim"), died_with("died after the farewell"));
                });
                break;
            case DieAt::AfterMark:
                backend->onWriteCommitted(layout.ownerKey("victim"), [&]
                {
                    if (owner().retirement_verified && !std::exchange(died, true))
                        std::rethrow_exception(died_with("died after the mark"));
                });
                break;
        }

        DecommissionReport first;
        bool threw = false;
        try
        {
            first = run("a1");
        }
        catch (const DB::Exception &)
        {
            threw = true;
        }
        ASSERT_TRUE(died);
        switch (die_at)
        {
            case DieAt::AfterTombstone:
                EXPECT_TRUE(threw) << "the tombstone write is refused after it landed; nothing else ran";
                break;
            case DieAt::AfterFarewell:
                EXPECT_FALSE(threw) << "a failed verify read is reported, not thrown";
                EXPECT_FALSE(first.member_retired);
                break;
            case DieAt::AfterMark:
                EXPECT_FALSE(threw) << "an unclear mark write is settled by the owner re-read";
                EXPECT_TRUE(first.member_retired) << "the re-read finds the verified mark";
                break;
        }
        expectOwner(die_at == DieAt::AfterMark ? 3 : 2, true, die_at == DieAt::AfterMark);

        const auto rerun = run("a2");
        EXPECT_TRUE(rerun.warnings.empty()) << fmt::format("{}", fmt::join(rerun.warnings, "; "));
        EXPECT_TRUE(rerun.member_retired);
        expectOwner(die_at == DieAt::AfterMark ? 5 : 4, true, true);
        EXPECT_FALSE(exists("p/roots/victim/loose_file"));
    }
}

namespace
{

/// The first roots delete under the victim lets the admin's local lease deadline pass (the fake clock jumps a
/// TTL); every delete admitted on the mount plane after it is refused. Optionally also GC-fences the body.
class ExpireLeaseOnFirstRootsDeleteBackend : public InMemoryBackend
{
public:
    std::shared_ptr<DB::Cas::tests::FakeClock> clock;
    uint64_t jump_ms = 0;
    bool fence_body = false;
    bool fired() const { return done; }

    RawRemoval remove(const String & key, const String & expected_value, TransportAccess & access) override
    {
        if (!done && key.starts_with("p/roots/victim/"))
        {
            done = true;
            clock->now += jump_ms;
            if (fence_body)
            {
                const auto current = InMemoryBackend::read(mount_key, access);
                const MountLease body = decodeMountLease(current->bytes).fencedCopy();
                if (!InMemoryBackend::write(mount_key, encodeMountLease(body), current->value, access))
                    throw std::runtime_error("expire fixture: the fence conflicted");
            }
        }
        return InMemoryBackend::remove(key, expected_value, access);
    }

private:
    inline static const String mount_key = "p/gc/server-roots/victim/mount";
    bool done = false;
};

/// On the staging LIST (the request before the roots LIST) lets the admin's local lease deadline pass and
/// GC-fences its body in the store: the fence loss then surfaces on the roots LIST page, not on a delete.
class ExpireLeaseBeforeRootsListBackend : public InMemoryBackend
{
public:
    using Backend::list;
    std::shared_ptr<DB::Cas::tests::FakeClock> clock;
    uint64_t jump_ms = 0;
    bool fired() const { return done; }

    RawListPage list(const String & prefix, const String & cursor, size_t limit, TransportAccess & access) override
    {
        RawListPage page = InMemoryBackend::list(prefix, cursor, limit, access);
        if (!done && prefix == "p/staging/victim/")
        {
            done = true;
            clock->now += jump_ms;
            const auto current = InMemoryBackend::read(mount_key, access);
            const MountLease body = decodeMountLease(current->bytes).fencedCopy();
            if (!InMemoryBackend::write(mount_key, encodeMountLease(body), current->value, access))
                throw std::runtime_error("expire-before-list fixture: the fence conflicted");
        }
        return page;
    }

private:
    inline static const String mount_key = "p/gc/server-roots/victim/mount";
    bool done = false;
};

/// A store whose `roots/` objects carry content-derived ETags (as S3 does). While `capture` is set, every
/// delete under the victim's prefixes applies and also leaves a copy of the attempt in flight, which the test
/// delivers later with `replay`.
class LateDeleteBackend : public InMemoryBackend
{
public:
    struct Late { String key; Etag etag; };
    std::vector<Late> late;
    bool capture = false;

    std::optional<Raw> read(const String & key, TransportAccess & access) override
    {
        auto raw = InMemoryBackend::read(key, access);
        if (raw && isRoots(key))
            raw->value = contentValue(raw->bytes);
        return raw;
    }

    std::optional<RawMeta> head(const String & key, TransportAccess & access) override
    {
        auto meta = InMemoryBackend::head(key, access);
        if (meta && isRoots(key))
            if (const auto raw = InMemoryBackend::read(key, access))
                meta->value = contentValue(raw->bytes);
        return meta;
    }

    RawListPage list(const String & prefix, const String & cursor, size_t limit, TransportAccess & access) override
    {
        RawListPage page = InMemoryBackend::list(prefix, cursor, limit, access);
        for (RawListedKey & listed : page.keys)
            if (listed.value && isRoots(listed.key))
                if (const auto raw = InMemoryBackend::read(listed.key, access))
                    listed.value = contentValue(raw->bytes);
        return page;
    }

    std::expected<String, RawConflict> write(const String & key, const String & bytes,
                                             const std::optional<String> & expected_value, TransportAccess & access) override
    {
        if (!isRoots(key))
            return InMemoryBackend::write(key, bytes, expected_value, access);
        std::optional<String> minted_expected;
        if (expected_value)
        {
            const auto current = InMemoryBackend::read(key, access);
            if (!current || contentValue(current->bytes) != *expected_value)
                return std::unexpected(RawConflict{});
            minted_expected = current->value;
        }
        const auto written = InMemoryBackend::write(key, bytes, minted_expected, access);
        if (!written)
            return written;
        return contentValue(bytes);
    }

    RawRemoval remove(const String & key, const String & expected_value, TransportAccess & access) override
    {
        if (capture && isVictim(key))
        {
            OperationForTest op(*this);
            if (const auto meta = (*op).head(key, Retry::once()))
                late.push_back({key, meta->etag});
        }
        if (!isRoots(key))
            return InMemoryBackend::remove(key, expected_value, access);
        const auto current = InMemoryBackend::read(key, access);
        if (!current)
            return RawRemoval::Gone;
        if (contentValue(current->bytes) != expected_value)
            return RawRemoval::Mismatch;
        return InMemoryBackend::remove(key, current->value, access);
    }

    /// Deliver every captured attempt now, through the ordinary exact-incarnation delete.
    std::vector<std::pair<String, Removal>> replay()
    {
        std::vector<std::pair<String, Removal>> outcomes;
        OperationForTest op(*this);
        for (const Late & attempt : late)
            outcomes.emplace_back(attempt.key, (*op).remove(attempt.key, attempt.etag, Retry::once()));
        return outcomes;
    }

private:
    static bool isRoots(const String & key) { return key.starts_with("p/roots/victim/"); }
    static bool isVictim(const String & key)
    {
        return isRoots(key) || key.starts_with("p/staging/victim/") || key.starts_with("p/cas/manifests/victim/")
            || key.starts_with("p/cas/ns/");
    }
    static String contentValue(const String & bytes) { return "content-" + DB::Cas::tests::hexOf(bytes); }
};

/// Each delete under the victim's roots prefix takes `delay`, as a slow store would. `on_first_delete` runs
/// once, before the first of them; `after_delay` runs after each delay.
class SlowRootsDeleteBackend : public InMemoryBackend
{
public:
    std::chrono::milliseconds delay{0};
    std::function<void()> on_first_delete;
    std::function<void()> after_delay;

    RawRemoval remove(const String & key, const String & expected_value, TransportAccess & access) override
    {
        if (key.starts_with("p/roots/victim/"))
        {
            if (on_first_delete)
                std::exchange(on_first_delete, {})();
            std::this_thread::sleep_for(delay);
            if (after_delay)
                after_delay();
        }
        return InMemoryBackend::remove(key, expected_value, access);
    }
};

/// The tombstone write lands, GC fences the admin's body, and the write's answer is lost; the owner then
/// cannot be read for `unreadable_ms` of the fake clock, longer than the write's own retry window, so the
/// write gives up after sending.
class OwnerUnreadableAfterTombstoneBackend : public InMemoryBackend
{
public:
    std::shared_ptr<DB::Cas::tests::FakeClock> clock;
    std::atomic<uint64_t> unreadable_ms = 0;
    bool landed() const { return landed_at.load() != 0; }

    std::expected<String, RawConflict> write(const String & key, const String & bytes,
                                             const std::optional<String> & expected_value, TransportAccess & access) override
    {
        if (!landed() && key == owner_key && decodeOwner(bytes).retired_at_ms)
        {
            landed_at = clock->now.load();
            const auto current = InMemoryBackend::read(mount_key, access);
            const MountLease body = decodeMountLease(current->bytes).fencedCopy();
            if (!InMemoryBackend::write(mount_key, encodeMountLease(body), current->value, access))
                throw std::runtime_error("unreadable-owner fixture: the fence conflicted");
            injectAmbiguousLandedWrite(key);
        }
        return InMemoryBackend::write(key, bytes, expected_value, access);
    }

    std::optional<Raw> read(const String & key, TransportAccess & access) override
    {
        if (landed() && key == owner_key && clock->now.load() < landed_at.load() + unreadable_ms.load())
            throw Poco::TimeoutException("unreadable-owner fixture: the owner does not answer");
        return InMemoryBackend::read(key, access);
    }

private:
    inline static const String mount_key = "p/gc/server-roots/victim/mount";
    inline static const String owner_key = "p/gc/server-roots/victim/owner";
    std::atomic<uint64_t> landed_at = 0;
};

}

TEST_F(CASDecommission, DrainStopsOnTheAdminLeaseFence)
{
    auto backend = startVictim(std::make_shared<ExpireLeaseOnFirstRootsDeleteBackend>());
    seed("p/roots/victim/a_first");
    seed("p/roots/victim/b_second");
    auto clock = farClock();
    backend->clock = clock;
    backend->jump_ms = 31'000;   /// past the default 30 s TTL

    const auto report = runOnClock("a1", clock);

    ASSERT_TRUE(backend->fired());
    EXPECT_FALSE(report.member_retired);
    ASSERT_FALSE(report.warnings.empty());
    EXPECT_EQ(report.mountpoint_objects_removed, 1u) << "the first delete was already sent";
    EXPECT_TRUE(exists("p/roots/victim/b_second"))
        << "a delete admitted after the lease deadline is refused locally";
    expectOwner(2, true, false);
}

/// After the first delete refused by the fence the walk stops: one warning, the later objects untouched.
TEST_F(CASDecommission, DrainStopsWalkingAfterTheFirstRefusedDelete)
{
    auto backend = startVictim(std::make_shared<ExpireLeaseOnFirstRootsDeleteBackend>());
    for (const char * name : {"a_first", "b_second", "c_third"})
        seed(String("p/roots/victim/") + name);
    auto clock = farClock();
    backend->clock = clock;
    backend->jump_ms = 31'000;

    const auto report = runOnClock("a1", clock);

    ASSERT_TRUE(backend->fired());
    const auto refused = std::count_if(report.warnings.begin(), report.warnings.end(),
        [](const String & warning) { return warning.starts_with("decommission drain: "); });
    EXPECT_EQ(refused, 1) << fmt::format("{}", fmt::join(report.warnings, "; "));
    EXPECT_TRUE(exists("p/roots/victim/c_third"));
}

/// The admin is fenced in the store AND past its local deadline: the drain stops, and the rollback goes
/// through the control plane, which the mount fence does not gate.
TEST_F(CASDecommission, RollbackRunsOnTheControlPlaneAfterAFence)
{
    auto backend = startVictim(std::make_shared<ExpireLeaseOnFirstRootsDeleteBackend>());
    seed("p/roots/victim/a_first");
    seed("p/roots/victim/b_second");
    auto clock = farClock();
    backend->clock = clock;
    backend->jump_ms = 31'000;
    backend->fence_body = true;

    const auto report = runOnClock("a1", clock);

    ASSERT_TRUE(backend->fired());
    EXPECT_FALSE(report.member_retired);
    expectOwner(3, false, false);   /// rolled back
    EXPECT_TRUE(slot().gc_fenced);
}

TEST_F(CASDecommission, FenceLossOnAListPageStillRunsTheConfirm)
{
    auto backend = startVictim(std::make_shared<ExpireLeaseBeforeRootsListBackend>());
    auto clock = farClock();
    backend->clock = clock;
    backend->jump_ms = 31'000;

    EXPECT_THROW(runOnClock("a1", clock), DB::Exception);

    ASSERT_TRUE(backend->fired());
    expectOwner(3, false, false);   /// the exception exit confirmed and rolled back
}

/// Real time: the background renewer keeps the admin's lease alive across a drain longer than one TTL (1 s
/// TTL, renewal every 200 ms, three roots deletes of 400 ms each), and the run retires the member.
TEST_F(CASDecommission, LongDrainOutlivesOneTtl)
{
    auto backend = startVictim(std::make_shared<SlowRootsDeleteBackend>());
    backend->delay = std::chrono::milliseconds(400);
    for (const char * name : {"a", "b", "c"})
        seed(String("p/roots/victim/") + name);
    /// A stop that races a renewal skips the farewell; that race is a known open defect. The last delete
    /// waits for a renewal to land and then half a renewal period, so the stop lands between two renewals.
    struct MountWrites
    {
        std::mutex mutex;
        std::condition_variable landed;
        size_t count = 0;
    };
    auto mount_writes = std::make_shared<MountWrites>();
    backend->onWriteCommitted(layout.mountKey("victim"), [mount_writes]
    {
        std::lock_guard lock(mount_writes->mutex);
        ++mount_writes->count;
        mount_writes->landed.notify_all();
    });
    auto deletes = std::make_shared<size_t>(0);
    backend->after_delay = [mount_writes, deletes]
    {
        if (++*deletes != 3)
            return;
        std::unique_lock lock(mount_writes->mutex);
        const size_t seen = mount_writes->count;
        ASSERT_TRUE(mount_writes->landed.wait_for(lock, std::chrono::seconds(5), [&] { return mount_writes->count > seen; }));
        lock.unlock();
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    };

    const auto started = std::chrono::steady_clock::now();
    const auto report = run("a1", realTimeConfig());

    EXPECT_GE(std::chrono::steady_clock::now() - started, std::chrono::milliseconds(1200));
    EXPECT_EQ(report.mountpoint_objects_removed, 3u);
    EXPECT_TRUE(report.warnings.empty()) << fmt::format("{}", fmt::join(report.warnings, "; "));
    EXPECT_TRUE(report.member_retired);
}

/// Once the owner is tombstoned, the admin's own self-remount fails on the owner check and never reclaims
/// under a fresh epoch.
TEST_F(CASDecommission, FencedAdminFailsInsteadOfRemounting)
{
    auto backend = startVictim();
    auto admin = Pool::openForDecommission(backend, PoolConfig{.pool_prefix = "p", .server_root_id = "a1"}, "victim");
    const uint64_t admin_epoch = admin->writerEpoch();
    tombstoneOwner();
    fenceSlot();
    admin->tripMountLost();   /// what the renewer does when it finds its body fenced
    EXPECT_FALSE(admin->tryRemountOnce());
    const MountLease body = slot();
    EXPECT_EQ(body.writer_epoch, admin_epoch);
    EXPECT_TRUE(body.gc_fenced);
    EXPECT_FALSE(admin->mayMutate());
}

/// The admin is GC-fenced during its drain; its renewer trips and its self-remount fails
/// (owner retired). The run's exit confirms, finds the claim lost and rolls the tombstone back. Real time:
/// TTL 1 s, renewal every 200 ms, a 400 ms delete per roots object.
TEST_F(CASDecommission, FailedRemountStillRunsTheConfirm)
{
    auto backend = startVictim(std::make_shared<SlowRootsDeleteBackend>());
    backend->delay = std::chrono::milliseconds(400);
    for (const char * name : {"a", "b", "c", "d"})
        seed(String("p/roots/victim/") + name);
    bool fenced = false;
    backend->on_first_delete = [&]
    {
        fenced = true;
        fenceSlot();
    };
    const uint64_t remount_failures_before = ProfileEvents::global_counters[ProfileEvents::CASRemountFailed].load();

    const auto report = run("a1", realTimeConfig());

    ASSERT_TRUE(fenced);
    EXPECT_FALSE(report.member_retired);
    EXPECT_GT(ProfileEvents::global_counters[ProfileEvents::CASRemountFailed].load(), remount_failures_before)
        << "the admin's self-remount ran and failed";
    expectOwner(3, false, false);   /// rolled back: the claim was lost
    EXPECT_TRUE(slot().gc_fenced);
}

/// A paused run resumes after a rerun refreshed the tombstone: its rollback names an older incarnation and
/// cannot remove the rerun's tombstone.
TEST_F(CASDecommission, PausedRunCannotUntombstoneARerunsTombstone)
{
    auto backend = startVictim();
    std::optional<DecommissionReport> rerun;
    bool armed = true;
    backend->onWriteCommitted(layout.ownerKey("victim"), [&]
    {
        if (!std::exchange(armed, false))
            return;
        /// Run 1 is paused right after its tombstone. GC fences it; the rerun reclaims, refreshes the
        /// tombstone, drains, farewells and verifies.
        fenceSlot();
        rerun = run("a2");
    });

    const auto first = run("a1");

    ASSERT_TRUE(rerun.has_value());
    EXPECT_TRUE(rerun->member_retired);
    EXPECT_FALSE(first.member_retired);
    EXPECT_EQ(first.warnings.size(), 1u) << fmt::format("{}", fmt::join(first.warnings, "; "));
    expectOwner(4, true, true);   /// run 1 tombstone 2, rerun tombstone 3, rerun mark 4
}

/// A delete attempt sent by a run and delivered after a rejoin can remove only an object whose key and bytes
/// repeat: the roots objects. Namespace, manifest and staging keys of the new member never match.
TEST_F(CASDecommission, InFlightDeleteAfterRejoinHitsOnlyIdenticalRootsBytes)
{
    auto backend = startVictim(std::make_shared<LateDeleteBackend>(), oneTable);   /// no debris: it would block the mark
    const RootNamespace ns("victim/db/t1");
    seed("p/staging/victim/3/old.tmp");
    {
        OperationForTest seed_op(*backend);
        (*seed_op).create("p/roots/victim/clickhouse_access_check", "probe", Retry::once());
    }
    backend->capture = true;
    const auto report = run("a1");
    backend->capture = false;
    ASSERT_TRUE(report.member_retired);
    ASSERT_FALSE(backend->late.empty());

    /// The rejoin and the new member's first writes, by hand: the next owner life, a new staging object, a
    /// new manifest, and the access probe with the same bytes under the same key.
    {
        OperationForTest op(*backend);
        const auto current = (*op).read(layout.ownerKey("victim"), Retry::once());
        const OwnerObject rejoined = nextOwnerLife(decodeOwner(current->bytes), std::nullopt, false);
        ASSERT_TRUE(std::holds_alternative<Committed>(
            (*op).replace(layout.ownerKey("victim"), encodeOwner(rejoined), current->etag, Retry::once())));
        ASSERT_TRUE(std::holds_alternative<Committed>((*op).create("p/staging/victim/9/new.tmp", "y", Retry::once())));
        ASSERT_TRUE(std::holds_alternative<Committed>((*op).create("p/roots/victim/clickhouse_access_check", "probe", Retry::once())));
    }
    const ManifestRef new_ref{.writer_epoch = 9, .build_sequence = 1, .manifest_ordinal = 1};
    const ManifestId new_manifest = writeManifestRaw(*backend, layout, ns, new_ref, {});

    backend->replay();
    EXPECT_FALSE(exists("p/roots/victim/clickhouse_access_check"))
        << "identical bytes repeat the ETag: the late delete lands on the new member's probe (the documented residual)";
    EXPECT_TRUE(exists("p/staging/victim/9/new.tmp"));
    EXPECT_TRUE(exists(layout.manifestKey(new_manifest)));
}

/// The tombstone lands and the engine throws before reporting it, while GC fences the admin: the exit reads
/// the owner back, finds this run's tombstone, sees the lost claim and rolls the tombstone back.
TEST_F(CASDecommission, TombstoneLandedBehindAnErrorIsConfirmed)
{
    auto backend = startVictim();
    bool landed = false;
    backend->onWriteCommitted(layout.ownerKey("victim"), [&]
    {
        if (std::exchange(landed, true))
            return;
        fenceSlot();
        throw DB::Exception(DB::ErrorCodes::BAD_ARGUMENTS, "the tombstone landed and its answer was lost");
    });

    EXPECT_THROW(run("a1"), DB::Exception);

    ASSERT_TRUE(landed);
    expectOwner(3, false, false);   /// claim 1, tombstone 2, rollback 3
}

/// The tombstone write gives up after sending; the owner answers again later and holds this run's tombstone.
/// GC fenced the admin, so the exit rolls the tombstone back.
TEST_F(CASDecommission, TombstoneWriteThatGaveUpAfterSendingIsConfirmed)
{
    auto backend = startVictim(std::make_shared<OwnerUnreadableAfterTombstoneBackend>());
    auto clock = farClock();
    backend->clock = clock;
    backend->unreadable_ms = 100'000;   /// longer than the tombstone write retries

    EXPECT_THROW(runOnClock("a1", clock), DB::Exception);

    ASSERT_TRUE(backend->landed());
    backend->unreadable_ms = 0;
    expectOwner(3, false, false);   /// claim 1, tombstone 2, rollback 3
}

/// A later run tombstones the owner between this run's farewell and its verify. The owner is past this run's
/// tombstone but unverified: this run reports rerun.
TEST_F(CASDecommission, LaterUnverifiedTombstoneIsNotARetirement)
{
    auto backend = startVictim();
    bool refreshed = false;
    backend->onWriteCommitted(layout.mountKey("victim"), [&]
    {
        if (slot().isFarewell() && !std::exchange(refreshed, true))
            tombstoneOwner();
    });

    const auto report = run("a1");

    ASSERT_TRUE(refreshed);
    EXPECT_FALSE(report.member_retired);
    EXPECT_EQ(report.warnings.size(), 2u) << fmt::format("{}", fmt::join(report.warnings, "; "));
    expectOwner(3, true, false);
}

/// The owner moves on to a later, unretired life between this run's farewell and its verify (a rejoin, or
/// another run's rollback). Only a verified mark is a retirement: the run reports 0 and says the owner moved.
TEST_F(CASDecommission, OwnerThatMovedOnIsNotReportedRetired)
{
    auto backend = startVictim();
    bool moved = false;
    backend->onWriteCommitted(layout.mountKey("victim"), [&]
    {
        if (!slot().isFarewell() || std::exchange(moved, true))
            return;
        OperationForTest op(*backend);
        const auto current = (*op).read(layout.ownerKey("victim"), Retry::once());
        ASSERT_TRUE(std::holds_alternative<Committed>((*op).replace(layout.ownerKey("victim"),
            encodeOwner(nextOwnerLife(decodeOwner(current->bytes), std::nullopt, false)), current->etag, Retry::once())));
    });

    const auto report = run("a1");

    ASSERT_TRUE(moved);
    EXPECT_FALSE(report.member_retired);
    ASSERT_EQ(report.warnings.size(), 2u) << fmt::format("{}", fmt::join(report.warnings, "; "));
    EXPECT_NE(report.warnings.back().find("moved on to life 3"), String::npos) << report.warnings.back();
    EXPECT_FALSE(owner().retired_at_ms.has_value());
}

TEST_F(CASDecommission, RetirementLogsTheUuidAndLife)
{
    auto backend = startVictim();
    DB::Cas::tests::ScopedLoggerCapture capture("CasDecommission", "information");

    const auto report = run("a1");

    ASSERT_TRUE(report.member_retired);
    const String expected = "retired server_uuid=" + u128ToHex(owner().server_uuid) + " at owner life 2";
    EXPECT_NE(capture.captured().find(expected), String::npos) << capture.captured();
}

/// The owner moves under the mark write: the run reports why the mark is missing, not only that it is.
TEST_F(CASDecommission, MarkWriteConflictIsReported)
{
    auto backend = startVictim();
    bool farewell_landed = false;
    backend->onWriteCommitted(layout.mountKey("victim"), [&]
    {
        if (slot().isFarewell())
            farewell_landed = true;
    });
    bool moved = false;
    backend->onBeforeWrite(layout.ownerKey("victim"), [&]
    {
        if (!farewell_landed || std::exchange(moved, true))
            return;
        OperationForTest op(*backend);
        const auto current = (*op).read(layout.ownerKey("victim"), Retry::once());
        ASSERT_TRUE(std::holds_alternative<Committed>(
            (*op).replace(layout.ownerKey("victim"), current->bytes, current->etag, Retry::once())));
    });

    const auto report = run("a1");

    ASSERT_TRUE(moved);
    EXPECT_FALSE(report.member_retired);
    const auto mark_warning = std::find_if(report.warnings.begin(), report.warnings.end(),
        [](const String & warning) { return warning.starts_with("the verified mark was not written"); });
    EXPECT_NE(mark_warning, report.warnings.end()) << fmt::format("{}", fmt::join(report.warnings, "; "));
    EXPECT_FALSE(owner().retirement_verified);
}

namespace
{

/// Once armed, the next catalog read fails, and so does the slot read after it: a drain that fails on a store
/// outage, followed by a confirm that fails for the same reason.
class DrainThenConfirmFailBackend : public InMemoryBackend
{
public:
    void arm() { armed = true; }
    bool confirmFailed() const { return slot_failed; }

    std::optional<Raw> read(const String & key, TransportAccess & access) override
    {
        if (armed && !catalog_failed && key == catalog_key)
        {
            catalog_failed = true;
            throw std::runtime_error("injected: the catalog does not answer");
        }
        if (catalog_failed && !slot_failed && key == mount_key)
        {
            slot_failed = true;
            throw std::runtime_error("injected: the slot does not answer");
        }
        return InMemoryBackend::read(key, access);
    }

private:
    inline static const String catalog_key = Layout("p").refCatalogKey();
    inline static const String mount_key = "p/gc/server-roots/victim/mount";
    bool armed = false;
    bool catalog_failed = false;
    bool slot_failed = false;
};

}

/// The drain fails and the confirm's own read fails too: the drain's error is the one that propagates, and
/// the failed confirm is logged.
TEST_F(CASDecommission, FailedConfirmKeepsTheDrainError)
{
    auto backend = startVictim(std::make_shared<DrainThenConfirmFailBackend>());
    backend->onWriteCommitted(layout.ownerKey("victim"), [&] { backend->arm(); });
    DB::Cas::tests::ScopedLoggerCapture capture("CasDecommission", "warning");

    try
    {
        run("a1");
        ADD_FAILURE() << "the drain must fail";
    }
    catch (const std::exception & e)
    {
        EXPECT_NE(String(e.what()).find("the catalog does not answer"), String::npos) << e.what();
    }

    ASSERT_TRUE(backend->confirmFailed());
    EXPECT_NE(capture.captured().find("could not confirm the admin claim"), String::npos) << capture.captured();
}

TEST_F(CASDecommission, OpenLogsTheSkippedProbe)
{
    auto backend = startVictim();
    DB::Cas::tests::ScopedLoggerCapture capture("CasPool", "information");
    auto admin = Pool::openForDecommission(backend, PoolConfig{.pool_prefix = "p", .server_root_id = "a1"}, "victim");
    EXPECT_NE(capture.captured().find("capability probe was skipped"), String::npos) << capture.captured();
}

/// The admin impersonates the owner it retires, so whose life is "ours" is ambiguous: it never takes another
/// uuid's terminal body and refuses before writing anything.
TEST_F(CASDecommission, AdminDoesNotReclaimAForeignTerminalBody)
{
    startVictim();
    {
        OperationForTest op(*victim_backend);
        const auto got = (*op).read(layout.mountKey("victim"), Retry::once());
        MountLease body = decodeMountLease(got->bytes);
        body.server_uuid = UInt128(5);
        body.seq += 1;
        ASSERT_TRUE(std::holds_alternative<Committed>(
            (*op).replace(layout.mountKey("victim"), encodeMountLease(body), got->etag, Retry::once())));
    }
    const OwnerObject owner_before = owner();
    const MountLease slot_before = slot();

    expectThrowsCodeWithMessage(ErrorCodes::ABORTED, "alive or contended", [&] { run("admin"); });

    expectOwner(owner_before.owner_life, /*retired=*/false, /*verified=*/false);
    EXPECT_EQ(slot().seq, slot_before.seq);
}

namespace
{
/// A rejoin needs a real server uuid: a start without one (uuid 0) never rejoins.
const UInt128 victim_uuid = UInt128(7);

PoolConfig victimConfig(UInt128 uuid = victim_uuid)
{
    return PoolConfig{.pool_prefix = "p", .server_id = uuid, .server_root_id = "victim"};
}

/// One completed decommission of the victim with nothing left for GC: the state a rejoin starts from.
void retireVictim(const std::shared_ptr<InMemoryBackend> & backend, const String & admin_srid)
{
    const auto report = decommissionPoolMember(backend, PoolConfig{.pool_prefix = "p", .server_root_id = admin_srid}, "victim");
    if (!report.member_retired || report.namespaces_pending_gc != 0)
        throw std::runtime_error("fixture: the decommission did not retire the victim cleanly");
}

bool victimSubtreeEmpty(Backend & backend)
{
    const Layout layout("p");
    CasRequests requests = openRequestsForTest(backend);
    CasOperation op = requests.admit();
    return serverRootSubtreeEmpty(op, layout, "victim", CasRefCatalog::read(op, layout).catalog);
}

/// GC removes the dropped namespaces' catalog rows and manifests over a few rounds; a rejoin waits for all of them.
void gcUntilVictimSubtreeEmpty(const std::shared_ptr<InMemoryBackend> & backend)
{
    auto store = Pool::open(backend, PoolConfig{
        .pool_prefix = "p", .server_root_id = "gc", .gc_fold_threshold = 1, .gc_fold_max_defer_rounds = 0});
    Gc gc(store, UInt128{991});
    for (int round = 0; round < 8 && !victimSubtreeEmpty(*backend); ++round)
        runRegularRoundReclaiming(gc);
    if (!victimSubtreeEmpty(*backend))
        throw std::runtime_error("fixture: GC left data of the victim's previous life");
}

/// The rejoined member writes a part under the old life's namespace name; GC rounds run from another member
/// keep it, and the member's slot stays its own.
void expectWriteSurvivesGc(const std::shared_ptr<InMemoryBackend> & backend, const PoolPtr & member, UInt128 member_uuid)
{
    const RootNamespace ns{"victim/db/t1"};
    const String payload = "written after the rejoin";
    PartWriteInfo info;
    info.intended_ref = ns.string() + "/r";
    auto build = member->beginPartWrite(info);
    ManifestEntry entry;
    entry.path = "data.bin";
    entry.placement = EntryPlacement::Blob;
    entry.ref = idOf(payload);
    entry.blob_size = payload.size();
    const ManifestId manifest = build->stageManifest({entry});
    build->precommitAdd(ns, "r", manifest);
    build->putBlob(idOf(payload), BlobSource::fromString(payload));
    build->promote(ns, "r", build->buildId(), manifest);

    {
        auto gc_store = Pool::open(backend, PoolConfig{
            .pool_prefix = "p", .server_root_id = "gc", .gc_fold_threshold = 1, .gc_fold_max_defer_rounds = 0});
        Gc gc(gc_store, UInt128{992});
        for (int round = 0; round < 3; ++round)
        {
            runRegularRoundReclaiming(gc);
            member->renewWatermarkOnce();
        }
    }

    const Layout layout("p");
    EXPECT_EQ(member->listRefs(ns).size(), 1u);
    OperationForTest op(*backend);
    EXPECT_TRUE((*op).head(layout.manifestKey(manifest), Retry::once()).has_value());
    EXPECT_TRUE((*op).head(layout.blobKey(idOf(payload)), Retry::once()).has_value());
    const MountLease slot = readSlot(*backend, layout, "victim");
    EXPECT_TRUE(slot.isHeldBy(member_uuid, member->writerEpoch()));
    EXPECT_FALSE(slot.gc_fenced);
}

/// The slot body rewritten by hand; `edit` changes the decoded body before it is written back.
void rewriteSlot(Backend & backend, const Layout & layout, const std::function<void(MountLease &)> & edit)
{
    OperationForTest op(backend);
    const auto got = (*op).read(layout.mountKey("victim"), Retry::once());
    ASSERT_TRUE(got.has_value());
    MountLease body = decodeMountLease(got->bytes);
    edit(body);
    body.seq += 1;
    ASSERT_TRUE(std::holds_alternative<Committed>(
        (*op).replace(layout.mountKey("victim"), encodeMountLease(body), got->etag, Retry::once())));
}
}

TEST(CASRejoin, SameUuidStartsEmptyAndContinuesTheCounter)
{
    auto backend = std::make_shared<InMemoryBackend>();
    {
        auto victim = Pool::open(backend, victimConfig());
        makeTableWithRefs(*victim, "victim/db/t1", 2, 0);
    }
    ASSERT_TRUE(decommissionPoolMember(backend, PoolConfig{.pool_prefix = "p", .server_root_id = "admin"}, "victim").member_retired);
    gcUntilVictimSubtreeEmpty(backend);
    const Layout layout("p");
    const OwnerObject retired = readOwner(*backend, layout, "victim");
    ASSERT_TRUE(retired.retirement_verified);
    const uint64_t admin_epoch = readSlot(*backend, layout, "victim").writer_epoch;

    auto rejoined = Pool::open(backend, victimConfig());

    EXPECT_GT(rejoined->writerEpoch(), admin_epoch) << "the kept counter continues past every earlier life";
    const OwnerObject owner = readOwner(*backend, layout, "victim");
    EXPECT_EQ(owner.owner_life, retired.owner_life + 1);
    EXPECT_FALSE(owner.retired_at_ms.has_value());
    EXPECT_FALSE(owner.retirement_verified);
    EXPECT_TRUE(victimSubtreeEmpty(*backend)) << "the member comes back empty";
    EXPECT_EQ(readSlot(*backend, layout, "victim").writer_epoch, rejoined->writerEpoch());
    expectWriteSurvivesGc(backend, rejoined, victim_uuid);
}

TEST(CASRejoin, NewUuidReclaimsTheOldLifesFarewell)
{
    auto backend = std::make_shared<InMemoryBackend>();
    { auto victim = Pool::open(backend, victimConfig()); }
    retireVictim(backend, "admin");
    const Layout layout("p");
    const MountLease farewell = readSlot(*backend, layout, "victim");

    auto rejoined = Pool::open(backend, victimConfig(UInt128(77)));

    const OwnerObject owner = readOwner(*backend, layout, "victim");
    EXPECT_EQ(owner.server_uuid, UInt128(77));
    EXPECT_FALSE(owner.retired_at_ms.has_value());
    const MountLease slot = readSlot(*backend, layout, "victim");
    EXPECT_EQ(slot.server_uuid, UInt128(77));
    EXPECT_GT(slot.writer_epoch, farewell.writer_epoch);
    EXPECT_EQ(slot.writer_epoch, rejoined->writerEpoch());
    expectWriteSurvivesGc(backend, rejoined, UInt128(77));
}

/// The old victim reclaimed the admin's farewell under a fresh epoch, saw the tombstone and fenced itself.
TEST(CASRejoin, NewUuidReclaimsASelfFencedOldVictimBody)
{
    auto backend = std::make_shared<InMemoryBackend>();
    { auto victim = Pool::open(backend, victimConfig()); }
    retireVictim(backend, "admin");
    const Layout layout("p");
    uint64_t victim_epoch = 0;
    {
        OperationForTest op(*backend);
        victim_epoch = allocateWriterEpoch(*op, layout, "victim", [] { return RefCatalog{}; });
    }
    rewriteSlot(*backend, layout, [&](MountLease & body)
    {
        body.writer_epoch = victim_epoch;
        body.min_active_build_sequence = 0;
        body.gc_fenced = true;
    });

    auto rejoined = Pool::open(backend, victimConfig(UInt128(77)));

    const MountLease slot = readSlot(*backend, layout, "victim");
    EXPECT_EQ(slot.server_uuid, UInt128(77));
    EXPECT_EQ(slot.writer_epoch, rejoined->writerEpoch());
    EXPECT_FALSE(slot.gc_fenced);
}

TEST(CASRejoin, RefusesWhileTheDecommissionRuns)
{
    auto backend = std::make_shared<InMemoryBackend>();
    { auto victim = Pool::open(backend, victimConfig()); }
    retireVictim(backend, "admin");
    const Layout layout("p");
    auto rerun_admin = Pool::openForDecommission(backend, PoolConfig{.pool_prefix = "p", .server_root_id = "admin2"}, "victim");
    const OwnerObject before = readOwner(*backend, layout, "victim");

    for (const UInt128 uuid : {victim_uuid, UInt128(77)})
    {
        SCOPED_TRACE(u128ToHex(uuid));
        expectThrowsCodeWithMessage(ErrorCodes::ABORTED, "still running", [&] { Pool::open(backend, victimConfig(uuid)); });
    }

    const OwnerObject after = readOwner(*backend, layout, "victim");
    EXPECT_EQ(after.owner_life, before.owner_life);
    EXPECT_TRUE(after.retirement_verified);
    EXPECT_EQ(readSlot(*backend, layout, "victim").writer_epoch, rerun_admin->writerEpoch()) << "the admin's live body is untouched";
}

/// Each part of the old life that can remain refuses the rejoin, names the part and writes nothing; once the
/// part is gone the start rejoins. The verified mark does not wait for GC; the rejoin does.
TEST(CASRejoin, RefusesWhileDataOfTheOldLifeRemains)
{
    const Layout layout("p");
    const auto control_objects = [&](Backend & backend)
    {
        OperationForTest op(backend);
        std::vector<Etag> etags;
        for (const String & key : {layout.ownerKey("victim"), layout.epochKey("victim"), layout.mountKey("victim")})
            etags.push_back((*op).read(key, Retry::once())->etag);
        return etags;
    };
    for (const ServerRootLeftover leftover :
         {ServerRootLeftover::CatalogRows, ServerRootLeftover::Manifests, ServerRootLeftover::RootsObjects})
    {
        SCOPED_TRACE(static_cast<int>(leftover));
        auto backend = std::make_shared<InMemoryBackend>();
        OperationForTest op(*backend);
        /// The server uuid as `IDisk::checkAccess` formats it for the probe key and the server passes as its id.
        const String old_uuid = toString(parseFromString<UUID>("01234567-89ab-cdef-fedc-ba9876543210"));
        String old_uuid_hex = old_uuid;
        std::erase(old_uuid_hex, '-');
        const String probe = "p/roots/victim/clickhouse_access_check_" + old_uuid;
        {
            auto victim = Pool::open(backend, victimConfig(hexToU128(old_uuid_hex)));
            if (leftover == ServerRootLeftover::CatalogRows)
                makeTableWithRefs(*victim, "victim/db/t1", 1, 0);
        }
        const auto report = decommissionPoolMember(backend, PoolConfig{.pool_prefix = "p", .server_root_id = "admin"}, "victim");
        ASSERT_TRUE(report.member_retired);
        ASSERT_EQ(report.namespaces_pending_gc, leftover == ServerRootLeftover::CatalogRows ? 1u : 0u);

        String part;
        std::function<void()> clear;
        switch (leftover)
        {
            case ServerRootLeftover::CatalogRows:
                part = "catalog rows of the old life";
                clear = [&] { gcUntilVictimSubtreeEmpty(backend); };
                break;
            case ServerRootLeftover::Manifests:
                ASSERT_TRUE(std::holds_alternative<Committed>((*op).create("p/cas/manifests/victim/m", "m", Retry::once())));
                part = "manifests under cas/manifests/victim/";
                clear = [&] { ASSERT_EQ((*op).removeCurrent("p/cas/manifests/victim/m", Retry::once()), Removal::Removed); };
                break;
            case ServerRootLeftover::RootsObjects:
                ASSERT_TRUE(std::holds_alternative<Committed>((*op).create(probe, "test", Retry::once())));
                ASSERT_TRUE(std::holds_alternative<Committed>((*op).create("p/roots/victim/backup.zip", "zip", Retry::once())));
                part = "an object under roots/victim/";
                clear = [&] { ASSERT_EQ((*op).removeCurrent("p/roots/victim/backup.zip", Retry::once()), Removal::Removed); };
                break;
            case ServerRootLeftover::None:
                FAIL();
        }
        const std::vector<Etag> before = control_objects(*backend);

        try
        {
            Pool::open(backend, victimConfig(UInt128(77)));
            ADD_FAILURE() << "the rejoin was not refused";
        }
        catch (const DB::Exception & e)
        {
            EXPECT_EQ(e.code(), ErrorCodes::ABORTED) << e.message();
            EXPECT_NE(e.message().find(part), String::npos) << e.message();
            if (leftover != ServerRootLeftover::RootsObjects)
                EXPECT_NE(e.message().find("on the GC leader"), String::npos) << e.message();
        }

        EXPECT_EQ(control_objects(*backend), before);
        if (leftover == ServerRootLeftover::RootsObjects)
            EXPECT_FALSE((*op).head(probe, Retry::once()).has_value()) << "the old life's access probe is deleted first";
        clear();
        EXPECT_NO_THROW(Pool::open(backend, victimConfig(UInt128(77))));
    }
}

/// A rerun over a verified retirement died holding the slot. The rejoin waits until GC fences the lapsed lease.
TEST(CASRejoin, RejoinsOnceGcFencesAKilledRerunsLease)
{
    for (const UInt128 uuid : {victim_uuid, UInt128(77)})
    {
        SCOPED_TRACE(u128ToHex(uuid));
        auto backend = std::make_shared<InMemoryBackend>();
        { auto victim = Pool::open(backend, victimConfig()); }
        retireVictim(backend, "admin");
        const Layout layout("p");
        OperationForTest op(*backend);
        const uint64_t rerun_epoch = allocateWriterEpoch(*op, layout, "victim", [] { return RefCatalog{}; });
        rewriteSlot(*backend, layout, [&](MountLease & body)
        {
            body.writer_epoch = rerun_epoch;
            body.min_active_build_sequence = 0;
        });
        tombstoneOwnerForTest(*backend, layout.ownerKey("victim"));
        ASSERT_TRUE(readOwner(*backend, layout, "victim").retirement_verified);

        expectThrowsCodeWithMessage(ErrorCodes::ABORTED, "still running", [&] { Pool::open(backend, victimConfig(uuid)); });

        MountObservationMap observations;
        std::set<String> warned;
        for (const uint64_t mono_ms : {uint64_t{0}, uint64_t{1'000'000'000}})
            computeHeartbeatFloor(*op, layout, /*now_ms=*/0, [mono_ms] { return mono_ms; }, /*observer_lease_ms=*/30'000,
                                  /*cadence_ms=*/1'000, observations, warned);
        ASSERT_TRUE(readSlot(*backend, layout, "victim").gc_fenced);

        auto rejoined = Pool::open(backend, victimConfig(uuid));
        EXPECT_EQ(readOwner(*backend, layout, "victim").server_uuid, uuid);
        EXPECT_TRUE(readSlot(*backend, layout, "victim").isHeldBy(uuid, rejoined->writerEpoch()));
    }
}

TEST(CASRejoin, RefusesSlotRetiredBy2664)
{
    const Layout layout("p");
    {
        SCOPED_TRACE("26.6.4 end state: tombstone without life or mark, no epoch, no mount");
        auto backend = std::make_shared<InMemoryBackend>();
        { auto other = Pool::open(backend, PoolConfig{.pool_prefix = "p", .server_root_id = "other"}); }
        OperationForTest op(*backend);
        const String tombstone = encodeOwner(OwnerObject{.server_uuid = UInt128{}, .retired_at_ms = 1});
        ASSERT_TRUE(std::holds_alternative<Committed>((*op).create(layout.ownerKey("victim"), tombstone, Retry::once())));
        try
        {
            Pool::open(backend, victimConfig());
            ADD_FAILURE() << "the start was not refused";
        }
        catch (const DB::Exception & e)
        {
            EXPECT_EQ(e.code(), ErrorCodes::CORRUPTED_DATA) << e.message();
            EXPECT_NE(e.message().find("retired by an older version"), String::npos) << e.message();
            EXPECT_EQ(e.message().find("not finished"), String::npos) << "a rerun refuses the absent counter: " << e.message();
        }
        EXPECT_EQ((*op).read(layout.ownerKey("victim"), Retry::once())->bytes, tombstone);
        EXPECT_FALSE((*op).head(layout.epochKey("victim"), Retry::once()).has_value());
        EXPECT_FALSE((*op).head(layout.mountKey("victim"), Retry::once()).has_value());
    }
    {
        SCOPED_TRACE("verified retirement whose epoch counter was deleted by hand");
        auto backend = std::make_shared<InMemoryBackend>();
        { auto victim = Pool::open(backend, victimConfig()); }
        retireVictim(backend, "admin");
        OperationForTest op(*backend);
        ASSERT_EQ((*op).removeCurrent(layout.epochKey("victim"), Retry::once()), Removal::Removed);
        expectThrowsCodeWithMessage(ErrorCodes::CORRUPTED_DATA, "cannot rejoin", [&] { Pool::open(backend, victimConfig()); });
        EXPECT_FALSE((*op).head(layout.epochKey("victim"), Retry::once()).has_value());
        EXPECT_TRUE(readOwner(*backend, layout, "victim").retired_at_ms.has_value());
    }
    {
        SCOPED_TRACE("verified retirement whose mount slot was deleted by hand");
        auto backend = std::make_shared<InMemoryBackend>();
        { auto victim = Pool::open(backend, victimConfig()); }
        retireVictim(backend, "admin");
        OperationForTest op(*backend);
        ASSERT_EQ((*op).removeCurrent(layout.mountKey("victim"), Retry::once()), Removal::Removed);
        expectThrowsCodeWithMessage(ErrorCodes::CORRUPTED_DATA, "cannot rejoin", [&] { Pool::open(backend, victimConfig()); });
        EXPECT_FALSE((*op).head(layout.mountKey("victim"), Retry::once()).has_value());
        EXPECT_TRUE(readOwner(*backend, layout, "victim").retired_at_ms.has_value());
    }
}

/// Starter 88 rewrote the slot and stalled before its owner write; starter 77 then writes its owner and
/// takes the loser's terminal body. A later start of 88 is refused and writes nothing.
TEST(CASRejoin, ConcurrentStartersWithDifferentUuidsOneWins)
{
    auto backend = std::make_shared<InMemoryBackend>();
    { auto victim = Pool::open(backend, victimConfig()); }
    retireVictim(backend, "admin");
    const Layout layout("p");
    bool loser_wrote_mount = false;
    backend->onBeforeWrite(layout.ownerKey("victim"), [&]
    {
        if (std::exchange(loser_wrote_mount, true))
            return;
        rewriteSlot(*backend, layout, [](MountLease & body) { body.server_uuid = UInt128(88); });
    });

    auto winner = Pool::open(backend, victimConfig(UInt128(77)));

    ASSERT_TRUE(loser_wrote_mount);
    EXPECT_EQ(readOwner(*backend, layout, "victim").server_uuid, UInt128(77));
    EXPECT_EQ(readSlot(*backend, layout, "victim").server_uuid, UInt128(77));
    expectThrowsCodeWithMessage(ErrorCodes::CORRUPTED_DATA, "different server", [&] { Pool::open(backend, victimConfig(UInt128(88))); });
    EXPECT_EQ(readSlot(*backend, layout, "victim").writer_epoch, winner->writerEpoch());
}

/// Starter 88's owner write loses its `If-Match` to starter 77's owner write: it decides again from a fresh
/// owner read, sees the foreign owner and refuses. It has already rewritten the slot, as every new uuid does
/// before its owner write, and leaves the owner alone.
TEST(CASRejoin, LostOwnerWriteDecidesAgainAndRefuses)
{
    auto backend = std::make_shared<InMemoryBackend>();
    { auto victim = Pool::open(backend, victimConfig()); }
    retireVictim(backend, "admin");
    const Layout layout("p");
    const OwnerObject retired = readOwner(*backend, layout, "victim");
    const MountLease slot_before = readSlot(*backend, layout, "victim");
    const uint64_t slot_seq = slot_before.seq;
    bool winner_wrote = false;
    String winner_body;
    backend->onBeforeWrite(layout.ownerKey("victim"), [&]
    {
        if (std::exchange(winner_wrote, true))
            return;
        OperationForTest op(*backend);
        const auto current = (*op).read(layout.ownerKey("victim"), Retry::once());
        winner_body = encodeOwner(OwnerObject{.server_uuid = UInt128(77), .retired_at_ms = std::nullopt, .owner_life = retired.owner_life + 1});
        ASSERT_TRUE(std::holds_alternative<Committed>((*op).replace(layout.ownerKey("victim"), winner_body, current->etag, Retry::once())));
    });

    expectThrowsCodeWithMessage(ErrorCodes::CORRUPTED_DATA, "different server", [&] { Pool::open(backend, victimConfig(UInt128(88))); });

    ASSERT_TRUE(winner_wrote);
    OperationForTest op(*backend);
    EXPECT_EQ((*op).read(layout.ownerKey("victim"), Retry::once())->bytes, winner_body) << "the loser left the owner alone";
    const MountLease slot = readSlot(*backend, layout, "victim");
    EXPECT_EQ(slot.seq, slot_seq + 1);
    EXPECT_EQ(slot.server_uuid, UInt128(88));
    EXPECT_NE(slot.write_attempt_id, slot_before.write_attempt_id);
    EXPECT_EQ(slot.pid, static_cast<uint64_t>(::getpid()));
}

/// The old uuid rejoins after a new uuid replaced the mount body, and the new uuid writes the owner first. The old
/// uuid writes no body of its own: under the new owner it would be a farewell a 26.6.4 starter of the old uuid,
/// prechecked before the decommission, could still reclaim.
TEST(CASRejoin, SameUuidLeavesTheMountBodyAlone)
{
    auto backend = std::make_shared<InMemoryBackend>();
    { auto victim = Pool::open(backend, victimConfig()); }
    retireVictim(backend, "admin");
    const Layout layout("p");
    const OwnerObject retired = readOwner(*backend, layout, "victim");
    rewriteSlot(*backend, layout, [](MountLease & body) { body.server_uuid = UInt128(77); });
    const MountLease replaced = readSlot(*backend, layout, "victim");
    std::atomic<bool> fired = false;
    backend->onBeforeWrite(layout.ownerKey("victim"), [&]
    {
        if (fired.exchange(true))
            return;
        OperationForTest op(*backend);
        const auto current = (*op).read(layout.ownerKey("victim"), Retry::once());
        const String body = encodeOwner(
            OwnerObject{.server_uuid = UInt128(77), .retired_at_ms = std::nullopt, .owner_life = retired.owner_life + 1});
        ASSERT_TRUE(std::holds_alternative<Committed>(
            (*op).replace(layout.ownerKey("victim"), body, current->etag, Retry::once())));
    });

    expectThrowsCodeWithMessage(ErrorCodes::CORRUPTED_DATA, "different server", [&] { Pool::open(backend, victimConfig()); });

    backend->onBeforeWrite(layout.ownerKey("victim"), {});
    ASSERT_TRUE(fired);
    const MountLease slot = readSlot(*backend, layout, "victim");
    EXPECT_EQ(slot.server_uuid, UInt128(77)) << "the old uuid wrote no body under the new owner";
    EXPECT_EQ(slot.seq, replaced.seq);
}

/// Every rejoin write loses its `If-Match`: the start gives up after a bounded number of decisions.
TEST(CASRejoin, OwnerChangingOnEveryDecisionIsBounded)
{
    auto backend = std::make_shared<InMemoryBackend>();
    { auto victim = Pool::open(backend, victimConfig()); }
    retireVictim(backend, "admin");
    const Layout layout("p");
    bool in_hook = false;
    size_t fires = 0;
    backend->onBeforeWrite(layout.ownerKey("victim"), [&]
    {
        if (std::exchange(in_hook, true))
            return;
        ++fires;
        OperationForTest op(*backend);
        const auto current = (*op).read(layout.ownerKey("victim"), Retry::once());
        ASSERT_TRUE(std::holds_alternative<Committed>((*op).replace(layout.ownerKey("victim"), current->bytes, current->etag, Retry::once())));
        in_hook = false;
    });

    expectThrowsCodeWithMessage(ErrorCodes::ABORTED, "kept changing", [&] { Pool::open(backend, victimConfig(UInt128(77))); });

    EXPECT_EQ(fires, 4u);
    EXPECT_TRUE(readOwner(*backend, layout, "victim").retired_at_ms.has_value());
}

/// A start of the old life allocated its epoch before the decommission and claims only after a rejoin.
TEST(CASRejoin, DelayedLowerEpochClaimAfterRejoinIsRefused)
{
    for (const UInt128 rejoiner : {victim_uuid, UInt128(77)})
    {
        SCOPED_TRACE(rejoiner == victim_uuid ? "same uuid" : "new uuid: a foreign farewell");
        auto backend = std::make_shared<InMemoryBackend>();
        { auto victim = Pool::open(backend, victimConfig()); }
        const Layout layout("p");
        uint64_t delayed_epoch = 0;
        {
            OperationForTest op(*backend);
            delayed_epoch = allocateWriterEpoch(*op, layout, "victim", [] { return RefCatalog{}; });
        }
        retireVictim(backend, "admin");
        { auto rejoined = Pool::open(backend, victimConfig(rejoiner)); }
        OperationForTest op(*backend);
        const auto before = (*op).read(layout.mountKey("victim"), Retry::once());
        const OwnerExpectation old_life{.server_uuid = victim_uuid, .owner_life = 1, .retired = RetiredOwnerPolicy::Refuse};
        if (rejoiner == victim_uuid)
            expectThrowsCode(ErrorCodes::ABORTED, [&] { claimMount(*op, layout, "victim", victim_uuid, delayed_epoch, 1, 100, {}, {}, {}, old_life); });
        else
            EXPECT_EQ(claimMount(*op, layout, "victim", victim_uuid, delayed_epoch, 1, 100, {}, {}, {}, old_life).kind, MountClaimResult::ForeignOwner);
        EXPECT_EQ((*op).read(layout.mountKey("victim"), Retry::once())->etag, before->etag);
    }
}

/// The victim restarts while the admin holds the slot; it reclaims the farewell, sees the tombstone and
/// fences itself. A completed rerun later, the same victim rejoins on its next start.
TEST(CASRejoin, VictimRefusedAfterFarewellRejoinsOnItsNextStart)
{
    auto backend = std::make_shared<InMemoryBackend>();
    { auto victim = Pool::open(backend, victimConfig()); }
    const Layout layout("p");
    auto admin = Pool::openForDecommission(backend, PoolConfig{.pool_prefix = "p", .server_root_id = "admin"}, "victim");
    auto fake_boot = std::make_shared<std::atomic<uint64_t>>(0);
    bool decommissioned = false;
    PoolConfig waiting = victimConfig();
    waiting.boot_ms_fn = [fake_boot] { return fake_boot->load(); };
    waiting.wait_sleep_fn = [&, fake_boot](uint64_t ms)
    {
        *fake_boot += ms;
        if (std::exchange(decommissioned, true))
            return;
        tombstoneOwnerForTest(*backend, layout.ownerKey("victim"));
        admin.reset();
    };
    expectThrowsCodeWithMessage(ErrorCodes::CORRUPTED_DATA, "decommissioned", [&] { Pool::open(backend, waiting); });
    ASSERT_TRUE(readSlot(*backend, layout, "victim").gc_fenced);

    retireVictim(backend, "admin2");
    auto rejoined = Pool::open(backend, victimConfig());
    EXPECT_FALSE(readOwner(*backend, layout, "victim").retired_at_ms.has_value());
    EXPECT_EQ(readSlot(*backend, layout, "victim").writer_epoch, rejoined->writerEpoch());
}

TEST(CASRejoin, RefusesWhileAnUnfinishedDecommissionLeftData)
{
    auto backend = std::make_shared<FailingDeleteBackend>();
    { auto victim = Pool::open(backend, victimConfig()); }
    {
        OperationForTest op(*backend);
        ASSERT_TRUE(std::holds_alternative<Committed>((*op).create("p/roots/victim/kept", "x", Retry::once())));
    }
    backend->failWithTokenMismatch("p/roots/victim/kept");
    ASSERT_FALSE(decommissionPoolMember(backend, PoolConfig{.pool_prefix = "p", .server_root_id = "admin"}, "victim").member_retired);
    const Layout layout("p");
    const OwnerObject tombstone = readOwner(*backend, layout, "victim");
    ASSERT_TRUE(tombstone.retired_at_ms.has_value());
    ASSERT_FALSE(tombstone.retirement_verified);

    for (const UInt128 uuid : {victim_uuid, UInt128(77)})
    {
        SCOPED_TRACE(u128ToHex(uuid));
        expectThrowsCodeWithMessage(ErrorCodes::CORRUPTED_DATA, "decommission is not finished", [&] { Pool::open(backend, victimConfig(uuid)); });
    }
    EXPECT_EQ(readOwner(*backend, layout, "victim").owner_life, tombstone.owner_life);

    backend->disarm();
    retireVictim(backend, "admin2");
    EXPECT_NO_THROW(Pool::open(backend, victimConfig()));
}

/// A losing racing rejoiner of uuid 88 left its terminal body; the old uuid's own restart still rejoins.
TEST(CASRejoin, SameUuidRejoinsOverAForeignTerminalBody)
{
    auto backend = std::make_shared<InMemoryBackend>();
    { auto victim = Pool::open(backend, victimConfig()); }
    retireVictim(backend, "admin");
    const Layout layout("p");
    rewriteSlot(*backend, layout, [](MountLease & body) { body.server_uuid = UInt128(88); });

    auto rejoined = Pool::open(backend, victimConfig());

    const MountLease slot = readSlot(*backend, layout, "victim");
    EXPECT_EQ(slot.server_uuid, victim_uuid);
    EXPECT_EQ(slot.writer_epoch, rejoined->writerEpoch());
}

/// Only a start rejoins. A process that was fenced and finds its id retired keeps refusing to remount.
TEST(CASRejoin, RemountDoesNotRejoin)
{
    auto backend = std::make_shared<InMemoryBackend>();
    auto store = Pool::open(backend, victimConfig());
    const Layout layout("p");
    tombstoneOwnerForTest(*backend, layout.ownerKey("victim"), /*verified=*/true);
    fenceOutMountForTest(*backend, layout.mountKey("victim"));
    expectThrowsCode(ErrorCodes::ABORTED, [&] { store->renewWatermarkOnce(); });
    const OwnerObject before = readOwner(*backend, layout, "victim");

    EXPECT_FALSE(store->tryRemountOnce());

    const OwnerObject after = readOwner(*backend, layout, "victim");
    EXPECT_EQ(after.owner_life, before.owner_life);
    EXPECT_TRUE(after.retired_at_ms.has_value());
}


/// A 26.6.4 starter of the old uuid read the farewell before the rejoin and reclaims it by `If-Match` with
/// no owner recheck; it fires just before the rejoin's owner write.
TEST(CASRejoin, NewUuidReplacesTheMountBeforeTheOwner)
{
    auto backend = std::make_shared<InMemoryBackend>();
    { auto victim = Pool::open(backend, victimConfig()); }
    retireVictim(backend, "admin");
    const Layout layout("p");
    OperationForTest raw_op(*backend);
    const auto old_farewell = (*raw_op).read(layout.mountKey("victim"), Retry::once());
    ASSERT_TRUE(old_farewell.has_value());
    std::optional<WriteResult> old_claim;
    backend->onBeforeWrite(layout.ownerKey("victim"), [&]
    {
        if (old_claim)
            return;
        MountLease body = decodeMountLease(old_farewell->bytes);
        body.writer_epoch += 1;
        body.min_active_build_sequence = 0;
        body.seq += 1;
        OperationForTest op(*backend);
        old_claim = (*op).replace(layout.mountKey("victim"), encodeMountLease(body), old_farewell->etag, Retry::once());
    });

    auto rejoined = Pool::open(backend, victimConfig(UInt128(77)));

    ASSERT_TRUE(old_claim.has_value());
    EXPECT_TRUE(std::holds_alternative<Conflict>(*old_claim)) << "the rejoin rewrote the mount body first";
    EXPECT_EQ(readSlot(*backend, layout, "victim").server_uuid, UInt128(77));
}

/// A decommission rerun claims the slot between the rejoin's reads and its first write: the rejoin's
/// `If-Match` fails, the next decision sees the admin's live body and refuses; the owner stays retired.
TEST(CASRejoin, DecommissionRerunDuringARejoinWins)
{
    auto backend = std::make_shared<InMemoryBackend>();
    { auto victim = Pool::open(backend, victimConfig()); }
    retireVictim(backend, "admin");
    const Layout layout("p");
    const OwnerObject before = readOwner(*backend, layout, "victim");
    std::atomic<bool> fired = false;
    PoolPtr rerun_admin;
    const auto admin_claims = [&]
    {
        if (fired.exchange(true))
            return;
        rerun_admin = Pool::openForDecommission(backend, PoolConfig{.pool_prefix = "p", .server_root_id = "admin2"}, "victim");
    };
    backend->onBeforeWrite(layout.mountKey("victim"), admin_claims);
    backend->onBeforeWrite(layout.ownerKey("victim"), admin_claims);

    expectThrowsCodeWithMessage(ErrorCodes::ABORTED, "still running", [&] { Pool::open(backend, victimConfig(UInt128(77))); });

    backend->onBeforeWrite(layout.mountKey("victim"), {});
    backend->onBeforeWrite(layout.ownerKey("victim"), {});
    ASSERT_TRUE(rerun_admin != nullptr);
    const OwnerObject after = readOwner(*backend, layout, "victim");
    EXPECT_EQ(after.server_uuid, before.server_uuid);
    EXPECT_EQ(after.owner_life, before.owner_life);
    EXPECT_TRUE(after.retirement_verified);
    EXPECT_EQ(readSlot(*backend, layout, "victim").writer_epoch, rerun_admin->writerEpoch());
}

/// A writable open without a server uuid (uuid 0) stays refused over a retired owner, whoever owned it.
TEST(CASRejoin, ZeroUuidDoesNotRejoin)
{
    for (const UInt128 old_uuid : {UInt128(77), UInt128{}})
    {
        SCOPED_TRACE(u128ToHex(old_uuid));
        auto backend = std::make_shared<InMemoryBackend>();
        { auto victim = Pool::open(backend, victimConfig(old_uuid)); }
        retireVictim(backend, "admin");
        const Layout layout("p");
        const OwnerObject before = readOwner(*backend, layout, "victim");
        const MountLease slot_before = readSlot(*backend, layout, "victim");

        expectThrowsCodeWithMessage(
            ErrorCodes::CORRUPTED_DATA, "no server uuid",
            [&] { Pool::open(backend, victimConfig(UInt128{})); });

        const OwnerObject after = readOwner(*backend, layout, "victim");
        EXPECT_EQ(after.owner_life, before.owner_life);
        EXPECT_TRUE(after.retired_at_ms.has_value());
        EXPECT_EQ(readSlot(*backend, layout, "victim").seq, slot_before.seq);
    }
}
