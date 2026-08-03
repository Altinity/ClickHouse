#include <gtest/gtest.h>

#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Formats/CasFoldSealFormat.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Formats/CasGcStateFormat.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Formats/CasRefCkptFormat.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Formats/CasRefLogFormat.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Gc/CasGc.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Gc/CatalogLifecycleReconciler.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasPartWriteTxn.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasPool.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasRefProtocol.h>
#include <Common/ProfileEvents.h>
#include "cas_test_helpers.h"

#include <Poco/StreamChannel.h>

#include <algorithm>
#include <condition_variable>
#include <mutex>
#include <set>
#include <sstream>
#include <stdexcept>
#include <thread>

namespace DB::ErrorCodes
{
    extern const int NETWORK_ERROR;
}

/// THE DESTRUCTIVE-ROUND FRONTIER PROOF (spec 2026-07-27 "ref chain complete cut" §5).
///
/// Reachability is a property of the WHOLE POOL. A blob is unreferenced only if no namespace anywhere
/// owns an edge to it, so a round that deletes one is asserting something about every namespace at
/// once -- including the ones it never looked at. Task 7 made the per-namespace half of that assertion
/// cheap and exact: one `GET` at the cursor's arithmetic successor, absent means end-of-stream. Task 8
/// made a namespace that could NOT be walked say so durably. What neither can supply is the SET those
/// proofs have to cover, and that is what this task is about.
///
/// So the gate has three terms, and a round destroys only when all three are clear:
///
///     suppress_destructive = any anomaly this round
///                          OR any hold the seal carries
///                          OR the frontier is incomplete
///
/// The second term is STRUCTURAL. Every hold recorded today also records an anomaly, so the first term
/// happens to imply it -- but the invariant is the hold SET, not that coincidence, and the gate reads
/// the seal directly so that a future change to anomaly recording cannot quietly open it.
///
/// The third term is where Stage A admits what it cannot do. The universe is
/// `(sealed cursors) ∪ (this round's hint)`, two sources that can BOTH omit a namespace at once, and
/// the scenario that makes that fatal is the one these tests open with: a hidden acked `+1` in a
/// namespace neither source names, while a visible `-1` elsewhere drives the shared blob's OBSERVABLE
/// in-degree to zero. Every proof the round holds comes back clean and the blob is still owned. So
/// `UniversePolicy::kDefault` is `StageA_Suppressed` and production destroys NOTHING this stage; the
/// per-namespace logic below is reached only by a test that has constructed a closed universe and says
/// so. Stage B's catalog makes the universe knowable and its Task 7b flips that one constant.
///
/// Everything here is written so that flip is a source change and not a redesign: the per-namespace
/// proofs, the quiet-namespace probes, the budget, and every gated site are live and tested now.

using namespace DB::Cas;
using namespace DB::Cas::tests;

namespace ProfileEvents
{
extern const Event CasGcRefWalkPlansBuilt;
extern const Event CasGcUnmatchedAdoptedParentLives;
extern const Event CasGcNamespaceCleanupLeaks;
}

namespace
{

const UInt128 kGc = hexToU128("00000000000000000000000000000001");

/// The lying store, shared from `cas_test_helpers.h`: every key is served by exact GET while the
/// selected ones are HIDDEN from every LIST. That is the only way to build the cross-namespace
/// scenario -- the hidden namespace's records stay durable and readable, so a round that KNOWS to
/// look for them finds them, while a round that only enumerates never learns they exist. Composed
/// over `CountingBackend` because these tests also assert request counts.
using CountingHintHoleBackend = DB::Cas::tests::HintHoleBackendOn<DB::Cas::tests::CountingBackend>;

class DrainRaceBackend final : public CountingBackend
{
public:
    using CountingBackend::casPut;
    using CountingBackend::get;
    using CountingBackend::putIfAbsent;

    void blockNextCatalogCas(const String & key)
    {
        std::lock_guard lock(control_mutex);
        catalog_key = key;
        block_next_catalog_cas = true;
    }

    void loseNextCatalogCasResponse(const String & key)
    {
        std::lock_guard lock(control_mutex);
        catalog_key = key;
        lose_next_catalog_cas_response = true;
    }

    void conflictNextCatalogCas(const String & key)
    {
        std::lock_guard lock(control_mutex);
        catalog_key = key;
        conflict_next_catalog_cas = true;
    }

    void waitForBlockedCatalogCas()
    {
        std::unique_lock lock(control_mutex);
        control_cv.wait(lock, [&] { return catalog_cas_blocked; });
    }

    void releaseBlockedCatalogCas()
    {
        std::lock_guard lock(control_mutex);
        release_catalog_cas = true;
        control_cv.notify_all();
    }

    void clearJournal()
    {
        std::lock_guard lock(journal_mutex);
        journal.clear();
    }

    std::vector<String> journalSnapshot() const
    {
        std::lock_guard lock(journal_mutex);
        return journal;
    }

    std::optional<GetResult> get(const String & key, Range range) override
    {
        record("get " + key);
        return CountingBackend::get(key, range);
    }

    ListPage list(const String & prefix, const String & cursor, size_t limit) override
    {
        record("list " + prefix);
        return CountingBackend::list(prefix, cursor, limit);
    }

    PutResult putIfAbsent(const String & key, const String & bytes, const ObjectMeta & meta) override
    {
        record("put_begin " + key);
        const PutResult result = CountingBackend::putIfAbsent(key, bytes, meta);
        record("put_end " + key);
        return result;
    }

    CasResult casPut(
        const String & key, const String & bytes, const std::optional<Token> & expected,
        const ObjectMeta & meta) override
    {
        record("cas_begin " + key);
        bool lose_response = false;
        bool force_conflict = false;
        {
            std::unique_lock lock(control_mutex);
            if (key == catalog_key && block_next_catalog_cas)
            {
                block_next_catalog_cas = false;
                catalog_cas_blocked = true;
                control_cv.notify_all();
                control_cv.wait(lock, [&] { return release_catalog_cas; });
            }
            if (key == catalog_key && lose_next_catalog_cas_response)
            {
                lose_next_catalog_cas_response = false;
                lose_response = true;
            }
            if (key == catalog_key && conflict_next_catalog_cas)
            {
                conflict_next_catalog_cas = false;
                force_conflict = true;
            }
        }
        if (force_conflict)
        {
            record("cas_forced_conflict " + key);
            return {.outcome = CasOutcome::Conflict, .token = {}};
        }
        const CasResult result = CountingBackend::casPut(key, bytes, expected, meta);
        record("cas_end " + key);
        if (lose_response && result.outcome == CasOutcome::Committed)
        {
            record("cas_response_lost " + key);
            throw std::runtime_error("injected lost catalog CAS response");
        }
        return result;
    }

private:
    void record(String entry) const
    {
        std::lock_guard lock(journal_mutex);
        journal.push_back(std::move(entry));
    }

    mutable std::mutex journal_mutex;
    mutable std::vector<String> journal;
    std::mutex control_mutex;
    std::condition_variable control_cv;
    String catalog_key;
    bool block_next_catalog_cas = false;
    bool catalog_cas_blocked = false;
    bool release_catalog_cas = false;
    bool lose_next_catalog_cas_response = false;
    bool conflict_next_catalog_cas = false;
};

class PostFoldUnreadableTerminalBackend final : public CountingBackend
{
public:
    ListPage list(const String & prefix, const String & cursor, size_t limit) override
    {
        ListPage page = CountingBackend::list(prefix, cursor, limit);
        if (prefix.ends_with("/cas/ns/"))
            for (ListedKey & listed : page.keys)
                listed.token.reset();
        return page;
    }

    HeadResult head(const String & key) override
    {
        if (key == unreadable_key)
            throw std::runtime_error("injected post-fold terminal read failure for " + key);
        return CountingBackend::head(key);
    }

    void makeUnreadable(String key)
    {
        unreadable_key = std::move(key);
    }

    bool existsIgnoringFault(const String & key)
    {
        return InMemoryBackend::head(key).exists;
    }

private:
    String unreadable_key;
};

class ScopedCasGcLogCapture
{
public:
    ScopedCasGcLogCapture()
        : logger(getLogger("CasGc"))
        , channel(new Poco::StreamChannel(stream))
        , old_channel(logger->getChannel())
        , old_level(logger->getLevel())
    {
        logger->setChannel(channel.get());
        logger->setLevel("warning");
    }

    ~ScopedCasGcLogCapture()
    {
        logger->setChannel(old_channel);
        logger->setLevel(old_level);
    }

    String captured() const
    {
        return stream.str();
    }

private:
    LoggerPtr logger;
    std::ostringstream stream; // STYLE_CHECK_ALLOW_STD_STRING_STREAM
    Poco::AutoPtr<Poco::StreamChannel> channel;
    Poco::Channel * old_channel;
    int old_level;
};

struct CompletedRemovingFixture
{
    RootNamespace ns;
    UInt128 life_id{};
    String checkpoint_key;
    String checkpoint_bytes;
};

CompletedRemovingFixture seedCompletedRemoving(
    DrainRaceBackend & backend, const PoolPtr & store, const UInt128 & lease_owner)
{
    const Layout & layout = store->layout();
    CompletedRemovingFixture fixture{
        .ns = RootNamespace{"00/drain-race@cas@"},
        .life_id = UInt128{177},
        .checkpoint_key = {},
        .checkpoint_bytes = {}};
    CasRefCatalog::casAdmitEntry(backend, layout, store->poolConfig().gc_shards, CatalogEntry{
        .ns = fixture.ns, .state = NsState::Live, .incarnation = fixture.life_id});
    fixture.checkpoint_key = layout.refCkptKey(
        NamespaceLifeId::fromCatalogEntry(fixture.ns, fixture.life_id));
    fixture.checkpoint_bytes = encodeRefCkpt(RefCkpt{
        .life_epoch = 1,
        .committed_through = std::nullopt,
        .checkpoint_snapshot_id = std::nullopt,
        .last_epoch_seal = std::nullopt,
    });
    backend.putIfAbsent(fixture.checkpoint_key, fixture.checkpoint_bytes);
    EXPECT_TRUE(store->namespaceFilesLifeIfReadable(fixture.ns));
    CasRefCatalog::casUpdate(backend, layout, [](const RefCatalog & current)
    {
        RefCatalog next = current;
        next.entries[0].state = NsState::Removing;
        next.entries[0].removal_started_round = 1;
        return next;
    });

    CasFoldSeal parent;
    parent.generation = 1;
    parent.ref_lives.emplace(fixture.life_id, RefLifeFoldState{
        .coverage = RefCoverage{.classification = 2, .last_folded_ref_id = RefTxnId{1, 1}},
        .cleanup_evidence = RefCleanupEvidence{.remove_txn_id = RefTxnId{1, 1}}});
    for (uint64_t shard = 0; shard < store->poolConfig().gc_shards; ++shard)
        parent.condemned_summary.emplace(shard, CondemnedSummary{});
    backend.putIfAbsent(layout.foldSealKey(1, 1), encodeFoldSeal(parent));

    GcState state;
    state.round = 1;
    state.gc_shards = store->poolConfig().gc_shards;
    state.snap_generation = 1;
    state.snap_attempt = 1;
    state.lease = GcLease{.owner = lease_owner, .seq = 1};
    backend.putIfAbsent(layout.gcStateKey(), encodeGcState(state));

    return fixture;
}

void seedCompletedRemovingBatch(
    DrainRaceBackend & backend, const PoolPtr & store, const UInt128 & lease_owner, size_t count)
{
    const Layout & layout = store->layout();
    std::vector<CatalogEntry> entries;
    entries.reserve(count);
    for (size_t i = 0; i < count; ++i)
    {
        CatalogEntry entry{
            .ns = RootNamespace{fmt::format("00/drain-batch-{}@cas@", i)},
            .state = NsState::Live,
            .incarnation = UInt128{200 + i}};
        CasRefCatalog::casAdmitEntry(backend, layout, store->poolConfig().gc_shards, entry);
        entries.push_back(std::move(entry));
    }
    CasRefCatalog::casUpdate(backend, layout, [](const RefCatalog & current)
    {
        RefCatalog next = current;
        for (CatalogEntry & entry : next.entries)
        {
            entry.state = NsState::Removing;
            entry.removal_started_round = 1;
        }
        return next;
    });

    CasFoldSeal parent;
    parent.generation = 1;
    for (const CatalogEntry & entry : entries)
        parent.ref_lives.emplace(entry.incarnation, RefLifeFoldState{
            .coverage = RefCoverage{.classification = 2, .last_folded_ref_id = RefTxnId{1, 1}},
            .cleanup_evidence = RefCleanupEvidence{.remove_txn_id = RefTxnId{1, 1}}});
    for (uint64_t shard = 0; shard < store->poolConfig().gc_shards; ++shard)
        parent.condemned_summary.emplace(shard, CondemnedSummary{});
    ASSERT_EQ(backend.putIfAbsent(layout.foldSealKey(1, 1), encodeFoldSeal(parent)).outcome,
        PutOutcome::Done);

    GcState state;
    state.round = 1;
    state.gc_shards = store->poolConfig().gc_shards;
    state.snap_generation = 1;
    state.snap_attempt = 1;
    state.lease = GcLease{.owner = lease_owner, .seq = 1};
    ASSERT_EQ(backend.putIfAbsent(layout.gcStateKey(), encodeGcState(state)).outcome, PutOutcome::Done);
}

enum class CompetingCatalogOutcome : uint8_t
{
    Absent,
    Replacement,
};

class CasGcCompletedRemovalFenceRace : public testing::TestWithParam<CompetingCatalogOutcome>
{
};

void transferGcLease(DrainRaceBackend & backend, const Layout & layout, const UInt128 & new_owner)
{
    const auto got = backend.get(layout.gcStateKey());
    ASSERT_TRUE(got);
    GcState state = decodeGcState(got->bytes);
    state.lease.owner = new_owner;
    ++state.lease.seq;
    ASSERT_EQ(backend.casPut(layout.gcStateKey(), encodeGcState(state), got->token).outcome,
        CasOutcome::Committed);
}

size_t findJournalAfter(const std::vector<String> & journal, const String & entry, size_t after)
{
    const auto it = std::find(journal.begin() + static_cast<ptrdiff_t>(after), journal.end(), entry);
    return it == journal.end() ? journal.size() : static_cast<size_t>(it - journal.begin());
}

/// A pool whose GC frontier-probe budget is set explicitly. Everything else matches `openPoolForTest`.
PoolPtr openPoolWithProbeBudget(std::shared_ptr<InMemoryBackend> backend, uint64_t budget)
{
    return Pool::open(std::move(backend),
        PoolConfig{.pool_prefix = "p", .server_root_id = "test",
                   .gc_frontier_probe_budget = budget, .gc_fold_max_defer_rounds = 0});
}

/// Publish `ref_name` in `ns` pinning `blob`, allocating the next ref-log id. Writes the blob body and
/// the manifest body too, so the published edge is one GC can actually fold.
ManifestRef publish(Backend & backend, const Layout & layout, const RootNamespace & ns,
                    const String & ref_name, uint64_t build_sequence, const DB::UInt128 & blob)
{
    const ManifestRef mref{.writer_epoch = 1, .build_sequence = build_sequence, .manifest_ordinal = 1};
    writeBlobBody(backend, layout, blob);
    writeManifestRaw(backend, layout, ns, mref, {blobEntryFor("data.bin", blob)});
    publishCommittedTransition(backend, layout, ns, ref_name, std::nullopt, mref);
    return mref;
}

/// The blob key for a raw hash, as the tests spell it.
String blobKeyOf(const Layout & layout, const DB::UInt128 & hash)
{
    return layout.blobKey(legacyMetaTestRef(hash));
}

/// The sealed fold cursor for `ns` as a full `RefTxnId`. Every seed here allocates `writer_epoch = 1`,
/// which is what `foldCursorOf` (returning the sequence alone) assumes too.
RefTxnId sealedCursorOf(Backend & backend, const Layout & layout, const RootNamespace & ns)
{
    return RefTxnId{1, foldCursorOf(backend, layout, ns, /*shard*/ 0)};
}

/// Drive `rounds` GC rounds under the given policy, renewing the store's watermark between them the way
/// the production scheduler does.
void drive(const PoolPtr & store, Gc & gc, int rounds, UniversePolicy policy)
{
    for (int i = 0; i < rounds; ++i)
    {
        gc.runRegularRound({}, /*allow_steal*/true, policy);
        store->renewWatermarkOnce();
    }
}

/// Every key the backend was asked to delete, rendered for a failing assertion's message.
String deletedKeysMessage(const CountingBackend & backend)
{
    String out;
    for (const String & key : backend.deletedKeys())
        out += "\n    " + key;
    return out.empty() ? String{" (none)"} : out;
}

}

/// ===================== THE KILL SHOT: A HIDDEN `+1` IN AN UNKNOWN NAMESPACE =====================
///
/// Two namespaces share one blob. `visible` publishes it and then drops it, so the round observes
/// `+1` then `-1` and reads the blob's in-degree as zero. `hidden` also owns it -- durably, acked,
/// readable by exact key -- but is absent from the round's LIST hint AND has never been folded, so it
/// has no sealed cursor either. Neither source names it, and every per-namespace proof the round can
/// take comes back clean.
///
/// This is the scenario the Stage-A constant exists for, and the three arms below are the whole
/// argument: the production default refuses; flipping the constant while the namespace is genuinely
/// outside the universe DELETES the blob (so the constant is load-bearing, not decorative); and once
/// the namespace is inside the universe, the probe finds the hidden `+1` and the blob is safe on the
/// per-namespace logic alone -- which is exactly what Stage B will rely on.

