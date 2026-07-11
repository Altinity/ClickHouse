#include <gtest/gtest.h>

#include <optional>
#include <vector>

#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasBuild.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasEvent.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasFsck.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasGc.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasServerRoot.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasStore.h>
#include "cas_test_helpers.h"

using namespace DB::Cas;
using namespace DB::Cas::tests;

namespace
{
const UInt128 kGc = hexToU128("00000000000000000000000000000001");
ManifestRef ref(const String &, uint64_t seq, uint64_t inst)
{
    return ManifestRef{.writer_epoch = 1, .build_sequence = seq, .manifest_ordinal = static_cast<uint32_t>(inst)};
}
bool blobExists(InMemoryBackend & b, const Layout & layout, const UInt128 & hash)
{
    return b.head(layout.blobKey(BlobRef{BlobHashAlgo::CityHash128, BlobDigest::fromU128(hash)})).exists;
}

/// The current retired entry for `hash` (dereferenced through gc/state.retired_refs, shard 0), or nullopt.
std::optional<RetiredEntry> currentEntryFor(Backend & backend, const Layout & layout, const UInt128 & hash)
{
    for (const RetiredEntry & e : currentRetiredSet(backend, layout, /*shard*/0))
        if (e.ref == BlobRef{BlobHashAlgo::CityHash128, BlobDigest::fromU128(hash)})
            return e;
    return std::nullopt;
}
}

/// The owner-removed manifest body is deleted only after a full round (its decrement is sealed — #11).
TEST(CasGcRetire, ManifestBodyDeletedAfterDecrementsSealed)
{
    auto backend = std::make_shared<InMemoryBackend>();
    auto store = openStoreForTest(backend);
    const RootNamespace ns{"00/aa@cas@"};
    const ManifestRef r = ref("srv-a:1", 1, 0xAA);
    writeManifestRaw(*backend, store->layout(), ns, r, {blobEntryFor("a", DB::UInt128(1))});
    publishCommittedTransition(*backend, store->layout(), ns, "tbl", std::nullopt, r);
    Gc gc(store, kGc);
    gc.runRegularRound();
    EXPECT_TRUE(backend->head(store->layout().manifestKey(ManifestId{ns, r})).exists);

    dropRefTransition(*backend, store->layout(), ns, "tbl", r);
    gc.runRegularRound();
    EXPECT_FALSE(backend->head(store->layout().manifestKey(ManifestId{ns, r})).exists);
}

/// A publish racing the pass (in-degree restored) is SPARED, not deleted (#14).
TEST(CasGcRecheck, PublishRacingFenceSparesBlob)
{
    auto backend = std::make_shared<InMemoryBackend>();
    auto store = openStoreForTest(backend);
    const RootNamespace ns{"00/aa@cas@"};
    const ManifestRef r1 = ref("srv-a:1", 1, 0xA1);
    const ManifestRef r2 = ref("srv-a:1", 2, 0xA2);
    writeBlobBody(*backend, store->layout(), DB::UInt128(1));
    writeManifestRaw(*backend, store->layout(), ns, r1, {blobEntryFor("a", DB::UInt128(1))});
    publishCommittedTransition(*backend, store->layout(), ns, "tbl", std::nullopt, r1);
    Gc gc(store, kGc);
    gc.runRegularRound();
    // Repoint the ref from r1 to r2 (both reference blob 1) in the same window before the next round
    // folds. ONE repoint event {old=committed(r1), new=committed(r2)} — the -1 (r1's body) and +1
    // (r2's body) net to in-degree 1, so blob 1 is re-pinned and must be SPARED. (Not a separate drop
    // THEN repoint — that would double-count the -1 on r1's body and over-delete.)
    writeManifestRaw(*backend, store->layout(), ns, r2, {blobEntryFor("a", DB::UInt128(1))});
    publishCommittedTransition(*backend, store->layout(), ns, "tbl", r1, r2);
    gc.runRegularRound();   // net in-degree 1 => spared
    EXPECT_TRUE(blobExists(*backend, store->layout(), DB::UInt128(1)));
}

/// A genuinely unreferenced blob is deleted with its exact token (the single content-delete site). The
/// delete is not one-round-after-drop: the entry condemns, graduates the round AFTER the condemning
/// round (round-paced, unconditional), then the NEXT pass executes the exact-token delete.
TEST(CasGcRecheck, UnreferencedBlobDeletedExactToken)
{
    auto backend = std::make_shared<InMemoryBackend>();
    auto store = openStoreForTest(backend);
    const RootNamespace ns{"00/aa@cas@"};
    const ManifestRef r = ref("srv-a:1", 1, 0xAA);
    writeBlobBody(*backend, store->layout(), DB::UInt128(1));
    writeManifestRaw(*backend, store->layout(), ns, r, {blobEntryFor("a", DB::UInt128(1))});
    publishCommittedTransition(*backend, store->layout(), ns, "tbl", std::nullopt, r);
    Gc gc(store, kGc);
    gc.runRegularRound();
    dropRefTransition(*backend, store->layout(), ns, "tbl", r);
    // The drop's -1 condemns blob 1; the retired-cursor pipeline (condemn -> graduate -> delete) reclaims it.
    EXPECT_TRUE(runRoundsUntilAbsent(store, gc, *backend, store->layout(), DB::UInt128(1)));
    EXPECT_FALSE(backend->head(store->layout().manifestKey(ManifestId{ns, r})).exists);
}

