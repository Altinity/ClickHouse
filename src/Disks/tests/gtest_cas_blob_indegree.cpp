#include <gtest/gtest.h>

#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasBlobInDegree.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasGcFormats.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasInMemoryBackend.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasLayout.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasRunFile.h>
#include <Disks/tests/cas_test_helpers.h>
#include <Common/Exception.h>

using namespace DB::Cas;

namespace
{
UInt128 b(uint64_t n) { return UInt128(n); }
UInt128 s(uint64_t n) { return UInt128(n); }   // source-edge id
}

TEST(CasBlobInDegree, FoldStartsFromEmptyPriorGeneration)
{
    InMemoryBackend backend;
    Layout layout{"pool"};

    /// Generation 1 from empty prior: two distinct edges on b1 and one on b2.
    /// Edge (b1,s1), (b1,s2), (b2,s1) => indeg(b1)=2, indeg(b2)=1.
    std::vector<BlobDelta> deltas{
        {b(1), s(1), false},
        {b(1), s(2), false},
        {b(2), s(1), false},
    };
    std::vector<RunRef> runs;
    foldDeltasIntoGeneration(backend, layout, /*prior*/0, /*prior_attempt*/0, /*new*/1, /*attempt*/0, /*shard*/0, deltas, runs);
    ASSERT_FALSE(runs.empty());

    const auto zero = zeroInDegree(backend, layout, /*gen*/1, /*attempt*/0, /*shard*/0);
    EXPECT_TRUE(zero.empty());   /// nothing at zero yet
}

TEST(CasBlobInDegree, PlusMinusCancelToZeroDetectsCandidate)
{
    InMemoryBackend backend;
    Layout layout{"pool"};

    /// Gen 1: activate edge (b1,s1) and (b2,s1).
    std::vector<RunRef> runs1;
    foldDeltasIntoGeneration(backend, layout, 0, /*prior_attempt*/0, 1, /*attempt*/0, 0,
        {{b(1), s(1), false}, {b(2), s(1), false}}, runs1);

    /// Generation 2 merges prior gen-1 run with removal of (b1,s1): indeg(b1)=0, indeg(b2)=1.
    std::vector<RunRef> runs2;
    foldDeltasIntoGeneration(backend, layout, /*prior*/1, /*prior_attempt*/0, /*new*/2, /*attempt*/0, 0,
        {{b(1), s(1), true}}, runs2);

    const auto zero = zeroInDegree(backend, layout, 2, /*attempt*/0, 0);
    ASSERT_EQ(zero.size(), 1u);
    EXPECT_EQ(zero[0].hash, b(1));
}

TEST(CasBlobInDegree, RunsAreByteDeterministic)
{
    InMemoryBackend a;
    InMemoryBackend b2;
    Layout layout{"pool"};
    std::vector<RunRef> ra;
    std::vector<RunRef> rb;
    /// Same deltas in a DIFFERENT input order must produce the same sealed run bytes (sorted by key).
    foldDeltasIntoGeneration(a,  layout, 0, /*prior_attempt*/0, 1, /*attempt*/0, 0,
        {{b(3), s(1), false}, {b(1), s(1), false}, {b(2), s(1), false}}, ra);
    foldDeltasIntoGeneration(b2, layout, 0, /*prior_attempt*/0, 1, /*attempt*/0, 0,
        {{b(1), s(1), false}, {b(2), s(1), false}, {b(3), s(1), false}}, rb);
    const auto ga = a.get(layout.blobTargetRunKey(1, /*attempt*/0, 0, 0));
    const auto gb = b2.get(layout.blobTargetRunKey(1, /*attempt*/0, 0, 0));
    ASSERT_TRUE(ga.has_value());
    ASSERT_TRUE(gb.has_value());
    EXPECT_EQ(ga->bytes, gb->bytes);
    ASSERT_EQ(ra.size(), 1u);
    ASSERT_EQ(rb.size(), 1u);
    EXPECT_EQ(ra[0].checksum, rb[0].checksum);
}

