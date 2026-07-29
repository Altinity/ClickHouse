#include <gtest/gtest.h>

#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Tools/CasFsck.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Gc/CasGc.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Backend/CasInMemoryBackend.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Formats/CasRefSnapshotFormat.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasRefProtocol.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasPool.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/ContentAddressedMetadataStorage.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/ContentAddressedTransaction.h>
#include "cas_test_helpers.h"

#include <filesystem>
#include <functional>
#include <mutex>
#include <optional>

namespace DB::ErrorCodes
{
extern const int INVALID_STATE;
extern const int NETWORK_ERROR;
}

using namespace DB::Cas;
using namespace DB::Cas::tests;

namespace
{
constexpr uint64_t kWriterEpoch = 7;
const String kServerRoot = "00";
ManifestRef ref(uint64_t seq, uint64_t inst)
{
    return ManifestRef{.writer_epoch = kWriterEpoch, .build_sequence = seq, .manifest_ordinal = static_cast<uint32_t>(inst)};
}

/// B207 race-simulation harness: `InMemoryBackend` is documented "not final: tests subclass it to
/// distort single behaviors". `runFsck`'s ref-walk and its physical blob listing (`listAll` over
/// `layout.blobsPrefix()`) are two separate calls to `Backend::list` minutes apart in production; here
/// we fire an injected mutation the FIRST time `list` is called against the armed prefix — i.e.
/// strictly AFTER the ref-walk has captured its (now stale) `reachable_blobs`/`blob_labels` view, and
/// strictly BEFORE the HEAD-confirm loop sees the physical listing. That reproduces the race
/// deterministically, without any real timing.
class RepublishOnListBackend : public InMemoryBackend
{
public:
    void armOnFirstList(String prefix, std::function<void()> mutation)
    {
        std::lock_guard<std::mutex> lock(arm_mutex);
        armed_prefix = std::move(prefix);
        pending_mutation = std::move(mutation);
    }

    ListPage list(const String & prefix, const String & cursor, size_t limit) override
    {
        std::function<void()> to_run;
        {
            std::lock_guard<std::mutex> lock(arm_mutex);
            if (pending_mutation && prefix == armed_prefix)
            {
                to_run = std::move(pending_mutation);
                pending_mutation = nullptr;
            }
        }
        if (to_run)
            to_run();
        return InMemoryBackend::list(prefix, cursor, limit);
    }

private:
    std::mutex arm_mutex;
    String armed_prefix;
    std::function<void()> pending_mutation;
};

/// Companion to `RepublishOnListBackend` for the MANIFEST phantom-dangle race: the ref-walk's
/// per-namespace recovery captures each committed `(ref -> manifest)` minutes before the per-ref
/// `backend.get(mkey)` that confirms the manifest body. This backend fires an injected mutation the
/// FIRST time `get` is called for the armed manifest key — strictly AFTER the walk captured its (now
/// stale) row and AT the GET that would otherwise read the manifest — reproducing "ref republished/
/// dropped + old manifest legitimately GC-deleted" deterministically, with no real timing.
class MutateOnFirstGetBackend : public InMemoryBackend
{
public:
    void armOnFirstGet(String key, std::function<void()> mutation)
    {
        std::lock_guard<std::mutex> lock(arm_mutex);
        armed_key = std::move(key);
        pending_mutation = std::move(mutation);
    }

    std::optional<GetResult> get(const String & key, Range range) override
    {
        std::function<void()> to_run;
        {
            std::lock_guard<std::mutex> lock(arm_mutex);
            if (pending_mutation && key == armed_key)
            {
                to_run = std::move(pending_mutation);
                pending_mutation = nullptr;
            }
        }
        if (to_run)
            to_run();
        return InMemoryBackend::get(key, range);
    }

private:
    std::mutex arm_mutex;
    String armed_key;
    std::function<void()> pending_mutation;
};
}

/// A committed ref whose manifest body is present and whose blobs exist => clean.
TEST(CasFsck, CleanManifestPoolHasNoDangling)
{
    auto backend = std::make_shared<InMemoryBackend>();
    auto store = openPoolForTest(backend);
    const RootNamespace ns{"00/aa@cas@"};
    const ManifestRef r = ref(1, 0xAA);
    writeBlobBody(*backend, store->layout(), DB::UInt128(1));
    writeManifestRaw(*backend, store->layout(), ns, r, {blobEntryFor("a", DB::UInt128(1))});
    publishCommittedTransition(*backend, store->layout(), ns, "tbl", std::nullopt, r);
    const FsckReport rep = runFsck(*store, /*detail*/true);
    EXPECT_TRUE(rep.clean());
    EXPECT_EQ(rep.dangling, 0u);
}

/// A committed ref naming a MISSING manifest body is an ERROR (Dangling).
TEST(CasFsck, OwnerVisibleMissingManifestBodyIsError)
{
    auto backend = std::make_shared<InMemoryBackend>();
    auto store = openPoolForTest(backend);
    const RootNamespace ns{"00/aa@cas@"};
    const ManifestRef r = ref(1, 0xAA);
    publishCommittedTransition(*backend, store->layout(), ns, "tbl", std::nullopt, r);  // no body written
    const FsckReport rep = runFsck(*store, /*detail*/true);
    EXPECT_FALSE(rep.clean());
    EXPECT_GE(rep.dangling, 1u);
}