/// Task 5 (spec 2026-07-09 §raw-body-refinement, v3): GC writes the writer's freshness meta ALONGSIDE
/// the unchanged ledger retire (RetiredEntry, body token) — the meta is the writer/promote gate's
/// point-read signal (Task 3/4), not a replacement for the ledger or the exact-token body delete.
TEST(CasGcRetire, CondemnWritesMetaCondemned)
{
    auto backend = std::make_shared<InMemoryBackend>();
    auto store = openStoreForTest(backend);
    const RootNamespace ns{"00/aa@cas@"};
    const ManifestRef r = ref("srv-a:1", 1, 0xAA);
    writeBlobBody(*backend, store->layout(), DB::UInt128(1));
    writeManifestRaw(*backend, store->layout(), ns, r, {blobEntryFor("a", DB::UInt128(1))});
    publishCommittedTransition(*backend, store->layout(), ns, "tbl", std::nullopt, r);
    Gc gc(store, kGc);
    gc.runRegularRound();   /// +1 folds; blob referenced (`writeBlobBody` never wrote a meta itself)
    dropRefTransition(*backend, store->layout(), ns, "tbl", r);
    gc.runRegularRound();   /// -1 folds => in-degree 0 => condemned THIS round

    const auto lm = loadMetaForTest(*backend, store->layout(), DB::UInt128(1));
    ASSERT_TRUE(lm.has_value()) << "GC must write the freshness meta at condemn time (Task 5)";
    EXPECT_EQ(lm->meta.state, MetaState::Condemned);
    EXPECT_TRUE(blobExists(*backend, store->layout(), DB::UInt128(1))) << "condemned, NOT yet deleted";
}

/// Task 5: the round's exact-token body delete drops the meta alongside it (advisory, no tombstone —
/// an absent meta reads exactly like a Clean one for the writer's point-read gate).
TEST(CasGcRetire, DeleteRemovesBodyAndMeta)
{
    auto backend = std::make_shared<InMemoryBackend>();
    auto store = openStoreForTest(backend);
    const RootNamespace ns{"00/aa@cas@"};
    const ManifestRef r = ref("srv-a:1", 1, 0xAA);
    writeBlobBody(*backend, store->layout(), DB::UInt128(1));
    writeManifestRaw(*backend, store->layout(), ns, r, {blobEntryFor("a", DB::UInt128(1))});
    publishCommittedTransition(*backend, store->layout(), ns, "tbl", std::nullopt, r);
    Gc gc(store, kGc);
    gc.runRegularRound();
    dropRefTransition(*backend, store->layout(), ns, "tbl", r);
    // condemn -> graduate (round-paced) -> delete (the retired-cursor pipeline).
    ASSERT_TRUE(runRoundsUntilAbsent(store, gc, *backend, store->layout(), DB::UInt128(1)));

    EXPECT_FALSE(blobExists(*backend, store->layout(), DB::UInt128(1))) << "body gone via exact-token delete";
    EXPECT_FALSE(loadMetaForTest(*backend, store->layout(), DB::UInt128(1)).has_value())
        << "the meta must be dropped alongside the exact-token body delete (Task 5)";
}

