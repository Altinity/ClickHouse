#pragma once
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Backend/CasBackend.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Formats/CasLayout.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Primitives/CasTypes.h>
#include <optional>
#include <string>
#include <vector>

namespace DB::Cas
{

/// Provides the pool's plain-object surface: loose, non-content-addressed objects whose key is
/// chosen by the caller. This covers namespace files under `roots/<ns>/_files/` and mountpoint
/// objects mirrored by path. The object bodies are raw passthrough bytes; this component does not
/// decode them as CAS metadata.
///
/// The component holds references to the shared `Backend` and `Layout` only. It owns no pool mutex
/// and has no pool back-reference, allowing `Pool` to retain thin forwarding methods with the same
/// external interface. The private helpers implement the shared head-plus-conditional-write and
/// head-plus-exact-delete protocols used by both object families. A conditional outcome means that
/// the observed incarnation changed, so the helper re-reads the head and retries; the fixed bound
/// prevents an unexpected continuous conflict from becoming an unbounded operation and reports
/// `ABORTED` when it is reached.
class CasPlainObjects
{
public:
    CasPlainObjects(Backend & backend_, const Layout & layout_) : backend(backend_), layout(layout_) {}

    /// Stores the raw bytes under the namespace's `_files/` prefix. Existing files are replaced
    /// conditionally using the incarnation observed by `Backend::head`; a storage failure or an
    /// exhausted conflict-retry bound is propagated as an exception.
    void putNamespaceFile(const RootNamespace & ns, const String & name, const String & bytes);

    /// Reads a namespace file without interpreting its body. Returns `nullopt` when the object is
    /// absent and propagates backend read failures.
    std::optional<String> getNamespaceFile(const RootNamespace & ns, const String & name);

    /// Enumerates the flat file names directly below the namespace's `_files/` prefix. Fetches all
    /// paginated backend results, strips the prefix, and returns names in sorted order independent
    /// of backend listing order.
    std::vector<String> listNamespaceFiles(const RootNamespace & ns);

    /// Removes the current namespace-file incarnation, if any. A concurrent replacement is never
    /// removed accidentally: the exact-delete helper re-reads and retries with the new token.
    void removeNamespaceFile(const RootNamespace & ns, const String & name);

    /// Stores raw bytes for a loose mountpoint file at the path-derived object key. The key is
    /// validated and constructed by `Layout`; this method applies the same conditional overwrite
    /// protocol as namespace files.
    void putMountpointObject(const String & key, const String & bytes);

    /// Reads a path-mirrored mountpoint object as raw bytes. Returns `nullopt` for an absent object
    /// and propagates backend read failures.
    std::optional<String> getMountpointObject(const String & key);

    /// Checks only object metadata, not the body. A directory at the path-derived key is therefore
    /// reported as absent, matching object-store semantics and avoiding a filesystem exception from
    /// attempting to read a directory as an object.
    bool mountpointObjectExists(const String & key);

    /// Removes the current path-mirrored mountpoint-object incarnation, if present, using exact-token
    /// deletion so a concurrent rewrite remains intact.
    void removeMountpointObject(const String & key);

private:
    /// Creates or conditionally replaces one raw object. The method re-heads after a conditional
    /// conflict and throws `ABORTED` after the bounded retry loop cannot establish a stable token.
    void casPutObject(const String & full_key, const String & bytes);

    /// Reads one raw object by its complete backend key and returns `nullopt` when it is absent.
    std::optional<String> casGetObject(const String & full_key);

    /// Removes one raw object by exact token. Absence is a successful no-op; a token mismatch causes
    /// a fresh head and retry, while a bounded retry failure throws `ABORTED`.
    void casRemoveObject(const String & full_key);

    Backend & backend;
    const Layout & layout;
};

}
