# CAS writer↔GC protocol simplification: EDGE-BEFORE-OBSERVE + per-hash meta-descriptor

**Date:** 2026-07-09 (combined rewrite; the original Tier-2 list-based text is in git history of this file)
**Branch:** `cas-gc-rebuild`
**Status:** design (user-driven; combined design approved 2026-07-09; Phase A independently consulted —
verdict SOUND-WITH-CHANGES, findings A-G folded in below)
**Supersedes:** backlog PROMOTE-REVALIDATION-MINIMIZATION variants A (HEAD-skip) and B (pinned-round ack);
the original list-based Tier-2 text of this spec; the tokenless copy-forward remains but its data source
changes in Phase B.
**Relation to landed work:** the two condemn-race fixes (promote resurrect-tokened, tokenless
copy-forward) made the promote gate non-fatal. Phase A removes the tokened half of that gate; Phase B
replaces the writer-distributed condemned list with a per-hash meta-descriptor.

## Motivation {#motivation}

Two pains, one root:

1. The writer↔GC freshness machinery accreted across redesigns (retire-view syncing, the `view_gate`
   install drain, the fence-round mid-closure refresh, per-leaf promote revalidation with resurrect
   machinery, retained sources) — each layer independently guards against blob deletion racing an in-flight
   commit, and most are redundant defense-in-depth after the B188 write reordering.
2. Writers must periodically download the ENTIRE retired list (`gc/state` + per-shard retired objects) —
   a term that scales as `condemned-set × writers × refresh-rate`, independent of writer activity, just to
   answer a point question at dedup time ("is this hash's current token condemned?").

Goal: maximal simplification preserving the core guarantees (`INV_NO_DANGLE`, `INV_NO_LOSS`,
`INV_NO_RETURN`), every deletion proven redundant by a TLA+ sabotage-flip.

## Part I — the load-bearing invariant: EDGE-BEFORE-OBSERVE {#edge-before-observe}

Current write order (`ContentAddressedTransaction::publishStaging`, `ContentAddressedTransaction.cpp:227-255`;
tokenless twin `CachedPartFolderAccess.cpp:194-200`; the ONLY production `putBlob` caller is
`publishStaging`, always post-precommit):

```
stageManifest      (manifest body durable — fold activation requires body present)
→ precommitAdd     (create-precommit RootOwnerEvent durable in the shard journal;
                    its closure names EVERY blob hash of the staged manifest)
→ putBlob loop     (the FIRST backend observations/adoptions happen here)
→ promote          (owner move)
```

**Every observation happens under an already-durable protecting edge.** Combined with three existing GC
mechanisms — fold activation of the precommit closure when the body is present
(`foldManifestEdges(+1)` `CasGc.cpp:894`, fold-barrier clamp on missing body `CasGc.cpp:897-901`),
per-hash in-degree settlement with `d > 0 → spared` ("recovery wins even past the floor",
`CasBlobInDegree.cpp:196-217`), and the two-phase delete that re-computes `d` at BOTH the graduation pass
and the delete pass (`d > 0` on a pending entry ⇒ impossible-spared, loud) — this yields the theorem:

