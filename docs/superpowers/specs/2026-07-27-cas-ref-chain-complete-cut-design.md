---
description: 'Design for closing the LIST-incompleteness release blocker: every ref-log record carries a prev link to its durable predecessor, the GC fold advances its cursor only along the verified chain (repairing listing holes with exact-key reads), a fixed-key CAS seal pointer makes void intervals decidable without trusting listings, the orphan-manifest sweep gains a provable owner-grant bound, REBUILD and fsck stop trusting hot listings, and every residual is named with its detector.'
sidebar_label: 'CAS ref-chain complete-cut'
sidebar_position: 20260727
slug: /superpowers/specs/cas-ref-chain-complete-cut-design
title: 'CAS: ref-log prev-chain and the complete-cut fold'
doc_type: 'reference'
---

# CAS: ref-log prev-chain and the complete-cut fold {#cas-ref-chain-complete-cut}

**Date:** 2026-07-27. **Status:** v4 — after three adversarial review rounds (all REJECT; every finding
incorporated, refuted-with-record, or superseded — §14–§16); awaiting review round 4 and user review.
**Branch:** `cas-gc-rebuild`.

Fixes BACKLOG `{#list-as-journal-dataloss-2026-07-25}` — observed, not modelled: an enumeration of
`cas/refs/` omitted two objects durable for nineteen seconds while returning a key written 2.2 ms after
them (`reports/2026-07-26-list-incompleteness-investigation.md`, evidence in
`reports/2026-07-26-list-incompleteness-proof/`). Realizes proposal P4 of `cas/draft-fixes-20260726.md`
("authoritative per-namespace chain plus a complete-cut gate") and satisfies its refutation condition:
the append path and the fold's per-record path add zero requests.

## 1. Problem {#problem}

GC discovers ref-log transactions by listing the ref prefix, folds what the listing returned, and seals
the per-namespace cursor (`ShardCoverage::last_folded_ref_id`) above every record the round OBSERVED —
not above every record that EXISTS. The intake loop (`CasGc.cpp`, `fold`) iterates listed ids only, so an
id the listing omitted between two listed ids is sealed past silently and can never be folded again. A
skipped `-1` leaves a permanent retention leak; a skipped `+1` hides a live owner and lets GC delete a
blob a committed manifest still references — acknowledged-data loss.

The untrusted-listing shape has several destructive consumers, all covered here: the fold cursor (§5),
the orphan-manifest sweep's protection set (§8), snapshot/log cleanup planning (§7), writer recovery
(§9), `SYSTEM CONTENT ADDRESSED GC REBUILD` and fsck (§10).

Why nothing simpler works: `next_ref_sequence` is pool-wide (`CasRefLedger.h`), shared by every
namespace of the mounted writer, so within one namespace arbitrary id gaps are the NORM. The candidate
ids of a suspected hole cannot be enumerated and probed. Completeness therefore needs declarations from
the writer: a per-record `prev` link, and a per-namespace point-readable seal pointer.

## 2. Trust boundary {#trust-boundary}

- **Hot-prefix LIST is UNTRUSTED.** The GC fold, the orphan sweep, cleanup planning, REBUILD and fsck
  enumerate `cas/refs/` under concurrent appends and deletes. No destructive decision may assume such a
  listing is complete — and no VOID decision may depend on a listing having shown the seal (§4's
  pointer exists precisely for that).
- **Cold-prefix LIST is TRUSTED for the acked region.** Recovery lists a namespace whose writer session
  has ended; every ACKED transaction settled strictly before that session ended, so the documented
  strong read-after-write contract (which includes `LIST`) covers it. Permitted concurrent mutators are
  enumerated and handled (late snapshot PUTs of the dead session; peer-GC log deletions gated by
  cursor AND snapshot coverage; peer-GC snapshot deletions after §7's pin; `Removed`-snapshot
  publication) — object-level races go through the vanish-restart loop with a sticky floor (§9).
