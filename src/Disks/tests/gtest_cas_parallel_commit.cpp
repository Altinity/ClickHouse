#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/ContentAddressedMetadataStorage.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/ContentAddressedTransaction.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Parts/PartFolderAccess.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasCommitThreadPool.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasPartWriteTxn.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasPool.h>
#include <Disks/tests/cas_test_helpers.h>
#include <IO/ReadHelpers.h>
#include <IO/SharedThreadPools.h>
#include <Common/ThreadPool.h>
#include <fmt/format.h>
#include <gtest/gtest.h>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <map>
#include <mutex>
#include <sstream>

/// Task 2 of the CAS parallel-write-path plan (docs/superpowers/sdd): `promoteBuild`/`repointRef`
/// return an exact, in-lane-derived `Cas::CommitOutcome` instead of `void`/`bool`, and
/// `dropRefIfMatches` gives a future rollback a conditional drop keyed on that exact outcome instead
/// of the unsafe-under-concurrency `dropRef` (which removes whatever manifest currently occupies the
/// ref name). This suite grows across the later parallel-commit tasks; here it only proves the
/// outcome is exact and that the conditional drop is a true guard -- still single-threaded commit, no
/// concurrency yet.
///
/// Task 3 reworks `ContentAddressedTransaction::commit()`'s rollback to be EXACT (per-part
/// `Cas::CommitOutcome` slots + `dropRefIfMatches`) while the commit loop stays single-threaded --
/// correctness-first, before Task 5 adds concurrency. `CasCommitRollback` below drives real
/// `ContentAddressedTransaction`s (not the bare pool primitives `CaWiringFixture` above exercises)
/// through the exact `publishStaging` call path production `commit()` uses, so the fault seams
/// (`armPromoteFailure`/`armAfterPromoteHook`) fire from the real thing.

using namespace DB;
using namespace DB::Cas::tests;

namespace
{

/// The dedicated CAS commit pool `commit()` dispatches onto is initialized once, process-wide, by the
/// unit-test `main` (`src/Common/tests/gtest_main.cpp`) -- mirroring server startup -- so tests here can
/// commit transactions without arranging it themselves.

/// Fixture mirroring `gtest_cas_part_folder_access.cpp`'s `publishPart`/`cacheOn` helpers: a fresh
/// in-memory pool + a `CachedPartFolderAccess` facade over it, plus the minimal staging helpers this
/// suite's tests need (stage a simple one-file part without promoting it; stage-and-promote it in one
/// call; repoint an already-committed ref onto a fresh manifest, modeling a later writer).
struct CaWiringFixture
{
    std::shared_ptr<Cas::InMemoryBackend> backend = std::make_shared<Cas::InMemoryBackend>();
    Cas::PoolPtr store = openPoolForTest(backend);
    Cas::CachedPartFolderAccess access{store};
    Cas::RootNamespace namespace_{"srv/t1"};
    int content_counter = 0;

    const Cas::RootNamespace & ns() const { return namespace_; }
    Cas::CachedPartFolderAccess & partAccess() { return access; }

    static Cas::ManifestEntry inlineEntry(const String & path, const String & bytes)
    {
        Cas::ManifestEntry e;
        e.path = path;
        e.placement = Cas::EntryPlacement::Inline;
        e.ref = Cas::BlobRef{Cas::BlobHashAlgo::CityHash128, Cas::BlobDigest::fromU128(u128Of(bytes))};
        e.blob_size = bytes.size();
        e.inline_bytes = bytes;
        return e;
    }

    struct Staged
    {
        Cas::PartWriteTxnPtr build;
        Cas::ManifestId id;
    };

    /// Stages a fresh build (manifest + precommit) for `key` over `blobs` inline entries, WITHOUT
    /// promoting it -- the caller drives `promoteBuild` itself so it can observe the exact
    /// `CommitResult` the promote primitive derives.
    Staged stageSimplePart(const Cas::PartRefKey & key, int blobs)
    {
        std::vector<Cas::ManifestEntry> entries;
        for (int i = 0; i < blobs; ++i)
            entries.push_back(inlineEntry(fmt::format("f{}", i), fmt::format("payload-{}-{}", key.ref, i)));
        auto build = store->beginPartWrite(Cas::PartWriteInfo{
            .intended_ref = key.ns.string() + "/" + key.ref, .intended_namespace = key.ns, .op = Cas::ProvenanceOp::Insert});
        const Cas::ManifestId id = build->stageManifest(entries);
        build->precommitAdd(key.ns, key.ref, id);
        return {std::move(build), id};
    }

    /// Stages and promotes one simple part end-to-end, returning the exact `CommitResult`.
    Cas::CommitResult commitSimplePart(const Cas::PartRefKey & key, int blobs)
    {
        auto staged = stageSimplePart(key, blobs);
        return access.promoteBuild(*staged.build, key, staged.build->buildId(), staged.id);
    }