namespace
{
/// Build the shared-blob scenario. `hidden` owns `blob` and is hidden from every LIST; `visible`
/// publishes and drops it. Returns the pool.
PoolPtr buildCrossNamespaceScenario(const std::shared_ptr<CountingHintHoleBackend> & backend,
                                    const RootNamespace & hidden, const RootNamespace & visible,
                                    const DB::UInt128 & blob, bool fold_hidden_first)
{
    auto store = openPoolForTest(backend, /*gc_fold_max_defer_rounds=*/0);
    const Layout & layout = store->layout();

    if (fold_hidden_first)
    {
        /// Give the hidden namespace a sealed cursor, WITHOUT folding the edge under test. It publishes
        /// an unrelated blob and one round folds that; from then on the namespace is in the universe via
        /// its cursor even after the hint stops naming it.
        ///
        /// The unrelated blob is what makes this arm mean anything: if the shared blob's `+1` had
        /// already been folded by the seeding round, the blob would survive on the DURABLE in-degree and
        /// the test would pass whether or not the round probes anything. Publishing it only AFTER the
        /// seal puts it strictly above the cursor, so the probe is the one and only thing that can find
        /// it.
        publish(*backend, layout, hidden, "seed_ref", 7, DB::UInt128(0x5eed));
        Gc seed(store, kGc);
        seed.runRegularRound();
        store->renewWatermarkOnce();
    }

    publish(*backend, layout, hidden, "kept_ref", 1, blob);
    backend->hidePrefix(layout.namespaceStreamPrefix(fixture::fixtureLife(hidden)));

    const ManifestRef dropped = publish(*backend, layout, visible, "dropped_ref", 2, blob);
    dropRefTransition(*backend, layout, visible, "dropped_ref", dropped);
    return store;
}
}

TEST(CasGcFrontierGate, HiddenPlusOneInAnUnknownNamespaceIsRefusedByTheProductionDefault)
{
    auto backend = std::make_shared<CountingHintHoleBackend>();
    const RootNamespace hidden{"00/hidden@cas@"};
    const RootNamespace visible{"00/visible@cas@"};
    const DB::UInt128 blob(0x5ade);

    auto store = buildCrossNamespaceScenario(backend, hidden, visible, blob, /*fold_hidden_first=*/false);
    const Layout & layout = store->layout();

    Gc gc(store, kGc);
    backend->resetCounts();
    drive(store, gc, /*rounds*/ 5, UniversePolicy::kDefault);

    EXPECT_TRUE(backend->head(blobKeyOf(layout, blob)).exists)
        << "the blob a hidden namespace still owns must survive";
    EXPECT_EQ(backend->deleteTotal(), 0u)
        << "the production default proves no frontier, so the round destroys NOTHING. Deleted:"
        << deletedKeysMessage(*backend);
}

/// THE CONSTANT USED TO BE LOAD-BEARING HERE, and no longer is -- Stage B (Task 4-C) closed the gap it
/// demonstrated. Same pool, same hidden `+1`; the caller still asserts the universe is closed (a lie it
/// tells deliberately), but discovery is now catalog-authoritative: `hidden` is `Live` in the catalog, so
/// it stays a member of `frontier_namespaces` regardless of what the LIST hides, and it never sealed a
/// cursor, so its OWN frontier is provably unproven. `frontier_proven == frontier_namespaces` therefore
/// fails on member count alone, and destructive work stays suppressed even though the caller asserted the
/// universe closed. The blob survives. This is a strict improvement: the old failure mode this test
/// pinned was exactly the "list liar" vulnerability `CasListLiarEndToEnd`'s whole suite is about, and it
/// is gone even under the deliberately-dangerous test override, not merely under the production default.
TEST(CasGcFrontierGate, TheSameHiddenPlusOneSurvivesEvenWhenTheUniverseIsDeclaredAuthoritative)
{
    auto backend = std::make_shared<CountingHintHoleBackend>();
    const RootNamespace hidden{"00/hidden@cas@"};
    const RootNamespace visible{"00/visible@cas@"};
    const DB::UInt128 blob(0x5ade);

    auto store = buildCrossNamespaceScenario(backend, hidden, visible, blob, /*fold_hidden_first=*/false);
    const Layout & layout = store->layout();

    Gc gc(store, kGc);
    drive(store, gc, /*rounds*/ 5, UniversePolicy::AuthoritativeForTest);

    EXPECT_TRUE(backend->head(blobKeyOf(layout, blob)).exists)
        << "`hidden` is catalog-Live and never proved its own frontier, so the round-wide frontier stays "
           "incomplete and nothing irreversible runs, even though the caller declared the universe closed";
}

/// Review I4: the two arms above assert "nothing was deleted" and "nothing was deleted" -- neither now
/// distinguishes the gate correctly refusing from the round simply never deleting anything at all. This
/// is the replacement positive control: `hidden` is genuinely folded through its OWN drop of the same
/// blob (an honest exact-key read of a record the LIST still hides -- the arithmetic-intake mechanism
/// this whole file is about), so its frontier is REALLY proven, not merely declared so, and the blob is
/// REALLY unreferenced by both namespaces. The round drains it. Proves the machinery this task changed
/// can still reclaim genuine garbage once every namespace is honestly proven -- the two zero-deletion
/// arms above would pass identically if the round were simply incapable of ever deleting anything.
TEST(CasGcFrontierGate, TheSameBlobDrainsOnceHiddenGenuinelyProvesItsOwnFrontier)
{
    auto backend = std::make_shared<CountingHintHoleBackend>();
    const RootNamespace hidden{"00/hidden@cas@"};
    const RootNamespace visible{"00/visible@cas@"};
    const DB::UInt128 blob(0x5ade);

    auto store = openPoolForTest(backend, /*gc_fold_max_defer_rounds=*/0);
    const Layout & layout = store->layout();
    Gc gc(store, kGc);

    /// `hidden`'s BIRTH is folded (and its cursor SEALED) with everything still listed -- a namespace
    /// with no `_ckpt` (the raw-fixture admission this file's helper uses never publishes one) has no
    /// genesis signal EXCEPT a sealed cursor or a visible LIST, so a real fold first is what makes an
    /// arithmetic (cursor-relative) genesis available at all for what follows.
    const ManifestRef kept = publish(*backend, layout, hidden, "kept_ref", 1, blob);
    ASSERT_TRUE(gc.runRegularRound().acquired_lease);
    store->renewWatermarkOnce();

    /// NOW `hidden` drops its own reference (written while still fully listed, so the raw fixture's own
    /// LIST -- finding the greatest existing log id, to derive the next one -- sees the truth), and
    /// ONLY THEN does its whole prefix vanish from every subsequent LIST. With a sealed cursor already
    /// in hand the walk's genesis is arithmetic (`cursor + 1`), so this drop is found and folded by
    /// exact key alone -- the arithmetic-intake mechanism this whole file is about, exercised honestly
    /// rather than declared past by fiat.
    dropRefTransition(*backend, layout, hidden, "kept_ref", kept);
    backend->hidePrefix(layout.namespaceStreamPrefix(fixture::fixtureLife(hidden)));

    const ManifestRef dropped = publish(*backend, layout, visible, "dropped_ref", 2, blob);
    dropRefTransition(*backend, layout, visible, "dropped_ref", dropped);

    drive(store, gc, /*rounds*/ 5, UniversePolicy::AuthoritativeForTest);

    EXPECT_FALSE(backend->head(blobKeyOf(layout, blob)).exists)
        << "both namespaces genuinely proved their frontier and the blob is genuinely unreferenced -- "
           "the round must still be able to reclaim it";
}

/// AND THE PER-NAMESPACE LOGIC IS WHAT SAVES IT. Identical to the arm above except that the hidden
/// namespace was folded once first, so it carries a sealed cursor and is therefore IN the universe even
/// though the hint has gone silent about it. The round probes its expected-next by exact key, finds the
/// record the listing hid, folds the `+1`, and the blob is never condemned.
TEST(CasGcFrontierGate, AKnownNamespaceIsProbedByExactKeyAndItsHiddenEdgeSavesTheBlob)
{
    auto backend = std::make_shared<CountingHintHoleBackend>();
    const RootNamespace hidden{"00/hidden@cas@"};
    const RootNamespace visible{"00/visible@cas@"};
    const DB::UInt128 blob(0x5ade);

    auto store = buildCrossNamespaceScenario(backend, hidden, visible, blob, /*fold_hidden_first=*/true);
    const Layout & layout = store->layout();

    Gc gc(store, kGc);
    drive(store, gc, /*rounds*/ 5, UniversePolicy::AuthoritativeForTest);

    EXPECT_TRUE(backend->head(blobKeyOf(layout, blob)).exists)
        << "the cursor kept the namespace in the universe, so its frontier was probed and its edge folded";
}

/// ===================== THE PRODUCTION DEFAULT IS INERT, PROOFS OR NO PROOFS =====================
///
/// A pool with nothing hidden, nothing held, no anomaly, and every namespace walked to an honest
/// end-of-stream: every per-namespace proof is green. The production default still destroys nothing,
/// because the term it fails is the one about the SET, not about any namespace in it.
TEST(CasGcFrontierGate, ProductionDefaultDestroysNothingEvenWithEveryProofGreen)
{
    auto backend = std::make_shared<CountingBackend>();
    auto store = openPoolForTest(backend, /*gc_fold_max_defer_rounds=*/0);
    const Layout & layout = store->layout();
    const RootNamespace ns{"00/aa@cas@"};
    const DB::UInt128 blob(0xdead);

    const ManifestRef mref = publish(*backend, layout, ns, "ref_1", 1, blob);
    dropRefTransition(*backend, layout, ns, "ref_1", mref);

    Gc gc(store, kGc);
    backend->resetCounts();
    drive(store, gc, /*rounds*/ 5, UniversePolicy::kDefault);

    EXPECT_TRUE(backend->head(blobKeyOf(layout, blob)).exists);
    EXPECT_EQ(backend->deleteTotal(), 0u)
        << "no anomaly, no hold, every namespace proven -- and still inert, because Stage A cannot "
           "prove the universe. Deleted:" << deletedKeysMessage(*backend);

    /// The control: the very same pool reclaims once the universe is declared closed. Without it this
    /// test would also pass on a pool where nothing was ever condemnable.
    drive(store, gc, /*rounds*/ 4, UniversePolicy::AuthoritativeForTest);
    EXPECT_FALSE(backend->head(blobKeyOf(layout, blob)).exists)
        << "the pool WAS reclaimable; only the universe seam was holding it";
}

/// ===================== EVERY DESTRUCTIVE SITE, INDIVIDUALLY =====================
///
/// The inventory as an assertion. The pool below has real work waiting at every gated site: a
/// graduated blob to delete, an owner-removed manifest body to delete, aged generations to prune and
/// hand off, ref logs and snapshots covered by a durable snapshot, and a removed namespace with a
/// Pending cleanup item. A suppressed round issues ZERO deletes against ALL of them, and the per-site
/// assertions name which one leaked if any does.

namespace
{
/// A pool with destructive work pending at every site, plus a few completed rounds so generations have
/// aged past the retention floor. Returns the hash of a blob whose in-degree has dropped to zero.
DB::UInt128 buildPoolWithWorkAtEverySite(const std::shared_ptr<CountingBackend> & backend,
                                         const PoolPtr & store, Gc & gc)
{
    const Layout & layout = store->layout();
    const RootNamespace live{"00/live@cas@"};
    const RootNamespace doomed{"00/doomed@cas@"};
    const DB::UInt128 blob(0xfeed);

    /// A long-lived namespace that keeps publishing, so snapshots and covered logs accumulate and
    /// generations keep advancing past the retention floor.
    for (uint64_t i = 1; i <= 4; ++i)
    {
        publish(*backend, layout, live, "ref_" + std::to_string(i), i, DB::UInt128(0x1000 + i));
        runRegularRoundReclaiming(gc);
        store->renewWatermarkOnce();
    }

    /// The condemnable blob: published in `doomed`, then dropped. Its manifest body becomes
    /// owner-removed cleanup work at the same time.
    const ManifestRef mref = publish(*backend, layout, doomed, "doomed_ref", 9, blob);
    runRegularRoundReclaiming(gc);
    store->renewWatermarkOnce();
    dropRefTransition(*backend, layout, doomed, "doomed_ref", mref);
    return blob;
}
}

TEST(CasGcFrontierGate, EveryInventoriedDestructiveSiteIsInertUnderSuppression)
{
    auto backend = std::make_shared<CountingBackend>();
    auto store = openPoolForTest(backend, /*gc_fold_max_defer_rounds=*/0);
    const Layout & layout = store->layout();

    Gc gc(store, kGc);
    const DB::UInt128 blob = buildPoolWithWorkAtEverySite(backend, store, gc);

    /// From here on the rounds run on the production default: every site has work queued and every site
    /// must decline it.
    backend->resetCounts();
    drive(store, gc, /*rounds*/ 6, UniversePolicy::kDefault);

    EXPECT_EQ(backend->deleteTotal(), 0u)
        << "a suppressed round performs NO destructive work at any site. Deleted:"
        << deletedKeysMessage(*backend);

    /// And the same statement per site, so a failure names the leak rather than just its count.
    EXPECT_EQ(backend->deleteCountForKeysContaining("/blobs/"), 0u) << "pre-CAS blob delete";
    EXPECT_EQ(backend->deleteCountForKeysContaining("/cas/manifests/"), 0u) << "manifest-body delete";
    EXPECT_EQ(backend->deleteCountForKeysContaining("/gc/gen/"), 0u)
        << "generation prune and hand-off reclaim";
    EXPECT_EQ(backend->deleteCountForKeysContaining("/cas/ns/stream/"), 0u)
        << "covered-log / superseded-snapshot cleanup";

    EXPECT_TRUE(backend->head(blobKeyOf(layout, blob)).exists);

    /// The control again: the identical pool DOES reclaim at those sites once the universe is closed,
    /// so the zeros above are the gate at work and not an empty work queue.
    drive(store, gc, /*rounds*/ 4, UniversePolicy::AuthoritativeForTest);
    EXPECT_GT(backend->deleteTotal(), 0u)
        << "the work queue was real -- an authoritative round drains it";
    EXPECT_FALSE(backend->head(blobKeyOf(layout, blob)).exists);
}

/// R11 (`docs/superpowers/cas/2026-07-28-ref-rework-adjacent-findings.md {#r11-empty-universe-vacuous}`):
/// `Gc::fold` computes `frontier_complete = universe_authoritative && frontier_proven ==
/// frontier_namespaces`. With an empty universe that equality is `0 == 0` -- vacuously TRUE unless
/// guarded -- so this pool is built to make `frontier_namespaces` GENUINELY zero by every source that
/// feeds it: no catalog entry (`injectRetire` touches only `gc/state`'s shard bookkeeping, never a
/// namespace), no sealed cursor (no namespace has EVER folded, so the prior seal's coverage map --
/// `parent_cursors` -- carries nothing), and no ref-log hint (none was ever written). A condemned blob
/// with a real, present body and in-degree 0 is queued the way a real round would leave it
/// (`injectRetire`), so if the guard is missing this round has every reason to delete it.
TEST(CasGcFrontierGate, AGenuinelyEmptyUniverseRefusesTheFrontierDespiteZeroEqualsZero)
{
    auto backend = std::make_shared<CountingBackend>();
    auto store = openPoolForTest(backend, /*gc_fold_max_defer_rounds=*/0);
    const Layout & layout = store->layout();
    const DB::UInt128 blob(0xbead);

    writeBlobBody(*backend, layout, blob);
    const BlobRef blob_ref = legacyMetaTestRef(blob);
    const Token blob_token = backend->head(layout.blobKey(blob_ref)).token;
    injectRetire(*backend, layout, /*round*/ 1, /*shard*/ 0,
        {RetiredEntry{.kind = ObjectKind::Blob, .ref = blob_ref, .token = blob_token, .size = 0}});
    store->renewWatermarkOnce();

    Gc gc(store, kGc);
    backend->resetCounts();
    drive(store, gc, /*rounds*/ 8, UniversePolicy::AuthoritativeForTest);

    /// Per-delete-family, not an aggregate: an aggregate zero can hide one family that ran while
    /// another did not (the same lesson R11's own writeup draws from this campaign's
    /// `entries_redeleted >= objects_deleted` vacuous-truth precedent).
    EXPECT_EQ(backend->deleteTotal(), 0u)
        << "an authoritative-but-EMPTY universe must refuse to be a complete frontier -- 0 == 0 is not "
           "a proof. Deleted:" << deletedKeysMessage(*backend);
    EXPECT_EQ(backend->deleteCountForKeysContaining("/blobs/"), 0u) << "pre-CAS blob delete";
    EXPECT_EQ(backend->deleteCountForKeysContaining("/cas/manifests/"), 0u) << "manifest-body delete";
    EXPECT_EQ(backend->deleteCountForKeysContaining("/gc/gen/"), 0u)
        << "generation prune and hand-off reclaim";
    EXPECT_EQ(backend->deleteCountForKeysContaining("/cas/ns/stream/"), 0u)
        << "covered-log / superseded-snapshot cleanup";
    EXPECT_TRUE(backend->head(layout.blobKey(blob_ref)).exists);

    /// The control: admitting ONE namespace with nothing durable in it (a catalog-only walk target,
    /// proven by a single absent exact GET -- `AQuietKnownNamespaceCostsExactlyOneExactGet`'s own
    /// shape) closes the universe non-vacuously (`frontier_namespaces = 1`, `frontier_proven = 1`), and
    /// the SAME blob drains -- proving the zeros above were the guard, not an empty work queue.
    ///
    /// `casAdmitEntry` alone reaches `Live` with NO `_ckpt` (the acknowledged bridge divergence from
    /// `completeCreation`'s production path, which always publishes `_ckpt.life_epoch` first); the walk
    /// needs that `life_epoch` to seed its FIRST probe for a namespace with no log at all (the `expected`
    /// initialization in `Gc::fold`, Stage B's `checkpoints.life_epochs` read). So this control writes a
    /// `_ckpt` with `life_epoch` set directly, at the same sentinel key `casAdmitEntry` pinned the
    /// namespace's incarnation to -- proving the namespace's own genesis, not guessing it.
    const RootNamespace empty_ns{"00/empty@cas@"};
    fixture::admitLive(*backend, layout, empty_ns);
    backend->putIfAbsent(layout.refCkptKey(fixture::fixtureLife(empty_ns)),
        encodeRefCkpt(RefCkpt{.life_epoch = std::optional<uint64_t>{1},
                              .checkpoint_snapshot_id = std::nullopt, .last_epoch_seal = std::nullopt}));
    drive(store, gc, /*rounds*/ 8, UniversePolicy::AuthoritativeForTest);
    EXPECT_FALSE(backend->head(layout.blobKey(blob_ref)).exists)
        << "the work WAS real -- once the universe is provably closed (even trivially), it drains";
}

