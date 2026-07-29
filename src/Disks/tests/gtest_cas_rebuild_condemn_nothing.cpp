#include <gtest/gtest.h>

#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Formats/CasFoldSealFormat.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Formats/CasGcStateFormat.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Formats/CasRefCkptFormat.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Formats/CasRefLogFormat.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Formats/CasRefSnapshotFormat.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Gc/CasBlobInDegree.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Gc/CasGc.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasPool.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasRefProtocol.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Tools/CasFsck.h>
#include "cas_test_helpers.h"

#include <set>
#include <stdexcept>
#include <string>
#include <vector>

/// REBUILD CONDEMNS NOTHING, AND fsck WALKS STREAMS BY ARITHMETIC (spec 2026-07-27 "ref chain complete
/// cut" §7).
///
/// REBUILD used to end with a LIST of `blobs/` and condemn every listed body its traversal had not
/// reached. That is the r5-finding-4 data-loss vector: the traversal itself is listing-driven, so a
/// store that omits a durable ref-log or manifest key from a LIST hides a LIVE owner, and the very same
/// pass then condemns the blob that owner pins. One lying enumeration, and acked data is scheduled for
/// deletion. The condemnation is GONE — REBUILD rebuilds cursors and edges and reclaims nothing.
///
/// The NAMED residual that removal creates (Stage-A staging contract, register R4): a blob whose
/// manifest no longer exists anywhere is unreclaimable until the build/upload registry can enumerate
/// in-flight uploads. No substitute reclamation is added in its place — a quiet one would be the same
/// vector wearing a different hat (Constraint 3: no fallback).
///
/// fsck's half is the other side of the same rule: it may not rest a verdict on a listing either. It
/// walks each namespace's ref stream by ARITHMETIC from `_ckpt.checkpoint` upward, reading every id by
/// exact key, and reports one verdict per namespace — `chain-broken` (a 404 below a CONFIRMED durable
/// same-epoch id: a hole, fatal in the summary AND in the exit code), `unchecked` (could not prove it
/// either way), or nothing at all. A finding is RECORDED, never thrown: an fsck that dies on the first
/// bad namespace says nothing about the ones it never reached.

using namespace DB::Cas;
using namespace DB::Cas::tests;