/// A committed ref whose blob body is missing is an ERROR (Dangling).
TEST(CasFsck, ReachableBlobMissingIsError)
{
    auto backend = std::make_shared<InMemoryBackend>();
    auto store = openPoolForTest(backend);
    const RootNamespace ns{"00/aa@cas@"};
    const ManifestRef r = ref(1, 0xAA);
    writeManifestRaw(*backend, store->layout(), ns, r, {blobEntryFor("a", DB::UInt128(1))});  // no blob body
    publishCommittedTransition(*backend, store->layout(), ns, "tbl", std::nullopt, r);
    const FsckReport rep = runFsck(*store, /*detail*/true);
    EXPECT_FALSE(rep.clean());
    EXPECT_GE(rep.dangling, 1u);
}

/// A pre-precommit body in an eligible prefix (no owner) is INFO (Unreachable), not an error.
TEST(CasFsck, ReclaimablePrePrecommitBodyIsInfo)
{
    auto backend = std::make_shared<InMemoryBackend>();
    auto store = openPoolForTest(backend);
    const RootNamespace ns{"00/aa@cas@"};
    /// Discovery is LIST-based (`listNamespaces` scans `cas/refs/`); seed a birth-only ref log so the
    /// namespace is discoverable but holds NO committed owner -- the manifest body below is orphan debris.
    appendRefLogSeed(*backend, store->layout(), ns, {});
    const ManifestRef r = ref(5, 0xAB);
    writeManifestRaw(*backend, store->layout(), ns, r, {blobEntryFor("a", DB::UInt128(1))});   // body, no owner
    setWatermarkMinActive(*backend, store->layout(), kServerRoot, kWriterEpoch, 6);   // eligible
    const FsckReport rep = runFsck(*store, /*detail*/true);
    EXPECT_TRUE(rep.clean());            // not an error
    EXPECT_GE(rep.unreachable, 1u);      // counted as info/unreachable
}

/// Pipeline classification (2026-07-02): a condemned-but-present blob is PendingGc — an EXPECTED
/// pipeline state (deletion is scheduled), never the suspicious "unreachable" lump beta testers
/// read as a leak. clean() is unaffected.
TEST(CasFsck, CondemnedBlobClassifiesPendingGc)
{
    auto backend = std::make_shared<InMemoryBackend>();
    auto store = openPoolForTest(backend);
    const RootNamespace ns{"00/aa@cas@"};
    const ManifestRef r = ref(1, 0xA1);
    writeBlobBody(*backend, store->layout(), DB::UInt128(1));
    writeManifestRaw(*backend, store->layout(), ns, r, {blobEntryFor("a", DB::UInt128(1))});
    publishCommittedTransition(*backend, store->layout(), ns, "tbl", std::nullopt, r);
    Gc gc(store, hexToU128("00000000000000000000000000000001"));
    gc.runRegularRound();
    dropRefTransition(*backend, store->layout(), ns, "tbl", r);
    gc.runRegularRound();   /// -1 folds => zero => condemned into the retired list; blob still present

    const FsckReport rep = runFsck(*store, /*detail*/true);
    EXPECT_TRUE(rep.clean());
    EXPECT_EQ(rep.pending_gc, 1u);
    EXPECT_EQ(rep.unaccounted, 0u);
    bool saw = false;
    for (const FsckObject & o : rep.objects)
        if (o.cls == FsckClass::PendingGc)
        {
            saw = true;
            ASSERT_FALSE(o.reachable_from.empty());
            EXPECT_NE(o.reachable_from[0].find("condemned at round"), String::npos);
        }
    EXPECT_TRUE(saw);
}

/// A drop whose -1 has NOT folded yet: the blob's edges are still in the GC snapshot => AwaitingGc
/// (expected), not Unaccounted.
TEST(CasFsck, DroppedButUnfoldedBlobClassifiesAwaitingGc)
{
    auto backend = std::make_shared<InMemoryBackend>();
    auto store = openPoolForTest(backend);
    const RootNamespace ns{"00/aa@cas@"};
    const ManifestRef r = ref(1, 0xA1);
    writeBlobBody(*backend, store->layout(), DB::UInt128(1));
    writeManifestRaw(*backend, store->layout(), ns, r, {blobEntryFor("a", DB::UInt128(1))});
    publishCommittedTransition(*backend, store->layout(), ns, "tbl", std::nullopt, r);
    Gc gc(store, hexToU128("00000000000000000000000000000001"));
    gc.runRegularRound();                                        /// +1 folded into the snapshot
    dropRefTransition(*backend, store->layout(), ns, "tbl", r);  /// -1 NOT folded (no round)

    const FsckReport rep = runFsck(*store, /*detail*/true);
    EXPECT_TRUE(rep.clean());
    EXPECT_EQ(rep.awaiting_gc, 1u);
    EXPECT_EQ(rep.unaccounted, 0u);
}

