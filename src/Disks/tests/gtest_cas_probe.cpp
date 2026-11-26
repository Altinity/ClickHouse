#include <gtest/gtest.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Backend/CasFence.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Backend/CasInMemoryBackend.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Backend/CasObjectStorageBackend.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Backend/CasProbe.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Backend/CasRequests.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Formats/CasBlobIntentFormat.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasPool.h>
#include <Disks/tests/cas_test_helpers.h>
#include <Disks/DiskObjectStorage/ObjectStorages/Local/LocalObjectStorage.h>
#include <Common/ErrnoException.h>
#include <base/scope_guard.h>
#include <Common/Exception.h>
#include <Poco/Exception.h>

#include "config.h"
#if USE_AWS_S3
#include <IO/S3Common.h>
#endif

#include <exception>

namespace DB::ErrorCodes
{
extern const int NETWORK_ERROR;
extern const int NOT_IMPLEMENTED;
}

#include <cerrno>
#include <filesystem>
#include <set>
#include <unistd.h>

using namespace DB::Cas;

namespace DB::ErrorCodes
{
    extern const int CANNOT_OPEN_FILE;
    extern const int NETWORK_ERROR;
    extern const int NOT_IMPLEMENTED;
    extern const int READONLY;
}

namespace
{

/// Every test here constructs one backend and runs the battery against it once; a non-owning
/// `BackendPtr` over the test's stack- or shared_ptr-held backend keeps that construction pattern
/// rather than forcing a second allocation. The open fence never trips: `runCapabilityProbe` runs
/// during pool bootstrap, before any mount fence exists to enforce.
CasRequests makeRequests(Backend & backend)
{
    return CasRequests(BackendPtr(&backend, [](Backend *) {}), Fence::open(), {}, {}, nullptr, DB::Cas::kDefaultRequestWindowMs);
}

}

TEST(CASProbe, PassesOnEnforcingBackend)
{
    auto b = std::make_shared<InMemoryBackend>();
    auto requests = makeRequests(*b);
    auto op = requests.admit();
    EXPECT_NO_THROW(runCapabilityProbe(op, "p/.cas_probe"));
    EXPECT_TRUE(op.list("p/.cas_probe", "", 10, Retry::once()).keys.empty());   // probe cleans up after itself
}

namespace
{

class IgnoresConditionalBatchTokensBackend : public InMemoryBackend
{
public:
    std::vector<RawBatchRemoveResult> removeManyByConditional(
        const std::vector<RawConditionalRemove> & removals, TransportAccess & access) override
    {
        std::vector<RawBatchRemoveResult> results;
        results.reserve(removals.size());
        for (const RawConditionalRemove & removal : removals)
        {
            const auto current = InMemoryBackend::head(removal.key, access);
            results.push_back(current ? InMemoryBackend::remove(removal.key, current->value, access) : RawRemoval::Gone);
        }
        return results;
    }
};

class NoOpConditionalBatchDeleteBackend : public InMemoryBackend
{
public:
    std::vector<RawBatchRemoveResult> removeManyByConditional(
        const std::vector<RawConditionalRemove> & removals, TransportAccess & access) override
    {
        std::vector<RawBatchRemoveResult> results;
        results.reserve(removals.size());
        for (const RawConditionalRemove & removal : removals)
        {
            const auto current = InMemoryBackend::head(removal.key, access);
            if (!current)
                results.push_back(RawRemoval::Gone);
            else if (current->value != removal.expected_token)
                results.push_back(RawRemoval::Mismatch);
            else
                results.push_back(RawRemoval::Removed);
        }
        return results;
    }
};

class ReportsAbsentKeyAsRemovedBackend : public InMemoryBackend
{
public:
    std::vector<RawBatchRemoveResult> removeManyByConditional(
        const std::vector<RawConditionalRemove> & removals, TransportAccess & access) override
    {
        auto results = InMemoryBackend::removeManyByConditional(removals, access);
        EXPECT_EQ(results, (std::vector<RawBatchRemoveResult>{RawRemoval::Removed, RawRemoval::Mismatch, RawRemoval::Removed, RawRemoval::Gone}));
        for (auto & result : results)
            if (result == RawRemoval::Gone)
                result = RawRemoval::Removed;
        return results;
    }
};

class FailingConditionalBatchBackend : public InMemoryBackend
{
public:
    explicit FailingConditionalBatchBackend(const std::exception_ptr & failure_) : failure(failure_)
    {
    }

    std::vector<RawBatchRemoveResult> removeManyByConditional(const std::vector<RawConditionalRemove> &, TransportAccess &) override
    {
        ++batch_attempts;
        std::rethrow_exception(failure);
    }

    size_t batch_attempts = 0;

private:
    std::exception_ptr failure;
};

class GenerationConditionalBatchBackend : public FailingConditionalBatchBackend
{
public:
    using FailingConditionalBatchBackend::FailingConditionalBatchBackend;

