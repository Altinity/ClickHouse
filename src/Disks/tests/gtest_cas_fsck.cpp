#include <gtest/gtest.h>

#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Tools/CasFsck.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Gc/CasGc.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Backend/CasInMemoryBackend.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Formats/CasFoldSealFormat.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Formats/CasGcStateFormat.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Formats/CasRefSnapshotFormat.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasRefProtocol.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasPool.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/ContentAddressedMetadataStorage.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/ContentAddressedTransaction.h>
#include "cas_test_helpers.h"
#include "config.h"
#if USE_AWS_S3
#include <IO/S3Common.h>
#endif

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <functional>
#include <map>
#include <mutex>
#include <set>
#include <optional>
#include <string_view>
#include <thread>

namespace DB::ErrorCodes
{
extern const int BAD_ARGUMENTS;
extern const int INVALID_STATE;
extern const int NETWORK_ERROR;
extern const int NOT_IMPLEMENTED;
extern const int TIMEOUT_EXCEEDED;
}

using namespace DB::Cas;
using namespace DB::Cas::tests;

namespace
{
constexpr uint64_t kWriterEpoch = 7;
const String kServerRoot = "00";
ManifestRef ref(uint64_t seq, uint64_t inst)
{
    return ManifestRef{.writer_epoch = kWriterEpoch, .build_sequence = seq, .manifest_ordinal = static_cast<uint32_t>(inst)};
}

/// B207 race-simulation harness: `InMemoryBackend` is documented "not final: tests subclass it to
/// distort single behaviors". `runFsck`'s ref-walk and its physical blob listing (`listAll` over
/// `layout.blobsPrefix()`) are two separate calls to `Backend::list` minutes apart in production; here
/// we fire an injected mutation the FIRST time `list` is called against the armed prefix — i.e.
/// strictly AFTER the ref-walk has captured its (now stale) `reachable_blobs`/`blob_labels` view, and
/// strictly BEFORE the HEAD-confirm loop sees the physical listing. That reproduces the race
/// deterministically, without any real timing.
class RepublishOnListBackend : public InMemoryBackend
{
public:
    void armOnFirstList(String prefix, std::function<void()> mutation)
    {
        std::lock_guard<std::mutex> lock(arm_mutex);
        armed_prefix = std::move(prefix);
        pending_mutation = std::move(mutation);
    }

    RawListPage list(const String & prefix, const String & cursor, size_t limit, TransportAccess & access) override
    {
        std::function<void()> to_run;
        {
            std::lock_guard<std::mutex> lock(arm_mutex);
            if (pending_mutation && prefix == armed_prefix)
            {
                to_run = std::move(pending_mutation);
                pending_mutation = nullptr;
            }
        }
        if (to_run)
            to_run();
        return InMemoryBackend::list(prefix, cursor, limit, access);
    }

private:
    std::mutex arm_mutex;
    String armed_prefix;
    std::function<void()> pending_mutation;
};

/// Companion to `RepublishOnListBackend` for the MANIFEST phantom-dangle race: the ref-walk's
/// per-namespace recovery captures each committed `(ref -> manifest)` minutes before the per-ref
/// `backend.get(mkey)` that confirms the manifest body. This backend fires an injected mutation the
/// FIRST time `get` is called for the armed manifest key — strictly AFTER the walk captured its (now
/// stale) row and AT the GET that would otherwise read the manifest — reproducing "ref republished/
/// dropped + old manifest legitimately GC-deleted" deterministically, with no real timing.
class MutateOnFirstGetBackend : public InMemoryBackend
{
public:
    void armOnFirstGet(String key, std::function<void()> mutation)
    {
        std::lock_guard<std::mutex> lock(arm_mutex);
        armed_key = std::move(key);
        pending_mutation = std::move(mutation);
    }

    std::optional<Raw> read(const String & key, TransportAccess & access) override
    {
        std::function<void()> to_run;
        {
            std::lock_guard<std::mutex> lock(arm_mutex);
            if (pending_mutation && key == armed_key)
            {
                to_run = std::move(pending_mutation);
                pending_mutation = nullptr;
            }
        }
        if (to_run)
            to_run();
        return InMemoryBackend::read(key, access);
    }

private:
    std::mutex arm_mutex;
    String armed_key;
    std::function<void()> pending_mutation;
};

enum class FsckListingMode : uint8_t
{
    Full,
    Empty,
    Partial,
    Reordered,
};

/// Distort only one namespace stream LIST after fixture deposition. Exact GET/HEAD and every other
/// prefix retain ordinary backend semantics, so the test varies the hint and nothing authoritative.
class FsckListingBackend : public InMemoryBackend
{
public:
    void distort(String prefix_, FsckListingMode mode_)
    {
        prefix = std::move(prefix_);
        mode = mode_;
    }

    RawListPage list(const String & listed_prefix, const String & cursor, size_t limit, TransportAccess & access) override
    {
        RawListPage page = InMemoryBackend::list(listed_prefix, cursor, limit, access);
        if (listed_prefix != prefix)
            return page;
        if (mode == FsckListingMode::Empty)
            page.keys.clear();
        else if (mode == FsckListingMode::Partial && !page.keys.empty())
            page.keys.erase(page.keys.begin());
        else if (mode == FsckListingMode::Reordered)
            std::reverse(page.keys.begin(), page.keys.end());
        return page;
    }

private:
    String prefix;
    FsckListingMode mode = FsckListingMode::Full;
};

#if USE_AWS_S3
/// Fail one exact GET without disturbing LIST or any other object read. This keeps the checkpoint
/// authority stable while proving that fsck distinguishes a transport failure from durable corruption.
/// An access denial is the class the request engine surfaces on the first attempt instead of reissuing
/// (`InMemoryBackend::refreshCredentials` answers false by default), so the failure needs no retry
/// budget and the caller sees the injected message unchanged.
class FailExactGetBackend : public InMemoryBackend
{
public:
    void fail(String key_)
    {
        key = std::move(key_);
    }

    std::optional<Raw> read(const String & requested_key, TransportAccess & access) override
    {
        if (requested_key == key)
            throw DB::S3Exception("injected access denial on exact GET", Aws::S3::S3Errors::ACCESS_DENIED);
        return InMemoryBackend::read(requested_key, access);
    }

private:
    String key;
};
#endif

/// Publish the exact `_ckpt` authority an ordinary Live test life would have after its first committed
/// record. Raw ref-log helpers deliberately do not do this: several protocol tests need malformed or
/// pre-creation states. Fsck tests that exercise a recoverable Live life must make the durable authority
/// explicit instead of accidentally borrowing the legacy LIST-only recovery rule.
void writeFsckCheckpoint(Backend & backend, const Layout & layout, const RootNamespace & ns, RefTxnId committed_through)
{
    CasRequests requests = DB::Cas::tests::openRequestsForTest(backend);
    CasOperation op = requests.admit();
    const CasRefCatalog::Snapshot cut = CasRefCatalog::read(op, layout);
    const auto it = std::find_if(cut.catalog.entries.begin(), cut.catalog.entries.end(),
        [&](const CatalogEntry & entry) { return entry.ns == ns; });
    ASSERT_NE(it, cut.catalog.entries.end());
    const NamespaceLifeId life = NamespaceLifeId::fromCatalogEntry(it->ns, it->incarnation);
    const String key = layout.refCkptKey(life);
    const String body = encodeRefCkptAtGeneration(RefCkpt{
        .life_epoch = committed_through.writer_epoch,
        .committed_through = committed_through,
        .checkpoint_snapshot_id = std::nullopt,
        .last_epoch_seal = std::nullopt}, 1);
    const auto current = op.head(key, Retry::once());
    const WriteResult put = current
        ? op.replace(key, body, current->etag, Retry::once())
        : op.create(key, body, Retry::once());
    ASSERT_TRUE(std::holds_alternative<Committed>(put));
}

void writeFsckCheckpointWithBase(
    Backend & backend, const Layout & layout, const RootNamespace & ns, RefTxnId base,
    std::optional<RefTxnId> last_epoch_seal = std::nullopt,
    std::optional<RefTxnId> committed_through = std::nullopt)
{
    CasRequests requests = DB::Cas::tests::openRequestsForTest(backend);
    CasOperation op = requests.admit();
    const NamespaceLifeId life = *CasRefCatalog::lifeIfCataloged(op, layout, ns);
    ASSERT_TRUE(std::holds_alternative<Committed>(op.create(layout.refCkptKey(life), encodeRefCkptAtGeneration(RefCkpt{
        .life_epoch = 1,
        .committed_through = committed_through.value_or(base),
        .checkpoint_snapshot_id = base,
        .last_epoch_seal = last_epoch_seal}, 1), Retry::once())));
}

void expectCheckpointBaseVerdict(
    const FsckReport & report, const String & exact_base_key, FsckClass expected_class,
    std::string_view expected_reason)
{
    EXPECT_EQ(report.chain_broken, expected_class == FsckClass::ChainBroken ? 1u : 0u);
    EXPECT_EQ(report.unchecked, expected_class == FsckClass::Unchecked ? 1u : 0u);
    EXPECT_EQ(report.clean(), expected_class != FsckClass::ChainBroken);

    size_t matching = 0;
    for (const FsckObject & object : report.objects)
    {
        if (object.cls != expected_class || object.key != exact_base_key)
            continue;
        ++matching;
        ASSERT_EQ(object.reachable_from.size(), 1u);
        EXPECT_NE(object.reachable_from.front().find(expected_reason), String::npos);
    }
    EXPECT_EQ(matching, 1u) << "the checkpoint-base verdict must identify its exact named base and cause";
}

/// Test-only external catalog writer. It changes the namespace's current logical life after fsck took
/// its catalog cut, precisely the competing-cut mutation that fsck must not splice into its verdict.
void replaceCatalogLife(Backend & backend, const Layout & layout, const RootNamespace & ns, UInt128 incarnation)
{
    CasRequests requests = DB::Cas::tests::openRequestsForTest(backend);
    CasOperation op = requests.admit();
    CasRefCatalog::Snapshot current = CasRefCatalog::read(op, layout);
    const auto it = std::find_if(current.catalog.entries.begin(), current.catalog.entries.end(),
        [&](const CatalogEntry & entry) { return entry.ns == ns; });
    ASSERT_NE(it, current.catalog.entries.end());
    it->incarnation = incarnation;
    it->state = NsState::Live;
    it->creator.reset();
    it->removal_started_round.reset();
    ASSERT_TRUE(current.etag.has_value());
    ASSERT_TRUE(std::holds_alternative<Committed>(
        op.replace(layout.refCatalogKey(), encodeRefCatalog(current.catalog), *current.etag, Retry::standard())));
}

FsckReport runFsckWithListingMode(FsckListingMode mode, std::string_view suffix)
{
    auto backend = std::make_shared<FsckListingBackend>();
    auto store = openPoolForTest(backend);
    const Layout & layout = store->layout();
    const RootNamespace ns{"00/listing_" + String(suffix) + "@cas@"};
    const ManifestRef r1 = ref(1, 0xD1);
    const ManifestRef r2 = ref(2, 0xD2);
    const DB::UInt128 h1 = u128Of("fsck-listing-old-" + String(suffix));
    const DB::UInt128 h2 = u128Of("fsck-listing-new-" + String(suffix));
    writeBlobBody(*backend, layout, h1);
    writeBlobBody(*backend, layout, h2);
    writeManifestRaw(*backend, layout, ns, r1, {blobEntryFor("a", h1)});
    writeManifestRaw(*backend, layout, ns, r2, {blobEntryFor("a", h2)});
    publishCommittedTransition(*backend, layout, ns, "tbl", std::nullopt, r1);
    const uint64_t frontier = publishCommittedTransition(*backend, layout, ns, "tbl", r1, r2);
    writeFsckCheckpoint(*backend, layout, ns, RefTxnId{1, frontier});

    CasRequests requests = DB::Cas::tests::openRequestsForTest(backend);
    CasOperation op = requests.admit();
    const NamespaceLifeId life = *CasRefCatalog::lifeIfCataloged(op, layout, ns);
    backend->distort(layout.namespaceStreamPrefix(life), mode);
    return runFsck(*store, /*detail=*/true);
}

void expectListingIndependentFsck(const FsckReport & report)
{
    EXPECT_EQ(report.chain_broken, 0u);
    EXPECT_EQ(report.unchecked, 0u);
    EXPECT_EQ(report.dangling, 0u);
    EXPECT_EQ(report.ref_records_walked, 2u);
    EXPECT_EQ(report.reachable, 1u);
}

struct FsckAuthorityVerdict
{
    bool clean = false;
    uint64_t hard_findings = 0;
    uint64_t chain_broken = 0;
    uint64_t unchecked = 0;
    uint64_t ref_records_walked = 0;
    uint64_t reachable = 0;
    uint64_t dangling = 0;

    bool operator==(const FsckAuthorityVerdict &) const = default;
};

FsckAuthorityVerdict authorityVerdict(const FsckReport & report)
{
    uint64_t hard_findings = 0;
    for (const FsckHardFinding & finding : kFsckHardFindings)
        hard_findings += report.*(finding.value);
    return FsckAuthorityVerdict{
        .clean = report.clean(),
        .hard_findings = hard_findings,
        .chain_broken = report.chain_broken,
        .unchecked = report.unchecked,
        .ref_records_walked = report.ref_records_walked,
        .reachable = report.reachable,
        .dangling = report.dangling,
    };
}

FsckReport runCheckpointBaseFsckWithListingMode(
    FsckListingMode mode, std::string_view suffix, bool corrupt_exact_base)
{
    auto backend = std::make_shared<FsckListingBackend>();
    auto store = openPoolForTest(backend);
    const Layout & layout = store->layout();
    const RootNamespace ns{"00/listing_base_" + String(suffix) + "@cas@"};
    fixture::admitLive(*backend, layout, ns);

    const RefTxnId base{1, 1};
    const RefLogTxn birth{
        .ns = ns.string(), .txn_id = base, .ops = {namespaceBirthOp()}, .prev_epoch_seal = std::nullopt};
    fixture::writeRefLogRaw(*backend, layout, birth);
    RefTableState base_state;
    applyRefLogTxn(base_state, birth);
    writeRefSnapshotRaw(*backend, layout, snapshotOf(base_state, ns.string()));
    writeFsckCheckpointWithBase(*backend, layout, ns, base);

    CasRequests requests = DB::Cas::tests::openRequestsForTest(backend);
    CasOperation op = requests.admit();
    const NamespaceLifeId life = *CasRefCatalog::lifeIfCataloged(op, layout, ns);
    if (corrupt_exact_base)
    {
        const String base_snapshot_key = layout.refSnapshotKey(life, base);
        const auto head = op.head(base_snapshot_key, Retry::once());
        EXPECT_TRUE(head.has_value());
        if (head)
            EXPECT_EQ(op.remove(base_snapshot_key, head->etag, Retry::once()), Removal::Removed);
    }
    else
    {
        /// This newer pair is deliberately outside `_ckpt.committed_through`. It is inert garbage:
        /// changing whether LIST happens to reveal it must not add or remove an fsck finding.
        const RefTxnId unadopted{1, 2};
        const RefOwnerBinding listed_binding{RefOwnerKind::Precommit, "listed", ref(9, 0xE9)};
        const RefLogTxn listed_log{
            .ns = ns.string(),
            .txn_id = unadopted,
            .ops = {ownerTransitionOp(std::nullopt, listed_binding)},
            .prev_epoch_seal = std::nullopt};
        fixture::writeRefLogRaw(*backend, layout, listed_log);
        RefTableState listed_state = base_state;
        applyRefLogTxn(listed_state, listed_log);
        RefTableSnapshot unadopted_snapshot = snapshotOf(listed_state, ns.string());
        unadopted_snapshot.precommits.push_back(
            RefOwnerBinding{RefOwnerKind::Precommit, "unlisted", ref(10, 0xEA)});
        writeRefSnapshotRaw(*backend, layout, unadopted_snapshot);
    }

    backend->distort(layout.namespaceStreamPrefix(life), mode);
    return runFsck(*store, /*detail=*/true);
}
}

/// A committed ref whose manifest body is present and whose blobs exist => clean.
TEST(CASFsck, CleanManifestPoolHasNoDangling)
{
    auto backend = std::make_shared<InMemoryBackend>();
    auto store = openPoolForTest(backend);
    const RootNamespace ns{"00/aa@cas@"};
    const ManifestRef r = ref(1, 0xAA);
    writeBlobBody(*backend, store->layout(), DB::UInt128(1));
    writeManifestRaw(*backend, store->layout(), ns, r, {blobEntryFor("a", DB::UInt128(1))});
    const uint64_t sequence = publishCommittedTransition(*backend, store->layout(), ns, "tbl", std::nullopt, r);
    writeFsckCheckpoint(*backend, store->layout(), ns, RefTxnId{1, sequence});
    const FsckReport rep = runFsck(*store, /*detail*/true);
    EXPECT_TRUE(rep.clean());
    EXPECT_EQ(rep.dangling, 0u);
}

/// A committed ref naming a MISSING manifest body is an ERROR (Dangling).
TEST(CASFsck, OwnerVisibleMissingManifestBodyIsError)
{
    auto backend = std::make_shared<InMemoryBackend>();
    auto store = openPoolForTest(backend);
    const RootNamespace ns{"00/aa@cas@"};
    const ManifestRef r = ref(1, 0xAA);
    const uint64_t sequence = publishCommittedTransition(
        *backend, store->layout(), ns, "tbl", std::nullopt, r);  // no body written
    writeFsckCheckpoint(*backend, store->layout(), ns, RefTxnId{1, sequence});
    const FsckReport rep = runFsck(*store, /*detail*/true);
    EXPECT_FALSE(rep.clean());
    EXPECT_GE(rep.dangling, 1u);
}

/// A committed ref whose blob body is missing is an ERROR (Dangling).
TEST(CASFsck, ReachableBlobMissingIsError)
{
    auto backend = std::make_shared<InMemoryBackend>();
    auto store = openPoolForTest(backend);
    const RootNamespace ns{"00/aa@cas@"};
    const ManifestRef r = ref(1, 0xAA);
    writeManifestRaw(*backend, store->layout(), ns, r, {blobEntryFor("a", DB::UInt128(1))});  // no blob body
    const uint64_t sequence = publishCommittedTransition(*backend, store->layout(), ns, "tbl", std::nullopt, r);
    writeFsckCheckpoint(*backend, store->layout(), ns, RefTxnId{1, sequence});
    const FsckReport rep = runFsck(*store, /*detail*/true);
    EXPECT_FALSE(rep.clean());
    EXPECT_GE(rep.dangling, 1u);
}

/// fsck RECORDS AND CONTINUES over a key that names no namespace life. It is the forensic tool an
/// operator reaches for after something has already gone wrong, so one bad key must not make it report
/// NOTHING -- including about the healthy namespaces it would never reach. The finding is hard (an
/// un-incarnated key is corruption behind the format bump) and counted once per key, not once per sweep.
TEST(CASFsck, LifelessKeyIsRecordedAndTheHealthyNamespaceIsStillReported)
{
    auto backend = std::make_shared<InMemoryBackend>();
    auto store = openPoolForTest(backend);
    const RootNamespace ns{"00/aa@cas@"};
    const ManifestRef r = ref(1, 0xA1);
    writeBlobBody(*backend, store->layout(), DB::UInt128(1));
    writeManifestRaw(*backend, store->layout(), ns, r, {blobEntryFor("a", DB::UInt128(1))});
    const uint64_t sequence = publishCommittedTransition(*backend, store->layout(), ns, "tbl", std::nullopt, r);
    writeFsckCheckpoint(*backend, store->layout(), ns, RefTxnId{1, sequence});

    /// Hand-built: no helper can mint the un-incarnated shape any more.
    const String lifeless = store->layout().casRefsPrefix() + ns.string() + "/_log/"
        + renderRefTxnId(RefTxnId{1, 1}) + ".zst";
    {
        OperationForTest op(*backend);
        ASSERT_TRUE(std::holds_alternative<Committed>((*op).create(lifeless, "garbage", Retry::once())));
    }

    FsckReport rep;
    ASSERT_NO_THROW(rep = runFsck(*store, /*detail*/true))
        << "the audit must not be taken out by the damage it exists to report";

    /// The finding, named, and counted ONCE even though several sweeps enumerate namespaces.
    EXPECT_EQ(rep.lifeless_keys, 1u);
    EXPECT_FALSE(rep.clean());
    bool saw = false;
    for (const FsckObject & o : rep.objects)
        if (o.cls == FsckClass::LifelessKey)
        {
            saw = true;
            EXPECT_EQ(o.key, lifeless);
        }
    EXPECT_TRUE(saw) << "a counted finding with no row is a number nobody can act on";

    /// And the healthy namespace was still reached: its committed ref resolved to a present manifest and
    /// a present blob, which only a sweep that ran can report.
    EXPECT_GE(rep.reachable, 1u);
    EXPECT_EQ(rep.dangling, 0u);
}

