#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasBuild.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasEnvelope.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasPlacement.h>
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
}

BlobRef Build::putBlob(const BlobId & id, BlobSource source)
{
    requireAlive();

    const UInt128 logical_hash = hexToU128(id.string());
    const String key = store->layout().blobKey(id);
    const PoolConfig & cfg = store->poolConfig();

    /// Materialize source bytes once — needed both for the primary stream and for uploadFromSource
    /// (INV-1: condemned-dedup re-upload from writer's own bytes, never reading the dying object).
    /// Verify the byte count matches the declared size before any I/O to fail early and cleanly.
    String source_bytes;
    {
        WriteBufferFromString wb{source_bytes};
        source.write_payload(wb);
        wb.finalize();
    }
    if (source_bytes.size() != source.size)
        throw Exception(ErrorCodes::LOGICAL_ERROR,
            "putBlob: source wrote {} bytes, declared {}", source_bytes.size(), source.size);

    /// B168 P1/P2: HEAD-before-PUT on a likely dedup hit (cache says present) or a large body (where a
    /// wasted body-PUT that 412s is expensive — and on a store that early-closes a doomed conditional
    /// PUT, the broken-pipe + retry storm of B187). A present HEAD ⇒ admit without streaming the body;
    /// a stale/absent HEAD ⇒ fall through to the normal conditional upload. SAFE by construction: we
    /// always genuinely observe present-at-round before skipping the body, so the cache can never cause
    /// a dangle (a stale hit just HEADs 404 and uploads).
    const bool head_first =
        store->dedupCacheContains(logical_hash)
        || (cfg.dedup_head_first_min_bytes > 0 && source.size >= cfg.dedup_head_first_min_bytes);
    if (head_first)
    {
        ProfileEvents::increment(ProfileEvents::CasBlobHeadFirst);
        const HeadResult hr = store->backend().head(key);
        if (hr.exists)
        {
            ProfileEvents::increment(ProfileEvents::CasBlobBodyPutAvoided);
            if (store->dedupCacheContains(logical_hash))
                ProfileEvents::increment(ProfileEvents::CasBlobDedupCacheHit);
            try
            {
                const uint64_t admitted = observeAndAdmit(ObjectKind::Blob, logical_hash, key, hr);
                store->dedupCacheAdd(logical_hash);
                return BlobRef{id, admitted};
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
            uploadFromSource(ObjectKind::Blob, logical_hash, key, source_bytes);
            /// P1: this hash is now known-present — future writers can HEAD-first and skip the body.
            store->dedupCacheAdd(logical_hash);
            return BlobRef{id, source.size};
        }
        catch (const Exception & e)
        {
            /// ABORTED from uploadFromSource is retryable — re-upload from our held source bytes
            /// (bounded). Two cases produce it:
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

bool Build::depIsTokened(const UInt128 & hash) const
{
    /// Discriminator for B156b: a putBlob'd blob records a TOKENED dep (recreatable by retrying), an
    /// adoptFromTree carry-forward records a TOKENLESS W-EVIDENCE dep (not recreatable — pinned by a
    /// committed source). Returns false when this build has no dep for the hash (the caller decides
    /// the default; not-tokened is the fail-loud, INV-NO-LOSS-safe choice).
    auto it = deps.find({static_cast<uint8_t>(ObjectKind::Blob), hash});
    return it != deps.end() && it->second.token.has_value();
}

bool Build::hasDep(const UInt128 & hash) const
{
    return deps.contains({static_cast<uint8_t>(ObjectKind::Blob), hash});
}

uint64_t Build::observeAndAdmit(ObjectKind kind, const UInt128 & hash, const String & key)
{
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
    return observeAndAdmit(kind, hash, key, hr);
}

uint64_t Build::observeAndAdmit(ObjectKind kind, const UInt128 & hash, const String & key, const HeadResult & hr)
{
    /// `hr.exists` is guaranteed by the caller (the 3-arg wrapper checked it; the putBlob HEAD-first
    /// path only calls this on a present HEAD). Avoids a redundant second HEAD on the dedup-hit path.
    /// Logical (payload) size = object size minus the pool's fixed blob header. A tree object is laid
    /// out as [blob_header_len envelope][encodeTree payload], so the same formula applies. GUARD against
    /// unsigned underflow: a truncated/corrupt object whose size is below the header length must surface
    /// as CORRUPTED_DATA, never wrap to a huge value. Mirrors the GC path's `retiredLogicalSize`.
    /// B92: trees previously reported 0 ("would need a decode"); the layout makes that unnecessary.
    const uint64_t header_len = store->poolMeta().blob_header_len;
    if (hr.size < header_len)
        throw Exception(ErrorCodes::CORRUPTED_DATA,
            "Build: {} object {} size {} is below the pool blob header length {}",
            kind == ObjectKind::Blob ? "blob" : "tree", key, hr.size, header_len);
    const uint64_t logical_size = hr.size - header_len;

    const CasEventObjectKind ev_kind = toEventKind(kind);
    if (store->retireView().isCondemnedToken(kind, hash, hr.token))
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
            e.object_hash = u128ToHex(hash);
            e.token = hr.token.value;
            e.round = store->retireView().round();
            e.outcome = "condemned";
            e.reason = "observed token is condemned; caller must re-upload from source (INV-1)";
        });
        throw Exception(ErrorCodes::ABORTED,
            "Build::observeAndAdmit: condemned token for {} — caller must re-upload from source bytes (INV-1)",
            key);
    }

    /// Adopt the current incarnation — free, no bytes moved.
    /// B170: reuse ADOPTED an existing incarnation's token as NOT condemned (per this build's
    /// retire-view). Was the CAREUSE adopt audit line. Token-join this against a later blob_delete
    /// of the same hash/token to pin a reuse-of-an-object-being-deleted race.
    EventEmitter{*store}.emit([&](CasEvent & e)
    {
        e.type = CasEventType::BlobReuseAdopt;
        e.object_kind = ev_kind;
        e.object_hash = u128ToHex(hash);
        e.token = hr.token.value;
        e.round = store->retireView().round();
        e.outcome = "adopt";
        e.reason = "observed token not condemned; adopted the live incarnation (no bytes moved)";
    });
    deps[{static_cast<uint8_t>(kind), hash}] =
        DepEntry{kind, hr.token, store->retireView().round(), logical_size};
    return logical_size;
}

void Build::uploadFromSource(ObjectKind kind, const UInt128 & hash, const String & key, std::string_view source_bytes)
{
    /// INV-1 (revival-from-source): re-upload a condemned or absent object from the writer's OWN
    /// source bytes — NEVER calls backend().get to read the dying object. W-FRESH-TAG: fresh
    /// incarnation_tag and this build's build_id so the new incarnation is owned by THIS live build
    /// (the B167 fix — the prior resurrect was the lone gap).
    const PoolMeta & meta = store->poolMeta();
    const PoolConfig & cfg = store->poolConfig();

    auto buildHeader = [&]() -> String
    {
        EnvelopeHeader header;
        header.kind = kind;
        header.hash_algo = 1;
        header.logical_size = source_bytes.size();
        header.logical_hash = hash;
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
            observeAndAdmit(kind, hash, k_);
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
        deps[{static_cast<uint8_t>(kind), hash}] =
            DepEntry{kind, tok, store->retireView().round(), source_bytes.size()};
        EventEmitter{*store}.emit([&](CasEvent & e)
        {
            e.type = (kind == ObjectKind::Tree) ? CasEventType::TreePut : CasEventType::BlobPut;
            e.object_kind = ev_kind;
            e.object_hash = u128ToHex(hash);
            e.token = tok.value;
            e.round = store->retireView().round();
            e.outcome = "ok";
            e.reason = "uploadFromSource: fresh incarnation from writer's own source bytes (INV-1)";
            e.detail = {{"size", std::to_string(source_bytes.size())}, {"build_id", u128ToHex(build_id)}};
        });
    };

    /// Phase 1: try If-None-Match upload (object absent or race with another writer).
    {
        /// B171: no owner metadata (protection is the precommit edge — reachability, not `cas_owner`).
        WriteSinkPtr sink = store->backend().putIfAbsentStream(key);
        writeString(buildHeader(), sink->buffer());
        writeString(source_bytes, sink->buffer());
        const PutResult res = sink->finalize();
        if (res.outcome == PutOutcome::Done)
        {
            recordDoneAndEmit(res.token);
            return;
        }
    }

    /// PreconditionFailed: an incarnation exists. HEAD it to check whether it is condemned or live.
    /// We do NOT read the body (no backend().get) — we only need the token to decide:
    ///   • live (not condemned)  → adopt; this is the standard dedup case.
    ///   • condemned             → displace via putOverwrite(If-Match: current_token) using our
    ///                            source_bytes, so we never read the dying object. This is the
    ///                            equivalent of the old `resurrect` minus the GET.
    const HeadResult hr = store->backend().head(key);
    if (!hr.exists)
    {
        /// Object vanished between putIfAbsentStream (412d) and our HEAD — a concurrent GC delete.
        /// The object is now absent; re-try putIfAbsentStream (tail-recurse is safe — bounded by caller).
        WriteSinkPtr sink2 = store->backend().putIfAbsentStream(key);
        writeString(buildHeader(), sink2->buffer());
        writeString(source_bytes, sink2->buffer());
        const PutResult res2 = sink2->finalize();
        if (res2.outcome == PutOutcome::Done)
        {
            recordDoneAndEmit(res2.token);
            return;
        }
        /// Still 412 after the vanish-and-retry: a racing writer re-created it. Adopt their token.
        /// reviveObserve converts FILE_DOESNT_EXIST (deleted again in the window) → ABORTED (retryable).
        reviveObserve(key);
        return;
    }

    if (!store->retireView().isCondemnedToken(kind, hash, hr.token))
    {
        /// Live (not condemned): adopt the current incarnation — free, no bytes moved.
        observeAndAdmit(kind, hash, key, hr);
        return;
    }

    /// Condemned: displace the condemned incarnation with our fresh source bytes via If-Match.
    /// CRITICAL: we use putOverwrite (NOT backend().get) — we have the source bytes, so no GET needed.
    /// W-FRESH-TAG: a fresh incarnation_tag minted inside buildHeader() ensures INV-NO-RETURN.
    const PutResult overwrite_res = store->backend().putOverwrite(key, buildHeader() + String(source_bytes), hr.token);
    if (overwrite_res.outcome == PutOutcome::Done)
    {
        recordDoneAndEmit(overwrite_res.token);
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
            WriteSinkPtr sink3 = store->backend().putIfAbsentStream(key);
            writeString(buildHeader(), sink3->buffer());
            writeString(source_bytes, sink3->buffer());
            const PutResult res3 = sink3->finalize();
            if (res3.outcome == PutOutcome::Done)
            {
                recordDoneAndEmit(res3.token);
                return;
            }
            /// Still 412 — a racing writer re-created it. Observe their token.
            /// reviveObserve converts FILE_DOESNT_EXIST (deleted again in the window) → ABORTED (retryable).
            reviveObserve(key);
            return;
        }
        /// Present with a different token — a racing writer displaced the condemned incarnation.
        /// Adopt their token (or throw ABORTED if it too is condemned — bounded by caller loop).
        observeAndAdmit(kind, hash, key, hr2);
    }
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
        deps[{static_cast<uint8_t>(ObjectKind::Blob), entry.blob_hash}] =
            DepEntry{ObjectKind::Blob, std::nullopt, store->retireView().round(), entry.blob_size};
    }
}

