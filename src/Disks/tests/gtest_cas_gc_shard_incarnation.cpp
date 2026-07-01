#include <gtest/gtest.h>

#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasGc.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasGcCursorKey.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasInMemoryBackend.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasStore.h>
#include "cas_test_helpers.h"

using namespace DB::Cas;
using namespace DB::Cas::tests;

namespace
{

StorePtr makeStoreWithShards(std::shared_ptr<InMemoryBackend> & out_backend, uint64_t gc_shards = 1)
{
    out_backend = std::make_shared<InMemoryBackend>();
    return Store::open(out_backend,
        PoolConfig{.pool_prefix = "p", .server_root_id = "test", .gc_shards = gc_shards});
}

ManifestRef testRef(uint64_t seq)
{
    return ManifestRef{.writer_epoch = 1, .build_sequence = seq, .manifest_ordinal = 1};
}

}

/// Task 4: LIST-based discovery. Publishing a ref into ns A shard 0 makes (A,0) discoverable;
/// ns B with no shard object is NOT discovered.
TEST(CasGcShardIncarnation, DiscoveryEqualsPresentShards)
{
    for (const uint64_t gc_shards : {1u, 4u})
    {
        std::shared_ptr<InMemoryBackend> backend;
        auto store = makeStoreWithShards(backend, gc_shards);
        Gc gc(store, hexToU128("0000000000000000000000000000000a"));

        const RootNamespace ns_a{"srv1/tblA"};
        const RootNamespace ns_b{"srv1/tblB"};

        /// Write a ref shard for ns A shard 0 only.
        writeManifestRaw(*backend, store->layout(), ns_a, testRef(1), {});
        publishCommittedTransition(*backend, store->layout(), ns_a, "part_1", std::nullopt, testRef(1), /*shard=*/0);

        /// ns B: no shard object written at all.

        const auto universe = gc.discoverUniverseForTest();

        /// Must contain (A, 0).
        bool found_a = false;
        for (const auto & [ns, shard] : universe)
        {
            if (ns.string() == "srv1/tblA" && shard == 0)
                found_a = true;
            EXPECT_NE(ns.string(), "srv1/tblB") << "ns B should not appear in universe (no shard written)";
        }
        EXPECT_TRUE(found_a) << "ns A shard 0 must be in the universe (shard object present)";
    }
}

/// Task 4: listNamespaces is LIST-based; no registry involved.
/// Publishing into ns A makes it appear in listNamespaces(""); ns B absent.
TEST(CasGcShardIncarnation, ListNamespacesFromRefsNotRegistry)
{
    for (const uint64_t gc_shards : {1u, 4u})
    {
        std::shared_ptr<InMemoryBackend> backend;
        auto store = makeStoreWithShards(backend, gc_shards);

        const RootNamespace ns_a{"srv1/tblA"};

        EXPECT_TRUE(store->listNamespaces("").empty());

        /// Write a ref shard for ns A — no registry write.
        writeManifestRaw(*backend, store->layout(), ns_a, testRef(1), {});
        publishCommittedTransition(*backend, store->layout(), ns_a, "part_1", std::nullopt, testRef(1), /*shard=*/0);

        const auto nss = store->listNamespaces("");
        ASSERT_EQ(nss.size(), 1u);
        EXPECT_EQ(nss[0], "srv1/tblA");

        /// Prefix filter: no match.
        EXPECT_TRUE(store->listNamespaces("srv2/").empty());
        /// Prefix filter: match.
        const auto filtered = store->listNamespaces("srv1/");
        ASSERT_EQ(filtered.size(), 1u);
        EXPECT_EQ(filtered[0], "srv1/tblA");
    }
}
