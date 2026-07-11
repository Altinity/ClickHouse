#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasBuild.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasBlobMeta.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasEnvelope.h>
#include <IO/WriteBufferFromString.h>
#include <IO/WriteHelpers.h>
#include <Common/Exception.h>
#include <Common/logger_useful.h>
#include <Common/ProfileEvents.h>
#include <Common/thread_local_rng.h>
#include <base/defines.h>
#include <algorithm>
#include <chrono>

namespace ProfileEvents
{
    extern const Event CasBlobDedupCacheHit;
    extern const Event CasBlobHeadFirst;
    extern const Event CasBlobBodyPutAvoided;
    extern const Event CasBlobCopyForward;
}

namespace DB
{
namespace ErrorCodes
{
    extern const int LOGICAL_ERROR;
    extern const int BAD_ARGUMENTS;
    extern const int FILE_DOESNT_EXIST;
    extern const int NOT_IMPLEMENTED;
    extern const int ABORTED;
    extern const int CORRUPTED_DATA;
    extern const int LIMIT_EXCEEDED;
}
}

namespace DB::Cas
{

namespace
{

/// OQ7 backpressure caps, enforced fail-closed in stageManifest BEFORE the body write returns.
constexpr uint64_t kMaxManifestEntries = 1048576;
constexpr uint64_t kMaxManifestEncodedBytes = 256ULL << 20;        /// 256 MiB
constexpr uint64_t kMaxManifestInlineBytesTotal = 16ULL << 20;     /// 16 MiB
constexpr uint64_t kMaxLargestInlineEntryBytes = 1ULL << 20;       /// 1 MiB

/// Two thread_local_rng draws composed into a UInt128. Used both to mint build ids and to mint, on
/// every upload/re-upload, a FRESH incarnation_tag (W-FRESH-TAG).
UInt128 mintU128()
{
    const UInt64 hi = thread_local_rng();
    const UInt64 lo = thread_local_rng();
    return (static_cast<UInt128>(hi) << 64) | lo;
}

uint64_t nowMs()
{
    return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count());
}

}

/// The POOL-WIDE content-hash convention, with the pool's selected `algo` (P1-T3a, CAS
/// pluggable-blob-hash design §5): `Cas::blobHashHexOneShot` uses the SAME streaming/chunked
/// convention the write path uses for `CityHash128` (`ContentAddressedWriteBuffers`: `getHashHex` ->
/// `BlobId` -> `hexToU128`) and the one-shot `XXH3_128bits` call for `XXH3_128` (defined to agree
/// with its own streaming digest). The core otherwise NEVER re-hashes payloads; copy-forward is the
/// one sanctioned re-verification and MUST use this convention — a one-shot `CityHash128` (as opposed
/// to the chunked convention) diverges for any payload larger than one hash block (found live:
/// 2026-07-03 soak, a false `CORRUPTED_DATA` re-bricked the attach path copy-forward exists to fix).
///
/// Phase 3 T2 (mixed-algo pools): returns the full `BlobRef` pair, at `algo`'s own width (via
/// `codecFor`, `CasBlobRef.h`) — never a bare digest. Exported (declared in `CasBuild.h`) so the
/// wiring's inline-candidate hashing (`ContentAddressedTransaction.cpp`) can mint the SAME way the
/// streaming blob path does, rather than reimplementing the hex round-trip inline.
BlobRef poolContentHash(BlobHashAlgo algo, std::string_view payload)
{
    return BlobRef{algo, codecFor(algo).fromHex(blobHashHexOneShot(algo, payload))};
}

BlobSource BlobSource::fromString(String bytes)
{
    BlobSource source;
    source.size = bytes.size();
    source.write_payload = [b = std::move(bytes)](WriteBuffer & out) { writeString(b, out); };
    return source;
}

Build::Build(StorePtr store_, UInt128 build_id_,
             uint64_t build_seq_, uint64_t epoch_, BuildInfo info_)
    : store(std::move(store_))
    , build_id(build_id_)
    , build_seq(build_seq_)
    , epoch(epoch_)
    , info(std::move(info_))
{
    /// B170: a build began (W-HEARTBEAT durable). build_id/seq/epoch identify it for token-join
    /// attribution against the GC delete rows.
    EventEmitter{*store}.emit([&](CasEvent & e)
    {
        e.type = CasEventType::BuildStart;
        e.ref_name = info.intended_ref.value_or("");
        e.token = u128ToHex(build_id);
        e.outcome = "started";
        e.reason = "startBuild: build in-flight";
        e.detail = {{"build_seq", std::to_string(build_seq)}, {"epoch", std::to_string(epoch)}};
    });
}

Build::~Build()
{
    /// Crash semantics: the Build dtor retires the build_seq so the GC watermark floor (minActive) can
    /// advance even if neither publish nor abandon ran (idempotent — safe if already retired).
    store->retireBuildSeq(build_seq);
}

void Build::requireAlive() const
{
    if (!alive)
        throw Exception(ErrorCodes::LOGICAL_ERROR, "Build has been abandoned; no further operations allowed");
    /// Self-remount (fence-out recovery) supersedes the mount incarnation this build was minted
    /// under; its write fence already interrupted the build mid-flight, so every further step fails
    /// closed and the caller restarts the build under the live epoch.
    if (const uint64_t live = store->liveWriterEpoch(); epoch != live)
        throw Exception(ErrorCodes::ABORTED,
            "Build (writer_epoch {}) belongs to a superseded mount incarnation (live epoch {}) — "
            "the mount was fenced out and self-remounted; restart the build", epoch, live);
}

PutBlobResult Build::putBlob(const BlobRef & ref, BlobSource source)
{
    requireAlive();

    const PoolConfig & cfg = store->poolConfig();
    /// Phase 3 T2/T3: `putBlob` writes the blob identified by `ref` directly -- the caller (the
    /// write-mint site) already produced the full `BlobRef` pair (algo + digest), so there is no hex
    /// round-trip here anymore. `logical_ref` is the blob identity end-to-end from here on: the dedup
    /// cache, the dep map, and every downstream event render key off THIS value directly.
    const BlobRef & logical_ref = ref;

    const String key = store->layout().blobKey(ref);

    /// The source is RE-READABLE (the caller's `write_payload` re-reads a staged temp file, or re-emits a
    /// captured String): it can be invoked MULTIPLE times — the primary streaming PUT plus any INV-1
    /// re-upload — so we never materialize the whole blob into memory here. The byte count is verified
    /// against `source.size` at each streaming write site (via the sink buffer's `count()`), not by a
    /// full pre-materialization, so peak memory is bounded by the write-buffer, not the blob size.

    /// B168 P1/P2: HEAD-before-PUT on a likely dedup hit (cache says present) or a large body (where a
    /// wasted body-PUT that 412s is expensive — and on a store that early-closes a doomed conditional
    /// PUT, the broken-pipe + retry storm of B187). A present HEAD ⇒ admit without streaming the body;
    /// a stale/absent HEAD ⇒ fall through to the normal conditional upload. SAFE by construction: we
    /// always genuinely observe present-at-round before skipping the body, so the cache can never cause
    /// a dangle (a stale hit just HEADs 404 and uploads).
    const bool head_first =
        store->dedupCacheContains(logical_ref)
        || (cfg.dedup_head_first_min_bytes > 0 && source.size >= cfg.dedup_head_first_min_bytes);
    if (head_first)
    {
        ProfileEvents::increment(ProfileEvents::CasBlobHeadFirst);
        const HeadResult hr = store->backend().head(key);
        if (hr.exists)
        {
            ProfileEvents::increment(ProfileEvents::CasBlobBodyPutAvoided);
            if (store->dedupCacheContains(logical_ref))
                ProfileEvents::increment(ProfileEvents::CasBlobDedupCacheHit);
            try
            {
                const uint64_t admitted = observeAndAdmit(ObjectKind::Blob, logical_ref, key, hr);
                store->dedupCacheAdd(logical_ref);
                return PutBlobResult{ref, admitted};
            }
            catch (const Exception & e)
            {
                /// INV-1: HEAD-first path hit a condemned token → re-upload from our OWN source bytes.
                if (e.code() != ErrorCodes::ABORTED)
                    throw;
                /// Fall through to uploadFromSource below.
            }
        }
        /// hr.exists == false OR condemned-ABORTED → fall through to uploadFromSource / fresh upload.
    }

    /// B136 / INV-1: fresh-upload path + condemned-dedup recovery. Try the primary upload via
    /// uploadFromSource (which handles condemned-present via putOverwrite and absent via putIfAbsentStream
    /// without any backend().get). Bounded loop guards against rare concurrent-condemnation churn.
    constexpr int max_attempts = 8;
    for (int attempt = 0; attempt < max_attempts; ++attempt)
    {
        try
        {
            uploadFromSource(ObjectKind::Blob, logical_ref, key, source);
            /// P1: this hash is now known-present — future writers can HEAD-first and skip the body.
            store->dedupCacheAdd(logical_ref);
            return PutBlobResult{ref, source.size};
        }
        catch (const Exception & e)
        {
            /// ABORTED from uploadFromSource is retryable — re-upload by re-streaming from our re-readable
            /// source (bounded). Two cases produce it:
            ///   • a racing writer displaced the condemned token before our putOverwrite landed and
            ///     their fresh incarnation is itself already condemned (observeAndAdmit → ABORTED); or
            ///   • the object was GC-deleted during the post-412 revival re-observe (B190 sibling):
            ///     reviveObserve converts FILE_DOESNT_EXIST → ABORTED so the vanish re-uploads here
            ///     rather than escaping FATAL. Both are rare races covered by the bounded loop.
            if (e.code() != ErrorCodes::ABORTED || attempt + 1 == max_attempts)
                throw;
        }
    }

    /// Unreachable: the loop either returns or rethrows on the final attempt.
    throw Exception(ErrorCodes::LOGICAL_ERROR, "putBlob: exhausted retries for {}", key);
}

