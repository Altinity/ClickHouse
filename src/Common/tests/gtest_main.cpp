#include <gtest/gtest.h>

#include <base/sanitizer_options.h>

#include <Common/ThreadPool.h>
#include <Common/scope_guard_safe.h>
#include <IO/SharedThreadPools.h>
#include <Common/tests/gtest_global_context.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasCommitThreadPool.h>

class ContextEnvironment : public testing::Environment
{
public:
    void SetUp() override { getContext(); }
    void TearDown() override { getMutableContext().destroy(); }
};

int main(int argc, char ** argv)
{
    /// Join global-pool threads before the statics they may have accessed are destroyed.
    /// That way, accesses happen-before destruction. The dedicated CAS commit pool is shut down first
    /// (mirrors server teardown: next to `StaticThreadPool::shutdownAll`, before `GlobalThreadPool` goes)
    /// so its workers cannot outlive the global pool.
    SCOPE_EXIT_SAFE({
        DB::Cas::shutdownCasCommitThreadPool();
        DB::StaticThreadPool::shutdownAll();
        GlobalThreadPool::shutdown();
    });

    /// `ContentAddressedTransaction::commit` dispatches per-part work onto the dedicated CAS commit pool
    /// (`getCasCommitThreadPool`), which throws unless initialized first -- production wires this at
    /// server startup. Initialize it ONCE here so every content-addressed test that commits a transaction
    /// has it available, without each test file having to arrange it (and without a lazy self-init that
    /// would make the server-side size setting a silent no-op).
    DB::Cas::initializeCasCommitThreadPool(16, 0, 10000);

    testing::InitGoogleTest(&argc, argv);

    auto & options = getTestCommandLineOptions();
    options.argc = argc;
    options.argv = argv;

    testing::AddGlobalTestEnvironment(new ContextEnvironment);

    return RUN_ALL_TESTS();
}