/// The generation prune's cursor must not move on a suppressed round either. It is a monotone
/// high-water mark that the wholesale prune never revisits, so a cursor that advanced past a generation
/// this round declined to delete would strand that generation's whole prefix with no reclaimer left.
TEST(CasGcFrontierGate, ASuppressedRoundDoesNotAdvanceTheGenerationPruneCursor)
{
    auto backend = std::make_shared<CountingBackend>();
    auto store = openPoolForTest(backend, /*gc_fold_max_defer_rounds=*/0);
    const Layout & layout = store->layout();
    const RootNamespace ns{"00/aa@cas@"};

    Gc gc(store, kGc);
    for (uint64_t i = 1; i <= 6; ++i)
    {
        publish(*backend, layout, ns, "ref_" + std::to_string(i), i, DB::UInt128(0x2000 + i));
        runRegularRoundReclaiming(gc);
        store->renewWatermarkOnce();
    }
    const uint64_t pruned_through_before =
        decodeGcState(backend->get(layout.gcStateKey())->bytes).snap_pruned_through;

    for (uint64_t i = 7; i <= 10; ++i)
    {
        publish(*backend, layout, ns, "ref_" + std::to_string(i), i, DB::UInt128(0x2000 + i));
        gc.runRegularRound();
        store->renewWatermarkOnce();
    }

    EXPECT_EQ(decodeGcState(backend->get(layout.gcStateKey())->bytes).snap_pruned_through,
              pruned_through_before)
        << "the retention cursor is a high-water mark; it may not pass a generation nothing deleted";
}

/// THE HAND-OFF RECLAIM, WHICH THE INVENTORY TEST ABOVE CANNOT REACH. This site only fires for a
/// generation the wholesale prune SKIPPED while a live ref still pinned it (so the retention cursor
/// moved past it and will never revisit it) and which a later round's ref then moves off. Building that
/// takes a deliberately idle shard and a retention cursor driven past it, which is why it gets its own
/// test rather than riding on the inventory pool.
///
/// It is reachable under suppression precisely because FOLDING still happens on a suppressed round: the
/// ref moves off the old generation exactly as it would otherwise, and only the reclaim is withheld.
TEST(CasGcFrontierGate, TheHandOffReclaimIsInertUnderSuppression)
{
    auto backend = std::make_shared<CountingBackend>();
    auto store = Pool::open(backend,
        PoolConfig{.pool_prefix = "p", .server_root_id = "test",
                   .gc_snap_generations_to_keep = 1, .gc_fold_max_defer_rounds = 0});
    const Layout & layout = store->layout();
    const RootNamespace ns{"00/aa@cas@"};

    const ManifestRef r1 = publish(*backend, layout, ns, "tbl", 1, DB::UInt128(0xa1));

    Gc gc(store, kGc);
    runRegularRoundReclaiming(gc);
    store->renewWatermarkOnce();
    const uint64_t old_gen = decodeGcState(backend->get(layout.gcStateKey())->bytes).snap_generation;
    const String old_prefix = layout.gcGenPrefix(old_gen);
    ASSERT_FALSE(backend->list(old_prefix, "", 1000).keys.empty());

    /// Idle-carry the ref until the retention cursor is strictly PAST its generation. Until then an
    /// ordinary prune could still reclaim it and the hand-off would not be the load-bearing path.
    for (int i = 0; i < 6; ++i)
    {
        runRegularRoundReclaiming(gc);
        store->renewWatermarkOnce();
    }
    ASSERT_GT(decodeGcState(backend->get(layout.gcStateKey())->bytes).snap_pruned_through, old_gen)
        << "the generation must be behind the retention cursor before the hand-off is exercised";
    ASSERT_FALSE(backend->list(old_prefix, "", 1000).keys.empty())
        << "and still retained, because a live ref pins it";

    /// A real delta moves the shard's run off the old generation. This is the round the hand-off would
    /// reclaim it on -- and it runs on the production default.
    const ManifestRef r2{.writer_epoch = 1, .build_sequence = 2, .manifest_ordinal = 1};
    writeBlobBody(*backend, layout, DB::UInt128(0xb2));
    writeManifestRaw(*backend, layout, ns, r2, {blobEntryFor("data.bin", DB::UInt128(0xb2))});
    publishCommittedTransition(*backend, layout, ns, "tbl", r1, r2);

    backend->resetCounts();
    gc.runRegularRound();

    EXPECT_EQ(backend->deleteCountForKeysContaining("/gc/gen/"), 0u)
        << "a suppressed round hands nothing off. Deleted:" << deletedKeysMessage(*backend);
    EXPECT_FALSE(backend->list(old_prefix, "", 1000).keys.empty())
        << "the superseded generation's prefix survives a suppressed round intact";

    /// AND THE OPPORTUNITY IS CONSUMED, NOT DEFERRED -- the one place in this task where the gate
    /// costs something permanent, so it is asserted here rather than left to be discovered later.
    ///
    /// The hand-off is a one-shot DIFFERENCE: it compares the PARENT seal's runs against the new
    /// seal's, and the suppressed round above already folded the delta, so the next round's parent
    /// seal no longer mentions the old generation. Nothing revisits it -- the retention cursor is
    /// already past it and the prune never goes back. The prefix is left to `fsck`, which is exactly
    /// the outcome the site's own doc comment already records for a crash in the same window ("the
    /// cursor already advanced, so a plain retry will NOT re-attempt it; fsck is the backstop").
    /// Bounded (one small run per shard per occurrence) and not a correctness problem -- but in
    /// Stage A every round is suppressed, so every such transition leaks rather than one in a crash.
    ///
    /// The hand-off itself is not going untested: `CasGcRetention.HandOffDeletesSupersededRef` drives
    /// the same transition on an authoritative round and asserts the prefix IS reclaimed.
    runRegularRoundReclaiming(gc);
    EXPECT_FALSE(backend->list(old_prefix, "", 1000).keys.empty())
        << "the hand-off is a one-shot difference: the suppressed round consumed it, so the prefix is "
           "now fsck's problem rather than a later round's";
}

/// THE ORPHAN-MANIFEST SWEEP, which the inventory pool above also cannot reach: it only deletes bodies
/// that no ref names AND whose build is provably dead by the durable watermark floor, so it needs a
/// pool seeded with exactly that -- orphan bodies and a floor above them.
///
/// It is gated with its CURSOR, not just its deletes. The cursor paces a cold-prefix enumeration and
/// nothing revisits a range it passed, so advancing it on a round that swept nothing would silently
/// skip that range forever. A suppressed round therefore declines the whole pass.
TEST(CasGcFrontierGate, TheOrphanManifestSweepAndItsCursorAreInertUnderSuppression)
{
    auto backend = std::make_shared<CountingBackend>();
    auto store = Pool::open(backend,
        PoolConfig{.pool_prefix = "p", .server_root_id = "gc-runner",
                   .manifest_sweep_list_budget_keys = 1, .manifest_sweep_delete_budget_keys = 1,
                   .gc_fold_max_defer_rounds = 0});
    const Layout & layout = store->layout();
    const RootNamespace ns{"test/aa@cas@"};
    /// The control arm below needs a recoverable catalog life whose frontier is exactly the carried
    /// cursor. An empty non-seal transaction is a valid genesis that recovers to an empty table while
    /// leaving the manifest epoch below the cursor's epoch.
    fixture::admitLive(*backend, layout, ns);
    fixture::writeRefLogRaw(*backend, layout, RefLogTxn{
        .ns = ns.string(),
        .txn_id = RefTxnId{6, 1},
        .ops = {},
        .prev_epoch_seal = std::nullopt,
    });
    writeRecoverableCkptForRawFixture(*backend, layout, ns, RefCkpt{
        .life_epoch = 6,
        .committed_through = RefTxnId{6, 1},
        .checkpoint_snapshot_id = std::nullopt,
        .last_epoch_seal = std::nullopt,
    });

    /// Two manifest bodies no ref ever named, under a build the durable floor has already passed.
    const ManifestRef r1{.writer_epoch = 5, .build_sequence = 0xCA01, .manifest_ordinal = 1};
    const ManifestRef r2{.writer_epoch = 5, .build_sequence = 0xCA02, .manifest_ordinal = 1};
    writeManifestRaw(*backend, layout, ns, r1, {blobEntryFor("a", DB::UInt128(0xa1))});
    writeManifestRaw(*backend, layout, ns, r2, {blobEntryFor("b", DB::UInt128(0xb2))});
    setWatermarkMinActive(*backend, layout, "test", r1.writer_epoch, /*min_active*/ 0xCA03);
    /// The §6 deletion premise is a second precondition on the CONTROL arm below: a manifest of an
    /// epoch-`E` build is deletable only once the namespace's sealed fold cursor sits in an epoch
    /// strictly above `E`. Sealing that cursor here is what keeps this test about the GATE — without it
    /// the control arm would stop deleting for the premise's reason, and a removed gate would no longer
    /// show up as a difference between the two arms. A real round rewrites this row with the same cursor
    /// (the namespace is known, quiet and unheld, so the walk probes `cursor+1`, finds the frontier and
    /// carries the cursor), so the seeded fact survives every round below.
    seedFoldCursorForTest(*backend, layout, ns, RefTxnId{r1.writer_epoch + 1, 1});

    Gc gc(store, kGc);
    backend->resetCounts();
    for (int i = 0; i < 4; ++i)
    {
        gc.runRegularRound();
        store->renewWatermarkOnce();
    }

    EXPECT_EQ(backend->deleteCountForKeysContaining("/cas/manifests/"), 0u)
        << "a suppressed round sweeps nothing. Deleted:" << deletedKeysMessage(*backend);
    EXPECT_TRUE(backend->head(layout.manifestKey(ManifestId{ns, r1})).exists);
    EXPECT_TRUE(backend->head(layout.manifestKey(ManifestId{ns, r2})).exists);
    EXPECT_TRUE(decodeGcState(backend->get(layout.gcStateKey())->bytes).manifest_sweep_cursor.empty())
        << "the sweep cursor must not advance over a range the round declined to sweep -- nothing "
           "revisits it";

    /// The control: the same orphans ARE swept once the universe is authoritative.
    for (int i = 0; i < 4; ++i)
    {
        runRegularRoundReclaiming(gc);
        store->renewWatermarkOnce();
    }
    EXPECT_FALSE(backend->head(layout.manifestKey(ManifestId{ns, r1})).exists);
    EXPECT_FALSE(backend->head(layout.manifestKey(ManifestId{ns, r2})).exists);
}


/// ===================== QUIET NAMESPACES AND THE PROBE BUDGET =====================

/// THE TALLY ARITHMETIC, at a PARTIAL budget — the case neither 0 nor the default reaches.
///
/// `frontier_namespaces` is the denominator an operator reads as "the round's universe", and the
/// integration test reads it too. A valid checkpoint at every quiet namespace's carried cursor is
/// authoritative independently of LIST and the probe budget: all three lives are proven without
/// successor probes, so the budget leaves no namespace unprobed.
TEST(CasGcFrontierGate, APartialProbeBudgetPublishesATallyThatMatchesTheSealedSet)
{
    auto backend = std::make_shared<CountingHintHoleBackend>();
    auto store = openPoolWithProbeBudget(backend, /*budget*/ 1);
    const Layout & layout = store->layout();
    const RootNamespace a{"00/quiet_a@cas@"};
    const RootNamespace b{"00/quiet_b@cas@"};
    const RootNamespace c{"00/quiet_c@cas@"};

    for (const RootNamespace & ns : {a, b, c})
        publish(*backend, layout, ns, "ref_1", 1, DB::UInt128(0x300 + ns.string().size()));

    Gc gc(store, kGc);
    runRegularRoundReclaiming(gc);
    store->renewWatermarkOnce();
    for (const RootNamespace & ns : {a, b, c})
        ASSERT_NE(sealedCursorOf(*backend, layout, ns), (RefTxnId{})) << ns.string();

    /// All three go unhinted at once. Their valid checkpoint frontiers still prove their carried
    /// cursors, so this does not consume the successor-probe budget.
    for (const RootNamespace & ns : {a, b, c})
        backend->hidePrefix(layout.namespaceStreamPrefix(fixture::fixtureLife(ns)));

    std::map<String, UInt64> intake;
    gc.setPhaseSink([&](const GcPhaseRecord & rec)
    {
        if (rec.phase == "fold_ref_intake")
            intake = rec.metrics;
    });
    runRegularRoundReclaiming(gc);
    gc.setPhaseSink({});

    ASSERT_FALSE(intake.empty()) << "the intake phase must have emitted its row";
    EXPECT_EQ(intake["unhinted_quiet_walked"], 3u)
        << "a valid checkpoint frontier makes every quiet life eligible without a successor probe";
    EXPECT_EQ(intake["frontier_unprobed_budget"], 0u)
        << "the CTE authority, not the probe budget, decides these quiet lives";
    EXPECT_EQ(intake["frontier_proven"], 3u)
        << "each carried cursor equals its valid checkpoint frontier";
    EXPECT_EQ(intake["frontier_namespaces"], 3u)
        << "the denominator is the complete authoritative set of sealed quiet lives";
    EXPECT_EQ(intake["frontier_proven"], intake["frontier_namespaces"])
        << "a valid CTE frontier remains authoritative even when LIST omits every namespace";

    /// And the seal really does carry all three rows — the denominator's claim, checked against the
    /// object it describes rather than against another counter.
    for (const RootNamespace & ns : {a, b, c})
    {
        EXPECT_NE(sealedCursorOf(*backend, layout, ns), (RefTxnId{}))
            << "every namespace in the tally must have a sealed cursor: " << ns.string();
        const NamespaceLifeId life = *CasRefCatalog::lifeIfCataloged(*backend, layout, ns);
        const auto checkpoint = readCkpt(*backend, layout, life);
        ASSERT_TRUE(checkpoint.has_value());
        EXPECT_EQ(checkpoint->ckpt.committed_through, (RefTxnId{1, 1}))
            << "LIST omission and the probe budget do not alter a valid CTE";
    }
}

/// A checkpoint boundary already equal to the carried cursor proves a quiet catalog life complete;
/// GC must not manufacture a successor `GET` merely because its LIST is empty.
TEST(CasGcFrontierGate, AQuietKnownNamespaceAtItsCheckpointFrontierCostsNoSuccessorGet)
{
    auto backend = std::make_shared<CountingHintHoleBackend>();
    auto store = openPoolForTest(backend, /*gc_fold_max_defer_rounds=*/0);
    const Layout & layout = store->layout();
    const RootNamespace quiet{"00/quiet@cas@"};

    publish(*backend, layout, quiet, "ref_1", 1, DB::UInt128(0x11));
    replaceRecoverableCkptForRawFixture(*backend, layout, quiet, RefCkpt{
        .life_epoch = 1,
        .committed_through = RefTxnId{1, 1},
        .checkpoint_snapshot_id = std::nullopt,
        .last_epoch_seal = std::nullopt,
    });

    Gc gc(store, kGc);
    runRegularRoundReclaiming(gc);
    store->renewWatermarkOnce();

    const RefTxnId sealed = sealedCursorOf(*backend, layout, quiet);
    ASSERT_NE(sealed, (RefTxnId{})) << "the seeding round must have sealed a cursor to carry";

    /// Now the store stops listing the namespace entirely.
    backend->hidePrefix(layout.namespaceStreamPrefix(fixture::fixtureLife(quiet)));
    backend->resetCounts();
    std::map<String, UInt64> intake;
    gc.setPhaseSink([&](const GcPhaseRecord & rec)
    {
        if (rec.phase == "fold_ref_intake")
            intake = rec.metrics;
    });
    const RoundReport report = runRegularRoundReclaiming(gc);
    gc.setPhaseSink({});

    const String expected_next =
        layout.refLogKey(fixture::fixtureLife(quiet), RefTxnId{sealed.writer_epoch, sealed.ref_sequence + 1});
    EXPECT_EQ(backend->getCount(expected_next), 0u)
        << "the inclusive checkpoint boundary proves this quiet life without a successor probe";
    EXPECT_TRUE(report.anomalies.empty());
    ASSERT_FALSE(intake.empty());
    EXPECT_EQ(intake["frontier_proven"], intake["frontier_namespaces"])
        << "the inherited cursor already at the checkpoint boundary is destructive-eligible";
}

/// A checkpoint must never retreat below a sealed cursor. Its inclusive frontier can prove a cursor
/// already at that point, but cannot explain one that has advanced beyond it.
TEST(CasGcFrontierGate, CheckpointFrontierBehindAnInheritedCursorFailsClosed)
{
    auto backend = std::make_shared<CountingBackend>();
    auto store = openPoolForTest(backend, /*gc_fold_max_defer_rounds=*/0);
    const Layout & layout = store->layout();
    const RootNamespace ns{"00/checkpoint-behind-inherited-cursor@cas@"};

    fixture::admitLive(*backend, layout, ns);
    publish(*backend, layout, ns, "first", 1, DB::UInt128(0xfb));
    publish(*backend, layout, ns, "second", 2, DB::UInt128(0xfc));
    replaceRecoverableCkptForRawFixture(*backend, layout, ns, RefCkpt{
        .life_epoch = 1,
        .committed_through = RefTxnId{1, 2},
        .checkpoint_snapshot_id = std::nullopt,
        .last_epoch_seal = std::nullopt,
    });

    Gc gc(store, kGc);
    ASSERT_TRUE(runRegularRoundReclaiming(gc).acquired_lease);
    ASSERT_EQ(sealedCursorOf(*backend, layout, ns), (RefTxnId{1, 2}));

    const NamespaceLifeId life = *CasRefCatalog::lifeIfCataloged(*backend, layout, ns);
    const String checkpoint_key = layout.refCkptKey(life);
    const HeadResult checkpoint_head = backend->head(checkpoint_key);
    ASSERT_TRUE(checkpoint_head.exists);
    ASSERT_EQ(backend->putOverwrite(checkpoint_key, encodeRefCkpt(RefCkpt{
        .life_epoch = 1,
        .committed_through = RefTxnId{1, 1},
        .checkpoint_snapshot_id = std::nullopt,
        .last_epoch_seal = std::nullopt,
    }), checkpoint_head.token).outcome, PutOutcome::Done);

    std::map<String, UInt64> intake;
    gc.setPhaseSink([&](const GcPhaseRecord & rec)
    {
        if (rec.phase == "fold_ref_intake")
            intake = rec.metrics;
    });
    const RoundReport report = runRegularRoundReclaiming(gc);
    gc.setPhaseSink({});

    EXPECT_FALSE(report.anomalies.empty());
    ASSERT_FALSE(intake.empty());
    EXPECT_LT(intake["frontier_proven"], intake["frontier_namespaces"]);
}