/// A COMPLETE, canonical namespace-life key (a real `_files` write under a real admitted life) whose
/// catalog row is then removed entirely -- exactly what a fenced GC's exact-CAS row deletion leaves
/// behind, before the perpetual namespace janitor's next page reaches it -- must classify as
/// `janitor_pending`, a SOFT finding, never `lifeless_keys`. The report stays clean.
TEST(CASFsck, CanonicalDeadLifeResidueIsJanitorPendingNotHardFinding)
{
    auto backend = std::make_shared<InMemoryBackend>();
    auto store = openPoolForTest(backend);
    const RootNamespace ns{"00/aa@cas@"};
    const NamespaceLifeId life = store->namespaceLife(ns);
    store->putNamespaceFile(life, "format_version.txt", "1\n");

    /// Simulate a fenced GC's exact-CAS catalog-row deletion: the row is gone, the life-owned physical
    /// object above survives it (the janitor's own job, not GC's own round). `casUpdate` deliberately
    /// refuses to add or delete rows (there is no generic catalog remove-by-name API -- deletion is
    /// only `deleteCompletedRemovingAtSnapshot`/`cancelStalledCreating`, both requiring the full fenced-GC
    /// protocol this fixture is not driving), so inject the post-deletion catalog snapshot directly,
    /// mirroring `DuplicateLifeIdIsReportedWhileAnUnrelatedUniqueNamespaceStillProgresses` below.
    {
        CasOperation op = store->openRequests().admit();
        CasRefCatalog::Snapshot snapshot = CasRefCatalog::read(op, store->layout());
        const auto it = std::find_if(snapshot.catalog.entries.begin(), snapshot.catalog.entries.end(),
            [&](const CatalogEntry & entry) { return entry.ns == ns; });
        ASSERT_NE(it, snapshot.catalog.entries.end());
        snapshot.catalog.entries.erase(it);
        const auto catalog_head = op.head(store->layout().refCatalogKey(), Retry::once());
        ASSERT_TRUE(catalog_head.has_value());
        ASSERT_TRUE(std::holds_alternative<Committed>(op.replace(store->layout().refCatalogKey(), encodeRefCatalog(snapshot.catalog),
            catalog_head->etag, Retry::once())));
    }

    FsckReport rep;
    ASSERT_NO_THROW(rep = runFsck(*store, /*detail*/true))
        << "janitor-pending residue must never abort the scan";

    EXPECT_EQ(rep.lifeless_keys, 0u);
    EXPECT_GE(rep.namespace_janitor_pending, 1u);
    EXPECT_EQ(rep.namespace_janitor_pending_lives, 1u);
    EXPECT_TRUE(rep.clean()) << "janitor-pending residue is not a hard finding";
    bool saw = false;
    for (const FsckObject & o : rep.objects)
        if (o.cls == FsckClass::JanitorPending)
            saw = true;
    EXPECT_TRUE(saw) << "a counted soft finding with no row is a number nobody can act on";
}

/// The observe-then-cut race: a life admitted between fsck's namespace-tree LIST and the catalog cut
/// it takes AFTER that listing must NOT be misread as residue. Mirrors
/// `CASNamespaceJanitor.PostListCatalogCutProtectsConcurrentCreationWithOneGet` -- the same ordering,
/// the same reason: creation admits `Creating` before writing any life-owned object, so a life visible
/// only in the LATER cut cannot have raced this listing.
namespace
{
class AdmitLifeAfterNamespaceListingBackend : public InMemoryBackend
{
public:
    explicit AdmitLifeAfterNamespaceListingBackend(NamespaceLifeId life_) : protected_life(std::move(life_)) {}

    RawListPage list(const String & prefix, const String & cursor, size_t limit, TransportAccess & access) override
    {
        RawListPage page = InMemoryBackend::list(prefix, cursor, limit, access);
        if (!published && prefix.ends_with("/cas/ns/"))
        {
            published = true;
            /// The base call above has already released the backend's lock, so admitting through this
            /// same backend from here cannot deadlock.
            CasRequests requests = DB::Cas::tests::openRequestsForTest(*this);
            CasOperation op = requests.admit();
            CasRefCatalog::casAdmitEntry(op, Layout("p"), /*gc_shards*/1,
                CatalogEntry{.ns = protected_life.ns, .state = NsState::Live,
                    .incarnation = protected_life.incarnation});
        }
        return page;
    }

private:
    NamespaceLifeId protected_life;
    bool published = false;
};
}

TEST(CASFsck, LifeAdmittedBetweenNamespaceListingAndLaterCutIsNotResidue)
{
    const RootNamespace ns{"00/late@cas@"};
    const NamespaceLifeId life = NamespaceLifeId::fromCatalogEntry(ns, UInt128{909});
    auto backend = std::make_shared<AdmitLifeAfterNamespaceListingBackend>(life);
    auto store = openPoolForTest(backend);
    /// The physical object exists before the listing runs, exactly as a legitimate late admission would
    /// leave it: written only after `casAdmitEntry` above, but here pre-seeded since the injected
    /// backend admits the CATALOG row, not the physical file, on the list callback.
    {
        OperationForTest op(*backend);
        ASSERT_TRUE(std::holds_alternative<Committed>(
            (*op).create(store->layout().namespaceFilesPrefix(life) + "format_version.txt", "1\n", Retry::once())));
    }

    FsckReport rep;
    ASSERT_NO_THROW(rep = runFsck(*store, /*detail*/true));
    EXPECT_EQ(rep.namespace_janitor_pending, 0u)
        << "a life visible in the post-listing cut must not be misclassified as residue";
    EXPECT_EQ(rep.lifeless_keys, 0u);
    EXPECT_TRUE(rep.clean());
}

/// Malformed or non-canonical namespace-tree shapes must stay HARD findings even after the
/// janitor-pending split: a dirty `_files` relative name (the parser-asymmetry fix), a zero life id, an
/// uppercase life id, and an unrecognized kind directory all name no current writer's grammar.
TEST(CASFsck, MalformedNamespaceTreeShapesStayHardFindings)
{
    const Layout layout("p");
    const NamespaceLifeId life = NamespaceLifeId::fromCatalogEntry(RootNamespace{"00/bb@cas@"}, UInt128{909});
    const struct { String key; String description; } cases[] = {
        {layout.namespaceFilesPrefix(life) + "../escape", "dirty _files relative name"},
        {"p/cas/ns/state/" + String(32, '0') + "/_files/format_version.txt", "zero life id"},
        {"p/cas/ns/state/112233445566778899AABBCCDDEEFF01/_files/format_version.txt", "uppercase life id"},
        {"p/cas/ns/stream/" + renderIncarnation(UInt128{909}) + "/_unknown_kind/x.zst", "unknown kind directory"},
    };
    for (const auto & c : cases)
    {
        auto backend = std::make_shared<InMemoryBackend>();
        auto store = openPoolForTest(backend);
        {
            OperationForTest op(*backend);
            ASSERT_TRUE(std::holds_alternative<Committed>((*op).create(c.key, "garbage", Retry::once()))) << c.description;
        }

        FsckReport rep;
        ASSERT_NO_THROW(rep = runFsck(*store, /*detail*/true)) << c.description;
        EXPECT_GE(rep.lifeless_keys, 1u) << c.description;
        EXPECT_EQ(rep.namespace_janitor_pending, 0u) << c.description;
        EXPECT_FALSE(rep.clean()) << c.description;
    }
}

/// Mutation caught: calling the destructive consumer's global `throwIfAmbiguous` from fsck aborts
/// before the unique row is audited. The read-only tool reports the ambiguous physical id and
/// continues through an unrelated unique namespace.
TEST(CASFsck, DuplicateLifeIdIsReportedWhileAnUnrelatedUniqueNamespaceStillProgresses)
{
    auto backend = std::make_shared<InMemoryBackend>();
    auto store = openPoolForTest(backend);
    const Layout & layout = store->layout();
    const RootNamespace unique_ns{"00/unique@cas@"};
    const ManifestRef r = ref(1, 0xA1);
    writeBlobBody(*backend, layout, DB::UInt128(1));
    writeManifestRaw(*backend, layout, unique_ns, r, {blobEntryFor("a", DB::UInt128(1))});
    const uint64_t sequence = publishCommittedTransition(*backend, layout, unique_ns, "tbl", std::nullopt, r);
    writeFsckCheckpoint(*backend, layout, unique_ns, RefTxnId{1, sequence});

    CasRequests requests = DB::Cas::tests::openRequestsForTest(backend);
    CasOperation op = requests.admit();
    CasRefCatalog::Snapshot snapshot = CasRefCatalog::read(op, layout);
    snapshot.catalog.entries.push_back(CatalogEntry{
        .ns = RootNamespace{"bad/a"}, .state = NsState::Live, .incarnation = UInt128{777}});
    snapshot.catalog.entries.push_back(CatalogEntry{
        .ns = RootNamespace{"bad/b"},
        .state = NsState::Removing,
        .incarnation = UInt128{777},
        .removal_started_round = 1});
    std::sort(snapshot.catalog.entries.begin(), snapshot.catalog.entries.end(),
        [](const CatalogEntry & lhs, const CatalogEntry & rhs) { return lhs.ns.string() < rhs.ns.string(); });
    const auto catalog_head = op.head(layout.refCatalogKey(), Retry::once());
    ASSERT_TRUE(catalog_head.has_value());
    ASSERT_TRUE(std::holds_alternative<Committed>(op.replace(layout.refCatalogKey(), encodeRefCatalog(snapshot.catalog), catalog_head->etag, Retry::once())));

    FsckReport report;
    ASSERT_NO_THROW(report = runFsck(*store, /*detail=*/true));
    EXPECT_GE(report.lifeless_keys, 1u);
    EXPECT_GE(report.reachable, 1u) << "the unrelated unique namespace must still be audited";
    EXPECT_EQ(report.dangling, 0u);
}

/// A physical namespace-life key whose life id is ambiguous in the POST-LISTING cut (two catalog rows
/// share one incarnation) must be recorded as a `lifeless_keys` finding and must NOT abort the scan:
/// `CatalogLifeIndex::resolve` throws `CORRUPTED_DATA` on a duplicate, and the janitor-pending
/// classification loop must catch it exactly like every other catalog-authority failure in this scan.
/// Mirrors `DuplicateLifeIdIsReportedWhileAnUnrelatedUniqueNamespaceStillProgresses`, but that fixture
/// has no physical object under the duplicated life id, so it never drives a candidate into the new
/// post-listing loop at all -- this is the case that actually exercises it.
TEST(CASFsck, AmbiguousLifeUnderAPhysicalKeyIsRecordedNotAborted)
{
    auto backend = std::make_shared<InMemoryBackend>();
    auto store = openPoolForTest(backend);
    const Layout & layout = store->layout();
    const RootNamespace unique_ns{"00/unique@cas@"};
    const ManifestRef r = ref(1, 0xA1);
    writeBlobBody(*backend, layout, DB::UInt128(1));
    writeManifestRaw(*backend, layout, unique_ns, r, {blobEntryFor("a", DB::UInt128(1))});
    const uint64_t sequence = publishCommittedTransition(*backend, layout, unique_ns, "tbl", std::nullopt, r);
    writeFsckCheckpoint(*backend, layout, unique_ns, RefTxnId{1, sequence});

    const NamespaceLifeId duplicated_life = NamespaceLifeId::fromCatalogEntry(RootNamespace{"bad/a"}, UInt128{777});
    CasRequests requests = DB::Cas::tests::openRequestsForTest(backend);
    CasOperation op = requests.admit();
    ASSERT_TRUE(std::holds_alternative<Committed>(
        op.create(layout.namespaceFilesPrefix(duplicated_life) + "format_version.txt", "1\n", Retry::once())));
    CasRefCatalog::Snapshot snapshot = CasRefCatalog::read(op, layout);
    snapshot.catalog.entries.push_back(CatalogEntry{
        .ns = RootNamespace{"bad/a"}, .state = NsState::Live, .incarnation = UInt128{777}});
    snapshot.catalog.entries.push_back(CatalogEntry{
        .ns = RootNamespace{"bad/b"},
        .state = NsState::Removing,
        .incarnation = UInt128{777},
        .removal_started_round = 1});
    std::sort(snapshot.catalog.entries.begin(), snapshot.catalog.entries.end(),
        [](const CatalogEntry & lhs, const CatalogEntry & rhs) { return lhs.ns.string() < rhs.ns.string(); });
    const auto catalog_head = op.head(layout.refCatalogKey(), Retry::once());
    ASSERT_TRUE(catalog_head.has_value());
    ASSERT_TRUE(std::holds_alternative<Committed>(op.replace(layout.refCatalogKey(), encodeRefCatalog(snapshot.catalog), catalog_head->etag, Retry::once())));

    FsckReport report;
    ASSERT_NO_THROW(report = runFsck(*store, /*detail=*/true))
        << "an ambiguous life under a physical key must be a recorded finding, never an abort";
    EXPECT_GE(report.lifeless_keys, 1u);
    EXPECT_GE(report.reachable, 1u) << "the unrelated unique namespace must still be audited";
    EXPECT_EQ(report.dangling, 0u);
}

/// Fsck's namespace universe is catalog-authoritative. Admit `ns`, publish one real ref-log record,
/// then hide its whole stream prefix from LIST. Fsck must still retain the namespace from the catalog
/// cut and use the checkpoint-anchored arithmetic walk to read the record.
///
/// Proves `ref_records_walked`, not `dangling`/`clean()`: `checkRefStream` (which this proves runs) has
/// its own `_ckpt`-anchored arithmetic walk and so is reachable here. The distinct
/// `manifestStillReferenced` recheck now receives the same frozen catalog row and exact `_ckpt` authority;
/// its competing-cut regression is pinned separately by `MissingManifestRecheckStaysOnInitialCatalogCut`.
TEST(CASFsck, CatalogLiveNamespaceHiddenFromListIsStillWalked)
{
    auto backend = std::make_shared<HintHoleBackend>();
    auto store = openPoolForTest(backend);
    const Layout & layout = store->layout();
    const RootNamespace ns{"00/hidden_from_list@cas@"};

    fixture::admitLive(*backend, layout, ns);
    const uint64_t sequence = appendRefLogSeed(
        *backend, layout, ns, {});   // one real record: a birth-only ref-log transaction

    /// `casAdmitEntry` never publishes a `_ckpt` (by its own design), and the write above used
    /// `appendRefLogSeed`'s hardcoded writer_epoch 1. `checkRefStream`'s own walk needs SOME anchor -- a
    /// `_ckpt.life_epoch`, a listed snapshot, or a listed log -- to know where to start reading, and this
    /// test is about to hide every listed one. Without an anchor the walk sees nothing at all and
    /// correctly treats the namespace as never-born, the same "nothing to probe" trap the I4 replacement
    /// controls hit and were restructured around (fold-before-hide). fsck has no "fold" step to run
    /// first, so the anchor is published directly, by exact key, before the hide -- the exact-key GET
    /// this enables is unaffected by list-hiding either way.
    writeFsckCheckpoint(*backend, layout, ns, RefTxnId{1, sequence});
    const NamespaceLifeId life = fixture::fixtureLife(ns);

    backend->hidePrefix(layout.namespaceStreamPrefix(life));

    FsckReport rep;
    ASSERT_NO_THROW(rep = runFsck(*store, /*detail*/true));
    EXPECT_GT(backend->holesServed(), 0u)
        << "the hide must actually have been exercised by the stream LIST, or this test passes vacuously";
    EXPECT_GE(rep.ref_records_walked, 1u)
        << "the namespace must be discovered and its stream actually read even when LIST omits every one "
           "of its keys, or the catalog-authoritative universe supplement did not run";
}

TEST(CASFsckAuthority, FullListingDoesNotDefineStreamGeometry)
{
    expectListingIndependentFsck(runFsckWithListingMode(FsckListingMode::Full, "full"));
}

TEST(CASFsckAuthority, EmptyListingDoesNotDefineStreamGeometry)
{
    expectListingIndependentFsck(runFsckWithListingMode(FsckListingMode::Empty, "empty"));
}

TEST(CASFsckAuthority, PartialListingDoesNotDefineStreamGeometry)
{
    expectListingIndependentFsck(runFsckWithListingMode(FsckListingMode::Partial, "partial"));
}

TEST(CASFsckAuthority, ReorderedListingDoesNotDefineStreamGeometry)
{
    expectListingIndependentFsck(runFsckWithListingMode(FsckListingMode::Reordered, "reordered"));
}

/// Stream LIST is not fsck authority. The same exact catalog + `_ckpt` + checkpoint-base triple must
/// yield the same result when LIST is complete, empty, partial, or reordered. A newer unadopted log and
/// snapshot are inert garbage, while damage to the exact checkpoint base remains a hard finding under
/// every listing. Mutation caught: the old LIST-derived snapshot oracle makes only listings that reveal
/// the unadopted pair non-clean.
TEST(CASFsckAuthority, StreamListingDoesNotChangeCheckpointBaseVerdict)
{
    const std::array modes{
        FsckListingMode::Full,
        FsckListingMode::Empty,
        FsckListingMode::Partial,
        FsckListingMode::Reordered,
    };

    std::optional<FsckAuthorityVerdict> clean_reference;
    std::optional<FsckAuthorityVerdict> corrupt_reference;
    for (size_t i = 0; i < modes.size(); ++i)
    {
        const FsckAuthorityVerdict clean = authorityVerdict(runCheckpointBaseFsckWithListingMode(
            modes[i], "clean_" + std::to_string(i), /*corrupt_exact_base=*/false));
        if (!clean_reference)
            clean_reference = clean;
        EXPECT_EQ(clean, *clean_reference);
        EXPECT_TRUE(clean.clean);
        EXPECT_EQ(clean.hard_findings, 0u);

        const FsckAuthorityVerdict corrupt = authorityVerdict(runCheckpointBaseFsckWithListingMode(
            modes[i], "corrupt_" + std::to_string(i), /*corrupt_exact_base=*/true));
        if (!corrupt_reference)
            corrupt_reference = corrupt;
        EXPECT_EQ(corrupt, *corrupt_reference);
        EXPECT_FALSE(corrupt.clean);
        EXPECT_EQ(corrupt.chain_broken, 1u);
        EXPECT_EQ(corrupt.unchecked, 0u);
        EXPECT_EQ(corrupt.hard_findings, 1u);
    }
}

/// A durable log on the chain is history even above a generation 1 frontier: both fsck legs walk it, so
/// the owner it adds is audited and the owner it removes no longer makes its blob reachable.
TEST(CASFsckAuthority, LogAboveTheOldFrontierIsWalked)
{
    auto backend = std::make_shared<InMemoryBackend>();
    auto store = openPoolForTest(backend);
    const Layout & layout = store->layout();
    const RootNamespace ns{"00/visible_f_plus_one@cas@"};
    const ManifestRef committed_ref = ref(1, 0xE1);
    const ManifestRef unfrontiered_ref = ref(2, 0xE2);
    const DB::UInt128 committed_blob = u128Of("fsck-frontier-committed");
    writeBlobBody(*backend, layout, committed_blob);
    writeManifestRaw(*backend, layout, ns, committed_ref, {blobEntryFor("a", committed_blob)});
    const uint64_t frontier = publishCommittedTransition(
        *backend, layout, ns, "tbl", std::nullopt, committed_ref);
    writeFsckCheckpoint(*backend, layout, ns, RefTxnId{1, frontier});

    /// `_ckpt` is deliberately NOT advanced to this record: the semantic wrapper would advance it, so
    /// deposit the raw shape a stopped writer leaves behind. Its manifest is absent.
    std::vector<RefOp> unfrontiered_ops;
    unfrontiered_ops.push_back(ownerTransitionOp(
        RefOwnerBinding{RefOwnerKind::Committed, "tbl", committed_ref}, std::nullopt));
    const std::vector<RefOp> commit_ops = publishCommittedOps("tbl", unfrontiered_ref);
    unfrontiered_ops.insert(unfrontiered_ops.end(), commit_ops.begin(), commit_ops.end());
    ASSERT_EQ(appendRefLogSeed(*backend, layout, ns, std::move(unfrontiered_ops)), frontier + 1);

    const FsckReport report = runFsck(*store, /*detail=*/true);
    EXPECT_EQ(report.chain_broken, 0u);
    EXPECT_EQ(report.unchecked, 0u);
    EXPECT_EQ(report.ref_records_walked, 2u);
    EXPECT_EQ(report.dangling, 1u);
    EXPECT_EQ(report.reachable, 0u);
}

/// INV-2 materializes every burned global epoch, including an empty one, as a sequence-1 seal. A
/// direct `{1,2}` -> `{7,1}` chain that omits `{2,1}` is therefore data loss, not a legal sparse epoch
/// transition. Mutation caught: accepting the later head as a shortcut blesses the missing seal.
TEST(CASFsckAuthority, MissingBurnedEpochSealIsChainBroken)
{
    auto backend = std::make_shared<InMemoryBackend>();
    auto store = openPoolForTest(backend);
    const Layout & layout = store->layout();
    const RootNamespace ns{"00/skipped_writer_epoch@cas@"};

    fixture::writeRefLogRaw(*backend, layout, RefLogTxn{
        .ns = ns.string(), .txn_id = RefTxnId{1, 1}, .ops = {namespaceBirthOp()},
        .prev_epoch_seal = std::nullopt});
    RefOp seal;
    seal.kind = RefOpKind::EpochSeal;
    fixture::writeRefLogRaw(*backend, layout, RefLogTxn{
        .ns = ns.string(), .txn_id = RefTxnId{1, 2}, .ops = {seal},
        .prev_epoch_seal = std::nullopt});

    CasRequests requests = DB::Cas::tests::openRequestsForTest(backend);
    CasOperation op = requests.admit();
    const NamespaceLifeId life = *CasRefCatalog::lifeIfCataloged(op, layout, ns);
    /// The codec now rejects this skip. Deposit its old on-disk corruption shape by changing only the
    /// fixed-width epoch token of an otherwise encodable body, so fsck still proves that a missing
    /// intermediate epoch is reported rather than treated as a sparse legal transition.
    String skipped_bytes = encodeRefLogTxn(RefLogTxn{
        .ns = ns.string(), .txn_id = RefTxnId{7, 1}, .ops = {}, .prev_epoch_seal = RefTxnId{6, 1}});
    const String old_epoch_token = R"("!prev_epoch":"6")";
    const auto old_epoch = skipped_bytes.find(old_epoch_token);
    ASSERT_NE(old_epoch, String::npos);
    skipped_bytes.replace(old_epoch, old_epoch_token.size(), R"("!prev_epoch":"1")");
    ASSERT_TRUE(std::holds_alternative<Committed>(op.create(layout.refLogKey(life, RefTxnId{7, 1}),
        sealObject(FormatId::RefLog, skipped_bytes), Retry::once())));

    ASSERT_TRUE(std::holds_alternative<Committed>(op.create(layout.refCkptKey(life), encodeRefCkptAtGeneration(RefCkpt{
        .life_epoch = 1,
        .committed_through = RefTxnId{7, 1},
        .checkpoint_snapshot_id = std::nullopt,
        .last_epoch_seal = RefTxnId{6, 1}}, 1), Retry::once())));

    const FsckReport report = runFsck(*store, /*detail=*/true);
    EXPECT_EQ(report.chain_broken, 1u);
    EXPECT_EQ(report.unchecked, 0u);
    EXPECT_EQ(report.ref_records_walked, 2u);
}