/// GC freshness meta is ADD-ONLY (spec 2026-07-11 deposed-leader `clearSparedMeta` fix): an entry whose
/// in-degree recovers before graduation is SPARED (unchanged ledger behavior) but GC must NEVER flip its
/// meta `Condemned -> Clean` on the spare. A deposed leader that cleared-then-lost the round would leave a
/// stray-`Clean` over a still-condemned body; a writer reading `Clean` would reuse the exact condemned
/// token, which a stale exact-token redelete then deletes (INV_NO_LOSS live-blob loss —
/// `reports/2026-07-11-cas-deposed-leader-stray-clean-meta.md`). The spare leaves the meta `Condemned`;
/// ONLY a writer that displaces the body with a fresh incarnation token publishes `Clean`.
TEST(CasGcRetire, SpareLeavesMetaCondemned)
{
    auto backend = std::make_shared<InMemoryBackend>();
    auto store = openStoreForTest(backend);
    const RootNamespace ns{"00/aa@cas@"};

    /// A content-addressed body + Clean meta via a real fresh upload, so a later writer dedup-attempt
    /// resolves to THIS exact hash (GC condemns it; the writer resurrects it).
    const String payload = "spare-add-only-payload";
    const UInt128 hash = u128Of(payload);
    const BlobRef id = idOf(payload);
    {
        auto seed = store->startBuild({});
        seed->putBlob(id, BlobSource::fromString(payload));
    }
    const Token t_seed = backend->head(store->layout().blobKey(id)).token;

    const ManifestRef r1 = ref("srv-a:1", 1, 0xA1);
    writeManifestRaw(*backend, store->layout(), ns, r1, {blobEntryFor("a", hash)});
    publishCommittedTransition(*backend, store->layout(), ns, "tbl", std::nullopt, r1);
    Gc gc(store, kGc);
    gc.runRegularRound();   /// +1 folds
    dropRefTransition(*backend, store->layout(), ns, "tbl", r1);
    gc.runRegularRound();   /// -1 folds => in-degree 0 => condemned; meta flipped Condemned
    ASSERT_TRUE(currentEntryFor(*backend, store->layout(), hash).has_value());
    {
        const auto lm = loadMetaForTest(*backend, store->layout(), hash);
        ASSERT_TRUE(lm.has_value());
        ASSERT_EQ(lm->meta.state, MetaState::Condemned);
    }

    /// Re-reference the SAME blob (same body/token — never re-uploaded) via a fresh ref before
    /// graduation. The pass merge nets in-degree back to 1: the prior retired entry is SPARED
    /// (recovery wins, even past the floor) -- not the resurrect-supersede path (the token never changed).
    const ManifestRef r2 = ref("srv-a:1", 2, 0xA2);
    writeManifestRaw(*backend, store->layout(), ns, r2, {blobEntryFor("a", hash)});
    publishCommittedTransition(*backend, store->layout(), ns, "tbl_detached", std::nullopt, r2);
    gc.runRegularRound();   /// +1 folds => spared

    EXPECT_FALSE(currentEntryFor(*backend, store->layout(), hash).has_value())
        << "the spared entry drops from the retired set";
    EXPECT_TRUE(blobExists(*backend, store->layout(), hash));
    EXPECT_EQ(backend->head(store->layout().blobKey(id)).token, t_seed)
        << "spare does not touch the body — the incarnation token is unchanged";

    /// ADD-ONLY: the spare must NOT clear the meta back to Clean (that is the deposed-leader hole).
    {
        const auto lm = loadMetaForTest(*backend, store->layout(), hash);
        ASSERT_TRUE(lm.has_value());
        EXPECT_EQ(lm->meta.state, MetaState::Condemned)
            << "GC freshness meta is add-only: a spare leaves the meta Condemned (never -> Clean)";
    }

    /// Only a WRITER re-publishes Clean, and only by displacing the body with a fresh incarnation token:
    /// a dedup-attempt on the condemned hash resurrects (uploadFromSource) — the body token CHANGES and
    /// the meta flips to Clean WITH that token change.
    auto build = store->startBuild({});
    auto ref_w = build->putBlob(id, BlobSource::fromString(payload));
    EXPECT_EQ(ref_w.ref, id);
    const Token t_resurrect = backend->head(store->layout().blobKey(id)).token;
    EXPECT_NE(t_resurrect, t_seed) << "resurrect displaces the body with a fresh incarnation token";
    const auto lm_after = loadMetaForTest(*backend, store->layout(), hash);
    ASSERT_TRUE(lm_after.has_value());
    EXPECT_EQ(lm_after->meta.state, MetaState::Clean)
        << "the writer's resurrect path is the SOLE Condemned -> Clean transition";
}

/// Two-leader stale-redelete regression — the executable form of the deposed-leader spec §2. A stale
/// leader's pre-CAS exact-token redelete `deleteExact(h, t1)` must never delete a live reuse. With the
/// buggy clear-on-spare, a spare publishes `Clean`; a writer reads `Clean` and REUSES `t1`; the stale
/// `deleteExact(t1)` then deletes the LIVE body (INV_NO_LOSS). Add-only meta closes it: the spare leaves
/// `Condemned`, the writer resurrects to `t2`, and the stale `deleteExact(t1)` is a `TokenMismatch` no-op.
///
/// Interleaving fidelity (APPROXIMATED): the deposed leader's destructive side effect is its pre-CAS
/// exact-token `deleteExact(h, t1)`. We reproduce it deterministically by CAPTURING `t1` at condemn time
/// (exactly the token a paused leader's `delete_pending` snapshot holds) and firing that exact
/// `deleteExact` AFTER the surviving leader's spare and the writer's resurrect — the faithful destructive
/// op, without a mid-round CAS-interrupt seam on the delete path (which the backend does not expose).
TEST(CasGcRetire, StaleRedeleteAfterSpareDoesNotDeleteLiveReuse)
{
    auto backend = std::make_shared<InMemoryBackend>();
    auto store = openStoreForTest(backend);
    const RootNamespace ns{"00/aa@cas@"};

    const String payload = "two-leader-stale-redelete-payload";
    const UInt128 hash = u128Of(payload);
    const BlobRef id = idOf(payload);
    const String blob_key = store->layout().blobKey(id);
    {
        auto seed = store->startBuild({});
        seed->putBlob(id, BlobSource::fromString(payload));
    }

    const ManifestRef r1 = ref("srv-a:1", 1, 0xA1);
    writeManifestRaw(*backend, store->layout(), ns, r1, {blobEntryFor("a", hash)});
    publishCommittedTransition(*backend, store->layout(), ns, "tbl", std::nullopt, r1);
    Gc gc(store, kGc);
    gc.runRegularRound();   /// +1 folds
    dropRefTransition(*backend, store->layout(), ns, "tbl", r1);
    gc.runRegularRound();   /// -1 => in-degree 0 => condemned at t1

    /// The OLD leader L1's planned pre-CAS delete uses the EXACT token it observed at condemn: capture t1.
    const auto condemned_entry = currentEntryFor(*backend, store->layout(), hash);
    ASSERT_TRUE(condemned_entry.has_value());
    const Token t1 = condemned_entry->token;
    ASSERT_EQ(backend->head(blob_key).token, t1);

    /// A NEW leader L2 folds a +1 that recovered h's in-degree and adopts a SPARE for h.
    const ManifestRef r2 = ref("srv-a:1", 2, 0xA2);
    writeManifestRaw(*backend, store->layout(), ns, r2, {blobEntryFor("a", hash)});
    publishCommittedTransition(*backend, store->layout(), ns, "tbl_live", std::nullopt, r2);
    gc.runRegularRound();   /// +1 => spared

    /// Add-only: the spare left the meta Condemned (the stale-redelete guard depends on it).
    {
        const auto lm = loadMetaForTest(*backend, store->layout(), hash);
        ASSERT_TRUE(lm.has_value());
        EXPECT_EQ(lm->meta.state, MetaState::Condemned)
            << "add-only: a spare must not clear the meta (the writer must still see the hash condemned)";
    }

    /// A writer dedup-hits h. It point-reads Condemned and RESURRECTS to a fresh token t2
    /// (uploadFromSource) — it never reuses t1.
    {
        auto build = store->startBuild({});
        build->putBlob(id, BlobSource::fromString(payload));
    }
    const Token t2 = backend->head(blob_key).token;
    EXPECT_NE(t2, t1) << "the writer resurrected to a fresh incarnation, not a reuse of t1";

    /// L1 resumes and executes its stale pre-CAS exact-token redelete `deleteExact(h, t1)`: it must be a
    /// TokenMismatch no-op (the live body is now t2), NEVER a Deleted of the live reuse.
    const DeleteOutcome stale = backend->deleteExact(blob_key, t1);
    EXPECT_EQ(stale.kind, DeleteOutcome::Kind::TokenMismatch)
        << "the stale exact-token redelete must miss the live reuse (add-only closes INV_NO_LOSS)";

    /// The live body under t2 survives, stays reachable via the committed r2, and fsck sees no dangle.
    const HeadResult hr = backend->head(blob_key);
    ASSERT_TRUE(hr.exists);
    EXPECT_EQ(hr.token, t2);
    EXPECT_EQ(runFsck(*store, /*detail=*/false).dangling, 0u)
        << "no live reference dangles: the stale redelete did not delete the reused body";
}

