#include <gtest/gtest.h>

#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasPartWriteTxn.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasPool.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Backend/CasInMemoryBackend.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Formats/CasServerRootFormats.h>
#include <Disks/tests/cas_test_helpers.h>

#include <chrono>
#include <limits>
#include <memory>
#include <vector>

namespace DB::ErrorCodes
{
extern const int NETWORK_ERROR;
}

using namespace DB::Cas;

namespace
{

PoolConfig singleAttemptConfig()
{
    PoolConfig config{
        .pool_prefix = "p",
        .server_root_id = "test",
        .background_watermark = false,
    };
    config.cas_request_budget.max_attempts = 1;
    config.cas_request_budget.attempt_timeout_ms = 100;
    config.cas_request_budget.operation_deadline_ms = 5000;
    config.cas_request_budget.lease_safety_margin_ms = 100;
    return config;
}

PoolPtr openSingleAttemptPool(const BackendPtr & backend)
{
    DB::Cas::tests::seedPoolMetaForRestart(*backend);
    return Pool::open(backend, singleAttemptConfig());
}

PartWriteTxnPtr stageEmptyManifest(
    const PoolPtr & store, const RootNamespace & ns, const String & ref_name, ManifestId & id)
{
    PartWriteInfo info;
    info.intended_namespace = ns;
    info.intended_ref = ns.string() + "/" + ref_name;
    auto build = store->beginPartWrite(std::move(info));
    id = build->stageManifest({});
    return build;
}

}

/// Removing the deferred-cleanup transfer from `~PartWriteTxn` makes this test fail at the first
/// `minActive` assertion: the old unconditional destructor retirement advances the build floor while
/// the owner-grant outcome is still unknown. The later assertions pin the other half of the duty: the
/// next mutation resolves the durable wedge, removes the exact old precommit, and only then retires it.
TEST(CasWriterDuties, UncertainAdoptedGrantStaysActiveUntilTheNextMutationRemovesIt)
{
    auto backend = std::make_shared<DB::Cas::tests::ChunkFaultBackend>();
    auto store = openSingleAttemptPool(backend);
    const RootNamespace ns{"srv1/writer_duty_adopt"};
    DB::Cas::tests::casAdmitEntry(*backend, store->layout(), ns);

    ManifestId abandoned_id;
    auto abandoned = stageEmptyManifest(store, ns, "abandoned", abandoned_id);
    const uint64_t abandoned_seq = abandoned->buildSeq();
    const String abandoned_manifest_key = store->layout().manifestKey(abandoned_id);

    backend->fault_substr = store->layout().namespaceStreamPrefix(NamespaceLifeId::stageATransition(ns)) + "_log/";
    backend->mode = DB::Cas::tests::ChunkFaultBackend::Mode::LandedThenLost;
    backend->fault_count = 1;
    DB::Cas::tests::expectThrowsCode(
        DB::ErrorCodes::NETWORK_ERROR,
        [&] { abandoned->precommitAdd(ns, "abandoned", abandoned_id); });
    ASSERT_TRUE(store->refLaneWedgedForTest(ns));
    ASSERT_EQ(abandoned->precommitState(), PartWriteTxn::PrecommitState::Uncertain);

    abandoned.reset();
    EXPECT_EQ(store->minActive(), abandoned_seq)
        << "an unresolved owner grant must keep its build active after the transaction object is gone";

    backend->mode = DB::Cas::tests::ChunkFaultBackend::Mode::None;
    ManifestId successor_id;
    auto successor = stageEmptyManifest(store, ns, "successor", successor_id);
    const uint64_t successor_seq = successor->buildSeq();
    successor->precommitAdd(ns, "successor", successor_id);

    EXPECT_FALSE(store->refLaneWedgedForTest(ns));
    EXPECT_EQ(store->minActive(), successor_seq)
        << "the abandoned build retires only after its exact cleanup duty settles";
    EXPECT_EQ(
        store->livePrecommitsForTest(ns),
        (std::set<std::pair<String, ManifestRef>>{{"successor", successor_id.ref}}));
    EXPECT_TRUE(backend->head(abandoned_manifest_key).exists)
        << "the removed precommit body remains GC-owned until its decrement is sealed";

    successor->abandon();
    EXPECT_TRUE(store->livePrecommitsForTest(ns).empty());
}

