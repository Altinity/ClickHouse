#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasBlobInDegree.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasFsck.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasGcFormats.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasGenerationSeal.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasLayout.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasRunFile.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasManifestCodec.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasManifestId.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasOrphanManifestSweep.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasRootShardCodec.h>

#include <Common/Exception.h>
#include <Common/HashTable/Hash.h>

#include <set>
#include <unordered_map>
#include <unordered_set>

namespace DB::ErrorCodes
{
    extern const int TIMEOUT_EXCEEDED;
}

namespace DB::Cas
{

namespace
{
constexpr uint64_t PROGRESS_PAGES = 16;

using Deadline = std::optional<std::chrono::steady_clock::time_point>;

void checkDeadline(const Deadline & deadline, std::string_view phase)
{
    if (deadline && std::chrono::steady_clock::now() > *deadline)
        throw Exception(ErrorCodes::TIMEOUT_EXCEEDED,
            "fsck: exceeded the deadline during '{}' — run against a QUIESCED pool or raise --timeout.", phase);
}

void listAll(Backend & backend, const String & prefix, std::unordered_map<String, uint64_t> & out,
             const FsckProgress & on_progress, const Deadline & deadline, std::string_view phase)
{
    String cursor;
    uint64_t pages = 0;
    while (true)
    {
        ListPage page = backend.list(prefix, cursor, 1000);
        for (const auto & k : page.keys)
            out[k.key] = k.size;
        ++pages;
        checkDeadline(deadline, phase);
        if (on_progress && pages % PROGRESS_PAGES == 0)
            on_progress(phase, out.size(), pages);
        if (page.next_cursor.empty())
            break;
        cursor = page.next_cursor;
    }
    if (on_progress)
        on_progress(phase, out.size(), pages);
}

/// Parse (writer_epoch, build_sequence) from a manifest object key of the shape
/// `<manifest-prefix><writer_epoch>/<build_seq>/<ordinal>.proto`. Returns false on a malformed key.
bool parseBuildPrefix(const String & key, const String & manifests_prefix, BuildPrefix & out)
{
    if (!key.starts_with(manifests_prefix))
        return false;
    const String rest = key.substr(manifests_prefix.size());
    const size_t file_sep = rest.rfind('/');
    if (file_sep == String::npos)
        return false;
    const size_t build_sep = rest.rfind('/', file_sep == 0 ? 0 : file_sep - 1);
    if (build_sep == String::npos)
        return false;
    const size_t writer_sep = rest.rfind('/', build_sep == 0 ? 0 : build_sep - 1);
    if (writer_sep != String::npos)
        return false;
    const String writer_epoch_str = rest.substr(0, build_sep);
    const String seq_str = rest.substr(build_sep + 1, file_sep - build_sep - 1);
    const String file = rest.substr(file_sep + 1);
    if (writer_epoch_str.empty() || seq_str.empty() || file.size() != 12 || !file.ends_with(".proto"))
        return false;
    try
    {
        size_t consumed = 0;
        out.writer_epoch = std::stoull(writer_epoch_str, &consumed);
        if (consumed != writer_epoch_str.size())
            return false;
        consumed = 0;
        out.build_sequence = std::stoull(seq_str, &consumed);
        if (consumed != seq_str.size())
            return false;
        consumed = 0;
        const uint64_t ordinal = std::stoull(file.substr(0, 6), &consumed);
        return consumed == 6 && ordinal > 0 && ordinal <= kMaxManifestOrdinal;
    }
    catch (...)
    {
        return false;
    }
}

void runFsckImpl(Store & store, bool detail, const FsckProgress & on_progress, const Deadline & deadline,
                  const String & namespace_prefix, FsckReport & report)
{
    const Layout & layout = store.layout();
    Backend & backend = store.backend();

    /// OQ8 manifest audit. Reachability is recomputed from the AUTHORITATIVE refs (never from gc state):
    /// for each namespace, each committed ref resolves to a ManifestId; read its body; a committed ref
    /// naming a MISSING body is an ERROR (Dangling); a present body whose blobs are missing is an ERROR.
    std::set<String> reachable_blobs;        /// blob object keys named by a live owner
    std::set<String> owned_manifest_keys;    /// manifest object keys named by a committed owner
    std::unordered_map<String, std::vector<String>> blob_labels;   /// blob key -> "ns/ref" (detail)

    uint64_t refs_walked = 0;
    for (const String & ns_str : store.listNamespaces(namespace_prefix))
    {
        const RootNamespace ns{ns_str};
        for (const auto & [ref_name, resolved] : store.listRefs(ns))
        {
            const ManifestId id = resolved.manifest_id;
            const String mkey = layout.manifestKey(id);
            owned_manifest_keys.insert(mkey);
            const String label = ns_str + "/" + ref_name;

            const auto got = backend.get(mkey);
            if (!got)
            {
                /// A committed ref naming a missing manifest body — INV-NO-DANGLE surfaced (error).
                ++report.dangling;
                if (detail || true)
                {
                    FsckObject o;
                    o.key = mkey;
                    o.kind = ObjectKind::Blob;   /// manifests have no ObjectKind; reuse Blob as the generic kind
                    o.size = 0;
                    o.cls = FsckClass::Dangling;
                    o.reachable_from = {label};
                    report.objects.push_back(std::move(o));
                }
                ++refs_walked;
                continue;
            }

            PartManifest body = decodePartManifest(got->bytes);
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
                const String bkey = layout.blobKey(BlobId(u128ToHex(e.blob_hash)));
                reachable_blobs.insert(bkey);
                ++report.total_blob_refs;
                report.referenced_logical_bytes += e.blob_size;
                if (detail)
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
    /// (`blobMetaKey(id) == blobKey(id) + ".meta"`, v3 raw-body-refinement) lives under the SAME
    /// `blobsPrefix()` as the body, so partition the raw LIST into bodies vs `.meta` objects up
    /// front — a `.meta` key must never be classified as a content body (it would otherwise be
    /// misread as an unreferenced blob and fall into the dangling/pending/unaccounted pipeline
    /// below), and a body must never be misread as a `.meta`.
    std::unordered_map<String, uint64_t> present_all;
    listAll(backend, layout.blobsPrefix(), present_all, on_progress, deadline, "listing blobs");
    std::unordered_map<String, uint64_t> present_blobs;
    std::unordered_set<UInt128, UInt128Hash> present_meta_hashes;
    present_blobs.reserve(present_all.size());
    for (const auto & [key, sz] : present_all)
    {
        if (key.ends_with(".meta"))
        {
            const String body_key = key.substr(0, key.size() - String(".meta").size());
            const size_t slash = body_key.rfind('/');
            if (slash == String::npos)
                continue;
            try
            {
                present_meta_hashes.insert(hexToU128(body_key.substr(slash + 1)));
            }
            catch (...)
            {
                continue;   /// foreign key shape under blobs/ — not ours to pair
            }
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
            if (detail)
                if (const auto lit = blob_labels.find(bkey); lit != blob_labels.end())
                    o.reachable_from = lit->second;
            report.objects.push_back(std::move(o));
        }
    }

    /// Present-but-unreferenced blobs: classify through the GC pipeline view instead of one
    /// suspicious "unreachable" lump (2026-07-02; the two-phase graduation keeps a nonzero churning
    /// set here on ANY active pool, and beta testers read "unreachable" as a leak). The GC state is
    /// read for LABELING ONLY — reachability above never consults it.
    std::unordered_map<UInt128, RetiredEntry, UInt128Hash> retired_by_hash;
    std::unordered_set<UInt128, UInt128Hash> unref_hashes;
    std::unordered_set<UInt128, UInt128Hash> in_run_hashes;
    bool have_gc_state = false;

    for (const auto & [bkey, sz] : present_blobs)
        if (!reachable_blobs.count(bkey))
        {
            const size_t slash = bkey.rfind('/');
            if (slash != String::npos)
                unref_hashes.insert(hexToU128(bkey.substr(slash + 1)));
        }

    if (!unref_hashes.empty())
    {
        if (const auto state_got = backend.get(layout.gcStateKey()))
        {
            have_gc_state = true;
            const GcState gc_state = decodeGcState(state_got->bytes);
            for (const auto & [shard, ref_key] : gc_state.retired_refs)
            {
                checkDeadline(deadline, "reading retired sets");
                if (const auto got = backend.get(ref_key))
                    for (const RetiredEntry & e : decodeRetiredSet(got->bytes).entries)
                        if (e.kind == ObjectKind::Blob && unref_hashes.count(e.hash))
                            retired_by_hash.emplace(e.hash, e);
            }
            /// The adopted fold seal names the snapshot runs (T0: resolution is by ref, never by key
            /// construction). Every row whose hash is in our candidate set marks "known to GC" —
            /// edges still counted (drop unfolded) or an explicit zero-marker mid-pipeline.
            if (const auto seal_got = backend.get(layout.foldSealKey(gc_state.snap_generation, gc_state.snap_attempt)))
            {
                uint64_t rows = 0;
                for (const RunRef & run : decodeFoldSeal(seal_got->bytes).blob_target_runs)
                {
                    checkDeadline(deadline, "reading gc snapshot runs");
                    /// Typed open (spec §2.1): every source-edge run reader goes through openSourceEdgeRun.
                    /// Fsck only keys off the row's hash (via parseSrcEdgeRunKey), so a hash carried by a
                    /// `kCondemned` sentinel row now also correctly marks the blob "known to GC".
                    RunFileReader reader = openSourceEdgeRun(backend, run.key);
                    String key;
                    String payload;
                    while (reader.next(key, payload))
                    {
                        UInt128 hash;
                        UInt128 source_id;
                        if (parseSrcEdgeRunKey(key, hash, source_id) && unref_hashes.count(hash))
                            in_run_hashes.insert(hash);
                        if (on_progress && ++rows % 65536 == 0)
                            on_progress("reading gc snapshot runs", in_run_hashes.size(), rows);
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

        const size_t slash = bkey.rfind('/');
        const UInt128 hash = slash != String::npos ? hexToU128(bkey.substr(slash + 1)) : UInt128{};

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

    /// Meta <-> body pairing (spec 2026-07-09 §raw-body-refinement, v3): a `.meta` object with no
    /// body is an INV-META-BODY violation (the fixed meta/body lifecycle never leaves a meta
    /// orphaned of its body) — a real ERROR, distinct from `dangling` (which is reachability-driven).
    /// A body with no `.meta` is a benign not-yet-adopted (or crashed-birth) artifact, NOT a dangle
    /// — it still classifies through the ordinary present-but-unreferenced pipeline above.
    std::unordered_set<UInt128, UInt128Hash> present_body_hashes;
    present_body_hashes.reserve(present_blobs.size());
    for (const auto & [bkey, _] : present_blobs)
    {
        const size_t slash = bkey.rfind('/');
        if (slash == String::npos)
            continue;
        try
        {
            present_body_hashes.insert(hexToU128(bkey.substr(slash + 1)));
        }
        catch (...)
        {
            continue;   /// foreign key shape under blobs/ — not ours to pair
        }
    }
    for (const UInt128 & hash : present_meta_hashes)
        if (!present_body_hashes.count(hash))
            ++report.meta_without_body;
    for (const UInt128 & hash : present_body_hashes)
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
            if (h.exists)
            {
                ++report.reachable;
                report.physical_bytes += h.size;
            }
            else
                ++report.dangling;
            if (detail || !h.exists)
            {
                FsckObject o;
                o.key = bkey;
                o.kind = ObjectKind::Blob;
                o.size = h.exists ? h.size : 0;
                o.cls = h.exists ? FsckClass::Reachable : FsckClass::Dangling;
                if (detail)
                    if (const auto lit = blob_labels.find(bkey); lit != blob_labels.end())
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
                const bool parsed = parseBuildPrefix(mkey, manifests_prefix, prefix);
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

FsckReport runFsck(Store & store, bool detail, FsckProgress on_progress,
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
