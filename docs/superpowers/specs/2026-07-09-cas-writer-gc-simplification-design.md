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

### v3 (user, 2026-07-10): KEEP the in-body incarnation tag; the meta is a freshness point-read {#raw-body-refinement}

> **This supersedes the v1 "raw bodies — eliminate the envelope entirely" and v2 "terminal tombstone" cuts
> (both recorded below as REJECTED). The user identified, from git history, that both recreated an
> already-rejected design.**

**What went wrong (v1/v2).** The v1 cut dropped the blob envelope ENTIRELY, including the in-body
`incarnation_tag`, making the body raw immutable content (etag = content hash, fixed). But the `incarnation_tag`
is the ONE load-bearing envelope field: it varies the body's etag per incarnation so `resurrect` displaces the
body (fresh etag) and GC's exact-token `DELETE If-Match` MISSES a resurrected body. Dropping it removed that
shield, so a resurrect could no longer displace the body — which forced the v2 "terminal tombstone + writer
waits on GC" contortion (a writer↔GC liveness coupling). The alternative I then explored — per-incarnation body
keys `blobs/xx/<hash>.<inc>` — is **exactly the generation-in-key design already REJECTED** (see
`docs/superpowers/cas/01-architecture.md` §"Approaches tested and REJECTED": EBR's `blobs/<H>/<g>` and Merkle
`child_gen`): it forces a `404→LIST` read path, propagates the incarnation UP into the manifest/parent (breaking
the pure-content manifest and FUSE-readiness), and needs per-hash floors/pins/Keeper. The settled 2026-06-10
design deliberately put **incarnation in the BODY, one key per hash**, so "resurrecting a blob produces a
distinct backend token without changing any parent's hash or key."

**v3 corrected design — keep the settled safety core, add ONLY the Phase-B freshness win:**

- **Body** = a MINIMAL in-body `incarnation_tag` (16 B) + raw payload. Drop the rest of the envelope junk
  (`logical_hash`, `build_id`, `provenance`, `intended_ref`, `hash_algo`, `domain_id`, magic/padding) — the
  user's "keep the tag, drop the chepukha". One key per hash; reads use a CONSTANT offset (like today's
  `blob_header_len`), stay content-addressed, and the manifest stays pure content (FUSE-ready preserved).
- **Delete stays exact-token on the BODY** (the validated `CaIncarnationCore` core): GC captures the body's
  token at condemn and `DELETE If-Match token`. A `resurrect` overwrites the body with a FRESH `incarnation_tag`
  → distinct etag → GC's exact-token delete MISSES (TokenMismatch) → survives. **No tombstone. No wait. No
  per-incarnation keys. No cross-object atomicity.**
- **Resurrect** = overwrite the body from the writer's OWN source with a fresh `incarnation_tag` (the original
  behaviour; the writer holds the bytes at `putBlob`; tokenless copy-forward GETs the condemned body once, the
  documented INV-1 exception). This costs one body re-upload on resurrect — the accepted, month-soaked cost.
- **The META is a per-hash FRESHNESS POINT-READ, NOT the linearization.** Its ONLY job is to answer the
  dedup-adoption question — "is this hash's current incarnation condemned?" — as an O(1) point read, replacing
  the O(condemned-set) writer-side retire-view download (the actual Part-II motivation). It is a **2-state
  marker `{clean, condemned}`** (+ `condemn_round` for pacing/introspection). GC sets it condemned at condemn,
  clears it at spare, deletes it at delete. Because delete-safety lives in the BODY's exact-token delete, the
  meta being momentarily behind is harmless for safety (it only ever makes a writer resurrect conservatively);
  a strongly-consistent GET (S3 RAW consistency, a pool requirement) makes it fresh enough for the K1 gate.
- **Read path unchanged in shape** (content key + constant offset; reads never touch the meta). **Manifest
  unchanged** (pure content hashes). **fsck** gains a meta⟺body pairing check.

STATUS (v3): reverts the raw-body over-reach; keeps one-key-per-hash + in-body tag + exact-token delete — the
`CaIncarnationCore` safety core (TLA+ + Apalache-inductive + one-month soak) is UNCHANGED. The only new
mechanism is the freshness meta replacing the retire-view; its modeling obligation is small — "the meta
point-read is at least as fresh as the retire-view for the K1 dedup-adoption gate" — and delete-safety needs no
new model (it is the unchanged exact-token core). Task 1's meta codec is reused; the meta needs no fresh-nonce
linearization etag (it is not the linearizer), so Task 1B is not load-bearing here.

<details><summary>REJECTED cuts (kept for the record)</summary>

- **v1 — raw bodies, no envelope, meta = sole 3-state linearization `{clean,condemned,tombstone}`.** Dropped the
  load-bearing in-body tag → body etag fixed → resurrect can't displace → needed the tombstone handshake.
- **v2 — terminal tombstone** (`CaMetaDescriptorRaw.tla`, was GREEN): a writer observing `tombstone` waits for
  `absent` then re-uploads; never `tombstone→clean`. Correct but couples writer liveness to GC and keeps the
  meta as sole linearizer. Superseded by v3 (the in-body tag makes the tombstone unnecessary).
- **Option B — per-incarnation body keys** (`CaMetaIncarnationKey.tla`, was GREEN): the already-rejected
  generation-in-key design (404→LIST, manifest carries the incarnation, breaks FUSE-readiness).

Both models remain in `docs/superpowers/models/` as explored-and-rejected evidence.
</details>

### Layout and invariant {#meta-layout}