    /// Repoints an already-committed `key` onto a fresh manifest (different content), through the
    /// public `repointRef` primitive -- models "another writer" rebinding the ref after this
    /// fixture's own `commitSimplePart`.
    Cas::CommitResult repointToFreshManifest(const Cas::PartRefKey & key)
    {
        return access.repointRef(key, {inlineEntry("f0", fmt::format("repoint-{}", ++content_counter))},
            Cas::ProvenanceOp::Other);
    }
};

}

TEST(CasCommitOutcome, PromoteReportsCreatedAndManifest)
{
    CaWiringFixture fx;
    const Cas::PartRefKey key{fx.ns(), "20260101_1_1_0"};
    auto staged = fx.stageSimplePart(key, /*blobs=*/1);

    const Cas::CommitResult oc = fx.partAccess().promoteBuild(*staged.build, key, staged.build->buildId(), staged.id);

    /// `promoteBuild` now returns the allocation-free `CommitResult` (Fix 2): the two fields derived
    /// INSIDE the durable `appendRefOps` builder, no identity strings (the caller already knows `key`).
    EXPECT_TRUE(oc.created);
    EXPECT_EQ(oc.manifest_ref, staged.id.ref);
}

TEST(CasCommitOutcome, DropRefIfMatchesRemovesOnlyExact)
{
    CaWiringFixture fx;
    const Cas::PartRefKey key{fx.ns(), "20260101_2_2_0"};
    const Cas::CommitResult oc1 = fx.commitSimplePart(key, /*blobs=*/1);
    EXPECT_TRUE(oc1.created);

    /// Rebind key -> M2 (a legitimate repoint by "another writer").
    const Cas::CommitResult oc2 = fx.repointToFreshManifest(key);
    EXPECT_FALSE(oc2.created);
    ASSERT_NE(oc1.manifest_ref, oc2.manifest_ref);

    /// Conditional drop keyed on the STALE M1 must NOT remove the current M2 binding.
    EXPECT_FALSE(fx.partAccess().dropRefIfMatches(key, oc1.manifest_ref));
    EXPECT_TRUE(fx.partAccess().existsRef(key, Cas::Freshness::ForceFresh));

    /// Conditional drop keyed on the CURRENT M2 removes it.
    EXPECT_TRUE(fx.partAccess().dropRefIfMatches(key, oc2.manifest_ref));
    EXPECT_FALSE(fx.partAccess().existsRef(key, Cas::Freshness::ForceFresh));
}

TEST(CasCommitOutcome, DropRefIfMatchesOnAbsentRefIsANoOp)
{
    CaWiringFixture fx;
    const Cas::PartRefKey key{fx.ns(), "20260101_3_3_0"};
    Cas::ManifestRef bogus;
    EXPECT_FALSE(fx.partAccess().dropRefIfMatches(key, bogus)) << "no committed ref at all: nothing to match";
    EXPECT_FALSE(fx.partAccess().existsRef(key, Cas::Freshness::ForceFresh));
}

/// `repointRef`'s byte-equal candidate is a documented ZERO-pool-mutation no-op (it must not mint a
/// fresh manifest just to compare it). The returned `CommitResult` must still describe reality: the
/// CURRENTLY committed manifest, unchanged, `created=false`.
TEST(CasCommitOutcome, RepointRefByteEqualNoOpReportsCurrentManifestNotCreated)
{
    CaWiringFixture fx;
    const Cas::PartRefKey key{fx.ns(), "20260101_4_4_0"};
    const Cas::CommitResult oc1 = fx.commitSimplePart(key, /*blobs=*/1);

    const Cas::CommitResult oc_noop = fx.partAccess().repointRef(
        key, {CaWiringFixture::inlineEntry("f0", fmt::format("payload-{}-0", key.ref))}, Cas::ProvenanceOp::Other);
    EXPECT_FALSE(oc_noop.created);
    EXPECT_EQ(oc_noop.manifest_ref, oc1.manifest_ref);
}

namespace
{

/// Fixture for the `CasCommitRollback` suite: wraps a real `ContentAddressedMetadataStorage` and
/// drives ordinary `ContentAddressedTransaction`s through disk paths, so the fault seams under test
/// (`ContentAddressedMetadataStorage::armPromoteFailureForTest`/`setAfterPromoteHookForTest`, the
/// minimal test-only hooks this task adds) fire from the SAME `publishStaging` call path production
/// `commit()` uses -- unlike `CaWiringFixture` above, which pokes the bare pool primitives directly.
/// Every part in one fixture instance shares ONE fixed table uuid (and therefore one `RootNamespace`),
/// matching every test's single `fx.ns()`.
struct CaTxnRollbackFixture
{
    static constexpr const char * kTableUuid = "c3c3c3c3-0000-4000-8000-c3c3c3c3c3c3";

    std::shared_ptr<DB::ContentAddressedMetadataStorage> storage;
    Cas::RootNamespace namespace_;
    Cas::ManifestRef last_repoint_manifest;
    int content_counter = 0;

