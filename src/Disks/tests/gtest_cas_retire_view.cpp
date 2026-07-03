#include <gtest/gtest.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasGcFormats.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasIds.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasInMemoryBackend.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasLayout.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasRetireView.h>

#include <Disks/tests/cas_test_helpers.h>

#include <algorithm>

using namespace DB::Cas;

/// RetireView reads the CURRENT retired list by dereferencing `gc/state.retired_refs` (spec §5,
/// ack-floor redesign) — NOT by LISTing a retired prefix. These tests inject GC state by writing
/// gc/state (with retired_refs pointing at the retired objects) and the retired objects themselves:
/// that is the documented on-storage interface between GC and the writer publish gate. `seedRetired`
/// writes a set at retiredKey(gen, attempt, round, shard) and records the ref in gc/state under that
/// shard, mirroring what the GC pass's retire bridge does.
namespace
{

/// Seed gc/state{round, snap_generation=gen, snap_attempt=attempt} plus per-shard retired sets whose
/// keys are recorded in retired_refs — the shape RetireView::refresh consumes.
void seedState(Backend & b, const Layout & layout, uint64_t round, uint64_t gen, uint64_t attempt,
               const std::map<uint64_t, RetiredSet> & per_shard)
{
    GcState state;
    state.round = round;
    state.snap_generation = gen;
    state.snap_attempt = attempt;
    for (const auto & [shard, set] : per_shard)
    {
        const String key = layout.retiredKey(gen, attempt, round, shard);
        b.putIfAbsent(key, encodeRetiredSet(set));
        state.retired_refs[shard] = key;
    }
    const auto head = b.head(layout.gcStateKey());
    if (head.exists)
        b.putOverwrite(layout.gcStateKey(), encodeGcState(state), head.token);
    else
        b.putIfAbsent(layout.gcStateKey(), encodeGcState(state));
}

}

TEST(CasRetireView, EmptyPoolIsRoundZero)
{
    auto b = std::make_shared<InMemoryBackend>();
    Layout layout("p");
    RetireView v(b, layout);
    v.refresh();
    EXPECT_EQ(v.round(), 0u);
    EXPECT_FALSE(v.findCondemned(ObjectKind::Blob, hexToU128("000102030405060708090a0b0c0d0e0f")).has_value());
}

TEST(CasRetireView, SeesInjectedRetirements)
{
    auto b = std::make_shared<InMemoryBackend>();
    Layout layout("p");
    RetiredSet rs;
    const auto h = hexToU128("000102030405060708090a0b0c0d0e0f");
    rs.entries.push_back({ObjectKind::Blob, h, Token{"3", TokenType::Emulated}, 10});
    seedState(*b, layout, /*round*/ 2, /*gen*/ 0, /*attempt*/ 0, {{0, rs}});
    RetireView v(b, layout);
    v.refresh();
    EXPECT_EQ(v.round(), 2u);
    auto hit = v.findCondemned(ObjectKind::Blob, h);
    ASSERT_TRUE(hit.has_value());
    EXPECT_EQ(hit->size(), 1u);
    EXPECT_EQ((*hit)[0].value, "3");
}

TEST(CasRetireView, RefreshDropsRewrittenEntries)
{
    auto b = std::make_shared<InMemoryBackend>();
    Layout layout("p");
    RetiredSet rs;
    const auto h = hexToU128("000102030405060708090a0b0c0d0e0f");
    rs.entries.push_back({ObjectKind::Blob, h, Token{"3", TokenType::Emulated}, 10});
    seedState(*b, layout, /*round*/ 2, /*gen*/ 0, /*attempt*/ 0, {{0, rs}});

    RetireView v(b, layout);
    v.refresh();
    EXPECT_EQ(v.round(), 2u);
    ASSERT_TRUE(v.findCondemned(ObjectKind::Blob, h).has_value());

    /// GC "drops" the entry by rewriting the retired object WITHOUT it, and bumps gc/state.
    /// Reading current storage state IS the protocol's view. Re-seed at round 3 with an empty set.
    seedState(*b, layout, /*round*/ 3, /*gen*/ 0, /*attempt*/ 0, {{0, RetiredSet{}}});

    v.refresh();
    EXPECT_EQ(v.round(), 3u);
    EXPECT_FALSE(v.findCondemned(ObjectKind::Blob, h).has_value());
}

