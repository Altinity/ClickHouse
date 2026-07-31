---
description: 'v9 CORE design for the LIST-incompleteness release blocker: per-namespace contiguous ref ids, in-band epoch seals, a namespace catalog with opaque life identities, a per-namespace checkpoint object, a mandatory destructive-round frontier proof and a REBUILD-surviving hold — LIST is a zero-trust hint; two new object kinds, both off the append hot path. Adjacent pre-existing defects surfaced by eight review rounds live in the companion register, not here.'
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

**INV-3 — the catalog, with opaque life identities.** `cas/ref_catalog`, token-CAS like `gc/state`:
`namespace → {state: Creating | Live | Removing, incarnation}` (+ creator fence identity while
`Creating`; immutable `removal_started_round` while `Removing`). The
removal actor samples the current adopted `gc/state.round` before `Live → Removing` and stores it in
the same catalog CAS; it is diagnostic age, never a safety fence. The incarnation (random 128-bit,
minted at `Creating`) qualifies every life-owned stream/state object:
the physical key families below, with a canonical grammar and refusal of legacy-shaped keys. The
same value is the opaque `life_id` those keys carry; the field is not renamed.
Removal has no fourth state and no pruning finalizer. `Creating → Live → Removing → absent`. The fold
that consumes the terminal attaches optional cleanup evidence directly to that life's adopted
fold-state row; presence is positive evidence that the terminal fold is durable, never a claim that
physical cleanup succeeded. Deletion additionally requires that the same row carries no durable hold.
In the same fenced round that adopts that seal, GC performs one bounded, suppression-aware best-effort
cleanup pass and exact-CAS-deletes the complete observed `Removing` catalog row before releasing the
round guard. No detached finalizer may reuse a historical seal. A stop before the CAS makes the next
round repeat the idempotent pass. The bounded physical pass obeys the ordinary destructive-suppression
gate and may therefore do zero work; LIST omissions, token mismatches and that skipped pass remain
leak-only and do not prevent the lifecycle CAS. The CAS has its own narrower proof: exact unique catalog
row, current adopted evidence row, no hold and current GC fence. Catalog ambiguity, an invalid adopted
seal or a lost fence fail closed before it. The bounded pass never deletes `_ckpt` while its `Removing`
row exists. `_ckpt` and
every missed object become ordinary dead-life debris owned by the
perpetual janitor; synchronous removal neither proves physical emptiness nor has a second physical
deletion window.

The old marker-driven `Pending → Completed` handshake is deleted with `_cleanup`: `RefNsCleanupState`,
the separate item collection and its wire `state` field all disappear. Cleanup evidence is just an
optional field of the life row.

**Why direct entry deletion is safe now, stated because it was unsound under the old shape.** The old
name-keyed cursor could be inherited by a same-name rebirth, so deleting the catalog row before pruning
was unsafe; while it remained `Removing`, every later round recreated the cursor. Neither premise
survives the opaque-id, single-producer design. The old row is keyed by the predecessor's random
`life_id`; a rebirth receives a different id; and the next catalog-built plan drops the absent old id.
The deletion-owning seal may still contain its completed predecessor row, but no operation for the new
life can address or inherit it. Waiting an extra round and representing that wait as `RemovalReady`
would preserve an enum state, transition, finalizer and recovery windows that prove no remaining safety
fact.

**There is one producer, not five predicates that must remain synchronized.** `CasFoldSeal` carries one
`ref_lives` map keyed by opaque `life_id`; each value is one `RefLifeFoldState{coverage,
optional cleanup evidence}`. The pure `buildRefWalkPlan(catalog_cut, inputs)` constructor creates rows
only in its catalog loop, and only for `Live` or `Removing`. Its input bundle contains parent coverage,
stream-LIST hints, carried holds and `_ckpt` observations; the corresponding internal adapters may
enrich an existing row but have no insertion API. The returned plan freezes its key set and exposes
only lookup/enrichment of an existing row, which lets folding attach newly earned cleanup evidence. REBUILD
calls the same constructor rather than rebuilding a second lifecycle predicate. Therefore
`Creating` and absent ids are structurally unable to acquire a walk, coverage or hold.
The old independent `per_ns_shard` and `ns_cleanup_items` collections, the string `"<namespace>/0"`
cursor grammar and `cursorKey`/`parseCursorKey` are deleted. Ref streams have one coverage value inside
their life row, not a separately keyed cursor or a fictional shard 0; blob-target GC sharding remains
separate and unchanged.

