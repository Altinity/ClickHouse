#include <gtest/gtest.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasPartWriteTxn.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasPool.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Backend/CasInMemoryBackend.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Formats/CasBlobEnvelopeFormat.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Formats/CasLayout.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Primitives/CasTypes.h>
#include <Disks/tests/cas_test_helpers.h>
#include <Common/Exception.h>
#include <Common/ProfileEvents.h>
#include <new>
#include <vector>

using namespace DB::Cas;
using DB::Cas::tests::CountingBackend;
using DB::Cas::tests::idOf;
using DB::Cas::tests::JournalEntry;
using DB::Cas::tests::JournalVerb;
using DB::Cas::tests::u128Of;
using DB::Cas::tests::intentKeyOfCurrentBody;
using DB::Cas::tests::objectPresent;
using DB::Cas::tests::plantIntentForCurrentBody;
using DB::Cas::tests::blobEntryFor;
using DB::Cas::tests::expectThrowsCode;  // NOLINT(misc-unused-using-decls): only used inside `#ifndef DEBUG_OR_SANITIZER_BUILD` -- unused in a sanitizer build's TU, used in a release build's

namespace ProfileEvents
{
extern const Event CASBlobBodyPutAvoided;
}

namespace DB::ErrorCodes
{
extern const int FILE_DOESNT_EXIST;
extern const int LOGICAL_ERROR;
}

namespace
{

/// Open a Pool over `b`.
PoolPtr openUploadPool(const std::shared_ptr<InMemoryBackend> & b)
{
    return Pool::open(b, PoolConfig{.pool_prefix = "p", .server_root_id = "test"});
}

BlobSource reReadableStagedSource(
    const BackendPtr & backend, const String & staging_key, uint64_t payload_size, uint64_t header_len)
{
    BlobSource source;
    source.size = payload_size;
    source.server_side_copy_from = staging_key;
    source.open = [backend, staging_key, header_len, payload_size]() -> std::unique_ptr<DB::ReadBuffer>
    {
        DB::Cas::tests::OperationForTest op(backend);
        std::unique_ptr<DB::ReadBuffer> staged = (*op).stream(staging_key, Retry::once());
        if (!staged)
            throw DB::Exception(DB::ErrorCodes::FILE_DOESNT_EXIST, "staging object {} is absent", staging_key);
        String encoded_header(header_len, '\0');
        staged->readStrict(encoded_header.data(), encoded_header.size());
        (void)decodeEnvelopeHeader(encoded_header, header_len + payload_size, ObjectKind::Blob);
        return staged;
    };
    return source;
}

/// Stage a one-blob manifest for `payload` and durably precommit it before materialization.
PartWriteTxnPtr precommitBuildFor(
    const PoolPtr & s, const RootNamespace & ns, const String & ref, const String & payload)
{
    PartWriteInfo info;
    info.intended_ref = ns.string() + "/" + ref;
    PartWriteTxnPtr build = s->beginPartWrite(std::move(info));
    const ManifestId id = build->stageManifest({blobEntryFor("col.bin", u128Of(payload), payload.size())});
    build->precommitAdd(ns, ref, id);
    return build;
}

/// A one-shot `create`, asserting it committed (mirrors the retired `backend.putIfAbsent(key, bytes)`).
void createObj(Backend & backend, const String & key, const String & bytes)
{
    DB::Cas::tests::OperationForTest op(backend);
    ASSERT_TRUE(std::holds_alternative<Committed>((*op).create(key, bytes, Retry::once())));
}

/// An exact read (mirrors the retired `backend.get(key)`).
std::optional<Object> readObj(Backend & backend, const String & key)
{
    DB::Cas::tests::OperationForTest op(backend);
    return (*op).read(key, Retry::standard());
}

/// Whether `key` has a value, through a HEAD (mirrors the retired `backend.head(key).exists`).
bool headPresent(Backend & backend, const String & key)
{
    DB::Cas::tests::OperationForTest op(backend);
    return (*op).head(key, Retry::standard()).has_value();
}

/// Seed a present, well-formed blob body whose LOGICAL bytes are exactly `payload` (a fixed envelope
/// header followed by the payload), so a later HEAD returns a token and a logical size of `payload.size()`.
void seedPresentBody(
    InMemoryBackend & b, const Layout & layout, const PoolMeta & pm, const BlobRef & ref, const String & payload)
{
    EnvelopeHeader h;
    h.kind = ObjectKind::Blob;
    h.incarnation_tag = DB::UInt128(0xABCD);
    h.build_id = DB::UInt128(0x1111);
    const String head = encodeEnvelopeHeader(h, static_cast<uint32_t>(pm.blob_header_len));
    createObj(b, layout.blobKey(ref), head + payload);
}

/// The logical payload stored at `key` (object body minus the fixed blob header), or empty when absent.
String logicalPayloadAt(InMemoryBackend & b, const String & key, uint64_t header_len)
{
    const auto got = readObj(b, key);
    if (!got || got->bytes.size() < header_len)
        return {};
    return got->bytes.substr(header_len);
}

/// Whether the body now at the blob's key has a deletion intent.
bool currentBodyHasIntent(InMemoryBackend & b, const Layout & layout, const BlobRef & ref)
{
    const auto key = intentKeyOfCurrentBody(b, layout, ref);
    return key && objectPresent(b, *key);
}

/// The watched blob lane of `backend`'s journal in order: `HEAD` and publication of `blob_key`, `GET` of an intent.
/// Only this lane, so pool-open and precommit traffic cannot obscure the transaction-level ordering asserted below.
std::vector<String> blobLane(const CountingBackend & backend, const String & blob_key, const String & intent_prefix)
{
    std::vector<String> operations;
    for (const JournalEntry & entry : backend.journal())
    {
        if (entry.keys.size() != 1)
            continue;
        const String & key = entry.keys.front();
        if (entry.verb == JournalVerb::Head && key == blob_key)
            operations.emplace_back("head");
        else if (entry.verb == JournalVerb::Read && key.starts_with(intent_prefix))
            operations.emplace_back("intent-get");
        else if (entry.verb == JournalVerb::Publish && key == blob_key)
            operations.emplace_back("publish");
    }
    return operations;
}

}

