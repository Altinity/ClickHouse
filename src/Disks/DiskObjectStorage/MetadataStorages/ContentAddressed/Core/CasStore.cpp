#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasStore.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasEnvelope.h>
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
    extern const int BAD_ARGUMENTS;
    extern const int CORRUPTED_DATA;
    extern const int FILE_DOESNT_EXIST;
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

std::optional<Resolved> Store::resolveRef(const RootNamespace & ns, const String & ref_name)
{
    /// Read side (spec §6): no GC awareness, no tokens. The ref is pure manifest state — resolving it
    /// only reads the owning shard manifest; whether the named tree object is still present is checked
    /// later by readTree (INV-NO-DANGLE surfaces there).
    const auto [root, _] = readShard(ns, shardOf(ref_name));
    auto it = root.refs.find(ref_name);
    if (it == root.refs.end())
        return std::nullopt;

    const RefPayload & payload = it->second;
    return Resolved{
        .tree_id = TreeId(u128ToHex(payload.tree_id)),
        .tree_size = payload.tree_size,
        .mutable_files = payload.mutable_files,
    };
}

std::vector<TreeEntry> Store::readTree(const TreeId & id)
{
    /// A live ref naming a missing tree object is a storage invariant violation (INV-NO-DANGLE): surface
    /// it, never substitute an empty tree or any default. Readers have no condemnation awareness — a
    /// present-but-condemned object reads fine here.
    std::optional<GetResult> object = pool_backend->get(pool_layout.treeKey(id));
    if (!object)
        throw Exception(ErrorCodes::FILE_DOESNT_EXIST,
            "live ref names tree {} but its object is missing — INV-NO-DANGLE", id.string());

    /// Validate the envelope (magic / kind=Tree / header_hash / size arithmetic), then the key↔hash
    /// binding: a tree stored at a key other than hex(logical_hash) is corruption.
    const EnvelopeHeader header = decodeEnvelopeHeader(object->bytes, object->bytes.size(), ObjectKind::Tree);
    if (u128ToHex(header.logical_hash) != id.string())
        throw Exception(ErrorCodes::CORRUPTED_DATA,
            "CAS tree key/hash mismatch: object at tree key {} carries logical_hash {}",
            id.string(), u128ToHex(header.logical_hash));

    return decodeTree(std::string_view(object->bytes).substr(payloadOffset(header)));
}

BlobLocation Store::locate(const TreeEntry & entry) const
{
    /// A ranged read into the content object: the payload starts at a constant offset for blobs
    /// (the pool's fixed blob_header_len — no per-object header read), and at the slice offset for
    /// pack slices. Inline/Subtree carry no standalone object location.
    switch (entry.placement)
    {
        case Placement::Blob:
            return BlobLocation{
                .key = pool_layout.blobKey(BlobId(u128ToHex(entry.file_hash))),
                .offset = meta.blob_header_len,
                .length = entry.file_size,
            };
        case Placement::PackSlice:
            /// Encoded and validated from day one; produced by nobody until packing lands in M-F.
            return BlobLocation{
                .key = pool_layout.packKey(PackId(u128ToHex(entry.pack_hash))),
                .offset = entry.pack_offset,
                .length = entry.pack_length,
            };
        case Placement::Inline:
        case Placement::Subtree:
            throw Exception(ErrorCodes::BAD_ARGUMENTS,
                "entry placement {} has no blob location", static_cast<int>(entry.placement));
    }
    throw Exception(ErrorCodes::BAD_ARGUMENTS,
        "entry placement {} has no blob location", static_cast<int>(entry.placement));
}

std::map<String, Resolved> Store::listRefs(const RootNamespace & ns)
{
    /// Refs are sharded by name across all root shards; the full ref set is the union over every shard.
    std::map<String, Resolved> result;
    for (uint64_t shard = 0; shard < meta.root_shards; ++shard)
    {
        const auto [root, _] = readShard(ns, shard);
        for (const auto & [ref_name, payload] : root.refs)
        {
            result.emplace(ref_name, Resolved{
                .tree_id = TreeId(u128ToHex(payload.tree_id)),
                .tree_size = payload.tree_size,
                .mutable_files = payload.mutable_files,
            });
        }
    }
    return result;
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

std::pair<RootShard, std::optional<Token>> Store::readShard(const RootNamespace & ns, uint64_t shard)
{
    /// An absent shard manifest means the shard holds no refs yet — a fresh, empty manifest with no
    /// token (nothing to CAS against). This is normal state, NOT a fallback masking an error.
    std::optional<GetResult> object = pool_backend->get(pool_layout.rootShardKey(ns, shard));
    if (!object)
        return {RootShard{}, std::nullopt};
    return {decodeRootShard(object->bytes), object->token};
}

}
