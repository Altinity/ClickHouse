---
description: 'Rev.6 Task 13 compliance audit: the six named GC-side defense sites checked against the lease-boundary-exclusivity anomaly policy (zero S3 requests spent fighting a foreign writer on hot paths). All six sites found compliant; no code changes.'
sidebar_label: 'GC-Defense Compliance Audit (rev.6 Task 13)'
sidebar_position: 20260714
slug: /superpowers/reports/cas-gc-defense-audit-2026-07-14
title: 'CAS GC-Defense Compliance Audit vs rev.6 Anomaly Policy (Task 13)'
doc_type: 'reference'
---

# CAS GC-Defense Compliance Audit vs rev.6 Anomaly Policy (Task 13) {#cas-gc-defense-audit}

**Date:** 2026-07-14. **Branch:** `cas-gc-rebuild`. **Scope:** Task 13 of the
[rev.6 lease-exclusivity design](../specs/2026-07-13-cas-ref-lease-exclusivity-rev6-design.md)'s
[§GC-defense audit](../specs/2026-07-13-cas-ref-lease-exclusivity-rev6-design.md#gc-defense-audit).
Audit only — code changes were in scope solely for a site the audit found non-compliant.

## Principle being audited {#principle}

Rev.6 solves writer exclusivity once, at the mount-lease boundary. Past that boundary, "no hot path
spends a single S3 request detecting or fighting a foreign write" (spec §Principle); the only
exception is **incidental checks** — signals that arrive for free on operations already being
performed for another reason (a conditional-write token mismatch, a value read as part of the work
itself) — and **CAS-linearized commits**, where a deposed actor simply fails its own conditional
write and needs no separate detection request. This audit checks six named GC-side sites against
that principle: for each, what it defends against, its request cost on the hot path, and a
compliant/non-compliant verdict.

Two axes of "foreign" appear among the six sites: a **foreign writer** (the per-table mount-lease
holder, whose exclusivity rev.6 now fences at the mount boundary) and a **deposed or zombie GC
leader** (GC-vs-GC leadership, explicitly scoped by the spec's decision log #10 as "not redesigned —
linearized by a single CAS on `gc/state` per round, no clocks, a deposed leader simply fails its
commit"). All six sites are read against the same test: is any request spent **beyond** what the
underlying CAS/lease mechanics already require, purely to look for or contest interference.

## Site 1 — `gc/state` round-ownership re-read {#site-1-round-ownership-reread}

**Location:** the `round_still_ours` lambda,
`Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasGc.cpp:1466-1473`, called from
`Gc::runNamespaceCleanupPasses` at `CasGc.cpp:1483` (once, before starting a `Pending` namespace's
physical-reclaim pass) and at `CasGc.cpp:1504` (once per LIST page inside that pass, alongside a
completion-marker `HEAD` at `CasGc.cpp:1519` checked per listed key).

**What it defends against:** `runNamespaceCleanupPasses` performs the ONE place in `Core/` where GC
physically deletes an entire removed namespace's manifest bodies and verbatim files
(`CasGc.cpp:1490-1541`) — a destructive, page-by-page enumerate-and-delete that necessarily runs
*after* this round's single `gc/state` CAS already committed (that CAS happens earlier, at
`CasGc.cpp:581`, inside `runRegularRound`'s R5 step). Because this destructive work outlives its own
round's CAS, "a deposed leader simply fails its commit" does not by itself cover it: a *later* round
could re-elect a different leader (or the writer could recreate the namespace after a legitimate
successor `Completed` it) while this pass is still mid-flight. `round_still_ours` re-reads `round`
(strictly incrementing, CAS-linearized, spec §GC State) — never a wall clock — and aborts the
destructive pass the instant a successor has advanced past it, so a possibly-stale leader cannot
delete data a successor may have let the writer legitimately recreate.

**Request cost on the hot path:** this lambda, and the pass it guards, execute **only** when at
least one `RefNsCleanupState::Pending` item exists in the fold seal — i.e., only while an in-progress
namespace removal (`DROP`-class event) is being physically reclaimed. The ordinary per-round fold
hot path (the vast majority of rounds, which have no pending namespace removal) never calls it: zero
cost there. When it does run, the true cost is larger than a first read of the page-level checks
suggests: one `GET gc/state` (`round_still_ours`) plus one `HEAD` on `marker_key` per LIST page
(`CasGc.cpp:1504`) — **and, separately, one additional uncached `HEAD` on `marker_key` per listed
key** inside the page's inner loop (`CasGc.cpp:1519`, up to 1000 per page). For a listed key that
already carries its token from the LIST (no `HEAD` otherwise needed for its own delete), that
per-key marker check **doubles** its request cost (1 `HEAD` + 1 `deleteExact` instead of 1
`deleteExact` alone).

**Verdict: compliant, keep — including the per-key check.** This is not writer-defense (writer
exclusivity is the mount-lease boundary's job, unaffected by this code) and it is not "detecting
interference" speculatively — it is a freshness fence gating one specific, rare, genuinely
destructive operation whose blast radius (deleting real data) justifies the cost. The per-key `HEAD`
specifically is load-bearing, not gratuitous: the comment at `CasGc.cpp:1511-1518` explains it closes
the exact window a page-level-only check would reopen — a namespace recreation landing **mid-page**,
after the page's own freshness check passed but before every key in it has been deleted. A
per-**pass**-only check would be weaker still, given pages carry up to 1000 keys and a page's deletes
are not instantaneous. No cheaper compliant alternative preserving the same recreation-safety
guarantee was found; this per-key cost is **not** backlogged for reduction. Matches the spec's own
expected verdict for this site ("linearized by the single round CAS, no clocks, deposed leader fails
its commit") — the extra cost buys safety for a rare, destructive, already-expensive pass, not
interference-detection on a hot path.

## Site 2 — zombie-steal committed-pair threading {#site-2-zombie-steal-threading}

**Location:** documented at
`Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasGc.h:247-248`; realized by
`Gc::fold` (declared `CasGc.h:254`, called `CasGc.cpp:352`) mutating the caller's `state`/
`state_token` **in place**, and `Gc::runRegularRound`'s R5 single round CAS
(`CasGc.cpp:572-587`) consuming that exact `state_token` — acquired once at the top of the round via
`acquireOrRenewLease` (`CasGc.cpp:231`) — with no intervening re-read across R2 (fold, `CasGc.cpp:352`),
R3 (pre-CAS redelete/retire, `CasGc.cpp:369-566`), and R4 (retired-in-snapshot bookkeeping,
`CasGc.cpp:567-570`).

**What it defends against:** a naive "re-read `gc/state`, then CAS" pattern would open a TOCTOU
window in which a zombie GC instance — one deposed and then re-elected, or racing a concurrent leader
— could read a fresh value, act on it, and still lose the race at CAS time, having spent a request
that bought nothing (the CAS itself already re-validates the token atomically). Threading the
already-held `(state, state_token)` pair straight through to the single terminal CAS closes that
window by construction.

**Request cost on the hot path:** **zero** — this is the absence of a request. It is the strongest
form of compliance available: not "cheap," but literally no GET where a re-read would otherwise be
tempting.

**Verdict: compliant, keep.**

## Site 3 — deposed-leader debris handling {#site-3-deposed-leader-debris}

**Location:** `Gc::pruneSupersededGenerations`, part (2),
`Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasGc.cpp:1714-1728` (part (1), the
wholesale generation-retention prune that now does the reclaiming, is `CasGc.cpp:1683-1712`).

**What it defends against:** a deposed leader can write attempt-scoped debris (fold seal, target
runs, part-manifest cleanup, and — under the ack-floor model — retired/outcome sets) under its own
unadopted `lease.seq` before its round CAS fails. That debris needs eventual reclaim.

**Request cost on the hot path: zero**, and the code comment (`CasGc.cpp:1714-1728`) documents why:
a *prior* revision LISTed the current generation's whole `gc/gen/<G>/` prefix **every round**
specifically to hunt this debris — steady-state S3 budget spent chasing a rare concurrent-leader
collision (recorded in this codebase's history as the "GC-DISCOVERY-LIST-QUADRATIC" concern). That
per-round LIST has already been removed. Today, deposed-leader debris is reclaimed **only**
incidentally: the wholesale generation-retention prune in part (1) runs every round regardless (it is
ordinary space reclaim for `gc_snap_generations_to_keep`, unrelated to any leader-defense concern),
and once a stale generation ages past the retention window its *whole* prefix — deposed-leader debris
included — is deleted in one wholesale `deletePrefixWholesale` call. No request is issued that exists
*only* to find or fight a deposed leader.

**Verdict: compliant, keep.** This site was already brought into compliance ahead of rev.6 (the
per-round LIST was already deleted by the time of this audit); the audit confirms no regression and
records the current zero-cost design for the record.

## Site 4 — orphan-sweep prior-epoch eligibility gate {#site-4-orphan-sweep-eligibility-gate}

**Location:** `prefixEligible`,
`Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasOrphanManifestSweep.cpp:207-224`;
called from `sweepNamespace` (`CasOrphanManifestSweep.cpp:228`) and from
`sweepManifestCursorPage` (`CasOrphanManifestSweep.cpp:312`, memoized per unique
`(namespace, writer_epoch, build_sequence)` triple in the `eligible_by_prefix` map,
`CasOrphanManifestSweep.cpp:288-317`, so one sweep page never repeats the check for the same
prefix).

**What it defends against:** nothing foreign — it establishes the one durable fact the sweep needs
to do its job at all: whether a candidate build prefix is below the durable mount watermark
(`floorForNamespace`'s `GET` of the mount lease, `CasOrphanManifestSweep.cpp:43-63`), i.e.
provably dead per spec §Orphan Manifest Protection control #9 ("never a frozen-seq / judged-dead
guess"). A missing watermark fails open to *not eligible* (deletes nothing), so this gate cannot
misfire against a live writer's own in-progress prefix.

**Request cost on the hot path:** up to N `GET`s of the mount lease per unique (namespace, prefix)
the sweep encounters in a page, not a flat one — `floorForNamespace` walks successively shorter
namespace prefixes in a loop (`CasOrphanManifestSweep.cpp:43-63`), issuing one `GET` per candidate
until a durable mount body is found or the candidates are exhausted, so the count is bounded by the
namespace's path-segment depth. This *is* the sweep's actual eligibility determination, not a
supplementary defensive scan layered on top of it; no separate "is someone still writing" probe
exists beyond this single durable-fact lookup, and repeats within a page are memoized to zero.

**Verdict: compliant, keep.** Matches the spec's own framing verbatim: "the cleanup mechanism for
legitimately dead epochs, not a race defense."

## Site 5 — `TokenMismatch`/404 delete tolerance {#site-5-tokenmismatch-404-tolerance}

**Location:** `sweepNamespace`'s exact-token delete,
`Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasOrphanManifestSweep.cpp:261-266`,
and `sweepManifestCursorPage`'s equivalent, `CasOrphanManifestSweep.cpp:346-366`.

**What it defends against:** nothing — it explicitly **tolerates**, rather than reacts to, the two
benign outcomes of a legitimate concurrent event racing an orphan sweep's own exact-token conditional
delete: `NotFound` (the object vanished between `HEAD`/LIST and delete) or `TokenMismatch` (a fresh
owner reclaimed the key under a new incarnation). Both are logged via the `ManifestDelete` audit
event (`CasOrphanManifestSweep.cpp:372-382`) and the sweep moves on — no retry, no re-verification
`GET`, no escalation.

**Request cost on the hot path:** zero incremental. The `HEAD` (when the LIST page carried no token,
`CasOrphanManifestSweep.cpp:263` / `:352-358`) and the conditional `deleteExact` are operations the
sweep must perform anyway to reclaim an orphaned body under the exact-token discipline every delete
site in this codebase follows; classifying the two racy outcomes as benign costs nothing beyond the
delete attempt itself.

**Verdict: compliant, keep.** Matches the spec's own framing verbatim: "tolerates intra-node races
(keep)."

## Site 6 — fold-lag clamps {#site-6-fold-lag-clamps}

The spec names three components under this site: "restart-on-vanish, clamp barriers, clamp
suppression." All three are covered below.

**Location:** the per-log clamp barrier,
`Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasGc.cpp:1014` (mechanism spans
`CasGc.cpp:930-1094`: per-table ref-log intake, `foldManifestEdges` body reads, and the barrier
that keeps a table's durable fold cursor below a log whose manifest body is not yet visible), and
clamp suppression, `CasGc.cpp:1246-1261`.

**What it defends against:** nothing foreign — the clamp barrier handles the *ordinary*, expected
condition of a writer's manifest body not yet being visible to GC (an in-flight upload, not a race
to fight), by simply not advancing the fold cursor past that log and re-reading the same log next
round. Clamp suppression is the round-level consequence: if *any* table clamped this pass,
graduations and pending deletes for the **whole** pass are suppressed (carried forward) because the
"landed before cut ⇒ folded before graduation" lemma no longer holds while any cursor lags
(`CasGc.cpp:1246-1251`).

**Request cost on the hot path:** zero incremental in both directions. The barrier does not cause
*more* requests than an unclamped fold — a log below the clamp is re-read next round (the very same
GET intake the fold performs every round for logs above the cursor; the clamp changes *which* round
reads it, not whether a request is spent detecting anything). Clamp suppression is pure in-memory
computation over `report.anomalies`, already populated by the fold loop above (`CasGc.cpp:1252-1253`)
— it issues no request of its own; it only gates whether previously-computed redeletes/graduations
execute this pass.

**Verdict: compliant, keep.** Matches the spec's own framing verbatim: "intra-node, keep."

### Restart-on-vanish {#site-6-restart-on-vanish}

**Location:** `Gc::fold`'s ref-log intake loop,
`Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasGc.cpp:983-990` (a `GET` of a
listed log's body returns absent or the body fails to decode). This is a genuinely separate
mechanism from the clamp barrier above: the barrier clamps a table's cursor on a not-yet-visible
**manifest** body; restart-on-vanish aborts on a vanished or invalid **ref log** body. Both sit
inside the brief's cited range for this site (`Core/CasGc.cpp:986-1052`).

**What it defends against:** input-visibility ambiguity, not a foreign writer — the log's key was
durable enough to be LISTed this round, so a subsequent `GET` returning absent (or bytes that fail
to decode) means the read raced the backend's own list/read consistency window, not that anyone is
fighting GC for the key. Setting `ref_folding_aborted = true` (`:986`, `:999`) stops folding new
edges for the **rest of the round** — not just this table: the per-table loop's `if
(ref_folding_aborted) break;` (`CasGc.cpp:938`) skips every table not yet visited, and the abort path
(`:1103-1118`) discards every table's in-progress cursor advance for this round, resetting each back
to its parent (already-durable) cursor.

**Request cost on the hot path:** zero incremental, by the same logic as the clamp barrier. Nothing
is re-fetched to confirm or diagnose the vanish; the abort simply forgoes the round's remaining
ref-log `GET`s (a *reduction* in requests this round, not an addition) and lets every table's cursor
— including the one that vanished — resolve on a **subsequent** round's ordinary intake `GET`, the
exact same request every log above a table's durable cursor requires regardless of whether a prior
round aborted.

**Verdict: compliant, keep.** Matches the spec's own framing for the whole "fold-lag machinery"
group: intra-node, keep. Consumed at `CasGc.cpp:1103` (deltas/cleanup/seal reset),
`:1131` (namespace-removal items skipped this round), and `:1150` (Completed-item retirement skipped
this round) — all pure in-memory carry-forward, no request spent at any consumption site either.

## Summary {#summary}

| Site | Location | Verdict |
|---|---|---|
| 1. `gc/state` round-ownership re-read | `CasGc.cpp:1466-1473`, called `:1483`, `:1504`, `:1519` | Compliant — destructive-only, bounded to active namespace removal; per-page **and** per-key checks, the latter load-bearing (closes a mid-page recreation race), not backlogged for reduction |
| 2. Zombie-steal committed-pair threading | `CasGc.h:247-248`; `CasGc.cpp:231,352,572-587` | Compliant — zero requests (absence of a re-read) |
| 3. Deposed-leader debris handling | `CasGc.cpp:1714-1728` (reclaimed via `:1683-1712`) | Compliant — the former per-round hunt was already removed |
| 4. Orphan-sweep prior-epoch eligibility gate | `CasOrphanManifestSweep.cpp:207-224`, memoized `:288-317`, walk `:43-63` | Compliant — the sweep's own eligibility fact (up to N `GET`s, bounded by path depth), not a race defense |
| 5. `TokenMismatch`/404 delete tolerance | `CasOrphanManifestSweep.cpp:261-266`, `:346-366` | Compliant — tolerate-and-continue, zero incremental requests |
| 6. Fold-lag clamps (barrier, suppression, restart-on-vanish) | barrier `CasGc.cpp:1014` (`:930-1094`), suppression `:1246-1261`, restart-on-vanish `:983-990` | Compliant — barrier/restart-on-vanish both re-read/re-fetch what a later round would read anyway (a reduction, not addition, of this round's requests); suppression is pure computation |

All six sites are compliant with the "incidental checks only, zero S3 budget spent fighting a
foreign writer on hot paths" principle. Site 3 documents a defense mechanism that was **already**
brought into compliance (the per-round debris-hunting LIST was removed) prior to this audit; the
other five were compliant by original design. Site 1's per-key marker check (`CasGc.cpp:1519`) is a
real, non-trivial S3 cost — up to one extra `HEAD` per listed key on top of the per-page checks — but
is judged load-bearing for recreation safety, not gratuitous, so it is kept as-is rather than
backlogged for a cheaper alternative. **No code changes result from this audit** — only this
report.
