#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasBuild.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasEnvelope.h>
#include <IO/WriteHelpers.h>
#include <Common/Exception.h>
#include <Common/thread_local_rng.h>
#include <base/defines.h>
#include <algorithm>
#include <chrono>

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

/// Two thread_local_rng draws composed into a UInt128. Used both to mint build ids and, per upload
/// AND per resurrect, a FRESH incarnation_tag (W-FRESH-TAG).
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

Build::Build(StorePtr store_, std::unique_ptr<HeartbeatKeeper> heartbeat_, UInt128 build_id_, BuildInfo info_)
    : store(std::move(store_))
    , heartbeat(std::move(heartbeat_))
    , build_id(build_id_)
    , info(std::move(info_))
{
}

Build::~Build()
{
    /// Crash semantics: the Build dtor takes no special action. If the build was not abandoned, the
    /// heartbeat keeper's own dtor stops its background thread WITHOUT discarding the key, so the
    /// uploads become debris that full GC reclaims under the heartbeat rules (spec §5). abandon() is
    /// the only path that proactively discards.
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
    const PoolMeta & meta = store->poolMeta();
    const PoolConfig & cfg = store->poolConfig();

    /// B136: bounded retry loop. The happy paths (fresh upload ⇒ Done; existing live incarnation ⇒
    /// adopt) execute exactly once. The loop only re-iterates on the dedup GC race: when our
    /// conditional upload hit PreconditionFailed (the content key already exists as a prior
    /// incarnation), observeAndAdmit ⇒ resurrect HEADs then GETs the key, and GC's exact-token
    /// content delete can land in that HEAD→GET window so the GET returns nothing and resurrect throws
    /// FILE_DOESNT_EXIST. We STILL HOLD the body (the BlobSource is re-invokable), so this is the
    /// spec's "bytes in hand ⇒ re-PUT" resurrect arm: re-run the fresh-upload path. The object is now
    /// absent ⇒ putIfAbsent creates it fresh under a new incarnation; or a racing writer re-created it
    /// ⇒ PreconditionFailed again ⇒ observeAndAdmit, which now succeeds. Bounded so pathological churn
    /// cannot spin forever; on exhaustion the last error propagates. Each attempt mints a FRESH
    /// incarnation_tag (W-FRESH-TAG) — never reusing the condemned token, so INV-NO-RETURN holds.
    constexpr int max_attempts = 8;
    for (int attempt = 0; attempt < max_attempts; ++attempt)
    {
        /// W-FRESH-TAG: a fresh incarnation_tag for this upload attempt.
        EnvelopeHeader header;
        header.kind = ObjectKind::Blob;
        header.hash_algo = 1;
        header.logical_size = source.size;
        header.logical_hash = logical_hash;
        header.domain_id = meta.pool_id;
        header.incarnation_tag = mintU128();
        header.build_id = build_id;
        header.provenance = Provenance{nowMs(), cfg.server_id, /*ch_version*/ 0, info.op};
        header.intended_ref = info.intended_ref;
        header.pad_to_header_len = static_cast<uint32_t>(meta.blob_header_len);

        String head_bytes;
        try
        {
            head_bytes = encodeEnvelopeHeader(header);
        }
        catch (const Exception & e)
        {
            if (e.code() != ErrorCodes::BAD_ARGUMENTS)
                throw;
            /// intended_ref is diagnostic-only: when it makes the natural header exceed the pool's fixed
            /// blob_header_len, drop it (never grow the fixed blob header) and re-encode.
            header.intended_ref.reset();
            head_bytes = encodeEnvelopeHeader(header);
        }

        WriteSinkPtr sink = store->backend().putIfAbsentStream(key);
        writeString(head_bytes, sink->buffer());
        const size_t before = sink->buffer().count();
        source.write_payload(sink->buffer());
        const size_t written = sink->buffer().count() - before;
        if (written != source.size)
        {
            /// Fail-closed: the envelope's logical_size would lie. Cancel the stream — the key is never
            /// created by a cancelled sink — and never publish a truncation-undetectable object.
            sink->cancel();
            throw Exception(ErrorCodes::LOGICAL_ERROR,
                "putBlob: source wrote {} bytes, declared {}", written, source.size);
        }

        Token tok;
        const PutOutcome outcome = sink->finalize(&tok);
        if (outcome == PutOutcome::Done)
        {
            deps[{static_cast<uint8_t>(ObjectKind::Blob), logical_hash}] =
                DepEntry{ObjectKind::Blob, tok, store->retireView().round(), source.size};
            return BlobRef{id, source.size};
        }

        /// PreconditionFailed ⇒ the key already exists (another writer's incarnation). The body shipped is
        /// harmless: the dedup saving is the skipped INCARNATION, not the bytes. Apply the cold-reuse rule.
        try
        {
            const uint64_t admitted = observeAndAdmit(ObjectKind::Blob, logical_hash, key);
            return BlobRef{id, admitted};
        }
        catch (const Exception & e)
        {
            /// B136/B137: the condemned incarnation vanished mid-dedup (GC's exact-token delete in the
            /// HEAD→GET window). We hold the body — re-run the fresh-upload path instead of failing. The
            /// vanish surfaces two ways: FILE_DOESNT_EXIST from observeAndAdmit's HEAD-absent check (the
            /// object was gone before resurrect's GET) and ABORTED from resurrect's own vanish (gone in
            /// the GET window, B137) — both mean "object vanished ⇒ re-upload the held body". Only this
            /// writer-with-a-body site may recover; the shared resurrect stays retryable-but-fail-closed
            /// for bodyless callers (revalidateDeps / gateCheckDeps), which propagate the ABORTED. Any
            /// other error propagates; on the last attempt rethrow so pathological churn surfaces rather
            /// than spinning silently.
            const bool vanished = e.code() == ErrorCodes::FILE_DOESNT_EXIST || e.code() == ErrorCodes::ABORTED;
            if (!vanished || attempt + 1 == max_attempts)
                throw;
        }
    }

    /// Unreachable: the loop either returns or rethrows on the final attempt.
    throw Exception(ErrorCodes::LOGICAL_ERROR, "putBlob: exhausted retries for {}", key);
}

