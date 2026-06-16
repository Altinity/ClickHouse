# CA resurrect = always re-upload a fresh incarnation (B167)

**Status:** design, awaiting review · **Date:** 2026-06-16 · **Branch:** `cas-mergetree-poc`
**Backlog:** B167. Found by the B160 soak validation (soak #11). This is a **liveness/robustness** fix, not a safety fix — the protocol safety (INV-NO-LOSS/NO-DANGLE/NO-RETURN) is intact and was confirmed (no data loss). The rejected alternative "C" (GC-side ordering/reservation) is explicitly out of scope (adds a GC→build back-link and new states).

## Problem

The incremental GC condemns a blob once it is zero-in-degree (`present ∧ everEdged ∧ InDeg=0`). A blob can be referenced → dropped → **condemned**, and then a new merge/mutation **dedup-hits** the same content (conditional PUT → 412 "exists") and must **resurrect** that condemned incarnation. `Build::resurrect` (`CasBuild.cpp:276`) re-uploads under a fresh incarnation tag, BUT it **sources the bytes by GET-ing the existing condemned object** (`body = new_head + got->bytes.substr(header_len)`) — not from a caller-held body. So:

- If GC's exact-token delete lands in the `HEAD→GET` window, the GET returns nothing → `ABORTED "deleted by GC between HEAD and GET; retry"`.
- `putBlob` (body in hand) catches that and re-uploads its held body (B136/B137, bounded 8). But the **publish-gate dep-revalidation** (`revalidateDeps`/`gateCheckDeps`) is **bodyless** — it only has the recorded `DepEntry{token,round,size}`, not the source — so it propagates the `ABORTED`, the merge retries, and under **productive GC (B160) + the RustFS-412 dedup churn** it never converges → the merge produces a **broken part** (detached `broken_*`) referencing GC-deleted blobs.

Soak #11 evidence: part `20260616_2881_3917_13`, blobs `2b/2bfab…`,`d1/d144…`; 29 broken detached parts; `resurrect…deleted by GC` aborts. No data loss (active table intact; broken parts are failed merges whose source data stays in the active parts) — this is a **convergence (liveness)** failure of the bodyless gate path, which a safety model does not check.

## Root cause (the one-liner)

`resurrect` re-derives bytes by **reading the existing object (GET-from-existing)**, so it depends on that object surviving the `HEAD→GET` window — and the gate path has no body to fall back on when GC wins. Resurrection should be **a re-upload of the caller's own (recreatable) bytes**, which the writer always has (it produced them, or read them from a local source part), never a GET of the racy existing object.

## Fix — writer-side only, no GC change

**Principle: a resurrect/revalidation of a RECREATABLE dependency always re-uploads a FRESH incarnation from the caller's own bytes; it never GET-from-existing.** A fresh incarnation (new tag, **never-everEdged**) cannot be condemned by GC's retire guard, so the re-upload is GC-safe by construction and converges. This is exactly the "bytes in hand → re-PUT" arm the TLA+ model already proves safe — `putBlob` has it; we extend it to the gate.

1. **Thread recreatability through `deps`.** `DepEntry` carries a way to re-produce the dependency's bytes (a re-invokable `BlobSource`/tree handle, or a `body_recreatable` flag + the local source coordinates). `putBlob`/`putTree` record a recreatable source; `reuseBlob`/adopt record recreatability per B156b's existing `body_recreatable` notion.
2. **Resurrect = re-upload for recreatable deps.** When a dep is condemned (at the gate's revalidation, or at `observeAndAdmit`'s condemned branch), if it is recreatable, **re-upload a fresh incarnation from the caller's bytes** (re-invoke the source) under a new tag and record the new token — never GET-from-existing. The GET-from-existing path is removed for recreatable deps, so the `HEAD→GET` race cannot occur there.
3. **Residual genuinely-bodyless adopt.** The only case without caller bytes is a tokenless `adoptFromTree` dep on a committed blob. Such a source is normally **live** (its part is active → in-degree>0 → not condemned) → no resurrect needed. If it is condemned (a rare concurrent drop), its bytes are still **local** (the merge/mutation reads the source part locally) → re-read locally → recreatable → re-upload. So this collapses into the recreatable path; **no GC-side reservation/ordering is introduced.**
4. **GC unchanged.** No back-link, no new states, no heartbeat dependency for the incremental GC. The convergence comes entirely from the writer re-uploading a fresh (GC-safe) incarnation.

Dedup is preserved for the common case: a dedup hit on a **live** existing blob still adopts it for free (no resurrect, no re-upload). The re-upload happens ONLY on the dedup-hit-on-a-**condemned**-incarnation edge — the racy case — trading one duplicate write for guaranteed convergence.

## Safety

Unchanged. Re-uploading a fresh incarnation is the model's existing safe arm (W-FRESH-TAG: the condemned token is never reused, INV-NO-RETURN holds; the new incarnation is never-everEdged so GC won't condemn it before the publish references it). No GC behavior changes, so no-loss/no-dangle is preserved by the existing argument.