/// The checkpoint base is the inclusive frontier, so there is no replay tail in which another hole
/// could satisfy this test. The missing same-id log itself makes the stable exact authority corrupt.
/// Mutation caught: mapping every `readCheckpointSnapshotBase` failure to `Unchecked` leaves `clean` true.
TEST(CASFsckAuthority, MissingCheckpointBaseLogIsChainBroken)
{
    auto backend = std::make_shared<InMemoryBackend>();
    auto store = openPoolForTest(backend);
    const Layout & layout = store->layout();
    const RootNamespace ns{"00/missing_checkpoint_base_log@cas@"};
    fixture::admitLive(*backend, layout, ns);

    const RefTxnId base{1, 1};
    CasRequests requests = DB::Cas::tests::openRequestsForTest(backend);
    CasOperation op = requests.admit();
    const NamespaceLifeId life = *CasRefCatalog::lifeIfCataloged(op, layout, ns);
    writeFsckCheckpointWithBase(*backend, layout, ns, base);

    const FsckReport report = runFsck(*store, /*detail=*/true);
    EXPECT_EQ(report.ref_records_walked, 0u);
    expectCheckpointBaseVerdict(
        report, layout.refSnapshotKey(life, base), FsckClass::ChainBroken, "has no matching log");
}

/// A present, valid non-seal base log rules out a stream hole; only its checkpoint-named same-id
/// snapshot is absent. Stable exact absence is damage, not lost diagnostic coverage.
TEST(CASFsckAuthority, MissingCheckpointBaseSnapshotIsChainBroken)
{
    auto backend = std::make_shared<InMemoryBackend>();
    auto store = openPoolForTest(backend);
    const Layout & layout = store->layout();
    const RootNamespace ns{"00/missing_checkpoint_base_snapshot@cas@"};
    fixture::admitLive(*backend, layout, ns);

    const RefTxnId base{1, 1};
    CasRequests requests = DB::Cas::tests::openRequestsForTest(backend);
    CasOperation op = requests.admit();
    const NamespaceLifeId life = *CasRefCatalog::lifeIfCataloged(op, layout, ns);
    fixture::writeRefLogRaw(*backend, layout, RefLogTxn{
        .ns = ns.string(), .txn_id = base, .ops = {namespaceBirthOp()}, .prev_epoch_seal = std::nullopt});
    writeFsckCheckpointWithBase(*backend, layout, ns, base);

    const FsckReport report = runFsck(*store, /*detail=*/true);
    EXPECT_EQ(report.ref_records_walked, 0u);
    expectCheckpointBaseVerdict(
        report, layout.refSnapshotKey(life, base), FsckClass::ChainBroken,
        "is absent under the supplied immutable lifecycle authority");
}

/// `_ckpt.checkpoint_snapshot_id` names a state snapshot, never an `EpochSeal`. The forged base is an
/// OLDER seal, deliberately different from `last_epoch_seal`, so comparing checkpoint metadata cannot
/// expose it: the stream audit must exact-read the base log and reject it before recovery can bless the
/// same-id snapshot.
TEST(CASFsckAuthority, CheckpointSnapshotAtOlderEpochSealIsChainBroken)
{
    auto backend = std::make_shared<InMemoryBackend>();
    auto store = openPoolForTest(backend);
    const Layout & layout = store->layout();
    const RootNamespace ns{"00/checkpoint_base_seal@cas@"};
    fixture::admitLive(*backend, layout, ns);

    const RefLogTxn birth{
        .ns = ns.string(), .txn_id = RefTxnId{1, 1}, .ops = {namespaceBirthOp()},
        .prev_epoch_seal = std::nullopt};
    fixture::writeRefLogRaw(*backend, layout, birth);
    RefOp seal;
    seal.kind = RefOpKind::EpochSeal;
    const RefLogTxn seal_txn{
        .ns = ns.string(), .txn_id = RefTxnId{1, 2}, .ops = {seal},
        .prev_epoch_seal = std::nullopt};
    fixture::writeRefLogRaw(*backend, layout, seal_txn);
    RefOp later_seal;
    later_seal.kind = RefOpKind::EpochSeal;
    fixture::writeRefLogRaw(*backend, layout, RefLogTxn{
        .ns = ns.string(), .txn_id = RefTxnId{2, 1}, .ops = {later_seal},
        .prev_epoch_seal = RefTxnId{1, 2}});

    RefTableState through_seal;
    applyRefLogTxn(through_seal, birth);
    applyRefLogTxn(through_seal, seal_txn);
    writeRefSnapshotRaw(*backend, layout, snapshotOf(through_seal, ns.string()));

    CasRequests requests = DB::Cas::tests::openRequestsForTest(backend);
    CasOperation op = requests.admit();
    const NamespaceLifeId life = *CasRefCatalog::lifeIfCataloged(op, layout, ns);
    ASSERT_TRUE(std::holds_alternative<Committed>(op.create(layout.refCkptKey(life), encodeRefCkptAtGeneration(RefCkpt{
        .life_epoch = 1,
        .committed_through = RefTxnId{2, 1},
        .checkpoint_snapshot_id = RefTxnId{1, 2},
        .last_epoch_seal = RefTxnId{2, 1}}, 1), Retry::once())));

    const FsckReport report = runFsck(*store, /*detail=*/true);
    EXPECT_EQ(report.ref_records_walked, 0u)
        << "the seal is rejected as the checkpoint base, not walked as a normal replay record";
    expectCheckpointBaseVerdict(
        report, layout.refSnapshotKey(life, RefTxnId{1, 2}), FsckClass::ChainBroken,
        "names an EpochSeal, not a snapshot base");
}

#if USE_AWS_S3
/// A transport failure while exact-reading the same valid checkpoint base proves neither presence nor
/// absence. It remains the honest third answer and must not become a hard finding. The fault is armed
/// as an access denial so it surfaces on the read's first attempt (see `FailExactGetBackend`), which
/// keeps this test's cost at one request instead of a run through `Retry::standard()`'s deadline.
TEST(CASFsckAuthority, CheckpointBaseTransportFailureIsUnchecked)
{
    auto backend = std::make_shared<FailExactGetBackend>();
    auto store = openPoolForTest(backend);
    const Layout & layout = store->layout();
    const RootNamespace ns{"00/checkpoint_base_transport@cas@"};
    fixture::admitLive(*backend, layout, ns);

    const RefTxnId base{1, 1};
    CasRequests requests = DB::Cas::tests::openRequestsForTest(backend);
    CasOperation op = requests.admit();
    const NamespaceLifeId life = *CasRefCatalog::lifeIfCataloged(op, layout, ns);
    fixture::writeRefLogRaw(*backend, layout, RefLogTxn{
        .ns = ns.string(), .txn_id = base, .ops = {namespaceBirthOp()}, .prev_epoch_seal = std::nullopt});
    RefTableState state;
    applyRefLogTxn(state, RefLogTxn{
        .ns = ns.string(), .txn_id = base, .ops = {namespaceBirthOp()}, .prev_epoch_seal = std::nullopt});
    writeRefSnapshotRaw(*backend, layout, snapshotOf(state, ns.string()));
    writeFsckCheckpointWithBase(*backend, layout, ns, base);
    backend->fail(layout.refLogKey(life, base));

    const FsckReport report = runFsck(*store, /*detail=*/true);
    EXPECT_EQ(report.ref_records_walked, 0u);
    expectCheckpointBaseVerdict(
        report, layout.refSnapshotKey(life, base), FsckClass::Unchecked, "injected access denial on exact GET");
}
#endif

/// Cleanup may advance `_ckpt` after fsck sampled it and retire the old base before fsck exact-reads it.
/// The miss is authority instability, so fsck walks again from the new `_ckpt`, which here names no base.
/// Mutation caught: classifying `CORRUPTED_DATA` without rechecking the sampled checkpoint token.
TEST(CASFsckAuthority, CheckpointBaseVanishingAfterAuthorityAdvanceRestartsTheWalk)
{
    auto backend = std::make_shared<MutateOnFirstGetBackend>();
    auto store = openPoolForTest(backend);
    const Layout & layout = store->layout();
    const RootNamespace ns{"00/checkpoint_base_advanced@cas@"};
    fixture::admitLive(*backend, layout, ns);

    const RefTxnId old_base{1, 1};
    CasRequests requests = DB::Cas::tests::openRequestsForTest(backend);
    CasOperation op = requests.admit();
    const NamespaceLifeId life = *CasRefCatalog::lifeIfCataloged(op, layout, ns);
    writeFsckCheckpointWithBase(*backend, layout, ns, old_base);
    backend->armOnFirstGet(layout.refLogKey(life, old_base), [&]
    {
        const String ckpt_key = layout.refCkptKey(life);
        OperationForTest nested_op(*backend);
        const auto head = (*nested_op).head(ckpt_key, Retry::once());
        ASSERT_TRUE(head.has_value());
        ASSERT_TRUE(std::holds_alternative<Committed>((*nested_op).replace(ckpt_key, encodeRefCkptAtGeneration(RefCkpt{
            .life_epoch = 1,
            .committed_through = std::nullopt,
            .checkpoint_snapshot_id = std::nullopt,
            .last_epoch_seal = std::nullopt}, 1), head->etag, Retry::once())));
    });

    const FsckReport report = runFsck(*store, /*detail=*/true);
    EXPECT_EQ(report.ref_records_walked, 0u);
    EXPECT_EQ(report.chain_broken, 0u);
    EXPECT_EQ(report.unchecked, 0u);
    EXPECT_TRUE(report.clean());
}

namespace
{

/// Valid checkpoint triple at `{1,1}`, `_ckpt.committed_through = {1,3}`, tail logs `{1,2}` and `{1,3}`
/// present: fsck passes the base check and walks the tail by exact key.
NamespaceLifeId writeFsckFixtureWithTail(Backend & backend, const Layout & layout, const RootNamespace & ns)
{
    fixture::admitLive(backend, layout, ns);
    const RefTxnId base{1, 1};
    const RefLogTxn birth{
        .ns = ns.string(), .txn_id = base, .ops = {namespaceBirthOp()}, .prev_epoch_seal = std::nullopt};
    fixture::writeRefLogRaw(backend, layout, birth);
    RefTableState state;
    applyRefLogTxn(state, birth);
    writeRefSnapshotRaw(backend, layout, snapshotOf(state, ns.string()));
    for (const uint64_t sequence : {2, 3})
        fixture::writeRefLogRaw(backend, layout, RefLogTxn{
            .ns = ns.string(), .txn_id = RefTxnId{1, sequence}, .ops = {}, .prev_epoch_seal = std::nullopt});

    writeFsckCheckpointWithBase(backend, layout, ns, base, std::nullopt, RefTxnId{1, 3});
    CasRequests requests = DB::Cas::tests::openRequestsForTest(backend);
    CasOperation op = requests.admit();
    return *CasRefCatalog::lifeIfCataloged(op, layout, ns);
}

size_t countVerdictsAtKey(const FsckReport & report, const String & key, FsckClass cls)
{
    return static_cast<size_t>(std::count_if(report.objects.begin(), report.objects.end(),
        [&](const FsckObject & object) { return object.key == key && object.cls == cls; }));
}

/// Deletes `key` with the raw backend, conditional on the token its `HEAD` shows; `what` prefixes the failure.
void removeFsckFixtureKey(DB::Cas::Backend & backend, const String & key, std::string_view what = {})
{
    OperationForTest op(backend);
    const auto head = (*op).head(key, Retry::once());
    ASSERT_TRUE(head.has_value()) << what << key;
    ASSERT_EQ((*op).remove(key, head->etag, Retry::once()), Removal::Removed);
}

bool uncheckedNoteContains(const FsckReport & report, const String & key, const String & text)
{
    return std::any_of(report.objects.begin(), report.objects.end(), [&](const FsckObject & object)
    {
        return object.key == key && object.cls == FsckClass::Unchecked && !object.reachable_from.empty()
            && object.reachable_from.front().find(text) != String::npos;
    });
}

}

/// Cleanup may advance `_ckpt` to a newer base and delete a tail log below it after fsck sampled the
/// checkpoint. The miss is authority instability, not a hole: fsck walks again from the new base.
/// Mutation caught: reporting an absent tail log as chain-broken without re-reading `_ckpt`.
TEST(CASFsckAuthority, CoveredTailLogVanishingAfterAuthorityAdvanceRestartsTheWalk)
{
    auto backend = std::make_shared<MutateOnFirstGetBackend>();
    auto store = openPoolForTest(backend);
    const Layout & layout = store->layout();
    const RootNamespace ns{"00/tail_log_advanced@cas@"};
    const NamespaceLifeId life = writeFsckFixtureWithTail(*backend, layout, ns);
    const String tail_key = layout.refLogKey(life, RefTxnId{1, 2});
    backend->armOnFirstGet(tail_key, [&]
    {
        RefTableState state;
        applyRefLogTxn(state, RefLogTxn{
            .ns = ns.string(), .txn_id = RefTxnId{1, 1}, .ops = {namespaceBirthOp()}, .prev_epoch_seal = std::nullopt});
        for (const uint64_t sequence : {2, 3})
            applyRefLogTxn(state, RefLogTxn{
                .ns = ns.string(), .txn_id = RefTxnId{1, sequence}, .ops = {}, .prev_epoch_seal = std::nullopt});
        writeRefSnapshotRaw(*backend, layout, snapshotOf(state, ns.string()));
        OperationForTest nested_op(*backend);
        const String ckpt_key = layout.refCkptKey(life);
        const auto ckpt_head = (*nested_op).head(ckpt_key, Retry::once());
        ASSERT_TRUE(ckpt_head.has_value());
        ASSERT_TRUE(std::holds_alternative<Committed>((*nested_op).replace(ckpt_key, encodeRefCkptAtGeneration(RefCkpt{
            .life_epoch = 1,
            .committed_through = RefTxnId{1, 3},
            .checkpoint_snapshot_id = RefTxnId{1, 3},
            .last_epoch_seal = std::nullopt}, 1), ckpt_head->etag, Retry::once())));
        const auto log_head = (*nested_op).head(tail_key, Retry::once());
        ASSERT_TRUE(log_head.has_value());
        ASSERT_EQ((*nested_op).remove(tail_key, log_head->etag, Retry::once()), Removal::Removed);
    });

    const FsckReport report = runFsck(*store, /*detail=*/true);
    EXPECT_EQ(report.chain_broken, 0u);
    EXPECT_EQ(report.unchecked, 0u);
    EXPECT_EQ(report.ref_records_walked, 0u) << "the new base is the frontier, so the second walk reads only its successor";
}

/// Cleanup may delete the covered log and `_ckpt` itself. An absent authority proves no hole.
/// Mutation caught: treating an absent `_ckpt` as the same incarnation during the tail revalidation.
TEST(CASFsckAuthority, TailLogVanishingTogetherWithCheckpointIsUnchecked)
{
    auto backend = std::make_shared<MutateOnFirstGetBackend>();
    auto store = openPoolForTest(backend);
    const Layout & layout = store->layout();
    const RootNamespace ns{"00/tail_log_ckpt_gone@cas@"};
    const NamespaceLifeId life = writeFsckFixtureWithTail(*backend, layout, ns);
    const String tail_key = layout.refLogKey(life, RefTxnId{1, 2});
    backend->armOnFirstGet(tail_key, [&]
    {
        OperationForTest nested_op(*backend);
        for (const String & key : {layout.refCkptKey(life), tail_key})
        {
            const auto head = (*nested_op).head(key, Retry::once());
            ASSERT_TRUE(head.has_value());
            ASSERT_EQ((*nested_op).remove(key, head->etag, Retry::once()), Removal::Removed);
        }
    });

    const FsckReport report = runFsck(*store, /*detail=*/true);
    EXPECT_EQ(report.chain_broken, 0u);
    EXPECT_EQ(countVerdictsAtKey(report, tail_key, FsckClass::Unchecked), 1u)
        << "an absent `_ckpt` must leave the absent tail log unchecked";
    EXPECT_TRUE(uncheckedNoteContains(report, tail_key, "changed"))
        << "the note must name the absent-authority arm, not the unreadable one";
}

/// A `_ckpt` re-read that fails cannot confirm the same incarnation, so the absent log stays unchecked.
/// Mutation caught: letting a failed re-read fall through to a chain-broken verdict.
TEST(CASFsckAuthority, TailLogVanishingWithUnreadableCheckpointIsUnchecked)
{
    auto backend = std::make_shared<MutateOnFirstGetBackend>();
    auto store = openPoolForTest(backend);
    const Layout & layout = store->layout();
    const RootNamespace ns{"00/tail_log_ckpt_unreadable@cas@"};
    const NamespaceLifeId life = writeFsckFixtureWithTail(*backend, layout, ns);
    const String tail_key = layout.refLogKey(life, RefTxnId{1, 2});
    backend->armOnFirstGet(tail_key, [&]
    {
        OperationForTest nested_op(*backend);
        const auto head = (*nested_op).head(tail_key, Retry::once());
        ASSERT_TRUE(head.has_value());
        ASSERT_EQ((*nested_op).remove(tail_key, head->etag, Retry::once()), Removal::Removed);
        backend->failNextReadWith(
            layout.refCkptKey(life), std::make_exception_ptr(std::runtime_error("injected checkpoint read fault")));
    });

    const FsckReport report = runFsck(*store, /*detail=*/true);
    EXPECT_EQ(report.chain_broken, 0u);
    EXPECT_EQ(countVerdictsAtKey(report, tail_key, FsckClass::Unchecked), 1u);
    EXPECT_TRUE(uncheckedNoteContains(report, tail_key, "could not be revalidated"))
        << "the verdict note must say the authority could not be revalidated";
}

/// The same, with a typed `DB::Exception` fault (a code the engine does not retry): its message reaches the verdict note.
/// Mutation caught: dropping the typed-exception arm, or classifying it chain-broken.
TEST(CASFsckAuthority, TailLogVanishingWithTypedCheckpointReadFaultIsUnchecked)
{
    auto backend = std::make_shared<MutateOnFirstGetBackend>();
    auto store = openPoolForTest(backend);
    const Layout & layout = store->layout();
    const RootNamespace ns{"00/tail_log_ckpt_typed_fault@cas@"};
    const NamespaceLifeId life = writeFsckFixtureWithTail(*backend, layout, ns);
    const String tail_key = layout.refLogKey(life, RefTxnId{1, 2});
    backend->armOnFirstGet(tail_key, [&]
    {
        OperationForTest nested_op(*backend);
        const auto head = (*nested_op).head(tail_key, Retry::once());
        ASSERT_TRUE(head.has_value());
        ASSERT_EQ((*nested_op).remove(tail_key, head->etag, Retry::once()), Removal::Removed);
        backend->failNextReadWith(
            layout.refCkptKey(life),
            std::make_exception_ptr(DB::Exception(DB::ErrorCodes::BAD_ARGUMENTS, "injected typed checkpoint fault")));
    });

    const FsckReport report = runFsck(*store, /*detail=*/true);
    EXPECT_EQ(report.chain_broken, 0u);
    EXPECT_EQ(countVerdictsAtKey(report, tail_key, FsckClass::Unchecked), 1u);
    EXPECT_TRUE(uncheckedNoteContains(report, tail_key, "could not be revalidated: "))
        << "the typed-exception arm appends the exception message";
    EXPECT_TRUE(uncheckedNoteContains(report, tail_key, "injected typed checkpoint fault"));
}

/// With `_ckpt` unchanged, an absent covered tail log is a real hole.
/// Mutation caught: recording `Unchecked` for every absent tail id.
TEST(CASFsckAuthority, MissingTailLogUnderUnchangedCheckpointIsChainBroken)
{
    auto backend = std::make_shared<InMemoryBackend>();
    auto store = openPoolForTest(backend);
    const Layout & layout = store->layout();
    const RootNamespace ns{"00/tail_log_missing@cas@"};
    const NamespaceLifeId life = writeFsckFixtureWithTail(*backend, layout, ns);
    const String tail_key = layout.refLogKey(life, RefTxnId{1, 2});
    {
        OperationForTest nested_op(*backend);
        const auto head = (*nested_op).head(tail_key, Retry::once());
        ASSERT_TRUE(head.has_value());
        ASSERT_EQ((*nested_op).remove(tail_key, head->etag, Retry::once()), Removal::Removed);
    }

    const FsckReport report = runFsck(*store, /*detail=*/true);
    EXPECT_EQ(report.chain_broken, 1u);
    EXPECT_EQ(report.unchecked, 0u);
    EXPECT_EQ(countVerdictsAtKey(report, tail_key, FsckClass::ChainBroken), 1u)
        << "the verdict must name the absent tail log's exact key";
    EXPECT_EQ(report.ref_records_walked, 0u);
}