BlobRef Build::reuseBlob(const BlobId & id)
{
    requireAlive();
    const UInt128 logical_hash = hexToU128(id.string());
    const String key = store->layout().blobKey(id);
    uint64_t admitted;
    try
    {
        admitted = observeAndAdmit(ObjectKind::Blob, logical_hash, key);
    }
    catch (const Exception & e)
    {
        /// B156: reuseBlob has NO body in hand (cold reuse of a blob staged earlier in this same
        /// transaction). If observeAndAdmit finds it HEAD-absent, GC's exact-token deleteExact landed
        /// between the reuse decision and our HEAD (e.g. the in-flight build's heartbeat lapsed under
        /// load and GC condemned+deleted it). This is a BENIGN race — the blob DID exist (it was
        /// putBlob'd in this transaction); surface it as retryable ABORTED so the caller retries (the
        /// retry re-uploads the content via putBlob from source), mirroring resurrect's GET-vanish.
        /// NOT a hard FILE_DOESNT_EXIST — that code is load-bearing for putBlob's body-holding retry
        /// and adoptTree's fail-closed never-existed semantics (both deliberately untouched).
        if (e.code() == ErrorCodes::FILE_DOESNT_EXIST)
            throw Exception(ErrorCodes::ABORTED,
                "Build::reuseBlob: blob {} vanished (GC-deleted between reuse decision and HEAD); retry the operation", key);
        throw;
    }
    return BlobRef{id, admitted};
}

uint64_t Build::observeAndAdmit(ObjectKind kind, const UInt128 & hash, const String & key)
{
    const HeadResult hr = store->backend().head(key);
    if (!hr.exists)
        throw Exception(ErrorCodes::FILE_DOESNT_EXIST,
            "Build: object {} absent — cannot reuse (caller must upload it)", key);

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

    if (store->retireView().isCondemnedToken(kind, hash, hr.token))
    {
        resurrect(kind, hash, key);
        /// resurrect recorded the dep with the new token; the admitted logical size is bookkeeping for
        /// GC (M-C3) and not load-bearing in M-C2 — report the logical size where cheap (Blob only).
        return logical_size;
    }

    /// Adopt the current incarnation — free, no bytes moved.
    deps[{static_cast<uint8_t>(kind), hash}] =
        DepEntry{kind, hr.token, store->retireView().round(), logical_size};
    return logical_size;
}