namespace
{

const UInt128 kGc = hexToU128("00000000000000000000000000000001");

const RootNamespace kNsA{"00/aa@cas@"};
const RootNamespace kNsB{"00/zz@cas@"};

BlobRef blobRefOf(const DB::UInt128 & hash)
{
    return BlobRef{BlobHashAlgo::CityHash128, BlobDigest::fromU128(hash)};
}

bool blobPresent(Backend & backend, const Layout & layout, const DB::UInt128 & hash)
{
    return backend.head(layout.blobKey(blobRefOf(hash))).exists;
}

/// Whether ANY run the newest fold seal references carries a `kCondemned` row for `hash`. This is where
/// a rebuild used to put its zero-edge condemnations, so "nothing was condemned" is checked HERE rather
/// than by watching for a deletion several rounds later.
bool condemnedInSealedRuns(Backend & backend, const Layout & layout, const DB::UInt128 & hash)
{
    const GcState st = decodeGcState(backend.get(layout.gcStateKey())->bytes);
    const auto sealed = backend.get(layout.foldSealKey(st.snap_generation, st.snap_attempt));
    if (!sealed)
        return false;
    const CasFoldSeal seal = decodeFoldSeal(sealed->bytes);
    for (const RunRef & r : seal.blob_target_runs)
    {
        auto reader = openSourceEdgeRun(backend, r.key);
        String k;
        String p;
        while (reader.next(k, p))
        {
            BlobRef ref;
            UInt128 sid;
            SourceEdgeKeyCodec::parse(k, ref, sid);
            if (p.empty() || p[0] != kCondemned)
                continue;
            if (ref.digest.toU128() == hash)
                return true;
        }
    }
    return false;
}

/// Publish `ref_name` -> a fresh manifest pinning `blob` at exactly `id`, and return the manifest's key
/// so a test can hide it from the listing.
String publishAtReturningManifestKey(
    Backend & backend, const Layout & layout, const RootNamespace & ns, const RefTxnId & id,
    const String & ref_name, uint64_t build_sequence, const DB::UInt128 & blob, bool birth = false,
    std::optional<RefTxnId> prev_epoch_seal = std::nullopt)
{
    publishAt(backend, layout, ns, id, ref_name, build_sequence, blob, birth, prev_epoch_seal);
    return layout.manifestKey(ManifestId{ns, ManifestRef{.writer_epoch = id.writer_epoch,
                                                         .build_sequence = build_sequence,
                                                         .manifest_ordinal = 1}});
}

/// Write `ns`'s `_ckpt` directly. The real writers merge by semantic maximum through `publishCkpt`;
/// a fixture that owns the whole namespace states the checkpoint outright.
void writeCkptRaw(Backend & backend, const Layout & layout, const RootNamespace & ns, const RefCkpt & ckpt)
{
    backend.putIfAbsent(layout.refCkptKey(RefNamespaceId::stageATransition(ns)), encodeRefCkpt(ckpt));
}

/// The table state after applying exactly `ids`, through the same builder the writer and the oracle
/// use — so a snapshot built from it is what the codec itself would have published.
RefTableState stateAfter(Backend & backend, const Layout & layout, const RootNamespace & ns,
                         const std::vector<RefTxnId> & ids)
{
    RefReplayBuilder builder(std::nullopt);
    for (const RefTxnId & id : ids)
    {
        const auto got = backend.get(layout.refLogKey(RefNamespaceId::stageATransition(ns), id));
        if (!got)
            throw std::runtime_error("stateAfter: fixture log " + std::to_string(id.writer_epoch) + "-"
                                     + std::to_string(id.ref_sequence) + " is missing");
        builder.applyOne(decodeRefLogTxn(openObject(FormatId::RefLog, got->bytes), ns.string(), id),
                         got->bytes.size());
    }
    return std::move(builder).finish().state;
}

/// Two writer epochs joined by a real seal: `{1,1} {1,2}` then the `{1,3}` seal, then `{2,1}` naming it
/// as its `prev_epoch_seal` and `{2,2}` after it. The shape both the walk's crossing and the oracle's
/// replay have to handle.
void seedSealedTwoEpochStream(Backend & backend, const Layout & layout, const RootNamespace & ns)
{
    publishAt(backend, layout, ns, RefTxnId{1, 1}, "ref_a", 1, DB::UInt128(1), /*birth=*/true);
    publishAt(backend, layout, ns, RefTxnId{1, 2}, "ref_b", 2, DB::UInt128(2));
    writeSealAt(backend, layout, ns, RefTxnId{1, 3});
    publishAt(backend, layout, ns, RefTxnId{2, 1}, "ref_c", 1, DB::UInt128(3), /*birth=*/false,
              /*prev_epoch_seal=*/RefTxnId{1, 3});
    publishAt(backend, layout, ns, RefTxnId{2, 2}, "ref_d", 2, DB::UInt128(4));
}

/// The number of `Unchecked` rows whose note mentions `needle`, over the whole report.
size_t uncheckedRowsMentioning(const FsckReport & rep, const String & needle)
{
    size_t n = 0;
    for (const FsckObject & o : rep.objects)
    {
        if (o.cls != FsckClass::Unchecked)
            continue;
        for (const String & note : o.reachable_from)
            if (note.find(needle) != String::npos)
                ++n;
    }
    return n;
}

}

/// ---- REBUILD condemns nothing ----

