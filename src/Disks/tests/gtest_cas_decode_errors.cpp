#include <gtest/gtest.h>

#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Formats/CasRefLogFormat.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Formats/CasTextFormat.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Gc/CasGc.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Gc/CasOrphanManifestSweep.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasPool.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasRefProtocol.h>
#include "cas_sweep_test_support.h"
#include "cas_test_helpers.h"
#include <Common/CurrentMemoryTracker.h>
#include <Common/MemoryTracker.h>
#include <Common/ThreadStatus.h>
#include <base/scope_guard.h>

#include <atomic>
#include <exception>
#include <functional>
#include <optional>
#include <thread>

namespace DB::ErrorCodes
{
extern const int CANNOT_ALLOCATE_MEMORY;
extern const int CANNOT_PARSE_ESCAPE_SEQUENCE;
extern const int CORRUPTED_DATA;
extern const int LOGICAL_ERROR;
extern const int MEMORY_LIMIT_EXCEEDED;
extern const int NETWORK_ERROR;
extern const int TIMEOUT_EXCEEDED;
extern const int TOO_DEEP_RECURSION;
extern const int TOO_LARGE_STRING_SIZE;
extern const int UNKNOWN_FORMAT_VERSION;
}

using namespace DB::Cas;
using namespace DB::Cas::tests;

/// GC holds, ends planning or retains only when a decode reports malformed bytes. Any other error from the decode,
/// a memory limit or a timeout, fails the operation and names the object.
namespace
{

const RootNamespace kNs{"00/aa@cas@"};

/// Only allocations of at least `kThrowThreshold` bytes are checked against `kMemoryLimit`, so the small allocations of
/// the reads around a decode never throw and the one that allocates a body declaring `kDeclaredBytes` always does.
constexpr Int64 kMemoryLimit = 16 << 20;
constexpr UInt64 kThrowThreshold = 8 << 20;
constexpr size_t kDeclaredBytes = 48 << 20;

/// A sanitizer build does not charge `operator new` to the memory tracker, as in `gtest_allocation_interceptors.cpp`.
bool memoryLimitReachesOperatorNew()
{
#if defined(SANITIZER)
    return false;
#else
    return true;
#endif
}

/// Outlives the allocation in the positive control, so the compiler cannot elide it.
String allocation_sink;

/// The code a direct allocation of `kDeclaredBytes` threw on the limited thread, or none.
struct LimitedRun
{
    std::optional<int> control_code;
};

/// Runs `fn` on a thread of its own whose allocations are charged to a tracker limited to `kMemoryLimit`, and
/// rethrows on the calling thread what `fn` threw. Before `fn`, it allocates `kDeclaredBytes` directly under the same
/// setup and records in `run` what that threw: the positive control that the limit is enforced at that size.
void runUnderMemoryLimit(LimitedRun & run, const std::function<void()> & fn)
{
    std::exception_ptr error;
    const UInt64 saved_threshold = CurrentMemoryTracker::getMinAllocationSizeBytesToThrow();
    /// The throwing `operator new` enforces a hard limit only for allocations at or above this size; the default is
    /// "never".
    CurrentMemoryTracker::setMinAllocationSizeBytesToThrow(kThrowThreshold);
    SCOPE_EXIT({ CurrentMemoryTracker::setMinAllocationSizeBytesToThrow(saved_threshold); });
    std::thread limited([&]
    {
        MemoryTracker limit_tracker(&total_memory_tracker, VariableContext::Process, /*log_peak_memory_usage_in_destructor_=*/false);
        limit_tracker.setHardLimit(kMemoryLimit);
        DB::ThreadStatus thread_status;
        thread_status.memory_tracker.setParent(&limit_tracker);
        thread_status.untracked_memory_limit = 0;
        try
        {
            allocation_sink.resize(kDeclaredBytes);
        }
        catch (const DB::Exception & e)
        {
            run.control_code = e.code();
        }
        allocation_sink.clear();
        allocation_sink.shrink_to_fit();
        try
        {
            fn();
        }
        catch (...)
        {
            error = std::current_exception();
        }
    });
    limited.join();
    if (error)
        std::rethrow_exception(error);
}

/// A valid zstd frame of `id` whose declared content is `kDeclaredBytes`: opening it allocates that much before the
/// decoder sees a byte, and with memory available the decoder rejects the content as `CORRUPTED_DATA`.
String largeDeclaredObject(FormatId id)
{
    return sealObject(id, String(kDeclaredBytes, ' '));
}

}