TEST(CASUploadDetached, FreshMissHeadsThenPublishesWithNoIntentRead)
{
    const String payload = "mandatory-head-fresh-miss";
    const BlobRef ref = idOf(payload);
    auto backend = std::make_shared<CountingBackend>();
    auto store = openUploadPool(backend);
    auto build = precommitBuildFor(store, RootNamespace{"srv1/protocol-fresh"}, "part", payload);
    const String blob_key = store->layout().blobKey(ref);
    backend->clearJournal();

    const BlobUploadResult result = build->uploadBlobDetached(
        BlobUploadRequest{ref, BlobSource::fromString(payload), payload.size()});
    const std::vector<String> operations = blobLane(*backend, blob_key, store->layout().blobIntentsPrefix());

    EXPECT_EQ(result.dep.proof, BlobDependencyProof::Materialized);
    EXPECT_EQ(operations, (std::vector<String>{"head", "publish"}));
}

TEST(CASUploadDetached, ExistingBodyWithoutIntentHeadsAndObservesWithoutPublication)
{
    const String payload = "mandatory-head-existing-clean";
    const BlobRef ref = idOf(payload);
    auto backend = std::make_shared<CountingBackend>();
    auto store = openUploadPool(backend);
    seedPresentBody(*backend, store->layout(), store->poolMeta(), ref, payload);
    auto build = precommitBuildFor(store, RootNamespace{"srv1/protocol-clean"}, "part", payload);
    backend->clearJournal();
    const uint64_t avoided_before = ProfileEvents::global_counters[ProfileEvents::CASBlobBodyPutAvoided];

    const BlobUploadResult result = build->uploadBlobDetached(
        BlobUploadRequest{ref, BlobSource::fromString(payload), payload.size()});
    const std::vector<String> operations
        = blobLane(*backend, store->layout().blobKey(ref), store->layout().blobIntentsPrefix());

    EXPECT_EQ(result.dep.proof, BlobDependencyProof::Materialized);
    EXPECT_EQ(operations, (std::vector<String>{"head", "intent-get"}));
    EXPECT_EQ(ProfileEvents::global_counters[ProfileEvents::CASBlobBodyPutAvoided], avoided_before + 1);
}

