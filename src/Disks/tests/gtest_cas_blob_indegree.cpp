#include <gtest/gtest.h>

#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Gc/CasBlobInDegree.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Formats/CasGcStateFormat.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Backend/CasInMemoryBackend.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Formats/CasLayout.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Formats/CasRecordStreamFormat.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Formats/CasWireVocab.h>
#include "config.h"
#if USE_AWS_S3
#include <IO/S3Common.h>
#endif
#include <Disks/tests/cas_test_helpers.h>
#include <IO/WriteBufferFromString.h>
#include <base/defines.h>
#include <Common/Exception.h>

#include <algorithm>
#include <cstdlib>
#include <set>

namespace DB::ErrorCodes { extern const int ABORTED; extern const int CORRUPTED_DATA; extern const int LOGICAL_ERROR; extern const int NOT_IMPLEMENTED; }

using namespace DB::Cas;

namespace
{
UInt128 b(uint64_t n) { return UInt128(n); }
UInt128 s(uint64_t n) { return UInt128(n); }   // source-edge id
/// A `BlobRef` (CityHash128) for the same literal `n` — every existing test's `BlobDelta.ref` /
/// `BlobCandidate.ref` / `inDegreeInRuns` argument is a `BlobRef` as of Phase 3 T3.
BlobRef bh(uint64_t n) { return BlobRef{BlobHashAlgo::CityHash128, BlobDigest::fromU128(UInt128(n))}; }

/// Scale threshold for the "the run genuinely spans several blocks" sanity assertions below. This is
/// NOT a format constant — the SourceEdge run is a plain NDJSON stream (`CasRecordStreamFormat`) with no
/// block framing of its own — it only pins the same byte-size scale the (now-deleted, codecs-v3 phase 6)
/// `CasRunFile` block codec used, so the multi-block-sized fixtures below stay meaningfully large.
constexpr uint32_t kLegacyBlockSize = 256u * 1024u;

/// The pending `HEAD` hook for merge tests: every pending body reads absent.
const BlobHeadFn kPendingBodyAbsent = [](const BlobRef &) { return std::optional<Meta>{}; };

using IntentCreateFn = std::function<IntentCreateResult(const BlobRef &, const CanonicalIdentity &)>;

/// Creates through `create`, each answered as soon as it is sent.
IntentCreates immediateCreates(IntentCreateFn create)
{
    IntentCreates creates;
    creates.start = [fn = std::move(create)](const BlobRef & ref, const CanonicalIdentity & identity) -> IntentCreateAnswer
    {
        IntentCreateResult answer = fn(ref, identity);
        return [result = std::move(answer)] { return result; };
    };
    return creates;
}

/// An intent create hook that counts its calls and confirms the first `confirmed` of them.
IntentCreateFn countingCreate(size_t & calls, size_t confirmed = std::numeric_limits<size_t>::max())
{
    return [&calls, confirmed](const BlobRef &, const CanonicalIdentity &)
    {
        ++calls;
        return calls <= confirmed ? IntentCreateResult{IntentCreate::Created, {}} : IntentCreateResult{IntentCreate::Unconfirmed, "refused"};
    };
}

}

TEST(CASBlobInDegree, FoldStartsFromEmptyPriorGeneration)
{
    InMemoryBackend backend;
    DB::Cas::tests::OperationForTest backend_req(backend);
    Layout layout{"pool"};

    /// Generation 1 from empty prior: two distinct edges on b1 and one on b2.
    /// Edge (b1,s1), (b1,s2), (b2,s1) => indeg(b1)=2, indeg(b2)=1.
    std::vector<BlobDelta> deltas{
        {bh(1), s(1), false},
        {bh(1), s(2), false},
        {bh(2), s(1), false},
    };
    std::vector<RunRef> runs;
    runs.push_back(foldDeltasIntoGeneration(
        *backend_req, layout, /*prior_runs*/{}, /*new*/1, /*attempt*/0, /*shard*/0, deltas));
    ASSERT_FALSE(runs.empty());

    const auto zero = zeroInDegree(*backend_req, runs);
    EXPECT_TRUE(zero.empty());   /// nothing at zero yet
}

TEST(CASBlobInDegree, PlusMinusCancelToZeroDetectsCandidate)
{
    InMemoryBackend backend;
    DB::Cas::tests::OperationForTest backend_req(backend);
    Layout layout{"pool"};

    /// Gen 1: activate edge (b1,s1) and (b2,s1).
    std::vector<RunRef> runs1;
    runs1.push_back(foldDeltasIntoGeneration(
        *backend_req, layout, /*prior_runs*/{}, 1, /*attempt*/0, 0, {{bh(1), s(1), false}, {bh(2), s(1), false}}));

    /// Generation 2 merges prior gen-1 run (resolved via runs1 refs) with removal of (b1,s1): indeg(b1)=0, indeg(b2)=1.
    std::vector<RunRef> runs2;
    runs2.push_back(foldDeltasIntoGeneration(
        *backend_req, layout, /*prior_runs*/runs1, /*new*/2, /*attempt*/0, 0, {{bh(1), s(1), true}}));

    const auto zero = zeroInDegree(*backend_req, runs2);
    ASSERT_EQ(zero.size(), 1u);
    EXPECT_EQ(zero[0].ref, bh(1));
}

TEST(CASBlobInDegree, RunsAreByteDeterministic)
{
    InMemoryBackend a;
    DB::Cas::tests::OperationForTest a_req(a);
    InMemoryBackend b2;
    DB::Cas::tests::OperationForTest b2_req(b2);
    Layout layout{"pool"};
    std::vector<RunRef> ra;
    std::vector<RunRef> rb;
    /// Same deltas in a DIFFERENT input order must produce the same sealed run bytes (sorted by key).
    ra.push_back(foldDeltasIntoGeneration(
        *a_req, layout, /*prior_runs*/{}, 1, /*attempt*/0, 0,
        {{bh(3), s(1), false}, {bh(1), s(1), false}, {bh(2), s(1), false}}));
    rb.push_back(foldDeltasIntoGeneration(
        *b2_req, layout, /*prior_runs*/{}, 1, /*attempt*/0, 0,
        {{bh(1), s(1), false}, {bh(2), s(1), false}, {bh(3), s(1), false}}));
    const auto ga = (*a_req).read(layout.blobTargetRunKey(1, /*attempt*/0, 0, 0), Retry::standard());
    const auto gb = (*b2_req).read(layout.blobTargetRunKey(1, /*attempt*/0, 0, 0), Retry::standard());
    ASSERT_TRUE(ga.has_value());
    ASSERT_TRUE(gb.has_value());
    EXPECT_EQ(ga->bytes, gb->bytes);
    ASSERT_EQ(ra.size(), 1u);
    ASSERT_EQ(rb.size(), 1u);
    EXPECT_EQ(ra[0].checksum, rb[0].checksum);
}

TEST(CASBlobInDegree, SameEdgeActivatedTwiceCountsOnce)
{
    /// Idempotency: activating the same (blob_hash, source_id) twice must not double-count.
    /// The source-edge set is a SET, not a counter — re-adding the same edge is a no-op.
    /// indeg(b1) must be 1 after both activations, not 2.
    InMemoryBackend backend;
    DB::Cas::tests::OperationForTest backend_req(backend);
    Layout layout{"pool"};
    std::vector<BlobDelta> deltas{
        {bh(1), s(1), false},   // activate (b1,s1)
        {bh(1), s(1), false},   // same edge again — must deduplicate
    };
    std::vector<RunRef> runs;
    runs.push_back(foldDeltasIntoGeneration(*backend_req, layout, /*prior_runs*/{}, 1, /*attempt*/0, 0, deltas));
    ASSERT_FALSE(runs.empty());

    const int64_t deg = DB::Cas::tests::inDegreeInRuns(backend, runs, bh(1));
    EXPECT_EQ(deg, 1);   /// deduplicated, not 2

    const auto zero = zeroInDegree(*backend_req, runs);
    EXPECT_TRUE(zero.empty());   /// b1 still has an active edge
}

TEST(CASBlobInDegree, FoldDeltaByteEqualReplayAdopts)
{
    InMemoryBackend backend;
    DB::Cas::tests::OperationForTest backend_req(backend);
    Layout layout{"pool"};
    std::vector<BlobDelta> deltas{{bh(1), s(1), false}};
    std::vector<RunRef> runs1;
    std::vector<RunRef> runs2;
    runs1.push_back(foldDeltasIntoGeneration(
        *backend_req, layout, /*prior_runs*/{}, 1, /*attempt*/7, /*shard*/0, deltas));
    /// Same inputs, same attempt => byte-identical run already present => adopt, no throw.
    EXPECT_NO_THROW(runs2.push_back(foldDeltasIntoGeneration(
        *backend_req, layout, /*prior_runs*/{}, 1, /*attempt*/7, /*shard*/0, deltas)));
    EXPECT_EQ(runs1, runs2);
}

TEST(CASBlobInDegree, FoldDeltaDivergentBytesThrowsCorrupted)
{
    InMemoryBackend backend;
    DB::Cas::tests::OperationForTest backend_req(backend);
    Layout layout{"pool"};
    /// Pre-occupy the run key (attempt 7) with junk, then fold => divergent => CORRUPTED_DATA.
    (*backend_req).create(layout.blobTargetRunKey(1, /*attempt*/7, /*shard*/0, /*seq*/0), "not-a-valid-run", Retry::once());
    std::vector<BlobDelta> deltas{{bh(1), s(1), false}};
    std::vector<RunRef> runs;
    DB::Cas::tests::expectThrowsCode(DB::ErrorCodes::CORRUPTED_DATA,
        [&] { runs.push_back(foldDeltasIntoGeneration(
            *backend_req, layout, /*prior_runs*/{}, 1, /*attempt*/7, /*shard*/0, deltas)); });
}

#if USE_AWS_S3
namespace
{
/// Refuses to serve one key's body. An access denial gets one credential refresh first; it surfaces on
/// the first attempt only because `InMemoryBackend::refreshCredentials` answers false by default, so the
/// write's resolve read ends having observed nothing.
class ReadRefusingBackend : public InMemoryBackend
{
public:
    std::optional<Raw> read(const String & key, TransportAccess & access) override
    {
        if (key == refuse_key)
            throw DB::S3Exception("injected access denial on the resolve read", Aws::S3::S3Errors::ACCESS_DENIED);
        return InMemoryBackend::read(key, access);
    }

    String refuse_key;
};
}

/// A refused write whose resolve read observed NOTHING says nothing about what is at the key, so it
/// must not be reported as pool corruption. `CORRUPTED_DATA` is a deterministic local failure: no
/// caller above reissues it, so a permission or credential blip during the resolve would wedge every
/// later round on the same artifact. The companion arm is `FoldDeltaDivergentBytesThrowsCorrupted`,
/// where the read DID observe divergent bytes and corruption is the right verdict.
TEST(CASBlobInDegree, DeterministicArtifactWhoseResolveReadObservedNothingIsNotCorruption)
{
    ReadRefusingBackend backend;
    DB::Cas::tests::OperationForTest backend_req(backend);
    Layout layout{"pool"};
    const String key = layout.blobTargetRunKey(1, /*attempt*/0, /*shard*/0, /*seq*/0);

    /// Occupy the key so the create's precondition is refused, THEN arm the refusal, so the failure
    /// falls on the resolve read rather than on the setup.
    ASSERT_TRUE(std::holds_alternative<Committed>((*backend_req).create(key, "someone else's bytes", Retry::once())));
    backend.refuse_key = key;

    DB::Cas::tests::expectThrowsCode(DB::ErrorCodes::ABORTED,
        [&] { putDeterministicArtifact(*backend_req, key, "our deterministic bytes"); });
}
#endif