This shape also makes the cardinality bound executable: every newly encoded seal has at most one
ref-life row per `Live`/`Removing` catalog row in its cut, cleanup evidence belongs to that same row,
and the seal encoder rejects duplicate life ids. An
input row whose id the cut does not name is counted and dropped, never carried as an independent source
of work. A hint id absent from the earlier round cut is post-cut/unknown and deferred; it cannot mint a
row. A carried hold is legal only on the `Live`/`Removing` row that already owns it, and the no-hold
deletion precondition above remains an enforcement rather than a derivation.

One bounded residue remains: the deletion-owning seal contains the completed predecessor life row. The
next round's constructor observes the id absent from the catalog and omits the whole row. This is
bounded cleanup, not a deletion precondition: every consumer joins through the new cut before an input
row can become work, and new life has a different key.

No physical-empty proof is required anywhere: surviving old-incarnation objects are structurally inert
(an opaque life id absent from the authoritative catalog; the fold works only off catalog entries) and a
**perpetual** janitor deletes dead-life debris whenever listed. **Discovery is a separately paced,
leak-only enumeration of
`cas/ns/`** — never the round's hot `stream/` LIST, which must stay scoped to scheduling. It nominates
every family's debris: id-bearing stream objects, `_ckpt` (including the lone checkpoint left by a
cancelled `Creating`) and `_files` alike. It takes **one fresh catalog GET per page, not per key**, and
is paced by a single cleanup-only cursor. The mandatory order is `LIST page → fresh catalog GET/decode →
classify the whole page → exact-token deletes under the GC fence`. A malformed, oversized or backend-rejected
cursor fails closed for its current round/page: surface the error, perform zero janitor deletes, reset
only durable cleanup progress safely, and let a later normally scheduled round
begin at the start. The one post-page catalog cut is the
life revalidation for every key on that page; there is NO catalog read per key. Before each delete the
janitor rechecks the GC fence and uses the token captured for that exact object. A duplicate current
`life_id`, an unreadable catalog or a lost fence suppresses the page's deletes. Unparseable keys are
surfaced and skipped. This janitor is the only reclaimer of a dead life's objects; a LIST omission defers
its work but can never affect visibility, rebirth or deletion safety.

**Layout rule: object keys carry an OPAQUE life id, and the hot enumeration sees only the stream.**
A life's physical identity is the catalog's `incarnation` used as a pool-wide opaque `life_id`; keys do
not repeat the logical namespace at all:

```text
cas/ns/stream/<life_id>/_log/<txn>       cas/ns/state/<life_id>/_ckpt
cas/ns/stream/<life_id>/_snap/<txn>      cas/ns/state/<life_id>/_files/<relative-name>
```

**The split is immutable sequence-addressed stream versus point- and path-addressed state, not "logs
versus everything else".** `_snap` therefore belongs to `stream/`: it is immutable, addressed by
transaction id, already grouped with logs for recovery hints and covered-object cleanup, and small
beside `_files`. Moving it to `state/` would force either a second hot LIST or a simultaneous
recovery-and-cleanup redesign, which is a worse trade than the one it would buy.

**What the opacity buys is not tidiness — it is that the catalog becomes the only authority for mapping
a life-owned stream/state key to a logical name.** Such a key can no longer reconstruct a name, so no
stream/state parser can silently supply catalog state from debris. Manifests and loose `roots/` objects
retain path identity, but neither is an authority for a namespace life.

An **immutable catalog cut** is the decoded bytes and object token returned by ONE successful
`GET cas/ref_catalog`, together with the reverse index built exactly once from those bytes. A consumer
never patches that index from a later GET or mixes two cuts inside one decision: if it needs an id that
appeared after its cut, it restarts the whole decision with a fresh cut or defers it. Every catalog row —
`Creating`, `Live` and `Removing` — participates in the reverse index
`life_id → {name, incarnation}`. A `life_id` is never deliberately reused, so the existing random-128
uniqueness assumption now holds pool-wide rather than per logical name.

