#include <gtest/gtest.h>

#include <base/scope_guard.h>

#include "config.h"

#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Backend/CasInMemoryBackend.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Formats/CasFormat.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Formats/CasLayout.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Formats/CasRefCkptFormat.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Formats/CasRefLogFormat.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Formats/CasTextFormat.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasPool.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasRefCkpt.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasRefProtocol.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasServerRoot.h>
#include <Disks/tests/cas_test_helpers.h>
#include <Common/Exception.h>
#include <Common/MemoryTracker.h>
#include <Common/ProfileEvents.h>

#include <Poco/Exception.h>

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <limits>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <thread>
#include <utility>
#include <vector>

/// Stage A task 6: recovery is `_ckpt` + an ARITHMETIC tail + a seal CAS-walk, and it installs nothing
/// without presenting the fence generation it was admitted under.
///
/// The one sentence this suite exists to defend: **recovery performs no stream `LIST`.** Everything the
/// old recovery knew about a table's durable stream came from one `LIST`, so a listing that silently
/// omitted a key produced a table missing an ACKED transaction and looked perfectly healthy. The
/// checkpoint now supplies the only base and finite frontier; arithmetic exact GETs decide recovery.
/// These list-liar fixtures are retained as sentinels: hiding or fabricating a listed key cannot affect
/// recovery because recovery sends zero stream LIST requests.
///
/// The other half is INV-2: a dead epoch is closed IN-BAND, by a seal transaction the store's own
/// conditional create places at exactly `{E, T+1}` -- the key a dying predecessor's in-flight PUT would
/// have taken. That is why the walk WRITES, and why every write it performs is gated on the ONE fence
/// generation captured when the recovery was admitted (slot-occupy, the `_ckpt` CAS, and the install
/// recheck -- one capture, three checks).
///
/// The suite name is prefixed `Cas` so it is covered by the `Cas*` unit-test gate filter.

namespace DB::ErrorCodes
{
extern const int CORRUPTED_DATA;
extern const int INVALID_STATE;
extern const int NETWORK_ERROR;
}

namespace ProfileEvents
{
extern const Event CASRefRecoveryRestarts;
extern const Event CASRefRecoveryEpochSealed;
extern const Event CASRefRecoveryEpochSealAdopted;
extern const Event CASRefRecoveryStragglerAdopted;
extern const Event CASRefRecoveryCancelled;
extern const Event CASRefCheckpointPublished;
extern const Event CASRefCheckpointStamped;
extern const Event CASRefAppendTombstoneWritten;
}

using namespace DB::Cas;
using DB::Cas::tests::VirtualRetryClock;
using DB::Cas::tests::committedRow;
using DB::Cas::tests::CountingBackend;
using DB::Cas::tests::expectThrowsCode;
using DB::Cas::tests::minimalLiveSnapshot;
using DB::Cas::tests::namespaceBirthOp;
using DB::Cas::tests::publishCommittedOps;
using DB::Cas::tests::rearmMountFenceAfterAnomalyForTest;
using DB::Cas::tests::tombstoneTxn;
using DB::Cas::tests::writeRefSnapshotRaw;

namespace
{

ManifestRef manifestRef(uint64_t epoch, uint64_t build_sequence, uint32_t ordinal)
{
    return ManifestRef{epoch, build_sequence, ordinal};
}

/// Fixture observations of durable state run on an OPEN fence: they are not writes a mount admitted,
/// and each owns the `CasRequests` its operation borrows, so none of these hands one back.
std::optional<CkptSample> readCkptForTest(const BackendPtr & backend, const Layout & layout,
                                          const NamespaceLifeId & life)
{
    CasRequests requests(backend, Fence::open(), {}, {}, nullptr, DB::Cas::kDefaultRequestWindowMs);
    CasOperation op = requests.admit();
    return readCkpt(op, layout, life);
}

CasRefCatalog::Snapshot readCatalogForTest(const BackendPtr & backend, const Layout & layout)
{
    CasRequests requests(backend, Fence::open(), {}, {}, nullptr, DB::Cas::kDefaultRequestWindowMs);
    CasOperation op = requests.admit();
    return CasRefCatalog::read(op, layout);
}

/// A fixture's own conditional replace, for the races these tests stage by hand.
bool replaceForTest(const BackendPtr & backend, const String & key, const String & bytes,
                    const Etag & expected)
{
    CasRequests requests(backend, Fence::open(), {}, {}, nullptr, DB::Cas::kDefaultRequestWindowMs);
    CasOperation op = requests.admit();
    return std::holds_alternative<Committed>(op.replace(key, bytes, expected, Retry::standard()));
}

/// Make the durable mount immediately reclaimable so a test that deliberately moved the local fence
/// generation can drive the production remount boundary without paying a live-lease expiry wait.
void fenceOutMountForRemount(Backend & backend, const String & mount_key)
{
    OperationForTest op(backend);
    const auto got = (*op).read(mount_key, Retry::once());
    ASSERT_TRUE(got.has_value());
    MountLease mount = decodeMountLease(got->bytes);
    mount.gc_fenced = true;
    mount.seq += 1;
    ASSERT_TRUE(std::holds_alternative<Committed>(
        (*op).replace(mount_key, encodeMountLease(mount), got->etag, Retry::once())));
}

/// The durable object at `key`, or `nullopt`.
std::optional<Object> readAt(Backend & backend, const String & key)
{
    OperationForTest op(backend);
    return (*op).read(key, Retry::once());
}

/// Unconditional create of a fresh key (the fixture's own setup, never a real conflict).
void createAt(Backend & backend, const String & key, const String & bytes)
{
    OperationForTest op(backend);
    EXPECT_TRUE(std::holds_alternative<Committed>((*op).create(key, bytes, Retry::once())));
}

/// A backend whose `LIST` can lie by omission. `hidden_keys` remain readable by exact key, so these
/// fixtures prove the stronger modern rule: recovery sends no stream `LIST` at all and therefore cannot
/// be affected by an enumeration inconsistency.
///
/// Deliberately NOT a "delete the object" fixture: an object that is genuinely gone is a different
/// (and already covered) case. The blocker is an object that EXISTS and is invisible to enumeration.
class HidingListBackend : public CountingBackend
{
public:
    explicit HidingListBackend(bool seed_pool_meta = true)
    {
        if (seed_pool_meta)
            DB::Cas::tests::seedPoolMetaForRestart(*this);
    }

    std::set<String> hidden_keys;
    std::set<String> phantom_list_keys;

    /// Every CREATING write of a key containing this substring throws `Poco::TimeoutException`, the
    /// class the request engine classifies as an unresolved transport fault and reissues under
    /// `Retry::standard()` -- a plain `std::exception` is instead the engine's signal for "this could
    /// not have landed" and propagates on the FIRST attempt (`CasRequests.cpp`'s
    /// `!dynamic_cast<const Poco::Exception *>(&e)` arms), which is a proven-not-landed verdict, not the
    /// ambiguity this fixture means to model. Persistent rather than one-shot on purpose: the subject is
    /// what recovery does when the store KEEPS refusing to say whether the write landed.
    String ambiguous_put_substr;

    /// Every attempt the `ambiguous_put_substr` fault intercepted, counted here because the throw below
    /// happens before delegating to `CountingBackend::write` -- its own per-key counters never see a
    /// faulted attempt at all. A test proves the engine actually reissued (rather than giving up after
    /// one attempt) by reading this after the call.
    std::atomic<uint64_t> ambiguous_put_attempts{0};

    /// Persistent thrown response for a matching CONDITIONAL replace of the mutable checkpoint. The
    /// ref-log create has already completed when tests arm this, producing the exact one-successor
    /// recovery window.
    String ambiguous_cas_substr;
    int ambiguous_cas_count = 0;

    /// Runs after a checkpoint publisher read the incarnation it expects but before that publisher
    /// presents its conditional write. This is the exact window in which another admitted writer can
    /// advance the frontier.
    std::function<void(const String &, const String &, const std::optional<String> &)> before_cas_put;

    DB::Cas::Backend::RawListPage list(const String & prefix, const String & cursor, size_t limit,
                                       DB::Cas::TransportAccess & access) override
    {
        DB::Cas::Backend::RawListPage page = CountingBackend::list(prefix, cursor, limit, access);
        std::vector<DB::Cas::Backend::RawListedKey> kept;
        kept.reserve(page.keys.size());
        for (DB::Cas::Backend::RawListedKey & lk : page.keys)
            if (!hidden_keys.contains(lk.key))
                kept.push_back(std::move(lk));
        if (cursor.empty())
        {
            for (const String & key : phantom_list_keys)
            {
                if (key.starts_with(prefix))
                    kept.push_back(DB::Cas::Backend::RawListedKey{.key = key, .size = 0, .value = std::nullopt});
            }
        }
        page.keys = std::move(kept);
        return page;
    }

    /// Both write faults hang off the ONE keyed primitive every caller now reaches the store through;
    /// which of them applies is decided by whether the write carries a precondition, which is exactly
    /// what used to separate `putIfAbsent` from `casPut`.
    std::expected<String, DB::Cas::Backend::RawConflict> write(
        const String & key, const String & bytes, const std::optional<String> & expected_value,
        DB::Cas::TransportAccess & access) override
    {
        if (!expected_value)
        {
            if (!ambiguous_put_substr.empty() && key.find(ambiguous_put_substr) != String::npos)
            {
                ambiguous_put_attempts.fetch_add(1);
                throw Poco::TimeoutException("injected ambiguous create");
            }
            return CountingBackend::write(key, bytes, expected_value, access);
        }
        if (before_cas_put)
            before_cas_put(key, bytes, expected_value);
        if (ambiguous_cas_count > 0 && !ambiguous_cas_substr.empty()
            && key.find(ambiguous_cas_substr) != String::npos)
        {
            --ambiguous_cas_count;
            throw Poco::TimeoutException("HidingListBackend: simulated ambiguous checkpoint replace");
        }
        return CountingBackend::write(key, bytes, expected_value, access);
    }
};

/// Fires `on_key` immediately AFTER a `putIfAbsent` whose key contains `watched_substr` -- the
/// deterministic way to act inside recovery's own write window (bump a fence, land a straggler) with no
/// sleep and no second thread. `skip` lets a test target the Nth such write.
class PutHookBackend : public HidingListBackend
{
public:
    String watched_substr;
    uint64_t skip = 0;
    std::function<void()> on_key;

    /// One override covers both write shapes: the `_ckpt` advance is a conditional replace, not a
    /// create, whenever the object already exists -- which is the normal case, since the namespace
    /// birth creates it -- so hooking only creates would silently never fire for it.
    std::expected<String, DB::Cas::Backend::RawConflict> write(
        const String & key, const String & bytes, const std::optional<String> & expected_value,
        DB::Cas::TransportAccess & access) override
    {
        auto result = HidingListBackend::write(key, bytes, expected_value, access);
        fireIfWatched(key);
        return result;
    }

private:
    void fireIfWatched(const String & key)
    {
        if (!on_key || watched_substr.empty() || key.find(watched_substr) == String::npos)
            return;
        if (skip > 0)
        {
            --skip;
            return;
        }
        auto hook = on_key;
        on_key = nullptr;   /// one-shot: a hook that re-enters its own trigger would recurse
        hook();
    }
};

/// Materializes `late_bytes` at `late_key` at the instant the walk READS that key and finds it absent --
/// i.e. strictly between the read and the conditional create that follows it.
///
/// This is the only faithful way to construct the race the `Occupied` arms exist for. Seeding the object
/// up front does NOT work, and finding that out is the point: the walk fetches every id by EXACT KEY, so
/// an object hidden from the listing is simply FOUND by the read and applied there. To meet it as an
/// OCCUPANT of the slot, it has to arrive after the read said absent -- which is exactly what a
/// straggler, or a concurrent recoverer's seal, does.
class LateMaterializeBackend : public HidingListBackend
{
public:
    String late_key;
    String late_bytes;

    std::optional<DB::Cas::Backend::Raw> read(const String & key, DB::Cas::TransportAccess & access) override
    {
        std::optional<DB::Cas::Backend::Raw> result = HidingListBackend::read(key, access);
        if (!result && !late_key.empty() && key == late_key)
        {
            (void)CountingBackend::write(late_key, late_bytes, std::nullopt, access);
            late_key.clear();   /// one-shot: the walk must see it present from here on
        }
        return result;
    }
};

/// Fires `on_key` immediately BEFORE a `get` whose key contains `watched_substr`, and can additionally
/// FAULT that read with a transient object-store error -- the I/O seam the remount-barrier test pauses
/// recovery at.
class GetSeamBackend : public HidingListBackend
{
public:
    String watched_substr;

    /// Assigned from the test thread and read from whatever thread the recovery runs on, so the
    /// read-and-move below is guarded. Today's tests all assign before starting the recovery thread and
    /// clear after joining it, so there is no race to fix -- but this is the same seam that already
    /// produced one use-after-free, and "the current tests happen not to race" is not a property a
    /// future test author can see. The mutex makes the constraint enforced rather than remembered.
    std::mutex hook_mutex;
    std::function<void(const String &)> on_key;

    std::optional<DB::Cas::Backend::Raw> read(const String & key, DB::Cas::TransportAccess & access) override
    {
        std::unique_lock hook_lock(hook_mutex);
        if (on_key && !watched_substr.empty() && key.find(watched_substr) != String::npos)
        {
            /// ONE-SHOT by moving the callback OUT before invoking it, and that is a correctness
            /// requirement rather than a convenience. A hook that cleared `on_key` from inside its own
            /// body would destroy the `std::function` whose closure it is still executing, and every
            /// by-reference capture it touched afterwards would read freed heap. That is not
            /// theoretical: it is what the first version of these tests did, and the ASan gate caught
            /// it as a `heap-use-after-free` while a hook was parked on a condition variable.
            auto hook = std::move(on_key);
            on_key = nullptr;
            /// Released before the hook runs: it parks on a condition variable, and holding the seam's
            /// own mutex across that would deadlock the very thread meant to release it.
            hook_lock.unlock();
            hook(key);
        }
        return HidingListBackend::read(key, access);
    }
};

/// Fires once after an exact GET has already fixed its result. This is the recovery authority seam:
/// another actor advances the log+checkpoint after the walk observed its old end, but before the walk
/// performs its final catalog/checkpoint validation.
class AfterGetHookBackend : public HidingListBackend
{
public:
    String watched_key;
    std::function<void()> after_get;

    std::optional<DB::Cas::Backend::Raw> read(const String & key, DB::Cas::TransportAccess & access) override
    {
        std::optional<DB::Cas::Backend::Raw> result = HidingListBackend::read(key, access);
        if (after_get && key == watched_key)
        {
            auto hook = std::move(after_get);
            after_get = nullptr;
            hook();
        }
        return result;
    }
};

/// Records `(method, key)` of every exact read, head, write and delete, in order. A write without a
/// precondition is `PUT`, one with a precondition is `CAS`.
class JournalingBackend : public HidingListBackend
{
public:
    std::vector<std::pair<String, String>> journal() const
    {
        std::lock_guard lock(journal_mutex);
        return entries;
    }

    void clearJournal()
    {
        std::lock_guard lock(journal_mutex);
        entries.clear();
    }

    std::optional<DB::Cas::Backend::Raw> read(const String & key, DB::Cas::TransportAccess & access) override
    {
        record("GET", key);
        return HidingListBackend::read(key, access);
    }

    std::optional<DB::Cas::Backend::RawMeta> head(const String & key, DB::Cas::TransportAccess & access) override
    {
        record("HEAD", key);
        return HidingListBackend::head(key, access);
    }

    std::expected<String, DB::Cas::Backend::RawConflict> write(
        const String & key, const String & bytes, const std::optional<String> & expected_value,
        DB::Cas::TransportAccess & access) override
    {
        record(expected_value ? "CAS" : "PUT", key);
        return HidingListBackend::write(key, bytes, expected_value, access);
    }

    DB::Cas::Backend::RawRemoval remove(const String & key, const String & expected_value,
                                        DB::Cas::TransportAccess & access) override
    {
        record("DELETE", key);
        return HidingListBackend::remove(key, expected_value, access);
    }

    void removeManyWriteOnce(const std::vector<DB::Cas::WriteOnceKey> & keys, DB::Cas::TransportAccess & access) override
    {
        for (const DB::Cas::WriteOnceKey & key : keys)
            record("DELETE", key.str());
        HidingListBackend::removeManyWriteOnce(keys, access);
    }

private:
    void record(const char * method, const String & key)
    {
        std::lock_guard lock(journal_mutex);
        entries.emplace_back(method, key);
    }

    mutable std::mutex journal_mutex;
    std::vector<std::pair<String, String>> entries;
};


/// More injected failures than the engine's own retry window can make attempts, on a clock that
/// advances at least a millisecond per pause: a call meeting this fault must end at its DEADLINE,
/// never by outliving the fault. A bounded count would be spent by ONE call's own reissues, because
/// the engine settles each ambiguity by an exact read and then reissues.
constexpr int kFaultsBeyondTheRetryWindow = 100'000;

CasRequestBudget tinyBudget()
{
    return CasRequestBudget{
        .attempt_timeout_ms = 50, .lease_safety_margin_ms = 50, .connect_timeout_cap_ms = std::nullopt};
}

PoolConfig walkTestConfig()
{
    PoolConfig config;
    config.pool_prefix = "p";
    config.server_root_id = "test";
    config.server_id = DB::UInt128(1);
    config.cas_request_budget = tinyBudget();
    config.wait_sleep_fn = [](uint64_t) {};
    /// No background publication: every test here drives its own, so a threshold-triggered snapshot can
    /// never move the base under an assertion about which base recovery chose.
    config.snapshot_log_count_threshold = 1ULL << 40;
    config.snapshot_log_bytes_threshold = 1ULL << 40;
    return config;
}

template <typename BackendT>
PoolPtr openWalkPool(const std::shared_ptr<BackendT> & backend, PoolConfig config = walkTestConfig())
{
    DB::Cas::tests::seedPoolMetaForRestart(*backend, config.pool_prefix);
    /// What the request engine reserves per attempt is the BACKEND's attempt timeout, not the budget
    /// field alone; pair the two so the fence math the recovery walk drives matches what admits.
    backend->setAttemptTimeoutMs(config.cas_request_budget.attempt_timeout_ms);
    return Pool::open(backend, std::move(config));
}

/// Burns durable writer epochs so a subsequent `Pool::open` allocates `target_live_epoch`. Epochs are
/// minted, never reclaimed (`CasPool.cpp`'s allocator), so this is exactly what a pool that has been
/// mounted `n` times looks like -- including the burned epochs in which nothing was ever written, which
/// the seal chain must cross.
void burnEpochsUpTo(const BackendPtr & backend, const Layout & layout, uint64_t target_live_epoch)
{
    CasRequests requests(backend, Fence::open(), {}, {}, nullptr, DB::Cas::kDefaultRequestWindowMs);
    CasOperation op = requests.admit();
    for (uint64_t e = 1; e < target_live_epoch; ++e)
        allocateWriterEpoch(op, layout, "test", [] { return RefCatalog{}; });
}

/// The clean farewell of `epoch` by this fixture's server, carrying `last_unclean_epoch`: the next claim
/// takes the marker over from it.
void seedCleanFarewell(Backend & backend, const Layout & layout, uint64_t epoch, uint64_t last_unclean_epoch)
{
    const PoolConfig config = walkTestConfig();
    const MountLease farewell{
        .server_uuid = config.server_id, .writer_epoch = epoch, .hostname = "h", .pid = 1, .started_at_ms = 1,
        .seq = 1, .expires_at_ms = 1, .min_active_build_sequence = std::numeric_limits<uint64_t>::max(),
        .gc_fenced = false, .write_attempt_id = UInt128(epoch), .last_unclean_epoch = last_unclean_epoch};
    createAt(backend, layout.mountKey(config.server_root_id), encodeMountLease(farewell));
}

/// The fenced, expired body of `epoch` by this fixture's server: an unclean end, so the next claim's marker
/// is `epoch`.
void seedFencedSlot(Backend & backend, const Layout & layout, uint64_t epoch)
{
    const PoolConfig config = walkTestConfig();
    const MountLease fenced{
        .server_uuid = config.server_id, .writer_epoch = epoch, .hostname = "h", .pid = 1, .started_at_ms = 1,
        .seq = 1, .expires_at_ms = 1, .min_active_build_sequence = 0, .gc_fenced = true,
        .write_attempt_id = UInt128(epoch), .last_unclean_epoch = epoch - 1};
    createAt(backend, layout.mountKey(config.server_root_id), encodeMountLease(fenced));
}

/// One ordinary transaction at `id`, publishing `ref` (prepending the birth op when `birth`).
RefLogTxn makeOrdinaryTxn(const RootNamespace & ns, RefTxnId id, const String & ref, bool birth,
                          std::optional<RefTxnId> prev_epoch_seal = std::nullopt)
{
    RefLogTxn txn;
    txn.ns = ns.string();
    txn.txn_id = id;
    if (birth)
        txn.ops.push_back(namespaceBirthOp());
    for (const RefOp & op : publishCommittedOps(ref, manifestRef(id.writer_epoch, id.ref_sequence, 1u)))
        txn.ops.push_back(op);
    txn.prev_epoch_seal = prev_epoch_seal;
    return txn;
}

/// The terminal `remove_namespace` op (this project's warning set requires every field named, so it is
/// built field-by-field rather than by designated init).
RefOp removeNamespaceOp()
{
    RefOp op;
    op.kind = RefOpKind::RemoveNamespace;
    return op;
}

/// One EPOCH SEAL transaction at `id` -- what a concurrent recoverer leaves behind.
RefLogTxn makeSealTxn(const RootNamespace & ns, RefTxnId id,
                      std::optional<RefTxnId> prev_epoch_seal = std::nullopt)
{
    RefLogTxn seal;
    seal.ns = ns.string();
    seal.txn_id = id;
    RefOp op;
    op.kind = RefOpKind::EpochSeal;
    seal.ops.push_back(op);
    seal.prev_epoch_seal = prev_epoch_seal;
    return seal;
}

void seedTxn(Backend & backend, const Layout & layout, const RootNamespace & ns, RefTxnId id,
             const String & ref, bool birth)
{
    DB::Cas::tests::fixture::writeRefLogRaw(backend, layout, makeOrdinaryTxn(ns, id, ref, birth));
}

/// Seeds the `_ckpt` a real namespace birth would have created, so recovery can ground its walk at the
/// namespace's `life_epoch` without consulting the (untrusted) listing. Raw, because these fixtures
/// never run a birth through the append lane.
void seedCkpt(Backend & backend, const Layout & layout, const RootNamespace & ns, const RefCkpt & ckpt, uint32_t generation = 1)
{
    createAt(backend, layout.refCkptKey(DB::Cas::tests::fixture::fixtureLife(ns)), encodeRefCkptAtGeneration(ckpt, generation));
}

RefCkpt lifeEpochCkpt(uint64_t life_epoch, std::optional<RefTxnId> committed_through = std::nullopt)
{
    return RefCkpt{.life_epoch = std::optional<uint64_t>{life_epoch},
                   .committed_through = committed_through,
                   .checkpoint_snapshot_id = std::nullopt,
                   .last_epoch_seal = std::nullopt};
}

/// The decoded transaction at `id`, or `nullopt` when the object is absent. Never dereferences a
/// disengaged optional: an aborted binary would take every later suite's result with it.
std::optional<RefLogTxn> readLogTxn(Backend & backend, const Layout & layout, const RootNamespace & ns, RefTxnId id)
{
    const auto got = readAt(backend, layout.refLogKey(DB::Cas::tests::fixture::fixtureLife(ns), id));
    if (!got)
        return std::nullopt;
    return decodeRefLogTxn(openObject(FormatId::RefLog, got->bytes), ns.string(), id);
}

uint64_t counterOf(ProfileEvents::Event event)
{
    return ProfileEvents::global_counters[event];
}

NamespaceLifeId catalogLife(const BackendPtr & backend, const Layout & layout, const RootNamespace & ns)
{
    const CasRefCatalog::Snapshot catalog = readCatalogForTest(backend, layout);
    for (const CatalogEntry & entry : catalog.catalog.entries)
        if (entry.ns == ns)
            return NamespaceLifeId::fromCatalogEntry(entry.ns, entry.incarnation);
    throw std::runtime_error("test namespace has no catalog life");
}

/// Publishes "a", then makes the install of "b" fail after its log is durable: the lane is
/// `NeedsRecovery` with the attempt retained, and the durable `{1,2}` is not acknowledged.
NamespaceLifeId strandOneDurableUnacknowledgedAppend(
    const std::shared_ptr<HidingListBackend> & backend, const PoolPtr & store, const Layout & layout,
    const RootNamespace & ns)
{
    store->appendRefOps(ns, MutationScope::ref("a"),
        [](const RefTableState & state)
        {
            std::vector<RefOp> ops;
            if (state.getLifecycle() != RefLifecycle::Live)
                ops.push_back(namespaceBirthOp());
            for (const RefOp & op : publishCommittedOps("a", manifestRef(1, 1, 1)))
                ops.push_back(op);
            return ops;
        }, RootMutationOrigin::Writer, RootMutationKind::Publish);

    /// Re-allowing allocations for the throw: `std::rethrow_exception` allocates, and the install region
    /// denies allocations in debug builds.
    auto planned_failure = std::make_exception_ptr(DB::Exception(DB::ErrorCodes::CORRUPTED_DATA, "install probe"));
    auto fired = std::make_shared<std::atomic<bool>>(false);
    store->setInstallRegionProbeForTest([planned_failure, fired]
    {
        if (fired->exchange(true))
            return;
        ALLOW_ALLOCATIONS_IN_SCOPE;
        std::rethrow_exception(planned_failure);
    });
    expectThrowsCode(DB::ErrorCodes::CORRUPTED_DATA, [&]
    {
        store->appendRefOps(ns, MutationScope::ref("b"),
            [](const RefTableState &) { return publishCommittedOps("b", manifestRef(1, 2, 1)); },
            RootMutationOrigin::Writer, RootMutationKind::Publish);
    });
    store->setInstallRegionProbeForTest(nullptr);
    const NamespaceLifeId life = catalogLife(backend, layout, ns);
    EXPECT_TRUE(readAt(*backend, layout.refLogKey(life, RefTxnId{1, 2})).has_value());
    return life;
}

CatalogEntry replaceCatalogLifeForTest(
    const BackendPtr & backend, const Layout & layout, const CatalogEntry & predecessor,
    UInt128 successor_incarnation)
{
    const CasRefCatalog::Snapshot before_delete = readCatalogForTest(backend, layout);
    RefCatalog without_predecessor = before_delete.catalog;
    std::erase_if(without_predecessor.entries, [&](const CatalogEntry & entry)
    {
        return entry.ns == predecessor.ns && entry.incarnation == predecessor.incarnation;
    });
    if (!before_delete.etag
        || !replaceForTest(backend, layout.refCatalogKey(), encodeRefCatalog(without_predecessor),
                           *before_delete.etag))
        throw std::runtime_error("test failed to retire exact predecessor catalog life");

    CatalogEntry successor{
        .ns = predecessor.ns,
        .state = NsState::Live,
        .incarnation = successor_incarnation,
        .creator = std::nullopt};
    const CasRefCatalog::Snapshot after_delete = readCatalogForTest(backend, layout);
    RefCatalog reborn = after_delete.catalog;
    reborn.entries.push_back(successor);
    if (!after_delete.etag
        || !replaceForTest(backend, layout.refCatalogKey(), encodeRefCatalog(reborn), *after_delete.etag))
        throw std::runtime_error("test failed to publish successor catalog life");
    return successor;
}

}