/// A Live catalog row without `_ckpt` is not a recoverable table, even when a listing happens to show a
/// complete ref log. Mutation caught: replacing the authority-taking recovery with the old LIST replay
/// makes this audit look clean and silently blesses a life whose durable frontier is unknown.
TEST(CASFsck, LiveNamespaceWithoutCheckpointIsUnchecked)
{
    auto backend = std::make_shared<InMemoryBackend>();
    auto store = openPoolForTest(backend);
    const RootNamespace ns{"00/live_without_checkpoint@cas@"};
    const ManifestRef r = ref(1, 0xC1);
    writeBlobBody(*backend, store->layout(), DB::UInt128(1));
    writeManifestRaw(*backend, store->layout(), ns, r, {blobEntryFor("a", DB::UInt128(1))});
    /// `publishCommittedTransition` correctly advances `_ckpt`; this test instead deposits the raw
    /// missing-checkpoint corruption shape that fsck must refuse to recover.
    std::vector<RefOp> ops = publishCommittedOps("tbl", r);
    appendRefLogSeed(*backend, store->layout(), ns, std::move(ops));

    const FsckReport report = runFsck(*store, /*detail=*/true);
    EXPECT_GE(report.unchecked, 1u)
        << "a Live life with no exact checkpoint cannot be recovered from a convenient LIST";
    EXPECT_EQ(report.reachable, 0u)
        << "fsck must not consume refs after the mandatory recovery authority was absent";
}

/// A rebirth during the scan replaces the walked life's catalog row. The catalog read after the per-life
/// loop no longer names that life, so its missing manifest is janitor-pending, not a dangle.
TEST(CASFsck, MissingManifestOfALifeRebornDuringTheScanIsJanitorPending)
{
    auto backend = std::make_shared<MutateOnFirstGetBackend>();
    auto store = openPoolForTest(backend);
    const Layout & layout = store->layout();
    const RootNamespace ns{"00/frozen_fsck_cut@cas@"};
    const ManifestRef r = ref(1, 0xC2);
    const uint64_t sequence = publishCommittedTransition(*backend, layout, ns, "tbl", std::nullopt, r);
    /// `publishCommittedTransition`'s raw fixture log writer uses its documented epoch 1.
    writeFsckCheckpoint(*backend, layout, ns, RefTxnId{1, sequence});

    backend->armOnFirstGet(layout.manifestKey(ManifestId{ns, r}), [&]
    {
        replaceCatalogLife(*backend, layout, ns, UInt128{0xC3});
    });

    const FsckReport report = runFsck(*store, /*detail=*/true);
    EXPECT_EQ(report.dangling, 0u) << "no finding is reported about a life the catalog no longer names";
    EXPECT_GE(report.namespace_janitor_pending_lives, 1u);
}

/// A pre-precommit body in an eligible prefix (no owner) is INFO (Unreachable), not an error.
TEST(CASFsck, ReclaimablePrePrecommitBodyIsInfo)
{
    auto backend = std::make_shared<InMemoryBackend>();
    auto store = openPoolForTest(backend);
    const RootNamespace ns{"00/aa@cas@"};
    /// Seed a birth-only ref log through the fixture helper, which also admits the catalog row, while
    /// leaving NO committed owner; the manifest body below is orphan debris.
    const uint64_t sequence = appendRefLogSeed(*backend, store->layout(), ns, {});
    writeFsckCheckpoint(*backend, store->layout(), ns, RefTxnId{1, sequence});
    const ManifestRef r = ref(5, 0xAB);
    writeManifestRaw(*backend, store->layout(), ns, r, {blobEntryFor("a", DB::UInt128(1))});   // body, no owner
    setWatermarkMinActive(*backend, store->layout(), kServerRoot, kWriterEpoch, 6);   // eligible
    const FsckReport rep = runFsck(*store, /*detail*/true);
    EXPECT_TRUE(rep.clean());            // not an error
    EXPECT_GE(rep.unreachable, 1u);      // counted as info/unreachable
}

/// Pipeline classification (2026-07-02): a condemned-but-present blob is PendingGc — an EXPECTED
/// pipeline state (deletion is scheduled), never the suspicious "unreachable" lump beta testers
/// read as a leak. clean() is unaffected.
TEST(CASFsck, CondemnedBlobClassifiesPendingGc)
{
    auto backend = std::make_shared<InMemoryBackend>();
    auto store = openPoolForTest(backend);
    const RootNamespace ns{"00/aa@cas@"};
    const ManifestRef r = ref(1, 0xA1);
    writeBlobBody(*backend, store->layout(), DB::UInt128(1));
    writeManifestRaw(*backend, store->layout(), ns, r, {blobEntryFor("a", DB::UInt128(1))});
    const uint64_t publish_sequence = publishCommittedTransition(
        *backend, store->layout(), ns, "tbl", std::nullopt, r);
    writeFsckCheckpoint(*backend, store->layout(), ns, RefTxnId{1, publish_sequence});
    Gc gc(store, hexToU128("00000000000000000000000000000001"));
    gc.runRegularRound();
    const uint64_t drop_sequence = dropRefTransition(*backend, store->layout(), ns, "tbl", r);
    writeFsckCheckpoint(*backend, store->layout(), ns, RefTxnId{1, drop_sequence});
    gc.runRegularRound();   /// -1 folds => zero => condemned, its intent confirmed (pending); blob still present

    const FsckReport rep = runFsck(*store, /*detail*/true);
    EXPECT_TRUE(rep.clean());
    EXPECT_EQ(rep.pending_gc, 1u);
    EXPECT_EQ(rep.unaccounted, 0u);
    bool saw = false;
    for (const FsckObject & o : rep.objects)
        if (o.cls == FsckClass::PendingGc)
        {
            saw = true;
            ASSERT_FALSE(o.reachable_from.empty());
            EXPECT_NE(o.reachable_from[0].find("delete_pending"), String::npos);
        }
    EXPECT_TRUE(saw);
}

/// A drop whose -1 has NOT folded yet: the blob's edges are still in the GC snapshot => AwaitingGc
/// (expected), not Unaccounted.
TEST(CASFsck, DroppedButUnfoldedBlobClassifiesAwaitingGc)
{
    auto backend = std::make_shared<InMemoryBackend>();
    auto store = openPoolForTest(backend);
    const RootNamespace ns{"00/aa@cas@"};
    const ManifestRef r = ref(1, 0xA1);
    writeBlobBody(*backend, store->layout(), DB::UInt128(1));
    writeManifestRaw(*backend, store->layout(), ns, r, {blobEntryFor("a", DB::UInt128(1))});
    const uint64_t publish_sequence = publishCommittedTransition(
        *backend, store->layout(), ns, "tbl", std::nullopt, r);
    writeFsckCheckpoint(*backend, store->layout(), ns, RefTxnId{1, publish_sequence});
    Gc gc(store, hexToU128("00000000000000000000000000000001"));
    gc.runRegularRound();                                        /// +1 folded into the snapshot
    const uint64_t drop_sequence = dropRefTransition(
        *backend, store->layout(), ns, "tbl", r);  /// -1 NOT folded (no round)
    writeFsckCheckpoint(*backend, store->layout(), ns, RefTxnId{1, drop_sequence});

    const FsckReport rep = runFsck(*store, /*detail*/true);
    EXPECT_TRUE(rep.clean());
    EXPECT_EQ(rep.awaiting_gc, 1u);
    EXPECT_EQ(rep.unaccounted, 0u);
}

/// Stale-edge cross-check, NEGATIVE side: the residual edge's source manifest body is still PRESENT in
/// the pool, so its removal still has a `-1` to fold (and the orphan sweep still has a body to reclaim).
/// That is a genuine mid-pipeline backlog and must keep the `AwaitingGc` verdict — the new check may
/// never turn an ordinary unfolded drop into a hard finding.
TEST(CASFsck, UnfoldedDropWithPresentSourceManifestStaysAwaitingGc)
{
    auto backend = std::make_shared<InMemoryBackend>();
    auto store = openPoolForTest(backend);
    const RootNamespace ns{"00/aa@cas@"};
    const ManifestRef r = ref(1, 0xA1);
    writeBlobBody(*backend, store->layout(), DB::UInt128(1));
    writeManifestRaw(*backend, store->layout(), ns, r, {blobEntryFor("a", DB::UInt128(1))});
    const uint64_t publish_sequence = publishCommittedTransition(
        *backend, store->layout(), ns, "tbl", std::nullopt, r);
    writeFsckCheckpoint(*backend, store->layout(), ns, RefTxnId{1, publish_sequence});
    Gc gc(store, hexToU128("00000000000000000000000000000001"));
    gc.runRegularRound();                                        /// +1 folded into the snapshot
    const uint64_t drop_sequence = dropRefTransition(
        *backend, store->layout(), ns, "tbl", r);  /// -1 NOT folded; the BODY survives
    writeFsckCheckpoint(*backend, store->layout(), ns, RefTxnId{1, drop_sequence});

    const FsckReport rep = runFsck(*store, /*detail*/true);
    EXPECT_TRUE(rep.clean());
    EXPECT_EQ(rep.awaiting_gc, 1u);
    EXPECT_EQ(rep.stale_edge, 0u);
    bool saw = false;
    for (const FsckObject & o : rep.objects)
        if (o.cls == FsckClass::AwaitingGc)
            saw = true;
    EXPECT_TRUE(saw);
}

/// Stale-edge cross-check, POSITIVE side: the blob's only residual `+1` names a manifest that no longer
/// exists anywhere in the pool, so no `-1` is left to fold — the in-degree stays at 1 for every future
/// round and the incremental GC can never nominate the blob. It must NOT be labeled `AwaitingGc`
/// ("expected, no action needed", the sentence that hid 56 permanently retained blobs); it is the hard
/// `StaleEdge` finding and the report is not `clean()`.
TEST(CASFsck, ResidualEdgeNamingAnAbsentManifestClassifiesStaleEdge)
{
    auto backend = std::make_shared<InMemoryBackend>();
    auto store = openPoolForTest(backend);
    const RootNamespace ns{"00/aa@cas@"};
    const ManifestRef r = ref(1, 0xA1);
    writeBlobBody(*backend, store->layout(), DB::UInt128(1));
    const ManifestId id = writeManifestRaw(*backend, store->layout(), ns, r, {blobEntryFor("a", DB::UInt128(1))});
    const uint64_t publish_sequence = publishCommittedTransition(
        *backend, store->layout(), ns, "tbl", std::nullopt, r);
    writeFsckCheckpoint(*backend, store->layout(), ns, RefTxnId{1, publish_sequence});
    Gc gc(store, hexToU128("00000000000000000000000000000001"));
    gc.runRegularRound();                                        /// +1 folded into the snapshot
    const uint64_t drop_sequence = dropRefTransition(
        *backend, store->layout(), ns, "tbl", r);  /// the owner is gone ...
    writeFsckCheckpoint(*backend, store->layout(), ns, RefTxnId{1, drop_sequence});
    deleteManifestBody(*backend, store->layout(), id);           /// ... and so is the body, un-folded

    const FsckReport rep = runFsck(*store, /*detail*/true);
    EXPECT_EQ(rep.stale_edge, 1u);
    EXPECT_EQ(rep.awaiting_gc, 0u);
    EXPECT_EQ(rep.dangling, 0u) << "no committed ref names the manifest any more — this is not a dangle";
    EXPECT_FALSE(rep.clean());
    bool saw = false;
    for (const FsckObject & o : rep.objects)
        if (o.cls == FsckClass::StaleEdge)
        {
            saw = true;
            ASSERT_FALSE(o.reachable_from.empty());
            EXPECT_NE(o.reachable_from[0].find("no longer exist"), String::npos);
        }
    EXPECT_TRUE(saw);
}

/// GC never ran on the pool: nothing is classifiable through the GC view — everything unreferenced
/// is AwaitingGc ("GC has not run yet"), never a false Unaccounted alarm.
TEST(CASFsck, GcNeverRanClassifiesAwaitingGc)
{
    auto backend = std::make_shared<InMemoryBackend>();
    auto store = openPoolForTest(backend);
    writeBlobBody(*backend, store->layout(), DB::UInt128(5));   /// present, never referenced, no gc/state

    const FsckReport rep = runFsck(*store, /*detail*/true);
    EXPECT_TRUE(rep.clean());
    EXPECT_EQ(rep.awaiting_gc, 1u);
    EXPECT_EQ(rep.unaccounted, 0u);
}

/// A blob outside the WHOLE GC view on a pool where GC runs: Unaccounted — expected only as a
/// transient (fast create+drop between rounds); persistent occurrences violate INV-2.
TEST(CASFsck, ForeignBlobClassifiesUnaccounted)
{
    auto backend = std::make_shared<InMemoryBackend>();
    auto store = openPoolForTest(backend);
    const RootNamespace ns{"00/aa@cas@"};
    const ManifestRef r = ref(1, 0xA1);
    writeBlobBody(*backend, store->layout(), DB::UInt128(1));
    writeManifestRaw(*backend, store->layout(), ns, r, {blobEntryFor("a", DB::UInt128(1))});
    const uint64_t sequence = publishCommittedTransition(*backend, store->layout(), ns, "tbl", std::nullopt, r);
    writeFsckCheckpoint(*backend, store->layout(), ns, RefTxnId{1, sequence});
    Gc gc(store, hexToU128("00000000000000000000000000000001"));
    gc.runRegularRound();

    writeBlobBody(*backend, store->layout(), DB::UInt128(0xF0F0));   /// never referenced anywhere

    const FsckReport rep = runFsck(*store, /*detail*/true);
    EXPECT_TRUE(rep.clean());
    EXPECT_EQ(rep.unaccounted, 1u);
    EXPECT_EQ(rep.pending_gc, 0u);
}

namespace
{
/// Injects the faults the intent classes distinguish: a refused read or `HEAD`, a weak token on a body
/// `HEAD`, and a refused delete. Every other request reaches the in-memory store unchanged. A refused
/// request is still journalled, as the real request path journals it on entry.
class FsckFaultBackend : public CountingBackend
{
public:
    std::set<String> refused_reads;
    std::set<String> refused_read_prefixes;
    std::set<String> refused_heads;
    std::set<String> refused_deletes;
    std::set<String> weak_heads;
    std::map<String, String> head_values;

    /// Runs `hook` once, just before the read of `key` that follows `skip` earlier reads of it.
    void beforeRead(const String & key, size_t skip, std::function<void()> hook)
    {
        std::lock_guard lock(hook_mutex);
        read_hooks[key] = {skip, std::move(hook)};
    }

    std::optional<Raw> read(const String & key, TransportAccess & access) override
    {
        std::function<void()> hook;
        {
            std::lock_guard lock(hook_mutex);
            if (const auto it = read_hooks.find(key); it != read_hooks.end())
            {
                if (it->second.first > 0)
                    --it->second.first;
                else
                {
                    hook = std::move(it->second.second);
                    read_hooks.erase(it);
                }
            }
        }
        if (hook)
            hook();
        auto got = CountingBackend::read(key, access);
        if (refused_reads.contains(key))
            throw DB::Exception(DB::ErrorCodes::NOT_IMPLEMENTED, "injected refusal of the read of {}", key);
        for (const String & prefix : refused_read_prefixes)
            if (key.starts_with(prefix))
                throw DB::Exception(DB::ErrorCodes::NOT_IMPLEMENTED, "injected refusal (403) of the read of {}", key);
        return got;
    }

    /// Runs `hook` once, just before the first `HEAD` of `key`.
    void beforeHead(const String & key, std::function<void()> hook)
    {
        std::lock_guard lock(hook_mutex);
        head_hooks[key] = std::move(hook);
    }

    std::optional<RawMeta> head(const String & key, TransportAccess & access) override
    {
        std::function<void()> hook;
        {
            std::lock_guard lock(hook_mutex);
            if (const auto it = head_hooks.find(key); it != head_hooks.end())
            {
                hook = std::move(it->second);
                head_hooks.erase(it);
            }
        }
        if (hook)
            hook();
        auto meta = CountingBackend::head(key, access);
        if (refused_heads.contains(key))
            throw DB::Exception(DB::ErrorCodes::NOT_IMPLEMENTED, "injected refusal of the HEAD of {}", key);
        if (meta && weak_heads.contains(key))
            meta->value = "W/\"weak\"";
        if (meta && head_values.contains(key))
            meta->value = head_values.at(key);
        return meta;
    }

    RawRemoval remove(const String & key, const String & expected_value, TransportAccess & access) override
    {
        if (refused_deletes.contains(key))
        {
            /// A value no object holds, so the journalled request deletes nothing before it is refused.
            (void)CountingBackend::remove(key, expected_value + "#refused", access);
            throw DB::Exception(DB::ErrorCodes::NOT_IMPLEMENTED, "injected refusal of the delete of {}", key);
        }
        return CountingBackend::remove(key, expected_value, access);
    }

private:
    std::mutex hook_mutex;
    std::map<String, std::pair<size_t, std::function<void()>>> read_hooks;
    std::map<String, std::function<void()>> head_hooks;
};

/// A pool whose bodies, intents and legacy markers the tests plant one by one, with no GC round.
struct FsckIntentFixture
{
    std::shared_ptr<FsckFaultBackend> backend = std::make_shared<FsckFaultBackend>();
    PoolPtr store = openPoolForTest(backend);
    const Layout & layout = store->layout();

    /// High bits set, so that blobs spread over the shards of a pool with `gc_shards` above one.
    static CanonicalIdentity staleIdentity(uint64_t b) { return *canonicalIdentity(Dialect::Emulated, std::to_string(900000 + b)); }
    String bodyKey(uint64_t b) const { return layout.blobKey(blobRefOf(b)); }
    String markerKey(uint64_t b) const { return layout.blobMetaKey(blobRefOf(b)); }

    void body(uint64_t b) { writeBlobBody(*backend, layout, numberedBlobHash(b)); }

    /// The intent of the body now at blob `b`, as the round that condemned it would have written it.
    String currentIntent(uint64_t b)
    {
        plantIntentForCurrentBody(*backend, layout, blobRefOf(b));
        return *intentKeyOfCurrentBody(*backend, layout, blobRefOf(b));
    }

    /// An intent that names an incarnation no body here ever carries.
    String staleIntent(uint64_t b)
    {
        const String key = layout.blobIntentKey(blobRefOf(b), staleIdentity(b));
        create(key, blobIntentRecord());
        return key;
    }

    void create(const String & key, const String & bytes)
    {
        OperationForTest op(*backend);
        ASSERT_TRUE(std::holds_alternative<Committed>((*op).create(key, bytes, Retry::standard()))) << key;
    }

    void marker(uint64_t b) { writeMetaClean(*backend, layout, numberedBlobHash(b), 1); }

    void removeKey(const String & key) { removeFsckFixtureKey(*backend, key); }

    FsckReport fsck(bool detail = true, bool fix = false, bool verify = false)
    {
        return runFsck(*store, detail, {}, {}, false, {}, fix, verify);
    }

    std::vector<String> allKeys() const
    {
        OperationForTest op(*backend);
        std::vector<String> keys;
        (*op).forEachListedKey("p/", [&](const ListedKey & k) { keys.push_back(k.key); return true; },
                               Retry::standard(), CasOperation::kCasListPageKeys);
        std::sort(keys.begin(), keys.end());
        return keys;
    }
};

bool isMutation(JournalVerb verb)
{
    return verb == JournalVerb::Write || verb == JournalVerb::Remove || verb == JournalVerb::RemoveMany || verb == JournalVerb::Publish;
}

/// Every key a mutating request of the journal named, with its verb, in order.
std::vector<std::pair<JournalVerb, String>> mutatedKeys(const std::vector<JournalEntry> & journal)
{
    std::vector<std::pair<JournalVerb, String>> out;
    for (const JournalEntry & entry : journal)
        if (isMutation(entry.verb))
            for (const String & key : entry.keys)
                out.emplace_back(entry.verb, key);
    return out;
}

size_t countKeyRequests(const std::vector<JournalEntry> & journal, JournalVerb verb, const String & key)
{
    size_t n = 0;
    for (const JournalEntry & entry : journal)
        if (entry.verb == verb)
            n += static_cast<size_t>(std::count(entry.keys.begin(), entry.keys.end(), key));
    return n;
}
}

/// The verdicts that are not a finding and the one that is, each from a fixture that produces exactly that
/// state: the counter, the detail row, and the clean verdict.
TEST(CASFsck, IntentClassesCurrentReplacedBodyGoneMalformed)
{
    FsckIntentFixture f;
    f.body(1);
    f.body(2);
    f.body(3);
    f.body(4);
    const String current = f.currentIntent(1);
    const String replaced = f.staleIntent(2);
    const String gone = f.currentIntent(3);
    f.removeKey(f.bodyKey(3));
    const String malformed = f.layout.blobIntentKey(blobRefOf(4), FsckIntentFixture::staleIdentity(4));
    f.create(malformed, "not an intent\n");

    const FsckReport rep = f.fsck(/*detail=*/true, /*fix_legacy_markers=*/ false, /*verify=*/true);

    EXPECT_TRUE(rep.intents_verified);
    EXPECT_EQ(rep.intent_listed, 4u) << "the key whose bytes are junk parses, so it is listed as well";
    EXPECT_EQ(rep.intent_current, 1u);
    EXPECT_EQ(rep.intent_replaced, 1u);
    EXPECT_EQ(rep.intent_body_gone, 1u);
    EXPECT_EQ(rep.intent_malformed, 1u);
    EXPECT_EQ(rep.intent_unread, 0u);
    EXPECT_EQ(countVerdictsAtKey(rep, current, FsckClass::IntentCurrent), 1u);
    EXPECT_EQ(countVerdictsAtKey(rep, replaced, FsckClass::IntentReplaced), 1u);
    EXPECT_EQ(countVerdictsAtKey(rep, gone, FsckClass::IntentBodyGone), 1u);
    EXPECT_EQ(countVerdictsAtKey(rep, malformed, FsckClass::IntentMalformed), 1u);
    EXPECT_EQ(rep.dangling, 0u);
}

