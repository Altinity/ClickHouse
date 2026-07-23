#include <gtest/gtest.h>

#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/ContentAddressedMetadataStorage.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/ContentAddressedTransaction.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Gc/CasGcScheduler.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasPool.h>
#include <Disks/tests/cas_test_helpers.h>
#include <Common/Exception.h>

#include <chrono>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>

/// Task 8 (rev.7 spec §1): the central six-class operation gate (`checkOpAdmitted`), the `Vanished` truth
/// semantics, and the [D5] per-reason typed messages. These tests build a real
/// `ContentAddressedMetadataStorage` over a Local object storage (the same harness as
/// gtest_ca_transaction.cpp), commit a real part, then force the pool lifecycle condition directly via the
/// Task-5 setter (`Pool::setLifecycleForTest`) to pin each class × state cell of the spec §1 table and
/// assert what every public entry does.
///
/// NOTE the harness idiom: `store()` itself is fail-closed on a terminal pool (it throws), so a test
/// captures the `PoolPtr` ONCE while the pool is still `Live` and drives `setLifecycleForTest` on that
/// captured handle -- the SAME object the metadata storage's `cas_store` points at -- rather than calling
/// `store()` again after forcing a terminal state.

namespace DB::ErrorCodes
{
extern const int INVALID_STATE;
extern const int FILE_DOESNT_EXIST;
}

using namespace DB;
using DB::Cas::PoolLifecycle;

namespace
{

/// A live table dir + part reused across the tests (the exact shape gtest_ca_transaction.cpp uses).
const std::string kTableDir = "g80/g80g80g8-0808-4808-8808-080808080808";
const std::string kPartDir = kTableDir + "/all_1_1_0";
const std::string kPartFile = kPartDir + "/data.bin";

std::shared_ptr<ContentAddressedMetadataStorage> openGateStorage()
{
    auto settings = Cas::tests::makeSettingsForTest(
        "test", std::filesystem::temp_directory_path() / "ca_op_gate_scratch");
    auto storage = std::make_shared<ContentAddressedMetadataStorage>(
        Cas::tests::makeLocalObjectStorageForTest(), "pool", "srv1", "", nullptr, settings);
    storage->startup();
    return storage;
}

/// Commit one real part (tmp -> final rename -> commit), leaving `kPartFile` durable and `kPartDir`/
/// `kTableDir` non-empty. Every op below runs against this committed state.
void commitOnePart(ContentAddressedMetadataStorage & storage)
{
    auto tx = storage.createTransaction();
    auto & ca_tx = dynamic_cast<ContentAddressedTransaction &>(*tx);
    auto buf = ca_tx.writeFile(kTableDir + "/tmp_insert_all_1_1_0/data.bin", 65536, WriteMode::Rewrite, {});
    const std::string bytes = "content-of-the-part";
    buf->write(bytes.data(), bytes.size());
    buf->finalize();
    tx->moveDirectory(kTableDir + "/tmp_insert_all_1_1_0", kPartDir);
    tx->commit(NoCommitOptions{});
}

std::string messageOf(const std::function<void()> & fn)
{
    try
    {
        fn();
    }
    catch (const Exception & e)
    {
        return std::string(e.message());
    }
    ADD_FAILURE() << "expected a DB::Exception";
    return {};
}

}

/// (a) Probes on a Vanished disk answer the truth: absent/empty, WITHOUT reaching the pool.
TEST(CasOperationGate, ProbesOnVanishedAnswerAbsentEmpty)
{
    auto storage = openGateStorage();
    commitOnePart(*storage);
    auto pool = storage->store();   /// captured while Live

    /// Live baseline: the probes see the committed part.
    ASSERT_TRUE(storage->existsFile(kPartFile));
    ASSERT_TRUE(storage->existsDirectory(kPartDir));
    ASSERT_TRUE(storage->existsFileOrDirectory(kPartFile));
    ASSERT_FALSE(storage->isDirectoryEmpty(kTableDir));
    ASSERT_FALSE(storage->listDirectory(kTableDir).empty());
    ASSERT_TRUE(storage->getStorageObjectsIfExist(kPartFile).has_value());

    pool->setLifecycleForTest(PoolLifecycle::VanishedReplaced);

    EXPECT_FALSE(storage->existsFile(kPartFile));
    EXPECT_FALSE(storage->existsDirectory(kPartDir));
    EXPECT_FALSE(storage->existsFileOrDirectory(kPartFile));
    EXPECT_TRUE(storage->listDirectory(kTableDir).empty());
    EXPECT_FALSE(storage->iterateDirectory(kTableDir)->isValid());
    EXPECT_TRUE(storage->isDirectoryEmpty(kTableDir));
    EXPECT_FALSE(storage->getStorageObjectsIfExist(kPartFile).has_value());
    /// The offender `liveTreeDirHasChildren` hardcoded-true is now truthful too: the disk root reads absent.
    EXPECT_FALSE(storage->liveTreeDirHasChildren(""));
}

