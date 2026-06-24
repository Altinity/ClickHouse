#include <gtest/gtest.h>

#include "config.h"

#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasBackend.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasInMemoryBackend.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasInstrumentedBackend.h>
#include <Common/ProfileEvents.h>
#include <IO/WriteHelpers.h>

#if USE_AWS_S3
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasObjectStorageBackend.h>
#include <Disks/DiskObjectStorage/ObjectStorages/Local/LocalObjectStorage.h>
#include <Disks/DiskObjectStorage/ObjectStorages/StoredObject.h>
#include <Disks/WriteMode.h>
#include <IO/ReadBufferFromFileBase.h>
#include <IO/S3Common.h>
#include <filesystem>
#include <map>
#include <mutex>
#endif

using namespace DB::Cas;

/// Minimal concrete implementation that overrides every pure virtual with trivial defaults.
/// Purpose: verify the interface compiles, is overridable, and result-type defaults are sane.
struct NullBackend final : Backend
{
    std::optional<GetResult> get(const String & /*key*/, Range /*range*/) override
    {
        return std::nullopt;
    }

    HeadResult head(const String & /*key*/) override
    {
        return HeadResult{};
    }

    PutResult putIfAbsent(const String & /*key*/, const String & /*bytes*/, const ObjectMeta & /*meta*/) override
    {
        return {PutOutcome::Done, {}};
    }

    WriteSinkPtr putIfAbsentStream(const String & /*key*/, const ObjectMeta & /*meta*/) override
    {
        return nullptr;   /// trivial default — streaming behavior is pinned by the CasBackendContract suite
    }

    PutResult putOverwrite(const String & /*key*/, const String & /*bytes*/, const Token & /*expected*/, const ObjectMeta & /*meta*/) override
    {
        return {PutOutcome::PreconditionFailed, {}};
    }

    CasResult casPut(const String & /*key*/, const String & /*bytes*/, const std::optional<Token> & /*expected*/, const ObjectMeta & /*meta*/) override
    {
        return {CasOutcome::Conflict, {}};
    }

    DeleteOutcome deleteExact(const String & /*key*/, const Token & /*token*/) override
    {
        return DeleteOutcome{};
    }

    ListPage list(const String & /*prefix*/, const String & /*cursor*/, size_t /*limit*/) override
    {
        return ListPage{};
    }
};

TEST(CasBackend, NullBackendShapeAndDefaults)
{
    NullBackend b;
    // Use the base-class reference so virtual dispatch uses base-class default args.
    Backend & ref = b;

    // get returns absent
    EXPECT_FALSE(ref.get("k").has_value());

    // head returns non-existent
    HeadResult h = b.head("k");
    EXPECT_FALSE(h.exists);
    EXPECT_EQ(h.size, 0u);
    EXPECT_TRUE(h.token.empty());

    // putIfAbsent returns Done
    EXPECT_EQ(ref.putIfAbsent("k", "v").outcome, PutOutcome::Done);

    // putOverwrite returns PreconditionFailed
    EXPECT_EQ(ref.putOverwrite("k", "v", Token{}).outcome, PutOutcome::PreconditionFailed);

    // casPut returns Conflict
    EXPECT_EQ(ref.casPut("k", "v", std::nullopt).outcome, CasOutcome::Conflict);

    // deleteExact default kind is NotFound
    DeleteOutcome d = b.deleteExact("k", Token{});
    EXPECT_EQ(d.kind, DeleteOutcome::Kind::NotFound);
    EXPECT_FALSE(d.created_delete_marker);

    // list returns empty page
    ListPage page = b.list("p/", "", 10);
    EXPECT_TRUE(page.keys.empty());
    EXPECT_TRUE(page.next_cursor.empty());

    // Range::whole() helper
    EXPECT_TRUE(Range{}.whole());
    Range r1; r1.offset = 1;
    EXPECT_FALSE(r1.whole());
    Range r2; r2.length = 5u;
    EXPECT_FALSE(r2.whole());
}

// =====================================================================
// Task 3: CasInMemoryBackend — enforcing token semantics
// =====================================================================

TEST(CasInMemory, PutIfAbsentAndGet)
{
    InMemoryBackend b;
    const auto put = b.putIfAbsent("k", "v1");
    const Token t1 = put.token;
    EXPECT_EQ(put.outcome, PutOutcome::Done);
    EXPECT_FALSE(t1.empty());
    EXPECT_EQ(b.putIfAbsent("k", "clobber").outcome, PutOutcome::PreconditionFailed);
    auto g = b.get("k");
    ASSERT_TRUE(g.has_value());
    EXPECT_EQ(g->bytes, "v1");
    EXPECT_EQ(g->token, t1);
    EXPECT_FALSE(b.get("absent").has_value());
}

