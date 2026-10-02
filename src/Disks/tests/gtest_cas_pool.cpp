#include <gtest/gtest.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasPartWriteTxn.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Formats/CasPoolMetaFormat.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Backend/CasInMemoryBackend.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Formats/CasLayout.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Formats/CasPartManifestFormat.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Primitives/CasTypes.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasServerRoot.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasPool.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Tools/CasFsck.h>
#include <Disks/tests/cas_test_helpers.h>
#include <Common/Exception.h>
#include <Common/CurrentMemoryTracker.h>
#include <Common/CurrentThread.h>
#include <Common/MemoryTracker.h>
#include <base/scope_guard.h>
#include <base/defines.h>
#include <Common/ProfileEvents.h>
#include <Common/logger_useful.h>
#include <Poco/Exception.h>
#include <Poco/StreamChannel.h>
#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <cstdlib>
#include <future>
#include <limits>
#include <mutex>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <thread>
#include <vector>

namespace DB::ErrorCodes
{
extern const int ABORTED;
extern const int BAD_ARGUMENTS;
extern const int CORRUPTED_DATA;
extern const int NOT_IMPLEMENTED;
extern const int UNKNOWN_FORMAT_VERSION;
extern const int FILE_DOESNT_EXIST;
extern const int UNKNOWN_EXCEPTION;
extern const int NETWORK_ERROR;
extern const int MEMORY_LIMIT_EXCEEDED;
extern const int LOGICAL_ERROR;
}

namespace ProfileEvents
{
extern const Event CASRefRecoveryEpochSealed;
extern const Event CASMountExclusivityViolation;
extern const Event CASMountLeaseLost;
extern const Event CASMountReleaseSkippedForeignOccupant;
extern const Event CASRemountAttempts;
extern const Event CASRemountSucceeded;
extern const Event CASRemountFailed;
extern const Event CASMountLeaseExpired;
extern const Event CASMountRenewalAttempts;
extern const Event CASMountRenewalRetries;
}

using namespace DB::Cas;
using DB::Cas::tests::blobEntryFor;
using DB::Cas::tests::expectThrowsCode;
using DB::Cas::tests::idOf;
using DB::Cas::tests::SharedWaitLog;
using DB::Cas::tests::u128Of;

namespace
{
/// Counts mutating backend calls so a test can assert an open path is write-free.
class WriteCountingBackend final : public DB::Cas::Backend
{
public:
    explicit WriteCountingBackend(std::shared_ptr<DB::Cas::Backend> inner_) : inner(std::move(inner_)) {}
    size_t writes = 0;

    bool supportsListTokens() const override { return inner->supportsListTokens(); }

    /// Every write reaches the store through these primitives, so `writes` sees it whichever verb
    /// (`create`/`replace`/`remove`/`publish`) issued it.
    std::optional<Raw> read(const String & key, TransportAccess & access) override { return inner->read(key, access); }
    std::optional<RawMeta> head(const String & key, TransportAccess & access) override { return inner->head(key, access); }
    RawListPage list(const String & prefix, const String & cursor, size_t limit, TransportAccess & access) override { return inner->list(prefix, cursor, limit, access); }
    RawRemoval remove(const String & key, const String & expected_value, TransportAccess & access) override
    {
        ++writes;
        return inner->remove(key, expected_value, access);
    }
    void removeManyWriteOnce(const std::vector<WriteOnceKey> & keys, TransportAccess & access) override
    {
        ++writes;
        inner->removeManyWriteOnce(keys, access);
    }
    std::expected<String, RawConflict> write(const String & key, const String & bytes,
                                             const std::optional<String> & expected_value, TransportAccess & access) override
    {
        ++writes;
        return inner->write(key, bytes, expected_value, access);
    }
    std::unique_ptr<DB::ReadBuffer> stream(const String & key, TransportAccess & access) override { return inner->stream(key, access); }
    void publish(const BlobPublishRequest & request, TransportAccess & access) override
    {
        ++writes;
        inner->publish(request, access);
    }
    Dialect dialect() const override { return inner->dialect(); }
private:
    std::shared_ptr<DB::Cas::Backend> inner;
};

/// A one-shot `create`, asserting it committed (mirrors the retired `backend.putIfAbsent(key, bytes)`).
void createObj(Backend & backend, const String & key, const String & bytes)
{
    DB::Cas::tests::OperationForTest op(backend);
    ASSERT_TRUE(std::holds_alternative<Committed>((*op).create(key, bytes, Retry::once())));
}

/// An exact read (mirrors the retired `backend.get(key)`).
std::optional<Object> readObj(Backend & backend, const String & key)
{
    DB::Cas::tests::OperationForTest op(backend);
    return (*op).read(key, Retry::standard());
}

/// A HEAD (mirrors the retired `backend.head(key)`).
std::optional<Meta> headObj(Backend & backend, const String & key)
{
    DB::Cas::tests::OperationForTest op(backend);
    return (*op).head(key, Retry::standard());
}

/// Publish one part `ref` through the REAL PartWriteTxn write path: stage a manifest holding a single content
/// blob whose payload is `payload`, precommit-add into the owning shard, then promote precommit ->
/// committed. Returns the published ManifestId. This is the canonical write-side fixture for the
/// read-path tests (the same shape as `publishPart` in gtest_cas_gc_log.cpp). The manifest entry path
/// is `data.bin` unless `entry_path` overrides it.
ManifestId publishPart(
    const PoolPtr & s, const String & ns, const String & ref, const String & payload,
    const String & entry_path = "data.bin")
{
    const RootNamespace nsr{ns};
    PartWriteInfo info;
    info.intended_ref = ns + "/" + ref;
    auto build = s->beginPartWrite(info);

    ManifestEntry e;
    e.path = entry_path;
    e.placement = EntryPlacement::Blob;
    e.ref = DB::Cas::BlobRef{DB::Cas::BlobHashAlgo::CityHash128, DB::Cas::BlobDigest::fromU128(u128Of(payload))};

    e.blob_size = payload.size();

    const ManifestId id = build->stageManifest({e});
    build->precommitAdd(nsr, ref, id);
    build->putBlob(idOf(payload), BlobSource::fromString(payload));
    build->promote(nsr, ref, build->buildId(), id);
    return id;
}

/// A ManifestRef carrying a unique instance id derived from `tag` (all fields explicit so the
/// missing-designated-field-initializer warning never fires). The writer/build fields are stable test
/// constants — the read path keys identity by the full ref, so any consistent choice works here.
ManifestRef manifestRefFor(const String & tag)
{
    uint32_t ordinal = 1;
    for (char c : tag)
        ordinal = ordinal * 131 + static_cast<unsigned char>(c);
    ordinal = ordinal % 999999 + 1;
    return ManifestRef{
        .writer_epoch = 1,
        .build_sequence = 1,
        .manifest_ordinal = ordinal};
}

/// Publish a part holding the given manifest entries verbatim through the real PartWriteTxn. Used by read-path
/// lookup/list tests that want a precise multi-entry manifest. Each Blob entry's body MUST be present at
/// promote: the promote gate revalidates EVERY blob leaf with a HEAD and fails closed on an absent body.
/// So write a blob body for each Blob entry (addressed by its hash) and record it as W-EVIDENCE before
/// staging. Inline entries need no body. Returns the published ManifestId.
ManifestId publishPartWithEntries(
    const PoolPtr & s, const String & ns, const String & ref, std::vector<ManifestEntry> entries)
{
    const RootNamespace nsr{ns};
    PartWriteInfo info;
    info.intended_ref = ns + "/" + ref;
    auto build = s->beginPartWrite(info);
    for (const auto & e : entries)
        if (e.placement == EntryPlacement::Blob)
        {
            /// Materialize the blob body so the promote-time HEAD revalidation succeeds, then record the
            /// tokenless W-EVIDENCE dep (the gate re-observes the current token at promote).
            DB::Cas::tests::writeBlobBody(*s->poolBackendPtr(), s->layout(), e.ref.digest.toU128());
            build->adoptEvidence(e);
        }
    const ManifestId id = build->stageManifest(std::move(entries));
    build->precommitAdd(nsr, ref, id);
    build->promote(nsr, ref, build->buildId(), id);
    return id;
}
}

TEST(CASPool, ReadOnlyOpenSkipsProbe)
{
    auto shared = std::make_shared<DB::Cas::InMemoryBackend>();

    DB::Cas::PoolConfig cfg;
    cfg.pool_prefix = "pool";
    cfg.server_id = DB::UInt128(1);
    cfg.server_root_id = "test";
    /// Writable open: creates _pool_meta and runs the probe (which writes+cleans up).
    DB::Cas::Pool::open(std::make_shared<WriteCountingBackend>(shared), cfg);

    /// Read-only re-open over the SAME data must perform ZERO writes (no probe, meta already present).
    auto counter = std::make_shared<WriteCountingBackend>(shared);
    DB::Cas::PoolConfig ro = cfg;
    ro.read_only = true;
    auto store = DB::Cas::Pool::open(counter, ro);
    EXPECT_EQ(counter->writes, 0u);
    ASSERT_NE(store, nullptr);
}

namespace
{
/// Records whether any MUTATING op touched a `_probe/` key, so a test can assert an open ran (or
/// skipped) the capability probe. Mirrors WriteCountingBackend above but keys on the probe subtree.
class ProbeWatchingBackend final : public DB::Cas::Backend
{
public:
    explicit ProbeWatchingBackend(std::shared_ptr<DB::Cas::Backend> inner_) : inner(std::move(inner_)) {}
    bool probe_touched = false;

    bool supportsListTokens() const override { return inner->supportsListTokens(); }

    /// Every mutation reaches the store through these primitives, so a probe-key touch is noted
    /// whichever verb (`create`/`replace`/`remove`/`publish`) issued it.
    std::optional<Raw> read(const String & key, TransportAccess & access) override { return inner->read(key, access); }
    std::optional<RawMeta> head(const String & key, TransportAccess & access) override { return inner->head(key, access); }
    RawListPage list(const String & prefix, const String & cursor, size_t limit, TransportAccess & access) override { return inner->list(prefix, cursor, limit, access); }
    RawRemoval remove(const String & key, const String & expected_value, TransportAccess & access) override
    {
        note(key);
        return inner->remove(key, expected_value, access);
    }
    void removeManyWriteOnce(const std::vector<WriteOnceKey> & keys, TransportAccess & access) override
    {
        for (const WriteOnceKey & key : keys)
            note(key.str());
        inner->removeManyWriteOnce(keys, access);
    }
    std::expected<String, RawConflict> write(const String & key, const String & bytes,
                                             const std::optional<String> & expected_value, TransportAccess & access) override
    {
        note(key);
        return inner->write(key, bytes, expected_value, access);
    }
    std::unique_ptr<DB::ReadBuffer> stream(const String & key, TransportAccess & access) override { return inner->stream(key, access); }
    void publish(const BlobPublishRequest & request, TransportAccess & access) override
    {
        note(request.destination_key);
        inner->publish(request, access);
    }
    Dialect dialect() const override { return inner->dialect(); }
private:
    void note(const String & k) { if (k.find("/_probe/") != String::npos) probe_touched = true; }
    std::shared_ptr<DB::Cas::Backend> inner;
};
}

TEST(CASPool, SkipAccessCheckOpenSkipsProbeButStaysWritable)
{
    auto shared = std::make_shared<DB::Cas::InMemoryBackend>();

    DB::Cas::PoolConfig cfg;
    cfg.pool_prefix = "pool";
    cfg.server_id = DB::UInt128(1);
    cfg.server_root_id = "srv-1";

    /// Baseline: a normal writable open runs the capability probe (PUT+delete of `_probe/` keys).
    {
        auto watch = std::make_shared<ProbeWatchingBackend>(shared);
        auto s = DB::Cas::Pool::open(watch, cfg);
        ASSERT_NE(s, nullptr);
        EXPECT_TRUE(watch->probe_touched) << "the probe must run by default";
    }

    /// skip_access_check open ("start now, fix later"): NO probe I/O, yet still a WRITABLE mount
    /// (owner/epoch/mount/watermark bootstrap writes still happen — unlike a read_only open, which is
    /// a total no-op). Distinct root over the same (now-created) pool.
    {
        auto watch = std::make_shared<ProbeWatchingBackend>(shared);
        DB::Cas::PoolConfig sac = cfg;
        sac.server_id = DB::UInt128(2);
        sac.server_root_id = "srv-2";
        sac.skip_access_check = true;
        auto s = DB::Cas::Pool::open(watch, sac);
        ASSERT_NE(s, nullptr);
        EXPECT_FALSE(watch->probe_touched) << "skip_access_check must perform no probe I/O";

        /// Prove the mount is genuinely WRITABLE, not merely non-null — a read_only open would also
        /// satisfy the two assertions above. Publish a part through the real PartWriteTxn write path
        /// (beginPartWrite/putBlob/stageManifest/precommitAdd/promote) and read it back.
        publishPart(s, "srv-2/tbl", "part_1", "payload-x");
        const auto r = s->resolveRef(DB::Cas::RootNamespace{"srv-2/tbl"}, "part_1");
        ASSERT_TRUE(r.has_value()) << "skip_access_check open must accept real writes, not just open";
    }
}

namespace
{
/// Delegates every storage operation to `inner` and leaves the mount-time capability gates at their
/// permissive defaults, so a subclass can make exactly ONE gate throw and a test can attribute a
/// refused mount to that gate alone.
class ForwardingBackend : public DB::Cas::Backend
{
public:
    explicit ForwardingBackend(std::shared_ptr<DB::Cas::Backend> inner_) : inner(std::move(inner_)) {}

    bool supportsListTokens() const override { return inner->supportsListTokens(); }

    /// The transport primitives forward to `inner`. Declared because `Backend` declares them pure.
    std::optional<Raw> read(const String & key, TransportAccess & access) override { return inner->read(key, access); }
    std::optional<RawMeta> head(const String & key, TransportAccess & access) override { return inner->head(key, access); }
    RawListPage list(const String & prefix, const String & cursor, size_t limit, TransportAccess & access) override { return inner->list(prefix, cursor, limit, access); }
    RawRemoval remove(const String & key, const String & expected_value, TransportAccess & access) override { return inner->remove(key, expected_value, access); }
    void removeManyWriteOnce(const std::vector<WriteOnceKey> & keys, TransportAccess & access) override { inner->removeManyWriteOnce(keys, access); }
    std::expected<String, RawConflict> write(const String & key, const String & bytes,
                                             const std::optional<String> & expected_value, TransportAccess & access) override
    {
        return inner->write(key, bytes, expected_value, access);
    }
    std::unique_ptr<DB::ReadBuffer> stream(const String & key, TransportAccess & access) override { return inner->stream(key, access); }
    void publish(const BlobPublishRequest & request, TransportAccess & access) override { inner->publish(request, access); }
    Dialect dialect() const override { return inner->dialect(); }

private:
    std::shared_ptr<DB::Cas::Backend> inner;
};

/// A backend whose checkConditionalWriteSingleAttemptSupport ALWAYS throws — a stand-in for a
/// Native-mode backend with no working single-attempt client (see
/// ObjectStorageBackend::checkConditionalWriteSingleAttemptSupport). Pins that skip_access_check does
/// NOT bypass this gate: the regression this guards is reverting Pool::open's skip_access_check
/// branch back to the naive "wrap the whole probe" shape, which would silently skip this check too.
class ThrowingSingleAttemptBackend final : public ForwardingBackend
{
public:
    using ForwardingBackend::ForwardingBackend;

    void checkConditionalWriteSingleAttemptSupport() override
    {
        throw DB::Exception(DB::ErrorCodes::NOT_IMPLEMENTED, "test: no single-attempt client");
    }
};

/// A backend whose store-level preconditions refuse the pool outright — a stand-in for a versioning or
/// dialect combination `ObjectStorageBackend::checkPoolPreconditions` rejects.
class ThrowingPoolPreconditionsBackend final : public ForwardingBackend
{
public:
    using ForwardingBackend::ForwardingBackend;

    void checkPoolPreconditions() override
    {
        throw DB::Exception(DB::ErrorCodes::NOT_IMPLEMENTED, "test: pool preconditions refused");
    }
};

/// A backend that forbids skipping the access-check battery — a stand-in for the writable
/// generation-dialect (GCS) backend (see ObjectStorageBackend::checkSkipAccessCheckSupport).
class ThrowingSkipAccessCheckBackend final : public ForwardingBackend
{
public:
    using ForwardingBackend::ForwardingBackend;

    void checkSkipAccessCheckSupport() override
    {
        throw DB::Exception(DB::ErrorCodes::NOT_IMPLEMENTED, "test: this backend forbids skip_access_check");
    }
};

DB::Cas::PoolConfig writablePoolConfigForTest()
{
    DB::Cas::PoolConfig cfg;
    cfg.pool_prefix = "pool";
    cfg.server_id = DB::UInt128(1);
    cfg.server_root_id = "test";
    return cfg;
}
}

TEST(CASPool, SkipAccessCheckStillEnforcesSingleAttemptGate)
{
    auto backend = std::make_shared<ThrowingSingleAttemptBackend>(std::make_shared<DB::Cas::InMemoryBackend>());

    DB::Cas::PoolConfig cfg = writablePoolConfigForTest();
    cfg.skip_access_check = true;

    /// skip_access_check must NOT bypass checkConditionalWriteSingleAttemptSupport (RFC
    /// cas-s3-timeout-retry-control): a writable open still refuses to mount on a backend that cannot
    /// prove single-attempt conditional-write support, exactly as it does without skip_access_check.
    EXPECT_THROW(DB::Cas::Pool::open(backend, cfg), DB::Exception);
}

/// A backend that forbids skipping the battery refuses the writable mount outright. Asserting the
/// gate's own message, not merely that open threw: Pool::open has many other refusals, and a mount
/// that failed for one of those would satisfy a bare EXPECT_THROW.
TEST(CASPool, SkipAccessCheckRefusedByBackendFailsTheWritableMount)
{
    auto backend = std::make_shared<ThrowingSkipAccessCheckBackend>(std::make_shared<DB::Cas::InMemoryBackend>());

    DB::Cas::PoolConfig cfg = writablePoolConfigForTest();
    cfg.skip_access_check = true;

    try
    {
        DB::Cas::Pool::open(backend, cfg);
        FAIL() << "expected the skip_access_check gate to refuse the mount";
    }
    catch (const DB::Exception & e)
    {
        EXPECT_EQ(e.code(), DB::ErrorCodes::NOT_IMPLEMENTED);
        EXPECT_NE(e.message().find("forbids skip_access_check"), std::string::npos) << "actual message: " << e.message();
    }
}

/// The discriminator for the test above: the SAME backend opens fine without the flag, so that
/// refusal came from the new gate rather than from anything else in the open path. It also pins the
/// gate's scope — it is consulted only where skip_access_check is honoured, so a mount that runs the
/// battery is unaffected.
TEST(CASPool, BackendForbiddingSkipAccessCheckStillOpensWhenTheBatteryRuns)
{
    auto backend = std::make_shared<ThrowingSkipAccessCheckBackend>(std::make_shared<DB::Cas::InMemoryBackend>());

    DB::Cas::PoolConfig cfg = writablePoolConfigForTest();
    cfg.background_watermark = false;
    ASSERT_FALSE(cfg.skip_access_check);

    auto store = DB::Cas::Pool::open(backend, cfg);
    ASSERT_NE(store, nullptr);
}

/// The ORDINARY writable mount -- the one that runs the battery -- must still be refused by the two
/// store-level gates. They used to be the capability probe's own first two steps; they are the caller's
/// now, and nothing else in the open path would notice if the caller stopped asking. The write counter is
/// what makes each of these a fence rather than a bare `EXPECT_THROW`: `Pool::open` refuses for many
/// reasons, but only a refusal BEFORE the battery leaves the store unwritten.
TEST(CASPool, WritableOpenRunsThePoolPreconditionGateBeforeTheBattery)
{
    auto counting = std::make_shared<WriteCountingBackend>(std::make_shared<DB::Cas::InMemoryBackend>());
    auto backend = std::make_shared<ThrowingPoolPreconditionsBackend>(counting);

    DB::Cas::PoolConfig cfg = writablePoolConfigForTest();
    ASSERT_FALSE(cfg.skip_access_check) << "this test is about the branch that RUNS the battery";

    try
    {
        DB::Cas::Pool::open(backend, cfg);
        FAIL() << "expected the pool-precondition gate to refuse the mount";
    }
    catch (const DB::Exception & e)
    {
        EXPECT_EQ(e.code(), DB::ErrorCodes::NOT_IMPLEMENTED);
        EXPECT_NE(e.message().find("pool preconditions refused"), std::string::npos)
            << "actual message: " << e.message();
    }
    EXPECT_EQ(counting->writes, 0u) << "the gate must refuse before the battery writes anything";
}

TEST(CASPool, WritableOpenRunsTheSingleAttemptGateBeforeTheBattery)
{
    auto counting = std::make_shared<WriteCountingBackend>(std::make_shared<DB::Cas::InMemoryBackend>());
    auto backend = std::make_shared<ThrowingSingleAttemptBackend>(counting);

    DB::Cas::PoolConfig cfg = writablePoolConfigForTest();
    ASSERT_FALSE(cfg.skip_access_check) << "this test is about the branch that RUNS the battery";

    try
    {
        DB::Cas::Pool::open(backend, cfg);
        FAIL() << "expected the single-attempt gate to refuse the mount";
    }
    catch (const DB::Exception & e)
    {
        EXPECT_EQ(e.code(), DB::ErrorCodes::NOT_IMPLEMENTED);
        EXPECT_NE(e.message().find("no single-attempt client"), std::string::npos)
            << "actual message: " << e.message();
    }
    EXPECT_EQ(counting->writes, 0u) << "the gate must refuse before the battery writes anything";
}

/// The positive control for the two above: with no gate refusing, the same open DOES write. Without it
/// `writes == 0` would be satisfied by an open that refused for any earlier reason, and both fences would
/// pass while the gates were gone.
TEST(CASPool, WritableOpenWithoutAGateRefusalDoesReachTheBattery)
{
    auto counting = std::make_shared<WriteCountingBackend>(std::make_shared<DB::Cas::InMemoryBackend>());

    DB::Cas::PoolConfig cfg = writablePoolConfigForTest();
    cfg.background_watermark = false;
    ASSERT_FALSE(cfg.skip_access_check);

    auto store = DB::Cas::Pool::open(counting, cfg);
    ASSERT_NE(store, nullptr);
    EXPECT_GT(counting->writes, 0u);
}

TEST(CASPool, MinActiveTracksInFlightBuilds)
{
    auto backend = std::make_shared<DB::Cas::InMemoryBackend>();
    DB::Cas::PoolConfig cfg;
    cfg.pool_prefix = "pool";
    cfg.server_id = DB::UInt128(1);
    cfg.server_root_id = "test";
    cfg.background_watermark = false;
    auto store = DB::Cas::Pool::open(backend, cfg);

    ASSERT_EQ(store->minActive(), store->peekNextBuildSeq());   /// no builds: floor == next seq
    auto b1 = store->beginPartWrite({});                            /// seq 1
    auto b2 = store->beginPartWrite({});                            /// seq 2
    ASSERT_EQ(store->minActive(), 1u);
    b1->abandon();                                              /// finishes seq 1
    ASSERT_EQ(store->minActive(), 2u);                          /// floor advances
    b2->abandon();
    ASSERT_EQ(store->minActive(), store->peekNextBuildSeq());   /// empty again
}

/// A throwing audit sink must NOT break a storage operation. The single reentrancy-safe event
/// dispatcher (stage-1 §1, Task 2) CONTAINS sink exceptions ("never throws through"), so an arbitrary
/// observer/sink callback failing during `beginPartWrite` is swallowed and construction succeeds --
/// consistent with `CASPartWriteTxn.AbandonSwallowsThrowingEventSink` and
/// `PromoteSwallowsPostDurableEventSinkFailure`, which already establish that an audit-sink failure
/// never aborts the operation. Before Task 2 the sink was invoked directly and its exception
/// propagated out of construction (audit-log backpressure breaking a write); the dispatcher removes
/// that. The build_seq lifecycle is still exercised: the in-flight build holds the `minActive` GC
/// floor and is retired on `abandon`.
TEST(CASPool, BeginPartWriteSwallowsThrowingEventSink)
{
    auto backend = std::make_shared<DB::Cas::InMemoryBackend>();
    DB::Cas::PoolConfig cfg;
    cfg.pool_prefix = "pool";
    cfg.server_id = DB::UInt128(1);
    cfg.server_root_id = "test";
    cfg.background_watermark = false;
    auto store = DB::Cas::Pool::open(backend, cfg);

    const uint64_t next_seq = store->peekNextBuildSeq();
    /// UNKNOWN_EXCEPTION (not LOGICAL_ERROR): this simulates an arbitrary observer/sink callback
    /// failing, not a CAS invariant violation -- LOGICAL_ERROR would abort the whole process under
    /// debug/sanitizer builds instead of behaving like a catchable exception.
    store->setEventSink([](const CasEvent & e)
    {
        if (e.type == CasEventType::BuildStart)
            throw DB::Exception(DB::ErrorCodes::UNKNOWN_EXCEPTION, "injected audit sink failure");
    });

    PartWriteTxnPtr build;
    ASSERT_NO_THROW({ build = store->beginPartWrite({}); })
        << "a throwing audit sink must be contained by the dispatcher, not fail construction";
    store->setEventSink(nullptr);

    EXPECT_EQ(build->buildSeq(), next_seq);
    EXPECT_EQ(store->peekNextBuildSeq(), next_seq + 1);
    EXPECT_EQ(store->minActive(), build->buildSeq());              /// the in-flight build holds the floor
    build->abandon();
    EXPECT_EQ(store->minActive(), store->peekNextBuildSeq());      /// retired on abandon
}

TEST(CASPool, BuildSeqIsStrictlyMonotone)
{
    auto backend = std::make_shared<DB::Cas::InMemoryBackend>();
    DB::Cas::PoolConfig cfg;
    cfg.pool_prefix = "pool";
    cfg.server_id = DB::UInt128(1);
    cfg.server_root_id = "test";
    cfg.background_watermark = false;
    auto store = DB::Cas::Pool::open(backend, cfg);
    auto a = store->beginPartWrite({});
    auto sa = a->buildSeq();
    a->abandon();
    auto b = store->beginPartWrite({});
    ASSERT_GT(b->buildSeq(), sa);                               /// never reused, never lower
}

TEST(CASPoolMeta, CreateThenReopen)
{
    auto b = std::make_shared<InMemoryBackend>();
    Layout layout("p");
    PoolMeta created = PoolMeta::createOrValidate(*DB::Cas::tests::OperationForTest(b), layout, /*blob_header_len*/ 256,
        BlobHashAlgo::CityHash128, /*allow_new*/ false, /*allow_mint*/ true);
    EXPECT_NE(created.pool_id, UInt128{});
    PoolMeta reopened = PoolMeta::createOrValidate(*DB::Cas::tests::OperationForTest(b), layout, /*blob_header_len*/ 512);
    EXPECT_EQ(reopened.pool_id, created.pool_id);     /// pool is authoritative — config ignored on reopen
    EXPECT_EQ(reopened.blob_header_len, 256u);
}

TEST(CASPoolMeta, FailClosed)
{
    Layout layout("p");
    /// Garbage bytes are not a valid cas_pool_meta text object => CORRUPTED_DATA at the header line
    /// (createOrValidate path). The future-version fail-closed (v > G_BUILD => UNKNOWN_FORMAT_VERSION)
    /// is exercised at the codec level by the battery's per-row v+1 gate.
    auto b2 = std::make_shared<InMemoryBackend>();
    createObj(*b2, layout.poolMetaKey(), "garbage");
    expectThrowsCode(DB::ErrorCodes::CORRUPTED_DATA,
        [&] { PoolMeta::createOrValidate(*DB::Cas::tests::OperationForTest(b2), layout, 256); });
}

TEST(CASPoolMeta, RoundTripAndReadability)
{
    PoolMeta pm;
    pm.pool_id = hexToU128("0123456789abcdeffedcba9876543210");
    pm.blob_header_len = 256;
    pm.algos_used = {static_cast<uint8_t>(BlobHashAlgo::CityHash128)};

    const String encoded = encodePoolMeta(pm);
    /// v3 text form: a header line + one JSON body object, human-readable (jq/less friendly). No binary
    /// magic; the object starts with '{' and names its type so a reader can identify it by eye.
    ASSERT_GE(encoded.size(), 8u);
    EXPECT_EQ(encoded.front(), '{');
    EXPECT_NE(encoded.find(String("cas_pool_meta")), String::npos);
    EXPECT_EQ(encoded.find(String("CAPM")), String::npos);

    PoolMeta decoded = decodePoolMeta(encoded);
    EXPECT_EQ(decoded.pool_id, pm.pool_id);
    EXPECT_EQ(decoded.blob_header_len, pm.blob_header_len);
}

TEST(CASPoolMeta, RejectsBadConstantsAtCreation)
{
    auto b = std::make_shared<InMemoryBackend>();
    Layout layout("p");

    /// not 8-aligned (above the floor, so it is the alignment rule that rejects it)
    expectThrowsCode(DB::ErrorCodes::BAD_ARGUMENTS,
        [&] { PoolMeta::createOrValidate(*DB::Cas::tests::OperationForTest(b), layout, 250); });
    /// below the v3 envelope floor (240) but 8-aligned: rejected by the floor, not the alignment rule.
    /// Without the raised floor this pool would pass creation and LOGICAL_ERROR on the first blob write.
    expectThrowsCode(DB::ErrorCodes::BAD_ARGUMENTS,
        [&] { PoolMeta::createOrValidate(*DB::Cas::tests::OperationForTest(b), layout, 128); });
    /// well below the floor
    expectThrowsCode(DB::ErrorCodes::BAD_ARGUMENTS,
        [&] { PoolMeta::createOrValidate(*DB::Cas::tests::OperationForTest(b), layout, 64); });
    /// above the 16 KiB ceiling
    expectThrowsCode(DB::ErrorCodes::BAD_ARGUMENTS,
        [&] { PoolMeta::createOrValidate(*DB::Cas::tests::OperationForTest(b), layout, 17 * 1024); });

    /// A creation that fails config validation must not have written anything.
    EXPECT_FALSE(readObj(*b, layout.poolMetaKey()).has_value());
}

TEST(CASPoolMeta, RejectsBadConstantsOnDecode)
{
    auto b = std::make_shared<InMemoryBackend>();
    Layout layout("p");
    /// Encode a PoolMeta with blob_header_len=100 (not 8-aligned); decode must reject it as CORRUPTED_DATA.
    PoolMeta bad_pm;
    bad_pm.pool_id = hexToU128("00000000000000000000000000000001");
    bad_pm.blob_header_len = 100;   /// violates 8-alignment invariant
    bad_pm.algos_used = {static_cast<uint8_t>(BlobHashAlgo::CityHash128)};
    createObj(*b, layout.poolMetaKey(), encodePoolMeta(bad_pm));
    expectThrowsCode(DB::ErrorCodes::CORRUPTED_DATA,
        [&] { PoolMeta::createOrValidate(*DB::Cas::tests::OperationForTest(b), layout, 256); });
}

TEST(CASPoolMeta, DecodeGarbageFails)
{
    /// Any non-CAPM framing byte sequence => CORRUPTED_DATA.
    expectThrowsCode(DB::ErrorCodes::CORRUPTED_DATA, [] { decodePoolMeta(String("garbage")); });
    expectThrowsCode(DB::ErrorCodes::CORRUPTED_DATA, [] { decodePoolMeta(String("")); });
}

TEST(CASPoolMeta, ConcurrentCreateRace)
{
    auto b = std::make_shared<InMemoryBackend>();
    Layout layout("p");

    /// A racing creator already wrote a valid foreign pool_id. createOrValidate must NOT overwrite it:
    /// it re-reads (after losing the create-if-absent CAS, or seeing it present) and returns the
    /// foreign pool_id, validated like a reopen.
    const UInt128 foreign = hexToU128("0123456789abcdeffedcba9876543210");
    PoolMeta foreign_pm;
    foreign_pm.pool_id = foreign;
    foreign_pm.blob_header_len = 256;
    foreign_pm.algos_used = {static_cast<uint8_t>(BlobHashAlgo::CityHash128)};
    createObj(*b, layout.poolMetaKey(), encodePoolMeta(foreign_pm));

    PoolMeta result = PoolMeta::createOrValidate(*DB::Cas::tests::OperationForTest(b), layout, /*blob_header_len*/ 512);
    EXPECT_EQ(result.pool_id, foreign);
    EXPECT_EQ(result.blob_header_len, 256u);     /// the foreign pool's constants win
}

TEST(CASPoolMeta, CasConflictReReadsWinner)
{
    /// The subtlest branch: the initial GET sees ABSENT, so createOrValidate proceeds to the
    /// create-if-absent write — and loses, because a racing creator committed in between. The loser
    /// must then re-read and return the WINNER's pool identity, not LOGICAL_ERROR. A single-threaded
    /// `refuseNextWrite` alone cannot exercise this: it returns Conflict without leaving the object
    /// readable, so the re-read would fire the LOGICAL_ERROR guard. We model the real interleaving
    /// with a backend whose write primitive commits the winner's object and THEN reports Conflict --
    /// exactly what the loser observes.
    class RacingBackend : public InMemoryBackend
    {
    public:
        String winner_bytes;
        /// The fault sits on the WRITE PRIMITIVE: the create-if-absent this models is issued there.
        std::expected<String, RawConflict> write(const String & key, const String & bytes,
            const std::optional<String> & expected_value, TransportAccess & access) override
        {
            if (!winner_committed && !expected_value)
            {
                winner_committed = true;
                /// The winner lands first; our create-if-absent now necessarily conflicts.
                (void)InMemoryBackend::write(key, winner_bytes, std::nullopt, access);
                return std::unexpected(RawConflict{});
            }
            return InMemoryBackend::write(key, bytes, expected_value, access);
        }
    private:
        bool winner_committed = false;
    };

    const UInt128 winner = hexToU128("0123456789abcdeffedcba9876543210");
    PoolMeta winner_pm;
    winner_pm.pool_id = winner;
    winner_pm.blob_header_len = 256;
    winner_pm.algos_used = {static_cast<uint8_t>(BlobHashAlgo::CityHash128)};

    auto b = std::make_shared<RacingBackend>();
    b->winner_bytes = encodePoolMeta(winner_pm);
    Layout layout("p");

    /// Our config (512) is what we WOULD have minted, but we lose the race and inherit the winner.
    PoolMeta result = PoolMeta::createOrValidate(*DB::Cas::tests::OperationForTest(b), layout, /*blob_header_len*/ 512,
        BlobHashAlgo::CityHash128, /*allow_new*/ false, /*allow_mint*/ true);
    EXPECT_EQ(result.pool_id, winner);
    EXPECT_EQ(result.blob_header_len, 256u);
}

TEST(CASPool, OpenFailsClosedOnNonEnforcingBackend)
{
    auto b = std::make_shared<InMemoryBackend>();
    b->setEnforceTokens(false);
    expectThrowsCode(DB::ErrorCodes::NOT_IMPLEMENTED,
        [&] { Pool::open(b, PoolConfig{.pool_prefix = "p", .server_root_id = "test"}); });   /// the probe error contract
}

TEST(CASPool, OpenCreatesPoolMetaAndReopens)
{
    auto b = std::make_shared<InMemoryBackend>();
    /// Two CONCURRENT opens over the same POOL: a shared pool is the multi-server model, so each
    /// mounts a DISTINCT server_root_id (and a distinct server_id) — same-root same-uuid co-mounting
    /// is correctly fail-closed by the mount-safety protocol. This test only asserts that pool-meta is
    /// pool-authoritative and shared across opens.
    auto s1 = Pool::open(b, PoolConfig{
        .pool_prefix = "p", .server_id = UInt128(1), .server_root_id = "srv-1"});
    auto s2 = Pool::open(b, PoolConfig{
        .pool_prefix = "p", .server_id = UInt128(2), .server_root_id = "srv-2"});
    EXPECT_EQ(s1->poolMeta().pool_id, s2->poolMeta().pool_id);      /// pool authoritative
}

TEST(CASPool, OpenWithExplicitConstantsCreatesThem)
{
    auto b = std::make_shared<InMemoryBackend>();
    auto s = Pool::open(b, PoolConfig{.pool_prefix = "p", .server_root_id = "test", .blob_header_len = 512});
    EXPECT_EQ(s->poolMeta().blob_header_len, 512u);                 /// config applies at creation
}

TEST(CASPool, VerbatimFilesLifecycle)
{
    auto b = std::make_shared<InMemoryBackend>();
    auto s = Pool::open(b, PoolConfig{.pool_prefix = "p", .server_root_id = "test"});
    RootNamespace ns{"srv1/tbl"};
    s->putNamespaceFile(DB::Cas::tests::fixture::fixtureLife(ns), "format_version.txt", "1\n");
    s->putNamespaceFile(DB::Cas::tests::fixture::fixtureLife(ns), "uuid.txt", "abc");
    EXPECT_EQ(s->getNamespaceFile(DB::Cas::tests::fixture::fixtureLife(ns), "format_version.txt"), String("1\n"));
    EXPECT_FALSE(s->getNamespaceFile(DB::Cas::tests::fixture::fixtureLife(ns), "absent").has_value());
    auto names = s->listNamespaceFiles(DB::Cas::tests::fixture::fixtureLife(ns));
    EXPECT_EQ(names, (std::vector<String>{"format_version.txt", "uuid.txt"}));
    s->putNamespaceFile(DB::Cas::tests::fixture::fixtureLife(ns), "uuid.txt", "def");                     /// overwrite allowed (head + putOverwrite)
    EXPECT_EQ(s->getNamespaceFile(DB::Cas::tests::fixture::fixtureLife(ns), "uuid.txt"), String("def"));
}

TEST(CASPool, ListNamespaceFilesEmpty)
{
    auto b = std::make_shared<InMemoryBackend>();
    auto s = Pool::open(b, PoolConfig{.pool_prefix = "p", .server_root_id = "test"});
    RootNamespace ns{"srv1/tbl"};
    EXPECT_TRUE(s->listNamespaceFiles(DB::Cas::tests::fixture::fixtureLife(ns)).empty());
}

/// ---------- read side (spec §6): resolveRef / readManifest / findEntry / entryRange / listRefs ----------

/// Phase 1c read path: a published ref resolves to a ManifestId; readManifest returns the immutable
/// body; locate yields a ranged blob read; an Inline entry has no location. Replaces the old
/// resolveRef().tree_id / readTree round trip (the tree model is gone — a part is a single ManifestId).
TEST(CASPool, ResolveReturnsManifestId)
{
    auto b = std::make_shared<InMemoryBackend>();
    auto s = Pool::open(b, PoolConfig{.pool_prefix = "p", .server_root_id = "test"});
    const RootNamespace ns{"srv1/tbl"};

    /// blob "hello world" + an inline file, published through the real PartWriteTxn write path.
    const String payload = "hello world";
    PartWriteInfo info;
    info.intended_ref = ns.string() + "/part_1";
    auto build = s->beginPartWrite(info);

    ManifestEntry blob_entry;
    blob_entry.path = "data.bin";
    blob_entry.placement = EntryPlacement::Blob;
    blob_entry.ref = DB::Cas::BlobRef{DB::Cas::BlobHashAlgo::CityHash128, DB::Cas::BlobDigest::fromU128(u128Of(payload))};

    blob_entry.blob_size = payload.size();
    ManifestEntry inline_entry;
    inline_entry.path = "small.txt";
    inline_entry.placement = EntryPlacement::Inline;
    inline_entry.inline_bytes = "tiny\n";

    const ManifestId id = build->stageManifest({blob_entry, inline_entry});
    build->precommitAdd(ns, "part_1", id);
    build->putBlob(idOf(payload), BlobSource::fromString(payload));
    build->promote(ns, "part_1", build->buildId(), id);

    auto r = s->resolveRef(ns, "part_1");
    ASSERT_TRUE(r.has_value());
    EXPECT_EQ(r->manifest_id, id);                  /// resolve yields the published ManifestId

    auto manifest = s->readManifest(r->manifest_id);
    ASSERT_EQ(manifest.entries.size(), 2u);

    /// "data.bin" sorts before "small.txt" (canonical path order).
    const auto * data = findEntry(manifest.entries, "data.bin");
    ASSERT_TRUE(data != nullptr);
    auto loc = s->locate(*data);
    EXPECT_EQ(loc.offset, s->poolMeta().blob_header_len);
    EXPECT_EQ(loc.length, payload.size());

    auto bytes = readObj(*b, loc.key);
    ASSERT_TRUE(bytes.has_value());
    /// The located window holds exactly the payload: the envelope header is outside it.
    EXPECT_EQ(bytes->bytes.substr(static_cast<size_t>(loc.offset), static_cast<size_t>(loc.length)), payload);

    const auto * small = findEntry(manifest.entries, "small.txt");
    ASSERT_TRUE(small != nullptr);
    EXPECT_THROW(s->locate(*small), DB::Exception);  /// Inline has no location
}

/// readManifest fail-closes on a body whose self-described `ref`/`root_namespace_id` does NOT match the
/// resolved ManifestId — the ref is addressing the wrong object / a cross-namespace dangle. We stage a
/// body raw (writeManifestRaw, the on-storage write fixture) at a ManifestId, then resolve through a
/// committed binding that names a DIFFERENT ManifestRef pointing at the SAME object key — so the head
/// succeeds, the body decodes, but refMatchesBody fails => CORRUPTED_DATA.
TEST(CASPool, ReadManifestValidatesBodyAndFailsClosed)
{
    auto b = std::make_shared<InMemoryBackend>();
    auto s = Pool::open(b, PoolConfig{.pool_prefix = "p", .server_root_id = "test"});
    const RootNamespace ns{"srv1/tbl"};
    Layout layout("p");

    /// (1) ref/namespace mismatch: the BODY self-describes namespace `srv1/other`, but it is addressed
    /// as a manifest of `srv1/tbl` => manifestNamespaceMatches fails => CORRUPTED_DATA. We craft an id
    /// whose key lives under `srv1/tbl` but whose body carries the foreign namespace.
    {
        const ManifestRef ref = manifestRefFor("mismatch-ns");
        const ManifestId addressed{.root_namespace = ns, .ref = ref};
        /// Encode a body that claims a DIFFERENT namespace than `addressed.root_namespace`.
        PartManifest body;
        body.ref = ref;                                     /// ref matches
        body.root_namespace_id = RootNamespace{"srv1/other"};  /// namespace does NOT
        body.entries = {blobEntryFor("f", u128Of("x"), 1)};
        body.payload_digest = computePayloadDigest(body);
        createObj(*b, layout.manifestKey(addressed), encodePartManifest(body));

        expectThrowsCode(DB::ErrorCodes::CORRUPTED_DATA, [&] { s->readManifest(addressed); });
    }

    /// (2) ref mismatch: the body self-describes a DIFFERENT ManifestRef than the id addressing it =>
    /// refMatchesBody fails => CORRUPTED_DATA.
    {
        const ManifestRef addressed_ref = manifestRefFor("addressed-ref");
        const ManifestRef body_ref = manifestRefFor("body-ref-other");
        const ManifestId addressed{.root_namespace = ns, .ref = addressed_ref};
        PartManifest body;
        body.ref = body_ref;                                /// ref does NOT match `addressed`
        body.root_namespace_id = ns;                        /// namespace matches
        body.entries = {blobEntryFor("f", u128Of("y"), 1)};
        body.payload_digest = computePayloadDigest(body);
        createObj(*b, layout.manifestKey(addressed), encodePartManifest(body));

        expectThrowsCode(DB::ErrorCodes::CORRUPTED_DATA, [&] { s->readManifest(addressed); });
    }

    /// (3) a committed ref naming a manifest with NO body present => readManifest throws
    /// FILE_DOESNT_EXIST (INV-NO-DANGLE surfaced on the read path). resolveRef itself SUCCEEDS — refs
    /// are pure manifest state. A raw ref-log fixture (not the real PartWriteTxn path, which validates the
    /// body exists at promote) is the only way to construct this state.
    {
        const ManifestRef missing_ref = manifestRefFor("never-staged");
        DB::Cas::tests::fixture::writeRefLogRaw(*b, layout, RefLogTxn{ns.string(), RefTxnId{1, 1},
            {DB::Cas::tests::namespaceBirthOp(), DB::Cas::tests::publishCommittedOps("part_dangle", missing_ref)[0],
             DB::Cas::tests::publishCommittedOps("part_dangle", missing_ref)[1]}, std::nullopt});
        DB::Cas::tests::writeRecoverableCkptForRawFixture(*b, layout, ns, RefCkpt{
            .life_epoch = 1,
            .committed_through = RefTxnId{1, 1},
            .checkpoint_snapshot_id = std::nullopt,
            .last_epoch_seal = std::nullopt,
        });

        auto r = s->resolveRef(ns, "part_dangle");
        ASSERT_TRUE(r.has_value());
        expectThrowsCode(DB::ErrorCodes::FILE_DOESNT_EXIST, [&] { s->readManifest(r->manifest_id); });
    }
}

/// findEntry and entryRange over a decoded part manifest's canonical-path-ordered entries.
TEST(CASPool, LookupAndListOverManifestEntries)
{
    auto b = std::make_shared<InMemoryBackend>();
    auto s = Pool::open(b, PoolConfig{.pool_prefix = "p", .server_root_id = "test"});
    const RootNamespace ns{"srv1/tbl"};

    /// A multi-file/multi-directory part: top-level + a projection subdir.
    std::vector<ManifestEntry> entries;
    entries.push_back(blobEntryFor("columns.txt", u128Of("cols"), 4));
    entries.push_back(blobEntryFor("data.bin", u128Of("data"), 8));
    entries.push_back(blobEntryFor("p.proj/data.bin", u128Of("proj-data"), 6));
    entries.push_back(blobEntryFor("p.proj/columns.txt", u128Of("proj-cols"), 5));
    const ManifestId id = publishPartWithEntries(s, ns.string(), "all_1_1_0", entries);

    auto r = s->resolveRef(ns, "all_1_1_0");
    ASSERT_TRUE(r.has_value());
    EXPECT_EQ(r->manifest_id, id);
    auto manifest = s->readManifest(r->manifest_id);
    ASSERT_EQ(manifest.entries.size(), 4u);

    /// findEntry: exact-path hit + miss.
    const auto * hit = findEntry(manifest.entries, "data.bin");
    ASSERT_TRUE(hit != nullptr);
    EXPECT_EQ(hit->ref.digest.toU128(), u128Of("data"));
    EXPECT_TRUE(findEntry(manifest.entries, "no_such_file") == nullptr);

    /// entryRange under "p.proj/" yields exactly the two projection files, in canonical order.
    auto [proj_first, proj_last] = entryRange(manifest.entries, "p.proj/");
    std::vector<ManifestEntry> proj(proj_first, proj_last);
    ASSERT_EQ(proj.size(), 2u);
    EXPECT_EQ(proj[0].path, "p.proj/columns.txt");
    EXPECT_EQ(proj[1].path, "p.proj/data.bin");

    /// The empty prefix lists everything (all four), still in canonical order.
    auto [all_first, all_last] = entryRange(manifest.entries, "");
    std::vector<ManifestEntry> all(all_first, all_last);
    ASSERT_EQ(all.size(), 4u);
    EXPECT_EQ(all[0].path, "columns.txt");
    EXPECT_EQ(all[3].path, "p.proj/data.bin");
}

/// The manifest decode cache is keyed by ManifestId alone: an id is minted once and its body is
/// written once, so one id names one content forever. Resolve+read the same ref twice: the second
/// readManifest is served from the cache with NO request at all. A fresh publish under a DIFFERENT
/// ref name mints a NEW ManifestId, so the cache misses and the body is fetched once.
TEST(CASPool, ManifestCacheIsKeyedById)
{
    auto b = std::make_shared<DB::Cas::tests::CountingBackend>();
    auto s = Pool::open(b, PoolConfig{.pool_prefix = "p", .server_root_id = "test"});
    const RootNamespace ns{"srv1/tbl"};
    Layout layout("p");

    const ManifestId id1 = publishPart(s, ns.string(), "part_1", "payload-1");
    const String key1 = layout.manifestKey(id1);
    b->resetCounts();

    /// First read: a body GET populates the id1 cache entry.
    {
        auto r = s->resolveRef(ns, "part_1");
        ASSERT_TRUE(r.has_value());
        auto m = s->readManifest(r->manifest_id);
        ASSERT_EQ(m.entries.size(), 1u);
    }
    const uint64_t gets_after_first = b->getCount(key1);
    ASSERT_GE(gets_after_first, 1u);               /// the first read DID fetch the body

    /// Second read of the SAME id: the id-keyed cache must serve it — NO additional body GET.
    {
        auto r = s->resolveRef(ns, "part_1");
        ASSERT_TRUE(r.has_value());
        EXPECT_EQ(r->manifest_id, id1);
        auto m = s->readManifest(r->manifest_id);
        ASSERT_EQ(m.entries.size(), 1u);
    }
    EXPECT_EQ(b->getCount(key1), gets_after_first)
        << "second readManifest re-GET the body for the same ManifestId — cache miss";
    EXPECT_EQ(b->headCount(key1), 0u) << "keyed by id alone: no HEAD on a miss or a hit";

    /// A fresh publish under a DIFFERENT ref name mints a NEW ManifestId: the cache (keyed by id) misses.
    /// (Promoting a different manifest over the SAME committed ref is a distinct promote-over-committed
    /// leak that `PartWriteTxn::promote` now forbids — see the CASPromoteRepublish tests.)
    const ManifestId id2 = publishPart(s, ns.string(), "part_2", "payload-2");
    EXPECT_FALSE(id2 == id1);                       /// a new publish never reuses a ManifestId
    const String key2 = layout.manifestKey(id2);

    auto r2 = s->resolveRef(ns, "part_2");
    ASSERT_TRUE(r2.has_value());
    EXPECT_EQ(r2->manifest_id, id2);               /// resolve now sees the new manifest
    auto m2 = s->readManifest(r2->manifest_id);
    ASSERT_EQ(m2.entries.size(), 1u);
    EXPECT_GE(b->getCount(key2), 1u)               /// the new id's body WAS fetched (cache miss)
        << "fresh publish (new ManifestId) should miss the id-keyed manifest cache";
    EXPECT_EQ(b->headCount(key2), 0u);
}

/// Phase 5 (part-folder cache spec): manifest_cache is now a byte-weighted CacheBase LRU instead of a
/// count-only bound, since decoded manifests carry inline bytes and can each be megabytes.
TEST(CASPool, ManifestDecodeCacheIsByteBounded)
{
    auto backend = std::make_shared<DB::Cas::tests::CountingBackend>();
    const DB::Cas::Layout layout("p");
    DB::Cas::tests::seedPoolMetaForRestart(*backend);
    const DB::Cas::RootNamespace ns{"srv/t1"};

    /// 8 manifests x ~1 MiB of inline bytes; a 2 MiB decode-cache bound must hold while every
    /// read stays correct (evicted decodes just re-GET + re-decode).
    std::vector<DB::Cas::ManifestId> ids;
    std::vector<DB::Cas::RefOp> birth_ops{DB::Cas::tests::namespaceBirthOp()};
    for (int i = 0; i < 8; ++i)
    {
        const DB::Cas::ManifestRef ref{.writer_epoch = 1, .build_sequence = static_cast<uint64_t>(i + 1),
                                       .manifest_ordinal = 1};
        DB::Cas::ManifestEntry e;
        e.path = "big.txt";
        e.placement = DB::Cas::EntryPlacement::Inline;
        e.ref = DB::Cas::BlobRef{DB::Cas::BlobHashAlgo::CityHash128, DB::Cas::BlobDigest::fromU128(DB::UInt128(i + 1))};

        e.inline_bytes = String(1 << 20, static_cast<char>('a' + i));
        e.blob_size = e.inline_bytes.size();
        ids.push_back(DB::Cas::tests::writeManifestRaw(*backend, layout, ns, ref, {e}));

        const String ref_name = "part_" + std::to_string(i);
        std::vector<DB::Cas::RefOp> ops = i == 0 ? birth_ops : std::vector<DB::Cas::RefOp>{};
        const auto committed_ops = DB::Cas::tests::publishCommittedOps(ref_name, ref);
        ops.insert(ops.end(), committed_ops.begin(), committed_ops.end());
        DB::Cas::tests::fixture::writeRefLogRaw(*backend, layout, RefLogTxn{ns.string(), RefTxnId{1, static_cast<uint64_t>(i + 1)}, ops, std::nullopt});
    }
    DB::Cas::tests::writeRecoverableCkptForRawFixture(*backend, layout, ns, RefCkpt{
        .life_epoch = 1,
        .committed_through = RefTxnId{1, 8},
        .checkpoint_snapshot_id = std::nullopt,
        .last_epoch_seal = std::nullopt,
    });

    DB::Cas::PoolConfig config{.pool_prefix = "p", .server_root_id = "test"};
    config.manifest_decode_cache_bytes = 2ULL << 20;
    auto store = DB::Cas::Pool::open(backend, std::move(config));

    uint64_t total_gets = 0;
    for (int round = 0; round < 2; ++round)
        for (int i = 0; i < 8; ++i)
        {
            auto resolved = store->resolveRef(ns, "part_" + std::to_string(i));
            ASSERT_TRUE(resolved.has_value());
            auto m = store->readManifestShared(resolved->manifest_id);
            ASSERT_EQ(m->entries.size(), 1u);
            EXPECT_EQ(m->entries[0].inline_bytes[0], static_cast<char>('a' + i));   /// always correct
        }
    for (const auto & id : ids)
        total_gets += backend->getCount(layout.manifestKey(id));

    /// The bound forces re-GETs (16 reads over a 2 MiB window of ~1 MiB decodes cannot all hit),
    /// proving eviction actually happens...
    EXPECT_GT(total_gets, 8u);
    /// ...and the cache reports an in-bound retained size.
    EXPECT_LE(store->manifestDecodeCacheBytesForTest(), 2ULL << 20);
}

TEST(CASPool, ResolveDecodeCacheInvalidatesOnWrite)
{
    /// B113: resolveRef uses a token-validated shard-manifest decode cache. A write to the shard
    /// mints a new token, so a subsequent resolve must observe the change (cache must NOT serve a
    /// stale decoded manifest). Without token invalidation this would still see the dropped ref.
    auto b = std::make_shared<InMemoryBackend>();
    auto s = Pool::open(b, PoolConfig{.pool_prefix = "p", .server_root_id = "test"});
    RootNamespace ns{"srv1/tbl"};

    publishPart(s, ns.string(), "part_1", "payload-1");

    /// First resolve decodes + caches; second is a cache hit — both must see part_1.
    ASSERT_TRUE(s->resolveRef(ns, "part_1").has_value());
    ASSERT_TRUE(s->resolveRef(ns, "part_1").has_value());

    /// Write through the Pool (mutateShard => new shard token), removing part_1.
    s->dropRef(ns, "part_1");

    /// The cache must invalidate on the token change: resolve now reflects the drop.
    EXPECT_FALSE(s->resolveRef(ns, "part_1").has_value());
    EXPECT_TRUE(s->listRefs(ns).empty());
}

TEST(CASPool, ResolveAbsentRefAndAbsentNamespace)
{
    auto b = std::make_shared<InMemoryBackend>();
    auto s = Pool::open(b, PoolConfig{.pool_prefix = "p", .server_root_id = "test"});
    RootNamespace ns{"srv1/tbl"};

    /// A freshly-opened pool has no shard manifests: an absent shard is an empty manifest, so resolve
    /// yields nullopt and listRefs is empty (NOT an error).
    EXPECT_FALSE(s->resolveRef(ns, "anything").has_value());
    EXPECT_TRUE(s->listRefs(ns).empty());
}

TEST(CASPool, ListRefsMergesAllShards)
{
    /// Task 10: refs are no longer sharded (the snapshot+log protocol caches one coherent table state
    /// per namespace, not one manifest per shard) -- this now proves listRefs returns every committed
    /// ref of a table built from a single multi-owner transaction, the closest surviving analogue of
    /// the old "merges refs spread across shards" contract.
    auto b = std::make_shared<InMemoryBackend>();
    auto s = Pool::open(b, PoolConfig{.pool_prefix = "p", .server_root_id = "test"});
    Layout layout("p");
    RootNamespace ns{"srv1/tbl"};

    std::vector<RefOp> ops{DB::Cas::tests::namespaceBirthOp()};
    for (char c = 'a'; c <= 'h'; ++c)
    {
        const String ref(1, c);
        const auto committed_ops = DB::Cas::tests::publishCommittedOps(ref, manifestRefFor("manifest-" + ref));
        ops.insert(ops.end(), committed_ops.begin(), committed_ops.end());
    }
    DB::Cas::tests::fixture::writeRefLogRaw(*b, layout, RefLogTxn{ns.string(), RefTxnId{1, 1}, ops, std::nullopt});
    DB::Cas::tests::writeRecoverableCkptForRawFixture(*b, layout, ns, RefCkpt{
        .life_epoch = 1,
        .committed_through = RefTxnId{1, 1},
        .checkpoint_snapshot_id = std::nullopt,
        .last_epoch_seal = std::nullopt,
    });

    auto refs = s->listRefs(ns);
    ASSERT_EQ(refs.size(), 8u);
    for (char c = 'a'; c <= 'h'; ++c)
    {
        const String ref(1, c);
        ASSERT_TRUE(refs.count(ref));
        EXPECT_EQ(refs.at(ref).manifest_id.ref, manifestRefFor("manifest-" + ref));
        EXPECT_EQ(refs.at(ref).manifest_id.root_namespace.string(), ns.string());
    }
}

/// An empty namespace recovers from its exact `_ckpt` authority and exact successor GET. It performs
/// ZERO LISTs and ZERO HEADs: recovery no longer enumerates the stream, and it never probes a shard
/// fan-out. Measure deltas around `listRefs`; `Pool::open` and fixture admission have their own metadata
/// traffic.
TEST(CASPool, ListRefsEmptyNamespaceCostsZeroListsAndHeads)
{
    auto b = std::make_shared<DB::Cas::tests::CountingBackend>();
    auto s = Pool::open(b, PoolConfig{.pool_prefix = "p", .server_root_id = "test"});
    RootNamespace ns{"srv1/tbl"};
    /// EMPTY, but EXISTING and recoverable. A namespace the catalog does not name is answered from the
    /// catalog and never reaches recovery; that separate shape is measured by the case below.
    DB::Cas::tests::casAdmitRecoverableEntry(*b, Layout("p"), ns);

    const uint64_t heads_before = b->headTotal();
    const uint64_t lists_before = b->listTotal();

    auto refs = s->listRefs(ns);

    EXPECT_TRUE(refs.empty());
    EXPECT_EQ(b->headTotal() - heads_before, 0u)
        << "empty-namespace listRefs must not HEAD any shard";
    EXPECT_EQ(b->listTotal() - lists_before, 0u)
        << "checkpoint-grounded recovery reads exact keys and must not LIST the ref stream";
}

/// The other shape: a namespace that was never born. A read must not be what brings one into existence,
/// so the answer comes from the catalog alone -- no recovery, and therefore not even the one LIST the
/// case above pins.
TEST(CASPool, ListRefsOnANeverBornNamespaceCostsNoListAndNoHead)
{
    auto b = std::make_shared<DB::Cas::tests::CountingBackend>();
    auto s = Pool::open(b, PoolConfig{.pool_prefix = "p", .server_root_id = "test"});
    RootNamespace ns{"srv1/tbl"};

    const uint64_t heads_before = b->headTotal();
    const uint64_t lists_before = b->listTotal();
    const uint64_t gets_before = b->getTotal();

    auto refs = s->listRefs(ns);

    EXPECT_TRUE(refs.empty());
    EXPECT_EQ(b->listTotal() - lists_before, 0u)
        << "a never-born namespace has no ref stream to LIST";
    EXPECT_EQ(b->headTotal() - heads_before, 0u);
    /// Positive control: the zeros above are the answer coming from the catalog, not from a call that
    /// did nothing at all.
    EXPECT_GT(b->getTotal() - gets_before, 0u)
        << "the answer must come from a catalog read";
}

/// listRefs must return every committed ref of a table, correctly, regardless of how many refs the
/// table holds (Task 10: there is no more shard fan-out to discover -- see the comment inside).
TEST(CASPool, ListRefsReturnsSameContentAsBefore)
{
    /// Task 10: there is no more per-shard HEAD fan-out to bound (a warm listRefs costs ZERO requests;
    /// a cold empty one costs zero LISTs and HEADs, already covered by
    /// `ListRefsEmptyNamespaceCostsZeroListsAndHeads`) -- this now just proves the returned content is
    /// correct for a multi-ref table built from a single raw ref-log fixture.
    auto b = std::make_shared<DB::Cas::tests::CountingBackend>();
    auto s = Pool::open(b, PoolConfig{.pool_prefix = "p", .server_root_id = "test"});
    Layout layout("p");
    RootNamespace ns{"srv1/tbl"};

    std::vector<RefOp> ops{DB::Cas::tests::namespaceBirthOp()};
    for (const String & ref : {String("a"), String("m"), String("z")})
    {
        const auto committed_ops = DB::Cas::tests::publishCommittedOps(ref, manifestRefFor("manifest-" + ref));
        ops.insert(ops.end(), committed_ops.begin(), committed_ops.end());
    }
    DB::Cas::tests::fixture::writeRefLogRaw(*b, layout, RefLogTxn{ns.string(), RefTxnId{1, 1}, ops, std::nullopt});
    DB::Cas::tests::writeRecoverableCkptForRawFixture(*b, layout, ns, RefCkpt{
        .life_epoch = 1,
        .committed_through = RefTxnId{1, 1},
        .checkpoint_snapshot_id = std::nullopt,
        .last_epoch_seal = std::nullopt,
    });

    auto refs = s->listRefs(ns);

    ASSERT_EQ(refs.size(), 3u);
    for (const String & ref : {String("a"), String("m"), String("z")})
    {
        ASSERT_TRUE(refs.count(ref));
        EXPECT_EQ(refs.at(ref).manifest_id.ref, manifestRefFor("manifest-" + ref));
        EXPECT_EQ(refs.at(ref).manifest_id.root_namespace.string(), ns.string());
    }
}

/// A stray key under the namespace's ref-object prefix that does not parse as one of Task 10's
/// `_log`/`_snap` kinds (a foreign/corrupt object) must not break listRefs — it is skipped
/// defensively, listRefs still returns the legit refs and never throws.
TEST(CASPool, ListRefsSkipsForeignKeys)
{
    auto b = std::make_shared<InMemoryBackend>();
    auto s = Pool::open(b, PoolConfig{.pool_prefix = "p", .server_root_id = "test"});
    Layout layout("p");
    RootNamespace ns{"srv1/tbl"};

    const String ref = "legit";
    const ManifestRef mref = manifestRefFor("manifest-" + ref);
    DB::Cas::tests::fixture::writeRefLogRaw(*b, layout, RefLogTxn{ns.string(), RefTxnId{1, 1},
        {DB::Cas::tests::namespaceBirthOp(), DB::Cas::tests::publishCommittedOps(ref, mref)[0],
         DB::Cas::tests::publishCommittedOps(ref, mref)[1]}, std::nullopt});
    DB::Cas::tests::writeRecoverableCkptForRawFixture(*b, layout, ns, RefCkpt{
        .life_epoch = 1,
        .committed_through = RefTxnId{1, 1},
        .checkpoint_snapshot_id = std::nullopt,
        .last_epoch_seal = std::nullopt,
    });

    /// A stray key directly under the namespace's ref-object prefix that is not `_log`/
    /// `_snap` shaped (also covers the legacy shard-number layout GC/dropNamespace still write).
    createObj(*b, layout.namespaceStreamPrefix(DB::Cas::tests::fixture::fixtureLife(ns)) + "garbage", "not-a-ref-object");

    std::map<String, Resolved> refs;
    EXPECT_NO_THROW(refs = s->listRefs(ns));
    ASSERT_EQ(refs.size(), 1u);
    ASSERT_TRUE(refs.count(ref));
    EXPECT_EQ(refs.at(ref).manifest_id.ref, mref);
}

/// readManifest fails CLOSED on a corrupt or kind-mismatched manifest body addressed by a live id.
TEST(CASPool, ReadManifestFailsClosed)
{
    auto b = std::make_shared<InMemoryBackend>();
    auto s = Pool::open(b, PoolConfig{.pool_prefix = "p", .server_root_id = "test"});
    Layout layout("p");
    const RootNamespace ns{"srv1/tbl"};

    /// (1) Garbage bytes at the manifest key => decodePartManifest throws CORRUPTED_DATA.
    {
        const ManifestRef ref = manifestRefFor("garbage-body");
        const ManifestId id{.root_namespace = ns, .ref = ref};
        createObj(*b, layout.manifestKey(id), "not a valid manifest body");
        expectThrowsCode(DB::ErrorCodes::CORRUPTED_DATA, [&] { s->readManifest(id); });
    }

    /// (2) A ref naming a manifest id with NO object present => readManifest throws FILE_DOESNT_EXIST
    /// (INV-NO-DANGLE), carrying the manifest key.
    {
        const ManifestRef ref = manifestRefFor("absent-body");
        const ManifestId id{.root_namespace = ns, .ref = ref};
        expectThrowsCode(DB::ErrorCodes::FILE_DOESNT_EXIST, [&] { s->readManifest(id); });
    }
}

/// ---------- ref lifecycle: dropRef / updateRefPublishedAt / dropNamespace ----------

TEST(CASPool, DropRefAppendsJournalAtomically)
{
    /// Task 10: the OLD shared-journal record assertions are gone (there is no shared mutable journal
    /// object anymore — dropRef appends its OWN immutable ref-log transaction); the surviving
    /// behavioral contract is: the drop is atomic (visible to resolveRef only once durable), and
    /// dropping a missing ref is fail-closed, never a silent no-op.
    auto b = std::make_shared<InMemoryBackend>();
    auto s = Pool::open(b, PoolConfig{.pool_prefix = "p", .server_root_id = "test"});
    RootNamespace ns{"srv1/tbl"};

    publishPart(s, ns.string(), "part_1", "payload-1");
    ASSERT_TRUE(s->resolveRef(ns, "part_1").has_value());

    s->dropRef(ns, "part_1");
    EXPECT_FALSE(s->resolveRef(ns, "part_1").has_value());
    EXPECT_TRUE(s->listRefs(ns).empty());

    /// Dropping a missing ref is fail-closed, never a silent no-op.
    expectThrowsCode(DB::ErrorCodes::FILE_DOESNT_EXIST, [&] { s->dropRef(ns, "no_such_ref"); });
}

/// Task 10 renamed this from "...WithoutJournal": updateRefPublishedAt now DOES append an immutable
/// `set_published_at` ref-log transaction (spec §Update Payload) -- the old journal-free in-place field
/// mutation had no equivalent once persistence is an append-only log; every change, even timestamp-only,
/// must be a logged operation to be part of the ordered history. All-tree-part-files Task 9: the
/// carrier's mutable-file map is gone -- `published_at_ms` is the only field left to mutate. The
/// surviving contract is the user-visible one: a `published_at_ms` update is observable through
/// resolveRef and the manifest edge cannot change on this path -- the `RefPublishedAtUpdate` carrier
/// deliberately has no `manifest_ref` field, so a reachability change is structurally impossible here
/// (it goes through publish/drop/repoint instead).
TEST(CASPool, UpdateRefPublishedAtUpdatesPublishedAtMs)
{
    auto b = std::make_shared<InMemoryBackend>();
    auto s = Pool::open(b, PoolConfig{.pool_prefix = "p", .server_root_id = "test"});
    RootNamespace ns{"srv1/tbl"};

    const ManifestId id = publishPart(s, ns.string(), "part_1", "payload-1");
    const ManifestRef manifest_ref = id.ref;

    s->updateRefPublishedAt(ns, "part_1", [](RefPublishedAtUpdate & r) { r.published_at_ms = 1; });
    s->updateRefPublishedAt(ns, "part_1", [](RefPublishedAtUpdate & r) { r.published_at_ms = 7; });

    auto after = s->resolveRef(ns, "part_1");
    ASSERT_TRUE(after.has_value());
    EXPECT_EQ(after->published_at_ms, 7u);
    EXPECT_EQ(after->manifest_id.ref, manifest_ref);
}

/// Task 11: dropNamespace removes every owner through the ref-log `remove_namespace` transaction and
/// performs NO physical deletion at all -- verbatim files survive until GC's perpetual janitor
/// reclaims the dead life. So after the drop every ref resolves away and
/// `listRefs` is empty, but the verbatim files remain readable.
TEST(CASPool, DropNamespaceRemovesEveryOwnerButLeavesFilesForGc)
{
    auto b = std::make_shared<InMemoryBackend>();
    auto s = Pool::open(b, PoolConfig{.pool_prefix = "p", .server_root_id = "test"});
    RootNamespace ns{"srv1/tbl"};

    const std::vector<String> ref_names{"alpha", "bravo", "charlie"};
    for (const String & name : ref_names)
        publishPart(s, ns.string(), name, "payload-" + name);
    for (const String & name : ref_names)
        ASSERT_TRUE(s->resolveRef(ns, name).has_value());

    s->putNamespaceFile(DB::Cas::tests::fixture::fixtureLife(ns), "format_version.txt", "1\n");
    s->putNamespaceFile(DB::Cas::tests::fixture::fixtureLife(ns), "uuid.txt", "abc");

    s->dropNamespace(ns);

    for (const String & name : ref_names)
        EXPECT_FALSE(s->resolveRef(ns, name).has_value());
    EXPECT_TRUE(s->listRefs(ns).empty());

    /// The writer performs NO physical deletion; verbatim files survive until the perpetual janitor
    /// reclaims the dead life.
    EXPECT_TRUE(s->getNamespaceFile(DB::Cas::tests::fixture::fixtureLife(ns), "format_version.txt").has_value());
    EXPECT_TRUE(s->getNamespaceFile(DB::Cas::tests::fixture::fixtureLife(ns), "uuid.txt").has_value());

    /// Repeated drop is idempotent: no throw, no second transaction (nothing left to observe changing).
    EXPECT_NO_THROW(s->dropNamespace(ns));

    /// Ordinary mutations on a cataloged `Removing` life are rejected with typed retry-later until
    /// the terminal fold and catalog-only drain complete.
    expectThrowsCode(DB::ErrorCodes::NETWORK_ERROR, [&] { s->dropRef(ns, "alpha"); });
}

TEST(CASPool, ListNamespacesFromCatalog)
{
    /// `listNamespaces` projects logical names from the authoritative catalog. Physical life keys
    /// contain no namespace spelling and therefore cannot participate in this enumeration.
    auto b = std::make_shared<InMemoryBackend>();
    auto s = Pool::open(b, PoolConfig{.pool_prefix = "p", .server_root_id = "test"});

    EXPECT_TRUE(s->listNamespaces("").namespaces.empty());   /// fresh pool: empty catalog

    /// The real publication path admits each namespace before writing its stream.
    DB::Cas::tests::publishCommittedTransition(*b, s->layout(), RootNamespace{"srv1/tbl"},
        "ref1", std::nullopt, DB::Cas::ManifestRef{.writer_epoch = 1, .build_sequence = 1, .manifest_ordinal = 1});
    DB::Cas::tests::publishCommittedTransition(*b, s->layout(), RootNamespace{"srv1/shadow/bk1/tbl"},
        "ref1", std::nullopt, DB::Cas::ManifestRef{.writer_epoch = 1, .build_sequence = 1, .manifest_ordinal = 1});
    DB::Cas::tests::publishCommittedTransition(*b, s->layout(), RootNamespace{"srv1/shadow/bk2/tbl"},
        "ref1", std::nullopt, DB::Cas::ManifestRef{.writer_epoch = 1, .build_sequence = 1, .manifest_ordinal = 1});

    const auto all = s->listNamespaces("").namespaces;
    EXPECT_EQ(all.size(), 3u);
    const auto shadows = s->listNamespaces("srv1/shadow/").namespaces;
    ASSERT_EQ(shadows.size(), 2u);
    /// listNamespaces returns results from an unordered_set; sort for deterministic comparison.
    auto sorted_shadows = shadows;
    std::sort(sorted_shadows.begin(), sorted_shadows.end());
    EXPECT_EQ(sorted_shadows[0], "srv1/shadow/bk1/tbl");
    EXPECT_EQ(sorted_shadows[1], "srv1/shadow/bk2/tbl");
    EXPECT_TRUE(s->listNamespaces("nope/").namespaces.empty());
}

/// Physical namespace files carry only an opaque life id and cannot mint a logical catalog row.
TEST(CASPool, ListNamespacesDoesNotMintLogicalNamesFromFileKeys)
{
    auto b = std::make_shared<InMemoryBackend>();
    auto s = Pool::open(b, PoolConfig{.pool_prefix = "p", .server_root_id = "test"});
    const RootNamespace ns{"test/tbl@cas@"};

    s->putNamespaceFile(DB::Cas::tests::fixture::fixtureLife(ns), "format_version.txt", "1\n");
    /// A second life of the SAME name, written by exact key because no helper mints two lives yet.
    const NamespaceLifeId other = NamespaceLifeId::fromCatalogEntry(ns, DB::UInt128(0x5eed));
    createObj(*b, s->layout().namespaceFileKey(other, "format_version.txt"), "1\n");

    const NamespaceListing listing = s->listNamespaces("");
    EXPECT_TRUE(listing.skipped.empty());
    EXPECT_TRUE(listing.namespaces.empty());
}

/// Catalog discovery neither adopts nor reports malformed physical debris. Diagnostic ownership-tree
/// scans, not ordinary logical enumeration, classify those keys.
TEST(CASPool, ListNamespacesDoesNotTreatPhysicalDebrisAsCatalogAuthority)
{
    auto b = std::make_shared<InMemoryBackend>();
    auto s = Pool::open(b, PoolConfig{.pool_prefix = "p", .server_root_id = "test"});
    const RootNamespace ns{"test/tbl@cas@"};

    /// One well-formed key per family, so the namespace is attributable either way.
    DB::Cas::tests::publishCommittedTransition(*b, s->layout(), ns,
        "ref1", std::nullopt, DB::Cas::ManifestRef{.writer_epoch = 1, .build_sequence = 1, .manifest_ordinal = 1});
    s->putNamespaceFile(DB::Cas::tests::fixture::fixtureLife(ns), "format_version.txt", "1\n");

    /// Hand-built un-incarnated keys: no helper can mint either shape any more.
    const String lifeless_ref = s->layout().casRefsPrefix() + ns.string() + "/_log/"
        + renderRefTxnId(RefTxnId{1, 1}) + ".zst";
    const String lifeless_file = s->layout().rootsPrefix() + ns.string() + "/_files/format_version.txt";
    createObj(*b, lifeless_ref, "garbage");
    createObj(*b, lifeless_file, "garbage");

    NamespaceListing listing;
    ASSERT_NO_THROW(listing = s->listNamespaces(""))
        << "one un-attributable key must not abort the enumeration for every consumer of it";

    /// The healthy namespace is still listed -- attribution is per key, so a namespace disappears only
    /// when every key that would name it is unattributable.
    ASSERT_EQ(listing.namespaces.size(), 1u);
    EXPECT_EQ(listing.namespaces[0], ns.string());

    EXPECT_TRUE(listing.skipped.empty());
    EXPECT_TRUE(headObj(*b, lifeless_ref).has_value());
    EXPECT_TRUE(headObj(*b, lifeless_file).has_value());
}

TEST(CASPool, ListMirroredChildren)
{
    using namespace DB::Cas;
    auto b = std::make_shared<InMemoryBackend>();
    auto store = Pool::open(b, PoolConfig{.pool_prefix = "p", .server_root_id = "test"});
    /// Seed two catalog-authoritative shadow archives; physical files alone carry no logical path.
    DB::Cas::tests::fixture::admitLive(*b, store->layout(), RootNamespace{"srv1/shadow/bk1/store/3f2/3f2a-uuid@cas@"});
    DB::Cas::tests::fixture::admitLive(*b, store->layout(), RootNamespace{"srv1/shadow/bk2/store/3f2/3f2a-uuid@cas@"});
    auto children = store->listMirroredChildren("srv1/shadow/");
    std::sort(children.begin(), children.end());
    ASSERT_EQ(children.size(), 2u);
    EXPECT_EQ(children[0], "bk1");
    EXPECT_EQ(children[1], "bk2");
}

namespace
{

/// Delegating backend that fences the mount slot IN PLACE the first time a `get` returns a present
/// body for the armed key — reproducing the S13 window: the GC's token-guarded fence-out lands
/// between the renewer adopt's GET and its CAS. The caller's subsequent token-guarded `putOverwrite`
/// then fails `PreconditionFailed`, the adopt re-reads, sees `gc_fenced`, and throws
/// `MountFencedException` — which `Pool::open`'s fence-recovery loop must turn into a fresh-epoch
/// retry rather than a permanent wedge (P3.1 vector C).
class FenceInAdoptWindowBackend final : public DB::Cas::Backend
{
public:
    explicit FenceInAdoptWindowBackend(std::shared_ptr<DB::Cas::Backend> inner_) : inner(std::move(inner_)) {}
    String fence_key;   /// empty = fault disarmed; set to the mount key to arm the one-shot fence

    bool supportsListTokens() const override { return inner->supportsListTokens(); }

    /// The fault sits on the READ PRIMITIVE: the renewer's adopt reads the mount slot through it.
    std::optional<Raw> read(const String & key, TransportAccess & access) override
    {
        auto got = inner->read(key, access);
        if (!fence_key.empty() && key == fence_key && got.has_value())
        {
            /// One-shot: fence the slot in place exactly as `computeHeartbeatFloor` does (preserve the
            /// body, gc_fenced = true, seq + 1, guarded against the incarnation we just read), then
            /// disarm so the retry can adopt cleanly.
            DB::Cas::MountLease fenced = DB::Cas::decodeMountLease(got->bytes);
            fenced.gc_fenced = true;
            fenced.seq += 1;
            (void)inner->write(key, DB::Cas::encodeMountLease(fenced), got->value, access);
            fence_key.clear();
        }
        return got;
    }
    std::optional<RawMeta> head(const String & key, TransportAccess & access) override { return inner->head(key, access); }
    RawListPage list(const String & prefix, const String & cursor, size_t limit, TransportAccess & access) override { return inner->list(prefix, cursor, limit, access); }
    RawRemoval remove(const String & key, const String & expected_value, TransportAccess & access) override { return inner->remove(key, expected_value, access); }
    void removeManyWriteOnce(const std::vector<WriteOnceKey> & keys, TransportAccess & access) override { inner->removeManyWriteOnce(keys, access); }
    std::expected<String, RawConflict> write(const String & key, const String & bytes,
                                             const std::optional<String> & expected_value, TransportAccess & access) override
    {
        return inner->write(key, bytes, expected_value, access);
    }
    std::unique_ptr<DB::ReadBuffer> stream(const String & key, TransportAccess & access) override { return inner->stream(key, access); }
    void publish(const BlobPublishRequest & request, TransportAccess & access) override { inner->publish(request, access); }
    Dialect dialect() const override { return inner->dialect(); }

private:
    std::shared_ptr<DB::Cas::Backend> inner;
};

}

TEST(CASPoolMountFence, OpenRecoversFromFenceInAdoptWindowWithFreshEpoch)
{
    auto inner = std::make_shared<InMemoryBackend>();
    auto fencing = std::make_shared<FenceInAdoptWindowBackend>(inner);
    /// Arm the one-shot fence on the mount slot. Pool::open first claims the mount (fresh mint), then
    /// the renewer adopts it — the adopt's GET trips the fence, its CAS fails, and open must recover.
    const DB::Cas::Layout layout("p");
    fencing->fence_key = layout.mountKey("test");

    /// The retry that recovers from the fence reclaims a same-uuid, different-epoch, `gc_fenced` body
    /// -> `MountPriorState::Fenced` (a fenced prior is reclaimed on the first attempt, with no
    /// observation polling -- see `CASMountOpenWaits.FencedPriorReclaimsWithoutAnyWait`). The injected
    /// `boot_ms_fn`/`wait_sleep_fn` below keep this test off the real clock regardless.
    /// Held in a shared atomic, not a plain local: `wait_sleep_fn` below mutates it, and the Pool can
    /// outlive this stack frame (a background publish holds `shared_from_this()`), so a by-reference
    /// capture of a local would dangle.
    auto fake_boot = std::make_shared<std::atomic<uint64_t>>(0);
    DB::Cas::PoolPtr store;
    ASSERT_NO_THROW(
        store = DB::Cas::Pool::open(fencing,
            DB::Cas::PoolConfig{.pool_prefix = "p", .server_root_id = "test",
                .boot_ms_fn = [fake_boot]
                {
                    return fake_boot->load();
                },
                .wait_sleep_fn = [fake_boot](uint64_t ms)
                {
                    *fake_boot += ms;
                }}))
        << "open must recover from a fence in the adopt window, not wedge (exit-49 S13 bug)";
    ASSERT_TRUE(store);

    /// The final live lease is unfenced and at a HIGHER writer_epoch than the first attempt (a fence
    /// costs an epoch): the first claim took epoch 1, got fenced, the retry took epoch 2 and mounted.
    const auto got = readObj(*inner, layout.mountKey("test"));
    ASSERT_TRUE(got.has_value());
    const MountLease final_lease = decodeMountLease(got->bytes);
    EXPECT_FALSE(final_lease.gc_fenced);
    EXPECT_GT(final_lease.writer_epoch, 1u) << "recovery must draw a fresh writer_epoch";
    EXPECT_TRUE(fencing->fence_key.empty()) << "the one-shot fence must have fired";
}

/// Task 12: the write-fence deadline is a CLOCK_BOOTTIME instant (boottime includes VM-suspend time,
/// so a resumed sleeper sees its fence expired — unlike CLOCK_MONOTONIC, which freezes across suspend).
/// A CLOCK_MONOTONIC freeze cannot be simulated in a unit test, so we exercise the injected-fn seam: a
/// fake boot clock that we advance past the ttl must flip mayMutate to false and make a gated mutate
/// fail closed with ABORTED.
TEST(CASPool, WriteFenceUsesInjectedBootClock)
{
    auto backend = std::make_shared<InMemoryBackend>();
    /// Held in a shared atomic, not a plain local: this test mutates the clock below, and the Pool can
    /// outlive this stack frame (a background publish holds `shared_from_this()`), so a by-reference
    /// capture of a local would dangle.
    auto fake_boot = std::make_shared<std::atomic<uint64_t>>(1'000'000);   /// arbitrary boottime origin (ms)
    auto store = DB::Cas::Pool::open(backend, DB::Cas::PoolConfig{
        .pool_prefix = "p",
        .server_root_id = "test",
        .mount_lease_ttl_ms = std::chrono::milliseconds(30000),
        .boot_ms_fn = [fake_boot]
        {
            return fake_boot->load();
        },
    });

    /// Freshly armed at open (deadline = fake_boot + ttl): well within the ttl, mutations are allowed.
    EXPECT_TRUE(store->mayMutate());

    /// Advance the boot clock just short of the deadline — still armed.
    *fake_boot += 29999;
    EXPECT_TRUE(store->mayMutate());

    /// Cross the deadline (ttl elapsed with no renew — a resumed sleeper's view). The fence must expire.
    /// (The "a gated mutate then fails closed with ABORTED" leg used `mutateShardForTest` -- the held
    /// Phase-E shard lane -- and moves there; here we pin the boot-clock fence flip itself.)
    *fake_boot += 2;   /// now fake_boot = origin + 30001 > origin + 30000
    EXPECT_FALSE(store->mayMutate());
}

/// ==== self-remount after GC fence-out (liveness counterpart of the fence-out safety rule) ====

namespace
{

/// GC's fence-out, applied directly: preserve the body, set gc_fenced, bump seq (token-guarded).
void fenceOutMount(DB::Cas::Backend & backend, const String & mount_key)
{
    DB::Cas::tests::OperationForTest op(backend);
    const auto got = (*op).read(mount_key, Retry::standard());
    ASSERT_TRUE(got.has_value());
    MountLease m = decodeMountLease(got->bytes);
    m.gc_fenced = true;
    m.seq += 1;
    ASSERT_TRUE(std::holds_alternative<Committed>(
        (*op).replace(mount_key, encodeMountLease(m), got->etag, Retry::standard())));
}

}

TEST(CASPoolRemount, FenceOutThenSelfRemountRestoresWrites)
{
    auto backend = std::make_shared<InMemoryBackend>();
    auto store = DB::Cas::tests::openPoolForTest(backend);
    const String mount_key = store->layout().mountKey("test");
    const uint64_t epoch_before = decodeMountLease(readObj(*backend, mount_key)->bytes).writer_epoch;
    EXPECT_EQ(store->liveWriterEpoch(), epoch_before);

    fenceOutMount(*backend, mount_key);

    /// The renewer's next renewal fails closed (foreign touch — never re-mint).
    EXPECT_THROW(store->renewWatermarkOnce(), DB::Exception);

    /// Self-remount claims a FRESH incarnation: epoch bumped, gc_fenced cleared, writes restored.
    ASSERT_TRUE(store->tryRemountOnce());
    const MountLease after = decodeMountLease(readObj(*backend, mount_key)->bytes);
    EXPECT_EQ(after.writer_epoch, epoch_before + 1);
    EXPECT_FALSE(after.gc_fenced);
    EXPECT_EQ(store->liveWriterEpoch(), epoch_before + 1);

    /// The renewal path works again (the new renewer owns the slot). (The follow-on "...and so does a
    /// ref-shard mutation" check used `mutateShardForTest` -- the held Phase-E shard lane -- and moves
    /// to Phase E's own tests; the self-remount liveness assertion above is the point of this test.)
    EXPECT_NO_THROW(store->renewWatermarkOnce());
}

TEST(CASPoolRemount, OldEpochBuildFailsClosedAfterRemount)
{
    auto backend = std::make_shared<InMemoryBackend>();
    auto store = DB::Cas::tests::openPoolForTest(backend);
    auto build = store->beginPartWrite({});

    fenceOutMount(*backend, store->layout().mountKey("test"));
    ASSERT_TRUE(store->tryRemountOnce());

    /// The build was minted under the superseded incarnation — every further step fails closed.
    expectThrowsCode(DB::ErrorCodes::NETWORK_ERROR,
        [&] { build->putBlob(DB::Cas::tests::idOf("x"), DB::Cas::BlobSource::fromString("x")); });

    /// A FRESH build under the live incarnation works once its publication edge is durable.
    const RootNamespace ns{"srv/remount"};
    PartWriteInfo info;
    info.intended_ref = ns.string() + "/fresh";
    auto fresh = store->beginPartWrite(info);
    const ManifestId id = fresh->stageManifest({blobEntryFor("data.bin", DB::Cas::tests::u128Of("y"))});
    fresh->precommitAdd(ns, "fresh", id);
    EXPECT_NO_THROW(fresh->putBlob(DB::Cas::tests::idOf("y"), DB::Cas::BlobSource::fromString("y")));
    fresh->abandon();
}

TEST(CASPoolRemount, ForeignOwnerIsNeverTakenOver)
{
    auto backend = std::make_shared<InMemoryBackend>();
    auto store = DB::Cas::tests::openPoolForTest(backend);
    const String mount_key = store->layout().mountKey("test");

    /// A genuinely foreign uuid holds the mount (live or not — foreign is terminal for the claim).
    DB::Cas::tests::OperationForTest overwrite_op(*backend);
    const auto got = (*overwrite_op).read(mount_key, Retry::standard());
    MountLease foreign = decodeMountLease(got->bytes);
    foreign.server_uuid = foreign.server_uuid + DB::UInt128(1);
    foreign.seq += 1;
    ASSERT_TRUE(std::holds_alternative<Committed>(
        (*overwrite_op).replace(mount_key, encodeMountLease(foreign), got->etag, Retry::standard())));

    EXPECT_FALSE(store->tryRemountOnce());
    /// The foreign body is untouched (no takeover, ever).
    EXPECT_EQ(decodeMountLease(readObj(*backend, mount_key)->bytes).server_uuid, foreign.server_uuid);

    /// Move the parent fixture to the production-recognized fenced terminal state before explicitly
    /// destroying its superseded renewer. The unfenced foreign-release guard is covered separately below.
    fenceOutMount(*backend, mount_key);
    store.reset();

    /// A foreign owner is never taken over — at remount OR at release. This was an `EXPECT_DEATH`
    /// pinning a `LOGICAL_ERROR` abort on the release half; the abort fired from `~Pool` and defeated
    /// `finishTeardown`'s own catch by aborting at exception construction. The runtime never observed a
    /// deposition (the slot was overwritten out of band), so the release takes the
    /// exclusivity-violation arm: refuse, leave the foreign occupant untouched, and SURVIVE teardown.
    auto foreign_backend = std::make_shared<InMemoryBackend>();
    auto invalid_store = DB::Cas::tests::openPoolForTest(foreign_backend);
    const String foreign_mount_key = invalid_store->layout().mountKey("test");
    DB::Cas::tests::OperationForTest foreign_overwrite_op(*foreign_backend);
    const auto foreign_got = (*foreign_overwrite_op).read(foreign_mount_key, Retry::standard());
    ASSERT_TRUE(foreign_got.has_value());
    MountLease foreign_lease = decodeMountLease(foreign_got->bytes);
    foreign_lease.server_uuid = foreign_lease.server_uuid + DB::UInt128(1);
    foreign_lease.seq += 1;
    ASSERT_TRUE(std::holds_alternative<Committed>((*foreign_overwrite_op).replace(
        foreign_mount_key, encodeMountLease(foreign_lease), foreign_got->etag, Retry::standard())));
    const auto occupant_before = readObj(*foreign_backend, foreign_mount_key);
    ASSERT_TRUE(occupant_before.has_value());

    EXPECT_FALSE(invalid_store->tryRemountOnce()) << "a foreign owner is never taken over at remount";

    const uint64_t violations_before
        = ProfileEvents::global_counters[ProfileEvents::CASMountExclusivityViolation].load();
    invalid_store.reset();   /// must not abort, must not terminate

    EXPECT_EQ(ProfileEvents::global_counters[ProfileEvents::CASMountExclusivityViolation].load(),
              violations_before + 1)
        << "the release must report the broken single-writer guarantee rather than dying on it";
    const auto occupant_after = readObj(*foreign_backend, foreign_mount_key);
    ASSERT_TRUE(occupant_after.has_value()) << "nor is it taken over at release";
    EXPECT_EQ(occupant_after->bytes, occupant_before->bytes)
        << "the slot must be left byte-for-byte as the foreign owner wrote it";
}

TEST(CASPoolRemount, ShutdownGuardRefusesToArmRemount)
{
    auto backend = std::make_shared<InMemoryBackend>();
    /// `background_watermark = true` so `scheduleRemount` can latch a recovery generation for the
    /// persistent worker in production mode (the same gate both runtime workers check).
    auto store = DB::Cas::Pool::open(backend,
        DB::Cas::PoolConfig{.pool_prefix = "p", .server_root_id = "test", .background_watermark = true});

    /// Teardown has begun: `Pool` latches this before joining either persistent worker.
    store->beginShutdownForTest();

    /// A lease-renewal failure firing during teardown re-enters `scheduleRemount`. With the guard it
    /// must refuse to latch another generation after the workers are stopping.
    EXPECT_FALSE(store->scheduleRemountForTest())
        << "scheduleRemount must not latch recovery work once teardown has begun";
}

namespace
{
/// A sequenced fake boot clock: the first N `bootMsNow()` calls return the values queued via
/// `.queue`, in order; every call after the queue drains returns `.steady`. `CasMountRuntime::bootMsNow`
/// re-invokes `PoolConfig::boot_ms_fn` on EVERY call, with zero memoization -- so a plain call-counter
/// deterministically distinguishes an early (anchor) reading from a later (response-time) one, with no
/// real sleep and no threads.
struct SequencedBootClock
{
    std::vector<uint64_t> queue;
    size_t next = 0;
    uint64_t steady = 0;

    uint64_t operator()()
    {
        if (next < queue.size())
            return queue[next++];
        return steady;
    }
};
}

/// Phase B addendum 2 (task 5b review, reviewer's probe): the self-remount arm must anchor at the
/// claim attempt's pre-I/O instant (`remount_anchor_boot_ms`, captured right after `installRenewer`
/// and right before `renewerStart()` in `Pool::tryRemountOnce`), never at a later reading taken after
/// `renewerStart`/`quiesceRefTablesForRemount` have already run.
///
/// The two `bootMsNow()` calls of interest, in the ORDER each code version issues them:
///   - FIXED code: call #1 = the new anchor (`remount_anchor_boot_ms`, before `renewerStart`);
///     call #2 = `MountLeaseRenewer::prepareRenew`'s own internal boot read inside `renewerStart`'s
///     `doStart` (feeds only the renewer's OWN internal `confirmed_deadline_ms` -- unrelated to the
///     Pool-level arm -- so its value is irrelevant to the arm post-fix).
///   - PRE-FIX code (no anchor line): call #1 = that SAME `prepareRenew` read (now the first boot
///     call of the attempt, since nothing reads the clock before `renewerStart`); call #2 = the
///     arm-site's own `mount_runtime.bootMsNow()`, read AFTER `renewerStart` returns -- the stale,
///     response-time reading this whole fix exists to stop using.
/// A sequenced clock returning 10000 then 11000 (a later response-time reading that remains inside
/// the normal renewal window) therefore arms the FIXED code from 10000 and the PRE-FIX code from
/// 11000, regardless of which call site reads which value -- letting a single deterministic probe
/// (`mayMutate()` at boot == 10000+ttl) tell
/// them apart with no sleep and no thread. (TDD evidence for both branches is recorded in the task-5
/// report, not re-asserted here: this test body only encodes the FIXED expectation.)
TEST(CASPoolRemount, RemountArmAnchorsAtClaimAttemptNotResponseTime)
{
    /// Heap-owned, not a plain stack local: the Pool can outlive this stack frame (a background
    /// publish holds `shared_from_this()`), so a by-reference capture of a local would dangle.
    auto clock = std::make_shared<SequencedBootClock>();
    auto backend = std::make_shared<InMemoryBackend>();
    auto store = DB::Cas::Pool::open(backend, DB::Cas::PoolConfig{
        .pool_prefix = "p", .server_root_id = "test",
        .mount_lease_ttl_ms = std::chrono::milliseconds(30'000),
        .boot_ms_fn = [clock]
        {
            return (*clock)();
        },
    });
    ASSERT_TRUE(store);

    /// Trip the fence exactly as every other remount test in this file does.
    fenceOutMount(*backend, store->layout().mountKey("test"));

    /// Arm the sequence for the upcoming remount attempt: the initial `open` above already drained
    /// an unrelated number of `bootMsNow()` calls (all served from `.steady = 0` -- irrelevant, since
    /// nothing probes the resulting arm before this point). Reset the counter so the FIRST call from
    /// here on is the remount attempt's own call #1.
    clock->queue = {10000, 11000};
    clock->next = 0;

    ASSERT_TRUE(store->tryRemountOnce());

    /// Probe at boot == anchor + ttl (10000 + 30000 = 40000): the fixed code armed from the anchor
    /// (10000), so the fence has JUST expired here -- `mayMutate` must be false. (The pre-fix code
    /// would still read `mayMutate` as true here, armed from 11000 + 30000 -- see the TDD run in the
    /// report.)
    clock->steady = 40000;
    EXPECT_FALSE(store->mayMutate())
        << "the remount arm must anchor at the claim attempt's pre-I/O instant, not a later "
           "response-time reading taken after renewerStart/quiesceRefTablesForRemount";
}

/// ==== self-remount vs. a live successor carrying the same uuid under the unsafe-reclaim knob ====
///
/// `cas_unsafe_remount_no_delay` is consulted at exactly one site: the writable `Pool::open` claim.
/// `Pool::tryRemountOnce` (self-remount after a fence loss) does NOT consult it -- an incarnation
/// superseded by a duplicate-uuid process must still OBSERVE the slot's write-token before it may
/// reclaim, or two processes sharing a uuid (a copied uuid file, a stalled predecessor restarted under
/// the knob) would alternate authority indefinitely.

TEST(CASMountRemount, SupersededIncarnationDoesNotReclaimALiveSuccessor)
{
    auto backend = std::make_shared<InMemoryBackend>();
    /// Held in shared atomics, not plain locals: `wait_sleep_fn`/`setWaitSleepForTest` below mutate
    /// them, and each Pool can outlive this stack frame (a background publish holds
    /// `shared_from_this()`), so a by-reference or by-raw-pointer capture of a local would dangle.
    auto boot_a = std::make_shared<std::atomic<uint64_t>>(0);
    auto boot_b = std::make_shared<std::atomic<uint64_t>>(0);
    /// Mirrors `UncleanOpenPaysOnlyTheObservationWindow`'s tiny budget: the 1s lease TTL below is far
    /// under the default `cas_request_budget`, so it must be scaled down to fit the required-timeout
    /// inequality (attempt_timeout + safety_margin < lease TTL).
    const CasRequestBudget tiny_budget{
        .attempt_timeout_ms = 50, .lease_safety_margin_ms = 50, .connect_timeout_cap_ms = std::nullopt};
    auto config_for = [&](const std::shared_ptr<std::atomic<uint64_t>> & boot, bool unsafe)
    {
        return PoolConfig{
            .pool_prefix = "p", .server_id = UInt128(1), .server_root_id = "test",
            .mount_lease_ttl_ms = std::chrono::milliseconds(1000),
            .mount_renew_period = std::chrono::milliseconds(200),
            .unsafe_remount_no_delay = unsafe,
            .cas_request_budget = tiny_budget,
            .boot_ms_fn = [boot]
            {
                return boot->load();
            },
            .wait_sleep_fn = [boot](uint64_t ms)
            {
                *boot += ms;
            },
        };
    };

    PoolPtr pool_a = Pool::open(backend, config_for(boot_a, /*unsafe=*/false));
    ASSERT_TRUE(pool_a);
    /// B carries the SAME (server_root_id, server_id) as A -- a copied uuid file -- and opens over A's
    /// still-live slot under the operator's unsafe knob, reclaiming it at once (no observation).
    PoolPtr pool_b = Pool::open(backend, config_for(boot_b, /*unsafe=*/true));
    ASSERT_TRUE(pool_b);
    EXPECT_NE(pool_a->liveWriterEpoch(), pool_b->liveWriterEpoch())
        << "the unsafe reclaim must have minted B a fresh epoch over A's slot";

    /// A's next renewal meets the token guard: same uuid, a newer epoch now sits on the slot. Pin the
    /// terminal classification directly (the "superseded" branch of `throwRenewConflict`, the one
    /// that maps to `MountRenewOutcome::Terminal`) rather than accepting any exception -- no accessor
    /// exposes the renewer's outcome/state today, so the error code and the classification's own
    /// wording are what distinguish this from every other terminal reason (foreign owner, GC fence,
    /// vanished slot, an unresolved write).
    try
    {
        pool_a->renewWatermarkOnce();
        FAIL() << "A's renewal must be refused once B's reclaim superseded its epoch";
    }
    catch (const DB::Exception & e)
    {
        EXPECT_EQ(e.code(), DB::ErrorCodes::ABORTED);
        EXPECT_NE(e.message().find("superseded by a newer incarnation"), std::string::npos)
            << "actual message: " << e.message();
    }
    EXPECT_FALSE(pool_a->mayMutate()) << "the superseded classification must trip A's local write fence closed";

    /// A's self-remount now observes the slot's write-token. Drive B's renewal from INSIDE every one
    /// of A's observation polls, so the token never stabilizes across the whole bounded observation --
    /// the knob is not consulted by `tryRemountOnce` (only by `Pool::open`), so nothing else could let
    /// A reclaim a slot a live successor keeps renewing. This cannot deadlock: A and B are distinct
    /// `Pool` objects, so B's `renewWatermarkOnce` takes none of A's locks (each `Pool` owns its own
    /// `remount_mutex`), and the wait fires between `claimMountAwaitingExpiry`'s polls -- with no
    /// backend request of A's own in flight -- so B's call is the only one touching the shared
    /// in-memory backend at that instant.
    /// Heap-owned, not a plain local: same lifetime rule as `boot_a`/`boot_b` above.
    auto polls = std::make_shared<std::atomic<size_t>>(0);
    pool_a->setWaitSleepForTest([boot_a, boot_b, polls, pool_b](uint64_t ms)
    {
        *boot_a += ms;
        ++(*polls);
        *boot_b += ms;
        EXPECT_NO_THROW(pool_b->renewWatermarkOnce());
    });
    EXPECT_FALSE(pool_a->tryRemountOnce())
        << "a superseded incarnation must never reclaim a live successor's slot";
    /// Bounded, not merely nonzero: B renews on every poll, so the observed token changes every
    /// iteration and the FIRST (non-restart) observation start plus `kMaxObservationRestarts` further
    /// restarts is exactly the number of polls before `claimMountAwaitingExpiry` gives up -- one
    /// `sleep_ms_fn` call per iteration that does not itself exceed the bound, and none on the
    /// terminal iteration that does. A widened or removed restart bound would make this hang instead
    /// of failing, so pin the exact count rather than only asserting it ran.
    EXPECT_EQ(polls->load(), DB::Cas::kMaxObservationRestarts + 1)
        << "the observation must give up after exactly kMaxObservationRestarts restarts, not wait "
           "indefinitely for a live twin to go quiet";

    const MountLease final_lease = decodeMountLease(readObj(*backend, pool_a->layout().mountKey("test"))->bytes);
    EXPECT_EQ(final_lease.writer_epoch, pool_b->liveWriterEpoch())
        << "the mount slot must still belong to B's incarnation -- A never reclaimed it";
}

/// Cutoff-only fencing: with no renewals and no competing incarnation at all, crossing the armed
/// deadline on the local BOOTTIME clock alone must fence a mount closed -- the mechanism
/// `SupersededIncarnationDoesNotReclaimALiveSuccessor` above relies on is not special-cased to a
/// renewal conflict; the plain boot-clock cutoff fences unconditionally.
TEST(CASMountRemount, CutoffFencesWithoutRenewals)
{
    auto backend = std::make_shared<InMemoryBackend>();
    /// Held in a shared atomic, not a plain local: this test mutates the clock below, and the Pool can
    /// outlive this stack frame (a background publish holds `shared_from_this()`), so a by-reference
    /// capture of a local would dangle.
    auto boot = std::make_shared<std::atomic<uint64_t>>(0);
    const CasRequestBudget tiny_budget{
        .attempt_timeout_ms = 50, .lease_safety_margin_ms = 50, .connect_timeout_cap_ms = std::nullopt};
    PoolPtr store = Pool::open(backend, PoolConfig{
        .pool_prefix = "p", .server_id = UInt128(1), .server_root_id = "test",
        .mount_lease_ttl_ms = std::chrono::milliseconds(1000),
        .mount_renew_period = std::chrono::milliseconds(200),
        .cas_request_budget = tiny_budget,
        .boot_ms_fn = [boot]
        {
            return boot->load();
        },
        .wait_sleep_fn = [boot](uint64_t ms)
        {
            *boot += ms;
        },
    });
    ASSERT_TRUE(store);
    EXPECT_TRUE(store->mayMutate()) << "freshly armed at open, well within the ttl";

    /// No renewals at all -- advance the boot clock past the armed deadline (open's claim anchor plus
    /// the lease ttl) on this incarnation's own clock alone.
    *boot += 1001;
    EXPECT_FALSE(store->mayMutate())
        << "crossing the armed deadline must fence closed on the boot clock alone, with no renewal "
           "conflict needed to trip it";
}

/// ==== rev.6 Task 5: clean-release drain gates the farewell marker ====

namespace
{
/// Makes every write whose key contains `fault_key_substr` throw an ambiguous exception -- the minimal
/// subset of `RefWriterTestBackend`'s fault injection (gtest_cas_ref_writer.cpp) this file's shutdown
/// and remount tests need to drive a ref-log append into the wedge outcome. It stays armed: one
/// ambiguous attempt is not a wedge, because the engine resolves it by reading and reissues -- the lane
/// wedges only once a bound refuses with an attempt already sent, so the tests injecting it also give
/// the pool a clock they can advance.
class UnresolvedPutBackend final : public DB::Cas::tests::CountingBackend
{
public:
    String fault_key_substr;
    int fault_count = 0;

    /// The fault sits on the WRITE PRIMITIVE: the ref-log append it models is issued there. Nothing
    /// reaches the store, so the engine's resolve read proves the key absent and every reissue is
    /// ambiguous again -- which is what leaves the lane wedged once a bound refuses.
    std::expected<String, RawConflict> write(const String & key, const String & bytes,
        const std::optional<String> & expected_value, DB::Cas::TransportAccess & access) override
    {
        /// Only the create: a ref-log append is a create-if-absent, so a conditional write on the same
        /// key must not consume the fault.
        if (!expected_value && fault_count > 0 && !fault_key_substr.empty()
            && key.find(fault_key_substr) != String::npos)
        {
            --fault_count;
            throw Poco::TimeoutException("UnresolvedPutBackend: simulated ambiguous result (response lost)");
        }
        return DB::Cas::tests::CountingBackend::write(key, bytes, expected_value, access);
    }
};

class RuntimeRenewBackend final : public DB::Cas::tests::CountingBackend
{
public:
    enum class Fault : uint8_t
    {
        None,
        ThrowBefore,
        LandThenThrow,
        BlockThenDelegate,
        BlockThenThrow,
        ThrowMemoryLimitExceeded,
    };

    Fault fault = Fault::None;
    DB::Cas::tests::ManualBarrier * barrier = nullptr;
    std::function<void()> after_commit;
    /// While it answers TRUE, every conditional write throws a transport timeout, before `fault` is
    /// consulted. Read on the renewing thread; set it before the workers start.
    std::function<bool()> outage;
    /// Runs on every write `outage` fails, before it throws.
    std::function<void()> on_outage_write;
    std::atomic<uint64_t> outage_writes{0};

    /// The fault sits on the WRITE PRIMITIVE, and only on a CONDITIONAL one: a lease renewal is a
    /// replace, so a create on the same key must not consume the one-shot fault.
    std::expected<String, RawConflict> write(const String & key, const String & bytes,
        const std::optional<String> & expected_value, DB::Cas::TransportAccess & access) override
    {
        if (!expected_value)
            return DB::Cas::tests::CountingBackend::write(key, bytes, expected_value, access);
        if (outage && outage())
        {
            outage_writes.fetch_add(1, std::memory_order_relaxed);
            if (on_outage_write)
                on_outage_write();
            throw Poco::TimeoutException("injected runtime renewal outage");
        }
        const Fault current = std::exchange(fault, Fault::None);
        if (current == Fault::BlockThenDelegate || current == Fault::BlockThenThrow)
        {
            if (!barrier)
                throw DB::Exception(DB::ErrorCodes::CORRUPTED_DATA, "runtime renewal barrier is absent");
            barrier->arriveAndWait();
        }
        if (current == Fault::ThrowMemoryLimitExceeded)
            throw DB::Exception(DB::ErrorCodes::MEMORY_LIMIT_EXCEEDED, "injected memory limit exceeded on the renewal request");
        if (current == Fault::ThrowBefore || current == Fault::BlockThenThrow)
            throw Poco::TimeoutException("injected runtime renewal ambiguity before result");

        auto result = DB::Cas::tests::CountingBackend::write(key, bytes, expected_value, access);
        if (after_commit)
            after_commit();
        if (current == Fault::LandThenThrow)
            throw Poco::TimeoutException("injected runtime renewal response loss after commit");
        return result;
    }
};

CasRequestBudget runtimeRenewBudget();

/// A directly-constructed `CasMountRuntime` plus the request planes `Pool` would give it.
class RuntimeUnderTest
{
public:
    template <typename BackendT, typename... Args>
    RuntimeUnderTest(const std::shared_ptr<BackendT> & backend, Args &&... args)
        : farewell(backend, DB::Cas::Fence::open())
        , lease(backend, DB::Cas::Fence::open())
        , runtime(backend, farewell, lease, std::forward<Args>(args)...)
    {
        /// What the request engine reserves per attempt is the BACKEND's attempt timeout, not the
        /// budget field alone; every construction of this holder pairs the two via `runtimeRenewBudget`,
        /// the sole budget it is ever built with in this file.
        backend->setAttemptTimeoutMs(runtimeRenewBudget().attempt_timeout_ms);
        /// The runtime arms its lease deadline on ITS boot clock, and the engine measures that deadline
        /// against the clock it reads. Production runs both on `CLOCK_BOOTTIME`, so they agree; a test
        /// that injects one MUST inject the other, or `Retry::untilLeaseSafe` compares a synthetic
        /// deadline against real boottime, finds it long past, and refuses every request unsent.
        farewell.setNowFnForTest([this] { return runtime.bootMsNow(); });
        lease.setNowFnForTest([this] { return runtime.bootMsNow(); });
        /// As `Pool` wires it: a stop wakes the lease thread's renewal wait.
        lease.setSleepFnForTest([this](uint64_t ms) { runtime.sleepInterruptibly(ms); });
    }

    /// The workers are joined HERE, not only by the tests that assert on teardown: `CasMountRuntime`
    /// aborts the process when it is destroyed with a worker still joinable, so an exception on any
    /// path out of a test body -- a barrier that timed out, an assertion that threw -- would take the
    /// whole binary down and hide every test after it.
    ~RuntimeUnderTest()
    {
        try
        {
            runtime.stopBackgroundWorkers();
        }
        catch (...)   // NOLINT(bugprone-empty-catch)
        {
        }
    }

    CasMountRuntime & operator*() { return runtime; }

    /// The retry wait of the lease plane, where the renewal runs. Call before the lease thread starts.
    void setRetrySleepForTest(const std::function<void(uint64_t)> & sleep_fn)
    {
        lease.setSleepFnForTest(sleep_fn);
    }

private:
    DB::Cas::CasRequests farewell;
    DB::Cas::CasRequests lease;
    CasMountRuntime runtime;
};

enum class ForeignConflictSinkBehavior : uint8_t
{
    ReenterSameRuntime,
    Throw,
};

void verifyForeignConflictSinkIsNonInterfering(ForeignConflictSinkBehavior behavior)
{
    auto backend = std::make_shared<RuntimeRenewBackend>();
    const Layout layout(
        behavior == ForeignConflictSinkBehavior::ReenterSameRuntime
            ? "runtime-reentrant-foreign-conflict"
            : "runtime-throwing-foreign-conflict");
    const String server_root_id = "test";
    const String key = layout.mountKey(server_root_id);
    uint64_t wall_ms = 1000;
    uint64_t boot_ms = 100;
    const UInt128 uuid{1};
    ASSERT_EQ(claimMount(*DB::Cas::tests::OperationForTest(backend), layout, server_root_id, uuid, 1, wall_ms, 1000).kind, MountClaimResult::Claimed);

    std::vector<CasEvent> events;
    bool reentered = false;
    std::optional<PoolLifecycle> reentrant_lifecycle;
    std::optional<bool> reentrant_may_mutate;
    CasMountRuntime * runtime_ptr = nullptr;
    CasEventSink sink = [&](CasEvent event)
    {
        const bool foreign_conflict
            = event.type == CasEventType::MountConflict && event.outcome == "foreign_writer";
        events.push_back(event);
        if (!foreign_conflict)
            return;
        if (behavior == ForeignConflictSinkBehavior::ReenterSameRuntime)
        {
            if (!std::exchange(reentered, true))
            {
                reentrant_lifecycle = runtime_ptr->lifecycle();
                reentrant_may_mutate = runtime_ptr->mayMutate();
                throw std::runtime_error("injected reentrant mount diagnostic sink failure");
            }
        }
        else
        {
            throw std::runtime_error("injected mount diagnostic sink failure");
        }
    };
    RuntimeUnderTest runtime_holder(
        backend,
        layout,
        MountConfig{
            .mount_lease_ttl_ms = std::chrono::milliseconds(1000),
            .boot_ms_fn = [&] { return boot_ms; },
        },
        server_root_id,
        sink,
        runtimeRenewBudget(),
        [] { return false; });
    CasMountRuntime & runtime = *runtime_holder;
    runtime_ptr = &runtime;
    runtime.installRenewer(uuid, 1, [&] { return wall_ms; });
    const uint64_t anchor = runtime.startRenewer();
    runtime.armMountFence(uuid, 1, anchor + 1000);

    DB::Cas::tests::OperationForTest successor_op(*backend);
    auto ours = (*successor_op).read(key, Retry::standard());
    ASSERT_TRUE(ours.has_value());
    MountLease successor = decodeMountLease(ours->bytes);
    successor.server_uuid = UInt128{2};
    successor.writer_epoch = 9;
    successor.seq += 1;
    ASSERT_TRUE(std::holds_alternative<Committed>(
        (*successor_op).replace(key, encodeMountLease(successor), ours->etag, Retry::standard())));
    const uint64_t skipped_before
        = ProfileEvents::global_counters[ProfileEvents::CASMountReleaseSkippedForeignOccupant].load();
    const uint64_t violations_before
        = ProfileEvents::global_counters[ProfileEvents::CASMountExclusivityViolation].load();

    int failure_code = 0;
    String failure_message;
    try
    {
        runtime.renewWatermarkOnce();
        ADD_FAILURE() << "authoritative foreign successor must terminalize renewal";
    }
    catch (const DB::Exception & e)
    {
        failure_code = e.code();
        failure_message = e.message();
    }

    EXPECT_EQ(reentered, behavior == ForeignConflictSinkBehavior::ReenterSameRuntime);
    if (behavior == ForeignConflictSinkBehavior::ReenterSameRuntime)
    {
        ASSERT_TRUE(reentrant_lifecycle.has_value());
        EXPECT_EQ(*reentrant_lifecycle, PoolLifecycle::Live);
        ASSERT_TRUE(reentrant_may_mutate.has_value());
        EXPECT_TRUE(*reentrant_may_mutate);
    }
    else
    {
        EXPECT_FALSE(reentrant_lifecycle.has_value());
        EXPECT_FALSE(reentrant_may_mutate.has_value());
    }
    EXPECT_EQ(failure_code, DB::ErrorCodes::ABORTED) << failure_message;
    EXPECT_NE(failure_message.find("held by a foreign server"), String::npos) << failure_message;
    EXPECT_FALSE(runtime.mayMutate());
    EXPECT_EQ(runtime.lifecycle(), PoolLifecycle::TransientNotLive);
    EXPECT_EQ(ProfileEvents::global_counters[ProfileEvents::CASMountReleaseSkippedForeignOccupant].load(),
              skipped_before + 1);
    const auto failed = std::find_if(events.begin(), events.end(), [](const CasEvent & event)
    {
        return event.type == CasEventType::WatermarkRenew && event.outcome == "failed";
    });
    EXPECT_NE(failed, events.end());
    if (failed != events.end())
        EXPECT_EQ(failed->detail.at("classification"), "conflict");

    const auto successor_before_teardown = readObj(*backend, key);
    ASSERT_TRUE(successor_before_teardown.has_value());
    const uint64_t heads_before_teardown = backend->headCount(key);
    const uint64_t gets_before_teardown = backend->getCount(key);
    const uint64_t writes_before_teardown = backend->putOverwriteCount(key);
    const uint64_t skipped_before_teardown
        = ProfileEvents::global_counters[ProfileEvents::CASMountReleaseSkippedForeignOccupant].load();
    runtime.finishTeardown(true);
    EXPECT_EQ(backend->headCount(key), heads_before_teardown);
    EXPECT_EQ(backend->getCount(key), gets_before_teardown);
    EXPECT_EQ(backend->putOverwriteCount(key), writes_before_teardown);
    EXPECT_EQ(ProfileEvents::global_counters[ProfileEvents::CASMountReleaseSkippedForeignOccupant].load(),
              skipped_before_teardown);
    EXPECT_EQ(ProfileEvents::global_counters[ProfileEvents::CASMountExclusivityViolation].load(), violations_before);
    const auto successor_after_teardown = readObj(*backend, key);
    ASSERT_TRUE(successor_after_teardown.has_value());
    EXPECT_EQ(successor_after_teardown->bytes, successor_before_teardown->bytes);
}

TEST(CASPoolRemount, SameRuntimeReentrantForeignConflictSinkCannotReplaceTerminalOutcome)
{
    verifyForeignConflictSinkIsNonInterfering(ForeignConflictSinkBehavior::ReenterSameRuntime);
}

TEST(CASPoolRemount, ThrowingForeignConflictSinkCannotReplaceTerminalOutcome)
{
    verifyForeignConflictSinkIsNonInterfering(ForeignConflictSinkBehavior::Throw);
}

class RemountStepBackend final : public DB::Cas::tests::CountingBackend
{
public:
    void failNextRead(String key)
    {
        failed_key = std::move(key);
    }

    /// The fault sits on the READ PRIMITIVE: the lifecycle gate reads `_pool_meta` through
    /// `probeSentinelRaw`, which speaks the primitives. A legacy caller reaches it anyway, through the
    /// forwarder, so arming it here covers both surfaces rather than only one.
    std::optional<Raw> read(const String & key, DB::Cas::TransportAccess & access) override
    {
        if (!failed_key.empty() && key == failed_key)
        {
            failed_key.clear();
            throw DB::Exception(DB::ErrorCodes::NETWORK_ERROR, "injected remount probe failure");
        }
        return DB::Cas::tests::CountingBackend::read(key, access);
    }

private:
    String failed_key;
};

class ScopedRemountLogCapture
{
public:
    ScopedRemountLogCapture()
        : logger(getLogger("CasPool"))
        , channel(new Poco::StreamChannel(stream))
        , old_channel(logger->getChannel(), /*shared=*/true)
        , old_level(logger->getLevel())
    {
        logger->setChannel(channel.get());
        logger->setLevel("information");
    }

    ~ScopedRemountLogCapture()
    {
        logger->setChannel(old_channel);
        logger->setLevel(old_level);
    }

    String captured() const { return stream.str(); }

private:
    LoggerPtr logger;
    std::ostringstream stream; // STYLE_CHECK_ALLOW_STD_STRING_STREAM
    Poco::AutoPtr<Poco::StreamChannel> channel;
    /// A real reference (shared=true), so the parked previous channel cannot die while ours is installed.
    Poco::AutoPtr<Poco::Channel> old_channel;
    int old_level;
};

size_t countRemountFinalLogs(const String & output)
{
    constexpr std::string_view needle = "CAS whole-chain remount attempt";
    size_t count = 0;
    for (size_t pos = 0; (pos = output.find(needle, pos)) != String::npos; pos += needle.size())
        ++count;
    return count;
}

class WorkerExitLatch
{
public:
    void recordExit()
    {
        std::lock_guard lock(mutex);
        ++exits;
        cv.notify_all();
    }

    bool waitForAtLeast(uint64_t expected)
    {
        std::unique_lock lock(mutex);
        return cv.wait_for(lock, std::chrono::seconds(20), [&] { return exits >= expected; });
    }

    uint64_t count() const
    {
        std::lock_guard lock(mutex);
        return exits;
    }

private:
    mutable std::mutex mutex;
    std::condition_variable cv;
    uint64_t exits = 0;
};

/// The budget every runtime-renewal test uses. `attempt_timeout_ms`/`lease_safety_margin_ms` bound the
/// mount lease's own admission arithmetic; a renewal write's own attempt count and backoff are the
/// request engine's fence-derived `Retry::standard()` policy now, not a budget knob -- every caller of
/// this helper used to pass `max_attempts=1` and no other value, so that parameter carried nothing.
CasRequestBudget runtimeRenewBudget()
{
    return CasRequestBudget{
        .attempt_timeout_ms = 10,
        .lease_safety_margin_ms = 20,
        /// The default cap (1000 ms) would make the envelope (10 + 2*1000 = 2010) blow every tiny TTL
        /// this budget is used against; no connect notion is exercised by these tests.
        .connect_timeout_cap_ms = std::nullopt,
    };
}
}

TEST(CASPoolShutdown, CleanStopDrainsAndWritesFarewell)
{
    auto backend = std::make_shared<InMemoryBackend>();
    auto store = DB::Cas::Pool::open(backend, DB::Cas::PoolConfig{.pool_prefix = "p", .server_root_id = "test"});
    publishPart(store, "srv/clean_stop", "x", "payload");

    const String mount_key = store->layout().mountKey("test");
    store.reset();   /// drives ~Pool(): with no in-flight ref-log PUT, the drain must succeed.

    const auto got = readObj(*backend, mount_key);
    ASSERT_TRUE(got.has_value());
    const MountLease lease = decodeMountLease(got->bytes);
    EXPECT_EQ(lease.min_active_build_sequence, std::numeric_limits<uint64_t>::max())
        << "a clean drain (no in-flight ref-log PUT) must write the farewell marker";
}

TEST(CASPoolShutdown, UnresolvedWedgeSkipsFarewell)
{
    CasRequestBudget budget;
    budget.attempt_timeout_ms = 100;
    budget.lease_safety_margin_ms = 100;

    auto backend = std::make_shared<UnresolvedPutBackend>();
    /// What the request engine reserves per attempt is the BACKEND's attempt timeout, not the budget
    /// field alone; pair the two so the mount lease's admission arithmetic sees what the budget claims.
    backend->setAttemptTimeoutMs(budget.attempt_timeout_ms);
    /// Held in a shared atomic, not a plain local: `wait_sleep_fn` and the retry-sleep hook below
    /// mutate it, and the Pool can outlive this stack frame (a background publish holds
    /// `shared_from_this()`), so a by-reference capture of a local would dangle.
    auto fake_boot = std::make_shared<std::atomic<uint64_t>>(1'000'000);
    auto store = DB::Cas::Pool::open(backend, DB::Cas::PoolConfig{
        .pool_prefix = "p", .server_root_id = "test", .cas_request_budget = budget,
        .boot_ms_fn = [fake_boot]
        {
            return fake_boot->load();
        },
        .wait_sleep_fn = [fake_boot](uint64_t ms)
        {
            *fake_boot += ms;
        }});
    /// The engine's own inter-attempt sleep advances the same clock its deadlines are read from, so the
    /// retry bound is reached in test time rather than in ninety real seconds.
    store->setCasRetrySleepForTest([fake_boot](uint64_t ms)
    {
        *fake_boot += ms;
    });
    /// By value: `layout` is used after `store.reset()` below, a reference would dangle.
    const Layout layout = store->layout();
    const RootNamespace ns{"srv/wedge_shutdown"};
    /// Stage B (Task 4-C): pin `ns` to the Stage-A sentinel BEFORE its first real touch, so the fault
    /// injected below (computed from that same sentinel) lands on the key production actually writes
    /// to -- otherwise the real append mints an unrelated random incarnation and the fault misses.
    DB::Cas::tests::casAdmitRecoverableEntry(*backend, layout, ns, store->liveWriterEpoch());
    publishPart(store, ns.string(), "x", "payload");

    /// Force the ref-log append the drop below performs into the wedge outcome (as in the wedge tests
    /// in gtest_cas_ref_writer.cpp): every attempt is ambiguous, so the lane is still unresolved when
    /// the retry bound refuses.
    backend->fault_key_substr = layout.namespaceStreamPrefix(DB::Cas::tests::fixture::fixtureLife(ns)) + "_log/";
    backend->fault_count = std::numeric_limits<int>::max();
    expectThrowsCode(DB::ErrorCodes::NETWORK_ERROR, [&] { store->dropRef(ns, "x"); });
    ASSERT_TRUE(store->refLaneWedgedForTest(ns));

    const String mount_key = store->layout().mountKey("test");
    store.reset();   /// drives ~Pool(): the still-wedged lane must skip the farewell marker.

    const auto got = readObj(*backend, mount_key);
    ASSERT_TRUE(got.has_value());
    const MountLease lease = decodeMountLease(got->bytes);
    EXPECT_NE(lease.min_active_build_sequence, std::numeric_limits<uint64_t>::max())
        << "an unresolved ref-log PUT must skip the clean-release farewell marker";
    EXPECT_FALSE(lease.gc_fenced);

    /// A successor claimMount on this body must return LiveDoubleStart (unclean path): no certificate of
    /// death (not fenced, not the clean farewell marker, no proven-dead observation) justifies a
    /// same-uuid, different-epoch reclaim.
    const MountClaimResult claim = claimMount(*DB::Cas::tests::OperationForTest(backend), layout, "test", lease.server_uuid,
        lease.writer_epoch + 1, /*now_ms=*/1, /*ttl_ms=*/30000);
    EXPECT_EQ(claim.kind, MountClaimResult::LiveDoubleStart);
}

/// ==== What a writable mount open may block on ====
///
/// Exactly one thing: the token-stability observation window, and only when the predecessor's death
/// has to be OBSERVED rather than certified. The post-reclaim materialization grace (`T_mat`) that
/// used to run beside it is retired -- it existed so a straggler conditional `PUT` from the dying
/// epoch would settle before the successor trusted its recovery LISTINGS, and recovery does not trust
/// listings any more (it walks arithmetically and fences the straggler with an in-band `EpochSeal`).
/// These three tests pin the surviving shape from all three directions: observed-dead, certified-dead,
/// and cleanly departed.

TEST(CASMountOpenWaits, UncleanOpenPaysOnlyTheObservationWindow)
{
    auto b = std::make_shared<InMemoryBackend>();
    Layout l{"p"};
    DB::Cas::tests::seedPoolMetaForRestart(*b);
    /// Predecessor: claim epoch 7, no farewell (simulate crash: just drop the renewer) -- a bare
    /// `claimMount` plants the lease directly, with no clean-farewell `min_active_build_sequence` marker and no
    /// `gc_fenced`, so the successor below has no certificate of death until it observes one itself.
    ASSERT_EQ(claimMount(*DB::Cas::tests::OperationForTest(b), l, "test", UInt128(1), /*epoch*/ 7, /*now_ms*/ 1000, /*ttl_ms*/ 500).kind,
              MountClaimResult::Claimed);
    /// A real predecessor at epoch 7 durably minted it first (`allocateWriterEpoch` always runs
    /// before the mount claim); seed that durable epoch object here too, or the successor's own
    /// `allocateWriterEpoch` trips the Phase C guard (epoch absent, mount present -> fail closed).
    createObj(*b, l.epochKey("test"), encodeServerEpoch(ServerEpoch{.next_writer_epoch = 8}));

    /// A 500ms lease TTL is far below the default `cas_request_budget` (RFC
    /// cas-s3-timeout-retry-control §required-timeout-model requires attempt_timeout + safety_margin <
    /// lease TTL), so scale the budget down to fit -- mirrors `CasMountStartup::StaleSelfMountReclaimedAfterWait`.
    const CasRequestBudget tiny_budget{
        .attempt_timeout_ms = 50, .lease_safety_margin_ms = 50, .connect_timeout_cap_ms = std::nullopt};

    /// Held in shared, heap-owned state, not plain locals: the hooks below mutate them, and the Pool
    /// can outlive this stack frame (a background publish holds `shared_from_this()`), so a
    /// by-reference capture of a local would dangle.
    auto fake_boot = std::make_shared<std::atomic<uint64_t>>(0);
    auto waits = std::make_shared<SharedWaitLog>();
    PoolPtr store;
    ASSERT_NO_THROW(
        store = Pool::open(b, PoolConfig{
            .pool_prefix = "p", .server_id = UInt128(1), .server_root_id = "test",
            .mount_lease_ttl_ms = std::chrono::milliseconds(500),
            .mount_renew_period = std::chrono::milliseconds(100),
            .cas_request_budget = tiny_budget,
            .boot_ms_fn = [fake_boot]
            {
                return fake_boot->load();
            },
            .wait_sleep_fn = [fake_boot, waits](uint64_t ms)
            {
                *fake_boot += ms;
                waits->push(ms);
            },
        }));
    ASSERT_TRUE(store);

    /// The token-stability observation window is paid in full, pinned to the exact configured
    /// formula (`mountObservationThresholdMs`): threshold_ms = ttl_ms + ttl_ms/20 + poll_interval_ms
    /// = 500 + 25 + 50 = 575 ms, where poll_interval_ms = max(1, mount_renew_period/2) = 50 ms. The
    /// loop only re-checks the threshold between polls, so the observed wait rounds UP to the next
    /// whole poll: ceil(575 / 50) * 50 = 600 ms, i.e. exactly 12 polls of 50 ms each -- because this
    /// predecessor's death was never certified, only observed.
    const std::vector<uint64_t> observed_waits = waits->snapshot();
    uint64_t total = 0;
    for (uint64_t w : observed_waits)
        total += w;
    EXPECT_EQ(total, 600u) << "the observation window must be paid in full, poll-rounded to the "
                              "configured threshold -- neither less (a shortened wait) nor more "
                              "(a reintroduced grace period)";
    /// And every one of those polls is exactly one poll interval -- no wait beyond the observation
    /// poll (the straggler it used to wait out is fenced by the recovery seal instead).
    for (uint64_t w : observed_waits)
        EXPECT_EQ(w, 50u)
            << "an unclean reclaim must not block on any wait beyond the observation poll -- the "
               "straggler it used to wait out is fenced by the recovery seal instead";
}

TEST(CASMountOpenWaits, UnsafeNoDelayOpensWithoutTheObservationWindow)
{
    auto b = std::make_shared<InMemoryBackend>();
    Layout l{"p"};
    DB::Cas::tests::seedPoolMetaForRestart(*b);
    /// Same predecessor shape as UncleanOpenPaysOnlyTheObservationWindow above: a bare `claimMount`
    /// plants the lease directly, with no clean-farewell marker and no `gc_fenced`, so this slot has no
    /// certificate of death -- only `cas_unsafe_remount_no_delay` below will let the successor skip
    /// observing it.
    ASSERT_EQ(claimMount(*DB::Cas::tests::OperationForTest(b), l, "test", UInt128(1), 7, 1000, 500).kind, MountClaimResult::Claimed);
    /// A real predecessor at epoch 7 durably minted this first; seed it here too, or the successor's
    /// own `allocateWriterEpoch` trips the Phase C guard (epoch absent, mount present -> fail closed).
    createObj(*b, l.epochKey("test"), encodeServerEpoch(ServerEpoch{.next_writer_epoch = 8}));
    /// Held in shared, heap-owned state, not plain locals: the hooks below mutate them, and the Pool
    /// can outlive this stack frame (a background publish holds `shared_from_this()`), so a
    /// by-reference capture of a local would dangle.
    auto events = std::make_shared<DB::Cas::tests::SharedEventLog>();
    auto fake_boot = std::make_shared<std::atomic<uint64_t>>(0);
    auto waits = std::make_shared<SharedWaitLog>();
    PoolPtr store;
    /// Same server_id (uuid) as the seeded predecessor and a different epoch -- exactly the shape
    /// `unsafe_remount_no_delay` is for. Unlike the neighbour test, no wait is expected: the bare
    /// `claimMount` reclaims at once under the operator's authorization.
    ASSERT_NO_THROW(store = Pool::open(b, PoolConfig{
        .pool_prefix = "p", .server_id = UInt128(1), .server_root_id = "test",
        .event_sink = [events](CasEvent e)
        {
            events->push(std::move(e));
        },
        .mount_lease_ttl_ms = std::chrono::milliseconds(500), .mount_renew_period = std::chrono::milliseconds(100),
        .unsafe_remount_no_delay = true,
        .cas_request_budget = CasRequestBudget{.attempt_timeout_ms = 50, .lease_safety_margin_ms = 50, .connect_timeout_cap_ms = std::nullopt},
        .boot_ms_fn = [fake_boot]
        {
            return fake_boot->load();
        },
        .wait_sleep_fn = [fake_boot, waits](uint64_t ms)
        {
            *fake_boot += ms;
            waits->push(ms);
        },
    }));
    ASSERT_TRUE(store);
    EXPECT_TRUE(waits->snapshot().empty()) << "no observation window under the unsafe setting";
    /// `Pool` has no test accessor for the adopted `MountPriorState`, so the `UncleanUnsafe`
    /// classification is asserted through the mount audit event instead: `claimMount`'s unsafe-reclaim
    /// branch (`CasServerRoot.cpp`) emits exactly one `MountClaim`/"reclaim" event whose reason names
    /// the setting, and `CASMountClaim.UnsafeAuthorizationIsTokenExact` already pins the classification
    /// itself at the `claimMount` level.
    const std::vector<CasEvent> observed_events = events->snapshot();
    const auto reclaim_event = std::ranges::find_if(observed_events,
        [](const CasEvent & e) { return e.reason.find("cas_unsafe_remount_no_delay") != String::npos; });
    ASSERT_NE(reclaim_event, observed_events.end());
    EXPECT_EQ(reclaim_event->type, CasEventType::MountClaim);
    EXPECT_EQ(reclaim_event->outcome, "reclaim");
    EXPECT_EQ(decodeMountLease((*DB::Cas::tests::OperationForTest(b)).read(l.mountKey("test"), Retry::standard())->bytes).writer_epoch, 8u);
}

TEST(CASMountOpenWaits, CleanOpenSkipsAllWaits)
{
    auto b = std::make_shared<InMemoryBackend>();
    /// Predecessor released cleanly (drain + farewell from Task 5): open, then reset() drives ~Pool(),
    /// which -- with nothing in flight -- writes the farewell marker (min_active_build_sequence == UINT64_MAX).
    auto predecessor = Pool::open(b, PoolConfig{
        .pool_prefix = "p", .server_id = UInt128(1), .server_root_id = "test"});
    predecessor.reset();

    /// Heap-owned, not a plain local: the hook below mutates it, and the Pool can outlive this stack
    /// frame (a background publish holds `shared_from_this()`), so a by-reference capture would dangle.
    auto waits = std::make_shared<SharedWaitLog>();
    PoolPtr successor;
    ASSERT_NO_THROW(
        successor = Pool::open(b, PoolConfig{
            .pool_prefix = "p", .server_id = UInt128(1), .server_root_id = "test",
            .wait_sleep_fn = [waits](uint64_t ms)
            {
                waits->push(ms);
            },
        }));
    ASSERT_TRUE(successor);

    EXPECT_TRUE(waits->snapshot().empty())
        << "a clean farewell (Task 5) needs no observation window";
}

namespace
{
/// Reports the SHIPPED PRODUCTION default envelope (`CasRequestBudget{}`'s own defaults --
/// `attempt_timeout_ms=5000`, `connect_timeout_cap_ms=1000` -> `attemptEnvelopeMs()=7000`), so the
/// teardown below pays the SAME two-envelope reservation (14000 ms) production pays, not the
/// near-zero envelope a bare `InMemoryBackend` reports by default.
struct DefaultBudgetEnvelopeBackend : InMemoryBackend
{
    uint64_t attemptTimeoutMs() const override { return 5000; }
    uint64_t attemptEnvelopeMs() const override { return 7000; }
};
}

/// `CleanOpenSkipsAllWaits` above proves a clean farewell skips the observation window, but its bare
/// `InMemoryBackend` reports a zero attempt envelope, so its teardown never exercises the farewell's
/// own policy window against a write's real cost. Pin the shipped default budget specifically: a
/// window that cannot admit the write's `2 * attemptEnvelopeMs()` reservation refuses the farewell
/// before its first attempt, and the successor below then pays a full incarnation-stability
/// observation instead of reclaiming instantly.
TEST(CASMountOpenWaits, CleanTeardownUnderDefaultBudgetLeavesAFarewell)
{
    auto b = std::make_shared<DefaultBudgetEnvelopeBackend>();
    auto predecessor = Pool::open(b, PoolConfig{
        .pool_prefix = "p", .server_id = UInt128(1), .server_root_id = "test"});
    predecessor.reset();   /// drives ~Pool(): with nothing in flight, this is the graceful-shutdown farewell.

    const Layout layout{"p"};
    const auto got = readObj(*b, layout.mountKey("test"));
    ASSERT_TRUE(got.has_value());
    const MountLease lease = decodeMountLease(got->bytes);
    EXPECT_EQ(lease.min_active_build_sequence, std::numeric_limits<uint64_t>::max())
        << "the farewell's policy window must admit the write's own two-envelope reservation at the "
           "shipped default budget (2 * 7000 ms) -- otherwise a clean teardown never hands the mount "
           "slot back";

    /// Heap-owned, not a plain local: the hook below mutates it, and the Pool can outlive this stack
    /// frame (a background publish holds `shared_from_this()`), so a by-reference capture would dangle.
    auto waits = std::make_shared<SharedWaitLog>();
    PoolPtr successor;
    ASSERT_NO_THROW(
        successor = Pool::open(b, PoolConfig{
            .pool_prefix = "p", .server_id = UInt128(1), .server_root_id = "test",
            .wait_sleep_fn = [waits](uint64_t ms)
            {
                waits->push(ms);
            },
        }));
    ASSERT_TRUE(successor);
    EXPECT_TRUE(waits->snapshot().empty())
        << "a clean farewell needs no observation window on reopen, even at the shipped default budget";
}

TEST(CASMountOpenWaits, FencedPriorReclaimsWithoutAnyWait)
{
    auto b = std::make_shared<InMemoryBackend>();
    Layout l{"p"};
    DB::Cas::tests::seedPoolMetaForRestart(*b);
    ASSERT_EQ(claimMount(*DB::Cas::tests::OperationForTest(b), l, "test", UInt128(1), /*epoch*/ 7, /*now_ms*/ 1000, /*ttl_ms*/ 500).kind,
              MountClaimResult::Claimed);
    /// A real predecessor at epoch 7 durably minted it first (`allocateWriterEpoch` always runs
    /// before the mount claim); seed that durable epoch object here too, or the successor's own
    /// `allocateWriterEpoch` trips the Phase C guard (epoch absent, mount present -> fail closed).
    createObj(*b, l.epochKey("test"), encodeServerEpoch(ServerEpoch{.next_writer_epoch = 8}));
    /// Predecessor lease carries gc_fenced=true: fence it directly, exactly as `computeHeartbeatFloor`'s
    /// fence-out does (preserve the body, gc_fenced = true, seq + 1, token-guarded).
    fenceOutMount(*b, l.mountKey("test"));

    /// See UncleanOpenPaysOnlyTheObservationWindow above: a 500ms TTL needs a scaled-down budget too.
    const CasRequestBudget tiny_budget{
        .attempt_timeout_ms = 50, .lease_safety_margin_ms = 50, .connect_timeout_cap_ms = std::nullopt};

    /// Heap-owned, not a plain local: the hook below mutates it, and the Pool can outlive this stack
    /// frame (a background publish holds `shared_from_this()`), so a by-reference capture would dangle.
    auto waits = std::make_shared<SharedWaitLog>();
    PoolPtr store;
    ASSERT_NO_THROW(
        store = Pool::open(b, PoolConfig{
            .pool_prefix = "p", .server_id = UInt128(1), .server_root_id = "test",
            .mount_lease_ttl_ms = std::chrono::milliseconds(500),
            .cas_request_budget = tiny_budget,
            .wait_sleep_fn = [waits](uint64_t ms)
            {
                waits->push(ms);
            },
        }));
    ASSERT_TRUE(store);

    /// A GC-fenced prior is a terminal, already-threshold-gated certificate of death -- reclaimed on the
    /// FIRST attempt, with no observation polling. It is also an UNCLEAN prior, which used to mean it
    /// paid the materialization grace; nothing is owed now, so this open blocks on nothing at all.
    EXPECT_TRUE(waits->snapshot().empty())
        << "a certified-dead predecessor needs neither the observation window nor any grace period";
}

/// A reclaim arms the fence only when its claim still admits a ref append: more than two envelopes plus
/// the margin of lease, strictly, with no period term. With attempt 100 and cap 100 that is 650 ms of the
/// 1000 ms lease. A quiescence that leaves less reports success at step `claimed_not_armed` with the pool
/// not `Live`; the lease thread's next renewal arms it.
TEST(CASPoolRemount, RemountRenewerRedoUsesTheEnvelope)
{
    struct AtRemountEvent
    {
        String step;
        PoolLifecycle lifecycle = PoolLifecycle::IdentityLost;
        bool may_mutate = true;
    };
    /// One reclaim whose quiescence costs `quiesce_ms`; returns what the MountRemount event and the pool
    /// show while the lease thread is held inside the sink that reported it.
    const auto remountWithQuiesce = [](uint64_t quiesce_ms) -> AtRemountEvent
    {
        auto backend = std::make_shared<DB::Cas::tests::CountingBackend>();
        /// Held in shared state: the hooks below mutate it, and the Pool can outlive this lambda's frame.
        auto fake_boot = std::make_shared<std::atomic<uint64_t>>(1'000'000);
        auto committed = std::make_shared<DB::Cas::tests::ManualBarrier>();
        auto step = std::make_shared<String>();
        auto store = Pool::open(backend, PoolConfig{
            .pool_prefix = "remount-renewer-redo-envelope",
            .server_root_id = "test",
            .background_watermark = true,
            .event_sink = [committed, step](const CasEvent & event)
            {
                if (event.type == CasEventType::MountRemount && event.outcome == "ok")
                {
                    *step = event.detail.at("step");
                    committed->arriveAndWait();
                }
            },
            .mount_lease_ttl_ms = std::chrono::milliseconds(1000),
            .mount_renew_period = std::chrono::milliseconds(100),
            .cas_request_budget = CasRequestBudget{.attempt_timeout_ms = 100, .lease_safety_margin_ms = 50, .connect_timeout_cap_ms = 100},
            .boot_ms_fn = [fake_boot]
            {
                return fake_boot->load();
            },
            .wait_sleep_fn = [fake_boot](uint64_t ms)
            {
                *fake_boot += ms;
            },
            .remount_quiesce_hook_for_test = [fake_boot, quiesce_ms]
            {
                *fake_boot += quiesce_ms;
            },
        });
        fenceOutMount(*backend, store->layout().mountKey("test"));
        EXPECT_TRUE(store->scheduleRemountForTest()) << "the remount must be latched with quiesce_ms=" << quiesce_ms;
        committed->waitUntilArrived();
        AtRemountEvent seen{.step = *step, .lifecycle = store->lifecycle(), .may_mutate = store->mayMutate()};
        committed->release();
        return seen;
    };

    for (uint64_t quiesce_ms : {0, 300})
    {
        const AtRemountEvent seen = remountWithQuiesce(quiesce_ms);
        EXPECT_EQ(seen.step, "publish_live") << "quiesce_ms=" << quiesce_ms;
        EXPECT_EQ(seen.lifecycle, PoolLifecycle::Live) << "quiesce_ms=" << quiesce_ms;
        EXPECT_TRUE(seen.may_mutate) << "quiesce_ms=" << quiesce_ms;
    }
    /// 350: 650 ms left, exactly the reservation, refused. 450: 550 ms left; two bare attempts plus the
    /// margin (250) would fit, two envelopes do not.
    for (uint64_t quiesce_ms : {350, 450})
    {
        const AtRemountEvent seen = remountWithQuiesce(quiesce_ms);
        EXPECT_EQ(seen.step, "claimed_not_armed") << "quiesce_ms=" << quiesce_ms;
        EXPECT_EQ(seen.lifecycle, PoolLifecycle::TransientNotLive) << "quiesce_ms=" << quiesce_ms;
        EXPECT_FALSE(seen.may_mutate) << "quiesce_ms=" << quiesce_ms;
    }
}

namespace
{
/// Ages the claim on its own I/O, and counts what the open writes afterwards.
///
/// The mount key is written twice before the open decides whether to arm: once by `claimMount`'s
/// reclaim, then once by the renewer's adopt, and the claim's start is taken BETWEEN them. A hook on
/// the SECOND write therefore ages the claim the open arms from.
class StalledMountClaimBackend final : public DB::Cas::InMemoryBackend
{
public:
    String mount_key;
    std::function<void()> on_second_mount_write;
    std::atomic<int> mount_writes{0};
    std::atomic<int> mount_writes_after_stall{0};

    /// The hook sits on the WRITE PRIMITIVE: both the reclaim and the renewer's adopt reach the mount
    /// slot through it. It counts only CONDITIONAL overwrites, which is what every production mount-slot
    /// write is -- the unconditional create that seeds the predecessor lease reaches this same virtual
    /// too, and counting it would shift the stall onto the reclaim instead of the adopt.
    std::expected<String, RawConflict> write(const String & key, const String & bytes,
        const std::optional<String> & expected_value, DB::Cas::TransportAccess & access) override
    {
        if (key == mount_key && expected_value)
        {
            const int n = ++mount_writes;
            if (n == 2 && on_second_mount_write)
                on_second_mount_write();
            else if (n > 2)
                ++mount_writes_after_stall;
        }
        return InMemoryBackend::write(key, bytes, expected_value, access);
    }
};
}

/// A claim path that consumed most of the lease must not arm the fence from that claim: the open waits for
/// the lease thread's first renewal, which writes the lease once more and arms from its own start.
TEST(CASPool, StartupArmRedoesLeaseWriteWhenTheClaimConsumesTtl)
{
    auto backend = std::make_shared<StalledMountClaimBackend>();
    DB::Cas::Layout layout("pool");
    DB::Cas::tests::seedPoolMetaForRestart(*backend, "pool");
    const String srid = "s";
    const DB::UInt128 uuid(0x42);
    backend->mount_key = layout.mountKey(srid);

    /// Seed a FENCED, expired predecessor body under a DIFFERENT epoch (7, matching
    /// `FencedPriorPaysOnlyTmat`'s convention). The durable epoch object seeded a few lines below
    /// carries `next_writer_epoch = 8`, so THIS pool's own first-allocated `writer_epoch` is 8 --
    /// non-colliding with the seeded epoch-7 prior by construction. With no collision the first
    /// (and only) claim attempt reclaims directly with MountPriorState::Fenced, with no silent
    /// FencedSelf fence-recovery detour to account for -- so the mount key is written exactly twice
    /// before the arm, which is what the stall hook counts on.
    {
        DB::Cas::MountLease prior;
        prior.server_uuid = uuid;
        prior.writer_epoch = 7;
        prior.seq = 7;
        prior.expires_at_ms = 1;      /// long expired
        prior.gc_fenced = true;
        prior.write_attempt_id = DB::UInt128{7};
        createObj(*backend, layout.mountKey(srid), DB::Cas::encodeMountLease(prior));
    }
    /// A real predecessor at epoch 7 durably minted it first (`allocateWriterEpoch` always runs
    /// before the mount claim); seed that durable epoch object here too, or `Pool::open`'s own
    /// `allocateWriterEpoch` trips the Phase C guard (epoch absent, mount present -> fail closed).
    createObj(*backend, layout.epochKey(srid), DB::Cas::encodeServerEpoch(DB::Cas::ServerEpoch{.next_writer_epoch = 8}));
    /// Held in a shared atomic, not a plain local: `on_second_mount_write` below mutates it, and the
    /// Pool can outlive this stack frame (a background publish holds `shared_from_this()`), so a
    /// by-reference capture of a local would dangle.
    auto fake_boot_ms = std::make_shared<std::atomic<uint64_t>>(10'000);
    DB::Cas::PoolConfig cfg;
    cfg.pool_prefix = "pool";
    cfg.server_id = uuid;
    cfg.server_root_id = srid;
    cfg.background_watermark = true;
    cfg.mount_lease_ttl_ms = std::chrono::milliseconds(30'000);
    cfg.boot_ms_fn = [fake_boot_ms]
    {
        return fake_boot_ms->load();
    };
    /// The renewer's adopt write stalls for 15 s of boot clock: 15 s of the lease are left, a ref append
    /// needs 2 x 7 s + 2 s = 16 s, so the open waits; the loop's renewal is due at once (10 s cadence).
    backend->on_second_mount_write = [fake_boot_ms]
    {
        *fake_boot_ms += 15'000;
    };

    auto store = DB::Cas::Pool::open(backend, cfg);
    ASSERT_NE(store, nullptr);

    ASSERT_EQ(backend->mount_writes.load(), 3)
        << "the fixture assumes exactly two mount writes before the open decides (the reclaim and the "
           "renewer's adopt, with the claim's start between them), then the loop's renewal";
    EXPECT_EQ(backend->mount_writes_after_stall.load(), 1)
        << "a claim that consumed the lease is followed by exactly one renewal before the open returns";
    EXPECT_TRUE(store->mayMutate()) << "the open returns with the fence armed";
}

/// The arming rule at an open reserves what a ref append needs: two attempt envelopes, connect cap
/// included, plus the margin, strictly, and no renewal period. With attempt 100 and cap 100 the envelope is
/// 300 ms, so a claim arms the fence only while more than 2 x 300 + 50 = 650 ms of its 1000 ms lease remain.
/// The 340 ms period keeps the lease thread from renewing an armed claim before the count is read.
TEST(CASMountOpenWaits, PublicationHorizonUsesTheEnvelope)
{
    /// Opens over a fenced predecessor whose adopt write ages the claim by `claim_age_ms`, and returns the
    /// conditional writes of the mount key made before the open returned.
    const auto mountWritesAtOpen = [](uint64_t claim_age_ms) -> int
    {
        auto backend = std::make_shared<StalledMountClaimBackend>();
        DB::Cas::Layout layout("pool");
        DB::Cas::tests::seedPoolMetaForRestart(*backend, "pool");
        backend->mount_key = layout.mountKey("s");
        MountLease prior;
        prior.server_uuid = DB::UInt128(0x42);
        prior.writer_epoch = 7;
        prior.seq = 7;
        prior.expires_at_ms = 1;
        prior.gc_fenced = true;
        prior.write_attempt_id = DB::UInt128{7};
        createObj(*backend, layout.mountKey("s"), encodeMountLease(prior));
        createObj(*backend, layout.epochKey("s"), encodeServerEpoch(ServerEpoch{.next_writer_epoch = 8}));
        /// Held in a shared atomic: the Pool can outlive this lambda's frame.
        auto fake_boot = std::make_shared<std::atomic<uint64_t>>(10'000);
        backend->on_second_mount_write = [fake_boot, claim_age_ms]
        {
            *fake_boot += claim_age_ms;
        };
        DB::Cas::PoolConfig cfg;
        cfg.pool_prefix = "pool";
        cfg.server_id = DB::UInt128(0x42);
        cfg.server_root_id = "s";
        cfg.background_watermark = true;
        cfg.mount_lease_ttl_ms = std::chrono::milliseconds(1000);
        cfg.mount_renew_period = std::chrono::milliseconds(340);
        cfg.cas_request_budget = CasRequestBudget{.attempt_timeout_ms = 100, .lease_safety_margin_ms = 50, .connect_timeout_cap_ms = 100};
        cfg.boot_ms_fn = [fake_boot]
        {
            return fake_boot->load();
        };
        cfg.wait_sleep_fn = [fake_boot](uint64_t ms)
        {
            *fake_boot += ms;
        };
        auto store = Pool::open(backend, cfg);
        EXPECT_TRUE(store && store->mayMutate()) << "claim_age_ms=" << claim_age_ms;
        return backend->mount_writes.load();
    };

    /// 1000 ms left: the claim arms; the reclaim and the adopt only.
    EXPECT_EQ(mountWritesAtOpen(0), 2);
    /// 700 ms left: the claim arms. A window with the period in it (340 + 600 = 940) would not fit here.
    EXPECT_EQ(mountWritesAtOpen(300), 2) << "the arming rule has no period term";
    /// 650 ms left, exactly the reservation: refused, strict like `admit`; the loop's renewal arms.
    EXPECT_EQ(mountWritesAtOpen(350), 3) << "an exact boundary must be refused";
    /// 600 ms left: two bare attempts plus the margin (250) would fit, two envelopes (650) do not.
    EXPECT_EQ(mountWritesAtOpen(400), 3) << "the reservation counts envelopes, not bare attempts";
}

/// ==== What a self-remount may block on ====
///
/// Nothing an operator configures. The remount used to consult `refLanesSettledForRemount` and pay the
/// materialization grace whenever a ref lane still held an undecided `PUT`; both are retired, because
/// the undecided `PUT` is settled by the protocol rather than waited out — recovery closes the dead
/// epoch with an in-band `EpochSeal` written as a conditional create, and the straggler's own create
/// loses to it. `gtest_cas_retirement_sweep.cpp` proves that conflict directly; these two pin that the
/// wait is gone from both the drained and the still-wedged path.

TEST(CASRemountWaits, DrainedRemountPaysNoWait)
{
    auto backend = std::make_shared<InMemoryBackend>();
    /// Held in shared, heap-owned state, not plain locals: the hooks below mutate them, and the Pool
    /// can outlive this stack frame (a background publish holds `shared_from_this()`), so a
    /// by-reference capture of a local would dangle.
    auto fake_boot = std::make_shared<std::atomic<uint64_t>>(1'000'000);
    auto waits = std::make_shared<SharedWaitLog>();
    auto store = Pool::open(backend, PoolConfig{
        .pool_prefix = "p", .server_root_id = "test",
        .mount_lease_ttl_ms = std::chrono::milliseconds(30000),
        .boot_ms_fn = [fake_boot]
        {
            return fake_boot->load();
        },
        .wait_sleep_fn = [fake_boot, waits](uint64_t ms)
        {
            *fake_boot += ms;
            waits->push(ms);
        },
    });
    ASSERT_TRUE(store);
    EXPECT_TRUE(waits->snapshot().empty()) << "a fresh mount (no predecessor) pays no wait at open";
    store->setCasRetrySleepForTest([fake_boot](uint64_t ms)
    {
        *fake_boot += ms;
    });

    /// Trip the fence: advance the local boot clock past the deadline (as in `WriteFenceUsesInjectedBootClock`
    /// above) and mark the durable lease `gc_fenced` (the certificate `claimMountAwaitingExpiry` reclaims
    /// on its FIRST attempt, no observation polling -- avoids a real sleep in this test).
    *fake_boot += 30001;
    fenceOutMount(*backend, store->layout().mountKey("test"));

    /// No in-flight ref-log PUT at all -- the easy direction.
    ASSERT_TRUE(store->tryRemountOnce());

    EXPECT_TRUE(waits->snapshot().empty())
        << "a drained self-remount must pay no wait";
}

TEST(CASRemountWaits, UnresolvedWedgeRemountPaysNoWaitEither)
{
    CasRequestBudget budget;
    budget.attempt_timeout_ms = 100;
    budget.lease_safety_margin_ms = 100;

    auto backend = std::make_shared<UnresolvedPutBackend>();
    /// What the request engine reserves per attempt is the BACKEND's attempt timeout, not the budget
    /// field alone; pair the two so the mount lease's admission arithmetic sees what the budget claims.
    backend->setAttemptTimeoutMs(budget.attempt_timeout_ms);
    /// Held in shared, heap-owned state, not plain locals: the hooks below mutate them, and the Pool
    /// can outlive this stack frame (a background publish holds `shared_from_this()`), so a
    /// by-reference capture of a local would dangle.
    auto fake_boot = std::make_shared<std::atomic<uint64_t>>(1'000'000);
    auto waits = std::make_shared<SharedWaitLog>();
    auto store = Pool::open(backend, PoolConfig{
        .pool_prefix = "p", .server_root_id = "test",
        .mount_lease_ttl_ms = std::chrono::milliseconds(30000),
        .cas_request_budget = budget,
        .boot_ms_fn = [fake_boot]
        {
            return fake_boot->load();
        },
        .wait_sleep_fn = [fake_boot, waits](uint64_t ms)
        {
            *fake_boot += ms;
            waits->push(ms);
        },
    });
    ASSERT_TRUE(store);
    EXPECT_TRUE(waits->snapshot().empty()) << "a fresh mount (no predecessor) pays no wait at open";
    /// `dropRef` below drives the fault through `ensureRefTableRecovered`'s own recovery-retry loop,
    /// which sleeps via `recovery_retry_sleep_fn` (a REAL 200ms-slice sleep by default) while measuring
    /// elapsed time against `boot_ms_now_fn` -- the frozen `fake_boot` this fixture already injects.
    /// Without also virtualizing the sleep, that elapsed check never advances and the loop spins for
    /// real until the harness times the test out.
    store->setCasRetrySleepForTest([fake_boot](uint64_t ms)
    {
        *fake_boot += ms;
    });

    const Layout & layout = store->layout();
    const RootNamespace ns{"srv/remount_wedge"};
    /// Stage B (Task 4-C): see `CASPoolShutdown.UnresolvedWedgeSkipsFarewell`'s identical comment.
    DB::Cas::tests::casAdmitRecoverableEntry(*backend, layout, ns, store->liveWriterEpoch());
    publishPart(store, ns.string(), "x", "payload");

    /// Force the ref-log append `dropRef` below performs into the Unresolved/wedge outcome (as in
    /// `CASPoolShutdown.UnresolvedWedgeSkipsFarewell`): the single attempt the budget allows fails
    /// ambiguously.
    backend->fault_key_substr = layout.namespaceStreamPrefix(DB::Cas::tests::fixture::fixtureLife(ns)) + "_log/";
    backend->fault_count = std::numeric_limits<int>::max();
    expectThrowsCode(DB::ErrorCodes::NETWORK_ERROR, [&] { store->dropRef(ns, "x"); });
    ASSERT_TRUE(store->refLaneWedgedForTest(ns));

    /// Trip the fence exactly as in `DrainedRemountSkipsGrace` above.
    *fake_boot += 30001;
    fenceOutMount(*backend, store->layout().mountKey("test"));

    /// THE HARD DIRECTION, and the one the retired wait existed for: a ref lane that still holds an
    /// UNDECIDED conditional PUT when the fence trips. It used to buy a 30 s grace. It buys nothing now
    /// -- the remount proceeds straight through, and the undecided PUT is decided by the seal the next
    /// recovery writes into its slot.
    ASSERT_TRUE(store->tryRemountOnce());

    EXPECT_TRUE(waits->snapshot().empty())
        << "an unresolved ref-lane wedge must not make the remount block: the straggler it describes is "
           "fenced by the recovery seal, not waited out";
}

/// Sealing is decided by ARITHMETIC -- `epoch < live_epoch` -- and by nothing else. This test used to
/// pin the opposite ("a table recovered under a later CLEAN boundary must not seal"), which was the
/// right rule while a seal was a synthetic SNAPSHOT published only to close an unclean handover: such a
/// seal after a clean shutdown was pure parasitic cost, so it was gated on the per-epoch unclean flag.
///
/// INV-2's seal is not that object. It is the chain link that makes a MISSING epoch detectable across a
/// transition, and a chain that skips every epoch whose mount happened to shut down cleanly is not a
/// chain -- the next sequence-1 transaction would have no `prev_epoch_seal` to name, and no reader could
/// tell "epoch 2 was empty" from "epoch 2's records are gone". So a late-touched table now closes EVERY
/// dead epoch below the live one, however its predecessors died, and this test pins that plus the two
/// things that must still be true: the seals land IN-BAND (at log keys, at the slot a straggler would
/// have taken) and no synthetic seal SNAPSHOT is written anywhere.
TEST(CASRemountWaits, ALateTouchedTableClosesEveryDeadEpochInBandHoweverItsPredecessorsDied)
{
    CasRequestBudget budget;
    budget.attempt_timeout_ms = 100;
    budget.lease_safety_margin_ms = 100;

    auto backend = std::make_shared<UnresolvedPutBackend>();
    /// What the request engine reserves per attempt is the BACKEND's attempt timeout, not the budget
    /// field alone; pair the two so the mount lease's admission arithmetic sees what the budget claims.
    backend->setAttemptTimeoutMs(budget.attempt_timeout_ms);
    /// Held in a shared atomic, not a plain local: the hooks below mutate it, and the Pool can outlive
    /// this stack frame (a background publish holds `shared_from_this()`), so a by-reference capture
    /// of a local would dangle.
    auto fake_boot = std::make_shared<std::atomic<uint64_t>>(1'000'000);
    auto store = Pool::open(backend, PoolConfig{
        .pool_prefix = "p", .server_root_id = "test",
        .mount_lease_ttl_ms = std::chrono::milliseconds(30000),
        .cas_request_budget = budget,
        .boot_ms_fn = [fake_boot]
        {
            return fake_boot->load();
        },
        .wait_sleep_fn = [fake_boot](uint64_t ms)
        {
            *fake_boot += ms;
        },
    });
    ASSERT_TRUE(store);
    store->setCasRetrySleepForTest([fake_boot](uint64_t ms)
    {
        *fake_boot += ms;
    });

    const Layout & layout = store->layout();
    const RootNamespace ns1{"srv/table_a"};
    const RootNamespace ns2{"srv/table_b"};
    /// Stage B (Task 4-C): `ns1` is pinned because the fault below targets its key by exact sentinel
    /// match. `ns2` must ALSO be pinned: the epoch-close assertions further down read its ref-log keys
    /// directly at `DB::Cas::tests::fixture::fixtureLife(ns2)`.
    DB::Cas::tests::casAdmitRecoverableEntry(*backend, layout, ns1, store->liveWriterEpoch());
    DB::Cas::tests::casAdmitRecoverableEntry(*backend, layout, ns2, store->liveWriterEpoch());
    publishPart(store, ns1.string(), "x", "payload-a");
    /// ns2's epoch-1 data: never touched again by this incarnation until the final check below, well
    /// after both remounts -- the "table recovered for the first time, late" the fix must not over-seal.
    /// Distinct content from ns1's part: identical payloads collide on the same blob and race
    /// `PartWriteTxn::ensureBlobPresent`'s mandatory observation, unrelated to what this test is about.
    publishPart(store, ns2.string(), "y", "payload-b");

    /// Force ns1's ref-log append into the Unresolved/wedge outcome (mirrors
    /// `UnresolvedWedgeRemountPaysNoWaitEither` above).
    backend->fault_key_substr = layout.namespaceStreamPrefix(DB::Cas::tests::fixture::fixtureLife(ns1)) + "_log/";
    backend->fault_count = std::numeric_limits<int>::max();
    expectThrowsCode(DB::ErrorCodes::NETWORK_ERROR, [&] { store->dropRef(ns1, "x"); });
    ASSERT_TRUE(store->refLaneWedgedForTest(ns1));

    /// Self-remount #1: UNCLEAN (the wedge above). Epoch 1 -> 2.
    *fake_boot += 30001;
    fenceOutMount(*backend, store->layout().mountKey("test"));
    ASSERT_TRUE(store->tryRemountOnce());
    ASSERT_EQ(store->liveWriterEpoch(), 2u);

    /// Self-remount #2: CLEAN (no wedge left behind -- `quiesceRefTablesForRemount` already cleared the
    /// cache). Epoch 2 -> 3.
    *fake_boot += 30001;
    fenceOutMount(*backend, store->layout().mountKey("test"));
    ASSERT_TRUE(store->tryRemountOnce());
    ASSERT_EQ(store->liveWriterEpoch(), 3u);

    using ProfileEvents::global_counters;
    const auto sealed_before = global_counters[ProfileEvents::CASRefRecoveryEpochSealed].load();

    /// ns2's FIRST recovery under this incarnation happens now, at epoch 3 -- strictly after both
    /// remounts. Its only data is at epoch 1, so epochs 1 and 2 are both dead for it.
    EXPECT_EQ(store->listRefs(ns2).size(), 1u);

    EXPECT_EQ(global_counters[ProfileEvents::CASRefRecoveryEpochSealed].load(), sealed_before + 2)
        << "both dead epochs must be closed -- the chain link is what a later reader needs to tell an "
           "EMPTY epoch from a LOST one, and that is independent of how each mount ended";
    EXPECT_TRUE(readObj(*backend, layout.refLogKey(DB::Cas::tests::fixture::fixtureLife(ns2), RefTxnId{1, 2})).has_value())
        << "epoch 1 closes at the slot right after its last durable id, in-band";
    EXPECT_TRUE(readObj(*backend, layout.refLogKey(DB::Cas::tests::fixture::fixtureLife(ns2), RefTxnId{2, 1})).has_value())
        << "empty epoch 2 closes at its own sequence 1, chained to the epoch-1 seal";
    const RefTxnId retired_sentinel_id{2, std::numeric_limits<uint64_t>::max()};
    EXPECT_FALSE(readObj(*backend, layout.refSnapshotKey(DB::Cas::tests::fixture::fixtureLife(ns2), retired_sentinel_id)).has_value())
        << "and NO synthetic seal snapshot is written: that shape is retired";
}

TEST(CASPool, ReadManifestSharedReturnsSharedDecodeWithoutCopy)
{
    auto backend = std::make_shared<DB::Cas::tests::CountingBackend>();
    const DB::Cas::Layout layout("p");
    DB::Cas::tests::seedPoolMetaForRestart(*backend);
    const DB::Cas::RootNamespace ns{"srv/t1"};
    const DB::Cas::ManifestRef ref{.writer_epoch = 1, .build_sequence = 1, .manifest_ordinal = 1};
    const auto id = DB::Cas::tests::writeManifestRaw(*backend, layout, ns, ref,
        {DB::Cas::tests::blobEntryFor("data.bin", DB::UInt128(7))});
    DB::Cas::tests::fixture::writeRefLogRaw(*backend, layout, RefLogTxn{ns.string(), RefTxnId{1, 1},
        {DB::Cas::tests::namespaceBirthOp(), DB::Cas::tests::publishCommittedOps("part_1", ref)[0],
         DB::Cas::tests::publishCommittedOps("part_1", ref)[1]}, std::nullopt});
    DB::Cas::tests::writeRecoverableCkptForRawFixture(*backend, layout, ns, RefCkpt{
        .life_epoch = 1,
        .committed_through = RefTxnId{1, 1},
        .checkpoint_snapshot_id = std::nullopt,
        .last_epoch_seal = std::nullopt,
    });

    auto store = DB::Cas::Pool::open(backend,
        DB::Cas::PoolConfig{.pool_prefix = "p", .server_root_id = "test"});
    const auto resolved = store->resolveRef(ns, "part_1");
    ASSERT_TRUE(resolved.has_value());

    const String manifest_key = layout.manifestKey(id);
    backend->resetCounts();

    auto m1 = store->readManifestShared(resolved->manifest_id);
    auto m2 = store->readManifestShared(resolved->manifest_id);
    EXPECT_EQ(m1.get(), m2.get());                          /// the SAME shared decode, no copy
    EXPECT_EQ(backend->getCount(manifest_key), 1u);         /// one body GET
    EXPECT_EQ(backend->headCount(manifest_key), 0u);        /// keyed by id: no HEAD on a miss or a hit
    ASSERT_EQ(m1->entries.size(), 1u);
    EXPECT_EQ(m1->entries[0].path, "data.bin");
}

/// A miss whose object is absent is the one dangling-reference case the reader still detects
/// itself: exactly one GET, no HEAD, one `ReadMissing` event, FILE_DOESNT_EXIST.
TEST(CASPool, ReadManifestAbsentBodyEmitsReadMissingWithOneGetAndNoHead)
{
    auto b = std::make_shared<DB::Cas::tests::CountingBackend>();
    auto s = Pool::open(b, PoolConfig{.pool_prefix = "p", .server_root_id = "test"});
    Layout layout("p");
    const RootNamespace ns{"srv1/tbl"};
    const ManifestId id{.root_namespace = ns, .ref = manifestRefFor("absent-body-event")};
    const String key = layout.manifestKey(id);

    /// Heap-owned, not a plain local: `setEventSink(nullptr)` below only stops FUTURE sink installs from
    /// using this closure -- it does not guarantee an already-in-flight background call is not still
    /// executing the old one -- and the Pool can outlive this stack frame regardless (a background
    /// publish holds `shared_from_this()`).
    auto events = std::make_shared<DB::Cas::tests::SharedEventLog>();
    s->setEventSink([events](CasEvent e)
    {
        events->push(std::move(e));
    });
    b->resetCounts();
    expectThrowsCode(DB::ErrorCodes::FILE_DOESNT_EXIST, [&] { s->readManifest(id); });
    s->setEventSink(nullptr);

    EXPECT_EQ(b->getCount(key), 1u);
    EXPECT_EQ(b->headCount(key), 0u);
    size_t read_missing = 0;
    for (const auto & e : events->snapshot())
    {
        if (e.type != CasEventType::ReadMissing)
            continue;
        ++read_missing;
        EXPECT_EQ(e.object_kind, CasEventObjectKind::Manifest);
        EXPECT_EQ(e.detail.at("code"), "FILE_DOESNT_EXIST");
        EXPECT_EQ(e.detail.at("site"), "readManifest");
    }
    EXPECT_EQ(read_missing, 1u);
}

/// A reader holding a decode for a manifest the collector has since removed sees a snapshot-consistent
/// manifest: the second read is the same shared decode with no request, `locate` is pure, and the
/// missing blob is observed only when its key is read. Nothing here is a fallback: the absence is
/// surfaced by the blob read, never masked by the cache.
TEST(CASPool, StaleSnapshotServesCachedManifestAndBlobAbsenceSurfacesOnRead)
{
    auto b = std::make_shared<DB::Cas::tests::CountingBackend>();
    auto s = Pool::open(b, PoolConfig{.pool_prefix = "p", .server_root_id = "test"});
    Layout layout("p");
    const RootNamespace ns{"srv1/tbl"};

    const ManifestId id = publishPart(s, ns.string(), "part_1", "payload-1");
    auto r = s->resolveRef(ns, "part_1");
    ASSERT_TRUE(r.has_value());
    auto m1 = s->readManifestShared(r->manifest_id);
    const String manifest_key = layout.manifestKey(id);
    const String blob_key = layout.blobKey(idOf("payload-1"));

    /// What GC does after the owner is removed and the decrement is adopted: exact-token deletes of
    /// the body and of the now-unreferenced blob.
    {
        DB::Cas::tests::OperationForTest op(*b);
        const auto h = (*op).head(manifest_key, Retry::standard());
        ASSERT_TRUE(h.has_value());
        (*op).remove(manifest_key, h->etag, Retry::once());
    }
    {
        DB::Cas::tests::OperationForTest op(*b);
        const auto h = (*op).head(blob_key, Retry::standard());
        ASSERT_TRUE(h.has_value());
        (*op).remove(blob_key, h->etag, Retry::once());
    }
    b->resetCounts();

    auto m2 = s->readManifestShared(r->manifest_id);
    EXPECT_EQ(m1.get(), m2.get());
    EXPECT_EQ(b->getCount(manifest_key), 0u);
    EXPECT_EQ(b->headCount(manifest_key), 0u);

    ASSERT_EQ(m2->entries.size(), 1u);
    const BlobLocation location = s->locate(m2->entries[0]);
    EXPECT_EQ(location.key, blob_key);
    EXPECT_EQ(b->getCount(blob_key), 0u);          /// locate is pure: no I/O until the read
    EXPECT_FALSE(readObj(*b, location.key).has_value());   /// the read observes the absence
}

/// The scoped contract for mutation evidence, executable. A carry-forward from a committed source
/// (what createHardLink, republishRef, repointRef and the relink receiver do) adopts each entry as a
/// tokenless TrustedManifest dependency and promote issues no probe for it: the live source edge is
/// what keeps the blob alive, and under protocol-compliant GC the state "cached source decode, blob
/// gone" cannot be constructed. Out-of-band deletion of BOTH the cached source body and the blob is
/// outside that contract; the carry-forward then commits a ref to an absent blob and fsck's
/// reachable-but-absent scan is the detector. This test pins that documented outcome so a later
/// change that silently alters it is noticed. It is not a defect report.
TEST(CASPool, CachedSourceDecodeLetsAdoptionCommitAnAbsentBlobThatFsckReports)
{
    auto b = std::make_shared<DB::Cas::tests::CountingBackend>();
    auto s = Pool::open(b, PoolConfig{.pool_prefix = "p", .server_root_id = "test"});
    Layout layout("p");
    const RootNamespace ns{"srv1/tbl"};

    const ManifestId src_id = publishPart(s, ns.string(), "part_src", "payload-src");
    auto src = s->resolveRef(ns, "part_src");
    ASSERT_TRUE(src.has_value());
    const auto src_manifest = s->readManifestShared(src->manifest_id);   /// warms the decode cache
    const String src_manifest_key = layout.manifestKey(src_id);
    const String blob_key = layout.blobKey(idOf("payload-src"));

    /// Out of band: both objects gone, the committed source ref untouched.
    for (const String & key : {src_manifest_key, blob_key})
    {
        DB::Cas::tests::OperationForTest op(*b);
        const auto h = (*op).head(key, Retry::standard());
        ASSERT_TRUE(h.has_value());
        (*op).remove(key, h->etag, Retry::once());
    }
    b->resetCounts();

    /// The carry-forward reaches its source through the reader, the way every production caller does,
    /// and the cache answers: the same decode as before, with no request on the body just deleted.
    /// Adopting from the `shared_ptr` held across the deletion would prove nothing about the cache --
    /// were the cache to stop retaining, this re-read would fetch and throw, and the rest of this
    /// scenario would be unreachable in production for the same reason.
    const auto cached_manifest = s->readManifestShared(src->manifest_id);
    ASSERT_EQ(cached_manifest.get(), src_manifest.get());
    EXPECT_EQ(b->getCount(src_manifest_key), 0u);
    EXPECT_EQ(b->headCount(src_manifest_key), 0u);

    /// The carry-forward, in the order prepareEntries runs it for a committed source: adopt, stage,
    /// precommit, promote. No blob body is written.
    PartWriteInfo info;
    info.intended_ref = ns.string() + "/part_dst";
    info.intended_namespace = ns;
    auto build = s->beginPartWrite(info);
    ASSERT_EQ(src_manifest->entries.size(), 1u);
    build->adoptEvidence(cached_manifest->entries[0]);
    const ManifestId dst_id = build->stageManifest({cached_manifest->entries[0]});
    build->precommitAdd(ns, "part_dst", dst_id);
    EXPECT_NO_THROW(build->promote(ns, "part_dst", build->buildId(), dst_id));
    EXPECT_EQ(b->headCount(blob_key), 0u);   /// a TrustedManifest leaf is not probed, by design
    EXPECT_EQ(b->getCount(blob_key), 0u);

    /// The documented outcome: a committed ref names an absent blob, and fsck reports it.
    ASSERT_TRUE(s->resolveRef(ns, "part_dst").has_value());
    const FsckReport rep = runFsck(*s, /*detail=*/true);
    EXPECT_GE(rep.dangling, 1u);
    bool blob_reported = false;
    for (const FsckObject & o : rep.objects)
        if (o.key == blob_key && o.cls == FsckClass::Dangling)
            blob_reported = true;
    EXPECT_TRUE(blob_reported) << "fsck must report the adopted-but-absent blob " << blob_key;
}

#if defined(DEBUG_OR_SANITIZER_BUILD)
#define EXPECT_RUNTIME_STATE_REJECTION(statement) EXPECT_DEATH({ statement; }, "CAS mount runtime")
#else
#define EXPECT_RUNTIME_STATE_REJECTION(statement) EXPECT_THROW(statement, DB::Exception)
#endif

TEST(CASPoolRemount, DirectRenewIsRefusedForBackgroundConfiguredRuntimeAfterStop)
{
    auto backend = std::make_shared<RuntimeRenewBackend>();
    const Layout layout("runtime-direct-after-stop");
    uint64_t wall_ms = 1000;
    uint64_t boot_ms = 100;
    const UInt128 uuid{1};
    ASSERT_EQ(claimMount(*DB::Cas::tests::OperationForTest(backend), layout, "test", uuid, 1, wall_ms, 1000).kind, MountClaimResult::Claimed);
    CasEventSink sink;
    RuntimeUnderTest runtime_holder(
        backend, layout,
        MountConfig{.mount_lease_ttl_ms = std::chrono::milliseconds(1000), .background_watermark = true,
                    .boot_ms_fn = [&] { return boot_ms; }},
        "test", sink, runtimeRenewBudget(), [] { return false; });
    CasMountRuntime & runtime = *runtime_holder;
    runtime.installRenewer(uuid, 1, [&] { return wall_ms; });
    const uint64_t anchor = runtime.startRenewer();
    runtime.armMountFence(uuid, 1, anchor + 1000);
    runtime.startBackgroundWorkers(std::chrono::hours(1));
    runtime.stopBackgroundWorkers();
    EXPECT_RUNTIME_STATE_REJECTION(runtime.renewWatermarkOnce());
    runtime.finishTeardown(true);
}

#undef EXPECT_RUNTIME_STATE_REJECTION

TEST(CASPoolRemount, ReclaimWaitsForTheRenewalToEnd)
{
    auto backend = std::make_shared<RuntimeRenewBackend>();
    const Layout layout("runtime-reclaim-waits");
    uint64_t wall_ms = 1000;
    uint64_t boot_ms = 10'000;
    const UInt128 uuid{1};
    ASSERT_EQ(claimMount(*DB::Cas::tests::OperationForTest(backend), layout, "test", uuid, 1, wall_ms, 1000).kind, MountClaimResult::Claimed);
    DB::Cas::tests::ManualBarrier renewal_barrier;
    DB::Cas::tests::ManualBarrier remount_barrier;
    std::atomic<uint64_t> remount_calls{0};
    std::atomic<bool> renewal_request_returned{false};
    std::atomic<bool> reclaim_saw_the_request_returned{false};
    CasEventSink sink;
    RuntimeUnderTest runtime_holder(
        backend, layout,
        MountConfig{.mount_lease_ttl_ms = std::chrono::milliseconds(1000), .background_watermark = true,
                    .boot_ms_fn = [&] { return boot_ms; }},
        "test", sink, runtimeRenewBudget(), [&]
        {
            ++remount_calls;
            reclaim_saw_the_request_returned = renewal_request_returned.load();
            remount_barrier.arriveAndWait();
            return false;
        });
    CasMountRuntime & runtime = *runtime_holder;
    runtime.installRenewer(uuid, 1, [&] { return wall_ms; });
    const uint64_t anchor = runtime.startRenewer();
    runtime.armMountFence(uuid, 1, anchor + 1000);
    backend->barrier = &renewal_barrier;
    backend->fault = RuntimeRenewBackend::Fault::BlockThenDelegate;
    /// Set after the setup's own writes: only the held renewal request sets it.
    backend->after_commit = [&] { renewal_request_returned = true; };
    runtime.startBackgroundWorkers(std::chrono::milliseconds(0));
    renewal_barrier.waitUntilArrived();
    runtime.tripMountLost();
    runtime.scheduleRemount();
    EXPECT_EQ(remount_calls.load(), 0u) << "the reclaim must wait until the renewal in flight ends";
    renewal_barrier.release();
    remount_barrier.waitUntilArrived();
    EXPECT_EQ(remount_calls.load(), 1u);
    EXPECT_TRUE(reclaim_saw_the_request_returned.load())
        << "the reclaim must start only after the renewal's request in flight returned";
    remount_barrier.release();
    runtime.stopBackgroundWorkers();
    runtime.finishTeardown(false);
}

TEST(CASPoolRemount, TeardownJoinsTheLeaseThreadBeforeRelease)
{
    auto backend = std::make_shared<RuntimeRenewBackend>();
    const Layout layout("runtime-join");
    uint64_t wall_ms = 1000;
    uint64_t boot_ms = 100;
    const UInt128 uuid{1};
    ASSERT_EQ(claimMount(*DB::Cas::tests::OperationForTest(backend), layout, "test", uuid, 1, wall_ms, 1000).kind, MountClaimResult::Claimed);
    std::atomic<uint64_t> worker_exits{0};
    RuntimeWorkerFactory factory = [&](std::function<void()> worker_body)
    {
        return ThreadFromGlobalPool([&, body = std::move(worker_body)]
        {
            body();
            ++worker_exits;
        });
    };
    CasEventSink sink;
    RuntimeUnderTest runtime_holder(
        backend, layout,
        MountConfig{.mount_lease_ttl_ms = std::chrono::milliseconds(1000), .background_watermark = true,
                    .boot_ms_fn = [&] { return boot_ms; }, .worker_factory = factory},
        "test", sink, runtimeRenewBudget(), [] { return false; });
    CasMountRuntime & runtime = *runtime_holder;
    runtime.installRenewer(uuid, 1, [&] { return wall_ms; });
    const uint64_t anchor = runtime.startRenewer();
    runtime.armMountFence(uuid, 1, anchor + 1000);
    runtime.startBackgroundWorkers(std::chrono::hours(1));
    runtime.stopBackgroundWorkers();
    EXPECT_EQ(worker_exits.load(), 1u);
    runtime.finishTeardown(true);
    EXPECT_EQ(decodeMountLease(readObj(*backend, layout.mountKey("test"))->bytes).min_active_build_sequence,
              std::numeric_limits<uint64_t>::max());
}

namespace
{

/// Above the 4 MiB a thread batches before it reaches the tracker, below the 16 MiB at which an
/// allocation over an ignored limit also sends a trace.
constexpr Int64 kOverLimitAllocationBytes = 8 * 1024 * 1024;

struct OverLimitAllocation
{
    /// What the tracker threw; empty when it did not throw.
    String failure;
    /// How much the calling thread's tracker grew by the allocation.
    Int64 counted = 0;
};

/// One accounted allocation made while the global tracker is over its hard limit. The limit is restored
/// before this returns, whatever the allocation did. The growth is read from the calling thread's own
/// tracker, so an allocation or free on another thread cannot disturb it.
OverLimitAllocation allocateOverTheGlobalLimit()
{
    DB::CurrentThread::flushUntrackedMemory();
    const Int64 saved_limit = total_memory_tracker.getHardLimit();
    SCOPE_EXIT({ total_memory_tracker.setHardLimit(saved_limit); });
    total_memory_tracker.setHardLimit(1);

    OverLimitAllocation result;
    MemoryTracker * thread_tracker = DB::CurrentThread::getMemoryTracker();
    if (!thread_tracker)
    {
        result.failure = "the calling thread has no memory tracker";
        return result;
    }
    const Int64 before = thread_tracker->get();
    try
    {
        std::ignore = CurrentMemoryTracker::alloc(kOverLimitAllocationBytes);
    }
    catch (...)
    {
        result.failure = DB::getCurrentExceptionMessage(/*with_stacktrace=*/false);
        return result;
    }
    result.counted = thread_tracker->get() - before;
    std::ignore = CurrentMemoryTracker::free(kOverLimitAllocationBytes);
    return result;
}

}

/// With the global tracker over its limit, an allocation on the lease thread before the request and
/// another while the result is consumed do not throw and are still counted; a memory-limit exception
/// raised inside the request is retried, does not trip the fence, and the thread goes on renewing.
TEST(CASMountRuntime, MemoryLimitDoesNotEndTheLeaseThread)
{
    auto backend = std::make_shared<RuntimeRenewBackend>();
    const Layout layout("runtime-memory-limit");
    uint64_t wall_ms = 1000;
    uint64_t boot_ms = 100;
    const UInt128 uuid{1};
    ASSERT_EQ(claimMount(*DB::Cas::tests::OperationForTest(backend), layout, "test", uuid, 1, wall_ms, 1000).kind, MountClaimResult::Claimed);
    const Int64 hard_limit_before = total_memory_tracker.getHardLimit();

    std::atomic<uint32_t> admissions{0};
    OverLimitAllocation before_request;
    DB::Cas::tests::ManualBarrier second_admission;

    std::optional<OverLimitAllocation> while_consumed;
    String outcome;
    String attempts_sent;
    String classification;
    DB::Cas::tests::ManualBarrier reported;
    CasEventSink sink = [&](CasEvent event)
    {
        if (event.type != CasEventType::WatermarkRenew || while_consumed)
            return;
        while_consumed = allocateOverTheGlobalLimit();
        outcome = event.outcome;
        attempts_sent = event.detail["attempts_sent"];
        classification = event.detail["classification"];
        reported.arriveAndWait();
    };

    RuntimeUnderTest runtime_holder(
        backend, layout,
        MountConfig{
            .mount_lease_ttl_ms = std::chrono::milliseconds(1000),
            .background_watermark = true,
            .boot_ms_fn = [&] { return boot_ms; },
            .renewal_admitted_hook_for_test = [&]
            {
                const uint32_t admission = ++admissions;
                if (admission == 1)
                    before_request = allocateOverTheGlobalLimit();
                else if (admission == 2)
                    second_admission.arriveAndWait();
            }},
        "test", sink, runtimeRenewBudget(), [] { return false; });
    /// Declared after the runtime so it runs first: a failed expectation must not leave the lease thread
    /// parked on a barrier while the runtime's destructor joins it.
    SCOPE_EXIT({
        reported.release();
        second_admission.release();
    });
    CasMountRuntime & runtime = *runtime_holder;
    runtime.installRenewer(uuid, 1, [&] { return wall_ms; });
    const uint64_t anchor = runtime.startRenewer();
    runtime.armMountFence(uuid, 1, anchor + 1000);
    const uint64_t leases_lost_before = ProfileEvents::global_counters[ProfileEvents::CASMountLeaseLost].load();

    backend->fault = RuntimeRenewBackend::Fault::ThrowMemoryLimitExceeded;
    runtime.startBackgroundWorkers(std::chrono::milliseconds(0));

    reported.waitUntilArrived();
    const String reported_outcome = outcome;
    reported.release();
    ASSERT_EQ(reported_outcome, "recovered") << "a memory-limit exception inside the request must be retried";
    EXPECT_EQ(attempts_sent, "2");
    EXPECT_EQ(classification, "committed_after_retry");

    /// The worker is admitted for its next renewal: the thread outlived the injected failure.
    second_admission.waitUntilArrived();
    EXPECT_TRUE(runtime.mayMutate());
    EXPECT_EQ(runtime.lifecycle(), PoolLifecycle::Live);
    EXPECT_EQ(ProfileEvents::global_counters[ProfileEvents::CASMountLeaseLost].load(), leases_lost_before);

    EXPECT_EQ(before_request.failure, "") << "the allocation before the request threw";
    EXPECT_GE(before_request.counted, kOverLimitAllocationBytes);
    ASSERT_TRUE(while_consumed.has_value());
    EXPECT_EQ(while_consumed->failure, "") << "the allocation while the result was consumed threw";
    EXPECT_GE(while_consumed->counted, kOverLimitAllocationBytes);
    EXPECT_EQ(total_memory_tracker.getHardLimit(), hard_limit_before);

    second_admission.release();
    runtime.stopBackgroundWorkers();
    runtime.finishTeardown(false);
}

TEST(CASPoolRemount, NaturalTerminalTransitionMakesTheLeaseThreadExit)
{
    for (PoolLifecycle terminal : {PoolLifecycle::IdentityLost, PoolLifecycle::VanishedReplaced})
    {
        auto backend = std::make_shared<RuntimeRenewBackend>();
        const Layout layout(terminal == PoolLifecycle::IdentityLost
            ? "runtime-natural-identity-lost"
            : "runtime-natural-vanished");
        uint64_t wall_ms = 1000;
        uint64_t boot_ms = 100;
        const UInt128 uuid{1};
        ASSERT_EQ(claimMount(*DB::Cas::tests::OperationForTest(backend), layout, "test", uuid, 1, wall_ms, 1000).kind, MountClaimResult::Claimed);
        WorkerExitLatch exits;
        DB::Cas::tests::ManualBarrier transitioned;
        RuntimeWorkerFactory factory = [&](std::function<void()> worker_body)
        {
            return ThreadFromGlobalPool([&, body = std::move(worker_body)]
            {
                body();
                exits.recordExit();
            });
        };
        CasMountRuntime * runtime_ptr = nullptr;
        CasEventSink sink;
        RuntimeUnderTest runtime_holder(
            backend, layout,
            MountConfig{
                .mount_lease_ttl_ms = std::chrono::milliseconds(1000),
                .background_watermark = true,
                .boot_ms_fn = [&] { return boot_ms; },
                .worker_factory = factory},
            "test", sink, runtimeRenewBudget(), [&]
            {
                if (terminal == PoolLifecycle::IdentityLost)
                    runtime_ptr->enterIdentityLost();
                else
                    runtime_ptr->enterVanished(PoolLifecycle::VanishedReplaced, "injected natural replacement");
                transitioned.arriveAndWait();
                return false;
            });
        CasMountRuntime & runtime = *runtime_holder;
        runtime_ptr = &runtime;
        runtime.installRenewer(uuid, 1, [&] { return wall_ms; });
        const uint64_t anchor = runtime.startRenewer();
        runtime.armMountFence(uuid, 1, anchor + 1000);
        runtime.startBackgroundWorkers(std::chrono::hours(1));
        runtime.tripMountLost();
        runtime.scheduleRemount();
        transitioned.waitUntilArrived();
        transitioned.release();
        const bool exited_without_stop = exits.waitForAtLeast(1);
        runtime.stopBackgroundWorkers();
        EXPECT_TRUE(exited_without_stop);
        EXPECT_EQ(exits.count(), 1u);
        runtime.finishTeardown(false);
    }
}

/// A terminal publication that races a wait of the lease loop is serialized by `driver_mutex`: it
/// cannot land between the wait predicate's sample and the wait, so the thread exits without a stop.
/// Two waits: the cadence wait and the reclaim backoff. The backoff case checks only the exit: a missed
/// edge there costs one backoff, at most 1 s here, so it does not catch an unserialized publication.
TEST(CASPoolRemount, ALeaseWaitCannotMissNaturalTerminalPublication)
{
    enum class Wait : uint8_t { Cadence, ReclaimBackoff };
    for (Wait wait : {Wait::Cadence, Wait::ReclaimBackoff})
    {
        for (PoolLifecycle terminal : {PoolLifecycle::IdentityLost, PoolLifecycle::VanishedReplaced})
        {
            auto backend = std::make_shared<RuntimeRenewBackend>();
            const Layout layout(fmt::format(
                "runtime-wait-terminal-{}-{}",
                wait == Wait::Cadence ? "cadence" : "backoff",
                terminal == PoolLifecycle::IdentityLost ? "identity-lost" : "vanished"));
            uint64_t wall_ms = 1000;
            uint64_t boot_ms = 100;
            const UInt128 uuid{1};
            ASSERT_EQ(claimMount(*DB::Cas::tests::OperationForTest(backend), layout, "test", uuid, 1, wall_ms, 1000).kind,
                      MountClaimResult::Claimed);
            WorkerExitLatch exits;
            std::once_flag pause_once;
            /// Bounded: a regression fails after the barrier's timeout instead of hanging the gate.
            DB::Cas::tests::ManualBarrier waiter;
            std::atomic<bool> waiter_timed_out{false};
            std::atomic<bool> waiter_holds_driver_mutex{false};
            std::atomic<bool> publication_entered_while_held{false};
            const auto release_waiter = [&] { waiter.release(); };
            RuntimeWorkerFactory factory = [&](std::function<void()> worker_body)
            {
                return ThreadFromGlobalPool([&, body = std::move(worker_body)]
                {
                    body();
                    exits.recordExit();
                });
            };
            CasEventSink sink;
            RuntimeUnderTest runtime_holder(
                backend, layout,
                MountConfig{
                    .mount_lease_ttl_ms = std::chrono::milliseconds(1000),
                    .background_watermark = true,
                    .boot_ms_fn = [&] { return boot_ms; },
                    .worker_factory = factory,
                    .lease_wait_predicate_false_hook_for_test = [&]
                    {
                        std::call_once(pause_once, [&]
                        {
                            waiter_holds_driver_mutex.store(true, std::memory_order_release);
                            try
                            {
                                waiter.arriveAndWait();
                            }
                            catch (...)
                            {
                                waiter_timed_out = true;
                            }
                            waiter_holds_driver_mutex.store(false, std::memory_order_release);
                        });
                    },
                    .terminal_publication_driver_lock_contended_hook_for_test = [&]
                    {
                        release_waiter();
                    },
                    .terminal_publication_driver_lock_acquired_hook_for_test = [&]
                    {
                        if (waiter_holds_driver_mutex.load(std::memory_order_acquire))
                            publication_entered_while_held.store(true, std::memory_order_release);
                        release_waiter();
                    }},
                "test", sink, runtimeRenewBudget(), [] { return false; });
            /// Declared after the runtime so it runs first: a failed expectation must not leave the
            /// lease thread inside the hook while the runtime's destructor joins it.
            SCOPE_EXIT({ release_waiter(); });
            CasMountRuntime & runtime = *runtime_holder;
            runtime.installRenewer(uuid, 1, [&] { return wall_ms; });
            const uint64_t anchor = runtime.startRenewer();
            runtime.armMountFence(uuid, 1, anchor + 1000);
            if (wait == Wait::ReclaimBackoff)
            {
                /// A request before the start makes the first pass a reclaim, which fails and backs off.
                runtime.tripMountLost();
                runtime.scheduleRemount();
            }
            runtime.startBackgroundWorkers(std::chrono::hours(1));

            waiter.waitUntilArrived();
            if (terminal == PoolLifecycle::IdentityLost)
            {
                runtime.tripMountLost();
                runtime.enterIdentityLost();
            }
            else
            {
                runtime.enterVanished(PoolLifecycle::VanishedReplaced, "injected replacement during a lease wait");
            }
            /// A publication that took no `driver_mutex` released nobody; release the waiter here so it
            /// waits, misses the edge, and the expectation below fails instead of hanging.
            release_waiter();
            const bool exited_without_stop = exits.waitForAtLeast(1);
            runtime.stopBackgroundWorkers();
            EXPECT_FALSE(waiter_timed_out.load());
            EXPECT_FALSE(publication_entered_while_held.load())
                << "the publication must not take driver_mutex while the waiter holds it";
            EXPECT_TRUE(exited_without_stop);
            EXPECT_EQ(exits.count(), 1u);
            runtime.finishTeardown(false);
        }
    }
}

TEST(CASPoolRemount, VanishedReasonPreparationFailureLeavesTerminalTransitionRetryable)
{
    auto backend = std::make_shared<RuntimeRenewBackend>();
    const Layout layout("runtime-vanished-reason-preparation");
    uint64_t wall_ms = 1000;
    uint64_t boot_ms = 100;
    const UInt128 uuid{1};
    ASSERT_EQ(claimMount(*DB::Cas::tests::OperationForTest(backend), layout, "test", uuid, 1, wall_ms, 1000).kind, MountClaimResult::Claimed);
    WorkerExitLatch exits;
    RuntimeWorkerFactory factory = [&](std::function<void()> worker_body)
    {
        return ThreadFromGlobalPool([&, body = std::move(worker_body)]
        {
            body();
            exits.recordExit();
        });
    };
    std::atomic<uint64_t> preparation_calls{0};
    CasEventSink sink;
    RuntimeUnderTest runtime_holder(
        backend, layout,
        MountConfig{
            .mount_lease_ttl_ms = std::chrono::milliseconds(1000),
            .background_watermark = true,
            .boot_ms_fn = [&] { return boot_ms; },
            .worker_factory = factory,
            .vanished_reason_prepare_hook_for_test = [&]
            {
                if (preparation_calls.fetch_add(1) == 0)
                    throw DB::Exception(DB::ErrorCodes::NETWORK_ERROR, "injected vanished-reason preparation failure");
            }},
        "test", sink, runtimeRenewBudget(), [] { return false; });
    CasMountRuntime & runtime = *runtime_holder;
    runtime.installRenewer(uuid, 1, [&] { return wall_ms; });
    const uint64_t anchor = runtime.startRenewer();
    runtime.armMountFence(uuid, 1, anchor + 1000);
    runtime.startBackgroundWorkers(std::chrono::hours(1));
    runtime.tripMountLost();

    expectThrowsCode(DB::ErrorCodes::NETWORK_ERROR, [&]
    {
        runtime.enterVanished(PoolLifecycle::VanishedReplaced, "must-not-publish");
    });
    EXPECT_FALSE(runtime.vanishedIntentPublished());
    EXPECT_EQ(runtime.lifecycle(), PoolLifecycle::TransientNotLive);
    EXPECT_TRUE(runtime.vanishedReason().empty());

    runtime.enterVanished(PoolLifecycle::VanishedReplaced, "retry-completed");
    EXPECT_EQ(runtime.lifecycle(), PoolLifecycle::VanishedReplaced);
    EXPECT_EQ(runtime.vanishedReason(), "retry-completed");
    runtime.enterVanished(PoolLifecycle::VanishedForgotten, "must-remain-ignored");
    EXPECT_EQ(runtime.lifecycle(), PoolLifecycle::VanishedReplaced);
    EXPECT_EQ(runtime.vanishedReason(), "retry-completed");
    const bool exited_without_stop = exits.waitForAtLeast(1);
    runtime.stopBackgroundWorkers();
    EXPECT_TRUE(exited_without_stop);
    EXPECT_EQ(exits.count(), 1u);
    EXPECT_EQ(preparation_calls.load(), 2u);
    runtime.finishTeardown(false);
}

TEST(CASPoolRemount, WorkerConstructionRollbackFailsOpenClosed)
{
    auto backend = std::make_shared<RuntimeRenewBackend>();
    const Layout layout("runtime-worker-failure");
    uint64_t wall_ms = 1000;
    uint64_t boot_ms = 100;
    RuntimeWorkerFactory factory = [](std::function<void()>) -> ThreadFromGlobalPool
    {
        throw DB::Exception(DB::ErrorCodes::NETWORK_ERROR, "injected runtime worker construction failure");
    };
    const UInt128 uuid{1};
    ASSERT_EQ(claimMount(*DB::Cas::tests::OperationForTest(backend), layout, "test", uuid, 1, wall_ms, 1000).kind, MountClaimResult::Claimed);
    CasEventSink sink;
    RuntimeUnderTest runtime_holder(
        backend, layout,
        MountConfig{.mount_lease_ttl_ms = std::chrono::milliseconds(1000), .background_watermark = true,
                    .boot_ms_fn = [&] { return boot_ms; }, .worker_factory = factory},
        "test", sink, runtimeRenewBudget(), [] { return false; });
    CasMountRuntime & runtime = *runtime_holder;
    runtime.installRenewer(uuid, 1, [&] { return wall_ms; });
    const uint64_t anchor = runtime.startRenewer();
    runtime.armMountFence(uuid, 1, anchor + 1000);
    EXPECT_THROW(runtime.startBackgroundWorkers(std::chrono::milliseconds(10)), DB::Exception);
    EXPECT_FALSE(runtime.mayMutate());
    EXPECT_FALSE(runtime.workersRunningForTest());
    runtime.finishTeardown(false);
}

TEST(CASPoolRemount, ExternalLossDuringRenewalUsesOneRecoveryGeneration)
{
    auto backend = std::make_shared<RuntimeRenewBackend>();
    const Layout layout("runtime-external-loss");
    uint64_t wall_ms = 1000;
    uint64_t boot_ms = 100'000;
    const UInt128 uuid{1};
    ASSERT_EQ(claimMount(*DB::Cas::tests::OperationForTest(backend), layout, "test", uuid, 1, wall_ms, 1000).kind, MountClaimResult::Claimed);
    DB::Cas::tests::ManualBarrier renewal_barrier;
    DB::Cas::tests::ManualBarrier remount_barrier;
    std::atomic<uint64_t> remount_calls{0};
    std::atomic<uint64_t> fresh_epochs{0};
    CasEventSink sink;
    /// The remount callback reaches the runtime it is installed on, so it goes through a pointer the
    /// line after construction fills in -- the callback runs only once the workers are started.
    CasMountRuntime * runtime_ptr = nullptr;
    RuntimeUnderTest runtime_holder(
        backend, layout,
        MountConfig{.mount_lease_ttl_ms = std::chrono::milliseconds(1000), .background_watermark = true,
                    .boot_ms_fn = [&] { return boot_ms; }},
        "test", sink, runtimeRenewBudget(), [&]
        {
            ++remount_calls;
            runtime_ptr->beginReclaim();
            ++fresh_epochs;
            fenceOutMount(*backend, layout.mountKey("test"));
            const MountClaimResult fresh = claimMount(*DB::Cas::tests::OperationForTest(backend), layout, "test", uuid, 2, wall_ms, 1000);
            EXPECT_EQ(fresh.kind, MountClaimResult::Claimed);
            if (fresh.kind != MountClaimResult::Claimed)
                return false;
            runtime_ptr->installRenewer(uuid, 2, [&] { return wall_ms; });
            const uint64_t fresh_anchor = runtime_ptr->startRenewer();
            runtime_ptr->setProcessEpoch(2, std::memory_order_release);
            runtime_ptr->setLiveWriterEpoch(2);
            EXPECT_TRUE(runtime_ptr->armIfAdmissible(fresh_anchor + 1000));
            remount_barrier.arriveAndWait();
            return true;
        });
    CasMountRuntime & runtime = *runtime_holder;
    runtime_ptr = &runtime;
    runtime.installRenewer(uuid, 1, [&] { return wall_ms; });
    const uint64_t anchor = runtime.startRenewer();
    runtime.armMountFence(uuid, 1, anchor + 1000);
    backend->barrier = &renewal_barrier;
    backend->fault = RuntimeRenewBackend::Fault::BlockThenDelegate;
    const auto lost_before = ProfileEvents::global_counters[ProfileEvents::CASMountLeaseLost].load();
    runtime.startBackgroundWorkers(std::chrono::milliseconds(0));
    renewal_barrier.waitUntilArrived();
    runtime.tripMountLost();
    runtime.scheduleRemount();
    renewal_barrier.release();
    remount_barrier.waitUntilArrived();
    EXPECT_EQ(remount_calls.load(), 1u);
    EXPECT_EQ(fresh_epochs.load(), 1u);
    EXPECT_EQ(runtime.remountRequestedGenerationForTest(), 1u);
    EXPECT_EQ(ProfileEvents::global_counters[ProfileEvents::CASMountLeaseLost].load(), lost_before + 1);
    remount_barrier.release();
    runtime.stopBackgroundWorkers();
    runtime.finishTeardown(false);
}

TEST(CASPoolRemount, ConcurrentRemountRequestIsProcessedAfterActiveGeneration)
{
    auto backend = std::make_shared<RuntimeRenewBackend>();
    const Layout layout("runtime-generations");
    uint64_t wall_ms = 1000;
    uint64_t boot_ms = 100;
    const UInt128 uuid{1};
    ASSERT_EQ(claimMount(*DB::Cas::tests::OperationForTest(backend), layout, "test", uuid, 1, wall_ms, 1000).kind, MountClaimResult::Claimed);
    DB::Cas::tests::ManualBarrier first;
    DB::Cas::tests::ManualBarrier second;
    std::atomic<uint64_t> calls{0};
    CasEventSink sink;
    RuntimeUnderTest runtime_holder(
        backend, layout,
        MountConfig{.mount_lease_ttl_ms = std::chrono::milliseconds(1000), .background_watermark = true,
                    .boot_ms_fn = [&] { return boot_ms; }},
        "test", sink, runtimeRenewBudget(), [&]
        {
            const uint64_t call = ++calls;
            (call == 1 ? first : second).arriveAndWait();
            return true;
        });
    CasMountRuntime & runtime = *runtime_holder;
    runtime.installRenewer(uuid, 1, [&] { return wall_ms; });
    const uint64_t anchor = runtime.startRenewer();
    runtime.armMountFence(uuid, 1, anchor + 1000);
    runtime.startBackgroundWorkers(std::chrono::hours(1));
    runtime.tripMountLost();
    runtime.scheduleRemount();
    first.waitUntilArrived();
    runtime.scheduleRemount();
    first.release();
    second.waitUntilArrived();
    EXPECT_EQ(calls.load(), 2u);
    EXPECT_EQ(runtime.remountRequestedGenerationForTest(), 2u);
    second.release();
    runtime.stopBackgroundWorkers();
    runtime.finishTeardown(false);
}

TEST(CASPoolRemount, ImmediatePostRemountRenewalFailureIsNotDropped)
{
    auto backend = std::make_shared<RuntimeRenewBackend>();
    const Layout layout("runtime-catchup");
    uint64_t wall_ms = 1000;
    uint64_t boot_ms = 100;
    const UInt128 uuid{1};
    ASSERT_EQ(claimMount(*DB::Cas::tests::OperationForTest(backend), layout, "test", uuid, 1, wall_ms, 10'000).kind, MountClaimResult::Claimed);
    DB::Cas::tests::ManualBarrier first;
    DB::Cas::tests::ManualBarrier second;
    std::atomic<uint64_t> calls{0};
    CasEventSink sink;
    /// The remount callback reaches the runtime it is installed on, so it goes through a pointer the
    /// line after construction fills in -- the callback runs only once the workers are started.
    CasMountRuntime * runtime_ptr = nullptr;
    RuntimeUnderTest runtime_holder(
        backend, layout,
        MountConfig{.mount_lease_ttl_ms = std::chrono::milliseconds(10'000), .background_watermark = true,
                    .boot_ms_fn = [&] { return boot_ms; }},
        "test", sink, runtimeRenewBudget(), [&]
        {
            const uint64_t call = ++calls;
            if (call == 1)
            {
                runtime_ptr->beginReclaim();
                fenceOutMount(*backend, layout.mountKey("test"));
                const MountClaimResult fresh = claimMount(*DB::Cas::tests::OperationForTest(backend), layout, "test", uuid, 2, wall_ms, 10'000);
                EXPECT_EQ(fresh.kind, MountClaimResult::Claimed);
                if (fresh.kind != MountClaimResult::Claimed)
                    return false;
                runtime_ptr->installRenewer(uuid, 2, [&] { return wall_ms; });
                const uint64_t fresh_anchor = runtime_ptr->startRenewer();
                EXPECT_TRUE(runtime_ptr->armIfAdmissible(fresh_anchor + 10'000));
                boot_ms = 2'000;
                /// A definitive answer for the fresh incarnation's first renewal on the lease thread; a transient
                /// fault would only be retried.
                fenceOutMount(*backend, layout.mountKey("test"));
                first.arriveAndWait();
                return true;
            }
            second.arriveAndWait();
            return false;
        });
    CasMountRuntime & runtime = *runtime_holder;
    runtime_ptr = &runtime;
    runtime.installRenewer(uuid, 1, [&] { return wall_ms; });
    const uint64_t anchor = runtime.startRenewer();
    runtime.armMountFence(uuid, 1, anchor + 10'000);
    runtime.startBackgroundWorkers(std::chrono::milliseconds(1000));
    runtime.tripMountLost();
    runtime.scheduleRemount();
    first.waitUntilArrived();
    first.release();
    second.waitUntilArrived();
    EXPECT_EQ(calls.load(), 2u);
    EXPECT_EQ(runtime.remountRequestedGenerationForTest(), 2u);
    second.release();
    runtime.stopBackgroundWorkers();
    runtime.finishTeardown(false);
}

TEST(CASPoolRemount, ThrowingEventSinkAfterCommitLeavesRuntimeLive)
{
    auto backend = std::make_shared<InMemoryBackend>();
    auto store = Pool::open(backend, PoolConfig{
        .pool_prefix = "throwing-remount-event", .server_root_id = "test", .background_watermark = true});
    /// Heap-owned, not a plain local declared after `store`: if `waitUntilArrived` below throws on its
    /// own internal timeout, unwinding would destroy a stack-local barrier before `store`'s destructor
    /// joins the lease thread, and that thread can still be inside `arriveAndWait` on the dangling
    /// reference. A `shared_ptr` capture keeps the barrier alive for as long as the thread needs it,
    /// independent of declaration order.
    auto committed = std::make_shared<DB::Cas::tests::ManualBarrier>();
    store->setEventSink([committed](const CasEvent & event)
    {
        if (event.type == CasEventType::MountRemount && event.outcome == "ok")
        {
            committed->arriveAndWait();
            throw DB::Exception(DB::ErrorCodes::NETWORK_ERROR, "injected remount event sink failure");
        }
    });
    fenceOutMount(*backend, store->layout().mountKey("test"));
    ASSERT_TRUE(store->scheduleRemountForTest());
    committed->waitUntilArrived();
    EXPECT_EQ(store->lifecycle(), PoolLifecycle::Live);
    EXPECT_TRUE(store->mayMutate());
    committed->release();
    EXPECT_NO_THROW(store.reset());
}

TEST(CASPoolShutdown, PreSendCancellationAllowsFarewellButAmbiguityDoesNot)
{
    const auto run = [](bool ambiguous)
    {
        auto backend = std::make_shared<RuntimeRenewBackend>();
        const Layout layout(ambiguous ? "shutdown-ambiguous" : "shutdown-presend");
        uint64_t wall_ms = 1000;
        uint64_t boot_ms = 100;
        const UInt128 uuid{1};
        const MountClaimResult claim = claimMount(*DB::Cas::tests::OperationForTest(backend), layout, "test", uuid, 1, wall_ms, 1000);
        EXPECT_EQ(claim.kind, MountClaimResult::Claimed);
        if (claim.kind != MountClaimResult::Claimed)
            return uint64_t{0};
        DB::Cas::tests::ManualBarrier barrier;
        CasEventSink sink;
        RuntimeUnderTest runtime_holder(
            backend, layout,
            MountConfig{.mount_lease_ttl_ms = std::chrono::milliseconds(1000), .background_watermark = true,
                        .boot_ms_fn = [&] { return boot_ms; }},
            "test", sink, runtimeRenewBudget(), [] { return false; });
        CasMountRuntime & runtime = *runtime_holder;
        runtime.installRenewer(uuid, 1, [&] { return wall_ms; });
        const uint64_t anchor = runtime.startRenewer();
        runtime.armMountFence(uuid, 1, anchor + 1000);
        if (ambiguous)
        {
            backend->barrier = &barrier;
            backend->fault = RuntimeRenewBackend::Fault::BlockThenThrow;
            runtime.startBackgroundWorkers(std::chrono::milliseconds(0));
            barrier.waitUntilArrived();
            auto stop = std::async(std::launch::async, [&] { runtime.stopBackgroundWorkers(); });
            barrier.release();
            stop.get();
        }
        else
        {
            runtime.startBackgroundWorkers(std::chrono::hours(1));
            runtime.stopBackgroundWorkers();
        }
        runtime.finishTeardown(true);
        return decodeMountLease(readObj(*backend, layout.mountKey("test"))->bytes).min_active_build_sequence;
    };

    EXPECT_EQ(run(false), std::numeric_limits<uint64_t>::max());
    EXPECT_NE(run(true), std::numeric_limits<uint64_t>::max());
}

/// The test seam rethrows a terminal renewal as the typed failure it ended with.
TEST(CASPool, DirectTerminalFailureRethrowsTypedException)
{
    std::atomic<bool> renewal_live{true};
    uint64_t wall_ms = 1000;
    uint64_t boot_ms = 100;
    auto backend = std::make_shared<RuntimeRenewBackend>();
    const Layout layout("typed-direct");
    const UInt128 uuid{1};
    ASSERT_EQ(claimMount(*DB::Cas::tests::OperationForTest(backend), layout, "test", uuid, 1, wall_ms, 1000).kind, MountClaimResult::Claimed);
    CasEventSink sink;
    RuntimeUnderTest runtime_holder(
        backend, layout,
        MountConfig{
            .mount_lease_ttl_ms = std::chrono::milliseconds(1000),
            .boot_ms_fn = [&] { return boot_ms; },
            .renewal_live_for_test = [&] { return renewal_live.load(std::memory_order_acquire); }},
        "test", sink, runtimeRenewBudget(), [] { return false; });
    CasMountRuntime & runtime = *runtime_holder;
    runtime.installRenewer(uuid, 1, [&] { return wall_ms; });
    const uint64_t anchor = runtime.startRenewer();
    runtime.armMountFence(uuid, 1, anchor + 1000);
    /// The renewal lands, then its liveness ends before the commit is confirmed.
    backend->after_commit = [&] { renewal_live.store(false, std::memory_order_release); };
    try
    {
        runtime.renewWatermarkOnce();
        ADD_FAILURE() << "terminal renewal did not propagate";
    }
    catch (const DB::Exception & e)
    {
        EXPECT_EQ(e.code(), DB::ErrorCodes::NETWORK_ERROR) << e.message();
    }
    runtime.finishTeardown(false);
}

TEST(CASPool, BackgroundCadenceMustFitLeaseBeforeWritablePublication)
{
    auto backend = std::make_shared<DB::Cas::tests::CountingBackend>();
    PoolConfig config{
        .pool_prefix = "invalid-renew-cadence",
        .server_root_id = "test",
        .background_watermark = true,
        .mount_lease_ttl_ms = std::chrono::milliseconds(100),
        .mount_renew_period = std::chrono::milliseconds(80),
        .cas_request_budget = runtimeRenewBudget(),
    };
    EXPECT_THROW((void)Pool::open(backend, config), DB::Exception);
    /// One assertion over every write shape: the counters now sit on the write primitive, which both
    /// the create- and the replace-shaped verbs reach.
    EXPECT_EQ(backend->writeTotal(), 0u);
}

TEST(CASPool, DecommissionCadenceValidationPrecedesAuthorityWrites)
{
    auto backend = std::make_shared<DB::Cas::tests::CountingBackend>();
    {
        auto victim = Pool::open(backend, PoolConfig{.pool_prefix = "invalid-decommission-cadence", .server_root_id = "victim"});
    }
    backend->resetCounts();
    PoolConfig config{
        .pool_prefix = "invalid-decommission-cadence",
        .server_root_id = "admin",
        .mount_lease_ttl_ms = std::chrono::milliseconds(100),
        .mount_renew_period = std::chrono::milliseconds(80),
        .cas_request_budget = runtimeRenewBudget(),
    };
    expectThrowsCode(DB::ErrorCodes::BAD_ARGUMENTS, [&]
    {
        (void)Pool::openForDecommission(backend, config, "victim");
    });
    /// One assertion over every write shape: the counters now sit on the write primitive, which both
    /// the create- and the replace-shaped verbs reach.
    EXPECT_EQ(backend->writeTotal(), 0u);
}

TEST(CASPool, DisabledBackgroundDoesNotReserveRenewalCadence)
{
    auto backend = std::make_shared<RuntimeRenewBackend>();
    /// Captured by value: `fake_boot` is never mutated in this test, and the Pool can outlive this
    /// stack frame (a background publish holds `shared_from_this()`), so a by-reference capture would
    /// dangle.
    const uint64_t fake_boot = 100;
    PoolConfig config{
        .pool_prefix = "disabled-renew-cadence",
        .server_root_id = "test",
        .background_watermark = false,
        .mount_lease_ttl_ms = std::chrono::milliseconds(100),
        .mount_renew_period = std::chrono::hours(24),
        .cas_request_budget = runtimeRenewBudget(),
        .boot_ms_fn = [] { return fake_boot; },
    };
    auto store = Pool::open(backend, config);
    const String key = store->layout().mountKey("test");
    EXPECT_EQ(backend->putOverwriteCount(key), 1u)
        << "an open with no lease thread whose claim admits a ref append writes the lease only once";
}

TEST(CASPool, DeterministicWorkerFailureFencesWithoutWaitingForCadence)
{
    auto backend = std::make_shared<RuntimeRenewBackend>();
    const Layout layout("worker-failure");
    uint64_t wall_ms = 1000;
    uint64_t boot_ms = 100;
    const UInt128 uuid{1};
    ASSERT_EQ(claimMount(*DB::Cas::tests::OperationForTest(backend), layout, "test", uuid, 1, wall_ms, 1000).kind, MountClaimResult::Claimed);
    DB::Cas::tests::ManualBarrier remount_entered;
    CasEventSink sink;
    RuntimeUnderTest runtime_holder(
        backend, layout,
        MountConfig{.mount_lease_ttl_ms = std::chrono::milliseconds(1000), .background_watermark = true,
                    .boot_ms_fn = [&] { return boot_ms; }},
        "test", sink, runtimeRenewBudget(), [&]
        {
            remount_entered.arriveAndWait();
            return false;
        });
    CasMountRuntime & runtime = *runtime_holder;
    runtime.installRenewer(uuid, 1, [&] { return wall_ms; });
    const uint64_t anchor = runtime.startRenewer();
    runtime.armMountFence(uuid, 1, anchor + 1000);
    /// A definitive answer ends the lease thread's renewal; a transient fault would only be retried.
    fenceOutMount(*backend, layout.mountKey("test"));
    runtime.startBackgroundWorkers(std::chrono::milliseconds(0));
    remount_entered.waitUntilArrived();
    EXPECT_FALSE(runtime.mayMutate());
    EXPECT_EQ(runtime.lifecycle(), PoolLifecycle::TransientNotLive);
    remount_entered.release();
    runtime.stopBackgroundWorkers();
    runtime.finishTeardown(false);
}

/// FORGET while the lease thread's renewal retries past its lease: the intent then the trip, in the order
/// `Pool::forgetDisk` uses, end the renewal inside the wait it is in, the lease thread exits, and no remount
/// generation is raised.
TEST(CASMountRuntime, ForgetEndsAnUnboundedRenewal)
{
    auto backend = std::make_shared<RuntimeRenewBackend>();
    const Layout layout("unbounded-forget");
    const UInt128 uuid{1};
    uint64_t wall_ms = 1000;
    std::atomic<uint64_t> boot_ms{100};
    std::atomic<uint64_t> past_the_lease{std::numeric_limits<uint64_t>::max()};
    std::atomic<bool> held{false};
    std::atomic<uint64_t> waits{0};
    DB::Cas::tests::ManualBarrier holding;
    WorkerExitLatch exits;
    RuntimeWorkerFactory factory = [&](std::function<void()> worker_body)
    {
        return ThreadFromGlobalPool([&, body = std::move(worker_body)]
        {
            body();
            exits.recordExit();
        });
    };
    ASSERT_EQ(claimMount(*DB::Cas::tests::OperationForTest(backend), layout, "test", uuid, 1, wall_ms, 1000).kind,
              MountClaimResult::Claimed);
    CasEventSink sink;
    RuntimeUnderTest runtime_holder(
        backend, layout,
        MountConfig{.mount_lease_ttl_ms = std::chrono::milliseconds(1000), .background_watermark = true,
                    .boot_ms_fn = [&] { return boot_ms.load(); }, .worker_factory = factory},
        "test", sink, runtimeRenewBudget(), [] { return false; });
    CasMountRuntime & runtime = *runtime_holder;
    runtime.installRenewer(uuid, 1, [&] { return wall_ms; });
    const uint64_t anchor = runtime.startRenewer();
    runtime.armMountFence(uuid, 1, anchor + 1000);
    past_the_lease = anchor + 5'000;
    runtime_holder.setRetrySleepForTest([&](uint64_t ms)
    {
        ++waits;
        boot_ms += ms;
        if (boot_ms.load() >= past_the_lease.load() && !held.exchange(true))
            holding.arriveAndWait();
    });
    backend->outage = [] { return true; };
    runtime.startBackgroundWorkers(std::chrono::milliseconds(0));

    holding.waitUntilArrived();
    const uint64_t writes_at_forget = backend->outage_writes.load();
    const uint64_t waits_at_forget = waits.load();
    const uint64_t generation_at_forget = runtime.remountRequestedGenerationForTest();
    runtime.publishVanishedIntent();
    runtime.tripMountLost();
    holding.release();

    const bool exited = exits.waitForAtLeast(1);
    EXPECT_TRUE(exited) << "the lease loop must exit on the published intent";
    EXPECT_EQ(backend->outage_writes.load(), writes_at_forget) << "no request starts after FORGET";
    EXPECT_EQ(waits.load(), waits_at_forget) << "no wait starts after FORGET";
    EXPECT_EQ(runtime.remountRequestedGenerationForTest(), generation_at_forget);
    EXPECT_FALSE(runtime.mayMutate());
    runtime.stopBackgroundWorkers();
    runtime.finishTeardown(false);
}

TEST(CASMountRuntime, StopWakesTheRetryWaitOfAnUnboundedRenewal)
{
    auto backend = std::make_shared<RuntimeRenewBackend>();
    const Layout layout("unbounded-stop-in-wait");
    const UInt128 uuid{1};
    uint64_t wall_ms = 1000;
    const uint64_t boot_ms = 100;
    std::promise<uint64_t> wait_entered;
    std::future<uint64_t> wait_requested = wait_entered.get_future();
    std::atomic<bool> first_wait{true};
    std::atomic<int64_t> first_wait_slept_ms{-1};
    /// The first wait is held far longer than any scheduling delay of the test thread, so only a stop can
    /// end it before the expectation below; a stop that failed to wake it fails after this long.
    constexpr uint64_t held_wait_ms = 30'000;
    ASSERT_EQ(claimMount(*DB::Cas::tests::OperationForTest(backend), layout, "test", uuid, 1, wall_ms, 1000).kind,
              MountClaimResult::Claimed);
    CasEventSink sink;
    RuntimeUnderTest runtime_holder(
        backend, layout,
        MountConfig{.mount_lease_ttl_ms = std::chrono::milliseconds(1000), .background_watermark = true,
                    .boot_ms_fn = [&] { return boot_ms; }},
        "test", sink, runtimeRenewBudget(), [] { return false; });
    CasMountRuntime & runtime = *runtime_holder;
    runtime.installRenewer(uuid, 1, [&] { return wall_ms; });
    const uint64_t anchor = runtime.startRenewer();
    runtime.armMountFence(uuid, 1, anchor + 1000);
    runtime_holder.setRetrySleepForTest([&](uint64_t ms)
    {
        const bool first = first_wait.exchange(false);
        if (first)
            wait_entered.set_value(ms);
        const auto started = std::chrono::steady_clock::now();
        runtime.sleepInterruptibly(first ? held_wait_ms : ms);
        if (first)
            first_wait_slept_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - started).count();
    });
    backend->outage = [] { return true; };
    const uint64_t lost_before = ProfileEvents::global_counters[ProfileEvents::CASMountLeaseLost].load();
    runtime.startBackgroundWorkers(std::chrono::milliseconds(0));

    ASSERT_EQ(wait_requested.wait_for(std::chrono::seconds(20)), std::future_status::ready);
    const uint64_t requested_ms = wait_requested.get();
    runtime.stopBackgroundWorkers();
    /// The renewal the stop ended had sent a request, so it ends terminal on a `Live` pool.
    EXPECT_FALSE(runtime.mayMutate()) << "the stop latches the fence";
    EXPECT_EQ(runtime.lifecycle(), PoolLifecycle::Live);
    EXPECT_EQ(ProfileEvents::global_counters[ProfileEvents::CASMountLeaseLost].load(), lost_before)
        << "the stop counts no lease loss";

    EXPECT_GE(requested_ms, kMountRenewRetrySpacingMs * 8 / 10);
    EXPECT_LE(requested_ms, kMountRenewRetrySpacingMs * 12 / 10);
    ASSERT_GE(first_wait_slept_ms.load(), 0);
    EXPECT_LT(static_cast<uint64_t>(first_wait_slept_ms.load()), held_wait_ms / 2)
        << "the stop woke the wait instead of letting it run out";
    EXPECT_EQ(backend->outage_writes.load(), 1u) << "nothing is sent after the stop";
    runtime.finishTeardown(false);
}

/// A success whose lease is already over is followed by the next renewal with no cadence wait.
TEST(CASMountRuntime, AStaleSuccessIsFollowedAtOnceByTheNextRenewal)
{
    const Layout layout("unbounded-stale-success");
    const UInt128 uuid{1};
    uint64_t wall_ms = 1000;
    std::atomic<uint64_t> boot_ms{100};
    std::atomic<uint64_t> outage_until{0};
    std::vector<uint64_t> commit_boot_ms;
    std::vector<bool> may_mutate_at_commit;
    DB::Cas::tests::ManualBarrier second_commit;
    CasMountRuntime * runtime_ptr = nullptr;
    /// Far above the longest legitimate outage here (one request per second for three lease lengths).
    constexpr uint64_t max_outage_requests = 500;
    std::atomic<bool> request_bound_hit{false};
    /// Declared after the locals its hooks capture.
    auto backend = std::make_shared<RuntimeRenewBackend>();
    ASSERT_EQ(claimMount(*DB::Cas::tests::OperationForTest(backend), layout, "test", uuid, 1, wall_ms, 1000).kind,
              MountClaimResult::Claimed);
    CasEventSink sink;
    RuntimeUnderTest runtime_holder(
        backend, layout,
        MountConfig{.mount_lease_ttl_ms = std::chrono::milliseconds(1000), .background_watermark = true,
                    .boot_ms_fn = [&] { return boot_ms.load(); },
                    /// The test clock moves only through the sleep seam, so a regression that zeroes the
                    /// pauses would retry forever; the request count ends the renewal instead.
                    .renewal_live_for_test = [&]
                    {
                        if (backend->outage_writes.load() < max_outage_requests)
                            return true;
                        request_bound_hit = true;
                        return false;
                    }},
        "test", sink, runtimeRenewBudget(), [] { return false; });
    CasMountRuntime & runtime = *runtime_holder;
    runtime_ptr = &runtime;
    runtime.installRenewer(uuid, 1, [&] { return wall_ms; });
    const uint64_t anchor = runtime.startRenewer();
    runtime.armMountFence(uuid, 1, anchor + 1000);
    runtime_holder.setRetrySleepForTest([&](uint64_t ms) { boot_ms += ms; });
    /// The first renewal starts one period after the anchor and fails for three lease lengths.
    boot_ms = anchor + 500;
    outage_until = anchor + 3'500;
    backend->outage = [&] { return boot_ms.load() < outage_until.load(); };
    backend->after_commit = [&]
    {
        commit_boot_ms.push_back(boot_ms.load());
        may_mutate_at_commit.push_back(runtime_ptr->mayMutate());
        if (commit_boot_ms.size() == 2)
            second_commit.arriveAndWait();
    };
    runtime.startBackgroundWorkers(std::chrono::milliseconds(500));

    bool arrived = true;
    try
    {
        second_commit.waitUntilArrived();
    }
    catch (const DB::Exception &)
    {
        arrived = false;
    }
    if (!arrived)
    {
        runtime.stopBackgroundWorkers();
        runtime.finishTeardown(false);
        FAIL() << "the second renewal never committed; request bound hit: " << request_bound_hit.load();
    }
    EXPECT_FALSE(request_bound_hit.load())
        << "the first renewal sent " << max_outage_requests << " requests without the clock reaching the end of the outage";
    ASSERT_EQ(commit_boot_ms.size(), 2u);
    EXPECT_GE(commit_boot_ms[0], anchor + 3'500) << "the first renewal outlived its own lease";
    EXPECT_EQ(commit_boot_ms[1], commit_boot_ms[0])
        << "no time passed: a cadence wait on this frozen clock would never have ended";
    EXPECT_FALSE(may_mutate_at_commit[1]) << "the stale success left the lease expired";
    second_commit.release();
    runtime.stopBackgroundWorkers();
    runtime.finishTeardown(false);
}

/// A remount request raised on an armed fence: the reclaim latches the fence before anything else and
/// counts one loss. An interference report raised while that reclaim runs: it arms nothing, the next
/// one starts with the fence latched, and no write is admitted between the two.
TEST(CASMountRuntime, NothingArmsWhileARemountRequestIsPending)
{
    auto backend = std::make_shared<RuntimeRenewBackend>();
    const Layout layout("runtime-nothing-arms-while-pending");
    uint64_t wall_ms = 1000;
    const uint64_t boot_ms = 100;
    const UInt128 uuid{1};
    ASSERT_EQ(claimMount(*DB::Cas::tests::OperationForTest(backend), layout, "test", uuid, 1, wall_ms, 1000).kind,
              MountClaimResult::Claimed);
    std::atomic<uint32_t> calls{0};
    bool may_mutate_before_first_latch = false;
    bool may_mutate_after_first_latch = true;
    PoolLifecycle lifecycle_after_first_latch = PoolLifecycle::Live;
    uint64_t lost_after_first_latch = 0;
    bool first_armed = true;
    bool may_mutate_after_first = true;
    bool may_mutate_at_second_entry = true;
    bool may_mutate_after_second_latch = true;
    PoolLifecycle lifecycle_after_second_latch = PoolLifecycle::Live;
    bool second_armed = false;
    bool may_mutate_after_second = false;
    DB::Cas::tests::ManualBarrier second_done;
    CasMountRuntime * runtime_ptr = nullptr;
    CasEventSink sink;
    RuntimeUnderTest runtime_holder(
        backend, layout,
        MountConfig{.mount_lease_ttl_ms = std::chrono::milliseconds(1000), .background_watermark = true,
                    .boot_ms_fn = [&] { return boot_ms; }},
        "test", sink, runtimeRenewBudget(), [&]
        {
            CasMountRuntime & reclaiming = *runtime_ptr;
            if (++calls == 1)
            {
                may_mutate_before_first_latch = reclaiming.mayMutate();
                reclaiming.beginReclaim();
                may_mutate_after_first_latch = reclaiming.mayMutate();
                lifecycle_after_first_latch = reclaiming.lifecycle();
                lost_after_first_latch = ProfileEvents::global_counters[ProfileEvents::CASMountLeaseLost].load();
                /// An interference report while this reclaim runs.
                reclaiming.tripAndRequestRemount();
                first_armed = reclaiming.armIfAdmissible(boot_ms + 1000);
                may_mutate_after_first = reclaiming.mayMutate();
                return true;
            }
            may_mutate_at_second_entry = reclaiming.mayMutate();
            reclaiming.beginReclaim();
            may_mutate_after_second_latch = reclaiming.mayMutate();
            lifecycle_after_second_latch = reclaiming.lifecycle();
            second_armed = reclaiming.armIfAdmissible(boot_ms + 1000);
            may_mutate_after_second = reclaiming.mayMutate();
            second_done.arriveAndWait();
            return true;
        });
    /// Declared after the runtime so it runs first: a failed expectation must not leave the reclaim
    /// parked on the barrier while the runtime's destructor joins the thread.
    SCOPE_EXIT({ second_done.release(); });
    CasMountRuntime & runtime = *runtime_holder;
    runtime_ptr = &runtime;
    runtime.installRenewer(uuid, 1, [&] { return wall_ms; });
    const uint64_t anchor = runtime.startRenewer();
    runtime.armMountFence(uuid, 1, anchor + 1000);
    const uint64_t lost_before = ProfileEvents::global_counters[ProfileEvents::CASMountLeaseLost].load();
    runtime.startBackgroundWorkers(std::chrono::hours(1));
    /// A request without a trip: the fence is still armed when the reclaim starts.
    runtime.scheduleRemount();

    second_done.waitUntilArrived();
    EXPECT_TRUE(may_mutate_before_first_latch) << "the request alone must leave the fence armed";
    EXPECT_FALSE(may_mutate_after_first_latch) << "a reclaim starts with the fence latched";
    EXPECT_EQ(lifecycle_after_first_latch, PoolLifecycle::TransientNotLive);
    EXPECT_EQ(lost_after_first_latch, lost_before + 1) << "the latch counts the one loss";
    EXPECT_FALSE(first_armed) << "a reclaim must not arm while a newer remount request is pending";
    EXPECT_FALSE(may_mutate_after_first);
    EXPECT_FALSE(may_mutate_at_second_entry) << "no write may be admitted between the two reclaims";
    EXPECT_FALSE(may_mutate_after_second_latch) << "a reclaim starts with the fence latched";
    EXPECT_EQ(lifecycle_after_second_latch, PoolLifecycle::TransientNotLive);
    EXPECT_TRUE(second_armed) << "the reclaim that served the last request arms";
    EXPECT_TRUE(may_mutate_after_second);
    EXPECT_EQ(calls.load(), 2u);
    second_done.release();
    runtime.stopBackgroundWorkers();
    runtime.finishTeardown(false);
}

/// An interference report that lands while a reclaim runs, before its arm or while the arm holds
/// `driver_mutex`: the arm does not override it. The fence ends latched with one request pending, the
/// report counts a loss only on a `Live` pool, and the next reclaim serves it.
TEST(CASMountRuntime, AnInterferenceReportDuringAReclaimIsServedByTheNextReclaim)
{
    enum class ReportAt : uint8_t { BeforeTheArm, InsideTheArm };
    const auto run = [](ReportAt report_at)
    {
        auto backend = std::make_shared<RuntimeRenewBackend>();
        const Layout layout(report_at == ReportAt::BeforeTheArm ? "report-before-arm" : "report-inside-arm");
        uint64_t wall_ms = 1000;
        const uint64_t boot_ms = 100;
        const UInt128 uuid{1};
        ASSERT_EQ(claimMount(*DB::Cas::tests::OperationForTest(backend), layout, "test", uuid, 1, wall_ms, 1000).kind,
                  MountClaimResult::Claimed);
        std::atomic<uint32_t> calls{0};
        bool first_armed = false;
        bool may_mutate_after_first = true;
        uint64_t generation_after_first = 0;
        bool may_mutate_at_second_entry = true;
        bool second_armed = false;
        std::future<void> report;
        std::atomic<bool> report_never_started{false};
        DB::Cas::tests::ManualBarrier second_done;
        CasMountRuntime * runtime_ptr = nullptr;
        CasEventSink sink;
        RuntimeUnderTest runtime_holder(
            backend, layout,
            MountConfig{.mount_lease_ttl_ms = std::chrono::milliseconds(1000), .background_watermark = true,
                        .boot_ms_fn = [&] { return boot_ms; }},
            "test", sink, runtimeRenewBudget(), [&]
            {
                CasMountRuntime & reclaiming = *runtime_ptr;
                if (++calls == 1)
                {
                    reclaiming.beginReclaim();
                    if (report_at == ReportAt::BeforeTheArm)
                    {
                        reclaiming.tripAndRequestRemount();
                    }
                    else
                    {
                        /// The hook runs inside the arm with `driver_mutex` held, so the report starts
                        /// there and can finish only after the arm's section.
                        reclaiming.setArmMountFenceInterpositionHookForTest([&]
                        {
                            const uint64_t requests_before = runtime_ptr->scheduleRemountCallCountForTest();
                            report = std::async(std::launch::async, [&] { runtime_ptr->tripAndRequestRemount(); });
                            /// One step: the count rises before the lock, so no trip has landed yet. Two
                            /// steps: the unlocked trip lands before the count rises, and the arm clears it.
                            /// The bound only turns a hang into a failure.
                            const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(20);
                            while (runtime_ptr->scheduleRemountCallCountForTest() == requests_before)
                            {
                                if (std::chrono::steady_clock::now() >= until)
                                {
                                    report_never_started = true;
                                    break;
                                }
                                std::this_thread::yield();
                            }
                        });
                    }
                    first_armed = reclaiming.armIfAdmissible(boot_ms + 1000);
                    reclaiming.setArmMountFenceInterpositionHookForTest({});
                    if (report.valid())
                        report.get();
                    may_mutate_after_first = reclaiming.mayMutate();
                    generation_after_first = reclaiming.remountRequestedGenerationForTest();
                    return true;
                }
                may_mutate_at_second_entry = reclaiming.mayMutate();
                reclaiming.beginReclaim();
                second_armed = reclaiming.armIfAdmissible(boot_ms + 1000);
                second_done.arriveAndWait();
                return true;
            });
        SCOPE_EXIT({ second_done.release(); });
        CasMountRuntime & runtime = *runtime_holder;
        runtime_ptr = &runtime;
        runtime.installRenewer(uuid, 1, [&] { return wall_ms; });
        const uint64_t anchor = runtime.startRenewer();
        runtime.armMountFence(uuid, 1, anchor + 1000);
        const uint64_t lost_before = ProfileEvents::global_counters[ProfileEvents::CASMountLeaseLost].load();
        runtime.startBackgroundWorkers(std::chrono::hours(1));
        runtime.scheduleRemount();

        second_done.waitUntilArrived();
        EXPECT_FALSE(report_never_started.load()) << "the report did not start within the bound";
        if (report_at == ReportAt::BeforeTheArm)
            EXPECT_FALSE(first_armed) << "a report raised during the reclaim must stop its arm";
        else
            EXPECT_TRUE(first_armed) << "the report waits for the arm's section";
        EXPECT_FALSE(may_mutate_after_first) << "the arm must not override the report's trip";
        EXPECT_EQ(generation_after_first, 2u) << "the report raises one generation";
        EXPECT_FALSE(may_mutate_at_second_entry);
        EXPECT_TRUE(second_armed) << "the next reclaim serves the report";
        EXPECT_EQ(calls.load(), 2u);
        EXPECT_EQ(runtime.remountRequestedGenerationForTest(), 2u);
        /// The first latch counts one loss; the report counts one more only when the arm made the pool `Live`.
        EXPECT_EQ(ProfileEvents::global_counters[ProfileEvents::CASMountLeaseLost].load(),
                  lost_before + (report_at == ReportAt::BeforeTheArm ? 1 : 2));
        second_done.release();
        runtime.stopBackgroundWorkers();
        runtime.finishTeardown(false);
    };
    run(ReportAt::BeforeTheArm);
    run(ReportAt::InsideTheArm);
}

/// One interference report produces one remount generation, one epoch change and one lease-loss
/// count, also when the reclaim fails twice before it succeeds. Two failures cost one and two seconds
/// of the loop's real backoff.
TEST(CASMountRuntime, AReclaimAcknowledgesOnlyTheGenerationItServed)
{
    auto backend = std::make_shared<RuntimeRenewBackend>();
    const Layout layout("runtime-reclaim-acknowledges");
    uint64_t wall_ms = 1000;
    const uint64_t boot_ms = 100;
    const UInt128 uuid{1};
    ASSERT_EQ(claimMount(*DB::Cas::tests::OperationForTest(backend), layout, "test", uuid, 1, wall_ms, 1000).kind,
              MountClaimResult::Claimed);
    std::atomic<uint32_t> calls{0};
    std::atomic<uint32_t> fresh_epochs{0};
    DB::Cas::tests::ManualBarrier reclaimed;
    CasMountRuntime * runtime_ptr = nullptr;
    CasEventSink sink;
    RuntimeUnderTest runtime_holder(
        backend, layout,
        MountConfig{.mount_lease_ttl_ms = std::chrono::milliseconds(1000), .background_watermark = true,
                    .boot_ms_fn = [&] { return boot_ms; }},
        "test", sink, runtimeRenewBudget(), [&]
        {
            CasMountRuntime & reclaiming = *runtime_ptr;
            reclaiming.beginReclaim();
            if (++calls < 3)
                return false;
            fenceOutMount(*backend, layout.mountKey("test"));
            const MountClaimResult fresh
                = claimMount(*DB::Cas::tests::OperationForTest(backend), layout, "test", uuid, 2, wall_ms, 1000);
            EXPECT_EQ(fresh.kind, MountClaimResult::Claimed);
            if (fresh.kind != MountClaimResult::Claimed)
                return false;
            ++fresh_epochs;
            reclaiming.installRenewer(uuid, 2, [&] { return wall_ms; });
            const uint64_t fresh_anchor = reclaiming.startRenewer();
            reclaiming.setLiveWriterEpoch(2);
            EXPECT_TRUE(reclaiming.armIfAdmissible(fresh_anchor + 1000));
            reclaimed.arriveAndWait();
            return true;
        });
    SCOPE_EXIT({ reclaimed.release(); });
    CasMountRuntime & runtime = *runtime_holder;
    runtime_ptr = &runtime;
    runtime.installRenewer(uuid, 1, [&] { return wall_ms; });
    const uint64_t anchor = runtime.startRenewer();
    runtime.armMountFence(uuid, 1, anchor + 1000);
    const uint64_t lost_before = ProfileEvents::global_counters[ProfileEvents::CASMountLeaseLost].load();
    runtime.startBackgroundWorkers(std::chrono::hours(1));
    /// One interference report.
    runtime.tripMountLost();
    runtime.scheduleRemount();

    reclaimed.waitUntilArrived();
    EXPECT_EQ(calls.load(), 3u);
    EXPECT_EQ(fresh_epochs.load(), 1u);
    EXPECT_EQ(runtime.remountRequestedGenerationForTest(), 1u) << "a failed reclaim raises no generation";
    EXPECT_EQ(ProfileEvents::global_counters[ProfileEvents::CASMountLeaseLost].load(), lost_before + 1)
        << "the latch of each attempt counts no further loss";
    EXPECT_TRUE(runtime.mayMutate());
    reclaimed.release();
    runtime.stopBackgroundWorkers();
    EXPECT_EQ(calls.load(), 3u) << "the served generation is not reclaimed again";
    runtime.finishTeardown(false);
}

/// The first two steps of `Pool::forgetDisk` land while a reclaim runs: the reclaim that finishes
/// after them arms nothing, so the fence is latched before FORGET's second trip, and the thread exits.
TEST(CASMountRuntime, AReclaimFinishedAfterTheForgetIntentArmsNothing)
{
    auto backend = std::make_shared<RuntimeRenewBackend>();
    const Layout layout("runtime-reclaim-after-intent");
    uint64_t wall_ms = 1000;
    const uint64_t boot_ms = 100;
    const UInt128 uuid{1};
    ASSERT_EQ(claimMount(*DB::Cas::tests::OperationForTest(backend), layout, "test", uuid, 1, wall_ms, 1000).kind,
              MountClaimResult::Claimed);
    WorkerExitLatch exits;
    uint64_t threads = 0;
    RuntimeWorkerFactory factory = [&](std::function<void()> worker_body)
    {
        ++threads;
        return ThreadFromGlobalPool([&, body = std::move(worker_body)]
        {
            body();
            exits.recordExit();
        });
    };
    DB::Cas::tests::ManualBarrier latched;
    bool armed = true;
    bool may_mutate_after_reclaim = true;
    CasMountRuntime * runtime_ptr = nullptr;
    CasEventSink sink;
    RuntimeUnderTest runtime_holder(
        backend, layout,
        MountConfig{.mount_lease_ttl_ms = std::chrono::milliseconds(1000), .background_watermark = true,
                    .boot_ms_fn = [&] { return boot_ms; }, .worker_factory = factory},
        "test", sink, runtimeRenewBudget(), [&]
        {
            CasMountRuntime & reclaiming = *runtime_ptr;
            reclaiming.beginReclaim();
            latched.arriveAndWait();
            armed = reclaiming.armIfAdmissible(boot_ms + 1000);
            may_mutate_after_reclaim = reclaiming.mayMutate();
            return true;
        });
    SCOPE_EXIT({ latched.release(); });
    CasMountRuntime & runtime = *runtime_holder;
    runtime_ptr = &runtime;
    runtime.installRenewer(uuid, 1, [&] { return wall_ms; });
    const uint64_t anchor = runtime.startRenewer();
    runtime.armMountFence(uuid, 1, anchor + 1000);
    runtime.startBackgroundWorkers(std::chrono::hours(1));
    runtime.tripMountLost();
    runtime.scheduleRemount();

    latched.waitUntilArrived();
    runtime.publishVanishedIntent();
    runtime.tripMountLost();
    latched.release();

    const bool exited = exits.waitForAtLeast(threads);
    EXPECT_TRUE(exited) << "the published intent must end the loop";
    EXPECT_FALSE(armed) << "a reclaim that finished after the intent must not arm the fence";
    EXPECT_FALSE(may_mutate_after_reclaim);
    EXPECT_FALSE(runtime.mayMutate()) << "the fence is latched before FORGET's second trip";
    runtime.stopBackgroundWorkers();
    runtime.finishTeardown(false);
}

/// A stop requested during a reclaim: the join returns once the attempt returns, the reclaim arms
/// nothing, and no farewell is written for the slot this runtime did not claim back.
TEST(CASMountRuntime, StopDuringAReclaimJoinsTheThread)
{
    auto backend = std::make_shared<RuntimeRenewBackend>();
    const Layout layout("runtime-stop-during-reclaim");
    uint64_t wall_ms = 1000;
    const uint64_t boot_ms = 100;
    const UInt128 uuid{1};
    ASSERT_EQ(claimMount(*DB::Cas::tests::OperationForTest(backend), layout, "test", uuid, 1, wall_ms, 1000).kind,
              MountClaimResult::Claimed);
    WorkerExitLatch exits;
    uint64_t threads = 0;
    RuntimeWorkerFactory factory = [&](std::function<void()> worker_body)
    {
        ++threads;
        return ThreadFromGlobalPool([&, body = std::move(worker_body)]
        {
            body();
            exits.recordExit();
        });
    };
    DB::Cas::tests::ManualBarrier reclaim_entered;
    std::atomic<bool> stop_requested_by_test{false};
    bool stop_seen_by_reclaim = false;
    bool armed = true;
    std::atomic<bool> reclaim_returned{false};
    uint64_t lost_at_latch = 0;
    CasMountRuntime * runtime_ptr = nullptr;
    CasEventSink sink;
    RuntimeUnderTest runtime_holder(
        backend, layout,
        MountConfig{.mount_lease_ttl_ms = std::chrono::milliseconds(1000), .background_watermark = true,
                    .boot_ms_fn = [&] { return boot_ms; }, .worker_factory = factory},
        "test", sink, runtimeRenewBudget(), [&]
        {
            CasMountRuntime & reclaiming = *runtime_ptr;
            reclaiming.beginReclaim();
            lost_at_latch = ProfileEvents::global_counters[ProfileEvents::CASMountLeaseLost].load();
            reclaim_entered.arriveAndWait();
            /// Returns on the stop; the timeout only bounds a failing run.
            reclaiming.sleepInterruptibly(20'000);
            stop_seen_by_reclaim = stop_requested_by_test.load();
            armed = reclaiming.armIfAdmissible(boot_ms + 1000);
            reclaim_returned = true;
            return true;
        });
    SCOPE_EXIT({ reclaim_entered.release(); });
    CasMountRuntime & runtime = *runtime_holder;
    runtime_ptr = &runtime;
    runtime.installRenewer(uuid, 1, [&] { return wall_ms; });
    const uint64_t anchor = runtime.startRenewer();
    runtime.armMountFence(uuid, 1, anchor + 1000);
    const String key = layout.mountKey("test");
    /// A definitive answer ends the first renewal and requests the reclaim; nobody claims the slot back.
    fenceOutMount(*backend, key);
    const uint64_t lost_before = ProfileEvents::global_counters[ProfileEvents::CASMountLeaseLost].load();
    runtime.startBackgroundWorkers(std::chrono::milliseconds(0));

    reclaim_entered.waitUntilArrived();
    stop_requested_by_test = true;
    auto stop = std::async(std::launch::async, [&] { runtime.stopBackgroundWorkers(); });
    reclaim_entered.release();
    stop.get();

    EXPECT_TRUE(reclaim_returned.load()) << "the join waits for the reclaim attempt to return";
    EXPECT_EQ(exits.count(), threads);
    EXPECT_TRUE(stop_seen_by_reclaim) << "the reclaim must finish after the stop was requested";
    EXPECT_FALSE(armed) << "a reclaim that finishes after a stop must not arm the fence";
    EXPECT_FALSE(runtime.mayMutate());
    EXPECT_EQ(lost_at_latch, lost_before + 1) << "the fenced-out renewal counts the one loss";
    const auto slot_before_teardown = readObj(*backend, key);
    ASSERT_TRUE(slot_before_teardown.has_value());
    runtime.finishTeardown(true);
    const auto slot_after_teardown = readObj(*backend, key);
    ASSERT_TRUE(slot_after_teardown.has_value());
    EXPECT_EQ(slot_after_teardown->bytes, slot_before_teardown->bytes)
        << "no farewell for a slot this runtime did not claim back";
}

/// An interference report while the renewal retries past its lease ends it inside the wait or the
/// request it is in: no request and no wait starts after the report. The thread that renewed runs the
/// reclaim and then renews under the new epoch.
TEST(CASMountRuntime, ARemountRequestEndsTheRenewalAndTheSameThreadReclaims)
{
    enum class RequestDuring : uint8_t { Wait, Request };
    const auto run = [](RequestDuring request_during)
    {
        const Layout layout(request_during == RequestDuring::Wait ? "same-thread-reclaim-in-wait" : "same-thread-reclaim-in-request");
        const UInt128 uuid{1};
        uint64_t wall_ms = 1000;
        std::atomic<uint64_t> boot_ms{100};
        std::atomic<uint64_t> past_the_lease{std::numeric_limits<uint64_t>::max()};
        std::atomic<bool> held{false};
        std::atomic<bool> outage_on{true};
        std::atomic<uint64_t> waits{0};
        std::atomic<uint64_t> writes_at_reclaim{0};
        std::atomic<uint64_t> waits_at_reclaim{0};
        std::atomic<uint64_t> worker_threads{0};
        std::atomic<bool> reclaimed{false};
        std::atomic<bool> renewed_after_reclaim{false};
        std::mutex thread_ids_mutex;
        std::thread::id renewing_thread;
        std::thread::id reclaiming_thread;
        std::thread::id renewing_after_reclaim_thread;
        DB::Cas::tests::ManualBarrier holding;
        DB::Cas::tests::ManualBarrier renewed;
        CasMountRuntime * runtime_ptr = nullptr;
        const auto record_thread = [&](std::thread::id & slot)
        {
            std::lock_guard lock(thread_ids_mutex);
            slot = std::this_thread::get_id();
        };
        const auto hold_once_past_the_lease = [&]
        {
            if (boot_ms.load() >= past_the_lease.load() && !held.exchange(true))
            {
                record_thread(renewing_thread);
                holding.arriveAndWait();
            }
        };
        RuntimeWorkerFactory factory = [&](std::function<void()> worker_body)
        {
            ++worker_threads;
            return ThreadFromGlobalPool(std::move(worker_body));
        };
        /// Declared after the locals its hooks capture.
        auto backend = std::make_shared<RuntimeRenewBackend>();
        ASSERT_EQ(claimMount(*DB::Cas::tests::OperationForTest(backend), layout, "test", uuid, 1, wall_ms, 1000).kind,
                  MountClaimResult::Claimed);
        CasEventSink sink;
        RuntimeUnderTest runtime_holder(
            backend, layout,
            MountConfig{.mount_lease_ttl_ms = std::chrono::milliseconds(1000), .background_watermark = true,
                        .boot_ms_fn = [&] { return boot_ms.load(); }, .worker_factory = factory},
            "test", sink, runtimeRenewBudget(), [&]
            {
                CasMountRuntime & reclaiming = *runtime_ptr;
                writes_at_reclaim = backend->outage_writes.load();
                waits_at_reclaim = waits.load();
                record_thread(reclaiming_thread);
                outage_on = false;
                reclaiming.beginReclaim();
                fenceOutMount(*backend, layout.mountKey("test"));
                const MountClaimResult fresh
                    = claimMount(*DB::Cas::tests::OperationForTest(backend), layout, "test", uuid, 2, wall_ms, 1000);
                EXPECT_EQ(fresh.kind, MountClaimResult::Claimed);
                if (fresh.kind != MountClaimResult::Claimed)
                    return false;
                reclaiming.installRenewer(uuid, 2, [&] { return wall_ms; });
                const uint64_t fresh_anchor = reclaiming.startRenewer();
                reclaiming.setLiveWriterEpoch(2);
                EXPECT_TRUE(reclaiming.armIfAdmissible(fresh_anchor + 1000));
                reclaimed = true;
                return true;
            });
        SCOPE_EXIT({
            holding.release();
            renewed.release();
        });
        CasMountRuntime & runtime = *runtime_holder;
        runtime_ptr = &runtime;
        runtime.installRenewer(uuid, 1, [&] { return wall_ms; });
        const uint64_t anchor = runtime.startRenewer();
        runtime.armMountFence(uuid, 1, anchor + 1000);
        /// Five lease lengths on: a renewal bounded by its lease ended long before.
        past_the_lease = anchor + 5'000;
        runtime_holder.setRetrySleepForTest([&](uint64_t ms)
        {
            ++waits;
            boot_ms += ms;
            if (request_during == RequestDuring::Wait)
                hold_once_past_the_lease();
        });
        if (request_during == RequestDuring::Request)
            backend->on_outage_write = hold_once_past_the_lease;
        backend->outage = [&] { return outage_on.load(); };
        backend->after_commit = [&]
        {
            if (reclaimed.load() && !renewed_after_reclaim.exchange(true))
            {
                record_thread(renewing_after_reclaim_thread);
                renewed.arriveAndWait();
            }
        };
        runtime.startBackgroundWorkers(std::chrono::milliseconds(0));

        holding.waitUntilArrived();
        const uint64_t writes_at_request = backend->outage_writes.load();
        const uint64_t waits_at_request = waits.load();
        /// An interference report while the renewal retries past its lease.
        runtime.tripMountLost();
        runtime.scheduleRemount();
        holding.release();
        renewed.waitUntilArrived();

        EXPECT_EQ(worker_threads.load(), 1u) << "one thread renews and reclaims";
        {
            std::lock_guard lock(thread_ids_mutex);
            EXPECT_EQ(reclaiming_thread, renewing_thread) << "the reclaim runs on the thread that renewed";
            EXPECT_EQ(renewing_after_reclaim_thread, renewing_thread) << "the same thread renews after the reclaim";
        }
        EXPECT_EQ(writes_at_reclaim.load(), writes_at_request) << "no request starts after the remount request";
        EXPECT_EQ(waits_at_reclaim.load(), waits_at_request) << "no wait starts after the remount request";
        EXPECT_EQ(decodeMountLease(readObj(*backend, layout.mountKey("test"))->bytes).writer_epoch, 2u)
            << "the renewal after the reclaim runs under the new epoch";
        EXPECT_EQ(runtime.remountRequestedGenerationForTest(), 1u);
        renewed.release();
        runtime.stopBackgroundWorkers();
        runtime.finishTeardown(false);
    };
    run(RequestDuring::Wait);
    run(RequestDuring::Request);
}

/// A renewal whose write committed before a remount request is consumed before the reclaim runs, so
/// its deadline cannot overwrite the one the reclaim arms. The renewal's last liveness check comes
/// after its write committed: the request is raised there, so the renewal still returns `Committed`.
TEST(CASMountRuntime, ACommitConsumedAfterARemountRequestCannotOverwriteTheReclaimedDeadline)
{
    const Layout layout("commit-consumed-after-request");
    const UInt128 uuid{1};
    uint64_t wall_ms = 1000;
    std::atomic<uint64_t> boot_ms{100};
    std::atomic<uint64_t> worker_threads{0};
    std::atomic<std::thread::id> first_worker_thread{};
    std::atomic<bool> committed{false};
    std::atomic<bool> requested{false};
    std::atomic<bool> held{false};
    std::atomic<bool> hold_timed_out{false};
    std::atomic<bool> armed_by_reclaim{false};
    std::atomic<uint32_t> admissions{0};
    std::promise<void> reclaim_armed;
    std::shared_future<void> reclaim_armed_future = reclaim_armed.get_future().share();
    DB::Cas::tests::ManualBarrier second_admission;
    CasMountRuntime * runtime_ptr = nullptr;
    RuntimeWorkerFactory factory = [&](std::function<void()> worker_body)
    {
        const bool first = worker_threads.fetch_add(1) == 0;
        return ThreadFromGlobalPool([&, first, body = std::move(worker_body)]
        {
            if (first)
                first_worker_thread = std::this_thread::get_id();
            body();
        });
    };
    /// With a second thread the reclaim can run before the renewing thread consumes its result. Hold
    /// that consumption at its first clock read until the reclaim armed. With one thread this never
    /// waits: the consumption comes before the reclaim on the same thread.
    const auto hold_consumption_until_armed = [&]
    {
        if (worker_threads.load() == 2 && requested.load()
            && std::this_thread::get_id() == first_worker_thread.load() && !held.exchange(true))
        {
            if (reclaim_armed_future.wait_for(std::chrono::seconds(20)) != std::future_status::ready)
                hold_timed_out = true;
        }
    };
    /// Declared after the locals its hooks capture.
    auto backend = std::make_shared<RuntimeRenewBackend>();
    ASSERT_EQ(claimMount(*DB::Cas::tests::OperationForTest(backend), layout, "test", uuid, 1, wall_ms, 1000).kind,
              MountClaimResult::Claimed);
    CasEventSink sink;
    RuntimeUnderTest runtime_holder(
        backend, layout,
        MountConfig{
            .mount_lease_ttl_ms = std::chrono::milliseconds(1000),
            .background_watermark = true,
            .boot_ms_fn = [&]
            {
                hold_consumption_until_armed();
                return boot_ms.load();
            },
            .worker_factory = factory,
            .renewal_admitted_hook_for_test = [&]
            {
                if (++admissions == 2)
                    second_admission.arriveAndWait();
            },
            .renewal_live_for_test = [&]
            {
                if (committed.load() && !requested.exchange(true))
                    runtime_ptr->scheduleRemount();
                return true;
            }},
        "test", sink, runtimeRenewBudget(), [&]
        {
            CasMountRuntime & reclaiming = *runtime_ptr;
            reclaiming.beginReclaim();
            fenceOutMount(*backend, layout.mountKey("test"));
            const MountClaimResult fresh
                = claimMount(*DB::Cas::tests::OperationForTest(backend), layout, "test", uuid, 2, wall_ms, 1000);
            EXPECT_EQ(fresh.kind, MountClaimResult::Claimed);
            if (fresh.kind != MountClaimResult::Claimed)
                return false;
            reclaiming.installRenewer(uuid, 2, [&] { return wall_ms; });
            boot_ms = 500;
            const uint64_t fresh_anchor = reclaiming.startRenewer();
            reclaiming.setLiveWriterEpoch(2);
            armed_by_reclaim = reclaiming.armIfAdmissible(fresh_anchor + 1000);
            reclaim_armed.set_value();
            return true;
        });
    SCOPE_EXIT({ second_admission.release(); });
    CasMountRuntime & runtime = *runtime_holder;
    runtime_ptr = &runtime;
    runtime.installRenewer(uuid, 1, [&] { return wall_ms; });
    const uint64_t anchor = runtime.startRenewer();
    runtime.armMountFence(uuid, 1, anchor + 1000);
    /// Set after the setup's own writes: only the loop's renewal may raise the request.
    backend->after_commit = [&] { committed = true; };
    runtime.startBackgroundWorkers(std::chrono::milliseconds(0));

    second_admission.waitUntilArrived();
    EXPECT_FALSE(hold_timed_out.load());
    ASSERT_TRUE(armed_by_reclaim.load());
    /// Between the first renewal's deadline (100 + 1000) and the reclaim's (500 + 1000).
    boot_ms = 1200;
    EXPECT_TRUE(runtime.mayMutate())
        << "the deadline is the reclaim's, not the one of the renewal consumed after the request";
    second_admission.release();
    runtime.stopBackgroundWorkers();
    runtime.finishTeardown(false);
}

/// While the lease thread runs, no other thread may replace, reset or start the renewer. Constructing
/// a `LOGICAL_ERROR` exception aborts under a debug or sanitizer build, so there the same contract is
/// a death expectation.
#ifndef DEBUG_OR_SANITIZER_BUILD
TEST(CASMountRuntime, OnlyTheLeaseThreadReplacesTheRenewerWhileItRuns)
#else
TEST(CASMountRuntimeDeathTest, OnlyTheLeaseThreadReplacesTheRenewerWhileItRuns)
#endif
{
    auto backend = std::make_shared<RuntimeRenewBackend>();
    const Layout layout("runtime-renewer-owner");
    uint64_t wall_ms = 1000;
    const uint64_t boot_ms = 100;
    const UInt128 uuid{1};
    ASSERT_EQ(claimMount(*DB::Cas::tests::OperationForTest(backend), layout, "test", uuid, 1, wall_ms, 1000).kind,
              MountClaimResult::Claimed);
    DB::Cas::tests::ManualBarrier admitted;
    CasEventSink sink;
    RuntimeUnderTest runtime_holder(
        backend, layout,
        MountConfig{.mount_lease_ttl_ms = std::chrono::milliseconds(1000), .background_watermark = true,
                    .boot_ms_fn = [&] { return boot_ms; },
                    .renewal_admitted_hook_for_test = [&] { admitted.arriveAndWait(); }},
        "test", sink, runtimeRenewBudget(), [] { return false; });
    SCOPE_EXIT({ admitted.release(); });
    CasMountRuntime & runtime = *runtime_holder;
    runtime.installRenewer(uuid, 1, [&] { return wall_ms; });
    const uint64_t anchor = runtime.startRenewer();
    runtime.armMountFence(uuid, 1, anchor + 1000);
    runtime.startBackgroundWorkers(std::chrono::milliseconds(0));
    /// The lease thread is held between its decision to renew and the renewal, with no lock held.
    admitted.waitUntilArrived();

#ifndef DEBUG_OR_SANITIZER_BUILD
    const auto expect_owner_refusal = [](const char * what, const std::function<void()> & call)
    {
        try
        {
            call();
            ADD_FAILURE() << what << " from a test thread must be refused while the lease thread runs";
        }
        catch (const DB::Exception & e)
        {
            EXPECT_EQ(e.code(), DB::ErrorCodes::LOGICAL_ERROR) << what;
            EXPECT_NE(e.message().find("only the lease thread"), String::npos) << what << ": " << e.message();
        }
    };
    expect_owner_refusal("installRenewer", [&] { runtime.installRenewer(uuid, 2, [&] { return wall_ms; }); });
    expect_owner_refusal("renewerReset", [&] { runtime.renewerReset(); });
    expect_owner_refusal("startRenewer", [&] { (void)runtime.startRenewer(); });
#else
    /// The child exits through `std::_Exit` if the call does not abort: running exit handlers in a
    /// forked child can block on a thread pool mutex a vanished thread held.
    EXPECT_DEATH({ runtime.installRenewer(uuid, 2, [&] { return wall_ms; }); std::_Exit(0); }, "only the lease thread");
    EXPECT_DEATH({ runtime.renewerReset(); std::_Exit(0); }, "only the lease thread");
    EXPECT_DEATH({ (void)runtime.startRenewer(); std::_Exit(0); }, "only the lease thread");
#endif

    admitted.release();
    runtime.stopBackgroundWorkers();
    /// With the thread joined the caller drives the renewer again.
    EXPECT_NO_THROW(runtime.renewerReset());
    runtime.finishTeardown(false);
}

TEST(CASPool, RenewWatermarkOnceRefreshesFenceAndDepositsOneFailure)
{
    auto backend = std::make_shared<RuntimeRenewBackend>();
    /// Held in a shared atomic, not a plain local: this test mutates it directly below, and the Pool
    /// can outlive this stack frame (a background publish holds `shared_from_this()`), so a
    /// by-reference capture of a local would dangle.
    auto fake_boot = std::make_shared<std::atomic<uint64_t>>(100);
    PoolConfig config{
        .pool_prefix = "direct-renew",
        .server_root_id = "test",
        .mount_lease_ttl_ms = std::chrono::milliseconds(1000),
        .cas_request_budget = runtimeRenewBudget(),
        .boot_ms_fn = [fake_boot]
        {
            return fake_boot->load();
        },
    };
    auto store = Pool::open(backend, config);
    fake_boot->store(500);
    EXPECT_NO_THROW(store->renewWatermarkOnce());
    fake_boot->store(1200);
    EXPECT_TRUE(store->mayMutate()) << "direct success must refresh the local fence from attempt start";

    /// GC fences the slot out: a definitive answer, so the renewal ends on its first attempt.
    fenceOutMount(*backend, store->layout().mountKey("test"));
    const uint64_t schedules_before = store->scheduleRemountCallCountForTest();
    expectThrowsCode(DB::ErrorCodes::ABORTED, [&] { store->renewWatermarkOnce(); });
    EXPECT_FALSE(store->mayMutate());
    EXPECT_EQ(store->scheduleRemountCallCountForTest(), schedules_before + 1);
}

TEST(CASPoolRemount, WholeChainResultsAreNumberedAndStepLabelled)
{
    auto backend = std::make_shared<RemountStepBackend>();
    /// Heap-owned, not a plain local: the Pool can outlive this stack frame (a background publish holds
    /// `shared_from_this()`), so a by-reference capture of a local would dangle.
    auto events = std::make_shared<DB::Cas::tests::SharedEventLog>();
    auto store = Pool::open(backend, PoolConfig{
        .pool_prefix = "remount-observability",
        .server_root_id = "test",
    });
    store->setEventSink([events](CasEvent event)
    {
        events->push(std::move(event));
    });
    ScopedRemountLogCapture logs;

    store->tripMountLost();
    backend->failNextRead(store->layout().poolMetaKey());
    const uint64_t attempts_before = ProfileEvents::global_counters[ProfileEvents::CASRemountAttempts].load();
    const uint64_t succeeded_before = ProfileEvents::global_counters[ProfileEvents::CASRemountSucceeded].load();
    const uint64_t failed_before = ProfileEvents::global_counters[ProfileEvents::CASRemountFailed].load();
    EXPECT_FALSE(store->tryRemountOnce());

    fenceOutMount(*backend, store->layout().mountKey("test"));
    EXPECT_TRUE(store->tryRemountOnce());

    EXPECT_EQ(ProfileEvents::global_counters[ProfileEvents::CASRemountAttempts].load(), attempts_before + 2);
    EXPECT_EQ(ProfileEvents::global_counters[ProfileEvents::CASRemountSucceeded].load(), succeeded_before + 1);
    EXPECT_EQ(ProfileEvents::global_counters[ProfileEvents::CASRemountFailed].load(), failed_before + 1);

    const std::vector<CasEvent> observed_events = events->snapshot();
    std::vector<CasEvent> remounts;
    std::copy_if(observed_events.begin(), observed_events.end(), std::back_inserter(remounts), [](const CasEvent & event)
    {
        return event.type == CasEventType::MountRemount;
    });
    ASSERT_EQ(remounts.size(), 2u);
    EXPECT_EQ(remounts[0].outcome, "failed");
    EXPECT_EQ(remounts[0].detail.at("step"), "pool_identity_probe");
    EXPECT_EQ(remounts[1].outcome, "ok");
    EXPECT_EQ(remounts[1].detail.at("step"), "publish_live");
    const uint64_t first_attempt = std::stoull(remounts[0].detail.at("attempt_no"));
    const uint64_t second_attempt = std::stoull(remounts[1].detail.at("attempt_no"));
    EXPECT_EQ(second_attempt, first_attempt + 1);
    EXPECT_EQ(countRemountFinalLogs(logs.captured()), 2u) << logs.captured();
}

TEST(CASPoolRemount, LeaseLossHasOneOperationalOwner)
{
    auto backend = std::make_shared<RemountStepBackend>();
    auto store = Pool::open(backend, PoolConfig{
        .pool_prefix = "lease-loss-owner",
        .server_root_id = "test",
    });
    const uint64_t lost_before = ProfileEvents::global_counters[ProfileEvents::CASMountLeaseLost].load();

    store->tripMountLost();
    store->tripMountLost();
    backend->failNextRead(store->layout().poolMetaKey());
    EXPECT_FALSE(store->tryRemountOnce());
    store->beginShutdownForTest();
    store->tripMountLost();

    EXPECT_EQ(ProfileEvents::global_counters[ProfileEvents::CASMountLeaseLost].load(), lost_before + 1);
}

TEST(CASPoolRemount, LiveForgetDoesNotCountOperationalLeaseLoss)
{
    auto backend = std::make_shared<InMemoryBackend>();
    auto store = Pool::open(backend, PoolConfig{
        .pool_prefix = "forget-is-not-lease-loss",
        .server_root_id = "test",
    });
    ASSERT_EQ(store->lifecycle(), PoolLifecycle::Live);
    const uint64_t lost_before = ProfileEvents::global_counters[ProfileEvents::CASMountLeaseLost].load();

    store->forgetDisk([] {}, "deliberate test decommission");

    EXPECT_EQ(store->lifecycle(), PoolLifecycle::VanishedForgotten);
    EXPECT_EQ(ProfileEvents::global_counters[ProfileEvents::CASMountLeaseLost].load(), lost_before)
        << "a deliberate terminal decommission is not an operational recovery generation";
}

/// Coverage gap (Task 13a): restores the get/exists/remove roundtrip for the mount access-check probe
/// object. The old `CASPool.MountpointObjectRoundTrip` was dropped in the refactor; the wiring test only
/// exercises `putMountpointObject` + `existsFile`, leaving `getMountpointObject`'s value round-trip and
/// `removeMountpointObject` unasserted even though both `Pool` methods remain live.
TEST(CASPool, MountpointObjectRoundTrip)
{
    auto b = std::make_shared<DB::Cas::InMemoryBackend>();
    auto store = DB::Cas::Pool::open(b, DB::Cas::PoolConfig{.pool_prefix = "p", .server_root_id = "test"});
    const String key = "srv1/clickhouse_access_check_abc";
    EXPECT_FALSE(store->getMountpointObject(key).has_value());
    EXPECT_FALSE(store->mountpointObjectExists(key));
    store->putMountpointObject(key, "probe-bytes");
    EXPECT_TRUE(store->mountpointObjectExists(key));
    auto got = store->getMountpointObject(key);
    ASSERT_TRUE(got.has_value());
    EXPECT_EQ(*got, "probe-bytes");
    store->removeMountpointObject(key);
    EXPECT_FALSE(store->getMountpointObject(key).has_value());
    EXPECT_FALSE(store->mountpointObjectExists(key));
}

namespace ProfileEvents
{
    extern const Event CASHotKeyReadStarts;
    extern const Event CASRequestResolveRead;
    extern const Event CASRequestConflictPause;
}

TEST(CASPool, ConcurrentNamespaceCreationsNeverRaceEachOtherOnTheCatalog)
{
    auto backend = std::make_shared<DB::Cas::tests::CountingBackend>();
    auto pool = DB::Cas::tests::openPoolForTest(backend);
    const DB::Cas::Layout layout("p");
    const String key = layout.refCatalogKey();
    constexpr int N = 6;

    /// Drain whatever the pool's own bootstrap touched on the catalog key before measuring.
    (void)pool->namespaceLife(DB::Cas::RootNamespace{"warmup"});

    const uint64_t writes_before = backend->writeCount(key);
    const auto reads_before = ProfileEvents::global_counters[ProfileEvents::CASHotKeyReadStarts].load();
    const auto resolves_before = ProfileEvents::global_counters[ProfileEvents::CASRequestResolveRead].load();
    std::vector<std::thread> threads;
    for (int i = 0; i < N; ++i)
        threads.emplace_back([&, i] { (void)pool->namespaceLife(DB::Cas::RootNamespace{"ns" + std::to_string(i)}); });
    for (auto & t : threads)
        t.join();

    EXPECT_EQ(backend->writeCount(key) - writes_before, 2u * N) << "two catalog steps per creation, each one write";
    EXPECT_EQ(ProfileEvents::global_counters[ProfileEvents::CASRequestResolveRead].load() - resolves_before, 0u)
        << "no refused precondition, so no resolve read";
    EXPECT_LE(ProfileEvents::global_counters[ProfileEvents::CASHotKeyReadStarts].load() - reads_before, 1u)
        << "at most one lane read; every later hold started from the cache";

    /// Another server writes the catalog between two of this pool's mutations: one extra read and one
    /// retry write, then the cache is current again. Raw `getCount` cannot isolate that cost: a
    /// `namespaceLife` call on a fresh namespace also issues the ledger's own snapshot reads
    /// (`CasRefCatalog::read`), which are outside the lane by design and fire the same number of times
    /// whether or not an external write happened. The lane's own signals are what the external write
    /// actually moves.
    {
        auto external_requests = DB::Cas::tests::openRequestsForTest(backend);
        auto external = external_requests.admit();
        DB::Cas::CasRefCatalog::casAdmitEntry(external, layout, 1,
            DB::Cas::CatalogEntry{.ns = DB::Cas::RootNamespace{"zz"}, .state = DB::Cas::NsState::Live, .incarnation = UInt128{99}});
    }
    const uint64_t writes_mid = backend->writeCount(key);
    const auto resolves_mid = ProfileEvents::global_counters[ProfileEvents::CASRequestResolveRead].load();
    const auto lane_reads_mid = ProfileEvents::global_counters[ProfileEvents::CASHotKeyReadStarts].load();
    (void)pool->namespaceLife(DB::Cas::RootNamespace{"after"});
    EXPECT_EQ(ProfileEvents::global_counters[ProfileEvents::CASRequestResolveRead].load() - resolves_mid, 1u)
        << "one resolve read for the external write";
    EXPECT_EQ(ProfileEvents::global_counters[ProfileEvents::CASHotKeyReadStarts].load() - lane_reads_mid, 0u)
        << "the next hold starts from what the resolve read saw";
    EXPECT_EQ(backend->writeCount(key) - writes_mid, 3u) << "one refused, two landed";
}

namespace
{

/// Fails the guarded mount `PUT`s that `on_put` says to fail, with a timeout raised before the store
/// applied anything, and the reads of the mount slot that `on_read` says to fail. Both run on the
/// renewing thread with the 1-based number of the request of their kind.
class ExpiryScriptBackend final : public DB::Cas::tests::CountingBackend
{
public:
    std::function<bool(uint32_t put_no)> on_put;
    std::function<bool(uint32_t read_no)> on_read;

    std::optional<DB::Cas::Backend::Raw> read(const String & key, DB::Cas::TransportAccess & access) override
    {
        if (on_read && key.ends_with("/mount") && on_read(++reads))
            throw Poco::TimeoutException("injected resolve read timeout");
        return DB::Cas::tests::CountingBackend::read(key, access);
    }

    std::expected<String, RawConflict> write(const String & key, const String & bytes,
        const std::optional<String> & expected_value, DB::Cas::TransportAccess & access) override
    {
        if (on_put && expected_value && key.ends_with("/mount") && on_put(++puts))
            throw Poco::TimeoutException("injected renewal timeout before the store applied it");
        return DB::Cas::tests::CountingBackend::write(key, bytes, expected_value, access);
    }

private:
    uint32_t puts = 0;
    uint32_t reads = 0;
};

constexpr uint64_t kExpiryTtlMs = 30'000;
constexpr uint64_t kExpiryPeriodMs = 10'000;
constexpr uint64_t kExpiryClaimBootMs = 100'000;
constexpr uint64_t kExpiryDeadlineBootMs = kExpiryClaimBootMs + kExpiryTtlMs;
constexpr uint64_t kExpiryFirstStartBootMs = kExpiryClaimBootMs + kExpiryPeriodMs;
/// Longer than the largest retry-spacing draw, so a failed request is retried at once.
constexpr uint64_t kExpiryFailedPutMs = 1'300;
/// Enough failures to carry the first renewal past its own start + TTL.
constexpr uint32_t kExpiryFailedPuts = 24;
constexpr uint64_t kExpiryRestoreBootMs = kExpiryFirstStartBootMs + kExpiryFailedPuts * kExpiryFailedPutMs;
/// A request sent while the lease is expired and the first renewal still retries.
constexpr uint32_t kExpiryObservedPut = 20;
static_assert(kExpiryFirstStartBootMs + (kExpiryObservedPut - 1) * kExpiryFailedPutMs > kExpiryDeadlineBootMs);
static_assert(kExpiryRestoreBootMs > kExpiryFirstStartBootMs + kExpiryTtlMs);

const char * expiryAdmitName(Fence::Admit verdict)
{
    switch (verdict)
    {
        case Fence::Admit::Ok: return "Ok";
        case Fence::Admit::LostOrRearmed: return "LostOrRearmed";
        case Fence::Admit::NoBudget: return "NoBudget";
    }
    return "unknown";
}

uint64_t eventCount(ProfileEvents::Event event)
{
    return ProfileEvents::global_counters[event].load();
}

struct ExpiryObservation
{
    uint64_t generation_before = 0;
    uint64_t attempts_before = 0;
    uint64_t retries_before = 0;
    uint64_t lease_expired_before = 0;
    uint64_t lease_lost_before = 0;

    /// Inside `PUT` number `kExpiryObservedPut`.
    Fence::Admit admit_during = Fence::Admit::Ok;
    bool may_mutate_during = true;
    PoolLifecycle lifecycle_during = PoolLifecycle::TransientNotLive;
    std::optional<uint64_t> expired_since_during;
    String last_failure_during;
    uint64_t attempts_during = 0;
    uint64_t retries_during = 0;

    /// At the loop pass after the renewal that committed with a start more than a TTL ago.
    Fence::Admit admit_after_stale = Fence::Admit::Ok;
    std::optional<uint64_t> expired_since_after_stale;
    String last_failure_after_stale;
    uint64_t lease_expired_after_stale = 0;
    uint64_t boot_ms_after_stale = 0;
    uint64_t boot_ms_at_next_admission = 0;

    /// At the loop pass after the restoring renewal.
    Fence::Admit admit_after_restore = Fence::Admit::LostOrRearmed;
    std::optional<uint64_t> expired_since_after_restore;
    String last_failure_after_restore;
    PoolLifecycle lifecycle_after_restore = PoolLifecycle::TransientNotLive;
    uint64_t generation_after_restore = 0;
    uint64_t lease_expired_after_restore = 0;
    uint64_t lease_lost_after_restore = 0;
    uint64_t attempts_after_restore = 0;
    uint64_t retries_after_restore = 0;

    uint64_t writer_epoch_on_store = 0;
    uint32_t remount_calls = 0;
    std::vector<CasEvent> events;
    String log;
};

void runExpiryScenario(const String & layout_prefix, ExpiryObservation & seen)
{
    /// Everything the hooks capture is declared before the backend and the runtime that store them.
    const Layout layout(layout_prefix);
    const UInt128 uuid{1};
    uint64_t wall_ms = 1000;
    std::atomic<uint64_t> boot_ms{kExpiryClaimBootMs};
    std::atomic<uint32_t> remount_calls{0};
    uint32_t loop_passes = 0;
    DB::Cas::tests::ManualBarrier third_pass;
    CasMountRuntime * runtime_ptr = nullptr;
    auto events = std::make_shared<DB::Cas::tests::SharedEventLog>();
    CasEventSink sink = [events](CasEvent event) { events->push(std::move(event)); };
    ScopedRemountLogCapture log_capture;
    auto backend = std::make_shared<ExpiryScriptBackend>();

    ASSERT_EQ(claimMount(*DB::Cas::tests::OperationForTest(backend), layout, "test", uuid, 1, wall_ms, kExpiryTtlMs).kind,
              MountClaimResult::Claimed);

    const auto observe_after_stale = [&]
    {
        CasMountRuntime & runtime = *runtime_ptr;
        seen.admit_after_stale = runtime.admit(seen.generation_before, 0);
        seen.expired_since_after_stale = runtime.leaseExpiredSinceBootMs();
        seen.last_failure_after_stale = runtime.lastRenewFailure();
        seen.lease_expired_after_stale = eventCount(ProfileEvents::CASMountLeaseExpired) - seen.lease_expired_before;
        seen.boot_ms_after_stale = boot_ms.load();
    };
    const auto observe_after_restore = [&]
    {
        CasMountRuntime & runtime = *runtime_ptr;
        seen.admit_after_restore = runtime.admit(seen.generation_before, 0);
        seen.expired_since_after_restore = runtime.leaseExpiredSinceBootMs();
        seen.last_failure_after_restore = runtime.lastRenewFailure();
        seen.lifecycle_after_restore = runtime.lifecycle();
        seen.generation_after_restore = runtime.fenceGeneration();
        seen.lease_expired_after_restore = eventCount(ProfileEvents::CASMountLeaseExpired) - seen.lease_expired_before;
        seen.lease_lost_after_restore = eventCount(ProfileEvents::CASMountLeaseLost) - seen.lease_lost_before;
        seen.attempts_after_restore = eventCount(ProfileEvents::CASMountRenewalAttempts) - seen.attempts_before;
        seen.retries_after_restore = eventCount(ProfileEvents::CASMountRenewalRetries) - seen.retries_before;
    };

    RuntimeUnderTest runtime_holder(
        backend, layout,
        MountConfig{
            .mount_lease_ttl_ms = std::chrono::milliseconds(kExpiryTtlMs),
            .background_watermark = true,
            .boot_ms_fn = [&] { return boot_ms.load(); },
            .renewal_before_driver_lock_hook_for_test = [&]
            {
                ++loop_passes;
                if (loop_passes == 1)
                {
                    boot_ms.store(kExpiryFirstStartBootMs);
                }
                else if (loop_passes == 2)
                {
                    observe_after_stale();
                }
                else if (loop_passes == 3)
                {
                    observe_after_restore();
                    third_pass.arriveAndWait();
                }
            },
            .renewal_admitted_hook_for_test = [&]
            {
                if (loop_passes == 2)
                    seen.boot_ms_at_next_admission = boot_ms.load();
            }},
        "test", sink, runtimeRenewBudget(), [&]
        {
            ++remount_calls;
            return false;
        });
    CasMountRuntime & runtime = *runtime_holder;
    runtime_ptr = &runtime;
    runtime.installRenewer(uuid, 1, [&] { return wall_ms; });
    const uint64_t anchor = runtime.startRenewer();
    ASSERT_EQ(anchor, kExpiryClaimBootMs);
    runtime.armMountFence(uuid, 1, anchor + kExpiryTtlMs);

    seen.generation_before = runtime.fenceGeneration();
    seen.attempts_before = eventCount(ProfileEvents::CASMountRenewalAttempts);
    seen.retries_before = eventCount(ProfileEvents::CASMountRenewalRetries);
    seen.lease_expired_before = eventCount(ProfileEvents::CASMountLeaseExpired);
    seen.lease_lost_before = eventCount(ProfileEvents::CASMountLeaseLost);

    backend->on_put = [&](uint32_t put_no)
    {
        if (put_no == kExpiryObservedPut)
        {
            CasMountRuntime & renewing = *runtime_ptr;
            seen.admit_during = renewing.admit(seen.generation_before, 0);
            seen.may_mutate_during = renewing.mayMutate();
            seen.lifecycle_during = renewing.lifecycle();
            seen.expired_since_during = renewing.leaseExpiredSinceBootMs();
            seen.last_failure_during = renewing.lastRenewFailure();
            seen.attempts_during = eventCount(ProfileEvents::CASMountRenewalAttempts) - seen.attempts_before;
            seen.retries_during = eventCount(ProfileEvents::CASMountRenewalRetries) - seen.retries_before;
        }
        if (put_no > kExpiryFailedPuts)
            return false;
        boot_ms.fetch_add(kExpiryFailedPutMs);
        return true;
    };

    runtime.startBackgroundWorkers(std::chrono::milliseconds(kExpiryPeriodMs));
    third_pass.waitUntilArrived();

    seen.writer_epoch_on_store = decodeMountLease(readObj(*backend, layout.mountKey("test"))->bytes).writer_epoch;
    seen.remount_calls = remount_calls.load();
    seen.events = events->snapshot();
    seen.log = log_capture.captured();

    third_pass.release();
    runtime.stopBackgroundWorkers();
    runtime.finishTeardown(false);
}

/// One renewal whose first `PUT` is unclear and whose resolve reads then fail long enough for the lease
/// to expire; the 19th read finds the slot unchanged and the reissued `PUT` lands.
constexpr uint32_t kReadOutageFailedReads = 18;
constexpr uint32_t kReadOutageObservedRead = 17;
static_assert(kExpiryFirstStartBootMs + (kReadOutageObservedRead - 1) * kExpiryFailedPutMs > kExpiryDeadlineBootMs);

struct ReadOutageObservation
{
    uint64_t attempts_before = 0;
    uint64_t retries_before = 0;
    /// Inside read number `kReadOutageObservedRead`.
    std::optional<uint64_t> expired_since_during;
    String last_failure_during;
    uint64_t attempts_during = 0;
    uint64_t retries_during = 0;
    /// At the loop pass after the renewal.
    uint64_t attempts_after = 0;
    uint64_t retries_after = 0;
};

void runReadOutageScenario(const String & layout_prefix, ReadOutageObservation & seen)
{
    const Layout layout(layout_prefix);
    const UInt128 uuid{1};
    uint64_t wall_ms = 1000;
    std::atomic<uint64_t> boot_ms{kExpiryClaimBootMs};
    uint32_t loop_passes = 0;
    DB::Cas::tests::ManualBarrier second_pass;
    CasMountRuntime * runtime_ptr = nullptr;
    CasEventSink sink;
    auto backend = std::make_shared<ExpiryScriptBackend>();

    ASSERT_EQ(claimMount(*DB::Cas::tests::OperationForTest(backend), layout, "test", uuid, 1, wall_ms, kExpiryTtlMs).kind,
              MountClaimResult::Claimed);

    RuntimeUnderTest runtime_holder(
        backend, layout,
        MountConfig{
            .mount_lease_ttl_ms = std::chrono::milliseconds(kExpiryTtlMs),
            .background_watermark = true,
            .boot_ms_fn = [&] { return boot_ms.load(); },
            .renewal_before_driver_lock_hook_for_test = [&]
            {
                ++loop_passes;
                if (loop_passes == 1)
                {
                    boot_ms.store(kExpiryFirstStartBootMs);
                }
                else if (loop_passes == 2)
                {
                    seen.attempts_after = eventCount(ProfileEvents::CASMountRenewalAttempts) - seen.attempts_before;
                    seen.retries_after = eventCount(ProfileEvents::CASMountRenewalRetries) - seen.retries_before;
                    second_pass.arriveAndWait();
                }
            }},
        "test", sink, runtimeRenewBudget(), [] { return false; });
    CasMountRuntime & runtime = *runtime_holder;
    runtime_ptr = &runtime;
    runtime.installRenewer(uuid, 1, [&] { return wall_ms; });
    const uint64_t anchor = runtime.startRenewer();
    runtime.armMountFence(uuid, 1, anchor + kExpiryTtlMs);

    seen.attempts_before = eventCount(ProfileEvents::CASMountRenewalAttempts);
    seen.retries_before = eventCount(ProfileEvents::CASMountRenewalRetries);
    backend->on_put = [](uint32_t put_no) { return put_no == 1; };
    backend->on_read = [&](uint32_t read_no)
    {
        if (read_no == kReadOutageObservedRead)
        {
            seen.expired_since_during = runtime_ptr->leaseExpiredSinceBootMs();
            seen.last_failure_during = runtime_ptr->lastRenewFailure();
            seen.attempts_during = eventCount(ProfileEvents::CASMountRenewalAttempts) - seen.attempts_before;
            seen.retries_during = eventCount(ProfileEvents::CASMountRenewalRetries) - seen.retries_before;
        }
        if (read_no > kReadOutageFailedReads)
            return false;
        boot_ms.fetch_add(kExpiryFailedPutMs);
        return true;
    };

    runtime.startBackgroundWorkers(std::chrono::milliseconds(kExpiryPeriodMs));
    second_pass.waitUntilArrived();
    second_pass.release();
    runtime.stopBackgroundWorkers();
    runtime.finishTeardown(false);
}

std::vector<CasEvent> renewEventsOf(const std::vector<CasEvent> & events)
{
    std::vector<CasEvent> renewals;
    for (const CasEvent & event : events)
        if (event.type == CasEventType::WatermarkRenew)
            renewals.push_back(event);
    return renewals;
}

}

/// An expiry refuses writes without touching the fence, a renewal that commits with a start more than a
/// TTL ago restores nothing and is followed at once, and the next one restores writes under the same
/// epoch and generation. Runs the real worker loop.
TEST(CASMountRuntime, ExpiryRefusesWritesAndResumesUnderTheSameEpoch)
{
    ExpiryObservation seen;
    ASSERT_NO_FATAL_FAILURE(runExpiryScenario("runtime-expiry-resume", seen));

    EXPECT_STREQ(expiryAdmitName(seen.admit_during), "NoBudget") << "refused, and not because the fence is lost";
    EXPECT_FALSE(seen.may_mutate_during);
    EXPECT_EQ(seen.lifecycle_during, PoolLifecycle::Live);

    EXPECT_STREQ(expiryAdmitName(seen.admit_after_stale), "NoBudget")
        << "a renewal whose start + TTL is already past restores nothing";
    /// The clock moves only when the test moves it, so a renewal admitted at all after the stale success
    /// was admitted without a cadence wait; a wait would have left loop pass 3 unreached.
    EXPECT_EQ(seen.boot_ms_at_next_admission, seen.boot_ms_after_stale);

    EXPECT_STREQ(expiryAdmitName(seen.admit_after_restore), "Ok");
    EXPECT_EQ(seen.lifecycle_after_restore, PoolLifecycle::Live);
    EXPECT_EQ(seen.generation_after_restore, seen.generation_before) << "an expiry is not a re-arm";
    EXPECT_EQ(seen.lease_lost_after_restore, 0u);
    EXPECT_EQ(seen.writer_epoch_on_store, 1u) << "the same epoch holds the slot";
    EXPECT_EQ(seen.remount_calls, 0u);
}

/// What an operator sees: the expiry and its last failure while it lasts, the attempt counters moving
/// during the outage, and one counter, event field and warning when a renewal restores the lease.
TEST(CASMountRuntime, ExpiryIsReported)
{
    ExpiryObservation seen;
    ASSERT_NO_FATAL_FAILURE(runExpiryScenario("runtime-expiry-report", seen));

    ASSERT_TRUE(seen.expired_since_during.has_value());
    EXPECT_EQ(*seen.expired_since_during, kExpiryDeadlineBootMs);
    EXPECT_NE(seen.last_failure_during.find("injected renewal timeout"), String::npos) << seen.last_failure_during;
    /// The request being sent is counted before it reaches the store.
    EXPECT_EQ(seen.attempts_during, kExpiryObservedPut) << "attempts advance as requests are sent";
    EXPECT_EQ(seen.retries_during, kExpiryObservedPut - 1);

    ASSERT_TRUE(seen.expired_since_after_stale.has_value());
    EXPECT_EQ(*seen.expired_since_after_stale, kExpiryDeadlineBootMs)
        << "a renewal committed past its own start + TTL keeps the expiry and when it began";
    EXPECT_EQ(seen.lease_expired_after_stale, 0u) << "that renewal is not a restore";
    EXPECT_NE(seen.last_failure_after_stale.find("injected renewal timeout"), String::npos)
        << "the lease is still expired, so its failures still explain it: " << seen.last_failure_after_stale;

    EXPECT_FALSE(seen.expired_since_after_restore.has_value());
    EXPECT_TRUE(seen.last_failure_after_restore.empty())
        << "a restore ends the outage the text described: " << seen.last_failure_after_restore;
    EXPECT_EQ(seen.lease_expired_after_restore, 1u);
    /// 25 requests by the first renewal and one by the second, each counted once.
    EXPECT_EQ(seen.attempts_after_restore, kExpiryFailedPuts + 2);
    EXPECT_EQ(seen.retries_after_restore, kExpiryFailedPuts);

    const std::vector<CasEvent> renewals = renewEventsOf(seen.events);
    ASSERT_EQ(renewals.size(), 2u) << "the retried renewal and the restoring one";
    EXPECT_FALSE(renewals[0].detail.contains("expired_ms")) << "the first renewal left the lease expired";
    EXPECT_EQ(renewals[0].detail.at("classification"), "committed_after_retry");
    EXPECT_EQ(renewals[1].outcome, "recovered");
    EXPECT_EQ(renewals[1].detail.at("classification"), "committed_after_expiry");
    ASSERT_TRUE(renewals[1].detail.contains("expired_ms"));
    EXPECT_EQ(renewals[1].detail.at("expired_ms"), std::to_string(kExpiryRestoreBootMs - kExpiryDeadlineBootMs));

    EXPECT_NE(seen.log.find(fmt::format("expired for {} ms", kExpiryRestoreBootMs - kExpiryDeadlineBootMs)), String::npos)
        << seen.log;
    EXPECT_NE(seen.log.find("injected renewal timeout"), String::npos) << "the warning names the last failure: " << seen.log;

    /// An unclear `PUT` whose resolve reads then fail: the reads are what the operator needs to see,
    /// and they are not requests of the renewal.
    ReadOutageObservation reads;
    ASSERT_NO_FATAL_FAILURE(runReadOutageScenario("runtime-expiry-read-outage", reads));
    ASSERT_TRUE(reads.expired_since_during.has_value()) << "the read outage outlasted the lease, so the row shows this text";
    EXPECT_NE(reads.last_failure_during.find("injected resolve read timeout"), String::npos)
        << "lifecycle_detail carries the last failed read: " << reads.last_failure_during;
    EXPECT_EQ(reads.attempts_during, 1u) << "failed reads do not advance the attempt counter";
    EXPECT_EQ(reads.retries_during, 0u);
    EXPECT_EQ(reads.attempts_after, 2u) << "the unclear PUT and its reissue";
    EXPECT_EQ(reads.retries_after, 1u);
}

namespace
{

const String kExpiryWarningPrefix = "CAS mount lease of 'test' expired ";
const String kRestoreWarningText = "was expired for";

size_t countText(const String & haystack, const String & needle)
{
    size_t count = 0;
    for (size_t at = haystack.find(needle); at != String::npos; at = haystack.find(needle, at + needle.size()))
        ++count;
    return count;
}

/// What the lease thread sees while it runs `runExpiryLogScenario`. Declared by the test before the
/// runtime that stores the hooks capturing it.
struct ExpiryLogRig
{
    ScopedRemountLogCapture log;
    std::atomic<uint64_t> boot_ms{kExpiryClaimBootMs};
    CasMountRuntime * runtime = nullptr;
    uint64_t generation = 0;

    /// Return true to fail the guarded mount `PUT` with that 1-based number.
    std::function<bool(ExpiryLogRig &, uint32_t put_no)> on_put;
    std::function<void(ExpiryLogRig &, uint32_t pass)> on_pass;

    size_t expiryWarnings() const { return countText(log.captured(), kExpiryWarningPrefix); }

    /// Per `PUT`, as the lease thread enters it.
    std::vector<size_t> expiry_at_put;
    std::vector<uint64_t> boot_at_put;
    /// Per loop pass, before the renewal of that pass.
    std::vector<size_t> expiry_at_pass;
    std::vector<size_t> restore_at_pass;
    uint32_t refused_writes = 0;
    bool overran = false;
    String final_log;
};

constexpr uint32_t kExpiryLogMaxPasses = 8;

/// Runs the real worker loop until pass `stop_pass`, which is the loop pass before the renewal after
/// the one whose result the test wants to see.
void runExpiryLogScenario(const String & layout_prefix, ExpiryLogRig & rig, uint32_t stop_pass)
{
    const Layout layout(layout_prefix);
    const UInt128 uuid{1};
    uint64_t wall_ms = 1000;
    uint32_t loop_passes = 0;
    DB::Cas::tests::ManualBarrier stop;
    CasEventSink sink;
    auto backend = std::make_shared<ExpiryScriptBackend>();

    ASSERT_EQ(claimMount(*DB::Cas::tests::OperationForTest(backend), layout, "test", uuid, 1, wall_ms, kExpiryTtlMs).kind,
              MountClaimResult::Claimed);

    RuntimeUnderTest runtime_holder(
        backend, layout,
        MountConfig{
            .mount_lease_ttl_ms = std::chrono::milliseconds(kExpiryTtlMs),
            .background_watermark = true,
            .boot_ms_fn = [&] { return rig.boot_ms.load(); },
            .renewal_before_driver_lock_hook_for_test = [&]
            {
                ++loop_passes;
                if (loop_passes == 1)
                    rig.boot_ms.store(kExpiryFirstStartBootMs);
                const String text = rig.log.captured();
                rig.expiry_at_pass.push_back(countText(text, kExpiryWarningPrefix));
                rig.restore_at_pass.push_back(countText(text, kRestoreWarningText));
                if (rig.on_pass)
                    rig.on_pass(rig, loop_passes);
                if (loop_passes >= stop_pass || loop_passes >= kExpiryLogMaxPasses)
                {
                    rig.overran = loop_passes > stop_pass;
                    stop.arriveAndWait();
                }
            }},
        "test", sink, runtimeRenewBudget(), [] { return false; });
    CasMountRuntime & runtime = *runtime_holder;
    rig.runtime = &runtime;
    runtime.installRenewer(uuid, 1, [&] { return wall_ms; });
    const uint64_t anchor = runtime.startRenewer();
    runtime.armMountFence(uuid, 1, anchor + kExpiryTtlMs);
    rig.generation = runtime.fenceGeneration();

    backend->on_put = [&](uint32_t put_no)
    {
        rig.expiry_at_put.push_back(rig.expiryWarnings());
        rig.boot_at_put.push_back(rig.boot_ms.load());
        if (!rig.on_put || !rig.on_put(rig, put_no))
            return false;
        rig.boot_ms.fetch_add(kExpiryFailedPutMs);
        return true;
    };

    runtime.startBackgroundWorkers(std::chrono::milliseconds(kExpiryPeriodMs));
    stop.waitUntilArrived();
    rig.final_log = rig.log.captured();

    stop.release();
    runtime.stopBackgroundWorkers();
    runtime.finishTeardown(false);
}

}

/// The log carries one `WARNING` per expiry, written at the first request after it: not for an
/// outage the lease outlives, not per retry or per refused write, not when a renewal commits after its
/// own deadline, and again for a second expiry.
TEST(CASMountRuntime, ExpiryIsLoggedOncePerExpiry)
{
    {
        ExpiryLogRig rig;
        constexpr uint32_t kShortOutagePuts = 5;
        static_assert(kExpiryFirstStartBootMs + kShortOutagePuts * kExpiryFailedPutMs < kExpiryDeadlineBootMs);
        rig.on_put = [](ExpiryLogRig &, uint32_t put_no) { return put_no <= kShortOutagePuts; };
        ASSERT_NO_FATAL_FAILURE(runExpiryLogScenario("runtime-expiry-log-short", rig, 2));
        EXPECT_FALSE(rig.overran);
        EXPECT_EQ(rig.expiry_at_put.size(), kShortOutagePuts + 1u) << "the outage ran as scripted";
        EXPECT_EQ(countText(rig.final_log, kExpiryWarningPrefix), 0u) << rig.final_log;
        EXPECT_EQ(countText(rig.final_log, kRestoreWarningText), 0u) << rig.final_log;
    }

    ExpiryLogRig rig;
    uint64_t second_outage_floor_boot_ms = 0;
    rig.on_put = [&](ExpiryLogRig & r, uint32_t put_no)
    {
        if (put_no == kExpiryObservedPut)
        {
            for (int i = 0; i < 3; ++i)
            {
                try
                {
                    r.runtime->checkFenceOrThrow(r.generation);
                }
                catch (const DB::Exception &)
                {
                    ++r.refused_writes;
                }
            }
        }
        if (put_no <= kExpiryFailedPuts)
            return true;
        if (second_outage_floor_boot_ms == 0)
            return false;
        return r.boot_ms.load() <= second_outage_floor_boot_ms;
    };
    rig.on_pass = [&](ExpiryLogRig & r, uint32_t pass)
    {
        /// The restoring renewal has committed. The next one starts a period later and fails until
        /// the lease that was just restored has run out.
        if (pass == 3)
        {
            second_outage_floor_boot_ms = r.boot_ms.load() + kExpiryTtlMs;
            r.boot_ms.fetch_add(kExpiryPeriodMs);
        }
    };
    ASSERT_NO_FATAL_FAILURE(runExpiryLogScenario("runtime-expiry-log-twice", rig, 4));
    ASSERT_FALSE(rig.overran) << "the worker ran more passes than scripted";
    ASSERT_GT(rig.expiry_at_put.size(), kExpiryFailedPuts + 1u);

    /// Outage past the deadline: a request that sees the lease expired has already written the line.
    for (uint32_t put_no = 1; put_no <= kExpiryFailedPuts + 1; ++put_no)
    {
        const size_t expected = rig.boot_at_put[put_no - 1] >= kExpiryDeadlineBootMs ? 1 : 0;
        EXPECT_EQ(rig.expiry_at_put[put_no - 1], expected) << "at PUT " << put_no << " (boot " << rig.boot_at_put[put_no - 1] << ")";
    }
    EXPECT_EQ(rig.refused_writes, 3u);

    /// Pass 2 follows the renewal that committed after its own deadline: the lease is still expired.
    ASSERT_GE(rig.expiry_at_pass.size(), 4u);
    EXPECT_EQ(rig.expiry_at_pass[1], 1u) << "a renewal that commits past its own deadline does not warn again";
    EXPECT_EQ(rig.restore_at_pass[1], 0u);
    /// Pass 3 follows the restoring renewal.
    EXPECT_EQ(rig.expiry_at_pass[2], 1u);
    EXPECT_EQ(rig.restore_at_pass[2], 1u);
    /// Pass 4 follows the renewal that restored a second expiry.
    EXPECT_EQ(rig.expiry_at_pass[3], 2u) << "a lease that expires again warns again";
    EXPECT_EQ(rig.restore_at_pass[3], 2u);

    EXPECT_EQ(countText(rig.final_log, kExpiryWarningPrefix), 2u) << rig.final_log;

    /// The first line names the server root, the injected failure and a positive elapsed time.
    const size_t first = rig.final_log.find(kExpiryWarningPrefix);
    ASSERT_NE(first, String::npos) << rig.final_log;
    const String line = rig.final_log.substr(first, rig.final_log.find('\n', first) - first);
    EXPECT_NE(line.find("injected renewal timeout"), String::npos) << line;
    const uint64_t elapsed_ms = std::stoull(line.substr(kExpiryWarningPrefix.size()));
    EXPECT_GT(elapsed_ms, 0u) << line;
    EXPECT_LE(elapsed_ms, kExpiryFailedPutMs) << "written at the first request after the expiry: " << line;
}

/// The failure text explains the current run of trouble only. A renewal that fails once and then commits
/// with a deadline in the future ends that run; a later expiry in which no request fails shows no text.
TEST(CASMountRuntime, ASuccessEndsTheFailureTextOfItsRun)
{
    /// Everything the hooks capture is declared before the backend and the runtime that store them.
    const Layout layout("runtime-expiry-failure-text");
    const UInt128 uuid{1};
    uint64_t wall_ms = 1000;
    std::atomic<uint64_t> boot_ms{kExpiryClaimBootMs};
    uint32_t loop_passes = 0;
    DB::Cas::tests::ManualBarrier third_pass;
    CasMountRuntime * runtime_ptr = nullptr;
    String failure_after_success = "unobserved";
    std::optional<uint64_t> expired_since_after_stall;
    String failure_after_stall = "unobserved";
    /// The second renewal's `PUT` lands, but only after the lease it was meant to extend ran out.
    constexpr uint64_t second_start_boot_ms = kExpiryFirstStartBootMs + 15'000;
    constexpr uint64_t stall_ms = 2 * kExpiryTtlMs;
    CasEventSink sink;
    auto backend = std::make_shared<ExpiryScriptBackend>();

    ASSERT_EQ(claimMount(*DB::Cas::tests::OperationForTest(backend), layout, "test", uuid, 1, wall_ms, kExpiryTtlMs).kind,
              MountClaimResult::Claimed);

    RuntimeUnderTest runtime_holder(
        backend, layout,
        MountConfig{
            .mount_lease_ttl_ms = std::chrono::milliseconds(kExpiryTtlMs),
            .background_watermark = true,
            .boot_ms_fn = [&] { return boot_ms.load(); },
            .renewal_before_driver_lock_hook_for_test = [&]
            {
                ++loop_passes;
                if (loop_passes == 1)
                {
                    boot_ms.store(kExpiryFirstStartBootMs);
                }
                else if (loop_passes == 2)
                {
                    failure_after_success = runtime_ptr->lastRenewFailure();
                    boot_ms.store(second_start_boot_ms);
                }
                else if (loop_passes == 3)
                {
                    expired_since_after_stall = runtime_ptr->leaseExpiredSinceBootMs();
                    failure_after_stall = runtime_ptr->lastRenewFailure();
                    third_pass.arriveAndWait();
                }
            }},
        "test", sink, runtimeRenewBudget(), [] { return false; });
    CasMountRuntime & runtime = *runtime_holder;
    runtime_ptr = &runtime;
    runtime.installRenewer(uuid, 1, [&] { return wall_ms; });
    const uint64_t anchor = runtime.startRenewer();
    runtime.armMountFence(uuid, 1, anchor + kExpiryTtlMs);

    backend->on_put = [&](uint32_t put_no)
    {
        if (put_no == 1)
        {
            boot_ms.fetch_add(kExpiryFailedPutMs);
            return true;
        }
        if (put_no == 3)
            boot_ms.fetch_add(stall_ms);
        return false;
    };

    runtime.startBackgroundWorkers(std::chrono::milliseconds(kExpiryPeriodMs));
    third_pass.waitUntilArrived();
    third_pass.release();
    runtime.stopBackgroundWorkers();
    runtime.finishTeardown(false);

    EXPECT_TRUE(failure_after_success.empty())
        << "a commit with a deadline in the future ends the run of trouble: " << failure_after_success;
    ASSERT_TRUE(expired_since_after_stall.has_value()) << "the stalled renewal committed past its own start + TTL";
    EXPECT_TRUE(failure_after_stall.empty())
        << "no request of this expiry failed, so lifecycle_detail is empty: " << failure_after_stall;
}

/// Two expiries in a row, each ended by a restoring renewal, count 2: a stale success counts nothing,
/// and writes refused while the lease is expired do not count either.
TEST(CASMountRuntime, EachRestoredExpiryIsCountedOnce)
{
    const Layout layout("runtime-expiry-twice");
    const UInt128 uuid{1};
    uint64_t wall_ms = 1000;
    std::atomic<uint64_t> boot_ms{kExpiryClaimBootMs};
    uint32_t loop_passes = 0;
    uint64_t generation = 0;
    DB::Cas::tests::ManualBarrier last_pass;
    CasMountRuntime * runtime_ptr = nullptr;
    const uint64_t expired_before = eventCount(ProfileEvents::CASMountLeaseExpired);
    std::vector<uint64_t> counted_at_pass;
    std::vector<uint32_t> refused_while_expired;
    std::vector<uint64_t> counted_after_refusals;
    CasEventSink sink;
    auto backend = std::make_shared<ExpiryScriptBackend>();

    ASSERT_EQ(claimMount(*DB::Cas::tests::OperationForTest(backend), layout, "test", uuid, 1, wall_ms, kExpiryTtlMs).kind,
              MountClaimResult::Claimed);

    RuntimeUnderTest runtime_holder(
        backend, layout,
        MountConfig{
            .mount_lease_ttl_ms = std::chrono::milliseconds(kExpiryTtlMs),
            .background_watermark = true,
            .boot_ms_fn = [&] { return boot_ms.load(); },
            .renewal_before_driver_lock_hook_for_test = [&]
            {
                ++loop_passes;
                if (loop_passes == 1)
                {
                    boot_ms.store(kExpiryFirstStartBootMs);
                    return;
                }
                counted_at_pass.push_back(eventCount(ProfileEvents::CASMountLeaseExpired) - expired_before);
                if (loop_passes == 3)
                    boot_ms.fetch_add(kExpiryPeriodMs);
                else if (loop_passes == 5)
                    last_pass.arriveAndWait();
            }},
        "test", sink, runtimeRenewBudget(), [] { return false; });
    CasMountRuntime & runtime = *runtime_holder;
    runtime_ptr = &runtime;
    runtime.installRenewer(uuid, 1, [&] { return wall_ms; });
    const uint64_t anchor = runtime.startRenewer();
    runtime.armMountFence(uuid, 1, anchor + kExpiryTtlMs);
    generation = runtime.fenceGeneration();

    /// Renewal 1 fails 24 times and then lands stale (put 25); renewal 2 restores (put 26). Renewal 3
    /// does the same over puts 27 to 52. Puts `kExpiryObservedPut` and 48 are sent while the lease is expired.
    const auto refuse_while_expired = [&]
    {
        uint32_t refused = 0;
        for (int i = 0; i < 3; ++i)
        {
            try
            {
                runtime_ptr->checkFenceOrThrow(generation);
            }
            catch (const DB::Exception &)
            {
                ++refused;
            }
        }
        refused_while_expired.push_back(refused);
        counted_after_refusals.push_back(eventCount(ProfileEvents::CASMountLeaseExpired) - expired_before);
    };
    backend->on_put = [&](uint32_t put_no)
    {
        if (put_no == kExpiryObservedPut || put_no == 48)
            refuse_while_expired();
        const bool failing = put_no <= kExpiryFailedPuts
            || (put_no > kExpiryFailedPuts + 2 && put_no <= 2 * kExpiryFailedPuts + 2);
        if (failing)
            boot_ms.fetch_add(kExpiryFailedPutMs);
        return failing;
    };

    runtime.startBackgroundWorkers(std::chrono::milliseconds(kExpiryPeriodMs));
    last_pass.waitUntilArrived();
    last_pass.release();
    runtime.stopBackgroundWorkers();
    runtime.finishTeardown(false);

    /// Passes 2 to 5: after the first stale success, the first restore, the second stale success and the
    /// second restore.
    EXPECT_EQ(counted_at_pass, (std::vector<uint64_t>{0, 1, 1, 2}));
    EXPECT_EQ(refused_while_expired, (std::vector<uint32_t>{3, 3})) << "the lease was expired for both batches of refusals";
    EXPECT_EQ(counted_after_refusals, (std::vector<uint64_t>{0, 1})) << "a refused write is not a restore";
}

/// An expiry that ends because GC fenced the mount out is a loss, not a restore: nothing is counted.
TEST(CASMountRuntime, AnExpiryEndedByAFenceIsNotCountedAsARestore)
{
    const Layout layout("runtime-expiry-fenced");
    const UInt128 uuid{1};
    uint64_t wall_ms = 1000;
    std::atomic<uint64_t> boot_ms{kExpiryClaimBootMs};
    uint32_t loop_passes = 0;
    DB::Cas::tests::ManualBarrier remount_entered;
    const uint64_t expired_before = eventCount(ProfileEvents::CASMountLeaseExpired);
    CasEventSink sink;
    auto backend = std::make_shared<ExpiryScriptBackend>();

    ASSERT_EQ(claimMount(*DB::Cas::tests::OperationForTest(backend), layout, "test", uuid, 1, wall_ms, kExpiryTtlMs).kind,
              MountClaimResult::Claimed);

    RuntimeUnderTest runtime_holder(
        backend, layout,
        MountConfig{
            .mount_lease_ttl_ms = std::chrono::milliseconds(kExpiryTtlMs),
            .background_watermark = true,
            .boot_ms_fn = [&] { return boot_ms.load(); },
            .renewal_before_driver_lock_hook_for_test = [&]
            {
                ++loop_passes;
                if (loop_passes == 1)
                    boot_ms.store(kExpiryFirstStartBootMs);
                else if (loop_passes == 2)
                    fenceOutMount(*backend, layout.mountKey("test"));
            }},
        "test", sink, runtimeRenewBudget(), [&]
        {
            remount_entered.arriveAndWait();
            return false;
        });
    CasMountRuntime & runtime = *runtime_holder;
    runtime.installRenewer(uuid, 1, [&] { return wall_ms; });
    const uint64_t anchor = runtime.startRenewer();
    runtime.armMountFence(uuid, 1, anchor + kExpiryTtlMs);
    /// The first renewal lands stale, so the lease is still expired when GC fences the slot out; the
    /// next renewal meets the fence.
    backend->on_put = [&](uint32_t put_no)
    {
        const bool failing = put_no <= kExpiryFailedPuts;
        if (failing)
            boot_ms.fetch_add(kExpiryFailedPutMs);
        return failing;
    };

    runtime.startBackgroundWorkers(std::chrono::milliseconds(kExpiryPeriodMs));
    remount_entered.waitUntilArrived();

    EXPECT_EQ(runtime.lifecycle(), PoolLifecycle::TransientNotLive);
    EXPECT_FALSE(runtime.leaseExpiredSinceBootMs().has_value()) << "a fenced mount is not an expired one";
    EXPECT_EQ(eventCount(ProfileEvents::CASMountLeaseExpired) - expired_before, 0u);

    remount_entered.release();
    runtime.stopBackgroundWorkers();
    runtime.finishTeardown(false);
}

namespace
{
/// Clock values of the runtime readiness cases. With `runtimeRenewBudget` a ref append needs
/// 2 x 10 + 20 = 40 ms of lease. The claim starts at 100 s and the open decides with 30 ms left.
constexpr uint64_t kReadinessDecideBootMs = kExpiryDeadlineBootMs - 30;
/// The first readiness renewal fails 24 times, 1.3 s each, and commits with a start more than a TTL ago.
constexpr uint64_t kReadinessStaleCommitBootMs = kReadinessDecideBootMs + kExpiryFailedPuts * kExpiryFailedPutMs;
static_assert(kReadinessStaleCommitBootMs >= kReadinessDecideBootMs + kExpiryTtlMs);
/// The second commits with 35 ms of its lease left: in the future, but under the 40 ms a ref append needs.
constexpr uint64_t kReadinessShortCommitBootMs = kReadinessStaleCommitBootMs + kExpiryTtlMs - 35;
/// Far above any request count these tests reach, so only a regression hits it.
constexpr uint32_t kReadinessRequestBound = 100;

struct ReadinessView
{
    const char * admit = "";
    bool may_mutate = true;
    PoolLifecycle lifecycle = PoolLifecycle::IdentityLost;
    bool expired = true;
    String failure;
    uint64_t generation = 0;
    bool ref_append_ok = false;
};

ReadinessView readinessViewOf(CasMountRuntime & runtime)
{
    return ReadinessView{
        .admit = expiryAdmitName(runtime.admit(runtime.fenceGeneration(), 0)),
        .may_mutate = runtime.mayMutate(),
        .lifecycle = runtime.lifecycle(),
        .expired = runtime.leaseExpiredSinceBootMs().has_value(),
        .failure = runtime.lastRenewFailure(),
        .generation = runtime.fenceGeneration(),
        .ref_append_ok = runtime.refAppendFenceOk(),
    };
}
}

/// The open's closed state, at the runtime level: a claim with too little lease for a ref append leaves
/// the fence latched, and only a loop renewal whose own deadline leaves that room arms it. A stale success
/// and a success with too little room arm nothing, and each is followed at once by the next renewal.
TEST(CASMountRuntime, AReadinessRenewalArmsOnlyWithRoomForARefAppend)
{
    /// Everything the hooks capture is declared before the backend and the runtime that store them.
    const Layout layout("runtime-readiness-room");
    const UInt128 uuid{1};
    uint64_t wall_ms = 1000;
    std::atomic<uint64_t> boot_ms{kExpiryClaimBootMs};
    std::atomic<uint32_t> puts{0};
    std::atomic<uint32_t> remount_calls{0};
    std::atomic<bool> request_bound_hit{false};
    uint32_t loop_passes = 0;
    std::vector<uint64_t> admitted_at;
    ReadinessView during_first_put;
    ReadinessView after_stale;
    ReadinessView after_short;
    ReadinessView after_arm;
    DB::Cas::tests::ManualBarrier armed_pass;
    CasMountRuntime * runtime_ptr = nullptr;
    const uint64_t lost_before = eventCount(ProfileEvents::CASMountLeaseLost);
    const uint64_t expired_before = eventCount(ProfileEvents::CASMountLeaseExpired);
    CasEventSink sink;
    auto backend = std::make_shared<ExpiryScriptBackend>();

    ASSERT_EQ(claimMount(*DB::Cas::tests::OperationForTest(backend), layout, "test", uuid, 1, wall_ms, kExpiryTtlMs).kind,
              MountClaimResult::Claimed);

    RuntimeUnderTest runtime_holder(
        backend, layout,
        MountConfig{
            .mount_lease_ttl_ms = std::chrono::milliseconds(kExpiryTtlMs),
            .background_watermark = true,
            .boot_ms_fn = [&] { return boot_ms.load(); },
            .renewal_before_driver_lock_hook_for_test = [&]
            {
                ++loop_passes;
                if (loop_passes == 2)
                    after_stale = readinessViewOf(*runtime_ptr);
                else if (loop_passes == 3)
                    after_short = readinessViewOf(*runtime_ptr);
                else if (loop_passes == 4)
                {
                    after_arm = readinessViewOf(*runtime_ptr);
                    armed_pass.arriveAndWait();
                }
            },
            .renewal_admitted_hook_for_test = [&] { admitted_at.push_back(boot_ms.load()); },
            .renewal_live_for_test = [&]
            {
                if (puts.load() < kReadinessRequestBound)
                    return true;
                request_bound_hit = true;
                return false;
            }},
        "test", sink, runtimeRenewBudget(), [&]
        {
            ++remount_calls;
            return false;
        });
    CasMountRuntime & runtime = *runtime_holder;
    runtime_ptr = &runtime;
    runtime.installRenewer(uuid, 1, [&] { return wall_ms; });
    const uint64_t anchor = runtime.startRenewer();
    ASSERT_EQ(anchor, kExpiryClaimBootMs);

    /// Before the thread starts: the claim leaves 30 ms of its lease, a ref append needs 40.
    boot_ms = kReadinessDecideBootMs;
    const uint64_t generation_before = runtime.fenceGeneration();
    EXPECT_FALSE(runtime.armIfAdmissible(anchor + kExpiryTtlMs)) << "a claim without room for a ref append arms nothing";
    const uint64_t latched_generation = runtime.fenceGeneration();
    EXPECT_EQ(latched_generation, generation_before + 1) << "the latch ends the unarmed incarnation";
    EXPECT_STREQ(expiryAdmitName(runtime.admit(latched_generation, 0)), "LostOrRearmed");
    EXPECT_FALSE(runtime.mayMutate());
    EXPECT_EQ(runtime.lifecycle(), PoolLifecycle::Live) << "the open's closed state; nothing was lost";
    EXPECT_FALSE(runtime.leaseExpiredSinceBootMs().has_value()) << "a latched fence is not an expiry";
    EXPECT_EQ(eventCount(ProfileEvents::CASMountLeaseLost), lost_before) << "the latch counts no lease loss";

    backend->on_put = [&](uint32_t put_no)
    {
        ++puts;
        if (put_no == 1)
            during_first_put = readinessViewOf(*runtime_ptr);
        if (put_no <= kExpiryFailedPuts)
        {
            boot_ms.fetch_add(kExpiryFailedPutMs);
            return true;
        }
        /// Put 25 commits the first renewal, stale. Put 26, the second renewal, lands with 35 ms left.
        if (put_no == kExpiryFailedPuts + 2)
            boot_ms.store(kReadinessShortCommitBootMs);
        return false;
    };
    runtime.startBackgroundWorkers(std::chrono::milliseconds(kExpiryPeriodMs));

    bool arrived = true;
    try
    {
        armed_pass.waitUntilArrived();
    }
    catch (const DB::Exception &)
    {
        arrived = false;
    }
    if (!arrived)
    {
        runtime.stopBackgroundWorkers();
        runtime.finishTeardown(false);
        FAIL() << "no renewal armed the fence; request bound hit: " << request_bound_hit.load();
    }

    /// While the readiness renewal itself is sent.
    EXPECT_STREQ(during_first_put.admit, "LostOrRearmed") << "the latched fence refuses a write while the renewal is sent";
    EXPECT_FALSE(during_first_put.may_mutate);
    EXPECT_FALSE(during_first_put.expired);

    /// A stale success: the first renewal started at 129.97 s and committed at 161.17 s, past its own start + TTL.
    EXPECT_STREQ(after_stale.admit, "LostOrRearmed") << "a stale success arms nothing";
    EXPECT_EQ(after_stale.lifecycle, PoolLifecycle::Live);
    EXPECT_EQ(after_stale.generation, latched_generation);
    EXPECT_FALSE(after_stale.expired) << "a latched fence reports no expiry, also after a stale success";
    EXPECT_NE(after_stale.failure.find("injected renewal timeout"), String::npos)
        << "a stale success does not end the run of trouble: " << after_stale.failure;
    ASSERT_EQ(admitted_at.size(), 3u);
    EXPECT_EQ(admitted_at[0], kReadinessDecideBootMs) << "the first renewal is due at once";
    EXPECT_EQ(admitted_at[1], kReadinessStaleCommitBootMs) << "a stale success is followed at once";

    /// A short success: the second renewal started at 161.17 s and committed at 191.135 s with 35 ms left.
    EXPECT_STREQ(after_short.admit, "LostOrRearmed") << "a success with less lease than a ref append needs arms nothing";
    EXPECT_EQ(after_short.generation, latched_generation);
    EXPECT_TRUE(after_short.failure.empty()) << "a deadline in the future ends the run of trouble: " << after_short.failure;
    EXPECT_FALSE(after_short.expired);
    EXPECT_EQ(admitted_at[2], kReadinessShortCommitBootMs) << "a short success is followed at once";

    /// The third renewal started at 191.135 s with the whole TTL ahead: it arms, from its own start.
    EXPECT_STREQ(after_arm.admit, "Ok");
    EXPECT_TRUE(after_arm.ref_append_ok) << "the arm leaves room for a ref append";
    EXPECT_EQ(after_arm.lifecycle, PoolLifecycle::Live);
    EXPECT_EQ(after_arm.generation, latched_generation + 1) << "one arm, one new generation";
    EXPECT_FALSE(after_arm.expired);
    EXPECT_EQ(eventCount(ProfileEvents::CASMountLeaseLost), lost_before) << "readiness is not a lease loss";
    EXPECT_EQ(eventCount(ProfileEvents::CASMountLeaseExpired), expired_before) << "an arm is not a restore";
    EXPECT_EQ(puts.load(), kExpiryFailedPuts + 3);
    EXPECT_FALSE(request_bound_hit.load());
    EXPECT_EQ(remount_calls.load(), 0u);

    armed_pass.release();
    runtime.stopBackgroundWorkers();
    runtime.finishTeardown(false);
}

/// A trip that comes with no remount request, no intent and no stop does not end a loop renewal: its
/// requests keep going out under the latched fence until one commits.
TEST(CASMountRuntime, ARenewalIsSentUnderALatchedFence)
{
    const Layout layout("runtime-renew-under-latch");
    const UInt128 uuid{1};
    uint64_t wall_ms = 1000;
    std::atomic<uint64_t> boot_ms{kExpiryClaimBootMs};
    std::atomic<uint32_t> puts{0};
    std::atomic<uint32_t> remount_calls{0};
    std::atomic<bool> request_bound_hit{false};
    uint32_t loop_passes = 0;
    std::vector<String> admit_at_put;
    DB::Cas::tests::ManualBarrier third_put;
    DB::Cas::tests::ManualBarrier second_pass;
    CasMountRuntime * runtime_ptr = nullptr;
    CasEventSink sink;
    auto backend = std::make_shared<ExpiryScriptBackend>();

    ASSERT_EQ(claimMount(*DB::Cas::tests::OperationForTest(backend), layout, "test", uuid, 1, wall_ms, kExpiryTtlMs).kind,
              MountClaimResult::Claimed);

    RuntimeUnderTest runtime_holder(
        backend, layout,
        MountConfig{
            .mount_lease_ttl_ms = std::chrono::milliseconds(kExpiryTtlMs),
            .background_watermark = true,
            .boot_ms_fn = [&] { return boot_ms.load(); },
            .renewal_before_driver_lock_hook_for_test = [&]
            {
                ++loop_passes;
                if (loop_passes == 1)
                    boot_ms.store(kExpiryFirstStartBootMs);
                else if (loop_passes == 2)
                    second_pass.arriveAndWait();
            },
            .renewal_live_for_test = [&]
            {
                if (puts.load() < kReadinessRequestBound)
                    return true;
                request_bound_hit = true;
                return false;
            }},
        "test", sink, runtimeRenewBudget(), [&]
        {
            ++remount_calls;
            return false;
        });
    CasMountRuntime & runtime = *runtime_holder;
    runtime_ptr = &runtime;
    runtime.installRenewer(uuid, 1, [&] { return wall_ms; });
    const uint64_t anchor = runtime.startRenewer();
    runtime.armMountFence(uuid, 1, anchor + kExpiryTtlMs);

    /// Puts 1 to 4 fail, 1.3 s each; put 5 commits at 115.2 s, inside the lease of the 100 s claim.
    backend->on_put = [&](uint32_t put_no)
    {
        ++puts;
        admit_at_put.push_back(expiryAdmitName(runtime_ptr->admit(runtime_ptr->fenceGeneration(), 0)));
        if (put_no == 3)
            third_put.arriveAndWait();
        if (put_no <= 4)
        {
            boot_ms.fetch_add(kExpiryFailedPutMs);
            return true;
        }
        return false;
    };
    runtime.startBackgroundWorkers(std::chrono::milliseconds(kExpiryPeriodMs));

    third_put.waitUntilArrived();
    runtime.tripMountLost();
    third_put.release();
    bool arrived = true;
    try
    {
        second_pass.waitUntilArrived();
    }
    catch (const DB::Exception &)
    {
        arrived = false;
    }
    if (!arrived)
    {
        runtime.stopBackgroundWorkers();
        runtime.finishTeardown(false);
        FAIL() << "the renewal never finished; request bound hit: " << request_bound_hit.load();
    }

    EXPECT_EQ(puts.load(), 5u) << "the renewal kept sending after the trip until it committed";
    EXPECT_EQ(admit_at_put, (std::vector<String>{"Ok", "Ok", "Ok", "LostOrRearmed", "LostOrRearmed"}))
        << "puts 4 and 5 went out under the latched fence";
    EXPECT_EQ(runtime.remountRequestedGenerationForTest(), 0u) << "a lost fence alone requests no remount";
    EXPECT_EQ(remount_calls.load(), 0u);
    EXPECT_FALSE(request_bound_hit.load());

    second_pass.release();
    runtime.stopBackgroundWorkers();
    runtime.finishTeardown(false);
}

/// A FORGET intent published while the renewal retries ends it at its next admission check, with no trip
/// and no remount request; the loop exits.
TEST(CASMountRuntime, TheForgetIntentAloneEndsARenewal)
{
    const Layout layout("runtime-intent-alone");
    const UInt128 uuid{1};
    uint64_t wall_ms = 1000;
    std::atomic<uint64_t> boot_ms{kExpiryClaimBootMs};
    std::atomic<uint32_t> puts{0};
    std::atomic<uint32_t> remount_calls{0};
    std::atomic<bool> request_bound_hit{false};
    uint32_t loop_passes = 0;
    DB::Cas::tests::ManualBarrier third_put;
    WorkerExitLatch exits;
    RuntimeWorkerFactory factory = [&](std::function<void()> worker_body)
    {
        return ThreadFromGlobalPool([&, body = std::move(worker_body)]
        {
            body();
            exits.recordExit();
        });
    };
    const uint64_t lost_before = eventCount(ProfileEvents::CASMountLeaseLost);
    CasEventSink sink;
    auto backend = std::make_shared<ExpiryScriptBackend>();

    ASSERT_EQ(claimMount(*DB::Cas::tests::OperationForTest(backend), layout, "test", uuid, 1, wall_ms, kExpiryTtlMs).kind,
              MountClaimResult::Claimed);

    RuntimeUnderTest runtime_holder(
        backend, layout,
        MountConfig{
            .mount_lease_ttl_ms = std::chrono::milliseconds(kExpiryTtlMs),
            .background_watermark = true,
            .boot_ms_fn = [&] { return boot_ms.load(); },
            .worker_factory = factory,
            .renewal_before_driver_lock_hook_for_test = [&]
            {
                if (++loop_passes == 1)
                    boot_ms.store(kExpiryFirstStartBootMs);
            },
            .renewal_live_for_test = [&]
            {
                if (puts.load() < kReadinessRequestBound)
                    return true;
                request_bound_hit = true;
                return false;
            }},
        "test", sink, runtimeRenewBudget(), [&]
        {
            ++remount_calls;
            return false;
        });
    CasMountRuntime & runtime = *runtime_holder;
    runtime.installRenewer(uuid, 1, [&] { return wall_ms; });
    const uint64_t anchor = runtime.startRenewer();
    runtime.armMountFence(uuid, 1, anchor + kExpiryTtlMs);

    /// Every put fails; put 3 is held until the intent is published.
    backend->on_put = [&](uint32_t put_no)
    {
        ++puts;
        if (put_no == 3)
            third_put.arriveAndWait();
        boot_ms.fetch_add(kExpiryFailedPutMs);
        return true;
    };
    runtime.startBackgroundWorkers(std::chrono::milliseconds(kExpiryPeriodMs));

    third_put.waitUntilArrived();
    const uint64_t generation_at_intent = runtime.remountRequestedGenerationForTest();
    runtime.publishVanishedIntent();
    third_put.release();

    const bool exited = exits.waitForAtLeast(1);
    EXPECT_TRUE(exited) << "the loop exits on the published intent";
    EXPECT_EQ(puts.load(), 3u) << "no request after the intent";
    EXPECT_FALSE(request_bound_hit.load()) << "the intent, not the request bound, ended the renewal";
    EXPECT_EQ(runtime.remountRequestedGenerationForTest(), generation_at_intent);
    EXPECT_EQ(remount_calls.load(), 0u);
    EXPECT_FALSE(runtime.mayMutate()) << "the early end trips the fence";
    EXPECT_EQ(eventCount(ProfileEvents::CASMountLeaseLost), lost_before) << "a FORGET is not a lease loss";

    runtime.stopBackgroundWorkers();
    runtime.finishTeardown(false);
}

/// A trip with no request, no intent and no stop: the next renewal that commits with room for a ref
/// append arms the fence again under the same epoch and reports `Live`.
TEST(CASMountRuntime, ATripAloneIsRearmedByTheNextRenewal)
{
    const Layout layout("runtime-trip-alone");
    const UInt128 uuid{1};
    uint64_t wall_ms = 1000;
    std::atomic<uint64_t> boot_ms{kExpiryClaimBootMs};
    std::atomic<uint32_t> remount_calls{0};
    uint32_t loop_passes = 0;
    uint64_t generation_after_trip = 0;
    ReadinessView after_renewal;
    DB::Cas::tests::ManualBarrier second_pass;
    CasMountRuntime * runtime_ptr = nullptr;
    const uint64_t lost_before = eventCount(ProfileEvents::CASMountLeaseLost);
    CasEventSink sink;
    auto backend = std::make_shared<ExpiryScriptBackend>();

    ASSERT_EQ(claimMount(*DB::Cas::tests::OperationForTest(backend), layout, "test", uuid, 1, wall_ms, kExpiryTtlMs).kind,
              MountClaimResult::Claimed);

    RuntimeUnderTest runtime_holder(
        backend, layout,
        MountConfig{
            .mount_lease_ttl_ms = std::chrono::milliseconds(kExpiryTtlMs),
            .background_watermark = true,
            .boot_ms_fn = [&] { return boot_ms.load(); },
            .renewal_before_driver_lock_hook_for_test = [&]
            {
                ++loop_passes;
                if (loop_passes == 1)
                {
                    boot_ms.store(kExpiryFirstStartBootMs);
                    runtime_ptr->tripMountLost();
                    generation_after_trip = runtime_ptr->fenceGeneration();
                }
                else if (loop_passes == 2)
                {
                    after_renewal = readinessViewOf(*runtime_ptr);
                    second_pass.arriveAndWait();
                }
            }},
        "test", sink, runtimeRenewBudget(), [&]
        {
            ++remount_calls;
            return false;
        });
    CasMountRuntime & runtime = *runtime_holder;
    runtime_ptr = &runtime;
    runtime.installRenewer(uuid, 1, [&] { return wall_ms; });
    const uint64_t anchor = runtime.startRenewer();
    runtime.armMountFence(uuid, 1, anchor + kExpiryTtlMs);
    runtime.startBackgroundWorkers(std::chrono::milliseconds(kExpiryPeriodMs));

    second_pass.waitUntilArrived();
    EXPECT_STREQ(after_renewal.admit, "Ok") << "the renewal at 110 s re-armed the fence";
    EXPECT_EQ(after_renewal.lifecycle, PoolLifecycle::Live);
    EXPECT_EQ(after_renewal.generation, generation_after_trip + 1);
    EXPECT_EQ(runtime.liveWriterEpoch(), 0u) << "no reclaim ran: the epoch the test never published is unchanged";
    EXPECT_EQ(runtime.remountRequestedGenerationForTest(), 0u);
    EXPECT_EQ(remount_calls.load(), 0u);
    EXPECT_EQ(eventCount(ProfileEvents::CASMountLeaseLost), lost_before + 1) << "the trip counted its loss once";
    second_pass.release();
    runtime.stopBackgroundWorkers();
    runtime.finishTeardown(false);
}

/// A renewal that commits while a remount request is pending does not arm a latched fence, whatever room
/// its deadline leaves: the reclaim that follows starts from a fence that refuses writes. The request is
/// raised from the renewal's liveness check after its write landed, the last check before the commit is
/// consumed; a request raised earlier ends the renewal instead.
TEST(CASMountRuntime, ARenewalArmsNothingWhileARequestIsPending)
{
    const Layout layout("runtime-no-arm-while-pending");
    const UInt128 uuid{1};
    uint64_t wall_ms = 1000;
    uint64_t boot_ms = 100;
    String admit_at_reclaim = "unobserved";
    bool may_mutate_at_reclaim = true;
    PoolLifecycle lifecycle_at_reclaim = PoolLifecycle::Live;
    std::atomic<bool> renewal_landed{false};
    bool request_raised = false;
    DB::Cas::tests::ManualBarrier reclaim_entered;
    CasMountRuntime * runtime_ptr = nullptr;
    CasEventSink sink;
    auto backend = std::make_shared<RuntimeRenewBackend>();
    ASSERT_EQ(claimMount(*DB::Cas::tests::OperationForTest(backend), layout, "test", uuid, 1, wall_ms, 1000).kind,
              MountClaimResult::Claimed);
    RuntimeUnderTest runtime_holder(
        backend, layout,
        MountConfig{
            .mount_lease_ttl_ms = std::chrono::milliseconds(1000),
            .background_watermark = true,
            .boot_ms_fn = [&] { return boot_ms; },
            .renewal_live_for_test = [&]
            {
                if (renewal_landed.load() && !request_raised)
                {
                    request_raised = true;
                    runtime_ptr->tripMountLost();
                    runtime_ptr->scheduleRemount();
                }
                return true;
            }},
        "test", sink, runtimeRenewBudget(), [&]
        {
            admit_at_reclaim = expiryAdmitName(runtime_ptr->admit(runtime_ptr->fenceGeneration(), 0));
            may_mutate_at_reclaim = runtime_ptr->mayMutate();
            lifecycle_at_reclaim = runtime_ptr->lifecycle();
            reclaim_entered.arriveAndWait();
            return false;
        });
    CasMountRuntime & runtime = *runtime_holder;
    runtime_ptr = &runtime;
    runtime.installRenewer(uuid, 1, [&] { return wall_ms; });
    const uint64_t anchor = runtime.startRenewer();
    runtime.armMountFence(uuid, 1, anchor + 1000);
    const String key = layout.mountKey("test");
    const uint64_t writes_before = backend->putOverwriteCount(key);
    const uint64_t requests_before = runtime.scheduleRemountCallCountForTest();
    backend->after_commit = [&] { renewal_landed = true; };
    runtime.startBackgroundWorkers(std::chrono::milliseconds(0));

    reclaim_entered.waitUntilArrived();
    EXPECT_TRUE(request_raised);
    EXPECT_EQ(backend->putOverwriteCount(key), writes_before + 1) << "the renewal committed, with 1000 ms of lease ahead";
    EXPECT_EQ(runtime.scheduleRemountCallCountForTest(), requests_before + 1)
        << "the commit was consumed as one: a terminal loop renewal would have counted a request of its own";
    EXPECT_EQ(admit_at_reclaim, "LostOrRearmed") << "a commit consumed under a pending request arms nothing";
    EXPECT_FALSE(may_mutate_at_reclaim);
    EXPECT_EQ(lifecycle_at_reclaim, PoolLifecycle::TransientNotLive);
    reclaim_entered.release();
    runtime.stopBackgroundWorkers();
    runtime.finishTeardown(false);
}

namespace
{
/// Fails the conditional writes of the mount key that `on_mount_write` says to fail, with a timeout raised
/// before the store applied anything. The hook gets the 1-based number of the conditional write of the
/// mount key, counting every one: reclaims, adopts, renewals and a test's own fence-out.
class MountWriteScriptBackend final : public DB::Cas::InMemoryBackend
{
public:
    String mount_key;
    std::function<bool(uint32_t write_no)> on_mount_write;
    std::atomic<uint32_t> mount_writes{0};

    std::expected<String, RawConflict> write(const String & key, const String & bytes,
        const std::optional<String> & expected_value, DB::Cas::TransportAccess & access) override
    {
        if (key == mount_key && expected_value)
        {
            const uint32_t write_no = ++mount_writes;
            if (on_mount_write && on_mount_write(write_no))
                throw Poco::TimeoutException("injected readiness renewal timeout");
        }
        return InMemoryBackend::write(key, bytes, expected_value, access);
    }
};

const UInt128 kReadyUuid{0x42};
constexpr uint64_t kReadyClaimBootMs = 10'000;
/// The adopt write or the quiesce takes 20 s: the claim leaves 10 s of the default 30 s lease, and a ref
/// append needs 2 x 7 s + 2 s = 16 s with the default budget.
constexpr uint64_t kReadyClaimAgeMs = 20'000;
constexpr uint64_t kReadyPeriodMs = 10'000;

/// Pool meta, and a fenced, long-expired predecessor under epoch 7 with the epoch object that minted it.
/// An open as `kReadyUuid` reclaims it at once and writes the mount key exactly twice (the reclaim, then
/// the renewer's adopt) before it decides whether to arm.
void seedFencedPredecessor(DB::Cas::Backend & backend, const String & pool_prefix)
{
    DB::Cas::tests::seedPoolMetaForRestart(backend, pool_prefix);
    const Layout layout(pool_prefix);
    MountLease prior;
    prior.server_uuid = kReadyUuid;
    prior.writer_epoch = 7;
    prior.seq = 7;
    prior.expires_at_ms = 1;
    prior.gc_fenced = true;
    prior.write_attempt_id = UInt128{7};
    createObj(backend, layout.mountKey("s"), encodeMountLease(prior));
    createObj(backend, layout.epochKey("s"), encodeServerEpoch(ServerEpoch{.next_writer_epoch = 8}));
}

/// A writable pool with a lease thread, the default lease, period and budget, and a boot clock that only
/// the test and the injected sleeps move.
PoolConfig readinessPoolConfig(const String & pool_prefix, const std::shared_ptr<std::atomic<uint64_t>> & fake_boot)
{
    PoolConfig config;
    config.pool_prefix = pool_prefix;
    config.server_id = kReadyUuid;
    config.server_root_id = "s";
    config.background_watermark = true;
    config.boot_ms_fn = [fake_boot]
    {
        return fake_boot->load();
    };
    config.wait_sleep_fn = [fake_boot](uint64_t ms)
    {
        *fake_boot += ms;
    };
    config.retry_sleep_fn = [fake_boot](uint64_t ms)
    {
        *fake_boot += ms;
    };
    return config;
}

/// What a readiness scenario records on the lease thread. Heap-owned and captured by value: a `Pool` can
/// outlive the test frame.
struct ReadinessSeen
{
    std::thread::id opening_thread;
    std::atomic<bool> renewal_on_opening_thread{true};
    std::atomic<uint64_t> renewal_boot_ms{0};
    std::atomic<uint32_t> failed_renewals{0};
    std::atomic<bool> request_bound_hit{false};
    std::atomic<Pool *> pool{nullptr};
    std::atomic<int> lifecycle_at_renewal{-1};
    std::atomic<int> may_mutate_at_renewal{-1};
    std::atomic<int> lifecycle_at_arm{-1};
    std::atomic<int> may_mutate_at_arm{-1};
    std::atomic<int> lifecycle_after_arm{-1};
    std::atomic<int> may_mutate_after_arm{-1};
    /// Written on the lease thread before the sink's barrier, read by the test after it.
    String remount_outcome;
    String remount_step;
};
}

/// An open whose claim is too old returns only after the lease thread's
/// renewal armed the fence; a reclaim whose quiescence aged its claim the same way reports success with
/// the pool not `Live` and the fence latched, and the next renewal arms it. No sampled point sees the
/// fence armed while the pool is not `Live`.
TEST(CASMountRuntime, OpenAndRemountReportReadyOnlyWhenWritable)
{
    /// An open whose claim starts at 10 s and whose adopt write ends at 30 s.
    {
        auto fake_boot = std::make_shared<std::atomic<uint64_t>>(kReadyClaimBootMs);
        auto seen = std::make_shared<ReadinessSeen>();
        seen->opening_thread = std::this_thread::get_id();
        auto backend = std::make_shared<MountWriteScriptBackend>();
        seedFencedPredecessor(*backend, "ready-open");
        backend->mount_key = Layout("ready-open").mountKey("s");
        backend->on_mount_write = [fake_boot, seen](uint32_t write_no)
        {
            if (write_no == 2)
            {
                *fake_boot += kReadyClaimAgeMs;
            }
            else if (write_no == 3)
            {
                seen->renewal_boot_ms = fake_boot->load();
                seen->renewal_on_opening_thread = std::this_thread::get_id() == seen->opening_thread;
            }
            return false;
        };
        auto store = Pool::open(backend, readinessPoolConfig("ready-open", fake_boot));
        ASSERT_TRUE(store);
        EXPECT_EQ(backend->mount_writes.load(), 3u) << "the reclaim, the adopt and one renewal";
        EXPECT_FALSE(seen->renewal_on_opening_thread.load()) << "the lease thread renews, not the opening thread";
        EXPECT_EQ(seen->renewal_boot_ms.load(), kReadyClaimBootMs + kReadyClaimAgeMs) << "the renewal is sent with no cadence wait";
        EXPECT_EQ(store->lifecycle(), PoolLifecycle::Live);
        EXPECT_TRUE(store->mayMutate());
        /// The claim's deadline (40 s) would leave 10 s at 30 s; only the renewal's (60 s) admits the append.
        EXPECT_NO_THROW(publishPart(store, "srv/ready-open", "x", "payload")) << "a ref append is admitted when the open returns";
    }

    /// A fresh open arms at once. Then a reclaim: claim at 10 s, quiescence until 30 s.
    {
        auto fake_boot = std::make_shared<std::atomic<uint64_t>>(kReadyClaimBootMs);
        auto seen = std::make_shared<ReadinessSeen>();
        auto remounted = std::make_shared<DB::Cas::tests::ManualBarrier>();
        auto renewed_after_arm = std::make_shared<DB::Cas::tests::ManualBarrier>();
        auto backend = std::make_shared<MountWriteScriptBackend>();
        seedFencedPredecessor(*backend, "ready-remount");
        const String mount_key = Layout("ready-remount").mountKey("s");
        backend->mount_key = mount_key;
        /// 1, 2: the open's reclaim and adopt. 3: the test's fence-out. 4, 5: the remount's claim and adopt.
        /// 6: the renewal that arms. 7: the renewal after it.
        backend->on_mount_write = [fake_boot, seen, renewed_after_arm](uint32_t write_no)
        {
            if (write_no == 6)
            {
                Pool * pool = seen->pool.load();
                seen->renewal_boot_ms = fake_boot->load();
                seen->lifecycle_at_renewal = static_cast<int>(pool->lifecycle());
                seen->may_mutate_at_renewal = pool->mayMutate();
            }
            else if (write_no == 7)
            {
                Pool * pool = seen->pool.load();
                seen->lifecycle_after_arm = static_cast<int>(pool->lifecycle());
                seen->may_mutate_after_arm = pool->mayMutate();
                renewed_after_arm->arriveAndWait();
            }
            return false;
        };
        PoolConfig config = readinessPoolConfig("ready-remount", fake_boot);
        config.event_sink = [seen, remounted](const CasEvent & event)
        {
            if (event.type != CasEventType::MountRemount)
                return;
            seen->remount_outcome = event.outcome;
            seen->remount_step = event.detail.at("step");
            remounted->arriveAndWait();
        };
        config.remount_quiesce_hook_for_test = [fake_boot]
        {
            *fake_boot += kReadyClaimAgeMs;
        };
        auto store = Pool::open(backend, config);
        ASSERT_TRUE(store);
        ASSERT_EQ(backend->mount_writes.load(), 2u) << "a fresh claim arms at once";
        seen->pool = store.get();
        /// Runs inside the arm, after `Live` is reported and before the fence opens, so it sees the order of
        /// the two. It also makes the next renewal due at once, so its request shows the state the arm left.
        store->setArmMountFenceInterpositionHookForTest([seen, fake_boot]
        {
            Pool * pool = seen->pool.load();
            seen->lifecycle_at_arm = static_cast<int>(pool->lifecycle());
            seen->may_mutate_at_arm = pool->mayMutate();
            *fake_boot += kReadyPeriodMs;
        });
        const uint64_t succeeded_before = eventCount(ProfileEvents::CASRemountSucceeded);
        fenceOutMount(*backend, mount_key);
        ASSERT_TRUE(store->scheduleRemountForTest());

        remounted->waitUntilArrived();
        EXPECT_EQ(seen->remount_outcome, "ok");
        EXPECT_EQ(seen->remount_step, "claimed_not_armed");
        EXPECT_EQ(eventCount(ProfileEvents::CASRemountSucceeded), succeeded_before + 1);
        EXPECT_EQ(store->lifecycle(), PoolLifecycle::TransientNotLive) << "not Live after the reclaim";
        EXPECT_FALSE(store->mayMutate()) << "the fence stays latched";
        remounted->release();

        renewed_after_arm->waitUntilArrived();
        ASSERT_EQ(backend->mount_writes.load(), 7u);
        EXPECT_EQ(seen->renewal_boot_ms.load(), kReadyClaimBootMs + kReadyClaimAgeMs) << "the loop renewed at once";
        /// At every sampled point, an armed fence comes with `Live`.
        EXPECT_EQ(seen->lifecycle_at_renewal.load(), static_cast<int>(PoolLifecycle::TransientNotLive));
        EXPECT_EQ(seen->may_mutate_at_renewal.load(), 0) << "the renewal is sent under the latched fence";
        EXPECT_EQ(seen->lifecycle_at_arm.load(), static_cast<int>(PoolLifecycle::Live)) << "`Live` is reported first";
        EXPECT_EQ(seen->may_mutate_at_arm.load(), 0) << "inside the arm, after `Live`, the fence still refuses";
        EXPECT_EQ(seen->lifecycle_after_arm.load(), static_cast<int>(PoolLifecycle::Live));
        EXPECT_EQ(seen->may_mutate_after_arm.load(), 1) << "after the arm: `Live` and writable";
        renewed_after_arm->release();
    }
}

/// Every readiness renewal fails: the open waits one lease on the fence clock, then
/// stops and joins the lease thread and fails, naming the last failed request.
TEST(CASMountRuntime, AnOpenWhoseReadinessRenewalKeepsFailingFailsAfterOneTtl)
{
    /// Longer than the largest spacing draw, so a failed request is retried at once.
    constexpr uint64_t failed_request_ms = 1'300;
    /// Far above the 24 failures one lease takes; reached only if the open never gives up.
    constexpr uint32_t max_failed_renewals = 10'000;
    auto fake_boot = std::make_shared<std::atomic<uint64_t>>(kReadyClaimBootMs);
    auto seen = std::make_shared<ReadinessSeen>();
    auto exits = std::make_shared<WorkerExitLatch>();
    auto backend = std::make_shared<MountWriteScriptBackend>();
    seedFencedPredecessor(*backend, "ready-fail");
    backend->mount_key = Layout("ready-fail").mountKey("s");
    backend->on_mount_write = [fake_boot, seen](uint32_t write_no)
    {
        if (write_no == 2)
            *fake_boot += kReadyClaimAgeMs;
        if (write_no < 3)
            return false;
        if (seen->failed_renewals.load() >= max_failed_renewals)
        {
            seen->request_bound_hit = true;
            return false;
        }
        ++seen->failed_renewals;
        *fake_boot += failed_request_ms;
        return true;
    };
    PoolConfig config = readinessPoolConfig("ready-fail", fake_boot);
    config.worker_factory = [exits](std::function<void()> worker_body)
    {
        return ThreadFromGlobalPool([exits, body = std::move(worker_body)]
        {
            body();
            exits->recordExit();
        });
    };
    const uint64_t lost_before = eventCount(ProfileEvents::CASMountLeaseLost);
    const uint64_t expired_before = eventCount(ProfileEvents::CASMountLeaseExpired);

    int code = 0;
    String message;
    try
    {
        (void)Pool::open(backend, config);
        ADD_FAILURE() << "an open whose readiness renewal never succeeds must fail";
    }
    catch (const DB::Exception & e)
    {
        code = e.code();
        message = e.message();
    }

    EXPECT_EQ(code, DB::ErrorCodes::ABORTED) << message;
    EXPECT_NE(message.find("injected readiness renewal timeout"), String::npos) << "the failure names the last request: " << message;
    EXPECT_FALSE(seen->request_bound_hit.load()) << "the open never gave up";
    /// The wait starts at 30 s or later and ends once the fence clock passes one lease after its start:
    /// 23 failures reach 59.9 s, the 24th 61.2 s.
    EXPECT_GE(seen->failed_renewals.load(), 24u) << "the open waited one lease, not until a lease bound";
    EXPECT_EQ(exits->count(), 1u) << "the lease thread was joined before the open failed";
    EXPECT_EQ(eventCount(ProfileEvents::CASMountLeaseLost), lost_before) << "a failed open is not a lease loss";
    EXPECT_EQ(eventCount(ProfileEvents::CASMountLeaseExpired), expired_before);
}

/// A writable open with no lease thread whose claim does not admit a ref append fails at once
/// with a retryable error, and sends no renewal.
TEST(CASPool, AnOpenWithoutALeaseThreadFailsWhenItsClaimIsTooOld)
{
    auto fake_boot = std::make_shared<std::atomic<uint64_t>>(kReadyClaimBootMs);
    auto backend = std::make_shared<MountWriteScriptBackend>();
    seedFencedPredecessor(*backend, "ready-no-thread");
    backend->mount_key = Layout("ready-no-thread").mountKey("s");
    backend->on_mount_write = [fake_boot](uint32_t write_no)
    {
        if (write_no == 2)
            *fake_boot += kReadyClaimAgeMs;
        return false;
    };
    PoolConfig config = readinessPoolConfig("ready-no-thread", fake_boot);
    config.background_watermark = false;
    const uint64_t attempts_before = eventCount(ProfileEvents::CASMountRenewalAttempts);

    int code = 0;
    String message;
    try
    {
        (void)Pool::open(backend, config);
        ADD_FAILURE() << "an open with no lease thread and a claim too old must fail";
    }
    catch (const DB::Exception & e)
    {
        code = e.code();
        message = e.message();
    }

    EXPECT_EQ(code, DB::ErrorCodes::ABORTED) << message;
    EXPECT_NE(message.find("no lease thread"), String::npos) << message;
    EXPECT_EQ(eventCount(ProfileEvents::CASMountRenewalAttempts), attempts_before) << "nothing renewed the claim";
}

namespace
{
/// What the open's wait and the reclaim saw when the readiness renewal met a definitive answer.
struct DefinitiveReadinessSeen
{
    bool armed = false;
    uint32_t puts_at_first_reclaim = 0;
    String admit_at_first_reclaim = "unobserved";
    bool may_mutate_at_first_reclaim = true;
    uint32_t reclaims = 0;
    uint64_t boot_before_wait = 0;
    uint64_t boot_after_wait = 0;
    ReadinessView after;
    uint64_t live_writer_epoch = 0;
    uint64_t requested_generation = 0;
    uint64_t lease_lost = 0;
};

/// The open's shape at the runtime level, with a lease of 1000 ms: the claim starts at 100 ms and the open
/// decides at 1070 ms, with 30 ms left and 40 ms needed. The slot is GC-fenced before the loop starts, so the
/// readiness renewal gets a definitive answer. The reclaim either claims epoch 2 and arms, or fails each
/// attempt and moves the fence clock 600 ms.
void runDefinitiveAnswerDuringReadiness(bool reclaim_arms, DefinitiveReadinessSeen & seen)
{
    const Layout layout(reclaim_arms ? "runtime-readiness-definitive-reclaimed" : "runtime-readiness-definitive-failing");
    const UInt128 uuid{1};
    uint64_t wall_ms = 1000;
    std::atomic<uint64_t> boot_ms{100};
    std::atomic<uint32_t> puts{0};
    std::atomic<uint32_t> reclaims{0};
    CasMountRuntime * runtime_ptr = nullptr;
    const uint64_t lost_before = eventCount(ProfileEvents::CASMountLeaseLost);
    CasEventSink sink;
    auto backend = std::make_shared<ExpiryScriptBackend>();
    ASSERT_EQ(claimMount(*DB::Cas::tests::OperationForTest(backend), layout, "test", uuid, 1, wall_ms, 1000).kind,
              MountClaimResult::Claimed);

    RuntimeUnderTest runtime_holder(
        backend, layout,
        MountConfig{
            .mount_lease_ttl_ms = std::chrono::milliseconds(1000),
            .background_watermark = true,
            .boot_ms_fn = [&] { return boot_ms.load(); }},
        "test", sink, runtimeRenewBudget(), [&]
        {
            CasMountRuntime & reclaiming = *runtime_ptr;
            if (++reclaims == 1)
            {
                seen.puts_at_first_reclaim = puts.load();
                seen.admit_at_first_reclaim = expiryAdmitName(reclaiming.admit(reclaiming.fenceGeneration(), 0));
                seen.may_mutate_at_first_reclaim = reclaiming.mayMutate();
            }
            reclaiming.beginReclaim();
            if (!reclaim_arms)
            {
                boot_ms.fetch_add(600);
                return false;
            }
            if (claimMount(*DB::Cas::tests::OperationForTest(backend), layout, "test", uuid, 2, wall_ms, 1000).kind
                != MountClaimResult::Claimed)
                return false;
            reclaiming.installRenewer(uuid, 2, [&] { return wall_ms; });
            const uint64_t fresh_anchor = reclaiming.startRenewer();
            reclaiming.setLiveWriterEpoch(2);
            /// On a regression that does not arm, the clock passes the open's bound and the arm's
            /// notification ends the wait, so the test fails instead of hanging.
            if (!reclaiming.armIfAdmissible(fresh_anchor + 1000))
                boot_ms.fetch_add(2000);
            return true;
        });
    CasMountRuntime & runtime = *runtime_holder;
    runtime_ptr = &runtime;
    runtime.installRenewer(uuid, 1, [&] { return wall_ms; });
    const uint64_t anchor = runtime.startRenewer();
    boot_ms = 1070;
    ASSERT_FALSE(runtime.armIfAdmissible(anchor + 1000)) << "the claim leaves 30 ms, a ref append needs 40";
    fenceOutMount(*backend, layout.mountKey("test"));
    backend->on_put = [&](uint32_t put_no)
    {
        ++puts;
        /// A second renewal before any reclaim is the regression this test names. Fail it and move the
        /// clock past the open's bound: the failure notifies the wait, which then gives up.
        if (put_no >= 2 && reclaims.load() == 0)
        {
            boot_ms.fetch_add(2000);
            return true;
        }
        return false;
    };
    runtime.startBackgroundWorkers(std::chrono::milliseconds(300));

    seen.boot_before_wait = boot_ms.load();
    seen.armed = runtime.waitUntilArmed(1000);
    seen.boot_after_wait = boot_ms.load();

    /// The join orders every write of the lease thread before the reads below.
    runtime.stopBackgroundWorkers();
    seen.after = readinessViewOf(runtime);
    seen.reclaims = reclaims.load();
    seen.live_writer_epoch = runtime.liveWriterEpoch();
    seen.requested_generation = runtime.remountRequestedGenerationForTest();
    seen.lease_lost = eventCount(ProfileEvents::CASMountLeaseLost) - lost_before;
    runtime.finishTeardown(false);
}

/// Throws a `LOGICAL_ERROR` from the renewal's conditional write of the mount key once `failing` is set: the
/// renewal hands it to the loop, which leaves through its own error path.
class LeaseLoopFailureBackend final : public DB::Cas::tests::CountingBackend
{
public:
    std::atomic<bool> failing{false};
    std::atomic<uint32_t> failures{0};

    std::expected<String, RawConflict> write(const String & key, const String & bytes,
        const std::optional<String> & expected_value, DB::Cas::TransportAccess & access) override
    {
        if (failing.load() && expected_value && key.ends_with("/mount"))
        {
            /// Bounds a retry of the failure, should the engine ever retry one: the renewal then commits,
            /// the fence arms and the test fails on `armed` instead of hanging.
            if (++failures >= kReadinessRequestBound)
                failing = false;
            throw DB::Exception(DB::ErrorCodes::LOGICAL_ERROR, "injected lease-loop failure");
        }
        return DB::Cas::tests::CountingBackend::write(key, bytes, expected_value, access);
    }
};

struct LeaseThreadEndSeen
{
    uint32_t exits = 0;
    uint32_t failures = 0;
    bool armed = true;
    uint64_t boot_before_wait = 0;
    uint64_t boot_after_wait = 0;
    bool may_mutate = true;
};

/// The open's shape with a 1000 ms lease, as above; the readiness renewal's first request throws a
/// `LOGICAL_ERROR`. After the thread is gone, every read of the fence clock moves it 600 ms, so the wait's
/// bound is reached on that clock.
void runLeaseThreadEndsOnItsOwn(LeaseThreadEndSeen & seen)
{
    const Layout layout("runtime-readiness-thread-ended");
    const UInt128 uuid{1};
    uint64_t wall_ms = 1000;
    std::atomic<uint64_t> boot_ms{100};
    std::atomic<bool> thread_gone{false};
    WorkerExitLatch exits;
    RuntimeWorkerFactory factory = [&](std::function<void()> worker_body)
    {
        return ThreadFromGlobalPool([&, body = std::move(worker_body)]
        {
            body();
            exits.recordExit();
        });
    };
    CasEventSink sink;
    auto backend = std::make_shared<LeaseLoopFailureBackend>();
    ASSERT_EQ(claimMount(*DB::Cas::tests::OperationForTest(backend), layout, "test", uuid, 1, wall_ms, 1000).kind,
              MountClaimResult::Claimed);

    RuntimeUnderTest runtime_holder(
        backend, layout,
        MountConfig{
            .mount_lease_ttl_ms = std::chrono::milliseconds(1000),
            .background_watermark = true,
            .boot_ms_fn = [&] { return thread_gone.load() ? boot_ms.fetch_add(600) + 600 : boot_ms.load(); },
            .worker_factory = factory},
        "test", sink, runtimeRenewBudget(), [] { return false; });
    CasMountRuntime & runtime = *runtime_holder;
    runtime.installRenewer(uuid, 1, [&] { return wall_ms; });
    const uint64_t anchor = runtime.startRenewer();
    boot_ms = 1070;
    ASSERT_FALSE(runtime.armIfAdmissible(anchor + 1000));
    backend->failing = true;
    runtime.startBackgroundWorkers(std::chrono::milliseconds(300));

    ASSERT_TRUE(exits.waitForAtLeast(1)) << "the loop must leave through its own error path";
    thread_gone = true;
    seen.boot_before_wait = boot_ms.load();
    seen.armed = runtime.waitUntilArmed(1000);
    seen.boot_after_wait = boot_ms.load();
    seen.may_mutate = runtime.mayMutate();
    seen.exits = static_cast<uint32_t>(exits.count());
    seen.failures = backend->failures.load();
    runtime.stopBackgroundWorkers();
    runtime.finishTeardown(false);
}
}

/// The open's readiness renewal meets a definitive answer: the renewal is terminal and raises one remount
/// generation, the fence stays latched, and the loop reclaims before any further renewal. The open's wait
/// ends armed when the reclaim arms under the new epoch, and gives up after one lease on the fence clock when
/// every reclaim fails.
TEST(CASMountRuntime, ADefinitiveAnswerDuringReadinessIsServedByAReclaim)
{
    DefinitiveReadinessSeen reclaimed;
    ASSERT_NO_FATAL_FAILURE(runDefinitiveAnswerDuringReadiness(/*reclaim_arms=*/true, reclaimed));
    EXPECT_TRUE(reclaimed.armed) << "the reclaim armed the fence";
    EXPECT_EQ(reclaimed.puts_at_first_reclaim, 1u) << "the loop reclaimed before any further renewal";
    EXPECT_EQ(reclaimed.admit_at_first_reclaim, "LostOrRearmed") << "no write is admitted before the arm";
    EXPECT_FALSE(reclaimed.may_mutate_at_first_reclaim);
    EXPECT_EQ(reclaimed.reclaims, 1u);
    EXPECT_STREQ(reclaimed.after.admit, "Ok");
    EXPECT_TRUE(reclaimed.after.ref_append_ok);
    EXPECT_EQ(reclaimed.after.lifecycle, PoolLifecycle::Live);
    EXPECT_EQ(reclaimed.live_writer_epoch, 2u) << "armed under the new epoch";
    EXPECT_EQ(reclaimed.requested_generation, 1u) << "one definitive answer, one remount generation";
    EXPECT_EQ(reclaimed.lease_lost, 1u) << "the definitive answer counts one lease loss; the open's latch none";

    DefinitiveReadinessSeen failing;
    ASSERT_NO_FATAL_FAILURE(runDefinitiveAnswerDuringReadiness(/*reclaim_arms=*/false, failing));
    EXPECT_FALSE(failing.armed);
    EXPECT_GE(failing.boot_after_wait, failing.boot_before_wait + 1000) << "the wait gave up after one lease on the fence clock";
    EXPECT_EQ(failing.puts_at_first_reclaim, 1u);
    EXPECT_EQ(failing.admit_at_first_reclaim, "LostOrRearmed");
    EXPECT_GE(failing.reclaims, 1u);
    EXPECT_STREQ(failing.after.admit, "LostOrRearmed") << "nothing armed";
    EXPECT_FALSE(failing.after.may_mutate);
    EXPECT_EQ(failing.live_writer_epoch, 0u);
    EXPECT_EQ(failing.requested_generation, 1u);
    EXPECT_EQ(failing.lease_lost, 1u);
}

#ifndef DEBUG_OR_SANITIZER_BUILD
/// The lease thread leaves through its own error path while the open waits. The fence stays latched and the
/// wait returns false; nothing notifies it, so it ends at its bound of one lease on the fence clock.
TEST(CASMountRuntime, AnOpenFailsWhenTheLeaseThreadEndedOnItsOwn)
{
    LeaseThreadEndSeen seen;
    ASSERT_NO_FATAL_FAILURE(runLeaseThreadEndsOnItsOwn(seen));
    EXPECT_EQ(seen.exits, 1u);
    EXPECT_EQ(seen.failures, 1u) << "the first readiness request reached the loop's error path";
    EXPECT_FALSE(seen.armed);
    EXPECT_GE(seen.boot_after_wait, seen.boot_before_wait + 1000) << "the wait ended at its bound on the fence clock";
    EXPECT_FALSE(seen.may_mutate) << "the fence stays latched";
}
#else
/// A `LOGICAL_ERROR` aborts a debug or sanitizer build where it is constructed, so the loop's error path is
/// reached only in a release build. Here the scenario must end the process. The child is a re-executed
/// process, not a fork: a forked child inherits the global thread pool's count of idle workers but not
/// the workers, so the lease thread's job could stay queued and the join would hang.
TEST(CASMountRuntimeDeathTest, AnOpenFailsWhenTheLeaseThreadEndedOnItsOwn)
{
    GTEST_FLAG_SET(death_test_style, "threadsafe");
    EXPECT_DEATH(
        {
            LeaseThreadEndSeen seen;
            runLeaseThreadEndsOnItsOwn(seen);
            std::_Exit(0);
        },
        "injected lease-loop failure");
}
#endif