/// Copy-forward aftermath, republished arm (spec 2026-07-02-cas-copy-forward-condemned-evidence.md):
/// after a condemned incarnation (hash, t0) is displaced by a verified copy-forward (fresh token t1)
/// and the republished part's +1 lands, the listed (hash, t0) entry settles WITHOUT touching the new
/// incarnation: its exact-token delete is a mismatch no-op and the entry drops; the blob survives at t1.
TEST(CasGcRetire, CopyForwardedBlobSurvivesWhenRepublished)
{
    auto backend = std::make_shared<InMemoryBackend>();
    auto store = openStoreForTest(backend);
    const RootNamespace ns{"00/aa@cas@"};
    const ManifestRef r1 = ref("srv-a:1", 1, 0xA1);
    writeBlobBody(*backend, store->layout(), DB::UInt128(1));
    writeManifestRaw(*backend, store->layout(), ns, r1, {blobEntryFor("a", DB::UInt128(1))});
    publishCommittedTransition(*backend, store->layout(), ns, "tbl", std::nullopt, r1);
    Gc gc(store, kGc);
    gc.runRegularRound();
    dropRefTransition(*backend, store->layout(), ns, "tbl", r1);
    gc.runRegularRound();   /// -1 folds => in-degree 0 => entry (1, t0) condemned
    ASSERT_TRUE(currentEntryFor(*backend, store->layout(), DB::UInt128(1)).has_value());

    /// The raw equivalent of Build::copyForwardFromCondemned: displace EXACTLY t0 with the same
    /// verified bytes under a fresh token t1, then republish a part referencing the blob (the
    /// promoted dst ref of a republishRef move).
    const String blob_key = store->layout().blobKey(BlobRef{BlobHashAlgo::CityHash128, BlobDigest::fromU128(DB::UInt128(1))});
    const Token t0 = backend->head(blob_key).token;
    const auto res = backend->putOverwrite(blob_key, backend->get(blob_key)->bytes, t0);
    ASSERT_EQ(res.outcome, PutOutcome::Done);
    const ManifestRef r2 = ref("srv-a:1", 2, 0xA2);
    writeManifestRaw(*backend, store->layout(), ns, r2, {blobEntryFor("a", DB::UInt128(1))});
    publishCommittedTransition(*backend, store->layout(), ns, "tbl_detached", std::nullopt, r2);

    /// The +1 folds => spared; the (1, t0) entry drops; the t1 incarnation is never deleted.
    for (int i = 0; i < 4; ++i)
        gc.runRegularRound();
    EXPECT_FALSE(currentEntryFor(*backend, store->layout(), DB::UInt128(1)).has_value());
    const HeadResult hr = backend->head(blob_key);
    ASSERT_TRUE(hr.exists);
    EXPECT_EQ(hr.token, res.token);
}

