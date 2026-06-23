#include <gtest/gtest.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasFsck.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasStore.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasBuild.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasInMemoryBackend.h>
#include <Disks/tests/cas_test_helpers.h>
#include <Common/Exception.h>

#include <algorithm>
#include <chrono>
#include <set>

using namespace DB::Cas;
using DB::Cas::tests::idOf;
using DB::Cas::tests::u128Of;

namespace
{
StorePtr openPool(std::shared_ptr<Backend> backend)
{
    PoolConfig cfg;
    cfg.pool_prefix = "pool";
    cfg.server_id = DB::UInt128(7);
    return Store::open(backend, cfg);
}

/// Simulates LIST eventual-consistency lag: `list` OMITS a chosen key (as if the LIST index has not
/// caught up to a recently-written object), while `head`/`get` of that key still see it (HEAD is
/// authoritative for presence). The fsck must HEAD-confirm a suspected-dangling key before declaring
/// loss — a reachable, present-on-HEAD object absent from a lagging LIST is NOT dangling.
class ListLaggingBackend final : public DB::Cas::Backend
{
public:
    explicit ListLaggingBackend(std::shared_ptr<DB::Cas::Backend> inner_) : inner(std::move(inner_)) {}

    /// Keys that LIST pretends not to see (lagging LIST index). HEAD/GET still hit `inner` unchanged.
    std::set<String> hidden_from_list;

    std::optional<DB::Cas::GetResult> get(const String & k, DB::Cas::Range r = {}) override { return inner->get(k, r); }
    DB::Cas::HeadResult head(const String & k) override { return inner->head(k); }
    DB::Cas::PutResult putIfAbsent(const String & k, const String & b, const DB::Cas::ObjectMeta & meta = {}) override { return inner->putIfAbsent(k, b, meta); }
    DB::Cas::WriteSinkPtr putIfAbsentStream(const String & k, const DB::Cas::ObjectMeta & meta = {}) override { return inner->putIfAbsentStream(k, meta); }
    DB::Cas::PutResult putOverwrite(const String & k, const String & b, const DB::Cas::Token & e, const DB::Cas::ObjectMeta & meta = {}) override { return inner->putOverwrite(k, b, e, meta); }
    DB::Cas::CasResult casPut(const String & k, const String & b, const std::optional<DB::Cas::Token> & e, const DB::Cas::ObjectMeta & meta = {}) override { return inner->casPut(k, b, e, meta); }
    DB::Cas::DeleteOutcome deleteExact(const String & k, const DB::Cas::Token & t) override { return inner->deleteExact(k, t); }

    DB::Cas::ListPage list(const String & p, const String & c, size_t l) override
    {
        DB::Cas::ListPage page = inner->list(p, c, l);
        if (!hidden_from_list.empty())
        {
            auto & keys = page.keys;
            keys.erase(
                std::remove_if(keys.begin(), keys.end(),
                    [&](const DB::Cas::ListedKey & lk) { return hidden_from_list.contains(lk.key); }),
                keys.end());
        }
        return page;
    }
private:
    std::shared_ptr<DB::Cas::Backend> inner;
};

/// Publish a one-blob part `ref` carrying `payload` (shared content => shared blob).
void publishPart(Store & store, const String & payload, const String & ref)
{
    auto build = store.startBuild({});
    build->putBlob(idOf(payload), BlobSource::fromString(payload));
    TreeEntry e; e.name = "data.bin"; e.placement = Placement::Blob;
    e.file_hash = u128Of(payload); e.file_size = payload.size();
    auto tree = build->putTree({e});
    build->publish(RootNamespace{"uui/uuid-1"}, ref, tree, {});
}
}

TEST(CasFsck, CleanPoolHasNoDangling)
{
    auto backend = std::make_shared<InMemoryBackend>();
    auto store = openPool(backend);
    publishPart(*store, "shared-A", "all_1_1_0");
    publishPart(*store, "shared-A", "all_2_2_0");   /// identical content => same blob (dedup)

    auto report = runFsck(*store, /*detail=*/true);
    EXPECT_EQ(report.dangling, 0u);
    EXPECT_TRUE(report.clean());
    EXPECT_EQ(report.unreachable, 0u);
    EXPECT_EQ(report.distinct_blobs, 1u);
    EXPECT_EQ(report.total_blob_refs, 2u);
}

// #5: the progress callback fires during the scan and does NOT alter the report (purely observational).
TEST(CasFsck, ProgressCallbackFiresAndIsObservational)
{
    auto backend = std::make_shared<InMemoryBackend>();
    auto store = openPool(backend);
    publishPart(*store, "shared-A", "all_1_1_0");
    publishPart(*store, "shared-B", "all_2_2_0");

    const auto baseline = runFsck(*store, /*detail=*/true);

    uint64_t calls = 0;
    const auto with_progress = runFsck(*store, /*detail=*/true,
        [&](std::string_view, uint64_t, uint64_t) { ++calls; });

    EXPECT_GT(calls, 0u);   // at least the end-of-listAll calls (blobs/trees/packs) fire
    EXPECT_EQ(with_progress.reachable, baseline.reachable);
    EXPECT_EQ(with_progress.dangling, baseline.dangling);
    EXPECT_EQ(with_progress.unreachable, baseline.unreachable);
    EXPECT_EQ(with_progress.physical_bytes, baseline.physical_bytes);
}

