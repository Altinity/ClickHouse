# Ref-rework adjacent findings register — 2026-07-28

Companion to `specs/2026-07-27-cas-ref-chain-complete-cut-design.md` (v9 core). Eight adversarial
review rounds plus a blinded consult attacked the whole surface and surfaced defects that are REAL
but are NOT the LIST-incompleteness blocker — most exist in today's code. They were being folded into
the spec until the second scope intervention; they live here instead, each with its evidence and its
own disposition. Full findings: `tmp/codex_r1_findings.md` .. `tmp/codex_r8_findings.md`,
`tmp/codex_simplify_design.md` (persist those with this file before the stand's `tmp` is cleaned).

Legend: **[today]** = exists in current code, independent of the rework; **[rework]** = only
matters once the core lands; severity is the reviewer's.

---

## R1. Verbatim-file rebirth aliasing — [today, major] {#r1-verbatim-alias}

Verbatim files are keyed `{namespace, file_name}` only (`CasLayout.h` ~:175; `CasPlainObjects` has
no incarnation parameter). Rebirth is gated by the `_cleanup` marker, whose "physically empty" proof
comes from LIST — a hidden old-life file survives into the reborn namespace and can be read as its
own (r6 finding 1, r8 finding 3). Reads are deliberately not fence-gated, and `namespaceFilesReadable`
is a separate pre-check with a TOCTOU before the later read (`ContentAddressedMetadataStorage.cpp`
~:1234). Direction: qualify the file layer by incarnation, or add a read-side life gate — its own
small spec; until then the core keeps today's `_cleanup` gate for files and states the weaker
read contract (stale-or-`NotFound`, never alias for the REF layer only).

## R2. Writer cleanup duties and unconditional build retirement — [today, major] {#r2-writer-cleanup}

`~PartWriteTxn` retires the build unconditionally (`CasPartWriteTxn.cpp:119`) while a grant may be
wedged-Unresolved (r3 blocker 3), and staged-body cleanup swallows failures (`:1438`) with GC as the
documented backstop — current-epoch manifest bodies leak until the epoch seals (r5 finding 5,
r6 finding 5). Direction: an in-memory duty queue retried while the mount lives + the successor-seal
path for crash remnants, and "do not retire a build while an owner-grant outcome is uncertain"
(the core's every-attempt rule gives the primitive). Lands with R3.

## R3. Orphan-blob reclamation (nomination path) + S42 sweep rework — [today, major] {#r3-nomination}

The sweep deletes manifest bodies and deliberately emits no blob deltas (`CasOrphanManifestSweep.h`
~:41): blobs of a swept manifest have no in-degree row and are never condemned — a permanent leak
class visible today (r6 finding 9). The S42 defect (sweep strands folded `+1` edges) is the same
component. Direction, agreed across r7-3/r8-6: exact-GET and decode the manifest FIRST; feed its
`BlobRef`s through a NEUTRAL nomination input (bypassing B2 ordinals and unmatched-remove accounting
— a synthetic `BlobDelta` would corrupt both, `CasBlobInDegree.cpp` ~:591); adopt nominations in the
round's `gc/state` CAS; only then exact-token-delete. Death-after-adoption leaves a manifest leak
that is "safe to retry when rediscovered" (not a guaranteed retry — r8 finding 6). Manifest keys are
immutable monotone identities; a different token at the same key is illegal ABA → retain + surface.
One coherent sweep change together with the core's §6 deletion premise.

## R4. REBUILD condemnation and the build/upload registry — [today, blocker-class] {#r4-rebuild}

Today's REBUILD condemns every physically listed blob absent from a LIST-derived manifest/build edge
set (`CasGc.cpp` ~:2739): a hidden live manifest plus a listed blob condemns acked data (r5
finding 4). The core makes REBUILD condemn-nothing; the consequence — REBUILD cannot reclaim
manifest-less orphan blobs — is permanent until an authoritative build/upload registry exists
(r6 finding 9). Registry = future work; the manifest-less-blob residual is a NAMED leak.

## R5. Decommission duties — [rework, same-rollout dependency] {#r5-decommission}

`2026-07-13-cas-pool-member-decommission-design.md` discovers namespaces by scoped LIST
(`CasDecommission.cpp` ~:116) and can retire a server-root slot while a hidden `Removing` namespace
still needs its only legal sequencer (r7 finding 6). Required changes, in the same rollout as the
core (r8 finding 4): after claiming the victim, enumerate its catalog entries EXACTLY; `Removing`
without a terminal record is resumable writer work — `_ckpt` present → recover and append the
terminal under the claimed fence; `_ckpt` absent → the finalization window after cleanup →
exact-CAS-remove the catalog entry, else corruption; a final exact catalog GET/token check before
slot retirement; retirement forbidden while any entry owned by that root remains.

## R6. Wedge autonomy — [rework, note, accepted] {#r6-wedge}

The core's wedge retry is demand-driven: a permanently quiet wedged namespace retries on its next
caller or an independently occurring remount (r8 finding 8). Accepted as-is: the unresolved
operation was never acknowledged. Recorded so nobody later "fixes" it with a background
deadline-resetting loop (refused in r7 finding 8).

## R7. Probe A gating policy — [DECIDED and EXECUTED, Stage A task 12] {#r7-probe-a-gating}

`todo-20260726.md` §0's open decision (should probe A gate a soak) is answered: it gates NOTHING.
Probe A is a sampled store-quality detector — deterministic cadence (`PoolConfig::gc_probe_a_period`,
default 16), durable `due`/`performed`/`skipped` observability on the `ref_list_probe` phase row and in
`CasGcProbeA*`, aborting no round and recording no anomaly. The round enumerates `cas/refs/` once; the
second enumeration is the detector's own, on the rounds it samples. Reasoning and evidence:
`2026-07-28-stage-a-retirement-verdicts.md` (item 1). The mount-time LIST probe (#23) is the store GATE
and remains separate work.

## R9. Never-born namespaces have no late-PUT fence — [final review M4, fail-closed retention] {#r9-neverborn-fence}

The recovery walk deliberately skips sealing dead epochs of non-Live streams (`CasRefLedger.cpp:768`
vicinity), so an ambiguous BIRTH PUT that lands very late — after a remount and a successful re-birth
at a higher epoch — produces a two-birth stream that permanently HOLDS the namespace
(`UnconsumedSealCrossing`). Never loss: the hold is fail-closed retention plus suppressed destruction.
Stage B incarnations close it structurally (an incarnation-keyed birth cannot collide with a dead
predecessor's straggler). Until then it is a named residual: a namespace wedged this way stays held
until an operator intervenes. Owner: Stage B incarnation work; found by the Stage A final
whole-branch review (finding M4).

## R8. Register hygiene {#r8-hygiene}

The eight rounds' findings files and the blinded consult's design live under `tmp/` — copy
`tmp/codex_r{1..8}_findings.md` and `tmp/codex_simplify_design.md` into
`docs/superpowers/reports/2026-07-28-ref-rework-reviews/` before the stand's `tmp` hygiene sweep, so
the evidence trail survives (the `{#leak-repro-lost}` lesson).