/// Copy-forward aftermath, stale-entry arm: a listed (hash, t0) entry whose incarnation was
/// displaced (token now t1) with NO accompanying owner events. The entry graduates and its
/// exact-token delete MISMATCHES — a no-op, the entry drops, the t1 incarnation is NEVER
/// wrong-token-deleted (no wedge, no unsafe delete). This is a RAW-displacement model, stronger
/// than the real flow: in real `republishRef` the dst precommit + body are durable BEFORE the
/// promote pre-pass runs (reachability-before-content, B188), so an abandoned real copy-forward
/// is fully reclaimed by the pipeline (+1 spare -> reclaim -1 -> transition to zero -> fresh
/// (hash, t1) entry -> exact delete). The raw shape pins the GC-side invariant in isolation.
TEST(CasGcRetire, AbandonedCopyForwardDropsEntryWithoutWrongTokenDelete)
{
    auto backend = std::make_shared<InMemoryBackend>();
    auto store = openStoreForTest(backend);
    const RootNamespace ns{"00/aa@cas@"};
    const ManifestRef r1 = ref("srv-a:1", 1, 0xA1);
    writeBlobBody(*backend, store->layout(), DB::UInt128(1));
    writeManifestRaw(*backend, store->layout(), ns, r1, {blobEntryFor("a", DB::UInt128(1))});
    publishCommittedTransition(*backend, store->layout(), ns, "tbl", std::nullopt, r1);
    Gc gc(store, kGc);
    gc.runRegularRound();
    dropRefTransition(*backend, store->layout(), ns, "tbl", r1);
    gc.runRegularRound();
    ASSERT_TRUE(currentEntryFor(*backend, store->layout(), DB::UInt128(1)).has_value());

    const String blob_key = store->layout().blobKey(BlobRef{BlobHashAlgo::CityHash128, BlobDigest::fromU128(DB::UInt128(1))});
    const Token t0 = backend->head(blob_key).token;
    const auto res = backend->putOverwrite(blob_key, backend->get(blob_key)->bytes, t0);
    ASSERT_EQ(res.outcome, PutOutcome::Done);

    /// No events land at all (raw displacement). Drive rounds with the store's ack kept current so
    /// the (1, t0) entry graduates; its exact-token delete mismatches t1 and the entry drops.
    for (int i = 0; i < 6; ++i)
    {
        gc.runRegularRound();
        store->renewWatermarkOnce();
    }
    EXPECT_FALSE(currentEntryFor(*backend, store->layout(), DB::UInt128(1)).has_value())
        << "the stale (hash, t0) entry must settle (mismatch redelete drops it), not wedge the list";
    const HeadResult hr = backend->head(blob_key);
    ASSERT_TRUE(hr.exists) << "the fresh incarnation must never be deleted under the stale token";
    EXPECT_EQ(hr.token, res.token);
}

/// A completed round adopts the SAME attempt its fold minted (the round's single gc/state CAS commits the
/// fold's (snap_generation, snap_attempt) together). Completion seals are a retired concept, so the durable
/// index of the adopted round is the FOLD seal at (snap_generation, snap_attempt). Across rounds each
/// `runRegularRound` re-acquires the lease (bumping `lease.seq`), so a later round mints a FRESH attempt.
TEST(CasGcRecheck, CompletionInheritsFoldAttempt)
{
    auto backend = std::make_shared<InMemoryBackend>();
    auto store = openStoreForTest(backend);
    const RootNamespace ns{"00/aa@cas@"};
    const ManifestRef r = ref("srv-a:1", 1, 0xAA);
    writeBlobBody(*backend, store->layout(), DB::UInt128(1));
    writeManifestRaw(*backend, store->layout(), ns, r, {blobEntryFor("a", DB::UInt128(1))});
    publishCommittedTransition(*backend, store->layout(), ns, "tbl", std::nullopt, r);
    Gc gc(store, kGc);

    gc.runRegularRound();          // round 1: one pass, single CAS commits (snap_generation, snap_attempt)
    const auto after_round1 = decodeGcState(backend->get(store->layout().gcStateKey())->bytes);
    // The round adopted the attempt of THIS round's fold: snap_attempt == the lease.seq that folded it.
    EXPECT_EQ(after_round1.snap_attempt, after_round1.lease.seq);
    EXPECT_GT(after_round1.snap_generation, 0u);
    // The fold seal is durable under the adopted (snap_generation, snap_attempt) pair (no completion seal).
    EXPECT_TRUE(backend->head(store->layout()
        .foldSealKey(after_round1.snap_generation, after_round1.snap_attempt)).exists);

    dropRefTransition(*backend, store->layout(), ns, "tbl", r);
    gc.runRegularRound();          // round 2: re-acquire (bump lease.seq) -> fresh attempt at its fold
    const auto after_round2 = decodeGcState(backend->get(store->layout().gcStateKey())->bytes);
    EXPECT_EQ(after_round2.snap_attempt, after_round2.lease.seq);
    EXPECT_GT(after_round2.snap_attempt, after_round1.snap_attempt);   // per-round monotone attempt
    EXPECT_GT(after_round2.snap_generation, after_round1.snap_generation);
    EXPECT_TRUE(backend->head(store->layout()
        .foldSealKey(after_round2.snap_generation, after_round2.snap_attempt)).exists);
}

/// ---- round-paced graduation suite (spec 2026-07-02 + Task-9 amendment; re-keyed off acks in v3 Task 6) ----