/// ---------------------------------------------------------------------------------------------
/// The checkpoint-bounded arithmetic tail: no recovery LIST
/// ---------------------------------------------------------------------------------------------

/// The durable stream is `{1,1} {1,2} {1,3}` while the backend hides the middle key from LIST.
/// Recovery must make zero stream LIST requests and recover the same exact checkpoint range.
TEST(CASRefRecoveryCasWalk, HiddenMiddleLogDoesNotAffectCheckpointRecovery)
{
    auto backend = std::make_shared<HidingListBackend>();
    const Layout layout("p");
    const RootNamespace ns{"srv1/hint_middle"};

    seedCkpt(*backend, layout, ns, lifeEpochCkpt(1, RefTxnId{1, 3}));
    seedTxn(*backend, layout, ns, RefTxnId{1, 1}, "a", /*birth=*/true);
    seedTxn(*backend, layout, ns, RefTxnId{1, 2}, "b", /*birth=*/false);
    seedTxn(*backend, layout, ns, RefTxnId{1, 3}, "c", /*birth=*/false);
    backend->hidden_keys.insert(layout.refLogKey(DB::Cas::tests::fixture::fixtureLife(ns), RefTxnId{1, 2}));

    auto store = openWalkPool(backend);
    ASSERT_TRUE(store);
    backend->resetCounts();

    const auto refs = store->listRefs(ns);
    EXPECT_EQ(backend->listCount(layout.namespaceStreamPrefix(DB::Cas::tests::fixture::fixtureLife(ns))), 0u);
    EXPECT_EQ(refs.size(), 3u) << "the arithmetic walk must fetch {1,2} by exact key";
    EXPECT_TRUE(refs.contains("a"));
    EXPECT_TRUE(refs.contains("b")) << "'b' is the ref the omitted transaction published";
    EXPECT_TRUE(refs.contains("c"));
}

/// The same sentinel at the tail. A hidden tail key is still found by the bounded exact walk, not by a
/// stream enumeration.
TEST(CASRefRecoveryCasWalk, HiddenTailLogDoesNotAffectCheckpointRecovery)
{
    auto backend = std::make_shared<HidingListBackend>();
    const Layout layout("p");
    const RootNamespace ns{"srv1/hint_tail"};

    seedCkpt(*backend, layout, ns, lifeEpochCkpt(1, RefTxnId{1, 2}));
    seedTxn(*backend, layout, ns, RefTxnId{1, 1}, "a", /*birth=*/true);
    seedTxn(*backend, layout, ns, RefTxnId{1, 2}, "b", /*birth=*/false);
    backend->hidden_keys.insert(layout.refLogKey(DB::Cas::tests::fixture::fixtureLife(ns), RefTxnId{1, 2}));

    auto store = openWalkPool(backend);
    ASSERT_TRUE(store);
    backend->resetCounts();

    const auto refs = store->listRefs(ns);
    EXPECT_EQ(backend->listCount(layout.namespaceStreamPrefix(DB::Cas::tests::fixture::fixtureLife(ns))), 0u);
    EXPECT_EQ(refs.size(), 2u) << "an omitted TAIL id is indistinguishable from the end of the stream to a "
                                  "listing; recovery never enumerates it and exact-reads the checkpoint range";
    EXPECT_TRUE(refs.contains("b"));
}

/// Hiding the checkpoint base snapshot from LIST cannot matter: the checkpoint names it, recovery
/// exact-reads its matching non-seal log first, then exact-reads the snapshot.
TEST(CASRefRecoveryCasWalk, CkptNamedBaseIsRecoveredWithoutStreamList)
{
    auto backend = std::make_shared<HidingListBackend>();
    const Layout layout("p");
    const RootNamespace ns{"srv1/hint_snap"};

    const RefTxnId base{1, 1};
    seedTxn(*backend, layout, ns, base, "a", /*birth=*/true);
    writeRefSnapshotRaw(*backend, layout,
        minimalLiveSnapshot(ns.string(), base, {committedRow("a", manifestRef(1, 1, 1))}));
    seedTxn(*backend, layout, ns, RefTxnId{1, 2}, "c", /*birth=*/false);
    seedCkpt(*backend, layout, ns, RefCkpt{.life_epoch = std::optional<uint64_t>{1},
                                           .committed_through = base,
                                           .checkpoint_snapshot_id = base,
                                           .last_epoch_seal = std::nullopt});
    backend->hidden_keys.insert(layout.refSnapshotKey(DB::Cas::tests::fixture::fixtureLife(ns), base));

    auto store = openWalkPool(backend);
    ASSERT_TRUE(store);
    backend->resetCounts();

    const auto refs = store->listRefs(ns);
    EXPECT_EQ(backend->listCount(layout.namespaceStreamPrefix(DB::Cas::tests::fixture::fixtureLife(ns))), 0u);
    EXPECT_EQ(refs.size(), 2u) << "the checkpoint names the base; the listing's omission is irrelevant";
    EXPECT_TRUE(refs.contains("a")) << "'a' exists inside the checkpoint-named snapshot";
    EXPECT_TRUE(refs.contains("c"));
}

TEST(CASRefRecoveryCasWalk, MissingExactIdAtOrBelowCommittedFrontierIsCorruption)
{
    auto backend = std::make_shared<HidingListBackend>();
    const Layout layout("p");
    const RootNamespace ns{"srv1/missing_below_frontier"};
    const RefTxnId frontier{1, 2};

    DB::Cas::tests::fixture::admitLive(*backend, layout, ns);
    const NamespaceLifeId life = catalogLife(backend, layout, ns);
    seedTxn(*backend, layout, ns, RefTxnId{1, 1}, "a", /*birth=*/true);
    createAt(*backend, layout.refCkptKey(life), encodeRefCkptAtGeneration(RefCkpt{
        .life_epoch = std::optional<uint64_t>{1},
        .committed_through = frontier,
        .checkpoint_snapshot_id = std::nullopt,
        .last_epoch_seal = std::nullopt}, 1));
    const auto ckpt_before = readCkptForTest(backend, layout, life);
    ASSERT_TRUE(ckpt_before);

    auto store = openWalkPool(backend);
    expectThrowsCode(DB::ErrorCodes::CORRUPTED_DATA, [&] { (void)store->listRefs(ns); });

    const auto ckpt_after = readCkptForTest(backend, layout, life);
    ASSERT_TRUE(ckpt_after);
    EXPECT_EQ(tokenText(ckpt_after->etag), tokenText(ckpt_before->etag))
        << "an unchanged checkpoint makes the missing committed id corruption, not a shorter stream";
}

TEST(CASRefRecoveryCasWalk, UncommittedSnapshotIsUnobservedWithoutStreamList)
{
    auto backend = std::make_shared<HidingListBackend>();
    const Layout layout("p");
    const RootNamespace ns{"srv1/hint_above_frontier"};
    const RefTxnId frontier{1, 1};
    const RefTxnId uncommitted_snapshot_id{1, 2};

    DB::Cas::tests::fixture::admitLive(*backend, layout, ns);
    const NamespaceLifeId life = catalogLife(backend, layout, ns);
    seedTxn(*backend, layout, ns, frontier, "committed", /*birth=*/true);
    writeRefSnapshotRaw(*backend, layout,
        minimalLiveSnapshot(ns.string(), uncommitted_snapshot_id,
            {committedRow("laundered", manifestRef(1, 2, 1))}));
    createAt(*backend, layout.refCkptKey(life), encodeRefCkptAtGeneration(RefCkpt{
        .life_epoch = std::optional<uint64_t>{1},
        .committed_through = frontier,
        .checkpoint_snapshot_id = std::nullopt,
        .last_epoch_seal = std::nullopt}, 1));

    auto store = openWalkPool(backend);
    backend->resetCounts();
    const auto refs = store->listRefs(ns);

    EXPECT_EQ(backend->listCount(layout.namespaceStreamPrefix(life)), 0u);
    EXPECT_TRUE(refs.contains("committed"));
    EXPECT_FALSE(refs.contains("laundered"))
        << "a physical snapshot not named by `_ckpt` cannot raise the recovered cut";
}

TEST(CASRefRecoveryCasWalk, ListingShapeDoesNotAffectCheckpointRecovery)
{
    const Layout layout("p");
    const RootNamespace ns{"srv1/list_equivalence"};
    const RefTxnId base{1, 1};
    const RefTxnId frontier{1, 2};
    auto seed = std::make_shared<HidingListBackend>();

    DB::Cas::tests::fixture::admitLive(*seed, layout, ns);
    seedTxn(*seed, layout, ns, base, "a", /*birth=*/true);
    writeRefSnapshotRaw(*seed, layout,
        minimalLiveSnapshot(ns.string(), base, {committedRow("a", manifestRef(1, 1, 1))}));
    seedTxn(*seed, layout, ns, frontier, "b", /*birth=*/false);
    seedCkpt(*seed, layout, ns, RefCkpt{
        .life_epoch = std::optional<uint64_t>{1},
        .committed_through = frontier,
        .checkpoint_snapshot_id = base,
        .last_epoch_seal = std::nullopt});
    const NamespaceLifeId life = catalogLife(seed, layout, ns);

    const auto clone_seed = [&]() -> std::shared_ptr<HidingListBackend>
    {
        /// A clone starts empty: constructing the normal fixture would pre-seed independent pool-meta
        /// bytes before this loop could copy the source's identical durable image.
        auto backend = std::make_shared<HidingListBackend>(/*seed_pool_meta=*/false);
        CasRequests seed_requests(seed, Fence::open(), {}, {}, nullptr, DB::Cas::kDefaultRequestWindowMs);
        CasOperation seed_op = seed_requests.admit();
        String cursor;
        do
        {
            const ListPage page = seed_op.list("", cursor, 1000, Retry::standard());
            for (const ListedKey & listed : page.keys)
            {
                const auto object = seed_op.read(listed.key, Retry::standard());
                if (!object)
                    throw std::runtime_error("seed LIST returned a key that exact GET could not read");
                const auto existing = readAt(*backend, listed.key);
                if (existing)
                {
                    if (existing->bytes != object->bytes)
                        throw std::runtime_error("clone backend constructor disagreed with seeded object");
                }
                else
                {
                    OperationForTest clone_op(*backend);
                    if (!std::holds_alternative<Committed>((*clone_op).create(listed.key, object->bytes, Retry::once())))
                        throw std::runtime_error("clone backend failed to copy seeded object");
                }
            }
            cursor = page.next_cursor;
        } while (!cursor.empty());
        return backend;
    };
    const auto recover = [&](const std::shared_ptr<HidingListBackend> & backend)
    {
        auto store = openWalkPool(backend);
        backend->resetCounts();
        const auto refs = store->listRefs(ns);
        EXPECT_EQ(backend->listCount(layout.namespaceStreamPrefix(life)), 0u);
        return refs;
    };

    const auto full = recover(clone_seed());
    auto empty_backend = clone_seed();
    empty_backend->hidden_keys.insert(layout.refSnapshotKey(life, base));
    empty_backend->hidden_keys.insert(layout.refLogKey(life, frontier));
    const auto empty = recover(empty_backend);
    ASSERT_EQ(full.size(), empty.size());
    for (const auto & [name, resolved] : full)
    {
        ASSERT_TRUE(empty.contains(name));
        EXPECT_EQ(resolved.manifest_id, empty.at(name).manifest_id);
        EXPECT_EQ(resolved.manifest_size, empty.at(name).manifest_size);
        EXPECT_EQ(resolved.published_at_ms, empty.at(name).published_at_ms);
    }
    EXPECT_TRUE(empty.contains("a"));
    EXPECT_TRUE(empty.contains("b"));
}

TEST(CASRefRecoveryCasWalk, PhantomListedSnapshotIsUnobserved)
{
    auto backend = std::make_shared<HidingListBackend>();
    const Layout layout("p");
    const RootNamespace ns{"srv1/stale_snapshot_hint"};
    const RefTxnId checkpoint_base{1, 1};
    const RefTxnId frontier{1, 2};

    seedTxn(*backend, layout, ns, checkpoint_base, "a", /*birth=*/true);
    writeRefSnapshotRaw(*backend, layout,
        minimalLiveSnapshot(ns.string(), checkpoint_base, {committedRow("a", manifestRef(1, 1, 1))}));
    seedTxn(*backend, layout, ns, frontier, "b", /*birth=*/false);
    seedCkpt(*backend, layout, ns, RefCkpt{
        .life_epoch = std::optional<uint64_t>{1},
        .committed_through = frontier,
        .checkpoint_snapshot_id = checkpoint_base,
        .last_epoch_seal = std::nullopt});
    const NamespaceLifeId life = catalogLife(backend, layout, ns);
    backend->phantom_list_keys.insert(layout.refSnapshotKey(life, frontier));

    auto store = openWalkPool(backend);
    backend->resetCounts();
    const auto refs = store->listRefs(ns);

    EXPECT_EQ(backend->listCount(layout.namespaceStreamPrefix(life)), 0u);
    EXPECT_TRUE(refs.contains("a"));
    EXPECT_TRUE(refs.contains("b"));
}

TEST(CASRefRecoveryCasWalk, ListedFPlusTwoWithoutFPlusOneIsInertUncommittedDebris)
{
    auto backend = std::make_shared<HidingListBackend>();
    const Layout layout("p");
    const RootNamespace ns{"srv1/listed_uncommitted_debris"};
    const RefTxnId frontier{1, 1};

    seedTxn(*backend, layout, ns, frontier, "a", /*birth=*/true);
    seedTxn(*backend, layout, ns, RefTxnId{1, 3}, "debris", /*birth=*/false);
    seedCkpt(*backend, layout, ns, lifeEpochCkpt(1, frontier));

    auto store = openWalkPool(backend);
    backend->resetCounts();
    const auto refs = store->listRefs(ns);

    EXPECT_EQ(backend->listCount(layout.namespaceStreamPrefix(DB::Cas::tests::fixture::fixtureLife(ns))), 0u);
    EXPECT_TRUE(refs.contains("a"));
    EXPECT_FALSE(refs.contains("debris"));
}

TEST(CASRefRecoveryCasWalk, DuplicateCatalogLifeIsCorruptionBeforeColdRuntimeAdmission)
{
    auto backend = std::make_shared<HidingListBackend>();
    const Layout layout("p");
    const RootNamespace ns{"srv1/ambiguous_life_a"};
    auto store = openWalkPool(backend);
    const NamespaceLifeId life = strandOneDurableUnacknowledgedAppend(backend, store, layout, ns);
    ASSERT_EQ(store->laneStateForTest(ns), RefLaneState::NeedsRecovery);

    const CasRefCatalog::Snapshot sampled = readCatalogForTest(backend, layout);
    ASSERT_EQ(sampled.catalog.entries.size(), 1u);
    RefCatalog ambiguous = sampled.catalog;
    ambiguous.entries.push_back(CatalogEntry{
        .ns = RootNamespace{"srv1/ambiguous_life_b"},
        .state = NsState::Live,
        .incarnation = life.incarnation});
    std::sort(ambiguous.entries.begin(), ambiguous.entries.end(),
        [](const CatalogEntry & lhs, const CatalogEntry & rhs) { return lhs.ns.string() < rhs.ns.string(); });
    ASSERT_TRUE(sampled.etag);
    ASSERT_TRUE(replaceForTest(backend, layout.refCatalogKey(), encodeRefCatalog(ambiguous), *sampled.etag));

    expectThrowsCode(DB::ErrorCodes::CORRUPTED_DATA, [&]
    {
        auto cold_store = openWalkPool(backend);
        (void)cold_store->listRefs(ns);
    });
}

TEST(CASRefRecoveryCasWalk, CheckpointAdvanceAfterLastLogProbeRestartsBeforeInstall)
{
    auto backend = std::make_shared<AfterGetHookBackend>();
    const Layout layout("p");
    const RootNamespace ns{"srv1/final_authority_validation"};
    const RefTxnId initial_frontier{1, 1};
    const RefTxnId concurrent_frontier{1, 2};

    seedTxn(*backend, layout, ns, initial_frontier, "a", /*birth=*/true);
    seedCkpt(*backend, layout, ns, lifeEpochCkpt(1, initial_frontier));
    const NamespaceLifeId life = catalogLife(backend, layout, ns);
    backend->watched_key = layout.refLogKey(life, concurrent_frontier);
    backend->after_get = [&]
    {
        seedTxn(*backend, layout, ns, concurrent_frontier, "b", /*birth=*/false);
        const auto sampled = readCkptForTest(backend, layout, life);
        ASSERT_TRUE(sampled);
        RefCkpt advanced = sampled->ckpt;
        advanced.committed_through = concurrent_frontier;
        ASSERT_TRUE(replaceForTest(backend, layout.refCkptKey(life), encodeRefCkptAtGeneration(advanced, 1), sampled->etag));
    };

    auto store = openWalkPool(backend);
    const uint64_t restarts_before = store->refRecoveryRestartsForTest(ns);
    const auto refs = store->listRefs(ns);

    EXPECT_TRUE(refs.contains("a"));
    EXPECT_TRUE(refs.contains("b"))
        << "the old private cut must be discarded when final exact authority moved after its last probe";
    EXPECT_GT(store->refRecoveryRestartsForTest(ns), restarts_before)
        << "the final authority observation is recovery's linearization point";
}

TEST(CASRefRecoveryCasWalk, LiveCatalogLifeWithoutReadableCheckpointIsCorruption)
{
    auto backend = std::make_shared<HidingListBackend>();
    const Layout layout("p");
    const RootNamespace ns{"srv1/live_without_ckpt"};

    DB::Cas::tests::fixture::admitLive(*backend, layout, ns);
    const NamespaceLifeId life = catalogLife(backend, layout, ns);
    seedTxn(*backend, layout, ns, RefTxnId{7, 1}, "hint-must-not-be-genesis", /*birth=*/true);
    ASSERT_FALSE(readCkptForTest(backend, layout, life));

    auto store = openWalkPool(backend);
    expectThrowsCode(DB::ErrorCodes::CORRUPTED_DATA, [&] { (void)store->listRefs(ns); });
}

