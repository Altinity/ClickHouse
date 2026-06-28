#include <gtest/gtest.h>

#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasFsck.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasGc.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasGcFormats.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasStore.h>
#include "cas_test_helpers.h"

using namespace DB::Cas;
using namespace DB::Cas::tests;

namespace DB::ErrorCodes
{
extern const int ABORTED;
}

/// Unit-level GC-CONCURRENT-LEADER-LEAK regression (the original bug the attempt-scoped-generation fix
/// closes).
///
/// The historical wedge: two GC leaders fold the same generation. A DEPOSED leader writes its
/// `fold_seal(G_f)` to a FINAL `gc/gen/<G_f>/fold_seal` key just before its lease-guarded `gc/state` CAS
/// fails (lease lost mid-round). That orphaned write-once seal then poisons every future round: each
/// honest round recomputes `G_f`, hits the orphan's divergent bytes, throws "concurrent leader"
/// (`ABORTED`) forever — GC wedged, nothing reclaimed.
///
/// The fix: every per-round `gc/gen` artifact is ATTEMPT-scoped (keyed by the folding leader's
/// `lease.seq`). A deposed leader writes its fold seal under its OWN attempt `a1`, which the failed
/// `gc/state` CAS never adopts — so it is pure unadopted debris, invisible to every reader resolving
/// only the adopted `(snap_generation, snap_attempt)`. The next honest round renews the lease (a fresh
/// `lease.seq`), folds under a DIFFERENT attempt, never collides, and drains.
///
/// This test proves it at the unit level: deny exactly the fold-adopt `gc/state` CAS #1 of a round that
/// processes a drop (leaving the deposed fold seal under `a1`), then run an honest GC to a fixpoint and
/// assert it drains the now-unreachable blob to zero without throwing `CORRUPTED_DATA`/`ABORTED`-wedge.
/// On a PRE-FIX tree (final-key seal) step 2's first fold would adopt-collide with the deposed seal and
/// wedge forever; here it does not.

namespace
{

const UInt128 kGcA = hexToU128("00000000000000000000000000000001");

ManifestRef ref(const String & writer, uint64_t seq, uint64_t inst)
{
    return ManifestRef{.writer_instance_id = writer, .build_sequence = seq, .manifest_instance_id = DB::UInt128(inst)};
}

/// Whether a blob's body object is present in the backend (HEADs the object key directly).
bool blobExists(InMemoryBackend & b, const Layout & layout, const UInt128 & hash)
{
    return b.head(layout.blobKey(BlobId(u128ToHex(hash)))).exists;
}

/// Drive regular GC to a fixpoint. A round that only retired/fenced without deleting has not reached a
/// fixpoint: the delete-cascade for a just-retired blob lands in a SUBSEQUENT round, so the loop stays
/// alive while ANY work counter is nonzero. (Copied from gtest_cas_gc_leak.cpp — per-file helper.)
size_t runGcToFixpoint(Gc & gc, size_t max_rounds = 64)
{
    size_t rounds = 0;
    for (; rounds < max_rounds; ++rounds)
    {
        const RoundReport rep = gc.runRegularRound();
        if (!rep.acquired_lease)
            continue;
        if (rep.candidates == 0 && rep.deleted == 0 && rep.absent == 0
            && rep.replaced == 0 && rep.spared == 0)
            break;
    }
    return rounds;
}

/// A backend that throws ONCE on the FOLD-ADOPT `gc/state` CAS #1 — the casPut that advances
/// snap_generation while NO retired set is durable yet (fold runs before retire writes any retired key,
/// so the absence of a `/retired/` key uniquely distinguishes CAS #1 from the recheck CAS #2, mirroring
/// gtest_cas_gc_resume.cpp's InterruptRecheckBackend which keys on the PRESENCE of a retired set). This
/// isolates the fold-adopt CAS without fragile ordinal counting, so it stays correct across refactors.
class InterruptFoldAdoptBackend : public InMemoryBackend
{
public:
    explicit InterruptFoldAdoptBackend(String gc_state_key_) : gc_state_key(std::move(gc_state_key_)) {}

    CasResult casPut(const String & key, const String & bytes, const std::optional<Token> & expected,
                     const ObjectMeta & meta = {}) override
    {
        if (arm_interrupt && key == gc_state_key)
        {
            const auto stored = get(key);
            const uint64_t stored_gen = stored ? decodeGcState(stored->bytes).snap_generation : 0;
            const uint64_t next_gen = decodeGcState(bytes).snap_generation;
            /// Fold-adopt CAS #1: advances snap_generation with no retired set yet. (The recheck CAS #2
            /// also advances snap_generation, but only AFTER retire has written durable retired sets.)
            if (next_gen > stored_gen && !hasDurableRetiredSet())
            {
                arm_interrupt = false;   /// one-shot: only depose the first fold-adopt CAS
                throw DB::Exception(DB::ErrorCodes::ABORTED,
                    "test-injected: fold-adopt CAS #1 denied (leader deposed mid-fold; lease lost)");
            }
        }
        return InMemoryBackend::casPut(key, bytes, expected, meta);
    }

    bool arm_interrupt = false;

private:
    bool hasDurableRetiredSet()
    {
        String cursor;
        for (;;)
        {
            const ListPage page = list("", cursor, 1024);
            for (const auto & item : page.keys)
                if (item.key.find("/retired/") != String::npos)
                    return true;
            if (page.next_cursor.empty())
                return false;
            cursor = page.next_cursor;
        }
    }

    String gc_state_key;
};

}