> **A hash named in a durable precommit closure cannot be deleted by GC.** Deletion requires `d = 0` at two
> consecutive passes; any pass whose seal of the TARGET REF'S SHARD is taken after `precommitAdd` folds the
> closure edge ⇒ `d ≥ 1` ⇒ spared (the condemned entry is dropped). (Precision, consult finding E: the seal
> is per-shard at `readShard`, `CasGc.cpp:763` — not one pass-global seal. The argument is unaffected — the
> edge lives in the target shard's journal — but the TLA+ model must use per-shard seals.)

Case analysis for a blob observed by `putBlob` / named by the manifest:

- **Condemned after `precommitAdd`** (the 01156/01710/02346/03283 window): the condemning pass missed the
  edge; the next pass folds it ⇒ spared. Can never graduate. The condemnation is real but doomed — the
  promote gate was aborting (now resurrecting) an object that was never in danger.
- **Condemned AND graduated before `precommitAdd`** (pre-existing `delete_pending`): the writer's condemned
  check at the observation point sees it (Part I: the installed view — floor-guaranteed delivery; Part II:
  the meta says so directly) ⇒ refuse + re-upload fresh (INV-1). If the delete already executed ⇒ observed
  absent ⇒ fresh upload.
- **Delete pass racing `precommitAdd`**: sealed before the append ⇒ object deleted ⇒ observed absent
  (fresh upload); sealed after ⇒ edge folded ⇒ `d > 0` on pending ⇒ impossible-spared (loud log).

**Consult verdict (Phase A, adversarial, 2026-07-09): the theorem and the K1 race closure are SOUND — no
gap found.** The K1 chain verified at: graduate iff `condemn_round < min_ack` (`CasBlobInDegree.cpp:208`);
`min_ack` = min over live mounts' `observed_gc_round` (`CasServerRoot.cpp:533`); `observed_gc_round` ==
installed view round (`CasStore.cpp:459-462`); delete_pending persists in the retired list until redelete
(`CasBlobInDegree.cpp:213`, `CasRetireView.cpp:52-53`).

### The one check that stays forever: the dedup-adoption gate {#dedup-adoption-gate}

A writer adopting an existing incarnation does not mutate it — the adoption is INVISIBLE to an in-flight
delete decision. The concrete interleaving (K1): entry `delete_pending` in state N → pass N+1 seals the
target shard before our `precommitAdd` → its fold misses our edge → `d = 0` → `deleteExact` executes AFTER
our observation saw the object present → commit dangles. Neither the theorem (graduation predates the
edge) nor the two-phase re-check (its fold sealed before our append) covers it. The ONLY closure: at the
point of adoption, consult the hash's condemned status, and on condemned **displace** with a fresh
incarnation (exact-token discipline resolves the race with `deleteExact` in both orders). This is a
hazard-pointer-shaped protocol: in a content-addressed pool an object is globally addressable until the
byte-delete, so "unreachable for new references" can only be a writer-side promise. Part I sources the
status from the installed retire view (floor = delivery guarantee); Part II sources it from the per-hash
meta (point read = intrinsically fresh) and thereby deletes the floor and the list distribution.

## Part II — the meta-descriptor (Phase B end-state) {#meta-descriptor}

### Layout and invariant {#meta-layout}

Per blob hash, TWO objects:

- **Body** (unchanged key): `blobs/xx/<hash>` — the payload with its envelope. **Reads use it directly and
  never consult the meta** (the read path is untouched).
- **Meta**: `blobs/xx/<hash>.meta` (~100-200 bytes, fixed codec, `ca-inspect` support required):
  `{version, incarnation (u128 fresh tag), condemned (bool), condemn_round (u64), size (u64)}`.

> **INV-META-BODY: meta present ⇒ body present.** Maintained by ordering: **create bottom-up** (PUT body,
> then PUT meta If-None-Match), **delete top-down** (delete meta FIRST, then body). This invariant is what
> makes the 1-GET adopt safe.

**The meta is the linearization point for the hash's lifecycle.** All conditional lifecycle operations CAS
the 100-byte meta; the body's own conditional semantics become secondary (body writes are gated by having
won the meta CAS first). The envelope's in-body `incarnation_tag` stays for self-description/fsck but is no
longer the conditional authority.

### Protocols {#meta-protocols}