    static std::string tablePrefix()
    {
        return std::string(kTableUuid).substr(0, 3) + "/" + kTableUuid;
    }

    const Cas::RootNamespace & ns() const { return namespace_; }
    Cas::CachedPartFolderAccess & partAccess() { return *storage->partAccess(); }

    DB::MetadataTransactionPtr beginTxn() { return storage->createTransaction(); }

    /// Stages `blobs` small distinct files for `key` under a tmp build dir and re-keys them to the
    /// final ref name -- the standard MergeTree-insert shape (`gtest_ca_transaction.cpp`'s
    /// `writeFileTx` + `moveDirectory` idiom) this storage's routing expects; `key.ns` must be `ns()`.
    void stageInto(const DB::MetadataTransactionPtr & txn, const Cas::PartRefKey & key, int blobs)
    {
        auto & ca_tx = dynamic_cast<DB::ContentAddressedTransaction &>(*txn);
        const std::string tmp_dir = tablePrefix() + "/tmp_insert_" + key.ref;
        for (int i = 0; i < blobs; ++i)
        {
            auto buf = ca_tx.writeFile(fmt::format("{}/f{}.bin", tmp_dir, i), 65536, DB::WriteMode::Rewrite, {});
            const std::string bytes = fmt::format("payload-{}-{}", key.ref, i);
            buf->write(bytes.data(), bytes.size());
            buf->finalize();
        }
        txn->moveDirectory(tmp_dir, tablePrefix() + "/" + key.ref);
    }

    /// Stages and commits one part end-to-end in its own transaction -- sets up a pre-existing
    /// committed ref before the transaction under test begins.
    void commitSimplePart(const Cas::PartRefKey & key, int blobs)
    {
        auto txn = beginTxn();
        stageInto(txn, key, blobs);
        txn->commit(DB::NoCommitOptions{});
    }

    /// Repoints an already-committed `key` onto a fresh manifest through the public `repointRef`
    /// primitive directly -- models "another writer" rebinding the ref concurrently with the
    /// transaction under test. Records the manifest for `lastRepointManifest()`.
    void repointToFreshManifest(const Cas::PartRefKey & key)
    {
        const std::string bytes = fmt::format("repoint-{}", ++content_counter);
        Cas::ManifestEntry e;
        e.path = "f0.bin";
        e.placement = Cas::EntryPlacement::Inline;
        e.ref = Cas::BlobRef{Cas::BlobHashAlgo::CityHash128, Cas::BlobDigest::fromU128(u128Of(bytes))};
        e.blob_size = bytes.size();
        e.inline_bytes = bytes;
        const auto oc = partAccess().repointRef(key, {e}, Cas::ProvenanceOp::Other);
        last_repoint_manifest = oc.manifest_ref;
    }

    /// The manifest CURRENTLY bound to `key`, or a default-constructed (zero) `ManifestRef` when `key`
    /// has no committed ref at all.
    Cas::ManifestRef currentManifest(const Cas::PartRefKey & key)
    {
        auto view = partAccess().getView(key, Cas::Freshness::ForceFresh);
        return view ? view->manifestId().ref : Cas::ManifestRef{};
    }

    Cas::ManifestRef lastRepointManifest() const { return last_repoint_manifest; }

    /// Test-only fault seam (see `ContentAddressedMetadataStorage::armPromoteFailureForTest`): the
    /// NEXT `publishStaging` promote/repoint for `key` (the full `(ns, ref)` routed identity) throws
    /// instead of committing.
    void armPromoteFailure(const Cas::PartRefKey & key) { storage->armPromoteFailureForTest(key); }
    /// Test-only hook (see `ContentAddressedMetadataStorage::setAfterPromoteHookForTest`): runs once,
    /// synchronously, immediately after `key`'s promote/repoint confirms.
    void armAfterPromoteHook(const Cas::PartRefKey & key, std::function<void()> hook)
    {
        storage->setAfterPromoteHookForTest(key, std::move(hook));
    }

    /// Force the per-part commit fan-out (Task 5): 1 = sequential oracle, N = N bounded worker-loops.
    void setCommitConcurrency(uint64_t n) { storage->setCommitConcurrencyForTest(n); }

    /// Rendezvous for the join-before-rollback test. The "slow" part's before-promote hook blocks
    /// until the "poison" part's before-promote hook releases it -- a real handshake, NO sleep -- so the
    /// slow commit worker is provably still in-flight when the poison worker throws. `slow_released_by_poison`
    /// records whether the bounded wait woke on the poison signal (genuine concurrency) rather than on the
    /// timeout (a serial regression). The slow part's COMPLETION marker is set from the separate
    /// after-publish seam (see the test), which fires only once `publishStaging` has FULLY returned.
    struct SlowRendezvous
    {
        std::mutex m;
        std::condition_variable cv;
        bool poison_failed = false;
        std::atomic<bool> slow_released_by_poison{false};
    };
    std::shared_ptr<SlowRendezvous> rendezvous = std::make_shared<SlowRendezvous>();