    Dialect dialect() const override { return Dialect::Generation; }
};

}

TEST(CASProbe, ReportsConditionalBatchCapability)
{
    InMemoryBackend enforcing;
    auto enforcing_requests = makeRequests(enforcing);
    auto enforcing_op = enforcing_requests.admit();
    EXPECT_TRUE(runCapabilityProbe(enforcing_op, "p/.cas_probe").conditional_batch_delete_supported);

    IgnoresConditionalBatchTokensBackend ignores;
    auto ignores_requests = makeRequests(ignores);
    auto ignores_op = ignores_requests.admit();
    EXPECT_FALSE(runCapabilityProbe(ignores_op, "p/.cas_probe").conditional_batch_delete_supported);
}

TEST(CASProbe, SkipsConditionalBatchDeleteForGenerationTokens)
{
    GenerationConditionalBatchBackend backend(std::make_exception_ptr(
        DB::Exception(DB::ErrorCodes::NOT_IMPLEMENTED, "conditional batch delete unsupported")));
    auto requests = makeRequests(backend);
    auto op = requests.admit();

    EXPECT_FALSE(runCapabilityProbe(op, "p/.cas_probe").conditional_batch_delete_supported);
    EXPECT_EQ(backend.batch_attempts, 0u);
    EXPECT_TRUE(op.list("p/.cas_probe", "", 10, Retry::once()).keys.empty());
}

TEST(CASProbe, RejectsConditionalBatchSuccessWithoutDeletion)
{
    NoOpConditionalBatchDeleteBackend backend;
    auto requests = makeRequests(backend);
    auto op = requests.admit();

    EXPECT_FALSE(runCapabilityProbe(op, "p/.cas_probe").conditional_batch_delete_supported);
    EXPECT_TRUE(op.list("p/.cas_probe", "", 10, Retry::once()).keys.empty());
}

TEST(CASProbe, RequiresGoneForAnAlreadyDeletedKeyInTheBatch)
{
    ReportsAbsentKeyAsRemovedBackend backend;
    auto requests = makeRequests(backend);
    auto op = requests.admit();

    EXPECT_FALSE(runCapabilityProbe(op, "p/.cas_probe").conditional_batch_delete_supported);
}

TEST(CASProbe, SingletonMismatchCannotProveBatchTokenEnforcement)
{
    class PartialProbeBackend final : public IgnoresConditionalBatchTokensBackend
    {
    public:
        std::vector<size_t> request_sizes;
        std::vector<RawBatchRemoveResult> removeManyByConditional(
            const std::vector<RawConditionalRemove> & removals, TransportAccess & access) override
        {
            request_sizes.push_back(removals.size());
            if (request_sizes.size() == 1 && removals.size() == 4)
            {
                auto reply = InMemoryBackend::removeManyByConditional(removals, access);
                reply[1] = std::unexpected(std::make_exception_ptr(Poco::TimeoutException("stale key throttled")));
                return reply;
            }
            if (removals.size() == 1)
                return InMemoryBackend::removeManyByConditional(removals, access);
            return IgnoresConditionalBatchTokensBackend::removeManyByConditional(removals, access);
        }
    };

    auto backend = std::make_shared<PartialProbeBackend>();
    DB::Cas::tests::FakeClock clock;
    CasRequests requests(backend, Fence::open(), clock.nowFn(), clock.sleepFn(), nullptr, kDefaultRequestWindowMs);
    auto op = requests.admit();
    EXPECT_FALSE(runCapabilityProbe(op, "p/.cas_probe").conditional_batch_delete_supported);
    EXPECT_EQ(backend->request_sizes, (std::vector<size_t>{4, 4}));
    EXPECT_TRUE(op.list("p/.cas_probe", "", 10, Retry::once()).keys.empty());
}

TEST(CASProbe, ExplicitlyUnsupportedConditionalBatchReturnsFalseAndCleansUp)
{
    FailingConditionalBatchBackend backend(std::make_exception_ptr(
        DB::Exception(DB::ErrorCodes::NOT_IMPLEMENTED, "conditional batch delete unsupported")));
    auto requests = makeRequests(backend);
    auto op = requests.admit();

    EXPECT_FALSE(runCapabilityProbe(op, "p/.cas_probe").conditional_batch_delete_supported);
    EXPECT_EQ(backend.batch_attempts, 1u);
    EXPECT_TRUE(op.list("p/.cas_probe", "", 10, Retry::once()).keys.empty());
}

