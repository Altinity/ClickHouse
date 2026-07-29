#include <gtest/gtest.h>

#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Formats/CasByteBudget.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Formats/CasFoldSealFormat.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Formats/CasGcStateFormat.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Formats/CasRefCkptFormat.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Formats/CasRefLogFormat.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Gc/CasGc.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Gc/CasGcShardPlan.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasPool.h>
#include <Common/ProfileEvents.h>
#include "cas_test_helpers.h"

#include <cstring>
#include <functional>
#include <limits>
#include <mutex>
#include <set>

/// DURABLE HOLDS (spec 2026-07-27 "ref chain complete cut" §5).
///
/// A namespace whose ref-log walk meets an IMPOSSIBLE shape stops there, and that stop has to survive
/// the round. Before this task the stop was a single bit — `classification == 4` — and everything that
/// explained it (what went wrong, and exactly WHERE) lived in a log line and an in-memory anomaly, both
/// gone by the next round. That is not enough for three separate reasons:
///
///   * the next round could not RETRY the exact position, so a hold only survived while the round's
///     hint happened to keep mentioning the namespace;
///   * the hold could be cleared by an ABSENT — precisely the observation a lying store produces, and
///     precisely the shape that made the hold necessary in the first place;
///   * REBUILD rewrote coverage from owner state and silently dropped every hold, handing back a
///     baseline that looked proven when it was not.
///
/// So the hold is now DURABLE and STRICTLY GRAMMARED: `{reason, offending_position, retry_count,
/// next_retry_round}` present if and only if `classification == 4`, rejected in both directions
/// otherwise. It rides the seal across rounds — including rounds whose hint omits the namespace
/// entirely — and across REBUILD, and it clears by exactly ONE event: the fold resolving the offending
/// position and that result being adopted in `gc/state`.
///
/// The carried hold is also a WITNESS, and a better one than the listing: it is durable proof that the
/// walk once reached that position, so an absent below it is a gap rather than a frontier no matter
/// what the hint says this round. That is what makes "retry the exact offending position" work for a
/// hold that sits above an epoch boundary.

using namespace DB::Cas;
using namespace DB::Cas::tests;

namespace DB::ErrorCodes
{
extern const int CORRUPTED_DATA;
extern const int LIMIT_EXCEEDED;
}

namespace ProfileEvents
{
extern const Event CasGcRebuildVirginByEnumeration;
}

namespace
{

const UInt128 kGc = hexToU128("00000000000000000000000000000001");

/// ===================== FIXTURES =====================

/// A backend that hides keys from every LIST while serving them by exact key (the observed lying-store
/// shape) AND counts reads. The hold tests need both: the hint has to go quiet while the exact GET the
/// hold forces stays observable.
class HintHoleCountingBackend : public CountingBackend
{
public:
    void hide(const String & key)
    {
        std::lock_guard lock(m);
        hidden.insert(key);
    }

    size_t holesServed() const
    {
        std::lock_guard lock(m);
        return served;
    }

    ListPage list(const String & prefix, const String & cursor, size_t limit) override
    {
        ListPage page = CountingBackend::list(prefix, cursor, limit);
        std::lock_guard lock(m);
        if (hidden.empty())
            return page;
        const size_t before = page.keys.size();
        std::erase_if(page.keys, [&](const ListedKey & k) { return hidden.contains(k.key); });
        if (page.keys.size() != before)
            ++served;
        return page;
    }

private:
    mutable std::mutex m;
    std::set<String> hidden;
    size_t served = 0;
};

/// Write the namespace's `_ckpt` naming `checkpoint` as its snapshot base, through the real codec — the
/// fold's second witness source is a decode of exactly these bytes, so a hand-rolled body would prove
/// nothing about the object the writers actually publish.
void writeCkptAt(
    Backend & backend, const Layout & layout, const RootNamespace & ns, const RefTxnId & checkpoint)
{
    backend.putIfAbsent(layout.refCkptKey(ns),
                        encodeRefCkpt(RefCkpt{.life_epoch = std::nullopt,
                                              .checkpoint_snapshot_id = checkpoint,
                                              .last_epoch_seal = std::nullopt}));
}

/// The newest fold seal, scanning downward from the adopted generation (a completed round's gc/state
/// points at the recheck generation).
std::optional<CasFoldSeal> newestSeal(Backend & backend, const Layout & layout)
{
    const uint64_t gen = currentGenerationOf(backend, layout);
    const uint64_t attempt = currentAttemptOf(backend, layout);
    for (uint64_t g = gen; ; --g)
    {
        if (const auto got = backend.get(layout.foldSealKey(g, attempt)))
            return decodeFoldSeal(got->bytes);
        if (g == 0)
            return std::nullopt;
    }
}

std::optional<ShardCoverage> coverageOf(Backend & backend, const Layout & layout, const RootNamespace & ns)
{
    const auto seal = newestSeal(backend, layout);
    if (!seal)
        return std::nullopt;
    const auto it = seal->per_ns_shard.find(cursorKeyForTest(ns, /*shard*/0));
    if (it == seal->per_ns_shard.end())
        return std::nullopt;
    return it->second;
}

/// The cursor `ns` was sealed at, or `{0, 0}` when the round sealed NO row for it at all. It never
/// dereferences a disengaged optional: a test that aborts the process takes every test after it in the
/// binary down with it, and "there is no coverage row" is exactly the shape a regression in the hold
/// carry produces — so it has to read as a failed expectation, not as a crash that hides the rest.
RefTxnId sealedCursorOf(Backend & backend, const Layout & layout, const RootNamespace & ns)
{
    const auto cov = coverageOf(backend, layout, ns);
    EXPECT_TRUE(cov.has_value()) << "no coverage row for " << ns.string();
    return cov ? cov->last_folded_ref_id : RefTxnId{};
}

/// The coverage row a round MUST have sealed for `ns`, held. Fails the test rather than returning an
/// empty optional, so every caller below reads a real hold.
RefHold holdOf(Backend & backend, const Layout & layout, const RootNamespace & ns)
{
    const auto cov = coverageOf(backend, layout, ns);
    EXPECT_TRUE(cov.has_value()) << "no coverage row for " << ns.string();
    if (!cov)
        return RefHold{};
    EXPECT_EQ(cov->classification, 4) << "a held namespace is classification 4";
    EXPECT_TRUE(cov->hold.has_value()) << "classification 4 without a hold is the forbidden shape";
    return cov->hold ? *cov->hold : RefHold{};
}

/// A seal carrying exactly one held coverage row, with every numeric at its maximum and a coverage key
/// that needs JSON escaping — the worst case the per-row line reservation has to survive.
CasFoldSeal maximalHoldSeal(const String & map_key)
{
    CasFoldSeal seal;
    seal.generation = std::numeric_limits<uint64_t>::max();
    seal.parent_generation = std::numeric_limits<uint64_t>::max();
    ShardCoverage cov;
    cov.classification = 4;
    cov.folded_token = Token{"\"\\\n", TokenType::ETag};   /// every byte of this token escapes
    cov.last_folded_ref_id = RefTxnId{std::numeric_limits<uint64_t>::max(),
                                      std::numeric_limits<uint64_t>::max()};
    cov.hold = RefHold{.reason = HoldReason::UnconsumedSealCrossing,   /// the longest reason word
                       .offending_position = RefTxnId{std::numeric_limits<uint64_t>::max(),
                                                      std::numeric_limits<uint64_t>::max()},
                       .retry_count = std::numeric_limits<uint32_t>::max(),
                       .next_retry_round = std::numeric_limits<uint64_t>::max()};
    seal.per_ns_shard[map_key] = cov;
    return seal;
}

/// The `cov` line of an encoded seal (line 3: header, meta, then the single record).
String covLineOf(const String & encoded)
{
    size_t begin = encoded.find('\n') + 1;   /// past the header
    begin = encoded.find('\n', begin) + 1;   /// past the meta line
    return encoded.substr(begin, encoded.find('\n', begin) - begin);
}

/// A one-row seal whose `cov` line is EXACTLY `target_bytes` long, built by padding the coverage key
/// with plain ASCII (one padding byte == one encoded byte).
CasFoldSeal holdSealWithCovLineOfExactly(uint64_t target_bytes)
{
    const String probe_key = "ns/0";
    const uint64_t base = covLineOf(encodeFoldSeal(maximalHoldSeal(probe_key))).size();
    EXPECT_LE(base, target_bytes) << "the unpadded maximal hold row already exceeds the target";
    return maximalHoldSeal(probe_key + String(target_bytes - base, 'a'));
}

}

