#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasBuild.h>
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
}
}

namespace DB::Cas
{

namespace
{

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

Build::Build(StorePtr store_, std::unique_ptr<HeartbeatKeeper> heartbeat_, UInt128 build_id_,
             uint64_t build_seq_, uint64_t epoch_, BuildInfo info_)
    : store(std::move(store_))
    , heartbeat(std::move(heartbeat_))
    , build_id(build_id_)
    , build_seq(build_seq_)
    , epoch(epoch_)
    , info(std::move(info_))
{
    /// B170: a build began (W-HEARTBEAT durable). build_id/seq/epoch identify it for token-join
    /// attribution against the GC delete rows.
    if (store->hasEventSink())
    {
        CasEvent _ev0;
        _ev0.type = CasEventType::BuildStart;
        _ev0.ref_name = info.intended_ref.value_or("");
        _ev0.token = u128ToHex(build_id);
        _ev0.outcome = "started";
        _ev0.reason = "startBuild: heartbeat durable; build in-flight";
        _ev0.detail = {{"build_seq", std::to_string(build_seq)}, {"epoch", std::to_string(epoch)}};
        store->emitEvent(_ev0);
    }
}

Build::~Build()
{
    /// Crash semantics: the Build dtor takes no special action on the heartbeat. If the build was not
    /// abandoned, the heartbeat keeper's own dtor stops its background thread WITHOUT discarding the
    /// key, so the uploads become debris that full GC reclaims under the heartbeat rules (spec §5).
    /// abandon() is the only path that proactively discards.
    /// Retire our build_seq from the Store's active set so the GC watermark floor (minActive) can
    /// advance even if neither publish nor abandon ran (idempotent — safe if already retired).
    store->retireBuildSeq(build_seq);
}

void Build::requireAlive() const
{
    if (!alive)
        throw Exception(ErrorCodes::LOGICAL_ERROR, "Build has been abandoned; no further operations allowed");
}

void Build::renewHeartbeat()
{
    requireAlive();
    heartbeat->renewOnce();
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
            /// ABORTED from observeAndAdmit inside uploadFromSource: a racing writer concurrently
            /// displaced the condemned token before our putOverwrite landed, and their fresh incarnation
            /// is itself already condemned. Extremely rare. Retry uploadFromSource (bounded).
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
    /// Logical (payload) size = object size minus the pool's fixed blob header; trees would need a
    /// decode, so report 0. GUARD against unsigned underflow: a truncated/corrupt blob whose object
    /// size is below the header length must surface as CORRUPTED_DATA, never wrap to a huge value
    /// (the result is the caller-visible BlobRef::size). Mirrors the GC path's `retiredLogicalSize`.
    uint64_t logical_size = 0;
    if (kind == ObjectKind::Blob)
    {
        const uint64_t header_len = store->poolMeta().blob_header_len;
        if (hr.size < header_len)
            throw Exception(ErrorCodes::CORRUPTED_DATA,
                "Build: blob {} object size {} is below the pool blob header length {}", key, hr.size, header_len);
        logical_size = hr.size - header_len;
    }

    const CasEventObjectKind ev_kind =
        kind == ObjectKind::Tree ? CasEventObjectKind::Tree
        : (kind == ObjectKind::Pack ? CasEventObjectKind::Pack : CasEventObjectKind::Blob);
    if (store->retireView().isCondemnedToken(kind, hash, hr.token))
    {
        /// INV-1 (revival-from-source): the observed token is condemned — we must NOT read the dying
        /// object via backend().get. Throw ABORTED so the caller can re-upload from its OWN source bytes.
        ///   • putBlob (has BlobSource): catches ABORTED, calls uploadFromSource from held bytes.
        ///   • uploadStagedTree / recreateTree (has retained_trees payload): calls uploadFromSource.
        ///   • gate bodyless (no source): propagates ABORTED (retryable; caller retries op).
        if (store->hasEventSink())
        {
            CasEvent _ev2;
            _ev2.type = CasEventType::BlobReuseResurrect;
            _ev2.object_kind = ev_kind;
            _ev2.object_hash = u128ToHex(hash);
            _ev2.token = hr.token.value;
            _ev2.round = store->retireView().round();
            _ev2.outcome = "condemned";
            _ev2.reason = "observed token is condemned; caller must re-upload from source (INV-1)";
            store->emitEvent(_ev2);
        }
        throw Exception(ErrorCodes::ABORTED,
            "Build::observeAndAdmit: condemned token for {} — caller must re-upload from source bytes (INV-1)",
            key);
    }

    /// Adopt the current incarnation — free, no bytes moved.
    /// B170: reuse ADOPTED an existing incarnation's token as NOT condemned (per this build's
    /// retire-view). Was the CAREUSE adopt audit line. Token-join this against a later blob_delete
    /// of the same hash/token to pin a reuse-of-an-object-being-deleted race.
    if (store->hasEventSink())
    {
        CasEvent _ev3;
        _ev3.type = CasEventType::BlobReuseAdopt;
        _ev3.object_kind = ev_kind;
        _ev3.object_hash = u128ToHex(hash);
        _ev3.token = hr.token.value;
        _ev3.round = store->retireView().round();
        _ev3.outcome = "adopt";
        _ev3.reason = "observed token not condemned; adopted the live incarnation (no bytes moved)";
        store->emitEvent(_ev3);
    }
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
        {
            header.intended_ref = info.intended_ref;
            header.pad_to_header_len = static_cast<uint32_t>(meta.blob_header_len);
        }
        /// Trees use natural header length (no pad) — pad_to_header_len stays 0.
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

    const CasEventObjectKind ev_kind =
        kind == ObjectKind::Tree ? CasEventObjectKind::Tree
        : (kind == ObjectKind::Pack ? CasEventObjectKind::Pack : CasEventObjectKind::Blob);

    auto recordDoneAndEmit = [&](Token tok)
    {
        deps[{static_cast<uint8_t>(kind), hash}] =
            DepEntry{kind, tok, store->retireView().round(), source_bytes.size()};
        if (store->hasEventSink())
        {
            CasEvent _ev;
            _ev.type = (kind == ObjectKind::Tree) ? CasEventType::TreePut : CasEventType::BlobPut;
            _ev.object_kind = ev_kind;
            _ev.object_hash = u128ToHex(hash);
            _ev.token = tok.value;
            _ev.round = store->retireView().round();
            _ev.outcome = "ok";
            _ev.reason = "uploadFromSource: fresh incarnation from writer's own source bytes (INV-1)";
            _ev.detail = {{"size", std::to_string(source_bytes.size())}, {"build_id", u128ToHex(build_id)}};
            store->emitEvent(_ev);
        }
    };

    /// Phase 1: try If-None-Match upload (object absent or race with another writer).
    {
        /// B171: no owner metadata (protection is the precommit edge — reachability, not `cas_owner`).
        WriteSinkPtr sink = store->backend().putIfAbsentStream(key);
        writeString(buildHeader(), sink->buffer());
        writeString(source_bytes, sink->buffer());
        Token tok;
        const PutOutcome outcome = sink->finalize(&tok);
        if (outcome == PutOutcome::Done)
        {
            recordDoneAndEmit(tok);
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
        Token tok2;
        const PutOutcome outcome2 = sink2->finalize(&tok2);
        if (outcome2 == PutOutcome::Done)
        {
            recordDoneAndEmit(tok2);
            return;
        }
        /// Still 412 after the vanish-and-retry: a racing writer re-created it. Adopt their token.
        observeAndAdmit(kind, hash, key);
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
    Token new_tok;
    const PutOutcome oc = store->backend().putOverwrite(key, buildHeader() + String(source_bytes), hr.token, &new_tok);
    if (oc == PutOutcome::Done)
    {
        recordDoneAndEmit(new_tok);
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
            Token tok3;
            const PutOutcome outcome3 = sink3->finalize(&tok3);
            if (outcome3 == PutOutcome::Done)
            {
                recordDoneAndEmit(tok3);
                return;
            }
            /// Still 412 — a racing writer re-created it. Observe their token.
            observeAndAdmit(kind, hash, key);
            return;
        }
        /// Present with a different token — a racing writer displaced the condemned incarnation.
        /// Adopt their token (or throw ABORTED if it too is condemned — bounded by caller loop).
        observeAndAdmit(kind, hash, key, hr2);
    }
}

void Build::adoptEvidence(const TreeEntry & entry)
{
    requireAlive();

    /// W-EVIDENCE: record a TOKENLESS dependency — liveness evidence is the live source root, not a
    /// token. Inline entries reference no standalone object, so they record nothing.
    /// NO backend call (no HEAD, no GET, no PUT) — the caller already holds the resolved entry.
    const uint64_t view_round = store->retireView().round();
    switch (entry.placement)
    {
        case Placement::Blob:
            deps[{static_cast<uint8_t>(ObjectKind::Blob), entry.file_hash}] =
                DepEntry{ObjectKind::Blob, std::nullopt, view_round, entry.file_size};
            break;
        case Placement::Subtree:
            deps[{static_cast<uint8_t>(ObjectKind::Tree), entry.file_hash}] =
                DepEntry{ObjectKind::Tree, std::nullopt, view_round, entry.file_size};
            break;
        case Placement::PackSlice:
            deps[{static_cast<uint8_t>(ObjectKind::Pack), entry.pack_hash}] =
                DepEntry{ObjectKind::Pack, std::nullopt, view_round, entry.pack_length};
            break;
        case Placement::Inline:
            break;   /// no standalone object — nothing to depend on
    }
}

void Build::recordPendingBlobDep(const UInt128 & hash, uint64_t size)
{
    requireAlive();
    deps[{static_cast<uint8_t>(ObjectKind::Blob), hash}] =
        DepEntry{ObjectKind::Blob, std::nullopt, store->retireView().round(), size};
}

TreeEntry Build::adoptFromTree(const TreeId & source, const String & name)
{
    requireAlive();

    auto it = source_tree_cache.find(source);
    if (it == source_tree_cache.end())
        it = source_tree_cache.emplace(source, store->readTree(source)).first;

    for (const TreeEntry & entry : it->second)
    {
        if (entry.name != name)
            continue;

        adoptEvidence(entry);
        return entry;
    }

    throw Exception(ErrorCodes::BAD_ARGUMENTS,
        "adoptFromTree: no entry named '{}' in source tree {}", name, source.string());
}

void Build::adoptTree(const TreeId & id)
{
    requireAlive();
    /// Whole-tree adoption is a COLD REUSE, not blind evidence (amended 2026-06-12, found
    /// refreshing the TLA+ model): the object is OBSERVED at adopt time (one HEAD inside
    /// observeAndAdmit) and the dependency recorded TOKEN-BEARING, so the full W-REVALIDATE
    /// machinery covers it. A blind tokenless dep here was a dangle: a detached tree already
    /// reclaimed by a COMPLETED round has no view hit (entries drop on confirmed outcomes), and a
    /// view already refreshed AT the current round skips both the publish-time re-observation
    /// (the observed_view_round >= round keep branch) and the fence-advanced refresh (view round
    /// == fence round) - the publish would land a manifest naming a deleted tree. The live-source
    /// argument that justifies tokenless evidence for adoptFromTree CHILDREN does not apply to
    /// the root being adopted: a detached/frozen tree is exactly NOT pinned by anything. Absent
    /// => FILE_DOESNT_EXIST (fail closed; the caller re-creates from source or aborts the
    /// re-attach); condemned => ABORTED from observeAndAdmit (INV-1: no GET; caller retries).
    /// FOLLOW-UP(M-W): size 0 here flows into `RefPayload.tree_size` at publish for adopt-published
    /// parts (FREEZE / detached re-attach / replication relink). Harmless in M-C2 (no read path
    /// consumes tree_size — `readTree` GETs the whole object, `locate` uses per-entry file_size), but
    /// `Resolved.tree_size` is reader-facing precisely so the M-W wiring can build StoredObjects
    /// without a HEAD (spec §6). Recover the real size on the adopt path before M-W relies on it
    /// (the subtree case in `adoptFromTree` already carries entry.file_size); add a round-trip test
    /// asserting tree_size survives adopt-republish.
    const UInt128 hash = hexToU128(id.string());
    observeAndAdmit(ObjectKind::Tree, hash, keyFor(ObjectKind::Tree, hash));
}

TreeId Build::stageTree(std::vector<TreeEntry> entries)
{
    requireAlive();

    /// W-TREE-BUILD: bottom-up discipline — every referenced child must already be a dependency.
    for (const TreeEntry & entry : entries)
    {
        switch (entry.placement)
        {
            case Placement::Blob:
                if (!deps.contains({static_cast<uint8_t>(ObjectKind::Blob), entry.file_hash}))
                    throw Exception(ErrorCodes::LOGICAL_ERROR,
                        "stageTree: child blob {} not in dependency set (W-TREE-BUILD)",
                        u128ToHex(entry.file_hash));
                break;
            case Placement::Subtree:
                if (!deps.contains({static_cast<uint8_t>(ObjectKind::Tree), entry.file_hash}))
                    throw Exception(ErrorCodes::LOGICAL_ERROR,
                        "stageTree: child tree {} not in dependency set (W-TREE-BUILD)",
                        u128ToHex(entry.file_hash));
                break;
            case Placement::PackSlice:
                if (!deps.contains({static_cast<uint8_t>(ObjectKind::Pack), entry.pack_hash}))
                    throw Exception(ErrorCodes::LOGICAL_ERROR,
                        "stageTree: child pack {} not in dependency set (W-TREE-BUILD)",
                        u128ToHex(entry.pack_hash));
                break;
            case Placement::Inline:
                break;   /// inline bytes need no dependency
        }
    }

    const String encoded = encodeTree(std::move(entries));   /// canonical sort + duplicate-name check
    const TreeId id = treeIdFor(encoded);
    const UInt128 logical_hash = hexToU128(id.string());

    /// RETAIN the encoded payload: trees are always re-creatable during the gate's W-REVALIDATE
    /// (Task 13), unlike blob payloads which are not retained.
    retained_trees[logical_hash] = encoded;

    /// Record a TOKENLESS Tree dep — LOCAL ONLY, no upload. precommit tolerates an absent tree
    /// object (it just needs the dep in the set); uploadStagedTree uploads the object post-precommit.
    deps[{static_cast<uint8_t>(ObjectKind::Tree), logical_hash}] =
        DepEntry{ObjectKind::Tree, std::nullopt, store->retireView().round(), encoded.size()};

    return id;
}

void Build::uploadStagedTree(const TreeId & id)
{
    requireAlive();

    const UInt128 logical_hash = hexToU128(id.string());

    /// Recover the retained payload — must have been staged first.
    auto it = retained_trees.find(logical_hash);
    if (it == retained_trees.end())
        throw Exception(ErrorCodes::LOGICAL_ERROR,
            "uploadStagedTree: tree {} was not staged", id.string());

    const String key = store->layout().treeKey(id);
    /// Thin caller of uploadFromSource — INV-1: never reads the dying object even if the tree
    /// is condemned (uploadFromSource uses the retained payload, not a GET).
    uploadFromSource(ObjectKind::Tree, logical_hash, key, it->second);
}

TreeId Build::putTree(std::vector<TreeEntry> entries)
{
    auto id = stageTree(std::move(entries));
    uploadStagedTree(id);
    return id;
}

String Build::keyFor(ObjectKind kind, const UInt128 & hash) const
{
    return objectKey(store->layout(), kind, hash);
}

RootNamespace Build::precommitNs() const
{
    /// `<server_hex>/_precommits` — the precommit namespace for this server (B171, spec §A; relocated
    /// under the server's own subtree in Phase 6).
    return RootNamespace{u128ToHex(store->poolConfig().server_id) + "/_precommits"};
}

String Build::buildRef() const
{
    /// The precommit ref name IS the per-process monotone build_seq (B171 fix). Many builds share a
    /// precommit shard, each keyed by its own ref name, exactly like a table namespace's refs.
    return std::to_string(build_seq);
}

uint64_t Build::buildShard() const
{
    /// B171 fix: shard the precommit namespace EXACTLY like every other namespace — hash the ref name. The
    /// precommit namespace then has at most root_shards shards (bounded), each holding many builds'
    /// precommit refs keyed by build_seq, folded via the normal [0, root_shards) enumeration. Was
    /// `build_seq` (per-process unique), which created an unbounded shard per build and wedged GC fold.
    return store->shardOf(buildRef());
}

void Build::precommit(const TreeId & manifest)
{
    requireAlive();

    /// B171 two-phase commit, phase 1. Publish the build's manifest tree as a ref (named build_seq) under
    /// the precommit namespace `<server_hex>/_precommits`, shard = shardOf(build_seq). GC discovers it via the registry
    /// (ensureRegistered) and folds it like any namespace → root edge → tree_expand → child in-degree ≥ 1,
    /// so every object reachable from the precommit is protected by REACHABILITY (replacing the revocable
    /// `cas_owner` hint). The manifest must be in this build's W-DEP-SET — it was built/adopted by this
    /// Build (the caller putTree'd / adoptTree'd it), mirroring `publish`'s precondition.
    const UInt128 manifest_hash = hexToU128(manifest.string());
    auto dep_it = deps.find({static_cast<uint8_t>(ObjectKind::Tree), manifest_hash});
    if (dep_it == deps.end())
        throw Exception(ErrorCodes::LOGICAL_ERROR,
            "precommit: tree {} was not built/adopted by this Build", manifest.string());

    /// W-REGISTER: the precommit namespace must be in `gc/registry` before its first manifest exists,
    /// so GC discovers it (it is an ordinary namespace key-wise; only behavioral branches key off
    /// `isPrecommitNamespace`). Monotone — a cache hit short-circuits without I/O.
    const RootNamespace ns = precommitNs();
    store->ensureRegistered(ns);

    /// tree_size: for a tree we built we have the encoded payload size; for an adopted tree we may only
    /// have 0 (the dep size) — acceptable GC bookkeeping in M-C2 (mirrors `publish`).
    auto retained_it = retained_trees.find(manifest_hash);
    const uint64_t tree_size =
        retained_it != retained_trees.end() ? retained_it->second.size() : dep_it->second.size;

    /// ONE CAS on the precommit shard: set refs[build_seq] = manifest, append {+, build_seq, manifest},
    /// shard_version++. The SAME RootShard machinery as `publish` — the precommit edge is an ordinary
    /// root ref the GC fold will expand. The ref name is the build_seq and the shard is shardOf(ref), so
    /// many builds co-locate in the same bounded shard (B171 fix).
    const String ref = buildRef();
    store->mutateShard(ns, buildShard(), [&](RootShard & root)
    {
        RefPayload payload;
        payload.tree_id = manifest_hash;
        payload.tree_size = tree_size;
        root.refs[ref] = payload;
        root.journal.push_back(JournalRecord{
            .op = JournalRecord::Op::Add, .ref_name = ref, .tree_id = manifest_hash,
            .at_version = root.shard_version + 1});
    });

    /// B171: the precommit was published — the precommit edge now protects the manifest's closure.
    /// Record that fact so the successful-commit path (and only it) removes the edge.
    precommitted = true;
    if (store->hasEventSink())
    {
        CasEvent _ev;
        _ev.type = CasEventType::Precommit;
        _ev.namespace_ = ns.string();
        _ev.ref_name = ref;
        _ev.object_kind = CasEventObjectKind::Tree;
        _ev.object_hash = manifest.string();
        _ev.token = u128ToHex(build_id);
        _ev.round = store->retireView().round();
        _ev.outcome = "ok";
        _ev.reason = "precommit: published precommit manifest edge to protect the closure during the build";
        _ev.detail = {{"build_seq", std::to_string(build_seq)}};
        store->emitEvent(_ev);
    }
}

void Build::recreateTree(const UInt128 & hash)
{
    /// W-REVALIDATE re-create branch. The encoded payload was RETAINED at stageTree, so a tree is
    /// always re-creatable. Thin caller of uploadFromSource — INV-1: never reads the dying object.
    const auto retained_it = retained_trees.find(hash);
    if (retained_it == retained_trees.end())
        throw Exception(ErrorCodes::LOGICAL_ERROR,
            "Build::recreateTree: no retained payload for tree {} (only built trees are re-creatable)",
            u128ToHex(hash));

    const TreeId id{u128ToHex(hash)};
    const String key = store->layout().treeKey(id);
    uploadFromSource(ObjectKind::Tree, hash, key, retained_it->second);
}

void Build::checkAndResolveDeps()
{
    /// B190 Task 3 merged gate pass: W-REVALIDATE (the model's `WPublishReval`) + W-EVIDENCE /
    /// condemned-token scan in a SINGLE loop over `deps`. Replaces the former separate
    /// `revalidateDeps` + `gateCheckDeps` calls. Runs INSIDE the publish mutateShard lambda —
    /// re-runs on every CAS retry (idempotent: re-observe is safe).
    ///
    /// Iterating `deps` while uploadFromSource/observeAndAdmit/recreateTree mutate deps is safe:
    /// all three overwrite the SAME (kind, hash) entry (deps[thatkey].token / observed_view_round),
    /// never insert a new key — so the map structure and the current iterator stay valid.
    if (store->hasEventSink())
    {
        CasEvent _ev6;
        _ev6.type = CasEventType::GateRevalidate;
        _ev6.token = u128ToHex(build_id);
        _ev6.round = store->retireView().round();
        _ev6.outcome = "revalidating";
        _ev6.reason = "fail-closed commit: re-prove the dependency closure is present before writing the ref";
        _ev6.detail = {{"deps", std::to_string(deps.size())}};
        store->emitEvent(_ev6);
    }

    /// Gate-local wrapper: calls the 3-arg observeAndAdmit and converts FILE_DOESNT_EXIST (object absent
    /// at observe time) to ABORTED. At the publish gate, an object that is fully GC-deleted before our
    /// HEAD is a transient race under INV-3 — the caller retries the operation and re-materializes from
    /// source. Every other absent/condemned outcome in the gate already throws ABORTED; this wrapper
    /// makes the absent-at-HEAD path consistent. Note: adoptTree also calls the 3-arg overload and
    /// intentionally keeps FILE_DOESNT_EXIST (fail-closed semantics for detached-tree re-attach) — it
    /// does NOT use this wrapper.
    auto gateObserveAndAdmit = [&](ObjectKind k_, const UInt128 & h_, const String & kstr_)
    {
        try
        {
            observeAndAdmit(k_, h_, kstr_);
        }
        catch (const Exception & e_)
        {
            if (e_.code() != ErrorCodes::FILE_DOESNT_EXIST)
                throw;
            /// B190 residual (INV-3): object vanished (GC-deleted) between the retire-view hit and our
            /// HEAD. The dep is lost with no source bytes at the gate → retryable ABORTED, matching all
            /// other absent/condemned gate branches. The soak saw: "WORKLOAD FAILURE: Code 107 ... blobs/
            /// 0b/0b343... absent — cannot reuse" — exactly this path for a bodyless adopt dep.
            throw Exception(ErrorCodes::ABORTED,
                "checkAndResolveDeps: object {} vanished (GC-deleted) before gate observe; "
                "retry the operation — re-upload/re-materialize from source (INV-3)", kstr_);
        }
    };

    for (auto & [key, dep] : deps)
    {
        /// Iteration-safety invariant: DepKey's kind byte (key.first) == dep.kind, so any mutation
        /// inside uploadFromSource/observeAndAdmit/recreateTree overwrites THIS entry, never a new key.
        chassert(static_cast<uint8_t>(dep.kind) == key.first);

        const ObjectKind kind = dep.kind;
        const UInt128 & hash = key.second;
        const String k = keyFor(kind, hash);

        const std::optional<std::vector<Token>> hits = store->retireView().findCondemned(kind, hash);

        if (!dep.token.has_value())
        {
            /// ── TOKENLESS (W-EVIDENCE) dep ──────────────────────────────────────────────────────
            ///
            /// Priority 1: any view hit by hash — act regardless of staleness.
            /// Retire entries drop on confirmed outcomes (F1), so "no hit" can mean "condemned, deleted,
            /// entry dropped". When the entry IS still there (hit), it identifies a condemned token for
            /// this hash. observeAndAdmit HEADs the object; if the current token is condemned it throws
            /// ABORTED (INV-1: no GET of the dying object). This covers both fresh and stale evidence
            /// deps — the old two-pass did this in gateCheckDeps after revalidateDeps' `continue`.
            if (hits.has_value())
            {
                gateObserveAndAdmit(kind, hash, k);
                continue;
            }

            /// Priority 2: no hit + stale (the view round advanced past the evidence's recording round).
            /// The live-source-root argument covers only the SAME round window; a full boundary in between
            /// invalidates it (the source may have been dropped and the object reclaimed). Re-observe.
            if (dep.observed_view_round < store->retireView().round())
            {
                const HeadResult hr = store->backend().head(k);
                if (!hr.exists)
                {
                    if (kind == ObjectKind::Tree && retained_trees.contains(hash))
                        recreateTree(hash);
                    else
                        throw Exception(ErrorCodes::ABORTED,
                            "publish evidence dependency {} lost and not re-creatable; retry the operation",
                            u128ToHex(hash));
                }
                else
                {
                    /// Present: adopt the current token via the 4-arg overload to avoid a redundant second
                    /// HEAD (we already hold `hr`). Condemned → ABORTED from observeAndAdmit (INV-1).
                    observeAndAdmit(kind, hash, k, hr);
                }
            }
            /// Priority 3: no hit + fresh → keep (evidence as fresh as the view; the handshake covers it).
        }
        else
        {
            /// ── TOKEN-BEARING dep ───────────────────────────────────────────────────────────────
            ///
            /// Priority 1: any view hit by hash — act regardless of staleness.
            /// A hit is by HASH: the current incarnation may have been displaced to a different token t'
            /// that GC then condemned; our dep.token may be stale and not capture t'. We act on any hit.
            if (hits.has_value())
            {
                if (store->hasEventSink())
                {
                    CasEvent _ev5;
                    _ev5.type = CasEventType::GateResurrect;
                    _ev5.object_kind = kind == ObjectKind::Tree ? CasEventObjectKind::Tree
                        : (kind == ObjectKind::Pack ? CasEventObjectKind::Pack : CasEventObjectKind::Blob);
                    _ev5.object_hash = u128ToHex(hash);
                    _ev5.token = u128ToHex(build_id);
                    _ev5.round = store->retireView().round();
                    _ev5.outcome = "recreate-or-reobserve";
                    _ev5.reason = "gate: dep has a view hit by hash (INV-1: no GET)";
                    store->emitEvent(_ev5);
                }

                if (store->retireView().isCondemnedToken(kind, hash, *dep.token))
                {
                    /// Case (a): dep's own token is condemned.
                    if (kind == ObjectKind::Tree && retained_trees.contains(hash))
                        recreateTree(hash);
                    else
                        /// No source bytes at the gate (blob/pack with condemned own token) →
                        /// retryable ABORTED. The outer INSERT retry re-uploads from source. (INV-1)
                        throw Exception(ErrorCodes::ABORTED,
                            "checkAndResolveDeps: condemned dep {} has no retained payload; retry the operation (INV-1)",
                            u128ToHex(hash));
                }
                else
                {
                    /// Case (b): dep's own token is live, but a displaced incarnation's token was
                    /// condemned. Re-observe to adopt the current token (HEAD-only, no GET). observeAndAdmit
                    /// throws ABORTED if the current incarnation is itself condemned (bounded by caller).
                    /// gateObserveAndAdmit converts FILE_DOESNT_EXIST (object absent — GC-deleted in the
                    /// window between the view hit and our HEAD) to ABORTED (INV-3 retryable).
                    gateObserveAndAdmit(kind, hash, k);
                }
                continue;
            }

            /// Priority 2: no hit + stale — W-REVALIDATE single re-observation (one HEAD).
            if (dep.observed_view_round < store->retireView().round())
            {
                const HeadResult hr = store->backend().head(k);
                if (!hr.exists)
                {
                    /// Deleted / absent in the refreshed reality.
                    if (kind == ObjectKind::Tree && retained_trees.contains(hash))
                        recreateTree(hash);   /// spec §7 step 4: deleted/absent ⇒ re-create
                    else
                        throw Exception(ErrorCodes::ABORTED,
                            "publish dependency {} lost and not re-creatable; retry the operation",
                            u128ToHex(hash));
                }
                else if (hr.token == *dep.token)
                {
                    /// current == observed ⇒ KEEP — safe by the IN-FLIGHT DISJUNCTION (model-checked): a
                    /// delete in flight for (hash, t) implies its retire entry is still HELD (a VIEW HIT),
                    /// and we are in the no-hit branch. Re-stamp so a later same-publish recheck is fresh.
                    dep.observed_view_round = store->retireView().round();
                }
                else
                {
                    /// current token differs ⇒ adopt the newer token via the 4-arg overload (pass the
                    /// already-fetched hr — avoids a redundant second HEAD). condemned → ABORTED (INV-1).
                    observeAndAdmit(kind, hash, k, hr);
                }
            }
            /// Priority 3: no hit + fresh → keep (already valid at this view round).
        }
    }
}

void Build::publish(const RootNamespace & ns, const String & ref_name, const TreeId & tree, RefPayload payload)
{
    requireAlive();   /// abandon disables publish too (the test asserts LOGICAL_ERROR after abandon)

    /// The published root MUST be in the W-DEP-SET — built or adopted by this Build (spec §5).
    const UInt128 tree_hash = hexToU128(tree.string());
    auto dep_it = deps.find({static_cast<uint8_t>(ObjectKind::Tree), tree_hash});
    if (dep_it == deps.end())
        throw Exception(ErrorCodes::LOGICAL_ERROR,
            "publish: tree {} was not built/adopted by this Build", tree.string());

    /// Fill the tree fields of the caller's payload (mutable_files is caller input, preserved).
    payload.tree_id = tree_hash;
    auto retained_it = retained_trees.find(tree_hash);
    /// tree_size: for a tree we built we have the encoded payload size; for an adopted tree we may only
    /// have 0 (the dep size) — acceptable GC bookkeeping in M-C2.
    payload.tree_size = retained_it != retained_trees.end() ? retained_it->second.size() : dep_it->second.size;

    /// Heartbeat local-sanity (LIVENESS, never safety): if our heartbeat is stale by the local clock,
    /// renew it once. With background_heartbeats=false and a fresh build the elapsed time is ~0, so this
    /// never fires in tests — keep it non-flaky (no test depends on timing).
    if (heartbeat)
    {
        const auto elapsed = std::chrono::steady_clock::now() - heartbeat->lastRenewTime();
        if (elapsed > 3 * store->poolConfig().heartbeat_period)
            heartbeat->renewOnce();
    }

    /// W-REGISTER (spec §5, decision 2026-06-12): a namespace must be in `gc/registry` BEFORE its
    /// first manifest exists — that is what orders namespace CREATION against the GC fence. The
    /// returned registry fence_round is the GATE FLOOR for this publish: a brand-new namespace's
    /// shard manifest carries fence_round 0 and could never trigger the refresh below on its own,
    /// so a stale-view writer would slip a condemned hash past the recheck (the absent-shard
    /// ordering hole). Already-registered namespaces return floor 0 (cache hit) — R3 fences all
    /// their shards each round, so the per-shard fence carries the ordering.
    const uint64_t registry_fence = store->ensureRegistered(ns);

    /// Publish = ONE CAS (spec §5 step 4): set refs[name], append {+, name, T}, shard_version++.
    /// We REUSE Store::mutateShard (the verified Task-10 loop: re-read-inside-loop, size-guard,
    /// casPut, bounded retry). The gate-aware mutate lambda owns the gate; because mutateShard
    /// re-reads + re-invokes the lambda each attempt, a fence-advanced conflict naturally re-runs the
    /// gate on the fresh fence_round (spec §5 step 5).
    /// B170: captured inside the CAS lambda (which mutateShard re-runs per attempt — they reflect the
    /// COMMITTED attempt) to classify the publish event as RefPublish vs RefRepoint.
    bool repointed_over = false;
    uint64_t committed_at_version = 0;
    store->mutateShard(ns, store->shardOf(ref_name), [&](RootShard & root)
    {
        /// Fence vs view: if the manifest's fence_round (floored by the registry fence at
        /// registration, W-REGISTER) is ahead of our view, GC advanced — refresh BEFORE revalidating
        /// so the dependency set is checked against the freshest reality. This ordering (refresh, then
        /// revalidate) is load-bearing for W-REGISTER (the registry fence is the gate floor); keep it.
        if (store->retireView().round() < std::max(root.fence_round, registry_fence))
            store->retireView().refresh();

        /// B171 INV-COMMIT-FAILCLOSED (fail-closed commit): revalidate UNCONDITIONALLY — every commit
        /// re-proves the full closure is present (re-pin via recreate / observeAndAdmit; a missing
        /// non-recreatable blob → ABORTED, caller retries). This used to run only when the fence advanced
        /// past our view, which left a window: a precommit prematurely reclaimed could let the shared blob
        /// be collected, yet a stale-but-not-fence-advanced view would skip the presence check and commit a
        /// dangle. Running checkAndResolveDeps every time closes that window — a publish can NEVER commit a
        /// table ref over a missing dependency. checkAndResolveDeps is idempotent and re-runs naturally on
        /// each mutateShard CAS retry (spec §4.4, §4.6). It merges the former revalidateDeps +
        /// gateCheckDeps into a single pass (B190 Task 3).
        checkAndResolveDeps();

        repointed_over = root.refs.contains(ref_name);
        committed_at_version = root.shard_version + 1;
        root.refs[ref_name] = payload;
        /// at_version == the committed shard_version: mutateShard bumps AFTER the lambda, so inside it
        /// the post-commit version is root.shard_version + 1 (matches dropRef).
        root.journal.push_back(JournalRecord{
            .op = JournalRecord::Op::Add, .ref_name = ref_name, .tree_id = payload.tree_id,
            .at_version = root.shard_version + 1});
    });

    /// B170: the ref was published (RefRepoint when it already named a tree, else RefPublish). The
    /// at_version is the committed shard_version — the journal record GC will fold as a root Add.
    if (store->hasEventSink())
    {
        CasEvent _ev7;
        _ev7.type = repointed_over ? CasEventType::RefRepoint : CasEventType::RefPublish;
        _ev7.namespace_ = ns.string();
        _ev7.ref_name = ref_name;
        _ev7.object_kind = CasEventObjectKind::Tree;
        _ev7.object_hash = tree.string();
        _ev7.token = u128ToHex(build_id);
        _ev7.at_version = committed_at_version;
        _ev7.outcome = "ok";
        _ev7.reason = repointed_over ? "published over an existing ref (repoint)" : "published a new ref";
        store->emitEvent(_ev7);
    }

    /// B171: the table ref is now durably committed — the manifest's closure is table-pinned. Remove
    /// the precommit edge so GC's fold releases it. ORDER: the table ref Add committed FIRST
    /// (above), THEN the precommit is removed — never the reverse, which would leave a window with
    /// neither edge protecting the closure. A transient failure is SAFE to ignore: the commit already
    /// succeeded, so the closure is protected by the table ref; the leftover precommit is a stale
    /// precommit edge GC reclaims later (Task 4). We therefore catch/log/emit and continue — failing
    /// the publish here would be wrong (the part IS committed). Only attempt removal if this build
    /// actually precommitted (spec §B.2, design §4.4).
    ///
    /// NB: we drop via mutateShard on the precommit shard, mirroring how `precommit` wrote it (ref name
    /// = build_seq, shard = shardOf(ref)) — NOT via Store::dropRef, which routes by shardOf(ref_name) of
    /// the TABLE ref. The shard happens to be shardOf(build_seq) too, so this is the same routing, but we
    /// keep it explicit. Same CAS shape as dropRef: erase refs[build_seq] + append a Remove journal record
    /// (the journal Remove is what GC's fold reads to release edges).
    if (precommitted)
    {
        const String ref = buildRef();
        try
        {
            store->mutateShard(precommitNs(), buildShard(), [&](RootShard & root)
            {
                auto it = root.refs.find(ref);
                if (it == root.refs.end())
                    throw Exception(ErrorCodes::FILE_DOESNT_EXIST,
                        "remove precommit: no ref {} in precommit shard {}", ref, buildShard());
                const UInt128 part_tree = it->second.tree_id;
                root.refs.erase(it);
                root.journal.push_back(JournalRecord{
                    .op = JournalRecord::Op::Remove, .ref_name = ref, .tree_id = part_tree,
                    .at_version = root.shard_version + 1});
            });
            precommitted = false;
            if (store->hasEventSink())
            {
                CasEvent _evpr;
                _evpr.type = CasEventType::PrecommitRemoved;
                _evpr.namespace_ = precommitNs().string();
                _evpr.ref_name = ref;
                _evpr.object_kind = CasEventObjectKind::Tree;
                _evpr.object_hash = tree.string();
                _evpr.token = u128ToHex(build_id);
                _evpr.outcome = "removed";
                _evpr.reason = "fail-closed commit succeeded; closure now table-pinned, releasing the precommit edge";
                _evpr.detail = {{"build_seq", std::to_string(build_seq)}};
                store->emitEvent(_evpr);
            }
        }
        catch (const Exception & e)
        {
            /// Best-effort: the commit already succeeded, so leaving a stale precommit is benign (GC
            /// reclaims it). Do NOT propagate — that would fail an already-committed publish.
            if (store->hasEventSink())
            {
                CasEvent _evpr;
                _evpr.type = CasEventType::PrecommitRemoved;
                _evpr.namespace_ = precommitNs().string();
                _evpr.ref_name = ref;
                _evpr.object_kind = CasEventObjectKind::Tree;
                _evpr.object_hash = tree.string();
                _evpr.token = u128ToHex(build_id);
                _evpr.outcome = "deferred";
                _evpr.reason = "precommit removal failed transiently after a successful commit; left for GC reclaim: "
                    + e.message();
                _evpr.detail = {{"build_seq", std::to_string(build_seq)}};
                store->emitEvent(_evpr);
            }
        }
    }

    /// Published: this build is no longer in-flight. Retire its seq so the GC watermark floor
    /// (minActive) can advance past it (idempotent — the dtor also retires).
    store->retireBuildSeq(build_seq);
    /// B170: the build's terminal success — the part is committed.
    if (store->hasEventSink())
    {
        CasEvent _ev8;
        _ev8.type = CasEventType::BuildPublish;
        _ev8.namespace_ = ns.string();
        _ev8.ref_name = ref_name;
        _ev8.object_kind = CasEventObjectKind::Tree;
        _ev8.object_hash = tree.string();
        _ev8.token = u128ToHex(build_id);
        _ev8.at_version = committed_at_version;
        _ev8.outcome = "published";
        _ev8.reason = "build committed its root manifest; no longer in-flight";
        _ev8.detail = {{"build_seq", std::to_string(build_seq)}};
        store->emitEvent(_ev8);
    }
}

void Build::abandon()
{
    requireAlive();
    heartbeat->stopBackground();
    heartbeat->discard();
    /// No longer in-flight: retire the seq so the GC watermark floor can advance (idempotent).
    store->retireBuildSeq(build_seq);
    alive = false;
    /// B170: the build was abandoned (heartbeat discarded; its uploads become GC-reclaimable debris).
    if (store->hasEventSink())
    {
        CasEvent _ev9;
        _ev9.type = CasEventType::BuildAbort;
        _ev9.token = u128ToHex(build_id);
        _ev9.outcome = "abandoned";
        _ev9.reason = "abandon: heartbeat discarded; uploads become reclaimable debris";
        _ev9.detail = {{"build_seq", std::to_string(build_seq)}};
        store->emitEvent(_ev9);
    }
}

}
