#include <gtest/gtest.h>

#include "config.h"

#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Backend/CasInMemoryBackend.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Formats/CasFormat.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Formats/CasLayout.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Formats/CasRefCkptFormat.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Formats/CasRefLogFormat.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasPool.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasRefCkpt.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasRefProtocol.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasServerRoot.h>
#include <Disks/tests/cas_test_helpers.h>
#include <Common/Exception.h>
#include <Common/MemoryTracker.h>
#include <Common/ProfileEvents.h>

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <thread>
#include <vector>

/// Stage A task 6: recovery is `_ckpt` + an ARITHMETIC tail + a seal CAS-walk, and it installs nothing
/// without presenting the fence generation it was admitted under.
///
/// The one sentence this suite exists to defend: **a LIST is a hint, never a census.** Everything the
/// old recovery knew about a table's durable stream came from one `LIST`, so a listing that silently
/// omitted a key produced a table that was missing an ACKED transaction and looked perfectly healthy --
/// the blocker observed live on 2026-07-26. Completeness is now decided by arithmetic (INV-1: ids are
/// dense `1..T` within `(namespace, epoch)`), so a hint omission costs one exact-key `GET` and changes
/// no outcome, while a genuine hole is a `CORRUPTED_DATA` nobody can mistake for an empty tail.
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
extern const int NETWORK_ERROR;
}

namespace ProfileEvents
{
extern const Event CasRefRecoveryRestarts;
extern const Event CasRefRecoveryEpochSealed;
extern const Event CasRefRecoveryEpochSealAdopted;
extern const Event CasRefRecoveryStragglerAdopted;
extern const Event CasRefRecoveryCancelled;
extern const Event CasRefCkptPublished;
}

using namespace DB::Cas;
using DB::Cas::tests::committedRow;
using DB::Cas::tests::CountingBackend;
using DB::Cas::tests::expectThrowsCode;
using DB::Cas::tests::minimalLiveSnapshot;
using DB::Cas::tests::namespaceBirthOp;
using DB::Cas::tests::publishCommittedOps;
using DB::Cas::tests::rearmMountFenceAfterAnomalyForTest;
using DB::Cas::tests::writeRefLogTxnRaw;
using DB::Cas::tests::writeRefSnapshotRaw;

namespace
{

ManifestRef manifestRef(uint64_t epoch, uint64_t build_sequence, uint32_t ordinal)
{
    return ManifestRef{epoch, build_sequence, ordinal};
}

/// A backend whose LIST can be made to LIE by omission -- the whole point of this suite. `hidden_keys`
/// are still readable by exact key (that is what a real listing inconsistency looks like: the object is
/// there, the enumeration simply did not mention it), and `list` filters them out of every page.
///
/// Deliberately NOT a "delete the object" fixture: an object that is genuinely gone is a different
/// (and already covered) case. The blocker is an object that EXISTS and is invisible to enumeration.
class HidingListBackend : public CountingBackend
{
public:
    using CountingBackend::get;
    using CountingBackend::list;
    using CountingBackend::putIfAbsent;

    std::set<String> hidden_keys;

    /// Every `putIfAbsent` of a key containing this substring throws a PLAIN (non-`DB::Exception`)
    /// error, which `classifyConditionalWriteResult` can only ever classify `Unresolved` -- never
    /// `DefiniteFailure`. Persistent rather than one-shot on purpose: the subject is what recovery does
    /// when the store KEEPS refusing to say whether the write landed.
    String ambiguous_put_substr;

    ListPage list(const String & prefix, const String & cursor, size_t limit) override
    {
        ListPage page = CountingBackend::list(prefix, cursor, limit);
        std::vector<ListedKey> kept;
        kept.reserve(page.keys.size());
        for (ListedKey & lk : page.keys)
            if (!hidden_keys.contains(lk.key))
                kept.push_back(std::move(lk));
        page.keys = std::move(kept);
        return page;
    }

    PutResult putIfAbsent(const String & key, const String & bytes, const ObjectMeta & meta) override
    {
        if (!ambiguous_put_substr.empty() && key.find(ambiguous_put_substr) != String::npos)
            throw std::runtime_error("injected ambiguous putIfAbsent");
        return CountingBackend::putIfAbsent(key, bytes, meta);
    }
};

/// Fires `on_key` immediately AFTER a `putIfAbsent` whose key contains `watched_substr` -- the
/// deterministic way to act inside recovery's own write window (bump a fence, land a straggler) with no
/// sleep and no second thread. `skip` lets a test target the Nth such write.
class PutHookBackend : public HidingListBackend
{
public:
    using HidingListBackend::putIfAbsent;

    using HidingListBackend::casPut;

    String watched_substr;
    uint64_t skip = 0;
    std::function<void()> on_key;

