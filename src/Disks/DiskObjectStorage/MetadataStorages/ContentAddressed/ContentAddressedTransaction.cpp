#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/ContentAddressedTransaction.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/ContentAddressedWriteBuffers.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasBuild.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasIds.h>
#include <IO/ReadBufferFromFile.h>
#include <IO/copyData.h>
#include <Common/Exception.h>
#include <ctime>

namespace DB
{

namespace ErrorCodes
{
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

void ContentAddressedTransaction::commit(const TransactionCommitOptionsVariant &)
{
    /// One putTree + publish per staged part (D-W2); a staging with NO new uploads on an EXISTING
    /// ref is a mutable-payload update (updateRefPayload — no journal record, no tree rebuild).
    for (auto & [key, st] : parts)
    {
        const Cas::RootNamespace ns{key.first};
        const std::string & ref = key.second;

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
            continue;
        }

        if (!st.build)
            throw Exception(ErrorCodes::LOGICAL_ERROR,
                "ContentAddressedTransaction: staged entries for {}/{} without a Build", key.first, ref);

        auto tree = st.build->putTree(st.entries);
        Cas::RefPayload payload;
        payload.mutable_files = st.mutable_files;
        /// The publish wall-clock stamp backing getLastModified (reserved name, filtered from
        /// every listing by the read side).
        payload.mutable_files[".ca_mtime"] = std::to_string(static_cast<uint64_t>(::time(nullptr)));
        st.build->publish(ns, ref, tree, std::move(payload));
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

std::optional<StoredObjects> ContentAddressedTransaction::tryGetInFlightStorageObjects(const std::string &) const
{
    return {};
}

std::unique_ptr<ReadBufferFromFileBase> ContentAddressedTransaction::tryReadFileInFlight(
    const std::string &, const ReadSettings &, std::optional<size_t>) const
{
    return nullptr;
}

std::optional<uint64_t> ContentAddressedTransaction::tryGetInFlightFileSize(const std::string &) const
{
    return {};
}

bool ContentAddressedTransaction::hasInFlightDirectory(const std::string &) const
{
    return false;
}

std::vector<std::string> ContentAddressedTransaction::listInFlightDirectory(const std::string &) const
{
    return {};
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
        Cas::RootNamespace ns{""};
        std::string name;
        if (auto tf = ContentAddressed::parseTableFilePath(path))
        {
            ns = metadata_storage.liveNamespace(tf->table_uuid);
            name = tf->tail;
        }
        else
        {
            ns = metadata_storage.genericNamespace();
            name = path;
        }
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

void ContentAddressedTransaction::removeDirectory(const std::string &)
{
    notYet("removeDirectory");
}

void ContentAddressedTransaction::removeRecursive(const std::string &, const ShouldRemoveObjectsPredicate &)
{
    notYet("removeRecursive");
}

void ContentAddressedTransaction::createHardLink(const std::string &, const std::string &)
{
    notYet("createHardLink");
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

void ContentAddressedTransaction::moveDirectory(const std::string &, const std::string &)
{
    notYet("moveDirectory");
}

void ContentAddressedTransaction::moveFile(const std::string &, const std::string &)
{
    notYet("moveFile");
}

void ContentAddressedTransaction::replaceFile(const std::string &, const std::string &)
{
    notYet("replaceFile");
}

void ContentAddressedTransaction::unlinkFile(const std::string &, bool, bool)
{
    notYet("unlinkFile");
}

void ContentAddressedTransaction::truncateFile(const std::string &, size_t)
{
    notYet("truncateFile");
}

}
