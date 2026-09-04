#include <gtest/gtest.h>

#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Backend/CasInMemoryBackend.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Backend/CasThrottlingBackend.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Gc/CasGc.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasPartWriteTxn.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasPool.h>
#include <Disks/tests/cas_test_helpers.h>

#include "config.h"

#if USE_AWS_S3

using namespace DB::Cas;
using DB::Cas::tests::CountingBackend;

namespace
{

/// The PartWriteTxn fixture every pool test uses: stage an empty manifest, precommit it under `ref`,
/// promote it. Empty content is enough -- this gate exercises the request contract under throttling,
/// not the blob path.
void publishEmptyPart(const PoolPtr & store, const RootNamespace & ns, const String & ref)
{
    PartWriteInfo info;
    info.intended_namespace = ns;
    info.intended_ref = ns.string() + "/" + ref;
    auto build = store->beginPartWrite(info);
    const ManifestId id = build->stageManifest({});
    build->precommitAdd(ns, ref, id);
    build->promote(ns, ref, build->buildId(), id);
}

/// Every key the total per-key request count a `CountingBackend` observed, summed across every
/// primitive: whichever verb the throttled key was refused on, this is what "requested again later"
/// means.
uint64_t totalRequestsFor(const CountingBackend & inner, const String & key)
{
    return inner.getCount(key) + inner.headCount(key) + inner.listCount(key) + inner.writeCount(key)
        + inner.deleteCount(key) + inner.publishCount(key);
}

}

/// The `ThrottlingBackend` gate (plan Task 21): every user-visible statement -- table creation, an
/// insert, a rename, a drop, the writable mount `Pool::open` itself performs, and one GC round -- must
/// still SUCCEED when every key it touches is refused exactly once (`FirstPerKey`, HTTP 429) before it
/// is honored. `refusals(key) == 1` for every key the gate recorded, and every one of them was requested
/// at least twice: once refused, at least once more to actually land.
///
/// EXCLUDED, by design, and never reached by these scenarios: the in-band recovery walk's epoch seal at
/// `{E, T+1}` -- nothing here trips a fence, forces a remount, or drives recovery.
TEST(CASThrottlingGate, EveryUserVisibleStatementSucceedsUnderFirstPerKeyThrottling)
{
    auto inner = std::make_shared<CountingBackend>();
    auto throttled = std::make_shared<ThrottlingBackend>(
        inner, ThrottlingBackend::Mode::FirstPerKey, /*n=*/0, /*status=*/429);

    /// The writable mount at open, probe included: `Pool::open` itself issues the identity probe, the
    /// mount claim and the epoch allocation under this same throttled backend.
    auto store = DB::Cas::tests::openPoolForTest(throttled);

    const RootNamespace ns{"test/throttle_gate"};

    /// CREATE-shaped: the namespace's first part write births it.
    ASSERT_NO_THROW(publishEmptyPart(store, ns, "created"));
    EXPECT_TRUE(store->resolveRef(ns, "created").has_value());

    /// INSERT-shaped: a second part write into the now-live namespace.
    ASSERT_NO_THROW(publishEmptyPart(store, ns, "inserted"));
    EXPECT_TRUE(store->resolveRef(ns, "inserted").has_value());

    /// RENAME-shaped: content addressing has no rename primitive (`PartFolderAccess::republishRef`'s own
    /// comment) -- a rename publishes equivalent content at the destination ref and drops the source.
    ASSERT_NO_THROW(publishEmptyPart(store, ns, "renamed"));
    ASSERT_NO_THROW(store->dropRef(ns, "inserted"));
    EXPECT_TRUE(store->resolveRef(ns, "renamed").has_value());
    EXPECT_FALSE(store->resolveRef(ns, "inserted").has_value());

    /// One GC round, still under throttling.
    Gc gc(store, UInt128{7101});
    ASSERT_NO_THROW(DB::Cas::tests::runRegularRoundReclaiming(gc));

    /// DROP-shaped: the whole namespace goes last, so the statements above still have something to act on.
    ASSERT_NO_THROW(store->dropNamespace(ns));

    /// Every key the gate ever decided must have been refused EXACTLY once and requested again
    /// afterwards -- a refusal count above one means the same key was throttled more than the
    /// `FirstPerKey` contract promises; an inner (post-refusal) request count of zero means the caller
    /// never retried at all, which the statements above already ruled out by succeeding.
    for (const String & key : throttled->decidedKeys())
    {
        EXPECT_EQ(throttled->refusals(key), 1u) << "key: " << key;
        EXPECT_GE(totalRequestsFor(*inner, key), 1u)
            << "key: " << key << " -- one refusal plus at least one later success is 'requested at least twice'";
    }
    EXPECT_FALSE(throttled->decidedKeys().empty()) << "the gate must have actually decided some keys";
}

#endif
