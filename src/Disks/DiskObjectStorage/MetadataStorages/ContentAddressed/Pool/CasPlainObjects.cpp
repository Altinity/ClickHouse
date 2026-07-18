#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasPlainObjects.h>
#include <Common/Exception.h>
#include <algorithm>

namespace DB
{
namespace ErrorCodes
{
    extern const int ABORTED;
}
}

namespace DB::Cas
{

namespace
{
    constexpr size_t MAX_CAS_ATTEMPTS = 100;
}

void CasPlainObjects::casPutObject(const String & full_key, const String & bytes)
{
    /// The read determines whether this is a conditional create or replacement. The token is only
    /// valid for the incarnation returned by that head, so a precondition failure means another
    /// writer won the race and the loop must observe the new incarnation before trying again.
    for (size_t attempt = 0; attempt < MAX_CAS_ATTEMPTS; ++attempt)
    {
        HeadResult head = backend.head(full_key);
        if (!head.exists)
        {
            if (backend.putIfAbsent(full_key, bytes).outcome == PutOutcome::Done)
                return;
        }
        else
        {
            if (backend.putOverwrite(full_key, bytes, head.token).outcome == PutOutcome::Done)
                return;
        }
        /// `PreconditionFailed` means the observed state changed under us; re-head and retry.
    }
    throw Exception(ErrorCodes::ABORTED, "object CAS contention on '{}'", full_key);
}

std::optional<String> CasPlainObjects::casGetObject(const String & full_key)
{
    std::optional<GetResult> result = backend.get(full_key);
    if (!result)
        return std::nullopt;
    return result->bytes;
}

void CasPlainObjects::casRemoveObject(const String & full_key)
{
    /// Delete only the incarnation observed by the preceding head. A token mismatch leaves the
    /// replacement untouched and is retried against a fresh observation; absence is a successful
    /// no-op.
    for (size_t attempt = 0; attempt < MAX_CAS_ATTEMPTS; ++attempt)
    {
        const HeadResult head = backend.head(full_key);
        if (!head.exists)
            return;
        const DeleteOutcome outcome = backend.deleteExact(full_key, head.token);
        if (outcome.kind == DeleteOutcome::Kind::Deleted || outcome.kind == DeleteOutcome::Kind::NotFound)
            return;
        /// `TokenMismatch` means a concurrent rewrite; re-head and retry.
    }
    throw Exception(ErrorCodes::ABORTED, "object CAS contention on '{}' (runaway live-lock brake)", full_key);
}

void CasPlainObjects::putNamespaceFile(const RootNamespace & ns, const String & name, const String & bytes)
{
    casPutObject(layout.namespaceFileKey(ns, name), bytes);
}

std::optional<String> CasPlainObjects::getNamespaceFile(const RootNamespace & ns, const String & name)
{
    return casGetObject(layout.namespaceFileKey(ns, name));
}

std::vector<String> CasPlainObjects::listNamespaceFiles(const RootNamespace & ns)
{
    const String prefix = layout.namespaceFilesPrefix(ns);
    std::vector<String> names;
    String cursor;
    while (true)
    {
        ListPage page = backend.list(prefix, cursor, /*limit*/ 1000);
        for (const ListedKey & listed : page.keys)
        {
            /// Strip the storage prefix so callers receive the bare flat file name.
            if (listed.key.starts_with(prefix))
                names.push_back(listed.key.substr(prefix.size()));
        }
        if (page.next_cursor.empty())
            break;
        cursor = page.next_cursor;
    }
    /// Backends are not required to return pages in the same order, so make the public result
    /// deterministic instead of relying on `InMemoryBackend` ordering.
    std::sort(names.begin(), names.end());
    return names;
}

void CasPlainObjects::removeNamespaceFile(const RootNamespace & ns, const String & name)
{
    casRemoveObject(layout.namespaceFileKey(ns, name));
}

void CasPlainObjects::putMountpointObject(const String & key, const String & bytes)
{
    casPutObject(layout.mountpointObjectKey(key), bytes);
}

std::optional<String> CasPlainObjects::getMountpointObject(const String & key)
{
    return casGetObject(layout.mountpointObjectKey(key));
}

bool CasPlainObjects::mountpointObjectExists(const String & key)
{
    /// Use metadata rather than a body GET because a path probe may resolve to a directory, such as
    /// the `store` pool subdirectory traversed by `system.remote_data_paths`. The local backend
    /// treats a directory as not an object, so this returns false instead of attempting a body read
    /// that would raise a filesystem exception for a directory.
    return backend.head(layout.mountpointObjectKey(key)).exists;
}

void CasPlainObjects::removeMountpointObject(const String & key)
{
    casRemoveObject(layout.mountpointObjectKey(key));
}

}