// #5: an already-expired deadline makes runFsck throw (bounded scan, not an opaque hang).
TEST(CasFsck, ExpiredDeadlineThrowsTimeout)
{
    auto backend = std::make_shared<InMemoryBackend>();
    auto store = openPool(backend);
    publishPart(*store, "shared-A", "all_1_1_0");

    const auto past = std::chrono::steady_clock::now() - std::chrono::seconds(1);
    EXPECT_THROW(runFsck(*store, /*detail=*/false, {}, past), DB::Exception);
}

TEST(CasFsck, MissingReachableBlobIsDangling)
{
    auto backend = std::make_shared<InMemoryBackend>();
    auto store = openPool(backend);
    publishPart(*store, "lonely", "all_1_1_0");

    const String bkey = store->layout().blobKey(idOf("lonely"));
    const auto tok = backend->head(bkey).token;
    ASSERT_EQ(backend->deleteExact(bkey, tok).kind, DeleteOutcome::Kind::Deleted);

    auto report = runFsck(*store, /*detail=*/true);
    EXPECT_EQ(report.dangling, 1u);
    EXPECT_FALSE(report.clean());
    auto it = std::find_if(report.objects.begin(), report.objects.end(),
        [&](const FsckObject & o) { return o.cls == FsckClass::Dangling; });
    ASSERT_NE(it, report.objects.end());
    EXPECT_EQ(it->key, bkey);
    EXPECT_FALSE(it->reachable_from.empty());
}

TEST(CasFsck, DroppedButUnreclaimedBlobIsUnreachable)
{
    auto backend = std::make_shared<InMemoryBackend>();
    auto store = openPool(backend);
    publishPart(*store, "ghost", "all_1_1_0");
    store->dropRef(RootNamespace{"uui/uuid-1"}, "all_1_1_0");

    auto report = runFsck(*store, /*detail=*/false);
    EXPECT_EQ(report.dangling, 0u);
    EXPECT_GE(report.unreachable, 1u);
}

/// B141 false-positive: a reachable object that is genuinely PRESENT (HEAD finds it) but ABSENT from a
/// LAGGING LIST (eventual consistency / mid-churn). fsck must HEAD-confirm before declaring loss, so
/// this must classify as reachable, NOT dangling.
TEST(CasFsck, ListLagDoesNotFalseFlagPresentObjectAsDangling)
{
    auto inner = std::make_shared<InMemoryBackend>();
    auto backend = std::make_shared<ListLaggingBackend>(inner);
    auto store = openPool(backend);
    publishPart(*store, "lonely", "all_1_1_0");

    /// The blob IS present (HEAD/GET succeed) but the LIST index lags and omits it.
    const String bkey = store->layout().blobKey(idOf("lonely"));
    ASSERT_TRUE(backend->head(bkey).exists);
    backend->hidden_from_list.insert(bkey);

    auto report = runFsck(*store, /*detail=*/true);
    EXPECT_EQ(report.dangling, 0u);
    EXPECT_TRUE(report.clean());
    /// It must be accounted as reachable, never dangling.
    auto it = std::find_if(report.objects.begin(), report.objects.end(),
        [&](const FsckObject & o) { return o.key == bkey; });
    ASSERT_NE(it, report.objects.end());
    EXPECT_EQ(it->cls, FsckClass::Reachable);
}

/// True loss: a reachable object that HEAD ALSO cannot find (and is absent from LIST) is genuinely
/// dangling — the HEAD-confirm must NOT mask a real INV-NO-LOSS violation.
TEST(CasFsck, HeadAbsentReachableIsStillDangling)
{
    auto inner = std::make_shared<InMemoryBackend>();
    auto backend = std::make_shared<ListLaggingBackend>(inner);
    auto store = openPool(backend);
    publishPart(*store, "lonely", "all_1_1_0");

    /// Actually delete the blob: HEAD now says absent (true loss), and it is absent from LIST too.
    const String bkey = store->layout().blobKey(idOf("lonely"));
    const auto tok = backend->head(bkey).token;
    ASSERT_EQ(backend->deleteExact(bkey, tok).kind, DeleteOutcome::Kind::Deleted);
    ASSERT_FALSE(backend->head(bkey).exists);

    auto report = runFsck(*store, /*detail=*/true);
    EXPECT_EQ(report.dangling, 1u);
    EXPECT_FALSE(report.clean());
    auto it = std::find_if(report.objects.begin(), report.objects.end(),
        [&](const FsckObject & o) { return o.cls == FsckClass::Dangling; });
    ASSERT_NE(it, report.objects.end());
    EXPECT_EQ(it->key, bkey);
}
