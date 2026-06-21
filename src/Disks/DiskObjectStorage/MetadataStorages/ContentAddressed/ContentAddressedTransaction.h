#pragma once
#include <Disks/DiskObjectStorage/MetadataStorages/IMetadataStorage.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/ContentAddressedMetadataStorage.h>
#include <Disks/WriteMode.h>
#include <IO/WriteBufferFromFileBase.h>
#include <IO/WriteSettings.h>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace DB
{

/// The M-W write-path wiring: accumulates ClickHouse operations and maps them to ONE Cas::Build
/// per written part at commit (design 2026-06-11 section 4; plan D-W2).
///
/// T3 state: writeFile (content blobs through a spill+hash buffer into Build::putBlob; mutable
/// per-part bytes staged; verbatim namespace files durable on finalize) + commit (one
/// putTree+publish per staged part, with the reserved .ca_mtime publish stamp) + destructor
/// abandon. Remaining operations land task by task (T5 carry-forward/renames, T6 removals, T7
/// detached/ATTACH/FREEZE, T8 read-your-writes) — each with wiring tests; the SQL suites gate the
/// completed milestone (T13).
class ContentAddressedTransaction : public IMetadataTransaction
{
public:
    explicit ContentAddressedTransaction(ContentAddressedMetadataStorage & metadata_storage_);

    bool supportsChmod() const override { return false; }

    void commit(const TransactionCommitOptionsVariant & options) override;
    TransactionCommitOutcomeVariant tryCommit(const TransactionCommitOptionsVariant & options) override;

    ObjectStorageKey generateObjectKeyForPath(const std::string & path) override;
    StoredObjects getSubmittedForRemovalBlobs() override;

    std::optional<StoredObjects> tryGetInFlightStorageObjects(const std::string & path) const override;
    std::unique_ptr<ReadBufferFromFileBase> tryReadFileInFlight(
        const std::string & path, const ReadSettings & settings, std::optional<size_t> read_hint) const override;
    std::optional<uint64_t> tryGetInFlightFileSize(const std::string & path) const override;
    bool hasInFlightDirectory(const std::string & path) const override;
    std::vector<std::string> listInFlightDirectory(const std::string & path) const override;

    void createMetadataFile(const std::string & path, const StoredObjects & objects) override;

    /// The disk-layer write entry (DiskObjectStorageTransaction's CA branch calls this directly).
    std::unique_ptr<WriteBufferFromFileBase> writeFile(
        const std::string & path,
        size_t buf_size,
        WriteMode mode,
        const WriteSettings & settings);

    void createDirectory(const std::string & path) override;
    void createDirectoryRecursive(const std::string & path) override;
    void removeDirectory(const std::string & path) override;
    void removeRecursive(const std::string & path, const ShouldRemoveObjectsPredicate & should_remove_objects) override;
    void createHardLink(const std::string & path_from, const std::string & path_to) override;
    void setLastModified(const std::string & path, const Poco::Timestamp & timestamp) override;
    void chmod(const String & path, mode_t mode) override;
    void setReadOnly(const std::string & path) override;
    void moveDirectory(const std::string & path_from, const std::string & path_to) override;
    void moveFile(const std::string & path_from, const std::string & path_to) override;
    void replaceFile(const std::string & path_from, const std::string & path_to) override;
    void unlinkFile(const std::string & path, bool if_exists, bool should_remove_objects) override;
    void truncateFile(const std::string & path, size_t size) override;

    ~ContentAddressedTransaction() override;

protected:
    ContentAddressedMetadataStorage & metadata_storage;

private:
    /// One part being assembled by this transaction (D-W2: ONE Cas::Build per written part,
    /// opened lazily at the FIRST staged write — W-HEARTBEAT requires the heartbeat durable
    /// before the first PUT, and buffer finalize uploads immediately).
    struct PartStaging
    {
        Cas::BuildPtr build;                       /// nullptr until the first content upload
        std::vector<Cas::TreeEntry> entries;       /// staged tree entries (uploads + adoptions)
        std::map<std::string, std::string> mutable_files;
        std::set<std::string> mutable_removed;     /// staged deletions for a COMMITTED part's payload
        bool published = false;                    /// the ref is already durably published (at the
                                                   /// lock-free rename); commit() must not re-publish it.

        struct PendingBlob { UInt128 hash; std::string temp_path; uint64_t size = 0; };
        std::vector<PendingBlob> pending_blobs;    /// B188: spilled+hashed locally; uploaded post-precommit
    };

    /// Keyed by (namespace string, ref name) — the routed identity, so live/detached/shadow
    /// stagings never collide.
    std::map<std::pair<std::string, std::string>, PartStaging> parts;
    bool committed = false;

    /// Refs published EARLY (at the lock-free tmp->final rename in moveDirectory), BEFORE the
    /// owning transaction's commit decision. If the transaction is abandoned (destructed without a
    /// successful commit() — e.g. a ZK multi failure on the replicated INSERT path rolls back AFTER
    /// renameParts() already published), these durable refs must be dropped, else a rolled-back
    /// insert's part survives as an orphan ref (resurrection / unexpected-part on restart). B151.
    std::vector<std::pair<Cas::RootNamespace, std::string>> rename_published_refs;

    PartStaging & stagingFor(const ContentAddressedMetadataStorage::Route & r);
    PartStaging * findStaging(const ContentAddressedMetadataStorage::Route & r);
    const Cas::TreeEntry * findStagedEntry(const ContentAddressedMetadataStorage::Route & r) const;
    /// B188: return a pointer to the pending (staged-but-not-yet-uploaded) blob for `hash`, or nullptr
    /// if not pending (already uploaded or never staged).
    const PartStaging::PendingBlob * findPendingBlob(const PartStaging & st, const UInt128 & hash) const;
    Cas::Build & buildFor(const ContentAddressedMetadataStorage::Route & r, PartStaging & st);
    std::optional<ContentAddressedMetadataStorage::Route> routeOf(const std::string & path) const;
    /// Move a COMMITTED ref by republish (adoptTree + publish same tree + dropRef). false = absent source.
    bool republishRef(const Cas::RootNamespace & src_ns, const std::string & src_ref,
                      const Cas::RootNamespace & dst_ns, const std::string & dst_ref);

    /// Idempotent ref removal: drop `ref` if it resolves, tolerating a concurrent drop that races
    /// between the resolve and the drop (the removal unit is meant to be replay-safe).
    void dropRefIfPresent(const Cas::RootNamespace & ns, const std::string & ref);

    void cleanupPendingTempFiles() noexcept;   /// B188: remove all parts' pending temp files (commit/abort/dtor)

    /// B190 Task 4: unified adopt helper that collapses the 6 inline pending/uploaded dispatch
    /// blocks in createHardLink / moveFile (cross-part) / moveDirectory (two-build merge).
    ///
    /// `pb != nullptr` (pending, not yet uploaded):
    ///   - `copy_pending=true`  → push a copy of *pb into dst_st.pending_blobs (hardlink semantics:
    ///     both src and dst upload independently; src's copy is left in place by the caller).
    ///   - `copy_pending=false` → the pb record is already in dst_st (moved or already there);
    ///     just record the dep without any additional push.
    ///   In both cases: dst_build.recordPendingBlobDep(entry.file_hash, entry.file_size).
    ///
    /// `pb == nullptr` (uploaded / committed): dst_build.adoptEvidence(entry) — tokenless W-EVIDENCE,
    /// no pool HEAD/GET before precommit.
    void adoptStagedBlob(const PartStaging::PendingBlob * pb, const Cas::TreeEntry & entry,
                         PartStaging & dst_st, Cas::Build & dst_build, bool copy_pending);

    /// Publish one staged part durably (putTree + publish, or updateRefPayload for a mutable-only
    /// staging) and mark it `published`. Idempotent: a no-op if already published. Returns true iff
    /// this call newly CREATED a ref that did not exist before (for commit()'s rollback tracking).
    bool publishStaging(const Cas::RootNamespace & ns, const std::string & ref, PartStaging & st);
};

}