TEST(CASUploadDetached, AbsentBodyPublishesWithNoIntentRead)
{
    const String payload = "mandatory-head-absent-stale-intent";
    const BlobRef ref = idOf(payload);
    auto backend = std::make_shared<CountingBackend>();
    auto store = openUploadPool(backend);
    /// An intent of an earlier incarnation stays; the body is gone, so nothing reads it.
    seedPresentBody(*backend, store->layout(), store->poolMeta(), ref, payload);
    plantIntentForCurrentBody(*backend, store->layout(), ref);
    {
        DB::Cas::tests::OperationForTest op(*backend);
        const String key = store->layout().blobKey(ref);
        ASSERT_EQ((*op).remove(key, (*op).head(key, Retry::standard())->etag, Retry::standard()), Removal::Removed);
    }
    auto build = precommitBuildFor(store, RootNamespace{"srv1/protocol-stale"}, "part", payload);
    backend->clearJournal();

    const BlobUploadResult result = build->uploadBlobDetached(
        BlobUploadRequest{ref, BlobSource::fromString(payload), payload.size()});
    const std::vector<String> operations
        = blobLane(*backend, store->layout().blobKey(ref), store->layout().blobIntentsPrefix());

    EXPECT_EQ(result.dep.proof, BlobDependencyProof::Materialized);
    EXPECT_EQ(result.diagnostics.reason, BlobPublicationReason::Absent);
    EXPECT_EQ(operations, (std::vector<String>{"head", "publish"}));
}

TEST(CASUploadDetached, PresentWithIntentPublishesFreshAndQueuedOldDeleteMisses)
{
    const String payload = "mandatory-head-present-condemned";
    const BlobRef ref = idOf(payload);
    auto backend = std::make_shared<CountingBackend>();
    auto store = openUploadPool(backend);
    seedPresentBody(*backend, store->layout(), store->poolMeta(), ref, payload);
    plantIntentForCurrentBody(*backend, store->layout(), ref);
    auto build = precommitBuildFor(store, RootNamespace{"srv1/protocol-condemned"}, "part", payload);
    const String blob_key = store->layout().blobKey(ref);
    DB::Cas::tests::OperationForTest condemned_probe(*backend);
    const Etag condemned_token = (*condemned_probe).head(blob_key, Retry::standard())->etag;
    backend->clearJournal();
    const uint64_t avoided_before = ProfileEvents::global_counters[ProfileEvents::CASBlobBodyPutAvoided];

    const BlobUploadResult result = build->uploadBlobDetached(
        BlobUploadRequest{ref, BlobSource::fromString(payload), payload.size()});
    const std::vector<String> operations = blobLane(*backend, blob_key, store->layout().blobIntentsPrefix());

    EXPECT_EQ(result.dep.proof, BlobDependencyProof::Materialized);
    EXPECT_EQ(result.diagnostics.reason, BlobPublicationReason::IntentPresent);
    EXPECT_EQ(operations, (std::vector<String>{"head", "intent-get", "publish"}));
    EXPECT_EQ(ProfileEvents::global_counters[ProfileEvents::CASBlobBodyPutAvoided], avoided_before);
    {
        DB::Cas::tests::OperationForTest op(*backend);
        EXPECT_EQ((*op).remove(blob_key, condemned_token, Retry::once()), Removal::Mismatch);
        EXPECT_TRUE((*op).head(blob_key, Retry::standard()).has_value());
    }
}

namespace
{

/// The body at the blob key before the upload, and where the writer's payload comes from.
struct UploadCell
{
    bool condemned = false;   /// a present body whose incarnation has a deletion intent; otherwise no body
    bool staged = false;      /// a re-readable staging object; otherwise the payload in memory
};

class CASUploadDetachedMatrix : public ::testing::TestWithParam<UploadCell>
{
};

}

INSTANTIATE_TEST_SUITE_P(CASUpload, CASUploadDetachedMatrix,
    ::testing::Values(UploadCell{false, false}, UploadCell{false, true}, UploadCell{true, false}, UploadCell{true, true}),
    [](const ::testing::TestParamInfo<UploadCell> & param)
    {
        return String(param.param.condemned ? "Condemned" : "Absent") + (param.param.staged ? "Staged" : "Local");
    });