/// ===================== THE SHARED BYTE ARITHMETIC =====================
///
/// Two caps, two predicates, one place they are computed. Stage B's catalog reuses THESE functions for
/// its additive "does one more entry still fit" question, so their boundary behaviour is pinned here
/// rather than re-derived per format: a cap is the largest PERMITTED value (equality fits), and every
/// sum saturates, because a wrapped sum answers "fits" for an object that does not — turning an
/// overflow into a durable object nothing can read.
TEST(CasGcHoldGrammarBudget, BothPredicatesAcceptEqualityAndRefuseOneMore)
{
    static_assert(fitsLineCap(64, 64));
    static_assert(!fitsLineCap(65, 64));
    static_assert(fitsObjectCap(40, 24, 64));
    static_assert(!fitsObjectCap(40, 25, 64));

    EXPECT_TRUE(fitsLineCap(64, 64));
    EXPECT_FALSE(fitsLineCap(65, 64));
    EXPECT_TRUE(fitsObjectCap(64, 0, 64));
    EXPECT_FALSE(fitsObjectCap(64, 1, 64));

    /// A cap of 0 means the format declares none (a streamed object never materialized whole).
    EXPECT_TRUE(fitsLineCap(std::numeric_limits<uint64_t>::max(), 0));
    EXPECT_TRUE(fitsObjectCap(std::numeric_limits<uint64_t>::max(), 1, 0));
}

TEST(CasGcHoldGrammarBudget, SumsSaturateInsteadOfWrapping)
{
    constexpr uint64_t kMax = std::numeric_limits<uint64_t>::max();
    static_assert(addByteBudget(kMax, 1) == kMax);
    static_assert(addByteBudget(kMax, kMax) == kMax);
    static_assert(addByteBudget(3, 4) == 7);

    /// The predicate that matters: a reservation that would wrap must REFUSE, not report a tiny sum.
    EXPECT_FALSE(fitsObjectCap(kMax, 2, 256 * 1024 * 1024));
}

/// ===================== THE STRICT CLASSIFICATION-4 GRAMMAR =====================

TEST(CasGcHoldGrammar, EveryHoldReasonRoundTrips)
{
    for (const HoldReason reason : {HoldReason::GapBelowWitness, HoldReason::UnconsumedSealCrossing,
                                    HoldReason::WitnessDisappeared, HoldReason::BodyUndecodable,
                                    HoldReason::ManifestBodyMissing})
    {
        CasFoldSeal seal;
        seal.generation = 3;
        seal.parent_generation = 2;
        ShardCoverage cov;
        cov.classification = 4;
        cov.folded_token = Token{"t", TokenType::ETag};
        cov.last_folded_ref_id = RefTxnId{4, 5};
        cov.hold = RefHold{.reason = reason, .offending_position = RefTxnId{4, 6},
                           .retry_count = 7, .next_retry_round = 99};
        seal.per_ns_shard["ns/0"] = cov;

        const CasFoldSeal back = decodeFoldSeal(encodeFoldSeal(seal));
        EXPECT_EQ(back, seal) << "hold reason " << static_cast<int>(reason);
        ASSERT_TRUE(back.per_ns_shard.at("ns/0").hold.has_value());
        EXPECT_EQ(back.per_ns_shard.at("ns/0").hold->reason, reason);
    }
}

TEST(CasGcHoldGrammar, AHoldOnAnyOtherClassificationIsRefusedBothWays)
{
    CasFoldSeal seal;
    seal.generation = 1;
    ShardCoverage cov;
    cov.classification = 2;   /// folded — and yet carrying a hold
    cov.hold = RefHold{.reason = HoldReason::GapBelowWitness, .offending_position = RefTxnId{1, 2},
                       .retry_count = 0, .next_retry_round = 1};
    seal.per_ns_shard["ns/0"] = cov;

    /// The ENCODER refuses: a durable seal must never contain the contradiction in the first place.
    expectThrowsCode(DB::ErrorCodes::CORRUPTED_DATA, [&] { encodeFoldSeal(seal); });

    /// And so does the DECODER, for bytes some other producer wrote. Built by demoting a legitimate
    /// held row's classification, so the hold fields are exactly the ones the encoder emits.
    cov.classification = 4;
    seal.per_ns_shard["ns/0"] = cov;
    String text = encodeFoldSeal(seal);
    const size_t at = text.find("\"cls\":4");
    ASSERT_NE(at, String::npos);
    text[at + 6] = '2';
    expectThrowsCode(DB::ErrorCodes::CORRUPTED_DATA, [&] { decodeFoldSeal(text); });
}

TEST(CasGcHoldGrammar, ClassificationFourWithoutAHoldIsRefusedBothWays)
{
    CasFoldSeal seal;
    seal.generation = 1;
    ShardCoverage cov;
    cov.classification = 4;
    cov.last_folded_ref_id = RefTxnId{1, 1};
    seal.per_ns_shard["ns/0"] = cov;   /// held, with nothing saying why or where

    expectThrowsCode(DB::ErrorCodes::CORRUPTED_DATA, [&] { encodeFoldSeal(seal); });

    /// Every single hold field is REQUIRED: dropping any one of them is corruption, not a default.
    cov.hold = RefHold{.reason = HoldReason::BodyUndecodable, .offending_position = RefTxnId{1, 2},
                       .retry_count = 3, .next_retry_round = 4};
    seal.per_ns_shard["ns/0"] = cov;
    const String whole = encodeFoldSeal(seal);
    for (const String & field : {String("\"hr\":\"body_undecodable\""), String("\"hpe\":\"1\""),
                                 String("\"hps\":\"2\""), String("\"hrc\":3"), String("\"hnr\":\"4\"")})
    {
        SCOPED_TRACE("without " + field);
        const size_t at = whole.find(field);
        ASSERT_NE(at, String::npos) << "the encoder does not emit " << field;
        String without = whole;
        without.erase(at - 1, field.size() + 1);   /// the field and the ',' before it
        expectThrowsCode(DB::ErrorCodes::CORRUPTED_DATA, [&] { decodeFoldSeal(without); });
    }
}

TEST(CasGcHoldGrammar, DuplicateHoldKeyIsCorruptedData)
{
    CasFoldSeal seal;
    seal.generation = 1;
    ShardCoverage cov;
    cov.classification = 4;
    cov.hold = RefHold{.reason = HoldReason::GapBelowWitness, .offending_position = RefTxnId{1, 2},
                       .retry_count = 0, .next_retry_round = 5};
    seal.per_ns_shard["ns/0"] = cov;

    const String whole = encodeFoldSeal(seal);
    const String field = "\"hr\":\"gap_below_witness\"";
    const size_t at = whole.find(field);
    ASSERT_NE(at, String::npos);
    /// The same key twice, with a DIFFERENT value: last-wins would silently rewrite the reason.
    String doubled = whole;
    doubled.insert(at, "\"hr\":\"witness_disappeared\",");
    expectThrowsCode(DB::ErrorCodes::CORRUPTED_DATA, [&] { decodeFoldSeal(doubled); });
}

