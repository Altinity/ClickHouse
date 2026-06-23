#pragma once
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasStore.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasHeartbeat.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasTreeCodec.h>
#include <functional>
#include <map>
#include <set>
#include <vector>

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
/// ref. Thread-compat: a Build is used by ONE thread (the wiring's commit path); the Store-shared
/// RetireView it consults is itself thread-safe.
class Build
{
public:
    Build(StorePtr store_, std::unique_ptr<HeartbeatKeeper> heartbeat_, UInt128 build_id_,
          uint64_t build_seq_, uint64_t epoch_, BuildInfo info_);
    ~Build();

    /// W-FRESH-TAG: every upload attempt mints a fresh random incarnation_tag.
    /// New content: streaming PUT If-None-Match:*; on PreconditionFailed ⇒ the cold-reuse rule
    /// (observe current token; condemned ⇒ uploadFromSource — re-upload from the writer's source
    /// bytes; else adopt — free).
    BlobRef putBlob(const BlobId & id, BlobSource source);

    /// B156b discriminator: does this build hold a TOKENED Blob dep for `hash` (putBlob'd here ⇒
    /// tokened) versus a TOKENLESS W-EVIDENCE dep (adoptFromTree / adoptEvidence ⇒ tokenless)? False also
    /// when this build has no dep for the hash at all.
    bool depIsTokened(const UInt128 & hash) const;
    /// Whether this build holds ANY Blob dep (tokened or tokenless) for `hash`.
    bool hasDep(const UInt128 & hash) const;

    /// Carry-forward (W-EVIDENCE): records a TOKENLESS dependency (liveness evidence = the live
    /// source root). Inline entries record nothing. Returns the entry found by name in the source tree.
    TreeEntry adoptFromTree(const TreeId & source, const String & name);

    /// B188: record a TOKENLESS W-EVIDENCE dep directly from an already-resolved TreeEntry — NO HEAD,
    /// no backend call. Lets the staging adopt sites record the dep by hash without asserting presence
    /// before precommit; the publish gate observes/resurrects it post-precommit. Inline entries record
    /// nothing. adoptFromTree delegates here.
    void adoptEvidence(const TreeEntry & entry);

    /// B188: record a TOKENLESS Blob dep by hash (no HEAD) for a blob whose bytes are staged locally
    /// and will be putBlob'd post-precommit. putBlob later overwrites it with the tokened dep on
    /// upload. Lets stageTree's W-TREE-BUILD check pass without any pool op at staging time.
    void recordPendingBlobDep(const UInt128 & hash, uint64_t size);

    /// Whole-tree adoption (FREEZE / detached re-attach / replication relink): tokenless evidence on
    /// the tree root; the closure is covered by the publish gate + fence/recheck handshake.
    void adoptTree(const TreeId & id);

    /// Encode + hash the tree, retain its payload, record a TOKENLESS Tree dep — LOCAL ONLY, no
    /// upload. precommit needs the dep (it tolerates an absent tree object); uploadStagedTree uploads
    /// the object post-precommit. W-TREE-BUILD: every child must already be in the dep set.
    TreeId stageTree(std::vector<TreeEntry> entries);
    /// Upload a previously-staged tree object (putIfAbsentStream from the retained payload; on
    /// PreconditionFailed -> observeAndAdmit). Records the TOKENED dep. Runs after precommit.
    void uploadStagedTree(const TreeId & id);
    /// Convenience = stageTree + uploadStagedTree (callers/tests that do not precommit-first).
    TreeId putTree(std::vector<TreeEntry> entries);

    /// B171 two-phase commit, phase 1: publish the build's manifest tree under the precommit
    /// namespace so GC's fold lifts the in-degree of every reachable object (protection by
    /// reachability, replacing the revocable `cas_owner` hint). Must be called after the manifest
    /// tree is assembled and BEFORE any adopted/dedup'd source ref could be dropped. Implemented
    /// as the B171 precommit-first protocol: the build root ref is written to the precommit
    /// namespace via a durable CAS PUT before any blob/tree pool operations run post-staging.
    void precommit(const TreeId & manifest);

    /// The publish gate: runs `checkAndResolveDeps` (merged W-REVALIDATE + W-EVIDENCE +
    /// condemned-token scan) then writes the final ref under `ns`/`ref_name`.
    void publish(const RootNamespace & ns, const String & ref_name, const TreeId & tree, RefPayload payload);

    /// Stop renewals and delete own heartbeat; uploads become debris (heartbeat-gated full-GC reclaim).
    void abandon();

    UInt128 buildId() const { return build_id; }
    /// The strictly-increasing per-process build_seq (spec 2026-06-16). Stamped into object owner
    /// metadata (Task 8) and the GC watermark floor (minActive) tracks it across in-flight builds.
    uint64_t buildSeq() const { return build_seq; }
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