#if USE_AWS_S3
TEST(CASProbe, ConditionalBatchAccessDeniedPropagatesAndCleansUp)
{
    FailingConditionalBatchBackend backend(std::make_exception_ptr(
        DB::S3Exception("conditional batch delete denied", Aws::S3::S3Errors::ACCESS_DENIED)));
    auto requests = makeRequests(backend);
    auto op = requests.admit();

    try
    {
        runCapabilityProbe(op, "p/.cas_probe");
        ADD_FAILURE() << "expected S3 access denial";
    }
    catch (const DB::S3Exception & e)
    {
        EXPECT_EQ(e.getS3ErrorCode(), Aws::S3::S3Errors::ACCESS_DENIED);
    }
    EXPECT_EQ(backend.batch_attempts, 1u);
    EXPECT_TRUE(op.list("p/.cas_probe", "", 10, Retry::once()).keys.empty());
}
#endif

TEST(CASProbe, ConditionalBatchTimeoutPropagatesAfterRetriesAndCleansUp)
{
    DB::Cas::tests::FakeClock clock;
    auto backend = std::make_shared<FailingConditionalBatchBackend>(std::make_exception_ptr(Poco::TimeoutException()));
    CasRequests requests(backend, Fence::open(), clock.nowFn(), clock.sleepFn(), nullptr, kDefaultRequestWindowMs);
    auto op = requests.admit();

    DB::Cas::tests::expectThrowsCodeWithMessage(DB::ErrorCodes::NETWORK_ERROR, "removeManyByConditional",
        [&] { runCapabilityProbe(op, "p/.cas_probe"); });
    EXPECT_GT(backend->batch_attempts, 1u);
    EXPECT_TRUE(op.list("p/.cas_probe", "", 10, Retry::once()).keys.empty());
}

TEST(CASProbe, FailsClosedOnNonEnforcingDelete)
{
    auto b = std::make_shared<InMemoryBackend>();
    b->setEnforceTokens(false);                                  // the MinIO-OSS failure mode
    auto requests = makeRequests(*b);
    auto op = requests.admit();
    EXPECT_THROW(runCapabilityProbe(op, "p/.cas_probe"), DB::Exception);
}

TEST(CASProbe, FailsClosedOnDeleteMarkers)
{
    auto b = std::make_shared<InMemoryBackend>();
    b->setSimulateDeleteMarkers(true);                           // versioning enabled on the prefix
    auto requests = makeRequests(*b);
    auto op = requests.admit();
    String message;
    try
    {
        runCapabilityProbe(op, "p/.cas_probe");
    }
    catch (const DB::Exception & e)
    {
        message = e.message();
    }
    EXPECT_NE(message.find("does not make a versioned bucket usable"), String::npos) << message;
    EXPECT_EQ(message.find("has no override"), String::npos) << message;
}

TEST(CASProbe, PassesOnEmulatedLocal)
{
    auto b = std::make_shared<ObjectStorageBackend>(
        DB::Cas::tests::makeLocalObjectStorageForTest(), ObjectStorageBackend::Mode::EmulatedSingleProcess);
    auto requests = makeRequests(*b);
    auto op = requests.admit();
    EXPECT_NO_THROW(runCapabilityProbe(op, "p/.cas_probe"));
}

/// Two servers mounting the SAME shared CA pool concurrently must not race on the probe keys.
/// We simulate "a concurrent mounter's probe is in flight" by PRE-SEEDING the fixed-name probe key
/// `<pool>/_probe/token` over a shared backend, then opening the Pool. A fixed-key probe would meet
/// the seeded object as a refused precondition on its own `create` and fail the open; with the
/// per-mount unique probe prefix `<pool>/_probe/<rand>/token`, the seeded key does not collide and
/// the open succeeds — exactly the concurrent-shared-pool-mount behaviour we need.
///
/// Goes through `Pool::open` (owned elsewhere), so it exercises `runCapabilityProbe` only indirectly
/// and needs no signature change here.
TEST(CASProbe, ConcurrentMountsDoNotCollide)
{
    auto b = std::make_shared<InMemoryBackend>();
    auto probe_requests = makeRequests(*b);
    auto probe_op = probe_requests.admit();

    /// Simulate a concurrent mounter whose probe object under the legacy fixed key is still present.
    ASSERT_TRUE(std::holds_alternative<Committed>(
        probe_op.create("p/_probe/token", "concurrent-mounter-in-flight", Retry::once())));

    /// A real (second) mount over the same shared pool must still succeed — its probe runs under a
    /// fresh per-mount-unique prefix and never touches the seeded fixed key.
    EXPECT_NO_THROW(Pool::open(b, PoolConfig{.pool_prefix = "p", .server_root_id = "test"}));

    /// And two genuinely-concurrent mounts (distinct unique prefixes) both succeed over one backend.
    EXPECT_NO_THROW(Pool::open(b, PoolConfig{.pool_prefix = "p", .server_root_id = "test"}));

    /// The seeded fixed-key artifact is untouched (the probe never collided with it).
    EXPECT_TRUE(probe_op.read("p/_probe/token", Retry::once()).has_value());
}