/// A carried `EpochSeal` may have its authoritative successor in the next epoch. The arithmetic
/// successor in the sealed epoch is absent by design, so the exact checkpoint frontier must nominate
/// the shared seal-chain crossing before that absence is classified as a same-epoch gap.
TEST(CasGcFrontierGate, CheckpointFrontierCrossesAnInheritedEpochSeal)
{
    auto backend = std::make_shared<CountingBackend>();
    auto store = openPoolForTest(backend, /*gc_fold_max_defer_rounds=*/0);
    const Layout & layout = store->layout();
    const RootNamespace ns{"00/checkpoint-inherited-seal-crossing@cas@"};
    const DB::UInt128 crossed_blob(0xfd);

    fixture::admitLive(*backend, layout, ns);
    publishAt(*backend, layout, ns, RefTxnId{1, 1}, "birth", 1, DB::UInt128(0xfe), /*birth=*/true);
    writeSealAt(*backend, layout, ns, RefTxnId{1, 2});
    writeRecoverableCkptForRawFixture(*backend, layout, ns, RefCkpt{
        .life_epoch = 1,
        .committed_through = RefTxnId{1, 2},
        .checkpoint_snapshot_id = std::nullopt,
        .last_epoch_seal = RefTxnId{1, 2},
    });

    Gc gc(store, kGc);
    ASSERT_TRUE(runRegularRoundReclaiming(gc).acquired_lease);
    ASSERT_EQ(sealedCursorOf(*backend, layout, ns), (RefTxnId{1, 2}));

    publishAt(*backend, layout, ns, RefTxnId{2, 1}, "crossed", 2, crossed_blob,
              /*birth=*/false, /*prev_epoch_seal=*/RefTxnId{1, 2});
    const NamespaceLifeId life = *CasRefCatalog::lifeIfCataloged(*backend, layout, ns);
    const String checkpoint_key = layout.refCkptKey(life);
    const HeadResult checkpoint_head = backend->head(checkpoint_key);
    ASSERT_TRUE(checkpoint_head.exists);
    ASSERT_EQ(backend->putOverwrite(checkpoint_key, encodeRefCkpt(RefCkpt{
        .life_epoch = 1,
        .committed_through = RefTxnId{2, 1},
        .checkpoint_snapshot_id = std::nullopt,
        .last_epoch_seal = RefTxnId{1, 2},
    }), checkpoint_head.token).outcome, PutOutcome::Done);

    std::map<String, UInt64> intake;
    gc.setPhaseSink([&](const GcPhaseRecord & rec)
    {
        if (rec.phase == "fold_ref_intake")
            intake = rec.metrics;
    });
    const RoundReport report = runRegularRoundReclaiming(gc);
    gc.setPhaseSink({});

    EXPECT_TRUE(report.anomalies.empty());
    EXPECT_GT(inDegreeOf(*backend, layout, crossed_blob), 0);
    ASSERT_FALSE(intake.empty());
    EXPECT_EQ(intake["frontier_proven"], intake["frontier_namespaces"]);
}

/// The exact checkpoint successor must chain to the seal just consumed. Merely being in the next epoch
/// is insufficient: an incorrect predecessor would skip an unclosed history segment forever.
TEST(CasGcFrontierGate, CheckpointFrontierRejectsWrongPredecessorAfterFreshEpochSeal)
{
    auto backend = std::make_shared<CountingBackend>();
    auto store = openPoolForTest(backend, /*gc_fold_max_defer_rounds=*/0);
    const Layout & layout = store->layout();
    const RootNamespace ns{"00/checkpoint-wrong-fresh-seal-predecessor@cas@"};

    fixture::admitLive(*backend, layout, ns);
    publishAt(*backend, layout, ns, RefTxnId{1, 1}, "birth", 1, DB::UInt128(0xff), /*birth=*/true);
    writeSealAt(*backend, layout, ns, RefTxnId{1, 2});
    publishAt(*backend, layout, ns, RefTxnId{2, 1}, "wrong_predecessor", 2, DB::UInt128(0x100),
              /*birth=*/false, /*prev_epoch_seal=*/RefTxnId{1, 1});
    writeRecoverableCkptForRawFixture(*backend, layout, ns, RefCkpt{
        .life_epoch = 1,
        .committed_through = RefTxnId{2, 1},
        .checkpoint_snapshot_id = std::nullopt,
        .last_epoch_seal = RefTxnId{1, 2},
    });

    std::map<String, UInt64> intake;
    Gc gc(store, kGc);
    gc.setPhaseSink([&](const GcPhaseRecord & rec)
    {
        if (rec.phase == "fold_ref_intake")
            intake = rec.metrics;
    });
    const RoundReport report = runRegularRoundReclaiming(gc);
    gc.setPhaseSink({});

    EXPECT_FALSE(report.anomalies.empty());
    EXPECT_EQ(sealedCursorOf(*backend, layout, ns), (RefTxnId{1, 2}));
    ASSERT_FALSE(intake.empty());
    EXPECT_LT(intake["frontier_proven"], intake["frontier_namespaces"]);
}

/// A namespace that was WRONGLY quiet -- the hint hid a record that is durably there -- is walked this
/// round, not next: the probe finds the record and the walk continues from it.
TEST(CasGcFrontierGate, AWronglyQuietNamespaceIsWalkedTheSameRound)
{
    auto backend = std::make_shared<CountingHintHoleBackend>();
    auto store = openPoolForTest(backend, /*gc_fold_max_defer_rounds=*/0);
    const Layout & layout = store->layout();
    const RootNamespace quiet{"00/quiet@cas@"};
    const DB::UInt128 late_blob(0x77);

    publish(*backend, layout, quiet, "ref_1", 1, DB::UInt128(0x11));
    replaceRecoverableCkptForRawFixture(*backend, layout, quiet, RefCkpt{
        .life_epoch = 1,
        .committed_through = RefTxnId{1, 1},
        .checkpoint_snapshot_id = std::nullopt,
        .last_epoch_seal = std::nullopt,
    });

    Gc gc(store, kGc);
    runRegularRoundReclaiming(gc);
    store->renewWatermarkOnce();
    const RefTxnId sealed_before = sealedCursorOf(*backend, layout, quiet);

    /// A second publish lands, and the store hides the namespace from every LIST at the same moment.
    publish(*backend, layout, quiet, "ref_2", 2, late_blob);
    const NamespaceLifeId life = *CasRefCatalog::lifeIfCataloged(*backend, layout, quiet);
    const String checkpoint_key = layout.refCkptKey(life);
    const HeadResult checkpoint_head = backend->head(checkpoint_key);
    ASSERT_TRUE(checkpoint_head.exists);
    ASSERT_EQ(backend->putOverwrite(checkpoint_key, encodeRefCkpt(RefCkpt{
        .life_epoch = 1,
        .committed_through = RefTxnId{1, 2},
        .checkpoint_snapshot_id = std::nullopt,
        .last_epoch_seal = std::nullopt,
    }), checkpoint_head.token).outcome, PutOutcome::Done);
    backend->hidePrefix(layout.namespaceStreamPrefix(fixture::fixtureLife(quiet)));

    runRegularRoundReclaiming(gc);

    EXPECT_LT(sealed_before, sealedCursorOf(*backend, layout, quiet))
        << "the probe found the hidden record, so the walk folded it and the cursor advanced";
    EXPECT_GT(inDegreeOf(*backend, layout, late_blob), 0)
        << "the hidden publish's edge folded this round -- the hint never mentioned it";
}

/// The catalog life is grounded by its exact decoded `_ckpt`, not by the round's listing or a later
/// absent probe. A durable `F+1` is physically present but not committed history, so this fold may apply
/// only `F`; in particular it must not read `F+2`. Reaching `F` still proves the checkpoint-bounded
/// cut, so the physical successor cannot suppress otherwise eligible destructive work.
TEST(CasGcFrontierGate, CheckpointFrontierBoundsOrdinaryFoldBeforeDurableSuccessor)
{
    auto backend = std::make_shared<CountingBackend>();
    auto store = openPoolForTest(backend, /*gc_fold_max_defer_rounds=*/0);
    const Layout & layout = store->layout();
    const RootNamespace ns{"00/checkpoint-bounds-fold@cas@"};
    const DB::UInt128 committed_blob(0xf1);
    const DB::UInt128 beyond_frontier_blob(0xf2);

    fixture::admitLive(*backend, layout, ns);
    publish(*backend, layout, ns, "committed", 1, committed_blob);
    const ManifestRef uncommitted{.writer_epoch = 1, .build_sequence = 2, .manifest_ordinal = 1};
    writeBlobBody(*backend, layout, beyond_frontier_blob);
    writeManifestRaw(*backend, layout, ns, uncommitted, {blobEntryFor("data.bin", beyond_frontier_blob)});
    fixture::writeRefLogRaw(*backend, layout, RefLogTxn{
        .ns = ns.string(),
        .txn_id = RefTxnId{1, 2},
        .ops = publishCommittedOps("durable_but_uncommitted", uncommitted),
        .prev_epoch_seal = std::nullopt,
    });
    replaceRecoverableCkptForRawFixture(*backend, layout, ns, RefCkpt{
        .life_epoch = 1,
        .committed_through = RefTxnId{1, 1},
        .checkpoint_snapshot_id = std::nullopt,
        .last_epoch_seal = std::nullopt,
    });

    std::map<String, UInt64> intake;
    Gc gc(store, kGc);
    gc.setPhaseSink([&](const GcPhaseRecord & rec)
    {
        if (rec.phase == "fold_ref_intake")
            intake = rec.metrics;
    });
    backend->resetCounts();
    const RoundReport report = runRegularRoundReclaiming(gc);
    ASSERT_TRUE(report.acquired_lease);
    gc.setPhaseSink({});

    const NamespaceLifeId life = *CasRefCatalog::lifeIfCataloged(*backend, layout, ns);
    EXPECT_EQ(sealedCursorOf(*backend, layout, ns), (RefTxnId{1, 1}));
    EXPECT_EQ(inDegreeOf(*backend, layout, beyond_frontier_blob), 0)
        << "a durable log above `_ckpt.committed_through` is not foldable history";
    EXPECT_EQ(backend->getCount(layout.refLogKey(life, RefTxnId{1, 2})), 0u)
        << "the checkpoint frontier stops the walk before `F+1`";
    EXPECT_EQ(backend->getCount(layout.refLogKey(life, RefTxnId{1, 3})), 0u)
        << "a 404 above `F+1` must not authorize the destructive frontier";
    EXPECT_TRUE(report.anomalies.empty());
    ASSERT_FALSE(intake.empty());
    EXPECT_EQ(intake["frontier_proven"], intake["frontier_namespaces"])
        << "the consumed checkpoint frontier, not the physical successor, authorizes this cut";
}

/// With no physical successor at all, consuming the exact inclusive checkpoint frontier proves this
/// catalog life complete. This is the control for the same bounded-cut proof exercised with a durable
/// uncommitted successor above.
TEST(CasGcFrontierGate, ConsumedCheckpointFrontierProvesOrdinaryLifeWithoutSuccessor)
{
    auto backend = std::make_shared<CountingBackend>();
    auto store = openPoolForTest(backend, /*gc_fold_max_defer_rounds=*/0);
    const Layout & layout = store->layout();
    const RootNamespace ns{"00/checkpoint-complete-fold@cas@"};

    fixture::admitLive(*backend, layout, ns);
    publish(*backend, layout, ns, "committed", 1, DB::UInt128(0xf3));
    replaceRecoverableCkptForRawFixture(*backend, layout, ns, RefCkpt{
        .life_epoch = 1,
        .committed_through = RefTxnId{1, 1},
        .checkpoint_snapshot_id = std::nullopt,
        .last_epoch_seal = std::nullopt,
    });

    std::map<String, UInt64> intake;
    Gc gc(store, kGc);
    gc.setPhaseSink([&](const GcPhaseRecord & rec)
    {
        if (rec.phase == "fold_ref_intake")
            intake = rec.metrics;
    });
    backend->resetCounts();
    const RoundReport report = runRegularRoundReclaiming(gc);
    ASSERT_TRUE(report.acquired_lease);
    gc.setPhaseSink({});

    const NamespaceLifeId life = *CasRefCatalog::lifeIfCataloged(*backend, layout, ns);
    EXPECT_EQ(sealedCursorOf(*backend, layout, ns), (RefTxnId{1, 1}));
    EXPECT_EQ(backend->getCount(layout.refLogKey(life, RefTxnId{1, 2})), 0u)
        << "the checkpoint boundary proves the cut without a post-frontier 404";
    EXPECT_TRUE(report.anomalies.empty());
    ASSERT_FALSE(intake.empty());
    EXPECT_EQ(intake["frontier_proven"], intake["frontier_namespaces"]);
}

/// The same cut remains complete when the durable uncommitted successor is hidden from every LIST.
/// Exact reads still serve that successor, but the checkpoint ceiling must leave it untouched and must
/// not let the list omission suppress the checkpoint-bounded destructive path.
TEST(CasGcFrontierGate, CheckpointFrontierProvesLifeWithHiddenDurableSuccessor)
{
    auto backend = std::make_shared<CountingHintHoleBackend>();
    auto store = openPoolForTest(backend, /*gc_fold_max_defer_rounds=*/0);
    const Layout & layout = store->layout();
    const RootNamespace ns{"00/checkpoint-hidden-successor@cas@"};
    const DB::UInt128 beyond_frontier_blob(0xf4);

    fixture::admitLive(*backend, layout, ns);
    publish(*backend, layout, ns, "committed", 1, DB::UInt128(0xf5));
    const ManifestRef uncommitted{.writer_epoch = 1, .build_sequence = 2, .manifest_ordinal = 1};
    writeBlobBody(*backend, layout, beyond_frontier_blob);
    writeManifestRaw(*backend, layout, ns, uncommitted, {blobEntryFor("data.bin", beyond_frontier_blob)});
    fixture::writeRefLogRaw(*backend, layout, RefLogTxn{
        .ns = ns.string(),
        .txn_id = RefTxnId{1, 2},
        .ops = publishCommittedOps("hidden_durable_but_uncommitted", uncommitted),
        .prev_epoch_seal = std::nullopt,
    });
    replaceRecoverableCkptForRawFixture(*backend, layout, ns, RefCkpt{
        .life_epoch = 1,
        .committed_through = RefTxnId{1, 1},
        .checkpoint_snapshot_id = std::nullopt,
        .last_epoch_seal = std::nullopt,
    });

    const NamespaceLifeId life = *CasRefCatalog::lifeIfCataloged(*backend, layout, ns);
    backend->hide(layout.refLogKey(life, RefTxnId{1, 2}));

    std::map<String, UInt64> intake;
    Gc gc(store, kGc);
    gc.setPhaseSink([&](const GcPhaseRecord & rec)
    {
        if (rec.phase == "fold_ref_intake")
            intake = rec.metrics;
    });
    backend->resetCounts();
    const RoundReport report = runRegularRoundReclaiming(gc);
    ASSERT_TRUE(report.acquired_lease);
    gc.setPhaseSink({});

    EXPECT_GT(backend->holesServed(), 0u) << "the F+1 log must really be hidden from LIST";
    EXPECT_EQ(sealedCursorOf(*backend, layout, ns), (RefTxnId{1, 1}));
    EXPECT_EQ(inDegreeOf(*backend, layout, beyond_frontier_blob), 0);
    EXPECT_EQ(backend->getCount(layout.refLogKey(life, RefTxnId{1, 2})), 0u)
        << "the hidden durable successor is outside the checkpoint cut";
    EXPECT_TRUE(report.anomalies.empty());
    ASSERT_FALSE(intake.empty());
    EXPECT_EQ(intake["frontier_proven"], intake["frontier_namespaces"]);
}

/// The checkpoint's inclusive endpoint is itself a durable witness. If that exact log is absent,
/// the namespace is corrupt rather than complete; a 404 at the endpoint must not authorize cleanup.
TEST(CasGcFrontierGate, MissingCommittedCheckpointLogHoldsInsteadOfProvingTheFrontier)
{
    auto backend = std::make_shared<CountingBackend>();
    auto store = openPoolForTest(backend, /*gc_fold_max_defer_rounds=*/0);
    const Layout & layout = store->layout();
    const RootNamespace ns{"00/missing-committed-checkpoint-log@cas@"};

    fixture::admitLive(*backend, layout, ns);
    publish(*backend, layout, ns, "first", 1, DB::UInt128(0xf6));
    publish(*backend, layout, ns, "missing_but_committed", 2, DB::UInt128(0xf7));
    replaceRecoverableCkptForRawFixture(*backend, layout, ns, RefCkpt{
        .life_epoch = 1,
        .committed_through = RefTxnId{1, 2},
        .checkpoint_snapshot_id = std::nullopt,
        .last_epoch_seal = std::nullopt,
    });

    const NamespaceLifeId life = *CasRefCatalog::lifeIfCataloged(*backend, layout, ns);
    const String missing_key = layout.refLogKey(life, RefTxnId{1, 2});
    const HeadResult missing_head = backend->head(missing_key);
    ASSERT_TRUE(missing_head.exists);
    ASSERT_EQ(backend->deleteExact(missing_key, missing_head.token).kind, DeleteOutcome::Kind::Deleted);

    std::map<String, UInt64> intake;
    Gc gc(store, kGc);
    gc.setPhaseSink([&](const GcPhaseRecord & rec)
    {
        if (rec.phase == "fold_ref_intake")
            intake = rec.metrics;
    });
    const RoundReport report = runRegularRoundReclaiming(gc);
    ASSERT_TRUE(report.acquired_lease);
    gc.setPhaseSink({});

    EXPECT_EQ(sealedCursorOf(*backend, layout, ns), (RefTxnId{1, 1}));
    EXPECT_FALSE(report.anomalies.empty()) << "the missing committed checkpoint record is corruption";
    ASSERT_FALSE(intake.empty());
    EXPECT_LT(intake["frontier_proven"], intake["frontier_namespaces"]);
}

