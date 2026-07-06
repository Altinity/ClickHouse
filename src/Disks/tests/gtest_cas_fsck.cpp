#include <gtest/gtest.h>

#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasFsck.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasGc.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasInMemoryBackend.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasStore.h>
#include "cas_test_helpers.h"

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
}

/// A committed ref whose manifest body is present and whose blobs exist => clean.
TEST(CasFsck, CleanManifestPoolHasNoDangling)
{
    auto backend = std::make_shared<InMemoryBackend>();
    auto store = openStoreForTest(backend);
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
    auto store = openStoreForTest(backend);
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
    auto store = openStoreForTest(backend);
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
    auto store = openStoreForTest(backend);
    const RootNamespace ns{"00/aa@cas@"};
    /// Task 4: discovery is LIST-based; write an actual shard so the namespace is discoverable.
    /// We use a fence-only shard (no refs) by writing an empty manifest directly.
    fenceNamespace(*backend, store->layout(), ns, /*n_shards=*/1, /*round=*/0);
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
    auto store = openStoreForTest(backend);
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
    auto store = openStoreForTest(backend);
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

/// GC never ran on the pool: nothing is classifiable through the GC view — everything unreferenced
/// is AwaitingGc ("GC has not run yet"), never a false Unaccounted alarm.
TEST(CasFsck, GcNeverRanClassifiesAwaitingGc)
{
    auto backend = std::make_shared<InMemoryBackend>();
    auto store = openStoreForTest(backend);
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
    auto store = openStoreForTest(backend);
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

/// A scan whose deadline is already in the past: partial_on_deadline=false keeps the old
/// throw-on-timeout contract; partial_on_deadline=true returns the accumulated lower-bound counts
/// instead of failing empty-handed (the 2026-07-05 campaign lost 5 verdicts to this).
TEST(CasFsckPartial, DeadlineReturnsAccumulatedCountsInsteadOfThrowing)
{
    auto backend = std::make_shared<InMemoryBackend>();
    auto store = openStoreForTest(backend);
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
    auto store = openStoreForTest(backend);

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
