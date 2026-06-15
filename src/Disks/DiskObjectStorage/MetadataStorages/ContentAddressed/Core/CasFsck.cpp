#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasFsck.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasLayout.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasTreeCodec.h>

#include <Common/Exception.h>

#include <functional>
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
/// Report progress every PROGRESS_PAGES list pages so a long/slow scan is visibly working (#5).
constexpr uint64_t PROGRESS_PAGES = 16;

using Deadline = std::optional<std::chrono::steady_clock::time_point>;

/// Bound the WHOLE scan (checked between pages/refs): a slow-but-progressing scan surfaces a clear
/// error instead of an opaque hang. (A single page stuck in S3-client retries is bounded by the
/// disk's S3 retry/timeout settings, not here.)
void checkDeadline(const Deadline & deadline, std::string_view phase)
{
    if (deadline && std::chrono::steady_clock::now() > *deadline)
        throw Exception(ErrorCodes::TIMEOUT_EXCEEDED,
            "fsck: exceeded the deadline during '{}' — likely a RustFS LIST stall under load. "
            "Run against a QUIESCED pool, raise --timeout, or lower the disk's S3 retry budget (see B158).", phase);
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
}

FsckReport runFsck(Store & store, bool detail, FsckProgress on_progress,
                   std::optional<std::chrono::steady_clock::time_point> deadline)
{
    const Layout & layout = store.layout();
    Backend & backend = store.backend();

    FsckReport report;

    std::unordered_map<String, std::vector<String>> reachable;
    std::set<String> reachable_blobs;

    std::function<void(const TreeId &, const String &, std::set<String> &)> walk =
        [&](const TreeId & tree_id, const String & label, std::set<String> & seen)
    {
        const String tkey = layout.treeKey(tree_id);
        if (detail)
            reachable[tkey].push_back(label);
        else
            reachable.try_emplace(tkey);
        if (!seen.insert(tree_id.string()).second)
            return;
        for (const TreeEntry & e : store.readTree(tree_id))
        {
            if (e.placement == Placement::Blob)
            {
                const String bkey = layout.blobKey(BlobId(u128ToHex(e.file_hash)));
                if (detail)
                    reachable[bkey].push_back(label);
                else
                    reachable.try_emplace(bkey);
                reachable_blobs.insert(bkey);
                ++report.total_blob_refs;
                report.referenced_logical_bytes += e.file_size;
            }
            else if (e.placement == Placement::PackSlice)
            {
                /// Packs are M-F (not produced yet); the producing-path regression test lands with
                /// them. A slice's liveness keeps the whole pack object reachable, keyed by pack_hash.
                const String pkey = layout.packKey(PackId(u128ToHex(e.pack_hash)));
                if (detail)
                    reachable[pkey].push_back(label);
                else
                    reachable.try_emplace(pkey);
                ++report.total_blob_refs;
                report.referenced_logical_bytes += e.file_size;
            }
            else if (e.placement == Placement::Subtree)
            {
                walk(TreeId(u128ToHex(e.file_hash)), label, seen);
            }
            /// Placement::Inline carries its bytes in the tree payload — no separate object.
        }
    };

    uint64_t refs_walked = 0;
    for (const String & ns_str : store.listNamespaces(""))
    {
        const RootNamespace ns{ns_str};
        for (const auto & [ref_name, resolved] : store.listRefs(ns))
        {
            std::set<String> seen;
            walk(resolved.tree_id, ns_str + "/" + ref_name, seen);
            if (++refs_walked % 64 == 0)
            {
                checkDeadline(deadline, "walking refs");
                if (on_progress)
                    on_progress("walking refs", reachable.size(), refs_walked);
            }
        }
    }
    report.distinct_blobs = reachable_blobs.size();

    std::unordered_map<String, uint64_t> present;
    listAll(backend, layout.blobsPrefix(), present, on_progress, deadline, "listing blobs");
    listAll(backend, layout.treesPrefix(), present, on_progress, deadline, "listing trees");
    listAll(backend, layout.packsPrefix(), present, on_progress, deadline, "listing packs");
    for (const auto & [_, sz] : present)
        report.physical_bytes += sz;

    auto kindOf = [&](const String & key)
    {
        if (key.starts_with(layout.blobsPrefix())) return ObjectKind::Blob;
        if (key.starts_with(layout.packsPrefix())) return ObjectKind::Pack;
        return ObjectKind::Tree;
    };

    for (const auto & [key, labels] : reachable)
    {
        auto it = present.find(key);
        bool exists = it != present.end();
        uint64_t size = exists ? it->second : 0;

        /// INV-NO-LOSS is SAFETY-critical, so it must be authoritative against LIST lag. A LIST can lag
        /// for recently-written objects (eventual consistency / mid-churn), so a reachable object that is
        /// actually PRESENT may be absent from a lagging LIST. Before declaring loss, HEAD-confirm the
        /// suspected-dangling key — `head` is authoritative for presence, LIST is merely advisory. Only a
        /// reachable object that HEAD ALSO cannot find is truly dangling. We HEAD only the reachable∖LIST
        /// set (~0 in the healthy case), so this stays cheap.
        if (!exists)
        {
            const HeadResult h = backend.head(key);
            if (h.exists)
            {
                exists = true;
                size = h.size;
                /// LIST lagged behind a present object — count its bytes in physical accounting too,
                /// since the LIST-based scan above missed it.
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
            o.key = key;
            o.kind = kindOf(key);
            o.size = size;
            o.cls = exists ? FsckClass::Reachable : FsckClass::Dangling;
            o.reachable_from = labels;
            report.objects.push_back(std::move(o));
        }
    }
    for (const auto & [key, sz] : present)
    {
        if (reachable.contains(key))
            continue;
        ++report.unreachable;
        if (detail)
        {
            FsckObject o;
            o.key = key;
            o.kind = kindOf(key);
            o.size = sz;
            o.cls = FsckClass::Unreachable;
            report.objects.push_back(std::move(o));
        }
    }

    return report;
}

}