/// Removing the absent-owner arm makes the deferred duty either stay forever or try to remove an owner
/// that was never transmitted. A controller pre-attempt refusal proves the grant absent; the next
/// healthy mutation must drain that duty as a no-op and retire the old build before publishing itself.
TEST(CasWriterDuties, ProvenAbsentGrantDrainsAsNoOpBeforeTheNextMutation)
{
    auto backend = std::make_shared<DB::Cas::InMemoryBackend>();
    DB::Cas::tests::seedPoolMetaForRestart(*backend);
    PoolConfig config = singleAttemptConfig();
    config.boot_ms_fn = [] { return uint64_t{0}; };
    config.mount_renew_period = std::chrono::hours{1};
    auto store = Pool::open(backend, config);
    const RootNamespace ns{"srv1/writer_duty_reject"};

    ManifestId rejected_id;
    auto rejected = stageEmptyManifest(store, ns, "rejected", rejected_id);
    const uint64_t rejected_seq = rejected->buildSeq();

    store->setMountDeadline(100);
    DB::Cas::tests::expectThrowsCode(
        DB::ErrorCodes::NETWORK_ERROR,
        [&] { rejected->precommitAdd(ns, "rejected", rejected_id); });
    ASSERT_FALSE(store->refLaneWedgedForTest(ns));
    ASSERT_EQ(rejected->precommitState(), PartWriteTxn::PrecommitState::Uncertain);

    rejected.reset();
    EXPECT_EQ(store->minActive(), rejected_seq)
        << "the destructor cannot retire even an uncertain grant whose rejection has not been consumed";

    store->setMountDeadline(30000);
    ManifestId successor_id;
    auto successor = stageEmptyManifest(store, ns, "successor", successor_id);
    const uint64_t successor_seq = successor->buildSeq();
    successor->precommitAdd(ns, "successor", successor_id);

    EXPECT_EQ(store->minActive(), successor_seq);
    EXPECT_EQ(
        store->livePrecommitsForTest(ns),
        (std::set<std::pair<String, ManifestRef>>{{"successor", successor_id.ref}}));

    successor->abandon();
    EXPECT_TRUE(store->livePrecommitsForTest(ns).empty());
}

/// Removing the pending-duty term from `Pool` teardown makes this test fail at the farewell
/// assertion: a clean marker would falsely certify that the durable precommit below has no remaining
/// writer work. The unclean handoff forces a fresh writer epoch; its arithmetic recovery seal then
/// makes the ordinary stale-precommit sweep the crash-remnant cleanup path.
TEST(CasWriterDuties, PendingDutySkipsCleanFarewellAndSuccessorSweepsTheCrashRemnant)
{
    auto backend = std::make_shared<DB::Cas::InMemoryBackend>();
    DB::Cas::tests::seedPoolMetaForRestart(*backend);
    const CasRequestBudget budget{
        .attempt_timeout_ms = 50,
        .operation_deadline_ms = 500,
        .max_attempts = 1,
        .lease_safety_margin_ms = 50,
    };
    const RootNamespace ns{"srv1/writer_duty_crash"};

    auto predecessor = Pool::open(backend, PoolConfig{
        .pool_prefix = "p",
        .server_id = UInt128(1),
        .server_root_id = "test",
        .background_watermark = false,
        .mount_lease_ttl_ms = std::chrono::milliseconds(500),
        .mount_renew_period = std::chrono::milliseconds(100),
        .cas_request_budget = budget,
    });
    DB::Cas::tests::casAdmitEntry(*backend, predecessor->layout(), ns);

    ManifestId abandoned_id;
    auto abandoned = stageEmptyManifest(predecessor, ns, "abandoned", abandoned_id);
    abandoned->precommitAdd(ns, "abandoned", abandoned_id);
    const uint64_t predecessor_epoch = predecessor->writerEpoch();
    const Layout layout = predecessor->layout();
    const String mount_key = layout.mountKey("test");

    abandoned.reset();
    predecessor.reset();

    const auto mount = backend->get(mount_key);
    ASSERT_TRUE(mount.has_value());
    EXPECT_NE(decodeMountLease(mount->bytes).min_active, std::numeric_limits<uint64_t>::max())
        << "a live writer-cleanup duty forbids the clean-release certificate";

    uint64_t fake_boot = 0;
    std::vector<uint64_t> waits;
    auto successor_store = Pool::open(backend, PoolConfig{
        .pool_prefix = "p",
        .server_id = UInt128(1),
        .server_root_id = "test",
        .background_watermark = false,
        .mount_lease_ttl_ms = std::chrono::milliseconds(500),
        .mount_renew_period = std::chrono::milliseconds(100),
        .cas_request_budget = budget,
        .boot_ms_fn = [&] { return fake_boot; },
        .wait_sleep_fn = [&](uint64_t ms) { fake_boot += ms; waits.push_back(ms); },
    });
    ASSERT_GT(successor_store->writerEpoch(), predecessor_epoch);
    ASSERT_FALSE(waits.empty()) << "the predecessor supplied no clean-death certificate";

    ManifestId successor_id;
    auto successor = stageEmptyManifest(successor_store, ns, "successor", successor_id);
    successor->precommitAdd(ns, "successor", successor_id);

    EXPECT_EQ(
        successor_store->livePrecommitsForTest(ns),
        (std::set<std::pair<String, ManifestRef>>{{"successor", successor_id.ref}}));
    const auto seal = successor_store->lastEpochSealForTest(ns);
    ASSERT_TRUE(seal.has_value());
    EXPECT_EQ(seal->writer_epoch, predecessor_epoch);

    successor->abandon();
}