    void onFirstDropRefIfMatches(std::function<void()> cb)
    {
        storage->setBeforeDropRefIfMatchesHookForTest(std::move(cb));
    }
    /// True iff the slow worker's bounded wait was released by the poison worker's signal (proving the two
    /// ran on different workers at once), not by the wait timing out (which a serial regression would hit).
    bool slowReleasedByPoison() const { return rendezvous->slow_released_by_poison.load(); }

    /// Arm `slow` to block mid-`publishStaging` until `poison`'s promote fails, and `poison` to release
    /// `slow` and then fail its promote. With commit concurrency >= 2 they land on different workers.
    void holdSlowWorkerUntilPoisonFails(const Cas::PartRefKey & slow, const Cas::PartRefKey & poison)
    {
        auto rv = rendezvous;
        storage->setBeforePromoteHookForTest(slow, [rv]
        {
            std::unique_lock lk(rv->m);
            /// Bounded wait: a safety net only -- `poison_failed` is signalled by the poison worker under
            /// commit concurrency >= 2. Never a fixed sleep to sequence threads. Record whether the wake
            /// was the poison signal (true) or the timeout (false) so the test can assert real overlap.
            const bool released = rv->cv.wait_for(lk, std::chrono::seconds(60), [&] { return rv->poison_failed; });
            rv->slow_released_by_poison.store(released);
        });
        storage->setBeforePromoteHookForTest(poison, [rv]
        {
            {
                std::lock_guard lk(rv->m);
                rv->poison_failed = true;
            }
            rv->cv.notify_all();
        });
        storage->armPromoteFailureForTest(poison);
    }

    /// Two-party rendezvous for the hardlink-shared-blob test: both parts' before-upload hooks arrive
    /// here and neither proceeds into the shared-blob upload until BOTH have arrived, so their uploads
    /// provably overlap (exercising the concurrent putIfAbsent / 412-adopt path on one content key).
    struct UploadBarrier
    {
        std::mutex m;
        std::condition_variable cv;
        size_t arrived = 0;
        std::atomic<bool> both_arrived{false};
    };
    std::shared_ptr<UploadBarrier> upload_barrier = std::make_shared<UploadBarrier>();

    void forceUploadsToOverlap(const Cas::PartRefKey & a, const Cas::PartRefKey & b)
    {
        auto bar = upload_barrier;
        auto hook = [bar]
        {
            std::unique_lock lk(bar->m);
            ++bar->arrived;
            bar->cv.notify_all();
            /// Bounded safety net; both parts always arrive under commit concurrency >= 2.
            if (bar->cv.wait_for(lk, std::chrono::seconds(30), [&] { return bar->arrived >= 2; }))
                bar->both_arrived.store(true);
        };
        storage->setBeforeUploadHookForTest(a, hook);
        storage->setBeforeUploadHookForTest(b, hook);
    }
    bool bothUploadsOverlapped() const { return upload_barrier->both_arrived.load(); }

    /// Reads back one committed part file's blob payload (excluding the envelope) through the same
    /// locate + `readBlobPayload` pipeline production reads use -- proving the shared blob is durable
    /// and its bytes are intact, not merely that the ref exists.
    std::string readCommittedBlob(const Cas::PartRefKey & key, const std::string & file)
    {
        auto view = partAccess().getView(key, Cas::Freshness::ForceFresh);
        const auto * entry = view ? view->findFile(file) : nullptr;
        if (!entry)
            return "<absent>";
        const auto location = storage->store()->locate(*entry);
        auto buf = storage->readBlobPayload(location, file, {});
        std::string out;
        DB::readStringUntilEOF(out, *buf);
        return out;
    }

    /// Stage TWO parts that share ONE pending blob: write the blob into part A, then `createHardLink`
    /// it into part B (copying the pending record). Committed in parallel, both parts upload the SAME
    /// content key -- driving the concurrent putIfAbsent/412/adopt path on one blob.
    void stageSharedBlobIntoTwoParts(const DB::MetadataTransactionPtr & txn,
                                     const Cas::PartRefKey & key_a, const Cas::PartRefKey & key_b)
    {
        auto & ca = dynamic_cast<DB::ContentAddressedTransaction &>(*txn);
        const std::string a_file = tablePrefix() + "/" + key_a.ref + "/data.bin";
        const std::string b_file = tablePrefix() + "/" + key_b.ref + "/data.bin";
        const std::string bytes = "shared-blob-payload";
        auto buf = ca.writeFile(a_file, 65536, DB::WriteMode::Rewrite, {});
        buf->write(bytes.data(), bytes.size());
        buf->finalize();
        txn->createHardLink(a_file, b_file);
    }

