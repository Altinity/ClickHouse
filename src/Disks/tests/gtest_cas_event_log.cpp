#include <gtest/gtest.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasPartWriteTxn.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Primitives/CasEvent.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Gc/CasGc.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasPool.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Backend/CasInMemoryBackend.h>
#include <Disks/tests/cas_test_helpers.h>
#include <Interpreters/ContentAddressedLog.h>
#include <DataTypes/DataTypeLowCardinality.h>
#include <Common/typeid_cast.h>
#include <Common/Exception.h>
#include <Poco/Exception.h>
#include <algorithm>
#include <atomic>
#include <mutex>
#include <utility>
#include <vector>
using namespace DB::Cas;
using DB::Cas::tests::idOf;
using DB::Cas::tests::u128Of;

namespace DB::ErrorCodes
{
extern const int BAD_ARGUMENTS;
extern const int NETWORK_ERROR;
}

namespace DB::Cas
{
void configureMountRenewObservability(
    const String * server_root_id, const CasEventSink * event_sink, bool deferred) noexcept;
void reportMountRenewCompletion(const MountRenewResult & result) noexcept;
}

namespace
{

class RenewalEventBackend final : public InMemoryBackend
{
public:
    bool throw_before_next_write = false;
    bool throw_nonretryable_next_write = false;
    bool vanish_on_next_write = false;
    /// Runs just before an armed fault throws. The engine draws its inter-attempt backoff randomly and
    /// admits the reissue against that drawn duration, so a test that needs the ambiguity refused
    /// rather than reissued has to move the injected clock here -- from inside the attempt, the only
    /// point between admission and the resolve read a test can reach.
    std::function<void()> before_throw;

    void armResolveProbe()
    {
        std::lock_guard lock(resolve_mutex);
        observe_next_read = true;
        resolve_started = false;
    }

    bool resolveStarted()
    {
        std::lock_guard lock(resolve_mutex);
        return resolve_started;
    }

    /// The engine settles an ambiguous write by reading the key back, so the observation belongs on the
    /// READ PRIMITIVE -- the resolve read never reaches the legacy `get`.
    std::optional<Raw> read(const String & key, DB::Cas::TransportAccess & access) override
    {
        {
            std::lock_guard lock(resolve_mutex);
            if (observe_next_read)
            {
                resolve_started = true;
                observe_next_read = false;
            }
        }
        return InMemoryBackend::read(key, access);
    }

    /// The faults sit on the WRITE PRIMITIVE, and only on a CONDITIONAL write: a lease renewal is a
    /// replace, so the pool's own create-if-absent writes must not consume a one-shot fault.
    std::expected<String, RawConflict> write(
        const String & key,
        const String & bytes,
        const std::optional<String> & expected_value,
        DB::Cas::TransportAccess & access) override
    {
        if (!expected_value)
            return InMemoryBackend::write(key, bytes, expected_value, access);
        if (std::exchange(vanish_on_next_write, false))
        {
            (void)InMemoryBackend::remove(key, *expected_value, access);
            return std::unexpected(RawConflict{});
        }
        if (std::exchange(throw_nonretryable_next_write, false))
            throw DB::Exception(DB::ErrorCodes::BAD_ARGUMENTS, "injected deterministic renewal rejection");
        if (std::exchange(throw_before_next_write, false))
        {
            if (before_throw)
                before_throw();
            throw Poco::TimeoutException("injected renewal timeout before commit");
        }
        return InMemoryBackend::write(key, bytes, expected_value, access);
    }

private:
    std::mutex resolve_mutex;
    bool observe_next_read = false;
    bool resolve_started = false;
};

CasRequestBudget renewalEventBudget()
{
    return CasRequestBudget{
        .attempt_timeout_ms = 10,
        .operation_deadline_ms = 500,
        .max_attempts = 2,
        .lease_safety_margin_ms = 20,
        .retry_initial_backoff_ms = 0,
        .retry_max_backoff_ms = 0,
    };
}

PoolPtr openRenewalEventPool(
    const std::shared_ptr<RenewalEventBackend> & backend,
    uint64_t & boot_ms,
    CasRequestBudget budget = renewalEventBudget(),
    String prefix = "renewal-events",
    String server_root_id = "test")
{
    return Pool::open(backend, PoolConfig{
        .pool_prefix = std::move(prefix),
        .server_root_id = std::move(server_root_id),
        .mount_lease_ttl_ms = std::chrono::milliseconds(1000),
        .cas_request_budget = budget,
        .boot_ms_fn = [&] { return boot_ms; },
    });
}

std::vector<CasEvent> watermarkRenewEvents(const std::vector<CasEvent> & events)
{
    std::vector<CasEvent> result;
    std::copy_if(events.begin(), events.end(), std::back_inserter(result), [](const CasEvent & event)
    {
        return event.type == CasEventType::WatermarkRenew;
    });
    return result;
}

}