/// `uploadBlobDetached` publishes what `putBlob` publishes, from a separate fresh store, and records no dependency
/// proof. An absent body with a staged source is copied verbatim; every other cell streams. A condemned body is
/// displaced by a fresh incarnation from the writer's own source, never a read or copy of the dying object.
TEST_P(CASUploadDetachedMatrix, DetachedUploadMatchesPutBlob)
{
    const UploadCell cell = GetParam();
    const RootNamespace ns{"srv1/nsDetached"};
    const String ref_name = "part";
    const String payload = "detached-upload-payload";
    const BlobRef blob = idOf(payload);
    const String staging_key = "p/staging/mount1/upload.tmp";

    auto arrange = [&](std::shared_ptr<InMemoryBackend> & b, PoolPtr & s, PartWriteTxnPtr & build, String & staging_bytes)
    {
        b = std::make_shared<InMemoryBackend>();
        s = openUploadPool(b);
        /// The staging object holds [header][payload], exactly as the S3-staging writer emits it.
        EnvelopeHeader h;
        h.kind = ObjectKind::Blob;
        h.incarnation_tag = DB::UInt128(0xC0FFEE);
        staging_bytes = encodeEnvelopeHeader(h, static_cast<uint32_t>(s->poolMeta().blob_header_len)) + payload;
        if (cell.staged)
            createObj(*b, staging_key, staging_bytes);
        if (cell.condemned)
        {
            /// A staged body is what a verbatim promote of the staging object produces.
            if (cell.staged)
                createObj(*b, s->layout().blobKey(blob), staging_bytes);
            else
                seedPresentBody(*b, s->layout(), s->poolMeta(), blob, payload);
            plantIntentForCurrentBody(*b, s->layout(), blob);
        }
        build = precommitBuildFor(s, ns, ref_name, payload);
    };
    auto sourceFor = [&](const std::shared_ptr<InMemoryBackend> & b, const PoolPtr & s)
    {
        return cell.staged ? reReadableStagedSource(b, staging_key, payload.size(), s->poolMeta().blob_header_len)
                           : BlobSource::fromString(payload);
    };

    std::shared_ptr<InMemoryBackend> b1;
    PoolPtr s1;
    PartWriteTxnPtr build1;
    String staging_bytes1;
    arrange(b1, s1, build1, staging_bytes1);
    const String key = s1->layout().blobKey(blob);
    DB::Cas::tests::OperationForTest token_probe(*b1);
    std::optional<Etag> condemned_token;
    if (cell.condemned)
    {
        condemned_token = (*token_probe).head(key, Retry::standard())->etag;
        ASSERT_TRUE(currentBodyHasIntent(*b1, s1->layout(), blob));
    }
    else
        ASSERT_FALSE(headPresent(*b1, key));
    EXPECT_EQ(build1->dependencyProof(blob), std::nullopt);

    const BlobUploadResult r = build1->uploadBlobDetached(BlobUploadRequest{blob, sourceFor(b1, s1), payload.size()});

    EXPECT_EQ(r.diagnostics.action, BlobMaterializationAction::Published);
    EXPECT_EQ(r.diagnostics.reason, cell.condemned ? BlobPublicationReason::IntentPresent : BlobPublicationReason::Absent);
    EXPECT_EQ(r.diagnostics.transport,
              cell.staged && !cell.condemned ? BlobPublicationTransport::ServerSideCopy : BlobPublicationTransport::Streaming);
    EXPECT_EQ(r.ref, blob);
    EXPECT_EQ(r.dep.proof, BlobDependencyProof::Materialized);
    EXPECT_EQ(r.dep.size, payload.size());

    EXPECT_EQ(build1->dependencyProof(blob), std::nullopt) << "only the attached path records the dependency proof";
    ASSERT_TRUE(headPresent(*b1, key));
    if (cell.condemned)
        EXPECT_NE(tokenText((*token_probe).head(key, Retry::standard())->etag), tokenText(*condemned_token))
            << "the condemned incarnation was displaced by a fresh one";
    EXPECT_FALSE(currentBodyHasIntent(*b1, s1->layout(), blob));
    EXPECT_EQ(logicalPayloadAt(*b1, key, s1->poolMeta().blob_header_len), payload);
    const auto got = readObj(*b1, key);
    ASSERT_TRUE(got.has_value());
    if (cell.staged && !cell.condemned)
        EXPECT_EQ(got->bytes, staging_bytes1) << "the server-side copy moved the staging bytes verbatim";

    std::shared_ptr<InMemoryBackend> b2;
    PoolPtr s2;
    PartWriteTxnPtr build2;
    String staging_bytes2;
    arrange(b2, s2, build2, staging_bytes2);
    const PutBlobResult pr = build2->putBlob(blob, sourceFor(b2, s2));
    EXPECT_EQ(build2->dependencyProof(blob), BlobDependencyProof::Materialized);
    EXPECT_EQ(pr.size, r.dep.size);

    /// A fresh envelope's incarnation tag differs per upload, but the LOGICAL payload matches.
    EXPECT_EQ(logicalPayloadAt(*b1, key, s1->poolMeta().blob_header_len),
              logicalPayloadAt(*b2, key, s2->poolMeta().blob_header_len));
    EXPECT_EQ(currentBodyHasIntent(*b1, s1->layout(), blob), currentBodyHasIntent(*b2, s2->layout(), blob));
    if (cell.staged && !cell.condemned)
    {
        const auto got2 = readObj(*b2, key);
        ASSERT_TRUE(got2.has_value());
        EXPECT_EQ(got->bytes, got2->bytes);
    }
}

