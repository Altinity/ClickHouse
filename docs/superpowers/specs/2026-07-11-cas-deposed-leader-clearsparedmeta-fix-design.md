---
description: "Design for fixing the deposed-leader stray-Clean clearSparedMeta hole (INV_NO_LOSS live-blob data loss): make GC freshness metadata ADD-ONLY — GC never transitions Condemned->Clean on a spare; only a writer that has displaced the body with a fresh incarnation token may publish Clean. Restores the exact-token delete argument against stale pre-CAS redeletes."
sidebar_label: "Deposed-leader clearSparedMeta fix"
sidebar_position: 21
slug: /superpowers/specs/deposed-leader-clearsparedmeta-fix
title: "CAS GC — add-only freshness meta (deposed-leader clearSparedMeta fix, design)"
doc_type: reference
---

# CAS GC — add-only freshness meta (deposed-leader `clearSparedMeta` fix) {#design}

**Status:** design (2026-07-11), branch `cas-gc-rebuild`. Fixes the real bug in
[`reports/2026-07-11-cas-deposed-leader-stray-clean-meta.md`](../reports/2026-07-11-cas-deposed-leader-stray-clean-meta.md).
Fix chosen after TWO independent strong-model consults: the first proposed adoption-coupling the clear
(post-CAS) with a `condemn_round` guard; the second (decisive) proved that **insufficient** and
recommended the stronger, simpler **add-only** rule adopted here.

## 1. The defect (recap) {#defect}

`clearSparedMeta` flips a blob's per-hash freshness meta `Condemned → Clean` when a GC round SPARES it
(in-degree recovered). It is scheduled on the spare verdict in the R3 fold loop
(`CasGc.cpp:417`, `scheduleMetaJob(... clearSparedMeta ...)`) and completed by `meta_pool->wait()`
(`CasGc.cpp:513`) BEFORE the round's single `gc/state` `casPut` (`CasGc.cpp:530`). A deposed leader that
clears then loses the CAS leaves a durable **stray-Clean** meta over a still-`delete_pending` blob;
stray-Clean defeats the writer's resurrect gate, so a writer dedup-reuses the exact condemned token and a
later exact-token redelete deletes the live reuse (`INV_NO_LOSS`). The RED witness
`CaRetiredInRunFoldAbortWitness` reproduces it; the isolation result proved the SOLE violation source is
the stray-**Clean** clear (stray-Condemned is safe).

## 2. Why "just adoption-couple the clear" (Fix 1) is INSUFFICIENT {#why-not-fix1}

Moving `clearSparedMeta` after the winning `gc/state` CAS fixes the *reported* witness but not all
concurrent-leader interleavings, because the **destructive** op — the exact-token redelete `deleteExact`
of a `delete_pending` entry — is itself issued **pre-CAS** by whichever leader holds that snapshot, and
the final CAS fences **adoption, not pre-CAS side effects**:

1. An OLD leader `L1` (round N) reads an adopted state with `delete_pending(h, t1)` and plans the pre-CAS
   `deleteExact(h, t1)`; it pauses before executing it.
2. A NEW leader `L2` steals the lease, folds a later `+1` that recovered `h`'s in-degree, adopts a SPARE
   of `h`, and — under Fix 1 — clears the meta post-CAS → **Clean** (a legitimately adopted clear).
3. A writer dedup-hits `h`, point-reads **Clean**, and **reuses `t1`** (no resurrect) — `h` is now live
   under `t1`.
4. `L1` resumes and executes `deleteExact(h, t1)` — deleting the **live** reuse.
5. `L1`'s eventual `gc/state` CAS fails, but the body deletion already landed. Loss.

A successful post-CAS clear cannot prove that every leader capable of deleting the old token has
quiesced, so Fix 1 cannot close this. (It would need a real stale-delete fence / deletion-claim state /
cross-object transaction — at which point it is no longer the smallest fix. The backend offers only
per-object CAS, not multi-key transactions, so Fixes 2 and 3 — a HEAD/meta re-check before `deleteExact`,
or an "atomic" re-condemn-then-clear — are also just TOCTOU narrowings, not closes.)

