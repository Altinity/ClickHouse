---
description: 'Root cause of a CREATE TABLE giving up at the 90-second policy deadline on the CAS S3 stateless lane: 53 concurrent writers compare-and-swap one ref-catalog object, and the request engine paces a lost race with the same growing backoff as a transport fault, so the oldest loser starves. Numbers, code path, spec sentence, and the two fixes.'
sidebar_label: 'Ref-catalog starvation RCA (2026-09-04)'
sidebar_position: 6
slug: /superpowers/cas/ref-catalog-starvation-rca-2026-09-04
title: 'Ref-catalog compare-and-swap starvation under a parallel suite (RCA, 2026-09-04)'
doc_type: 'reference'
---

# RCA: ref_catalog CAS starvation under load {#rca-ref-catalog-cas-starvation-under-load}

## Symptom, reproduced from the run {#symptom-reproduced-from-the-run}

Test `01039_mergetree_exec_time` failed with:

```
Code: 210. DB::Exception: CAS write could not be committed (CAS ref catalog 'cas_s3/cas/ref_catalog'
update: gave up at the policy deadline after one or more attempt(s)); retrying later. (NETWORK_ERROR)
```

on `CREATE TABLE tab (`A` Int64) ENGINE = MergeTree ...`, query_id `1f96b289-9d70-4fc7-8c9f-f2cc71452781`,
duration 78763 ms (`system.query_log`, type `ExceptionBeforeStart`).

## The numbers (server queried live inside the praktika container, `system.text_log` / `system.query_log`, window 2026-09-04 00:39:50–00:41:10 UTC) {#the-numbers-server-queried-live-inside-the-praktika-container-system-text-log-sy}

| metric | value |
|---|---|
| `PreconditionFailed` (HTTP 412) events on `cas_s3/cas/ref_catalog`, whole window | 113 over ~80 s (bursty: up to 10/s in some seconds, idle in others) |
| distinct `text_log` threads that hit the 412 | 53 |
| distinct `query_id`s attributed to those 412s | 5 (most 412 rows carry an empty `query_id` — logged from a background/detached execution context, not the originating query) |
| `PreconditionFailed` attempts by the eventually-failing writer alone | 35, spread across its whole 78.9 s lifetime |
| `CREATE TABLE ... MergeTree` durations in the window (`query_log`, `QueryFinish`, n=109) | p50 = 203 ms, p90 = 456.8 ms, max = 10441 ms |

The failing writer's own 35 conflict timestamps (`grep "S3Exception name PreconditionFailed"` filtered to its `query_id`) show inter-attempt gaps growing from 0–1 s for the first few attempts to a roughly flat 1–6 s (mean ≈ 2.3 s) for the rest — the shape of `uniform(0, min(5000, 200·2ⁿ))` full-jitter backoff saturating its 5 s cap after ~5 losses, not a short fixed retry interval. Meanwhile the aggregate 412 rate on the same key from *other* writers stayed close to 1–2/s the whole time (with bursts to 10/s), i.e. some other writer committed against that key roughly every ~1 s throughout — fast enough that a writer sitting in a multi-second backoff essentially never finds the key free when it wakes up.

## The code: conflict and transport fault share one pacing schedule {#the-code-conflict-and-transport-fault-share-one-pacing-schedule}

`src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Backend/CasRequests.cpp`:

- `writeLoop` (`CasRequests.cpp:709-851`) returns `Conflict{...}` at `CasRequests.cpp:829`/`:840` once a refused precondition resolves to "somebody else really holds the key" — this is an ordinary, expected outcome under contention, not a fault.
- `readModifyWrite` (`CasRequests.cpp:866-919`) is the loop that owns retrying that `Conflict`. On a `Conflict` result (`CasRequests.cpp:890`) it falls straight into:
  ```cpp
  if (auto given_up = pauseAndReissue(state, bound))   // CasRequests.cpp:902
      return *given_up;
  ```
- `pauseAndReissue` (`CasRequests.cpp:692-707`) is also the function the *transport-fault* / unresolved-but-repeatable path calls (`CasRequests.cpp:805`, `:849`), and it paces every call the same way:
  ```cpp
  const uint64_t pause_ms = Retry::backoff(++state.reissues);   // CasRequests.cpp:694
  ```
  `state.reissues` is one counter shared by every reissue reason inside one `readModifyWrite` call — a lost race and a genuine transport ambiguity both increment it and both draw from the same schedule.
- `Retry::backoff` (`CasRetry.cpp:11-18`): full jitter, `uniform(0, min(5000, 200 << (attempt-1)))` ms, 1-based `attempt`. So attempt 1 → 0 ms, attempt 2 → `uniform(0,200)`, … saturating at `uniform(0,5000)` by attempt 6.