TEST(CasRetireView, MultipleShardsUnion)
{
    auto b = std::make_shared<InMemoryBackend>();
    Layout layout("p");

    /// Two retired objects on DIFFERENT gc-shards condemn DIFFERENT tokens of the SAME (kind, hash) —
    /// the view is the UNION over all refs recorded in gc/state.
    const auto h = hexToU128("000102030405060708090a0b0c0d0e0f");
    RetiredSet rs0;
    rs0.entries.push_back({ObjectKind::Blob, h, Token{"7", TokenType::Emulated}, 10});
    RetiredSet rs1;
    rs1.entries.push_back({ObjectKind::Blob, h, Token{"9", TokenType::Emulated}, 10});
    seedState(*b, layout, /*round*/ 2, /*gen*/ 0, /*attempt*/ 0, {{0, rs0}, {3, rs1}});

    RetireView v(b, layout);
    v.refresh();
    EXPECT_EQ(v.round(), 2u);

    auto hit = v.findCondemned(ObjectKind::Blob, h);
    ASSERT_TRUE(hit.has_value());
    ASSERT_EQ(hit->size(), 2u);
    std::vector<String> values{(*hit)[0].value, (*hit)[1].value};
    std::sort(values.begin(), values.end());
    EXPECT_EQ(values[0], "7");
    EXPECT_EQ(values[1], "9");

    EXPECT_TRUE(v.isCondemnedToken(ObjectKind::Blob, h, Token{"7", TokenType::Emulated}));
    EXPECT_TRUE(v.isCondemnedToken(ObjectKind::Blob, h, Token{"9", TokenType::Emulated}));
    EXPECT_FALSE(v.isCondemnedToken(ObjectKind::Blob, h, Token{"8", TokenType::Emulated}));
}

/// Attempt-scoping (writer-facing leak): a retired set planted under a NON-adopted attempt must be
/// INVISIBLE to RetireView — because refs come out of gc/state, which only records the adopted attempt's
/// keys. A decoy set that gc/state does NOT reference can never condemn a live writer token. Recording
/// the ref then makes it condemned (positive control), proving the test is real.
TEST(CasRetireView, NonReferencedRetiredSetInvisible)
{
    auto b = std::make_shared<InMemoryBackend>();
    Layout layout("p");

    const auto h = hexToU128("000102030405060708090a0b0c0d0e0f");
    const Token tok{"3", TokenType::Emulated};

    /// gc/state adopts (snap_generation=4, snap_attempt=42), round 2, with NO retired_refs (decoy unreferenced).
    {
        GcState state;
        state.round = 2;
        state.snap_generation = 4;
        state.snap_attempt = 42;
        b->putIfAbsent(layout.gcStateKey(), encodeGcState(state));
    }
    /// Decoy: a retired set at a plausible key that gc/state does NOT reference.
    {
        RetiredSet rs;
        rs.entries.push_back({ObjectKind::Blob, h, tok, 10});
        b->putIfAbsent(layout.retiredKey(4, 42, 2, 0), encodeRetiredSet(rs));
    }

    RetireView v(b, layout);
    v.refresh();
    EXPECT_EQ(v.round(), 2u);
    EXPECT_FALSE(v.isCondemnedToken(ObjectKind::Blob, h, tok))
        << "a retired set not referenced by gc/state must be invisible to the writer publish gate";

    /// Positive control: record the ref in gc/state; after refresh it IS condemned.
    {
        GcState state;
        state.round = 2;
        state.snap_generation = 4;
        state.snap_attempt = 42;
        state.retired_refs[0] = layout.retiredKey(4, 42, 2, 0);
        const auto head = b->head(layout.gcStateKey());
        b->putOverwrite(layout.gcStateKey(), encodeGcState(state), head.token);
    }
    v.refresh();
    EXPECT_TRUE(v.isCondemnedToken(ObjectKind::Blob, h, tok))
        << "a retired set referenced by gc/state must be visible to the writer publish gate";
}