## 3. The fix: GC freshness metadata is ADD-ONLY {#fix}

**Rule.** GC may publish `Condemned`, and may REMOVE the meta after the exact body token is confirmed
deleted/absent (`deleteConfirmedMeta`), but it must **NEVER** transition `Condemned → Clean` on a spare.
Only a **writer** that has already displaced the body with a **fresh incarnation token** may publish
`Clean` (via `uploadFromSource` + `writeResurrectMetaClean`, and the tokenless committed-source
`copyForwardFromCondemned` clean-flip).

**Invariant.**
> Once a hash is `Condemned`, observing `Clean` means EITHER the condemned body is absent OR a writer has
> already changed its incarnation token.

This restores the exact-token delete argument in full: any stale pending `deleteExact(t1)` must either
find the body **absent** or get **`TokenMismatch`** (the body is now `t2`), because a writer that
re-referenced a condemned hash always read `Condemned` and resurrected to `t2` — it never reused `t1`.
Codex's §2 interleaving is closed at step 3: the writer never reads Clean, so never reuses `t1`.

**Change.** Remove the spare-side `scheduleMetaJob(... clearSparedMeta ...)` (`CasGc.cpp:417`) and delete
the now-unused `clearSparedMeta` helper. Keep the pre-CAS `writeCondemnedMeta` (condemn direction,
add-only, isolation-proven safe) and `deleteConfirmedMeta` (safe — the token is physically consumed when
it runs) exactly as today. No new I/O, no state-format change, no delete-path change. The `condemn_round`
guard / round-refresh the first consult proposed is **not needed** — there is no clear to race, so the
etag-ABA it addressed cannot arise.

## 4. Interactions and cost {#interactions}

- **Retired-in-snapshot unchanged.** The authoritative retirement facts are the `kCondemned` rows + the
  seal `condemned_summary`; graduation stays round-paced. The per-hash `.meta` is ONLY the writer gate,
  so a conservative `Condemned` marker with no current `kCondemned` row is safe.
- **Resurrect-supersede (`ReplacedEntry`) unaffected.** A successful resurrection / copy-forward changes
  the body token before publishing `Clean`, so any stale `delete_pending` token misses. If the hash later
  nets zero again, `ReplacedEntry` re-captures and re-condemns the current token. Multiple deposed
  leaders can only ADD `Condemned`; they cannot reopen same-token adoption.
- **No unconditional eventual-clear, and none is needed.** A spared hash may stay `Condemned` indefinitely
  if nobody writes it again — safe, because normal reads never consult the meta. The next `putBlob` self-
  heals it: `Build::observeAndAdmit` refuses same-token adoption on `Condemned` → `uploadFromSource`
  mints a fresh incarnation before attempting `Clean`; if that `Clean` CAS fails, the marker stays
  conservative and the next writer repeats. **Availability cost:** a no-source / no-evidence recreation
  path may fail-closed (writer must re-materialize from source) until a recreating writer appears; and a
  dedup-hit writer after a spare pays one full-body resurrection / verified copy-forward instead of a
  cheap reuse. This write-amplification on recurring-hash churn is the accepted cost of correctness.
- **Cost delta:** saves the spare-side meta GET + CAS per spared entry; adds no per-delete I/O; changes
  no run format, seal, `condemned_summary`, or graduation logic.

## 5. TLA+ gate plan {#tla}

Update `docs/superpowers/models/CaRetiredInRunFoldAbortWitness.tla` (+ `.cfg`, `run_foldabort_witness.sh`)
so BOTH adopting (`FoldRound`) and deposed (`FoldAbort`) folds treat GC meta writes as **add-only**:

- a condemn or pending result may set `meta[b] = "cond"`; a spare leaves `meta[b]` unchanged;
- a successful exact-token deletion may model the meta as absent/clean (the matching body is already
  absent);
