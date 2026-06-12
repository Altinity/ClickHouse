#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/ContentAddressedMetadataStorage.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/ContentAddressedTransaction.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasIds.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasObjectStorageBackend.h>
#include <Disks/DiskObjectStorage/MetadataStorages/StaticDirectoryIterator.h>
#include <IO/ReadBufferFromFileView.h>
#include <IO/ReadBufferFromMemory.h>
#include <IO/ReadPipeline.h>
#include <Common/Exception.h>
#include <Common/logger_useful.h>
#include <filesystem>
#include <unordered_set>

namespace fs = std::filesystem;

namespace DB
{

namespace ErrorCodes
{
    extern const int FILE_DOESNT_EXIST;
    extern const int LOGICAL_ERROR;
}

namespace
{

/// Wiring-reserved RefPayload.mutable_files keys (never real MergeTree files — dot-prefixed).
/// `.ca_mtime` carries the publish wall-clock (decimal seconds) for getLastModified.
bool isReservedMutableName(const std::string & name)
{
    return name.starts_with(".ca_");
}

/// A projection DIRECTORY is recognized by its LAST path component (.proj / .tmp_proj) — the same
/// recognizer the PoC used (B64: also matches the nested detached-staging shape). `file` here is
/// the ROUTED in-tree file path (the detached part prefix already split away).
std::optional<std::string> projectionDirPrefix(const std::string & file)
{
    if (file.empty())
        return std::nullopt;
    const auto last_slash = file.find_last_of('/');
    const std::string_view last_component
        = last_slash == std::string::npos ? std::string_view(file) : std::string_view(file).substr(last_slash + 1);
    if (last_component.ends_with(".proj") || last_component.ends_with(".tmp_proj"))
        return file + "/";
    return std::nullopt;
}

/// "<first>/<rest...>" -> {first, rest} ({whole, ""} when there is no '/').
std::pair<std::string, std::string> splitFirstComponent(const std::string & s)
{
    const auto slash = s.find('/');
    if (slash == std::string::npos)
        return {s, ""};
    return {s.substr(0, slash), s.substr(slash + 1)};
}

void addFirstComponent(std::unordered_set<std::string> & out, const std::string & name)
{
    const auto slash = name.find('/');
    out.emplace(slash == std::string::npos ? name : name.substr(0, slash));
}

std::vector<std::string> toVector(std::unordered_set<std::string> && set)
{
    return std::vector<std::string>(std::make_move_iterator(set.begin()), std::make_move_iterator(set.end()));
}

/// The server uuid string (with dashes) -> the core's UInt128 server id.
UInt128 serverIdToU128(const std::string & server_id)
{
    String hex;
    hex.reserve(32);
    for (char c : server_id)
        if (c != '-')
            hex += c;
    if (hex.size() == 32)
        return Cas::hexToU128(hex);
    /// Unit-test ids ("srv1") are not uuids — hash them stably.
    UInt128 r{};
    for (char c : server_id)
        r = r * 131 + static_cast<unsigned char>(c);
    return r == UInt128(0) ? UInt128(1) : r;
}

}

ContentAddressedMetadataStorage::ContentAddressedMetadataStorage(
    ObjectStoragePtr object_storage_,
    String storage_path_prefix_,
    String server_id_,
    String local_scratch_path_,
    ContextPtr context_)
    : object_storage(std::move(object_storage_))
    , storage_path_prefix(std::move(storage_path_prefix_))
    , storage_path_full(fs::path(object_storage->getRootPrefix()) / storage_path_prefix)
    , server_id(std::move(server_id_))
    , local_scratch_path(std::move(local_scratch_path_))
    , context(context_)
{
}

void ContentAddressedMetadataStorage::startup()
{
    if (cas_store)
        return;

    /// Native mode rides real conditional ops (probed fail-closed by Store::open); Local object
    /// storage has none, so the backend emulates exact token semantics in-process (single server).
    const auto mode = object_storage->getType() == ObjectStorageType::Local
        ? Cas::ObjectStorageBackend::Mode::EmulatedSingleProcess
        : Cas::ObjectStorageBackend::Mode::Native;
    auto backend = std::make_shared<Cas::ObjectStorageBackend>(object_storage, mode);

    Cas::PoolConfig pool_config;
    pool_config.pool_prefix = storage_path_prefix;
    pool_config.server_id = serverIdToU128(server_id);
    pool_config.background_heartbeats = context != nullptr;
    cas_store = Cas::Store::open(std::move(backend), std::move(pool_config));
    pool_uuid = Cas::u128ToHex(cas_store->poolMeta().pool_id);

    /// The background GC scheduler lands in M-W T10 (context-gated).
}

void ContentAddressedMetadataStorage::shutdown()
{
    cas_store.reset();
}

const Cas::StorePtr & ContentAddressedMetadataStorage::store() const
{
    if (!cas_store)
        throw Exception(ErrorCodes::LOGICAL_ERROR,
            "ContentAddressedMetadataStorage: store accessed before startup");
    return cas_store;
}

MetadataTransactionPtr ContentAddressedMetadataStorage::createTransaction()
{
    return std::make_shared<ContentAddressedTransaction>(*this);
}

/// ==== D-W1 namespace mapping ====

Cas::RootNamespace ContentAddressedMetadataStorage::liveNamespace(const std::string & table_uuid) const
{
    return Cas::RootNamespace{server_id + "/" + table_uuid};
}

Cas::RootNamespace ContentAddressedMetadataStorage::detachedNamespace(const std::string & table_uuid) const
{
    return Cas::RootNamespace{server_id + "/detached/" + table_uuid};
}

Cas::RootNamespace ContentAddressedMetadataStorage::shadowNamespace(const std::string & shadow_table_dir)
{
    /// The LITERAL shadow table dir (shadow/<backup>/store/<u3>/<uuid> or .../data/<db>/<tbl>):
    /// bijective with the disk path for both layouts, pool-global (backups are read by any
    /// replica), and the shadow tree enumerates from Store::listNamespaces("shadow/").
    return Cas::RootNamespace{shadow_table_dir};
}

Cas::RootNamespace ContentAddressedMetadataStorage::genericNamespace() const
{
    /// Generic disk-root files (the startup access-check probe). "_disk" can never collide with a
    /// real table identifier (uuids / data/<db>/<tbl> paths).
    return Cas::RootNamespace{server_id + "/_disk"};
}

std::optional<ContentAddressedMetadataStorage::Route>
ContentAddressedMetadataStorage::route(const ContentAddressed::PartFilePath & p) const
{
    Route r;
    if (!p.backup_name.empty())
    {
        r.ns = shadowNamespace(p.shadow_table_dir);
        r.ref = p.part_name;
        r.file = p.file;
        return r;
    }
    if (p.part_name == ContentAddressed::kDetachedDirName)
    {
        /// The parser reports detached paths with part_name == "detached" and the real detached
        /// part dir as the first component of `file` (the PoC contract). The NEW layout keeps each
        /// detached part as its OWN ref in the detached namespace, so re-split here.
        r.ns = detachedNamespace(p.table_uuid);
        std::tie(r.ref, r.file) = splitFirstComponent(p.file);
        return r;
    }
    r.ns = liveNamespace(p.table_uuid);
    r.ref = p.part_name;
    r.file = p.file;
    return r;
}

std::optional<std::pair<Cas::Resolved, std::vector<Cas::TreeEntry>>>
ContentAddressedMetadataStorage::resolveRouted(const Route & r) const
{
    auto resolved = store()->resolveRef(r.ns, r.ref);
    if (!resolved)
        return std::nullopt;
    /// A live ref to a missing/corrupt tree throws (INV-NO-DANGLE surfaced, never substituted).
    return std::make_pair(*resolved, store()->readTree(resolved->tree_id));
}

/// ==== read surface ====

bool ContentAddressedMetadataStorage::existsFile(const std::string & path) const
{
    if (!ContentAddressed::isPartFilePath(path))
    {
        if (auto tf = ContentAddressed::parseTableFilePath(path))
            return store()->getNamespaceFile(liveNamespace(tf->table_uuid), tf->tail).has_value();
        return store()->getNamespaceFile(genericNamespace(), path).has_value();
    }

    auto p = ContentAddressed::parsePartFilePath(path);
    if (!p || p->file.empty())
        return false;
    auto r = route(*p);
    if (!r || r->file.empty())
        return false;

    if (ContentAddressed::isMutablePerPartFile(r->file))
    {
        auto resolved = store()->resolveRef(r->ns, r->ref);
        return resolved && !isReservedMutableName(r->file) && resolved->mutable_files.contains(r->file);
    }

    auto rt = resolveRouted(*r);
    if (!rt)
        return false;
    for (const auto & entry : rt->second)
        if (entry.name == r->file)
            return true;
    return false;
}

bool ContentAddressedMetadataStorage::existsDirectory(const std::string & path) const
{
    /// FREEZE shadow namespace — routed BEFORE the live branches (a shadow table dir also
    /// satisfies parseTableUuid).
    if (ContentAddressed::isShadowPath(path))
    {
        if (auto p = ContentAddressed::parsePartFilePath(path); p && !p->backup_name.empty() && p->file.empty())
            return store()->resolveRef(shadowNamespace(p->shadow_table_dir), p->part_name).has_value();
        if (ContentAddressed::endsWithTableUuidPair(path))
            return !store()->listRefs(shadowNamespace(path)).empty();
        /// Intermediate dir (shadow/<bk>, shadow/<bk>/store, ...): exists iff some shadow
        /// namespace (= a literal shadow table dir) lives under it.
        const std::string prefix = path + "/";
        for (const auto & ns : store()->listNamespaces("shadow/"))
            if (ns.starts_with(prefix))
                return true;
        return false;
    }

    if (auto uuid = ContentAddressed::parseTableUuid(path))
        /// Table dir exists iff it has at least one committed part (the PoC's refs-only rule).
        return !store()->listRefs(liveNamespace(*uuid)).empty();

    auto p = ContentAddressed::parsePartFilePath(path);
    if (p)
    {
        auto r = route(*p);
        /// The detached CONTAINER dir <table>/detached: route gives an empty ref.
        if (r && r->ref.empty() && p->part_name == ContentAddressed::kDetachedDirName)
            return !store()->listRefs(r->ns).empty();
        /// A part dir (live, detached, or shadow): exists iff its ref is present.
        if (r && !r->ref.empty() && r->file.empty())
            return store()->resolveRef(r->ns, r->ref).has_value();
        /// A projection dir: at least one tree entry (or mutable file) under its prefix.
        if (r && !r->ref.empty())
        {
            if (auto prefix = projectionDirPrefix(r->file))
            {
                auto rt = resolveRouted(*r);
                if (!rt)
                    return false;
                for (const auto & entry : rt->second)
                    if (entry.name.starts_with(*prefix))
                        return true;
                for (const auto & [file, _] : rt->first.mutable_files)
                    if (file.starts_with(*prefix))
                        return true;
                return false;
            }
        }
    }

    /// A table-level SUBDIRECTORY (deduplication_logs/...): at least one verbatim file under it.
    if (auto tf = ContentAddressed::parseTableFilePath(path))
    {
        const std::string prefix = tf->tail + "/";
        for (const auto & name : store()->listNamespaceFiles(liveNamespace(tf->table_uuid)))
            if (name.starts_with(prefix))
                return true;
        return false;
    }
    return false;
}

bool ContentAddressedMetadataStorage::existsFileOrDirectory(const std::string & path) const
{
    return existsFile(path) || existsDirectory(path);
}

uint64_t ContentAddressedMetadataStorage::getFileSize(const std::string & path) const
{
    if (auto bytes = tryGetInManifestBytes(path))
        return bytes->size();

    if (!ContentAddressed::isPartFilePath(path))
        throw Exception(ErrorCodes::FILE_DOESNT_EXIST, "ContentAddressed: no object for {}", path);

    auto p = ContentAddressed::parsePartFilePath(path);
    if (!p || p->file.empty())
        throw Exception(ErrorCodes::FILE_DOESNT_EXIST, "ContentAddressed: not a part file path: {}", path);
    auto r = route(*p);
    if (!r || r->file.empty())
        throw Exception(ErrorCodes::FILE_DOESNT_EXIST, "ContentAddressed: not a part file path: {}", path);

    auto rt = resolveRouted(*r);
    if (!rt)
        throw Exception(ErrorCodes::FILE_DOESNT_EXIST, "ContentAddressed: no ref for {}", path);
    for (const auto & entry : rt->second)
        if (entry.name == r->file)
            return entry.file_size;
    throw Exception(ErrorCodes::FILE_DOESNT_EXIST, "ContentAddressed: file {} not in tree of {}", r->file, path);
}

Poco::Timestamp ContentAddressedMetadataStorage::getLastModified(const std::string & path) const
{
    /// Timestamps are DERIVED for content addressing: the part's publish wall-clock, stamped by
    /// the transaction into the reserved `.ca_mtime` payload key (decimal seconds). Every shape
    /// (part dir, detached part dir, projection dir, part file) reports its part's stamp; a part
    /// published without a stamp reports the epoch (harmless: stamps only feed cleanup TTLs and
    /// system tables).
    auto resolve_stamp = [&](const Route & r) -> Poco::Timestamp
    {
        auto resolved = store()->resolveRef(r.ns, r.ref);
        if (!resolved)
            throw Exception(ErrorCodes::FILE_DOESNT_EXIST, "ContentAddressed: no ref for {}", path);
        auto it = resolved->mutable_files.find(".ca_mtime");
        if (it == resolved->mutable_files.end())
            return Poco::Timestamp(0);
        return Poco::Timestamp::fromEpochTime(static_cast<time_t>(std::stoull(it->second)));
    };

    if (auto p = ContentAddressed::parsePartFilePath(path))
    {
        auto r = route(*p);
        if (r && !r->ref.empty())
            return resolve_stamp(*r);
    }
    /// Table-level / generic verbatim files: no per-object mtime is kept — epoch.
    if (existsFile(path))
        return Poco::Timestamp(0);
    throw Exception(ErrorCodes::FILE_DOESNT_EXIST, "ContentAddressed: no object for {}", path);
}

std::vector<std::string> ContentAddressedMetadataStorage::listDirectory(const std::string & path) const
{
    if (ContentAddressed::isShadowPath(path))
    {
        /// Shadow PART dir: the frozen part's file names (first components).
        if (auto p = ContentAddressed::parsePartFilePath(path); p && !p->backup_name.empty() && p->file.empty())
        {
            auto rt = resolveRouted(Route{shadowNamespace(p->shadow_table_dir), p->part_name, ""});
            if (!rt)
                return {};
            std::unordered_set<std::string> result;
            for (const auto & entry : rt->second)
                addFirstComponent(result, entry.name);
            for (const auto & [file, _] : rt->first.mutable_files)
                if (!isReservedMutableName(file))
                    addFirstComponent(result, file);
            return toVector(std::move(result));
        }
        /// Shadow TABLE dir: the frozen part names.
        if (ContentAddressed::endsWithTableUuidPair(path))
        {
            std::vector<std::string> result;
            for (const auto & [ref, _] : store()->listRefs(shadowNamespace(path)))
                result.push_back(ref);
            return result;
        }
        /// Shadow INTERMEDIATE dir: derive children from the registered shadow namespaces.
        const std::string prefix = path.empty() ? "shadow" : path;
        std::unordered_set<std::string> result;
        for (const auto & ns : store()->listNamespaces("shadow/"))
            if (ns.starts_with(prefix + "/"))
                addFirstComponent(result, ns.substr(prefix.size() + 1));
        return toVector(std::move(result));
    }

    /// Table dir: part names from refs + table-level verbatim file names (first components —
    /// StorageMergeTree::loadMutations scans this dir for mutation_* entries).
    if (auto uuid = ContentAddressed::parseTableUuid(path))
    {
        std::unordered_set<std::string> result;
        for (const auto & [ref, _] : store()->listRefs(liveNamespace(*uuid)))
            result.emplace(ref);
        for (const auto & name : store()->listNamespaceFiles(liveNamespace(*uuid)))
            addFirstComponent(result, name);
        return toVector(std::move(result));
    }

    if (auto p = ContentAddressed::parsePartFilePath(path))
    {
        auto r = route(*p);
        /// The detached CONTAINER dir: the detached part directory names (B36's intent — never
        /// the files inside, never a dir-stripped mutable file).
        if (r && r->ref.empty() && p->part_name == ContentAddressed::kDetachedDirName)
        {
            std::vector<std::string> result;
            for (const auto & [ref, _] : store()->listRefs(r->ns))
                result.push_back(ref);
            return result;
        }
        /// A part dir (live, detached part, shadow handled above): logical file names, nested
        /// keys collapsed to their first component (projections surface as ONE <proj>.proj entry).
        if (r && !r->ref.empty() && r->file.empty())
        {
            auto rt = resolveRouted(*r);
            if (!rt)
                return {};
            std::unordered_set<std::string> result;
            for (const auto & entry : rt->second)
                addFirstComponent(result, entry.name);
            for (const auto & [file, _] : rt->first.mutable_files)
                if (!isReservedMutableName(file))
                    addFirstComponent(result, file);
            return toVector(std::move(result));
        }
        /// A projection dir: inner names with the <proj>.proj/ prefix stripped.
        if (r && !r->ref.empty())
        {
            if (auto prefix = projectionDirPrefix(r->file))
            {
                auto rt = resolveRouted(*r);
                if (!rt)
                    return {};
                std::unordered_set<std::string> result;
                for (const auto & entry : rt->second)
                    if (entry.name.starts_with(*prefix))
                        result.emplace(entry.name.substr(prefix->size()));
                for (const auto & [file, _] : rt->first.mutable_files)
                    if (!isReservedMutableName(file) && file.starts_with(*prefix))
                        result.emplace(file.substr(prefix->size()));
                return toVector(std::move(result));
            }
        }
    }

    /// A table-level SUBDIRECTORY: verbatim files under <subdir>/, first-component collapsed.
    if (auto tf = ContentAddressed::parseTableFilePath(path))
    {
        const std::string prefix = tf->tail + "/";
        std::unordered_set<std::string> result;
        for (const auto & name : store()->listNamespaceFiles(liveNamespace(tf->table_uuid)))
            if (name.starts_with(prefix))
                addFirstComponent(result, name.substr(prefix.size()));
        return toVector(std::move(result));
    }

    return {};
}

DirectoryIteratorPtr ContentAddressedMetadataStorage::iterateDirectory(const std::string & path) const
{
    /// Mirror MetadataStorageFromPlainObjectStorage: iterateDirectory includes the path.
    auto names = listDirectory(path);
    std::vector<fs::path> fs_paths;
    fs_paths.reserve(names.size());
    for (const auto & child : names)
        fs_paths.push_back(fs::path(path) / child);
    return std::make_unique<StaticDirectoryIterator>(std::move(fs_paths));
}

bool ContentAddressedMetadataStorage::isDirectoryEmpty(const std::string & path) const
{
    /// A part directory's files are virtual (derived from the tree): report it EMPTY so
    /// DiskObjectStorage::removeDirectory proceeds straight to the ref-unlink instead of throwing
    /// CANNOT_RMDIR per removal (the PoC's B45). Same for a projection subdir (B60). The detached
    /// CONTAINER and TABLE dirs keep the listing-based emptiness (DROP TABLE's non-empty guard).
    if (auto p = ContentAddressed::parsePartFilePath(path))
    {
        auto r = route(*p);
        if (r && !r->ref.empty() && r->file.empty())
            return true;
        if (r && !r->ref.empty() && projectionDirPrefix(r->file))
            return true;
    }
    return !iterateDirectory(path)->isValid();
}

StoredObjects ContentAddressedMetadataStorage::getStorageObjects(const std::string & path) const
{
    /// In-manifest bytes (mutable per-part files, inline entries, verbatim namespace files) have
    /// no object of their own: DiskObjectStorage::prepareRead serves them via tryGetInManifestBytes
    /// BEFORE asking for storage objects. The sized empty-key placeholder below keeps size-only
    /// consumers working and makes any bypassing reader fail LOUDLY (never silently wrong bytes).
    if (auto bytes = tryGetInManifestBytes(path))
        return {StoredObject("", path, bytes->size())};

    if (!ContentAddressed::isPartFilePath(path))
        throw Exception(ErrorCodes::FILE_DOESNT_EXIST, "ContentAddressed: no object for {}", path);

    auto p = ContentAddressed::parsePartFilePath(path);
    if (!p || p->file.empty())
        throw Exception(ErrorCodes::FILE_DOESNT_EXIST, "ContentAddressed: not a part file path: {}", path);
    auto r = route(*p);
    if (!r || r->file.empty())
        throw Exception(ErrorCodes::FILE_DOESNT_EXIST, "ContentAddressed: not a part file path: {}", path);

    auto rt = resolveRouted(*r);
    if (!rt)
        throw Exception(ErrorCodes::FILE_DOESNT_EXIST, "ContentAddressed: no ref for {}", path);
    for (const auto & entry : rt->second)
    {
        if (entry.name != r->file)
            continue;
        const auto location = store()->locate(entry);
        /// StoredObject carries no range (the recorded upstream delta) — the PAYLOAD length is the
        /// size (what every size consumer wants); the header offset is applied by
        /// prepareReadPipeline's view, the only byte-reading path.
        return {StoredObject(location.key, path, location.length)};
    }
    throw Exception(ErrorCodes::FILE_DOESNT_EXIST, "ContentAddressed: file {} not in tree of {}", r->file, path);
}

std::optional<String> ContentAddressedMetadataStorage::tryGetInManifestBytes(const std::string & path) const
{
    if (!cas_store)
        return std::nullopt;

    if (!ContentAddressed::isPartFilePath(path))
    {
        if (auto tf = ContentAddressed::parseTableFilePath(path))
            return store()->getNamespaceFile(liveNamespace(tf->table_uuid), tf->tail);
        return store()->getNamespaceFile(genericNamespace(), path);
    }

    auto p = ContentAddressed::parsePartFilePath(path);
    if (!p || p->file.empty())
        return std::nullopt;
    auto r = route(*p);
    if (!r || r->file.empty())
        return std::nullopt;

    if (ContentAddressed::isMutablePerPartFile(r->file))
    {
        auto resolved = store()->resolveRef(r->ns, r->ref);
        if (!resolved || isReservedMutableName(r->file))
            return std::nullopt;
        auto it = resolved->mutable_files.find(r->file);
        if (it == resolved->mutable_files.end())
            return std::nullopt;
        return it->second;
    }

    auto rt = resolveRouted(*r);
    if (!rt)
        return std::nullopt;
    for (const auto & entry : rt->second)
        if (entry.name == r->file && entry.placement == Cas::Placement::Inline)
            return entry.inline_bytes;
    return std::nullopt;
}

bool ContentAddressedMetadataStorage::prepareReadPipeline(
    const std::string & path, const ReadSettings & settings, ReadPipeline & pipeline) const
{
    /// In-manifest bytes (mutable per-part files, inline entries, verbatim namespace files):
    /// served from memory — there is no object to read.
    if (auto bytes = tryGetInManifestBytes(path))
    {
        const auto size = bytes->size();
        auto creator = [path, data = std::move(*bytes)](
            const StoredObject &, const ReadSettings &, bool, bool) -> std::unique_ptr<ReadBufferFromFileBase>
        {
            return std::make_unique<ReadBufferFromOwnMemoryFile>(path, data);
        };
        pipeline.setSource(std::move(creator), {StoredObject("", path, size)}, settings);
        return true;
    }

    /// Blob-backed part file: read the object and expose ONLY the payload through a view (the
    /// CHCA envelope header is `location.offset` bytes at the front).
    if (!ContentAddressed::isPartFilePath(path))
        return false;
    auto p = ContentAddressed::parsePartFilePath(path);
    if (!p || p->file.empty())
        return false;
    auto r = route(*p);
    if (!r || r->file.empty())
        return false;
    auto rt = resolveRouted(*r);
    if (!rt)
        return false;
    for (const auto & entry : rt->second)
    {
        if (entry.name != r->file)
            continue;
        const auto location = store()->locate(entry);
        auto creator = [storage = object_storage, location, path](
            const StoredObject &, const ReadSettings & creator_settings, bool, bool) -> std::unique_ptr<ReadBufferFromFileBase>
        {
            auto impl = storage->readObject(
                StoredObject(location.key, path, location.offset + location.length), creator_settings);
            return std::make_unique<ReadBufferFromFileView>(
                std::move(impl), path, location.offset, location.offset + location.length);
        };
        pipeline.setSource(std::move(creator), {StoredObject(location.key, path, location.length)}, settings);
        return true;
    }
    return false;
}

/// ==== IContentAddressedExchange (relink lands in M-W T11; SAFE DEGRADATION until then) ====

std::optional<String> ContentAddressedMetadataStorage::getPartTreeId(const String & /*part_path*/) const
{
    /// T2 degradation: no relink offer => the sender streams bytes (correct, just slower).
    return std::nullopt;
}

bool ContentAddressedMetadataStorage::adoptPart(
    const String & /*table_uuid*/, const String & /*part_name*/,
    const String & /*tree_id_hex*/, const std::map<String, String> & /*mutable_files*/)
{
    /// T2 degradation: adoption "not possible" => the receiver falls back to a byte fetch.
    return false;
}

}