Two current rows sharing a `life_id` are `CORRUPTED_DATA`, never "first row wins". Both rows are
unresolvable. Catalog mutations, REBUILD, decommission, GC fold adoption and every destructive GC path
fail closed on that cut; the janitor may continue enumeration and diagnostics but deletes nothing.
Read-only fsck/inspect report both rows and continue over unrelated unique ids, and point I/O already
holding — or freshly resolving — an unrelated unique life may continue. Thus physical aliasing stops
pool-wide maintenance that depends on a complete ownership graph, not all unrelated data-plane I/O.

Absence from a cut is not by itself proof of absence from the catalog's future. A listed id is inert
debris only when the cut is known to have been read AFTER that object was observed. The janitor obtains
exactly that order by reading the catalog after each LIST page; catalog-before-object publication then
proves an id absent from the post-page cut is dead. The fold takes its round cut before the hot LIST, so
an id absent there is merely post-cut/unknown: ignore it as a scheduling hint and defer it to a later
round, never resolve it from key text and never delete on that observation.

**The round's one hot LIST covers `cas/ns/stream/` only** — `_ckpt` and `_files` are never enumerated by
it. That LIST stays what it always was: a scheduling and performance hint, never the correctness path,
which remains catalog membership plus exact arithmetic reads and the frontier probe. Dead-life debris of
every family is reclaimed by a separately paced, leak-only enumeration of `cas/ns/`, which takes one
fresh catalog GET per page rather than per key.

**Rejected, and recorded so the cheaper-looking options are not re-proposed.** Deleting the full LIST in
favour of an unbounded serial `GET N+1` chase has no bounded frontier while a namespace is being written
and throws away the listing's scheduling witness. Adding an authoritative head object would put a CAS on
the append path, which this design exists to avoid. Storing the logical path in the key — as `_path` or
as a `roots/<path>` pointer — would create a second thing that looks like authority with no reader that
needs it; a backup that can be mistaken for authority is worse than none, and the catalog already
answers introspection for every active life. Path-shaped browsing is a **user** concern, served by the
disk's logical `listDirectory` through `clickhouse-disks` and by the inspection tool.

`roots/` keeps only objects no CAS catalog owns: loose mountpoint objects mirrored at their ClickHouse
path, with no namespace, no life id and no reserved wrapper.

**Mount safety changes premise, not strength, and this is the one place the split could have opened a
hole.** There is no `cas/ns/<srid>/` prefix to probe any more, because a physical key no longer names a
server root. So a `server_root_id` has owned live namespace work **iff the mandatory catalog holds a
`Creating`, `Live` or `Removing` logical name under that root** — the catalog
observation is threaded down from the pool layer rather than having the low-level module decode a
catalog of its own. `cas/manifests/<srid>/` and `roots/<srid>/` are still probed physically, because
those families kept path identity. Dead opaque stream or state debris alone must NOT block owner or
epoch recreation, and an unreadable catalog must fail closed rather than fall back to a physical
guess. "Under that root" is a canonical path-component relation, never raw string `starts_with`. The
successfully decoded mandatory catalog observation and both physical probes form one precondition bundle
for the absent-owner and absent-epoch paths. If either conditional create conflicts, its retry recomputes
the whole bundle rather than reusing a stale "no owned work" observation.

