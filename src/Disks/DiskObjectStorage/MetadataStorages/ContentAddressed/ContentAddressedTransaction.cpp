#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/ContentAddressedTransaction.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/ContentAddressedWriteBuffers.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasBuild.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasIds.h>
#include <IO/ReadBufferFromFile.h>
#include <IO/ReadBufferFromMemory.h>
#include <IO/WriteBufferFromFile.h>
#include <IO/copyData.h>
#include <algorithm>
#include <filesystem>
#include <unordered_set>
#include <Common/Exception.h>
#include <Common/HashTable/Hash.h>
#include <Common/getRandomASCIIString.h>
#include <Common/logger_useful.h>
#include <base/hex.h>
#include <base/scope_guard.h>
#include <city.h>
#include <ctime>

namespace DB::ContentAddressed
{

namespace
{

bool hasSuffix(std::string_view s, std::string_view suffix)
{
    return s.size() >= suffix.size() && s.substr(s.size() - suffix.size()) == suffix;
}

}

bool partFileMustStayBlob(std::string_view file_name)
{
    if (file_name == "primary.idx")
        return true;
    for (std::string_view suffix : {".bin", ".mrk", ".mrk2", ".mrk3", ".cmrk", ".cmrk2", ".cmrk3"})
        if (hasSuffix(file_name, suffix))
            return true;
    return false;
}

}

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

/// Inline candidates above this size spill to a blob instead of riding the tree object — a tuning
/// knob (could become a disk setting later). Keeps the tree object bounded against an unexpectedly
/// large eager file.
constexpr size_t INLINE_CAP = 1024 * 1024;   /// 1 MiB

}

ContentAddressedTransaction::ContentAddressedTransaction(ContentAddressedMetadataStorage & metadata_storage_)
    : metadata_storage(metadata_storage_)
{
}

