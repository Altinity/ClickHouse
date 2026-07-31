---
description: 'v9 CORE design for the LIST-incompleteness release blocker: per-namespace contiguous ref ids, in-band epoch seals, a namespace catalog with ref-layer-scoped incarnations, a per-namespace checkpoint object, a mandatory destructive-round frontier proof and a REBUILD-surviving hold — LIST is a zero-trust hint; two new object kinds, both off the append hot path. Adjacent pre-existing defects surfaced by eight review rounds live in the companion register, not here.'
sidebar_label: 'CAS ref contiguous-chain'
sidebar_position: 20260727
slug: /superpowers/specs/cas-ref-chain-complete-cut-design
title: 'CAS: contiguous ref streams and the in-band epoch seal'
doc_type: 'reference'
---

# CAS: contiguous ref streams and the in-band epoch seal {#cas-ref-contiguous-chain}

**Date:** 2026-07-28. **Status:** v9 CONVERGED — round 9 verdict APPROVE-WITH-FIXES (no core
data-loss counterexample; the six fixes are folded into this text and the plan obligations); the
user delegated the proceed decision to convergence (unattended directive). **Branch:**
`cas-gc-rebuild`.

Fixes BACKLOG `{#list-as-journal-dataloss-2026-07-25}` (observed:
`reports/2026-07-26-list-incompleteness-investigation.md`) and closes the rev.4 `Late Predecessor
PUT` limitation. Realizes P4 of `cas/draft-fixes-20260726.md`: zero added requests on the append path
and the fold's per-record path. Two new object kinds (`ref_catalog`, `_ckpt`), both off the hot path.

## 1. Problem {#problem}

GC folds what a listing returned and seals a cursor above what it OBSERVED; a hidden `-1` is a
permanent leak, a hidden `+1` deletes acked data. Recovery, the sweep, REBUILD and fsck consume the
same untrusted listings. Root cause (converged on independently twice): **absence is undecidable in a
sparse id space** — the pool-wide `next_ref_sequence` makes per-namespace gaps the norm, so every
hole demands a certificate. The fix is invariants under which absence and committed-ness are decided
by arithmetic, point reads and conditional writes — the operations the store performed honestly even
while its LIST lied. Stated up front: blob in-degree is POOL-WIDE, so destructive rounds need a
frontier proof for EVERY catalog namespace — one exact 404 `GET` per quiet namespace per destructive
round is the honest price (fold-only rounds are free; at extreme namespace counts this is the knob
where the head-CAS alternative re-enters, §8).

**Why the namespace universe is an object and not a listing — stated precisely, because the loose
version invites a correct rebuttal.** The claim is NOT "LIST is unreliable". It is that a paginated
LIST and a single-object read answer different questions. A catalog read is one object version: every
member as of one instant, a **cut**. A multi-page walk stitches separately fetched pages, so the set
it returns need not have existed at any instant, and it may omit a member that exists throughout.
Both observations are equally stale by the time anything acts on them — TOCTOU is present in both,
and pointing that out is true but does not distinguish them. The distinction is that **staleness after
a cut and a scan that never was a cut are different proof obligations**: staleness is closed
downstream by protocol (`Creating` forbids publication, the frontier proof, delayed condemnation, the
delete-site in-degree re-read), whereas nothing downstream can recover a namespace the universe never
named — the frontier iterates the members it was given, so an omitted namespace is simply not probed
and its unfolded work never holds destruction. Anyone re-reading this spec and objecting "but LIST
returns everything that was created" is answering the wrong claim: the requirement is atomic
completeness, not freshness, and a bounded re-LIST or retry-on-out-of-order buys nothing here because
a wholesale-absent namespace produces no observation to retry on.

## 2. Invariants {#invariants}

**INV-1 — per-namespace contiguous ids.** Next id = `greatest_applied.ref_sequence + 1` (already the
trial preview in `commitRefChunk`); the pool-wide atomic is deleted; within `(namespace, epoch)`
durable ids are dense `1..T`. Reuse only under the **every-attempt rule**: an id is freed only when
nothing was sent or every sent attempt has its own conclusive rejection; a definite rejection AFTER
an ambiguous attempt keeps the lane wedged. The wedge stores its admission fence generation; each
later caller's flush performs at most one bounded same-`(key, bytes)` conditional create under that
generation (no background deadline-resetting loop); a successor's `EpochSeal` found at the key is a
conclusive rejection. A permanently quiet wedged namespace retries on its next caller or an
independently occurring remount — acceptable: the operation was never acknowledged.

