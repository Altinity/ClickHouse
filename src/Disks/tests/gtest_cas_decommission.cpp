#include "cas_test_helpers.h"
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasDecommission.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasServerRoot.h>
#include <stdexcept>

using namespace DB;
using namespace DB::Cas;
using namespace DB::Cas::tests;

namespace
{

/// Open a store for the VICTIM srid over `backend` (the pool's future dead member).
StorePtr openVictim(std::shared_ptr<InMemoryBackend> backend)
{
    return Store::open(backend, PoolConfig{.pool_prefix = "p", .server_root_id = "victim"});
}

/// Fails `deleteExact` for one or two designated keys -- either by throwing (a transient backend
/// hiccup) or by returning a synthetic `TokenMismatch` (a "listed but raced" outcome) -- delegating
/// every other key to the base `InMemoryBackend` untouched. Drives the drain phases' per-object
/// fail-close path (`deleteListedPrefix`/`sweepNamespace`, `CasDecommission.cpp`/
/// `CasOrphanManifestSweep.cpp`): a failure on one listed object must record a warning and let the rest
/// of the sweep proceed, never abort the whole phase.
///
/// Also fails any `get`/`list` whose key/prefix CONTAINS a designated namespace substring -- models a
/// fully unreadable protection view (a corrupt snapshot / unavailable ref range) for exactly one
/// namespace, without touching anything else. `victim/db2` in `ManifestDebrisPhaseFailuresWarnAndContinue`
/// below carries NO ref objects of its own, so this cannot also break Task 2's namespace-erasure loop
/// (`listNamespaces` never discovers it) -- it is reachable only from `sweepNamespace`'s
/// `activeManifestKeys` call in the manifest-debris drain.
class FailingDeleteBackend : public InMemoryBackend
{
public:
    void failWithThrow(const String & key) { throw_key = key; }
    void failWithTokenMismatch(const String & key) { mismatch_key = key; }
    void failNamespaceReads(const String & ns_substring) { unreadable_ns_substring = ns_substring; }

    DeleteOutcome deleteExact(const String & key, const Token & token) override
    {
        if (key == throw_key)
            throw std::runtime_error("injected transient delete failure for " + key);
        if (key == mismatch_key)
            return DeleteOutcome{.kind = DeleteOutcome::Kind::TokenMismatch};
        return InMemoryBackend::deleteExact(key, token);
    }

    std::optional<GetResult> get(const String & key, Range range = {}) override
    {
        maybeFailUnreadable(key);
        return InMemoryBackend::get(key, range);
    }

    ListPage list(const String & prefix, const String & cursor, size_t limit) override
    {
        maybeFailUnreadable(prefix);
        return InMemoryBackend::list(prefix, cursor, limit);
    }

private:
    void maybeFailUnreadable(const String & key) const
    {
        if (!unreadable_ns_substring.empty() && key.find(unreadable_ns_substring) != String::npos)
            throw Exception(ErrorCodes::LOGICAL_ERROR, "injected unreadable protection view for {}", key);
    }

