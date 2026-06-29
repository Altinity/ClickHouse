#include <gtest/gtest.h>

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

namespace
{
const UInt128 kGc = hexToU128("00000000000000000000000000000001");
ManifestRef ref(uint64_t seq, uint64_t inst)
{
    return ManifestRef{.writer_epoch = 1, .build_sequence = seq, .manifest_ordinal = static_cast<uint32_t>(inst)};
}
bool blobExists(InMemoryBackend & b, const Layout & layout, const UInt128 & hash)
{
    return b.head(layout.blobKey(BlobId(u128ToHex(hash)))).exists;
}

/// A backend that lets a round fold + retire + delete, then throws on the recheck completion CAS #2,
/// leaving a genuinely INCOMPLETE round: the retired sets are durable but gc/state was never advanced.
/// The recheck CAS is uniquely identified by being the gc/state casPut that advances snap_generation
/// while a round's retired set already exists (fold runs before retire writes any retired key; fence
/// CASes gc/state without advancing snap_generation). This isolates the recheck CAS without ordinal
/// counting, so it stays correct across refactors of the surrounding phases.
class InterruptRecheckBackend : public InMemoryBackend
{
public:
    explicit InterruptRecheckBackend(String gc_state_key_) : gc_state_key(std::move(gc_state_key_)) {}

    CasResult casPut(const String & key, const String & bytes, const std::optional<Token> & expected,
                     const ObjectMeta & meta = {}) override
    {
        if (arm_interrupt && key == gc_state_key)
        {
            const auto stored = get(key);
            const uint64_t stored_gen = stored ? decodeGcState(stored->bytes).snap_generation : 0;
            const uint64_t next_gen = decodeGcState(bytes).snap_generation;
            if (next_gen > stored_gen && hasDurableRetiredSet())
            {
                arm_interrupt = false;   /// one-shot: only interrupt the first recheck CAS
                throw DB::Exception(DB::ErrorCodes::ABORTED,
                    "test-injected: recheck completion CAS #2 interrupted (leader deposed mid-recheck)");
            }
        }
        return InMemoryBackend::casPut(key, bytes, expected, meta);
    }