TEST(CASDecodeErrors, OnlyBadBytesCountAsAMalformedStoredRecord)
{
    for (const int code : {DB::ErrorCodes::CORRUPTED_DATA, DB::ErrorCodes::UNKNOWN_FORMAT_VERSION,
                           DB::ErrorCodes::CANNOT_PARSE_ESCAPE_SEQUENCE, DB::ErrorCodes::TOO_DEEP_RECURSION})
        EXPECT_TRUE(isMalformedStoredRecordError(code)) << code;
    for (const int code : {DB::ErrorCodes::MEMORY_LIMIT_EXCEEDED, DB::ErrorCodes::LOGICAL_ERROR,
                           DB::ErrorCodes::CANNOT_ALLOCATE_MEMORY, DB::ErrorCodes::TIMEOUT_EXCEEDED,
                           DB::ErrorCodes::NETWORK_ERROR, DB::ErrorCodes::TOO_LARGE_STRING_SIZE})
        EXPECT_FALSE(isMalformedStoredRecordError(code)) << code;
    EXPECT_TRUE(isMalformedStoredRecordError(DB::Exception(DB::ErrorCodes::CORRUPTED_DATA, "malformed")));
    EXPECT_FALSE(isMalformedStoredRecordError(DB::Exception(DB::ErrorCodes::MEMORY_LIMIT_EXCEEDED, "out of memory")));
}

/// With memory available the record at the cursor is malformed and the resolver holds. A memory limit while decoding
/// it says nothing about the bytes, so it propagates, naming the key, instead of becoming a hold.
TEST(CASDecodeErrors, AMemoryLimitWhileResolvingACursorPropagatesInsteadOfHolding)
{
    if (!memoryLimitReachesOperatorNew())
        GTEST_SKIP() << "the memory tracker enforces no limit on operator new in a sanitizer build";
    auto backend = std::make_shared<InMemoryBackend>();
    const Layout layout{"pool"};
    const NamespaceLifeId life = fixture::fixtureLife(kNs);
    const RefTxnId cursor{1, 2};
    const String key = layout.refLogKey(life, cursor);
    {
        OperationForTest op(*backend);
        ASSERT_TRUE(std::holds_alternative<Committed>((*op).create(key, largeDeclaredObject(FormatId::RefLog), Retry::once())));
    }
    {
        OperationForTest op(*backend);
        const RefCursorResume control = resolveRefCursorSuccessor(*op, layout, life, cursor, std::nullopt);
        EXPECT_EQ(control.kind, RefCursorResumeKind::Hold);
        EXPECT_NE(control.detail.find("undecodable"), String::npos) << control.detail;
    }
    LimitedRun run;
    expectThrowsCodeWithMessage(DB::ErrorCodes::MEMORY_LIMIT_EXCEEDED, key, [&]
    {
        runUnderMemoryLimit(run, [&]
        {
            OperationForTest op(*backend);
            (void)resolveRefCursorSuccessor(*op, layout, life, cursor, std::nullopt);
        });
    });
    EXPECT_EQ(run.control_code, DB::ErrorCodes::MEMORY_LIMIT_EXCEEDED) << "positive control: the limit is enforced";
}

namespace
{

/// Two eligible orphan manifest keys of one build whose fold cursor has consumed its epoch's seal: the sweep reads
/// both bodies to nominate them. The test plants the first one's body; the second is a legal manifest that sorts
/// after it.
struct SweepableOrphans
{
    std::shared_ptr<InMemoryBackend> backend = std::make_shared<InMemoryBackend>();
    PoolPtr store;
    ManifestRef first{.writer_epoch = 1, .build_sequence = 5, .manifest_ordinal = 0xAB};
    String legal_key;

    SweepableOrphans()
    {
        store = openPoolForTest(backend);
        casAdmitRecoverableEntry(*backend, store->layout(), kNs);
        setWatermarkMinActive(*backend, store->layout(), "00", /*writer_epoch*/ 1, /*min_active_build_sequence*/ 6);
        const RefTxnId cursor{2, 1};
        seedFoldCursorForTest(*backend, store->layout(), kNs, cursor);
        fixture::writeRefLogRaw(*backend, store->layout(), RefLogTxn{kNs.string(), cursor, {}, std::nullopt});
        const ManifestId legal = writeManifestRaw(*backend, store->layout(), kNs,
            ManifestRef{.writer_epoch = 1, .build_sequence = 5, .manifest_ordinal = 0xCD}, {blobEntryFor("b", DB::UInt128(2))});
        legal_key = store->layout().manifestKey(legal);
    }

    String firstKey() const { return store->layout().manifestKey(ManifestId{kNs, first}); }

    bool exists(const String & key) const
    {
        OperationForTest op(*backend);
        return (*op).head(key, Retry::once()).has_value();
    }
};

}