bool Build::isCopyForwardableTokenless(const BlobRef & ref) const
{
    auto it = deps.find(ref);
    return it != deps.end() && !it->second.token.has_value();
}

bool Build::depIsTokened(const BlobRef & ref) const
{
    /// Discriminator for B156b: a putBlob'd blob records a TOKENED dep (recreatable by retrying), an
    /// adoptFromTree carry-forward records a TOKENLESS W-EVIDENCE dep (not recreatable — pinned by a
    /// committed source). Returns false when this build has no dep for the ref (the caller decides
    /// the default; not-tokened is the fail-loud, INV-NO-LOSS-safe choice).
    auto it = deps.find(ref);
    return it != deps.end() && it->second.token.has_value();
}

uint64_t Build::observeAndAdmit(ObjectKind kind, const BlobRef & ref, const String & key)
{
    /// EDGE-BEFORE-OBSERVE (spec 2026-07-09-cas-writer-gc-simplification): adopting an EXISTING
    /// incarnation is safe ONLY under this build's durable precommit closure — an adopted blob carries the
    /// ORIGINAL writer's build_id, so the newborn-debris watermark does not cover it. Fresh uploads
    /// pre-precommit stay legal (watermark-protected). See the TLA+ order sabotage (Gate A).
    chassert(precommitted);
    const HeadResult hr = store->backend().head(key);
    if (!hr.exists)
        /// Object absent at observe time. Two contexts call this overload:
        ///   1. adoptTree (fail-closed): a detached/frozen tree that is absent at adopt time is
        ///      NOT a transient race — it was GC-collected. The caller must abort or re-create from
        ///      source (not retry forever). FILE_DOESNT_EXIST is the right outcome for adoptTree.
        ///   2. checkAndResolveDeps gate (retryable): a GC-deleted object at gate time is a race
        ///      under INV-3 — the caller retries the whole operation and re-materializes from source.
        ///      The gate catches FILE_DOESNT_EXIST here and re-throws it as ABORTED (see
        ///      gateObserveAndAdmit below) so the INSERT layer sees the uniform retryable error.
        throw Exception(ErrorCodes::FILE_DOESNT_EXIST,
            "Build::observeAndAdmit: object {} vanished (GC-deleted) before observe; "
            "caller must re-upload/re-materialize from source", key);
    return observeAndAdmit(kind, ref, key, hr);
}

uint64_t Build::observeAndAdmit(ObjectKind kind, const BlobRef & ref, const String & key, const HeadResult & hr)
{
    /// EDGE-BEFORE-OBSERVE (spec 2026-07-09-cas-writer-gc-simplification): adopting an EXISTING
    /// incarnation is safe ONLY under this build's durable precommit closure — an adopted blob carries the
    /// ORIGINAL writer's build_id, so the newborn-debris watermark does not cover it. Fresh uploads
    /// pre-precommit stay legal (watermark-protected). See the TLA+ order sabotage (Gate A).
    chassert(precommitted);
    /// `hr.exists` is guaranteed by the caller (the 3-arg wrapper checked it; the putBlob HEAD-first
    /// path only calls this on a present HEAD). Avoids a redundant second HEAD on the dedup-hit path.
    /// Logical (payload) size = object size minus the pool's fixed blob header. GUARD against
    /// unsigned underflow: a truncated/corrupt object whose size is below the header length must surface
    /// as CORRUPTED_DATA, never wrap to a huge value. Mirrors the GC path's `retiredLogicalSize`.
    const uint64_t header_len = store->poolMeta().blob_header_len;
    if (hr.size < header_len)
        throw Exception(ErrorCodes::CORRUPTED_DATA,
            "Build: {} object {} size {} is below the pool blob header length {}",
            kind == ObjectKind::Blob ? "blob" : "manifest", key, hr.size, header_len);
    const uint64_t logical_size = hr.size - header_len;

    const CasEventObjectKind ev_kind = toEventKind(kind);

    /// Task 3 (spec §meta-protocols v3): the condemned decision is now a per-hash META POINT-READ, not
    /// the writer-side RetireView. `absent` meta means "not condemned" — GC always writes a `Condemned`
    /// meta BEFORE it ever deletes a body, so an absent meta is exactly as live as a `Clean` one.
    /// Phase 3 T3: the meta ops layer is `BlobRef`-keyed directly (derives its codec from `ref.algo`
    /// internally). Every event-log render below uses `blobIdOf(ref)` ("<algoName>:<hex>"), never a
    /// bare hex.
    const auto lm = loadMeta(store->backend(), store->layout(), ref);
    const bool condemned = lm && lm->meta.state == MetaState::Condemned;
    if (condemned)
    {
        /// INV-1 (revival-from-source): the observed token is condemned — we must NOT read the dying
        /// object via backend().get. Throw ABORTED so the caller can re-upload from its OWN source bytes.
        ///   • putBlob (has BlobSource): catches ABORTED, calls uploadFromSource from held bytes.
        ///   • uploadStagedTree / recreateTree (has retained_trees payload): calls uploadFromSource.
        ///   • gate bodyless (no source): propagates ABORTED (retryable; caller retries op).
        EventEmitter{*store}.emit([&](CasEvent & e)
        {
            e.type = CasEventType::BlobReuseResurrect;
            e.object_kind = ev_kind;
            e.object_hash = blobIdOf(ref);
            e.token = hr.token.value;
            e.round = 0;   /// the round is no longer a writer concept (meta point-read replaces RetireView)
            e.outcome = "condemned";
            e.reason = "observed token is condemned (meta point-read); caller must re-upload from source (INV-1)";
        });
        throw Exception(ErrorCodes::ABORTED,
            "Build::observeAndAdmit: condemned token for {} — caller must re-upload from source bytes (INV-1)",
            key);
    }

    /// `!lm`: no meta yet for this hash (a pre-existing blob from before this protocol, or a lost race
    /// with a concurrent fresh-uploader's own meta write). Best-effort create it as Clean so future
    /// point-readers (writers and GC) never have to fall back to a HEAD-only guess. A Conflict here just
    /// means a racing writer already created it — both agree on the same Clean steady state.
    if (!lm)
        putMetaIfAbsent(store->backend(), store->layout(), ref,
            BlobMeta{.state = MetaState::Clean, .condemn_round = 0, .size = logical_size});

    /// Adopt the current incarnation — free, no bytes moved.
    /// B170: reuse ADOPTED an existing incarnation's token as NOT condemned (per the meta point-read).
    /// Was the CAREUSE adopt audit line. Token-join this against a later blob_delete
    /// of the same hash/token to pin a reuse-of-an-object-being-deleted race.
    EventEmitter{*store}.emit([&](CasEvent & e)
    {
        e.type = CasEventType::BlobReuseAdopt;
        e.object_kind = ev_kind;
        e.object_hash = blobIdOf(ref);
        e.token = hr.token.value;
        e.round = 0;   /// the round is no longer a writer concept (meta point-read replaces RetireView)
        e.outcome = "adopt";
        e.reason = "observed token not condemned (meta point-read); adopted the live incarnation (no bytes moved)";
    });
    deps[ref] = DepEntry{kind, hr.token, logical_size};
    return logical_size;
}

