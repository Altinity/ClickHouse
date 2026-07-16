# CAS merge upload-failure resilience — fix design (#37)

**Status:** design approved (brainstorming), pending spec review → `writing-plans`.
**Branch:** `cas-gc-rebuild`. **Scope:** CAS-side only; no upstream coupling; no new error code; no architectural staged-part preservation.

## Motivation {#motivation}

Campaign item #37: a background MERGE on a content-addressed (S3-backed) disk, under a sustained object-store fault, "resets progress in loops" — it recomputes the whole output part every ~2s for the entire outage instead of retrying just the publish.

This was investigated rigorously (code-map + `s3faultproxy` reproduction + two independent model consults). The original framing — "INSERT survives by re-streaming from scratch, MERGE reruns the whole merge" — is **refuted**: under the same fault a plain INSERT dies with the byte-identical `Code 236`. There is no merge-vs-insert difference at the CAS layer; the in-commit upload-retry stack is shared and caller-agnostic.

**Validated mechanism (log-verified):** the sustained fault fails the CAS write **mount-lease renewal** PUT; the lease deadline expires / the write fence trips; every write (merge *and* insert) then fails `stageManifest`'s `fence_ok()` gate instantly (pre-attempt reject); the self-remount recovery loop is blocked by the same fault, so the write outage lasts the whole fault window; on fault clear, self-remount recovers in ~16s and the next merge commits cleanly.

**This is fail-closed-correct: no data loss, self-heals.** The problem is *availability + efficiency*, not correctness. Three CA-side defects amplify a transient blip into a pool-wide, multi-minute write outage plus a recompute storm.

## The three defects {#defects}

1. **Over-fencing (root amplifier).** `SingleWriterSlot::backgroundLoop` (`CasServerRoot.cpp:995-1025`) treats *any* exception from `renewOnce` (`CasServerRoot.cpp:913-931`) as terminal → fence lost, incarnation burned. `renewOnce` calls `putOverwrite` single-shot and throws at `:926` *before* the mismatch check at `:927-928`, so a transient network fault is misread as foreign-token supersession. A brief S3 blip thus burns the whole writer incarnation even though the lease is still valid for ~28s.
2. **`ABORTED` defeats the existing merge backoff.** ClickHouse already has exponential backoff for failed replicated merges (`ReplicatedMergeTreeQueue::getPostponeTimeMsForEntry`, `2^num_tries`, cap `max_postpone_time_for_failed_replicated_merges_ms`). It never engages because CAS throws `ABORTED`, and `ReplicatedMergeMutateTaskBase.cpp:63-98` treats `ABORTED` (and `PART_IS_TEMPORARILY_LOCKED`) as "deliberately cancelled, not an error" → `updateLastExeption` is never called → `last_exception_time_ms` stays 0 → postpone computes 0 → tight loop (239 full recomputes observed vs the ~15-20 the policy would allow).
3. **Opacity.** The thrown message says "retry budget exhausted" when nothing was ever attempted (fence-lost is a pre-attempt reject). The failures log at Information level with `last_exception` never populated → the storm is nearly invisible in `system.replication_queue`.

Fail-closed-correctness is preserved by construction throughout: the mount-lease protocol guarantees no other writer can claim the slot until the current lease deadline expires.

## Fix 1 — renew-retry while the lease is still valid {#fix-over-fencing}

Change the renewal loop to distinguish two failure modes:

- **Observed token mismatch** (`onRenewMismatch`: the PUT completed but returned a different owner/epoch = *proven* supersession by another writer) → **trip the fence immediately** (correct fail-close; we have been deposed).
- **Transient exception** (`putOverwrite` threw — timeout / 5xx / reset; outcome *not* observed) → **do not fence yet**. Keep the loop alive and **retry `renewOnce`** on subsequent beats (short backoff to fit more attempts), while the lease deadline is still valid. Trip the fence only when the deadline actually nears (`now + lease_safety_margin >= lease_deadline` with no successful renew since) or a later renew observes a real mismatch.

**Correctness invariant:** the lease protocol guarantees no other writer can `claim` the slot until the current lease **expires**. Therefore continuing to write while `now < lease_deadline` is safe — we provably still hold the slot. The fix makes the *deadline* the fail-close boundary (it already is, protocol-wise) instead of "first transient exception," giving ~lease-TTL (~30s) of blip tolerance. It never permits writing past a proven supersession (immediate fence) or past the deadline (fence).

Confined to `CasServerRoot` / `MountLeaseKeeper` (CA code); no generic/upstream code touched.

## Fix 2 — retry-later error class via one helper {#fix-error-class}

Introduce a single helper that all *escaping* retry-later throws route through:

```cpp
[[noreturn]] void throwCasWriteRetryLater(const String & why);
// throws Exception(ErrorCodes::NETWORK_ERROR, "CAS write could not be committed ({}); retrying later", why)
```

The full motivating comment (lives above the helper — see Appendix A) records why `NETWORK_ERROR` and how to switch to a dedicated code later in one line.