TEST(CasInMemory, OverwriteIsTokenExactAndMintsFreshToken)
{
    InMemoryBackend b;
    const Token t1 = b.putIfAbsent("k", "v1").token;
    EXPECT_EQ(b.putOverwrite("k", "v2", Token{"wrong", TokenType::Emulated}).outcome, PutOutcome::PreconditionFailed);
    EXPECT_EQ(b.get("k")->bytes, "v1");                       // untouched on mismatch
    const auto overwrite = b.putOverwrite("k", "v2", t1);
    EXPECT_EQ(overwrite.outcome, PutOutcome::Done);
    EXPECT_NE(overwrite.token, t1);                           // tokens never repeat
    EXPECT_EQ(b.get("k")->bytes, "v2");
}

TEST(CasInMemory, CasPutCreateAndSwap)
{
    InMemoryBackend b;
    const auto create = b.casPut("m", "s1", std::nullopt);
    const Token t1 = create.token;
    EXPECT_EQ(create.outcome, CasOutcome::Committed);                             // create-if-absent
    EXPECT_EQ(b.casPut("m", "s1x", std::nullopt).outcome, CasOutcome::Conflict);  // exists now
    EXPECT_EQ(b.casPut("m", "s2", Token{"stale", TokenType::Emulated}).outcome, CasOutcome::Conflict);
    EXPECT_EQ(b.get("m")->bytes, "s1");
    EXPECT_EQ(b.casPut("m", "s2", t1).outcome, CasOutcome::Committed);
    EXPECT_EQ(b.get("m")->bytes, "s2");
}

TEST(CasInMemory, DeleteExactEnforced)
{
    InMemoryBackend b;
    const Token t1 = b.putIfAbsent("k", "v1").token;
    auto d1 = b.deleteExact("k", Token{"wrong", TokenType::Emulated});
    EXPECT_EQ(d1.kind, DeleteOutcome::Kind::TokenMismatch);
    EXPECT_TRUE(b.get("k").has_value());                      // SURVIVES wrong-token delete
    auto d2 = b.deleteExact("k", t1);
    EXPECT_EQ(d2.kind, DeleteOutcome::Kind::Deleted);
    EXPECT_FALSE(d2.created_delete_marker);
    EXPECT_FALSE(b.get("k").has_value());
    EXPECT_EQ(b.deleteExact("k", t1).kind, DeleteOutcome::Kind::NotFound);
}

TEST(CasInMemory, RangeGetAndHeadAndList)
{
    InMemoryBackend b;
    b.putIfAbsent("p/a", "0123456789");
    b.putIfAbsent("p/b", "xy");
    b.putIfAbsent("q/c", "z");
    EXPECT_EQ(b.get("p/a", Range{.offset = 2, .length = 3})->bytes, "234");
    auto h = b.head("p/a");
    EXPECT_TRUE(h.exists);
    EXPECT_EQ(h.size, 10u);
    auto page = b.list("p/", "", 10);
    ASSERT_EQ(page.keys.size(), 2u);                          // sorted, prefix-scoped
    EXPECT_EQ(page.keys[0].key, "p/a");
    EXPECT_EQ(page.keys[1].key, "p/b");
    EXPECT_TRUE(page.next_cursor.empty());
    auto page1 = b.list("p/", "", 1);                         // pagination
    EXPECT_EQ(page1.keys.size(), 1u);
    EXPECT_FALSE(page1.next_cursor.empty());
    auto page2 = b.list("p/", page1.next_cursor, 1);
    EXPECT_EQ(page2.keys[0].key, "p/b");
}

// =====================================================================
// Task 4: CasInMemoryBackend — fault injection and probe-test modes
// =====================================================================

TEST(CasInMemoryFaults, HeldDeleteLandsLater)
{
    InMemoryBackend b;
    const Token t1 = b.putIfAbsent("k", "v1").token;
    b.setHoldDeletes(true);
    auto d = b.deleteExact("k", t1);                  // message "sent", not landed
    EXPECT_EQ(d.kind, DeleteOutcome::Kind::Deleted);  // caller sees the send accepted
    EXPECT_TRUE(b.get("k").has_value());              // ... but nothing landed yet
    ASSERT_EQ(b.pendingDeletes(), 1u);
    // the object is resurrected before the zombie lands:
    b.putOverwrite("k", "v1'", t1);
    auto landed = b.landPendingDelete(0);             // the zombie lands NOW
    EXPECT_EQ(landed.kind, DeleteOutcome::Kind::TokenMismatch);   // 412 — INV-NO-RETURN in miniature
    EXPECT_EQ(b.get("k")->bytes, "v1'");
}