TEST(CASProbe, PoolPinsConditionalBatchCapabilityOnlyAfterProbe)
{
    auto backend = std::make_shared<InMemoryBackend>();
    EXPECT_EQ(backend->conditionalBatchDeleteKeyLimit(), 1u);

    auto store = Pool::open(backend, PoolConfig{.pool_prefix = "p", .server_root_id = "test"});
    EXPECT_EQ(store->poolBackendPtr()->conditionalBatchDeleteKeyLimit(), kBulkDeleteMaxKeys);

    auto skipped_backend = std::make_shared<InMemoryBackend>();
    auto skipped = Pool::open(skipped_backend, PoolConfig{
        .pool_prefix = "skip", .server_root_id = "test", .skip_access_check = true});
    EXPECT_EQ(skipped->poolBackendPtr()->conditionalBatchDeleteKeyLimit(), 1u);
}

/// RFC cas-s3-timeout-retry-control: a Native-mode mount over an object storage that does not support
/// the SingleAttempt retry profile must never silently proceed under the disk's default (~500-attempt)
/// transparent retry policy — see Backend::checkConditionalWriteSingleAttemptSupport. This calls the
/// hook directly on the backend (not through `runCapabilityProbe`, which no longer runs it — see
/// CasProbe.h), so it is unaffected by the request-contract migration.
/// LocalObjectStorage never supports the profile (IObjectStorage::supportsRetryProfile's default
/// implementation only answers true for Default), so Native mode over it is exactly the case this must
/// refuse. EmulatedSingleProcess is exempt: it never claims single-attempt S3 semantics in the first
/// place (PassesOnEmulatedLocal above).
TEST(CASProbe, FailsClosedOnUnsupportedSingleAttemptProfile)
{
    auto native = std::make_shared<ObjectStorageBackend>(
        DB::Cas::tests::makeLocalObjectStorageForTest(), ObjectStorageBackend::Mode::Native);
    EXPECT_THROW(native->checkConditionalWriteSingleAttemptSupport(), DB::Exception);

    auto emulated = std::make_shared<ObjectStorageBackend>(
        DB::Cas::tests::makeLocalObjectStorageForTest(), ObjectStorageBackend::Mode::EmulatedSingleProcess);
    EXPECT_NO_THROW(emulated->checkConditionalWriteSingleAttemptSupport());
}

namespace
{

/// Honors every conditional WRITE but ignores the precondition on a conditional REMOVE. This is what a
/// GCS delete degenerates to when its numeric generation leaves as a raw `If-Match` — no
/// `x-goog-if-generation-match` — and the service ignores the header it does not recognise. Gated on
/// the PRIMITIVE (`Backend::remove`), which is what `CasOperation::remove` actually calls; a fault
/// injected on the legacy `deleteExact` forwarder would no longer intercept anything.
class IgnoresDeleteTokenBackend : public InMemoryBackend
{
public:
    RawRemoval remove(const String & key, const String & /*expected_value*/, TransportAccess & access) override
    {
        const auto meta = InMemoryBackend::head(key, access);
        if (!meta)
            return RawRemoval::Gone;
        return InMemoryBackend::remove(key, meta->value, access);
    }
};

/// The other half of that degeneracy: the service refuses the unrecognised header outright, so even
/// the correct incarnation never removes anything.
class RejectsDeleteTokenBackend : public InMemoryBackend
{
public:
    RawRemoval remove(const String &, const String &, TransportAccess &) override
    {
        return RawRemoval::Mismatch;
    }
};

}

/// A GCS mount whose exact deletes lost their generation semantics can fail in either direction, and
/// the probe's delete battery must reject the mount both times. Both backends enforce every
/// conditional write, so every step before the battery's delete checks passes and only the
/// stale-incarnation-preserved check or the correct-incarnation-removed check can be what fires —
/// `PassesOnEnforcingBackend` above is the control showing the same probe succeeds when only `remove`
/// is left alone.
///
/// This is about the battery, not about the marking: that the `NativeConditional` mode actually
/// reaches the production request object is proven where the request is built, not here.
TEST(CASProbe, ExactDeleteBatteryDetectsMissingGenerationMode)
{
    IgnoresDeleteTokenBackend ignores;
    auto ignores_requests = makeRequests(ignores);
    auto ignores_op = ignores_requests.admit();
    EXPECT_THROW(runCapabilityProbe(ignores_op, "p/.cas_probe"), DB::Exception);

    RejectsDeleteTokenBackend rejects;
    auto rejects_requests = makeRequests(rejects);
    auto rejects_op = rejects_requests.admit();
    EXPECT_THROW(runCapabilityProbe(rejects_op, "p/.cas_probe"), DB::Exception);
}