    /// A canonical, encoding-independent fold of the committed state: every live ref, each ref's
    /// manifest entries (path, placement, blob-hash identity, size, inline payload) sorted by path, and
    /// a global blob in-degree. Deliberately EXCLUDES allocated identities (`ManifestRef`
    /// writer_epoch/build_sequence) and timestamps, which legitimately differ between a sequential and a
    /// parallel commit -- only the LOGICAL state must match.
    std::string foldedLogicalState()
    {
        std::map<std::string, int64_t> in_degree;
        std::ostringstream out;
        for (const auto & [ref_name, _] : storage->store()->listRefs(ns()))
        {
            auto view = partAccess().getView({ns(), ref_name}, Cas::Freshness::ForceFresh);
            if (!view)
                continue;
            out << "REF " << ref_name << "\n";
            std::vector<Cas::ManifestEntry> entries = view->manifest()->entries;
            std::sort(entries.begin(), entries.end(),
                      [](const Cas::ManifestEntry & a, const Cas::ManifestEntry & b) { return a.path < b.path; });
            for (const auto & e : entries)
            {
                const std::string ident =
                    std::to_string(static_cast<int>(e.ref.algo)) + ":" + DB::Cas::u128ToHex(e.ref.digest.toU128());
                out << "  " << e.path << " placement=" << static_cast<int>(e.placement)
                    << " ident=" << ident << " size=" << e.size();
                if (e.placement == Cas::EntryPlacement::Inline)
                    out << " inline=" << e.inline_bytes;
                out << "\n";
                in_degree[ident] += 1;
            }
        }
        out << "INDEGREE\n";
        for (const auto & [ident, count] : in_degree)
            out << "  " << ident << "=" << count << "\n";
        return out.str();
    }
};

CaTxnRollbackFixture makeCaWiringFixture()
{
    static std::atomic<uint64_t> counter{0};
    const auto scratch = std::filesystem::temp_directory_path()
        / fmt::format("ca_commit_rollback_scratch_{}_{}", ::getpid(), counter.fetch_add(1));
    auto settings = DB::Cas::tests::makeSettingsForTest("test", scratch);
    auto storage = std::make_shared<DB::ContentAddressedMetadataStorage>(
        DB::Cas::tests::makeLocalObjectStorageForTest(), "pool", "srv1", "", nullptr, settings);
    storage->startup();

    CaTxnRollbackFixture fx;
    fx.storage = storage;
    fx.namespace_ = storage->liveNamespace(CaTxnRollbackFixture::kTableUuid);
    return fx;
}

}

/// [TXN-ONE-PIPELINE] Task 3: `commit()` publishes `new_a` (created=true) then fails on `new_b`'s
/// promote. The rollback must drop the just-created `new_a` (absent afterward) but never touch the
/// unrelated `pre_existing` ref committed by an EARLIER, already-finished transaction.
TEST(CasCommitRollback, AbsentBeforeDroppedPreExistingUntouched)
{
    auto fx = makeCaWiringFixture();
    const Cas::PartRefKey pre{fx.ns(), "pre_existing_1_1_0"};
    fx.commitSimplePart(pre, 1);                                  // a pre-existing ref, must survive
    // A transaction that commits one NEW part then fails on a second part's promote.
    auto txn = fx.beginTxn();
    fx.stageInto(txn, {fx.ns(), "new_a_1_1_0"}, 1);
    fx.stageInto(txn, {fx.ns(), "new_b_1_1_0"}, 1);
    fx.armPromoteFailure({fx.ns(), "new_b_1_1_0"});               // fault injection in publishStaging's promote
    EXPECT_ANY_THROW(txn->commit({}));
    EXPECT_FALSE(fx.partAccess().existsRef({fx.ns(), "new_a_1_1_0"}, Cas::Freshness::ForceFresh)); // rolled back
    EXPECT_TRUE (fx.partAccess().existsRef(pre, Cas::Freshness::ForceFresh));                       // untouched
}

/// [TXN-ONE-PIPELINE] Task 3: T1 (this transaction) promotes `shared` (M1), then a concurrent writer
/// (modeled by the after-promote hook) repoints it to M2 BEFORE T1's own commit later fails on
/// `poison`'s promote. Rollback must use `dropRefIfMatches(M1)`: M1 != the now-current M2, so the
/// conditional drop must leave `shared` bound to M2 untouched.
///
/// `commit()` publishes `parts` in the map's own (ns, ref) sort order -- so the "shared" part is named
/// `a_shared_...` and the "poison" part `z_poison_...` here purely so `'a' < 'z'` makes "shared"
/// publish (and get repointed by the hook) deterministically BEFORE "poison" fails; this is a test
/// naming choice, not a production ordering guarantee.
TEST(CasCommitRollback, RepointByOtherWriterSurvivesRollback)
{
    auto fx = makeCaWiringFixture();
    const Cas::PartRefKey key{fx.ns(), "a_shared_1_1_0"};
    auto txn = fx.beginTxn();
    fx.stageInto(txn, key, 1);                                   // T1 will create R -> M1
    fx.armAfterPromoteHook(key, [&]{ fx.repointToFreshManifest(key); }); // T2 repoints R -> M2 right after T1's promote
    fx.stageInto(txn, {fx.ns(), "z_poison_1_1_0"}, 1);
    fx.armPromoteFailure({fx.ns(), "z_poison_1_1_0"});
    EXPECT_ANY_THROW(txn->commit({}));
    // T1's rollback used dropRefIfMatches(M1); M2 != M1 so it must survive.
    EXPECT_TRUE(fx.partAccess().existsRef(key, Cas::Freshness::ForceFresh));
    EXPECT_EQ(fx.currentManifest(key), fx.lastRepointManifest());
}