**INV-2 — every epoch transition is closed in-band.** First touch after ANY writer-epoch transition
CAS-walks the dead tail via the dedicated `slot-occupy` primitive
(`Created | Occupied(bytes, token) | Unresolved`): `Occupied` → adopt and replay (a straggler, or an
`EpochSeal`, terminating the walk); `Created` → the seal occupies `(E, T+1)` and the `Late
Predecessor PUT` ghost can never land — the store's conditional create is the fence. Grammar: a seal
transaction contains exactly one seal operation; `prev_epoch_seal` is required on exactly sequence 1
of every non-genesis epoch (including a sequence-1 seal closing an empty epoch) and forbidden
elsewhere. A dying lane that observes the seal retries `T+1`, never mints `T+2` (state-derived ids).

**INV-3 — the catalog, with ref-layer incarnations.** `cas/ref_catalog`, token-CAS like `gc/state`:
`namespace → {state: Creating | Live | Removing | RemovalReady, incarnation}` (+ creator fence
identity while `Creating`; immutable `removal_started_round` while `Removing`/`RemovalReady`). The
removal actor samples the current adopted `gc/state.round` before `Live → Removing` and stores it in
the same catalog CAS; it is diagnostic age, never a safety fence. The incarnation (random 128-bit,
minted at `Creating`) qualifies **the ref layer only**:
`<ns>/<inc>/{_log, _snap, _ckpt}`, with a canonical grammar and refusal of legacy-shaped keys.
Removal ends in a fourth, **transient** state. `Creating → Live → Removing → RemovalReady → absent`.
A namespace enters `RemovalReady` only when its terminal record has folded into a life-scoped cleanup
item in the adopted seal — positive evidence, never inferred from an item's absence — and
the transition additionally requires that the namespace holds no durable hold. `Completed` means that
the terminal fold is durable; it does **not** claim physical cleanup succeeded. After adopting that seal,
GC performs one bounded, suppression-aware best-effort cleanup pass and only then attempts the fenced
`Removing → RemovalReady` CAS. A stop before the CAS repeats the idempotent pass; LIST omissions,
token mismatches and suppression remain leak-only and do not prevent the CAS. The old marker-driven
`Pending → Completed` handshake is deleted with `_cleanup`: the fold that consumes the terminal emits
the completed-form item directly. Because no second state remains, `RefNsCleanupState` and the item's
wire `state` field are deleted too; the item's presence is the positive evidence.

A catalog cut that says `RemovalReady` is **never a fold target**: no new walk, probe or cursor may be
admitted from that cut. The seal which earned the transition may still carry its predecessor cursor;
the first round cut after the transition removes that cursor and cleanup item together. Only
after the adopted seal proves both absent may the removal driver exact-delete `_ckpt` and exact-CAS-delete
the complete observed `RemovalReady` row. `RemovalReady` is not a tombstone — it exists only during an
in-flight removal, is counted by the same admission reservation as the other states, and is deleted, so
the catalog stays O(active + in-flight removals).

**Why a state and not an ordering, stated because the ordering was tried and is unsound.** An earlier
formulation required only that a seal with the cursor pruned be durable before entry deletion. That
observation is **not stable**: while the entry still says `Removing`, the frontier proof force-adds
every `Live`/`Removing` entry to the walked set and writes a cursor for each, so a later round
re-creates the very cursor that was pruned, and the resumed driver has no safe-and-live choice —
trusting the historical seal deletes an entry whose current seal carries a cursor, and revalidating can
wait forever. `RemovalReady` replaces a temporal claim about past seals with a durable catalog fact,
and cursor absence becomes monotone.

**Monotone only if the predicate is enforced at every producer, so the producers are enumerated here
rather than asserted away.** Five write a cursor or admit a walk target: the catalog-only frontier
loop; the parent-cursor carry (which must consult the state per CARRIED cursor, not only per walked
target); hint-named namespaces from the round's listing (surviving debris is still listed, so the hint
names a `RemovalReady` namespace — like `Creating`, it is cataloged but not walkable, distinct from both
`Live`/`Removing` and absent); REBUILD, which rebuilds cursors from catalog + `_ckpt` + arithmetic
tails and would resurrect one, since `_ckpt` outlives the transition; and carried holds, which force an
exact walk even when the hint omits the namespace — hence the no-hold precondition above, which is an
enforcement and not a derivation. **A monotonicity claim IS a claim about all producers**; the list is
the proof obligation, not a convenience.

One bounded residue remains: the transition-owning round necessarily took its catalog cut while the row
was still `Removing`, so its adopted seal may carry the predecessor cursor. Because only the fenced GC
leader performs the transition and rounds serialize through a single `gc/state` adoption, no second
pre-transition cut can be adopted afterward. The first post-transition round prunes the cursor and item;
convergence is one round, and the driver's revalidation before deletion waits for that adopted seal.

