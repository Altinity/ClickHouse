#include <gtest/gtest.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasBuild.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasEvent.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasGc.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasStore.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasInMemoryBackend.h>
#include <Disks/tests/cas_test_helpers.h>
#include <mutex>
#include <vector>
using namespace DB::Cas;
using DB::Cas::tests::idOf;
using DB::Cas::tests::u128Of;
TEST(CasEvent, ConstructAndCopyAndName)
{
    CasEvent e;
    e.type = CasEventType::BlobDelete;
    e.object_kind = CasEventObjectKind::Blob;
    e.object_hash = "abcd";
    e.token = "tok";
    e.round = 7; e.gen = 3;
    e.reason = "in-degree 0 after strip";
    e.detail["freed"] = "10";
    CasEvent c = e;
    EXPECT_EQ(c.type, CasEventType::BlobDelete);
    EXPECT_EQ(c.object_hash, "abcd");
    EXPECT_EQ(c.detail.at("freed"), "10");
    EXPECT_EQ(toString(CasEventType::BlobDelete), "blob_delete");
    EXPECT_EQ(toString(CasEventType::IndegZero), "indeg_zero");
    EXPECT_EQ(toString(CasEventType::GcRecheckVerdict), "gc_recheck_verdict");
    EXPECT_EQ(toString(CasEventObjectKind::Tree), "tree");
}

TEST(CasEvent, StoreEmitsToSink)
{
    auto b = std::make_shared<InMemoryBackend>();
    auto s = Store::open(b, PoolConfig{.pool_prefix = "p"});
    std::vector<CasEvent> seen;
    s->setEventSink([&](const CasEvent & e){ seen.push_back(e); });
    CasEvent e;
    e.type = CasEventType::BlobPut;
    e.object_hash = "h";
    s->emitEvent(e);
    ASSERT_EQ(seen.size(), 1u);
    EXPECT_EQ(seen[0].type, CasEventType::BlobPut);
    /// null sink => no-op (no crash, no row)
    s->setEventSink(nullptr);
    s->emitEvent(e);
    EXPECT_EQ(seen.size(), 1u);
}

namespace
{

/// A single-blob part: upload one blob, build a one-entry tree naming it, publish the ref. Returns
/// the blob's object_hash (lowercase hex) so the test can filter the captured rows by it.
String publishOneBlobPart(const StorePtr & s, const String & ns, const String & ref, const String & payload)
{
    auto build = s->startBuild({});
    build->putBlob(idOf(payload), BlobSource::fromString(payload));
    TreeEntry e;
    e.name = "data.bin";
    e.placement = Placement::Blob;
    e.file_hash = u128Of(payload);
    e.file_size = payload.size();
    const TreeId tree = build->putTree({e});
    build->publish(RootNamespace{ns}, ref, tree, {});
    return u128ToHex(u128Of(payload));
}

void runGcToFixpoint(Gc & gc, size_t max_rounds = 64)
{
    for (size_t r = 0; r < max_rounds; ++r)
    {
        const RoundReport rep = gc.runRegularRound();
        if (!rep.acquired_lease)
            continue;
        if (rep.candidates == 0 && rep.deleted == 0 && rep.absent == 0
            && rep.replaced == 0 && rep.spared == 0)
            break;
    }
}

bool hasType(const std::vector<CasEvent> & events, CasEventType t)
{
    for (const auto & e : events)
        if (e.type == t)
            return true;
    return false;
}

}

