// Content-Addressed Shared MergeTree — proof-of-concept (standalone, no ClickHouse deps).
//
// Faithfully models the CORE of the v3 design:
//   * content-addressed blobs (column data) keyed by their content hash;
//   * a per-part content-addressed manifest (footer + inline small files);
//   * a catalog of refs (part_name -> manifest_hash), with the MergeTree
//     name + covering convention (ActivePartSet) deriving the active set;
//   * tombstone (empty covering) refs expressing DROP (removal supersession);
//   * mutation carry-forward by reference (unchanged columns reuse blob hashes);
//   * reachability GC over the union of live refs (+ detached/frozen/reader pins),
//     with grace measured from loss-of-reachability;
//   * ephemeral reader pins keeping a snapshot's blobs alive across "nodes".
//
// This is NOT integrated into ClickHouse; it validates the algorithms/data model.
// In real ClickHouse the hash would be cityHash128 (we use a deterministic 128-bit
// hash here) and the object store would be S3/IObjectStorage (we use a local dir).

#pragma once

#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <vector>

namespace cas
{

// ---------------------------------------------------------------------------
// 128-bit content hash (deterministic; production would use cityHash128).
// ---------------------------------------------------------------------------
struct Hash128
{
    uint64_t lo = 0;
    uint64_t hi = 0;
    bool operator==(const Hash128 & o) const { return lo == o.lo && hi == o.hi; }
    bool operator<(const Hash128 & o) const { return hi != o.hi ? hi < o.hi : lo < o.lo; }
    std::string hex() const;
};

Hash128 hash128(const std::string & bytes);

// ---------------------------------------------------------------------------
// Object store: dumb content-addressed key/value over an opaque namespace.
// Keys look like "blobs/<hex>", "manifests/<hex>", "refs/<name>", ...
// ---------------------------------------------------------------------------
class ObjectStore
{
public:
    virtual ~ObjectStore() = default;

    // Create-if-absent. Returns true if newly written, false if the key already
    // existed (idempotent — identical content addressing makes retries free).
    virtual bool putIfAbsent(const std::string & key, const std::string & data) = 0;

    // Overwrite/create unconditionally (used for mutable ref objects).
    virtual void put(const std::string & key, const std::string & data) = 0;

    virtual bool exists(const std::string & key) const = 0;
    virtual std::string get(const std::string & key) const = 0; // throws if missing
    virtual bool remove(const std::string & key) = 0;           // true if existed
    virtual std::vector<std::string> list(const std::string & prefix) const = 0;

    // Observable counters (for test assertions).
    struct Stats
    {
        size_t put_new = 0;     // objects actually written
        size_t put_skipped = 0; // idempotent putIfAbsent that found an existing key
        size_t get = 0;
        size_t removed = 0;
    };
    virtual const Stats & stats() const = 0;
    virtual size_t countUnder(const std::string & prefix) const = 0;
};

// Local-filesystem backed object store (the "bucket" is a directory).
std::unique_ptr<ObjectStore> makeLocalObjectStore(const std::string & root_dir);

// ---------------------------------------------------------------------------
// PartInfo — a faithful subset of MergeTreePartInfo, including the covering rule.
// Name format: <partition>_<min>_<max>_<level>[_<mutation>].
// Tombstone (DROP marker) parts carry no data and a sentinel max level.
// ---------------------------------------------------------------------------
struct PartInfo
{
    std::string partition;
    int64_t min_block = 0;
    int64_t max_block = 0;
    uint32_t level = 0;
    int64_t mutation = 0; // 0 == no mutation
    bool tombstone = false;

    static constexpr uint32_t TOMBSTONE_LEVEL = 0xFFFFFFFFu;

    static PartInfo parse(const std::string & name);
    std::string name() const;