/// THE REGRESSION TEST FOR r5-finding-4. A blob pinned by a COMMITTED ref, whose ref-log record and
/// whose manifest body the store both omit from every LIST while serving them perfectly by exact key.
/// REBUILD's traversal is listing-driven on both legs — the owner replay reads the ref prefix, the
/// trimmed-but-live pass reads the manifest prefix — so it reaches neither, and the blob ends the
/// rebuild with zero edges. It used to be condemned right there, from the `blobs/` LIST: acked data,
/// scheduled for deletion, because one enumeration lied. Now nothing is condemned at all, so a hidden
/// owner costs retention and never data.
TEST(CasRebuildCondemnNothing, HiddenLiveManifestBlobIsNotCondemned)
{
    auto backend = std::make_shared<HintHoleBackend>();
    auto store = openPoolForTest(backend, /*gc_fold_max_defer_rounds=*/0);
    const Layout & layout = store->layout();

    /// Visible owner: ref_a pins blob 1.
    publishAt(*backend, layout, kNsA, RefTxnId{1, 1}, "ref_a", /*build_sequence=*/1, DB::UInt128(1), /*birth=*/true);
    /// HIDDEN owner: ref_b pins blob 2, and neither its record nor its manifest is ever listed.
    const String hidden_manifest =
        publishAtReturningManifestKey(*backend, layout, kNsA, RefTxnId{1, 2}, "ref_b", /*build_sequence=*/2, DB::UInt128(2));
    backend->hide(layout.refLogKey(RefNamespaceId::stageATransition(kNsA), RefTxnId{1, 2}));
    backend->hide(hidden_manifest);

    /// Precondition: both objects really are durable and really are hidden.
    ASSERT_TRUE(backend->get(layout.refLogKey(RefNamespaceId::stageATransition(kNsA), RefTxnId{1, 2})).has_value());
    ASSERT_TRUE(backend->get(hidden_manifest).has_value());

    Gc gc(store, kGc);
    const RebuildReport rep = gc.rebuildBaseline(/*force=*/true);
    ASSERT_TRUE(rep.performed) << rep.refusal;
    ASSERT_GT(backend->holesServed(), 0u) << "the hidden keys were never actually omitted from a LIST";
    EXPECT_EQ(rep.committed_refs, 1u) << "precondition: the rebuild must NOT have seen the hidden owner";

    EXPECT_FALSE(condemnedInSealedRuns(*backend, layout, DB::UInt128(2)))
        << "the hidden owner's blob was condemned — that is acked data scheduled for deletion";
    EXPECT_FALSE(loadMetaForTest(*backend, layout, DB::UInt128(2)).has_value())
        << "a rebuild condemns nothing, so it publishes no condemn marker";

    /// And it survives the pipeline: rounds run, nothing reclaims it.
    for (int i = 0; i < 4; ++i)
    {
        gc.runRegularRound();
        store->renewWatermarkOnce();
    }
    EXPECT_TRUE(blobPresent(*backend, layout, DB::UInt128(2))) << "acked data was deleted after a rebuild";
    EXPECT_TRUE(blobPresent(*backend, layout, DB::UInt128(1)));
}

/// An ORPHAN blob — one no manifest anywhere names — is likewise left alone. This is the NAMED residual
/// (register R4) stated as a test rather than as prose: until the build/upload registry can enumerate
/// in-flight uploads, a manifest-less blob is unreclaimable, and the rebuild does NOT get to guess. The
/// blob a live ref pins and the blob nothing pins are indistinguishable from a LIST, which is exactly
/// why the old pass could not tell them apart either.
TEST(CasRebuildCondemnNothing, OrphanBlobIsRetainedNotCondemned)
{
    auto backend = std::make_shared<InMemoryBackend>();
    auto store = openPoolForTest(backend, /*gc_fold_max_defer_rounds=*/0);
    const Layout & layout = store->layout();

    publishAt(*backend, layout, kNsA, RefTxnId{1, 1}, "ref_a", /*build_sequence=*/1, DB::UInt128(1), /*birth=*/true);
    writeBlobBody(*backend, layout, DB::UInt128(2));   /// orphan: present, named by nothing

    Gc gc(store, kGc);
    const RebuildReport rep = gc.rebuildBaseline(/*force=*/true);
    ASSERT_TRUE(rep.performed) << rep.refusal;

    EXPECT_FALSE(condemnedInSealedRuns(*backend, layout, DB::UInt128(2)));
    const GcState st = decodeGcState(backend->get(layout.gcStateKey())->bytes);
    const CasFoldSeal seal = decodeFoldSeal(backend->get(layout.foldSealKey(st.snap_generation, st.snap_attempt))->bytes);
    ASSERT_TRUE(seal.condemned_summary.contains(0)) << "the summary stays TOTAL over gc_shards";
    EXPECT_EQ(seal.condemned_summary.at(0).condemned_total, 0u) << "a rebuild condemns nothing";

    for (int i = 0; i < 4; ++i)
    {
        gc.runRegularRound();
        store->renewWatermarkOnce();
    }
    EXPECT_TRUE(blobPresent(*backend, layout, DB::UInt128(2)))
        << "the residual is RETENTION: an orphan is kept, not quietly reclaimed by a substitute pass";
}