/// A plain scan classifies an intent from its key alone: it counts it and sends no `GET` or `HEAD` for it.
/// The bytes and the body behind a listed key are looked at only by a verifying scan.
TEST(CASFsck, PlainScanCountsIntentsFromTheListingAndReadsNone)
{
    FsckIntentFixture f;
    f.body(1);
    f.body(2);
    f.body(3);
    const String current = f.currentIntent(1);
    const String replaced = f.staleIntent(2);
    const String junk = f.layout.blobIntentKey(blobRefOf(3), FsckIntentFixture::staleIdentity(3));
    f.create(junk, "not an intent\n");
    const String malformed = f.layout.blobIntentsPrefix() + "zz/not-a-blob/etag/00";
    f.create(malformed, blobIntentRecord());
    f.backend->clearJournal();

    const FsckReport rep = f.fsck(/*detail=*/true);

    EXPECT_FALSE(rep.intents_verified);
    EXPECT_EQ(rep.intent_listed, 3u) << "positive control: the well-formed keys were counted";
    EXPECT_EQ(rep.intent_malformed, 1u) << "only the key the parser refuses; the junk bytes need `verify`";
    EXPECT_EQ(rep.intent_current + rep.intent_replaced + rep.intent_body_gone + rep.intent_unread, 0u);
    EXPECT_GE(countKeyRequests(f.backend->journal(), JournalVerb::List, f.layout.blobIntentsPrefix()), 1u)
        << "positive control: the intent prefix was listed";
    for (const String & key : {current, replaced, junk, malformed})
        EXPECT_EQ(countKeyRequests(f.backend->journal(), JournalVerb::Read, key), 0u) << key;
    for (const uint64_t b : {1, 2, 3})
        EXPECT_EQ(countKeyRequests(f.backend->journal(), JournalVerb::Head, f.bodyKey(b)), 0u) << b << ": the body is not looked at";
    EXPECT_FALSE(rep.clean()) << "the key the parser refuses is a hard finding in every scan";
}

/// What a plain scan does not see, stated by the test: junk bytes under a well-formed key are found only when
/// verifying, and then the report is unclean.
TEST(CASFsck, MalformedBytesNeedTheVerifyingScan)
{
    FsckIntentFixture f;
    f.body(1);
    f.create(f.layout.blobIntentKey(blobRefOf(1), FsckIntentFixture::staleIdentity(1)), "junk");

    const FsckReport plain = f.fsck(/*detail=*/false);
    EXPECT_EQ(plain.intent_malformed, 0u);
    EXPECT_TRUE(plain.clean());

    const FsckReport verified = f.fsck(/*detail=*/false, /*fix_legacy_markers=*/ false, /*verify=*/true);
    EXPECT_EQ(verified.intent_malformed, 1u);
    EXPECT_FALSE(verified.clean());
    EXPECT_NE(formatFsckSummary(verified).find("intent_malformed=1"), String::npos) << formatFsckSummary(verified);
}

/// A key under `blob-intents/` that `parseBlobIntentKey` refuses is malformed, and is not read.
TEST(CASFsck, MalformedKeyIsIntentMalformed)
{
    FsckIntentFixture f;
    const String key = f.layout.blobIntentsPrefix() + "zz/not-a-blob/etag/00";
    f.create(key, blobIntentRecord());
    ASSERT_FALSE(f.layout.parseBlobIntentKey(key).has_value()) << "fixture: the key must be one the parser refuses";
    f.backend->clearJournal();

    const FsckReport rep = f.fsck();

    EXPECT_EQ(rep.intent_malformed, 1u);
    EXPECT_EQ(countVerdictsAtKey(rep, key, FsckClass::IntentMalformed), 1u);
    EXPECT_GE(countKeyRequests(f.backend->journal(), JournalVerb::List, f.layout.blobIntentsPrefix()), 1u)
        << "positive control: the intent prefix was listed";
    EXPECT_EQ(countKeyRequests(f.backend->journal(), JournalVerb::Read, key), 0u) << "a key that is no intent key is not read";
}

/// `intent_malformed` is a hard finding: the report is unclean and the summary line carries the count.
TEST(CASFsck, MalformedIntentMakesTheReportUnclean)
{
    FsckIntentFixture f;
    f.body(1);
    f.create(f.layout.blobIntentsPrefix() + "zz/not-a-blob/etag/00", blobIntentRecord());

    const FsckReport rep = f.fsck(/*detail=*/false);

    EXPECT_EQ(rep.intent_malformed, 1u);
    EXPECT_FALSE(rep.clean());
    EXPECT_NE(formatFsckSummary(rep).find("intent_malformed=1"), String::npos) << formatFsckSummary(rep);
}

/// An intent over an absent body is the normal state of a deleted blob until GC's later round.
TEST(CASFsck, BodyGoneIntentKeepsTheReportClean)
{
    FsckIntentFixture f;
    f.body(1);
    f.currentIntent(1);
    f.removeKey(f.bodyKey(1));

    const FsckReport rep = f.fsck(/*detail=*/false, /*fix_legacy_markers=*/ false, /*verify=*/true);

    EXPECT_EQ(rep.intent_body_gone, 1u) << "positive control: the intent was classified";
    EXPECT_TRUE(rep.clean());
    EXPECT_EQ(rep.dangling, 0u);
}

/// A read of the intent or a `HEAD` of its body that fails is `intent_unread`: a hard finding that carries the
/// first reason, and nothing is deleted.
TEST(CASFsck, FailedReadIsUnread)
{
    FsckIntentFixture f;
    f.body(1);
    f.body(2);
    const String read_refused = f.currentIntent(1);
    const String head_refused = f.currentIntent(2);
    f.backend->refused_reads.insert(read_refused);
    f.backend->refused_heads.insert(f.bodyKey(2));
    f.backend->clearJournal();

    const FsckReport rep = f.fsck(/*detail=*/true, /*fix_legacy_markers=*/ false, /*verify=*/true);

    EXPECT_EQ(rep.intent_unread, 2u);
    EXPECT_EQ(rep.intent_current + rep.intent_replaced + rep.intent_body_gone + rep.intent_malformed, 0u);
    EXPECT_EQ(countVerdictsAtKey(rep, read_refused, FsckClass::IntentUnread), 1u);
    EXPECT_EQ(countVerdictsAtKey(rep, head_refused, FsckClass::IntentUnread), 1u);
    EXPECT_FALSE(rep.clean());
    EXPECT_NE(rep.intent_unread_reason.find("NOT_IMPLEMENTED"), String::npos) << rep.intent_unread_reason;
    EXPECT_NE(rep.intent_unread_reason.find("injected refusal"), String::npos) << rep.intent_unread_reason;
    EXPECT_TRUE(mutatedKeys(f.backend->journal()).empty());
}

/// A refusal of everything under `blob-intents/` (a bucket policy without the read grant) must not read as a
/// clean pool: every intent is unread, the report is unclean and the summary names the count.
TEST(CASFsck, RefusedIntentPrefixIsAHardFinding)
{
    FsckIntentFixture f;
    f.body(1);
    f.body(2);
    f.currentIntent(1);
    f.currentIntent(2);
    f.backend->refused_read_prefixes.insert(f.layout.blobIntentsPrefix());

    const FsckReport rep = f.fsck(/*detail=*/false, /*fix_legacy_markers=*/ false, /*verify=*/true);

    EXPECT_EQ(rep.intent_unread, 2u);
    EXPECT_FALSE(rep.clean());
    EXPECT_NE(formatFsckSummary(rep).find("intent_unread=2"), String::npos) << formatFsckSummary(rep);
    EXPECT_FALSE(rep.intent_unread_reason.empty());
    EXPECT_LE(rep.intent_unread_reason.size(), 300u) << "the reason is shortened";

    const FsckReport plain = f.fsck(/*detail=*/false);
    EXPECT_EQ(plain.intent_unread, 0u) << "a plain scan reads no intent of its own: nothing to refuse";
}

/// A body token with no canonical identity cannot be compared with the intent's key.
TEST(CASFsck, UncanonicalHeadTokenIsUnread)
{
    FsckIntentFixture f;
    f.body(1);
    const String intent = f.currentIntent(1);
    f.backend->weak_heads.insert(f.bodyKey(1));

    const FsckReport rep = f.fsck(/*detail=*/true, /*fix_legacy_markers=*/ false, /*verify=*/true);

    EXPECT_EQ(rep.intent_unread, 1u);
    EXPECT_EQ(countVerdictsAtKey(rep, intent, FsckClass::IntentUnread), 1u);
    EXPECT_EQ(rep.intent_current + rep.intent_replaced + rep.intent_body_gone, 0u) << "no verdict from a token that cannot be compared";
    EXPECT_FALSE(rep.clean());
    EXPECT_FALSE(rep.intent_unread_reason.empty());
}

/// In no mode does a scan write, publish, or delete a body or an intent. The fix run deletes `.meta` keys only.
TEST(CASFsck, NoIntentIsEverDeleted)
{
    FsckIntentFixture f;
    f.body(1);
    f.body(2);
    f.body(3);
    const String intent_one = f.currentIntent(1);
    f.staleIntent(2);
    f.currentIntent(3);
    f.removeKey(f.bodyKey(3));
    f.marker(1);
    f.marker(5);
    const std::vector<String> before = f.allKeys();

    for (const auto & [fix, verify] : std::vector<std::pair<bool, bool>>{
             {/*fix_legacy_markers=*/ false, false}, {/*fix_legacy_markers=*/ false, true}, {/*fix_legacy_markers=*/ true, false}, {/*fix_legacy_markers=*/ true, true}})
    {
        f.backend->clearJournal();
        const FsckReport rep = f.fsck(/*detail=*/true, fix, verify);

        EXPECT_EQ(rep.intent_listed, 3u) << "positive control: intents were listed";
        if (verify)
        {
            EXPECT_EQ(rep.intent_current, 1u) << "positive control: intents were classified";
            EXPECT_EQ(rep.intent_replaced, 1u);
            EXPECT_EQ(rep.intent_body_gone, 1u);
        }
        const auto journal = f.backend->journal();
        EXPECT_EQ(countKeyRequests(journal, JournalVerb::Read, intent_one) >= 1u, verify) << "positive control: an intent was read only when verifying";
        for (const auto & [verb, key] : mutatedKeys(journal))
        {
            EXPECT_EQ(verb, JournalVerb::Remove) << "the only mutation a scan may send is a single delete: " << key;
            EXPECT_TRUE(fix == /*fix_legacy_markers=*/ true && key.ends_with(".meta")) << "unexpected mutation of " << key;
        }
    }

    const std::vector<String> after = f.allKeys();
    for (const String & key : before)
        if (!key.ends_with(".meta"))
            EXPECT_TRUE(std::binary_search(after.begin(), after.end(), key)) << "deleted " << key;
}

/// A refused delete stops the run: markers before it are gone, the refused one and those after it stay, and no
/// key outside `.meta` was deleted.
TEST(CASFsck, RefusedDeleteStopsTheRun)
{
    FsckIntentFixture f;
    f.body(1);
    const String intent = f.currentIntent(1);
    for (const uint64_t b : {11, 12, 13})
        f.marker(b);
    std::vector<String> markers{f.markerKey(11), f.markerKey(12), f.markerKey(13)};
    std::sort(markers.begin(), markers.end());
    f.backend->refused_deletes.insert(markers[1]);
    f.backend->clearJournal();

    const String message
        = messageOf(DB::ErrorCodes::NOT_IMPLEMENTED, [&] { (void)f.fsck(/*detail=*/false, /*fix_legacy_markers=*/ true); });
    EXPECT_NE(message.find("1 legacy marker(s) deleted before"), String::npos) << message;
    EXPECT_FALSE(objectPresent(*f.backend, markers[0]));
    EXPECT_TRUE(objectPresent(*f.backend, markers[1]));
    EXPECT_TRUE(objectPresent(*f.backend, markers[2])) << "the run stops at the refusal";
    EXPECT_TRUE(objectPresent(*f.backend, f.bodyKey(1)));
    EXPECT_TRUE(objectPresent(*f.backend, intent));
    const auto mutations = mutatedKeys(f.backend->journal());
    EXPECT_FALSE(mutations.empty()) << "positive control: the fix sent deletes";
    for (const auto & [verb, key] : mutations)
        EXPECT_TRUE(key.ends_with(".meta")) << "a failed fix run deleted " << key;
}

/// A marker on a generation 4 pool is debris: reported, never an error; only the fix removes it, and only keys
/// that parse as a marker's.
TEST(CASFsck, LegacyMarkersAreDebrisAndTheFixDeletesThem)
{
    FsckIntentFixture f;
    f.body(1);
    f.marker(1);
    f.marker(2);
    f.marker(3);
    const String foreign = f.layout.blobsPrefix() + "not-a-blob.meta";
    f.create(foreign, "x");

    const FsckReport plain = f.fsck();
    EXPECT_EQ(plain.legacy_marker, 3u);
    EXPECT_EQ(plain.legacy_markers_deleted, 0u);
    EXPECT_TRUE(plain.clean());
    EXPECT_EQ(countVerdictsAtKey(plain, f.markerKey(1), FsckClass::LegacyMarker), 1u);
    for (const uint64_t b : {1, 2, 3})
        EXPECT_TRUE(objectPresent(*f.backend, f.markerKey(b))) << "a scan without the fix deletes nothing";

    const FsckReport fixed = f.fsck(/*detail=*/false, /*fix_legacy_markers=*/ true);
    EXPECT_EQ(fixed.legacy_marker, 3u);
    EXPECT_EQ(fixed.legacy_markers_deleted, 3u);
    for (const uint64_t b : {1, 2, 3})
        EXPECT_FALSE(objectPresent(*f.backend, f.markerKey(b))) << b;
    EXPECT_TRUE(objectPresent(*f.backend, f.bodyKey(1)));
    EXPECT_TRUE(objectPresent(*f.backend, foreign)) << "a key that is not a marker's is left alone";
    EXPECT_NE(formatFsckSummary(fixed).find("legacy_markers_deleted=3"), String::npos) << formatFsckSummary(fixed);

    const FsckReport again = f.fsck(/*detail=*/false, /*fix_legacy_markers=*/ true);
    EXPECT_EQ(again.legacy_marker, 0u);
    EXPECT_EQ(again.legacy_markers_deleted, 0u);
}

/// A fix run has no mount, no lease and no probe: on the read-only open, as `cas-fsck` opens the pool, it
/// writes nothing, and the store differs afterwards only by the markers it removed.
TEST(CASFsck, FixRunWritesNothingAndMountsNothing)
{
    FsckIntentFixture f;
    f.body(1);
    f.currentIntent(1);
    f.marker(1);
    f.marker(2);
    const std::vector<String> before = f.allKeys();
    f.backend->clearJournal();

    PoolConfig config{.pool_prefix = "p", .server_root_id = "test"};
    config.read_only = true;
    const PoolPtr read_only_store = Pool::open(f.backend, config);
    ASSERT_TRUE(read_only_store->poolConfig().read_only) << "fixture: the pool is open read-only";
    const FsckReport rep = runFsck(*read_only_store, false, {}, {}, false, {}, /*fix_legacy_markers=*/ true);

    EXPECT_EQ(rep.legacy_markers_deleted, 2u) << "positive control: the fix ran";
    EXPECT_FALSE(f.backend->journal().empty());
    for (const JournalEntry & entry : f.backend->journal())
    {
        EXPECT_NE(entry.verb, JournalVerb::Write) << "a fix run wrote " << (entry.keys.empty() ? String{} : entry.keys.front());
        EXPECT_NE(entry.verb, JournalVerb::Publish);
        EXPECT_NE(entry.verb, JournalVerb::RemoveMany);
    }
    std::vector<String> expected;
    for (const String & key : before)
        if (!key.ends_with(".meta"))
            expected.push_back(key);
    EXPECT_EQ(f.allKeys(), expected);
}

/// The fix loop honours the scan deadline: an expired deadline stops it before the first delete, and the
/// markers are untouched.
TEST(CASFsck, FixLoopHonoursTheDeadline)
{
    FsckIntentFixture f;
    f.marker(1);
    f.marker(2);
    const std::vector<String> keys{f.markerKey(1), f.markerKey(2)};
    uint64_t deleted = 0;

    expectThrowsCodeWithMessage(DB::ErrorCodes::TIMEOUT_EXCEEDED, "0 legacy marker(s) deleted before", [&]
    {
        (void)removeLegacyMarkers(*f.store, keys, 1, std::chrono::steady_clock::now() - std::chrono::seconds(1),
                                  [&](uint64_t n) { deleted = n; });
    });
    EXPECT_EQ(deleted, 0u);
    EXPECT_TRUE(objectPresent(*f.backend, f.markerKey(1)));
    EXPECT_TRUE(objectPresent(*f.backend, f.markerKey(2)));

    EXPECT_EQ(removeLegacyMarkers(*f.store, keys, 1, std::chrono::steady_clock::now() + std::chrono::hours(1),
                                  [&](uint64_t n) { deleted = n; }).removed, 2u) << "positive control: with time left the fix deletes";
    EXPECT_EQ(deleted, 2u) << "the callback reports the running count";
}

/// The deadline is checked before every request, not once per batch: a deadline that passes during one marker's `HEAD`
/// stops the fix before that marker's delete, and the count of markers already deleted is reported.
TEST(CASFsck, FixLoopChecksTheDeadlineBeforeEachRequest)
{
    for (const size_t slow : {size_t{0}, size_t{1}})
    {
        SCOPED_TRACE(slow);
        FsckIntentFixture f;
        f.marker(1);
        f.marker(2);
        const std::vector<String> keys{f.markerKey(1), f.markerKey(2)};
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(1);
        /// The deadline is a point on the real steady clock, so the slow `HEAD` waits for that clock to pass it.
        f.backend->beforeHead(keys[slow], [deadline]
        {
            while (std::chrono::steady_clock::now() <= deadline)
            {
            }
        });
        f.backend->clearJournal();
        uint64_t reported = 99;

        expectThrowsCodeWithMessage(DB::ErrorCodes::TIMEOUT_EXCEEDED, fmt::format("{} legacy marker(s) deleted before", slow), [&]
        {
            (void)removeLegacyMarkers(*f.store, keys, 1, deadline, [&](uint64_t n) { reported = n; });
        });
        const auto journal = f.backend->journal();
        EXPECT_EQ(countKeyRequests(journal, JournalVerb::Head, keys[slow]), 1u) << "positive control: the slow HEAD was sent";
        EXPECT_EQ(countKeyRequests(journal, JournalVerb::Remove, keys[slow]), 0u) << "deleted after the deadline";
        EXPECT_EQ(countKeyRequests(journal, JournalVerb::Head, keys[1]) + countKeyRequests(journal, JournalVerb::Head, keys[0]),
                  slow + 1) << "a request was sent after the deadline";
        EXPECT_EQ(reported, slow) << "the partial count reaches the caller";
        EXPECT_EQ(objectPresent(*f.backend, keys[0]), slow == 0);
        EXPECT_TRUE(objectPresent(*f.backend, keys[1]));
    }
}

/// Both long loops report progress every thousand keys, so a slow scan is visibly working.
TEST(CASFsck, IntentAndMarkerLoopsReportProgress)
{
    FsckIntentFixture f;
    for (uint64_t b = 1; b <= 1000; ++b)
    {
        f.staleIntent(b);
        f.marker(b);
    }
    std::map<String, std::vector<uint64_t>> seen;
    const FsckProgress on_progress = [&](std::string_view phase, uint64_t objects, uint64_t)
    {
        seen[String(phase)].push_back(objects);
    };

    const FsckReport rep = runFsck(*f.store, false, on_progress, {}, false, {}, /*fix_legacy_markers=*/ true);

    ASSERT_EQ(rep.intent_listed, 1000u) << "positive control: the loops had keys to walk";
    ASSERT_EQ(rep.legacy_markers_deleted, 1000u);
    EXPECT_EQ(seen["counting blob intents"], std::vector<uint64_t>{1000});
    EXPECT_EQ(seen["removing legacy markers"], std::vector<uint64_t>{1000});
}

/// A scoped scan lists no markers, so a fix over it would silently do nothing: it is refused.
TEST(CASFsck, FixNeedsAWholePoolScan)
{
    FsckIntentFixture f;
    f.marker(1);
    expectThrowsCode(DB::ErrorCodes::BAD_ARGUMENTS, [&]
    {
        (void)runFsck(*f.store, false, {}, {}, false, "00", /*fix_legacy_markers=*/ true);
    });
    EXPECT_TRUE(objectPresent(*f.backend, f.markerKey(1)));
}

/// Intents are objects of the pool: each listed key lands in exactly one intent class, and its bytes are in
/// `physical_bytes`. They are not blobs, so `unreachable` does not count them.
TEST(CASFsck, IntentsAreAccountedInTheTotals)
{
    FsckIntentFixture f;
    f.body(1);
    f.body(2);
    f.currentIntent(1);
    f.staleIntent(2);
    f.staleIntent(3);
    f.create(f.layout.blobIntentKey(blobRefOf(4), FsckIntentFixture::staleIdentity(4)), "junk");

    uint64_t listed = 0;
    uint64_t intent_bytes = 0;
    uint64_t body_bytes = 0;
    {
        OperationForTest op(*f.backend);
        (*op).forEachListedKey(f.layout.blobIntentsPrefix(), [&](const ListedKey & k) { ++listed; intent_bytes += k.size; return true; },
                               Retry::standard(), CasOperation::kCasListPageKeys);
        (*op).forEachListedKey(f.layout.blobsPrefix(), [&](const ListedKey & k) { body_bytes += k.size; return true; },
                               Retry::standard(), CasOperation::kCasListPageKeys);
    }
    ASSERT_EQ(listed, 4u);

    const FsckReport rep = f.fsck(/*detail=*/false, /*fix_legacy_markers=*/ false, /*verify=*/true);

    EXPECT_EQ(rep.intent_current + rep.intent_replaced + rep.intent_body_gone + rep.intent_malformed + rep.intent_unread, listed);
    EXPECT_EQ(rep.physical_bytes, body_bytes + intent_bytes);
    EXPECT_EQ(rep.unreachable, 2u) << "the two bodies, not the intents";

    const FsckReport plain = f.fsck(/*detail=*/false);
    EXPECT_EQ(plain.intent_listed + plain.intent_malformed, listed) << "a plain scan accounts for every listed intent too";
    EXPECT_EQ(plain.physical_bytes, rep.physical_bytes);
}