## TLA+ (extends `CaIncarnationCore`, the missing liveness dimension)

The current model proves SAFETY and the body-in-hand re-upload converges; it did NOT catch B167 because (a) it models the body-in-hand writer, not a **bodyless** dep-revalidation that gives up, and (b) it checks safety, not **liveness**. Add:
- A **caller mode** per dep: `recreatable` vs `bodyless`, and a publish-gate revalidation step that, on a condemned dep, either re-uploads (recreatable) or propagates ABORTED (bodyless).
- A **liveness property** under weak fairness: a build that keeps retrying **eventually publishes** (`<>Published`).
- Result to demonstrate: with a `bodyless` gate (today) + an adversarial GC that always wins the `HEAD→GET` race, `<>Published` is **violated** (the B167 livelock, reproduced); with the `recreatable` re-upload, `<>Published` **holds** (fresh incarnation is non-everEdged → GC can't condemn it → converges). Safety invariants stay green in both.

## Testing (TDD)

- **Unit (`gtest_cas_build.cpp` / `gtest_cas_protocol_scenarios.cpp`):** with `CasInMemoryBackend` + a GC stub that exact-token-deletes a condemned blob on every HEAD→GET window: (a) reproduce the bodyless-gate livelock (publish never succeeds / exhausts) on the OLD path; (b) with the fix, the gate re-uploads a fresh incarnation and the publish **succeeds in bounded steps**; (c) a dedup hit on a **live** blob still adopts free (no re-upload — dedup preserved); (d) the residual tokenless-adopt-of-condemned re-reads local and converges.
- **No regression:** full `Cas*`/`CaWiring*` suite green (only the pre-existing B140 leak red).
- **Soak (with B160):** rebuild and run the two-replica soak; confirm **0 broken detached parts** and a clean fsck (`dangling=0`) under productive GC — i.e. B160+B167 together.

## Risks

1. **Source lifetime in `deps`.** The recreatable source/handle must stay valid until publish. For a merge/mutation the bytes are the local source parts (retained until the part commits); for an INSERT it is the input block. Verify the source is re-invokable at gate time (the `putBlob` comment already states the `BlobSource` is re-invokable) and bound memory (re-read local, don't buffer the whole part in RAM — ties into the B165 write-buffer footprint note).
2. **Extra duplicate write on the condemned-dedup edge.** Acceptable — it is the rare racy case, and it is what guarantees convergence; the common live-dedup case is unchanged.
3. **Residual adopt correctness.** Confirm a condemned tokenless-adopt source's bytes are always locally available in the write path; if a genuine "no bytes anywhere" case exists, it must fail closed (loud), never silently drop — but the design expects this case not to arise (live sources aren't condemned).

## Out of scope
- "C" (GC-side ordering/reservation / back-link) — rejected.
- Any incremental-GC change (the fix is purely writer-side).
- The full GC (M-F) heartbeat semantics — unrelated.

## Sequencing
B167 ships **with** B160: B160 makes the GC productive, which is what exposes B167; merging B160 alone would create broken parts under dedup+GC load.