void Build::uploadFromSource(ObjectKind kind, const BlobRef & ref, const String & key, const BlobSource & source)
{
    /// INV-1 (revival-from-source): re-upload a condemned or absent object from the writer's OWN
    /// re-readable source — NEVER calls backend().get to read the dying object. W-FRESH-TAG: fresh
    /// incarnation_tag and this build's build_id so the new incarnation is owned by THIS live build
    /// (the B167 fix — the prior resurrect was the lone gap). The payload is STREAMED into the put sink
    /// (`source.write_payload`), never materialized into a full in-memory copy on the common
    /// If-None-Match path; `source.write_payload` is re-invoked on each attempt (it re-reads the staged
    /// temp file), which is exactly what preserves INV-1 across retries.
    const PoolMeta & meta = store->poolMeta();
    const PoolConfig & cfg = store->poolConfig();
    /// Phase 3 T3: `ref` is the full blob identity (algo + digest) end-to-end -- the `.meta` API is
    /// `BlobRef`-keyed directly, and the dep map + every event render below key off `ref` too.

    auto buildHeader = [&]() -> String
    {
        EnvelopeHeader header;
        header.kind = kind;
        header.hash_algo = static_cast<uint8_t>(ref.algo);
        header.domain_id = meta.pool_id;
        header.incarnation_tag = mintU128();
        header.build_id = build_id;
        header.provenance = Provenance{nowMs(), cfg.server_id, /*ch_version*/ 0, info.op};
        if (kind == ObjectKind::Blob)
            header.intended_ref = info.intended_ref;
        /// Both blobs and trees pad to the pool's fixed header length, so every object's payload starts
        /// at a constant offset (a constant-shift locate for blobs; uniform layout for trees).
        header.pad_to_header_len = static_cast<uint32_t>(meta.blob_header_len);
        try
        {
            return encodeEnvelopeHeader(header);
        }
        catch (const Exception & e)
        {
            if (e.code() != ErrorCodes::BAD_ARGUMENTS)
                throw;
            /// intended_ref is diagnostic-only: when it makes the header exceed blob_header_len, drop it.
            header.intended_ref.reset();
            return encodeEnvelopeHeader(header);
        }
    };

    const CasEventObjectKind ev_kind = toEventKind(kind);

    /// Revival-local wrapper for the post-412 re-observe (B190 sibling, INV-3). On the post-412 path a
    /// racing writer is assumed to have (re-)created the object, so we adopt its token via the 3-arg
    /// observeAndAdmit. But the object can be GC-deleted in the window (present at the conditional PUT
    /// → 412, gone at the subsequent HEAD), making the 3-arg overload throw FILE_DOESNT_EXIST. The
    /// caller (putBlob / uploadStagedTree / recreateTree) HOLDS the source bytes, so a vanish here is a
    /// retryable race: convert FILE_DOESNT_EXIST → ABORTED so putBlob's bounded retry loop re-uploads
    /// from those bytes (and tree callers likewise re-create). Without this, FILE_DOESNT_EXIST escaped
    /// putBlob's ABORTED-only catch as a FATAL INSERT failure — the sibling of the gate bug.
    auto reviveObserve = [&](const String & k_)
    {
        try
        {
            observeAndAdmit(kind, ref, k_);
        }
        catch (const Exception & e_)
        {
            if (e_.code() != ErrorCodes::FILE_DOESNT_EXIST)
                throw;
            throw Exception(ErrorCodes::ABORTED,
                "uploadFromSource: object {} vanished (GC-deleted) during revival re-observe; "
                "retry the operation — re-upload from held source bytes (INV-3)", k_);
        }
    };

    auto recordDoneAndEmit = [&](Token tok)
    {
        deps[ref] = DepEntry{kind, tok, source.size};
        EventEmitter{*store}.emit([&](CasEvent & e)
        {
            e.type = CasEventType::BlobPut;
            e.object_kind = ev_kind;
            e.object_hash = blobIdOf(ref);
            e.token = tok.value;
            e.round = 0;   /// the round is no longer a writer concept (meta point-read replaces RetireView)
            e.outcome = "ok";
            e.reason = "uploadFromSource: fresh incarnation streamed from writer's own re-readable source (INV-1)";
            e.detail = {{"size", std::to_string(source.size)}, {"build_id", u128ToHex(build_id)}};
        });
    };

    /// Task 3 (spec §meta-protocols v3) meta write for the FRESH (absent -> present) upload cases: the
    /// body just transitioned via If-None-Match, so the freshness meta is created as Clean.
    /// `putMetaIfAbsent` — a Conflict just means a racing writer already created it (fine; both agree
    /// on the same Clean steady state).
    auto writeFreshMetaClean = [&]()
    {
        putMetaIfAbsent(store->backend(), store->layout(), ref,
            BlobMeta{.state = MetaState::Clean, .condemn_round = 0, .size = source.size});
    };

    /// Meta write for the RESURRECT (condemned-displacement) case: flip the now-stale Condemned meta
    /// back to Clean now that a live incarnation has displaced the condemned body. `lm_before` is the
    /// point-read taken just before the condemned decision — its etag is the CAS precondition. Bounded
    /// retry on a racing meta writer (another resurrect, or GC re-condemning); an absent meta (raced
    /// away between the point-read and here) falls back to putMetaIfAbsent. Best-effort: the body's
    /// incarnation_tag + exact-token delete are already the safety core — this meta write is only a
    /// freshness marker for the NEXT point-reader.
    auto writeResurrectMetaClean = [&](std::optional<LoadedMeta> lm_before)
    {
        const BlobMeta clean{.state = MetaState::Clean, .condemn_round = 0, .size = source.size};
        constexpr int max_meta_attempts = 8;
        for (int attempt = 0; attempt < max_meta_attempts; ++attempt)
        {
            const CasResult res = lm_before
                ? casMeta(store->backend(), store->layout(), ref, lm_before->etag, clean)
                : putMetaIfAbsent(store->backend(), store->layout(), ref, clean);
            if (res.outcome == CasOutcome::Committed)
                return;
            lm_before = loadMeta(store->backend(), store->layout(), ref);
        }
        /// Exhausted retries: leave the meta as-is — best-effort (a later writer/GC pass reconciles it).
    };

    /// Stream header + payload into a fresh putIfAbsentStream sink WITHOUT materializing the whole blob.
    /// `source.write_payload` re-reads the staged temp file (INV-1: the writer's own source, never the
    /// dying object). The payload byte count is verified against `source.size` via the sink buffer's
    /// `count()` (total bytes written so far) — the streaming equivalent of the old pre-materialized
    /// size check, with no full in-memory copy. A mismatch is a LOGICAL_ERROR (a buggy/racing source).
    auto streamIfAbsent = [&]() -> PutResult
    {
        /// S3-native staging promote (spec 2026-07-11-cas-s3-native-staging §5/§8): when the source
        /// carries a server-side-copy descriptor, the write-once CREATE primitive is a conditional
        /// server-side copy of the staging object to `key` (`If-None-Match:*`) instead of a client-side
        /// streaming PUT. Same write-once contract (`Done` + dest-ETag token on created,
        /// `PreconditionFailed` when `key` already exists), so every gate branch below is UNCHANGED. The
        /// staging object IS the promote source — no envelope is streamed here. The LOCAL path (no
        /// descriptor) is byte-for-byte unchanged.
        if (source.server_side_copy_from)
            return store->backend().promoteStaged(*source.server_side_copy_from, key);

        /// B171: no owner metadata (protection is the precommit edge — reachability, not `cas_owner`).
        WriteSinkPtr sink = store->backend().putIfAbsentStream(key);
        WriteBuffer & out = sink->buffer();
        writeString(buildHeader(), out);
        const size_t before = out.count();
        source.write_payload(out);
        const size_t written = out.count() - before;
        if (written != source.size)
            throw Exception(ErrorCodes::LOGICAL_ERROR,
                "uploadFromSource: source streamed {} bytes, declared {}", written, source.size);
        return sink->finalize();
    };

    /// Phase 1: try If-None-Match upload (object absent or race with another writer).
    {
        const PutResult res = streamIfAbsent();
        if (res.outcome == PutOutcome::Done)
        {
            recordDoneAndEmit(res.token);
            writeFreshMetaClean();
            return;
        }
    }

    /// PreconditionFailed: an incarnation exists. HEAD it to check whether it is condemned or live.
    /// We do NOT read the body (no backend().get) — we only need the token to decide:
    ///   • live (not condemned)  → adopt; this is the standard dedup case.
    ///   • condemned             → displace via putOverwrite(If-Match: current_token) by re-reading our
    ///                            own source, so we never read the dying object. This is the
    ///                            equivalent of the old `resurrect` minus the GET.
    const HeadResult hr = store->backend().head(key);
    if (!hr.exists)
    {
        /// Object vanished between putIfAbsentStream (412d) and our HEAD — a concurrent GC delete.
        /// The object is now absent; re-stream from our re-readable source (bounded by caller).
        const PutResult res2 = streamIfAbsent();
        if (res2.outcome == PutOutcome::Done)
        {
            recordDoneAndEmit(res2.token);
            writeFreshMetaClean();
            return;
        }
        /// Still 412 after the vanish-and-retry: a racing writer re-created it. Adopt their token.
        /// reviveObserve converts FILE_DOESNT_EXIST (deleted again in the window) → ABORTED (retryable).
        reviveObserve(key);
        return;
    }

    /// Task 3 (spec §meta-protocols v3): the condemned decision is now a per-hash META POINT-READ, not
    /// the writer-side RetireView. `lm` (and its etag) is reused below to flip a condemned meta back to
    /// Clean once the resurrect displacement lands.
    const auto lm = loadMeta(store->backend(), store->layout(), ref);
    const bool condemned = lm && lm->meta.state == MetaState::Condemned;
    if (!condemned)
    {
        /// Live (not condemned): adopt the current incarnation — free, no bytes moved.
        observeAndAdmit(kind, ref, key, hr);
        return;
    }

    /// Condemned: displace the condemned incarnation with our fresh source.
    if (source.server_side_copy_from)
    {
        /// S3-native staging RESURRECT (spec §5/§9, INV-NO-RETURN fix 2026-07-11): re-establish a fresh
        /// incarnation by re-uploading OUR OWN staging PAYLOAD under a FRESHLY-tagged envelope header —
        /// NEVER a read/copy of the condemned `key` (`feedback_ca_resurrect_invariant`). We reach here
        /// ONLY after the per-hash meta point-read observed `Condemned` just above, so this overwrites a
        /// condemned body, never a live blob (INV: never overwrite a live blob, spec §9).
        ///
        /// CRITICAL — a VERBATIM server-side copy of the create-time staging object would reproduce the
        /// condemned incarnation's exact bytes ⇒ identical ETag ⇒ the queued exact-token delete of the
        /// condemned incarnation would kill the live resurrection (data loss). `buildHeader` mints a
        /// FRESH `incarnation_tag` distinct from the staging header's tag, so the resurrected body (and
        /// hence its ETag) differs from the condemned incarnation regardless of edge-before-observe. The
        /// backend reads the payload from the staging object skipping its own `blob_header_len` envelope
        /// header and prepends this fresh header.
        const String fresh_header = buildHeader();
        const Token tok = store->backend().resurrectStaged(
            *source.server_side_copy_from, key, fresh_header, meta.blob_header_len);
        recordDoneAndEmit(tok);
        writeResurrectMetaClean(lm);
        return;
    }

    /// CRITICAL: we re-read the writer's OWN source (NOT backend().get) — no GET of the dying object.
    /// W-FRESH-TAG: a fresh incarnation_tag minted inside buildHeader() ensures INV-NO-RETURN.
    /// putOverwrite is whole-body only (no streaming variant), so this rare condemned-displacement path
    /// materializes the header+payload on-demand by re-invoking `source.write_payload` (re-reading the
    /// staged temp file). This is the only path that holds a full in-memory copy, and only when a
    /// condemned incarnation must actually be displaced — not the common fresh-upload path.
    String overwrite_body;
    {
        WriteBufferFromString wb{overwrite_body};
        writeString(buildHeader(), wb);
        const size_t before = wb.count();
        source.write_payload(wb);
        wb.finalize();
        const size_t written = wb.count() - before;
        if (written != source.size)
            throw Exception(ErrorCodes::LOGICAL_ERROR,
                "uploadFromSource: source wrote {} bytes for overwrite, declared {}", written, source.size);
    }
    const PutResult overwrite_res = store->backend().putOverwrite(key, overwrite_body, hr.token);
    if (overwrite_res.outcome == PutOutcome::Done)
    {
        recordDoneAndEmit(overwrite_res.token);
        writeResurrectMetaClean(lm);
        return;
    }
    /// PreconditionFailed from putOverwrite: either a racing writer displaced the condemned token
    /// before us (their fresh token doesn't match hr.token), OR GC deleted the object between our
    /// HEAD and the putOverwrite (absent → If-Match always fails). HEAD again to disambiguate.
    {
        const HeadResult hr2 = store->backend().head(key);
        if (!hr2.exists)
        {
            /// Object deleted in the window — try fresh If-None-Match upload (it is now absent).
            const PutResult res3 = streamIfAbsent();
            if (res3.outcome == PutOutcome::Done)
            {
                recordDoneAndEmit(res3.token);
                writeFreshMetaClean();
                return;
            }
            /// Still 412 — a racing writer re-created it. Observe their token.
            /// reviveObserve converts FILE_DOESNT_EXIST (deleted again in the window) → ABORTED (retryable).
            reviveObserve(key);
            return;
        }
        /// Present with a different token — a racing writer displaced the condemned incarnation.
        /// Adopt their token (or throw ABORTED if it too is condemned — bounded by caller loop).
        observeAndAdmit(kind, ref, key, hr2);
    }
}