/// A 404 BELOW the exact committed frontier is not the end of the stream -- it is a HOLE, and a hole
/// in a dense stream is corruption. Recovery exact-reads the checkpoint token once to distinguish a
/// concurrently moved cut from durable-data loss, then FAILS CLOSED while that token is unchanged. It
/// must never fold what it has: that is precisely how an acknowledged transaction disappears.
TEST(CASRefRecoveryCasWalk, AbsentIdBelowADurableHigherIdFailsClosed)
{
    auto backend = std::make_shared<HidingListBackend>();
    const Layout layout("p");
    const RootNamespace ns{"srv1/hole"};

    seedCkpt(*backend, layout, ns, lifeEpochCkpt(1, RefTxnId{1, 3}));
    seedTxn(*backend, layout, ns, RefTxnId{1, 1}, "a", /*birth=*/true);
    /// {1,2} is MISSING while {1,3} is durable and listed: the listing itself witnesses the hole.
    seedTxn(*backend, layout, ns, RefTxnId{1, 3}, "c", /*birth=*/false);

    auto store = openWalkPool(backend);
    ASSERT_TRUE(store);
    store->setCasRetrySleepForTest([](uint64_t) {});

    expectThrowsCode(DB::ErrorCodes::CORRUPTED_DATA, [&] { store->listRefs(ns); });
}

/// ---------------------------------------------------------------------------------------------
/// The CAS-walk: closing dead epochs in-band
/// ---------------------------------------------------------------------------------------------

/// The ordinary case: one dead epoch, closed by OUR seal at `{E, T+1}` -- the exact key a dying
/// predecessor's in-flight PUT would have taken, which is what makes the store's conditional create the
/// fence (INV-2) rather than a detector after the fact.
TEST(CASRefRecoveryCasWalk, DeadEpochIsClosedByOurOwnSealAtTPlusOne)
{
    auto backend = std::make_shared<HidingListBackend>();
    const Layout layout("p");
    const RootNamespace ns{"srv1/seal_created"};

    burnEpochsUpTo(backend, layout, /*target_live_epoch=*/2);
    seedCkpt(*backend, layout, ns, lifeEpochCkpt(1, RefTxnId{1, 1}));
    seedTxn(*backend, layout, ns, RefTxnId{1, 1}, "a", /*birth=*/true);

    auto store = openWalkPool(backend);
    ASSERT_TRUE(store);
    ASSERT_EQ(store->liveWriterEpoch(), 2u);

    const uint64_t sealed_before = counterOf(ProfileEvents::CASRefRecoveryEpochSealed);
    ASSERT_EQ(store->listRefs(ns).size(), 1u);
    EXPECT_EQ(counterOf(ProfileEvents::CASRefRecoveryEpochSealed), sealed_before + 1);

    const RefTxnId seal_id{1, 2};
    const auto seal = readLogTxn(*backend, layout, ns, seal_id);
    ASSERT_TRUE(seal.has_value()) << "epoch 1 is dead and must be closed at {1,2}";
    EXPECT_TRUE(refLogTxnIsEpochSeal(*seal));
    EXPECT_EQ(seal->prev_epoch_seal, std::nullopt) << "sequence 2 never carries a chain link";
    EXPECT_EQ(store->lastEpochSealForTest(ns), std::optional<RefTxnId>(seal_id))
        << "the chain link the next epoch's sequence-1 transaction must name";
}

/// A concurrent recoverer got there first. Its seal is already at `{E, T+1}`, so our conditional create
/// loses -- and the right reaction is to ADOPT it, not to treat a peer's correct write as interference.
/// The adopted seal is the same chain link ours would have been.
TEST(CASRefRecoveryCasWalk, ConcurrentRecoverersSealIsAdoptedNotContested)
{
    auto backend = std::make_shared<LateMaterializeBackend>();
    const Layout layout("p");
    const RootNamespace ns{"srv1/seal_adopt"};

    burnEpochsUpTo(backend, layout, /*target_live_epoch=*/2);
    seedCkpt(*backend, layout, ns, lifeEpochCkpt(1, RefTxnId{1, 1}));
    seedTxn(*backend, layout, ns, RefTxnId{1, 1}, "a", /*birth=*/true);
    /// The peer's seal lands between our read of {1,2} and our create of it, so we meet it as an
    /// OCCUPANT rather than as a tail entry.
    backend->late_key = layout.refLogKey(DB::Cas::tests::fixture::fixtureLife(ns), RefTxnId{1, 2});
    backend->late_bytes = sealObject(FormatId::RefLog, encodeRefLogTxn(makeSealTxn(ns, RefTxnId{1, 2})));

    auto store = openWalkPool(backend);
    ASSERT_TRUE(store);

    const uint64_t adopted_before = counterOf(ProfileEvents::CASRefRecoveryEpochSealAdopted);
    ASSERT_EQ(store->listRefs(ns).size(), 1u);
    EXPECT_GT(counterOf(ProfileEvents::CASRefRecoveryEpochSealAdopted), adopted_before);
    EXPECT_EQ(store->lastEpochSealForTest(ns), std::optional<RefTxnId>(RefTxnId{1, 2}))
        << "an adopted seal is this namespace's chain link exactly as a minted one is";
}

/// A STRAGGLER: an ordinary transaction of the dead epoch landed at `{E, T+1}` after our read of the
/// tail and before our seal. The rule is state-derived ids (INV-2): adopt the transaction, advance `T`
/// by ONE, and re-seal at the NEW `T+1`. Never mint `T+2` around it -- that writes a hole into the
/// durable stream that no later reader can tell from a lost object.
TEST(CASRefRecoveryCasWalk, StragglerAtTPlusOneIsAdoptedAndResealedAtTheNewTPlusOne)
{
    auto backend = std::make_shared<LateMaterializeBackend>();
    const Layout layout("p");
    const RootNamespace ns{"srv1/straggler"};

    burnEpochsUpTo(backend, layout, /*target_live_epoch=*/2);
    seedCkpt(*backend, layout, ns, lifeEpochCkpt(1, RefTxnId{1, 1}));
    seedTxn(*backend, layout, ns, RefTxnId{1, 1}, "a", /*birth=*/true);
    /// The dying epoch's last append materializes between our read of {1,2} and our create of it -- the
    /// straggler, arriving exactly where the every-attempt rule says it can.
    backend->late_key = layout.refLogKey(DB::Cas::tests::fixture::fixtureLife(ns), RefTxnId{1, 2});
    backend->late_bytes = sealObject(FormatId::RefLog,
        encodeRefLogTxn(makeOrdinaryTxn(ns, RefTxnId{1, 2}, "late", /*birth=*/false)));

    auto store = openWalkPool(backend);
    ASSERT_TRUE(store);

    const uint64_t straggler_before = counterOf(ProfileEvents::CASRefRecoveryStragglerAdopted);
    const auto refs = store->listRefs(ns);
    EXPECT_EQ(refs.size(), 2u) << "the straggler's transaction is durable and must be applied, not skipped";
    EXPECT_TRUE(refs.contains("late"));
    EXPECT_GT(counterOf(ProfileEvents::CASRefRecoveryStragglerAdopted), straggler_before);

    const auto seal = readLogTxn(*backend, layout, ns, RefTxnId{1, 3});
    ASSERT_TRUE(seal.has_value()) << "the epoch must be re-sealed at the NEW T+1 = {1,3}, never at a blindly minted T+2";
    EXPECT_TRUE(refLogTxnIsEpochSeal(*seal));
    EXPECT_EQ(store->lastEpochSealForTest(ns), std::optional<RefTxnId>(RefTxnId{1, 3}));
}

/// A recovery whose generation 3 stamp does not resolve sends no tombstone and installs nothing: the
/// tombstone may follow only a stamp that landed.
TEST(CASRefRecoveryCasWalk, UnresolvedRecoveryStampWritesNoTombstoneAndInstallsNothing)
{
    auto backend = std::make_shared<HidingListBackend>();
    const Layout layout("p");
    const RootNamespace ns{"srv1/unresolved_recovery_stamp"};

    burnEpochsUpTo(backend, layout, 2);
    seedTxn(*backend, layout, ns, RefTxnId{1, 1}, "a", /*birth=*/true);
    seedCkpt(*backend, layout, ns, lifeEpochCkpt(1, RefTxnId{1, 1}));
    const NamespaceLifeId life = catalogLife(backend, layout, ns);
    const String ckpt_key = layout.refCkptKey(life);
    const String tombstone_key = layout.refLogKey(life, RefTxnId{1, 2});

    /// Held in a shared atomic, not a plain local: the retry-sleep hook below mutates it, and the Pool
    /// can outlive this stack frame, so a by-reference capture of a local would dangle.
    auto fake_now = std::make_shared<std::atomic<uint64_t>>(1'000'000);
    PoolConfig config = walkTestConfig();
    config.boot_ms_fn = [fake_now] { return fake_now->load(); };
    config.cas_request_budget.recovery_retry_budget_ms = 1;
    config.cas_request_budget.recovery_retry_initial_backoff_ms = 1;
    config.cas_request_budget.recovery_retry_max_backoff_ms = 1;
    auto store = openWalkPool(backend, config);
    ASSERT_EQ(store->liveWriterEpoch(), 2u);
    store->setCasRetrySleepForTest([fake_now](uint64_t ms) { *fake_now += ms; });

    backend->ambiguous_cas_substr = ckpt_key;
    backend->ambiguous_cas_count = kFaultsBeyondTheRetryWindow;
    backend->resetCounts();
    expectThrowsCode(DB::ErrorCodes::NETWORK_ERROR, [&] { (void)store->listRefs(ns); });
    const int stamp_attempts = kFaultsBeyondTheRetryWindow - backend->ambiguous_cas_count;
    backend->ambiguous_cas_count = 0;

    EXPECT_GE(stamp_attempts, 1) << "the stamp was sent";
    EXPECT_EQ(backend->writeCount(tombstone_key), 0u) << "no tombstone follows a stamp that did not resolve";
    EXPECT_FALSE(readAt(*backend, tombstone_key));
    const auto ckpt = readAt(*backend, ckpt_key);
    ASSERT_TRUE(ckpt);
    ASSERT_TRUE(sniffHeaderLine(ckpt->bytes).has_value());
    EXPECT_EQ(sniffHeaderLine(ckpt->bytes)->v, 1u) << "the ambiguous stamp did not land";
    EXPECT_FALSE(store->refTableRecoveredForTest(ns)) << "nothing is installed";
}

/// GENESIS. A namespace born at epoch 5 has no epochs 1-4 of its own: they are not "empty epochs it
/// failed to close", they are epochs before it existed. The walk starts at the namespace's `life_epoch`
/// and writes no phantom seals below it, and with no transition ever having happened it installs NO
/// chain link -- `nullopt` means genesis and must mean it exactly, or the table's first transaction
/// would be required to name a seal that never existed.
TEST(CASRefRecoveryCasWalk, GenesisAtEpochFiveWritesNoPhantomSealsBelowLifeEpoch)
{
    auto backend = std::make_shared<HidingListBackend>();
    const Layout layout("p");
    const RootNamespace ns{"srv1/genesis5"};

    burnEpochsUpTo(backend, layout, /*target_live_epoch=*/5);
    seedCkpt(*backend, layout, ns, lifeEpochCkpt(5, RefTxnId{5, 1}));
    seedTxn(*backend, layout, ns, RefTxnId{5, 1}, "a", /*birth=*/true);

    auto store = openWalkPool(backend);
    ASSERT_TRUE(store);
    ASSERT_EQ(store->liveWriterEpoch(), 5u);

    ASSERT_EQ(store->listRefs(ns).size(), 1u);

    for (uint64_t e = 1; e <= 4; ++e)
        EXPECT_FALSE(readLogTxn(*backend, layout, ns, RefTxnId{e, 1}).has_value())
            << "no seal may be written for epoch " << e << ", which predates this namespace";
    EXPECT_EQ(store->lastEpochSealForTest(ns), std::nullopt)
        << "no transition ever happened for this namespace: nullopt means GENESIS and must mean it exactly";
}

/// ---------------------------------------------------------------------------------------------
/// The trio: ONE captured generation, three checks
/// ---------------------------------------------------------------------------------------------

/// The GENERIC mid-walk bump: the fence moves while recovery is doing I/O, so the incarnation that
/// admitted this work is gone. Nothing may be installed -- the recovered view belongs to a mount that no
/// longer owns the namespace. The table stays unrecovered, and a retry under the CURRENT generation
/// succeeds, which is what makes this a refusal rather than a wedge.
TEST(CASRefRecoveryCasWalk, FenceBumpedMidWalkRefusesTheInstallAndTheRetrySucceeds)
{
    auto backend = std::make_shared<GetSeamBackend>();
    const Layout layout("p");
    const RootNamespace ns{"srv1/bump_midwalk"};

    seedCkpt(*backend, layout, ns, lifeEpochCkpt(1, RefTxnId{1, 1}));
    seedTxn(*backend, layout, ns, RefTxnId{1, 1}, "a", /*birth=*/true);

    auto store = openWalkPool(backend);
    ASSERT_TRUE(store);

    std::atomic<bool> bumped{false};
    backend->watched_substr = "_log/";
    backend->on_key = [&](const String &)
    {
        if (!bumped.exchange(true))
            rearmMountFenceAfterAnomalyForTest(store);
    };

    EXPECT_ANY_THROW(store->listRefs(ns)) << "a recovery whose I/O window straddled a fence bump must install nothing";

    backend->on_key = nullptr;
    fenceOutMountForRemount(*backend, layout.mountKey("test"));
    ASSERT_TRUE(store->tryRemountOnce())
        << "a generation bump cannot rebind the captured runtime; the production remount must publish "
           "a distinct runtime at the accepted generation";
    EXPECT_EQ(store->listRefs(ns).size(), 1u) << "the retry through the remounted runtime succeeds";
}

TEST(CASRefRecoveryCasWalk, RetiredLifePausedInRealRecoveryIoWritesAndInstallsNothing)
{
    auto backend = std::make_shared<GetSeamBackend>();
    const Layout layout("p");
    const RootNamespace ns{"srv1/recovery-retired-mid-io"};

    burnEpochsUpTo(backend, layout, /*target_live_epoch=*/2);
    seedCkpt(*backend, layout, ns, lifeEpochCkpt(1, RefTxnId{1, 1}));
    seedTxn(*backend, layout, ns, RefTxnId{1, 1}, "predecessor", /*birth=*/true);
    const CatalogEntry predecessor = readCatalogForTest(backend, layout).catalog.entries.front();
    const NamespaceLifeId predecessor_life
        = NamespaceLifeId::fromCatalogEntry(predecessor.ns, predecessor.incarnation);
    const auto predecessor_ckpt_before = readAt(*backend, layout.refCkptKey(predecessor_life));
    ASSERT_TRUE(predecessor_ckpt_before);

    auto store = openWalkPool(backend);
    ASSERT_EQ(store->liveWriterEpoch(), 2u);
    const uint64_t recovery_installs_before = store->recoveryInstallCountForTest();

    std::mutex mutex;
    std::condition_variable cv;
    bool paused = false;
    bool resume = false;
    backend->watched_substr = "_log/";
    backend->on_key = [&](const String &)
    {
        std::unique_lock lock(mutex);
        paused = true;
        cv.notify_all();
        cv.wait(lock, [&] { return resume; });
    };

    std::exception_ptr recovery_error;
    std::thread recovery([&]
    {
        try
        {
            (void)store->listRefs(ns);
        }
        catch (...)
        {
            recovery_error = std::current_exception();
        }
    });
    {
        std::unique_lock lock(mutex);
        cv.wait(lock, [&] { return paused; });
    }

    const CatalogEntry successor = replaceCatalogLifeForTest(backend, layout, predecessor, UInt128{0x5152});
    const NamespaceLifeId successor_life
        = NamespaceLifeId::fromCatalogEntry(successor.ns, successor.incarnation);
    createAt(*backend, layout.refCkptKey(successor_life), encodeRefCkptAtGeneration(lifeEpochCkpt(2), 1));
    const auto successor_ckpt_before = readAt(*backend, layout.refCkptKey(successor_life));
    ASSERT_TRUE(successor_ckpt_before);
    store->invalidateRemovedCatalogLife(predecessor_life);
    backend->resetCounts();

    {
        std::lock_guard lock(mutex);
        resume = true;
    }
    cv.notify_all();
    recovery.join();

    EXPECT_TRUE(recovery_error) << "the predecessor recovery must be refused, not exposed";
    EXPECT_EQ(backend->putCount(layout.refLogKey(predecessor_life, RefTxnId{1, 2})), 0u)
        << "no predecessor seal retry may be sent after exact retirement";
    EXPECT_EQ(backend->writeCount(layout.refCkptKey(predecessor_life)), 0u)
        << "no predecessor checkpoint CAS may be sent after exact retirement";
    const auto predecessor_ckpt_after = readAt(*backend, layout.refCkptKey(predecessor_life));
    ASSERT_TRUE(predecessor_ckpt_after);
    EXPECT_EQ(tokenText(predecessor_ckpt_after->etag), tokenText(predecessor_ckpt_before->etag));
    EXPECT_FALSE(store->refTableRecoveredForTest(ns)) << "the detached predecessor result was installed";
    EXPECT_EQ(store->recoveryInstallCountForTest(), recovery_installs_before)
        << "the detached predecessor reached the recovery publication point";
    const String successor_prefix = layout.namespaceStreamPrefix(successor_life);
    for (const String & key : backend->touchedKeys())
        EXPECT_EQ(key.find(successor_prefix), String::npos)
            << "predecessor recovery retargeted storage I/O into successor key " << key;
    const auto successor_ckpt_after = readAt(*backend, layout.refCkptKey(successor_life));
    ASSERT_TRUE(successor_ckpt_after);
    EXPECT_EQ(tokenText(successor_ckpt_after->etag), tokenText(successor_ckpt_before->etag));
    EXPECT_EQ(successor_ckpt_after->bytes, successor_ckpt_before->bytes);
}

/// Bump AFTER the tombstone landed and BEFORE the install. The tombstone is durable (it was written under a
/// generation that was still valid), but nothing may be installed and no `_ckpt` write follows it.
TEST(CASRefRecoveryCasWalk, FenceBumpedAfterTheTombstoneInstallsNothingAndWritesNoCkpt)
{
    auto backend = std::make_shared<PutHookBackend>();
    const Layout layout("p");
    const RootNamespace ns{"srv1/bump_after_seal"};

    burnEpochsUpTo(backend, layout, /*target_live_epoch=*/2);
    seedCkpt(*backend, layout, ns, lifeEpochCkpt(1), 3);
    seedTxn(*backend, layout, ns, RefTxnId{1, 1}, "a", /*birth=*/true);

    auto store = openWalkPool(backend);
    ASSERT_TRUE(store);

    const auto ckpt_before = readCkptForTest(backend, layout, DB::Cas::tests::fixture::fixtureLife(ns));
    ASSERT_TRUE(ckpt_before.has_value());

    backend->watched_substr = "_log/";
    backend->on_key = [&] { rearmMountFenceAfterAnomalyForTest(store); };

    EXPECT_ANY_THROW(store->listRefs(ns));
    EXPECT_FALSE(store->refTableRecoveredForTest(ns));
    EXPECT_TRUE(readLogTxn(*backend, layout, ns, RefTxnId{1, 2}).has_value()) << "the tombstone is durable";

    const auto ckpt_after = readCkptForTest(backend, layout, DB::Cas::tests::fixture::fixtureLife(ns));
    ASSERT_TRUE(ckpt_after.has_value());
    EXPECT_EQ(tokenText(ckpt_after->etag), tokenText(ckpt_before->etag)) << "no `_ckpt` write follows the tombstone";

    backend->on_key = nullptr;
    fenceOutMountForRemount(*backend, layout.mountKey("test"));
    ASSERT_TRUE(store->tryRemountOnce());
    EXPECT_EQ(store->listRefs(ns).size(), 1u) << "the retry through the remounted runtime installs normally";
}

/// Bump AFTER the generation 3 stamp landed and BEFORE the tombstone: the stamp is harmless, the
/// tombstone is refused before it is sent, and nothing is installed.
TEST(CASRefRecoveryCasWalk, FenceBumpedAfterTheStampWritesNoTombstoneAndInstallsNothing)
{
    auto backend = std::make_shared<PutHookBackend>();
    const Layout layout("p");
    const RootNamespace ns{"srv1/bump_after_ckpt"};

    burnEpochsUpTo(backend, layout, /*target_live_epoch=*/2);
    seedCkpt(*backend, layout, ns, lifeEpochCkpt(1, RefTxnId{1, 1}));
    seedTxn(*backend, layout, ns, RefTxnId{1, 1}, "a", /*birth=*/true);

    auto store = openWalkPool(backend);
    ASSERT_TRUE(store);

    backend->watched_substr = "/_ckpt";
    backend->on_key = [&] { rearmMountFenceAfterAnomalyForTest(store); };

    EXPECT_ANY_THROW(store->listRefs(ns)) << "a recovery whose generation moved must install nothing";
    EXPECT_FALSE(store->refTableRecoveredForTest(ns));

    const auto ckpt_after = readCkptForTest(backend, layout, DB::Cas::tests::fixture::fixtureLife(ns));
    ASSERT_TRUE(ckpt_after.has_value());
    EXPECT_EQ(ckpt_after->ckpt.generation, 3u) << "the stamp landed before the generation moved";
    EXPECT_FALSE(readLogTxn(*backend, layout, ns, RefTxnId{1, 2}).has_value())
        << "no tombstone is sent under a generation that moved";

    backend->on_key = nullptr;
    fenceOutMountForRemount(*backend, layout.mountKey("test"));
    ASSERT_TRUE(store->tryRemountOnce())
        << "a generation bump cannot rebind the captured runtime; the production remount must publish "
           "a distinct runtime at the accepted generation";
    EXPECT_EQ(store->listRefs(ns).size(), 1u) << "the retry through the remounted runtime installs normally";
}

/// ---------------------------------------------------------------------------------------------
/// The self-remount barrier
/// ---------------------------------------------------------------------------------------------