TEST(CasBlobInDegree, SameEdgeActivatedTwiceCountsOnce)
{
    /// Idempotency: activating the same (blob_hash, source_id) twice must not double-count.
    /// The source-edge set is a SET, not a counter — re-adding the same edge is a no-op.
    /// indeg(b1) must be 1 after both activations, not 2.
    InMemoryBackend backend;
    Layout layout{"pool"};
    std::vector<BlobDelta> deltas{
        {b(1), s(1), false},   // activate (b1,s1)
        {b(1), s(1), false},   // same edge again — must deduplicate
    };
    std::vector<RunRef> runs;
    foldDeltasIntoGeneration(backend, layout, 0, /*prior_attempt*/0, 1, /*attempt*/0, 0, deltas, runs);
    ASSERT_FALSE(runs.empty());

    const int64_t deg = inDegreeInGeneration(backend, layout, 1, /*attempt*/0, 0, b(1));
    EXPECT_EQ(deg, 1);   /// deduplicated, not 2

    const auto zero = zeroInDegree(backend, layout, 1, /*attempt*/0, 0);
    EXPECT_TRUE(zero.empty());   /// b1 still has an active edge
}

TEST(CasBlobInDegree, FoldDeltaByteEqualReplayAdopts)
{
    InMemoryBackend backend;
    Layout layout{"pool"};
    std::vector<BlobDelta> deltas{{b(1), s(1), false}};
    std::vector<RunRef> runs1;
    std::vector<RunRef> runs2;
    foldDeltasIntoGeneration(backend, layout, 0, /*prior_attempt*/7, 1, /*attempt*/7, /*shard*/0, deltas, runs1);
    /// Same inputs, same attempt => byte-identical run already present => adopt, no throw.
    EXPECT_NO_THROW(foldDeltasIntoGeneration(backend, layout, 0, /*prior_attempt*/7, 1, /*attempt*/7, /*shard*/0, deltas, runs2));
    EXPECT_EQ(runs1, runs2);
}

TEST(CasBlobInDegree, FoldDeltaDivergentBytesThrowsCorrupted)
{
    InMemoryBackend backend;
    Layout layout{"pool"};
    /// Pre-occupy the run key (attempt 7) with junk, then fold => divergent => CORRUPTED_DATA.
    backend.putIfAbsent(layout.blobTargetRunKey(1, /*attempt*/7, /*shard*/0, /*seq*/0), "not-a-valid-run");
    std::vector<BlobDelta> deltas{{b(1), s(1), false}};
    std::vector<RunRef> runs;
    EXPECT_THROW(foldDeltasIntoGeneration(backend, layout, 0, /*prior_attempt*/7, 1, /*attempt*/7, /*shard*/0, deltas, runs),
                 DB::Exception);
}

/// ==== three-cursor merge (spec 2026-07-02-cas-gc-ack-floor-fence-redesign) ====

namespace
{

RetiredEntry entry(uint64_t hash, uint64_t condemn_round, const String & tok = "t")
{
    RetiredEntry e;
    e.kind = DB::Cas::ObjectKind::Blob;
    e.hash = b(hash);
    e.token = Token{.value = tok, .type = TokenType::Emulated};
    e.size = 1;
    e.condemn_round = condemn_round;
    return e;
}

/// head_blob stub: present with a fixed token/size.
std::function<std::optional<HeadResult>(const UInt128 &)> headPresent(const String & tok, uint64_t size)
{
    return [tok, size](const UInt128 &) -> std::optional<HeadResult>
    {
        HeadResult hr;
        hr.exists = true;
        hr.size = size;
        hr.token = Token{.value = tok, .type = TokenType::Emulated};
        return hr;
    };
}

}

