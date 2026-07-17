#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/ContentAddressedTransaction.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/ContentAddressedTransaction.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasPartWriteTxn.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Formats/CasBlobEnvelopeFormat.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Primitives/CasTypes.h>
#include <IO/ReadBufferFromFile.h>
#include <IO/ReadBufferFromFileView.h>
#include <IO/ReadBufferFromMemory.h>
#include <IO/WriteBufferFromFile.h>
#include <IO/copyData.h>
#include <Disks/IO/WriteBufferWithFinalizeCallback.h>
#include <Disks/IDiskTransaction.h>
#include <Common/thread_local_rng.h>
#include <Common/config_version.h>
#include <algorithm>
#include <filesystem>
#include <unordered_set>
#include <Common/Exception.h>
#include <Common/HashTable/Hash.h>
#include <Common/getRandomASCIIString.h>
#include <Common/logger_useful.h>
#include <base/hex.h>
#include <base/scope_guard.h>
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
    /// Historical note: these were the M-W milestone skeleton stubs (plan 2026-06-12); the ones
    /// still standing are generic disk-transaction operations that have no content-addressed
    /// equivalent or are not wired yet. Keep the message self-explanatory — operators see it.
    throw Exception(ErrorCodes::NOT_IMPLEMENTED,
        "The operation '{}' is not implemented for a content-addressed disk: it belongs to the "
        "generic disk-transaction surface that the content-addressed write path does not use. "
        "Hitting it usually means the disk is wrapped by a layer that bypasses the "
        "content-addressed write path.", op);
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

    /// An uncommitted transaction's uploads become min_active-spared debris: abandon every
    /// still-open PartWriteTxn so its build_seq is retired. Replaces the PoC's pin machinery.
    if (committed)
        return;

    /// [TXN-ONE-PIPELINE] No refs are published before commit() any more (moveDirectory tmp->final is a
    /// pure re-key, Task 1.1), so an abandoned transaction has no early-published ref to compensate — the
    /// final ref simply never became durable. The only cleanup left is abandoning still-open Builds below.
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

Cas::PartWriteTxn & ContentAddressedTransaction::buildFor(
    const ContentAddressedMetadataStorage::Route & r, PartStaging & st)
{
    if (!st.build)
        st.build = metadata_storage.store()->beginPartWrite(
            Cas::PartWriteInfo{.intended_ref = r.ns.string() + "/" + r.ref,
                           .intended_namespace = r.ns, .op = Cas::ProvenanceOp::Insert});
    return *st.build;
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
            if (pb.backend == StagingBackend::Local)
            {
                std::error_code ec;
                std::filesystem::remove(pb.staging_key, ec);
            }
            else if (committed)
            {
                /// Task 6 (S3-native staging plan): a successful commit deletes the S3 staging object
                /// HERE — `committed` is only ever true when EVERY part's `publishStaging` ran to
                /// completion (commit() sets it right before this call), which means every referenced
                /// pending blob was already promoted (`PartWriteTxn::putBlob` → `promoteStaged`/`resurrectStaged`)
                /// or, for a B189-orphaned pending blob (its entry removed by `unlinkFile`/`replaceFile`
                /// before commit), was never going to be promoted at all — either way the staging object
                /// is no longer needed as a resurrect source, so it is safe to reclaim now.
                ///
                /// An ABORTED/exception-unwound transaction (`committed == false`, including a partial
                /// multi-part commit failure where an EARLIER part's blobs were already promoted) leaves
                /// its S3 staging objects in place — `staging_key` is a remote object-storage key, never a
                /// bare `fs::remove` target, and is reclaimed by the mount-lease-scoped sweeper
                /// (`Cas::sweepOwnMountStaging`), never here. This mirrors the local path's own asymmetry:
                /// `Local` staging is a private per-transaction scratch file removed unconditionally on
                /// both commit and abort (nobody else can ever read it), whereas an `S3` staging object
                /// is the sanctioned resurrect source for the promote gate and must outlive an aborted
                /// transaction so a later attempt (or the sweeper) can still account for it.
                try
                {
                    metadata_storage.objectStorage()->removeObjectIfExists(StoredObject(pb.staging_key));
                }
                catch (...) // NOLINT(bugprone-empty-catch)
                {
                    /// Best-effort (noexcept context): a stubborn delete just leaves debris for the
                    /// mount-lease sweeper to reclaim on a later mount.
                }
            }
            /// else: an S3-mode pending blob of an ABORTED transaction — intentionally left in place
            /// (see above); the mount-lease sweeper (`Cas::sweepOwnMountStaging`) is its reclaimer.
        }
        st.pending_blobs.clear();
    }
}

const ContentAddressedTransaction::PartStaging::PendingBlob *
ContentAddressedTransaction::findPendingBlob(const PartStaging & st, const Cas::BlobRef & ref) const
{
    /// B188: locate a pending blob by ref. Returns nullptr when the blob has already been uploaded
    /// (post-precommit, pending_blobs is cleared) or was never staged as pending.
    for (const auto & pb : st.pending_blobs)
        if (pb.ref == ref)
            return &pb;
    return nullptr;
}