/// ==== two-cursor settlement merge (retired-in-snapshot T3, spec §2.1/§3) ====
///
/// The retired input is no longer a separate `prior_retired` vector — the prior generation's `RunMarker::Condemned`
/// rows RIDE the source-edge run at the zero-sentinel key. These helpers build such a prior run directly
/// (via the sorted-NDJSON `SourceEdgeRunWriter`, codecs-v3 phase 5) and decode a run for assertions.

namespace
{

/// A `RunMarker::Condemned` sentinel record for `h` at the zero source_id, carrying the condemned incarnation.
SourceEdgeRecord condemnedRec(UInt128 h, const CondemnedRow & row)
{
    return SourceEdgeRecord{.ref = BlobRef{BlobHashAlgo::CityHash128, BlobDigest::fromU128(h)},
                            .source_id = UInt128{0}, .marker = RunMarker::Condemned,
                            .delete_pending = row.delete_pending, .token = row.token,
                            .size = row.size, .condemn_round = row.condemn_round,
                            .marker_confirmed = row.marker_confirmed, .standing = row.standing};
}

/// An active-edge record (`RunMarker::Edge`) for `h` at source `sid`.
SourceEdgeRecord edgeRec(UInt128 h, UInt128 sid)
{
    return SourceEdgeRecord{.ref = BlobRef{BlobHashAlgo::CityHash128, BlobDigest::fromU128(h)},
                            .source_id = sid, .marker = RunMarker::Edge};
}

/// `head_blob` / `peek_head` stub. Only the request engine mints an incarnation, so the stub cannot
/// fabricate one: it writes `size` bytes at the blob's own key and hands back the head of what it
/// wrote, which is the incarnation the fold then condemns.
BlobHeadFn headPresent(CasOperation & op, const Layout & layout, uint64_t size)
{
    return [&op, &layout, size](const BlobRef & ref) -> std::optional<Meta>
    {
        const String key = layout.blobKey(ref);
        op.create(key, String(size, 'x'), Retry::standard());
        return op.head(key, Retry::standard());
    };
}

/// A `CondemnedRow` mirroring the old `entry(hash, condemn_round)` fixture (token "t", size 1).
CondemnedRow condemnedRowFor(uint64_t condemn_round, const String & tok = "t",
                             bool delete_pending = false, uint64_t size = 1)
{
    return CondemnedRow{.delete_pending = delete_pending,
                        .token = PersistedEtag{"emulated", tok},
                        .size = size, .condemn_round = condemn_round};
}

/// Build a source-edge run (`kSourceEdgeKeySchema128`) carrying the given `RunMarker::Condemned` sentinel rows
/// and surviving edges, write it under `blobTargetRunKey(gen, attempt, shard, 0)`, and return its
/// `RunRef`. Rows are emitted in (blob_hash, source_id) order (sentinels at source_id 0 sort first
/// per blob).
RunRef writeSourceEdgeRun(InMemoryBackend & backend, const Layout & layout,
                          uint64_t gen, uint64_t attempt, uint64_t shard,
                          const std::vector<std::pair<UInt128, CondemnedRow>> & condemned,
                          const std::vector<std::pair<UInt128, UInt128>> & edges = {})
{
    std::vector<SourceEdgeRecord> recs;
    for (const auto & [h, row] : condemned)
        recs.push_back(condemnedRec(h, row));
    for (const auto & [h, sid] : edges)
        recs.push_back(edgeRec(h, sid));
    /// The writer requires non-decreasing (ref, source_id) order (sentinels at source_id 0 sort first
    /// per blob, exactly reproducing the old raw-key order).
    std::stable_sort(recs.begin(), recs.end(), [](const SourceEdgeRecord & a, const SourceEdgeRecord & bb)
    {
        if (a.ref < bb.ref)
            return true;
        if (bb.ref < a.ref)
            return false;
        return a.source_id < bb.source_id;
    });

    DB::WriteBufferFromOwnString out;
    SourceEdgeRunWriter writer(out);
    for (const auto & rec : recs)
        writer.append(rec);
    writer.finish();
    out.finalize();

    const String bytes = out.str();
    const String key = layout.blobTargetRunKey(gen, attempt, shard, 0);
    DB::Cas::tests::OperationForTest op(backend);
    (*op).create(key, bytes, Retry::once());
    return RunRef{.key = key, .checksum = sourceEdgeRunChecksum(bytes), .shard = shard, .key_generation = gen};
}

struct DecodedRun
{
    std::vector<std::pair<UInt128, CondemnedRow>> condemned;   /// (blob_hash, row)
    std::vector<UInt128> zero_markers;                         /// blob hashes with a zero-transition marker
    std::vector<std::pair<UInt128, UInt128>> edges;            /// (blob_hash, source_id)
};

DecodedRun decodeRun(CasOperation & op, const RunRef & run)
{
    DecodedRun d;
    auto r = openSourceEdgeRun(op, run.key);
    /// Every run this test helper decodes is CityHash128 (16-byte), so `.toU128()` is a
    /// provably-exact round trip.
    SourceEdgeRecord rec;
    while (r.next(rec))
    {
        const UInt128 bh = rec.ref.digest.toU128();
        switch (rec.marker)
        {
            case RunMarker::Condemned: d.condemned.emplace_back(bh, condemnedRowOf(rec)); break;
            case RunMarker::Zero: d.zero_markers.push_back(bh); break;
            case RunMarker::Edge: d.edges.emplace_back(bh, rec.source_id); break;
        }
    }
    return d;
}

}

/// Per-consumer whole-file seal-checksum RED tests (codecs-v3 phase 5, Task 6): a run whose ROWS are
/// well-formed (so `cursor.advance()` never aborts first) but whose `RunRef.checksum` disagrees with the
/// stored bytes must fail closed at each deletion-deriving consumer BEFORE any decision is produced. The
/// stored bytes are the valid run; only the seal checksum handed to the consumer is wrong.
TEST(CASBlobInDegree, FoldSealChecksumMismatchFailsClosed)
{
    InMemoryBackend backend;
    DB::Cas::tests::OperationForTest backend_req(backend);
    Layout layout{"pool"};
    const RunRef good = writeSourceEdgeRun(backend, layout, /*gen*/1, /*attempt*/0, /*shard*/0,
                                           /*condemned*/{}, /*edges*/{{b(1), s(1)}});
    RunRef bad = good;
    bad.checksum = good.checksum + 1;   /// rows still parse; only the seal disagrees
    std::vector<RunRef> prior{bad};
    std::vector<RunRef> out;
    /// A delta on a DIFFERENT blob forces the two-cursor merge to stream the prior run to completion, so
    /// the end-of-segment verifyAgainst fires (not a row-invariant abort).
    EXPECT_THROW(
        out.push_back(foldDeltasIntoGeneration(
            *backend_req, layout, prior, /*new*/2, /*attempt*/0, /*shard*/0,
            std::vector<BlobDelta>{{bh(2), s(1), false}})),
        DB::Exception);
}

TEST(CASBlobInDegree, ZeroInDegreeSealChecksumMismatchFailsClosed)
{
    InMemoryBackend backend;
    DB::Cas::tests::OperationForTest backend_req(backend);
    Layout layout{"pool"};
    const RunRef good = writeSourceEdgeRun(backend, layout, /*gen*/1, /*attempt*/0, /*shard*/0,
                                           /*condemned*/{}, /*edges*/{{b(1), s(1)}});
    RunRef bad = good;
    bad.checksum = good.checksum + 1;
    std::vector<RunRef> runs{bad};
    EXPECT_THROW(zeroInDegree(*backend_req, runs), DB::Exception);
}

TEST(CASThreeCursorMerge, CandidateRowsAtZeroCreateTheirIntents)
{
    InMemoryBackend backend;
    DB::Cas::tests::OperationForTest backend_req(backend);
    Layout layout{"pool"};

    /// Gen 1's run holds one unrelated surviving edge (b9) plus candidate rows for b1 and b2, neither with an
    /// edge. Both creates are sent; the first is confirmed, the second is not.
    const RunRef gen1 = writeSourceEdgeRun(backend, layout, /*gen*/1, 0, 0,
        {{b(1), condemnedRowFor(2)}, {b(2), condemnedRowFor(3)}}, {{b(9), s(1)}});

    size_t creates = 0;
    std::vector<RunRef> runs2;
    RetiredMergeResult rmr;
    runs2.push_back(foldDeltasIntoGeneration(*backend_req, layout, /*prior_runs*/{gen1}, 2, 0, 0, {},
        ShardFoldOptions{
            .current_round = 4,
            .condemn_round = 4,
            .create_intent = immediateCreates(countingCreate(creates, 1)),
            .out_retired = &rmr}));

    EXPECT_EQ(creates, 2u);
    ASSERT_EQ(rmr.confirmed.size(), 1u);
    EXPECT_EQ(rmr.confirmed[0].ref, bh(1));
    EXPECT_EQ(rmr.unconfirmed_creates.count, 1u);
    ASSERT_EQ(rmr.still_retired.size(), 2u);
    EXPECT_TRUE(rmr.still_retired[0].delete_pending);
    EXPECT_EQ(rmr.still_retired[0].condemn_round, 2u);
    EXPECT_FALSE(rmr.still_retired[1].delete_pending);
    EXPECT_EQ(rmr.still_retired[1].condemn_round, 3u);   /// carried unchanged, not re-stamped
    EXPECT_TRUE(rmr.spared.empty());
    EXPECT_TRUE(rmr.body_deletes.empty());

    /// still_retired mirrors exactly the RunMarker::Condemned rows written into the output run, in order.
    const DecodedRun out = decodeRun(*backend_req, runs2[0]);
    ASSERT_EQ(out.condemned.size(), 2u);
    EXPECT_EQ(out.condemned[0].first, b(1));
    EXPECT_TRUE(out.condemned[0].second.delete_pending);
    EXPECT_EQ(out.condemned[1].first, b(2));
    EXPECT_FALSE(out.condemned[1].second.delete_pending);
    EXPECT_TRUE(out.zero_markers.empty());

    /// Without a create hook no create is sent and both rows carry.
    std::vector<RunRef> runs3;
    RetiredMergeResult carried;
    runs3.push_back(foldDeltasIntoGeneration(*backend_req, layout, /*prior_runs*/{gen1}, 3, 0, 0, {},
        {.current_round = 4, .condemn_round = 4, .out_retired = &carried}));
    ASSERT_EQ(carried.still_retired.size(), 2u);
    EXPECT_FALSE(carried.still_retired[0].delete_pending);
    EXPECT_TRUE(carried.confirmed.empty());
}

TEST(CASThreeCursorMerge, PendingRowWhoseHeadShowsItsTokenGoesStandingWithABodyDelete)
{
    InMemoryBackend backend;
    DB::Cas::tests::OperationForTest backend_req(backend);
    Layout layout{"pool"};

    /// A pending row whose body still holds the row's incarnation: the row is written `standing` and its
    /// body delete handed to the caller, conditional on the token the HEAD saw.
    (*backend_req).create(layout.blobKey(bh(1)), "body", Retry::standard());
    const Meta body = *(*backend_req).head(layout.blobKey(bh(1)), Retry::standard());
    CondemnedRow pending = condemnedRowFor(1, "", /*delete_pending*/true);
    pending.token = PersistedEtag::capture(body.etag);
    const RunRef gen1 = writeSourceEdgeRun(backend, layout, /*gen*/1, 0, 0, {{b(1), pending}});
    const BlobHeadFn present = [&](const BlobRef & ref) { return (*backend_req).head(layout.blobKey(ref), Retry::standard()); };

    std::vector<RunRef> runs2;
    RetiredMergeResult rmr;
    runs2.push_back(foldDeltasIntoGeneration(*backend_req, layout, /*prior_runs*/{gen1}, 2, 0, 0, {},
        {.current_round = 9, .condemn_round = 9, .pending_head = present, .out_retired = &rmr}));

    ASSERT_EQ(rmr.body_deletes.size(), 1u);
    EXPECT_EQ(rmr.body_deletes[0].entry.ref, bh(1));
    ASSERT_TRUE(rmr.body_deletes[0].observed.has_value());
    EXPECT_EQ(rmr.body_deletes[0].observed->etag, body.etag);
    ASSERT_EQ(rmr.still_retired.size(), 1u);
    EXPECT_TRUE(rmr.still_retired[0].standing);
    EXPECT_TRUE(rmr.intent_deletes.empty()) << "the intent never leaves with its body";

    const DecodedRun out = decodeRun(*backend_req, runs2[0]);
    ASSERT_EQ(out.condemned.size(), 1u);
    EXPECT_TRUE(out.condemned[0].second.standing);
}