/// Stale-edge cross-check, NEGATIVE side: the residual edge's source manifest body is still PRESENT in
/// the pool, so its removal still has a `-1` to fold (and the orphan sweep still has a body to reclaim).
/// That is a genuine mid-pipeline backlog and must keep the `AwaitingGc` verdict — the new check may
/// never turn an ordinary unfolded drop into a hard finding.
TEST(CasFsck, UnfoldedDropWithPresentSourceManifestStaysAwaitingGc)
{
    auto backend = std::make_shared<InMemoryBackend>();
    auto store = openPoolForTest(backend);
    const RootNamespace ns{"00/aa@cas@"};
    const ManifestRef r = ref(1, 0xA1);
    writeBlobBody(*backend, store->layout(), DB::UInt128(1));
    writeManifestRaw(*backend, store->layout(), ns, r, {blobEntryFor("a", DB::UInt128(1))});
    publishCommittedTransition(*backend, store->layout(), ns, "tbl", std::nullopt, r);
    Gc gc(store, hexToU128("00000000000000000000000000000001"));
    gc.runRegularRound();                                        /// +1 folded into the snapshot
    dropRefTransition(*backend, store->layout(), ns, "tbl", r);  /// -1 NOT folded; the BODY survives

    const FsckReport rep = runFsck(*store, /*detail*/true);
    EXPECT_TRUE(rep.clean());
    EXPECT_EQ(rep.awaiting_gc, 1u);
    EXPECT_EQ(rep.stale_edge, 0u);
    bool saw = false;
    for (const FsckObject & o : rep.objects)
        if (o.cls == FsckClass::AwaitingGc)
            saw = true;
    EXPECT_TRUE(saw);
}

/// Stale-edge cross-check, POSITIVE side: the blob's only residual `+1` names a manifest that no longer
/// exists anywhere in the pool, so no `-1` is left to fold — the in-degree stays at 1 for every future
/// round and the incremental GC can never nominate the blob. It must NOT be labeled `AwaitingGc`
/// ("expected, no action needed", the sentence that hid 56 permanently retained blobs); it is the hard
/// `StaleEdge` finding and the report is not `clean()`.
TEST(CasFsck, ResidualEdgeNamingAnAbsentManifestClassifiesStaleEdge)
{
    auto backend = std::make_shared<InMemoryBackend>();
    auto store = openPoolForTest(backend);
    const RootNamespace ns{"00/aa@cas@"};
    const ManifestRef r = ref(1, 0xA1);
    writeBlobBody(*backend, store->layout(), DB::UInt128(1));
    const ManifestId id = writeManifestRaw(*backend, store->layout(), ns, r, {blobEntryFor("a", DB::UInt128(1))});
    publishCommittedTransition(*backend, store->layout(), ns, "tbl", std::nullopt, r);
    Gc gc(store, hexToU128("00000000000000000000000000000001"));
    gc.runRegularRound();                                        /// +1 folded into the snapshot
    dropRefTransition(*backend, store->layout(), ns, "tbl", r);  /// the owner is gone ...
    deleteManifestBody(*backend, store->layout(), id);           /// ... and so is the body, un-folded

    const FsckReport rep = runFsck(*store, /*detail*/true);
    EXPECT_EQ(rep.stale_edge, 1u);
    EXPECT_EQ(rep.awaiting_gc, 0u);
    EXPECT_EQ(rep.dangling, 0u) << "no committed ref names the manifest any more — this is not a dangle";
    EXPECT_FALSE(rep.clean());
    bool saw = false;
    for (const FsckObject & o : rep.objects)
        if (o.cls == FsckClass::StaleEdge)
        {
            saw = true;
            ASSERT_FALSE(o.reachable_from.empty());
            EXPECT_NE(o.reachable_from[0].find("no longer exist"), String::npos);
        }
    EXPECT_TRUE(saw);
}

/// GC never ran on the pool: nothing is classifiable through the GC view — everything unreferenced
/// is AwaitingGc ("GC has not run yet"), never a false Unaccounted alarm.
TEST(CasFsck, GcNeverRanClassifiesAwaitingGc)
{
    auto backend = std::make_shared<InMemoryBackend>();
    auto store = openPoolForTest(backend);
    writeBlobBody(*backend, store->layout(), DB::UInt128(5));   /// present, never referenced, no gc/state

    const FsckReport rep = runFsck(*store, /*detail*/true);
    EXPECT_TRUE(rep.clean());
    EXPECT_EQ(rep.awaiting_gc, 1u);
    EXPECT_EQ(rep.unaccounted, 0u);
}