**A same-name birth is refused while the predecessor is `Removing`**, as a typed retry-later that names
what it waits for — never an internal error. The window spans the terminal fold, one bounded cleanup
pass and the exact catalog deletion; it no longer includes a pruning round. `CREATE` may wake the
existing GC scheduler, then returns retry-later. It does not fold a terminal, run cleanup, drive a GC
round or mutate the predecessor. No wait loops, no assistance protocol and no second transition driver.
**The catalog stays O(`Creating` + `Live` + `Removing`) under any create/drop churn** (stalled
creators occupy entries until fence-terminal reconciliation — r9-6). Manifests keep their
`(namespace, mount-epoch, build-sequence)` identity — mount-global build ids already prevent
rebirth aliasing; namespace files use the opaque life-owned state prefix above, while loose mountpoint
objects remain outside catalog ownership. Capacity: namespace names
get a byte bound; the creation CAS checks an additive predicate (encoded catalog + a per-entry
worst-form ref-life-row reservation vs both the catalog and fold-seal caps — the catalog is the
serialized admission ledger); `encodeFoldSeal(...).size()` is checked against the cap before every
PUT. Admission refuses loudly; removal is never refused. **The reservation is deliberately
over-covering, not exact.** The asymmetry decides it: over-charging costs admitted namespaces, while
under-charging wedges the fold round, because the seal is then refused on every attempt. So the
reservation is a conservative constant per counted catalog row plus fixed structural headroom for rows
that are not one-for-one with catalog entries: `btr` rows per admitted run segment and `cnd` rows per GC
shard. The per-entry constant covers one worst-form `RefLifeFoldState`, including its optional cleanup
evidence and hold; separate cursor and `nsc` index-set arithmetic no longer exists. An already adopted
seal may retain a row whose catalog entry was just deleted, but that seal was already cap-checked and
the next encoder drops the id before admitting adapters. New catalog rows and old adopted rows are never
combined in one encoded `ref_lives` map. Exactness here buys a handful of extra admissions and costs a boundary-arithmetic proof
obligation that has to be re-derived on every format change; do not restore it.

> **Amendment history, consolidated 2026-07-31.** The 2026-07-29 namespace-life amendment first made
> namespace files incarnation-qualified and required one typed
> `NamespaceLifeId{namespace, incarnation}` at every ref and namespace-file API. Its literal
> `roots/<namespace>/<incarnation>/_files/` placement is superseded by this invariant's opaque
> `cas/ns/state/<life_id>/_files/` grammar; its hot-path request-count, captured-life and stale-handle
> requirements remain authoritative. Manifests and loose mountpoint objects are unchanged.
>
> The `_cleanup` object class and the `Removed` snapshot die together. A marker that must outlive every
> possibly-stalled actor is unbounded,
> and the recreate gate it fed is replaced by the catalog cut: a reader answers absence from
> `Removing` or an absent entry, so the `Removed` lifecycle snapshot loses its last
> consumer. Fresh name resolution enforces that cut; an already-held life handle keeps the explicit
> stale-or-`NotFound` contract without a hot-path catalog read. `namespaceIsRemoved` is therefore
> deleted with the snapshot. The two die together — leaving either one keeps a physical-empty vestige
> alive with no reader.

**INV-4 — `_ckpt`.** `cas/ns/state/<life_id>/_ckpt`, token-CAS,
`{life_epoch, checkpoint_snapshot_id | none, last_epoch_seal | none}` — forced by prefix cleaning
(a cleaned prefix plus a hidden snapshot is indistinguishable from empty). One update algorithm for
both writers (snapshot publisher; sealer): read → validate → merge by semantic maximum per field →
token-CAS; identical merged body → return without a CAS; retries bound to the recovery deadline.
Snapshots are deletable only STRICTLY BELOW `_ckpt.checkpoint` (a stale pointer can only
under-clean). Missing sampled base → reread `_ckpt`: token advanced → restart; unchanged →
corruption. Removal does not synchronously delete `_ckpt`: once the completed, unheld ref-life row is
durable, the catalog row is the only lifecycle mutation. After its exact deletion, `_ckpt` is inert
dead-life debris and the perpetual janitor exact-token-deletes it when listed.