/// Spec §3: "self-remount cancels or waits out recovery before rearming." The install recheck alone is
/// not that rule -- it protects the install, not the WINDOW. A recovery paused in its I/O while the
/// fence is re-armed would still be holding an admitted generation that is about to be superseded, and
/// the barrier is what guarantees no `_ckpt` CAS and no install can follow the re-arm.
///
/// Driven at a real I/O seam: recovery blocks inside a `get`, the remount barrier is invoked from
/// another thread and must BLOCK, the recovery is released, acknowledges the cancellation, and only then
/// does the barrier return.
TEST(CASRefRecoveryCasWalk, RemountBarrierBlocksUntilAPausedRecoveryAcknowledgesCancellation)
{
    auto backend = std::make_shared<GetSeamBackend>();
    const Layout layout("p");
    const RootNamespace ns{"srv1/remount_barrier"};

    burnEpochsUpTo(backend, layout, /*target_live_epoch=*/2);
    seedCkpt(*backend, layout, ns, lifeEpochCkpt(1, RefTxnId{1, 1}));
    seedTxn(*backend, layout, ns, RefTxnId{1, 1}, "a", /*birth=*/true);

    auto store = openWalkPool(backend);
    ASSERT_TRUE(store);

    std::mutex m;
    std::condition_variable cv;
    bool recovery_parked = false;
    bool release_recovery = false;

    backend->watched_substr = "_log/";
    /// `GetSeamBackend` moves the hook out before calling it, so this parks exactly once without the
    /// hook having to clear itself -- see its `get` for why self-clearing is a use-after-free.
    backend->on_key = [&](const String &)
    {
        std::unique_lock lock(m);
        recovery_parked = true;
        cv.notify_all();
        cv.wait(lock, [&] { return release_recovery; });
    };

    const uint64_t ckpt_before = counterOf(ProfileEvents::CASRefCheckpointPublished);
    const uint64_t cancelled_before = counterOf(ProfileEvents::CASRefRecoveryCancelled);

    std::thread recovery([&] { try { store->listRefs(ns); } catch (...) {} }); // NOLINT(bugprone-empty-catch): the outcome is asserted below via the ProfileEvents counters, not this thread's exception

    {
        std::unique_lock lock(m);
        cv.wait(lock, [&] { return recovery_parked; });
    }

    std::atomic<bool> barrier_returned{false};
    std::thread barrier([&]
    {
        store->cancelRefRecoveriesAndAwaitQuiescence();
        barrier_returned.store(true);
    });

    /// Wait for the barrier's REQUEST to be visible before touching anything else. Releasing the parked
    /// recovery any earlier would race it past a flag set a moment too late, and the test would observe
    /// an ordinary completion and call it a missing cancellation.
    while (!store->refRecoveryCancelRequestedForTest(ns))
        std::this_thread::yield();

    /// The request is published and the recovery is still parked, so the barrier is now provably inside
    /// its wait. It must not have returned: fence re-arm may not proceed while a recovery is in flight.
    EXPECT_FALSE(barrier_returned.load());

    {
        std::lock_guard lock(m);
        release_recovery = true;
    }
    cv.notify_all();

    barrier.join();
    recovery.join();
    EXPECT_TRUE(barrier_returned.load());

    EXPECT_GT(counterOf(ProfileEvents::CASRefRecoveryCancelled), cancelled_before)
        << "the released recovery must observe the cancellation rather than run to completion";
    EXPECT_EQ(counterOf(ProfileEvents::CASRefCheckpointPublished), ckpt_before)
        << "a cancelled recovery performs ZERO _ckpt CASes";
    EXPECT_FALSE(store->refTableRecoveredForTest(ns)) << "and ZERO installs";
}

/// The cancellation reaches the walk's REQUESTS, not just its own polls. `readCheckpointSnapshotBase`
/// issues several reads back to back -- the base log, then the snapshot body -- and the walk's poll runs
/// only before the call, so a cancellation landing between those two reads used to be invisible until
/// the whole call returned. The walk's operation now carries the cancellation in its liveness, so the
/// next request is the one that refuses.
///
/// Parked on the base-log read, which is the FIRST request that call makes, so the snapshot body read
/// is the one that must never happen.
TEST(CASRefRecoveryCasWalk, CancellationStopsTheWalkBetweenTwoReadsOfOneCall)
{
    auto backend = std::make_shared<GetSeamBackend>();
    const Layout layout("p");
    const RootNamespace ns{"srv1/cancel_between_reads"};
    const RefTxnId base{1, 1};
    const RefTxnId frontier{1, 2};

    DB::Cas::tests::fixture::admitLive(*backend, layout, ns);
    seedTxn(*backend, layout, ns, base, "a", /*birth=*/true);
    writeRefSnapshotRaw(*backend, layout,
        minimalLiveSnapshot(ns.string(), base, {committedRow("a", manifestRef(1, 1, 1))}));
    seedTxn(*backend, layout, ns, frontier, "b", /*birth=*/false);
    seedCkpt(*backend, layout, ns, RefCkpt{
        .life_epoch = std::optional<uint64_t>{1},
        .committed_through = frontier,
        .checkpoint_snapshot_id = base,
        .last_epoch_seal = std::nullopt});
    const NamespaceLifeId life = catalogLife(backend, layout, ns);

    auto store = openWalkPool(backend);
    ASSERT_TRUE(store);

    std::mutex m;
    std::condition_variable cv;
    bool recovery_parked = false;
    bool release_recovery = false;

    backend->watched_substr = "_log/";
    /// `GetSeamBackend` moves the hook out before calling it, so this parks exactly once.
    backend->on_key = [&](const String &)
    {
        std::unique_lock lock(m);
        recovery_parked = true;
        cv.notify_all();
        cv.wait(lock, [&] { return release_recovery; });
    };

    std::thread recovery([&] { try { store->listRefs(ns); } catch (...) {} }); // NOLINT(bugprone-empty-catch): the outcome is asserted below through the request counts

    {
        std::unique_lock lock(m);
        cv.wait(lock, [&] { return recovery_parked; });
    }

    std::thread barrier([&] { store->cancelRefRecoveriesAndAwaitQuiescence(); });
    /// Wait for the REQUEST to be visible before releasing: releasing any earlier would race the walk
    /// past a flag set a moment too late, and the test would read an ordinary completion as a
    /// cancellation that never happened.
    while (!store->refRecoveryCancelRequestedForTest(ns))
        std::this_thread::yield();

    const uint64_t snapshot_reads_before = backend->getCount(layout.refSnapshotKey(life, base));
    ASSERT_EQ(snapshot_reads_before, 0u) << "the parked read is the base LOG read, before the body read";

    {
        std::lock_guard lock(m);
        release_recovery = true;
    }
    cv.notify_all();
    barrier.join();
    recovery.join();

    EXPECT_EQ(backend->getCount(layout.refSnapshotKey(life, base)), 0u)
        << "the cancellation must refuse the very next request of the same call, not be noticed after it";
    EXPECT_FALSE(store->refTableRecoveredForTest(ns)) << "and nothing is installed";
}

/// A `NeedsRecovery` lane replays the known-durable transaction before returning to `Ready`.
TEST(CASRefRecoveryCasWalk, NeedsRecoveryReplaysTheStrandedTxn)
{
    auto backend = std::make_shared<CountingBackend>();
    const Layout layout("p");
    const RootNamespace ns{"srv1/poisoned"};

    auto store = openWalkPool(backend);
    ASSERT_TRUE(store);

    /// Publish one ref through the real lane, then fail the next commit's install region: the
    /// transaction is durable and the install that would have recorded it throws.
    ASSERT_NO_THROW(store->appendRefOps(ns, MutationScope::ref("a"),
        [](const RefTableState & state)
        {
            std::vector<RefOp> ops;
            if (state.getLifecycle() != RefLifecycle::Live)
                ops.push_back(namespaceBirthOp());
            for (const RefOp & op : publishCommittedOps("a", manifestRef(1, 1, 1)))
                ops.push_back(op);
            return ops;
        },
        RootMutationOrigin::Writer, RootMutationKind::Publish));

    /// Built outside the region. One-shot, and re-allowing allocations for
    /// the duration of the throw: `std::rethrow_exception` allocates through libc++'s
    /// `__cxa_rethrow_primary_exception`, which the debug build's `DENY_ALLOCATIONS_IN_SCOPE` aborts on.
    /// (Found by the debug gate -- the first cut of this probe took the whole binary down there.) Same
    /// shape as `gtest_cas_ref_install_safety.cpp`'s `armOneShotInstallFailure`.
    auto planned_failure = std::make_exception_ptr(DB::Exception(DB::ErrorCodes::CORRUPTED_DATA, "install probe"));
    auto fired = std::make_shared<std::atomic<bool>>(false);
    store->setInstallRegionProbeForTest([planned_failure, fired]
    {
        if (fired->exchange(true))
            return;
        ALLOW_ALLOCATIONS_IN_SCOPE;
        std::rethrow_exception(planned_failure);
    });
    EXPECT_ANY_THROW(store->appendRefOps(ns, MutationScope::ref("b"),
        [](const RefTableState &) { return publishCommittedOps("b", manifestRef(1, 2, 1)); },
        RootMutationOrigin::Writer, RootMutationKind::Publish));
    store->setInstallRegionProbeForTest(nullptr);

    ASSERT_EQ(store->laneStateForTest(ns), RefLaneState::NeedsRecovery);
    /// "Durable but not applied here", stated as the two facts it is made of: the object IS in the
    /// store, and this runtime's floor is what keeps the allocator off its id. `ns` was born through
    /// the REAL production lane (`appendRefOps`), not the raw `seedTxn`/`casAdmitEntry` fixtures this
    /// file's OTHER tests use -- so its ref-layer objects sit at a REAL, catalog-minted incarnation,
    /// not the Stage-A sentinel `readLogTxn` assumes. Resolved here rather than through `readLogTxn`.
    {
        const CasRefCatalog::Snapshot snap = readCatalogForTest(backend, layout);
        const CatalogEntry * entry = nullptr;
        for (const CatalogEntry & e : snap.catalog.entries)
            if (e.ns.string() == ns.string())
                entry = &e;
        ASSERT_NE(entry, nullptr) << "the birth above must have minted a catalog entry for " << ns.string();
        const NamespaceLifeId life = NamespaceLifeId::fromCatalogEntry(entry->ns, entry->incarnation);
        ASSERT_TRUE(readAt(*backend, layout.refLogKey(life, RefTxnId{1, 2})).has_value())
            << "the stranded transaction must be durable -- otherwise recovery is not owed";
    }

    /// The next touch drives recovery again -- this is the structural closure Task 3 deferred here.
    const auto refs = store->listRefs(ns);
    EXPECT_EQ(refs.size(), 2u);
    EXPECT_TRUE(refs.contains("b")) << "the walk re-derived the stranded transaction from the durable log";
    EXPECT_EQ(store->laneStateForTest(ns), RefLaneState::Ready)
        << "only a completed recovery install returns the lane to Ready";
}

/// A restart has no in-memory attempt: the cold writer finds the durable unacknowledged append on the
/// chain, and its first append closes the epoch after it. A `NeedsRecovery` lane holds nothing in flight,
/// so the farewell is clean and the read writes nothing.
TEST(CASRefRecoveryCasWalk, ColdWriterRecoveryAdoptsTheDurableUnacknowledgedAppendBeforeItsTombstone)
{
    auto backend = std::make_shared<HidingListBackend>();
    const Layout layout("p");
    const RootNamespace ns{"srv1/recovery_cold_successor"};
    auto store = openWalkPool(backend);
    const NamespaceLifeId life = strandOneDurableUnacknowledgedAppend(backend, store, layout, ns);
    ASSERT_EQ(store->laneStateForTest(ns), RefLaneState::NeedsRecovery);
    store.reset();

    auto cold_store = openWalkPool(backend);
    ASSERT_EQ(cold_store->liveEpochPairForTest(), (LiveEpochPair{2, 0}));
    const auto refs = cold_store->listRefs(ns);

    EXPECT_TRUE(refs.contains("b"));
    EXPECT_EQ(cold_store->laneStateForTest(ns), RefLaneState::Ready);
    EXPECT_FALSE(readAt(*backend, layout.refLogKey(life, RefTxnId{1, 3})));
    cold_store->appendRefOps(ns, MutationScope::ref("c"),
        [](const RefTableState &) { return publishCommittedOps("c", manifestRef(2, 1, 1)); },
        RootMutationOrigin::Writer, RootMutationKind::Publish);
    const auto tombstone = readAt(*backend, layout.refLogKey(life, RefTxnId{1, 3}));
    ASSERT_TRUE(tombstone);
    const RefLogTxn decoded = decodeRefLogTxn(openObject(FormatId::RefLog, tombstone->bytes), ns.string(), RefTxnId{1, 3});
    EXPECT_TRUE(refLogTxnIsEpochSeal(decoded));
    EXPECT_EQ(decoded.next_epoch, std::optional<uint64_t>{2});
}

TEST(CASRefRecoveryCasWalk, WriterRecoveryRejectsDifferentOrdinaryBytesAtTheRetainedSuccessorSlot)
{
    auto backend = std::make_shared<HidingListBackend>();
    const Layout layout("p");
    const RootNamespace ns{"srv1/recovery_different_successor"};
    auto store = openWalkPool(backend);
    const NamespaceLifeId life = strandOneDurableUnacknowledgedAppend(backend, store, layout, ns);
    ASSERT_EQ(store->laneStateForTest(ns), RefLaneState::NeedsRecovery);

    const String successor_key = layout.refLogKey(life, RefTxnId{1, 2});
    const auto original = readAt(*backend, successor_key);
    ASSERT_TRUE(original);
    const RefLogTxn different = makeOrdinaryTxn(ns, RefTxnId{1, 2}, "different", /*birth=*/false);
    {
        OperationForTest op(*backend);
        ASSERT_TRUE(std::holds_alternative<Committed>((*op).replace(successor_key,
            sealObject(FormatId::RefLog, encodeRefLogTxn(different)),
            original->etag, Retry::once())));
    }

    const auto ckpt_before = readCkptForTest(backend, layout, life);
    ASSERT_TRUE(ckpt_before);
    expectThrowsCode(DB::ErrorCodes::CORRUPTED_DATA, [&] { (void)store->listRefs(ns); });
    EXPECT_EQ(store->laneStateForTest(ns), RefLaneState::NeedsRecovery);
    EXPECT_EQ(tokenText(readCkptForTest(backend, layout, life)->etag), tokenText(ckpt_before->etag)) << "a refused recovery writes no `_ckpt`";
}

TEST(CASRefRecoveryCasWalk, RetainedOldWriterAttemptLosesConclusiveToASuccessorSeal)
{
    auto backend = std::make_shared<HidingListBackend>();
    const Layout layout("p");
    const RootNamespace ns{"srv1/recovery_successor_seal"};
    auto store = openWalkPool(backend);
    const NamespaceLifeId life = strandOneDurableUnacknowledgedAppend(backend, store, layout, ns);
    ASSERT_EQ(store->laneStateForTest(ns), RefLaneState::NeedsRecovery);

    const RefTxnId successor_id{1, 2};
    const String successor_key = layout.refLogKey(life, successor_id);
    const auto original = readAt(*backend, successor_key);
    ASSERT_TRUE(original);
    const RefLogTxn successor_tombstone = tombstoneTxn(ns.string(), successor_id, 2);
    {
        OperationForTest op(*backend);
        ASSERT_TRUE(std::holds_alternative<Committed>((*op).replace(successor_key,
            sealObject(FormatId::RefLog, encodeRefLogTxn(successor_tombstone)),
            original->etag, Retry::once())));
    }

    const auto refs = store->listRefs(ns);

    EXPECT_FALSE(refs.contains("b")) << "the old writer's retained ordinary bytes lost at the tombstone's slot";
    EXPECT_EQ(store->lastEpochSealForTest(ns), std::make_optional(successor_id));
    EXPECT_EQ(store->laneStateForTest(ns), RefLaneState::Closed)
        << "the tombstone names an epoch above this mount's: the lane is deposed";
}

/// ---------------------------------------------------------------------------------------------
/// Fail-closed on an unresolved slot
/// ---------------------------------------------------------------------------------------------

/// `Unresolved` from the slot-occupy means the store will not say whether our seal landed. That is not a
/// state to guess about: recovery takes the transient-retry path and, once its budget is spent, fails
/// closed with the table left unrecovered. Exposing a table whose dead epoch may or may not be closed is
/// the one outcome that must be impossible.
TEST(CASRefRecoveryCasWalk, UnresolvedSealSlotFailsClosedWithoutInstalling)
{
    auto backend = std::make_shared<HidingListBackend>();
    const Layout layout("p");
    const RootNamespace ns{"srv1/unresolved"};

    burnEpochsUpTo(backend, layout, /*target_live_epoch=*/2);
    seedCkpt(*backend, layout, ns, lifeEpochCkpt(1, RefTxnId{1, 1}));
    seedTxn(*backend, layout, ns, RefTxnId{1, 1}, "a", /*birth=*/true);

    /// The backoff sleep ADVANCES the same fake clock the budget is measured against, so the retry
    /// envelope is spent in a handful of iterations instead of spinning against a frozen clock. Not
    /// cosmetic: with a frozen clock this test burns ~700k retries and the same number of log lines,
    /// which is how a real regression in this arm would become invisible in the noise.
    /// Held in a shared atomic, not a plain local: the retry-sleep hook below mutates it, and the Pool
    /// can outlive this stack frame (a background publish holds `shared_from_this()`), so a
    /// by-reference capture of a local would dangle.
    auto fake_now = std::make_shared<std::atomic<uint64_t>>(1'000'000);
    PoolConfig config = walkTestConfig();
    config.boot_ms_fn = [fake_now]
    {
        return fake_now->load();
    };
    auto store = openWalkPool(backend, config);
    ASSERT_TRUE(store);

    store->setCasRetrySleepForTest([fake_now](uint64_t ms)
    {
        *fake_now += ms;
    });
    backend->ambiguous_put_substr = "/_log/";

    const uint64_t fake_now_before = fake_now->load();
    EXPECT_ANY_THROW(store->listRefs(ns));
    EXPECT_FALSE(store->refTableRecoveredForTest(ns))
        << "a table whose dead epoch may or may not be closed must never be exposed as recovered";
    /// The engine reissued -- more than one physical attempt -- and paid a real retry pause on the
    /// injected clock before giving up; a fault settled by a single, unretried attempt would not
    /// exercise the transient-retry path this test's own name and docstring claim to drive.
    EXPECT_GT(backend->ambiguous_put_attempts.load(), 1u);
    EXPECT_GT(fake_now->load(), fake_now_before);
}

/// ---------------------------------------------------------------------------------------------
/// Carried forward from the retired `RefWriterRecoverySeal` suite
/// ---------------------------------------------------------------------------------------------

/// THE property the whole in-band design exists for, and the one the retired suite could only
/// approximate with a detector: the Late Predecessor PUT is REFUSED, by the store, at the key it wanted.
///
/// A dying writer of epoch 1 has an append in flight for `{1,2}`. Recovery closes epoch 1 by occupying
/// exactly that slot. When the ghost's conditional create finally reaches the store there is nothing for
/// it to do -- the key is write-once and taken. The old sentinel seal was a SNAPSHOT at a synthetic id,
/// which left `{1,2}` free: the ghost landed, and all anyone could do was notice afterwards.
TEST(CASRefRecoveryCasWalk, ALatePredecessorPutAtTheSealedSlotIsRefusedByTheStore)
{
    auto backend = std::make_shared<HidingListBackend>();
    const Layout layout("p");
    const RootNamespace ns{"srv1/ghost"};

    burnEpochsUpTo(backend, layout, /*target_live_epoch=*/2);
    seedCkpt(*backend, layout, ns, lifeEpochCkpt(1, RefTxnId{1, 1}));
    seedTxn(*backend, layout, ns, RefTxnId{1, 1}, "a", /*birth=*/true);

    auto store = openWalkPool(backend);
    ASSERT_TRUE(store);
    ASSERT_EQ(store->listRefs(ns).size(), 1u);

    /// The ghost: the exact append the dead epoch's writer had in flight, arriving late.
    const RefTxnId ghost_id{1, 2};
    const String ghost_bytes = sealObject(FormatId::RefLog,
        encodeRefLogTxn(makeOrdinaryTxn(ns, ghost_id, "ghost", /*birth=*/false)));
    OperationForTest ghost_op(*backend);
    const WriteResult put = (*ghost_op).create(
        layout.refLogKey(DB::Cas::tests::fixture::fixtureLife(ns), ghost_id), ghost_bytes, Retry::once());
    EXPECT_TRUE(std::holds_alternative<Conflict>(put))
        << "the seal occupies the ghost's own key, so the store itself is the fence";

    /// And the object at that key is still the seal, byte for byte -- nothing adopted the ghost.
    const auto occupant = readLogTxn(*backend, layout, ns, ghost_id);
    ASSERT_TRUE(occupant.has_value());
    EXPECT_TRUE(refLogTxnIsEpochSeal(*occupant));
}

/// An occupant at the seal slot that this build cannot decode is NOT a straggler to adopt and NOT a
/// peer's seal to defer to: it is an object at a key this namespace exclusively owns whose meaning is
/// unknown. Recovery fails closed on it -- and, just as importantly, stays RESTARTABLE: the throw must
/// leave `recovery_in_progress` cleared, or the table would be unrecoverable for the mount's life and
/// every later toucher would park forever on a condition variable nobody will signal.
TEST(CASRefRecoveryCasWalk, UndecodableOccupantAtTheSealSlotFailsClosedAndLeavesRecoveryRestartable)
{
    auto backend = std::make_shared<LateMaterializeBackend>();
    const Layout layout("p");
    const RootNamespace ns{"srv1/foreign_slot"};

    burnEpochsUpTo(backend, layout, /*target_live_epoch=*/2);
    seedCkpt(*backend, layout, ns, lifeEpochCkpt(1, RefTxnId{1, 1}));
    seedTxn(*backend, layout, ns, RefTxnId{1, 1}, "a", /*birth=*/true);
    backend->late_key = layout.refLogKey(DB::Cas::tests::fixture::fixtureLife(ns), RefTxnId{1, 2});
    backend->late_bytes = "not a ref-log object at all";

    auto store = openWalkPool(backend);
    ASSERT_TRUE(store);

    expectThrowsCode(DB::ErrorCodes::CORRUPTED_DATA, [&] { store->listRefs(ns); });
    EXPECT_FALSE(store->refTableRecoveredForTest(ns));

    /// Restartable: a second touch runs a WHOLE new attempt (it fails the same way, which is the point --
    /// it reaches the failure again rather than hanging on a stuck `recovery_in_progress`).
    expectThrowsCode(DB::ErrorCodes::CORRUPTED_DATA, [&] { store->listRefs(ns); });
}