/// An absent ref target (gc/state names a key that has no object) contributes nothing — NOT an error.
TEST(CasRetireView, AbsentRefKeyIsEmptyNoThrow)
{
    auto b = std::make_shared<InMemoryBackend>();
    Layout layout("p");
    GcState state;
    state.round = 5;
    state.retired_refs[0] = layout.retiredKey(0, 0, 5, 0);   /// dangling: no object written
    b->putIfAbsent(layout.gcStateKey(), encodeGcState(state));

    RetireView v(b, layout);
    EXPECT_NO_THROW(v.refresh());
    EXPECT_EQ(v.round(), 5u);
    EXPECT_FALSE(v.findCondemned(ObjectKind::Blob, hexToU128("000102030405060708090a0b0c0d0e0f")).has_value());
}

/// Absent gc/state => round 0, empty view (a pool GC never touched).
TEST(CasRetireView, AbsentGcStateIsRoundZero)
{
    auto b = std::make_shared<InMemoryBackend>();
    Layout layout("p");
    RetireView v(b, layout);
    EXPECT_NO_THROW(v.refresh());
    EXPECT_EQ(v.round(), 0u);
}

/// The view stores tokens regardless of an entry's condemn_round — the writer gate does not consult it
/// (only GC uses condemn_round). Seed round 5 with entries condemned at rounds 4 and 5; both are visible.
TEST(CasRetireView, CondemnRoundIgnoredByView)
{
    auto b = std::make_shared<InMemoryBackend>();
    Layout layout("p");
    const auto h4 = hexToU128("00000000000000000000000000000004");
    const auto h5 = hexToU128("00000000000000000000000000000005");
    RetiredSet rs;
    RetiredEntry e4{ObjectKind::Blob, h4, Token{"t4", TokenType::Emulated}, 10};
    e4.condemn_round = 4;
    RetiredEntry e5{ObjectKind::Blob, h5, Token{"t5", TokenType::Emulated}, 10};
    e5.condemn_round = 5;
    rs.entries.push_back(e4);
    rs.entries.push_back(e5);
    seedState(*b, layout, /*round*/ 5, /*gen*/ 0, /*attempt*/ 0, {{0, rs}});

    RetireView v(b, layout);
    v.refresh();
    EXPECT_EQ(v.round(), 5u);
    EXPECT_TRUE(v.isCondemnedToken(ObjectKind::Blob, h4, Token{"t4", TokenType::Emulated}));
    EXPECT_TRUE(v.isCondemnedToken(ObjectKind::Blob, h5, Token{"t5", TokenType::Emulated}));
}

TEST(CasRetireView, IsCondemnedTokenIdentity)
{
    auto b = std::make_shared<InMemoryBackend>();
    Layout layout("p");
    const auto h = hexToU128("000102030405060708090a0b0c0d0e0f");
    RetiredSet rs;
    rs.entries.push_back({ObjectKind::Blob, h, Token{"3", TokenType::Emulated}, 10});
    seedState(*b, layout, /*round*/ 1, /*gen*/ 0, /*attempt*/ 0, {{0, rs}});

    RetireView v(b, layout);
    v.refresh();

    /// PINNED SEMANTICS: token equality is Token::operator== — value AND type must both match.
    /// The same value under a different TokenType is a DIFFERENT token.
    EXPECT_TRUE(v.isCondemnedToken(ObjectKind::Blob, h, Token{"3", TokenType::Emulated}));
    EXPECT_FALSE(v.isCondemnedToken(ObjectKind::Blob, h, Token{"3", TokenType::ETag}));
    EXPECT_FALSE(v.isCondemnedToken(ObjectKind::Blob, h, Token{"4", TokenType::Emulated}));
}