TEST(CasGcHoldGrammar, UnknownHoldReasonWordIsCorruptedData)
{
    CasFoldSeal seal;
    seal.generation = 1;
    ShardCoverage cov;
    cov.classification = 4;
    cov.hold = RefHold{.reason = HoldReason::GapBelowWitness, .offending_position = RefTxnId{1, 2},
                       .retry_count = 0, .next_retry_round = 5};
    seal.per_ns_shard["ns/0"] = cov;

    String text = encodeFoldSeal(seal);
    const size_t at = text.find("gap_below_witness");
    ASSERT_NE(at, String::npos);
    text.replace(at, strlen("gap_below_witness"), "gap_below_witnesX");
    expectThrowsCode(DB::ErrorCodes::CORRUPTED_DATA, [&] { decodeFoldSeal(text); });
}

/// ===================== THE TWO CAPS, KEPT DISTINCT =====================
///
/// (a) The LINE cap bounds ONE record. A `cov` row that exceeds it produces an object that can never be
/// decoded again, so the writer refuses it rather than persisting it — and the reader refuses it too,
/// for bytes that reached the store some other way.
TEST(CasGcHoldGrammar, MaximalHoldRowFitsTheLineCapAndOneMoreByteDoesNot)
{
    const uint64_t line_cap = foldSealCaps().line_cap;
    ASSERT_GT(line_cap, 0u);

    /// AT the cap: accepted, and it round-trips — equality is inside the budget, on both sides.
    const CasFoldSeal at_cap = holdSealWithCovLineOfExactly(line_cap);
    const String encoded = encodeFoldSeal(at_cap);
    ASSERT_EQ(covLineOf(encoded).size(), line_cap);
    EXPECT_EQ(decodeFoldSeal(encoded), at_cap);

    /// ONE byte of payload growth: the reader refuses the line...
    String over = encoded;
    over.insert(over.find("\"key\":\"ns/0") + 8, "a");
    ASSERT_EQ(covLineOf(over).size(), line_cap + 1);
    expectThrowsCode(DB::ErrorCodes::CORRUPTED_DATA, [&] { decodeFoldSeal(over); });

    /// ...and the WRITER refuses to produce it, so the unreadable object is never created.
    expectThrowsCode(DB::ErrorCodes::LIMIT_EXCEEDED,
        [&] { encodeFoldSeal(holdSealWithCovLineOfExactly(line_cap + 1)); });
}

/// (b) The OBJECT cap bounds the whole seal. Nothing on the fold-seal READ path enforces it (the seal
/// is read raw, never through `openObject`), so an oversized PUT would leave a durable seal that no
/// later round can decode — unrecoverable. The gate therefore sits before the bytes are handed out, and
/// equality is still accepted: the cap is the largest permitted size, not the first forbidden one.
TEST(CasGcHoldGrammar, ObjectCapAcceptsEqualityAndRefusesOneMoreByte)
{
    const uint64_t object_cap = foldSealCaps().object_cap;
    ASSERT_EQ(object_cap, 256u * 1024 * 1024);

    EXPECT_NO_THROW(checkFoldSealObjectBytes(object_cap - 1));
    EXPECT_NO_THROW(checkFoldSealObjectBytes(object_cap));
    expectThrowsCode(DB::ErrorCodes::LIMIT_EXCEEDED, [&] { checkFoldSealObjectBytes(object_cap + 1); });

    /// An ordinary seal is nowhere near it, so the gate costs a comparison and changes nothing.
    EXPECT_NO_THROW(encodeFoldSeal(maximalHoldSeal("ns/0")));
}

/// ===================== HOLDS ARE CREATED WITH AN EXACT POSITION =====================

TEST(CasGcHoldGrammar, GapBelowWitnessNamesTheExactAbsentPosition)
{
    auto backend = std::make_shared<InMemoryBackend>();
    auto store = openPoolForTest(backend, /*gc_fold_max_defer_rounds=*/0);
    const Layout & layout = store->layout();
    const RootNamespace ns{"00/aa@cas@"};

    publishAt(*backend, layout, ns, RefTxnId{1, 1}, "ref_1", 1, DB::UInt128(1), /*birth=*/true);
    publishAt(*backend, layout, ns, RefTxnId{1, 2}, "ref_2", 2, DB::UInt128(2));
    /// {1,3} never existed; {1,4} is durable AND listed, so the gap is impossible under contiguity.
    publishAt(*backend, layout, ns, RefTxnId{1, 4}, "ref_4", 4, DB::UInt128(4));

    Gc gc(store, kGc);
    ASSERT_TRUE(gc.runRegularRound().acquired_lease);

    const RefHold hold = holdOf(*backend, layout, ns);
    EXPECT_EQ(hold.reason, HoldReason::GapBelowWitness);
    EXPECT_EQ(hold.offending_position, (RefTxnId{1, 3}));
    EXPECT_EQ(hold.retry_count, 0u) << "the round that creates a hold has retried nothing yet";
    EXPECT_GT(hold.next_retry_round, 0u);
    EXPECT_EQ(sealedCursorOf(*backend, layout, ns), (RefTxnId{1, 2}));
}

TEST(CasGcHoldGrammar, UnconsumedSealCrossingNamesTheAbsentPosition)
{
    auto backend = std::make_shared<InMemoryBackend>();
    auto store = openPoolForTest(backend, /*gc_fold_max_defer_rounds=*/0);
    const Layout & layout = store->layout();
    const RootNamespace ns{"00/aa@cas@"};

    publishAt(*backend, layout, ns, RefTxnId{1, 1}, "ref_1", 1, DB::UInt128(1), /*birth=*/true);
    /// Epoch 1 ends at {1,1} with NO seal, and epoch 2 chains to a seal at {1,3} that this cursor
    /// never consumed (and that does not exist). The nearest witness above the absent {1,2} therefore
    /// sits in another epoch, and the crossing has nothing to prove itself from.
    publishAt(*backend, layout, ns, RefTxnId{2, 1}, "ref_2", 2, DB::UInt128(2),
              /*birth=*/false, /*prev_epoch_seal=*/RefTxnId{1, 3});

    Gc gc(store, kGc);
    ASSERT_TRUE(gc.runRegularRound().acquired_lease);

    const RefHold hold = holdOf(*backend, layout, ns);
    EXPECT_EQ(hold.reason, HoldReason::UnconsumedSealCrossing);
    EXPECT_EQ(hold.offending_position, (RefTxnId{1, 2})) << "the hold names the position that read absent";
    EXPECT_EQ(sealedCursorOf(*backend, layout, ns), (RefTxnId{1, 1}));
    EXPECT_EQ(inDegreeOf(*backend, layout, DB::UInt128(2)), 0)
        << "nothing beyond the unproven boundary may fold";
}

TEST(CasGcHoldGrammar, UndecodableBodyNamesTheRecordItCouldNotRead)
{
    auto backend = std::make_shared<InMemoryBackend>();
    auto store = openPoolForTest(backend, /*gc_fold_max_defer_rounds=*/0);
    const Layout & layout = store->layout();
    const RootNamespace ns{"00/aa@cas@"};

    publishAt(*backend, layout, ns, RefTxnId{1, 1}, "ref_1", 1, DB::UInt128(1), /*birth=*/true);
    backend->putIfAbsent(layout.refLogKey(ns, RefTxnId{1, 2}), "this is not a cas_ref_log object");

    Gc gc(store, kGc);
    ASSERT_TRUE(gc.runRegularRound().acquired_lease);

    const RefHold hold = holdOf(*backend, layout, ns);
    EXPECT_EQ(hold.reason, HoldReason::BodyUndecodable);
    EXPECT_EQ(hold.offending_position, (RefTxnId{1, 2}));
}