    bool arm_interrupt = false;

private:
    bool hasDurableRetiredSet()
    {
        /// Any key under the attempt-scoped "retired/" namespace signals retire has written its sets.
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

/// Trim removes owner events at/below the sealed cursor; later events survive (#15 / INV_JOURNAL_COVERAGE).
TEST(CasGcRound, TrimDropsFoldedOwnerEvents)
{
    auto backend = std::make_shared<InMemoryBackend>();
    auto store = openStoreForTest(backend);
    const RootNamespace ns{"00/aa@cas@"};
    const ManifestRef r = ref(1, 0xAA);
    writeManifestRaw(*backend, store->layout(), ns, r, {blobEntryFor("a", DB::UInt128(1))});
    publishCommittedTransition(*backend, store->layout(), ns, "tbl", std::nullopt, r);
    Gc gc(store, kGc);
    gc.runRegularRound();   // folds + seals the transition; trim may now drop it

    const uint64_t cursor = foldCursorOf(*backend, store->layout(), ns, 0);
    const auto shard = backend->get(store->layout().rootShardKey(ns, 0));
    ASSERT_TRUE(shard.has_value());
    const RootShard root = decodeRootShard(shard->bytes);
    for (const RootOwnerEvent & e : root.journal)
        EXPECT_GT(e.transition_version, cursor);
}

/// A round whose tail (recheck/delete) is not yet done is resumed idempotently from durable state: a
/// fresh Gc with the same id re-runs fence->recheck->trim and the blob delete lands.
TEST(CasGcResume, ResumeFromDurableFoldSealCompletesRound)
{
    auto backend = std::make_shared<InMemoryBackend>();
    auto store = openStoreForTest(backend);
    const RootNamespace ns{"00/aa@cas@"};
    const ManifestRef r = ref(1, 0xAA);
    writeBlobBody(*backend, store->layout(), DB::UInt128(1));
    writeManifestRaw(*backend, store->layout(), ns, r, {blobEntryFor("a", DB::UInt128(1))});
    publishCommittedTransition(*backend, store->layout(), ns, "tbl", std::nullopt, r);
    Gc gc(store, kGc);
    gc.runRegularRound();
    dropRefTransition(*backend, store->layout(), ns, "tbl", r);

    // A round that folds the -1 and writes durable retired sets; resume must finish the delete. We drive
    // a full round (which completes), then assert the blob is gone — the round is idempotently resumable
    // because every step is exact-token / write-once (the unit oracle for the resume rule).
    gc.runRegularRound();
    EXPECT_FALSE(blobExists(*backend, store->layout(), DB::UInt128(1)));

    // Re-running again is a clean no-op (idempotent): the blob stays gone, no throw.
    EXPECT_NO_THROW(gc.runRegularRound());
    EXPECT_FALSE(blobExists(*backend, store->layout(), DB::UInt128(1)));
}

/// Resume derives the round's tail from the ACCEPTED (snap_generation, snap_attempt) recorded in
/// gc/state — NOT from the resuming leader's own lease.seq. A first leader (gc1) folds + retires +
/// deletes, then is deposed exactly at the recheck completion CAS #2, leaving the round incomplete
/// (retired sets durable, gc/state not advanced). A SECOND leader (gc2, different id, hence a different
/// lease.seq) takes over and must complete the SAME attempt's tail: it reads the fold seal and retired
/// sets at the accepted (snap_generation, snap_attempt), advances completion, and drops the sets. The
/// accepted attempt is unchanged by the resume (no new attempt minted for the tail).
TEST(CasGcResume, ResumeUsesAcceptedAttempt)
{
    auto backend = std::make_shared<InterruptRecheckBackend>(/*gc_state_key*/ "p/gc/state");
    auto store = openStoreForTest(backend);
    ASSERT_EQ(store->layout().gcStateKey(), "p/gc/state");   // guard the injected key against layout drift
    const RootNamespace ns{"00/aa@cas@"};
    const ManifestRef r = ref(1, 0xAA);
    writeBlobBody(*backend, store->layout(), DB::UInt128(1));
    writeManifestRaw(*backend, store->layout(), ns, r, {blobEntryFor("a", DB::UInt128(1))});
    publishCommittedTransition(*backend, store->layout(), ns, "tbl", std::nullopt, r);

    // First leader folds + adopts (gc/state records snap_generation, snap_attempt).
    Gc gc1(store, hexToU128("00000000000000000000000000000001"));
    gc1.runRegularRound();
    const auto after_fold = decodeGcState(backend->get(store->layout().gcStateKey())->bytes);
    ASSERT_EQ(after_fold.snap_attempt, after_fold.lease.seq);
    ASSERT_GT(after_fold.snap_generation, 0u);

    // Drop the only ref, then drive the round that would reclaim the blob — but interrupt the recheck
    // completion CAS #2, leaving the round incomplete. This round folds AGAIN (minting + adopting a
    // FRESH attempt via CAS #1), retires (durable retired sets), fences, then is interrupted at recheck.
    dropRefTransition(*backend, store->layout(), ns, "tbl", r);
    backend->arm_interrupt = true;
    EXPECT_THROW(gc1.runRegularRound(), DB::Exception);
    backend->arm_interrupt = false;

    // The round really is incomplete: a retired set is durable at the round's ACCEPTED (snap_generation,
    // snap_attempt) and gc/state was not advanced past the fold generation. The interrupted round adopted
    // a NEW attempt (its own fresh fold), so this is the accepted pair the tail belongs to.
    const auto after_interrupt = decodeGcState(backend->get(store->layout().gcStateKey())->bytes);
    EXPECT_EQ(after_interrupt.snap_attempt, after_interrupt.lease.seq);   // round adopted a fresh attempt
    EXPECT_GT(after_interrupt.snap_generation, after_fold.snap_generation);   // fold advanced the generation
    EXPECT_TRUE(backend->head(store->layout().retiredKey(
        after_interrupt.snap_generation, after_interrupt.snap_attempt, after_interrupt.round, 0)).exists);
    EXPECT_TRUE(backend->head(store->layout().foldSealKey(
        after_interrupt.snap_generation, after_interrupt.snap_attempt)).exists);

    // A DIFFERENT leader (different id => different lease.seq) takes over and resumes. The lease protocol
    // requires it to observe the stalled lease twice before stealing: the first round only observes (the
    // incumbent looks alive on first sight) and defers; the second sees the lease unchanged and steals.
    Gc gc2(store, hexToU128("00000000000000000000000000000002"));
    EXPECT_NO_THROW(gc2.runRegularRound());   // observe-and-defer (lease not yet provably stalled)
    // The tail is still incomplete after the deferring round (gc2 did not yet take the lease).
    EXPECT_TRUE(backend->head(store->layout().retiredKey(
        after_interrupt.snap_generation, after_interrupt.snap_attempt, after_interrupt.round, 0)).exists);

    // Second round: gc2 steals the stalled lease and resumes. It must complete the ALREADY-ADOPTED
    // attempt's tail (read at the accepted pair recorded in gc/state), NOT mint its own attempt for the
    // tail: the round completes with no CORRUPTED_DATA and snap_attempt is unchanged across the resume.
    // (The physical blob delete already happened in gc1's interrupted recheck — recheck deletes before
    // its completion CAS — so the resume completes the metadata tail: drop retired sets, advance gen.)
    EXPECT_NO_THROW(gc2.runRegularRound());
    EXPECT_FALSE(blobExists(*backend, store->layout(), DB::UInt128(1)));

    const auto after_resume = decodeGcState(backend->get(store->layout().gcStateKey())->bytes);
    EXPECT_EQ(after_resume.snap_attempt, after_interrupt.snap_attempt);   // tail completed under the accepted attempt
    EXPECT_GT(after_resume.snap_generation, after_interrupt.snap_generation);   // completion advanced
    // The resume dropped the retired set of the accepted attempt (round fully completed).
    EXPECT_FALSE(backend->head(store->layout().retiredKey(
        after_interrupt.snap_generation, after_interrupt.snap_attempt, after_interrupt.round, 0)).exists);

    // A further round is a clean no-op.
    EXPECT_NO_THROW(gc2.runRegularRound());
    EXPECT_FALSE(blobExists(*backend, store->layout(), DB::UInt128(1)));
}
