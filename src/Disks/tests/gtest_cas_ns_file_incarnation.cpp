#include <gtest/gtest.h>

#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Formats/CasFormat.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Formats/CasPoolMetaFormat.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Gc/CasGc.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasPool.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasRefCatalog.h>
#include "cas_test_helpers.h"
#include <algorithm>

namespace DB::ErrorCodes
{
    extern const int LOGICAL_ERROR;
    extern const int UNKNOWN_FORMAT_VERSION;
}

/// Namespace files are keyed by an opaque LIFE, not by its name: `cas/ns/state/<life_id>/_files/<name>`
/// (Stage B Task 4b, directive design change 2). This file pins the three properties that re-key exists
/// to produce, and the one it must NOT produce.
///
/// THE HOLE IT CLOSES. Before the re-key, a namespace file lived at a name-keyed prefix shared by every
/// life of that name. A file the store's LIST omitted therefore survived namespace removal -- nothing
/// enumerated it, so nothing deleted it -- and then became VISIBLE to the next namespace created under
/// the same name, because that namespace read the same prefix. Deletion was load-bearing for
/// correctness, and deletion depends on enumeration, which is the one thing an object store is allowed
/// to be late about (`HintHoleBackendOn` is that lateness as an interface -- see its doc).
///
/// WHY THE KEY IS THE FIX AND THE DELETE IS NOT. After the re-key the old file is at a prefix the new
/// life cannot name. It is unreachable whether or not it was ever deleted, so a blind LIST costs
/// STORAGE and nothing else -- the directive's "LIST omission may only leak storage, never visibility,
/// rebirth or deletion safety". `OldFileHiddenByListIsInvisibleAfterRebirth` asserts exactly that split
/// by leaving the old object physically present and byte-intact.
///
/// WHAT REBIRTH NO LONGER WAITS FOR. Catalog removal depends on folded terminal evidence for the old
/// opaque life, not on a physical-empty proof. `RebirthDoesNotWaitForFilesToBeEmpty` keeps old `_files`
/// bytes present while that evidence is adopted; their later reclamation belongs to the perpetual
/// janitor and is not a precondition for same-name reuse.

using namespace DB::Cas;
using namespace DB::Cas::tests;

namespace
{

const String kNsString = "00/aa@cas@";
const String kFile = "format_version.txt";

/// Two DELIBERATELY DISTINGUISHABLE incarnations. Hand-picked rather than random so a failure message
/// names which life a key belongs to, and both differ from the deterministic fixture id returned by
/// `stageATransition`; the parser must return the id encoded in the key.
const UInt128 kInc1 = hexToU128("11111111111111111111111111111111");
const UInt128 kInc2 = hexToU128("22222222222222222222222222222222");

const UInt128 kGcId = hexToU128("00000000000000000000000000000001");

/// Admit `ns` as `Live` at exactly `incarnation`, through the catalog's own public admission primitive.
void admitLifeAt(Backend & backend, const Layout & layout, const RootNamespace & ns, const UInt128 & incarnation)
{
    CatalogEntry entry;
    entry.ns = ns;
    entry.state = NsState::Live;
    entry.incarnation = incarnation;
    CasRefCatalog::casAdmitEntry(backend, layout, entry);
}

/// Complete the catalog-only half of removal through the production exact-deletion authority. This
/// fixture supplies the adopted-parent evidence that a real prior fold would have made durable.
void retireCatalogEntry(Backend & backend, const Layout & layout, const RootNamespace & ns)
{
    CasRefCatalog::casUpdate(backend, layout, [&](const RefCatalog & current)
    {
        RefCatalog next = current;
        const auto it = std::find_if(next.entries.begin(), next.entries.end(), [&](const CatalogEntry & entry)
        {
            return entry.ns == ns;
        });
        if (it == next.entries.end())
            throw DB::Exception(DB::ErrorCodes::LOGICAL_ERROR, "Missing fixture catalog entry '{}'", ns.string());
        it->state = NsState::Removing;
        it->removal_started_round = 1;
        return next;
    });

    const CasRefCatalog::Snapshot snapshot = CasRefCatalog::read(backend, layout);
    const auto it = std::find_if(snapshot.catalog.entries.begin(), snapshot.catalog.entries.end(), [&](const CatalogEntry & entry)
    {
        return entry.ns == ns;
    });
    ASSERT_NE(it, snapshot.catalog.entries.end());
    CasFoldSeal parent;
    parent.ref_lives.emplace(it->incarnation, RefLifeFoldState{
        .coverage = RefCoverage{.classification = 2, .last_folded_ref_id = RefTxnId{1, 1}},
        .cleanup_evidence = RefCleanupEvidence{.remove_txn_id = RefTxnId{1, 1}}});
    ASSERT_EQ(CasRefCatalog::deleteCompletedRemoving(
        backend, layout, *it, parent, 1,
        [](uint64_t) { return CasRefCatalog::LeaderFenceStatus::Held; }),
        CasRefCatalog::CompletedRemovingDeleteOutcome::Deleted);
}

}