namespace
{

/// Rejects, LOCALLY and without touching the store, any conditional write/remove whose precondition
/// value is not grammar-valid under the claimed dialect — the production shape of the retired
/// `DialectGatedCountingBackend`, ported to the primitive interface (`write`/`remove` carry a raw
/// precondition VALUE now, not a typed token, so the gate is `isIncarnationValue` rather than a type-tag
/// compare). `write_reached`/`remove_reached` count only the calls that got PAST the gate, so a
/// regression that reintroduces a synthesized (grammar-invalid-somewhere) precondition drops one of
/// these counts instead of passing silently.
class DialectGatedBackend : public InMemoryBackend
{
public:
    explicit DialectGatedBackend(Dialect claimed_dialect_) : claimed_dialect(claimed_dialect_) {}
    Dialect dialect() const override { return claimed_dialect; }

    std::expected<String, RawConflict> write(const String & key, const String & bytes,
                                             const std::optional<String> & expected_value, TransportAccess & access) override
    {
        if (expected_value && !isIncarnationValue(claimed_dialect, *expected_value))
            return std::unexpected(RawConflict{});   /// dialect-gated: never reaches the real store
        ++write_reached;
        return InMemoryBackend::write(key, bytes, expected_value, access);
    }

    RawRemoval remove(const String & key, const String & expected_value, TransportAccess & access) override
    {
        if (!isIncarnationValue(claimed_dialect, expected_value))
            return RawRemoval::Mismatch;   /// dialect-gated: never reaches the real store
        ++remove_reached;
        return InMemoryBackend::remove(key, expected_value, access);
    }

    int write_reached = 0;
    int remove_reached = 0;

private:
    Dialect claimed_dialect;
};

}

/// The probe's reordered "wrong incarnation" steps always reuse a REAL, backend-minted `Etag`
/// from the same key rather than a synthesized value — see CasProbe.cpp's step comments — and every
/// such value is grammar-valid under every dialect by construction (`InMemoryBackend`'s minted values are
/// a monotonically increasing decimal starting at "1": non-empty and comma/`*`-free for ETag, a canonical
/// positive decimal for Generation, merely non-empty for Emulated). So under EVERY dialect the battery's
/// four conditional writes (steps 1-4) and two conditional removes (steps 5, 7) must all reach the real
/// store — asserting the exact counts is what makes this test able to fail: a regression that
/// reintroduces a synthesized, foreign-dialect precondition would get gated locally on at least one
/// dialect, dropping one of these counts below the total instead of merely changing an outcome enum.
TEST(CASProbe, ReorderedProbePassesOnAllThreeDialects)
{
    for (const Dialect dialect : {Dialect::ETag, Dialect::Generation, Dialect::Emulated})
    {
        DialectGatedBackend b(dialect);
        auto requests = makeRequests(b);
        auto op = requests.admit();
        EXPECT_NO_THROW(runCapabilityProbe(op, "p/.cas_probe")) << "dialect " << static_cast<int>(dialect);
        EXPECT_TRUE(op.list("p/.cas_probe", "", 10, Retry::once()).keys.empty()) << "dialect " << static_cast<int>(dialect);
        /// The optional batch check adds four writes and one cleanup remove when it runs.
        const bool runs_batch_probe = dialect != Dialect::Generation;
        EXPECT_EQ(b.write_reached, runs_batch_probe ? 8 : 4) << "dialect " << static_cast<int>(dialect);
        EXPECT_EQ(b.remove_reached, runs_batch_probe ? 3 : 2) << "dialect " << static_cast<int>(dialect);
    }
}

TEST(CASProbe, RefusesATokenThatIsNotAnIdentity)
{
    const String expected = "the store returns a token that cannot serve as a blob identity: ";

    /// HEAD on a probe key answers with a weak ETag, which no blob incarnation can be named by.
    auto weak = std::make_shared<DB::Cas::tests::HeadValueOverrideBackend>(Dialect::Emulated,
        [](const String & key, const String & value) { return key.find("/_probe/") != String::npos ? String("W/\"x\"") : value; });
    {
        auto requests = makeRequests(*weak);
        auto op = requests.admit();
        const String message
            = DB::Cas::tests::messageOf(DB::ErrorCodes::NOT_IMPLEMENTED, [&] { runCapabilityProbe(op, "p/_probe/u"); });
        EXPECT_NE(message.find(expected), String::npos) << message;
        EXPECT_NE(message.find("W/\"x\""), String::npos) << message;
    }

    String pool_message;
    try
    {
        Pool::open(weak, PoolConfig{.pool_prefix = "p", .server_root_id = "test"});
    }
    catch (const DB::Exception & e)
    {
        pool_message = e.message();
    }
    EXPECT_NE(pool_message.find(expected), String::npos) << pool_message;
    {
        auto requests = makeRequests(*weak);
        auto op = requests.admit();
        EXPECT_FALSE(op.read("p/_pool_meta", Retry::once()).has_value());
    }

    /// Positive control: the same open over a backend with ordinary tokens writes `_pool_meta`.
    auto plain = std::make_shared<InMemoryBackend>();
    ASSERT_NO_THROW(Pool::open(plain, PoolConfig{.pool_prefix = "p", .server_root_id = "test"}));
    auto requests = makeRequests(*plain);
    auto op = requests.admit();
    EXPECT_TRUE(op.read("p/_pool_meta", Retry::once()).has_value());
}