/// A memory limit while the sweep decodes a manifest says nothing about the bytes: the page fails, naming the key, and
/// neither counts the object undecodable nor decides the key after it. Once memory allows, the same page decodes the
/// object, finds it malformed, retains it, and walks past it.
TEST(CASDecodeErrors, AMemoryLimitWhileTheSweepDecodesAManifestFailsThePageAndTheNextPageRecovers)
{
    if (!memoryLimitReachesOperatorNew())
        GTEST_SKIP() << "the memory tracker enforces no limit on operator new in a sanitizer build";
    SweepableOrphans f;
    ASSERT_LT(f.firstKey(), f.legal_key);
    {
        OperationForTest op(*f.backend);
        ASSERT_TRUE(std::holds_alternative<Committed>(
            (*op).create(f.firstKey(), largeDeclaredObject(FormatId::PartManifest), Retry::once())));
    }

    LimitedRun run;
    expectThrowsCodeWithMessage(DB::ErrorCodes::MEMORY_LIMIT_EXCEEDED, f.firstKey(), [&]
    {
        runUnderMemoryLimit(run, [&]
        {
            (void)sweepManifestCursorPageForTest(*f.store, "", /*list_budget*/ 8, /*delete_budget*/ 8);
        });
    });
    EXPECT_EQ(run.control_code, DB::ErrorCodes::MEMORY_LIMIT_EXCEEDED) << "positive control: the limit is enforced";
    EXPECT_TRUE(f.exists(f.firstKey()));
    EXPECT_TRUE(f.exists(f.legal_key)) << "the failed page decided the key after the one it failed on";

    ManifestSweepResult recovered;
    ASSERT_NO_THROW(recovered = sweepManifestCursorPageForTest(*f.store, "", /*list_budget*/ 8, /*delete_budget*/ 8));
    EXPECT_EQ(recovered.undecodable, 1u);
    EXPECT_TRUE(f.exists(f.firstKey())) << "a malformed body is retained";
    EXPECT_TRUE(recovered.wrapped) << "the page did not reach the end of the keyspace";
    EXPECT_FALSE(f.exists(f.legal_key)) << "the page stopped at the malformed key instead of walking past it";
}

namespace
{

const UInt128 kGcOwner = hexToU128("00000000000000000000000000000001");
const RootNamespace kChainNs{"00/decoded@cas@"};

/// One namespace with ref logs `{1,1}` to `{1,3}`, each publishing a ref; a round folds all three.
struct PublishedChain
{
    std::shared_ptr<CountingBackend> backend = std::make_shared<CountingBackend>();
    PoolPtr store;

    PublishedChain()
    {
        store = Pool::open(backend, PoolConfig{
            .pool_prefix = "p", .server_root_id = "test", .gc_fold_max_defer_rounds = 0, .gc_io_concurrency = 2});
        for (uint64_t i = 1; i <= 3; ++i)
            publishAt(*backend, store->layout(), kChainNs, RefTxnId{1, i}, "ref_" + std::to_string(i), i, DB::UInt128(i),
                      /*birth=*/i == 1);
        writeRecoverableCkptForRawFixture(*backend, store->layout(), kChainNs, refCkptOf(1, RefTxnId{1, 3}));
        backend->resetCounts();
    }

    String firstLogKey() const { return store->layout().refLogKey(fixture::fixtureLife(kChainNs), RefTxnId{1, 1}); }
};

}

/// A timeout is not about the bytes of the record planning decodes: the round fails, naming the record, instead of
/// planning ending there.
TEST(CASDecodeErrors, ATimeoutWhilePlanningDecodesARecordFailsTheRound)
{
    PublishedChain chain;
    Gc gc(chain.store, kGcOwner);
    std::atomic<size_t> decodes{0};
    setPlannedRecordDecodeProbeForTest([&]
    {
        ++decodes;
        throw DB::Exception(DB::ErrorCodes::TIMEOUT_EXCEEDED, "injected while planning decodes a record");
    });
    SCOPE_EXIT({ setPlannedRecordDecodeProbeForTest({}); });
    expectThrowsCodeWithMessage(DB::ErrorCodes::TIMEOUT_EXCEEDED, chain.firstLogKey(), [&] { (void)gc.runRegularRound(); });
    EXPECT_GE(decodes.load(), 1u) << "positive control: planning reached a record";
    EXPECT_EQ(chain.backend->deleteTotal(), 0u);
}

/// A timeout is not about the bytes of the record the walk decodes: the round fails, naming the record, instead of
/// holding the namespace.
TEST(CASDecodeErrors, ATimeoutWhileTheWalkDecodesARecordFailsTheRound)
{
    PublishedChain chain;
    Gc gc(chain.store, kGcOwner);
    std::atomic<size_t> decodes{0};
    setWalkRecordDecodeProbeForTest([&]
    {
        ++decodes;
        throw DB::Exception(DB::ErrorCodes::TIMEOUT_EXCEEDED, "injected while the walk decodes a record");
    });
    SCOPE_EXIT({ setWalkRecordDecodeProbeForTest({}); });
    expectThrowsCodeWithMessage(DB::ErrorCodes::TIMEOUT_EXCEEDED, chain.firstLogKey(), [&] { (void)gc.runRegularRound(); });
    EXPECT_GE(decodes.load(), 1u) << "positive control: the walk reached a record";
    EXPECT_EQ(chain.backend->deleteTotal(), 0u);
}