void Build::recordPendingBlobDep(const UInt128 & hash, uint64_t size)
{
    requireAlive();
    deps[{static_cast<uint8_t>(ObjectKind::Blob), hash}] =
        DepEntry{ObjectKind::Blob, std::nullopt, store->retireView().round(), size};
}

String Build::writerInstanceId() const
{
    /// OQ6: stable server id + durable process epoch. A new process incarnation gets a new epoch, so it
    /// cannot reuse a prior incarnation's `_manifests/<writer_instance_id>/...` build prefix.
    return u128ToHex(store->poolConfig().server_id) + ":" + std::to_string(epoch);
}

RootNamespace Build::manifestNamespace() const
{
    /// The owning namespace is BuildInfo::intended_ref minus its last `/`-segment (the ref name). A CA
    /// namespace itself contains `/` (e.g. "srv1/<uuid>@cas@"), so split on the LAST slash only.
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

    /// Mint the identity. manifest_instance_id is random (NoManifestIdReuse) and never derived from
    /// payload. With writer_instance_id + build_seq it forms the ManifestRef; with the owning namespace
    /// it forms the ManifestId.
    const ManifestRef ref{writerInstanceId(), build_seq, mintU128()};

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
            "stageManifest: manifest_instance_id collision at {} (PreconditionFailed) — failing closed", key);

    staged_manifests.push_back(id);
    return id;
}