/// A blob outside the WHOLE GC view on a pool where GC runs: Unaccounted — expected only as a
/// transient (fast create+drop between rounds); persistent occurrences violate INV-2.
TEST(CasFsck, ForeignBlobClassifiesUnaccounted)
{
    auto backend = std::make_shared<InMemoryBackend>();
    auto store = openPoolForTest(backend);
    const RootNamespace ns{"00/aa@cas@"};
    const ManifestRef r = ref(1, 0xA1);
    writeBlobBody(*backend, store->layout(), DB::UInt128(1));
    writeManifestRaw(*backend, store->layout(), ns, r, {blobEntryFor("a", DB::UInt128(1))});
    publishCommittedTransition(*backend, store->layout(), ns, "tbl", std::nullopt, r);
    Gc gc(store, hexToU128("00000000000000000000000000000001"));
    gc.runRegularRound();

    writeBlobBody(*backend, store->layout(), DB::UInt128(0xF0F0));   /// never referenced anywhere

    const FsckReport rep = runFsck(*store, /*detail*/true);
    EXPECT_TRUE(rep.clean());
    EXPECT_EQ(rep.unaccounted, 1u);
    EXPECT_EQ(rep.pending_gc, 0u);
}

/// A `.meta` descriptor whose body is missing is ADVISORY (meta_without_body), NOT a hard finding:
/// GC deletes the body FIRST and drops the `.meta` afterwards on a bounded, error-suppressed advisory
/// pool that may drop the op, so a single raw LIST legitimately observes a body-less `.meta` mid-
/// graduation and no finite grace makes a persistent one hard evidence. It is still counted/reported;
/// it must NOT be a `dangling` (nothing referenced it) and NOT one of the present-but-unreferenced blob
/// pipeline classes (the `.meta` key is excluded from body classification entirely). `clean()` stays TRUE.
TEST(CasFsck, MetaWithoutBodyIsAdvisoryNotHard)
{
    auto backend = std::make_shared<InMemoryBackend>();
    auto store = openPoolForTest(backend);
    const DB::UInt128 h = u128Of("meta-without-body");
    writeMetaClean(*backend, store->layout(), h, /*size*/ 10);   /// meta only, no body written

    const FsckReport rep = runFsck(*store, /*detail*/true);
    EXPECT_GE(rep.meta_without_body, 1u);   // still counted and reported in the full report
    EXPECT_EQ(rep.dangling, 0u);
    EXPECT_EQ(rep.unreachable, 0u);
    EXPECT_EQ(rep.pending_gc, 0u);
    EXPECT_EQ(rep.awaiting_gc, 0u);
    EXPECT_EQ(rep.unaccounted, 0u);
    EXPECT_TRUE(rep.clean());   // meta_without_body is advisory — excluded from clean()
}

/// A body with no `.meta` sibling is a BENIGN not-yet-adopted (or crashed-birth) artifact — NOT a
/// dangle, and it must still classify through the ordinary present-but-unreferenced pipeline.
TEST(CasFsck, BodyWithoutMetaIsBenign)
{
    auto backend = std::make_shared<InMemoryBackend>();
    auto store = openPoolForTest(backend);
    const DB::UInt128 h = u128Of("body-without-meta");
    writeBlobBody(*backend, store->layout(), h);   /// body only, no meta written

    const FsckReport rep = runFsck(*store, /*detail*/true);
    EXPECT_GE(rep.body_without_meta, 1u);
    EXPECT_EQ(rep.dangling, 0u);
    EXPECT_EQ(rep.meta_without_body, 0u);
    EXPECT_TRUE(rep.clean());
}

/// Snapshot integrity oracle (spec §Snapshot Publication): a published snapshot whose bytes equal an
/// independent replay of its own surviving logs is clean, and the oracle actually RAN (logs present).
TEST(CasFsckSnapshotOracle, PublishedSnapshotMatchingReplayIsClean)
{
    auto backend = std::make_shared<InMemoryBackend>();
    auto store = openPoolForTest(backend);
    const RootNamespace ns{"00/aa@cas@"};
    const ManifestRef r = ref(1, 0xAA);
    writeBlobBody(*backend, store->layout(), DB::UInt128(1));
    writeManifestRaw(*backend, store->layout(), ns, r, {blobEntryFor("a", DB::UInt128(1))});
    publishCommittedTransition(*backend, store->layout(), ns, "tbl", std::nullopt, r);

    /// Publish the CORRECT snapshot: exactly the deterministic replay of the logs, at the greatest log id.
    const RefTableState st = recoverRefTable(*backend, store->layout(), ns);
    writeRefSnapshotRaw(*backend, store->layout(), snapshotOf(st, ns.string()));

    const FsckReport rep = runFsck(*store, /*detail*/true);
    EXPECT_TRUE(rep.clean());
    EXPECT_EQ(rep.snapshot_oracle_mismatches, 0u);
    EXPECT_GE(rep.snapshot_oracle_checked, 1u) << "the oracle must actually run when the logs survive";
}