TEST(CasThreeCursorMerge, FloorBoundary)
{
    InMemoryBackend backend;
    Layout layout{"pool"};

    /// Gen 1 holds one unrelated edge (b9) so the prior run exists; A=b1 and B=b2 have no edges at
    /// all (in-degree 0 by definition). A was condemned at round 2, B at round 3; min_ack = 3:
    /// strictly-below graduates, at-the-floor stays.
    std::vector<RunRef> runs1;
    foldDeltasIntoGeneration(backend, layout, 0, 0, 1, 0, 0, {{b(9), s(1), false}}, runs1);

    std::vector<RunRef> runs2;
    RetiredMergeResult rmr;
    foldDeltasIntoGeneration(backend, layout, 1, 0, 2, 0, 0, {}, runs2,
        {entry(1, 2), entry(2, 3)}, /*min_ack*/3, /*condemn_round*/4, {}, &rmr);

    /// Two-phase graduation: the floor-passed entry is REPUBLISHED pending (still in the list);
    /// its physical delete belongs to the NEXT pass.
    ASSERT_EQ(rmr.graduated.size(), 1u);
    EXPECT_EQ(rmr.graduated[0].hash, b(1));
    EXPECT_TRUE(rmr.graduated[0].delete_pending);
    ASSERT_EQ(rmr.still_retired.size(), 2u);
    EXPECT_EQ(rmr.still_retired[0].hash, b(1));
    EXPECT_TRUE(rmr.still_retired[0].delete_pending);
    EXPECT_EQ(rmr.still_retired[1].hash, b(2));
    EXPECT_FALSE(rmr.still_retired[1].delete_pending);
    EXPECT_EQ(rmr.still_retired[1].condemn_round, 3u);   /// carried unchanged, not re-stamped
    EXPECT_TRUE(rmr.spared.empty());
    EXPECT_TRUE(rmr.redelete.empty());
}

TEST(CasThreeCursorMerge, PendingRedeletesAndDrops)
{
    InMemoryBackend backend;
    Layout layout{"pool"};

    /// An entry the PRIOR pass published as delete_pending: this pass hands it to `redelete`
    /// (executed pre-CAS by the caller) and drops it from the list output.
    RetiredEntry pending = entry(1, 1);
    pending.delete_pending = true;

    std::vector<RunRef> runs;
    RetiredMergeResult rmr;
    foldDeltasIntoGeneration(backend, layout, 0, 0, 1, 0, 0, {}, runs,
        {pending}, /*min_ack*/9, /*condemn_round*/9, {}, &rmr);

    ASSERT_EQ(rmr.redelete.size(), 1u);
    EXPECT_EQ(rmr.redelete[0].hash, b(1));
    EXPECT_TRUE(rmr.still_retired.empty());
    EXPECT_TRUE(rmr.graduated.empty());
    EXPECT_TRUE(rmr.spared.empty());
}

TEST(CasThreeCursorMerge, RecoverySpares)
{
    InMemoryBackend backend;
    Layout layout{"pool"};

    /// A (=b1) is retired at round 1 and the floor has long passed (min_ack = 5) — but this pass's
    /// delta adds an edge to it: recovery WINS over graduation, the entry is dropped as spared.
    std::vector<RunRef> runs;
    RetiredMergeResult rmr;
    foldDeltasIntoGeneration(backend, layout, 0, 0, 1, 0, 0, {{b(1), s(1), false}}, runs,
        {entry(1, 1)}, /*min_ack*/5, /*condemn_round*/6, {}, &rmr);

    ASSERT_EQ(rmr.spared.size(), 1u);
    EXPECT_EQ(rmr.spared[0].hash, b(1));
    EXPECT_TRUE(rmr.graduated.empty());
    EXPECT_TRUE(rmr.still_retired.empty());
}

