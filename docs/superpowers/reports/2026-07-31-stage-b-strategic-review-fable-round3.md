---
description: 'Round 3 of the Stage B strategic review: review of spec commit c5a6243a325, acceptance of the RESTORE and debris-phrasing corrections, concession on Ordinary, and the full obligation trace for the replacement Task 5'
sidebar_label: 'Stage B strategic review round 3 (Fable)'
sidebar_position: 102
slug: /superpowers/reports/stage-b-strategic-review-fable-round3-2026-07-31
title: 'Stage B strategic review — round 3, Fable'
doc_type: 'reference'
---

# Stage B strategic review — round 3 (Fable) {#stage-b-round3}

Written 2026-07-31 against spec commit `c5a6243a325` and the current tree. This round has no second
reviewer to check it: the other reviewer stopped producing output, so uncertainties are flagged
inline rather than smoothed over — see §5 for the two places I would have wanted attacked.

## 1. The spec edit: faithful, with three flags {#spec-review}

The four substantive changes render the arguments correctly. §1's cut-vs-scan paragraph, the INV-3
ordering-as-invariant text (including "coexisting lives … unrepresentable **in GC-held state**" —
correctly scoped, since physical debris of old lives still coexists and is the janitor's), the
over-covering asymmetry, and the §3 structural-`Creating` decision are all accurate. The
alternatives-table rows state the bound argument correctly, including the point that UUID churn does
*not* bound a `Retired` state — every fresh UUID also leaves a permanent row — which is stronger than
what I wrote in round 2, and right. Three flags, all wording-level:

1. **A stale sentence now contradicts INV-3.** The pre-existing `seq_floor` rejection row in §10
   still ends "Incarnations make debris inert WITHOUT a physical-empty proof, **so entries delete
   immediately**." Entries now delete LAST, gated on item retirement and a pruned seal. The spirit
   survives (the gate is bounded GC bookkeeping, not a physical-empty proof), but "immediately" is
   now false as written; suggest "so entries delete as soon as removal completes, with no
   physical-empty proof".
2. **"Temporary and self-clearing" hides a magnitude.** The refusal window is now terminal-fold +
   cleanup pass + retirement observation (a *later* round) + entry delete — plural GC rounds, not
   seconds. That was fine while same-name rebirth was an edge case; the same commit makes `Ordinary`
   supported, under which same-name rebirth "stops being rare and becomes ordinary" (the spec's own
   words). So a UUID-less `DROP`+`CREATE` waits multiple rounds, and the spec should own that
   magnitude where it states the refusal — or Task 5 should record the mitigation decision (an eager
   targeted fold of a `Removing` namespace's tail driven by the DROP path, versus accept-and-document).
   This is the one place my retracted round-2 urgency legitimately returns, narrowed to the UUID-less
   layout.
3. **`_cleanup`'s death is nowhere in the spec.** INV-3 still says (in the superseded-but-standing
   clause) "verbatim FILES … keep today's `_cleanup` gate", and nothing records that the marker class
   is being deleted. If Task 5 deletes it — it should — record that in the spec too, or the invariant
   text licenses a future reader to re-wire it.

## 2. The two corrections: accepted {#corrections}

- **RESTORE:** accepted plainly. Stock `RESTORE` coordinates fresh UUIDs, so restore-from-backup is
  not a route to the same `RootNamespace`; my round-2 route list was wrong there, the conclusion
  stands on shadow-by-construction and explicit-UUID replay, and the spec's three-route paragraph
  (which lists UUID-less paths, not restore) is the correct list.
- **"Physical debris":** accepted. The incarnation qualifies `_log`/`_snap`/`_ckpt` and `_files`
  only; manifests keep their `(namespace, mount-epoch, build-sequence)` identity and loose mountpoint
  objects are unchanged. Namespace-file debris alone carries the argument, and the spec's precise
  scoping is the right text — my phrase was over-broad and should not be inherited.

## 3. The `Ordinary` decision: I concede, verified {#ordinary}

I checked the decisive fact myself: `src/Databases/DatabaseReplicated.cpp:1602-1604` — *"We use
Ordinary engine for destination database, because it's the only way to discard table UUID"*, in the
broken-tables path of replica recovery. The ban would break that path. My "ban regardless"
recommendation rested entirely on the identity model leaning on UUID freshness; with the incarnation
kept, that premise is gone, and the spec's reframe — the UUID-less layout as a reason to *exercise*
the rebirth path in tests — is the correct disposition. One consequence to carry forward: this
promotes the rebirth path and the refusal window (§1 flag 2) from edge case to mainstream for that
layout.

## 4. Replacement Task 5: the obligation trace {#task5-trace}

Traced from the current Task 5 section (plan `:1106-1315`). Your survivor list — R12 non-minting,
cursor prune-before-entry-delete, `rt->life` invalidation, the janitor, anomaly discrimination,
deleting the `_cleanup` marker class — is right and nothing on it should die. It is missing seven
things, and two obligations you did not list can die.

### 4.1 Missing — add these {#missing}

- **M1. The `Removing`-without-`_ckpt` window ownership** (`:1151-1157`): the removal driver resumes
  entry deletion on the owning writer's next mount, idempotently, never re-creating `_ckpt`; the
  dead-root branch stays Task 7's. Task 5b's refuse-to-ground is only safe because this window has an
  owner — dropping it from the rewrite orphans 5b.
- **M2. Stuck-removal surfacing** (`:1139-1141`): GC observing a terminal-less `Removing` for N
  rounds surfaces it durably (counter + log) and appends nothing. It is the only observability for a
  wedged removal.
- **M3. Retry-later refusal, with an owner.** The spec now mandates "reported as retry-later, never
  as an internal error", but `createNamespace` today throws `LOGICAL_ERROR` on *any* existing entry
  (`Pool/CasRefCatalog.cpp:236`). Converting recreate-during-removal into the typed retryable refusal
  — with a message naming what it waits for — is a code obligation only Task 5 can take.
- **M4. The rebirth tests survive the re-key's death.** The same-epoch rebirth regression test
  (`:1240-1242`) now pins the *ordering* rather than a key format — keep it. Add an end-to-end
  same-name rebirth test through a UUID-less/shadow-shaped name, per the spec's new testing reframe;
  `Atomic`-based suites will never exercise it naturally.
- **M5. The janitor's no-`Pending` case** (`:1210-1224`): a removed namespace whose only residue is
  `_files` promotes to `Completed` on the first fold, so the janitor cannot key off `Pending`. "The
  janitor" as a list item loses this sub-case easily; it is the whole leak interval Task 4b opened.
- **M6. `_cleanup` has consumers, not just a key.** `namespaceIsRemoved` gates its recreate-flip on
  the `_cleanup` marker (`Pool/CasPool.h:500-508`) and `dropNamespace` publishes the constant-size
  `Removed` snapshot (`:495-497`). "A dropped table's files read absent" must be re-answered from
  catalog state (`Removing`, then entry-absent) before the marker dies, or the reader-side predicate
  is orphaned mid-rewrite.
- **M7. Three hygiene riders**, one checklist line each so they survive the rewrite: delete the false
  `per_ns_shard` comment (`:1260-1263`); delete 4-C's accepted-cost comment (`:1275-1276`); carry the
  "do not test frees-at-`Removing`" note (`:1280-1285`).

### 4.2 Dies — beyond the cursor re-key already killed {#dies}

- **D1. The deposited-incarnation machinery shrinks to its cheap half** (dies *because of the
  ordering*). Under entry-delete-LAST it becomes derivable: an unretired cleanup item implies its
  entry still exists (retirement precedes deletion), so a resumed pass re-deriving from the catalog
  would get the *same old* incarnation — the reborn-life data-loss race is unconstructible by
  ordering. Keep capture-at-deposition as the implementation (cheap, and TLA-proven in
  `CaRefNsCleanupStaleLeaderCore`), but the elaborate stale-resume-vs-reborn-life test family
  (`:1166-1186`) collapses to one test asserting the ordering makes the race unconstructible.
- **D2. The Σ-index-set decision and its worst-case test die** (ordering + the spec's over-covering
  reservation). "Two `nsc` rows for one namespace" and "an `nsc` row with no catalog entry"
  (`:1286-1300`) both require a rebirth before the first removal retired — unconstructible under the
  ordering. The over-covering constant supersedes the exact decision; replace the worst-case test
  with an anomaly if GC ever observes an entry-less `nsc` row, since that now indicates a broken
  invariant rather than a budget case.
- **D3. Anomaly discrimination: keep the requirement, re-derive the evidence set** (partially dies —
  for another reason: its named evidence is being deleted). The landed bullet (`:1265-1270`) names
  "the terminal record / `_cleanup` marker" as removal evidence — half of that is dying with
  `_cleanup`, and under the new ordering an *ordinary* removal may produce no un-cataloged transient
  at all (cursor pruned and item retired before the entry goes; entry-less ref-layer debris is
  old-incarnation-shaped, i.e. the janitor's, not the anomaly's). The discrimination likely collapses
  to "entry-less key: old-incarnation-shaped → janitor; current-shaped or terminal-less → anomaly".
  Design it from the new ordering rather than porting the old two-evidence rule.

### 4.3 Confirmations against the tree {#confirmations}

R12's ref-layer half is still open: `dropNamespace`, `listRefs`, `resolveRef` all reach
`ensureRefTableRecovered` (`Pool/CasRefLedger.cpp:282,345,366,1460,1496`), which mints; the existing
pin `RemovalOnANeverOpenedTableLeavesTheCatalogUntouched`
(`src/Disks/tests/gtest_cas_namespace_file_request_profile.cpp:512`) covers the `_files` arm only.
The plan's red-first detector — add `removeRecursive(kTablePath)` to that test — is still the right
first failing test for the operation-level closure and is worth carrying verbatim.

### 4.4 The smallest ordered step list for the new task {#steps}

Each step with what its test must pin — the ordering, not a key format.

1. **R12 operation-level non-minting** (reads, removals, `DROP DETACHED` arm). Test pins: a removal
   or read of a never-opened table leaves the catalog byte-identical — the extended
   `RemovalOnANeverOpenedTable…` detector, red first.
2. **Removal sequence**: `Live → Removing` CAS → fenced terminal record (owner or fenced successor
   only) → fold → one bounded, suppression-aware cleanup pass → item retirement + shard-0 cursor
   pruned in a durable seal → `_ckpt` exact-token delete → entry delete last. Test pins: the ORDER,
   asserted via the backend op journal; a kill between any two steps resumes idempotently (M1),
   and no step re-creates `_ckpt`.
3. **Retry-later rebirth refusal** (M3). Test pins: `CREATE` of a name in `Removing` (or with an
   unretired item) receives the typed retryable error naming the wait — never `LOGICAL_ERROR`; after
   the entry is gone, the same `CREATE` succeeds and mints a fresh incarnation.
4. **Rebirth folds from its own beginning** (M4). Test pins: a fresh incarnation reusing low
   sequence numbers within one writer epoch is folded from `{0,0}` — i.e. cursor pruning happened
   before entry deletion, whatever the key format; plus the end-to-end UUID-less/shadow-shaped
   same-name rebirth test.
5. **`rt->life` invalidation on entry removal.** Test pins: drop and rebirth **without** an LRU
   eviction in between — the warm-runtime case; the writer uses the NEW life (an eviction-first test
   passes for the wrong reason, plan `:1206-1208`).
6. **Janitor** (M5 included). Test pins: foreign-incarnation debris under a known namespace is
   deleted under `suppress_destructive` rules by exact token; a token mismatch retains and surfaces;
   and a removed namespace whose ONLY residue is `_files` is reclaimed even though its item promoted
   to `Completed` on the first fold.
7. **Anomaly discrimination, re-derived** (D3). Test pins both directions: an ordinary removal —
   through the full new sequence — raises NO un-cataloged anomaly in any round; a fabricated
   entry-less, current-shaped key with no terminal record raises one. Plus the D2 replacement:
   an entry-less `nsc` row is an anomaly, not a budget case.
8. **Delete the `_cleanup` marker class** (M6): rewire `namespaceIsRemoved`/the reader-absence
   predicate onto catalog state first, then remove the marker and its publication from
   `dropNamespace`. Test pins: a dropped table's files read absent during `Removing` AND after entry
   deletion, with no `_cleanup` object ever written (op journal); and the spec is amended to record
   the deletion (§1 flag 3).
9. **Stuck-removal surfacing** (M2). Test pins: a terminal-less `Removing` held for N rounds
   increments the durable counter and logs, and GC appends nothing.
10. **Hygiene riders** (M7), no tests — comment deletions and the retracted-claim note.

Steps 1 and 8's rewiring half are ordered first deliberately: both change what the rest of the task
observes (catalog writes, reader predicates), so every later test runs against the final semantics.

## 5. Where I am uncertain, and what the other reviewer should have attacked {#uncertainty}

1. **D1/D2 lean on "retirement precedes entry deletion" being airtight across crash-resume.** I
   derived unconstructibility from the ordering, not from a model. The old worst case ("two `nsc`
   rows, no entry") was found by a review round, not by design — the replacement claim that it is now
   unconstructible deserves the same adversarial treatment, ideally a TLA sabotage config
   (`CaRefNsCleanupStaleLeaderCore` extended with the new ordering) rather than my prose. Until that
   exists, keep capture-at-deposition (D1's cheap half) precisely because it is the belt for the
   case my derivation missed.
2. **D3's "ordinary removal produces no un-cataloged transient at all"** assumes no round interleaves
   between the seal that prunes the cursor and the entry delete in a way that re-adds the namespace
   to `walk_targets` from some other carrier. I checked the `parent_cursors` re-add path
   (`Gc/CasGc.cpp:1814-1840`) but not every carrier; the discrimination design should enumerate the
   carriers rather than trust this paragraph.

Both uncertainties are cheap to resolve inside the task (one model extension, one carrier
enumeration) and neither blocks writing the task text.
