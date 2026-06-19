#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/ContentAddressedTransaction.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/ContentAddressedWriteBuffers.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasBuild.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasIds.h>
#include <IO/ReadBufferFromFile.h>
#include <IO/ReadBufferFromMemory.h>
#include <IO/copyData.h>
#include <algorithm>
#include <Common/Exception.h>
#include <ctime>

namespace DB
{

namespace ErrorCodes
{
    extern const int FILE_DOESNT_EXIST;
    extern const int LOGICAL_ERROR;
    extern const int NOT_IMPLEMENTED;
}

namespace
{

[[noreturn]] void notYet(const char * op)
{
    /// M-W skeleton (plan 2026-06-12, T2): the write path lands task by task (T3-T9).
    throw Exception(ErrorCodes::NOT_IMPLEMENTED, "ContentAddressedTransaction::{} not wired yet (M-W)", op);
}

}

ContentAddressedTransaction::ContentAddressedTransaction(ContentAddressedMetadataStorage & metadata_storage_)
    : metadata_storage(metadata_storage_)
{
}

ContentAddressedTransaction::~ContentAddressedTransaction()
{
    /// An uncommitted transaction's uploads become heartbeat-gated debris (W-HEARTBEAT): abandon
    /// every still-open Build so its heartbeat is discarded. Replaces the PoC's pin machinery.
    if (committed)
        return;

    /// Drop refs we published early at the rename (B151) — the transaction did not commit, so a
    /// rolled-back insert must not leave a durable orphan ref. Best-effort: a failed drop leaves
    /// GC-reclaimable debris (and the replicated restart's ZK reconcile detaches an unexpected
    /// part), never a masked exception out of the destructor.
    for (const auto & [ns, ref] : rename_published_refs)
    {
        try
        {
            metadata_storage.store()->dropRef(ns, ref);
        }
        catch (...) // NOLINT(bugprone-empty-catch)
        {
        }
    }

    for (auto & [key, st] : parts)
    {
        if (!st.build)
            continue;
        try
        {
            st.build->abandon();
        }
        catch (...) // NOLINT(bugprone-empty-catch)
        {
            /// Best-effort: destructor must not throw; lingering debris is GC-reclaimed.
        }
    }
}

ContentAddressedTransaction::PartStaging &
ContentAddressedTransaction::stagingFor(const ContentAddressedMetadataStorage::Route & r)
{
    return parts[{r.ns.string(), r.ref}];
}

Cas::Build & ContentAddressedTransaction::buildFor(
    const ContentAddressedMetadataStorage::Route & r, PartStaging & st)
{
    if (!st.build)
        st.build = metadata_storage.store()->startBuild(
            Cas::BuildInfo{.intended_ref = r.ns.string() + "/" + r.ref, .op = Cas::ProvenanceOp::Insert});
    return *st.build;
}

bool ContentAddressedTransaction::republishRef(
    const Cas::RootNamespace & src_ns, const std::string & src_ref,
    const Cas::RootNamespace & dst_ns, const std::string & dst_ref)
{
    /// Move a COMMITTED ref: content addressing has no rename, so publish the SAME tree under the
    /// destination (adoptTree observes the tree and the publish gate carries the GC-race safety)
    /// and drop the source ref. Returns false (nothing written) when the source ref is absent.
    /// Force-fresh (Pillar B): RENAME/move source read — stale mutable_files must not carry to dst.
    auto resolved = metadata_storage.store()->resolveRef(src_ns, src_ref);
    if (!resolved)
        return false;
    auto build = metadata_storage.store()->startBuild(
        Cas::BuildInfo{.intended_ref = dst_ns.string() + "/" + dst_ref, .op = Cas::ProvenanceOp::Other});
    build->adoptTree(resolved->tree_id);
    Cas::RefPayload payload;
    payload.tree_size = resolved->tree_size;
    payload.mutable_files = resolved->mutable_files;   /// the .ca_mtime stamp carries over (a rename is not a new part)
    /// B171: protect the adopted closure via a build-root precommit before the fail-closed publish.
    build->precommit(resolved->tree_id);
    build->publish(dst_ns, dst_ref, resolved->tree_id, std::move(payload));
    metadata_storage.store()->dropRef(src_ns, src_ref);
    return true;
}

void ContentAddressedTransaction::dropRefIfPresent(const Cas::RootNamespace & ns, const std::string & ref)
{
    /// resolveRef gates the common case (a tmp ref that was never committed is a no-op, not an error);
    /// dropRef re-reads the shard inside its own CAS loop, so a concurrent drop can land in the window
    /// between our resolve and that re-read — surfacing as FILE_DOESNT_EXIST. Removal is replay-safe,
    /// so a ref that is already gone is success, not failure; any other error still propagates.
    if (!metadata_storage.store()->resolveRef(ns, ref, /*allow_stale=*/true))
        return;
    try
    {
        metadata_storage.store()->dropRef(ns, ref);
    }
    catch (const Exception & e)
    {
        if (e.code() != ErrorCodes::FILE_DOESNT_EXIST)
            throw;
    }
}

ContentAddressedTransaction::PartStaging * ContentAddressedTransaction::findStaging(
    const ContentAddressedMetadataStorage::Route & r)
{
    auto it = parts.find({r.ns.string(), r.ref});
    return it == parts.end() ? nullptr : &it->second;
}

std::optional<ContentAddressedMetadataStorage::Route>
ContentAddressedTransaction::routeOf(const std::string & path) const
{
    auto p = ContentAddressed::parsePartFilePath(path);
    if (!p)
        return std::nullopt;
    return metadata_storage.route(*p);
}

bool ContentAddressedTransaction::publishStaging(const Cas::RootNamespace & ns, const std::string & ref, PartStaging & st)
{
    if (st.published)
        return false;   /// already durable (published at the lock-free rename) — never re-publish

    if (!st.build && st.entries.empty())
    {
        /// Mutable-only staging: the MVCC layer's autocommit one-shots (txn_version.txt
        /// fill-in/rewrite) and metadata_version bumps on a COMMITTED part.
        if (!st.mutable_files.empty() || !st.mutable_removed.empty())
        {
            metadata_storage.store()->updateRefPayload(ns, ref, [&](Cas::RefPayload & payload)
            {
                for (const auto & [name, bytes] : st.mutable_files)
                    payload.mutable_files[name] = bytes;
                for (const auto & name : st.mutable_removed)
                    payload.mutable_files.erase(name);
            });
        }
        st.published = true;
        return false;   /// updateRefPayload mutates an existing ref — never a new ref to roll back
    }

    if (!st.build)
        throw Exception(ErrorCodes::LOGICAL_ERROR,
            "ContentAddressedTransaction: staged entries for {}/{} without a Build", ns.string(), ref);

    auto tree = st.build->putTree(st.entries);
    Cas::RefPayload payload;
    payload.mutable_files = st.mutable_files;
    /// The publish wall-clock stamp backing getLastModified (reserved name, filtered from
    /// every listing by the read side).
    payload.mutable_files[".ca_mtime"] = std::to_string(static_cast<uint64_t>(::time(nullptr)));

    /// B171: publish the build-root precommit edge as soon as the manifest tree is assembled and
    /// BEFORE the fail-closed publish. Every child of `tree` (including blobs adopted/dedup'd from a
    /// source part whose ref may be dropped concurrently) is now reachable from a durable build root,
    /// so GC cannot reclaim the closure out from under this commit or the GC rounds in between.
    st.build->precommit(tree);

    /// Force-fresh (Pillar B): publish-gate rollback tracking — a stale result mis-tracks rollback.
    const bool ref_existed = metadata_storage.store()->resolveRef(ns, ref).has_value();
    st.build->publish(ns, ref, tree, std::move(payload));
    st.published = true;
    return !ref_existed;
}

void ContentAddressedTransaction::commit(const TransactionCommitOptionsVariant &)
{
    /// Publish each staged part not already published at the lock-free rename (B151). Commit
    /// atomicity (B122): there is no multi-ref atomic publish, so a publish that throws after
    /// earlier parts already published would leave a PARTIAL commit — some refs durably visible while
    /// the transaction reports failure, diverging the durable pool from the disk layer's all-or-nothing
    /// expectation. Track the refs THIS commit creates and, on any exception, best-effort unpublish
    /// them before rethrowing. A partial commit is NOT a protocol violation (each publish/dropRef is
    /// individually gate-checked and journalled; the leftover uploads are GC-reclaimable debris) — this
    /// restores the wiring-layer transaction contract, not a CAS invariant.
    ///
    /// Fail-closed (CLAUDE.md): only refs that were ABSENT before we published them are rolled back. A
    /// ref that already existed is pre-existing data this commit must never destroy on its error path.
    /// Publishing over a live ref does not occur in the MergeTree write path (unique part names), but
    /// the rollback must not assume it. updateRefPayload mutations (autocommit one-shots on a COMMITTED
    /// part) are individually durable by design and are deliberately NOT rolled back.
    std::vector<std::pair<Cas::RootNamespace, std::string>> created_refs;
    try
    {
        for (auto & [key, st] : parts)
        {
            const Cas::RootNamespace ns{key.first};
            if (publishStaging(ns, key.second, st))
                created_refs.emplace_back(ns, key.second);
        }
    }
    catch (...)
    {
        /// Compensating rollback. Best-effort: a ref we cannot unpublish becomes unreferenced debris
        /// (GC-reclaimed); never mask the original failure with a rollback failure.
        for (const auto & [ns, ref] : created_refs)
        {
            try
            {
                metadata_storage.store()->dropRef(ns, ref);
            }
            catch (...) // NOLINT(bugprone-empty-catch)
            {
            }
        }
        throw;
    }
    committed = true;
}

TransactionCommitOutcomeVariant ContentAddressedTransaction::tryCommit(const TransactionCommitOptionsVariant & options)
{
    if (!std::holds_alternative<NoCommitOptions>(options))
        throw Exception(ErrorCodes::LOGICAL_ERROR,
            "ContentAddressed transaction supports only tryCommit without options");
    commit(options);
    return true;
}

ObjectStorageKey ContentAddressedTransaction::generateObjectKeyForPath(const std::string &)
{
    notYet("generateObjectKeyForPath");
}

StoredObjects ContentAddressedTransaction::getSubmittedForRemovalBlobs()
{
    /// CA never hands shared backing objects to the disk layer for removal — reclamation is the
    /// GC's. Empty unconditionally (the PoC contract, kept through the skeleton).
    return {};
}

const Cas::TreeEntry * ContentAddressedTransaction::findStagedEntry(
    const ContentAddressedMetadataStorage::Route & r) const
{
    auto it = parts.find({r.ns.string(), r.ref});
    if (it == parts.end())
        return nullptr;
    auto eit = std::find_if(it->second.entries.begin(), it->second.entries.end(),
        [&](const Cas::TreeEntry & e) { return e.name == r.file; });
    return eit == it->second.entries.end() ? nullptr : &*eit;
}

std::optional<StoredObjects> ContentAddressedTransaction::tryGetInFlightStorageObjects(const std::string & path) const
{
    /// B59 read-your-writes: a projection spill-and-merge reads back its own temp blocks before
    /// the parent part's single commit. Staged content blobs are already uploaded - locate them.
    auto r = const_cast<ContentAddressedTransaction *>(this)->routeOf(path);
    if (!r || r->file.empty())
        return {};
    if (const auto * entry = findStagedEntry(*r))
    {
        if (entry->placement == Cas::Placement::Blob)
        {
            const auto location = metadata_storage.store()->locate(*entry);
            return StoredObjects{StoredObject(location.key, path, location.length)};
        }
        return StoredObjects{StoredObject("", path, entry->file_size)};
    }
    return {};
}

std::unique_ptr<ReadBufferFromFileBase> ContentAddressedTransaction::tryReadFileInFlight(
    const std::string & path, const ReadSettings & settings, std::optional<size_t> /*read_hint*/) const
{
    auto r = const_cast<ContentAddressedTransaction *>(this)->routeOf(path);
    if (!r || r->file.empty())
        return nullptr;
    auto it = parts.find({r->ns.string(), r->ref});
    if (it == parts.end())
        return nullptr;
    /// Staged mutable bytes serve from memory; staged blobs read through the payload view.
    if (auto mit = it->second.mutable_files.find(r->file); mit != it->second.mutable_files.end())
        return std::make_unique<ReadBufferFromOwnMemoryFile>(path, mit->second);
    if (const auto * entry = findStagedEntry(*r))
    {
        if (entry->placement == Cas::Placement::Inline)
            return std::make_unique<ReadBufferFromOwnMemoryFile>(path, entry->inline_bytes);
        if (entry->placement == Cas::Placement::Blob)
            return metadata_storage.readBlobPayload(metadata_storage.store()->locate(*entry), path, settings);
    }
    return nullptr;
}

std::optional<uint64_t> ContentAddressedTransaction::tryGetInFlightFileSize(const std::string & path) const
{
    auto r = const_cast<ContentAddressedTransaction *>(this)->routeOf(path);
    if (!r || r->file.empty())
        return {};
    auto it = parts.find({r->ns.string(), r->ref});
    if (it == parts.end())
        return {};
    if (auto mit = it->second.mutable_files.find(r->file); mit != it->second.mutable_files.end())
        return mit->second.size();
    if (const auto * entry = findStagedEntry(*r))
        return entry->file_size;
    return {};
}

bool ContentAddressedTransaction::hasInFlightDirectory(const std::string & path) const
{
    /// Directory overlay (B59): true iff at least one staged file lives under `path` for `path`'s
    /// part - what makes a carried-forward projection dir visible to loadProjections.
    auto r = const_cast<ContentAddressedTransaction *>(this)->routeOf(path);
    /// INNER directories only (the PoC contract): the overlay exists for staged projection dirs
    /// (B58/B63 - loadProjections during finalize). The PART DIR ITSELF answers FALSE - a
    /// dedup-rejected temporary part still holds its uncommitted transaction at destruction, and
    /// an overlay "exists" for the bare part dir sends removeIfNeeded into remove(), whose
    /// bare-disk check then logs the "part to remove doesn't exist" warning (T13 finding).
    if (!r || r->ref.empty() || r->file.empty())
        return false;
    auto it = parts.find({r->ns.string(), r->ref});
    if (it == parts.end())
        return false;
    const std::string prefix = r->file + "/";
    for (const auto & entry : it->second.entries)
        if (entry.name.starts_with(prefix))
            return true;
    for (const auto & [name, _] : it->second.mutable_files)
        if (name.starts_with(prefix))
            return true;
    return false;
}

std::vector<std::string> ContentAddressedTransaction::listInFlightDirectory(const std::string & path) const
{
    /// Immediate-child names staged directly under `path` (one level) - loadProjections'
    /// withPartFormatFromDisk iterates a staged projection dir to find its mark file.
    auto r = const_cast<ContentAddressedTransaction *>(this)->routeOf(path);
    std::vector<std::string> result;
    if (!r || r->ref.empty())
        return result;
    auto it = parts.find({r->ns.string(), r->ref});
    if (it == parts.end())
        return result;
    const std::string prefix = r->file.empty() ? "" : r->file + "/";
    std::set<std::string> names;
    auto add = [&](const std::string & name)
    {
        if (!name.starts_with(prefix) || name.size() <= prefix.size())
            return;
        const auto rest = name.substr(prefix.size());
        const auto slash = rest.find('/');
        names.insert(slash == std::string::npos ? rest : rest.substr(0, slash));
    };
    for (const auto & entry : it->second.entries)
        add(entry.name);
    for (const auto & [name, _] : it->second.mutable_files)
        add(name);
    return {names.begin(), names.end()};
}

void ContentAddressedTransaction::createMetadataFile(const std::string &, const StoredObjects &)
{
    notYet("createMetadataFile");
}

std::unique_ptr<WriteBufferFromFileBase> ContentAddressedTransaction::writeFile(
    const std::string & path, size_t buf_size, WriteMode mode, const WriteSettings & settings)
{
    /// Non-part files are VERBATIM namespace files, durable on finalize (no commit involvement -
    /// the disk layer's autocommit contract for them rides exactly this). Append is serviced by
    /// read-modify-rewrite: the existing bytes are carried forward (the MVCC mutation-entry CSN
    /// append depends on this).
    if (!ContentAddressed::isPartFilePath(path))
    {
        if (auto tf = ContentAddressed::parseTableFilePath(path))
        {
            const Cas::RootNamespace ns = metadata_storage.liveNamespace(tf->table_uuid);
            const std::string name = tf->tail;
            std::string prefix_bytes;
            if (mode == WriteMode::Append)
                if (auto existing = metadata_storage.store()->getNamespaceFile(ns, name))
                    prefix_bytes = std::move(*existing);
            return std::make_unique<ContentAddressed::CaInlineWriteBuffer>(
                [this, ns, name, carried = std::move(prefix_bytes)](std::string bytes)
                {
                    metadata_storage.store()->putNamespaceFile(ns, name, carried + bytes);
                });
        }
        /// A loose disk file (the startup write probe): a plain mountpoint object (design §5.2).
        const std::string key = metadata_storage.serverId() + "/" + path;
        std::string prefix_bytes;
        if (mode == WriteMode::Append)
            if (auto existing = metadata_storage.store()->getMountpointObject(key))
                prefix_bytes = std::move(*existing);
        return std::make_unique<ContentAddressed::CaInlineWriteBuffer>(
            [this, key, carried = std::move(prefix_bytes)](std::string bytes)
            {
                metadata_storage.store()->putMountpointObject(key, carried + bytes);
            });
    }

    auto p = ContentAddressed::parsePartFilePath(path);
    auto r = p ? metadata_storage.route(*p) : std::nullopt;
    if (!r || r->file.empty())
        throw Exception(ErrorCodes::NOT_IMPLEMENTED,
            "ContentAddressedTransaction::writeFile: not a part file path: {}", path);

    /// A MUTABLE per-part file stages into the part's payload map; commit publishes it (with the
    /// part for a staged part, via updateRefPayload for a committed one).
    if (ContentAddressed::isMutablePerPartFile(r->file))
    {
        return std::make_unique<ContentAddressed::CaInlineWriteBuffer>(
            [this, route = *r](std::string bytes)
            {
                auto & st = stagingFor(route);
                st.mutable_files[route.file] = std::move(bytes);
                st.mutable_removed.erase(route.file);
            });
    }

    /// A CONTENT part file: spill + hash, then upload through Build::putBlob (W-FRESH-TAG /
    /// dedup-as-cold-reuse / retire-view checks live in the core) and stage the tree entry.
    return std::make_unique<ContentAddressed::CaContentWriteBuffer>(
        metadata_storage.scratchPath(),
        buf_size,
        settings.use_adaptive_write_buffer,
        settings.adaptive_write_buffer_initial_size,
        [this, route = *r](const std::string & hash_hex, size_t size, const std::string & temp_path)
        {
            auto & st = stagingFor(route);
            Cas::BlobSource source;
            source.size = size;
            source.write_payload = [&temp_path](WriteBuffer & out)
            {
                ReadBufferFromFile in(temp_path);
                copyData(in, out);
            };
            buildFor(route, st).putBlob(Cas::BlobId(hash_hex), std::move(source));

            Cas::TreeEntry entry;
            entry.name = route.file;
            entry.placement = Cas::Placement::Blob;
            entry.file_hash = Cas::hexToU128(hash_hex);
            entry.file_size = size;
            /// A re-write of the SAME staged name replaces the entry (whole-file rewrites).
            std::erase_if(st.entries, [&](const Cas::TreeEntry & e) { return e.name == entry.name; });
            st.entries.push_back(std::move(entry));
        });
}

void ContentAddressedTransaction::createDirectory(const std::string &)
{
    /// Object storage has no real directories (mirrors the plain-rewritable transaction).
}

void ContentAddressedTransaction::createDirectoryRecursive(const std::string &)
{
}

void ContentAddressedTransaction::removeDirectory(const std::string & path)
{
    /// The MergeTree fast-removal path unlinks a part's files one by one (no-ops here) and then
    /// calls removeDirectory(<part>) - the SINGLE authoritative point at which the part's ref must
    /// be unlinked (the PoC's B45). Part dirs route to dropRef; anything else is a no-op (object
    /// storage has no real directories; tables/detached/shadow are removed via removeRecursive).
    if (auto r = routeOf(path); r && !r->ref.empty() && r->file.empty())
    {
        dropRefIfPresent(r->ns, r->ref);
        return;
    }
}

void ContentAddressedTransaction::removeRecursive(const std::string & path, const ShouldRemoveObjectsPredicate & /*should_remove_objects*/)
{
    /// Removal = pointer-unlink + deferred GC: only refs and verbatim files go; the shared
    /// blobs/trees are reclaimed by Cas::Gc once unreachable. The predicate gates backing-object
    /// deletion, which CA always defers - intentionally ignored (PoC contract).

    /// FREEZE shadow shapes first (a shadow table dir also satisfies parseTableUuid).
    if (ContentAddressed::isShadowPath(path))
    {
        if (auto p = ContentAddressed::parsePartFilePath(path); p && !p->backup_name.empty() && p->file.empty())
        {
            const auto ns = ContentAddressedMetadataStorage::shadowNamespace(p->shadow_table_dir);
            dropRefIfPresent(ns, p->part_name);
            return;
        }
        if (ContentAddressed::endsWithTableUuidPair(path))
        {
            metadata_storage.store()->dropNamespace(ContentAddressedMetadataStorage::shadowNamespace(path));
            return;
        }
        /// Backup root / intermediate dir (SYSTEM UNFREEZE WITH NAME): drop every shadow
        /// namespace under it. Canonicalize: callers hand trailing-slash dirs (T13).
        std::string prefix = path;
        while (!prefix.empty() && prefix.back() == '/')
            prefix.pop_back();
        for (const auto & ns : metadata_storage.store()->listNamespaces(prefix + "/"))
            metadata_storage.store()->dropNamespace(Cas::RootNamespace{ns});
        return;
    }

    /// Table dir: the live namespace, its detached namespace, and every verbatim file go.
    if (auto uuid = ContentAddressed::parseTableUuid(path))
    {
        metadata_storage.store()->dropNamespace(metadata_storage.liveNamespace(*uuid));
        metadata_storage.store()->dropNamespace(metadata_storage.detachedNamespace(*uuid));
        return;
    }

    if (auto p = ContentAddressed::parsePartFilePath(path))
    {
        auto r = metadata_storage.route(*p);
        /// The detached CONTAINER dir: drop the whole detached namespace.
        if (r && r->ref.empty() && p->part_name == ContentAddressed::kDetachedDirName)
        {
            metadata_storage.store()->dropNamespace(r->ns);
            return;
        }
        /// A single part dir (live or detached): drop its ref.
        if (r && !r->ref.empty() && r->file.empty())
        {
            dropRefIfPresent(r->ns, r->ref);
            return;
        }
        /// A projection subdir: virtual (nested in the parent tree) - removal is a no-op; the
        /// blobs go when the part's ref does (the PoC's B60 contract).
        if (r && !r->ref.empty())
            return;
    }

    /// Table-level SUBDIRECTORY (deduplication_logs/): remove every verbatim file under it.
    if (auto tf = ContentAddressed::parseTableFilePath(path))
    {
        const auto ns = metadata_storage.liveNamespace(tf->table_uuid);
        const std::string prefix = tf->tail + "/";
        for (const auto & name : metadata_storage.store()->listNamespaceFiles(ns))
            if (name.starts_with(prefix))
                metadata_storage.store()->removeNamespaceFile(ns, name);
        return;
    }
}

void ContentAddressedTransaction::createHardLink(const std::string & path_from, const std::string & path_to)
{
    auto src = routeOf(path_from);
    auto dst = routeOf(path_to);
    if (!src || src->file.empty() || !dst || dst->file.empty())
        throw Exception(ErrorCodes::LOGICAL_ERROR,
            "ContentAddressed: createHardLink requires two part-file paths: {} -> {}", path_from, path_to);

    auto & dst_st = stagingFor(*dst);

    /// A MUTABLE per-part file carries forward by VALUE (its private bytes), never by reference
    /// (the mutable decision is made on the SOURCE file — B62's clone shapes route cleanly now).
    if (ContentAddressed::isMutablePerPartFile(src->file))
    {
        if (auto * src_st = findStaging(*src))
            if (auto it = src_st->mutable_files.find(src->file); it != src_st->mutable_files.end())
            {
                dst_st.mutable_files[dst->file] = it->second;
                return;
            }
        /// Force-fresh (Pillar B): projection hardlink source — carry the current payload/tree_id.
        auto resolved = metadata_storage.store()->resolveRef(src->ns, src->ref);
        if (!resolved || !resolved->mutable_files.contains(src->file))
            throw Exception(ErrorCodes::FILE_DOESNT_EXIST,
                "ContentAddressed: createHardLink source mutable file missing: {}", path_from);
        dst_st.mutable_files[dst->file] = resolved->mutable_files.at(src->file);
        return;
    }

    /// Content file. Prefer an entry staged earlier in THIS transaction (the destination Build
    /// re-observes the blob via cold reuse — its own dependency); else carry forward from the
    /// COMMITTED source part (adoptFromTree: tokenless evidence pinned by the witnessed live
    /// source tree, W-EVIDENCE).
    Cas::TreeEntry entry;
    if (auto * src_st = findStaging(*src))
    {
        auto it = std::find_if(src_st->entries.begin(), src_st->entries.end(),
            [&](const Cas::TreeEntry & e) { return e.name == src->file; });
        if (it != src_st->entries.end())
        {
            entry = *it;
            if (entry.placement == Cas::Placement::Blob)
            {
                /// B156b: the staged source entry is either putBlob'd in this txn (TOKENED dep in the
                /// source build ⇒ recreatable by retrying the INSERT) or adopted from a committed source
                /// (TOKENLESS W-EVIDENCE dep ⇒ NOT recreatable, absence is a real loss). Discriminate on
                /// the SOURCE build's dep so a vanished putBlob'd blob retries (ABORTED) while a vanished
                /// adopted blob fails loud (FILE_DOESNT_EXIST). No source build ⇒ default not-recreatable.
                const bool body_recreatable = src_st->build && src_st->build->depIsTokened(entry.file_hash);
                buildFor(*dst, dst_st).reuseBlob(Cas::BlobId(Cas::u128ToHex(entry.file_hash)), body_recreatable);
            }
            else if (entry.placement != Cas::Placement::Inline)
                throw Exception(ErrorCodes::LOGICAL_ERROR,
                    "ContentAddressed: staged hardlink of unsupported placement for {}", path_from);
            entry.name = dst->file;
            std::erase_if(dst_st.entries, [&](const Cas::TreeEntry & e) { return e.name == entry.name; });
            dst_st.entries.push_back(std::move(entry));
            return;
        }
    }

    /// Force-fresh (Pillar B): projection hardlink source — carry the current payload/tree_id.
    auto resolved = metadata_storage.store()->resolveRef(src->ns, src->ref);
    if (!resolved)
        throw Exception(ErrorCodes::FILE_DOESNT_EXIST,
            "ContentAddressed: createHardLink source part missing: {}", path_from);
    entry = buildFor(*dst, dst_st).adoptFromTree(resolved->tree_id, src->file);
    entry.name = dst->file;
    std::erase_if(dst_st.entries, [&](const Cas::TreeEntry & e) { return e.name == entry.name; });
    dst_st.entries.push_back(std::move(entry));
}

void ContentAddressedTransaction::setLastModified(const std::string &, const Poco::Timestamp &)
{
    /// Timestamps are derived for content addressing (the publish stamp) — accept and ignore,
    /// exactly as the PoC did.
}

void ContentAddressedTransaction::chmod(const String &, mode_t)
{
    notYet("chmod");
}

void ContentAddressedTransaction::setReadOnly(const std::string &)
{
    /// Read-only flags have no content-addressed representation — accept and ignore (PoC behavior).
}

void ContentAddressedTransaction::moveDirectory(const std::string & path_from, const std::string & path_to)
{
    auto src_p = ContentAddressed::parsePartFilePath(path_from);
    auto dst_p = ContentAddressed::parsePartFilePath(path_to);
    auto src = src_p ? metadata_storage.route(*src_p) : std::nullopt;
    auto dst = dst_p ? metadata_storage.route(*dst_p) : std::nullopt;

    /// RENAME TABLE / cross-engine move: both endpoints are TABLE dirs. Republish every live and
    /// detached ref plus every verbatim file under the new table identity, then drop the old
    /// namespaces (the blobs/trees are content-addressed and untouched).
    if (auto src_table = ContentAddressed::parseTableUuid(path_from))
    {
        if (auto dst_table = ContentAddressed::parseTableUuid(path_to))
        {
            auto move_namespace = [&](const Cas::RootNamespace & from_ns, const Cas::RootNamespace & to_ns)
            {
                auto refs = metadata_storage.store()->listRefs(from_ns);
                for (const auto & [ref, _] : refs)
                    republishRef(from_ns, ref, to_ns, ref);
                for (const auto & name : metadata_storage.store()->listNamespaceFiles(from_ns))
                    if (auto bytes = metadata_storage.store()->getNamespaceFile(from_ns, name))
                        metadata_storage.store()->putNamespaceFile(to_ns, name, *bytes);
                metadata_storage.store()->dropNamespace(from_ns);
            };
            move_namespace(metadata_storage.liveNamespace(*src_table), metadata_storage.liveNamespace(*dst_table));
            move_namespace(metadata_storage.detachedNamespace(*src_table), metadata_storage.detachedNamespace(*dst_table));
            return;
        }
    }

    if (!src || !dst)
        throw Exception(ErrorCodes::LOGICAL_ERROR,
            "ContentAddressed: moveDirectory cannot classify {} -> {}", path_from, path_to);

    /// Projection MATERIALIZE/merge renames the staged <proj>_<n>.tmp_proj subdir to <proj>.proj
    /// inside the SAME staged part: re-key the staged entry-name prefixes so the published tree
    /// carries the final keys.
    if (!src->file.empty() && !dst->file.empty()
        && src->ns.string() == dst->ns.string() && src->ref == dst->ref
        && src->file.ends_with(".tmp_proj") && dst->file.ends_with(".proj"))
    {
        if (auto * st = findStaging(*src))
        {
            const std::string old_prefix = src->file + "/";
            const std::string new_prefix = dst->file + "/";
            for (auto & entry : st->entries)
                if (entry.name.starts_with(old_prefix))
                    entry.name = new_prefix + entry.name.substr(old_prefix.size());
            for (auto it = st->mutable_files.begin(); it != st->mutable_files.end();)
            {
                if (it->first.starts_with(old_prefix))
                {
                    st->mutable_files[new_prefix + it->first.substr(old_prefix.size())] = std::move(it->second);
                    it = st->mutable_files.erase(it);
                }
                else
                    ++it;
            }
            return;
        }
    }

    /// Every remaining shape is a PART-DIR move: (ns, ref) -> (ns', ref') with empty files. This
    /// uniformly covers tmp->final (staged), committed renames (delete_tmp_, merge results),
    /// DETACH (live -> detached ns), detached renames (attaching_/deleting_), and ATTACH
    /// (detached -> live ns) - in the new layout they are all the same two moves: re-key any
    /// staging, then move any committed ref.
    if (!src->ref.empty() && src->file.empty() && !dst->ref.empty() && dst->file.empty())
    {
        const std::pair<std::string, std::string> src_key{src->ns.string(), src->ref};
        const std::pair<std::string, std::string> dst_key{dst->ns.string(), dst->ref};
        if (src_key == dst_key)
            return;

        /// Re-key a STAGED source into the destination (B67: merge stagings; on a mutable-file
        /// collision PREFER the existing destination bytes - the newer MVCC state).
        if (auto src_it = parts.find(src_key); src_it != parts.end())
        {
            PartStaging & dst_st = parts[dst_key];
            PartStaging & src_st = src_it->second;
            for (auto & entry : src_st.entries)
            {
                std::erase_if(dst_st.entries, [&](const Cas::TreeEntry & e) { return e.name == entry.name; });
                dst_st.entries.push_back(std::move(entry));
            }
            for (auto & [file, bytes] : src_st.mutable_files)
                dst_st.mutable_files.emplace(file, std::move(bytes));   /// dest bytes win on collision
            for (const auto & file : src_st.mutable_removed)
                dst_st.mutable_removed.insert(file);
            if (!src_st.build)
            {
            }
            else if (!dst_st.build)
            {
                dst_st.build = std::move(src_st.build);
            }
            else
            {
                /// Two Builds for one destination part: keep the destination's; the source build's
                /// deps ride the staged entries (re-observed by the destination build at adopt
                /// time is unnecessary - entries staged via putBlob/adopt carry deps in the SOURCE
                /// build... merge conservatively by abandoning nothing and re-observing):
                for (const auto & entry : dst_st.entries)
                    if (entry.placement == Cas::Placement::Blob)
                    {
                        /// B156b: dst_st.entries here is the UNION of the original destination entries
                        /// (deps in dst_st.build) and the just-moved source entries (deps still in
                        /// src_st.build, which we abandon below). An entry is recreatable iff EITHER build
                        /// holds a TOKENED (putBlob'd) dep for it; a tokenless (adopted) dep or no dep in
                        /// either build ⇒ not recreatable (fail-loud on vanish, no INV-NO-LOSS masking).
                        const bool body_recreatable =
                            dst_st.build->depIsTokened(entry.file_hash)
                            || src_st.build->depIsTokened(entry.file_hash);
                        dst_st.build->reuseBlob(Cas::BlobId(Cas::u128ToHex(entry.file_hash)), body_recreatable);
                    }
                src_st.build->abandon();
            }
            parts.erase(src_it);

            /// B151: this is a freshly-written part being finalized tmp->final (the ONLY rename shape
            /// with a STAGED source here — DETACH/ATTACH/delete_tmp renames have a COMMITTED source and
            /// MISS this branch). renameParts() runs lock-free, so publish the FINAL ref NOW (off the
            /// data_parts lock) and mark it published; commit() then skips it. Single publish — the tmp
            /// ref was never durably published, so the republishRef below is a no-op for it.
            publishStaging(dst->ns, dst->ref, parts[dst_key]);
            /// B151 rollback safety: this ref was published BEFORE the owning transaction's commit
            /// decision (renameParts() precedes the ZK multi). If the transaction is abandoned, the
            /// destructor drops it (see ~ContentAddressedTransaction).
            rename_published_refs.emplace_back(dst->ns, dst->ref);
        }

        /// Move any COMMITTED source ref (a merge/mutation result rename, DETACH, ATTACH, a
        /// delete_tmp_ rename, an early-committed child ref being renamed away). Absent = a pure
        /// staged/tmp move - nothing durable to touch.
        republishRef(src->ns, src->ref, dst->ns, dst->ref);
        return;
    }

    throw Exception(ErrorCodes::LOGICAL_ERROR,
        "ContentAddressed: moveDirectory from {} to {} has an unsupported shape", path_from, path_to);
}

void ContentAddressedTransaction::moveFile(const std::string & path_from, const std::string & path_to)
{
    /// Verbatim table-level files and loose mountpoint files: physically move the object (the
    /// mutation entry tmp_mutation_N.txt -> mutation_N.txt rename; already durable from its finalize).
    if (!ContentAddressed::isPartFilePath(path_from) && !ContentAddressed::isPartFilePath(path_to))
    {
        auto move_table_verbatim = [&](const ContentAddressed::TableFilePath & src_tf,
                                       const ContentAddressed::TableFilePath & dst_tf)
        {
            const Cas::RootNamespace src_ns = metadata_storage.liveNamespace(src_tf.table_uuid);
            const Cas::RootNamespace dst_ns = metadata_storage.liveNamespace(dst_tf.table_uuid);
            if (src_ns.string() == dst_ns.string() && src_tf.tail == dst_tf.tail)
                return;
            auto bytes = metadata_storage.store()->getNamespaceFile(src_ns, src_tf.tail);
            if (!bytes)
                throw Exception(ErrorCodes::FILE_DOESNT_EXIST, "ContentAddressed: moveFile source missing: {}", path_from);
            metadata_storage.store()->putNamespaceFile(dst_ns, dst_tf.tail, *bytes);
            metadata_storage.store()->removeNamespaceFile(src_ns, src_tf.tail);
        };
        auto src_tf = ContentAddressed::parseTableFilePath(path_from);
        auto dst_tf = ContentAddressed::parseTableFilePath(path_to);
        if (src_tf && dst_tf)
        {
            move_table_verbatim(*src_tf, *dst_tf);
            return;
        }
        /// Loose mountpoint files (rare): read + put + remove plain objects.
        const std::string src_key = metadata_storage.serverId() + "/" + path_from;
        const std::string dst_key = metadata_storage.serverId() + "/" + path_to;
        if (src_key == dst_key)
            return;
        auto bytes = metadata_storage.store()->getMountpointObject(src_key);
        if (!bytes)
            throw Exception(ErrorCodes::FILE_DOESNT_EXIST, "ContentAddressed: moveFile source missing: {}", path_from);
        metadata_storage.store()->putMountpointObject(dst_key, *bytes);
        metadata_storage.store()->removeMountpointObject(src_key);
        return;
    }

    auto src = routeOf(path_from);
    auto dst = routeOf(path_to);
    /// A part-DIRECTORY rename reaching moveFile (PartsTemporaryRename::rollBackAll undoes an
    /// attach via moveFile - B87): delegate to moveDirectory, which owns directory shapes.
    if (src && dst && !src->ref.empty() && src->file.empty() && !dst->ref.empty() && dst->file.empty())
    {
        moveDirectory(path_from, path_to);
        return;
    }
    if (!src || src->file.empty() || !dst || dst->file.empty())
        throw Exception(ErrorCodes::LOGICAL_ERROR,
            "ContentAddressed: moveFile requires two part-file paths: {} -> {}", path_from, path_to);

    auto & src_st = stagingFor(*src);
    auto & dst_st = stagingFor(*dst);

    /// Staged mutable bytes re-key in place.
    if (auto mit = src_st.mutable_files.find(src->file); mit != src_st.mutable_files.end())
    {
        auto bytes = std::move(mit->second);
        src_st.mutable_files.erase(mit);
        dst_st.mutable_files[dst->file] = std::move(bytes);
        return;
    }
    /// Staged content entry re-keys in place (cross-part included; deps follow the entries).
    auto it = std::find_if(src_st.entries.begin(), src_st.entries.end(),
        [&](const Cas::TreeEntry & e) { return e.name == src->file; });
    if (it != src_st.entries.end())
    {
        auto entry = std::move(*it);
        src_st.entries.erase(it);
        entry.name = dst->file;
        if (&src_st != &dst_st && entry.placement == Cas::Placement::Blob)
        {
            /// B156b: the cross-part re-keyed entry came from src_st; its dep lives in src_st.build.
            /// Tokened (putBlob'd in this txn) ⇒ recreatable ⇒ retryable ABORTED on vanish; tokenless
            /// (adopted from a committed source) or no source build ⇒ not recreatable ⇒ fail-loud.
            const bool body_recreatable = src_st.build && src_st.build->depIsTokened(entry.file_hash);
            buildFor(*dst, dst_st).reuseBlob(Cas::BlobId(Cas::u128ToHex(entry.file_hash)), body_recreatable);
        }
        std::erase_if(dst_st.entries, [&](const Cas::TreeEntry & e) { return e.name == entry.name; });
        dst_st.entries.push_back(std::move(entry));
        return;
    }
    /// Source not staged in THIS transaction: a standalone one-shot rename of a COMMITTED mutable
    /// file (the MVCC txn_version.txt.tmp -> txn_version.txt move). Re-stage the committed bytes
    /// under the destination and mark the source removed; commit publishes via updateRefPayload.
    if (ContentAddressed::isMutablePerPartFile(dst->file))
    {
        /// Force-fresh (Pillar B): RENAME/move source read — stale mutable_files must not carry to dst.
        auto resolved = metadata_storage.store()->resolveRef(src->ns, src->ref);
        if (!resolved || !resolved->mutable_files.contains(src->file))
            throw Exception(ErrorCodes::FILE_DOESNT_EXIST,
                "ContentAddressed: moveFile source mutable file missing: {}", path_from);
        dst_st.mutable_files[dst->file] = resolved->mutable_files.at(src->file);
        src_st.mutable_removed.insert(src->file);
        return;
    }
    throw Exception(ErrorCodes::LOGICAL_ERROR, "ContentAddressed: moveFile source not staged: {}", path_from);
}

void ContentAddressedTransaction::replaceFile(const std::string & path_from, const std::string & path_to)
{
    /// replaceFile = moveFile that overwrites the destination. Drop any staged destination state
    /// first, then delegate (the verbatim branch's putNamespaceFile already overwrites).
    if (auto dst = routeOf(path_to); dst && !dst->file.empty())
    {
        auto & dst_st = stagingFor(*dst);
        std::erase_if(dst_st.entries, [&](const Cas::TreeEntry & e) { return e.name == dst->file; });
        dst_st.mutable_files.erase(dst->file);
        dst_st.mutable_removed.erase(dst->file);
    }
    moveFile(path_from, path_to);
}

void ContentAddressedTransaction::unlinkFile(const std::string & path, bool /*if_exists*/, bool /*should_remove_objects*/)
{
    /// Part file: drop the staged state so it never reaches the published tree. A mutable file
    /// that exists only in the COMMITTED payload is staged for removal (commit publishes the
    /// deletion via updateRefPayload - the MVCC removeTmpMetadataFile shape). Committed CONTENT
    /// files are never unlinked one-by-one on CA (the part's ref-drop is the removal unit) - the
    /// per-file unlinks of the MergeTree fast-removal path are no-ops here.
    if (auto r = routeOf(path); r && !r->file.empty())
    {
        auto & st = stagingFor(*r);
        const bool staged_here = st.mutable_files.contains(r->file)
            || std::any_of(st.entries.begin(), st.entries.end(),
                           [&](const Cas::TreeEntry & e) { return e.name == r->file; });
        std::erase_if(st.entries, [&](const Cas::TreeEntry & e) { return e.name == r->file; });
        st.mutable_files.erase(r->file);
        if (!staged_here && ContentAddressed::isMutablePerPartFile(r->file))
            st.mutable_removed.insert(r->file);
        return;
    }

    /// Verbatim table-level / loose mountpoint file: reclaim the object NOW (GC never scans them;
    /// a pruned mutation entry would otherwise leak until DROP - the PoC's B50).
    if (auto tf = ContentAddressed::parseTableFilePath(path))
    {
        metadata_storage.store()->removeNamespaceFile(metadata_storage.liveNamespace(tf->table_uuid), tf->tail);
        return;
    }
    /// Loose mountpoint file: exact-token delete of the plain object (design §5.2).
    metadata_storage.store()->removeMountpointObject(metadata_storage.serverId() + "/" + path);
}

void ContentAddressedTransaction::truncateFile(const std::string &, size_t)
{
    /// No-op: content-addressed blobs are immutable; committed part files are never truncated
    /// (whole-file rewrites replace the staged entry instead). PoC behavior, kept.
}

}