| Operation | Protocol |
|---|---|
| **Fresh upload** (`putBlob`, cache-miss) | optimistic PUT body (If-None-Match). Success ⇒ PUT meta (If-None-Match; a 412 here = racing writer won ⇒ re-GET meta ⇒ adopt). Body 412 ⇒ dedup path ↓ |
| **Dedup-adopt** (`putBlob`, cache-hit or body-412) | **ONE GET meta.** Present + clean ⇒ adopt (reference the body directly — INV-META-BODY guarantees presence, no body HEAD). Condemned ⇒ resurrect ↓. Absent + no body-412 seen ⇒ fresh-upload path. **Absent + body-412 (a crashed pre-meta birth left a debris body) ⇒ COMPLETE THE BIRTH:** PUT meta If-None-Match `{fresh incarnation, clean}` then adopt — the debris content is hash-correct by construction (its uploader wrote it conditionally under this content key); a 412 on this completion ⇒ someone else completed or the sweep claimed it ⇒ re-GET and follow. Prevents the body-412/meta-absent livelock |
| **Resurrect** (writer, own source bytes) | CAS meta (If-Match condemned-etag) → `{fresh incarnation, clean}`; THEN re-upload the body from the writer's OWN bytes (INV-1 — the dying body is never read). Loser of the CAS re-GETs and follows the new state |
| **Copy-forward** (tokenless leaf, no source) | CAS meta (If-Match condemned-etag) → `{fresh incarnation, clean}`; THEN GET body (the documented INV-1 exception — committed-source provenance), verify payload hash, re-wrap, PUT body |
| **Condemn** (GC, at fold `d=0`) | GET meta (etag) + CAS meta `{condemned=true, condemn_round}` |
| **Spare** (GC, `d>0` recovered) | CAS meta `{condemned=false}` — the reprieve is explicit and cheap |
| **Delete** (GC, two-phase pacing kept) | `deleteExact(meta, condemned-etag)` FIRST — the linearization: a racing resurrect's CAS and this delete target the same etag, exactly one wins. Only on success ⇒ HEAD body + `deleteExact(body, observed token)`. No tombstone needed |
| **Reads** | GET body by content key — meta never consulted |

Race audit:

- **Adopt vs fresh condemn:** adoption is still invisible, but a fresh condemn cannot reach delete —
  EDGE-BEFORE-OBSERVE spares it at the next fold (GC also CASes the meta back to clean).
- **Adopt vs graduated entry (K1):** the meta has said condemned for ≥2 full GC rounds before any delete is
  executable; a strongly-consistent GET cannot miss it ⇒ the writer resurrects instead of adopting. **The
  ack floor becomes unnecessary** — point-read freshness replaces list-delivery. (Backend requirement:
  strong read-after-write consistency — S3 since 2020, RustFS yes; recorded as a pool requirement.)
- **Resurrect vs delete:** both target the same meta etag with one CAS/exact-delete — exactly one wins; the
  loser re-reads and follows (delete-loser aborts the body delete; resurrect-loser sees absent ⇒ fresh
  upload; the pending body delete then misses the fresh token).
- **Two writers, fresh upload:** both PUT identical bodies (idempotent by content), second meta PUT 412s ⇒
  re-GET ⇒ adopt.
- **Debris sweep vs birth completion:** the unaccounted-debris sweep must follow the SAME top-down
  discipline — it first CLAIMS the hash with PUT meta If-None-Match `{condemned tombstone}`; a 412 means a
  writer completed the birth concurrently ⇒ not debris, skip; only the claim winner deletes the body (then
  the tombstone meta). Deleting a debris body without the claim races the birth-completion above into
  meta-without-body — forbidden (Gate B sabotage (e)).
- **Crash matrices:** body-without-meta (crash between the two creates) = invisible-to-dedup debris, swept
  by the claim-first debris pass above. Resurrect crash after meta-CAS, before body PUT: meta claims a
  fresh incarnation, body still carries the old envelope — content is IDENTICAL (same hash), reads correct;
  GC's aborted delete left the body alone; a later delete HEADs the body for the actual token (never trusts
  a possibly-stale meta hint). Meta-without-body cannot arise by construction (create bottom-up, delete
  top-down); out-of-band corruption is `ca-fsck`'s domain (new checks: meta⟺body pairing, stale-incarnation
  metas).