(v3.) Per blob hash, TWO objects:

- **Body** (unchanged key): `blobs/xx/<hash>` — a MINIMAL in-body `incarnation_tag` (16 B) + raw payload
  (envelope junk dropped). One key per hash; reads use a CONSTANT offset (`blob_header_len`, now 16) and never
  consult the meta. The `incarnation_tag` varies the body's etag per incarnation — this is the LOAD-BEARING
  field: `resurrect` overwrites the body with a fresh tag → distinct etag → GC's exact-token delete misses.
- **Meta**: `blobs/xx/<hash>.meta` (tiny fixed codec, `ca-inspect` support required):
  `{version, state (clean|condemned), condemn_round (u64), size (u64)}` — a **2-state FRESHNESS MARKER**, not
  the linearizer, and not on the read path. GC sets it `condemned` at condemn, clears it at spare, deletes it
  at delete. (No `tombstone` state, no `incarnation` nonce — the meta is advisory; the body's etag is the
  conditional authority.)

> **Delete-safety is the BODY's exact-token delete (the unchanged `CaIncarnationCore` core), NOT the meta.**
> GC captures the body token at condemn and `DELETE If-Match token`; a resurrect displaces the body (fresh
> tag → new etag) so the exact-token delete misses. The meta only answers "is the current incarnation
> condemned?" for the writer's dedup-adoption gate. A momentarily-stale meta is safe: it can only make a
> writer resurrect **conservatively** (re-upload when it could have adopted) — never adopt a doomed body,
> because a strongly-consistent GET (S3 RAW consistency, a pool requirement) reflects GC's latest condemn.
> Ordering: create body-then-meta, delete body-then-meta, so a `clean` meta never outlives its body in a way
> that misleads the gate.

### Protocols {#meta-protocols}

(v3. The BODY's in-body `incarnation_tag` varies its etag; exact-token BODY delete is the safety
linearization — the unchanged `CaIncarnationCore` core. The META is a 2-state freshness marker for the
writer's dedup-adoption gate, replacing the retire-view download. No tombstone; GC must write the meta
`condemned` before it can delete, so an absent meta means "not condemned".)

| Operation | Protocol |
|---|---|
| **Fresh upload** (`putBlob`, cache-miss) | PUT body `{fresh incarnation_tag + payload}` (If-None-Match). Success ⇒ PUT meta If-None-Match `{clean, size}` (a 412 = racing writer created it ⇒ fine). Body 412 ⇒ dedup path ↓ |
| **Dedup-adopt** (`putBlob`, cache-hit or body-412) | **point-read the meta** (condemned?). `clean` or **absent** ⇒ adopt (body present, not condemned — record the token; if the meta was absent, best-effort PUT meta If-None-Match `{clean}` to help future readers). `condemned` ⇒ resurrect ↓. (Safe: GC always writes the meta `condemned` before deleting, so a non-condemned meta ⇒ the body is not mid-delete; EDGE-BEFORE-OBSERVE + exact-token delete carry the rest.) |
| **Resurrect** (writer, condemned) | overwrite the body from the writer's OWN source with a FRESH `incarnation_tag` (INV-1 — never GET the dying body) → distinct etag; then CAS/PUT meta `{clean}`. GC's exact-token delete of the OLD token now misses (TokenMismatch). **No wait.** |
| **Copy-forward** (tokenless leaf, no source, committed-source provenance) | condemned ⇒ GET the still-present condemned body (the documented INV-1 exception), verify payload hash, re-wrap with a fresh `incarnation_tag`, `putOverwrite`; then CAS/PUT meta `{clean}`. (Unchanged from the current `copyForwardFromCondemned`.) |
| **Condemn** (GC, at fold `d=0`) | HEAD body → capture the exact token into the retired ledger (unchanged), AND PUT/CAS meta `{condemned, condemn_round}` (the writer's freshness signal) |
| **Spare** (GC, `d>0` recovered) | CAS meta condemned→`{clean}` |
| **Delete** (GC, graduated; two-phase pacing kept) | `DELETE body If-Match <condemn-time token>` (unchanged exact-token delete — a resurrect displaced the token ⇒ TokenMismatch ⇒ survives), then delete the meta. **No tombstone, no handshake, no cross-object atomicity, no writer wait.** |
| **Reads** | GET body by content key at the constant `blob_header_len` offset — meta never consulted (unchanged) |

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

**Gate B — v3 (2026-07-10): the safety core is the UNCHANGED `CaIncarnationCore` (exact-token BODY delete +
retire-view/W-REVALIDATE), already TLA+ + Apalache-inductive + one-month-soaked.** v3 keeps one-key-per-hash
+ in-body `incarnation_tag` + exact-token delete, so NO new delete-safety model is required. The ONLY new
mechanism is the freshness meta replacing the retire-view as the K1 dedup-adoption signal; its obligation is
narrow — *the meta point-read is at least as fresh, for the K1 gate, as the retire-view it replaces* (GC writes
the meta `condemned` before any delete; a strongly-consistent GET observes it) — to be discharged by a small
focused model or a written argument, NOT a re-derivation of the delete core. **The v2 model below
(`CaMetaDescriptorRaw.tla`, terminal tombstone) and `CaMetaIncarnationKey.tla` (per-incarnation keys) are
SUPERSEDED/REJECTED — retained only as explored-and-rejected evidence.**

**REJECTED v2 model — RE-RUN GREEN (`CaMetaDescriptorRaw.tla`, 2026-07-10), superseded by v3:**

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