TEST(CASThreeCursorMerge, PendingRowWithAnAbsentBodyGoesStandingWithNoRequest)
{
    InMemoryBackend backend;
    DB::Cas::tests::OperationForTest backend_req(backend);
    Layout layout{"pool"};
    const RunRef gen1 = writeSourceEdgeRun(backend, layout, /*gen*/1, 0, 0,
        {{b(1), condemnedRowFor(1, "t", /*delete_pending*/true)}});

    std::vector<RunRef> runs2;
    RetiredMergeResult rmr;
    runs2.push_back(foldDeltasIntoGeneration(*backend_req, layout, /*prior_runs*/{gen1}, 2, 0, 0, {},
        {.current_round = 9, .condemn_round = 9, .pending_head = kPendingBodyAbsent, .out_retired = &rmr}));

    EXPECT_TRUE(rmr.body_deletes.empty());
    ASSERT_EQ(rmr.still_retired.size(), 1u);
    EXPECT_TRUE(rmr.still_retired[0].standing);
    EXPECT_TRUE(rmr.spared.empty());
}

/// A pending row at zero whose key this pass folded a record of is carried with no body delete, although its `HEAD`
/// shows the row's token: an unmatched removal alone, or an addition and its removal.
TEST(CASThreeCursorMerge, PendingRowAtATouchedZeroIsCarried)
{
    const std::vector<std::vector<BlobDelta>> windows{
        {{bh(1), s(99), true}},
        {{bh(1), s(7), false}, {bh(1), s(7), true}}};
    for (const auto & window : windows)
    {
        SCOPED_TRACE(window.size() == 1 ? "unmatched removal" : "addition and its removal");
        InMemoryBackend backend;
        DB::Cas::tests::OperationForTest backend_req(backend);
        Layout layout{"pool"};
        (*backend_req).create(layout.blobKey(bh(1)), "body", Retry::standard());
        const Meta body = *(*backend_req).head(layout.blobKey(bh(1)), Retry::standard());
        CondemnedRow pending = condemnedRowFor(1, "", /*delete_pending*/true);
        pending.token = PersistedEtag::capture(body.etag);
        const RunRef gen1 = writeSourceEdgeRun(backend, layout, /*gen*/1, 0, 0, {{b(1), pending}});
        size_t heads = 0;
        const BlobHeadFn present = [&](const BlobRef & ref)
        {
            ++heads;
            return (*backend_req).head(layout.blobKey(ref), Retry::standard());
        };

        std::vector<RunRef> runs2;
        RetiredMergeResult rmr;
        runs2.push_back(foldDeltasIntoGeneration(*backend_req, layout, /*prior_runs*/{gen1}, 2, 0, 0, window,
            ShardFoldOptions{
                .current_round = 9,
                .condemn_round = 9,
                .peek_head = present,
                .pending_head = present,
                .out_retired = &rmr}));

        EXPECT_EQ(heads, 1u) << "positive control: the decision HEAD was sent";
        EXPECT_EQ(rmr.unmatched_removes, window.size() == 1 ? 1u : 0u);
        EXPECT_TRUE(rmr.body_deletes.empty());
        EXPECT_TRUE(rmr.intent_deletes.empty());
        EXPECT_TRUE(rmr.spared.empty());
        ASSERT_EQ(rmr.still_retired.size(), 1u);
        EXPECT_TRUE(rmr.still_retired[0].delete_pending);
        EXPECT_FALSE(rmr.still_retired[0].standing);
        EXPECT_EQ(rmr.still_retired[0].token.canonical(), body.etag.canonical());
    }
}

TEST(CASThreeCursorMerge, StandingRowAtZeroDeletesItsIntentAndLeavesTheRun)
{
    InMemoryBackend backend;
    DB::Cas::tests::OperationForTest backend_req(backend);
    Layout layout{"pool"};
    CondemnedRow standing = condemnedRowFor(1, "t", /*delete_pending*/true);
    standing.standing = true;
    const RunRef gen1 = writeSourceEdgeRun(backend, layout, /*gen*/1, 0, 0, {{b(1), standing}});

    std::vector<RunRef> runs2;
    RetiredMergeResult rmr;
    runs2.push_back(foldDeltasIntoGeneration(*backend_req, layout, /*prior_runs*/{gen1}, 2, 0, 0, {},
        {.current_round = 9, .condemn_round = 9, .out_retired = &rmr}));

    ASSERT_EQ(rmr.intent_deletes.size(), 1u);
    EXPECT_EQ(rmr.intent_deletes[0].str(), layout.blobIntentKey(bh(1), *PersistedEtag{"emulated", "t"}.canonical()));
    EXPECT_TRUE(rmr.still_retired.empty());
    const DecodedRun out = decodeRun(*backend_req, runs2[0]);
    EXPECT_TRUE(out.condemned.empty());
    EXPECT_TRUE(out.zero_markers.empty());
}

TEST(CASThreeCursorMerge, RecoverySpares)
{
    InMemoryBackend backend;
    DB::Cas::tests::OperationForTest backend_req(backend);
    Layout layout{"pool"};

    /// A (=b1) has a row from round 1, and this pass's delta adds an edge to it: the row is dropped as spared.
    const RunRef gen1 = writeSourceEdgeRun(backend, layout, /*gen*/1, 0, 0, {{b(1), condemnedRowFor(1)}});

    std::vector<RunRef> runs2;
    RetiredMergeResult rmr;
    runs2.push_back(foldDeltasIntoGeneration(
        *backend_req, layout, /*prior_runs*/{gen1}, 2, 0, 0, {{bh(1), s(1), false}},
        {.current_round = 5, .condemn_round = 6, .out_retired = &rmr}));

    ASSERT_EQ(rmr.spared.size(), 1u);
    EXPECT_EQ(rmr.spared[0].ref, bh(1));
    EXPECT_TRUE(rmr.still_retired.empty());

    /// b1 recovered its edge: the output run carries the surviving edge and no sentinel for it.
    const DecodedRun out = decodeRun(*backend_req, runs2[0]);
    EXPECT_TRUE(out.condemned.empty());
    ASSERT_EQ(out.edges.size(), 1u);
    EXPECT_EQ(out.edges[0].first, b(1));
}

TEST(CASThreeCursorMerge, NewCandidateCondemned)
{
    InMemoryBackend backend;
    DB::Cas::tests::OperationForTest backend_req(backend);
    Layout layout{"pool"};

    /// Gen 1: C (=b3) has one edge. Gen 2 removes it => transition to zero, not retired =>
    /// condemned with the head-captured token at THIS pass's condemn_round.
    std::vector<RunRef> runs1;
    runs1.push_back(foldDeltasIntoGeneration(*backend_req, layout, /*prior_runs*/{}, 1, 0, 0, {{bh(3), s(1), false}}));

    std::vector<RunRef> runs2;
    RetiredMergeResult rmr;
    runs2.push_back(foldDeltasIntoGeneration(
        *backend_req, layout, /*prior_runs*/runs1, 2, 0, 0, {{bh(3), s(1), true}},
        {.condemn_round = 7, .head_blob = headPresent(*backend_req, layout, 42), .out_retired = &rmr}));

    ASSERT_EQ(rmr.still_retired.size(), 1u);
    EXPECT_EQ(rmr.still_retired[0].ref, bh(3));
    const std::optional<Meta> present = (*backend_req).head(layout.blobKey(bh(3)), Retry::standard());
    ASSERT_TRUE(present.has_value());
    EXPECT_TRUE(rmr.still_retired[0].token.matches(present->etag));
    EXPECT_EQ(rmr.still_retired[0].size, 42u);
    EXPECT_EQ(rmr.still_retired[0].condemn_round, 7u);
    EXPECT_TRUE(rmr.spared.empty());

    /// The fresh condemn is emitted as a RunMarker::Condemned row (not a zero marker) into the output run.
    const DecodedRun out = decodeRun(*backend_req, runs2[0]);
    ASSERT_EQ(out.condemned.size(), 1u);
    EXPECT_EQ(out.condemned[0].first, b(3));
    EXPECT_TRUE(out.condemned[0].second.token.matches(present->etag));
    EXPECT_TRUE(out.zero_markers.empty());
}

TEST(CASThreeCursorMerge, AbsentBlobNotCondemned)
{
    InMemoryBackend backend;
    DB::Cas::tests::OperationForTest backend_req(backend);
    Layout layout{"pool"};

    /// Same transition-to-zero as above, but the blob object is already gone at condemn time:
    /// nothing to delete later, so no entry is minted — a plain zero marker is emitted instead.
    std::vector<RunRef> runs1;
    runs1.push_back(foldDeltasIntoGeneration(*backend_req, layout, /*prior_runs*/{}, 1, 0, 0, {{bh(3), s(1), false}}));

    std::vector<RunRef> runs2;
    RetiredMergeResult rmr;
    runs2.push_back(foldDeltasIntoGeneration(
        *backend_req, layout, /*prior_runs*/runs1, 2, 0, 0, {{bh(3), s(1), true}},
        ShardFoldOptions{
            .condemn_round = 7,
            .head_blob = [](const BlobRef &) -> std::optional<Meta> { return std::nullopt; },
            .out_retired = &rmr}));

    EXPECT_TRUE(rmr.still_retired.empty());
    EXPECT_TRUE(rmr.spared.empty());

    const DecodedRun out = decodeRun(*backend_req, runs2[0]);
    EXPECT_TRUE(out.condemned.empty());
    ASSERT_EQ(out.zero_markers.size(), 1u);
    EXPECT_EQ(out.zero_markers[0], b(3));
}

