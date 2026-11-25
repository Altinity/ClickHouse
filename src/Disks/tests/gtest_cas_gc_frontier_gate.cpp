#include <gtest/gtest.h>

#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Backend/CasHotKeys.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Formats/CasFoldSealFormat.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Formats/CasGcStateFormat.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Formats/CasRefCkptFormat.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Formats/CasRefLogFormat.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Gc/CasGc.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Gc/CasGcScheduler.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Gc/CatalogLifecycleReconciler.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasPartWriteTxn.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasPool.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasRefProtocol.h>
#include <Common/ProfileEvents.h>
#include "cas_test_helpers.h"

#include <Poco/Exception.h>
#include <Poco/StreamChannel.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <future>
#include <latch>
#include <map>
#include <mutex>
#include <set>
#include <sstream>
#include <stdexcept>
#include <thread>

namespace DB::ErrorCodes
{
    extern const int NETWORK_ERROR;
    extern const int CORRUPTED_DATA;
}

/// THE DESTRUCTIVE-ROUND FRONTIER PROOF (spec 2026-07-27 "ref chain complete cut" §5).
///
/// Reachability is a property of the WHOLE POOL. A blob is unreferenced only if no namespace anywhere
/// owns an edge to it, so a round that deletes one is asserting something about every namespace at
/// once -- including the ones it never looked at. A `GET` at the cursor's arithmetic successor makes
/// the per-namespace proof cheap and exact, and a namespace that cannot be walked reports that fact
/// durably. Neither supplies the SET those proofs have to cover.
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
/// The third term is the SET, and only the catalog supplies it. The scenario that makes that so is the
/// one these tests open with: a hidden acked `+1` in a namespace no listing mentions and no sealed cursor
/// names, while a visible `-1` elsewhere drives the shared blob's OBSERVABLE in-degree to zero. Every
/// proof the round holds comes back clean and the blob is still owned. It survives because the round's
/// universe is the catalog's `Live`/`Removing` set, so that namespace is a member the round owes a proof
/// for and cannot supply one -- neither the listing's silence nor the missing cursor can shrink the set.
///
/// The tests here come in two shapes. Ones whose subject is the OPEN gate run on the production path,
/// with no policy argument at all. Ones whose subject is a SUPPRESSOR either pass `StageA_Suppressed`
/// explicitly or arrange the suppressing condition on the pool, and assert every delete family inert PER
/// FAMILY -- an aggregate zero can hide one family running while another did not.

using namespace DB::Cas;
using namespace DB::Cas::tests;

namespace ProfileEvents
{
extern const Event CASGCRefWalkPlansBuilt;
extern const Event CASGCUnmatchedAdoptedParentLives;
extern const Event CASGCNamespaceCleanupLeaks;
extern const Event CASGCStalledCreatingCancelled;
extern const Event CASOwnCreatingCancelled;
}

namespace
{

const UInt128 kGc = hexToU128("00000000000000000000000000000001");

/// The `CASCatalogLifecycleReconciler` suites hand their operation a liveness that reads a local bool
/// directly, and `DrainRaceBackend::afterReadOf` moves that bool at an exact request boundary -- so
/// nothing is cached behind a read and there is nothing for a refresh to re-read. Named rather than an
/// inline no-op because the argument is mandatory, precisely so that erasing without a refresh has to
/// be said out loud.
void noAuthorityRefresh() {}

/// The retirement announcement is mandatory for the same reason: a drain that erases without it has to
/// say so.
void noRetirementRecord(const NamespaceLifeId &) {}

/// For the suites whose catalog holds no `Creating` row: no creator is ever proved terminal.
CreatorSlotStatus noCreatorProvedTerminal(const CreatorFence &) { return CreatorSlotStatus::Live; }

/// For the suites that are not about the probe: every creator is proved terminal.
CreatorSlotStatus everyCreatorTerminal(const CreatorFence &) { return CreatorSlotStatus::Terminal; }

/// The lying store, shared from `cas_test_helpers.h`: every key is served by exact GET while the
/// selected ones are HIDDEN from every LIST. That is the only way to build the cross-namespace
/// scenario -- the hidden namespace's records stay durable and readable, so a round that KNOWS to
/// look for them finds them, while a round that only enumerates never learns they exist. Composed
/// over `CountingBackend` because these tests also assert request counts.
using CountingHintHoleBackend = DB::Cas::tests::HintHoleBackendOn<DB::Cas::tests::CountingBackend>;

class DrainRaceBackend final : public CountingBackend
{
public:
    /// Unhide the names the primitive overrides below would otherwise shadow.
    using CountingBackend::list;

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

    /// Lets `allowed` conditional writes of `key` through, then throws from every further one.
    void failCatalogCasesAfter(const String & key, size_t allowed)
    {
        std::lock_guard lock(control_mutex);
        catalog_key = key;
        catalog_cas_budget = allowed;
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

    /// Runs after every completed read of `key`. It is where a test moves a fact the operation's
    /// liveness predicate samples, so admission can be lost at an exact request boundary.
    void afterReadOf(const String & key, std::function<void()> hook)
    {
        std::lock_guard lock(control_mutex);
        after_read_key = key;
        after_read_hook = std::move(hook);
    }

    std::optional<Raw> read(const String & key, TransportAccess & access) override
    {
        record("get " + key);
        std::optional<Raw> raw = CountingBackend::read(key, access);
        std::function<void()> hook;
        {
            std::lock_guard lock(control_mutex);
            if (key == after_read_key)
                hook = after_read_hook;
        }
        if (hook)
            hook();
        return raw;
    }

    RawListPage list(const String & prefix, const String & cursor, size_t limit, TransportAccess & access) override
    {
        record("list " + prefix);
        return CountingBackend::list(prefix, cursor, limit, access);
    }

    std::expected<String, RawConflict> write(
        const String & key, const String & bytes, const std::optional<String> & expected_value,
        TransportAccess & access) override
    {
        /// One primitive now carries both shapes the journal used to name separately: a write with no
        /// precondition is the create, a write with one is the conditional replace.
        const bool conditional = expected_value.has_value();
        record((conditional ? "cas_begin " : "put_begin ") + key);
        bool lose_response = false;
        bool force_conflict = false;
        {
            std::unique_lock lock(control_mutex);
            if (conditional && key == catalog_key && block_next_catalog_cas)
            {
                block_next_catalog_cas = false;
                catalog_cas_blocked = true;
                control_cv.notify_all();
                control_cv.wait(lock, [&] { return release_catalog_cas; });
            }
            if (conditional && key == catalog_key && lose_next_catalog_cas_response)
            {
                lose_next_catalog_cas_response = false;
                lose_response = true;
            }
            if (conditional && key == catalog_key && catalog_cas_budget)
            {
                if (*catalog_cas_budget == 0)
                    throw DB::Exception(DB::ErrorCodes::NETWORK_ERROR, "injected catalog write failure");
                --*catalog_cas_budget;
            }
            if (conditional && key == catalog_key && conflict_next_catalog_cas)
            {
                conflict_next_catalog_cas = false;
                force_conflict = true;
            }
        }
        if (force_conflict)
        {
            record("cas_forced_conflict " + key);
            return std::unexpected(RawConflict{});
        }
        std::expected<String, RawConflict> result = CountingBackend::write(key, bytes, expected_value, access);
        record((conditional ? "cas_end " : "put_end ") + key);
        if (lose_response && result.has_value())
        {
            record("cas_response_lost " + key);
            /// `Poco::TimeoutException`, because that is the class the write loop cannot distinguish
            /// from a lost response: it settles the attempt by an exact read, finds these bytes under a
            /// moved incarnation, and reports the write committed. A non-`Poco` exception is rethrown
            /// unchanged instead, which would propagate a landed write as a failure.
            throw Poco::TimeoutException("injected lost catalog CAS response");
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
    std::optional<size_t> catalog_cas_budget;
    String after_read_key;
    std::function<void()> after_read_hook;
};

class DeadCheckpointHeadFailureBackend final : public CountingBackend
{
public:
    /// Unhide the names the primitive overrides below would otherwise shadow.
    using CountingBackend::head;
    using CountingBackend::list;

    RawListPage list(const String & prefix, const String & cursor, size_t limit, TransportAccess & access) override
    {
        RawListPage page = CountingBackend::list(prefix, cursor, limit, access);
        if (prefix.ends_with("/cas/ns/"))
            for (RawListedKey & listed : page.keys)
                listed.value.reset();
        return page;
    }

    std::optional<RawMeta> head(const String & key, TransportAccess & access) override
    {
        if (!bypass_fault && key == unreadable_key)
            throw std::runtime_error("injected dead checkpoint HEAD failure for " + key);
        return CountingBackend::head(key, access);
    }

    void makeUnreadable(String key)
    {
        unreadable_key = std::move(key);
    }

    /// The test's own look at the key the fault hides, taken through the same primitive with the fault
    /// suspended -- there is no second door to the store. A fresh open-fence CasRequests over `this`
    /// (aliasing, owns nothing): every Backend call needs a CasRequests-minted TransportAccess, and this
    /// method has no caller-supplied one to reuse.
    bool existsIgnoringFault(const String & key)
    {
        bypass_fault = true;
        CasRequests requests(BackendPtr(std::shared_ptr<void>(), this), Fence::open(), {}, {}, nullptr, DB::Cas::kDefaultRequestWindowMs);
        CasOperation op = requests.admit();
        const bool present = op.head(key, Retry::once()).has_value();
        bypass_fault = false;
        return present;
    }

private:
    String unreadable_key;
    bool bypass_fault = false;
};

class ScopedCasGcLogCapture
{
public:
    explicit ScopedCasGcLogCapture(const String & name = "CasGc", const String & level = "warning")
        : logger(getLogger(name))
        , channel(new Poco::StreamChannel(stream))
        , old_channel(logger->getChannel(), /*shared=*/true)
        , old_level(logger->getLevel())
    {
        logger->setChannel(channel.get());
        logger->setLevel(level);
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
    /// A real reference (shared=true), so the parked previous channel cannot die while ours is installed.
    Poco::AutoPtr<Poco::Channel> old_channel;
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
    CasOperation & op, const PoolPtr & store, const UInt128 & lease_owner)
{
    const Layout & layout = store->layout();
    CompletedRemovingFixture fixture{
        .ns = RootNamespace{"00/drain-race@cas@"},
        .life_id = UInt128{177},
        .checkpoint_key = {},
        .checkpoint_bytes = {}};
    CasRefCatalog::casAdmitEntry(op, layout, store->poolConfig().gc_shards, CatalogEntry{
        .ns = fixture.ns, .state = NsState::Live, .incarnation = fixture.life_id});
    fixture.checkpoint_key = layout.refCkptKey(
        NamespaceLifeId::fromCatalogEntry(fixture.ns, fixture.life_id));
    fixture.checkpoint_bytes = encodeRefCkptAtGeneration(RefCkpt{
        .life_epoch = 1,
        .committed_through = std::nullopt,
        .checkpoint_snapshot_id = std::nullopt,
        .last_epoch_seal = std::nullopt,
    }, 1);
    op.create(fixture.checkpoint_key, fixture.checkpoint_bytes, Retry::once());
    EXPECT_TRUE(store->namespaceFilesLifeIfReadable(fixture.ns));
    CasRefCatalog::casUpdate(op, layout, [](const RefCatalog & current)
    {
        RefCatalog next = current;
        next.entries[0].state = NsState::Removing;
        next.entries[0].removal_started_round = 1;
        return next;
    });

    CasFoldSeal parent;
    parent.generation = 1;
    parent.ref_lives.emplace(fixture.life_id, RefLifeFoldState{
        .coverage = RefCoverage{.classification = CoverageClass::Folded, .last_folded_ref_id = RefTxnId{1, 1}},
        .cleanup_evidence = RefCleanupEvidence{.remove_txn_id = RefTxnId{1, 1}}});
    for (uint64_t shard = 0; shard < store->poolConfig().gc_shards; ++shard)
        parent.condemned_summary.emplace(shard, CondemnedSummary{});
    op.create(layout.foldSealKey(1, 1), encodeFoldSeal(parent), Retry::once());

    GcState state;
    state.round = 1;
    state.gc_shards = store->poolConfig().gc_shards;
    state.snap_generation = 1;
    state.snap_attempt = 1;
    state.lease = GcLease{.owner = lease_owner, .seq = 1};
    op.create(layout.gcStateKey(), encodeGcState(state), Retry::once());

    return fixture;
}

void seedCompletedRemovingBatch(
    CasOperation & op, const PoolPtr & store, const UInt128 & lease_owner,
    size_t count)
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
        CasRefCatalog::casAdmitEntry(op, layout, store->poolConfig().gc_shards, entry);
        entries.push_back(std::move(entry));
    }
    CasRefCatalog::casUpdate(op, layout, [](const RefCatalog & current)
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
            .coverage = RefCoverage{.classification = CoverageClass::Folded, .last_folded_ref_id = RefTxnId{1, 1}},
            .cleanup_evidence = RefCleanupEvidence{.remove_txn_id = RefTxnId{1, 1}}});
    for (uint64_t shard = 0; shard < store->poolConfig().gc_shards; ++shard)
        parent.condemned_summary.emplace(shard, CondemnedSummary{});
    ASSERT_TRUE(std::holds_alternative<Committed>(op.create(layout.foldSealKey(1, 1), encodeFoldSeal(parent), Retry::once())));

    GcState state;
    state.round = 1;
    state.gc_shards = store->poolConfig().gc_shards;
    state.snap_generation = 1;
    state.snap_attempt = 1;
    state.lease = GcLease{.owner = lease_owner, .seq = 1};
    ASSERT_TRUE(std::holds_alternative<Committed>(op.create(layout.gcStateKey(), encodeGcState(state), Retry::once())));
}

enum class CompetingCatalogOutcome : uint8_t
{
    Absent,
    Replacement,
};

class CASGCCompletedRemovalFenceRace : public testing::TestWithParam<CompetingCatalogOutcome>
{
};

void transferGcLease(DrainRaceBackend & backend, const Layout & layout, const UInt128 & new_owner)
{
    auto requests = openRequestsForTest(backend);
    auto op = requests.admit();
    const auto got = op.read(layout.gcStateKey(), Retry::once());
    ASSERT_TRUE(got);
    GcState state = decodeGcState(got->bytes);
    state.lease.owner = new_owner;
    ++state.lease.seq;
    ASSERT_TRUE(std::holds_alternative<Committed>(
        op.replace(layout.gcStateKey(), encodeGcState(state), got->etag, Retry::once())));
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

/// The gate's own verdict for one round, READ OFF THE PHASE ROWS rather than recomputed in the test. A
/// test that re-derived `frontier_complete` from the tally would agree with a wrong formula just as
/// readily as with the right one.
struct GateVerdict
{
    bool saw_fold = false;
    bool frontier_complete = false;
    bool suppress_destructive = false;
    uint64_t frontier_namespaces = 0;
    uint64_t frontier_proven = 0;
    uint64_t frontier_unprobed_budget = 0;
    uint64_t catalog_entries = 0;
    bool catalog_proved_empty = false;
    /// `Removing` rows the pre-fold drain erased this round.
    uint64_t drain_erased = 0;
    /// `Creating` rows the pre-fold drain cancelled this round.
    uint64_t drain_cancelled = 0;
};

GateVerdict runRoundCapturingGate(const PoolPtr & store, Gc & gc, UniversePolicy policy)
{
    GateVerdict verdict;
    gc.setPhaseSink([&](const GcPhaseRecord & rec)
    {
        const auto value = [&](const char * name) -> std::optional<UInt64>
        {
            const auto it = rec.metrics.find(name);
            return it == rec.metrics.end() ? std::nullopt : std::optional<UInt64>{it->second};
        };
        if (rec.phase == "pre_fold_ref_drain")
        {
            if (const auto erased = value("deleted"))
                verdict.drain_erased = *erased;
            if (const auto cancelled = value("cancelled_creating"))
                verdict.drain_cancelled = *cancelled;
        }
        else if (rec.phase == "fold_reduce")
        {
            if (const auto complete = value("frontier_complete"))
            {
                verdict.saw_fold = true;
                verdict.frontier_complete = *complete != 0;
            }
            if (const auto suppress = value("suppress_destructive"))
                verdict.suppress_destructive = *suppress != 0;
        }
        else if (rec.phase == "fold_ref_intake")
        {
            if (const auto total = value("frontier_namespaces"))
                verdict.frontier_namespaces = *total;
            if (const auto proven = value("frontier_proven"))
                verdict.frontier_proven = *proven;
            if (const auto unprobed = value("frontier_unprobed_budget"))
                verdict.frontier_unprobed_budget = *unprobed;
            if (const auto entries = value("catalog_entries"))
                verdict.catalog_entries = *entries;
            if (const auto proved_empty = value("catalog_proved_empty"))
                verdict.catalog_proved_empty = *proved_empty != 0;
        }
    });
    gc.runRegularRound({}, /*allow_steal*/true, policy);
    gc.setPhaseSink({});
    store->renewWatermarkOnce();
    return verdict;
}

/// Every delete family a round can reach, asserted PER FAMILY: an aggregate zero can hide one family
/// running while another did not.
void expectEveryDeleteFamilyInert(const CountingBackend & backend, const char * where)
{
    EXPECT_EQ(backend.deleteCountForKeysContaining("/blobs/"), 0u) << where << ": blob delete";
    EXPECT_EQ(backend.deleteCountForKeysContaining("/cas/manifests/"), 0u)
        << where << ": manifest-body delete";
    EXPECT_EQ(backend.deleteCountForKeysContaining("/gc/gen/"), 0u)
        << where << ": generation prune and hand-off reclaim";
    EXPECT_EQ(backend.deleteCountForKeysContaining("/cas/ns/stream/"), 0u)
        << where << ": covered-log / superseded-snapshot cleanup";
    EXPECT_EQ(backend.deleteTotal(), 0u)
        << where << ": a family not named above also ran. Deleted:" << deletedKeysMessage(backend);
}

/// The row an interrupted cold admission leaves: `Creating`, under the fence of the server that ran it.
CatalogEntry seedStalledCreating(
    CasOperation & op, const PoolPtr & store, const String & ns, const UInt128 & incarnation,
    const CreatorFence & creator)
{
    const CatalogEntry entry{
        .ns = RootNamespace{ns}, .state = NsState::Creating, .incarnation = incarnation, .creator = creator};
    CasRefCatalog::casAdmitEntry(op, store->layout(), store->poolConfig().gc_shards, entry);
    return entry;
}

/// The checkpoint a stalled creation wrote in its step 2, before the flip that never came.
String seedStalledCheckpoint(CasOperation & op, const Layout & layout, const CatalogEntry & entry)
{
    const String key = layout.refCkptKey(NamespaceLifeId::fromCatalogEntry(entry.ns, entry.incarnation));
    EXPECT_TRUE(std::holds_alternative<Committed>(op.create(key, encodeRefCkptAtGeneration(RefCkpt{
        .life_epoch = entry.creator->writer_epoch,
        .committed_through = std::nullopt,
        .checkpoint_snapshot_id = std::nullopt,
        .last_epoch_seal = std::nullopt,
    }, 1), Retry::once())));
    return key;
}

/// A clean farewell at `epoch` in `root`'s mount slot. It certifies every lower epoch of `root` terminal.
void retireCreatorRootAt(Backend & backend, const Layout & layout, const String & root, uint64_t epoch)
{
    setWatermarkMinActive(backend, layout, root, epoch, std::numeric_limits<uint64_t>::max());
}

std::optional<CatalogEntry> catalogRow(CasOperation & op, const Layout & layout, const RootNamespace & ns)
{
    for (const CatalogEntry & entry : CasRefCatalog::read(op, layout).catalog.entries)
        if (entry.ns == ns)
            return entry;
    return std::nullopt;
}

/// A namespace's first append through the real lane, which is what resolves its catalog life.
RefTxnId publishBirth(const PoolPtr & store, const RootNamespace & ns, const String & ref, uint64_t build_sequence = 1)
{
    return store->appendRefOps(ns, MutationScope::ref(ref),
        [&ref, build_sequence](const RefTableState & state)
        {
            std::vector<RefOp> ops;
            if (state.getLifecycle() != RefLifecycle::Live)
                ops.push_back(namespaceBirthOp());
            for (const RefOp & op : publishCommittedOps(ref, ManifestRef{1, build_sequence, 1}))
                ops.push_back(op);
            return ops;
        },
        RootMutationOrigin::Writer, RootMutationKind::Publish);
}

}

/// ===================== A HIDDEN `+1` IN AN UNKNOWN NAMESPACE =====================
///
/// Two namespaces share one blob. `visible` publishes it and then drops it, so the round observes
/// `+1` then `-1` and reads the blob's in-degree as zero. `hidden` also owns it -- durably, acked,
/// readable by exact key -- but is absent from the round's LIST hint. Its own publish still carries a
/// real checkpoint, so the arithmetic walk finds and folds its `+1` by exact key regardless of what the
/// LIST omits.
///
/// The three arms below are the whole argument: the exact-key probe finds `hidden`'s edge and saves the
/// blob on a complete frontier; the blob still drains once `hidden` also honestly folds its own removal;
/// and a namespace inside the universe with a sealed cursor has its hidden `+1` found by the exact-key
/// probe the same way.

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

/// Rounds on the PRODUCTION path -- no policy argument -- because that is what the claim is about.
/// `hidden`'s own publish left it a real checkpoint, so the arithmetic walk's exact-key probe finds and
/// folds its `+1` no matter what the LIST hides: the blob survives on its own complete, proven frontier,
/// not on the round declining to touch anything.
TEST(CASGCFrontierGate, AHiddenEdgeIsFoundByTheExactKeyProbeAndSavesTheBlobOnACompleteFrontier)
{
    auto backend = std::make_shared<CountingHintHoleBackend>();
    const RootNamespace hidden{"00/hidden@cas@"};
    const RootNamespace visible{"00/visible@cas@"};
    const DB::UInt128 blob(0x5ade);

    auto store = buildCrossNamespaceScenario(backend, hidden, visible, blob, /*fold_hidden_first=*/false);
    const Layout & layout = store->layout();

    Gc gc(store, kGc);
    backend->resetCounts();
    GateVerdict verdict;
    for (int i = 0; i < 5; ++i)
    {
        const GateVerdict round = runRoundCapturingGate(store, gc, UniversePolicy::kDefault);
        if (round.saw_fold)
            verdict = round;
        store->renewWatermarkOnce();
    }

    ASSERT_TRUE(verdict.saw_fold) << "no round folded, so none published a gate verdict";
    OperationForTest raw_op(*backend);
    EXPECT_TRUE((*raw_op).head(blobKeyOf(layout, blob), Retry::once()).has_value())
        << "the blob a hidden namespace still owns must survive";
    EXPECT_TRUE(verdict.frontier_complete)
        << "the exact-key probe reads at `cursor + 1` and a LIST hole cannot hide an exact key, so the "
           "catalog-named hidden namespace IS provable — if this is false the blob above survived on "
           "suppression instead of on its own in-degree, which proves nothing about the edge";
    EXPECT_FALSE(verdict.suppress_destructive);
}

/// The arm above asserts "nothing was deleted", which does not on its own distinguish the gate correctly
/// refusing from the round simply never deleting anything at all. This
/// is the positive control: `hidden` is genuinely folded through its OWN drop of the same
/// blob (an honest exact-key read of a record the LIST still hides -- the arithmetic-intake mechanism
/// this whole file is about), so its frontier is REALLY proven, not merely declared so, and the blob is
/// REALLY unreferenced by both namespaces. The round drains it -- the zero-deletion arm above would pass
/// identically if the round were simply incapable of ever deleting anything.
TEST(CASGCFrontierGate, TheSameBlobDrainsOnceHiddenGenuinelyProvesItsOwnFrontier)
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

    drive(store, gc, /*rounds*/ 5, UniversePolicy::Authoritative);

    OperationForTest raw_op(*backend);
    EXPECT_FALSE((*raw_op).head(blobKeyOf(layout, blob), Retry::once()).has_value())
        << "both namespaces genuinely proved their frontier and the blob is genuinely unreferenced -- "
           "the round must still be able to reclaim it";
}

/// AND THE PER-NAMESPACE LOGIC IS WHAT SAVES IT. Identical to the arm above except that the hidden
/// namespace was folded once first, so it carries a sealed cursor and is therefore IN the universe even
/// though the hint has gone silent about it. The round probes its expected-next by exact key, finds the
/// record the listing hid, folds the `+1`, and the blob is never condemned.
TEST(CASGCFrontierGate, AKnownNamespaceIsProbedByExactKeyAndItsHiddenEdgeSavesTheBlob)
{
    auto backend = std::make_shared<CountingHintHoleBackend>();
    const RootNamespace hidden{"00/hidden@cas@"};
    const RootNamespace visible{"00/visible@cas@"};
    const DB::UInt128 blob(0x5ade);

    auto store = buildCrossNamespaceScenario(backend, hidden, visible, blob, /*fold_hidden_first=*/true);
    const Layout & layout = store->layout();

    Gc gc(store, kGc);
    drive(store, gc, /*rounds*/ 5, UniversePolicy::Authoritative);

    OperationForTest raw_op(*backend);
    EXPECT_TRUE((*raw_op).head(blobKeyOf(layout, blob), Retry::once()).has_value())
        << "the cursor kept the namespace in the universe, so its frontier was probed and its edge folded";
}

/// ===================== THE GATE FORMULA, TERM BY TERM =====================
///
/// The healthy case first, because every suppressor arm below is only meaningful against it: a pool with
/// nothing hidden, nothing held, no anomaly, and every namespace walked to an honest end-of-stream OPENS
/// the gate and reclaims. The two booleans are read off the fold's own rows, so a formula that computed
/// them differently would fail here rather than agree with the test.
TEST(CASGCFrontierGate, AHealthyCatalogRoundOpensTheGateAndReclaims)
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
    const GateVerdict verdict = runRoundCapturingGate(store, gc, UniversePolicy::kDefault);

    ASSERT_TRUE(verdict.saw_fold) << "the round did not fold, so it published no gate verdict";
    EXPECT_TRUE(verdict.frontier_complete)
        << "every namespace in a healthy catalog universe reached a proven frontier";
    EXPECT_FALSE(verdict.suppress_destructive)
        << "no anomaly, no hold, a complete frontier -- the gate has nothing left to refuse on";
    EXPECT_GT(verdict.frontier_namespaces, 0u)
        << "a zero-namespace universe would satisfy the equality vacuously; this pool must not be one";
    EXPECT_EQ(verdict.frontier_proven, verdict.frontier_namespaces);