/// A round performs NO fence writes to ref shards: the fence machinery is gone. A no-op round (no owner
/// events, nothing to fold) leaves the discovered ref shard's token byte-unchanged (the old fence step
/// would have bumped fence_round and rewritten the shard).
TEST(CasGcAckFloor, NoOpRoundDoesNotMutateRefShards)
{
    auto backend = std::make_shared<InMemoryBackend>();
    auto store = openStoreForTest(backend);
    const RootNamespace ns{"00/aa@cas@"};
    publishCommittedTransition(*backend, store->layout(), ns, "tbl", std::nullopt,
        ManifestRef{.writer_epoch = 1, .build_sequence = 1, .manifest_ordinal = 1});
    Gc gc(store, kGc);
    gc.runRegularRound();   // first round folds the publish
    const auto before = backend->head(store->layout().rootShardKey(ns, 0));
    ASSERT_TRUE(before.exists);
    gc.runRegularRound();   // a second, no-op round must not touch the ref shard
    const auto after = backend->head(store->layout().rootShardKey(ns, 0));
    ASSERT_TRUE(after.exists);
    EXPECT_EQ(before.token.value, after.token.value);   // no fence write, token unchanged
    // The registry object is gone (Task 4); the fence never existed to write it.
    EXPECT_FALSE(backend->get("p/gc/registry").has_value());
}

/// The canonical pipeline: a blob condemned at round K stays present after the condemning round; the
/// VERY NEXT round graduates it (round-paced, unconditional — condemn_round < current_round the first
/// round current_round exceeds it) and publishes it delete_pending — the blob still exists; the round
/// AFTER THAT executes the exact-token delete and the blob becomes absent. This pins the critical
/// off-by-one: current_round MUST equal state.round + 1 (the SAME basis condemn_round is stamped at),
/// so an entry graduates exactly one round after it was condemned — never the same round, never never.
TEST(CasGcAckFloor, CondemnThenGraduatesNextRoundThenDeletes)
{
    auto backend = std::make_shared<InMemoryBackend>();
    auto store = openStoreForTest(backend);
    const RootNamespace ns{"00/aa@cas@"};
    const ManifestRef r = ref("srv-a:1", 1, 0xAA);
    const UInt128 blob = DB::UInt128(1);
    writeBlobBody(*backend, store->layout(), blob);
    writeManifestRaw(*backend, store->layout(), ns, r, {blobEntryFor("a", blob)});
    publishCommittedTransition(*backend, store->layout(), ns, "tbl", std::nullopt, r);
    Gc gc(store, kGc);

    gc.runRegularRound();                 // round 1: folds the +1; blob referenced
    dropRefTransition(*backend, store->layout(), ns, "tbl", r);

    // The condemning round: the -1 drops in-degree to 0; the blob is condemned into the current retired
    // list but NOT deleted. The entry is present and NOT yet pending. report.condemned counts it.
    {
        const RoundReport rep = gc.runRegularRound();
        EXPECT_EQ(rep.condemned, 1u);        // one blob condemned this round
        EXPECT_EQ(rep.graduated, 0u);        // must NOT graduate the same round it was condemned
        EXPECT_EQ(rep.redeleted, 0u);        // nothing pending to delete yet
        EXPECT_TRUE(blobExists(*backend, store->layout(), blob));
        const auto e = currentEntryFor(*backend, store->layout(), blob);
        ASSERT_TRUE(e.has_value());
        EXPECT_FALSE(e->delete_pending);   // condemned, not yet graduated
    }

    // The VERY NEXT round graduates it deterministically (no ack/heartbeat dependency).
    {
        const RoundReport rep = gc.runRegularRound();
        EXPECT_EQ(rep.graduated, 1u);
        EXPECT_EQ(rep.redeleted, 0u);   // the delete lands on the NEXT pass, not this one
        const auto e = currentEntryFor(*backend, store->layout(), blob);
        ASSERT_TRUE(e.has_value());
        EXPECT_TRUE(e->delete_pending);
        EXPECT_TRUE(blobExists(*backend, store->layout(), blob));   // pending: still present this pass
    }

    // The pass AFTER the pending publish executes the exact-token delete; the blob becomes absent and the
    // entry is dropped from the current retired list. report.redeleted counts the executed pending delete.
    {
        const RoundReport rep = gc.runRegularRound();
        EXPECT_EQ(rep.redeleted, 1u);        // the pending delete executed this round
        EXPECT_FALSE(blobExists(*backend, store->layout(), blob));
        EXPECT_FALSE(currentEntryFor(*backend, store->layout(), blob).has_value());
    }
}

/// A publish re-referencing the condemned blob before graduation is folded and SPARES the entry: the entry
/// is dropped (recovery wins even past graduation) and the blob survives.
TEST(CasGcAckFloor, PublishBeforeGraduationSpares)
{
    auto backend = std::make_shared<InMemoryBackend>();
    auto store = openStoreForTest(backend);
    const RootNamespace ns{"00/aa@cas@"};
    const ManifestRef r1 = ref("srv-a:1", 1, 0xA1);
    const ManifestRef r2 = ref("srv-a:1", 2, 0xA2);
    const UInt128 blob = DB::UInt128(1);
    writeBlobBody(*backend, store->layout(), blob);
    writeManifestRaw(*backend, store->layout(), ns, r1, {blobEntryFor("a", blob)});
    publishCommittedTransition(*backend, store->layout(), ns, "tbl", std::nullopt, r1);
    Gc gc(store, kGc);

    gc.runRegularRound();
    store->renewWatermarkOnce();
    dropRefTransition(*backend, store->layout(), ns, "tbl", r1);
    gc.runRegularRound();   // condemns blob 1 (in-degree 0)
    store->renewWatermarkOnce();
    ASSERT_TRUE(currentEntryFor(*backend, store->layout(), blob).has_value());

    // Re-publish a committed ref pointing at the same blob BEFORE it graduates: the next pass folds the +1,
    // the merge sees in-degree 1, and the entry is spared (dropped from the retired list).
    writeManifestRaw(*backend, store->layout(), ns, r2, {blobEntryFor("a", blob)});
    publishCommittedTransition(*backend, store->layout(), ns, "tbl", std::nullopt, r2);
    gc.runRegularRound();
    store->renewWatermarkOnce();
    EXPECT_FALSE(currentEntryFor(*backend, store->layout(), blob).has_value());   // spared: entry dropped
    EXPECT_TRUE(blobExists(*backend, store->layout(), blob));

    // Keep running: the re-referenced blob must never be deleted.
    for (int i = 0; i < 4; ++i)
    {
        gc.runRegularRound();
        store->renewWatermarkOnce();
    }
    EXPECT_TRUE(blobExists(*backend, store->layout(), blob));
}