`src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasRefCatalog.cpp:152-184` (`casUpdateImpl`, the function behind `CREATE TABLE`/`DROP TABLE`'s catalog update) calls exactly this path: `op.readModifyWrite(key, decide, Retry::standard())` at line 176, `Retry::standard()` = 90 s window (`CasRetry.h:34`). Each iteration's `decide` (`CasRefCatalog.cpp:162-174`) fully decodes the current catalog object, re-applies `mutate` against the **fresh** read, and re-encodes the whole catalog — consistent with the observed object size climbing from 8208 to 13850+ bytes over the window as more concurrent `CREATE`/`DROP` land. This work is comparatively cheap; the pacing cost is dominated by the shared `pauseAndReissue` sleep, not the re-encode.

## Pre-migration comparison: the old catalog loop had no pacing on conflict at all {#pre-migration-comparison-the-old-catalog-loop-had-no-pacing-on-conflict-at-all}

Found via `git log --follow` on `CasRefCatalog.cpp`; the migration to `CasOperation`/`readModifyWrite` landed in commit `2f4aa25b03c` ("ca-ref: catalog and checkpoint publisher on `CasOperation` — `readModifyWrite`, `admitted()` at the verdict points"). Its parent, `2f4aa25b03c^`, had:

```cpp
// CasRefCatalog.cpp, casUpdateImpl, pre-migration
for (size_t attempt = 0; attempt < kMaxCatalogCasAttempts; ++attempt)     // kMaxCatalogCasAttempts = 100
{
    snap.life_index.throwIfAmbiguous("CAS ref catalog mutation");
    RefCatalog candidate = mutate(snap.catalog);
    const String bytes = encode(candidate);
    const CasResult res = backend.casPut(key, bytes, snap.token);
    if (res.outcome == CasOutcome::Committed)
        return candidate;
    snap = CasRefCatalog::read(backend, layout);          // conflict: immediate re-read, no sleep, no backoff
}
throwCasWriteRetryLater(...);   // after 100 unslept attempts
```

Genuine conflicts (`CasOutcome != Committed`, i.e. a different token now at the key) were retried **immediately**, up to 100 times, with **zero** pacing — the only bound was the attempt count, not a wall-clock deadline. So pre-migration, a losing writer's next attempt was never handicapped relative to a brand-new one; the race was closer to "whoever's `casPut` lands first, repeatedly," which converges quickly under contention (the loser's re-read + re-encode + re-`casPut` round trip is the only inter-attempt delay, tens of milliseconds, not seconds).

## The spec explicitly prescribes today's behavior — this is not an oversight {#the-spec-explicitly-prescribes-today-s-behavior-this-is-not-an-oversight}

`docs/superpowers/specs/2026-09-02-cas-backend-token-contract-design.md`, the `readModifyWrite` section:

> "...on `Conflict` the engine's resolve read **is** the next iteration's read — the loop sleeps with jitter and re-decides on it, never reading twice per conflict on the hot keys... Conflicts spend the same budget as errors: a hot key is also a failure, and it must end — GCS bounds mutations of one object at about one per second, and today's `_ckpt` and catalog loops reissue without a sleep." (lines 624–632)

And in the "Where each verb goes" rules:

> "**A hand-written loop captures one `Retry` before it starts, shares it across every call it makes, and sleeps with the engine's jitter between iterations**... The loop ends when that deadline does — never a hundred unslept iterations against the hot catalog key, and never a hundred ninety-second budgets. This makes '**conflicts spend the same budget as errors**' true everywhere it is stated." (lines 683–687)

The before/after code sample for the ref catalog specifically (lines 647–665) shows the old `/// Conflict: loop, no sleep` comment being replaced by `readModifyWrite` under `Retry::standard()` — i.e. the design's stated goal *is* exactly what `CasRefCatalog.cpp:176` implements. The spec never discusses fairness among multiple independent concurrent writers on one hot key; its cited justification is per-writer/per-backend rate control (bounding total attempts against a backend — GCS in particular — that throttles mutations of one object to roughly one per second), not the relative treatment of an old contender against new ones.

## Verdict {#verdict}

**Spec-conformant, but the spec's safety argument has a real gap: fairness under sustained multi-writer contention was never analyzed, and the code now exhibits genuine starvation as a result.**

The migration deliberately replaced "conflicts retry immediately, capped only by attempt count" with "conflicts and transport faults share one capped-exponential jittered backoff, capped by wall-clock deadline" — explicitly to stop catalog conflicts from hammering the object un-paced (the spec's own `today's ... catalog loops reissue without a sleep` line names this as the problem being fixed, motivated by GCS's ~1 write/s per-object budget). That per-writer rationale is sound. What it does not account for is the *relative* dynamic among many independent writers: a writer's own backoff grows with **its own personal loss count** (`state.reissues`), while a brand-new contender always starts at `attempt=1` (0 ms backoff). Under sustained arrival of new writers (10 parallel test jobs constantly issuing `CREATE`/`DROP`), an unlucky writer's backoff races monotonically upward toward its 5 s cap while fresh arrivals keep winning at near-zero backoff — so the *oldest* loser is systematically the *least* likely to win the next race, not more likely, right up until the shared 90 s deadline. That is the observed 78.9 s, 35-attempt failure: spec-conformant code, produced by a safety property (bounded per-writer rate) that the spec proved, sitting next to a fairness property (bounded per-writer wait under N-way contention) that it never stated or proved.

## Recommended fix {#recommended-fix}

Give conflicts their own, small, **non-growing** jittered pause, decoupled from the transport-fault/ambiguous-reissue counter — e.g. a separate `Retry::conflictBackoff()` drawing `uniform(0, ~200–300 ms)` regardless of how many times *this* call has already lost, used only at `CasRequests.cpp:902` (and the equivalent site in `readModifyWriteOnPresence`), while `pauseAndReissue`'s existing exponential-with-5s-cap schedule stays exactly as is for genuine transport ambiguity/refresh reissues at `CasRequests.cpp:805`/`:849`. This keeps a real pace limiter on the hot key (no un-slept hammering, so the GCS-rate-limit motivation is preserved), keeps the shared 90 s deadline as the only hard bound, but removes the property where losing a race makes the *next* race harder to win than it is for a writer that hasn't lost yet — every writer contending on the key gets i.i.d. small delays, so no one accumulates a structural handicap. Flag for whoever picks this up: the flat ceiling should be sized against the same "GCS ~1 write/s per object" budget the spec cites (ceiling × expected concurrent writers on one key should not blow past that), or the fix should note that a shared/coordinated per-key backpressure signal (rather than a private per-call counter) is the more robust long-term answer if GCS deployments show the same symptom under real contention — this fix targets the S3-class-backend fairness bug demonstrated here, not a from-scratch redesign of the throttle.

**Test to pin it:** using the existing fake-clock/`setSleepFnForTest` harness in `CasRequests`' unit tests, drive N (e.g. 20) independent simulated `readModifyWrite` writers contending on one key, each new writer "arriving" (starting its own attempt-1) at a steady simulated cadence while earlier ones keep retrying. Assert two things on the fake clock: (1) every writer eventually commits before the shared policy deadline (today's behavior already gives this under `Retry::standard()`'s 90 s, so it is not the regression to pin), and (2) the **worst-case number of consecutive losses for the earliest-arriving writer is bounded** (e.g. does not exceed a small constant multiple of the number of writers) rather than unbounded/monotonically growing while newer arrivals keep winning — the second assertion is red today (the earliest writer's loss streak grows without bound as more contenders keep arriving under the current shared-counter, growing-backoff scheme) and green after decoupling conflict pacing from `state.reissues`.

---
**412/s figure:** 113 `PreconditionFailed` events on `cas_s3/cas/ref_catalog` over the ~80 s failure window (~1.4/s average, bursts to 10/s), from 53 distinct threads; the one query that ultimately failed (78.9 s, `CREATE TABLE tab (A Int64) ...`) alone accounted for 35 of those attempts, spaced by a growing/saturating jittered backoff (0–1 s early, ~1–6 s once capped) while competitors kept committing against the same key roughly every ~1 s throughout.

**Verdict:** Spec-conformant (the migration's shared conflict/error backoff is exactly what `docs/superpowers/specs/2026-09-02-cas-backend-token-contract-design.md` prescribes), but the spec never analyzed fairness under sustained N-way contention on one hot key, and the code now genuinely starves the unluckiest writer — a real regression relative to the pre-migration immediate-retry loop for that specific dimension, traded for a real fix (bounded, paced retries) to a different problem (unbounded un-slept hammering of the object).

**Recommended fix (two lines):** Decouple conflict pacing from the transport-fault reissue counter — a flat, small, non-growing jittered pause (~`uniform(0,250ms)`) for `Conflict` retries in `readModifyWrite`/`readModifyWriteOnPresence`, keeping the existing capped-exponential backoff only for genuine transport/ambiguous-attempt reissues. Pin it with an N-concurrent-writer fake-clock test asserting the earliest writer's consecutive-loss streak stays bounded, not monotonically growing, under sustained new arrivals.