    /// And the gate being open is worth something: the condemned blob actually drains.
    EXPECT_TRUE(runRoundsUntilAbsent(store, gc, *backend, layout, blob))
        << "an unsuppressed round must reclaim a blob no ref owns any more";
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

/// (3a) THE NEGATIVE-POLICY SEAM, and the per-site inventory at the same time. A caller that supplies no
/// universe suppresses on that term ALONE: this pool has no anomaly, no hold, and a frontier every
/// per-namespace probe proves -- the control at the end of the test is what says so, since the identical
/// pool drains on the production path.
TEST(CASGCFrontierGate, EveryInventoriedDestructiveSiteIsInertUnderSuppression)
{
    auto backend = std::make_shared<CountingBackend>();
    auto store = openPoolForTest(backend, /*gc_fold_max_defer_rounds=*/0);
    const Layout & layout = store->layout();

    Gc gc(store, kGc);
    const DB::UInt128 blob = buildPoolWithWorkAtEverySite(backend, store, gc);

    /// From here on the rounds supply NO universe: every site has work queued and every site must
    /// decline it.
    backend->resetCounts();
    const GateVerdict verdict = runRoundCapturingGate(store, gc, UniversePolicy::StageA_Suppressed);
    for (int i = 0; i < 5; ++i)
        runRoundCapturingGate(store, gc, UniversePolicy::StageA_Suppressed);

    ASSERT_TRUE(verdict.saw_fold) << "the round did not fold, so it published no gate verdict";
    EXPECT_FALSE(verdict.frontier_complete)
        << "with no universe supplied the frontier can never be complete, whatever the probes proved";
    EXPECT_TRUE(verdict.suppress_destructive);
    expectEveryDeleteFamilyInert(*backend, "no universe supplied");
    OperationForTest raw_op(*backend);
    EXPECT_TRUE((*raw_op).head(blobKeyOf(layout, blob), Retry::once()).has_value());

    /// The control: the identical pool DOES reclaim at those sites on the production path, so the zeros
    /// above are the gate at work and not an empty work queue -- and it is also what makes the "on that
    /// term alone" claim above true rather than assumed.
    drive(store, gc, /*rounds*/ 4, UniversePolicy::kDefault);
    EXPECT_GT(backend->deleteTotal(), 0u)
        << "the work queue was real -- a round with a universe drains it";
    EXPECT_FALSE((*raw_op).head(blobKeyOf(layout, blob), Retry::once()).has_value());
}

/// (1) ONE ANOMALY. A namespace whose `_ckpt` is present but undecodable records the "no usable
/// checkpoint" anomaly, and the round declines every site on that. It leaves the frontier incomplete too,
/// so what this arm pins is "an anomaly suppresses", not "only the anomaly does" -- which is why every
/// assertion below is about inertness and not about which term fired.
TEST(CASGCFrontierGate, AnUndecodableCheckpointAnomalySuppressesEveryDeleteFamily)
{
    auto backend = std::make_shared<CountingBackend>();
    auto store = openPoolForTest(backend, /*gc_fold_max_defer_rounds=*/0);
    CasRequests requests = openRequestsForTest(backend);
    CasOperation op = requests.admit();
    const Layout & layout = store->layout();

    Gc gc(store, kGc);
    const DB::UInt128 blob = buildPoolWithWorkAtEverySite(backend, store, gc);

    const RootNamespace damaged{"00/damaged@cas@"};
    publish(*backend, layout, damaged, "damaged_ref", 42, DB::UInt128(0xda43));
    /// Resolved through the catalog, not minted from the namespace name: the corruption has to land on
    /// the very object the round's own life resolution will read, or the round folds normally and this
    /// test measures nothing.
    const std::optional<NamespaceLifeId> damaged_life =
        CasRefCatalog::lifeIfCataloged(op, layout, damaged);
    ASSERT_TRUE(damaged_life.has_value()) << "the publish must have left a catalog entry to resolve";
    const std::optional<CkptSample> damaged_ckpt = readCkpt(op, layout, *damaged_life);
    ASSERT_TRUE(damaged_ckpt.has_value()) << "the publish must have left a `_ckpt` to damage";
    ASSERT_TRUE(std::holds_alternative<Committed>(op.replace(
        layout.refCkptKey(*damaged_life), "not a checkpoint", damaged_ckpt->etag, Retry::once())));

    backend->resetCounts();
    std::vector<size_t> anomaly_counts;
    for (int i = 0; i < 6; ++i)
    {
        anomaly_counts.push_back(gc.runRegularRound().anomalies.size());
        store->renewWatermarkOnce();
    }

    EXPECT_GT(anomaly_counts.front(), 0u)
        << "the undecodable `_ckpt` must be RECORDED, not silently absorbed -- a silent exit would make "
           "this test pass for the wrong reason";
    expectEveryDeleteFamilyInert(*backend, "one anomaly");
    EXPECT_TRUE(op.head(blobKeyOf(layout, blob), Retry::once()).has_value());
}

/// (2) ONE CARRIED HOLD. The gate's second term reads the SEAL, not this round's anomaly list, so the
/// round that matters here is a LATER one: the hold was detected earlier, rides forward because its
/// offending position is still unresolved, and must suppress on its own.
TEST(CASGCFrontierGate, ACarriedHoldSuppressesEveryDeleteFamily)
{
    auto backend = std::make_shared<CountingBackend>();
    auto store = openPoolForTest(backend, /*gc_fold_max_defer_rounds=*/0);
    const Layout & layout = store->layout();

    Gc gc(store, kGc);
    const DB::UInt128 blob = buildPoolWithWorkAtEverySite(backend, store, gc);

    /// A committed gap: the checkpoint says `{1,2}` is committed while only `{1,1}` was ever written, so
    /// the walk reads `{1,2}` absent BELOW its authority ceiling and holds there. Nothing repairs it, so
    /// every later round re-detects the same position and carries the same hold.
    const RootNamespace gapped{"00/gapped@cas@"};
    publish(*backend, layout, gapped, "gapped_ref", 44, DB::UInt128(0x6a9));
    replaceRecoverableCkptForRawFixture(*backend, layout, gapped, RefCkpt{
        .life_epoch = 1,
        .committed_through = RefTxnId{1, 2},
        .checkpoint_snapshot_id = std::nullopt,
        .last_epoch_seal = std::nullopt,
    });

    std::map<String, UInt64> intake;
    gc.setPhaseSink([&](const GcPhaseRecord & rec)
    {
        if (rec.phase == "fold_ref_intake")
            intake = rec.metrics;
    });
    gc.runRegularRound();
    gc.setPhaseSink({});
    store->renewWatermarkOnce();
    ASSERT_FALSE(intake.empty());
    ASSERT_GT(intake.at("tables_held"), 0u)
        << "the gap must be HELD, or the later rounds carry nothing and this test proves nothing";

    backend->resetCounts();
    for (int i = 0; i < 5; ++i)
    {
        gc.runRegularRound();
        store->renewWatermarkOnce();
    }
    expectEveryDeleteFamilyInert(*backend, "one carried hold");
    OperationForTest raw_op(*backend);
    EXPECT_TRUE((*raw_op).head(blobKeyOf(layout, blob), Retry::once()).has_value());
}

/// (3c) THE PROBE BUDGET. A namespace with a sealed cursor, no `_ckpt` and no listing left can be proven
/// only by a successor probe; a zero budget denies it one, so it counts toward the universe and not toward
/// the proofs, and the equality fails.
TEST(CASGCFrontierGate, AnExhaustedProbeBudgetSuppressesEveryDeleteFamily)
{
    auto backend = std::make_shared<CountingHintHoleBackend>();
    CasRequests requests = openRequestsForTest(backend);
    CasOperation op = requests.admit();
    auto store = openPoolWithProbeBudget(backend, /*budget*/ 0);
    const Layout & layout = store->layout();

    Gc gc(store, kGc);
    const DB::UInt128 blob = buildPoolWithWorkAtEverySite(backend, store, gc);

    /// The budget is spent only on a namespace the round knows about and can reach NO other way. Three
    /// conditions, and all three are load-bearing: a SEALED cursor (an unhinted namespace with no cursor
    /// is a no-genesis shape the budget never reaches), NO listing (a hinted target is walked for free),
    /// and NO readable `_ckpt` (a recoverable checkpoint proves the frontier without spending a probe --
    /// which is why publishing and hiding alone leaves the namespace provable and measures nothing).
    const RootNamespace quiet{"00/quiet@cas@"};
    publish(*backend, layout, quiet, "quiet_ref", 43, DB::UInt128(0x9a1e));
    runRoundCapturingGate(store, gc, UniversePolicy::StageA_Suppressed);
    ASSERT_NE(sealedCursorOf(*backend, layout, quiet), (RefTxnId{}))
        << "without a sealed cursor the namespace never becomes a budget-spending probe target";

    const std::optional<NamespaceLifeId> quiet_life =
        CasRefCatalog::lifeIfCataloged(op, layout, quiet);
    ASSERT_TRUE(quiet_life.has_value());
    const std::optional<CkptSample> quiet_ckpt = readCkpt(op, layout, *quiet_life);
    ASSERT_TRUE(quiet_ckpt.has_value()) << "there must be a `_ckpt` to remove";
    ASSERT_EQ(op.remove(layout.refCkptKey(*quiet_life), quiet_ckpt->etag, Retry::once()),
              Removal::Removed);
    backend->hidePrefix(layout.namespaceStreamPrefix(*quiet_life));

    backend->resetCounts();
    const GateVerdict verdict = runRoundCapturingGate(store, gc, UniversePolicy::kDefault);
    for (int i = 0; i < 5; ++i)
        runRoundCapturingGate(store, gc, UniversePolicy::kDefault);

    ASSERT_TRUE(verdict.saw_fold);
    EXPECT_GT(verdict.frontier_unprobed_budget, 0u)
        << "this arm must suppress on the BUDGET term; a zero here means some other suppressor fired and "
           "the test would pass without ever exhausting a budget";
    EXPECT_LT(verdict.frontier_proven, verdict.frontier_namespaces)
        << "the unprobed namespace must count toward the universe and not toward the proofs";
    EXPECT_FALSE(verdict.frontier_complete);
    EXPECT_TRUE(verdict.suppress_destructive);
    expectEveryDeleteFamilyInert(*backend, "exhausted probe budget");
    EXPECT_TRUE(op.head(blobKeyOf(layout, blob), Retry::once()).has_value());
}

/// `frontier_proven == frontier_namespaces` is `0 == 0` -- vacuously TRUE -- on an empty universe, which
/// is not by itself a proof of anything: a fresh pool, a damaged catalog, and a genuinely emptied pool
/// all produce the same zeros. The gate's non-vacuity term therefore has TWO ways to be satisfied:
/// `frontier_namespaces > 0` (an ordinary nonempty pool, everything proven), or the round's own hot-scan
/// catalog cut positively proving the universe empty (present, token-bearing, decoded, zero rows of
/// every lifecycle state). The next two tests are that positive/negative pair.
TEST(CASGCFrontierGate, ADecodedTokenBearingEmptyCatalogCompletesTheFrontierAndDrainsRetiredWork)
{
    auto backend = std::make_shared<CountingBackend>();
    auto store = openPoolForTest(backend, /*gc_fold_max_defer_rounds=*/0);
    CasRequests requests = openRequestsForTest(backend);
    CasOperation op = requests.admit();
    const Layout & layout = store->layout();
    const DB::UInt128 blob(0xbead);

    /// Built to make `frontier_namespaces` GENUINELY zero by every source that feeds it -- no catalog
    /// entry (this pool never admitted a namespace), no sealed cursor, no ref-log hint -- while a
    /// condemned blob with a real, present body and in-degree 0 sits queued exactly the way a real
    /// round leaves one (`injectRetire`).
    writeBlobBody(*backend, layout, blob);
    const BlobRef blob_ref = legacyMetaTestRef(blob);
    const std::optional<Meta> blob_observed = op.head(layout.blobKey(blob_ref), Retry::once());
    ASSERT_TRUE(blob_observed) << "the seeded blob body must be present before it is condemned";
    const PersistedEtag blob_token = PersistedEtag::capture(blob_observed->etag);
    injectRetire(*backend, layout, /*round*/ 1, /*shard*/ 0,
        {RetiredEntry{.kind = ObjectKind::Blob, .ref = blob_ref, .token = blob_token, .size = 0}});
    store->renewWatermarkOnce();

    ASSERT_TRUE(CasRefCatalog::read(op, layout).catalog.entries.empty())
        << "the scenario needs a genuinely, provably empty catalog, or this test measures nothing";

    Gc gc(store, kGc);
    backend->resetCounts();

    /// The drive is bounded tightly, so slower pacing fails loudly instead of being absorbed by a generous loop.
    ASSERT_TRUE(op.head(layout.blobKey(blob_ref), Retry::once()).has_value())
        << "the scenario starts with the condemned blob present, or the loop below measures nothing";

    constexpr int kMaxRounds = 5;   /// bound on the drain; see the comment above
    /// Observe the TWO-PHASE PIPELINE explicitly: an open, unsuppressed gate while the blob was still
    /// present (the graduate side), and only a STRICTLY LATER round removing it (the delete side).
    /// Asserting only the final state and the last verdict would pass just as readily if the blob
    /// vanished by some other path entirely.
    int round_gate_opened_while_present = -1;   /// the graduate side: FIRST round that folded, unsuppressed,
                                                 /// with the blob still present
    int round_blob_vanished = -1;               /// the delete side
    GateVerdict last;
    int rounds_run = 0;
    for (int i = 0; i < kMaxRounds && op.head(layout.blobKey(blob_ref), Retry::once()).has_value(); ++i)
    {
        last = runRoundCapturingGate(store, gc, UniversePolicy::Authoritative);
        ++rounds_run;
        const bool still_present = op.head(layout.blobKey(blob_ref), Retry::once()).has_value();
        if (round_gate_opened_while_present < 0 && last.saw_fold && !last.suppress_destructive && still_present)
            round_gate_opened_while_present = rounds_run;
        if (round_blob_vanished < 0 && !still_present)
            round_blob_vanished = rounds_run;
    }

    ASSERT_GT(rounds_run, 0) << "the loop must actually run, or every assertion below is vacuous";
    ASSERT_LE(rounds_run, kMaxRounds)
        << "the drain took more than the measured cadence -- this is a real regression in the "
           "confirm/retry pacing, not something to hide by bumping the bound; re-derive the cadence";
    ASSERT_TRUE(last.saw_fold);
    EXPECT_EQ(last.frontier_namespaces, 0u);
    EXPECT_EQ(last.frontier_proven, 0u);
    EXPECT_TRUE(last.catalog_proved_empty)
        << "the catalog cut itself must be the proof, not the bare 0==0 equality";
    EXPECT_TRUE(last.frontier_complete);
    EXPECT_FALSE(last.suppress_destructive);
    ASSERT_GT(round_gate_opened_while_present, 0)
        << "the gate must have opened (folded, unsuppressed) at least one round BEFORE the blob was "
           "gone -- the graduate side of the two-phase pipeline -- not just at the round that deleted it";
    ASSERT_GT(round_blob_vanished, round_gate_opened_while_present)
        << "the delete must be a round STRICTLY LATER than the one that opened the gate, never the same "
           "round -- a round that both graduates and deletes in one step would hide the two-phase split";
    EXPECT_FALSE(op.head(layout.blobKey(blob_ref), Retry::once()).has_value())
        << "a proved-empty universe is a COMPLETE frontier, not a suppressed one -- the condemned blob "
           "must drain through the ordinary two-phase pipeline instead of leaking forever";
}

/// The negative half of the pair. A `Creating` row is a birth in progress (spec §3: no publication can
/// exist under it, so `live_incarnation`/`walk_plan.lives()` exclude it -- see the R10 comment above the
/// intake loop), not an empty universe -- but it produces the SAME `frontier_namespaces ==
/// frontier_proven == 0` a genuinely empty catalog does. Only `catalog_cut_proved_empty` tells them
/// apart, because it reads `entries` (every lifecycle state), not the frontier counters.
TEST(CASGCFrontierGate, AZeroWalkableFrontierWithACreatingCatalogRowIsNotProvedEmpty)
{
    auto backend = std::make_shared<CountingBackend>();
    auto store = openPoolForTest(backend, /*gc_fold_max_defer_rounds=*/0);
    CasRequests requests = openRequestsForTest(backend);
    CasOperation op = requests.admit();
    const Layout & layout = store->layout();
    const DB::UInt128 blob(0xbead);

    writeBlobBody(*backend, layout, blob);
    const BlobRef blob_ref = legacyMetaTestRef(blob);
    const std::optional<Meta> blob_observed = op.head(layout.blobKey(blob_ref), Retry::once());
    ASSERT_TRUE(blob_observed) << "the seeded blob body must be present before it is condemned";
    const PersistedEtag blob_token = PersistedEtag::capture(blob_observed->etag);
    injectRetire(*backend, layout, /*round*/ 1, /*shard*/ 0,
        {RetiredEntry{.kind = ObjectKind::Blob, .ref = blob_ref, .token = blob_token, .size = 0}});
    store->renewWatermarkOnce();

    const RootNamespace stalled{"00/stalled@cas@"};
    CatalogEntry entry;
    entry.ns = stalled;
    entry.state = NsState::Creating;
    entry.incarnation = hexToU128("00000000000000000000000000000042");
    entry.creator = CreatorFence{
        .server_root_id = "test-stalled-creator", .writer_epoch = 1, .fence_generation = 1};
    CasRefCatalog::casAdmitEntry(op, layout, /*gc_shards*/ 1, entry);

    Gc gc(store, kGc);
    backend->resetCounts();
    for (int i = 0; i < 6; ++i)
    {
        const GateVerdict v = runRoundCapturingGate(store, gc, UniversePolicy::Authoritative);
        ASSERT_TRUE(v.saw_fold);
        EXPECT_EQ(v.frontier_namespaces, 0u);
        EXPECT_EQ(v.frontier_proven, 0u);
        EXPECT_FALSE(v.catalog_proved_empty)
            << "a Creating-only catalog is a birth in progress, not proof of an empty universe";
        EXPECT_FALSE(v.frontier_complete);
        EXPECT_TRUE(v.suppress_destructive);
    }
    expectEveryDeleteFamilyInert(*backend, "Creating-only catalog");
    EXPECT_TRUE(op.head(layout.blobKey(blob_ref), Retry::once()).has_value());
}

/// The wedge after drop-all: every table is dropped, and the catalog holds only the `Creating` row of a
/// `CREATE TABLE` whose server has since moved to a higher writer epoch. The condemned blob's seeded run gives
/// the first round an adopted parent, so that round cancels the row, proves its own cut empty, and the
/// blob drains.
TEST(CASGCFrontierGate, AStalledCreatingRowWhoseCreatorMovedToAHigherEpochIsCancelledAndTheBlobDrains)
{
    auto backend = std::make_shared<CountingBackend>();
    auto store = openPoolForTest(backend, /*gc_fold_max_defer_rounds=*/0);
    CasRequests requests = openRequestsForTest(backend);
    CasOperation op = requests.admit();
    const Layout & layout = store->layout();
    const DB::UInt128 blob(0xbead);

    writeBlobBody(*backend, layout, blob);
    const BlobRef blob_ref = legacyMetaTestRef(blob);
    const std::optional<Meta> blob_observed = op.head(layout.blobKey(blob_ref), Retry::once());
    ASSERT_TRUE(blob_observed);
    injectRetire(*backend, layout, /*round*/ 1, /*shard*/ 0,
        {RetiredEntry{.kind = ObjectKind::Blob, .ref = blob_ref, .token = PersistedEtag::capture(blob_observed->etag), .size = 0}});
    store->renewWatermarkOnce();

    const CatalogEntry stalled = seedStalledCreating(op, store, "00/stalled@cas@",
        hexToU128("00000000000000000000000000000042"),
        CreatorFence{.server_root_id = "moved-creator", .writer_epoch = 1, .fence_generation = 1});
    const String stalled_ckpt = seedStalledCheckpoint(op, layout, stalled);
    retireCreatorRootAt(*backend, layout, "moved-creator", /*epoch*/ 2);

    Gc gc(store, kGc);
    const GateVerdict cancelling = runRoundCapturingGate(store, gc, UniversePolicy::Authoritative);
    ASSERT_TRUE(cancelling.saw_fold);
    EXPECT_EQ(cancelling.drain_cancelled, 1u);
    EXPECT_EQ(cancelling.drain_erased, 0u) << "a cancel is not an erase of a `Removing` row";
    EXPECT_FALSE(catalogRow(op, layout, stalled.ns));
    EXPECT_TRUE(cancelling.catalog_proved_empty) << "the cut the same round takes after its drain is empty";
    EXPECT_TRUE(cancelling.frontier_complete);
    EXPECT_FALSE(cancelling.suppress_destructive);

    constexpr int kMaxRounds = 6;
    for (int i = 0; i < kMaxRounds && (op.head(layout.blobKey(blob_ref), Retry::once()) || op.head(stalled_ckpt, Retry::once())); ++i)
        EXPECT_FALSE(runRoundCapturingGate(store, gc, UniversePolicy::Authoritative).suppress_destructive);
    EXPECT_FALSE(op.head(layout.blobKey(blob_ref), Retry::once())) << "the condemned blob drains";
    EXPECT_FALSE(op.head(stalled_ckpt, Retry::once())) << "the namespace janitor reclaims the cancelled life's checkpoint";
}

/// Generation zero has no adopted parent and runs no pre-fold drain, so the bootstrap round leaves a
/// certified row and the first round with a parent cancels it.
TEST(CASGCFrontierGate, AStalledCreatingRowWaitsOutTheBootstrapRound)
{
    auto backend = std::make_shared<CountingBackend>();
    auto store = openPoolForTest(backend, /*gc_fold_max_defer_rounds=*/0);
    CasRequests requests = openRequestsForTest(backend);
    CasOperation op = requests.admit();
    const Layout & layout = store->layout();
    const CatalogEntry stalled = seedStalledCreating(op, store, "00/stalled@cas@",
        hexToU128("00000000000000000000000000000043"),
        CreatorFence{.server_root_id = "moved-creator", .writer_epoch = 1, .fence_generation = 1});
    retireCreatorRootAt(*backend, layout, "moved-creator", /*epoch*/ 2);

    Gc gc(store, kGc);
    const GateVerdict bootstrap = runRoundCapturingGate(store, gc, UniversePolicy::Authoritative);
    ASSERT_TRUE(bootstrap.saw_fold);
    EXPECT_EQ(bootstrap.drain_cancelled, 0u);
    EXPECT_FALSE(bootstrap.catalog_proved_empty);
    EXPECT_EQ(catalogRow(op, layout, stalled.ns), std::optional<CatalogEntry>{stalled});

    const GateVerdict next = runRoundCapturingGate(store, gc, UniversePolicy::Authoritative);
    EXPECT_EQ(next.drain_cancelled, 1u);
    EXPECT_FALSE(catalogRow(op, layout, stalled.ns));
    EXPECT_TRUE(next.catalog_proved_empty);
}

/// A creator alive at the epoch it recorded holds no certificate, so its row is never touched. The
/// certified sibling in the same catalog shows the drain did consider `Creating` rows.
TEST(CASGCFrontierGate, AStalledCreatingRowWhoseCreatorIsAliveIsNotTouched)
{
    auto backend = std::make_shared<CountingBackend>();
    auto store = openPoolForTest(backend, /*gc_fold_max_defer_rounds=*/0);
    CasRequests requests = openRequestsForTest(backend);
    CasOperation op = requests.admit();
    const Layout & layout = store->layout();

    const CatalogEntry alive = seedStalledCreating(op, store, "00/alive@cas@",
        hexToU128("00000000000000000000000000000051"),
        CreatorFence{.server_root_id = store->poolConfig().server_root_id, .writer_epoch = store->writerEpoch(), .fence_generation = 1});
    const CatalogEntry certified = seedStalledCreating(op, store, "00/certified@cas@",
        hexToU128("00000000000000000000000000000052"),
        CreatorFence{.server_root_id = "moved-creator", .writer_epoch = 1, .fence_generation = 1});
    retireCreatorRootAt(*backend, layout, "moved-creator", /*epoch*/ 2);

    Gc gc(store, kGc);
    uint64_t erased = 0;
    for (int i = 0; i < 4; ++i)
    {
        const GateVerdict v = runRoundCapturingGate(store, gc, UniversePolicy::Authoritative);
        erased += v.drain_cancelled;
        EXPECT_TRUE(v.suppress_destructive) << "the alive creator's row is still a birth in progress";
        EXPECT_EQ(catalogRow(op, layout, alive.ns), std::optional<CatalogEntry>{alive}) << "round " << i;
    }
    EXPECT_EQ(erased, 1u);
    EXPECT_FALSE(catalogRow(op, layout, certified.ns)) << "positive control: the certified row went";
}

/// One certified and one uncertified `Creating` row: only the certified one goes, and the frontier stays
/// suppressed while the uncertified one remains. An absent mount slot is no certificate.
TEST(CASGCFrontierGate, OnlyTheCertifiedOfTwoStalledCreatingRowsIsCancelledAndTheSuppressionStays)
{
    auto backend = std::make_shared<CountingBackend>();
    auto store = openPoolForTest(backend, /*gc_fold_max_defer_rounds=*/0);
    CasRequests requests = openRequestsForTest(backend);
    CasOperation op = requests.admit();
    const Layout & layout = store->layout();
    const DB::UInt128 blob(0xbead);

    writeBlobBody(*backend, layout, blob);
    const BlobRef blob_ref = legacyMetaTestRef(blob);
    const std::optional<Meta> blob_observed = op.head(layout.blobKey(blob_ref), Retry::once());
    ASSERT_TRUE(blob_observed);
    injectRetire(*backend, layout, /*round*/ 1, /*shard*/ 0,
        {RetiredEntry{.kind = ObjectKind::Blob, .ref = blob_ref, .token = PersistedEtag::capture(blob_observed->etag), .size = 0}});
    store->renewWatermarkOnce();

    const CatalogEntry certified = seedStalledCreating(op, store, "00/certified@cas@",
        hexToU128("00000000000000000000000000000061"),
        CreatorFence{.server_root_id = "moved-creator", .writer_epoch = 1, .fence_generation = 1});
    const CatalogEntry uncertified = seedStalledCreating(op, store, "00/uncertified@cas@",
        hexToU128("00000000000000000000000000000062"),
        CreatorFence{.server_root_id = "never-mounted", .writer_epoch = 1, .fence_generation = 1});
    retireCreatorRootAt(*backend, layout, "moved-creator", /*epoch*/ 2);

    Gc gc(store, kGc);
    backend->resetCounts();
    uint64_t erased = 0;
    for (int i = 0; i < 4; ++i)
    {
        const GateVerdict v = runRoundCapturingGate(store, gc, UniversePolicy::Authoritative);
        erased += v.drain_cancelled;
        ASSERT_TRUE(v.saw_fold);
        EXPECT_EQ(v.frontier_namespaces, 0u);
        EXPECT_FALSE(v.catalog_proved_empty);
        EXPECT_TRUE(v.suppress_destructive);
    }
    EXPECT_EQ(erased, 1u);
    EXPECT_FALSE(catalogRow(op, layout, certified.ns));
    EXPECT_EQ(catalogRow(op, layout, uncertified.ns), std::optional<CatalogEntry>{uncertified});
    EXPECT_EQ(backend->deleteCountForKeysContaining("/blobs/"), 0u);
    EXPECT_TRUE(op.head(layout.blobKey(blob_ref), Retry::once()).has_value());
}

/// With tables present the frontier is complete either way, but the row would still leak. It is cancelled,
/// and the `Live` row beside it is left exactly as it was.
TEST(CASGCFrontierGate, AStalledCreatingRowBesideALiveNamespaceIsCancelledAndNothingElseChanges)
{
    auto backend = std::make_shared<CountingBackend>();
    auto store = openPoolForTest(backend, /*gc_fold_max_defer_rounds=*/0);
    CasRequests requests = openRequestsForTest(backend);
    CasOperation op = requests.admit();
    const Layout & layout = store->layout();

    const RootNamespace live_ns{"00/live@cas@"};
    store->namespaceLife(live_ns);
    const std::optional<CatalogEntry> live = catalogRow(op, layout, live_ns);
    ASSERT_TRUE(live && live->state == NsState::Live);
    const CatalogEntry stalled = seedStalledCreating(op, store, "00/stalled@cas@",
        hexToU128("00000000000000000000000000000071"),
        CreatorFence{.server_root_id = "moved-creator", .writer_epoch = 1, .fence_generation = 1});
    retireCreatorRootAt(*backend, layout, "moved-creator", /*epoch*/ 2);

    Gc gc(store, kGc);
    uint64_t erased = 0;
    for (int i = 0; i < 3; ++i)
    {
        const GateVerdict v = runRoundCapturingGate(store, gc, UniversePolicy::Authoritative);
        erased += v.drain_cancelled;
        EXPECT_EQ(v.frontier_namespaces, 1u);
    }
    EXPECT_EQ(erased, 1u);
    EXPECT_EQ(CasRefCatalog::read(op, layout).catalog.entries, std::vector<CatalogEntry>{*live});
}

namespace
{

/// A mount slot of `root` at `epoch` that is neither fenced nor a farewell, unless asked.
void writeCreatorSlot(Backend & backend, const Layout & layout, const String & root, uint64_t epoch, bool gc_fenced)
{
    MountLease lease;
    lease.writer_epoch = epoch;
    lease.min_active_build_sequence = 1;
    lease.gc_fenced = gc_fenced;
    lease.seq = 1;
    lease.write_attempt_id = UInt128{1};
    OperationForTest op(backend);
    ASSERT_TRUE(std::holds_alternative<Committed>((*op).create(layout.mountKey(root), encodeMountLease(lease), Retry::once())));
}

}

/// A foreign creator whose slot still holds the row's epoch, and one whose slot holds a LOWER epoch, carry
/// no certificate: their rows stay. The certified row beside them shows the drain considered all three.
TEST(CASGCFrontierGate, AForeignCreatorAliveAtItsEpochOrBelowItKeepsItsRow)
{
    auto backend = std::make_shared<CountingBackend>();
    auto store = openPoolForTest(backend, /*gc_fold_max_defer_rounds=*/0);
    CasRequests requests = openRequestsForTest(backend);
    CasOperation op = requests.admit();
    const Layout & layout = store->layout();

    const CatalogEntry alive = seedStalledCreating(op, store, "00/alive@cas@", UInt128{0x101},
        CreatorFence{.server_root_id = "alive-creator", .writer_epoch = 3, .fence_generation = 1});
    const CatalogEntry lower = seedStalledCreating(op, store, "00/lower@cas@", UInt128{0x102},
        CreatorFence{.server_root_id = "lower-creator", .writer_epoch = 3, .fence_generation = 1});
    const CatalogEntry certified = seedStalledCreating(op, store, "00/certified@cas@", UInt128{0x103},
        CreatorFence{.server_root_id = "moved-creator", .writer_epoch = 1, .fence_generation = 1});
    writeCreatorSlot(*backend, layout, "alive-creator", /*epoch*/ 3, /*gc_fenced*/ false);
    writeCreatorSlot(*backend, layout, "lower-creator", /*epoch*/ 2, /*gc_fenced*/ true);
    retireCreatorRootAt(*backend, layout, "moved-creator", /*epoch*/ 2);

    Gc gc(store, kGc);
    uint64_t erased = 0;
    for (int i = 0; i < 3; ++i)
        erased += runRoundCapturingGate(store, gc, UniversePolicy::Authoritative).drain_cancelled;
    EXPECT_EQ(erased, 1u);
    EXPECT_FALSE(catalogRow(op, layout, certified.ns)) << "positive control";
    EXPECT_EQ(catalogRow(op, layout, alive.ns), std::optional<CatalogEntry>{alive});
    EXPECT_EQ(catalogRow(op, layout, lower.ns), std::optional<CatalogEntry>{lower})
        << "a fence on an older epoch says nothing about this one";
}

/// Each certificate through a real round: a `GC` fence and a clean farewell, both at the row's own epoch.
TEST(CASGCFrontierGate, AGcFenceOrAFarewellAtTheCreatorsEpochCancelsItsRow)
{
    auto backend = std::make_shared<CountingBackend>();
    auto store = openPoolForTest(backend, /*gc_fold_max_defer_rounds=*/0);
    CasRequests requests = openRequestsForTest(backend);
    CasOperation op = requests.admit();
    const Layout & layout = store->layout();

    const CatalogEntry fenced = seedStalledCreating(op, store, "00/fenced@cas@", UInt128{0x111},
        CreatorFence{.server_root_id = "fenced-creator", .writer_epoch = 4, .fence_generation = 1});
    const CatalogEntry farewell = seedStalledCreating(op, store, "00/farewell@cas@", UInt128{0x112},
        CreatorFence{.server_root_id = "farewell-creator", .writer_epoch = 4, .fence_generation = 1});
    writeCreatorSlot(*backend, layout, "fenced-creator", /*epoch*/ 4, /*gc_fenced*/ true);
    retireCreatorRootAt(*backend, layout, "farewell-creator", /*epoch*/ 4);

    Gc gc(store, kGc);
    runRoundCapturingGate(store, gc, UniversePolicy::Authoritative);
    EXPECT_EQ(runRoundCapturingGate(store, gc, UniversePolicy::Authoritative).drain_cancelled, 2u);
    EXPECT_FALSE(catalogRow(op, layout, fenced.ns));
    EXPECT_FALSE(catalogRow(op, layout, farewell.ns));
}

/// The suppression warning of a catalog with no walkable row breaks its rows down by state and points at
/// `cas-fsck` for the creators.
TEST(CASGCFrontierGate, TheNoWalkableRowWarningBreaksTheRowsDownByState)
{
    auto backend = std::make_shared<CountingBackend>();
    auto store = openPoolForTest(backend, /*gc_fold_max_defer_rounds=*/0);
    CasRequests requests = openRequestsForTest(backend);
    CasOperation op = requests.admit();
    seedStalledCreating(op, store, "00/a@cas@", UInt128{0x141},
        CreatorFence{.server_root_id = "never-mounted", .writer_epoch = 1, .fence_generation = 1});
    seedStalledCreating(op, store, "00/b@cas@", UInt128{0x142},
        CreatorFence{.server_root_id = "never-mounted", .writer_epoch = 1, .fence_generation = 1});

    Gc gc(store, kGc);
    ScopedCasGcLogCapture capture;
    for (int i = 0; i < 2; ++i)
        ASSERT_TRUE(runRoundCapturingGate(store, gc, UniversePolicy::Authoritative).suppress_destructive);
    const String log = capture.captured();
    EXPECT_NE(log.find("catalog holds 2 row(s), none walkable/provable: 2 `Creating`, 0 `Live`, 0 `Removing`; "
                       "`cas-fsck` names the creator"), String::npos) << log;
}

/// A creator killed and never started again: its mount slot stops changing, the leader's heartbeat floor
/// fences it once it has watched the slot unchanged long enough, and the next drain cancels the row.
TEST(CASGCFrontierGate, ACreatorThatStopsRenewingIsFencedAndItsRowCancelled)
{
    auto backend = std::make_shared<CountingBackend>();
    auto store = openPoolForTest(backend, /*gc_fold_max_defer_rounds=*/0);
    CasRequests requests = openRequestsForTest(backend);
    CasOperation op = requests.admit();
    const Layout & layout = store->layout();
    const CatalogEntry stalled = seedStalledCreating(op, store, "00/killed@cas@", UInt128{0x131},
        CreatorFence{.server_root_id = "killed-creator", .writer_epoch = 1, .fence_generation = 1});
    writeCreatorSlot(*backend, layout, "killed-creator", /*epoch*/ 1, /*gc_fenced*/ false);

    uint64_t mono_ms = 1'000'000;
    Gc gc(store, kGc, {}, [&] { return mono_ms; });
    const GateVerdict first = runRoundCapturingGate(store, gc, UniversePolicy::Authoritative);
    ASSERT_TRUE(first.saw_fold);
    ASSERT_EQ(catalogRow(op, layout, stalled.ns), std::optional<CatalogEntry>{stalled})
        << "positive control: a slot not yet watched long enough certifies nothing";
    uint64_t cancelled = 0;
    for (int i = 0; i < 6 && catalogRow(op, layout, stalled.ns); ++i)
    {
        mono_ms += 3'600'000;
        cancelled += runRoundCapturingGate(store, gc, UniversePolicy::Authoritative).drain_cancelled;
    }
    EXPECT_FALSE(catalogRow(op, layout, stalled.ns));
    EXPECT_EQ(cancelled, 1u);
    const auto slot = op.read(layout.mountKey("killed-creator"), Retry::once());
    ASSERT_TRUE(slot);
    EXPECT_TRUE(decodeMountLease(slot->bytes).gc_fenced);
}

namespace
{

/// Reopened once, so this server's mount slot holds a later epoch than the one its first open recorded.
PoolPtr openPoolAtSecondEpoch(const std::shared_ptr<DrainRaceBackend> & backend)
{
    {
        auto first = openPoolForTest(backend, /*gc_fold_max_defer_rounds=*/0);
    }
    return openPoolForTest(backend, /*gc_fold_max_defer_rounds=*/0);
}

}

/// A `CREATE` retried on the leader's own server after its remount takes its old `Creating` row over at the
/// later epoch, between the drain's selection read and its cancel. The cancel loses the exact swap, and the
/// opener keeps its life: the same incarnation, a runtime it installed, and appends that land.
TEST(CASGCFrontierGate, AnOpenerThatTakesTheStalledRowOverBeforeTheCancelKeepsItsLife)
{
    auto backend = std::make_shared<DrainRaceBackend>();
    PoolPtr store = openPoolAtSecondEpoch(backend);
    ASSERT_GT(store->writerEpoch(), 1u);
    CasRequests requests = openRequestsForTest(backend);
    CasOperation op = requests.admit();
    const Layout & layout = store->layout();
    const String & root = store->poolConfig().server_root_id;
    Gc gc(store, kGc);
    runRoundCapturingGate(store, gc, UniversePolicy::Authoritative);

    const CatalogEntry stalled = seedStalledCreating(op, store, "00/retried@cas@",
        hexToU128("00000000000000000000000000000081"),
        CreatorFence{.server_root_id = root, .writer_epoch = store->writerEpoch() - 1, .fence_generation = 1});

    bool opener_ran = false;
    size_t opener_done_at = 0;
    backend->afterReadOf(layout.refCatalogKey(), [&]
    {
        backend->afterReadOf(layout.refCatalogKey(), {});
        opener_ran = true;
        publishBirth(store, stalled.ns, "a");
        opener_done_at = backend->journalSnapshot().size();
    });
    const GateVerdict v = runRoundCapturingGate(store, gc, UniversePolicy::Authoritative);
    ASSERT_TRUE(opener_ran);
    EXPECT_EQ(v.drain_cancelled, 0u);

    /// Positive control: after the takeover, the drain still probed the creator of the row it had selected,
    /// before the round's next listing.
    const std::vector<String> journal = backend->journalSnapshot();
    const auto next_list = std::find_if(journal.begin() + opener_done_at, journal.end(),
        [](const String & entry) { return entry.starts_with("list "); });
    EXPECT_NE(std::find(journal.begin() + opener_done_at, next_list, "get " + layout.mountKey(root)), next_list)
        << "the drain never considered the row it selected before the takeover";

    const std::optional<CatalogEntry> row = catalogRow(op, layout, stalled.ns);
    ASSERT_TRUE(row);
    EXPECT_EQ(row->state, NsState::Live);
    EXPECT_EQ(row->incarnation, stalled.incarnation) << "the takeover keeps the incarnation";
    const NamespaceLifeId life = NamespaceLifeId::fromCatalogEntry(stalled.ns, stalled.incarnation);
    EXPECT_EQ(store->namespaceLife(stalled.ns), life);
    RefTxnId second{};
    EXPECT_NO_THROW(second = publishBirth(store, stalled.ns, "b", 2)) << "the winner's runtime stays usable";
    EXPECT_TRUE(op.head(layout.refLogKey(life, second), Retry::once()).has_value());
}

/// The other order: the opener read the stalled row, then the drain cancels it before the takeover is
/// sent. The takeover finds the row gone, and the opener creates a fresh life and appends under it. Its
/// first call may end retry-later, because the cancel's retirement record lands inside its admission.
TEST(CASGCFrontierGate, ACancelThatWinsTheRaceLeavesTheOpenerAFreshLife)
{
    auto backend = std::make_shared<DrainRaceBackend>();
    PoolPtr store = openPoolAtSecondEpoch(backend);
    ASSERT_GT(store->writerEpoch(), 1u);
    CasRequests requests = openRequestsForTest(backend);
    CasOperation op = requests.admit();
    const Layout & layout = store->layout();
    Gc gc(store, kGc);
    runRoundCapturingGate(store, gc, UniversePolicy::Authoritative);

    const CatalogEntry stalled = seedStalledCreating(op, store, "00/retried@cas@",
        hexToU128("00000000000000000000000000000082"),
        CreatorFence{.server_root_id = store->poolConfig().server_root_id, .writer_epoch = store->writerEpoch() - 1, .fence_generation = 1});

    size_t opener_catalog_reads = 0;
    GateVerdict cancelling;
    backend->afterReadOf(layout.refCatalogKey(), [&]
    {
        if (++opener_catalog_reads != 2)
            return;
        backend->afterReadOf(layout.refCatalogKey(), {});
        cancelling = runRoundCapturingGate(store, gc, UniversePolicy::Authoritative);
    });
    try
    {
        publishBirth(store, stalled.ns, "a");
    }
    catch (const DB::Exception & e)
    {
        EXPECT_EQ(e.code(), DB::ErrorCodes::NETWORK_ERROR) << e.message();
    }
    EXPECT_EQ(cancelling.drain_cancelled, 1u);

    const std::optional<CatalogEntry> fresh = catalogRow(op, layout, stalled.ns);
    ASSERT_TRUE(fresh);
    EXPECT_EQ(fresh->state, NsState::Live);
    EXPECT_NE(fresh->incarnation, stalled.incarnation) << "the cancelled life is never reused";
    const NamespaceLifeId fresh_life = NamespaceLifeId::fromCatalogEntry(stalled.ns, fresh->incarnation);
    RefTxnId id{};
    ASSERT_NO_THROW(id = publishBirth(store, stalled.ns, "b", 2));
    EXPECT_TRUE(op.head(layout.refLogKey(fresh_life, id), Retry::once()).has_value());
    EXPECT_EQ(store->namespaceLife(stalled.ns), fresh_life);
}

/// The stalled creator's step 2 lands after the cancel, beside a rebirth of the same name. The late
/// checkpoint names the cancelled incarnation, so it is janitor debris and the new life is untouched.
TEST(CASGCFrontierGate, ALateCheckpointOfACancelledLifeLeavesTheRebornNamespaceIntact)
{
    auto backend = std::make_shared<CountingBackend>();
    auto store = openPoolForTest(backend, /*gc_fold_max_defer_rounds=*/0);
    CasRequests requests = openRequestsForTest(backend);
    CasOperation op = requests.admit();
    const Layout & layout = store->layout();

    const CatalogEntry stalled = seedStalledCreating(op, store, "00/reborn@cas@",
        hexToU128("00000000000000000000000000000091"),
        CreatorFence{.server_root_id = "moved-creator", .writer_epoch = 1, .fence_generation = 1});
    retireCreatorRootAt(*backend, layout, "moved-creator", /*epoch*/ 2);

    Gc gc(store, kGc);
    runRoundCapturingGate(store, gc, UniversePolicy::Authoritative);
    ASSERT_EQ(runRoundCapturingGate(store, gc, UniversePolicy::Authoritative).drain_cancelled, 1u);

    const NamespaceLifeId reborn_life = store->namespaceLife(stalled.ns);
    ASSERT_NE(reborn_life.incarnation, stalled.incarnation);
    store->putNamespaceFile(reborn_life, "format_version.txt", "1\n");
    const std::optional<CatalogEntry> reborn = catalogRow(op, layout, stalled.ns);
    ASSERT_TRUE(reborn && reborn->state == NsState::Live);
    const String reborn_file = layout.namespaceFileKey(reborn_life, "format_version.txt");
    ASSERT_TRUE(op.head(reborn_file, Retry::once()));

    const String late_ckpt = seedStalledCheckpoint(op, layout, stalled);
    for (int i = 0; i < 6 && op.head(late_ckpt, Retry::once()); ++i)
        EXPECT_FALSE(runRoundCapturingGate(store, gc, UniversePolicy::Authoritative).suppress_destructive);

    EXPECT_FALSE(op.head(late_ckpt, Retry::once())) << "the namespace janitor reclaims the late checkpoint";
    EXPECT_EQ(catalogRow(op, layout, stalled.ns), reborn);
    EXPECT_TRUE(op.head(layout.refCkptKey(reborn_life), Retry::once())) << "the reborn life's checkpoint survives";
    EXPECT_TRUE(op.head(reborn_file, Retry::once())) << "and so do its files";
    EXPECT_EQ(store->namespaceLife(stalled.ns), reborn_life);
}

/// The bootstrap-only absent-as-empty representation (`initializeEmptyForNewPool`) must never leak into
/// the operational round: an absent mandatory catalog is corruption, never an empty authority set.
TEST(CASGCFrontierGate, AnAbsentCatalogNeverReadsAsAnEmptyUniverse)
{
    auto backend = std::make_shared<CountingBackend>();
    auto store = openPoolForTest(backend, /*gc_fold_max_defer_rounds=*/0);
    const Layout & layout = store->layout();

    OperationForTest raw_op(*backend);
    const auto catalog_head = (*raw_op).head(layout.refCatalogKey(), Retry::once());
    ASSERT_TRUE(catalog_head.has_value());
    ASSERT_EQ((*raw_op).remove(layout.refCatalogKey(), catalog_head->etag, Retry::once()), Removal::Removed);

    Gc gc(store, kGc);
    backend->resetCounts();
    bool saw_fold = false;
    gc.setPhaseSink([&](const GcPhaseRecord & rec) { if (rec.phase == "fold_reduce") saw_fold = true; });
    try
    {
        gc.runRegularRound({}, /*allow_steal*/true, UniversePolicy::Authoritative);
        FAIL() << "expected the missing mandatory catalog to throw before any fold gate verdict";
    }
    catch (const DB::Exception & e)
    {
        EXPECT_EQ(e.code(), DB::ErrorCodes::CORRUPTED_DATA);
    }
    gc.setPhaseSink({});
    EXPECT_FALSE(saw_fold) << "an absent catalog must abort before the round computes any gate verdict";
    EXPECT_EQ(backend->deleteTotal(), 0u) << "no destructive work may run on an unauthorized round";
}

/// A malformed, truncated, wrong-typed, count-mismatched, or future-versioned catalog must not decode
/// into a `Snapshot` at all -- these are the replacement guards for R11's original "damaged catalog"
/// concern, now that the empty case has a positive proof to keep separate from a broken one.
/// One table-driven test: each row installs a different broken body at the mandatory key and expects
/// decode failure before any destructive work.
///
/// NOT covered here: a header version BELOW `RefCatalog`'s own birth generation. `checkCompatibility`
/// today only rejects a version ABOVE `G_BUILD`; a version below a type's birth floor decodes as if it
/// were legal, and `decodeRefCatalog` discards the parsed header entirely, so "decoded successfully"
/// does not yet imply "legal version for this type". That gap is not load-bearing for THIS proof: the
/// proof is token-present + a full structural decode (type, complete records, matching count trailer,
/// no trailing bytes) + zero entries, and a well-formed-but-out-of-protocol empty catalog is already an
/// accepted residual under the trusted-store model (the token proves byte identity, not history) --
/// closing the version floor would only shrink that residual, not remove it. Tracked separately as
/// `[cas-format-version-floor]` in `BACKLOG.md`; deliberately out of scope for this gate.
TEST(CASGCFrontierGate, AMalformedCatalogNeverDecodesIntoAnEmptyProof)
{
    /// A one-entry catalog's canonical bytes, the base every mutation below starts from.
    RefCatalog one_entry;
    CatalogEntry entry;
    entry.ns = RootNamespace{"00/malformed-base@cas@"};
    entry.state = NsState::Live;
    entry.incarnation = hexToU128("00000000000000000000000000000099");
    one_entry.entries.push_back(entry);
    const String base = encodeRefCatalog(one_entry);
    const String empty_base = encodeRefCatalog(RefCatalog{});

    const String type_needle = fmt::format("\"type\":\"{}\"", traitsFor(FormatId::RefCatalog).type);
    ASSERT_NE(empty_base.find(type_needle), String::npos);
    const String version_needle = fmt::format("\"v\":{}", currentCompatibilityVersion());
    ASSERT_NE(empty_base.find(version_needle), String::npos);
    ASSERT_NE(base.find("\"n\":1"), String::npos);

    const auto replaceOnce = [](const String & haystack, const String & needle, const String & replacement) -> String
    {
        const auto pos = haystack.find(needle);
        EXPECT_NE(pos, String::npos) << "expected to find '" << needle << "'";
        String out = haystack;
        out.replace(pos, needle.size(), replacement);
        return out;
    };

    struct Case { const char * name; String bytes; };
    const std::vector<Case> cases = {
        {"wrong-type", replaceOnce(empty_base, type_needle, "\"type\":\"cas_ref_ckpt\"")},
        /// The one version case the CURRENT (unmodified) gate actually enforces: a version ABOVE
        /// `G_BUILD` is refused by `checkCompatibility` before decode proceeds.
        {"future-version", replaceOnce(empty_base, version_needle, "\"v\":999999")},
        {"trailer-count-mismatch", replaceOnce(base, "\"n\":1", "\"n\":2")},
        /// The trailer line entirely gone: decode's post-entry loop expects another line and hits EOF.
        {"missing-trailer", base.substr(0, base.rfind("{\"n\":1}\n"))},
        /// The trailer present but its own line has no terminator: EOF strictly inside a line.
        {"truncated-mid-line", base.substr(0, base.size() - 2)},
    };

    for (const Case & c : cases)
    {
        auto backend = std::make_shared<CountingBackend>();
        auto store = openPoolForTest(backend, /*gc_fold_max_defer_rounds=*/0);
        const Layout & layout = store->layout();
        OperationForTest raw_op(*backend);
        const auto bootstrap_head = (*raw_op).head(layout.refCatalogKey(), Retry::once());
        ASSERT_TRUE(bootstrap_head.has_value()) << c.name;
        ASSERT_TRUE(std::holds_alternative<Committed>(
            (*raw_op).replace(layout.refCatalogKey(), c.bytes, bootstrap_head->etag, Retry::once()))) << c.name;

        Gc gc(store, kGc);
        backend->resetCounts();
        bool saw_fold = false;
        gc.setPhaseSink([&](const GcPhaseRecord & rec) { if (rec.phase == "fold_reduce") saw_fold = true; });
        EXPECT_THROW(
            gc.runRegularRound({}, /*allow_steal*/true, UniversePolicy::Authoritative), DB::Exception)
            << c.name;
        gc.setPhaseSink({});
        EXPECT_FALSE(saw_fold) << c.name << ": a broken catalog must abort before any fold gate verdict";
        EXPECT_EQ(backend->deleteTotal(), 0u) << c.name << ": no destructive work may run on it";
    }
}

/// `StageA_Suppressed` refuses outright regardless of what the catalog proves -- a proved-empty cut
/// satisfies the frontier term but is not the only term the gate reads.
TEST(CASGCFrontierGate, AProvedEmptyCatalogUnderStageASuppressedStaysSuppressed)
{
    auto backend = std::make_shared<CountingBackend>();
    auto store = openPoolForTest(backend, /*gc_fold_max_defer_rounds=*/0);
    const Layout & layout = store->layout();
    const DB::UInt128 blob(0xbead);

    writeBlobBody(*backend, layout, blob);
    const BlobRef blob_ref = legacyMetaTestRef(blob);
    CasRequests requests = openRequestsForTest(backend);
    CasOperation op = requests.admit();
    const std::optional<Meta> blob_observed = op.head(layout.blobKey(blob_ref), Retry::once());
    ASSERT_TRUE(blob_observed) << "the seeded blob body must be present before it is condemned";
    const PersistedEtag blob_token = PersistedEtag::capture(blob_observed->etag);
    injectRetire(*backend, layout, /*round*/ 1, /*shard*/ 0,
        {RetiredEntry{.kind = ObjectKind::Blob, .ref = blob_ref, .token = blob_token, .size = 0}});
    store->renewWatermarkOnce();

    Gc gc(store, kGc);
    backend->resetCounts();
    for (int i = 0; i < 6; ++i)
    {
        const GateVerdict v = runRoundCapturingGate(store, gc, UniversePolicy::StageA_Suppressed);
        ASSERT_TRUE(v.saw_fold);
        EXPECT_TRUE(v.catalog_proved_empty)
            << "the catalog cut is still genuinely empty -- the fact does not depend on policy";
        EXPECT_FALSE(v.frontier_complete)
            << "StageA_Suppressed refuses outright no matter what the catalog cut proves";
        EXPECT_TRUE(v.suppress_destructive);
    }
    expectEveryDeleteFamilyInert(*backend, "StageA_Suppressed over a proved-empty catalog");
    EXPECT_TRUE(op.head(layout.blobKey(blob_ref), Retry::once()).has_value());
}

/// THE BIRTH-AFTER-EMPTY-CUT BLOB RACE. The proved-empty exception's soundness rests on one hard fact:
/// under this pool's protocol every live or live-precommit edge requires an exact `Live` catalog row
/// (INV-3), so a catalog cut with zero rows proves no namespace ANYWHERE holds one -- AT THAT INSTANT.
/// A namespace born strictly after the cut is invisible to the round that took it; safety for blob
/// CONTENT then rests entirely on the condemned-marker/resurrection protocol (EDGE-BEFORE-OBSERVE:
/// `ContentAddressedTransaction.cpp` persists the precommit edge before observing/uploading the pool
/// blob), never on the frontier proof, which by construction cannot see a birth postdating its own cut.
/// This test pins that: a real writer, through the production `createNamespace` lifecycle
/// (`precommitAdd` on a namespace that has never existed), lands a precommit edge to an
/// ALREADY-CONDEMNED blob strictly after round R's catalog cut but strictly before round R executes the
/// pending delete that cut licensed.
TEST(CASGCFrontierGate, ANamespaceBornAfterTheEmptyCutResurrectsTheCondemnedBlobInstead)
{
    ensureBlobUploadPoolForTest();

    auto backend = std::make_shared<CountingBackend>();
    auto store = openPoolForTest(backend, /*gc_fold_max_defer_rounds=*/0);
    CasRequests requests = openRequestsForTest(backend);
    CasOperation op = requests.admit();
    const Layout & layout = store->layout();
    const RootNamespace doomed{"00/doomed@cas@"};

    const String payload = "empty-cut-birth-race-payload";
    const DB::UInt128 hash = u128Of(payload);
    const BlobRef id = idOf(payload);
    const String key = layout.blobKey(id);
    String raw_body(store->poolMeta().blob_header_len, '\0');
    raw_body += payload;
    writeRawBlobBody(*backend, layout, hash, raw_body);

    const ManifestRef mref{.writer_epoch = 1, .build_sequence = 1, .manifest_ordinal = 1};
    writeManifestRaw(*backend, layout, doomed, mref, {blobEntryFor("data.bin", hash)});
    publishCommittedTransition(*backend, layout, doomed, "ref_1", std::nullopt, mref);

    Gc gc(store, kGc);
    runRegularRoundReclaiming(gc);                                 /// folds the +1 edge
    store->renewWatermarkOnce();
    dropRefTransition(*backend, layout, doomed, "ref_1", mref);
    runRegularRoundReclaiming(gc);                                 /// condemns: intent confirmed, row pending
    store->renewWatermarkOnce();
    const auto condemned_head = op.head(key, Retry::once());
    ASSERT_TRUE(condemned_head.has_value());
    const Etag condemned_token = condemned_head->etag;
    ASSERT_TRUE(objectPresent(*backend, layout.blobIntentKey(id, *condemned_token.canonical())))
        << "the delete round R is about to execute must be backed by a confirmed intent";

    /// Remove `doomed` entirely -- the ONLY way the catalog can become genuinely, provably empty. A raw
    /// `RemoveNamespace` op plus the Removing-state CAS mirrors
    /// `CleanupEvidenceLeavesRemovedNamespaceCheckpointForJanitor`'s own recipe exactly.
    RefOp remove_op;
    remove_op.kind = RefOpKind::RemoveNamespace;
    const uint64_t remove_seq = appendRefLogSeed(*backend, layout, doomed, {remove_op});
    publishRecoverableCkptForSemanticWrapper(*backend, layout, doomed, RefTxnId{1, remove_seq});
    CasRefCatalog::casUpdate(op, layout, [&](const RefCatalog & current) -> RefCatalog
    {
        RefCatalog next = current;
        const auto it = std::find_if(next.entries.begin(), next.entries.end(),
            [&](const CatalogEntry & e) { return e.ns == doomed; });
        EXPECT_NE(it, next.entries.end());
        it->state = NsState::Removing;
        it->removal_started_round = 1;
        return next;
    });

    /// A SUPPRESSED round folds `doomed` through its removal terminal and records `cleanup_evidence` in
    /// the seal, WITHOUT executing the delete of the pending row the condemning round wrote -- suppressed
    /// rounds carry pending deletes forward untouched. `StageA_Suppressed` here is the test's OWN
    /// control over timing, not the scenario under test: it exists only to keep blob X's delete pending
    /// until round R below, rather than letting it drain the ordinary way while `doomed` is still Live.
    gc.runRegularRound({}, /*allow_steal*/true, UniversePolicy::StageA_Suppressed);
    store->renewWatermarkOnce();
    EXPECT_TRUE(op.head(key, Retry::once()).has_value()) << "the pending delete must still be carried, not yet run";

    /// Round R: its pre-fold drain (`drainCompletedRemoving`) reads the round just above's
    /// `cleanup_evidence` and drops `doomed`'s catalog row BEFORE this round's own hot-scan `GET` --
    /// so round R's catalog cut is the first one that is genuinely, provably empty. The hook fires the
    /// instant that cut is taken and races a real namespace birth into the window before round R's own
    /// pre-CAS delete phase runs.
    bool hook_fired = false;
    std::optional<Etag> fresh_token;
    gc.setPostHotScanCatalogReadHookForTest([&]()
    {
        hook_fired = true;
        ASSERT_TRUE(CasRefCatalog::read(op, layout).catalog.entries.empty())
            << "the race must land inside the window where the cut itself is already empty";

        const RootNamespace newborn{"00/newborn@cas@"};
        auto build = store->beginPartWrite(
            PartWriteInfo{.intended_ref = newborn.string() + "/ref_1", .intended_namespace = newborn});
        const ManifestId new_id = build->stageManifest({blobEntryFor("data.bin", hash)});
        build->precommitAdd(newborn, "ref_1", new_id);            /// mints `newborn` via real createNamespace
        const PutBlobResult uploaded = build->putBlob(id, BlobSource::fromString(payload));
        EXPECT_EQ(uploaded.ref, id);
        const auto fresh_head = op.head(key, Retry::once());
        ASSERT_TRUE(fresh_head.has_value());
        fresh_token = fresh_head->etag;
        EXPECT_NE(tokenText(*fresh_token), tokenText(condemned_token))
            << "the writer must have met the intent and resurrected -- a fresh token, not an adopt "
               "of the dying incarnation";
    });

    const GateVerdict verdict = runRoundCapturingGate(store, gc, UniversePolicy::Authoritative);
    ASSERT_TRUE(hook_fired) << "the race hook never fired -- this test proves nothing about the race";
    ASSERT_TRUE(verdict.saw_fold);
    EXPECT_TRUE(verdict.catalog_proved_empty);
    EXPECT_TRUE(verdict.frontier_complete);
    EXPECT_FALSE(verdict.suppress_destructive);

    const auto surviving_head = op.head(key, Retry::once());
    EXPECT_TRUE(surviving_head.has_value())
        << "the resurrected incarnation must survive round R's delete";
    ASSERT_TRUE(fresh_token.has_value());
    EXPECT_EQ(tokenText(surviving_head->etag), tokenText(*fresh_token)) << "and it is still the writer's incarnation";
    EXPECT_EQ(op.remove(key, condemned_token, Retry::once()), Removal::Mismatch)
        << "the condemned token can never remove the fresh object (INV_NO_LOSS)";

    /// A later round's own fresh catalog cut names `newborn`, folds its `+1`, and the blob's frontier is
    /// intact going forward.
    const GateVerdict later = runRoundCapturingGate(store, gc, UniversePolicy::Authoritative);
    ASSERT_TRUE(later.saw_fold);
    EXPECT_EQ(later.frontier_namespaces, 1u);
    EXPECT_EQ(later.frontier_proven, 1u);
    EXPECT_TRUE(op.head(key, Retry::once()).has_value()) << "the newly folded owner keeps the blob alive";
}

/// The generation prune's cursor must not move on a suppressed round either. It is a monotone
/// high-water mark that the wholesale prune never revisits, so a cursor that advanced past a generation
/// this round declined to delete would strand that generation's whole prefix with no reclaimer left.
TEST(CASGCFrontierGate, ASuppressedRoundDoesNotAdvanceTheGenerationPruneCursor)
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
    OperationForTest raw_op(*backend);
    const uint64_t pruned_through_before =
        decodeGcState((*raw_op).read(layout.gcStateKey(), Retry::once())->bytes).snap_pruned_through;

