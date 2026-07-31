---
description: 'Round 2 of the Stage B strategic review: confrontation with the Codex report, the retirement-as-catalog-state question answered, one round-1 cost claim retracted, verdict unchanged — keep the incarnation'
sidebar_label: 'Stage B strategic review round 2 (Fable)'
sidebar_position: 101
slug: /superpowers/reports/stage-b-strategic-review-fable-round2-2026-07-31
title: 'Stage B strategic review — round 2, Fable'
doc_type: 'reference'
---

# Stage B strategic review — round 2 (Fable) {#stage-b-round2}

Written 2026-07-31 after reading `2026-07-31-stage-b-strategic-review-codex.md` and the confrontation
brief. Everything new here was re-verified against the tree.

## 0. A retraction first {#retraction}

**My round-1 cost note about CI lanes was wrong.** I wrote that refusing same-name rebirth until the
terminal record folds "will burn stateless CI lanes because they drop-and-recreate names constantly."
The namespace is not the SQL name: `liveNamespace` derives it from the table UUID
(`ContentAddressedMetadataStorage.cpp:1232-1238`, `store/<u3>/<uuid>@cas@`), and the tree states in its
own words that a recreated table "always mints a fresh UUID, hence a fresh namespace"
(`Pool/CasPool.h:505-508`). Under `Atomic` databases — the stateless default — drop-and-recreate never
collides, no refusal occurs, and the "decide the eager-fold mitigation now" urgency I attached to it
evaporates. What survives of that paragraph: the `CREATE`-blocked-until-fold window still exists for
the namespace kinds where the `RootNamespace` *is* reusable (next section), and for those it is a
same-name-backup latency question, not a lane-burner.

## 1. The split, and whether it is real {#split}

