#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasStore.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasProbe.h>
#include <Common/Exception.h>
#include <city.h>
#include <algorithm>

namespace DB
{
namespace ErrorCodes
{
    extern const int NOT_IMPLEMENTED;
    extern const int ABORTED;
}
}

namespace DB::Cas
{

Store::Store(BackendPtr backend_, PoolConfig config_, PoolMeta meta_)
    : pool_backend(std::move(backend_))
    , config(std::move(config_))
    , meta(std::move(meta_))
    , pool_layout(config.pool_prefix)
    , retire_view(pool_backend, pool_layout)
{
}

StorePtr Store::open(BackendPtr backend, PoolConfig config)
{
    /// FAIL-CLOSED (design §6): the capability probe throws NOT_IMPLEMENTED on any failed check, and
    /// PoolMeta::createOrValidate is pool-authoritative — the config constants apply only at creation.
    Layout layout(config.pool_prefix);
    runCapabilityProbe(*backend, config.pool_prefix + "/_probe");
    PoolMeta meta = PoolMeta::createOrValidate(*backend, layout, config.root_shards, config.blob_header_len);

    /// Private ctor: make_shared cannot reach it.
    StorePtr store(new Store(std::move(backend), std::move(config), std::move(meta)));

    /// Prime the writer-side retire view once (rare by construction; see RetireView).
    store->retire_view.refresh();
    return store;
}

uint64_t Store::shardOf(const String & ref_name) const
{
    /// root_shards >= 1 is a PoolMeta invariant, so the modulus is always well-defined.
    return CityHash_v1_0_2::CityHash64(ref_name.data(), ref_name.size()) % meta.root_shards;
}

void Store::putNamespaceFile(const RootNamespace & ns, const String & name, const String & bytes)
{
    /// Verbatim files are plain keys, never content-addressed: head + conditional write, with a bounded
    /// retry on contention. These keys have a single owner, so a contention storm is impossible — the
    /// bound is purely a runaway brake.
    const String key = pool_layout.namespaceFileKey(ns, name);
    for (size_t attempt = 0; attempt < 100; ++attempt)
    {
        HeadResult head = pool_backend->head(key);
        if (!head.exists)
        {
            if (pool_backend->putIfAbsent(key, bytes) == PutOutcome::Done)
                return;
        }
        else
        {
            if (pool_backend->putOverwrite(key, bytes, head.token) == PutOutcome::Done)
                return;
        }
        /// PreconditionFailed ⇒ the observed state changed under us; re-head and retry.
    }
    throw Exception(ErrorCodes::ABORTED, "verbatim file CAS contention on '{}'", key);
}

std::optional<String> Store::getNamespaceFile(const RootNamespace & ns, const String & name)
{
    const String key = pool_layout.namespaceFileKey(ns, name);
    std::optional<GetResult> result = pool_backend->get(key);
    if (!result)
        return std::nullopt;
    return result->bytes;
}

std::vector<String> Store::listNamespaceFiles(const RootNamespace & ns)
{
    const String prefix = pool_layout.namespaceFilesPrefix(ns);
    std::vector<String> names;
    String cursor;
    while (true)
    {
        ListPage page = pool_backend->list(prefix, cursor, /*limit*/ 1000);
        for (const ListedKey & listed : page.keys)
        {
            /// Strip the prefix to yield the bare flat file name.
            if (listed.key.size() >= prefix.size() && listed.key.compare(0, prefix.size(), prefix) == 0)
                names.push_back(listed.key.substr(prefix.size()));
        }
        if (page.next_cursor.empty())
            break;
        cursor = page.next_cursor;
    }
    /// The InMemoryBackend lists sorted, but sort explicitly to stay backend-agnostic.
    std::sort(names.begin(), names.end());
    return names;
}

BuildPtr Store::startBuild(BuildInfo)
{
    throw Exception(ErrorCodes::NOT_IMPLEMENTED, "Store::startBuild: M-C2 Task 11");
}

std::optional<Resolved> Store::resolveRef(const RootNamespace &, const String &)
{
    throw Exception(ErrorCodes::NOT_IMPLEMENTED, "Store::resolveRef: M-C2 Task 9");
}

std::vector<TreeEntry> Store::readTree(const TreeId &)
{
    throw Exception(ErrorCodes::NOT_IMPLEMENTED, "Store::readTree: M-C2 Task 9");
}

BlobLocation Store::locate(const TreeEntry &) const
{
    throw Exception(ErrorCodes::NOT_IMPLEMENTED, "Store::locate: M-C2 Task 9");
}

std::map<String, Resolved> Store::listRefs(const RootNamespace &)
{
    throw Exception(ErrorCodes::NOT_IMPLEMENTED, "Store::listRefs: M-C2 Task 9");
}

void Store::dropRef(const RootNamespace &, const String &)
{
    throw Exception(ErrorCodes::NOT_IMPLEMENTED, "Store::dropRef: M-C2 Task 10");
}

void Store::updateRefPayload(const RootNamespace &, const String &, std::function<void(RefPayload &)>)
{
    throw Exception(ErrorCodes::NOT_IMPLEMENTED, "Store::updateRefPayload: M-C2 Task 10");
}

void Store::dropNamespace(const RootNamespace &)
{
    throw Exception(ErrorCodes::NOT_IMPLEMENTED, "Store::dropNamespace: M-C2 Task 10");
}

std::pair<RootShard, std::optional<Token>> Store::readShard(const RootNamespace &, uint64_t)
{
    throw Exception(ErrorCodes::NOT_IMPLEMENTED, "Store::readShard: M-C2 Task 9");
}

}
