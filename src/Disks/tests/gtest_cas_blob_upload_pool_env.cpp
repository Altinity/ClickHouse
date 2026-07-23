#include <gtest/gtest.h>
#include <Disks/tests/cas_test_helpers.h>

/// Stage-1 §1: `ContentAddressedTransaction::uploadPendingBlobs` fans out on the server-wide blob
/// upload pool, whose getter is fail-loud (throws `LOGICAL_ERROR` -- an ABORT under a sanitizer build --
/// if the pool was never initialized). Any CA test that commits a transaction with a pending blob would
/// therefore abort the whole `unit_tests_dbms` process if the pool happened to be down.
///
/// This listener brings the pool up before EVERY test, so the pool is always initialized at the start of
/// a test body regardless of link/run order. It is deliberately a before-each hook (not a one-shot
/// `Environment::SetUp`): the raw-lifecycle suite in `gtest_cas_blob_upload_pool.cpp` shuts the pool
/// down inside its own bodies, and those tests explicitly re-establish whatever pool state they assert
/// on as their FIRST action, so re-ensuring the pool before them is harmless.
namespace
{

class BlobUploadPoolEnsuringListener : public ::testing::EmptyTestEventListener
{
public:
    void OnTestStart(const ::testing::TestInfo &) override
    {
        DB::Cas::tests::ensureBlobUploadPoolForTest();
    }
};

const bool registered_blob_upload_pool_listener = []
{
    ::testing::UnitTest::GetInstance()->listeners().Append(new BlobUploadPoolEnsuringListener);
    return true;
}();

}