/// An expired mount is fenced out by the round's heartbeat step: gc_fenced is set on its body (a
/// token-guarded rewrite that bumps seq). The fence is pure liveness (re-arms the write fence so a
/// resumed sleeper can never mutate again); reclaim itself no longer depends on any mount's heartbeat —
/// graduation paces on GC rounds. The fenced mount's own subsequent renew then fails closed, because the
/// fence invalidated the token it held.
TEST(CasGcAckFloor, ExpiredMountFencedOutAndExcluded)
{
    auto backend = std::make_shared<InMemoryBackend>();
    // A live store (the GC leader's own mount, on the SYSTEM clock) plus a SECOND server's keeper on a
    // FAKE clock that we freeze — so GC's own fake clock can jump past it deterministically.
    std::vector<CasEvent> events;   /// declared BEFORE the Store so it outlives the background syncer's emits (ASan 2026-07-09)
    auto store = openStoreForTest(backend);
    const Layout & layout = store->layout();

    // srid2's keeper: started at fake now=1000 with ttl=100 => lease expires_at = 1100.
    const String srid2 = "stale-server";
    uint64_t srid2_now = 1000;
    MountLeaseKeeper srid2_keeper(backend, layout, srid2, DB::UInt128(0x2222), /*writer_epoch=*/1,
        std::chrono::milliseconds(100), [&] { return srid2_now; }, [] { return 0u; });
    srid2_keeper.start();
    ASSERT_FALSE(decodeMountLease(backend->get(layout.mountKey(srid2))->bytes).gc_fenced);

    // GC runs on a fake clock jumped well past srid2's deadline + margin (ttl/2 = 15000 for the store's
    // 30s ttl). The store's own mount carries a SYSTEM-clock expires_at (~1.7e12 ms), far above the fake
    // GC clock, so it stays live regardless of srid2's fate.
    uint64_t gc_now = 1100 + 60000;
    Gc gc(store, kGc, [&] { return gc_now; });

    // Capture the emitted events so we can assert the round emits exactly one GcFenceOut row for srid2.
    store->setEventSink([&](const CasEvent & e) { events.push_back(e); });

    const RootNamespace ns{"00/aa@cas@"};
    const ManifestRef r = ref("srv-a:1", 1, 0xAA);
    const UInt128 blob = DB::UInt128(7);
    writeBlobBody(*backend, layout, blob);
    writeManifestRaw(*backend, layout, ns, r, {blobEntryFor("a", blob)});
    publishCommittedTransition(*backend, layout, ns, "tbl", std::nullopt, r);
    const RoundReport rep = gc.runRegularRound();
    store->renewWatermarkOnce();

    // The round's heartbeat step fenced srid2 out (expired on the GC clock).
    EXPECT_EQ(rep.fence_outs, 1u);   // exactly one expired mount fenced-out this round
    const MountLease fenced = decodeMountLease(backend->get(layout.mountKey(srid2))->bytes);
    EXPECT_TRUE(fenced.gc_fenced);

    // Exactly one GcFenceOut audit row was emitted, naming srid2 in its detail.
    size_t fence_out_rows = 0;
    for (const CasEvent & e : events)
        if (e.type == CasEventType::GcFenceOut)
        {
            ++fence_out_rows;
            EXPECT_EQ(e.outcome, "fenced");
            EXPECT_FALSE(e.reason.empty());
            const auto it = e.detail.find("srid");
            ASSERT_NE(it, e.detail.end());
            EXPECT_EQ(it->second, srid2);
        }
    EXPECT_EQ(fence_out_rows, 1u);

    // srid2's writer comes back and tries to renew: its held token was invalidated by the fence rewrite,
    // so renewOnce fails closed. (It renews on its own clock; liveness is irrelevant — the token guard
    // trips regardless.)
    srid2_now = 1050;
    EXPECT_THROW(srid2_keeper.renewOnce(), DB::Exception);

    // The fence-out is pure liveness cleanup: reclaim proceeds through the normal (round-paced) pipeline
    // regardless of srid2's fate — fencing one stale mount must never wedge the reclaim pipeline.
    dropRefTransition(*backend, layout, ns, "tbl", r);
    EXPECT_TRUE(runRoundsUntilAbsent(store, gc, *backend, layout, blob));
}