/// THE HEADLINE HOLE. An old life's file that the store's enumeration omits is invisible to the reborn
/// namespace -- and the object is still physically there, which is the point: correctness comes from
/// the KEY, not from having managed to delete it.
TEST(CasNsFileIncarnation, OldFileHiddenByListIsInvisibleAfterRebirth)
{
    auto backend = std::make_shared<HintHoleBackendOn<InMemoryBackend>>();
    PoolPtr store = openPoolForTest(backend);
    const Layout & layout = store->layout();
    const RootNamespace ns{kNsString};

    /// Life 1 writes one namespace file, and the store then starts lying about it in LIST only: `get`,
    /// `head`, `putIfAbsent` and `deleteExact` stay honest, exactly like the real defect.
    admitLifeAt(*backend, layout, ns, kInc1);
    const NamespaceLifeId life1 = NamespaceLifeId::fromCatalogEntry(ns, kInc1);
    store->putNamespaceFile(life1, kFile, "1\n");
    const String key1 = layout.namespaceFileKey(life1, kFile);
    backend->hide(key1);

    ASSERT_TRUE(backend->head(key1).exists) << "the lie must be in LIST only -- the object is durable";
    ASSERT_TRUE(store->listNamespaceFiles(life1).empty())
        << "precondition: enumeration omits the file, so no cleanup pass can ever find it";

    /// The namespace is removed and created again under the SAME NAME at a different life. Nothing
    /// deleted the old file, and nothing can.
    retireCatalogEntry(*backend, layout, ns);
    admitLifeAt(*backend, layout, ns, kInc2);

    /// Resolved the way production resolves it -- from the catalog, not from a life the test built.
    /// Hand-constructing `life2` here would assert that two different keys hold different things,
    /// which was never in doubt; what is in doubt is which life a READER lands on.
    const std::optional<NamespaceLifeId> life2 = store->namespaceFilesLifeIfReadable(ns);
    ASSERT_TRUE(life2.has_value());
    EXPECT_EQ(life2->incarnation, kInc2) << "the reborn namespace must read at its OWN life";

    EXPECT_FALSE(store->getNamespaceFile(*life2, kFile).has_value())
        << "the previous life's file must be structurally unreachable from the new life";
    EXPECT_TRUE(store->listNamespaceFiles(*life2).empty());

    /// The old object is untouched -- storage leaked, visibility not.
    EXPECT_TRUE(backend->head(key1).exists);
    const auto still_there = backend->get(key1);
    ASSERT_TRUE(still_there.has_value());
    EXPECT_EQ(still_there->bytes, "1\n") << "the leak is a storage leak: the bytes are intact and inert";
}