/// `mergeBlobUploadResults` folds N detached results in ONE call to EXACTLY the same deps a serial
/// putBlob fold would produce. Both worlds run the identical sequence of backend calls (same
/// precommit and same blobs in the same order). The merge path adds no backend calls of its own, only
/// in-memory bookkeeping, so a deep dependency-map comparison is exact.
TEST(CASUploadDetached, MergeAppliesAllDeps)
{
    const RootNamespace ns{"srv1/nsMergeAll"};
    const String ref_name = "part";
    const std::vector<String> payloads = {"merge-fresh-a", "merge-fresh-b", "merge-fresh-c"};

    auto arrange = [&](std::shared_ptr<InMemoryBackend> & b, PoolPtr & s, PartWriteTxnPtr & build)
    {
        b = std::make_shared<InMemoryBackend>();
        s = openUploadPool(b);
        build = precommitBuildFor(s, ns, ref_name, "manifest-seed");
    };

    std::shared_ptr<InMemoryBackend> b1;
    PoolPtr s1;
    PartWriteTxnPtr build1;
    arrange(b1, s1, build1);

    std::vector<BlobUploadResult> results;
    for (const auto & payload : payloads)
    {
        const BlobRef blob = idOf(payload);
        EXPECT_EQ(build1->dependencyProof(blob), std::nullopt);
        results.push_back(build1->uploadBlobDetached(
            BlobUploadRequest{blob, BlobSource::fromString(payload), payload.size()}));
    }
    /// Still untouched before the merge -- uploadBlobDetached folds nothing.
    for (const auto & payload : payloads)
        EXPECT_EQ(build1->dependencyProof(idOf(payload)), std::nullopt);

    build1->mergeBlobUploadResults(results);

    for (const auto & payload : payloads)
        EXPECT_EQ(build1->dependencyProof(idOf(payload)), BlobDependencyProof::Materialized);

    std::shared_ptr<InMemoryBackend> b2;
    PoolPtr s2;
    PartWriteTxnPtr build2;
    arrange(b2, s2, build2);
    for (const auto & payload : payloads)
        build2->putBlob(idOf(payload), BlobSource::fromString(payload));

    EXPECT_EQ(build1->depsSnapshotForTest(), build2->depsSnapshotForTest());
}