/// A checkpoint may name a durable record that the round's LIST omitted. Exact GETs must still fold
/// that committed record; the frozen list tail is only a scheduling hint, never a history boundary.
TEST(CasGcFrontierGate, HiddenCommittedCheckpointLogIsFoldedThroughTheAuthorityCeiling)
{
    auto backend = std::make_shared<CountingHintHoleBackend>();
    auto store = openPoolForTest(backend, /*gc_fold_max_defer_rounds=*/0);
    const Layout & layout = store->layout();
    const RootNamespace ns{"00/hidden-committed-checkpoint-log@cas@"};
    const DB::UInt128 hidden_blob(0xf8);

    fixture::admitLive(*backend, layout, ns);
    publish(*backend, layout, ns, "first", 1, DB::UInt128(0xf9));
    publish(*backend, layout, ns, "hidden_but_committed", 2, hidden_blob);
    replaceRecoverableCkptForRawFixture(*backend, layout, ns, RefCkpt{
        .life_epoch = 1,
        .committed_through = RefTxnId{1, 2},
        .checkpoint_snapshot_id = std::nullopt,
        .last_epoch_seal = std::nullopt,
    });

    const NamespaceLifeId life = *CasRefCatalog::lifeIfCataloged(*backend, layout, ns);
    backend->hide(layout.refLogKey(life, RefTxnId{1, 2}));

    std::map<String, UInt64> intake;
    Gc gc(store, kGc);
    gc.setPhaseSink([&](const GcPhaseRecord & rec)
    {
        if (rec.phase == "fold_ref_intake")
            intake = rec.metrics;
    });
    const RoundReport report = runRegularRoundReclaiming(gc);
    ASSERT_TRUE(report.acquired_lease);
    gc.setPhaseSink({});

    EXPECT_GT(backend->holesServed(), 0u) << "the committed endpoint must really be omitted from LIST";
    EXPECT_EQ(sealedCursorOf(*backend, layout, ns), (RefTxnId{1, 2}));
    EXPECT_GT(inDegreeOf(*backend, layout, hidden_blob), 0);
    EXPECT_TRUE(report.anomalies.empty());
    ASSERT_FALSE(intake.empty());
    EXPECT_EQ(intake["frontier_proven"], intake["frontier_namespaces"]);
}

/// A valid checkpoint with no committed record is an authoritative empty history. It is complete for
/// a never-folded life without probing a fabricated first transaction.
TEST(CasGcFrontierGate, EmptyCheckpointFrontierProvesAnUnfoldedLife)
{
    auto backend = std::make_shared<CountingBackend>();
    auto store = openPoolForTest(backend, /*gc_fold_max_defer_rounds=*/0);
    const Layout & layout = store->layout();
    const RootNamespace ns{"00/empty-checkpoint-frontier@cas@"};

    casAdmitRecoverableEntry(*backend, layout, ns);

    std::map<String, UInt64> intake;
    Gc gc(store, kGc);
    gc.setPhaseSink([&](const GcPhaseRecord & rec)
    {
        if (rec.phase == "fold_ref_intake")
            intake = rec.metrics;
    });
    const RoundReport report = runRegularRoundReclaiming(gc);
    ASSERT_TRUE(report.acquired_lease);
    gc.setPhaseSink({});

    EXPECT_TRUE(report.anomalies.empty());
    ASSERT_FALSE(intake.empty());
    EXPECT_EQ(intake["frontier_proven"], intake["frontier_namespaces"]);
}

/// Empty history cannot explain an inherited cursor. An operator-corrupted checkpoint that erases its
/// own committed boundary must clamp the life rather than silently authorize destruction.
TEST(CasGcFrontierGate, EmptyCheckpointFrontierRejectsAnInheritedCursor)
{
    auto backend = std::make_shared<CountingBackend>();
    auto store = openPoolForTest(backend, /*gc_fold_max_defer_rounds=*/0);
    const Layout & layout = store->layout();
    const RootNamespace ns{"00/empty-checkpoint-after-cursor@cas@"};

    fixture::admitLive(*backend, layout, ns);
    publish(*backend, layout, ns, "first", 1, DB::UInt128(0xfa));
    replaceRecoverableCkptForRawFixture(*backend, layout, ns, RefCkpt{
        .life_epoch = 1,
        .committed_through = RefTxnId{1, 1},
        .checkpoint_snapshot_id = std::nullopt,
        .last_epoch_seal = std::nullopt,
    });

    Gc gc(store, kGc);
    ASSERT_TRUE(runRegularRoundReclaiming(gc).acquired_lease);
    ASSERT_EQ(sealedCursorOf(*backend, layout, ns), (RefTxnId{1, 1}));

    const NamespaceLifeId life = *CasRefCatalog::lifeIfCataloged(*backend, layout, ns);
    const String checkpoint_key = layout.refCkptKey(life);
    const HeadResult checkpoint_head = backend->head(checkpoint_key);
    ASSERT_TRUE(checkpoint_head.exists);
    ASSERT_EQ(backend->putOverwrite(checkpoint_key, encodeRefCkpt(RefCkpt{
        .life_epoch = 1,
        .committed_through = std::nullopt,
        .checkpoint_snapshot_id = std::nullopt,
        .last_epoch_seal = std::nullopt,
    }), checkpoint_head.token).outcome, PutOutcome::Done);

    std::map<String, UInt64> intake;
    gc.setPhaseSink([&](const GcPhaseRecord & rec)
    {
        if (rec.phase == "fold_ref_intake")
            intake = rec.metrics;
    });
    const RoundReport report = runRegularRoundReclaiming(gc);
    ASSERT_TRUE(report.acquired_lease);
    gc.setPhaseSink({});

    EXPECT_FALSE(report.anomalies.empty()) << "an empty checkpoint cannot explain a nonzero cursor";
    ASSERT_FALSE(intake.empty());
    EXPECT_LT(intake["frontier_proven"], intake["frontier_namespaces"]);
}

/// A catalog `Live` life without its exact checkpoint cannot derive either its genesis or a frontier
/// from the ref LIST. Even a durable listed first log must be retained until the authority is repaired.
TEST(CasGcFrontierGate, CatalogLifeWithoutCheckpointDefersWithoutUsingListedFrontier)
{
    auto backend = std::make_shared<CountingBackend>();
    auto store = openPoolForTest(backend, /*gc_fold_max_defer_rounds=*/0);
    const Layout & layout = store->layout();
    const RootNamespace ns{"00/missing-checkpoint-fold@cas@"};
    const DB::UInt128 blob(0xc7);

    fixture::admitLive(*backend, layout, ns);
    const ManifestRef manifest{.writer_epoch = 1, .build_sequence = 1, .manifest_ordinal = 1};
    writeBlobBody(*backend, layout, blob);
    writeManifestRaw(*backend, layout, ns, manifest, {blobEntryFor("data.bin", blob)});
    appendRefLogSeed(*backend, layout, ns, publishCommittedOps("must_remain_unfolded", manifest));

    const NamespaceLifeId life = *CasRefCatalog::lifeIfCataloged(*backend, layout, ns);
    ASSERT_FALSE(readCkpt(*backend, layout, life).has_value());

    std::map<String, UInt64> intake;
    Gc gc(store, kGc);
    gc.setPhaseSink([&](const GcPhaseRecord & rec)
    {
        if (rec.phase == "fold_ref_intake")
            intake = rec.metrics;
    });
    backend->resetCounts();
    ASSERT_TRUE(runRegularRoundReclaiming(gc).acquired_lease);
    gc.setPhaseSink({});

    EXPECT_EQ(foldCursorOf(*backend, layout, ns, /*shard=*/0), 0u);
    EXPECT_EQ(inDegreeOf(*backend, layout, blob), 0)
        << "a missing checkpoint must defer rather than fold the listed log";
    EXPECT_EQ(backend->getCount(layout.refLogKey(life, RefTxnId{1, 1})), 0u)
        << "the listed log is not authority for a checkpoint-less catalog life";
    EXPECT_EQ(backend->getCount(layout.refLogKey(life, RefTxnId{1, 2})), 0u)
        << "the next 404 is not authority for a checkpoint-less catalog life";
    EXPECT_EQ(backend->deleteTotal(), 0u) << deletedKeysMessage(*backend);
    ASSERT_FALSE(intake.empty());
    EXPECT_LT(intake["frontier_proven"], intake["frontier_namespaces"]);
}

/// A valid checkpoint frontier proves a quiet unhinted life without spending the successor-probe budget.
/// A zero budget therefore cannot suppress unrelated destructive work merely because this life is absent
/// from LIST.
TEST(CasGcFrontierGate, AnExhaustedProbeBudgetSealsCursorsAndDeletesNothing)
{
    auto backend = std::make_shared<CountingHintHoleBackend>();
    auto store = openPoolWithProbeBudget(backend, /*budget*/ 0);
    const Layout & layout = store->layout();
    const RootNamespace quiet{"00/quiet@cas@"};
    const RootNamespace busy{"00/busy@cas@"};
    const DB::UInt128 blob(0xbeef);

    publish(*backend, layout, quiet, "quiet_ref", 1, DB::UInt128(0x11));
    const ManifestRef mref = publish(*backend, layout, busy, "busy_ref", 2, blob);

    Gc gc(store, kGc);
    runRegularRoundReclaiming(gc);
    store->renewWatermarkOnce();
    const RefTxnId quiet_cursor = sealedCursorOf(*backend, layout, quiet);
    ASSERT_NE(quiet_cursor, (RefTxnId{}));

    /// The quiet namespace goes unhinted and the budget is zero. Its CTE still proves the carried
    /// cursor, while the busy namespace drops its ref and may proceed through reclamation.
    backend->hidePrefix(layout.namespaceStreamPrefix(fixture::fixtureLife(quiet)));
    dropRefTransition(*backend, layout, busy, "busy_ref", mref);

    backend->resetCounts();
    drive(store, gc, /*rounds*/ 5, UniversePolicy::AuthoritativeForTest);

    EXPECT_GT(backend->deleteTotal(), 0u)
        << "the quiet life's checkpoint authority leaves unrelated deletion eligible";
    EXPECT_FALSE(backend->head(blobKeyOf(layout, blob)).exists)
        << "the busy life's removal remains reclaimable despite the quiet LIST omission";
    EXPECT_EQ(sealedCursorOf(*backend, layout, quiet), quiet_cursor)
        << "the unprobed namespace's cursor rides verbatim -- it is never dropped";
    const NamespaceLifeId quiet_life = *CasRefCatalog::lifeIfCataloged(*backend, layout, quiet);
    const auto quiet_checkpoint = readCkpt(*backend, layout, quiet_life);
    ASSERT_TRUE(quiet_checkpoint.has_value());
    EXPECT_EQ(quiet_checkpoint->ckpt.committed_through, quiet_cursor)
        << "the quiet life's valid CTE is unaffected by LIST omission and a zero probe budget";
    EXPECT_GT(decodeGcState(backend->get(layout.gcStateKey())->bytes).round, 1u)
        << "the round still commits; only its destructive half is withheld";
}

/// ===================== A COMMITTED GAP IS REDETECTED UNTIL REPAIRED =====================
///
/// A hold's committed checkpoint frontier remains a durable witness of its own gap. Hiding the later
/// log from LIST cannot make that gap quiet: every retry exact-reads the missing position, redetects the
/// hold, and suppresses destructive work until an operator repairs the record stream.
TEST(CasGcFrontierGate, ACommittedGapIsRedetectedAndSuppressesEveryRound)
{
    auto backend = std::make_shared<CountingHintHoleBackend>();
    auto store = openPoolForTest(backend, /*gc_fold_max_defer_rounds=*/0);
    const Layout & layout = store->layout();
    const RootNamespace held{"00/held@cas@"};
    const RootNamespace busy{"00/busy@cas@"};
    const DB::UInt128 blob(0xbeef);

    /// {1,3} never existed while {1,4} is durable and listed.
    publish(*backend, layout, held, "ref_1", 1, DB::UInt128(0x21));
    publish(*backend, layout, held, "ref_2", 2, DB::UInt128(0x22));
    const ManifestRef orphan_ref{.writer_epoch = 1, .build_sequence = 4, .manifest_ordinal = 1};
    writeBlobBody(*backend, layout, DB::UInt128(0x24));
    writeManifestRaw(*backend, layout, held, orphan_ref, {blobEntryFor("data.bin", DB::UInt128(0x24))});
    RefLogTxn txn;
    txn.ns = held.string();
    txn.txn_id = RefTxnId{1, 4};
    txn.ops = publishCommittedOps("ref_4", orphan_ref);
    fixture::writeRefLogRaw(*backend, layout, txn);
    replaceRecoverableCkptForRawFixture(*backend, layout, held, RefCkpt{
        .life_epoch = 1,
        .committed_through = RefTxnId{1, 4},
        .checkpoint_snapshot_id = std::nullopt,
        .last_epoch_seal = std::nullopt,
    });

    Gc gc(store, kGc);
    std::map<String, UInt64> first_intake;
    gc.setPhaseSink([&](const GcPhaseRecord & rec)
    {
        if (rec.phase == "fold_ref_intake")
            first_intake = rec.metrics;
    });
    const RoundReport first_round = runRegularRoundReclaiming(gc);
    gc.setPhaseSink({});
    store->renewWatermarkOnce();
    ASSERT_EQ(sealedCursorOf(*backend, layout, held), (RefTxnId{1, 2}))
        << "round 1 must stop below the gap and hold there";
    ASSERT_FALSE(first_intake.empty());
    EXPECT_GT(first_intake["tables_clamped"], 0u);
    EXPECT_GT(first_intake["tables_held"], 0u);
    EXPECT_FALSE(first_round.anomalies.empty());

    const NamespaceLifeId held_life = *CasRefCatalog::lifeIfCataloged(*backend, layout, held);
    const auto held_checkpoint = readCkpt(*backend, layout, held_life);
    ASSERT_TRUE(held_checkpoint.has_value());
    EXPECT_EQ(held_checkpoint->ckpt.committed_through, (RefTxnId{1, 4}));

    /// Hiding `{1,4}` from LIST does not hide the committed CTE frontier. The next round exact-reads
    /// the missing `{1,3}`, re-detects the gap, and seals a fresh hold.
    backend->hidePrefix(layout.refLogKey(fixture::fixtureLife(held), RefTxnId{1, 4}));

    /// Meanwhile a blob elsewhere becomes condemnable, so the round has real destructive work to decline.
    const ManifestRef mref = publish(*backend, layout, busy, "busy_ref", 9, blob);
    dropRefTransition(*backend, layout, busy, "busy_ref", mref);

    backend->resetCounts();
    std::map<String, UInt64> second_intake;
    gc.setPhaseSink([&](const GcPhaseRecord & rec)
    {
        if (rec.phase == "fold_ref_intake")
            second_intake = rec.metrics;
    });
    const RoundReport second_round = runRegularRoundReclaiming(gc);
    gc.setPhaseSink({});
    store->renewWatermarkOnce();

    ASSERT_FALSE(second_intake.empty());
    EXPECT_GT(second_intake["tables_clamped"], 0u)
        << "the committed `{1,4}` frontier is a durable witness that re-detects the missing `{1,3}`";
    EXPECT_GT(second_intake["tables_held"], 0u)
        << "the fresh clamp preserves the unresolved hold in the next sealed coverage";
    EXPECT_FALSE(second_round.anomalies.empty());

    drive(store, gc, /*rounds*/ 4, UniversePolicy::AuthoritativeForTest);

    EXPECT_EQ(backend->deleteTotal(), 0u)
        << "the re-detected committed gap suppresses each round's destructive work. "
           "Deleted:" << deletedKeysMessage(*backend);
    EXPECT_TRUE(backend->head(blobKeyOf(layout, blob)).exists);
    EXPECT_EQ(sealedCursorOf(*backend, layout, held), (RefTxnId{1, 2}))
        << "the committed gap remains unresolved and the cursor cannot advance through it";
    const auto final_checkpoint = readCkpt(*backend, layout, held_life);
    ASSERT_TRUE(final_checkpoint.has_value());
    EXPECT_EQ(final_checkpoint->ckpt.committed_through, (RefTxnId{1, 4}));
}

/// ===================== THE TEMPORAL LEMMA, ALL THREE ARMS =====================
///
/// The gate says WHEN a round may destroy. These say that even a round which may destroy cannot
/// destroy a blob some edge still owns, over the three interleavings that matter.

/// ARM (a): a `+1` that lands after this round's probes and is followed by the SAME round's
/// condemnation. Round pacing makes it safe on its own: an entry condemned at round K cannot graduate
/// before K+1 and cannot be deleted before K+2, so the round that condemns never deletes.
TEST(CasGcFrontierGate, ABlobCondemnedThisRoundIsNeverDeletedThisRound)
{
    auto backend = std::make_shared<CountingBackend>();
    auto store = openPoolForTest(backend, /*gc_fold_max_defer_rounds=*/0);
    const Layout & layout = store->layout();
    const RootNamespace ns{"00/aa@cas@"};
    const DB::UInt128 blob(0xc04d);

    const ManifestRef mref = publish(*backend, layout, ns, "ref_1", 1, blob);
    Gc gc(store, kGc);
    runRegularRoundReclaiming(gc);
    store->renewWatermarkOnce();

    dropRefTransition(*backend, layout, ns, "ref_1", mref);
    backend->resetCounts();
    runRegularRoundReclaiming(gc);

    EXPECT_TRUE(backend->head(blobKeyOf(layout, blob)).exists)
        << "the condemning round must not also delete";
    EXPECT_EQ(backend->deleteCount(blobKeyOf(layout, blob)), 0u)
        << "not merely still present -- the delete was never attempted";
}

/// ARM (c) of the temporal lemma is the delete-site in-degree re-read, and it is NORMATIVE (spec §5,
/// third arm): an edge folded AFTER the condemnation but BEFORE the delete pass spares the blob
/// outright, `indeg > 0` winning over `delete_pending` past the floor. The other two arms bound WHEN
/// and WHAT a delete may remove; only this one asks whether the blob is still referenced at the moment
/// the pass decides.
TEST(CasGcFrontierGate, ALateEdgeSparesADeletePendingBlobAtTheDeleteSite)
{
    auto backend = std::make_shared<CountingBackend>();
    auto store = openPoolForTest(backend, /*gc_fold_max_defer_rounds=*/0);
    const Layout & layout = store->layout();
    const RootNamespace ns{"00/aa@cas@"};
    const DB::UInt128 blob(0x1a7e);

    const ManifestRef mref = publish(*backend, layout, ns, "ref_1", 1, blob);
    Gc gc(store, kGc);
    runRegularRoundReclaiming(gc);
    store->renewWatermarkOnce();

    /// Condemn it, then graduate it to delete_pending.
    dropRefTransition(*backend, layout, ns, "ref_1", mref);
    runRegularRoundReclaiming(gc);            /// condemn
    store->renewWatermarkOnce();
    runRegularRoundReclaiming(gc);            /// graduate: delete_pending published
    store->renewWatermarkOnce();

    /// A new owner appears BEFORE the delete pass. The pass recomputes the in-degree from the merge it
    /// just ran and finds it nonzero.
    const ManifestRef revived{.writer_epoch = 1, .build_sequence = 42, .manifest_ordinal = 1};
    writeManifestRaw(*backend, layout, ns, revived, {blobEntryFor("data.bin", blob)});
    publishCommittedTransition(*backend, layout, ns, "revived_ref", std::nullopt, revived);

    runRegularRoundReclaiming(gc);
    store->renewWatermarkOnce();

    EXPECT_TRUE(backend->head(blobKeyOf(layout, blob)).exists)
        << "the delete-site in-degree re-read spares a blob a fresh edge re-referenced";
    EXPECT_GT(inDegreeOf(*backend, layout, blob), 0);
}

