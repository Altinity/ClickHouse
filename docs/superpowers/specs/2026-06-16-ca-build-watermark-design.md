# CA build liveness as a per-server watermark (B167, heartbeat redesign)

**Status:** design, awaiting review · **Date:** 2026-06-16 · **Branch:** `cas-mergetree-poc`
**Backlog:** B167. Supersedes the per-build heartbeat guard (the reverted Part B, `affcde65375`). Part A (`resurrect` re-stamps the current build's identity, `16a12eee416`) is **kept** and generalized. This is a **liveness/robustness** fix; protocol safety (`INV-NO-LOSS`/`NO-DANGLE`/`NO-RETURN`) is intact and unchanged.

## Problem recap

The incremental GC condemns a blob once `present ∧ everEdged ∧ InDeg=0`. A blob can be referenced → dropped → **condemned**, then a new merge **dedup-hits** the same content and must resurrect it. Under productive GC (B160) + dedup churn, the build's own fresh incarnation is itself `everEdged ∧ InDeg=0` until publish, so GC can re-condemn and delete it in the upload→publish span. The bodyless publish-gate path then starves → broken detached parts (soak #11). It is a **liveness** failure: no data loss (the active table is intact), the build never converges.

The first fix attempt (B167 Part B) gated condemnation on the **existence** of a per-build heartbeat object `builds/<build_id>`. That proved **fragile**: a successful `publish()` does not discard its heartbeat (only `abandon()` does), so a completed build's heartbeat lingers and the guard treats its later-dropped blobs as in-flight forever — empirically breaking `CasGcRetire.DeletedCandidateDoesNotReappear`. Relying on every publish to sweep up its own marker is the same class of bug we already had (only `abandon` cleaned up), moved to the success path.

## Core idea — liveness is a per-server watermark, not a per-build object

Replace the per-build heartbeat with **one durable object per server** that advances a **watermark** over that server's in-flight builds. Build completion is reflected *automatically* by the watermark moving — there is no per-build object to leak, so nothing to forget to sweep. Durable state is **O(servers)**, not O(builds).

### Identity: the triple `(server_id, epoch, build_seq)`

The random-u128 `build_id` dissolves into a triple that is globally unique by construction:

- **`server_id`** (u128) — which server's watermark governs this object. Already present in the `Provenance` TLV.
- **`epoch`** (u64) — a fresh **random** value chosen at process start. GC never orders epochs; it only asks "is this the server's *current* epoch?". A blob carrying a stale epoch belongs to a dead incarnation. No durable counter, no restart coordination.
- **`build_seq`** (u64) — assigned from a strictly-increasing in-memory counter per `(server, epoch)`; may reset to 0 on restart (it is only ever compared within a matching epoch). **Strict monotonicity is load-bearing, not merely convenient** (TLA+ `CaBuildWatermarkNum`, `_nonmonotonic`): because `min_active` is a single scalar floor, a build number that was unique but issued *out of order* (a lower number starting after a higher one finished) would pull `min_active` back down and re-protect a finished build's condemned blob (a leak). A counter — never reused, never out-of-order — makes `min_active` monotone non-decreasing and forbids that.

The triple is written into **S3 user metadata** (`x-amz-meta-*`) on every object write, so GC reads the owner from the **HEAD it already performs**, with no body read. This is the only load-bearing copy. (Implementation note: the envelope `build_id` field is left UNCHANGED — it still carries the random per-build id the deferred full-GC debris path reads; a forensic `epoch‖build_seq` body copy was considered but dropped to avoid disturbing that path. fsck recovers the triple from metadata.) No envelope format-version change.

The **ETag remains the token** (`TokenType::ETag`), unchanged: `deleteExact` stays an `If-Match` conditional delete, and token displacement on re-stamp is still driven by a fresh in-body `incarnation_tag` (a body-sensitive ETag). The triple (liveness) and the ETag (delete fence) are two independent layers.

### The watermark object `servers/<server_id>`

Strict JSON (per the §4 encoding split), recreatable-if-absent like `gc/hb`:

```text
{ "format":"cas_server_watermark", "version":1,
  "epoch": <u64>,          // this process's current epoch
  "min_active": <u64>,     // oldest still-in-flight build_seq; UINT64_MAX once retired
  "seq": <u64> }           // liveness counter, bumped every renewal
```

- **`min_active`** is the floor. The GC guard protects a candidate iff `epoch == header.epoch ∧ header.build_seq ≥ min_active` for a **live** server. `min_active` only ever advances upward as builds finish.
- **`seq`** is the liveness signal for crashed-server detection (below). No wall clock is ever read.
- **No `max_active`.** It never appears in the guard; the async-renewal lag is safe-by-direction (a stale, published `min_active` lags *low* → over-protects → only delays reclaim, never deletes a live blob).

The server keeps its **active-build set in memory** (the live `Build` objects). On build start, allocate the next `build_seq` and insert it; on publish/abandon/destruction, remove it. `min_active` = the minimum of the in-memory set (or the next-to-be-issued `build_seq` when the set is empty).

### Durability ordering

- **Startup anchor (synchronous, once per process).** Before the first object PUT, durably write `servers/<server_id>` with the fresh `epoch` and the initial `min_active`. This preserves the invariant *"a watermark with epoch E is durable before any object carrying epoch E exists."* (Mirrors today's `W-HEARTBEAT` durable-before-first-PUT, but once per process, not per build.)
- **Renewal (async, ~2 s, off the write path).** A background thread bumps `seq` and republishes `min_active`. Never on the hot write path.
- **Farewell (graceful shutdown).** Flush a final watermark with `min_active = UINT64_MAX`, retiring the epoch: no `build_seq` clears the floor, so GC may reclaim all of that epoch's `everEdged ∧ InDeg=0` blobs immediately — no staleness wait. A crash skips the farewell and falls to the detection below.

### GC condemn guard

At the R2 observe step (`Gc::retire`), for each candidate:

```text
condemn ⟺  present ∧ everEdged ∧ InDeg=0 ∧ ¬protectedByLiveBuild
protectedByLiveBuild ⟺  serverLive(server_id)
                        ∧ watermark.epoch == header.epoch
                        ∧ header.build_seq ≥ watermark.min_active
```

GC reads `(server_id, epoch, build_seq)` from the candidate's HEAD metadata (no body read) and the server's watermark **once per server per round** (cached). A protected candidate is **skipped this round** (non-destructive deferral) — unlike full-GC debris reclaim, no delete is at stake, so a skip is always fail-safe.

A candidate **lacking the triple metadata** (an object written before this change) is treated as **unprotected** — the pre-watermark behavior, and safe because the publish gate still backstops any in-flight reference. The PoC pool is recreated per soak, so mixed-version objects do not arise in practice; this is the conservative default, not a migration path.

### Crashed-server detection — O(servers), clock-free

A crashed server leaves a finite `min_active` and a frozen `seq`, so its in-flight blobs stay protected. GC reclaims them by the **B160 mechanism**: sample each referenced server's `seq` at the start and end of its observation window; `seq` unchanged across **K = 2 consecutive GC passes** ⇒ presumed not-renewing ⇒ `serverLive = false` ⇒ its blobs become condemnable. No wall clock; identical in spirit to the `gc/state` lease-steal. A crashed *build* on a *live* server needs no staleness judgment — `min_active` simply advances past it. **Server restart** is handled by `epoch`: a stale-epoch blob fails `watermark.epoch == header.epoch` and is immediately condemnable.

The verdict is **local to the pass and self-correcting**: GC does not overwrite the server's watermark. If the server was alive and renews, the next pass sees `seq` advance and re-protects. The residual — GC condemned a blob a falsely-dead-but-alive build still needs — is caught by the **publish gate** (next section): a false positive is a liveness hiccup (abort + retry), never data loss.

### Writer side — re-stamp with the body in hand (Part A, generalized)

The writer relies on **one** re-stamp, always with the body in hand, on the **write path**:

- **Dedup-hit on a `condemned` incarnation** (`putBlob` / `putTree` 412 → `retireView` hit): re-stream the caller's own bytes into a **fresh incarnation** (`W-FRESH-TAG`: fresh `incarnation_tag` → fresh ETag → token displaced) stamped with **this build's triple** in metadata. The `BlobSource` is re-invokable, so no GET-from-existing and no retention. After this, the blob is owned by this live build → the GC guard protects it until publish.
- **Dedup-hit on an `uncondemned` incarnation**: adopt it free (record the dep; no re-upload — dedup's write-saving is preserved). This is the dominant case.

There is **no bodyless re-stamp and no server-side copy.** The publish gate (`gateCheckDeps`/`revalidateDeps`) only **verifies** deps are present and uncondemned; for a live, protected build its resurrect branches are unreachable (the watermark kept GC off the deps). If the gate *does* find a dep condemned/absent — only possible if this build's protection lapsed (e.g. a wedged renewal thread that also fails the gate's own local-heartbeat sanity) — the build **aborts retryably**; the retry re-produces the body via `putBlob` (body in hand). This is a bounded abort-and-retry, never a livelock.

## Cross-server reuse of in-build objects

Only one dedup class is dangerous; the writer keys its decision on what it can **observe** (it cannot compute `InDeg` — that is GC's global property):

- **Hit on a live, published blob** (`InDeg ≥ 1`) — protected by its committed ref; the watermark is irrelevant; adopt free. Dominant case.
- **Hit on a never-published in-build blob** — not `everEdged`; GC never touches it; adopt free.
- **Hit on a `condemned` (hence `everEdged`, `InDeg=0`) blob** — the only dangerous case. The writer re-stamps it to **its own triple** with its own body (above), transferring protection to its own watermark.

Concurrent in-flight referrers race on the single header: **last-writer-wins**. The loser's protection lapses, its gate finds the dep at risk, it aborts and re-stamps on retry (body in hand). The **publish gate is the correctness backstop**, exactly as in the original protocol; the watermark only makes "my own deps survive to publish" reliable, killing the livelock. A genuinely tokenless adopt of a condemned blob with no body anywhere fails closed, loudly (unchanged).

## Backend capability and portability (verified)

The design needs: user metadata settable on PUT and returned on HEAD; exact-token conditional delete (already required, `INV-NO-RETURN`). It needs **no** server-side copy. Probed `rustfs/rustfs:1.0.0-beta.8` (our CI target) directly on 2026-06-16:

| Mechanic | RustFS beta.8 | AWS S3 | MinIO | Ceph RGW |
|---|---|---|---|---|
| `x-amz-meta-*` set on PUT, returned on HEAD | ✅ verified | ✅ | ✅ | ✅ |
| `DeleteObject If-Match` (exact-token delete) | ✅ verified | ✅ (GA Sep 2025) | OSS fail-closed (spec §2) | not documented (fail-closed) |
| Fresh `incarnation_tag` ⇒ fresh ETag (body-sensitive token) | ✅ (PUT path) | ✅ | ✅ | ✅ |

User metadata is the most universally supported part of the S3 surface (returned on HEAD; ~2 KB budget vs our ~40-byte triple; names come back lowercased — read case-insensitively; US-ASCII values — encode as hex/decimal). The exact-token delete is the pre-existing `_pool_meta` capability probe; backends failing it already refuse `metadata_type = content_addressed`. **No new capability is required beyond what the protocol already probes.**

(The earlier exploration of a server-side multipart-copy re-stamp — to move the ETag over identical bytes via a changed part split — is **dropped**: the watermark makes bodyless re-stamp unnecessary, so the design does not depend on `UploadPartCopy` / `MetadataDirective=REPLACE`, where MinIO has known gaps.)

## Safety

Unchanged for `INV-NO-LOSS`/`NO-DANGLE`/`NO-RETURN`:

- Re-stamp uses `W-FRESH-TAG`: the condemned token is never reused; the fresh incarnation carries a new ETag GC never condemned. `INV-NO-RETURN` holds.
- The GC guard only ever **skips** a deletion (never deletes more): a watermark-protected blob is spared; when the owner finishes (`min_active` advances), retires (farewell / epoch change), or is detected dead (frozen `seq`), GC proceeds. Worst case is a transient space leak, never a correctness violation (`INV-OVER-COUNT-ONLY` preserved).
- The publish gate remains the correctness mechanism for the residual false-positive-death window: a wrongly-deleted in-flight dep is in the build's dep-set, so the gate's HEAD finds it absent and the build aborts retryably rather than committing a dangling ref.

This **revises** the §5 protocol-spec line *"the publish gate, not the heartbeat, is the safety mechanism"* and the `CasHeartbeat.h` comment: the per-server watermark becomes a co-liveness mechanism for incremental GC, while the publish gate stays the safety backstop.

## TLA+ (`CaBuildWatermark`, adapt `CaResurrectLiveness`)

The rewritten `CaResurrectLiveness` proved the abstract heartbeat guard is load-bearing (guard ON → `<>published` holds; guard OFF → starvable lasso). Adapt it to the concrete watermark oracle:

- Model `min_active` advancing as builds finish, `epoch`, and `serverLive` (frozen-`seq` detection).
- Guard becomes `epoch == watermark.epoch ∧ build_seq ≥ min_active ∧ serverLive` instead of the abstract boolean.
- Add the **cross-build re-stamp / last-writer-wins** transition and check `<>published` still holds (the loser aborts-and-re-stamps; the publish gate backstops the false-death residual).
- Sabotage controls: (a) guard off (no watermark) ⇒ starvable lasso (the existing B167 counterexample); (b) `min_active` advancing past a *still-active* build ⇒ premature condemn (must be a modeling error, i.e. cannot happen when the active set is correct) — confirms the in-memory active-set discipline; (c) crashed-server detection with K=1 ⇒ false reclaim of a slow-but-alive server (motivates K=2), mirroring `CaGcLeaseCore`'s `SabotageNoHeartbeat`.

## Testing (TDD)

- **Unit (`gtest_cas_*`, `CasInMemoryBackend` extended with user metadata):**
  - watermark codec round-trip; `min_active` = min of the in-memory active set; advances on build completion.
  - GC guard: a candidate whose `(epoch, build_seq)` is `≥ min_active` of a live server is **skipped**; one below `min_active`, or with a stale epoch, or of a frozen-`seq` server, is **condemned**.
  - `DeletedCandidateDoesNotReappear` and the full `CasGcRetire.*`/`CasGcRecheck.*` suite stay green (the lingering-heartbeat fragility is gone — there is no per-build object).
  - Part A: a dedup-hit on a condemned blob re-streams a fresh incarnation stamped with this build's triple (no GET-from-existing).
  - Convergence: with a GC that exact-token-deletes any *unprotected* condemned blob, the publish succeeds in bounded steps (was: livelock).
  - dedup-hit on a live/uncondemned blob still adopts free (no re-upload).
  - crashed-server: frozen `seq` across K=2 passes ⇒ its blobs become condemnable; a renewing server's blobs stay protected.
- **No regression:** full `Cas*`/`CaWiring*` suite green (only the pre-existing B140 leak red).
- **Soak (with B160):** rebuild the two-replica soak; confirm **0 broken detached parts** and a clean fsck (`dangling=0`) under productive GC.

## Cost

- GC: **+1 HEAD-metadata read is free** (the R2 observe HEAD already happens; it now also returns the triple). **+1 watermark GET per server per round** (cached), independent of candidate count. No per-candidate body read (the dropped B167-Part-B ranged-GET cost is avoided). Ties into B157.
- Writers: **+1 small async PUT per ~2 s per server** (watermark renewal), off the write path; **+1 synchronous PUT per process** (startup anchor) and **+1 on graceful shutdown** (farewell). Re-stamp re-uploads bytes only on the condemned-dedup edge (unchanged from Part A).

## Out of scope

- Server-side copy / retag-without-rewrite (`UploadPartCopy`, `MetadataDirective=REPLACE`) — dropped; the watermark removes the bodyless re-stamp it would have served.
- Envelope format-version change — none; the triple lives in S3 metadata (+ a forensic copy in the existing `build_id` field).
- B163 (prefix-sharded parallel GC) — separate milestone.
- A wedged-but-alive build watchdog beyond the gate's local-heartbeat sanity — backlog.

## Sequencing

Ships **with** B160 (which makes GC productive and exposes B167). Merging B160 without this creates broken parts under dedup + GC load.

## Implementation notes {#implementation-notes}

Two facts discovered during implementation, both forced by the strict-JSON codec's integer interop bound (it caps integers at 2^53):

- The `min_active = UINT64_MAX` retirement sentinel (the graceful-shutdown farewell) is wire-encoded as the JSON string `"retired"` rather than a numeric literal, and decoded back to `UINT64_MAX`. A raw `UINT64_MAX` would exceed the codec's 2^53 integer cap.
- `epoch` is masked to 52 bits (not a full u64) so it stays within the same 2^53 codec interop bound. This is still collision-safe for an equality-only token: GC checks `epoch` for equality, never ordering, so 52 bits of randomness per process start suffice.
