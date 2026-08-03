---
description: 'A chronological, trial-and-error account of how the content-addressed (CAS) MergeTree design was reached: every architectural turn, the counterexample or review or soak finding that forced it, and the approach it replaced. The narrative companion to 01-architecture (the what), 02-methodology (the how), and 06-tla-models (the proofs).'
sidebar_label: 'How we got here'
sidebar_position: 11
slug: /superpowers/cas/how-we-got-here
title: 'CAS MergeTree — How We Got Here (by Trial and Error)'
doc_type: 'guide'
---

# CAS MergeTree — How We Got Here (by Trial and Error) {#cas-how-we-got-here}

**Status:** narrative history; covers `2026-06-01 → 2026-08-03` (merge-base `2ed6626a25e` → HEAD of
`cas-gc-rebuild`, ~3600 commits; §§18–22 cover `3693e1d → 4bc136c` alone, ~1100 commits in nine days).

The other documents in this set describe the design as it *is*: `01-architecture.md` (the object model),
`03-writer-protocol.md` / `04-gc-protocol.md` / `09-read-protocol.md` (the protocols),
`06-tla-models.md` (the proofs). This document describes how the design *became* what it is — the wrong
turns, the counterexamples, the mechanisms built and then deleted. Almost nothing here was designed
correctly on the first try. The value of the record is not the destination but the sequence of errors that
ruled out the alternatives, because most of those alternatives look reasonable until you see the
counterexample.

Read this alongside `02-methodology.md`, which explains *why* the process caught these errors; this
document is the catalogue of the errors themselves.

---

## 1. The thesis: a design search with three independent oracles {#thesis}

Every architectural turn below was forced by one of three filters, applied in this order:

1. **Adversarial design review, before any code or model.** A simplicity/performance persona (a "virtual
   A. Milovidov") and a distributed-systems happens-before review attacked each specification. This filter
   killed the first GC design outright (§3).
2. **TLA+ as a hard pre-implementation gate.** *No code task in a behavior-changing phase began until the
   relevant TLA+ model was green.* "Green" had a precise, unusually strict definition: every safety and
   liveness stage HOLDS, **and** every negative control (a deliberately sabotaged variant, named `sab_*`)
   VIOLATES the specific invariant it targets. A negative control that fails to reproduce its named
   counterexample was treated as *as bad as* a safety violation — it means the model does not actually cover
   the case. This "sabotage-or-it-didn't-happen" discipline is what makes the corpus trustworthy: every
   safety rule ships with the counterexample that appears when you remove it.
3. **A deterministic chaos soak as an empirical oracle.** Two `ReplicatedMergeTree` replicas sharing one CA
   pool, a seeded workload, a seeded fault injector, and quiesced checkpoints that cross-check SQL results
   against a model oracle and run `clickhouse-disks ca-fsck` + `ca-gc-dryrun` for `dangling=0`.

The recurring lesson is that these three catch **different** classes of error, and none is sufficient
alone. TLA+ proved design constraints before a line of C++ existed (the two-coordinate registry proof, the
build-root necessity proof). The soak found bugs the idealized models abstracted away (the B140 dangle, the
resurrect-reupload orphan). External code review found a bug neither model nor soak did (the condemn-marker
swallow, §14). The design's confidence rests on all three plus deterministic gtests — never on the models
alone.

A second recurring lesson governs *what* gets rejected. A mechanism was killed on sight if it (a) required
Keeper for **correctness** (not just coordination), (b) put load on Keeper or S3 **proportional to data
volume**, (c) let a single stuck or wedged writer stall reclamation **pool-wide**, (d) could ever
*accelerate* a delete past its safety gates (only mechanisms that can *delay* a delete are acceptable —
"over-count only"), or (e) coupled a generic `ReplicatedMergeTree` / Keeper surface to the CA disk
implementation. Nearly every pivot below traces back to one of these five.

---

## 2. The starting point (2026-06-01) {#starting-point}

The branch opens with a working proof-of-concept and a one-paragraph thesis: **"git for `MergeTree`."**
Store every part file once, keyed by its content hash (`blobs/<hash>`); let multiple replicas share one
object-storage pool with no byte duplication; make part identity a hash of a tree of hashes, like a git
tree. A rapid week of specifications followed — `ReplicatedMergeTree` on a CA disk (fetch-by-relink instead
of byte transfer), MVCC transactions, `BACKUP`/`RESTORE`, and a GC umbrella spec — each a separate integration
point of a content-addressed disk under an existing `MergeTree` feature.

A decision made in this first week and never reversed: **CAS coexists with zero-copy replication; it does
not replace it** (commit `129ba00`, "B1"). `metadata_type = content_addressed` is opt-in per disk. This is
lesson (e) applied pre-emptively — removing zero-copy would have forced a migration and coupled every
existing deployment to the new code.

Everything else in the first design was wrong, or at least did not survive contact with a model.

---

## 3. Turn 1 — the generation-in-the-key dead end (EBR → incarnation tokens) {#turn-ebr}

**What we had.** The first GC was an **Epoch-Based Reclamation (EBR)** core, modelled as `CaGcCore.tla`.
Blob keys carried a generation: `blobs/<H>/<g>`. Writers held a "pin epoch" (an ephemeral Keeper node); GC
advanced a `safe_epoch` and could only reclaim once every writer had advanced past it. Reads that missed
the current generation fell back to a `404 → LIST` degraded path. Merkle tree objects (`trees/<T>`) carried
a `child_gen` *inside* the tree's identity.

**What broke.** Two things, in sequence.

First, the 2026-06-07 adversarial reviews attacked the specification directly and found the **D6
write-ahead-intent** mechanism — per-commit, per-file *persistent* Keeper writes to track orphan builds —
was both unsafe and unaffordable:

- The distributed review: the intent key `leases/<epoch>/<key>` **collided across writers building
  identical content**, so owner attribution broke; and the "in-degree == 0 alone" reclaim guard was
  over-stated — in the window between deciding to reference a blob and that reference becoming durable, the
  fold legitimately reads in-degree 0 for a *live* blob.
- The simplicity review: D6 put `O(files)` persistent Keeper writes on **every commit**, re-introducing
  exactly the data-proportional Keeper traffic that made zero-copy replication painful. The verdict was
  blunt: *"CUT it; crash debris goes to the periodic Retention-guarded sweep."*

D6 was cut entirely — lesson (b). But the deeper problem was structural. The EBR model itself, while its
positive stages passed, only passed *because* four counterexamples found during its development had been
encoded as rules (the flush-`+`-then-advance ordering; reuse must target an observed epoch; a self-fence on
a local elapsed-time deadline is required to close the session-expiry gap). And the whole generation-in-key
scheme carried permanent costs: the `404 → LIST` read path, `child_gen` ancestor rebuilds (a reclaim at any
child propagated a new generation up the entire tree chain), durable per-hash floors, and **Keeper required
for correctness** — lesson (a). Worst of all, a single stuck writer held down `safe_epoch` and stalled
reclamation of *all* dropped parts across *all* writers — lesson (c).

**The turn.** The 2026-06-10 **incarnation-token** redesign (`5624e67`) moved the resurrection marker *into
the object body* and deletion precision *into the backend token* (the S3 ETag / conditional-op token),
eliminating the generation from every key. Part identity became a monotone composite `ManifestId`, not a
content hash. The `404 → LIST` path disappeared. There is no `safe_epoch` for a stuck writer to pin, so a
wedged writer can no longer stall pool-wide reclamation at all. Keeper became optional.

This design was then subjected to the most intensive verification in the corpus. `CaIncarnationCore.tla`
carries four safety invariants (`INV_NO_DANGLE`, `INV_NO_LOSS`, `INV_NO_RETURN`, `INV_JOURNAL_COVERAGE`)
across the full adversarial interleaving; a 2026-06-11 hunt ran 782M distinct states by BFS plus 8.3
billion deep random visits with zero violations, and an Apalache run
(`CaIncarnationProofCore.tla`) machine-checked an *inductive* invariant (stronger than bounded model
checking) for the single-leader fragment. Eleven negative controls each encode a load-bearing rule — the
fence must write to *every* manifest, not just active ones (`sab_nofence`); deletes must be exact-token
(`sab_unconddelete`); resurrect must mint a *fresh* tag (`sab_reusedtag`); the registry fence must use the
*committed*, not fold-time, universe (`sab_foldtimeuniverse`, which "was found as a real C++ hole"). This
model remains the design's **architecture-independent safety spine** — its concrete structure has since
been superseded, but its invariant proofs are still canonical.

The Merkle tree layer went with EBR: `ObjectKind::Tree`, the `trees/` prefix, and `child_gen` were removed
entirely (2026-07-03). Trees became manifest-internal; `Blob` is the sole object kind. The generation-in-key
idea, notably, was *proposed and killed three separate times* over the branch's life (see §13 and §16) — it
looks reasonable each time.

---

## 4. Turn 2 — integer refcount → source-edge set {#turn-source-edge}

**What we had.** Blob liveness tracked by an integer in-degree counter, incremented per reference and
decremented per release.

**What broke.** The same 2026-06-07 distributed review that killed D6 found the precise race: in the
decide-to-reference-then-not-yet-durable window, the fold reads in-degree 0 for a live blob, and an
"in-degree == 0" guard would reclaim it → a dangle. More generally, a mutable counter on S3 costs a CAS
round-trip per write *and* per drop (mutable object metadata is write-once-per-upload; changing it requires
a copy), which is proportional to data volume — lesson (b) — and distributed decrement is a classic hazard:
a missed decrement leaks, a premature one dangles.

**The turn.** Do not store the count at all. The GC log records **source edges** — `+`/`-` deltas keyed by
`(kind, hash, source_manifest)` — and a fold computes net in-degree from the edge multiset. The invariant
`BlobInDegreeMatchesActiveManifests` asserts the edge multiset is exactly the set of active manifests
referencing the blob. Because the fold is idempotent (set semantics), losing or duplicating a record can
only *delay* reclamation, never accelerate a delete — lesson (d). The `+`-not-yet-durable problem became a
pending-fold problem (the fold barrier withholds any `+` until the body is confirmed present) rather than a
guard problem. This is the moment the design committed to "the count is derived, never stored," a principle
that survives to HEAD.

---

## 5. Turn 3 — format convergence: abandon JSON, delete the packs and the heartbeat (2026-06-24 → 26) {#turn-format}

A deliberate format-freeze epoch. Early objects used several encodings, including JSON for some structures,
"pack" objects that bundled small files, and a separate build-heartbeat object.

**What broke.** Nothing dramatic — this was simplification under review, not a bug. But the direction is
instructive: every optional encoding was a surface where an invariant could be violated or a schema could
drift. The `/simplify` passes of early June had already shown that co-locating related entities shrinks the
surface where invariants break (the architecture reorg pulled `RefPayload`, `ObjectIO`, `GcLayout`, and the
write buffers into cohesive files and cut `ContentAddressedTransaction.cpp` from 2505 to 2243 lines).

**The turn.** JSON was abandoned entirely (`084dc46`) in favour of exactly two encodings: a binary hashed
format for immutable content and protobuf for the mutable hot path. Pack objects were deleted as dead code
(`1a8188b`); the build heartbeat, which was write-only and never read, was removed (`ae28447`); headers were
unified into one `CasHeader` envelope with a schema-evolution framework (`fa4d950`) so future format
changes are self-describing rather than ad-hoc. The header checksum moved from CRC-32C to CityHash64
(`8e48d8d`), and the tree hash to `cityHash128`. RustFS was chosen as the testbed backend because it passes
the full conditional-write battery (`da08ac6`); Garage was evaluated and **rejected** because it silently
ignores conditional operations (`215ad90`) — a backend that ignores `If-None-Match` cannot enforce
write-once, so it cannot host content-addressed storage at all.

(The format kept maturing after the freeze — see §16 on codecs v3, which eventually made *everything* a
text-friendly format.)

---

## 6. Turn 4 — the B140 dangle: revocable per-blob hints → build-root reachability {#turn-b140}

This is the canonical "the soak found what the model abstracted away" story.

**What we had.** A blob was protected from GC by a **revocable per-blob hint** — `cas_owner` /
`protectedByLiveBuild` — bound to the byte-writer that first uploaded it, transferred to an adopter by the
`reuseBlob` (dedup-adopt) operation.

**What broke.** In a 2026-06-18 soak run, `system.content_addressed_log` (the B170 event log built into the
soak precisely to make such events observable) pinned a **dangle**: a blob GC-deleted while still referenced
by a committed part. Root cause: `reuseBlob` transferred the blob *hash* but not the `cas_owner`
protection, so an adopted blob's protection remained bound to a *retired* byte-writer. GC reclaimed it under
a still-in-flight adopter. A revocable hint is fragile precisely because the revocation and the adoption are
separate steps.

**The turn.** Protection became **reachability from a durable build root**, a structural property, not a
revocable hint. Before any C++ changed, `CaBuildRootPrecommit.tla` proved the fix with a 2×2
necessity/sufficiency matrix: with no build root, the counterexample reproduces the soak dangle exactly
(`WriteBlob → AdoptBlob → BuildDie → GcDelete → Commit-with-no-presence-check`, 6 states); build-root
*alone* still dangles through an ordering window; fail-closed commit *alone* is clean only vacuously.
**Both halves are independently necessary and jointly sufficient** — a shape that recurs throughout the
design (§9, §12). A later extension caught a second bug in the same area (`lazyleak`): a lazy closure read
of an *absent* tree records an empty closure, so the blob never enters the "ever-snapped" set and can leak
forever — the precommit must record its closure structure, not read it lazily.

The whole revocable-hint vocabulary (`cas_owner`, `protectedByLiveBuild`, `reuseBlob` protection transfer,
seal-TTL) was deleted. The 2026-06-22 model-currency audit later confirmed none of it survives anywhere.

---

## 7. Turn 5 — the build-watermark livelock, and a proof that outlived its design (B167) {#turn-b167}

**What we had.** After the build-root turn, an intermediate mechanism (B167) tried to keep a freshly
re-uploaded blob alive against a GC that had just condemned its stale predecessor incarnation, using a
per-candidate blob guard (`protectedByLiveBuild`) and a build watermark.

**What broke.** `CaResurrectLiveness.tla` found a **livelock lasso**: a stale-incarnation dedup hit →
`GcDelete` → fresh `BuildUpload` → GC re-condemns the build's *own* fresh incarnation in the
non-atomic upload-to-publish gap → `GcDelete` → forever, `published` never becomes true. The key insight is
that the upload→publish span is not atomic, so writer-side re-upload *alone* is starvable. Two companion
models (`CaBuildWatermark`, `CaBuildWatermarkNum`) proved that the watermark's `build_seq` must be a
**monotone** counter, not merely unique — a non-monotone allocation pulls `min_active` back below a finished
build's sequence and re-protects a condemned blob (a leak).

**The turn.** The blob-guard mechanism was replaced wholesale by the precommit-first reachability of §6. But
one piece of the discarded design was *kept*: the monotone-`build_seq` watermark-floor lemma now gates
precommit-ref reclaim liveness (`Gc::prefixEligible` / `BuildPrefix`). This is a rare and deliberate
outcome — a proof outliving the mechanism it was written for. The three B167 models were removed from the
tree in the 2026-07-21 audit, but they are documented because the *shape* of the livelock they caught is
load-bearing knowledge.

---

## 8. Turn 6 — the tree-forest GC → the root-local part-manifest (hot/cold split) {#turn-root-local}

**What we had.** GC discovery ran a single `LIST roots/` that returned ref shards, manifest bodies, and
verbatim files all interleaved. With tens of thousands of part-manifests per namespace, each discovery
round paged the entire backlog — an `O(N²/page)` cost. The tree of trees was a forest of small objects.

**The turn (rev.4 → rev.15 in days).** Collapse the tree forest into a **single root-local full-tree
manifest** per part (`bd14574`, rev.8), and split the pool layout into a **hot** path (`cas/refs/`, the only
thing GC hot-lists) and a **cold** path (`cas/manifests/`, cursor-paged, never hot-listed). Discovery
becomes `GET gc/state + LIST cas/refs/`, cost `O(total ref shards)`. This is modelled by
`CaGcRootLocalPartManifestCore.tla`, the largest model in the corpus (15+ invariants, positive stages up to
~984M states, 28 negative controls). Its counterexamples nailed down rules like "the fence is always
all-shard fresh, never reused" (`sab_lazyfenceunsafe`), "one global coordinator fences every root shard,
not each reducer its own" (`sab_reducerownsfence`), and "the destructive land gate uses the stored token
only if it matches the exact current token" (`sab_staletokenoverdelete`). Two soak-found regressions later
became named gates inside this same model: the `DANGLING-PRECOMMIT` orphan (S30) and the
`PROMOTE-OVER-COMMITTED-LEAK` (a `promote` that overwrote a ref without releasing a pre-existing *different*
committed manifest).

---

## 9. Turn 7 — the namespace registry → shard incarnation (the two-coordinate proof, D1) {#turn-registry}

**What we had.** GC discovered namespaces from a persistent, append-only `gc/registry`, and each round
CAS-bumped a fence into the Cartesian product `registry × root_shards`, minting fence-only manifests even
for absent shards.

**What broke.** Three confirmed problems: `dropNamespace` never deregistered, so the registry grew
monotonically with every table ever created; the fence cost `O(namespaces-ever-created × root_shards)`, not
live namespaces; and empty shard objects were never reclaimed. The interesting part is *why the obvious
fixes were all rejected*, which the design spec enumerates:

- **Writer deregisters at `dropNamespace`** — unsafe. The removal events (the `-1` in-degree edges) are the
  *only* carrier of blob-reachability updates. If the namespace vanishes from discovery before GC folds the
  window past the fence, the `-1`s are lost → a permanent blob leak.
- **GC hard-deregisters when "empty + settled"** — an empty-but-live table (created, no inserts yet) is
  indistinguishable from a dropped one by emptiness alone; this would deregister a live table between
  `CREATE` and its first `INSERT`.
- **Delete-and-recreate the shard object at its stable path** — an ABA hazard: a recreated shard resets
  `shard_version` to 0, and an old sealed fold cursor silently skips the new incarnation's events → lost
  edges → dangle or leak.

**The turn.** `CaGcShardIncarnationCore.tla` answered a design question that had been *deferred because the
alternatives looked superficially viable*: are two orthogonal coordinates necessary, or does one suffice?
The model (724,944 states in the methodology cross-reference) proved that **both** a durable, monotone,
never-reused per-`(ns, shard)` `incarnation` tag **and** the pool-global GC `round` are required — three
negative controls each break the invariant they target (`sab_newbornnofloor` shows the round self-floor is
irreducible; `sab_pathkeyedcursor` shows the incarnation is irreducible against ABA;
`sab_deletebeforefold` shows fold-before-reclaim ordering). Neither coordinate alone works. With the theorem
proven, the registry was deleted entirely (`gc/registry`, `RootsRegistry`, `ensureRegistered`); discovery
migrated to `LIST(cas/refs/)`; `dropNamespace` now appends a tombstone journal event that GC reclaims via
exact-token delete.

---

## 10. Turn 8 — the fence+recheck round → the one-pass ack-floor round (2026-07-02) {#turn-ack-floor}

**What we had.** The GC round was `discover → fold → retire → fence → recheck → exact-token delete → trim`.
The **fence** CAS-wrote a `fence_round` into every root shard; the **recheck** re-folded the fenced window
per shard.

**What broke.** Both the fence and the recheck were `O(universe)` GET + CAS-PUT *every round* — about 2.4
million requests at 100k tables × 8 root shards — and the recheck's per-candidate `inDegreeInGeneration`
re-reads were the quadratic hot spot that started the whole investigation. This is a pure lesson-(b)/(c)
cost problem: the protocol scaled with the size of the pool, not with the churn.

**The turn.** The **ack-floor round** (`d3973af`, `774404e`) replaced fence + recheck with a causal **ack
floor**: a graduation is gated by `condemn_round < min_ack` over live heartbeats, and the entire round
becomes a single **three-cursor streaming merge** (prior snapshot + deltas + prior retired run) that
verifies old candidates, graduates the safe ones, and condemns the new ones in one fold, ending in a single
`gc/state` CAS. There is no fence phase, no recheck phase, and no crash-resume step. Cost dropped from ~2.4M
requests per round to roughly 2–3k. `CaGcAckFloorCore.tla` and `CaGcAckFloorZombie.tla` gate it; the latter
proved that two-phase graduation (`delete_pending`) is load-bearing under two fully-interleaved leaders
(`sab_eagerdelete` dangles without it), and surfaced the ordering rule that the ack floor must be latched
*no later than the fold cut*. A clamp-suppression extension (2026-07-03) reproduces a real night incident —
"31 dangling blobs" — as `sab_clampnosuppress`: a fold may hold back one landed ref, but it must *declare*
it, because a silent hold-back is "the lethal lying-fold counterexample."

The fence-era negative controls from the previous model were *kept as historical evidence* that fencing
only changed shards, or reusing a stale fence position, was unsound *within that mechanism* — which is
exactly why the mechanism, not a patch of it, had to be replaced.

---

## 11. Turn 9 — the concurrent-leader leak → attempt-scoped generations {#turn-attempt-scoped}

**What we had.** GC round artifacts (`retired`, `outcomes`) were keyed by `(round, fence_seq)`. An initial
proposal attempt-scoped only the write-once `gc/gen/<gen>/…` artifacts and left `retired`/`outcomes` under
the old keys, dismissing stale debris as "bounded sweep, minor."

**What broke.** The soak's S33 scenario (concurrent GC leaders) exposed that `retired` is a
**writer-facing publish-gate input** — writers LIST `gcRetiredPrefix()` on the commit path. A stale retired
set written by a *deposed* leader under its own `(round, fence_seq)` survives that LIST and **influences
live writers**, directly violating the invariant *"no unadopted artifact may ever influence a decision."*
There was also no existing sweep for such stale artifacts.

**The turn.** Every decision-bearing round artifact — `retired` and `outcomes` included — moved under
`gc/gen/<g>/attempt/<a>/…`, where the attempt is the leader's lease sequence, and only the *adopted* attempt
is reader-visible (`INV_ONLY_ADOPTED_VIEWABLE`). This also folded cleanup into the `gc/gen/<g>/` retention
prune — no separate sweep. S33 turned from an expected-FAIL scenario card into a passing regression guard.

---

## 12. Turn 10 — mutable root-shards → per-table snapshot+log (a whole optimization family evaporates) {#turn-snapshot-log}

This is the largest single simplification in the branch, and the clearest example of an optimization stack
built to prop up a design that should have been replaced instead.

**What we had.** Refs (the mutable `name → manifest` bindings) were sharded across a fixed `root_shards`
fan-out per namespace, each shard a **mutable object** rewritten in full (live-refs plus journal tail) on
every mutation, routed by `PoolMeta.root_shards` + `Store::shardOf`. To survive the cost of that mutable
object, an entire family of optimizations accreted on top: per-namespace adaptive `root_shards`, adaptive
hash-prefix shard **splits**, and a shard-mutation **flat-combining queue** for group-commit.

**What broke.** The soak measured the mutable-shard model directly: **637k `casPut` attempts for 380k landed
mutations — 40% conflicts, 92% under storms**, each conflict re-reading ~280 KB, from up to 156 concurrent
mutating threads contending over 64 shard keys with no intra-server serialization. Every optimization in the
family existed only to tame that contention; none addressed its cause, which was that the ref binding was a
mutable object rewritten proportionally to its journal tail — lesson (b) again.

**The turn.** Refs are persisted as an **immutable snapshot plus an append-only log** of ref transactions
with strictly increasing ids, mutated through a `CasSingleWriterSlot` per-table append lane that makes
intra-server conflicts *structurally impossible*. `PoolMeta.root_shards`, `Store::shardOf`, and the entire
optimization family became moot and were deleted; the `RootShardManifest` object format was removed. The
`CaRef*` model family (five models: core snapshot+log, delta intake, fold-clamp recovery, stale-leader
namespace cleanup, writer cleanup) proves the new protocol. Notably the "cursor-inside-the-snapshot"
principle from the B140 merge model (`CaB140DangleMerge`) carried forward here: the sealed snapshot carries
its own fold cursor, and the log may be trimmed only up to that cursor. An intermediate version had *GC* own
the base snapshot; within a day that flipped to **writer-owned** (rev.5), because the writer is the one
party that can seal the base atomically with its own writes.

---

## 13. Turn 11 — writer↔GC simplification (edge-before-observe) and freshness-meta v3 {#turn-edge-before-observe}

**What we had.** At promote, a writer re-HEAD'd *all* of a build's blob dependencies (`revalidateDeps`), and
consulted a whole-prefix retire-view LIST plus a "refresh your view to ≥ `fence_round` before publishing"
rule.

**The turn (2026-07-09/10).** Two changes, each proven by its own model.

**EDGE-BEFORE-OBSERVE** (`CaEdgeBeforeObserve.tla`): with the write order "precommit (closure durable) →
adopt/observe → promote," the precommit closure is durable *before* `putBlob` ever observes it, so
promote-time revalidation of *tokened* leaves is provably redundant and was removed. Four negative controls
keep the load-bearing parts honest, including `sab_late_edge` (the pre-fix order, where adoption precedes the
durable closure).

**Freshness-meta v3** replaced the writer-side retire-view LIST with a per-hash `.meta` **point-read**: a
writer reads each blob's advisory `MetaState{Clean, Condemned}` and copies-forward-if-condemned inline,
instead of LISTing a whole prefix. Crucially, the meta is **advisory, not the linearization point** — reads
of the blob never consult the meta, GC deletes the *body* first, and an absent meta reads identically to
`Clean`. This distinction is why an earlier model, `CaMetaDescriptor.tla`, was *deleted* rather than kept:
it proved a design where the meta *was* the linearizer (meta present ⇒ body present), which the shipped code
deliberately does not implement — keeping the model would have been "false comfort."

Two variants of the freshness mechanism were proposed here and **rejected**, both because they re-created the
generation-in-key dead end of §3:

- `CaMetaDescriptorRaw.tla` (raw immutable bodies with the meta as a three-state linearizer): its
  `SabResurrectFromTombstone` counterexample re-enabled an un-tombstone race → data loss. Raw immutable
  bodies cannot let a resurrect displace the body, which forces a writer↔GC liveness coupling.
- `CaMetaIncarnationKey.tla` ("Option B": per-incarnation body keys `blobs/xx/<hash>.<incarnation>`): its
  `SabResurrectReuseIncarnation` counterexample reintroduced the shared-key race. This *is* the
  generation-in-key design already rejected as EBR — a second, independent rediscovery that the idea is a
  dead end because it forces `404 → LIST` and leaks the incarnation into the (otherwise pure-content)
  manifest.

The winning v3 keeps the in-body `incarnation_tag` plus exact-token body delete.

---

## 14. Turn 12 — three bugs the models couldn't have caught alone {#turn-three-oracles}

Three fixes from this period are worth calling out because each was found by a *different* oracle, and
together they justify why the design does not lean on TLA+ alone.

**The resurrect-reupload orphan (soak, S30).** A condemned token is replaced by a resurrect re-upload (a
new token at the same content-hash key); the old token's exact-token delete finds the newer token and skips;
and the fold — being touch-gated and hash-keyed in the *shipped* code — never re-condemns the replaced
token, which stutters "present" forever. The canonical `CaIncarnationCore` model *could not* reproduce this,
because its `GRetire` condemns by `(hash, current-token)` and is un-touch-gated, so it always re-condemns —
the *correct* algorithm was already in the model, and the shipped `closeBlob` had diverged from it. The
deeper lesson, quoted in the model index: the canonical model "abstracts that away (idealized
always-eventually-condemn GC) — which is the DEEPER reason it misses this class." A focused, code-faithful
model (`CaGcResurrectReuploadOrphan.tla`, now removed) reproduced it in 194 states; the fix made `closeBlob`
match the model's already-proven `GRetire`.