The controller reads the split as two answers to different questions — temporary refusal (me) versus
permanent non-reuse (codex). **I accept that reading for round 1's texts**: codex's rebuttal of my
position ("a merely temporary refuse-while-old-objects-survive rule does not remove the need for an
incarnation, because `LIST` cannot certify the old objects are gone") is a rebuttal of a position I do
not hold — my round-1 recommendation was refusal *plus* incarnation, with the incarnation carrying
exactly the burden codex says `LIST` cannot: making uncertifiable physical debris inert. On mechanism
we agree completely; three propositions are jointly held by both reviews: (a) LIST cannot prove
physical emptiness; (b) therefore either debris must be inert (incarnation) or the name must never
return (permanent retirement); (c) the catalog must stay the universe either way.

**But round 2's verified facts collapse the two questions into one, and the answer eliminates codex's
branch.** Permanent exact-`RootNamespace` non-reuse is only acceptable if no supported workflow reuses
a `RootNamespace`. Codex's own report names this as its change-my-mind trigger — "a supported,
pre-release-critical workflow must reuse the exact same `RootNamespace` … and cannot instead mint a
fresh table UUID or shadow namespace". That trigger fires, on the shadow path codex's report never
examined:

- **Shadow namespaces are reusable by construction.** `shadowNamespace` is the *literal* shadow table
  directory — `shadow/<backup>/store/<u3>/<uuid>` or `shadow/<backup>/data/<db>/<tbl>` — "bijective
  with the disk path", pool-global (`ContentAddressedMetadataStorage.cpp:1246-1253`). A freeze/backup
  under name `B`, its removal (`removeRecursive`/`dropNamespace` tombstone the shadow tree,
  `:1472`), and a second freeze under the same name `B` is the *routine* reuse case — not an operator
  error, not `Ordinary`, not an explicit UUID. Under permanent non-reuse, every backup name is
  single-use forever. Backups are in scope pre-release; this is disqualifying on its own.
- **Explicit-UUID creation is supported syntax.** `ATTACH TABLE … UUID '<u>'` / `CREATE … UUID` exist,
  and `DatabaseReplicated` *relies* on shared explicit UUIDs across replicas (its metadata log carries
  the UUID; `src/Databases/DatabaseReplicated.cpp:633-657` reasons about same-UUID replicas
  explicitly). A drop followed by any replay/re-attach that presents the old UUID reaches the retired
  `RootNamespace`. Rare, but reachable through supported syntax, and the operator cannot always choose
  a fresh UUID (a replicated DDL log replays what it stores).
- `Ordinary` databases (verified present, `src/Databases/DatabaseFactory.cpp:37-65`) are the third
  route; that one *can* simply be forbidden — see §3.

Codex's fallback for these — "the operator must choose a fresh namespace" — is not available for a
backup name baked into tooling or a UUID baked into a replicated DDL log. So the split resolves not by
preference but by reachability: **permanent non-reuse is off the table, and once it is, codex's own
§Invariants-to-cut concedes the conclusion** — "incarnation becomes unnecessary only with permanent
exact-`RootNamespace` non-reuse (or with a durable generation token, which is incarnation under another
name)". The incarnation stays.

One more cost asymmetry worth stating because it is decision-relevant even if reuse were somehow
closed: codex's proposal reverts the `NamespaceLifeId` identity across the same ~66 sites Stage B
just rewrote, regenerates the format goldens, and re-derives the four TLA models whose properties are
stated in terms of incarnation inertness — churn on landed, gate-verified safety code in the same
pre-release week Task 5 must land. "Net code deletion" is true of the end state and false of the
path. Keeping a verified mechanism is the low-risk branch; deleting one days before a deadline is not.

## 2. The commissioner's question: retirement as a catalog state {#catalog-state}

Asked concretely, answered concretely — first for the no-incarnation design the question was posed
against, then for the design I recommend.

### 2.1 Does codex's ordering argument survive the marker moving into the catalog? {#ordering}

**Yes — trivially, and the ordering itself disappears.** Codex orders the marker write before `_ckpt`
deletion so the refusal is durable before the evidence that enforces it is destroyed, and needs that
ordering *because* marker and catalog are two objects. Inside the catalog there are not two objects:
the terminal step becomes one token-CAS, `Removing → Retired`, *replacing* entry deletion. There is no
window between "refusal durable" and "entry gone" because the entry never goes. The removal sequence
shortens by one object class and one ordering obligation — strictly simpler than the marker variant.

What does **not** survive is codex's stated benefit — "even catalog loss cannot turn the removal window
into a fresh birth" — and it does not need to. Catalog loss is pool-fatal in *every* Stage B variant,
codex's included: the catalog is the universe, every live's identity derives from it, REBUILD consumes
it as an *input* and never reconstructs it (spec §7), and `casUpdate` treats a present-then-vanished
catalog as `LOGICAL_ERROR`, fail-closed (`Pool/CasRefCatalog.h:82-88`). A marker that survives an
apocalypse the rest of the system does not survive protects nothing. So on this sub-point the
commissioner is right and codex's outside-marker argument is moot: **if** retirement is wanted, a
`Retired` catalog state is the better shape.

### 2.2 What actually breaks with `Retired`-as-state {#retired-cost}

Not ordering, not correctness — **the bound**. Name the failure: the catalog becomes
O(historical namespaces) on the one object that is (i) re-PUT in full by every lifecycle CAS, (ii) a
measured write hot spot (137/250 timeout lines, BACKLOG `{#ref-catalog-write-hotspot}`), and (iii)
guarded by a capacity-admission predicate that will start **refusing legitimate creations** when
historical debris fills the budget (`Pool/CasRefCatalog.h:112-121`; the predicate charges entries, and
INV-3's "stays O(`Creating`+`Live`+`Removing`)" is the invariant this breaks). Quantified: a retired
row is small but not nothing — the name dominates, ~70-100 bytes for a UUID-shaped `RootNamespace` —
and the pools we actually run (CI lanes, soaks) create 10³-10⁵ namespaces per day. That is megabytes
of dead rows within days, every one re-serialized into every subsequent DDL PUT. For a low-DDL
production pool it is survivable for years; for our own test infrastructure it is not.

**Is there a compaction rule that keeps it honest?** Only a partial one, and it is worth having if the
no-incarnation road is taken: UUID-derived names never recur under normal DDL, so their `Retired` rows
protect only against explicit-UUID re-attach; a rule "drop `Retired` rows for UUID-shaped names once
their cleanup item retires" bounds growth by the *reusable-name* population (shadow names, `Ordinary`
names) instead of table churn. But note what it silently does: it converts explicit-UUID `ATTACH`
after a drop from "refused" into "admitted over possibly-surviving debris" — the aliasing hole,
reachable by supported syntax. Compaction of a retirement record and certification of physical
emptiness are the same problem, and `LIST` cannot do it. There is no honest full compaction.

### 2.3 The answer for the design I recommend: no retirement anywhere {#no-retirement}

With the incarnation kept, the commissioner's constraint — *"I really do not want to invent deletion
markers and the like again — better that this lives directly in the catalog"* — is satisfied more
thoroughly than by a `Retired` state: **nothing needs to remember a dead namespace at all.** Removal
ends in entry deletion; debris is inert by foreign prefix; the catalog stays O(active); there is no
marker object *and* no immortal row. And the same principle cuts an existing marker class: the
`_cleanup` marker that today gates same-namespace recreation (`Pool/CasPool.h:508`, and the Task 5
text where its non-publication makes a name "permanently unrecreatable", plan `:1256-1258`) is a
Stage-A physical-empty vestige that the incarnation makes unnecessary — Task 5′ should *delete* it,
not wire it into the new removal. That is the concrete "fewer markers" deliverable this review can
hand the commissioner this week.

### 2.4 The smallest catalog state set, across every namespace kind in the tree {#states}

Namespace kinds found: live-tree table namespaces (UUID-derived, `store/<u3>/<uuid>@cas@`,
`ContentAddressedMetadataStorage.cpp:1232`), shadow/backup namespaces (literal shadow dir, both
layouts, `:1246-1253`), and the detached-parts surface, which is *not* a namespace kind — detached
parts share the table's namespace under a `detached/` ref prefix (`:1267-1274`). Plain-file
(non-table) content rides namespace files inside an existing namespace, not a separate kind.
**Pool-member decommission is not a namespace lifecycle** and should not become a catalog state:
its subject is a server root / mount slot (register R5), its registry already exists, and putting
member states into the namespace catalog would couple two lifecycles that share nothing but the word
"decommission".

So the smallest set, uniform across both real namespace kinds:

- **with incarnations (recommended): `Creating`, `Live`, `Removing`** — exactly what is landed; the
  states the commissioner listed ("normal, decommissioning, creating") map onto these, with
  member-decommission living in the member registry where it already is;
- without incarnations: the same three **plus an immortal `Retired`** — the growth cost of §2.2 is the
  price of deleting the incarnation, and it is paid on the hot object.

## 3. `Ordinary` databases: forbid them anyway, but it is not the answer {#ordinary}

Forbidding `Ordinary`+CAS is correct, cheap, and worth doing **regardless of every other decision** —
a deprecated engine whose UUID-less layout (`data/<db>/<tbl>`) makes every table's `RootNamespace`
stable across drop-and-recreate has no business on a pool whose whole identity model leans on
UUID freshness. One check at disk/table initialization, one test.

But it is **not sufficient** to make permanent non-reuse safe, because it closes only one of three
routes (§1): shadow names remain reusable *by construction*, and explicit-UUID attach/replay remains
reachable. Confining a generation token to just the shadow path — the brief's follow-up — buys
nothing: it means two key grammars, two lifecycle variants, and two proof obligations where today
there is one, i.e. *more* of exactly the spread this review exists to reduce, while still leaving the
explicit-UUID route unguarded. If any namespace kind needs a generation, uniformity is cheaper than
confinement. The refusal an operator would see under non-reuse, per route: a second `BACKUP`/`FREEZE`
under a reused name — permanently refused, recoverable only by renaming forever; an explicit-UUID
`ATTACH` after a drop — permanently refused, recoverable by minting a fresh UUID *when the operator
controls it*, which a replicated DDL log does not allow. No object-store surgery, but permanent
refusals with no in-band recovery are exactly the operational cliff a storage layer should not have.

## 4. Restated recommendation {#restated}

**Unchanged: option 2, incarnation kept.** What round 2 changed: I retract the CI-lane cost claim
(§0); I withdraw the "decide the eager-fold mitigation now" urgency with it; and I adopt three things
from codex's report because they are better than my round-1 equivalents — `ensureLive` moving wholesale
into `CasRefCatalog` (subsumes my fenced-writer primitive with a cleaner boundary), the immutable
`GcNamespacePlan` handed to fold/cleanup/fsck/decommission (finishes what `FoldResult::live_incarnation`
started), and the hot-spot-as-release-gate framing (rerun the S3 lane after non-minting reads and the
simplified lifecycle; destruction stays off if creation still times out). Codex's seven-step removal
sequence is adopted with steps 4 and 7 merged per §2.1 — no marker, entry transition last — and with
the `_cleanup` marker class deleted per §2.3.

The arguments of codex's I considered and rejected, with the reasons: permanent non-reuse (fails on
reachable reuse routes its report did not examine — shadow by construction, explicit-UUID replay — and
its own change-my-mind clause concedes the incarnation once such a workflow exists, §1); the
outside-catalog retirement marker (its ordering benefit is real but is subsumed by a single CAS once
retirement moves into the catalog, and its catalog-loss benefit protects against a pool-fatal event,
§2.1); and "net code deletion in one focused week" (true of the end state, false of the path — it
reverts verified identity plumbing across ~66 sites plus format goldens plus four TLA models in the
same week Task 5 must land, §1).