/// The fold barrier is a hold too, and it is the ONE hold whose ordinary cause is benign: a writer that
/// has appended its precommit record but not yet finished uploading the manifest body. It gets the same
/// durable treatment as the corruption shapes because it stops the namespace the same way — and because
/// a barrier that is durably named is one an operator can distinguish from a wedge.
TEST(CasGcHoldGrammar, MissingManifestBodyBarrierIsADurableHold)
{
    auto backend = std::make_shared<InMemoryBackend>();
    auto store = openPoolForTest(backend, /*gc_fold_max_defer_rounds=*/0);
    const Layout & layout = store->layout();
    const RootNamespace ns{"00/aa@cas@"};

    publishAt(*backend, layout, ns, RefTxnId{1, 1}, "ref_1", 1, DB::UInt128(1), /*birth=*/true);
    publishAt(*backend, layout, ns, RefTxnId{1, 2}, "ref_2", 2, DB::UInt128(2));
    deleteManifestBody(*backend, layout,
                       ManifestId{ns, ManifestRef{.writer_epoch = 1, .build_sequence = 2, .manifest_ordinal = 1}});

    Gc gc(store, kGc);
    ASSERT_TRUE(gc.runRegularRound().acquired_lease);

    const RefHold hold = holdOf(*backend, layout, ns);
    EXPECT_EQ(hold.reason, HoldReason::ManifestBodyMissing);
    EXPECT_EQ(hold.offending_position, (RefTxnId{1, 2})) << "the hold names the LOG whose edges could not fold";
    EXPECT_EQ(sealedCursorOf(*backend, layout, ns), (RefTxnId{1, 1}));
}

/// An above-cursor record that answered one GET and then stopped answering is CORRUPTION, not a
/// frontier: nothing may legitimately remove an object above the fold cursor. It is the one hold shape
/// that no amount of waiting can clear, and naming it durably is what stops a later round from reading
/// the same namespace as quiet and granting it a frontier proof.
TEST(CasGcHoldGrammar, AWitnessThatStopsAnsweringIsWitnessDisappeared)
{
    /// Answers on odd-numbered reads and 404s on even ones: `crossFromSeal` proves the position, and
    /// the walk's own GET of it then fails.
    class AlternatingGetBackend : public InMemoryBackend
    {
    public:
        using DB::Cas::Backend::get;
        String flaky;
        size_t reads = 0;

        std::optional<GetResult> get(const String & key, Range range) override
        {
            if (key == flaky && ++reads % 2 == 0)
                return std::nullopt;
            return InMemoryBackend::get(key, range);
        }
    };

    auto backend = std::make_shared<AlternatingGetBackend>();
    auto store = openPoolForTest(backend, /*gc_fold_max_defer_rounds=*/0);
    const Layout & layout = store->layout();
    const RootNamespace ns{"00/aa@cas@"};

    publishAt(*backend, layout, ns, RefTxnId{1, 1}, "ref_1", 1, DB::UInt128(1), /*birth=*/true);
    writeSealAt(*backend, layout, ns, RefTxnId{1, 2});
    publishAt(*backend, layout, ns, RefTxnId{2, 1}, "ref_2", 2, DB::UInt128(2),
              /*birth=*/false, /*prev_epoch_seal=*/RefTxnId{1, 2});
    /// A third epoch keeps the unstable position from reading as a frontier.
    publishAt(*backend, layout, ns, RefTxnId{3, 1}, "ref_3", 3, DB::UInt128(3),
              /*birth=*/false, /*prev_epoch_seal=*/RefTxnId{2, 1});
    backend->flaky = layout.refLogKey(ns, RefTxnId{2, 1});

    Gc gc(store, kGc);
    ASSERT_TRUE(gc.runRegularRound().acquired_lease);

    const RefHold hold = holdOf(*backend, layout, ns);
    EXPECT_EQ(hold.reason, HoldReason::WitnessDisappeared);
    /// The walk crossed into epoch 2 on the record's first answer and then could not read it: the hold
    /// names {2,1}, the position that stopped being readable, and the cursor stays on the seal below it.
    EXPECT_EQ(hold.offending_position, (RefTxnId{2, 1}));
    EXPECT_EQ(sealedCursorOf(*backend, layout, ns), (RefTxnId{1, 2}));
}

/// ===================== THE SECOND WITNESS: `_ckpt.checkpoint` =====================
///
/// A listing is a SNAPSHOT: a record that became durable after the enumeration is invisible to that
/// round's probes, so an absent expected-next reads as a frontier when it is really a gap. The
/// namespace's own durable checkpoint decides the same question without asking the listing anything —
/// and this pair of pools is the proof, because they differ in nothing else.
TEST(CasGcHoldGrammar, CheckpointWitnessHoldsAGapTheHintIsSilentAbout)
{
    const RootNamespace ns{"00/aa@cas@"};
    const auto seed = [&](HintHoleCountingBackend & backend, const Layout & layout)
    {
        publishAt(backend, layout, ns, RefTxnId{1, 1}, "ref_1", 1, DB::UInt128(1), /*birth=*/true);
        publishAt(backend, layout, ns, RefTxnId{1, 2}, "ref_2", 2, DB::UInt128(2));
        /// {1,3} is missing and {1,4}, though durable, is invisible to every LIST.
        publishAt(backend, layout, ns, RefTxnId{1, 4}, "ref_4", 4, DB::UInt128(4));
        backend.hide(layout.refLogKey(ns, RefTxnId{1, 4}));
    };

    /// Hint-only: nothing above {1,2} is visible, so the walk honestly reads a frontier and does not hold.
    {
        auto backend = std::make_shared<HintHoleCountingBackend>();
        auto store = openPoolForTest(backend, /*gc_fold_max_defer_rounds=*/0);
        seed(*backend, store->layout());
        Gc gc(store, kGc);
        ASSERT_TRUE(gc.runRegularRound().acquired_lease);
        ASSERT_GT(backend->holesServed(), 0u);
        const auto cov = coverageOf(*backend, store->layout(), ns);
        ASSERT_TRUE(cov.has_value());
        EXPECT_FALSE(cov->hold.has_value()) << "without a witness an absent IS the frontier";
    }

    /// Same pool, same hint, plus the checkpoint: the gap becomes decidable and holds at the same
    /// position, with the same reason, as if the hint had shown the witness itself.
    ///
    /// The `_ckpt` object is hidden from every LIST as well, so the two pools' listings are byte-for-byte
    /// the same and the only difference between them is an object reachable by EXACT KEY alone. That is
    /// what makes this a proof of hint-INDEPENDENCE rather than of a richer hint.
    {
        auto backend = std::make_shared<HintHoleCountingBackend>();
        auto store = openPoolForTest(backend, /*gc_fold_max_defer_rounds=*/0);
        const Layout & layout = store->layout();
        seed(*backend, layout);
        writeCkptAt(*backend, layout, ns, RefTxnId{1, 4});
        backend->hide(layout.refCkptKey(ns));

        Gc gc(store, kGc);
        ASSERT_TRUE(gc.runRegularRound().acquired_lease);

        const RefHold hold = holdOf(*backend, layout, ns);
        EXPECT_EQ(hold.reason, HoldReason::GapBelowWitness);
        EXPECT_EQ(hold.offending_position, (RefTxnId{1, 3}));
    }
}

