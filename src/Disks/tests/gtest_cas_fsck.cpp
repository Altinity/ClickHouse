#include <gtest/gtest.h>

#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Primitives/CasBoundedRun.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Tools/CasFsck.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Gc/CasBlobInDegree.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Gc/CasGc.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Gc/CasGcShardPlan.h>
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

#include <Poco/Exception.h>
#include <base/scope_guard.h>
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
extern const int MEMORY_LIMIT_EXCEEDED;
extern const int BAD_ARGUMENTS;
extern const int CORRUPTED_DATA;
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

    /// Runs `mutation` once, at the first read of any key under `prefix`; an empty prefix matches every key.
    void armOnFirstGetUnder(String prefix, std::function<void()> mutation)
    {
        std::lock_guard<std::mutex> lock(arm_mutex);
        armed_prefix = std::move(prefix);
        pending_prefix_mutation = std::move(mutation);
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
            else if (pending_prefix_mutation && key.starts_with(armed_prefix))
            {
                to_run = std::move(pending_prefix_mutation);
                pending_prefix_mutation = nullptr;
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
    String armed_prefix;
    std::function<void()> pending_prefix_mutation;
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

namespace
{
/// A summary scan configured as `cas-fsck` runs it: it reads the creator slot of each `Creating` row.
FsckReport runFsckReadingCreatorSlots(Pool & store)
{
    return runFsck(store, /*detail=*/false, {}, {}, /*partial_on_deadline=*/false, /*namespace_prefix=*/{},
                   /*fix_legacy_markers=*/false, /*verify_intents=*/false, /*fix_concurrency=*/1, /*read_creator_slots=*/true);
}
}

/// Every `Creating` row is reported with its creator and what the creator's slot says, in summary mode too:
/// a higher epoch or a farewell is terminal, a slot at the row's epoch is live, no slot is absent, and a
/// slot that does not decode is undecodable. Only the terminal ones count as `stalled_creating`.
TEST(CASFsck, EveryCreatingRowIsReportedWithItsCreatorSlot)
{
    auto backend = std::make_shared<InMemoryBackend>();
    auto store = openPoolForTest(backend);
    const Layout & layout = store->layout();
    CasOperation op = store->openRequests().admit();
    store->namespaceLife(RootNamespace{"00/live@cas@"});
    const auto admit_creating = [&](const String & ns, UInt128 incarnation, const String & root)
    {
        CasRefCatalog::casAdmitEntry(op, layout, store->poolConfig().gc_shards, CatalogEntry{
            .ns = RootNamespace{ns}, .state = NsState::Creating, .incarnation = incarnation,
            .creator = CreatorFence{.server_root_id = root, .writer_epoch = 1, .fence_generation = 1}});
    };
    admit_creating("00/superseded@cas@", UInt128{0xa1}, "moved-creator");
    admit_creating("00/farewell@cas@", UInt128{0xa2}, "retired-creator");
    admit_creating("00/unproven@cas@", UInt128{0xa3}, "never-mounted");
    admit_creating("00/running@cas@", UInt128{0xa4}, "running-creator");
    admit_creating("00/garbled@cas@", UInt128{0xa5}, "garbled-creator");
    setWatermarkMinActive(*backend, layout, "moved-creator", /*writer_epoch*/ 2, std::numeric_limits<uint64_t>::max());
    setWatermarkMinActive(*backend, layout, "retired-creator", /*writer_epoch*/ 1, std::numeric_limits<uint64_t>::max());
    setWatermarkMinActive(*backend, layout, "running-creator", /*writer_epoch*/ 1, /*min_active_build_sequence*/ 1);
    ASSERT_TRUE(std::holds_alternative<Committed>(op.create(layout.mountKey("garbled-creator"), "not-a-lease", Retry::once())));

    const FsckReport rep = runFsckReadingCreatorSlots(*store);

    EXPECT_EQ(rep.stalled_creating, 2u) << formatFsckSummary(rep);
    EXPECT_EQ(rep.creating_unproven, 3u) << formatFsckSummary(rep);
    EXPECT_NE(formatFsckSummary(rep).find("stalled_creating=2"), String::npos) << formatFsckSummary(rep);
    EXPECT_NE(formatFsckSummary(rep).find("creating_unproven=3"), String::npos) << formatFsckSummary(rep);
    std::map<String, std::pair<String, CreatorSlotStatus>> rows;
    for (const FsckCreatingRow & row : rep.creating_rows)
    {
        EXPECT_EQ(row.writer_epoch, 1u);
        ASSERT_TRUE(row.slot) << row.ns << ": " << row.slot_read_error;
        rows.emplace(row.ns, std::pair{row.creator_root, *row.slot});
    }
    EXPECT_EQ(rows, (std::map<String, std::pair<String, CreatorSlotStatus>>{
        {"00/superseded@cas@", {"moved-creator", CreatorSlotStatus::Terminal}},
        {"00/farewell@cas@", {"retired-creator", CreatorSlotStatus::Terminal}},
        {"00/unproven@cas@", {"never-mounted", CreatorSlotStatus::Absent}},
        {"00/running@cas@", {"running-creator", CreatorSlotStatus::Live}},
        {"00/garbled@cas@", {"garbled-creator", CreatorSlotStatus::Undecodable}}}));
    EXPECT_TRUE(rep.clean()) << "a `Creating` row is not a hard finding";
    EXPECT_FALSE(rep.catalog_has_no_walkable_row) << "the `Live` namespace is walkable";
}

/// The line `cas-fsck` prints for a `Creating` row names the namespace, the creator, the epoch, and the
/// slot status with what GC does about it.
TEST(CASFsck, TheCreatingRowNoteSaysWhatGcDoesForEachSlotStatus)
{
    const auto note = [](CreatorSlotStatus slot)
    {
        return formatFsckCreatingRow(FsckCreatingRow{.ns = "00/t@cas@", .creator_root = "srv", .writer_epoch = 7, .slot = slot,
            .slot_read_error = {}});
    };
    for (const CreatorSlotStatus slot :
         {CreatorSlotStatus::Terminal, CreatorSlotStatus::Live, CreatorSlotStatus::Absent, CreatorSlotStatus::Undecodable})
        EXPECT_NE(note(slot).find("namespace '00/t@cas@' is `Creating` under creator 'srv' at writer epoch 7"), String::npos)
            << note(slot);
    EXPECT_NE(note(CreatorSlotStatus::Terminal).find("terminal: the next GC round cancels"), String::npos);
    EXPECT_NE(note(CreatorSlotStatus::Live).find("live: "), String::npos);
    EXPECT_NE(note(CreatorSlotStatus::Absent).find("absent on this read: "), String::npos);
    EXPECT_NE(note(CreatorSlotStatus::Undecodable).find("undecodable: "), String::npos);
    const String unreadable = formatFsckCreatingRow(FsckCreatingRow{.ns = "00/t@cas@", .creator_root = "srv",
        .writer_epoch = 7, .slot = std::nullopt, .slot_read_error = "injected"});
    EXPECT_NE(unreadable.find("namespace '00/t@cas@' is `Creating` under creator 'srv' at writer epoch 7"), String::npos)
        << unreadable;
    EXPECT_NE(unreadable.find("unreadable (injected)"), String::npos) << unreadable;
}

/// The leader's keep or refusal is attributed to the leader, and the creating server's own cleanup is named
/// apart from it on every row the leader does not cancel: that server never reads its slot.
TEST(CASFsck, TheCreatingRowNoteNamesTheLeaderAndTheCreatorsOwnCleanup)
{
    const String owner_clause = "a writable creator with its GC scheduler running also cancels its own idle rows of its "
                                "current writer epoch";
    const auto note = [](CreatorSlotStatus slot)
    {
        return formatFsckCreatingRow(FsckCreatingRow{.ns = "00/t@cas@", .creator_root = "srv", .writer_epoch = 7, .slot = slot,
            .slot_read_error = {}});
    };
    const String unreadable = formatFsckCreatingRow(FsckCreatingRow{.ns = "00/t@cas@", .creator_root = "srv",
        .writer_epoch = 7, .slot = std::nullopt, .slot_read_error = "injected"});
    for (const String & line : {note(CreatorSlotStatus::Live), note(CreatorSlotStatus::Absent),
                                note(CreatorSlotStatus::Undecodable), unreadable})
    {
        EXPECT_NE(line.find("the GC leader"), String::npos) << line;
        EXPECT_NE(line.find(owner_clause), String::npos) << line;
    }
    EXPECT_NE(unreadable.find("unreadable (injected)"), String::npos) << unreadable;
    EXPECT_EQ(note(CreatorSlotStatus::Terminal).find(owner_clause), String::npos) << note(CreatorSlotStatus::Terminal);

    FsckReport rep;
    rep.pending_gc = 3;
    rep.catalog_has_no_walkable_row = true;
    const String pipeline = formatFsckPipelineNote(rep);
    EXPECT_NE(pipeline.find("`stalled_creating` counts terminal rows, which the GC leader cancels"), String::npos) << pipeline;
    EXPECT_NE(pipeline.find(owner_clause), String::npos) << pipeline;
}

#if USE_AWS_S3
/// A store error reading one creator's mount slot marks that row unreadable and the scan goes on: the other
/// `Creating` row is still classified.
TEST(CASFsck, AnUnreadableCreatorSlotMarksItsRowAndTheScanGoesOn)
{
    auto backend = std::make_shared<FailExactGetBackend>();
    auto store = openPoolForTest(backend);
    const Layout & layout = store->layout();
    CasOperation op = store->openRequests().admit();
    const auto admit_creating = [&](const String & ns, UInt128 incarnation, const String & root)
    {
        CasRefCatalog::casAdmitEntry(op, layout, store->poolConfig().gc_shards, CatalogEntry{
            .ns = RootNamespace{ns}, .state = NsState::Creating, .incarnation = incarnation,
            .creator = CreatorFence{.server_root_id = root, .writer_epoch = 1, .fence_generation = 1}});
        setWatermarkMinActive(*backend, layout, root, /*writer_epoch*/ 1, /*min_active_build_sequence*/ 1);
    };
    admit_creating("00/a-unreadable@cas@", UInt128{0xb1}, "unreadable-creator");
    admit_creating("00/b-running@cas@", UInt128{0xb2}, "running-creator");
    backend->fail(layout.mountKey("unreadable-creator"));

    FsckReport rep;
    ASSERT_NO_THROW(rep = runFsckReadingCreatorSlots(*store));

    ASSERT_EQ(rep.creating_rows.size(), 2u);
    EXPECT_EQ(rep.stalled_creating, 0u);
    EXPECT_EQ(rep.creating_unproven, 2u);
    for (const FsckCreatingRow & row : rep.creating_rows)
    {
        if (row.creator_root == "unreadable-creator")
        {
            EXPECT_FALSE(row.slot);
            EXPECT_NE(row.slot_read_error.find("injected access denial"), String::npos) << row.slot_read_error;
        }
        else
        {
            EXPECT_EQ(row.slot, std::optional<CreatorSlotStatus>{CreatorSlotStatus::Live});
        }
    }
}
#endif

/// GC deletes nothing only while the catalog has rows and none of them is walkable, so only then does the
/// note on the deletion pipeline stop calling the backlog "no action needed".
TEST(CASFsck, ThePipelineNoteSaysNoActionNeededUnlessNoCatalogRowIsWalkable)
{
    FsckReport rep;
    rep.pending_gc = 3;
    rep.stalled_creating = 1;
    EXPECT_NE(formatFsckPipelineNote(rep).find("no action needed"), String::npos) << formatFsckPipelineNote(rep);
    rep.catalog_has_no_walkable_row = true;
    const String note = formatFsckPipelineNote(rep);
    EXPECT_EQ(note.find("no action needed"), String::npos) << note;
    EXPECT_NE(note.find("Creating"), String::npos) << note;
}

TEST(CASFsck, ACatalogOfOnlyCreatingRowsHasNoWalkableRow)
{
    auto backend = std::make_shared<InMemoryBackend>();
    auto store = openPoolForTest(backend);
    const Layout & layout = store->layout();
    CasOperation op = store->openRequests().admit();
    EXPECT_FALSE(runFsck(*store, /*detail*/ false).catalog_has_no_walkable_row) << "an empty catalog is proved empty";
    CasRefCatalog::casAdmitEntry(op, layout, store->poolConfig().gc_shards, CatalogEntry{
        .ns = RootNamespace{"00/only@cas@"}, .state = NsState::Creating, .incarnation = UInt128{0xb1},
        .creator = CreatorFence{.server_root_id = "never-mounted", .writer_epoch = 1, .fence_generation = 1}});
    EXPECT_TRUE(runFsck(*store, /*detail*/ false).catalog_has_no_walkable_row);
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

/// Hooks that run at the first `_ckpt` read of a walked life, standing for GC erasing the row of a dropped
/// table and the namespace janitor deleting the life's `_ckpt`, both after the catalog cut.
namespace
{
void eraseCatalogRow(Backend & backend, const Layout & layout, const RootNamespace & ns)
{
    OperationForTest op(backend);
    CasRefCatalog::Snapshot snapshot = CasRefCatalog::read(*op, layout);
    const auto it = std::find_if(snapshot.catalog.entries.begin(), snapshot.catalog.entries.end(),
        [&](const CatalogEntry & entry) { return entry.ns == ns; });
    ASSERT_NE(it, snapshot.catalog.entries.end());
    snapshot.catalog.entries.erase(it);
    ASSERT_TRUE(snapshot.etag.has_value());
    ASSERT_TRUE(std::holds_alternative<Committed>(
        (*op).replace(layout.refCatalogKey(), encodeRefCatalog(snapshot.catalog), *snapshot.etag, Retry::standard())));
}

void removeCheckpoint(Backend & backend, const Layout & layout, const NamespaceLifeId & life)
{
    OperationForTest op(backend);
    const auto head = (*op).head(layout.refCkptKey(life), Retry::once());
    ASSERT_TRUE(head.has_value());
    ASSERT_EQ((*op).remove(layout.refCkptKey(life), head->etag, Retry::once()), Removal::Removed);
}
}

/// The walk's `_ckpt` read finds the key deleted and the catalog row erased since the cut. The life is
/// janitor-pending, and no `CORRUPTED_DATA` exception is constructed: its destructor would log an error.
TEST(CASFsck, CheckpointDeletedWithTheRowErasedAfterTheCutIsJanitorPendingAndLogsNothing)
{
    auto backend = std::make_shared<MutateOnFirstGetBackend>();
    auto store = openPoolForTest(backend);
    const Layout & layout = store->layout();
    const RootNamespace ns{"00/ckpt_gone_row_erased@cas@"};
    const NamespaceLifeId life = writeFsckFixtureWithTail(*backend, layout, ns);
    backend->armOnFirstGet(layout.refCkptKey(life), [&]
    {
        eraseCatalogRow(*backend, layout, ns);
        removeCheckpoint(*backend, layout, life);
    });

    ScopedLogCapture capture("ForcedCriticalErrorsLogger", "error");
    const FsckReport report = runFsck(*store, /*detail=*/true);

    EXPECT_EQ(report.namespace_janitor_pending_lives, 1u) << "the hook ran and the life is janitor-pending";
    EXPECT_EQ(report.unchecked, 0u);
    EXPECT_EQ(report.chain_broken, 0u);
    EXPECT_EQ(capture.count("CORRUPTED_DATA"), 0u) << capture.captured();
}

/// Control: with the row still named at confirmation, a Live life without `_ckpt` is reported.
TEST(CASFsck, CheckpointDeletedWhileTheRowStaysIsReportedUnchecked)
{
    auto backend = std::make_shared<MutateOnFirstGetBackend>();
    auto store = openPoolForTest(backend);
    const Layout & layout = store->layout();
    const RootNamespace ns{"00/ckpt_gone_row_stays@cas@"};
    const NamespaceLifeId life = writeFsckFixtureWithTail(*backend, layout, ns);
    backend->armOnFirstGet(layout.refCkptKey(life), [&] { removeCheckpoint(*backend, layout, life); });

    const FsckReport report = runFsck(*store, /*detail=*/true);

    EXPECT_EQ(report.namespace_janitor_pending_lives, 0u);
    EXPECT_EQ(report.unchecked, 1u);
    EXPECT_EQ(countVerdictsAtKey(report, layout.namespaceStreamPrefix(life), FsckClass::Unchecked), 1u);
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
            EXPECT_NE(o.reachable_from[0].find("`pending` row"), String::npos);
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

/// A GC round commits between fsck's read of `gc/state` and its manifest pass: it folds the drop and deletes the
/// manifest body, so the blob of the older snapshot names an absent manifest. That is the round at work, not a
/// stale edge.
TEST(CASFsck, EdgeFoldedByARoundDuringTheManifestPassIsSupersededNotStale)
{
    auto backend = std::make_shared<MutateOnFirstGetBackend>();
    auto store = openPoolForTest(backend);
    const RootNamespace ns{"00/aa@cas@"};
    const ManifestRef r = ref(1, 0xA1);
    writeBlobBody(*backend, store->layout(), DB::UInt128(1));
    const ManifestId id = writeManifestRaw(*backend, store->layout(), ns, r, {blobEntryFor("a", DB::UInt128(1))});
    const uint64_t publish_sequence = publishCommittedTransition(*backend, store->layout(), ns, "tbl", std::nullopt, r);
    writeFsckCheckpoint(*backend, store->layout(), ns, RefTxnId{1, publish_sequence});
    Gc gc(store, hexToU128("00000000000000000000000000000001"));
    gc.runRegularRound();
    const uint64_t drop_sequence = dropRefTransition(*backend, store->layout(), ns, "tbl", r);
    writeFsckCheckpoint(*backend, store->layout(), ns, RefTxnId{1, drop_sequence});
    bool round_ran = false;
    backend->armOnFirstGet(store->layout().manifestKey(id), [&]
    {
        gc.runRegularRound();
        round_ran = true;
    });

    const FsckReport rep = runFsck(*store, /*detail*/true);

    ASSERT_TRUE(round_ran) << "the manifest pass read the body, so the round ran inside the scan";
    EXPECT_EQ(rep.stale_edge, 0u);
    EXPECT_EQ(rep.stale_edge_superseded, 1u);
    EXPECT_TRUE(rep.clean());
    for (const FsckObject & o : rep.objects)
        EXPECT_NE(o.cls, FsckClass::StaleEdge) << o.key;
}

/// Control: with no round in between, the same blob is a stale edge and the superseded count stays zero.
TEST(CASFsck, EdgeStillHeldInTheRereadStateIsReportedStale)
{
    auto backend = std::make_shared<InMemoryBackend>();
    auto store = openPoolForTest(backend);
    const RootNamespace ns{"00/aa@cas@"};
    const ManifestRef r = ref(1, 0xA1);
    writeBlobBody(*backend, store->layout(), DB::UInt128(1));
    const ManifestId id = writeManifestRaw(*backend, store->layout(), ns, r, {blobEntryFor("a", DB::UInt128(1))});
    const uint64_t publish_sequence = publishCommittedTransition(*backend, store->layout(), ns, "tbl", std::nullopt, r);
    writeFsckCheckpoint(*backend, store->layout(), ns, RefTxnId{1, publish_sequence});
    Gc gc(store, hexToU128("00000000000000000000000000000001"));
    gc.runRegularRound();
    const uint64_t drop_sequence = dropRefTransition(*backend, store->layout(), ns, "tbl", r);
    writeFsckCheckpoint(*backend, store->layout(), ns, RefTxnId{1, drop_sequence});
    deleteManifestBody(*backend, store->layout(), id);
    gc.runRegularRound();   /// a round that cannot fold the edge: its manifest is gone

    const FsckReport rep = runFsck(*store, /*detail*/true);

    EXPECT_EQ(rep.stale_edge, 1u);
    EXPECT_EQ(rep.stale_edge_superseded, 0u);
    EXPECT_FALSE(rep.clean());
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

/// A `gc/state` below header 4 in a generation 4 pool comes from an older GC; every round of this build refuses it, so
/// the scan cannot read the GC snapshot through it.
TEST(CASFsck, GcStateBelowGeneration4IsUnread)
{
    for (const uint32_t header : {G_REF_CHAIN, G_BLOB_INTENTS})
    {
        SCOPED_TRACE(header);
        auto backend = std::make_shared<InMemoryBackend>();
        auto store = openPoolForTest(backend);
        writeBlobBody(*backend, store->layout(), DB::UInt128(5));
        GcState state;
        state.header_generation = header;
        state.gc_shards = store->poolConfig().gc_shards;
        OperationForTest op(*backend);
        ASSERT_TRUE(std::holds_alternative<Committed>((*op).create(store->layout().gcStateKey(), encodeGcState(state), Retry::standard())));

        const FsckReport rep = runFsck(*store, /*detail*/true);
        if (header == G_BLOB_INTENTS)
        {
            EXPECT_TRUE(rep.clean()) << "positive control: the same state at header 4 is read";
            EXPECT_EQ(rep.intent_unread, 0u);
            continue;
        }
        EXPECT_FALSE(rep.clean());
        EXPECT_EQ(rep.intent_unread, 1u);
        EXPECT_NE(rep.intent_unread_reason.find("gc/state is at generation 3"), String::npos) << rep.intent_unread_reason;
    }
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
    std::set<String> timed_out_reads;
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
        if (timed_out_reads.contains(key))
            throw Poco::TimeoutException("injected timeout of the read of " + key);
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

    /// Runs `hook` before each stream of a key that contains `fragment`, except the streams the hook itself opens.
    void beforeEachStream(const String & fragment, std::function<void(const String &)> hook)
    {
        std::lock_guard lock(hook_mutex);
        stream_fragment = fragment;
        stream_hook = std::move(hook);
    }

    std::unique_ptr<DB::ReadBuffer> stream(const String & key, TransportAccess & access) override
    {
        std::function<void(const String &)> hook;
        {
            std::lock_guard lock(hook_mutex);
            if (stream_hook && !in_stream_hook && key.find(stream_fragment) != String::npos)
            {
                hook = stream_hook;
                in_stream_hook = true;
            }
        }
        if (hook)
        {
            SCOPE_EXIT({
                std::lock_guard lock(hook_mutex);
                in_stream_hook = false;
            });
            hook(key);
        }
        return CountingBackend::stream(key, access);
    }

private:
    std::mutex hook_mutex;
    std::map<String, std::pair<size_t, std::function<void()>>> read_hooks;
    std::map<String, std::function<void()>> head_hooks;
    String stream_fragment;
    std::function<void(const String &)> stream_hook;
    bool in_stream_hook = false;
};

/// A scan deadline `timeout` from now on the pool's request clock.
ToolDeadline fsckDeadlineIn(Pool & store, std::chrono::milliseconds timeout)
{
    return ToolDeadline(store.openRequests(), timeout, "fsck", "raise --timeout");
}

/// Moves the virtual clock just past `deadline`.
void advancePast(VirtualRetryClock & clock, const ToolDeadline & deadline)
{
    clock.advance(deadline.remainingMs() + 1);
}

/// Moves the virtual clock until at most `ms` are left before `deadline`.
void advanceUntilLeft(VirtualRetryClock & clock, const ToolDeadline & deadline, uint64_t ms)
{
    if (deadline.remainingMs() > ms)
        clock.advance(deadline.remainingMs() - ms);
}

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
    EXPECT_EQ(rep.intents_listed, 4u) << "the key whose bytes are junk parses, so it is listed as well";
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
    EXPECT_EQ(rep.intents_listed, 3u) << "positive control: the well-formed keys were counted";
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

        EXPECT_EQ(rep.intents_listed, 3u) << "positive control: intents were listed";
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

/// The fix removes markers on pool threads with a concurrency above 1, and on the scanning thread by default.
TEST(CASFsck, FixConcurrencyMovesTheMarkerRemovalOffTheScanningThread)
{
    for (const size_t concurrency : {size_t{1}, size_t{4}})
    {
        SCOPED_TRACE(concurrency);
        FsckIntentFixture f;
        std::mutex mutex;
        std::set<std::thread::id> threads;
        for (const uint64_t b : {1, 2, 3, 4})
        {
            f.marker(b);
            f.backend->beforeHead(f.markerKey(b), [&]
            {
                std::lock_guard lock(mutex);
                threads.insert(std::this_thread::get_id());
            });
        }
        const FsckReport report = runFsck(*f.store, false, {}, {}, false, {}, /*fix_legacy_markers=*/ true, false, concurrency);
        EXPECT_EQ(report.legacy_markers_deleted, 4u) << "positive control: the fix ran";
        ASSERT_FALSE(threads.empty());
        EXPECT_EQ(threads.contains(std::this_thread::get_id()), concurrency == 1);
    }
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
        (void)removeLegacyMarkers(*f.store, keys, *makeBoundedPool(1), fsckDeadlineIn(*f.store, std::chrono::milliseconds(0)),
                                  [&](uint64_t n) { deleted = n; });
    });
    EXPECT_EQ(deleted, 0u);
    EXPECT_TRUE(objectPresent(*f.backend, f.markerKey(1)));
    EXPECT_TRUE(objectPresent(*f.backend, f.markerKey(2)));

    EXPECT_EQ(removeLegacyMarkers(*f.store, keys, *makeBoundedPool(1), fsckDeadlineIn(*f.store, std::chrono::hours(1)),
                                  [&](uint64_t n) { deleted = n; }).deleted, 2u) << "positive control: with time left the fix deletes";
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
        const auto clock = VirtualRetryClock::installOn(f.store);
        const ToolDeadline deadline = fsckDeadlineIn(*f.store, std::chrono::seconds(1));
        /// The slow `HEAD` moves the pool's virtual clock past the deadline.
        f.backend->beforeHead(keys[slow], [clock, &deadline] { advancePast(*clock, deadline); });
        f.backend->clearJournal();
        uint64_t reported = 99;

        expectThrowsCodeWithMessage(DB::ErrorCodes::TIMEOUT_EXCEEDED, fmt::format("{} legacy marker(s) deleted before", slow), [&]
        {
            (void)removeLegacyMarkers(*f.store, keys, *makeBoundedPool(1), deadline, [&](uint64_t n) { reported = n; });
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

namespace
{

struct FsckProgressEvent
{
    FsckProgressStep step;
    String phase;
    String unit;
    uint64_t done;
    double seconds;
};

using FsckProgressLog = std::vector<FsckProgressEvent>;

FsckProgress recordInto(FsckProgressLog & log)
{
    return [&log](FsckProgressStep step, std::string_view phase, std::string_view unit, uint64_t done, double seconds)
    {
        log.push_back({step, String(phase), String(unit), done, seconds});
    };
}

std::vector<String> startsOf(const FsckProgressLog & log)
{
    std::vector<String> names;
    for (const FsckProgressEvent & e : log)
        if (e.step == FsckProgressStep::Start)
            names.push_back(e.phase);
    return names;
}

std::vector<uint64_t> ticksOf(const FsckProgressLog & log, std::string_view phase)
{
    std::vector<uint64_t> done;
    for (const FsckProgressEvent & e : log)
        if (e.step == FsckProgressStep::Tick && e.phase == phase)
            done.push_back(e.done);
    return done;
}

size_t countOf(const FsckProgressLog & log, std::string_view phase, FsckProgressStep step)
{
    return static_cast<size_t>(std::count_if(log.begin(), log.end(),
        [&](const FsckProgressEvent & e) { return e.phase == phase && e.step == step; }));
}

/// The `End` of `phase`, or null when it has none.
const FsckProgressEvent * endOf(const FsckProgressLog & log, std::string_view phase)
{
    for (const FsckProgressEvent & e : log)
        if (e.phase == phase && e.step == FsckProgressStep::End)
            return &e;
    return nullptr;
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
    FsckProgressLog log;

    const FsckReport rep = runFsck(*f.store, false, recordInto(log), {}, false, {}, /*fix_legacy_markers=*/ true);

    ASSERT_EQ(rep.intents_listed, 1000u) << "positive control: the loops had keys to walk";
    ASSERT_EQ(rep.legacy_markers_deleted, 1000u);
    EXPECT_EQ(ticksOf(log, "counting blob intents"), std::vector<uint64_t>{1000});
    EXPECT_EQ(ticksOf(log, "removing legacy markers"), std::vector<uint64_t>{1000});
}

/// The marker count is cumulative, so a partial last batch ticks the running total and the end repeats it.
TEST(CASFsck, MarkerRemovalTicksTheCumulativeCount)
{
    FsckIntentFixture f;
    for (uint64_t b = 1; b <= 1001; ++b)
        f.marker(b);
    FsckProgressLog log;

    const FsckReport rep = runFsck(*f.store, false, recordInto(log), {}, false, {}, /*fix_legacy_markers=*/ true);

    ASSERT_EQ(rep.legacy_markers_deleted, 1001u);
    EXPECT_EQ((ticksOf(log, "removing legacy markers")), (std::vector<uint64_t>{1000, 1001}));
    const FsckProgressEvent * end = endOf(log, "removing legacy markers");
    ASSERT_NE(end, nullptr);
    EXPECT_EQ(end->done, 1001u);
    EXPECT_EQ(end->unit, "markers");
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

/// A scoped scan lists no intents, so verifying them over it would verify nothing: it is refused.
TEST(CASFsck, VerifyIntentsNeedsAWholePoolScan)
{
    FsckIntentFixture f;
    f.body(1);
    f.currentIntent(1);
    expectThrowsCode(DB::ErrorCodes::BAD_ARGUMENTS, [&]
    {
        (void)runFsck(*f.store, false, {}, {}, false, "00", /*fix_legacy_markers=*/ false, /*verify_intents=*/ true);
    });
    EXPECT_NO_THROW((void)runFsck(*f.store, false, {}, {}, false, "00", /*fix_legacy_markers=*/ false, /*verify_intents=*/ false))
        << "positive control: the scoped scan alone runs";
    EXPECT_TRUE(runFsck(*f.store, false, {}, {}, false, "", false, /*verify_intents=*/ true).intents_verified)
        << "positive control: the whole-pool scan verifies";
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
    EXPECT_EQ(plain.intents_listed + plain.intent_malformed, listed) << "a plain scan accounts for every listed intent too";
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
    ManifestId manifest_id;

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
        manifest_id = writeManifestRaw(*backend, store->layout(), ns, manifest, entries);
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
    EXPECT_TRUE(anyNoteContains(rep, FsckClass::PendingGc, "`pending` row"));
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
    const auto clock = VirtualRetryClock::installOn(f.store);
    const ToolDeadline deadline = fsckDeadlineIn(*f.store, std::chrono::seconds(1));
    /// The first body `HEAD` moves the pool's virtual clock past the deadline.
    for (const uint64_t b : blobs)
        f.backend->beforeHead(f.bodyKey(b), [clock, &deadline] { advancePast(*clock, deadline); });
    f.backend->clearJournal();

    expectThrowsCodeWithMessage(DB::ErrorCodes::TIMEOUT_EXCEEDED, "during 'head-checking retired bodies'", [&]
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
    const auto clock = VirtualRetryClock::installOn(f.store);
    const ToolDeadline deadline = fsckDeadlineIn(*f.store, std::chrono::seconds(1));
    for (const uint64_t b : blobs)
        f.backend->beforeRead(f.intentKey(b), 0, [clock, &deadline] { advancePast(*clock, deadline); });
    f.backend->clearJournal();

    expectThrowsCodeWithMessage(DB::ErrorCodes::TIMEOUT_EXCEEDED, "during 'head-checking retired bodies'", [&]
    {
        (void)runFsck(*f.store, /*detail=*/false, {}, deadline);
    });
    EXPECT_EQ(bodyHeads(f, blobs), 1u) << "the second row's body HEAD was sent after the deadline";
}

/// A pending row whose body is already gone is checked after the bodies loop; a deadline passing there names that phase.
TEST(CASFsck, DeadlinePassingInThePendingRowsPhaseNamesIt)
{
    FsckGcFixture f(/*create_budget=*/0, {1, 2});
    f.drop();
    f.round();
    /// The intent key is derived from the live body, so it is taken before the body goes.
    const std::vector<String> intents{f.intentKey(1), f.intentKey(2)};
    f.removeBody(1);
    f.removeBody(2);
    const auto clock = VirtualRetryClock::installOn(f.store);
    const ToolDeadline deadline = fsckDeadlineIn(*f.store, std::chrono::seconds(1));
    for (const String & key : intents)
        f.backend->beforeRead(key, 0, [clock, &deadline] { advancePast(*clock, deadline); });
    f.backend->clearJournal();

    expectThrowsCodeWithMessage(DB::ErrorCodes::TIMEOUT_EXCEEDED, "during 'checking pending rows'", [&]
    {
        (void)runFsck(*f.store, /*detail=*/false, {}, deadline);
    });
    size_t intent_reads = 0;
    for (const String & key : intents)
        intent_reads += countKeyRequests(f.backend->journal(), JournalVerb::Read, key);
    EXPECT_EQ(intent_reads, 1u) << "positive control: the first row was checked, the second was stopped";
}

/// A deadline that passes during the intent read stops the scan before the body `HEAD` that follows an absent intent.
TEST(CASFsck, DeadlinePassingDuringAnAbsentIntentReadStopsBeforeTheBodyHead)
{
    FsckGcFixture f(/*create_budget=*/0, {1});
    f.drop();
    f.round();
    const String intent_key = f.intentKey(1);
    f.removeIntent(1);
    const auto clock = VirtualRetryClock::installOn(f.store);
    const ToolDeadline deadline = fsckDeadlineIn(*f.store, std::chrono::seconds(1));
    f.backend->beforeRead(intent_key, 0, [clock, &deadline] { advancePast(*clock, deadline); });
    f.backend->clearJournal();

    expectThrowsCodeWithMessage(DB::ErrorCodes::TIMEOUT_EXCEEDED, "during 'head-checking retired bodies'", [&]
    {
        (void)runFsck(*f.store, /*detail=*/false, {}, deadline);
    });
    EXPECT_EQ(bodyHeads(f, {1}), 1u) << "only the retired-body HEAD precedes the deadline; the pending check sent another";
}

/// A summary-only scan counts intents straight from the listing; a deadline refusing that listing names the counting phase.
TEST(CASFsck, DeadlinePassingBeforeTheSummaryIntentListingNamesTheCountingPhase)
{
    FsckIntentFixture f;
    f.staleIntent(1);
    const auto clock = VirtualRetryClock::installOn(f.store);
    const ToolDeadline deadline = fsckDeadlineIn(*f.store, std::chrono::seconds(1));
    bool counting_started = false;
    const FsckProgress passes_deadline = [&, clock](FsckProgressStep step, std::string_view phase, std::string_view, uint64_t, double)
    {
        if (step == FsckProgressStep::Start && phase == "counting blob intents")
        {
            counting_started = true;
            advancePast(*clock, deadline);
        }
    };

    expectThrowsCodeWithMessage(DB::ErrorCodes::TIMEOUT_EXCEEDED, "during 'counting blob intents'", [&]
    {
        (void)runFsck(*f.store, /*detail=*/false, passes_deadline, deadline);
    });
    EXPECT_TRUE(counting_started) << "positive control: the counting phase started";
}

/// With intent verification, a deadline that passes during an intent's read stops the scan before that intent's body
/// `HEAD`.
TEST(CASFsck, DeadlinePassingDuringAVerifiedIntentReadStopsBeforeTheBodyHead)
{
    FsckIntentFixture f;
    f.body(1);
    const String intent = f.currentIntent(1);
    const auto clock = VirtualRetryClock::installOn(f.store);
    const ToolDeadline deadline = fsckDeadlineIn(*f.store, std::chrono::seconds(1));
    f.backend->beforeRead(intent, 0, [clock, &deadline] { advancePast(*clock, deadline); });
    f.backend->clearJournal();

    expectThrowsCodeWithMessage(DB::ErrorCodes::TIMEOUT_EXCEEDED, "during 'verifying blob intents'", [&]
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

/// The request engine reserves this much per attempt, so it refuses to start one in the last `kFsckEnvelopeMs` before
/// a policy's deadline.
constexpr uint64_t kFsckEnvelopeMs = 400;

class EnvelopeFsckFaultBackend : public FsckFaultBackend
{
public:
    uint64_t attemptEnvelopeMs() const override { return kFsckEnvelopeMs; }
};

class EnvelopeMutateOnFirstGetBackend : public MutateOnFirstGetBackend
{
public:
    uint64_t attemptEnvelopeMs() const override { return kFsckEnvelopeMs; }
};

bool anyNoteContains(const FsckReport & report, const String & text)
{
    return std::any_of(report.objects.begin(), report.objects.end(), [&](const FsckObject & object)
    {
        return std::any_of(object.reachable_from.begin(), object.reachable_from.end(),
            [&](const String & note) { return note.find(text) != String::npos; });
    });
}

}

/// A verified intent read that ends with less time left than one attempt ends the scan as partial: the body `HEAD` the
/// engine would refuse is not counted as an unread intent.
TEST(CASFsckPartial, TimeLeftShorterThanOneAttemptEndsIntentVerificationAsPartial)
{
    auto backend = std::make_shared<EnvelopeFsckFaultBackend>();
    PoolPtr store = openPoolForTest(backend);
    const Layout & layout = store->layout();
    writeBlobBody(*backend, layout, numberedBlobHash(1));
    plantIntentForCurrentBody(*backend, layout, blobRefOf(1));
    const String intent = *intentKeyOfCurrentBody(*backend, layout, blobRefOf(1));
    const String body = layout.blobKey(blobRefOf(1));
    const auto clock = VirtualRetryClock::installOn(store);
    const ToolDeadline deadline = fsckDeadlineIn(*store, std::chrono::seconds(2));
    backend->beforeRead(intent, 0, [clock, &deadline] { advanceUntilLeft(*clock, deadline, kFsckEnvelopeMs / 2); });
    backend->clearJournal();

    const FsckReport report = runFsck(*store, /*detail=*/false, {}, deadline, /*partial_on_deadline=*/true, {},
                                      /*fix_legacy_markers=*/false, /*verify_intents=*/true);

    ASSERT_TRUE(report.partial) << formatFsckSummary(report);
    EXPECT_NE(report.partial_reason.find("exceeded the deadline during 'verifying blob intents'"), String::npos)
        << report.partial_reason;
    EXPECT_EQ(report.intent_unread, 0u) << formatFsckSummary(report);
    const auto journal = backend->journal();
    EXPECT_EQ(countKeyRequests(journal, JournalVerb::Read, intent), 1u) << "positive control: the intent was read";
    EXPECT_EQ(countKeyRequests(journal, JournalVerb::Head, body), 0u);
}

/// A read that fails when less time is left than one attempt ends the scan as partial: the engine cannot reissue it, and
/// the failure is the scan's timeout, not an unread intent.
TEST(CASFsckPartial, AReadFailingWithLessThanOneAttemptLeftEndsTheScanAsPartial)
{
    auto backend = std::make_shared<EnvelopeFsckFaultBackend>();
    PoolPtr store = openPoolForTest(backend);
    const Layout & layout = store->layout();
    writeBlobBody(*backend, layout, numberedBlobHash(1));
    plantIntentForCurrentBody(*backend, layout, blobRefOf(1));
    const String intent = *intentKeyOfCurrentBody(*backend, layout, blobRefOf(1));
    const auto clock = VirtualRetryClock::installOn(store);
    const ToolDeadline deadline = fsckDeadlineIn(*store, std::chrono::seconds(2));
    backend->timed_out_reads.insert(intent);
    backend->beforeRead(intent, 0, [clock, &deadline] { advanceUntilLeft(*clock, deadline, kFsckEnvelopeMs / 2); });
    backend->clearJournal();

    const FsckReport report = runFsck(*store, /*detail=*/false, {}, deadline, /*partial_on_deadline=*/true, {},
                                      /*fix_legacy_markers=*/false, /*verify_intents=*/true);

    ASSERT_TRUE(report.partial) << formatFsckSummary(report);
    EXPECT_NE(report.partial_reason.find("exceeded the deadline during 'verifying blob intents'"), String::npos)
        << report.partial_reason;
    EXPECT_EQ(report.intent_unread, 0u) << formatFsckSummary(report);
    EXPECT_EQ(countKeyRequests(backend->journal(), JournalVerb::Read, intent), 1u) << "positive control: the intent read was sent";
}

/// A ref stream read that ends with less time left than one attempt ends the scan as partial, not as a namespace fsck
/// could not examine.
TEST(CASFsckPartial, TimeLeftShorterThanOneAttemptEndsTheRefWalkAsPartial)
{
    auto backend = std::make_shared<EnvelopeMutateOnFirstGetBackend>();
    PoolPtr store = openPoolForTest(backend);
    const Layout & layout = store->layout();
    const RootNamespace ns{"00/envelope_deadline@cas@"};
    const NamespaceLifeId life = writeFsckFixtureWithTail(*backend, layout, ns);
    const auto clock = VirtualRetryClock::installOn(store);
    const ToolDeadline deadline = fsckDeadlineIn(*store, std::chrono::seconds(2));
    bool spun = false;
    backend->armOnFirstGet(layout.refLogKey(life, RefTxnId{1, 2}), [&]
    {
        advanceUntilLeft(*clock, deadline, kFsckEnvelopeMs / 2);
        spun = true;
    });

    const FsckReport report = runFsck(*store, /*detail=*/true, {}, deadline, /*partial_on_deadline=*/true);

    ASSERT_TRUE(spun) << "positive control: the walk read the tail log";
    ASSERT_TRUE(report.partial) << formatFsckSummary(report);
    EXPECT_NE(report.partial_reason.find("exceeded the deadline during 'walking refs'"), String::npos) << report.partial_reason;
    EXPECT_EQ(report.unchecked, 0u) << formatFsckSummary(report);
    EXPECT_FALSE(anyNoteContains(report, "could not")) << formatFsckSummary(report);
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

namespace
{

std::vector<RunRef> adoptedRunsOf(FsckGcFixture & f)
{
    OperationForTest op(*f.backend);
    const GcState state = decodeGcState((*op).read(f.store->layout().gcStateKey(), Retry::standard())->bytes);
    const auto seal = (*op).read(f.store->layout().foldSealKey(state.snap_generation, state.snap_attempt), Retry::standard());
    return decodeFoldSeal(seal->bytes, state.snap_generation).blob_target_runs;
}

/// What a busy pool does to the run a reader is about to open: a round folds a change to blob `b` into a newer
/// generation, and the retention of a later round deletes the run.
void pruneUnderTheReader(FsckGcFixture & f, uint64_t b, const String & run_key)
{
    const Layout & layout = f.store->layout();
    const ManifestRef manifest = ref(1, 0xB0 + b);
    const String name = "churn" + std::to_string(b);
    writeManifestRaw(*f.backend, layout, f.ns, manifest, {blobEntryFor("f" + std::to_string(b), numberedBlobHash(b))});
    publishCommittedTransition(*f.backend, layout, f.ns, name, std::nullopt, manifest);
    writeFsckCheckpoint(*f.backend, layout, f.ns, RefTxnId{1, dropRefTransition(*f.backend, layout, f.ns, name, manifest)});
    f.round();
    if (objectPresent(*f.backend, run_key))
        removeFsckFixtureKey(*f.backend, run_key);
}

/// The counts a scan takes from the GC snapshot.
std::vector<uint64_t> snapshotCountsOf(const FsckReport & rep)
{
    return {rep.pending_gc, rep.awaiting_gc, rep.unaccounted, rep.stale_edge, rep.corrupted_runs, rep.intent_unread,
            rep.pending_without_intent, rep.pending_intent_absent, rep.pending_token_rejected, rep.standing_present};
}

std::set<String> previewedKeys(const Gc::DeletePreview & preview)
{
    std::set<String> keys;
    for (const Gc::PreviewEntry & e : preview.deletes)
        keys.insert(e.reason + " " + e.key);
    return keys;
}

}

/// A round prunes the run the scan is about to read after `gc/state` moved on: the scan reads the newer snapshot and
/// counts what a scan of that snapshot counts.
TEST(CASFsck, RunPrunedUnderAMovedStateIsReadFromTheNewState)
{
    FsckGcFixture f;
    writeBlobBody(*f.backend, f.store->layout(), numberedBlobHash(2));
    const std::vector<RunRef> old_runs = adoptedRunsOf(f);
    ASSERT_EQ(old_runs.size(), 1u);
    size_t pruned = 0;
    f.backend->beforeEachStream("/blob_target/", [&](const String & key)
    {
        if (pruned++ == 0)
            pruneUnderTheReader(f, 2, key);
    });
    f.backend->clearJournal();

    std::optional<FsckReport> scanned;
    {
        ScopedLogCapture forced("ForcedCriticalErrorsLogger", "error");
        scanned = runFsck(*f.store, /*detail=*/true);
        EXPECT_EQ(forced.prioritiesOf("is absent").size(), 0u) << forced.captured();
    }
    const FsckReport & rep = *scanned;

    ASSERT_GE(pruned, 1u) << "the scan never read a run";
    const std::vector<RunRef> new_runs = adoptedRunsOf(f);
    ASSERT_EQ(new_runs.size(), 1u);
    ASSERT_NE(new_runs.front().key, old_runs.front().key) << "positive control: the round wrote a newer run";
    ASSERT_FALSE(objectPresent(*f.backend, old_runs.front().key)) << "positive control: the old run is gone";
    EXPECT_EQ(rep.corrupted_runs, 0u) << formatFsckSummary(rep);
    EXPECT_EQ(rep.intent_unread, 0u) << rep.intent_unread_reason;
    EXPECT_GE(countKeyRequests(f.backend->journal(), JournalVerb::Stream, new_runs.front().key), 1u);
    EXPECT_EQ(countVerdictsAtKey(rep, f.bodyKey(2), FsckClass::Unaccounted), 0u) << "only the newer snapshot knows blob 2";
    f.backend->beforeEachStream("/blob_target/", {});
    const FsckReport settled = runFsck(*f.store, /*detail=*/true);
    EXPECT_EQ(snapshotCountsOf(rep), snapshotCountsOf(settled)) << formatFsckSummary(rep) << "\n" << formatFsckSummary(settled);
}

/// `gc/state` moves before every read of a run: the snapshot is unread, which is a finding, and no run is corrupt.
TEST(CASFsck, StateMovingAtEveryRunReadLeavesTheSnapshotUnread)
{
    FsckGcFixture f;
    f.drop();
    f.round();
    for (uint64_t b = 2; b < 2 + kAdoptedSnapshotReadAttempts + 1; ++b)
        writeBlobBody(*f.backend, f.store->layout(), numberedBlobHash(b));
    size_t pruned = 0;
    f.backend->beforeEachStream("/blob_target/", [&](const String & key)
    {
        pruneUnderTheReader(f, 2 + pruned, key);
        ++pruned;
    });

    const FsckReport rep = runFsck(*f.store, /*detail=*/true);

    EXPECT_EQ(pruned, kAdoptedSnapshotReadAttempts);
    EXPECT_EQ(rep.corrupted_runs, 0u) << formatFsckSummary(rep);
    EXPECT_EQ(rep.intent_unread, 1u) << formatFsckSummary(rep);
    EXPECT_NE(rep.intent_unread_reason.find("gc/state moved"), String::npos) << rep.intent_unread_reason;
    EXPECT_FALSE(rep.clean());
    EXPECT_EQ(rep.unaccounted, 0u) << "a blob the unread snapshot cannot speak for is no anomaly";
    EXPECT_EQ(countVerdictsAtKey(rep, f.bodyKey(2), FsckClass::Unchecked), 1u);
}

/// The dry run of a pool whose round prunes the run being read lists the rows of the newer snapshot.
TEST(CASGcPreview, RunPrunedUnderAMovedStateIsPreviewedFromTheNewState)
{
    FsckGcFixture f;
    f.drop();
    f.round();
    writeBlobBody(*f.backend, f.store->layout(), numberedBlobHash(2));
    size_t pruned = 0;
    f.backend->beforeEachStream("/blob_target/", [&](const String & key)
    {
        if (pruned++ == 0)
            pruneUnderTheReader(f, 2, key);
    });

    std::optional<Gc::DeletePreview> previewed;
    {
        ScopedLogCapture forced("ForcedCriticalErrorsLogger", "error");
        previewed = f.gc->previewDeletes();
        EXPECT_EQ(forced.prioritiesOf("is absent").size(), 0u) << forced.captured();
    }
    const Gc::DeletePreview & racing = *previewed;

    ASSERT_GE(pruned, 1u) << "the preview never read a run";
    EXPECT_TRUE(racing.refused_shards.empty()) << racing.refused_shards.front();
    EXPECT_TRUE(racing.unread.empty()) << racing.unread;
    EXPECT_EQ(racing.snapshot, Gc::SnapshotVerdict::Adopted);
    f.backend->beforeEachStream("/blob_target/", {});
    const Gc::DeletePreview settled = f.gc->previewDeletes();
    EXPECT_TRUE(previewedKeys(settled).contains("pending " + f.bodyKey(2))) << "positive control: blob 2 is condemned";
    EXPECT_EQ(previewedKeys(racing), previewedKeys(settled));
}

/// A run absent under an unchanged `gc/state` is a refused shard named by today's text, and nothing logs it.
TEST(CASGcPreview, RunAbsentUnderAnUnchangedStateIsARefusedShard)
{
    FsckGcFixture f;
    f.drop();
    f.round();
    const std::vector<RunRef> runs = adoptedRunsOf(f);
    ASSERT_EQ(runs.size(), 1u);
    removeFsckFixtureKey(*f.backend, runs.front().key);

    std::optional<Gc::DeletePreview> previewed;
    {
        ScopedLogCapture forced("ForcedCriticalErrorsLogger", "error");
        previewed = f.gc->previewDeletes();
        EXPECT_EQ(forced.prioritiesOf("is absent").size(), 0u) << forced.captured();
    }

    ASSERT_EQ(previewed->refused_shards.size(), 1u);
    EXPECT_EQ(previewed->refused_shards.front(), "CAS source-edge run: object " + runs.front().key + " is absent");
    EXPECT_TRUE(previewed->unread.empty()) << previewed->unread;
}

/// `gc/state` moves before every read of a run: the dry run is unread, not a refused shard.
TEST(CASGcPreview, StateMovingAtEveryRunReadLeavesThePreviewUnread)
{
    FsckGcFixture f;
    f.drop();
    f.round();
    for (uint64_t b = 2; b < 2 + kAdoptedSnapshotReadAttempts + 1; ++b)
        writeBlobBody(*f.backend, f.store->layout(), numberedBlobHash(b));
    size_t pruned = 0;
    f.backend->beforeEachStream("/blob_target/", [&](const String & key)
    {
        pruneUnderTheReader(f, 2 + pruned, key);
        ++pruned;
    });

    const Gc::DeletePreview preview = f.gc->previewDeletes();

    EXPECT_EQ(pruned, kAdoptedSnapshotReadAttempts);
    EXPECT_TRUE(preview.refused_shards.empty()) << preview.refused_shards.front();
    EXPECT_TRUE(preview.deletes.empty());
    EXPECT_NE(preview.unread.find("gc/state moved"), String::npos) << preview.unread;
    EXPECT_EQ(preview.snapshot, Gc::SnapshotVerdict::Unread);
}

/// No `gc/state`, and a generation-0 `gc/state` with no seal, are a pool with no baseline: nothing was adopted.
TEST(CASGcPreview, NoStateAndGenerationZeroWithoutSealAreNoBaseline)
{
    FsckIntentFixture f;
    Gc gc(f.store, DB::UInt128(1));
    const auto expect_no_baseline = [&](const Gc::DeletePreview & preview)
    {
        EXPECT_EQ(preview.snapshot, Gc::SnapshotVerdict::NoBaseline);
        EXPECT_TRUE(preview.deletes.empty());
        EXPECT_TRUE(preview.refused_shards.empty());
        EXPECT_TRUE(preview.unread.empty()) << preview.unread;
        EXPECT_TRUE(preview.missing_seal.empty()) << preview.missing_seal;
    };
    expect_no_baseline(gc.previewDeletes());

    GcState state;
    state.header_generation = G_BLOB_INTENTS;
    state.gc_shards = f.store->poolConfig().gc_shards;
    OperationForTest op(*f.backend);
    ASSERT_TRUE(std::holds_alternative<Committed>((*op).create(f.layout.gcStateKey(), encodeGcState(state), Retry::standard())));
    expect_no_baseline(gc.previewDeletes());
}

/// A read snapshot is `Adopted` also when it holds no row to delete.
TEST(CASGcPreview, ReadSnapshotIsAdoptedAlsoWhenEmpty)
{
    FsckGcFixture f;
    const Gc::DeletePreview live = f.gc->previewDeletes();
    EXPECT_EQ(live.snapshot, Gc::SnapshotVerdict::Adopted);
    EXPECT_TRUE(live.deletes.empty());

    f.drop();
    f.round();
    const Gc::DeletePreview dropped = f.gc->previewDeletes();
    EXPECT_EQ(dropped.snapshot, Gc::SnapshotVerdict::Adopted);
    EXPECT_TRUE(previewedKeys(dropped).contains("pending " + f.bodyKey(1))) << "positive control: the dropped blob is condemned";
}

/// The seal `gc/state` names is absent and `gc/state` did not move: damage, confirmed by one `HEAD` of `gc/state`.
TEST(CASGcPreview, MissingSealUnderUnchangedStateIsReported)
{
    FsckGcFixture f;
    f.drop();
    f.round();
    const String seal_key = adoptedSealKeyOf(f);
    removeFsckFixtureKey(*f.backend, seal_key);
    f.backend->clearJournal();

    const Gc::DeletePreview preview = f.gc->previewDeletes();

    EXPECT_EQ(preview.snapshot, Gc::SnapshotVerdict::SealMissing);
    EXPECT_TRUE(preview.deletes.empty());
    EXPECT_TRUE(preview.unread.empty()) << preview.unread;
    EXPECT_TRUE(preview.refused_shards.empty());
    EXPECT_NE(preview.missing_seal.find(seal_key), String::npos) << preview.missing_seal;
    EXPECT_NE(preview.missing_seal.find("generation"), String::npos) << preview.missing_seal;
    EXPECT_EQ(countKeyRequests(f.backend->journal(), JournalVerb::Head, f.store->layout().gcStateKey()), 1u);
}

/// The confirming `HEAD` of `gc/state` fails: the error propagates, no verdict is returned.
TEST(CASGcPreview, ConfirmationHeadErrorPropagates)
{
    FsckGcFixture f;
    f.drop();
    f.round();
    removeFsckFixtureKey(*f.backend, adoptedSealKeyOf(f));
    f.backend->refused_heads.insert(f.store->layout().gcStateKey());

    EXPECT_THROW(static_cast<void>(f.gc->previewDeletes()), DB::Exception);
}

/// A round prunes the seal being read and moves `gc/state`: the preview reads the seal of the new state.
TEST(CASGcPreview, MissingSealPrunedUnderAMovedStateIsReadFromTheNewState)
{
    FsckGcFixture f;
    f.drop();
    f.round();
    writeBlobBody(*f.backend, f.store->layout(), numberedBlobHash(2));
    const String old_seal = adoptedSealKeyOf(f);
    String new_seal;
    bool fired = false;
    f.backend->beforeRead(old_seal, 0, [&]
    {
        fired = true;
        pruneUnderTheReader(f, 2, old_seal);
        new_seal = adoptedSealKeyOf(f);
    });
    f.backend->clearJournal();

    const Gc::DeletePreview preview = f.gc->previewDeletes();

    ASSERT_TRUE(fired) << "the preview never read the seal";
    ASSERT_NE(new_seal, old_seal) << "positive control: the round moved gc/state";
    ASSERT_FALSE(objectPresent(*f.backend, old_seal)) << "positive control: the old seal is gone";
    EXPECT_EQ(preview.snapshot, Gc::SnapshotVerdict::Adopted);
    EXPECT_TRUE(preview.missing_seal.empty()) << preview.missing_seal;
    EXPECT_GE(countKeyRequests(f.backend->journal(), JournalVerb::Read, new_seal), 1u);
}

/// `gc/state` moves before every read of the seal and the seal just read is gone each time: unread, not damaged.
TEST(CASGcPreview, MissingSealUnderAStateMovingAtEveryReadIsUnread)
{
    FsckGcFixture f;
    f.drop();
    f.round();
    for (uint64_t b = 2; b < 2 + kAdoptedSnapshotReadAttempts + 1; ++b)
        writeBlobBody(*f.backend, f.store->layout(), numberedBlobHash(b));
    size_t fired = 0;
    std::function<void(const String &)> arm = [&](const String & seal)
    {
        f.backend->beforeRead(seal, 0, [&, seal]
        {
            pruneUnderTheReader(f, 2 + fired, seal);
            ++fired;
            const String next = adoptedSealKeyOf(f);
            ASSERT_NE(next, seal) << "the round did not move gc/state";
            arm(next);
        });
    };
    arm(adoptedSealKeyOf(f));

    const Gc::DeletePreview preview = f.gc->previewDeletes();

    EXPECT_EQ(fired, kAdoptedSnapshotReadAttempts);
    EXPECT_EQ(preview.snapshot, Gc::SnapshotVerdict::Unread);
    EXPECT_NE(preview.unread.find("gc/state moved"), String::npos) << preview.unread;
    EXPECT_TRUE(preview.missing_seal.empty()) << preview.missing_seal;
    EXPECT_TRUE(preview.deletes.empty());
}

/// `gc/state` vanishes together with the seal while the preview confirms: unread, never "no baseline".
TEST(CASGcPreview, StateVanishingWhileConfirmingIsUnread)
{
    FsckGcFixture f;
    f.drop();
    f.round();
    const String seal = adoptedSealKeyOf(f);
    bool fired = false;
    f.backend->beforeRead(seal, 0, [&]
    {
        fired = true;
        removeFsckFixtureKey(*f.backend, seal);
        removeFsckFixtureKey(*f.backend, f.store->layout().gcStateKey());
    });

    const Gc::DeletePreview preview = f.gc->previewDeletes();

    ASSERT_TRUE(fired) << "the preview never read the seal";
    EXPECT_EQ(preview.snapshot, Gc::SnapshotVerdict::Unread);
    EXPECT_NE(preview.unread.find("vanished"), String::npos) << preview.unread;
    EXPECT_TRUE(preview.deletes.empty());
}

namespace
{
/// Arms `f` so that the next read of `gc/state` finds a fresh generation-zero state: a new GC leader's first write.
void recreateGenerationZeroStateOnNextRead(FsckGcFixture & f)
{
    f.backend->beforeRead(f.store->layout().gcStateKey(), 0, [&f]
    {
        GcState fresh;
        fresh.header_generation = G_BLOB_INTENTS;
        fresh.gc_shards = f.store->poolConfig().gc_shards;
        OperationForTest op(*f.backend);
        ASSERT_TRUE(std::holds_alternative<Committed>((*op).create(f.store->layout().gcStateKey(), encodeGcState(fresh), Retry::standard())));
    });
}
}

/// `gc/state` vanishes and a fresh generation-zero state appears before the next read: this dry run saw a live state
/// disappear, so the result is unread, never "no baseline".
TEST(CASGcPreview, StateVanishingAndRecreatedAfterAMissingSealIsUnread)
{
    FsckGcFixture f;
    f.drop();
    f.round();
    const String seal = adoptedSealKeyOf(f);
    bool fired = false;
    f.backend->beforeRead(seal, 0, [&]
    {
        fired = true;
        removeFsckFixtureKey(*f.backend, seal);
        removeFsckFixtureKey(*f.backend, f.store->layout().gcStateKey());
        recreateGenerationZeroStateOnNextRead(f);
    });

    const Gc::DeletePreview preview = f.gc->previewDeletes();

    ASSERT_TRUE(fired) << "the preview never read the seal";
    EXPECT_EQ(preview.snapshot, Gc::SnapshotVerdict::Unread);
    EXPECT_NE(preview.unread.find("vanished"), String::npos) << preview.unread;
    EXPECT_TRUE(preview.deletes.empty());
}

/// Same schedule with a refused shard instead of a missing seal.
TEST(CASGcPreview, StateVanishingAndRecreatedAfterARefusedShardIsUnread)
{
    FsckGcFixture f;
    f.drop();
    f.round();
    bool fired = false;
    f.backend->beforeEachStream("/blob_target/", [&](const String & key)
    {
        if (fired)
            return;
        fired = true;
        removeFsckFixtureKey(*f.backend, key);
        removeFsckFixtureKey(*f.backend, f.store->layout().gcStateKey());
        recreateGenerationZeroStateOnNextRead(f);
    });

    const Gc::DeletePreview preview = f.gc->previewDeletes();

    ASSERT_TRUE(fired) << "the preview never read a run";
    EXPECT_EQ(preview.snapshot, Gc::SnapshotVerdict::Unread);
    EXPECT_NE(preview.unread.find("vanished"), String::npos) << preview.unread;
    EXPECT_TRUE(preview.refused_shards.empty());
}

namespace
{
/// Replaces `gc/state` with a fresh generation-zero state at once: a new GC leader's first write landing between two requests.
void replaceStateWithGenerationZero(FsckGcFixture & f)
{
    removeFsckFixtureKey(*f.backend, f.store->layout().gcStateKey());
    GcState fresh;
    fresh.header_generation = G_BLOB_INTENTS;
    fresh.gc_shards = f.store->poolConfig().gc_shards;
    OperationForTest op(*f.backend);
    ASSERT_TRUE(std::holds_alternative<Committed>((*op).create(f.store->layout().gcStateKey(), encodeGcState(fresh), Retry::standard())));
}
}

/// A fresh generation-zero `gc/state` appears before the confirming `HEAD`, so the `HEAD` sees a present, different
/// token: the preview already read a positive generation, so a lower one is unread, never "no baseline".
TEST(CASGcPreview, StateRecreatedAtGenerationZeroBeforeTheConfirmationIsUnread)
{
    FsckGcFixture f;
    f.drop();
    f.round();
    const String seal = adoptedSealKeyOf(f);
    bool fired = false;
    f.backend->beforeRead(seal, 0, [&]
    {
        fired = true;
        removeFsckFixtureKey(*f.backend, seal);
        f.backend->beforeHead(f.store->layout().gcStateKey(), [&] { replaceStateWithGenerationZero(f); });
    });

    const Gc::DeletePreview preview = f.gc->previewDeletes();

    ASSERT_TRUE(fired) << "the preview never read the seal";
    EXPECT_EQ(preview.snapshot, Gc::SnapshotVerdict::Unread);
    EXPECT_NE(preview.unread.find("generation"), String::npos) << preview.unread;
    EXPECT_TRUE(preview.deletes.empty());
}

/// Same schedule with a refused shard instead of a missing seal.
TEST(CASGcPreview, StateRecreatedAtGenerationZeroBeforeTheRefusalConfirmationIsUnread)
{
    FsckGcFixture f;
    f.drop();
    f.round();
    bool fired = false;
    f.backend->beforeEachStream("/blob_target/", [&](const String & key)
    {
        if (fired)
            return;
        fired = true;
        removeFsckFixtureKey(*f.backend, key);
        f.backend->beforeHead(f.store->layout().gcStateKey(), [&] { replaceStateWithGenerationZero(f); });
    });

    const Gc::DeletePreview preview = f.gc->previewDeletes();

    ASSERT_TRUE(fired) << "the preview never read a run";
    EXPECT_EQ(preview.snapshot, Gc::SnapshotVerdict::Unread);
    EXPECT_NE(preview.unread.find("generation"), String::npos) << preview.unread;
    EXPECT_TRUE(preview.refused_shards.empty());
}

/// Control: a seal that is present but undecodable keeps its own `CORRUPTED_DATA`; only absence is labelled `SealMissing`.
TEST(CASGcPreview, UndecodableSealStillPropagates)
{
    FsckGcFixture f;
    f.drop();
    f.round();
    const String seal_key = adoptedSealKeyOf(f);
    {
        OperationForTest op(*f.backend);
        const auto current = (*op).read(seal_key, Retry::standard());
        ASSERT_TRUE(current.has_value());
        ASSERT_TRUE(std::holds_alternative<Committed>((*op).replace(seal_key, "truncated", current->etag, Retry::once())));
    }

    try
    {
        static_cast<void>(f.gc->previewDeletes());
        FAIL() << "an undecodable seal did not throw";
    }
    catch (const DB::Exception & e)
    {
        EXPECT_EQ(e.code(), DB::ErrorCodes::CORRUPTED_DATA) << e.message();
        EXPECT_NE(e.message().find("line without terminator"), String::npos) << e.message();
    }
}

/// A run absent under an unchanged `gc/state` is one finding; the blobs of its shard are not examined, while the other
/// shard's blobs are classified as usual.
TEST(CASFsck, BlobsOfARefusedShardAreNotExamined)
{
    constexpr uint64_t shards = 2;
    uint64_t in_shard[shards] = {0, 0};
    for (uint64_t b = 1; !in_shard[0] || !in_shard[1]; ++b)
        if (!in_shard[blobShard(blobRefOf(b), shards)])
            in_shard[blobShard(blobRefOf(b), shards)] = b;
    FsckGcFixture f(/*create_budget=*/0, {in_shard[0], in_shard[1]}, shards);
    f.drop();
    f.round();
    std::vector<String> removed;
    for (const RunRef & run : adoptedRunsOf(f))
        if (run.shard == 1)
        {
            removeFsckFixtureKey(*f.backend, run.key);
            removed.push_back(run.key);
        }
    ASSERT_FALSE(removed.empty());

    std::optional<FsckReport> scanned;
    {
        ScopedLogCapture forced("ForcedCriticalErrorsLogger", "error");
        scanned = runFsck(*f.store, /*detail=*/true);
        EXPECT_EQ(forced.prioritiesOf("is absent").size(), 0u) << forced.captured();
    }
    const FsckReport & rep = *scanned;

    EXPECT_EQ(rep.corrupted_runs, 1u) << formatFsckSummary(rep);
    const auto corrupted = std::find_if(rep.objects.begin(), rep.objects.end(),
        [&](const FsckObject & object) { return object.key == removed.front() && object.cls == FsckClass::CorruptedRun; });
    ASSERT_NE(corrupted, rep.objects.end()) << formatFsckSummary(rep);
    EXPECT_EQ(corrupted->reachable_from, std::vector<String>{"CAS source-edge run: object " + removed.front() + " is absent"});
    EXPECT_EQ(rep.unaccounted, 0u) << formatFsckSummary(rep);
    EXPECT_EQ(countVerdictsAtKey(rep, f.bodyKey(in_shard[1]), FsckClass::Unchecked), 1u);
    EXPECT_EQ(countVerdictsAtKey(rep, f.bodyKey(in_shard[0]), FsckClass::PendingGc), 1u) << "positive control: shard 0 was read";
}

namespace
{
const RootNamespace kBaitNs{"00/bb@cas@"};

/// Committed refs over numbered blobs on a pool whose GC rounds the test runs itself.
struct StaleEdgeFixture
{
    std::shared_ptr<MutateOnFirstGetBackend> backend = std::make_shared<MutateOnFirstGetBackend>();
    PoolPtr store;
    Gc gc;
    uint64_t next_ref = 1;

    explicit StaleEdgeFixture(uint64_t gc_shards = 1)
        : store(Pool::open(backend, PoolConfig{.pool_prefix = "p", .server_root_id = "test", .gc_shards = gc_shards}))
        , gc(store, hexToU128("00000000000000000000000000000001"))
    {
    }
    struct Published
    {
        RootNamespace ns;
        ManifestRef manifest;
        ManifestId id;
    };
    std::map<String, Published> refs;

    const Layout & layout() const { return store->layout(); }
    String bodyKey(uint64_t b) const { return layout().blobKey(blobRefOf(b)); }
    String manifestKeyOf(const String & name) const { return layout().manifestKey(refs.at(name).id); }

    void checkpointThrough(const RootNamespace & ns, uint64_t sequence)
    {
        writeFsckCheckpoint(*backend, layout(), ns, RefTxnId{1, sequence});
    }

    /// A namespace holding a real stale edge keeps GC from folding anything beside it, so a round that has to fold
    /// another ref's drop needs a namespace of its own.
    void publish(const String & name, std::initializer_list<uint64_t> blobs, const RootNamespace & ns = RootNamespace{"00/aa@cas@"})
    {
        std::vector<ManifestEntry> entries;
        for (const uint64_t b : blobs)
        {
            if (!objectPresent(*backend, bodyKey(b)))
                writeBlobBody(*backend, layout(), numberedBlobHash(b));
            entries.push_back(blobEntryFor("f" + std::to_string(b), numberedBlobHash(b)));
        }
        const ManifestRef manifest = ref(next_ref, 0xA0 + next_ref);
        ++next_ref;
        const ManifestId id = writeManifestRaw(*backend, layout(), ns, manifest, entries);
        checkpointThrough(ns, publishCommittedTransition(*backend, layout(), ns, name, std::nullopt, manifest));
        refs[name] = {ns, manifest, id};
    }

    void drop(const String & name)
    {
        const Published & p = refs.at(name);
        checkpointThrough(p.ns, dropRefTransition(*backend, layout(), p.ns, name, p.manifest));
    }

    void removeManifestBody(const String & name) { deleteManifestBody(*backend, layout(), refs.at(name).id); }

    /// Runs `mutation` at the first read of the named manifest's body: the scan's manifest pass, after its first `gc/state` read.
    void armDuringManifestPass(const String & name, std::function<void()> mutation)
    {
        backend->armOnFirstGet(manifestKeyOf(name), std::move(mutation));
    }

    String gcStateBytes() const
    {
        OperationForTest op(*backend);
        return (*op).read(layout().gcStateKey(), Retry::standard())->bytes;
    }

    /// A round, with proof that it committed a new `gc/state`.
    void roundThatMovesTheState()
    {
        const String before = gcStateBytes();
        gc.runRegularRound();
        ASSERT_NE(before, gcStateBytes()) << "the round did not commit";
    }

    /// Removes the runs the adopted seal names, of every shard or of `shard` only, and returns their keys.
    std::vector<String> removeAdoptedRuns(std::optional<uint64_t> shard = {})
    {
        const GcState state = decodeGcState(gcStateBytes());
        OperationForTest op(*backend);
        const auto seal = (*op).read(layout().foldSealKey(state.snap_generation, state.snap_attempt), Retry::standard());
        std::vector<String> removed;
        for (const RunRef & run : decodeFoldSeal(seal->bytes, state.snap_generation).blob_target_runs)
            if (!shard || run.shard == *shard)
            {
                removeFsckFixtureKey(*backend, run.key);
                removed.push_back(run.key);
            }
        return removed;
    }

    /// Replaces `gc/state` with a fresh one at snapshot generation zero, as a state recreated from nothing.
    void recreateGcStateAtGenerationZero()
    {
        OperationForTest op(*backend);
        const auto current = (*op).read(layout().gcStateKey(), Retry::standard());
        GcState fresh;
        fresh.gc_shards = decodeGcState(current->bytes).gc_shards;
        ASSERT_TRUE(std::holds_alternative<Committed>(
            (*op).replace(layout().gcStateKey(), encodeGcState(fresh), current->etag, Retry::once())));
    }
};

/// The first blob numbers from 1 up that fall in each of `shards` GC shards.
std::vector<uint64_t> firstBlobOfEachShard(uint64_t shards)
{
    std::vector<uint64_t> in_shard(shards, 0);
    for (uint64_t b = 1, found = 0; found < shards; ++b)
        if (!in_shard[blobShard(blobRefOf(b), shards)])
        {
            in_shard[blobShard(blobRefOf(b), shards)] = b;
            ++found;
        }
    return in_shard;
}
}

/// A round that commits between the scan's two `gc/state` reads cannot fold an edge whose manifest body is
/// already gone: the edge is still held by the second read and is reported.
TEST(CASFsck, RealStaleEdgeSurvivesARoundBetweenTheTwoReads)
{
    StaleEdgeFixture f;
    f.publish("live", {2});
    f.publish("stale", {1});
    f.publish("bait", {2}, kBaitNs);   /// shares blob 2 with `live`, so its drop leaves no candidate of its own
    f.gc.runRegularRound();
    f.drop("stale");
    f.drop("bait");
    f.removeManifestBody("stale");
    bool round_ran = false;
    f.armDuringManifestPass("bait", [&]
    {
        f.roundThatMovesTheState();
        round_ran = true;
    });

    const FsckReport rep = runFsck(*f.store, /*detail=*/true);

    ASSERT_TRUE(round_ran) << "the manifest pass read the bait body";
    EXPECT_EQ(rep.stale_edge, 1u);
    EXPECT_EQ(rep.stale_edge_superseded, 0u);
    EXPECT_EQ(countVerdictsAtKey(rep, f.bodyKey(1), FsckClass::StaleEdge), 1u);
    EXPECT_FALSE(rep.clean());
}

/// Of two candidates only the one whose edge the round folded is superseded; the other is reported. A round holds
/// back body deletes while a stale edge exists, so the test removes the folded ref's body itself.
TEST(CASFsck, OnlyTheCandidateTheRoundFoldedIsSuperseded)
{
    StaleEdgeFixture f;
    f.publish("live", {2});
    f.publish("stale", {1});
    f.publish("bait", {3}, kBaitNs);
    f.gc.runRegularRound();
    f.drop("stale");
    f.drop("bait");
    f.removeManifestBody("stale");
    bool round_ran = false;
    f.armDuringManifestPass("bait", [&]
    {
        f.roundThatMovesTheState();
        f.removeManifestBody("bait");
        round_ran = true;
    });

    const FsckReport rep = runFsck(*f.store, /*detail=*/true);

    ASSERT_TRUE(round_ran);
    EXPECT_EQ(rep.stale_edge, 1u);
    EXPECT_EQ(rep.stale_edge_superseded, 1u);
    EXPECT_EQ(countVerdictsAtKey(rep, f.bodyKey(1), FsckClass::StaleEdge), 1u);
    EXPECT_EQ(countVerdictsAtKey(rep, f.bodyKey(3), FsckClass::StaleEdge), 0u);
}

/// A blob with two source edges, one folded by the round and one not: the held edge is reported, and only it is counted.
TEST(CASFsck, CandidateWithSomeSourcesFoldedIsReportedForTheHeldOnes)
{
    StaleEdgeFixture f;
    f.publish("stale", {1});
    f.publish("bait", {1}, kBaitNs);
    f.gc.runRegularRound();
    f.drop("stale");
    f.drop("bait");
    f.removeManifestBody("stale");
    bool round_ran = false;
    f.armDuringManifestPass("bait", [&]
    {
        f.roundThatMovesTheState();
        f.removeManifestBody("bait");
        round_ran = true;
    });

    const FsckReport rep = runFsck(*f.store, /*detail=*/true);

    ASSERT_TRUE(round_ran);
    EXPECT_EQ(rep.stale_edge, 1u);
    EXPECT_EQ(rep.stale_edge_superseded, 0u);
    EXPECT_TRUE(anyNoteContains(rep, FsckClass::StaleEdge, "all 1 source edges")) << "only the held source is listed";
    EXPECT_FALSE(rep.clean());
}

/// The second read cannot speak for the candidate's shard: whether a round folded the edge is unknown, which is a
/// hard `intent_unread`, not a stale edge and not a superseded one.
TEST(CASFsck, UnreadableRereadOfTheCandidatesShardIsIntentUnread)
{
    StaleEdgeFixture f;
    f.publish("live", {2});
    f.publish("stale", {1});
    f.publish("bait", {3}, kBaitNs);
    f.gc.runRegularRound();
    f.drop("stale");
    f.drop("bait");
    f.removeManifestBody("stale");
    bool runs_removed = false;
    f.armDuringManifestPass("bait", [&]
    {
        f.removeAdoptedRuns();
        runs_removed = true;
    });

    const FsckReport rep = runFsck(*f.store, /*detail=*/true);

    ASSERT_TRUE(runs_removed);
    EXPECT_EQ(rep.stale_edge, 0u);
    EXPECT_EQ(rep.stale_edge_superseded, 0u);
    EXPECT_EQ(rep.intent_unread, 1u);
    EXPECT_EQ(countVerdictsAtKey(rep, f.bodyKey(1), FsckClass::IntentUnread), 1u);
    EXPECT_FALSE(rep.clean());
}

/// A run that stays unreadable in one shard is that shard's own finding; a real stale edge in another shard is still
/// reported.
TEST(CASFsck, StaleEdgeInAReadShardIsReportedBesideARefusedShard)
{
    constexpr uint64_t shards = 2;
    uint64_t in_shard[shards] = {0, 0};
    for (uint64_t b = 1; !in_shard[0] || !in_shard[1]; ++b)
        if (!in_shard[blobShard(blobRefOf(b), shards)])
            in_shard[blobShard(blobRefOf(b), shards)] = b;
    FsckGcFixture f(/*create_budget=*/0, {in_shard[0], in_shard[1]}, shards);
    f.drop();
    deleteManifestBody(*f.backend, f.store->layout(), f.manifest_id);
    for (const RunRef & run : adoptedRunsOf(f))
        if (run.shard == 1)
            removeFsckFixtureKey(*f.backend, run.key);

    const FsckReport rep = runFsck(*f.store, /*detail=*/true);

    EXPECT_EQ(rep.corrupted_runs, 1u) << formatFsckSummary(rep);
    EXPECT_EQ(rep.stale_edge, 1u) << formatFsckSummary(rep);
    EXPECT_EQ(rep.intent_unread, 0u) << formatFsckSummary(rep);
    EXPECT_EQ(countVerdictsAtKey(rep, f.bodyKey(in_shard[0]), FsckClass::StaleEdge), 1u);
    EXPECT_EQ(countVerdictsAtKey(rep, f.bodyKey(in_shard[1]), FsckClass::Unchecked), 1u);
}

/// A deadline that passes during the second read leaves the candidate unconfirmed: a partial report counts no stale
/// edge and shows no row for it.
TEST(CASFsck, DeadlineDuringTheRereadLeavesNoUnconfirmedStaleEdge)
{
    FsckGcFixture f;
    f.drop();
    deleteManifestBody(*f.backend, f.store->layout(), f.manifest_id);
    const FsckReport control = runFsck(*f.store, /*detail=*/true);
    ASSERT_EQ(control.stale_edge, 1u) << "positive control: the edge is stale without a deadline";

    OperationForTest op(*f.backend);
    const GcState state = decodeGcState((*op).read(f.store->layout().gcStateKey(), Retry::standard())->bytes);
    const String seal_key = f.store->layout().foldSealKey(state.snap_generation, state.snap_attempt);
    const auto clock = VirtualRetryClock::installOn(f.store);
    const ToolDeadline deadline = fsckDeadlineIn(*f.store, std::chrono::seconds(1));
    bool fired = false;
    f.backend->beforeRead(seal_key, /*skip=*/1, [&]
    {
        clock->advance(2000);
        fired = true;
    });

    const FsckReport rep = runFsck(*f.store, /*detail=*/true, {}, deadline, /*partial_on_deadline=*/true);

    ASSERT_TRUE(fired) << "the second read of the seal never ran";
    EXPECT_TRUE(rep.partial);
    EXPECT_EQ(rep.stale_edge, 0u);
    EXPECT_EQ(countVerdictsAtKey(rep, f.bodyKey(1), FsckClass::StaleEdge), 0u);
}

/// A round between the two reads folds the candidate in one shard, and a run of the other shard is then missing: the
/// re-read confirms that refusal under a `gc/state` that did not move, so it is a finding of the scan.
TEST(CASFsck, RunRefusalConfirmedByTheRereadIsReported)
{
    const std::vector<uint64_t> in_shard = firstBlobOfEachShard(2);
    StaleEdgeFixture f(/*gc_shards=*/2);
    f.publish("live", {in_shard[1]});
    f.publish("bait", {in_shard[0]}, kBaitNs);
    f.gc.runRegularRound();
    f.drop("bait");
    std::vector<String> removed;
    f.armDuringManifestPass("bait", [&]
    {
        f.roundThatMovesTheState();
        removed = f.removeAdoptedRuns(/*shard=*/1);
    });

    const FsckReport rep = runFsck(*f.store, /*detail=*/true);

    ASSERT_EQ(removed.size(), 1u) << "the round's seal names one run of shard 1";
    EXPECT_EQ(rep.stale_edge_superseded, 1u) << "positive control: the re-read ran and folded the candidate";
    EXPECT_EQ(rep.corrupted_runs, 1u) << formatFsckSummary(rep);
    EXPECT_EQ(countVerdictsAtKey(rep, removed.front(), FsckClass::CorruptedRun), 1u);
    EXPECT_FALSE(rep.clean());
}

/// A run both reads refuse is one finding: counted once and listed once.
TEST(CASFsck, RunRefusedByBothReadsIsCountedOnce)
{
    const std::vector<uint64_t> in_shard = firstBlobOfEachShard(2);
    FsckGcFixture f(/*create_budget=*/0, {in_shard[0], in_shard[1]}, /*gc_shards=*/2);
    f.drop();
    deleteManifestBody(*f.backend, f.store->layout(), f.manifest_id);
    std::vector<String> removed;
    for (const RunRef & run : adoptedRunsOf(f))
        if (run.shard == 1)
        {
            removeFsckFixtureKey(*f.backend, run.key);
            removed.push_back(run.key);
        }
    ASSERT_EQ(removed.size(), 1u);

    const FsckReport rep = runFsck(*f.store, /*detail=*/true);

    EXPECT_EQ(rep.stale_edge, 1u) << "positive control: the stale candidate made the scan re-read the snapshot";
    EXPECT_EQ(rep.corrupted_runs, 1u) << formatFsckSummary(rep);
    EXPECT_EQ(countVerdictsAtKey(rep, removed.front(), FsckClass::CorruptedRun), 1u);
}

/// `gc/state` recreated at generation zero between the two reads holds no edge, yet no round folded the candidate's:
/// the re-read went backwards, so the candidate is inconclusive, not superseded.
TEST(CASFsck, RereadOfARegressedStateLeavesTheCandidateInconclusive)
{
    StaleEdgeFixture f;
    f.publish("live", {2});
    f.publish("stale", {1});
    f.publish("bait", {2}, kBaitNs);
    f.gc.runRegularRound();
    f.drop("stale");
    f.drop("bait");
    f.removeManifestBody("stale");
    bool recreated = false;
    f.armDuringManifestPass("bait", [&]
    {
        f.recreateGcStateAtGenerationZero();
        recreated = true;
    });

    const FsckReport rep = runFsck(*f.store, /*detail=*/true);

    ASSERT_TRUE(recreated) << "the manifest pass read the bait body";
    EXPECT_EQ(rep.stale_edge_superseded, 0u);
    EXPECT_EQ(rep.stale_edge, 0u);
    EXPECT_EQ(rep.intent_unread, 1u) << formatFsckSummary(rep);
    EXPECT_EQ(countVerdictsAtKey(rep, f.bodyKey(1), FsckClass::IntentUnread), 1u);
    EXPECT_FALSE(rep.clean());
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
        EXPECT_EQ(rep.intents_listed, 1u) << "positive control: the real intent was listed";
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

    EXPECT_EQ(rep.intents_listed, 1u) << "only the intent that was read is counted";
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

/// `Creating` rows of one creator root and writer epoch share one read of its mount slot, a failed one
/// too: a slot that keeps failing would otherwise cost a request window per row. Every row is reported.
TEST(CASFsck, CreatingRowsOfOneCreatorShareOneFailedSlotRead)
{
    auto backend = std::make_shared<FsckFaultBackend>();
    PoolPtr store = openPoolForTest(backend);
    const Layout & layout = store->layout();
    CasOperation op = store->openRequests().admit();
    const auto admit_creating = [&](const String & ns, UInt128 incarnation, const String & root)
    {
        CasRefCatalog::casAdmitEntry(op, layout, store->poolConfig().gc_shards, CatalogEntry{
            .ns = RootNamespace{ns}, .state = NsState::Creating, .incarnation = incarnation,
            .creator = CreatorFence{.server_root_id = root, .writer_epoch = 1, .fence_generation = 1}});
    };
    admit_creating("00/a-first@cas@", UInt128{0xc1}, "shared-creator");
    admit_creating("00/b-second@cas@", UInt128{0xc2}, "shared-creator");
    admit_creating("00/c-third@cas@", UInt128{0xc3}, "shared-creator");
    admit_creating("00/d-other@cas@", UInt128{0xc4}, "other-creator");
    backend->refused_reads.insert(layout.mountKey("shared-creator"));
    backend->refused_reads.insert(layout.mountKey("other-creator"));

    FsckReport rep;
    ASSERT_NO_THROW(rep = runFsckReadingCreatorSlots(*store));

    EXPECT_EQ(backend->getCount(layout.mountKey("shared-creator")), 1u) << "three rows of one creator, one slot read";
    EXPECT_EQ(backend->getCount(layout.mountKey("other-creator")), 1u) << "control: another creator costs its own read";
    ASSERT_EQ(rep.creating_rows.size(), 4u);
    EXPECT_EQ(rep.creating_unproven, 4u);
    for (const FsckCreatingRow & row : rep.creating_rows)
    {
        EXPECT_FALSE(row.slot) << row.ns;
        EXPECT_NE(row.slot_read_error.find("injected refusal of the read of " + layout.mountKey(row.creator_root)),
                  String::npos) << row.ns << ": " << row.slot_read_error;
    }
}

/// `SYSTEM CAS FSCK` has no column for `Creating` rows, so its scan reads no creator's mount slot; the scan
/// `cas-fsck` runs reads each one and reports the rows.
TEST(CASFsck, OnlyTheCliScanReadsCreatorSlots)
{
    auto backend = std::make_shared<FsckFaultBackend>();
    PoolPtr store = openPoolForTest(backend);
    const Layout & layout = store->layout();
    CasOperation op = store->openRequests().admit();
    const auto admit_creating = [&](const String & ns, UInt128 incarnation, const String & root)
    {
        CasRefCatalog::casAdmitEntry(op, layout, store->poolConfig().gc_shards, CatalogEntry{
            .ns = RootNamespace{ns}, .state = NsState::Creating, .incarnation = incarnation,
            .creator = CreatorFence{.server_root_id = root, .writer_epoch = 1, .fence_generation = 1}});
        setWatermarkMinActive(*backend, layout, root, /*writer_epoch*/ 1, /*min_active_build_sequence*/ 1);
    };
    admit_creating("00/a@cas@", UInt128{0xd1}, "creator-a");
    admit_creating("00/b@cas@", UInt128{0xd2}, "creator-b");
    const auto slot_reads = [&]
    {
        return backend->getCount(layout.mountKey("creator-a")) + backend->getCount(layout.mountKey("creator-b"));
    };

    const FsckReport sql = runFsck(*store, /*detail*/ false);

    EXPECT_EQ(slot_reads(), 0u);
    EXPECT_TRUE(sql.creating_rows.empty());
    EXPECT_EQ(sql.stalled_creating + sql.creating_unproven, 0u);
    EXPECT_TRUE(sql.catalog_has_no_walkable_row) << "the catalog is still read";

    const FsckReport cli = runFsckReadingCreatorSlots(*store);

    EXPECT_EQ(slot_reads(), 2u) << "one read per creator";
    EXPECT_EQ(cli.creating_rows.size(), 2u);
    EXPECT_EQ(cli.creating_unproven, 2u);
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

namespace
{
/// An object listed under the blobs prefix that `Layout::parseBlobKey` refuses.
String foreignBlobKey(const Layout & layout, std::string_view tail = "md5/aa/aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa")
{
    const String key = layout.blobsPrefix() + String(tail);
    EXPECT_FALSE(layout.parseBlobKey(key).has_value()) << "fixture: the key must be one the parser refuses";
    return key;
}

void plantForeignBody(Backend & backend, const String & key)
{
    OperationForTest op(backend);
    ASSERT_TRUE(std::holds_alternative<Committed>((*op).create(key, "x", Retry::standard())));
}

const FsckObject * rowAt(const FsckReport & report, const String & key)
{
    for (const FsckObject & o : report.objects)
        if (o.key == key)
            return &o;
    return nullptr;
}

/// The scan names the key `Unaccounted` with the fixed note and no other class.
void expectForeignRow(const FsckReport & report, const String & key)
{
    const FsckObject * row = rowAt(report, key);
    ASSERT_NE(row, nullptr) << key;
    EXPECT_EQ(row->cls, FsckClass::Unaccounted) << key;
    EXPECT_EQ(row->size, 1u) << key;
    ASSERT_FALSE(row->reachable_from.empty()) << key;
    EXPECT_NE(row->reachable_from[0].find("Malformed blob key"), String::npos) << row->reachable_from[0];
    EXPECT_EQ(std::count_if(report.objects.begin(), report.objects.end(),
        [&](const FsckObject & o) { return o.key == key; }), 1) << "exactly one row for the key";
}
}

/// An unparsable key beside the zero-byte body, which the all-zero digest keys, is its own `Unaccounted` row and leaves
/// the zero body's verdict alone.
TEST(CASFsck, UnparsableBlobKeyIsNotTheZeroBodyAwaitingGc)
{
    ASSERT_EQ(blobRefOf(0), BlobRef{});
    ASSERT_EQ(idOf(String()), BlobRef{});
    for (const bool detail : {true, false})
    {
        SCOPED_TRACE(detail);
        FsckGcFixture f(0, {0});
        f.drop();
        const String zero_key = f.bodyKey(0);
        const String foreign = foreignBlobKey(f.store->layout());
        const FsckReport before = runFsck(*f.store, detail);
        ASSERT_EQ(before.awaiting_gc, 1u) << "positive control: the zero body awaits GC";

        plantForeignBody(*f.backend, foreign);
        const FsckReport after = runFsck(*f.store, detail);

        EXPECT_EQ(after.unreachable, before.unreachable + 1);
        EXPECT_EQ(after.unaccounted, before.unaccounted + 1);
        EXPECT_EQ(after.awaiting_gc, 1u);
        if (detail)
        {
            expectForeignRow(after, foreign);
            EXPECT_EQ(countVerdictsAtKey(after, zero_key, FsckClass::AwaitingGc), 1u);
        }
    }
}

/// The zero body's own stale edge is hard and is reported once; the foreign key does not repeat it.
TEST(CASFsck, UnparsableBlobKeyIsNotTheZeroBodyStaleEdge)
{
    ASSERT_EQ(blobRefOf(0), BlobRef{});
    for (const bool detail : {true, false})
    {
        SCOPED_TRACE(detail);
        FsckGcFixture f(0, {0});
        f.drop();
        deleteManifestBody(*f.backend, f.store->layout(), f.manifest_id);
        f.round();
        const String zero_key = f.bodyKey(0);
        const String foreign = foreignBlobKey(f.store->layout());
        const FsckReport before = runFsck(*f.store, detail);
        ASSERT_EQ(before.stale_edge, detail ? 1u : 0u);

        plantForeignBody(*f.backend, foreign);
        const FsckReport after = runFsck(*f.store, detail);

        EXPECT_EQ(after.unaccounted, before.unaccounted + 1);
        EXPECT_EQ(after.stale_edge, before.stale_edge);
        EXPECT_EQ(after.stale_edge_superseded, 0u);
        EXPECT_EQ(after.awaiting_gc, before.awaiting_gc);
        if (detail)
        {
            EXPECT_FALSE(after.clean());
            EXPECT_EQ(countVerdictsAtKey(after, zero_key, FsckClass::StaleEdge), 1u) << "the genuine body stays a hard finding";
            expectForeignRow(after, foreign);
        }
    }
}

/// No `gc/state` made every unreferenced body `AwaitingGc`, the foreign key included.
TEST(CASFsck, UnparsableBlobKeyWithNoGcStateIsUnaccounted)
{
    const std::array<String, 3> tails = {
        "md5/aa/aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa",
        "ch128/00/ff" + String(30, '0'),
        "ch128/aa/aabbcc",
    };
    for (const String & tail : tails)
        for (const bool detail : {true, false})
        {
            SCOPED_TRACE(tail);
            SCOPED_TRACE(detail);
            auto backend = std::make_shared<InMemoryBackend>();
            auto store = openPoolForTest(backend);
            writeBlobBody(*backend, store->layout(), numberedBlobHash(5));
            const String foreign = foreignBlobKey(store->layout(), tail);
            plantForeignBody(*backend, foreign);

            const FsckReport rep = runFsck(*store, detail);

            EXPECT_EQ(rep.unreachable, 2u);
            EXPECT_EQ(rep.unaccounted, 1u);
            EXPECT_EQ(rep.awaiting_gc, 1u) << "positive control: body 5 awaits GC";
            if (detail)
            {
                expectForeignRow(rep, foreign);
                EXPECT_EQ(countVerdictsAtKey(rep, store->layout().blobKey(blobRefOf(5)), FsckClass::AwaitingGc), 1u);
            }
        }
}

/// A `gc/state` the scan cannot read leaves real bodies `Unchecked`; a key that is no blob key is not one of them.
TEST(CASFsck, UnparsableBlobKeyUnderAnUnreadSnapshotIsUnaccountedNotUnchecked)
{
    for (const bool detail : {true, false})
    {
        SCOPED_TRACE(detail);
        auto backend = std::make_shared<InMemoryBackend>();
        auto store = openPoolForTest(backend);
        writeBlobBody(*backend, store->layout(), numberedBlobHash(5));
        GcState state;
        state.header_generation = G_REF_CHAIN;
        state.gc_shards = store->poolConfig().gc_shards;
        {
            OperationForTest op(*backend);
            ASSERT_TRUE(std::holds_alternative<Committed>((*op).create(store->layout().gcStateKey(), encodeGcState(state), Retry::standard())));
        }
        const String foreign = foreignBlobKey(store->layout());
        plantForeignBody(*backend, foreign);

        const FsckReport rep = runFsck(*store, detail);

        EXPECT_EQ(rep.unaccounted, 1u);
        EXPECT_EQ(rep.intent_unread, 1u);
        EXPECT_FALSE(rep.clean());
        if (detail)
        {
            expectForeignRow(rep, foreign);
            EXPECT_EQ(countVerdictsAtKey(rep, foreign, FsckClass::Unchecked), 0u);
            EXPECT_EQ(countVerdictsAtKey(rep, store->layout().blobKey(blobRefOf(5)), FsckClass::Unchecked), 1u)
                << "positive control: the real refusal is still reported";
        }
    }
}

/// The retired zero body gets the `HEAD` and the intent read; the foreign key gets neither.
TEST(CASFsck, UnparsableBlobKeyIsNeverLookedUpAgainstARetiredZeroBody)
{
    ASSERT_EQ(blobRefOf(0), BlobRef{});
    for (const bool detail : {true, false})
    {
        SCOPED_TRACE(detail);
        auto backend = std::make_shared<FsckFaultBackend>();
        PoolPtr store = openPoolForTest(backend);
        const Layout & layout = store->layout();
        const BlobRef blob = blobRefOf(0);
        writeBlobBody(*backend, layout, numberedBlobHash(0));
        const String zero_key = layout.blobKey(blob);
        const PersistedEtag token{"emulated", "\"tag-1\""};
        backend->head_values[zero_key] = "tag-1";
        injectRetire(*backend, layout, /*round=*/3, /*shard=*/0, {RetiredEntry{
            .kind = ObjectKind::Blob, .ref = blob, .token = token,
            .size = 1, .condemn_round = 3, .delete_pending = true}});
        const String intent = layout.blobIntentKey(blob, *token.canonical());
        const String foreign = foreignBlobKey(layout);
        const FsckReport before = runFsck(*store, detail);
        ASSERT_EQ(before.pending_without_intent, 1u) << "positive control: the zero body's row has no intent";

        plantForeignBody(*backend, foreign);
        backend->head_values[foreign] = "tag-1";
        backend->clearJournal();
        const FsckReport after = runFsck(*store, detail);

        EXPECT_EQ(countKeyRequests(backend->journal(), JournalVerb::Head, foreign), 0u);
        EXPECT_EQ(countKeyRequests(backend->journal(), JournalVerb::Read, intent), 1u);
        EXPECT_EQ(after.pending_without_intent, 1u);
        EXPECT_EQ(after.unaccounted, before.unaccounted + 1);
        EXPECT_EQ(countVerdictsAtKey(after, zero_key, FsckClass::PendingWithoutIntent), 1u);
        if (detail)
            expectForeignRow(after, foreign);
    }
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

    const ToolDeadline past = fsckDeadlineIn(*store, std::chrono::milliseconds(0));
    /// partial_on_deadline=false keeps the old contract:
    EXPECT_THROW(DB::Cas::runFsck(*store, /*detail=*/false, {}, past), DB::Exception);
    /// partial_on_deadline=true returns a flagged report:
    const auto report = DB::Cas::runFsck(*store, false, {}, past, /*partial_on_deadline=*/true);
    EXPECT_TRUE(report.partial);
    EXPECT_FALSE(report.partial_reason.empty());
}

/// A scan cut short by its deadline has checked only part of the pool, so it cannot be a clean verdict.
TEST(CASFsckPartial, DeadlineWithNoFindingIsNotClean)
{
    auto backend = std::make_shared<InMemoryBackend>();
    auto store = openPoolForTest(backend);
    const RootNamespace ns{"00/aa@cas@"};
    const ManifestRef r = ref(1, 0xAA);
    writeBlobBody(*backend, store->layout(), DB::UInt128(1));
    writeManifestRaw(*backend, store->layout(), ns, r, {blobEntryFor("a", DB::UInt128(1))});
    const uint64_t sequence = publishCommittedTransition(*backend, store->layout(), ns, "tbl", std::nullopt, r);
    writeFsckCheckpoint(*backend, store->layout(), ns, RefTxnId{1, sequence});

    const ToolDeadline past = fsckDeadlineIn(*store, std::chrono::milliseconds(0));
    const auto report = DB::Cas::runFsck(*store, false, {}, past, /*partial_on_deadline=*/true);

    ASSERT_TRUE(report.partial);
    for (const FsckHardFinding & finding : kFsckHardFindings)
        EXPECT_EQ(report.*finding.value, 0u) << finding.name;
    EXPECT_FALSE(report.clean());
}

/// A finding recorded before the cut stays in the report, and the line carries both facts.
TEST(CASFsckPartial, FindingRecordedBeforeTheCutIsReportedAndNotClean)
{
    FsckGcFixture f(/*create_budget=*/0, {1, 2, 3, 4});
    f.drop();
    f.round();
    const String lifeless = f.store->layout().casRefsPrefix() + f.ns.string() + "/_log/"
        + renderRefTxnId(RefTxnId{1, 1}) + ".zst";
    {
        OperationForTest op(*f.backend);
        ASSERT_TRUE(std::holds_alternative<Committed>((*op).create(lifeless, "garbage", Retry::once())));
    }
    const auto clock = VirtualRetryClock::installOn(f.store);
    const ToolDeadline deadline = fsckDeadlineIn(*f.store, std::chrono::seconds(1));
    for (const uint64_t b : {1, 2, 3, 4})
        f.backend->beforeHead(f.bodyKey(b), [clock] { clock->advance(2000); });

    const FsckReport rep = runFsck(*f.store, /*detail=*/true, {}, deadline, /*partial_on_deadline=*/true);

    ASSERT_TRUE(rep.partial);
    EXPECT_NE(rep.partial_reason.find("during 'head-checking retired bodies'"), String::npos) << rep.partial_reason;
    EXPECT_EQ(rep.lifeless_keys, 1u);
    size_t rows = 0;
    for (const FsckObject & o : rep.objects)
        if (o.cls == FsckClass::LifelessKey)
            ++rows;
    EXPECT_EQ(rows, 1u);
    EXPECT_FALSE(rep.clean());
    const String line = formatFsckSummary(rep);
    EXPECT_NE(line.find("lifeless_keys=1"), String::npos) << line;
    EXPECT_NE(line.find("partial=1"), String::npos) << line;
}

/// A deadline that is not reached leaves the report clean: `clean` does not depend on a deadline existing.
TEST(CASFsckPartial, CompleteScanWithADeadlineIsClean)
{
    auto backend = std::make_shared<InMemoryBackend>();
    auto store = openPoolForTest(backend);
    const RootNamespace ns{"00/aa@cas@"};
    const ManifestRef r = ref(1, 0xAA);
    writeBlobBody(*backend, store->layout(), DB::UInt128(1));
    writeManifestRaw(*backend, store->layout(), ns, r, {blobEntryFor("a", DB::UInt128(1))});
    const uint64_t sequence = publishCommittedTransition(*backend, store->layout(), ns, "tbl", std::nullopt, r);
    writeFsckCheckpoint(*backend, store->layout(), ns, RefTxnId{1, sequence});

    const ToolDeadline later = fsckDeadlineIn(*store, std::chrono::hours(1));
    const auto report = DB::Cas::runFsck(*store, false, {}, later, /*partial_on_deadline=*/true);

    EXPECT_FALSE(report.partial);
    EXPECT_TRUE(report.clean());
    EXPECT_EQ(formatFsckSummary(report).find("partial="), String::npos);
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

/// A memory-limit fault on a manifest read is reissued by the fsck walk, so the dangle behind it is
/// still recorded instead of the namespace being skipped as unchecked.
TEST(CASFsckScoped, AMemoryLimitOnACommittedManifestReadStillRecordsTheDangle)
{
    auto backend = std::make_shared<InMemoryBackend>();
    auto store = openPoolForTest(backend);
    const RootNamespace ns{"nsa"};
    const ManifestRef r_a = ref(1, 0xA1);
    const ManifestRef r_other = ref(1, 0xB1);
    /// A valid body that describes `r_other`, stored where the committed ref names `r_a`.
    PartManifest body;
    body.ref = r_other;
    body.root_namespace_id = ns;
    body.payload_digest = computePayloadDigest(body);
    {
        OperationForTest op(*backend);
        ASSERT_TRUE(std::holds_alternative<Committed>((*op).create(
            store->layout().manifestKey(ManifestId{ns, r_a}),
            sealObject(FormatId::PartManifest, encodePartManifest(body)), Retry::standard())));
    }
    const uint64_t sequence = publishCommittedTransition(*backend, store->layout(), ns, "tbl", std::nullopt, r_a);
    writeFsckCheckpoint(*backend, store->layout(), ns, RefTxnId{1, sequence});
    backend->failNextReadWith(store->layout().manifestKey(ManifestId{ns, r_a}), std::make_exception_ptr(
        DB::Exception(DB::ErrorCodes::MEMORY_LIMIT_EXCEEDED, "Query memory limit exceeded")));

    const auto scoped = DB::Cas::runFsck(*store, false, {}, {}, false, /*namespace_prefix=*/"nsa");

    EXPECT_EQ(scoped.dangling, 1u);
    EXPECT_EQ(scoped.unchecked, 0u);
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
    for (const char * name : {"intents_listed=0", "pending_intent_absent=0", "pending_token_rejected=0", "standing_present=0", "intents_verified=0"})
        EXPECT_NE(plain.find(name), String::npos) << name << " missing from: " << plain;
    report.intents_verified = true;
    report.intents_listed = 5;
    report.standing_present = 2;
    const String verified = formatFsckSummary(report);
    EXPECT_NE(verified.find("intents_verified=1"), String::npos) << verified;
    EXPECT_NE(verified.find("intents_listed=5"), String::npos) << verified;
    EXPECT_NE(verified.find("standing_present=2"), String::npos) << verified;
    EXPECT_TRUE(report.clean()) << "the advisory counters never make a report unclean";
}

/// Pins the exact token order and set of a complete clean line, which scripts parse by key.
TEST(CASFsckSummary, CompleteCleanLineIsUnchanged)
{
    EXPECT_EQ(formatFsckSummary(FsckReport{}), 
        "reachable=0 dangling=0 unreachable=0 pending_gc=0 awaiting_gc=0 unaccounted=0 stale_edge=0 stale_edge_superseded=0 corrupted_runs=0 chain_broken=0 lifeless_keys=0 intents_listed=0 intent_current=0 intent_replaced=0 intent_body_gone=0 intent_malformed=0 intent_unread=0 pending_without_intent=0 pending_intent_absent=0 pending_token_rejected=0 standing_present=0 intents_verified=0 legacy_marker=0 legacy_markers_deleted=0 legacy_markers_changed=0 janitor_pending=0 janitor_pending_bytes=0 janitor_pending_lives=0 stalled_creating=0 creating_unproven=0 unchecked=0 ref_records_walked=0 physical_bytes=0 referenced_logical_bytes=0 distinct_blobs=0 total_blob_refs=0 dedup_ratio=0");
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

    const auto clock = VirtualRetryClock::installOn(store);
    const ToolDeadline deadline = fsckDeadlineIn(*store, std::chrono::seconds(2));
    /// Runs out the virtual clock while the first life's walk meets its hole, so the deadline fires
    /// before the second life, after the first life's finding is held.
    backend->armOnFirstGet(hole, [&] { advancePast(*clock, deadline); });

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

    const auto clock = VirtualRetryClock::installOn(store);
    const ToolDeadline deadline = fsckDeadlineIn(*store, std::chrono::seconds(2));
    /// Both blobs are missing. The first is re-resolved and held; the virtual clock runs out before the
    /// second one's recheck, so the deadline fires inside the blob loop with one dangling held.
    backend->blobs_prefix = layout.blobsPrefix();
    backend->on_second_blob_head = [&] { advancePast(*clock, deadline); };

    const FsckReport report = runFsck(*store, /*detail=*/true, {}, deadline, /*partial_on_deadline=*/true);
    ASSERT_TRUE(report.partial);
    EXPECT_EQ(report.dangling, 0u) << "a finding the catalog did not confirm is not reported as corruption";
    EXPECT_EQ(report.unchecked, 1u);
    const String note = "late recheck not confirmed before the deadline";
    EXPECT_NE(uncheckedNoteContains(report, layout.blobKey(first.ref), note),
              uncheckedNoteContains(report, layout.blobKey(second.ref), note))
        << "exactly the first blob of the loop was held";
}

namespace
{

/// One namespace with committed refs `r000` to `r128`, each over a present manifest, and `zz`, whose manifest body is
/// missing: 130 refs, the last of them a real finding.
struct FsckManyRefsPool
{
    std::shared_ptr<InMemoryBackend> backend = std::make_shared<InMemoryBackend>();
    PoolPtr store = openPoolForTest(backend);
    RootNamespace ns{"00/aa@cas@"};

    FsckManyRefsPool()
    {
        const Layout & layout = store->layout();
        writeBlobBody(*backend, layout, numberedBlobHash(1));
        uint64_t sequence = 0;
        for (uint64_t i = 0; i <= 128; ++i)
        {
            const ManifestRef manifest = ref(1, 0x100 + i);
            writeManifestRaw(*backend, layout, ns, manifest, {blobEntryFor("a", numberedBlobHash(1))});
            sequence = publishCommittedTransition(*backend, layout, ns, fmt::format("r{:03}", i), std::nullopt, manifest);
        }
        sequence = publishCommittedTransition(*backend, layout, ns, "zz", std::nullopt, ref(1, 0x1FF));
        writeFsckCheckpoint(*backend, layout, ns, RefTxnId{1, sequence});
    }
};

}

/// Each request-bound phase starts, ends, and counts items of its own unit; a skipped item is not counted.
TEST(CASFsck, ProgressPhasesRunInOrderWithRealUnits)
{
    FsckGcFixture f(0, {1, 2, 3, 4, 5});
    f.drop();
    f.round();
    FsckProgressLog log;

    const FsckReport rep = runFsck(*f.store, /*detail=*/false, recordInto(log), {}, false, {}, false, /*verify_intents=*/true);

    ASSERT_EQ(rep.pending_gc, 5u) << "positive control: five bodies waited in the pipeline";
    const std::vector<String> expected{"listing namespaces", "walking refs", "listing blobs", "head-checking reachable blobs",
        "reading gc snapshot runs", "head-checking retired bodies", "checking pending rows", "listing blob intents",
        "verifying blob intents", "listing manifests"};
    EXPECT_EQ(startsOf(log), expected);
    for (const String & phase : expected)
    {
        EXPECT_EQ(countOf(log, phase, FsckProgressStep::Start), 1u) << phase;
        EXPECT_EQ(countOf(log, phase, FsckProgressStep::End), 1u) << phase;
        double last = 0;
        for (const FsckProgressEvent & e : log)
            if (e.phase == phase)
            {
                EXPECT_GE(e.seconds, last) << phase;
                last = e.seconds;
            }
    }
    ASSERT_NE(endOf(log, "head-checking retired bodies"), nullptr);
    EXPECT_EQ(endOf(log, "head-checking retired bodies")->done, 5u);
    EXPECT_EQ(endOf(log, "head-checking retired bodies")->unit, "bodies");
    ASSERT_NE(endOf(log, "checking pending rows"), nullptr);
    EXPECT_EQ(endOf(log, "checking pending rows")->done, 0u) << "every pending row was checked in the bodies loop";
    ASSERT_NE(endOf(log, "verifying blob intents"), nullptr);
    EXPECT_EQ(endOf(log, "verifying blob intents")->done, 5u);
    EXPECT_EQ(endOf(log, "verifying blob intents")->unit, "keys");
    ASSERT_NE(endOf(log, "reading gc snapshot runs"), nullptr);
    EXPECT_EQ(endOf(log, "reading gc snapshot runs")->unit, "rows");
}

/// A ref whose manifest is missing is counted like any other, so the tick comes at the 64th and 128th ref.
TEST(CASFsck, ProgressRefsTickEverySixtyFourthRef)
{
    FsckManyRefsPool pool;
    FsckProgressLog log;

    const FsckReport rep = runFsck(*pool.store, /*detail=*/true, recordInto(log));

    ASSERT_EQ(rep.dangling, 1u) << "positive control: the ref without a manifest was reached";
    EXPECT_EQ((ticksOf(log, "walking refs")), (std::vector<uint64_t>{64, 128}));
    const FsckProgressEvent * end = endOf(log, "walking refs");
    ASSERT_NE(end, nullptr);
    EXPECT_EQ(end->done, 130u);
    EXPECT_EQ(end->unit, "refs");
}

namespace
{

std::vector<std::tuple<String, FsckClass, std::vector<String>>> objectsOf(const FsckReport & report)
{
    std::vector<std::tuple<String, FsckClass, std::vector<String>>> rows;
    for (const FsckObject & o : report.objects)
        rows.emplace_back(o.key, o.cls, o.reachable_from);
    std::sort(rows.begin(), rows.end());
    return rows;
}

/// A sink that throws `code`: on every call, or only at the ticks of `walking refs`.
FsckProgress throwingSink(int code, bool only_ref_ticks, size_t & calls)
{
    return [code, only_ref_ticks, &calls](FsckProgressStep step, std::string_view phase, std::string_view, uint64_t, double)
    {
        if (only_ref_ticks && !(step == FsckProgressStep::Tick && phase == "walking refs"))
            return;
        ++calls;
        throw DB::Exception(code, "progress sink failure");
    };
}

}

/// A sink that throws changes nothing the checker reports, including a finding located after the throw.
TEST(CASFsck, ThrowingSinkDoesNotChangeTheReport)
{
    FsckManyRefsPool pool_a;
    FsckGcFixture pool_b(0, {1});
    pool_b.drop();
    pool_b.round();
    pool_b.removeIntent(1);

    struct Case
    {
        const char * name;
        Pool * store;
        bool verify;
        bool only_ref_ticks;
    };
    const Case cases[] = {
        {"A, throw at the 64th ref only", pool_a.store.get(), false, true},
        {"A, throw on every call", pool_a.store.get(), false, false},
        {"B, throw on every call", pool_b.store.get(), true, false},
    };
    for (const Case & c : cases)
    {
        for (const int code : {DB::ErrorCodes::CORRUPTED_DATA, DB::ErrorCodes::TIMEOUT_EXCEEDED})
        {
            SCOPED_TRACE(String(c.name) + ", code " + std::to_string(code));
            const FsckReport baseline = runFsck(*c.store, true, {}, {}, /*partial_on_deadline=*/true, {}, false, c.verify);
            size_t calls = 0;
            const FsckReport with_sink
                = runFsck(*c.store, true, throwingSink(code, c.only_ref_ticks, calls), {}, /*partial_on_deadline=*/true, {}, false, c.verify);

            for (const FsckReport * r : {&baseline, &with_sink})
            {
                EXPECT_FALSE(r->clean());
                EXPECT_EQ(r->unchecked, 0u);
                EXPECT_FALSE(r->partial);
            }
            const bool is_a = c.store == pool_a.store.get();
            EXPECT_EQ(baseline.dangling, is_a ? 1u : 0u);
            EXPECT_EQ(with_sink.dangling, is_a ? 1u : 0u);
            EXPECT_EQ(baseline.pending_without_intent, is_a ? 0u : 1u);
            EXPECT_EQ(with_sink.pending_without_intent, is_a ? 0u : 1u);
            EXPECT_GT(calls, 0u) << "the sink was never reached";
            EXPECT_EQ(formatFsckSummary(with_sink), formatFsckSummary(baseline));
            EXPECT_EQ(with_sink.partial_reason, baseline.partial_reason);
            EXPECT_EQ(objectsOf(with_sink), objectsOf(baseline));
        }
    }
}

/// A sink that throws on every call neither stops the marker removal nor changes its result.
TEST(CASFsck, ThrowingSinkDuringMarkerRemovalDoesNotChangeTheReport)
{
    const auto populate = [](FsckIntentFixture & f)
    {
        for (uint64_t b = 1; b <= 1000; ++b)
            f.marker(b);
    };
    FsckIntentFixture plain;
    populate(plain);
    const FsckReport baseline = runFsck(*plain.store, false, {}, {}, false, {}, /*fix_legacy_markers=*/ true);

    FsckIntentFixture f;
    populate(f);
    size_t calls = 0;
    FsckReport rep;
    ASSERT_NO_THROW(rep = runFsck(*f.store, false, throwingSink(DB::ErrorCodes::CORRUPTED_DATA, false, calls), {}, false, {},
                                  /*fix_legacy_markers=*/ true));

    EXPECT_GT(calls, 0u) << "the sink was never reached";
    EXPECT_EQ(rep.legacy_markers_deleted, 1000u);
    EXPECT_EQ(formatFsckSummary(rep), formatFsckSummary(baseline));
}

/// Each line names the unit of its count.
TEST(CASFsck, ProgressLineNamesTheUnit)
{
    EXPECT_EQ(formatFsckProgress(FsckProgressStep::Start, "walking refs", "refs", 0, 0.0), "cas-fsck: walking refs: started");
    EXPECT_EQ(formatFsckProgress(FsckProgressStep::Tick, "walking refs", "refs", 64, 12.5), "cas-fsck: walking refs: 64 refs, 12.5 s");
    EXPECT_EQ(formatFsckProgress(FsckProgressStep::End, "reading gc snapshot runs", "rows", 65536, 3.0),
              "cas-fsck: reading gc snapshot runs: done, 65536 rows, 3.0 s");
}

/// A phase left by the deadline has a start and no end, so the last line of a stopped run names it. The deadline
/// passes on the pool's request clock, which the first body `HEAD` moves.
TEST(CASFsck, ProgressStopsAtTheInterruptedPhase)
{
    FsckGcFixture f(/*create_budget=*/0, {1, 2, 3, 4});
    f.drop();
    f.round();
    const auto clock = VirtualRetryClock::installOn(f.store);
    const ToolDeadline deadline = fsckDeadlineIn(*f.store, std::chrono::seconds(1));
    for (const uint64_t b : {1, 2, 3, 4})
        f.backend->beforeHead(f.bodyKey(b), [clock] { clock->advance(2000); });
    FsckProgressLog log;

    const FsckReport rep = runFsck(*f.store, /*detail=*/false, recordInto(log), deadline, /*partial_on_deadline=*/true);

    ASSERT_TRUE(rep.partial);
    EXPECT_NE(rep.partial_reason.find("during 'head-checking retired bodies'"), String::npos) << rep.partial_reason;
    EXPECT_EQ(countOf(log, "head-checking retired bodies", FsckProgressStep::Start), 1u);
    EXPECT_EQ(countOf(log, "head-checking retired bodies", FsckProgressStep::End), 0u);
    ASSERT_FALSE(log.empty());
    EXPECT_EQ(log.back().phase, "head-checking retired bodies");
    EXPECT_EQ(log.back().step, FsckProgressStep::Start) << "no line follows the interrupted phase's start";
}

/// One progress tick per 16 list pages.
constexpr uint64_t kListTickKeysForTest = 16 * CasOperation::kCasListPageKeys;

/// Listing the manifests for the stale-edge check reports its own progress, in keys, before any manifest is read.
TEST(CASFsck, StaleEdgeManifestListingReportsProgressInKeys)
{
    FsckGcFixture f(/*create_budget=*/0, {1});
    f.drop();
    deleteManifestBody(*f.backend, f.store->layout(), f.manifest_id);
    f.round();   /// a round that cannot fold the edge: its manifest is gone
    {
        OperationForTest op(*f.backend);
        const String prefix = f.store->layout().manifestNamespacePrefix(f.ns);
        for (uint64_t i = 0; i < kListTickKeysForTest + 1; ++i)
            (*op).create(prefix + fmt::format("junk{:06}", i), "x", Retry::standard());
    }
    FsckProgressLog log;

    const FsckReport rep = runFsck(*f.store, /*detail=*/true, recordInto(log));

    ASSERT_EQ(rep.stale_edge, 1u) << "positive control: the stale-edge check ran";
    const String listing = "listing manifests for the stale-edge check";
    EXPECT_EQ(countOf(log, listing, FsckProgressStep::Start), 1u);
    EXPECT_EQ(countOf(log, listing, FsckProgressStep::End), 1u);
    EXPECT_FALSE(ticksOf(log, listing).empty());
    const FsckProgressEvent * end = endOf(log, listing);
    ASSERT_NE(end, nullptr);
    EXPECT_EQ(end->unit, "keys");
    const std::vector<String> started = startsOf(log);
    const auto listing_at = std::find(started.begin(), started.end(), listing);
    const auto reading_at = std::find(started.begin(), started.end(), "reading manifests for the stale-edge check");
    ASSERT_NE(listing_at, started.end());
    EXPECT_TRUE(reading_at == started.end() || listing_at < reading_at);
}

namespace
{
/// Two namespaces, each with live manifests and one dropped ref whose manifest body is gone: a stale-edge candidate in each.
void publishStaleEdgesInTwoNamespaces(StaleEdgeFixture & f)
{
    f.publish("live_a1", {10});
    f.publish("live_a2", {11});
    f.publish("stale_a", {1});
    f.publish("live_b1", {12}, kBaitNs);
    f.publish("live_b2", {13}, kBaitNs);
    f.publish("stale_b", {2}, kBaitNs);
    f.gc.runRegularRound();
    f.drop("stale_a");
    f.drop("stale_b");
    f.removeManifestBody("stale_a");
    f.removeManifestBody("stale_b");
}

/// The Start and End steps of the two stale-edge phases, in emission order.
std::vector<String> staleEdgePhaseSteps(const FsckProgressLog & log)
{
    std::vector<String> steps;
    for (const FsckProgressEvent & e : log)
    {
        const String kind = e.phase == "listing manifests for the stale-edge check" ? "listing"
            : e.phase == "reading manifests for the stale-edge check"               ? "reading"
                                                                                    : "";
        if (!kind.empty() && e.step != FsckProgressStep::Tick)
            steps.push_back(kind + (e.step == FsckProgressStep::Start ? " start" : " end"));
    }
    return steps;
}
}

/// The manifest-read phase of the stale-edge check is scoped to the namespace it reads, like its listing.
TEST(CASFsck, StaleEdgeCheckListsAndReadsManifestsPerNamespace)
{
    StaleEdgeFixture f;
    publishStaleEdgesInTwoNamespaces(f);
    FsckProgressLog log;

    const FsckReport rep = runFsck(*f.store, /*detail=*/true, recordInto(log));

    ASSERT_EQ(rep.stale_edge, 2u) << "positive control: both namespaces held a candidate";
    EXPECT_EQ(staleEdgePhaseSteps(log), (std::vector<String>{"listing start", "listing end", "reading start", "reading end",
                                                             "listing start", "listing end", "reading start", "reading end"}));
}

/// A read that stalls in the second namespace leaves that namespace's read phase as the last line.
TEST(CASFsck, StaleEdgeReadInterruptedInTheSecondNamespaceEndsOnItsReadingStart)
{
    StaleEdgeFixture f;
    publishStaleEdgesInTwoNamespaces(f);
    const auto clock = VirtualRetryClock::installOn(f.store);
    const ToolDeadline deadline = fsckDeadlineIn(*f.store, std::chrono::seconds(1));
    bool fired = false;
    FsckProgressLog log;
    const FsckProgress record = recordInto(log);
    /// The ref walk reads the live manifests too, so the stall is armed at the next read after the second listing starts.
    size_t listings_started = 0;
    const FsckProgress sink = [&](FsckProgressStep step, std::string_view phase, std::string_view unit, uint64_t done, double seconds)
    {
        record(step, phase, unit, done, seconds);
        if (step == FsckProgressStep::Start && phase == "listing manifests for the stale-edge check" && ++listings_started == 2)
            f.backend->armOnFirstGetUnder("", [&]
            {
                fired = true;
                clock->advance(2000);
            });
    };

    const FsckReport rep = runFsck(*f.store, /*detail=*/true, sink, deadline, /*partial_on_deadline=*/true);

    ASSERT_TRUE(fired) << "positive control: the second namespace's manifests were read";
    ASSERT_TRUE(rep.partial);
    ASSERT_FALSE(log.empty());
    EXPECT_EQ(log.back().phase, "reading manifests for the stale-edge check");
    EXPECT_EQ(log.back().step, FsckProgressStep::Start) << "no line follows the interrupted read phase's start";
    EXPECT_EQ(staleEdgePhaseSteps(log), (std::vector<String>{"listing start", "listing end", "reading start", "reading end",
                                                             "listing start", "listing end", "reading start"}));
}

/// A retired body with a `pending` row and no intent costs two `HEAD`s, so the phase counts bodies, not `HEAD`s.
TEST(CASFsck, RetiredBodyWithTwoHeadsIsCountedAsOneBody)
{
    FsckGcFixture f;
    f.drop();
    f.round();
    f.removeIntent(1);
    f.backend->clearJournal();
    FsckProgressLog log;

    const FsckReport rep = runFsck(*f.store, /*detail=*/false, recordInto(log));

    ASSERT_EQ(rep.pending_without_intent, 1u) << "positive control: the body took the pending-row path";
    EXPECT_GE(countKeyRequests(f.backend->journal(), JournalVerb::Head, f.bodyKey(1)), 2u) << "positive control: more than one HEAD";
    const FsckProgressEvent * end = endOf(log, "head-checking retired bodies");
    ASSERT_NE(end, nullptr);
    EXPECT_EQ(formatFsckProgress(end->step, end->phase, end->unit, end->done, 0.0),
              "cas-fsck: head-checking retired bodies: done, 1 bodies, 0.0 s");
}

/// A reachable blob the listing missed costs one `HEAD`; the phase counts it as a blob, as the scoped scan's phase does.
TEST(CASFsck, ReachableBlobHeadCheckCountsBlobs)
{
    FsckGcFixture f;
    f.removeBody(1);
    FsckProgressLog log;

    const FsckReport rep = runFsck(*f.store, /*detail=*/false, recordInto(log));

    ASSERT_EQ(rep.dangling, 1u) << "positive control: the reachable blob was head-checked";
    const FsckProgressEvent * end = endOf(log, "head-checking reachable blobs");
    ASSERT_NE(end, nullptr);
    EXPECT_EQ(formatFsckProgress(end->step, end->phase, end->unit, end->done, 0.0),
              "cas-fsck: head-checking reachable blobs: done, 1 blobs, 0.0 s");
}

/// Bodies the snapshot has not retired are classified without a request and are not counted; a pool GC never ran on
/// counts none.
TEST(CASFsck, BodyClassificationProgressCountsOnlyRetiredBodies)
{
    auto backend = std::make_shared<InMemoryBackend>();
    auto store = openPoolForTest(backend);
    for (uint64_t b = 1; b <= 3; ++b)
        writeBlobBody(*backend, store->layout(), numberedBlobHash(b));
    FsckProgressLog log;

    const FsckReport rep = runFsck(*store, /*detail=*/false, recordInto(log));

    ASSERT_EQ(rep.awaiting_gc, 3u) << "positive control: three bodies were classified";
    const FsckProgressEvent * end = endOf(log, "head-checking retired bodies");
    ASSERT_NE(end, nullptr);
    EXPECT_EQ(end->unit, "bodies");
    EXPECT_EQ(end->done, 0u);
    EXPECT_EQ(formatFsckProgress(end->step, end->phase, end->unit, end->done, 0.0),
              "cas-fsck: head-checking retired bodies: done, 0 bodies, 0.0 s");
}

/// After the first failure of the sink in a phase it is not called again in that phase, except for the end line,
/// and the phase logs one line; a failing end line adds none.
TEST(CASFsck, FailingSinkIsLoggedOncePerPhase)
{
    FsckManyRefsPool pool;
    const FsckReport baseline = runFsck(*pool.store, /*detail=*/true);
    size_t refs_calls = 0;
    FsckProgress sink = [&refs_calls](FsckProgressStep, std::string_view phase, std::string_view, uint64_t, double)
    {
        if (phase != "walking refs")
            return;
        ++refs_calls;
        throw DB::Exception(DB::ErrorCodes::CORRUPTED_DATA, "progress sink failure");
    };
    ScopedLogCapture own("CasFsck", "error");
    ScopedLogCapture forced("ForcedCriticalErrorsLogger", "error");

    const FsckReport rep = runFsck(*pool.store, /*detail=*/true, sink);

    EXPECT_EQ(refs_calls, 2u) << "the start, then the end line once";
    EXPECT_EQ(own.count("progress sink failure"), 1u) << own.captured();
    EXPECT_EQ(forced.count("progress sink failure"), 0u) << forced.captured();
    EXPECT_EQ(formatFsckSummary(rep), formatFsckSummary(baseline));
    EXPECT_EQ(objectsOf(rep), objectsOf(baseline));
}