/// B170 Task 4 acceptance: drive a full publish -> drop -> GC-to-delete lifecycle through a capturing
/// sink and assert (a) the taxonomy of events is emitted, (b) EVERY event carries a non-empty reason,
/// (c) filtering by a deleted blob's object_hash reconstructs its edge/retire/delete chain in order.
TEST(CasEvent, LifecycleReconstructionFromRows)
{
    auto b = std::make_shared<InMemoryBackend>();
    auto s = Store::open(b, PoolConfig{.pool_prefix = "p"});

    std::vector<CasEvent> events;
    std::mutex events_mutex;
    s->setEventSink([&](const CasEvent & e)
    {
        std::lock_guard lock(events_mutex);
        events.push_back(e);
    });

    const RootNamespace ns{"srv1/tbl"};
    const String ref = "all_0_0_0";
    const String payload = "the-doomed-blob-payload";

    /// publish -> the blob's whole closure is born and a ref names it.
    const String blob_hash = publishOneBlobPart(s, ns.string(), ref, payload);

    /// drop the ref and advance the watermark so the now-unreferenced closure is collectable.
    s->dropRef(ns, ref);
    s->renewWatermarkOnce();

    /// GC reclaims the tree and the blob to a fixpoint.
    Gc gc(s, u128Of("gc-event-log"));
    runGcToFixpoint(gc);

    /// The blob must actually be gone (the delete fired).
    ASSERT_FALSE(b->head(s->layout().blobKey(BlobId{blob_hash})).exists)
        << "GC must have deleted the now-unreferenced blob";

    /// (a) the expected taxonomy was emitted across the lifecycle.
    EXPECT_TRUE(hasType(events, CasEventType::BlobPut));
    EXPECT_TRUE(hasType(events, CasEventType::TreePut));
    EXPECT_TRUE(hasType(events, CasEventType::RefPublish));
    EXPECT_TRUE(hasType(events, CasEventType::RootAdd) || hasType(events, CasEventType::TreeExpand))
        << "a fold must have recorded the root edge / expanded the tree";
    EXPECT_TRUE(hasType(events, CasEventType::RefDrop));
    EXPECT_TRUE(hasType(events, CasEventType::IndegZero));
    EXPECT_TRUE(hasType(events, CasEventType::GcRetireObserve)
        || hasType(events, CasEventType::GcRetireDecision)
        || hasType(events, CasEventType::GcRecheckVerdict))
        << "a GC retire/recheck transition must be recorded";
    EXPECT_TRUE(hasType(events, CasEventType::BlobDelete) || hasType(events, CasEventType::TreeDelete))
        << "the single content-delete site must emit a delete row";

    /// (b) completeness mandate: every emitted event has a non-empty reason (the human WHY).
    for (const auto & e : events)
        EXPECT_FALSE(e.reason.empty())
            << "event " << toString(e.type) << " (" << e.object_hash << ") has an empty reason";

    /// (c) lifecycle reconstruction: filtering by the deleted blob's object_hash yields, in time
    /// order, at least its in-degree-zero -> retire-observe -> delete chain — its whole story.
    std::vector<CasEventType> chain;
    for (const auto & e : events)
        if (e.object_hash == blob_hash)
            chain.push_back(e.type);

    ASSERT_FALSE(chain.empty()) << "no rows reference the deleted blob " << blob_hash;

    /// The decisive ordering: the blob's in-degree hit 0 BEFORE GC observed/condemned it, which was
    /// BEFORE it was deleted. Find the first index of each and assert the order.
    auto firstIndexOf = [&](CasEventType t) -> int
    {
        for (size_t i = 0; i < chain.size(); ++i)
            if (chain[i] == t)
                return static_cast<int>(i);
        return -1;
    };
    const int i_indeg = firstIndexOf(CasEventType::IndegZero);
    const int i_observe = firstIndexOf(CasEventType::GcRetireObserve);
    const int i_delete = firstIndexOf(CasEventType::BlobDelete);
    ASSERT_GE(i_indeg, 0) << "the blob's indeg_zero must be in its chain";
    ASSERT_GE(i_observe, 0) << "the blob's gc_retire_observe must be in its chain";
    ASSERT_GE(i_delete, 0) << "the blob's blob_delete must be in its chain";
    EXPECT_LT(i_indeg, i_observe) << "in-degree hit 0 before GC observed it";
    EXPECT_LT(i_observe, i_delete) << "GC observed it before deleting it";
}