namespace
{
/// A pool whose GC rounds make the rows, so the report is read over what the protocol itself writes.
struct FsckGcFixture
{
    std::shared_ptr<FsckFaultBackend> backend = std::make_shared<FsckFaultBackend>();
    PoolPtr store;
    std::unique_ptr<Gc> gc;
    RootNamespace ns{"00/aa@cas@"};
    ManifestRef manifest = ref(1, 0xA1);

    explicit FsckGcFixture(uint64_t create_budget = 0, std::initializer_list<uint64_t> blobs = {1}, uint64_t gc_shards = 1)
    {
        PoolConfig config{.pool_prefix = "p", .server_root_id = "test"};
        config.gc_round_intent_create_budget = create_budget;
        config.gc_shards = gc_shards;
        store = Pool::open(backend, config);
        std::vector<ManifestEntry> entries;
        for (const uint64_t b : blobs)
        {
            writeBlobBody(*backend, store->layout(), numberedBlobHash(b));
            entries.push_back(blobEntryFor("f" + std::to_string(b), numberedBlobHash(b)));
        }
        writeManifestRaw(*backend, store->layout(), ns, manifest, entries);
        const uint64_t publish_sequence = publishCommittedTransition(*backend, store->layout(), ns, "tbl", std::nullopt, manifest);
        writeFsckCheckpoint(*backend, store->layout(), ns, RefTxnId{1, publish_sequence});
        gc = std::make_unique<Gc>(store, hexToU128("00000000000000000000000000000001"));
        gc->runRegularRound();
    }

    void drop()
    {
        const uint64_t drop_sequence = dropRefTransition(*backend, store->layout(), ns, "tbl", manifest);
        writeFsckCheckpoint(*backend, store->layout(), ns, RefTxnId{1, drop_sequence});
    }

    void round() { gc->runRegularRound(); }

    String bodyKey(uint64_t b) const { return store->layout().blobKey(blobRefOf(b)); }

    /// The key of the intent the round wrote for the body now at blob `b`.
    String intentKey(uint64_t b) const
    {
        return *intentKeyOfCurrentBody(*backend, store->layout(), blobRefOf(b));
    }

    /// Removes the intent with the raw backend, as nothing in the protocol does while its row is `pending`.
    void removeBody(uint64_t b) { removeFsckFixtureKey(*backend, bodyKey(b)); }

    void removeIntent(uint64_t b) { removeFsckFixtureKey(*backend, intentKey(b), "fixture: the round wrote the intent of "); }
};

bool anyNoteContains(const FsckReport & report, FsckClass cls, std::string_view text)
{
    for (const FsckObject & o : report.objects)
        if (o.cls == cls)
            for (const String & note : o.reachable_from)
                if (note.find(text) != String::npos)
                    return true;
    return false;
}
}

/// What a healthy pool holds mid-reclaim: a `pending` row with its intent over a present body, then a
/// `standing` row with its intent over an absent body. All of it is normal: the report is clean and the counts
/// are informational.
TEST(CASFsck, HealthyPoolWithRowsAndIntentsOverAbsentBodiesIsClean)
{
    FsckGcFixture f;
    f.drop();
    f.round();   /// zero => condemned, intent created, row pending; body present

    const FsckReport pending = runFsck(*f.store, /*detail=*/true, {}, {}, false, {}, /*fix_legacy_markers=*/ false, /*verify_intents=*/true);
    EXPECT_TRUE(pending.clean());
    EXPECT_EQ(pending.pending_gc, 1u);
    EXPECT_EQ(pending.pending_without_intent, 0u);
    EXPECT_EQ(pending.intent_current, 1u);
    EXPECT_EQ(pending.intent_body_gone, 0u);

    f.round();   /// the body is deleted; the row is standing and the intent stays
    const FsckReport standing = runFsck(*f.store, /*detail=*/true, {}, {}, false, {}, /*fix_legacy_markers=*/ false, /*verify_intents=*/true);
    EXPECT_TRUE(standing.clean());
    EXPECT_EQ(standing.pending_gc, 0u) << "the body is gone";
    EXPECT_EQ(standing.intent_body_gone, 1u) << "positive control: the intent outlives its body";
    EXPECT_EQ(standing.dangling, 0u);
    EXPECT_EQ(standing.unaccounted, 0u);
}

/// A `candidate` row has no confirmed intent yet. Its note says so, and no note promises an acknowledgement
/// by writers, which nothing in the intent protocol waits for.
TEST(CASFsck, CandidateAndPendingRowsGetTheirOwnNotes)
{
    FsckGcFixture f(/*create_budget=*/1, {1, 2});
    f.drop();
    f.round();   /// two blobs reach zero, one create fits the budget

    const FsckReport rep = runFsck(*f.store, /*detail=*/true);

    EXPECT_TRUE(rep.clean());
    EXPECT_EQ(rep.pending_gc, 2u);
    EXPECT_TRUE(anyNoteContains(rep, FsckClass::PendingGc, "delete_pending"));
    EXPECT_TRUE(anyNoteContains(rep, FsckClass::PendingGc, "candidate"));
    EXPECT_FALSE(anyNoteContains(rep, FsckClass::PendingGc, "graduates"));
    EXPECT_FALSE(anyNoteContains(rep, FsckClass::PendingGc, "writer"));
}

/// A `standing` row says the body of this incarnation is deleted. The same token seen on a body again is
/// reported as its own advisory class, not as a pending delete: GC records it and deletes nothing.
TEST(CASFsck, StandingRowKeepsItsBitInTheReport)
{
    FsckGcFixture f;
    const String body_key = f.store->layout().blobKey(blobRefOf(1));
    String token;
    {
        OperationForTest op(*f.backend);
        token = PersistedEtag::capture((*op).head(body_key, Retry::standard())->etag).value;
    }
    f.drop();
    f.round();
    f.round();   /// standing; the body is deleted
    ASSERT_FALSE(objectPresent(*f.backend, body_key));
    writeBlobBody(*f.backend, f.store->layout(), numberedBlobHash(1));
    f.backend->head_values[body_key] = token;

    f.backend->clearJournal();

    const FsckReport rep = runFsck(*f.store, /*detail=*/true);

    EXPECT_EQ(rep.standing_present, 1u);
    EXPECT_EQ(rep.pending_gc, 0u) << "a standing row is not a pending delete";
    EXPECT_EQ(rep.pending_without_intent, 0u);
    EXPECT_TRUE(anyNoteContains(rep, FsckClass::StandingPresent, "standing"));
    EXPECT_NE(formatFsckSummary(rep).find("standing_present=1"), String::npos) << formatFsckSummary(rep);
    EXPECT_TRUE(rep.clean()) << "advisory: the intent is present, so a writer publishes fresh";
    EXPECT_EQ(countKeyRequests(f.backend->journal(), JournalVerb::Read, f.intentKey(1)), 0u) << "a standing row's intent is not read";
}

/// A `pending` row is the one state GC deletes from without re-reading the intent, so a plain scan reads the
/// intent of each: one `GET` per row, and the row stays an expected backlog when the intent is there.
TEST(CASFsck, PendingRowWithItsIntentIsExpectedAndCostsOneIntentRead)
{
    FsckGcFixture f;
    f.drop();
    f.round();
    ASSERT_TRUE(objectPresent(*f.backend, f.intentKey(1))) << "fixture: the round wrote the intent";
    ASSERT_TRUE(objectPresent(*f.backend, f.bodyKey(1)));
    f.backend->clearJournal();

    const FsckReport rep = runFsck(*f.store, /*detail=*/true);

    EXPECT_EQ(rep.pending_gc, 1u);
    EXPECT_EQ(rep.pending_without_intent + rep.pending_intent_absent + rep.pending_token_rejected, 0u);
    EXPECT_TRUE(rep.clean());
    EXPECT_TRUE(anyNoteContains(rep, FsckClass::PendingGc, "intent is present"));
    EXPECT_EQ(countKeyRequests(f.backend->journal(), JournalVerb::Read, f.intentKey(1)), 1u);
    EXPECT_EQ(countKeyRequests(f.backend->journal(), JournalVerb::Read, f.store->layout().gcStateKey()), 1u)
        << "no re-read of gc/state without a suspect";
}

/// A `pending` row whose intent is absent and whose body was gone or replaced before the scan started is classified
/// from the row: the body listing never shows it.
TEST(CASFsck, PendingRowWithoutIntentOverABodyGoneBeforeTheScanIsInformational)
{
    for (const bool replaced : {false, true})
    {
        SCOPED_TRACE(replaced ? "replaced" : "gone");
        FsckGcFixture f;
        f.drop();
        f.round();
        const String intent = f.intentKey(1);
        f.removeIntent(1);
        f.removeBody(1);
        if (replaced)
            writeBlobBody(*f.backend, f.store->layout(), numberedBlobHash(1));
        f.backend->clearJournal();

        const FsckReport rep = runFsck(*f.store, /*detail=*/true);

        EXPECT_EQ(rep.pending_intent_absent, 1u) << formatFsckSummary(rep);
        EXPECT_EQ(rep.pending_without_intent, 0u);
        EXPECT_TRUE(rep.clean()) << formatFsckSummary(rep);
        EXPECT_EQ(countVerdictsAtKey(rep, f.bodyKey(1), FsckClass::PendingIntentAbsent), 1u);
        EXPECT_EQ(countKeyRequests(f.backend->journal(), JournalVerb::Read, intent), 1u)
            << "positive control: the row's intent was read";
    }
}

/// The hard case: a `pending` row, its intent gone, the body present with the row's token. GC would delete
/// this body without warning any writer.
TEST(CASFsck, PendingRowWithoutItsIntentOverItsBodyIsHard)
{
    FsckGcFixture f;
    f.drop();
    f.round();
    f.removeIntent(1);
    ASSERT_TRUE(objectPresent(*f.backend, f.bodyKey(1)));
    f.backend->clearJournal();
    /// The re-read is not a way out: the row is still `pending` for the same token, so the finding stands.
    bool reread_seen = false;
    f.backend->beforeRead(f.store->layout().gcStateKey(), 1, [&] { reread_seen = true; });

    const FsckReport rep = runFsck(*f.store, /*detail=*/false);

    EXPECT_TRUE(reread_seen) << "positive control: the state was read a second time";
    EXPECT_EQ(rep.pending_without_intent, 1u);
    EXPECT_EQ(rep.pending_gc, 0u);
    EXPECT_FALSE(rep.clean());
    EXPECT_NE(formatFsckSummary(rep).find("pending_without_intent=1"), String::npos) << formatFsckSummary(rep);
    EXPECT_EQ(countVerdictsAtKey(rep, f.bodyKey(1), FsckClass::PendingWithoutIntent), 1u) << "the row is listed even without `detail`";
    EXPECT_TRUE(anyNoteContains(rep, FsckClass::PendingWithoutIntent, "without warning"));
    const auto journal = f.backend->journal();
    EXPECT_EQ(countKeyRequests(journal, JournalVerb::Read, f.intentKey(1)), 1u);
    EXPECT_EQ(countKeyRequests(journal, JournalVerb::Read, f.store->layout().gcStateKey()), 2u) << "the state is re-read once before the verdict";
    EXPECT_TRUE(mutatedKeys(journal).empty()) << "fsck repairs nothing: it does not recreate the intent";
    EXPECT_FALSE(objectPresent(*f.backend, f.intentKey(1)));
}

/// Intent absent and the body gone by the time it is looked at: the row is about to become `standing`.
/// Nothing is at risk, so the report stays clean.
TEST(CASFsck, PendingRowWithoutIntentOverAnAbsentBodyIsInformational)
{
    FsckGcFixture f;
    f.drop();
    f.round();
    f.removeIntent(1);
    const String intent = f.intentKey(1);
    f.backend->beforeRead(intent, 0, [&] { f.removeBody(1); });

    const FsckReport rep = runFsck(*f.store, /*detail=*/true);

    EXPECT_EQ(rep.pending_intent_absent, 1u);
    EXPECT_EQ(rep.pending_without_intent, 0u);
    EXPECT_TRUE(rep.clean());
    EXPECT_EQ(countVerdictsAtKey(rep, f.bodyKey(1), FsckClass::PendingIntentAbsent), 1u);
}

/// Intent absent and another incarnation at the body's key: the row names a body that is gone.
TEST(CASFsck, PendingRowWithoutIntentOverAnotherTokenIsInformational)
{
    FsckGcFixture f;
    f.drop();
    f.round();
    f.removeIntent(1);
    const String intent = f.intentKey(1);
    String before;
    {
        OperationForTest op(*f.backend);
        before = (*op).head(f.bodyKey(1), Retry::standard())->etag.render();
    }
    f.backend->beforeRead(intent, 0, [&] { f.removeBody(1); writeBlobBody(*f.backend, f.store->layout(), numberedBlobHash(1)); });

    const FsckReport rep = runFsck(*f.store, /*detail=*/true);

    {
        OperationForTest op(*f.backend);
        ASSERT_NE((*op).head(f.bodyKey(1), Retry::standard())->etag.render(), before) << "fixture: another incarnation is at the key";
    }
    EXPECT_EQ(rep.pending_intent_absent, 1u);
    EXPECT_EQ(rep.pending_without_intent, 0u);
    EXPECT_TRUE(rep.clean());
}

/// A round may commit between the scan's read of `gc/state` and the verdict. The verdict is taken after one
/// more read: a row that moved on is no finding.
TEST(CASFsck, PendingRowThatMovesBeforeTheRereadIsNoFinding)
{
    FsckGcFixture f;
    f.drop();
    f.round();
    f.removeIntent(1);
    f.backend->beforeRead(f.store->layout().gcStateKey(), 1, [&]
    {
        f.round();   /// the body is deleted and the row becomes `standing`
    });

    const FsckReport rep = runFsck(*f.store, /*detail=*/true);

    ASSERT_FALSE(objectPresent(*f.backend, f.bodyKey(1))) << "positive control: the round ran between the scan and the re-read";
    EXPECT_EQ(rep.pending_without_intent, 0u);
    EXPECT_EQ(rep.pending_intent_absent, 1u);
    EXPECT_TRUE(rep.clean());
}

/// Rows live in every shard of the snapshot: the check reads them all.
TEST(CASFsck, PendingRowsWithoutIntentsAreFoundInEveryGcShard)
{
    FsckGcFixture f(/*create_budget=*/0, {1, 2, 3, 4, 5, 6, 7, 8}, /*gc_shards=*/4);
    f.drop();
    f.round();
    uint64_t rows_outside_shard_zero = 0;
    uint64_t rows = 0;
    for (uint64_t shard = 0; shard < 4; ++shard)
        for (const RetiredEntry & row : currentRetiredSet(*f.backend, f.store->layout(), shard))
        {
            ASSERT_TRUE(row.delete_pending && !row.standing) << "fixture: every row is pending";
            ++rows;
            rows_outside_shard_zero += shard != 0;
        }
    ASSERT_EQ(rows, 8u);
    ASSERT_GT(rows_outside_shard_zero, 0u) << "fixture: some row sits in a shard other than zero";
    for (const uint64_t b : {1, 2, 3, 4, 5, 6, 7, 8})
        f.removeIntent(b);

    const FsckReport rep = runFsck(*f.store, /*detail=*/false);

    EXPECT_EQ(rep.pending_without_intent, 8u);
    EXPECT_FALSE(rep.clean());
}

/// The deadline is checked before each request for a present `pending` body: one that passes during the first row's
/// `HEAD` stops the scan before that row's intent read and before any other row's requests.
TEST(CASFsck, PresentPendingBodiesStopAtTheDeadline)
{
    const std::vector<uint64_t> blobs{1, 2, 3, 4};
    FsckGcFixture f(/*create_budget=*/0, {1, 2, 3, 4});
    f.drop();
    f.round();
    std::vector<String> intent_keys;
    for (const uint64_t b : blobs)
    {
        ASSERT_TRUE(objectPresent(*f.backend, f.bodyKey(b))) << "fixture: the body is present";
        intent_keys.push_back(f.intentKey(b));
    }
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(1);
    /// The deadline is a point on the real steady clock, so the first body `HEAD` waits for that clock to pass it.
    for (const uint64_t b : blobs)
        f.backend->beforeHead(f.bodyKey(b), [deadline]
        {
            while (std::chrono::steady_clock::now() <= deadline)
            {
            }
        });
    f.backend->clearJournal();

    expectThrowsCodeWithMessage(DB::ErrorCodes::TIMEOUT_EXCEEDED, "during 'checking pending rows'", [&]
    {
        (void)runFsck(*f.store, /*detail=*/false, {}, deadline);
    });
    const auto journal = f.backend->journal();
    size_t body_heads = 0;
    size_t intent_reads = 0;
    for (const uint64_t b : blobs)
        body_heads += countKeyRequests(journal, JournalVerb::Head, f.bodyKey(b));
    for (const String & key : intent_keys)
        intent_reads += countKeyRequests(journal, JournalVerb::Read, key);
    EXPECT_EQ(body_heads, 1u) << "a body HEAD was sent after the deadline";
    EXPECT_EQ(intent_reads, 0u) << "an intent was read after the deadline";
}

namespace
{

void spinPast(std::chrono::steady_clock::time_point deadline)
{
    while (std::chrono::steady_clock::now() <= deadline)
    {
    }
}

size_t bodyHeads(FsckGcFixture & f, const std::vector<uint64_t> & blobs)
{
    size_t heads = 0;
    for (const uint64_t b : blobs)
        heads += countKeyRequests(f.backend->journal(), JournalVerb::Head, f.bodyKey(b));
    return heads;
}

}

/// A deadline that passes while one row is classified stops the scan before the next row's retired-body `HEAD`.
TEST(CASFsck, DeadlinePassingDuringARowStopsBeforeTheNextRowsHead)
{
    const std::vector<uint64_t> blobs{1, 2};
    FsckGcFixture f(/*create_budget=*/0, {1, 2});
    f.drop();
    f.round();
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(1);
    for (const uint64_t b : blobs)
        f.backend->beforeRead(f.intentKey(b), 0, [deadline] { spinPast(deadline); });
    f.backend->clearJournal();

    expectThrowsCodeWithMessage(DB::ErrorCodes::TIMEOUT_EXCEEDED, "during 'classifying unreferenced bodies'", [&]
    {
        (void)runFsck(*f.store, /*detail=*/false, {}, deadline);
    });
    EXPECT_EQ(bodyHeads(f, blobs), 1u) << "the second row's body HEAD was sent after the deadline";
}

/// A deadline that passes during the intent read stops the scan before the body `HEAD` that follows an absent intent.
TEST(CASFsck, DeadlinePassingDuringAnAbsentIntentReadStopsBeforeTheBodyHead)
{
    FsckGcFixture f(/*create_budget=*/0, {1});
    f.drop();
    f.round();
    const String intent_key = f.intentKey(1);
    f.removeIntent(1);
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(1);
    f.backend->beforeRead(intent_key, 0, [deadline] { spinPast(deadline); });
    f.backend->clearJournal();

    expectThrowsCodeWithMessage(DB::ErrorCodes::TIMEOUT_EXCEEDED, "during 'checking pending rows'", [&]
    {
        (void)runFsck(*f.store, /*detail=*/false, {}, deadline);
    });
    EXPECT_EQ(bodyHeads(f, {1}), 1u) << "only the retired-body HEAD precedes the deadline; the pending check sent another";
}

/// With intent verification, a deadline that passes during an intent's read stops the scan before that intent's body
/// `HEAD`.
TEST(CASFsck, DeadlinePassingDuringAVerifiedIntentReadStopsBeforeTheBodyHead)
{
    FsckIntentFixture f;
    f.body(1);
    const String intent = f.currentIntent(1);
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(1);
    f.backend->beforeRead(intent, 0, [deadline] { spinPast(deadline); });
    f.backend->clearJournal();

    expectThrowsCodeWithMessage(DB::ErrorCodes::TIMEOUT_EXCEEDED, "during 'reading blob intents'", [&]
    {
        (void)runFsck(*f.store, /*detail=*/false, {}, deadline, /*partial_on_deadline=*/false, {},
                      /*fix_legacy_markers=*/false, /*verify_intents=*/true);
    });
    const auto journal = f.backend->journal();
    EXPECT_EQ(countKeyRequests(journal, JournalVerb::Read, intent), 1u) << "positive control: the intent was read";
    EXPECT_EQ(countKeyRequests(journal, JournalVerb::Head, f.bodyKey(1)), 0u) << "the body HEAD was sent after the deadline";
}

namespace
{

String adoptedSealKeyOf(FsckGcFixture & f)
{
    OperationForTest op(*f.backend);
    const GcState state = decodeGcState((*op).read(f.store->layout().gcStateKey(), Retry::standard())->bytes);
    return f.store->layout().foldSealKey(state.snap_generation, state.snap_attempt);
}

}

/// `gc/state` names a fold seal that is gone, and `gc/state` has not moved: the GC snapshot is unread. That is a hard
/// finding, not a pool with no `pending` rows.
TEST(CASFsck, AdoptedSealMissingUnderAnUnchangedStateIsAnUnreadSnapshot)
{
    FsckGcFixture f;
    f.drop();
    f.round();
    f.removeIntent(1);
    ASSERT_TRUE(objectPresent(*f.backend, f.bodyKey(1))) << "fixture: a pending row without its intent over its body";
    const String seal_key = adoptedSealKeyOf(f);
    removeFsckFixtureKey(*f.backend, seal_key);

    const FsckReport rep = runFsck(*f.store, /*detail=*/false);

    EXPECT_EQ(rep.intent_unread, 1u) << formatFsckSummary(rep);
    EXPECT_NE(rep.intent_unread_reason.find(seal_key), String::npos) << rep.intent_unread_reason;
    EXPECT_FALSE(rep.clean()) << formatFsckSummary(rep);
}