void ContentAddressedTransaction::adoptStagedBlob(
    const PartStaging::PendingBlob * pb, const Cas::ManifestEntry & entry,
    PartStaging & dst_st, Cas::PartWriteTxn & dst_build, bool copy_pending)
{
    if (pb)
    {
        /// Pending blob (not yet uploaded): record a tokenless dependency without any pool operation.
        /// If copy_pending, push a copy of the pb record into dst_st so publishStaging uploads it
        /// for the dst part too (hardlink = copy semantics). If !copy_pending, the record is already
        /// in dst_st (moved by caller) — skip the push.
        if (copy_pending)
            dst_st.pending_blobs.push_back(*pb);
        dst_build.recordPendingBlobDep(entry.ref, entry.blob_size);
    }
    else
    {
        /// Uploaded / committed: record a tokenless W-EVIDENCE dep — no pool HEAD/GET before precommit.
        /// §4 manifest-trust: the publish gate (promote) TRUSTS this committed-source adopted leaf via the
        /// durable manifest edge — it does NOT observe/resurrect it. Only tokened / pending-upload leaves
        /// are resurrected (by putBlob, before promote); a genuinely-absent adopted blob is an fsck finding.
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

void ContentAddressedTransaction::uploadPendingBlobs(PartStaging & st)
{
    /// B189: build the set of blob hashes actually referenced by the staged manifest. Only Blob
    /// entries represent pending content uploads — Inline are not pending blobs. A pending_blob whose
    /// hash is NOT in this set had its entry removed by unlinkFile/replaceFile and must not be uploaded
    /// (it is an orphan). Its temp file is still cleaned by cleanupPendingTempFiles at commit end.
    std::unordered_set<Cas::BlobRef, Cas::BlobRefHash> referenced_hashes;
    for (const auto & entry : st.entries)
        if (entry.placement == Cas::EntryPlacement::Blob)
            referenced_hashes.insert(entry.ref);

    for (const auto & pb : st.pending_blobs)         /// pool writes — uploads + 412/HEAD/resurrect
    {
        if (!referenced_hashes.count(pb.ref))
            continue;   /// B189: orphaned pending blob (entry removed by unlinkFile/replaceFile) — skip
        /// Task 5 (plan docs/superpowers/plans/2026-07-11-cas-s3-native-staging.md): each pending blob
        /// is promoted through the SAME condemn/resurrect gate in `PartWriteTxn::putBlob`. Only the upload
        /// primitive differs by staging backend:
        ///   - `StagingBackend::Local`: `write_payload` re-reads the local staged temp file and streams
        ///     it into a write-once `putIfAbsentStream` create (byte-for-byte the pre-Task-5 path).
        ///   - `StagingBackend::S3`: the bytes already live in an S3 staging object (`pb.staging_key`);
        ///     `server_side_copy_from` drives a WRITE-ONCE conditional SERVER-SIDE COPY (and an
        ///     unconditional resurrect copy FROM the staging object for a condemned incarnation). No
        ///     local read-back — `write_payload` is left unset.
        Cas::BlobSource source;
        source.size = pb.size;
        if (pb.backend == StagingBackend::S3)
        {
            source.server_side_copy_from = pb.staging_key;
        }
        else
        {
            const std::string staging_key = pb.staging_key;
            source.write_payload = [staging_key](WriteBuffer & out)
            {
                ReadBufferFromFile in(staging_key);
                copyData(in, out);
            };
        }
        st.build->putBlob(pb.ref, std::move(source));
    }
}

bool ContentAddressedTransaction::publishStaging(const Cas::RootNamespace & ns, const std::string & ref, PartStaging & st)
{
    if (st.published)
        return false;   /// this staging was already published earlier in this commit loop — never re-publish

    if (!st.build && st.entries.empty() && st.content_removed.empty())
    {
        /// Nothing staged for this ref this transaction -- a touched-but-empty PartStaging (e.g. the
        /// harmless residue of a removeDirectory that already superseded this staging's marks,
        /// content_removed cleared to empty). Benign no-op.
        st.published = true;
        return false;
    }

    /// Committed-ref standalone writes AND removal marks (all-tree-part-files Task 4 + Task 8, spec
    /// 2026-07-14-cas-all-tree-part-files-design.md §4/§6): `st.entries` here holds only the
    /// CHANGED/ADDED entries (see stageBlobPartFile / the inline writeFile path) — never the whole
    /// part once the ref already exists; `st.content_removed` holds paths a same-transaction
    /// unlinkFile staged for removal (§6). Carry every OTHER committed entry forward (minus any
    /// content_removed path) and republish once via the repoint path (Task 3), rather than letting
    /// the PartWriteTxn path below replace the manifest with just the delta (which Task 2's promote guard
    /// would now reject with ABORTED for a genuine content change, or silently drop the untouched
    /// files for a pre-Task-2 build). Handles both sub-cases the interface allows: entries staged
    /// WITH a PartWriteTxn (this transaction uploaded new content) and WITHOUT one (a former mutable
    /// per-part file that is now an ordinary tree entry, or a marks-only removal with no writes).
    if (!st.entries.empty() || !st.content_removed.empty())
    {
        if (auto view = metadata_storage.partAccess().getView({ns, ref}, ContentAddressed::Freshness::ForceFresh))
        {
            if (st.build)
            {
                /// EDGE-BEFORE-OBSERVE (spec 2026-07-09-cas-writer-gc-simplification) is still load-
                /// bearing here: a fresh blob's hash must be durably NAMED by a live precommit's
                /// manifest body before `putBlob` makes its first backend observation. `repointRef`
                /// below promotes through its OWN internal build (`adoptEvidence`, no `putBlob`) — it
                /// protects entries whose content ALREADY exists (the carried-forward ones, and this
                /// transaction's uploads once they land), but cannot itself protect a brand-new upload
                /// made mid-repoint. So THIS build stages+precommits a SCRATCH manifest over
                /// `st.entries` (BEFORE it is merged/moved below) — exactly the same closure the normal
                /// (non-repoint) path further down establishes with `st.build->stageManifest(st.entries)`
                /// + `precommitAdd`, which already names every hash this transaction is about to upload
                /// — purely to hold that edge across the upload loop. Once `repointRef`'s own promote
                /// makes the real (merged) manifest live, this scratch precommit is abandoned; it never
                /// gets promoted. A marks-only removal never enters this sub-block (`st.build` is null).
                const Cas::ManifestId scratch_id = st.build->stageManifest(st.entries);
                st.build->precommitAdd(ns, ref, scratch_id);
                uploadPendingBlobs(st);
            }

            std::vector<Cas::ManifestEntry> merged;
            for (const auto & e : view->manifest()->entries)
                if (!st.content_removed.contains(e.path)
                    && std::none_of(st.entries.begin(), st.entries.end(),
                                     [&](const Cas::ManifestEntry & s) { return s.path == e.path; }))
                    merged.push_back(e);
            for (auto & s : st.entries)
                merged.push_back(std::move(s));

            metadata_storage.partAccess().repointRef({ns, ref}, std::move(merged), Cas::ProvenanceOp::Other);
            if (st.build)
                st.build->abandon();   /// scratch precommit's protecting job is done; the real manifest is live
            st.published = true;
            return false;   /// a repoint never creates a new ref
        }
    }

    if (!st.build)
        throw Exception(ErrorCodes::LOGICAL_ERROR,
            "ContentAddressedTransaction: staged entries or removal marks for {}/{} without a Build", ns.string(), ref);

    /// Write path (rev. 15): stage the part manifest body (mints a ManifestId), precommitAdd a
    /// build-intent owner (closure now protected by reachability), upload the pending blobs, then
    /// promote — an atomic owner move that revalidates every non-tokened blob fail-closed.
    ///
    /// ORDERING IS LOAD-BEARING (EDGE-BEFORE-OBSERVE, spec 2026-07-09-cas-writer-gc-simplification):
    /// precommitAdd's durable closure names EVERY blob hash BEFORE putBlob makes the first backend
    /// observation. This is what lets promote skip re-validating tokened leaves (a condemnation in the
    /// putBlob→promote window cannot graduate — the next fold sees the edge). Moving putBlob before
    /// precommitAdd would adopt an incarnation with no protecting edge (the pre-B188 dangle shape) and
    /// trips the EDGE-BEFORE-OBSERVE fail-closed throw in PartWriteTxn::observeAndAdmit; the TLA+ order
    /// sabotage (Gate A) is the formal guard.
    const Cas::ManifestId id = st.build->stageManifest(st.entries);
    st.build->precommitAdd(ns, ref, id);
    uploadPendingBlobs(st);

    const bool ref_existed = metadata_storage.partAccess().existsRef({ns, ref}, ContentAddressed::Freshness::ForceFresh);
    metadata_storage.partAccess().promoteBuild(*st.build, {ns, ref}, st.build->buildId(), id);
    st.published = true;
    return !ref_existed;
}

void ContentAddressedTransaction::commit(const TransactionCommitOptionsVariant &)
{
    /// Publish each staged part. [TXN-ONE-PIPELINE] This is the ONLY place a ref becomes durable — the
    /// tmp->final rename is a pure overlay re-key. Commit
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
            metadata_storage.partAccess().dropRefBestEffort({ns, ref});
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

const Cas::ManifestEntry * ContentAddressedTransaction::findStagedEntry(
    const ContentAddressedMetadataStorage::Route & r) const
{
    auto it = parts.find({r.ns.string(), r.ref});
    if (it == parts.end())
        return nullptr;
    auto eit = std::find_if(it->second.entries.begin(), it->second.entries.end(),
        [&](const Cas::ManifestEntry & e) { return e.path == r.file; });
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
        if (entry->placement == Cas::EntryPlacement::Blob)
        {
            /// B188: a pending blob has not been uploaded yet — its storage object does not exist in
            /// the pool. Return empty so the caller falls back to tryReadFileInFlight (local temp read).
            if (findPendingBlob(it->second, entry->ref))
                return {};
            const auto location = metadata_storage.store()->locate(*entry);
            return StoredObjects{StoredObject(location.key, path, location.length)};
        }
        /// An Inline entry carries its bytes in `inline_bytes`; `size()` (not `blob_size`, which is 0
        /// for an inline entry carried forward from a decoded source manifest — createHardLink) reports
        /// the real inline byte count, so an in-flight read of a carried-forward inline sidecar (e.g. a
        /// MATERIALIZE-PROJECTION projection marks file) resolves to its real size, matching the
        /// committed getStorageObjects path.
        return StoredObjects{StoredObject("", path, entry->size())};
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
    if (const auto * entry = findStagedEntry(*r))
    {
        if (entry->placement == Cas::EntryPlacement::Inline)
            return std::make_unique<ReadBufferFromOwnMemoryFile>(path, entry->inline_bytes);
        if (entry->placement == Cas::EntryPlacement::Blob)
        {
            /// B188: a pending blob has not been uploaded yet — serve reads from the staging area (the
            /// same bytes that will be promoted to the pool in publishStaging post-precommit): a local
            /// temp file for `StagingBackend::Local`, or the S3 staging object for `StagingBackend::S3`
            /// (Task 6, S3-native staging plan — `staging_key` is a remote object key there, never a
            /// local path, so `ReadBufferFromFile` would misinterpret it as a filesystem path).
            if (const auto * pb = findPendingBlob(it->second, entry->ref))
            {
                if (pb->backend == StagingBackend::S3)
                {
                    /// S3-native staging fix 2026-07-11: the staging object now holds `[header][payload]`
                    /// (the fixed-length `blob_header_len` CABL envelope, so the promote can stay a
                    /// verbatim server-side copy). Read-your-writes must serve the PAYLOAD ONLY — wrap the
                    /// object read in a `ReadBufferFromFileView` windowed to `[header_len, header_len+size)`
                    /// so position 0 is the payload start, else the reader would see 256 bytes of header
                    /// prepended to the payload (corruption). The LOCAL staging temp file holds the payload
                    /// verbatim (no header), so its path is unchanged.
                    const uint64_t header_len = metadata_storage.store()->poolMeta().blob_header_len;
                    const uint64_t payload_end = header_len + pb->size;
                    auto impl = metadata_storage.objectStorage()->readObject(
                        StoredObject(pb->staging_key, path, payload_end), settings);
                    return std::make_unique<ReadBufferFromFileView>(
                        std::move(impl), path, header_len, payload_end);
                }
                return std::make_unique<ReadBufferFromFile>(pb->staging_key);
            }
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
    if (const auto * entry = findStagedEntry(*r))
        /// `size()` (not `blob_size` directly, which is 0 for an inline entry carried forward via
        /// createHardLink from a decoded source manifest). Without this, an in-flight size query for a
        /// carried-forward inline sidecar returns 0 — the 02941 MATERIALIZE-PROJECTION "Empty marks
        /// file: 0, must be: 144" corruption on a same-session read.
        return entry->size();
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
        if (entry.path.starts_with(prefix))
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
        add(entry.path);
    return {names.begin(), names.end()};
}

void ContentAddressedTransaction::createMetadataFile(const std::string &, const StoredObjects &)
{
    notYet("createMetadataFile");
}

void ContentAddressedTransaction::stageBlobPartFile(
    const ContentAddressedMetadataStorage::Route & route,
    const Cas::BlobRef & ref, size_t size, const std::string & staging_key, StagingBackend backend)
{
    /// B188: do NOT upload here. Record the pending blob (uploaded post-precommit in publishStaging)
    /// and a tokenless dependency; putBlob later overwrites it with the tokened dependency.
    /// The staging bytes are kept (the transaction owns them) — a local temp file for
    /// `StagingBackend::Local`, an S3 staging object for `StagingBackend::S3` (Task 4).
    auto & st = stagingFor(route);
    st.pending_blobs.push_back({ref, staging_key, size, backend});
    buildFor(route, st).recordPendingBlobDep(ref, size);

    Cas::ManifestEntry entry;
    entry.path = route.file;
    entry.placement = Cas::EntryPlacement::Blob;
    entry.ref = ref;
    entry.blob_size = size;
    std::erase_if(st.entries, [&](const Cas::ManifestEntry & e) { return e.path == entry.path; });
    st.entries.push_back(std::move(entry));
}

std::string ContentAddressedTransaction::buildS3StagingBlobHeader(
    const ContentAddressedMetadataStorage::Route & route) const
{
    /// Mirror `PartWriteTxn::uploadFromSource`'s `buildHeader` (minus the dropped `logical_size`/`logical_hash`
    /// fields and minus `build_id`, which is not known until commit and is diagnostic-only). A FRESH
    /// `incarnation_tag` per staging object keeps the incarnation zone unique; the header is padded to
    /// the pool's fixed `blob_header_len` so the payload starts at a constant offset.
    const Cas::PoolPtr & store = metadata_storage.store();
    const Cas::PoolMeta & meta = store->poolMeta();
    const Cas::PoolConfig & cfg = store->poolConfig();

    Cas::EnvelopeHeader header;
    header.kind = Cas::ObjectKind::Blob;
    header.incarnation_tag = (static_cast<UInt128>(thread_local_rng()) << 64) | thread_local_rng();
    header.build_id = 0;   /// not known at stream time; diagnostic-only (not read by GC/read paths)
    /// ch = the real ClickHouse VERSION_INTEGER (diagnostic-only; consistent with `PartWriteTxn::buildHeader`).
    /// The v3 envelope drops hash_algo/domain_id/writer_version, so forensics ride on ch + bld.
    header.provenance = Cas::Provenance{
        /*created_at_ms*/ 0, cfg.server_id, VERSION_INTEGER, Cas::ProvenanceOp::Other};
    header.intended_ref = route.ns.string() + "/" + route.ref;
    /// The v3 codec pads to the pool's fixed header length and TRUNCATES a too-long intended_ref
    /// internally (it is diagnostic-only), so the old drop-and-retry is gone — one encode call.
    return Cas::encodeEnvelopeHeader(header, static_cast<uint32_t>(meta.blob_header_len));
}

std::unique_ptr<WriteBufferFromFileBase> ContentAddressedTransaction::tryCreateWriteBuffer(
    const std::shared_ptr<IDiskTransaction> & owner,
    const std::string & path, size_t buf_size, WriteMode mode,
    const WriteSettings & settings, bool autocommit)
{
    /// [TXN-ONE-PIPELINE] CA owns the write (moved verbatim from the disk layer's former CA write block).
    /// Append is serviceable (read-modify-rewrite) only for a non-part / table-level verbatim file
    /// (handled inside writeFile). A part file is a content blob or a whole-rewritten inline entry, so
    /// append on a part-file path is unsupported.
    if (mode == WriteMode::Append && ContentAddressed::isPartFilePath(path))
        throw Exception(ErrorCodes::NOT_IMPLEMENTED, "Disk does not support WriteMode::Append for content part files");

    /// Autocommit cannot work for a CONTENT BLOB part file (column data/marks, primary.idx): a part's
    /// blobs are always written together as one build, whose manifest + ref publish only when commit()
    /// runs. A small INLINE-eligible part file IS autocommittable (a standalone one-shot write): the write
    /// lands as an ordinary manifest entry and, if the ref is already committed, `publishStaging`'s repoint
    /// branch carries the rest of the part forward and republishes once (the transactional-INSERT
    /// creation-CSN fill-in / removal-TID rewrite / rollback path). Verbatim / table-level files (not part
    /// files) are durable on finalize regardless of `autocommit`.
    if (autocommit && ContentAddressed::isPartFilePath(path))
    {
        auto p = ContentAddressed::parsePartFilePath(path);
        if (!p || p->file.empty() || ContentAddressed::partFileMustStayBlob(p->file))
            throw Exception(ErrorCodes::NOT_IMPLEMENTED,
                "Autocommit writes are not supported for content part files on a content-addressed disk");

        auto inner = writeFile(path, buf_size, mode, settings);
        auto commit_callback = [owner](size_t) mutable { owner->commit(); };
        return std::make_unique<WriteBufferWithFinalizeCallback>(
            std::move(inner), std::move(commit_callback), path, /*create_blob_if_empty=*/true);
    }

    /// Non-autocommit (or verbatim autocommit): pin the owning disk transaction for the returned buffer's
    /// lifetime. The CA write buffers capture a bare `this` in their deferred finalize / pin-blob callbacks;
    /// MergedBlockOutputStream may finalize them LATER (another thread, or after the part storage /
    /// transaction would otherwise be torn down on async-insert / cancel / exception-unwind). Holding
    /// `owner` (which owns this ContentAddressedTransaction by shared_ptr) keeps that `this` valid until the
    /// buffer — and so this callback — is destroyed after finalize (the B90 CA-S3 lifetime fix, now
    /// expressed generically via `owner`). No cycle: the transaction does not hold the buffer.
    auto inner = writeFile(path, buf_size, mode, settings);
    auto keep_alive_callback = [owner](size_t) mutable {};
    return std::make_unique<WriteBufferWithFinalizeCallback>(
        std::move(inner), std::move(keep_alive_callback), path, /*create_blob_if_empty=*/true);
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
        const std::string key = metadata_storage.serverRootId() + "/" + path;
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

    /// all-tree-part-files Task 6 (spec 2026-07-14-cas-all-tree-part-files-design.md §4): the former
    /// mutable-per-part-file branch (uuid.txt/metadata_version.txt/txn_version.txt staging directly
    /// into a separate mutable payload) is DELETED here — these three names fall through to the
    /// ordinary content path below like any other tree file. Task 9 completed the sweep: the
    /// `kMutablePerPartFiles`/`isMutablePerPartFile` predicate itself is gone too — there is no
    /// filename left to special-case. During part build these files land in the initial manifest with
    /// every other staged file; a standalone write on an already-committed part repoints (Task 4).

    /// A CONTENT part file that must stay a blob (per-column data/marks, primary.idx): spill + hash,
    /// then stage the blob as PENDING (B188 precommit-first). The blob is NOT uploaded/promoted here;
    /// publishStaging uploads it (Local) or a later task's promote drives it (S3) post-precommit.
    /// recordPendingBlobDep (inside stageBlobPartFile) records a tokenless dependency without any
    /// pool operation at staging time.
    if (ContentAddressed::partFileMustStayBlob(r->file))
    {
        /// S3-native staging (Task 4, plan docs/superpowers/plans/2026-07-11-cas-s3-native-staging.md):
        /// when this disk opted in (`staging_backend=s3`) AND the mount-time capability probe
        /// (Task 3) proved the object storage enforces write-once conditional copy, stream directly
        /// to a fresh per-mount S3 staging object while hashing — no local-disk round trip. Otherwise
        /// (the OFF BY DEFAULT global constraint, or a probe fail-close) fall through to the existing,
        /// byte-for-byte-unchanged local-temp-file path below.
        /// P1-T3a (CAS pluggable blob hash): hash with this Pool's NODE-LOCAL write algo, not a
        /// hardcoded cityHash128 (Phase 3 T4: `PoolMeta` no longer records a single pool-wide algo --
        /// mixed-algo pools track `algos_used`; `writeAlgo()` is the write-mint accessor now).
        const auto hash_algo = metadata_storage.store()->writeAlgo();
        /// Phase 3 T2: `hash_hex` is rendered by the streaming write buffer at `hash_algo`'s own width —
        /// parse it back at that SAME width via `Cas::codecFor(hash_algo)` (never a pool-wide
        /// `DigestCodec`, which no longer exists) into a full `BlobRef` pair.

        if (metadata_storage.stagingBackend() == StagingBackend::S3 && metadata_storage.conditionalCopySupported())
        {
            const std::string staging_key = metadata_storage.stagingKeyPrefix() + "/" + getRandomASCIIString(32) + ".tmp";
            auto object_sink = metadata_storage.objectStorage()->writeObject(StoredObject(staging_key), WriteMode::Rewrite);
            /// S3-native staging fix 2026-07-11: build the fixed-length CABL envelope header NOW (before
            /// the payload is streamed) so the staging object holds `[header][payload]` and the promote
            /// stays a verbatim server-side copy. The header carries a FRESH `incarnation_tag`; `build_id`
            /// is left 0 (not known at stream time — diagnostic-only, not read by GC/read paths). The
            /// buffer writes this header first, UNHASHED and excluded from the reported size, so the
            /// content key stays the pool's hash of `payload` and `blob_size` stays the payload size.
            std::string envelope_header = buildS3StagingBlobHeader(*r);
            return std::make_unique<ContentAddressed::CaContentWriteBuffer>(
                std::move(object_sink),
                staging_key,
                std::move(envelope_header),
                hash_algo,
                buf_size,
                settings.use_adaptive_write_buffer,
                settings.adaptive_write_buffer_initial_size,
                [this, route = *r, hash_algo](const std::string & hash_hex, size_t size, const std::string & key)
                {
                    const Cas::BlobRef ref{hash_algo, Cas::codecFor(hash_algo).fromHex(hash_hex)};
                    stageBlobPartFile(route, ref, size, key, StagingBackend::S3);
                });
        }

        return std::make_unique<ContentAddressed::CaContentWriteBuffer>(
            metadata_storage.scratchPath(),
            hash_algo,
            buf_size,
            settings.use_adaptive_write_buffer,
            settings.adaptive_write_buffer_initial_size,
            [this, route = *r, hash_algo](const std::string & hash_hex, size_t size, const std::string & temp_path)
            {
                const Cas::BlobRef ref{hash_algo, Cas::codecFor(hash_algo).fromHex(hash_hex)};
                stageBlobPartFile(route, ref, size, temp_path, StagingBackend::Local);
            });
    }

    /// Inline candidate (small eager metadata): buffer in memory, decide at finalize. <= INLINE_CAP
    /// rides the single tree object as an Inline entry (one-GET part open, B10/B97); an oversized
    /// candidate spills to a blob (the safety net).
    return std::make_unique<ContentAddressed::CaInlineWriteBuffer>(
        [this, route = *r](std::string bytes)
        {
            /// Phase 3 T2: mint via the ONE write mint, `Cas::poolContentHash` (algo, payload) -> BlobRef
            /// (`CasPartWriteTxn.h`) -- the SAME mint the streaming blob path's callers use, so an inline file
            /// and a standalone blob of identical content get the same ref (same content hash identity)
            /// under EVERY algo, including sha256.
            const auto hash_algo = metadata_storage.store()->writeAlgo();
            const Cas::BlobRef ref = Cas::poolContentHash(hash_algo, bytes);
            if (bytes.size() <= INLINE_CAP)
            {
                auto & st = stagingFor(route);
                /// An inline (no-blob) entry still requires a PartWriteTxn. `publishStaging` stages the
                /// manifest body, precommits, and promotes the ref even for a part with NO blob uploads;
                /// it asserts `st.build != nullptr` whenever `st.entries` is non-empty. Without this, a
                /// part whose files are ALL inline (a tiny/empty merge output, every file <= INLINE_CAP)
                /// reaches `publishStaging` with entries but no PartWriteTxn -> LOGICAL_ERROR "staged entries
                /// without a PartWriteTxn" -> server crash under abort_on_logical_error (CRASH-CA-S3, pre-existing
                /// since the inline-files feature). The blob path already establishes the PartWriteTxn via
                /// `buildFor`; the inline path must do the same.
                buildFor(route, st);
                Cas::ManifestEntry entry;
                entry.path = route.file;
                entry.placement = Cas::EntryPlacement::Inline;
                entry.ref = ref;   /// content hash identity (same for inline and blob of same content)
                /// `blob_size` stays 0 (its default) for an Inline entry — matching decode, which never
                /// fills it for Inline. `entry.size()` is the logical size, derived from `inline_bytes`.
                entry.inline_bytes = std::move(bytes);
                std::erase_if(st.entries, [&](const Cas::ManifestEntry & e) { return e.path == entry.path; });
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
                /// stageBlobPartFile throws, drop the orphan instead of leaking it into scratch. This
                /// fallback is always `StagingBackend::Local` — an oversized inline candidate is rare
                /// enough (a safety net, not a hot path) that Task 4's S3-staging mode does not cover it.
                bool staged = false;
                SCOPE_EXIT({ if (!staged) { std::error_code ec; std::filesystem::remove(temp_path, ec); } });
                stageBlobPartFile(route, ref, bytes.size(), temp_path, StagingBackend::Local);
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
        metadata_storage.partAccess().dropRefIfPresent(r->refKey());
        /// all-tree-part-files Task 8 (B123 evolution, spec 2026-07-14-cas-all-tree-part-files-design.md
        /// §6): this transaction's staged removal marks for the SAME ref (content_removed, populated by
        /// unlinkFile's per-file unlinks that the MergeTree fast-removal path issues right before this
        /// call) are superseded by the whole-part ref-drop just performed above — discard them so
        /// publishStaging's committed-ref repoint branch never chases an already-dropped ref, and the
        /// dominant removal path pays zero repoints (one ref-drop only).
        if (auto * st = findStaging(*r))
            st->content_removed.clear();
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
            metadata_storage.partAccess().dropRefIfPresent({ns, p->part_name});
            return;
        }
        if (ContentAddressed::endsWithTableUuidPair(path))
        {
            metadata_storage.partAccess().dropNamespace(ContentAddressedMetadataStorage::shadowNamespace(path));
            return;
        }
        /// Backup root / intermediate dir (SYSTEM UNFREEZE WITH NAME): drop every shadow
        /// namespace under it. Canonicalize: callers hand trailing-slash dirs (T13).
        std::string prefix = path;
        while (!prefix.empty() && prefix.back() == '/')
            prefix.pop_back();
        for (const auto & ns : metadata_storage.store()->listNamespaces(prefix + "/"))
            metadata_storage.partAccess().dropNamespace(Cas::RootNamespace{ns});
        return;
    }

    /// Table dir: the table's namespace (live + folded-in detached refs, B181) and every verbatim
    /// file go in one dropNamespace.
    if (auto uuid = ContentAddressed::parseTableUuid(path))
    {
        metadata_storage.partAccess().dropNamespace(metadata_storage.liveNamespace(*uuid));
        return;
    }

    if (auto p = ContentAddressed::parsePartFilePath(path))
    {
        auto r = metadata_storage.route(*p);
        /// The detached CONTAINER dir (DROP DETACHED / table-detach): drop all detached refs (B181).
        if (r && r->ref.empty() && p->part_name == ContentAddressed::kDetachedDirName)
        {
            for (const auto & ref : metadata_storage.detachedRefNames(r->ns))
                metadata_storage.partAccess().dropRefIfPresent({r->ns, ref});
            return;
        }
        /// The moving CONTAINER dir (MOVE-to-CA fix, mirrors detached): the mover's crash-cleanup
        /// (MergeTreeData.cpp, MOVING_DIR_NAME) calls this at table load to reclaim every staging
        /// ref an interrupted move left behind.
        if (r && r->ref.empty() && p->part_name == ContentAddressed::kMovingDirName)
        {
            for (const auto & ref : metadata_storage.movingRefNames(r->ns))
                metadata_storage.partAccess().dropRefIfPresent({r->ns, ref});
            return;
        }
        /// A single part dir (live or detached): drop its ref.
        if (r && !r->ref.empty() && r->file.empty())
        {
            metadata_storage.partAccess().dropRefIfPresent(r->refKey());
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

    /// Content file. Prefer an entry staged earlier in THIS transaction (the destination PartWriteTxn
    /// re-observes the blob via cold reuse — its own dependency); else carry forward from the
    /// COMMITTED source part (adoptFromTree: tokenless evidence pinned by the witnessed live
    /// source tree, W-EVIDENCE).
    Cas::ManifestEntry entry;
    if (auto * src_st = findStaging(*src))
    {
        auto it = std::find_if(src_st->entries.begin(), src_st->entries.end(),
            [&](const Cas::ManifestEntry & e) { return e.path == src->file; });
        if (it != src_st->entries.end())
        {
            entry = *it;
            if (entry.placement == Cas::EntryPlacement::Blob)
            {
                /// B190 Task 4: unified adopt dispatch. copy_pending=(&dst_st != src_st) so the pending
                /// blob record is copied into dst_st only when the destination is a different part
                /// (hardlink = copy semantics; same-part is a self-ref that shouldn't duplicate the record).
                const auto * pb = findPendingBlob(*src_st, entry.ref);
                adoptStagedBlob(pb, entry, dst_st, buildFor(*dst, dst_st), /*copy_pending=*/(&dst_st != src_st));
            }
            else if (entry.placement != Cas::EntryPlacement::Inline)
                throw Exception(ErrorCodes::LOGICAL_ERROR,
                    "ContentAddressed: staged hardlink of unsupported placement for {}", path_from);
            entry.path = dst->file;
            std::erase_if(dst_st.entries, [&](const Cas::ManifestEntry & e) { return e.path == entry.path; });
            dst_st.entries.push_back(std::move(entry));
            return;
        }
    }

    /// Carry forward from the COMMITTED source part: read the source manifest, find the named entry,
    /// record a TOKENLESS W-EVIDENCE dep for its blob (no HEAD before precommit; promote re-proves it).
    /// ForceFresh getView == resolveRef(allow_stale=false) + readManifestShared, so this is the same
    /// request pattern as before, now instrumented via the facade (spec §Method Routing).
    auto view = metadata_storage.partAccess().getView(src->refKey(), ContentAddressed::Freshness::ForceFresh);
    if (!view)
        throw Exception(ErrorCodes::FILE_DOESNT_EXIST,
            "ContentAddressed: createHardLink source part missing: {}", path_from);
    const auto * src_entry = view->findFile(src->file);
    if (!src_entry)
        throw Exception(ErrorCodes::FILE_DOESNT_EXIST,
            "ContentAddressed: createHardLink source file missing in manifest: {}", path_from);
    buildFor(*dst, dst_st).adoptEvidence(*src_entry);
    entry = *src_entry;
    entry.path = dst->file;
    std::erase_if(dst_st.entries, [&](const Cas::ManifestEntry & e) { return e.path == entry.path; });
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
                    metadata_storage.partAccess().republishRef({from_ns, ref}, {to_ns, ref});
                for (const auto & name : metadata_storage.store()->listNamespaceFiles(from_ns))
                    if (auto bytes = metadata_storage.store()->getNamespaceFile(from_ns, name))
                        metadata_storage.store()->putNamespaceFile(to_ns, name, *bytes);
                metadata_storage.partAccess().dropNamespace(from_ns);
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
                if (entry.path.starts_with(old_prefix))
                    entry.path = new_prefix + entry.path.substr(old_prefix.size());
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
                /// All-tree-part-files Task 9: a genuine collision (both src and dst independently
                /// staged DIFFERING bytes for the SAME path) is a fail-loud LOGICAL_ERROR rather than a
                /// silent lost-update — the same defensive rule this loop used to apply only to the
                /// three legacy mutable names now applies uniformly to every entry (that scoping was
                /// itself a leftover of the mutable-file/entry split; there is only one kind of staged
                /// file left). Identical bytes are a benign idempotent re-key; distinct paths are the
                /// ordinary source-wins merge (a genuine collision is not expected in normal operation
                /// — only some future op-order re-keying the same file under both stagings).
                if (const auto existing = std::find_if(dst_st.entries.begin(), dst_st.entries.end(),
                        [&](const Cas::ManifestEntry & e) { return e.path == entry.path; });
                    existing != dst_st.entries.end() && !(*existing == entry))
                    throw Exception(ErrorCodes::LOGICAL_ERROR,
                        "ContentAddressed: moveDirectory file collision on '{}' ({} -> {}): "
                        "source and destination staged different bytes for the same file",
                        entry.path, src->ref, dst->ref);
                std::erase_if(dst_st.entries, [&](const Cas::ManifestEntry & e) { return e.path == entry.path; });
                dst_st.entries.push_back(std::move(entry));
            }
            /// all-tree-part-files Task 8: carry any staged removal marks forward too — a re-key of the
            /// staging key must not silently drop them.
            for (const auto & file : src_st.content_removed)
                dst_st.content_removed.insert(file);
            /// B188: move pending blobs from src to dst — they will be uploaded in dst's publishStaging.
            for (auto & pb : src_st.pending_blobs)
                dst_st.pending_blobs.push_back(std::move(pb));
            src_st.pending_blobs.clear();
            if (!dst_st.build)
            {
                dst_st.build = std::move(src_st.build);
            }
            else if (src_st.build)
            {
                /// Two Builds for one destination part: keep the destination's; the source build's
                /// deps ride the staged entries (re-observed by the destination build at adopt
                /// time is unnecessary - entries staged via putBlob/adopt carry deps in the SOURCE
                /// build... merge conservatively by abandoning nothing and re-observing):
                for (const auto & entry : dst_st.entries)
                    if (entry.placement == Cas::EntryPlacement::Blob)
                    {
                        /// B190 Task 4: unified adopt dispatch. Pending blob records were already moved
                        /// to dst_st.pending_blobs above (MOVE semantics), so copy_pending=false.
                        adoptStagedBlob(findPendingBlob(dst_st, entry.ref), entry, dst_st, *dst_st.build, /*copy_pending=*/false);
                    }
                src_st.build->abandon();
            }
            parts.erase(src_it);

            /// [TXN-ONE-PIPELINE]: a freshly-written part finalized tmp->final is re-keyed in the
            /// overlay above (entries/marks/pending blobs/build moved src->dst). The durable publish
            /// happens only in this transaction's commit (the existing publishStaging loop), NOT in
            /// this method. Since the part-durability fix (spec
            /// 2026-07-17-part-durability-before-keeper-commit-design.md), MergeTree calls that
            /// commit from Transaction::renameParts — still off the data_parts lock, and BEFORE the
            /// Keeper multi. No early-published ref to compensate on abort within this method
            /// (see ~ContentAddressedTransaction).
        }

        if (had_staged_source)
        {
            /// B183: a nested text-index sub-storage (MergeTask/MutateTask createTemporaryTextIndexStorage)
            /// may have DURABLY published a committed scratch ref at THIS part's own path holding only
            /// `<part>/text_index_tmp/` files. That ref is not ours and is not staged; drop it now so the
            /// overlay we publish in commit() is the authoritative manifest. Independent of our publish
            /// timing (it targets an already-committed foreign ref), so it stays a call-time drop.
            metadata_storage.partAccess().dropRefIfPresent(src->refKey());
            return;
        }

        /// Move any COMMITTED source ref (a merge/mutation result rename, DETACH, ATTACH, a
        /// delete_tmp_ rename, an early-committed child ref being renamed away). Absent = a pure
        /// staged/tmp move - nothing durable to touch.
        metadata_storage.partAccess().republishRef(src->refKey(), dst->refKey());
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
        const std::string src_key = metadata_storage.serverRootId() + "/" + path_from;
        const std::string dst_key = metadata_storage.serverRootId() + "/" + path_to;
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

    /// Staged content entry re-keys in place (cross-part included; deps follow the entries). B124
    /// canonical policy: SOURCE-wins — a move/rename carries the source's content to the destination,
    /// overwriting any prior dest bytes (the POSIX `rename` semantic, and what the atomic-write
    /// `.tmp -> final` rename requires). moveDirectory's staged merge is aligned to this same policy.
    auto it = std::find_if(src_st.entries.begin(), src_st.entries.end(),
        [&](const Cas::ManifestEntry & e) { return e.path == src->file; });
    if (it != src_st.entries.end())
    {
        auto entry = std::move(*it);
        src_st.entries.erase(it);
        entry.path = dst->file;
        if (&src_st != &dst_st && entry.placement == Cas::EntryPlacement::Blob)
        {
            /// B190 Task 4: unified adopt dispatch. MOVE semantics — physically move the pending blob
            /// record from src_st to dst_st FIRST (so dst_st owns the upload), then call adoptStagedBlob
            /// with copy_pending=false (the record is already in dst_st; no additional copy needed).
            auto pb_it = std::find_if(src_st.pending_blobs.begin(), src_st.pending_blobs.end(),
                [&](const PartStaging::PendingBlob & pb) { return pb.ref == entry.ref; });
            if (pb_it != src_st.pending_blobs.end())
            {
                dst_st.pending_blobs.push_back(std::move(*pb_it));
                src_st.pending_blobs.erase(pb_it);
            }
            adoptStagedBlob(findPendingBlob(dst_st, entry.ref), entry, dst_st, buildFor(*dst, dst_st), /*copy_pending=*/false);
        }
        std::erase_if(dst_st.entries, [&](const Cas::ManifestEntry & e) { return e.path == entry.path; });
        dst_st.entries.push_back(std::move(entry));
        return;
    }
    /// Source not staged in THIS transaction: previously this covered a standalone one-shot rename of
    /// a COMMITTED mutable file (the MVCC `txn_version.txt.tmp -> txn_version.txt` move). All-tree-
    /// part-files Task 5 short-circuited that rename entirely on atomic-write storages (CA included)
    /// -- `VersionMetadataOnDisk::storeInfoToDataPartStorage` writes `txn_version.txt` directly, no
    /// `.tmp` + `replaceFile` dance -- so this branch has had no live caller since Task 5 landed; Task
    /// 9 removes it rather than reimplement it without the deleted `isMutablePerPartFile` predicate.
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
        std::erase_if(dst_st.entries, [&](const Cas::ManifestEntry & e) { return e.path == dst->file; });
    }
    moveFile(path_from, path_to);
}

void ContentAddressedTransaction::unlinkFile(const std::string & path, bool /*if_exists*/, bool /*should_remove_objects*/)
{
    /// Part file. Two sub-cases:
    ///   1. A file STAGED in this transaction (content entry or legacy mutable bytes): drop the
    ///      staged state so it never reaches the published tree.
    ///   2. A COMMITTED CONTENT file (not staged here): all-tree-part-files Task 8 (B123 evolution,
    ///      spec 2026-07-14-cas-all-tree-part-files-design.md §6) — stage a REMOVAL MARK
    ///      (`content_removed`). The mark is resolved at publish (`publishStaging`): a repoint
    ///      republishes the manifest minus the removed paths, UNLESS this same transaction also
    ///      drops the whole part directory (`removeDirectory`), in which case the mark is
    ///      superseded — see `removeDirectory` below.
    ///
    /// B123 — LOAD-BEARING INVARIANT, do not "fix" with a blanket fail-closed assert:
    /// On a content-addressed disk a committed part is ONE atomic ref (its manifest tree); the removal
    /// UNIT is the whole-part ref-drop done by `removeDirectory(<part>)`, NOT per-file unlinks. The
    /// MergeTree fast-removal path (IMergeTreeDataPart::remove) unlinks EVERY part file one-by-one and
    /// THEN calls `removeDirectory` — so a batched per-file unlink storm immediately followed by a
    /// ref-drop in the SAME transaction must cost exactly one ref-drop and zero repoints, not one
    /// repoint per unlinked file. `removeDirectory` clears any marks staged here for the same ref
    /// before the transaction publishes, which is what makes the storm-then-drop shape free. A lone
    /// surgical unlink NOT followed by a ref-drop in the same transaction (ATTACH's
    /// `removeVersionMetadata`, a future backfill/repair delete) resolves to one repoint-remove —
    /// this closes the file's former fail-open (a committed content file could never actually be
    /// deleted on its own; see the pre-Task-8 comment this replaces).
    if (auto r = routeOf(path); r && !r->file.empty())
    {
        auto & st = stagingFor(*r);
        const bool staged_here = std::any_of(st.entries.begin(), st.entries.end(),
                           [&](const Cas::ManifestEntry & e) { return e.path == r->file; });
        /// A matching pending_blobs record (if any) is left in place — its temp file is cleaned by
        /// cleanupPendingTempFiles at commit end, and the orphaned record is filtered out of the
        /// publish upload by the staged-tree-hash check in publishStaging (B189). We do NOT purge it
        /// eagerly because the same hash may still be referenced by another staged entry.
        std::erase_if(st.entries, [&](const Cas::ManifestEntry & e) { return e.path == r->file; });
        if (!staged_here)
            st.content_removed.insert(r->file);
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
    metadata_storage.store()->removeMountpointObject(metadata_storage.serverRootId() + "/" + path);
}

void ContentAddressedTransaction::truncateFile(const std::string &, size_t)
{
    /// No-op: content-addressed blobs are immutable; committed part files are never truncated
    /// (whole-file rewrites replace the staged entry instead). PoC behavior, kept.
}

}

// ===== Merged from ContentAddressedWriteBuffers.cpp (merge #2) =====

namespace fs = std::filesystem;

namespace DB::ContentAddressed
{

CaContentWriteBuffer::CaContentWriteBuffer(
    std::string temp_dir,
    Cas::BlobHashAlgo hash_algo,
    size_t buf_size,
    bool use_adaptive_buffer_size,
    size_t adaptive_buffer_initial_size,
    OnFinalized on_finalized_)
    : WriteBufferFromFileBase(use_adaptive_buffer_size ? adaptive_buffer_initial_size : buf_size, nullptr, 0)
    , on_finalized(std::move(on_finalized_))
{
    fs::create_directories(temp_dir);
    temp_path = temp_dir + "/" + getRandomASCIIString(32) + ".tmp";

    /// The spill buffer is a SECOND per-stream buffer; thread the adaptive flag into it too so a
    /// wide part keeps its footprint small. Its IO is a local temp file, not the remote stream.
    sink = std::make_unique<WriteBufferFromFile>(
        temp_path,
        buf_size,
        /*flags=*/-1,
        /*throttler=*/nullptr,
        /*mode=*/0666,
        /*existing_memory=*/nullptr,
        /*alignment=*/0,
        use_adaptive_buffer_size,
        adaptive_buffer_initial_size);
    hashing = Cas::makeBlobHashingWriteBuffer(hash_algo, *sink);
}

CaContentWriteBuffer::CaContentWriteBuffer(
    std::unique_ptr<WriteBufferFromFileBase> object_store_sink,
    std::string object_key,
    std::string envelope_header,
    Cas::BlobHashAlgo hash_algo,
    size_t buf_size,
    bool use_adaptive_buffer_size,
    size_t adaptive_buffer_initial_size,
    OnFinalized on_finalized_)
    : WriteBufferFromFileBase(use_adaptive_buffer_size ? adaptive_buffer_initial_size : buf_size, nullptr, 0)
    , on_finalized(std::move(on_finalized_))
    , temp_path(std::move(object_key))
    , is_s3_staging(true)
    , sink(std::move(object_store_sink))
{
    /// The sink is ALREADY opened against the staging object by the caller (writeFile) — this
    /// constructor wraps it in the hashing chain, exactly like the local-temp-file mode.
    ///
    /// S3-native staging fix 2026-07-11: write the CABL envelope header to the sink FIRST, DIRECTLY —
    /// bypassing `hashing` (so it is excluded from the content hash) and this outer buffer's `count()`
    /// (so the reported size is the payload only). The staging object therefore holds `[header][payload]`
    /// and the promote stays a verbatim server-side copy. Only the payload the caller subsequently writes
    /// through THIS buffer flows through `hashing`. The header write precedes any payload write, so the
    /// on-object byte order is header-then-payload.
    if (!envelope_header.empty())
        sink->write(envelope_header.data(), envelope_header.size());

    /// The adaptive-sizing params only affect THIS outer buffer (mirroring the Local ctor above); the
    /// sink's own buffering was decided by the caller when it opened the object-store write.
    hashing = Cas::makeBlobHashingWriteBuffer(hash_algo, *sink);
}

CaContentWriteBuffer::~CaContentWriteBuffer()
{
    /// Best-effort cleanup if finalize was never reached (exception unwind / cancel).
    cancel();
    /// B188: if on_finalized ran successfully the transaction (Local mode) or a later task's promote
    /// path (S3 mode) owns the staged bytes and cleans them up. Do not remove them here. S3-mode
    /// staging objects are never removed by this class at all (see cancelImpl / removeTempFile).
    if (!temp_ownership_transferred && !is_s3_staging)
        removeTempFile();
}

void CaContentWriteBuffer::nextImpl()
{
    if (!offset())
        return;
    hashing->write(working_buffer.begin(), offset());
}

void CaContentWriteBuffer::finalizeImpl()
{
    next();
    const size_t size = count();

    /// getHashHex flushes the chain and returns the streaming digest (the pool's selected algo) of
    /// everything written, as 32 lowercase hex chars.
    const std::string hash_hex = hashing->getHashHex();

    hashing->finalize();
    sink->finalize();

    /// B188: on successful finalize, ownership of temp_path (Local: the local temp path; S3: the
    /// staging object key) transfers to the caller (the transaction uploads/promotes it and cleans
    /// up). cancel() still removes/cancels it.
    if (on_finalized)
    {
        on_finalized(hash_hex, size, temp_path);
        temp_ownership_transferred = true;
    }
}

void CaContentWriteBuffer::cancelImpl() noexcept
{
    if (hashing)
        hashing->cancel();
    if (sink)
        sink->cancel();
    /// S3 mode: `temp_path` is a remote object key, not a path on this filesystem — do NOT attempt
    /// to delete the (possibly partially-written) staging object here. Cancelling `sink` above is
    /// enough to make sure no partial finalize happens; reclaiming an orphaned staging object is the
    /// mount-lease sweeper's job (a later task).
    if (!is_s3_staging)
        removeTempFile();
}

void CaContentWriteBuffer::removeTempFile() noexcept
{
    std::error_code ec;
    fs::remove(temp_path, ec);
}

void CaContentWriteBuffer::sync()
{
    next();
    hashing->next();
    sink->sync();
}

std::string CaContentWriteBuffer::getFileName() const
{
    return temp_path;
}

CaInlineWriteBuffer::CaInlineWriteBuffer(OnInlined on_inlined_)
    : WriteBufferFromFileBase(DBMS_DEFAULT_BUFFER_SIZE, nullptr, 0)
    , on_inlined(std::move(on_inlined_))
{
}

CaInlineWriteBuffer::~CaInlineWriteBuffer()
{
    cancel();
}

void CaInlineWriteBuffer::nextImpl()
{
    if (!offset())
        return;
    accumulated.append(working_buffer.begin(), offset());
}

void CaInlineWriteBuffer::finalizeImpl()
{
    next();
    if (on_inlined)
        on_inlined(std::move(accumulated));
}

void CaInlineWriteBuffer::sync()
{
    next();
}

std::string CaInlineWriteBuffer::getFileName() const
{
    return "ca_inline";
}

}