- ONLY a writer fresh-upload / resurrection changes a present-`Condemned` hash to `Clean`, together with
  a token change.

Required results:
- the former (buggy) `CaRetiredInRunFoldAbortWitness.cfg` becomes **GREEN**;
- `INV_NO_LOSS`, `INV_NO_RETURN`, `INV_COVERAGE`, `INV_ONE_PASS` all enabled and GREEN;
- existing sabotages `inmem_token`, `attempt_reuse`, `no_pacing` stay **RED**;
- a NEW named sabotage `gc_clear_on_spare` (the old clear-on-spare behavior) stays **RED**.

**Strengthen the model** to defeat a false-green: two bounded in-flight leaders with **split actions** —
(capture snapshot) → (execute pre-CAS side effects incl. `deleteExact`) → (attempt adoption). This covers
the ordering where a winner adopts a spare before an older leader executes its stale redelete. A
`post_adoption_clear` sabotage (models Fix 1 — a post-CAS clear on the adopting branch) must be **RED**
under this model — this is the config that distinguishes Fix 4 from Fix 1 and prevents re-introducing the
subtly-broken clear. (Note: the second consult is consolidating these gate changes; this spec records the
target; align the implemented model to whichever the gate work lands, keeping every required result
above.)

## 6. Implementation units (TDD-sized) {#impl}

All under `src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/`. No meta format change.

1. **Rewrite `SpareClearsMeta`-style unit test → add-only** (`src/Disks/tests/gtest_cas_*`): an adopted
   spare leaves the marker `Condemned`; add a writer follow-up proving the next dedup attempt displaces
   the body, changes its token, then publishes `Clean`.
2. **Two-leader regression test:** pause an old leader after it plans `deleteExact(t1)`; let another
   leader adopt a spare for the same hash; assert the meta is still `Condemned`; let a writer resurrect to
   `t2`; resume the stale `deleteExact(t1)` and assert `TokenMismatch`, body `t2` present, no dangling
   reference. (This is the executable form of §2 — it must FAIL on the old clear-on-spare code and PASS
   after the removal.)
3. **Remove the clear** (`Core/CasGc.cpp` R3 spared branch, ~`:417`): delete the spare-side
   `scheduleMetaJob(clearSparedMeta)` and the now-unused `clearSparedMeta` helper; keep pre-CAS waiting
   for condemn writes and delete-confirmed cleanup. Update the protocol comment block and the fold's
   meta-ops documentation to state explicitly that **GC never publishes `Clean`** (add-only), citing the
   finding report.
4. **TLA gate** (§5): add-only `FoldRound`/`FoldAbort`, split-action two-leader model, run
   `run_foldabort_witness.sh` GREEN; `gc_clear_on_spare` + `post_adoption_clear` sabotages RED; existing
   sabotages RED.
5. **Report + ROADMAP closeout:** flip `reports/2026-07-11-cas-deposed-leader-stray-clean-meta.md` to
   FIXED with the gate results and the "Fix 1 insufficient — add-only chosen" addendum; ROADMAP
   `TODO (HARD)` row → DONE.

Execution order: **TLA gate first** (prove add-only green + `post_adoption_clear`/`gc_clear_on_spare` red
before coding), then test 1 → test 2 (RED) → code unit 3 (GREEN) → closeout. Land the model + code in one
change set so the witness never sits RED on the branch.

## 7. Global constraints {#constraints}

Allman braces; never use `sleep` to fix races; wrap SQL/class/function names in `code`; "exception" not
"crash"; CA is pre-release → no compat scaffolding (no meta-format bump). GC must never throw on a
data-plane 404.

## 8. Validation {#validation}

TLA gate green/red as §5; `CasGc*`/`CasBlobMeta*`/`CasBuild*` unit battery green (incl. the two new
tests); a 20-min soak green (`dangling=0`, `dryrun_subset=ok`) and the CA-s3 stateless lane (`05008`
unmodified) green; the RED witness now GREEN; an S33-class concurrent-leader scenario re-run.
