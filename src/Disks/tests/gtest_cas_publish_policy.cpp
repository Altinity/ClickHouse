#include <gtest/gtest.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Parts/PartFolderAccess.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasPartWriteTxn.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasPool.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Backend/CasInMemoryBackend.h>
#include <Disks/tests/cas_test_helpers.h>
#include <Common/Exception.h>

namespace DB::ErrorCodes
{
extern const int CAS_OWNERSHIP_CONFLICT;
extern const int NETWORK_ERROR;
}

using namespace DB::Cas;

namespace
{

PoolPtr openPool(const std::shared_ptr<InMemoryBackend> & b)
{
    return Pool::open(b, PoolConfig{.pool_prefix = "p", .server_root_id = "test"});
}

/// One inline-entry manifest naming `path` with content `bytes` (distinct bytes => distinct content).
std::vector<ManifestEntry> inlineEntries(const String & path, const String & bytes)
{
    ManifestEntry e;
    e.path = path;
    e.placement = EntryPlacement::Inline;
    e.inline_bytes = bytes;
    return {e};
}

ManifestId publishCommitted(const PoolPtr & s, const RootNamespace & ns, const String & ref, const std::vector<ManifestEntry> & entries)
{
    auto build = s->beginPartWrite(PartWriteInfo{.intended_ref = ns.string() + "/" + ref, .intended_namespace = ns});
    const ManifestId id = build->stageManifest(entries);
    build->precommitAdd(ns, ref, id);
    build->promote(ns, ref, build->buildId(), id);
    return id;
}

enum class Prior { Absent, SameBytes, Different };
enum class Expect { Publish, Refused, Conflict };

struct Case
{
    bool replace;   /// `replaceIfCurrent(current)` vs `create()`
    Prior committed;
    Expect expect;
};

}

/// The outcome table of PublishPolicy for `publishEntries`, which always stages a FRESH manifest: a
/// committed ref naming the same bytes still names a different manifest, so only `replaceIfCurrent`
/// binds the new one. `Publish` = the destination ends up naming the new manifest (first bind or repoint),
/// `Refused` = the unique-ref guard's write-retry-later error (NETWORK_ERROR) and no change,
/// `Conflict` = CAS_OWNERSHIP_CONFLICT and no change. The byte-equal no-op lives one layer up, in
/// `repointRef` (tested below).
TEST(CASPublishPolicy, OutcomeTable)
{
    const std::vector<Case> cases = {
        {false, Prior::Absent, Expect::Publish},
        {false, Prior::SameBytes, Expect::Refused},
        {false, Prior::Different, Expect::Refused},
        {true, Prior::Absent, Expect::Conflict},
        {true, Prior::SameBytes, Expect::Publish},
        {true, Prior::Different, Expect::Publish},
    };

    for (const auto & c : cases)
    {
        SCOPED_TRACE(String(c.replace ? "replaceIfCurrent" : "create") + " / committed=" + std::to_string(static_cast<int>(c.committed)));
        auto backend = std::make_shared<InMemoryBackend>();
        auto store = openPool(backend);
        CachedPartFolderAccess access(store);
        const RootNamespace ns{"srv/tbl@cas@"};
        const PartRefKey key{ns, "all_0_0_0"};

        std::optional<ManifestRef> current;
        if (c.committed != Prior::Absent)
            current = publishCommitted(store, ns, key.ref, inlineEntries("f", c.committed == Prior::SameBytes ? "NEW" : "OLD")).ref;

        PublishPolicy policy;
        if (c.replace)
        {
            /// Absent: expect a manifest committed under another name, so nothing at `key` satisfies it.
            policy = PublishPolicy::replaceIfCurrent(current ? *current : publishCommitted(store, ns, "other_0_0_0", inlineEntries("f", "OLD")).ref);
        }

        try
        {
            const CommitOutcome oc = access.publishEntries(key, inlineEntries("f", "NEW"), ProvenanceOp::Other, policy);
            ASSERT_EQ(c.expect, Expect::Publish) << "publish succeeded where a refusal was expected";
            EXPECT_EQ(oc.created, c.committed == Prior::Absent);
            const auto resolved = access.resolve(key, Freshness::ForceFresh);
            ASSERT_TRUE(resolved.has_value());
            EXPECT_TRUE(resolved->manifest_id.ref == oc.manifest_ref);
            if (current)
                EXPECT_FALSE(resolved->manifest_id.ref == *current) << "a Publish over a committed ref must rebind it to the fresh manifest";
        }
        catch (const DB::Exception & e)
        {
            ASSERT_NE(c.expect, Expect::Publish) << e.message();
            EXPECT_EQ(e.code(), c.expect == Expect::Refused ? DB::ErrorCodes::NETWORK_ERROR : DB::ErrorCodes::CAS_OWNERSHIP_CONFLICT) << e.message();
            const auto resolved = access.resolve(key, Freshness::ForceFresh);
            if (current)
            {
                ASSERT_TRUE(resolved.has_value());
                EXPECT_TRUE(resolved->manifest_id.ref == *current) << "a refusal must leave the committed manifest unchanged";
            }
            else
                EXPECT_FALSE(resolved.has_value()) << "a refusal must not create the ref";
        }
    }
}

/// ReplaceIfCurrent with a stale expectation: the ref was repointed under the caller.
TEST(CASPublishPolicy, ReplaceIfCurrentRefusesAStaleExpectation)
{
    auto backend = std::make_shared<InMemoryBackend>();
    auto store = openPool(backend);
    CachedPartFolderAccess access(store);
    const RootNamespace ns{"srv/tbl@cas@"};
    const PartRefKey key{ns, "all_0_0_0"};
    const ManifestRef first = publishCommitted(store, ns, key.ref, inlineEntries("f", "v1")).ref;
    const ManifestRef second = access.repointRef(key, inlineEntries("f", "v2"), ProvenanceOp::Other, first).manifest_ref;
    ASSERT_FALSE(first == second);

    try
    {
        access.publishEntries(key, inlineEntries("f", "v3"), ProvenanceOp::Other, PublishPolicy::replaceIfCurrent(first));
        FAIL() << "a stale expectation must not repoint";
    }
    catch (const DB::Exception & e)
    {
        EXPECT_EQ(e.code(), DB::ErrorCodes::CAS_OWNERSHIP_CONFLICT) << e.message();
    }
    EXPECT_TRUE(access.resolve(key, Freshness::ForceFresh)->manifest_id.ref == second);
}

/// `repointRef` adds the byte-equal no-op on top of the table: same decoded entries, zero pool mutation.
TEST(CASPublishPolicy, RepointRefByteEqualIsANoOp)
{
    auto backend = std::make_shared<InMemoryBackend>();
    auto store = openPool(backend);
    CachedPartFolderAccess access(store);
    const RootNamespace ns{"srv/tbl@cas@"};
    const PartRefKey key{ns, "all_0_0_0"};
    const ManifestRef first = publishCommitted(store, ns, key.ref, inlineEntries("f", "v1")).ref;
    const CommitOutcome oc = access.repointRef(key, inlineEntries("f", "v1"), ProvenanceOp::Other, first);
    EXPECT_FALSE(oc.created);
    EXPECT_TRUE(oc.manifest_ref == first);
    EXPECT_TRUE(access.resolve(key, Freshness::ForceFresh)->manifest_id.ref == first);
}