No physical-empty proof is required anywhere: surviving old-incarnation objects are structurally inert
(foreign prefix; the fold works only off catalog entries) and a **perpetual** janitor deletes
foreign-incarnation debris whenever listed. **Discovery has ONE source, because a namespace owns one
subtree** (see the layout rule below): the round's existing pool-wide `cas/ns/` enumeration nominates
every family's debris — id-bearing ref objects, `_ckpt` (including the lone checkpoint left by a
cancelled `Creating`) and `_files` alike. No second scan and no second cursor exist. Before every
exact-token delete the janitor revalidates that the catalog still either omits the life or names a
different incarnation; unparseable keys are surfaced and skipped. This janitor is the only reclaimer of
a dead life's objects; a LIST omission defers its work but can never affect visibility, rebirth or
deletion safety.

**Layout rule: one namespace, one subtree, and `roots/` knows nothing about lifecycles.**
`cas/ns/<namespace>/<incarnation>/` owns everything that belongs to a life —
`_log`, `_snap`, `_ckpt` and `_files/<relative-name>`. `roots/` holds only objects that are NOT owned by
a CAS catalog: loose mountpoint objects mirrored at their ClickHouse path, with no namespace, no
incarnation and no reserved wrapper. Two consequences are the point of the rule rather than side
effects: a dead life's debris is reachable by exactly one prefix, so the janitor needs one LIST rather
than a paced scan of a tree it does not own; and the inverse parser that classified listed `roots/` keys
by a reserved `_files` segment and extracted a life disappears — nothing under `roots/` carries a life
to extract. The tree is named `cas/ns/`, not `cas/refs/`, because it stopped being the ref stream when
it took ownership of the life; a name that has to be explained is a name that will be misread. Nothing
is added to `roots/` to help traversal: path-shaped browsing is a **user** concern, served by the disk's
own logical `listDirectory` through `clickhouse-disks` and by the inspection tool, so a pointer object
would be a new object kind with no reader in the system.

The mount-safety empty-root precondition is unaffected in shape and still lists three subtrees —
`cas/ns/<srid>/`, `cas/manifests/<srid>/` and `roots/<srid>/` — so a life's files remain inside a checked
subtree and an owner or epoch can never be re-created over surviving data.

**A same-name birth is refused while the predecessor is `Removing` or `RemovalReady`**, as a typed
retry-later that names what it waits for — never an internal error. State the magnitude honestly: the
window spans a terminal fold, a bounded cleanup pass, the transition, one pruning round and the deletion
— several GC rounds. Under UUID-derived names that is an edge case; under a UUID-less table path a
`DROP` + `CREATE` of the same table hits it every time. `CREATE` may therefore wake the existing GC
scheduler and may invoke the idempotent finalizer for an **already** `RemovalReady` row once the adopted
seal carries neither item nor cursor. It may not perform the GC-owned `Removing → RemovalReady`
transition, fold a terminal, run cleanup or drive a GC round; without finalization evidence it returns
retry-later. No wait loops, no second transition driver.
**The catalog stays O(`Creating` + `Live` + `Removing` + `RemovalReady`) under any create/drop churn** (stalled
creators occupy entries until fence-terminal reconciliation — r9-6). Manifests keep their
`(namespace, mount-epoch, build-sequence)` identity — mount-global build ids already prevent
rebirth aliasing; verbatim FILES stay unqualified and keep today's `_cleanup` gate — their
pre-existing rebirth-aliasing hazard is register item R1, not this spec. Capacity: namespace names
get a byte bound; the creation CAS checks an additive predicate (encoded catalog + a per-entry
cursor/cleanup/hold reservation vs both the catalog and fold-seal caps — the catalog is the
serialized admission ledger); `encodeFoldSeal(...).size()` is checked against the cap before every
PUT. Admission refuses loudly; removal is never refused. **The reservation is deliberately
over-covering, not exact.** The asymmetry decides it: over-charging costs admitted namespaces, while
under-charging wedges the fold round, because the seal is then refused on every attempt. So the
reservation is a conservative constant per counted catalog row plus fixed structural headroom for rows
that are not one-for-one with catalog entries: `btr` rows per admitted run segment and `cnd` rows per GC
shard. A namespace-cleanup item does **not** survive entry deletion under this lifecycle: the adopted
seal must retire it before the row can be deleted. Exactness here buys a handful of extra admissions and
costs a boundary-arithmetic proof obligation that has to be re-derived on every format change; do not
restore it.