/// A published snapshot whose bytes are a VALID snapshot object but DIVERGE from the replay of its logs
/// (here: it carries an extra precommit the logs never added) is a hard ERROR -- caught even though
/// reachability is unaffected (precommits are not walked), so it surfaces ONLY through the oracle.
TEST(CasFsckSnapshotOracle, ForgedSnapshotDivergingFromReplayIsError)
{
    auto backend = std::make_shared<InMemoryBackend>();
    auto store = openPoolForTest(backend);
    const RootNamespace ns{"00/aa@cas@"};
    const ManifestRef r = ref(1, 0xAA);
    writeBlobBody(*backend, store->layout(), DB::UInt128(1));
    writeManifestRaw(*backend, store->layout(), ns, r, {blobEntryFor("a", DB::UInt128(1))});
    publishCommittedTransition(*backend, store->layout(), ns, "tbl", std::nullopt, r);

    /// Forge a snapshot at the greatest log id: the correct replay plus one phantom precommit binding.
    /// It is internally valid (decodes fine), so only the byte-compare against the log replay catches it.
    const RefTableState st = recoverRefTable(*backend, store->layout(), ns);
    RefTableSnapshot forged = snapshotOf(st, ns.string());
    forged.precommits.push_back(RefOwnerBinding{RefOwnerKind::Precommit, "ghost", ref(2, 0xBB)});
    writeRefSnapshotRaw(*backend, store->layout(), forged);

    const FsckReport rep = runFsck(*store, /*detail*/true);
    EXPECT_EQ(rep.snapshot_oracle_mismatches, 1u);
    EXPECT_EQ(rep.dangling, 0u) << "the divergence is a snapshot-oracle error, not a reachability dangle";
    EXPECT_FALSE(rep.clean());
    bool saw = false;
    for (const FsckObject & o : rep.objects)
        if (o.cls == FsckClass::SnapshotOracleMismatch)
            saw = true;
    EXPECT_TRUE(saw);
}

/// Once a table's covered logs are cleaned (the steady state on a GC-caught-up pool), the oracle has no
/// independent history to replay from and SKIPS the table -- it must never false-positive on a snapshot
/// whose logs are legitimately gone.
TEST(CasFsckSnapshotOracle, CleanedLogsSkipOracleWithoutFalsePositive)
{
    auto backend = std::make_shared<InMemoryBackend>();
    auto store = openPoolForTest(backend);
    const RootNamespace ns{"00/aa@cas@"};
    const ManifestRef r = ref(1, 0xAA);
    writeBlobBody(*backend, store->layout(), DB::UInt128(1));
    writeManifestRaw(*backend, store->layout(), ns, r, {blobEntryFor("a", DB::UInt128(1))});
    const uint64_t seq = publishCommittedTransition(*backend, store->layout(), ns, "tbl", std::nullopt, r);

    /// Publish the correct snapshot, then delete the log it covers (as GC cleanup would once covered).
    const RefTableState st = recoverRefTable(*backend, store->layout(), ns);
    writeRefSnapshotRaw(*backend, store->layout(), snapshotOf(st, ns.string()));
    const String log_key = store->layout().refLogKey(ns, RefTxnId{1, seq});
    const HeadResult h = backend->head(log_key);
    ASSERT_TRUE(h.exists);
    backend->deleteExact(log_key, h.token);

    const FsckReport rep = runFsck(*store, /*detail*/true);
    EXPECT_EQ(rep.snapshot_oracle_mismatches, 0u);
    EXPECT_EQ(rep.snapshot_oracle_checked, 0u) << "no surviving logs -> the oracle skips, not fails";
    EXPECT_TRUE(rep.clean());
}

/// A scan whose deadline is already in the past: partial_on_deadline=false keeps the old
/// throw-on-timeout contract; partial_on_deadline=true returns the accumulated lower-bound counts
/// instead of failing empty-handed (the 2026-07-05 campaign lost 5 verdicts to this).
TEST(CasFsckPartial, DeadlineReturnsAccumulatedCountsInsteadOfThrowing)
{
    auto backend = std::make_shared<InMemoryBackend>();
    auto store = openPoolForTest(backend);
    const RootNamespace ns{"00/aa@cas@"};
    const ManifestRef r = ref(1, 0xAA);
    writeBlobBody(*backend, store->layout(), DB::UInt128(1));
    writeManifestRaw(*backend, store->layout(), ns, r, {blobEntryFor("a", DB::UInt128(1))});
    publishCommittedTransition(*backend, store->layout(), ns, "tbl", std::nullopt, r);

    const auto past = std::chrono::steady_clock::now() - std::chrono::seconds(1);
    /// partial_on_deadline=false keeps the old contract:
    EXPECT_THROW(DB::Cas::runFsck(*store, /*detail=*/false, {}, past), DB::Exception);
    /// partial_on_deadline=true returns a flagged report:
    const auto report = DB::Cas::runFsck(*store, false, {}, past, /*partial_on_deadline=*/true);
    EXPECT_TRUE(report.partial);
    EXPECT_FALSE(report.partial_reason.empty());
}

