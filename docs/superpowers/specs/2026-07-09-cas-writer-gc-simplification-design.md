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

### OPEN REFINEMENT (user, 2026-07-10): raw bodies — eliminate the blob envelope entirely {#raw-body-refinement}

The blob envelope/header's ONLY load-bearing job is the `incarnation_tag` — it varies the body's etag per
incarnation so exact-token (`If-Match`) deletes and the resurrect/displace dance work. Everything else
(`logical_hash` = the key, `logical_size` = object metadata / a meta field, `build_id`/`provenance`/
`intended_ref` = diagnostics, `hash_algo`/`domain_id`/`pad_to_header_len` = framing) is non-load-bearing.
Once the META owns the incarnation and is the sole linearization point, the body no longer needs a varying
etag, so the header can be dropped ENTIRELY and the body becomes RAW immutable content:

- **Body** = raw payload, write-once (`PUT If-None-Match` on the content key), etag = content hash. No envelope.
- **Meta** = the sole conditionally-operated object, with a THREE-state lifecycle `{clean, condemned, tombstone}`
  (`gen` = the etag = the only linearization token; no body token anywhere).
- **Resurrect (from `condemned` ONLY)**: if the body is still present, resurrect = `CAS meta condemned→clean`
  — NO body re-upload (same content). Its `condemned→clean` CAS races GC's `condemned→tombstone` on the shared
  condemned etag → exactly one wins. (Supersedes the Part-II resurrect-re-uploads-body and the C2 exact-token
  body delete.)
- **Delete = a TERMINAL tombstone handshake** (replaces C2): (1) `CAS meta condemned→tombstone` (If-Match, wins
  the race vs a resurrect); (2) delete the raw body; (3) delete the tombstone meta (→ `absent`). Tombstone is
  **terminal**: nothing un-tombstones it. A writer observing `tombstone` **waits** (bounded) for the meta to
  reach `absent`, then fresh-uploads a new incarnation — it MUST NOT `CAS tombstone→clean`.
- **Read path** simplifies too (raw GET, no header framing; reads still never touch the meta). **fsck**
  self-identifies a body by re-hashing it to its key (copyForward already does this).

