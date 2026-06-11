#include <gtest/gtest.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasBackend.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasInMemoryBackend.h>
#include <functional>
#include <memory>

using namespace DB::Cas;

/// Parameterized contract suite: every case creates a fresh backend from the factory,
/// then exercises the Backend seam generically (no InMemoryBackend-specific calls).
/// Fault-injection-only features are excluded — those are InMemory-specific tests.
class CasBackendContract : public ::testing::TestWithParam<std::function<BackendPtr()>>
{
protected:
    BackendPtr make() { return GetParam()(); }
};

TEST_P(CasBackendContract, PutIfAbsentAndGet)
{
    auto b = GetParam()();
    Token t1;
    EXPECT_EQ(b->putIfAbsent("k", "v1", &t1), PutOutcome::Done);
    EXPECT_FALSE(t1.empty());
    EXPECT_EQ(b->putIfAbsent("k", "clobber"), PutOutcome::PreconditionFailed);
    auto g = b->get("k");
    ASSERT_TRUE(g.has_value());
    EXPECT_EQ(g->bytes, "v1");
    EXPECT_EQ(g->token, t1);
    EXPECT_FALSE(b->get("absent").has_value());
}

TEST_P(CasBackendContract, OverwriteIsTokenExactAndMintsFreshToken)
{
    auto b = GetParam()();
    Token t1, t2;
    b->putIfAbsent("k", "v1", &t1);
    EXPECT_EQ(b->putOverwrite("k", "v2", Token{"wrong", TokenType::Emulated}), PutOutcome::PreconditionFailed);
    EXPECT_EQ(b->get("k")->bytes, "v1");                       // untouched on mismatch
    EXPECT_EQ(b->putOverwrite("k", "v2", t1, &t2), PutOutcome::Done);
    EXPECT_NE(t2, t1);                                         // tokens never repeat
    EXPECT_EQ(b->get("k")->bytes, "v2");
}

TEST_P(CasBackendContract, CasPutCreateAndSwap)
{
    auto b = GetParam()();
    Token t1, t2;
    EXPECT_EQ(b->casPut("m", "s1", std::nullopt, &t1), CasOutcome::Committed);     // create-if-absent
    EXPECT_EQ(b->casPut("m", "s1x", std::nullopt), CasOutcome::Conflict);          // exists now
    EXPECT_EQ(b->casPut("m", "s2", Token{"stale", TokenType::Emulated}), CasOutcome::Conflict);
    EXPECT_EQ(b->get("m")->bytes, "s1");
    EXPECT_EQ(b->casPut("m", "s2", t1, &t2), CasOutcome::Committed);
    EXPECT_EQ(b->get("m")->bytes, "s2");
}

TEST_P(CasBackendContract, DeleteExactnessAndSurvival)
{
    auto b = GetParam()();
    Token t1;
    b->putIfAbsent("k", "v1", &t1);
    auto d1 = b->deleteExact("k", Token{"wrong", TokenType::Emulated});
    EXPECT_EQ(d1.kind, DeleteOutcome::Kind::TokenMismatch);
    EXPECT_TRUE(b->get("k").has_value());                      // SURVIVES wrong-token delete
    auto d2 = b->deleteExact("k", t1);
    EXPECT_EQ(d2.kind, DeleteOutcome::Kind::Deleted);
    EXPECT_FALSE(d2.created_delete_marker);
    EXPECT_FALSE(b->get("k").has_value());
}

TEST_P(CasBackendContract, DeleteNotFound)
{
    auto b = GetParam()();
    Token t1;
    b->putIfAbsent("k", "v1", &t1);
    b->deleteExact("k", t1);
    EXPECT_EQ(b->deleteExact("k", t1).kind, DeleteOutcome::Kind::NotFound);
}

TEST_P(CasBackendContract, RangeGet)
{
    auto b = GetParam()();
    b->putIfAbsent("k", "0123456789");
    Range r;
    r.offset = 2;
    r.length = 3u;
    EXPECT_EQ(b->get("k", r)->bytes, "234");
}

TEST_P(CasBackendContract, Head)
{
    auto b = GetParam()();
    b->putIfAbsent("k", "hello");
    auto h = b->head("k");
    EXPECT_TRUE(h.exists);
    EXPECT_EQ(h.size, 5u);
    EXPECT_FALSE(h.token.empty());
    auto h2 = b->head("missing");
    EXPECT_FALSE(h2.exists);
}

TEST_P(CasBackendContract, ListPagination)
{
    auto b = GetParam()();
    b->putIfAbsent("p/a", "0123456789");
    b->putIfAbsent("p/b", "xy");
    b->putIfAbsent("q/c", "z");
    auto page = b->list("p/", "", 10);
    ASSERT_EQ(page.keys.size(), 2u);                          // sorted, prefix-scoped
    EXPECT_EQ(page.keys[0].key, "p/a");
    EXPECT_EQ(page.keys[1].key, "p/b");
    EXPECT_TRUE(page.next_cursor.empty());
    auto page1 = b->list("p/", "", 1);                        // pagination
    EXPECT_EQ(page1.keys.size(), 1u);
    EXPECT_FALSE(page1.next_cursor.empty());
    auto page2 = b->list("p/", page1.next_cursor, 1);
    EXPECT_EQ(page2.keys[0].key, "p/b");
}

TEST_P(CasBackendContract, ReadAfterWrite)
{
    auto b = GetParam()();
    Token t1;
    b->putIfAbsent("rw", "payload", &t1);
    auto g = b->get("rw");
    ASSERT_TRUE(g.has_value());
    EXPECT_EQ(g->bytes, "payload");
    EXPECT_EQ(g->token, t1);
    auto h = b->head("rw");
    EXPECT_TRUE(h.exists);
    EXPECT_EQ(h.token, t1);
}

INSTANTIATE_TEST_SUITE_P(InMemory, CasBackendContract,
    ::testing::Values(+[]() -> BackendPtr { return std::make_shared<InMemoryBackend>(); }));
