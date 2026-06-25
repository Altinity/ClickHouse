#include <gtest/gtest.h>
#include <algorithm>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasBuild.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasStore.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasInMemoryBackend.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasEnvelope.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasGc.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasGcFormats.h>
#include <Disks/tests/cas_test_helpers.h>
#include <IO/WriteBuffer.h>
#include <IO/WriteHelpers.h>
#include <Common/Exception.h>

namespace DB::ErrorCodes
{
extern const int BAD_ARGUMENTS;
extern const int FILE_DOESNT_EXIST;
extern const int LOGICAL_ERROR;
extern const int NOT_IMPLEMENTED;
extern const int ABORTED;
}

using namespace DB::Cas;
using DB::Cas::tests::expectThrowsCode;
using DB::Cas::tests::fenceNamespace;
using DB::Cas::tests::idOf;
using DB::Cas::tests::injectRetire;
using DB::Cas::tests::shardOfForTest;
using DB::Cas::tests::u128Of;
using DB::Cas::tests::writeTreeRaw;

namespace
{

StorePtr openStore(const std::shared_ptr<InMemoryBackend> & b)
{
    return Store::open(b, PoolConfig{.pool_prefix = "p"});
}

/// A one-shot backend hook (mirrors the WriteCountingBackend delegation pattern in gtest_cas_store.cpp):
/// it delegates every op to a wrapped Backend, but the FIRST time head(target_key) is called it fires a
/// deleteExact(target_key, condemned_token) AFTER computing the (present) HEAD result and BEFORE returning
/// it — simulating GC's exact-token content delete landing in the writer's HEAD->GET window (B136).
class HeadThenDeleteOnceBackend final : public DB::Cas::Backend
{
public:
    HeadThenDeleteOnceBackend(BackendPtr inner_, String target_key_, DB::Cas::Token condemned_)
        : inner(std::move(inner_)), target_key(std::move(target_key_)), condemned(condemned_) {}

    DB::Cas::HeadResult head(const String & k) override
    {
        const DB::Cas::HeadResult hr = inner->head(k);
        if (k == target_key && !fired)
        {
            fired = true;
            /// GC's single content-delete site, landing in the HEAD->GET window.
            inner->deleteExact(target_key, condemned);
        }
        return hr;
    }

    std::optional<DB::Cas::GetResult> get(const String & k, DB::Cas::Range r = {}) override { return inner->get(k, r); }
    DB::Cas::ListPage list(const String & p, const String & c, size_t l) override { return inner->list(p, c, l); }
    DB::Cas::PutResult putIfAbsent(const String & k, const String & b, const DB::Cas::ObjectMeta & meta = {}) override { return inner->putIfAbsent(k, b, meta); }
    DB::Cas::WriteSinkPtr putIfAbsentStream(const String & k, const DB::Cas::ObjectMeta & meta = {}) override { return inner->putIfAbsentStream(k, meta); }
    DB::Cas::PutResult putOverwrite(const String & k, const String & b, const DB::Cas::Token & e, const DB::Cas::ObjectMeta & meta = {}) override { return inner->putOverwrite(k, b, e, meta); }
    DB::Cas::CasResult casPut(const String & k, const String & b, const std::optional<DB::Cas::Token> & e, const DB::Cas::ObjectMeta & meta = {}) override { return inner->casPut(k, b, e, meta); }
    DB::Cas::DeleteOutcome deleteExact(const String & k, const DB::Cas::Token & t) override { return inner->deleteExact(k, t); }

private:
    BackendPtr inner;
    String target_key;
    DB::Cas::Token condemned;
    bool fired = false;
};

}

TEST(CasBuild, PutBlobWritesEnvelopeWithFixedHeader)
{
    auto b = std::make_shared<InMemoryBackend>();
    auto s = openStore(b);
    auto build = s->startBuild({});
    auto ref = build->putBlob(idOf("hello world"), BlobSource::fromString("hello world"));
    EXPECT_EQ(ref.size, 11u);

    auto raw = b->get(s->layout().blobKey(ref.id));
    ASSERT_TRUE(raw.has_value());
    auto h = decodeEnvelopeHeader(raw->bytes, raw->bytes.size(), ObjectKind::Blob);
    EXPECT_EQ(h.header_len, s->poolMeta().blob_header_len);   /// 256
    EXPECT_EQ(h.logical_size, 11u);
    EXPECT_EQ(u128ToHex(h.logical_hash), ref.id.string());
    EXPECT_EQ(h.domain_id, s->poolMeta().pool_id);
    EXPECT_EQ(h.build_id, build->buildId());
    EXPECT_NE(h.incarnation_tag, UInt128{});
    EXPECT_EQ(raw->bytes.substr(h.header_len), "hello world");
}

/// B171: the `cas_owner` owner-triple stamping (`Build::ownerMeta`) was DELETED — protection is now
/// the build-root precommit edge (reachability), not revocable object metadata GC reads per-candidate.
/// The old `CasBuild.BlobCarriesOwnerTripleInMetadata` asserted that stamping; its coverage is replaced
/// by the build-root precommit/reclaim tests (`CasBuildRoot*`, `CasBuildRootDangle*`), which prove a
/// written-but-unreferenced object is protected by a live precommit and collectable once it is abandoned.

TEST(CasBuild, PutBlobDedupSecondWriterAdopts)
{
    auto b = std::make_shared<InMemoryBackend>();
    auto s = openStore(b);

    auto build_a = s->startBuild({});
    auto ref_a = build_a->putBlob(idOf("dup"), BlobSource::fromString("dup"));
    const Token token_a = b->head(s->layout().blobKey(ref_a.id)).token;

    auto build_b = s->startBuild({});
    auto ref_b = build_b->putBlob(idOf("dup"), BlobSource::fromString("dup"));

    EXPECT_EQ(ref_b.id, ref_a.id);
    /// A's incarnation survives — the second writer adopts, nothing was overwritten.
    EXPECT_EQ(b->head(s->layout().blobKey(ref_a.id)).token, token_a);
}

TEST(CasBuild, PutBlobWrongSizeFailsClosed)
{
    auto b = std::make_shared<InMemoryBackend>();
    auto s = openStore(b);
    auto build = s->startBuild({});

    BlobSource lying;
    lying.size = 11;   /// declares 11 but writes 5
    lying.write_payload = [](DB::WriteBuffer & out) { DB::writeString(std::string_view("short"), out); };

    const BlobId id = idOf("does-not-matter");
    expectThrowsCode(DB::ErrorCodes::LOGICAL_ERROR, [&] { build->putBlob(id, std::move(lying)); });
    /// The cancelled stream created nothing.
    EXPECT_FALSE(b->head(s->layout().blobKey(id)).exists);
}

/// B190: reuseBlob is removed (it had no production callers post-B188). Its behaviors are now covered by:
///   - absent blob / absent-at-gate: RevalidateAbsentBlobDepAbortsRetryable (CasProtocol).
///   - condemned dep at gate:        PublishBodylessCondemnedDepThrowsAbortedRetryable (CasBuild).
///   - evidence tokenless vs tokened: DepIsTokenedDiscriminatesPutBlobVsAdopt (CasBuildReuseBlob).
///   - adoptEvidence lazy-observe:   AdoptedBlobVanishedIsRetryableNotFatal Part A (CasBuild).

TEST(CasBuildReuseBlob, DepIsTokenedDiscriminatesPutBlobVsAdopt)
{
    /// B156b discriminator unit: putBlob records a TOKENED dep (token observed at upload time),
    /// adoptFromTree records a TOKENLESS W-EVIDENCE dep (no token; liveness from the source ref).
    auto b = std::make_shared<InMemoryBackend>();
    auto s = openStore(b);
    DB::Cas::Layout layout("p");

    /// putBlob'd hash ⇒ tokened.
    auto build = s->startBuild({});
    build->putBlob(idOf("written"), BlobSource::fromString("written"));
    EXPECT_TRUE(build->depIsTokened(u128Of("written")));
    EXPECT_TRUE(build->hasDep(u128Of("written")));

    /// Adopted hash ⇒ tokenless. The blob must exist for adoptFromTree's source tree to reference it.
    {
        auto s0 = openStore(b);
        auto b0 = s0->startBuild({});
        b0->putBlob(idOf("adopted"), BlobSource::fromString("adopted"));
    }
    TreeEntry src_entry;
    src_entry.name = "f";
    src_entry.placement = Placement::Blob;
    src_entry.file_hash = u128Of("adopted");
    src_entry.file_size = 7;
    const TreeId source = writeTreeRaw(*b, layout, {src_entry}, s->poolMeta().pool_id);
    build->adoptFromTree(source, "f");
    EXPECT_FALSE(build->depIsTokened(u128Of("adopted")));
    EXPECT_TRUE(build->hasDep(u128Of("adopted")));

    /// Unknown hash ⇒ no dep, not tokened.
    EXPECT_FALSE(build->depIsTokened(u128Of("unknown")));
    EXPECT_FALSE(build->hasDep(u128Of("unknown")));
}

/// B190: ReuseBlobCondemnedThrowsAbortedRetryable is removed (reuseBlob is gone).
/// Condemned-blob-at-gate behavior is covered by:
///   FenceConflictCondemnedBlobDepAbortsRetryable / WedgedHeartbeatCondemnedBlobDepAbortsRetryable
///   / PublishBodylessCondemnedDepThrowsAbortedRetryable (observeAndAdmit throws ABORTED; no GET).