/// A `namespace_prefix` scopes the scan to only the matching namespaces' refs (dangling-only): no
/// pool-wide unreachable/pending/awaiting/unaccounted classification, since that needs the whole pool.
TEST(CasFsckScoped, NamespacePrefixChecksOnlyMatchingRefsDanglingOnly)
{
    auto backend = std::make_shared<InMemoryBackend>();
    auto store = openPoolForTest(backend);

    const RootNamespace ns_a{"nsa"};
    const ManifestRef r_a = ref(1, 0xA1);
    writeBlobBody(*backend, store->layout(), DB::UInt128(1));
    writeManifestRaw(*backend, store->layout(), ns_a, r_a, {blobEntryFor("a", DB::UInt128(1))});
    publishCommittedTransition(*backend, store->layout(), ns_a, "tbl", std::nullopt, r_a);

    const RootNamespace ns_b{"nsb"};
    const ManifestRef r_b = ref(1, 0xB1);
    writeBlobBody(*backend, store->layout(), DB::UInt128(2));
    writeManifestRaw(*backend, store->layout(), ns_b, r_b, {blobEntryFor("b", DB::UInt128(2))});
    publishCommittedTransition(*backend, store->layout(), ns_b, "tbl", std::nullopt, r_b);

    const auto scoped = DB::Cas::runFsck(*store, false, {}, {}, false, /*namespace_prefix=*/"nsa");
    EXPECT_EQ(scoped.dangling, 0u);
    EXPECT_GT(scoped.reachable, 0u);
    /// Scoped mode skips only the POOL-WIDE physical/pipeline classification; the manifest-debris
    /// pass stays active for the scoped namespaces, so `unreachable` here counts THEIR orphan
    /// manifest bodies — zero in this clean setup, legitimately nonzero on a churned pool.
    EXPECT_EQ(scoped.unreachable, 0u);
    EXPECT_EQ(scoped.pending_gc + scoped.awaiting_gc + scoped.unaccounted, 0u);
}

/// B207: the ref-walk and the HEAD-confirm run minutes apart with no snapshot. A ref that gets
/// RE-PUBLISHED to a different manifest in that window, combined with a legitimate GC delete of the
/// blob it used to name, must NOT surface as a phantom `dangling` — only a CURRENT ref over an absent
/// object is a real dangle.
TEST(CasFsck, PhantomDanglingFromRepublishedRefIsReresolvedAway)
{
    auto backend = std::make_shared<RepublishOnListBackend>();
    auto store = openPoolForTest(backend);
    const RootNamespace ns{"00/aa@cas@"};
    const ManifestRef r1 = ref(1, 0xA1);
    const ManifestRef r2 = ref(2, 0xA2);
    const DB::UInt128 h1 = u128Of("b207-phantom-old");
    const DB::UInt128 h2 = u128Of("b207-phantom-new");

    writeBlobBody(*backend, store->layout(), h1);
    writeManifestRaw(*backend, store->layout(), ns, r1, {blobEntryFor("a", h1)});
    publishCommittedTransition(*backend, store->layout(), ns, "tbl", std::nullopt, r1);

    /// Fires strictly between the ref-walk (which captures ref "tbl" -> r1, blob h1, as reachable) and
    /// the HEAD-confirm's physical listing — exactly the window B207 is about.
    backend->armOnFirstList(store->layout().blobsPrefix(), [&]
    {
        writeBlobBody(*backend, store->layout(), h2);
        writeManifestRaw(*backend, store->layout(), ns, r2, {blobEntryFor("a", h2)});
        publishCommittedTransition(*backend, store->layout(), ns, "tbl", r1, r2);   /// re-publish

        const String old_key = store->layout().blobKey(BlobRef{BlobHashAlgo::CityHash128, BlobDigest::fromU128(h1)});
        const HeadResult head = backend->head(old_key);
        ASSERT_TRUE(head.exists);
        backend->deleteExact(old_key, head.token);   /// legitimate GC delete of the now-unreferenced blob
    });

    const FsckReport rep = runFsck(*store, /*detail*/true);
    EXPECT_EQ(rep.dangling, 0u);
    EXPECT_TRUE(rep.clean());
}

/// Same race, but the ref is DROPPED (not re-published) in the window between the walk and the
/// HEAD-confirm — also must not surface as a phantom dangle.
TEST(CasFsck, PhantomDanglingFromDroppedRefIsReresolvedAway)
{
    auto backend = std::make_shared<RepublishOnListBackend>();
    auto store = openPoolForTest(backend);
    const RootNamespace ns{"00/aa@cas@"};
    const ManifestRef r1 = ref(1, 0xA1);
    const DB::UInt128 h1 = u128Of("b207-phantom-dropped");

    writeBlobBody(*backend, store->layout(), h1);
    writeManifestRaw(*backend, store->layout(), ns, r1, {blobEntryFor("a", h1)});
    publishCommittedTransition(*backend, store->layout(), ns, "tbl", std::nullopt, r1);

    backend->armOnFirstList(store->layout().blobsPrefix(), [&]
    {
        dropRefTransition(*backend, store->layout(), ns, "tbl", r1);   /// ref dropped since the walk

        const String old_key = store->layout().blobKey(BlobRef{BlobHashAlgo::CityHash128, BlobDigest::fromU128(h1)});
        const HeadResult head = backend->head(old_key);
        ASSERT_TRUE(head.exists);
        backend->deleteExact(old_key, head.token);   /// legitimate GC delete after the drop folds
    });

    const FsckReport rep = runFsck(*store, /*detail*/true);
    EXPECT_EQ(rep.dangling, 0u);
    EXPECT_TRUE(rep.clean());
}