**Why `NETWORK_ERROR` (existing code, no new code):**
- It is *not* in the merge exemption set (only `ABORTED` and `PART_IS_TEMPORARILY_LOCKED` are), so the existing exponential backoff engages automatically — this is the actual fix for defect 2.
- It is already in ClickHouse's transient/retryable taxonomy (`checkDataPart::isRetryableException` lists it beside `ABORTED`), so a part under verification is not misread as corrupted.
- Nothing on the merge / insert / replication commit path special-cases it in a way that would misfire (ZooKeeper retriability keys on `Coordination::Exception`, a different type); it is not caught specially on the CAS write path.
- Reusing it is *not* the anti-pattern that caused defect 2: `ABORTED`'s handling was *incompatible* (silent, no backoff); `NETWORK_ERROR`'s handling is *compatible* (transient → backoff) — the correct kind of reuse.

**Honest caveat + escape hatch:** `NETWORK_ERROR` is coarser than the true condition (accurate for throttled/timed-out store and lost-lease; a mild overstatement for a purely logical fence loss). The precise cause is always in the message. If the imprecision ever matters, switch the single throw in the helper to a dedicated `CONTENT_ADDRESSED_WRITE_RETRY_LATER` code (one appended line in `ErrorCodes.cpp`, optionally add to `checkDataPart::isRetryableException` and an HTTP-status mapping); backoff still engages automatically. We keep `NETWORK_ERROR` now for zero new coupling to generic code, consistent with the rest of the CAS layer.

**Scope — which throws route through the helper (the escaping retry-later class):**
- `CasPartWriteTxn.cpp`: `requireAlive` `:134`/`:141`, `uploadFromSource` `:542` (Unresolved), `stageManifest` `:787`/`:796`, `promote` `:903`/`:907`/`:909`/`:949`/`:1015`.
- `CasRefLedger.cpp`: `:231`, `:393`, `:799`, `:904`, `:918`, `:1144`, `:986`, `:1278`, `:1265`.

**`ABORTED` values that stay (NOT rerouted):** the internal control-flow signals `observeAndAdmit:317` / `reviveObserve:417` (condemned/vanished → re-upload from source, caught inside `putBlob`, never escape); startup/decommission (`CasPool.cpp:406`/`:421`/`:429`); generic live-lock brake (`CasPlainObjects.cpp:40`/`:64`); `republishRef` genuine content conflict (`PartFolderAccess.cpp:359`); GC-internal (`CasGc.cpp`).

**One intended behavior change:** `putBlob`'s bounded loop (`CasPartWriteTxn.cpp:222`) catches only `ABORTED`; once `uploadFromSource:542` throws `NETWORK_ERROR`, `putBlob` no longer locally retries the fence-lost/Unresolved case 8× — it escapes to the merge backoff instead. Desirable (no point hammering a lost fence 8× locally).

## Fix 3 — honest message + observability {#fix-opacity}

- The message is carried by `throwCasWriteRetryLater(why)`: `why` states the precise cause ("mount-fence lost", "conditional PUT budget exhausted after N attempts", "namespace being dropped"). Distinguish fence-lost (0 attempts) from genuine 16-attempt/90s budget exhaustion at the throw site.
- Visibility is largely free from Fix 2: because the throw is now non-exempt `NETWORK_ERROR`, `ReplicatedMergeMutateTaskBase` calls `updateLastExeption`, so the cause appears in `system.replication_queue.last_exception` / `last_exception_time`.
- Raise the retry-later log level Information → Warning, **rate-limited** (`LogSeriesLimiter`, used strictly to throttle the repeated line under a sustained outage). With Fixes 1+2 the volume is already much lower (blips no longer fence; backoff caps recomputes).

## Testing {#testing}

- **Regression for Fix 1 (closes the chaos-coverage gap — soak chaos only faulted *nodes*, never a degraded-S3-while-alive):** on `docker-compose-s3faultproxy.yml`, fault S3 PUT/POST for a **short** window (< lease TTL, ~15s) with nodes alive and writing; assert the mount-lease is **not** lost (no fence trip, no incarnation recycle, no epoch bump) — writes pause/retry-through and resume, no pool-wide outage.
- **Regression for Fix 2:** a **long** fault window (> lease TTL) → fence trips (correct fail-close); assert the merge **backs off** (growing intervals, not 239 recomputes) and recovers cleanly after the fault clears.
- **Correctness verification (#4, previously unverified):** in the same leg, after recovery run `fsck` to fixpoint; assert `dangling=0` / no orphans. Link the precommit-window edge to the known S30 DANGLING-PRECOMMIT class.
- **Unit (gtest):** renew-retry logic (transient exception → retry while deadline valid → fence on deadline-near / mismatch); and that the retry-later throw is `NETWORK_ERROR`, not `ABORTED`.
- Fold the S3-fault leg into the permanent scenario/soak regression, not a one-off repro.

## Non-goals {#non-goals}

- **Staged merged-part preservation / resume-upload** — explicitly out of scope. Blobs are content-addressed, so a recompute's blobs dedup-resolve; once Fix 1 makes the scenario rare and Fix 2 makes it back off, the recompute cost is bounded and not worth the architectural risk.
- **A new error code** — deferred (escape hatch documented in Fix 2 / Appendix A).
- **MOVE-to-CA (`promote 'moving'`)** — a separate, larger feature design (its own spec).

## Appendix A — helper comment {#appendix-a}

The full `throwCasWriteRetryLater` motivating comment (approved in brainstorming) is reproduced verbatim in the implementation plan; it explains the `NETWORK_ERROR` choice, the defeated-backoff mechanism it fixes, the honest caveat, the one-line switch to a dedicated code, and the scope (escaping retry-later throws only; internal-signal / startup / contention `ABORTED`s untouched).