> **SUPERSEDED 2026-07-29 — the "verbatim FILES stay unqualified" clause above.** Per the
> authoritative directive
> `docs/superpowers/specs/2026-07-29-cas-stage-b-namespace-life-amendments.md`, namespace files ARE
> incarnation-qualified, and one typed
> `NamespaceLifeId{namespace, incarnation}` — replacing `RefNamespaceId` — is required by every ref
> AND namespace-file key helper. Manifests and loose mountpoint objects keep exactly the boundary
> this invariant states; they are explicitly unchanged. The `_cleanup` LIST-derived physical-empty
> proof for `_files` is deleted: rebirth never waits for `_files` to be physically empty, and LIST
> omission may only leak storage, never affect visibility, rebirth or deletion safety. Register item
> R1's file half is therefore closed STRUCTURALLY instead of deferred. The rest of this invariant
> stands as written.
>
> **Extended 2026-07-31: the `_cleanup` OBJECT CLASS dies too, and so does the `Removed` snapshot.**
> The clause above still reads "verbatim FILES stay unqualified and keep today's `_cleanup` gate";
> both halves are now false. A marker that must outlive every possibly-stalled actor is unbounded,
> and the recreate gate it fed is replaced by the catalog cut: a reader answers absence from
> `Removing`, `RemovalReady` or an absent entry, so the `Removed` lifecycle snapshot loses its last
> consumer. Fresh name resolution enforces that cut; an already-held life handle keeps the explicit
> stale-or-`NotFound` contract without a hot-path catalog read. `namespaceIsRemoved` is therefore
> deleted with the snapshot. The two die together — leaving either one keeps a physical-empty vestige
> alive with no reader.

**INV-4 — `_ckpt`.** `<ns>/<inc>/_ckpt`, token-CAS,
`{life_epoch, checkpoint_snapshot_id | none, last_epoch_seal | none}` — forced by prefix cleaning
(a cleaned prefix plus a hidden snapshot is indistinguishable from empty). One update algorithm for
both writers (snapshot publisher; sealer): read → validate → merge by semantic maximum per field →
token-CAS; identical merged body → return without a CAS; retries bound to the recovery deadline.
Snapshots are deletable only STRICTLY BELOW `_ckpt.checkpoint` (a stale pointer can only
under-clean). Missing sampled base → reread `_ckpt`: token advanced → restart; unchanged →
corruption. Removal deletes `_ckpt` by exact token only after the entry is `RemovalReady` and the
adopted seal has retired its item and cursor; the catalog entry is deleted last.

**Entry absence is authoritative, and the catalog is mandatory.** A canonical object whose incarnation
no catalog entry names is **inert debris, never evidence of damage** — a legal removal and a fabricated
entry loss leave byte-identical stores once the cleanup item is gone, so no classifier can separate them
and none may try. What protects an entry is therefore not detection but the mutation API: the only
exported deletion transitions are (1) exact-CAS cancellation of a complete observed `Creating` row
after its creator fence is terminal, and (2) exact-CAS finalization of a complete observed
`RemovalReady` row after item/cursor pruning. No `Live` or `Removing` row is deletable, a live-fenced
`Creating` is not deletable, and there is no generic remove-by-name mutation. A cursor naming no catalog
entry is **counted and logged, never suppressed on** — the measured pool-wide reclamation stall must not
return, but silent omission would mask a defect in the producer predicate. Correspondingly, after pool
bootstrap an absent or undecodable `cas/ref_catalog` is `CORRUPTED_DATA` and stops creation, removal and
GC: bootstrap creates the empty catalog and marks the pool ready, and nothing may ever bootstrap an
empty catalog again. This removes the "vanished catalog reads as virgin" special case rather than
adding lifecycle design.

**Two failures that look alike and must not be treated alike.** A terminal record that is unreadable
**before** it folds is a loss of EVIDENCE: removal legitimately blocks in `Removing`, and the operator
must see a terminal-corrupt stuck removal. Do not promise `REBUILD` as the escape — nothing establishes
that it can reconstruct that exact terminal; the credible exits are restoring the object or recreating
the pool. A `Removing` row without a matching cleanup item is reported once the current round reaches
an overflow-safe age of N rounds, on every round thereafter; the catalog field makes the threshold
stable across restart. A recorded exact-read failure names the unreadable key; otherwise the diagnostic
says only that the terminal has not folded, without inventing a terminal key the catalog does not store.
Once the terminal HAS folded and the adopted seal carries the cleanup item derived from it, the
object's later inaccessibility matters only for physically deleting manifests. That is a cleanup failure
and stays **leak-only**: the transition fires, entry deletion lands, and the
manifests remain orphan debris under a non-suppressing leak counter. Any other reading lets physical
cleanup back into the lifecycle through a side door.