/// A round prunes the fold seal of the `gc/state` the scan read before the scan reads the seal: the scan reads the
/// snapshot of the `gc/state` that moved.
TEST(CASFsck, AdoptedSealPrunedUnderAMovedStateIsReadFromTheNewState)
{
    FsckGcFixture f;
    f.drop();
    f.round();
    const String old_seal = adoptedSealKeyOf(f);
    String new_seal;
    f.backend->beforeRead(old_seal, 0, [&]
    {
        f.round();
        new_seal = adoptedSealKeyOf(f);
        removeFsckFixtureKey(*f.backend, old_seal);
    });
    f.backend->clearJournal();

    const FsckReport rep = runFsck(*f.store, /*detail=*/false);

    ASSERT_FALSE(new_seal.empty()) << "the scan never read the seal";
    ASSERT_NE(new_seal, old_seal) << "positive control: the round moved gc/state";
    EXPECT_EQ(rep.intent_unread, 0u) << rep.intent_unread_reason;
    EXPECT_GE(countKeyRequests(f.backend->journal(), JournalVerb::Read, new_seal), 1u) << "the scan did not read the new snapshot";
    EXPECT_TRUE(rep.clean()) << formatFsckSummary(rep);
}

/// The capability probe's scratch object under `blob-intents/_probe/` is not an intent and no finding.
TEST(CASFsck, ProbeScratchObjectIsNoIntent)
{
    FsckIntentFixture f;
    f.body(1);
    f.currentIntent(1);
    f.create(f.layout.blobIntentsProbePrefix() + "0123456789abcdef0123456789abcdef/intent", blobIntentRecord());

    for (const bool verify : {false, true})
    {
        const FsckReport rep = f.fsck(/*detail=*/true, /*fix_legacy_markers=*/ false, verify);
        EXPECT_EQ(rep.intent_malformed, 0u) << formatFsckSummary(rep);
        EXPECT_EQ(rep.intent_listed, 1u) << "positive control: the real intent was listed";
        EXPECT_TRUE(rep.clean()) << formatFsckSummary(rep);
    }
}

/// A verifying scan counts an intent as listed only once its read found it: a key that vanished between the
/// listing and the read is not an object any more.
TEST(CASFsck, VerifiedIntentIsListedOnlyAfterItsRead)
{
    FsckIntentFixture f;
    f.body(1);
    f.body(2);
    const String vanishing = f.currentIntent(1);
    f.currentIntent(2);
    f.backend->beforeRead(vanishing, 0, [&] { f.removeKey(vanishing); });

    const FsckReport rep = f.fsck(/*detail=*/true, /*fix_legacy_markers=*/ false, /*verify=*/true);

    EXPECT_EQ(rep.intent_listed, 1u) << "only the intent that was read is counted";
    EXPECT_EQ(rep.intent_current, 1u) << "positive control: the other intent was read and classified";
}

/// A writer adopts the body of a `pending` row whose intent is gone: the body is referenced again, and GC would
/// still delete it. The row is checked whether or not its body is referenced; a referenced body stays out of the
/// backlog counts.
TEST(CASFsck, PendingRowOverAReferencedBodyIsCheckedToo)
{
    for (const bool remove_intent : {false, true})
    {
        SCOPED_TRACE(remove_intent);
        FsckGcFixture f;
        f.drop();
        f.round();
        const String intent = f.intentKey(1);
        if (remove_intent)
            f.removeIntent(1);
        const ManifestRef adopter = ref(2, 0xA2);
        writeManifestRaw(*f.backend, f.store->layout(), f.ns, adopter, {blobEntryFor("f1", numberedBlobHash(1))});
        const uint64_t sequence = publishCommittedTransition(*f.backend, f.store->layout(), f.ns, "adopter", std::nullopt, adopter);
        writeFsckCheckpoint(*f.backend, f.store->layout(), f.ns, RefTxnId{1, sequence});
        f.backend->clearJournal();

        const FsckReport rep = runFsck(*f.store, /*detail=*/false);

        EXPECT_EQ(rep.reachable, 1u) << "positive control: the body is referenced again";
        EXPECT_EQ(countKeyRequests(f.backend->journal(), JournalVerb::Read, intent), 1u) << "the row's intent was read";
        EXPECT_EQ(rep.pending_gc + rep.pending_intent_absent + rep.unreachable, 0u) << "a referenced body is not backlog";
        EXPECT_EQ(rep.pending_without_intent, remove_intent ? 1u : 0u);
        EXPECT_EQ(rep.clean(), !remove_intent);
    }
}

/// The row keeps the token as captured, quoted; the store answers the same tag bare. GC compares canonically and
/// would delete that body, so fsck checks the row's intent the same way.
TEST(CASFsck, PendingRowTokenIsComparedCanonically)
{
    auto backend = std::make_shared<FsckFaultBackend>();
    PoolPtr store = openPoolForTest(backend);
    const BlobRef blob = blobRefOf(1);
    writeBlobBody(*backend, store->layout(), numberedBlobHash(1));
    const String body_key = store->layout().blobKey(blob);
    backend->head_values[body_key] = "tag-1";
    injectRetire(*backend, store->layout(), /*round=*/3, /*shard=*/0, {RetiredEntry{
        .kind = ObjectKind::Blob, .ref = blob, .token = PersistedEtag{"emulated", "\"tag-1\""},
        .size = 1, .condemn_round = 3, .delete_pending = true}});

    const FsckReport rep = runFsck(*store, /*detail=*/true);

    EXPECT_EQ(rep.pending_without_intent, 1u) << formatFsckSummary(rep);
    EXPECT_FALSE(rep.clean());
    EXPECT_EQ(countVerdictsAtKey(rep, body_key, FsckClass::PendingWithoutIntent), 1u);
}

/// The second read of `gc/state` clears a suspect only when it is a complete snapshot: a seal it names that is
/// gone, or a run whose checksum disagrees, proves nothing about the row, so the scan is inconclusive.
TEST(CASFsck, PendingSuspectIsClearedOnlyByACompleteReread)
{
    enum class Damage : uint8_t { None, SealMissing, RunChecksum };
    for (const Damage damage : {Damage::None, Damage::SealMissing, Damage::RunChecksum})
    {
        SCOPED_TRACE(static_cast<int>(damage));
        FsckGcFixture f;
        f.drop();
        f.round();
        f.removeIntent(1);
        const Layout & layout = f.store->layout();
        f.backend->beforeRead(layout.gcStateKey(), 1, [&]
        {
            f.round();   /// the row moves on: the body is deleted and the row becomes `standing`
            OperationForTest op(*f.backend);
            const GcState state = decodeGcState((*op).read(layout.gcStateKey(), Retry::standard())->bytes);
            const String seal_key = layout.foldSealKey(state.snap_generation, state.snap_attempt);
            const auto seal = (*op).read(seal_key, Retry::standard());
            ASSERT_TRUE(seal.has_value());
            if (damage == Damage::SealMissing)
                ASSERT_EQ((*op).remove(seal_key, seal->etag, Retry::standard()), Removal::Removed);
            if (damage == Damage::RunChecksum)
            {
                CasFoldSeal decoded = decodeFoldSeal(seal->bytes, state.snap_generation);
                ASSERT_FALSE(decoded.blob_target_runs.empty());
                decoded.blob_target_runs.front().checksum += 1;
                ASSERT_TRUE(std::holds_alternative<Committed>((*op).replace(seal_key, encodeFoldSeal(decoded), seal->etag, Retry::standard())));
            }
        });

        const FsckReport rep = runFsck(*f.store, /*detail=*/true);

        ASSERT_FALSE(objectPresent(*f.backend, f.bodyKey(1))) << "positive control: the round ran before the re-read";
        EXPECT_EQ(rep.pending_without_intent, 0u);
        if (damage == Damage::None)
        {
            EXPECT_EQ(rep.pending_intent_absent, 1u);
            EXPECT_TRUE(rep.clean()) << formatFsckSummary(rep);
        }
        else
        {
            EXPECT_EQ(rep.pending_intent_absent, 0u);
            EXPECT_FALSE(rep.clean()) << formatFsckSummary(rep);
            EXPECT_NE(rep.intent_unread_reason.find("inconclusive"), String::npos) << rep.intent_unread_reason;
        }
    }
}

/// A `pending` row whose token has no canonical identity names no intent key: its own informational class.
TEST(CASFsck, PendingRowWithAnUncanonicalTokenIsItsOwnClass)
{
    auto backend = std::make_shared<FsckFaultBackend>();
    PoolPtr store = openPoolForTest(backend);
    const BlobRef blob = blobRefOf(1);
    writeBlobBody(*backend, store->layout(), numberedBlobHash(1));
    const String body_key = store->layout().blobKey(blob);
    backend->head_values[body_key] = "W/\"weak\"";
    injectRetire(*backend, store->layout(), /*round=*/3, /*shard=*/0, {RetiredEntry{
        .kind = ObjectKind::Blob, .ref = blob, .token = PersistedEtag{"emulated", "W/\"weak\""},
        .size = 1, .condemn_round = 3, .delete_pending = true}});
    backend->clearJournal();

    const FsckReport rep = runFsck(*store, /*detail=*/true);

    EXPECT_EQ(rep.pending_token_rejected, 1u);
    EXPECT_EQ(rep.pending_without_intent, 0u);
    EXPECT_EQ(rep.pending_gc, 0u);
    EXPECT_TRUE(rep.clean());
    EXPECT_EQ(countVerdictsAtKey(rep, body_key, FsckClass::PendingTokenRejected), 1u);
    for (const JournalEntry & entry : backend->journal())
        for (const String & key : entry.keys)
            EXPECT_FALSE(key.starts_with(store->layout().blobIntentsPrefix()) && entry.verb == JournalVerb::Read) << "no intent key exists: " << key;
}

/// A refusal of the intent read under a `pending` row is `intent_unread`, a hard finding: the scan could not
/// tell whether the warning exists.
TEST(CASFsck, UnreadableIntentOfAPendingRowIsAHardFinding)
{
    FsckGcFixture f;
    f.drop();
    f.round();
    f.backend->refused_reads.insert(f.intentKey(1));

    const FsckReport rep = runFsck(*f.store, /*detail=*/false);

    EXPECT_EQ(rep.intent_unread, 1u);
    EXPECT_FALSE(rep.clean());
    EXPECT_NE(rep.intent_unread_reason.find("NOT_IMPLEMENTED"), String::npos) << rep.intent_unread_reason;
    EXPECT_EQ(rep.pending_without_intent, 0u) << "an unanswered read is not an absent intent";
}

/// A scan whose deadline is already in the past: partial_on_deadline=false keeps the old
/// throw-on-timeout contract; partial_on_deadline=true returns the accumulated lower-bound counts
/// instead of failing empty-handed (the 2026-07-05 campaign lost 5 verdicts to this).
TEST(CASFsckPartial, DeadlineReturnsAccumulatedCountsInsteadOfThrowing)
{
    auto backend = std::make_shared<InMemoryBackend>();
    auto store = openPoolForTest(backend);
    const RootNamespace ns{"00/aa@cas@"};
    const ManifestRef r = ref(1, 0xAA);
    writeBlobBody(*backend, store->layout(), DB::UInt128(1));
    writeManifestRaw(*backend, store->layout(), ns, r, {blobEntryFor("a", DB::UInt128(1))});
    const uint64_t sequence = publishCommittedTransition(*backend, store->layout(), ns, "tbl", std::nullopt, r);
    writeFsckCheckpoint(*backend, store->layout(), ns, RefTxnId{1, sequence});

    const auto past = std::chrono::steady_clock::now() - std::chrono::seconds(1);
    /// partial_on_deadline=false keeps the old contract:
    EXPECT_THROW(DB::Cas::runFsck(*store, /*detail=*/false, {}, past), DB::Exception);
    /// partial_on_deadline=true returns a flagged report:
    const auto report = DB::Cas::runFsck(*store, false, {}, past, /*partial_on_deadline=*/true);
    EXPECT_TRUE(report.partial);
    EXPECT_FALSE(report.partial_reason.empty());
}

/// A `namespace_prefix` scopes the scan to only the matching namespaces' refs (dangling-only): no
/// pool-wide unreachable/pending/awaiting/unaccounted classification, since that needs the whole pool.
TEST(CASFsckScoped, NamespacePrefixChecksOnlyMatchingRefsDanglingOnly)
{
    auto backend = std::make_shared<InMemoryBackend>();
    auto store = openPoolForTest(backend);

    const RootNamespace ns_a{"nsa"};
    const ManifestRef r_a = ref(1, 0xA1);
    writeBlobBody(*backend, store->layout(), DB::UInt128(1));
    writeManifestRaw(*backend, store->layout(), ns_a, r_a, {blobEntryFor("a", DB::UInt128(1))});
    const uint64_t sequence_a = publishCommittedTransition(
        *backend, store->layout(), ns_a, "tbl", std::nullopt, r_a);
    writeFsckCheckpoint(*backend, store->layout(), ns_a, RefTxnId{1, sequence_a});

    const RootNamespace ns_b{"nsb"};
    const ManifestRef r_b = ref(1, 0xB1);
    writeBlobBody(*backend, store->layout(), DB::UInt128(2));
    writeManifestRaw(*backend, store->layout(), ns_b, r_b, {blobEntryFor("b", DB::UInt128(2))});
    const uint64_t sequence_b = publishCommittedTransition(
        *backend, store->layout(), ns_b, "tbl", std::nullopt, r_b);
    writeFsckCheckpoint(*backend, store->layout(), ns_b, RefTxnId{1, sequence_b});

    const auto scoped = DB::Cas::runFsck(*store, false, {}, {}, false, /*namespace_prefix=*/"nsa");
    EXPECT_EQ(scoped.dangling, 0u);
    EXPECT_GT(scoped.reachable, 0u);
    /// Scoped mode skips only the POOL-WIDE physical/pipeline classification; the manifest-debris
    /// pass stays active for the scoped namespaces, so `unreachable` here counts THEIR orphan
    /// manifest bodies — zero in this clean setup, legitimately nonzero on a churned pool.
    EXPECT_EQ(scoped.unreachable, 0u);
    EXPECT_EQ(scoped.pending_gc + scoped.awaiting_gc + scoped.unaccounted, 0u);
}

/// B207: the ref-walk and the HEAD-confirm run minutes apart with no snapshot. A ref that gets
/// RE-PUBLISHED to a different manifest in that window, combined with a legitimate GC delete of the
/// blob it used to name, must NOT surface as a phantom `dangling` — only a CURRENT ref over an absent
/// object is a real dangle.
TEST(CASFsck, PhantomDanglingFromRepublishedRefIsReresolvedAway)
{
    auto backend = std::make_shared<RepublishOnListBackend>();
    auto store = openPoolForTest(backend);
    const RootNamespace ns{"00/aa@cas@"};
    const ManifestRef r1 = ref(1, 0xA1);
    const ManifestRef r2 = ref(2, 0xA2);
    const DB::UInt128 h1 = u128Of("b207-phantom-old");
    const DB::UInt128 h2 = u128Of("b207-phantom-new");

    writeBlobBody(*backend, store->layout(), h1);
    writeManifestRaw(*backend, store->layout(), ns, r1, {blobEntryFor("a", h1)});
    const uint64_t initial_sequence = publishCommittedTransition(
        *backend, store->layout(), ns, "tbl", std::nullopt, r1);
    writeFsckCheckpoint(*backend, store->layout(), ns, RefTxnId{1, initial_sequence});

    /// Fires strictly between the ref-walk (which captures ref "tbl" -> r1, blob h1, as reachable) and
    /// the HEAD-confirm's physical listing — exactly the window B207 is about.
    backend->armOnFirstList(store->layout().blobsPrefix(), [&]
    {
        writeBlobBody(*backend, store->layout(), h2);
        writeManifestRaw(*backend, store->layout(), ns, r2, {blobEntryFor("a", h2)});
        const uint64_t repoint_sequence = publishCommittedTransition(
            *backend, store->layout(), ns, "tbl", r1, r2);   /// re-publish
        writeFsckCheckpoint(*backend, store->layout(), ns, RefTxnId{1, repoint_sequence});

        const String old_key = store->layout().blobKey(BlobRef{BlobHashAlgo::CityHash128, BlobDigest::fromU128(h1)});
        OperationForTest nested_op(*backend);
        const auto head = (*nested_op).head(old_key, Retry::once());
        ASSERT_TRUE(head.has_value());
        (*nested_op).remove(old_key, head->etag, Retry::once());   /// legitimate GC delete of the now-unreferenced blob
    });

    const FsckReport rep = runFsck(*store, /*detail*/true);
    EXPECT_EQ(rep.dangling, 0u);
    EXPECT_TRUE(rep.clean());
}

/// Same race, but the ref is DROPPED (not re-published) in the window between the walk and the
/// HEAD-confirm — also must not surface as a phantom dangle.
TEST(CASFsck, PhantomDanglingFromDroppedRefIsReresolvedAway)
{
    auto backend = std::make_shared<RepublishOnListBackend>();
    auto store = openPoolForTest(backend);
    const RootNamespace ns{"00/aa@cas@"};
    const ManifestRef r1 = ref(1, 0xA1);
    const DB::UInt128 h1 = u128Of("b207-phantom-dropped");

    writeBlobBody(*backend, store->layout(), h1);
    writeManifestRaw(*backend, store->layout(), ns, r1, {blobEntryFor("a", h1)});
    const uint64_t initial_sequence = publishCommittedTransition(
        *backend, store->layout(), ns, "tbl", std::nullopt, r1);
    writeFsckCheckpoint(*backend, store->layout(), ns, RefTxnId{1, initial_sequence});

    backend->armOnFirstList(store->layout().blobsPrefix(), [&]
    {
        const uint64_t drop_sequence = dropRefTransition(
            *backend, store->layout(), ns, "tbl", r1);   /// ref dropped since the walk
        writeFsckCheckpoint(*backend, store->layout(), ns, RefTxnId{1, drop_sequence});

        const String old_key = store->layout().blobKey(BlobRef{BlobHashAlgo::CityHash128, BlobDigest::fromU128(h1)});
        OperationForTest nested_op(*backend);
        const auto head = (*nested_op).head(old_key, Retry::once());
        ASSERT_TRUE(head.has_value());
        (*nested_op).remove(old_key, head->etag, Retry::once());   /// legitimate GC delete after the drop folds
    });

    const FsckReport rep = runFsck(*store, /*detail*/true);
    EXPECT_EQ(rep.dangling, 0u);
    EXPECT_TRUE(rep.clean());
}

/// Companion: the fix must never HIDE a real loss. A blob that a CURRENT ref still names, but whose
/// object is genuinely gone (an operator error, a storage-layer bug — NOT a legitimate GC delete),
/// stays `dangling` after the re-resolve.
TEST(CASFsck, RealDanglingStillCaughtAfterReresolve)
{
    auto backend = std::make_shared<InMemoryBackend>();
    auto store = openPoolForTest(backend);
    const RootNamespace ns{"00/aa@cas@"};
    const ManifestRef r = ref(1, 0xA1);
    const DB::UInt128 h = u128Of("b207-real-dangle");

    writeBlobBody(*backend, store->layout(), h);
    writeManifestRaw(*backend, store->layout(), ns, r, {blobEntryFor("a", h)});
    const uint64_t sequence = publishCommittedTransition(*backend, store->layout(), ns, "tbl", std::nullopt, r);
    writeFsckCheckpoint(*backend, store->layout(), ns, RefTxnId{1, sequence});

    const String key = store->layout().blobKey(BlobRef{BlobHashAlgo::CityHash128, BlobDigest::fromU128(h)});
    OperationForTest op(*backend);
    const auto head = (*op).head(key, Retry::once());
    ASSERT_TRUE(head.has_value());
    (*op).remove(key, head->etag, Retry::once());   /// genuine loss — the ref is UNCHANGED, still names this blob

    const FsckReport rep = runFsck(*store, /*detail*/true);
    EXPECT_EQ(rep.dangling, 1u);
    EXPECT_FALSE(rep.clean());
}

/// The MANIFEST analogue of the blob phantom-dangle. The ref-walk captures "tbl" -> r1's manifest, then
/// the ref is RE-PUBLISHED to a different manifest r2 and the OLD r1 manifest body is legitimately
/// GC-deleted before the per-ref body GET. The missing OLD manifest must be revalidated away — a fresh
/// re-resolve shows the CURRENT ref no longer names it — never surfacing as a phantom `dangling`.
TEST(CASFsck, PhantomDanglingManifestFromRepublishedRefIsReresolvedAway)
{
    auto backend = std::make_shared<MutateOnFirstGetBackend>();
    auto store = openPoolForTest(backend);
    const RootNamespace ns{"00/aa@cas@"};
    const ManifestRef r1 = ref(1, 0xA1);
    const ManifestRef r2 = ref(2, 0xA2);
    const DB::UInt128 h1 = u128Of("phantom-manifest-old");
    const DB::UInt128 h2 = u128Of("phantom-manifest-new");

    writeBlobBody(*backend, store->layout(), h1);
    writeManifestRaw(*backend, store->layout(), ns, r1, {blobEntryFor("a", h1)});
    const uint64_t initial_sequence = publishCommittedTransition(
        *backend, store->layout(), ns, "tbl", std::nullopt, r1);
    writeFsckCheckpoint(*backend, store->layout(), ns, RefTxnId{1, initial_sequence});

    const String m1_key = store->layout().manifestKey(ManifestId{ns, r1});
    /// Fires strictly between the ref-walk (captures "tbl" -> r1) and the per-ref GET of r1's manifest:
    /// re-publish "tbl" to r2 and legitimately GC-delete the now-superseded r1 manifest body.
    backend->armOnFirstGet(m1_key, [&]
    {
        writeBlobBody(*backend, store->layout(), h2);
        writeManifestRaw(*backend, store->layout(), ns, r2, {blobEntryFor("a", h2)});
        const uint64_t repoint_sequence = publishCommittedTransition(
            *backend, store->layout(), ns, "tbl", r1, r2);   /// re-publish
        writeFsckCheckpoint(*backend, store->layout(), ns, RefTxnId{1, repoint_sequence});

        OperationForTest nested_op(*backend);
        const auto head = (*nested_op).head(m1_key, Retry::once());
        ASSERT_TRUE(head.has_value());
        (*nested_op).remove(m1_key, head->etag, Retry::once());   /// legitimate GC delete of the superseded manifest
    });

    const FsckReport rep = runFsck(*store, /*detail*/true);
    EXPECT_EQ(rep.dangling, 0u);
    EXPECT_TRUE(rep.clean());
}