/// Removing the condemnation must not disturb the other thing a rebuild owes: every hold in the prior
/// seal rides through VERBATIM (Task 8). Asserted together with the condemn-nothing rule because the
/// two used to be produced by the same pass, and a hold dropped here would hand back a baseline that
/// claims a frontier proof it does not have.
TEST(CasRebuildCondemnNothing, CarriesHoldsVerbatimWhileCondemningNothing)
{
    auto backend = std::make_shared<InMemoryBackend>();
    auto store = openPoolForTest(backend, /*gc_fold_max_defer_rounds=*/0);
    const Layout & layout = store->layout();

    publishAt(*backend, layout, kNsA, RefTxnId{1, 1}, "ref_a", /*build_sequence=*/1, DB::UInt128(1), /*birth=*/true);
    writeBlobBody(*backend, layout, DB::UInt128(2));   /// an orphan alongside the held namespace

    /// One real round first: it establishes the pool's `gc/state` and takes the lease under THIS
    /// identity, so the rebuild below is the disaster-recovery path and not a lease conflict.
    Gc gc(store, kGc);
    ASSERT_TRUE(gc.runRegularRound().acquired_lease);

    const RefHold planted{.reason = HoldReason::GapBelowWitness, .offending_position = RefTxnId{4, 9},
                          .retry_count = 17, .next_retry_round = 23};
    {
        const GcState adopted = decodeGcState(backend->get(layout.gcStateKey())->bytes);
        seedFoldCursorForTest(*backend, layout, kNsA, RefTxnId{1, 1}, planted,
                              adopted.snap_generation, adopted.snap_attempt);
    }

    const RebuildReport rep = gc.rebuildBaseline(/*force=*/true);
    ASSERT_TRUE(rep.performed) << rep.refusal;

    const GcState st = decodeGcState(backend->get(layout.gcStateKey())->bytes);
    const CasFoldSeal seal = decodeFoldSeal(backend->get(layout.foldSealKey(st.snap_generation, st.snap_attempt))->bytes);
    const auto it = seal.per_ns_shard.find(cursorKeyForTest(kNsA, /*shard=*/0));
    ASSERT_NE(it, seal.per_ns_shard.end());
    EXPECT_EQ(it->second.classification, 4);
    ASSERT_TRUE(it->second.hold.has_value());
    EXPECT_EQ(*it->second.hold, planted) << "a rebuild retried nothing, so it rewrites nothing about the hold";

    EXPECT_FALSE(condemnedInSealedRuns(*backend, layout, DB::UInt128(2)));
}

/// ---- fsck: arithmetic streams ----

/// A pool with nothing wrong reports nothing: no hole, no unproven namespace, a clean bill of health
/// and a zero exit. `unchecked` is not a resting state — it is a verdict a healthy pool never reaches.
TEST(CasRebuildCondemnNothingFsck, HealthyArithmeticPoolIsClean)
{
    auto backend = std::make_shared<InMemoryBackend>();
    auto store = openPoolForTest(backend, /*gc_fold_max_defer_rounds=*/0);
    const Layout & layout = store->layout();

    publishAt(*backend, layout, kNsA, RefTxnId{1, 1}, "ref_a", 1, DB::UInt128(1), /*birth=*/true);
    publishAt(*backend, layout, kNsA, RefTxnId{1, 2}, "ref_b", 2, DB::UInt128(2));
    publishAt(*backend, layout, kNsA, RefTxnId{1, 3}, "ref_c", 3, DB::UInt128(3));

    const FsckReport rep = runFsck(*store, /*detail=*/true);
    EXPECT_TRUE(rep.clean()) << formatFsckSummary(rep);
    EXPECT_EQ(rep.chain_broken, 0u);
    EXPECT_EQ(rep.unchecked, 0u) << "a healthy namespace is PROVEN, not merely uncomplained-about";
    EXPECT_EQ(rep.ref_records_walked, 3u);
    EXPECT_EQ(rep.dangling, 0u);
}

