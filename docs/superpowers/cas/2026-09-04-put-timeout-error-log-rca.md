---
description: 'Root cause of stateless tests on the CAS S3 lane failing on a WriteBufferFromS3 timeout logged at Error although the write succeeded through the request engine: the 5-second single attempt, where the log line comes from, why it predates the request-contract migration, and the recommended log-severity scope.'
sidebar_label: 'PUT-timeout Error-log RCA (2026-09-04)'
sidebar_position: 5
slug: /superpowers/cas/put-timeout-error-log-rca-2026-09-04
title: 'CAS S3 lane: a timed-out single-attempt PUT is logged at Error (RCA, 2026-09-04)'
doc_type: 'reference'
---

# PUT-timeout Error-log RCA (local CA-s3 stateless lane) {#put-timeout-error-log-rca-local-ca-s3-stateless-lane}

Read-only investigation. Repo `/home/mfilimonov/workspace/ClickHouse/master`, branch `cas-gc-rebuild`.

## Symptom (confirmed from `build/test_task24_stateless.log`) {#symptom-confirmed-from-build-test-task24-stateless-log}

Three occurrences, two within the same second (`19:31:49` / `19:31:51`):

```
2026-09-03 19:31:06 Reason: having stderror:
Message: Poco::Exception. Code: 1000, e.code() = 0, Timeout (version 26.6.2.20000.altinityantalya), bucket test, key cas_s3/cas/ref_catalog, object size 2295
2026-09-03 19:31:49 Reason: having stderror:
Message: Poco::Exception. Code: 1000, e.code() = 0, Timeout (...), bucket test, key ...
2026-09-03 19:31:51 Reason: having stderror:
Message: Poco::Exception. Code: 1000, e.code() = 0, Timeout (...), bucket test, key cas_s3/cas/ref_catalog, object size 6797
```

All three tests' stdout was correct — the writes eventually landed.

## 1. Where the `Error` line comes from {#1-where-the-error-line-comes-from}

`src/IO/WriteBufferFromS3.cpp:761-834`, `makeSinglepartUpload`'s `upload_worker` lambda. The loop runs
`max_retry = max(request_settings[max_unexpected_write_error_retries], 1)` iterations
(`WriteBufferFromS3.cpp:775-776`). On every failed `PutObject` outcome that is not `NO_SUCH_KEY` and
not a precondition-failed 412 (`S3::isPreconditionFailedError`), it does:

```cpp
LOG_ERROR(log, "S3Exception name {}, Message: {}, bucket {}, key {}, object size {}",
          outcome.GetError().GetExceptionName(), outcome.GetError().GetMessage(), bucket, key, content_length);
throw S3Exception(...);
```

(`WriteBufferFromS3.cpp:817-826`). This fires on **every single failed attempt inside this loop**, not
only "when the buffer gives up" — those coincide here only because CAS conditional writes set
`max_retry = 1` (see §2), so the loop's only iteration is also its last. There is no thread-local
"expected transient" scope guarding this line (unlike 404s, see §5); `outcome.GetError().GetExceptionName()`
is empty and the message is the AWS SDK's own text for a `Poco::TimeoutException` surfaced through
`PocoHTTPClient`, i.e. the client-side HTTP timeout fired before the server responded.

## 2. CAS write path and its effective attempt timeout {#2-cas-write-path-and-its-effective-attempt-timeout}

`ObjectStorageBackend::write` (`CasObjectStorageBackend.cpp:827-846`) builds
`conditionalWriteSettings()` for every Native-mode conditional write (`ref_catalog`, ref-log chunks,
`gc/state`, the mount registry, etc. — anything going through `write`/`putIfAbsent`/`casPut`):

```cpp
WriteSettings ObjectStorageBackend::conditionalWriteSettings() const   // CasObjectStorageBackend.cpp:805-825
{
    ws.object_storage_request_mode = ObjectStorageRequestMode::NativeConditional;
    ws.s3_check_objects_after_upload_override = false;
    ws.s3_max_unexpected_write_error_retries_override = 1;                 // exactly 1 WriteBufferFromS3-level attempt
    ws.object_storage_retry_profile = ObjectStorageRetryProfile::SingleAttempt;
    ws.object_storage_attempt_timeout_ms = attempt_timeout_ms;             // CasRequestBudget::attempt_timeout_ms
    return ws;
}
```