TEST(CasInMemoryFaults, InjectedCasConflictFiresOnce)
{
    InMemoryBackend b;
    const Token t1 = b.casPut("m", "s1", std::nullopt).token;
    b.failNextCasPut("m");
    EXPECT_EQ(b.casPut("m", "s2", t1).outcome, CasOutcome::Conflict);     // injected
    EXPECT_EQ(b.get("m")->bytes, "s1");
    EXPECT_EQ(b.casPut("m", "s2", t1).outcome, CasOutcome::Committed);    // next attempt is real
}

TEST(CasInMemoryFaults, NonEnforcingModeMimicsBadBackend)
{
    InMemoryBackend b;
    b.setEnforceTokens(false);                        // MinIO-OSS-shaped backend
    b.putIfAbsent("k", "v1");
    auto d = b.deleteExact("k", Token{"totally-wrong", TokenType::Emulated});
    EXPECT_EQ(d.kind, DeleteOutcome::Kind::Deleted);  // silently deletes anyway — the dangerous behavior
    EXPECT_FALSE(b.get("k").has_value());
}

TEST(CasInMemoryFaults, VersioningMarkerMode)
{
    InMemoryBackend b;
    b.setSimulateDeleteMarkers(true);
    const Token t1 = b.putIfAbsent("k", "v1").token;
    EXPECT_TRUE(b.deleteExact("k", t1).created_delete_marker);    // probe must reject this pool
}

TEST(CasInMemoryBackend, RoundTripsUserMetadata)
{
    DB::Cas::InMemoryBackend backend;
    const DB::Cas::ObjectMeta meta{{"cas_owner", "ab:7:42"}};
    ASSERT_EQ(backend.putIfAbsent("k/key", "body", meta).outcome, DB::Cas::PutOutcome::Done);

    const auto hr = backend.head("k/key");
    ASSERT_TRUE(hr.exists);
    ASSERT_EQ(hr.attributes.at("cas_owner"), "ab:7:42");

    const auto gr = backend.get("k/key");
    ASSERT_TRUE(gr.has_value());
    ASSERT_EQ(gr->attributes.at("cas_owner"), "ab:7:42");
}

// =====================================================================
// B168 P0: InstrumentedBackend per-namespace/op ProfileEvents
// =====================================================================

namespace ProfileEvents
{
extern const Event CasBlobPut;
extern const Event CasBlobPutDedup;
extern const Event CasBlobHead;
extern const Event CasBlobHeadMiss;
extern const Event CasGcCas;
}