Token Build::copyForwardFromCondemned(const BlobRef & ref, const String & key, HeadResult hr)
{
    /// See the header comment: the narrow INV-1 exception for tokenless committed-manifest evidence.
    /// Blob-only: adoptEvidence never records tree deps (trees are recreatable from retained payloads).
    const PoolMeta & meta = store->poolMeta();
    const PoolConfig & cfg = store->poolConfig();
    /// Phase 3 T3: `ref` is the full blob identity (the caller — `promote`'s non-tokened-leaf
    /// revalidation — passes `entry.ref` directly), so every meta-op below is `BlobRef`-keyed
    /// directly, and every hex render routes through `blobIdOf(ref)`.

    constexpr int max_attempts = 8;
    for (int attempt = 0; attempt < max_attempts; ++attempt)
    {
        /// 1. Read the dying object IN FULL. Absent ⇒ the delete won; with no source this is
        ///    fail-closed ABORTED (never putIfAbsent after a lost race — that would revive deleted
        ///    data). The GET is atomic per object: we see one whole incarnation or nothing.
        const auto got = store->backend().get(key);
        if (!got)
            throw Exception(ErrorCodes::ABORTED,
                "copyForwardFromCondemned: object {} vanished before the copy-forward read — "
                "the exact-token delete won; failing closed (retry the operation)", key);
        if (got->token != hr.token)
        {
            /// The incarnation moved under us. Task 4 (spec §meta-protocols v3): condemnation is now a
            /// per-hash META POINT-READ, not a per-token retire-view fact — a positively-Clean meta means
            /// someone else's copy-forward/resurrect already reconciled this hash (adopt their token, no
            /// further work); anything else (Condemned, or absent — never observed as clean) falls through
            /// to re-verify and re-displace THIS incarnation. Safe either way: a spurious re-displacement
            /// just costs one more GET+PUT and still lands on a verified fresh incarnation.
            const auto lm_drift = loadMeta(store->backend(), store->layout(), ref);
            if (lm_drift && lm_drift->meta.state == MetaState::Clean)
                return got->token;
            hr.token = got->token;
        }

        /// 2. Verify fail-closed before re-publishing a single byte: the envelope must decode (header
        ///    CRC), declare a Blob of this pool, and the PAYLOAD must re-hash to the content key.
        const EnvelopeHeader header_in = decodeEnvelopeHeader(got->bytes, got->bytes.size(), ObjectKind::Blob);
        const std::string_view payload{got->bytes.data() + header_in.header_len,
                                       got->bytes.size() - header_in.header_len};
        const BlobRef payload_ref = poolContentHash(ref.algo, payload);
        if (payload_ref != ref)
            throw Exception(ErrorCodes::CORRUPTED_DATA,
                "copyForwardFromCondemned: object {} payload does not verify against its content key "
                "(payload hash {}, expected {}) — refusing to copy forward",
                key, blobIdOf(payload_ref), blobIdOf(ref));

        /// 3. Re-wrap under a fresh envelope: fresh incarnation_tag + THIS build's build_id
        ///    (W-FRESH-TAG, B167 — the new incarnation is owned by this live build).
        EnvelopeHeader header;
        header.kind = ObjectKind::Blob;
        header.hash_algo = static_cast<uint8_t>(ref.algo);
        header.domain_id = meta.pool_id;
        header.incarnation_tag = mintU128();
        header.build_id = build_id;
        header.provenance = Provenance{nowMs(), cfg.server_id, /*ch_version*/ 0, info.op};
        header.intended_ref = info.intended_ref;
        header.pad_to_header_len = static_cast<uint32_t>(meta.blob_header_len);
        String bytes_out;
        try
        {
            bytes_out = encodeEnvelopeHeader(header);
        }
        catch (const Exception & e)
        {
            if (e.code() != ErrorCodes::BAD_ARGUMENTS)
                throw;
            /// intended_ref is diagnostic-only: when it makes the header exceed blob_header_len, drop it.
            header.intended_ref.reset();
            bytes_out = encodeEnvelopeHeader(header);
        }
        bytes_out.append(payload);

        /// 4. Displace EXACTLY the incarnation we read and verified — token-conditional, so a
        ///    concurrent exact-token delete or a racing recreate surfaces as PreconditionFailed,
        ///    never as a blind resurrection.
        const PutResult res = store->backend().putOverwrite(key, bytes_out, hr.token);
        if (res.outcome == PutOutcome::Done)
        {
            ProfileEvents::increment(ProfileEvents::CasBlobCopyForward);
            EventEmitter{*store}.emit([&](CasEvent & e)
            {
                e.type = CasEventType::BlobCopyForward;
                e.object_kind = CasEventObjectKind::Blob;
                e.object_hash = blobIdOf(ref);
                e.token = res.token.value;
                e.round = 0;   /// the round is no longer a writer concept (meta point-read replaces RetireView)
                e.outcome = "ok";
                e.reason = "verified copy-forward of a condemned incarnation still referenced by a "
                           "committed source manifest (tokenless evidence dep; INV-1 exception)";
                e.detail = {{"displaced_token", hr.token.value},
                            {"size", std::to_string(payload.size())},
                            {"build_id", u128ToHex(build_id)}};
            });

            /// Task 4 (spec §meta-protocols v3): the displacement just landed a fresh, verified
            /// incarnation — flip the (now-stale) Condemned meta back to Clean so the NEXT point-reader
            /// (writer dedup-hit or another promote revalidation) never has to fall back to a HEAD-only
            /// guess. Best-effort bounded retry on a racing meta writer (another copy-forward/resurrect,
            /// or GC re-condemning): a Conflict just means someone else already reconciled it to the same
            /// steady state; putMetaIfAbsent covers the (rare) case the meta was never created at all.
            {
                auto lm_out = loadMeta(store->backend(), store->layout(), ref);
                const BlobMeta clean{.state = MetaState::Clean, .condemn_round = 0, .size = payload.size()};
                constexpr int max_meta_attempts = 8;
                for (int meta_attempt = 0; meta_attempt < max_meta_attempts; ++meta_attempt)
                {
                    const CasResult meta_res = lm_out
                        ? casMeta(store->backend(), store->layout(), ref, lm_out->etag, clean)
                        : putMetaIfAbsent(store->backend(), store->layout(), ref, clean);
                    if (meta_res.outcome == CasOutcome::Committed)
                        break;
                    lm_out = loadMeta(store->backend(), store->layout(), ref);
                    /// Exhausted retries: leave the meta as-is — best-effort (a later writer/GC pass
                    /// reconciles it); the body-side displacement above is already durable and verified.
                }
            }
            return res.token;
        }

        /// PreconditionFailed: re-observe and re-decide (absent ⇒ fail closed at the top of the loop;
        /// clean token ⇒ adopt; condemned again ⇒ another bounded attempt).
        const HeadResult hr2 = store->backend().head(key);
        if (!hr2.exists)
            throw Exception(ErrorCodes::ABORTED,
                "copyForwardFromCondemned: object {} deleted between the verified read and the "
                "token-conditional overwrite — failing closed (retry the operation)", key);
        hr = hr2;
    }
    throw Exception(ErrorCodes::ABORTED,
        "copyForwardFromCondemned: exhausted {} attempts for {} — every observed incarnation was "
        "re-condemned under us; failing closed (retry the operation)", max_attempts, key);
}

