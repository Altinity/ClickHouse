#include <gtest/gtest.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasGcFormats.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasIds.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasInMemoryBackend.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasLayout.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasRetireView.h>

#include <Disks/tests/cas_test_helpers.h>

#include <algorithm>

using namespace DB::Cas;

/// RetireView tests inject GC state by writing the gc/state and gc/retired/ objects directly:
/// that is the documented on-storage interface between GC and the writer (spec §5), not a
/// white-box shortcut.

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
    b->putIfAbsent(layout.gcStateKey(), tests::encodeMinimalGcState(2, 1));
    RetiredSet rs;
    const auto h = hexToU128("000102030405060708090a0b0c0d0e0f");
    rs.entries.push_back({ObjectKind::Blob, h, Token{"3", TokenType::Emulated}, 10});
    b->putIfAbsent(layout.retiredKey(2, 1, 0), encodeRetiredSet(rs));
    RetireView v(b, layout);
    v.refresh();
    EXPECT_EQ(v.round(), 2u);
    auto hit = v.findCondemned(ObjectKind::Blob, h);
    ASSERT_TRUE(hit.has_value());
    EXPECT_EQ(hit->size(), 1u);
    EXPECT_EQ((*hit)[0].value, "3");
    EXPECT_FALSE(v.findCondemned(ObjectKind::Tree, h).has_value());   /// kind is part of the identity
}

TEST(CasRetireView, RefreshDropsRewrittenEntries)
{
    auto b = std::make_shared<InMemoryBackend>();
    Layout layout("p");
    b->putIfAbsent(layout.gcStateKey(), tests::encodeMinimalGcState(2, 1));
    RetiredSet rs;
    const auto h = hexToU128("000102030405060708090a0b0c0d0e0f");
    rs.entries.push_back({ObjectKind::Blob, h, Token{"3", TokenType::Emulated}, 10});
    b->putIfAbsent(layout.retiredKey(2, 1, 0), encodeRetiredSet(rs));

    RetireView v(b, layout);
    v.refresh();
    EXPECT_EQ(v.round(), 2u);
    ASSERT_TRUE(v.findCondemned(ObjectKind::Blob, h).has_value());

    /// GC "drops" the entry by rewriting the retired object WITHOUT it, and bumps gc/state.
    /// Reading current storage state IS the protocol's view.
    {
        auto head = b->head(layout.retiredKey(2, 1, 0));
        ASSERT_TRUE(head.exists);
        ASSERT_EQ(b->putOverwrite(layout.retiredKey(2, 1, 0), encodeRetiredSet(RetiredSet{}), head.token).outcome,
                  PutOutcome::Done);
    }
    {
        auto head = b->head(layout.gcStateKey());
        ASSERT_TRUE(head.exists);
        ASSERT_EQ(b->putOverwrite(layout.gcStateKey(), tests::encodeMinimalGcState(3, 1), head.token).outcome,
                  PutOutcome::Done);
    }

    v.refresh();
    EXPECT_EQ(v.round(), 3u);
    EXPECT_FALSE(v.findCondemned(ObjectKind::Blob, h).has_value());
}

TEST(CasRetireView, MultipleRoundsUnion)
{
    auto b = std::make_shared<InMemoryBackend>();
    Layout layout("p");
    b->putIfAbsent(layout.gcStateKey(), tests::encodeMinimalGcState(2, 1));

    /// Two retired objects at different (round, fence_seq, shard) keys condemn DIFFERENT tokens
    /// of the SAME (kind, hash) — the view is the UNION over all present retired objects.
    const auto h = hexToU128("000102030405060708090a0b0c0d0e0f");
    {
        RetiredSet rs;
        rs.entries.push_back({ObjectKind::Blob, h, Token{"7", TokenType::Emulated}, 10});
        b->putIfAbsent(layout.retiredKey(1, 1, 0), encodeRetiredSet(rs));
    }
    {
        RetiredSet rs;
        rs.entries.push_back({ObjectKind::Blob, h, Token{"9", TokenType::Emulated}, 10});
        b->putIfAbsent(layout.retiredKey(2, 1, 3), encodeRetiredSet(rs));
    }

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

TEST(CasRetireView, PaginationCoversManyObjects)
{
    /// RetireView::refresh lists with an internal page limit of 1000, so five objects would come
    /// back in ONE page and the cursor-continuation loop would never run. Clamp the backend's page
    /// size to 2 so refresh MUST follow next_cursor across multiple pages to see all five.
    class TinyPageBackend : public InMemoryBackend
    {
    public:
        ListPage list(const String & prefix, const String & cursor, size_t limit) override
        {
            return InMemoryBackend::list(prefix, cursor, std::min<size_t>(limit, 2));
        }
    };

    auto b = std::make_shared<TinyPageBackend>();
    Layout layout("p");
    b->putIfAbsent(layout.gcStateKey(), tests::encodeMinimalGcState(5, 1));

    /// Five retired objects under gc/retired/, each condemning a distinct hash. The view must
    /// union ALL of them; with the clamped page size the union completes only if the cursor loop
    /// is correct.
    std::vector<UInt128> hashes;
    for (uint64_t i = 0; i < 5; ++i)
    {
        const auto h = UInt128(i + 1);
        hashes.push_back(h);
        RetiredSet rs;
        rs.entries.push_back({ObjectKind::Tree, h, Token{std::to_string(100 + i), TokenType::Emulated}, i});
        b->putIfAbsent(layout.retiredKey(i + 1, 1, i), encodeRetiredSet(rs));
    }

    RetireView v(b, layout);
    v.refresh();
    EXPECT_EQ(v.round(), 5u);
    for (size_t i = 0; i < hashes.size(); ++i)
    {
        auto hit = v.findCondemned(ObjectKind::Tree, hashes[i]);
        ASSERT_TRUE(hit.has_value()) << "hash " << i;
        ASSERT_EQ(hit->size(), 1u);
        EXPECT_EQ((*hit)[0].value, std::to_string(100 + i));
    }
}

TEST(CasRetireView, IsCondemnedTokenIdentity)
{
    auto b = std::make_shared<InMemoryBackend>();
    Layout layout("p");
    b->putIfAbsent(layout.gcStateKey(), tests::encodeMinimalGcState(1, 1));
    const auto h = hexToU128("000102030405060708090a0b0c0d0e0f");
    RetiredSet rs;
    rs.entries.push_back({ObjectKind::Blob, h, Token{"3", TokenType::Emulated}, 10});
    b->putIfAbsent(layout.retiredKey(1, 1, 0), encodeRetiredSet(rs));

    RetireView v(b, layout);
    v.refresh();

    /// PINNED SEMANTICS: token equality is Token::operator== — value AND type must both match.
    /// The same value under a different TokenType is a DIFFERENT token.
    EXPECT_TRUE(v.isCondemnedToken(ObjectKind::Blob, h, Token{"3", TokenType::Emulated}));
    EXPECT_FALSE(v.isCondemnedToken(ObjectKind::Blob, h, Token{"3", TokenType::ETag}));
    EXPECT_FALSE(v.isCondemnedToken(ObjectKind::Blob, h, Token{"4", TokenType::Emulated}));
}
