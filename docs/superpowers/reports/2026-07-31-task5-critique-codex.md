# Codex critique of the rewritten Task 5 (2026-07-31)

Verdict: the rewritten Task 5 is NOT yet a sound derivation of INV-3. Three blocking problems, plus
nine silently dropped obligations. Recorded verbatim in substance; see the RemovalReady proposal for
the author's own replacement.

## Blocking 1 — cursor pruning is not monotone across crash-resume

Stop after the pruned seal is durable but before `_ckpt` or entry deletion. The catalog still contains
`Removing`. A later GC round adds every `Live`/`Removing` catalog entry to `walk_targets`
(`Gc/CasGc.cpp:1982-1998`) and writes a cursor for every walked target (`Gc/CasGc.cpp:2557`). The
name-keyed cursor is therefore reintroduced, and the resumed removal driver has no safe-and-live
choice: trusting the historical pruned seal deletes the entry while the CURRENT seal carries a cursor,
and revalidating the current seal can wait forever because every new round may add it again.
Consequence: killing the incarnation-scoped cursor is premature, and the Step 2 crash test must run at
least one further GC round after the stop.

## Blocking 2 — the stale-resume death argument contradicts the spec AND an existing model

The spec still mandates capture-at-deposition and "a resumed pass NEVER re-derives it"
(spec `:159-163`), and `docs/superpowers/models/CaRefNsCleanupStaleLeaderCore.tla:25-44` CONSTRUCTS the
interleaving the rewrite declared unconstructible: a leader captures an item, stalls, removal retires
and deletes the entry, the name is reborn, the stale leader resumes; re-deriving then targets the live
incarnation and violates `NoLiveDataDeleted`. Ordering controls durable lifecycle state; it does not
revoke a running actor's local copy of an older seal. Keep capture-at-deposition, its capture-time
test, and at least one stale-leader-after-rebirth data-loss test.

## Blocking 3 — Step 7's two tests demand incompatible classifications

After `_cleanup` dies, these two worlds are observationally identical: a legal removal (entry absent,
canonical `<ns>/<inc>/…` debris survives — INV-3 explicitly permits this, spec `:79-81`) and damage or
catalog loss (entry absent, the same canonical key survives). "Old-incarnation-shaped" does not
separate them, because with no entry there is no current incarnation to be old relative to. So
"an ordinary removal raises NO anomaly" and "a fabricated entry-less canonical key raises one" cannot
both hold. Three resolutions exist: treat the catalog as fully authoritative and delete the
fabricated-missing-entry requirement; retain finite exact removal evidence and bound it despite
incomplete LIST; or keep the entry until a proof exists, which conflicts with the no-physical-empty
design.

## Silently dropped obligations — the failure mode of a rewrite

1. Unauthorized terminal-append negative test (a happy-path owner test cannot catch removal of the
   fence check).
2. The capture-at-deposition test itself — kept as a requirement, given no test.
3. `NamespaceRemovalDoesNotListOrDeleteFiles`: no `_files` LIST, no `_files` delete, entry deleted while
   files still exist. This was the EXECUTABLE proof that removal has not regained a physical-empty
   dependency.
4. Bounded cleanup failure is leak-only and must not block lifecycle completion.
5. Janitor suppression: a janitor that deletes during a suppressed round passes every test the rewrite
   names.
6. Exact-CAS entry removal against the complete observed row, not "catalog entry deleted".
7. The non-`nsc` capacity terms (`btr` per run segment, `cnd` per gc-shard) — untouched by ordering, so
   they did not die with the Σ decision.
8. Tests for the `namespaceAllLivesPrefix` helper, including concept-negative and malformed-key
   boundaries.
9. The execution gates and the commit step.

## Test-by-test: would it fail on regression?

- Step 1 — passes for the wrong reason: final catalog byte-equality permits mint-then-delete. Pin ZERO
  catalog mutation in the operation journal, and exercise `listRefs`, `resolveRef`/DROP DETACHED and
  table removal separately.
- Step 2 — a journal can show the seal PUT before the entry CAS even if `gc/state` never adopted that
  seal. Assert the DURABLE state points at the pruned seal.
- Step 4 — passes for the wrong reason: an `Atomic` fixture silently gets a fresh UUID, so it never
  tests same-name rebirth. Pre-install a nonzero predecessor cursor, reuse the writer epoch, prove the
  same `RootNamespace` was reused, and interleave a post-prune GC round.
- Step 5 — prove the same runtime object stayed resident, and cover BOTH life-assignment writers.
- Step 6 — add a suppressed round that deletes nothing, and assert bytes were actually deleted in the
  only-`_files` case.
- Step 8 — plant an old-life file and prevent its cleanup, else reads may be absent because the object
  vanished rather than because the predicate uses catalog state.
- Step 9 — a counter firing on round 1 is still fired at round N: assert no signal through N−1.

## Step 3's option pair is not exhaustive

A third policy: bounded CREATE-side assistance — `CREATE` meeting `Removing` drives the same targeted
lifecycle under the normal lease/state protocol within the DDL deadline, then returns the typed
retry-later error if it cannot finish. `DROP` keeps its semantics, the cost is paid only when exact-name
reuse is actually requested, and UUID-derived names pay nothing. It may not bypass the durable
terminal/retirement/prune sequence.