/// A second caller that arrives while a recovery is mid-walk WAITS for it rather than racing an
/// independent walk of its own. Two concurrent walks would both try to occupy the same seal slot, and
/// while the loser adopts correctly, they would also both replay the whole tail and one would install a
/// state the other's install immediately replaces -- work and I/O for nothing, on the path that is
/// already the most expensive one in the system.
TEST(CASRefRecoveryCasWalk, ASecondCallerWaitsForTheWalkInsteadOfRacingIt)
{
    auto backend = std::make_shared<GetSeamBackend>();
    const Layout layout("p");
    const RootNamespace ns{"srv1/serialized"};

    burnEpochsUpTo(backend, layout, /*target_live_epoch=*/2);
    seedCkpt(*backend, layout, ns, lifeEpochCkpt(1, RefTxnId{1, 1}));
    seedTxn(*backend, layout, ns, RefTxnId{1, 1}, "a", /*birth=*/true);

    auto store = openWalkPool(backend);
    ASSERT_TRUE(store);

    std::mutex m;
    std::condition_variable cv;
    bool parked = false;
    bool release = false;

    backend->watched_substr = "_log/";
    backend->on_key = [&](const String &)
    {
        std::unique_lock lock(m);
        parked = true;
        cv.notify_all();
        cv.wait(lock, [&] { return release; });
    };

    const uint64_t adopted_before = counterOf(ProfileEvents::CASRefRecoveryEpochSealAdopted);
    std::thread first([&] { store->listRefs(ns); });
    {
        std::unique_lock lock(m);
        cv.wait(lock, [&] { return parked; });
    }

    /// The second caller blocks on `recovery_in_progress`. Its own recovery would have to LIST, and the
    /// walk holds no lock while parked, so nothing but the serialization flag can be keeping it out.
    std::atomic<bool> second_done{false};
    std::thread second([&] { store->listRefs(ns); second_done.store(true); });
    for (int i = 0; i < 50 && !second_done.load(); ++i)
        std::this_thread::yield();
    EXPECT_FALSE(second_done.load()) << "a second caller must wait out the in-flight walk, not race it";

    {
        std::lock_guard lock(m);
        release = true;
    }
    cv.notify_all();
    first.join();
    second.join();

    EXPECT_TRUE(store->refTableRecoveredForTest(ns));
    /// Exactly ONE walk minted the seal, and no second walk ever met it as an occupant. Adopting is the
    /// CORRECT outcome for a concurrent recoverer -- it is just work this serialization exists to avoid
    /// paying inside one process, so observing zero adoptions is what proves the second caller waited.
    EXPECT_EQ(counterOf(ProfileEvents::CASRefRecoveryEpochSealAdopted), adopted_before);
    const auto seal = readLogTxn(*backend, layout, ns, RefTxnId{1, 2});
    ASSERT_TRUE(seal.has_value());
    EXPECT_TRUE(refLogTxnIsEpochSeal(*seal));
}

/// Checkpoint-grounded recovery starts at the recreated life's own genesis and does not replay or
/// extend the predecessor life's stream.
///
/// The old same-stream fixture claimed that recovery walked through the epoch-1 removal into epoch 2.
/// With authoritative `_ckpt.life_epoch=2`, epoch 2 is instead the current life's genesis and the walk
/// begins at `{2,1}`. Epoch-1 objects are inert predecessor-life debris: they neither supply state nor
/// receive a recovery seal.
TEST(CASRefRecoveryCasWalk, RecoveryStartsAtRecreatedLifeGenesisAndLeavesPredecessorStreamUntouched)
{
    auto backend = std::make_shared<HidingListBackend>();
    const Layout layout("p");
    const RootNamespace ns{"srv1/removed_then_reborn"};

    burnEpochsUpTo(backend, layout, /*target_live_epoch=*/3);
    seedCkpt(*backend, layout, ns, lifeEpochCkpt(2, RefTxnId{2, 1}));
    seedTxn(*backend, layout, ns, RefTxnId{1, 1}, "a", /*birth=*/true);

    /// Epoch 1 ends with the terminal record: the ref is removed, then the namespace.
    RefLogTxn removal;
    removal.ns = ns.string();
    removal.txn_id = RefTxnId{1, 2};
    removal.ops = {DB::Cas::tests::ownerTransitionOp(
                       RefOwnerBinding{RefOwnerKind::Committed, "a", manifestRef(1, 1, 1u)}, std::nullopt),
                   removeNamespaceOp()};
    DB::Cas::tests::fixture::writeRefLogRaw(*backend, layout, removal);

    /// The current life starts in epoch 2. Its birth is sequence 1 of its own genesis epoch, so it
    /// carries no chain link to the predecessor life.
    seedTxn(*backend, layout, ns, RefTxnId{2, 1}, "reborn", /*birth=*/true);

    auto store = openWalkPool(backend);
    ASSERT_TRUE(store);
    ASSERT_EQ(store->liveWriterEpoch(), 3u);

    const auto refs = store->listRefs(ns);
    EXPECT_EQ(refs.size(), 1u);
    EXPECT_TRUE(refs.contains("reborn")) << "recovery must begin at the recreated life's genesis";

    EXPECT_FALSE(readLogTxn(*backend, layout, ns, RefTxnId{1, 3}).has_value())
        << "recovery of life epoch 2 must not extend the predecessor-life stream";
    const auto seal2 = readLogTxn(*backend, layout, ns, RefTxnId{2, 2});
    ASSERT_TRUE(seal2.has_value()) << "epoch 2 IS live again by the time it dies, so it closes normally";
    EXPECT_TRUE(refLogTxnIsEpochSeal(*seal2));
    EXPECT_EQ(seal2->prev_epoch_seal, std::nullopt) << "sequence 2 carries no chain link";
}

/// `PutHookBackendComposesHidingListBackendCasPutFaultInjection` was retired: it pinned that
/// `PutHookBackend::casPut` reaches its immediate parent `HidingListBackend::casPut` rather than
/// bypassing it to `CountingBackend` -- a fact about this file's own fixture class hierarchy (ordinary
/// C++ virtual dispatch), not a claim any production change could falsify. `PutHookBackend` and
/// `HidingListBackend` are still exercised together, on real recovery-walk scenarios, elsewhere in this
/// file (search for `PutHookBackend>`).

/// The request sequence of writer recovery over an old-format `_ckpt`, a log above its frontier and two
/// epochs that never wrote the namespace: the walk to the first absent key, the stamp, one tombstone
/// that names the live epoch, and the final compare.
TEST(CASRefRecoveryCasWalk, RecoveryRequestSequenceStampsThenWritesOneTombstone)
{
    auto backend = std::make_shared<JournalingBackend>();
    const Layout layout("p");
    const RootNamespace ns{"srv1/request_sequence"};

    burnEpochsUpTo(backend, layout, /*target_live_epoch=*/4);
    seedCkpt(*backend, layout, ns, lifeEpochCkpt(1, RefTxnId{1, 2}));
    seedTxn(*backend, layout, ns, RefTxnId{1, 1}, "a", /*birth=*/true);
    seedTxn(*backend, layout, ns, RefTxnId{1, 2}, "b", /*birth=*/false);
    seedTxn(*backend, layout, ns, RefTxnId{1, 3}, "c", /*birth=*/false);

    auto store = openWalkPool(backend);
    ASSERT_TRUE(store);
    ASSERT_EQ(store->liveWriterEpoch(), 4u);
    const NamespaceLifeId life = catalogLife(backend, layout, ns);
    const String log_key = layout.refLogKey(life, RefTxnId{1, 1});
    const String stream_prefix = log_key.substr(0, log_key.find("_log/"));
    const String ckpt_key = layout.refCkptKey(life);

    backend->clearJournal();
    const uint64_t stamped_before = ProfileEvents::global_counters[ProfileEvents::CASRefCheckpointStamped];
    const uint64_t sealed_before = ProfileEvents::global_counters[ProfileEvents::CASRefRecoveryEpochSealed];
    const uint64_t lane_before = ProfileEvents::global_counters[ProfileEvents::CASRefAppendTombstoneWritten];
    EXPECT_EQ(store->listRefs(ns).size(), 3u);
    EXPECT_EQ(ProfileEvents::global_counters[ProfileEvents::CASRefCheckpointStamped] - stamped_before, 1u);
    EXPECT_EQ(ProfileEvents::global_counters[ProfileEvents::CASRefRecoveryEpochSealed] - sealed_before, 1u);
    EXPECT_EQ(ProfileEvents::global_counters[ProfileEvents::CASRefAppendTombstoneWritten] - lane_before, 0u);

    std::vector<std::pair<String, String>> requests;
    for (const auto & [method, key] : backend->journal())
    {
        if (key == ckpt_key)
            requests.emplace_back(method, "_ckpt");
        else if (key.starts_with(stream_prefix))
            requests.emplace_back(method, key.substr(stream_prefix.size()));
    }
    const std::vector<std::pair<String, String>> expected{
        {"GET", "_ckpt"},
        {"GET", "_log/0000000000000001-0000000000000001.zst"},
        {"GET", "_log/0000000000000001-0000000000000002.zst"},
        {"GET", "_log/0000000000000001-0000000000000003.zst"},
        {"GET", "_log/0000000000000001-0000000000000004.zst"},
        {"CAS", "_ckpt"},
        {"PUT", "_log/0000000000000001-0000000000000004.zst"},
        {"GET", "_ckpt"},
    };
    EXPECT_EQ(requests, expected);
    EXPECT_EQ(store->refRecoveryRestartsForTest(ns), 0u);
}

/// ---------------------------------------------------------------------------------------------
/// Generation 3: one-request append, forward-linked tombstones, uniform genesis
/// ---------------------------------------------------------------------------------------------

namespace
{

RefCkpt gen3Ckpt(uint64_t life_epoch, std::optional<RefTxnId> snapshot = std::nullopt)
{
    return RefCkpt{.life_epoch = life_epoch, .committed_through = std::nullopt,
                   .checkpoint_snapshot_id = snapshot, .last_epoch_seal = std::nullopt};
}

/// One positive append that publishes `ref`, prepending the birth on a never-born stream.
RefTxnId publishRefForTest(const PoolPtr & store, const RootNamespace & ns, const String & ref, ManifestRef manifest)
{
    return store->appendRefOps(ns, MutationScope::ref(ref),
        [ref, manifest](const RefTableState & state)
        {
            std::vector<RefOp> ops;
            if (state.getLifecycle() != RefLifecycle::Live)
                ops.push_back(namespaceBirthOp());
            for (const RefOp & op : publishCommittedOps(ref, manifest))
                ops.push_back(op);
            return ops;
        }, RootMutationOrigin::Writer, RootMutationKind::Publish);
}

/// Writes `txn` at its key with the header `encodeRefLogTxn` stamps.
void createTxnAt(Backend & backend, const Layout & layout, const NamespaceLifeId & life, const RefLogTxn & txn)
{
    createAt(backend, layout.refLogKey(life, txn.txn_id), sealObject(FormatId::RefLog, encodeRefLogTxn(txn)));
}

uint32_t storedLogHeader(Backend & backend, const Layout & layout, const NamespaceLifeId & life, RefTxnId id)
{
    const auto got = readAt(backend, layout.refLogKey(life, id));
    if (!got)
        return 0;
    const auto header = sniffHeaderLine(openObject(FormatId::RefLog, got->bytes));
    return header ? header->v : 0;
}

uint32_t storedCkptHeader(Backend & backend, const Layout & layout, const NamespaceLifeId & life)
{
    const auto got = readAt(backend, layout.refCkptKey(life));
    if (!got)
        return 0;
    const auto header = sniffHeaderLine(got->bytes);
    return header ? header->v : 0;
}

std::optional<RefLogTxn> readLogTxnOfLife(Backend & backend, const Layout & layout, const NamespaceLifeId & life, RefTxnId id)
{
    const auto got = readAt(backend, layout.refLogKey(life, id));
    if (!got)
        return std::nullopt;
    return decodeRefLogTxn(openObject(FormatId::RefLog, got->bytes), life.ns.string(), id);
}

bool isTombstoneTo(const std::optional<RefLogTxn> & txn, uint64_t next_epoch)
{
    return txn && refLogTxnIsEpochSeal(*txn) && txn->next_epoch == std::optional<uint64_t>{next_epoch};
}

/// Every log id of `life` whose epoch lies in `[from, to]`.
std::vector<RefTxnId> logIdsInEpochs(Backend & backend, const Layout & layout, const NamespaceLifeId & life,
                                     uint64_t from, uint64_t to)
{
    OperationForTest op(backend);
    std::vector<RefTxnId> ids;
    String cursor;
    do
    {
        const ListPage page = (*op).list(layout.namespaceStreamPrefix(life), cursor, 1000, Retry::standard());
        for (const ListedKey & listed : page.keys)
        {
            const auto parsed = layout.parseRefObjectKey(listed.key);
            if (parsed && parsed->kind == RefObjectKind::Log && parsed->txn_id.writer_epoch >= from
                && parsed->txn_id.writer_epoch <= to)
                ids.push_back(parsed->txn_id);
        }
        cursor = page.next_cursor;
    } while (!cursor.empty());
    return ids;
}

/// Allocates `n` durable writer epochs past whatever is already allocated.
void burnMoreEpochs(const BackendPtr & backend, const Layout & layout, uint64_t n)
{
    CasRequests requests(backend, Fence::open(), {}, {}, nullptr, DB::Cas::kDefaultRequestWindowMs);
    CasOperation op = requests.admit();
    for (uint64_t i = 0; i < n; ++i)
        allocateWriterEpoch(op, layout, "test", [] { return RefCatalog{}; });
}

/// Two logs `{1,1}` (birth of "a") and `{1,2}` ("b") under a generation 3 `_ckpt` of life epoch 1.
NamespaceLifeId seedTwoRecordChain(const BackendPtr & backend, const Layout & layout, const RootNamespace & ns)
{
    seedTxn(*backend, layout, ns, RefTxnId{1, 1}, "a", /*birth=*/true);
    seedTxn(*backend, layout, ns, RefTxnId{1, 2}, "b", /*birth=*/false);
    seedCkpt(*backend, layout, ns, gen3Ckpt(1), 3);
    return catalogLife(backend, layout, ns);
}

}

TEST(CASRefWriterTombstone, LaneStampMismatchNeedsRecovery)
{
    auto backend = std::make_shared<JournalingBackend>();
    const Layout layout("p");
    const RootNamespace ns{"srv1/lane_stamp_mismatch"};
    DB::Cas::tests::fixture::admitLive(*backend, layout, ns);
    seedCkpt(*backend, layout, ns, lifeEpochCkpt(1), 1);
    const NamespaceLifeId life = catalogLife(backend, layout, ns);
    const String ckpt_key = layout.refCkptKey(life);
    const String log_prefix = layout.namespaceStreamPrefix(life) + "_log/";
    const auto writes_to_ns = [&]
    {
        size_t n = 0;
        for (const auto & [method, key] : backend->journal())
            n += (method == "PUT" || method == "CAS") && (key == ckpt_key || key.starts_with(log_prefix)) ? 1 : 0;
        return n;
    };

    auto store = openWalkPool(backend);
    ASSERT_EQ(store->liveWriterEpoch(), 1u);
    backend->clearJournal();
    EXPECT_TRUE(store->listRefs(ns).empty());
    EXPECT_EQ(store->laneStateForTest(ns), RefLaneState::Ready);
    EXPECT_EQ(writes_to_ns(), 0u) << "a recovery whose chain is in the live epoch writes nothing";
    EXPECT_EQ(storedCkptHeader(*backend, layout, life), 1u);

    bool armed = true;
    backend->before_cas_put = [&](const String & key, const String &, const std::optional<String> &)
    {
        if (!armed || key != ckpt_key)
            return;
        armed = false;
        const auto current = readAt(*backend, ckpt_key);
        ASSERT_TRUE(current);
        ASSERT_TRUE(replaceForTest(backend, ckpt_key, encodeRefCkptAtGeneration(lifeEpochCkpt(1), 1), current->etag));
    };
    expectThrowsCode(DB::ErrorCodes::NETWORK_ERROR, [&] { publishRefForTest(store, ns, "a", manifestRef(1, 1, 1)); });
    backend->before_cas_put = nullptr;
    EXPECT_FALSE(armed);
    EXPECT_EQ(store->laneStateForTest(ns), RefLaneState::NeedsRecovery);
    EXPECT_TRUE(logIdsInEpochs(*backend, layout, life, 1, 1).empty()) << "no log is written before the stamp";

    backend->clearJournal();
    publishRefForTest(store, ns, "a", manifestRef(1, 1, 1));
    const auto journal = backend->journal();
    const auto stamp = std::find(journal.begin(), journal.end(), std::make_pair(String("CAS"), ckpt_key));
    const auto first_log = std::find_if(journal.begin(), journal.end(),
        [&](const auto & entry) { return entry.first == "PUT" && entry.second.starts_with(log_prefix); });
    ASSERT_NE(stamp, journal.end());
    ASSERT_NE(first_log, journal.end());
    EXPECT_LT(stamp - journal.begin(), first_log - journal.begin());
    EXPECT_EQ(storedCkptHeader(*backend, layout, life), 3u);
}

TEST(CASRefWriterTombstone, LaneStampUnresolvedNeedsRecovery)
{
    auto backend = std::make_shared<JournalingBackend>();
    const Layout layout("p");
    const RootNamespace ns{"srv1/lane_stamp_unresolved"};
    DB::Cas::tests::fixture::admitLive(*backend, layout, ns);
    seedCkpt(*backend, layout, ns, lifeEpochCkpt(1), 1);
    const NamespaceLifeId life = catalogLife(backend, layout, ns);

    auto store = openWalkPool(backend);
    ASSERT_EQ(store->liveWriterEpoch(), 1u);
    EXPECT_TRUE(store->listRefs(ns).empty());

    backend->ambiguous_cas_substr = layout.refCkptKey(life);
    backend->ambiguous_cas_count = 1;
    expectThrowsCode(DB::ErrorCodes::NETWORK_ERROR, [&] { publishRefForTest(store, ns, "a", manifestRef(1, 1, 1)); });
    EXPECT_EQ(backend->ambiguous_cas_count, 0);
    EXPECT_EQ(store->laneStateForTest(ns), RefLaneState::NeedsRecovery);
    EXPECT_TRUE(logIdsInEpochs(*backend, layout, life, 1, 1).empty());

    publishRefForTest(store, ns, "a", manifestRef(1, 1, 1));
    EXPECT_TRUE(store->listRefs(ns).contains("a"));
    EXPECT_EQ(storedCkptHeader(*backend, layout, life), 3u);
}

TEST(CASRefWriterTombstone, FirstAppendOfAnEpochWritesTombstoneThenHead)
{
    auto backend = std::make_shared<HidingListBackend>();
    const Layout layout("p");
    const RootNamespace ns{"srv1/tombstone_then_head"};
    burnEpochsUpTo(backend, layout, 4);
    const NamespaceLifeId life = seedTwoRecordChain(backend, layout, ns);

    auto store = openWalkPool(backend);
    ASSERT_EQ(store->liveWriterEpoch(), 4u);
    publishRefForTest(store, ns, "c", manifestRef(4, 1, 1));

    const auto tombstone = readLogTxn(*backend, layout, ns, RefTxnId{1, 3});
    EXPECT_TRUE(isTombstoneTo(tombstone, 4));
    EXPECT_EQ(storedLogHeader(*backend, layout, life, RefTxnId{1, 3}), 3u);
    const auto head = readLogTxn(*backend, layout, ns, RefTxnId{4, 1});
    ASSERT_TRUE(head);
    EXPECT_EQ(head->prev_epoch_seal, std::make_optional(RefTxnId{1, 3}));
    EXPECT_EQ(storedLogHeader(*backend, layout, life, RefTxnId{4, 1}), 3u);
    EXPECT_TRUE(logIdsInEpochs(*backend, layout, life, 2, 3).empty());
}

TEST(CASRefWriterAppendLane, FirstHeadAfterATombstonePreviewsAndAppendsWithItsBacklink)
{
    auto backend = std::make_shared<HidingListBackend>();
    const Layout layout("p");
    const RootNamespace ns{"srv1/head_previews"};
    burnEpochsUpTo(backend, layout, 4);
    seedTwoRecordChain(backend, layout, ns);

    auto store = openWalkPool(backend);
    EXPECT_NO_THROW(publishRefForTest(store, ns, "c", manifestRef(4, 1, 1)));
    EXPECT_NO_THROW(publishRefForTest(store, ns, "d", manifestRef(4, 2, 1)));
    const auto second = readLogTxn(*backend, layout, ns, RefTxnId{4, 2});
    ASSERT_TRUE(second);
    EXPECT_EQ(second->prev_epoch_seal, std::nullopt);
    EXPECT_TRUE(store->listRefs(ns).contains("d"));
}

TEST(CASRefWriterTombstone, BacklinkSkipsEpochsThatNeverOpenedTheNamespace)
{
    auto backend = std::make_shared<HidingListBackend>();
    const Layout layout("p");
    const RootNamespace ns{"srv1/backlink_skips"};
    burnEpochsUpTo(backend, layout, 6);
    const NamespaceLifeId life = seedTwoRecordChain(backend, layout, ns);

    auto store = openWalkPool(backend);
    ASSERT_EQ(store->liveWriterEpoch(), 6u);
    publishRefForTest(store, ns, "c", manifestRef(6, 1, 1));

    EXPECT_TRUE(isTombstoneTo(readLogTxn(*backend, layout, ns, RefTxnId{1, 3}), 6));
    const auto head = readLogTxn(*backend, layout, ns, RefTxnId{6, 1});
    ASSERT_TRUE(head);
    EXPECT_EQ(head->prev_epoch_seal, std::make_optional(RefTxnId{1, 3}));
    EXPECT_TRUE(logIdsInEpochs(*backend, layout, life, 2, 5).empty());
}

TEST(CASRefWriterTombstone, TombstoneOnlyEpochIsClosedAtItsFirstSlot)
{
    auto backend = std::make_shared<HidingListBackend>();
    const Layout layout("p");
    const RootNamespace ns{"srv1/tombstone_only_epoch"};
    burnEpochsUpTo(backend, layout, 6);
    const NamespaceLifeId life = seedTwoRecordChain(backend, layout, ns);
    createTxnAt(*backend, layout, life, tombstoneTxn(ns.string(), RefTxnId{1, 3}, 4));

    auto store = openWalkPool(backend);
    ASSERT_EQ(store->liveWriterEpoch(), 6u);
    publishRefForTest(store, ns, "c", manifestRef(6, 1, 1));

    const auto closing = readLogTxn(*backend, layout, ns, RefTxnId{4, 1});
    EXPECT_TRUE(isTombstoneTo(closing, 6));
    ASSERT_TRUE(closing);
    EXPECT_EQ(closing->prev_epoch_seal, std::make_optional(RefTxnId{1, 3}));
    const auto head = readLogTxn(*backend, layout, ns, RefTxnId{6, 1});
    ASSERT_TRUE(head);
    EXPECT_EQ(head->prev_epoch_seal, std::make_optional(RefTxnId{4, 1}));
}

