#pragma once
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasStore.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasManifestId.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasManifestCodec.h>
#include <functional>
#include <map>
#include <optional>
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

/// The writer protocol (spec §5, CA GC root-local part-manifest redesign rev. 15) — the W-rules live
/// HERE, not in the wiring. One Build per written ref. Thread-compat: a Build is used by ONE thread (the
/// wiring's commit path); the Store-shared RetireView it consults is itself thread-safe.
///
/// Write path: stageManifest (mint a ManifestId, stream-write the body) -> precommitAdd (append a
/// create-precommit RootOwnerEvent in the target root shard) -> putBlob (blob bodies) -> promote
/// (atomic single-shard owner move precommit->committed, fail-closed revalidation). Only blobs stay
/// content-addressed; a part is one immutable single-owner ManifestId.
class Build
{
public:
    Build(StorePtr store_, UInt128 build_id_,
          uint64_t build_seq_, uint64_t epoch_, BuildInfo info_);
    ~Build();

    /// W-FRESH-TAG: every upload attempt mints a fresh random incarnation_tag.
    /// New content: streaming PUT If-None-Match:*; on PreconditionFailed ⇒ the cold-reuse rule
    /// (observe current token; condemned ⇒ uploadFromSource — re-upload from the writer's source
    /// bytes; else adopt — free).
    BlobRef putBlob(const BlobId & id, BlobSource source);

    /// B156b discriminator: does this build hold a TOKENED Blob dep for `hash` (putBlob'd here ⇒
    /// tokened) versus a TOKENLESS W-EVIDENCE dep (adoptEvidence ⇒ tokenless)? False also when this
    /// build has no dep for the hash at all.
    bool depIsTokened(const UInt128 & hash) const;

    /// B188: record a TOKENLESS W-EVIDENCE blob dep directly from a ManifestEntry — NO HEAD, no backend
    /// call. Lets staging adopt sites record the dep by hash without asserting presence before
    /// precommit; the promote gate observes/resurrects it post-precommit. Inline entries record nothing.
    void adoptEvidence(const ManifestEntry & entry);

    /// B188: record a TOKENLESS Blob dep by hash (no HEAD) for a blob whose bytes are staged locally and
    /// will be putBlob'd post-precommit. putBlob later overwrites it with the tokened dep on upload.
    void recordPendingBlobDep(const UInt128 & hash, uint64_t size);

    /// Mint a root-local part ManifestId, stream-write its body under
    /// `cas/manifests/<ns>/<writer_epoch>/<build_sequence>/000001.proto` via putIfAbsentStream (NO preliminary HEAD —
    /// the manifest_ordinal is per-build monotone). Enforces the OQ7 caps fail-closed BEFORE the body write
    /// returns (and therefore before any owner transition is published). The body is not retained after a
    /// successful write; on retry the caller re-stages from source. NoManifestIdReuse: a fresh random
    /// manifest_ordinal per call. The id is recorded for best-effort `abandon` cleanup.
    ManifestId stageManifest(std::vector<ManifestEntry> entries);

    /// Build-intent owner add, written to the SAME root shard as the future committed ref (spec §Precommit
    /// Add) — there is no `_precommits` namespace. ONE root-shard CAS appending a RootOwnerEvent
    /// {old=none, new={Precommit, final_ref_name, build_id, id.ref}} to the single ordered journal; shard =
    /// store->shardOf(final_ref_name), so the later promote is an atomic owner move in this same shard.
    /// Needs NO body-exists HEAD as a safety authority: GC and promotion handle a missing precommit
    /// manifest body by failing closed (a missing-body precommit is a non-activating, non-promotable intent).
    void precommitAdd(const RootNamespace & target_ns, const String & final_ref_name, const ManifestId & id);

    /// Atomic commit promotion (spec §Promote Precommit): ONE root-shard CAS in shardOf(final_ref_name).
    ///  1. (mutateShard refreshes the retire view if the shard fence demands it)
    ///  2. stream-read the precommit manifest body; validate RefMatchesBody / ManifestNamespaceMatches;
    ///  3. revalidate EVERY blob leaf listed in the manifest (fail-closed);
    ///  4. body absent | a blob absent | a blob condemned-and-not-recreatable ⇒ ABORTED;
    ///  5. atomically replace precommit(build_id) owner with committed(final_ref_name) owner by appending
    ///     ONE pure-move RootOwnerEvent (old={Precommit,final_ref_name,build_id,T}, new={Committed,final_ref_name,T},
    ///     same manifest_ref T) and setting refs[final_ref_name];
    ///  6. promotion NEVER emits blob deltas (spec rev. 15 §Promote Precommit). A missing-body precommit
    ///     is non-activating and was rejected at step 4 (the writer re-stages with a fresh ManifestId).
    void promote(const RootNamespace & target_ns, const String & final_ref_name, UInt128 build_id, const ManifestId & id);

    /// Carry the mutable per-ref payload (txn_version.txt, ...) that promote writes into RootRef.mutable_files.
    void setPendingMutableFiles(std::map<String, String> files) { pending_mutable_files = std::move(files); }

