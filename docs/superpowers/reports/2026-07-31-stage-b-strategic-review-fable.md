---
description: 'Strategic review of CAS Stage B (catalog + incarnations): verdict option 2 — keep the landed core, restate Task 5 around removal ordering, concentrate fence enforcement, cut exactness invariants, defer the non-blocking tail past the pre-release gate'
sidebar_label: 'Stage B strategic review (Fable)'
sidebar_position: 100
slug: /superpowers/reports/stage-b-strategic-review-fable-2026-07-31
title: 'Stage B strategic review — independent reviewer, Fable'
doc_type: 'reference'
---

# Stage B strategic review — independent reviewer (Fable) {#stage-b-strategic-review-fable}

Written 2026-07-31 against branch `cas-gc-rebuild` as it stands (Tasks 1–4c landed, Task 5 next).
Everything cited was re-derived against the current tree, not taken from prior review rounds.

## Verdict {#verdict}

**Option 2 — revise to minimise.** The catalog-as-universe and the incarnation are *both* worth keeping
and are already landed; the complexity the commissioner is rightly worried about is concentrated almost
entirely in the *not-yet-written* Task 5 and in three cross-cutting enforcement conventions that can be
pulled into primitives. The single strongest reason: the value-carrying decisions (Tasks 1–4c) cost
~570 lines of real code (`Pool/CasRefCatalog.{h,cpp}`), while the spread the commissioner complains
about is (a) migration scaffolding scheduled for deletion, (b) invariant enforcement distributed to call
sites by convention and prose instead of by mechanism, and (c) a Task 5 that as written supports
*coexisting namespace lives* it does not need to support. All three are fixable by subtraction, not by
rollback or redesign.

## Testing the commissioner's premises {#premises}

### "LIST lacks only cross-page snapshot isolation" — refuted by the captured evidence {#premise-list}

This premise is central and it is **wrong**, by an argument the investigation itself did not spell out.
The captured firing (`2026-07-26-list-incompleteness-investigation.md` §2): keys `…1430c`, `…1430d`
durable at `16:47:19.211–.212`, key `…1430e` durable at `.2136`; one walk returned `e` and omitted
`c`, `d`. CAS pagination resumes by *last key returned* (`forEachListedKey`, fresh `start_after`
iterator per page — investigation §4.3). For `c`, `d` to be missing from the whole walk, the page
preceding the page that returned `e` must have ended at some key `K < c`. The next page was then a
single enumeration call over `(K, …]` that returned `e` but not `c`, `d` — keys lexicographically
*before* `e`, durable *before* `e`. No assignment of per-page snapshots explains that: it is a
**within-call, non-monotonic omission**, not a cross-page isolation artifact. The store returned a page
whose own snapshot contained a later write but not two earlier ones.