TEST(CasBuild, PutBlobResurrectVanishedReUploadsHeldBody)
{
    auto b = std::make_shared<InMemoryBackend>();

    /// 1. Write payload-X via a throwaway build to create the blob; capture its token t0.
    BlobId id;
    Token t0;
    {
        auto s0 = openStore(b);
        auto build0 = s0->startBuild({});
        id = build0->putBlob(idOf("payload-X"), BlobSource::fromString("payload-X")).id;
        t0 = b->head(s0->layout().blobKey(id)).token;
    }

    /// 2. Condemn (Blob, hash(X), t0) in the retire view.
    DB::Cas::Layout layout("p");
    const String blob_key = layout.blobKey(id);
    injectRetire(*b, layout, /*round*/ 1, /*fence_seq*/ 0, /*shard*/ 0,
        {RetiredEntry{.kind = ObjectKind::Blob, .hash = u128Of("payload-X"), .token = t0, .size = 9}});

    /// 3. Wrap the backend so the NEXT head(blob_key) returns the (present) result and THEN fires
    ///    deleteExact(blob_key, t0) exactly once — GC's delete in the HEAD->GET window. Open a FRESH
    ///    Store over the hook so its retire view (refreshed at open) sees the condemnation.
    auto hook = std::make_shared<HeadThenDeleteOnceBackend>(b, blob_key, t0);
    auto s = Store::open(hook, PoolConfig{.pool_prefix = "p"});
    auto build = s->startBuild({});

    /// 4. putBlob with a re-invokable body.
    ///    BEFORE fix: putIfAbsent -> PreconditionFailed -> observeAndAdmit HEAD (present, condemned)
    ///                -> resurrect GET (vanished, deleted in the window) -> throws FILE_DOESNT_EXIST.
    ///    AFTER fix:  condemned dedup → uploadFromSource directly from held body; the object is
    ///                recreated under a FRESH token. NO GET of the condemned object (INV-1).
    auto ref = build->putBlob(idOf("payload-X"), BlobSource::fromString("payload-X"));
    EXPECT_EQ(ref.id, id);

    /// 5. The blob is present again under a FRESH token, with the same payload; and the condemned token
    ///    never returns (INV-NO-RETURN).
    const HeadResult hr = b->head(blob_key);
    ASSERT_TRUE(hr.exists);
    EXPECT_NE(hr.token, t0);

    auto raw = b->get(blob_key);
    ASSERT_TRUE(raw.has_value());
    auto h = decodeEnvelopeHeader(raw->bytes, raw->bytes.size(), ObjectKind::Blob);
    EXPECT_EQ(h.header_len, s->poolMeta().blob_header_len);
    EXPECT_EQ(raw->bytes.substr(h.header_len), "payload-X");

    EXPECT_EQ(b->deleteExact(blob_key, t0).kind, DeleteOutcome::Kind::TokenMismatch);
}

/// INV-1 (revival-from-source): a condemned blob is NEVER read via GET to revive it.
/// putBlob on a condemned-dedup hit must re-upload from its OWN source bytes — never calling
/// backend().get(blob_key). This test counts backend GETs on the blob key and asserts zero.
TEST(CasBuild, PutBlobCondemnedDedupNeverGetsTheDyingObject)
{
    /// A delegating backend that counts get() calls on a specific key to assert INV-1.
    struct GetCountingBackend final : public DB::Cas::Backend
    {
        explicit GetCountingBackend(BackendPtr inner_, String watched_key_)
            : inner(std::move(inner_)), watched_key(std::move(watched_key_)) {}
        size_t get_count = 0;

        DB::Cas::HeadResult head(const String & k) override { return inner->head(k); }
        std::optional<DB::Cas::GetResult> get(const String & k, DB::Cas::Range r = {}) override
        {
            if (k == watched_key)
                ++get_count;
            return inner->get(k, r);
        }
        DB::Cas::ListPage list(const String & p, const String & c, size_t l) override { return inner->list(p, c, l); }
        DB::Cas::PutResult putIfAbsent(const String & k, const String & bts, const DB::Cas::ObjectMeta & m = {}) override { return inner->putIfAbsent(k, bts, m); }
        DB::Cas::WriteSinkPtr putIfAbsentStream(const String & k, const DB::Cas::ObjectMeta & m = {}) override { return inner->putIfAbsentStream(k, m); }
        DB::Cas::PutResult putOverwrite(const String & k, const String & bts, const DB::Cas::Token & e, const DB::Cas::ObjectMeta & m = {}) override { return inner->putOverwrite(k, bts, e, m); }
        DB::Cas::CasResult casPut(const String & k, const String & bts, const std::optional<DB::Cas::Token> & e, const DB::Cas::ObjectMeta & m = {}) override { return inner->casPut(k, bts, e, m); }
        DB::Cas::DeleteOutcome deleteExact(const String & k, const DB::Cas::Token & tok) override { return inner->deleteExact(k, tok); }
    private:
        BackendPtr inner;
        String watched_key;
    };

    auto b = std::make_shared<InMemoryBackend>();

    /// 1. Upload blob Y via a throwaway build; capture the token t0.
    BlobId id;
    Token t0;
    {
        auto s0 = openStore(b);
        auto build0 = s0->startBuild({});
        id = build0->putBlob(idOf("payload-Y"), BlobSource::fromString("payload-Y")).id;
        t0 = b->head(s0->layout().blobKey(id)).token;
    }

    /// 2. Condemn (Blob, hash(Y), t0) in the retire view, then GC-delete the object so it is absent
    ///    (simulates GC completing the delete before the writer's dedup hit).
    DB::Cas::Layout layout("p");
    const String blob_key = layout.blobKey(id);
    injectRetire(*b, layout, /*round*/ 1, /*fence_seq*/ 0, /*shard*/ 0,
        {RetiredEntry{.kind = ObjectKind::Blob, .hash = u128Of("payload-Y"), .token = t0, .size = 9}});
    b->deleteExact(blob_key, t0);
    ASSERT_FALSE(b->head(blob_key).exists);

    /// 3. Open a fresh Store over a GET-counting wrapper; the retire view sees the condemnation at open.
    auto counting = std::make_shared<GetCountingBackend>(b, blob_key);
    auto s = Store::open(counting, PoolConfig{.pool_prefix = "p"});
    auto build = s->startBuild({});

    /// 4. putBlob Y — the object is absent (was deleted). The dedup-hit (PreconditionFailed) path
    ///    won't fire (object is gone, so putIfAbsentStream → Done on the first attempt).
    ///    However, even if a racing re-creation happens between the check and the upload, the
    ///    condemned branch must NEVER call backend().get(blob_key).
    ///
    ///    Second scenario tested below: object still PRESENT and condemned (GC hasn't deleted it yet).
    auto ref = build->putBlob(idOf("payload-Y"), BlobSource::fromString("payload-Y"));
    EXPECT_EQ(ref.id, id);
    EXPECT_EQ(counting->get_count, 0u) << "INV-1: putBlob must not GET the dying object to revive it";

    const HeadResult hr = b->head(blob_key);
    ASSERT_TRUE(hr.exists);
    EXPECT_NE(hr.token, t0) << "a fresh incarnation must have a new token";
    const auto raw = b->get(blob_key);
    ASSERT_TRUE(raw.has_value());
    const auto hdr = decodeEnvelopeHeader(raw->bytes, raw->bytes.size(), ObjectKind::Blob);
    EXPECT_EQ(raw->bytes.substr(hdr.header_len), "payload-Y");
}

/// INV-1 variant: blob is PRESENT and condemned (GC hasn't fired the delete yet). putBlob dedup-hits
/// it via PreconditionFailed, sees condemned token, and must re-upload from source — NEVER GET.
TEST(CasBuild, PutBlobCondemnedDedupPresentNeverGetsTheDyingObject)
{
    struct GetCountingBackend final : public DB::Cas::Backend
    {
        explicit GetCountingBackend(BackendPtr inner_, String watched_key_)
            : inner(std::move(inner_)), watched_key(std::move(watched_key_)) {}
        size_t get_count = 0;

        DB::Cas::HeadResult head(const String & k) override { return inner->head(k); }
        std::optional<DB::Cas::GetResult> get(const String & k, DB::Cas::Range r = {}) override
        {
            if (k == watched_key)
                ++get_count;
            return inner->get(k, r);
        }
        DB::Cas::ListPage list(const String & p, const String & c, size_t l) override { return inner->list(p, c, l); }
        DB::Cas::PutResult putIfAbsent(const String & k, const String & bts, const DB::Cas::ObjectMeta & m = {}) override { return inner->putIfAbsent(k, bts, m); }
        DB::Cas::WriteSinkPtr putIfAbsentStream(const String & k, const DB::Cas::ObjectMeta & m = {}) override { return inner->putIfAbsentStream(k, m); }
        DB::Cas::PutResult putOverwrite(const String & k, const String & bts, const DB::Cas::Token & e, const DB::Cas::ObjectMeta & m = {}) override { return inner->putOverwrite(k, bts, e, m); }
        DB::Cas::CasResult casPut(const String & k, const String & bts, const std::optional<DB::Cas::Token> & e, const DB::Cas::ObjectMeta & m = {}) override { return inner->casPut(k, bts, e, m); }
        DB::Cas::DeleteOutcome deleteExact(const String & k, const DB::Cas::Token & tok) override { return inner->deleteExact(k, tok); }
    private:
        BackendPtr inner;
        String watched_key;
    };

    auto b = std::make_shared<InMemoryBackend>();

    /// 1. Upload blob Z via a throwaway build; capture the token t0.
    BlobId id;
    Token t0;
    {
        auto s0 = openStore(b);
        auto build0 = s0->startBuild({});
        id = build0->putBlob(idOf("payload-Z"), BlobSource::fromString("payload-Z")).id;
        t0 = b->head(s0->layout().blobKey(id)).token;
    }

    /// 2. Condemn (Blob, hash(Z), t0) — object still PRESENT (GC condemned but not yet deleted).
    DB::Cas::Layout layout("p");
    const String blob_key = layout.blobKey(id);
    injectRetire(*b, layout, /*round*/ 1, /*fence_seq*/ 0, /*shard*/ 0,
        {RetiredEntry{.kind = ObjectKind::Blob, .hash = u128Of("payload-Z"), .token = t0, .size = 9}});
    ASSERT_TRUE(b->head(blob_key).exists) << "blob must be PRESENT for the condemned-present path";

    /// 3. Open a fresh Store over a GET-counting wrapper.
    auto counting = std::make_shared<GetCountingBackend>(b, blob_key);
    auto s = Store::open(counting, PoolConfig{.pool_prefix = "p"});
    auto build = s->startBuild({});

    /// 4. putBlob Z: putIfAbsentStream → PreconditionFailed (object present) → observeAndAdmit →
    ///    sees condemned token → must call uploadFromSource (NOT resurrect/GET).
    ///    The re-upload overwrites the condemned incarnation with a fresh one via putIfAbsentStream
    ///    (which 412s because the object is still present) → uploadFromSource calls observeAndAdmit
    ///    again (adopt the now-present fresh or still-condemned-but-displaced result). The critical
    ///    invariant: backend().get(blob_key) is NEVER called at any point.
    auto ref = build->putBlob(idOf("payload-Z"), BlobSource::fromString("payload-Z"));
    EXPECT_EQ(ref.id, id);
    EXPECT_EQ(counting->get_count, 0u) << "INV-1: putBlob must not GET the condemned object";

    const HeadResult hr = b->head(blob_key);
    ASSERT_TRUE(hr.exists);
    EXPECT_NE(hr.token, t0) << "condemned incarnation must be displaced by a fresh token";
    const auto raw = b->get(blob_key);
    ASSERT_TRUE(raw.has_value());
    const auto hdr = decodeEnvelopeHeader(raw->bytes, raw->bytes.size(), ObjectKind::Blob);
    EXPECT_EQ(raw->bytes.substr(hdr.header_len), "payload-Z");
}