TEST(CASThreeCursorMerge, SnapshotEdgesUnperturbedByRetired)
{
    /// Retired-in-snapshot changes the byte-invariant: the retired machinery now WRITES RunMarker::Condemned
    /// sentinel rows into the run, so a retired-engaged run is no longer byte-identical to a plain one.
    /// The preserved invariant (spec §2.1) is narrower: the retired machinery touches ONLY the sentinel
    /// namespace — the surviving EDGE rows are byte-identical to a plain fold of the same deltas.
    InMemoryBackend plain;
    DB::Cas::tests::OperationForTest plain_req(plain);
    InMemoryBackend engaged;
    DB::Cas::tests::OperationForTest engaged_req(engaged);
    Layout layout{"pool"};

    std::vector<RunRef> r1;
    r1.push_back(foldDeltasIntoGeneration(
        *plain_req, layout, /*prior_runs*/{}, 1, 0, 0,
        {{bh(1), s(1), false}, {bh(2), s(1), false}, {bh(2), s(2), true}}));

    /// Engaged: the SAME deltas, but the prior run carries retired rows for b1 (which the delta re-edges
    /// => spared) and b5 (no edge => its intent is created and confirmed).
    const RunRef prior = writeSourceEdgeRun(engaged, layout, /*gen*/1, 0, 0,
        {{b(1), condemnedRowFor(1)}, {b(5), condemnedRowFor(2)}});
    std::vector<RunRef> r2;
    RetiredMergeResult rmr;
    size_t creates = 0;
    r2.push_back(foldDeltasIntoGeneration(
        *engaged_req, layout, /*prior_runs*/{prior}, 2, 0, 0,
        {{bh(1), s(1), false}, {bh(2), s(1), false}, {bh(2), s(2), true}},
        ShardFoldOptions{
            .current_round = 9,
            .condemn_round = 3,
            .head_blob = headPresent(*engaged_req, layout, 1),
            .create_intent = immediateCreates(countingCreate(creates)),
            .out_retired = &rmr}));

    const DecodedRun plain_run = decodeRun(*plain_req, r1[0]);
    const DecodedRun engaged_run = decodeRun(*engaged_req, r2[0]);
    EXPECT_EQ(plain_run.edges, engaged_run.edges);   /// edge rows byte-identical
    EXPECT_TRUE(plain_run.condemned.empty());
    /// The engaged run carries only the retired sentinel(s) on top: b1 spared (no row), b5 pending.
    ASSERT_EQ(engaged_run.condemned.size(), 1u);
    EXPECT_EQ(engaged_run.condemned[0].first, b(5));
    EXPECT_TRUE(engaged_run.condemned[0].second.delete_pending);
}

TEST(CASTwoCursorMerge, CarriedSentinelIsNotATouch)
{
    /// Gen 1 condemns b (a real +edge/-edge net-to-zero with head_blob present) -> a RunMarker::Condemned row. Gen 2
    /// has NO deltas at all: the carried row must (a) survive byte-identically, (b) emit no zero marker,
    /// (c) never call peek_head (a carried sentinel is not a touch).
    InMemoryBackend backend;
    DB::Cas::tests::OperationForTest backend_req(backend);
    Layout layout{"pool"};

    /// Gen 1: (b,s1) added then removed => net-to-zero => fresh condemn at round 5 (token "tok", size 7).
    std::vector<RunRef> runs1;
    RetiredMergeResult rmr1;
    runs1.push_back(foldDeltasIntoGeneration(
        *backend_req, layout, /*prior_runs*/{}, 1, 0, 0, {{bh(2), s(1), false}, {bh(2), s(1), true}},
        {.condemn_round = 5, .head_blob = headPresent(*backend_req, layout, 7), .out_retired = &rmr1}));
    ASSERT_EQ(rmr1.still_retired.size(), 1u);
    {
        const DecodedRun g1 = decodeRun(*backend_req, runs1[0]);
        ASSERT_EQ(g1.condemned.size(), 1u);
        EXPECT_EQ(g1.condemned[0].first, b(2));
        EXPECT_TRUE(g1.zero_markers.empty());   /// a condemned blob emits RunMarker::Condemned, never a zero marker
    }

    /// Gen 2: empty deltas and no create hook: the candidate carries. peek_head must NOT fire.
    size_t peek_calls = 0;
    auto peek = [&](const BlobRef &) -> std::optional<Meta> { ++peek_calls; return {}; };
    std::vector<RunRef> runs2;
    RetiredMergeResult rmr2;
    runs2.push_back(foldDeltasIntoGeneration(*backend_req, layout, /*prior_runs*/runs1, 2, 0, 0, {},
        {.current_round = 1, .condemn_round = 6, .peek_head = peek, .out_retired = &rmr2}));

    EXPECT_EQ(peek_calls, 0u);
    ASSERT_EQ(rmr2.still_retired.size(), 1u);
    EXPECT_EQ(rmr2.still_retired[0].ref, bh(2));
    EXPECT_EQ(rmr2.still_retired[0].condemn_round, 5u);   /// carried unchanged

    const DecodedRun g2 = decodeRun(*backend_req, runs2[0]);
    ASSERT_EQ(g2.condemned.size(), 1u);
    EXPECT_EQ(g2.condemned[0].first, b(2));
    const std::optional<Meta> present = (*backend_req).head(layout.blobKey(bh(2)), Retry::standard());
    ASSERT_TRUE(present.has_value());
    EXPECT_TRUE(g2.condemned[0].second.token.matches(present->etag));
    EXPECT_EQ(g2.condemned[0].second.size, 7u);
    EXPECT_TRUE(g2.zero_markers.empty());
}

TEST(CASTwoCursorMerge, MalformedRunFailsClosed)
{
    Layout layout{"pool"};

    /// (1) An active edge at the reserved sentinel source_id 0 -> the merge cursor fails closed.
    {
        InMemoryBackend backend;
        DB::Cas::tests::OperationForTest backend_req(backend);
        DB::WriteBufferFromOwnString out;
        SourceEdgeRunWriter writer(out);
        writer.append(edgeRec(1, UInt128{0}));   // edge at sentinel key
        writer.finish();
        out.finalize();
        const String bytes = out.str();
        const RunRef bad{.key = layout.blobTargetRunKey(1, 0, 0, 0),
                         .checksum = sourceEdgeRunChecksum(bytes), .shard = 0, .key_generation = 1};
        (*backend_req).create(bad.key, bytes, Retry::once());

        std::vector<RunRef> runs2;
        EXPECT_THROW(runs2.push_back(foldDeltasIntoGeneration(*backend_req, layout, /*prior_runs*/{bad}, 2, 0, 0, {})),
                     DB::Exception);
    }

    /// (2) Two sentinel rows for one blob -> duplicate sentinel -> the merge cursor fails closed.
    {
        InMemoryBackend backend;
        DB::Cas::tests::OperationForTest backend_req(backend);
        DB::WriteBufferFromOwnString out;
        SourceEdgeRunWriter writer(out);
        /// Same (b,0) key twice (equal keys are allowed by the writer) — two condemned sentinels for b1.
        writer.append(condemnedRec(1, condemnedRowFor(1)));
        writer.append(condemnedRec(1, condemnedRowFor(2)));
        writer.finish();
        out.finalize();
        const String bytes = out.str();
        const RunRef bad{.key = layout.blobTargetRunKey(1, 0, 0, 0),
                         .checksum = sourceEdgeRunChecksum(bytes), .shard = 0, .key_generation = 1};
        (*backend_req).create(bad.key, bytes, Retry::once());

        std::vector<RunRef> runs2;
        EXPECT_THROW(runs2.push_back(foldDeltasIntoGeneration(*backend_req, layout, /*prior_runs*/{bad}, 2, 0, 0, {})),
                     DB::Exception);
    }
}

/// A prior run several times larger than one buffer folds correctly with the streaming prior cursor
/// AND the backend is never asked to READ that run's key — the cursor reaches it only through the
/// streaming open. Byte-reproducibility of the merged output is the load-bearing canary (the merge
/// logic is unchanged; only the prior cursor's byte source moved from materialize-whole to stream).
/// What this does NOT check is how much the open stream buffers: the streaming primitive carries no
/// window, so the seam has nothing to measure.
TEST(CASBlobInDegree, FoldStreamsPriorRunWithoutReadingItWhole)
{
    using DB::Cas::tests::CountingBackend;
    CountingBackend backend;
    DB::Cas::tests::OperationForTest backend_req(backend);
    /// InMemory oracle: the SAME two folds against a plain backend must yield byte-identical runs —
    /// the streaming cursor changes I/O shape, not bytes.
    InMemoryBackend oracle;
    DB::Cas::tests::OperationForTest oracle_req(oracle);
    Layout layout{"pool"};

    /// Gen 1 from empty prior: enough edges that the SourceEdge run spills across many 256KB blocks.
    /// Each record is 4 + 32(key) + 4 + 1(payload) = 41 bytes, so ~20000 edges is ~820KB => several
    /// blocks under the default block_size, exercising the multi-block streaming path in the fold.
    std::vector<BlobDelta> gen1;
    gen1.reserve(20000);
    for (uint64_t i = 0; i < 20000; ++i)
        gen1.push_back({bh(i), s(1), false});

    std::vector<RunRef> runs1_c;
    std::vector<RunRef> runs1_o;
    runs1_c.push_back(foldDeltasIntoGeneration(*backend_req, layout, /*prior_runs*/{}, 1, 0, 0, gen1));
    runs1_o.push_back(foldDeltasIntoGeneration(*oracle_req, layout, /*prior_runs*/{}, 1, 0, 0, gen1));

    const String gen1_run_key = layout.blobTargetRunKey(1, 0, 0, 0);
    const auto gen1_run = (*backend_req).read(gen1_run_key, Retry::standard());
    ASSERT_TRUE(gen1_run.has_value());
    const String gen1_run_bytes = gen1_run->bytes;
    /// Sanity: the prior run is far larger than one read buffer, so "it was never read whole" is a
    /// claim about a genuinely large object rather than one a single buffer could have swallowed.
    ASSERT_GT(gen1_run_bytes.size(), static_cast<size_t>(kLegacyBlockSize) * 3);

    /// Reset counters and fold gen 2 with a small delta: remove one edge and add another. The prior
    /// gen-1 run must be consumed via one streaming open per prior segment, NEVER a whole-object get.
    backend.resetCounts();
    /// Arm a window smaller than the run so the fold's output is proven correct under chunked delivery,
    /// not merely served in one piece: unarmed, `getStream` never installs the recording wrapper at all.
    backend.setStreamChunkForTest(kLegacyBlockSize / 4);
    std::vector<BlobDelta> gen2{{bh(0), s(1), true}, {bh(19999), s(2), false}};
    std::vector<RunRef> runs2_c;
    std::vector<RunRef> runs2_o;
    runs2_c.push_back(foldDeltasIntoGeneration(*backend_req, layout, /*prior_runs*/runs1_c, 2, 0, 0, gen2));
    runs2_o.push_back(foldDeltasIntoGeneration(*oracle_req, layout, /*prior_runs*/runs1_o, 2, 0, 0, gen2));

    /// Byte-reproducibility canary: streaming and materialized folds produce identical output bytes.
    const String gen2_run_key = layout.blobTargetRunKey(2, 0, 0, 0);
    const auto gen2_c = (*backend_req).read(gen2_run_key, Retry::standard());
    const auto gen2_o = (*oracle_req).read(gen2_run_key, Retry::standard());
    ASSERT_TRUE(gen2_c.has_value());
    ASSERT_TRUE(gen2_o.has_value());
    EXPECT_EQ(gen2_c->bytes, gen2_o->bytes);
    ASSERT_EQ(runs2_c.size(), 1u);
    ASSERT_EQ(runs2_o.size(), 1u);
    EXPECT_EQ(runs2_c[0].checksum, runs2_o[0].checksum);

    /// The core assertion: the prior run is never read whole — the cursor reaches it only through the
    /// streaming open, so the seam sees no read of that key at all.
    EXPECT_EQ(backend.getCount(gen1_run_key), 0u);
    /// The cursor opened the prior run's segment through the streaming reader.
    EXPECT_GE(backend.getStreamCount(gen1_run_key), 1u);
    /// The other half of the evidence: a nonzero value here can only come from the recording wrapper
    /// `getStream` installs when armed, so this proves the arming actually took effect and the
    /// byte-parity check above ran under genuinely chunked delivery, not a no-op setter.
    EXPECT_GT(backend.largestStreamChunk(gen1_run_key), 0u);
    /// This does NOT bound how much the stream buffers per request: the streaming primitive carries no
    /// window for the seam to measure, so resident memory inside the open stream is out of its reach.
}

