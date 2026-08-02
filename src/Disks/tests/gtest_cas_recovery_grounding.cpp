#include <gtest/gtest.h>

#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Formats/CasRefCatalogFormat.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Formats/CasRefCkptFormat.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Formats/CasFormat.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Backend/CasInMemoryBackend.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasRefCkpt.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasRefCatalog.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasRefProtocol.h>
#include <Disks/tests/cas_test_helpers.h>
#include <Common/Exception.h>
#include <algorithm>
#include <functional>
#include <limits>
#include <map>

namespace DB::ErrorCodes
{
extern const int CORRUPTED_DATA;
extern const int INVALID_STATE;
}

using namespace DB::Cas;

namespace
{

using DB::Cas::tests::CountingBackend;
using DB::Cas::tests::casAdmitEntry;
using DB::Cas::tests::minimalLiveSnapshot;
using DB::Cas::tests::namespaceBirthOp;
using DB::Cas::tests::publishCommittedOps;
using DB::Cas::tests::seedPoolMetaForRestart;
using DB::Cas::tests::writeRefLogTxnRaw;
using DB::Cas::tests::writeRefSnapshotRaw;

enum class ListingMode : uint8_t
{
    Full,
    Empty,
    Partial,
    Reordered,
    HintAboveFrontier,
};

class RecoveryListingBackend : public CountingBackend
{
public:
    explicit RecoveryListingBackend(ListingMode mode_) : mode(mode_) { seedPoolMetaForRestart(*this); }

    size_t list_calls = 0;