/// Round-B opt §6: `reason` is templated rationale (a handful of distinct strings repeated across
/// every row), unlike `object_hash`/`token` which are genuinely per-row varied -- it belongs alongside
/// the log's other LowCardinality columns (event_type/object_kind/outcome), not as a full String.
TEST(CASContentAddressedLog, ReasonColumnIsLowCardinality)
{
    const auto columns = DB::ContentAddressedLogElement::getColumnsDescription();
    const auto & reason_col = columns.get("reason");
    EXPECT_TRUE(typeid_cast<const DB::DataTypeLowCardinality *>(reason_col.type.get()))
        << "reason column must be LowCardinality(String) (Round-B opt §6)";
}
TEST(CASEvent, ConstructAndCopyAndName)
{
    CasEvent e;
    e.type = CasEventType::BlobDelete;
    e.object_kind = CasEventObjectKind::Blob;
    e.object_hash = "abcd";
    e.token = "tok";
    e.round = 7; e.gen = 3;
    e.reason = "in-degree 0 after strip";
    e.detail["freed"] = "10";
    CasEvent c = e;
    EXPECT_EQ(c.type, CasEventType::BlobDelete);
    EXPECT_EQ(c.object_hash, "abcd");
    EXPECT_EQ(c.detail.at("freed"), "10");
    EXPECT_EQ(toString(CasEventType::BlobDelete), "blob_delete");
    EXPECT_EQ(toString(CasEventType::IndegZero), "indegree_zero");
    EXPECT_EQ(toString(CasEventType::GcRecheckVerdict), "gc_recheck_verdict");
    EXPECT_EQ(toString(CasEventObjectKind::Manifest), "manifest");
}

TEST(CASEvent, PoolEmitsToSink)
{
    auto b = std::make_shared<InMemoryBackend>();
    std::vector<CasEvent> seen;   /// declared BEFORE the Pool so it outlives the background syncer's emits (ASan 2026-07-09)
    auto s = Pool::open(b, PoolConfig{.pool_prefix = "p", .server_root_id = "test"});
    s->setEventSink([&](const CasEvent & e){ seen.push_back(e); });
    CasEvent e;
    e.type = CasEventType::BlobPut;
    e.object_hash = "h";
    s->emitEvent(std::move(e));
    ASSERT_EQ(seen.size(), 1u);
    EXPECT_EQ(seen[0].type, CasEventType::BlobPut);
    /// null sink => no-op (no crash, no row); a fresh event, not the one already moved above.
    s->setEventSink(nullptr);
    CasEvent e2;
    e2.type = CasEventType::BlobPut;
    s->emitEvent(std::move(e2));
    EXPECT_EQ(seen.size(), 1u);
}

TEST(CASEvent, FirstAttemptRenewalIsSilent)
{
    auto backend = std::make_shared<RenewalEventBackend>();
    uint64_t boot_ms = 100;
    std::vector<CasEvent> events;
    auto store = openRenewalEventPool(backend, boot_ms);
    store->setEventSink([&](CasEvent event) { events.push_back(std::move(event)); });

    EXPECT_NO_THROW(store->renewWatermarkOnce());
    EXPECT_TRUE(watermarkRenewEvents(events).empty());
}