void Build::adoptEvidence(const ManifestEntry & entry)
{
    requireAlive();

    /// W-EVIDENCE: record a TOKENLESS dependency — liveness evidence is the live source manifest, not a
    /// token. Inline entries reference no standalone object, so they record nothing. NO backend call
    /// (no HEAD, no GET, no PUT) — the caller already holds the resolved entry. Part manifests have only
    /// Inline / Blob placements (no Subtree): only blobs are content-addressed.
    if (entry.placement == EntryPlacement::Blob)
    {
        /// Phase 3 T2: carry `entry.ref` WHOLE (the pair, never re-derived) — this is what makes a
        /// mixed-algo manifest's entries each dep-track under their OWN algo.
        deps[entry.ref] = DepEntry{ObjectKind::Blob, std::nullopt, entry.blob_size};
    }
}

void Build::recordPendingBlobDep(const BlobRef & ref, uint64_t size)
{
    requireAlive();
    deps[ref] = DepEntry{ObjectKind::Blob, std::nullopt, size};
}

RootNamespace Build::manifestNamespace() const
{
    /// The wiring sets the owning namespace EXPLICITLY (BuildInfo::intended_namespace). This is the
    /// authoritative source: a ref can itself contain `/` (the `detached/<part>` fold, B181), so we
    /// must NOT recover the namespace by splitting intended_ref on the last `/` — that would yield a
    /// spurious `<ns>/detached` namespace and the precommit namespace-match check would throw.
    if (info.intended_namespace)
        return *info.intended_namespace;

    /// Fallback (Core tests that set only the diagnostic intended_ref, where the ref has no `/`): the
    /// owning namespace is intended_ref minus its last `/`-segment (the ref name). A CA namespace itself
    /// contains `/` (e.g. "srv1/<uuid>@cas@"), so split on the LAST slash only.
    const String intended = info.intended_ref.value_or("");
    const size_t slash = intended.find_last_of('/');
    if (slash == String::npos || slash == 0)
        throw Exception(ErrorCodes::LOGICAL_ERROR,
            "stageManifest: intended_ref '{}' has no namespace/ref split", intended);
    return RootNamespace{intended.substr(0, slash)};
}