### What Phase B deletes (beyond Phase A) {#phase-b-deletions}

| Mechanism | Fate |
|---|---|
| Writer-side `RetireView` + retired-list downloads | **deleted** — writers never read `gc/state` or retired lists |
| Retired-view syncer | **deleted** |
| `observed_gc_round` in the beat + ack-floor graduation gating (`min_ack`) | **deleted** — graduation paces on rounds alone (`condemn_round < current_round`); the beat keeps the lease + `min_active` (precommit-reclaim floor) |
| Resurrect-supersede machinery (`ReplacedEntry`, peek_head) | **reduced** — incarnation supersession becomes a meta-generation compare at fold; sized during implementation |
| Retired lists as objects | **kept, GC-private** — the three-cursor merge remains the settlement engine (streaming O(condemned)); only its writer-distribution disappears. Deriving it from meta LISTs instead is rejected (full-prefix LIST per pass is dearer than the merge) |
| K1/K3 checks | **kept, re-sourced** — same sites, the condemned status now comes from the meta GET instead of the installed view |

### Budget (estimated 2026-07-09) {#meta-budget}

Per blob lifetime (steady state — every blob eventually dies): today 1 paid PUT; Phase B **3 paid PUTs**
(+1 meta at birth, +1 condemn CAS; deletes are free-class; spare is rare). Unit economics: **≈ +$10 per
million blobs churned** on AWS request pricing (+$5/M for the rejected lazy-marker alternative); meta
storage ~150 B × live blobs (negligible); read path — zero delta; adopt becomes ONE small GET (cheaper
class than today's HEAD+412 dance). Removed: the list-distribution term (`writers × (1+shards) GETs /
sync-interval` + MB-scale downloads under churn) — the badly-scaling term goes, a churn-linear term of
tiny ops arrives. Self-hosted RustFS: request cost ≈ 0; effects = +1-2 small-object IOPS per blob
lifetime, 2× object count (metadata pressure; fsck LIST covers 2× keys).

> ⚠️ **Mass-DROP requirement:** condemning 1M blobs = 1M meta CASes by the GC leader. Sequential at
> ~20 ms/op ≈ 5.5 h/pass — unacceptable. The GC condemn/spare/delete meta operations MUST run on a
> parallel pool (the delete phase already pays per-object ops today; same pool serves both).

**Rejected alternative — lazy marker** (meta created only at condemn): saves the birth PUT (+$5/M instead
of +$10/M) but adopt needs 2 calls (HEAD body + HEAD marker: marker absence no longer implies anything
about the body) and the body remains the conditional authority (today's heavyweight exact-token dance on
possibly-large objects stays). INV-META-BODY ("meta ⟺ body", 1-GET adopt, 100-byte linearization) is worth
one tiny PUT per blob.

## Deletions (Phase A — consult-reviewed) {#deletions}

| # | Mechanism | Anchor | Justification |
|---|---|---|---|
| D1 | Promote per-leaf HEAD + bounded resurrect loop for **tokened** deps | `CasBuild.cpp` revalidation loop | EDGE-BEFORE-OBSERVE + the putBlob gate (observing under the durable edge). **Boundary (consult finding A): skip iff `depIsTokened(hash)`; every NON-tokened leaf (tokenless copy-forwardable OR no-dep) keeps HEAD + fail-closed** — a no-dep absent leaf must ABORT, else a staging bug dangles |
| D2 | `retained_sources` map, `retainedSourceFor`, the `putBlob` `insert_or_assign` | `CasBuild.h`, `CasBuild.cpp:137-139, 216-221` | Existed only to feed D1 |
| D3 | Copy-forward **pre-pass** | `CasBuild.cpp:816-836` | One in-closure gate remains (K3). Trade-off (finding F): the rare condemned-tokenless copy-forward now runs its GET+PUT inside the shard CAS closure, briefly blocking the flat-combining queue; idempotent under CAS retry (re-run sees its own fresh token). Accepted — Phase B replaces the heavy body-dance with a meta CAS anyway |
| D4 | `view_gate` drain (shared/exclusive `shared_mutex`) | `CasStore.h:622-626`, `CasStore.cpp:690, 1243` | **Corrected justification (finding B):** K3 still reads the retire view inside the closure in Phase A, so "nothing reads the view in-closure" is false. Drain removal is safe because of entry persistence + monotone installs: a graduated entry (condemned at C) is in EVERY view ≥ C+1, and the writer's own advertised round exceeded C before graduation, so any later in-closure read sees the entry regardless of drain; displacement is exact-token-safe. The syncer installs under `RetireView`'s internal mutex (thread-safety was never the drain's job) |
| D5 | Writer-side `fence_round` mid-closure view refresh | `CasBuild.cpp:856-857` | Blob-side freshness is edge-protected; consult verified THM-NO-RETURN redundancy (a just-started writer is counted with its primed round before it can mutate). **TLA+-gated** (newborn sabotage must flip); GC-side `fence_round` uses stay (round recovery `CasGc.cpp:1872`, `birth_floor_provider` `CasStore.cpp:1319`, codec/inspect) |
| D6 | `DepEntry::observed_view_round` | `CasBuild.h:110` | Zero readers; **four writer sites** (finding D: positional aggregate inits at `CasBuild.cpp:313, 388, 628, 636`) — all four edited with the field removal |
| D7 | `hasDep` (finding G) | `CasBuild.cpp:228-241` | No non-test callers after D1; **`depIsTokened` is RETAINED** — it becomes the D1 loop boundary predicate (correcting the finding's "both dead") |
| D8 | Backlog variants A + B | `BACKLOG.md` | Superseded: the pin is strictly weaker than the durable edge |

**No paranoid mode.** Deleted checks are not kept behind a config flag: fallback paths hide bugs
(CLAUDE.md), and the clamp-suppression incident (31 dangles, `CasBlobInDegree.h:91`) hit COMMITTED
manifests — the promote gate never covered that class. Detection stays with `ca-fsck`, soak assertions,
and `system.content_addressed_log`.

## Kept (and why) {#kept}

| # | Mechanism | Why it stays |
|---|---|---|
| K1 | `putBlob` dedup-adoption gate (condemned ⇒ displace fresh, INV-1, W-FRESH-TAG, bounded retry) | **Safety-critical** — §the-dedup-adoption-gate. Phase A source: installed view (floor-delivered). Phase B source: the meta GET |
| K2 | Promote owner-liveness check + body read + `RefMatchesBody` + namespace match | Owner-move correctness (`WPromote owner==bld`); unrelated to blob freshness. Also the abandon/reclaim defense (consult-verified: reclaim touches only DEAD builds via `min_active`; owner check aborts over any reclaimed precommit) |
| K3 | Promote: ONE presence check per **non-tokened** leaf (tokenless OR no-dep); absent ⇒ `ABORTED`; condemned-present + copy-forwardable ⇒ copy-forward; condemned + no-dep ⇒ `ABORTED` | `adoptEvidence` is observation-free (B188) — the single mandatory presence observation; the no-dep arm is the fail-closed staging-bug guard (finding A). Phase A: HEAD body + view check. Phase B: GET meta — absent meta ⇒ ABORT (the hash is not referencable; note absent-meta does NOT imply absent body — debris bodies exist — but a legitimate tokenless leaf comes from a committed manifest whose blob was fully born, so its meta exists until condemned-and-deleted, and fail-closed is correct either way) |
| K4 | Mount lease + beat (`min_active`), TTL fence-out, local write fence, epoch/self-remount | Liveness backbone. Phase B drops only `observed_gc_round` from the beat |
| K5 | GC three-cursor merge, two-phase delete pacing, fold barriers | The settlement engine and the theorem's pillars — untouched in both phases |
| K6 | Read path | Untouched in both phases (verified: `retireView` has no consumers outside Build/Store/Gc; reads never touch the meta) |

**Promote cost:** Phase A — body GET + (non-tokened leaf count) HEADs + shard CAS; for `INSERT` (all
tokened) **zero** revalidation HEADs. Phase B — same with HEADs→tiny meta GETs.

## Ordering becomes a checked protocol invariant {#ordering-invariant}

EDGE-BEFORE-OBSERVE makes `stageManifest → precommitAdd → putBlob` load-bearing. Guards:

- Comment blocks at `publishStaging`'s precommit site and at `Build::putBlob` referencing this spec and the
  TLA+ order-sabotage config.
- A `chassert(precommitted)` in `Build::putBlob`'s **adopt paths only** (the HEAD-first admit and the
  dedup-adopt): adopting an existing incarnation without a durable closure edge is the exact unprotected
  shape (the adopted blob carries the ORIGINAL writer's `build_id`, so the debris watermark does not cover
  it). A FRESH upload before precommit stays legal — protected by the newborn-debris watermark (`build_id`
  + `min_active`; consult-verified: a brand-new blob is never merge-visited, `CasBlobInDegree.cpp:271`).
  Tests that dedup-adopt pre-precommit are rewritten to the wiring order in Phase A.
- The TLA+ order sabotage is the formal guard.

## TLA+ gates {#tla-gates}

Technique: **redundancy proof by sabotage-flip** — a config that today must fail proves a mechanism
load-bearing; the same sabotage passing in the extended model proves the mechanism redundant. Models must
use **per-shard seals** (finding E).

**Gate A (before Phase A code):**

1. Extend the model (base: `CaBuildRootPrecommit.tla` closure/fold/in-degree + `CaIncarnationCore.tla`
   tokens/retire; expect a small composition) with the writer order: durable closure append precedes every
   observation; fold activates closure edges (body present ⇒ activated); settlement spares `d > 0`;
   two-phase delete re-checks `d`.
2. **Must FLIP green:** `SabotageNoReobserve` (stale-view publish re-observation); the newborn/THM-NO-RETURN
   writer-refresh case against the shard-incarnation model — if it does NOT flip, D5 is cancelled (the
   spec's only conditional deletion).
3. **Must STAY red:** (a) order sabotage — adoption before the durable closure append (the pre-B188 shape)
   must dangle; (b) K3 removed — a tokenless/no-dep absent leaf published blind must dangle; (c) K1 removed
   (`SabotageNoRetireView` exists) — dedup-adoption without the condemned check must dangle via the
   pre-precommit-graduated interleaving; (d) **finding C:** the K3-shaped tokenless sabotage — adopt-at-
   promote of a pre-graduated condemned leaf with the check removed — as a DISTINCT action from (c)'s
   putBlob shape.
4. **Positive:** the reduced model (no tokened revalidation, no drain, no writer fence refresh) holds
   `INV_NO_DANGLE`, `INV_NO_LOSS`, `INV_NO_RETURN`, `INV_JOURNAL_COVERAGE`, `MonotoneGC`.

**Gate B (before Phase B code):**

1. Model the meta as an atomic register per hash: `{incarnation, condemned}` + present/absent; body
   presence tied by INV-META-BODY; writer adopt/resurrect and GC condemn/spare/delete as CASes on it; NO
   retire view, NO floor, NO round-ack.
2. **Must hold:** `INV_NO_DANGLE`, `INV_NO_LOSS`, INV-META-BODY, INV_NO_RETURN reformulated over meta
   generations (a deleted generation never becomes current again).
3. **Must STAY red:** (a) create order flipped (meta before body) — adopt of a meta whose body never landed
   must dangle; (b) delete order flipped (body before meta) — 1-GET adopt of a clean meta over a deleted
   body must dangle; (c) adopt without the meta GET (blind adopt) — the K1 interleaving must dangle;
   (d) GC body-delete unconditional on winning the meta delete — must dangle against a racing resurrect;
   (e) debris sweep deleting the body WITHOUT first claiming the meta — must produce meta-without-body
   against a racing birth-completion.
4. **Must FLIP green:** the Gate-A floor/view sabotages re-run in the meta model (list machinery absent
   entirely) — proving the floor's job moved into the point-read.

## Implementation phases {#implementation-phases}

**Phase A (= the consulted Tier-2, with findings folded in):**

1. TLA+ Gate A (no code before green; D5 resolved here).
2. D1+D2+D3+D6+D7 with test migration: `PromoteResurrectsCondemnedTokenedBlob` → replaced by "promote
   succeeds WITHOUT touching the blob (token unchanged)"; `PromoteAbandonedPrecommitAbortsWithoutResurrect`
   kept (owner-check abort); `FenceConflict…`/`WedgedHeartbeat…`/`RevalidateReObserves…` rewritten to the
   new contract (success, no re-upload, token unchanged); `RevalidateAbsentTokenedBlob…` **deleted**
   (premise protocol-unreachable under EDGE-BEFORE-OBSERVE — hand-deleting a putBlob'd blob is out-of-band
   corruption, `ca-fsck`'s domain; documented in the test header); tokenless + no-dep tests kept as-is;
   the ordering guards (§Ordering).
3. D4 (drain removal): syncer installs under `RetireView`'s internal mutex; `flushShardBatch` drops the
   shared lock.
4. D5 if Gate A flipped it; D8 backlog notes.
5. Validation: full `Ca*/Cas*` gtests (known-flaky `CaWiring*` set not grown) → the 4 condemn-race
   stateless tests + full CA-s3 lane (0 promote aborts, 0 fsck dangles) → **soak with the fsck gate**.

**Phase B (after Phase A + one clean soak):**

1. TLA+ Gate B.
2. Meta codec + layout + `ca-inspect`/`ca-fsck` support (INV-META-BODY checks, stale-incarnation pruning).
3. `putBlob` rewrite (optimistic body PUT → meta PUT; dedup = 1 meta GET; resurrect = meta CAS + body
   re-upload; **birth completion** on body-412+meta-absent); K3 re-source (meta GET); copy-forward
   re-shape (meta CAS first).
4. GC: condemn/spare = meta CAS, delete = meta-first exact-delete then body — **on a parallel pool**
   (mass-DROP requirement); **claim-first debris sweep** (PUT tombstone meta If-None-Match before any
   debris body delete); supersede machinery reduction; event-log events for meta transitions.
5. Delete: writer-side `RetireView`, syncer, `observed_gc_round` (beat + floor R1 min_ack gating —
   graduation paces on rounds), the last view consumers.
6. Validation: gtests → full CA-s3 lane → soak; plus a mass-DROP scenario (utils/ca-soak) sizing the
   parallel-pool throughput.

## Non-goals {#non-goals}

- Read-path changes (none in either phase).
- Compatibility scaffolding: CAS is pre-release (standing rule). Phase B changes the pool layout (metas) —
  existing dev pools are recreated, no migration.
- Replacing the GC-internal three-cursor merge with meta LISTs (rejected — cost).

## Consult findings register (Phase A review, 2026-07-09) {#findings-register}

A (Important) → folded into D1/K3 (non-tokened boundary). B (Important) → D4 justification corrected.
C (Important) → Gate A 3(d). D (Minor) → D6 four writer sites. E (Minor) → per-shard seal precision
(theorem + models). F (Minor) → D3 trade-off note. G (Minor) → D7, with `depIsTokened` retained as the
boundary predicate. Consult verified-negative: floor→view→beat chain airtight; chassert premise safe;
D5 THM-NO-RETURN redundancy; read path clean. **Phase B requires its own adversarial consult before its
plan** (the meta protocols and crash matrices above are design-level, not yet independently reviewed).