/// The preview consumer `zeroInDegree` streams a large run instead of materializing it whole: the
/// backend is never asked to READ the run's key, only to open it as a stream, and the candidate set
/// equals the pre-change (borrowed-mode) result. Byte-parity against an InMemory oracle is the
/// load-bearing canary — the scan logic is unchanged; only the byte source moved to the stream.
/// As above, the buffering inside the open stream is not bounded here; nothing at the seam sees it.
TEST(CASBlobInDegree, ZeroInDegreeStreamsRunWithoutReadingItWhole)
{
    using DB::Cas::tests::CountingBackend;
    CountingBackend backend;
    DB::Cas::tests::OperationForTest backend_req(backend);
    InMemoryBackend oracle;
    DB::Cas::tests::OperationForTest oracle_req(oracle);
    Layout layout{"pool"};

    /// Gen 1 from empty prior: ~20000 active edges spill the SourceEdge run across several 256KB blocks.
    std::vector<BlobDelta> gen1;
    gen1.reserve(20000);
    for (uint64_t i = 0; i < 20000; ++i)
        gen1.push_back({bh(i), s(1), false});

    std::vector<RunRef> runs1_c;
    std::vector<RunRef> runs1_o;
    runs1_c.push_back(foldDeltasIntoGeneration(*backend_req, layout, /*prior_runs*/{}, 1, 0, 0, gen1));
    runs1_o.push_back(foldDeltasIntoGeneration(*oracle_req, layout, /*prior_runs*/{}, 1, 0, 0, gen1));

    /// Gen 2 removes every edge on two of the blobs => two zero-transition markers in the gen-2 run,
    /// which is itself multi-block (the surviving-edge rows still span blocks).
    std::vector<BlobDelta> gen2{{bh(0), s(1), true}, {bh(19999), s(1), true}};
    std::vector<RunRef> runs2_c;
    std::vector<RunRef> runs2_o;
    runs2_c.push_back(foldDeltasIntoGeneration(*backend_req, layout, /*prior_runs*/runs1_c, 2, 0, 0, gen2));
    runs2_o.push_back(foldDeltasIntoGeneration(*oracle_req, layout, /*prior_runs*/runs1_o, 2, 0, 0, gen2));

    const String gen2_run_key = layout.blobTargetRunKey(2, 0, 0, 0);
    const auto gen2_run = (*backend_req).read(gen2_run_key, Retry::standard());
    ASSERT_TRUE(gen2_run.has_value());
    /// Sanity: the run is far larger than one read buffer, for the same reason as above.
    ASSERT_GT(gen2_run->bytes.size(), static_cast<size_t>(kLegacyBlockSize) * 3);

    backend.resetCounts();
    /// Arm a window smaller than the run so the candidate set below is proven correct under chunked
    /// delivery, not merely served in one piece: unarmed, `getStream` never installs the recording
    /// wrapper at all.
    backend.setStreamChunkForTest(kLegacyBlockSize / 4);
    const auto zero_c = zeroInDegree(*backend_req, runs2_c);
    const auto zero_o = zeroInDegree(*oracle_req, runs2_o);

    /// Equivalence with the borrowed-mode (InMemory oracle) result: same candidates, in the same order.
    ASSERT_EQ(zero_c.size(), zero_o.size());
    ASSERT_EQ(zero_c.size(), 2u);
    for (size_t i = 0; i < zero_c.size(); ++i)
        EXPECT_EQ(zero_c[i].ref, zero_o[i].ref);

    /// The core assertion: the run is never read whole — the scan reaches it only through the streaming
    /// open, so the seam sees no read of that key at all.
    EXPECT_EQ(backend.getCount(gen2_run_key), 0u);
    /// The scan opened the run through the streaming reader.
    EXPECT_GE(backend.getStreamCount(gen2_run_key), 1u);
    /// The other half of the evidence: a nonzero value here can only come from the recording wrapper
    /// `getStream` installs when armed, so this proves the arming actually took effect and the
    /// candidate-set check above ran under genuinely chunked delivery, not a no-op setter.
    EXPECT_GT(backend.largestStreamChunk(gen2_run_key), 0u);
    /// This does NOT bound how much the stream buffers per request: the streaming primitive carries no
    /// window for the seam to measure, so resident memory inside the open stream is out of its reach.
}

/// ==== Typed source-edge open ====

TEST(CASSourceEdgeRun, SourceEdgeIdZeroIsReserved)
{
    /// The zero source_id is the sentinel namespace; producers fail closed on a zero hash
    /// (probability 2^-128 — the check documents the reservation).
    EXPECT_DEATH(
        {
            DB::abort_on_logical_error.store(true, std::memory_order_relaxed);
            DB::Cas::assertValidSourceEdgeId(UInt128{0});
        },
        "source_id 0 is the reserved sentinel key");
    EXPECT_NO_THROW(DB::Cas::assertValidSourceEdgeId(UInt128{1}));
}

/// ==== schema 3 key codec (Phase 3 T3, mixed-algo pools) ====

TEST(CASSourceEdgeKeySchema3, MixedWidthKeysOrderAlgoFirst)
{
    const BlobDigest d16 = BlobDigest::fromU128((UInt128(0xFFFFFFFFFFFFFFFFULL) << 64) | 0xFFULL);
    BlobDigest d32{};                                    /// sha256 digest starting 0x00,0x01 — small bytes
    d32.bytes[1] = 0x01;
    const BlobRef ch{BlobHashAlgo::CityHash128, d16};    /// algo=1, digest all-FF prefix
    const BlobRef sh{BlobHashAlgo::Sha256, d32};         /// algo=3, tiny digest
    const String k_ch = SourceEdgeKeyCodec::key(ch, UInt128(7));   /// 33 bytes
    const String k_sh = SourceEdgeKeyCodec::key(sh, UInt128(7));   /// 49 bytes
    EXPECT_EQ(k_ch.size(), 33u);
    EXPECT_EQ(k_sh.size(), 49u);
    /// algo byte decides BEFORE any digest byte can: ch128(1) < sha256(3) even though the ch128
    /// digest bytes are all 0xFF and the sha256 digest bytes are almost all zero.
    EXPECT_LT(k_ch, k_sh);
    /// sentinel-first inside one blob group:
    EXPECT_LT(SourceEdgeKeyCodec::key(ch, UInt128(0)), k_ch);
}

TEST(CASSourceEdgeKeySchema3, ParseFailsClosed)
{
    BlobRef r; UInt128 sid;
    String k = SourceEdgeKeyCodec::key(BlobRef{BlobHashAlgo::XXH3_128, BlobDigest::fromU128(UInt128(5))}, UInt128(9));
    SourceEdgeKeyCodec::parse(k, r, sid);
    EXPECT_EQ(r.algo, BlobHashAlgo::XXH3_128);
    EXPECT_EQ(r.digest.toU128(), UInt128(5));
    EXPECT_EQ(sid, UInt128(9));
    k[0] = static_cast<char>(99);                        /// unknown algo byte
    DB::Cas::tests::expectThrowsCode(DB::ErrorCodes::NOT_IMPLEMENTED, [&]{ SourceEdgeKeyCodec::parse(k, r, sid); });
    k[0] = static_cast<char>(1);                         /// known algo, wrong length (33 expected, this is 33 — truncate)
    DB::Cas::tests::expectThrowsCode(DB::ErrorCodes::CORRUPTED_DATA, [&]{ SourceEdgeKeyCodec::parse(std::string_view(k).substr(0, 20), r, sid); });
}

TEST(CASBlobInDegree, TwoAlgoFoldSettlesBothInOneShardRun)
{
    /// Step 3 (Phase 3 T3): extend the fold with deltas for ch128:X and sha256:Y in ONE shard run —
    /// both settle (edges present, condemn on removal works per ref), mixed rows in one run, no
    /// algo loop.
    InMemoryBackend backend;
    DB::Cas::tests::OperationForTest backend_req(backend);
    Layout layout{"pool"};

    const BlobRef ch_x{BlobHashAlgo::CityHash128, BlobDigest::fromU128(UInt128(11))};
    BlobDigest sha_y{};
    sha_y.bytes[0] = 0xAB;
    const BlobRef sha_y_ref{BlobHashAlgo::Sha256, sha_y};

    std::vector<RunRef> runs1;
    runs1.push_back(foldDeltasIntoGeneration(
        *backend_req, layout, /*prior_runs*/{}, 1, /*attempt*/0, 0, {{ch_x, s(1), false}, {sha_y_ref, s(1), false}}));
    ASSERT_FALSE(runs1.empty());

    EXPECT_EQ(DB::Cas::tests::inDegreeInRuns(backend, runs1, ch_x), 1);
    EXPECT_EQ(DB::Cas::tests::inDegreeInRuns(backend, runs1, sha_y_ref), 1);
    EXPECT_TRUE(zeroInDegree(*backend_req, runs1).empty());

    /// Remove both edges in gen 2: each transitions to zero independently, condemned per its own ref.
    std::vector<RunRef> runs2;
    RetiredMergeResult rmr;
    runs2.push_back(foldDeltasIntoGeneration(
        *backend_req, layout, /*prior_runs*/runs1, 2, /*attempt*/0, 0, {{ch_x, s(1), true}, {sha_y_ref, s(1), true}},
        {.condemn_round = 1, .head_blob = headPresent(*backend_req, layout, 1), .out_retired = &rmr}));

    ASSERT_EQ(rmr.still_retired.size(), 2u);
    std::vector<BlobRef> condemned_refs{rmr.still_retired[0].ref, rmr.still_retired[1].ref};
    EXPECT_NE(std::find(condemned_refs.begin(), condemned_refs.end(), ch_x), condemned_refs.end());
    EXPECT_NE(std::find(condemned_refs.begin(), condemned_refs.end(), sha_y_ref), condemned_refs.end());
    EXPECT_EQ(DB::Cas::tests::inDegreeInRuns(backend, runs2, ch_x), 0);
    EXPECT_EQ(DB::Cas::tests::inDegreeInRuns(backend, runs2, sha_y_ref), 0);
}

/// [UNMATCHED-MINUS-ONE] pin. In-degree is a SET of source edges applied last-wins per
/// (ref, ManifestId, path) key -- NOT a counter. A removal delta whose matching activation was
/// never folded (reachable today via a false-404 at the activation fold plus a dead-build skip)
/// must therefore be a per-key NO-OP: it marks an already-absent edge absent and cannot strip a
/// sibling manifest's edge for the SAME blob. The whole "that interleaving is harmless" argument in
/// the publish-confirm design rests on this; if the model ever regresses to counter arithmetic this
/// test goes red and premature deletion becomes reachable again.
TEST(CASBlobInDegree, UnmatchedRemovalIsAPerKeyNoOpAndSparesSiblingEdges)
{
    InMemoryBackend backend;
    DB::Cas::tests::OperationForTest backend_req(backend);
    Layout layout{"pool"};

    /// Generation 1: blob b1 is referenced by TWO distinct sources (two manifests).
    std::vector<RunRef> runs1;
    runs1.push_back(foldDeltasIntoGeneration(
        *backend_req, layout, /*prior_runs*/{}, /*new_generation*/1, /*attempt*/0, /*shard*/0,
        {{bh(1), s(1), false}, {bh(1), s(2), false}}));

    /// Generation 2: fold a removal for a THIRD source that never had an activation folded.
    std::vector<RunRef> runs2;
    runs2.push_back(foldDeltasIntoGeneration(
        *backend_req, layout, /*prior_runs*/runs1, /*new_generation*/2, /*attempt*/0, /*shard*/0,
        {{bh(1), s(99), true}}));

    /// Both original edges survive: the unmatched removal touched only its own (absent) key.
    const DecodedRun out = decodeRun(*backend_req, runs2[0]);
    ASSERT_EQ(out.edges.size(), 2u) << "an unmatched removal must not strip sibling edges";
    /// And the blob is NOT a deletion candidate.
    const auto zero = zeroInDegree(*backend_req, runs2);
    EXPECT_TRUE(zero.empty()) << "b1 still has two live source edges";
}