    String throw_key;
    String mismatch_key;
    String unreadable_ns_substring;
};

/// Seed one victim table with `committed` committed refs and `precommits` dangling precommit bindings,
/// via the raw ref-log seeding helpers (fixture idiom of e.g. `gtest_cas_gc_fold.cpp`: `writeManifestRaw`
/// + `publishCommittedTransition`/`addPrecommitTransition` against `victim`'s own backend/layout) -- this
/// fixture only needs the ref-table SHAPE `dropNamespace` erases, not a real build. Precommit bindings
/// are seeded at an artificially high `writer_epoch` so the writer's own stale-precommit sweep (armed
/// unconditionally by this table's recovery, unrelated to decommission -- spec §Clean Up Old Precommits)
/// never reclaims them, in its OWN separate transaction, ahead of `dropNamespace`'s removal.
void makeTableWithRefs(Store & victim, const String & ns_str, uint64_t committed, uint64_t precommits)
{
    const RootNamespace ns(ns_str);
    Backend & backend = victim.backend();
    const Layout & layout = victim.layout();

    for (uint64_t i = 0; i < committed; ++i)
    {
        const ManifestRef ref{.writer_epoch = 1, .build_sequence = i + 1, .manifest_ordinal = 1};
        writeManifestRaw(backend, layout, ns, ref, {});
        publishCommittedTransition(backend, layout, ns, "committed_" + std::to_string(i), std::nullopt, ref);
    }
    for (uint64_t i = 0; i < precommits; ++i)
    {
        const ManifestRef ref{.writer_epoch = 999999, .build_sequence = i + 1, .manifest_ordinal = 1};
        writeManifestRaw(backend, layout, ns, ref, {});
        addPrecommitTransition(backend, layout, ns, UInt128(1), "precommit_" + std::to_string(i), std::nullopt, ref);
    }

    /// Self-checking: `listRefs` must observe exactly `committed` committed refs before returning.
    ASSERT_EQ(victim.listRefs(ns).size(), committed);
}

/// Pre-precommit manifest debris: a staged manifest body under `ns_str`, at the store's own
/// `writer_epoch`, named by NO owner event -- a build the writer staged and never finished (fixture
/// idiom of `gtest_cas_orphan_manifest_sweep.cpp`'s `EligibleAndUnownedIsDeleted`). `build_sequence = 99`
/// is picked well clear of `makeTableWithRefs`'s own committed/precommit build sequences so it can never
/// collide with a real owned manifest key. Returns the seeded body's `ManifestId` so a caller can target
/// it (e.g. its exact object key) for further fixture setup.
ManifestId seedOrphanManifestBody(Store & victim, const String & ns_str)
{
    const RootNamespace ns(ns_str);
    const ManifestRef ref{.writer_epoch = victim.writerEpoch(), .build_sequence = 99, .manifest_ordinal = 1};
    const ManifestId id = writeManifestRaw(victim.backend(), victim.layout(), ns, ref, {});
    /// EXPECT, not ASSERT: this function returns a value now, and ASSERT_* expands to a bare `return;`
    /// -- invalid in a non-void function.
    EXPECT_TRUE(victim.backend().head(victim.layout().manifestKey(id)).exists);
    return id;
}

}

TEST(CasDecommission, RefusesLiveMember)
{
    auto backend = std::make_shared<InMemoryBackend>();
    auto victim = openVictim(backend);   /// keeps its mount lease unexpired — the member is alive

    expectThrowsCode(ErrorCodes::ABORTED, [&]
    {
        Store::openForDecommission(backend, PoolConfig{.pool_prefix = "p", .server_root_id = "admin"}, "victim");
    });
}

TEST(CasDecommission, ClaimsDeadMemberAndBumpsEpoch)
{
    auto backend = std::make_shared<InMemoryBackend>();
    uint64_t victim_epoch = 0;
    {
        auto victim = openVictim(backend);
        victim_epoch = victim->writerEpoch();
    }   /// graceful close: lease stamped already-expired + farewell — the slot is claimable

    auto admin = Store::openForDecommission(backend, PoolConfig{.pool_prefix = "p", .server_root_id = "admin"}, "victim");
    ASSERT_TRUE(admin != nullptr);
    EXPECT_GT(admin->writerEpoch(), victim_epoch);
    /// The admin store IS the victim server root now (impersonation).
    EXPECT_EQ(admin->poolConfig().server_root_id, "victim");
}

TEST(CasDecommission, RefusesUnknownMember)
{
    auto backend = std::make_shared<InMemoryBackend>();
    expectThrowsCode(ErrorCodes::BAD_ARGUMENTS, [&]
    {
        Store::openForDecommission(backend, PoolConfig{.pool_prefix = "p", .server_root_id = "admin"}, "never_existed");
    });
}

TEST(CasDecommission, SecondConcurrentDecommissionRefused)
{
    auto backend = std::make_shared<InMemoryBackend>();
    { auto victim = openVictim(backend); }

    auto first = Store::openForDecommission(backend, PoolConfig{.pool_prefix = "p", .server_root_id = "admin"}, "victim");
    expectThrowsCode(ErrorCodes::ABORTED, [&]
    {
        Store::openForDecommission(backend, PoolConfig{.pool_prefix = "p", .server_root_id = "admin2"}, "victim");
    });
}