/// ARM (b): a TOKENED adoption of an already-delete-pending blob. The writer's admit gate reads the
/// `Condemned` meta, refuses to adopt the dying incarnation, and rematerializes from its own source as
/// a FRESH incarnation -- so the delayed exact-token delete the previous round published finds a
/// different token and removes nothing. The blob's identity is preserved by re-upload, never by
/// reviving the condemned object.
TEST(CasGcFrontierGate, AResurrectedIncarnationSurvivesTheDelayedStaleTokenDelete)
{
    ensureBlobUploadPoolForTest();
    ensureCondemnedUploadAdmissionForTest();

    auto backend = std::make_shared<CountingBackend>();
    auto store = openPoolForTest(backend, /*gc_fold_max_defer_rounds=*/0);
    const Layout & layout = store->layout();
    const RootNamespace ns{"00/aa@cas@"};

    /// A REAL content-addressed blob, so the writer path below addresses exactly the object GC condemns.
    const String payload = "frontier-gate-resurrect-payload";
    const DB::UInt128 hash = u128Of(payload);
    const BlobRef id = idOf(payload);
    const String key = layout.blobKey(id);
    String raw_body(store->poolMeta().blob_header_len, '\0');
    raw_body += payload;
    writeRawBlobBody(*backend, layout, hash, raw_body);

    /// Publish and drop it so GC condemns and then graduates it to delete_pending.
    const ManifestRef mref{.writer_epoch = 1, .build_sequence = 1, .manifest_ordinal = 1};
    writeManifestRaw(*backend, layout, ns, mref, {blobEntryFor("data.bin", hash)});
    publishCommittedTransition(*backend, layout, ns, "ref_1", std::nullopt, mref);

    Gc gc(store, kGc);
    runRegularRoundReclaiming(gc);
    store->renewWatermarkOnce();
    dropRefTransition(*backend, layout, ns, "ref_1", mref);
    runRegularRoundReclaiming(gc);            /// condemn: writes the durable Condemned meta
    store->renewWatermarkOnce();
    runRegularRoundReclaiming(gc);            /// graduate: publishes delete_pending against THIS token
    store->renewWatermarkOnce();

    const Token condemned_token = backend->head(key).token;
    const auto condemned_meta = loadMetaForTest(*backend, layout, hash);
    ASSERT_TRUE(condemned_meta.has_value());
    ASSERT_EQ(condemned_meta->meta.state, MetaState::Condemned)
        << "the delete GC is about to execute must be backed by durable Condemned evidence";

    /// A writer now adopts the blob through the REAL admit gate. It point-reads the Condemned meta,
    /// refuses to adopt the dying incarnation, and rematerializes from its OWN source bytes -- never by
    /// reading the condemned object. The key ends up holding a DIFFERENT incarnation.
    auto build = store->beginPartWrite({});
    const PutBlobResult uploaded = build->putBlob(id, BlobSource::fromString(payload));
    EXPECT_EQ(uploaded.ref, id);
    const Token fresh_token = backend->head(key).token;
    ASSERT_NE(fresh_token, condemned_token) << "a resurrect must displace the condemned incarnation";

    /// GC's delayed delete still names the OLD token. It cannot touch the new object.
    drive(store, gc, /*rounds*/ 2, UniversePolicy::AuthoritativeForTest);

    ASSERT_TRUE(backend->head(key).exists)
        << "the resurrected incarnation survives the delete published against its predecessor";
    EXPECT_EQ(backend->head(key).token, fresh_token) << "and it is still the writer's incarnation";
    EXPECT_EQ(backend->deleteExact(key, condemned_token).kind, DeleteOutcome::Kind::TokenMismatch)
        << "the condemned token can never remove the fresh object (INV-NO-RETURN)";
}

/// ARM (c): a TOKENLESS relink -- the receiver adopts by evidence, holding no token at all. Safety
/// then rests entirely on ORDER, so the operation journal has to show it: the receiver's `+1` is
/// durable BEFORE the source releases its own committed edge, and no point in the schedule leaves the
/// blob with zero durable owners.
TEST(CasGcFrontierGate, ATokenlessRelinkMakesTheReceiverEdgeDurableBeforeTheSourceReleases)
{
    auto backend = std::make_shared<CountingBackend>();
    auto store = openPoolForTest(backend, /*gc_fold_max_defer_rounds=*/0);
    const Layout & layout = store->layout();
    const RootNamespace source{"00/source@cas@"};
    const RootNamespace receiver{"00/receiver@cas@"};
    const DB::UInt128 blob(0x8e11);

    const ManifestRef source_ref = publish(*backend, layout, source, "part_1", 1, blob);
    Gc gc(store, kGc);
    runRegularRoundReclaiming(gc);
    store->renewWatermarkOnce();
    ASSERT_GT(inDegreeOf(*backend, layout, blob), 0);

    /// The relink, in the only order the protocol permits: the receiver's manifest body and its
    /// committed edge first (tokenless -- it never HEADs the blob), and only afterwards the source's
    /// removal. Between the two writes the blob has TWO durable owners; it never has zero.
    const ManifestRef receiver_ref{.writer_epoch = 1, .build_sequence = 5, .manifest_ordinal = 1};
    writeManifestRaw(*backend, layout, receiver, receiver_ref, {blobEntryFor("data.bin", blob)});
    publishCommittedTransition(*backend, layout, receiver, "part_1", std::nullopt, receiver_ref);

    /// The round that observes ONLY the receiver's `+1` -- the exact midpoint of the schedule.
    runRegularRoundReclaiming(gc);
    store->renewWatermarkOnce();
    EXPECT_GE(inDegreeOf(*backend, layout, blob), 2)
        << "at the midpoint both owners are durable; the handoff never dips to zero";

    dropRefTransition(*backend, layout, source, "part_1", source_ref);
    drive(store, gc, /*rounds*/ 4, UniversePolicy::AuthoritativeForTest);

    EXPECT_TRUE(backend->head(blobKeyOf(layout, blob)).exists)
        << "the source released its edge only after the receiver's was durable, so nothing may collect it";
    EXPECT_EQ(inDegreeOf(*backend, layout, blob), 1)
        << "the receiver is the sole remaining owner";
}

/// ===================== CLEANUP RANGES ARE COMPUTED, NOT ENUMERATED =====================
///
/// `planRefCleanup` is pure, so the boundary arithmetic is pinned directly rather than inferred from a
/// round's side effects. Its sole coverage authority is the checkpoint-named base; a listed snapshot
/// is merely a physical observation until the same-id triple has been validated.

TEST(CasGcFrontierGateCleanupRange, CoveredLogsStopAtTheMinimumOfCheckpointAndCursor)
{
    RefTableListing listing;
    listing.logs = {{1, 1}, {1, 2}, {1, 3}, {1, 4}, {1, 5}};
    listing.snapshots = {{1, 5}};

    /// No checkpoint means no recovery base at all. A snapshot PUT that has not reached the `_ckpt`
    /// CAS must retain every listed object.
    const RefCleanupPlan without = planRefCleanup(listing, RefTxnId{1, 4});
    EXPECT_TRUE(without.deletable_logs.empty());
    EXPECT_TRUE(without.deletable_snapshots.empty());

    /// A checkpoint BELOW the cursor tightens it to {1,2}. Its exact `_log` witness must survive,
    /// so cleanup may remove only the strictly older entry.
    const RefCleanupPlan with = planRefCleanup(listing, RefTxnId{1, 4}, RefTxnId{1, 2});
    EXPECT_EQ(with.deletable_logs, (std::vector<RefTxnId>{{1, 1}}))
        << "the checkpoint witness and everything above it must survive";

    /// Once validation has established a later checkpoint base, its earlier covered history is
    /// reclaimable even if the hot fold cursor has not yet reached that base.
    const RefCleanupPlan ahead = planRefCleanup(listing, RefTxnId{1, 4}, RefTxnId{1, 9});
    EXPECT_EQ(ahead.deletable_logs, (std::vector<RefTxnId>{{1, 1}, {1, 2}, {1, 3}, {1, 4}}));
    EXPECT_EQ(ahead.deletable_snapshots, (std::vector<RefTxnId>{{1, 5}}));
}

TEST(CasGcFrontierGateCleanupRange, ASnapshotAtTheCheckpointSurvivesAndOnlyStrictlyOlderOnesGo)
{
    RefTableListing listing;
    listing.logs = {{1, 1}, {1, 2}, {1, 3}};
    listing.snapshots = {{1, 1}, {1, 2}, {1, 3}};

    /// A LIST-only newest snapshot is never a cleanup boundary.
    const RefCleanupPlan without = planRefCleanup(listing, RefTxnId{1, 3});
    EXPECT_TRUE(without.deletable_snapshots.empty());

    /// With the checkpoint AT {1,2}, only {1,1} is strictly below it. The snapshot the checkpoint names
    /// is the one a recovering reader samples, so it must survive its own cleanup.
    const RefCleanupPlan with = planRefCleanup(listing, RefTxnId{1, 3}, RefTxnId{1, 2});
    EXPECT_EQ(with.deletable_snapshots, (std::vector<RefTxnId>{{1, 1}}));

    /// The oldest checkpoint deletes nothing at all.
    const RefCleanupPlan oldest = planRefCleanup(listing, RefTxnId{1, 3}, RefTxnId{1, 1});
    EXPECT_TRUE(oldest.deletable_snapshots.empty());
}

/// Cleanup shares recovery's validator rather than inferring its own authority from a LIST. The
/// missing-base case is the no-checkpoint range above; the three physical triple failures below must
/// each reject exactly the checkpoint-named candidate.
TEST(CasGcFrontierGateCleanupRange, CheckpointBaseValidatorRejectsMissingLogSnapshotAndSeal)
{
    auto backend = std::make_shared<InMemoryBackend>();
    const Layout layout{"p"};
    const RefTxnId base{1, 1};
    const RefCkpt checkpoint{
        .life_epoch = 1,
        .committed_through = base,
        .checkpoint_snapshot_id = base,
        .last_epoch_seal = std::nullopt};
    CasRefCatalog::initializeEmptyForNewPool(*backend, layout);

    {
        const RootNamespace ns{"00/cleanup-missing-base-log@cas@"};
        fixture::admitLive(*backend, layout, ns);
        const NamespaceLifeId life = CasRefCatalog::lifeIfCataloged(*backend, layout, ns).value();
        writeRefSnapshotRaw(*backend, layout, minimalLiveSnapshot(ns.string(), base));
        EXPECT_THROW((void)readCheckpointSnapshotBase(*backend, layout, life, checkpoint), DB::Exception);
    }
    {
        const RootNamespace ns{"00/cleanup-missing-base-snapshot@cas@"};
        fixture::writeRefLogRaw(*backend, layout, RefLogTxn{
            .ns = ns.string(), .txn_id = base, .ops = {namespaceBirthOp()}, .prev_epoch_seal = std::nullopt});
        const NamespaceLifeId life = CasRefCatalog::lifeIfCataloged(*backend, layout, ns).value();
        EXPECT_THROW((void)readCheckpointSnapshotBase(*backend, layout, life, checkpoint), DB::Exception);
    }
    {
        const RootNamespace ns{"00/cleanup-seal-is-not-base@cas@"};
        writeSealAt(*backend, layout, ns, base);
        const NamespaceLifeId life = CasRefCatalog::lifeIfCataloged(*backend, layout, ns).value();
        writeRefSnapshotRaw(*backend, layout, minimalLiveSnapshot(ns.string(), base));
        EXPECT_THROW((void)readCheckpointSnapshotBase(*backend, layout, life, checkpoint), DB::Exception);
    }
}

TEST(CasGcFrontierGateCleanupRange, LaterEpochBaseWithoutItsContextualBacklinkCannotLicenseDeletion)
{
    auto backend = std::make_shared<InMemoryBackend>();
    const Layout layout{"p"};
    CasRefCatalog::initializeEmptyForNewPool(*backend, layout);
    const RefTxnId seal_id{1, 2};
    const RefTxnId base_id{2, 1};

    const auto expect_no_deletion_authority = [&](const RootNamespace & ns, std::optional<RefTxnId> backlink)
    {
        fixture::writeRefLogRaw(*backend, layout, RefLogTxn{
            .ns = ns.string(), .txn_id = RefTxnId{1, 1}, .ops = {namespaceBirthOp()},
            .prev_epoch_seal = std::nullopt});
        writeSealAt(*backend, layout, ns, seal_id);
        fixture::writeRefLogRaw(*backend, layout, RefLogTxn{
            .ns = ns.string(), .txn_id = base_id, .ops = {}, .prev_epoch_seal = backlink});
        writeRefSnapshotRaw(*backend, layout, minimalLiveSnapshot(ns.string(), base_id));
        const NamespaceLifeId life = CasRefCatalog::lifeIfCataloged(*backend, layout, ns).value();

        std::optional<RefTxnId> validated_base;
        try
        {
            (void)readCheckpointSnapshotBase(*backend, layout, life, RefCkpt{
                .life_epoch = 1,
                .committed_through = base_id,
                .checkpoint_snapshot_id = base_id,
                .last_epoch_seal = seal_id});
            validated_base = base_id;
        }
        catch (const DB::Exception &)
        {
        }

        RefTableListing listing;
        listing.logs = {{1, 1}, seal_id, base_id};
        listing.snapshots = {{1, 1}, base_id};
        const RefCleanupPlan plan = planRefCleanup(listing, base_id, validated_base);
        EXPECT_TRUE(plan.deletable_logs.empty());
        EXPECT_TRUE(plan.deletable_snapshots.empty());
    };

    expect_no_deletion_authority(RootNamespace{"00/cleanup-base-missing-backlink@cas@"}, std::nullopt);
    expect_no_deletion_authority(RootNamespace{"00/cleanup-base-wrong-backlink@cas@"}, RefTxnId{1, 99});
}

/// Folding a namespace terminal records evidence but performs no lifecycle-specific physical cleanup.
/// The checkpoint is inert debris for the perpetual janitor, and no `_cleanup` marker is published.
TEST(CasGcFrontierGate, CleanupEvidenceLeavesRemovedNamespaceCheckpointForJanitor)
{
    auto backend = std::make_shared<CountingBackend>();
    auto store = openPoolForTest(backend, /*gc_fold_max_defer_rounds=*/0);
    const Layout & layout = store->layout();
    const RootNamespace removed{"00/removed@cas@"};
    const RefOp birth_op = namespaceBirthOp();
    RefOp remove_op;
    remove_op.kind = RefOpKind::RemoveNamespace;
    fixture::writeRefLogRaw(*backend, layout, RefLogTxn{
        .ns = removed.string(), .txn_id = RefTxnId{1, 1}, .ops = {birth_op}, .prev_epoch_seal = std::nullopt});
    fixture::writeRefLogRaw(*backend, layout, RefLogTxn{
        .ns = removed.string(), .txn_id = RefTxnId{1, 2}, .ops = {remove_op}, .prev_epoch_seal = std::nullopt});
    const NamespaceLifeId life = CasRefCatalog::lifeIfCataloged(*backend, layout, removed).value();
    CasRefCatalog::casUpdate(*backend, layout, [&](const RefCatalog & current)
    {
        RefCatalog next = current;
        const auto it = std::find_if(next.entries.begin(), next.entries.end(), [&](const CatalogEntry & entry)
        {
            return entry.ns == removed;
        });
        EXPECT_NE(it, next.entries.end());
        it->state = NsState::Removing;
        it->removal_started_round = 1;
        return next;
    });
    const String ckpt_key = layout.refCkptKey(life);
    backend->putIfAbsent(ckpt_key, encodeRefCkpt(RefCkpt{
        .life_epoch = 1,
        .committed_through = RefTxnId{1, 2},
        .checkpoint_snapshot_id = std::nullopt,
        .last_epoch_seal = std::nullopt,
    }));

    /// The removal evidence must arise from a replay-valid terminal lifecycle, rather than merely
    /// from a raw terminal record that the recovery state machine refuses.
    const RecoveredRefTable recovered = recoverRefTableDetailedAtCatalogCutForTest(
        *backend, layout, CasRefCatalog::read(*backend, layout), removed);
    EXPECT_EQ(recovered.state.getLifecycle(), RefLifecycle::Removed);
    EXPECT_EQ(recovered.state.getRemoveTxnId(), (RefTxnId{1, 2}));

    Gc gc(store, kGc);
    ASSERT_TRUE(runRegularRoundReclaiming(gc).acquired_lease);

    const GcState st = decodeGcState(backend->get(layout.gcStateKey())->bytes);
    const CasFoldSeal seal = decodeFoldSeal(
        backend->get(layout.foldSealKey(st.snap_generation, st.snap_attempt))->bytes);
    const auto row_it = seal.ref_lives.find(life.incarnation);
    ASSERT_NE(row_it, seal.ref_lives.end());
    ASSERT_TRUE(row_it->second.cleanup_evidence.has_value());
    EXPECT_EQ(row_it->second.cleanup_evidence->remove_txn_id, (RefTxnId{1, 2}));
    EXPECT_TRUE(backend->head(ckpt_key).exists);
    for (const String & key : backend->touchedKeys())
        EXPECT_EQ(key.find("/_cleanup/"), String::npos) << key;

    ASSERT_TRUE(runRegularRoundReclaiming(gc).acquired_lease);
    EXPECT_TRUE(CasRefCatalog::read(*backend, layout).catalog.entries.empty());
    EXPECT_FALSE(CasRefCatalog::lifeIfCataloged(*backend, layout, removed));
    EXPECT_TRUE(backend->head(ckpt_key).exists);
    EXPECT_EQ(backend->deleteCount(ckpt_key), 0);
}

