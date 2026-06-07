# CA Merkle-store spec — distributed-systems / S3 correctness review, round 2 (2026-06-07)

Second adversarial pass over `docs/superpowers/specs/2026-06-07-ca-merkle-store-design.md` after D6 was cut and
orphan reclamation became a condemn-not-delete reconcile. Also evaluated the proposed §4.5 merge-sort /
"seen-marker" refinement as the intended final form. Method: Lamport happens-before, state analysis, failure
injection.

## Verdict {#verdict}
**SOUND WITH MUST-FIX.** Condemn-not-delete is genuinely safer than the cut D6 and closes the prior round's
Findings 1/2/4 — there is no `Retention`-gated *deletion* anywhere now; quiescence + in-degree + `e+2` + fence is
the sole authority. The core gate survives every schedule **provided one implicit ordering rule is made
normative**. The proposed `LIST ⋈ snapshot` refinement must **not** be adopted as written.

## Rulings on the named claims {#claims}
- **(1) condemn-not-delete is safe: TRUE-WITH-CAVEAT.** Two independent gates must both fail for loss (reconcile
  wrongly marks reachable-as-orphan, AND the R4 fold independently agrees in-degree 0). For an object reachable
  from a *durable* ref, the narrative §4.5 (walk `refs/`) cannot fail gate 1 — it reads the durable, written-last
  ref directly; the dead-writer / unfolded-`+` worry is irrelevant because reconcile reads the ref, not the fold.
  Caveat: the spec never states the temporal order of the `LIST` vs the `refs/` read; must be **read-refs-last**.
- **(2) `LIST ⋈ snapshot` ≡ reachability-from-`refs/`: FALSE.** The snapshot is a "sloppy filter" (§5) that can
  drift/tear; you cannot claim it is both sloppy and an exact reachability oracle. No *loss* schedule found (the
  durable `+` co-folds with the seen-marker and out-votes it), so the refinement is *safe* but **strictly weaker**:
  it destroys the snapshot-rebuild property §4.4/§7 depend on (you cannot rebuild a torn snapshot by consuming it
  as the merge input). Fix: merge against `refs/`-reachability (sorted spill), not the snapshot.
- **(4) age-filter is perf-only, not safety: TRUE.** With age=0 (condemn 1-second-old in-flight uploads), worst
  case is wasted re-upload + generation roll — no loss schedule found, because a live writer pins `safe_epoch ≤
  O_W ≤ E` (blocks `safe_epoch > E`) and a durable `+` blocks in-degree. Rests on the D1 self-fence (CE-4) and
  flush-`+`-then-advance. Precise phrasing: "fail-safe toward leak (never toward loss)" in both directions.

## MUST-FIX findings {#must-fix}
1. **Read-`refs/`-after-`LIST`; reclaim gate re-reads in-degree from the *current* fold, not a snapshot frozen at
   reconcile time.** Keeps the two gates independent so a ref published mid-scan can't slip both. Add both
   orderings to §4.5 normatively.
2. **Reject `LIST ⋈ snapshot`; merge against `refs/`-reachability (sorted spill).** Per ruling (2). Preserves the
   snapshot-rebuild + defense-in-depth that recovery and torn-snap rely on, and is still streaming/`O(1)`-memory.
   The "zero new safety argument" claim is false — swapping the authoritative root (`refs/`) for a derived cache
   (snapshot) as the merge input *is* a new argument, and it fails on §7's own torn-snapshot row.
7. **Add the `epoch` znode to the §4.4 recovery purge set.** A backup-restored ghost `epoch` with a stale-high
   value lets a writer read `O_W > E_cur` before the leader's S3-first refresh — Keeper *ahead* of S3, the one
   forbidden direction. §4.4 purges `leader/`/`writers/` but not `epoch`.

## SHOULD-FIX {#should-fix}
3. **Seen-markers must be idempotent** (`(node,gen)`+`event_id`-keyed, folding to an indicator, never additive),
   else two overlapping reconciles double-count to a phantom positive in-degree → orphan leaks forever.
4. **Cascade reads child edges from the immutable `condemned/<e>` record (or before the tree DELETE), never
   after.** §4.2 R4 is silent on the ordering. (No loss found — per-edge accounting protects a shared sibling —
   but state it.)
8. **§4.2 R3 empty-writer branch (`safe_epoch := E_cur`) needs a `sync`-ed membership read.** Currently only in
   the obsolete R3 review, not the spec body.

## Could not break {#cannot-break}
Condemn-not-delete vs cut-D6 (strictly safer); age=0 (no loss, only re-upload); shared-blob sibling under cascade
(per-node in-degree + DAG-acyclicity protect it); seen-marker rescue by a durable `+`. Each rests on
flush-`+`-then-advance + the closed-epoch fold barrier + the D1 self-fence.

## TLA+ gaps {#tla}
The model gives **zero** evidence for the reconcile path: no `Reconcile`/seen-marker action, and its `Retention=1`
guard is subsumed by the `e+2` limbo so it tests nothing. Add: a `Reconcile` action (condemn in-degree-0
non-reachable nodes, never delete); `RetentionEps=0` with reconcile enabled (model-check claim 4); a `seen`
marker variable + fold rule; torn/lost-snapshot under reconcile (show the reachability-walk spares a live node
and the `⋈ snapshot` refinement near-misses); ghost-`epoch`-on-recovery; cascade reading children post-delete.

## Disposition {#disposition}
Resolution adopted in discussion: stream the merge (Milovidov's `O(1)`-memory requirement) but against the
`refs/`-reachability closure spilled sorted (this Finding 2), with read-refs-last + current-fold reclaim gate
(Finding 1) and the `epoch` purge (Finding 7). SHOULD-fixes + TLA+ gaps folded into the spec / §11 / §10.