/// (b) Removes on a Vanished disk are no-op SUCCESS and never touch the backend: after restoring Live the
/// part is still there. This is what lets a vanished-disk table's DROP complete.
TEST(CasOperationGate, RemovesOnVanishedAreNoOpSuccessBackendUntouched)
{
    auto storage = openGateStorage();
    commitOnePart(*storage);
    auto pool = storage->store();   /// captured while Live
    ASSERT_TRUE(storage->existsDirectory(kPartDir));

    pool->setLifecycleForTest(PoolLifecycle::VanishedReplaced);

    /// A whole-table removeRecursive + commit (the DROP shape): both no-op-succeed.
    {
        auto tx = storage->createTransaction();
        EXPECT_NO_THROW(tx->removeRecursive(kTableDir, /*should_remove_objects=*/nullptr));
        EXPECT_NO_THROW(tx->commit(NoCommitOptions{}));   /// empty parts -> Remove -> no-op success
    }
    /// A single removeDirectory of the part dir + commit: no-op-succeed.
    {
        auto tx = storage->createTransaction();
        EXPECT_NO_THROW(tx->removeDirectory(kPartDir));
        EXPECT_NO_THROW(tx->commit(NoCommitOptions{}));
    }

    /// Truth check: nothing was actually removed. Back on Live the part is intact.
    pool->setLifecycleForTest(PoolLifecycle::Live);
    EXPECT_TRUE(storage->existsDirectory(kPartDir)) << "a remove on a Vanished disk must not touch the backend";
    EXPECT_TRUE(storage->existsFile(kPartFile));
}

/// (c) A content read on a Vanished disk throws the typed per-reason [D5] message -- the exact substring
/// names the ACTUAL sub-state (erased / replaced / forgotten), never a wrong diagnosis.
TEST(CasOperationGate, ContentReadOnVanishedThrowsTypedPerReasonMessage)
{
    auto storage = openGateStorage();
    commitOnePart(*storage);
    auto pool = storage->store();   /// captured while Live

    pool->setLifecycleForTest(PoolLifecycle::VanishedReplaced);
    EXPECT_NE(messageOf([&] { storage->getFileSize(kPartFile); }).find("foreign pool"), std::string::npos);
    EXPECT_NE(messageOf([&] { storage->getStorageObjects(kPartFile); }).find("foreign pool"), std::string::npos);

    pool->setLifecycleForTest(PoolLifecycle::VanishedForgotten);
    EXPECT_NE(messageOf([&] { storage->getFileSize(kPartFile); }).find("erasure was NOT verified"),
              std::string::npos);
}

/// (d) Every class but Factory throws 668 on BOTH TransientNotLive AND IdentityLost. The 668 message
/// distinguishes the two: a transient blip reads "mount lease not held" (auto-recovering); IdentityLost
/// gets its own richer, non-auto-recovering [D5] diagnosis ("identity lost … restart or FORGET").
TEST(CasOperationGate, EveryClassThrows668OnTransientAndIdentityLost)
{
    for (const auto lc : {PoolLifecycle::TransientNotLive, PoolLifecycle::IdentityLost})
    {
        auto storage = openGateStorage();
        commitOnePart(*storage);
        storage->store()->setLifecycleForTest(lc);   /// one force from Live; no later store() call

        /// Probe
        Cas::tests::expectThrowsCode(ErrorCodes::INVALID_STATE, [&] { storage->existsFile(kPartFile); });
        Cas::tests::expectThrowsCode(ErrorCodes::INVALID_STATE, [&] { storage->existsDirectory(kPartDir); });
        Cas::tests::expectThrowsCode(ErrorCodes::INVALID_STATE, [&] { storage->listDirectory(kTableDir); });
        Cas::tests::expectThrowsCode(ErrorCodes::INVALID_STATE, [&] { storage->isDirectoryEmpty(kTableDir); });
        /// ContentRead
        Cas::tests::expectThrowsCode(ErrorCodes::INVALID_STATE, [&] { storage->getFileSize(kPartFile); });
        Cas::tests::expectThrowsCode(ErrorCodes::INVALID_STATE, [&] { storage->getStorageObjects(kPartFile); });
        /// Write (via a transaction)
        Cas::tests::expectThrowsCode(ErrorCodes::INVALID_STATE, [&] {
            auto tx = storage->createTransaction();
            auto & ca_tx = dynamic_cast<ContentAddressedTransaction &>(*tx);
            ca_tx.writeFile(kTableDir + "/tmp_x/data.bin", 65536, WriteMode::Rewrite, {});
        });
        /// Remove (via a transaction)
        Cas::tests::expectThrowsCode(ErrorCodes::INVALID_STATE, [&] {
            auto tx = storage->createTransaction();
            tx->removeRecursive(kTableDir, /*should_remove_objects=*/nullptr);
        });
        /// Admin
        Cas::tests::expectThrowsCode(ErrorCodes::INVALID_STATE, [&] { storage->runOneGcRoundForTest(); });

        /// The 668 message names the ACTUAL sub-state (not a uniform string): transient vs IdentityLost.
        const std::string msg = messageOf([&] { storage->getFileSize(kPartFile); });
        if (lc == PoolLifecycle::TransientNotLive)
            EXPECT_NE(msg.find("mount lease not held"), std::string::npos) << msg;
        else
            EXPECT_NE(msg.find("identity lost"), std::string::npos) << msg;
    }
}