TEST(CasInstrumentedBackend, ClassifierAndPerNamespaceOpEvents)
{
    /// Namespace classification by substring.
    EXPECT_EQ(classifyCasNs("pool/blobs/ab/abcdef"), CasNs::Blob);
    EXPECT_EQ(classifyCasNs("pool/trees/00/deadbeef"), CasNs::Tree);
    EXPECT_EQ(classifyCasNs("pool/gc/registry"), CasNs::Gc);   /// registry relocated roots/_registry -> gc/registry (design §5.3)
    EXPECT_EQ(classifyCasNs("pool/roots/default/_files/x"), CasNs::Root);
    EXPECT_EQ(classifyCasNs("pool/gc/state"), CasNs::Gc);
    EXPECT_EQ(classifyCasNs("pool/builds/b7"), CasNs::Build);   /// build heartbeats
    /// Phase 6: server control state lives under roots/<server-hex>/; classified by suffix/segment.
    EXPECT_EQ(classifyCasNs("pool/roots/aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa/_watermark"), CasNs::Server);
    EXPECT_EQ(classifyCasNs("pool/roots/aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa/_precommits/3"), CasNs::Build);
    EXPECT_EQ(classifyCasNs("pool/_pool_meta"), CasNs::Other);

    auto inner = std::make_shared<InMemoryBackend>();
    InstrumentedBackend b(inner);

    using ProfileEvents::global_counters;
    const auto blob_put_before   = global_counters[ProfileEvents::CasBlobPut].load();
    const auto blob_dedup_before = global_counters[ProfileEvents::CasBlobPutDedup].load();
    const auto blob_head_before  = global_counters[ProfileEvents::CasBlobHead].load();
    const auto blob_miss_before  = global_counters[ProfileEvents::CasBlobHeadMiss].load();
    const auto gc_cas_before     = global_counters[ProfileEvents::CasGcCas].load();

    const String blob_key = "pool/blobs/ab/abcdef0123456789";

    /// First put of a blob ⇒ Put.
    EXPECT_EQ(b.putIfAbsent(blob_key, "payload").outcome, PutOutcome::Done);
    /// Second put of the same key ⇒ PutDedup (content already exists).
    EXPECT_EQ(b.putIfAbsent(blob_key, "payload").outcome, PutOutcome::PreconditionFailed);
    /// head of an absent blob key ⇒ HeadMiss (the 404 signal).
    EXPECT_FALSE(b.head("pool/blobs/zz/absent").exists);
    /// head of the present blob key ⇒ Head.
    EXPECT_TRUE(b.head(blob_key).exists);
    /// casPut create on a gc key ⇒ Gc Cas.
    EXPECT_EQ(b.casPut("pool/gc/state", "g1", std::nullopt).outcome, CasOutcome::Committed);
    /// Streaming put to a fresh blob key, then finalize ⇒ Put.
    {
        auto sink = b.putIfAbsentStream("pool/blobs/cd/cafebabe");
        ASSERT_TRUE(sink != nullptr);
        DB::writeString(String("streamed"), sink->buffer());
        EXPECT_EQ(sink->finalize().outcome, PutOutcome::Done);
    }

    /// Under coverage builds ProfileEvents propagate into a thread-local subtree that does not reach
    /// `global_counters`; deltas read 0 there only (see gtest_unique_key_index_cache).
#if !WITH_COVERAGE
    EXPECT_EQ(global_counters[ProfileEvents::CasBlobPut].load()      - blob_put_before,   2u);
    EXPECT_EQ(global_counters[ProfileEvents::CasBlobPutDedup].load() - blob_dedup_before, 1u);
    EXPECT_EQ(global_counters[ProfileEvents::CasBlobHead].load()     - blob_head_before,  1u);
    EXPECT_EQ(global_counters[ProfileEvents::CasBlobHeadMiss].load() - blob_miss_before,  1u);
    EXPECT_EQ(global_counters[ProfileEvents::CasGcCas].load()        - gc_cas_before,     1u);
#else
    (void)blob_put_before; (void)blob_dedup_before; (void)blob_head_before;
    (void)blob_miss_before; (void)gc_cas_before;
#endif
}

// =====================================================================
// M-C2 Task 2: typed S3 precondition signal
// =====================================================================

#if USE_AWS_S3

/// The Native conditional-PUT path discriminates a lost precondition by the canonical S3 error code
/// string ("PreconditionFailed", "NoSuchKey", ...) that `S3Exception` carries from the response XML
/// `<Code>` — a 412 is UNMODELED for the AWS SDK (the enum value is UNKNOWN), so the name is the only
/// machine-readable signal.
TEST(CasS3Signal, S3ExceptionCarriesCanonicalErrorName)
{
    DB::S3Exception e("412 from backend", Aws::S3::S3Errors::UNKNOWN, "PreconditionFailed");
    EXPECT_EQ(e.getExceptionName(), "PreconditionFailed");
    DB::S3Exception bare("no name attached", Aws::S3::S3Errors::UNKNOWN);
    EXPECT_TRUE(bare.getExceptionName().empty());
}

namespace
{

/// WriteBuffer stub whose finalize throws a configured S3Exception — drives the classifier directly.
class ThrowOnFinalizeBuffer final : public DB::WriteBuffer
{
public:
    ThrowOnFinalizeBuffer() : DB::WriteBuffer(nullptr, 0) {}

    explicit ThrowOnFinalizeBuffer(DB::S3Exception e) : DB::WriteBuffer(nullptr, 0), to_throw(std::move(e)) {}

private:
    void nextImpl() override {}

    void finalizeImpl() override
    {
        if (to_throw)
            throw *to_throw;
    }

    std::optional<DB::S3Exception> to_throw;
};

}

