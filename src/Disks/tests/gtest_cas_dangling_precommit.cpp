#include <gtest/gtest.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasGenerationSeal.h>

using namespace DB::Cas;

/// The GC-reclaim tests that used to live here (`AbandonedPrecommitOrphansManifestUntilFix`,
/// `ReclaimIsIdempotentAndSelfTerminating`, `SkipPreservedForLivePrecommitAndForNoPrecommit`,
/// `DoubleRemovalOfReclaimedPrecommitIsIdempotent`) were removed with the snapshot+log ref model.
/// They asserted that GC reclaims an abandoned precommit once the mount watermark proves it dead, and
/// that the token-diff Skip optimization self-terminates. Per spec §Responsibility Boundary, reclaiming
/// an abandoned precommit is now the WRITER's job (it appends the exact `owner_transition` removal), and
/// the token-diff Skip machinery (`computeDiscoverDecisions`/`discoverDecisionsForTest`) no longer exists
/// -- the "did it change" signal is simply logs above the durable cursor. There is no GC-side reclaim to
/// assert, so these tests are obsolete rather than adaptable.
///
/// The live-precommit watermark fields (`has_live_precommit`/`min_live_precommit_*`) that fed that
/// removed reclaim were deleted from `ShardCoverage` with it (T13). The still-meaningful fold-seal
/// assertion is the round-trip of `last_folded_ref_id` -- the per-table durable ref cursor that replaced
/// them in the same struct under the snapshot+log ref model.
TEST(CasDanglingPrecommit, ShardCoverageRoundTripsLastFoldedRefId)
{
    CasFoldSeal seal;
    seal.generation = 3;
    seal.parent_generation = 2;
    ShardCoverage cov;
    cov.classification = 1;
    cov.folded_cursor = 7;
    cov.last_folded_ref_id = RefTxnId{4, 11};
    seal.per_ns_shard["srv/tbl@cas@/0"] = cov;

    const CasFoldSeal back = decodeFoldSeal(encodeFoldSeal(seal));
    const ShardCoverage & r = back.per_ns_shard.at("srv/tbl@cas@/0");
    EXPECT_EQ(r.last_folded_ref_id, (RefTxnId{4, 11}));

    /// Default (nothing folded) round-trips as {0,0}.
    CasFoldSeal empty_seal;
    empty_seal.per_ns_shard["srv/tbl@cas@/1"] = ShardCoverage{};
    const CasFoldSeal e_back = decodeFoldSeal(encodeFoldSeal(empty_seal));
    EXPECT_EQ(e_back.per_ns_shard.at("srv/tbl@cas@/1").last_folded_ref_id, (RefTxnId{}));
}