TEST(CasBuild, PutBlobVanishDuringRevivalReUploadsNotFatal)
{
    /// B190 sibling (INV-3): inside uploadFromSource the post-412 path re-observes via the 3-arg
    /// observeAndAdmit. If the object is GC-deleted in the window (present at the conditional PUT
    /// → 412, but gone at the subsequent HEAD), the 3-arg overload throws FILE_DOESNT_EXIST. Before
    /// the fix that escaped putBlob's ABORTED-only retry catch as a FATAL INSERT failure. putBlob
    /// HOLDS the source bytes, so a vanish here must RE-UPLOAD from those bytes within the bounded
    /// retry loop — never fatal. The fix wraps uploadFromSource's two 3-arg observeAndAdmit calls so
    /// FILE_DOESNT_EXIST becomes the retryable ABORTED putBlob already handles.
    ///
    /// A scripted backend reproduces the exact race for the watched key:
    ///   • the first TWO putIfAbsentStream finalize() calls return PreconditionFailed (the object was
    ///     "present" at the conditional PUT, then "re-created" by a racing writer) — modelling S3's 412;
    ///   • the first TWO head() calls return absent (GC deleted the object in the HEAD window).
    /// After the script is exhausted both delegate to the inner backend, so the bounded retry's clean
    /// re-upload lands.
    ///
    /// Trace WITH the fix:
    ///   attempt 0: putIfAbsentStream#1 finalize→412 ; head#1→absent (uploadFromSource line ~378 branch);
    ///              putIfAbsentStream#2 finalize→412 ; observeAndAdmit head#2→absent→ABORTED (wrapped) →
    ///              putBlob catch retries.
    ///   attempt 1: putIfAbsentStream#3 finalize→delegates→Done → putBlob returns. No fatal escape.
    struct ScriptedVanishBackend final : public DB::Cas::Backend
    {
        ScriptedVanishBackend(BackendPtr inner_, String watched_key_)
            : inner(std::move(inner_)), watched_key(std::move(watched_key_)) {}

        size_t head_absent_budget = 2;       /// first N head(watched) calls report absent
        size_t finalize_412_budget = 2;      /// first N finalize() on watched return PreconditionFailed

        /// A sink wrapper that forces PreconditionFailed for the scripted budget, else delegates.
        struct ScriptedSink final : public DB::Cas::WriteSink
        {
            ScriptedSink(WriteSinkPtr inner_, bool force_412_)
                : inner(std::move(inner_)), force_412(force_412_) {}
            DB::WriteBuffer & buffer() override { return inner->buffer(); }
            DB::Cas::PutResult finalize() override
            {
                if (force_412)
                {
                    /// Abandon the underlying upload so the key is never created by it, and report 412.
                    inner->cancel();
                    return {DB::Cas::PutOutcome::PreconditionFailed, {}};
                }
                return inner->finalize();
            }
            void cancel() noexcept override { inner->cancel(); }
            WriteSinkPtr inner;
            bool force_412;
        };

        DB::Cas::HeadResult head(const String & k) override
        {
            if (k == watched_key && head_absent_budget > 0)
            {
                --head_absent_budget;
                return DB::Cas::HeadResult{};   /// exists == false
            }
            return inner->head(k);
        }
        DB::Cas::WriteSinkPtr putIfAbsentStream(const String & k, const DB::Cas::ObjectMeta & meta = {}) override
        {
            const bool force_412 = (k == watched_key && finalize_412_budget > 0);
            if (force_412)
                --finalize_412_budget;
            return std::make_unique<ScriptedSink>(inner->putIfAbsentStream(k, meta), force_412);
        }
        std::optional<DB::Cas::GetResult> get(const String & k, DB::Cas::Range r = {}) override { return inner->get(k, r); }
        DB::Cas::ListPage list(const String & p, const String & c, size_t l) override { return inner->list(p, c, l); }
        DB::Cas::PutResult putIfAbsent(const String & k, const String & bts, const DB::Cas::ObjectMeta & m = {}) override { return inner->putIfAbsent(k, bts, m); }
        DB::Cas::PutResult putOverwrite(const String & k, const String & bts, const DB::Cas::Token & e, const DB::Cas::ObjectMeta & m = {}) override { return inner->putOverwrite(k, bts, e, m); }
        DB::Cas::CasResult casPut(const String & k, const String & bts, const std::optional<DB::Cas::Token> & e, const DB::Cas::ObjectMeta & m = {}) override { return inner->casPut(k, bts, e, m); }
        DB::Cas::DeleteOutcome deleteExact(const String & k, const DB::Cas::Token & t) override { return inner->deleteExact(k, t); }

        BackendPtr inner;
        String watched_key;
    };

    auto raw = std::make_shared<InMemoryBackend>();
    DB::Cas::Layout layout("p");
    const String blob_key = layout.blobKey(idOf("payload-V"));

    /// The blob does NOT need to pre-exist: the scripted backend models the conditional-PUT 412
    /// (object present at PUT time) independently of the inner store, then reports absent on HEAD.
    auto scripted = std::make_shared<ScriptedVanishBackend>(raw, blob_key);
    auto s = Store::open(scripted, PoolConfig{.pool_prefix = "p"});
    auto build = s->startBuild({});

    /// putBlob holds "payload-V" as source bytes. The vanish-during-revival must NOT be fatal:
    ///   BEFORE fix: observeAndAdmit (line ~393) throws FILE_DOESNT_EXIST, escapes putBlob's
    ///               ABORTED-only catch → fatal.
    ///   AFTER fix:  wrapped to ABORTED → putBlob retries → re-uploads from held bytes → succeeds.
    BlobRef ref;
    EXPECT_NO_THROW(ref = build->putBlob(idOf("payload-V"), BlobSource::fromString("payload-V")));
    EXPECT_EQ(ref.id, idOf("payload-V"));

    /// The blob is present with a fresh incarnation and the exact payload — re-uploaded from source.
    const HeadResult hr = raw->head(blob_key);
    ASSERT_TRUE(hr.exists) << "putBlob must have re-uploaded the vanished blob from its held source bytes";
    const auto stored = raw->get(blob_key);
    ASSERT_TRUE(stored.has_value());
    const auto h = decodeEnvelopeHeader(stored->bytes, stored->bytes.size(), ObjectKind::Blob);
    EXPECT_EQ(stored->bytes.substr(h.header_len), "payload-V");
}