/// The silence in `UnmatchedRemovalIsAPerKeyNoOpAndSparesSiblingEdges` above is exactly what let a whole
/// class of GC defects survive months of soak runs undetected — the fold's per-key no-op left no trace.
/// This test pins the COUNTING surface added on top: `RetiredMergeResult::unmatched_removes` /
/// `unmatched_remove_example` must report the unmatched remove precisely (one hit, naming the right blob
/// and source id), while the byte-level no-op behaviour (asserted above) is unchanged.
TEST(CASBlobInDegree, UnmatchedRemovalIsCountedWithAnExample)
{
    InMemoryBackend backend;
    DB::Cas::tests::OperationForTest backend_req(backend);
    Layout layout{"pool"};

    /// Generation 1: blob b1 is referenced by TWO distinct sources (two manifests), same fixture as the
    /// no-op test above.
    std::vector<RunRef> runs1;
    runs1.push_back(foldDeltasIntoGeneration(
        *backend_req, layout, /*prior_runs*/{}, /*new_generation*/1, /*attempt*/0, /*shard*/0,
        {{bh(1), s(1), false}, {bh(1), s(2), false}}));

    /// Generation 2: fold a removal for a THIRD source that never had an activation folded.
    std::vector<RunRef> runs2;
    RetiredMergeResult rmr;
    runs2.push_back(foldDeltasIntoGeneration(
        *backend_req, layout, /*prior_runs*/runs1, /*new_generation*/2, /*attempt*/0, /*shard*/0,
        {{bh(1), s(99), true}},
        {.out_retired = &rmr}));

    /// The run is byte-identical to the no-op test's outcome for the blob's OTHER edges: both survive.
    const DecodedRun out = decodeRun(*backend_req, runs2[0]);
    ASSERT_EQ(out.edges.size(), 2u) << "the counting surface must not perturb the no-op fold outcome";
    EXPECT_EQ(out.edges[0].first, b(1));
    EXPECT_EQ(out.edges[1].first, b(1));
    std::vector<UInt128> surviving_sources{out.edges[0].second, out.edges[1].second};
    EXPECT_NE(std::find(surviving_sources.begin(), surviving_sources.end(), s(1)), surviving_sources.end());
    EXPECT_NE(std::find(surviving_sources.begin(), surviving_sources.end(), s(2)), surviving_sources.end());

    /// The counting surface reports exactly the one unmatched remove, naming the right blob and source id.
    EXPECT_EQ(rmr.unmatched_removes, 1u);
    ASSERT_TRUE(rmr.unmatched_remove_example.has_value());
    EXPECT_EQ(rmr.unmatched_remove_example->ref, bh(1));
    EXPECT_EQ(rmr.unmatched_remove_example->source_id, s(99));
}

namespace
{
/// N distinct condemned rows for blobs b(1)..b(n), same shape `condemnedRowFor` produces, varying
/// only the token so distinct rows are trivially distinguishable in a failure message.
std::vector<std::pair<UInt128, CondemnedRow>> condemnedCohort(uint64_t n, uint64_t condemn_round, bool delete_pending)
{
    std::vector<std::pair<UInt128, CondemnedRow>> rows;
    for (uint64_t i = 1; i <= n; ++i)
        rows.push_back({b(i), condemnedRowFor(condemn_round, "t" + std::to_string(i), delete_pending)});
    return rows;
}
}

/// The redelete cohort is capped at `GcRoundWorkBudget::max_redeletes` per call. Excess
/// entries stay in `still_retired`, still `delete_pending`, to be redeleted by a later round — the
/// durable pipeline never loses one to the cap.
TEST(CASThreeCursorMerge, RedeleteBudgetCapsCohortAndCarriesExcess)
{
    InMemoryBackend backend;
    DB::Cas::tests::OperationForTest backend_req(backend);
    Layout layout{"pool"};
    const RunRef gen1 = writeSourceEdgeRun(backend, layout, 1, 0, 0, condemnedCohort(10, 1, /*delete_pending*/true));

    GcRoundWorkBudget budget;
    budget.max_redeletes = 3;
    size_t heads = 0;
    const BlobHeadFn absent = [&](const BlobRef &) { ++heads; return std::optional<Meta>{}; };

    std::vector<RunRef> runs2;
    RetiredMergeResult rmr;
    runs2.push_back(foldDeltasIntoGeneration(*backend_req, layout, /*prior_runs*/{gen1}, 2, 0, 0, {},
        {.current_round = 9, .condemn_round = 9, .pending_head = absent, .out_retired = &rmr, .work_budget = &budget}));

    EXPECT_EQ(heads, 3u);
    EXPECT_EQ(budget.redeletes_used, 3u);
    EXPECT_TRUE(rmr.spared.empty());
    ASSERT_EQ(rmr.still_retired.size(), 10u);
    size_t standing = 0;
    for (const RetiredEntry & e : rmr.still_retired)
    {
        EXPECT_TRUE(e.delete_pending);
        standing += e.standing;
    }
    EXPECT_EQ(standing, 3u) << "only the rows the budget admitted were decided; the rest carry pending";
}

/// Mirror test for the create cap: rows past `max_intent_creates` carry unchanged as candidates, and a later
/// round creates them.
TEST(CASThreeCursorMerge, CreateBudgetCapsCohortAndCarriesExcess)
{
    InMemoryBackend backend;
    DB::Cas::tests::OperationForTest backend_req(backend);
    Layout layout{"pool"};
    const RunRef gen1 = writeSourceEdgeRun(backend, layout, 1, 0, 0, condemnedCohort(10, /*condemn_round*/1, /*delete_pending*/false));

    GcRoundWorkBudget budget;
    budget.max_intent_creates = 3;
    size_t creates = 0;

    std::vector<RunRef> runs2;
    RetiredMergeResult rmr;
    runs2.push_back(foldDeltasIntoGeneration(*backend_req, layout, /*prior_runs*/{gen1}, 2, 0, 0, {},
        ShardFoldOptions{
            .current_round = 5,
            .condemn_round = 6,
            .pending_head = kPendingBodyAbsent,
            .create_intent = immediateCreates(countingCreate(creates)),
            .out_retired = &rmr,
            .work_budget = &budget}));

    EXPECT_EQ(creates, 3u);
    EXPECT_EQ(rmr.confirmed.size(), 3u);
    EXPECT_EQ(budget.intent_creates_used, 3u);
    ASSERT_EQ(rmr.still_retired.size(), 10u);
    size_t pending_count = 0;
    size_t carried_count = 0;
    for (const RetiredEntry & e : rmr.still_retired)
        e.delete_pending ? ++pending_count : ++carried_count;
    EXPECT_EQ(pending_count, 3u)  << "only the 3 created rows are pending";
    EXPECT_EQ(carried_count, 7u) << "the rest carry unchanged, still eligible next round";
}

/// The mandatory convergence proof: a cohort well past the per-round cap fully drains over
/// ceil(N / cap) rounds, feeding each round's output run back as the next round's prior — the exact
/// shape a real GC round repeats every pass.
TEST(CASThreeCursorMerge, RedeleteBudgetDrainsCohortToFixpointOverRounds)
{
    InMemoryBackend backend;
    DB::Cas::tests::OperationForTest backend_req(backend);
    Layout layout{"pool"};
    std::vector<RunRef> priors{writeSourceEdgeRun(backend, layout, 1, 0, 0, condemnedCohort(10, 1, /*delete_pending*/true))};

    /// A row decided in one round is `standing` in its output; the next round removes its intent and the row.
    uint64_t decided = 0;
    uint64_t rounds = 0;
    bool pending_left = true;
    while (rounds < 10 && pending_left)
    {
        GcRoundWorkBudget budget;
        budget.max_redeletes = 3;
        std::vector<RunRef> out_runs;
        RetiredMergeResult rmr;
        out_runs.push_back(foldDeltasIntoGeneration(*backend_req, layout, priors, 2 + rounds, 0, 0, {},
            ShardFoldOptions{
                .current_round = 100,
                .condemn_round = 100,
                .pending_head = kPendingBodyAbsent,
                .out_retired = &rmr,
                .work_budget = &budget}));
        ++rounds;
        decided += std::count_if(rmr.still_retired.begin(), rmr.still_retired.end(), [](const RetiredEntry & e) { return e.standing; });
        pending_left = std::any_of(rmr.still_retired.begin(), rmr.still_retired.end(), [](const RetiredEntry & e) { return !e.standing; });
        ASSERT_FALSE(out_runs.empty());
        priors = out_runs;
    }
    EXPECT_EQ(decided, 10u) << "no row lost to the cap across the whole drain";
    EXPECT_EQ(rounds, 4u) << "ceil(10 / 3) rounds to decide every pending row";
}

TEST(CASThreeCursorMerge, OnlyASentCreateSpendsABudgetSlot)
{
    for (const size_t unconfirmed : {size_t{0}, size_t{2}})
    {
        SCOPED_TRACE(unconfirmed);
        InMemoryBackend backend;
        DB::Cas::tests::OperationForTest backend_req(backend);
        Layout layout{"pool"};
        const RunRef gen1 = writeSourceEdgeRun(backend, layout, 1, 0, 0, condemnedCohort(10, /*condemn_round*/1, false));

        GcRoundWorkBudget budget;
        budget.max_intent_creates = 3;
        size_t creates = 0;
        const IntentCreateFn create = [&](const BlobRef &, const CanonicalIdentity &)
        {
            return ++creates <= unconfirmed ? IntentCreateResult{} : IntentCreateResult{IntentCreate::Created, {}};
        };

        std::vector<RunRef> runs2;
        RetiredMergeResult rmr;
        runs2.push_back(foldDeltasIntoGeneration(*backend_req, layout, /*prior_runs*/{gen1}, 2, 0, 0, {},
            ShardFoldOptions{
                .current_round = 5,
                .condemn_round = 6,
                .pending_head = kPendingBodyAbsent,
                .create_intent = immediateCreates(create),
                .out_retired = &rmr,
                .work_budget = &budget}));

        EXPECT_EQ(creates, 3u) << "a create was sent without a budget slot";
        EXPECT_EQ(rmr.confirmed.size(), 3u - unconfirmed) << "an unconfirmed create was counted";
        EXPECT_EQ(rmr.unconfirmed_creates.count, unconfirmed);
        EXPECT_EQ(budget.intent_creates_used, 3u);
        EXPECT_EQ(rmr.still_retired.size(), 10u);
    }
}