/// Fix 2: the per-part outcome slot is engaged with the IDENTITY before the durable promote and
/// finalized with no-throw POD (`CommitResult`) assignments AFTER it, so a throw in the POST-promote
/// path can never lose the just-created ref. This injects a throw in the after-promote hook -- which
/// runs AFTER the durable append and AFTER the slot is fully written -- and asserts the created ref is
/// still rolled back. Before the fix, the outcome was COPIED into the slot after the durable commit
/// (`out_slot = oc`, a `bad_alloc`-capable string copy); had that copy thrown, the slot would stay
/// disengaged and rollback could not drop the ref. Here the throw is deterministic, standing in for
/// that allocation failure, and proves the slot is engaged+written before any post-promote work.
TEST(CasCommitRollback, OutcomeSlotSurvivesPostPromoteThrow)
{
    auto fx = makeCaWiringFixture();
    fx.setCommitConcurrency(1);
    const Cas::PartRefKey key{fx.ns(), "post_promote_throw_1_1_0"};
    auto txn = fx.beginTxn();
    fx.stageInto(txn, key, 1);
    fx.armAfterPromoteHook(key, [] { throw std::runtime_error("injected post-promote failure"); });
    EXPECT_ANY_THROW(txn->commit({}));
    EXPECT_FALSE(fx.partAccess().existsRef(key, Cas::Freshness::ForceFresh))
        << "the ref created just before the post-promote throw must have been rolled back "
           "(outcome slot was engaged + written before the throw)";
}

/// Task 4 of the CAS parallel-write-path plan: `cas_commit_concurrency` (a `PoolConfig` tunable) +
/// `DB::Cas::getCasCommitThreadPool()` (a dedicated, process-wide pool for Task 5's per-part commit
/// dispatch). No behavior change yet -- this test only proves the two pieces exist and that the pool
/// is genuinely disjoint from the S3 writer pool, which is the entire point of a dedicated pool (a
/// commit worker blocks in `WriteBufferFromS3::finalize` waiting on the writer pool; if the worker
/// were itself a writer-pool thread, that wait would deadlock one level down).
///
/// `Context::getThreadPoolWriter()` is a non-static member function reachable only through a fully
/// bootstrapped `Context` (it lazily constructs `shared->threadpool_writer`, sized from
/// `threadpool_writer_pool_size`/`threadpool_writer_queue_size` -- see `Context::getThreadPoolWriter`
/// in `Interpreters/Context.cpp`), so this lightweight disk-layer unit test cannot reach it directly.
/// Disjointness is proven STRUCTURALLY instead, per three independent facts:
///   1) `getCasCommitThreadPool()`'s `ThreadPool` lives in a function-local static PRIVATE to
///      `CasCommitThreadPool.cpp` (the `CasCommitPoolHolder` singleton) -- no other translation unit
///      can obtain a reference to it except through this accessor.
///   2) `Context::getThreadPoolWriter()`'s `ThreadPool` lives in `shared->threadpool_writer`, a member
///      of `Context::ContextSharedPart` -- a completely different object, in a completely different
///      translation unit, with a completely different sizing setting.
///   3) `getIOThreadPool()`'s `ThreadPool` (checked below, since it IS reachable from a unit test) is
///      a third, separately-declared static in `IO/SharedThreadPools.cpp`.
/// Three distinct statics in three distinct translation units never alias one another; the runtime
/// check below additionally proves (1) and (3) apart, and that (1) is a stable singleton.
TEST(CasCommitPool, DistinctFromWriterPoolAndBounded)
{
    /// `getCasCommitThreadPool()` throws `LOGICAL_ERROR` unless `initializeCasCommitThreadPool()` ran
    /// first (no lazy self-initializing fallback -- see the header's doc comment). The unit-test `main`
    /// (`src/Common/tests/gtest_main.cpp`) wires that once, process-wide, mirroring server startup, so by
    /// the time any test runs the pool is available.
    ThreadPool & cas_commit_pool = DB::Cas::getCasCommitThreadPool();

    /// Singleton stability: the same object every call (a prerequisite for "disjoint from the writer
    /// pool" to mean anything -- a pool that handed out a fresh object per call would trivially be
    /// "distinct" from everything, including itself).
    EXPECT_EQ(&cas_commit_pool, &DB::Cas::getCasCommitThreadPool());

    /// `initializeWithDefaultSettingsIfNotInitialized` is idempotent (guarded by the same
    /// `std::call_once` as an explicit `initialize()`), so this is safe even if another test in this
    /// same process already initialized (or will initialize) the IO pool.
    DB::getIOThreadPool().initializeWithDefaultSettingsIfNotInitialized();
    EXPECT_NE(static_cast<void *>(&cas_commit_pool), static_cast<void *>(&DB::getIOThreadPool().get()));

    /// Default concurrency setting is present.
    EXPECT_EQ(DB::Cas::PoolConfig{}.cas_commit_concurrency, 16u);
}