TEST(CasBuild, PublishBodylessCondemnedDepThrowsAbortedRetryable)
{
    /// B137/B190: a BODYLESS publish dep (tokenless W-EVIDENCE) whose hash is condemned must surface as
    /// ABORTED ("retry the operation") — a retryable transient, matching the sibling lost-dependency
    /// branches — NOT a hard FILE_DOESNT_EXIST (which became an HTTP-500 INSERT failure). Under INV-1 the
    /// gate never reads the dying object: observeAndAdmit does a HEAD only; a condemned HEAD ⇒ ABORTED
    /// (the caller retries from source). Even with a concurrent exact-token delete racing the HEAD, the
    /// outcome is the same retryable ABORTED.
    auto b = std::make_shared<InMemoryBackend>();

    /// 1. Write payload-X via a throwaway build to create the blob object; capture its token t0.
    BlobId id;
    Token t0;
    {
        auto s0 = openStore(b);
        auto build0 = s0->startBuild({});
        id = build0->putBlob(idOf("payload-X"), BlobSource::fromString("payload-X")).id;
        t0 = b->head(s0->layout().blobKey(id)).token;
    }

    /// 2. A source tree referencing blob hash(X) by name. adoptFromTree records a TOKENLESS (evidence)
    ///    Blob dep on hash(X) WITHOUT holding the body — exactly the bodyless dependency B137 is about.
    DB::Cas::Layout layout("p");
    const String blob_key = layout.blobKey(id);
    TreeEntry src_entry;
    src_entry.name = "data.bin";
    src_entry.placement = Placement::Blob;
    src_entry.file_hash = u128Of("payload-X");
    src_entry.file_size = 9;
    const TreeId source = writeTreeRaw(*b, layout, {src_entry}, openStore(b)->poolMeta().pool_id);

    /// 3. Condemn (Blob, hash(X), t0) in the retire view, so the publish gate sees the evidence dep as a
    ///    condemned hit and must resolve it (observeAndAdmit -> HEAD-only, no GET).
    injectRetire(*b, layout, /*round*/ 1, /*fence_seq*/ 0, /*shard*/ 0,
        {RetiredEntry{.kind = ObjectKind::Blob, .hash = u128Of("payload-X"), .token = t0, .size = 9}});

    /// 4. Wrap the backend so the NEXT head(blob_key) returns the (present) result and THEN fires
    ///    deleteExact(blob_key, t0) exactly once — GC's exact-token delete racing the gate's HEAD.
    ///    Open a FRESH Store over the hook so its retire view (refreshed at open) sees the condemnation.
    ///    The FIRST head(blob_key) in this build is the one inside the gate's observeAndAdmit
    ///    (adoptFromTree only reads the source tree; putTree of the new tree does not head the blob), so
    ///    the one-shot fires precisely in that HEAD window.
    auto hook = std::make_shared<HeadThenDeleteOnceBackend>(b, blob_key, t0);
    auto s = Store::open(hook, PoolConfig{.pool_prefix = "p"});
    auto build = s->startBuild({});

    /// 5. Adopt the blob as a tokenless evidence dep, then build a NEW tree referencing it. No body in hand.
    const TreeEntry adopted = build->adoptFromTree(source, "data.bin");
    const TreeId tree = build->putTree({adopted});

    /// 6. Publish drives the bodyless gate: tokenless+condemned -> observeAndAdmit head(blob_key) [hook
    ///    fires deleteExact(blob_key, t0)] -> condemned (or vanished) HEAD -> ABORTED, no GET (INV-1).
    ///    BEFORE fix: FILE_DOESNT_EXIST (hard). AFTER fix: ABORTED "retry the operation" (retryable).
    expectThrowsCode(DB::ErrorCodes::ABORTED,
        [&] { build->publish(RootNamespace{"srv1/tbl"}, "part_1", tree, RefPayload{}); });
}

TEST(CasBuild, GateBodylessAdoptFullyDeletedObjectThrowsAbortedNotFatal)
{
    /// B190 residual (soak bug): a TOKENLESS (W-EVIDENCE) adopt dep whose object is FULLY GC-DELETED
    /// (HEAD absent, not merely condemned-present) must throw ABORTED from the gate — not the old fatal
    /// FILE_DOESNT_EXIST that the 3-arg observeAndAdmit used to emit.
    ///
    /// Scenario:
    ///   1. Blob X exists with token t0; a tokenless dep is recorded via adoptFromTree (no body in hand).
    ///   2. GC condemns X (retire view hit by hash) AND fully deletes the object (deleteExact → HEAD absent).
    ///   3. The publish gate runs: tokenless dep + view hit → calls observeAndAdmit(3-arg) → HEAD →
    ///      absent → should throw ABORTED (retryable, INV-3), NOT FILE_DOESNT_EXIST (fatal).
    ///
    /// The gate's absent-object path is distinct from the condemned-present path tested by
    /// PublishBodylessCondemnedDepThrowsAbortedRetryable (which fires GC delete AFTER the HEAD via
    /// HeadThenDeleteOnceBackend). HERE the object is absent BEFORE the HEAD call.
    auto b = std::make_shared<InMemoryBackend>();

    /// 1. Write payload-B190 via a throwaway build; capture its token t0.
    BlobId id;
    Token t0;
    {
        auto s0 = openStore(b);
        auto build0 = s0->startBuild({});
        id = build0->putBlob(idOf("payload-B190"), BlobSource::fromString("payload-B190")).id;
        t0 = b->head(s0->layout().blobKey(id)).token;
    }

    DB::Cas::Layout layout("p");
    const String blob_key = layout.blobKey(id);

    /// 2. A source tree referencing blob hash(B190) by name — adoptFromTree records a TOKENLESS dep.
    TreeEntry src_entry;
    src_entry.name = "data.bin";
    src_entry.placement = Placement::Blob;
    src_entry.file_hash = u128Of("payload-B190");
    src_entry.file_size = 11;
    const TreeId source = writeTreeRaw(*b, layout, {src_entry}, openStore(b)->poolMeta().pool_id);

    /// 3. Condemn (Blob, hash(B190), t0) in the retire view AND immediately GC-delete the object.
    ///    The retire entry stays present (GC has not yet confirmed the outcome and dropped it), so
    ///    `checkAndResolveDeps` will find a view hit by hash — triggering the observeAndAdmit path.
    ///    The object is now absent: HEAD will return exists=false.
    injectRetire(*b, layout, /*round*/ 1, /*fence_seq*/ 0, /*shard*/ 0,
        {RetiredEntry{.kind = ObjectKind::Blob, .hash = u128Of("payload-B190"), .token = t0, .size = 11}});
    ASSERT_EQ(b->deleteExact(blob_key, t0).kind, DeleteOutcome::Kind::Deleted);
    ASSERT_FALSE(b->head(blob_key).exists) << "object must be absent before the gate HEAD";

    /// 4. Open a fresh Store over the raw backend — retire view (refreshed at open) sees the condemnation.
    auto s = Store::open(b, PoolConfig{.pool_prefix = "p"});
    auto build = s->startBuild({});

    /// 5. Adopt the blob as a tokenless evidence dep; build a NEW tree referencing it. No body in hand.
    const TreeEntry adopted = build->adoptFromTree(source, "data.bin");
    const TreeId tree = build->putTree({adopted});

    /// 6. Publish drives the gate:
    ///    tokenless dep + view hit → observeAndAdmit(3-arg) → HEAD → absent → ABORTED (INV-3).
    ///    BEFORE fix: threw FILE_DOESNT_EXIST "object ... absent — cannot reuse" → fatal INSERT failure.
    ///    AFTER fix:  throws ABORTED (retryable) — the outer INSERT retries and re-materializes from source.
    expectThrowsCode(DB::ErrorCodes::ABORTED,
        [&] { build->publish(RootNamespace{"srv1/tbl"}, "part_1", tree, RefPayload{}); });

    /// No ref was published and the blob stays absent.
    EXPECT_FALSE(s->resolveRef(RootNamespace{"srv1/tbl"}, "part_1").has_value());
    EXPECT_FALSE(b->head(blob_key).exists);
}

TEST(CasBuild, PutTreeEnforcesBottomUp)
{
    auto b = std::make_shared<InMemoryBackend>();
    auto s = openStore(b);
    auto build = s->startBuild({});

    /// A blob entry referencing an unknown hash → W-TREE-BUILD violation.
    TreeEntry blob_entry;
    blob_entry.name = "data.bin";
    blob_entry.placement = Placement::Blob;
    blob_entry.file_hash = u128Of("blob-content");
    blob_entry.file_size = 12;
    expectThrowsCode(DB::ErrorCodes::LOGICAL_ERROR, [&] { build->putTree({blob_entry}); });

    /// After putBlob of that hash → putTree succeeds.
    build->putBlob(idOf("blob-content"), BlobSource::fromString("blob-content"));
    const TreeId child = build->putTree({blob_entry});

    /// A Subtree entry referencing an unknown child tree → violation.
    TreeEntry subtree_entry;
    subtree_entry.name = "sub";
    subtree_entry.placement = Placement::Subtree;
    subtree_entry.file_hash = u128Of("unknown-child-tree");
    subtree_entry.file_size = 1;
    expectThrowsCode(DB::ErrorCodes::LOGICAL_ERROR, [&] { build->putTree({subtree_entry}); });

    /// adoptTree(child) records the dep; then a Subtree pointing at it succeeds.
    build->adoptTree(child);
    TreeEntry good_subtree;
    good_subtree.name = "sub";
    good_subtree.placement = Placement::Subtree;
    good_subtree.file_hash = DB::Cas::hexToU128(child.string());
    good_subtree.file_size = 1;
    EXPECT_NO_THROW(build->putTree({good_subtree}));

    /// An Inline-only tree needs no deps.
    TreeEntry inline_entry;
    inline_entry.name = "small";
    inline_entry.placement = Placement::Inline;
    inline_entry.inline_bytes = "abc";
    inline_entry.file_size = 3;
    EXPECT_NO_THROW(build->putTree({inline_entry}));
}

TEST(CasBuild, AdoptFromTreeRecordsEvidence)
{
    auto b = std::make_shared<InMemoryBackend>();
    auto s = openStore(b);

    /// Build a source tree with a Blob entry "data.bin".
    DB::Cas::Layout layout("p");
    TreeEntry src_entry;
    src_entry.name = "data.bin";
    src_entry.placement = Placement::Blob;
    src_entry.file_hash = u128Of("source-blob");
    src_entry.file_size = 11;
    const TreeId source = writeTreeRaw(*b, layout, {src_entry}, s->poolMeta().pool_id);

    auto build = s->startBuild({});

    /// Unknown name → BAD_ARGUMENTS.
    expectThrowsCode(DB::ErrorCodes::BAD_ARGUMENTS, [&] { build->adoptFromTree(source, "missing"); });

    /// The entry is returned and a tokenless dep recorded — observed indirectly: a subsequent putTree
    /// referencing that blob hash must NOT throw W-TREE-BUILD.
    const TreeEntry adopted = build->adoptFromTree(source, "data.bin");
    EXPECT_EQ(adopted.name, "data.bin");
    EXPECT_EQ(adopted.file_hash, u128Of("source-blob"));

    TreeEntry reuse_entry = adopted;
    EXPECT_NO_THROW(build->putTree({reuse_entry}));
}