TEST(CASRefWriterTombstone, LatePredecessorAppendIsAdoptedAndTheTombstoneMovesOneSlot)
{
    auto backend = std::make_shared<LateMaterializeBackend>();
    const Layout layout("p");
    const RootNamespace ns{"srv1/late_append_adopted"};
    burnEpochsUpTo(backend, layout, 4);
    const NamespaceLifeId life = seedTwoRecordChain(backend, layout, ns);
    backend->late_key = layout.refLogKey(life, RefTxnId{1, 3});
    backend->late_bytes = sealObject(FormatId::RefLog,
        encodeRefLogTxn(makeOrdinaryTxn(ns, RefTxnId{1, 3}, "late", /*birth=*/false)));

    auto store = openWalkPool(backend);
    const auto refs = store->listRefs(ns);
    EXPECT_TRUE(backend->late_key.empty()) << "the late append must land between the read and the create";
    EXPECT_TRUE(refs.contains("late"));
    EXPECT_TRUE(isTombstoneTo(readLogTxn(*backend, layout, ns, RefTxnId{1, 4}), 4));
}

TEST(CASRefWriterTombstone, LatePredecessorTombstoneIsAdoptedAndTheNextOneLinksToIt)
{
    auto backend = std::make_shared<LateMaterializeBackend>();
    const Layout layout("p");
    const RootNamespace ns{"srv1/late_tombstone_adopted"};
    burnEpochsUpTo(backend, layout, 6);
    const NamespaceLifeId life = seedTwoRecordChain(backend, layout, ns);
    backend->late_key = layout.refLogKey(life, RefTxnId{1, 3});
    backend->late_bytes = sealObject(FormatId::RefLog, encodeRefLogTxn(tombstoneTxn(ns.string(), RefTxnId{1, 3}, 4)));

    auto store = openWalkPool(backend);
    ASSERT_EQ(store->listRefs(ns).size(), 2u);
    EXPECT_TRUE(backend->late_key.empty());
    EXPECT_TRUE(isTombstoneTo(readLogTxn(*backend, layout, ns, RefTxnId{1, 3}), 4));
    const auto next = readLogTxn(*backend, layout, ns, RefTxnId{4, 1});
    EXPECT_TRUE(isTombstoneTo(next, 6));
    ASSERT_TRUE(next);
    EXPECT_EQ(next->prev_epoch_seal, std::make_optional(RefTxnId{1, 3}));
}

TEST(CASRefWriterTombstone, NewerTombstoneClosesTheLane)
{
    auto backend = std::make_shared<HidingListBackend>();
    const Layout layout("p");
    const RootNamespace ns{"srv1/newer_tombstone_append"};
    burnEpochsUpTo(backend, layout, 4);
    const NamespaceLifeId life = seedTwoRecordChain(backend, layout, ns);

    auto store = openWalkPool(backend);
    ASSERT_EQ(store->liveWriterEpoch(), 4u);
    publishRefForTest(store, ns, "c", manifestRef(4, 1, 1));
    ASSERT_TRUE(readLogTxn(*backend, layout, ns, RefTxnId{4, 1}));
    createTxnAt(*backend, layout, life, tombstoneTxn(ns.string(), RefTxnId{4, 2}, 7));

    expectThrowsCode(DB::ErrorCodes::INVALID_STATE, [&] { publishRefForTest(store, ns, "d", manifestRef(4, 2, 1)); });
    EXPECT_EQ(store->laneStateForTest(ns), RefLaneState::Closed);

    const RootNamespace recovered_ns{"srv1/newer_tombstone_recovery"};
    const NamespaceLifeId recovered_life = seedTwoRecordChain(backend, layout, recovered_ns);
    createTxnAt(*backend, layout, recovered_life, tombstoneTxn(recovered_ns.string(), RefTxnId{1, 3}, 7));
    const uint64_t writes = backend->writeTotal();
    EXPECT_EQ(store->listRefs(recovered_ns).size(), 2u);
    EXPECT_EQ(store->laneStateForTest(recovered_ns), RefLaneState::Closed);
    EXPECT_EQ(backend->writeTotal(), writes) << "a recovery that finds the chain in a later epoch writes nothing";
}

TEST(CASRefWriterTombstone, TerminalOccupantEndsTheStream)
{
    auto backend = std::make_shared<LateMaterializeBackend>();
    const Layout layout("p");
    const RootNamespace ns{"srv1/terminal_occupant"};
    burnEpochsUpTo(backend, layout, 4);
    const NamespaceLifeId life = seedTwoRecordChain(backend, layout, ns);
    RefLogTxn removal;
    removal.ns = ns.string();
    removal.txn_id = RefTxnId{1, 3};
    removal.ops = {DB::Cas::tests::ownerTransitionOp(RefOwnerBinding{RefOwnerKind::Committed, "a", manifestRef(1, 1, 1u)}, std::nullopt),
                   DB::Cas::tests::ownerTransitionOp(RefOwnerBinding{RefOwnerKind::Committed, "b", manifestRef(1, 2, 1u)}, std::nullopt),
                   removeNamespaceOp()};
    backend->late_key = layout.refLogKey(life, RefTxnId{1, 3});
    backend->late_bytes = sealObject(FormatId::RefLog, encodeRefLogTxn(removal));

    auto store = openWalkPool(backend);
    EXPECT_TRUE(store->listRefs(ns).empty());
    EXPECT_TRUE(backend->late_key.empty());
    EXPECT_TRUE(store->refTableRecoveredForTest(ns));
    const auto occupant = readLogTxn(*backend, layout, ns, RefTxnId{1, 3});
    ASSERT_TRUE(occupant);
    EXPECT_FALSE(refLogTxnIsEpochSeal(*occupant));
    EXPECT_FALSE(readLogTxn(*backend, layout, ns, RefTxnId{1, 4})) << "no key after a terminal record";
    EXPECT_TRUE(logIdsInEpochs(*backend, layout, life, 2, 4).empty());
}

TEST(CASRefWriterRecovery, DeadEpochIsSealedInOneWriteWithoutACkptWrite)
{
    auto backend = std::make_shared<HidingListBackend>();
    const Layout layout("p");
    const RootNamespace ns{"srv1/dead_epoch_one_write"};
    burnEpochsUpTo(backend, layout, 4);
    seedTxn(*backend, layout, ns, RefTxnId{1, 1}, "a", /*birth=*/true);
    seedTxn(*backend, layout, ns, RefTxnId{1, 2}, "b", /*birth=*/false);
    writeRefSnapshotRaw(*backend, layout, minimalLiveSnapshot(ns.string(), RefTxnId{1, 2},
        {committedRow("a", manifestRef(1, 1, 1)), committedRow("b", manifestRef(1, 2, 1))}));
    seedCkpt(*backend, layout, ns, gen3Ckpt(1, RefTxnId{1, 2}), 3);
    const NamespaceLifeId life = catalogLife(backend, layout, ns);
    const String ckpt_key = layout.refCkptKey(life);

    auto store = openWalkPool(backend);
    ASSERT_EQ(store->liveWriterEpoch(), 4u);
    backend->resetCounts();
    EXPECT_EQ(store->listRefs(ns).size(), 2u);

    EXPECT_EQ(backend->getCount(ckpt_key), 2u);
    EXPECT_EQ(backend->getCount(layout.refLogKey(life, RefTxnId{1, 2})), 1u);
    EXPECT_EQ(backend->getCount(layout.refSnapshotKey(life, RefTxnId{1, 2})), 1u);
    EXPECT_EQ(backend->getCount(layout.refLogKey(life, RefTxnId{1, 3})), 1u);
    EXPECT_EQ(backend->writeCount(layout.refLogKey(life, RefTxnId{1, 3})), 1u);
    EXPECT_EQ(backend->getCount(layout.refCatalogKey()), 3u) << "two of the cold admission, one confirmation";
    EXPECT_EQ(backend->writeCount(ckpt_key), 0u);
    EXPECT_TRUE(isTombstoneTo(readLogTxn(*backend, layout, ns, RefTxnId{1, 3}), 4));
}

TEST(CASRefWriterRecovery, UnresolvedTombstoneInstallsNothing)
{
    auto backend = std::make_shared<HidingListBackend>();
    const Layout layout("p");
    const RootNamespace ns{"srv1/unresolved_tombstone"};
    burnEpochsUpTo(backend, layout, 2);
    seedTxn(*backend, layout, ns, RefTxnId{1, 1}, "a", /*birth=*/true);
    seedCkpt(*backend, layout, ns, gen3Ckpt(1), 3);
    const NamespaceLifeId life = catalogLife(backend, layout, ns);

    auto fake_now = std::make_shared<std::atomic<uint64_t>>(1'000'000);
    PoolConfig config = walkTestConfig();
    config.boot_ms_fn = [fake_now] { return fake_now->load(); };
    auto store = openWalkPool(backend, config);
    store->setCasRetrySleepForTest([fake_now](uint64_t ms) { *fake_now += ms; });
    backend->ambiguous_put_substr = layout.refLogKey(life, RefTxnId{1, 2});

    expectThrowsCode(DB::ErrorCodes::NETWORK_ERROR, [&] { (void)store->listRefs(ns); });
    EXPECT_FALSE(store->refTableRecoveredForTest(ns));
    EXPECT_GE(backend->ambiguous_put_attempts.load(), 1u);
}

TEST(CASRefWriterRecovery, StampsCkptBeforeTheFirstNewWrite)
{
    auto backend = std::make_shared<JournalingBackend>();
    const Layout layout("p");
    burnEpochsUpTo(backend, layout, 4);

    const RootNamespace framed{"srv1/stamp_framed"};
    seedTxn(*backend, layout, framed, RefTxnId{1, 1}, "a", /*birth=*/true);
    seedTxn(*backend, layout, framed, RefTxnId{1, 2}, "b", /*birth=*/false);
    seedCkpt(*backend, layout, framed, lifeEpochCkpt(1, RefTxnId{1, 2}), 1);
    const NamespaceLifeId framed_life = catalogLife(backend, layout, framed);

    const RootNamespace bare{"srv1/stamp_bare"};
    seedTxn(*backend, layout, bare, RefTxnId{1, 1}, "a", /*birth=*/true);
    seedCkpt(*backend, layout, bare, lifeEpochCkpt(1), 1);
    const NamespaceLifeId bare_life = catalogLife(backend, layout, bare);

    auto store = openWalkPool(backend);
    ASSERT_EQ(store->liveWriterEpoch(), 4u);
    backend->clearJournal();
    EXPECT_EQ(store->listRefs(framed).size(), 2u);

    const auto journal = backend->journal();
    const String ckpt_key = layout.refCkptKey(framed_life);
    const auto stamp = std::find(journal.begin(), journal.end(), std::make_pair(String("CAS"), ckpt_key));
    const auto tombstone = std::find(journal.begin(), journal.end(),
        std::make_pair(String("PUT"), layout.refLogKey(framed_life, RefTxnId{1, 3})));
    ASSERT_NE(stamp, journal.end());
    ASSERT_NE(tombstone, journal.end());
    EXPECT_LT(stamp - journal.begin(), tombstone - journal.begin());
    EXPECT_EQ(storedCkptHeader(*backend, layout, framed_life), 3u);
    const auto stamped = readCkptForTest(backend, layout, framed_life);
    ASSERT_TRUE(stamped);
    RefCkpt expected = gen3Ckpt(1);
    expected.generation = 3;
    EXPECT_EQ(stamped->ckpt, expected) << "the stamp keeps life_epoch and drops the old frontier";

    EXPECT_EQ(store->listRefs(bare).size(), 1u);
    EXPECT_EQ(storedCkptHeader(*backend, layout, bare_life), 3u) << "the stamp compares the generation, not the fields";
}

TEST(CASRefWriterRecovery, RestartsWhenTheBaseMovesUnderTheWalk)
{
    auto backend = std::make_shared<GetSeamBackend>();
    const Layout layout("p");
    const RootNamespace ns{"srv1/base_moves_under_walk"};
    const NamespaceLifeId life = seedTwoRecordChain(backend, layout, ns);
    backend->watched_substr = layout.refLogKey(life, RefTxnId{1, 3});
    backend->on_key = [&](const String &)
    {
        writeRefSnapshotRaw(*backend, layout, minimalLiveSnapshot(ns.string(), RefTxnId{1, 2},
            {committedRow("a", manifestRef(1, 1, 1)), committedRow("b", manifestRef(1, 2, 1))}));
        const auto current = readCkptForTest(backend, layout, life);
        ASSERT_TRUE(current);
        ASSERT_TRUE(replaceForTest(backend, layout.refCkptKey(life),
            encodeRefCkptAtGeneration(gen3Ckpt(1, RefTxnId{1, 2}), 3), current->etag));
    };

    auto store = openWalkPool(backend);
    ASSERT_EQ(store->liveWriterEpoch(), 1u);
    const auto refs = store->listRefs(ns);
    EXPECT_FALSE(backend->on_key);
    EXPECT_EQ(store->refRecoveryRestartsForTest(ns), 1u);
    EXPECT_EQ(refs.size(), 2u);
    EXPECT_TRUE(refs.contains("a"));
    EXPECT_TRUE(refs.contains("b"));
    EXPECT_EQ(store->newestPublishedSnapshotIdForTest(ns), std::make_optional(RefTxnId{1, 2}));
}

TEST(CASRefWriterRecovery, RestartsWhenTheBaseMovesUnderTheTombstoneCreate)
{
    const Layout layout("p");
    {
        auto backend = std::make_shared<AfterGetHookBackend>();
        const RootNamespace ns{"srv1/base_moves_under_tombstone"};
        burnEpochsUpTo(backend, layout, 4);
        const NamespaceLifeId life = seedTwoRecordChain(backend, layout, ns);
        size_t fired = 0;
        backend->watched_key = layout.refLogKey(life, RefTxnId{1, 3});
        backend->after_get = [&]
        {
            ++fired;
            createTxnAt(*backend, layout, life, makeOrdinaryTxn(ns, RefTxnId{1, 3}, "c", /*birth=*/false));
            createTxnAt(*backend, layout, life, makeOrdinaryTxn(ns, RefTxnId{1, 4}, "d", /*birth=*/false));
            writeRefSnapshotRaw(*backend, layout, minimalLiveSnapshot(ns.string(), RefTxnId{1, 4},
                {committedRow("a", manifestRef(1, 1, 1)), committedRow("b", manifestRef(1, 2, 1)),
                 committedRow("c", manifestRef(1, 3, 1)), committedRow("d", manifestRef(1, 4, 1))}));
            const auto current = readCkptForTest(backend, layout, life);
            ASSERT_TRUE(current);
            ASSERT_TRUE(replaceForTest(backend, layout.refCkptKey(life),
                encodeRefCkptAtGeneration(gen3Ckpt(1, RefTxnId{1, 4}), 3), current->etag));
            OperationForTest op(*backend);
            ASSERT_EQ((*op).removeCurrent(layout.refLogKey(life, RefTxnId{1, 3}), Retry::once()), Removal::Removed);
        };

        auto store = openWalkPool(backend);
        ASSERT_EQ(store->liveWriterEpoch(), 4u);
        const auto refs = store->listRefs(ns);
        EXPECT_EQ(fired, 1u);
        EXPECT_EQ(store->refRecoveryRestartsForTest(ns), 1u);
        EXPECT_TRUE(refs.contains("c"));
        EXPECT_TRUE(refs.contains("d"));
        EXPECT_TRUE(isTombstoneTo(readLogTxn(*backend, layout, ns, RefTxnId{1, 5}), 4));
    }
    {
        auto backend = std::make_shared<AfterGetHookBackend>();
        const RootNamespace ns{"srv1/base_stays_under_tombstone"};
        burnEpochsUpTo(backend, layout, 4);
        seedTwoRecordChain(backend, layout, ns);
        auto store = openWalkPool(backend);
        EXPECT_EQ(store->listRefs(ns).size(), 2u);
        EXPECT_EQ(store->refRecoveryRestartsForTest(ns), 0u);
        EXPECT_TRUE(isTombstoneTo(readLogTxn(*backend, layout, ns, RefTxnId{1, 3}), 4));
    }
}

TEST(CASRefWriterRecovery, FollowsOldSealsByPlusOneAndLinksAfterThem)
{
    auto backend = std::make_shared<HidingListBackend>();
    const Layout layout("p");
    const RootNamespace ns{"srv1/old_seals_then_tombstone"};
    burnEpochsUpTo(backend, layout, 5);
    seedTxn(*backend, layout, ns, RefTxnId{1, 1}, "a", /*birth=*/true);
    seedCkpt(*backend, layout, ns, lifeEpochCkpt(1), 1);
    const NamespaceLifeId life = catalogLife(backend, layout, ns);
    createAt(*backend, layout.refLogKey(life, RefTxnId{1, 2}),
        sealObject(FormatId::RefLog, encodeRefLogTxnAtGeneration(makeSealTxn(ns, RefTxnId{1, 2}), 1)));
    createAt(*backend, layout.refLogKey(life, RefTxnId{2, 1}),
        sealObject(FormatId::RefLog, encodeRefLogTxnAtGeneration(makeSealTxn(ns, RefTxnId{2, 1}, RefTxnId{1, 2}), 1)));

    auto store = openWalkPool(backend);
    ASSERT_EQ(store->liveWriterEpoch(), 5u);
    publishRefForTest(store, ns, "b", manifestRef(5, 1, 1));

    const auto closing = readLogTxn(*backend, layout, ns, RefTxnId{3, 1});
    EXPECT_TRUE(isTombstoneTo(closing, 5));
    ASSERT_TRUE(closing);
    EXPECT_EQ(closing->prev_epoch_seal, std::make_optional(RefTxnId{2, 1}));
    const auto head = readLogTxn(*backend, layout, ns, RefTxnId{5, 1});
    ASSERT_TRUE(head);
    EXPECT_EQ(head->prev_epoch_seal, std::make_optional(RefTxnId{3, 1}));
}

TEST(CASRefWriterGenesis, BirthInALaterEpochSealsTheCreationSlotAndWritesNoCkpt)
{
    auto backend = std::make_shared<HidingListBackend>();
    const Layout layout("p");
    const RootNamespace ns{"srv1/birth_in_later_epoch"};
    burnEpochsUpTo(backend, layout, 2);
    NamespaceLifeId life = DB::Cas::tests::fixture::fixtureLife(ns);
    {
        auto creator = openWalkPool(backend);
        ASSERT_EQ(creator->liveWriterEpoch(), 2u);
        life = creator->namespaceLife(ns);
    }
    EXPECT_EQ(storedCkptHeader(*backend, layout, life), 3u);
    const auto created = readCkptForTest(backend, layout, life);
    ASSERT_TRUE(created);
    EXPECT_EQ(created->ckpt.life_epoch, std::optional<uint64_t>{2});
    burnMoreEpochs(backend, layout, 2);
    const uint64_t ckpt_writes = backend->writeCount(layout.refCkptKey(life));

    auto store = openWalkPool(backend);
    ASSERT_EQ(store->liveWriterEpoch(), 5u);
    publishRefForTest(store, ns, "a", manifestRef(5, 1, 1));

    const auto genesis = readLogTxnOfLife(*backend, layout, life, RefTxnId{2, 1});
    EXPECT_TRUE(isTombstoneTo(genesis, 5));
    ASSERT_TRUE(genesis);
    EXPECT_EQ(genesis->prev_epoch_seal, std::nullopt);
    const auto birth = readLogTxnOfLife(*backend, layout, life, RefTxnId{5, 1});
    ASSERT_TRUE(birth);
    EXPECT_EQ(birth->prev_epoch_seal, std::make_optional(RefTxnId{2, 1}));
    EXPECT_EQ(backend->writeCount(layout.refCkptKey(life)), ckpt_writes);
    EXPECT_EQ(readCkptForTest(backend, layout, life)->ckpt.life_epoch, std::optional<uint64_t>{2});
}

TEST(CASRefWriterGenesis, LateBirthOfAPredecessorIsAdoptedOrRefused)
{
    const Layout layout("p");
    {
        auto backend = std::make_shared<LateMaterializeBackend>();
        const RootNamespace ns{"srv1/late_birth_adopted"};
        burnEpochsUpTo(backend, layout, 5);
        DB::Cas::tests::fixture::admitLive(*backend, layout, ns);
        seedCkpt(*backend, layout, ns, gen3Ckpt(2), 3);
        const NamespaceLifeId life = catalogLife(backend, layout, ns);
        backend->late_key = layout.refLogKey(life, RefTxnId{2, 1});
        backend->late_bytes = sealObject(FormatId::RefLog,
            encodeRefLogTxn(makeOrdinaryTxn(ns, RefTxnId{2, 1}, "late", /*birth=*/true)));

        auto store = openWalkPool(backend);
        ASSERT_EQ(store->liveWriterEpoch(), 5u);
        EXPECT_TRUE(store->listRefs(ns).contains("late"));
        EXPECT_TRUE(backend->late_key.empty());
        EXPECT_TRUE(isTombstoneTo(readLogTxn(*backend, layout, ns, RefTxnId{2, 2}), 5));
    }
    {
        auto backend = std::make_shared<HidingListBackend>();
        const RootNamespace ns{"srv1/late_birth_refused"};
        burnEpochsUpTo(backend, layout, 2);
        DB::Cas::tests::fixture::admitLive(*backend, layout, ns);
        seedCkpt(*backend, layout, ns, gen3Ckpt(2), 3);
        const NamespaceLifeId life = catalogLife(backend, layout, ns);

        auto store = openWalkPool(backend);
        ASSERT_EQ(store->liveWriterEpoch(), 2u);
        EXPECT_TRUE(store->listRefs(ns).empty());
        createTxnAt(*backend, layout, life, tombstoneTxn(ns.string(), RefTxnId{2, 1}, 5));
        expectThrowsCode(DB::ErrorCodes::INVALID_STATE, [&] { publishRefForTest(store, ns, "late", manifestRef(2, 1, 1)); });
        EXPECT_EQ(store->laneStateForTest(ns), RefLaneState::Closed);
        EXPECT_TRUE(isTombstoneTo(readLogTxn(*backend, layout, ns, RefTxnId{2, 1}), 5));
    }
}