TEST(CasDecommission, ErasesAllVictimNamespaces)
{
    auto backend = std::make_shared<InMemoryBackend>();
    {
        auto victim = openVictim(backend);
        /// Two tables: ns "victim/db/t1" with 2 committed refs, ns "victim/db/t2" with 1 committed
        /// ref + 1 stale precommit (fixture idiom of gtest_cas_ref_writer.cpp).
        makeTableWithRefs(*victim, "victim/db/t1", /*committed=*/2, /*precommits=*/0);
        makeTableWithRefs(*victim, "victim/db/t2", /*committed=*/1, /*precommits=*/1);
    }

    const auto report = decommissionPoolMember(
        backend, PoolConfig{.pool_prefix = "p", .server_root_id = "admin"}, "victim");

    EXPECT_EQ(report.srid, "victim");
    EXPECT_EQ(report.namespaces_removed, 2u);
    EXPECT_EQ(report.namespaces_already_removed, 0u);
    EXPECT_EQ(report.committed_refs_removed, 3u);
    EXPECT_EQ(report.precommits_removed, 1u);
    EXPECT_EQ(report.edge_deltas_emitted, 4u);

    /// The namespaces are durably Removed — visible to a fresh admin store.
    auto check = Store::openForDecommission(backend, PoolConfig{.pool_prefix = "p", .server_root_id = "chk"}, "victim");
    EXPECT_TRUE(check->namespaceIsRemoved(RootNamespace("victim/db/t1")));
    EXPECT_TRUE(check->namespaceIsRemoved(RootNamespace("victim/db/t2")));
}

TEST(CasDecommission, RerunCountsAlreadyRemoved)
{
    auto backend = std::make_shared<InMemoryBackend>();
    {
        auto victim = openVictim(backend);
        makeTableWithRefs(*victim, "victim/db/t1", 1, 0);
    }
    (void)decommissionPoolMember(backend, PoolConfig{.pool_prefix = "p", .server_root_id = "a1"}, "victim");
    /// Task 4 will delete the slot on success and make a full re-run BAD_ARGUMENTS; until then a
    /// re-run must skip the Removed namespace idempotently.
    const auto second = decommissionPoolMember(backend, PoolConfig{.pool_prefix = "p", .server_root_id = "a2"}, "victim");
    EXPECT_EQ(second.namespaces_removed, 0u);
    EXPECT_EQ(second.namespaces_already_removed, 1u);
}

/// Task 2 review finding 1: `makeTableWithRefs`'s precommit seed uses an artificially high
/// `writer_epoch` (999999) specifically to dodge the writer's OWN stale-precommit sweep -- which
/// means it never exercised the path a REAL victim precommit takes. A genuine writer stamps
/// `manifest_ref.writer_epoch` from its OWN `liveWriterEpoch()` at precommit time
/// (`Build::precommitAdd`, CasStore.cpp:2087), i.e. the victim's era -- always LOWER than the admin
/// mount's freshly-minted epoch (`openForDecommission` always bumps strictly higher). `appendRefOps`
/// hoists `maybeSweepStalePrecommits` at its top (CasStore.cpp:1716), so without the
/// `skip_stale_precommit_sweep` fix that sweep would reclaim this realistic-epoch precommit in its
/// OWN transaction before `dropNamespace`'s removal transaction ever counts it, leaving
/// `precommits_removed` at 0 for exactly the case that matters.
TEST(CasDecommission, CountsRealisticEpochPrecommit)
{
    auto backend = std::make_shared<InMemoryBackend>();
    uint64_t victim_epoch = 0;
    {
        auto victim = openVictim(backend);
        victim_epoch = victim->writerEpoch();
        makeTableWithRefs(*victim, "victim/db/t1", /*committed=*/1, /*precommits=*/0);

        const RootNamespace ns("victim/db/t1");
        /// `build_sequence = 2`: distinct from `makeTableWithRefs`'s committed ref (`build_sequence = 1`)
        /// -- a REAL build's `ManifestRef` is unique per build, and a colliding one would trip the ref
        /// state machine's "manifest already has a conflicting owner" guard.
        const ManifestRef ref{.writer_epoch = victim_epoch, .build_sequence = 2, .manifest_ordinal = 1};
        writeManifestRaw(victim->backend(), victim->layout(), ns, ref, {});
        addPrecommitTransition(victim->backend(), victim->layout(), ns, UInt128(1), "precommit_0", std::nullopt, ref);
    }

    const auto report = decommissionPoolMember(
        backend, PoolConfig{.pool_prefix = "p", .server_root_id = "admin"}, "victim");

    EXPECT_EQ(report.namespaces_removed, 1u);
    EXPECT_EQ(report.committed_refs_removed, 1u);
    EXPECT_EQ(report.precommits_removed, 1u);
    EXPECT_EQ(report.edge_deltas_emitted, 2u);
}