ManifestId Build::stageManifest(std::vector<ManifestEntry> entries)
{
    requireAlive();

    /// Fail-closed caps (OQ7) — checked BEFORE the body write so no owner transition can ever name a
    /// manifest that breaches a cap. Inline payload is read on every part-open and every owner
    /// transition, so cap the total, not only per-entry.
    if (entries.size() > kMaxManifestEntries)
        throw Exception(ErrorCodes::LIMIT_EXCEEDED,
            "stageManifest: {} entries exceeds cap {}", entries.size(), kMaxManifestEntries);
    uint64_t inline_total = 0;
    for (const ManifestEntry & e : entries)
    {
        if (e.placement == EntryPlacement::Inline)
        {
            if (e.inline_bytes.size() > kMaxLargestInlineEntryBytes)
                throw Exception(ErrorCodes::LIMIT_EXCEEDED,
                    "stageManifest: inline entry '{}' of {} bytes exceeds cap {}",
                    e.path, e.inline_bytes.size(), kMaxLargestInlineEntryBytes);
            inline_total += e.inline_bytes.size();
        }
    }
    if (inline_total > kMaxManifestInlineBytesTotal)
        throw Exception(ErrorCodes::LIMIT_EXCEEDED,
            "stageManifest: total inline {} bytes exceeds cap {}", inline_total, kMaxManifestInlineBytesTotal);

    /// Mint the identity. `epoch` is the Store's durable writer_epoch; `build_seq` is monotone inside
    /// that epoch; `manifest_ordinal` is monotone inside this Build. Together with the owning namespace
    /// this gives NoManifestIdReuse by construction, with no random manifest instance id.
    if (next_manifest_ordinal > kMaxManifestOrdinal)
        throw Exception(ErrorCodes::LIMIT_EXCEEDED,
            "stageManifest: manifest ordinal cap {} exceeded for build_seq {}", kMaxManifestOrdinal, build_seq);
    const ManifestRef ref{epoch, build_seq, next_manifest_ordinal++};

    /// Build the body. payload_digest is integrity/debug only — never a key, never dedup, never
    /// in-degree. The body repeats its own ref + namespace for fail-closed RefMatchesBody /
    /// ManifestNamespaceMatches at read/fold/promote time.
    const RootNamespace owning_ns = manifestNamespace();
    PartManifest body;
    body.ref = ref;
    body.root_namespace_id = owning_ns;
    body.entries = std::move(entries);
    body.payload_digest = computePayloadDigest(body);
    const String encoded = encodePartManifest(body);
    if (encoded.size() > kMaxManifestEncodedBytes)
        throw Exception(ErrorCodes::LIMIT_EXCEEDED,
            "stageManifest: encoded manifest {} bytes exceeds cap {}", encoded.size(), kMaxManifestEncodedBytes);

    const ManifestId id{owning_ns, ref};
    const String key = store->layout().manifestKey(id);

    /// Stream-write the body — NO preliminary HEAD (the instance id is random). A PreconditionFailed
    /// would mean a 128-bit collision: fail closed before any root transition becomes visible.
    WriteSinkPtr sink = store->backend().putIfAbsentStream(key);
    writeString(encoded, sink->buffer());
    const PutResult res = sink->finalize();
    if (res.outcome != PutOutcome::Done)
        throw Exception(ErrorCodes::LOGICAL_ERROR,
            "stageManifest: manifest ordinal collision at {} (PreconditionFailed) — failing closed", key);

    EventEmitter{*store}.emit([&](CasEvent & e)
    {
        e.type = CasEventType::ManifestPut;
        e.namespace_ = owning_ns.string();
        e.object_kind = CasEventObjectKind::Manifest;
        e.object_hash = manifestRefDebugString(id.ref);
        e.token = res.token.value;
        e.reason = "stageManifest: part-manifest body written";
    });

    staged_manifests.push_back(id);
    return id;
}

/// `Layout::blobKey(const BlobRef &)`/`blobMetaKey(const BlobRef &)` (declared in `CasLayout.h`) are
/// defined HERE, not inline in that header, because `CasBlobDigest.h` (which `CasBlobRef.h` includes)
/// already includes `CasPoolMeta.h`, which includes `CasLayout.h` back -- including `CasBlobRef.h` from
/// `CasLayout.h` would cycle. This translation unit already has both complete types via `CasBuild.h` ->
/// `CasStore.h` -> `CasBlobRef.h` (mirrors the old `objectKey` cyclic-include workaround, which this
/// task's per-entry `BlobRef` addressing makes unnecessary — deleted, `Blob` was its only case anyway).
String Layout::blobKey(const BlobRef & ref) const
{
    return shardedKey("blobs/" + String(blobHashAlgoName(ref.algo)), blobHexOf(ref));
}

String Layout::blobMetaKey(const BlobRef & ref) const
{
    return blobKey(ref) + ".meta";
}

std::optional<BlobRef> Layout::parseBlobKey(std::string_view key) const
{
    std::string_view rest = key;
    static constexpr std::string_view kMetaSuffix = ".meta";
    if (rest.size() >= kMetaSuffix.size() && rest.substr(rest.size() - kMetaSuffix.size()) == kMetaSuffix)
        rest.remove_suffix(kMetaSuffix.size());

    const String blobs_root = blobsPrefix();   /// "<prefix>/blobs/"
    if (rest.size() <= blobs_root.size() || rest.substr(0, blobs_root.size()) != blobs_root)
        return std::nullopt;
    rest.remove_prefix(blobs_root.size());

    const size_t algo_sep = rest.find('/');
    if (algo_sep == std::string_view::npos)
        return std::nullopt;
    const std::string_view algo_name = rest.substr(0, algo_sep);
    rest.remove_prefix(algo_sep + 1);

    const size_t shard_sep = rest.find('/');
    if (shard_sep == std::string_view::npos)
        return std::nullopt;
    const std::string_view shard = rest.substr(0, shard_sep);
    const std::string_view hex = rest.substr(shard_sep + 1);
    if (shard.size() != 2 || hex.size() < 2 || shard != hex.substr(0, 2))
        return std::nullopt;   /// malformed shard/hex shape -- not ours

    /// `<algoName>` -> `BlobHashAlgo`: the small enum-value set makes a linear scan against
    /// `blobHashAlgoName` (the ONE name authority) cheaper and safer than a second name table that
    /// could drift from it.
    std::optional<BlobHashAlgo> algo;
    for (BlobHashAlgo candidate : {BlobHashAlgo::CityHash128, BlobHashAlgo::XXH3_128, BlobHashAlgo::Sha256})
        if (algo_name == blobHashAlgoName(candidate))
        {
            algo = candidate;
            break;
        }
    if (!algo)
        return std::nullopt;   /// unknown/foreign algo segment -- debris, not ours

    if (hex.size() != 2 * blobHashLenFor(*algo))
        return std::nullopt;   /// a KNOWN algo but the wrong-width hex -- not ours either

    try
    {
        return BlobRef{*algo, codecFor(*algo).fromHex(String(hex))};
    }
    catch (const Exception &)
    {
        return std::nullopt;   /// non-hex characters -- malformed, not ours
    }
}

void Build::precommitAdd(const RootNamespace & target_ns, const String & final_ref_name, const ManifestId & id)
{
    requireAlive();

    /// ManifestNamespaceMatches at the source: the precommit's manifest must belong to the target
    /// namespace (its key is built from id.root_namespace). A cross-namespace precommit is a bug.
    if (id.root_namespace != target_ns)
        throw Exception(ErrorCodes::LOGICAL_ERROR,
            "precommitAdd: manifest namespace '{}' != target namespace '{}'",
            id.root_namespace.string(), target_ns.string());

    /// ONE CAS on the TARGET shard (shardOf(final_ref_name)): append a create-precommit RootOwnerEvent
    /// {old=none, new={Precommit, final_ref_name, build_id, id.ref}} to the single ordered journal. No body
    /// HEAD — a missing body is a legal fail-closed, non-activating intent (spec §Precommit Add).
    /// Task 2: pass the build's own (writer_epoch, build_seq) as the birth incarnation. On the
    /// create-if-absent path (shard doesn't exist yet) mutateShard stamps root.incarnation before
    /// the journal append; on subsequent calls the stamp is skipped (shard already present).
    /// Task 5: lazily read the current GC round — only on the NEWBORN create-if-absent branch.
    /// On the common existing-shard path `mutateShard` never invokes the provider, so ZERO extra
    /// S3 round-trips are incurred. `currentGcRound` does a `gc/state` GET; hoisting it here (eager)
    /// would waste one GET per publish on every existing shard, which matters for S3 budget.
    /// `birth_incarnation` stays eager (reads only Build members, no S3 — do not change).
    store->mutateShard(target_ns, store->shardOf(final_ref_name), MutationScope::ref(final_ref_name), [&](RootShard & root)
    {
        root.journal.push_back(RootOwnerEvent{
            .transition_version = root.shard_version + 1,
            .old_binding = std::nullopt,
            .new_binding = OwnerBinding{
                .owner_kind = OwnerKind::Precommit,
                .ref_name = final_ref_name,
                .build_id = build_id,
                .manifest_ref = id.ref}});
    }, nullptr, RootMutationOrigin::Writer, RootMutationKind::Precommit,
    ShardIncarnation{.writer_epoch = epoch, .build_sequence = build_seq},
    [this] { return store->currentGcRound(); });

    precommit_target_ns = target_ns;
    precommit_final_ref = final_ref_name;
    precommit_manifest = id.ref;
    precommitted = true;

    EventEmitter{*store}.emit([&](CasEvent & e)
    {
        e.type = CasEventType::Precommit;
        e.namespace_ = target_ns.string();
        e.ref_name = final_ref_name;
        e.token = u128ToHex(build_id);
        e.outcome = "ok";
        e.reason = "precommitAdd: build-intent owner add in the target shard (owner = precommit(build_id))";
        e.detail = {{"build_seq", std::to_string(build_seq)}};
    });
}

