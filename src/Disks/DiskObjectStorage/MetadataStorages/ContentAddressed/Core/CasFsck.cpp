#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasFsck.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasLayout.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasTreeCodec.h>

#include <functional>
#include <set>
#include <unordered_map>

namespace DB::Cas
{

namespace
{
void listAll(Backend & backend, const String & prefix, std::unordered_map<String, uint64_t> & out)
{
    String cursor;
    while (true)
    {
        ListPage page = backend.list(prefix, cursor, 1000);
        for (const auto & k : page.keys)
            out[k.key] = k.size;
        if (page.next_cursor.empty())
            break;
        cursor = page.next_cursor;
    }
}
}

FsckReport runFsck(Store & store, bool detail)
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

    for (const String & ns_str : store.listNamespaces(""))
    {
        const RootNamespace ns{ns_str};
        for (const auto & [ref_name, resolved] : store.listRefs(ns))
        {
            std::set<String> seen;
            walk(resolved.tree_id, ns_str + "/" + ref_name, seen);
        }
    }
    report.distinct_blobs = reachable_blobs.size();

    std::unordered_map<String, uint64_t> present;
    listAll(backend, layout.blobsPrefix(), present);
    listAll(backend, layout.treesPrefix(), present);
    listAll(backend, layout.packsPrefix(), present);
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