/// Merge exception safety (spec Test 16): a hook injected between per-result applications throws
/// after the FIRST result would have applied; the SECOND result must never reach `deps`, and neither
/// may a PRE-EXISTING unrelated dep be disturbed -- a DEEP snapshot (the whole map, not one ref probed
/// at a time) proves the build is byte-for-byte at its pre-merge state, all-or-nothing observed.
TEST(CASUploadDetached, MergeFailureLeavesBuildUntouched)
{
    const RootNamespace ns{"srv1/nsMergeFail"};
    const String ref_name = "part";
    const String payload_existing = "merge-fail-existing";
    const String payload_a = "merge-fail-a";
    const String payload_b = "merge-fail-b";

    auto b = std::make_shared<InMemoryBackend>();
    auto s = openUploadPool(b);
    auto build = precommitBuildFor(s, ns, ref_name, "manifest-seed");

    /// A pre-existing folded dep the merge must leave completely alone.
    build->putBlob(idOf(payload_existing), BlobSource::fromString(payload_existing));
    ASSERT_EQ(build->dependencyProof(idOf(payload_existing)), BlobDependencyProof::Materialized);

    std::vector<BlobUploadResult> results;
    results.push_back(build->uploadBlobDetached(
        BlobUploadRequest{idOf(payload_a), BlobSource::fromString(payload_a), payload_a.size()}));
    results.push_back(build->uploadBlobDetached(
        BlobUploadRequest{idOf(payload_b), BlobSource::fromString(payload_b), payload_b.size()}));

    const auto pre_merge_snapshot = build->depsSnapshotForTest();
    ASSERT_EQ(pre_merge_snapshot.size(), 1u);   /// only the pre-existing dep; the detached uploads folded nothing

    build->setMergeHookForTest([](size_t applied_so_far)
    {
        if (applied_so_far == 1)
            throw std::bad_alloc();
    });

    EXPECT_THROW(build->mergeBlobUploadResults(results), std::bad_alloc);

    EXPECT_EQ(build->depsSnapshotForTest(), pre_merge_snapshot);
    EXPECT_EQ(build->dependencyProof(idOf(payload_a)), std::nullopt);
    EXPECT_EQ(build->dependencyProof(idOf(payload_b)), std::nullopt);
}

/// Duplicate-grouping consistency: two results for the SAME ref with conflicting sizes are rejected
/// as a staging bug (LOGICAL_ERROR) BEFORE any result applies -- the fan-out's one-task-per-unique-ref
/// invariant means this should never happen upstream, so merge itself is the backstop. LOGICAL_ERROR
/// aborts the whole process in debug/sanitizer builds instead of behaving like a catchable exception
/// (`Common/Exception.cpp`'s `handle_error_code`) -- `CASUploadDetachedDeathTest` below proves the
/// abort positively in those builds instead (it cannot also verify the build-untouched postcondition,
/// since there is no continuation after a real abort).
#ifndef DEBUG_OR_SANITIZER_BUILD
TEST(CASUploadDetached, MergeValidatesSizes)
{
    const RootNamespace ns{"srv1/nsMergeSizes"};
    const String ref_name = "part";
    const String payload = "merge-size-conflict";
    const BlobRef blob = idOf(payload);

    auto b = std::make_shared<InMemoryBackend>();
    auto s = openUploadPool(b);
    auto build = precommitBuildFor(s, ns, ref_name, "manifest-seed");

    const BlobUploadResult r = build->uploadBlobDetached(
        BlobUploadRequest{blob, BlobSource::fromString(payload), payload.size()});
    ASSERT_EQ(build->dependencyProof(blob), std::nullopt);

    BlobUploadResult conflicting = r;
    conflicting.dep.size = r.dep.size + 1;   /// same ref, conflicting declared size

    const auto pre_merge_snapshot = build->depsSnapshotForTest();

    expectThrowsCode(DB::ErrorCodes::LOGICAL_ERROR, [&]
    {
        build->mergeBlobUploadResults(std::vector<BlobUploadResult>{r, conflicting});
    });

    EXPECT_EQ(build->depsSnapshotForTest(), pre_merge_snapshot);
    EXPECT_EQ(build->dependencyProof(blob), std::nullopt);
}
#endif

#if defined(DEBUG_OR_SANITIZER_BUILD)
TEST(CASUploadDetachedDeathTest, MergeValidatesSizesAborts)
{
    const RootNamespace ns{"srv1/nsMergeSizes"};
    const String ref_name = "part";
    const String payload = "merge-size-conflict";
    const BlobRef blob = idOf(payload);

    auto b = std::make_shared<InMemoryBackend>();
    auto s = openUploadPool(b);
    auto build = precommitBuildFor(s, ns, ref_name, "manifest-seed");

    const BlobUploadResult r = build->uploadBlobDetached(
        BlobUploadRequest{blob, BlobSource::fromString(payload), payload.size()});

    BlobUploadResult conflicting = r;
    conflicting.dep.size = r.dep.size + 1;   /// same ref, conflicting declared size

    EXPECT_DEATH(
        { build->mergeBlobUploadResults(std::vector<BlobUploadResult>{r, conflicting}); }, "");
}
#endif