/// A read-only store answers the probe's first create with its own refusal. The probe reports that refusal at
/// once, with the store's code, instead of retrying it to the deadline and naming a missing capability.
TEST(CASProbe, ReadOnlyStoreIsReportedAtOnce)
{
    const auto root = std::filesystem::temp_directory_path() / ("cas_probe_readonly_" + std::to_string(::getpid()));
    std::filesystem::remove_all(root);
    std::filesystem::create_directories(root);
    SCOPE_EXIT({ std::filesystem::remove_all(root); });
    auto storage = std::make_shared<DB::LocalObjectStorage>(DB::LocalObjectStorageSettings("test", root.string() + "/", /*read_only_=*/true));
    auto backend = std::make_shared<ObjectStorageBackend>(storage, ObjectStorageBackend::Mode::EmulatedSingleProcess);
    DB::Cas::tests::FakeClock clock;
    CasRequests requests(backend, Fence::open(), clock.nowFn(), clock.sleepFn(), nullptr, DB::Cas::kDefaultRequestWindowMs);
    auto op = requests.admit();

    const String message = DB::Cas::tests::messageOf(DB::ErrorCodes::READONLY, [&] { runCapabilityProbe(op, "p/.cas_probe"); });
    EXPECT_NE(message.find("CasProbe: the store refused the create of 'p/.cas_probe/token'"), String::npos) << message;
    EXPECT_NE(message.find("readonly"), String::npos) << message;
    EXPECT_TRUE(clock.sleeps.empty()) << "the refused create was retried " << clock.sleeps.size() << " time(s)";
}

/// A probe key some other writer holds is still a create that did not commit, reported as before.
TEST(CASProbe, OccupiedProbeKeyKeepsItsMessage)
{
    auto b = std::make_shared<InMemoryBackend>();
    auto requests = makeRequests(*b);
    auto op = requests.admit();
    ASSERT_TRUE(std::holds_alternative<Committed>(op.create("p/.cas_probe/token", "someone else", Retry::once())));

    DB::Cas::tests::expectThrowsCodeWithMessage(DB::ErrorCodes::NOT_IMPLEMENTED, "CasProbe: create on a fresh key did not commit", [&]
    {
        runCapabilityProbe(op, "p/.cas_probe");
    });
}

TEST(CASProbe, LocalRefusalsAreDefinite)
{
    EXPECT_TRUE(isDefinitelyRefusedWrite(DB::Exception(DB::ErrorCodes::READONLY, "read-only")));
    for (const int error : {EACCES, EPERM, EROFS})
        EXPECT_TRUE(isDefinitelyRefusedWrite(DB::ErrnoException(std::string("open"), DB::ErrorCodes::CANNOT_OPEN_FILE, error))) << error;
    /// Positive controls: failures that may pass, or may have landed, stay retryable.
    EXPECT_FALSE(isDefinitelyRefusedWrite(DB::ErrnoException(std::string("open"), DB::ErrorCodes::CANNOT_OPEN_FILE, EIO)));
    EXPECT_FALSE(isDefinitelyRefusedWrite(DB::Exception(DB::ErrorCodes::NETWORK_ERROR, "reset")));
}

namespace
{

using DB::Cas::tests::JournalEntry;
using DB::Cas::tests::JournalVerb;

/// Refuses one kind of request under the intent probe's sub-prefix, as a credential scoped away from it does.
class IntentRightsBackend : public DB::Cas::tests::CountingBackend
{
public:
    /// `Delete` refuses both delete verbs; `UnconditionalDelete` refuses only the one without a precondition.
    enum class Deny : uint8_t { Nothing, Get, Put, List, Delete, UnconditionalDelete, ListOfThePrefix };
    Deny deny = Deny::Nothing;
    Dialect claimed_dialect = Dialect::Emulated;

    Dialect dialect() const override { return claimed_dialect; }