What the honest reading *does* concede to the commissioner: the omission's staleness bound is unknown
downward. The walk provably ran after `.2136`, but possibly milliseconds after — so every observed hole
is consistent with "LIST can omit keys written very recently". If that recency bound were proven, a
wait-and-settle re-LIST would close the defect cheaply. It is not proven, the RustFS mechanism is
unknown (§2.5), and the design decision already on record — "the fix must hold against a store that
answers inconsistently, because we have one" (§8.2) — was made with that in view. I think that decision
is right for a product that targets arbitrary S3-compatible stores; see [what would change my
mind](#change-my-mind) for the store-model caveat.

Cheaper constructions for the *namespace-universe* instance specifically (brief Q1): a bounded re-LIST
or wait-and-retry closes nothing you can name, because the failure to defend against is a namespace
**wholesale absent** from the answer — probe A's own documented blind spot 2 — and an absent namespace
produces no out-of-order observation to retry on. A generation counter tells GC *that* it missed a
creation, not *which*; reconciling the count requires an authoritative membership record, which **is** a
catalog. The one genuinely different construction — record namespace births/terminals as ref-log records
in a well-known genesis namespace, inheriting Stage A's arithmetic completeness — is elegant and reuses
the chain machinery, but it serialises DDL through one CAS lane exactly as the catalog does (no hotspot
advantage), delays universe knowledge behind a fold, and is a redesign of landed code. Not worth it now.

### "TOCTOU is present in both designs" — true, and it misses the point {#premise-toctou}

Correct: a catalog read is as stale by act time as a LIST (`Gc::runRegularRound` reads the catalog once,
`Gc/CasGc.cpp:1379`). But the two failure modes are different in kind:

- **Staleness** (both designs): closed by protocol — `Creating` forbids publication, the
  destructive-round frontier proof, the temporal lemma's three arms, the delete-site in-degree re-read
  (spec §5). These closures *consume* a snapshot that is guaranteed complete-as-of-some-instant.
- **Incompleteness** (LIST only): a single-object token-CAS read is atomic — you get every entry as of
  one instant. A LIST can omit an existing member, and no downstream protocol can recover what the
  universe never named: the frontier proof iterates "every `Live`/`Removing` entry" and a namespace the
  answer omitted is simply not probed, so its unfolded `+1` never holds destruction.

So the catalog's advantage over `LIST` is real but is **atomic completeness, not freshness**. The plan
should say exactly that, because as motivated ("LIST is unreliable") the commissioner's narrower reading
looks like a rebuttal when it isn't one.

### The `ref_catalog` hot spot — the commissioner is mostly right, with two reservations {#premise-hotspot}

BACKLOG `{#ref-catalog-write-hotspot}`: 137 of 250 S3 timeout lines name `cas/ref_catalog`, 66
namespaces, stateless lane. Discounting this is defensible: contention is DDL-rate by design, the lane
creates tables at a rate no deployment approaches, and the entry itself says "upper bound on severity".
Two reservations, both actionable without sharding:

1. **R12 makes it worse than DDL-rate** (`2026-07-28-ref-rework-adjacent-findings.md:182`): `existsFile`
   on a never-born namespace *mints a catalog entry* — a durable CAS + `_ckpt` publish on a read path —
   and Task 5's own obligation list confirms `dropNamespace`/`listRefs`/`resolveRef` also recover-and-
   mint (plan `:1301-1315`). Reads multiplying writes on the one pool-wide object is a bug at any rate.
2. **A timeout at 66 namespaces is a tuning defect regardless of design**: `casUpdate` retries 100×
   against conflict (`Pool/CasRefCatalog.h:77-80`) but evidently converts contention into 499s rather
   than backoff. Fix the loop; do not touch the object layout pre-release.

### "Incarnation minted in one place, unchanged on lease change" — verified true {#premise-incarnation}

`createNamespace` is the only mint site ("mints a random nonzero incarnation",
`Pool/CasRefCatalog.h:165-180`; tree-wide grep finds no other). `reconcileStaleCreator` CASes only
`creator`; "`state` and `incarnation` are UNCHANGED … over the SAME incarnation, never a fresh one"
(`Pool/CasRefCatalog.h:233-236`). A stalled creator resumes via `completeCreation` on the same entry.
Confirmed as the brief stated it.

## Are the two changes separable, and which carries the value? {#separability}

They are separable, and **both carry value, for different failure classes**:

- **Catalog-as-universe** closes the data-loss branch: a hidden `+1` from a namespace the universe never
  named, versus a visible `-1` on a shared (deduplicated) blob → in-degree 0 → deletion of acked data.
  Nothing else in the system catches this — the delete-site re-check re-reads state computed *without*
  the hidden edge.
- **The incarnation** converts LIST omission and stale-actor damage from *correctness* hazards into
  *leak-only* hazards at removal/rebirth: dead-life debris is structurally inert (foreign prefix), so
  removal needs no LIST-based physical-empty proof — whose omission direction would be
  rebirth-permitted-over-surviving-debris, i.e. the R1 aliasing class, at DDL rate. It also makes a
  deposed writer's late writes inert instead of corrupting a reused prefix.

The commissioner's instinct that the incarnation's elegance "does not require LIST to be replaced" is
therefore correct — but the converse also holds: dropping catalog-as-universe while keeping incarnations
reopens the hidden-namespace branch. Keep both. They are landed; reverting either costs more than it
saves.

## Where the corner-case code accumulated, and the mechanism of spread {#spread}

Named, per the brief's Q3:

1. **Life-resolution: five mechanisms, 66 call sites across 12 files** (measured by grep, slightly under
   the brief's ~80). But the five are not five copies of one decision — they are: writer-mints
   (`CasRefLedger::resolveNamespaceLife`, `Pool/CasRefLedger.cpp:920` — a 90-line four-outcome state
   machine), read-never-mints (`lifeIfCataloged`), bulk universe (`liveUniverse`/`discoverUniverse`),
   discovery-with-fixture-fallback (`resolveLifeOrSentinel`), and a migration sentinel
   (`NamespaceLifeId::stageATransition`, `Primitives/CasNamespaceLifeId.h:107`, **already scheduled for
   deletion by Task 6**, grep-gated). Post-Task-6 the honest count is four, and `resolveLifeOrSentinel`
   exists chiefly so raw-fixture *tests* that bypass the catalog keep working
   (`Pool/CasRefCatalog.h:34-47`) — test convenience shaping a production API, collapsible into
   `lifeIfCataloged` + explicit fixture seeding. The end state is a principled trio: create / read /
   enumerate.
2. **Fence discipline by convention**: `check_fence_or_throw` + `admitted_generation` are threaded
   through every catalog mutation and `_ckpt` publish, with the "inside `mutate`, on EVERY fresh read"
   rule re-explained in prose at each site (`Pool/CasRefCatalog.h:95-107`, `:193-205`, `:238-243`;
   `Pool/CasRefLedger.cpp:945-1005`). A cross-cutting invariant enforced at N call sites by closure
   passing and comments is exactly what "spreading tumour" feels like from inside. This is the top
   concentration candidate.
3. **GC-side accretion in `Gc/CasGc.cpp` (4742 lines)**: the catalog read + `live_incarnation` /
   `creating_incarnation` split + the R10 anomaly discrimination (`:1350-1400`), the frontier-union and
   its budget (`:1814-1840`), `suppress_destructive` recomputed before every destructive site, holds
   carried in the seal grammar. Individually justified; collectively the file Task 13 already admits
   must be split.
4. **Review-round accretion as process**: the adjacent-findings register grew R9→R13 *during* Stage B
   execution; Task 5's section carries eight interlocking post-hoc obligations placed on four different
   dates (plan `:1196-1315`), including corrections of its own earlier corrections. Per the standing
   rule (≥2 new object kinds/rounds = re-derive the invariant), this is the signal that Task 5's
   invariant set — not its implementation — needs one subtraction pass before anyone writes it.

**The mechanism of spread, in one sentence:** the design centralises its *invariants* in the spec but
distributes their *enforcement* to call sites — closures, conventions, per-site gates, and prose. The
one place Stage B did the opposite — r9-3's typed `NamespaceLifeId` making incarnation-dropping
unrepresentable at compile time — is the one piece of pervasive change nobody complains about. That is
the template.

## The concentration proposal {#concentration}

Concrete, in dependency order, all inside `Pool/`:

1. **A fenced-mutation primitive** (fold into Task 6, or a small task before it): one type — call it
   `FencedCatalogWriter` — constructed from `(admitted_generation, check_fence_or_throw)` once per
   mount, owning `casUpdate`/`casAdmitEntry`/`publishCkpt` dispatch and performing the fence re-check
   inside every CAS attempt itself. Deletes the two threaded parameters from every signature in
   `CasRefCatalog`, `CasRefCkpt`, and the `CasRefLedger` call chain; the "inside `mutate`" rule becomes
   a property of the primitive with one test, instead of a convention with N paragraphs. Interface:
   `writer.update(mutate)`, `writer.admit(entry)`, `writer.publishCkpt(life, contribution)` — throw =
   fenced, uniformly.
2. **Collapse life resolution to three named entry points** (Task 6, alongside the already-planned
   `stageATransition` deletion): `CasRefCatalog::lifeIfCataloged` (read), `CasRefLedger::
   resolveNamespaceLife` (writer-mints — after R12's fix, the *only* minting path), `CasRefCatalog::
   liveUniverse` (GC/fsck/sweep/decommission). `resolveLifeOrSentinel` dies; fixtures seed the catalog
   explicitly via the existing test helper.
3. **Removal ordering as the load-bearing invariant** (Task 5, restated below) so that the
   incarnation-scoped cursor re-key and half the coexistence tests become unnecessary rather than
   implemented.

## Task 5, restated {#task5}

Task 5 as written supports *coexisting lives* in state the rest of the system holds about a namespace,
and most of its accreted obligations are the bill for that. But the plan has **already converged** on
the ordering that makes coexistence unnecessary: "delete the catalog entry only after the `_cleanup`
marker and the item's retirement are durable, and drop the namespace's shard-0 cursor from the seal when
it goes" (plan `:1271-1274`). Make that ordering the *invariant* — **no same-name entry can be created
while any GC-held state (cursor, cleanup item, seal row) for the previous life survives** — and then:

- **Cut the incarnation-scoped fold cursor re-key** (plan `:1232-1239`): with prune-before-entry-delete
  guaranteed, a rebirth cannot inherit a cursor; keep the same-epoch rebirth regression test as the pin
  on the *ordering*, not on a key format. (Belt-and-braces has a cost here: re-keying `cursorKey`,
  `parseCursorKey`, `CasFoldSeal::per_ns_shard` and every consumer is precisely the pervasive identity
  change the commissioner is objecting to, duplicated for a second object family.)
- **Keep** the anomaly discrimination + cursor pruning pair (`:1265-1274`) — the pruning is what
  prevents the measured pool-wide permanent reclamation stall, and the discrimination is cheap once
  removal evidence exists.
- **Keep** the deposited-incarnation cleanup rule and the janitor (both leak-only machinery, both
  small; the janitor is additionally the only reclaimer of dead-life `_files`, `:1210-1224`).
- **Fold R12's fix in** (`:1301-1315` already places it here): reads and removals resolve without
  minting. This is also the biggest lever on the catalog hot spot.
- **Keep** `rt->life` invalidation on entry removal (`:1196-1208`) — small, and the failure it prevents
  is nondeterministic.

**One user-visible cost to decide eyes-open, which the plan does not currently state:** under *either*
variant, the catalog entry survives until the terminal record **folds** — a GC-round latency — so
`CREATE TABLE` of a just-dropped name is refused (today `LOGICAL_ERROR`, `Pool/CasRefCatalog.cpp:236`;
Task 5 must at minimum make it a retryable refusal) until the next fold. Stateless CI lanes
drop-and-recreate names constantly; this *will* burn lanes at Task 5, not at some future scale. Decide
now between: an eager targeted fold of the removing namespace's tail driven by the DROP path, or an
aggressive round cadence in test configs plus a retryable error message that names the wait. Refusing
rebirth is fine (the commissioner's leaning, and I agree — production rebirth is rare); refusing it for
an unbounded, unnamed interval is not.

Per brief Q5: with rebirth refused until GC-held state is retired, **the incarnation remains
necessary** — it is what makes the refusal window's *physical* debris (`_files`, late writes from
deposed actors) inert without a LIST-based empty proof, which is exactly the proof whose omission
direction is a correctness hole. What collapses is not the incarnation but the *coexistence machinery*:
cursor re-keying, the stale-cleanup-resume-races-a-reborn-life test family shrinks to the janitor's
foreign-prefix case, and `rt->life` staleness downgrades from corruption to leak.

## Invariants to cut or weaken {#invariants}

| invariant | what it buys today | what breaks without it | ruling |
|---|---|---|---|
| Byte-exact capacity reservation (worst-case cursor/cleanup/hold arithmetic incl. escaping/framing/trailer growth, boundary+1 tests; the Σ-index-set decision at plan `:1286-1300`) | the fold-seal PUT is never refused mid-round | nothing, if replaced by a *generous over-covering* bound (count × conservative constant, hard namespace cap ~10⁴); the plan's own asymmetry analysis says over-charge | **Weaken.** Keep the direction (over-cover, refuse admission loudly), delete the exactness and its test battery |
| `Creating`-forbids-publication as a separate write-path check (`checkPublicationAdmittedOrThrow`, unwired — `Pool/CasRefCatalog.h:259-268`; Task 4 "owns closing the gap") | a second fence on a path already fenced | nothing observable: a production writer cannot obtain a life while `Creating` — `resolveNamespaceLife` loops until `Live` and is the sole life source post-R12 | **Cut** the production-path obligation; state the structural argument where the unwired function is documented; keep the function for the removal/`Removing` refusal in Task 6 |
| Incarnation-scoped fold cursors (Task 5 obligation) | rebirth safety independent of removal ordering | nothing, once prune-before-entry-delete is the pinned invariant (above) | **Cut**, keep the ordering test |
| `_ckpt` conflict-is-corruption join (R13, `adjacent-findings.md:149`) | detects true divergence | already known to wedge ordinary operation with two honest contributors | **Weaken per the register** — semantic-max merge, corruption only on genuinely incomparable fields |
| Janitor exact-token deletes on *foreign-incarnation* debris | second belt over an already-inert prefix | a mis-token delete of an object the prefix already proves dead | **Keep** (cheap), but do not grow it: retained-and-surfaced on mismatch is enough |
| Hold grammar strictness, REBUILD-refuses-on-missing-seal, frontier proof, delete-site in-degree re-read | each closes a named data-loss arm (spec §5) | acked-data loss | **Keep, untouchable** |

## What to keep from what is landed, and what to revert {#keep-revert}

**Keep all of Tasks 1–4c. Revert nothing.** Specifically: the typed `NamespaceLifeId` and deleted
namespace-only overloads (Task 1/1c — the best-executed piece of the phase), the catalog object and its
lifecycle primitives (Tasks 2–3), universe-from-catalog and the re-keys (Tasks 4/4b), `_ckpt`
strengthening (4c). The only landed item I would consciously *degrade* is the capacity-exactness
machinery of Task 2 — and even there, degrade opportunistically when Task 5 touches the predicate (the
Σ-index-set decision forces a rewrite anyway), not as its own revert.

## Cost, against the pre-release deadline {#cost}

The chain that destruction enablement irreducibly needs, in any design: **5′ → 5b → 6(+concentration)
→ 6b → 7 → 7a → 7b → 11**. That is 8 units of the remaining 15, with Task 5′ visibly smaller than
Task 5 as written (cursor re-key gone, coexistence test family halved, capacity exactness weakened) and
Task 6 slightly larger (absorbs the fenced-writer primitive and the resolution collapse — both
mechanical once stated).

**Defer past the pre-release gate, with named placement** (a ledger line is not placement): Task 8
(R2+R3 sweep rework — not in Task 7b's dependency list, plan `:206`), Task 9 (doc-only), Task 12 (perf
research), Task 13 (file split), and Task 10 except any model a Task 5′ obligation actually consumes.
Each gets a post-release milestone owner in the BACKLOG, not just an entry.

Net: roughly **half the remaining plan ships pre-release**, the deferred half is genuinely severable,
and nothing shipped carries a known data-loss arm — the suppressed-destruction posture
(`UniversePolicy::kDefault`, `Gc/CasGc.h:54`) stays until 7b exactly as planned.

## What would change my mind {#change-my-mind}

1. **A store-model commitment.** AWS S3 has documented strong read-after-write LIST consistency
   (since 2020), and MinIO claims the same. If the product line commits to *only* such stores and treats
   RustFS as a test-stand artifact, the catalog-as-universe motivation drops from "closes a data-loss
   branch" to "defense in depth against an undocumented store", and Option 1 (keep incarnations, revert
   universe authority, keep LIST + probe A fail-closed) becomes arguable. The spec chose the opposite
   trust model explicitly, and I think correctly — but it is a *values* choice, and reversing it is the
   one clean route to a much smaller Stage B.
2. **A proof that the omission window is recency-bounded.** If the RustFS mechanism is found and shown
   to affect only keys younger than some bound B, a settle-window rule ("never seal past, and never
   frontier-prove over, anything younger than B") closes both the record-level and namespace-level
   defects for that store class — cheaper than everything above. Today nothing bounds B.
3. **Task 5′ accreting anyway.** If the restated Task 5 still spawns ≥3 new register items in its review
   rounds, the ordering invariant is the wrong invariant, and the genesis-namespace ("catalog as a ref
   chain") redesign — which inherits Stage A's completeness arithmetic instead of adding a second
   authority mechanism — should be costed seriously rather than dismissed as I dismiss it above.
4. **The hot spot reproducing at production DDL rates** (not lane rates) after the R12 fix and retry
   backoff — that would reopen the sharding question the BACKLOG already poses, and with it the
   single-object atomicity argument this whole review leans on.

---

**STAGE B VERDICT: option 2** — keep the landed catalog+incarnation core and the 7b chain, restate
Task 5 around prune-before-entry-delete (cutting cursor re-keying and coexistence machinery),
concentrate fence enforcement into one primitive, weaken capacity exactness, and defer Tasks
8/9/12/13 and most of 10 past the pre-release gate.
