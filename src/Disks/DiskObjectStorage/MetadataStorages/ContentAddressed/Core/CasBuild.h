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
    /// (observe current token; condemned in the retire view ⇒ resurrect; else adopt — free).
    BlobRef putBlob(const BlobId & id, BlobSource source);

    /// Cold reuse without bytes: HEAD; condemned ⇒ resurrect (GET + fresh-tag header rewrite +
    /// putOverwrite If-Match). On HEAD-absent the outcome depends on `body_recreatable` (B156b):
    ///   true  — the blob was putBlob'd by a build in this txn (recreatable by retrying) ⇒ the
    ///           absence is the benign dedup-vs-GC race ⇒ retryable ABORTED (caller retries, re-uploads);
    ///   false — the blob was adopted from a committed source (tokenless W-EVIDENCE, NOT recreatable) ⇒
    ///           the absence is a real INV-NO-LOSS loss ⇒ fail-loud FILE_DOESNT_EXIST (never masked).
    /// Use depIsTokened on the SOURCE build to pick the flag (tokened == putBlob'd == recreatable).
    /// B188: the ContentAddressedTransaction adopt sites no longer call reuseBlob for committed-source
    /// entries — they use adoptEvidence + the post-precommit gate; reuseBlob remains for direct
    /// callers/tests.
    BlobRef reuseBlob(const BlobId & id, bool body_recreatable);

    /// B156b discriminator: does this build hold a TOKENED Blob dep for `hash` (putBlob'd here ⇒
    /// recreatable) versus a TOKENLESS W-EVIDENCE dep (adoptFromTree ⇒ not recreatable)? False also
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
    /// tree is assembled and BEFORE any adopted/dedup'd source ref could be dropped. STUB here
    /// (Task 1 RED): records nothing yet so the dangle still reproduces. Implemented in Task 2.
    void precommit(const TreeId & manifest);

    /// The publish gate — Tasks 12/13. (Stub here.)
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

    /// The §5 step-2 cold-reuse rule, shared by putBlob's PreconditionFailed branch and reuseBlob:
    /// HEAD the key; absent ⇒ FILE_DOESNT_EXIST; condemned-at-current-token ⇒ resurrect; else record
    /// the current token as the dep. Returns the admitted size.
    uint64_t observeAndAdmit(ObjectKind kind, const UInt128 & hash, const String & key);
    /// Overload for callers that already hold a fresh, present HeadResult for `key` (the putBlob
    /// HEAD-before-PUT path), avoiding a redundant second HEAD. `hr.exists` MUST be true. (B168 P1/P2)
    uint64_t observeAndAdmit(ObjectKind kind, const UInt128 & hash, const String & key, const HeadResult & hr);
    /// Resurrect: GET whole object, rebuild header with a FRESH incarnation_tag preserving header_len
    /// and all other TLVs, putOverwrite(If-Match observed token). Records the new token as the dep.
    void resurrect(ObjectKind kind, const UInt128 & hash, const String & key);

    /// The publish gate's W-EVIDENCE + condemned-token scan (spec §5). For each dependency: a
    /// token-bearing dep condemned at its token ⇒ resurrect; a tokenless (W-EVIDENCE) dep whose
    /// (kind, hash) has any condemned token ⇒ observeAndAdmit (adopt-or-resurrect the HEAD). With an
    /// empty retire view this is a no-op. Runs INSIDE the publish mutate lambda, so it re-runs on
    /// every CAS retry — idempotent (re-observe). Task 13 extends it with revalidateDeps.
    void gateCheckDeps();

    /// W-REVALIDATE (the model's `WPublishReval`). Called once, immediately after a fence-advanced
    /// `retireView().refresh` in the publish lambda and BEFORE gateCheckDeps. A token observation is
    /// valid only relative to the retire view at which it was made (finding F1): retire entries drop
    /// on confirmed outcomes, so the refreshed view alone cannot condemn a STALE observation — the
    /// durable witness is the object itself. For every token-bearing member whose hash has NO entry
    /// in the refreshed view yet was observed under an older round, this re-observes (one HEAD) and
    /// keeps / adopts / re-creates per the rule. See spec §7 step 4 (the no-return argument's
    /// publish-after-fence branch).
    void revalidateDeps();

    /// W-REVALIDATE re-create branch for trees: re-upload retained_trees[hash] as a FRESH Tree
    /// incarnation (fresh incarnation_tag, putIfAbsentStream If-None-Match), then record the new
    /// token + observed_view_round. On PreconditionFailed (someone re-created it concurrently) ⇒
    /// observeAndAdmit. Trees are ALWAYS re-creatable because their encoded payload is retained;
    /// blob payloads are not retained (hence the ABORTED branch in revalidateDeps for a lost blob).
    void recreateTree(const UInt128 & hash);

    /// Map (kind, hash) to its object key per kind (blob/tree/pack).
    String keyFor(ObjectKind kind, const UInt128 & hash) const;

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