**Entry absence is authoritative, and the catalog is mandatory.** A canonical object whose incarnation
no catalog entry names is **inert debris, never evidence of damage** — a legal removal and a fabricated
entry loss leave byte-identical stores once the ref-life row is gone, so no classifier can separate them
and none may try. What protects an entry is therefore not detection but the mutation API: the only
exported deletion transitions are (1) exact-CAS cancellation of a complete observed `Creating` row
after its creator fence is terminal, and (2) exact-CAS deletion of a complete observed `Removing` row
by fenced GC after the adopted matching ref-life row has cleanup evidence and no hold. No `Live` row is
deletable, an unproved `Removing` row is not deletable, a live-fenced `Creating` is not deletable, and
there is no generic remove-by-name mutation. An input ref-life row whose
opaque id no catalog entry names is **counted and logged, then dropped, never suppressed on** — the
measured pool-wide reclamation stall must not return, but silent omission would mask corruption or a
defect at the sole plan-construction boundary. Correspondingly, after pool
bootstrap an absent or undecodable `cas/ref_catalog` is `CORRUPTED_DATA` and stops creation, removal and
GC: bootstrap creates the empty catalog and marks the pool ready, and nothing may ever bootstrap an
empty catalog again. This removes the "vanished catalog reads as virgin" special case rather than
adding lifecycle design.

**Two failures that look alike and must not be treated alike.** A terminal record that is unreadable
**before** it folds is a loss of EVIDENCE: removal legitimately blocks in `Removing`, and the operator
must see a terminal-corrupt stuck removal. Do not promise `REBUILD` as the escape — nothing establishes
that it can reconstruct that exact terminal; the credible exits are restoring the object or recreating
the pool. A `Removing` row whose matching fold-state row has no cleanup evidence is reported once the
current round reaches
an overflow-safe age of N rounds, on every round thereafter; the catalog field makes the threshold
stable across restart. A recorded exact-read failure names the unreadable key; otherwise the diagnostic
says only that the terminal has not folded, without inventing a terminal key the catalog does not store.
Once the terminal HAS folded and the adopted life row carries the cleanup evidence derived from it, the
object's later inaccessibility matters only for physically deleting manifests. That is a cleanup failure
and stays **leak-only**: entry deletion lands, and the
manifests remain orphan debris under a non-suppressing leak counter. Any other reading lets physical
cleanup back into the lifecycle through a side door.

**Read-side contract, stated honestly:** life-owned readers hold `NamespaceLifeId{namespace,
incarnation}` and can never alias a new life (a foreign opaque-life prefix); a stale reader gets
stale-or-`NotFound`, not rejection —
rejection would need a fence/catalog read that hot paths do not pay. Destructive cleanup revalidates
life and fence at the destructive protocol's required cut/fence boundary. **Closed at the API boundary
(r9-3):** one typed `NamespaceLifeId{namespace, incarnation}` is required by every ref and
namespace-file prefix/key/cache helper and
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
exact-CAS-deletes the complete observed `Creating` row and performs no physical delete. The surviving
`_ckpt` belongs to an incarnation no entry names, so the new life gets a different one and the perpetual
janitor reclaims it. This also removes the race in which a losing `DROP` could delete a checkpoint just
published by a concurrent reconciler.
No terminal record and no `Removing` are involved, because publication out of
`Creating` is structurally impossible. **Removal:** under the local append lane, serialize catalog
`Live → Removing` as the admission bound: once `Removing` is observed, an already-held runtime or
handle cannot reopen positive append admission. A catalog CAS retry keeps admission closed. A failure
before the transition becomes durable may reopen only after a fresh exact catalog observation still
proves `Live` under the same life and fence; otherwise it fails closed. The terminal record follows
under that same admitted removal ownership and is appended ONLY by the owning mounted writer or a
successor that has claimed and fenced that server root; GC surfaces stuck
removals, never appends, and — see INV-3 — GC exact-CAS-deletes the proved complete `Removing` row,
threading its leader generation through the mutation like every other fenced caller. The decommission
actor's exact duties (catalog-exact enumeration, the `Removing` recovery branch, refusal of a
`Removing`-without-`_ckpt` corruption, and no slot retirement with owned entries) are register item R5
— a same-rollout dependency on the decommission spec. **The cleanup work captures the ref-life row's
opaque `life_id` at deposition, and a resumed pass NEVER re-derives it from a logical name in a later
catalog cut** — a re-derivation resolves a reborn name to the NEW life's incarnation and deletes live data (phase-0
model `CaRefNsCleanupStaleLeaderCore`); §2's "derive it from the catalog" applies to discovering
current lives, never to a deposited cleanup scope. The logical name may accompany an operation as a
diagnostic or as part of the exact observed catalog row, but is not duplicated in cleanup evidence and
cannot redirect physical cleanup. **Recovery ownership:** the mount-fence
generation is captured at admission and required on every `slot-occupy`, `_ckpt` CAS and install;
self-remount cancels or waits out recovery before rearming. **Migration: recreate-only.** Task 4d's
layout is generation 6 and Task 5's incompatible `ref_lives`/`_cleanup` wire removal is generation 7;
each pool format bump advances writer generation AND backward floor. Older-format startup fails closed
at pool open naming recreation; there is no dual reader or migration, and recreation must be quiesced
so no old writer touches the reused prefix.

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