void Build::promote(const RootNamespace & target_ns, const String & final_ref_name, UInt128 promote_build_id, const ManifestId & id)
{
    requireAlive();

    if (id.root_namespace != target_ns)
        throw Exception(ErrorCodes::LOGICAL_ERROR,
            "promote: manifest namespace '{}' != target namespace '{}'",
            id.root_namespace.string(), target_ns.string());

    /// Read + validate the manifest body ONCE (O(manifest entries), one streaming read). Absent or
    /// invalid ⇒ fail closed: a committed ref must never name a missing/mismatched manifest.
    const String manifest_key = store->layout().manifestKey(id);
    const auto body_got = store->backend().get(manifest_key);
    if (!body_got)
        throw Exception(ErrorCodes::ABORTED,
            "promote: manifest body absent at {} — failing closed (retry with a fresh ManifestId)", manifest_key);
    const PartManifest body = decodePartManifest(body_got->bytes);
    if (!refMatchesBody(id.ref, body))
        throw Exception(ErrorCodes::ABORTED, "promote: RefMatchesBody failed for {}", manifest_key);
    if (!manifestNamespaceMatches(target_ns, body))
        throw Exception(ErrorCodes::ABORTED, "promote: ManifestNamespaceMatches failed for {}", manifest_key);

    /// D3 (spec 2026-07-09-cas-writer-gc-simplification): the copy-forward pre-pass is removed — the
    /// in-closure blob revalidation below is now the SINGLE copy-forward site. Trade-off: the rare
    /// condemned-tokenless copy-forward (a GET+PUT) now runs inside the shard CAS closure, briefly
    /// blocking the flat-combining queue; it is idempotent under CAS retry (a re-run sees its own fresh
    /// token). Accepted — Phase B replaces the heavy body-dance with a meta CAS anyway.

    /// The OwnerBinding that the create-precommit event installed (precommitAdd). The promote is a pure
    /// owner MOVE off EXACTLY this binding (spec §Promote Precommit step 5; TLA+ `WPromote` old-binding).
    const OwnerBinding expected_precommit{
        .owner_kind = OwnerKind::Precommit, .ref_name = final_ref_name,
        .build_id = promote_build_id, .manifest_ref = id.ref};

    store->mutateShard(target_ns, store->shardOf(final_ref_name), MutationScope::ref(final_ref_name), [&](RootShard & root)
    {
        /// NO writer-side view refresh here (spec 2026-07-09-cas-writer-gc-simplification D5, TLA+
        /// Gate A): promote-time view freshness is not load-bearing — tokened leaves are edge-protected
        /// (EDGE-BEFORE-OBSERVE) and the tokenless K3 gate below reads the live view, which the floor
        /// guarantees contains every graduated entry (in EVERY view >= condemn round + 1). The shard's
        /// fence_round itself (the newborn birth floor, THM-NO-RETURN) and all GC-side uses stay.

        /// BUG 1 / TLA+ `WPromote` guard (`owner[m] = bld`): a promote is a PURE owner MOVE that emits NO
        /// blob delta (Δ=0) — it restores no blob in-degree. It is therefore only sound when the precommit
        /// is STILL the live owner of the ref: if an abandon or GC reclaim already appended a REMOVAL of
        /// the precommit binding, the blobs' in-degree was already decremented and a Δ=0 move would
        /// re-publish a committed ref over to-be-deleted blobs ⇒ a dangling committed manifest
        /// (INV_NO_DANGLE).
        ///
        /// The ref's owner state is the model's `owner[m]`; the converged model keeps a precommit owner
        /// ONLY in the root journal (precommitAdd appends the create-precommit event; it never touches
        /// `root.refs`). A removal of a precommit is a `new_binding = none` event whose `old_binding`
        /// names EXACTLY that precommit binding (the encoding shared by `Build::abandon`,
        /// `Store::dropRef`, and `Gc::reclaimAbandonedPrecommit`). We replay the shard journal exactly as
        /// the GC fold dispatches it — `old_binding` removes a live binding, `new_binding` installs one —
        /// to see whether the precommit binding is removed. The create-precommit `new_binding` may already
        /// be TRIMMED below the sealed fold cursor once GC has activated it (a trimmed-but-still-live
        /// precommit); a removal event, by contrast, is what makes the precommit no longer the owner. So
        /// the precommit is the live owner iff replaying the journal does NOT leave the binding removed:
        /// either it is present in the (untrimmed) live set, or it was trimmed away and no later removal
        /// for it appears. Any removal of EXACTLY this precommit binding ⇒ fail closed (ABORTED): the
        /// build must restart.
        std::vector<OwnerBinding> live;
        for (const RootOwnerEvent & e : root.journal)
        {
            if (e.old_binding)
                std::erase(live, *e.old_binding);
            if (e.new_binding)
                live.push_back(*e.new_binding);
        }
        const bool present_in_live = std::find(live.begin(), live.end(), expected_precommit) != live.end();
        /// A removal of EXACTLY this precommit binding (old = precommit, new = none) anywhere in the
        /// (untrimmed) journal means it stopped being the owner; only a later re-add (present_in_live)
        /// restores it.
        bool seen_removal = false;
        for (const RootOwnerEvent & e : root.journal)
            if (!e.new_binding && e.old_binding && *e.old_binding == expected_precommit)
                seen_removal = true;
        if (seen_removal && !present_in_live)
            throw Exception(ErrorCodes::ABORTED,
                "promote: precommit owner binding for ref '{}' (build {}) was removed (abandon or GC "
                "reclaim) and is no longer the live owner — failing closed; the build must restart "
                "(WPromote owner==bld)",
                final_ref_name, u128ToHex(promote_build_id));

        /// Blob-leaf revalidation (spec 2026-07-09-cas-writer-gc-simplification, Phase A): TOKENED leaves
        /// are edge-protected — EDGE-BEFORE-OBSERVE: the precommit closure was durable BEFORE putBlob
        /// observed them, so a condemnation in the putBlob→promote window cannot graduate (the next fold
        /// sees the edge, d >= 1, spared), and putBlob's gate already validated them against the installed
        /// view under that edge. They are NOT re-checked here. Every NON-tokened leaf (a tokenless
        /// W-EVIDENCE adopt — observation-free by design, B188 — or a no-dep staging bug) gets the single
        /// mandatory presence observation: absent => fail closed; condemned-present + copy-forwardable =>
        /// verified copy-forward (the INV-1 exception); condemned + no-dep => fail closed.
        for (const ManifestEntry & e : body.entries)
        {
            if (e.placement != EntryPlacement::Blob)
                continue;
            if (depIsTokened(e.ref))
                continue;   /// edge-protected (EDGE-BEFORE-OBSERVE); putBlob validated under the durable edge
            const String blob_key = store->layout().blobKey(e.ref);
            constexpr int max_reval_attempts = 8;
            bool validated = false;
            for (int attempt = 0; attempt < max_reval_attempts; ++attempt)
            {
                const HeadResult hr = store->backend().head(blob_key);
                if (!hr.exists)
                    throw Exception(ErrorCodes::ABORTED,
                        "promote: blob {} absent at commit revalidation — failing closed", blob_key);
                /// Task 4 (spec §meta-protocols v3): the condemned decision is now a per-hash META
                /// POINT-READ, not the writer-side RetireView — mirrors Task 3's putBlob/uploadFromSource
                /// re-sourcing. An absent meta reads as "not condemned" (same convention as the writer gate).
                const auto lm = loadMeta(store->backend(), store->layout(), e.ref);
                const bool condemned = lm && lm->meta.state == MetaState::Condemned;
                if (condemned)
                {
                    if (!isCopyForwardableTokenless(e.ref))
                        throw Exception(ErrorCodes::ABORTED,
                            "promote: blob {} condemned at commit revalidation — failing closed (INV-1)", blob_key);
                    copyForwardFromCondemned(e.ref, blob_key, hr);
                    continue;
                }
                validated = true;
                break;
            }
            if (!validated)
                throw Exception(ErrorCodes::ABORTED,
                    "promote: blob {} still condemned after {} copy-forward attempts at commit revalidation — "
                    "failing closed (INV-1)", blob_key, max_reval_attempts);
        }

        /// Promotion is a PURE OWNER MOVE (spec rev. 15 §Promote Precommit): append ONE RootOwnerEvent
        /// whose old_binding and new_binding name the SAME manifest_ref T, moving ownership from
        /// precommit(build_id) to committed(final_ref_name). It emits NO blob deltas. The activating +
        /// edges came from GC's barrier-activation of the create-precommit event (the fold barrier
        /// guarantees that event is folded — body present ⇒ activated — before this promote is folded).
        const uint64_t v = root.shard_version + 1;
        root.journal.push_back(RootOwnerEvent{
            .transition_version = v,
            .old_binding = OwnerBinding{
                .owner_kind = OwnerKind::Precommit, .ref_name = final_ref_name,
                .build_id = promote_build_id, .manifest_ref = id.ref},
            .new_binding = OwnerBinding{
                .owner_kind = OwnerKind::Committed, .ref_name = final_ref_name,
                .build_id = UInt128(0), .manifest_ref = id.ref}});
        /// BUG 1a: refuse to overwrite a live committed ref that already names a DIFFERENT manifest — that
        /// would orphan the old manifest (its owner-removal `-1` is never emitted). This enforces the
        /// model's `RefFreeFor` guard (WPromote requires it). A re-promote of the SAME manifest_ref is
        /// idempotent and allowed. Fail-closed with ABORTED (not LOGICAL_ERROR): a conflicting durable
        /// state the caller handles (republishRef is made idempotent so its legitimate re-drive skips
        /// promote entirely), never a must-not-happen invariant.
        if (const auto it = root.refs.find(final_ref_name);
            it != root.refs.end() && it->second.manifest_ref != id.ref)
            throw Exception(ErrorCodes::ABORTED,
                "promote: ref '{}' already names a different committed manifest — refusing to overwrite "
                "(unique-ref invariant; use republishRef for an intended repoint)", final_ref_name);
        root.refs[final_ref_name] = RootRef{
            .ref_name = final_ref_name, .manifest_ref = id.ref,
            .mutable_files = pending_mutable_files, .published_at_ms = nowMs()};
    }, nullptr, RootMutationOrigin::Writer, RootMutationKind::Promote);

    precommitted = false;
    store->retireBuildSeq(build_seq);
    EventEmitter{*store}.emit([&](CasEvent & e)
    {
        e.type = CasEventType::BuildPublish;
        e.namespace_ = target_ns.string();
        e.ref_name = final_ref_name;
        e.object_hash = manifestRefDebugString(id.ref);
        e.token = u128ToHex(promote_build_id);
        e.outcome = "promoted";
        e.reason = "promote: atomic owner move precommit(build_id) -> ref(final_ref_name) after fail-closed reval";
        e.detail = {{"build_seq", std::to_string(build_seq)}};
    });
}