/// Task 2 review finding 2: the `member_decommission` begin/namespace_removed/end events
/// (CasDecommission.cpp) had no assertion at all. Wire a capturing sink (the `gtest_cas_event_log.cpp`
/// idiom) into `decommissionPoolMember` and check the emitted sequence and its per-namespace detail.
TEST(CasDecommission, EmitsMemberDecommissionEvents)
{
    auto backend = std::make_shared<InMemoryBackend>();
    {
        auto victim = openVictim(backend);
        makeTableWithRefs(*victim, "victim/db/t1", /*committed=*/1, /*precommits=*/0);
    }

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
}

/// Task 3: the manifest-debris / staging / roots drain phases fill their three `DecommissionReport`
/// counters and leave nothing of the victim behind under `staging/` or `roots/`.
TEST(CasDecommission, DrainsDebrisStagingAndRoots)
{
    auto backend = std::make_shared<InMemoryBackend>();
    {
        auto victim = openVictim(backend);
        makeTableWithRefs(*victim, "victim/db/t1", 1, 0);
        seedOrphanManifestBody(*victim, "victim/db/t1");
    }
    /// Foreign staging + mountpoint objects, written raw (no writer machinery needed): the victim's
    /// writers are fenced by the claim before decommission ever gets here, so these are ordinary debris,
    /// not a live in-flight write.
    backend->putIfAbsent("p/staging/victim/upload1.tmp", "x");
    backend->putIfAbsent("p/staging/victim/upload2.tmp", "x");
    backend->putIfAbsent("p/roots/victim/clickhouse_access_check_abc", "x");

    const auto report = decommissionPoolMember(
        backend, PoolConfig{.pool_prefix = "p", .server_root_id = "admin"}, "victim");

    EXPECT_EQ(report.manifest_debris_removed, 1u);
    EXPECT_EQ(report.staging_objects_removed, 2u);
    EXPECT_EQ(report.mountpoint_objects_removed, 1u);
    EXPECT_TRUE(report.warnings.empty());

    /// Nothing of the victim remains under staging/ or roots/ (scoped LISTs are empty).
    EXPECT_TRUE(backend->list("p/staging/victim/", "", 10).keys.empty());
    EXPECT_TRUE(backend->list("p/roots/victim/", "", 10).keys.empty());
}

