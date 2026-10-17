---
description: 'Proposal rev.2: a client that carries the CAS single-attempt retry strategy / SingleAttempt write profile belongs to an outer retry loop, so its two Error-level log sites (Client.cpp network error, WriteBufferFromS3 S3Exception) log at Debug instead, and the expected-412 branch drops from Info to Debug. Two conditions on signals that already exist.'
sidebar_label: 'Single-attempt client log level (proposal, 2026-09-04)'
sidebar_position: 6
slug: /superpowers/cas/single-attempt-client-log-level-proposal-2026-09-04
title: 'Proposal: the CAS single-attempt S3 client logs its failed attempt below Error'
doc_type: 'reference'
---

# Proposal: the single-attempt CAS S3 client logs its failed attempt below Error {#proposal-single-attempt-client-log-level}

## Problem {#problem}

The CAS control plane of a writable Native mount issues its conditional and control-plane requests
through a client built by `S3ObjectStorage::getSingleAttemptClient` (`retry_strategy.max_retries = 0`,
`SingleAttemptRetryStrategy`); read-only mounts keep the default profile
(`ContentAddressedMetadataStorage.cpp:785`) and blob publication deliberately writes through a default
`WriteBufferFromS3` (`CasObjectStorageBackend.cpp:885`), so those paths keep today's logging. That is deliberate: a conditional write whose outcome is unknown must
not be replayed blindly by the client; `CasOperation::writeLoop` resolves the outcome by a read and
reissues. It works, but two upstream log sites treat the first network failure as the final one:

- `src/IO/S3/Client.cpp:817` -- `tryLogCurrentException(log, "Network error on S3 request, attempt {} of {}")`, level Error, printed as `attempt 1 of 1`;
- `src/IO/WriteBufferFromS3.cpp:818` -- `LOG_ERROR(log, "S3Exception name {}, ...")` for every non-412 error.

On a plain S3 disk the same timeouts are absorbed by the client's own loop (`s3_retry_attempts` = 500
by default) and never reach those lines. On the CAS lanes they reach stderr of a stateless test whose
write then succeeds through the engine, and fail the test on the log line alone
(`docs/superpowers/cas/2026-09-04-put-timeout-error-log-rca.md`). On the real-AWS soak of 2026-09-04
both lines appeared once each; both requests succeeded on the engine's second attempt.

## Change (rev.3; Codex round 1 REVISE, round 2 APPROVE WITH MINORS, minors applied) {#change}

The signal is NOT `max_retries == 0`: zero retries is a supported user configuration for ordinary
clients (`s3_retry_attempts`, documented "0 means no retries", copied into
`retry_strategy.max_retries` by `diskSettings.cpp`; backups and data-lake catalogs likewise), and those
clients have no outer loop, so their one failed attempt is terminal and must stay at Error. What
uniquely marks the CAS profile is what `getSingleAttemptClient` installs: the `SingleAttemptRetryStrategy`
object, and on the write side the `ObjectStorageRetryProfile::SingleAttempt` the engine already carries
in `WriteSettings`. Both are semantic, already present, and cannot be produced by user configuration.

1. `src/IO/S3/Client.cpp`, `net_exception_handler`: when the installed strategy is a
   `SingleAttemptRetryStrategy` (one `const` helper on `Client`, e.g. `usesSingleAttemptRetryStrategy`,
   implemented by `dynamic_cast` on `client_configuration.retryStrategy`), log the exception at Debug
   (`LOG_DEBUG` with `getCurrentExceptionMessage`) instead of `tryLogCurrentException` (Error). The
   `S3ReadRequestsErrors`/`S3WriteRequestsErrors` counters, the constructed `NETWORK_CONNECTION` outcome
   and the `ShouldRetry` consultation do not change.
2. `src/IO/WriteBufferFromS3.cpp`, the `S3Exception name` site: when
   `write_settings.object_storage_retry_profile == ObjectStorageRetryProfile::SingleAttempt`, use
   `LOG_DEBUG`; otherwise `LOG_ERROR` as today. The thrown `S3Exception` and
   `WriteBufferFromS3RequestsErrors` do not change.
3. Same file, the neighbouring 412 branch (ours, ef9931bfd3a): `LOG_INFO` becomes `LOG_DEBUG`. Info is
   for something the operator should know about; a conditional write losing its precondition is the
   caller's expected answer, handled one frame up, and says nothing to the operator.

Scope: the thrown `Poco::TimeoutException` path, which is what the lanes and the AWS soak showed.
A failure that arrives as an HTTP status (504, other 5xx) is logged at Error one layer lower, in
`PocoHTTPClient.cpp` ("Response status: ..."), which knows neither the strategy nor the profile; that
site is untouched here and becomes a follow-up only if a lane ever shows it.
Also untouched: `WriteBufferFromS3`'s multipart cleanup path (`tryToAbortMultipartUpload`, Error when
the abort itself fails). A control-plane object is far below the single-part threshold, and an abort
that fails is a genuine error on any profile.

## Implementation record {#implementation-record}

Landed as 08c2a2ec25e (product: `Client.h` +4, `Client.cpp` +10/-2, `WriteBufferFromS3.cpp` +8/-2;
tests: +179 in `gtest_aws_s3_client.cpp`, +109 in `gtest_writebuffer_s3.cpp`). Network exception
driven through `Client::PutObject` by a derived test client overriding the SDK virtual slot, because a
socket-level fault is converted to a non-throwing outcome inside `PocoHTTPClient` and never reaches
`net_exception_handler`. Reviews: ca-review-lite APPROVE; Codex round 3 APPROVE WITH MINORS (the 412
test captured at the Error threshold and so could not tell Info from Debug: fixed by capturing at
Information and asserting on the site's own text, since the cancel path logs Info lines of its own; Allman braces on the three `TEST_P` bodies: fixed; the verification matrix asserts
attempts, outcome type and error counters but not the outcome message text: accepted as is). Gates:
release `CAS*` 2472 and `WBS3` 37 green; ASan 2512 and `WBS3` 36 green (one test skipped under ASan by
its own guard).

## Alternatives considered {#alternatives}

- A thread-local scope flag around the engine's backend calls, like `Expect404ResponseScope`: a new
  class and thread-local, and every engine entry point (write, read, head, list, remove) must remember
  to open it. The 404 scope exists because one client serves both expected and unexpected 404s; here
  the signal is a property of the client, so a scope is redundant.
- Catching in `CasObjectStorageBackend`: too late, the lines are written below.
- Enabling client retries for CAS: rejected on principle (unknown-outcome conditional writes).

## Verification {#verification}

- Unit matrix (gtests already build clients by hand; `src/IO/tests/gtest_writebuffer_s3.cpp:261` builds
  one with `max_retries = 0` but the ORDINARY strategy, which is exactly the case that must keep Error):
  (a) `SingleAttemptRetryStrategy` installed / `SingleAttempt` profile: Debug; (b) ordinary strategy with
  zero retries: Error; (c) ordinary retrying client: unchanged. In every cell the counters, the
  exception type and message, and the attempt count are asserted equal to today's. Log capture: the
  RAII `Poco::StreamChannel` pattern of `src/IO/S3/tests/gtest_aws_logger.cpp:23`, in the
  ownership-preserving form used by `src/Disks/tests/gtest_cas_backend_generation.cpp:132`.
- Lane: the CA-s3 stateless lane must show no `<Error> WriteBufferFromS3: ... Timeout` and no
  `<Error> S3Client: Network error on S3 request, attempt 1 of 1` in any test's stderr.