namespace
{
/// Build a real `ContentAddressedMetadataStorage` over Local object storage and start it (Mounted) --
/// the same harness gtest_cas_operation_gate.cpp uses. Each call gets an isolated pool root.
std::shared_ptr<DB::ContentAddressedMetadataStorage> openRunningStorageForTest()
{
    auto settings = makeSettingsForTest("test", std::filesystem::temp_directory_path() / "ca_fsck_running_scratch");
    auto storage = std::make_shared<DB::ContentAddressedMetadataStorage>(
        makeLocalObjectStorageForTest(), "pool", "srv1", "", nullptr, settings);
    storage->startup();
    return storage;
}

/// Commit one real part (tmp -> final rename -> commit) so a RUNNING FSCK has live committed content.
void commitOneRunningPart(DB::ContentAddressedMetadataStorage & storage)
{
    const std::string table_dir = "g80/g80g80g8-0808-4808-8808-080808080808";
    auto tx = storage.createTransaction();
    auto & ca_tx = dynamic_cast<DB::ContentAddressedTransaction &>(*tx);
    auto buf = ca_tx.writeFile(table_dir + "/tmp_insert_all_1_1_0/data.bin", 65536, DB::WriteMode::Rewrite, {});
    const std::string bytes = "content-of-the-part";
    buf->write(bytes.data(), bytes.size());
    buf->finalize();
    tx->moveDirectory(table_dir + "/tmp_insert_all_1_1_0", table_dir + "/all_1_1_0");
    tx->commit(DB::NoCommitOptions{});
}
}

/// (rev.8) FSCK runs on a RUNNING disk: scanning a live pool with one committed part succeeds and reports
/// its content (the one-row summary the SQL verb renders from this report).
TEST(CASFsckRunning, FsckOnMountedDiskSucceeds)
{
    auto storage = openRunningStorageForTest();
    commitOneRunningPart(*storage);

    FsckReport rep;
    EXPECT_NO_THROW(rep = storage->runFsckNow(/*detail=*/false));
    EXPECT_TRUE(rep.clean());
    EXPECT_GE(rep.distinct_blobs, 1u) << "the running scan must see the live committed part's blob";
    EXPECT_EQ(rep.dangling, 0u);
}

/// (rev.8) FSCK is Admin-class: on a not-live pool (a lease blip / IdentityLost) it refuses before
/// scanning, exactly like the GC entry points -- an FSCK of a disk whose data root may be gone or replaced
/// is meaningless (the operator has the snapshot / FORGET path). The two states refuse in DIFFERENT
/// classes, and the pairing is the point: a lease blip is transient unavailability (upstream-retryable),
/// an identity loss is terminal (668).
TEST(CASFsckRunning, FsckOnNotLiveDiskRefusesTransientRetryableAndIdentityLostTerminal)
{
    for (const auto & [lc, code] : {std::pair{PoolLifecycle::TransientNotLive, DB::ErrorCodes::NETWORK_ERROR},
                                    std::pair{PoolLifecycle::IdentityLost, DB::ErrorCodes::INVALID_STATE}})
    {
        auto storage = openRunningStorageForTest();
        storage->store()->setLifecycleForTest(lc);   /// one force from Live; no later store() call
        expectThrowsCode(code, [&] { storage->runFsckNow(/*detail=*/false); });
    }
}

/// The summary line is the ONLY thing most consumers ever read: the soak harness parses it, an operator
/// eyeballs it, and `exit_code` gates CI on it. So a field that `clean()` treats as a hard finding but the
/// summary omits is invisible in practice, however faithfully it is counted -- which is exactly what
/// happened to `corrupted_runs`: counted since the seal check landed, part of `clean()`, rendered in
/// `--detail` rows, and absent from the summary, so no run has ever reported one.
///
/// This test ITERATES `kFsckHardFindings` -- the list `clean()` is computed from -- and never names a
/// finding itself, so a term added to that list and not rendered fails HERE. It used to claim exactly
/// that while its body was a hand-listed set of five names, and the claim was false: `lifeless_keys` was
/// added to `clean()` and nothing failed anywhere, which is how it reached the SQL row's absence too.
/// `formatFsckSummary` exists to be testable at all: the line used to be built inline in
/// `CommandFsck::executeImpl`, where nothing could reach it.
///
/// A per-finding DISTINCT value is what makes this more than a substring sweep: it catches a formatter
/// that prints the right names against the wrong counters.
TEST(CASFsckSummary, EveryHardFindingAppearsOnTheSummaryLine)
{
    FsckReport rep;
    uint64_t value = 11;
    for (const FsckHardFinding & finding : kFsckHardFindings)
    {
        rep.*finding.value = value;
        value += 11;
    }

    const String line = formatFsckSummary(rep);

    value = 11;
    for (const FsckHardFinding & finding : kFsckHardFindings)
    {
        const String token = String(finding.name) + "=" + std::to_string(value);
        EXPECT_NE(line.find(token), String::npos)
            << "hard finding '" << finding.name << "' is missing from the summary line (expected `"
            << token << "`); the line was: " << line;
        value += 11;
    }

    /// A report carrying these values is NOT clean; the line must not be mistakable for a clean one.
    EXPECT_FALSE(rep.clean());
}

/// A zero must be PRINTED, not omitted. The harness's `stale_edge_verdict` fails closed on an absent key
/// precisely because "field missing" and "field zero" are different facts, and a formatter that skips
/// zeros would turn every clean pool into an unparseable one.
TEST(CASFsckSummary, ZeroValuedHardFindingsAreStillPrinted)
{
    const String line = formatFsckSummary(FsckReport{});
    /// Iterated for the same reason the test above is: a new hard finding printed only when nonzero is a
    /// finding the harness's fail-closed-on-absence consumers would read as missing.
    for (const FsckHardFinding & finding : kFsckHardFindings)
        EXPECT_NE(line.find(String(finding.name) + "=0"), String::npos)
            << "hard finding '" << finding.name << "' prints no zero; the line was: " << line;
    EXPECT_EQ(line.find("partial="), String::npos) << "a non-partial report must not claim partial: " << line;
}

/// A partial scan is a lower bound over the visited subset, so the flag and its reason must travel WITH
/// the counts -- a consumer that sees the numbers but not `partial=1` reads a truncated walk as the pool
/// truth.
/// The advisory counters and the verification flag are on the line too, zeros included: "absent" and "zero"
/// are different facts to a consumer, and `intents_verified=0` is what says the `intent_*` verdicts were not run.
TEST(CASFsckSummary, AdvisoryIntentCountersAndTheVerificationFlagArePrinted)
{
    FsckReport report;
    const String plain = formatFsckSummary(report);
    for (const char * name : {"intent_listed=0", "pending_intent_absent=0", "pending_token_rejected=0", "standing_present=0", "intents_verified=0"})
        EXPECT_NE(plain.find(name), String::npos) << name << " missing from: " << plain;
    report.intents_verified = true;
    report.intent_listed = 5;
    report.standing_present = 2;
    const String verified = formatFsckSummary(report);
    EXPECT_NE(verified.find("intents_verified=1"), String::npos) << verified;
    EXPECT_NE(verified.find("intent_listed=5"), String::npos) << verified;
    EXPECT_NE(verified.find("standing_present=2"), String::npos) << verified;
    EXPECT_TRUE(report.clean()) << "the advisory counters never make a report unclean";
}

TEST(CASFsckSummary, PartialFlagAndReasonTravelWithTheCounts)
{
    FsckReport rep;
    rep.partial = true;
    rep.partial_reason = "deadline exceeded after 180s";
    const String line = formatFsckSummary(rep);
    EXPECT_NE(line.find("partial=1"), String::npos) << line;
    EXPECT_NE(line.find("reason='deadline exceeded after 180s'"), String::npos) << line;
}

namespace
{

void eraseCatalogRowForFsck(Backend & backend, const Layout & layout, const RootNamespace & ns)
{
    CasRequests requests = DB::Cas::tests::openRequestsForTest(backend);
    CasOperation op = requests.admit();
    CasRefCatalog::Snapshot current = CasRefCatalog::read(op, layout);
    std::erase_if(current.catalog.entries, [&](const CatalogEntry & entry) { return entry.ns == ns; });
    ASSERT_TRUE(current.etag.has_value());
    ASSERT_TRUE(std::holds_alternative<Committed>(
        op.replace(layout.refCatalogKey(), encodeRefCatalog(current.catalog), *current.etag, Retry::standard())));
}

void removeForFsck(Backend & backend, const String & key)
{
    OperationForTest op(backend);
    const auto head = (*op).head(key, Retry::once());
    ASSERT_TRUE(head.has_value());
    ASSERT_EQ((*op).remove(key, head->etag, Retry::once()), Removal::Removed);
}

}

TEST(CASFsck, ChainWithASkippedEpochIsClean)
{
    ScopedReadCeiling c(3);
    auto backend = std::make_shared<InMemoryBackend>();
    auto store = openPoolForTest(backend);
    const Layout & layout = store->layout();
    const RootNamespace ns{"00/fsck_skipped_epoch@cas@"};
    fixture::admitLive(*backend, layout, ns);
    CasRequests requests = DB::Cas::tests::openRequestsForTest(backend);
    CasOperation op = requests.admit();
    const NamespaceLifeId life = *CasRefCatalog::lifeIfCataloged(op, layout, ns);
    const auto put = [&](const RefLogTxn & txn)
    {
        ASSERT_TRUE(std::holds_alternative<Committed>(op.create(layout.refLogKey(life, txn.txn_id),
            sealObject(FormatId::RefLog, encodeRefLogTxnAtGeneration(txn, G_REF_CHAIN)), Retry::once())));
    };
    put(RefLogTxn{ns.string(), RefTxnId{1, 1}, {namespaceBirthOp()}, std::nullopt});
    put(tombstoneTxn(ns.string(), RefTxnId{1, 2}, 5));
    put(RefLogTxn{ns.string(), RefTxnId{5, 1}, {}, RefTxnId{1, 2}});
    ASSERT_TRUE(std::holds_alternative<Committed>(
        op.create(layout.refCkptKey(life), encodeRefCkptAtGeneration(refCkptOf(1), G_REF_CHAIN), Retry::once())));

    const FsckReport report = runFsck(*store, /*detail=*/true);
    EXPECT_EQ(report.chain_broken, 0u);
    EXPECT_EQ(report.unchecked, 0u);
    EXPECT_EQ(report.dangling, 0u);
    EXPECT_EQ(report.ref_records_walked, 3u);
    EXPECT_TRUE(report.clean());
}

TEST(CASFsck, LifeErasedDuringTheScanIsJanitorPending)
{
    auto backend = std::make_shared<MutateOnFirstGetBackend>();
    auto store = openPoolForTest(backend);
    const Layout & layout = store->layout();
    const RootNamespace ns{"00/fsck_erased_life@cas@"};
    const ManifestRef r = ref(1, 0xE7);
    const uint64_t sequence = publishCommittedTransition(*backend, layout, ns, "tbl", std::nullopt, r);
    writeFsckCheckpoint(*backend, layout, ns, RefTxnId{1, sequence});
    CasRequests requests = DB::Cas::tests::openRequestsForTest(backend);
    CasOperation op = requests.admit();
    const NamespaceLifeId life = *CasRefCatalog::lifeIfCataloged(op, layout, ns);
    backend->armOnFirstGet(layout.refCkptKey(life), [&]
    {
        eraseCatalogRowForFsck(*backend, layout, ns);
        for (uint64_t s = 1; s <= sequence; ++s)
            removeForFsck(*backend, layout.refLogKey(life, RefTxnId{1, s}));
    });

    const FsckReport report = runFsck(*store, /*detail=*/true);
    EXPECT_EQ(report.dangling, 0u);
    EXPECT_EQ(report.namespace_janitor_pending_lives, 1u);
    EXPECT_EQ(report.chain_broken, 0u);
    EXPECT_EQ(report.unchecked, 0u);
}

TEST(CASFsck, LateRecheckOfAnErasedLifeIsNotDangling)
{
    auto backend = std::make_shared<RepublishOnListBackend>();
    auto store = openPoolForTest(backend);
    const Layout & layout = store->layout();
    const RootNamespace ns{"00/fsck_late_erased@cas@"};
    const ManifestRef r = ref(1, 0xE8);
    writeManifestRaw(*backend, layout, ns, r, {blobEntryFor("a", u128Of("fsck-late-erased-blob"))});
    const uint64_t sequence = publishCommittedTransition(*backend, layout, ns, "tbl", std::nullopt, r);
    writeFsckCheckpoint(*backend, layout, ns, RefTxnId{1, sequence});
    backend->armOnFirstList(layout.blobsPrefix(), [&] { eraseCatalogRowForFsck(*backend, layout, ns); });

    const FsckReport report = runFsck(*store, /*detail=*/true);
    EXPECT_EQ(report.dangling, 0u);
}

namespace
{

/// After every read of `watched_key` while armed, replaces `_ckpt` with its own bytes (a new token).
class FsckCkptChurnBackend : public CountingBackend
{
public:
    String watched_key;
    String ckpt_key;
    bool armed = false;
    String arm_on_key;

    std::optional<Raw> read(const String & key, TransportAccess & access) override
    {
        if (!arm_on_key.empty() && key == arm_on_key)
            armed = true;
        std::optional<Raw> result = CountingBackend::read(key, access);
        if (armed && key == watched_key)
        {
            const auto current = InMemoryBackend::read(ckpt_key, access);
            if (!current || !InMemoryBackend::write(ckpt_key, current->bytes, current->value, access).has_value())
                throw std::runtime_error("test checkpoint churn failed");
        }
        return result;
    }
};

}

TEST(CASFsckAuthority, CheckpointMovingOnEveryWalkExhaustsTheRestartsAndIsUnchecked)
{
    auto backend = std::make_shared<FsckCkptChurnBackend>();
    auto store = openPoolForTest(backend);
    const Layout & layout = store->layout();
    const RootNamespace ns{"00/fsck_ckpt_churn@cas@"};
    const NamespaceLifeId life = writeFsckFixtureWithTail(*backend, layout, ns);
    backend->ckpt_key = layout.refCkptKey(life);
    backend->watched_key = layout.refLogKey(life, RefTxnId{1, 1});
    backend->armed = true;
    backend->resetCounts();

    const FsckReport report = runFsck(*store, /*detail=*/true);
    EXPECT_EQ(report.chain_broken, 0u);
    EXPECT_EQ(report.unchecked, 1u);
    EXPECT_TRUE(std::any_of(report.objects.begin(), report.objects.end(), [](const FsckObject & object)
    {
        return object.cls == FsckClass::Unchecked && !object.reachable_from.empty()
            && object.reachable_from.front().find("checkpoint authority changed while walking the chain") != String::npos;
    }));
    EXPECT_EQ(backend->getCount(layout.refLogKey(life, RefTxnId{1, 1})), kRefReadOnlyRecoveryMaxRestarts + 1)
        << "the base log is read once per walk: the first and one per restart";
}

TEST(CASFsck, LateRecheckWhoseCheckpointMovesOnEveryWalkIsUnchecked)
{
    auto backend = std::make_shared<FsckCkptChurnBackend>();
    auto store = openPoolForTest(backend);
    const Layout & layout = store->layout();
    const RootNamespace ns{"00/fsck_late_churn@cas@"};
    const ManifestRef r = ref(1, 0xE9);
    const uint64_t sequence = publishCommittedTransition(*backend, layout, ns, "tbl", std::nullopt, r);
    writeFsckCheckpoint(*backend, layout, ns, RefTxnId{1, sequence});
    CasRequests requests = DB::Cas::tests::openRequestsForTest(backend);
    CasOperation op = requests.admit();
    const NamespaceLifeId life = *CasRefCatalog::lifeIfCataloged(op, layout, ns);
    /// The primary walk is quiet; the churn starts at the missing manifest's read, before the late recheck.
    backend->ckpt_key = layout.refCkptKey(life);
    backend->watched_key = layout.refLogKey(life, RefTxnId{1, 1});
    backend->arm_on_key = layout.manifestKey(ManifestId{ns, r});
    backend->resetCounts();

    const FsckReport report = runFsck(*store, /*detail=*/true);
    EXPECT_EQ(report.unchecked, 1u);
    EXPECT_TRUE(std::any_of(report.objects.begin(), report.objects.end(), [](const FsckObject & object)
    {
        return object.cls == FsckClass::Unchecked && !object.reachable_from.empty()
            && object.reachable_from.front().find("kept moving during the walk") != String::npos;
    }));
    EXPECT_EQ(backend->getCount(layout.refLogKey(life, RefTxnId{1, 1})), 1 + kRefReadOnlyRecoveryMaxRestarts + 1)
        << "one primary walk, then the late recheck's first walk and one per restart";
}

/// Findings held for the catalog confirmation are not dropped by a deadline: they are reported unchecked.
TEST(CASFsckPartial, DeadlineReportsHeldFindingsAsUnchecked)
{
    auto backend = std::make_shared<MutateOnFirstGetBackend>();
    auto store = openPoolForTest(backend);
    const Layout & layout = store->layout();
    const RootNamespace broken{"00/a_deadline_broken@cas@"};
    const RootNamespace later{"00/b_deadline_later@cas@"};
    const NamespaceLifeId broken_life = writeFsckFixtureWithTail(*backend, layout, broken);
    writeFsckFixtureWithTail(*backend, layout, later);
    const String hole = layout.refLogKey(broken_life, RefTxnId{1, 2});
    removeForFsck(*backend, hole);

    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    /// Runs out the clock while the first life's walk meets its hole, so the deadline fires before the
    /// second life, after the first life's finding is held.
    backend->armOnFirstGet(hole, [&]
    {
        while (std::chrono::steady_clock::now() <= deadline)
            std::this_thread::yield();
    });

    const FsckReport report = runFsck(*store, /*detail=*/true, {}, deadline, /*partial_on_deadline=*/true);
    ASSERT_TRUE(report.partial);
    EXPECT_EQ(report.chain_broken, 0u) << "a finding the catalog did not confirm is not reported as corruption";
    EXPECT_EQ(report.unchecked, 1u);
    EXPECT_TRUE(std::any_of(report.objects.begin(), report.objects.end(), [&](const FsckObject & object)
    {
        return object.cls == FsckClass::Unchecked && !object.reachable_from.empty()
            && object.reachable_from.front().find("not confirmed before the deadline") != String::npos;
    }));
}

namespace
{

/// Once `on_second_blob_head` is set, runs it before the second HEAD of a key under `blobs_prefix`.
class SecondBlobHeadHookBackend : public InMemoryBackend
{
public:
    String blobs_prefix;
    std::function<void()> on_second_blob_head;

    std::optional<RawMeta> head(const String & key, TransportAccess & access) override
    {
        if (on_second_blob_head && key.starts_with(blobs_prefix) && ++blob_heads == 2)
            on_second_blob_head();
        return InMemoryBackend::head(key, access);
    }

private:
    size_t blob_heads = 0;
};

}

/// A dangling blob held for the late catalog confirmation is not dropped by a deadline in the blob loop:
/// it is reported unchecked.
TEST(CASFsckPartial, DeadlineInTheBlobLoopReportsHeldDanglingsAsUnchecked)
{
    auto backend = std::make_shared<SecondBlobHeadHookBackend>();
    auto store = openPoolForTest(backend);
    const Layout & layout = store->layout();
    const RootNamespace ns{"00/blob_loop_deadline@cas@"};
    const ManifestRef r = ref(1, 0xB1);
    const ManifestEntry first = blobEntryFor("a", u128Of("fsck-blob-loop-deadline-a"));
    const ManifestEntry second = blobEntryFor("b", u128Of("fsck-blob-loop-deadline-b"));
    writeManifestRaw(*backend, layout, ns, r, {first, second});
    const uint64_t sequence = publishCommittedTransition(*backend, layout, ns, "tbl", std::nullopt, r);
    writeFsckCheckpoint(*backend, layout, ns, RefTxnId{1, sequence});

    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    /// Both blobs are missing. The first is re-resolved and held; the clock runs out before the second
    /// one's recheck, so the deadline fires inside the blob loop with one dangling held.
    backend->blobs_prefix = layout.blobsPrefix();
    backend->on_second_blob_head = [&]
    {
        while (std::chrono::steady_clock::now() <= deadline)
            std::this_thread::yield();
    };

    const FsckReport report = runFsck(*store, /*detail=*/true, {}, deadline, /*partial_on_deadline=*/true);
    ASSERT_TRUE(report.partial);
    EXPECT_EQ(report.dangling, 0u) << "a finding the catalog did not confirm is not reported as corruption";
    EXPECT_EQ(report.unchecked, 1u);
    const String note = "late recheck not confirmed before the deadline";
    EXPECT_NE(uncheckedNoteContains(report, layout.blobKey(first.ref), note),
              uncheckedNoteContains(report, layout.blobKey(second.ref), note))
        << "exactly the first blob of the loop was held";
}
