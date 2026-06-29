#include <gtest/gtest.h>

#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasGc.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasRootsRegistry.h>
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
    return b.head(layout.blobKey(BlobId(u128ToHex(hash)))).exists;
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

/// Fence raises fence_round on every root shard + the registry.
TEST(CasGcFence, RaisesAllShardAndRegistryFence)
{
    auto backend = std::make_shared<InMemoryBackend>();
    auto store = openStoreForTest(backend);
    const RootNamespace ns{"00/aa@cas@"};
    registerNamespaceRaw(*backend, store->layout(), ns);
    Gc gc(store, kGc);
    gc.runRegularRound();
    const auto shard0 = backend->get(store->layout().rootShardKey(ns, 0));
    ASSERT_TRUE(shard0.has_value());
    EXPECT_GE(decodeRootShard(shard0->bytes).fence_round, 1u);
    const auto reg = backend->get(store->layout().rootsRegistryKey());
    ASSERT_TRUE(reg.has_value());
    EXPECT_GE(decodeRootsRegistry(reg->bytes).fence_round, 1u);
}

/// A publish racing the fence (in-degree restored) is SPARED, not deleted (#14).
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

/// A genuinely unreferenced blob is deleted with its exact token (the single content-delete site).
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
    gc.runRegularRound();   // round 2: fold -1, retire, fence, recheck deletes blob 1
    EXPECT_FALSE(blobExists(*backend, store->layout(), DB::UInt128(1)));
    EXPECT_FALSE(backend->head(store->layout().manifestKey(ManifestId{ns, r})).exists);
}

/// CAS #2 (completion-advance) INHERITS CAS #1's (fold-adopt's) attempt WITHIN the same round: it never
/// re-mints. After a full round the adopted `snap_attempt` equals the `lease.seq` that folded that round,
/// and the completion seal is durable at the adopted `(snap_generation, snap_attempt)` pair. Across rounds
/// each `runRegularRound` re-acquires the lease (bumping `lease.seq`), so the next round mints a FRESH
/// attempt for ITS fold and the completion inherits that one — never the prior round's.
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

    gc.runRegularRound();          // round 1: fold-adopt (CAS #1) -> ... -> completion-advance (CAS #2)
    const auto after_round1 = decodeGcState(backend->get(store->layout().gcStateKey())->bytes);
    // CAS #2 inherited the fold attempt of THIS round: snap_attempt == the lease.seq that folded it.
    EXPECT_EQ(after_round1.snap_attempt, after_round1.lease.seq);
    EXPECT_GT(after_round1.snap_generation, 0u);
    // The completion seal is durable under the adopted (snap_generation, snap_attempt) pair.
    EXPECT_TRUE(backend->head(store->layout()
        .completionSealKey(after_round1.snap_generation, after_round1.snap_attempt)).exists);

    dropRefTransition(*backend, store->layout(), ns, "tbl", r);
    gc.runRegularRound();          // round 2: re-acquire (bump lease.seq) -> fold-adopt -> completion-advance
    const auto after_round2 = decodeGcState(backend->get(store->layout().gcStateKey())->bytes);
    // Round 2 minted a FRESH attempt at its fold; the completion inherited THAT one.
    EXPECT_EQ(after_round2.snap_attempt, after_round2.lease.seq);
    EXPECT_GT(after_round2.snap_attempt, after_round1.snap_attempt);   // per-round monotone attempt
    EXPECT_GT(after_round2.snap_generation, after_round1.snap_generation);
    EXPECT_TRUE(backend->head(store->layout()
        .completionSealKey(after_round2.snap_generation, after_round2.snap_attempt)).exists);
}