/// A 404 BELOW a durable same-epoch id. Ids are dense `1..T` within `(namespace, epoch)` (INV-1), so
/// this cannot be the end of a stream: a durable record is missing and every transaction above it is
/// unreachable. The verdict is FATAL — it appears in the machine-parseable summary line and it makes
/// the report unclean, which is what turns into the command's nonzero exit.
TEST(CasRebuildCondemnNothingFsck, MidChainHoleBelowAWitnessIsChainBroken)
{
    auto backend = std::make_shared<InMemoryBackend>();
    auto store = openPoolForTest(backend, /*gc_fold_max_defer_rounds=*/0);
    const Layout & layout = store->layout();

    publishAt(*backend, layout, kNsA, RefTxnId{1, 1}, "ref_a", 1, DB::UInt128(1), /*birth=*/true);
    publishAt(*backend, layout, kNsA, RefTxnId{1, 2}, "ref_b", 2, DB::UInt128(2));
    publishAt(*backend, layout, kNsA, RefTxnId{1, 3}, "ref_c", 3, DB::UInt128(3));

    /// Punch the hole: {1,2} is gone while {1,3} stays durable and listed.
    const String holed = layout.refLogKey(RefNamespaceId::stageATransition(kNsA), RefTxnId{1, 2});
    const HeadResult h = backend->head(holed);
    ASSERT_TRUE(h.exists);
    backend->deleteExact(holed, h.token);

    FsckReport rep;
    ASSERT_NO_THROW(rep = runFsck(*store, /*detail=*/true))
        << "a finding is RECORDED, never thrown — an fsck that dies reports nothing";
    EXPECT_EQ(rep.chain_broken, 1u);
    EXPECT_FALSE(rep.clean()) << "chain-broken is a hard finding";
    EXPECT_NE(formatFsckSummary(rep).find("chain_broken=1"), String::npos) << formatFsckSummary(rep);

    bool row = false;
    for (const FsckObject & o : rep.objects)
        if (o.cls == FsckClass::ChainBroken)
            row = true;
    EXPECT_TRUE(row) << "the fatal must name the position it was detected at";
}

/// The tail ABOVE `_ckpt.checkpoint` is WALKED, not assumed. Here the store lists neither of the two
/// records above the checkpoint, so a listing-driven audit would see an empty tail and report a clean
/// pool it never read. Arithmetic reads them by exact key: they are walked, counted, and the namespace
/// comes back PROVEN — not `unchecked`, which is reserved for what cannot be proved at all.
TEST(CasRebuildCondemnNothingFsck, TailAboveTheCheckpointIsWalkedNotUnchecked)
{
    auto backend = std::make_shared<HintHoleBackend>();
    auto store = openPoolForTest(backend, /*gc_fold_max_defer_rounds=*/0);
    const Layout & layout = store->layout();

    publishAt(*backend, layout, kNsA, RefTxnId{1, 1}, "ref_a", 1, DB::UInt128(1), /*birth=*/true);
    publishAt(*backend, layout, kNsA, RefTxnId{1, 2}, "ref_b", 2, DB::UInt128(2));
    publishAt(*backend, layout, kNsA, RefTxnId{1, 3}, "ref_c", 3, DB::UInt128(3));
    publishAt(*backend, layout, kNsA, RefTxnId{1, 4}, "ref_d", 4, DB::UInt128(4));

    /// A published snapshot at {1,2}, named by the checkpoint. Its bytes are the codec's own view of
    /// the state at {1,2}, so the snapshot oracle validates rather than trips over it.
    writeRefSnapshotRaw(*backend, layout,
                        snapshotOf(stateAfter(*backend, layout, kNsA, {RefTxnId{1, 1}, RefTxnId{1, 2}}), kNsA.string()));
    writeCkptRaw(*backend, layout, kNsA,
                 RefCkpt{.life_epoch = 1, .checkpoint_snapshot_id = RefTxnId{1, 2}, .last_epoch_seal = std::nullopt});

    /// The store stops listing the tail. It stays perfectly readable by exact key.
    backend->hide(layout.refLogKey(RefNamespaceId::stageATransition(kNsA), RefTxnId{1, 3}));
    backend->hide(layout.refLogKey(RefNamespaceId::stageATransition(kNsA), RefTxnId{1, 4}));

    const FsckReport rep = runFsck(*store, /*detail=*/true);
    ASSERT_GT(backend->holesServed(), 0u) << "the tail was never actually hidden from a LIST";
    EXPECT_EQ(rep.ref_records_walked, 2u) << "the two records above the checkpoint must be read by exact key";
    EXPECT_EQ(rep.unchecked, 0u) << "a walked tail is PROVEN; `unchecked` is not a default";
    EXPECT_EQ(rep.chain_broken, 0u);
    EXPECT_TRUE(rep.clean()) << formatFsckSummary(rep);
}