void Build::resurrect(ObjectKind kind, const UInt128 & hash, const String & key)
{
    const std::optional<GetResult> got = store->backend().get(key);
    if (!got)
        /// resurrect is ONLY ever entered on a CONDEMNED (kind, hash), so a vanish here is ALWAYS the
        /// benign GC race: exact-token deleteExact landed in the HEAD->GET window. This is the same
        /// retryable lost-dependency situation as the sibling branches below (revalidateDeps /
        /// gateCheckDeps), so surface it as ABORTED "retry the operation" — NOT a hard FILE_DOESNT_EXIST
        /// (which became an HTTP-500 INSERT failure on the bodyless publish path, B137). A
        /// genuinely-never-existed dep is a SEPARATE FILE_DOESNT_EXIST at observeAndAdmit (HEAD-absent),
        /// which is untouched. putBlob's body-holding retry catches this ABORTED and re-uploads in hand.
        throw Exception(ErrorCodes::ABORTED,
            "Build::resurrect: condemned incarnation of {} deleted by GC between HEAD and GET; retry the operation", key);

    EnvelopeHeader header = decodeEnvelopeHeader(got->bytes, got->bytes.size(), kind);

    /// FOLLOW-UP(M-F): for huge blobs, server-side multipart copy-to-self (spec §5 step 2) instead of
    /// GET+PUT — copy preserving the bytes, rewriting only the incarnation header.
    /// W-FRESH-TAG on resurrect: a fresh incarnation_tag forces a distinct body so the condemned
    /// incarnation can never be re-derived; everything else (header_len, provenance, ...) is preserved.
    header.incarnation_tag = mintU128();
    header.pad_to_header_len = header.header_len;   /// preserve the exact header length on re-encode
    const String new_head = encodeEnvelopeHeader(header);
    const String body = new_head + got->bytes.substr(header.header_len);

    Token new_tok;
    const PutOutcome oc = store->backend().putOverwrite(key, body, got->token, &new_tok);
    if (oc == PutOutcome::PreconditionFailed)
    {
        /// A racing writer displaced the incarnation we read (their resurrect won). Re-observe: their
        /// fresh incarnation is adoptable unless it too is condemned. observeAndAdmit records the dep.
        observeAndAdmit(kind, hash, key);
        return;
    }

    deps[{static_cast<uint8_t>(kind), hash}] =
        DepEntry{kind, new_tok, store->retireView().round(),
            kind == ObjectKind::Blob ? got->bytes.size() - store->poolMeta().blob_header_len : 0};
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

        /// W-EVIDENCE: record a TOKENLESS dependency — liveness evidence is the live source root, not a
        /// token. Inline entries reference no standalone object, so they record nothing.
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
    /// re-attach); condemned => resurrect (the cold-reuse rule, same as reuseBlob).
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

TreeId Build::putTree(std::vector<TreeEntry> entries)
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
                        "putTree: child blob {} not in dependency set (W-TREE-BUILD)",
                        u128ToHex(entry.file_hash));
                break;
            case Placement::Subtree:
                if (!deps.contains({static_cast<uint8_t>(ObjectKind::Tree), entry.file_hash}))
                    throw Exception(ErrorCodes::LOGICAL_ERROR,
                        "putTree: child tree {} not in dependency set (W-TREE-BUILD)",
                        u128ToHex(entry.file_hash));
                break;
            case Placement::PackSlice:
                if (!deps.contains({static_cast<uint8_t>(ObjectKind::Pack), entry.pack_hash}))
                    throw Exception(ErrorCodes::LOGICAL_ERROR,
                        "putTree: child pack {} not in dependency set (W-TREE-BUILD)",
                        u128ToHex(entry.pack_hash));
                break;
            case Placement::Inline:
                break;   /// inline bytes need no dependency
        }
    }

    const String encoded = encodeTree(std::move(entries));   /// canonical sort + duplicate-name check
    const TreeId id = treeIdFor(encoded);
    const UInt128 logical_hash = hexToU128(id.string());
    const String key = store->layout().treeKey(id);
    const PoolMeta & meta = store->poolMeta();
    const PoolConfig & cfg = store->poolConfig();

    /// Tree envelope: NATURAL header length (no pad). W-FRESH-TAG: fresh incarnation_tag.
    EnvelopeHeader header;
    header.kind = ObjectKind::Tree;
    header.hash_algo = 1;
    header.logical_size = encoded.size();
    header.logical_hash = logical_hash;
    header.domain_id = meta.pool_id;
    header.incarnation_tag = mintU128();
    header.build_id = build_id;
    header.provenance = Provenance{nowMs(), cfg.server_id, /*ch_version*/ 0, info.op};
    /// intended_ref deliberately omitted from tree envelopes (forensics live on the published blob set).
    const String head_bytes = encodeEnvelopeHeader(header);

    WriteSinkPtr sink = store->backend().putIfAbsentStream(key);
    writeString(head_bytes, sink->buffer());
    writeString(encoded, sink->buffer());
    Token tok;
    const PutOutcome outcome = sink->finalize(&tok);
    if (outcome == PutOutcome::Done)
    {
        deps[{static_cast<uint8_t>(ObjectKind::Tree), logical_hash}] =
            DepEntry{ObjectKind::Tree, tok, store->retireView().round(), encoded.size()};
    }
    else
    {
        /// The identical tree already exists (a just-dropped identical tree may be condemned): apply
        /// the cold-reuse rule, which adopts or resurrects and records the Tree dep.
        observeAndAdmit(ObjectKind::Tree, logical_hash, key);
    }

    /// RETAIN the encoded payload: trees are always re-creatable during the gate's W-REVALIDATE
    /// (Task 13), unlike blob payloads which are not retained.
    retained_trees[logical_hash] = encoded;
    return id;
}