TEST(CASRefWriterGenesis, RecreatedNamespaceStartsItsOwnChain)
{
    auto backend = std::make_shared<HidingListBackend>();
    const Layout layout("p");
    const RootNamespace ns{"srv1/recreated"};
    burnEpochsUpTo(backend, layout, 2);

    NamespaceLifeId first = DB::Cas::tests::fixture::fixtureLife(ns);
    {
        auto store = openWalkPool(backend);
        ASSERT_EQ(store->liveWriterEpoch(), 2u);
        publishRefForTest(store, ns, "a", manifestRef(2, 1, 1));
        first = catalogLife(backend, layout, ns);
        store->dropNamespace(ns);
    }
    const auto snapshotOfLife = [&](const NamespaceLifeId & life)
    {
        std::map<String, String> objects;
        OperationForTest op(*backend);
        String cursor;
        do
        {
            const ListPage page = (*op).list(layout.namespaceStreamPrefix(life), cursor, 1000, Retry::standard());
            for (const ListedKey & listed : page.keys)
                objects[listed.key] = readAt(*backend, listed.key).value().bytes;
            cursor = page.next_cursor;
        } while (!cursor.empty());
        if (const auto ckpt = readAt(*backend, layout.refCkptKey(life)))
            objects[layout.refCkptKey(life)] = ckpt->bytes;
        return objects;
    };
    const auto first_objects = snapshotOfLife(first);
    ASSERT_FALSE(first_objects.empty());

    /// What the GC erase does to the catalog row once the terminal record is folded.
    {
        const CasRefCatalog::Snapshot catalog = readCatalogForTest(backend, layout);
        RefCatalog without = catalog.catalog;
        std::erase_if(without.entries, [&](const CatalogEntry & entry) { return entry.ns == ns; });
        ASSERT_TRUE(catalog.etag);
        ASSERT_TRUE(replaceForTest(backend, layout.refCatalogKey(), encodeRefCatalog(without), *catalog.etag));
    }
    burnMoreEpochs(backend, layout, 1);

    auto store = openWalkPool(backend);
    ASSERT_EQ(store->liveWriterEpoch(), 4u);
    publishRefForTest(store, ns, "b", manifestRef(4, 1, 1));
    const NamespaceLifeId second = catalogLife(backend, layout, ns);
    EXPECT_NE(second.incarnation, first.incarnation);
    EXPECT_NE(layout.refCkptKey(second), layout.refCkptKey(first));
    EXPECT_EQ(storedCkptHeader(*backend, layout, second), 3u);
    EXPECT_EQ(readCkptForTest(backend, layout, second)->ckpt.life_epoch, std::optional<uint64_t>{4});
    const auto first_log = readAt(*backend, layout.refLogKey(second, RefTxnId{4, 1}));
    ASSERT_TRUE(first_log);
    const RefLogTxn birth = decodeRefLogTxn(openObject(FormatId::RefLog, first_log->bytes), ns.string(), RefTxnId{4, 1});
    EXPECT_EQ(birth.prev_epoch_seal, std::nullopt);
    EXPECT_EQ(snapshotOfLife(first), first_objects) << "no key of the first life changed";
}

namespace
{

/// Chain `{1,1}`, `{1,2}`, `T{1,3}->4`, live epoch 6: recovery's tombstone goes to `{4,1}` with backlink
/// `{1,3}`. Right after the walk reads `{4,1}` absent, `H{4,1}<-{1,4}` lands there, a head that names
/// another tombstone, or with `undecodable` a transaction of id `{4,2}`, which does not decode at that key.
/// With `move_base`, a stray `T{1,4}->4`, `{4,2}` and a snapshot at `{4,2}` land too, and `_ckpt` is
/// replaced to name that snapshot.
struct NonChainingOccupant
{
    std::shared_ptr<AfterGetHookBackend> backend = std::make_shared<AfterGetHookBackend>();
    Layout layout{"p"};
    RootNamespace ns;
    NamespaceLifeId life = DB::Cas::tests::fixture::fixtureLife(ns);
    size_t fired = 0;

    NonChainingOccupant(const String & name, bool move_base, bool undecodable = false)
        : ns(name)
    {
        burnEpochsUpTo(backend, layout, 6);
        life = seedTwoRecordChain(backend, layout, ns);
        createTxnAt(*backend, layout, life, tombstoneTxn(ns.string(), RefTxnId{1, 3}, 4));
        backend->watched_key = layout.refLogKey(life, RefTxnId{4, 1});
        backend->after_get = [this, move_base, undecodable]
        {
            ++fired;
            if (move_base)
                createTxnAt(*backend, layout, life, tombstoneTxn(ns.string(), RefTxnId{1, 4}, 4));
            if (undecodable)
                createAt(*backend, layout.refLogKey(life, RefTxnId{4, 1}), sealObject(FormatId::RefLog,
                    encodeRefLogTxn(makeOrdinaryTxn(ns, RefTxnId{4, 2}, "x", /*birth=*/false))));
            else
                createTxnAt(*backend, layout, life, makeOrdinaryTxn(ns, RefTxnId{4, 1}, "c", /*birth=*/false, RefTxnId{1, 4}));
            if (!move_base)
                return;
            createTxnAt(*backend, layout, life, makeOrdinaryTxn(ns, RefTxnId{4, 2}, "d", /*birth=*/false));
            writeRefSnapshotRaw(*backend, layout, minimalLiveSnapshot(ns.string(), RefTxnId{4, 2},
                {committedRow("a", manifestRef(1, 1, 1)), committedRow("b", manifestRef(1, 2, 1)),
                 committedRow("c", manifestRef(4, 1, 1)), committedRow("d", manifestRef(4, 2, 1))}));
            const auto current = readCkptForTest(backend, layout, life);
            ASSERT_TRUE(current);
            ASSERT_TRUE(replaceForTest(backend, layout.refCkptKey(life),
                encodeRefCkptAtGeneration(gen3Ckpt(1, RefTxnId{4, 2}), 3), current->etag));
        };
    }
};

}

TEST(CASRefWriterRecovery, OccupantThatDoesNotChainRestartsWhenTheBaseMoved)
{
    NonChainingOccupant fixture("srv1/non_chaining_occupant_base_moved", /*move_base=*/true);
    auto store = openWalkPool(fixture.backend);
    ASSERT_EQ(store->liveWriterEpoch(), 6u);

    std::map<String, Resolved> refs;
    ASSERT_NO_THROW(refs = store->listRefs(fixture.ns));
    EXPECT_EQ(fixture.fired, 1u);
    EXPECT_EQ(store->refRecoveryRestartsForTest(fixture.ns), 1u);
    EXPECT_TRUE(refs.contains("c"));
    EXPECT_TRUE(refs.contains("d"));
    EXPECT_TRUE(isTombstoneTo(readLogTxn(*fixture.backend, fixture.layout, fixture.ns, RefTxnId{4, 3}), 6))
        << "the restarted walk closes the chain above the new base";
}

TEST(CASRefWriterRecovery, OccupantThatDoesNotChainIsCorruptionUnderAnUnchangedBase)
{
    NonChainingOccupant fixture("srv1/non_chaining_occupant_same_base", /*move_base=*/false);
    auto store = openWalkPool(fixture.backend);
    ASSERT_EQ(store->liveWriterEpoch(), 6u);

    expectThrowsCode(DB::ErrorCodes::CORRUPTED_DATA, [&] { (void)store->listRefs(fixture.ns); });
    EXPECT_EQ(fixture.fired, 1u);
    EXPECT_EQ(store->refRecoveryRestartsForTest(fixture.ns), 0u) << "an unchanged base is a verdict, not a restart";
    EXPECT_FALSE(store->refTableRecoveredForTest(fixture.ns));
}

TEST(CASRefWriterRecovery, OccupantThatDoesNotDecodeRestartsWhenTheBaseMoved)
{
    NonChainingOccupant fixture("srv1/undecodable_occupant_base_moved", /*move_base=*/true, /*undecodable=*/true);
    auto store = openWalkPool(fixture.backend);
    ASSERT_EQ(store->liveWriterEpoch(), 6u);

    std::map<String, Resolved> refs;
    ASSERT_NO_THROW(refs = store->listRefs(fixture.ns));
    EXPECT_EQ(fixture.fired, 1u);
    EXPECT_EQ(store->refRecoveryRestartsForTest(fixture.ns), 1u);
    EXPECT_TRUE(refs.contains("d"));
    EXPECT_TRUE(isTombstoneTo(readLogTxn(*fixture.backend, fixture.layout, fixture.ns, RefTxnId{4, 3}), 6));
}

TEST(CASRefWriterRecovery, OccupantThatDoesNotDecodeIsCorruptionUnderAnUnchangedBase)
{
    NonChainingOccupant fixture("srv1/undecodable_occupant_same_base", /*move_base=*/false, /*undecodable=*/true);
    auto store = openWalkPool(fixture.backend);
    ASSERT_EQ(store->liveWriterEpoch(), 6u);

    expectThrowsCode(DB::ErrorCodes::CORRUPTED_DATA, [&] { (void)store->listRefs(fixture.ns); });
    EXPECT_EQ(fixture.fired, 1u);
    EXPECT_EQ(store->refRecoveryRestartsForTest(fixture.ns), 0u);
    EXPECT_FALSE(store->refTableRecoveredForTest(fixture.ns));
}

namespace
{

/// Chain `{1,1}`, `{1,2}`, then `{1,3}`, a record that chains but carries a second birth, so applying it
/// fails. With `move_base`, right after the walk reads `{1,3}`, a snapshot at `{1,3}` lands and `_ckpt`
/// is replaced to name it.
struct UnappliableWalkedRecord
{
    std::shared_ptr<AfterGetHookBackend> backend = std::make_shared<AfterGetHookBackend>();
    Layout layout{"p"};
    RootNamespace ns;
    NamespaceLifeId life = DB::Cas::tests::fixture::fixtureLife(ns);
    size_t fired = 0;

    UnappliableWalkedRecord(const String & name, bool move_base)
        : ns(name)
    {
        burnEpochsUpTo(backend, layout, 6);
        life = seedTwoRecordChain(backend, layout, ns);
        createTxnAt(*backend, layout, life, makeOrdinaryTxn(ns, RefTxnId{1, 3}, "c", /*birth=*/true));
        backend->watched_key = layout.refLogKey(life, RefTxnId{1, 3});
        backend->after_get = [this, move_base]
        {
            ++fired;
            if (!move_base)
                return;
            writeRefSnapshotRaw(*backend, layout, minimalLiveSnapshot(ns.string(), RefTxnId{1, 3},
                {committedRow("a", manifestRef(1, 1, 1)), committedRow("b", manifestRef(1, 2, 1)),
                 committedRow("c", manifestRef(1, 3, 1))}));
            const auto current = readCkptForTest(backend, layout, life);
            ASSERT_TRUE(current);
            ASSERT_TRUE(replaceForTest(backend, layout.refCkptKey(life),
                encodeRefCkptAtGeneration(gen3Ckpt(1, RefTxnId{1, 3}), 3), current->etag));
        };
    }
};

}

TEST(CASRefWriterRecovery, WalkedRecordThatDoesNotApplyRestartsWhenTheBaseMoved)
{
    UnappliableWalkedRecord fixture("srv1/unappliable_walked_record_base_moved", /*move_base=*/true);
    auto store = openWalkPool(fixture.backend);
    ASSERT_EQ(store->liveWriterEpoch(), 6u);

    std::map<String, Resolved> refs;
    ASSERT_NO_THROW(refs = store->listRefs(fixture.ns));
    EXPECT_EQ(fixture.fired, 1u);
    EXPECT_EQ(store->refRecoveryRestartsForTest(fixture.ns), 1u);
    EXPECT_TRUE(refs.contains("c"));
    EXPECT_TRUE(isTombstoneTo(readLogTxn(*fixture.backend, fixture.layout, fixture.ns, RefTxnId{1, 4}), 6))
        << "the restarted walk closes the chain above the new base";
}

TEST(CASRefWriterRecovery, WalkedRecordThatDoesNotApplyIsCorruptionUnderAnUnchangedBase)
{
    UnappliableWalkedRecord fixture("srv1/unappliable_walked_record_same_base", /*move_base=*/false);
    auto store = openWalkPool(fixture.backend);
    ASSERT_EQ(store->liveWriterEpoch(), 6u);

    expectThrowsCode(DB::ErrorCodes::CORRUPTED_DATA, [&] { (void)store->listRefs(fixture.ns); });
    EXPECT_EQ(fixture.fired, 1u);
    EXPECT_EQ(store->refRecoveryRestartsForTest(fixture.ns), 0u) << "an unchanged base is a verdict, not a restart";
    EXPECT_FALSE(store->refTableRecoveredForTest(fixture.ns));
    EXPECT_FALSE(readLogTxn(*fixture.backend, fixture.layout, fixture.ns, RefTxnId{1, 4}));
}

namespace
{

/// The fixture of `DeadEpochIsSealedInOneWriteWithoutACkptWrite`: `{1,1}`, `{1,2}`, a snapshot at `{1,2}` and a
/// generation 3 `_ckpt` naming it.
NamespaceLifeId seedSnapshotBasedChain(const BackendPtr & backend, const Layout & layout, const RootNamespace & ns)
{
    seedTxn(*backend, layout, ns, RefTxnId{1, 1}, "a", /*birth=*/true);
    seedTxn(*backend, layout, ns, RefTxnId{1, 2}, "b", /*birth=*/false);
    writeRefSnapshotRaw(*backend, layout, minimalLiveSnapshot(ns.string(), RefTxnId{1, 2},
        {committedRow("a", manifestRef(1, 1, 1)), committedRow("b", manifestRef(1, 2, 1))}));
    seedCkpt(*backend, layout, ns, gen3Ckpt(1, RefTxnId{1, 2}), 3);
    return catalogLife(backend, layout, ns);
}

/// The sealed bytes of `T{id}->next_epoch` without a backlink, as the lane writes them.
String tombstoneBytes(const RootNamespace & ns, RefTxnId id, uint64_t next_epoch)
{
    return sealObject(FormatId::RefLog, encodeRefLogTxn(tombstoneTxn(ns.string(), id, next_epoch)));
}

}

TEST(CASRefWriterRecovery, CleanPredecessorCostsSixReadsAndNoWrite)
{
    auto backend = std::make_shared<HidingListBackend>();
    const Layout layout("p");
    const RootNamespace ns{"srv1/clean_predecessor_reads"};
    burnEpochsUpTo(backend, layout, 4);
    seedCleanFarewell(*backend, layout, 3, 0);
    const NamespaceLifeId life = seedSnapshotBasedChain(backend, layout, ns);
    const String ckpt_key = layout.refCkptKey(life);

    auto store = openWalkPool(backend);
    ASSERT_EQ(store->liveEpochPairForTest(), (LiveEpochPair{4, 0}));
    backend->resetCounts();
    EXPECT_EQ(store->listRefs(ns).size(), 2u);

    EXPECT_EQ(backend->getCount(ckpt_key), 2u);
    EXPECT_EQ(backend->getCount(layout.refLogKey(life, RefTxnId{1, 2})), 1u);
    EXPECT_EQ(backend->getCount(layout.refSnapshotKey(life, RefTxnId{1, 2})), 1u);
    EXPECT_EQ(backend->getCount(layout.refLogKey(life, RefTxnId{1, 3})), 1u);
    EXPECT_EQ(backend->getCount(layout.refCatalogKey()), 3u) << "two of the cold admission, one confirmation";
    EXPECT_EQ(backend->getTotal(), 8u) << "no GET of any other key";
    EXPECT_EQ(backend->writeCount(layout.refLogKey(life, RefTxnId{1, 3})), 0u);
    EXPECT_EQ(backend->writeCount(ckpt_key), 0u);
    EXPECT_EQ(backend->putTotal(), 0u) << "a recovery after a clean end of the chain's epoch creates nothing";
    EXPECT_EQ(store->laneStateForTest(ns), RefLaneState::Ready);
}

TEST(CASRefWriterRecovery, UncleanPredecessorSealsBeforeInstall)
{
    auto backend = std::make_shared<HidingListBackend>();
    const Layout layout("p");
    const RootNamespace ns{"srv1/unclean_predecessor_seals"};
    burnEpochsUpTo(backend, layout, 4);
    const NamespaceLifeId life = seedSnapshotBasedChain(backend, layout, ns);
    const String ckpt_key = layout.refCkptKey(life);

    auto store = openWalkPool(backend);
    ASSERT_EQ(store->liveEpochPairForTest(), (LiveEpochPair{4, 3})) << "no slot: every earlier epoch counts as unclean";
    backend->resetCounts();
    EXPECT_EQ(store->listRefs(ns).size(), 2u);

    EXPECT_EQ(backend->getCount(ckpt_key), 2u);
    EXPECT_EQ(backend->getCount(layout.refLogKey(life, RefTxnId{1, 2})), 1u);
    EXPECT_EQ(backend->getCount(layout.refSnapshotKey(life, RefTxnId{1, 2})), 1u);
    EXPECT_EQ(backend->getCount(layout.refLogKey(life, RefTxnId{1, 3})), 1u);
    EXPECT_EQ(backend->getCount(layout.refCatalogKey()), 3u);
    EXPECT_EQ(backend->getTotal(), 8u) << "no GET of any other key";
    EXPECT_EQ(backend->writeCount(layout.refLogKey(life, RefTxnId{1, 3})), 1u);
    EXPECT_EQ(backend->putTotal(), 1u);
    EXPECT_EQ(backend->writeCount(ckpt_key), 0u);
    EXPECT_TRUE(isTombstoneTo(readLogTxn(*backend, layout, ns, RefTxnId{1, 3}), 4));
}

TEST(CASRefWriterRecovery, EagerRuleSurvivesACleanEpochThatNeverReadTheNamespace)
{
    auto backend = std::make_shared<HidingListBackend>();
    const Layout layout("p");
    const RootNamespace ns{"srv1/eager_through_clean_epoch"};
    burnEpochsUpTo(backend, layout, 3);
    /// Epoch 2 never touched the namespace and ended cleanly, so it passes on epoch 1's unclean end.
    seedCleanFarewell(*backend, layout, 2, 1);
    seedTwoRecordChain(backend, layout, ns);

    auto store = openWalkPool(backend);
    ASSERT_EQ(store->liveEpochPairForTest(), (LiveEpochPair{3, 1}));
    EXPECT_EQ(store->listRefs(ns).size(), 2u);
    EXPECT_TRUE(isTombstoneTo(readLogTxn(*backend, layout, ns, RefTxnId{1, 3}), 3));
}

TEST(CASRefWriterRecovery, UncleanReaderIsSealedByTheNextReaderAndTheWriterLinksThroughIt)
{
    auto backend = std::make_shared<HidingListBackend>();
    const Layout layout("p");
    const RootNamespace ns{"srv1/unclean_reader_sealed"};
    burnEpochsUpTo(backend, layout, 3);
    /// Reader 2 ended uncleanly without touching the namespace.
    seedFencedSlot(*backend, layout, 2);
    const NamespaceLifeId life = seedTwoRecordChain(backend, layout, ns);

    {
        auto reader = openWalkPool(backend);
        ASSERT_EQ(reader->liveEpochPairForTest(), (LiveEpochPair{3, 2}));
        EXPECT_EQ(reader->listRefs(ns).size(), 2u);
        EXPECT_TRUE(isTombstoneTo(readLogTxn(*backend, layout, ns, RefTxnId{1, 3}), 3))
            << "the chain's epoch 1 is at or below the marker 2, so reader 3 seals it";
    }   /// a clean farewell carrying marker 2

    auto store = openWalkPool(backend);
    ASSERT_EQ(store->liveEpochPairForTest(), (LiveEpochPair{4, 2}));
    EXPECT_EQ(store->listRefs(ns).size(), 2u);
    EXPECT_FALSE(readLogTxn(*backend, layout, ns, RefTxnId{3, 1}))
        << "the chain's epoch 3 is above the marker 2, so the read in epoch 4 writes nothing";
    publishRefForTest(store, ns, "c", manifestRef(4, 1, 1));

    const auto closing = readLogTxn(*backend, layout, ns, RefTxnId{3, 1});
    EXPECT_TRUE(isTombstoneTo(closing, 4));
    ASSERT_TRUE(closing);
    EXPECT_EQ(closing->prev_epoch_seal, std::make_optional(RefTxnId{1, 3}));
    const auto head = readLogTxn(*backend, layout, ns, RefTxnId{4, 1});
    ASSERT_TRUE(head);
    EXPECT_EQ(head->prev_epoch_seal, std::make_optional(RefTxnId{3, 1}));
    EXPECT_TRUE(logIdsInEpochs(*backend, layout, life, 2, 2).empty());
}

TEST(CASRefWriterGenesis, EagerRuleSealsANeverBornStream)
{
    auto backend = std::make_shared<HidingListBackend>();
    const Layout layout("p");
    const RootNamespace ns{"srv1/eager_never_born"};
    burnEpochsUpTo(backend, layout, 4);
    DB::Cas::tests::fixture::admitLive(*backend, layout, ns);
    seedCkpt(*backend, layout, ns, gen3Ckpt(2), 3);

    auto store = openWalkPool(backend);
    ASSERT_EQ(store->liveEpochPairForTest(), (LiveEpochPair{4, 3}));
    EXPECT_TRUE(store->listRefs(ns).empty());
    EXPECT_TRUE(isTombstoneTo(readLogTxn(*backend, layout, ns, RefTxnId{2, 1}), 4));
}

TEST(CASRefWriterTombstone, BacklinkSkipsEpochsThatOnlyRead)
{
    auto backend = std::make_shared<HidingListBackend>();
    const Layout layout("p");
    const RootNamespace ns{"srv1/backlink_skips_readers"};
    burnEpochsUpTo(backend, layout, 3);
    seedCleanFarewell(*backend, layout, 2, 0);
    const NamespaceLifeId life = seedTwoRecordChain(backend, layout, ns);

    auto store = openWalkPool(backend);
    ASSERT_EQ(store->liveEpochPairForTest(), (LiveEpochPair{3, 0}));
    EXPECT_EQ(store->listRefs(ns).size(), 2u);
    EXPECT_EQ(backend->writeCount(layout.refLogKey(life, RefTxnId{1, 3})), 0u) << "the read writes nothing";

    /// The tombstone and the head come from one batch, so the head's preview sees the tombstone.
    EXPECT_NO_THROW(publishRefForTest(store, ns, "c", manifestRef(3, 1, 1)));
    const auto tombstone = readLogTxn(*backend, layout, ns, RefTxnId{1, 3});
    EXPECT_TRUE(isTombstoneTo(tombstone, 3));
    ASSERT_TRUE(tombstone);
    EXPECT_EQ(tombstone->prev_epoch_seal, std::nullopt);
    const auto head = readLogTxn(*backend, layout, ns, RefTxnId{3, 1});
    ASSERT_TRUE(head);
    EXPECT_EQ(head->prev_epoch_seal, std::make_optional(RefTxnId{1, 3}));
    EXPECT_TRUE(logIdsInEpochs(*backend, layout, life, 2, 2).empty());
    EXPECT_TRUE(store->listRefs(ns).contains("c"));
}