    std::optional<Raw> read(const String & key, TransportAccess & access) override
    {
        if (deny == Deny::Get && underProbe(key))
            throw DB::Exception(DB::ErrorCodes::NOT_IMPLEMENTED, "injected 403 on the GET of {}", key);
        return CountingBackend::read(key, access);
    }
    std::expected<String, RawConflict> write(const String & key, const String & bytes, const std::optional<String> & expected_value,
                                             TransportAccess & access) override
    {
        if (deny == Deny::Put && underProbe(key))
            throw DB::Exception(DB::ErrorCodes::READONLY, "injected 403 on the PUT of {}", key);
        return CountingBackend::write(key, bytes, expected_value, access);
    }
    RawListPage list(const String & prefix, const String & cursor, size_t limit, TransportAccess & access) override
    {
        if (deny == Deny::List && underProbe(prefix))
            throw DB::Exception(DB::ErrorCodes::NOT_IMPLEMENTED, "injected 403 on the LIST of {}", prefix);
        /// A grant scoped by the exact `s3:prefix` of the request: the sub-prefix lists, the prefix GC lists does not.
        if (deny == Deny::ListOfThePrefix && prefix == Layout("p").blobIntentsPrefix())
            throw DB::Exception(DB::ErrorCodes::NOT_IMPLEMENTED, "injected 403 on the LIST of {}", prefix);
        return CountingBackend::list(prefix, cursor, limit, access);
    }
    RawRemoval remove(const String & key, const String & expected_value, TransportAccess & access) override
    {
        if (deny == Deny::Delete && underProbe(key))
            throw DB::Exception(DB::ErrorCodes::NOT_IMPLEMENTED, "injected 403 on the DELETE of {}", key);
        return CountingBackend::remove(key, expected_value, access);
    }
    void removeManyWriteOnce(const std::vector<DB::Cas::WriteOnceKey> & keys, TransportAccess & access) override
    {
        if (deny == Deny::Delete || deny == Deny::UnconditionalDelete)
            for (const DB::Cas::WriteOnceKey & key : keys)
                if (underProbe(key.str()))
                    throw DB::Exception(DB::ErrorCodes::NOT_IMPLEMENTED, "injected 403 on the unconditional DELETE of {}", key.str());
        CountingBackend::removeManyWriteOnce(keys, access);
    }

    std::vector<String> keysUnderProbe()
    {
        DB::Cas::tests::OperationForTest op(*this);
        std::vector<String> keys;
        const Deny saved = std::exchange(deny, Deny::Nothing);
        (*op).forEachListedKey(Layout("p").blobIntentsProbePrefix(), [&](const ListedKey & k) { keys.push_back(k.key); return true; },
                               Retry::standard());
        deny = saved;
        return keys;
    }

private:
    static bool underProbe(const String & key) { return key.starts_with(Layout("p").blobIntentsProbePrefix()); }
};

String probeMessage(IntentRightsBackend & backend, IntentPrefixRights rights)
{
    auto requests = makeRequests(backend);
    auto op = requests.admit();
    try
    {
        runIntentPrefixProbe(op, Layout("p"), rights);
    }
    catch (const DB::Exception & e)
    {
        return e.message();
    }
    return {};
}

}

TEST(CASProbe, IntentPrefixProbeUsesEveryRightAndLeavesNothing)
{
    IntentRightsBackend backend;
    EXPECT_EQ(probeMessage(backend, IntentPrefixRights::ReadWrite), "");
    EXPECT_TRUE(backend.keysUnderProbe().empty());
    std::set<JournalVerb> verbs;
    for (const JournalEntry & e : backend.journal())
        if (!e.keys.empty() && e.keys.front().starts_with(Layout("p").blobIntentsProbePrefix()) && e.verb != JournalVerb::Head)
            verbs.insert(e.verb);
    EXPECT_EQ(verbs, (std::set<JournalVerb>{JournalVerb::Read, JournalVerb::Write, JournalVerb::List, JournalVerb::RemoveMany}));
}

TEST(CASProbe, IntentPrefixProbeNamesTheMissingRight)
{
    using Deny = IntentRightsBackend::Deny;
    const std::vector<std::pair<Deny, String>> cases{
        {Deny::Get, "the credential cannot tell an absent intent from a denied one: grant GET and LIST on 'p/blob-intents/'"},
        {Deny::Put, "grant PUT with If-None-Match on 'p/blob-intents/'"},
        {Deny::List, "grant LIST on 'p/blob-intents/'"},
        {Deny::Delete, "grant DELETE on 'p/blob-intents/'"},
        {Deny::UnconditionalDelete, "grant DELETE on 'p/blob-intents/'"}};
    for (const auto & [deny, expected] : cases)
    {
        SCOPED_TRACE(expected);
        IntentRightsBackend backend;
        backend.deny = deny;
        const String message = probeMessage(backend, IntentPrefixRights::ReadWrite);
        EXPECT_NE(message.find(expected), String::npos) << message;
        if (deny != Deny::Delete)
            EXPECT_TRUE(backend.keysUnderProbe().empty()) << "the probe object outlived a failed probe";
    }
}