/// A SEALED multi-epoch stream, walked. The epoch boundary is crossed the way the protocol proves it —
/// through the next epoch's `prev_epoch_seal` back-chain — never by guessing `epoch + 1`. The
/// checkpoint sits below the seal, so the tail the walk owes covers the boundary itself.
TEST(CasRebuildCondemnNothingFsck, SealedStreamIsWalkedAcrossTheEpochBoundary)
{
    auto backend = std::make_shared<InMemoryBackend>();
    auto store = openPoolForTest(backend, /*gc_fold_max_defer_rounds=*/0);
    const Layout & layout = store->layout();

    seedSealedTwoEpochStream(*backend, layout, kNsA);
    writeRefSnapshotRaw(*backend, layout,
                        snapshotOf(stateAfter(*backend, layout, kNsA, {RefTxnId{1, 1}, RefTxnId{1, 2}}), kNsA.string()));
    writeCkptRaw(*backend, layout, kNsA,
                 RefCkpt{.life_epoch = 1, .checkpoint_snapshot_id = RefTxnId{1, 2}, .last_epoch_seal = RefTxnId{1, 3}});

    const FsckReport rep = runFsck(*store, /*detail=*/true);
    EXPECT_TRUE(rep.clean()) << formatFsckSummary(rep);
    EXPECT_EQ(rep.chain_broken, 0u);
    EXPECT_EQ(rep.unchecked, 0u) << "a PROVED crossing is not an unproven one";
    EXPECT_EQ(rep.ref_records_walked, 3u) << "the seal plus both records of the epoch it opened";
}

/// The snapshot oracle over the same sealed stream. It replays the tail through the shared state
/// machine, and that tail CONTAINS a seal and an epoch crossing — the case that only started applying
/// in T6. A published snapshot above the boundary must reproduce byte-for-byte from below it.
TEST(CasRebuildCondemnNothingFsck, SnapshotOracleReplaysASealedStream)
{
    auto backend = std::make_shared<InMemoryBackend>();
    auto store = openPoolForTest(backend, /*gc_fold_max_defer_rounds=*/0);
    const Layout & layout = store->layout();

    seedSealedTwoEpochStream(*backend, layout, kNsA);
    /// Two snapshots: the oracle checks the newest (`{2,2}`, above the boundary) by replaying from the
    /// older one (`{1,2}`, below it) — so the seal and the crossing are inside the replayed span.
    writeRefSnapshotRaw(*backend, layout,
                        snapshotOf(stateAfter(*backend, layout, kNsA, {RefTxnId{1, 1}, RefTxnId{1, 2}}), kNsA.string()));
    writeRefSnapshotRaw(*backend, layout, snapshotOf(recoverRefTable(*backend, layout, kNsA), kNsA.string()));

    const FsckReport rep = runFsck(*store, /*detail=*/true);
    EXPECT_TRUE(rep.clean()) << formatFsckSummary(rep);
    EXPECT_EQ(rep.snapshot_oracle_checked, 1u) << "the oracle must have been able to replay this table";
    EXPECT_EQ(rep.snapshot_oracle_mismatches, 0u);
    EXPECT_EQ(rep.unchecked, 0u) << "an oracle that could not replay the seal would report `unchecked` here";
}

