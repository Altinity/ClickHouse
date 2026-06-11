#pragma once
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasStore.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasHeartbeat.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasTreeCodec.h>
#include <functional>
#include <map>

namespace DB { class WriteBuffer; }

namespace DB::Cas
{

struct BlobSource
{
    uint64_t size = 0;
    std::function<void(WriteBuffer &)> write_payload;   /// must write exactly `size` bytes
    static BlobSource fromString(String bytes);         /// convenience for small content/tests
};

struct BlobRef
{
    BlobId id;
    uint64_t size = 0;
};

/// The writer protocol (spec §5) — the W-rules live HERE, not in the wiring. One Build per written
/// part. Thread-compat: a Build is used by ONE thread (the wiring's commit path); the Store-shared
/// RetireView it consults is itself thread-safe.
class Build
{
public:
    Build(StorePtr store_, std::unique_ptr<HeartbeatKeeper> heartbeat_, UInt128 build_id_, BuildInfo info_);
    ~Build();

    /// W-FRESH-TAG: every upload attempt mints a fresh random incarnation_tag.
    /// New content: streaming PUT If-None-Match:*; on PreconditionFailed ⇒ the cold-reuse rule
    /// (observe current token; condemned in the retire view ⇒ resurrect; else adopt — free).
    BlobRef putBlob(const BlobId & id, BlobSource source);

    /// Cold reuse without bytes: HEAD; absent ⇒ FILE_DOESNT_EXIST (caller must putBlob);
    /// condemned ⇒ resurrect (GET + fresh-tag header rewrite + putOverwrite If-Match).
    BlobRef reuseBlob(const BlobId & id);

    /// Carry-forward (W-EVIDENCE): records a TOKENLESS dependency (liveness evidence = the live
    /// source root). Inline entries record nothing. Returns the entry found by name in the source tree.
    TreeEntry adoptFromTree(const TreeId & source, const String & name);

    /// Whole-tree adoption (FREEZE / detached re-attach / replication relink): tokenless evidence on
    /// the tree root; the closure is covered by the publish gate + fence/recheck handshake.
    void adoptTree(const TreeId & id);

    /// W-TREE-BUILD: every child referenced by `entries` must already be in the dependency set
    /// (LOGICAL_ERROR otherwise). Encodes canonically, uploads like a blob (natural header length),
    /// and RETAINS the encoded payload (trees are always re-creatable during the gate's W-REVALIDATE;
    /// blob payloads are not retained).
    TreeId putTree(std::vector<TreeEntry> entries);

    /// The publish gate — Tasks 12/13. (Stub here.)
    void publish(const RootNamespace & ns, const String & ref_name, const TreeId & tree, RefPayload payload);

    /// Stop renewals and delete own heartbeat; uploads become debris (heartbeat-gated full-GC reclaim).
    void abandon();

    UInt128 buildId() const { return build_id; }
    void renewHeartbeat();                                /// test hook == HeartbeatKeeper::renewOnce

private:
    struct DepEntry
    {
        ObjectKind kind = ObjectKind::Blob;
        std::optional<Token> token;                       /// nullopt = live-root evidence (W-EVIDENCE)
        uint64_t observed_view_round = 0;                 /// W-REVALIDATE bookkeeping (Task 13)
        uint64_t size = 0;
    };
    using DepKey = std::pair<uint8_t, UInt128>;           /// (kind, hash)

    /// The §5 step-2 cold-reuse rule, shared by putBlob's PreconditionFailed branch and reuseBlob:
    /// HEAD the key; absent ⇒ FILE_DOESNT_EXIST; condemned-at-current-token ⇒ resurrect; else record
    /// the current token as the dep. Returns the admitted size.
    uint64_t observeAndAdmit(ObjectKind kind, const UInt128 & hash, const String & key);
    /// Resurrect: GET whole object, rebuild header with a FRESH incarnation_tag preserving header_len
    /// and all other TLVs, putOverwrite(If-Match observed token). Records the new token as the dep.
    void resurrect(ObjectKind kind, const UInt128 & hash, const String & key);

    void requireAlive() const;                            /// throws LOGICAL_ERROR after abandon

    StorePtr store;
    std::unique_ptr<HeartbeatKeeper> heartbeat;
    UInt128 build_id{};
    BuildInfo info;
    bool alive = true;

    std::map<DepKey, DepEntry> deps;                      /// the W-DEP-SET
    std::map<UInt128, String> retained_trees;             /// encoded tree payloads for gate re-create (Task 13)
    std::map<TreeId, std::vector<TreeEntry>> source_tree_cache;   /// adoptFromTree memoization
};

}