TEST(CasThreeCursorMerge, NewCandidateCondemned)
{
    InMemoryBackend backend;
    Layout layout{"pool"};

    /// Gen 1: C (=b3) has one edge. Gen 2 removes it => transition to zero, not retired =>
    /// condemned with the head-captured token at THIS pass's condemn_round.
    std::vector<RunRef> runs1;
    foldDeltasIntoGeneration(backend, layout, 0, 0, 1, 0, 0, {{b(3), s(1), false}}, runs1);

    std::vector<RunRef> runs2;
    RetiredMergeResult rmr;
    foldDeltasIntoGeneration(backend, layout, 1, 0, 2, 0, 0, {{b(3), s(1), true}}, runs2,
        {}, /*min_ack*/0, /*condemn_round*/7, headPresent("t9", 42), &rmr);

    ASSERT_EQ(rmr.still_retired.size(), 1u);
    EXPECT_EQ(rmr.still_retired[0].hash, b(3));
    EXPECT_EQ(rmr.still_retired[0].token.value, "t9");
    EXPECT_EQ(rmr.still_retired[0].size, 42u);
    EXPECT_EQ(rmr.still_retired[0].condemn_round, 7u);
    EXPECT_TRUE(rmr.graduated.empty());
    EXPECT_TRUE(rmr.spared.empty());
}

TEST(CasThreeCursorMerge, AbsentBlobNotCondemned)
{
    InMemoryBackend backend;
    Layout layout{"pool"};

    /// Same transition-to-zero as above, but the blob object is already gone at condemn time:
    /// nothing to delete later, so no entry is minted.
    std::vector<RunRef> runs1;
    foldDeltasIntoGeneration(backend, layout, 0, 0, 1, 0, 0, {{b(3), s(1), false}}, runs1);

    std::vector<RunRef> runs2;
    RetiredMergeResult rmr;
    foldDeltasIntoGeneration(backend, layout, 1, 0, 2, 0, 0, {{b(3), s(1), true}}, runs2,
        {}, /*min_ack*/0, /*condemn_round*/7,
        [](const UInt128 &) -> std::optional<HeadResult> { return std::nullopt; }, &rmr);

    EXPECT_TRUE(rmr.still_retired.empty());
    EXPECT_TRUE(rmr.graduated.empty());
    EXPECT_TRUE(rmr.spared.empty());
}

TEST(CasThreeCursorMerge, SnapshotBytesUnchanged)
{
    /// The retired cursor must not perturb the snapshot run bytes: identical edge inputs produce
    /// byte-identical runs whether or not the retired machinery is engaged.
    InMemoryBackend plain;
    InMemoryBackend engaged;
    Layout layout{"pool"};

    std::vector<RunRef> r1;
    foldDeltasIntoGeneration(plain, layout, 0, 0, 1, 0, 0,
        {{b(1), s(1), false}, {b(2), s(1), false}, {b(2), s(2), true}}, r1);

    std::vector<RunRef> r2;
    RetiredMergeResult rmr;
    foldDeltasIntoGeneration(engaged, layout, 0, 0, 1, 0, 0,
        {{b(1), s(1), false}, {b(2), s(1), false}, {b(2), s(2), true}}, r2,
        {entry(1, 1), entry(5, 2)}, /*min_ack*/9, /*condemn_round*/3, headPresent("t", 1), &rmr);

    const auto ga = plain.get(layout.blobTargetRunKey(1, 0, 0, 0));
    const auto gb = engaged.get(layout.blobTargetRunKey(1, 0, 0, 0));
    ASSERT_TRUE(ga.has_value());
    ASSERT_TRUE(gb.has_value());
    EXPECT_EQ(ga->bytes, gb->bytes);
}