String Build::keyFor(ObjectKind kind, const UInt128 & hash) const
{
    return objectKey(store->layout(), kind, hash);
}

void Build::gateCheckDeps()
{
    /// Iterating `deps` while resurrect/observeAndAdmit MUTATE deps is safe: both OVERWRITE the SAME
    /// (kind, hash) entry (deps[thatkey].token), never insert a new key, so the iterator we hold and
    /// the map structure stay valid. (We resolve via the loop's own key.)
    for (auto & [key, dep] : deps)
    {
        /// Iteration-safety invariant, made self-enforcing: the DepKey's kind byte (key.first) matches
        /// dep.kind, so deps[{kind, hash}] inside resurrect/observeAndAdmit overwrites the SAME entry
        /// being iterated — never inserts a new key that would invalidate the loop.
        chassert(static_cast<uint8_t>(dep.kind) == key.first);

        const ObjectKind kind = dep.kind;
        const UInt128 & hash = key.second;

        if (dep.token.has_value())
        {
            /// Token-bearing: ANY view hit BY HASH ⇒ resurrect/recreate (spec §5 step 5: "members with a
            /// view hit ⇒ resurrect" — a view hit is by HASH, not by our exact observed token). The
            /// current incarnation may have been displaced to a DIFFERENT token t' that GC then condemned;
            /// keeping our stale token while the object lives at the condemned t' would let an in-flight
            /// DELETE If-Match:t' (zombie/duplicate GC delete) hit the live object ⇒ dangling ref. The §8
            /// R4 spared-orphan is safe only because the orphan's token was DISPLACED by a resurrect first;
            /// resurrect/recreate on any hit performs exactly that displacement (a fresh tag never reuses
            /// an old token, INV-NO-RETURN), closing the window.
            if (store->retireView().findCondemned(kind, hash).has_value())
            {
                if (kind == ObjectKind::Tree && retained_trees.contains(hash))
                    recreateTree(hash);
                else
                    resurrect(kind, hash, keyFor(kind, hash));
            }
        }
        else
        {
            /// Tokenless evidence (W-EVIDENCE): any condemned token for (kind, hash) ⇒ resolve the HEAD
            /// via observeAndAdmit (adopt the live incarnation, or resurrect if it too is condemned).
            /// This turns the dep token-bearing.
            if (store->retireView().findCondemned(kind, hash).has_value())
                observeAndAdmit(kind, hash, keyFor(kind, hash));
        }
    }
}