TEST(CASEvent, WatermarkRenewEventsAreBoundedAndComplete)
{
    auto backend = std::make_shared<RenewalEventBackend>();
    uint64_t boot_ms = 100;
    std::vector<CasEvent> events;
    auto store = openRenewalEventPool(backend, boot_ms);
    store->setEventSink([&](CasEvent event) { events.push_back(std::move(event)); });

    backend->throw_before_next_write = true;
    EXPECT_NO_THROW(store->renewWatermarkOnce());

    const std::vector<CasEvent> renewals = watermarkRenewEvents(events);
    /// ONE event per logical renewal, whatever the physical attempts cost: the engine owns its own
    /// reissues, and the terminal event carries their count rather than announcing each one.
    ASSERT_EQ(renewals.size(), 1u);
    EXPECT_EQ(renewals[0].outcome, "recovered");
    EXPECT_EQ(renewals[0].detail.at("attempts_sent"), "2");
    EXPECT_EQ(renewals[0].detail.at("classification"), "committed_after_retry");
    EXPECT_EQ(renewals[0].detail.at("server_root_id"), "test");
    EXPECT_EQ(renewals[0].detail.at("writer_epoch"), std::to_string(store->writerEpoch()));
    EXPECT_EQ(renewals[0].detail.at("seq"), "2");
    EXPECT_FALSE(renewals[0].detail.at("write_attempt_id").empty());
    EXPECT_LT(renewals[0].detail.at("write_attempt_id").size(), 32u);
    /// Both attempts sent the same body, so the event names the id the lease actually landed with --
    /// a reissue that minted a fresh id would leave the two disagreeing.
    const MountLease landed = decodeMountLease(backend->get(store->layout().mountKey("test"))->bytes);
    EXPECT_EQ(renewals[0].detail.at("write_attempt_id"), u128ToHex(landed.write_attempt_id).substr(0, 12));

    for (const String & key : {
             "server_root_id",
             "writer_epoch",
             "seq",
             "write_attempt_id",
             "attempts_sent",
             "elapsed_ms",
             "remaining_confirmed_budget_ms",
             "classification"})
        EXPECT_TRUE(renewals[0].detail.contains(key)) << "missing detail key " << key;
}