/// A prior run spanning several blocks folds correctly with the streaming prior cursor AND the backend
/// sees only block-bounded ranged/stream requests for it — never a whole-object get of the prior run
/// key. Byte-reproducibility of the merged output is the load-bearing canary (the merge logic is
/// unchanged; only the prior cursor's byte source moved from materialize-whole to stream).
TEST(CasBlobInDegree, FoldStreamsPriorRunBlockBounded)
{
    using DB::Cas::tests::CountingBackend;
    CountingBackend backend;
    /// InMemory oracle: the SAME two folds against a plain backend must yield byte-identical runs —
    /// the streaming cursor changes I/O shape, not bytes.
    InMemoryBackend oracle;
    Layout layout{"pool"};

    /// Gen 1 from empty prior: enough edges that the SourceEdge run spills across many 256KB blocks.
    /// Each record is 4 + 32(key) + 4 + 1(payload) = 41 bytes, so ~20000 edges is ~820KB => several
    /// blocks under the default block_size, exercising the multi-block streaming path in the fold.
    std::vector<BlobDelta> gen1;
    gen1.reserve(20000);
    for (uint64_t i = 0; i < 20000; ++i)
        gen1.push_back({b(i), s(1), false});

    std::vector<RunRef> runs1_c;
    std::vector<RunRef> runs1_o;
    foldDeltasIntoGeneration(backend, layout, 0, 0, 1, 0, 0, gen1, runs1_c);
    foldDeltasIntoGeneration(oracle, layout, 0, 0, 1, 0, 0, gen1, runs1_o);

    const String gen1_run_key = layout.blobTargetRunKey(1, 0, 0, 0);
    const auto gen1_run = backend.get(gen1_run_key);
    ASSERT_TRUE(gen1_run.has_value());
    const String gen1_run_bytes = gen1_run->bytes;
    /// Sanity: the prior run really spans several blocks (else the block-bounded assertions are
    /// vacuous). Blocks seal at kRunTargetBlockSize (256KB); ~820KB is 3-4 blocks.
    ASSERT_GT(gen1_run_bytes.size(), static_cast<size_t>(kRunTargetBlockSize) * 3);

    /// Reset counters and fold gen 2 with a small delta: remove one edge and add another. The prior
    /// gen-1 run must be consumed via the streaming cursor (head + tail get + body getStream + per-seq
    /// head probe), NEVER a whole-object get.
    backend.resetCounts();
    std::vector<BlobDelta> gen2{{b(0), s(1), true}, {b(19999), s(2), false}};
    std::vector<RunRef> runs2_c;
    std::vector<RunRef> runs2_o;
    foldDeltasIntoGeneration(backend, layout, 1, 0, 2, 0, 0, gen2, runs2_c);
    foldDeltasIntoGeneration(oracle, layout, 1, 0, 2, 0, 0, gen2, runs2_o);

    /// Byte-reproducibility canary: streaming and materialized folds produce identical output bytes.
    const String gen2_run_key = layout.blobTargetRunKey(2, 0, 0, 0);
    const auto gen2_c = backend.get(gen2_run_key);
    const auto gen2_o = oracle.get(gen2_run_key);
    ASSERT_TRUE(gen2_c.has_value());
    ASSERT_TRUE(gen2_o.has_value());
    EXPECT_EQ(gen2_c->bytes, gen2_o->bytes);
    ASSERT_EQ(runs2_c.size(), 1u);
    ASSERT_EQ(runs2_o.size(), 1u);
    EXPECT_EQ(runs2_c[0].checksum, runs2_o[0].checksum);

    /// The core assertion: no whole-object get of the prior run key — every read carried a Range or a
    /// stream (the resident-memory proof at the seam).
    EXPECT_EQ(backend.wholeGetCount(gen1_run_key), 0u);
    /// The cursor opened the prior run's segment via the streaming reader (head + tail get + getStream).
    EXPECT_GE(backend.getStreamCount(gen1_run_key), 1u);
    /// Every ranged-get window on the prior run stays within one block + the footer allowance. This
    /// bound is strict here because the prior run's footer fits inside the fixed tail probe (only very
    /// large runs — ~13k blocks — spill the footer past the probe and add one exact-footer get; a note
    /// for that regime lives in the streaming reader's open comment).
    EXPECT_LE(backend.maxRangedGetLen(gen1_run_key),
              static_cast<uint64_t>(kRunHardCapBlockSize) + 64u * 1024u);
    /// Streaming open touches the prior run's tail probe (and at most one exact-footer get); it is never
    /// re-materialized whole.
    EXPECT_LE(backend.getCount(gen1_run_key), 2u);
}