The fold takes one immutable catalog cut per round and performs ONE strict `cas/ns/stream/` hint
enumeration (intake, covered-stream cleanup planning, defer). The separately paced dead-life janitor is
not part of this hot enumeration; when scheduled, it takes one bounded `cas/ns/` page and its own
post-page catalog cut as specified by INV-3. Fold
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
framing and trailer growth are plan obligations with boundary and boundary-plus-one tests (r9-4).
`suppress_destructive = anomalies || carried_holds || !frontier_complete`, computed before EVERY
destructive site. Every carried hold is pool-wide suppression: blob in-degree is pool-wide and no
ownership-partition proof permits namespace-local reclamation. A carried hold forces an
exact retry of its offending position even when the hint omits the namespace, and clears ONLY by
folding through that position and adopting the result in `gc/state` — never by observing another
absent. **REBUILD carries every hold verbatim into the rebuilt baseline; with a missing or undecodable
prior seal REBUILD REFUSES, naming pool recreation as the recovery path** (r9-1: the "pool-wide-held
baseline" alternative is not representable in the per-namespace shapes and would need an invented
offending position that could never be folded through — the refusal branch is the already-safe one).

Ref coverage is keyed by opaque current `life_id`; a catalog-created unhinted plan row carries its
matching parent coverage verbatim. An input life row absent from the cut is counted and dropped. B1:
`logs_accounted == logs_applied` over the cut, `EpochSeal` an applied no-op (B2 `produced=false`).
Probe A: sampled, deterministic cadence, durable due/performed/skipped observability; aborts
nothing; the mount-time store gate (#23) is separate. Cleanup: covered logs are contiguous
computable ranges under `_ckpt.checkpoint` + cursor; crossed dead epochs delete as closed ranges.
A malformed physical stream key, an ambiguous current `life_id` or a generation-5 current-name mismatch
may abort ref folding and suppress destruction. A well-formed opaque id absent from the round's earlier
catalog cut does not: it is post-cut/unknown and is deferred. A well-formed id absent from the janitor's
later post-page cut is dead-life debris and is handled by that leak-only path.

## 6. Sweep deletion premise {#sweep}

Two rules (the sweep's own rework — S42, orphan-blob nomination, writer cleanup duties — is register
R2/R3 and lands as one coherent change referencing them):

- a manifest of an epoch-`E` build is deletable only when the cursor has consumed epoch `E`'s seal
  AND no unconsumed tail record above the cursor names it as a removal target (removals cross
  epochs; grants do not);
- on ANY uncertainty — unreached frontier, budget exhaustion, hold — retain; delay is never damage.

## 7. REBUILD and fsck {#rebuild-fsck}

REBUILD rebuilds ref-life coverage and edges from catalog + `_ckpt` + arithmetic tails, **condemns nothing**,
and preserves holds (§5). REBUILD uses the same catalog-only walk-plan constructor as an ordinary fold;
every listed physical id is joined through its one catalog cut, and no other input can create a
`ref_lives` row. It therefore constructs coverage only for an id that cut names as `Live`/`Removing`,
never creates a phantom logical namespace from an absent id, and refuses an ambiguous current id. fsck
takes the same catalog-authoritative
universe and reverse-index rule, but as a read-only diagnostic it reports duplicate rows, malformed
keys and absent physical ids and continues over unrelated unique lives. It walks streams by arithmetic
(`chain-broken` fatal in summary AND exit code), checks tails above `_ckpt.checkpoint`, reserves
`unchecked` for the genuinely unproven, and returns clean for a healthy pool.

## 8. Costs, honestly {#costs}

Append path: +0 requests (the allocator gets simpler). Recovery: +1 conditional PUT per touched
namespace per epoch transition, and each adopted straggler costs a conditional PUT PLUS an exact
`GET` (`putIfAbsent` conflicts return no bytes — r9-6) + one `_ckpt` CAS. Snapshot publication:
+1 `_ckpt` CAS (async). DDL: three conditional writes to create; removal = one `gc/state` GET for the
diagnostic start round + append-lane-admitted `Live→Removing` CAS +
terminal append + one bounded best-effort cleanup pass + one exact catalog-entry CAS. Fold: +1
catalog `GET` per fold round; destructive rounds add one exact `GET` per quiet namespace (the frontier
proof). When scheduled, the perpetual janitor adds one bounded page of the separately paced leak-only
`cas/ns/` enumeration plus one catalog GET after that page; it never widens or repeats the round's hot
`stream/` LIST and never reads the catalog per key. Deep arithmetic walks
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
`CaRefNsCleanupStaleLeaderCore` rewritten around catalog states + incarnations and per-life cleanup
evidence rather than a second item state; `CaRefCatalogCore` NEW (the three-state lifecycle, positive
cleanup-evidence/no-hold deletion precondition, the ENTRY-COUNT half of capacity — the byte-arithmetic
half is the plan's boundary tests; direct deletion and incarnation inertness; under-clean-only is gated in the
Task-1 module's `ckpt` rules, not here); `CaCasMountCore` extended (recovery generations; wedge-retry
vs successor-seal);
`CaRefWriterCleanupCore`/`CaRefFoldClampRecoveryCore` extended per register items when those land.
`CaRefDeltaIntakeCore` additionally asserts that the ref walk-plan key set is exactly the set of catalog
`Live`/`Removing` ids; sabotage lets a parent, hint, hold or checkpoint adapter mint one forbidden row.

RED-first fault-injected controls, the load-bearing set: the cross-namespace hidden-`+1` vs visible
`-1` (dies without the frontier proof); held namespace → `FORCE REBUILD` → hint hides the witness →
`B:-1` (dies without hold carry); carried hold with the namespace omitted from the hint; late `+1`
after the probe during condemnation/graduation/deletion rounds;
`Creating`/`Removing` catalog races and deletion with a stale or held ref-life row; a cached writer
paused across `Live→Removing` cannot append positive ownership, including CAS-retry and fence-change
admission cases;
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
| Full head-CAS commit chain (blinded consult) | North star: revisit when the wedge is worth deleting or namespace counts make the frontier sweep expensive. v9 carries its catalog, checkpoint and opaque life identities. |
| `seq_floor` in the catalog instead of incarnations | Rejected by the user's churn scenario: floors for dead names never retire → unbounded catalog. Incarnations make debris inert WITHOUT a physical-empty proof, so an entry deletes as soon as its removal completes rather than waiting on one. |
| Delete the incarnation entirely; forbid exact `RootNamespace` reuse forever | **Proposed and withdrawn, 2026-07-31**, after two independent reviews of the whole phase. It is coherent, and it needs somewhere to remember every retired name — which is the same shape as the `seq_floor` row above, so it fails for the same reason, one level up. A never-deleted `Retired` catalog state grows by one row per historical namespace and eventually refuses admission at the object cap; **normal UUID churn does not bound it**, since every fresh UUID also leaves a permanent row. No bounded exact compaction exists for opaque names, because compacting a retirement record and certifying physical emptiness are the same problem. A marker object outside the catalog bounds nothing either and reintroduces the marker class this design removes. Independently, permanent non-reuse is a regression against supported workflows (§3). |
| Retirement as a `Retired` catalog state, or as a marker object | Rejected with the row above; both are only needed if the incarnation is deleted. Keeping it means **nothing has to remember a dead namespace at all** — the entry is deleted, debris is inert under its unreferenced opaque life id, and the catalog stays O(active). |
| Namespace-local hold suppression | Deferred: blob in-degree is pool-wide, and no ownership-partition proof shows a held namespace cannot own a blob nominated by another namespace. The global destructive gate remains authoritative. |
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
