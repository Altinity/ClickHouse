# CA resurrect = re-stream a fresh incarnation, protected by the build heartbeat (B167)

**Status:** design, revised after TLA+ recheck · **Date:** 2026-06-16 · **Branch:** `cas-mergetree-poc`
**Backlog:** B167. Found by the B160 soak validation (soak #11). This is a **liveness/robustness** fix, not a safety fix — the protocol safety (INV-NO-LOSS/NO-DANGLE/NO-RETURN) is intact and was confirmed (no data loss). The rejected alternative "C" (a GC→build back-link / reservation with new GC states) is explicitly out of scope.

> **Revision note (the falsified premise).** An earlier draft of this spec claimed the fix was *writer-side only, no GC change*, on the grounds that *"a fresh incarnation (new tag, never-everEdged) cannot be condemned by GC."* That is **wrong**: `everEdged` is a property of the `(kind, hash)`, not of the incarnation tag. The condemned blob is `everEdged` (it was published once, then dropped — that is *why* it became condemned), and it stays `InDeg=0` until this build publishes. So GC's condemn guard `present ∧ everEdged ∧ InDeg=0` **is** satisfied for the build's own fresh incarnation; GC can re-condemn and delete it in the upload→publish span. The TLA+ liveness model, once it stopped collapsing upload+publish into one atomic step, made this concrete (see TLA+ section). The corrected fix has **two** parts: a writer-side re-stream (no retention, no GET race) **and** an incremental-GC change (honor the build heartbeat) that protects the fresh incarnation until publish.

## Problem

The incremental GC condemns a blob once it is zero-in-degree (`present ∧ everEdged ∧ InDeg=0`). A blob can be referenced → dropped → **condemned**, and then a new merge/mutation **dedup-hits** the same content (conditional PUT → 412 "exists") and must **resurrect** that condemned incarnation. `Build::resurrect` (`CasBuild.cpp:276`) re-uploads under a fresh incarnation tag, BUT it **sources the bytes by GET-ing the existing condemned object** (`body = new_head + got->bytes.substr(header_len)`) — not from a caller-held body. So:

- If GC's exact-token delete lands in the `HEAD→GET` window, the GET returns nothing → `ABORTED "deleted by GC between HEAD and GET; retry"`.
- `putBlob` (body in hand) catches that and re-uploads its held body (B136/B137, bounded 8). But the **publish-gate dep-revalidation** (`revalidateDeps`/`gateCheckDeps`) is **bodyless** — it only has the recorded `DepEntry{token,round,size}`, not the source — so it propagates the `ABORTED`, the merge retries, and under **productive GC (B160) + the RustFS-412 dedup churn** it never converges → the merge produces a **broken part** (detached `broken_*`) referencing GC-deleted blobs.

Soak #11 evidence: part `20260616_2881_3917_13`, blobs `2b/2bfab…`,`d1/d144…`; 29 broken detached parts; `resurrect…deleted by GC` aborts. No data loss (active table intact; broken parts are failed merges whose source data stays in the active parts) — this is a **convergence (liveness)** failure, which a safety model does not check.

## Root cause (two layers)

1. **The bytes are sourced racily.** `resurrect` re-derives bytes by **reading the existing object (GET-from-existing)**, so it depends on that object surviving the `HEAD→GET` window — and the bodyless gate path has no body to fall back on when GC wins.
2. **A fresh incarnation is NOT GC-safe by construction.** Even once the writer re-streams its own bytes into a fresh incarnation, the `(kind,hash)` is `everEdged` and `InDeg=0` until publish, so GC can re-condemn and delete that fresh incarnation before the publish references it. Re-streaming closes the *race* but not the *window*; nothing yet keeps GC off the in-flight blob.

## Fix — two parts

### Part A — writer-side: re-stream a fresh incarnation from the caller's own bytes (no GET, no retention)

The writer **always has the body** at `putBlob` time: the `BlobSource` is re-invokable (the retry loop already calls `write_payload` up to 8 times). So on a dedup-hit on a **condemned** incarnation, instead of GET-from-existing or buffering the body in RAM (rejected — column blobs can be gigabytes), re-stream the source into a **fresh incarnation stamped with this build's own `build_id`**:

- conditional PUT `If-None-Match:*` (W-FRESH-TAG, fresh `incarnation_tag` + this build's `build_id` in the envelope header);
- on 412 (the condemned object is still there) → `putOverwrite If-Match(observed condemned token)` with the re-streamed body + the fresh header; if that 412s (GC deleted it first) → loop back to the `If-None-Match` create.

This is the user's "HEAD-first, then start the new PUT and stream the body" shape, done where the body is in hand. No body is retained between `putBlob` and publish; the source is simply re-invoked. The recorded `DepEntry` token now points at an incarnation that carries the live build's `build_id`.

### Part B — incremental GC honors the build heartbeat

Add a fourth condemn guard to the incremental GC's R2 observe step (`Gc::retire`, `CasGc.cpp:626-644`):

```
condemn ⟺  present ∧ everEdged ∧ InDeg=0 ∧ ¬liveBuild(build_id)
```

At observe time GC reads the candidate blob's envelope `build_id` (already stored, `CasEnvelope.h:54`) and **skips condemning** any blob whose `builds/<build_id>` heartbeat is live (`HeartbeatKeeper`/`Heartbeat`, `CasHeartbeat.h`). Combined with Part A, the build's fresh incarnation carries its own live `build_id`, so GC will not condemn it until the build publishes (→ `InDeg≥1`, no longer a candidate) or the build dies (heartbeat expires → it becomes debris reclaimed by full GC, which already heartbeat-gates).

**This is not "C".** No reverse link, no new GC state: GC reads the blob's *own* header `build_id` and an *existing* heartbeat key — exactly the data full GC already consults for debris. It is the user's original intuition ("GC shouldn't touch in-flight things while heartbeats are happening"), now extended from full GC to incremental GC.

**This revises one line of §5 of the protocol spec** — *"the publish gate, not the heartbeat, is the safety mechanism"* — and the `CasHeartbeat.h:17-20` comment that says heartbeats gate only full-GC debris. After this change the build heartbeat is a **co-safety/co-liveness mechanism for incremental GC too**. Both the spec line and the header comment are updated in the plan.

Dedup is preserved for the common case: a dedup hit on a **live** existing blob still adopts it for free (no re-stream). Part A's re-stream happens ONLY on the dedup-hit-on-a-**condemned**-incarnation edge — the racy case — trading one duplicate write for guaranteed convergence.

## Cost

Reading `build_id` needs the envelope header bytes, so a condemnation candidate's R2 observe becomes a small **ranged GET** of the header instead of a bare HEAD. Bounded — only for candidates GC is about to condemn, not every object — and far cheaper than streaming gigabytes twice (the retention alternative). Ties into B157 (op-count); call it out in the op-count budget.

## Safety

Unchanged for INV-NO-LOSS/NO-DANGLE/NO-RETURN:
- Part A re-uses W-FRESH-TAG: the condemned token is never reused; the fresh incarnation carries a new token GC never condemned. INV-NO-RETURN holds.
- Part B only ever **skips** a deletion (never deletes more): a heartbeat-live blob is spared; when the heartbeat dies GC proceeds. No loss. The bound on a wedged/hung build holding a heartbeat forever is the heartbeat TTL + `abandon` — the same property full GC already has for debris. Worst case is a transient space leak, never a correctness violation.

## TLA+ (`CaResurrectLiveness`, the missing liveness dimension)

The safety-only `CaIncarnationCore` did not catch B167 (it proves safety, models the body-in-hand writer, and does not check liveness). `CaResurrectLiveness` adds liveness (`Liveness == <>published`, weak fairness on the build's own actions, GC unconstrained).

The model was **rewritten** after this spec's revision: its first version collapsed upload+publish into one atomic step (encoding the falsified "fresh ⇒ untouchable" premise) and so trivially "proved" convergence. The corrected model makes upload→publish **non-atomic**, lets GC act in the gap, and makes the **heartbeat guard** the checked constant:

- `HeartbeatGuard = TRUE` (Parts A+B) → `<>published` **HOLDS** (4 states): once the build mints its fresh incarnation, the guard blocks `GcCondemn` (and `GcDelete` needs `condemned`), so `present ∧ freshOwned ∧ ¬condemned` is stable → weak fairness forces publish.
- `HeartbeatGuard = FALSE` (Part A only, today's incremental GC) → **VIOLATED** (7 states), lasso `GcDelete → BuildUpload(freshOwned) → GcCondemn(freshOwned!) → GcDelete → …`. Crucially this is the **body-in-hand** writer, proving Part A alone is starvable and Part B is load-bearing.

Results: `docs/superpowers/models/CaResurrectLiveness_RESULTS.md`.

## Testing (TDD)

- **Unit (`gtest_cas_build.cpp` / `gtest_cas_protocol_scenarios.cpp`):** with `CasInMemoryBackend` + a GC stub:
  - (a) **Part A:** a dedup-hit on a condemned blob re-streams a fresh incarnation from the held source (no GET-from-existing); the recorded dep token carries this build's `build_id`.
  - (b) **Part B:** the incremental GC stub does NOT condemn a candidate whose `build_id` heartbeat is live; it DOES condemn once the heartbeat is gone.
  - (c) **Convergence:** with a GC that exact-token-deletes a condemned blob whenever it is *not* heartbeat-protected, the publish **succeeds in bounded steps** (was: livelock/exhaust on the old bodyless path).
  - (d) a dedup hit on a **live** blob still adopts free (no re-stream — dedup preserved).
- **No regression:** full `Cas*`/`CaWiring*` suite green (only the pre-existing B140 leak red).
- **Soak (with B160):** rebuild and run the two-replica soak; confirm **0 broken detached parts** and a clean fsck (`dangling=0`) under productive GC — i.e. B160+B167 together.

## Risks

1. **Source lifetime / re-invocability (Part A).** The `BlobSource` must be re-invokable at the condemned-dedup edge (the `putBlob` comment already states it is). Bound memory — re-read from the local source, never buffer the whole part in RAM (ties into B165).
2. **Extra duplicate write on the condemned-dedup edge.** Acceptable — the rare racy case; the common live-dedup case is unchanged.
3. **Header read at GC observe (Part B).** One ranged GET per condemnation candidate (op-count, B157). Bounded; see Cost.
4. **Wedged build holding a heartbeat (Part B).** Bounded by heartbeat TTL + `abandon`; worst case a transient space leak, never a correctness issue (same as full-GC debris today).

## Out of scope
- "C" (a GC→build back-link / reservation with new GC states) — rejected.
- Body retention in RAM (the `retained_blobs` draft) — rejected: column blobs can be gigabytes.
- Tree recreate path — already correct (`recreateTree` retains the small encoded tree payload).

## Sequencing
B167 ships **with** B160: B160 makes the GC productive, which is what exposes B167; merging B160 alone would create broken parts under dedup+GC load.