void Build::recreateTree(const UInt128 & hash)
{
    /// W-REVALIDATE re-create branch. The encoded payload was RETAINED at putTree, so a tree is always
    /// re-creatable: re-upload it as a FRESH incarnation. W-FRESH-TAG: a fresh incarnation_tag.
    const auto retained_it = retained_trees.find(hash);
    if (retained_it == retained_trees.end())
        throw Exception(ErrorCodes::LOGICAL_ERROR,
            "Build::recreateTree: no retained payload for tree {} (only built trees are re-creatable)",
            u128ToHex(hash));

    const String & encoded = retained_it->second;
    const TreeId id{u128ToHex(hash)};
    const String key = store->layout().treeKey(id);
    const PoolMeta & meta = store->poolMeta();
    const PoolConfig & cfg = store->poolConfig();

    EnvelopeHeader header;
    header.kind = ObjectKind::Tree;
    header.hash_algo = 1;
    header.logical_size = encoded.size();
    header.logical_hash = hash;
    header.domain_id = meta.pool_id;
    header.incarnation_tag = mintU128();
    header.build_id = build_id;
    header.provenance = Provenance{nowMs(), cfg.server_id, /*ch_version*/ 0, info.op};
    const String head_bytes = encodeEnvelopeHeader(header);

    WriteSinkPtr sink = store->backend().putIfAbsentStream(key);
    writeString(head_bytes, sink->buffer());
    writeString(encoded, sink->buffer());
    Token tok;
    const PutOutcome outcome = sink->finalize(&tok);
    if (outcome == PutOutcome::Done)
    {
        deps[{static_cast<uint8_t>(ObjectKind::Tree), hash}] =
            DepEntry{ObjectKind::Tree, tok, store->retireView().round(), encoded.size()};
        return;
    }

    /// PreconditionFailed ⇒ a concurrent writer re-created it first. Apply the cold-reuse rule against
    /// the current incarnation (adopt it, or resurrect if it too is condemned). observeAndAdmit records
    /// the dep with the current round.
    observeAndAdmit(ObjectKind::Tree, hash, key);
}

