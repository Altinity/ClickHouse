#include <gtest/gtest.h>
#include "cas_test_helpers.h"

#include <memory>
#include <string>
#include <vector>

/// The namespace-file REQUEST PROFILE gate (directive §dedup-performance-constraint) -- for the
/// `Pool` namespace-file surface, which is the layer that must be read carefully below.
///
/// `MergeTreeDeduplicationLog` rotates namespace files on the insert path because the CA disk cannot
/// append, so every namespace-file operation's request profile is insert latency. The directive's
/// constraint has five clauses: no catalog request per file operation, no ref-log append, no blob
/// upload, no folder-manifest rewrite, and unchanged direct-object backend request counts.
///
/// WHAT THIS FILE PINS: the last clause, per key. The counts below were READ OFF this tree before any
/// key change and pasted as literals, which is the whole point of the file -- expectations re-derived
/// after a change measure the change against itself. Incarnation qualification changes the KEY a
/// namespace file is stored under, so the keys are derived from `Layout` rather than spelled out; what
/// must not move is the count per key and the set of keys touched.
///
/// WHAT IT DOES NOT PIN, stated so the next implementer of this constraint learns it here instead of
/// rediscovering it: the four "no ..." clauses are NOT fenced by these cases. Every case drives
/// `Pool::putNamespaceFile`/`getNamespaceFile`/`removeNamespaceFile`/`listNamespaceFiles`, which reach
/// `CasPlainObjects` and have no catalog, ref-log, blob or manifest path to take -- so those four
/// negatives hold by construction of the call, not by anything measured here. The layer where they can
/// actually be violated is the DISK operation above: `ContentAddressedTransaction::writeFile` resolving
/// its namespace through `ContentAddressedMetadataStorage::liveNamespace` is where a catalog GET would
/// be introduced, and no fixture in this file can observe it (the metadata storage builds its own
/// `Backend` from an `ObjectStoragePtr` and accepts no injected one, which is why the append case here
/// is the two-call composition rather than the real write buffer). Fencing those four clauses needs a
/// test at that layer.

using namespace DB::Cas;
using namespace DB::Cas::tests;

namespace
{

const String kNsString = "test/req_profile@cas@";
const String kFile = "format_version.txt";
/// A NESTED relative name, which is what the dedup log actually stores (its segments live in a
/// table-level subdirectory), so the profile is captured on the shape the constraint is about.
const String kSegment1 = "deduplication_logs/deduplication_log_1.txt";
const String kSegment2 = "deduplication_logs/deduplication_log_2.txt";

/// The identity every case below operates under. `stageATransition` is the transitional mint Task 6
/// deletes; what matters to this file is only that ONE life is used throughout, so a count is not
/// split across two prefixes.
NamespaceLifeId testLife()
{
    return NamespaceLifeId::stageATransition(RootNamespace{kNsString});
}

/// A pool over `CountingBackend`, with the counts reset AFTER open: `Pool::open` runs its own
/// capability probe and mount claim, and those requests belong to no file operation.
PoolPtr openCountedPool(std::shared_ptr<CountingBackend> & out_backend)
{
    out_backend = std::make_shared<CountingBackend>();
    PoolPtr store = openPoolForTest(out_backend);
    out_backend->resetCounts();
    return store;
}

}

/// CREATE (the key is absent) and REWRITE (the key is present) are different request shapes on the
/// same call, and the profile pins both: one HEAD to learn the token, then the create-if-absent or the
/// token-conditioned replacement that HEAD selected.
TEST(CasNamespaceFileRequestProfile, CreateThenRewrite)
{
    std::shared_ptr<CountingBackend> backend;
    PoolPtr store = openCountedPool(backend);
    const NamespaceLifeId life = testLife();
    const String key = store->layout().namespaceFileKey(life, kFile);

    store->putNamespaceFile(life, kFile, "1\n");

    EXPECT_EQ(backend->headCount(key), 1u);
    EXPECT_EQ(backend->putCount(key), 1u);            /// putIfAbsent -- the key was absent
    EXPECT_EQ(backend->putOverwriteCount(key), 0u);
    EXPECT_EQ(backend->getCount(key), 0u);
    EXPECT_EQ(backend->deleteCount(key), 0u);
    EXPECT_EQ(backend->listTotal(), 0u);
    EXPECT_EQ(backend->casPutTotal(), 0u);
    EXPECT_EQ(backend->touchedKeys(), std::vector<String>{key});

    backend->resetCounts();
    store->putNamespaceFile(life, kFile, "2\n");

    EXPECT_EQ(backend->headCount(key), 1u);
    EXPECT_EQ(backend->putOverwriteCount(key), 1u);   /// token-conditioned replacement -- it existed
    EXPECT_EQ(backend->putCount(key), 0u);
    EXPECT_EQ(backend->getCount(key), 0u);
    EXPECT_EQ(backend->deleteCount(key), 0u);
    EXPECT_EQ(backend->listTotal(), 0u);
    EXPECT_EQ(backend->casPutTotal(), 0u);
    EXPECT_EQ(backend->touchedKeys(), std::vector<String>{key});
}