/// Once a terminal has folded, a later physical read failure is janitor debt, not lifecycle evidence
/// loss. Removing this per-key leak handling would either make the signal disappear or let one dead
/// object prevent the janitor from considering the rest of its page.
TEST(CasGcFrontierGate, PostFoldUnreadableTerminalIsCountedWithoutSuppressingProgress)
{
    auto backend = std::make_shared<PostFoldUnreadableTerminalBackend>();
    auto store = openPoolForTest(backend, /*gc_fold_max_defer_rounds=*/0);
    const Layout & layout = store->layout();
    const RootNamespace removed{"00/post-fold-unreadable@cas@"};
    const RootNamespace progressing{"00/post-fold-progress@cas@"};

    fixture::writeRefLogRaw(*backend, layout, RefLogTxn{
        .ns = removed.string(), .txn_id = RefTxnId{1, 1}, .ops = {namespaceBirthOp()},
        .prev_epoch_seal = std::nullopt});
    RefOp remove_op;
    remove_op.kind = RefOpKind::RemoveNamespace;
    fixture::writeRefLogRaw(*backend, layout, RefLogTxn{
        .ns = removed.string(), .txn_id = RefTxnId{1, 2}, .ops = {remove_op},
        .prev_epoch_seal = std::nullopt});
    const NamespaceLifeId removed_life = CasRefCatalog::lifeIfCataloged(*backend, layout, removed).value();
    CasRefCatalog::casUpdate(*backend, layout, [&](const RefCatalog & current)
    {
        RefCatalog next = current;
        const auto it = std::find_if(next.entries.begin(), next.entries.end(), [&](const CatalogEntry & entry)
        {
            return entry.ns == removed;
        });
        if (it == next.entries.end())
            throw std::runtime_error("test fixture lost removing catalog row");
        it->state = NsState::Removing;
        it->removal_started_round = 1;
        return next;
    });
    ASSERT_EQ(backend->putIfAbsent(layout.refCkptKey(removed_life), encodeRefCkpt(RefCkpt{
        .life_epoch = 1,
        .committed_through = RefTxnId{1, 2},
        .checkpoint_snapshot_id = std::nullopt,
        .last_epoch_seal = std::nullopt,
    })).outcome, PutOutcome::Done);

    const DB::UInt128 blob(0xfeed);
    const ManifestRef manifest = publish(*backend, layout, progressing, "victim", 1, blob);
    const ManifestId manifest_id{progressing, manifest};

    Gc gc(store, kGc);
    ASSERT_TRUE(runRegularRoundReclaiming(gc).acquired_lease);
    const GcState folded_state = decodeGcState(backend->get(layout.gcStateKey())->bytes);
    const CasFoldSeal folded_seal = decodeFoldSeal(
        backend->get(layout.foldSealKey(folded_state.snap_generation, folded_state.snap_attempt))->bytes);
    const auto folded_row = folded_seal.ref_lives.find(removed_life.incarnation);
    ASSERT_NE(folded_row, folded_seal.ref_lives.end());
    ASSERT_TRUE(folded_row->second.cleanup_evidence.has_value());

    dropRefTransition(*backend, layout, progressing, "victim", manifest);
    const String terminal_key = layout.refLogKey(removed_life, RefTxnId{1, 2});
    const String later_dead_residue = layout.refLogKey(removed_life, RefTxnId{1, 3});
    ASSERT_EQ(backend->putIfAbsent(later_dead_residue, "dead residue after the folded terminal").outcome,
        PutOutcome::Done);
    backend->makeUnreadable(terminal_key);

    std::map<String, UInt64> namespace_cleanup;
    const uint64_t leaks_before
        = ProfileEvents::global_counters[ProfileEvents::CasGcNamespaceCleanupLeaks].load();
    gc.setPhaseSink([&](const GcPhaseRecord & record)
    {
        if (record.phase == "namespace_cleanup")
            namespace_cleanup = record.metrics;
    });
    ScopedCasGcLogCapture log_capture;
    const RoundReport report = runRegularRoundReclaiming(gc);
    gc.setPhaseSink({});

    ASSERT_TRUE(report.acquired_lease);
    EXPECT_FALSE(CasRefCatalog::lifeIfCataloged(*backend, layout, removed))
        << "post-fold physical cleanup cannot gate catalog removal";
    EXPECT_TRUE(CasRefCatalog::lifeIfCataloged(*backend, layout, progressing));
    EXPECT_EQ(report.manifests_deleted, 1u)
        << "the janitor leak cannot promote itself into pool-wide destructive suppression";
    EXPECT_FALSE(backend->head(layout.manifestKey(manifest_id)).exists);
    EXPECT_TRUE(backend->existsIgnoringFault(terminal_key));
    EXPECT_FALSE(backend->existsIgnoringFault(later_dead_residue))
        << "one unreadable key cannot stop the perpetual janitor from deciding the rest of its page";
    ASSERT_FALSE(namespace_cleanup.empty());
    EXPECT_EQ(namespace_cleanup["leaked"], 1u);
    EXPECT_EQ(
        ProfileEvents::global_counters[ProfileEvents::CasGcNamespaceCleanupLeaks].load() - leaks_before,
        1u);
    const String captured = log_capture.captured();
    EXPECT_NE(captured.find(terminal_key), String::npos);
    EXPECT_NE(captured.find("leak"), String::npos);
}

TEST(CasGcFrontierGate, UnmatchedAdoptedParentLifeDoesNotSuppressAuthoritativeDeletion)
{
    auto backend = std::make_shared<CountingBackend>();
    auto store = openPoolForTest(backend, /*gc_fold_max_defer_rounds=*/0);
    const Layout & layout = store->layout();
    const RootNamespace ns{"00/unmatched-parent@cas@"};
    const DB::UInt128 blob(0xcafe);
    const ManifestRef mref = publish(*backend, layout, ns, "victim", 1, blob);
    const ManifestId manifest_id{ns, mref};

    Gc gc(store, kGc);
    ASSERT_TRUE(runRegularRoundReclaiming(gc).acquired_lease);
    ASSERT_TRUE(backend->head(layout.manifestKey(manifest_id)).exists);

    const GcState before = decodeGcState(backend->get(layout.gcStateKey())->bytes);
    const String parent_seal_key = layout.foldSealKey(before.snap_generation, before.snap_attempt);
    const auto parent_object = backend->get(parent_seal_key);
    ASSERT_TRUE(parent_object);
    CasFoldSeal parent = decodeFoldSeal(parent_object->bytes, before.snap_generation);
    const UInt128 unmatched_life = hexToU128("fedcba98765432100123456789abcdef");
    ASSERT_FALSE(parent.ref_lives.contains(unmatched_life));
    parent.ref_lives.emplace(unmatched_life, RefLifeFoldState{
        .coverage = RefCoverage{.classification = 2, .last_folded_ref_id = RefTxnId{9, 9}}});
    ASSERT_EQ(
        backend->putOverwrite(parent_seal_key, encodeFoldSeal(parent), parent_object->token).outcome,
        PutOutcome::Done);

    dropRefTransition(*backend, layout, ns, "victim", mref);
    const uint64_t events_before =
        ProfileEvents::global_counters[ProfileEvents::CasGcUnmatchedAdoptedParentLives].load();
    const RoundReport report = runRegularRoundReclaiming(gc);

    ASSERT_TRUE(report.acquired_lease);
    EXPECT_EQ(
        ProfileEvents::global_counters[ProfileEvents::CasGcUnmatchedAdoptedParentLives].load() - events_before,
        1u);
    EXPECT_EQ(report.manifests_deleted, 1u)
        << "an unmatched adopted-parent row is observed and dropped, not promoted to pool-wide suppression";
    EXPECT_FALSE(backend->head(layout.manifestKey(manifest_id)).exists)
        << "the valid manifest candidate must be physically deleted by the same authoritative round";

    const GcState after = decodeGcState(backend->get(layout.gcStateKey())->bytes);
    const CasFoldSeal successor = decodeFoldSeal(
        backend->get(layout.foldSealKey(after.snap_generation, after.snap_attempt))->bytes,
        after.snap_generation);
    EXPECT_FALSE(successor.ref_lives.contains(unmatched_life));
}

TEST(CatalogLifecycleReconciler, EmptyCatalogReturnsAuthoritativeCompleteCut)
{
    auto backend = std::make_shared<DrainRaceBackend>();
    auto store = openPoolForTest(backend, /*gc_fold_max_defer_rounds=*/0);
    const Layout & layout = store->layout();
    ASSERT_TRUE(CasRefCatalog::initializeEmptyForNewPool(*backend, layout).catalog.entries.empty());

    CasFoldSeal parent;
    CatalogLifecycleReconciler reconciler(
        *backend,
        layout,
        parent,
        /*admitted_generation=*/1,
        [](uint64_t)
        {
            return CasRefCatalog::LeaderFenceStatus::Held;
        });
    const CatalogLifecycleReconcileResult result = reconciler.reconcile();

    EXPECT_EQ(result.authority_status, AuthorityStatus::Authoritative);
    EXPECT_EQ(result.catalog_resolution, CatalogResolution::DrainComplete);
    ASSERT_TRUE(result.final_catalog_cut);
    EXPECT_TRUE(result.final_catalog_cut->catalog.entries.empty());
    EXPECT_TRUE(result.retired_lives.empty());
    EXPECT_EQ(result.deleted, 0);
}

TEST(CatalogLifecycleReconciler, DeletesEligibleRowsFromReturnedResolutionCuts)
{
    auto backend = std::make_shared<DrainRaceBackend>();
    auto store = openPoolForTest(backend, /*gc_fold_max_defer_rounds=*/0);
    const Layout & layout = store->layout();
    constexpr size_t deletes = 3;
    seedCompletedRemovingBatch(*backend, store, kGc, deletes);
    const auto parent_object = backend->get(layout.foldSealKey(1, 1));
    ASSERT_TRUE(parent_object);
    const CasFoldSeal parent = decodeFoldSeal(parent_object->bytes);
    backend->clearJournal();
    backend->resetCounts();

    CatalogLifecycleReconciler reconciler(
        *backend,
        layout,
        parent,
        /*admitted_generation=*/1,
        [](uint64_t)
        {
            return CasRefCatalog::LeaderFenceStatus::Held;
        });
    const CatalogLifecycleReconcileResult result = reconciler.reconcile();

    EXPECT_EQ(result.authority_status, AuthorityStatus::Authoritative);
    EXPECT_EQ(result.catalog_resolution, CatalogResolution::DrainComplete);
    EXPECT_EQ(result.deleted, deletes);
    ASSERT_EQ(result.retired_lives.size(), deletes);
    ASSERT_TRUE(result.final_catalog_cut);
    EXPECT_TRUE(result.final_catalog_cut->catalog.entries.empty());
    const std::vector<String> journal = backend->journalSnapshot();
    const String catalog_get = "get " + layout.refCatalogKey();
    EXPECT_EQ(std::count(journal.begin(), journal.end(), catalog_get), deletes + 1);
}

TEST(CatalogLifecycleReconciler, ReturnsRetiredLifeWhenAuthorityMovesAfterResolution)
{
    auto backend = std::make_shared<DrainRaceBackend>();
    auto store = openPoolForTest(backend, /*gc_fold_max_defer_rounds=*/0);
    const Layout & layout = store->layout();
    const CompletedRemovingFixture fixture = seedCompletedRemoving(*backend, store, kGc);
    const auto parent_object = backend->get(layout.foldSealKey(1, 1));
    ASSERT_TRUE(parent_object);
    const CasFoldSeal parent = decodeFoldSeal(parent_object->bytes);
    size_t fence_checks = 0;

    CatalogLifecycleReconciler reconciler(
        *backend,
        layout,
        parent,
        /*admitted_generation=*/1,
        [&fence_checks](uint64_t)
        {
            ++fence_checks;
            return fence_checks == 2
                ? CasRefCatalog::LeaderFenceStatus::Moved
                : CasRefCatalog::LeaderFenceStatus::Held;
        });
    const CatalogLifecycleReconcileResult result = reconciler.reconcile();

    EXPECT_EQ(result.authority_status, AuthorityStatus::FencedOut);
    EXPECT_EQ(result.catalog_resolution, CatalogResolution::ExactRowAbsent);
    ASSERT_EQ(result.retired_lives.size(), 1);
    EXPECT_EQ(result.retired_lives.front(),
        NamespaceLifeId::fromCatalogEntry(fixture.ns, fixture.life_id));
    EXPECT_EQ(result.deleted, 0);
    EXPECT_FALSE(result.final_catalog_cut);
}

TEST(CatalogLifecycleReconciler, InitialFenceLossReportsEligibleRowStillPresent)
{
    auto backend = std::make_shared<DrainRaceBackend>();
    auto store = openPoolForTest(backend, /*gc_fold_max_defer_rounds=*/0);
    const Layout & layout = store->layout();
    const CompletedRemovingFixture fixture = seedCompletedRemoving(*backend, store, kGc);
    const auto parent_object = backend->get(layout.foldSealKey(1, 1));
    ASSERT_TRUE(parent_object);
    const CasFoldSeal parent = decodeFoldSeal(parent_object->bytes);
    backend->resetCounts();

    CatalogLifecycleReconciler reconciler(
        *backend,
        layout,
        parent,
        /*admitted_generation=*/1,
        [](uint64_t)
        {
            return CasRefCatalog::LeaderFenceStatus::Moved;
        });
    const CatalogLifecycleReconcileResult result = reconciler.reconcile();

    EXPECT_EQ(result.authority_status, AuthorityStatus::FencedOut);
    EXPECT_EQ(result.catalog_resolution, CatalogResolution::ExactRowStillPresent);
    EXPECT_TRUE(result.retired_lives.empty());
    EXPECT_EQ(result.deleted, 0);
    EXPECT_FALSE(result.final_catalog_cut);
    EXPECT_EQ(backend->getCount(layout.refCatalogKey()), 2)
        << "the initial selection and mandatory erase-resolution cuts are the only catalog reads";
    EXPECT_EQ(backend->casPutCount(layout.refCatalogKey()), 0);
    EXPECT_EQ(CasRefCatalog::lifeIfCataloged(*backend, layout, fixture.ns),
        NamespaceLifeId::fromCatalogEntry(fixture.ns, fixture.life_id));
}

TEST(CatalogLifecycleReconciler, RetriesFromTheMandatoryConflictResolutionCut)
{
    auto backend = std::make_shared<DrainRaceBackend>();
    auto store = openPoolForTest(backend, /*gc_fold_max_defer_rounds=*/0);
    const Layout & layout = store->layout();
    seedCompletedRemoving(*backend, store, kGc);
    const auto parent_object = backend->get(layout.foldSealKey(1, 1));
    ASSERT_TRUE(parent_object);
    const CasFoldSeal parent = decodeFoldSeal(parent_object->bytes);
    backend->clearJournal();
    backend->resetCounts();
    backend->conflictNextCatalogCas(layout.refCatalogKey());

    CatalogLifecycleReconciler reconciler(
        *backend,
        layout,
        parent,
        /*admitted_generation=*/1,
        [](uint64_t)
        {
            return CasRefCatalog::LeaderFenceStatus::Held;
        });
    const CatalogLifecycleReconcileResult result = reconciler.reconcile();

    EXPECT_EQ(result.authority_status, AuthorityStatus::Authoritative);
    EXPECT_EQ(result.catalog_resolution, CatalogResolution::DrainComplete);
    EXPECT_EQ(result.deleted, 1);
    const std::vector<String> journal = backend->journalSnapshot();
    const String catalog_get = "get " + layout.refCatalogKey();
    EXPECT_EQ(std::count(journal.begin(), journal.end(), catalog_get), 3)
        << "the token-conflict retry must reuse its mandatory resolution cut";
}

TEST(CatalogLifecycleReconciler, PropagatesAuthorityFailureBeforeEraseCas)
{
    auto backend = std::make_shared<DrainRaceBackend>();
    auto store = openPoolForTest(backend, /*gc_fold_max_defer_rounds=*/0);
    const Layout & layout = store->layout();
    seedCompletedRemoving(*backend, store, kGc);
    const auto parent_object = backend->get(layout.foldSealKey(1, 1));
    ASSERT_TRUE(parent_object);
    const CasFoldSeal parent = decodeFoldSeal(parent_object->bytes);
    size_t fence_checks = 0;

    CatalogLifecycleReconciler reconciler(
        *backend,
        layout,
        parent,
        /*admitted_generation=*/1,
        [&fence_checks](uint64_t)
        {
            if (++fence_checks == 2)
                throw std::runtime_error("injected reconciler authority failure before CAS");
            return CasRefCatalog::LeaderFenceStatus::Held;
        });
    try
    {
        (void)reconciler.reconcile();
        FAIL() << "the authority exception must propagate";
    }
    catch (const std::runtime_error & e)
    {
        EXPECT_STREQ(e.what(), "injected reconciler authority failure before CAS");
    }
}

TEST(CatalogLifecycleReconciler, PropagatesAuthorityFailureAfterMandatoryResolution)
{
    auto backend = std::make_shared<DrainRaceBackend>();
    auto store = openPoolForTest(backend, /*gc_fold_max_defer_rounds=*/0);
    const Layout & layout = store->layout();
    const CompletedRemovingFixture fixture = seedCompletedRemoving(*backend, store, kGc);
    const auto parent_object = backend->get(layout.foldSealKey(1, 1));
    ASSERT_TRUE(parent_object);
    const CasFoldSeal parent = decodeFoldSeal(parent_object->bytes);
    size_t fence_checks = 0;

    CatalogLifecycleReconciler reconciler(
        *backend,
        layout,
        parent,
        /*admitted_generation=*/1,
        [&fence_checks](uint64_t)
        {
            if (++fence_checks == 3)
                throw std::runtime_error("injected reconciler authority failure after resolution");
            return CasRefCatalog::LeaderFenceStatus::Held;
        });
    try
    {
        (void)reconciler.reconcile();
        FAIL() << "the authority exception must propagate";
    }
    catch (const std::runtime_error & e)
    {
        EXPECT_STREQ(e.what(), "injected reconciler authority failure after resolution");
        EXPECT_FALSE(CasRefCatalog::lifeIfCataloged(*backend, layout, fixture.ns));
    }
}

TEST(CasGcFrontierGate, HealthyRebuildUsesTheCatalogLifecycleReconciler)
{
    auto backend = std::make_shared<DrainRaceBackend>();
    auto store = openPoolForTest(backend, /*gc_fold_max_defer_rounds=*/0);
    const Layout & layout = store->layout();
    const CompletedRemovingFixture fixture = seedCompletedRemoving(*backend, store, kGc);
    const uint64_t catalog_cas_before = backend->casPutCount(layout.refCatalogKey());

    Gc gc(store, kGc);
    const RebuildReport result = gc.rebuildBaseline(/*force=*/true);

    EXPECT_TRUE(result.performed);
    EXPECT_FALSE(CasRefCatalog::lifeIfCataloged(*backend, layout, fixture.ns));
    EXPECT_EQ(backend->casPutCount(layout.refCatalogKey()), catalog_cas_before + 1);
}