/// A leader whose fold-adopt CAS #1 is denied (lease lost mid-fold) leaves its fold seal ONLY under its
/// own attempt `a1`; it never occupies the adopted attempt, so a subsequent honest round is not wedged
/// and drains the now-unreachable blob to zero.
TEST(CasGcAttempt, DeposedFoldAttemptDoesNotWedge)
{
    auto backend = std::make_shared<InterruptFoldAdoptBackend>(/*gc_state_key*/ "p/gc/state");
    auto store = openStoreForTest(backend);
    ASSERT_EQ(store->layout().gcStateKey(), "p/gc/state");   // guard the injected key against layout drift

    const RootNamespace ns{"00/aa@cas@"};
    const ManifestRef r = ref("srv-a:1", 1, 0xAA);
    writeBlobBody(*backend, store->layout(), DB::UInt128(1));
    writeManifestRaw(*backend, store->layout(), ns, r, {blobEntryFor("a", DB::UInt128(1))});
    publishCommittedTransition(*backend, store->layout(), ns, "tbl", std::nullopt, r);

    Gc gc(store, kGcA);

    // Round 1 (honest): fold the +1 so the blob is pinned in the in-degree generation, and adopt the
    // first (snap_generation, snap_attempt).
    gc.runRegularRound();
    EXPECT_EQ(inDegreeOf(*backend, store->layout(), DB::UInt128(1)), 1) << "blob pinned by the committed ref";
    const auto after_fold = decodeGcState(backend->get(store->layout().gcStateKey())->bytes);
    ASSERT_EQ(after_fold.snap_attempt, after_fold.lease.seq);
    ASSERT_GT(after_fold.snap_generation, 0u);

    // Drop the only ref and advance the watermark floor so the now-orphaned blob is not spared in-flight.
    dropRefTransition(*backend, store->layout(), ns, "tbl", r);
    store->renewWatermarkOnce();

    // Round 2 (DEPOSED): the round folds the -1 and writes its fold seal under its own attempt `a1`, then
    // its fold-adopt CAS #1 is DENIED (lease lost mid-fold). The round must throw and must NOT advance the
    // adopted (snap_generation, snap_attempt).
    backend->arm_interrupt = true;
    EXPECT_ANY_THROW(gc.runRegularRound());   // ABORTED: fold-adopt CAS #1 denied
    backend->arm_interrupt = false;

    const auto after_deposed = decodeGcState(backend->get(store->layout().gcStateKey())->bytes);
    EXPECT_EQ(after_deposed.snap_generation, after_fold.snap_generation)
        << "the denied fold-adopt CAS must NOT advance the adopted generation";
    EXPECT_EQ(after_deposed.snap_attempt, after_fold.snap_attempt)
        << "the denied fold-adopt CAS must NOT advance the adopted attempt";

    // The deposed leader DID write its fold seal under its OWN attempt `a1` (= the lease.seq it renewed
    // for round 2, which is strictly past the still-adopted attempt) at its fold generation `G_f`
    // (= snap_generation + 1; fold mints the next generation, completion adopts it later). That orphan is
    // pure debris: it is under an attempt that gc/state never adopted, so no reader resolving
    // (snap_generation, snap_attempt) can see it. On a PRE-FIX tree this seal would instead sit at the
    // FINAL `gc/gen/<G_f>/fold_seal` key and wedge every future round's fold at the same G_f.
    const uint64_t a1 = after_fold.lease.seq + 1;       // round 2 renewed the lease => seq bumped once
    const uint64_t g_f = after_fold.snap_generation + 1;  // the generation the deposed fold minted
    EXPECT_NE(a1, after_deposed.snap_attempt) << "the deposed attempt must differ from the adopted one";
    EXPECT_TRUE(backend->head(store->layout().foldSealKey(g_f, a1)).exists)
        << "the deposed leader's fold seal is durable under its own (unadopted) attempt a1";
    EXPECT_FALSE(backend->head(store->layout().foldSealKey(g_f, after_deposed.snap_attempt)).exists)
        << "no fold seal exists under the still-adopted attempt at the deposed fold generation (orphan is invisible)";

    // An HONEST GC to a fixpoint (CAS now allowed). The KEY property: with attempt-scoping this SUCCEEDS —
    // the next honest fold mints a FRESH attempt (a different lease.seq), never collides with the deposed
    // seal under a1, and drains the unreachable blob. On a pre-fix (final-key) tree, the next fold would
    // adopt-collide with the deposed final-key seal's divergent bytes and throw forever (GC wedged).
    EXPECT_NO_THROW(runGcToFixpoint(gc));

    EXPECT_FALSE(blobExists(*backend, store->layout(), DB::UInt128(1)))
        << "the dropped blob must be reclaimed (GC drained past the deposed attempt)";
    EXPECT_EQ(inDegreeOf(*backend, store->layout(), DB::UInt128(1)), 0) << "no stranded positive in-degree";
    EXPECT_EQ(runFsck(*store, /*detail=*/false).unreachable, 0u)
        << "INV-NO-LEAK: the deposed fold attempt did not wedge GC; the pool fully drained";

    // GC advanced past the deposed attempt: the adopted (snap_generation, snap_attempt) moved on, and the
    // adopted attempt is a fresh one (never the deposed a1).
    const auto after_drain = decodeGcState(backend->get(store->layout().gcStateKey())->bytes);
    EXPECT_GT(after_drain.snap_generation, after_fold.snap_generation) << "completion advanced the generation";
    EXPECT_NE(after_drain.snap_attempt, a1) << "the drained round never adopted the deposed attempt a1";
}
