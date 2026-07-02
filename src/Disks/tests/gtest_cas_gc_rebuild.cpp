#include <gtest/gtest.h>

#include <optional>

#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasGc.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasGcFormats.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasServerRoot.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasStore.h>
#include "cas_test_helpers.h"

using namespace DB::Cas;
using namespace DB::Cas::tests;

namespace DB::ErrorCodes
{
extern const int CORRUPTED_DATA;
}

namespace
{
const UInt128 kGc = hexToU128("00000000000000000000000000000001");
ManifestRef ref(uint64_t seq, uint64_t inst)
{
    return ManifestRef{.writer_epoch = 1, .build_sequence = seq, .manifest_ordinal = static_cast<uint32_t>(inst)};
}
}

/// Guard (spec Part 1): a fold with NO baseline must refuse when a shard journal proves trimmed
/// history — otherwise a fresh GC folds only the surviving tails and mass-deletes live data.
TEST(CasGcBaselineGuard, FreshStateOverTrimmedJournalsFailsClosed)
{
    auto backend = std::make_shared<InMemoryBackend>();
    auto store = openStoreForTest(backend);   /// gc_trim_min_events = 0 => eager trim
    const RootNamespace ns{"00/aa@cas@"};
    const ManifestRef r = ref(1, 0xAA);
    writeBlobBody(*backend, store->layout(), DB::UInt128(1));
    writeManifestRaw(*backend, store->layout(), ns, r, {blobEntryFor("a", DB::UInt128(1))});
    publishCommittedTransition(*backend, store->layout(), ns, "tbl", std::nullopt, r);
    Gc gc(store, kGc);
    gc.runRegularRound();
    gc.runRegularRound();   /// trims the folded events (eager gate)

    /// Disaster: gc/state vanishes on a lived-in pool.
    const HeadResult st = backend->head(store->layout().gcStateKey());
    ASSERT_TRUE(st.exists);
    ASSERT_EQ(backend->deleteExact(store->layout().gcStateKey(), st.token).kind, DeleteOutcome::Kind::Deleted);

    /// A fresh GC (fresh leader id — the old lease died with the state) must REFUSE, not delete.
    Gc gc2(store, hexToU128("00000000000000000000000000000002"));
    expectThrowsCode(DB::ErrorCodes::CORRUPTED_DATA, [&] { gc2.runRegularRound(); });
    EXPECT_TRUE(backend->head(store->layout().blobKey(BlobId(u128ToHex(DB::UInt128(1))))).exists)
        << "the guard must fire BEFORE any destructive step";
}

/// A genuinely fresh pool (journals start at version 1) passes the guard — rounds run as today.
TEST(CasGcBaselineGuard, GenuinelyFreshPoolIsUnaffected)
{
    auto backend = std::make_shared<InMemoryBackend>();
    auto store = openStoreForTest(backend);
    const RootNamespace ns{"00/aa@cas@"};
    const ManifestRef r = ref(1, 0xAA);
    writeBlobBody(*backend, store->layout(), DB::UInt128(1));
    writeManifestRaw(*backend, store->layout(), ns, r, {blobEntryFor("a", DB::UInt128(1))});
    publishCommittedTransition(*backend, store->layout(), ns, "tbl", std::nullopt, r);
    Gc gc(store, kGc);
    EXPECT_NO_THROW(gc.runRegularRound());
    EXPECT_NO_THROW(gc.runRegularRound());
}

/// (б) audit: snap_generation > 0 whose adopted fold seal is ABSENT must be CORRUPTED_DATA,
/// never silently treated as an empty baseline.
TEST(CasGcBaselineGuard, AbsentAdoptedSealFailsClosed)
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

    /// Corrupt (б): delete the adopted fold seal out from under a healthy gc/state.
    const GcState st = decodeGcState(backend->get(store->layout().gcStateKey())->bytes);
    ASSERT_GT(st.snap_generation, 0u);
    const String seal_key = store->layout().foldSealKey(st.snap_generation, st.snap_attempt);
    const HeadResult sh = backend->head(seal_key);
    ASSERT_TRUE(sh.exists);
    ASSERT_EQ(backend->deleteExact(seal_key, sh.token).kind, DeleteOutcome::Kind::Deleted);

    expectThrowsCode(DB::ErrorCodes::CORRUPTED_DATA, [&] { gc.runRegularRound(); });
}
