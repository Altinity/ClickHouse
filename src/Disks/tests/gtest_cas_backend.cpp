#include <gtest/gtest.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasBackend.h>

using namespace DB::Cas;

/// Minimal concrete implementation that overrides every pure virtual with trivial defaults.
/// Purpose: verify the interface compiles, is overridable, and result-type defaults are sane.
struct NullBackend final : Backend
{
    std::optional<GetResult> get(const String & /*key*/, Range /*range*/) override
    {
        return std::nullopt;
    }

    HeadResult head(const String & /*key*/) override
    {
        return HeadResult{};
    }

    PutOutcome putIfAbsent(const String & /*key*/, const String & /*bytes*/, Token * /*out_token*/) override
    {
        return PutOutcome::Done;
    }

    PutOutcome putOverwrite(const String & /*key*/, const String & /*bytes*/, const Token & /*expected*/, Token * /*out_token*/) override
    {
        return PutOutcome::PreconditionFailed;
    }

    CasOutcome casPut(const String & /*key*/, const String & /*bytes*/, const std::optional<Token> & /*expected*/, Token * /*out_token*/) override
    {
        return CasOutcome::Conflict;
    }

    DeleteOutcome deleteExact(const String & /*key*/, const Token & /*token*/) override
    {
        return DeleteOutcome{};
    }

    ListPage list(const String & /*prefix*/, const String & /*cursor*/, size_t /*limit*/) override
    {
        return ListPage{};
    }
};

TEST(CasBackend, NullBackendShapeAndDefaults)
{
    NullBackend b;
    // Use the base-class reference so virtual dispatch uses base-class default args.
    Backend & ref = b;

    // get returns absent
    EXPECT_FALSE(ref.get("k").has_value());

    // head returns non-existent
    HeadResult h = b.head("k");
    EXPECT_FALSE(h.exists);
    EXPECT_EQ(h.size, 0u);
    EXPECT_TRUE(h.token.empty());

    // putIfAbsent returns Done
    EXPECT_EQ(ref.putIfAbsent("k", "v"), PutOutcome::Done);

    // putOverwrite returns PreconditionFailed
    EXPECT_EQ(ref.putOverwrite("k", "v", Token{}), PutOutcome::PreconditionFailed);

    // casPut returns Conflict
    EXPECT_EQ(ref.casPut("k", "v", std::nullopt), CasOutcome::Conflict);

    // deleteExact default kind is NotFound
    DeleteOutcome d = b.deleteExact("k", Token{});
    EXPECT_EQ(d.kind, DeleteOutcome::Kind::NotFound);
    EXPECT_FALSE(d.created_delete_marker);

    // list returns empty page
    ListPage page = b.list("p/", "", 10);
    EXPECT_TRUE(page.keys.empty());
    EXPECT_TRUE(page.next_cursor.empty());

    // Range::whole() helper
    EXPECT_TRUE(Range{}.whole());
    Range r1; r1.offset = 1;
    EXPECT_FALSE(r1.whole());
    Range r2; r2.length = 5u;
    EXPECT_FALSE(r2.whole());
}