String Build::keyFor(ObjectKind kind, const UInt128 & hash) const
{
    return objectKey(store->layout(), kind, hash);
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

    /// W-REGISTER: the target namespace must be in `gc/registry` before its first transition exists.
    store->ensureRegistered(target_ns);

    /// ONE CAS on the TARGET shard (shardOf(final_ref_name)): append a create-precommit RootOwnerEvent
    /// {old=none, new={Precommit, final_ref_name, build_id, id.ref}} to the single ordered journal. No body
    /// HEAD — a missing body is a legal fail-closed, non-activating intent (spec §Precommit Add).
    store->mutateShard(target_ns, store->shardOf(final_ref_name), [&](RootShard & root)
    {
        root.journal.push_back(RootOwnerEvent{
            .transition_version = root.shard_version + 1,
            .old_binding = std::nullopt,
            .new_binding = OwnerBinding{
                .owner_kind = OwnerKind::Precommit,
                .ref_name = final_ref_name,
                .build_id = build_id,
                .manifest_ref = id.ref}});
    });

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

    const uint64_t registry_fence = store->ensureRegistered(target_ns);

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

    store->mutateShard(target_ns, store->shardOf(final_ref_name), [&](RootShard & root)
    {
        /// Refresh-then-revalidate (W-REGISTER ordering, load-bearing): if the shard's fence_round
        /// (floored by the registry fence) is ahead of our view, GC advanced — refresh first.
        if (store->retireView().round() < std::max(root.fence_round, registry_fence))
            store->retireView().refresh();

        /// Fail-closed blob revalidation of EVERY blob leaf (spec §Promote Precommit step 3). A condemned
        /// blob is recreatable only from this build's own source (INV-1); a missing or condemned blob ⇒
        /// ABORTED.
        for (const ManifestEntry & e : body.entries)
        {
            if (e.placement != EntryPlacement::Blob)
                continue;
            const BlobId blob_id{u128ToHex(e.blob_hash)};
            const String blob_key = store->layout().blobKey(blob_id);
            const HeadResult hr = store->backend().head(blob_key);
            if (!hr.exists)
                throw Exception(ErrorCodes::ABORTED,
                    "promote: blob {} absent at commit revalidation — failing closed", blob_key);
            if (store->retireView().isCondemnedToken(ObjectKind::Blob, e.blob_hash, hr.token))
                throw Exception(ErrorCodes::ABORTED,
                    "promote: blob {} condemned at commit revalidation — failing closed (INV-1)", blob_key);
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
        root.refs[final_ref_name] = RootRef{
            .ref_name = final_ref_name, .manifest_ref = id.ref,
            .mutable_files = pending_mutable_files, .published_at_ms = nowMs()};
    });

    precommitted = false;
    store->retireBuildSeq(build_seq);
    EventEmitter{*store}.emit([&](CasEvent & e)
    {
        e.type = CasEventType::BuildPublish;
        e.namespace_ = target_ns.string();
        e.ref_name = final_ref_name;
        e.object_hash = u128ToHex(id.ref.manifest_instance_id);
        e.token = u128ToHex(promote_build_id);
        e.outcome = "promoted";
        e.reason = "promote: atomic owner move precommit(build_id) -> ref(final_ref_name) after fail-closed reval";
        e.detail = {{"build_seq", std::to_string(build_seq)}};
    });
}

void Build::abandon()
{
    requireAlive();
    /// No longer in-flight: retire the seq so the GC watermark floor can advance (idempotent).
    store->retireBuildSeq(build_seq);
    alive = false;

    /// Best-effort writer cleanup of THIS build's pre-precommit/staged `_manifests` debris (spec
    /// §Pre-Precommit Part-Manifest Debris). The common case is writer cleanup; a missed object is
    /// benign — the Phase-1d namespace-scoped orphan sweep reclaims it. Exact-token delete only; never
    /// throw from abandon.
    for (const ManifestId & id : staged_manifests)
    {
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
        e.reason = "abandon: best-effort _manifests debris cleanup; remainder reaped by the orphan sweep";
        e.detail = {{"build_seq", std::to_string(build_seq)}, {"staged", std::to_string(staged_manifests.size())}};
    });
}

}