`attempt_timeout_ms` is `CasRequestBudget::attempt_timeout_ms` (`CasRequestBudget.h:16`, default
`5000`), threaded from `ContentAddressedSettings` (`ContentAddressedSettings.cpp:80`:
`DECLARE(UInt64, attempt_timeout_ms, 5000, ...)`) through
`ContentAddressedMetadataStorage.cpp:305,771,782` into `pool_config.cas_request_budget`, and from there
into every `CasObjectStorageBackend`'s `attempt_timeout_ms` member
(`CasObjectStorageBackend.cpp:128-132`), used by every keyed primitive including `write`
(`CasObjectStorageBackend.cpp:823`). No test config under `tests/config/` overrides it, so the value in
effect for the local CA-s3 lane is the shipped default.

`ObjectStorageRetryProfile::SingleAttempt` resolves, in `S3ObjectStorage::clientForRetryProfile`
(`S3ObjectStorage.cpp:1073-1079`), to `getSingleAttemptClient(request_timeout_ms)`
(`S3ObjectStorage.cpp:1043-1071`), which clones the base client with
`cfg.retry_strategy.max_retries = 0`, `cfg.retryStrategy = SingleAttemptRetryStrategy`, and
`cfg.requestTimeoutMs = request_timeout_ms` when nonzero (`S3ObjectStorage.cpp:1057-1068`).

**Effective per-attempt timeout for a `ref_catalog` PUT today: 5000 ms**, with the AWS SDK layer making
exactly one HTTP attempt (`max_retries = 0`) and `WriteBufferFromS3`'s own loop also making exactly one
attempt (`max_unexpected_write_error_retries_override = 1`). So a stall past 5 s at the HTTP layer is
guaranteed to throw and log `Error` exactly once per engine-level PUT attempt — there is no absorption
layer below the CAS request engine.

## 3. Pre-migration comparison — not a migration regression {#3-pre-migration-comparison-not-a-migration-regression}

The team-lead brief pointed at `8ea8888e185^`, but that commit (`ca-requests: apply second-review
corrections to Retry and Incarnation`) never touches `CasObjectStorageBackend.cpp` — its diff is
confined to `CasIncarnation.h/.cpp`, `CasRetry.h/.cpp`, and the gtest file. So `8ea8888e185^`'s
`CasObjectStorageBackend.cpp` is byte-identical to today's on this path.

Walking further back to before the request-contract engine existed at all
(`6726575e665^`, the commit immediately preceding `ca-requests: the contract's types`), the same
`conditionalWriteSettings()` — same `SingleAttempt` profile, same
`s3_max_unexpected_write_error_retries_override = 1`, same `object_storage_attempt_timeout_ms =
attempt_timeout_ms` — was already in place (`CasObjectStorageBackend.cpp:827-840` at that revision).

`git log -S SingleAttemptRetryStrategy` / `-S cas-s3-timeout-retry-control` traces this whole mechanism
to the older `a5783037dbb` (`cas: generic ObjectStorageRetryProfile + S3 single-attempt client owned by
S3ObjectStorage (F5a)`) / `S3 conditional-write support and 412 no-retry policy` work, well before the
request-contract migration series (`6726575e665` onward) that this unit's brief is reviewing.

**Verdict: pre-existing behaviour, not a migration regression.** The request-contract migration
(`CasRequests.cpp` engine, `writeLoop`, `Retry`, `Incarnation`) changed *how the ambiguity from this
error is resolved and reissued* (§4), but did not change the single-attempt client, the 5 s timeout, or
the `LOG_ERROR` call site — all three predate it by many commits. The same S3 stall against the same
`SingleAttempt` client with the same 5 s budget would have produced the identical `Error` line before
the migration too; only the caller that reissues after it is new.

## 4. Engine reissue after the timeout — confirms stdout correctness {#4-engine-reissue-after-the-timeout-confirms-stdout-correctness}

`CasOperation::writeLoop` (`CasRequests.cpp:709-852`) calls `owner.backend->write(...)` inside a
`try`/`catch (const Exception & e)` (`CasRequests.cpp:748-778`). `S3Exception` derives from `DB::Exception`
(`src/IO/S3Common.h:29`), so a timed-out PUT lands in this arm, not the `catch (const std::exception & e)`
/ `Poco::Exception` arm below it.

Classification (`CasRequests.cpp:757-778`):
- `isDeterministicLocalFailure(e.code())` (`CasRequests.cpp:211-215`) only matches `LOGICAL_ERROR`,
  `NOT_IMPLEMENTED`, `BAD_ARGUMENTS`, `CORRUPTED_DATA` — a network timeout's S3 error code is none of
  these, so it does not rethrow.
