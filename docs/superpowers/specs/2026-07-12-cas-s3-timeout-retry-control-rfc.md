---
description: 'RFC for bounded, lease-aware S3 timeouts and retries in the content-addressed storage protocol'
sidebar_label: 'CAS S3 Timeout and Retry Control'
sidebar_position: 20260712
slug: /superpowers/specs/cas-s3-timeout-retry-control-rfc
title: 'CAS S3 Timeout and Retry Control RFC'
doc_type: 'reference'
---

# CAS S3 Timeout and Retry Control RFC {#cas-s3-timeout-retry-control-rfc}

**Date:** 2026-07-12
**Branch:** `cas-gc-rebuild`
**Status:** proposed

## Summary {#summary}

CAS correctness depends on knowing whether a protocol write definitely succeeded, definitely did not
happen, or has an uncertain result. The generic S3 client currently owns request timeouts and may retry
requests internally. That is insufficient for conditional CAS writes because an internal retry can
hide the boundary between these outcomes and can continue after the writer's mount lease is no longer
valid.

This RFC makes timeout and retry policy explicit for every CAS S3 operation:

- an HTTP-attempt timeout is not treated as a deadline for the complete logical operation;
- protocol-significant conditional writes are attempted once by the S3 client;
- CAS code, not the generic SDK retry strategy, decides whether another attempt is legal;
- no new attempt starts after the local mount fence is lost or without enough lease time remaining;
- an uncertain write is resolved from the exact object and expected deterministic bytes where the
  operation permits it;
- retry and resolution budgets are bounded and observable.

This policy reduces and exposes the late-write window. It does not prove that S3 cancels a request after
a client timeout and therefore does not, by itself, solve the late-predecessor correctness limitation
in the ref snapshot/log protocol.

## Current Behavior {#current-behavior}

The relevant defaults are:

```text
S3 connect timeout              1 second
S3 send/receive request timeout 30 seconds
S3 automatic retries            500 (up to 501 HTTP attempts)
mount lease TTL                 30 seconds
mount renewal period            10 seconds
```

`S3::DEFAULT_CONNECT_TIMEOUT_MS`, `S3::DEFAULT_REQUEST_TIMEOUT_MS`, and
`S3::DEFAULT_RETRY_ATTEMPTS` define the S3 defaults. `PoolConfig::mount_lease_ttl_ms` and
`PoolConfig::mount_renew_period` define the mount defaults.

`PocoHTTPClient` applies `request_timeout_ms` as socket send and receive timeouts. It is not one strict
wall-clock deadline for the complete logical S3 operation. Adaptive S3 timeouts may further select the
per-attempt values. The SDK retry strategy can then start additional HTTP attempts.

CAS conditional writes are issued through `ObjectStorageBackend::nativeConditionalPut`. The
`If-None-Match` or `If-Match` condition is attached to `WriteSettings`, and the final result is observed
when the write buffer is finalized. The current conditional-write settings do not establish a
CAS-specific retry policy.

The mount lease itself is protected by a conditional overwrite of its previous token. Successful
renewal extends the local monotonic deadline; a renewal conflict marks the local mount fence lost.
Ordinary ref-shard mutation checks `Store::mayMutate` before its CAS loop, but the S3 write and the
mount-lease token are two different objects and are not updated atomically.

## Why A Request Timeout Is Not Cancellation {#why-a-request-timeout-is-not-cancellation}

The following result is valid from the client's point of view:

```text
client sends the complete PUT request
S3 accepts or begins applying it
the response is lost or delayed
the client receive timeout expires
the object exists, or becomes visible after the client stopped waiting
```

The timeout proves only that ClickHouse did not receive a conclusive response within its wait. It does
not prove that S3 rejected or cancelled the request. Closing the connection does not provide a storage
protocol guarantee that the object will never appear.

Consequently, even a request timeout shorter than the mount lease TTL is only an operational risk
reduction. It is not a correctness fence. The current equal 30-second request timeout and lease TTL do
not even provide that operational margin.

## Why Transparent Retries Are Dangerous {#why-transparent-retries-are-dangerous}

### Conditional Create {#conditional-create}

For `putIfAbsent`, an internal retry can produce:

```text
attempt 1 creates the object
attempt 1 response is lost
attempt 2 repeats If-None-Match: *
attempt 2 receives PreconditionFailed
```

`PreconditionFailed` no longer proves that a foreign writer created the object. The first attempt may
have created the exact intended bytes. CAS must resolve the exact key and deterministic body before it
classifies the operation.

### Conditional Overwrite {#conditional-overwrite}

For `putOverwrite` or `casPut`, an internal retry can produce:

```text
attempt 1 replaces token A with the intended object at token B
attempt 1 response is lost
attempt 2 repeats If-Match: A
attempt 2 receives PreconditionFailed
```