> **v2 CORRECTION (consult 2026-07-10, CRITICAL — terminal tombstone).** The first cut had a writer
> "complete" a `tombstone` by re-uploading the body + `CAS tombstone→clean`. Under raw **immutable** bodies
> this is UNSAFE: the body's etag never changes on re-upload (content-addressed), so GC's already-committed
> body delete (decided when it won the tombstone) still hits the body → the just-resurrected committed ref
> **dangles**. In the old envelope design a resurrect minted a fresh `incarnation_tag` → new body etag → GC's
> exact-token delete *missed* the live body; raw bodies removed exactly that shield. Why S3 cannot make it
> safe: a body delete cannot be made atomically conditional on a *different* object's (the meta's) state.
> **Fix:** tombstone is terminal (above). `Gate B v2` (`CaMetaDescriptorRaw.tla`) models the body delete as a
> NON-ATOMIC two-step gated on a delete-commitment flag (not the current meta state) and adds
> `SabResurrectFromTombstone`, which re-enables the bug and MUST break `INV_NO_LOSS` — proving
> terminal-tombstone load-bearing. `INV_NO_LOSS` (`gcDeleteCommitted ⇒ no live ref`) + `INV_NO_RETURN` added.
>
> **v2 CORRECTION (consult finding #8 — meta carries a fresh `incarnation` nonce).** S3 ETags are
> content-derived, so a `clean` meta re-encodes to an identical etag across incarnations (a latent ABA on the
> `clean→condemned` precondition). The meta therefore carries a fresh `incarnation` (u128) nonce on EVERY
> write, making each meta object globally unique (etag = incarnation, literally true on S3) and matching the
> model's fresh-`gen`-per-write assumption. This restores the §meta-layout `incarnation` field the first
> raw-body cut had dropped.

STATUS: a genuine further simplification (drops the entire blob envelope + the body *conditional* token), a
PROTOCOL change to the delete path (C2 → terminal tombstone). Gate B v2 RE-RUN GREEN (reduced holds
`INV_NO_DANGLE`, `INV_META_BODY`, `INV_NO_LOSS`, `INV_NO_RETURN`) with all five sabotages RED
(`meta_first`, `blind_adopt`, `adopt_tomb`, `del_notomb`, `resurrect_tomb`). Applies to the new meta layout
only — the current pre-meta code keeps its envelope.

### Layout and invariant {#meta-layout}

Per blob hash, TWO objects:

- **Body** (unchanged key): `blobs/xx/<hash>` — the **raw** payload (v2 raw-body: NO envelope), write-once
  via `PUT If-None-Match`, etag = content. **Reads use it directly at offset 0 and never consult the meta**
  (the read path drops only the header offset).
- **Meta**: `blobs/xx/<hash>.meta` (fixed codec, `ca-inspect` support required):
  `{version, incarnation (u128 fresh nonce, minted on EVERY meta write), state (clean|condemned|tombstone),
  condemn_round (u64), size (u64)}`. The `incarnation` nonce makes each meta object's bytes — and thus its
  S3 etag — globally unique (finding #8), so "etag = incarnation" is literally true.

> **INV-META-BODY: a `clean` or `condemned` meta ⇒ body present.** Maintained by ordering: **create bottom-up**
> (PUT body, then PUT meta If-None-Match), **delete top-down** (`condemned→tombstone` CAS, then delete body,
> then delete tombstone meta). A `tombstone` meta is the mid-delete state where the body may already be gone;
> an `absent` meta says nothing. This invariant is what makes the 1-GET adopt safe.

**The meta is the SOLE linearization point for the hash's lifecycle.** All conditional lifecycle operations
CAS the meta (keyed on its etag = incarnation); the raw body has no conditional token — it is created
write-once and deleted only by GC under a won tombstone claim (never re-uploaded over a present body, so it
never needs a varying etag). Tombstone is **terminal**.

### Protocols {#meta-protocols}

(v2 raw-body / terminal-tombstone. Every meta write mints a fresh `incarnation` nonce; the meta etag is the
sole conditional token; the raw body has no conditional token.)

| Operation | Protocol |
|---|---|
| **Fresh upload** (`putBlob`, cache-miss) | optimistic PUT raw body (If-None-Match). Success ⇒ PUT meta (If-None-Match `{fresh incarnation, clean, size}`; a 412 here = racing writer won ⇒ re-GET meta ⇒ adopt/follow). Body 412 ⇒ dedup path ↓ |
| **Dedup-adopt** (`putBlob`, cache-hit or body-412) | **ONE GET meta.** `clean` ⇒ adopt (reference the body directly — INV-META-BODY guarantees presence, no body HEAD). `condemned` ⇒ resurrect ↓. `tombstone` ⇒ **wait** ↓. `absent` + body present ⇒ **BIRTH-COMPLETION** (crashed pre-meta birth): re-establish the body from the writer's OWN re-readable `BlobSource` (`PUT If-None-Match`; a 412 = already present, fine) THEN PUT meta If-None-Match `{fresh incarnation, clean}`; a 412 on the meta ⇒ re-GET and follow. `absent` + body absent ⇒ fresh-upload path |
| **Resurrect** (writer, from `condemned` ONLY) | CAS meta (If-Match condemned-etag) → `{fresh incarnation, clean}`. Body is present and immutable ⇒ **NO body re-upload** (raw-body). The `condemned→clean` CAS races GC's `condemned→tombstone` on the same etag → exactly one wins; the loser re-GETs and follows (if GC won the tombstone, the writer now sees `tombstone` and waits ↓) |
| **Wait-on-tombstone** (writer) | `tombstone` is terminal — the content is being deleted. Bounded re-GET/backoff until the meta reaches `absent` (GC finished), then take the fresh-upload path. NEVER `CAS tombstone→clean` (v2: that dangles — see the terminal-tombstone correction). Bounded exhaustion ⇒ `ABORTED` (the build restarts) |
| **Copy-forward** (tokenless leaf, no source, committed-source provenance) | GET meta. `clean` ⇒ ok. `condemned` ⇒ CAS meta (If-Match condemned-etag) → `{fresh incarnation, clean}` (body present & immutable — no body touch). `tombstone`/`absent` ⇒ fail closed (`ABORTED`) — the content is dead; the writer restarts. (The old envelope re-wrap is gone; raw bodies are immutable.) |
| **Condemn** (GC, at fold `d=0`) | GET meta (etag); if `clean` ⇒ CAS meta (If-Match clean-etag) → `{fresh incarnation, condemned, condemn_round}`; the resulting condemned-etag is stored in the retired ledger as the delete precondition |
| **Spare** (GC, `d>0` recovered) | CAS meta (If-Match condemned-etag) → `{fresh incarnation, clean}` — the reprieve is explicit and cheap |
| **Delete** (GC, graduated; two-phase pacing kept; on a parallel pool) | **Tombstone handshake:** (1) `CAS meta (If-Match condemned-etag) → {tombstone}` — the linearization: a racing resurrect's `condemned→clean` and this both target the condemned-etag, exactly one wins; a lost CAS ⇒ abort the delete (spared/resurrected). (2) On win ⇒ delete the raw body — HEAD body + `deleteExact(body, head-token)`; safe because tombstone is terminal, so no writer re-established the body after the win (a fresh HEAD here cannot observe a live resurrected body — the v2 fix). (3) `deleteExact(meta, tombstone-etag)` → `absent`. **Idempotent redelete (consult I3):** on retry after a crash mid-handshake, an already-`absent`/already-`tombstone` meta still drives the body delete + tombstone-meta delete to completion, so a meta-less body is never stranded |
| **Reads** | GET raw body by content key at offset 0 — meta never consulted |

Race audit:

- **Adopt vs fresh condemn:** adoption is still invisible, but a fresh condemn cannot reach delete —
  EDGE-BEFORE-OBSERVE spares it at the next fold (GC also CASes the meta back to clean).
- **Adopt vs graduated entry (K1):** the meta has said condemned for ≥2 full GC rounds before any delete is
  executable; a strongly-consistent GET cannot miss it ⇒ the writer resurrects instead of adopting. **The
  ack floor becomes unnecessary** — point-read freshness replaces list-delivery. (Backend requirement:
  strong read-after-write consistency — S3 since 2020, RustFS yes; recorded as a pool requirement.)
- **Resurrect vs delete (v2):** resurrect is legal only from `condemned`; its `condemned→clean` CAS and GC's
  `condemned→tombstone` CAS both target the condemned-etag → exactly one wins. Resurrect wins ⇒ GC's tombstone
  CAS 412s ⇒ GC aborts (no body delete). GC wins ⇒ tombstone is terminal ⇒ the writer now sees `tombstone`
  and waits for `absent`, then fresh-uploads. **Neither order dangles** (the v1 `tombstone→clean` race that
  did is forbidden — `SabResurrectFromTombstone`, Gate B v2 red).
- **Two writers, fresh upload:** both PUT identical raw bodies (idempotent by content), second meta PUT 412s ⇒
  re-GET ⇒ adopt.
- **Debris sweep vs birth completion:** the unaccounted-debris sweep follows the top-down discipline — it
  first CLAIMS the hash with `PUT meta If-None-Match {tombstone}`; a 412 means a writer completed the birth
  concurrently ⇒ not debris, skip; only the claim winner deletes the body (then the tombstone meta). Deleting
  a debris body without the claim races birth-completion into meta-without-body — forbidden (Gate B sabotage
  `del_notomb`).
- **Crash matrices (v2):** body-without-meta (crash between the two creates) = invisible-to-dedup debris,
  completed by a later writer's birth-completion (from source) or swept by the claim-first debris pass.
  Resurrect crash after the meta-CAS `condemned→clean`: the body was untouched (raw, immutable) and the meta
  is `clean` with a fresh incarnation — fully consistent, reads correct. GC delete crash mid-handshake:
  idempotent redelete (I3) drives it to `absent`. Meta-without-body (a `clean`/`condemned` meta with no body)
  cannot arise by construction; out-of-band corruption is `ca-fsck`'s domain (new checks: meta⟺body pairing;
  a `body_without_meta` is benign pre-birth debris, a `meta_without_body` is an INV-META-BODY violation).

### What Phase B deletes (beyond Phase A) {#phase-b-deletions}

| Mechanism | Fate |
|---|---|
| Writer-side `RetireView` + retired-list downloads | **deleted** — writers never read `gc/state` or retired lists |
| Retired-view syncer | **deleted** |
| `observed_gc_round` in the beat + ack-floor graduation gating (`min_ack`) | **deleted** — graduation paces on rounds alone (`condemn_round < current_round`); the beat keeps the lease + `min_active` (precommit-reclaim floor) |
| Resurrect-supersede machinery (`ReplacedEntry`, peek_head) | **re-shaped, NOT mechanically reduced (consult I1 — this mechanism previously leaked live, RESURRECT-REUPLOAD-ORPHAN):** (a) the GC ledger stores the **meta condemn-etag** as the delete precondition (today: only the body token) — condemn = GET meta + capture etag + CAS condemned; (b) the fold re-reads the meta (`peek_meta`, the analog of today's `peek_head`) for net-zero-touched entries, and on a generation mismatch supersedes the stale entry + re-condemns the current generation; (c) the untouched-entry drain path (`settleRetiredBelow`) has no peek — its safety rests on the induction "every resurrect's edges fold in some window where the blob is touched": stated here as an invariant and modeled in Gate B. Delete-SAFETY needs no re-read (the meta etag precondition covers it); the peek is for LEDGER correctness (not orphaning resurrected incarnations) |
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

**Gate B — RE-RUN GREEN v2 (`CaMetaDescriptorRaw.tla`, 2026-07-10):**

1. The meta is a per-hash register `{incarnation (fresh nonce per write), state ∈ {clean,condemned,tombstone},
   condemn_round}` with a fresh `gen` (= etag) minted on every write; body presence is a boolean tied by
   INV-META-BODY; writer FreshUpload/Adopt/Resurrect(from condemned)/BirthCompletion(from absent) and GC
   Condemn/Spare/ClaimTombstone/DeleteBody/DeleteMeta as actions on it; NO retire view, NO floor, NO
   round-ack. **The body delete is a NON-ATOMIC two-step gated on a delete-commitment flag** (set when GC
   wins the tombstone), NOT on the current meta state — faithfully modeling "GC decided, gap, then deletes",
   which is what makes the C2 race expressible. (The I1 untouched-entry ledger induction is NOT captured at
   this single-hash granularity — it is covered by targeted Task-5 tests; noted.)
2. **Holds (reduced, GREEN):** `INV_NO_DANGLE`, `INV_META_BODY`, `INV_NO_LOSS` (`gcDeleteCommitted ⇒ no live
   ref`), `INV_NO_RETURN` (a deleted meta gen never becomes current again).
3. **Stays RED (all five sabotages):** `sab_meta_first` (create order flipped — meta before body → dangle);
   `sab_blind_adopt` (adopt ignores condemned/tombstone → `INV_NO_LOSS`); `sab_adopt_tomb` (adopt over
   tombstone → `INV_NO_LOSS`); `sab_del_notomb` (GC deletes body under condemned without the tombstone claim
   → `INV_META_BODY`); **`sab_resurrect_tomb`** (v2, THE C2 fix: a writer un-tombstones `tombstone→clean`
   while GC has committed to delete → `INV_NO_LOSS` — proves **terminal-tombstone** load-bearing).
   **Superseded:** the original envelope-era sabotage (g) "GC body-delete keyed on a FRESH HEAD instead of the
   condemn-time token" — under raw immutable bodies there is no displaced body; a fresh HEAD after a won
   terminal tombstone is safe. `sab_resurrect_tomb` is its correct raw-body replacement.
4. **Floor/view redundancy:** the Gate-A floor/view machinery is absent from the model entirely (no retire
   view, no ack floor) and the reduced model still holds — the floor's job moved into the point-read.

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

1. TLA+ Gate B — RE-RUN GREEN v2 (terminal tombstone; done 2026-07-10) + the wedge committed-removal gate.
2. Meta codec (`{version, incarnation nonce, state, condemn_round, size}`) + layout (`blobMetaKey`) +
   `ca-inspect`/`ca-fsck` support. `ca-inspect`: the `.meta` suffix must dispatch BEFORE the `blobs/`-prefix
   branch (`CasInspect.cpp:422`), AND the raw-body branch must render a raw body (re-hash) not decode an
   envelope. `ca-fsck`: INV-META-BODY pairing (`meta_without_body` = violation; `body_without_meta` = benign
   pre-birth debris). `rebuildBaseline`: `blobs/` LIST skips `.meta`; capture meta state + store the
   condemned-meta-etag; repair INV-META-BODY breaks (consult I2).
3. `putBlob` rewrite (optimistic RAW body PUT If-None-Match → meta PUT If-None-Match; dedup = 1 meta GET;
   resurrect = meta CAS from **condemned only**, NO body re-upload; **wait-on-tombstone** then fresh-upload;
   **birth completion** from source on absent-meta+present-body); read path offset 0 (drop the envelope);
   K3 re-source (meta GET); copy-forward re-shape (meta CAS, no re-wrap; tombstone/absent ⇒ ABORT).
4. GC: condemn = GET meta + CAS clean→condemned (store condemned-etag); spare = CAS condemned→clean; delete =
   **terminal tombstone handshake** (CAS condemned→tombstone; on win HEAD+delete body; delete tombstone meta;
   idempotent redelete I3) — **on a parallel pool** (mass-DROP); **claim-first debris sweep** (PUT tombstone
   meta If-None-Match before any debris body delete); the I1 `peek_meta` supersede re-shape (ledger stores the
   condemned-meta-etag); `graduationDue` re-keyed to `condemn_round < current_round` (consult M5 — verified
   safe); event-log events for meta transitions.
5. Delete the writer-side `RetireView`, syncer, `observed_gc_round` (beat + `min_ack`/`max_ack` gating —
   graduation paces on rounds) and every residual consumer (`RoundReport::min_ack`, `graduationDueForTest`,
   `CasInspect` mount-lease `observed_gc_round` render) — the last view consumers.
6. Validation: gtests (incl. a deterministic GC-delete-vs-writer-resurrect race test) → full CA-s3 lane →
   soak; plus a mass-DROP scenario (utils/ca-soak) sizing the parallel-pool throughput.

## Non-goals {#non-goals}

- Read-path changes (none in either phase).
- Compatibility scaffolding: CAS is pre-release (standing rule). Phase B changes the pool layout (metas) —
  existing dev pools are recreated, no migration.
- Replacing the GC-internal three-cursor merge with meta LISTs (rejected — cost).

## Consult findings register (Phase A review, 2026-07-09) {#findings-register}

**Phase A consult (SOUND-WITH-CHANGES):** A (Important) → folded into D1/K3 (non-tokened boundary).
B (Important) → D4 justification corrected. C (Important) → Gate A 3(d). D (Minor) → D6 four writer sites.
E (Minor) → per-shard seal precision (theorem + models). F (Minor) → D3 trade-off note. G (Minor) → D7,
with `depIsTokened` retained as the boundary predicate. Verified-negative: floor→view→beat chain airtight;
chassert premise safe; D5 THM-NO-RETURN redundancy; read path clean. TLA+ Gate A subsequently RAN GREEN
(reduced) with all four sabotages red.

**Phase B consult (SOUND-WITH-CHANGES, 2026-07-09):** C1 (Critical) → birth-completion re-specified as
resurrect-from-source (the meta-absent∧body-present state is GC's transient delete window, never adoptable);
C2 (Critical) → GC body delete keys on the condemn-time token, never a fresh HEAD. I1 (Important) → the
supersede re-shape specified (`peek_meta`, ledger stores the meta condemn-etag, the untouched-entry
induction stated + Gate B property). I2 (Important) → rebuild captures meta state + INV-META-BODY repair.
I3 (Important) → idempotent redelete past an already-absent meta. Verified: the no-floor argument SOUND
with the minimal pipeline guarantee stated and code-provided; backend primitives key-agnostic across all
four backends (no gap); dedup-cache safe iff the meta GET is preserved on cache-hit (it is — spec'd);
`ca-inspect` `.meta` dispatch bug pre-identified. M2/M4/M5 folded (fsck tolerance, `condemn_round` in the
Gate B register, `graduationDue` re-key). Gate B gained sabotages (f) and (g).
