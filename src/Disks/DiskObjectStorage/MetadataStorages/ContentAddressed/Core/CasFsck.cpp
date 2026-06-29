#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasFsck.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasLayout.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasManifestCodec.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasManifestId.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasOrphanManifestSweep.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasRootShardCodec.h>

#include <Common/Exception.h>

#include <set>
#include <unordered_map>

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
}

FsckReport runFsck(Store & store, bool detail, FsckProgress on_progress,
                   std::optional<std::chrono::steady_clock::time_point> deadline)
{
    const Layout & layout = store.layout();
    Backend & backend = store.backend();

    FsckReport report;

    /// OQ8 manifest audit. Reachability is recomputed from the AUTHORITATIVE refs (never from gc state):
    /// for each namespace, each committed ref resolves to a ManifestId; read its body; a committed ref
    /// naming a MISSING body is an ERROR (Dangling); a present body whose blobs are missing is an ERROR.
    std::set<String> reachable_blobs;        /// blob object keys named by a live owner
    std::set<String> owned_manifest_keys;    /// manifest object keys named by a committed owner
    std::unordered_map<String, std::vector<String>> blob_labels;   /// blob key -> "ns/ref" (detail)

    uint64_t refs_walked = 0;
    for (const String & ns_str : store.listNamespaces(""))
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

    /// Physical listing: blobs + manifest bodies.
    std::unordered_map<String, uint64_t> present_blobs;
    listAll(backend, layout.blobsPrefix(), present_blobs, on_progress, deadline, "listing blobs");
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

    /// Unreachable present blobs (in-grace debris or a leak) — info, never an error.
    for (const auto & [bkey, sz] : present_blobs)
    {
        if (reachable_blobs.count(bkey))
            continue;
        ++report.unreachable;
        if (detail)
        {
            FsckObject o;
            o.key = bkey;
            o.kind = ObjectKind::Blob;
            o.size = sz;
            o.cls = FsckClass::Unreachable;
            report.objects.push_back(std::move(o));
        }
    }

    /// Pre-precommit manifest debris: a `cas/manifests/` body with no committed owner. An ELIGIBLE prefix's
    /// orphan is reclaimable debris => INFO (Unreachable); a non-eligible (in-flight) one is also info,
    /// never an error. The owner-visible missing-body case is the error above.
    for (const String & ns_str : store.listNamespaces(""))
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

    return report;
}

}