**The in-degree underflow (model-faithfulness gap).** `CaGcIndegRefoldCore.tla` isolated a hazard the large
model *structurally could not express*: the abstract model recomputes in-degree from a folded edge *set*
(idempotent — re-folding is a no-op), while the C++ accumulated in-degree as a *non-idempotent integer delta
stream*, which could reach `-1` (→ `CORRUPTED_DATA`) when a completion-seal cursor persisted at the wrong
position. The fix adopted the idempotent two-cursor presence-set merge, making the underflow structurally
impossible, and the model was removed because it now describes a design the code abandoned.

**The condemn-marker swallow (external code review).** Neither soak nor model found this one: the GC
swallowed failures of the asynchronous condemn-marker write while committing the retired entry anyway. Since
the per-hash marker is the writer's adopt gate, a lost marker lets a writer adopt the very token a later
graduation deletes → a dangling manifest. The fix (`CaGcCondemnMarkerGate.tla` gates it) requires confirmed
durable Condemned evidence before graduation to `delete_pending`, otherwise carrying the entry to the next
round, fail-safe.

---

## 15. Turn 13 — three cursors → two, and the ref-lease boundary {#turn-cursors-and-lease}

**retired-in-snapshot (2026-07-11).** The ack-floor round still carried a *separate* durable retired list
with its own per-gc-shard object family and its own merge cursor — a third cursor. `CaRetiredInRun.tla`
proved the retired list could ride the source-edge run itself as condemned-sentinel rows, with a per-shard
`condemned_summary` in the fold seal read at zero extra I/O. The round collapsed to two cursors; the
`RetiredSet` object family, its keys, and its format id were deleted. A companion witness model
(`CaRetiredInRunFoldAbortWitness.tla`) proved the freshness meta must be **add-only** — the weaker fix of
clearing the marker after the winning CAS is *still* unsafe — and this witness later refuted an attempted
revival of marker-clearing.