ContentAddressedTransaction::~ContentAddressedTransaction()
{
    /// B188: always clean up pending temp files (whether committed or not). On the success path
    /// cleanupPendingTempFiles was already called at the end of commit(); this call is the defensive
    /// backstop for aborted/exception-unwound transactions whose publishStaging never ran.
    cleanupPendingTempFiles();

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
    /// destination and drop the source ref. Returns false (nothing written) when the source ref is absent.
    /// Force-fresh (Pillar B): RENAME/move source read — stale mutable_files must not carry to dst.
    auto resolved = metadata_storage.store()->resolveRef(src_ns, src_ref);
    if (!resolved)
        return false;
    auto build = metadata_storage.store()->startBuild(
        Cas::BuildInfo{.intended_ref = dst_ns.string() + "/" + dst_ref, .op = Cas::ProvenanceOp::Other});
    /// B190 Task 4 (precommit-first): record a TOKENLESS tree-evidence dep WITHOUT HEADing the tree
    /// object. A synthetic Subtree TreeEntry carries the (hash, size) from the resolved ref metadata —
    /// no pool GET/HEAD needed. The merged publish gate (post-precommit) re-proves the dep at publish.
    /// This eliminates the adoptTree → observeAndAdmit → HEAD that happened before precommit.
    Cas::TreeEntry tree_evidence;
    tree_evidence.placement = Cas::Placement::Subtree;
    tree_evidence.file_hash = Cas::hexToU128(resolved->tree_id.string());
    tree_evidence.file_size = resolved->tree_size;
    build->adoptEvidence(tree_evidence);
    Cas::RefPayload payload;
    payload.tree_size = resolved->tree_size;
    payload.mutable_files = resolved->mutable_files;
    payload.published_at_ms = resolved->published_at_ms;   /// the publish stamp carries over (a rename is not a new part)
    /// B190 precommit-first: protect the adopted closure via a build-root precommit BEFORE the
    /// fail-closed publish. The publish gate (checkAndResolveDeps) re-proves the tree dep at publish time.
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

void ContentAddressedTransaction::cleanupPendingTempFiles() noexcept
{
    for (auto & [key, st] : parts)
    {
        for (const auto & pb : st.pending_blobs)
        {
            std::error_code ec;
            std::filesystem::remove(pb.temp_path, ec);
        }
        st.pending_blobs.clear();
    }
}

const ContentAddressedTransaction::PartStaging::PendingBlob *
ContentAddressedTransaction::findPendingBlob(const PartStaging & st, const UInt128 & hash) const
{
    /// B188: locate a pending blob by hash. Returns nullptr when the blob has already been uploaded
    /// (post-precommit, pending_blobs is cleared) or was never staged as pending.
    for (const auto & pb : st.pending_blobs)
        if (pb.hash == hash)
            return &pb;
    return nullptr;
}

void ContentAddressedTransaction::adoptStagedBlob(
    const PartStaging::PendingBlob * pb, const Cas::TreeEntry & entry,
    PartStaging & dst_st, Cas::Build & dst_build, bool copy_pending)
{
    if (pb)
    {
        /// Pending blob (not yet uploaded): record a tokenless dep so stageTree's W-TREE-BUILD
        /// check passes without any pool op. If copy_pending, push a copy of the pb record into
        /// dst_st so publishStaging uploads it for the dst part too (hardlink = copy semantics).
        /// If !copy_pending, the record is already in dst_st (moved by caller) — skip the push.
        if (copy_pending)
            dst_st.pending_blobs.push_back(*pb);
        dst_build.recordPendingBlobDep(entry.file_hash, entry.file_size);
    }
    else
    {
        /// Uploaded / committed: record a tokenless W-EVIDENCE dep — no pool HEAD/GET before
        /// precommit. The publish gate (post-precommit) observes/resurrects it if needed.
        dst_build.adoptEvidence(entry);
    }
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

    /// B188 precommit-first: stage the tree LOCALLY (no upload), precommit to protect the whole
    /// closure by reachability, THEN do every pool write (tree object + pending blobs) and publish.
    const Cas::TreeId tree = st.build->stageTree(st.entries);

    Cas::RefPayload payload;
    payload.mutable_files = st.mutable_files;
    payload.published_at_ms = static_cast<uint64_t>(::time(nullptr)) * 1000;

    st.build->precommit(tree);                       /// closure now reachable from a durable build root

    /// B189: build the set of blob hashes actually referenced by the staged tree. Only
    /// Placement::Blob entries represent pending content uploads — Subtree/Inline are
    /// not pending blobs. A pending_blob whose hash is NOT in this set had its tree entry removed
    /// by unlinkFile/replaceFile and must not be uploaded (it is an orphan). Its temp file is
    /// still cleaned by cleanupPendingTempFiles at commit end.
    std::unordered_set<UInt128, UInt128Hash> referenced_hashes;
    for (const auto & entry : st.entries)
        if (entry.placement == Cas::Placement::Blob)
            referenced_hashes.insert(entry.file_hash);

    st.build->uploadStagedTree(tree);                /// pool write #1 — under protection
    for (const auto & pb : st.pending_blobs)         /// pool writes #2 — uploads + 412/HEAD/resurrect
    {
        if (!referenced_hashes.count(pb.hash))
            continue;   /// B189: orphaned pending blob (entry removed by unlinkFile/replaceFile) — skip
        Cas::BlobSource source;
        source.size = pb.size;
        const std::string temp_path = pb.temp_path;
        source.write_payload = [temp_path](WriteBuffer & out)
        {
            ReadBufferFromFile in(temp_path);
            copyData(in, out);
        };
        st.build->putBlob(Cas::BlobId(Cas::u128ToHex(pb.hash)), std::move(source));
    }

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
    /// B188: temp files for all pending blobs have been uploaded in publishStaging; remove them now.
    cleanupPendingTempFiles();
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
    /// the parent part's single commit. Staged content blobs may be pending (B188: not yet uploaded).
    auto r = const_cast<ContentAddressedTransaction *>(this)->routeOf(path);
    if (!r || r->file.empty())
        return {};
    auto it = parts.find({r->ns.string(), r->ref});
    if (it == parts.end())
        return {};
    if (const auto * entry = findStagedEntry(*r))
    {
        if (entry->placement == Cas::Placement::Blob)
        {
            /// B188: a pending blob has not been uploaded yet — its storage object does not exist in
            /// the pool. Return empty so the caller falls back to tryReadFileInFlight (local temp read).
            if (findPendingBlob(it->second, entry->file_hash))
                return {};
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
        {
            /// B188: a pending blob has not been uploaded yet — serve reads from the local temp file
            /// (the same file that will be streamed to the pool in publishStaging post-precommit).
            if (const auto * pb = findPendingBlob(it->second, entry->file_hash))
                return std::make_unique<ReadBufferFromFile>(pb->temp_path);
            return metadata_storage.readBlobPayload(metadata_storage.store()->locate(*entry), path, settings);
        }
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

void ContentAddressedTransaction::stageBlobPartFile(
    const ContentAddressedMetadataStorage::Route & route,
    const UInt128 & hash, size_t size, const std::string & temp_path)
{
    /// B188: do NOT upload here. Record the pending blob (uploaded post-precommit in publishStaging)
    /// and a tokenless dep so stageTree's W-TREE-BUILD check passes; putBlob later overwrites it with
    /// the tokened dep. The temp file is kept (the transaction owns it).
    auto & st = stagingFor(route);
    st.pending_blobs.push_back({hash, temp_path, size});
    buildFor(route, st).recordPendingBlobDep(hash, size);

    Cas::TreeEntry entry;
    entry.name = route.file;
    entry.placement = Cas::Placement::Blob;
    entry.file_hash = hash;
    entry.file_size = size;
    std::erase_if(st.entries, [&](const Cas::TreeEntry & e) { return e.name == entry.name; });
    st.entries.push_back(std::move(entry));
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

    /// A CONTENT part file that must stay a blob (per-column data/marks, primary.idx): spill + hash
    /// into a local temp file, then stage the blob as PENDING (B188 precommit-first). The blob is NOT
    /// uploaded here; publishStaging uploads it post-precommit. recordPendingBlobDep (inside
    /// stageBlobPartFile) lets stageTree's W-TREE-BUILD check pass without any pool op at staging time.
    if (ContentAddressed::partFileMustStayBlob(r->file))
    {
        return std::make_unique<ContentAddressed::CaContentWriteBuffer>(
            metadata_storage.scratchPath(),
            buf_size,
            settings.use_adaptive_write_buffer,
            settings.adaptive_write_buffer_initial_size,
            [this, route = *r](const std::string & hash_hex, size_t size, const std::string & temp_path)
            {
                stageBlobPartFile(route, Cas::hexToU128(hash_hex), size, temp_path);
            });
    }

    /// Inline candidate (small eager metadata): buffer in memory, decide at finalize. <= INLINE_CAP
    /// rides the single tree object as an Inline entry (one-GET part open, B10/B97); an oversized
    /// candidate spills to a blob (the safety net).
    return std::make_unique<ContentAddressed::CaInlineWriteBuffer>(
        [this, route = *r](std::string bytes)
        {
            /// Hash via the SAME hex round-trip the blob path (CaContentWriteBuffer) uses, so an inline file
            /// and a standalone blob of identical content get the same file_hash (inline == blob for treeId).
            const UInt128 hash = Cas::hexToU128(
                getHexUIntLowercase(CityHash_v1_0_2::CityHash128(bytes.data(), bytes.size())));
            if (bytes.size() <= INLINE_CAP)
            {
                auto & st = stagingFor(route);
                Cas::TreeEntry entry;
                entry.name = route.file;
                entry.placement = Cas::Placement::Inline;
                entry.file_hash = hash;            /// content hash — inline == blob for the Merkle id
                entry.file_size = bytes.size();
                entry.inline_bytes = std::move(bytes);
                std::erase_if(st.entries, [&](const Cas::TreeEntry & e) { return e.name == entry.name; });
                st.entries.push_back(std::move(entry));
            }
            else
            {
                /// Safety fallback: an unexpectedly large candidate spills to a blob (preserves the
                /// invariant that big files are not held inline). Write the buffered bytes to a unique
                /// local temp file (same scratchPath + random-name scheme as CaContentWriteBuffer), then
                /// stage exactly like a streaming blob.
                std::filesystem::create_directories(metadata_storage.scratchPath());
                const std::string temp_path =
                    metadata_storage.scratchPath() + "/inline_overflow_" + getRandomASCIIString(32) + ".tmp";
                {
                    WriteBufferFromFile tmp(temp_path);
                    tmp.write(bytes.data(), bytes.size());
                    tmp.finalize();
                }
                /// Until stageBlobPartFile takes ownership, WE own the temp file — mirror the blob path,
                /// where CaContentWriteBuffer's dtor removes it unless the callback succeeded. If
                /// stageBlobPartFile throws, drop the orphan instead of leaking it into scratch.
                bool staged = false;
                SCOPE_EXIT({ if (!staged) { std::error_code ec; std::filesystem::remove(temp_path, ec); } });
                stageBlobPartFile(route, hash, bytes.size(), temp_path);
                staged = true;
            }
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

    /// Table dir: the table's namespace (live + folded-in detached refs, B181) and every verbatim
    /// file go in one dropNamespace.
    if (auto uuid = ContentAddressed::parseTableUuid(path))
    {
        metadata_storage.store()->dropNamespace(metadata_storage.liveNamespace(*uuid));
        return;
    }

    if (auto p = ContentAddressed::parsePartFilePath(path))
    {
        auto r = metadata_storage.route(*p);
        /// The detached CONTAINER dir (DROP DETACHED / table-detach): drop all detached refs (B181).
        if (r && r->ref.empty() && p->part_name == ContentAddressed::kDetachedDirName)
        {
            for (const auto & ref : metadata_storage.detachedRefNames(r->ns))
                dropRefIfPresent(r->ns, ref);
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
                /// B190 Task 4: unified adopt dispatch. copy_pending=(&dst_st != src_st) so the pending
                /// blob record is copied into dst_st only when the destination is a different part
                /// (hardlink = copy semantics; same-part is a self-ref that shouldn't duplicate the record).
                const auto * pb = findPendingBlob(*src_st, entry.file_hash);
                adoptStagedBlob(pb, entry, dst_st, buildFor(*dst, dst_st), /*copy_pending=*/(&dst_st != src_st));
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

    /// RENAME TABLE / cross-engine move: both endpoints are TABLE dirs. Republish every ref (live
    /// AND folded-in `detached/`-prefixed refs, B181) plus every verbatim file under the new table
    /// identity, then drop the old namespace (the blobs/trees are content-addressed and untouched).
    ///
    /// B126 — no native cross-namespace atomicity (object storage has no directory rename, unlike a
    /// non-CAS disk where RENAME TABLE is a single atomic directory rename). This is a best-effort
    /// multi-op move, but it is RE-DRIVABLE/IDEMPOTENT: `republishRef` no-ops when the source ref is
    /// already gone (resolveRef miss after a prior drive moved it), `putNamespaceFile` is
    /// last-writer-wins (re-putting identical bytes is a no-op), and `dropNamespace` of an
    /// already-empty/absent namespace is a no-op. So a mid-loop throw leaves the table SPLIT across the
    /// two namespaces, but re-driving the SAME rename completes it. There is no in-call compensation;
    /// true atomicity would need a durable move-journal (deliberately out of scope — it would touch the
    /// tested GC/journal layer). On partial failure we log loudly so the split state is diagnosable.
    if (auto src_table = ContentAddressed::parseTableUuid(path_from))
    {
        if (auto dst_table = ContentAddressed::parseTableUuid(path_to))
        {
            const auto from_ns = metadata_storage.liveNamespace(*src_table);
            const auto to_ns = metadata_storage.liveNamespace(*dst_table);
            try
            {
                for (const auto & [ref, _] : metadata_storage.store()->listRefs(from_ns))
                    republishRef(from_ns, ref, to_ns, ref);
                for (const auto & name : metadata_storage.store()->listNamespaceFiles(from_ns))
                    if (auto bytes = metadata_storage.store()->getNamespaceFile(from_ns, name))
                        metadata_storage.store()->putNamespaceFile(to_ns, name, *bytes);
                metadata_storage.store()->dropNamespace(from_ns);
            }
            catch (...)
            {
                LOG_ERROR(getLogger("ContentAddressedTransaction"),
                    "RENAME TABLE move was only partially applied: the table is SPLIT across namespaces "
                    "'{}' and '{}'. The move is idempotent — retrying the same RENAME re-drives it to "
                    "completion (already-moved refs/files are no-ops). Underlying error: {}",
                    from_ns.string(), to_ns.string(), getCurrentExceptionMessage(/*with_stacktrace=*/false));
                throw;
            }
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

        /// Re-key a STAGED source into the destination (B67: merge stagings). B124: a move carries the
        /// SOURCE's content to the destination — the POSIX `rename` semantic the rest of MergeTree
        /// assumes, and exactly what `moveFile` does (`dst[file] = src_bytes`). On the happy path the
        /// destination staging is freshly-created/empty so there is no collision at all; this only
        /// matters if some future op-order stages the same mutable file under BOTH keys.
        bool had_staged_source = false;
        if (auto src_it = parts.find(src_key); src_it != parts.end())
        {
            had_staged_source = true;
            PartStaging & dst_st = parts[dst_key];
            PartStaging & src_st = src_it->second;
            for (auto & entry : src_st.entries)
            {
                std::erase_if(dst_st.entries, [&](const Cas::TreeEntry & e) { return e.name == entry.name; });
                dst_st.entries.push_back(std::move(entry));
            }
            for (auto & [file, bytes] : src_st.mutable_files)
            {
                /// B124: source-wins (aligned with moveFile / POSIX rename). A genuine collision with
                /// DIFFERING bytes means the assumed operation order is wrong — fail loud rather than
                /// silently dropping a just-written mutable file (the lost-update hazard). Identical
                /// bytes are a benign re-key (idempotent), so only differing bytes throw.
                if (auto dit = dst_st.mutable_files.find(file);
                    dit != dst_st.mutable_files.end() && dit->second != bytes)
                    throw Exception(ErrorCodes::LOGICAL_ERROR,
                        "ContentAddressed: moveDirectory mutable-file collision on '{}' ({} -> {}): "
                        "source and destination staged different bytes for the same file",
                        file, src->ref, dst->ref);
                dst_st.mutable_files[file] = std::move(bytes);
            }
            for (const auto & file : src_st.mutable_removed)
                dst_st.mutable_removed.insert(file);
            /// B188: move pending blobs from src to dst — they will be uploaded in dst's publishStaging.
            for (auto & pb : src_st.pending_blobs)
                dst_st.pending_blobs.push_back(std::move(pb));
            src_st.pending_blobs.clear();
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
                        /// B190 Task 4: unified adopt dispatch. Pending blob records were already moved
                        /// to dst_st.pending_blobs above (MOVE semantics), so copy_pending=false.
                        adoptStagedBlob(findPendingBlob(dst_st, entry.file_hash), entry, dst_st, *dst_st.build, /*copy_pending=*/false);
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

        if (had_staged_source)
        {
            /// A freshly-written part finalized tmp->final: the staged manifest published above is the
            /// part's AUTHORITATIVE content. The B151 invariant assumes the tmp ref "was never durably
            /// published", so the move below would be a no-op — but a nested sub-storage CAN durably
            /// publish a committed ref AT THE PART'S OWN PATH: the vertical-merge / mutation text-index
            /// builder (MergeTask / MutateTask createTemporaryTextIndexStorage) writes scratch under
            /// `<part>/text_index_tmp/` through a SEPARATE DataPartStorage whose own commitTransaction
            /// publishes a committed `<part>` ref holding only those scratch files. republishing that
            /// over the just-published real manifest CLOBBERS the part (skip-index / statistics files
            /// vanish from the tree, B183). Drop the spurious scratch ref instead — its blobs become
            /// unreachable and GC reclaims them; the real manifest at `dst` stands.
            dropRefIfPresent(src->ns, src->ref);
            return;
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
            /// B123: a verbatim rename is emulated as get(src) -> put(dst) -> remove(src) because object
            /// storage has no atomic rename. SINGLE-WRITER CONTRACT: only the owning server renames its
            /// own table-level verbatim files (mutation entries), so there is no concurrent writer to
            /// race the blind put(dst) against — the put's last-writer-wins is safe under that contract.
            /// Idempotent re-drive: if the source is already gone but the destination is present, a
            /// previous drive completed this move — treat as done (matches a re-driven FS rename, which
            /// is an ENOENT-tolerant no-op) instead of throwing FILE_DOESNT_EXIST.
            auto bytes = metadata_storage.store()->getNamespaceFile(src_ns, src_tf.tail);
            if (!bytes)
            {
                if (metadata_storage.store()->getNamespaceFile(dst_ns, dst_tf.tail))
                    return;
                throw Exception(ErrorCodes::FILE_DOESNT_EXIST, "ContentAddressed: moveFile source missing: {}", path_from);
            }
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
        /// Loose mountpoint files (rare): read + put + remove plain objects. Same B123 single-writer
        /// contract + idempotent re-drive as the table-verbatim branch above.
        const std::string src_key = metadata_storage.serverId() + "/" + path_from;
        const std::string dst_key = metadata_storage.serverId() + "/" + path_to;
        if (src_key == dst_key)
            return;
        auto bytes = metadata_storage.store()->getMountpointObject(src_key);
        if (!bytes)
        {
            if (metadata_storage.store()->getMountpointObject(dst_key))
                return;
            throw Exception(ErrorCodes::FILE_DOESNT_EXIST, "ContentAddressed: moveFile source missing: {}", path_from);
        }
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

    /// Staged mutable bytes re-key in place. B124 canonical policy: SOURCE-wins — a move/rename carries
    /// the source's content to the destination, overwriting any prior dest bytes (the POSIX `rename`
    /// semantic, and what the atomic-write `.tmp -> final` rename requires). moveDirectory's staged
    /// merge is aligned to this same policy.
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
            /// B190 Task 4: unified adopt dispatch. MOVE semantics — physically move the pending blob
            /// record from src_st to dst_st FIRST (so dst_st owns the upload), then call adoptStagedBlob
            /// with copy_pending=false (the record is already in dst_st; no additional copy needed).
            auto pb_it = std::find_if(src_st.pending_blobs.begin(), src_st.pending_blobs.end(),
                [&](const PartStaging::PendingBlob & pb) { return pb.hash == entry.file_hash; });
            if (pb_it != src_st.pending_blobs.end())
            {
                dst_st.pending_blobs.push_back(std::move(*pb_it));
                src_st.pending_blobs.erase(pb_it);
            }
            adoptStagedBlob(findPendingBlob(dst_st, entry.file_hash), entry, dst_st, buildFor(*dst, dst_st), /*copy_pending=*/false);
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
        /// A matching pending_blobs record (if any) is left in place — its temp file is cleaned by
        /// cleanupPendingTempFiles at commit end, and the orphaned record is filtered out of the
        /// publish upload by the staged-tree-hash check in publishStaging (B189). We do NOT purge it
        /// eagerly because the same hash may still be referenced by another staged entry.
        std::erase_if(dst_st.entries, [&](const Cas::TreeEntry & e) { return e.name == dst->file; });
        dst_st.mutable_files.erase(dst->file);
        dst_st.mutable_removed.erase(dst->file);
    }
    moveFile(path_from, path_to);
}

void ContentAddressedTransaction::unlinkFile(const std::string & path, bool /*if_exists*/, bool /*should_remove_objects*/)
{
    /// Part file. Three sub-cases:
    ///   1. A file STAGED in this transaction (content entry or mutable bytes): drop the staged state
    ///      so it never reaches the published tree.
    ///   2. A MUTABLE file that exists only in the COMMITTED payload: stage it for removal so commit
    ///      publishes the deletion via updateRefPayload (the MVCC removeTmpMetadataFile shape).
    ///   3. A COMMITTED CONTENT file (not staged here, not mutable): **deliberate NO-OP**.
    ///
    /// B123 — LOAD-BEARING INVARIANT, do not "fix" with a blanket fail-closed assert:
    /// On a content-addressed disk a committed part is ONE atomic ref (its manifest tree); the removal
    /// UNIT is the whole-part ref-drop done by `removeDirectory(<part>)`, NOT per-file unlinks. The
    /// MergeTree fast-removal path (IMergeTreeDataPart::remove) unlinks EVERY part file one-by-one and
    /// THEN calls `removeDirectory` — so these per-file unlinks of committed content files MUST be
    /// no-ops here, and `removeDirectory` is what actually frees the part. An assertion that "a
    /// committed content-file unlink never happens" would therefore fire on every normal part removal
    /// and break it.
    ///
    /// The cost of this design is a narrow FAIL-OPEN: if some caller ever unlinks a SINGLE committed
    /// content file expecting it gone WITHOUT dropping the whole part, the bytes survive in the
    /// manifest (a no-op here), whereas on a non-CAS disk the file would actually be deleted. This
    /// shape does NOT occur in MergeTree today (a committed content file is only ever removed as part
    /// of a whole-part removal, i.e. followed by `removeDirectory`); the invariant holds because the
    /// MergeTree layer treats a part directory as the indivisible removal unit. If that ever changes
    /// (a code path that surgically deletes one committed content file and relies on it being gone),
    /// this no-op becomes a correctness bug and the removal must instead go through ref-drop.
    if (auto r = routeOf(path); r && !r->file.empty())
    {
        auto & st = stagingFor(*r);
        const bool staged_here = st.mutable_files.contains(r->file)
            || std::any_of(st.entries.begin(), st.entries.end(),
                           [&](const Cas::TreeEntry & e) { return e.name == r->file; });
        /// A matching pending_blobs record (if any) is left in place — its temp file is cleaned by
        /// cleanupPendingTempFiles at commit end, and the orphaned record is filtered out of the
        /// publish upload by the staged-tree-hash check in publishStaging (B189). We do NOT purge it
        /// eagerly because the same hash may still be referenced by another staged entry.
        std::erase_if(st.entries, [&](const Cas::TreeEntry & e) { return e.name == r->file; });
        st.mutable_files.erase(r->file);
        if (!staged_here && ContentAddressed::isMutablePerPartFile(r->file))
            st.mutable_removed.insert(r->file);
        /// else: a committed CONTENT file → sub-case 3, the deliberate no-op documented above.
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
