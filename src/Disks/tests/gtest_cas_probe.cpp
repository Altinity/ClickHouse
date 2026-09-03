#include <gtest/gtest.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Backend/CasFence.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Backend/CasInMemoryBackend.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Backend/CasObjectStorageBackend.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Backend/CasProbe.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Backend/CasRequests.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasPool.h>
#include <Disks/tests/cas_test_helpers.h>
#include <Common/Exception.h>

using namespace DB::Cas;

namespace
{

/// Every test here constructs one backend and runs the battery against it once; a non-owning
/// `BackendPtr` over the test's stack- or shared_ptr-held backend keeps that construction pattern
/// rather than forcing a second allocation. The open fence never trips: `runCapabilityProbe` runs
/// during pool bootstrap, before any mount fence exists to enforce.
CasRequests makeRequests(Backend & backend)
{
    return CasRequests(BackendPtr(&backend, [](Backend *) {}), Fence::open());
}

}

TEST(CASProbe, PassesOnEnforcingBackend)
{
    auto b = std::make_shared<InMemoryBackend>();
    auto requests = makeRequests(*b);
    auto op = requests.admit();
    EXPECT_NO_THROW(runCapabilityProbe(op, "p/.cas_probe"));
    EXPECT_TRUE(b->list("p/.cas_probe", "", 10).keys.empty());   // probe cleans up after itself
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
    EXPECT_THROW(runCapabilityProbe(op, "p/.cas_probe"), DB::Exception);
}

TEST(CASProbe, PassesOnEmulatedLocal)
{
    auto b = std::make_shared<ObjectStorageBackend>(
        DB::Cas::tests::makeLocalObjectStorageForTest(), ObjectStorageBackend::Mode::EmulatedSingleProcess);
    auto requests = makeRequests(*b);
    auto op = requests.admit();
    EXPECT_NO_THROW(runCapabilityProbe(op, "p/.cas_probe"));
}

/// B135: two servers mounting the SAME shared CA pool concurrently must not race on the probe keys.
/// We simulate "a concurrent mounter's probe is in flight" by PRE-SEEDING the fixed-name probe key
/// `<pool>/_probe/token` over a shared backend, then opening the Pool. With the OLD fixed-key probe
/// the open's `putIfAbsent("<pool>/_probe/token", …)` returns PreconditionFailed and `Pool::open`
/// throws NOT_IMPLEMENTED ("putIfAbsent on a fresh key returned PreconditionFailed"). With the
/// per-mount unique probe prefix `<pool>/_probe/<rand>/token`, the seeded key does not collide and
/// the open succeeds — exactly the concurrent-shared-pool-mount behaviour we need.
///
/// Goes through `Pool::open` (owned elsewhere), so it exercises `runCapabilityProbe` only indirectly
/// and needs no signature change here.
TEST(CASProbe, ConcurrentMountsDoNotCollide)
{
    auto b = std::make_shared<InMemoryBackend>();

    /// Simulate a concurrent mounter whose probe object under the legacy fixed key is still present.
    ASSERT_EQ(b->putIfAbsent("p/_probe/token", "concurrent-mounter-in-flight").outcome, PutOutcome::Done);

    /// A real (second) mount over the same shared pool must still succeed — its probe runs under a
    /// fresh per-mount-unique prefix and never touches the seeded fixed key.
    EXPECT_NO_THROW(Pool::open(b, PoolConfig{.pool_prefix = "p", .server_root_id = "test"}));

    /// And two genuinely-concurrent mounts (distinct unique prefixes) both succeed over one backend.
    EXPECT_NO_THROW(Pool::open(b, PoolConfig{.pool_prefix = "p", .server_root_id = "test"}));

    /// The seeded fixed-key artifact is untouched (the probe never collided with it).
    EXPECT_TRUE(b->get("p/_probe/token").has_value());
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

/// `InMemoryBackend`'s enforcing `remove`/`write` primitives compare opaque string values and never
/// consult a dialect; this decorator claims a different dialect LABEL while delegating everything else
/// unchanged, so the same enforcing backend can be probed under each dialect a live production backend
/// mints. `InMemoryBackend`'s minted values (a monotonically increasing decimal starting at "1") are
/// grammar-valid incarnations under all three: non-empty and comma/`*`-free for ETag, a canonical
/// positive decimal for Generation, merely non-empty for Emulated.
class DialectOverrideBackend : public InMemoryBackend
{
public:
    explicit DialectOverrideBackend(Dialect claimed_dialect_) : claimed_dialect(claimed_dialect_) {}
    Dialect dialect() const override { return claimed_dialect; }

private:
    Dialect claimed_dialect;
};

}

/// The probe's reordered "wrong incarnation" steps (Step 4, 5d, 6) always reuse a REAL, backend-minted
/// `Incarnation` from the same key rather than a synthesized value — see CasProbe.cpp's step comments —
/// so there is no dialect-specific construction left to regress. This exercises the whole battery under
/// each dialect a live production backend can mint, proving the probe's pass/fail verdict does not
/// depend on which one.
TEST(CASProbe, ReorderedProbePassesOnAllThreeDialects)
{
    for (const Dialect dialect : {Dialect::ETag, Dialect::Generation, Dialect::Emulated})
    {
        DialectOverrideBackend b(dialect);
        auto requests = makeRequests(b);
        auto op = requests.admit();
        EXPECT_NO_THROW(runCapabilityProbe(op, "p/.cas_probe")) << "dialect " << static_cast<int>(dialect);
        EXPECT_TRUE(b.list("p/.cas_probe", "", 10).keys.empty()) << "dialect " << static_cast<int>(dialect);
    }
}