What fits in the pre-release week, concretely: Task 5′ (removal with the merged ordering, R12
non-minting, `_cleanup` deletion, cursor prune-before-entry-delete; no cursor re-key, no coexistence
test family), the `Ordinary`+CAS refusal, Task 6 absorbing `ensureLive` + `GcNamespacePlan` + the
`stageATransition`/`resolveLifeOrSentinel` deletions, then 5b → 6b → 7 → 7a → 7b → 11 as in round 1,
with 8/9/12/13 and most of 10 deferred with named owners. If the week cannot absorb that chain, the
fallback is codex's and mine both: ship with `UniversePolicy` still suppressing destruction — never a
hurried Task 5 plus a flipped 7b.

**What would change my mind, updated:** a demonstration that shadow namespaces can be re-keyed by a
per-backup unique component (making them UUID-like, never reused) *and* that explicit-UUID
attach-after-drop can be refused at the DDL layer rather than the storage layer — those two together
would restore codex's no-reuse premise, and with it the legitimacy of cutting the incarnation. Neither
is a pre-release-week change; both belong on the north-star list next to head-CAS.

---

**STAGE B VERDICT: option 2** — keep the incarnation: permanent non-reuse fails on reachable routes
(shadow names by construction, explicit-UUID replay), retirement-as-catalog-state is the right shape
*if* one retires but costs O(historical) growth on the measured hot object, and the landed design is
the only variant needing no marker, no immortal row, and no reuse cliff.