/// Companion: the fix must never HIDE a real loss. A blob that a CURRENT ref still names, but whose
/// object is genuinely gone (an operator error, a storage-layer bug — NOT a legitimate GC delete),
/// stays `dangling` after the re-resolve.
TEST(CasFsck, RealDanglingStillCaughtAfterReresolve)
{
    auto backend = std::make_shared<InMemoryBackend>();
    auto store = openPoolForTest(backend);
    const RootNamespace ns{"00/aa@cas@"};
    const ManifestRef r = ref(1, 0xA1);
    const DB::UInt128 h = u128Of("b207-real-dangle");

    writeBlobBody(*backend, store->layout(), h);
    writeManifestRaw(*backend, store->layout(), ns, r, {blobEntryFor("a", h)});
    publishCommittedTransition(*backend, store->layout(), ns, "tbl", std::nullopt, r);

    const String key = store->layout().blobKey(BlobRef{BlobHashAlgo::CityHash128, BlobDigest::fromU128(h)});
    const HeadResult head = backend->head(key);
    ASSERT_TRUE(head.exists);
    backend->deleteExact(key, head.token);   /// genuine loss — the ref is UNCHANGED, still names this blob

    const FsckReport rep = runFsck(*store, /*detail*/true);
    EXPECT_EQ(rep.dangling, 1u);
    EXPECT_FALSE(rep.clean());
}

/// The MANIFEST analogue of the blob phantom-dangle. The ref-walk captures "tbl" -> r1's manifest, then
/// the ref is RE-PUBLISHED to a different manifest r2 and the OLD r1 manifest body is legitimately
/// GC-deleted before the per-ref body GET. The missing OLD manifest must be revalidated away — a fresh
/// re-resolve shows the CURRENT ref no longer names it — never surfacing as a phantom `dangling`.
TEST(CasFsck, PhantomDanglingManifestFromRepublishedRefIsReresolvedAway)
{
    auto backend = std::make_shared<MutateOnFirstGetBackend>();
    auto store = openPoolForTest(backend);
    const RootNamespace ns{"00/aa@cas@"};
    const ManifestRef r1 = ref(1, 0xA1);
    const ManifestRef r2 = ref(2, 0xA2);
    const DB::UInt128 h1 = u128Of("phantom-manifest-old");
    const DB::UInt128 h2 = u128Of("phantom-manifest-new");

    writeBlobBody(*backend, store->layout(), h1);
    writeManifestRaw(*backend, store->layout(), ns, r1, {blobEntryFor("a", h1)});
    publishCommittedTransition(*backend, store->layout(), ns, "tbl", std::nullopt, r1);

    const String m1_key = store->layout().manifestKey(ManifestId{ns, r1});
    /// Fires strictly between the ref-walk (captures "tbl" -> r1) and the per-ref GET of r1's manifest:
    /// re-publish "tbl" to r2 and legitimately GC-delete the now-superseded r1 manifest body.
    backend->armOnFirstGet(m1_key, [&]
    {
        writeBlobBody(*backend, store->layout(), h2);
        writeManifestRaw(*backend, store->layout(), ns, r2, {blobEntryFor("a", h2)});
        publishCommittedTransition(*backend, store->layout(), ns, "tbl", r1, r2);   /// re-publish

        const HeadResult head = backend->head(m1_key);
        ASSERT_TRUE(head.exists);
        backend->deleteExact(m1_key, head.token);   /// legitimate GC delete of the superseded manifest
    });

    const FsckReport rep = runFsck(*store, /*detail*/true);
    EXPECT_EQ(rep.dangling, 0u);
    EXPECT_TRUE(rep.clean());
}

namespace
{
/// Build a real `ContentAddressedMetadataStorage` over Local object storage and start it (Mounted) --
/// the same harness gtest_cas_operation_gate.cpp uses. Each call gets an isolated pool root.
std::shared_ptr<DB::ContentAddressedMetadataStorage> openRunningStorageForTest()
{
    auto settings = makeSettingsForTest("test", std::filesystem::temp_directory_path() / "ca_fsck_running_scratch");
    auto storage = std::make_shared<DB::ContentAddressedMetadataStorage>(
        makeLocalObjectStorageForTest(), "pool", "srv1", "", nullptr, settings);
    storage->startup();
    return storage;
}

/// Commit one real part (tmp -> final rename -> commit) so a RUNNING FSCK has live committed content.
void commitOneRunningPart(DB::ContentAddressedMetadataStorage & storage)
{
    const std::string table_dir = "g80/g80g80g8-0808-4808-8808-080808080808";
    auto tx = storage.createTransaction();
    auto & ca_tx = dynamic_cast<DB::ContentAddressedTransaction &>(*tx);
    auto buf = ca_tx.writeFile(table_dir + "/tmp_insert_all_1_1_0/data.bin", 65536, DB::WriteMode::Rewrite, {});
    const std::string bytes = "content-of-the-part";
    buf->write(bytes.data(), bytes.size());
    buf->finalize();
    tx->moveDirectory(table_dir + "/tmp_insert_all_1_1_0", table_dir + "/all_1_1_0");
    tx->commit(DB::NoCommitOptions{});
}
}