TEST(CasBuild, AbandonLeavesDebrisAndDisables)
{
    auto b = std::make_shared<InMemoryBackend>();
    auto s = openStore(b);
    auto build = s->startBuild({});

    auto blob_ref = build->putBlob(idOf("kept"), BlobSource::fromString("kept"));
    TreeEntry entry;
    entry.name = "f";
    entry.placement = Placement::Blob;
    entry.file_hash = u128Of("kept");
    entry.file_size = 4;
    const TreeId tree = build->putTree({entry});

    build->abandon();

    /// Objects still present (debris — full GC's job via min_active).
    EXPECT_TRUE(b->head(s->layout().blobKey(blob_ref.id)).exists);
    EXPECT_TRUE(b->head(s->layout().treeKey(tree)).exists);

    /// Further operations throw via requireAlive.
    expectThrowsCode(DB::ErrorCodes::LOGICAL_ERROR,
        [&] { build->putBlob(idOf("after"), BlobSource::fromString("after")); });
    expectThrowsCode(DB::ErrorCodes::LOGICAL_ERROR, [&] { build->putTree({entry}); });
    expectThrowsCode(DB::ErrorCodes::LOGICAL_ERROR,
        [&] { build->publish(RootNamespace("ns"), "ref", tree, RefPayload{}); });
}

TEST(CasBuild, PublishHappyPathRoundTrip)
{
    auto b = std::make_shared<InMemoryBackend>();
    auto s = openStore(b);
    auto build = s->startBuild({});
    auto blob = build->putBlob(idOf("hello world"), BlobSource::fromString("hello world"));
    EXPECT_EQ(blob.size, 11u);

    std::vector<TreeEntry> entries;
    TreeEntry e;
    e.name = "data.bin";
    e.placement = Placement::Blob;
    e.file_hash = u128Of("hello world");
    e.file_size = 11;
    entries.push_back(e);
    auto tree = build->putTree(entries);

    RefPayload payload;
    payload.mutable_files["txn_version.txt"] = "1";
    build->publish(RootNamespace{"srv1/tbl"}, "part_1", tree, payload);

    auto r = s->resolveRef(RootNamespace{"srv1/tbl"}, "part_1");
    ASSERT_TRUE(r.has_value());
    EXPECT_EQ(r->tree_id, tree);
    EXPECT_EQ(r->mutable_files.at("txn_version.txt"), "1");

    auto read = s->readTree(tree);
    ASSERT_EQ(read.size(), 1u);
    auto loc = s->locate(read[0]);
    auto got = b->get(loc.key, Range{loc.offset, loc.length});
    ASSERT_TRUE(got.has_value());
    EXPECT_EQ(got->bytes, "hello world");

    /// journal: read the shard manifest raw, assert journal.back() == {Add, "part_1", tree, at_version}
    /// with at_version == shard_version.
    const uint64_t shard = shardOfForTest("part_1", s->poolMeta().root_shards);
    auto manifest = b->get(s->layout().rootShardKey(RootNamespace{"srv1/tbl"}, shard));
    ASSERT_TRUE(manifest.has_value());
    const RootShard root = decodeRootShard(manifest->bytes);
    ASSERT_FALSE(root.journal.empty());
    const JournalRecord & last = root.journal.back();
    EXPECT_EQ(last.op, JournalRecord::Op::Add);
    EXPECT_EQ(last.ref_name, "part_1");
    EXPECT_EQ(u128ToHex(last.tree_id), tree.string());
    EXPECT_EQ(last.at_version, root.shard_version);
}

TEST(CasBuild, PublishRequiresTreeInDepSet)
{
    auto b = std::make_shared<InMemoryBackend>();
    auto s = openStore(b);
    auto build = s->startBuild({});

    /// A TreeId this build never built or adopted → LOGICAL_ERROR (root not in the W-DEP-SET).
    const TreeId stranger{u128ToHex(u128Of("nope"))};
    expectThrowsCode(DB::ErrorCodes::LOGICAL_ERROR,
        [&] { build->publish(RootNamespace{"srv1/tbl"}, "part_1", stranger, RefPayload{}); });
}

TEST(CasBuild, PublishOwnThreadConflictRetries)
{
    auto b = std::make_shared<InMemoryBackend>();
    auto s = openStore(b);
    auto build = s->startBuild({});

    build->putBlob(idOf("hello world"), BlobSource::fromString("hello world"));
    TreeEntry e;
    e.name = "data.bin";
    e.placement = Placement::Blob;
    e.file_hash = u128Of("hello world");
    e.file_size = 11;
    auto tree = build->putTree({e});

    /// One artificial Conflict on the shard's first casPut (no fence advance; the view is untouched).
    /// mutateShard re-reads + re-runs the lambda and lands on the retry.
    const uint64_t shard = shardOfForTest("part_1", s->poolMeta().root_shards);
    b->failNextCasPut(s->layout().rootShardKey(RootNamespace{"srv1/tbl"}, shard));

    build->publish(RootNamespace{"srv1/tbl"}, "part_1", tree, RefPayload{});

    auto r = s->resolveRef(RootNamespace{"srv1/tbl"}, "part_1");
    ASSERT_TRUE(r.has_value());
    EXPECT_EQ(r->tree_id, tree);
}

TEST(CasBuild, PublishIntoSecondNamespaceSameTree)
{
    auto b = std::make_shared<InMemoryBackend>();
    auto s = openStore(b);
    auto build = s->startBuild({});

    auto blob = build->putBlob(idOf("hello world"), BlobSource::fromString("hello world"));
    TreeEntry e;
    e.name = "data.bin";
    e.placement = Placement::Blob;
    e.file_hash = u128Of("hello world");
    e.file_size = 11;
    auto tree = build->putTree({e});

    const String blob_key = s->layout().blobKey(blob.id);
    const String tree_key = s->layout().treeKey(tree);
    const Token blob_token = b->head(blob_key).token;
    const Token tree_token = b->head(tree_key).token;

    /// The SAME tree published as "part_1" in two namespaces — the second publish must NOT re-upload
    /// (the tree dep is already present); both refs resolve to the same tree, a single object set.
    build->publish(RootNamespace{"srv1/tbl"}, "part_1", tree, RefPayload{});
    build->publish(RootNamespace{"srv1/tbl/detached"}, "part_1", tree, RefPayload{});

    auto r1 = s->resolveRef(RootNamespace{"srv1/tbl"}, "part_1");
    auto r2 = s->resolveRef(RootNamespace{"srv1/tbl/detached"}, "part_1");
    ASSERT_TRUE(r1.has_value());
    ASSERT_TRUE(r2.has_value());
    EXPECT_EQ(r1->tree_id, tree);
    EXPECT_EQ(r2->tree_id, tree);

    /// The blob/tree objects were uploaded once: their tokens are unchanged after both publishes.
    EXPECT_EQ(b->head(blob_key).token, blob_token);
    EXPECT_EQ(b->head(tree_key).token, tree_token);
}

TEST(CasBuild, TwoBuildsPublishToSameShardSerialize)
{
    auto b = std::make_shared<InMemoryBackend>();
    auto s = openStore(b);
    const uint64_t root_shards = s->poolMeta().root_shards;

    /// Find two distinct ref names that map to the SAME shard.
    String ref1;
    String ref2;
    {
        std::map<uint64_t, String> seen;
        for (char c = 'a'; c <= 'z' && ref2.empty(); ++c)
        {
            const String name(1, c);
            const uint64_t sh = shardOfForTest(name, root_shards);
            auto it = seen.find(sh);
            if (it != seen.end())
            {
                ref1 = it->second;
                ref2 = name;
            }
            else
            {
                seen.emplace(sh, name);
            }
        }
    }
    ASSERT_FALSE(ref1.empty());
    ASSERT_FALSE(ref2.empty());
    ASSERT_EQ(shardOfForTest(ref1, root_shards), shardOfForTest(ref2, root_shards));

    const RootNamespace ns{"srv1/tbl"};

    /// Build A publishes ref1.
    auto build_a = s->startBuild({});
    build_a->putBlob(idOf("content-a"), BlobSource::fromString("content-a"));
    TreeEntry ea;
    ea.name = "data.bin";
    ea.placement = Placement::Blob;
    ea.file_hash = u128Of("content-a");
    ea.file_size = 9;
    auto tree_a = build_a->putTree({ea});
    build_a->publish(ns, ref1, tree_a, RefPayload{});

    /// Build B publishes ref2 into the same shard: its mutateShard sees A's manifest (shard_version
    /// advanced past 0), re-reads, and lands. The single artificial conflict forces B to genuinely
    /// re-read after the first attempt.
    auto build_b = s->startBuild({});
    build_b->putBlob(idOf("content-b"), BlobSource::fromString("content-b"));
    TreeEntry eb;
    eb.name = "data.bin";
    eb.placement = Placement::Blob;
    eb.file_hash = u128Of("content-b");
    eb.file_size = 9;
    auto tree_b = build_b->putTree({eb});

    const uint64_t shard = shardOfForTest(ref2, root_shards);
    b->failNextCasPut(s->layout().rootShardKey(ns, shard));
    build_b->publish(ns, ref2, tree_b, RefPayload{});

    /// Both refs resolve.
    auto r1 = s->resolveRef(ns, ref1);
    auto r2 = s->resolveRef(ns, ref2);
    ASSERT_TRUE(r1.has_value());
    ASSERT_TRUE(r2.has_value());
    EXPECT_EQ(r1->tree_id, tree_a);
    EXPECT_EQ(r2->tree_id, tree_b);

    /// The shared manifest holds both refs and both Add journal records.
    auto manifest = b->get(s->layout().rootShardKey(ns, shard));
    ASSERT_TRUE(manifest.has_value());
    const RootShard root = decodeRootShard(manifest->bytes);
    EXPECT_TRUE(root.refs.contains(ref1));
    EXPECT_TRUE(root.refs.contains(ref2));
    size_t adds = 0;
    for (const JournalRecord & rec : root.journal)
        if (rec.op == JournalRecord::Op::Add)
            ++adds;
    EXPECT_EQ(adds, 2u);
}