void Build::revalidateDeps()
{
    /// W-REVALIDATE (the model's `WPublishReval`). Iterating `deps` while resurrect / observeAndAdmit /
    /// recreateTree MUTATE deps is safe for the same reason as gateCheckDeps: re-create keeps the SAME
    /// (kind, hash) — a tree's hash commits to its content, so its re-uploaded incarnation lives at the
    /// identical key — and resurrect/observeAndAdmit OVERWRITE deps[k], never insert a new key. So the
    /// iterator and the map structure stay valid.
    for (auto & [key, dep] : deps)
    {
        const ObjectKind kind = dep.kind;
        const UInt128 & hash = key.second;

        const String k = keyFor(kind, hash);
        const std::optional<std::vector<Token>> hits = store->retireView().findCondemned(kind, hash);

        /// Tokenless evidence (W-EVIDENCE) with a view HIT is resolved by gateCheckDeps. But a
        /// stale evidence member with NO hit must be RE-OBSERVED here, exactly like a stale token:
        /// retire entries DROP on confirmed outcomes (F1), so "no entry" can mean "the object was
        /// condemned, deleted, and its entry dropped" - the durable witness is the OBJECT, not the
        /// view. Evidence staleness = the view round advanced past the round the evidence was
        /// recorded at (the live-source-root argument only covers the fence/recheck handshake of
        /// the SAME round window; a full round boundary in between invalidates it - the source may
        /// have dropped and the object been reclaimed). Discovered by the Task-9 integration test:
        /// without this, an evidence dep on a deleted tree sails through a refreshed gate and the
        /// publish DANGLES (the model's DepOK requires present[h] for evidence deps too).
        if (!dep.token.has_value())
        {
            if (hits.has_value())
                continue;   /// gateCheckDeps resolves the hit (W-EVIDENCE)
            if (dep.observed_view_round >= store->retireView().round())
                continue;   /// evidence as fresh as the view - the handshake covers it
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
                /// Present: resolve the stale evidence to a TOKEN-BEARING entry observed at the
                /// current round (adopt the current token, or resurrect if it is condemned).
                observeAndAdmit(kind, hash, k);
            }
            continue;
        }

        if (hits.has_value())
        {
            /// ANY view hit BY HASH ⇒ resurrect (blob) or re-create from the retained payload (tree).
            /// Spec §5 step 5: "members with a view hit ⇒ resurrect" — a view hit is by HASH, NOT by our
            /// exact observed token. The current incarnation may be condemned at its CURRENT token (a
            /// prior writer displaced t → t', GC then condemned t'); displacing it (a fresh tag) makes any
            /// in-flight DELETE If-Match:<current> 412, closing the §8 R4 spared-orphan window. Resurrect
            /// on any hit is the conservative, model-faithful (WResurrect) choice (INV-NO-RETURN: a fresh
            /// tag never reuses an old token), and it covers the §7 step-4 "held entry ⇒ resurrect" horn.
            if (kind == ObjectKind::Tree && retained_trees.contains(hash))
                recreateTree(hash);
            else
                resurrect(kind, hash, k);
        }
        else if (dep.observed_view_round < store->retireView().round())
        {
            /// No entry for the hash (the `if` above consumed every view hit) AND stale-observed ⇒ the
            /// W-REVALIDATE single re-observation (one HEAD). We re-observe ONLY members observed under an
            /// older round; a member already observed at this round needs nothing (the else-keep below).
            const HeadResult hr = store->backend().head(k);
            if (!hr.exists)
            {
                /// deleted / absent in the refreshed reality.
                if (kind == ObjectKind::Tree && retained_trees.contains(hash))
                    recreateTree(hash);   /// spec §7 step 4: deleted/absent ⇒ re-create
                else
                    /// A blob payload is not retained — fail closed and retryable. Do NOT fabricate a
                    /// dangling reference (INV-NO-DANGLE); the caller re-runs the operation, which
                    /// re-uploads the blob bytes from source.
                    throw Exception(ErrorCodes::ABORTED,
                        "publish dependency {} lost and not re-creatable; retry the operation",
                        u128ToHex(hash));
            }
            else if (hr.token == *dep.token)
            {
                /// current == observed ⇒ KEEP — safe by the IN-FLIGHT DISJUNCTION (model-checked): a
                /// delete in flight for (hash, t) implies its retire entry is still HELD, or t was
                /// already DISPLACED (current ≠ t, forced through a gate-mandated resurrect). A delete
                /// for the CURRENT token therefore implies a held entry — which would be a VIEW HIT, and
                /// we are in the no-hit branch. So no-hit + current==observed is publishable. We only
                /// re-stamp observed_view_round so a later same-publish recheck treats it as fresh.
                dep.observed_view_round = store->retireView().round();
            }
            else
            {
                /// current token differs ⇒ treat as a FRESH cold reuse of the current token (spec §7
                /// step 4: replaced ⇒ adopt the newer token, or resurrect if it too is condemned).
                observeAndAdmit(kind, hash, k);
            }
        }
        /// else: already valid at this view round — either observed_view_round >= round (re-observed /
        /// uploaded under the current view), or condemned-but-not-at-our-token (a different incarnation's
        /// entry, irrelevant to us). KEEP.
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

    /// W-REGISTER (spec §5, decision 2026-06-12): a namespace must be in roots/_registry BEFORE its
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
    store->mutateShard(ns, store->shardOf(ref_name), [&](RootShard & root)
    {
        /// Fence vs view: if the manifest's fence_round (floored by the registry fence at
        /// registration, W-REGISTER) is ahead of our view, GC advanced — refresh.
        if (store->retireView().round() < std::max(root.fence_round, registry_fence))
        {
            store->retireView().refresh();
            /// W-REVALIDATE: a fence-advanced refresh invalidates every stale token observation; re-validate
            /// the whole dependency set BEFORE the gate scan (spec §5 step 5, §7 step 4).
            revalidateDeps();
        }

        gateCheckDeps();

        root.refs[ref_name] = payload;
        /// at_version == the committed shard_version: mutateShard bumps AFTER the lambda, so inside it
        /// the post-commit version is root.shard_version + 1 (matches dropRef).
        root.journal.push_back(JournalRecord{
            .op = JournalRecord::Op::Add, .ref_name = ref_name, .tree_id = payload.tree_id,
            .at_version = root.shard_version + 1});
    });
}

void Build::abandon()
{
    requireAlive();
    heartbeat->stopBackground();
    heartbeat->discard();
    alive = false;
}

}