/// Creates answered later than they are sent: the run is the one a synchronous create writes, each row takes its
/// own create's answer, and the records held behind an unanswered create are bounded.
TEST(CASThreeCursorMerge, OutstandingCreatesWriteTheSameRunAndBoundWhatIsHeld)
{
    constexpr size_t kEdges = 20000;
    /// Candidates at 1, 3 and 5, then one live edge for each of `kEdges` later blobs.
    const std::vector<std::pair<UInt128, CondemnedRow>> candidates{
        {b(1), condemnedRowFor(1, "t1")}, {b(3), condemnedRowFor(1, "t3")}, {b(5), condemnedRowFor(1, "t5")}};
    std::vector<BlobDelta> deltas;
    for (size_t i = 0; i < kEdges; ++i)
        deltas.push_back(BlobDelta{bh(10 + i), s(1), false, static_cast<uint32_t>(i)});
    /// The second candidate's create is unconfirmed; the other two are confirmed.
    const auto answerFor = [](const BlobRef & ref)
    {
        return ref == bh(3) ? IntentCreateResult{IntentCreate::Unconfirmed, "lost"} : IntentCreateResult{IntentCreate::Created, {}};
    };

    const auto foldWith = [&](const IntentCreates & creates, std::vector<uint8_t> & applied, RetiredMergeResult & rmr)
    {
        InMemoryBackend backend;
        DB::Cas::tests::OperationForTest backend_req(backend);
        Layout layout{"pool"};
        const RunRef gen1 = writeSourceEdgeRun(backend, layout, 1, 0, 0, candidates);
        applied.assign(kEdges, 0);
        std::vector<RunRef> runs2;
        runs2.push_back(foldDeltasIntoGeneration(*backend_req, layout, /*prior_runs*/{gen1}, 2, 0, 0, deltas,
            ShardFoldOptions{
                .current_round = 5,
                .condemn_round = 6,
                .create_intent = creates,
                .out_retired = &rmr,
                .out_applied_by_txn_ordinal = &applied}));
        return (*backend_req).read(runs2.at(0).key, Retry::standard())->bytes;
    };

    std::vector<uint8_t> applied_sync;
    RetiredMergeResult rmr_sync;
    const String sync_run = foldWith(immediateCreates([&](const BlobRef & ref, const CanonicalIdentity &) { return answerFor(ref); }),
                                     applied_sync, rmr_sync);

    /// Each answer is taken later; record how far the merge had gone when the first one was taken.
    std::vector<uint8_t> applied;
    std::optional<size_t> edges_merged_at_first_answer;
    size_t started = 0;
    IntentCreates later;
    later.window = 64;
    later.start = [&](const BlobRef & ref, const CanonicalIdentity &) -> IntentCreateAnswer
    {
        ++started;
        return [&, ref]
        {
            if (!edges_merged_at_first_answer)
                edges_merged_at_first_answer = static_cast<size_t>(std::count(applied.begin(), applied.end(), 1));
            return answerFor(ref);
        };
    };
    RetiredMergeResult rmr;
    const String run = foldWith(later, applied, rmr);

    EXPECT_EQ(started, 3u);
    EXPECT_EQ(run, sync_run) << "the run differs from the one written with answers at once";
    ASSERT_EQ(rmr.still_retired.size(), 3u);
    EXPECT_TRUE(rmr.still_retired[0].delete_pending);
    EXPECT_FALSE(rmr.still_retired[1].delete_pending) << "a row took another create's answer";
    EXPECT_TRUE(rmr.still_retired[2].delete_pending);
    EXPECT_EQ(rmr.confirmed.size(), 2u);
    EXPECT_EQ(rmr.unconfirmed_creates.count, 1u);
    ASSERT_TRUE(edges_merged_at_first_answer.has_value());
    EXPECT_LT(*edges_merged_at_first_answer, kEdges) << "every record of the merge was held behind one unanswered create";
    EXPECT_GT(*edges_merged_at_first_answer, 0u) << "positive control: the merge went on while the answer was outstanding";
}

namespace
{

/// Folds one candidate row stamped `row_round` in a pass stamped 6; returns the creates the rules sent.
size_t foldCandidateStamped(uint64_t row_round)
{
    InMemoryBackend backend;
    DB::Cas::tests::OperationForTest backend_req(backend);
    Layout layout{"pool"};
    const RunRef gen1 = writeSourceEdgeRun(backend, layout, 1, 0, 0, {{b(1), condemnedRowFor(row_round, "t1")}});
    size_t creates = 0;
    std::vector<RunRef> runs2;
    RetiredMergeResult rmr;
    runs2.push_back(foldDeltasIntoGeneration(*backend_req, layout, /*prior_runs*/{gen1}, 2, 0, 0, {},
        ShardFoldOptions{
            .current_round = 6,
            .condemn_round = 6,
            .create_intent = immediateCreates(countingCreate(creates)),
            .out_retired = &rmr}));
    return creates;
}

}

/// A row the pass itself stamped is never decided again in it: deciding it would act on the cut that minted it.
#ifndef DEBUG_OR_SANITIZER_BUILD
TEST(CASThreeCursorMerge, RowStampedByThisPassNeverReachesTheRules)
{
    ASSERT_EQ(foldCandidateStamped(5), 1u) << "positive control: a row of an earlier round is decided";
    try
    {
        foldCandidateStamped(6);
        FAIL() << "a row stamped by this pass reached the rules";
    }
    catch (const DB::Exception & e)
    {
        EXPECT_EQ(e.code(), DB::ErrorCodes::LOGICAL_ERROR);
        EXPECT_NE(e.message().find("minted in this pass"), String::npos) << e.message();
    }
}
#else
TEST(CASThreeCursorMergeDeathTest, RowStampedByThisPassNeverReachesTheRules)
{
    EXPECT_DEATH({ foldCandidateStamped(6); std::_Exit(0); }, "minted in this pass");
}
#endif

/// The legacy confirmed bit stays decodable but is never written: a confirmed create leaves it clear, and a row
/// carried from a run that had it set is written with it clear.
TEST(CASThreeCursorMerge, RowsThisBuildWritesCarryTheLegacyConfirmedBitClear)
{
    InMemoryBackend backend;
    DB::Cas::tests::OperationForTest backend_req(backend);
    Layout layout{"pool"};
    CondemnedRow created = condemnedRowFor(/*condemn_round*/1, "t1");
    created.marker_confirmed = true;
    CondemnedRow carried = condemnedRowFor(/*condemn_round*/1, "t2", /*delete_pending*/true);
    carried.marker_confirmed = true;
    const RunRef gen1 = writeSourceEdgeRun(backend, layout, 1, 0, 0, {{b(1), created}, {b(2), carried}});
    ASSERT_TRUE(decodeRun(*backend_req, gen1).condemned.at(1).second.marker_confirmed) << "positive control: the bit decodes";

    size_t creates = 0;
    std::vector<RunRef> runs2;
    RetiredMergeResult rmr;
    /// No `pending_head`: the pending row is carried unchanged.
    runs2.push_back(foldDeltasIntoGeneration(*backend_req, layout, /*prior_runs*/{gen1}, 2, 0, 0, {},
        ShardFoldOptions{
            .current_round = 5,
            .condemn_round = 6,
            .create_intent = immediateCreates(countingCreate(creates)),
            .out_retired = &rmr}));

    ASSERT_EQ(creates, 1u);
    const DecodedRun out = decodeRun(*backend_req, runs2.at(0));
    ASSERT_EQ(out.condemned.size(), 2u);
    EXPECT_TRUE(out.condemned[0].second.delete_pending) << "positive control: the create was confirmed";
    EXPECT_FALSE(out.condemned[0].second.marker_confirmed);
    EXPECT_TRUE(out.condemned[1].second.delete_pending) << "positive control: the pending row was carried";
    EXPECT_FALSE(out.condemned[1].second.marker_confirmed);
}

TEST(CASThreeCursorMerge, LegacyConfirmedBitDoesNotConfirmTheIntent)
{
    InMemoryBackend backend;
    DB::Cas::tests::OperationForTest backend_req(backend);
    Layout layout{"pool"};
    CondemnedRow row = condemnedRowFor(/*condemn_round*/1, "t1");
    row.marker_confirmed = true;
    const RunRef gen1 = writeSourceEdgeRun(backend, layout, 1, 0, 0, {{b(1), row}});

    size_t creates = 0;
    std::vector<RunRef> runs2;
    RetiredMergeResult rmr;
    runs2.push_back(foldDeltasIntoGeneration(*backend_req, layout, /*prior_runs*/{gen1}, 2, 0, 0, {},
        ShardFoldOptions{
            .current_round = 5,
            .condemn_round = 6,
            .create_intent = immediateCreates(countingCreate(creates, /*confirmed*/0)),
            .out_retired = &rmr}));

    EXPECT_EQ(creates, 1u) << "the bit skipped the create";
    EXPECT_TRUE(rmr.confirmed.empty());
    EXPECT_EQ(rmr.unconfirmed_creates.count, 1u);
    ASSERT_EQ(rmr.still_retired.size(), 1u);
    EXPECT_FALSE(rmr.still_retired.front().delete_pending) << "an unconfirmed create made the row pending";
}

TEST(CASThreeCursorMerge, StandingRowWithoutAnIdentityLeavesWithoutASpare)
{
    InMemoryBackend backend;
    DB::Cas::tests::OperationForTest backend_req(backend);
    Layout layout{"pool"};
    CondemnedRow standing = condemnedRowFor(1, "W/\"t\"", /*delete_pending*/true);
    standing.standing = true;
    ASSERT_FALSE(standing.token.canonical().has_value()) << "positive control: the token names no incarnation";
    const RunRef gen1 = writeSourceEdgeRun(backend, layout, /*gen*/1, 0, 0, {{b(1), standing}});

    std::vector<RunRef> runs2;
    RetiredMergeResult rmr;
    runs2.push_back(foldDeltasIntoGeneration(*backend_req, layout, /*prior_runs*/{gen1}, 2, 0, 0, {},
        {.current_round = 9, .condemn_round = 9, .out_retired = &rmr}));

    EXPECT_TRUE(rmr.still_retired.empty()) << "the row was not dropped";
    EXPECT_TRUE(rmr.spared.empty()) << "a row dropped at zero was reported as a spare";
    EXPECT_TRUE(rmr.intent_deletes.empty());
    EXPECT_EQ(rmr.uncanonical_tokens.count, 1u);
}

namespace
{

PendingHeadHints acceptAllHints(std::vector<BlobRef> & offered)
{
    return PendingHeadHints{
        .offer = [&offered](const BlobRef & ref, const CondemnedRow &)
        {
            offered.push_back(ref);
            return true;
        },
        .take_pending = [](const BlobRef &) {},
        .take_spare = [](const BlobRef &) {}};
}

std::vector<std::pair<UInt128, CondemnedRow>> condemnedRowsOf(const std::vector<BlobRef> & refs)
{
    std::vector<std::pair<UInt128, CondemnedRow>> rows;
    for (size_t i = 0; i < refs.size(); ++i)
        rows.push_back({refs[i].digest.toU128(), condemnedRowFor(1, "t" + std::to_string(i), /*delete_pending*/true)});
    return rows;
}

}

TEST(CASThreeCursorMerge, ScanAheadStopsAtTheRowCap)
{
    InMemoryBackend backend;
    DB::Cas::tests::OperationForTest backend_req(backend);
    Layout layout{"pool"};
    const uint64_t n = kPendingHeadLookaheadRows + 100;
    const RunRef gen1 = writeSourceEdgeRun(backend, layout, 1, 0, 0, condemnedCohort(n, 1, /*delete_pending*/true));

    std::vector<BlobRef> offered;
    const PendingHeadHints hints = acceptAllHints(offered);
    std::optional<size_t> offers_at_first_head;
    const BlobHeadFn head = [&](const BlobRef &)
    {
        if (!offers_at_first_head)
            offers_at_first_head = offered.size();
        return std::optional<Meta>{};
    };

    std::vector<RunRef> with_scan;
    RetiredMergeResult scanned;
    with_scan.push_back(foldDeltasIntoGeneration(*backend_req, layout, {gen1}, 2, 0, 0, {},
        ShardFoldOptions{
            .current_round = 5,
            .condemn_round = 6,
            .pending_head = head,
            .out_retired = &scanned,
            .pending_hints = &hints}));

    ASSERT_TRUE(offers_at_first_head.has_value());
    EXPECT_EQ(*offers_at_first_head, kPendingHeadLookaheadRows + 1) << "the current row and a full buffer, no more";
    EXPECT_EQ(offered.size(), n - 1) << "every row after the first is offered";
    EXPECT_EQ(std::set<BlobRef>(offered.begin(), offered.end()).size(), n - 1) << "a row was offered twice";

    /// Same output key: a different run would fail the write-once replay with `CORRUPTED_DATA`.
    std::vector<RunRef> without_scan;
    RetiredMergeResult plain;
    without_scan.push_back(foldDeltasIntoGeneration(*backend_req, layout, {gen1}, 2, 0, 0, {},
        {.current_round = 5, .condemn_round = 6, .pending_head = kPendingBodyAbsent, .out_retired = &plain}));
    ASSERT_EQ(with_scan.size(), 1u);
    ASSERT_EQ(without_scan.size(), 1u);
    EXPECT_EQ(with_scan.front().checksum, without_scan.front().checksum);
    EXPECT_EQ(scanned.still_retired.size(), plain.still_retired.size());
}