    /// The §5 step-2 cold-reuse rule: HEAD the key; absent ⇒ FILE_DOESNT_EXIST;
    /// condemned-at-current-token ⇒ throw ABORTED (caller must re-upload from its own source bytes);
    /// else record the current token as the dep. Returns the admitted size.
    uint64_t observeAndAdmit(ObjectKind kind, const UInt128 & hash, const String & key);
    /// Overload for callers that already hold a fresh, present HeadResult for `key` (the putBlob
    /// HEAD-before-PUT path), avoiding a redundant second HEAD. `hr.exists` MUST be true. (B168 P1/P2)
    uint64_t observeAndAdmit(ObjectKind kind, const UInt128 & hash, const String & key, const HeadResult & hr);
    /// INV-1 (revival-from-source): revive a condemned or absent object by re-uploading from source_bytes
    /// without reading the dying object (no backend().get). Builds a FRESH envelope header (new
    /// incarnation_tag, this build's build_id), streams header + source_bytes via putIfAbsentStream.
    /// On Done: records the tokened dep. On PreconditionFailed (live incarnation): calls observeAndAdmit
    /// (adopt the live token, or throw ABORTED if still condemned — caller retries with source bytes).
    void uploadFromSource(ObjectKind kind, const UInt128 & hash, const String & key, std::string_view source_bytes);

    /// Merged publish-gate pass (B190 Task 3): W-REVALIDATE + W-EVIDENCE + condemned-token scan in a
    /// single loop over `deps`. Replaces the former separate `revalidateDeps` + `gateCheckDeps` calls.
    ///
    /// Per dep, in priority order:
    ///
    /// Tokenless (W-EVIDENCE) dep:
    ///   • Any view hit by hash → observeAndAdmit (adopt the live HEAD; condemned → ABORTED, INV-1).
    ///     This covers both fresh and stale tokenless deps: the hit is unconditional on staleness.
    ///   • No hit + stale (observed_view_round < current round) → HEAD:
    ///       absent  → tree with retained: recreate; else ABORTED.
    ///       present → observeAndAdmit(4-arg, pass already-fetched hr — no redundant second HEAD).
    ///   • No hit + fresh → keep.
    ///
    /// Token-bearing dep:
    ///   • Any view hit by hash:
    ///       own token condemned (Case a) → tree with retained: recreate; else ABORTED (INV-1).
    ///       own token live (Case b, phantom) → observeAndAdmit (HEAD-only, adopt current token).
    ///   • No hit + stale → HEAD:
    ///       absent  → tree with retained: recreate; else ABORTED.
    ///       same token as dep → re-stamp observed_view_round (IN-FLIGHT DISJUNCTION keep-branch).
    ///       different token  → observeAndAdmit(4-arg, pass already-fetched hr — no second HEAD).
    ///   • No hit + fresh → keep.
    ///
    /// Iterating `deps` while uploadFromSource/observeAndAdmit/recreateTree mutate deps is safe:
    /// all three overwrite the SAME (kind, hash) entry (never insert a new key), so the map structure
    /// and the current iterator stay valid throughout.
    ///
    /// Runs INSIDE the publish mutateShard lambda — re-runs on every CAS retry (idempotent).
    void checkAndResolveDeps();

    /// W-REVALIDATE re-create branch for trees: re-upload retained_trees[hash] as a FRESH Tree
    /// incarnation (fresh incarnation_tag, putIfAbsentStream If-None-Match), then record the new
    /// token + observed_view_round. On PreconditionFailed (someone re-created it concurrently) ⇒
    /// observeAndAdmit. Trees are ALWAYS re-creatable because their encoded payload is retained;
    /// blob payloads are not retained (hence the ABORTED branch in checkAndResolveDeps for a lost blob).
    void recreateTree(const UInt128 & hash);

    /// Map (kind, hash) to its object key per kind (blob/tree/pack).
    String keyFor(ObjectKind kind, const UInt128 & hash) const;

    /// B199-S2 Task 4: build the inline closure of a staged manifest tree from the in-memory retained
    /// payloads — never reads from the pool/backend. Returns a vector of ClosureNodes, one per tree
    /// that this build staged (i.e. the hash is in retained_trees). Adopted subtrees (Subtree entries
    /// whose hashes are NOT in retained_trees) are recorded only as entries in their parent's node
    /// (leaf stop — we must NOT reclaim someone else's subtree). Dedup: each tree hash appears at most
    /// once across the result.
    std::vector<ClosureNode> buildStagedClosure(UInt128 root_hash) const;

    /// B171 precommit addressing (fixed 2026-06-19; relocated Phase 6). The precommit namespace is
    /// `<server-hex>/_precommits`, sharded EXACTLY like a table namespace: the precommit ref name is
    /// `build_seq` and the shard is `shardOf(build_seq)`. So the precommit namespace has at most
    /// root_shards shards (bounded), each holding many builds' precommit refs keyed by build_seq. GC
    /// derives the owning build's `build_seq` from the REF NAME (and the server from the namespace) to
    /// decide precommit-reclaim liveness.
    RootNamespace precommitNs() const;
    String buildRef() const;        /// the precommit ref name == std::to_string(build_seq)
    uint64_t buildShard() const;    /// == store->shardOf(buildRef())

    void requireAlive() const;                            /// throws LOGICAL_ERROR after abandon

    StorePtr store;
    std::unique_ptr<HeartbeatKeeper> heartbeat;
    UInt128 build_id{};
    uint64_t build_seq{};                                 /// per-process monotone seq (spec 2026-06-16)
    uint64_t epoch{};                                     /// owning Store's process_epoch (stamped in Task 8)
    BuildInfo info;
    bool alive = true;
    bool precommitted = false;                            /// B171: a precommit edge was published (remove on commit)

    std::map<DepKey, DepEntry> deps;                      /// the W-DEP-SET
    std::map<UInt128, String> retained_trees;             /// encoded tree payloads for gate re-create (Task 13)
    std::map<TreeId, std::vector<TreeEntry>> source_tree_cache;   /// adoptFromTree memoization
};

}