TEST(CasBuild, FirstPublishRegistersNamespace)
{
    /// W-REGISTER (spec section 5, decision 2026-06-12): the first publish into a namespace
    /// CAS-appends it to `gc/registry` BEFORE the manifest exists; later publishes into the same
    /// namespace hit the Store's monotone cache and leave the registry untouched.
    auto b = std::make_shared<InMemoryBackend>();
    auto s = Store::open(b, PoolConfig{.pool_prefix = "p"});
    auto build = s->startBuild({});
    auto blob = build->putBlob(idOf("reg-payload"), BlobSource::fromString("reg-payload"));
    std::vector<TreeEntry> entries;
    TreeEntry e;
    e.name = "f";
    e.placement = Placement::Blob;
    e.file_hash = u128Of("reg-payload");
    e.file_size = 11;
    entries.push_back(e);
    auto tree = build->putTree(entries);

    EXPECT_FALSE(b->get(s->layout().rootsRegistryKey()).has_value());
    build->publish(RootNamespace{"srv9/fresh"}, "part_1", tree, RefPayload{});

    const auto got = b->get(s->layout().rootsRegistryKey());
    ASSERT_TRUE(got.has_value());
    const RootsRegistry registry = decodeRootsRegistry(got->bytes);
    EXPECT_TRUE(registry.namespaces.contains("srv9/fresh"));
    const uint64_t version_after_first = registry.registry_version;

    /// second publish into the same namespace: cache hit, registry untouched
    build->publish(RootNamespace{"srv9/fresh"}, "part_2", tree, RefPayload{});
    const RootsRegistry again = decodeRootsRegistry(b->get(s->layout().rootsRegistryKey())->bytes);
    EXPECT_EQ(again.registry_version, version_after_first);
}

TEST(CasBuild, AdoptEvidenceNoBackendOp)
{
    /// B188: adoptEvidence records a TOKENLESS W-EVIDENCE dep from an already-resolved TreeEntry
    /// WITHOUT any backend call (no HEAD, no GET, no PUT).
    ///
    /// Two behavioural assertions:
    ///   1. No backend HEAD or stream_put fires during adoptEvidence (counted via CountingBackend).
    ///   2. The recorded dep is usable by putTree: after adoptEvidence(entry), calling putTree({entry})
    ///      does NOT throw LOGICAL_ERROR (W-TREE-BUILD enforces "every child in dep set").

    /// tests::CountingBackend (cas_test_helpers.h) extends InMemoryBackend directly and only intercepts
    /// head/get/putIfAbsent — NOT putIfAbsentStream. This test needs to prove adoptEvidence touches the
    /// backend on no path at all, including the streaming write path that putBlob/putTree use, so we use
    /// a delegating wrapper that intercepts putIfAbsentStream (and head/get) over an inner InMemoryBackend.
    struct LocalCountingBackend final : public Backend
    {
        explicit LocalCountingBackend(BackendPtr inner_) : inner(std::move(inner_)) {}
        size_t heads = 0;
        size_t stream_puts = 0;
        size_t gets = 0;

        HeadResult head(const String & k) override { ++heads; return inner->head(k); }
        WriteSinkPtr putIfAbsentStream(const String & k, const ObjectMeta & meta = {}) override
        {
            ++stream_puts;
            return inner->putIfAbsentStream(k, meta);
        }
        std::optional<GetResult> get(const String & k, Range r = {}) override { ++gets; return inner->get(k, r); }
        ListPage list(const String & p, const String & c, size_t l) override { return inner->list(p, c, l); }
        PutResult putIfAbsent(const String & k, const String & bts, const ObjectMeta & m = {}) override { return inner->putIfAbsent(k, bts, m); }
        PutResult putOverwrite(const String & k, const String & bts, const Token & e, const ObjectMeta & m = {}) override { return inner->putOverwrite(k, bts, e, m); }
        CasResult casPut(const String & k, const String & bts, const std::optional<Token> & e, const ObjectMeta & m = {}) override { return inner->casPut(k, bts, e, m); }
        DeleteOutcome deleteExact(const String & k, const Token & t) override { return inner->deleteExact(k, t); }
    private:
        BackendPtr inner;
    };

    /// 1. Set up the store: use a raw InMemoryBackend for setup writes, wrap it in the counting
    ///    decorator only for the Build that will call adoptEvidence.
    auto raw = std::make_shared<InMemoryBackend>();

    /// Upload the blob content so putTree's own putIfAbsentStream (for the tree object) succeeds.
    /// The blob itself must exist on the backend so that putTree can upload the tree that references
    /// it. We upload via a throwaway build using the raw backend (not the counting backend), so the
    /// setup counts don't pollute the test's counter.
    {
        auto s0 = openStore(raw);
        auto b0 = s0->startBuild({});
        b0->putBlob(idOf("b188-content"), BlobSource::fromString("b188-content"));
    }

    /// 2. Wrap the backend in the counting decorator and open a FRESH Store over it.
    auto counting = std::make_shared<LocalCountingBackend>(raw);
    auto s = Store::open(counting, PoolConfig{.pool_prefix = "p"});
    auto build = s->startBuild({});

    /// 3. Construct a Blob TreeEntry (the same content as above). adoptEvidence is called on a
    ///    hand-crafted TreeEntry rather than going through adoptFromTree — that IS the B188 interface.
    TreeEntry entry;
    entry.name = "b188.bin";
    entry.placement = Placement::Blob;
    entry.file_hash = u128Of("b188-content");
    entry.file_size = 12;

    /// Reset the counters after Store::open (which may HEAD gc/state etc. during retireView refresh).
    counting->heads = 0;
    counting->stream_puts = 0;
    counting->gets = 0;

    /// 4. Call adoptEvidence — must record the dep WITHOUT touching the backend.
    EXPECT_NO_THROW(build->adoptEvidence(entry));

    /// Assertion 1: no HEAD, no GET, no stream_put during adoptEvidence.
    EXPECT_EQ(counting->heads, 0u) << "adoptEvidence must not HEAD the backend";
    EXPECT_EQ(counting->stream_puts, 0u) << "adoptEvidence must not PUT to the backend";
    EXPECT_EQ(counting->gets, 0u) << "adoptEvidence must not GET from the backend";

    /// Assertion 2: dep is recorded — putTree({entry}) must succeed (W-TREE-BUILD passes).
    /// putTree WILL call putIfAbsentStream to upload the tree object, but that is expected and
    /// separate from the adoptEvidence call itself.
    EXPECT_NO_THROW(build->putTree({entry}));

    /// Inline entry: adoptEvidence records nothing (Inline has no standalone object).
    TreeEntry inline_entry;
    inline_entry.name = "small";
    inline_entry.placement = Placement::Inline;
    inline_entry.inline_bytes = "xy";
    inline_entry.file_size = 2;
    EXPECT_NO_THROW(build->adoptEvidence(inline_entry));
    /// putTree with inline only also succeeds (no dep needed for Inline).
    EXPECT_NO_THROW(build->putTree({inline_entry}));

    /// Subtree branch: build a REAL child tree (the one putTree({entry}) created above is a valid tree),
    /// then on a FRESH build over a FRESH counting backend adoptEvidence a Subtree entry pointing at it
    /// and assert ZERO backend ops — same tokenless-no-IO contract as the Blob case. We re-derive the
    /// child tree id deterministically by re-encoding the single-blob tree.
    const TreeId child_tree = merkleTreeId({entry});
    {
        auto counting2 = std::make_shared<LocalCountingBackend>(raw);
        auto s2 = Store::open(counting2, PoolConfig{.pool_prefix = "p"});
        auto build2 = s2->startBuild({});

        TreeEntry subtree_entry;
        subtree_entry.name = "sub";
        subtree_entry.placement = Placement::Subtree;
        subtree_entry.file_hash = hexToU128(child_tree.string());
        subtree_entry.file_size = 1;

        counting2->heads = 0;
        counting2->stream_puts = 0;
        counting2->gets = 0;
        EXPECT_NO_THROW(build2->adoptEvidence(subtree_entry));
        EXPECT_EQ(counting2->heads, 0u) << "adoptEvidence(Subtree) must not HEAD the backend";
        EXPECT_EQ(counting2->stream_puts, 0u) << "adoptEvidence(Subtree) must not PUT to the backend";
        EXPECT_EQ(counting2->gets, 0u) << "adoptEvidence(Subtree) must not GET from the backend";
    }
}

TEST(CasBuild, StageTreeRetainsAndDefersUpload)
{
    auto b = std::make_shared<InMemoryBackend>();
    auto s = openStore(b);
    auto build = s->startBuild({});
    build->putBlob(idOf("x"), BlobSource::fromString("x"));   /// child must be a dep first
    TreeEntry e;
    e.name = "f";
    e.placement = Placement::Blob;
    e.file_hash = u128Of("x");
    e.file_size = 1;

    const TreeId t = build->stageTree({e});
    /// staged: payload retained + dep recorded, but the tree OBJECT is NOT uploaded yet.
    EXPECT_FALSE(b->head(s->layout().treeKey(t)).exists);

    /// precommit accepts the staged tree even though its object is absent: stageTree recorded the
    /// (tokenless) Tree dep, which is exactly precommit's precondition.
    EXPECT_NO_THROW(build->precommit(t));

    build->uploadStagedTree(t);
    EXPECT_TRUE(b->head(s->layout().treeKey(t)).exists);
}