- `isRefreshableCredentialError(e)` — false for a timeout (credential-error name list only).
- `isDefinitelyRefusedWrite(e)` (`CasRequests.cpp:217-229`) only matches malformed-request,
  entity-too-large, access-denied, or refreshable-credential S3 errors — a timeout matches none, so it
  is not treated as a definite refusal.

So the `catch` block falls through without setting `outcome`, and at `CasRequests.cpp:798-799`:

```cpp
if (!outcome && !credential_answer)
    state.any_ambiguous = true;
```

the timeout is correctly recorded as an **ambiguous** attempt (it may have landed), then settled by an
exact read (`observe`/`observePresence`, `CasRequests.cpp:815-818`) and, if unresolved but the
precondition is still satisfiable, reissued (`CasRequests.cpp:843-850`) up to the call's deadline. This
is exactly the "ambiguity → resolve by read → reissue" path and explains why all three tests' stdout was
correct: the engine treated the timed-out attempt as unproven, read the key to check, and either found
its own write already landed or reissued and succeeded on a later attempt — while `WriteBufferFromS3`
had already unconditionally logged `Error` for that one HTTP attempt.

## 5. Options (not implemented) {#5-options-not-implemented}

**(a) Thread-local "expected transient" suppression scope**, modeled on `Expect404ResponseScope`
(`src/IO/Expect404ResponseScope.h/.cpp`, checked today only in `PocoHTTPClient.cpp:737` for a 404 status
code — nothing analogous exists today for a client-side timeout at the `WriteBufferFromS3.cpp:817-819`
call site, and there is no per-buffer/`S3RequestSettings` log-level knob to reuse instead). A new scope
set by `CasOperation::writeLoop` around `owner.backend->write` (single-attempt engine calls only) could
downgrade `LOG_ERROR`→`LOG_INFO` at `WriteBufferFromS3.cpp:818` for the timeout/network-stall class
specifically (leave 412/`NO_SUCH_KEY` as already handled, leave genuine SDK/auth/malformed-request
errors at `Error`), while still incrementing `ProfileEvents::WriteBufferFromS3RequestsErrors`
(`WriteBufferFromS3.cpp:802`) so the event is still observable in metrics, just not on stderr.

**(b) Raise `attempt_timeout_ms`.** Cheap but blunt: it slows down genuine-refusal detection and lease
budget math everywhere (`CasRequestBudget.cpp` validates it against `mount_lease_ttl_ms`,
`CasPool.cpp:96,816-817,978,1120,1467` use it for deadline/renewal arithmetic), and does not eliminate
the log line under sufficiently loaded local S3 — it only shifts the threshold.

**(c) Let `WriteBufferFromS3`'s own retries absorb the stall.** Not available on this path by design:
`s3_max_unexpected_write_error_retries_override = 1` and `SingleAttempt`/`max_retries = 0` are
deliberate (`CasObjectStorageBackend.cpp:812-820`'s comment: the engine's own reissue is what must
happen, not a blind retry of a conditional request at the buffer/SDK layer, since a client-level retry
here could double-send the same conditional write in ways the engine's read-and-reissue logic is
specifically built to avoid).

**(d) Treat as local-infra noise.** Not well supported: the 5 s `attempt_timeout_ms` and single-attempt
client are the *shipped* Native conditional-write defaults, not a local-only relaxation, and CI's
stateless lane runs the same parallel-container-against-local-S3 topology this failure reproduced under
here (two of the three occurrences landing in the same wall-clock second under load). Nothing found in
this investigation shows the stall is impossible in CI; dismissing it as infra-only would just mean it
resurfaces there intermittently instead.

**Recommendation: (a).** A "this attempt's failure is expected to be transient and already handled by
the request engine's own ambiguity-resolution" log-level scope is the correct fix, because it targets
the actual mismatch — `WriteBufferFromS3` logging at `Error` severity (and failing tests on stderr
presence) for an outcome the CAS request engine is *designed* to treat as ambiguous-and-recoverable, not
as a failure. The safety argument that must stay intact either way: a PUT that times out may have
landed, so nothing about this fix may touch `CasRequests.cpp`'s ambiguity/read/reissue path (§4) — it is
purely a logging-severity change at the `WriteBufferFromS3` call site, gated to single-attempt
engine-driven writes so an ordinary (non-CAS) `WriteBufferFromS3` timeout — which has no engine watching
it — keeps logging at `Error` as before.
