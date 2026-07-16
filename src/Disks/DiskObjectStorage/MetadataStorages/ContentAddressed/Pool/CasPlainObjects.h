#pragma once
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Backend/CasBackend.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Formats/CasLayout.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Primitives/CasTypes.h>
#include <optional>
#include <string>
#include <vector>

namespace DB::Cas
{

/// The plain-object surface of the pool: loose, non-content-addressed objects whose key is chosen by
/// the caller (namespace files under `roots/<ns>/_files/`, and loose mountpoint objects). Extracted
/// from `Pool` (spec §Decomposition, warm-up): it is STATELESS over `Backend &` + `const Layout &`
/// -- it owns no `Pool` mutex and holds no `Pool` back-reference, so `Pool` keeps thin delegates
/// with identical signatures and the external API is unchanged.
///
/// The three `cas*Object` helpers are the shared head+conditional-write / head+deleteExact loops that
/// back both the namespace-file and mountpoint-object paths. Single-owner keys make contention a
/// non-event; the attempt bound is a runaway live-lock brake (throws `ABORTED`).
class CasPlainObjects
{
public:
    CasPlainObjects(Backend & backend_, const Layout & layout_) : backend(backend_), layout(layout_) {}

    /// ---- namespace files (verbatim small objects under a namespace's `_files/` prefix) ----
    void putNamespaceFile(const RootNamespace & ns, const String & name, const String & bytes);
    std::optional<String> getNamespaceFile(const RootNamespace & ns, const String & name);
    std::vector<String> listNamespaceFiles(const RootNamespace & ns);
    void removeNamespaceFile(const RootNamespace & ns, const String & name);

    /// ---- plain mountpoint objects (loose, non-content-addressed disk files; design §5.2) ----
    void putMountpointObject(const String & key, const String & bytes);
    std::optional<String> getMountpointObject(const String & key);
    bool mountpointObjectExists(const String & key);
    void removeMountpointObject(const String & key);

private:
    /// ---- plain-object CAS helpers (shared by namespace-file and mountpoint-object paths) ----
    void casPutObject(const String & full_key, const String & bytes);
    std::optional<String> casGetObject(const String & full_key);
    void casRemoveObject(const String & full_key);

    Backend & backend;
    const Layout & layout;
};

}
