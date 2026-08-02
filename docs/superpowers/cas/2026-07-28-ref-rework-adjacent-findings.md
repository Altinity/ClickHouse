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

## R1. Verbatim-file rebirth aliasing — [closed, 2026-08-02] {#r1-verbatim-alias}

> **CLOSED.** Stage B re-keyed namespace files to
> `cas/ns/state/<life_id>/_files/<relative-name>` and bound reads and delayed writes to the exact
> `NamespaceLifeId` selected before rebirth. The independent loose mountpoint-object audit found no
> namespace/catalog re-resolution: those objects remain at `roots/<server_root_id>/<path>` and cannot
> be retargeted by same-name namespace rebirth. The evidence and per-sub-hazard disposition are in
> [the R1 closure note](./2026-08-02-r1-verbatim-file-aliasing-closure.md). The body below is retained
> as the historical statement of the pre-amendment problem, not the current contract.

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

**SUPERSEDED 2026-07-30 — probe A is DELETED, not re-gated.** The authoritative directive
`docs/superpowers/specs/2026-07-30-cas-gc-destructive-baseline-directive.md` removes the detector
entirely: its extra LIST, `PoolConfig::gc_probe_a_period`, the `ref_list_probe` phase row with its
`due`/`performed`/`skipped` observability, `CasGcProbeA*`, its tests and the comments that describe
it. The gating VERDICT above is not reversed — it gated nothing, and now there is nothing to gate.
The reason it goes rather than stays: a sampled store-quality detector whose signal is "a LIST can
be a liar" is obsolete once the catalog is the authoritative universe (Stage B Task 4) and recovery
is LIST-independent (Task 5b) — correctness no longer rests on LIST fidelity, so paying a second
full `cas/refs/` enumeration to measure it buys nothing. B1/B2 accounting and the mount-time
capability probe (#23) are explicitly KEPT. Executes as plan Task 7a of
`docs/superpowers/plans/2026-07-28-cas-ref-chain-stage-b-catalog.md`; the result criterion is that
no round performs a second full ref LIST.

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

## R10. Ref-key grouping drops the incarnation — a sibling of R1 in a different object {#r10-groupref-alias}

Found 2026-07-30 by Task 4's implementer while reading `groupRefKeys` (`Pool/CasRefProtocol.cpp`) before
writing code. **Same shape as R1, different member, and R1 does not cover it.**

`groupRefKeys` groups listed ref keys by `parsed->id.ns.string()` alone and silently drops
`parsed->id.incarnation`. So if a namespace is dropped and recreated before GC has cleaned up the dead
incarnation's ref objects, and both incarnations' keys appear in one round's listing, **both
incarnations' log, snapshot and cleanup-marker ids merge into ONE `RefTableListing` under the shared
namespace name.** Harmless while incarnations do not exist for real; a live aliasing bug the moment they
do, which is exactly what Stage B introduces.

R1 (`{#r1-verbatim-alias}`) is the same defect against verbatim/namespace files keyed
`{namespace, file_name}`. Task 9 owns R1's closure note. **That note should close the FAMILY, not the
member** — the register now has two instances of "an identity that forgot the incarnation", and the
generalisation is what stops a third.

Being fixed in Task 4 as a pre-filter ahead of `groupRefKeys` in `fold()` and in the `REBUILD` path:
listed keys whose parsed incarnation does not match the catalog's current entry for that namespace are
dropped before grouping. Note what this does NOT do, so nobody expects it: the PARSER still accepts a
dead incarnation's key, deliberately — `Layout` is catalog-independent by design, so it is the wrong
place to refuse.

## R13. `_ckpt.life_epoch` has TWO honest contributors, so "conflict is corruption" wedges ordinary operation {#r13-life-epoch-two-contributors}

Found 2026-07-31 by Task 4c's implementer, before writing the behaviour it was asked for — the fourth time this
stage that a directive's rule rested on a comment nobody had checked against the code.

`life_epoch` is contributed by two writers of ONE life, deriving it from epochs that **legitimately differ**:
`CasRefCatalog::completeCreation` contributes the creator's `writer_epoch`, and `commitRefChunk`'s birth chunk
contributes the ref-log `NamespaceBirth`'s `id.writer_epoch`. Task 4's re-key closed the rebirth alias between
two *lives*; it never touched two writers of one life.

Two reachable sequences, the second of which is **ordinary operation**:
- **Resumed creation.** Creator at E1 publishes `_ckpt{life_epoch=E1}` and dies before `Creating → Live`; a
  later actor at E2 reconciles and resumes `completeCreation`, contributing E2. The suite ALREADY pins that the
  later value is the correct one.
- **Restart between create and first write.** `completeCreation` publishes E_create, the mount's writer epoch
  advances, the first precommit emits the birth chunk at E_write. That is CREATE TABLE, restart, INSERT.

So `max` is not laundering a contradiction — it is load-bearing: `life_epoch` must equal the writer epoch of the
`NamespaceBirth` that actually landed, which is always the later, and writer epochs are monotone. Under "two
different present values are corruption" both sequences raise `CORRUPTED_DATA`, and since `_ckpt` has no repair
path and no writer may delete it outside namespace removal, **the namespace wedges permanently**: every retry
re-reads E1, re-contributes E2, re-throws.

**The false sentence that made the strict rule look free:** the field's own doc called `life_epoch` "a
namespace-lifetime constant, so its semantic maximum is itself". It is not constant across a resumed or a
restarted birth, and the rule was written on top of that claim.

**Ruling (2026-07-31): refuse a DECREASE, let a higher value win — and this is a SHARPENING, not a concession.**
By monotonicity a decrease is unreachable honestly, so ask what one *is* when it appears: a writer contributing
an epoch older than one already durable, i.e. **a fenced-out writer whose contribution landed anyway**. `max`
absorbs that silently today. So the narrowed rule detects the only case that indicates a real bug, while the
directive's version fires on the two cases that indicate nothing.

## R12. A pure READ births a catalog entry, and the catalog has a capacity budget {#r12-read-births-namespace}

Found 2026-07-31 by Task 4b's implementer while checking whether its own design added a request class — it
does not, but the path it sits on already does something worse.

`ensureRefTableRecovered` → `resolveNamespaceLife` **mints** a catalog entry when none exists: a
`createNamespace` CAS to `Live` plus a durable `_ckpt` publish. `namespaceIsRemoved` calls it, and every
namespace-file reader calls that. So **`existsFile` on a never-born namespace performs durable writes** — a
read with a write side effect, on a path whose whole contract is to answer a question.

**The consequence that sets its priority is not the surprise, it is the budget.** The catalog is ONE pool-wide
object under a capacity-admission predicate. Entries minted by reads are never reclaimed by anything (no
removal lifecycle exists yet), so **reads against nonexistent tables grow the catalog without bound**, and a
read storm can exhaust the admission budget and start refusing legitimate creations. A cluster does not need a
bug to produce that traffic — a stale replica, a dropped-then-queried table, or any probe against a table this
node has never had is enough.

Not introduced by Task 4b, which merely sits on the same path. Not a data-loss shape: the minted entry is a
`Live` namespace with no content, which the frontier proves legitimately empty.

**The fix direction, though the decision is open:** a read must resolve a life **without creating one** —
`resolveNamespaceLife` needs a read-only mode that answers "no such namespace" instead of minting, and the
reader paths take that mode. The `optional<NamespaceLifeId>` collapse Task 4b is building on the reader side is
the natural shape for it: *the life iff this namespace exists and its files are readable, else nothing*.

**Whether it gates the destruction flip:** it does not create a deletion hazard, so it is not a 7b blocker on
safety grounds. But it interacts with two things 7b depends on — the un-cataloged anomaly (entries that exist
for no reason are entries whose absence later looks like damage) and the capacity predicate that Task 5's
removal lifecycle must keep true. Decide before Task 5, with the removal ordering, rather than after.

## R11c. The THIRD way to a vacuous frontier: incarnation MISMATCH, not entry absence — GATES TASK 7b {#r11c-incarnation-mismatch}

Found 2026-07-30 by asking the fixed code the same question that found R11b: *is there another way to reach a
vacuously-complete frontier?* There was, and it survived both earlier fixes.

R11b's fix keys on the catalog not naming a namespace **at all**. It does nothing when the catalog names it
at an incarnation whose key space is empty. Catalog says `N` is live at `Y`, while `N`'s objects and its
shard-0 fold cursor belong to `X`. The walk GETs `refLogKey(Y, cursor+1)` → absent; `witnessAbove` finds
nothing, because the R10 filter dropped every `X` key from `ref_tables` **and** `readCheckpointWitnesses`
reads `_ckpt` at `Y` too; no carried hold, so the namespace is *proven*. And R10 drops `X`'s keys **silently
by design** — that is the ordinary different-incarnation case, not damage — so nothing objects. Frontier
complete, `X`'s live blobs read in-degree zero, destruction.

**Not reachable today, which is precisely why it must GATE Task 7b rather than sit in a ledger.**
Drop-and-recreate under the catalog arrives with Tasks 5/6, and the name-keyed fold cursor is documented in
`Gc/CasGc.cpp` as the Stage-A residual whose only protection is `UniversePolicy` suppression — i.e. the thing
7b removes. Two future tasks would independently make it live.

**The detector needs nothing from Task 5.** A namespace whose CURRENT life contributed nothing — no entry in
`checkpoints`, empty listing — while a NON-CURRENT life of the same name HAS listed objects is a
contradiction, and the R10 loop already holds both facts.

**CORRECTION (2026-07-31): the predicate I ruled for would NOT have closed this, and neither the reviewer's
framing nor mine noticed.** Both said R11c is "what the union-shaped predicate still permits". Work the case
against the authority-shaped predicate instead: the catalog **does** name `N`, at incarnation `Y`. So the life
IS catalog-derived, an "every walked life came from the catalog" conjunct is satisfied, and the walk proves it
at `Y` where nothing exists. **Both predicate shapes are blind to this family**, because both ask *where did
the life come from* and neither asks *does that life account for the objects that exist*.

What closes it is a detector that asks the second question, built on knowledge the R10 loop already has: track
every namespace name seen at a DIFFERENT (dead) incarnation, and raise an anomaly when such a namespace's
CURRENT incarnation contributed neither a listed ref object nor a `_ckpt`. That is the contradiction, and it is
observable without waiting for Task 5.

The explicit predicate is still worth having, for the narrower thing it actually does: it moves the
*absent-entirely* family's protection from three sites cooperating implicitly to one place refusing explicitly.
It must say in its own comment that it does not cover the mismatch family — a conjunct named "every walked life
came from the catalog" reads as covering precisely the case it misses.

**And the process lesson is sharper than the bug.** The ruling on R11b asked for a predicate *about the
authority rather than the union*. What shipped instead changed the numerator's SOURCE and left the predicate
byte-identical — the guarantee is genuinely equivalent today, but it became an **implicit invariant spread
over three sites** instead of one explicit statement, and the union-shaped predicate is exactly what still
permits this case. **An invariant that holds implicitly across three sites has three places to stay silent;
an explicit predicate has one place to refuse.**

## R11b. R11's first fix was insufficient, and the fix made the posture WORSE than before {#r11b-authority-vs-union}

Found 2026-07-30 by the Task 4-C review, on the exact question the review was asked to answer: *is
`frontier_namespaces > 0` the right predicate, or is there a second way to reach a vacuously-complete
frontier?* There is.

**The guard was over the wrong set.** The frontier's denominator is the UNION — round hint ∪ sealed cursors
∪ catalog `Live`/`Removing` — not the catalog. So `frontier_namespaces > 0` refuses only a pool where the
whole union is empty, i.e. a fresh one. It does not refuse the case R11 named by name: a **damaged**
catalog.

The reachable chain: the catalog is absent, so `read` yields an empty catalog; the R10 filter drops every
listed ref key (no entry, no match) and records no anomaly; each namespace the surviving seal still carries
a cursor for is walked anyway, with the life falling back to `stageATransition(ns)` — a key space where a
real-incarnation namespace has nothing; the expected-next GET is absent, there is no listing and no `_ckpt`
witness, so the namespace is *proven*; every namespace is proven, the frontier reads complete, suppression
clears, and the round destroys a pool in which every live blob has in-degree zero.

**And this is strictly worse than before the change.** With LIST-sourced discovery, that same damaged pool
would have been walked where its objects actually are, would have folded real records, and would have
produced a real frontier. Moving the authority onto one object made **that object's absence
indistinguishable from "everything is proven empty"**.

**The generalisation, which is the reusable part.** R11's own lesson was *when a gate rests on `a == b`, ask
what happens when both are zero*. Its sibling is: **ask where each side of the comparison CAME FROM.** Here
both sides were derived from a set the authority never contributed to, so the comparison was structurally
incapable of noticing that the authority was gone. A proof whose inputs can all come from a source other
than the authority is not a proof about the authority.

**Two fixes, both required** (ruled 2026-07-30): the sentinel fallback must not exist in the frontier walk —
a namespace the catalog does not name is UNPROVEN, not walked at a fabricated key; and the predicate must be
about the authority ("every walked namespace's life came from the catalog"), with an empty catalog under a
nonzero prior seal recorded as an anomaly so it both suppresses and reaches an operator.

**One ruling ON TOP of the review's proposal, because the review's own argument cuts both ways.** It argued
the class is not pre-release-only because an entry disappears during ordinary removal — which is also why
"ref objects exist, no entry" is a **legitimate transient state on every removal**, since the removal
lifecycle deletes the entry LAST. So an un-cataloged namespace is an anomaly only WITHOUT evidence of a
removal in progress. An anomaly that fires on normal operation trains operators to dismiss anomalies, which
is the same harm as a hard fsck finding raised against undamaged data.

## R11. An authoritative universe with zero entries satisfies the frontier VACUOUSLY {#r11-empty-universe-vacuous}

Found 2026-07-30 while adjudicating Task 4's scope, and verified on the code. `Gc::fold` computes
`result.frontier_complete = universe_authoritative && result.frontier_proven == result.frontier_namespaces`.
With an empty universe that comparison is `0 == 0` — **TRUE** — so the frontier reads complete,
`suppress_destructive` clears, and a round destroys against a pool where nothing is known to be live.

The hazard is not hypothetical once the universe becomes authoritative: a fresh pool, a damaged catalog,
or a read that legitimately returns nothing all produce zero entries. Today it is masked only because
`UniversePolicy::kDefault` is `StageA_Suppressed`, i.e. by the very thing Task 7b removes — so this must
be closed BEFORE that flip, not with it.

Task 4 closes it: zero entries must refuse to be a complete frontier, with the refusal reported in the
existing suppression voice and pinned by a test asserting per-delete-family inertness rather than an
aggregate zero, since an aggregate can hide one family that ran while another did not.

**The lesson generalises past this site.** A comparison of two counts that are both derived from the same
empty set is a vacuous truth wearing the clothes of a proof — the same class as this campaign's
trivially-true `entries_redeleted >= objects_deleted` and its vacuously-passing test. When a gate rests on
`a == b`, ask what happens when both are zero.