/// `unchecked` is the honest third answer, and it is reserved for exactly that. Here `ns_a`'s epoch 1
/// is never sealed while a record of epoch 2 exists that chains from nothing: the crossing has no
/// proof, so fsck says so instead of either blessing the namespace or condemning it. The healthy
/// `ns_b` in the same pool is unaffected — one namespace's unprovable stream never spreads.
TEST(CasRebuildCondemnNothingFsck, UncheckedOnlyForTheGenuinelyUnprovable)
{
    auto backend = std::make_shared<InMemoryBackend>();
    auto store = openPoolForTest(backend, /*gc_fold_max_defer_rounds=*/0);
    const Layout & layout = store->layout();

    publishAt(*backend, layout, kNsA, RefTxnId{1, 1}, "ref_a", 1, DB::UInt128(1), /*birth=*/true);
    publishAt(*backend, layout, kNsA, RefTxnId{1, 2}, "ref_b", 2, DB::UInt128(2));
    /// A record of epoch 2 that chains from `{1, 5}` — a position epoch 1 never reached and certainly
    /// never sealed. The walk stops at the frontier of epoch 1 having applied an ORDINARY record, so the
    /// crossing's first gate (the record we stopped on must be a seal) fails and no back-chain can
    /// rescue it. Unprovable, which is different from broken: nothing here says a record was LOST.
    publishAt(*backend, layout, kNsA, RefTxnId{2, 1}, "ref_c", 1, DB::UInt128(3), /*birth=*/false,
              /*prev_epoch_seal=*/RefTxnId{1, 5});

    publishAt(*backend, layout, kNsB, RefTxnId{1, 1}, "ref_z", 1, DB::UInt128(9), /*birth=*/true);

    FsckReport rep;
    ASSERT_NO_THROW(rep = runFsck(*store, /*detail=*/true));
    EXPECT_EQ(rep.unchecked, 1u) << "exactly the namespace that cannot be proved — never the healthy one";
    EXPECT_EQ(rep.chain_broken, 0u) << "an unprovable crossing is not a proven hole";
    EXPECT_GE(uncheckedRowsMentioning(rep, "crossing"), 1u) << "the verdict must say what could not be proved";
}

/// W2 (Task-3 review): a holed namespace used to make the WHOLE scan throw — `applyOne` raises
/// `CORRUPTED_DATA` on a non-contiguous replay and nothing caught it, so one bad table aborted the
/// audit and every namespace after it went unexamined. For recovery, throwing is the correct
/// fail-close; for a read-only diagnostic it violates "record and continue, never wedge".
TEST(CasRebuildCondemnNothingFsck, OneBadNamespaceDoesNotAbortTheAudit)
{
    auto backend = std::make_shared<InMemoryBackend>();
    auto store = openPoolForTest(backend, /*gc_fold_max_defer_rounds=*/0);
    const Layout & layout = store->layout();

    /// `ns_a` sorts FIRST, so a scan that dies on it never reaches `ns_b`.
    publishAt(*backend, layout, kNsA, RefTxnId{1, 1}, "ref_a", 1, DB::UInt128(1), /*birth=*/true);
    publishAt(*backend, layout, kNsA, RefTxnId{1, 2}, "ref_b", 2, DB::UInt128(2));
    publishAt(*backend, layout, kNsA, RefTxnId{1, 3}, "ref_c", 3, DB::UInt128(3));
    const String holed = layout.refLogKey(RefNamespaceId::stageATransition(kNsA), RefTxnId{1, 2});
    const HeadResult h = backend->head(holed);
    ASSERT_TRUE(h.exists);
    backend->deleteExact(holed, h.token);

    publishAt(*backend, layout, kNsB, RefTxnId{1, 1}, "ref_z", 1, DB::UInt128(9), /*birth=*/true);

    FsckReport rep;
    ASSERT_NO_THROW(rep = runFsck(*store, /*detail=*/true));
    EXPECT_EQ(rep.chain_broken, 1u);
    EXPECT_GE(rep.reachable, 1u) << "the namespace AFTER the broken one must still have been examined";
    EXPECT_EQ(rep.dangling, 0u) << "`ns_b` is healthy; a wedged scan would have reported nothing about it";
}