**Read-side contract, stated honestly:** ref-layer readers hold `(namespace, incarnation)` and can
never alias a new life (foreign prefix); a stale reader gets stale-or-`NotFound`, not rejection —
rejection would need a fence/catalog read that hot paths do not pay. Destructive cleanup revalidates
life and fence immediately before every delete. **Closed at the API boundary (r9-3):** one typed
`RefNamespaceId{namespace, incarnation}` is required by every ref prefix/key/parser/cache helper and
the namespace-only overloads are DELETED — recovery/fold/fsck/sweep derive it from the catalog, live
readers from their handle — so dropping the incarnation is unrepresentable, not merely forbidden.

## 3. Lifecycles {#lifecycles}

**Creation** (three conditional writes, DDL-rate): catalog `Creating` → `_ckpt` create → catalog
`Live`; `Creating` forbids publication, and a stale `Creating` is reconciled by token-exact CAS only
after its creator's fence is terminal. **`Creating` forbids publication STRUCTURALLY, and no separate
write-path check enforces it:** a production writer cannot obtain a life while the entry is
`Creating`, because the only life-minting resolution loops until `Live` and is the sole source of a
life. A second gate on that path would be a fence over an already-fenced route, and its absence is a
decision — do not add one on the grounds that the invariant is unenforced. **But `DROP` is a lifecycle
mutation and MAY cancel a stalled `Creating`, which an ordinary read may not**: if the creator's fence
is still live, `DROP` returns the typed retry-later and changes nothing; if the fence is terminal, it
exact-CAS-deletes the complete observed `Creating` row and only then best-effort exact-token deletes the
old `_ckpt`. That order is load-bearing — reversed, a concurrent reconciler could publish a new `_ckpt`
which the losing `DROP` then deletes. A stop between the two is safe: the surviving `_ckpt` belongs to an
incarnation no entry names, so the new life gets a different one and the perpetual janitor reclaims it.
No terminal record, no `Removing` and no `RemovalReady` are involved, because publication out of
`Creating` is structurally impossible. **Removal:** catalog `Live → Removing` (the admission bound;
`Removing` forbids new positive ownership), then the terminal record — appended ONLY by the owning
mounted writer or a successor that has claimed and fenced that server root; GC surfaces stuck
removals, never appends, and — see INV-3 — GC is also the actor that performs the
`Removing → RemovalReady` CAS, threading its leader generation through the mutation like every other
fenced caller. The decommission actor's exact duties (catalog-exact enumeration, the
`Removing` recovery branch, both `RemovalReady` finalization branches, refusal of a
`Removing`-without-`_ckpt` corruption, and no slot retirement with owned entries) are register item R5
— a same-rollout dependency on the decommission spec. **The namespace-cleanup item carries the
incarnation captured at deposition, and a resumed pass NEVER re-derives it from the catalog** — a
re-derivation resolves a reborn name to the NEW life's incarnation and deletes live data (phase-0
model `CaRefNsCleanupStaleLeaderCore`); §2's "derive it from the catalog" applies to discovering
current lives, never to a deposited cleanup scope. **Recovery ownership:** the mount-fence
generation is captured at admission and required on every `slot-occupy`, `_ckpt` CAS and install;
self-remount cancels or waits out recovery before rearming. **Migration: recreate-only.** The pool
format bumps (writer generation AND backward floor); old-format startup fails closed naming pool
recreation; recreation must be quiesced so no old writer touches the reused prefix.

**Same-name reuse is supported, and no database engine is forbidden.** A namespace derived from a
table UUID effectively never recurs, because a recreated table mints a fresh UUID — but three routes
do reach the same `RootNamespace`, and all three are supported syntax rather than misuse: a second
`FREEZE ... WITH NAME` under a previously used name (a shadow namespace IS the literal backup
directory, so reuse there is routine by construction), explicit-UUID `ATTACH`/`CREATE` including
replicated DDL replay that carries the UUID it stored, and any database engine whose table path omits
the UUID. The incarnation is what makes all three safe uniformly, which is why **banning an engine was
considered and rejected**: it closes one route of three, and replicated-database recovery deliberately
creates an `Ordinary` database precisely in order to discard and recreate a table's UUID — so the ban
would break a supported recovery path and buy nothing. Under a UUID-less layout same-name rebirth
stops being rare and becomes ordinary; that is an argument for exercising the rebirth path in tests,
not for prohibiting the layout. Confining a generation token to the shadow path alone was also
rejected: two key grammars and two lifecycle variants cost more than one uniform incarnation, and
still leave the explicit-UUID route unguarded.