TEST(CASRefWriterTombstone, AmbiguousTombstoneWedgesAndBlocksTheHead)
{
    auto backend = std::make_shared<HidingListBackend>();
    const Layout layout("p");
    const RootNamespace ns{"srv1/ambiguous_lane_tombstone"};
    burnEpochsUpTo(backend, layout, 3);
    seedCleanFarewell(*backend, layout, 2, 0);
    const NamespaceLifeId life = seedTwoRecordChain(backend, layout, ns);

    auto store = openWalkPool(backend);
    auto clock = VirtualRetryClock::installOn(store);
    ASSERT_EQ(store->liveEpochPairForTest(), (LiveEpochPair{3, 0}));
    EXPECT_EQ(store->listRefs(ns).size(), 2u);

    const uint64_t lane_before = ProfileEvents::global_counters[ProfileEvents::CASRefAppendTombstoneWritten];
    backend->ambiguous_put_substr = layout.refLogKey(life, RefTxnId{1, 3});
    expectThrowsCode(DB::ErrorCodes::NETWORK_ERROR, [&] { publishRefForTest(store, ns, "c", manifestRef(3, 1, 1)); });
    EXPECT_GE(backend->ambiguous_put_attempts.load(), 1u);
    EXPECT_TRUE(store->refLaneWedgedForTest(ns));
    EXPECT_TRUE(logIdsInEpochs(*backend, layout, life, 3, 3).empty()) << "no head while the tombstone is unresolved";
    EXPECT_EQ(ProfileEvents::global_counters[ProfileEvents::CASRefAppendTombstoneWritten] - lane_before, 0u);

    backend->ambiguous_put_substr.clear();
    publishRefForTest(store, ns, "c", manifestRef(3, 1, 1));
    EXPECT_EQ(ProfileEvents::global_counters[ProfileEvents::CASRefAppendTombstoneWritten] - lane_before, 1u)
        << "the tombstone the wedge resolution made durable is the lane's";
    const auto tombstone = readAt(*backend, layout.refLogKey(life, RefTxnId{1, 3}));
    ASSERT_TRUE(tombstone);
    EXPECT_EQ(tombstone->bytes, tombstoneBytes(ns, RefTxnId{1, 3}, 3));
    const auto head = readLogTxn(*backend, layout, ns, RefTxnId{3, 1});
    ASSERT_TRUE(head);
    EXPECT_EQ(head->prev_epoch_seal, std::make_optional(RefTxnId{1, 3}));
    EXPECT_FALSE(store->refLaneWedgedForTest(ns));
}

TEST(CASRefWriterTombstone, DetachedRuntimeInARemountWindowWritesNoTombstone)
{
    auto backend = std::make_shared<HidingListBackend>();
    const Layout layout("p");
    const RootNamespace ns{"srv1/detached_remount_window"};
    burnEpochsUpTo(backend, layout, 3);
    seedCleanFarewell(*backend, layout, 2, 0);
    const NamespaceLifeId life = seedTwoRecordChain(backend, layout, ns);

    struct Window
    {
        std::weak_ptr<Pool> store;
        bool fired = false;
        uint64_t live_epoch = 0;
        bool old_runtime_cached = false;
        int publish_code = 0;
        std::vector<RefTxnId> logs_after_publish;
    };
    auto window = std::make_shared<Window>();
    PoolConfig config = walkTestConfig();
    config.remount_epoch_published_hook_for_test = [window, backend, layout, ns, life]
    {
        const PoolPtr pool = window->store.lock();
        if (!pool)
            return;
        window->fired = true;
        window->live_epoch = pool->liveEpochPairForTest().writer_epoch;
        window->old_runtime_cached = pool->refTableRecoveredForTest(ns);
        std::thread publisher([&]
        {
            try
            {
                publishRefForTest(pool, ns, "c", manifestRef(3, 1, 1));
            }
            catch (const DB::Exception & e)
            {
                window->publish_code = e.code();
            }
        });
        publisher.join();
        window->logs_after_publish = logIdsInEpochs(*backend, layout, life, 1, 4);
    };

    auto store = openWalkPool(backend, config);
    window->store = store;
    ASSERT_EQ(store->liveEpochPairForTest(), (LiveEpochPair{3, 0}));
    EXPECT_EQ(store->listRefs(ns).size(), 2u);

    fenceOutMountForRemount(*backend, layout.mountKey("test"));
    EXPECT_THROW(store->renewWatermarkOnce(), DB::Exception);
    ASSERT_TRUE(store->tryRemountOnce());

    ASSERT_TRUE(window->fired);
    EXPECT_EQ(window->live_epoch, 4u) << "the hook runs after the new epoch is published";
    EXPECT_TRUE(window->old_runtime_cached) << "and before the old runtime is detached";
    EXPECT_EQ(window->publish_code, DB::ErrorCodes::NETWORK_ERROR);
    EXPECT_EQ(window->logs_after_publish, (std::vector<RefTxnId>{RefTxnId{1, 1}, RefTxnId{1, 2}}))
        << "no tombstone naming the new epoch, and no head";
}

TEST(CASRefWriterTombstone, LateRecoveryTombstoneOfAnEarlierBuildIsAdopted)
{
    auto backend = std::make_shared<HidingListBackend>();
    const Layout layout("p");
    const RootNamespace ns{"srv1/late_recovery_tombstone"};
    burnEpochsUpTo(backend, layout, 3);
    seedCleanFarewell(*backend, layout, 2, 0);
    const NamespaceLifeId life = seedTwoRecordChain(backend, layout, ns);

    auto store = openWalkPool(backend);
    ASSERT_EQ(store->liveEpochPairForTest(), (LiveEpochPair{3, 0}));
    EXPECT_EQ(store->listRefs(ns).size(), 2u);
    EXPECT_EQ(store->laneStateForTest(ns), RefLaneState::Ready);

    /// An unresolved recovery tombstone of epoch 2 that landed after epoch 2's clean farewell.
    createTxnAt(*backend, layout, life, tombstoneTxn(ns.string(), RefTxnId{1, 3}, 2));
    expectThrowsCode(DB::ErrorCodes::NETWORK_ERROR, [&] { publishRefForTest(store, ns, "c", manifestRef(3, 1, 1)); });
    EXPECT_EQ(store->laneStateForTest(ns), RefLaneState::NeedsRecovery);
    EXPECT_TRUE(logIdsInEpochs(*backend, layout, life, 2, 3).empty());

    publishRefForTest(store, ns, "c", manifestRef(3, 1, 1));
    const auto closing = readLogTxn(*backend, layout, ns, RefTxnId{2, 1});
    EXPECT_TRUE(isTombstoneTo(closing, 3));
    ASSERT_TRUE(closing);
    EXPECT_EQ(closing->prev_epoch_seal, std::make_optional(RefTxnId{1, 3}));
    const auto head = readLogTxn(*backend, layout, ns, RefTxnId{3, 1});
    ASSERT_TRUE(head);
    EXPECT_EQ(head->prev_epoch_seal, std::make_optional(RefTxnId{2, 1}));
    EXPECT_TRUE(store->listRefs(ns).contains("c"));
}

TEST(CASRefWriterTombstone, WedgedTombstoneThatMeetsAnEarlierTombstoneNeedsRecovery)
{
    const Layout layout("p");
    const auto wedge_the_tombstone = [&](const std::shared_ptr<HidingListBackend> & backend, const RootNamespace & ns)
    {
        burnEpochsUpTo(backend, layout, 3);
        seedCleanFarewell(*backend, layout, 2, 0);
        const NamespaceLifeId life = seedTwoRecordChain(backend, layout, ns);
        auto store = openWalkPool(backend);
        VirtualRetryClock::installOn(store);
        EXPECT_EQ(store->listRefs(ns).size(), 2u);
        backend->ambiguous_put_substr = layout.refLogKey(life, RefTxnId{1, 3});
        expectThrowsCode(DB::ErrorCodes::NETWORK_ERROR, [&] { publishRefForTest(store, ns, "c", manifestRef(3, 1, 1)); });
        EXPECT_TRUE(store->refLaneWedgedForTest(ns));
        backend->ambiguous_put_substr.clear();
        return std::make_pair(store, life);
    };
    {
        auto backend = std::make_shared<HidingListBackend>();
        const RootNamespace ns{"srv1/wedged_tombstone_meets_earlier"};
        const auto [store, life] = wedge_the_tombstone(backend, ns);
        createTxnAt(*backend, layout, life, tombstoneTxn(ns.string(), RefTxnId{1, 3}, 2));

        expectThrowsCode(DB::ErrorCodes::NETWORK_ERROR, [&] { publishRefForTest(store, ns, "c", manifestRef(3, 1, 1)); });
        EXPECT_EQ(store->laneStateForTest(ns), RefLaneState::NeedsRecovery);
        EXPECT_FALSE(store->refLaneWedgedForTest(ns));

        publishRefForTest(store, ns, "c", manifestRef(3, 1, 1));
        const auto closing = readLogTxn(*backend, layout, ns, RefTxnId{2, 1});
        EXPECT_TRUE(isTombstoneTo(closing, 3));
        ASSERT_TRUE(closing);
        EXPECT_EQ(closing->prev_epoch_seal, std::make_optional(RefTxnId{1, 3}));
        EXPECT_TRUE(store->listRefs(ns).contains("c"));
    }
    {
        auto backend = std::make_shared<HidingListBackend>();
        const RootNamespace ns{"srv1/wedged_tombstone_meets_later"};
        const auto [store, life] = wedge_the_tombstone(backend, ns);
        createTxnAt(*backend, layout, life, tombstoneTxn(ns.string(), RefTxnId{1, 3}, 5));

        expectThrowsCode(DB::ErrorCodes::INVALID_STATE, [&] { publishRefForTest(store, ns, "c", manifestRef(3, 1, 1)); });
        EXPECT_EQ(store->laneStateForTest(ns), RefLaneState::Closed);
        EXPECT_TRUE(logIdsInEpochs(*backend, layout, life, 2, 5).empty());
    }
}

namespace
{

/// The clean-predecessor fixture with a generation 2 `_ckpt` whose frontier is `{1,2}`: live epoch 3, chain
/// epoch 1, marker 0, so the read is lazy and the lane's first write must stamp `_ckpt` first.
NamespaceLifeId seedOldCheckpointChain(const BackendPtr & backend, const Layout & layout, const RootNamespace & ns)
{
    burnEpochsUpTo(backend, layout, 3);
    seedCleanFarewell(*backend, layout, 2, 0);
    seedTxn(*backend, layout, ns, RefTxnId{1, 1}, "a", /*birth=*/true);
    seedTxn(*backend, layout, ns, RefTxnId{1, 2}, "b", /*birth=*/false);
    seedCkpt(*backend, layout, ns, lifeEpochCkpt(1, RefTxnId{1, 2}), 2);
    return catalogLife(backend, layout, ns);
}

/// The index of the first journal entry equal to `(method, key)`, or `nullopt`.
std::optional<size_t> journalIndex(const std::vector<std::pair<String, String>> & journal, const String & method,
                                   const String & key)
{
    const auto it = std::find(journal.begin(), journal.end(), std::make_pair(method, key));
    if (it == journal.end())
        return std::nullopt;
    return static_cast<size_t>(it - journal.begin());
}

/// An ordinary transaction of id `{1,4}` stored at `{1,3}`: it does not decode at that key.
String undecodableAtTheTombstoneSlot(const RootNamespace & ns)
{
    return sealObject(FormatId::RefLog, encodeRefLogTxn(makeOrdinaryTxn(ns, RefTxnId{1, 4}, "x", /*birth=*/false)));
}

}

TEST(CASRefWriterTombstone, LaneTombstoneFollowsTheStampOfAnOldCheckpoint)
{
    auto backend = std::make_shared<JournalingBackend>();
    const Layout layout("p");
    const RootNamespace ns{"srv1/lane_tombstone_stamp"};
    const NamespaceLifeId life = seedOldCheckpointChain(backend, layout, ns);
    const String ckpt_key = layout.refCkptKey(life);

    auto store = openWalkPool(backend);
    ASSERT_EQ(store->liveEpochPairForTest(), (LiveEpochPair{3, 0}));
    backend->clearJournal();
    EXPECT_EQ(store->listRefs(ns).size(), 2u);
    for (const auto & [method, key] : backend->journal())
        EXPECT_FALSE((method == "PUT" || method == "CAS") && (key == ckpt_key || key.starts_with(layout.namespaceStreamPrefix(life))))
            << "the read writes nothing: " << method << " " << key;
    EXPECT_EQ(storedCkptHeader(*backend, layout, life), 2u);

    backend->clearJournal();
    const uint64_t stamped_before = ProfileEvents::global_counters[ProfileEvents::CASRefCheckpointStamped];
    const uint64_t lane_before = ProfileEvents::global_counters[ProfileEvents::CASRefAppendTombstoneWritten];
    const uint64_t sealed_before = ProfileEvents::global_counters[ProfileEvents::CASRefRecoveryEpochSealed];
    publishRefForTest(store, ns, "c", manifestRef(3, 1, 1));
    EXPECT_EQ(ProfileEvents::global_counters[ProfileEvents::CASRefCheckpointStamped] - stamped_before, 1u);
    EXPECT_EQ(ProfileEvents::global_counters[ProfileEvents::CASRefAppendTombstoneWritten] - lane_before, 1u);
    EXPECT_EQ(ProfileEvents::global_counters[ProfileEvents::CASRefRecoveryEpochSealed] - sealed_before, 0u);
    const auto journal = backend->journal();
    const auto stamp = journalIndex(journal, "CAS", ckpt_key);
    const auto tombstone = journalIndex(journal, "PUT", layout.refLogKey(life, RefTxnId{1, 3}));
    const auto head = journalIndex(journal, "PUT", layout.refLogKey(life, RefTxnId{3, 1}));
    ASSERT_TRUE(stamp);
    ASSERT_TRUE(tombstone);
    ASSERT_TRUE(head);
    EXPECT_LT(*stamp, *tombstone) << "the stamp precedes the tombstone";
    EXPECT_LT(*tombstone, *head);
    for (size_t i = *tombstone + 1; i < *head; ++i)
        EXPECT_NE(journal[i], (std::pair<String, String>{"GET", ckpt_key})) << "no `_ckpt` read between the tombstone and the head";
    EXPECT_EQ(storedCkptHeader(*backend, layout, life), 3u);
    EXPECT_TRUE(isTombstoneTo(readLogTxn(*backend, layout, ns, RefTxnId{1, 3}), 3));
}

TEST(CASRefWriterTombstone, LaneTombstoneStampMismatchOrUnresolvedNeedsRecoveryWithoutACreate)
{
    const Layout layout("p");
    const auto expect_no_create = [&](const std::shared_ptr<JournalingBackend> & backend, const PoolPtr & store,
                                      const RootNamespace & ns, const NamespaceLifeId & life)
    {
        expectThrowsCode(DB::ErrorCodes::NETWORK_ERROR, [&] { publishRefForTest(store, ns, "c", manifestRef(3, 1, 1)); });
        EXPECT_EQ(store->laneStateForTest(ns), RefLaneState::NeedsRecovery);
        const auto journal = backend->journal();
        EXPECT_FALSE(journalIndex(journal, "PUT", layout.refLogKey(life, RefTxnId{1, 3}))) << "no tombstone create";
        EXPECT_FALSE(journalIndex(journal, "PUT", layout.refLogKey(life, RefTxnId{3, 1}))) << "no head create";
        auto ids = logIdsInEpochs(*backend, layout, life, 1, 3);
        std::sort(ids.begin(), ids.end());
        EXPECT_EQ(ids, (std::vector<RefTxnId>{RefTxnId{1, 1}, RefTxnId{1, 2}}));
    };
    {
        auto backend = std::make_shared<JournalingBackend>();
        const RootNamespace ns{"srv1/lane_tombstone_stamp_mismatch"};
        const NamespaceLifeId life = seedOldCheckpointChain(backend, layout, ns);
        const String ckpt_key = layout.refCkptKey(life);
        auto store = openWalkPool(backend);
        EXPECT_EQ(store->listRefs(ns).size(), 2u);

        bool armed = true;
        backend->before_cas_put = [&](const String & key, const String &, const std::optional<String> &)
        {
            if (!armed || key != ckpt_key)
                return;
            armed = false;
            const auto current = readAt(*backend, ckpt_key);
            ASSERT_TRUE(current);
            ASSERT_TRUE(replaceForTest(backend, ckpt_key, encodeRefCkptAtGeneration(lifeEpochCkpt(1, RefTxnId{1, 2}), 2),
                current->etag));
        };
        backend->clearJournal();
        expect_no_create(backend, store, ns, life);
        backend->before_cas_put = nullptr;
        EXPECT_FALSE(armed) << "the stamp met the replaced token";
    }
    {
        auto backend = std::make_shared<JournalingBackend>();
        const RootNamespace ns{"srv1/lane_tombstone_stamp_unresolved"};
        const NamespaceLifeId life = seedOldCheckpointChain(backend, layout, ns);
        auto store = openWalkPool(backend);
        EXPECT_EQ(store->listRefs(ns).size(), 2u);

        backend->ambiguous_cas_substr = layout.refCkptKey(life);
        backend->ambiguous_cas_count = 1;
        backend->clearJournal();
        expect_no_create(backend, store, ns, life);
        EXPECT_EQ(backend->ambiguous_cas_count, 0);
    }
}

TEST(CASRefWriterTombstone, UndecodableOccupantOfTheSlotFaultsTheLane)
{
    const Layout layout("p");
    /// At the tombstone's own create.
    {
        auto backend = std::make_shared<HidingListBackend>();
        const RootNamespace ns{"srv1/undecodable_at_tombstone_create"};
        burnEpochsUpTo(backend, layout, 3);
        seedCleanFarewell(*backend, layout, 2, 0);
        const NamespaceLifeId life = seedTwoRecordChain(backend, layout, ns);
        auto store = openWalkPool(backend);
        EXPECT_EQ(store->listRefs(ns).size(), 2u);
        const String garbage = undecodableAtTheTombstoneSlot(ns);
        createAt(*backend, layout.refLogKey(life, RefTxnId{1, 3}), garbage);

        expectThrowsCode(DB::ErrorCodes::CORRUPTED_DATA, [&] { publishRefForTest(store, ns, "c", manifestRef(3, 1, 1)); });
        EXPECT_EQ(store->laneStateForTest(ns), RefLaneState::Faulted);
        EXPECT_FALSE(store->mayMutate()) << "the impossible-interference reaction must fence this mount closed";
        EXPECT_EQ(store->scheduleRemountCallCountForTest(), 1u);
        EXPECT_EQ(readAt(*backend, layout.refLogKey(life, RefTxnId{1, 3}))->bytes, garbage);
        EXPECT_TRUE(logIdsInEpochs(*backend, layout, life, 3, 3).empty());
    }
    /// At the resolution of a wedged tombstone.
    {
        auto backend = std::make_shared<HidingListBackend>();
        const RootNamespace ns{"srv1/undecodable_at_tombstone_wedge"};
        burnEpochsUpTo(backend, layout, 3);
        seedCleanFarewell(*backend, layout, 2, 0);
        const NamespaceLifeId life = seedTwoRecordChain(backend, layout, ns);
        auto store = openWalkPool(backend);
        VirtualRetryClock::installOn(store);
        EXPECT_EQ(store->listRefs(ns).size(), 2u);
        backend->ambiguous_put_substr = layout.refLogKey(life, RefTxnId{1, 3});
        expectThrowsCode(DB::ErrorCodes::NETWORK_ERROR, [&] { publishRefForTest(store, ns, "c", manifestRef(3, 1, 1)); });
        ASSERT_TRUE(store->refLaneWedgedForTest(ns));
        backend->ambiguous_put_substr.clear();
        const String garbage = undecodableAtTheTombstoneSlot(ns);
        createAt(*backend, layout.refLogKey(life, RefTxnId{1, 3}), garbage);

        expectThrowsCode(DB::ErrorCodes::CORRUPTED_DATA, [&] { publishRefForTest(store, ns, "c", manifestRef(3, 1, 1)); });
        EXPECT_EQ(store->laneStateForTest(ns), RefLaneState::Faulted);
        EXPECT_FALSE(store->refLaneWedgedForTest(ns));
        EXPECT_TRUE(logIdsInEpochs(*backend, layout, life, 3, 3).empty());
    }
    /// At an ordinary append, unchanged: the chain epoch is the live one.
    {
        auto backend = std::make_shared<HidingListBackend>();
        const RootNamespace ns{"srv1/undecodable_at_ordinary_append"};
        const NamespaceLifeId life = seedTwoRecordChain(backend, layout, ns);
        auto store = openWalkPool(backend);
        ASSERT_EQ(store->liveWriterEpoch(), 1u);
        EXPECT_EQ(store->listRefs(ns).size(), 2u);
        createAt(*backend, layout.refLogKey(life, RefTxnId{1, 3}), undecodableAtTheTombstoneSlot(ns));

        expectThrowsCode(DB::ErrorCodes::CORRUPTED_DATA, [&] { publishRefForTest(store, ns, "c", manifestRef(1, 3, 1)); });
        EXPECT_EQ(store->laneStateForTest(ns), RefLaneState::Faulted);
    }
}

TEST(CASRefWriterTombstone, TombstoneInstallRefusedAfterItsCreateNeedsRecoveryAndTheHeadLinksToIt)
{
    auto backend = std::make_shared<HidingListBackend>();
    const Layout layout("p");
    const RootNamespace ns{"srv1/tombstone_install_refused"};
    burnEpochsUpTo(backend, layout, 3);
    seedCleanFarewell(*backend, layout, 2, 0);
    const NamespaceLifeId life = seedTwoRecordChain(backend, layout, ns);

    auto store = openWalkPool(backend);
    ASSERT_EQ(store->liveEpochPairForTest(), (LiveEpochPair{3, 0}));
    EXPECT_EQ(store->listRefs(ns).size(), 2u);

    /// The admission is lost between the create and the install hold: the lease deadline is reached.
    Pool * const pool = store.get();
    store->setCarveHookForTest([pool](CasRefLedger::CarvePhaseForTest phase)
    {
        if (phase == CasRefLedger::CarvePhaseForTest::PostDurableTombstoneInstall)
            pool->setMountDeadline(pool->bootMsNow());
    });
    expectThrowsCode(DB::ErrorCodes::NETWORK_ERROR, [&] { publishRefForTest(store, ns, "c", manifestRef(3, 1, 1)); });
    store->setCarveHookForTest(nullptr);
    EXPECT_EQ(store->laneStateForTest(ns), RefLaneState::NeedsRecovery);
    EXPECT_TRUE(isTombstoneTo(readLogTxn(*backend, layout, ns, RefTxnId{1, 3}), 3)) << "the tombstone is durable";
    EXPECT_TRUE(logIdsInEpochs(*backend, layout, life, 3, 3).empty());

    store->setMountDeadline(store->bootMsNow() + 600'000);
    publishRefForTest(store, ns, "c", manifestRef(3, 1, 1));
    const auto head = readLogTxn(*backend, layout, ns, RefTxnId{3, 1});
    ASSERT_TRUE(head);
    EXPECT_EQ(head->prev_epoch_seal, std::make_optional(RefTxnId{1, 3}));
    auto ids = logIdsInEpochs(*backend, layout, life, 1, 3);
    std::sort(ids.begin(), ids.end());
    EXPECT_EQ(ids, (std::vector<RefTxnId>{RefTxnId{1, 1}, RefTxnId{1, 2}, RefTxnId{1, 3}, RefTxnId{3, 1}}))
        << "no second tombstone";
    EXPECT_TRUE(store->listRefs(ns).contains("c"));
}