TEST(CASThreeCursorMerge, ScanAheadStopsAtTheSegmentEnd)
{
    DB::Cas::tests::CountingBackend backend;
    DB::Cas::tests::OperationForTest backend_req(backend);
    Layout layout{"pool"};
    std::vector<BlobRef> refs;
    for (uint64_t i = 1; i <= 20; ++i)
        refs.push_back(bh(i));
    std::sort(refs.begin(), refs.end());
    const std::vector<BlobRef> first(refs.begin(), refs.begin() + 10);
    const std::vector<BlobRef> second(refs.begin() + 10, refs.end());
    const RunRef seg1 = writeSourceEdgeRun(backend, layout, 1, 0, 0, condemnedRowsOf(first));
    const RunRef seg2 = writeSourceEdgeRun(backend, layout, 1, 1, 0, condemnedRowsOf(second));
    const std::set<BlobRef> second_set(second.begin(), second.end());

    size_t early_offers = 0;
    std::vector<BlobRef> offered;
    const PendingHeadHints hints{
        .offer = [&](const BlobRef & ref, const CondemnedRow &)
        {
            if (second_set.contains(ref) && backend.getStreamCount(seg2.key) == 0)
                ++early_offers;
            offered.push_back(ref);
            return true;
        },
        .take_pending = [](const BlobRef &) {},
        .take_spare = [](const BlobRef &) {}};
    std::vector<BlobRef> headed;
    size_t segment_two_opens_at_first_head = 0;
    const BlobHeadFn head = [&](const BlobRef & ref)
    {
        /// The first HEAD runs right after the scan that offered segment 1's rows, which fit the row cap.
        if (headed.empty())
            segment_two_opens_at_first_head = backend.getStreamCount(seg2.key);
        headed.push_back(ref);
        return std::optional<Meta>{};
    };

    std::vector<RunRef> with_scan;
    with_scan.push_back(foldDeltasIntoGeneration(*backend_req, layout, {seg1, seg2}, 2, 0, 0, {},
        {.current_round = 5, .condemn_round = 6, .pending_head = head, .pending_hints = &hints}));

    EXPECT_EQ(segment_two_opens_at_first_head, 0u) << "the scan opened segment 2 before the merge reached it";
    EXPECT_EQ(early_offers, 0u) << "a row of segment 2 was offered before the merge opened it";
    EXPECT_EQ(offered.size(), 19u) << "offers resume once the next segment is open";
    ASSERT_EQ(headed.size(), 20u);
    EXPECT_EQ(headed[9], first.back()) << "the last row of segment 1 is still read";

    std::vector<RunRef> without_scan;
    without_scan.push_back(foldDeltasIntoGeneration(*backend_req, layout, {seg1, seg2}, 2, 0, 0, {},
        {.current_round = 5, .condemn_round = 6, .pending_head = kPendingBodyAbsent}));
    EXPECT_EQ(with_scan.front().checksum, without_scan.front().checksum);
}

TEST(CASThreeCursorMerge, ScanAheadKeepsRowAndChecksumValidation)
{
    Layout layout{"pool"};
    std::vector<BlobRef> unused;
    const PendingHeadHints hints = acceptAllHints(unused);

    for (const bool with_scan : {false, true})
    {
        SCOPED_TRACE(with_scan);
        /// A duplicate sentinel in the middle of a condemned cohort.
        {
            InMemoryBackend backend;
            DB::Cas::tests::OperationForTest op(backend);
            std::vector<SourceEdgeRecord> recs;
            for (uint64_t i = 1; i <= 6; ++i)
                recs.push_back(condemnedRec(b(i), condemnedRowFor(1, "t" + std::to_string(i), /*delete_pending*/true)));
            std::sort(recs.begin(), recs.end(), [](const SourceEdgeRecord & l, const SourceEdgeRecord & r) { return l.ref < r.ref; });
            const SourceEdgeRecord duplicate = recs[2];
            recs.insert(recs.begin() + 3, duplicate);
            DB::WriteBufferFromOwnString out;
            SourceEdgeRunWriter writer(out);
            for (const SourceEdgeRecord & rec : recs)
                writer.append(rec);
            writer.finish();
            out.finalize();
            const String bytes = out.str();
            const RunRef bad{.key = layout.blobTargetRunKey(1, 0, 0, 0), .checksum = sourceEdgeRunChecksum(bytes),
                             .shard = 0, .key_generation = 1};
            (*op).create(bad.key, bytes, Retry::once());

            std::vector<RunRef> out_runs;
            DB::Cas::tests::expectThrowsCode(DB::ErrorCodes::CORRUPTED_DATA, [&]
            {
                out_runs.push_back(foldDeltasIntoGeneration(*op, layout, {bad}, 2, 0, 0, {},
                    ShardFoldOptions{
                        .current_round = 5,
                        .condemn_round = 6,
                        .pending_head = kPendingBodyAbsent,
                        .pending_hints = with_scan ? &hints : nullptr}));
            });
            EXPECT_TRUE(out_runs.empty());
            EXPECT_FALSE((*op).read(layout.blobTargetRunKey(2, 0, 0, 0), Retry::once()).has_value()) << "a successor run was written";
        }
        /// A bad seal checksum on segment 2.
        {
            InMemoryBackend backend;
            DB::Cas::tests::OperationForTest op(backend);
            std::vector<BlobRef> refs;
            for (uint64_t i = 1; i <= 8; ++i)
                refs.push_back(bh(i));
            std::sort(refs.begin(), refs.end());
            const RunRef seg1 = writeSourceEdgeRun(backend, layout, 1, 0, 0,
                condemnedRowsOf(std::vector<BlobRef>(refs.begin(), refs.begin() + 4)));
            RunRef seg2 = writeSourceEdgeRun(backend, layout, 1, 1, 0,
                condemnedRowsOf(std::vector<BlobRef>(refs.begin() + 4, refs.end())));
            seg2.checksum = seg2.checksum + 1;

            std::vector<RunRef> out_runs;
            DB::Cas::tests::expectThrowsCode(DB::ErrorCodes::CORRUPTED_DATA, [&]
            {
                out_runs.push_back(foldDeltasIntoGeneration(*op, layout, {seg1, seg2}, 2, 0, 0, {},
                    ShardFoldOptions{
                        .current_round = 5,
                        .condemn_round = 6,
                        .pending_head = kPendingBodyAbsent,
                        .pending_hints = with_scan ? &hints : nullptr}));
            });
            EXPECT_TRUE(out_runs.empty());
            EXPECT_FALSE((*op).read(layout.blobTargetRunKey(2, 0, 0, 0), Retry::once()).has_value()) << "a successor run was written";
        }
    }
}

namespace
{

CondemnedRow standingRow()
{
    CondemnedRow row;
    row.delete_pending = true;
    row.marker_confirmed = true;
    row.standing = true;
    row.token = PersistedEtag{"emulated", "tok"};
    row.size = 12;
    row.condemn_round = 5;
    return row;
}

}

TEST(CASBlobInDegree, RunViewAndRetiredEntryCarryStanding)
{
    InMemoryBackend backend;
    DB::Cas::tests::OperationForTest backend_req(backend);
    Layout layout{"pool"};
    const RunRef gen1 = writeSourceEdgeRun(backend, layout, 1, 0, 0,
        {{b(1), standingRow()}, {b(2), standingRow()}});

    /// Standing survives the NDJSON round trip.
    {
        const DecodedRun in_run = decodeRun(*backend_req, gen1);
        ASSERT_EQ(in_run.condemned.size(), 2u);
        EXPECT_TRUE(in_run.condemned[0].second.standing);
    }

    /// A suppressed pass decides no intent delete: both rows carry into the next run as `standing`.
    std::vector<RunRef> runs2;
    RetiredMergeResult rmr;
    runs2.push_back(foldDeltasIntoGeneration(*backend_req, layout, /*prior_runs*/{gen1}, 2, 0, 0, {},
        ShardFoldOptions{
            .current_round = 9,
            .condemn_round = 9,
            .pending_head = kPendingBodyAbsent,
            .out_retired = &rmr,
            .suppress_destructive = true}));

    EXPECT_TRUE(rmr.intent_deletes.empty());
    ASSERT_EQ(rmr.still_retired.size(), 2u);
    EXPECT_TRUE(rmr.still_retired[0].standing);

    const DecodedRun out = decodeRun(*backend_req, runs2[0]);
    ASSERT_EQ(out.condemned.size(), 2u);
    EXPECT_TRUE(out.condemned[0].second.standing);
}

namespace
{

/// One fold of `nominations` over an empty prior generation; `peek_head` reads every body absent.
void foldNominations(const Layout & layout, std::vector<Layout::ParsedBlobIntentKey> nominations, bool suppressed)
{
    InMemoryBackend backend;
    DB::Cas::tests::OperationForTest backend_req(backend);
    std::vector<RunRef> runs;
    runs.push_back(foldDeltasIntoGeneration(*backend_req, layout, /*prior_runs*/{}, 1, 0, 0, {},
        ShardFoldOptions{
            .current_round = 2,
            .condemn_round = 2,
            .peek_head = kPendingBodyAbsent,
            .pending_head = kPendingBodyAbsent,
            .suppress_destructive = suppressed,
            .intent_nominations = std::move(nominations)}));
}

Layout::ParsedBlobIntentKey nominationOf(uint64_t blob, const CanonicalIdentity & identity)
{
    return Layout::ParsedBlobIntentKey{bh(blob), identity};
}

const CanonicalIdentity kTaggedIdentity = *canonicalIdentity(Dialect::Emulated, "t");
const CanonicalIdentity kTaglessIdentity{Dialect::Emulated, ""};

}

#ifndef DEBUG_OR_SANITIZER_BUILD
TEST(CASThreeCursorMerge, NominationWithoutATagIsALogicalError)
{
    Layout layout{"pool"};
    DB::Cas::tests::expectThrowsCode(DB::ErrorCodes::LOGICAL_ERROR,
        [&] { foldNominations(layout, {nominationOf(1, kTaglessIdentity)}, /*suppressed*/false); });
    EXPECT_NO_THROW(foldNominations(layout, {nominationOf(1, kTaggedIdentity)}, /*suppressed*/false))
        << "positive control: the same fold takes a tagged nomination";
}

TEST(CASThreeCursorMerge, NominationInASuppressedRoundIsALogicalError)
{
    Layout layout{"pool"};
    DB::Cas::tests::expectThrowsCode(DB::ErrorCodes::LOGICAL_ERROR,
        [&] { foldNominations(layout, {nominationOf(1, kTaggedIdentity)}, /*suppressed*/true); });
    EXPECT_NO_THROW(foldNominations(layout, {}, /*suppressed*/true)) << "positive control: no nomination, no refusal";
}
#else
TEST(CASThreeCursorMergeDeathTest, NominationWithoutATagIsALogicalError)
{
    Layout layout{"pool"};
    EXPECT_DEATH({ foldNominations(layout, {nominationOf(1, kTaglessIdentity)}, /*suppressed*/false); }, "carries no tag");
}

TEST(CASThreeCursorMergeDeathTest, NominationInASuppressedRoundIsALogicalError)
{
    Layout layout{"pool"};
    EXPECT_DEATH({ foldNominations(layout, {nominationOf(1, kTaggedIdentity)}, /*suppressed*/true); }, "suppressed round");
}
#endif