TEST(CasBuild, ConvergesUnderProductiveGc)
{
    /// B167/B171 LIVENESS — the re-upload/condemn livelock, now closed by the build-root precommit edge.
    ///
    /// THE BUG (before the fix): a blob H was referenced, dropped, and GC-condemned (everEdged ∧ InDeg=0,
    /// condemned in the retire view). A NEW build dedup-HITS H by content and must re-upload it from
    /// source — it re-streams a FRESH incarnation of H. But the productive GC, re-deriving H as a zero-in-degree
    /// candidate every round (zeroInDegreeKnown is stateless), kept RE-CONDEMNING and exact-token-DELETING
    /// that fresh incarnation in the build's upload→publish window. The build never converged: every retry
    /// re-uploaded only to have GC delete it again → livelock → broken/detached parts in soak.
    ///
    /// THE FIX (B171): protection is the build-root PRECOMMIT EDGE, not a `cas_owner` watermark hint. Build
    /// B precommits its manifest tree (naming H) BEFORE the adversarial loop, so the GC fold lifts H to
    /// in-degree ≥ 1 — H is never even a zero-in-degree candidate and is SPARED every round until B
    /// publishes (the table ref pins H, then the precommit is removed) → convergence. (The earlier fix used
    /// `protectedByLiveBuild` on per-object `cas_owner` metadata; both were deleted in B171 — see the
    /// `CasBuildRoot*` tests for the reclaim of an ABANDONED precommit.)
    ///
    /// FORM: full adversarial loop. A real Gc drives complete runRegularRound rounds against the same
    /// pool while build B holds an active watermark covering H's incarnation. We assert H is SPARED
    /// every round and that B publishes within a BOUNDED number of GC rounds, after which H reads back.
    auto b = std::make_shared<InMemoryBackend>();

    /// 1. Build A creates H ("shared-content"), publishes a part referencing it, then drops the ref.
    ///    Capture H's first incarnation token so we can condemn exactly it.
    PoolConfig cfg;
    cfg.pool_prefix = "p";
    cfg.server_id = UInt128(0xAB);
    cfg.background_heartbeats = false;
    const String content = "shared-content";
    BlobId h;
    Token h_token0;
    {
        auto s0 = Store::open(b, cfg);
        auto build_a = s0->startBuild({});
        h = build_a->putBlob(idOf(content), BlobSource::fromString(content)).id;
        TreeEntry e;
        e.name = "f";
        e.placement = Placement::Blob;
        e.file_hash = u128Of(content);
        e.file_size = content.size();
        const TreeId tree_a = build_a->putTree({e});
        build_a->publish(RootNamespace{"srv1/tbl"}, "part_1", tree_a, RefPayload{});
        h_token0 = b->head(s0->layout().blobKey(h)).token;
        s0->dropRef(RootNamespace{"srv1/tbl"}, "part_1");
    }

    /// 2. Condemn (Blob, H, h_token0) in the retire view. A fresh Store::open below refreshes its
    ///    retire view at open and sees the condemnation, so the dedup-hit on H takes the
    ///    uploadFromSource (re-stream-a-fresh-incarnation-from-source) path rather than free adoption.
    ///    INV-1 (B190): no resurrect-by-GET; putBlob re-uploads from its OWN source bytes.
    DB::Cas::Layout layout("p");
    injectRetire(*b, layout, /*round*/ 1, /*fence_seq*/ 0, /*shard*/ 0,
        {RetiredEntry{.kind = ObjectKind::Blob, .hash = u128Of(content), .token = h_token0,
                      .size = content.size()}});

    /// 3. Open the live Store and start build B. B dedup-hits the condemned H and re-uploads from
    ///    source (uploadFromSource via putBlob): a fresh incarnation, a NEW token. B stays ACTIVE for
    ///    the whole adversarial loop — its build_seq is never retired below.
    auto s = Store::open(b, cfg);
    const String blob_key = s->layout().blobKey(h);
    auto build_b = s->startBuild({});

    /// B190: use putBlob (holds source bytes) instead of reuseBlob (has no source bytes).
    /// putBlob detects the condemned dedup hit and calls uploadFromSource — no GET of dying object.
    const auto ref_b = build_b->putBlob(h, BlobSource::fromString(content));
    ASSERT_EQ(ref_b.id, h);

    const HeadResult after_reupload = b->head(blob_key);
    ASSERT_TRUE(after_reupload.exists);
    EXPECT_NE(after_reupload.token, h_token0);   /// a genuinely fresh incarnation
    /// B171: the re-uploaded incarnation no longer carries a `cas_owner` triple (stamping was deleted).
    /// Protection is now the build-root PRECOMMIT EDGE: B assembles its manifest tree naming H and
    /// precommits it BEFORE the adversarial GC loop, so H has in-degree ≥ 1 from the build-root fold and
    /// is never a zero-in-degree candidate — the reachability replacement for the old watermark hint.

    /// 4. Build B assembles its tree referencing H and PRECOMMITS it (build-root edge). H is now protected
    ///    by reachability, not by `cas_owner` — the precommit is the new upload→publish-window protection.
    TreeEntry eb;
    eb.name = "f";
    eb.placement = Placement::Blob;
    eb.file_hash = u128Of(content);
    eb.file_size = content.size();
    const TreeId tree_b = build_b->putTree({eb});
    build_b->precommit(tree_b);

    /// 5. THE ADVERSARIAL LOOP. A real, productive GC keeps trying to reclaim. Round 1 reclaims the
    ///    now-unreferenced tree_a (the finished build A's manifest — UNprotected) but H stays pinned by
    ///    B's PRECOMMIT edge (in-degree ≥ 1), so H is never even a zero-in-degree candidate. We drive far
    ///    more rounds than B needs to publish; H must survive ALL of them. (Each round still renews B's
    ///    watermark so the K=2 crash detector keeps judging B live — the watermark now drives precommit
    ///    reclaim liveness, so a frozen B would have its precommit reclaimed; an advancing seq keeps it.)
    Gc gc(s, hexToU128("00000000000000000000000000000001"));
    constexpr int MAX_GC_ROUNDS = 8;   /// bounded-step budget: convergence must not exceed this

    /// One adversarial GC round + the spare invariant. INV (the heart of the test): the in-flight,
    /// PRECOMMIT-protected H is NEVER condemned/deleted while build B is active — H stays present and
    /// reads back the exact content. (Before the fix, GC re-condemned+deleted the fresh incarnation in the
    /// upload→publish window, so H would VANISH here → livelock.) The blob plane holds exactly one blob
    /// (H), so a reclaim of H is the only way rep.deleted could count a blob — but trees zero/cascade too,
    /// so we assert directly on H's presence + content rather than on the aggregate counter.
    const auto driveRoundAndAssertHSpared = [&](int round_no)
    {
        /// A LIVE server renews its watermark continuously (a background thread every ~heartbeat_period
        /// in production). Renew once per GC round so B's watermark seq ADVANCES between rounds: that is
        /// precisely what distinguishes a live server from a crashed one. (Without this, B's seq freezes
        /// and the GC's K=2 frozen-seq crash detector correctly declares B dead and RECLAIMS B's
        /// precommit — releasing H. The renew keeps B live so the precommit is honored every round.)
        s->renewWatermarkOnce();
        gc.runRegularRound();
        /// (Lease may not be acquired on the very first round: injectRetire pre-seeded gc/state with
        /// no lease owner, so the GC observes once and steals on the next round — protocol-correct,
        /// not load-bearing for this test. The invariant below holds regardless of who leads.)
        const HeadResult hr = b->head(blob_key);
        ASSERT_TRUE(hr.exists) << "H was deleted by GC at round " << round_no
                               << " despite being pinned by the live build B's precommit (B167 livelock would do this)";
        const auto raw = b->get(blob_key);
        ASSERT_TRUE(raw.has_value());
        const auto hdr = decodeEnvelopeHeader(raw->bytes, raw->bytes.size(), ObjectKind::Blob);
        EXPECT_EQ(raw->bytes.substr(hdr.header_len), content)
            << "H's content was lost/corrupted at round " << round_no;
    };

    /// Phase 1 — the livelock window. H is referenced by NO published TABLE root (B has not published
    /// yet) but IS named by B's precommit, so the build-root fold lifts it to in-degree ≥ 1 — never a
    /// zero-in-degree candidate. Drive several full rounds (enough to establish the leader and reclaim
    /// tree_a/part_1); the precommit edge must SPARE H's fresh incarnation every single round.
    int rounds_run = 0;
    constexpr int PRE_PUBLISH_ROUNDS = 4;
    for (int i = 0; i < PRE_PUBLISH_ROUNDS; ++i)
    {
        driveRoundAndAssertHSpared(++rounds_run);
        if (::testing::Test::HasFatalFailure())
            return;
    }

    /// Phase 2 — converge. With H still alive (spared through the whole window), build B publishes a
    /// part referencing it. The gate sees H's dep is already tokened at the fresh incarnation (uploaded
    /// from source above) and live, so it keeps it; H is never absent. This MUST succeed — the build
    /// converges in bounded steps.
    build_b->publish(RootNamespace{"srv1/tbl"}, "part_2", tree_b, RefPayload{});
    const bool published = true;

    /// Phase 3 — keep the GC hammering after publish. H is now pinned by tree_b's TABLE root edge (the
    /// publish also removed the precommit); the GC must keep sparing it as a genuinely-reachable node.
    while (rounds_run < MAX_GC_ROUNDS)
    {
        driveRoundAndAssertHSpared(++rounds_run);
        if (::testing::Test::HasFatalFailure())
            return;
    }

    /// 6. ASSERT convergence: publish SUCCEEDED within the bounded budget, and H reads back intact.
    ASSERT_TRUE(published) << "build B never published — the B167 livelock is back";
    EXPECT_LE(rounds_run, MAX_GC_ROUNDS);

    const auto resolved = s->resolveRef(RootNamespace{"srv1/tbl"}, "part_2");
    ASSERT_TRUE(resolved.has_value());
    EXPECT_EQ(resolved->tree_id, tree_b);

    const auto read = s->readTree(tree_b);
    ASSERT_EQ(read.size(), 1u);
    const auto loc = s->locate(read[0]);
    const auto got = b->get(loc.key, Range{loc.offset, loc.length});
    ASSERT_TRUE(got.has_value());
    EXPECT_EQ(got->bytes, content);
}

