#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/ContentAddressedTransaction.h>
#include <Common/Exception.h>

namespace DB
{

namespace ErrorCodes
{
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

void ContentAddressedTransaction::commit(const TransactionCommitOptionsVariant &)
{
    notYet("commit");
}

TransactionCommitOutcomeVariant ContentAddressedTransaction::tryCommit(const TransactionCommitOptionsVariant &)
{
    notYet("tryCommit");
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
    const std::string &, size_t, WriteMode, const WriteSettings &)
{
    notYet("writeFile");
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