/// detail::finalizeConditionalWrite maps a lost precondition to an OUTCOME by exact-matching the
/// canonical S3 error name (plus the modeled NO_SUCH_KEY enum, which WriteBufferFromS3 surfaces
/// nameless on retry exhaustion) and rethrows anything else.
TEST(CasS3Signal, FinalizeClassifierMapsPreconditionLossExactly)
{
    using DB::Cas::detail::finalizeConditionalWrite;

    auto classify = [](DB::S3Exception e)
    {
        ThrowOnFinalizeBuffer buf(std::move(e));
        return finalizeConditionalWrite(buf);
    };

    EXPECT_EQ(classify(DB::S3Exception("412", Aws::S3::S3Errors::UNKNOWN, "PreconditionFailed")),
              PutOutcome::PreconditionFailed);
    EXPECT_EQ(classify(DB::S3Exception("404 gone under If-Match", Aws::S3::S3Errors::UNKNOWN, "NoSuchKey")),
              PutOutcome::PreconditionFailed);
    EXPECT_EQ(classify(DB::S3Exception("retries exhausted, no name attached", Aws::S3::S3Errors::NO_SUCH_KEY)),
              PutOutcome::PreconditionFailed);

    ThrowOnFinalizeBuffer unrelated(DB::S3Exception("503", Aws::S3::S3Errors::UNKNOWN, "SlowDown"));
    EXPECT_THROW(finalizeConditionalWrite(unrelated), DB::S3Exception);

    ThrowOnFinalizeBuffer clean;
    EXPECT_EQ(finalizeConditionalWrite(clean), PutOutcome::Done);
}

namespace
{

/// A `LocalObjectStorage` that round-trips user metadata in-process. The production
/// `LocalObjectStorage` deliberately drops the `attributes` argument of `writeObject` and never
/// populates `ObjectMetadata::attributes` (local files carry no `x-amz-meta-*`), so it cannot stand
/// in for S3/RustFS when verifying the metadata threading. This test-only subclass records the
/// attributes passed on write, keyed by physical path, and injects them back on metadata reads —
/// exactly what a real object store does for `x-amz-meta-*`. It exercises the `EmulatedSingleProcess`
/// `ObjectStorageBackend` threading (`putIfAbsent` → `writeObject` attributes → `head` attributes)
/// without a live S3 backend; the real S3/RustFS round trip is verified empirically out-of-band.
class AttributePreservingLocalObjectStorage final : public DB::LocalObjectStorage
{
public:
    using DB::LocalObjectStorage::LocalObjectStorage;

    std::unique_ptr<DB::WriteBufferFromFileBase> writeObject(
        const DB::StoredObject & object,
        DB::WriteMode mode,
        std::optional<DB::ObjectAttributes> attributes,
        size_t buf_size,
        const DB::WriteSettings & write_settings) override
    {
        if (attributes.has_value())
        {
            std::lock_guard lock(mutex);
            saved_attributes[object.remote_path] = *attributes;
        }
        return DB::LocalObjectStorage::writeObject(object, mode, attributes, buf_size, write_settings);
    }

    std::optional<DB::ObjectMetadata> tryGetObjectMetadata(const std::string & path, bool with_tags) const override
    {
        auto metadata = DB::LocalObjectStorage::tryGetObjectMetadata(path, with_tags);
        if (metadata)
            inject(path, *metadata);
        return metadata;
    }

    DB::ObjectMetadata getObjectMetadata(const std::string & path, bool with_tags) const override
    {
        auto metadata = DB::LocalObjectStorage::getObjectMetadata(path, with_tags);
        inject(path, metadata);
        return metadata;
    }

private:
    void inject(const std::string & path, DB::ObjectMetadata & metadata) const
    {
        std::lock_guard lock(mutex);
        if (auto it = saved_attributes.find(path); it != saved_attributes.end())
            metadata.attributes = it->second;
    }

    mutable std::mutex mutex;
    mutable std::map<std::string, DB::ObjectAttributes> saved_attributes;
};

DB::ObjectStoragePtr makeAttributePreservingStorageForTest()
{
    static std::atomic<uint64_t> counter{0};
    const auto unique = std::to_string(::getpid()) + "_" + std::to_string(counter.fetch_add(1));
    const auto root = (std::filesystem::temp_directory_path() / ("cas_meta_unit_" + unique)).string();

    std::error_code ec;
    std::filesystem::remove_all(root, ec);
    std::filesystem::create_directories(root, ec);

    DB::LocalObjectStorageSettings settings("test", root, /*read_only_=*/false);
    return std::make_shared<AttributePreservingLocalObjectStorage>(std::move(settings));
}

}