## 4. Recovery {#recovery}

Catalog (state + incarnation) → `_ckpt` → exact-key snapshot (revalidation rule) → arithmetic tail
(`last + 1`; hint omissions fetched by exact key; a 404 below a durable same-epoch higher id →
vanish-restart, then fail closed) → CAS-walk + seal → `_ckpt` CAS → install. Acked ⇒ durable ⇒
dense ⇒ found.

## 5. GC fold and deletion safety {#fold}

Per round: one catalog `GET`; ONE strict hint enumeration (intake, cleanup planning, defer). Fold
work advances hinted namespaces by arithmetic (`cursor + 1`; the `GET` per record was always owed;
hint holes — including the observed `0x1430c`/`0x1430d` shape — fold through unnoticed); epochs are
crossed only by consuming seals; impossible shapes (a 404 below a same-epoch witness — including one
that DISAPPEARS later: an above-cursor witness cannot be legitimately cleaned, so its disappearance
is corruption, never grounds for clearing — or an unconsumed-seal crossing) HOLD the namespace.

**Destructive-round frontier proof.** A round may run destructive work (condemnation, graduation,
deletion, sweep deletes, ref cleanup) only holding a frontier proof for EVERY `Live`/`Removing`
entry: hinted-active namespaces prove theirs by walking to an absent expected-next; every other
namespace gets one exact `GET cursor+1` (present → it was wrongly quiet, walk it). Budget exhausted
first → cursor advances may seal, all destruction suppressed. **The temporal lemma, normative (split per r9-2):** the proof is a snapshot, and the existing
machinery closes each window — a `+1` landing after its namespace's probe cannot lose data because a
newly condemned blob is not deleted in the same round, and for the already-delete-pending case each
writer path has its own closure: SOURCE-BACKED/TOKENED adoption reads `Condemned` meta and
rematerializes from source (the exact-token delete cannot remove the new incarnation), while
TOKENLESS relink (`adoptEvidence`) performs no meta read and is safe by ORDERING — the receiver's
`+1` is durable before the source releases its committed edge, so the blob inherits an unbroken
ownership chain; `Creating` cannot publish; `Removing` cannot add ownership; a late terminal record
only delays reclamation. **Third arm, found by the phase-0 model** (`CaRefDeltaIntakeCore`
`_sab_deleteignoresindeg`, RED): a `+1` that lands after its namespace's probe but BEFORE
condemnation hits a still-LIVE blob — no rematerialization triggers — and a later round folds it
while the pending exact-token delete still fires; the closure is the DELETE-SITE in-degree re-read
(the existing `deleteExact` liveness re-check), which is hereby NORMATIVE, not an optimization.
The model also proves a listing cannot be the SOLE witness source (it is a snapshot; a witness
durable after the enumeration is invisible to that round's probes) — `_ckpt.checkpoint` doubles as a
hint-independent second witness for the below-witness-404 hold, at zero cost (the fold already reads
it for cleanup ranges), closing the premature-cleanup class (`_fix_ckptwitness` green) but NOT the
general case: a gap above `_ckpt.checkpoint` in a namespace the hint never mentions stays invisible.
**Named residual (`_witness_corruptgap`, committed RED):** if an above-cursor record is lost to
CORRUPTION before any round observed a witness above it, and the hint never mentions the namespace,
the exact `GET cursor+1` honestly answers absent, the frontier proof is granted, and a `-1`
elsewhere can delete a blob an intact acked `+1` above the gap still names. The precondition
(silent loss of a durable object to point reads) is outside §1's trust model; the residual is
recorded here so it is a named exposure, not a silent one — the structural closure remains the
head-CAS alternative (§10 north star).

**The hold is durable and survives REBUILD.** `ShardCoverage::classification == 4` carries
`{reason, offending position, retry/backoff}` per `(namespace, incarnation)` — a strict grammar:
these fields required for classification 4, forbidden otherwise; the wire definition (a bounded
reason ENUM, numeric maxima, duplicate rules) and a byte-exact reservation including escaping,
framing and trailer growth are plan obligations with boundary and boundary-plus-one tests (r9-4). `suppress_destructive` = current
anomalies OR every carried hold, computed before EVERY destructive site. A carried hold forces an
exact retry of its offending position even when the hint omits the namespace, and clears ONLY by
folding through that position and adopting the result in `gc/state` — never by observing another
absent. **REBUILD carries every hold verbatim into the rebuilt baseline; with a missing or undecodable
prior seal REBUILD REFUSES, naming pool recreation as the recovery path** (r9-1: the "pool-wide-held
baseline" alternative is not representable in the per-namespace shapes and would need an invented
offending position that could never be folded through — the refusal branch is the already-safe one).

Cursors are keyed by catalog entries; unhinted namespaces carry verbatim. B1:
`logs_accounted == logs_applied` over the cut, `EpochSeal` an applied no-op (B2 `produced=false`).
Probe A: sampled, deterministic cadence, durable due/performed/skipped observability; aborts
nothing; the mount-time store gate (#23) is separate. Cleanup: covered logs are contiguous
computable ranges under `_ckpt.checkpoint` + cursor; crossed dead epochs delete as closed ranges.
Whole-round abort only for a key unattributable to any namespace.

## 6. Sweep deletion premise {#sweep}

Two rules (the sweep's own rework — S42, orphan-blob nomination, writer cleanup duties — is register
R2/R3 and lands as one coherent change referencing them):

- a manifest of an epoch-`E` build is deletable only when the cursor has consumed epoch `E`'s seal
  AND no unconsumed tail record above the cursor names it as a removal target (removals cross
  epochs; grants do not);
- on ANY uncertainty — unreached frontier, budget exhaustion, hold — retain; delay is never damage.

## 7. REBUILD and fsck {#rebuild-fsck}

REBUILD rebuilds cursors and edges from catalog + `_ckpt` + arithmetic tails, **condemns nothing**,
and preserves holds (§5). fsck: universe from the catalog, streams by arithmetic (`chain-broken`
fatal in summary AND exit code), tails above `_ckpt.checkpoint`, `unchecked` reserved for the
genuinely unproven; a healthy pool returns clean.

## 8. Costs, honestly {#costs}

Append path: +0 requests (the allocator gets simpler). Recovery: +1 conditional PUT per touched
namespace per epoch transition, and each adopted straggler costs a conditional PUT PLUS an exact
`GET` (`putIfAbsent` conflicts return no bytes — r9-6) + one `_ckpt` CAS. Snapshot publication:
+1 `_ckpt` CAS (async). DDL: three conditional writes to create; removal = one `gc/state` GET for the
diagnostic start round + `Live→Removing` CAS +
terminal append + one bounded best-effort cleanup pass + `Removing→RemovalReady` CAS + one pruning
round + exact `_ckpt` delete + catalog-entry CAS. Fold: +1
catalog `GET` per round; destructive rounds add one exact `GET` per quiet namespace (the frontier
proof); the perpetual janitor adds NO enumeration of its own, because a life's debris of every family
lies under the one `cas/ns/` subtree the round already enumerates. Deep arithmetic walks
dominate backlog cost and share one pool-level concurrency budget with cleanup. Performance interactions
with the measured GC study (3.42 M serial trips/round, 256 logs/s, 39.6 % manifest re-reads): P1 prefetch
becomes arithmetic (mispredictions impossible); ONE strict ref enumeration per round; range cleanup;
fsck replays tails. P2's round-scoped manifest cache and the
HEAD-per-edge veto are untouched; snapshot-diff folding is a BACKLOG future lever. At extreme
namespace counts the per-namespace frontier `GET` approaches the head-CAS design's read cost — the
recorded point to revisit that trade (§10).

## 9. Verification {#verification}

**TLA+ is phase 0** (models green before code; every property carries a `_sab_*` config proving it
can go red): `CaRefTableSnapshotLogCore` rewritten for INV-1/2/4 with `LatePredecessorPut` FLIPPED
from counterexample to proof; `CaRefDeltaIntakeCore` rewritten with **pool-wide state — one shared
blob, in-degree, condemnation phases, catalog sample, holds** — so the cross-namespace hidden-`+1`
sabotage, the temporal lemma's variants and the hold-then-`FORCE REBUILD` scenario are expressible;
`CaRelinkConfirmCore`'s `_sab_holeylist` becomes the fix's permanent regression witness;
`CaRefNsCleanupStaleLeaderCore` rewritten around catalog states + incarnations and cleanup-item
presence rather than a second item state; `CaRefCatalogCore` NEW (the four-state lifecycle, no-hold
transition, the ENTRY-COUNT half of capacity — the byte-arithmetic half is
the plan's boundary tests; `_ckpt` ordering, incarnation inertness; under-clean-only is gated in the
Task-1 module's `ckpt` rules, not here); `CaCasMountCore` extended (recovery generations; wedge-retry
vs successor-seal);
`CaRefWriterCleanupCore`/`CaRefFoldClampRecoveryCore` extended per register items when those land.

RED-first fault-injected controls, the load-bearing set: the cross-namespace hidden-`+1` vs visible
`-1` (dies without the frontier proof); held namespace → `FORCE REBUILD` → hint hides the witness →
`B:-1` (dies without hold carry); carried hold with the namespace omitted from the hint; late `+1`
after the probe during condemnation/graduation/deletion rounds;
`Creating`/`Removing`/`RemovalReady` catalog races and both checkpoint finalization windows;
ambiguous-then-definite id reuse; CAS-walk both directions incl. clean transitions and `T+1` retry;
`_ckpt` races (cleanup between PUT and CAS; stale base three-way; merge; no-op skip); recovery
across self-remount; incarnation inertness at rebirth under churn (create/drop per second: catalog
size stays flat); legacy-pool open fails closed; hold grammar strict codec; max-size seal write
check; and the four r9-5 sabotages: REBUILD with a missing/undecodable prior seal REFUSES (never
publishes a deletion-capable baseline); a held higher witness disappearing while the gap remains =
corruption; a ref key builder that drops the incarnation cannot compile/encode; an old-generation
wedge or recovery result returning after the successor sealed the slot is refused by a generation
recheck under the install lock (post-I/O, immediately before every install/unwedge/`_ckpt`
publication). The full enumerated list from rounds 5–9 rides in the implementation plan.

## 10. Alternatives and history {#alternatives}

| alternative | disposition |
|---|---|
| v1–v4 certificate stack (prev links, seal intervals, pointers, authorities, generations, `R*`, tombstones) | Rejected by the user as accretion; deleted. |
| Full head-CAS commit chain (blinded consult) | North star: revisit when the wedge is worth deleting or namespace counts make the frontier sweep expensive. v9 carries its catalog, checkpoint and (ref-layer-scoped) incarnations. |
| `seq_floor` in the catalog instead of incarnations | Rejected by the user's churn scenario: floors for dead names never retire → unbounded catalog. Incarnations make debris inert WITHOUT a physical-empty proof, so an entry deletes as soon as its removal completes rather than waiting on one. |
| Delete the incarnation entirely; forbid exact `RootNamespace` reuse forever | **Proposed and withdrawn, 2026-07-31**, after two independent reviews of the whole phase. It is coherent, and it needs somewhere to remember every retired name — which is the same shape as the `seq_floor` row above, so it fails for the same reason, one level up. A never-deleted `Retired` catalog state grows by one row per historical namespace and eventually refuses admission at the object cap; **normal UUID churn does not bound it**, since every fresh UUID also leaves a permanent row. No bounded exact compaction exists for opaque names, because compacting a retirement record and certifying physical emptiness are the same problem. A marker object outside the catalog bounds nothing either and reintroduces the marker class this design removes. Independently, permanent non-reuse is a regression against supported workflows (§3). |
| Retirement as a `Retired` catalog state, or as a marker object | Rejected with the row above; both are only needed if the incarnation is deleted. Keeping it means **nothing has to remember a dead namespace at all** — the entry is deleted, debris is inert by foreign prefix, and the catalog stays O(active). |
| Fresh-epoch rebirth | Failed round 6's audit (server-root-wide allocator; unqualified families). |
| Checkpoint inside the catalog; never cleaning covered logs; in-place migration; RefSnapLog combined state; local floors; enforced timing; widened probe A | Each rejected with its reason recorded in rounds 1–8 (`tmp/codex_r*_findings.md`) and the alternatives tables of v5–v8 (git history of this file). |

History in one paragraph: contiguity is the project's own I7 (2026-07-10) resurrected; the ghost was
the documented `Late Predecessor PUT` (rev.4), closed here within its own "no extra request per
ordinary mutation" constraint; eight adversarial rounds (`gpt-5.6-sol` `xhigh`) plus one blinded
simplification consult shaped the invariants — the full round-by-round record, including the two
user scope interventions that cut certificate accretion (after round 4) and scope accretion (after
round 8), lives in the review logs and this file's git history. Everything the rounds surfaced that
is NOT this blocker lives in `cas/2026-07-28-ref-rework-adjacent-findings.md`.

## 11. Out of scope {#out-of-scope}

Everything in the adjacent-findings register (R1 verbatim-file rebirth aliasing — pre-existing; R2
writer cleanup duties / build retirement; R3 orphan-blob nomination + S42 sweep rework; R4 REBUILD
condemnation + build/upload registry; R5 decommission duties — same-rollout dependency; R6 wedge
autonomy; R7 probe-A gating policy); the mount-time LIST probe (#23); snapshot-diff folding;
P1/P2/P3 and the rig (#10); the 56 leaked blobs; the `-1`-before-`+1` path; the RustFS mechanism.