/// deleteExact against a blob the writer RECREATED (fresh incarnation, different token) between the pending
/// publish and the deleting pass lands TokenMismatch — a terminal-OK outcome recorded as a replace: the
/// fresh incarnation is a live object and survives. report.replaced counts it.
TEST(CasGcAckFloor, RecreatedBlobDeleteIsTokenMismatchOk)
{
    auto backend = std::make_shared<InMemoryBackend>();
    auto store = openStoreForTest(backend);
    const RootNamespace ns{"00/aa@cas@"};
    const ManifestRef r = ref("srv-a:1", 1, 0xAA);
    const UInt128 blob = DB::UInt128(1);
    const BlobRef blob_id{BlobHashAlgo::CityHash128, BlobDigest::fromU128(blob)};
    writeBlobBody(*backend, store->layout(), blob);
    writeManifestRaw(*backend, store->layout(), ns, r, {blobEntryFor("a", blob)});
    publishCommittedTransition(*backend, store->layout(), ns, "tbl", std::nullopt, r);
    Gc gc(store, kGc);

    gc.runRegularRound();
    store->renewWatermarkOnce();
    dropRefTransition(*backend, store->layout(), ns, "tbl", r);
    gc.runRegularRound();   // condemn (captures the ORIGINAL token)
    store->renewWatermarkOnce();

    // Drive rounds until the entry is delete_pending (the token it holds is the original observation).
    bool pending = false;
    for (int i = 0; i < 6 && !pending; ++i)
    {
        gc.runRegularRound();
        store->renewWatermarkOnce();
        const auto e = currentEntryFor(*backend, store->layout(), blob);
        pending = e && e->delete_pending;
    }
    ASSERT_TRUE(pending);

    // The writer recreates the blob with a FRESH incarnation before the deleting pass: the current token no
    // longer matches the pending entry's captured token.
    displaceBlobToken(*backend, store->layout(), blob_id);

    // The deleting pass issues deleteExact(entry.token) → TokenMismatch → Replaced. The fresh incarnation
    // survives; the entry is dropped.
    const RoundReport rep = gc.runRegularRound();
    store->renewWatermarkOnce();
    EXPECT_EQ(rep.replaced, 1u);
    EXPECT_TRUE(blobExists(*backend, store->layout(), blob));   // the recreated incarnation is live
    EXPECT_FALSE(currentEntryFor(*backend, store->layout(), blob).has_value());
}

/// Idempotent replay of a crashed round: a fresh Gc instance (new lease seq = new attempt) re-runs a round
/// and completes; a delete that already landed under a prior pass replays onto NotFound (Absent outcome)
/// and the round still completes. We model the crash-after-delete-before-CAS replay by manually deleting
/// the pending blob (its exact token) BEFORE the deleting pass, then asserting the pass reports the delete
/// as absent (report.absent == 1) and completes (round advances).
TEST(CasGcAckFloor, ResumeAfterCrashBetweenRetiredPutAndStateCas)
{
    auto backend = std::make_shared<InMemoryBackend>();
    auto store = openStoreForTest(backend);
    const RootNamespace ns{"00/aa@cas@"};
    const ManifestRef r = ref("srv-a:1", 1, 0xAA);
    const UInt128 blob = DB::UInt128(1);
    const BlobRef blob_id{BlobHashAlgo::CityHash128, BlobDigest::fromU128(blob)};
    writeBlobBody(*backend, store->layout(), blob);
    writeManifestRaw(*backend, store->layout(), ns, r, {blobEntryFor("a", blob)});
    publishCommittedTransition(*backend, store->layout(), ns, "tbl", std::nullopt, r);

    // A fresh Gc per round (each acquires the lease, bumping lease.seq = a fresh attempt) — the replay
    // property: no wedging, each round completes under its own fresh attempt.
    {
        Gc gc(store, kGc);
        gc.runRegularRound();
        store->renewWatermarkOnce();
    }
    dropRefTransition(*backend, store->layout(), ns, "tbl", r);

    RetiredEntry pending_entry;
    bool pending = false;
    for (int i = 0; i < 6 && !pending; ++i)
    {
        Gc gc(store, kGc);
        gc.runRegularRound();
        store->renewWatermarkOnce();
        const auto e = currentEntryFor(*backend, store->layout(), blob);
        if (e && e->delete_pending)
        {
            pending = true;
            pending_entry = *e;
        }
    }
    ASSERT_TRUE(pending);

    // Simulate a crashed deleting pass that DID land the exact-token delete but crashed before the gc/state
    // CAS. The next (fresh-attempt) pass replays the delete → the object is already gone → NotFound → the
    // pass records Absent and completes.
    ASSERT_EQ(backend->deleteExact(store->layout().blobKey(blob_id), pending_entry.token).kind,
              DeleteOutcome::Kind::Deleted);

    const uint64_t round_before = decodeGcState(backend->get(store->layout().gcStateKey())->bytes).round;
    Gc gc2(store, kGc);
    const RoundReport rep = gc2.runRegularRound();
    store->renewWatermarkOnce();
    EXPECT_EQ(rep.absent, 1u);   // the replayed delete found the object already gone
    const uint64_t round_after = decodeGcState(backend->get(store->layout().gcStateKey())->bytes).round;
    EXPECT_GT(round_after, round_before);   // the round completed (no wedge)
    EXPECT_FALSE(currentEntryFor(*backend, store->layout(), blob).has_value());
}
