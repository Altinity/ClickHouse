#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Backend/CasBackend.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Gc/CasBlobInDegree.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Tools/CasFsck.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Formats/CasGcStateFormat.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Formats/CasFoldSealFormat.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Formats/CasTextFormat.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Formats/CasLayout.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Formats/CasPartManifestFormat.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Primitives/CasTypes.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Gc/CasOrphanManifestSweep.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasRefProtocol.h>

#include <Common/Exception.h>
#include <Common/HashTable/Hash.h>

#include <algorithm>
#include <set>
#include <unordered_map>
#include <unordered_set>

namespace DB
{
namespace ErrorCodes
{
    extern const int TIMEOUT_EXCEEDED;
}
}

namespace DB::Cas
{

namespace
{
constexpr uint64_t PROGRESS_PAGES = 16;

using Deadline = std::optional<std::chrono::steady_clock::time_point>;

/// Enforce the optional overall scan deadline between backend operations. A timeout is propagated as
/// `TIMEOUT_EXCEEDED`; the public `runFsck` wrapper may convert that exception into a partial report when
/// explicitly requested.
void checkDeadline(const Deadline & deadline, std::string_view phase)
{
    if (deadline && std::chrono::steady_clock::now() > *deadline)
        throw Exception(ErrorCodes::TIMEOUT_EXCEEDED,
            "fsck: exceeded the deadline during '{}' — run against a QUIESCED pool or raise --timeout.", phase);
}

void listAll(Backend & backend, const String & prefix, std::unordered_map<String, uint64_t> & out,
             const FsckProgress & on_progress, const Deadline & deadline, std::string_view phase)
{
    static constexpr size_t kPageLimit = 1000;
    uint64_t pages = 0;
    size_t count_in_page = 0;
    forEachListedKey(backend, prefix, [&](const ListedKey & k)
    {
        out[k.key] = k.size;
        if (++count_in_page == kPageLimit)
        {
            count_in_page = 0;
            ++pages;
            checkDeadline(deadline, phase);
            if (on_progress && pages % PROGRESS_PAGES == 0)
                on_progress(phase, out.size(), pages);
        }
    }, kPageLimit);
    /// The walk's `backend.list` lands at least once even for an empty/undersized final page --
    /// check it here, mirroring the original per-page loop (deadline checked after every physical page).
    if (count_in_page > 0 || pages == 0)
    {
        ++pages;
        checkDeadline(deadline, phase);
    }
    if (on_progress)
        on_progress(phase, out.size(), pages);
}

/// Parse (writer_epoch, build_sequence) from a manifest object key. Delegates to the one shared
/// `Layout::parseManifestKey` instead of hand-rolling a second parser; returns false on a
/// malformed or foreign key.
bool parseBuildPrefix(const Layout & layout, const String & key, BuildPrefix & out)
{
    const auto parsed = layout.parseManifestKey(key);
    if (!parsed)
        return false;
    out.writer_epoch = parsed->ref.writer_epoch;
    out.build_sequence = parsed->ref.build_sequence;
    return true;
}

/// The ref-walk (which builds `reachable_blobs`/`blob_labels`) and the HEAD-confirm below run minutes
/// apart with no snapshot between them. A ref that gets republished (now names a
/// different manifest) or DROPPED in that window, combined with a legitimate GC delete of the OLD
/// blob, makes the stale walk look like a genuine dangle (a "phantom dangling") — this made the fsck
/// oracle dishonest and falsely report a dangle during long-running validation.
///
/// Before counting a HEAD-absent blob as `Dangling`, re-resolve every `"ns/ref"` label that named it
/// freshly (`resolveRef` with `allow_stale=false`, never the walk's cached/stale view) and check whether
/// the CURRENT manifest still lists this blob as a `Blob` entry. `label` is split on the LAST '/' —
/// mirroring exactly how the walk built it (`ns_str + "/" + ref_name`): `ref_name` never contains '/',
/// but `ns_str` may, so the join separator is always the rightmost one.
///
/// Fails CLOSED on any ambiguity (a malformed label, a dropped-then-recreated ref that throws, a
/// corrupt current manifest): treated as "still referenced", i.e. the original conservative verdict.
/// The fix can only SHRINK false positives — it must never hide a real one.
bool blobStillReferenced(Pool & store, const Layout & layout,
                          const String & bkey, const std::vector<String> & labels, const Deadline & deadline)
{
    if (labels.empty())
        return true;
    for (const String & label : labels)
    {
        checkDeadline(deadline, "re-resolving refs at HEAD-absent");
        const size_t slash = label.rfind('/');
        if (slash == String::npos)
            return true;   /// malformed label — cannot re-resolve, fail closed
        const String ns_part = label.substr(0, slash);
        const String ref_name = label.substr(slash + 1);
        try
        {
            /// Read freshly via `recoverRefTable` (a full LIST + replay), not `store.resolveRef`: the
            /// mounted Pool caches its `RefTableState` and never re-recovers it, so a concurrent external
            /// ref write is invisible to the cache. The recovery equation sees every log.
            const RootNamespace rns{ns_part};
            const RefTableState table = recoverRefTable(store.backend(), layout, rns);
            const auto rit = table.committed.find(ref_name);
            if (rit == table.committed.end())
                continue;   /// the ref was DROPPED since the walk — this label no longer applies
            const PartManifest body = store.readManifest(ManifestId{rns, rit->second.manifest_ref});
            for (const ManifestEntry & e : body.entries)
            {
                if (e.placement != EntryPlacement::Blob)
                    continue;
                if (layout.blobKey(e.ref) == bkey)
                    return true;   /// a CURRENT ref still names this exact blob — a real dangle
            }
        }
        catch (...)
        {
            return true;   /// cannot confirm the ref moved away — keep the conservative verdict
        }
    }
    return false;   /// every label re-resolved away (re-published or dropped) — a stale-walk artifact
}

/// For the newest published snapshot `X` of `ns`, independently reconstruct
/// the table state AT `X` from an EARLIER base (the greatest snapshot below `X`, or the empty state) plus
/// the surviving logs in `(base, X]`, re-encode it deterministically, and byte-compare to the published
/// `X` object. Byte-determinism of the codec (canonical sort, verified in the codec's own gtests) makes
/// the comparison exact.
///
/// The check runs ONLY when every log needed for the independent reconstruction still survives. `X` reuses
/// its last covered log's id, so `X` itself is a log id; once `GC` folds and covers those logs it deletes
/// them, after which there is nothing independent left to replay from -- such a table is SKIPPED, never
/// failed. A selected object that vanishes mid-check (a concurrent `GC` cleanup published a covering newer
/// snapshot) is likewise a skip, exactly like recovery's restart-on-vanish -- never corruption. This
/// oracle therefore validates freshly-published snapshots and any table whose `GC` cleanup is still
/// behind; it uses only the shared `replay`/`snapshotOf` state machine, never a second copy.
void checkSnapshotOracle(Backend & backend, const Layout & layout, const RootNamespace & ns,
                         bool detail, const Deadline & deadline, FsckReport & report)
{
    checkDeadline(deadline, "snapshot oracle");

    /// One LIST of the table prefix; gather snapshot and log ids.
    std::vector<RefTxnId> snap_ids;
    std::vector<RefTxnId> log_ids;
    forEachListedKey(backend, layout.refsNamespacePrefix(ns), [&](const ListedKey & lk)
    {
        const auto parsed = layout.parseRefObjectKey(lk.key);
        if (!parsed || parsed->ns != ns)
            return;
        if (parsed->kind == RefObjectKind::Snap)
            snap_ids.push_back(parsed->txn_id);
        else if (parsed->kind == RefObjectKind::Log)
            log_ids.push_back(parsed->txn_id);
    });
    if (snap_ids.empty())
        return;   /// no published snapshot: recovering from logs alone is valid, nothing to oracle
    std::sort(snap_ids.begin(), snap_ids.end());
    std::sort(log_ids.begin(), log_ids.end());

    const RefTxnId snapshot_x = snap_ids.back();
    const auto got_x = backend.get(layout.refSnapshotKey(ns, snapshot_x));
    if (!got_x)
        return;   /// vanished (a covering newer snapshot superseded it): restart-on-vanish -> skip

    /// X reuses its last covered log's id, so X is itself a log id. Once GC covers and cleans that log
    /// (the steady state on a caught-up pool), the state AT X cannot be independently reconstructed from
    /// logs -- skip, never fail. This also guards against replaying an empty tail into a {0,0} state.
    if (!std::binary_search(log_ids.begin(), log_ids.end(), snapshot_x))
        return;

    /// Independent base: the greatest snapshot strictly below X we can still fetch, else the empty state.
    std::optional<RefTableSnapshot> base;
    RefTxnId base_id{};   /// {0,0} = empty base
    for (auto it = snap_ids.rbegin(); it != snap_ids.rend(); ++it)
    {
        if (!(*it < snapshot_x))
            continue;   /// X itself or anything not strictly below it
        const auto got_base = backend.get(layout.refSnapshotKey(ns, *it));
        if (!got_base)
            continue;   /// vanished; try the next older snapshot
        base = decodeRefTableSnapshot(openObject(FormatId::RefSnapshot, got_base->bytes), ns.string(), *it);
        base_id = *it;
        break;
    }

    /// Logs in (base_id, X]. X reuses its last log's id, so X itself is a log id here: if its log -- or any
    /// covered log above the base -- was already cleaned, the independent reconstruction is unavailable.
    std::vector<RefLogTxn> tail;
    for (const RefTxnId & id : log_ids)
    {
        if (!(base_id < id))
            continue;   /// <= base: already folded into the base snapshot
        if (snapshot_x < id)
            continue;   /// > X: not part of the state AT X
        checkDeadline(deadline, "snapshot oracle");
        const auto got_log = backend.get(layout.refLogKey(ns, id));
        if (!got_log)
            return;   /// a covered log was cleaned/vanished: oracle unavailable for X -> skip, not error
        tail.push_back(decodeRefLogTxn(openObject(FormatId::RefLog, got_log->bytes), ns.string(), id));
    }

    /// Reconstruct the state AT X and re-encode. `replay` revalidates the base snapshot in full and applies
    /// the tail through the SAME state machine the writer used; the last applied id is X, so `snapshotOf`
    /// yields a snapshot with id X whose bytes must equal the published object.
    const RefTableState reconstructed = replay(base, tail);
    /// `recomputed` is canonical TEXT; the stored object is Always/`.zst`, so compare against the
    /// DECOMPRESSED stored bytes (zstd byte-determinism is not relied on here -- the canonical text is).
    const String recomputed = encodeRefTableSnapshot(snapshotOf(reconstructed, ns.string()));
    ++report.snapshot_oracle_checked;
    if (recomputed != openObject(FormatId::RefSnapshot, got_x->bytes))
    {
        ++report.snapshot_oracle_mismatches;
        FsckObject o;
        o.key = layout.refSnapshotKey(ns, snapshot_x);
        o.kind = ObjectKind::Blob;   /// snapshots have no ObjectKind; reuse Blob as the generic kind
        o.size = got_x->bytes.size();
        o.cls = FsckClass::SnapshotOracleMismatch;
        if (detail)
            o.reachable_from = {"published snapshot bytes diverge from an independent replay of its logs "
                                "(writer cache or codec corruption)"};
        report.objects.push_back(std::move(o));
    }
}

/// Perform the scan and accumulate into `report`. This helper owns the read-only traversal: it first
/// recovers authoritative refs, then checks physical objects and GC labels, while preserving the
/// distinction between a missing live object and expected in-flight cleanup. Deadline exceptions are
/// intentionally left to `runFsck`, which decides whether partial results were requested.
void runFsckImpl(Pool & store, bool detail, const FsckProgress & on_progress, const Deadline & deadline,
                  const String & namespace_prefix, FsckReport & report)
{
    const Layout & layout = store.layout();
    Backend & backend = store.backend();
    /// Path-derived per-object algorithm parsing: every listed blob-tree key -- across every
    /// admitted algo, not just the pool's node-local write algo -- is classified via
    /// `Layout::parseBlobKey`, which derives the `BlobRef` from the key's OWN `<algo>` path segment
    /// (and its `.meta` sibling). A foreign/malformed key (unknown algo segment, wrong-width hex, a
    /// non-`.meta`/non-blob shape) parses to `std::nullopt` and is classified as debris, never an
    /// exception.

    /// Reachability is recomputed from the authoritative refs (never from GC state):
    /// for each namespace, each committed ref resolves to a ManifestId; read its body; a committed ref
    /// naming a MISSING body is an ERROR (Dangling); a present body whose blobs are missing is an ERROR.
    std::set<String> reachable_blobs;        /// blob object keys named by a live owner
    std::set<String> owned_manifest_keys;    /// manifest object keys named by a committed owner
    /// blob key -> "ns/ref" labels of the refs that named it. Always populated (not just under
    /// `detail`) — the HEAD-absent re-resolve below needs it in every mode.
    std::unordered_map<String, std::vector<String>> blob_labels;

    uint64_t refs_walked = 0;
    for (const String & ns_str : store.listNamespaces(namespace_prefix))
    {
        const RootNamespace ns{ns_str};
        /// Fresh recovery (LIST + replay), not the mounted Pool's cached `listRefs`: fsck is a read-only
        /// audit that must see the authoritative durable ref state, including any external write.
        const RefTableState table = recoverRefTable(backend, layout, ns);
        /// Snapshot integrity oracle: verify this table's newest published
        /// snapshot is byte-identical to an independent replay of its own logs. Fails closed (records a
        /// mismatch, making the report not `clean()`) on a genuine divergence; skips silently when the
        /// covered logs were already cleaned or an object vanished mid-check.
        checkSnapshotOracle(backend, layout, ns, detail, deadline, report);
        for (const auto [ref_name, row] : table.committed)
        {
            const ManifestId id{ns, row.manifest_ref};
            const String mkey = layout.manifestKey(id);
            owned_manifest_keys.insert(mkey);
            const String label = ns_str + "/" + ref_name;

            const auto got = backend.get(mkey);
            if (!got)
            {
                /// A committed ref naming a missing manifest body — INV-NO-DANGLE surfaced (error).
                ++report.dangling;
                FsckObject o;
                o.key = mkey;
                o.kind = ObjectKind::Blob;   /// manifests have no ObjectKind; reuse Blob as the generic kind
                o.size = 0;
                o.cls = FsckClass::Dangling;
                o.reachable_from = {label};
                report.objects.push_back(std::move(o));
                ++refs_walked;
                continue;
            }

            PartManifest body = decodePartManifest(openObject(FormatId::PartManifest, got->bytes));
            if (!refMatchesBody(id.ref, body) || !manifestNamespaceMatches(id.root_namespace, body))
            {
                ++report.dangling;
                FsckObject o;
                o.key = mkey;
                o.kind = ObjectKind::Blob;
                o.size = got->bytes.size();
                o.cls = FsckClass::Dangling;
                o.reachable_from = {label};
                report.objects.push_back(std::move(o));
                ++refs_walked;
                continue;
            }

            for (const ManifestEntry & e : body.entries)
            {
                if (e.placement != EntryPlacement::Blob)
                    continue;
                const String bkey = layout.blobKey(e.ref);
                reachable_blobs.insert(bkey);
                ++report.total_blob_refs;
                report.referenced_logical_bytes += e.blob_size;
                blob_labels[bkey].push_back(label);
            }

            ++refs_walked;
            checkDeadline(deadline, "walking refs");
            if (on_progress && refs_walked % 64 == 0)
                on_progress("walking refs", reachable_blobs.size(), refs_walked);
        }
    }
    report.distinct_blobs = reachable_blobs.size();

    /// Scoped mode skips the GLOBAL physical classification below: it is meaningless under a
    /// filter (blobs owned by other namespaces would read as unreachable) and would cost a
    /// pool-wide LIST for what should be O(scoped refs).
    if (namespace_prefix.empty())
    {
    /// Physical listing: blobs + manifest bodies. The per-hash `.meta` descriptor sibling
    /// (`blobMetaKey(id) == blobKey(id) + ".meta"`) lives under the SAME
    /// `blobsPrefix()` as the body, so partition the raw LIST into bodies vs `.meta` objects up
    /// front — a `.meta` key must never be classified as a content body (it would otherwise be
    /// misread as an unreferenced blob and fall into the dangling/pending/unaccounted pipeline
    /// below), and a body must never be misread as a `.meta`.
    std::unordered_map<String, uint64_t> present_all;
    listAll(backend, layout.blobsPrefix(), present_all, on_progress, deadline, "listing blobs");
    std::unordered_map<String, uint64_t> present_blobs;
    std::unordered_set<BlobRef, BlobRefHash> present_meta_hashes;
    present_blobs.reserve(present_all.size());
    for (const auto & [key, sz] : present_all)
    {
        if (key.ends_with(".meta"))
        {
            if (const std::optional<BlobRef> ref = layout.parseBlobKey(key))
                present_meta_hashes.insert(*ref);
            /// else: foreign key shape under blobs/ — not ours to pair
        }
        else
            present_blobs.emplace(key, sz);
    }
    for (const auto & [_, sz] : present_blobs)
        report.physical_bytes += sz;

    /// Reachable blobs must be present (HEAD-confirm against LIST lag before declaring loss).
    for (const String & bkey : reachable_blobs)
    {
        auto it = present_blobs.find(bkey);
        bool exists = it != present_blobs.end();
        uint64_t size = exists ? it->second : 0;
        if (!exists)
        {
            const HeadResult h = backend.head(bkey);
            if (h.exists)
            {
                exists = true;
                size = h.size;
                report.physical_bytes += h.size;
            }
        }

        const auto lit = blob_labels.find(bkey);
        if (!exists)
        {
            /// Before declaring a loss, re-resolve the referencing refs freshly — a ref
            /// re-published or dropped between the walk and this HEAD-confirm, combined with a
            /// legitimate GC delete of the OLD blob, must NOT surface as a phantom dangle.
            const bool still_referenced = blobStillReferenced(store, layout, bkey,
                lit != blob_labels.end() ? lit->second : std::vector<String>{}, deadline);
            if (!still_referenced)
                continue;   /// stale-walk artifact: neither reachable nor dangling — skip entirely
        }

        if (exists)
            ++report.reachable;
        else
            ++report.dangling;
        if (detail || !exists)
        {
            FsckObject o;
            o.key = bkey;
            o.kind = ObjectKind::Blob;
            o.size = size;
            o.cls = exists ? FsckClass::Reachable : FsckClass::Dangling;
            if (detail && lit != blob_labels.end())
                o.reachable_from = lit->second;
            report.objects.push_back(std::move(o));
        }
    }

    /// Present-but-unreferenced blobs: classify through the GC pipeline view instead of one
    /// suspicious "unreachable" lump (the multi-stage graduation keeps a nonzero churning
    /// set here on ANY active pool, and beta testers read "unreachable" as a leak). The GC state is
    /// read for LABELING ONLY — reachability above never consults it.
    std::unordered_map<BlobRef, RetiredEntry, BlobRefHash> retired_by_hash;
    std::unordered_set<BlobRef, BlobRefHash> unref_hashes;
    std::unordered_set<BlobRef, BlobRefHash> in_run_hashes;
    bool have_gc_state = false;

    for (const auto & [bkey, sz] : present_blobs)
        if (!reachable_blobs.count(bkey))
        {
            if (const std::optional<BlobRef> ref = layout.parseBlobKey(bkey))
                unref_hashes.insert(*ref);
        }

    if (!unref_hashes.empty())
    {
        if (const auto state_got = backend.get(layout.gcStateKey()))
        {
            have_gc_state = true;
            const GcState gc_state = decodeGcState(state_got->bytes);
            /// The adopted fold seal names the snapshot runs; resolution is by ref, never by key
            /// construction. Every row whose hash is in our candidate set marks "known to GC" —
            /// edges still counted (drop unfolded), an explicit zero-marker mid-pipeline, or a
            /// `kCondemned` sentinel row that carries the condemned state (retired-in-snapshot):
            /// the `kCondemned` rows feed `retired_by_hash` (the `PendingGc` classification) in the
            /// SAME pass, replacing the removed `retired_refs`/`decodeRetiredSet` loop.
            ///
            /// These sets are keyed by the full `BlobRef`, not a narrowed digest. The run's own
            /// algorithm-prefixed key is parsed by `SourceEdgeKeyCodec` and compared directly with
            /// the full identity parsed from the listed blob key. This is required for mixed-algorithm
            /// pools: a 64-hex digest must not be truncated or compared as though it used the pool's
            /// local write algorithm, or its true GC state could be hidden as `Unaccounted`.
            if (const auto seal_got = backend.get(layout.foldSealKey(gc_state.snap_generation, gc_state.snap_attempt)))
            {
                uint64_t rows = 0;
                for (const RunRef & run : decodeFoldSeal(seal_got->bytes).blob_target_runs)
                {
                    checkDeadline(deadline, "reading gc snapshot runs");
                    /// Typed open: the source-edge run reader goes through openSourceEdgeRun (the NDJSON
                    /// header gates type == cas_run + kind == source_edge). Fsck keys off the row's hash
                    /// (the record's own algo-prefixed key, never from pool meta).
                    SourceEdgeRunView reader = openSourceEdgeRun(backend, run.key);
                    String key;
                    String payload;
                    while (reader.next(key, payload))
                    {
                        BlobRef ref;
                        UInt128 source_id;
                        SourceEdgeKeyCodec::parse(key, ref, source_id);   // throws CORRUPTED_DATA on malformed (fail-closed)
                        if (unref_hashes.count(ref))
                        {
                            in_run_hashes.insert(ref);
                            if (!payload.empty() && payload[0] == kCondemned)
                            {
                                const CondemnedRow row = decodeCondemnedRow(payload);
                                RetiredEntry e;
                                e.kind = ObjectKind::Blob;
                                e.ref = ref;
                                e.token = row.token;
                                e.size = row.size;
                                e.condemn_round = row.condemn_round;
                                e.delete_pending = row.delete_pending;
                                retired_by_hash.emplace(ref, std::move(e));
                            }
                        }
                        if (on_progress && ++rows % 65536 == 0)
                            on_progress("reading gc snapshot runs", in_run_hashes.size(), rows);
                    }
                    /// Whole-file seal checksum: compare the drained run's accumulated
                    /// checksum to the seal's `RunRef::checksum`. Fsck is a read-only auditor — instead of
                    /// throwing (which would abort the whole scan on the first corrupt run), catalogue the
                    /// mismatch as a `CorruptedRun` finding (with the run key) and continue so the audit
                    /// enumerates every problem in one pass. The deletion-deriving consumers
                    /// (`fold`/`zeroInDegree`/`previewDeletes`) still fail closed on the same mismatch.
                    if (reader.accumulatedChecksum() != run.checksum)
                    {
                        ++report.corrupted_runs;
                        if (detail)
                            report.objects.push_back(FsckObject{.key = run.key, .cls = FsckClass::CorruptedRun, .reachable_from = {}});
                    }
                }
            }
        }
    }

    for (const auto & [bkey, sz] : present_blobs)
    {
        if (reachable_blobs.count(bkey))
            continue;
        ++report.unreachable;

        /// A foreign/malformed key (`parseBlobKey` -> `nullopt`) falls back to the default `BlobRef{}`,
        /// which cannot match a real `retired_by_hash`/`in_run_hashes` entry — it lands in the generic
        /// `Unaccounted` bucket below, exactly the "debris, not ours" classification `parseBlobKey`
        /// documents: foreign algorithm segments are debris, not pool objects.
        const BlobRef hash = layout.parseBlobKey(bkey).value_or(BlobRef{});

        FsckClass cls = FsckClass::Unaccounted;
        String note;
        if (const auto rit = retired_by_hash.find(hash); rit != retired_by_hash.end()
            && backend.head(bkey).token == rit->second.token)
        {
            /// The PRESENT incarnation is the condemned one — deletion is scheduled. A token
            /// mismatch means the listed entry belongs to a displaced older incarnation and says
            /// nothing about this object; fall through to the snapshot check.
            cls = FsckClass::PendingGc;
            note = rit->second.delete_pending
                ? "delete_pending: exact-token delete executes next GC round"
                : "condemned at round " + std::to_string(rit->second.condemn_round)
                    + "; graduates once every writer acks past it (expected)";
        }
        else if (in_run_hashes.count(hash))
        {
            cls = FsckClass::AwaitingGc;
            note = "edges still in the GC snapshot; the drop has not folded yet (expected)";
        }
        else if (!have_gc_state)
        {
            cls = FsckClass::AwaitingGc;
            note = "GC has not run on this pool yet";
        }
        else
        {
            note = "not in the current GC view — transient for a fast create+drop between rounds; "
                   "PERSISTENT occurrences violate INV-2 (reachability-before-content), investigate";
        }

        switch (cls)
        {
            case FsckClass::PendingGc:   ++report.pending_gc;   break;
            case FsckClass::AwaitingGc:  ++report.awaiting_gc;  break;
            default:                     ++report.unaccounted;  break;
        }
        if (detail)
        {
            FsckObject o;
            o.key = bkey;
            o.kind = ObjectKind::Blob;
            o.size = sz;
            o.cls = cls;
            o.reachable_from = {std::move(note)};
            report.objects.push_back(std::move(o));
        }
    }

    /// Meta <-> body pairing: a `.meta` object with no
    /// body is an INV-META-BODY violation (the fixed meta/body lifecycle never leaves a meta
    /// orphaned of its body) — a real ERROR, distinct from `dangling` (which is reachability-driven).
    /// A body with no `.meta` is a benign not-yet-adopted (or interrupted-birth) artifact, NOT a dangle
    /// — it still classifies through the ordinary present-but-unreferenced pipeline above.
    std::unordered_set<BlobRef, BlobRefHash> present_body_hashes;
    present_body_hashes.reserve(present_blobs.size());
    for (const auto & [bkey, _] : present_blobs)
        if (const std::optional<BlobRef> ref = layout.parseBlobKey(bkey))
            present_body_hashes.insert(*ref);
        /// else: foreign key shape under blobs/ — not ours to pair
    for (const BlobRef & hash : present_meta_hashes)
        if (!present_body_hashes.count(hash))
            ++report.meta_without_body;
    for (const BlobRef & hash : present_body_hashes)
        if (!present_meta_hashes.count(hash))
            ++report.body_without_meta;
    }
    else
    {
        /// Scoped mode: dangling-only for the selected namespaces. Each blob named by a scoped ref
        /// is HEAD-verified (O(scoped refs), no pool-wide LIST); the unreachable/pending pipeline
        /// classification needs the whole pool and is intentionally skipped.
        for (const String & bkey : reachable_blobs)
        {
            checkDeadline(deadline, "head-checking scoped blobs");
            const HeadResult h = backend.head(bkey);
            const auto lit = blob_labels.find(bkey);
            bool exists = h.exists;
            if (!exists)
            {
                /// Use the same HEAD-absent re-resolve as the global-mode loop above.
                const bool still_referenced = blobStillReferenced(store, layout, bkey,
                    lit != blob_labels.end() ? lit->second : std::vector<String>{}, deadline);
                if (!still_referenced)
                    continue;   /// stale-walk artifact — neither reachable nor dangling
            }
            if (exists)
            {
                ++report.reachable;
                report.physical_bytes += h.size;
            }
            else
                ++report.dangling;
            if (detail || !exists)
            {
                FsckObject o;
                o.key = bkey;
                o.kind = ObjectKind::Blob;
                o.size = exists ? h.size : 0;
                o.cls = exists ? FsckClass::Reachable : FsckClass::Dangling;
                if (detail && lit != blob_labels.end())
                    o.reachable_from = lit->second;
                report.objects.push_back(std::move(o));
            }
        }
    }

    /// Pre-precommit manifest debris: a `cas/manifests/` body with no committed owner. An ELIGIBLE prefix's
    /// orphan is reclaimable debris => INFO (Unreachable); a non-eligible (in-flight) one is also info,
    /// never an error. The owner-visible missing-body case is the error above.
    for (const String & ns_str : store.listNamespaces(namespace_prefix))
    {
        const RootNamespace ns{ns_str};
        const String manifests_prefix = layout.manifestNamespacePrefix(ns);
        std::unordered_map<String, uint64_t> manifest_bodies;
        listAll(backend, manifests_prefix, manifest_bodies, on_progress, deadline, "listing manifests");
        for (const auto & [mkey, sz] : manifest_bodies)
        {
            if (owned_manifest_keys.count(mkey))
                continue;   /// owned by a committed ref — accounted above
            ++report.unreachable;
            if (detail)
            {
                BuildPrefix prefix;
                const bool parsed = parseBuildPrefix(layout, mkey, prefix);
                FsckObject o;
                o.key = mkey;
                o.kind = ObjectKind::Blob;
                o.size = sz;
                o.cls = FsckClass::Unreachable;
                if (parsed && prefixEligible(store, ns, prefix))
                    o.reachable_from = {"reclaimable-pre-precommit"};
                else
                    o.reachable_from = {"in-flight-pre-precommit"};
                report.objects.push_back(std::move(o));
            }
        }
    }

}

}

FsckReport runFsck(Pool & store, bool detail, FsckProgress on_progress,
                   std::optional<std::chrono::steady_clock::time_point> deadline,
                   bool partial_on_deadline, const String & namespace_prefix)
{
    FsckReport report;
    try
    {
        runFsckImpl(store, detail, on_progress, deadline, namespace_prefix, report);
    }
    catch (const Exception & e)
    {
        if (!partial_on_deadline || e.code() != ErrorCodes::TIMEOUT_EXCEEDED)
            throw;
        report.partial = true;
        report.partial_reason = e.message();
    }
    return report;
}

}