/// The namespace whose second witness matters MOST: one the hint has stopped mentioning entirely, kept
/// in the round's universe by nothing but its CARRIED HOLD. Its checkpoint is why
/// `readCheckpointWitnesses` takes the parent cursors as well as the hint — the hold alone witnesses only
/// the position it stopped at, so a gap ABOVE that position, once the hold resolves, has no witness left.
TEST(CasGcHoldGrammar, CheckpointWitnessReachesAHeldNamespaceTheHintNoLongerNames)
{
    const RootNamespace ns{"00/aa@cas@"};

    /// Round 1 in both pools: held at {1,3} by a gap below the listed witness {1,4}. Then the hint goes
    /// silent about every one of the namespace's objects, {1,3} becomes readable (so the hold resolves and
    /// the walk runs on), and a durable-but-unlisted {1,6} leaves a fresh gap at {1,5}.
    const auto seedPool = [&](HintHoleCountingBackend & backend, const Layout & layout, Gc & gc)
    {
        publishAt(backend, layout, ns, RefTxnId{1, 1}, "ref_1", 1, DB::UInt128(1), /*birth=*/true);
        publishAt(backend, layout, ns, RefTxnId{1, 2}, "ref_2", 2, DB::UInt128(2));
        publishAt(backend, layout, ns, RefTxnId{1, 4}, "ref_4", 4, DB::UInt128(4));
        EXPECT_TRUE(gc.runRegularRound().acquired_lease);
        EXPECT_EQ(holdOf(backend, layout, ns).offending_position, (RefTxnId{1, 3}));

        publishAt(backend, layout, ns, RefTxnId{1, 3}, "ref_3", 3, DB::UInt128(3));
        publishAt(backend, layout, ns, RefTxnId{1, 6}, "ref_6", 6, DB::UInt128(6));
        for (const RefTxnId & id : {RefTxnId{1, 1}, RefTxnId{1, 2}, RefTxnId{1, 3}, RefTxnId{1, 4},
                                    RefTxnId{1, 6}})
            backend.hide(layout.refLogKey(ns, id));
    };

    /// Hold-witness only: it witnesses {1,3}, which the walk has now passed, so the absent {1,5} above it
    /// is an honest frontier and the namespace comes out clean.
    {
        auto backend = std::make_shared<HintHoleCountingBackend>();
        auto store = openPoolForTest(backend, /*gc_fold_max_defer_rounds=*/0);
        Gc gc(store, kGc);
        seedPool(*backend, store->layout(), gc);

        ASSERT_TRUE(gc.runRegularRound().acquired_lease);
        const auto cov = coverageOf(*backend, store->layout(), ns);
        ASSERT_TRUE(cov.has_value());
        EXPECT_FALSE(cov->hold.has_value()) << "a resolved hold witnesses nothing above itself";
        EXPECT_EQ(cov->last_folded_ref_id, (RefTxnId{1, 4}));
    }

    /// Same pool, plus the checkpoint — read by exact key for a namespace THIS round's hint never names.
    {
        auto backend = std::make_shared<HintHoleCountingBackend>();
        auto store = openPoolForTest(backend, /*gc_fold_max_defer_rounds=*/0);
        const Layout & layout = store->layout();
        Gc gc(store, kGc);
        seedPool(*backend, layout, gc);
        writeCkptAt(*backend, layout, ns, RefTxnId{1, 6});
        backend->hide(layout.refCkptKey(ns));

        ASSERT_TRUE(gc.runRegularRound().acquired_lease);
        const RefHold hold = holdOf(*backend, layout, ns);
        EXPECT_EQ(hold.reason, HoldReason::GapBelowWitness);
        EXPECT_EQ(hold.offending_position, (RefTxnId{1, 5}));
        EXPECT_EQ(sealedCursorOf(*backend, layout, ns), (RefTxnId{1, 4}));
    }
}

/// ===================== THE HOLD IS DURABLE =====================

namespace
{

/// Seed a namespace held at {1,3} by a gap below the listed witness {1,4}, then make the hint forget
/// the namespace exists. Returns the round-1 hold.
RefHold seedHeldThenUnhinted(
    const std::shared_ptr<HintHoleCountingBackend> & backend, const PoolPtr & store,
    const RootNamespace & ns, Gc & gc)
{
    const Layout & layout = store->layout();
    publishAt(*backend, layout, ns, RefTxnId{1, 1}, "ref_1", 1, DB::UInt128(1), /*birth=*/true);
    publishAt(*backend, layout, ns, RefTxnId{1, 2}, "ref_2", 2, DB::UInt128(2));
    publishAt(*backend, layout, ns, RefTxnId{1, 4}, "ref_4", 4, DB::UInt128(4));

    EXPECT_TRUE(gc.runRegularRound().acquired_lease);
    const RefHold hold = holdOf(*backend, layout, ns);

    /// Every one of the namespace's objects vanishes from every LIST while staying readable by key:
    /// the round that follows has no hint entry for this namespace at all.
    for (const RefTxnId & id : {RefTxnId{1, 1}, RefTxnId{1, 2}, RefTxnId{1, 4}})
        backend->hide(layout.refLogKey(ns, id));
    return hold;
}

}

TEST(CasGcHoldGrammar, HoldRidesARoundWhoseHintOmitsTheNamespace)
{
    auto backend = std::make_shared<HintHoleCountingBackend>();
    auto store = openPoolForTest(backend, /*gc_fold_max_defer_rounds=*/0);
    const RootNamespace ns{"00/aa@cas@"};
    Gc gc(store, kGc);
    const RefHold first = seedHeldThenUnhinted(backend, store, ns, gc);

    ASSERT_TRUE(gc.runRegularRound().acquired_lease);
    ASSERT_GT(backend->holesServed(), 0u);

    const RefHold second = holdOf(*backend, store->layout(), ns);
    EXPECT_EQ(second.reason, first.reason) << "a quiet hint must not rewrite why the namespace is held";
    EXPECT_EQ(second.offending_position, first.offending_position);
    EXPECT_EQ(sealedCursorOf(*backend, store->layout(), ns), (RefTxnId{1, 2}))
        << "the cursor may not advance while the hold stands";
    /// The one field that moves, and the reason it exists: it counts the rounds that retried and failed.
    EXPECT_EQ(second.retry_count, first.retry_count + 1);
}

TEST(CasGcHoldGrammar, HoldForcesAnExactRetryOfItsOffendingPositionWhenUnhinted)
{
    auto backend = std::make_shared<HintHoleCountingBackend>();
    auto store = openPoolForTest(backend, /*gc_fold_max_defer_rounds=*/0);
    const RootNamespace ns{"00/aa@cas@"};
    Gc gc(store, kGc);
    seedHeldThenUnhinted(backend, store, ns, gc);

    const String offending = store->layout().refLogKey(ns, RefTxnId{1, 3});
    const uint64_t before = backend->getCount(offending);
    ASSERT_TRUE(gc.runRegularRound().acquired_lease);

    EXPECT_GT(backend->getCount(offending), before)
        << "a carried hold must read its offending position by EXACT key; the hint cannot be asked, "
           "because the hint no longer mentions the namespace at all";
}

/// The clearing rule, stated as a test: an absent proves nothing. The round below observes the
/// offending position absent AGAIN, with no witness anywhere — exactly the observation a lying store
/// produces — and the hold survives it. Only the record actually appearing, being folded, and the
/// result reaching `gc/state` clears it.
TEST(CasGcHoldGrammar, HoldClearsOnlyByFoldingThroughTheOffendingPosition)
{
    auto backend = std::make_shared<HintHoleCountingBackend>();
    auto store = openPoolForTest(backend, /*gc_fold_max_defer_rounds=*/0);
    const Layout & layout = store->layout();
    const RootNamespace ns{"00/aa@cas@"};
    Gc gc(store, kGc);
    seedHeldThenUnhinted(backend, store, ns, gc);

    /// Round 2: another absent, no witness. NOT a clearance.
    ASSERT_TRUE(gc.runRegularRound().acquired_lease);
    EXPECT_EQ(holdOf(*backend, layout, ns).offending_position, (RefTxnId{1, 3}));

    /// The record appears at last (still invisible to every LIST — the hold is the only thing that
    /// knows to look there).
    publishAt(*backend, layout, ns, RefTxnId{1, 3}, "ref_3", 3, DB::UInt128(3));
    backend->hide(layout.refLogKey(ns, RefTxnId{1, 3}));

    ASSERT_TRUE(gc.runRegularRound().acquired_lease);
    const auto cov = coverageOf(*backend, layout, ns);
    ASSERT_TRUE(cov.has_value());
    EXPECT_FALSE(cov->hold.has_value()) << "folding through the offending position is what clears a hold";
    EXPECT_EQ(cov->classification, 2);
    EXPECT_EQ(cov->last_folded_ref_id, (RefTxnId{1, 4})) << "the walk resumed past the resolved gap";
    EXPECT_EQ(inDegreeOf(*backend, layout, DB::UInt128(4)), 1)
        << "the record above the gap finally contributed its owner edge";
}

