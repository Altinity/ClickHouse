#include <gtest/gtest.h>

#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasGc.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasInMemoryBackend.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasStore.h>
#include "cas_test_helpers.h"

using namespace DB::Cas;
using namespace DB::Cas::tests;

namespace
{
const UInt128 kGc = UInt128(0xAB);
}

TEST(CasGcRoundDefer, PredicateTruthTable)
{
    /// threshold=1 (default): defer ONLY when zero shards changed AND no graduation due AND within bound.
    EXPECT_TRUE (shouldDeferRound(/*changed*/0, /*grad_due*/false, /*since*/0, /*threshold*/1, /*max*/8));
    EXPECT_FALSE(shouldDeferRound(1, false, 0, 1, 8));   // a shard changed => fold
    EXPECT_FALSE(shouldDeferRound(0, true,  0, 1, 8));   // graduation due => force fold
    EXPECT_FALSE(shouldDeferRound(0, false, 8, 1, 8));   // defer bound reached => force fold

    /// threshold=3 (batching): defer while accumulated changed shards < threshold, no grad, within bound.
    EXPECT_TRUE (shouldDeferRound(2, false, 0, 3, 8));
    EXPECT_FALSE(shouldDeferRound(3, false, 0, 3, 8));   // reached threshold => fold
    EXPECT_FALSE(shouldDeferRound(2, true,  0, 3, 8));   // graduation due => force fold regardless of size
    EXPECT_FALSE(shouldDeferRound(2, false, 8, 3, 8));   // bound reached => force fold
}

/// graduationDue: a delete_pending entry, and an entry whose condemn_round < min_ack, each force it true;
/// an entry with condemn_round >= min_ack and not delete_pending leaves it false.
TEST(CasGcRoundDefer, GraduationDueDetectsDuePendingAndFloorCrossing)
{
    auto backend = std::make_shared<InMemoryBackend>();
    auto store = openStoreForTest(backend);
    const Layout & layout = store->layout();

    /// Seed the CURRENT retired list for gc-shard 0 with one condemned-but-not-floor-passed entry.
    injectRetire(*backend, layout, /*round*/0, /*fence_seq*/0, /*shard*/0,
        {RetiredEntry{.kind = ObjectKind::Blob, .hash = {}, .token = {}, .size = 0,
                      .condemn_round = 2, .delete_pending = false}});

    Gc gc(store, kGc);
    const GcState state = decodeGcState(backend->get(layout.gcStateKey())->bytes);

    EXPECT_FALSE(gc.graduationDueForTest(state, /*min_ack=*/2))
        << "condemn_round (2) is not < min_ack (2); not yet due to graduate";
    EXPECT_TRUE(gc.graduationDueForTest(state, /*min_ack=*/3))
        << "condemn_round (2) < min_ack (3) => due to graduate";

    /// Re-seed the SAME entry (same retired-list object) as delete_pending: due regardless of the floor.
    const String retired_key = state.retired_refs.at(0);
    const auto current = backend->get(retired_key);
    ASSERT_TRUE(current.has_value());
    backend->putOverwrite(retired_key,
        encodeRetiredSet(RetiredSet{.entries = {RetiredEntry{.kind = ObjectKind::Blob, .hash = {}, .token = {},
                                                              .size = 0, .condemn_round = 2, .delete_pending = true}}}),
        current->token);

    EXPECT_TRUE(gc.graduationDueForTest(state, /*min_ack=*/0))
        << "delete_pending must force graduationDue true regardless of the ack floor";
}

/// changedShardCount: with the fold seal covering shard s at its current token, a quiescent pool reports
/// 0; after one publish to a ref in shard s, it reports 1.
TEST(CasGcRoundDefer, ChangedShardCountIsZeroWhenQuiescent)
{
    auto backend = std::make_shared<InMemoryBackend>();
    auto store = openStoreForTest(backend);
    const Layout & layout = store->layout();
    const RootNamespace ns{"00/aa@cas@"};
    const ManifestRef r1{.writer_epoch = 1, .build_sequence = 1, .manifest_ordinal = 0xAA};

    writeBlobBody(*backend, layout, UInt128(1));
    writeManifestRaw(*backend, layout, ns, r1, {blobEntryFor("a", UInt128(1))});
    publishCommittedTransition(*backend, layout, ns, "tbl", std::nullopt, r1);

    Gc gc(store, kGc);
    ASSERT_TRUE(gc.runRegularRound().acquired_lease);   /// fold; the round's own trim then rewrites the
                                                         /// shard (compacting the just-folded event), so
                                                         /// its sealed token is the PRE-trim snapshot.
    ASSERT_TRUE(gc.runRegularRound().acquired_lease);   /// a second, work-free round: nothing left to
                                                         /// trim, so THIS round's fold seal finally
                                                         /// captures the shard's actual current token.

    const GcState quiescent_state = decodeGcState(backend->get(layout.gcStateKey())->bytes);
    EXPECT_EQ(gc.changedShardCountForTest(quiescent_state), 0u)
        << "a quiescent shard (listed token == sealed token) must not count as changed";

    /// Publish a second ref into the SAME shard: its LISTED token now differs from what
    /// `quiescent_state`'s adopted fold seal recorded.
    const ManifestRef r2{.writer_epoch = 1, .build_sequence = 2, .manifest_ordinal = 0xBB};
    writeBlobBody(*backend, layout, UInt128(2));
    writeManifestRaw(*backend, layout, ns, r2, {blobEntryFor("b", UInt128(2))});
    publishCommittedTransition(*backend, layout, ns, "tbl2", std::nullopt, r2);

    EXPECT_EQ(gc.changedShardCountForTest(quiescent_state), 1u)
        << "one shard whose token advanced since the sealed generation must count as changed";
}