/// (e) `createTransaction` (Factory: I/O-free) and the capability/introspection getters construct fine on
/// a Vanished disk -- so a vanished-disk table's DROP can allocate its removal transaction.
TEST(CasOperationGate, FactoryClassWorksOnVanished)
{
    auto storage = openGateStorage();
    storage->store()->setLifecycleForTest(PoolLifecycle::VanishedForgotten);   /// one force from Live

    EXPECT_NO_THROW({ auto tx = storage->createTransaction(); (void)tx; });
    EXPECT_EQ(storage->getType(), MetadataStorageType::ContentAddressed);
    EXPECT_NO_THROW((void)storage->getPath());
    EXPECT_NO_THROW((void)storage->isContentAddressed());
}

/// (f) `tryGetInManifestBytes` PROPAGATES the typed 668 on a terminal disk rather than converting it into
/// a silent-absent `std::nullopt` (the narrowed catch). RED before the narrowing.
TEST(CasOperationGate, TryGetInManifestBytesPropagatesTypedError)
{
    auto storage = openGateStorage();
    commitOnePart(*storage);
    auto pool = storage->store();   /// captured while Live

    pool->setLifecycleForTest(PoolLifecycle::VanishedReplaced);
    /// Never FILE_DOESNT_EXIST, never a swallowed nullopt -- the typed INVALID_STATE escapes.
    Cas::tests::expectThrowsCode(ErrorCodes::INVALID_STATE, [&] {
        storage->tryGetInManifestBytes(kTableDir + "/format_version.txt");
    });

    pool->setLifecycleForTest(PoolLifecycle::TransientNotLive);
    Cas::tests::expectThrowsCode(ErrorCodes::INVALID_STATE, [&] {
        storage->tryGetInManifestBytes(kTableDir + "/format_version.txt");
    });
}

/// (g) [Task 15] transitional guard: a Dormant disk (SYSTEM CONTENT ADDRESSED UNMOUNT) still answers the
/// OLD benign-absent for probes and "not mounted" for store-class ops -- the gate's not-Mounted branch
/// mirrors the landed `isMounted()` guards. REMOVE this test when Task 15 removes `MountState`.
TEST(CasOperationGate, DormantDiskKeepsOldBenignAbsent_RemoveAtTask15)
{
    auto storage = openGateStorage();
    commitOnePart(*storage);
    ASSERT_TRUE(storage->existsDirectory(kPartDir));

    storage->unmountSynchronously();   /// Mounted -> ... -> Dormant (no pool ref held above)

    /// Probes answer benign-absent (a Dormant disk is NOT Vanished; this is the transitional lie).
    EXPECT_FALSE(storage->existsFile(kPartFile));
    EXPECT_FALSE(storage->existsDirectory(kPartDir));
    EXPECT_TRUE(storage->listDirectory(kTableDir).empty());
    EXPECT_TRUE(storage->isDirectoryEmpty(kTableDir));
    /// Store-class ops throw "not mounted" (still INVALID_STATE), not the typed Vanished message.
    Cas::tests::expectThrowsCode(ErrorCodes::INVALID_STATE, [&] { storage->getFileSize(kPartFile); });
}

/// (h) The raw GC round entry points refuse on a not-live pool (Admin class): typed [D5] reason once Vanished.
TEST(CasOperationGate, GcEntryPointsRefuseOnNotLive)
{
    auto storage = openGateStorage();
    auto pool = storage->store();   /// captured while Live

    pool->setLifecycleForTest(PoolLifecycle::VanishedReplaced);
    EXPECT_NE(messageOf([&] { storage->runOneGcRoundForTest(); }).find("foreign pool"), std::string::npos);

    pool->setLifecycleForTest(PoolLifecycle::TransientNotLive);
    Cas::tests::expectThrowsCode(ErrorCodes::INVALID_STATE, [&] { storage->runOneGcRoundForTest(); });
}

/// (i) `CasGcScheduler::isQuiescent` reflects the round-in-flight flag: a round in flight => not quiescent.
/// (This is the join-completion signal the FORGET / GC-STOP tests rely on.)
TEST(CasOperationGate, GcSchedulerIsQuiescentReflectsRoundInFlight)
{
    auto backend = std::make_shared<Cas::InMemoryBackend>();
    auto pool = Cas::tests::openPoolForTest(backend);
    auto scheduler = std::make_shared<Cas::CasGcScheduler>(
        pool, std::chrono::seconds(3600), "op-gate-test-gc", "disk", Cas::GcRoundLogger{});
    EXPECT_TRUE(scheduler->isQuiescent());
    scheduler->setRoundInFlightForTest(true);
    EXPECT_FALSE(scheduler->isQuiescent()) << "a round in flight must NOT read as GC-quiescent";
    scheduler->setRoundInFlightForTest(false);
    EXPECT_TRUE(scheduler->isQuiescent());
}