    // Covering: does *this supersede `o` (and therefore make it inactive)?
    bool contains(const PartInfo & o) const;
    bool sameRangeIdentity(const PartInfo & o) const;
    bool operator==(const PartInfo & o) const;
};

// Make a DROP tombstone covering [min,max] of a partition.
PartInfo makeTombstone(const std::string & partition, int64_t min_block, int64_t max_block);

// ---------------------------------------------------------------------------
// A catalog ref: part_name -> manifest_hash (+ the part header, modeled as a
// columns hash for cross-replica divergence detection, kept like real CH).
// ---------------------------------------------------------------------------
struct Ref
{
    PartInfo info;
    std::string manifest_hash;  // empty for tombstone refs (no data)
    std::string columns_hash;   // header field (divergence detection); empty for tombstone

    std::string serialize() const;
    static Ref deserialize(const std::string &);
};

// ---------------------------------------------------------------------------
// Manifest — content-addressed part metadata.
//   blobs:  logical file name -> (blob hash, size)   [.bin / marks: separate blobs]
//   inline: logical file name -> raw bytes           [small service+index files]
// The manifest hash is over the canonical serialization (sorted), EXCLUDING the
// part identity (carried in the ref) so identical content dedups across names.
// ---------------------------------------------------------------------------
struct BlobRef
{
    std::string hash;
    uint64_t size = 0;
};

struct Manifest
{
    std::map<std::string, BlobRef> blobs;       // big/checksummed files
    std::map<std::string, std::string> inlined; // small files packed inline (one GET)

    std::string serialize() const;
    static Manifest deserialize(const std::string &);
    std::string contentHash() const;
};

// ---------------------------------------------------------------------------
// ActivePartSet — derive the active data parts from a set of refs via covering.
// (Mirrors ActiveDataPartSet: a data part is active iff no other ref contains it;
// tombstones cover but are not themselves "data".)
// ---------------------------------------------------------------------------
class ActivePartSet
{
public:
    void add(const PartInfo & info);
    // Active data parts (excludes tombstones and covered parts).
    std::vector<PartInfo> activeDataParts() const;
    // Data parts that are covered by some other ref (outdated; lifecycle-removable).
    std::vector<PartInfo> outdatedDataParts() const;
    // Tombstones that no longer cover any present data part (cleanup-able).
    std::vector<PartInfo> staleTombstones() const;
    bool isActive(const PartInfo & info) const;

private:
    std::vector<PartInfo> parts;
};

// ---------------------------------------------------------------------------
// Catalog — refs persisted in the object store. Namespaces:
//   refs/<name>       active/outdated data + tombstone refs
//   detached/<name>   detached parts (reachability root, not active)
//   frozen/<snap>/<name>  FREEZE snapshot roots
// ---------------------------------------------------------------------------
class Catalog
{
public:
    explicit Catalog(ObjectStore & store);

    void putRef(const std::string & ns, const Ref & ref);
    void removeRef(const std::string & ns, const std::string & name);
    std::optional<Ref> getRef(const std::string & ns, const std::string & name) const;
    std::vector<Ref> listRefs(const std::string & ns) const;

    // Active data parts in refs/ (covering applied).
    std::vector<Ref> activeDataRefs() const;
    std::vector<Ref> outdatedDataRefs() const;
    std::vector<Ref> staleTombstoneRefs() const;

    static constexpr const char * NS_REFS = "refs/";
    static constexpr const char * NS_DETACHED = "detached/";
    static constexpr const char * NS_FROZEN = "frozen/";

private:
    ObjectStore & store;
};

// ---------------------------------------------------------------------------
// Engine — the part lifecycle operations.
// ---------------------------------------------------------------------------
class Engine
{
public:
    explicit Engine(ObjectStore & store);

    // INSERT: `files` = logical file name -> bytes. Files whose name matches a
    // blob extension (.bin/.mrk/.cmrk) become content-addressed blobs; the rest
    // are packed inline in the manifest. Uploads blobs+manifest, creates the ref.
    Ref insertPart(const PartInfo & info, const std::map<std::string, std::string> & files);