    for (uint64_t i = 7; i <= 10; ++i)
    {
        publish(*backend, layout, ns, "ref_" + std::to_string(i), i, DB::UInt128(0x2000 + i));
        gc.runRegularRound({}, /*allow_steal*/true, UniversePolicy::StageA_Suppressed);
        store->renewWatermarkOnce();
    }

    EXPECT_EQ(decodeGcState((*raw_op).read(layout.gcStateKey(), Retry::once())->bytes).snap_pruned_through,
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
TEST(CASGCFrontierGate, TheHandOffReclaimIsInertUnderSuppression)
{
    auto backend = std::make_shared<CountingBackend>();
    auto store = Pool::open(backend,
        PoolConfig{.pool_prefix = "p", .server_root_id = "test",
                   .gc_snapshot_generations_to_keep = 2, .gc_fold_max_defer_rounds = 0});
    const Layout & layout = store->layout();
    const RootNamespace ns{"00/aa@cas@"};

    const ManifestRef r1 = publish(*backend, layout, ns, "tbl", 1, DB::UInt128(0xa1));

    Gc gc(store, kGc);
    runRegularRoundReclaiming(gc);
    store->renewWatermarkOnce();
    OperationForTest raw_op(*backend);
    const uint64_t old_gen = decodeGcState((*raw_op).read(layout.gcStateKey(), Retry::once())->bytes).snap_generation;
    const String old_prefix = layout.gcGenPrefix(old_gen);
    ASSERT_FALSE((*raw_op).list(old_prefix, "", 1000, Retry::once()).keys.empty());

    /// Idle-carry the ref until the retention cursor is strictly PAST its generation. Until then an
    /// ordinary prune could still reclaim it and the hand-off would not be the load-bearing path.
    for (int i = 0; i < 6; ++i)
    {
        runRegularRoundReclaiming(gc);
        store->renewWatermarkOnce();
    }
    ASSERT_GT(decodeGcState((*raw_op).read(layout.gcStateKey(), Retry::once())->bytes).snap_pruned_through, old_gen)
        << "the generation must be behind the retention cursor before the hand-off is exercised";
    ASSERT_FALSE((*raw_op).list(old_prefix, "", 1000, Retry::once()).keys.empty())
        << "and still retained, because a live ref pins it";

    /// A real delta moves the shard's run off the old generation. This is the round the hand-off would
    /// reclaim it on -- and it supplies no universe.
    const ManifestRef r2{.writer_epoch = 1, .build_sequence = 2, .manifest_ordinal = 1};
    writeBlobBody(*backend, layout, DB::UInt128(0xb2));
    writeManifestRaw(*backend, layout, ns, r2, {blobEntryFor("data.bin", DB::UInt128(0xb2))});
    publishCommittedTransition(*backend, layout, ns, "tbl", r1, r2);

    backend->resetCounts();
    gc.runRegularRound({}, /*allow_steal*/true, UniversePolicy::StageA_Suppressed);

    EXPECT_EQ(backend->deleteCountForKeysContaining("/gc/gen/"), 0u)
        << "a suppressed round hands nothing off. Deleted:" << deletedKeysMessage(*backend);
    EXPECT_FALSE((*raw_op).list(old_prefix, "", 1000, Retry::once()).keys.empty())
        << "the superseded generation's prefix survives a suppressed round intact";

    /// AND THE OPPORTUNITY IS CONSUMED, NOT DEFERRED -- the gate
    /// costs something permanent, so it is asserted here rather than left to be discovered later.
    ///
    /// The hand-off is a one-shot DIFFERENCE: it compares the PARENT seal's runs against the new
    /// seal's, and the suppressed round above already folded the delta, so the next round's parent
    /// seal no longer mentions the old generation. Nothing revisits it -- the retention cursor is
    /// already past it and the prune never goes back. The prefix is left to `fsck`, which is exactly
    /// the outcome the site's own doc comment already records for a crash in the same window ("the
    /// cursor already advanced, so a plain retry will NOT re-attempt it; fsck is the backstop").
    /// Bounded (one small run per shard per occurrence) and not a correctness problem.
    ///
    /// The hand-off itself is not going untested: `CASGCRetention.HandOffDeletesSupersededRef` drives
    /// the same transition on an authoritative round and asserts the prefix IS reclaimed.
    runRegularRoundReclaiming(gc);
    EXPECT_FALSE((*raw_op).list(old_prefix, "", 1000, Retry::once()).keys.empty())
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
TEST(CASGCFrontierGate, TheOrphanManifestSweepAndItsCursorAreInertUnderSuppression)
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
    setWatermarkMinActive(*backend, layout, "test", r1.writer_epoch, /*min_active_build_sequence*/ 0xCA03);
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
        gc.runRegularRound({}, /*allow_steal*/true, UniversePolicy::StageA_Suppressed);
        store->renewWatermarkOnce();
    }

    OperationForTest raw_op(*backend);
    EXPECT_EQ(backend->deleteCountForKeysContaining("/cas/manifests/"), 0u)
        << "a suppressed round sweeps nothing. Deleted:" << deletedKeysMessage(*backend);
    EXPECT_TRUE((*raw_op).head(layout.manifestKey(ManifestId{ns, r1}), Retry::once()).has_value());
    EXPECT_TRUE((*raw_op).head(layout.manifestKey(ManifestId{ns, r2}), Retry::once()).has_value());
    EXPECT_TRUE(decodeGcState((*raw_op).read(layout.gcStateKey(), Retry::once())->bytes).manifest_sweep_cursor.empty())
        << "the sweep cursor must not advance over a range the round declined to sweep -- nothing "
           "revisits it";

    /// The control: the same orphans ARE swept once the universe is authoritative.
    for (int i = 0; i < 4; ++i)
    {
        runRegularRoundReclaiming(gc);
        store->renewWatermarkOnce();
    }
    EXPECT_FALSE((*raw_op).head(layout.manifestKey(ManifestId{ns, r1}), Retry::once()).has_value());
    EXPECT_FALSE((*raw_op).head(layout.manifestKey(ManifestId{ns, r2}), Retry::once()).has_value());
}


/// ===================== QUIET NAMESPACES AND THE PROBE BUDGET =====================

/// THE TALLY ARITHMETIC, at a PARTIAL budget — the case neither 0 nor the default reaches.
///
/// `frontier_namespaces` is the denominator an operator reads as "the round's universe", and the
/// integration test reads it too. A valid checkpoint at every quiet namespace's carried cursor is
/// authoritative independently of LIST and the probe budget: all three lives are proven without
/// successor probes, so the budget leaves no namespace unprobed.
TEST(CASGCFrontierGate, APartialProbeBudgetPublishesATallyThatMatchesTheSealedSet)
{
    auto backend = std::make_shared<CountingHintHoleBackend>();
    CasRequests requests = openRequestsForTest(backend);
    CasOperation op = requests.admit();
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
        const NamespaceLifeId life = *CasRefCatalog::lifeIfCataloged(op, layout, ns);
        const auto checkpoint = readCkpt(op, layout, life);
        ASSERT_TRUE(checkpoint.has_value());
        EXPECT_EQ(checkpoint->ckpt.committed_through, (RefTxnId{1, 1}))
            << "LIST omission and the probe budget do not alter a valid CTE";
    }
}

