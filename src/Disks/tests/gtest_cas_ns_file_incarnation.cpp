#include <gtest/gtest.h>

#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Formats/CasFormat.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Formats/CasPoolMetaFormat.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Gc/CasGc.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasPool.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasRefCatalog.h>
#include "cas_test_helpers.h"

namespace DB::ErrorCodes
{
    extern const int UNKNOWN_FORMAT_VERSION;
}

/// Namespace files are keyed by the namespace's LIFE, not by its name: `roots/<ns>/<inc>/_files/<name>`
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
/// WHAT REBIRTH NO LONGER WAITS FOR. The `Pending -> Completed` gate used to LIST-probe the files
/// prefix, i.e. it required a PHYSICAL-EMPTY PROOF for files before a name could be reused. That probe
/// is deleted, not weakened: `PhysicalEmptyProofIgnoresFiles` pins the predicate answering "empty" with
/// files still on the store, and `RebirthDoesNotWaitForFilesToBeEmpty` pins the stronger operational
/// form -- creation does not merely tolerate the debris, it never looks at it.

using namespace DB::Cas;
using namespace DB::Cas::tests;

namespace
{

const String kNsString = "00/aa@cas@";
const String kFile = "format_version.txt";

/// Two DELIBERATELY DISTINGUISHABLE incarnations. Hand-picked rather than random so a failure message
/// names which life a key belongs to, and so neither can coincide with `stageATransition`'s sentinel
/// (whose render spells `__STAGE_A_TRANS`) -- a test that accidentally ran at the sentinel would agree
/// with the pre-re-key tree and prove nothing.
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

/// Delete `ns`'s catalog entry, which is what makes the name available again.
///
/// THE SEAM, STATED: this models the entry-delete half of the removal lifecycle that Task 5 owns. It
/// goes through the PUBLIC `casUpdate` primitive -- the same token-CAS loop every production transition
/// rides -- rather than rewriting the catalog object's bytes, so the catalog's grammar and ordering
/// checks run exactly as they will for the production caller. What is NOT yet drivable end-to-end is
/// `dropNamespace` reaching this point by itself: today it leaves the entry in place, so a same-name
/// rebirth reuses the SAME incarnation and no second life exists to test. That arrives with Task 5;
/// until it does, this function is the only way to construct the two-lives shape at all.
void retireCatalogEntry(Backend & backend, const Layout & layout, const RootNamespace & ns)
{
    CasRefCatalog::casUpdate(backend, layout, [&](const RefCatalog & current)
    {
        RefCatalog next;
        for (const CatalogEntry & entry : current.entries)
            if (entry.ns.string() != ns.string())
                next.entries.push_back(entry);
        return next;
    });
}

/// Every key the backend was asked about that sits under ANY life's files prefix. The assertion this
/// serves is "the operation never looked", so it spans every recorded operation family (head, get,
/// put, overwrite, CAS, delete, list) rather than LIST alone: a HEAD of a file key would be just as
/// much a physical-emptiness probe as a LIST of the prefix, and reporting the offending keys by name
/// is what makes a failure diagnosable.
std::vector<String> filesKeysTouched(const CountingBackend & backend)
{
    std::vector<String> touched;
    for (const String & key : backend.touchedKeys())
        if (key.find("/_files/") != String::npos)
            touched.push_back(key);
    return touched;
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

/// Rebirth does not wait for the previous life's files to be physically gone. Driven through the thing
/// that actually made it wait -- the `Pending -> Completed` promotion in a real GC round, which is the
/// precondition a name's reuse is gated on -- with `_files` debris present throughout.
///
/// WHY THIS DRIVER AND NOT A BARE RE-RESOLUTION: a test that only resolved a fresh life with debris
/// present would pass on the PRE-re-key tree too, because the resolution path never probed files; only
/// the completion gate did. Asserting the property at the resolution would therefore assert nothing
/// about this change. The gate is where the wait lived, so the gate is where its absence must be shown.
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

    /// The item must be COMPLETED -- the name is reusable -- even though files are still on the store.
    const GcState state = decodeGcState(backend->get(layout.gcStateKey())->bytes);
    ASSERT_GT(state.snap_generation, 0u);
    const CasFoldSeal seal = decodeFoldSeal(
        backend->get(layout.foldSealKey(state.snap_generation, state.snap_attempt))->bytes);
    ASSERT_EQ(seal.ns_cleanup_items.size(), 1u) << "the removal must have produced exactly one item";
    EXPECT_EQ(seal.ns_cleanup_items.begin()->second.state, RefNsCleanupState::Completed)
        << "a namespace's cleanup must complete with files still present -- rebirth waits for no file";
}

/// White-box on the `Pending -> Completed` emptiness predicate itself: with files under the namespace
/// and no manifests it answers EMPTY, and it does not so much as LIST the files prefix to decide. The
/// files arm of this probe WAS the physical-empty proof the directive forbids rebirth from waiting on,
/// so it is deleted rather than weakened; the manifest arm, unchanged by this task, still answers
/// truthfully.
TEST(CasNsFileIncarnation, PhysicalEmptyProofIgnoresFiles)
{
    auto backend = std::make_shared<CountingBackend>();
    PoolPtr store = openPoolForTest(backend);
    const Layout & layout = store->layout();
    const RootNamespace ns{kNsString};

    admitLifeAt(*backend, layout, ns, kInc1);
    const NamespaceLifeId life = NamespaceLifeId::fromCatalogEntry(ns, kInc1);
    store->putNamespaceFile(life, kFile, "1\n");
    ASSERT_FALSE(store->listNamespaceFiles(life).empty()) << "precondition: the debris is really there";

    Gc gc(store, kGcId);
    backend->resetCounts();
    EXPECT_TRUE(gc.namespaceManifestsPhysicallyEmptyForTest(ns))
        << "files must not hold the completion gate open";

    /// Not merely "answers empty" but "never looked": the deleted arm was a LIST, and a HEAD of a file
    /// key would be the same proof by another request, so the whole `_files` key space is checked.
    EXPECT_EQ(filesKeysTouched(*backend), std::vector<String>{})
        << "the predicate must not probe ANY files key";
    EXPECT_EQ(backend->listCount(layout.namespaceFilesPrefix(life)), 0u);

    /// The positive control, and the reason this is not a test that the predicate always says yes: a
    /// manifest body under the same namespace still holds it closed.
    writeManifestRaw(*backend, layout, ns,
        ManifestRef{.writer_epoch = 1, .build_sequence = 1, .manifest_ordinal = 1},
        {blobEntryFor("a", DB::UInt128(1))});
    EXPECT_FALSE(gc.namespaceManifestsPhysicallyEmptyForTest(ns))
        << "the manifest arm is unchanged by this task and must still answer truthfully";
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

    /// A generation-4 `_pool_meta`: the current encoder's output with its header generation moved back
    /// one, so every other byte is exactly what that generation really wrote.
    PoolMeta meta;
    meta.pool_id = hexToU128("0123456789abcdef0123456789abcdef");
    meta.blob_header_len = 256;
    meta.min_reader_generation = kNamespaceLifeKeyedGeneration - 1;
    meta.algos_used = {static_cast<uint8_t>(BlobHashAlgo::CityHash128)};
    String encoded = encodePoolMeta(meta);
    const String current_v = "\"v\":" + std::to_string(kNamespaceLifeKeyedGeneration);
    const String legacy_v = "\"v\":" + std::to_string(kNamespaceLifeKeyedGeneration - 1);
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