/// The non-minting reader assignment site accepts exactly a catalog `Live` row. `Creating`,
/// `Removing`, and absence neither install a runtime life nor mutate durable catalog/stream state.
TEST(CasNsFileIncarnation, FreshReaderAssignsOnlyLiveCatalogLifeWithoutMutation)
{
    auto backend = std::make_shared<CountingBackend>();
    PoolPtr store = openPoolForTest(backend);
    const Layout & layout = store->layout();
    const RootNamespace creating{"00/creating@cas@"};
    const RootNamespace live{"00/live@cas@"};
    const RootNamespace removing{"00/removing@cas@"};
    const RootNamespace absent{"00/absent@cas@"};

    CasRefCatalog::casAdmitEntry(*backend, layout, CatalogEntry{
        .ns = creating,
        .state = NsState::Creating,
        .incarnation = UInt128{31},
        .creator = CreatorFence{.server_root_id = "foreign", .writer_epoch = 7, .fence_generation = 1}});
    CasRefCatalog::casAdmitEntry(*backend, layout, CatalogEntry{
        .ns = live, .state = NsState::Live, .incarnation = UInt128{32}});
    CasRefCatalog::casAdmitEntry(*backend, layout, CatalogEntry{
        .ns = removing, .state = NsState::Live, .incarnation = UInt128{33}});
    CasRefCatalog::casUpdate(*backend, layout, [&](const RefCatalog & current)
    {
        RefCatalog next = current;
        const auto it = std::find_if(next.entries.begin(), next.entries.end(), [&](const CatalogEntry & entry)
        {
            return entry.ns == removing;
        });
        chassert(it != next.entries.end());
        it->state = NsState::Removing;
        it->removal_started_round = 1;
        return next;
    });

    backend->resetCounts();
    EXPECT_FALSE(store->namespaceFilesLifeIfReadable(creating));
    EXPECT_FALSE(store->namespaceFilesLifeIfReadable(removing));
    EXPECT_FALSE(store->namespaceFilesLifeIfReadable(absent));
    const std::optional<NamespaceLifeId> readable = store->namespaceFilesLifeIfReadable(live);
    ASSERT_TRUE(readable);
    EXPECT_EQ(readable->incarnation, UInt128{32});

    EXPECT_FALSE(store->refTableLifeForTest(creating));
    EXPECT_FALSE(store->refTableLifeForTest(removing));
    EXPECT_FALSE(store->refTableLifeForTest(absent));
    ASSERT_TRUE(store->refTableLifeForTest(live));
    EXPECT_EQ(store->refTableLifeForTest(live)->incarnation, UInt128{32});
    EXPECT_EQ(backend->putTotal(), 0u);
    EXPECT_EQ(backend->putOverwriteTotal(), 0u);
    EXPECT_EQ(backend->casPutTotal(), 0u);
}

/// A real GC fold records terminal evidence for the previous life while its namespace-file debris
/// remains physically present. Lifecycle completion is therefore independent of `_files` enumeration;
/// the perpetual janitor may reclaim the bytes later without participating in the removal proof.
TEST(CasNsFileIncarnation, RebirthDoesNotWaitForFilesToBeEmpty)
{
    auto backend = std::make_shared<CountingBackend>();
    PoolPtr store = openPoolForTest(backend, /*gc_fold_max_defer_rounds*/ 0);
    const Layout & layout = store->layout();
    const RootNamespace ns{kNsString};

    /// A removed namespace (a bare `remove_namespace` transaction -- no committed refs, so no
    /// owner-removal edge confounds this with an unconditional delete path) whose only surviving
    /// physical objects are namespace files: one flat, one nested in the dedup-log shape.
    {
        RefOp remove_op;
        remove_op.kind = RefOpKind::RemoveNamespace;
        appendRefLogSeed(*backend, layout, ns, {remove_op});
    }
    const NamespaceLifeId life = CasRefCatalog::resolveLifeOrSentinel(*backend, layout, ns);
    const String debris_key = layout.namespaceFileKey(life, kFile);
    backend->putIfAbsent(debris_key, "1\n");
    backend->putIfAbsent(layout.namespaceFileKey(life, "deduplication_logs/deduplication_log_1.txt"), "records");

    Gc gc(store, kGcId);
    gc.runRegularRound();

    /// Folding the terminal records positive evidence on the same life row even though files remain.
    const GcState state = decodeGcState(backend->get(layout.gcStateKey())->bytes);
    ASSERT_GT(state.snap_generation, 0u);
    const CasFoldSeal seal = decodeFoldSeal(
        backend->get(layout.foldSealKey(state.snap_generation, state.snap_attempt))->bytes);
    const auto row_it = seal.ref_lives.find(life.incarnation);
    ASSERT_NE(row_it, seal.ref_lives.end());
    ASSERT_TRUE(row_it->second.cleanup_evidence.has_value());
    EXPECT_EQ(row_it->second.cleanup_evidence->remove_txn_id, (RefTxnId{1, 1}));
    EXPECT_TRUE(backend->head(debris_key).exists) << "cleanup evidence does not gate on physical deletion";
}