/// An attempt that spends the lease it was admitted under must not START the resolving read. That read
/// is the only thing that can prove the ambiguous attempt landed, and issuing it past the lease-safe
/// bound would be a request made without the authority it was admitted under -- so the renewal reports
/// the deadline that refused it instead of resolving anything.
TEST(CASEvent, AnAmbiguityPastTheLeaseBoundNeverStartsTheResolvingRead)
{
    auto backend = std::make_shared<RenewalEventBackend>();
    uint64_t boot_ms = 100;
    std::vector<CasEvent> events;
    auto store = openRenewalEventPool(
        backend, boot_ms, renewalEventBudget(), "renewal-inflight-ambiguity");
    store->setEventSink([&](CasEvent event) { events.push_back(std::move(event)); });

    /// The lease was anchored at 100 with a 1000 ms TTL, so the fence expires at 1100 and holds a 20 ms
    /// safety margin. At 1081 only 19 ms remain, and admission refuses the resolve read.
    backend->before_throw = [&] { boot_ms = 1'081; };
    backend->throw_before_next_write = true;
    backend->armResolveProbe();
    EXPECT_THROW(store->renewWatermarkOnce(), DB::Exception);

    EXPECT_FALSE(backend->resolveStarted())
        << "an attempt that consumed the lease must not start the resolving read";
    const std::vector<CasEvent> renewals = watermarkRenewEvents(events);
    ASSERT_EQ(renewals.size(), 1u);
    EXPECT_EQ(renewals[0].outcome, "failed");
    EXPECT_EQ(renewals[0].detail.at("attempts_sent"), "1");
    EXPECT_EQ(renewals[0].detail.at("classification"), "external_lease_deadline");
}

/// Ten renewals nested through each other's conflict sinks, against an eight-slot observation stack.
/// The two calls beyond the stack get no rich event -- and must still report their own physical attempt
/// count, which rides the write result rather than the suppressed observation.
TEST(CASEvent, DeepReentrancyPreservesDeterministicPhysicalAttemptTruth)
{
    constexpr size_t depth = 10;
    constexpr size_t observation_stack_capacity = 8;
    std::array<std::shared_ptr<RenewalEventBackend>, depth> backends;
    std::array<std::unique_ptr<Layout>, depth> layouts;
    std::array<std::unique_ptr<CasRequests>, depth> planes;
    std::array<std::unique_ptr<MountLeaseKeeper>, depth> keepers;
    std::array<String, depth> server_root_ids;
    std::array<CasEventSink, depth> sinks;
    std::array<uint32_t, depth> renew_events{};
    uint64_t wall_ms = 100;
    uint64_t boot_ms = 100;
    std::optional<MountRenewResult> deepest_result;
    std::function<MountRenewResult(size_t)> renew_at;

    renew_at = [&](size_t index)
    {
        configureMountRenewObservability(&server_root_ids[index], &sinks[index], /*deferred=*/false);
        MountRenewResult result = keepers[index]->renew(MountRenewOperationEnvironment{});
        reportMountRenewCompletion(result);
        return result;
    };

    for (size_t index = 0; index < depth; ++index)
    {
        backends[index] = std::make_shared<RenewalEventBackend>();
        layouts[index] = std::make_unique<Layout>(fmt::format("deep-renewal-{}", index));
        server_root_ids[index] = fmt::format("deep-{}", index);
        sinks[index] = [&, index](CasEvent event)
        {
            if (event.type == CasEventType::WatermarkRenew)
                ++renew_events[index];
            if (event.type == CasEventType::MountConflict && index + 1 < depth)
            {
                MountRenewResult child_result = renew_at(index + 1);
                if (index + 2 == depth)
                    deepest_result = std::move(child_result);
            }
        };
        /// One open-fence plane per keeper, on the same injected clock the keeper anchors its lease
        /// against, and with a sleep that advances it: the deepest renewal reissues, and no unit test
        /// may serve the engine's jittered backoff for real.
        planes[index] = std::make_unique<CasRequests>(
            backends[index], Fence::open(), [&] { return boot_ms; }, [&](uint64_t ms) { boot_ms += ms; });
        keepers[index] = std::make_unique<MountLeaseKeeper>(
            *planes[index],
            *planes[index],
            *layouts[index],
            server_root_ids[index],
            UInt128(index + 1),
            7,
            std::chrono::milliseconds(1000),
            [&] { return wall_ms; },
            [] { return uint64_t{0}; },
            sinks[index],
            std::chrono::milliseconds(0),
            [&] { return boot_ms; });
        keepers[index]->start();

        if (index + 1 < depth)
        {
            const String key = layouts[index]->mountKey(server_root_ids[index]);
            auto observed = backends[index]->get(key);
            ASSERT_TRUE(observed.has_value());
            MountLease foreign = decodeMountLease(observed->bytes);
            foreign.server_uuid = UInt128(100 + index);
            ASSERT_EQ(
                backends[index]->putOverwrite(key, encodeMountLease(foreign), observed->token).outcome,
                PutOutcome::Done);
        }
    }
    /// The deepest slot is the only one nobody took, so its renewal can recover: the attempt is lost
    /// before its answer, the resolve read finds the precondition intact, and the reissue commits.
    backends.back()->throw_before_next_write = true;

    const MountRenewResult outer_result = renew_at(0);
    EXPECT_EQ(outer_result.outcome, MountRenewOutcome::Terminal);
    ASSERT_TRUE(deepest_result.has_value());
    EXPECT_EQ(deepest_result->outcome, MountRenewOutcome::Committed);
    EXPECT_EQ(deepest_result->attempts_sent, 2u)
        << "nesting beyond the rich-event stack must not erase physical attempt truth";

    for (size_t index = 0; index < depth; ++index)
        EXPECT_EQ(renew_events[index], index < observation_stack_capacity ? 1u : 0u)
            << "renewal " << index << " is " << (index < observation_stack_capacity ? "on" : "beyond")
            << " the observation stack";
}

TEST(CASEvent, WatermarkRenewSinkFailureCannotChangeOutcome)
{
    auto backend = std::make_shared<RenewalEventBackend>();
    uint64_t boot_ms = 100;
    auto store = openRenewalEventPool(backend, boot_ms);
    const String mount_key = store->layout().mountKey("test");
    const uint64_t seq_before = decodeMountLease(backend->get(mount_key)->bytes).seq;
    store->setEventSink([](const CasEvent & event)
    {
        if (event.type == CasEventType::WatermarkRenew)
            throw DB::Exception(DB::ErrorCodes::NETWORK_ERROR, "injected renewal event sink failure");
    });

    backend->throw_before_next_write = true;
    EXPECT_NO_THROW(store->renewWatermarkOnce());
    EXPECT_EQ(decodeMountLease(backend->get(mount_key)->bytes).seq, seq_before + 1);
    EXPECT_TRUE(store->mayMutate());
}

/// The two terminal endings a renewal reaches without ever settling its write: the store refusing it
/// outright, and the lease refusing to admit it. There is no attempt-count ending -- the engine bounds a
/// write by time, never by a number of tries -- and the deadline ending that DOES send an attempt first
/// is `AnAmbiguityPastTheLeaseBoundNeverStartsTheResolvingRead`.
TEST(CASEvent, TerminalRenewalDetailsPreservePhysicalTruthAndClassification)
{
    const auto one_failed_event = [](const std::vector<CasEvent> & events) -> std::optional<CasEvent>
    {
        const std::vector<CasEvent> renewals = watermarkRenewEvents(events);
        const auto failed = std::find_if(renewals.begin(), renewals.end(), [](const CasEvent & event)
        {
            return event.outcome == "failed";
        });
        if (failed == renewals.end())
            return std::nullopt;
        return *failed;
    };

    {
        auto backend = std::make_shared<RenewalEventBackend>();
        uint64_t boot_ms = 100;
        std::vector<CasEvent> events;
        auto store = openRenewalEventPool(backend, boot_ms, renewalEventBudget(), "renewal-deterministic-details");
        store->setEventSink([&](CasEvent event) { events.push_back(std::move(event)); });
        backend->throw_nonretryable_next_write = true;

        EXPECT_THROW(store->renewWatermarkOnce(), DB::Exception);
        const std::optional<CasEvent> failed = one_failed_event(events);
        ASSERT_TRUE(failed.has_value()) << "the store's refusal must reach the event log";
        /// A deterministic failure reaches the keeper as the exception the engine refuses to reissue,
        /// and an exception carries no attempt count -- so the classification is all this ending states.
        EXPECT_EQ(failed->detail.at("classification"), "deterministic_failure");
    }

    {
        auto backend = std::make_shared<RenewalEventBackend>();
        uint64_t boot_ms = 100;
        std::vector<CasEvent> events;
        auto store = openRenewalEventPool(backend, boot_ms, renewalEventBudget(), "renewal-deadline-details");
        store->setEventSink([&](CasEvent event) { events.push_back(std::move(event)); });
        /// The lease was anchored at 100 with a 1000 ms TTL and holds a 20 ms safety margin, so 1090
        /// leaves 10 ms of it and admission refuses the renewal before its first attempt.
        boot_ms = 1090;

        EXPECT_THROW(store->renewWatermarkOnce(), DB::Exception);
        const std::optional<CasEvent> failed = one_failed_event(events);
        ASSERT_TRUE(failed.has_value()) << "the refused admission must reach the event log";
        EXPECT_EQ(failed->detail.at("attempts_sent"), "0");
        EXPECT_EQ(failed->detail.at("classification"), "external_lease_deadline");
    }
}

TEST(CASEvent, ReentrantRenewalSinkPreservesOuterObservationIdentity)
{
    auto backend = std::make_shared<RenewalEventBackend>();
    uint64_t boot_ms = 100;
    std::vector<CasEvent> events;
    PoolPtr store = openRenewalEventPool(backend, boot_ms, renewalEventBudget(), "renewal-reentrant-sink");
    bool reentered = false;
    store->setEventSink([&](CasEvent event)
    {
        if (event.type != CasEventType::WatermarkRenew)
            return;
        events.push_back(event);
        if (event.outcome == "recovered" && !std::exchange(reentered, true))
            store->renewWatermarkOnce();
    });

    backend->throw_before_next_write = true;
    EXPECT_NO_THROW(store->renewWatermarkOnce());

    ASSERT_TRUE(reentered);
    /// The nested renewal commits on its first attempt, which is silent, so the outer recovery is the
    /// only event -- and it still names the outer renewal's own seq while the durable lease has already
    /// moved past it. An observation the nested call reused would report seq 3 here.
    ASSERT_EQ(events.size(), 1u);
    EXPECT_EQ(events[0].outcome, "recovered");
    EXPECT_EQ(events[0].detail.at("attempts_sent"), "2");
    EXPECT_EQ(events[0].detail.at("seq"), "2");
    EXPECT_EQ(decodeMountLease(backend->get(store->layout().mountKey("test"))->bytes).seq, 3u)
        << "the nested first-attempt success must run without replacing the outer observation";
}

TEST(CASEvent, PreCompletionConflictReentrancyPreservesOuterTerminalObservation)
{
    auto inner_backend = std::make_shared<RenewalEventBackend>();
    uint64_t inner_boot_ms = 100;
    auto inner = openRenewalEventPool(
        inner_backend, inner_boot_ms, renewalEventBudget(), "renewal-reentrant-inner", "inner");

    auto outer_backend = std::make_shared<RenewalEventBackend>();
    uint64_t outer_boot_ms = 100;
    auto outer = openRenewalEventPool(
        outer_backend, outer_boot_ms, renewalEventBudget(), "renewal-reentrant-outer", "outer");
    std::vector<CasEvent> outer_events;
    bool reentered = false;
    outer->setEventSink([&](CasEvent event)
    {
        outer_events.push_back(event);
        if (event.type == CasEventType::MountConflict && !std::exchange(reentered, true))
            inner->renewWatermarkOnce();
    });

    /// The inner renewal loses its first attempt's answer and recovers on a reissue, so it has its own
    /// identity and its own classification to report. Both must stay off the outer observation.
    inner_backend->throw_before_next_write = true;
    outer_backend->vanish_on_next_write = true;
    EXPECT_THROW(outer->renewWatermarkOnce(), DB::Exception);

    ASSERT_TRUE(reentered);
    const std::vector<CasEvent> renewals = watermarkRenewEvents(outer_events);
    ASSERT_EQ(renewals.size(), 1u);
    EXPECT_EQ(renewals[0].outcome, "failed");
    EXPECT_EQ(renewals[0].detail.at("server_root_id"), "outer");
    EXPECT_EQ(renewals[0].detail.at("classification"), "vanished");
}

/// Round-B opt §6: `emitEvent` takes the event BY VALUE (moved-through, not `const &`), so a
/// caller's local is genuinely moved-from -- not merely copied via a const reference -- by the time
/// the sink runs. Mirrors `makeCasEventSink`'s own move-out-of-the-by-value-event idiom (a small test
/// double stands in for the `ContentAddressedLogElement` it would normally build).
TEST(CASEvent, EmitEventMovesSourceIntoSink)
{
    auto b = std::make_shared<InMemoryBackend>();
    String captured_reason;
    std::map<String, String> captured_detail;
    auto s = Pool::open(b, PoolConfig{.pool_prefix = "p", .server_root_id = "test"});
    s->setEventSink([&](CasEvent ev)
    {
        captured_reason = std::move(ev.reason);
        captured_detail = std::move(ev.detail);
    });
    CasEvent e;
    e.type = CasEventType::BlobPut;
    e.reason = "sentinel-reason";
    e.detail["k"] = "v";
    s->emitEvent(std::move(e));
    EXPECT_EQ(captured_reason, "sentinel-reason");
    EXPECT_EQ(captured_detail.at("k"), "v");
    /// the source event must be MOVED-FROM after emit, not merely aliased/copied through -- reading
    /// `e` here is the whole point of the test, not an oversight.
    EXPECT_TRUE(e.reason.empty()); // NOLINT(bugprone-use-after-move, hicpp-invalid-access-moved)
    EXPECT_TRUE(e.detail.empty()); // NOLINT(bugprone-use-after-move, hicpp-invalid-access-moved)
}

namespace
{

/// A single-blob part: upload one blob, stage a one-entry manifest naming it, precommit + promote the
/// ref. Returns the blob's object_hash (lowercase hex) so the test can filter the captured rows by it.
String publishOneBlobPart(const PoolPtr & s, const String & ns, const String & ref, const String & payload)
{
    const RootNamespace nsr{ns};
    PartWriteInfo info;
    info.intended_ref = ns + "/" + ref;
    auto build = s->beginPartWrite(info);
    ManifestEntry e;
    e.path = "data.bin";
    e.placement = EntryPlacement::Blob;
    e.ref = DB::Cas::BlobRef{DB::Cas::BlobHashAlgo::CityHash128, DB::Cas::BlobDigest::fromU128(u128Of(payload))};

    e.blob_size = payload.size();
    const ManifestId id = build->stageManifest({e});
    build->precommitAdd(nsr, ref, id);
    build->putBlob(idOf(payload), BlobSource::fromString(payload));
    build->promote(nsr, ref, build->buildId(), id);
    /// Phase 3 (mixed-algo pools): every blob-content-hash event render is `blobIdOf(ref)`
    /// ("<algoName>:<hex>"), never a bare hex -- the prime directive that a digest never appears
    /// without its algo.
    return DB::Cas::blobIdOf(e.ref);
}

/// Whether the CURRENT retired list (any gc-shard) still holds an entry (ack-floor pipeline in flight).
bool anyRetiredPending(const PoolPtr & s)
{
    /// Condemned state rides the adopted fold seal's RunMarker::Condemned rows, not a
    /// separate retired list — reconstruct the in-flight set from the seal.
    return DB::Cas::tests::anyCondemnedInSeal(s->backend(), s->layout());
}

/// Drive regular GC to a fixpoint over the ACK-FLOOR round (renew the store's mount ack after each round;
/// stay alive while any work counter is nonzero OR an in-flight retired entry remains).
void runGcToFixpoint(const PoolPtr & s, Gc & gc, size_t max_rounds = 64)
{
    for (size_t r = 0; r < max_rounds; ++r)
    {
        const RoundReport rep = DB::Cas::tests::runRegularRoundReclaiming(gc);
        if (!rep.acquired_lease)
            continue;
        s->renewWatermarkOnce();
        const bool no_work = rep.candidates == 0 && rep.deleted == 0 && rep.absent == 0
            && rep.replaced == 0 && rep.spared == 0;
        if (no_work && !anyRetiredPending(s))
            break;
    }
}

bool hasType(const std::vector<CasEvent> & events, CasEventType t)
{
    for (const auto & e : events)
        if (e.type == t)
            return true;
    return false;
}

}

/// B170 Task 4 acceptance: drive a full publish -> drop -> GC-to-delete lifecycle through a capturing
/// sink and assert (a) the taxonomy of events is emitted, (b) EVERY event carries a non-empty reason,
/// (c) filtering by a deleted blob's object_hash reconstructs its edge/retire/delete chain in order.
TEST(CASEvent, LifecycleReconstructionFromRows)
{
    auto b = std::make_shared<InMemoryBackend>();
    /// Declared BEFORE the Pool so they OUTLIVE it: the Pool's background retired-view syncer can emit
    /// (e.g. a view-advance event) right up to the Pool's destructor, and a sink capturing locals that
    /// die first is a use-after-scope (found by ASan 2026-07-09; the production sink captures the Context
    /// shared_ptr by value and is immune).
    std::vector<CasEvent> events;
    std::mutex events_mutex;
    auto s = Pool::open(b, PoolConfig{.pool_prefix = "p", .server_root_id = "test"});

    s->setEventSink([&](const CasEvent & e)
    {
        std::lock_guard lock(events_mutex);
        events.push_back(e);
    });

    const RootNamespace ns{"srv1/tbl"};
    const String ref = "all_0_0_0";
    const String payload = "the-doomed-blob-payload";

    /// publish -> the blob's whole closure is born and a ref names it.
    const String blob_hash = publishOneBlobPart(s, ns.string(), ref, payload);

    /// drop the ref and advance the watermark so the now-unreferenced closure is collectable.
    s->dropRef(ns, ref);
    s->renewWatermarkOnce();

    /// GC reclaims the tree and the blob to a fixpoint.
    Gc gc(s, u128Of("gc-event-log"));
    runGcToFixpoint(s, gc);

    /// The blob must actually be gone (the delete fired).
    ASSERT_FALSE(b->head(s->layout().blobKey(BlobRef{BlobHashAlgo::CityHash128, BlobDigest::fromU128(u128Of(payload))})).exists)
        << "GC must have deleted the now-unreferenced blob";

    /// (a) the expected taxonomy was emitted across the lifecycle (manifest model: no standalone trees).
    EXPECT_TRUE(hasType(events, CasEventType::BlobPut));
    EXPECT_TRUE(hasType(events, CasEventType::RootAdd))
        << "a fold must have recorded the manifest owner's blob edge (+1)";
    EXPECT_TRUE(hasType(events, CasEventType::RefDrop));
    EXPECT_TRUE(hasType(events, CasEventType::IndegZero));
    EXPECT_TRUE(hasType(events, CasEventType::GcRetireObserve)
        || hasType(events, CasEventType::GcRetireDecision)
        || hasType(events, CasEventType::GcRecheckVerdict))
        << "a GC retire/recheck transition must be recorded";
    EXPECT_TRUE(hasType(events, CasEventType::BlobDelete) || hasType(events, CasEventType::ManifestDelete))
        << "the single content-delete site must emit a delete row";

    /// (b) completeness mandate: every emitted event has a non-empty reason (the human WHY).
    for (const auto & e : events)
        EXPECT_FALSE(e.reason.empty())
            << "event " << toString(e.type) << " (" << e.object_hash << ") has an empty reason";

    /// (c) lifecycle reconstruction: filtering by the deleted blob's object_hash yields, in time
    /// order, at least its in-degree-zero -> retire-observe -> delete chain — its whole story.
    std::vector<CasEventType> chain;
    for (const auto & e : events)
        if (e.object_hash == blob_hash)
            chain.push_back(e.type);

    ASSERT_FALSE(chain.empty()) << "no rows reference the deleted blob " << blob_hash;

    /// The decisive ordering: the blob's in-degree hit 0 BEFORE GC observed/condemned it, which was
    /// BEFORE it was deleted. Find the first index of each and assert the order.
    auto firstIndexOf = [&](CasEventType t) -> int
    {
        for (size_t i = 0; i < chain.size(); ++i)
            if (chain[i] == t)
                return static_cast<int>(i);
        return -1;
    };
    const int i_indeg = firstIndexOf(CasEventType::IndegZero);
    const int i_observe = firstIndexOf(CasEventType::GcRetireObserve);
    const int i_delete = firstIndexOf(CasEventType::BlobDelete);
    ASSERT_GE(i_indeg, 0) << "the blob's indegree_zero must be in its chain";
    ASSERT_GE(i_observe, 0) << "the blob's gc_retire_observe must be in its chain";
    ASSERT_GE(i_delete, 0) << "the blob's blob_delete must be in its chain";
    EXPECT_LT(i_indeg, i_observe) << "in-degree hit 0 before GC observed it";
    EXPECT_LT(i_observe, i_delete) << "GC observed it before deleting it";
}