void Build::abandon()
{
    requireAlive();
    alive = false;

    /// BUG 2 / TLA+ `WAbandonPrecommit`: if this build made a manifest a LIVE precommit owner input
    /// (`precommitAdd` ran), abandoning it must NOT writer-delete that body. Instead append a precommit
    /// REMOVAL event into the target shard, mirroring EXACTLY the removal encoding used by `Store::dropRef`
    /// and `Gc::reclaimAbandonedPrecommit` (old = the precommit binding, new = none). GC then folds the
    /// `-1` blob decrements and deletes the body only after they are sealed
    /// (delete-after-sealed-decrements). Deleting a live precommit body here would strand GC's fold
    /// barrier (live precommit, missing body → clamp forever) or lose the activating `+1`. This is the
    /// correctness-bearing step of abandon, so it runs through `mutateShard` (reliable CAS), not
    /// best-effort. It precedes the best-effort debris deletion below so a partial cleanup can never
    /// leave the precommit binding live without its body's GC release queued.
    if (precommitted)
    {
        store->mutateShard(precommit_target_ns, store->shardOf(precommit_final_ref), MutationScope::ref(precommit_final_ref), [&](RootShard & root)
        {
            root.journal.push_back(RootOwnerEvent{
                .transition_version = root.shard_version + 1,
                .old_binding = OwnerBinding{
                    .owner_kind = OwnerKind::Precommit,
                    .ref_name = precommit_final_ref,
                    .build_id = build_id,
                    .manifest_ref = precommit_manifest},
                .new_binding = std::nullopt});
        }, nullptr, RootMutationOrigin::Writer, RootMutationKind::Abandon);
        precommitted = false;

        EventEmitter{*store}.emit([&](CasEvent & e)
        {
            e.type = CasEventType::PrecommitRemoved;
            e.namespace_ = precommit_target_ns.string();
            e.ref_name = precommit_final_ref;
            e.object_kind = CasEventObjectKind::Root;
            e.object_hash = manifestRefDebugString(precommit_manifest);
            e.reason = "abandon: precommit binding removed";
        });
    }

    /// No longer in-flight: retire the seq so the GC watermark floor can advance (idempotent). This runs
    /// AFTER the precommit removal above (mirrors `Build::promote`, which retires after its CAS) so
    /// `min_active` can never advance — and let GC's `reclaimAbandonedPrecommit` observe a live precommit
    /// past the watermark — before the removal is durably committed; retiring first would open a window
    /// for a double removal race between this call and GC's reclaim.
    store->retireBuildSeq(build_seq);

    /// Best-effort writer cleanup of THIS build's pre-precommit/staged `_manifests` debris (spec
    /// §Pre-Precommit Part-Manifest Debris). The common case is writer cleanup; a missed object is
    /// benign — the Phase-1d namespace-scoped orphan sweep reclaims it. Exact-token delete only; never
    /// throw from abandon. SKIP the manifest that became a live precommit owner above: its body is a live
    /// precommit input whose deletion is GC's job after the sealed decrement (never writer-delete it).
    for (const ManifestId & id : staged_manifests)
    {
        if (id.ref == precommit_manifest && id.root_namespace == precommit_target_ns)
            continue;   /// the live precommit body — left for GC (delete-after-sealed-decrements)
        try
        {
            const String key = store->layout().manifestKey(id);
            const HeadResult hr = store->backend().head(key);
            if (hr.exists)
                store->backend().deleteExact(key, hr.token);
        }
        catch (...) {}   /// best-effort: GC backstop sweep is the durable guarantee
    }

    /// B170: the build was abandoned; its uploads become GC-reclaimable debris.
    EventEmitter{*store}.emit([&](CasEvent & e)
    {
        e.type = CasEventType::BuildAbort;
        e.token = u128ToHex(build_id);
        e.outcome = "abandoned";
        e.reason = "abandon: appended a precommit-removal event for the live precommit (body left for GC); "
                   "best-effort deleted the never-precommitted _manifests debris; remainder reaped by the orphan sweep";
        e.detail = {{"build_seq", std::to_string(build_seq)}, {"staged", std::to_string(staged_manifests.size())}};
    });
}

}