/// ===================== REBUILD =====================

namespace
{

/// Rewrite the fold seal at an EXACT `(generation, attempt)`, applying `mutate` to it. Needed where
/// the seal under test is not the adopted one — a step-down test plants its hold in a generation the
/// pool has already moved past.
void mutateSealAt(Backend & backend, const Layout & layout, uint64_t generation, uint64_t attempt,
                  const std::function<void(CasFoldSeal &)> & mutate)
{
    const String key = layout.foldSealKey(generation, attempt);
    CasFoldSeal seal = decodeFoldSeal(backend.get(key)->bytes);
    mutate(seal);
    backend.putOverwrite(key, encodeFoldSeal(seal), backend.head(key).token);
}

/// Rewrite the adopted fold seal, applying `mutate` to it. Used to plant a hold that the rebuild must
/// then carry: planting it directly (rather than by holding a real round) keeps the REBUILD tests about
/// the carry, not about how the hold arose.
void mutateAdoptedSeal(Backend & backend, const Layout & layout, const std::function<void(CasFoldSeal &)> & mutate)
{
    const GcState st = decodeGcState(backend.get(layout.gcStateKey())->bytes);
    const String key = layout.foldSealKey(st.snap_generation, st.snap_attempt);
    CasFoldSeal seal = decodeFoldSeal(backend.get(key)->bytes);
    mutate(seal);
    backend.putOverwrite(key, encodeFoldSeal(seal), backend.head(key).token);
}

RefHold plantedHold()
{
    return RefHold{.reason = HoldReason::WitnessDisappeared, .offending_position = RefTxnId{4, 9},
                   .retry_count = 17, .next_retry_round = 23};
}

}

/// A rebuild derives coverage from owner state, so without this it would overwrite every held row with
/// a clean one and hand back a baseline that LOOKS proven. Every hold rides through verbatim —
/// including `retry_count`, because a rebuild retried nothing and must not reset the count that tells
/// an operator how long the namespace has been stuck.
TEST(CasGcHoldGrammar, RebuildCarriesEveryHoldVerbatim)
{
    auto backend = std::make_shared<InMemoryBackend>();
    auto store = openPoolForTest(backend, /*gc_fold_max_defer_rounds=*/0);
    const Layout & layout = store->layout();
    const RootNamespace ns{"00/aa@cas@"};

    publishAt(*backend, layout, ns, RefTxnId{1, 1}, "ref_1", 1, DB::UInt128(1), /*birth=*/true);
    Gc gc(store, kGc);
    ASSERT_TRUE(gc.runRegularRound().acquired_lease);

    /// One hold on a namespace the rebuild WILL rediscover, one on a namespace it will not.
    mutateAdoptedSeal(*backend, layout, [&](CasFoldSeal & seal)
    {
        ShardCoverage & cov = seal.per_ns_shard[cursorKeyForTest(ns, 0)];
        cov.classification = 4;
        cov.hold = plantedHold();
        ShardCoverage gone;
        gone.classification = 4;
        gone.last_folded_ref_id = RefTxnId{2, 2};
        gone.hold = RefHold{.reason = HoldReason::GapBelowWitness, .offending_position = RefTxnId{2, 3},
                            .retry_count = 1, .next_retry_round = 2};
        seal.per_ns_shard[cursorKeyForTest(RootNamespace{"00/zz@cas@"}, 0)] = gone;
    });

    const RebuildReport rep = gc.rebuildBaseline(/*force=*/true);
    ASSERT_TRUE(rep.performed) << rep.refusal;

    const auto rebuilt = newestSeal(*backend, layout);
    ASSERT_TRUE(rebuilt.has_value());
    const auto rediscovered = rebuilt->per_ns_shard.find(cursorKeyForTest(ns, 0));
    ASSERT_NE(rediscovered, rebuilt->per_ns_shard.end());
    EXPECT_EQ(rediscovered->second.classification, 4);
    ASSERT_TRUE(rediscovered->second.hold.has_value());
    EXPECT_EQ(*rediscovered->second.hold, plantedHold());

    const auto vanished = rebuilt->per_ns_shard.find(cursorKeyForTest(RootNamespace{"00/zz@cas@"}, 0));
    ASSERT_NE(vanished, rebuilt->per_ns_shard.end())
        << "a hold on a namespace the rebuild cannot see is exactly the one that must not be dropped";
    ASSERT_TRUE(vanished->second.hold.has_value());
    EXPECT_EQ(vanished->second.hold->offending_position, (RefTxnId{2, 3}));
    EXPECT_EQ(vanished->second.last_folded_ref_id, (RefTxnId{2, 2})) << "its cursor rides with it";
}

/// AN ORDINARY CRASH IS NOT A CORRUPT POOL. A round writes its runs during the reduce phase and its
/// fold seal only at phase 10/18, so a crash in between leaves the newest generation existing WITHOUT
/// a seal — the commonest shape there is. If discovery stopped at the listing's maximum it would find
/// no seal there, conclude it could enumerate nothing, and refuse — telling the operator to recreate a
/// pool whose holds are sitting readable one generation down.
///
/// So discovery steps DOWN through the generations the listing itself reported until one carries a
/// seal. That spends no trust the maximum had not already been given. What it does NOT weaken is the
/// refusal above the maximum: that one stays terminal, because a seal found there is the listing
/// caught lying, not merely being incomplete about seals.
TEST(CasGcHoldGrammar, RebuildStepsDownPastACrashedNewestGenerationToTheSealBelowIt)
{
    auto backend = std::make_shared<InMemoryBackend>();
    auto store = openPoolForTest(backend, /*gc_fold_max_defer_rounds=*/0);
    const Layout & layout = store->layout();
    const RootNamespace ns{"00/aa@cas@"};

    publishAt(*backend, layout, ns, RefTxnId{1, 1}, "ref_1", 1, DB::UInt128(1), /*birth=*/true);
    Gc gc(store, kGc);
    ASSERT_TRUE(gc.runRegularRound().acquired_lease);
    const GcState after_first = decodeGcState(backend->get(layout.gcStateKey())->bytes);
    const uint64_t older_generation = after_first.snap_generation;
    const uint64_t older_attempt = after_first.snap_attempt;

    publishAt(*backend, layout, ns, RefTxnId{1, 2}, "ref_2", 2, DB::UInt128(2));
    ASSERT_TRUE(gc.runRegularRound().acquired_lease);
    const GcState after_second = decodeGcState(backend->get(layout.gcStateKey())->bytes);
    ASSERT_GT(after_second.snap_generation, older_generation) << "the fixture needs two generations";

    /// The older generation is the one holding the pool's durable hold.
    mutateSealAt(*backend, layout, older_generation, older_attempt, [&](CasFoldSeal & seal)
    {
        ShardCoverage & cov = seal.per_ns_shard[cursorKeyForTest(ns, 0)];
        cov.classification = 4;
        cov.hold = plantedHold();
    });

    /// THE CRASH: the newest generation's run objects are there, its seal never got written. Then
    /// `gc/state` is lost, which is this path's whole premise.
    const String newest_seal = layout.foldSealKey(after_second.snap_generation, after_second.snap_attempt);
    const HeadResult seal_head = backend->head(newest_seal);
    ASSERT_TRUE(seal_head.exists);
    ASSERT_EQ(backend->deleteExact(newest_seal, seal_head.token).kind, DeleteOutcome::Kind::Deleted);
    ASSERT_TRUE(backend->list(layout.gcGenPrefix(after_second.snap_generation), "", 1).keys.size() > 0)
        << "the crashed generation must still hold objects, or it is not the shape being modelled";
    const HeadResult sh = backend->head(layout.gcStateKey());
    ASSERT_EQ(backend->deleteExact(layout.gcStateKey(), sh.token).kind, DeleteOutcome::Kind::Deleted);

    Gc gc2(store, hexToU128("0000000000000000000000000000000c"));
    const RebuildReport rep = gc2.rebuildBaseline(/*force=*/false);
    ASSERT_TRUE(rep.performed) << rep.refusal;
    EXPECT_FALSE(rep.virgin_by_enumeration) << "a pool with a readable seal is not virgin";
    EXPECT_EQ(rep.adopted_seal_generation, older_generation)
        << "the report must name WHICH generation the holds came from, so a step-down is visible";

    const auto rebuilt = newestSeal(*backend, layout);
    ASSERT_TRUE(rebuilt.has_value());
    const auto it = rebuilt->per_ns_shard.find(cursorKeyForTest(ns, 0));
    ASSERT_NE(it, rebuilt->per_ns_shard.end());
    ASSERT_TRUE(it->second.hold.has_value())
        << "a crash between the run writes and the seal write turned into 'recreate the pool', and the "
           "hold readable one generation down was thrown away with it";
    EXPECT_EQ(*it->second.hold, plantedHold());
}