Again, the condition failure does not identify whether attempt 1 committed or another writer won. A
later overwrite may also have replaced the intended bytes, so not every uncertain conditional
overwrite can be resolved as success. Ambiguity must fail closed when the exact intended state can no
longer be proven.

### Lease Crossing {#lease-crossing}

An SDK retry can start after the operation's original lease check, after the local deadline, or after a
renewal conflict. That retry is invisible to the CAS state machine. A retry policy that is unaware of
the mount fence therefore defeats the purpose of checking the fence before mutation.

## Operation Classes {#operation-classes}

Every CAS backend operation is assigned one explicit class. The class determines who may retry and how
an uncertain result is resolved.

| Class | Examples | Default retry owner | Uncertain-result rule |
|---|---|---|---|
| Read-only | exact `GET`, `HEAD` | CAS bounded retry loop | Retry within the operation budget |
| Ordered enumeration | paginated `LIST` | CAS pagination loop | Resume only with the returned continuation token; restart the complete scan if its protocol requires one view |
| Immutable conditional create | ref log, snapshot, manifest, content-addressed blob | CAS | Exact-key observation; identical deterministic content may prove success |
| Tokened conditional overwrite | mount lease, epoch allocator, mutable GC state, existing ref shard | CAS | Prove the intended new state and token, otherwise return uncertain or conflict |
| Idempotent exact delete | obsolete immutable object | CAS cleanup loop | Repeated absence is success; never broaden the deletion target |
| Batch delete | exact cleanup-key batch | CAS cleanup loop | Inspect every per-key result and retry only failed exact keys |

No protocol-significant operation inherits an unexamined generic S3 retry policy.

## Required Timeout Model {#required-timeout-model}

CAS uses three separate limits:

```text
attempt_timeout      maximum client wait for one HTTP attempt
operation_deadline   maximum wall-clock time for the complete logical CAS operation
lease_deadline       local monotonic deadline established by successful mount renewal
```

They must not be represented by one `request_timeout_ms` value.

Before a writer starts a protocol-significant attempt, it requires:

```text
mount fence is not lost
now < lease_deadline
now + attempt_timeout + lease_safety_margin < lease_deadline
now + attempt_timeout <= operation_deadline
```

If these conditions do not hold, the attempt is not sent. The caller receives a mount-lost or bounded
timeout exception and may restart under a valid writer epoch.

`lease_safety_margin` covers local scheduling delay, timeout granularity, and configured clock-skew
allowance. It does not claim to bound server-side completion after a client timeout.

The final values must be selected from measured S3 latency and failure injection. As an initial
configuration invariant, `attempt_timeout + lease_safety_margin` must be strictly less than the mount
lease TTL. The current 30-second attempt timeout with a 30-second lease TTL is rejected for
protocol-significant writer operations.

## Required Retry Policy {#required-retry-policy}

### Disable Transparent Conditional-Write Retries {#disable-transparent-conditional-write-retries}

The underlying S3 client performs exactly one HTTP attempt for `If-None-Match` and `If-Match` writes.
If the shared client cannot provide a request-scoped retry override, CAS uses a client or execution path
whose retry strategy is disabled for these operations. Setting a process-wide retry count is not
acceptable if it changes unrelated ClickHouse S3 workloads.

### Retry Only In CAS Code {#retry-only-in-cas-code}

A CAS retry is a visible state-machine transition. Before every retry CAS:

1. records the preceding attempt and its exact outcome;
2. checks the operation deadline;
3. checks the local mount fence and remaining lease budget for writer operations;
4. determines whether exact-key resolution is required before another write;
5. preserves the same deterministic key, body, conditions, and writer epoch unless the protocol
   explicitly begins a new logical operation.

No retry crosses a writer epoch. No retry begins after `Store::mayMutate` becomes false.

### Resolve Before Reissuing {#resolve-before-reissuing}

After an uncertain immutable conditional create, CAS first observes the exact key:

- identical deterministic bytes mean the create committed;
- a different valid object means a real conflict;
- absence means another create attempt may be considered if all time and lease checks still pass;
- a read error or an object whose identity cannot be proven leaves the result uncertain.

For a large content-addressed object, identity may be proven from its canonical digest and trusted
object metadata without downloading the complete body only where the existing format proves that this
is equivalent. This exception must be specified per format; it is not a generic fallback.

After an uncertain tokened overwrite, CAS reads the exact object and token. It reports success only if
the operation-specific protocol proves that the observed state is exactly the intended committed
successor. Otherwise it reports conflict or uncertainty and performs no destructive fallback.

### Bound Read Retries Too {#bound-read-retries-too}

Read-only operations may retry transient failures, but both attempt count and total wall-clock duration
are bounded. A `GC` round or writer startup must not remain alive for hundreds of hidden retries. When
the budget is exhausted, the operation fails closed and the higher-level round or recovery attempt is
retried later.

## ACK And Cache Rules {#ack-and-cache-rules}

For a writer mutation:

