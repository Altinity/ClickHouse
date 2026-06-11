#pragma once
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasGcFormats.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasGcOutcomes.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasGcSnap.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasStore.h>
#include <map>
#include <utility>
#include <vector>

namespace DB::Cas
{

/// What one runRegularRound did (counters are health metrics, not protocol state).
struct RoundReport
{
    bool acquired_lease = false;  /// false => another leader is alive; nothing else was done
    uint64_t round = 0;
    uint64_t candidates = 0;
    uint64_t deleted = 0;
    uint64_t absent = 0;
    uint64_t replaced = 0;        /// 412-saves - a health metric (spec §7)
    uint64_t spared = 0;
    uint64_t cascaded = 0;        /// children freed by the cascade this round
};

/// Leader-paced regular GC (spec §7): fold -> retire -> fence -> recheck -> exact-token delete ->
/// cascade -> trim. The lease is WORK DEDUP ONLY - every step is idempotent and split-brain-safe
/// (monotone gc/state, append-by-unique-path retire/outcome logs, exact-token deletes); the TLA+
/// model (CaIncarnationCore.tla) proves the round safe with NO leadership assumption at all. A
/// stale leader can only duplicate work, never roll back state or mis-delete.
///
/// LEASE / STEAL WINDOW (deterministic - this class NEVER reads a clock). The lease lives inside
/// gc/state as {owner, seq} and moves only by CAS on the whole gc/state object. Each Gc instance
/// remembers the last (owner, seq) it OBSERVED on gc/state. On runRegularRound:
///
///   1. Read gc/state. Absent => fresh GcState with our lease = {gc_id, 1}, create-if-absent CAS.
///      Committed => we lead. Conflict => a racer created it first => re-read, fall through.
///   2. state.lease.owner == gc_id => RENEW: lease.seq++, CAS against the observed token.
///      Committed => we lead. Conflict => someone moved the lease (a steal happened) => re-read
///      once; if the owner is still us, retry the renew once; else not-acquired.
///   3. Foreign owner whose (owner, seq) DIFFERS from our remembered observation (or we have no
///      prior observation) => the incumbent is ALIVE (it renewed since our last look) => record
///      the new observation, return not-acquired (back off).
///   4. Foreign owner whose (owner, seq) is IDENTICAL to our remembered observation => the
///      incumbent did NOT renew across our full observation window - one whole prior
///      runRegularRound attempt of OURS, "the contender's own waiting window" (spec §8) => STEAL:
///      lease.owner = gc_id; lease.seq++; fence_seq++ - a new leadership epoch, so the new
///      leader's retire/outcome paths (<round>.<fence_seq>) never collide with the old leader's
///      (spec §4 append-by-unique-path). CAS against the observed token. Committed => we lead.
///      Conflict => a racer stole first => re-read, record the observation, not-acquired.
///
/// A lost CAS NEVER yields leadership within the same attempt beyond the single bounded renew
/// retry above (max 2 CAS attempts per call) - a contender that loses a steal race backs off and
/// re-enters the observation protocol from the freshly read state.
///
/// PRODUCTION PACING: the wiring's GC scheduler thread calls runRegularRound on a timer and
/// sleeps between calls. "Observed non-renewal across the contender's window" therefore means
/// the incumbent failed to renew for one full scheduler period - wall-clock enters ONLY through
/// the caller's pacing, never through this class (so unit tests drive the window by simply
/// calling runRegularRound, with no sleeps and no clock).
///
/// NOT thread-safe: one pacing thread drives a Gc instance. gc_id uniqueness across instances
/// (a random u128) is a CALLER obligation - duplicate ids make two leaders indistinguishable.
class Gc
{
public:
    Gc(StorePtr store_, UInt128 gc_id_);

    /// One full round. Returns acquired_lease=false (nothing else done) if another leader is alive.
    RoundReport runRegularRound();

    /// full-GC walk + debris reclaim: deferred (M-F); API slot reserved.

private:
    /// Lease acquire/renew/steal per the documented observation protocol. On success `state` holds
    /// the committed gc/state (with our lease) and `state_token` its backend token.
    bool acquireOrRenewLease(GcState & state, Token & state_token);

    /// What one R1 fold produced (all derived from durable state; `transitioned` is the only
    /// in-memory-transition artifact and it is REPORT/CROSS-CHECK ONLY - retire (Task 7) derives
    /// its candidates STATELESSLY from the durable snap via GcSnap::zeroInDegreeKnown, so a
    /// crash-replayed round sees the same candidates as the round that folded).
    struct FoldResult
    {
        std::map<uint64_t, GcSnap> snap;                              /// snap_shard -> loaded+updated shard
        std::vector<Candidate> transitioned;                          /// nodes that zeroed during THIS fold
        std::vector<std::pair<RootNamespace, uint64_t>> root_shards;  /// discovered present manifests
    };

    /// R1 (spec §7; the model's GFold): per root shard, stream-merge the journal records in
    /// (folded_cursor, shard_version] into the snap shards. Add => last-op-wins root edge (the
    /// displaced old target is collected into `transitioned` - the Task-3 republish carryover) +
    /// once-per-tree expansion (read the tree ONCE, add its child edges into each CHILD's snap
    /// shard, set the marker in the TREE's home shard); Remove => drop the root edge only (the
    /// cascade strip belongs to the delete pipeline, Task 10). Fresh uploads are invisible by
    /// construction (no journal record until publish).
    ///
    /// DURABLE-BEFORE-CURSOR: the updated snap goes durable FIRST (ALL shards, even unchanged
    /// ones - simplest correct v1; skipping byte-identical shards is a possible later
    /// optimization), only then does ONE gc/state CAS advance snap_generation + folded_cursor
    /// against `state_token`. Generation objects are write-once (putIfAbsent) and the write
    /// generation PROBES UPWARD from snap_generation+1: per generation, every shard Done or
    /// byte-equal (our own crash-replay) => adopt; any shard divergent => abandon that generation
    /// and try one higher - a FIXED generation would wedge GC forever once an orphan plus new
    /// journal records make the bytes unmatchable. Abandoned partials are harmless orphans
    /// (full-GC cleans them in M-F); generations need not be dense - the gc/state pointer is
    /// authoritative. A gc/state CAS Conflict means another leader advanced state - throw ABORTED
    /// (retry next round). snap_shards != 1 is refused (NOT_IMPLEMENTED): cross-shard last-op-wins
    /// displacement is undesigned in M-C3.
    ///
    /// On success `state` carries the committed snap_generation/folded_cursor.
    FoldResult fold(GcState & state, const Token & state_token);

    /// Update the remembered observation (steal protocol step 3/4).
    void rememberObservation(const GcLease & lease);

    StorePtr store;
    UInt128 gc_id{};              /// this leader's identity (random u128, never 0)

    /// the contender's observation window (steal protocol)
    bool has_observation = false;
    UInt128 last_seen_owner{};
    uint64_t last_seen_seq = 0;
};

}