/// An old-format pool carrying unqualified `roots/<ns>/_files/x` keys is REFUSED AT OPEN. It is not
/// read, not migrated, and not silently re-keyed: the file layer rides Task 4's format bump B, and the
/// pool-open floor is what makes "there is nothing to migrate" true rather than merely intended.
///
/// Asserted at OPEN rather than at the parser on purpose: `Layout` has no unqualified key constructor
/// at all (a compile-time concept check in `gtest_cas_namespace_life_id.cpp` pins that, and
/// `parseNamespaceFileKey`'s refusal of a legacy key is pinned there too), so the only reachable
/// question left is whether a pool that CONTAINS such keys can be opened. It cannot.
TEST(CasNsFileIncarnation, LegacyUnqualifiedFileKeyIsRefusedAtOpen)
{
    auto backend = std::make_shared<InMemoryBackend>();
    const Layout layout("p");

    /// A generation-5 `_pool_meta`: the current encoder's output with its header generation moved back
    /// one, so every other byte is exactly what that generation really wrote.
    PoolMeta meta;
    meta.pool_id = hexToU128("0123456789abcdef0123456789abcdef");
    meta.blob_header_len = 256;
    meta.min_reader_generation = kNamespaceLifeKeyedGeneration - 1;
    meta.algos_used = {static_cast<uint8_t>(BlobHashAlgo::CityHash128)};
    String encoded = encodePoolMeta(meta);
    const String current_v = "\"v\":" + std::to_string(G_BUILD);
    const String legacy_v = "\"v\":" + std::to_string(kNamespaceLifeKeyedGeneration);
    const size_t at = encoded.find(current_v);
    /// Guard the substitution itself: a silent no-op here would leave a CURRENT-generation pool and the
    /// test would pass by opening a pool it believes it downgraded.
    ASSERT_NE(at, String::npos) << "pool-meta header no longer spells its generation as " << current_v;
    encoded.replace(at, current_v.size(), legacy_v);
    ASSERT_NE(encoded.find(legacy_v), String::npos);
    backend->putIfAbsent(layout.poolMetaKey(), encoded);

    /// The legacy artifact this task removes: a namespace file keyed by NAME ONLY, with no incarnation
    /// segment. Written as raw bytes because no code path in the tree can produce this key any more.
    backend->putIfAbsent("p/roots/" + kNsString + "/_files/" + kFile, "1\n");

    try
    {
        openPoolForTest(backend);
        FAIL() << "an old-format pool must fail closed at open, naming recreation";
    }
    catch (const DB::Exception & e)
    {
        EXPECT_EQ(e.code(), DB::ErrorCodes::UNKNOWN_FORMAT_VERSION);
        EXPECT_NE(e.message().find("recreate"), String::npos)
            << "the refusal must tell the operator what to do; got: " << e.message();
    }
}