    // MERGE: caller supplies the merged part's files; new ref covers `sources`.
    Ref mergeParts(const PartInfo & new_info,
                   const std::vector<std::string> & source_names,
                   const std::map<std::string, std::string> & merged_files);

    // MUTATE with carry-forward: unchanged blob files reuse the source's blob
    // hashes (NO re-upload), changed/added files are uploaded fresh, dropped
    // files are removed. Demonstrates dedup-by-reference.
    Ref mutatePart(const PartInfo & new_info,
                   const std::string & source_name,
                   const std::map<std::string, std::string> & changed_files,
                   const std::set<std::string> & dropped_files = {});

    // DROP a [min,max] range: create a persisted tombstone covering ref.
    Ref dropRange(const std::string & partition, int64_t min_block, int64_t max_block);

    void detachPart(const std::string & name);
    void attachPart(const std::string & name);

    // FREEZE: materialize real bytes into a snapshot namespace (NOT a reference) —
    // models the v3 fix that FREEZE must copy bytes to keep filesystem backups working.
    void freezePart(const std::string & snapshot, const std::string & name);

    // READ: resolve name -> manifest -> reconstruct logical files (verifies blobs).
    std::map<std::string, std::string> readPart(const std::string & name) const;

    // Lifecycle: remove refs of covered (outdated) data parts older than
    // `parts_lifetime`, and stale tombstones — UNLESS reader-pinned. Models
    // grabOldParts / clearOldPartsAndRemoveFromZK. `now` is a logical clock.
    struct LifecycleResult { size_t refs_removed = 0; size_t tombstones_removed = 0; };
    LifecycleResult removeOutdatedRefs(int64_t now, int64_t parts_lifetime);

    Catalog & catalog() { return cat; }
    ObjectStore & objectStore() { return store; }

    // --- ephemeral reader pins (stateless-compute reader fence) ---
    // A reader pins the manifest hashes of its query snapshot. Auto-released by
    // unpin() / dropped on "session" loss. Included in the GC mark union.
    void pinSnapshot(const std::string & session, const std::vector<std::string> & part_names);
    void unpin(const std::string & session);
    std::set<std::string> pinnedManifestHashes() const;

    // record the logical time a ref was first marked outdated (for lifecycle delay)
    void markOutdatedObserved(const std::string & name, int64_t now);

private:
    ObjectStore & store;
    Catalog cat;
    std::map<std::string, std::set<std::string>> reader_pins; // session -> manifest hashes
    std::map<std::string, int64_t> outdated_since;            // part name -> first-outdated time

    static bool isBlobFile(const std::string & logical);
    std::optional<std::string> resolveManifestHash(const std::string & name) const; // searches refs/, detached/
};

// ---------------------------------------------------------------------------
// GC — reachability mark-and-sweep over blobs/ and manifests/.
//   reachable = manifests of (active+outdated-but-present refs) + detached + frozen
//               + reader-pinned manifest hashes; and the blobs those manifests list.
//   delete an object iff unreachable AND first-unreachable older than `grace`.
// `unreachable_since` is keyed on loss-of-reachability, not object age.
// ---------------------------------------------------------------------------
class GC
{
public:
    GC(ObjectStore & store, Engine & engine);

    struct Result
    {
        size_t blobs_deleted = 0;
        size_t manifests_deleted = 0;
        size_t blobs_kept = 0;
        size_t manifests_kept = 0;
        size_t blobs_aging = 0;     // unreachable but within grace
        size_t manifests_aging = 0;
    };

    Result run(int64_t now, int64_t grace);

    // Compute the reachable set of object keys (manifests/ and blobs/).
    std::set<std::string> markReachable() const;

private:
    ObjectStore & store;
    Engine & engine;
    std::map<std::string, int64_t> first_unreachable; // object key -> logical time
};

} // namespace cas