**The grace window → rev.6 lease exclusivity.** A cross-epoch hazard remained: a *fenced predecessor's*
in-flight late PUT can materialize below a successor's snapshot coverage (a missed `-1`/`+1`, a data-loss
class). The interim design tolerated this with a materialization **grace window**
(`snapshot_min_log_age_ms`) plus a per-entry `RefTableState` publish-path replay — but a timing window only
*documents* the hazard, it does not close it. The **rev.6 lease-boundary exclusivity** design solves
exclusivity *once*, at the mount-lease handover: an unclean handover waits, a clean release takes a fast
path, and the successor commits an eager recovery-snapshot **seal** before it is allowed to write ("mount
writable only after it commits"). The grace window, the per-entry replay, and the `CasRefLatePredecessorObserved`
diagnostic counter all go away.

Two clock/reclaim corrections landed alongside, both from the mount model `CaCasMountCore.tla`:

- The write-fence deadline moved from `steady_clock` to `CLOCK_BOOTTIME`, because `steady_clock` does not
  advance across a VM suspend — a 51-second pause in the ack-floor soak had fenced a mount that then did not
  see its own fence expired.
- Expired-mount reclaim became **observation-based**: the reclaimer must observe a stable holder token over
  a full `TTL + Drift` window on its *own* monotonic clock and install the successor's own body, never
  trusting the foreign body's wall-clock timestamp. `sab_wallclockreclaim` is the permanent negative control
  for that trust. The old per-write epoch/body-read supersession guard was correctly removed in favour of a
  pure-local in-memory check (`mayMutate = !lost ∧ now < deadline`) — no per-write S3 read.

---

## 16. Turn 14 — the acked-then-lost data loss, and the transaction collapse {#turn-dataloss-txn}

**The single most consequential correctness event of the branch (2026-07-17).** An RCA traced an
**acked-then-lost INSERT** — a client received an `INSERT` acknowledgement, but the part was later lost.
The origin was a task from early in the branch (`39cf327`, "Task 1.1") whose safety guard (B151) had since
been removed. The fix (`11077ee8`, "part durability before Keeper commit") makes the disk transaction close
in `renameParts`, ensuring the part is durable *before* the Keeper commit that acknowledges it. This fix is
generic — it is flagged as an upstream candidate, not CA-specific.

**TXN-ONE-PIPELINE (2026-07-15).** The `DiskObjectStorageTransaction` had grown *two* dispatch pipelines —
eager (CA staging) versus deferred-to-commit (durable ops) — whose ordering inversion had caused the
`01603` column-TTL abort, and every per-method `isContentAddressed()` branch "was added through a bug." The
redesign introduced an explicit two-phase `IDiskTransaction::precommit` contract: CA precommit *is* the
entire publish (manifest → precommit-add → upload → promote), CA commit is durable-intent materialization
only, and a single `dispatch` funnel replaces the CA subclass. `moveDirectory` stopped publishing, and
B151's rename-window publication machinery was deleted.

---

## 17. Turn 15 — maturation: formats, hashes, parts, staging, movement, backups {#turn-maturation}

With the safety core settled, the last weeks broadened the feature. These are less "wrong turns corrected"
and more "capabilities added on a now-stable base," but several carried their own rejected alternatives:

- **Codecs v3 — everything-text (2026-07-15).** The two-encoding freeze of §5 matured into a scheme where
  every file, including text formats, flows through the content-addressed codecs.
- **Pluggable blob hash (2026-07-11).** The blob hash became pluggable (`cityHash128` / `xxh3-128` /
  `sha256`) with mixed-algo pools, behind a `BlobDigest` migration.
- **All-tree part files (2026-07-15).** The mutable-file set went to ∅ — every part file, including those
  previously treated as mutable, is now content-addressed, with committed-part repoint and an MVCC
  atomic-write short-circuit.
- **S3-native staging (2026-07-11), opt-in.** A write-once, conditional, server-side-copy staging path — but
  kept strictly opt-in; the default remains a local scratch path, because staging directly on S3 changes the
  failure surface.
- **Part-folder cache, `MOVE PART/PARTITION` to a CA disk, and the backup model.** The backups design chose a
  four-verb snapshot / mirror / fetch / restore model and explicitly **rejected a reference-only `FREEZE`**,
  because `shadow/`'s contract is filesystem-readable hardlinks consumed by external backup pipelines — a
  reference-only freeze would silently break every such pipeline. Similarly, adding a CA-specific
  `manifest_hash` field to the Keeper `/parts` znode was **rejected outright** as upstream coupling (lesson
  (e)); the manifest id already travels in-band via the fetch-by-relink handshake.

The branch also hardened its own record: a 14-perspective umbrella review against the merge-base
(2026-07-20), a `RefTableState`-as-closed-class refactor that makes its invariants hold by construction, and
a sweep that removed the now-obsolete TLA+ models (the EBR-era `CaGcCore`, the watermark models, the rejected
meta-descriptor variants) — each removal justified by the "false comfort" principle, not housekeeping. The
frontier at that point (2026-07-22) was a CAS parallel write-path spec (`8c64d9e`) — which is where the
next act begins.

---

## 18. Turn 16 — the write path gets measured, and GC's minutes turn out to be round trips (2026-07-23 → 25) {#turn-writepath-gc-perf}

Two measurement campaigns bridged the maturation phase and the crisis that followed, and both replaced a
folk explanation with an arithmetic one.

**The wide INSERT was not "slow", it was single-threaded.** The write-path baseline
(`2026-07-23-cas-wide-insert-baseline.md`) measured CA-on-S3 at 3.0× a plain-S3 insert, dominated by one
thread doing serial blob uploads. Stage 1 (`CasBlobUploadPool` + `fanOutBlobUploads`, landed 2026-07-24)
fanned out a part's blob PUTs: wall 58.41 s → 30.26 s, CA-vs-plain 3.0× → 1.59×, and the single-threaded
signature vanished (top-thread share 72.3% → 14.5%). Stage 2 (concurrent `commitPart`) was researched and
**postponed by user decision** — the residual was now small enough not to justify its concurrency risk.

**GC's minutes-long rounds had no compute in them at all.** The round-duration study (`86597e6ceff`)
divided wall time by request count and got 0.465–0.535 ms per *total* S3 request — pure serial round-trip
latency, no CPU or lock term. A 30-minute round was simply 3.42M serial round trips; 39.6% of them were
redundant manifest re-fetches, and 44% were the `HEAD`-before-`GET` pairs protected by the standing
protocol veto (recorded as a cost, not re-litigated). One suspicion — the fold seal being re-read hot —
was **refuted by its own counter** (2 reads/round), a small instance of the "measure before blaming"
discipline.

**The full-scale campaign's postscript found a real retention defect.** The S42 full-scale soak had
killed its host environmentally (RSS growing at 21 GB/min), but the machinery verdict was HELD: 2184
injected faults, zero `CasRefApplyPoisoned`, 11,960 acked blocks intact — recorded as an INCONCLUSIVE
*certification*, deliberately not rounded up to a pass. The quiet-host reproduction that followed then
root-caused something real: the orphan-manifest sweep deleted manifests whose `+1` edges had already been
folded, because its premise — "unowned ⇒ no folded edges" — is **false** once a precommit's edges fold.
The manifest body vanished while its folded edges stayed forever. The fix became the §6-shaped sweep
premise that later shipped in Stage A (§21).

**And the hammer proved the wrong thing.** Three runs and ~19M listed keys produced **zero** LIST holes.
The conclusion drawn was not "LIST is fine" but "this instrument cannot decide the question" — the
replacement was probe A: stop trying to *reproduce* the anomaly and instead **record it at firing time**,
`HEAD`-ing the suspect key at the exact moment enumeration and arithmetic disagree. That redesign is what
made the next section possible.

---

## 19. Turn 17 — the LIST that lied (2026-07-26) {#turn-list-lied}

**The event.** Four minutes into an ordinary soak — after the 19M-key hammer had found nothing — probe A
fired (`4bf51596402`, "CAUGHT LIVE"): an enumeration of a ref-log stream returned `seq 0x1430e` while
omitting `0x1430c` and `0x1430d`. The `HEAD` taken at the moment of disagreement showed both omitted
objects `present`. `system.blob_storage_log` then proved the timeline (`38c2aec25dd`, "PROVEN BY
MEASUREMENT"): the three uploads completed 2.2 ms apart in strict id order at 16:47:19; the anomaly fired
at 16:47:38 — the omitted objects had been **durable for 19 seconds** while a key written *after* them was
returned. Across 65,263 ref-log uploads in that namespace, zero were out of order; all four alternative
explanations (concurrent delete, stale-epoch minting, out-of-order appends, a late `Unresolved` PUT) were
excluded on evidence at three source levels.

**The classification that keeps being re-argued (settled; see `2026-08-03-list-trust-verdict.md`).** The
tail was CORRECT — the lie was omission of *predecessors below the returned frontier*. That exact shape
is **legal on a contract-compliant S3** for writes concurrent with a paginated enumeration (the
read-after-write guarantee covers lists *started after* a PUT completes; there is no mid-walk snapshot
contract) — though the observed 19-second instance violated even that contract, making it a RustFS
defect. Nothing was proven against AWS. Meanwhile the trusted verbs stayed honest: exact-key `GET`/`HEAD`
told the truth *while* LIST lied, and a conditional write cannot lie without destroying the store's own
data.

**Why it was a blocker and not a bug.** GC folded what a listing returned and sealed a cursor above what
it observed. Sealing is permanent — a record below the cursor is never re-read — so a hidden `-1` was an
eternal retention leak and a hidden `+1` let GC delete a blob a committed manifest still referenced:
loss of acked data. The fail-closed path *worked* (probe A aborted the fold; a later complete round
reclaimed the keys), and a previously mysterious "56 unmatched `-1` deltas" leak was reclassified as a
*consequence* of the same hole, explicitly not to be patched in the reducer. The root cause was named
structurally, and the naming did the design work: **absence is undecidable in a sparse id space.** The
pool-wide `next_ref_sequence` made per-namespace gaps normal, so no hole could ever prove itself
innocent. That sentence, not the incident, is what the redesign answered.

---

## 20. Turn 18 — the certificate stack dies at v5: contiguous streams, the in-band seal, `_ckpt`, the catalog (2026-07-27 → 28) {#turn-contiguous-chain}

**What we tried first.** The initial spec (`4f687576d99`, "ref-log prev-chain + complete-cut fold")
answered the blocker the additive way: keep sparse ids, add certificates — prev-links in each record,
seal intervals, a `NeverBorn` state, a seal-pointer authority, recovery generations, an `R*` sweep bound,
tombstones, sticky floors, a quarantine. Four review rounds grew the stack; two user decisions inside
that arc (the `R*` owner-grant bound; "no authority object") pruned branches but not the shape.

**The kill.** At v5 (`a3a62d2ebe5`) the user rejected the accretion outright, and the entire certificate
stack was deleted in favour of changing the *invariants* — the same "re-derive the invariant, don't
patch" reflex that had killed EBR patches (§3) and the fence patches (§10):

- **INV-1, contiguous ids.** The pool-wide allocator is deleted; the next id is *derived* from the
  table's own `greatest_applied`, so durable ids are dense `1..T` within `(namespace, epoch)`. A missing
  key becomes computable by name — the "absence undecidable" root cause dissolves rather than being
  certified around. Density is only safe with the **every-attempt slot-reuse rule**: an id is freed only
  if nothing was sent or every sent attempt has its own conclusive rejection; a lane that cannot prove
  that stays wedged (acceptable — nothing was acked).
- **INV-2, the in-band epoch seal.** The seal takes the *exact log key* a dying predecessor's PUT would
  take, so the store's conditional create IS the fence — no timing argument, no grace interval. This
  killed the rev.4 "Late Predecessor PUT" ghost by construction.
- **INV-4, `_ckpt`.** A per-life O(1) token-CAS head object carrying `committed_through` — the exact
  acked frontier — plus the checkpoint-snapshot pointer and the seal chain. The append order becomes log
  PUT → `_ckpt` CAS → ack, at a deliberate, honestly-priced cost (~+2 serial S3 round trips per committed
  chunk): the original "zero added append requests" target was tried and **rejected by the recovery
  audit** — without one durable committed-through fact, an incomplete LIST cannot tell recovery where
  acknowledged history ends, and serial `GET N+1` has neither a bounded stop nor acceptable cost.
- **INV-3, the catalog.** A token-CAS namespace universe, read as a *cut*. The stated reason is careful:
  not "LIST is unreliable" but that a GET returns one object version — every member as of one instant —
  while a paginated walk stitches pages and may omit a member that exists throughout. Staleness after a
  cut is closed downstream by protocol; a namespace the universe never named is unrecoverable by anything
  downstream. **Incarnations** (opaque 128-bit life ids in every key) then make a removed life's debris
  structurally inert, so removal needs no physical-emptiness proof at all.

**The convergence.** Nine adversarial review rounds plus a blinded consult; v9 CONVERGED at
APPROVE-WITH-FIXES. The §10 disposition table preserves the graveyard: serial `GET N+1` (no bounded
stop), recovery-from-complete-LIST (completeness is the premise under test), `seq_floor` (floors for
dead names never retire), *delete the incarnation and ban name reuse forever* (proposed and withdrawn —
the same unbounded-memory shape one level up), a full head-CAS commit chain (a second authority for a
fact `_ckpt` already carries). One more thing the phase-0 TLA+ gate contributed before implementation: a
**third temporal-lemma arm** nobody had designed (`_sab_deleteignoresindeg`, RED — a `+1` landing after
the probe but before condemnation), whose closure — the delete-site in-degree re-read — was thereby
promoted from optimization to **normative**. The TLA phase closed 93/93.

---

## 21. Turn 19 — Stage A: the allocator that stopped existing (2026-07-28 → 29) {#turn-stage-a}

Stage A implemented the new invariants at the *old* key shape, with destruction suppressed
(`UniversePolicy::kDefault = StageA_Suppressed` — "a parameter with a default, not an override; no
config, no environment variable"). The honest reason: blob in-degree is pool-wide, so until the universe
is authoritative (Stage B), a hidden acked `+1` in an invisible namespace can coexist with a visible
`-1`, and no per-namespace machinery closes that. Folds, seals, cursors, holds and recovery all ran for
real; every delete family was inert. The implementation's own misadventures:

- **There is no allocator to get wrong.** The id is a pure function of the state it applies to, and the
  density rule is enforced on the *read* side (`applyTxnInPlace`) — a hole cannot become durable even if
  a future writer path forgets the rule, because every apply (writer, recovery replay, GC fold) runs it.
- **The sentinel seal fenced nothing.** The prior epoch-closure marker lived at a synthetic id
  `{E-1, UINT64_MAX}` — it "occupied no log key, so it fenced nothing." The real seal takes the ghost's
  exact key via the new `slotOccupy` primitive (`Created | Occupied | Unresolved`), and wedge resolution
  moved from a bare `GET` (where absent-is-not-a-rejection wedged lanes forever) to one bounded
  conditional create per flush under an **admission fence** — the generation captured at admission, never
  the current one, is what makes retries safe.
- **The arithmetic fold promptly diverged.** Folding "to the end of the stream" made round time
  `backlog / (walker_rate − writer_rate)`: zero completed rounds in 42 minutes on a hot pool. The fix —
  the frozen round-start tail — bounds *folding*, not *reading*: the walk still GETs `cursor+1` (that
  read IS the frontier proof), but folds nothing above the tail it froze at round start. Re-validated at
  64 bounded rounds.
- **A hold could erase itself.** The durable-hold work went through four fix rounds; the memorable
  defects were `offending_position {0,0}` ("not a degenerate hold — a hold that ERASES ITSELF") and a
  blast-radius bug where one undecodable 4-KiB `_ckpt` failed the *entire* round closed, every round,
  until `d4ddc736949` scoped it. A hold clears **only** by folding through its position — never by
  re-observing absence, that being exactly what a lying store produces.
- **REBUILD's condemn pass was the liar's accomplice.** Both legs of the `LIST(blobs/)` condemn
  traversal were listing-driven, so a store hiding a live owner made REBUILD condemn the very blob that
  owner pins (r5-finding-4). Deleted, not fixed. The fsck rework also surfaced a **budget inversion** —
  the subprocess deadline (180 s) was shorter than the scan deadline (600 s), so `--partial` mode was
  unreachable, and would have reported a fabricated `dangling=0` if reached.
- **The revert proof.** Nine LIST-trusting sites were converted; a harness backend
  (`setListOmissions`) states an omission as a set — enumeration hides exactly those keys, every other
  verb serves them honestly. Re-introducing a listing-driven step at the three arithmetic sites turned
  6 of 8 end-to-end tests red: the defenses are load-bearing, not decorative.

Stage A closed 2026-07-29, verdict `STAGE A: PASS` (`2026-07-28-stage-a-RESULTS.md`), with its residuals
named rather than absorbed — chiefly that fsck's reachability pass still trusted the recovery hint, a
listed precondition of ever flipping the suppression constant.

---

## 22. Turn 20 — Stage B: single authority, and the gates that had never run (2026-07-30 → 08-03, in progress) {#turn-stage-b}

Stage B's one idea is **single-authority consolidation**: everywhere CAS metadata had two ways to answer
a question — a durable token-CAS object and an enumeration — the enumeration is deleted or demoted to a
hint. The plan itself had a governance turn: the original Stage B plan was superseded mid-flight by a
midpoint audit plus a remaining-work plan published atomically (`f8df7d9a5e8`), so that two execution
authorities never coexist at a commit boundary.

- **Identity became a type, not a convention.** `NamespaceLifeId` (namespace + incarnation) has no
  default constructor and no conversion from a bare name; the sole factory takes both fields from one
  catalog entry. Losing the incarnation is now a compile error instead of a runtime aliasing bug. The
  last Stage-A bridge (`stageATransition`, a hash-derived fixture id) was retired with a ~440-site test
  migration through one named fixture seam. A real defect fell out en route: a read or unlink of a
  never-created table went through the *minting* resolver and grew the capacity-gated catalog for a
  namespace nobody created — pinned by a regression test on the **catalog**, not the file outcome,
  because the file outcome was "absent" both before and after, which is exactly why nothing had caught it.
- **Recovery stopped reading listings — by deletion, not by policy.** `recoverRefTable` was deleted
  outright (`357cf7b963f`), with deliberately no compatibility overload, replaced by
  recovery-from-authority: catalog cut → exact `_ckpt` → validated snapshot triple → finite arithmetic
  replay ending exactly at `committed_through`. The intermediate design had kept the listing as a hint
  and was deleted too, for three reasons worth remembering: a listed snapshot can be **well-formed and
  still uncommitted** (published before `_ckpt` adopted it — no amount of reading it harder validates it
  into safety); a short LIST omitting the *tail* contradicts nothing (replay ends early and fsck reports
  clean over a ref set missing an acked publish); and a LIST offers no stopping condition at all.
  `committed_through` was widened to a format-generation bump (8→9, recreate-only) to carry the exact
  frontier. The fsck LIST-derived snapshot oracle went the same way after a RED test showed its
  hard-finding verdict *changing across listing shapes of the same store* — an oracle that answers
  differently depending on how the store lies is not an oracle.
- **GC's registered defects (R2/R3) were ownership bugs, not fold bugs.** R2: `PartWriteTxn`'s
  destructor unconditionally retired its build sequence, advancing the GC floor while the precommit
  grant's outcome was still *uncertain* — an uncertain owner grant looked dead. The fix is a duty queue
  with the failure direction inverted: settle → retire → pop, any throw retains the duty, and a full
  queue pins the floor (an advanced floor is an irreversible authorization; a pinned one is latency).
  R3: the manifest sweep's delete-and-never-retire (the S42 defect of §18) became a **neutral
  nomination** riding a separate third cursor in the fold merge — deliberately not a `BlobDelta`, which
  would have contaminated both in-degree probes.
- **The gates themselves turned out to be under-tested — for the third time.** The `Cas*:CA*` gtest
  prefix filter had silently omitted suites twice before; both "fixes" had re-added names to the list.
  The third fix changed kind: a generated, cross-checked suite list (source `TEST(...)` grep ×
  `--gtest_list_tests`, `TEST_P` instantiation resolution, unclaimed suite = hard failure), on the
  principle that *a silently-omitting list is indistinguishable from a covering one, so the fix is the
  cross-check, not the list*. It paid immediately: it surfaced a `TEST_P` suite that had **never once
  run**, whose fix let the ASan lane reach a real 3-site heap-use-after-free (a pointer into a destroyed
  catalog-read temporary) — and the first ASan gate then turned out to reuse the *release* suite list,
  the same bug class again, same day. The TLA lane had its own fail-open: the temporal-smoke gate
  grepped for TLC's plural refusal message while the pinned jar emits the singular — a checker failing
  `<> TRUE` would have blessed every downstream temporal verdict.
- **A brief that would have pinned the wrong thing.** Task 2 asked for a test that a "poisoned" lane
  issues zero writes; there is no `Poisoned` state (it is `NeedsRecovery`), and recovery's catch-up
  `_ckpt` CAS is the *remedy* — pinning "zero writes" would have pressured a later change into removing
  it. The test was re-aimed at ordering, not a global zero: reviews here repeatedly moved tests from
  "assert the incidental number" to "assert the invariant."

Alongside: the mount-fence observability spec went to rev.2 after an MSan log **refuted its own
premise** (the real failure was Mode 2 — a hung renewal, a silently expired fence, and a silent re-arm;
no throw, no log, on any transition) — specced, not yet implemented; CI got its 6-hour-timeout sharding
(a TSan lane had finished 10,990/10,990 tests and was killed 16 seconds before it could say so) and a
RustFS fd-limit RCA; and GC observability gained per-phase timing (which immediately found the fold seal
being read five times per round where the design said two), an end to "round 0" logs on deferring rounds
(read for a week as "GC is dead"), and the `gc_anomaly` row that `HEAD`s the hole key *while the
disagreement exists* — because present and absent are opposite defects, indistinguishable after the fact.

**Status as of 2026-08-03** (transient, will age): T0/T1/T2/T4/T6a/T7 complete, both gate lanes green;
T3 and T5 not started; T6 — the `UniversePolicy` flip that re-enables destruction, the point of the
stage — blocked on them. The write-path cost of `_ckpt` and the per-namespace frontier probes both have
recorded optimization plans (`BACKLOG.md {#ckpt-read-policy}`, `{#gc-frontier-one-list}`), each shaped as
a pluggable policy with the conservative variant as default.

---

## 23. Cross-cutting patterns in the trial-and-error {#patterns}

Stepping back from the individual turns, seven patterns describe *how* the design search actually behaved:

1. **The generation-in-the-key idea was proposed and killed three times.** First as EBR (§3), then
   rediscovered as the `CaMetaDescriptorRaw` tombstone scheme, then again as `CaMetaIncarnationKey`
   "Option B" (§13). Each time it looked reasonable; each time the counterexample was the same class —
   `404 → LIST` degraded reads, incarnation leaking into the pure-content manifest, and a writer↔GC liveness
   coupling. The in-body incarnation tag plus backend-token delete is the design *because* it removes the
   generation from every key.

2. **"Both halves necessary, jointly sufficient" is the recurring shape of the fix proofs.** Build-root ∧
   fail-closed commit (§6); trim-gate ∧ cursor-in-snapshot (the B140 merge model); incarnation ∧ round
   self-floor (§9). In each case the pairwise matrix showed the obvious *single* fix still fails, and the
   counterexample for the one-coordinate variant is what forced the two-part design. Designs that "looked
   superficially viable" were the ones most worth model-checking.

3. **Superseded mechanisms were deleted, but their proofs and negative controls were kept as evidence.** The
   all-shard fence (→ ack-floor), the writer-heartbeat ack floor (→ round-only pacing), the blob-watermark
   guard (→ precommit reachability). Each supersession is recorded as "the mechanism, not a patch of it, was
   replaced," with the old `sab_*` controls retained to justify *why* a patch was insufficient.

4. **The model corpus is itself under test — for faithfulness to the code.** Two audits (2026-06-22 and
   2026-07-21/22) re-checked every model against the shipped code, producing a taxonomy
   (CURRENT / MIXED / STALE / HISTORICAL / REMOVED). The 2026-07-22 headline finding was **"no CODE-RISK"**:
   wherever a model and the code diverge, the code upholds the same safety conclusion via an equal-or-stronger
   mechanism. Models were removed when keeping them would assert a guarantee the code no longer makes — false
   comfort is treated as worse than no proof.

5. **Three oracles, three distinct failure classes.** TLA+ found design constraints before code existed
   (fence, recheck, registry, two-coordinate, build-root). The soak found what the idealized models
   abstracted away (B140, the resurrection-cap exhaustion, the resurrect-reupload orphan). External code
   review found what neither did (the condemn-marker swallow). The design's confidence is the *conjunction*
   of all three plus deterministic gtests.

6. **Delay-shaped errors are acceptable; authorization-shaped errors are not.** The asymmetry that decided
   LIST's fate (§19–20) is general: a stale-but-honest observation is a true cut from the past, and every
   error it induces falls toward retention/latency, closed forward by ordering, cross-round delay, and a
   final re-check at the destructive site. A fabricated cut (omission under a permanent seal) corresponds
   to no point in time and *authorizes* destruction. The design rule that fell out: any input feeding an
   irreversible action must be structurally unable to authorize it wrongly — it may only delay it. LIST
   survives in exactly the roles that satisfy this (work bound, witness set, genesis hint); the trusted
   verbs (exact-key reads, conditional writes) carry everything else.

7. **Instruments are under test too, and negative instruments prove nothing.** The 19M-key hammer found
   zero LIST holes; one `HEAD` taken at the moment of disagreement settled the question four minutes into
   an ordinary soak (§19) — detectors must *record at firing time*, not hope to reproduce. The same season
   produced the third recurrence of the silently-omitting gtest suite list (fixed by cross-check, not by
   re-adding names), a temporal-smoke gate that was fail-open on a grep pattern, an fsck oracle whose
   verdict varied with the store's lie, and a `--partial` mode made unreachable by its own budget
   inversion (§21–22). Each is the same lesson: a gate that cannot fail against a broken subject is
   indistinguishable from a passing one, so gates need their own negative controls.

---

## 24. Appendix — the turns at a glance {#appendix}

### 24.1 Model → decision {#appendix-models}

| Model | Decision it forced | Counterexample (the "error") |
|---|---|---|
| (design review, pre-model) | Cut D6 write-ahead intents | `O(files)` persistent Keeper writes per commit; intent keys collide across writers |
| `CaGcCore` (removed) | Abandon EBR / generation-in-key | Stuck writer stalls pool-wide reclaim; `404 → LIST`; Keeper required |
| `CaIncarnationCore` | The safety spine (in-body incarnation, exact-token delete) | 11 sabotages; registry fence must use committed universe (real C++ hole) |
| `CaIncarnationProofCore` (removed) | Inductive proof of the token core | `InflightCurrentUnreferenced` proven irredundant; re-observation required |
| `CaBuildRootPrecommit` | Build-root reachability ∧ fail-closed commit | Reproduces the B140 soak dangle exactly; each half alone still fails |
| `CaB140DangleMerge` | Cursor-inside-the-snapshot; trim only to cursor | Trim-before-durable across a lease handoff loses an edge |
| `CaResurrect*` / `CaBuildWatermark*` (removed) | Precommit reachability replaces blob guards; monotone `build_seq` | Re-upload alone is starvable; non-monotone seq re-protects a condemned blob |
| `CaGcRootLocalPartManifestCore` | Hot/cold split; single global coordinator fence | 28 sabotages; stale-token over-delete; deposed-leader visibility |
| `CaGcShardIncarnationCore` | Delete the registry; incarnation ∧ round (two coordinates) | Each one-coordinate variant produces a counterexample |
| `CaGcAckFloorCore` / `…Zombie` | One-pass ack-floor round; two-phase graduation | Silent fold hold-back = the "31 dangling blobs" incident; `sab_eagerdelete` dangles |
| `CaGcRoundDeferCore` | A deferring round must force-fold before any delete; bounded defer | Graduate-on-stale over-deletes; unbounded defer never folds |
| `CaEdgeBeforeObserve` | Remove promote-time revalidation of tokened leaves | Adoption before durable closure (the pre-fix order) dangles |
| `CaMetaDescriptor*` / `CaMetaIncarnationKey` (removed) | Reject meta-as-linearizer and per-incarnation keys | Un-tombstone race; shared-key race = generation-in-key again |
| `CaGcIndegRefoldCore` (removed) | Idempotent presence-set merge (no integer delta stream) | Non-idempotent integer in-degree reaches `-1` → `CORRUPTED_DATA` |
| `CaGcResurrectReuploadOrphan` (removed) | `closeBlob` must re-condemn the current token | Idealized model abstracted the class away; code had drifted from it |
| `CaGcCondemnMarkerGate` | Graduation needs confirmed durable Condemned evidence | Swallowed async marker write lets a writer adopt a doomed token |
| `CaRetiredInRun` / `…FoldAbortWitness` | Retired list rides the run (3→2 cursors); freshness meta add-only | Marker-clearing after the winning CAS is still unsafe |
| `CaRef*` (five models) | Per-table snapshot+log; writer-owned base; sealed birth coverage | 40–92% CAS conflicts on mutable shards; late-predecessor cross-epoch loss |
| `CaCasMountCore` | Observation-based reclaim; `CLOCK_BOOTTIME` fence; pure-local write guard | Trusting a foreign wall-clock timestamp violates exclusivity |
| `CaRefTableSnapshotLogCore` (v9 rewrite) | Dense ids, `_ckpt` recovery, slot-occupy seal | `_sab_scanistruth` reproduces the real `0x1430c` acked-data loss |
| `CaRefDeltaIntakeCore` (v9 rewrite) | `NoAckedLoss` as the central invariant | `_sab_deleteignoresindeg`: the third temporal arm — delete-site in-degree re-read is normative |
| `CaRefCatalogCore` | Namespace lifecycle under opaque life ids; catalog as the sole universe | No-alias / newborn-safe / bounded-catalog / removal-delete-proved |
| `CaRefNsCleanupStaleLeaderCore` | Janitor exact-token deletes survive rebirth | A stale leader's delete across a same-name rebirth |
| `CaRelinkConfirmCore` | Confirm adds no new dangle path (NOT "a confirmed relink cannot dangle") | `_sab_holeylist` VIOLATES with exactly one incomplete page |

### 24.2 The turning-point commits {#appendix-commits}

| Date | Commit | Turn |
|---|---|---|
| 06-01 | `dda531f` | v3 content-addressed shared-MergeTree design + PoC |
| 06-03 | `129ba00` | CAS coexists with zero-copy (opt-in, not a replacement) |
| 06-10 | `5624e67` | Incarnation-token design supersedes the EBR core |
| 06-11 | `30685b2` | Incarnation model hunt: 782M states + 8.3B visits, 0 violations |
| 06-12 | `215ad90` | Garage rejected (ignores conditional ops); RustFS chosen |
| 06-18 | `b0079ca` | Fail-closed commit; B140 build-root protection |
| 06-24 | `084dc46` | Abandon JSON entirely; two encodings |
| 06-26 | `bd14574` | rev.8 single root-local full-tree manifest (collapse the forest) |
| 07-01 | `5116068` | D1: shard incarnation + registry removal |
| 07-02 | `774404e` | One-pass ack-floor round; fence/recheck/retire/resume removed |
| 07-09 | `12c5fbe` | EDGE-BEFORE-OBSERVE + freshness meta-descriptor |
| 07-10 | `74c5b60` | Fold the retired list into the snapshot run (3→2 cursors) |
| 07-12 | `318291f` | Ref intake on snapshot+log — remove `RootShardManifest` |
| 07-13 | `dcdaf47` | Ref lease-boundary exclusivity rev.6 |
| 07-15 | `994507d` | All-tree part files (mutable set = ∅) |
| 07-15 | `5101a50` | TXN-ONE-PIPELINE (two pipelines → one precommit contract) |
| 07-17 | `11077ee` | Part durability before Keeper commit (acked-then-lost fix) |
| 07-21 | `fb3aca0` | `RefTableState` closed class (invariants by construction) |
| 07-22 | `8c64d9e` | CAS parallel write-path spec |
| 07-24 | — | Write-path stage 1 lands: wide INSERT 3.0× → 1.59×; stage 2 postponed |
| 07-26 | `4bf51596402` | Probe A catches LIST incompleteness LIVE (`0x1430c`/`0x1430d`) |
| 07-27 | `4f687576d99` | First contiguous-chain spec (prev-chain + complete-cut certificates) |
| 07-28 | `a3a62d2ebe5` | v5: the certificate stack is deleted for the invariant changes (INV-1..4) |
| 07-28 | `4d6f720c206` | Spec v9 CONVERGED; TLA phase 93/93; the third temporal arm becomes normative |
| 07-28 | `4d6074c5136` | INV-1 lands: `next_ref_sequence` deleted, ids derived, density enforced on read |
| 07-29 | `d4ddc736949` | `STAGE A: PASS` — arithmetic GC on old keys, destruction suppressed |
| 08-01 | `357cf7b963f` | `recoverRefTable` deleted outright — recovery-from-authority, zero stream LIST |
| 08-02 | `f8df7d9a5e8` | Stage B midpoint audit + remaining-work plan published atomically |
| 08-03 | `4bc136c0b78` | The LIST-trust verdict is written down as settled |

---

## 25. Where this leaves the design {#closing}

The shipped design is small relative to the space it explored. Blobs are content-addressed and their count
is *derived* from a folded edge multiset, never stored. Identity is an in-body incarnation tag plus a
backend token — no generation in any key, no `404 → LIST`. Refs are an immutable snapshot plus a
**contiguous** append-only log under an opaque life id, with a `_ckpt` head object carrying the exact
acked frontier; the universe of namespaces is a token-CAS catalog read as a cut. GC folds by arithmetic
to each life's frontier and may destroy only when the *whole* frontier is proven from one observation —
LIST authorizes nothing anywhere in the system; it can only delay. Keeper is optional; a stuck writer
stalls nothing pool-wide; nothing on any hot path scales with the pool's total size.

As of 2026-08-03 one sentence of that description is still promissory: production destruction remains
suppressed behind the Stage B flip (T6), which waits on the last two remaining tasks — a deliberate
ordering, since the suppression constant is the one thing in the system that must not flip before its
preconditions are proven rather than assumed.

None of those sentences describes a first draft. Each is the residue of a mechanism that was built, modelled,
found wrong by a counterexample or a soak or a review, and replaced — with the failed alternative's proof
kept nearby as the reason not to try it again.
