#pragma once
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasBackend.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasIds.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasLayout.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasPoolMeta.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasRetireView.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasRootShardCodec.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasTreeCodec.h>
#include <chrono>
#include <functional>
#include <map>
#include <memory>
#include <optional>

namespace DB::Cas
{

struct PoolConfig
{
    String pool_prefix;
    UInt128 server_id{};                      /// provenance + heartbeats
    uint64_t root_shards = 8;                 /// creation-time only; pool is authoritative on reopen
    uint64_t blob_header_len = 256;           /// creation-time only; ditto
    uint64_t manifest_soft_limit = 16ULL << 20;
    uint64_t manifest_hard_limit = 64ULL << 20;
    std::chrono::milliseconds heartbeat_period{5000};
    bool background_heartbeats = false;       /// tests drive renewOnce explicitly
};

struct Resolved
{
    TreeId tree_id;
    uint64_t tree_size = 0;
    std::map<String, String> mutable_files;
};

struct BlobLocation
{
    String key;
    uint64_t offset = 0;                      /// payload start within the object
    uint64_t length = 0;
};

struct BuildInfo
{
    std::optional<String> intended_ref;       /// "ns/ref" forensics for the envelope (diagnostic)
    ProvenanceOp op = ProvenanceOp::Other;
};

class Build;
using BuildPtr = std::shared_ptr<Build>;
class Gc;
class Store;
using StorePtr = std::shared_ptr<Store>;

/// One content-addressed pool. open is FAIL-CLOSED: capability probe + pool-format check; any
/// failure refuses the pool (design §6). The read side has no GC awareness and no tokens (spec §6).
class Store : public std::enable_shared_from_this<Store>
{
    /// Build drives the manifest publish CAS through the private mutateShard/shardOf: the gate logic
    /// (W-PUBLISH-GATE) is Build's responsibility (it owns deps/retireView access), but the CAS loop
    /// itself is the verified Store loop — reused, never duplicated.
    friend class Build;
    /// Gc drives the manifest fence CAS (R3) through the same private mutateShard loop — reused,
    /// never duplicated (the lease itself only needs the public accessors).
    friend class Gc;

public:
    static StorePtr open(BackendPtr backend, PoolConfig config);

    /// ---- write side ----
    BuildPtr startBuild(BuildInfo info);                          /// W-HEARTBEAT durable before return

    /// ---- read side (spec §6) ----
    std::optional<Resolved> resolveRef(const RootNamespace & ns, const String & ref_name);
    std::vector<TreeEntry> readTree(const TreeId & id);           /// validates envelope, kind, key↔hash
    BlobLocation locate(const TreeEntry & entry) const;           /// Blob/PackSlice placements only
    std::map<String, Resolved> listRefs(const RootNamespace & ns);

    /// ---- ref lifecycle (CAS loops on the owning shard) ----
    void dropRef(const RootNamespace & ns, const String & ref_name);            /// refs−− + '-' journal, atomic
    void updateRefPayload(const RootNamespace & ns, const String & ref_name,
                          std::function<void(RefPayload &)> mutator);           /// mutable fields only; NO journal
    void dropNamespace(const RootNamespace & ns);                 /// tombstone every shard + delete verbatim files

    /// ---- verbatim namespace files (format_version.txt, ...) — plain keys, never content-addressed ----
    void putNamespaceFile(const RootNamespace & ns, const String & name, const String & bytes);
    std::optional<String> getNamespaceFile(const RootNamespace & ns, const String & name);
    std::vector<String> listNamespaceFiles(const RootNamespace & ns);

    /// Internal surface for Build (same TU family; not for the wiring):
    const PoolConfig & poolConfig() const { return config; }
    const PoolMeta & poolMeta() const { return meta; }
    const Layout & layout() const { return pool_layout; }
    Backend & backend() { return *pool_backend; }
    RetireView & retireView() { return retire_view; }

private:
    Store(BackendPtr backend_, PoolConfig config_, PoolMeta meta_);

    uint64_t shardOf(const String & ref_name) const;             /// CityHash64(ref_name) % root_shards
    /// Read shard manifest (absent ⇒ empty RootShard with no token); used by resolve/list/drop.
    std::pair<RootShard, std::optional<Token>> readShard(const RootNamespace & ns, uint64_t shard);

    /// Read-modify-CAS one shard manifest under the manifest size guard. `mutate` edits the in-memory
    /// RootShard (which carries the freshly-read shard_version); the helper bumps shard_version, encodes,
    /// applies the manifest size guard (soft ⇒ LOG_WARNING, hard ⇒ LIMIT_EXCEEDED), and casPut against the
    /// observed token (nullopt when the shard was absent — create-if-absent). On Conflict it re-reads and
    /// retries the WHOLE mutate, bounded (100) then ABORTED ("manifest CAS contention on {}"). Single-writer
    /// shards make a real storm impossible; the bound is a runaway brake. `mutate` runs on the FRESHLY READ
    /// root each attempt, so a journal append is never double-applied across retries.
    /// `out_committed_version` (optional) receives the shard_version the successful casPut committed —
    /// the GC fence (R3) records it as the durable per-shard fence position (the model's fencePos[s]).
    void mutateShard(const RootNamespace & ns, uint64_t shard, std::function<void(RootShard &)> mutate,
                     uint64_t * out_committed_version = nullptr);

    BackendPtr pool_backend;
    PoolConfig config;
    PoolMeta meta;
    /// pool_layout MUST precede retire_view: the ctor init list builds retire_view from pool_layout.
    Layout pool_layout;
    RetireView retire_view;

    /// NOTE (M-C2): the manifest journal is never trimmed here — trimming needs folded_cursor
    /// (INV-JOURNAL-COVERAGE), which is GC state landing in M-C3; the manifest size guard
    /// (soft warn / hard throw, in the publish/drop CAS loop) bounds growth meanwhile.
};

}