/// GC's sweep lists `blob-intents/` itself, so the probe sends that request too: a right granted only on the
/// probe's sub-prefix is refused before anything is written.
TEST(CASProbe, IntentPrefixProbeListsThePrefixGcLists)
{
    for (const IntentPrefixRights rights : {IntentPrefixRights::ReadWrite, IntentPrefixRights::Read})
    {
        SCOPED_TRACE(rights == IntentPrefixRights::Read ? "read" : "read-write");
        IntentRightsBackend allowed;
        allowed.claimed_dialect = Dialect::ETag;
        EXPECT_EQ(probeMessage(allowed, rights), "");
        bool listed_prefix = false;
        for (const JournalEntry & e : allowed.journal())
            listed_prefix = listed_prefix || (e.verb == JournalVerb::List && e.keys.front() == Layout("p").blobIntentsPrefix());
        EXPECT_TRUE(listed_prefix) << "the probe never listed the prefix GC lists";

        IntentRightsBackend denied;
        denied.claimed_dialect = Dialect::ETag;
        denied.deny = IntentRightsBackend::Deny::ListOfThePrefix;
        const String message = probeMessage(denied, rights);
        EXPECT_NE(message.find("a LIST of 'p/blob-intents/'"), String::npos) << message;
        EXPECT_NE(message.find("grant LIST on 'p/blob-intents/'"), String::npos) << message;
        for (const JournalEntry & e : denied.journal())
            EXPECT_NE(e.verb, JournalVerb::Write) << "the probe wrote before it knew GC can list the prefix";
    }
}

/// A local store lists the whole subtree whatever the limit, and grants no right by prefix: the probe of a pool with
/// intents does not list `blob-intents/` itself there, and still proves the rights through its own sub-prefix.
TEST(CASProbe, IntentPrefixProbeOfALocalStoreDoesNotListTheIntents)
{
    for (const IntentPrefixRights rights : {IntentPrefixRights::ReadWrite, IntentPrefixRights::Read})
    {
        SCOPED_TRACE(rights == IntentPrefixRights::Read ? "read" : "read-write");
        IntentRightsBackend backend;
        ASSERT_EQ(backend.dialect(), Dialect::Emulated);
        const Layout layout("p");
        {
            DB::Cas::tests::OperationForTest op(backend);
            for (const char digit : {'a', 'b', 'c'})
            {
                const BlobRef ref{BlobHashAlgo::CityHash128, codecFor(BlobHashAlgo::CityHash128).fromHex(String(32, digit))};
                const String key = layout.blobIntentKey(ref, *canonicalIdentity(Dialect::Emulated, "1"));
                ASSERT_TRUE(std::holds_alternative<Committed>((*op).create(key, blobIntentRecord(), Retry::standard()))) << key;
            }
        }
        backend.clearJournal();

        EXPECT_EQ(probeMessage(backend, rights), "");
        bool listed_sub_prefix = false;
        for (const JournalEntry & e : backend.journal())
        {
            if (e.verb != JournalVerb::List)
                continue;
            EXPECT_NE(e.keys.front(), layout.blobIntentsPrefix()) << "the probe listed the intents of a local store";
            listed_sub_prefix = listed_sub_prefix || e.keys.front() == layout.blobIntentsProbePrefix();
        }
        EXPECT_TRUE(listed_sub_prefix) << "positive control: the probe listed its own sub-prefix";

        backend.deny = IntentRightsBackend::Deny::List;
        EXPECT_NE(probeMessage(backend, rights).find("grant LIST on 'p/blob-intents/'"), String::npos)
            << "a store that refuses LIST under the prefix is still refused";
    }
}

TEST(CASProbe, IntentPrefixReadProbeWritesNothing)
{
    IntentRightsBackend backend;
    EXPECT_EQ(probeMessage(backend, IntentPrefixRights::Read), "");
    bool listed = false;
    for (const JournalEntry & e : backend.journal())
    {
        EXPECT_TRUE(e.verb == JournalVerb::Read || e.verb == JournalVerb::List) << static_cast<int>(e.verb);
        listed = listed || e.verb == JournalVerb::List;
    }
    EXPECT_TRUE(listed) << "positive control: the read side listed the prefix";

    backend.deny = IntentRightsBackend::Deny::Get;
    EXPECT_NE(probeMessage(backend, IntentPrefixRights::Read).find("cannot tell an absent intent from a denied one"), String::npos);
}

TEST(CASProbe, IntentProbeKeyIsNeverAnIntentKey)
{
    const Layout layout("p");
    const String scratch = layout.blobIntentsProbePrefix() + "0123456789abcdef0123456789abcdef/intent";
    EXPECT_TRUE(layout.isBlobIntentProbeKey(scratch));
    EXPECT_FALSE(layout.parseBlobIntentKey(scratch));
    /// Positive control: a real intent key parses, and is no probe key.
    const BlobRef ref{BlobHashAlgo::CityHash128, codecFor(BlobHashAlgo::CityHash128).fromHex(String(32, 'a'))};
    const String intent = layout.blobIntentKey(ref, *canonicalIdentity(Dialect::ETag, "\"abc\""));
    EXPECT_TRUE(layout.parseBlobIntentKey(intent));
    EXPECT_FALSE(layout.isBlobIntentProbeKey(intent));
    for (const auto & entry : kBlobHashAlgoWords.entries)
        EXPECT_NE(entry.word, "_probe") << "a hash algorithm segment would collide with the probe's sub-prefix";
}