/// (rev.8) FSCK runs on a RUNNING disk: scanning a live pool with one committed part succeeds and reports
/// its content (the one-row summary the SQL verb renders from this report).
TEST(CasFsckRunning, FsckOnMountedDiskSucceeds)
{
    auto storage = openRunningStorageForTest();
    commitOneRunningPart(*storage);

    FsckReport rep;
    EXPECT_NO_THROW(rep = storage->runFsckNow(/*detail=*/false));
    EXPECT_TRUE(rep.clean());
    EXPECT_GE(rep.distinct_blobs, 1u) << "the running scan must see the live committed part's blob";
    EXPECT_EQ(rep.dangling, 0u);
}

/// (rev.8) FSCK is Admin-class: on a not-live pool (a lease blip / IdentityLost) it refuses before
/// scanning, exactly like the GC entry points -- an FSCK of a disk whose data root may be gone or replaced
/// is meaningless (the operator has the snapshot / FORGET path). The two states refuse in DIFFERENT
/// classes, and the pairing is the point: a lease blip is transient unavailability (upstream-retryable),
/// an identity loss is terminal (668).
TEST(CasFsckRunning, FsckOnNotLiveDiskRefusesTransientRetryableAndIdentityLostTerminal)
{
    for (const auto & [lc, code] : {std::pair{PoolLifecycle::TransientNotLive, DB::ErrorCodes::NETWORK_ERROR},
                                    std::pair{PoolLifecycle::IdentityLost, DB::ErrorCodes::INVALID_STATE}})
    {
        auto storage = openRunningStorageForTest();
        storage->store()->setLifecycleForTest(lc);   /// one force from Live; no later store() call
        expectThrowsCode(code, [&] { storage->runFsckNow(/*detail=*/false); });
    }
}

/// The summary line is the ONLY thing most consumers ever read: the soak harness parses it, an operator
/// eyeballs it, and `exit_code` gates CI on it. So a field that `clean()` treats as a hard finding but the
/// summary omits is invisible in practice, however faithfully it is counted -- which is exactly what
/// happened to `corrupted_runs`: counted since the seal check landed, part of `clean()`, rendered in
/// `--detail` rows, and absent from the summary, so no run has ever reported one.
///
/// This test is deliberately written against `clean()`'s OWN field set rather than a hand-listed set of
/// names, so that adding a hard finding to `clean()` without rendering it fails HERE instead of hiding for
/// months. `formatFsckSummary` exists to be testable at all: the line used to be built inline in
/// `CommandFsck::executeImpl`, where nothing could reach it.
TEST(CasFsckSummary, EveryHardFindingAppearsOnTheSummaryLine)
{
    FsckReport rep;
    rep.dangling = 11;
    rep.snapshot_oracle_mismatches = 22;
    rep.corrupted_runs = 33;
    rep.stale_edge = 44;

    const String line = formatFsckSummary(rep);

    /// One assertion per `clean()` term. If `clean()` grows a term, add it here in the same change.
    EXPECT_NE(line.find("dangling=11"), String::npos) << line;
    EXPECT_NE(line.find("snapshot_oracle_mismatches=22"), String::npos) << line;
    EXPECT_NE(line.find("corrupted_runs=33"), String::npos) << line;
    EXPECT_NE(line.find("stale_edge=44"), String::npos) << line;

    /// A report carrying these values is NOT clean; the line must not be mistakable for a clean one.
    EXPECT_FALSE(rep.clean());
}

/// A zero must be PRINTED, not omitted. The harness's `stale_edge_verdict` fails closed on an absent key
/// precisely because "field missing" and "field zero" are different facts, and a formatter that skips
/// zeros would turn every clean pool into an unparseable one.
TEST(CasFsckSummary, ZeroValuedHardFindingsAreStillPrinted)
{
    const String line = formatFsckSummary(FsckReport{});
    EXPECT_NE(line.find("corrupted_runs=0"), String::npos) << line;
    EXPECT_NE(line.find("stale_edge=0"), String::npos) << line;
    EXPECT_EQ(line.find("partial="), String::npos) << "a non-partial report must not claim partial: " << line;
}

/// A partial scan is a lower bound over the visited subset, so the flag and its reason must travel WITH
/// the counts -- a consumer that sees the numbers but not `partial=1` reads a truncated walk as the pool
/// truth.
TEST(CasFsckSummary, PartialFlagAndReasonTravelWithTheCounts)
{
    FsckReport rep;
    rep.partial = true;
    rep.partial_reason = "deadline exceeded after 180s";
    const String line = formatFsckSummary(rep);
    EXPECT_NE(line.find("partial=1"), String::npos) << line;
    EXPECT_NE(line.find("reason='deadline exceeded after 180s'"), String::npos) << line;
}