/// Fix 1: a zero-size commit pool is a MISCONFIGURATION, not a valid state -- a pool with no threads can
/// never run a scheduled callback, so `commit()`'s `waitForAllToFinish` would block forever on every
/// nonempty commit. `initializeCasCommitThreadPool(0, ...)` must throw (fail-closed, no silent clamp).
/// The zero-size check runs BEFORE the double-init check, so this assertion exercises the validation
/// path WITHOUT poisoning the process-wide singleton the other tests share: the throw leaves the
/// already-initialized pool untouched, verified by fetching it again afterward.
TEST(CasCommitPool, RejectsZeroSize)
{
    EXPECT_THROW(DB::Cas::initializeCasCommitThreadPool(0, 0, 10000), DB::Exception);
    /// The shared pool the unit-test main initialized is still intact (the throw did not touch it).
    EXPECT_NO_THROW((void)DB::Cas::getCasCommitThreadPool());
}

/// Task 5 -- the payoff. `commit()` now dispatches per-part `publishStaging` onto the dedicated CAS
/// commit pool at a bounded fan-out (`cas_commit_concurrency`), with the join structurally ordered
/// before rollback. These tests exercise the four hazards that concurrency introduces.

/// SATURATION / no self-wait deadlock AND a bounded, non-serial dispatch. A commit-pool bound of N with
/// > N parts must complete, schedule exactly `min(N, parts)` worker callbacks, keep exactly that many
/// simultaneously active (a rendezvous barrier in the probe proves they overlap), and process every part
/// index exactly once. This gates BOTH regressions the design forbids: a one-task-per-part impl would
/// schedule `parts` callbacks (not N), and a serial impl would peak at 1 active worker (not N).
///
/// The probe's per-worker barrier waits (bounded) until all `min(N, parts)` workers are simultaneously
/// active, so `peak_active` reaching N is real overlap, not a fluke of scheduling. A serial regression
/// never reaches the rendezvous and falls through on the timeout with `peak_active == 1`, failing the
/// EXPECT rather than hanging. The whole run is additionally wrapped in `timeout` by the harness.
TEST(CasParallelCommit, SaturationBoundedCompletion)
{
    auto fx = makeCaWiringFixture();
    fx.setCommitConcurrency(2);                       // N = 2 worker-loops (< the 5 parts below)
    DB::ContentAddressedMetadataStorage::CommitWorkerProbeForTest probe;
    fx.storage->setCommitWorkerProbeForTest(&probe);
    auto txn = fx.beginTxn();
    for (int i = 0; i < 5; ++i)                       // N + 3 parts, one blob each
        fx.stageInto(txn, {fx.ns(), fmt::format("p_{}_1_1_0", i)}, 1);
    ASSERT_NO_THROW(txn->commit({}));                 // must return; a self-wait deadlock would hang
    fx.storage->setCommitWorkerProbeForTest(nullptr); // detach before the stack probe dies

    EXPECT_EQ(probe.callbacks_started.load(), 2u)
        << "exactly min(concurrency, parts) worker callbacks -- a one-task-per-part regression schedules 5";
    EXPECT_EQ(probe.peak_active.load(), 2u)
        << "peak simultaneous-active workers must equal the bound -- a serial regression peaks at 1";
    EXPECT_EQ(probe.parts_processed.load(), 5u)
        << "every part index pulled from the cursor exactly once";
    for (int i = 0; i < 5; ++i)
        EXPECT_TRUE(fx.partAccess().existsRef({fx.ns(), fmt::format("p_{}_1_1_0", i)}, Cas::Freshness::ForceFresh));
}