TEST(CasBuild, AdoptedBlobVanishedIsRetryableNotFatal)
{
    /// B188 regression: the three adopt sites in ContentAddressedTransaction (createHardLink,
    /// moveDirectory two-build merge, moveFile cross-part) previously called reuseBlob for a committed-
    /// source (non-pending) blob DURING STAGING — before any precommit protection existed. The fix
    /// (B188) replaces those three reuseBlob calls with adoptEvidence, which records a tokenless dep
    /// WITHOUT any backend call, deferring the observation to the publish gate (post-precommit). The
    /// gate (`checkAndResolveDeps`) throws retryable ABORTED when the blob is absent — not a fatal error.
    /// B190 removes reuseBlob entirely (no production callers).
    ///
    /// This test validates the adoptEvidence contract at the Build API level:
    ///   A) adoptEvidence + stageTree + precommit + publish on an absent blob whose evidence went stale
    ///      (GC advanced a round after the dep was recorded) → ABORTED (retryable), NOT FILE_DOESNT_EXIST.
    ///   C) adoptEvidence + stageTree + precommit + publish on a PRESENT blob → succeeds (positive case).
    ///
    /// To make the evidence go stale (dep.observed_view_round < current retire_view round), the test uses
    /// the GC fence mechanism: inject gc/state at round=1 before Part A (so the dep records
    /// observed_view_round=1), delete the blob, then inject gc/state at round=2 and raise fence_round=2
    /// on the target shard. The publish lambda sees fence_round=2 > current view round=1, calls
    /// retireView().refresh() which reads the new gc/state (round=2), and `checkAndResolveDeps` then
    /// finds the tokenless dep stale (1 < 2), HEADs the absent blob, and throws ABORTED.
    auto b = std::make_shared<InMemoryBackend>();

    /// 1. Build A: upload the blob and publish a ref that references it. Capture the blob id and token.
    BlobId id;
    Token t0;
    {
        auto s0 = openStore(b);
        auto build_a = s0->startBuild({});
        id = build_a->putBlob(idOf("b188-content"), BlobSource::fromString("b188-content")).id;
        t0 = b->head(s0->layout().blobKey(id)).token;
        TreeEntry e;
        e.name = "f.bin";
        e.placement = Placement::Blob;
        e.file_hash = u128Of("b188-content");
        e.file_size = 12;
        const TreeId tree_a = build_a->putTree({e});
        build_a->publish(RootNamespace{"srv1/tbl"}, "part_1", tree_a, RefPayload{});
    }

    DB::Cas::Layout layout("p");
    const String blob_key = layout.blobKey(id);

    /// Part execution order is load-bearing: C runs first because it needs the blob present; A runs
    /// last because it permanently GC-deletes the blob to drive the absent-evidence gate path.
    ///
    /// Part C — POSITIVE: blob present; adoptEvidence + stageTree + precommit + publish succeeds.
    {
        auto s = openStore(b);
        auto build = s->startBuild({});

        TreeEntry entry;
        entry.name = "f.bin";
        entry.placement = Placement::Blob;
        entry.file_hash = u128Of("b188-content");
        entry.file_size = 12;

        build->adoptEvidence(entry);

        const TreeId staged = build->stageTree({entry});
        build->precommit(staged);
        build->uploadStagedTree(staged);

        EXPECT_NO_THROW(
            build->publish(RootNamespace{"srv1/tbl"}, "part_2", staged, RefPayload{}));
    }

    /// Part A — NEW CONTRACT: adoptEvidence records a tokenless dep WITHOUT eager HEAD. The publish gate
    /// (`checkAndResolveDeps`, post-precommit) observes the blob and throws ABORTED when it is absent.
    /// This is retryable — the caller retries the whole INSERT which re-uploads from source.
    ///
    /// Step: inject gc/state round=1 so the store opens with retire_view at round=1.
    injectRetire(*b, layout, /*round=*/ 1, /*fence_seq=*/ 0, /*shard=*/ 0, {});

    {
        auto s = openStore(b);   /// retire_view.refresh() at open → round=1
        auto build = s->startBuild({});

        TreeEntry entry;
        entry.name = "f.bin";
        entry.placement = Placement::Blob;
        entry.file_hash = u128Of("b188-content");
        entry.file_size = 12;

        /// adoptEvidence must NOT throw — no eager HEAD even though we will delete the blob next.
        EXPECT_NO_THROW(build->adoptEvidence(entry));   /// observed_view_round = 1

        const TreeId staged = build->stageTree({entry});

        /// precommit must not throw either — it sees the dep but does not HEAD the blob.
        EXPECT_NO_THROW(build->precommit(staged));
        build->uploadStagedTree(staged);

        /// Simulate the B188 race: GC deletes the blob in the adopt→precommit window (after staging,
        /// before the publish gate runs). Also advance the gc/state to round=2 and raise fence_round=2
        /// on the target namespace shard so the publish lambda's fence check triggers a view refresh,
        /// making the recorded dep stale (observed_view_round=1 < refreshed round=2).
        b->deleteExact(blob_key, t0);
        ASSERT_FALSE(b->head(blob_key).exists);

        /// Advance gc/state to round=2; re-inject with no retired entries (the blob was GC-deleted but
        /// we simulate this by direct deleteExact above — the test only needs the view round to advance).
        injectRetire(*b, layout, /*round=*/ 2, /*fence_seq=*/ 0, /*shard=*/ 0, {});

        /// Raise fence_round=2 on the target namespace (srv1/tbl) and target ref (part_3) so the
        /// publish lambda refreshes the view before running `checkAndResolveDeps`.
        const uint64_t root_shards = s->poolMeta().root_shards;
        fenceNamespace(*b, layout, RootNamespace{"srv1/tbl"}, root_shards, /*round=*/ 2);

        /// publish drives the gate: fence_round=2 > view_round=1 → view refresh to round=2 →
        /// `checkAndResolveDeps` finds tokenless dep with observed_view_round=1 < 2 → stale → HEAD absent
        /// blob → ABORTED (retryable). NOT the old FILE_DOESNT_EXIST.
        expectThrowsCode(DB::ErrorCodes::ABORTED,
            [&] { build->publish(RootNamespace{"srv1/tbl"}, "part_3", staged, RefPayload{}); });
    }
}

/// B199-S2: precommit populates the closure on the precommit-ns `Add` journal record from the build's
/// staged tree structure. The flat-manifest case: one staged tree (the manifest) referencing two blobs
/// → closure has exactly one `ClosureNode` containing the manifest's tree_hash and both blob entries.
TEST(CasBuild, PrecommitAddRecordCarriesInlineClosure)
{
    std::shared_ptr<InMemoryBackend> b;
    auto s = Store::open(
        [&]() -> std::shared_ptr<InMemoryBackend>
        {
            b = std::make_shared<InMemoryBackend>();
            return b;
        }(),
        PoolConfig{.pool_prefix = "p", .root_shards = 1});

    auto build = s->startBuild({});
    build->putBlob(idOf("B1"), BlobSource::fromString("B1"));
    build->putBlob(idOf("B2"), BlobSource::fromString("B2"));

    TreeEntry e1;
    e1.name = "data.bin";
    e1.placement = Placement::Blob;
    e1.file_hash = u128Of("B1");
    e1.file_size = 2;

    TreeEntry e2;
    e2.name = "data.mrk";
    e2.placement = Placement::Blob;
    e2.file_hash = u128Of("B2");
    e2.file_size = 2;

    const TreeId t = build->stageTree({e1, e2});
    build->precommit(t);

    /// Read the precommit shard manifest using the same pattern as PrematureReclaimCommitFailsClosed.
    const RootNamespace precommit_ns{u128ToHex(s->poolConfig().server_id) + "/_precommits"};
    const String precommit_ref = std::to_string(build->buildSeq());
    const String shard_key = s->layout().rootShardKey(precommit_ns, s->shardOf(precommit_ref));
    const auto shard_raw = b->get(shard_key);
    ASSERT_TRUE(shard_raw.has_value());

    const RootShard rs = decodeRootShard(shard_raw->bytes);

    /// The closure rides the precommit-ns `Add` journal record, not the ref payload.
    const auto rec_it = std::find_if(
        rs.journal.begin(), rs.journal.end(),
        [&](const JournalRecord & r)
        { return r.op == JournalRecord::Op::Add && r.ref_name == precommit_ref; });
    ASSERT_NE(rec_it, rs.journal.end());

    ASSERT_EQ(rec_it->closure.size(), 1u);
    EXPECT_EQ(rec_it->closure[0].tree_hash, hexToU128(t.string()));
    ASSERT_EQ(rec_it->closure[0].entries.size(), 2u);
}