/// Task 3 fail-close nuance (spec §core "Fail-close"): a per-object failure in the staging/roots drain
/// -- a thrown exception (a transient hiccup) or a `TokenMismatch` outcome (a "listed but raced" miss)
/// -- must record a warning and let the rest of the sweep proceed, never abort the whole phase or the
/// whole command. One staging object throws, the roots object comes back `TokenMismatch`; the OTHER
/// staging object must still be deleted and counted.
TEST(CasDecommission, PerObjectFailureWarnsAndContinuesDrain)
{
    auto backend = std::make_shared<FailingDeleteBackend>();
    {
        auto victim = openVictim(backend);
        makeTableWithRefs(*victim, "victim/db/t1", 1, 0);
    }
    backend->putIfAbsent("p/staging/victim/upload_ok.tmp", "x");
    backend->putIfAbsent("p/staging/victim/upload_throws.tmp", "x");
    backend->putIfAbsent("p/roots/victim/clickhouse_access_check_abc", "x");
    backend->failWithThrow("p/staging/victim/upload_throws.tmp");
    backend->failWithTokenMismatch("p/roots/victim/clickhouse_access_check_abc");

    const auto report = decommissionPoolMember(
        backend, PoolConfig{.pool_prefix = "p", .server_root_id = "admin"}, "victim");

    EXPECT_EQ(report.staging_objects_removed, 1u)
        << "the OTHER staging object must still be deleted despite the injected failure on its sibling";
    EXPECT_EQ(report.mountpoint_objects_removed, 0u);
    EXPECT_EQ(report.warnings.size(), 2u)
        << "one warning for the thrown exception, one for the TokenMismatch outcome";

    EXPECT_FALSE(backend->head("p/staging/victim/upload_ok.tmp").exists)
        << "the healthy staging object was actually deleted, not merely skipped";
    EXPECT_TRUE(backend->head("p/staging/victim/upload_throws.tmp").exists)
        << "the failing object is left behind (untouched) so a re-run can retry it";
    EXPECT_TRUE(backend->head("p/roots/victim/clickhouse_access_check_abc").exists)
        << "TokenMismatch means nothing was actually deleted -- the object survives";
}

/// Review finding (Task 3 fix): the manifest-debris drain (spec §core step 4) must honor the SAME
/// tolerate-and-continue contract as `deleteListedPrefix` above -- it did not. Two failure classes,
/// both inside `sweepNamespace` (`CasOrphanManifestSweep.cpp`): a per-key `deleteExact` that throws
/// (`victim/db/t1`'s orphan body), and a namespace whose protection view is unreadable
/// (`activeManifestKeys` throws for `victim/db2`, which carries NO ref objects of its own so Task 2's
/// `listNamespaces` never touches it -- isolating this failure to the manifest-debris phase alone).
/// Both must land in `report.warnings` (so `warnings.empty() == false`, the signal the future
/// slot-deletion phase gates on) and neither may abort the run: the healthy `victim/db/t1` namespace
/// erasure and the staging drain must still complete normally.
TEST(CasDecommission, ManifestDebrisPhaseFailuresWarnAndContinue)
{
    auto backend = std::make_shared<FailingDeleteBackend>();
    String debris_key;
    {
        auto victim = openVictim(backend);
        makeTableWithRefs(*victim, "victim/db/t1", 1, 0);
        const ManifestId debris_id = seedOrphanManifestBody(*victim, "victim/db/t1");
        debris_key = victim->layout().manifestKey(debris_id);
        /// No makeTableWithRefs for "victim/db2" -- it has manifest debris but no ref objects, so
        /// `listNamespaces` (Task 2) never lists it.
        seedOrphanManifestBody(*victim, "victim/db2");
    }
    backend->failWithThrow(debris_key);
    backend->failNamespaceReads("victim/db2");
    backend->putIfAbsent("p/staging/victim/upload_ok.tmp", "x");

    const auto report = decommissionPoolMember(
        backend, PoolConfig{.pool_prefix = "p", .server_root_id = "admin"}, "victim");

    EXPECT_EQ(report.namespaces_removed, 1u)
        << "victim/db/t1's namespace erasure (Task 2) is untouched by either injected failure";
    EXPECT_EQ(report.manifest_debris_removed, 0u)
        << "neither group's body was actually deleted: t1's throws, db2's protection view never even "
           "reaches the delete loop";
    EXPECT_EQ(report.warnings.size(), 2u)
        << "one warning for the thrown per-key delete, one for the unreadable protection view";
    EXPECT_EQ(report.staging_objects_removed, 1u)
        << "the staging phase still ran to completion after the manifest-debris phase's failures -- "
           "the whole command did not abort";

    EXPECT_TRUE(backend->head(debris_key).exists)
        << "the failing object is left behind (untouched) so a re-run can retry it";
}