    PutResult putIfAbsent(const String & key, const String & bytes, const ObjectMeta & meta) override
    {
        PutResult result = HidingListBackend::putIfAbsent(key, bytes, meta);
        fireIfWatched(key);
        return result;
    }

    /// The `_ckpt` advance is a token-CAS, not a create, whenever the object already exists -- which is
    /// the normal case, since the namespace birth creates it. Hooking only `putIfAbsent` would silently
    /// never fire for it.
    CasResult casPut(const String & key, const String & bytes, const std::optional<Token> & expected,
                     const ObjectMeta & meta) override
    {
        CasResult result = CountingBackend::casPut(key, bytes, expected, meta);
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
    using HidingListBackend::get;

    String late_key;
    String late_bytes;

    std::optional<GetResult> get(const String & key, Range range) override
    {
        std::optional<GetResult> result = HidingListBackend::get(key, range);
        if (!result && !late_key.empty() && key == late_key)
        {
            CountingBackend::putIfAbsent(late_key, late_bytes);
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
    using HidingListBackend::get;

    String watched_substr;

    /// Assigned from the test thread and read from whatever thread the recovery runs on, so the
    /// read-and-move below is guarded. Today's tests all assign before starting the recovery thread and
    /// clear after joining it, so there is no race to fix -- but this is the same seam that already
    /// produced one use-after-free, and "the current tests happen not to race" is not a property a
    /// future test author can see. The mutex makes the constraint enforced rather than remembered.
    std::mutex hook_mutex;
    std::function<void(const String &)> on_key;

    std::optional<GetResult> get(const String & key, Range range) override
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
        return HidingListBackend::get(key, range);
    }
};

CasRequestBudget tinyBudget()
{
    return CasRequestBudget{
        .attempt_timeout_ms = 50, .operation_deadline_ms = 500, .max_attempts = 1, .lease_safety_margin_ms = 50};
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

PoolPtr openWalkPool(const BackendPtr & backend, PoolConfig config = walkTestConfig())
{
    DB::Cas::tests::seedPoolMetaForRestart(*backend, config.pool_prefix);
    return Pool::open(backend, std::move(config));
}

/// Burns durable writer epochs so a subsequent `Pool::open` allocates `target_live_epoch`. Epochs are
/// minted, never reclaimed (`CasPool.cpp`'s allocator), so this is exactly what a pool that has been
/// mounted `n` times looks like -- including the burned epochs in which nothing was ever written, which
/// the seal chain must cross.
void burnEpochsUpTo(Backend & backend, const Layout & layout, uint64_t target_live_epoch)
{
    for (uint64_t e = 1; e < target_live_epoch; ++e)
        allocateWriterEpoch(backend, layout, "test");
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
    writeRefLogTxnRaw(backend, layout, makeOrdinaryTxn(ns, id, ref, birth));
}

/// Seeds the `_ckpt` a real namespace birth would have created, so recovery can ground its walk at the
/// namespace's `life_epoch` without consulting the (untrusted) listing. Raw, because these fixtures
/// never run a birth through the append lane.
void seedCkpt(Backend & backend, const Layout & layout, const RootNamespace & ns, const RefCkpt & ckpt)
{
    backend.putIfAbsent(layout.refCkptKey(RefNamespaceId::stageATransition(ns)), encodeRefCkpt(ckpt));
}

RefCkpt lifeEpochCkpt(uint64_t life_epoch)
{
    return RefCkpt{.life_epoch = std::optional<uint64_t>{life_epoch},
                   .checkpoint_snapshot_id = std::nullopt,
                   .last_epoch_seal = std::nullopt};
}

/// The decoded transaction at `id`, or `nullopt` when the object is absent. Never dereferences a
/// disengaged optional: an aborted binary would take every later suite's result with it.
std::optional<RefLogTxn> readLogTxn(Backend & backend, const Layout & layout, const RootNamespace & ns, RefTxnId id)
{
    const auto got = backend.get(layout.refLogKey(RefNamespaceId::stageATransition(ns), id));
    if (!got)
        return std::nullopt;
    return decodeRefLogTxn(openObject(FormatId::RefLog, got->bytes), ns.string(), id);
}

uint64_t counterOf(ProfileEvents::Event event)
{
    return ProfileEvents::global_counters[event].load();
}

}

/// ---------------------------------------------------------------------------------------------
/// The arithmetic tail: a hint is a hint
/// ---------------------------------------------------------------------------------------------

/// THE blocker's recovery face. The namespace's durable stream is `{1,1} {1,2} {1,3}`; the listing
/// omits the MIDDLE one. Under LIST-reconciliation recovery replayed `{1,1}` then met `{1,3}`, which is
/// not the contiguous successor -- so before INV-1 it silently produced a table missing an acked
/// transaction, and after INV-1 it would fail the whole table. Arithmetic fetches the omitted id by
/// exact key and the recovered table is IDENTICAL to the one a complete hint produces.
TEST(CasRefRecoveryCasWalk, HintOmittingAMiddleLogIdChangesNothing)
{
    auto backend = std::make_shared<HidingListBackend>();
    const Layout layout("p");
    const RootNamespace ns{"srv1/hint_middle"};

    seedCkpt(*backend, layout, ns, lifeEpochCkpt(1));
    seedTxn(*backend, layout, ns, RefTxnId{1, 1}, "a", /*birth=*/true);
    seedTxn(*backend, layout, ns, RefTxnId{1, 2}, "b", /*birth=*/false);
    seedTxn(*backend, layout, ns, RefTxnId{1, 3}, "c", /*birth=*/false);
    backend->hidden_keys.insert(layout.refLogKey(RefNamespaceId::stageATransition(ns), RefTxnId{1, 2}));

    auto store = openWalkPool(backend);
    ASSERT_TRUE(store);

    const auto refs = store->listRefs(ns);
    EXPECT_EQ(refs.size(), 3u) << "the hint omitted {1,2}; the arithmetic walk must fetch it by exact key";
    EXPECT_TRUE(refs.contains("a"));
    EXPECT_TRUE(refs.contains("b")) << "'b' is the ref the omitted transaction published";
    EXPECT_TRUE(refs.contains("c"));
}

/// The same omission at the TAIL. This is the more dangerous shape: a lost middle id at least makes the
/// replay non-contiguous, whereas a lost tail id looks exactly like "the stream ends here" -- there is
/// nothing above it to disagree with. Only an exact-key probe of `last + 1` can tell them apart.
TEST(CasRefRecoveryCasWalk, HintOmittingTheTailLogIdChangesNothing)
{
    auto backend = std::make_shared<HidingListBackend>();
    const Layout layout("p");
    const RootNamespace ns{"srv1/hint_tail"};

    seedCkpt(*backend, layout, ns, lifeEpochCkpt(1));
    seedTxn(*backend, layout, ns, RefTxnId{1, 1}, "a", /*birth=*/true);
    seedTxn(*backend, layout, ns, RefTxnId{1, 2}, "b", /*birth=*/false);
    backend->hidden_keys.insert(layout.refLogKey(RefNamespaceId::stageATransition(ns), RefTxnId{1, 2}));

    auto store = openWalkPool(backend);
    ASSERT_TRUE(store);

    const auto refs = store->listRefs(ns);
    EXPECT_EQ(refs.size(), 2u) << "an omitted TAIL id is indistinguishable from the end of the stream to a "
                                  "listing; the arithmetic probe of last+1 is what finds it";
    EXPECT_TRUE(refs.contains("b"));
}

/// A hint that omits the base SNAPSHOT is harmless for the same reason, but by a different route: the
/// base is chosen as the greatest of (hint, `_ckpt.checkpoint`) and fetched by EXACT KEY, so the
/// checkpoint alone is enough to find it. Without this, a cleaned prefix whose listing lost the newest
/// snapshot would replay from an older base -- or from nothing at all.
TEST(CasRefRecoveryCasWalk, CkptNamesTheBaseSnapshotTheHintLost)
{
    auto backend = std::make_shared<HidingListBackend>();
    const Layout layout("p");
    const RootNamespace ns{"srv1/hint_snap"};

    const RefTxnId base{1, 2};
    writeRefSnapshotRaw(*backend, layout,
        minimalLiveSnapshot(ns.string(), base, {committedRow("a", manifestRef(1, 1, 1))}));
    seedTxn(*backend, layout, ns, RefTxnId{1, 3}, "c", /*birth=*/false);
    seedCkpt(*backend, layout, ns, RefCkpt{.life_epoch = std::optional<uint64_t>{1},
                                           .checkpoint_snapshot_id = base,
                                           .last_epoch_seal = std::nullopt});
    backend->hidden_keys.insert(layout.refSnapshotKey(RefNamespaceId::stageATransition(ns), base));

    auto store = openWalkPool(backend);
    ASSERT_TRUE(store);

    const auto refs = store->listRefs(ns);
    EXPECT_EQ(refs.size(), 2u) << "the checkpoint names the base; the hint's omission of it is irrelevant";
    EXPECT_TRUE(refs.contains("a")) << "'a' exists only inside the snapshot the hint lost";
    EXPECT_TRUE(refs.contains("c"));
}

/// A 404 BELOW a durable same-epoch higher id is not the end of the stream -- it is a HOLE, and a hole
/// in a dense stream is corruption. Recovery restarts (a cleanup racing the read is the innocent
/// explanation), and once its restart budget is spent it FAILS CLOSED. It must never fold what it has:
/// that is precisely how an acked transaction disappears.
TEST(CasRefRecoveryCasWalk, AbsentIdBelowADurableHigherIdRestartsThenFailsClosed)
{
    auto backend = std::make_shared<HidingListBackend>();
    const Layout layout("p");
    const RootNamespace ns{"srv1/hole"};

    seedCkpt(*backend, layout, ns, lifeEpochCkpt(1));
    seedTxn(*backend, layout, ns, RefTxnId{1, 1}, "a", /*birth=*/true);
    /// {1,2} is MISSING while {1,3} is durable and listed: the listing itself witnesses the hole.
    seedTxn(*backend, layout, ns, RefTxnId{1, 3}, "c", /*birth=*/false);

    auto store = openWalkPool(backend);
    ASSERT_TRUE(store);
    store->setCasRetrySleepForTest([](uint64_t) {});

    const uint64_t restarts_before = counterOf(ProfileEvents::CasRefRecoveryRestarts);
    expectThrowsCode(DB::ErrorCodes::CORRUPTED_DATA, [&] { store->listRefs(ns); });
    EXPECT_GT(counterOf(ProfileEvents::CasRefRecoveryRestarts), restarts_before)
        << "the hole must be re-read (a racing cleanup is the innocent explanation) before it is called corruption";
}

/// ---------------------------------------------------------------------------------------------
/// The CAS-walk: closing dead epochs in-band
/// ---------------------------------------------------------------------------------------------

/// The ordinary case: one dead epoch, closed by OUR seal at `{E, T+1}` -- the exact key a dying
/// predecessor's in-flight PUT would have taken, which is what makes the store's conditional create the
/// fence (INV-2) rather than a detector after the fact.
TEST(CasRefRecoveryCasWalk, DeadEpochIsClosedByOurOwnSealAtTPlusOne)
{
    auto backend = std::make_shared<HidingListBackend>();
    const Layout layout("p");
    const RootNamespace ns{"srv1/seal_created"};

    burnEpochsUpTo(*backend, layout, /*target_live_epoch=*/2);
    seedCkpt(*backend, layout, ns, lifeEpochCkpt(1));
    seedTxn(*backend, layout, ns, RefTxnId{1, 1}, "a", /*birth=*/true);

    auto store = openWalkPool(backend);
    ASSERT_TRUE(store);
    ASSERT_EQ(store->liveWriterEpoch(), 2u);

    const uint64_t sealed_before = counterOf(ProfileEvents::CasRefRecoveryEpochSealed);
    ASSERT_EQ(store->listRefs(ns).size(), 1u);
    EXPECT_EQ(counterOf(ProfileEvents::CasRefRecoveryEpochSealed), sealed_before + 1);

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
TEST(CasRefRecoveryCasWalk, ConcurrentRecoverersSealIsAdoptedNotContested)
{
    auto backend = std::make_shared<LateMaterializeBackend>();
    const Layout layout("p");
    const RootNamespace ns{"srv1/seal_adopt"};

    burnEpochsUpTo(*backend, layout, /*target_live_epoch=*/2);
    seedCkpt(*backend, layout, ns, lifeEpochCkpt(1));
    seedTxn(*backend, layout, ns, RefTxnId{1, 1}, "a", /*birth=*/true);
    /// The peer's seal lands between our read of {1,2} and our create of it, so we meet it as an
    /// OCCUPANT rather than as a tail entry.
    backend->late_key = layout.refLogKey(RefNamespaceId::stageATransition(ns), RefTxnId{1, 2});
    backend->late_bytes = sealObject(FormatId::RefLog, encodeRefLogTxn(makeSealTxn(ns, RefTxnId{1, 2})));

    auto store = openWalkPool(backend);
    ASSERT_TRUE(store);

    const uint64_t adopted_before = counterOf(ProfileEvents::CasRefRecoveryEpochSealAdopted);
    ASSERT_EQ(store->listRefs(ns).size(), 1u);
    EXPECT_GT(counterOf(ProfileEvents::CasRefRecoveryEpochSealAdopted), adopted_before);
    EXPECT_EQ(store->lastEpochSealForTest(ns), std::optional<RefTxnId>(RefTxnId{1, 2}))
        << "an adopted seal is this namespace's chain link exactly as a minted one is";
}

/// A STRAGGLER: an ordinary transaction of the dead epoch landed at `{E, T+1}` after our read of the
/// tail and before our seal. The rule is state-derived ids (INV-2): adopt the transaction, advance `T`
/// by ONE, and re-seal at the NEW `T+1`. Never mint `T+2` around it -- that writes a hole into the
/// durable stream that no later reader can tell from a lost object.
TEST(CasRefRecoveryCasWalk, StragglerAtTPlusOneIsAdoptedAndResealedAtTheNewTPlusOne)
{
    auto backend = std::make_shared<LateMaterializeBackend>();
    const Layout layout("p");
    const RootNamespace ns{"srv1/straggler"};

    burnEpochsUpTo(*backend, layout, /*target_live_epoch=*/2);
    seedCkpt(*backend, layout, ns, lifeEpochCkpt(1));
    seedTxn(*backend, layout, ns, RefTxnId{1, 1}, "a", /*birth=*/true);
    /// The dying epoch's last append materializes between our read of {1,2} and our create of it -- the
    /// straggler, arriving exactly where the every-attempt rule says it can.
    backend->late_key = layout.refLogKey(RefNamespaceId::stageATransition(ns), RefTxnId{1, 2});
    backend->late_bytes = sealObject(FormatId::RefLog,
        encodeRefLogTxn(makeOrdinaryTxn(ns, RefTxnId{1, 2}, "late", /*birth=*/false)));

    auto store = openWalkPool(backend);
    ASSERT_TRUE(store);

    const uint64_t straggler_before = counterOf(ProfileEvents::CasRefRecoveryStragglerAdopted);
    const auto refs = store->listRefs(ns);
    EXPECT_EQ(refs.size(), 2u) << "the straggler's transaction is durable and must be applied, not skipped";
    EXPECT_TRUE(refs.contains("late"));
    EXPECT_GT(counterOf(ProfileEvents::CasRefRecoveryStragglerAdopted), straggler_before);

    const auto seal = readLogTxn(*backend, layout, ns, RefTxnId{1, 3});
    ASSERT_TRUE(seal.has_value()) << "the epoch must be re-sealed at the NEW T+1 = {1,3}, never at a blindly minted T+2";
    EXPECT_TRUE(refLogTxnIsEpochSeal(*seal));
    EXPECT_EQ(store->lastEpochSealForTest(ns), std::optional<RefTxnId>(RefTxnId{1, 3}));
}

/// Two BURNED epochs -- mounted, never written to, and abandoned. `CasPool`'s epoch allocator mints and
/// never reclaims, so this is the normal shape of a pool that has restarted a few times, not an
/// anomaly. Each empty epoch is closed by its own sequence-1 seal, and each carries the previous seal as
/// its `prev_epoch_seal`: the chain is what makes a MISSING epoch detectable, which arithmetic within an
/// epoch cannot do.
TEST(CasRefRecoveryCasWalk, TwoBurnedEmptyEpochsProduceTwoChainedSequenceOneSeals)
{
    auto backend = std::make_shared<HidingListBackend>();
    const Layout layout("p");
    const RootNamespace ns{"srv1/burned"};

    burnEpochsUpTo(*backend, layout, /*target_live_epoch=*/4);
    seedCkpt(*backend, layout, ns, lifeEpochCkpt(1));
    seedTxn(*backend, layout, ns, RefTxnId{1, 1}, "a", /*birth=*/true);

    auto store = openWalkPool(backend);
    ASSERT_TRUE(store);
    ASSERT_EQ(store->liveWriterEpoch(), 4u);

    ASSERT_EQ(store->listRefs(ns).size(), 1u);

    const auto seal1 = readLogTxn(*backend, layout, ns, RefTxnId{1, 2});
    ASSERT_TRUE(seal1.has_value()) << "epoch 1 closes at {1,2}";
    EXPECT_EQ(seal1->prev_epoch_seal, std::nullopt);

    const auto seal2 = readLogTxn(*backend, layout, ns, RefTxnId{2, 1});
    ASSERT_TRUE(seal2.has_value()) << "empty epoch 2 still closes -- at its sequence 1";
    EXPECT_TRUE(refLogTxnIsEpochSeal(*seal2));
    EXPECT_EQ(seal2->prev_epoch_seal, std::optional<RefTxnId>(RefTxnId{1, 2}))
        << "a sequence-1 seal MUST name the seal that closed the previous epoch";

    const auto seal3 = readLogTxn(*backend, layout, ns, RefTxnId{3, 1});
    ASSERT_TRUE(seal3.has_value()) << "empty epoch 3 closes too";
    EXPECT_EQ(seal3->prev_epoch_seal, std::optional<RefTxnId>(RefTxnId{2, 1}));

    EXPECT_FALSE(readLogTxn(*backend, layout, ns, RefTxnId{4, 1}).has_value())
        << "epoch 4 is LIVE -- sealing it would close the epoch this mount writes in";
    EXPECT_EQ(store->lastEpochSealForTest(ns), std::optional<RefTxnId>(RefTxnId{3, 1}));
}

/// GENESIS. A namespace born at epoch 5 has no epochs 1-4 of its own: they are not "empty epochs it
/// failed to close", they are epochs before it existed. The walk starts at the namespace's `life_epoch`
/// and writes no phantom seals below it, and with no transition ever having happened it installs NO
/// chain link -- `nullopt` means genesis and must mean it exactly, or the table's first transaction
/// would be required to name a seal that never existed.
TEST(CasRefRecoveryCasWalk, GenesisAtEpochFiveWritesNoPhantomSealsBelowLifeEpoch)
{
    auto backend = std::make_shared<HidingListBackend>();
    const Layout layout("p");
    const RootNamespace ns{"srv1/genesis5"};

    burnEpochsUpTo(*backend, layout, /*target_live_epoch=*/5);
    seedCkpt(*backend, layout, ns, lifeEpochCkpt(5));
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
TEST(CasRefRecoveryCasWalk, FenceBumpedMidWalkRefusesTheInstallAndTheRetrySucceeds)
{
    auto backend = std::make_shared<GetSeamBackend>();
    const Layout layout("p");
    const RootNamespace ns{"srv1/bump_midwalk"};

    seedCkpt(*backend, layout, ns, lifeEpochCkpt(1));
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
    EXPECT_EQ(store->listRefs(ns).size(), 1u) << "the retry runs under the current generation and succeeds";
}

/// Bump point 1 of the trio's two interior seams: AFTER the slot-occupy landed, BEFORE the `_ckpt` CAS.
/// The seal is durable (it was written under a generation that was still valid), but the checkpoint must
/// NOT advance and nothing may be installed. This is the seam a single "check the fence at entry" would
/// miss entirely.
TEST(CasRefRecoveryCasWalk, FenceBumpedAfterSlotOccupyBeforeCkptCasAdvancesNoCheckpoint)
{
    auto backend = std::make_shared<PutHookBackend>();
    const Layout layout("p");
    const RootNamespace ns{"srv1/bump_after_seal"};

    burnEpochsUpTo(*backend, layout, /*target_live_epoch=*/2);
    seedCkpt(*backend, layout, ns, lifeEpochCkpt(1));
    seedTxn(*backend, layout, ns, RefTxnId{1, 1}, "a", /*birth=*/true);

    auto store = openWalkPool(backend);
    ASSERT_TRUE(store);

    const auto ckpt_before = readCkpt(*backend, layout, ns);
    ASSERT_TRUE(ckpt_before.has_value());

    backend->watched_substr = "_log/";
    backend->on_key = [&] { rearmMountFenceAfterAnomalyForTest(store); };

    EXPECT_ANY_THROW(store->listRefs(ns));

    const auto ckpt_after = readCkpt(*backend, layout, ns);
    ASSERT_TRUE(ckpt_after.has_value());
    EXPECT_EQ(ckpt_after->ckpt.last_epoch_seal, std::nullopt)
        << "the seal is durable but the checkpoint must not record it under a generation that moved";
    EXPECT_EQ(ckpt_after->token, ckpt_before->token) << "no CAS was sent at all";
}

/// Bump point 2: AFTER the `_ckpt` CAS, BEFORE the install. The checkpoint advance is harmless (the
/// merge is a semantic maximum, so the retry re-derives the same or a greater value), but the STATE must
/// not be published: this runtime's view belongs to a dead incarnation. Today there is no such recheck
/// at all -- that gap is the whole reason this test exists.
TEST(CasRefRecoveryCasWalk, FenceBumpedAfterCkptCasBeforeInstallPublishesNoState)
{
    auto backend = std::make_shared<PutHookBackend>();
    const Layout layout("p");
    const RootNamespace ns{"srv1/bump_after_ckpt"};

    burnEpochsUpTo(*backend, layout, /*target_live_epoch=*/2);
    seedCkpt(*backend, layout, ns, lifeEpochCkpt(1));
    seedTxn(*backend, layout, ns, RefTxnId{1, 1}, "a", /*birth=*/true);

    auto store = openWalkPool(backend);
    ASSERT_TRUE(store);

    /// The `_ckpt` CAS is the LAST write recovery performs, so hooking it fires strictly between the
    /// checkpoint advance and the install recheck.
    backend->watched_substr = "/_ckpt";
    backend->on_key = [&] { rearmMountFenceAfterAnomalyForTest(store); };

    EXPECT_ANY_THROW(store->listRefs(ns)) << "the install recheck must refuse a result from a moved generation";

    const auto ckpt_after = readCkpt(*backend, layout, ns);
    ASSERT_TRUE(ckpt_after.has_value());
    EXPECT_EQ(ckpt_after->ckpt.last_epoch_seal, std::optional<RefTxnId>(RefTxnId{1, 2}))
        << "the checkpoint advance already landed and is harmless -- the merge is a semantic maximum";

    backend->on_key = nullptr;
    EXPECT_EQ(store->listRefs(ns).size(), 1u) << "the retry under the current generation installs normally";
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
TEST(CasRefRecoveryCasWalk, RemountBarrierBlocksUntilAPausedRecoveryAcknowledgesCancellation)
{
    auto backend = std::make_shared<GetSeamBackend>();
    const Layout layout("p");
    const RootNamespace ns{"srv1/remount_barrier"};

    burnEpochsUpTo(*backend, layout, /*target_live_epoch=*/2);
    seedCkpt(*backend, layout, ns, lifeEpochCkpt(1));
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

    const uint64_t ckpt_before = counterOf(ProfileEvents::CasRefCkptPublished);
    const uint64_t cancelled_before = counterOf(ProfileEvents::CasRefRecoveryCancelled);

    std::thread recovery([&] { try { store->listRefs(ns); } catch (...) {} });

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

    EXPECT_GT(counterOf(ProfileEvents::CasRefRecoveryCancelled), cancelled_before)
        << "the released recovery must observe the cancellation rather than run to completion";
    EXPECT_EQ(counterOf(ProfileEvents::CasRefCkptPublished), ckpt_before)
        << "a cancelled recovery performs ZERO _ckpt CASes";
    EXPECT_FALSE(store->refTableRecoveredForTest(ns)) << "and ZERO installs";
}

/// A `NeedsRecovery` lane replays the known-durable transaction before returning to `Ready`.
TEST(CasRefRecoveryCasWalk, NeedsRecoveryReplaysTheStrandedTxn)
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
    /// store, and this runtime's floor is what keeps the allocator off its id.
    ASSERT_TRUE(readLogTxn(*backend, layout, ns, RefTxnId{1, 2}).has_value())
        << "the stranded transaction must be durable -- otherwise recovery is not owed";

    /// The next touch drives recovery again -- this is the structural closure Task 3 deferred here.
    const auto refs = store->listRefs(ns);
    EXPECT_EQ(refs.size(), 2u);
    EXPECT_TRUE(refs.contains("b")) << "the walk re-derived the stranded transaction from the durable log";
    EXPECT_EQ(store->laneStateForTest(ns), RefLaneState::Ready)
        << "only a completed recovery install returns the lane to Ready";
}

/// ---------------------------------------------------------------------------------------------
/// Fail-closed on an unresolved slot
/// ---------------------------------------------------------------------------------------------

/// `Unresolved` from the slot-occupy means the store will not say whether our seal landed. That is not a
/// state to guess about: recovery takes the transient-retry path and, once its budget is spent, fails
/// closed with the table left unrecovered. Exposing a table whose dead epoch may or may not be closed is
/// the one outcome that must be impossible.
TEST(CasRefRecoveryCasWalk, UnresolvedSealSlotFailsClosedWithoutInstalling)
{
    auto backend = std::make_shared<HidingListBackend>();
    const Layout layout("p");
    const RootNamespace ns{"srv1/unresolved"};

    burnEpochsUpTo(*backend, layout, /*target_live_epoch=*/2);
    seedCkpt(*backend, layout, ns, lifeEpochCkpt(1));
    seedTxn(*backend, layout, ns, RefTxnId{1, 1}, "a", /*birth=*/true);

    /// The backoff sleep ADVANCES the same fake clock the budget is measured against, so the retry
    /// envelope is spent in a handful of iterations instead of spinning against a frozen clock. Not
    /// cosmetic: with a frozen clock this test burns ~700k retries and the same number of log lines,
    /// which is how a real regression in this arm would become invisible in the noise.
    uint64_t fake_now = 1'000'000;
    PoolConfig config = walkTestConfig();
    config.boot_ms_fn = [&fake_now] { return fake_now; };
    auto store = openWalkPool(backend, config);
    ASSERT_TRUE(store);

    store->setCasRetrySleepForTest([&fake_now](uint64_t ms) { fake_now += ms; });
    backend->ambiguous_put_substr = "/_log/";

    EXPECT_ANY_THROW(store->listRefs(ns));
    EXPECT_FALSE(store->refTableRecoveredForTest(ns))
        << "a table whose dead epoch may or may not be closed must never be exposed as recovered";
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
TEST(CasRefRecoveryCasWalk, ALatePredecessorPutAtTheSealedSlotIsRefusedByTheStore)
{
    auto backend = std::make_shared<HidingListBackend>();
    const Layout layout("p");
    const RootNamespace ns{"srv1/ghost"};

    burnEpochsUpTo(*backend, layout, /*target_live_epoch=*/2);
    seedCkpt(*backend, layout, ns, lifeEpochCkpt(1));
    seedTxn(*backend, layout, ns, RefTxnId{1, 1}, "a", /*birth=*/true);

    auto store = openWalkPool(backend);
    ASSERT_TRUE(store);
    ASSERT_EQ(store->listRefs(ns).size(), 1u);

    /// The ghost: the exact append the dead epoch's writer had in flight, arriving late.
    const RefTxnId ghost_id{1, 2};
    const String ghost_bytes = sealObject(FormatId::RefLog,
        encodeRefLogTxn(makeOrdinaryTxn(ns, ghost_id, "ghost", /*birth=*/false)));
    const PutResult put = backend->putIfAbsent(layout.refLogKey(RefNamespaceId::stageATransition(ns), ghost_id), ghost_bytes);
    EXPECT_EQ(put.outcome, PutOutcome::PreconditionFailed)
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
TEST(CasRefRecoveryCasWalk, UndecodableOccupantAtTheSealSlotFailsClosedAndLeavesRecoveryRestartable)
{
    auto backend = std::make_shared<LateMaterializeBackend>();
    const Layout layout("p");
    const RootNamespace ns{"srv1/foreign_slot"};

    burnEpochsUpTo(*backend, layout, /*target_live_epoch=*/2);
    seedCkpt(*backend, layout, ns, lifeEpochCkpt(1));
    seedTxn(*backend, layout, ns, RefTxnId{1, 1}, "a", /*birth=*/true);
    backend->late_key = layout.refLogKey(RefNamespaceId::stageATransition(ns), RefTxnId{1, 2});
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
TEST(CasRefRecoveryCasWalk, ASecondCallerWaitsForTheWalkInsteadOfRacingIt)
{
    auto backend = std::make_shared<GetSeamBackend>();
    const Layout layout("p");
    const RootNamespace ns{"srv1/serialized"};

    burnEpochsUpTo(*backend, layout, /*target_live_epoch=*/2);
    seedCkpt(*backend, layout, ns, lifeEpochCkpt(1));
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

    const uint64_t adopted_before = counterOf(ProfileEvents::CasRefRecoveryEpochSealAdopted);
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
    EXPECT_EQ(counterOf(ProfileEvents::CasRefRecoveryEpochSealAdopted), adopted_before);
    const auto seal = readLogTxn(*backend, layout, ns, RefTxnId{1, 2});
    ASSERT_TRUE(seal.has_value());
    EXPECT_TRUE(refLogTxnIsEpochSeal(*seal));
}

/// A REMOVED namespace's dead epoch is NOT sealed, and the walk keeps going past it.
///
/// Found by the gate, not by design: the first cut sealed every dead epoch unconditionally and then
/// refused to APPLY its own seal, because a seal is a statement about a live stream and `applyOp`
/// rejects one over a Removed table. That combination is the worst of both -- the object was already
/// durable when the apply threw, so the namespace was permanently unrecoverable. The two sides of the
/// rule now agree, and this pins both halves plus the reason skipping the write must not mean skipping
/// the walk: a namespace removed in one epoch and RECREATED in a later one still has durable
/// transactions above the dead life, and stopping at the removal would silently truncate them.
TEST(CasRefRecoveryCasWalk, ARemovedNamespaceIsNotSealedAndTheWalkContinuesToItsRecreation)
{
    auto backend = std::make_shared<HidingListBackend>();
    const Layout layout("p");
    const RootNamespace ns{"srv1/removed_then_reborn"};

    burnEpochsUpTo(*backend, layout, /*target_live_epoch=*/3);
    seedCkpt(*backend, layout, ns, lifeEpochCkpt(1));
    seedTxn(*backend, layout, ns, RefTxnId{1, 1}, "a", /*birth=*/true);

    /// Epoch 1 ends with the terminal record: the ref is removed, then the namespace.
    RefLogTxn removal;
    removal.ns = ns.string();
    removal.txn_id = RefTxnId{1, 2};
    removal.ops = {DB::Cas::tests::ownerTransitionOp(
                       RefOwnerBinding{RefOwnerKind::Committed, "a", manifestRef(1, 1, 1u)}, std::nullopt),
                   removeNamespaceOp()};
    writeRefLogTxnRaw(*backend, layout, removal);

    /// A RECREATION in epoch 2 -- above the dead life, and the reason the walk must not stop at the
    /// removal. Its birth is sequence 1 of its own genesis epoch, so it carries no chain link.
    seedTxn(*backend, layout, ns, RefTxnId{2, 1}, "reborn", /*birth=*/true);

    auto store = openWalkPool(backend);
    ASSERT_TRUE(store);
    ASSERT_EQ(store->liveWriterEpoch(), 3u);

    const auto refs = store->listRefs(ns);
    EXPECT_EQ(refs.size(), 1u);
    EXPECT_TRUE(refs.contains("reborn")) << "the walk must reach the recreated life above the removal";

    EXPECT_FALSE(readLogTxn(*backend, layout, ns, RefTxnId{1, 3}).has_value())
        << "epoch 1 died Removed: no seal, because a seal closes the epoch of a LIVE stream";
    const auto seal2 = readLogTxn(*backend, layout, ns, RefTxnId{2, 2});
    ASSERT_TRUE(seal2.has_value()) << "epoch 2 IS live again by the time it dies, so it closes normally";
    EXPECT_TRUE(refLogTxnIsEpochSeal(*seal2));
    EXPECT_EQ(seal2->prev_epoch_seal, std::nullopt) << "sequence 2 carries no chain link";
}