TEST(CasGcFrontierGate, DamagedStateRebuildDoesNotDeleteCompletedRemovingRows)
{
    auto backend = std::make_shared<DrainRaceBackend>();
    auto store = openPoolForTest(backend, /*gc_fold_max_defer_rounds=*/0);
    const Layout & layout = store->layout();
    const RootNamespace ns{"00/damaged-rebuild-removing@cas@"};
    CasRefCatalog::casAdmitEntry(*backend, layout, store->poolConfig().gc_shards, CatalogEntry{
        .ns = ns, .state = NsState::Live, .incarnation = UInt128{901}});
    CasRefCatalog::casUpdate(*backend, layout, [](const RefCatalog & current)
    {
        RefCatalog next = current;
        next.entries.front().state = NsState::Removing;
        next.entries.front().removal_started_round = 1;
        return next;
    });
    writeRecoverableCkptForRawFixture(*backend, layout, ns, RefCkpt{
        .life_epoch = 1,
        .committed_through = std::nullopt,
        .checkpoint_snapshot_id = std::nullopt,
        .last_epoch_seal = std::nullopt,
    });
    const uint64_t catalog_cas_before = backend->casPutCount(layout.refCatalogKey());

    Gc gc(store, kGc);
    const RebuildReport result = gc.rebuildBaseline(/*force=*/false);

    EXPECT_TRUE(result.performed);
    EXPECT_TRUE(CasRefCatalog::lifeIfCataloged(*backend, layout, ns));
    EXPECT_EQ(backend->casPutCount(layout.refCatalogKey()), catalog_cas_before);
}

TEST(CasGcFrontierGate, DeferredRoundDrainsCompletedRemovingBeforeReturning)
{
    auto backend = std::make_shared<CountingBackend>();
    auto store = openPoolForTest(backend, /*gc_fold_max_defer_rounds=*/100);
    const Layout & layout = store->layout();
    const RootNamespace removed{"00/deferred-removed@cas@"};
    const UInt128 life_id{77};
    CasRefCatalog::casAdmitEntry(*backend, layout, store->poolConfig().gc_shards, CatalogEntry{
        .ns = removed, .state = NsState::Live, .incarnation = life_id});
    CasRefCatalog::casUpdate(*backend, layout, [&](const RefCatalog & current)
    {
        RefCatalog next = current;
        next.entries[0].state = NsState::Removing;
        next.entries[0].removal_started_round = 1;
        return next;
    });

    CasFoldSeal parent;
    parent.generation = 1;
    parent.ref_lives.emplace(life_id, RefLifeFoldState{
        .coverage = RefCoverage{.classification = 2, .last_folded_ref_id = RefTxnId{1, 1}},
        .cleanup_evidence = RefCleanupEvidence{.remove_txn_id = RefTxnId{1, 1}}});
    for (uint64_t shard = 0; shard < store->poolConfig().gc_shards; ++shard)
        parent.condemned_summary.emplace(shard, CondemnedSummary{});
    ASSERT_EQ(backend->putIfAbsent(layout.foldSealKey(1, 1), encodeFoldSeal(parent)).outcome, PutOutcome::Done);
    GcState state;
    state.round = 1;
    state.gc_shards = store->poolConfig().gc_shards;
    state.snap_generation = 1;
    state.snap_attempt = 1;
    state.lease = GcLease{.owner = kGc, .seq = 1};
    ASSERT_EQ(backend->putIfAbsent(layout.gcStateKey(), encodeGcState(state)).outcome, PutOutcome::Done);

    const String ckpt_key = layout.refCkptKey(NamespaceLifeId::fromCatalogEntry(removed, life_id));
    ASSERT_EQ(backend->putIfAbsent(ckpt_key, "inert checkpoint debris").outcome, PutOutcome::Done);
    const uint64_t catalog_cas_before = backend->casPutCount(layout.refCatalogKey());

    Gc gc(store, kGc);
    const RoundReport report = runRegularRoundReclaiming(gc);
    ASSERT_TRUE(report.acquired_lease);
    EXPECT_TRUE(report.deferred);
    EXPECT_TRUE(CasRefCatalog::read(*backend, layout).catalog.entries.empty());
    EXPECT_FALSE(CasRefCatalog::lifeIfCataloged(*backend, layout, removed));
    EXPECT_EQ(backend->casPutCount(layout.refCatalogKey()), catalog_cas_before + 1);
    EXPECT_TRUE(backend->head(ckpt_key).exists);
    EXPECT_EQ(backend->deleteCount(ckpt_key), 0);
}

TEST(CasGcFrontierGate, StaleIssuedCatalogCasLosesAfterNewLeaderHelpsBeforeListing)
{
    auto backend = std::make_shared<DrainRaceBackend>();
    auto store = openPoolForTest(backend, /*gc_fold_max_defer_rounds=*/0);
    const Layout & layout = store->layout();
    const UInt128 leader_b = hexToU128("00000000000000000000000000000002");
    const CompletedRemovingFixture fixture = seedCompletedRemoving(*backend, store, kGc);
    backend->clearJournal();
    backend->blockNextCatalogCas(layout.refCatalogKey());

    std::exception_ptr leader_a_failure;
    std::thread leader_a([&]
    {
        try
        {
            Gc gc_a(store, kGc);
            (void)runRegularRoundReclaiming(gc_a);
        }
        catch (...)
        {
            leader_a_failure = std::current_exception();
        }
    });
    backend->waitForBlockedCatalogCas();

    transferGcLease(*backend, layout, leader_b);
    RoundReport report_b;
    std::exception_ptr leader_b_failure;
    try
    {
        Gc gc_b(store, leader_b);
        report_b = runRegularRoundReclaiming(gc_b);
    }
    catch (...)
    {
        leader_b_failure = std::current_exception();
    }

    const std::vector<String> before_a_release = backend->journalSnapshot();
    backend->releaseBlockedCatalogCas();
    leader_a.join();

    ASSERT_FALSE(leader_b_failure);
    ASSERT_TRUE(report_b.acquired_lease);
    ASSERT_FALSE(report_b.deferred);
    ASSERT_TRUE(CasRefCatalog::read(*backend, layout).catalog.entries.empty());

    const size_t catalog_cas_end = findJournalAfter(before_a_release, "cas_end " + layout.refCatalogKey(), 0);
    ASSERT_LT(catalog_cas_end, before_a_release.size());
    const size_t conclusive_rescan = findJournalAfter(
        before_a_release, "get " + layout.refCatalogKey(), catalog_cas_end + 1);
    ASSERT_LT(conclusive_rescan, before_a_release.size());
    const size_t stream_list = findJournalAfter(
        before_a_release, "list " + layout.casRefsPrefix(), conclusive_rescan + 1);
    ASSERT_LT(stream_list, before_a_release.size());
    const size_t fresh_catalog_cut = findJournalAfter(
        before_a_release, "get " + layout.refCatalogKey(), stream_list + 1);
    ASSERT_LT(fresh_catalog_cut, before_a_release.size());
    const GcState adopted = decodeGcState(backend->get(layout.gcStateKey())->bytes);
    const String successor_seal_key = layout.foldSealKey(adopted.snap_generation, adopted.snap_attempt);
    const size_t successor_seal_put = findJournalAfter(
        before_a_release, "put_end " + successor_seal_key, fresh_catalog_cut + 1);
    ASSERT_LT(successor_seal_put, before_a_release.size());
    const size_t redundant_plan_cut = findJournalAfter(
        before_a_release, "get " + layout.refCatalogKey(), fresh_catalog_cut + 1);
    const size_t successor_adoption = findJournalAfter(
        before_a_release, "cas_end " + layout.gcStateKey(), successor_seal_put + 1);
    ASSERT_LT(successor_adoption, before_a_release.size());
    EXPECT_LT(catalog_cas_end, conclusive_rescan);
    EXPECT_LT(conclusive_rescan, stream_list);
    EXPECT_LT(stream_list, fresh_catalog_cut);
    EXPECT_LT(fresh_catalog_cut, successor_seal_put);
    EXPECT_TRUE(redundant_plan_cut == before_a_release.size() || successor_seal_put < redundant_plan_cut)
        << "the fold must consume the one post-LIST catalog cut instead of reading a second plan cut";
    EXPECT_LT(successor_seal_put, successor_adoption);

    ASSERT_TRUE(leader_a_failure);
    EXPECT_FALSE(CasRefCatalog::lifeIfCataloged(*backend, layout, fixture.ns));
    EXPECT_EQ(backend->get(fixture.checkpoint_key)->bytes, fixture.checkpoint_bytes);
    EXPECT_EQ(backend->deleteCount(fixture.checkpoint_key), 0);
}

TEST(CasGcFrontierGate, LostCatalogCasResponseIsResolvedBeforeListing)
{
    auto backend = std::make_shared<DrainRaceBackend>();
    auto store = openPoolForTest(backend, /*gc_fold_max_defer_rounds=*/0);
    const Layout & layout = store->layout();
    const CompletedRemovingFixture fixture = seedCompletedRemoving(*backend, store, kGc);
    backend->clearJournal();
    backend->loseNextCatalogCasResponse(layout.refCatalogKey());

    Gc gc(store, kGc);
    const RoundReport report = runRegularRoundReclaiming(gc);
    ASSERT_TRUE(report.acquired_lease);
    ASSERT_FALSE(report.deferred);

    const std::vector<String> journal = backend->journalSnapshot();
    const size_t response_lost = findJournalAfter(
        journal, "cas_response_lost " + layout.refCatalogKey(), 0);
    ASSERT_LT(response_lost, journal.size());
    const size_t conclusive_rescan = findJournalAfter(
        journal, "get " + layout.refCatalogKey(), response_lost + 1);
    ASSERT_LT(conclusive_rescan, journal.size());
    const size_t stream_list = findJournalAfter(
        journal, "list " + layout.casRefsPrefix(), conclusive_rescan + 1);
    ASSERT_LT(stream_list, journal.size());
    const size_t fresh_catalog_cut = findJournalAfter(
        journal, "get " + layout.refCatalogKey(), stream_list + 1);
    ASSERT_LT(fresh_catalog_cut, journal.size());
    EXPECT_LT(response_lost, conclusive_rescan);
    EXPECT_LT(conclusive_rescan, stream_list);
    EXPECT_LT(stream_list, fresh_catalog_cut);

    EXPECT_FALSE(CasRefCatalog::lifeIfCataloged(*backend, layout, fixture.ns));
    EXPECT_EQ(backend->get(fixture.checkpoint_key)->bytes, fixture.checkpoint_bytes);
    EXPECT_EQ(backend->deleteCount(fixture.checkpoint_key), 0);
}

/// A stale leader may learn from its mandatory resolution read that the old life is gone, and must
/// invalidate that exact runtime, but loss of the leader fence remains the control outcome. It must
/// abort before the hot LIST and cannot build or publish any successor generation.
TEST_P(CasGcCompletedRemovalFenceRace, FencedLeaderStopsAfterWinnerRemovesOrReplacesLife)
{
    auto backend = std::make_shared<DrainRaceBackend>();
    auto store = openPoolForTest(backend, /*gc_fold_max_defer_rounds=*/0);
    const Layout & layout = store->layout();
    const UInt128 leader_b = hexToU128("00000000000000000000000000000002");
    const CompletedRemovingFixture fixture = seedCompletedRemoving(*backend, store, kGc);
    const NamespaceLifeId predecessor_life
        = NamespaceLifeId::fromCatalogEntry(fixture.ns, fixture.life_id);
    ASSERT_TRUE(store->refTableRecoveredForTest(fixture.ns))
        << "the fixture must retain a resident predecessor runtime before removal";
    ASSERT_EQ(store->refTableLifeForTest(fixture.ns), predecessor_life);
    const uint64_t predecessor_runtime = store->refTableRuntimeIdentityForTest(fixture.ns);
    ASSERT_NE(predecessor_runtime, 0u);
    backend->clearJournal();
    backend->blockNextCatalogCas(layout.refCatalogKey());

    std::exception_ptr leader_a_failure;
    std::thread leader_a([&]
    {
        try
        {
            Gc gc_a(store, kGc);
            (void)runRegularRoundReclaiming(gc_a);
        }
        catch (...)
        {
            leader_a_failure = std::current_exception();
        }
    });
    backend->waitForBlockedCatalogCas();

    transferGcLease(*backend, layout, leader_b);
    const CasRefCatalog::Snapshot observed = CasRefCatalog::read(*backend, layout);
    RefCatalog winner_catalog;
    if (GetParam() == CompetingCatalogOutcome::Replacement)
    {
        winner_catalog.entries.push_back(CatalogEntry{
            .ns = fixture.ns,
            .state = NsState::Live,
            .incarnation = UInt128{178}});
        /// Mirror production's publish-then-flip order: the successor life needs a readable `_ckpt`
        /// before its catalog row can read `Live`, or `chooseRecoveryGrounding` rejects it.
        const NamespaceLifeId successor_life = NamespaceLifeId::fromCatalogEntry(fixture.ns, UInt128{178});
        backend->putIfAbsent(layout.refCkptKey(successor_life), encodeRefCkpt(RefCkpt{
            .life_epoch = 1,
            .committed_through = std::nullopt,
            .checkpoint_snapshot_id = std::nullopt,
            .last_epoch_seal = std::nullopt,
        }));
    }
    ASSERT_EQ(backend->casPut(
        layout.refCatalogKey(), encodeRefCatalog(winner_catalog), observed.token).outcome,
        CasOutcome::Committed);

    backend->clearJournal();
    const uint64_t plans_before
        = ProfileEvents::global_counters[ProfileEvents::CasGcRefWalkPlansBuilt].load();
    backend->releaseBlockedCatalogCas();
    leader_a.join();

    const std::vector<String> journal = backend->journalSnapshot();
    ASSERT_TRUE(leader_a_failure);
    try
    {
        std::rethrow_exception(leader_a_failure);
        FAIL() << "expected DB::Exception";
    }
    catch (const DB::Exception & e)
    {
        EXPECT_EQ(e.code(), DB::ErrorCodes::NETWORK_ERROR);
        EXPECT_NE(e.message().find("pre-fold drain lost authority"), String::npos) << e.message();
    }
    EXPECT_EQ(ProfileEvents::global_counters[ProfileEvents::CasGcRefWalkPlansBuilt].load() - plans_before, 0u);
    EXPECT_EQ(findJournalAfter(journal, "list " + layout.casRefsPrefix(), 0), journal.size());
    EXPECT_EQ(findJournalAfter(journal, "cas_begin " + layout.gcStateKey(), 0), journal.size());
    EXPECT_FALSE(std::any_of(journal.begin(), journal.end(), [](const String & entry)
    {
        return entry.starts_with("put_begin ") && entry.ends_with("/fold_seal");
    }));
    EXPECT_LT(findJournalAfter(journal, "get " + layout.refCatalogKey(), 0), journal.size())
        << "the stale leader must still complete mandatory erase resolution";

    (void)store->namespaceLife(fixture.ns);
    EXPECT_NE(store->refTableRuntimeIdentityForTest(fixture.ns), 0u);
    ASSERT_TRUE(store->refTableLifeForTest(fixture.ns));
    EXPECT_NE(store->refTableLifeForTest(fixture.ns), predecessor_life)
        << "the next name-based resolution must not retain the retired predecessor life";
}

INSTANTIATE_TEST_SUITE_P(
    WinnerShape,
    CasGcCompletedRemovalFenceRace,
    testing::Values(CompetingCatalogOutcome::Absent, CompetingCatalogOutcome::Replacement),
    [](const testing::TestParamInfo<CompetingCatalogOutcome> & parameter)
    {
        return parameter.param == CompetingCatalogOutcome::Absent ? "Absent" : "Replacement";
    });

/// One initial full catalog read selects the first row; each successful erase's mandatory resolution
/// read becomes the next selection snapshot. Therefore N uncontended deletes cost N+1 reads before
/// the hot LIST. The round then takes one post-LIST walk-plan cut and, later in the separate
/// `namespace_cleanup` phase, one post-page janitor cut.
TEST(CasGcFrontierGate, CompletedRemovalDrainUsesNPlusOneCatalogReads)
{
    auto backend = std::make_shared<DrainRaceBackend>();
    auto store = openPoolForTest(backend, /*gc_fold_max_defer_rounds=*/0);
    const Layout & layout = store->layout();
    constexpr size_t deletes = 3;
    seedCompletedRemovingBatch(*backend, store, kGc, deletes);
    backend->clearJournal();
    backend->resetCounts();

    Gc gc(store, kGc);
    ASSERT_TRUE(runRegularRoundReclaiming(gc).acquired_lease);

    const std::vector<String> journal = backend->journalSnapshot();
    const size_t stream_list = findJournalAfter(journal, "list " + layout.casRefsPrefix(), 0);
    ASSERT_LT(stream_list, journal.size());
    const String catalog_get = "get " + layout.refCatalogKey();
    EXPECT_EQ(std::count(journal.begin(), journal.begin() + static_cast<ptrdiff_t>(stream_list), catalog_get),
        deletes + 1);
    const size_t walk_plan_cut = findJournalAfter(journal, catalog_get, stream_list);
    ASSERT_LT(walk_plan_cut, journal.size());
    const size_t janitor_list
        = findJournalAfter(journal, "list " + layout.namespaceRootPrefix(), walk_plan_cut);
    ASSERT_LT(janitor_list, journal.size());
    const size_t janitor_cut = findJournalAfter(journal, catalog_get, janitor_list);
    ASSERT_LT(janitor_cut, journal.size());
    EXPECT_EQ(findJournalAfter(journal, catalog_get, janitor_cut + 1), journal.size())
        << "one hot walk-plan cut and one janitor page cut are the only post-drain catalog reads";
    EXPECT_EQ(backend->listCount(layout.namespaceStreamRootPrefix()), 1u);
    EXPECT_EQ(backend->listCount(layout.namespaceRootPrefix()), 1u);
    EXPECT_EQ(backend->getCount(layout.refCatalogKey()), deletes + 3)
        << "N+1 drain reads, one post-hot-LIST cut, and one separate post-janitor-page cut";
}