    /// Retire seq so the GC watermark floor can advance; staged manifest debris is best-effort cleaned
    /// (the Phase-1d orphan sweep is the durable backstop).
    void abandon();

    UInt128 buildId() const { return build_id; }
    /// The strictly-increasing per-process build_seq (spec 2026-06-16).
    uint64_t buildSeq() const { return build_seq; }

private:
    struct DepEntry
    {
        ObjectKind kind = ObjectKind::Blob;
        std::optional<Token> token;                       /// nullopt = live-root evidence (W-EVIDENCE)
        uint64_t size = 0;
    };
    using DepKey = std::pair<uint8_t, UInt128>;           /// (kind, hash)

    /// The §5 step-2 cold-reuse rule: HEAD the key; absent ⇒ FILE_DOESNT_EXIST;
    /// condemned-at-current-token ⇒ throw ABORTED (caller must re-upload from its own source bytes);
    /// else record the current token as the dep. Returns the admitted size.
    uint64_t observeAndAdmit(ObjectKind kind, const UInt128 & hash, const String & key);
    /// Overload for callers that already hold a fresh, present HeadResult for `key` (the putBlob
    /// HEAD-before-PUT path), avoiding a redundant second HEAD. `hr.exists` MUST be true.
    uint64_t observeAndAdmit(ObjectKind kind, const UInt128 & hash, const String & key, const HeadResult & hr);
    /// INV-1 (revival-from-source): revive a condemned or absent object by re-uploading from the writer's
    /// OWN re-readable source without reading the dying object (no backend().get). The source is STREAMED
    /// into the put sink (header + `source.write_payload`), never materialized into a full in-memory copy;
    /// `source.write_payload` may be re-invoked on each conditional-write attempt (it re-reads the staged
    /// temp file / re-emits the captured String), so it is taken by const ref and not consumed.
    void uploadFromSource(ObjectKind kind, const UInt128 & hash, const String & key, const BlobSource & source);

    /// Verified copy-forward (spec 2026-07-02-cas-copy-forward-condemned-evidence.md): the narrow,
    /// deliberate exception to INV-1's "never read the dying object" — allowed ONLY for tokenless
    /// W-EVIDENCE deps (`adoptEvidence`: every call site adopts from a COMMITTED source manifest, so
    /// the blob is referenced by a live committed owner and this is a reference transfer, not a
    /// resurrection). Reads the condemned-but-present incarnation IN FULL, verifies fail-closed
    /// (envelope decodes + recomputed payload hash == `hash`), re-wraps under a fresh envelope
    /// (fresh incarnation_tag, this build's build_id — W-FRESH-TAG), and displaces EXACTLY the
    /// observed incarnation via token-conditional putOverwrite. Every failure mode (absent, corrupt,
    /// lost delete race) throws ABORTED — never a blind PUT, never putIfAbsent after a lost race.
    /// Returns the fresh (or adopted-clean) token. O(blob) resident memory — recovery path only.
    Token copyForwardFromCondemned(const UInt128 & hash, const String & key, HeadResult hr);

    /// The build's owning root namespace, derived from BuildInfo::intended_ref ("ns/ref" — the ref is the
    /// last `/`-segment; the namespace is everything before it). Sets a manifest body's root_namespace_id.
    RootNamespace manifestNamespace() const;

    /// Map (kind, hash) to its object key per kind (blob/tree).
    String keyFor(ObjectKind kind, const UInt128 & hash) const;

    void requireAlive() const;                            /// throws LOGICAL_ERROR after abandon

    /// A leaf is copy-forwardable iff this build holds a TOKENLESS W-EVIDENCE Blob dep for `hash`
    /// (adoptEvidence — in production always sourced from a committed manifest, which is the copy-forward
    /// exception's PROVENANCE; it is NOT a checked runtime guarantee here). The enforced runtime safety
    /// rests on the caller's owner-liveness check + fold barrier (see the backstop in `promote`), exactly
    /// as the tokened `uploadFromSource` resurrect does. A tokened dep, or NO dep at all (a staging bug —
    /// must fail closed), is NOT copy-forwardable. Single source of truth for the pre-pass and the backstop.
    bool isCopyForwardableTokenless(const UInt128 & hash) const;

    StorePtr store;
    UInt128 build_id{};
    uint64_t build_seq{};                                 /// per-process monotone seq (spec 2026-06-16)
    uint64_t epoch{};                                     /// owning Store's process_epoch
    uint32_t next_manifest_ordinal = 1;                   /// per-build monotone manifest ordinal
    BuildInfo info;
    bool alive = true;
    bool precommitted = false;                            /// a create-precommit RootOwnerEvent was appended

    /// Set by precommitAdd so promote knows which shard to move ownership in.
    RootNamespace precommit_target_ns;
    String precommit_final_ref;
    ManifestRef precommit_manifest;

    std::map<String, String> pending_mutable_files;       /// promote writes these into RootRef.mutable_files
    std::vector<ManifestId> staged_manifests;             /// for best-effort abandon cleanup

    std::map<DepKey, DepEntry> deps;                      /// the W-DEP-SET (blob-only now)
};

}