    ListPage list(const String & prefix, const String & cursor, size_t limit) override
    {
        ++list_calls;
        ListPage page = CountingBackend::list(prefix, cursor, limit);
        if (mode == ListingMode::Empty)
            page.keys.clear();
        else if (mode == ListingMode::Partial)
        {
            page.keys.erase(std::remove_if(page.keys.begin(), page.keys.end(), [](const ListedKey & key)
            {
                return key.key.find("/_log/") != String::npos;
            }), page.keys.end());
        }
        else if (mode == ListingMode::Reordered)
            std::reverse(page.keys.begin(), page.keys.end());
        else if (mode == ListingMode::HintAboveFrontier)
            page.keys.push_back(ListedKey{
                .key = prefix + "_snap/" + renderRefTxnId({9, 1}) + String(storedSuffix(FormatId::RefSnapshot)),
                .token = std::nullopt});
        return page;
    }

private:
    ListingMode mode;
};

RefLogTxn txn(const RootNamespace & ns, RefTxnId id, std::vector<RefOp> ops,
              std::optional<RefTxnId> previous_seal = std::nullopt)
{
    return RefLogTxn{.ns = ns.string(), .txn_id = id, .ops = std::move(ops), .prev_epoch_seal = previous_seal};
}

std::map<String, ManifestRef> committedOf(const RefTableState & state)
{
    std::map<String, ManifestRef> result;
    for (const auto [name, row] : state.getCommitted())
        result.emplace(name, row.manifest_ref);
    return result;
}

void seedAuthoritativeStream(Backend & backend, const Layout & layout, const RootNamespace & ns,
                             RefTxnId committed_through, bool include_f_plus_one = false)
{
    const ManifestRef first{1, 1, 1};
    std::vector<RefOp> birth{namespaceBirthOp()};
    const auto first_publish = publishCommittedOps("a", first);
    birth.insert(birth.end(), first_publish.begin(), first_publish.end());
    const RefLogTxn first_txn = txn(ns, {1, 1}, std::move(birth));
    writeRefLogTxnRaw(backend, layout, first_txn);

    if (committed_through > RefTxnId{1, 1})
    {
        RefOp seal_op;
        seal_op.kind = RefOpKind::EpochSeal;
        writeRefLogTxnRaw(backend, layout, txn(ns, {1, 2}, {std::move(seal_op)}));
        const ManifestRef second{2, 1, 1};
        writeRefLogTxnRaw(backend, layout,
            txn(ns, {2, 1}, publishCommittedOps("b", second), RefTxnId{1, 2}));
    }
    if (include_f_plus_one)
    {
        const ManifestRef extra{1, 2, 1};
        writeRefLogTxnRaw(backend, layout, txn(ns, {1, 2}, publishCommittedOps("uncommitted", extra)));
    }

    RefTableState snapshot_state;
    applyRefLogTxn(snapshot_state, first_txn);
    writeRefSnapshotRaw(backend, layout, snapshotOf(snapshot_state, ns.string()));

    const NamespaceLifeId life = *CasRefCatalog::lifeIfCataloged(backend, layout, ns);
    const RefCkpt authority{
        .life_epoch = 1,
        .committed_through = committed_through,
        .checkpoint_snapshot_id = std::nullopt,
        .last_epoch_seal = committed_through.writer_epoch > 1
            ? std::optional<RefTxnId>{RefTxnId{1, 2}} : std::nullopt};
    backend.putIfAbsent(layout.refCkptKey(life), encodeRefCkpt(authority));
}

/// This is deliberately caller-side plumbing, not a convenience overload in `CasRefProtocol`: production
/// callers obtain `entry` from their frozen `RefPlan::catalogCut` and sample `_ckpt` in the same plan.
/// The API under test receives those exact values and performs no catalog or checkpoint resolution itself.
RecoveredRefTable recoverFromCurrentCatalogCut(Backend & backend, const Layout & layout, const RootNamespace & ns)
{
    const CasRefCatalog::Snapshot cut = CasRefCatalog::read(backend, layout);
    std::optional<CatalogEntry> entry;
    for (const CatalogEntry & candidate : cut.catalog.entries)
    {
        if (candidate.ns == ns)
        {
            entry = candidate;
            break;
        }
    }
    std::optional<RefCkpt> checkpoint;
    if (entry)
    {
        const NamespaceLifeId life = NamespaceLifeId::fromCatalogEntry(entry->ns, entry->incarnation);
        if (const std::optional<CkptSample> sample = readCkpt(backend, layout, life))
            checkpoint = sample->ckpt;
    }
    return recoverRefTableDetailedFromAuthority(backend, layout, entry, checkpoint);
}

CatalogEntry catalog(NsState state)
{
    return CatalogEntry{.ns = RootNamespace{"srv1/recovery_grounding"}, .state = state, .incarnation = 1};
}

RefCkpt ckpt(uint64_t life_epoch, std::optional<RefTxnId> committed_through,
             std::optional<RefTxnId> checkpoint_snapshot_id = std::nullopt,
             std::optional<RefTxnId> last_epoch_seal = std::nullopt)
{
    return RefCkpt{.life_epoch = life_epoch,
                   .committed_through = committed_through,
                   .checkpoint_snapshot_id = checkpoint_snapshot_id,
                   .last_epoch_seal = last_epoch_seal};
}

void expectCode(const std::function<void()> & f, int code)
{
    try
    {
        f();
        FAIL() << "expected DB::Exception";
    }
    catch (const DB::Exception & e)
    {
        EXPECT_EQ(e.code(), code);
    }
}

TEST(CasRecoveryGrounding, CreatingAndAbsentCatalogEntriesAreNotRecovered)
{
    expectCode([&] { chooseRecoveryGrounding(catalog(NsState::Creating), ckpt(7, RefTxnId{7, 3}), std::nullopt); },
               DB::ErrorCodes::INVALID_STATE);
    expectCode([&] { chooseRecoveryGrounding(std::nullopt, ckpt(7, RefTxnId{7, 3}), std::nullopt); },
               DB::ErrorCodes::INVALID_STATE);
}

TEST(CasRecoveryGrounding, LiveAndRemovingRequireCheckpointAndLifeEpoch)
{
    expectCode([&] { chooseRecoveryGrounding(catalog(NsState::Live), std::nullopt, std::nullopt); },
               DB::ErrorCodes::CORRUPTED_DATA);
    expectCode([&] { chooseRecoveryGrounding(catalog(NsState::Removing), RefCkpt{}, std::nullopt); },
               DB::ErrorCodes::CORRUPTED_DATA);
}

TEST(CasRecoveryGrounding, MissingFrontierMeansNoCommittedTransaction)
{
    const RecoveryGrounding grounding = chooseRecoveryGrounding(catalog(NsState::Live), ckpt(7, std::nullopt), RefTxnId{7, 2});
    EXPECT_FALSE(grounding.base);
    EXPECT_FALSE(grounding.committed_through);
    EXPECT_TRUE(grounding.ignored_hinted_snapshot_above_frontier);
}

TEST(CasRecoveryGrounding, ChoosesGreatestEligibleBaseAndArithmeticWalkStart)
{
    const RecoveryGrounding grounding = chooseRecoveryGrounding(
        catalog(NsState::Live), ckpt(7, RefTxnId{7, 8}, RefTxnId{7, 4}), RefTxnId{7, 6});
    EXPECT_EQ(grounding.base, (RefTxnId{7, 6}));
    EXPECT_EQ(grounding.walk_from, (RefTxnId{7, 7}));
    EXPECT_EQ(grounding.committed_through, (RefTxnId{7, 8}));
}

TEST(CasRecoveryGrounding, BaseAtFrontierStillStartsAtItsExactSuccessor)
{
    /// A writer recovery probes exactly this slot for its sole possible unfrontiered successor. The
    /// grounding contract must supply the arithmetic start even when the committed replay tail is empty.
    const RecoveryGrounding grounding = chooseRecoveryGrounding(
        catalog(NsState::Live), ckpt(7, RefTxnId{7, 8}, RefTxnId{7, 8}), std::nullopt);

    EXPECT_EQ(grounding.base, (RefTxnId{7, 8}));
    EXPECT_EQ(grounding.walk_from, (RefTxnId{7, 9}));
    EXPECT_EQ(grounding.committed_through, (RefTxnId{7, 8}));
}

TEST(CasRecoveryGrounding, IgnoresHintAboveFrontierAndRecordsDiagnostic)
{
    const RecoveryGrounding grounding = chooseRecoveryGrounding(
        catalog(NsState::Live), ckpt(7, RefTxnId{7, 5}, RefTxnId{7, 3}), RefTxnId{7, 6});
    EXPECT_EQ(grounding.base, (RefTxnId{7, 3}));
    EXPECT_TRUE(grounding.ignored_hinted_snapshot_above_frontier);
}

TEST(CasRecoveryGrounding, WalksFromLifeEpochWithoutBaseAndNeverFromHintedLog)
{
    const RecoveryGrounding grounding = chooseRecoveryGrounding(catalog(NsState::Removing), ckpt(9, RefTxnId{9, 3}), std::nullopt);
    EXPECT_EQ(grounding.walk_from, (RefTxnId{9, 1}));
}

TEST(CasRecoveryGrounding, RejectsBaseWithoutARepresentableSuccessor)
{
    expectCode([&]
    {
        chooseRecoveryGrounding(catalog(NsState::Live),
            ckpt(7, RefTxnId{8, 1}, RefTxnId{7, std::numeric_limits<uint64_t>::max()}, RefTxnId{8, 1}),
            std::nullopt);
    }, DB::ErrorCodes::CORRUPTED_DATA);
}

TEST(CasRecoveryGrounding, RejectsCheckpointFieldsAboveCommittedFrontier)
{
    expectCode([&]
    {
        chooseRecoveryGrounding(catalog(NsState::Live), ckpt(7, RefTxnId{7, 3}, RefTxnId{7, 4}), std::nullopt);
    }, DB::ErrorCodes::CORRUPTED_DATA);
    expectCode([&]
    {
        chooseRecoveryGrounding(catalog(NsState::Live), ckpt(7, RefTxnId{7, 3}, std::nullopt, RefTxnId{7, 4}), std::nullopt);
    }, DB::ErrorCodes::CORRUPTED_DATA);
}

TEST(CasRecoveryGrounding, RecoveryIsEquivalentUnderFullEmptyPartialAndReorderedList)
{
    struct Observation
    {
        std::map<String, ManifestRef> committed;
        RefTxnId greatest_applied;
        std::optional<RefTxnId> last_epoch_seal;
        RefTxnId next_id;
        uint64_t log_gets = 0;
        uint64_t snapshot_gets = 0;
    };

    std::vector<Observation> observations;
    for (const ListingMode mode : {ListingMode::Full, ListingMode::Empty, ListingMode::Partial, ListingMode::Reordered})
    {
        auto backend = std::make_shared<RecoveryListingBackend>(mode);
        const Layout layout("p");
        const RootNamespace ns{"srv1/list_equivalence"};
        const RefTxnId frontier{2, 1};
        seedAuthoritativeStream(*backend, layout, ns, frontier);
        const NamespaceLifeId life = *CasRefCatalog::lifeIfCataloged(*backend, layout, ns);
        backend->resetCounts();

        const RecoveredRefTable recovered = recoverFromCurrentCatalogCut(*backend, layout, ns);
        const uint64_t log_gets = backend->getCount(layout.refLogKey(life, {1, 1}))
            + backend->getCount(layout.refLogKey(life, {1, 2}))
            + backend->getCount(layout.refLogKey(life, {2, 1}));
        const uint64_t snapshot_gets = backend->getCount(layout.refSnapshotKey(life, {1, 1}));
        observations.push_back(Observation{
            .committed = committedOf(recovered.state),
            .greatest_applied = recovered.state.getGreatestApplied(),
            .last_epoch_seal = recovered.last_epoch_seal,
            .next_id = recovered.state.nextTxnId(/*live_epoch=*/3),
            .log_gets = log_gets,
            .snapshot_gets = snapshot_gets});
    }

    ASSERT_EQ(observations.size(), 4u);
    for (size_t i = 1; i < observations.size(); ++i)
    {
        EXPECT_EQ(observations[i].committed, observations[0].committed);
        EXPECT_EQ(observations[i].greatest_applied, observations[0].greatest_applied);
        EXPECT_EQ(observations[i].last_epoch_seal, observations[0].last_epoch_seal);
        EXPECT_EQ(observations[i].next_id, observations[0].next_id);
    }
    for (const Observation & observation : observations)
        EXPECT_EQ(observation.log_gets, 3u)
            << "a selected hint validates its matching log, while an unhinted replay reads the same exact frontier";
    EXPECT_NE(observations[0].snapshot_gets, observations[1].snapshot_gets)
        << "LIST must remain a useful snapshot-performance hint";
}

TEST(CasRecoveryGrounding, CatalogLifecycleAndCheckpointAreMandatoryForReadOnlyRecovery)
{
    const Layout layout("p");
    const RootNamespace ns{"srv1/mandatory_authority"};

    {
        auto backend = std::make_shared<RecoveryListingBackend>(ListingMode::Full);
        writeRefLogTxnRaw(*backend, layout, txn(ns, {1, 1}, {namespaceBirthOp()}));
        expectCode([&] { (void)recoverFromCurrentCatalogCut(*backend, layout, ns); }, DB::ErrorCodes::CORRUPTED_DATA);
    }
    {
        auto backend = std::make_shared<RecoveryListingBackend>(ListingMode::Full);
        CasRefCatalog::casAdmitEntry(
            *backend, layout, 1, CatalogEntry{.ns = ns, .state = NsState::Live, .incarnation = 8});
        expectCode([&] { (void)recoverFromCurrentCatalogCut(*backend, layout, ns); }, DB::ErrorCodes::CORRUPTED_DATA);
    }
    {
        auto backend = std::make_shared<RecoveryListingBackend>(ListingMode::Full);
        const CatalogEntry live{.ns = ns, .state = NsState::Live, .incarnation = 9};
        CasRefCatalog::casAdmitEntry(*backend, layout, 1, live);
        const NamespaceLifeId life = NamespaceLifeId::fromCatalogEntry(live.ns, live.incarnation);
        ASSERT_EQ(backend->putIfAbsent(layout.refCkptKey(life), "not a sealed checkpoint").outcome,
                  PutOutcome::Done);
        expectCode([&] { (void)recoverFromCurrentCatalogCut(*backend, layout, ns); }, DB::ErrorCodes::CORRUPTED_DATA);
    }
    {
        auto backend = std::make_shared<RecoveryListingBackend>(ListingMode::Full);
        CatalogEntry creating{.ns = ns, .state = NsState::Creating, .incarnation = 7,
            .creator = CreatorFence{"srv1", 1, 1}};
        CasRefCatalog::casAdmitEntry(*backend, layout, 1, creating);
        backend->putIfAbsent(layout.refCkptKey(NamespaceLifeId::fromCatalogEntry(creating.ns, creating.incarnation)),
            encodeRefCkpt(RefCkpt{.life_epoch = 1, .committed_through = RefTxnId{1, 1},
                                  .checkpoint_snapshot_id = std::nullopt, .last_epoch_seal = std::nullopt}));
        expectCode([&] { (void)recoverFromCurrentCatalogCut(*backend, layout, ns); }, DB::ErrorCodes::INVALID_STATE);
    }
    {
        auto backend = std::make_shared<RecoveryListingBackend>(ListingMode::Full);
        backend->putIfAbsent(layout.refCkptKey(NamespaceLifeId::stageATransition(ns)),
            encodeRefCkpt(RefCkpt{.life_epoch = 1, .committed_through = RefTxnId{1, 1},
                                  .checkpoint_snapshot_id = std::nullopt, .last_epoch_seal = std::nullopt}));
        expectCode([&] { (void)recoverFromCurrentCatalogCut(*backend, layout, ns); }, DB::ErrorCodes::INVALID_STATE);
    }
}

TEST(CasRecoveryGrounding, NonrecoverableAuthorityPerformsNoBackendRecoveryIo)
{
    const Layout layout("p");
    const RootNamespace ns{"srv1/nonrecoverable_authority"};
    const RefCkpt valid_ckpt{
        .life_epoch = 1,
        .committed_through = std::nullopt,
        .checkpoint_snapshot_id = std::nullopt,
        .last_epoch_seal = std::nullopt};

    {
        auto backend = std::make_shared<RecoveryListingBackend>(ListingMode::Full);
        backend->resetCounts();
        const CatalogEntry creating{
            .ns = ns, .state = NsState::Creating, .incarnation = 1, .creator = CreatorFence{"srv1", 1, 1}};
        expectCode(
            [&] { (void)recoverRefTableDetailedFromAuthority(*backend, layout, creating, valid_ckpt); },
            DB::ErrorCodes::INVALID_STATE);
        EXPECT_EQ(backend->list_calls, 0u);
        EXPECT_EQ(backend->getTotal(), 0u);
    }
    {
        auto backend = std::make_shared<RecoveryListingBackend>(ListingMode::Full);
        backend->resetCounts();
        const CatalogEntry live{.ns = ns, .state = NsState::Live, .incarnation = 2};
        expectCode(
            [&] { (void)recoverRefTableDetailedFromAuthority(*backend, layout, live, std::nullopt); },
            DB::ErrorCodes::CORRUPTED_DATA);
        EXPECT_EQ(backend->list_calls, 0u);
        EXPECT_EQ(backend->getTotal(), 0u);
    }
    {
        auto backend = std::make_shared<RecoveryListingBackend>(ListingMode::Full);
        backend->resetCounts();
        expectCode(
            [&] { (void)recoverRefTableDetailedFromAuthority(*backend, layout, std::nullopt, valid_ckpt); },
            DB::ErrorCodes::INVALID_STATE);
        EXPECT_EQ(backend->list_calls, 0u);
        EXPECT_EQ(backend->getTotal(), 0u);
    }
}

TEST(CasRecoveryGrounding, ReadOnlyRecoveryNeverAdoptsFPlusOne)
{
    auto backend = std::make_shared<RecoveryListingBackend>(ListingMode::Full);
    const Layout layout("p");
    const RootNamespace ns{"srv1/read_only_excludes_f_plus_one"};
    seedAuthoritativeStream(*backend, layout, ns, RefTxnId{1, 1}, /*include_f_plus_one=*/true);

    const RecoveredRefTable recovered = recoverFromCurrentCatalogCut(*backend, layout, ns);
    EXPECT_EQ(recovered.state.getGreatestApplied(), (RefTxnId{1, 1}));
    EXPECT_TRUE(recovered.state.getCommitted().contains("a"));
    EXPECT_FALSE(recovered.state.getCommitted().contains("uncommitted"));
}

TEST(CasRecoveryGrounding, ReportsListedSnapshotAboveExactFrontierWithoutUsingIt)
{
    auto backend = std::make_shared<RecoveryListingBackend>(ListingMode::HintAboveFrontier);
    const Layout layout("p");
    const RootNamespace ns{"srv1/hint_above_frontier"};
    seedAuthoritativeStream(*backend, layout, ns, RefTxnId{1, 1});

    const RecoveredRefTable recovered = recoverFromCurrentCatalogCut(*backend, layout, ns);
    EXPECT_TRUE(recovered.ignored_hinted_snapshot_above_frontier);
    EXPECT_EQ(recovered.state.getGreatestApplied(), (RefTxnId{1, 1}));
    EXPECT_TRUE(recovered.state.getCommitted().contains("a"));
}

/// A snapshot listed at an epoch seal is not a usable recovery cut. `LIST` is only a performance
/// hint, so the full listing must converge with an empty listing after exact reads prove that the
/// nominated snapshot's matching log is a seal.
TEST(CasRecoveryGrounding, ListedSnapshotAtEpochSealIsDiscardedAndDoesNotChangeRecovery)
{
    struct Observation
    {
        std::map<String, ManifestRef> committed;
        RefTxnId greatest_applied;
        std::optional<RefTxnId> last_epoch_seal;
        bool discarded_hinted_snapshot;
    };

    std::vector<Observation> observations;
    for (const ListingMode mode : {ListingMode::Full, ListingMode::Empty})
    {
        auto backend = std::make_shared<RecoveryListingBackend>(mode);
        const Layout layout("p");
        const RootNamespace ns{"srv1/hinted_snapshot_at_seal"};
        seedAuthoritativeStream(*backend, layout, ns, RefTxnId{2, 1});

        RefTableState state_through_seal;
        std::vector<RefOp> birth{namespaceBirthOp()};
        const auto first_publish = publishCommittedOps("a", ManifestRef{1, 1, 1});
        birth.insert(birth.end(), first_publish.begin(), first_publish.end());
        applyRefLogTxn(state_through_seal, txn(ns, {1, 1}, std::move(birth)));
        RefOp seal;
        seal.kind = RefOpKind::EpochSeal;
        applyRefLogTxn(state_through_seal, txn(ns, {1, 2}, {std::move(seal)}));
        writeRefSnapshotRaw(*backend, layout, snapshotOf(state_through_seal, ns.string()));

        const RecoveredRefTable recovered = recoverFromCurrentCatalogCut(*backend, layout, ns);
        observations.push_back(Observation{
            .committed = committedOf(recovered.state),
            .greatest_applied = recovered.state.getGreatestApplied(),
            .last_epoch_seal = recovered.last_epoch_seal,
            .discarded_hinted_snapshot = recovered.discarded_hinted_snapshot});
    }

    ASSERT_EQ(observations.size(), 2u);
    EXPECT_EQ(observations[0].committed, observations[1].committed);
    EXPECT_EQ(observations[0].greatest_applied, (RefTxnId{2, 1}));
    EXPECT_EQ(observations[1].greatest_applied, (RefTxnId{2, 1}));
    EXPECT_EQ(observations[0].last_epoch_seal, (RefTxnId{1, 2}));
    EXPECT_EQ(observations[1].last_epoch_seal, (RefTxnId{1, 2}));
    EXPECT_TRUE(observations[0].discarded_hinted_snapshot);
    EXPECT_FALSE(observations[1].discarded_hinted_snapshot);
}

/// `decodeRefTableSnapshot` accepts this wire-valid row set, but a single manifest under a committed
/// and a precommit owner violates the table's semantic ownership invariant in `stateFromSnapshot`.
/// A LIST-only snapshot is merely a performance candidate, so full and empty listings must converge
/// after that validation rejects the candidate.
TEST(CasRecoveryGrounding, SemanticallyMalformedHintedSnapshotIsDiscardedAndDoesNotChangeRecovery)
{
    struct Observation
    {
        std::map<String, ManifestRef> committed;
        RefTxnId greatest_applied;
        bool discarded_hinted_snapshot;
    };

    std::vector<Observation> observations;
    for (const ListingMode mode : {ListingMode::Full, ListingMode::Empty})
    {
        auto backend = std::make_shared<RecoveryListingBackend>(mode);
        const Layout layout("p");
        const RootNamespace ns{"srv1/semantically_malformed_hint"};
        const ManifestRef manifest{1, 1, 1};
        std::vector<RefOp> ops{namespaceBirthOp()};
        const auto publish = publishCommittedOps("committed", manifest);
        ops.insert(ops.end(), publish.begin(), publish.end());
        writeRefLogTxnRaw(*backend, layout, txn(ns, {1, 1}, std::move(ops)));

        RefTableSnapshot malformed = minimalLiveSnapshot(
            ns.string(), {1, 1}, {DB::Cas::tests::committedRow("committed", manifest)});
        malformed.precommits.push_back(RefOwnerBinding{RefOwnerKind::Precommit, "precommit", manifest});
        writeRefSnapshotRaw(*backend, layout, malformed);

        const NamespaceLifeId life = *CasRefCatalog::lifeIfCataloged(*backend, layout, ns);
        ASSERT_EQ(backend->putIfAbsent(layout.refCkptKey(life), encodeRefCkpt(RefCkpt{
            .life_epoch = 1,
            .committed_through = RefTxnId{1, 1},
            .checkpoint_snapshot_id = std::nullopt,
            .last_epoch_seal = std::nullopt})).outcome, PutOutcome::Done);

        const RecoveredRefTable recovered = recoverFromCurrentCatalogCut(*backend, layout, ns);
        observations.push_back(Observation{
            .committed = committedOf(recovered.state),
            .greatest_applied = recovered.state.getGreatestApplied(),
            .discarded_hinted_snapshot = recovered.discarded_hinted_snapshot});
    }

    ASSERT_EQ(observations.size(), 2u);
    EXPECT_EQ(observations[0].committed, observations[1].committed);
    EXPECT_EQ(observations[0].greatest_applied, observations[1].greatest_applied);
    EXPECT_EQ(observations[0].committed, (std::map<String, ManifestRef>{{"committed", {1, 1, 1}}}));
    EXPECT_EQ(observations[0].greatest_applied, (RefTxnId{1, 1}));
    EXPECT_TRUE(observations[0].discarded_hinted_snapshot);
    EXPECT_FALSE(observations[1].discarded_hinted_snapshot);
}

/// A checkpoint-named snapshot is immutable lifecycle authority, not a list candidate. Its exact GET
/// and semantic decode must therefore fail closed rather than falling back to replaying the same log.
TEST(CasRecoveryGrounding, SemanticallyMalformedCheckpointSnapshotIsCorruptionAfterExactRead)
{
    auto backend = std::make_shared<RecoveryListingBackend>(ListingMode::Empty);
    const Layout layout("p");
    const RootNamespace ns{"srv1/semantically_malformed_checkpoint"};
    const ManifestRef manifest{1, 1, 1};
    std::vector<RefOp> ops{namespaceBirthOp()};
    const auto publish = publishCommittedOps("committed", manifest);
    ops.insert(ops.end(), publish.begin(), publish.end());
    writeRefLogTxnRaw(*backend, layout, txn(ns, {1, 1}, std::move(ops)));

    RefTableSnapshot malformed = minimalLiveSnapshot(
        ns.string(), {1, 1}, {DB::Cas::tests::committedRow("committed", manifest)});
    malformed.precommits.push_back(RefOwnerBinding{RefOwnerKind::Precommit, "precommit", manifest});
    writeRefSnapshotRaw(*backend, layout, malformed);

    const NamespaceLifeId life = *CasRefCatalog::lifeIfCataloged(*backend, layout, ns);
    const String snapshot_key = layout.refSnapshotKey(life, {1, 1});
    ASSERT_EQ(backend->putIfAbsent(layout.refCkptKey(life), encodeRefCkpt(RefCkpt{
        .life_epoch = 1,
        .committed_through = RefTxnId{1, 1},
        .checkpoint_snapshot_id = RefTxnId{1, 1},
        .last_epoch_seal = std::nullopt})).outcome, PutOutcome::Done);

    backend->resetCounts();
    try
    {
        (void)recoverFromCurrentCatalogCut(*backend, layout, ns);
        FAIL() << "expected checkpoint-named malformed snapshot to fail closed";
    }
    catch (const DB::Exception & e)
    {
        EXPECT_EQ(e.code(), DB::ErrorCodes::CORRUPTED_DATA);
        EXPECT_NE(String(e.message()).find("stateFromSnapshot"), String::npos);
    }
    EXPECT_EQ(backend->getCount(snapshot_key), 1u)
        << "the corruption must come from the checkpoint snapshot's exact decode";
}

TEST(CasRecoveryGrounding, CheckpointSnapshotAtEpochSealIsCorruption)
{
    auto backend = std::make_shared<RecoveryListingBackend>(ListingMode::Full);
    const Layout layout("p");
    const RootNamespace ns{"srv1/checkpoint_base_seal"};
    seedAuthoritativeStream(*backend, layout, ns, RefTxnId{2, 1});
    const NamespaceLifeId life = *CasRefCatalog::lifeIfCataloged(*backend, layout, ns);

    RefTableState through_seal;
    std::vector<RefOp> birth{namespaceBirthOp()};
    const auto first_publish = publishCommittedOps("a", ManifestRef{1, 1, 1});
    birth.insert(birth.end(), first_publish.begin(), first_publish.end());
    applyRefLogTxn(through_seal, txn(ns, {1, 1}, std::move(birth)));
    RefOp seal;
    seal.kind = RefOpKind::EpochSeal;
    applyRefLogTxn(through_seal, txn(ns, {1, 2}, {std::move(seal)}));
    writeRefSnapshotRaw(*backend, layout, snapshotOf(through_seal, ns.string()));

    const CkptSample before = *readCkpt(*backend, layout, life);
    const RefCkpt with_sealed_base{
        .life_epoch = 1,
        .committed_through = RefTxnId{2, 1},
        .checkpoint_snapshot_id = RefTxnId{1, 2},
        .last_epoch_seal = RefTxnId{1, 2}};
    ASSERT_EQ(backend->casPut(layout.refCkptKey(life), encodeRefCkpt(with_sealed_base), before.token).outcome,
              CasOutcome::Committed);

    expectCode([&] { (void)recoverFromCurrentCatalogCut(*backend, layout, ns); }, DB::ErrorCodes::CORRUPTED_DATA);
}

TEST(CasRecoveryGrounding, OlderCheckpointSnapshotAtSealIsCorruption)
{
    auto backend = std::make_shared<RecoveryListingBackend>(ListingMode::Full);
    const Layout layout("p");
    const RootNamespace ns{"srv1/older_checkpoint_base_seal"};
    seedAuthoritativeStream(*backend, layout, ns, RefTxnId{2, 1});
    const NamespaceLifeId life = *CasRefCatalog::lifeIfCataloged(*backend, layout, ns);

    RefOp second_seal;
    second_seal.kind = RefOpKind::EpochSeal;
    writeRefLogTxnRaw(*backend, layout, txn(ns, {2, 2}, {std::move(second_seal)}));
    writeRefLogTxnRaw(*backend, layout,
        txn(ns, {3, 1}, publishCommittedOps("c", ManifestRef{3, 1, 1}), RefTxnId{2, 2}));

    RefTableState through_first_seal;
    std::vector<RefOp> birth{namespaceBirthOp()};
    const auto first_publish = publishCommittedOps("a", ManifestRef{1, 1, 1});
    birth.insert(birth.end(), first_publish.begin(), first_publish.end());
    applyRefLogTxn(through_first_seal, txn(ns, {1, 1}, std::move(birth)));
    RefOp first_seal;
    first_seal.kind = RefOpKind::EpochSeal;
    applyRefLogTxn(through_first_seal, txn(ns, {1, 2}, {std::move(first_seal)}));
    writeRefSnapshotRaw(*backend, layout, snapshotOf(through_first_seal, ns.string()));

    const CkptSample before = *readCkpt(*backend, layout, life);
    const RefCkpt with_old_sealed_base{
        .life_epoch = 1,
        .committed_through = RefTxnId{3, 1},
        .checkpoint_snapshot_id = RefTxnId{1, 2},
        .last_epoch_seal = RefTxnId{2, 2}};
    ASSERT_EQ(backend->casPut(layout.refCkptKey(life), encodeRefCkpt(with_old_sealed_base), before.token).outcome,
              CasOutcome::Committed);

    expectCode([&] { (void)recoverFromCurrentCatalogCut(*backend, layout, ns); }, DB::ErrorCodes::CORRUPTED_DATA);
}

TEST(CasRecoveryGrounding, TerminalGapBelowFrontierIsCorruptionNotARebirth)
{
    auto backend = std::make_shared<RecoveryListingBackend>(ListingMode::Full);
    const Layout layout("p");
    const RootNamespace ns{"srv1/terminal_gap"};
    const RefLogTxn birth = txn(ns, {1, 1}, {namespaceBirthOp()});
    writeRefLogTxnRaw(*backend, layout, birth);
    RefOp remove;
    remove.kind = RefOpKind::RemoveNamespace;
    writeRefLogTxnRaw(*backend, layout, txn(ns, {1, 2}, {std::move(remove)}));
    writeRefLogTxnRaw(*backend, layout, txn(ns, {2, 1}, {namespaceBirthOp()}));

    const NamespaceLifeId life = *CasRefCatalog::lifeIfCataloged(*backend, layout, ns);
    ASSERT_EQ(backend->putIfAbsent(layout.refCkptKey(life), encodeRefCkpt(RefCkpt{
        .life_epoch = 1,
        .committed_through = RefTxnId{2, 1},
        .checkpoint_snapshot_id = std::nullopt,
        .last_epoch_seal = std::nullopt})).outcome, PutOutcome::Done);

    expectCode([&] { (void)recoverFromCurrentCatalogCut(*backend, layout, ns); }, DB::ErrorCodes::CORRUPTED_DATA);
}

}