```text
check the local mount fence and remaining attempt budget
perform one controlled S3 attempt
resolve an uncertain result if the protocol allows it
check the local mount fence again
only then update the writer cache and return ACK
```

If the final fence check fails, the writer does not update its cache and does not return ACK. If the S3
object may exist, the result is reported as uncertain, not as a definite non-commit.

This rule guarantees that an observed local fence loss prevents an ACK. It does not atomically prove
that the remote mount object was still owned at the instant another S3 object was created. The ref
snapshot/log RFC therefore continues to document late predecessor completion as an unresolved
cross-epoch corner case.

## Configuration {#configuration}

CAS-specific settings are required rather than relying silently on general S3 defaults. Exact names
may follow the final configuration hierarchy, but the model contains at least:

```text
cas_s3_connect_timeout_ms
cas_s3_attempt_timeout_ms
cas_s3_operation_timeout_ms
cas_s3_read_retry_attempts
cas_s3_conditional_write_retry_attempts = 0
cas_s3_retry_initial_backoff_ms
cas_s3_retry_max_backoff_ms
cas_mount_lease_safety_margin_ms
```

Startup validates the relationships among attempt timeout, operation timeout, safety margin, renewal
period, and lease TTL. Invalid writer settings fail before the mount becomes writable. They do not
silently fall back to generic S3 defaults.

The implementation may use fewer public settings by deriving safe values from the lease TTL. It must
still expose the effective values through logs and inspection output.

## Observability {#observability}

Counters and logs are separated by operation class and backend outcome:

- HTTP attempts started, completed, timed out, and cancelled locally;
- SDK-level retries, which must remain zero for conditional writes;
- CAS-controlled retries;
- exact-key resolutions yielding identical, absent, conflicting, or unreadable objects;
- attempts refused because the mount fence was lost;
- attempts refused because the remaining lease budget was insufficient;
- operations whose response arrived after the local lease deadline;
- uncertain results returned to callers;
- total attempt and logical-operation latency histograms;
- effective timeout, retry, lease TTL, renewal, and safety-margin configuration.

A response observed after the local fence is a diagnostic signal. Its absence cannot prove that no
request completed unobserved on the storage side.

## Failure-Injection Tests {#failure-injection-tests}

The test backend and RustFS integration tests cover at least:

- request rejected before any bytes are accepted;
- timeout while sending the body;
- complete body accepted, object created, response lost;
- complete body accepted, response delayed beyond the client timeout;
- first `If-None-Match` attempt commits and a hypothetical retry would receive
  `PreconditionFailed`;
- first `If-Match` attempt commits and a hypothetical retry would use the stale token;
- exact-key resolution finds identical bytes, different bytes, absence, and a read error;
- local mount fence expires before an attempt starts;
- renewal fails while an attempt is in flight;
- response succeeds after the local deadline and no ACK is returned;
- retry budget would cross the lease deadline and the retry is suppressed;
- read and `LIST` retry budgets terminate without authorizing destructive `GC` work;
- generic non-CAS S3 operations retain their separately configured retry behavior.

Tests assert both the final protocol state and exact request counts. A test must fail if a conditional
write performs an invisible SDK retry.

## Implementation Plan {#implementation-plan}

1. Add request-attempt instrumentation around the S3 client path used by
   `ObjectStorageBackend::nativeConditionalPut`.
2. Provide a request-scoped no-retry path for conditional creates and overwrites without changing
   unrelated S3 disks.
3. Add a CAS retry controller with an operation deadline, attempt budget, mount-fence callback, and
   explicit outcome classification.
4. Route `putIfAbsent`, `putOverwrite`, and `casPut` through that controller.
5. Add exact-key resolution for deterministic immutable creates and operation-specific resolution for
   tokened overwrites.
6. Check the local mount fence immediately before an attempt and after its resolved completion, before
   cache installation and ACK.
7. Bound read, `HEAD`, and paginated `LIST` retries used by writer recovery and `GC`.
8. Validate effective timeout and lease relationships during writable startup.
9. Add metrics, fault injection, and RustFS coverage before reducing or removing the existing generic
   behavior.

## Decisions {#decisions}

- A socket request timeout is not proof that an S3 write did not commit.
- The complete logical CAS operation has its own wall-clock deadline.
- The S3 client performs no transparent retry for conditional CAS writes.
- CAS owns every retry of a protocol-significant operation.
- Every writer attempt is gated by both the local mount fence and a remaining-lease budget.
- Uncertain immutable creates are resolved from the exact key and deterministic identity before any
  reissue.
- Tokened overwrite ambiguity fails closed unless the intended successor state is proven.
- No retry crosses a writer epoch.
- Cache mutation and ACK occur only after outcome resolution and a final local fence check.
- Timeout control reduces but does not eliminate the late-predecessor S3 completion corner case.
- Keeper-based commit or another cross-object fencing mechanism remains a separate possible solution
  for strict cross-epoch authority.
