#pragma once
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasGcFormats.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasGcOutcomes.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasGcSnap.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasStore.h>

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