/// The `EmulatedSingleProcess` `ObjectStorageBackend` must thread user metadata through to the
/// underlying object storage's `writeObject` attributes on `putIfAbsent` and read it back into
/// `HeadResult::attributes` on `head`. Verified here over an attribute-preserving object storage
/// (the production `LocalObjectStorage` drops attributes); the live S3/RustFS round trip is verified
/// empirically out-of-band.
TEST(CasObjectStorageBackend, EmulatedRoundTripsUserMetadata)
{
    ObjectStorageBackend backend(makeAttributePreservingStorageForTest(), ObjectStorageBackend::Mode::EmulatedSingleProcess);

    const DB::Cas::ObjectMeta meta{{"cas_owner", "ab:7:42"}};
    ASSERT_EQ(backend.putIfAbsent("k/key", "body", meta).outcome, DB::Cas::PutOutcome::Done);

    const auto hr = backend.head("k/key");
    ASSERT_TRUE(hr.exists);
    ASSERT_EQ(hr.attributes.at("cas_owner"), "ab:7:42");
}

namespace
{

/// A `LocalObjectStorage` whose `readObject` throws `S3Exception(NO_SUCH_KEY)` for a configured
/// physical key, while `tryGetObjectMetadata` still reports that key as PRESENT.
/// This simulates the HEAD→GET race window: the HEAD succeeds, then the object is deleted before
/// the GET arrives.
class NativeReadThrowsNoSuchKeyObjectStorage final : public DB::LocalObjectStorage
{
public:
    using DB::LocalObjectStorage::LocalObjectStorage;

    void setThrowOnRead(const std::string & path)
    {
        throw_on_read_path = path;
    }

    std::unique_ptr<DB::ReadBufferFromFileBase> readObject(
        const DB::StoredObject & object,
        const DB::ReadSettings & read_settings,
        std::optional<size_t> read_hint,
        bool use_external_buffer,
        bool restrict_seek) const override
    {
        if (object.remote_path == throw_on_read_path)
            throw DB::S3Exception(
                "NoSuchKey: The specified key does not exist.",
                Aws::S3::S3Errors::NO_SUCH_KEY);

        return DB::LocalObjectStorage::readObject(object, read_settings, read_hint, use_external_buffer, restrict_seek);
    }

private:
    std::string throw_on_read_path;
};

DB::ObjectStoragePtr makeThrowOnReadStorageForTest(const std::string & physical_key)
{
    static std::atomic<uint64_t> counter{0};
    const auto unique = std::to_string(::getpid()) + "_" + std::to_string(counter.fetch_add(1));
    const auto root = (std::filesystem::temp_directory_path() / ("cas_midget_unit_" + unique)).string();

    std::error_code ec;
    std::filesystem::remove_all(root, ec);
    std::filesystem::create_directories(root, ec);

    DB::LocalObjectStorageSettings settings("test", root, /*read_only_=*/false);
    auto storage = std::make_shared<NativeReadThrowsNoSuchKeyObjectStorage>(std::move(settings));

    /// Write the object so tryGetObjectMetadata reports it present (HEAD succeeds).
    {
        auto buf = storage->writeObject(DB::StoredObject(physical_key), DB::WriteMode::Rewrite, std::nullopt);
        buf->write("content", 7);
        buf->finalize();
    }

    /// Now configure: future readObject calls for this key will throw NO_SUCH_KEY.
    storage->setThrowOnRead(physical_key);
    return storage;
}

}

/// `ObjectStorageBackend::get` in `Native` mode: when `tryGetObjectMetadata` (`nativeHead`) reports the
/// key PRESENT but `readObject` throws `S3Exception(NO_SUCH_KEY)` — simulating a deletion in the
/// HEAD→GET window — `get` MUST return `std::nullopt` rather than letting the raw exception escape.
TEST(CasObjectStorageBackend, NativeModeGetReturnsNulloptOnMidGetNoSuchKey)
{
    /// The Native mode backend uses the key verbatim as the physical path (no emu_root prefix), so we
    /// use the same string for both the physical key and the logical key.
    const std::string key = "pool/blobs/ab/abcdef0123456789abcdef0123456789";
    auto storage = makeThrowOnReadStorageForTest(key);

    ObjectStorageBackend backend(storage, ObjectStorageBackend::Mode::Native);

    /// HEAD reports the key present; readObject then throws NO_SUCH_KEY.
    /// Contract: get must return std::nullopt, not propagate the S3Exception.
    /// Call through the base-class interface so the default `Range{}` arg is available.
    Backend & iface = backend;
    const auto result = iface.get(key);
    EXPECT_FALSE(result.has_value());
}

#endif