/// With no readable prior seal there is nothing to carry, and the holds it may have contained are
/// unknowable. The rebuild refuses rather than blessing a baseline whose provenance it cannot state —
/// a pool-wide hold is not representable (there is no offending position anyone could ever fold
/// through), so the honest answer is the refusal, and the recovery path is pool recreation.
TEST(CasGcHoldGrammar, RebuildRefusesWithAMissingPriorSeal)
{
    auto backend = std::make_shared<InMemoryBackend>();
    auto store = openPoolForTest(backend, /*gc_fold_max_defer_rounds=*/0);
    const Layout & layout = store->layout();
    const RootNamespace ns{"00/aa@cas@"};

    publishAt(*backend, layout, ns, RefTxnId{1, 1}, "ref_1", 1, DB::UInt128(1), /*birth=*/true);
    Gc gc(store, kGc);
    ASSERT_TRUE(gc.runRegularRound().acquired_lease);

    const GcState st = decodeGcState(backend->get(layout.gcStateKey())->bytes);
    ASSERT_GT(st.snap_generation, 0u);
    const String seal_key = layout.foldSealKey(st.snap_generation, st.snap_attempt);
    const HeadResult sh = backend->head(seal_key);
    ASSERT_TRUE(sh.exists);
    ASSERT_EQ(backend->deleteExact(seal_key, sh.token).kind, DeleteOutcome::Kind::Deleted);

    /// FORCE does not buy past it either: force means "rebuild deliberately", never "drop the holds".
    for (const bool force : {false, true})
    {
        SCOPED_TRACE(force ? "force" : "plain");
        expectThrowsCode(DB::ErrorCodes::CORRUPTED_DATA, [&] { gc.rebuildBaseline(force); });
    }

    const GcState after = decodeGcState(backend->get(layout.gcStateKey())->bytes);
    EXPECT_EQ(after.snap_generation, st.snap_generation) << "a refused rebuild adopts nothing";
}

TEST(CasGcHoldGrammar, RebuildRefusesWithAnUndecodablePriorSeal)
{
    auto backend = std::make_shared<InMemoryBackend>();
    auto store = openPoolForTest(backend, /*gc_fold_max_defer_rounds=*/0);
    const Layout & layout = store->layout();
    const RootNamespace ns{"00/aa@cas@"};

    publishAt(*backend, layout, ns, RefTxnId{1, 1}, "ref_1", 1, DB::UInt128(1), /*birth=*/true);
    Gc gc(store, kGc);
    ASSERT_TRUE(gc.runRegularRound().acquired_lease);

    const GcState st = decodeGcState(backend->get(layout.gcStateKey())->bytes);
    const String seal_key = layout.foldSealKey(st.snap_generation, st.snap_attempt);
    backend->putOverwrite(seal_key, "{\"type\":\"cas_fold_seal\",\"v\":4}\nthis is not a seal body\n",
                          backend->head(seal_key).token);

    expectThrowsCode(DB::ErrorCodes::CORRUPTED_DATA, [&] { gc.rebuildBaseline(/*force=*/true); });
}

/// LOSING THE POINTER IS NOT WEAKER THAN LOSING THE SEAL. `gc/state` names the adopted seal, and it is
/// the seal that carries the holds — so if the refusal only covered an unreadable seal, the *lesser*
/// corruption (the pointer is gone, every seal intact) would be treated more permissively than the
/// greater one, and the rebuild would write a baseline with no hold in it at all.
///
/// That matters because holds are not re-derivable by the next walk. `WitnessDisappeared` names a
/// record that is *gone*: the next round reads a clean frontier and would hand the namespace exactly
/// the frontier proof the hold exists to deny. Same for any hold whose only witness was the checkpoint
/// or the hold itself.
///
/// So with no adopted baseline named, the rebuild finds the newest fold seal OBJECT by enumeration and
/// carries its holds. This keeps the pool's disaster recovery intact — losing `gc/state` on a
/// lived-in pool is the scenario `REBUILD` exists for — while making it impossible to write a
/// hold-free baseline over a pool that had holds.
TEST(CasGcHoldGrammar, RebuildWithLostStateStillCarriesHoldsFromTheNewestSeal)
{
    auto backend = std::make_shared<InMemoryBackend>();
    auto store = openPoolForTest(backend, /*gc_fold_max_defer_rounds=*/0);
    const Layout & layout = store->layout();
    const RootNamespace ns{"00/aa@cas@"};

    publishAt(*backend, layout, ns, RefTxnId{1, 1}, "ref_1", 1, DB::UInt128(1), /*birth=*/true);
    Gc gc(store, kGc);
    ASSERT_TRUE(gc.runRegularRound().acquired_lease);
    mutateAdoptedSeal(*backend, layout, [&](CasFoldSeal & seal)
    {
        ShardCoverage & cov = seal.per_ns_shard[cursorKeyForTest(ns, 0)];
        cov.classification = 4;
        cov.hold = plantedHold();
    });

    /// The pointer vanishes; every seal object survives.
    const HeadResult sh = backend->head(layout.gcStateKey());
    ASSERT_TRUE(sh.exists);
    ASSERT_EQ(backend->deleteExact(layout.gcStateKey(), sh.token).kind, DeleteOutcome::Kind::Deleted);

    Gc gc2(store, hexToU128("00000000000000000000000000000009"));
    const RebuildReport rep = gc2.rebuildBaseline(/*force=*/false);
    ASSERT_TRUE(rep.performed) << rep.refusal;

    const auto rebuilt = newestSeal(*backend, layout);
    ASSERT_TRUE(rebuilt.has_value());
    const auto it = rebuilt->per_ns_shard.find(cursorKeyForTest(ns, 0));
    ASSERT_NE(it, rebuilt->per_ns_shard.end());
    ASSERT_TRUE(it->second.hold.has_value())
        << "the rebuild blessed a baseline with no hold in it, having read no seal at all";
    EXPECT_EQ(*it->second.hold, plantedHold());
}