/// JOIN BEFORE ROLLBACK: a slow worker is held mid-`publishStaging` while a second (poison) part's
/// promote fails. The rollback's first `dropRefIfMatches` must observe the slow part's publish already
/// FULLY returned, proving the drain (runner destructor / `waitForAllToFinish`) completed before the
/// catch's rollback.
///
/// Two things make this regression-sensitive rather than a trivial pass:
///   * The completion marker (`slow_done`) is set from the POST-PUBLISH seam -- it fires only once the
///     slow part's `publishStaging` has FULLY returned, so a broken impl that set a marker mid-publish
///     (as the old before-promote-hook version did) could no longer satisfy it early.
///   * The slow part is named to sort FIRST (`a_slow` < `z_poison`), so a serial / single-worker
///     regression pulls slow first and blocks in its before-promote hook waiting for poison, which that
///     same worker never gets to -> the bounded wait TIMES OUT. `slowReleasedByPoison()` asserts the
///     wake was the poison signal (genuine two-worker overlap), so a timeout (serial) fails the test
///     instead of quietly passing after 60s.
TEST(CasParallelCommit, JoinPrecedesRollbackUnderSlowWorker)
{
    auto fx = makeCaWiringFixture();
    fx.setCommitConcurrency(4);
    auto txn = fx.beginTxn();
    const Cas::PartRefKey slow{fx.ns(), "a_slow_1_1_0"};      // sorts BEFORE poison -> processed first
    const Cas::PartRefKey poison{fx.ns(), "z_poison_1_1_0"};
    fx.stageInto(txn, slow, 1);
    fx.stageInto(txn, poison, 1);
    fx.holdSlowWorkerUntilPoisonFails(slow, poison);

    std::atomic<bool> slow_done{false};
    std::atomic<bool> rollback_ran{false};
    // Completion marker from the POST-PUBLISH seam: set only once slow's publishStaging fully returned.
    fx.storage->setAfterPublishHookForTest(slow, [&] { slow_done = true; });
    fx.onFirstDropRefIfMatches([&]
    {
        rollback_ran = true;
        EXPECT_TRUE(slow_done.load()) << "rollback ran before the slow worker's publish fully returned (no drain)";
    });

    EXPECT_ANY_THROW(txn->commit({}));
    EXPECT_TRUE(rollback_ran.load()) << "the created 'slow' ref must have been rolled back";
    EXPECT_TRUE(fx.slowReleasedByPoison())
        << "slow was released by the poison worker's signal, not by the wait timeout -- proves genuine "
           "two-worker overlap (a serial regression would time out here)";
    // The slow ref was created then rolled back; the poison ref never committed.
    EXPECT_FALSE(fx.partAccess().existsRef(slow, Cas::Freshness::ForceFresh));
    EXPECT_FALSE(fx.partAccess().existsRef(poison, Cas::Freshness::ForceFresh));
}

/// HARDLINK-SHARED PENDING BLOB: two parts share ONE pending blob (createHardLink), committed in
/// parallel. A before-upload barrier forces both workers to reach the shared-blob upload before either
/// proceeds, so both upload the SAME content key CONCURRENTLY -- one wins putIfAbsent, the other takes
/// the 412/adopt path (without the barrier the two uploads could serialize and never exercise that
/// race). Both refs must commit, and reading BACK both hardlinked payloads (not just `existsRef`) proves
/// the shared blob is durable and byte-consistent for both parts.
TEST(CasParallelCommit, HardlinkSharedPendingBlobParallel)
{
    auto fx = makeCaWiringFixture();
    fx.setCommitConcurrency(4);
    const Cas::PartRefKey a{fx.ns(), "h_a_1_1_0"};
    const Cas::PartRefKey b{fx.ns(), "h_b_1_1_0"};
    auto txn = fx.beginTxn();
    fx.stageSharedBlobIntoTwoParts(txn, a, b);
    fx.forceUploadsToOverlap(a, b);                  // both reach the shared-blob upload before either proceeds
    ASSERT_NO_THROW(txn->commit({}));
    EXPECT_TRUE(fx.bothUploadsOverlapped()) << "the two shared-blob uploads must have genuinely overlapped";
    EXPECT_TRUE(fx.partAccess().existsRef(a, Cas::Freshness::ForceFresh));
    EXPECT_TRUE(fx.partAccess().existsRef(b, Cas::Freshness::ForceFresh));
    // Read back BOTH hardlinked payloads: the shared blob must be durable and identical for both parts.
    EXPECT_EQ(fx.readCommittedBlob(a, "data.bin"), "shared-blob-payload");
    EXPECT_EQ(fx.readCommittedBlob(b, "data.bin"), "shared-blob-payload");
}

/// SEMANTIC DETERMINISM: a forced-sequential commit (concurrency 1) and a parallel commit
/// (concurrency 8) of the SAME deterministic multi-part plan must fold to the SAME logical state --
/// bindings + manifest entries + payloads + blob in-degree. Allocated IDs/timestamps legitimately
/// differ and are excluded by `foldedLogicalState`.
TEST(CasParallelCommit, ParallelMatchesSequentialLogicalState)
{
    auto commitPlan = [](uint64_t concurrency)
    {
        auto fx = makeCaWiringFixture();
        fx.setCommitConcurrency(concurrency);
        auto txn = fx.beginTxn();
        for (int p = 0; p < 20; ++p)                                  // 20 parts, 3 blobs each
            fx.stageInto(txn, {fx.ns(), fmt::format("part_{:04}_1_1_0", p)}, 3);
        txn->commit({});
        return fx.foldedLogicalState();
    };
    EXPECT_EQ(commitPlan(1), commitPlan(8));
}