/// A plain read is one whole-object GET and nothing else.
TEST(CasNamespaceFileRequestProfile, Read)
{
    std::shared_ptr<CountingBackend> backend;
    PoolPtr store = openCountedPool(backend);
    const NamespaceLifeId life = testLife();
    const String key = store->layout().namespaceFileKey(life, kFile);

    store->putNamespaceFile(life, kFile, "1\n");
    backend->resetCounts();

    EXPECT_EQ(store->getNamespaceFile(life, kFile), String("1\n"));

    EXPECT_EQ(backend->getCount(key), 1u);
    EXPECT_EQ(backend->wholeGetCount(key), 1u);
    EXPECT_EQ(backend->headCount(key), 0u);
    EXPECT_EQ(backend->putCount(key), 0u);
    EXPECT_EQ(backend->putOverwriteCount(key), 0u);
    EXPECT_EQ(backend->touchedKeys(), std::vector<String>{key});
}

/// APPEND on a CA disk is serviced by read-modify-rewrite, and its request shape is the composition of
/// the two calls that implement it: a GET of the current body, then a whole-body PUT of base+delta.
/// Driven here as that composition against the same key, which is the shape whose count must not move.
TEST(CasNamespaceFileRequestProfile, ReadModifyRewriteAppend)
{
    std::shared_ptr<CountingBackend> backend;
    PoolPtr store = openCountedPool(backend);
    const NamespaceLifeId life = testLife();
    const String key = store->layout().namespaceFileKey(life, kSegment1);

    store->putNamespaceFile(life, kSegment1, "base");
    backend->resetCounts();

    const std::optional<String> carried = store->getNamespaceFile(life, kSegment1);
    ASSERT_TRUE(carried.has_value());
    store->putNamespaceFile(life, kSegment1, *carried + "-delta");

    EXPECT_EQ(backend->getCount(key), 1u);
    EXPECT_EQ(backend->headCount(key), 1u);
    EXPECT_EQ(backend->putOverwriteCount(key), 1u);
    EXPECT_EQ(backend->putCount(key), 0u);
    EXPECT_EQ(backend->deleteCount(key), 0u);
    EXPECT_EQ(backend->listTotal(), 0u);
    EXPECT_EQ(backend->touchedKeys(), std::vector<String>{key});
    EXPECT_EQ(store->getNamespaceFile(life, kSegment1), String("base-delta"));
}

/// REMOVE is exact-token deletion, so it is one HEAD for the token plus one delete against it.
TEST(CasNamespaceFileRequestProfile, Remove)
{
    std::shared_ptr<CountingBackend> backend;
    PoolPtr store = openCountedPool(backend);
    const NamespaceLifeId life = testLife();
    const String key = store->layout().namespaceFileKey(life, kFile);

    store->putNamespaceFile(life, kFile, "1\n");
    backend->resetCounts();

    store->removeNamespaceFile(life, kFile);

    EXPECT_EQ(backend->headCount(key), 1u);
    EXPECT_EQ(backend->deleteCount(key), 1u);
    EXPECT_EQ(backend->getCount(key), 0u);
    EXPECT_EQ(backend->putCount(key), 0u);
    EXPECT_EQ(backend->putOverwriteCount(key), 0u);
    EXPECT_EQ(backend->listTotal(), 0u);
    EXPECT_EQ(backend->touchedKeys(), std::vector<String>{key});
    EXPECT_FALSE(store->getNamespaceFile(life, kFile).has_value());
}

/// ROTATION is the sequence the constraint names: the retiring segment is enumerated, the new segment
/// is created, and the retired one is removed. One LIST of the files prefix serves the enumeration (a
/// single page here), and each segment carries its own create or remove shape.
TEST(CasNamespaceFileRequestProfile, DedupLogRotation)
{
    std::shared_ptr<CountingBackend> backend;
    PoolPtr store = openCountedPool(backend);
    const NamespaceLifeId life = testLife();
    const String prefix = store->layout().namespaceFilesPrefix(life);
    const String old_key = store->layout().namespaceFileKey(life, kSegment1);
    const String new_key = store->layout().namespaceFileKey(life, kSegment2);

    store->putNamespaceFile(life, kSegment1, "segment-1-records");
    backend->resetCounts();

    const std::vector<String> before = store->listNamespaceFiles(life);
    ASSERT_EQ(before, std::vector<String>{kSegment1});
    store->putNamespaceFile(life, kSegment2, "segment-2-records");
    store->removeNamespaceFile(life, kSegment1);

    EXPECT_EQ(backend->listCount(prefix), 1u);
    EXPECT_EQ(backend->listTotal(), 1u);
    EXPECT_EQ(backend->headCount(new_key), 1u);
    EXPECT_EQ(backend->putCount(new_key), 1u);
    EXPECT_EQ(backend->putOverwriteCount(new_key), 0u);
    EXPECT_EQ(backend->headCount(old_key), 1u);
    EXPECT_EQ(backend->deleteCount(old_key), 1u);
    EXPECT_EQ(backend->getTotal(), 0u);              /// rotation reads no body
    EXPECT_EQ(backend->casPutTotal(), 0u);
    /// Sorted, and the files prefix is a proper prefix of both segment keys, so it comes first.
    EXPECT_EQ(backend->touchedKeys(), (std::vector<String>{prefix, old_key, new_key}));

    EXPECT_EQ(store->listNamespaceFiles(life), std::vector<String>{kSegment2});
}