- **The unacked in-flight tail is NOT timing-bounded.** v3 argued a same-node ghost (a PUT recovery did
  not see that lands later) was chronologically impossible; review round 3 refuted it at source level:
  `attempt_timeout_ms` is a scheduling check only, the socket-level wait lives on the disk client and
  no mount validation ties it to the self-remount drain or `materialization_grace_ms`
  (`CasRequestControl.h`, `validateCasRequestBudget`, `CasPool.cpp` grace comment — which itself
  predicted this). A socket-stuck conditional PUT can therefore land AFTER recovery sealed past it.
  Consequence: voidness must be decidable from durable data — the seal pointer (§4) — not from timing
  and not from listings.
- Point reads (`GET`/`HEAD`/CAS of an exact key) are trusted; conditional writes already depend on
  them, and at the observed firing the point read told the truth while the listing lied. A store that
  violates the cold contract or conditional-write semantics is rejected by `Cas::Probe` / the
  mount-time LIST probe (task #23), and violations that leave traces are made LOUD (§5).

## 3. Invariant {#invariant}

**Complete cut.** A namespace's cursor advances only along the verified chain: every id between the old
and the new cursor is either (a) folded, in id order, or (b) inside the void interval declared by the
namespace's seal pointer. Completeness and voidness are proved from point-readable data; listing
completeness is assumed nowhere. Destructive decisions derived from a listing — sweep deletions (§8),
cleanup (§7), REBUILD condemnation (§10) — are valid only at or below a proven cut or under a proven
quiescent view.

## 4. Writer changes {#writer-changes}

**The `prev` link.** Every ref-log transaction body gains one field: `prev : RefTxnId` — the id of the
previous durable record of THIS namespace, across epoch boundaries; the value is `candidate_base_id`
in `commitRefChunk`, exact by the wedge discipline. Chunked flushes link chunk to chunk.

**The seal, unchanged in shape.** The recovery seal stays what it is: a write-once `RefTableSnapshot`
at `_snap/{adopted_epoch − 1, UINT64_MAX}` carrying `sealed_from` and the full recovered state,
published fail-closed before the table installs. Publishing it also advances the recovered state's
high-water to the seal id (r1 finding 1), so the first post-recovery append stamps `prev = seal_id` and
the chain passes THROUGH the seal. Voidness is the INTERVAL `sealed_from < X.id <= seal_id` — never
keyed by epoch (mount retries burn epochs; r2 finding 1).

**The seal POINTER — the seal's LIST-free discoverability (r3 blockers 1–2, user decision).** The
seal's own key is id-derived and not computable from a dead record's id, and discovering it through a
listing would hand the void decision back to the liar. Recovery therefore also advances a per-namespace
fixed-key object (`<ns>/_seal_latest`, ~100 bytes): `{seal_id, sealed_from, lineage}`. Discipline:

- written in the SAME fail-closed publication step: seal (write-once) → pointer (CAS) → install; a
  failed pointer CAS fails recovery exactly as a failed seal PUT does today, and the install re-checks
  `superseded_by_remount` before publishing (closing the adjacent install race r3 found);
- **CAS-monotone by `seal_id`**, same conditional-update mechanics as `gc/state`: a socket-stuck stale
  pointer PUT from an older recovery loses the CAS and rolls nothing back;
- updates MERGE fields: a seal update preserves the lineage slot and vice versa;
- `NeverBorn`: an unclean recovery over an EMPTY listing still publishes a seal + pointer. The seal
  format gains an explicit never-born state: `sealed_from = {0,0}` permitted in THIS state only (the
  codec's zero-rejection gains exactly this exception), no rows, no `remove_txn_id`, lifecycle
  distinct from `Live` and `Removed`, greatest-applied = seal id (r2 finding 3, r3 finding 10);
- namespace-removal cleanup RETAINS `_seal_latest` (it is the lineage tombstone, §6) and its presence
  does not contradict the "observed empty" completion criterion.

**Owner-grant bound `R*`, reformulated (r3 blockers 3–4).** v3 defined `R*` over settled grants; that
premise is false — `~PartWriteTxn` retires the build unconditionally while a grant may still be
wedged-Unresolved and resolve durable later. `R*` is therefore the member's **highest ALLOCATED ref
id** (`next_ref_sequence` sample), with enforced ordering: retire build → sample allocator → publish
the lease with `min_active` and `R*`. Every grant of a retired build has an id allocated before
retirement, hence `<= R*` regardless of how late it settles. Cross-epoch initialization: the FIRST
lease of a claimed epoch `E` publishes the bound `{E − 1, UINT64_MAX}`, covering every dead epoch's
grants by construction; `min_active`/`R*` replacement is monotone. An absent or undecodable bound
means RETAIN (§8).

**Id hygiene.** `allocateRefTxnId` saturates (fails closed) before `ref_sequence` reaches
`UINT64_MAX`; the codec rejects any non-anchor `prev >= txn_id`; chain walkers carry cycle detection
and a step bound.

Anchor forms of `prev`:

| form | meaning | acceptance rule at the consumer |
|---|---|---|
| `(E, s)` | ordinary link, incl. across a CLEAN epoch boundary and across removal/rebirth | `prev == resolved_through` |
| `{E, UINT64_MAX}` | the recovery seal published at the boundary the writer recovered across | seal point-read at its exact key AND `resolved_through == seal.sealed_from` (verbatim; consecutive seals chain by declared values; `NeverBorn` declares `{0,0}`) |
| `{0, 0}` | first record of the namespace's FIRST life | `resolved_through == {0,0}` AND the seal pointer is absent AND no lineage tombstone exists |

Cost: ~16 bytes per ref-log record; one `RefTxnId` in the lease; one ~100-byte CAS object per
namespace touched only on unclean recovery and removal completion; zero added requests on the append
path. Format: version bumps of `CasRefLogFormat`, `CasRefSnapshotFormat`, the lease format, and the new
pointer object; decoders do not accept old versions — pre-release, no compat scaffolding.

## 5. Fold rules {#fold-rules}

The listing is only a CANDIDATE stream; the chain and the pointer are the truth.

**Rule 0 — pointer read.** A namespace with NO listed candidates above its cursor is skipped entirely,
at zero cost, exactly as today. A namespace WITH candidates first point-reads `_seal_latest` (one small
`GET`; absent pointer = no unclean boundary ever = empty void interval). The read is per round, not
cached across rounds: a cache's validity would depend on knowing whether a remount happened, which
without the read is exactly the listing-trust hole.

Then, for each candidate X in ascending id order, after the body `GET` + decode the fold already
performs:

1. **Void.** `pointer.sealed_from < X.id <= pointer.seal_id` → certified void: skip, never fold (the
   writer retroactively erased it; folding a late `-1` or `remove_namespace` the table never applied is
   corruption), count `certified_void`, LOUD `gc_anomaly` (such a record is a socket-stuck straggler or
   worse — §2). Once the cursor crosses the anchor, void records sit below it and `cleanupRefObjects`
   removes them as covered debris.
2. **Continue.** `X.prev == resolved_through` → fold, advance. One comparison; zero added requests.
3. **Repair.** `X.prev > resolved_through` → walk backward by exact-key `GET` until reaching
   `resolved_through` or an anchor accepted by §4, then fold forward in id order. Bounded: bodies up to
   a byte budget, then ids-only with forward re-reads in bounded chunks; past a hard step bound → hold.
   Loud: `CasGcChainRepairedHoles` + `gc_anomaly` rows capped at 32/round with true totals.
4. **Anchor crossing.** `X.prev == {E', UINT64_MAX}` → point-read that seal (exact key from the anchor
   value). Accept iff `resolved_through == seal.sealed_from`; if `resolved_through < sealed_from`,
   enter rule 3's walk at `sealed_from` first.
5. **Breach.** `sealed_from < resolved_through < seal_id` at rule 0 or at an anchor crossing — a past
   round folded records later voided (the pointer race of §5c, or worse) → immediate breach: the
   namespace holds AT its current cursor (no further advance), `CasGcChainIntervalBreach`, persistent
   quarantine, destructive suppression stays asserted (§5b), operator escalation. Repair of
   already-folded void edges is a REBUILD-class action; the spec does not pretend a cheaper one exists.
6. **Hold.** A repair `GET` 404s (deposed-leader LOG cleanup cannot explain it — log deletion requires
   cursor AND snapshot coverage below `resolved_through`; seal/`Removed` deletion is excluded by §7's
   pin), or `X.prev < resolved_through` outside any void interval (chain split), or an anchor's seal
   point-read 404s → per-namespace hold: `classification = 4`, cursor stays, loud `gc_anomaly`,
   `CasGcChainHolds`. Sibling namespaces proceed.

The whole-round `ref_folding_aborted` remains only for an unparseable key in `groupRefKeys`; "ref log
body vanished mid-fold" becomes a rule-6 hold.

**Probe A is kept verbatim** (both walks, comparison, HEAD verdict, both counters, 32-row cap); the
chain repairs or holds instead of aborting. Blind spots close structurally: identical holes repair off
`prev`; a wholesale-dropped namespace is pure delay via §6.

### 5a. Accounting: probes B1 and B2, cut-scoped (r2 finding 8) {#fold-accounting}

One shared intake primitive processes listed AND repaired records; B2 opens ordinals only for
transactions whose deltas commit into the round. B1 is two-phase and cut-scoped:
`logs_accounted == logs_applied + logs_certified_void` over exactly `(parent_cursor, final_cursor]`;
speculative repairs and void observations above the final cut are separate metrics. Negative controls
in §11.

### 5b. Quarantine, persisted (r3 finding 9) {#quarantine}

Hold and breach states are DURABLE: the fold seal's coverage entry gains first-held round, retry
generation and the offending link/anchor. `suppress_destructive` is computed from every CARRIED
quarantine on every round — a backoff round that skips a held namespace's point reads must not let
destructive work resume. Backoff bounds only the namespace's anchor/repair point reads; the honest
liveness statement stands: one quarantined namespace suspends pool-wide destructive work until
revalidated or operator-resolved, because an unknown `+1` may name any shared blob. Revalidation (a
later round whose reads succeed and whose chain verifies) clears the quarantine explicitly.

### 5c. The fold-vs-recovery race, named (round-4 target) {#fold-recovery-race}

A fold (possibly on a peer node) can run CONCURRENTLY with a not-yet-finished recovery: it reads the
OLD pointer, and a socket-stuck straggler that the in-flight recovery will void can pass rule 2 as a
plain continuation. Claimed containment, to be attacked in round 4 and mechanised in the TLA model:
the ghost's edges cannot reach a destructive action before detection, because (a) deletion graduation
waits for the ack floor — every mount, including the recovering one, must ack past the minting round —
and (b) the recovering mount's first post-install fold reads the NEW pointer and rule 5 fires the
breach, asserting suppression, before that ack can land. If round 4 breaks this ordering, the fallback
is a recovery-generation check in graduation itself; the race does NOT weaken rule 1's decidability
for every round that starts after the pointer is durable.

## 6. Cursor lineage: carry-forward and tombstone handoff (r2 finding 6, r3 finding 8) {#cursor-lineage}

The new fold seal carries forward the parent `per_ns_shard` entry of every namespace NOT visited this
round, on the normal AND the abort path. Entries are retired exactly once, by HANDOFF: when a removal
reaches `Completed`, the lineage (`remove_txn_id`) is CAS-merged into `_seal_latest` FIRST, and only a
round that has verified the pointer carries the lineage drops the seal entry. The fold seal therefore
stays bounded by ACTIVE namespaces; the per-namespace tombstone is permanent, tiny, and point-readable.
Rebirth: `prev == remove_txn_id` is accepted against the seal entry or the pointer lineage; `{0,0}` is
rejected whenever either exists. A resurfaced old-life log is rejected by the same comparison.

Removal admission (r3 finding 8): a `remove_namespace` reserves capacity through a CASed pool-wide
admission object BEFORE appending; the slot is released at `Completed`. The cap is hard; the refusal is
loud. `DROP TABLE` is rare — one CAS on that path is acceptable.

## 7. Retention pins (r2 finding 7, r3 finding 11) {#snapshot-pin}

Recovery seals and `Removed` snapshots are not deletable while above the namespace's durable fold
cursor; ordinary snapshots keep newest-only retention. The cleanup planner receives the durable cursor
already; identifying `Removed` snapshots needs lifecycle metadata beside the listed id (one extra read
or a listing-side marker — plan detail, named).

## 8. Orphan-manifest sweep: deletions bounded by `R*` (r2 finding 2, r3 blockers 3–5) {#orphan-sweep}

The sweep's owner-set reconstruction switches to the chain-verified read path, and a manifest of build
B is deletable only if: B is below the CURRENT lease's `min_active`; the namespace's chain-verified cut
is at or above the `R*` paired with that `min_active`; and the cut shows no owner. Every other state —
absent `R*`, stale or unreadable lease, cut below `R*`, hold/quarantine in effect — RETAINS. With `R*`
as highest-allocated (§4) the bound covers wedged grants that settle late; with the epoch-claim
initialization it covers dead epochs.

Documented consequence of the member-wide bound (r3 finding 5): a QUIET namespace whose chain frontier
sits far below the member's allocator retains its orphans until the namespace writes again or a remount
seal dominates the bound. That is retention, not loss; a quiet namespace also produces no new orphans.
Per-namespace grant certificates are named future work if this retention ever matters in practice.

This section lands together with the S42 stale-edge fix (the sweep stranding folded `+1` edges) as one
coherent change to the sweep.

## 9. Recovery replay verification (r1 finding 7, r2 finding 5, r3 finding 7) {#recovery}

Recovery replay verifies the same chain (seeded by the selected snapshot's id; anchors per §4), repairs
missing middle links by exact-key `GET`, and routes a repair 404 through the bounded vanish-restart
loop — **with a sticky floor held in LEDGER-level state**, surviving runtime eviction and transferred
across self-remount: once a successor declared predecessor `P`, every later attempt must select a
snapshot covering at least `P` or find `P`; omission of the successor does not lower the floor.
Exhausting the brake fails recovery closed, loudly. The frontier above the listed maximum is trusted
per §2's acked-region contract; the unacked straggler is the seal's and pointer's job, not replay's.

## 10. REBUILD and fsck (r2 finding 4, r3 findings 6, 10) {#rebuild-fsck}

REBUILD condemns from a LIST-derived universe, and today even lease discovery is LIST-based — a
quiescence proof built on lease enumeration would be circular. Therefore: **REBUILD refuses to run**
until a durable pool-wide maintenance fence exists (the `SYSTEM` control surface dependency, named), OR
the operator passes an explicit attestation flag asserting all writers are stopped — trust-the-operator,
stated as such in its own output. This interim matters because `gc/state` corruption recovery currently
DIRECTS the operator to REBUILD; a silent dead end is not acceptable, a loud attested path is.

fsck gains a seal-aware oracle (a synthetic seal id with no same-id log is NOT evidence of listing
incompleteness — otherwise every healthy sealed pool reads `unchecked` forever), and its absence-based
verdicts are whole-run `unchecked` only when the universe or frontier is genuinely unproven. Both
`chain-broken` and the unchecked state are `clean()` terms and exit-code-fatal, with RED tests in both
directions: `unchecked` fires when it must, and a healthy sealed pool can still return clean.

## 11. Observability and verification {#verification}

ProfileEvents: `CasGcChainRepairedHoles`, `CasGcChainHolds`, `CasGcChainVoidCertified`,
`CasGcChainIntervalBreach`, `CasRefRecoveryChainRepairs`. Per-phase metrics on `fold_ref_intake`:
`chain_repairs`, `chain_holds`, `certified_void`, `pointer_reads`, held-namespace age. All registered
in the soak preflight. Audit: `gc_anomaly` rows for every repair, hold, void and breach, capped with
true totals. Counters die with the process; audit rows do not; ship both.

Verification, all tests proven RED first:

- **Unit, on `HoleyListBackend`:**
  - hidden middle key, honest `GET` → repair in id order (the observed `0x1430c`/`0x1430d` scenario);
    hidden key AND broken point read → hold, siblings advance;
  - ghost on a QUIET namespace, no successor ever: pointer read certifies void with no anchor crossing
    needed (r3 blocker 2's counterexample becomes the test);
  - same-round ghost + successor: void fires before continuation; breach fires when the cursor is
    already inside the interval (`resolved` inside, not only `parent_cursor` — the r3 correction);
  - burned epochs: interval semantics only, no epoch-keyed logic;
  - pointer: CAS-monotone under a late stale writer; merge preserves lineage; absent pointer on a
    clean-history namespace folds normally; recovery fails closed when the pointer CAS fails; install
    refuses after `superseded_by_remount`;
  - `NeverBorn`: codec round-trip with the zero-`sealed_from` exception, later `NamespaceBirth`
    proceeds, fsck reads it as sealed-clean, not unchecked;
  - seal high-water adoption: first post-recovery record's `prev == seal_id`;
  - rebirth and resurfaced old-life logs against seal entry and pointer lineage; handoff drops the
    seal entry only after the pointer carries the lineage; cleanup retains `_seal_latest`;
  - pins: seal and `Removed` snapshot above the cursor survive cleanup planning; ordinary snapshots do
    not accumulate;
  - sweep: newest promote hidden → retained; cut below `R*` → retained; stale/absent lease or bound →
    retained; wedged grant resolving late above a settled-style bound would delete (RED against the v3
    definition) and retains under highest-allocated; epoch-claim init covers dead epochs;
  - recovery sticky floor survives runtime eviction and self-remount;
  - B1 two-phase negative controls; id hygiene (saturating allocator, `prev >= txn_id` rejected, cycle
    bound); quarantine persistence: suppression computed from carried state on a backoff round.
- **TLA+ (phase 0):** extend `_sab_holeylist` with the chain rule, pointer/interval void semantics,
  and the sweep's `R*` bound; prove: the cursor never passes an unfolded non-void record under
  arbitrary listing omissions; void certification never voids a writer-applied record; the sweep never
  deletes an owned manifest; and the §5c race cannot reach a graduated deletion before breach
  suppression (or produce the counterexample that forces the graduation-side check).
- **Consult round 4** attacks, at minimum: the pointer lifecycle (CAS semantics, mid-sequence failure,
  merge, cleanup survival), §5c's containment argument, `R*` highest-allocated ordering
  enforceability, the REBUILD attestation interim, and the fsck seal-aware/unchecked balance.
- **Gate:** the existing soak; expected signature: repairs > 0 on a lying store, zero holds, zero
  breaches, zero aborts.

## 12. Performance, narrowed honestly (r3 finding 11) {#performance}

The append path and the fold's per-record path add zero requests (~16 bytes/record; one comparison).
Per-round additions: one `_seal_latest` `GET` per namespace WITH candidates (quiet namespaces: zero);
repair `GET`s equal to the omitted records' owed body reads, doubled only for over-budget runs; one
anchor seal read per crossing, repeated per round only while clamped below it, stopped by quarantine
backoff. NOT bounded by backoff and stated plainly: the two full ref-prefix enumerations per round,
probe A's set comparisons, and fold-seal encode/decode — the last no longer grows with dead namespaces
(lineage handoff, §6). Strict per-namespace apply order is unchanged; P1 prefetch stays compatible.

## 13. Alternatives rejected, with their review history {#alternatives}

| alternative | why rejected |
|---|---|
| GC-side containment only (hold + scoped re-`LIST` + K-round quorum) | Probabilistic; asks the liar again; sound elements survive as rules 3/6. |
| Local acked-floor file / `_tail` object / Keeper floor for the recovery TAIL | User ruling stands: the acked region is contract-covered; the floor protects nothing the seal machinery does not. |
| Widening probe A's witness rule | Permanent-block risk on cleaned namespaces (pre-existing decision). |
| `{0,0}` rebirth + cursor retirement | Destroyed the lineage proof (r2 finding 3). |
| NO pointer — chronology bounds the ghost (v3 §2/§13) | **Refuted by r3 at source level** (socket wait unbounded by any enforced relation); withdrawn. |
| Enforced-timing alternative (mount-validated `grace > full client socket budget`) | Considered at the r3 fork; rejected with the user: argued-from-config with an undocumented server-side materialization tail — INTENT wants demonstrated-from-data; the pointer costs one tiny GET per active namespace instead. |
| Void by settled-`R*` (v3 §8) | Refuted by r3 (unconditional build retirement vs wedged grants); superseded by highest-allocated. |
| Pinning every snapshot above the cursor | Unbounded accumulation (r2 finding 7); narrowed to seals + `Removed`. |
| Tombstones forever in every fold seal | Unbounded control object (r2 finding 6, r3 finding 8); superseded by pointer handoff. |

## 14. Review round 1 {#review-round-1}

`gpt-5.6-sol` `xhigh`, `tmp/codex_spec_review_sol.log`, **REJECT**, eleven findings — all verified
against code and acted on; eight remedies later refuted in whole or part by round 2 (§15). Mapping as
in v2/v3: seal adoption; sweep; rebirth/tombstones; empty seal; snapshot pin; B1/B2; recovery
mutators/404; fsck; pending liveness; bounded repair; id hygiene.

## 15. Review round 2 {#review-round-2}

Fresh context, tasked to refute round 1, `tmp/codex_spec_review_sol_r2.log`, **REJECT**, ten findings:
interval-not-epoch voidness; the sweep's missing primitive; `NeverBorn`; REBUILD/fsck as hot-LIST
consumers; sticky recovery floor; tombstone bounds; pin narrowing; cut-scoped B1; quarantine honesty;
repair-row caps. Two remedies (authority object; disable-sweep) were overruled in v3 — wrongly, as
round 3 showed for the first.

## 16. Review round 3 {#review-round-3}

Fresh context, tasked to attack v3's two overrules, `tmp/codex_spec_review_sol_r3.log`, **REJECT**:

| # | severity | finding | disposition in v4 |
|---|---|---|---|
| 1 | blocker | the in-flight chronology is not a protocol guarantee (socket wait unbounded by any enforced relation) | overrule withdrawn; seal pointer added (§4, user decision) |
| 2 | blocker | §5b detection neither eventual nor correct (quiet namespace; `resolved` vs `parent_cursor`) | pointer read before rule 2; breach condition on `resolved` (§5 rules 0/1/5) |
| 3 | blocker | settled-`R*` premise false (unconditional build retirement vs wedged grants) | `R*` = highest allocated, ordered sample (§4) |
| 4 | blocker | no cross-epoch `R*` initialization; `min_active=0` fresh leases | epoch-claim bound `{E−1, MAX}`; absent ⇒ retain (§4/§8) |
| 5 | major | member-wide `R*` starves quiet namespaces | documented retention; per-namespace certificates named future work (§8) |
| 6 | major | REBUILD quiescence proof circular over LIST-based lease discovery | REBUILD refuses; interim operator attestation flag (§10) |
| 7 | major | sticky floor stored in evictable runtime; install race with supersession | ledger-level floor; install re-check (§9, §4) |
| 8 | major | admission cap not linearizable; tombstones still unbounded | CASed admission object; pointer handoff bounds the seal (§6) |
| 9 | major | quarantine unpersisted; suppression could lapse on backoff rounds | durable quarantine; suppression from carried state (§5b) |
| 10 | major | `NeverBorn` unencodable as specified; seal-unaware fsck makes sealed pools forever `unchecked` | precise `NeverBorn` + seal-aware oracle (§4/§10) |
| 11 | minor | performance claim over-broad; planner cannot identify `Removed` snapshots | §12 narrowed; §7 metadata named |

Survivors it confirmed: interval construction under burned epochs; rebirth safety via the durable
tombstone; cut-scoped B1 coherence; cleanup planner's access to the durable cursor.

## 17. Out of scope, named {#out-of-scope}

- The mount-time LIST-consistency probe (task #23) — separate, tracked.
- The decommission/adoption proven-dead fencing against frozen-not-dead writers — the pool-member
  decommission spec.
- The durable pool-wide maintenance fence (`SYSTEM` control surface) — named dependency of §10; until
  it lands, REBUILD runs only under operator attestation.
- The soak-gating policy for the new counters; the 56 leaked blobs; the `-1`-before-`+1` path; GC
  performance work (P1/P2/P3); the fsck budget; the RustFS store-side mechanism.
