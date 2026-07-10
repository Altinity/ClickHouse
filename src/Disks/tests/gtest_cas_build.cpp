#include <gtest/gtest.h>
#include <algorithm>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasBuild.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasStore.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasInMemoryBackend.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasEnvelope.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasGc.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasGcFormats.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasManifestCodec.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasManifestId.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasRootShardCodec.h>
#include <Disks/tests/cas_test_helpers.h>
#include <IO/HashingReadBuffer.h>
#include <IO/ReadBufferFromMemory.h>
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
extern const int CORRUPTED_DATA;
}

using namespace DB::Cas;
using DB::Cas::tests::condemnMeta;
using DB::Cas::tests::expectThrowsCode;
using DB::Cas::tests::idOf;
using DB::Cas::tests::injectRetire;
using DB::Cas::tests::loadMetaForTest;
using DB::Cas::tests::shardOfForTest;
using DB::Cas::tests::streamingHexOf;
using DB::Cas::tests::u128Of;
using DB::Cas::tests::writeMetaClean;
using DB::Cas::tests::writeRawBlobBody;

namespace
{

StorePtr openStore(const std::shared_ptr<InMemoryBackend> & b)
{
    return Store::open(b, PoolConfig{.pool_prefix = "p", .server_root_id = "test"});
}

/// Start a build whose owning manifest namespace + final ref name are `ns`/`ref` (promote/stageManifest
/// derive the manifest namespace by splitting BuildInfo::intended_ref on the LAST '/').
BuildPtr startBuildFor(const StorePtr & s, const RootNamespace & ns, const String & ref)
{
    BuildInfo info;
    info.intended_ref = ns.string() + "/" + ref;
    return s->startBuild(info);
}

/// A one-entry Blob ManifestEntry for `payload` at `path` (the build's stageManifest entry).
ManifestEntry blobManifestEntry(const String & path, const String & payload)
{
    ManifestEntry e;
    e.path = path;
    e.placement = EntryPlacement::Blob;
    e.blob_hash = u128Of(payload);
    e.blob_size = payload.size();
    return e;
}

ManifestEntry blobManifestEntryStreaming(const String & path, const String & payload)
{
    ManifestEntry e;
    e.path = path;
    e.placement = EntryPlacement::Blob;
    e.blob_hash = hexToU128(streamingHexOf(payload));
    e.blob_size = payload.size();
    return e;
}

/// publishOneBlobPart with the streaming (production-convention) blob id.
ManifestId publishOneBlobPartStreaming(
    const StorePtr & s, const RootNamespace & ns, const String & ref, const String & path, const String & payload)
{
    auto build = startBuildFor(s, ns, ref);
    const ManifestId id = build->stageManifest({blobManifestEntryStreaming(path, payload)});
    build->precommitAdd(ns, ref, id);
    build->putBlob(BlobId{streamingHexOf(payload)}, BlobSource::fromString(payload));
    build->promote(ns, ref, build->buildId(), id);
    return id;
}

/// The full single-blob write flow (EDGE-BEFORE-OBSERVE wiring order):
/// stageManifest(one entry) -> precommitAdd -> putBlob -> promote. Returns the committed ManifestId.
ManifestId publishOneBlobPart(
    const StorePtr & s, const RootNamespace & ns, const String & ref, const String & path, const String & payload)
{
    auto build = startBuildFor(s, ns, ref);
    const ManifestId id = build->stageManifest({blobManifestEntry(path, payload)});
    build->precommitAdd(ns, ref, id);
    build->putBlob(idOf(payload), BlobSource::fromString(payload));
    build->promote(ns, ref, build->buildId(), id);
    return id;
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
    std::optional<DB::Cas::GetStreamResult> getStream(const String & k, DB::Cas::Range r = {}) override { return inner->getStream(k, r); }
    DB::Cas::ListPage list(const String & p, const String & c, size_t l) override { return inner->list(p, c, l); }
    DB::Cas::PutResult putIfAbsent(const String & k, const String & b, const DB::Cas::ObjectMeta & meta = {}) override { return inner->putIfAbsent(k, b, meta); }
    DB::Cas::WriteSinkPtr putIfAbsentStream(const String & k, const DB::Cas::ObjectMeta & meta = {}) override { return inner->putIfAbsentStream(k, meta); }
    DB::Cas::PutResult putOverwrite(const String & k, const String & b, const DB::Cas::Token & e, const DB::Cas::ObjectMeta & meta = {}) override { return inner->putOverwrite(k, b, e, meta); }
    DB::Cas::CasResult casPut(const String & k, const String & b, const std::optional<DB::Cas::Token> & e, const DB::Cas::ObjectMeta & meta = {}) override { return inner->casPut(k, b, e, meta); }
    DB::Cas::DeleteOutcome deleteExact(const String & k, const DB::Cas::Token & t) override { return inner->deleteExact(k, t); }
    bool supportsListTokens() const override { return inner->supportsListTokens(); }

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

TEST(CasBuild, StageManifestUsesPerBuildOrdinals)
{
    auto b = std::make_shared<InMemoryBackend>();
    auto s = openStore(b);
    const RootNamespace ns{"test/tbl@cas@"};

    auto build = startBuildFor(s, ns, "all_1_1_0");
    const ManifestId first = build->stageManifest({blobManifestEntry("a.bin", "a")});
    const ManifestId second = build->stageManifest({blobManifestEntry("b.bin", "b")});

    EXPECT_EQ(first.ref.writer_epoch, s->writerEpoch());
    EXPECT_EQ(first.ref.build_sequence, build->buildSeq());
    EXPECT_EQ(first.ref.manifest_ordinal, 1u);
    EXPECT_EQ(second.ref.writer_epoch, first.ref.writer_epoch);
    EXPECT_EQ(second.ref.build_sequence, first.ref.build_sequence);
    EXPECT_EQ(second.ref.manifest_ordinal, 2u);
    EXPECT_EQ(s->layout().manifestKey(first), "p/cas/manifests/test/tbl@cas@/"
        + std::to_string(s->writerEpoch()) + "/" + std::to_string(build->buildSeq()) + "/000001.proto");
    EXPECT_EQ(s->layout().manifestKey(second), "p/cas/manifests/test/tbl@cas@/"
        + std::to_string(s->writerEpoch()) + "/" + std::to_string(build->buildSeq()) + "/000002.proto");

    auto next_build = startBuildFor(s, ns, "all_2_2_0");
    const ManifestId next = next_build->stageManifest({blobManifestEntry("c.bin", "c")});
    EXPECT_EQ(next.ref.writer_epoch, first.ref.writer_epoch);
    EXPECT_NE(next.ref.build_sequence, first.ref.build_sequence);
    EXPECT_EQ(next.ref.manifest_ordinal, 1u);
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

    /// First writer FRESH-uploads (legal pre-precommit — newborn-debris watermark).
    auto build_a = s->startBuild({});
    auto ref_a = build_a->putBlob(idOf("dup"), BlobSource::fromString("dup"));
    const Token token_a = b->head(s->layout().blobKey(ref_a.id)).token;

    /// Second writer ADOPTS — the adopt must happen under a durable precommit edge (EDGE-BEFORE-OBSERVE:
    /// stageManifest -> precommitAdd -> putBlob), so give build_b the wiring order.
    const RootNamespace ns_b{"srv/tbl"};
    auto build_b = startBuildFor(s, ns_b, "ref_b");
    const ManifestId id_b = build_b->stageManifest({blobManifestEntry("data.bin", "dup")});
    build_b->precommitAdd(ns_b, "ref_b", id_b);
    auto ref_b = build_b->putBlob(idOf("dup"), BlobSource::fromString("dup"));

    EXPECT_EQ(ref_b.id, ref_a.id);
    /// A's incarnation survives — the second writer adopts, nothing was overwritten.
    EXPECT_EQ(b->head(s->layout().blobKey(ref_a.id)).token, token_a);
}

/// Task 3 (spec §meta-protocols v3): the writer's dedup gate no longer consults the RetireView for the
/// condemned decision — it point-reads the per-hash freshness meta instead. A fresh (absent -> present)
/// upload must WRITE that meta as Clean so future point-readers (other writers, GC) can see it.
TEST(CasBuild, PutBlobFreshUploadWritesCleanMeta)
{
    auto b = std::make_shared<InMemoryBackend>();
    auto s = openStore(b);
    auto build = s->startBuild({});

    const String payload = "fresh-meta-payload";
    auto ref = build->putBlob(idOf(payload), BlobSource::fromString(payload));
    EXPECT_EQ(ref.size, payload.size());

    const auto lm = loadMetaForTest(*b, s->layout(), u128Of(payload));
    ASSERT_TRUE(lm.has_value()) << "a fresh upload must write a Clean meta descriptor (writer point-read protocol)";
    EXPECT_EQ(lm->meta.state, MetaState::Clean);
    EXPECT_EQ(lm->meta.size, payload.size());
}

/// The adopt decision is driven PURELY by the meta point-read — no RetireView is ever seeded in this
/// test. A pre-existing body plus an independent Clean meta must be adopted (no putOverwrite/re-upload:
/// the pre-seeded incarnation's token survives untouched), and the meta stays Clean.
TEST(CasBuild, PutBlobAdoptsWhenMetaCleanNoRetireView)
{
    auto b = std::make_shared<InMemoryBackend>();
    auto s = openStore(b);

    const String payload = "adopt-meta-payload";
    const UInt128 hash = u128Of(payload);
    const BlobId id = idOf(payload);
    const String blob_key = s->layout().blobKey(id);

    /// Pre-seed a body big enough that observeAndAdmit's logical-size guard (hr.size - header_len)
    /// does not underflow, plus an INDEPENDENT Clean meta — deliberately NOT via a real putBlob (so the
    /// adopt decision below cannot be riding on THIS task's own fresh-upload meta write).
    const uint64_t header_len = s->poolMeta().blob_header_len;
    String raw_body(header_len, '\0');
    raw_body += payload;
    writeRawBlobBody(*b, s->layout(), hash, raw_body);
    writeMetaClean(*b, s->layout(), hash, payload.size());
    const Token t0 = b->head(blob_key).token;

    /// Adopt must happen under a durable precommit edge (EDGE-BEFORE-OBSERVE), mirroring
    /// PutBlobDedupSecondWriterAdopts above.
    const RootNamespace ns{"srv/tbl"};
    auto build = startBuildFor(s, ns, "ref_adopt");
    const ManifestId manifest_id = build->stageManifest({blobManifestEntry("data.bin", payload)});
    build->precommitAdd(ns, "ref_adopt", manifest_id);
    auto ref = build->putBlob(id, BlobSource::fromString(payload));

    EXPECT_EQ(ref.id, id);
    /// Adopted: the pre-seeded incarnation survives untouched — no putOverwrite/re-upload happened.
    EXPECT_EQ(b->head(blob_key).token, t0);

    const auto lm = loadMetaForTest(*b, s->layout(), hash);
    ASSERT_TRUE(lm.has_value());
    EXPECT_EQ(lm->meta.state, MetaState::Clean) << "an adopt must leave the meta Clean";
}

/// The resurrect decision (displace a condemned body) is likewise driven PURELY by the meta point-read
/// — again, no RetireView is seeded. A condemned meta must cause putBlob to displace the body (a fresh
/// token, the old one never returns — INV-NO-RETURN, unchanged body mechanics) AND flip the meta back
/// to Clean.
TEST(CasBuild, PutBlobResurrectsWhenMetaCondemned)
{
    auto b = std::make_shared<InMemoryBackend>();
    auto s = openStore(b);

    const String payload = "resurrect-meta-payload";
    const UInt128 hash = u128Of(payload);
    const BlobId id = idOf(payload);
    const String blob_key = s->layout().blobKey(id);

    const uint64_t header_len = s->poolMeta().blob_header_len;
    String raw_body(header_len, '\0');
    raw_body += payload;
    writeRawBlobBody(*b, s->layout(), hash, raw_body);
    writeMetaClean(*b, s->layout(), hash, payload.size());
    condemnMeta(*b, s->layout(), hash, /*condemn_round*/ 1);
    const Token t0 = b->head(blob_key).token;

    /// NO retire-view seeding anywhere: the resurrect must be decided purely from the meta point-read.
    auto build = s->startBuild({});
    auto ref = build->putBlob(id, BlobSource::fromString(payload));
    EXPECT_EQ(ref.id, id);

    /// Resurrected: the condemned incarnation was displaced by a fresh one.
    const HeadResult hr = b->head(blob_key);
    ASSERT_TRUE(hr.exists);
    EXPECT_NE(hr.token, t0) << "a condemned incarnation must be displaced by a fresh one (resurrect)";
    EXPECT_EQ(b->deleteExact(blob_key, t0).kind, DeleteOutcome::Kind::TokenMismatch)
        << "the condemned token must never return (INV-NO-RETURN)";

    const auto lm = loadMetaForTest(*b, s->layout(), hash);
    ASSERT_TRUE(lm.has_value());
    EXPECT_EQ(lm->meta.state, MetaState::Clean) << "a resurrect must flip the meta back to Clean";
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

/// The happy-path upload STREAMS the source directly into the put sink — it does NOT pre-materialize the
/// whole blob into an in-memory String before the I/O. We assert this by counting `write_payload`
/// invocations: a single fresh upload must invoke it EXACTLY ONCE (streamed straight into the sink). The
/// previous implementation buffered the whole blob into a `String source_bytes` first (a full in-memory
/// copy whose peak grew ~linearly with the blob size — the OOM); that pass would invoke `write_payload`
/// before the sink write. One invocation here is the streaming-not-materializing guarantee.
TEST(CasBuild, PutBlobStreamsSourceOnceNoFullMaterialization)
{
    auto b = std::make_shared<InMemoryBackend>();
    auto s = openStore(b);
    auto build = s->startBuild({});

    const String payload = "streamed-not-materialized";
    int invocations = 0;
    BlobSource source;
    source.size = payload.size();
    source.write_payload = [&invocations, &payload](DB::WriteBuffer & out)
    {
        ++invocations;
        DB::writeString(payload, out);
    };

    auto ref = build->putBlob(idOf(payload), std::move(source));
    EXPECT_EQ(ref.size, payload.size());
    EXPECT_EQ(invocations, 1) << "happy-path upload must stream the source exactly once (no pre-materialization pass)";

    /// And the object really landed with the streamed payload.
    auto raw = b->get(s->layout().blobKey(ref.id));
    ASSERT_TRUE(raw.has_value());
    auto h = decodeEnvelopeHeader(raw->bytes, raw->bytes.size(), ObjectKind::Blob);
    EXPECT_EQ(h.logical_size, payload.size());
}

/// B190: reuseBlob is removed (it had no production callers post-B188). Its behaviors are now covered by:
///   - absent blob / absent-at-gate: PromoteAbsentTokenlessBlobAbortsRetryable (CasBuild) for the
///     tokenless leaf; a tokened leaf is edge-protected (Phase A) and never re-observed at the gate.
///   - condemned dep at gate:        PromoteBodylessCondemnedDepThrowsAbortedRetryable (CasBuild).
///   - evidence tokenless vs tokened: DepIsTokenedDiscriminatesPutBlobVsAdopt (CasBuildReuseBlob).
///   - adoptEvidence lazy-observe:   AdoptedBlobVanishedIsRetryableNotFatal Part A (CasBuild).

TEST(CasBuildReuseBlob, DepIsTokenedDiscriminatesPutBlobVsAdopt)
{
    /// B156b discriminator unit: putBlob records a TOKENED dep (token observed at upload time),
    /// adoptEvidence records a TOKENLESS W-EVIDENCE dep (no token; liveness from the source ref).
    auto b = std::make_shared<InMemoryBackend>();
    auto s = openStore(b);

    auto build = s->startBuild({});

    /// putBlob'd hash ⇒ tokened.
    build->putBlob(idOf("written"), BlobSource::fromString("written"));
    EXPECT_TRUE(build->depIsTokened(u128Of("written")));

    /// Adopted hash ⇒ tokenless. adoptEvidence records the dep directly from a resolved ManifestEntry
    /// (the source manifest's entry); no body needs to be in hand for the dep to be recorded.
    build->adoptEvidence(blobManifestEntry("f", "adopted"));
    EXPECT_FALSE(build->depIsTokened(u128Of("adopted")));

    /// Unknown hash ⇒ no dep, not tokened.
    EXPECT_FALSE(build->depIsTokened(u128Of("unknown")));
}

/// B190: ReuseBlobCondemnedThrowsAbortedRetryable is removed (reuseBlob is gone).
/// Condemned-blob-at-gate behavior is covered by:
///   PromoteBodylessCondemnedDepThrowsAbortedRetryable (the promote gate HEADs every blob leaf; a
///   condemned HEAD ⇒ ABORTED; no GET).

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
    /// v3: the writer's condemned decision is a per-hash meta point-read (not the retire-view). Condemn the
    /// meta; t0 stays as the body token the delete-hook below fires with.
    condemnMeta(*b, layout, u128Of("payload-X"), /*condemn_round*/ 1);

    /// 3. Wrap the backend so the NEXT head(blob_key) returns the (present) result and THEN fires
    ///    deleteExact(blob_key, t0) exactly once — GC's delete in the HEAD->GET window. Open a FRESH
    ///    Store over the hook so its retire view (refreshed at open) sees the condemnation.
    auto hook = std::make_shared<HeadThenDeleteOnceBackend>(b, blob_key, t0);
    auto s = Store::open(hook, PoolConfig{.pool_prefix = "p", .server_root_id = "test"});
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
        std::optional<DB::Cas::GetStreamResult> getStream(const String & k, DB::Cas::Range r = {}) override { return inner->getStream(k, r); }
        DB::Cas::ListPage list(const String & p, const String & c, size_t l) override { return inner->list(p, c, l); }
        DB::Cas::PutResult putIfAbsent(const String & k, const String & bts, const DB::Cas::ObjectMeta & m = {}) override { return inner->putIfAbsent(k, bts, m); }
        DB::Cas::WriteSinkPtr putIfAbsentStream(const String & k, const DB::Cas::ObjectMeta & m = {}) override { return inner->putIfAbsentStream(k, m); }
        DB::Cas::PutResult putOverwrite(const String & k, const String & bts, const DB::Cas::Token & e, const DB::Cas::ObjectMeta & m = {}) override { return inner->putOverwrite(k, bts, e, m); }
        DB::Cas::CasResult casPut(const String & k, const String & bts, const std::optional<DB::Cas::Token> & e, const DB::Cas::ObjectMeta & m = {}) override { return inner->casPut(k, bts, e, m); }
        DB::Cas::DeleteOutcome deleteExact(const String & k, const DB::Cas::Token & tok) override { return inner->deleteExact(k, tok); }
        bool supportsListTokens() const override { return inner->supportsListTokens(); }
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
    auto s = Store::open(counting, PoolConfig{.pool_prefix = "p", .server_root_id = "test"});
    auto build = s->startBuild({});

    /// 4. putBlob Y — the object is absent (was deleted). The dedup-hit (PreconditionFailed) path
    ///    won't fire (object is gone, so putIfAbsentStream → Done on the first attempt).
    ///    However, even if a racing re-creation happens between the check and the upload, the
    ///    condemned branch must NEVER call backend().get(blob_key).
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
        std::optional<DB::Cas::GetStreamResult> getStream(const String & k, DB::Cas::Range r = {}) override { return inner->getStream(k, r); }
        DB::Cas::ListPage list(const String & p, const String & c, size_t l) override { return inner->list(p, c, l); }
        DB::Cas::PutResult putIfAbsent(const String & k, const String & bts, const DB::Cas::ObjectMeta & m = {}) override { return inner->putIfAbsent(k, bts, m); }
        DB::Cas::WriteSinkPtr putIfAbsentStream(const String & k, const DB::Cas::ObjectMeta & m = {}) override { return inner->putIfAbsentStream(k, m); }
        DB::Cas::PutResult putOverwrite(const String & k, const String & bts, const DB::Cas::Token & e, const DB::Cas::ObjectMeta & m = {}) override { return inner->putOverwrite(k, bts, e, m); }
        DB::Cas::CasResult casPut(const String & k, const String & bts, const std::optional<DB::Cas::Token> & e, const DB::Cas::ObjectMeta & m = {}) override { return inner->casPut(k, bts, e, m); }
        DB::Cas::DeleteOutcome deleteExact(const String & k, const DB::Cas::Token & tok) override { return inner->deleteExact(k, tok); }
        bool supportsListTokens() const override { return inner->supportsListTokens(); }
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
    /// v3: condemn via the per-hash meta (the writer's freshness point-read), object still PRESENT.
    condemnMeta(*b, layout, u128Of("payload-Z"), /*condemn_round*/ 1);
    ASSERT_TRUE(b->head(blob_key).exists) << "blob must be PRESENT for the condemned-present path";

    /// 3. Open a fresh Store over a GET-counting wrapper.
    auto counting = std::make_shared<GetCountingBackend>(b, blob_key);
    auto s = Store::open(counting, PoolConfig{.pool_prefix = "p", .server_root_id = "test"});
    auto build = s->startBuild({});

    /// 4. putBlob Z: putIfAbsentStream → PreconditionFailed (object present) → observeAndAdmit →
    ///    sees condemned token → must call uploadFromSource (NOT resurrect/GET).
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
        std::optional<DB::Cas::GetStreamResult> getStream(const String & k, DB::Cas::Range r = {}) override { return inner->getStream(k, r); }
        DB::Cas::ListPage list(const String & p, const String & c, size_t l) override { return inner->list(p, c, l); }
        DB::Cas::PutResult putIfAbsent(const String & k, const String & bts, const DB::Cas::ObjectMeta & m = {}) override { return inner->putIfAbsent(k, bts, m); }
        DB::Cas::PutResult putOverwrite(const String & k, const String & bts, const DB::Cas::Token & e, const DB::Cas::ObjectMeta & m = {}) override { return inner->putOverwrite(k, bts, e, m); }
        DB::Cas::CasResult casPut(const String & k, const String & bts, const std::optional<DB::Cas::Token> & e, const DB::Cas::ObjectMeta & m = {}) override { return inner->casPut(k, bts, e, m); }
        DB::Cas::DeleteOutcome deleteExact(const String & k, const DB::Cas::Token & t) override { return inner->deleteExact(k, t); }
        bool supportsListTokens() const override { return inner->supportsListTokens(); }

        BackendPtr inner;
        String watched_key;
    };

    auto raw = std::make_shared<InMemoryBackend>();
    DB::Cas::Layout layout("p");
    const String blob_key = layout.blobKey(idOf("payload-V"));

    /// The blob does NOT need to pre-exist: the scripted backend models the conditional-PUT 412
    /// (object present at PUT time) independently of the inner store, then reports absent on HEAD.
    auto scripted = std::make_shared<ScriptedVanishBackend>(raw, blob_key);
    auto s = Store::open(scripted, PoolConfig{.pool_prefix = "p", .server_root_id = "test"});
    /// Wiring order (EDGE-BEFORE-OBSERVE): the revive re-observes via observeAndAdmit, which requires a
    /// durable precommit edge — stageManifest -> precommitAdd before putBlob.
    const RootNamespace ns_v{"srv/tbl"};
    auto build = startBuildFor(s, ns_v, "part_v");
    const ManifestId id_v = build->stageManifest({blobManifestEntry("data.bin", "payload-V")});
    build->precommitAdd(ns_v, "part_v", id_v);

    /// putBlob holds "payload-V" as source bytes. The vanish-during-revival must NOT be fatal:
    ///   BEFORE fix: observeAndAdmit throws FILE_DOESNT_EXIST, escapes putBlob's ABORTED-only catch → fatal.
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

TEST(CasBuild, PromoteBodylessCondemnedDepThrowsAbortedRetryable)
{
    /// B137/B190: the promote gate's fail-closed blob revalidation (CasBuild.cpp promote step 3) HEADs
    /// EVERY blob leaf named by the manifest. A blob whose hash is condemned (or that vanishes in the
    /// HEAD window) must surface as ABORTED ("retry the operation") — a retryable transient, NOT a hard
    /// FILE_DOESNT_EXIST (which became an HTTP-500 INSERT failure).
    /// Since the copy-forward pre-pass (spec 2026-07-02-cas-copy-forward-condemned-evidence.md) a
    /// tokenless-evidence dep DOES read the condemned-but-present object to copy it forward; with the
    /// hook's exact-token delete landing right after the pre-pass HEAD, that read finds the object
    /// ABSENT and fails closed — this test now pins the copy-forward "deleted mid-flight ⇒ ABORTED,
    /// nothing written" contract (never putIfAbsent after a lost delete race).
    auto b = std::make_shared<InMemoryBackend>();
    const RootNamespace ns{"srv1/tbl"};

    /// 1. Write payload-X via a throwaway build to create the blob object; capture its token t0.
    BlobId id;
    Token t0;
    {
        auto s0 = openStore(b);
        auto build0 = s0->startBuild({});
        id = build0->putBlob(idOf("payload-X"), BlobSource::fromString("payload-X")).id;
        t0 = b->head(s0->layout().blobKey(id)).token;
    }

    DB::Cas::Layout layout("p");
    const String blob_key = layout.blobKey(id);

    /// 2. Condemn (Blob, hash(X)) via the meta (v3: the promote gate's condemned decision is a per-hash
    ///    meta point-read, not the retire-view) so the promote gate sees the blob leaf as a condemned hit
    ///    and must resolve it.
    condemnMeta(*b, layout, u128Of("payload-X"), /*condemn_round*/ 1);

    /// 3. Wrap the backend so the NEXT head(blob_key) returns the (present) result and THEN fires
    ///    deleteExact(blob_key, t0) exactly once — GC's exact-token delete racing the gate's HEAD.
    auto hook = std::make_shared<HeadThenDeleteOnceBackend>(b, blob_key, t0);
    auto s = Store::open(hook, PoolConfig{.pool_prefix = "p", .server_root_id = "test"});
    auto build = startBuildFor(s, ns, "part_1");

    /// 4. Adopt the blob as a tokenless evidence dep (no body in hand), then stage a manifest naming it
    ///    and precommit it. stageManifest writes the body without HEADing the blob; precommitAdd appends
    ///    the create-precommit owner event.
    build->adoptEvidence(blobManifestEntry("data.bin", "payload-X"));
    const ManifestId mid = build->stageManifest({blobManifestEntry("data.bin", "payload-X")});
    build->precommitAdd(ns, "part_1", mid);

    /// 5. promote drives the gate: it HEADs blob_key [hook fires deleteExact(blob_key, t0)] → condemned
    ///    (or vanished) HEAD → ABORTED, no GET (INV-1).
    ///    BEFORE fix: FILE_DOESNT_EXIST (hard). AFTER fix: ABORTED "retry the operation" (retryable).
    expectThrowsCode(DB::ErrorCodes::ABORTED,
        [&] { build->promote(ns, "part_1", build->buildId(), mid); });
}

TEST(CasBuild, CondemnedPresentEvidenceDepCopiesForwardAtGate)
{
    /// S13 soak run 3 / spec 2026-07-02-cas-copy-forward-condemned-evidence.md: `republishRef`
    /// (part move: ATTACH renameToDetached, DETACH, merge-result rename) records its deps via
    /// adoptEvidence — tokenless W-EVIDENCE from a COMMITTED source manifest, NO source bytes in
    /// hand. When the referenced blob's hash is condemned (v3: a per-hash meta point-read), the old
    /// gate threw ABORTED "retry the op" — but the attach caller never retries and the table stays
    /// readonly forever (a liveness brick, not a safety save).
    ///
    /// Post-fix contract (verified copy-forward): the gate reads the PRESENT dying object, verifies
    /// payload hash == key hash, re-wraps under a fresh envelope, putOverwrite@observed-token ⇒
    /// fresh incarnation; promote succeeds referencing ONLY the fresh token.
    auto b = std::make_shared<InMemoryBackend>();
    const RootNamespace ns{"srv1/tbl"};

    /// 1. Publish part A over blob CF — the committed SOURCE ref republishRef reads its entries
    ///    from (condition (a) of the invariant exception: a live committed owner references CF).
    Token t0;
    {
        auto s0 = openStore(b);
        publishOneBlobPartStreaming(s0, ns, "part_a", "data.bin", "payload-CF");
        t0 = b->head(s0->layout().blobKey(BlobId{streamingHexOf("payload-CF")})).token;
    }

    DB::Cas::Layout layout("p");
    const String blob_key = layout.blobKey(BlobId{streamingHexOf("payload-CF")});

    /// 2. Condemn (Blob, hash(CF)) via the meta — models the fold having condemned CF after stale-view
    ///    adoptions landed unfolded (the soak chain). Object stays PRESENT (no delete yet: a
    ///    non-pending entry is >= 2 passes from deletion).
    condemnMeta(*b, layout, hexToU128(streamingHexOf("payload-CF")), /*condemn_round*/ 1);

    /// 3. Fresh Store (restart) — republishRef's exact body: adoptEvidence over the source manifest
    ///    entry, stage a FRESH dst manifest, precommit, promote.
    std::vector<CasEvent> seen;   /// declared BEFORE the Store so it outlives the background syncer's emits (ASan 2026-07-09)
    auto s = Store::open(b, PoolConfig{.pool_prefix = "p", .server_root_id = "test"});
    s->setEventSink([&](const CasEvent & e){ seen.push_back(e); });
    auto build = startBuildFor(s, ns, "detached_part_a");
    build->adoptEvidence(blobManifestEntryStreaming("data.bin", "payload-CF"));
    const ManifestId mid = build->stageManifest({blobManifestEntryStreaming("data.bin", "payload-CF")});
    build->precommitAdd(ns, "detached_part_a", mid);

    /// 4. BEFORE fix: ABORTED (condemned at commit revalidation) — the attach brick.
    ///    AFTER fix: promote succeeds via copy-forward.
    EXPECT_NO_THROW(build->promote(ns, "detached_part_a", build->buildId(), mid));
    EXPECT_TRUE(s->resolveRef(ns, "detached_part_a").has_value());
    s->setEventSink(nullptr);

    /// Audit trail: exactly one `blob_copy_forward` event, naming the displaced token.
    size_t copy_forwards = 0;
    for (const CasEvent & e : seen)
        if (e.type == CasEventType::BlobCopyForward)
        {
            ++copy_forwards;
            EXPECT_EQ(e.object_hash, streamingHexOf("payload-CF"));
            EXPECT_EQ(e.detail.at("displaced_token"), t0.value);
        }
    EXPECT_EQ(copy_forwards, 1u);

    /// 5. The condemned incarnation was displaced: fresh token, same payload; GC's exact-token
    ///    delete of the listed (hash, t0) entry is now a mismatch no-op (entry drops, new
    ///    incarnation untouched).
    const HeadResult hr = b->head(blob_key);
    ASSERT_TRUE(hr.exists);
    EXPECT_NE(hr.token, t0) << "copy-forward must mint a FRESH incarnation, never bind the listed token";
    EXPECT_NE(b->deleteExact(blob_key, t0).kind, DeleteOutcome::Kind::Deleted)
        << "the listed token must no longer match the object";
    const auto stored = b->get(blob_key);
    ASSERT_TRUE(stored.has_value());
    const auto h = decodeEnvelopeHeader(stored->bytes, stored->bytes.size(), ObjectKind::Blob);
    EXPECT_EQ(stored->bytes.substr(h.header_len), "payload-CF") << "payload must survive byte-identical";
}

TEST(CasBuild, PromoteCopiesForwardCondemnedEvidenceInInstalledView)
{
    /// The K3 in-closure copy-forward gate (Phase A, spec 2026-07-09-cas-writer-gc-simplification): a
    /// tokenless (adoptEvidence) blob X whose hash is condemned is copied forward by the promote gate
    /// (the single copy-forward site after D3) instead of aborting, and promote SUCCEEDS.
    /// v3 (Task 4, spec §meta-protocols): the condemned decision is now a per-hash META POINT-READ, not
    /// the writer's RetireView — there is no "installed view" to explicitly refresh any more (the test
    /// name is legacy); a point-read is always live, so condemning the meta is immediately visible to K3.
    auto b = std::make_shared<InMemoryBackend>();
    auto s = openStore(b);
    const RootNamespace ns{"srv1/tbl"};

    /// A committed source part names X (independent live committed owner). putBlob mints X under the
    /// production streaming-hash id, so the copy-forward verifier's payload re-hash matches the content key.
    publishOneBlobPartStreaming(s, ns, "src_part", "data.bin", "payload-CFX");
    const String blob_key = s->layout().blobKey(BlobId{streamingHexOf("payload-CFX")});
    const Token t0 = b->head(blob_key).token;

    /// A second build ADOPTS X via adoptEvidence (tokenless dep, NO putBlob ⇒ no retained source), stages,
    /// precommits — but does NOT promote yet.
    auto build = startBuildFor(s, ns, "part_2");
    const ManifestEntry entry = blobManifestEntryStreaming("data.bin", "payload-CFX");
    build->adoptEvidence(entry);
    const ManifestId id = build->stageManifest({entry});
    build->precommitAdd(ns, "part_2", id);

    /// GC condemns X's hash in round 1 via the meta.
    condemnMeta(*b, s->layout(), hexToU128(streamingHexOf("payload-CFX")), /*condemn_round*/ 1);

    /// promote: the K3 gate sees X condemned (meta point-read) ⇒ copy-forward ⇒ commit.
    EXPECT_NO_THROW(build->promote(ns, "part_2", build->buildId(), id));

    /// The ref resolves and X rides a FRESH, non-condemned token (t0 never bound).
    EXPECT_TRUE(s->resolveRef(ns, "part_2").has_value());
    const HeadResult hr = b->head(blob_key);
    ASSERT_TRUE(hr.exists);
    EXPECT_NE(hr.token, t0) << "copy-forward must mint a FRESH incarnation, never bind the condemned token";
    const auto lm_after = loadMetaForTest(*b, s->layout(), hexToU128(streamingHexOf("payload-CFX")));
    ASSERT_TRUE(lm_after.has_value());
    EXPECT_EQ(lm_after->meta.state, MetaState::Clean) << "copy-forward must flip the meta back to Clean";
}

TEST(CasBuild, PromoteAbsentTokenlessBlobAbortsRetryable)
{
    /// A tokenless (adoptEvidence) blob that is ABSENT at the gate (deleted, no retained source) cannot be
    /// copied forward — copyForwardFromCondemned requires a present object to GET — so promote fails closed
    /// (ABORTED, retryable) and no ref is published.
    auto b = std::make_shared<InMemoryBackend>();
    auto s = openStore(b);
    const RootNamespace ns{"srv1/tbl"};

    /// Seed X (streaming-keyed) without a committed owner, then adopt it and delete it.
    {
        auto seed = s->startBuild({});
        seed->putBlob(BlobId{streamingHexOf("payload-ABS")}, BlobSource::fromString("payload-ABS"));
    }
    const String blob_key = s->layout().blobKey(BlobId{streamingHexOf("payload-ABS")});
    const Token t0 = b->head(blob_key).token;

    auto build = startBuildFor(s, ns, "part_1");
    const ManifestEntry entry = blobManifestEntryStreaming("data.bin", "payload-ABS");
    build->adoptEvidence(entry);
    const ManifestId id = build->stageManifest({entry});
    build->precommitAdd(ns, "part_1", id);

    /// A landed GC delete: X is gone by its current token — absent with no source to re-upload.
    ASSERT_EQ(b->deleteExact(blob_key, t0).kind, DeleteOutcome::Kind::Deleted);

    expectThrowsCode(DB::ErrorCodes::ABORTED, [&] { build->promote(ns, "part_1", build->buildId(), id); });
    EXPECT_FALSE(s->resolveRef(ns, "part_1").has_value());
}

TEST(CasBuild, PromoteCondemnedLeafWithoutDepAbortsFailClosed)
{
    /// A manifest blob leaf with NO recorded dep (a staging-bug shape: neither putBlob nor adoptEvidence
    /// recorded it) that is condemned at the gate must fail closed — isCopyForwardableTokenless is false,
    /// so the gate never silently copies it forward. The no-dep shape is reachable through the public build
    /// API (stageManifest names the leaf without any dep having been recorded), so no test accessor for the
    /// private predicate is needed.
    auto b = std::make_shared<InMemoryBackend>();
    auto s = openStore(b);
    const RootNamespace ns{"srv1/tbl"};

    /// X exists (streaming-keyed) but THIS build records NO dep for it (no putBlob, no adoptEvidence).
    {
        auto seed = s->startBuild({});
        seed->putBlob(BlobId{streamingHexOf("payload-NODEP")}, BlobSource::fromString("payload-NODEP"));
    }
    const String blob_key = s->layout().blobKey(BlobId{streamingHexOf("payload-NODEP")});
    const Token t0 = b->head(blob_key).token;

    auto build = startBuildFor(s, ns, "part_1");
    const ManifestEntry entry = blobManifestEntryStreaming("data.bin", "payload-NODEP");
    /// NB: NO adoptEvidence(entry) — deps stays empty for this hash, so isCopyForwardableTokenless is false.
    const ManifestId id = build->stageManifest({entry});
    build->precommitAdd(ns, "part_1", id);

    /// Condemn X (present) via the meta (v3: a per-hash point-read, not the retire-view) so the gate
    /// sees the condemnation on the no-dep leaf.
    condemnMeta(*b, s->layout(), hexToU128(streamingHexOf("payload-NODEP")), /*condemn_round*/ 1);

    expectThrowsCode(DB::ErrorCodes::ABORTED, [&] { build->promote(ns, "part_1", build->buildId(), id); });
    EXPECT_FALSE(s->resolveRef(ns, "part_1").has_value());
    /// The condemned token was never displaced (no copy-forward happened).
    EXPECT_EQ(b->head(blob_key).token, t0);
}

/// A one-shot hook: the FIRST head(target_key) returns the (stale) result and THEN displaces the
/// incarnation via putOverwrite(target_key, <same bytes>, expected) — a racing writer's recreate /
/// copy-forward landing in the observe->GET window. The fresh token is captured in `displaced_to`.
class HeadThenDisplaceOnceBackend final : public DB::Cas::Backend
{
public:
    /// `layout`/`hash` are v3 additions (Task 4): the hook models a COMPLETE racing writer, so after its
    /// own displacement it also flips the per-hash meta back to Clean — mirroring what a real
    /// copy-forward/resurrect does (step 4). Without this, the meta (a per-hash marker, not per-token)
    /// would still read Condemned and the promote gate's OWN copy-forward would legitimately re-displace
    /// this "racer" incarnation a second time.
    HeadThenDisplaceOnceBackend(BackendPtr inner_, String target_key_, DB::Cas::Token expected_,
                                DB::Cas::Layout layout_, DB::UInt128 hash_)
        : inner(std::move(inner_)), target_key(std::move(target_key_)), expected(expected_),
          layout(std::move(layout_)), hash(hash_) {}

    DB::Cas::HeadResult head(const String & k) override
    {
        const DB::Cas::HeadResult hr = inner->head(k);
        if (k == target_key && !fired)
        {
            fired = true;
            const auto bytes = inner->get(target_key);
            const auto res = inner->putOverwrite(target_key, bytes->bytes, expected);
            EXPECT_EQ(res.outcome, DB::Cas::PutOutcome::Done);
            displaced_to = res.token;

            const auto lm = DB::Cas::loadMeta(*inner, layout, hash);
            const DB::Cas::BlobMeta clean{.state = DB::Cas::MetaState::Clean, .condemn_round = 0,
                                          .size = bytes->bytes.size()};
            if (lm)
                DB::Cas::casMeta(*inner, layout, hash, lm->etag, clean);
            else
                DB::Cas::putMetaIfAbsent(*inner, layout, hash, clean);
        }
        return hr;
    }

    std::optional<DB::Cas::GetResult> get(const String & k, DB::Cas::Range r = {}) override { return inner->get(k, r); }
    std::optional<DB::Cas::GetStreamResult> getStream(const String & k, DB::Cas::Range r = {}) override { return inner->getStream(k, r); }
    DB::Cas::ListPage list(const String & p, const String & c, size_t l) override { return inner->list(p, c, l); }
    DB::Cas::PutResult putIfAbsent(const String & k, const String & b, const DB::Cas::ObjectMeta & meta = {}) override { return inner->putIfAbsent(k, b, meta); }
    DB::Cas::WriteSinkPtr putIfAbsentStream(const String & k, const DB::Cas::ObjectMeta & meta = {}) override { return inner->putIfAbsentStream(k, meta); }
    DB::Cas::PutResult putOverwrite(const String & k, const String & b, const DB::Cas::Token & e, const DB::Cas::ObjectMeta & meta = {}) override { return inner->putOverwrite(k, b, e, meta); }
    DB::Cas::CasResult casPut(const String & k, const String & b, const std::optional<DB::Cas::Token> & e, const DB::Cas::ObjectMeta & meta = {}) override { return inner->casPut(k, b, e, meta); }
    DB::Cas::DeleteOutcome deleteExact(const String & k, const DB::Cas::Token & t) override { return inner->deleteExact(k, t); }
    bool supportsListTokens() const override { return inner->supportsListTokens(); }

    DB::Cas::Token displaced_to;

private:
    BackendPtr inner;
    String target_key;
    DB::Cas::Token expected;
    DB::Cas::Layout layout;
    DB::UInt128 hash;
    bool fired = false;
};

/// SOAK 2026-07-03 regression (false CORRUPTED_DATA attach brick): the pool-wide content hash is
/// the STREAMING HashingWriteBuffer convention (chunked CityHash128, block = 2048 B), which
/// diverges from a one-shot CityHash128 for any payload larger than one block. Copy-forward's
/// verification must recompute with the pool convention — this test uses a MULTI-BLOCK payload
/// whose BlobId is minted exactly as the write path mints it.
TEST(CasBuild, CopyForwardMultiBlockPayloadVerifies)
{
    auto b = std::make_shared<InMemoryBackend>();
    const RootNamespace ns{"srv1/tbl"};

    String payload;
    payload.reserve(5000);
    for (size_t i = 0; i < 5000; ++i)
        payload.push_back(static_cast<char>('a' + (i * 131 + i / 7) % 23));
    ASSERT_GT(payload.size(), 2 * DBMS_DEFAULT_HASHING_BLOCK_SIZE) << "must span multiple hash blocks";

    /// Mint the id EXACTLY as ContentAddressedWriteBuffers does: streaming hash + lowercase hex.
    DB::ReadBufferFromMemory in(payload.data(), payload.size());
    DB::HashingReadBuffer hashing(in);
    hashing.ignoreAll();
    const String hash_hex = getHexUIntLowercase(hashing.getHash());
    const BlobId id{hash_hex};
    const UInt128 hash = hexToU128(hash_hex);

    Token t0;
    {
        auto s0 = openStore(b);
        auto build = startBuildFor(s0, ns, "part_a");
        build->putBlob(id, BlobSource::fromString(payload));
        ManifestEntry e;
        e.path = "data.bin";
        e.placement = EntryPlacement::Blob;
        e.blob_hash = hash;
        e.blob_size = payload.size();
        const ManifestId mid0 = build->stageManifest({e});
        build->precommitAdd(ns, "part_a", mid0);
        build->promote(ns, "part_a", build->buildId(), mid0);
        t0 = b->head(s0->layout().blobKey(id)).token;
    }

    DB::Cas::Layout layout("p");
    /// v3: condemn the hash via the meta (the promote gate's condemned decision is a per-hash point-read).
    condemnMeta(*b, layout, hash, /*condemn_round*/ 1);

    auto s = Store::open(b, PoolConfig{.pool_prefix = "p", .server_root_id = "test"});
    auto build = startBuildFor(s, ns, "detached_part_a");
    ManifestEntry e;
    e.path = "data.bin";
    e.placement = EntryPlacement::Blob;
    e.blob_hash = hash;
    e.blob_size = payload.size();
    build->adoptEvidence(e);
    const ManifestId mid = build->stageManifest({e});
    build->precommitAdd(ns, "detached_part_a", mid);

    /// BEFORE fix: one-shot CityHash mismatch => false CORRUPTED_DATA (the soak attach brick).
    /// AFTER fix: streaming recompute matches => copy-forward displaces and promote succeeds.
    EXPECT_NO_THROW(build->promote(ns, "detached_part_a", build->buildId(), mid));
    EXPECT_NE(b->head(layout.blobKey(id)).token, t0);
}

TEST(CasBuild, CopyForwardTokenDriftAdoptsCleanIncarnation)
{
    /// Copy-forward race arm: between the pre-pass HEAD (stale condemned token) and the copy-forward
    /// read, a racing writer displaces the condemned incarnation (their recreate landed first). The
    /// copy-forward must ADOPT the racer's clean token — one incarnation, not a second displacement.
    auto b = std::make_shared<InMemoryBackend>();
    const RootNamespace ns{"srv1/tbl"};

    Token t0;
    {
        auto s0 = openStore(b);
        publishOneBlobPart(s0, ns, "part_a", "data.bin", "payload-DRIFT");
        t0 = b->head(s0->layout().blobKey(idOf("payload-DRIFT"))).token;
    }
    DB::Cas::Layout layout("p");
    const String blob_key = layout.blobKey(idOf("payload-DRIFT"));
    /// v3: condemn the hash via the meta (the promote gate's condemned decision is a per-hash point-read).
    condemnMeta(*b, layout, u128Of("payload-DRIFT"), /*condemn_round*/ 1);

    auto hook = std::make_shared<HeadThenDisplaceOnceBackend>(b, blob_key, t0, layout, u128Of("payload-DRIFT"));
    auto s = Store::open(hook, PoolConfig{.pool_prefix = "p", .server_root_id = "test"});
    auto build = startBuildFor(s, ns, "detached_part_a");
    build->adoptEvidence(blobManifestEntry("data.bin", "payload-DRIFT"));
    const ManifestId mid = build->stageManifest({blobManifestEntry("data.bin", "payload-DRIFT")});
    build->precommitAdd(ns, "detached_part_a", mid);
    EXPECT_NO_THROW(build->promote(ns, "detached_part_a", build->buildId(), mid));

    /// The final incarnation is the RACER's (adopted), not a third token from a second displacement.
    const HeadResult hr = b->head(blob_key);
    ASSERT_TRUE(hr.exists);
    EXPECT_EQ(hr.token, hook->displaced_to) << "clean drifted token must be adopted, not displaced again";
    EXPECT_NE(hr.token, t0);
}

TEST(CasBuild, CopyForwardCorruptPayloadFailsClosed)
{
    /// Copy-forward verification arm: the condemned-but-present object's payload no longer re-hashes
    /// to its content key (bit rot / partial write). Copy-forward must REFUSE to re-publish the bytes
    /// (CORRUPTED_DATA), never mint a fresh incarnation over damaged content.
    auto b = std::make_shared<InMemoryBackend>();
    const RootNamespace ns{"srv1/tbl"};

    Token t0;
    {
        auto s0 = openStore(b);
        publishOneBlobPartStreaming(s0, ns, "part_a", "data.bin", "payload-ROT");
        t0 = b->head(s0->layout().blobKey(BlobId{streamingHexOf("payload-ROT")})).token;
    }
    DB::Cas::Layout layout("p");
    const String blob_key = layout.blobKey(BlobId{streamingHexOf("payload-ROT")});

    /// Corrupt ONE payload byte in place (valid envelope, damaged content); condemn the hash via the
    /// meta (v3: a per-hash point-read, not the retire-view — condemnation no longer distinguishes
    /// which token, so the corrupt incarnation's token doesn't need to be captured separately).
    {
        auto got = b->get(blob_key);
        ASSERT_TRUE(got.has_value());
        const auto h = decodeEnvelopeHeader(got->bytes, got->bytes.size(), ObjectKind::Blob);
        String damaged = got->bytes;
        damaged[h.header_len] = static_cast<char>(damaged[h.header_len] ^ 0xFF);
        const auto res = b->putOverwrite(blob_key, damaged, t0);
        ASSERT_EQ(res.outcome, PutOutcome::Done);
        condemnMeta(*b, layout, hexToU128(streamingHexOf("payload-ROT")), /*condemn_round*/ 1);
    }

    auto s = Store::open(b, PoolConfig{.pool_prefix = "p", .server_root_id = "test"});
    auto build = startBuildFor(s, ns, "detached_part_a");
    build->adoptEvidence(blobManifestEntryStreaming("data.bin", "payload-ROT"));
    const ManifestId mid = build->stageManifest({blobManifestEntryStreaming("data.bin", "payload-ROT")});
    build->precommitAdd(ns, "detached_part_a", mid);
    expectThrowsCode(DB::ErrorCodes::CORRUPTED_DATA,
        [&] { build->promote(ns, "detached_part_a", build->buildId(), mid); });
    EXPECT_FALSE(s->resolveRef(ns, "detached_part_a").has_value());
}

TEST(CasBuild, GateBodylessAdoptFullyDeletedObjectThrowsAbortedNotFatal)
{
    /// B190 residual (soak bug): the promote gate revalidating a blob leaf whose object is FULLY
    /// GC-DELETED (HEAD absent, not merely condemned-present) must throw ABORTED — not the old fatal
    /// FILE_DOESNT_EXIST.
    ///
    /// Scenario:
    ///   1. Blob X exists with token t0; a tokenless dep is recorded via adoptEvidence (no body in hand).
    ///   2. GC condemns X (retire view hit by hash) AND fully deletes the object (deleteExact → HEAD absent).
    ///   3. The promote gate runs: HEADs the blob leaf → absent → should throw ABORTED (retryable, INV-3),
    ///      NOT FILE_DOESNT_EXIST (fatal).
    ///
    /// The gate's absent-object path is distinct from the condemned-present path tested by
    /// PromoteBodylessCondemnedDepThrowsAbortedRetryable (which fires GC delete AFTER the HEAD via
    /// HeadThenDeleteOnceBackend). HERE the object is absent BEFORE the HEAD call.
    auto b = std::make_shared<InMemoryBackend>();
    const RootNamespace ns{"srv1/tbl"};

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

    /// 2. Condemn (Blob, hash(B190), t0) in the retire view AND immediately GC-delete the object.
    injectRetire(*b, layout, /*round*/ 1, /*fence_seq*/ 0, /*shard*/ 0,
        {RetiredEntry{.kind = ObjectKind::Blob, .hash = u128Of("payload-B190"), .token = t0, .size = 11}});
    ASSERT_EQ(b->deleteExact(blob_key, t0).kind, DeleteOutcome::Kind::Deleted);
    ASSERT_FALSE(b->head(blob_key).exists) << "object must be absent before the gate HEAD";

    /// 3. Open a fresh Store over the raw backend — retire view (refreshed at open) sees the condemnation.
    auto s = Store::open(b, PoolConfig{.pool_prefix = "p", .server_root_id = "test"});
    auto build = startBuildFor(s, ns, "part_1");

    /// 4. Adopt the blob as a tokenless evidence dep; stage a manifest naming it; precommit. No body in hand.
    build->adoptEvidence(blobManifestEntry("data.bin", "payload-B190"));
    const ManifestId mid = build->stageManifest({blobManifestEntry("data.bin", "payload-B190")});
    build->precommitAdd(ns, "part_1", mid);

    /// 5. promote drives the gate: HEADs the blob leaf → absent → ABORTED (INV-3).
    ///    BEFORE fix: threw FILE_DOESNT_EXIST → fatal INSERT failure.
    ///    AFTER fix:  throws ABORTED (retryable) — the outer INSERT retries and re-materializes from source.
    expectThrowsCode(DB::ErrorCodes::ABORTED,
        [&] { build->promote(ns, "part_1", build->buildId(), mid); });

    /// No ref was committed and the blob stays absent.
    EXPECT_FALSE(s->resolveRef(ns, "part_1").has_value());
    EXPECT_FALSE(b->head(blob_key).exists);
}

TEST(CasBuild, PromoteRevalidatesBlobPresenceFailClosed)
{
    /// Port of the old W-TREE-BUILD bottom-up enforcement (PutTreeEnforcesBottomUp). In the part-manifest
    /// model stageManifest does not validate its entries' bodies (the body is just written); the fail-closed
    /// authority moved to the promote gate (CasBuild.cpp promote step 3), which HEADs EVERY blob leaf and
    /// ABORTs on a missing one. This is the surviving "a committed ref never names a missing dependency"
    /// invariant.
    auto b = std::make_shared<InMemoryBackend>();
    auto s = openStore(b);
    const RootNamespace ns{"srv1/tbl"};

    /// Stage + precommit a manifest naming a blob hash that was NEVER uploaded.
    auto build = startBuildFor(s, ns, "part_1");
    const ManifestId mid = build->stageManifest({blobManifestEntry("data.bin", "never-uploaded")});
    build->precommitAdd(ns, "part_1", mid);

    /// promote must fail closed: the blob leaf is absent at commit revalidation ⇒ ABORTED. No ref committed.
    expectThrowsCode(DB::ErrorCodes::ABORTED,
        [&] { build->promote(ns, "part_1", build->buildId(), mid); });
    EXPECT_FALSE(s->resolveRef(ns, "part_1").has_value());

    /// After uploading the blob, a fresh build's promote succeeds — the same manifest content is now
    /// fully present.
    auto build2 = startBuildFor(s, ns, "part_1");
    build2->putBlob(idOf("never-uploaded"), BlobSource::fromString("never-uploaded"));
    const ManifestId mid2 = build2->stageManifest({blobManifestEntry("data.bin", "never-uploaded")});
    build2->precommitAdd(ns, "part_1", mid2);
    EXPECT_NO_THROW(build2->promote(ns, "part_1", build2->buildId(), mid2));
    EXPECT_TRUE(s->resolveRef(ns, "part_1").has_value());
}

TEST(CasBuild, AdoptEvidenceRecordsTokenlessDep)
{
    /// Port of AdoptFromTreeRecordsEvidence. adoptEvidence records a TOKENLESS W-EVIDENCE dep directly
    /// from a resolved ManifestEntry — a Blob entry is tokenless (depIsTokened false), an Inline entry
    /// records nothing. The dep's EXISTENCE + copy-forwardability (tokenless-recorded vs no-dep) is
    /// asserted end-to-end at the promote gate by CondemnedPresentEvidenceDepCopiesForwardAtGate
    /// (positive: tokenless dep ⇒ copy-forward) and PromoteCondemnedLeafWithoutDepAbortsFailClosed
    /// (negative control: no dep ⇒ fail closed).
    auto b = std::make_shared<InMemoryBackend>();
    auto s = openStore(b);
    auto build = s->startBuild({});

    const ManifestEntry adopted = blobManifestEntry("data.bin", "source-blob");
    build->adoptEvidence(adopted);
    EXPECT_FALSE(build->depIsTokened(u128Of("source-blob")));

    /// An Inline entry references no standalone object → records nothing (never tokened).
    ManifestEntry inline_entry;
    inline_entry.path = "small";
    inline_entry.placement = EntryPlacement::Inline;
    inline_entry.inline_bytes = "abc";
    build->adoptEvidence(inline_entry);
    EXPECT_FALSE(build->depIsTokened(u128Of("abc")));
}

TEST(CasBuild, AbandonRemovesStagedDebrisAndDisables)
{
    /// Port of AbandonLeavesDebrisAndDisables to the new abandon semantics (CasBuild.cpp abandon):
    /// abandon best-effort exact-token-DELETEs this build's STAGED manifest debris, leaves blob bodies
    /// (full GC's job via min_active), and disables the build (further ops throw via requireAlive).
    auto b = std::make_shared<InMemoryBackend>();
    auto s = openStore(b);
    const RootNamespace ns{"srv1/tbl"};
    auto build = startBuildFor(s, ns, "ref");

    auto blob_ref = build->putBlob(idOf("kept"), BlobSource::fromString("kept"));
    const ManifestId mid = build->stageManifest({blobManifestEntry("f", "kept")});

    /// The staged manifest body and the blob are present before abandon.
    EXPECT_TRUE(b->head(s->layout().blobKey(blob_ref.id)).exists);
    EXPECT_TRUE(b->head(s->layout().manifestKey(mid)).exists);

    build->abandon();

    /// Blob stays (debris — full GC reclaims it). The staged manifest debris is best-effort cleaned now.
    EXPECT_TRUE(b->head(s->layout().blobKey(blob_ref.id)).exists);
    EXPECT_FALSE(b->head(s->layout().manifestKey(mid)).exists)
        << "abandon must best-effort delete this build's staged manifest debris";

    /// Further operations throw via requireAlive.
    expectThrowsCode(DB::ErrorCodes::LOGICAL_ERROR,
        [&] { build->putBlob(idOf("after"), BlobSource::fromString("after")); });
    expectThrowsCode(DB::ErrorCodes::LOGICAL_ERROR, [&] { build->stageManifest({blobManifestEntry("g", "kept")}); });
    expectThrowsCode(DB::ErrorCodes::LOGICAL_ERROR, [&] { build->precommitAdd(ns, "ref", mid); });
}

TEST(CasBuild, PublishHappyPathRoundTrip)
{
    auto b = std::make_shared<InMemoryBackend>();
    auto s = openStore(b);
    const RootNamespace ns{"srv1/tbl"};
    auto build = startBuildFor(s, ns, "part_1");

    auto blob = build->putBlob(idOf("hello world"), BlobSource::fromString("hello world"));
    EXPECT_EQ(blob.size, 11u);

    const ManifestId id = build->stageManifest({blobManifestEntry("data.bin", "hello world")});
    build->precommitAdd(ns, "part_1", id);
    build->setPendingMutableFiles({{"txn_version.txt", "1"}});
    build->promote(ns, "part_1", build->buildId(), id);

    auto r = s->resolveRef(ns, "part_1");
    ASSERT_TRUE(r.has_value());
    EXPECT_EQ(r->manifest_id, id);
    EXPECT_EQ(r->mutable_files.at("txn_version.txt"), "1");

    /// Read the manifest back and locate its single blob leaf.
    const PartManifest manifest = s->readManifest(id);
    ASSERT_EQ(manifest.entries.size(), 1u);
    const auto * entry = findEntry(manifest.entries, "data.bin");
    ASSERT_TRUE(entry != nullptr);
    const auto loc = s->locate(*entry);
    auto got = b->get(loc.key, Range{loc.offset, loc.length});
    ASSERT_TRUE(got.has_value());
    EXPECT_EQ(got->bytes, "hello world");

    /// journal: read the shard manifest raw, assert the last RootOwnerEvent is the committed owner move
    /// for "part_1" naming this manifest_ref, and that refs[part_1] points at it.
    const uint64_t shard = shardOfForTest("part_1", s->poolMeta().root_shards);
    auto shard_raw = b->get(s->layout().rootShardKey(ns, shard));
    ASSERT_TRUE(shard_raw.has_value());
    const RootShard root = decodeRootShard(shard_raw->bytes);
    ASSERT_FALSE(root.journal.empty());
    const RootOwnerEvent & last = root.journal.back();
    ASSERT_TRUE(last.new_binding.has_value());
    EXPECT_EQ(last.new_binding->owner_kind, OwnerKind::Committed);
    EXPECT_EQ(last.new_binding->ref_name, "part_1");
    EXPECT_EQ(last.new_binding->manifest_ref, id.ref);
    EXPECT_EQ(last.transition_version, root.shard_version);
    ASSERT_TRUE(root.refs.contains("part_1"));
    EXPECT_EQ(root.refs.at("part_1").manifest_ref, id.ref);
}

TEST(CasBuild, PromoteCrossNamespaceManifestFailsClosed)
{
    /// Port of PublishRequiresTreeInDepSet. The W-DEP-SET "root must be a built/adopted dep" authority
    /// is gone (the tree object model it guarded is gone); the surviving fail-closed authority that
    /// refuses an inconsistent commit target is the namespace consistency check in precommitAdd/promote
    /// (CasBuild.cpp): a manifest whose root_namespace != the target namespace is a bug ⇒ LOGICAL_ERROR.
    auto b = std::make_shared<InMemoryBackend>();
    auto s = openStore(b);
    const RootNamespace ns{"srv1/tbl"};
    const RootNamespace other_ns{"srv1/other"};

    auto build = startBuildFor(s, ns, "part_1");
    build->putBlob(idOf("hello world"), BlobSource::fromString("hello world"));
    /// The manifest is minted in `ns` (derived from intended_ref). Promoting/precommitting it into a
    /// DIFFERENT namespace must fail closed.
    const ManifestId id = build->stageManifest({blobManifestEntry("data.bin", "hello world")});

    expectThrowsCode(DB::ErrorCodes::LOGICAL_ERROR,
        [&] { build->precommitAdd(other_ns, "part_1", id); });
    expectThrowsCode(DB::ErrorCodes::LOGICAL_ERROR,
        [&] { build->promote(other_ns, "part_1", build->buildId(), id); });
}

TEST(CasBuild, PublishOwnThreadConflictRetries)
{
    auto b = std::make_shared<InMemoryBackend>();
    auto s = openStore(b);
    const RootNamespace ns{"srv1/tbl"};
    auto build = startBuildFor(s, ns, "part_1");

    build->putBlob(idOf("hello world"), BlobSource::fromString("hello world"));
    const ManifestId id = build->stageManifest({blobManifestEntry("data.bin", "hello world")});
    build->precommitAdd(ns, "part_1", id);

    /// One artificial Conflict on the shard's NEXT casPut (the promote owner move). mutateShard re-reads
    /// + re-runs the lambda and lands on the retry.
    const uint64_t shard = shardOfForTest("part_1", s->poolMeta().root_shards);
    b->failNextCasPut(s->layout().rootShardKey(ns, shard));

    build->promote(ns, "part_1", build->buildId(), id);

    auto r = s->resolveRef(ns, "part_1");
    ASSERT_TRUE(r.has_value());
    EXPECT_EQ(r->manifest_id, id);
}

TEST(CasBuild, PublishIntoSecondNamespaceSameBlob)
{
    /// Port of PublishIntoSecondNamespaceSameTree. A part manifest is single-owner and namespace-qualified
    /// (precommitAdd/promote enforce id.root_namespace == target_ns), so the SAME ManifestId cannot be
    /// published into two namespaces — each namespace gets its OWN manifest. The invariant the original
    /// test protected is preserved at the BLOB plane: the shared blob is uploaded ONCE and adopted by the
    /// second build (its token is unchanged after the second publish).
    auto b = std::make_shared<InMemoryBackend>();
    auto s = openStore(b);
    const RootNamespace ns1{"srv1/tbl"};
    const RootNamespace ns2{"srv1/tbl/detached"};

    /// First build publishes part_1 in ns1, uploading the blob.
    auto build1 = startBuildFor(s, ns1, "part_1");
    auto blob = build1->putBlob(idOf("hello world"), BlobSource::fromString("hello world"));
    const String blob_key = s->layout().blobKey(blob.id);
    const Token blob_token = b->head(blob_key).token;
    const ManifestId id1 = build1->stageManifest({blobManifestEntry("data.bin", "hello world")});
    build1->precommitAdd(ns1, "part_1", id1);
    build1->promote(ns1, "part_1", build1->buildId(), id1);

    /// Second build publishes part_1 in ns2 referencing the SAME blob: putBlob dedup-hits and ADOPTS the
    /// present incarnation (no re-upload), so the blob token is unchanged. Wiring order
    /// (EDGE-BEFORE-OBSERVE): stageManifest -> precommitAdd -> putBlob -> promote.
    auto build2 = startBuildFor(s, ns2, "part_1");
    const ManifestId id2 = build2->stageManifest({blobManifestEntry("data.bin", "hello world")});
    build2->precommitAdd(ns2, "part_1", id2);
    build2->putBlob(idOf("hello world"), BlobSource::fromString("hello world"));
    build2->promote(ns2, "part_1", build2->buildId(), id2);

    auto r1 = s->resolveRef(ns1, "part_1");
    auto r2 = s->resolveRef(ns2, "part_1");
    ASSERT_TRUE(r1.has_value());
    ASSERT_TRUE(r2.has_value());
    EXPECT_EQ(r1->manifest_id, id1);
    EXPECT_EQ(r2->manifest_id, id2);

    /// The blob object was uploaded once: its token is unchanged after both publishes.
    EXPECT_EQ(b->head(blob_key).token, blob_token);
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
    auto build_a = startBuildFor(s, ns, ref1);
    build_a->putBlob(idOf("content-a"), BlobSource::fromString("content-a"));
    const ManifestId id_a = build_a->stageManifest({blobManifestEntry("data.bin", "content-a")});
    build_a->precommitAdd(ns, ref1, id_a);
    build_a->promote(ns, ref1, build_a->buildId(), id_a);

    /// Build B publishes ref2 into the same shard: its mutateShard sees A's manifest (shard_version
    /// advanced past 0), re-reads, and lands. The single artificial conflict forces B to genuinely
    /// re-read after the first attempt.
    auto build_b = startBuildFor(s, ns, ref2);
    build_b->putBlob(idOf("content-b"), BlobSource::fromString("content-b"));
    const ManifestId id_b = build_b->stageManifest({blobManifestEntry("data.bin", "content-b")});
    build_b->precommitAdd(ns, ref2, id_b);

    const uint64_t shard = shardOfForTest(ref2, root_shards);
    b->failNextCasPut(s->layout().rootShardKey(ns, shard));
    build_b->promote(ns, ref2, build_b->buildId(), id_b);

    /// Both refs resolve.
    auto r1 = s->resolveRef(ns, ref1);
    auto r2 = s->resolveRef(ns, ref2);
    ASSERT_TRUE(r1.has_value());
    ASSERT_TRUE(r2.has_value());
    EXPECT_EQ(r1->manifest_id, id_a);
    EXPECT_EQ(r2->manifest_id, id_b);

    /// The shared manifest holds both refs and two committed-owner events.
    auto shard_raw = b->get(s->layout().rootShardKey(ns, shard));
    ASSERT_TRUE(shard_raw.has_value());
    const RootShard root = decodeRootShard(shard_raw->bytes);
    EXPECT_TRUE(root.refs.contains(ref1));
    EXPECT_TRUE(root.refs.contains(ref2));

    /// Each promote appends a precommit->committed owner MOVE (old_binding={Precommit}, new={Committed},
    /// same manifest_ref). Both refs serialized into the shared shard, so there are exactly two such moves.
    size_t committed_moves = 0;
    for (const RootOwnerEvent & rec : root.journal)
        if (rec.old_binding && rec.old_binding->owner_kind == OwnerKind::Precommit
            && rec.new_binding && rec.new_binding->owner_kind == OwnerKind::Committed)
            ++committed_moves;
    EXPECT_EQ(committed_moves, 2u);
    EXPECT_EQ(root.refs.size(), 2u);
}

TEST(CasBuild, FirstPublishMakesNamespaceDiscoverable)
{
    /// After Task 4 the registry is deleted; a namespace becomes discoverable via LIST(cas/refs/)
    /// once its first ref shard exists (created by precommitAdd/promote).
    auto b = std::make_shared<InMemoryBackend>();
    auto s = Store::open(b, PoolConfig{.pool_prefix = "p", .server_root_id = "test"});
    const RootNamespace ns{"srv9/fresh"};

    EXPECT_TRUE(s->listNamespaces("").empty());
    publishOneBlobPart(s, ns, "part_1", "f", "reg-payload");

    /// The namespace is now discoverable via LIST — no registry write needed.
    const auto all = s->listNamespaces("");
    ASSERT_EQ(all.size(), 1u);
    EXPECT_EQ(all[0], "srv9/fresh");
}

TEST(CasBuild, AdoptEvidenceNoBackendOp)
{
    /// B188: adoptEvidence records a TOKENLESS W-EVIDENCE dep from an already-resolved ManifestEntry
    /// WITHOUT any backend call (no HEAD, no GET, no PUT).
    ///
    /// Two behavioural assertions:
    ///   1. No backend op fires during adoptEvidence (counted via a delegating wrapper).
    ///   2. The recorded dep is tokenless: after adoptEvidence(entry), depIsTokened is false (the
    ///      W-EVIDENCE dep is what the promote gate later revalidates).

    /// A delegating wrapper that counts every backend access path including the streaming write path.
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
        std::optional<GetStreamResult> getStream(const String & k, Range r = {}) override { return inner->getStream(k, r); }
        ListPage list(const String & p, const String & c, size_t l) override { return inner->list(p, c, l); }
        PutResult putIfAbsent(const String & k, const String & bts, const ObjectMeta & m = {}) override { return inner->putIfAbsent(k, bts, m); }
        PutResult putOverwrite(const String & k, const String & bts, const Token & e, const ObjectMeta & m = {}) override { return inner->putOverwrite(k, bts, e, m); }
        CasResult casPut(const String & k, const String & bts, const std::optional<Token> & e, const ObjectMeta & m = {}) override { return inner->casPut(k, bts, e, m); }
        DeleteOutcome deleteExact(const String & k, const Token & t) override { return inner->deleteExact(k, t); }
        bool supportsListTokens() const override { return inner->supportsListTokens(); }
    private:
        BackendPtr inner;
    };

    auto raw = std::make_shared<InMemoryBackend>();
    auto counting = std::make_shared<LocalCountingBackend>(raw);
    auto s = Store::open(counting, PoolConfig{.pool_prefix = "p", .server_root_id = "test"});
    auto build = s->startBuild({});

    /// A Blob ManifestEntry. adoptEvidence is called on a hand-crafted entry — that IS the B188 interface.
    const ManifestEntry entry = blobManifestEntry("b188.bin", "b188-content");

    /// Reset the counters after Store::open (which may HEAD/GET gc/server-roots etc. during startup).
    counting->heads = 0;
    counting->stream_puts = 0;
    counting->gets = 0;

    /// adoptEvidence — must record the dep WITHOUT touching the backend.
    EXPECT_NO_THROW(build->adoptEvidence(entry));
    EXPECT_EQ(counting->heads, 0u) << "adoptEvidence must not HEAD the backend";
    EXPECT_EQ(counting->stream_puts, 0u) << "adoptEvidence must not PUT to the backend";
    EXPECT_EQ(counting->gets, 0u) << "adoptEvidence must not GET from the backend";

    /// The dep is recorded — a tokenless W-EVIDENCE dep (depIsTokened false) that the promote gate later
    /// revalidates; the no-backend-op counts above are the B188 contract's primary guard.
    EXPECT_FALSE(build->depIsTokened(u128Of("b188-content")));

    /// Inline entry: adoptEvidence records nothing (Inline has no standalone object) and no backend op.
    ManifestEntry inline_entry;
    inline_entry.path = "small";
    inline_entry.placement = EntryPlacement::Inline;
    inline_entry.inline_bytes = "xy";
    EXPECT_NO_THROW(build->adoptEvidence(inline_entry));
    EXPECT_EQ(counting->heads, 0u);
    EXPECT_EQ(counting->stream_puts, 0u);
    EXPECT_EQ(counting->gets, 0u);
    EXPECT_FALSE(build->depIsTokened(u128Of("xy")));
}

TEST(CasBuild, ConvergesUnderProductiveGc)
{
    /// B167/B171 LIVENESS — the re-upload/condemn livelock, now closed by the build-root precommit edge.
    ///
    /// THE BUG (before the fix): a blob H was referenced, dropped, and GC-condemned (everEdged ∧ InDeg=0,
    /// condemned in the retire view). A NEW build dedup-HITS H by content and must re-upload it from
    /// source — it re-streams a FRESH incarnation of H. But the productive GC, re-deriving H as a
    /// zero-in-degree candidate every round, kept RE-CONDEMNING and exact-token-DELETING that fresh
    /// incarnation in the build's upload→commit window. The build never converged → livelock.
    ///
    /// THE FIX (B171): protection is the build-root PRECOMMIT EDGE. Build B precommits its manifest (naming
    /// H) BEFORE the adversarial loop, so the GC fold lifts H to in-degree ≥ 1 — H is never even a
    /// zero-in-degree candidate and is SPARED every round until B promotes (the committed ref then pins H).
    ///
    /// FORM: full adversarial loop. A real Gc drives complete runRegularRound rounds against the same
    /// pool while build B holds an active watermark covering H's incarnation. We assert H is SPARED
    /// every round and that B promotes within a BOUNDED number of GC rounds, after which H reads back.
    auto b = std::make_shared<InMemoryBackend>();
    const RootNamespace ns{"srv1/tbl"};

    /// root_shards=1 keeps the build-root precommit and the table ref in one shard (single-shard fold).
    PoolConfig cfg;
    cfg.pool_prefix = "p";
    cfg.server_root_id = "test";
    cfg.server_id = UInt128(0xAB);
    cfg.background_watermark = false;
    cfg.root_shards = 1;
    const String content = "shared-content";

    /// 1. Build A creates H ("shared-content"), publishes a part referencing it, then drops the ref.
    ///    Capture H's first incarnation token so we can condemn exactly it.
    BlobId h;
    Token h_token0;
    {
        auto s0 = Store::open(b, cfg);
        publishOneBlobPart(s0, ns, "part_1", "f", content);
        h = idOf(content);
        h_token0 = b->head(s0->layout().blobKey(h)).token;
        s0->dropRef(ns, "part_1");
    }

    /// 2. Condemn (Blob, H, h_token0): `injectRetire` seeds the LEDGER (a real round's later settle/spare
    ///    of h_token0 rides this entry — the adversarial loop below still exercises that), and v3's
    ///    `condemnMeta` seeds the per-hash META (the writer's condemned decision is now a point-read of
    ///    it, not the retire-view — Task 3). `publishOneBlobPart` already created H's meta as Clean, so
    ///    condemnMeta's read-modify-CAS finds it. Together these reproduce exactly what a real GC condemn
    ///    now writes (Task 5), without driving a full round just to observe H at in-degree 0.
    DB::Cas::Layout layout("p");
    injectRetire(*b, layout, /*round*/ 1, /*fence_seq*/ 0, /*shard*/ 0,
        {RetiredEntry{.kind = ObjectKind::Blob, .hash = u128Of(content), .token = h_token0,
                      .size = content.size()}});
    condemnMeta(*b, layout, u128Of(content), /*condemn_round*/ 1);

    /// 3. Open the live Store and start build B. B dedup-hits the condemned H and re-uploads from
    ///    source (uploadFromSource via putBlob): a fresh incarnation, a NEW token. B stays ACTIVE for
    ///    the whole adversarial loop — its build_seq is never retired below.
    auto s = Store::open(b, cfg);
    const String blob_key = s->layout().blobKey(h);
    auto build_b = startBuildFor(s, ns, "part_2");

    /// B190: use putBlob (holds source bytes). putBlob detects the condemned dedup hit and calls
    /// uploadFromSource — no GET of dying object.
    const auto ref_b = build_b->putBlob(h, BlobSource::fromString(content));
    ASSERT_EQ(ref_b.id, h);

    const HeadResult after_reupload = b->head(blob_key);
    ASSERT_TRUE(after_reupload.exists);
    EXPECT_NE(after_reupload.token, h_token0);   /// a genuinely fresh incarnation

    /// 4. Build B stages its manifest referencing H and PRECOMMITS it (build-root edge). H is now
    ///    protected by reachability: the GC fold lifts H to in-degree ≥ 1 from the precommit.
    const ManifestId mid_b = build_b->stageManifest({blobManifestEntry("f", content)});
    build_b->precommitAdd(ns, "part_2", mid_b);

    /// 5. THE ADVERSARIAL LOOP. A real, productive GC keeps trying to reclaim. It reclaims the now-
    ///    unreferenced part_1 manifest (build A's, UNprotected) but H stays pinned by B's PRECOMMIT edge
    ///    (in-degree ≥ 1), so H is never even a zero-in-degree candidate. We drive far more rounds than B
    ///    needs to promote; H must survive ALL of them. Each round renews B's watermark so the crash
    ///    detector keeps judging B live.
    Gc gc(s, hexToU128("00000000000000000000000000000001"));
    constexpr int MAX_GC_ROUNDS = 8;

    const auto driveRoundAndAssertHSpared = [&](int round_no)
    {
        /// A LIVE server renews its watermark continuously. Renew once per GC round so B's watermark seq
        /// ADVANCES between rounds — that is precisely what distinguishes a live server from a crashed one
        /// (a frozen B would have its precommit reclaimed; an advancing seq keeps it).
        s->renewWatermarkOnce();
        gc.runRegularRound();
        const HeadResult hr = b->head(blob_key);
        ASSERT_TRUE(hr.exists) << "H was deleted by GC at round " << round_no
                               << " despite being pinned by the live build B's precommit (B167 livelock would do this)";
        const auto raw = b->get(blob_key);
        ASSERT_TRUE(raw.has_value());
        const auto hdr = decodeEnvelopeHeader(raw->bytes, raw->bytes.size(), ObjectKind::Blob);
        EXPECT_EQ(raw->bytes.substr(hdr.header_len), content)
            << "H's content was lost/corrupted at round " << round_no;
    };

    /// Phase 1 — the livelock window. H is referenced by NO committed TABLE ref (B has not promoted yet)
    /// but IS named by B's precommit, so the build-root fold lifts it to in-degree ≥ 1. Drive several
    /// full rounds; the precommit edge must SPARE H's fresh incarnation every round.
    int rounds_run = 0;
    constexpr int PRE_PUBLISH_ROUNDS = 4;
    for (int i = 0; i < PRE_PUBLISH_ROUNDS; ++i)
    {
        driveRoundAndAssertHSpared(++rounds_run);
        if (::testing::Test::HasFatalFailure())
            return;
    }

    /// Phase 2 — converge. With H still alive (spared through the whole window), build B promotes a part
    /// referencing it. The promote gate sees H present + live (fresh incarnation uploaded above), so it
    /// commits. This MUST succeed — the build converges in bounded steps.
    build_b->promote(ns, "part_2", build_b->buildId(), mid_b);
    const bool published = true;

    /// Phase 3 — keep the GC hammering after promote. H is now pinned by the committed ref's manifest
    /// edge; the GC must keep sparing it as a genuinely-reachable node.
    while (rounds_run < MAX_GC_ROUNDS)
    {
        driveRoundAndAssertHSpared(++rounds_run);
        if (::testing::Test::HasFatalFailure())
            return;
    }

    /// 6. ASSERT convergence: promote SUCCEEDED within the bounded budget, and H reads back intact.
    ASSERT_TRUE(published) << "build B never published — the B167 livelock is back";
    EXPECT_LE(rounds_run, MAX_GC_ROUNDS);

    const auto resolved = s->resolveRef(ns, "part_2");
    ASSERT_TRUE(resolved.has_value());
    EXPECT_EQ(resolved->manifest_id, mid_b);

    const PartManifest manifest = s->readManifest(mid_b);
    ASSERT_EQ(manifest.entries.size(), 1u);
    const auto * entry = findEntry(manifest.entries, "f");
    ASSERT_TRUE(entry != nullptr);
    const auto loc = s->locate(*entry);
    const auto got = b->get(loc.key, Range{loc.offset, loc.length});
    ASSERT_TRUE(got.has_value());
    EXPECT_EQ(got->bytes, content);
}

TEST(CasBuild, AdoptedBlobVanishedIsRetryableNotFatal)
{
    /// B188 regression: the three adopt sites in ContentAddressedTransaction previously HEADed a
    /// committed-source blob DURING STAGING. The fix (B188) replaces those with adoptEvidence, which
    /// records a tokenless dep WITHOUT any backend call, deferring the observation to the promote gate
    /// (post-precommit). The gate throws retryable ABORTED when the blob is absent — not a fatal error.
    /// B190 removes reuseBlob entirely (no production callers).
    ///
    /// This test validates the adoptEvidence contract at the Build API level:
    ///   A) adoptEvidence + stageManifest + precommitAdd + promote on an absent blob → ABORTED (retryable),
    ///      NOT FILE_DOESNT_EXIST.
    ///   C) adoptEvidence + stageManifest + precommitAdd + promote on a PRESENT blob → succeeds (positive).
    auto b = std::make_shared<InMemoryBackend>();
    const RootNamespace ns{"srv1/tbl"};

    /// 1. Build A: upload the blob and publish a ref that references it. Capture the blob id and token.
    BlobId id;
    Token t0;
    {
        auto s0 = openStore(b);
        publishOneBlobPart(s0, ns, "part_1", "f.bin", "b188-content");
        id = idOf("b188-content");
        t0 = b->head(s0->layout().blobKey(id)).token;
    }

    DB::Cas::Layout layout("p");
    const String blob_key = layout.blobKey(id);

    /// Part execution order is load-bearing: C runs first (needs the blob present); A runs last because it
    /// permanently GC-deletes the blob to drive the absent-evidence gate path.
    ///
    /// Part C — POSITIVE: blob present; adoptEvidence + stageManifest + precommitAdd + promote succeeds.
    {
        auto s = openStore(b);
        auto build = startBuildFor(s, ns, "part_2");
        const ManifestEntry entry = blobManifestEntry("f.bin", "b188-content");
        build->adoptEvidence(entry);
        const ManifestId mid = build->stageManifest({entry});
        build->precommitAdd(ns, "part_2", mid);
        EXPECT_NO_THROW(build->promote(ns, "part_2", build->buildId(), mid));
    }

    /// Part A — NEW CONTRACT: adoptEvidence records a tokenless dep WITHOUT eager HEAD. The promote gate
    /// observes the blob and throws ABORTED when it is absent. This is retryable — the caller retries the
    /// whole INSERT which re-uploads from source.
    {
        auto s = openStore(b);
        auto build = startBuildFor(s, ns, "part_3");
        const ManifestEntry entry = blobManifestEntry("f.bin", "b188-content");

        /// adoptEvidence must NOT throw — no eager HEAD even though we will delete the blob next.
        EXPECT_NO_THROW(build->adoptEvidence(entry));
        const ManifestId mid = build->stageManifest({entry});
        /// precommitAdd must not throw either — it does not HEAD the blob.
        EXPECT_NO_THROW(build->precommitAdd(ns, "part_3", mid));

        /// Simulate the B188 race: GC deletes the blob in the adopt→promote window.
        b->deleteExact(blob_key, t0);
        ASSERT_FALSE(b->head(blob_key).exists);

        /// promote drives the gate: it HEADs the blob leaf → absent → ABORTED (retryable). NOT the old
        /// FILE_DOESNT_EXIST.
        expectThrowsCode(DB::ErrorCodes::ABORTED,
            [&] { build->promote(ns, "part_3", build->buildId(), mid); });
    }
}

/// BUG 1 (WPromote owner==bld): promote is a PURE owner MOVE (Δ=0 — it restores no blob in-degree). The
/// TLA+ `WPromote` requires the precommit to STILL be the live owner of the ref before the move (`owner[m]
/// = bld`). If the precommit was removed/reclaimed (an abandon or GC reclaim appended a removal event), a
/// Δ=0 move would re-publish a committed ref over blobs whose in-degree was already decremented to 0 — GC
/// then deletes them ⇒ a reachable committed manifest with dangling blobs (INV_NO_DANGLE violation).
/// promote MUST fail closed (ABORTED) unless the precommit is the current live owner binding of the ref.
TEST(CasBuild, PromoteFailsClosedWhenPrecommitNoLongerLiveOwner)
{
    auto b = std::make_shared<InMemoryBackend>();
    auto s = openStore(b);
    const RootNamespace ns{"srv1/tbl"};
    auto build = startBuildFor(s, ns, "part_1");

    build->putBlob(idOf("hello world"), BlobSource::fromString("hello world"));
    const ManifestId id = build->stageManifest({blobManifestEntry("data.bin", "hello world")});
    build->precommitAdd(ns, "part_1", id);

    /// Make the precommit NO LONGER the live owner: append a precommit-REMOVAL RootOwnerEvent on the
    /// target shard exactly as an abandon / GC reclaim would (old = the precommit binding, new = none).
    /// We drive the shard manifest CAS directly through the backend codec (the converged model keeps the
    /// precommit binding in the future committed ref's own shard, keyed by final_ref_name).
    {
        const uint64_t shard = shardOfForTest("part_1", s->poolMeta().root_shards);
        const String key = s->layout().rootShardKey(ns, shard);
        const auto got = b->get(key);
        ASSERT_TRUE(got.has_value());
        RootShard root = decodeRootShard(got->bytes);
        ++root.shard_version;
        root.journal.push_back(RootOwnerEvent{
            .transition_version = root.shard_version,
            .old_binding = OwnerBinding{
                .owner_kind = OwnerKind::Precommit, .ref_name = "part_1",
                .build_id = build->buildId(), .manifest_ref = id.ref},
            .new_binding = std::nullopt});
        ASSERT_EQ(b->casPut(key, encodeRootShard(root), got->token).outcome, CasOutcome::Committed);
    }

    /// promote must fail closed: the precommit is no longer the live owner, so a Δ=0 move would dangle.
    expectThrowsCode(DB::ErrorCodes::ABORTED,
        [&] { build->promote(ns, "part_1", build->buildId(), id); });
    /// No ref committed.
    EXPECT_FALSE(s->resolveRef(ns, "part_1").has_value());
}

/// BUG 1 happy path: a promote whose precommit is STILL the live owner succeeds (the guard must not
/// reject the normal commit). Distinct from PublishHappyPathRoundTrip in that it pins the WPromote guard.
TEST(CasBuild, PromoteSucceedsWhenPrecommitIsLiveOwner)
{
    auto b = std::make_shared<InMemoryBackend>();
    auto s = openStore(b);
    const RootNamespace ns{"srv1/tbl"};
    auto build = startBuildFor(s, ns, "part_1");

    build->putBlob(idOf("hello world"), BlobSource::fromString("hello world"));
    const ManifestId id = build->stageManifest({blobManifestEntry("data.bin", "hello world")});
    build->precommitAdd(ns, "part_1", id);

    EXPECT_NO_THROW(build->promote(ns, "part_1", build->buildId(), id));
    ASSERT_TRUE(s->resolveRef(ns, "part_1").has_value());
    EXPECT_EQ(s->resolveRef(ns, "part_1")->manifest_id, id);
}

/// BUG 2 (WAbandonPrecommit; delete-after-sealed-decrements): once `precommitAdd` has made a manifest a
/// LIVE precommit owner input, `abandon` must NOT writer-delete its body. The TLA+ `WAbandonPrecommit`
/// appends a REMOVAL event (`old = precommit(build_id, final_ref, T)`, `new = none`) and NEVER deletes
/// the body — GC decrements the precommit's blob edges and deletes the body only after the decrement is
/// sealed. Writer-deleting a live precommit body strands GC's fold barrier (live precommit, missing body
/// → clamp forever) or loses the activating +1.
TEST(CasBuild, AbandonAppendsPrecommitRemovalAndKeepsLivePrecommitBody)
{
    auto b = std::make_shared<InMemoryBackend>();
    auto s = openStore(b);
    const RootNamespace ns{"srv1/tbl"};
    auto build = startBuildFor(s, ns, "part_1");

    build->putBlob(idOf("kept"), BlobSource::fromString("kept"));
    const ManifestId mid = build->stageManifest({blobManifestEntry("data.bin", "kept")});
    const String manifest_key = s->layout().manifestKey(mid);
    const UInt128 abandoned_build_id = build->buildId();
    build->precommitAdd(ns, "part_1", mid);

    /// The precommit manifest body is present before abandon.
    ASSERT_TRUE(b->head(manifest_key).exists);

    build->abandon();

    /// (a) the LIVE precommit body must SURVIVE abandon (left for GC after the sealed decrement).
    EXPECT_TRUE(b->head(manifest_key).exists)
        << "abandon must NOT writer-delete a live precommit body (delete-after-sealed-decrements)";

    /// (b) a precommit-REMOVAL RootOwnerEvent (old = Precommit binding, new = none) is in the target shard.
    const uint64_t shard = shardOfForTest("part_1", s->poolMeta().root_shards);
    const auto shard_raw = b->get(s->layout().rootShardKey(ns, shard));
    ASSERT_TRUE(shard_raw.has_value());
    const RootShard root = decodeRootShard(shard_raw->bytes);
    const OwnerBinding expected{
        .owner_kind = OwnerKind::Precommit, .ref_name = "part_1",
        .build_id = abandoned_build_id, .manifest_ref = mid.ref};
    bool found_removal = false;
    for (const RootOwnerEvent & e : root.journal)
        if (!e.new_binding && e.old_binding && *e.old_binding == expected)
            found_removal = true;
    EXPECT_TRUE(found_removal)
        << "abandon must append a precommit-removal RootOwnerEvent (old=Precommit, new=none)";
}

/// BUG 2 regression for the never-precommitted path: a manifest that was STAGED but never precommitted is
/// still best-effort writer-deleted by abandon (pre-precommit debris) — only a LIVE precommit body is
/// spared. Confirms the fix narrows the skip to the precommitted manifest exactly.
TEST(CasBuild, AbandonStillDeletesNeverPrecommittedStagedDebris)
{
    auto b = std::make_shared<InMemoryBackend>();
    auto s = openStore(b);
    const RootNamespace ns{"srv1/tbl"};
    auto build = startBuildFor(s, ns, "part_1");

    build->putBlob(idOf("kept"), BlobSource::fromString("kept"));
    /// Two staged manifests: one becomes the precommit, the other is pure pre-precommit debris.
    const ManifestId debris = build->stageManifest({blobManifestEntry("debris.bin", "kept")});
    const ManifestId precommitted = build->stageManifest({blobManifestEntry("data.bin", "kept")});
    build->precommitAdd(ns, "part_1", precommitted);

    build->abandon();

    /// The never-precommitted debris is best-effort deleted; the live precommit body survives.
    EXPECT_FALSE(b->head(s->layout().manifestKey(debris)).exists)
        << "never-precommitted staged debris must still be best-effort deleted by abandon";
    EXPECT_TRUE(b->head(s->layout().manifestKey(precommitted)).exists)
        << "the live precommit body must be spared";
}