/// ...and when that newest seal cannot be read either, there is nothing left to carry and no way to
/// know what was lost, so the rebuild refuses exactly as it does for an unreadable adopted seal.
TEST(CasGcHoldGrammar, RebuildRefusesWhenTheNewestSealIsUnreadableAndTheStateIsLost)
{
    auto backend = std::make_shared<InMemoryBackend>();
    auto store = openPoolForTest(backend, /*gc_fold_max_defer_rounds=*/0);
    const Layout & layout = store->layout();
    const RootNamespace ns{"00/aa@cas@"};

    publishAt(*backend, layout, ns, RefTxnId{1, 1}, "ref_1", 1, DB::UInt128(1), /*birth=*/true);
    Gc gc(store, kGc);
    ASSERT_TRUE(gc.runRegularRound().acquired_lease);

    const GcState st = decodeGcState(backend->get(layout.gcStateKey())->bytes);
    const String seal_key = layout.foldSealKey(st.snap_generation, st.snap_attempt);
    backend->putOverwrite(seal_key, "{\"type\":\"cas_fold_seal\",\"v\":4}\nthis is not a seal body\n",
                          backend->head(seal_key).token);
    const HeadResult sh = backend->head(layout.gcStateKey());
    ASSERT_EQ(backend->deleteExact(layout.gcStateKey(), sh.token).kind, DeleteOutcome::Kind::Deleted);

    Gc gc2(store, hexToU128("0000000000000000000000000000000a"));
    for (const bool force : {false, true})
    {
        SCOPED_TRACE(force ? "force" : "plain");
        expectThrowsCode(DB::ErrorCodes::CORRUPTED_DATA, [&] { gc2.rebuildBaseline(force); });
    }
}

/// NEWEST-NESS IS NOT READ OFF A LISTING. Taking the newest seal from the pool-wide enumeration would
/// put the same hole one layer up: a listing that omits the true newest seal hands back an OLDER one,
/// and every hold detected since that older seal is silently lost. Two narrow single-generation probes
/// above the listing's maximum ask whether it lied.
///
/// And when it did lie, the answer is REFUSAL, not adoption of the newer seal. A store that misreports
/// its own enumeration DURING DISASTER RECOVERY does not get a second guess: adopting whatever the
/// second query happened to return would move the same trust one query along and prove nothing.
///
/// The fixture is the production shape rather than a contrivance: the broad `gc/gen/` enumeration
/// omits the newest generation's objects while a listing scoped to that generation still returns
/// them — the same class of lie the arithmetic ref walk was built for one layer down.
TEST(CasGcHoldGrammar, RebuildRefusesWhenANarrowProbeFindsASealAboveTheListingMaximum)
{
    /// Omits keys from ONE enumeration prefix only. Every other query — including a listing scoped to
    /// the generation itself — answers truthfully.
    class BroadListHoleBackend : public InMemoryBackend
    {
    public:
        String hide_under_prefix;
        String hidden_key_infix;
        size_t holes_served = 0;

        ListPage list(const String & prefix, const String & cursor, size_t limit) override
        {
            ListPage page = InMemoryBackend::list(prefix, cursor, limit);
            if (prefix != hide_under_prefix)
                return page;
            const size_t before = page.keys.size();
            std::erase_if(page.keys,
                          [&](const ListedKey & k) { return k.key.find(hidden_key_infix) != String::npos; });
            if (page.keys.size() != before)
                ++holes_served;
            return page;
        }
    };

    auto backend = std::make_shared<BroadListHoleBackend>();
    auto store = openPoolForTest(backend, /*gc_fold_max_defer_rounds=*/0);
    const Layout & layout = store->layout();
    const RootNamespace ns{"00/aa@cas@"};

    publishAt(*backend, layout, ns, RefTxnId{1, 1}, "ref_1", 1, DB::UInt128(1), /*birth=*/true);
    Gc gc(store, kGc);
    ASSERT_TRUE(gc.runRegularRound().acquired_lease);
    publishAt(*backend, layout, ns, RefTxnId{1, 2}, "ref_2", 2, DB::UInt128(2));
    ASSERT_TRUE(gc.runRegularRound().acquired_lease);

    const GcState st = decodeGcState(backend->get(layout.gcStateKey())->bytes);
    ASSERT_GT(st.snap_generation, 1u) << "the fixture needs a newer generation to hide";
    mutateAdoptedSeal(*backend, layout, [&](CasFoldSeal & seal)
    {
        ShardCoverage & cov = seal.per_ns_shard[cursorKeyForTest(ns, 0)];
        cov.classification = 4;
        cov.hold = plantedHold();
    });

    /// The pool-wide enumeration loses the newest generation entirely; the pointer to it is deleted.
    const String gen_prefix = layout.gcGenPrefix(0);
    backend->hide_under_prefix = gen_prefix.substr(0, gen_prefix.size() - 2);   /// ".../gc/gen/"
    backend->hidden_key_infix = layout.gcGenPrefix(st.snap_generation);
    const HeadResult sh = backend->head(layout.gcStateKey());
    ASSERT_EQ(backend->deleteExact(layout.gcStateKey(), sh.token).kind, DeleteOutcome::Kind::Deleted);

    Gc gc2(store, hexToU128("0000000000000000000000000000000b"));
    for (const bool force : {false, true})
    {
        SCOPED_TRACE(force ? "force" : "plain");
        expectThrowsCode(DB::ErrorCodes::CORRUPTED_DATA, [&] { gc2.rebuildBaseline(force); });
    }
    ASSERT_GT(backend->holes_served, 0u) << "the broad listing never actually lied";

    /// Nothing was adopted: the refusal fires before the lease, so the pool is exactly as it was.
    EXPECT_FALSE(backend->head(layout.gcStateKey()).exists)
        << "a refused rebuild must not mint a baseline, nor a bootstrap body";
}

/// The virgin verdict, pinned so the refusal can never grow to swallow a fresh pool — and pinned as
/// what it actually is. It rests on THREE pieces of enumeration evidence (wide LIST empty, narrow
/// generation-1 probe empty, no `gc/state`) and on no point read at all, so it is COUNTED: an operator
/// reading a disaster-recovery run needs to see that the clean slate came from enumeration rather than
/// from proof. `CasGcRebuildVirginByEnumeration` on a pool that has ever completed a round means the
/// enumeration lied.
TEST(CasGcHoldGrammar, RebuildProceedsOnAPoolThatNeverSealedABaselineAndCountsTheVerdict)
{
    auto backend = std::make_shared<InMemoryBackend>();
    auto store = openPoolForTest(backend, /*gc_fold_max_defer_rounds=*/0);
    const Layout & layout = store->layout();
    const RootNamespace ns{"00/aa@cas@"};

    /// No round has run, so there is no `gc/state` and no seal — only owner state to rebuild from.
    publishAt(*backend, layout, ns, RefTxnId{1, 1}, "ref_1", 1, DB::UInt128(1), /*birth=*/true);
    ASSERT_FALSE(backend->head(layout.gcStateKey()).exists);

    using ProfileEvents::global_counters;
    const auto virgin_before = global_counters[ProfileEvents::CasGcRebuildVirginByEnumeration].load();

    Gc gc(store, kGc);
    const RebuildReport rep = gc.rebuildBaseline(/*force=*/false);
    EXPECT_TRUE(rep.performed) << rep.refusal;
    EXPECT_GT(global_counters[ProfileEvents::CasGcRebuildVirginByEnumeration].load(), virgin_before)
        << "a clean slate granted from enumeration alone must be visible to whoever reads the run";
}
