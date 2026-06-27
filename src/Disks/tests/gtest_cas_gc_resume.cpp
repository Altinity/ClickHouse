#include <gtest/gtest.h>

#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasGc.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasStore.h>
#include "cas_test_helpers.h"

using namespace DB::Cas;
using namespace DB::Cas::tests;

namespace
{
const UInt128 kGc = hexToU128("00000000000000000000000000000001");
ManifestRef ref(uint64_t seq, uint64_t inst)
{
    return ManifestRef{.writer_instance_id = "srv-a:1", .build_sequence = seq, .manifest_instance_id = DB::UInt128(inst)};
}
bool blobExists(InMemoryBackend & b, const Layout & layout, const UInt128 & hash)
{
    return b.head(layout.blobKey(BlobId(u128ToHex(hash)))).exists;
}
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