/// A carried `EpochSeal` has its successor in the next epoch, and the next round folds it there.
TEST(CASGCFrontierGate, CheckpointFrontierCrossesAnInheritedEpochSeal)
{
    auto backend = std::make_shared<CountingBackend>();
    auto store = openPoolForTest(backend, /*gc_fold_max_defer_rounds=*/0);
    CasRequests requests = openRequestsForTest(backend);
    CasOperation op = requests.admit();
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
    const NamespaceLifeId life = *CasRefCatalog::lifeIfCataloged(op, layout, ns);
    const String checkpoint_key = layout.refCkptKey(life);
    const auto checkpoint_head = op.head(checkpoint_key, Retry::once());
    ASSERT_TRUE(checkpoint_head.has_value());
    ASSERT_TRUE(std::holds_alternative<Committed>(op.replace(checkpoint_key, encodeRefCkptAtGeneration(RefCkpt{
        .life_epoch = 1,
        .committed_through = RefTxnId{2, 1},
        .checkpoint_snapshot_id = std::nullopt,
        .last_epoch_seal = RefTxnId{1, 2},
    }, 1), checkpoint_head->etag, Retry::once())));

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
TEST(CASGCFrontierGate, CheckpointFrontierRejectsWrongPredecessorAfterFreshEpochSeal)
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
TEST(CASGCFrontierGate, AWronglyQuietNamespaceIsWalkedTheSameRound)
{
    auto backend = std::make_shared<CountingHintHoleBackend>();
    auto store = openPoolForTest(backend, /*gc_fold_max_defer_rounds=*/0);
    CasRequests requests = openRequestsForTest(backend);
    CasOperation op = requests.admit();
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
    const NamespaceLifeId life = *CasRefCatalog::lifeIfCataloged(op, layout, quiet);
    const String checkpoint_key = layout.refCkptKey(life);
    const auto checkpoint_head = op.head(checkpoint_key, Retry::once());
    ASSERT_TRUE(checkpoint_head.has_value());
    ASSERT_TRUE(std::holds_alternative<Committed>(op.replace(checkpoint_key, encodeRefCkptAtGeneration(RefCkpt{
        .life_epoch = 1,
        .committed_through = RefTxnId{1, 2},
        .checkpoint_snapshot_id = std::nullopt,
        .last_epoch_seal = std::nullopt,
    }, 1), checkpoint_head->etag, Retry::once())));
    backend->hidePrefix(layout.namespaceStreamPrefix(fixture::fixtureLife(quiet)));

    runRegularRoundReclaiming(gc);

    EXPECT_LT(sealed_before, sealedCursorOf(*backend, layout, quiet))
        << "the probe found the hidden record, so the walk folded it and the cursor advanced";
    EXPECT_GT(inDegreeOf(*backend, layout, late_blob), 0)
        << "the hidden publish's edge folded this round -- the hint never mentioned it";
}

/// The checkpoint's inclusive endpoint is itself a durable witness. If that exact log is absent,
/// the namespace is corrupt rather than complete; a 404 at the endpoint must not authorize cleanup.
TEST(CASGCFrontierGate, MissingCommittedCheckpointLogHoldsInsteadOfProvingTheFrontier)
{
    auto backend = std::make_shared<CountingBackend>();
    auto store = openPoolForTest(backend, /*gc_fold_max_defer_rounds=*/0);
    CasRequests requests = openRequestsForTest(backend);
    CasOperation op = requests.admit();
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

    const NamespaceLifeId life = *CasRefCatalog::lifeIfCataloged(op, layout, ns);
    const String missing_key = layout.refLogKey(life, RefTxnId{1, 2});
    const auto missing_head = op.head(missing_key, Retry::once());
    ASSERT_TRUE(missing_head.has_value());
    ASSERT_EQ(op.remove(missing_key, missing_head->etag, Retry::once()), Removal::Removed);

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
TEST(CASGCFrontierGate, HiddenCommittedCheckpointLogIsFoldedThroughTheAuthorityCeiling)
{
    auto backend = std::make_shared<CountingHintHoleBackend>();
    auto store = openPoolForTest(backend, /*gc_fold_max_defer_rounds=*/0);
    CasRequests requests = openRequestsForTest(backend);
    CasOperation op = requests.admit();
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

    const NamespaceLifeId life = *CasRefCatalog::lifeIfCataloged(op, layout, ns);
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

/// A never-folded life whose first key reads absent is complete.
TEST(CASGCFrontierGate, EmptyCheckpointFrontierProvesAnUnfoldedLife)
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

/// A catalog `Live` life without its exact checkpoint cannot derive either its genesis or a frontier
/// from the ref LIST. Even a durable listed first log must be retained until the authority is repaired.
TEST(CASGCFrontierGate, CatalogLifeWithoutCheckpointDefersWithoutUsingListedFrontier)
{
    auto backend = std::make_shared<CountingBackend>();
    auto store = openPoolForTest(backend, /*gc_fold_max_defer_rounds=*/0);
    CasRequests requests = openRequestsForTest(backend);
    CasOperation op = requests.admit();
    const Layout & layout = store->layout();
    const RootNamespace ns{"00/missing-checkpoint-fold@cas@"};
    const DB::UInt128 blob(0xc7);

    fixture::admitLive(*backend, layout, ns);
    const ManifestRef manifest{.writer_epoch = 1, .build_sequence = 1, .manifest_ordinal = 1};
    writeBlobBody(*backend, layout, blob);
    writeManifestRaw(*backend, layout, ns, manifest, {blobEntryFor("data.bin", blob)});
    appendRefLogSeed(*backend, layout, ns, publishCommittedOps("must_remain_unfolded", manifest));

    const NamespaceLifeId life = *CasRefCatalog::lifeIfCataloged(op, layout, ns);
    ASSERT_FALSE(readCkpt(op, layout, life).has_value());

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
TEST(CASGCFrontierGate, AnExhaustedProbeBudgetSealsCursorsAndDeletesNothing)
{
    auto backend = std::make_shared<CountingHintHoleBackend>();
    CasRequests requests = openRequestsForTest(backend);
    CasOperation op = requests.admit();
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
    drive(store, gc, /*rounds*/ 5, UniversePolicy::Authoritative);

    EXPECT_GT(backend->deleteTotal(), 0u)
        << "the quiet life's checkpoint authority leaves unrelated deletion eligible";
    EXPECT_FALSE(op.head(blobKeyOf(layout, blob), Retry::once()).has_value())
        << "the busy life's removal remains reclaimable despite the quiet LIST omission";
    EXPECT_EQ(sealedCursorOf(*backend, layout, quiet), quiet_cursor)
        << "the unprobed namespace's cursor rides verbatim -- it is never dropped";
    const NamespaceLifeId quiet_life = *CasRefCatalog::lifeIfCataloged(op, layout, quiet);
    const auto quiet_checkpoint = readCkpt(op, layout, quiet_life);
    ASSERT_TRUE(quiet_checkpoint.has_value());
    EXPECT_EQ(quiet_checkpoint->ckpt.committed_through, quiet_cursor)
        << "the quiet life's valid CTE is unaffected by LIST omission and a zero probe budget";
    EXPECT_GT(decodeGcState(op.read(layout.gcStateKey(), Retry::once())->bytes).round, 1u)
        << "the round still commits; only its destructive half is withheld";
}

/// ===================== A COMMITTED GAP IS REDETECTED UNTIL REPAIRED =====================
///
/// A hold's committed checkpoint frontier remains a durable witness of its own gap. Hiding the later
/// log from LIST cannot make that gap quiet: every retry exact-reads the missing position, redetects the
/// hold, and suppresses destructive work until an operator repairs the record stream.
TEST(CASGCFrontierGate, ACommittedGapIsRedetectedAndSuppressesEveryRound)
{
    auto backend = std::make_shared<CountingHintHoleBackend>();
    auto store = openPoolForTest(backend, /*gc_fold_max_defer_rounds=*/0);
    CasRequests requests = openRequestsForTest(backend);
    CasOperation op = requests.admit();
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

    const NamespaceLifeId held_life = *CasRefCatalog::lifeIfCataloged(op, layout, held);
    const auto held_checkpoint = readCkpt(op, layout, held_life);
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

    drive(store, gc, /*rounds*/ 4, UniversePolicy::Authoritative);

    EXPECT_EQ(backend->deleteTotal(), 0u)
        << "the re-detected committed gap suppresses each round's destructive work. "
           "Deleted:" << deletedKeysMessage(*backend);
    EXPECT_TRUE(op.head(blobKeyOf(layout, blob), Retry::once()).has_value());
    EXPECT_EQ(sealedCursorOf(*backend, layout, held), (RefTxnId{1, 2}))
        << "the committed gap remains unresolved and the cursor cannot advance through it";
    const auto final_checkpoint = readCkpt(op, layout, held_life);
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
TEST(CASGCFrontierGate, ABlobCondemnedThisRoundIsNeverDeletedThisRound)
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

    OperationForTest raw_op(*backend);
    EXPECT_TRUE((*raw_op).head(blobKeyOf(layout, blob), Retry::once()).has_value())
        << "the condemning round must not also delete";
    EXPECT_EQ(backend->deleteCount(blobKeyOf(layout, blob)), 0u)
        << "not merely still present -- the delete was never attempted";
}

/// ARM (c) of the temporal lemma is the delete-site in-degree re-read, and it is NORMATIVE (spec §5,
/// third arm): an edge folded AFTER the condemnation but BEFORE the delete pass spares the blob
/// outright, `indeg > 0` winning over `delete_pending` past the floor. The other two arms bound WHEN
/// and WHAT a delete may remove; only this one asks whether the blob is still referenced at the moment
/// the pass decides.
TEST(CASGCFrontierGate, ALateEdgeSparesADeletePendingBlobAtTheDeleteSite)
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

    /// Condemn it: the intent is confirmed and the row pending.
    dropRefTransition(*backend, layout, ns, "ref_1", mref);
    runRegularRoundReclaiming(gc);            /// condemn
    store->renewWatermarkOnce();

    /// A new owner appears BEFORE the delete pass. The pass recomputes the in-degree from the merge it
    /// just ran and finds it nonzero.
    const ManifestRef revived{.writer_epoch = 1, .build_sequence = 42, .manifest_ordinal = 1};
    writeManifestRaw(*backend, layout, ns, revived, {blobEntryFor("data.bin", blob)});
    publishCommittedTransition(*backend, layout, ns, "revived_ref", std::nullopt, revived);

    runRegularRoundReclaiming(gc);
    store->renewWatermarkOnce();

    OperationForTest raw_op(*backend);
    EXPECT_TRUE((*raw_op).head(blobKeyOf(layout, blob), Retry::once()).has_value())
        << "the delete-site in-degree re-read spares a blob a fresh edge re-referenced";
    EXPECT_GT(inDegreeOf(*backend, layout, blob), 0);
}

/// ARM (b): a TOKENED adoption of an already-delete-pending blob. The writer's admit gate reads the
/// incarnation's intent, refuses to adopt the dying incarnation, and rematerializes from its own source as
/// a FRESH incarnation -- so the delayed exact-token delete the previous round published finds a
/// different token and removes nothing. The blob's identity is preserved by re-upload, never by
/// reviving the condemned object.
TEST(CASGCFrontierGate, AResurrectedIncarnationSurvivesTheDelayedStaleTokenDelete)
{
    ensureBlobUploadPoolForTest();

    auto backend = std::make_shared<CountingBackend>();
    auto store = openPoolForTest(backend, /*gc_fold_max_defer_rounds=*/0);
    const Layout & layout = store->layout();
    const RootNamespace ns{"00/aa@cas@"};

    /// A REAL content-addressed blob, so the writer path below addresses exactly the object GC condemns.
    const String payload = "frontier-gate-republish-payload";
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
    runRegularRoundReclaiming(gc);            /// condemn: the intent of THIS token, the row pending
    store->renewWatermarkOnce();

    OperationForTest raw_op(*backend);
    const auto condemned_head = (*raw_op).head(key, Retry::once());
    ASSERT_TRUE(condemned_head.has_value());
    const Etag condemned_token = condemned_head->etag;
    ASSERT_TRUE(objectPresent(*backend, layout.blobIntentKey(id, *condemned_token.canonical())))
        << "the delete GC is about to execute must be backed by a confirmed intent";

    /// A writer now adopts the blob through the REAL admit gate. It reads the intent, refuses to adopt the
    /// dying incarnation, and rematerializes from its OWN source bytes -- never by reading the condemned
    /// object. The key ends up holding a DIFFERENT incarnation.
    PartWriteInfo info;
    info.intended_ref = ns.string() + "/republished";
    auto build = store->beginPartWrite(info);
    const ManifestId republished_manifest
        = build->stageManifest({blobEntryFor("data.bin", hash, payload.size())});
    build->precommitAdd(ns, "republished", republished_manifest);
    const PutBlobResult uploaded = build->putBlob(id, BlobSource::fromString(payload));
    EXPECT_EQ(uploaded.ref, id);
    build->promote(ns, "republished", build->buildId(), republished_manifest);
    const auto fresh_head = (*raw_op).head(key, Retry::once());
    ASSERT_TRUE(fresh_head.has_value());
    const Etag fresh_token = fresh_head->etag;
    ASSERT_NE(tokenText(fresh_token), tokenText(condemned_token)) << "republication must displace the condemned incarnation";

    /// GC's delayed delete still names the OLD token. It cannot touch the new object.
    drive(store, gc, /*rounds*/ 2, UniversePolicy::Authoritative);

    const auto surviving_head = (*raw_op).head(key, Retry::once());
    ASSERT_TRUE(surviving_head.has_value())
        << "the resurrected incarnation survives the delete published against its predecessor";
    EXPECT_EQ(tokenText(surviving_head->etag), tokenText(fresh_token)) << "and it is still the writer's incarnation";
    EXPECT_EQ((*raw_op).remove(key, condemned_token, Retry::once()), Removal::Mismatch)
        << "the condemned token can never remove the fresh object (INV-NO-RETURN)";
}

/// ARM (c): a TOKENLESS relink -- the receiver adopts by evidence, holding no token at all. Safety
/// then rests entirely on ORDER, so the operation journal has to show it: the receiver's `+1` is
/// durable BEFORE the source releases its own committed edge, and no point in the schedule leaves the
/// blob with zero durable owners.
TEST(CASGCFrontierGate, ATokenlessRelinkMakesTheReceiverEdgeDurableBeforeTheSourceReleases)
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
    drive(store, gc, /*rounds*/ 4, UniversePolicy::Authoritative);

    OperationForTest raw_op(*backend);
    EXPECT_TRUE((*raw_op).head(blobKeyOf(layout, blob), Retry::once()).has_value())
        << "the source released its edge only after the receiver's was durable, so nothing may collect it";
    EXPECT_EQ(inDegreeOf(*backend, layout, blob), 1)
        << "the receiver is the sole remaining owner";
}

/// ===================== CLEANUP RANGES ARE COMPUTED, NOT ENUMERATED =====================
///
/// `planRefCleanup` is pure, so the boundary arithmetic is pinned directly rather than inferred from a
/// round's side effects. Its sole coverage authority is the checkpoint-named base; a listed snapshot
/// is merely a physical observation until the same-id triple has been validated.

TEST(CASGCFrontierGateCleanupRange, CoveredLogsStopAtTheMinimumOfCheckpointAndCursor)
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

TEST(CASGCFrontierGateCleanupRange, ASnapshotAtTheCheckpointSurvivesAndOnlyStrictlyOlderOnesGo)
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
TEST(CASGCFrontierGateCleanupRange, CheckpointBaseValidatorRejectsMissingLogSnapshotAndSeal)
{
    auto backend = std::make_shared<InMemoryBackend>();
    CasRequests requests = openRequestsForTest(backend);
    CasOperation op = requests.admit();
    const Layout layout{"p"};
    const RefTxnId base{1, 1};
    const RefCkpt checkpoint{
        .life_epoch = 1,
        .committed_through = base,
        .checkpoint_snapshot_id = base,
        .last_epoch_seal = std::nullopt};
    CasRefCatalog::initializeEmptyForNewPool(op, layout);

    {
        const RootNamespace ns{"00/cleanup-missing-base-log@cas@"};
        fixture::admitLive(*backend, layout, ns);
        const NamespaceLifeId life = CasRefCatalog::lifeIfCataloged(op, layout, ns).value();
        writeRefSnapshotRaw(*backend, layout, minimalLiveSnapshot(ns.string(), base));
        EXPECT_THROW((void)readCheckpointSnapshotBase(op, layout, life, checkpoint), DB::Exception);
    }
    {
        const RootNamespace ns{"00/cleanup-missing-base-snapshot@cas@"};
        fixture::writeRefLogRaw(*backend, layout, RefLogTxn{
            .ns = ns.string(), .txn_id = base, .ops = {namespaceBirthOp()}, .prev_epoch_seal = std::nullopt});
        const NamespaceLifeId life = CasRefCatalog::lifeIfCataloged(op, layout, ns).value();
        EXPECT_THROW((void)readCheckpointSnapshotBase(op, layout, life, checkpoint), DB::Exception);
    }
    {
        const RootNamespace ns{"00/cleanup-seal-is-not-base@cas@"};
        writeSealAt(*backend, layout, ns, base);
        const NamespaceLifeId life = CasRefCatalog::lifeIfCataloged(op, layout, ns).value();
        writeRefSnapshotRaw(*backend, layout, minimalLiveSnapshot(ns.string(), base));
        EXPECT_THROW((void)readCheckpointSnapshotBase(op, layout, life, checkpoint), DB::Exception);
    }
}

TEST(CASGCFrontierGateCleanupRange, LaterEpochBaseWithoutItsContextualBacklinkCannotLicenseDeletion)
{
    auto backend = std::make_shared<InMemoryBackend>();
    CasRequests requests = openRequestsForTest(backend);
    CasOperation op = requests.admit();
    const Layout layout{"p"};
    CasRefCatalog::initializeEmptyForNewPool(op, layout);
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
        const NamespaceLifeId life = CasRefCatalog::lifeIfCataloged(op, layout, ns).value();

        std::optional<RefTxnId> validated_base;
        try
        {
            (void)readCheckpointSnapshotBase(op, layout, life, RefCkpt{
                .life_epoch = 1,
                .committed_through = base_id,
                .checkpoint_snapshot_id = base_id,
                .last_epoch_seal = seal_id});
            validated_base = base_id;
        }
        catch (const DB::Exception &) // NOLINT(bugprone-empty-catch): failure to validate is the tested case -- `validated_base` deliberately stays nullopt
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
TEST(CASGCFrontierGate, CleanupEvidenceLeavesRemovedNamespaceCheckpointForJanitor)
{
    auto backend = std::make_shared<CountingBackend>();
    auto store = openPoolForTest(backend, /*gc_fold_max_defer_rounds=*/0);
    CasRequests requests = openRequestsForTest(backend);
    CasOperation op = requests.admit();
    const Layout & layout = store->layout();
    const RootNamespace removed{"00/removed@cas@"};
    const RefOp birth_op = namespaceBirthOp();
    RefOp remove_op;
    remove_op.kind = RefOpKind::RemoveNamespace;
    fixture::writeRefLogRaw(*backend, layout, RefLogTxn{
        .ns = removed.string(), .txn_id = RefTxnId{1, 1}, .ops = {birth_op}, .prev_epoch_seal = std::nullopt});
    fixture::writeRefLogRaw(*backend, layout, RefLogTxn{
        .ns = removed.string(), .txn_id = RefTxnId{1, 2}, .ops = {remove_op}, .prev_epoch_seal = std::nullopt});
    const NamespaceLifeId life = CasRefCatalog::lifeIfCataloged(op, layout, removed).value();
    CasRefCatalog::casUpdate(op, layout, [&](const RefCatalog & current)
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
    op.create(ckpt_key, encodeRefCkptAtGeneration(RefCkpt{
        .life_epoch = 1,
        .committed_through = RefTxnId{1, 2},
        .checkpoint_snapshot_id = std::nullopt,
        .last_epoch_seal = std::nullopt,
    }, 1), Retry::once());

    /// The removal evidence must arise from a replay-valid terminal lifecycle, rather than merely
    /// from a raw terminal record that the recovery state machine refuses.
    const RecoveredRefTable recovered = recoverRefTableDetailedAtCatalogCutForTest(
        *backend, layout, CasRefCatalog::read(op, layout), removed);
    EXPECT_EQ(recovered.state.getLifecycle(), RefLifecycle::Removed);
    EXPECT_EQ(recovered.state.getRemoveTxnId(), (RefTxnId{1, 2}));

    Gc gc(store, kGc);
    ASSERT_TRUE(runRegularRoundReclaiming(gc).acquired_lease);

    const GcState st = decodeGcState(op.read(layout.gcStateKey(), Retry::once())->bytes);
    const CasFoldSeal seal = decodeFoldSeal(
        op.read(layout.foldSealKey(st.snap_generation, st.snap_attempt), Retry::once())->bytes);
    const auto row_it = seal.ref_lives.find(life.incarnation);
    ASSERT_NE(row_it, seal.ref_lives.end());
    ASSERT_TRUE(row_it->second.cleanup_evidence.has_value());
    EXPECT_EQ(row_it->second.cleanup_evidence->remove_txn_id, (RefTxnId{1, 2}));
    EXPECT_TRUE(op.head(ckpt_key, Retry::once()).has_value());
    for (const String & key : backend->touchedKeys())
        EXPECT_EQ(key.find("/_cleanup/"), String::npos) << key;

    /// Round 2 drops `removed`'s catalog row (the pre-fold drain, using round 1's `cleanup_evidence`),
    /// which makes THIS round's own hot-scan catalog cut genuinely, provably empty -- so its destructive
    /// gate opens for the first time (`catalog_cut_proved_empty`), and the namespace janitor -- a
    /// separate `namespace_cleanup` phase the SAME round call also runs -- reclaims the now-orphaned
    /// checkpoint. Reclaiming a removed namespace's `_ckpt` once the pool empties is exactly the
    /// standstill this gate exists to fix, so the janitor running here is the fix working, not a
    /// regression. What this test still pins is the DISCRIMINATION the title promises: the FOLD stage
    /// itself performs no lifecycle-specific physical cleanup (asserted above, unchanged), and the
    /// janitor is attributed the delete via its OWN phase counters -- never inferred from end-state
    /// absence, which would not distinguish "the janitor did it" from "something else did".
    std::map<String, UInt64> janitor_metrics;
    gc.setPhaseSink([&](const GcPhaseRecord & rec)
    {
        if (rec.phase == "namespace_cleanup")
            janitor_metrics = rec.metrics;
    });
    ASSERT_TRUE(runRegularRoundReclaiming(gc).acquired_lease);
    gc.setPhaseSink({});

    EXPECT_TRUE(CasRefCatalog::read(op, layout).catalog.entries.empty());
    EXPECT_FALSE(CasRefCatalog::lifeIfCataloged(op, layout, removed));
    ASSERT_FALSE(janitor_metrics.empty()) << "the namespace_cleanup phase must have run this round";
    EXPECT_GE(janitor_metrics.at("janitor_deleted"), 1u)
        << "the janitor's OWN counter must show the delete -- now that the proved-empty gate has "
           "opened, not because some other site happened to remove the key";
    EXPECT_FALSE(op.head(ckpt_key, Retry::once()).has_value());
    EXPECT_EQ(backend->deleteCount(ckpt_key), 1);
}

/// Once a terminal has folded, a later physical read failure is janitor debt, not lifecycle evidence
/// loss. Removing this per-key leak handling would either make the signal disappear or let one dead
/// object prevent the janitor from considering the rest of its page. The unreadable object is the dead
/// checkpoint: dead `_log`/`_snap` keys are deleted token-free and never read.
TEST(CASGCFrontierGate, PostFoldUnreadableDeadCheckpointIsCountedWithoutSuppressingProgress)
{
    auto backend = std::make_shared<DeadCheckpointHeadFailureBackend>();
    auto store = openPoolForTest(backend, /*gc_fold_max_defer_rounds=*/0);
    CasRequests requests = openRequestsForTest(backend);
    CasOperation op = requests.admit();
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
    const NamespaceLifeId removed_life = CasRefCatalog::lifeIfCataloged(op, layout, removed).value();
    CasRefCatalog::casUpdate(op, layout, [&](const RefCatalog & current)
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
    ASSERT_TRUE(std::holds_alternative<Committed>(op.create(layout.refCkptKey(removed_life), encodeRefCkptAtGeneration(RefCkpt{
        .life_epoch = 1,
        .committed_through = RefTxnId{1, 2},
        .checkpoint_snapshot_id = std::nullopt,
        .last_epoch_seal = std::nullopt,
    }, 1), Retry::once())));

    const DB::UInt128 blob(0xfeed);
    const ManifestRef manifest = publish(*backend, layout, progressing, "victim", 1, blob);
    const ManifestId manifest_id{progressing, manifest};

    Gc gc(store, kGc);
    ASSERT_TRUE(runRegularRoundReclaiming(gc).acquired_lease);
    const GcState folded_state = decodeGcState(op.read(layout.gcStateKey(), Retry::once())->bytes);
    const CasFoldSeal folded_seal = decodeFoldSeal(
        op.read(layout.foldSealKey(folded_state.snap_generation, folded_state.snap_attempt), Retry::once())->bytes);
    const auto folded_row = folded_seal.ref_lives.find(removed_life.incarnation);
    ASSERT_NE(folded_row, folded_seal.ref_lives.end());
    ASSERT_TRUE(folded_row->second.cleanup_evidence.has_value());

    dropRefTransition(*backend, layout, progressing, "victim", manifest);
    const String terminal_key = layout.refLogKey(removed_life, RefTxnId{1, 2});
    const String checkpoint_key = layout.refCkptKey(removed_life);
    const String later_dead_residue = layout.refLogKey(removed_life, RefTxnId{1, 3});
    ASSERT_TRUE(std::holds_alternative<Committed>(
        op.create(later_dead_residue, "dead residue after the folded terminal", Retry::once())));
    backend->makeUnreadable(checkpoint_key);

    std::map<String, UInt64> namespace_cleanup;
    const uint64_t leaks_before
        = ProfileEvents::global_counters[ProfileEvents::CASGCNamespaceCleanupLeaks];
    gc.setPhaseSink([&](const GcPhaseRecord & record)
    {
        if (record.phase == "namespace_cleanup")
            namespace_cleanup = record.metrics;
    });
    ScopedCasGcLogCapture log_capture;
    const RoundReport report = runRegularRoundReclaiming(gc);
    gc.setPhaseSink({});

    ASSERT_TRUE(report.acquired_lease);
    EXPECT_FALSE(CasRefCatalog::lifeIfCataloged(op, layout, removed))
        << "post-fold physical cleanup cannot gate catalog removal";
    EXPECT_TRUE(CasRefCatalog::lifeIfCataloged(op, layout, progressing));
    EXPECT_EQ(report.manifests_deleted, 1u)
        << "the janitor leak cannot promote itself into pool-wide destructive suppression";
    EXPECT_FALSE(op.head(layout.manifestKey(manifest_id), Retry::once()).has_value());
    EXPECT_TRUE(backend->existsIgnoringFault(checkpoint_key));
    EXPECT_FALSE(backend->existsIgnoringFault(terminal_key));
    EXPECT_FALSE(backend->existsIgnoringFault(later_dead_residue))
        << "one unreadable key cannot stop the perpetual janitor from deciding the rest of its page";
    ASSERT_FALSE(namespace_cleanup.empty());
    EXPECT_EQ(namespace_cleanup["leaked"], 1u);
    EXPECT_EQ(
        ProfileEvents::global_counters[ProfileEvents::CASGCNamespaceCleanupLeaks] - leaks_before,
        1u);
    const String captured = log_capture.captured();
    EXPECT_NE(captured.find(checkpoint_key), String::npos);
    EXPECT_NE(captured.find("leak"), String::npos);
}

TEST(CASGCFrontierGate, UnmatchedAdoptedParentLifeDoesNotSuppressAuthoritativeDeletion)
{
    auto backend = std::make_shared<CountingBackend>();
    auto store = openPoolForTest(backend, /*gc_fold_max_defer_rounds=*/0);
    const Layout & layout = store->layout();
    const RootNamespace ns{"00/unmatched-parent@cas@"};
    const DB::UInt128 blob(0xcafe);
    const ManifestRef mref = publish(*backend, layout, ns, "victim", 1, blob);
    const ManifestId manifest_id{ns, mref};

    Gc gc(store, kGc);
    OperationForTest raw_op(*backend);
    ASSERT_TRUE(runRegularRoundReclaiming(gc).acquired_lease);
    ASSERT_TRUE((*raw_op).head(layout.manifestKey(manifest_id), Retry::once()).has_value());

    const GcState before = decodeGcState((*raw_op).read(layout.gcStateKey(), Retry::once())->bytes);
    const String parent_seal_key = layout.foldSealKey(before.snap_generation, before.snap_attempt);
    const auto parent_object = (*raw_op).read(parent_seal_key, Retry::once());
    ASSERT_TRUE(parent_object);
    CasFoldSeal parent = decodeFoldSeal(parent_object->bytes, before.snap_generation);
    const UInt128 unmatched_life = hexToU128("fedcba98765432100123456789abcdef");
    ASSERT_FALSE(parent.ref_lives.contains(unmatched_life));
    parent.ref_lives.emplace(unmatched_life, RefLifeFoldState{
        .coverage = RefCoverage{.classification = CoverageClass::Folded, .last_folded_ref_id = RefTxnId{9, 9}}});
    ASSERT_TRUE(std::holds_alternative<Committed>(
        (*raw_op).replace(parent_seal_key, encodeFoldSeal(parent), parent_object->etag, Retry::once())));

    dropRefTransition(*backend, layout, ns, "victim", mref);
    const uint64_t events_before =
        ProfileEvents::global_counters[ProfileEvents::CASGCUnmatchedAdoptedParentLives];
    const RoundReport report = runRegularRoundReclaiming(gc);

    ASSERT_TRUE(report.acquired_lease);
    EXPECT_EQ(
        ProfileEvents::global_counters[ProfileEvents::CASGCUnmatchedAdoptedParentLives] - events_before,
        1u);
    EXPECT_EQ(report.manifests_deleted, 1u)
        << "an unmatched adopted-parent row is observed and dropped, not promoted to pool-wide suppression";
    EXPECT_FALSE((*raw_op).head(layout.manifestKey(manifest_id), Retry::once()).has_value())
        << "the valid manifest candidate must be physically deleted by the same authoritative round";

    const GcState after = decodeGcState((*raw_op).read(layout.gcStateKey(), Retry::once())->bytes);
    const CasFoldSeal successor = decodeFoldSeal(
        (*raw_op).read(layout.foldSealKey(after.snap_generation, after.snap_attempt), Retry::once())->bytes,
        after.snap_generation);
    EXPECT_FALSE(successor.ref_lives.contains(unmatched_life));
}

TEST(CASCatalogLifecycleReconciler, EmptyCatalogReturnsAuthoritativeCompleteCut)
{
    auto backend = std::make_shared<DrainRaceBackend>();
    auto store = openPoolForTest(backend, /*gc_fold_max_defer_rounds=*/0);
    CasRequests requests = openRequestsForTest(backend);
    CasOperation op = requests.admit();
    const Layout & layout = store->layout();
    ASSERT_TRUE(CasRefCatalog::initializeEmptyForNewPool(op, layout).catalog.entries.empty());

    CasFoldSeal parent;
    CatalogLifecycleReconciler reconciler(op, layout, parent, noCreatorProvedTerminal);
    const CatalogLifecycleReconcileResult result = reconciler.reconcile(noAuthorityRefresh, noRetirementRecord);

    EXPECT_EQ(result.authority_status, AuthorityStatus::Authoritative);
    EXPECT_EQ(result.catalog_resolution, CatalogResolution::DrainComplete);
    ASSERT_TRUE(result.final_catalog_cut);
    EXPECT_TRUE(result.final_catalog_cut->catalog.entries.empty());
    EXPECT_TRUE(reconciler.retiredLives().empty());
    EXPECT_EQ(reconciler.completedErases(), 0);
}

TEST(CASCatalogLifecycleReconciler, DeletesEligibleRowsFromReturnedResolutionCuts)
{
    auto backend = std::make_shared<DrainRaceBackend>();
    auto store = openPoolForTest(backend, /*gc_fold_max_defer_rounds=*/0);
    CasRequests requests = openRequestsForTest(backend);
    CasOperation op = requests.admit();
    const Layout & layout = store->layout();
    constexpr size_t deletes = 3;
    seedCompletedRemovingBatch(op, store, kGc, deletes);
    const auto parent_object = op.read(layout.foldSealKey(1, 1), Retry::once());
    ASSERT_TRUE(parent_object);
    const CasFoldSeal parent = decodeFoldSeal(parent_object->bytes);
    backend->clearJournal();
    backend->resetCounts();

    CatalogLifecycleReconciler reconciler(op, layout, parent, noCreatorProvedTerminal);
    const CatalogLifecycleReconcileResult result = reconciler.reconcile(noAuthorityRefresh, noRetirementRecord);

    EXPECT_EQ(result.authority_status, AuthorityStatus::Authoritative);
    EXPECT_EQ(result.catalog_resolution, CatalogResolution::DrainComplete);
    EXPECT_EQ(reconciler.completedErases(), deletes);
    ASSERT_EQ(reconciler.retiredLives().size(), deletes);
    ASSERT_TRUE(result.final_catalog_cut);
    EXPECT_TRUE(result.final_catalog_cut->catalog.entries.empty());
    const std::vector<String> journal = backend->journalSnapshot();
    const String catalog_get = "get " + layout.refCatalogKey();
    EXPECT_EQ(std::count(journal.begin(), journal.end(), catalog_get), deletes + 1);
}

/// The retirement announcement precedes the erase of its row: a record made afterwards leaves a
/// window in which the life is gone but unrecorded.
TEST(CASCatalogLifecycleReconciler, AnnouncesEachLifeBeforeItsRowIsErased)
{
    auto backend = std::make_shared<DrainRaceBackend>();
    auto store = openPoolForTest(backend, /*gc_fold_max_defer_rounds=*/0);
    CasRequests requests = openRequestsForTest(backend);
    CasOperation op = requests.admit();
    const Layout & layout = store->layout();
    constexpr size_t deletes = 3;
    seedCompletedRemovingBatch(op, store, kGc, deletes);
    const auto parent_object = op.read(layout.foldSealKey(1, 1), Retry::once());
    ASSERT_TRUE(parent_object);
    const CasFoldSeal parent = decodeFoldSeal(parent_object->bytes);

    std::vector<NamespaceLifeId> announced;
    size_t announced_while_cataloged = 0;
    CatalogLifecycleReconciler reconciler(op, layout, parent, noCreatorProvedTerminal);
    const CatalogLifecycleReconcileResult result = reconciler.reconcile(
        noAuthorityRefresh,
        [&](const NamespaceLifeId & life)
        {
            announced.push_back(life);
            if (CasRefCatalog::lifeIfCataloged(op, layout, life.ns) == std::optional<NamespaceLifeId>{life})
                ++announced_while_cataloged;
        });

    EXPECT_EQ(reconciler.completedErases(), deletes);
    EXPECT_EQ(announced, reconciler.retiredLives()) << "each life is announced exactly once, in erase order";
    EXPECT_EQ(announced_while_cataloged, deletes) << "the announcement must precede the erase of its row";
}

TEST(CASCatalogLifecycleReconciler, ReturnsRetiredLifeWhenAuthorityMovesAfterResolution)
{
    auto backend = std::make_shared<DrainRaceBackend>();
    auto store = openPoolForTest(backend, /*gc_fold_max_defer_rounds=*/0);
    CasRequests requests = openRequestsForTest(backend);
    CasOperation op = requests.admit();
    const Layout & layout = store->layout();
    const CompletedRemovingFixture fixture = seedCompletedRemoving(op, store, kGc);
    const auto parent_object = op.read(layout.foldSealKey(1, 1), Retry::once());
    ASSERT_TRUE(parent_object);
    const CasFoldSeal parent = decodeFoldSeal(parent_object->bytes);

    /// Admission is lost after the erase has been resolved: the second catalog read of the drain is
    /// the resolution cut, so the row is already gone when the loop's next verdict finds no admission.
    size_t catalog_reads = 0;
    bool authority_held = true;
    CasOperation fenced_op = requests.admit([&] { return authority_held; });
    backend->afterReadOf(layout.refCatalogKey(), [&]
    {
        if (++catalog_reads == 2)
            authority_held = false;
    });

    CatalogLifecycleReconciler reconciler(fenced_op, layout, parent, noCreatorProvedTerminal);
    const CatalogLifecycleReconcileResult result = reconciler.reconcile(noAuthorityRefresh, noRetirementRecord);

    EXPECT_EQ(result.authority_status, AuthorityStatus::FencedOut);
    EXPECT_EQ(result.catalog_resolution, CatalogResolution::ExactRowAbsent);
    ASSERT_EQ(reconciler.retiredLives().size(), 1);
    EXPECT_EQ(reconciler.retiredLives().front(),
        NamespaceLifeId::fromCatalogEntry(fixture.ns, fixture.life_id));
    EXPECT_EQ(reconciler.completedErases(), 1);
    EXPECT_FALSE(result.final_catalog_cut);
}

/// The store acknowledged the erase and the resolution read that follows throws: the erase is
/// counted and its life recorded anyway, because the drain's caller reads both after the throw.
TEST(CASCatalogLifecycleReconciler, RecordsAnAcknowledgedEraseWhoseResolutionReadThrows)
{
    auto backend = std::make_shared<DrainRaceBackend>();
    auto store = openPoolForTest(backend, /*gc_fold_max_defer_rounds=*/0);
    CasRequests requests = openRequestsForTest(backend);
    CasOperation op = requests.admit();
    const Layout & layout = store->layout();
    const CompletedRemovingFixture fixture = seedCompletedRemoving(op, store, kGc);
    const auto parent_object = op.read(layout.foldSealKey(1, 1), Retry::once());
    ASSERT_TRUE(parent_object);
    const CasFoldSeal parent = decodeFoldSeal(parent_object->bytes);

    /// The first catalog read selects the row; the second is the erase's resolution read.
    size_t catalog_reads = 0;
    backend->afterReadOf(layout.refCatalogKey(), [&]
    {
        if (++catalog_reads == 2)
            throw std::runtime_error("injected resolution read failure");
    });

    CatalogLifecycleReconciler reconciler(op, layout, parent, noCreatorProvedTerminal);
    EXPECT_THROW(reconciler.reconcile(noAuthorityRefresh, noRetirementRecord), std::runtime_error);
    backend->afterReadOf(layout.refCatalogKey(), {});

    ASSERT_FALSE(CasRefCatalog::lifeIfCataloged(op, layout, fixture.ns)) << "the erase landed";
    EXPECT_EQ(reconciler.completedErases(), 1u);
    EXPECT_EQ(reconciler.retiredLives(),
        std::vector<NamespaceLifeId>{NamespaceLifeId::fromCatalogEntry(fixture.ns, fixture.life_id)});
}

TEST(CASCatalogLifecycleReconciler, InitialFenceLossReportsEligibleRowStillPresent)
{
    auto backend = std::make_shared<DrainRaceBackend>();
    auto store = openPoolForTest(backend, /*gc_fold_max_defer_rounds=*/0);
    CasRequests requests = openRequestsForTest(backend);
    CasOperation op = requests.admit();
    const Layout & layout = store->layout();
    const CompletedRemovingFixture fixture = seedCompletedRemoving(op, store, kGc);
    const auto parent_object = op.read(layout.foldSealKey(1, 1), Retry::once());
    ASSERT_TRUE(parent_object);
    const CasFoldSeal parent = decodeFoldSeal(parent_object->bytes);
    backend->resetCounts();

    /// Admission is lost the moment the selection cut has been read, so the erase is never sent.
    bool authority_held = true;
    CasOperation fenced_op = requests.admit([&] { return authority_held; });
    backend->afterReadOf(layout.refCatalogKey(), [&] { authority_held = false; });

    CatalogLifecycleReconciler reconciler(fenced_op, layout, parent, noCreatorProvedTerminal);
    const CatalogLifecycleReconcileResult result = reconciler.reconcile(noAuthorityRefresh, noRetirementRecord);

    EXPECT_EQ(result.authority_status, AuthorityStatus::FencedOut);
    EXPECT_EQ(result.catalog_resolution, CatalogResolution::ExactRowStillPresent);
    EXPECT_TRUE(reconciler.retiredLives().empty());
    EXPECT_EQ(reconciler.completedErases(), 0);
    EXPECT_FALSE(result.final_catalog_cut);
    EXPECT_EQ(backend->getCount(layout.refCatalogKey()), 1)
        << "an operation whose admission is gone before the erase reports the selection cut it "
           "already holds and reads nothing further";
    EXPECT_EQ(backend->putOverwriteCount(layout.refCatalogKey()), 0);
    EXPECT_EQ(CasRefCatalog::lifeIfCataloged(op, layout, fixture.ns),
        NamespaceLifeId::fromCatalogEntry(fixture.ns, fixture.life_id));
}

TEST(CASCatalogLifecycleReconciler, RetriesFromTheMandatoryConflictResolutionCut)
{
    auto backend = std::make_shared<DrainRaceBackend>();
    auto store = openPoolForTest(backend, /*gc_fold_max_defer_rounds=*/0);
    CasRequests requests = openRequestsForTest(backend);
    CasOperation op = requests.admit();
    const Layout & layout = store->layout();
    seedCompletedRemoving(op, store, kGc);
    const auto parent_object = op.read(layout.foldSealKey(1, 1), Retry::once());
    ASSERT_TRUE(parent_object);
    const CasFoldSeal parent = decodeFoldSeal(parent_object->bytes);
    backend->clearJournal();
    backend->resetCounts();
    backend->conflictNextCatalogCas(layout.refCatalogKey());

    CatalogLifecycleReconciler reconciler(op, layout, parent, noCreatorProvedTerminal);
    size_t announcements = 0;
    const CatalogLifecycleReconcileResult result = reconciler.reconcile(
        noAuthorityRefresh, [&](const NamespaceLifeId &) { ++announcements; });

    EXPECT_EQ(result.authority_status, AuthorityStatus::Authoritative);
    EXPECT_EQ(result.catalog_resolution, CatalogResolution::DrainComplete);
    EXPECT_EQ(reconciler.completedErases(), 1);
    EXPECT_EQ(announcements, 1) << "one announcement per life, however many erase attempts it takes";
    const std::vector<String> journal = backend->journalSnapshot();
    const String catalog_get = "get " + layout.refCatalogKey();
    EXPECT_EQ(std::count(journal.begin(), journal.end(), catalog_get), 4)
        << "selection, the refused write's own resolve read, the mandatory resolution cut the retry "
           "reuses, and the committed erase's resolution -- the catalog takes no cut of its own";
}

/// A failing announcement aborts the drain before any erase is sent: no record means no erase.
TEST(CASCatalogLifecycleReconciler, AFailingAnnouncementSendsNoErase)
{
    auto backend = std::make_shared<DrainRaceBackend>();
    auto store = openPoolForTest(backend, /*gc_fold_max_defer_rounds=*/0);
    CasRequests requests = openRequestsForTest(backend);
    CasOperation op = requests.admit();
    const Layout & layout = store->layout();
    seedCompletedRemoving(op, store, kGc);
    const auto parent_object = op.read(layout.foldSealKey(1, 1), Retry::once());
    ASSERT_TRUE(parent_object);
    const CasFoldSeal parent = decodeFoldSeal(parent_object->bytes);
    backend->resetCounts();

    CatalogLifecycleReconciler reconciler(op, layout, parent, noCreatorProvedTerminal);
    EXPECT_THROW(
        (void)reconciler.reconcile(noAuthorityRefresh, [](const NamespaceLifeId &) { throw std::runtime_error("record failed"); }),
        std::runtime_error);
    EXPECT_EQ(backend->putOverwriteCount(layout.refCatalogKey()), 0) << "the erase must not be sent";
    EXPECT_FALSE(CasRefCatalog::read(op, layout).catalog.entries.empty()) << "the row must still be cataloged";
}

/// The leader is deposed after its first cancel. Its cached authority still reads true when it selects
/// the second row, so only the refresh at the top of the second cancel's attempt can stop that erase.
TEST(CASCatalogLifecycleReconciler, ALeaderDeposedBetweenTwoCancelsSendsNoSecond)
{
    auto backend = std::make_shared<DrainRaceBackend>();
    auto store = openPoolForTest(backend, /*gc_fold_max_defer_rounds=*/0);
    CasRequests requests = openRequestsForTest(backend);
    CasOperation op = requests.admit();
    const Layout & layout = store->layout();
    const CatalogEntry first = seedStalledCreating(op, store, "00/first@cas@",
        hexToU128("000000000000000000000000000000a1"), CreatorFence{.server_root_id = "gone", .writer_epoch = 1, .fence_generation = 1});
    const CatalogEntry second = seedStalledCreating(op, store, "00/second@cas@",
        hexToU128("000000000000000000000000000000a2"), CreatorFence{.server_root_id = "gone", .writer_epoch = 1, .fence_generation = 1});
    backend->resetCounts();

    bool authority_held = true;
    size_t refreshes = 0;
    CasOperation leader_op = requests.admit([&] { return authority_held; });
    const CasFoldSeal parent;
    CatalogLifecycleReconciler reconciler(leader_op, layout, parent, everyCreatorTerminal);
    const CatalogLifecycleReconcileResult result = reconciler.reconcile(
        [&] { authority_held = ++refreshes < 2; }, noRetirementRecord);

    EXPECT_EQ(result.authority_status, AuthorityStatus::FencedOut);
    EXPECT_EQ(backend->putOverwriteCount(layout.refCatalogKey()), 1u) << "no cancel is sent after the deposition";
    EXPECT_FALSE(catalogRow(op, layout, first.ns));
    EXPECT_EQ(catalogRow(op, layout, second.ns), std::optional<CatalogEntry>{second});
    EXPECT_EQ(reconciler.cancelledCreating(), 1u);
    EXPECT_EQ(reconciler.retiredLives(),
        std::vector<NamespaceLifeId>{NamespaceLifeId::fromCatalogEntry(first.ns, first.incarnation)});
}

/// The cancel lands and its response is lost. The write loop settles it from the store, so the cancel is
/// counted once and never resent.
TEST(CASCatalogLifecycleReconciler, ALostCancelResponseIsSettledFromTheStore)
{
    auto backend = std::make_shared<DrainRaceBackend>();
    auto store = openPoolForTest(backend, /*gc_fold_max_defer_rounds=*/0);
    CasRequests requests = openRequestsForTest(backend);
    CasOperation op = requests.admit();
    const Layout & layout = store->layout();
    const CatalogEntry stalled = seedStalledCreating(op, store, "00/lost@cas@",
        hexToU128("000000000000000000000000000000b1"), CreatorFence{.server_root_id = "gone", .writer_epoch = 1, .fence_generation = 1});
    backend->clearJournal();
    backend->loseNextCatalogCasResponse(layout.refCatalogKey());

    const CasFoldSeal parent;
    CatalogLifecycleReconciler reconciler(op, layout, parent, everyCreatorTerminal);
    const CatalogLifecycleReconcileResult result = reconciler.reconcile(noAuthorityRefresh, noRetirementRecord);

    EXPECT_EQ(result.authority_status, AuthorityStatus::Authoritative);
    EXPECT_EQ(result.catalog_resolution, CatalogResolution::DrainComplete);
    ASSERT_TRUE(result.final_catalog_cut);
    EXPECT_TRUE(result.final_catalog_cut->catalog.entries.empty());
    EXPECT_EQ(reconciler.cancelledCreating(), 1u);
    EXPECT_EQ(reconciler.retiredLives(),
        std::vector<NamespaceLifeId>{NamespaceLifeId::fromCatalogEntry(stalled.ns, stalled.incarnation)});
    const std::vector<String> journal = backend->journalSnapshot();
    EXPECT_EQ(std::count(journal.begin(), journal.end(), "cas_response_lost " + layout.refCatalogKey()), 1)
        << "positive control: the response was lost";
    EXPECT_EQ(std::count(journal.begin(), journal.end(), "cas_begin " + layout.refCatalogKey()), 1)
        << "a landed cancel is not resent";
}

/// An opener takes the row over after the drain selected it. The cancel loses the exact swap; the drain
/// neither throws nor retires the life, and leaves the row to its new creator.
TEST(CASCatalogLifecycleReconciler, ARowTakenOverDuringTheCancelIsLeftAndTheDrainCompletes)
{
    auto backend = std::make_shared<DrainRaceBackend>();
    auto store = openPoolForTest(backend, /*gc_fold_max_defer_rounds=*/0);
    CasRequests requests = openRequestsForTest(backend);
    CasOperation op = requests.admit();
    const Layout & layout = store->layout();
    const CatalogEntry stalled = seedStalledCreating(op, store, "00/taken@cas@",
        hexToU128("000000000000000000000000000000c1"), CreatorFence{.server_root_id = "gone", .writer_epoch = 1, .fence_generation = 1});
    const CreatorFence opener{.server_root_id = "opener", .writer_epoch = 5, .fence_generation = 1};
    backend->afterReadOf(layout.refCatalogKey(), [&]
    {
        backend->afterReadOf(layout.refCatalogKey(), {});
        ASSERT_EQ(CasRefCatalog::reconcileStaleCreator(op, layout, stalled, opener, [](const CreatorFence &) { return true; }),
            CasRefCatalog::ReconcileCreatorOutcome::Reconciled);
    });

    const CasFoldSeal parent;
    size_t probes = 0;
    CatalogLifecycleReconciler reconciler(op, layout, parent,
        [&](const CreatorFence & creator)
        {
            ++probes;
            return creator.server_root_id == "gone" ? CreatorSlotStatus::Terminal : CreatorSlotStatus::Live;
        });
    CatalogLifecycleReconcileResult result;
    ASSERT_NO_THROW(result = reconciler.reconcile(noAuthorityRefresh, noRetirementRecord));

    EXPECT_GE(probes, 1u) << "positive control: the drain selected the row before the takeover";
    EXPECT_EQ(result.authority_status, AuthorityStatus::Authoritative);
    EXPECT_EQ(result.catalog_resolution, CatalogResolution::DrainComplete);
    EXPECT_EQ(reconciler.cancelledCreating(), 0u);
    EXPECT_TRUE(reconciler.retiredLives().empty());
    const std::optional<CatalogEntry> row = catalogRow(op, layout, stalled.ns);
    ASSERT_TRUE(row && row->creator);
    EXPECT_EQ(*row->creator, opener);
}

/// The first cancel attempt loses its swap, and the leader is deposed before the second. Only a refresh
/// taken before EACH attempt sees that, so the second attempt sends nothing.
TEST(CASCatalogLifecycleReconciler, TheAuthorityIsRefreshedBeforeEveryCancelAttempt)
{
    auto backend = std::make_shared<DrainRaceBackend>();
    auto store = openPoolForTest(backend, /*gc_fold_max_defer_rounds=*/0);
    CasRequests requests = openRequestsForTest(backend);
    CasOperation op = requests.admit();
    const Layout & layout = store->layout();
    const CatalogEntry stalled = seedStalledCreating(op, store, "00/refreshed@cas@",
        hexToU128("000000000000000000000000000000d1"), CreatorFence{.server_root_id = "gone", .writer_epoch = 1, .fence_generation = 1});
    backend->clearJournal();
    backend->conflictNextCatalogCas(layout.refCatalogKey());

    bool authority_held = true;
    size_t refreshes = 0;
    CasOperation leader_op = requests.admit([&] { return authority_held; });
    const CasFoldSeal parent;
    CatalogLifecycleReconciler reconciler(leader_op, layout, parent, everyCreatorTerminal);
    const CatalogLifecycleReconcileResult result = reconciler.reconcile(
        [&] { authority_held = ++refreshes < 2; }, noRetirementRecord);

    const std::vector<String> journal = backend->journalSnapshot();
    EXPECT_EQ(std::count(journal.begin(), journal.end(), "cas_forced_conflict " + layout.refCatalogKey()), 1)
        << "positive control: the first attempt was sent and lost its swap";
    EXPECT_EQ(std::count(journal.begin(), journal.end(), "cas_begin " + layout.refCatalogKey()), 1)
        << "no second attempt after the deposition";
    EXPECT_EQ(result.authority_status, AuthorityStatus::FencedOut);
    EXPECT_EQ(reconciler.cancelledCreating(), 0u);
    EXPECT_EQ(catalogRow(op, layout, stalled.ns), std::optional<CatalogEntry>{stalled});
}

/// Polls until `key`'s lane holds `depth` items, the holder included. False when `stop` turns true or a
/// minute passes first, so a writer that never queues fails the test instead of hanging it.
namespace
{
bool waitForLaneDepth(const CasOperation & op, const String & key, size_t depth, const std::atomic<bool> & stop)
{
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::minutes(1);
    while (op.hotKeys().queueDepthForTest(key) < depth)
    {
        if (stop || std::chrono::steady_clock::now() >= deadline)
            return false;
        std::this_thread::yield();
    }
    return true;
}
}

/// The leader queues for the catalog lane behind another writer of this server and is deposed while it
/// waits. Its authority is read again once it holds the lane, so it sends no cancel.
TEST(CASCatalogLifecycleReconciler, ALeaderDeposedWhileQueuedForTheCatalogLaneSendsNoCancel)
{
    auto backend = std::make_shared<DrainRaceBackend>();
    auto store = openPoolForTest(backend, /*gc_fold_max_defer_rounds=*/0);
    CasRequests requests = openRequestsForTest(backend);
    CasOperation op = requests.admit();
    const Layout & layout = store->layout();
    const String catalog_key = layout.refCatalogKey();
    const CatalogEntry stalled = seedStalledCreating(op, store, "00/queued@cas@",
        hexToU128("000000000000000000000000000000d4"), CreatorFence{.server_root_id = "gone", .writer_epoch = 1, .fence_generation = 1});
    backend->clearJournal();

    std::latch holding(1);
    std::latch release(1);
    std::thread other_writer([&]
    {
        CasOperation other_op = requests.admit();
        (void)other_op.hotKeys().submit(catalog_key, other_op, other_op.freeze(Retry::standard()),
            [&](const std::optional<Object> &) -> std::optional<String>
            {
                holding.count_down();
                release.wait();
                return std::nullopt;
            });
    });
    holding.wait();

    /// `lease_moved` stands for the store's `gc/state`; only a refresh reads it into `authority_held`.
    std::atomic<bool> lease_moved = false;
    std::atomic<bool> authority_held = true;
    CasOperation leader_op = requests.admit([&] { return authority_held.load(); });
    const CasFoldSeal parent;
    CatalogLifecycleReconciler reconciler(leader_op, layout, parent, everyCreatorTerminal);
    CatalogLifecycleReconcileResult result;
    std::atomic<bool> leader_returned = false;
    std::thread leader([&]
    {
        result = reconciler.reconcile([&] { authority_held = !lease_moved; }, noRetirementRecord);
        leader_returned = true;
    });
    /// The leader has selected the row and waits behind the holder.
    const bool leader_queued = waitForLaneDepth(op, catalog_key, 2, leader_returned);
    lease_moved = true;
    release.count_down();
    leader.join();
    other_writer.join();
    ASSERT_TRUE(leader_queued) << "the leader returned, or did not queue for the catalog lane within a minute";

    const std::vector<String> journal = backend->journalSnapshot();
    EXPECT_EQ(std::count(journal.begin(), journal.end(), "cas_begin " + catalog_key), 0) << "no cancel is sent";
    EXPECT_EQ(result.authority_status, AuthorityStatus::FencedOut);
    EXPECT_EQ(reconciler.cancelledCreating(), 0u);
    EXPECT_EQ(catalogRow(op, layout, stalled.ns), std::optional<CatalogEntry>{stalled});
}

/// The leader reaches the catalog lane with a quarter of its cancel's window left, and every read of
/// `gc/state` fails retryably. The authority check ends by the cancel's deadline: no cancel is sent, the
/// row stays, and the writer queued behind gets the lane by then.
TEST(CASCatalogLifecycleReconciler, AnAuthorityReadFailingInTheCatalogLaneEndsByTheCancelDeadline)
{
    auto backend = std::make_shared<DrainRaceBackend>();
    auto store = openPoolForTest(backend, /*gc_fold_max_defer_rounds=*/0);
    CasRequests requests = openRequestsForTest(backend);
    FakeClock clock;
    requests.setNowFnForTest(clock.nowFn());
    requests.setSleepFnForTest(clock.sleepFn());
    CasOperation op = requests.admit();
    const Layout & layout = store->layout();
    const String catalog_key = layout.refCatalogKey();
    const String gc_state_key = layout.gcStateKey();
    const CatalogEntry stalled = seedStalledCreating(op, store, "00/slow-authority@cas@",
        hexToU128("000000000000000000000000000000d5"), CreatorFence{.server_root_id = "gone", .writer_epoch = 1, .fence_generation = 1});
    backend->clearJournal();
    backend->afterReadOf(gc_state_key, [] { throw Poco::TimeoutException("injected timeout of the gc/state read"); });

    std::latch holding(1);
    std::latch release(1);
    std::thread other_writer([&]
    {
        CasOperation other_op = requests.admit();
        (void)other_op.hotKeys().submit(catalog_key, other_op, other_op.freeze(Retry::standard()),
            [&](const std::optional<Object> &) -> std::optional<String>
            {
                holding.count_down();
                release.wait();
                return std::nullopt;
            });
    });
    holding.wait();

    /// Fail-closed like the GC leader's own refresh: authority is held only after a completed read.
    std::atomic<bool> authority_held = true;
    const auto refresh = [&](const Retry & bound)
    {
        authority_held = false;
        try
        {
            CasOperation read_op = requests.admit();
            (void)read_op.read(gc_state_key, bound);
            authority_held = true;
        }
        catch (...)
        {
            authority_held = false;
        }
    };
    CasOperation leader_op = requests.admit([&] { return authority_held.load(); });
    /// The cancel freezes `Retry::standard` before it queues; no fake time passes before that.
    const uint64_t cancel_deadline = clock.now.load() + DB::Cas::kDefaultRequestWindowMs;
    const CasFoldSeal parent;
    CatalogLifecycleReconciler reconciler(leader_op, layout, parent, everyCreatorTerminal);
    CatalogLifecycleReconcileResult result;
    std::atomic<bool> leader_returned = false;
    std::thread leader([&]
    {
        result = reconciler.reconcile(refresh, noRetirementRecord);
        leader_returned = true;
    });
    const bool leader_queued = waitForLaneDepth(op, catalog_key, 2, leader_returned);

    std::atomic<uint64_t> lane_reached_at = 0;
    std::atomic<bool> queued_writer_returned = false;
    std::optional<std::thread> queued_writer;
    bool writer_queued = false;
    if (leader_queued)
    {
        /// Three quarters of the cancel's window pass while it waits for the lane.
        clock.now.fetch_add(DB::Cas::kDefaultRequestWindowMs * 3 / 4);
        queued_writer.emplace([&]
        {
            CasOperation queued_op = requests.admit();
            (void)queued_op.hotKeys().submit(catalog_key, queued_op,
                queued_op.freeze(Retry::within(10 * DB::Cas::kDefaultRequestWindowMs)),
                [&](const std::optional<Object> &) -> std::optional<String>
                {
                    lane_reached_at = clock.now.load();
                    return std::nullopt;
                });
            queued_writer_returned = true;
        });
        writer_queued = waitForLaneDepth(op, catalog_key, 3, queued_writer_returned);
    }
    release.count_down();
    leader.join();
    if (queued_writer)
        queued_writer->join();
    other_writer.join();
    ASSERT_TRUE(leader_queued) << "the leader returned, or did not queue for the catalog lane within a minute";
    ASSERT_TRUE(writer_queued) << "the second writer did not queue behind the leader within a minute";

    const std::vector<String> journal = backend->journalSnapshot();
    EXPECT_GE(std::count(journal.begin(), journal.end(), "get " + gc_state_key), 2)
        << "positive control: the authority check ran in the lane and retried its read";
    EXPECT_EQ(std::count(journal.begin(), journal.end(), "cas_begin " + catalog_key), 0) << "no cancel is sent";
    EXPECT_EQ(result.authority_status, AuthorityStatus::FencedOut);
    EXPECT_EQ(reconciler.cancelledCreating(), 0u);
    EXPECT_EQ(catalogRow(op, layout, stalled.ns), std::optional<CatalogEntry>{stalled});
    ASSERT_NE(lane_reached_at.load(), 0u) << "the writer queued behind the leader got the lane";
    EXPECT_LE(lane_reached_at.load(), cancel_deadline)
        << "the lane was held " << lane_reached_at.load() - cancel_deadline << " ms past the cancel's deadline";
}

/// One creator root has rows at two writer epochs, and its slot is live at the later one. The earlier
/// epoch's row is cancelled and the later epoch's row stays: each epoch is judged on its own.
TEST(CASCatalogLifecycleReconciler, OneCreatorAtTwoEpochsHasEachEpochJudgedOnItsOwn)
{
    auto backend = std::make_shared<DrainRaceBackend>();
    auto store = openPoolForTest(backend, /*gc_fold_max_defer_rounds=*/0);
    CasRequests requests = openRequestsForTest(backend);
    CasOperation op = requests.admit();
    const Layout & layout = store->layout();
    const CatalogEntry superseded = seedStalledCreating(op, store, "00/a-epoch-1@cas@",
        hexToU128("000000000000000000000000000000f1"), CreatorFence{.server_root_id = "restarted", .writer_epoch = 1, .fence_generation = 1});
    const CatalogEntry current = seedStalledCreating(op, store, "00/b-epoch-2@cas@",
        hexToU128("000000000000000000000000000000f2"), CreatorFence{.server_root_id = "restarted", .writer_epoch = 2, .fence_generation = 1});
    setWatermarkMinActive(*backend, layout, "restarted", /*writer_epoch*/ 2, /*min_active_build_sequence*/ 1);
    ASSERT_EQ(probeCreatorSlot(op, layout, "restarted", 2), CreatorSlotStatus::Live);
    ASSERT_EQ(probeCreatorSlot(op, layout, "restarted", 1), CreatorSlotStatus::Terminal);

    const CasFoldSeal parent;
    CatalogLifecycleReconciler reconciler(op, layout, parent,
        [&](const CreatorFence & fence) { return probeCreatorSlot(op, layout, fence.server_root_id, fence.writer_epoch); });
    const CatalogLifecycleReconcileResult result = reconciler.reconcile(noAuthorityRefresh, noRetirementRecord);

    EXPECT_EQ(result.catalog_resolution, CatalogResolution::DrainComplete);
    EXPECT_EQ(reconciler.cancelledCreating(), 1u);
    EXPECT_FALSE(catalogRow(op, layout, superseded.ns));
    EXPECT_EQ(catalogRow(op, layout, current.ns), std::optional<CatalogEntry>{current});
}

/// `Creating` rows on both sides of `Removing` rows in one catalog: one drain settles all of them, and
/// only the `Removing` lives are announced before their erase.
TEST(CASCatalogLifecycleReconciler, CreatingRowsBesideRemovingRowsDrainTogetherAndOnlyRemovingIsAnnounced)
{
    auto backend = std::make_shared<DrainRaceBackend>();
    auto store = openPoolForTest(backend, /*gc_fold_max_defer_rounds=*/0);
    CasRequests requests = openRequestsForTest(backend);
    CasOperation op = requests.admit();
    const Layout & layout = store->layout();
    constexpr size_t removing = 2;
    seedCompletedRemovingBatch(op, store, kGc, removing);
    const CatalogEntry before = seedStalledCreating(op, store, "00/a-creating@cas@",
        hexToU128("000000000000000000000000000000d2"), CreatorFence{.server_root_id = "gone", .writer_epoch = 1, .fence_generation = 1});
    const CatalogEntry after = seedStalledCreating(op, store, "00/z-creating@cas@",
        hexToU128("000000000000000000000000000000d3"), CreatorFence{.server_root_id = "gone", .writer_epoch = 1, .fence_generation = 1});
    const auto parent_object = op.read(layout.foldSealKey(1, 1), Retry::once());
    ASSERT_TRUE(parent_object);
    const CasFoldSeal parent = decodeFoldSeal(parent_object->bytes);

    std::vector<NamespaceLifeId> announced;
    CatalogLifecycleReconciler reconciler(op, layout, parent, everyCreatorTerminal);
    const CatalogLifecycleReconcileResult result = reconciler.reconcile(
        noAuthorityRefresh, [&](const NamespaceLifeId & life) { announced.push_back(life); });

    EXPECT_EQ(result.catalog_resolution, CatalogResolution::DrainComplete);
    ASSERT_TRUE(result.final_catalog_cut);
    EXPECT_TRUE(result.final_catalog_cut->catalog.entries.empty());
    EXPECT_EQ(reconciler.completedErases(), removing);
    EXPECT_EQ(reconciler.cancelledCreating(), 2u);
    const NamespaceLifeId before_life = NamespaceLifeId::fromCatalogEntry(before.ns, before.incarnation);
    const NamespaceLifeId after_life = NamespaceLifeId::fromCatalogEntry(after.ns, after.incarnation);
    EXPECT_EQ(announced.size(), removing);
    EXPECT_EQ(std::count(announced.begin(), announced.end(), before_life), 0);
    EXPECT_EQ(std::count(announced.begin(), announced.end(), after_life), 0);
    const std::vector<NamespaceLifeId> & retired = reconciler.retiredLives();
    EXPECT_EQ(retired.size(), removing + 2);
    EXPECT_EQ(std::count(retired.begin(), retired.end(), before_life), 1);
    EXPECT_EQ(std::count(retired.begin(), retired.end(), after_life), 1);
}

/// Three rows of one creator cost one read of its mount slot.
TEST(CASCatalogLifecycleReconciler, RowsOfOneCreatorCostOneSlotRead)
{
    auto backend = std::make_shared<DrainRaceBackend>();
    auto store = openPoolForTest(backend, /*gc_fold_max_defer_rounds=*/0);
    CasRequests requests = openRequestsForTest(backend);
    CasOperation op = requests.admit();
    const Layout & layout = store->layout();
    const CreatorFence creator{.server_root_id = "moved-creator", .writer_epoch = 1, .fence_generation = 1};
    for (size_t i = 0; i < 3; ++i)
        seedStalledCreating(op, store, fmt::format("00/row-{}@cas@", i), UInt128{0xe0 + i}, creator);
    retireCreatorRootAt(*backend, layout, "moved-creator", /*epoch*/ 2);
    backend->clearJournal();

    const CasFoldSeal parent;
    CatalogLifecycleReconciler reconciler(op, layout, parent,
        [&](const CreatorFence & fence) { return probeCreatorSlot(op, layout, fence.server_root_id, fence.writer_epoch); });
    const CatalogLifecycleReconcileResult result = reconciler.reconcile(noAuthorityRefresh, noRetirementRecord);

    EXPECT_EQ(reconciler.cancelledCreating(), 3u) << "positive control: every row was probed terminal and cancelled";
    ASSERT_TRUE(result.final_catalog_cut);
    EXPECT_TRUE(result.final_catalog_cut->catalog.entries.empty());
    const std::vector<String> journal = backend->journalSnapshot();
    EXPECT_EQ(std::count(journal.begin(), journal.end(), "get " + layout.mountKey("moved-creator")), 1);
}

/// A creator whose slot read throws and one whose slot does not decode keep their rows for this drain,
/// each with one warning however many rows it has. The drain goes on: a certified creator's row and a
/// `Removing` row after them are settled.
TEST(CASCatalogLifecycleReconciler, AnUnreadableOrUndecodableSlotKeepsItsRowsAndTheDrainGoesOn)
{
    auto backend = std::make_shared<DrainRaceBackend>();
    auto store = openPoolForTest(backend, /*gc_fold_max_defer_rounds=*/0);
    CasRequests requests = openRequestsForTest(backend);
    CasOperation op = requests.admit();
    const Layout & layout = store->layout();
    seedCompletedRemovingBatch(op, store, kGc, 1);
    const CreatorFence unreadable{.server_root_id = "unreadable", .writer_epoch = 1, .fence_generation = 1};
    const CreatorFence garbled{.server_root_id = "garbled", .writer_epoch = 1, .fence_generation = 1};
    const CreatorFence gone{.server_root_id = "gone", .writer_epoch = 1, .fence_generation = 1};
    const std::vector<CatalogEntry> kept{
        seedStalledCreating(op, store, "00/a-unreadable@cas@", UInt128{0xf1}, unreadable),
        seedStalledCreating(op, store, "00/b-unreadable@cas@", UInt128{0xf2}, unreadable),
        seedStalledCreating(op, store, "00/c-garbled@cas@", UInt128{0xf3}, garbled),
        seedStalledCreating(op, store, "00/c-garbled-too@cas@", UInt128{0xf4}, garbled)};
    const CatalogEntry certified = seedStalledCreating(op, store, "00/c-gone@cas@", UInt128{0xf5}, gone);
    const auto parent_object = op.read(layout.foldSealKey(1, 1), Retry::once());
    ASSERT_TRUE(parent_object);
    const CasFoldSeal parent = decodeFoldSeal(parent_object->bytes);

    std::map<String, size_t> probes;
    ScopedCasGcLogCapture capture("CatalogLifecycleReconciler");
    CatalogLifecycleReconciler reconciler(op, layout, parent,
        [&](const CreatorFence & fence)
        {
            ++probes[fence.server_root_id];
            if (fence.server_root_id == "unreadable")
                throw DB::Exception(DB::ErrorCodes::NETWORK_ERROR, "injected mount slot read failure");
            return fence.server_root_id == "garbled" ? CreatorSlotStatus::Undecodable : CreatorSlotStatus::Terminal;
        });
    CatalogLifecycleReconcileResult result;
    ASSERT_NO_THROW(result = reconciler.reconcile(noAuthorityRefresh, noRetirementRecord));

    EXPECT_EQ(result.authority_status, AuthorityStatus::Authoritative);
    EXPECT_EQ(result.catalog_resolution, CatalogResolution::DrainComplete);
    EXPECT_EQ(reconciler.completedErases(), 1u) << "the `Removing` row";
    EXPECT_EQ(reconciler.cancelledCreating(), 1u) << "the certified row";
    EXPECT_FALSE(catalogRow(op, layout, certified.ns));
    EXPECT_FALSE(catalogRow(op, layout, RootNamespace{"00/drain-batch-0@cas@"}));
    for (const CatalogEntry & entry : kept)
        EXPECT_EQ(catalogRow(op, layout, entry.ns), std::optional<CatalogEntry>{entry}) << entry.ns.string();
    EXPECT_EQ(probes, (std::map<String, size_t>{{"unreadable", 1}, {"garbled", 1}, {"gone", 1}}));
    const String log = capture.captured();
    const auto lines_naming = [&](const String & root)
    {
        size_t lines = 0;
        for (size_t at = log.find("creator '" + root + "'"); at != String::npos; at = log.find("creator '" + root + "'", at + 1))
            ++lines;
        return lines;
    };
    EXPECT_EQ(lines_naming("unreadable"), 1u) << log;
    EXPECT_EQ(lines_naming("garbled"), 1u) << log;
    EXPECT_EQ(lines_naming("gone"), 0u) << log;
}

/// A leader that is deposed once its selection cut is read probes no creator.
TEST(CASCatalogLifecycleReconciler, ADeposedLeaderProbesNoCreator)
{
    auto backend = std::make_shared<DrainRaceBackend>();
    auto store = openPoolForTest(backend, /*gc_fold_max_defer_rounds=*/0);
    CasRequests requests = openRequestsForTest(backend);
    CasOperation op = requests.admit();
    const Layout & layout = store->layout();
    const CatalogEntry stalled = seedStalledCreating(op, store, "00/deposed@cas@",
        hexToU128("000000000000000000000000000000d4"), CreatorFence{.server_root_id = "gone", .writer_epoch = 1, .fence_generation = 1});

    bool authority_held = true;
    CasOperation leader_op = requests.admit([&] { return authority_held; });
    backend->afterReadOf(layout.refCatalogKey(), [&] { authority_held = false; });
    size_t probes = 0;
    const CasFoldSeal parent;
    CatalogLifecycleReconciler reconciler(leader_op, layout, parent,
        [&](const CreatorFence &)
        {
            ++probes;
            return CreatorSlotStatus::Terminal;
        });
    const CatalogLifecycleReconcileResult result = reconciler.reconcile(noAuthorityRefresh, noRetirementRecord);
    backend->afterReadOf(layout.refCatalogKey(), {});

    EXPECT_EQ(result.authority_status, AuthorityStatus::FencedOut);
    EXPECT_EQ(probes, 0u);
    EXPECT_EQ(catalogRow(op, layout, stalled.ns), std::optional<CatalogEntry>{stalled});
}

/// Each cancel writes one INFO line naming the namespace and its creator, and counts its own event.
TEST(CASCatalogLifecycleReconciler, EachCancelLogsOneLineAndCountsItsOwnEvent)
{
    auto backend = std::make_shared<DrainRaceBackend>();
    auto store = openPoolForTest(backend, /*gc_fold_max_defer_rounds=*/0);
    CasRequests requests = openRequestsForTest(backend);
    CasOperation op = requests.admit();
    const Layout & layout = store->layout();
    seedStalledCreating(op, store, "00/logged-a@cas@", UInt128{0x121},
        CreatorFence{.server_root_id = "gone", .writer_epoch = 1, .fence_generation = 1});
    seedStalledCreating(op, store, "00/logged-b@cas@", UInt128{0x122},
        CreatorFence{.server_root_id = "gone", .writer_epoch = 1, .fence_generation = 1});
    const uint64_t events_before = ProfileEvents::global_counters[ProfileEvents::CASGCStalledCreatingCancelled];

    String log;
    {
        ScopedCasGcLogCapture capture("CatalogLifecycleReconciler", "information");
        const CasFoldSeal parent;
        CatalogLifecycleReconciler reconciler(op, layout, parent, everyCreatorTerminal);
        reconciler.reconcile(noAuthorityRefresh, noRetirementRecord);
        EXPECT_EQ(reconciler.cancelledCreating(), 2u);
        EXPECT_EQ(reconciler.completedErases(), 0u);
        log = capture.captured();
    }

    EXPECT_EQ(ProfileEvents::global_counters[ProfileEvents::CASGCStalledCreatingCancelled] - events_before, 2u);
    EXPECT_NE(log.find("'00/logged-a@cas@'"), String::npos) << log;
    EXPECT_NE(log.find("'00/logged-b@cas@'"), String::npos) << log;
    EXPECT_NE(log.find("creator 'gone' at writer epoch 1"), String::npos) << log;
}

/// Two leaders cancel the same row: the first one's swap is held in flight while the second drains. The
/// row goes once, only the winner counts it, and the loser writes nothing more.
TEST(CASCatalogLifecycleReconciler, TwoLeadersRacingOneCancelRemoveTheRowOnce)
{
    auto backend = std::make_shared<DrainRaceBackend>();
    auto store = openPoolForTest(backend, /*gc_fold_max_defer_rounds=*/0);
    CasRequests requests = openRequestsForTest(backend);
    CasOperation op = requests.admit();
    const Layout & layout = store->layout();
    const CatalogEntry stalled = seedStalledCreating(op, store, "00/raced@cas@",
        hexToU128("000000000000000000000000000000d5"), CreatorFence{.server_root_id = "gone", .writer_epoch = 1, .fence_generation = 1});
    backend->clearJournal();
    backend->blockNextCatalogCas(layout.refCatalogKey());

    const CasFoldSeal parent;
    CasRequests first_requests = openRequestsForTest(backend);
    CasOperation first_op = first_requests.admit();
    CatalogLifecycleReconciler first(first_op, layout, parent, everyCreatorTerminal);
    CatalogLifecycleReconcileResult first_result;
    std::thread first_leader([&] { first_result = first.reconcile(noAuthorityRefresh, noRetirementRecord); });
    backend->waitForBlockedCatalogCas();

    CasRequests second_requests = openRequestsForTest(backend);
    CasOperation second_op = second_requests.admit();
    CatalogLifecycleReconciler second(second_op, layout, parent, everyCreatorTerminal);
    const CatalogLifecycleReconcileResult second_result = second.reconcile(noAuthorityRefresh, noRetirementRecord);
    backend->releaseBlockedCatalogCas();
    first_leader.join();

    EXPECT_EQ(second.cancelledCreating(), 1u);
    EXPECT_EQ(first.cancelledCreating(), 0u);
    EXPECT_TRUE(first.retiredLives().empty());
    EXPECT_EQ(first_result.catalog_resolution, CatalogResolution::DrainComplete);
    EXPECT_EQ(second_result.catalog_resolution, CatalogResolution::DrainComplete);
    EXPECT_FALSE(catalogRow(op, layout, stalled.ns));
    const std::vector<String> journal = backend->journalSnapshot();
    EXPECT_EQ(std::count(journal.begin(), journal.end(), "cas_begin " + layout.refCatalogKey()), 2)
        << "one swap per leader: the loser does not write again";
}

/// THE DRAIN'S AUTHORITY, end to end. `CatalogLifecycleReconciler` and
/// `deleteCompletedRemovingAtSnapshot` decide `FencedOut` from `CasOperation::admitted()`, and the GC
/// plane's fence is open -- so the only thing that can make that verdict false in production is the
/// `Liveness` the round hands its drain operation. Depose the leader in the window the round leaves
/// between acquiring its lease and the pre-fold drain, and the drain must erase nothing. Without the
/// predicate the verdict is a constant TRUE, the drain completes, and the completed-removal row is
/// gone -- which is what this test catches.
TEST(CASGCFrontierGate, ADeposedLeaderErasesNoCatalogRow)
{
    auto backend = std::make_shared<DrainRaceBackend>();
    auto store = openPoolForTest(backend, /*gc_fold_max_defer_rounds=*/0);
    CasRequests requests = openRequestsForTest(backend);
    CasOperation op = requests.admit();
    const Layout & layout = store->layout();
    const CompletedRemovingFixture fixture = seedCompletedRemoving(op, store, kGc);
    const uint64_t catalog_writes_before = backend->putOverwriteCount(layout.refCatalogKey());

    /// Another leader steals `gc/state` after this round's lease renewal and before its drain.
    const auto depose = [&]
    {
        const auto got = op.read(layout.gcStateKey(), Retry::once());
        ASSERT_TRUE(got);
        GcState stolen = decodeGcState(got->bytes);
        stolen.lease.owner = hexToU128("00000000000000000000000000000099");
        ++stolen.lease.seq;
        ASSERT_TRUE(std::holds_alternative<Committed>(
            op.replace(layout.gcStateKey(), encodeGcState(stolen), got->etag, Retry::once())));
    };

    Gc gc(store, kGc);
    EXPECT_THROW(gc.runRegularRound(depose), DB::Exception)
        << "a deposed leader must give up rather than drain the catalog";
    EXPECT_TRUE(CasRefCatalog::lifeIfCataloged(op, layout, fixture.ns))
        << "the completed-removal row survives a deposed leader's drain";
    EXPECT_EQ(backend->putOverwriteCount(layout.refCatalogKey()), catalog_writes_before)
        << "and no catalog write was even attempted";
}

/// THE SAME AUTHORITY, now DURING the drain. A drain erases one row per iteration, and what stops a
/// leader deposed between two erases is the refresh the ERASE runs at the top of every attempt (the
/// reconciler only forwards it): the first row goes, the second is never attempted, and the drain
/// reports `FencedOut` from the cut it already holds. One reading taken before the drain would
/// authorise both erases: the erase count and the surviving-row count below are what catch that,
/// since an unrefreshed drain sends a second erase and empties the catalog under a lease this leader
/// no longer owns.
TEST(CASGCFrontierGate, ALeaderDeposedBetweenTwoErasesStopsAfterTheFirst)
{
    auto backend = std::make_shared<DrainRaceBackend>();
    auto store = openPoolForTest(backend, /*gc_fold_max_defer_rounds=*/0);
    CasRequests requests = openRequestsForTest(backend);
    CasOperation op = requests.admit();
    const Layout & layout = store->layout();
    seedCompletedRemovingBatch(op, store, kGc, /*count=*/2);
    const std::vector<RootNamespace> seeded{
        RootNamespace{"00/drain-batch-0@cas@"}, RootNamespace{"00/drain-batch-1@cas@"}};
    const uint64_t catalog_writes_before = backend->putOverwriteCount(layout.refCatalogKey());

    /// The hook runs after every catalog read, and the first read it sees with an erase already behind
    /// it is the resolution read that closed erase one -- exactly the window between the two erases.
    /// Nothing before the drain reads or writes the catalog, so no earlier read can trip this.
    bool deposed = false;
    backend->afterReadOf(layout.refCatalogKey(), [&]
    {
        if (deposed || backend->putOverwriteCount(layout.refCatalogKey()) == catalog_writes_before)
            return;
        deposed = true;
        const auto got = op.read(layout.gcStateKey(), Retry::once());
        ASSERT_TRUE(got);
        GcState stolen = decodeGcState(got->bytes);
        stolen.lease.owner = hexToU128("00000000000000000000000000000099");
        ++stolen.lease.seq;
        EXPECT_TRUE(std::holds_alternative<Committed>(
            op.replace(layout.gcStateKey(), encodeGcState(stolen), got->etag, Retry::once())));
    });

    Gc gc(store, kGc);
    EXPECT_THROW(gc.runRegularRound(), DB::Exception)
        << "a leader deposed inside its own drain must not finish the round";
    EXPECT_TRUE(deposed) << "the round must have reached a catalog read after its first erase";
    EXPECT_EQ(backend->putOverwriteCount(layout.refCatalogKey()), catalog_writes_before + 1)
        << "one erase reached the store; the second was refused before it was sent";
    size_t still_cataloged = 0;
    for (const RootNamespace & ns : seeded)
        if (CasRefCatalog::lifeIfCataloged(op, layout, ns))
            ++still_cataloged;
    EXPECT_EQ(still_cataloged, 1u)
        << "one row was erased before the deposition; the other survives it";
}

/// A drain that throws after erasing a row still reports that row: the phase record is what an
/// operator reads to learn how far the round got.
TEST(CASGCFrontierGate, AnAbortedDrainReportsTheRowsItErased)
{
    auto backend = std::make_shared<DrainRaceBackend>();
    auto store = openPoolForTest(backend, /*gc_fold_max_defer_rounds=*/0);
    /// The injected failure is retried until the request window ends; a virtual clock ends it at once.
    const auto clock = DB::Cas::tests::VirtualRetryClock::installOn(store);
    CasRequests requests = openRequestsForTest(backend);
    CasOperation op = requests.admit();
    const Layout & layout = store->layout();
    seedCompletedRemovingBatch(op, store, kGc, /*count=*/2);
    backend->failCatalogCasesAfter(layout.refCatalogKey(), /*allowed=*/1);

    std::optional<std::map<String, UInt64>> drain_metrics;
    Gc gc(store, kGc);
    gc.setPhaseSink([&](const GcPhaseRecord & rec)
    {
        if (rec.phase == "pre_fold_ref_drain")
            drain_metrics = rec.metrics;
    });
    EXPECT_THROW(gc.runRegularRound(), DB::Exception);
    gc.setPhaseSink({});

    ASSERT_TRUE(drain_metrics.has_value()) << "the drain phase emits its record even when it throws";
    ASSERT_TRUE(drain_metrics->contains("deleted")) << "the aborted drain reports its erase count";
    EXPECT_EQ(drain_metrics->at("deleted"), 1u);
}

TEST(CASGCFrontierGate, HealthyRebuildUsesTheCatalogLifecycleReconciler)
{
    auto backend = std::make_shared<DrainRaceBackend>();
    auto store = openPoolForTest(backend, /*gc_fold_max_defer_rounds=*/0);
    CasRequests requests = openRequestsForTest(backend);
    CasOperation op = requests.admit();
    const Layout & layout = store->layout();
    const CompletedRemovingFixture fixture = seedCompletedRemoving(op, store, kGc);
    const uint64_t catalog_cas_before = backend->putOverwriteCount(layout.refCatalogKey());
    /// An admission in flight keeps the name's slot, so each retirement record lands in it.
    std::optional<CasRefLedger::ColdAdmission> admission = store->beginColdAdmissionForTest(fixture.ns);
    const uint64_t retirements_before = store->refNameRetirementsForTest(fixture.ns);

    Gc gc(store, kGc);
    const RebuildReport result = gc.rebuildBaseline(/*force=*/true);

    EXPECT_TRUE(result.performed);
    EXPECT_FALSE(CasRefCatalog::lifeIfCataloged(op, layout, fixture.ns));
    EXPECT_EQ(backend->putOverwriteCount(layout.refCatalogKey()), catalog_cas_before + 1);
    EXPECT_EQ(store->refNameRetirementsForTest(fixture.ns), retirements_before + 2)
        << "the rebuild drain records the retirement before the erase and again after it";
}

TEST(CASGCFrontierGate, DamagedStateRebuildDoesNotDeleteCompletedRemovingRows)
{
    auto backend = std::make_shared<DrainRaceBackend>();
    auto store = openPoolForTest(backend, /*gc_fold_max_defer_rounds=*/0);
    CasRequests requests = openRequestsForTest(backend);
    CasOperation op = requests.admit();
    const Layout & layout = store->layout();
    const RootNamespace ns{"00/damaged-rebuild-removing@cas@"};
    CasRefCatalog::casAdmitEntry(op, layout, store->poolConfig().gc_shards, CatalogEntry{
        .ns = ns, .state = NsState::Live, .incarnation = UInt128{901}});
    CasRefCatalog::casUpdate(op, layout, [](const RefCatalog & current)
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
    const uint64_t catalog_cas_before = backend->putOverwriteCount(layout.refCatalogKey());

    Gc gc(store, kGc);
    const RebuildReport result = gc.rebuildBaseline(/*force=*/false);

    EXPECT_TRUE(result.performed);
    EXPECT_TRUE(CasRefCatalog::lifeIfCataloged(op, layout, ns));
    EXPECT_EQ(backend->putOverwriteCount(layout.refCatalogKey()), catalog_cas_before);
}

TEST(CASGCFrontierGate, DeferredRoundDrainsCompletedRemovingBeforeReturning)
{
    auto backend = std::make_shared<CountingBackend>();
    auto store = openPoolForTest(backend, /*gc_fold_max_defer_rounds=*/100);
    CasRequests requests = openRequestsForTest(backend);
    CasOperation op = requests.admit();
    const Layout & layout = store->layout();
    const RootNamespace removed{"00/deferred-removed@cas@"};
    const UInt128 life_id{77};
    CasRefCatalog::casAdmitEntry(op, layout, store->poolConfig().gc_shards, CatalogEntry{
        .ns = removed, .state = NsState::Live, .incarnation = life_id});
    CasRefCatalog::casUpdate(op, layout, [&](const RefCatalog & current)
    {
        RefCatalog next = current;
        next.entries[0].state = NsState::Removing;
        next.entries[0].removal_started_round = 1;
        return next;
    });

    CasFoldSeal parent;
    parent.generation = 1;
    parent.ref_lives.emplace(life_id, RefLifeFoldState{
        .coverage = RefCoverage{.classification = CoverageClass::Folded, .last_folded_ref_id = RefTxnId{1, 1}},
        .cleanup_evidence = RefCleanupEvidence{.remove_txn_id = RefTxnId{1, 1}}});
    for (uint64_t shard = 0; shard < store->poolConfig().gc_shards; ++shard)
        parent.condemned_summary.emplace(shard, CondemnedSummary{});
    ASSERT_TRUE(std::holds_alternative<Committed>(op.create(layout.foldSealKey(1, 1), encodeFoldSeal(parent), Retry::once())));
    GcState state;
    state.round = 1;
    state.gc_shards = store->poolConfig().gc_shards;
    state.snap_generation = 1;
    state.snap_attempt = 1;
    state.lease = GcLease{.owner = kGc, .seq = 1};
    ASSERT_TRUE(std::holds_alternative<Committed>(op.create(layout.gcStateKey(), encodeGcState(state), Retry::once())));

    const String ckpt_key = layout.refCkptKey(NamespaceLifeId::fromCatalogEntry(removed, life_id));
    ASSERT_TRUE(std::holds_alternative<Committed>(op.create(ckpt_key, "inert checkpoint debris", Retry::once())));
    const uint64_t catalog_cas_before = backend->putOverwriteCount(layout.refCatalogKey());

    Gc gc(store, kGc);
    const RoundReport report = runRegularRoundReclaiming(gc);
    ASSERT_TRUE(report.acquired_lease);
    EXPECT_TRUE(report.deferred);
    EXPECT_TRUE(CasRefCatalog::read(op, layout).catalog.entries.empty());
    EXPECT_FALSE(CasRefCatalog::lifeIfCataloged(op, layout, removed));
    EXPECT_EQ(backend->putOverwriteCount(layout.refCatalogKey()), catalog_cas_before + 1);
    EXPECT_TRUE(op.head(ckpt_key, Retry::once()).has_value());
    EXPECT_EQ(backend->deleteCount(ckpt_key), 0);
}

TEST(CASGCFrontierGate, StaleIssuedCatalogCasLosesAfterNewLeaderHelpsBeforeListing)
{
    auto backend = std::make_shared<DrainRaceBackend>();
    auto store = openPoolForTest(backend, /*gc_fold_max_defer_rounds=*/0);
    CasRequests requests = openRequestsForTest(backend);
    CasOperation op = requests.admit();
    const Layout & layout = store->layout();
    const UInt128 leader_b = hexToU128("00000000000000000000000000000002");
    const CompletedRemovingFixture fixture = seedCompletedRemoving(op, store, kGc);
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
    /// `fixture.ns` is Removing with a durable `_ckpt`, same shape as
    /// `CleanupEvidenceLeavesRemovedNamespaceCheckpointForJanitor`: once leader_b's round drops its
    /// catalog row, the resulting cut is genuinely, provably empty, the destructive gate opens, and the
    /// namespace janitor -- a separate `namespace_cleanup` phase within this SAME round -- reclaims the
    /// checkpoint. Captured so the assertions below can attribute the delete to the janitor rather than
    /// assume survival.
    std::map<String, UInt64> janitor_metrics_b;
    try
    {
        Gc gc_b(store, leader_b);
        gc_b.setPhaseSink([&](const GcPhaseRecord & rec)
        {
            if (rec.phase == "namespace_cleanup")
                janitor_metrics_b = rec.metrics;
        });
        report_b = runRegularRoundReclaiming(gc_b);
        gc_b.setPhaseSink({});
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
    ASSERT_TRUE(CasRefCatalog::read(op, layout).catalog.entries.empty());

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
    const GcState adopted = decodeGcState(op.read(layout.gcStateKey(), Retry::once())->bytes);
    const String successor_seal_key = layout.foldSealKey(adopted.snap_generation, adopted.snap_attempt);
    const size_t successor_seal_put = findJournalAfter(
        before_a_release, "put_end " + successor_seal_key, fresh_catalog_cut + 1);
    ASSERT_LT(successor_seal_put, before_a_release.size());
    const size_t successor_adoption = findJournalAfter(
        before_a_release, "cas_end " + layout.gcStateKey(), successor_seal_put + 1);
    ASSERT_LT(successor_adoption, before_a_release.size());
    EXPECT_LT(catalog_cas_end, conclusive_rescan);
    EXPECT_LT(conclusive_rescan, stream_list);
    EXPECT_LT(stream_list, fresh_catalog_cut);
    /// The invariant this ordering must still prove: the fold's OWN walk plan is built from the single
    /// hot-scan cut, taken immediately after the ref-object LIST, with no earlier catalog read sneaking
    /// into that construction. `fresh_catalog_cut` is defined as the FIRST catalog `get` after
    /// `stream_list` (the `findJournalAfter` search above), so that already holds by construction --
    /// the walk plan physically cannot have consumed an earlier one.
    ///
    /// What this test used to also assert -- no SECOND catalog read anywhere before the seal PUT -- is
    /// no longer the right claim once the destructive gate can open on a proved-empty cut: other
    /// destructive families this SAME round now also runs (the orphan-manifest sweep, the namespace
    /// janitor) take their OWN separate catalog cuts by design, each after its own candidate listing,
    /// to resolve authority against a fresh read rather than the fold's frozen one -- exactly the shape
    /// measured here (`list p/cas/manifests/` immediately followed by a second `get
    /// p/cas/ref_catalog`, before the seal PUT, from the orphan sweep). That is expected, not redundant,
    /// so it is not asserted against; the fold's own single-cut plan construction is what remains pinned.
    EXPECT_LT(fresh_catalog_cut, successor_seal_put);
    EXPECT_LT(successor_seal_put, successor_adoption);

    ASSERT_TRUE(leader_a_failure);
    EXPECT_FALSE(CasRefCatalog::lifeIfCataloged(op, layout, fixture.ns));
    /// Same discrimination as `CleanupEvidenceLeavesRemovedNamespaceCheckpointForJanitor`: leader_b's
    /// round both drops `fixture.ns`'s catalog row AND, because the resulting cut is genuinely,
    /// provably empty, opens the destructive gate -- so the namespace janitor reclaims the checkpoint
    /// in this SAME round. Attribute the delete to the janitor's own counter rather than assume either
    /// survival (the old expectation) or absence (which an unchecked dereference here cannot
    /// distinguish from "never existed").
    ASSERT_FALSE(janitor_metrics_b.empty()) << "the namespace_cleanup phase must have run this round";
    EXPECT_GE(janitor_metrics_b.at("janitor_deleted"), 1u)
        << "the janitor's OWN counter must show the delete, now that the proved-empty gate has opened";
    EXPECT_FALSE(op.read(fixture.checkpoint_key, Retry::once()).has_value());
    EXPECT_EQ(backend->deleteCount(fixture.checkpoint_key), 1);
}

TEST(CASGCFrontierGate, LostCatalogCasResponseIsResolvedBeforeListing)
{
    auto backend = std::make_shared<DrainRaceBackend>();
    auto store = openPoolForTest(backend, /*gc_fold_max_defer_rounds=*/0);
    CasRequests requests = openRequestsForTest(backend);
    CasOperation op = requests.admit();
    const Layout & layout = store->layout();
    const CompletedRemovingFixture fixture = seedCompletedRemoving(op, store, kGc);
    backend->clearJournal();
    backend->loseNextCatalogCasResponse(layout.refCatalogKey());

    /// Same shape as `StaleIssuedCatalogCasLosesAfterNewLeaderHelpsBeforeListing`: `fixture.ns` is
    /// Removing with a durable `_ckpt`, so this round both drops its catalog row and, because the
    /// resulting cut is genuinely, provably empty, opens the destructive gate -- the namespace janitor
    /// (a separate `namespace_cleanup` phase within this SAME round) reclaims the checkpoint. Captured
    /// so the assertions below attribute the delete to the janitor's own counter.
    std::map<String, UInt64> janitor_metrics;
    Gc gc(store, kGc);
    gc.setPhaseSink([&](const GcPhaseRecord & rec)
    {
        if (rec.phase == "namespace_cleanup")
            janitor_metrics = rec.metrics;
    });
    const RoundReport report = runRegularRoundReclaiming(gc);
    gc.setPhaseSink({});
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

    EXPECT_FALSE(CasRefCatalog::lifeIfCataloged(op, layout, fixture.ns));
    /// See the discrimination comment in `CleanupEvidenceLeavesRemovedNamespaceCheckpointForJanitor`:
    /// attribute the delete to the janitor's own counter, never to end-state absence alone, and never
    /// assume survival -- both would be indistinguishable from a bug on this exact line (the old
    /// unchecked `->bytes` here is what aborted the whole binary once the janitor started reclaiming).
    ASSERT_FALSE(janitor_metrics.empty()) << "the namespace_cleanup phase must have run this round";
    EXPECT_GE(janitor_metrics.at("janitor_deleted"), 1u)
        << "the janitor's OWN counter must show the delete, now that the proved-empty gate has opened";
    EXPECT_FALSE(op.read(fixture.checkpoint_key, Retry::once()).has_value());
    EXPECT_EQ(backend->deleteCount(fixture.checkpoint_key), 1);
}

/// A stale leader may learn from its mandatory resolution read that the old life is gone, and must
/// invalidate that exact runtime, but loss of the leader fence remains the control outcome. It must
/// abort before the hot LIST and cannot build or publish any successor generation.
TEST_P(CASGCCompletedRemovalFenceRace, FencedLeaderStopsAfterWinnerRemovesOrReplacesLife)
{
    auto backend = std::make_shared<DrainRaceBackend>();
    auto store = openPoolForTest(backend, /*gc_fold_max_defer_rounds=*/0);
    CasRequests requests = openRequestsForTest(backend);
    CasOperation op = requests.admit();
    const Layout & layout = store->layout();
    const UInt128 leader_b = hexToU128("00000000000000000000000000000002");
    const CompletedRemovingFixture fixture = seedCompletedRemoving(op, store, kGc);
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
    const CasRefCatalog::Snapshot observed = CasRefCatalog::read(op, layout);
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
        op.create(layout.refCkptKey(successor_life), encodeRefCkptAtGeneration(RefCkpt{
            .life_epoch = 1,
            .committed_through = std::nullopt,
            .checkpoint_snapshot_id = std::nullopt,
            .last_epoch_seal = std::nullopt,
        }, 1), Retry::once());
    }
    ASSERT_TRUE(observed.etag);
    ASSERT_TRUE(std::holds_alternative<Committed>(op.replace(
        layout.refCatalogKey(), encodeRefCatalog(winner_catalog), *observed.etag, Retry::once())));

    backend->clearJournal();
    const uint64_t plans_before  /// NOLINT(clang-analyzer-deadcode.DeadStores)
        = ProfileEvents::global_counters[ProfileEvents::CASGCRefWalkPlansBuilt];
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
    EXPECT_EQ(ProfileEvents::global_counters[ProfileEvents::CASGCRefWalkPlansBuilt] - plans_before, 0u);
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
    CASWinnerShape,
    CASGCCompletedRemovalFenceRace,
    testing::Values(CompetingCatalogOutcome::Absent, CompetingCatalogOutcome::Replacement),
    [](const testing::TestParamInfo<CompetingCatalogOutcome> & parameter)
    {
        return parameter.param == CompetingCatalogOutcome::Absent ? "Absent" : "Replacement";
    });

/// One initial full catalog read selects the first row; each successful erase's mandatory resolution
/// read becomes the next selection snapshot. Therefore N uncontended deletes cost N+1 reads before
/// the hot LIST. The round then takes one post-LIST walk-plan cut and, later in the separate
/// `namespace_cleanup` phase, one post-page janitor cut.
TEST(CASGCFrontierGate, CompletedRemovalDrainUsesNPlusOneCatalogReads)
{
    auto backend = std::make_shared<DrainRaceBackend>();
    auto store = openPoolForTest(backend, /*gc_fold_max_defer_rounds=*/0);
    CasRequests requests = openRequestsForTest(backend);
    CasOperation op = requests.admit();
    const Layout & layout = store->layout();
    constexpr size_t deletes = 3;
    seedCompletedRemovingBatch(op, store, kGc, deletes);
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
    /// Between the hot walk-plan cut and the janitor's own page, the orphan-manifest sweep -- ANOTHER
    /// destructive family the now-open gate also unlocks (the batch drain above empties the catalog, so
    /// this round's frontier is proved empty and the sweep's own `!suppress_destructive` gate opens
    /// too) -- lists its own manifest candidates and takes its OWN separate catalog cut to resolve
    /// authority, exactly as the janitor does. Located explicitly so the final read count below states
    /// what it counts rather than drifting silently the next time a family is unlocked.
    const size_t orphan_sweep_list = findJournalAfter(journal, "list " + layout.casManifestsPrefix(), walk_plan_cut);
    ASSERT_LT(orphan_sweep_list, journal.size());
    const size_t orphan_sweep_cut = findJournalAfter(journal, catalog_get, orphan_sweep_list);
    ASSERT_LT(orphan_sweep_cut, journal.size());
    const size_t janitor_list
        = findJournalAfter(journal, "list " + layout.namespaceRootPrefix(), orphan_sweep_cut);
    ASSERT_LT(janitor_list, journal.size());
    const size_t janitor_cut = findJournalAfter(journal, catalog_get, janitor_list);
    ASSERT_LT(janitor_cut, journal.size());
    EXPECT_EQ(findJournalAfter(journal, catalog_get, janitor_cut + 1), journal.size())
        << "one hot walk-plan cut, one orphan-sweep cut, and one janitor page cut are the only "
           "post-drain catalog reads";
    EXPECT_EQ(backend->listCount(layout.namespaceStreamRootPrefix()), 1u);
    EXPECT_EQ(backend->listCount(layout.namespaceRootPrefix()), 1u);
    EXPECT_EQ(backend->getCount(layout.refCatalogKey()), deletes + 4)
        << "N+1 drain reads, one post-hot-LIST walk-plan cut, one orphan-manifest-sweep cut (now that "
           "the proved-empty gate has opened, unlocking that destructive family too), and one separate "
           "post-janitor-page cut";
}

/// ===================== THE OWNING MOUNT CANCELS ITS OWN IDLE `Creating` ROWS =====================
///
/// A cold admission that lands its `Creating` row and then fails leaves the row under this mount's live
/// writer epoch, which no certificate covers. The scheduler's per-tick pass removes such a row once no
/// admission of its name is in flight here.

namespace
{

/// Counts the catalog requests the owner tests assert on, and holds, parks, loses or fails one
/// conditional catalog write on demand. Every control is armed before the request it targets is sent.
class OwnerCancelBackend final : public CountingBackend
{
public:
    using CountingBackend::list;

    const String catalog_key = Layout{"p"}.refCatalogKey();

    size_t catalogGets() const
    {
        std::lock_guard lock(control_mutex);
        return catalog_gets;
    }
    size_t catalogCasBegins() const
    {
        std::lock_guard lock(control_mutex);
        return catalog_cas_begins;
    }
    size_t ckptWrites() const
    {
        std::lock_guard lock(control_mutex);
        return ckpt_writes;
    }
    std::vector<String> catalogCasBodies() const
    {
        std::lock_guard lock(control_mutex);
        return catalog_cas_bodies;
    }
    void resetCatalogCounts()
    {
        std::lock_guard lock(control_mutex);
        catalog_gets = 0;
        catalog_cas_begins = 0;
        ckpt_writes = 0;
        catalog_cas_bodies.clear();
    }

    /// The next conditional catalog write is not applied: `on_hold` runs, and the call fails as a lost
    /// response would. `applyHeld` lands it later iff the catalog still has the bytes it was decided on.
    void holdNextCatalogCasUnresolved(std::function<void()> on_hold)
    {
        std::lock_guard lock(control_mutex);
        hold_next = true;
        hold_hook = std::move(on_hold);
    }
    bool applyHeld(CasOperation & op)
    {
        std::optional<String> bytes;
        String base;
        {
            std::lock_guard lock(control_mutex);
            bytes = held_bytes;
            base = held_base;
        }
        if (!bytes)
            return false;
        const std::optional<DB::Cas::Object> current = op.read(catalog_key, Retry::standard());
        if (!current || current->bytes != base)
            return false;
        return std::holds_alternative<Committed>(op.replace(catalog_key, *bytes, current->etag, Retry::once()));
    }

    /// The next conditional catalog write waits inside the call until `releaseCatalogCas`.
    void parkNextCatalogCas() { cas_park.arm(control_mutex); }
    bool waitCatalogCasParked(const std::atomic<bool> & stop) { return cas_park.wait(control_mutex, stop); }
    void releaseCatalogCas() { cas_park.release(control_mutex); }

    /// The next write of exactly `key` waits inside the call until `releaseKeyWrite`.
    void parkNextWriteOf(const String & key)
    {
        std::lock_guard lock(control_mutex);
        park_key = key;
        key_park.armLocked();
    }
    bool waitKeyWriteParked(const std::atomic<bool> & stop) { return key_park.wait(control_mutex, stop); }
    void releaseKeyWrite() { key_park.release(control_mutex); }

    /// Every conditional catalog write whose bytes satisfy `when` fails before it is applied, with an
    /// error the engine never treats as possibly landed.
    void failCatalogCasWhen(std::function<bool(const String &)> when)
    {
        std::lock_guard lock(control_mutex);
        fail_when = std::move(when);
    }

    /// The next conditional catalog write is applied and its response lost; `after_landing` runs between.
    void loseNextCatalogCasResponse(std::function<void()> after_landing = {})
    {
        std::lock_guard lock(control_mutex);
        lose_next = true;
        lose_hook = std::move(after_landing);
    }
    bool responseLost() const
    {
        std::lock_guard lock(control_mutex);
        return response_lost;
    }

    /// Runs after each completed read of the catalog, including reads the hook itself makes.
    void afterCatalogRead(std::function<void()> hook)
    {
        std::lock_guard lock(control_mutex);
        after_read = std::move(hook);
    }

    std::optional<Raw> read(const String & key, TransportAccess & access) override
    {
        std::optional<Raw> raw = CountingBackend::read(key, access);
        if (key != catalog_key)
            return raw;
        std::function<void()> hook;
        {
            std::lock_guard lock(control_mutex);
            ++catalog_gets;
            hook = after_read;
        }
        if (hook)
            hook();
        return raw;
    }

    std::expected<String, RawConflict> write(
        const String & key, const String & bytes, const std::optional<String> & expected_value,
        TransportAccess & access) override
    {
        const bool catalog_cas = key == catalog_key && expected_value.has_value();
        std::function<void()> on_hold;
        std::function<void()> on_lost;
        bool hold = false;
        bool lose = false;
        {
            std::unique_lock lock(control_mutex);
            if (key.find("_ckpt") != String::npos)
                ++ckpt_writes;
            if (catalog_cas)
            {
                ++catalog_cas_begins;
                catalog_cas_bodies.push_back(bytes);
                cas_park.parkIfArmed(lock);
            }
            if (!park_key.empty() && key == park_key)
            {
                park_key.clear();
                key_park.parkIfArmed(lock);
            }
            if (catalog_cas && fail_when && fail_when(bytes))
                throw std::runtime_error("OwnerCancelBackend: injected catalog write failure, not applied");
            if (catalog_cas && std::exchange(hold_next, false))
            {
                hold = true;
                held_bytes = bytes;
                on_hold = std::move(hold_hook);
            }
            if (catalog_cas && std::exchange(lose_next, false))
            {
                lose = true;
                on_lost = std::move(lose_hook);
            }
        }
        if (hold)
        {
            const std::optional<Raw> base = CountingBackend::read(key, access);
            {
                std::lock_guard lock(control_mutex);
                held_base = base ? base->bytes : String{};
            }
            if (on_hold)
                on_hold();
            throw Poco::TimeoutException("OwnerCancelBackend: held catalog write, not applied yet");
        }
        std::expected<String, RawConflict> result = CountingBackend::write(key, bytes, expected_value, access);
        if (lose && result.has_value())
        {
            {
                std::lock_guard lock(control_mutex);
                response_lost = true;
            }
            if (on_lost)
                on_lost();
            throw Poco::TimeoutException("OwnerCancelBackend: lost catalog write response");
        }
        return result;
    }

private:
    /// One request held inside the backend call, released by the test.
    struct Park
    {
        bool armed = false;
        bool parked = false;
        bool released = false;
        std::condition_variable cv;

        void armLocked()
        {
            armed = true;
            parked = false;
            released = false;
        }
        void arm(std::mutex & mutex)
        {
            std::lock_guard lock(mutex);
            armLocked();
        }
        void parkIfArmed(std::unique_lock<std::mutex> & lock)
        {
            if (!std::exchange(armed, false))
                return;
            parked = true;
            cv.notify_all();
            cv.wait_for(lock, std::chrono::minutes(1), [&] { return released; });
        }
        /// False when `stop` turns true or a minute passes before a request parks.
        bool wait(std::mutex & mutex, const std::atomic<bool> & stop)
        {
            const auto deadline = std::chrono::steady_clock::now() + std::chrono::minutes(1);
            std::unique_lock lock(mutex);
            while (!parked)
            {
                if (stop || std::chrono::steady_clock::now() >= deadline)
                    return false;
                cv.wait_for(lock, std::chrono::milliseconds(10));
            }
            return true;
        }
        void release(std::mutex & mutex)
        {
            std::lock_guard lock(mutex);
            armed = false;
            released = true;
            cv.notify_all();
        }
    };

    mutable std::mutex control_mutex;
    size_t catalog_gets = 0;
    size_t catalog_cas_begins = 0;
    size_t ckpt_writes = 0;
    std::vector<String> catalog_cas_bodies;
    bool hold_next = false;
    std::function<void()> hold_hook;
    std::optional<String> held_bytes;
    String held_base;
    Park cas_park;
    Park key_park;
    String park_key;
    std::function<bool(const String &)> fail_when;
    bool lose_next = false;
    std::function<void()> lose_hook;
    bool response_lost = false;
    std::function<void()> after_read;
};

/// Lease renewal parked an hour out, so only the test moves the mount deadline.
PoolPtr openOwnerPool(const std::shared_ptr<OwnerCancelBackend> & backend, const String & root = "test")
{
    PoolConfig config{.pool_prefix = "p", .server_root_id = root, .gc_fold_max_defer_rounds = 0};
    config.mount_renew_period = std::chrono::milliseconds{3'600'000};
    return Pool::open(backend, config);
}

void expireLease(const PoolPtr & store)
{
    store->setMountDeadline(store->bootMsNow() - 5'000);
}

void restoreLease(const PoolPtr & store)
{
    store->setMountDeadline(store->bootMsNow() + 600'000);
}

CreatorFence liveFenceOf(const PoolPtr & store)
{
    return CreatorFence{.server_root_id = store->poolConfig().server_root_id,
                        .writer_epoch = store->writerEpoch(), .fence_generation = store->fenceGeneration()};
}

uint64_t ownCancels()
{
    return ProfileEvents::global_counters[ProfileEvents::CASOwnCreatingCancelled];
}

/// An interrupted `CREATE`: step 1 lands, then the lease budget runs out before the `_ckpt` create.
/// Returns the row left behind, with the lease restored at the same fence generation.
CatalogEntry failBirthAfterStep1(const PoolPtr & store, CasOperation & op, const RootNamespace & ns)
{
    CasHotKeys & lane = store->mountRequests().admit().hotKeys();
    bool fired = false;
    lane.cache_fill_hook_for_test = [&]
    {
        if (!std::exchange(fired, true))
            expireLease(store);
    };
    const uint64_t generation = store->fenceGeneration();
    bool threw = false;
    try
    {
        publishBirth(store, ns, "a");
    }
    catch (const DB::Exception & e)
    {
        threw = true;
        EXPECT_EQ(e.code(), DB::ErrorCodes::NETWORK_ERROR) << e.message();
    }
    lane.cache_fill_hook_for_test = {};
    restoreLease(store);
    EXPECT_TRUE(fired) << "the hook on the step 1 commit did not run";
    EXPECT_TRUE(threw) << "the birth must fail after its step 1";
    EXPECT_EQ(store->fenceGeneration(), generation);
    const std::optional<CatalogEntry> row = catalogRow(op, store->layout(), ns);
    if (!row)
    {
        ADD_FAILURE() << "the failed birth left no catalog row";
        return {};
    }
    EXPECT_EQ(row->state, NsState::Creating);
    EXPECT_EQ(row->creator, std::optional<CreatorFence>{liveFenceOf(store)});
    return *row;
}

bool catalogBodyLacks(const String & bytes, const RootNamespace & ns)
{
    return bytes.find(ns.string()) == String::npos;
}

}

TEST(CASOwnCreatingCancel, ABudgetRefusalAfterStep1IsCancelledOnceTheLeaseRecovers)
{
    auto backend = std::make_shared<OwnerCancelBackend>();
    PoolPtr store = openOwnerPool(backend);
    CasRequests requests = openRequestsForTest(backend);
    CasOperation op = requests.admit();
    const RootNamespace ns{"00/interrupted@cas@"};

    backend->resetCatalogCounts();
    const CatalogEntry row = failBirthAfterStep1(store, op, ns);
    EXPECT_EQ(backend->ckptWrites(), 0u) << "the budget ran out before the `_ckpt` create";

    expireLease(store);
    backend->resetCatalogCounts();
    store->cancelIdleOwnCreatingRows();
    EXPECT_EQ(backend->catalogGets() + backend->catalogCasBegins(), 0u) << "an unadmitted pass sends nothing";
    EXPECT_EQ(catalogRow(op, store->layout(), ns), std::optional<CatalogEntry>{row});

    restoreLease(store);
    const uint64_t before = ownCancels();
    store->cancelIdleOwnCreatingRows();
    EXPECT_FALSE(catalogRow(op, store->layout(), ns)) << "the owner pass cancels its own idle row";
    EXPECT_EQ(ownCancels() - before, 1u);
}

TEST(CASOwnCreatingCancel, ALateFirstPutAfterAnEmptyPassIsCancelled)
{
    auto backend = std::make_shared<OwnerCancelBackend>();
    PoolPtr store = openOwnerPool(backend);
    CasRequests requests = openRequestsForTest(backend);
    CasOperation op = requests.admit();
    const RootNamespace ns{"00/late@cas@"};

    backend->holdNextCatalogCasUnresolved([&] { expireLease(store); });
    EXPECT_THROW(publishBirth(store, ns, "a"), DB::Exception);
    restoreLease(store);
    ASSERT_FALSE(catalogRow(op, store->layout(), ns)) << "step 1 is held, not applied";

    backend->resetCatalogCounts();
    store->cancelIdleOwnCreatingRows();
    EXPECT_EQ(backend->catalogGets(), 1u) << "every pass scans the catalog once";
    EXPECT_EQ(backend->catalogCasBegins(), 0u);

    ASSERT_TRUE(backend->applyHeld(op)) << "the held step 1 lands on the unchanged catalog";
    const std::optional<CatalogEntry> landed = catalogRow(op, store->layout(), ns);
    ASSERT_TRUE(landed);
    EXPECT_EQ(landed->creator, std::optional<CreatorFence>{liveFenceOf(store)});

    const uint64_t before = ownCancels();
    store->cancelIdleOwnCreatingRows();
    EXPECT_FALSE(catalogRow(op, store->layout(), ns)) << "a row that lands after a pass is seen by the next one";
    EXPECT_EQ(ownCancels() - before, 1u);
}

TEST(CASOwnCreatingCancel, ATakeoverThatGaveUpAfterLandingIsCancelled)
{
    auto backend = std::make_shared<OwnerCancelBackend>();
    PoolPtr store = openOwnerPool(backend);
    CasRequests requests = openRequestsForTest(backend);
    CasOperation op = requests.admit();
    const Layout & layout = store->layout();
    const CatalogEntry foreign = seedStalledCreating(op, store, "00/taken@cas@",
        hexToU128("000000000000000000000000000000e1"), CreatorFence{.server_root_id = "gone", .writer_epoch = 1, .fence_generation = 1});
    retireCreatorRootAt(*backend, layout, "gone", /*epoch*/ 2);

    std::atomic<bool> fired = false;
    backend->onWriteCommitted(layout.refCatalogKey(), [&]
    {
        if (!fired.exchange(true))
            expireLease(store);
    });
    EXPECT_THROW(publishBirth(store, foreign.ns, "a"), DB::Exception);
    backend->onWriteCommitted(layout.refCatalogKey(), {});
    restoreLease(store);
    ASSERT_TRUE(fired);

    const std::optional<CatalogEntry> taken = catalogRow(op, layout, foreign.ns);
    ASSERT_TRUE(taken);
    EXPECT_EQ(taken->state, NsState::Creating);
    EXPECT_EQ(taken->incarnation, foreign.incarnation);
    EXPECT_EQ(taken->creator, std::optional<CreatorFence>{liveFenceOf(store)}) << "the takeover landed before the give-up";

    const uint64_t before = ownCancels();
    store->cancelIdleOwnCreatingRows();
    EXPECT_FALSE(catalogRow(op, layout, foreign.ns));
    EXPECT_EQ(ownCancels() - before, 1u);
}

TEST(CASOwnCreatingCancel, AHealthyMountSendsOneGetAndNoPut)
{
    auto backend = std::make_shared<OwnerCancelBackend>();
    PoolPtr store = openOwnerPool(backend);
    publishBirth(store, RootNamespace{"00/healthy@cas@"}, "a");

    backend->resetCatalogCounts();
    store->cancelIdleOwnCreatingRows();
    EXPECT_EQ(backend->catalogGets(), 1u);
    EXPECT_EQ(backend->catalogCasBegins(), 0u);
}

TEST(CASOwnCreatingCancel, ASiblingParkedInsideCompletionKeepsItsLife)
{
    auto backend = std::make_shared<OwnerCancelBackend>();
    PoolPtr store = openOwnerPool(backend);
    CasRequests requests = openRequestsForTest(backend);
    CasOperation op = requests.admit();
    const Layout & layout = store->layout();
    const RootNamespace a{"00/a@cas@"};
    const RootNamespace b{"00/b@cas@"};
    failBirthAfterStep1(store, op, a);

    /// `b`'s `_ckpt` key names the life its step 1 mints, so the park is armed on that commit.
    CasHotKeys & lane = store->mountRequests().admit().hotKeys();
    std::optional<CatalogEntry> b_step1;
    bool park_armed = false;
    lane.cache_fill_hook_for_test = [&]
    {
        if (std::exchange(park_armed, true))
            return;
        CasOperation hook_op = requests.admit();
        b_step1 = catalogRow(hook_op, layout, b);
        if (b_step1)
            backend->parkNextWriteOf(layout.refCkptKey(NamespaceLifeId::fromCatalogEntry(b, b_step1->incarnation)));
    };
    std::atomic<bool> b_returned = false;
    std::thread sibling([&]
    {
        try
        {
            publishBirth(store, b, "b");
        }
        catch (...)
        {
            ADD_FAILURE() << DB::getCurrentExceptionMessage(false);
        }
        b_returned = true;
    });
    const bool b_parked = backend->waitKeyWriteParked(b_returned);
    backend->resetCatalogCounts();

    std::atomic<size_t> submissions = 0;
    lane.enter_after_lane_hook_for_test = [&] { ++submissions; };
    const uint64_t before = ownCancels();
    if (b_parked)
        store->cancelIdleOwnCreatingRows();
    lane.enter_after_lane_hook_for_test = {};
    backend->releaseKeyWrite();
    sibling.join();
    lane.cache_fill_hook_for_test = {};
    ASSERT_TRUE(b_parked) << "the sibling did not park in its `_ckpt` create";
    ASSERT_TRUE(b_step1);

    EXPECT_EQ(submissions.load(), 1u) << "only `a` reaches the catalog lane";
    EXPECT_EQ(ownCancels() - before, 1u);
    EXPECT_FALSE(catalogRow(op, layout, a));
    for (const String & body : backend->catalogCasBodies())
        EXPECT_FALSE(catalogBodyLacks(body, b)) << "no catalog write after the park drops `b`";
    const std::optional<CatalogEntry> b_now = catalogRow(op, layout, b);
    ASSERT_TRUE(b_now);
    EXPECT_EQ(b_now->state, NsState::Live);
    EXPECT_EQ(b_now->incarnation, b_step1->incarnation);
}

TEST(CASOwnCreatingCancel, AnAdmissionStartingInsideTheLaneKeepsTheRow)
{
    auto backend = std::make_shared<OwnerCancelBackend>();
    PoolPtr store = openOwnerPool(backend);
    CasRequests requests = openRequestsForTest(backend);
    CasOperation op = requests.admit();
    const Layout & layout = store->layout();
    const String catalog_key = layout.refCatalogKey();
    const RootNamespace a{"00/a@cas@"};
    const CatalogEntry row = failBirthAfterStep1(store, op, a);

    CasOperation lane_op = store->mountRequests().admit();
    std::latch holding(1);
    std::latch release(1);
    std::thread holder([&]
    {
        CasOperation holder_op = store->mountRequests().admit();
        (void)holder_op.hotKeys().submit(catalog_key, holder_op, holder_op.freeze(Retry::standard()),
            [&](const std::optional<Object> &) -> std::optional<String>
            {
                holding.count_down();
                release.wait();
                return std::nullopt;
            });
    });
    holding.wait();

    backend->resetCatalogCounts();
    const uint64_t before = ownCancels();
    std::atomic<bool> pass_returned = false;
    std::thread pass([&]
    {
        store->cancelIdleOwnCreatingRows();
        pass_returned = true;
    });
    const bool queued = waitForLaneDepth(lane_op, catalog_key, 2, pass_returned);
    std::optional<CasRefLedger::ColdAdmission> admission;
    admission.emplace(store->beginColdAdmissionForTest(a));
    release.count_down();
    pass.join();
    holder.join();
    ASSERT_TRUE(queued) << "the pass returned, or did not queue for the catalog lane within a minute";

    for (const String & body : backend->catalogCasBodies())
        EXPECT_FALSE(catalogBodyLacks(body, a)) << "the admission that began while the cancel queued keeps the row";
    EXPECT_EQ(ownCancels() - before, 0u);
    EXPECT_EQ(catalogRow(op, layout, a), std::optional<CatalogEntry>{row});

    admission.reset();
    store->cancelIdleOwnCreatingRows();
    EXPECT_FALSE(catalogRow(op, layout, a)) << "once the admission ends, the next pass removes the row";
    EXPECT_EQ(ownCancels() - before, 1u);
}

TEST(CASOwnCreatingCancel, AFlipLandingFirstKeepsTheLiveRow)
{
    auto backend = std::make_shared<OwnerCancelBackend>();
    PoolPtr store = openOwnerPool(backend);
    CasRequests requests = openRequestsForTest(backend);
    CasOperation op = requests.admit();
    const Layout & layout = store->layout();
    const RootNamespace a{"00/a@cas@"};
    const CatalogEntry row = failBirthAfterStep1(store, op, a);

    backend->parkNextCatalogCas();
    const uint64_t before = ownCancels();
    std::atomic<bool> pass_returned = false;
    std::thread pass([&]
    {
        store->cancelIdleOwnCreatingRows();
        pass_returned = true;
    });
    const bool erase_held = backend->waitCatalogCasParked(pass_returned);
    /// An independent lane, so the flip is not queued behind the parked erase.
    CasRequests other = openRequestsForTest(backend);
    CasOperation other_op = other.admit();
    const auto flip = erase_held ? CasRefCatalog::completeCreation(other_op, layout, row)
                                 : CasRefCatalog::NamespaceCreationOutcome::Superseded;
    backend->releaseCatalogCas();
    pass.join();
    ASSERT_TRUE(erase_held) << "the owner pass sent no erase";
    EXPECT_EQ(flip, CasRefCatalog::NamespaceCreationOutcome::Live);

    const std::optional<CatalogEntry> now = catalogRow(op, layout, a);
    ASSERT_TRUE(now);
    EXPECT_EQ(now->state, NsState::Live);
    EXPECT_EQ(now->incarnation, row.incarnation);
    EXPECT_EQ(ownCancels() - before, 0u);
}

TEST(CASOwnCreatingCancel, AnEraseLandingFirstSendsTheCreatorToAFreshLife)
{
    auto backend = std::make_shared<OwnerCancelBackend>();
    PoolPtr store = openOwnerPool(backend);
    CasRequests requests = openRequestsForTest(backend);
    CasOperation op = requests.admit();
    const Layout & layout = store->layout();
    const RootNamespace a{"00/a@cas@"};

    /// Step 1 and the `_ckpt` land; the flip fails before it is applied.
    size_t catalog_writes = 0;
    backend->failCatalogCasWhen([&](const String &) { return ++catalog_writes == 2; });
    EXPECT_ANY_THROW(publishBirth(store, a, "a"));
    backend->failCatalogCasWhen({});
    const std::optional<CatalogEntry> row = catalogRow(op, layout, a);
    ASSERT_TRUE(row);
    ASSERT_EQ(row->state, NsState::Creating);
    const String old_ckpt = layout.refCkptKey(NamespaceLifeId::fromCatalogEntry(a, row->incarnation));
    ASSERT_TRUE(op.head(old_ckpt, Retry::once()));

    backend->parkNextCatalogCas();
    const uint64_t before = ownCancels();
    std::atomic<bool> pass_returned = false;
    std::thread pass([&]
    {
        store->cancelIdleOwnCreatingRows();
        pass_returned = true;
    });
    const bool erase_held = backend->waitCatalogCasParked(pass_returned);

    /// The retry reads the old row and waits in its `_ckpt` create; the erase lands; then the retry flips.
    std::atomic<bool> retry_returned = false;
    std::optional<RefTxnId> retried;
    std::exception_ptr retry_error;
    backend->parkNextWriteOf(old_ckpt);
    std::thread retry([&]
    {
        try
        {
            retried = publishBirth(store, a, "b");
        }
        catch (...)
        {
            retry_error = std::current_exception();
        }
        retry_returned = true;
    });
    const bool retry_parked = backend->waitKeyWriteParked(retry_returned);
    backend->releaseCatalogCas();
    pass.join();
    backend->releaseKeyWrite();
    retry.join();
    ASSERT_TRUE(erase_held) << "the owner pass sent no erase";
    EXPECT_EQ(ownCancels() - before, 1u);
    EXPECT_TRUE(retry_parked);
    ASSERT_FALSE(retry_error) << DB::getExceptionMessage(retry_error, false);
    ASSERT_TRUE(retried);

    const std::optional<CatalogEntry> fresh = catalogRow(op, layout, a);
    ASSERT_TRUE(fresh);
    EXPECT_EQ(fresh->state, NsState::Live);
    EXPECT_NE(fresh->incarnation, row->incarnation) << "the creator continues under a fresh life";
    EXPECT_TRUE(op.head(old_ckpt, Retry::once())) << "the cancelled life's `_ckpt` is left to the janitor";
    EXPECT_EQ(store->namespaceLife(a), NamespaceLifeId::fromCatalogEntry(a, fresh->incarnation));
}

TEST(CASOwnCreatingCancel, ALostEraseAcknowledgementCountsOnce)
{
    auto backend = std::make_shared<OwnerCancelBackend>();
    PoolPtr store = openOwnerPool(backend);
    CasRequests requests = openRequestsForTest(backend);
    CasOperation op = requests.admit();
    const RootNamespace a{"00/a@cas@"};
    failBirthAfterStep1(store, op, a);

    backend->loseNextCatalogCasResponse();
    const uint64_t before = ownCancels();
    EXPECT_NO_THROW(store->cancelIdleOwnCreatingRows());
    EXPECT_TRUE(backend->responseLost());
    EXPECT_FALSE(catalogRow(op, store->layout(), a));
    EXPECT_EQ(ownCancels() - before, 1u);
}

TEST(CASOwnCreatingCancel, ALostEraseAcknowledgementSettledOnOtherBytesIsNotCounted)
{
    auto backend = std::make_shared<OwnerCancelBackend>();
    PoolPtr store = openOwnerPool(backend);
    CasRequests requests = openRequestsForTest(backend);
    CasOperation op = requests.admit();
    const RootNamespace a{"00/a@cas@"};
    failBirthAfterStep1(store, op, a);

    backend->loseNextCatalogCasResponse([&]
    {
        seedStalledCreating(op, store, "00/z@cas@", hexToU128("000000000000000000000000000000e9"),
            CreatorFence{.server_root_id = "gone", .writer_epoch = 1, .fence_generation = 1});
    });
    const uint64_t before = ownCancels();
    EXPECT_NO_THROW(store->cancelIdleOwnCreatingRows());
    EXPECT_TRUE(backend->responseLost());
    EXPECT_FALSE(catalogRow(op, store->layout(), a));
    EXPECT_TRUE(catalogRow(op, store->layout(), RootNamespace{"00/z@cas@"}));
    EXPECT_EQ(ownCancels() - before, 0u);
}

TEST(CASOwnCreatingCancel, RowsOfOtherCreatorsAreUntouched)
{
    auto backend = std::make_shared<OwnerCancelBackend>();
    PoolPtr store = openOwnerPool(backend);
    CasRequests requests = openRequestsForTest(backend);
    CasOperation op = requests.admit();
    const CreatorFence ours = liveFenceOf(store);
    CreatorFence other_root = ours;
    other_root.server_root_id = "other";
    CreatorFence other_epoch = ours;
    other_epoch.writer_epoch = ours.writer_epoch + 1;
    CreatorFence other_generation = ours;
    other_generation.fence_generation = ours.fence_generation + 1;
    const CatalogEntry r1 = seedStalledCreating(op, store, "00/r1@cas@", UInt128{0xf1}, other_root);
    const CatalogEntry r2 = seedStalledCreating(op, store, "00/r2@cas@", UInt128{0xf2}, other_epoch);
    const CatalogEntry r3 = seedStalledCreating(op, store, "00/r3@cas@", UInt128{0xf3}, other_generation);
    const CatalogEntry mine = seedStalledCreating(op, store, "00/r4@cas@", UInt128{0xf4}, ours);

    const uint64_t before = ownCancels();
    store->cancelIdleOwnCreatingRows();
    EXPECT_EQ(catalogRow(op, store->layout(), r1.ns), std::optional<CatalogEntry>{r1});
    EXPECT_EQ(catalogRow(op, store->layout(), r2.ns), std::optional<CatalogEntry>{r2});
    EXPECT_EQ(catalogRow(op, store->layout(), r3.ns), std::optional<CatalogEntry>{r3});
    EXPECT_FALSE(catalogRow(op, store->layout(), mine.ns));
    EXPECT_EQ(ownCancels() - before, 1u);
}

TEST(CASOwnCreatingCancel, SixteenCancelsPerPass)
{
    auto backend = std::make_shared<OwnerCancelBackend>();
    PoolPtr store = openOwnerPool(backend);
    CasRequests requests = openRequestsForTest(backend);
    CasOperation op = requests.admit();
    for (size_t i = 0; i < 17; ++i)
        seedStalledCreating(op, store, fmt::format("00/r{:02}@cas@", i), UInt128{0x100 + i}, liveFenceOf(store));

    store->cancelIdleOwnCreatingRows();
    EXPECT_EQ(CasRefCatalog::read(op, store->layout()).catalog.entries.size(), 1u);
    store->cancelIdleOwnCreatingRows();
    EXPECT_TRUE(CasRefCatalog::read(op, store->layout()).catalog.entries.empty());
}

TEST(CASOwnCreatingCancel, AFailingRowDoesNotStarveTheOthers)
{
    auto backend = std::make_shared<OwnerCancelBackend>();
    PoolPtr store = openOwnerPool(backend);
    CasRequests requests = openRequestsForTest(backend);
    CasOperation op = requests.admit();
    const CatalogEntry r0 = seedStalledCreating(op, store, "00/r0@cas@", UInt128{0xa0}, liveFenceOf(store));
    const CatalogEntry r1 = seedStalledCreating(op, store, "00/r1@cas@", UInt128{0xa1}, liveFenceOf(store));
    backend->failCatalogCasWhen([&](const String & bytes) { return catalogBodyLacks(bytes, r0.ns); });

    EXPECT_ANY_THROW(store->cancelIdleOwnCreatingRows());
    EXPECT_TRUE(catalogRow(op, store->layout(), r0.ns));
    EXPECT_TRUE(catalogRow(op, store->layout(), r1.ns));

    EXPECT_ANY_THROW(store->cancelIdleOwnCreatingRows()) << "the second pass reaches `r0` again after `r1`";
    EXPECT_TRUE(catalogRow(op, store->layout(), r0.ns));
    EXPECT_FALSE(catalogRow(op, store->layout(), r1.ns)) << "the row after the failing one is cancelled";
}

namespace
{

void deleteCatalog(CasOperation & op, const Layout & layout)
{
    const std::optional<Object> catalog = op.read(layout.refCatalogKey(), Retry::standard());
    ASSERT_TRUE(catalog);
    ASSERT_EQ(op.remove(layout.refCatalogKey(), catalog->etag, Retry::standard()), Removal::Removed);
}

size_t countOf(const String & text, const String & needle)
{
    size_t n = 0;
    for (size_t at = text.find(needle); at != String::npos; at = text.find(needle, at + 1))
        ++n;
    return n;
}

const String kAbsentLine = "mandatory catalog absent; skipping owner cleanup";

}

TEST(CASOwnCreatingCancel, AnAbsentCatalogIsReportedWithoutThrowing)
{
    auto backend = std::make_shared<OwnerCancelBackend>();
    PoolPtr store = openOwnerPool(backend);
    CasRequests requests = openRequestsForTest(backend);
    CasOperation op = requests.admit();
    seedStalledCreating(op, store, "00/a@cas@", UInt128{0xb0}, liveFenceOf(store));
    deleteCatalog(op, store->layout());

    ScopedCasGcLogCapture log("CasRefLedger", "information");
    backend->resetCatalogCounts();
    EXPECT_NO_THROW(store->cancelIdleOwnCreatingRows());
    EXPECT_EQ(backend->catalogCasBegins(), 0u);
    EXPECT_EQ(countOf(log.captured(), kAbsentLine), 1u) << log.captured();
}

TEST(CASOwnCreatingCancel, ACatalogDeletedBeforeTheColdCancelDecidesIsReportedWithoutThrowing)
{
    auto backend = std::make_shared<OwnerCancelBackend>();
    PoolPtr store = openOwnerPool(backend);
    CasRequests requests = openRequestsForTest(backend);
    CasOperation op = requests.admit();
    seedStalledCreating(op, store, "00/a@cas@", UInt128{0xb0}, liveFenceOf(store));
    ASSERT_EQ(store->mountRequests().admit().hotKeys().cacheEntriesForTest(), 0u) << "the lane starts cold";

    backend->afterCatalogRead([&]
    {
        backend->afterCatalogRead({});
        deleteCatalog(op, store->layout());
    });
    ScopedCasGcLogCapture log("CasRefLedger", "information");
    backend->resetCatalogCounts();
    EXPECT_NO_THROW(store->cancelIdleOwnCreatingRows());
    EXPECT_EQ(backend->catalogCasBegins(), 0u) << "absence reaches the decision; nothing is written";
    EXPECT_EQ(countOf(log.captured(), kAbsentLine), 1u) << log.captured();
}

TEST(CASOwnCreatingCancel, ACatalogDeletedUnderTheWarmCancelIsReportedWithoutThrowing)
{
    auto backend = std::make_shared<OwnerCancelBackend>();
    PoolPtr store = openOwnerPool(backend);
    CasRequests requests = openRequestsForTest(backend);
    CasOperation op = requests.admit();
    seedStalledCreating(op, store, "00/a@cas@", UInt128{0xb0}, liveFenceOf(store));
    publishBirth(store, RootNamespace{"00/warm@cas@"}, "w");
    ASSERT_EQ(store->mountRequests().admit().hotKeys().cacheEntriesForTest(), 1u) << "the lane remembers the catalog";

    /// The scan reads the catalog, which is then deleted; the cancel writes from its cached body, is
    /// refused, and the read settling that refusal is the last request the lease admits.
    enum class Step { Scan, Deleting, Settle, Done };
    Step step = Step::Scan;
    backend->afterCatalogRead([&]
    {
        if (step == Step::Scan)
        {
            step = Step::Deleting;
            deleteCatalog(op, store->layout());
            step = Step::Settle;
        }
        else if (step == Step::Settle)
        {
            step = Step::Done;
            expireLease(store);
        }
    });
    ScopedCasGcLogCapture log("CasRefLedger", "information");
    backend->resetCatalogCounts();
    EXPECT_NO_THROW(store->cancelIdleOwnCreatingRows());
    backend->afterCatalogRead({});
    restoreLease(store);
    EXPECT_TRUE(step == Step::Done) << "the refused write was settled by a read";
    EXPECT_EQ(backend->catalogCasBegins(), 1u) << "one refused write from the cached body";
    EXPECT_FALSE(op.head(store->layout().refCatalogKey(), Retry::once())) << "nothing was committed";
    EXPECT_EQ(countOf(log.captured(), kAbsentLine), 1u) << log.captured();
}

/// Without the flag, absence stays the mandatory catalog's corruption verdict, which `DROP` and the GC
/// leader rely on.
TEST(CASOwnCreatingCancel, ACancelWithoutTheAbsenceFlagStillThrowsOnAnAbsentCatalog)
{
    auto backend = std::make_shared<OwnerCancelBackend>();
    PoolPtr store = openOwnerPool(backend);
    CasRequests requests = openRequestsForTest(backend);
    CasOperation op = requests.admit();
    const CatalogEntry row = seedStalledCreating(op, store, "00/a@cas@", UInt128{0xb0}, liveFenceOf(store));
    deleteCatalog(op, store->layout());

    CasOperation cancel_op = requests.admit();
    const auto grant = [](const CreatorFence &) { return true; };
    expectThrowsCode(DB::ErrorCodes::CORRUPTED_DATA, [&]
    {
        (void)CasRefCatalog::cancelStalledCreating(cancel_op, store->layout(), row, grant, [](const Retry &) {});
    });
    bool absent = false;
    EXPECT_EQ(CasRefCatalog::cancelStalledCreating(cancel_op, store->layout(), row, grant, [](const Retry &) {},
                                                   Retry::standard(), &absent),
              CasRefCatalog::StalledCreatingCancelOutcome::EntryChanged);
    EXPECT_TRUE(absent);
}

/// A `DROP` of a `Creating` name whose mandatory catalog vanishes between its read and the cancel reports
/// corruption. It does not treat the missing catalog as an already-absent namespace: only the owner pass does.
TEST(CASOwnCreatingCancel, DropOfACreatingNameReportsAMissingCatalogAsCorruption)
{
    auto backend = std::make_shared<OwnerCancelBackend>();
    PoolPtr store = openOwnerPool(backend);
    CasRequests requests = openRequestsForTest(backend);
    CasOperation op = requests.admit();
    const CatalogEntry row = seedStalledCreating(op, store, "00/a@cas@", UInt128{0xb0}, liveFenceOf(store));

    backend->afterCatalogRead([&]
    {
        backend->afterCatalogRead({});
        deleteCatalog(op, store->layout());
    });
    expectThrowsCode(DB::ErrorCodes::CORRUPTED_DATA, [&] { (void)store->dropNamespace(row.ns); });
}

/// A catalog read that fails backs off on the open plane's sleep, which teardown wakes
/// (`OpenPlaneSleepReturnsAtOnceOnceTeardownBegan`); the mount plane's sleep does not see teardown.
/// Teardown begins inside the pause, and the pass ends without the mount plane's pause ever returning.
TEST(CASOwnCreatingCancel, TeardownDuringARetryPauseEndsThePass)
{
    auto backend = std::make_shared<OwnerCancelBackend>();
    PoolPtr store = openOwnerPool(backend);
    CasRequests requests = openRequestsForTest(backend);
    CasOperation op = requests.admit();
    const CatalogEntry row = failBirthAfterStep1(store, op, RootNamespace{"00/a@cas@"});

    std::mutex mutex;
    std::condition_variable cv;
    bool mount_paused = false;
    bool release_mount_pause = false;
    bool pass_done = false;
    size_t open_pauses = 0;
    store->mountRequests().setSleepFnForTest([&](uint64_t)
    {
        store->beginTeardown();
        std::unique_lock lock(mutex);
        mount_paused = true;
        cv.notify_all();
        cv.wait_for(lock, std::chrono::minutes(1), [&] { return release_mount_pause; });
    });
    store->openRequests().setSleepFnForTest([&](uint64_t)
    {
        ++open_pauses;
        store->beginTeardown();
    });
    backend->afterCatalogRead([&]
    {
        backend->afterCatalogRead({});
        throw Poco::TimeoutException("OwnerCancelBackend: catalog read timed out");
    });
    backend->resetCatalogCounts();

    auto pass = std::async(std::launch::async, [&]
    {
        try
        {
            store->cancelIdleOwnCreatingRows();
        }
        catch (...) // NOLINT(bugprone-empty-catch)
        {
        }
        std::lock_guard lock(mutex);
        pass_done = true;
        cv.notify_all();
    });
    bool paused_on_mount_plane = false;
    {
        std::unique_lock lock(mutex);
        cv.wait_for(lock, std::chrono::minutes(1), [&] { return pass_done || mount_paused; });
        paused_on_mount_plane = mount_paused;
        release_mount_pause = true;
        cv.notify_all();
    }
    pass.get();

    EXPECT_FALSE(paused_on_mount_plane) << "the retry pause ran on the mount plane, which teardown does not wake";
    EXPECT_EQ(open_pauses, 1u) << "the failed catalog read backed off once on the open plane";
    EXPECT_EQ(backend->catalogGets(), 1u) << "teardown refused the reissue";
    EXPECT_EQ(backend->catalogCasBegins(), 0u);
    EXPECT_EQ(catalogRow(op, store->layout(), row.ns), std::optional<CatalogEntry>{row});
}

/// ===================== THE SCHEDULER RUNS THE OWNER PASS =====================

namespace
{

class OwnerRoundSink
{
public:
    GcRoundLogger logger()
    {
        return [this](const GcRoundLogRecord & r)
        {
            std::lock_guard lock(mutex);
            records.push_back(r);
            cv.notify_all();
        };
    }

    std::vector<GcRoundLogRecord> all()
    {
        std::lock_guard lock(mutex);
        return records;
    }

    std::optional<GcRoundLogRecord> waitForFinish()
    {
        std::unique_lock lock(mutex);
        const auto finish = [&]() -> const GcRoundLogRecord *
        {
            for (const GcRoundLogRecord & r : records)
                if (r.event_type == GcRoundLogRecord::EventType::Finish)
                    return &r;
            return nullptr;
        };
        if (!cv.wait_for(lock, std::chrono::minutes(1), [&] { return finish() != nullptr; }))
            return std::nullopt;
        return *finish();
    }

private:
    std::mutex mutex;
    std::condition_variable cv;
    std::vector<GcRoundLogRecord> records;
};

size_t countRecords(const std::vector<GcRoundLogRecord> & rows, GcRoundLogRecord::EventType type)
{
    return std::count_if(rows.begin(), rows.end(), [&](const GcRoundLogRecord & r) { return r.event_type == type; });
}

}

TEST(CASGCFrontierGate, ATeardownStopsThePassAndTheRound)
{
    auto backend = std::make_shared<OwnerCancelBackend>();
    PoolPtr store = openOwnerPool(backend);
    CasRequests requests = openRequestsForTest(backend);
    CasOperation op = requests.admit();
    const CatalogEntry row = failBirthAfterStep1(store, op, RootNamespace{"00/a@cas@"});

    OwnerRoundSink sink;
    CasGcScheduler scheduler(store, std::chrono::seconds(3600), "test::gc", "ca", sink.logger());
    std::atomic<bool> fired = false;
    backend->afterCatalogRead([&]
    {
        backend->afterCatalogRead({});
        store->beginTeardown();
        fired = true;
    });
    backend->resetCatalogCounts();
    scheduler.start();
    scheduler.requestRoundSoon();
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::minutes(1);
    while (!fired && std::chrono::steady_clock::now() < deadline)
        std::this_thread::yield();
    scheduler.stop();

    ASSERT_TRUE(fired) << "the tick never read the catalog";
    EXPECT_EQ(backend->catalogCasBegins(), 0u) << "no cancel after teardown began";
    EXPECT_EQ(countRecords(sink.all(), GcRoundLogRecord::EventType::Start), 0u)
        << "the tick that read the catalog for the owner pass starts no round once teardown began";
    EXPECT_EQ(catalogRow(op, store->layout(), row.ns), std::optional<CatalogEntry>{row});
}

TEST(CASGCFrontierGate, AFailedOwnerPassDoesNotSkipTheRound)
{
    auto backend = std::make_shared<OwnerCancelBackend>();
    PoolPtr store = openOwnerPool(backend);
    CasRequests requests = openRequestsForTest(backend);
    CasOperation op = requests.admit();
    const CatalogEntry row = failBirthAfterStep1(store, op, RootNamespace{"00/a@cas@"});
    backend->failCatalogCasWhen([&](const String & bytes) { return catalogBodyLacks(bytes, row.ns); });

    OwnerRoundSink sink;
    CasGcScheduler scheduler(store, std::chrono::seconds(3600), "test::gc", "ca", sink.logger());
    scheduler.start();
    scheduler.requestRoundSoon();
    const std::optional<GcRoundLogRecord> finish = sink.waitForFinish();
    scheduler.stop();

    ASSERT_TRUE(finish) << "the round after a failed owner pass did not run";
    EXPECT_EQ(countRecords(sink.all(), GcRoundLogRecord::EventType::Start), 1u);
    EXPECT_GE(backend->catalogCasBegins(), 1u) << "the owner pass tried the cancel";
    EXPECT_TRUE(catalogRow(op, store->layout(), row.ns));
}

TEST(CASGCFrontierGate, AFollowerRunsThePass)
{
    auto backend = std::make_shared<OwnerCancelBackend>();
    PoolPtr leader = openOwnerPool(backend, "leader");
    Gc leader_gc(leader, kGc);
    leader_gc.runRegularRound();
    PoolPtr follower = openOwnerPool(backend, "follower");
    CasRequests requests = openRequestsForTest(backend);
    CasOperation op = requests.admit();
    const CatalogEntry row = failBirthAfterStep1(follower, op, RootNamespace{"00/a@cas@"});

    OwnerRoundSink sink;
    CasGcScheduler scheduler(follower, std::chrono::seconds(3600), "test::gc", "ca", sink.logger());
    const uint64_t before = ownCancels();
    scheduler.start();
    scheduler.requestRoundSoon();
    const std::optional<GcRoundLogRecord> finish = sink.waitForFinish();
    scheduler.stop();

    ASSERT_TRUE(finish);
    EXPECT_EQ(finish->outcome, GcRoundLogRecord::Outcome::NotALeader);
    EXPECT_EQ(finish->trigger, GcRoundLogRecord::Trigger::Scheduled);
    EXPECT_EQ(ownCancels() - before, 1u);
    EXPECT_FALSE(catalogRow(op, follower->layout(), row.ns));
}

/// `FORGET` publishes its intent and trips the mount fence while the owner pass is inside its catalog read:
/// the tick starts no round on the pool being forgotten, and `FORGET` joins the scheduler.
TEST(CASGCFrontierGate, AForgetDuringThePassStartsNoRound)
{
    auto backend = std::make_shared<OwnerCancelBackend>();
    PoolPtr store = openOwnerPool(backend);
    CasRequests requests = openRequestsForTest(backend);
    CasOperation op = requests.admit();
    failBirthAfterStep1(store, op, RootNamespace{"00/a@cas@"});

    auto sink = std::make_shared<OwnerRoundSink>();
    auto scheduler = std::make_shared<CasGcScheduler>(store, std::chrono::seconds(3600), "test::gc", "ca",
        [sink](const GcRoundLogRecord & r) { sink->logger()(r); });
    const uint64_t generation = store->fenceGeneration();
    const auto forget_fenced = [store, generation] { return store->vanishedIntentPublished() && store->fenceGeneration() != generation; };
    auto parked = std::make_shared<std::atomic<bool>>(false);
    auto fenced = std::make_shared<std::atomic<bool>>(false);
    backend->afterCatalogRead([backend_ptr = backend.get(), forget_fenced, parked, fenced]
    {
        backend_ptr->afterCatalogRead({});
        *parked = true;
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::minutes(1);
        while (!forget_fenced() && std::chrono::steady_clock::now() < deadline)
            std::this_thread::yield();
        *fenced = forget_fenced();
    });
    backend->resetCatalogCounts();
    scheduler->start();
    scheduler->requestRoundSoon();
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::minutes(1);
    while (!*parked && std::chrono::steady_clock::now() < deadline)
        std::this_thread::yield();
    ASSERT_TRUE(*parked) << "the tick never read the catalog";

    /// A plain thread, not `std::async`: a deadlocked `FORGET` must fail the test in bounded time, and an
    /// `std::async` future would block in its destructor on the very call that hangs.
    auto forgot = std::make_shared<std::promise<void>>();
    std::future<void> forgot_done = forgot->get_future();
    std::thread forgetter([store, scheduler, forgot]
    {
        try
        {
            store->forgetDisk([&] { scheduler->stop(); }, "test decommission");
            forgot->set_value();
        }
        catch (...)
        {
            forgot->set_exception(std::current_exception());
        }
    });
    if (forgot_done.wait_for(std::chrono::minutes(1)) != std::future_status::ready)
    {
        /// Exit at once: the blocked scheduler worker would also hang the exit, which joins global pool threads.
        ADD_FAILURE() << "FORGET did not join the scheduler";
        std::fflush(stdout);
        std::_Exit(1);
    }
    forgetter.join();
    forgot_done.get();

    EXPECT_TRUE(*fenced) << "the pass resumed before FORGET published its intent and tripped the fence";
    EXPECT_EQ(countRecords(sink->all(), GcRoundLogRecord::EventType::Start), 0u)
        << "the tick started a round on a pool whose FORGET was in progress";
    EXPECT_EQ(backend->catalogCasBegins(), 0u) << "the tripped fence refused the cancel";
    EXPECT_TRUE(store->isVanished());
}

/// The pool is replaced while the owner pass is inside its catalog read: the tick starts no round, which
/// would address the replacement pool's `gc/state`.
TEST(CASGCFrontierGate, APoolReplacedDuringThePassStartsNoRound)
{
    auto backend = std::make_shared<OwnerCancelBackend>();
    PoolPtr store = openOwnerPool(backend);
    CasRequests requests = openRequestsForTest(backend);
    CasOperation op = requests.admit();
    failBirthAfterStep1(store, op, RootNamespace{"00/a@cas@"});

    OwnerRoundSink sink;
    CasGcScheduler scheduler(store, std::chrono::seconds(3600), "test::gc", "ca", sink.logger());
    std::atomic<bool> fired = false;
    backend->afterCatalogRead([&]
    {
        backend->afterCatalogRead({});
        store->setLifecycleForTest(PoolLifecycle::VanishedReplaced);
        fired = true;
    });
    backend->resetCatalogCounts();
    scheduler.start();
    scheduler.requestRoundSoon();
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::minutes(1);
    while (!fired && std::chrono::steady_clock::now() < deadline)
        std::this_thread::yield();
    /// `stop` joins the tick; it does not cut a round already past the checks.
    scheduler.stop();

    ASSERT_TRUE(fired) << "the tick never read the catalog";
    EXPECT_EQ(countRecords(sink.all(), GcRoundLogRecord::EventType::Start), 0u)
        << "the tick started a round on a replaced pool";
}

/// A stopped scheduler performs no owner pass even when a round is requested; a restarted one does.
/// The stopped half is a characterisation: it also passes on code that never ran the pass.
TEST(CASGCFrontierGate, AStoppedSchedulerRunsNoPassAndARestartedOneDoes)
{
    auto backend = std::make_shared<OwnerCancelBackend>();
    PoolPtr store = openOwnerPool(backend);
    CasRequests requests = openRequestsForTest(backend);
    CasOperation op = requests.admit();

    OwnerRoundSink sink;
    CasGcScheduler scheduler(store, std::chrono::seconds(3600), "test::gc", "ca", sink.logger());
    scheduler.start();
    scheduler.stop();
    const CatalogEntry row = failBirthAfterStep1(store, op, RootNamespace{"00/a@cas@"});
    backend->resetCatalogCounts();
    scheduler.requestRoundSoon();
    EXPECT_EQ(backend->catalogGets() + backend->catalogCasBegins(), 0u) << "a stopped scheduler runs no pass";
    EXPECT_EQ(catalogRow(op, store->layout(), row.ns), std::optional<CatalogEntry>{row});

    scheduler.start();
    scheduler.requestRoundSoon();
    const std::optional<GcRoundLogRecord> finish = sink.waitForFinish();
    scheduler.stop();
    ASSERT_TRUE(finish) << "the restarted scheduler did not tick";
    EXPECT_FALSE(catalogRow(op, store->layout(), row.ns)) << "the restarted scheduler runs the pass";
}

TEST(CASGCFrontierGate, AnOwnerRemountHandsTheRowToTheLeader)
{
    auto backend = std::make_shared<OwnerCancelBackend>();
    CasRequests requests = openRequestsForTest(backend);
    CasOperation op = requests.admit();
    CatalogEntry row;
    uint64_t first_epoch = 0;
    {
        PoolPtr first = openOwnerPool(backend);
        Gc bootstrap(first, kGc);
        runRoundCapturingGate(first, bootstrap, UniversePolicy::Authoritative);
        first_epoch = first->writerEpoch();
        row = failBirthAfterStep1(first, op, RootNamespace{"00/a@cas@"});
    }
    PoolPtr store = openOwnerPool(backend);
    ASSERT_GT(store->writerEpoch(), first_epoch);
    ASSERT_EQ(catalogRow(op, store->layout(), row.ns), std::optional<CatalogEntry>{row});

    const uint64_t own_before = ownCancels();
    const uint64_t leader_before = ProfileEvents::global_counters[ProfileEvents::CASGCStalledCreatingCancelled];
    backend->resetCatalogCounts();
    store->cancelIdleOwnCreatingRows();
    EXPECT_EQ(backend->catalogGets(), 1u);
    EXPECT_EQ(backend->catalogCasBegins(), 0u) << "a row of an earlier epoch is not the owner's";
    EXPECT_EQ(catalogRow(op, store->layout(), row.ns), std::optional<CatalogEntry>{row});

    Gc gc(store, kGc);
    const GateVerdict round = runRoundCapturingGate(store, gc, UniversePolicy::Authoritative);
    EXPECT_EQ(round.drain_cancelled, 1u);
    EXPECT_FALSE(catalogRow(op, store->layout(), row.ns));
    EXPECT_EQ(ProfileEvents::global_counters[ProfileEvents::CASGCStalledCreatingCancelled] - leader_before, 1u);
    EXPECT_EQ(ownCancels() - own_before, 0u);
}

TEST(CASGCFrontierGate, ACreatingOnlyCatalogRegainsAProvenFrontier)
{
    auto backend = std::make_shared<OwnerCancelBackend>();
    PoolPtr store = openOwnerPool(backend);
    CasRequests requests = openRequestsForTest(backend);
    CasOperation op = requests.admit();
    failBirthAfterStep1(store, op, RootNamespace{"00/a@cas@"});
    ASSERT_EQ(CasRefCatalog::read(op, store->layout()).catalog.entries.size(), 1u);

    Gc gc(store, kGc);
    const GateVerdict before = runRoundCapturingGate(store, gc, UniversePolicy::Authoritative);
    ASSERT_TRUE(before.saw_fold);
    EXPECT_TRUE(before.suppress_destructive) << "a Creating-only catalog suppresses destructive work";
    EXPECT_FALSE(before.catalog_proved_empty);

    store->cancelIdleOwnCreatingRows();
    EXPECT_TRUE(CasRefCatalog::read(op, store->layout()).catalog.entries.empty());
    const GateVerdict after = runRoundCapturingGate(store, gc, UniversePolicy::Authoritative);
    ASSERT_TRUE(after.saw_fold);
    EXPECT_TRUE(after.catalog_proved_empty);
    EXPECT_TRUE(after.frontier_complete);
    EXPECT_FALSE(after.suppress_destructive);
}
