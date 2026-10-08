---
description: 'External design study of the CAS mount lease settings: how comparable systems expose lease and timeout settings, an evaluation of options A to E of the settings notes, a recommendation (a fixed tick with holder-side reserves and an explicit paused-write state), and corrections to the notes.'
sidebar_label: 'Mount lease settings: external study'
sidebar_position: 106
slug: /superpowers/cas/mount-lease-settings-external-study
title: 'CAS mount lease settings: external study'
doc_type: 'guide'
---

# CAS mount lease settings: external study {#mount-lease-settings-external-study}

Answer of an external reviewer (`codex`, model `gpt-6.1-sol`, high reasoning effort, with web search) to the
question: study [the settings notes](../specs/2026-10-02-cas-mount-lease-settings-ux-notes.md) and
[the three studies](2026-10-02-mount-lease-settings-studies.md), compare with how similar systems are
configured, weigh options A to E, and recommend one or propose another. The text below is the answer as
given, with headings and links adjusted. It read code at commit `e1c38ca7478`; it built and ran nothing.

## Part 1 {#part-1}

I read both documents completely before inspecting code. Code checks used commit e1c38ca7478 through `git show` in the specified checkout. I made no filesystem changes, builds, or test runs.

The external comparisons below use live documentation or source. Recommendations and deductions are identified as such. The newer S3 measurements are supplied evidence; I did not independently inspect their raw samples.

### Comparable lease systems {#comparable-lease-systems}

### ZooKeeper {#zookeeper}

| Control | Meaning to an operator | Relation or derivation |
|---|---|---|
| `tickTime` | Base time unit for server heartbeats and timeout accounting | Required configuration; the guide’s example uses 2000 ms |
| `initLimit` | How many ticks a follower gets to connect and synchronize with its leader | Initial synchronization window = `initLimit × tickTime` |
| `syncLimit` | How many ticks a follower may fall behind communication with its leader | Ongoing synchronization window = `syncLimit × tickTime` |
| `minSessionTimeout` | Smallest session timeout a client may negotiate | Defaults to `2 × tickTime` |
| `maxSessionTimeout` | Largest session timeout a client may negotiate | Defaults to `20 × tickTime` |
| Client-requested session timeout | How long the ensemble should preserve this client’s session without hearing from it | Negotiated within the server’s bounds |

The important distinction is that `initLimit` and `syncLimit` concern ensemble members. They are not the client’s renewal and write-stop settings. ZooKeeper combines a base unit with independently negotiated sessions; it does not impose one universal three-stage lease profile. [Administrator’s guide](https://zookeeper.apache.org/doc/current/zookeeperAdmin.html)

For negotiated session timeout S, the Java client sets its read timeout to approximately `2S/3`. Its idle ping target is approximately `S/3`, with implementation adjustments. A broken connection can produce `Disconnected` earlier than `2S/3`. The fractions measure different idle intervals; they are not three exact instants on one common stopwatch. [Java client source](https://raw.githubusercontent.com/apache/zookeeper/master/zookeeper-server/src/main/java/org/apache/zookeeper/ClientCnxn.java)

`Disconnected` means communication is lost and the client tries to reconnect. The ensemble may still preserve the session and its ephemeral nodes. `Expired` means the session has ended. A disconnected client may learn that only after reconnecting. Applications relying on session ownership must handle uncertainty during disconnection. This is a useful naming precedent for CAS, but disconnection itself is not a universal prohibition on every application action. [Programmer’s guide](https://zookeeper.apache.org/doc/current/zookeeperProgrammers.html)

### ClickHouse Keeper {#clickhouse-keeper}

Keeper exposes separate durations rather than ZooKeeper’s single base unit.

| Setting | Operator meaning | Current upstream default |
|---|---|---:|
| `min_session_timeout_ms` | Smallest client session timeout | 10000 |
| `session_timeout_ms` | Largest client session timeout, despite its shorter name | 100000 |
| `operation_timeout_ms` | Default operation timeout | 10000 |
| `dead_session_check_period_ms` | How often the leader checks for expired sessions | 500 |
| `heart_beat_interval_ms` | Heartbeat interval between quorum members | 500 |
| `election_timeout_lower_bound_ms` | Lower bound of the election timer | 1000 |
| `election_timeout_upper_bound_ms` | Upper bound of the election timer | 2000 |
| `leadership_expiry_ms` | Expire leadership after missing peer responses | 0; when set, should not exceed the lower election bound |

These are distinct session, operation, and consensus controls. Session removal also has checking and processing latency. Client notifications depend on the client library; a ZooKeeper Java client uses the behavior above. There is no CAS-style write-reserve state defined by this settings table. [Settings source](https://raw.githubusercontent.com/ClickHouse/ClickHouse/master/src/Coordination/CoordinationSettings.cpp), [default constants](https://raw.githubusercontent.com/ClickHouse/ClickHouse/master/src/Common/ZooKeeper/ZooKeeperConstants.h)

### etcd {#etcd}

etcd has two separate timing families.

| Family | Controls | Meaning and relations |
|---|---|---|
| Raft | `--heartbeat-interval`, default 100 ms; `--election-timeout`, default 1000 ms | Leader-to-follower heartbeat cadence and follower failure-detection time |
| Application leases | Requested TTL, granted TTL, keep-alive | Lifetime of keys attached to a lease; renewal maintains those keys |

The tuning guide recommends heartbeat intervals around member RTT and election timeout at least ten times RTT. It requires consistent heartbeat and election settings across members. These recommendations should not be translated into a fixed relation between Raft election timeout and an application lease TTL. [Tuning guide](https://etcd.io/docs/v3.6/tuning/)

The Go client schedules the next keep-alive at approximately one third of the returned TTL. It sets a local response deadline from that TTL and closes keep-alive channels when renewal cannot be maintained. Server lease expiration removes attached keys. [Lease API](https://etcd.io/docs/v3.6/learning/api/), [Go client source](https://github.com/etcd-io/etcd/blob/main/client/v3/lease.go)

The cited API does not define a `Jeopardy` state. Channel closure signals lost renewal assurance. An application must arrange its own response; a lease on an etcd key does not automatically fence writes to another storage system.

### Kubernetes client-go leader election {#kubernetes-client-go-leader-election}

| Control | Operator meaning | Typical core-client default |
|---|---|---:|
| `LeaseDuration` | How long a contender waits without observing a changed leader record before attempting takeover | 15 s |
| `RenewDeadline` | How long the current leader retries renewal before giving up | 10 s |
| `RetryPeriod` | Delay between attempts | 2 s |

The constructor requires:

`LeaseDuration > RenewDeadline > 1.2 × RetryPeriod`

The holder retries renewal, then cancels leadership and invokes `OnStoppedLeading` when renewal fails. Contenders wait using their own observations and clocks. The ratio `LeaseDuration / RenewDeadline` controls approximate tolerance to clock-rate differences. The package explicitly does not guarantee fencing. Its intermediate distinction is “leader stopped locally; contenders still waiting,” rather than a named grace state. [Source and configuration documentation](https://github.com/kubernetes/client-go/blob/master/tools/leaderelection/leaderelection.go)

This is the closest precedent for naming three outcomes directly. Its authority guarantees are weaker than CAS requires.

### Chubby {#chubby}

The published design describes a default lease extension of 12 seconds. The master can lengthen it under load. A client immediately starts another `KeepAlive` after receiving the previous response; the master normally holds that request until renewal or event delivery is due.

The client maintains a conservative local approximation of the master’s lease. When that approximation expires, it disables its cache and enters `jeopardy`. Application calls block, and the application can quiesce. A successful exchange produces `safe`; failure to recover during the grace period, default 45 seconds, produces `expired`.

The grace period preserves the possibility of session recovery. It does not authorize continued protected work. The paper describes these protocol parameters, not a complete public operator settings interface. [Chubby paper, sections 2.8–2.9](https://storage.googleapis.com/gweb-research2023-media/pubtools/4444.pdf)

### Consul sessions {#consul-sessions}

| Control | Meaning |
|---|---|
| Session TTL | Renew the session before this interval passes |
| Associated health checks | Additional causes of session invalidation |
| `lock-delay`, default 15 s | Prevent reacquisition of released locks for a period after session invalidation |
| Session behavior | Release associated locks or delete their keys upon invalidation |

Consul documents TTL as a lower bound on expiration. It may expire a session later, including extensions around leader failover. Clients should renew conservatively before TTL. `lock-delay` ranges from zero to 60 seconds. It gives an old holder time to notice invalidation and stop.

This delay resembles option C, but Consul explicitly describes advisory locking and calls the delay a mitigation rather than a complete safety guarantee. There is no fixed holder stop instant derived from these settings. [Session design](https://developer.hashicorp.com/consul/docs/automate/session), [session API](https://developer.hashicorp.com/consul/api-docs/session)

### Kafka {#kafka}

For Kafka 4.1’s Classic consumer protocol:

| Setting | Operator meaning | Default |
|---|---|---:|
| `session.timeout.ms` | Broker removes a consumer after this long without heartbeats | 45000 |
| `heartbeat.interval.ms` | Consumer heartbeat cadence | 3000 |
| `max.poll.interval.ms` | Maximum gap between application polls | 300000 |

Heartbeat interval must be below session timeout; the recommended ceiling is one third of it. Session timeout must fit the broker’s configured minimum and maximum.

Heartbeats can continue while application processing stalls. `max.poll.interval.ms` detects that separate failure. For a static member, reaching the poll limit stops heartbeats; reassignment waits for session expiration. Under the newer Consumer protocol, the broker controls heartbeat and session timing instead.

The operator sees poll timeout, lost membership, or rebalancing. There is no general lease-reserve state. [Consumer configuration](https://kafka.apache.org/41/generated/consumer_config.html)

### HDFS leases {#hdfs-leases}

HDFS distinguishes two recovery thresholds:

- Soft limit: another writer can trigger recovery of an unrenewed lease.
- Hard limit: the NameNode initiates recovery without waiting for another writer.

The traditional soft limit is 60 seconds. Older code used a one-hour hard limit. Current upstream configuration exposes `dfs.namenode.lease-hard-limit-sec`, default 1200 seconds. These defaults must be identified by version. [Historical constants](https://github.com/apache/hadoop-common/blob/trunk/hadoop-hdfs-project/hadoop-hdfs/src/main/java/org/apache/hadoop/hdfs/protocol/HdfsConstants.java), [current configuration](https://raw.githubusercontent.com/apache/hadoop/trunk/hadoop-hdfs-project/hadoop-hdfs/src/main/resources/hdfs-default.xml)

The client renews leases for open files periodically. Recovery changes block generation stamps and reconciles block lengths. The interval between soft and hard limits is a recovery-policy distinction, not a guaranteed period during which the old holder has stopped. The relevant state is lease or block recovery. [Client documentation](https://hadoop.apache.org/docs/current/hadoop-project-dist/hadoop-hdfs-client/build/source/hadoop-hdfs-project/hadoop-hdfs-client/target/api/org/apache/hadoop/hdfs/client/impl/LeaseRenewer.html), [recovery source](https://raw.githubusercontent.com/apache/hadoop/trunk/hadoop-hdfs-project/hadoop-hdfs/src/main/java/org/apache/hadoop/hdfs/server/namenode/LeaseManager.java)

### Amazon DynamoDB lock client {#amazon-dynamodb-lock-client}

`leaseDuration`, default 20 seconds, specifies lock lifetime. `heartbeatPeriod`, default five seconds, controls background renewal. With background heartbeats enabled, construction requires:

`leaseDuration ≥ 2 × heartbeatPeriod`

A contender observes an unchanged record version for the recorded lease duration, then conditionally acquires it. The holder timestamps successful updates from before the request. This avoids using another machine’s wall clock. Unlike frozen CAS, the record carries lease duration. [Defaults](https://raw.githubusercontent.com/awslabs/amazon-dynamodb-lock-client/master/src/main/java/com/amazonaws/services/dynamodbv2/AmazonDynamoDBLockClientOptions.java), [implementation](https://raw.githubusercontent.com/awslabs/amazon-dynamodb-lock-client/master/src/main/java/com/amazonaws/services/dynamodbv2/AmazonDynamoDBLockClient.java)

An optional session monitor names the intermediate state “danger zone.” Its safe time must be strictly greater than heartbeat period and strictly less than lease duration. It invokes a callback; application behavior remains the caller’s responsibility. [Monitor documentation](https://github.com/awslabs/amazon-dynamodb-lock-client/blob/master/src/main/java/com/amazonaws/services/dynamodbv2/SessionMonitor.java)

### Redis Redlock {#redis-redlock}

The client chooses lock TTL and short timeouts for contacting individual Redis instances. It accepts acquisition only after obtaining a majority sufficiently quickly.

The documented minimum validity is:

`TTL − acquisition elapsed time − CLOCK_DRIFT`

The holder must finish protected work within that remaining validity. Redis instances can retain their keys after the holder’s usable validity ends. The documentation does not give that interval a named state or prescribe a universal drift percentage.

This is a direct precedent for subtracting consumed time and a safety allowance from apparent lease duration. It is not evidence that CAS’s particular reserve is sufficient. [Distributed-lock documentation](https://redis.io/docs/latest/develop/clients/patterns/distributed-locks/)

### Object-store table locking: Apache Iceberg {#object-store-table-locking-apache-iceberg}

Iceberg’s optional lock-manager settings include:

| Setting | Default | Meaning |
|---|---:|---|
| `lock.acquire-interval-ms` | 5000 | Delay between acquisition attempts |
| `lock.acquire-timeout-ms` | 180000 | Total acquisition retry window |
| `lock.heartbeat-interval-ms` | 3000 | Heartbeat cadence after acquisition |
| `lock.heartbeat-timeout-ms` | 15000 | Silence after which a lock is considered expired |

These apply to selected catalog implementations, not every Iceberg deployment. [Catalog properties](https://iceberg.apache.org/docs/latest/catalog-properties/)

The DynamoDB implementation stores lease duration and version, waits the observed duration, and conditionally replaces an unchanged version. Heartbeats conditionally replace the owner’s record. The examined implementation has no separate named holder write-stop state. Its stored timing makes it unsuitable as a direct compatibility model for CAS’s frozen format. [Versioned implementation](https://raw.githubusercontent.com/apache/iceberg/apache-iceberg-1.10.0/aws/src/main/java/org/apache/iceberg/aws/dynamodb/DynamoDbLockManager.java)

S3 itself supplies conditional creation and replacement through `If-None-Match` and `If-Match`. These protect an individual object. They do not establish a lease deadline across other objects. [S3 conditional-write documentation](https://docs.aws.amazon.com/AmazonS3/latest/userguide/conditional-writes.html)

### Request timeouts {#request-timeouts}

The general distinction is between an inactivity or phase limit and a deadline covering the entire attempt. Short phase limits help detect stalls. They do not replace the latter.

| System | Phase or inactivity controls | Whole-attempt or whole-call control | Documented advice or limitation |
|---|---|---|---|
| AWS Java SDK v2 | Connection, socket, read, write, TLS, connection-acquisition timeouts | `apiCallAttemptTimeout` covers one attempt; `apiCallTimeout` covers the API call, retries and retry waits | Configure both API limits; HTTP limits should fit inside attempt timeout, which fits inside call timeout |
| AWS C++ SDK | `requestTimeoutMs` is transport-dependent: socket read timeout on Windows; low-speed interval for curl | `httpRequestTimeoutMs` maps to curl’s total HTTP-transfer timeout | Total timeout is curl-specific and defaults to zero; it does not bound an entire retried SDK operation |
| gRPC | Transport controls are separate | RPC deadline | Explicitly set a realistic deadline; server work must cooperate with cancellation |
| Go HTTP | Dial, TLS handshake, response-header, and other transport limits | `http.Client.Timeout` covers connection, redirects and response-body reading; request context can supply a deadline | A zero client timeout is unlimited; response-header timeout excludes body reading |
| curl | Connect timeout; low-speed limit and duration | `CURLOPT_TIMEOUT` or `CURLOPT_TIMEOUT_MS` covers the complete transfer | Low-speed controls suit variable-duration transfers; a hard total timeout is needed for a hard time budget |

Sources: [AWS Java timeout hierarchy](https://docs.aws.amazon.com/sdk-for-java/latest/developer-guide/timeouts.html), [AWS C++ configuration source](https://raw.githubusercontent.com/aws/aws-sdk-cpp/main/src/aws-cpp-sdk-core/include/aws/core/client/ClientConfiguration.h), [gRPC deadlines](https://grpc.io/docs/guides/deadlines/), [Go HTTP API](https://pkg.go.dev/net/http), [curl total timeout](https://curl.se/libcurl/c/CURLOPT_TIMEOUT.html), [curl low-speed timeout](https://curl.se/libcurl/c/CURLOPT_LOW_SPEED_TIME.html).

Several distinctions matter for CAS:

- Not every transport timeout is literally a per-socket-operation timeout. A TLS-handshake timeout or response-header timeout can cover an entire phase.
- A deadline must continue through response-body consumption. Returning an HTTP response object does not mean the request is finished.
- An attempt deadline must cover hidden work: credentials, DNS, throttling, scheduling, redirects and reconnects.
- Cancellation bounds client participation. It does not establish that a remotely accepted mutation cannot subsequently apply.

The last point follows from the distinction between client cancellation and server execution. gRPC explicitly requires server application cooperation. S3’s conditional-write documentation supplies object preconditions, not client-deadline enforcement. Neither source proves a remote-completion bound for CAS.

## Part 2 {#part-2}

The following evaluation treats A–E as described in the notes. “No protocol change” means unchanged inter-server timing rules and object representation. Configuration resolution and local admission can still change.

### A. Keep the settings and arithmetic {#a-keep-the-settings-and-arithmetic}

Advantages:
- Smallest implementation.
- Preserves existing configuration and observer behavior.
- Reporting can improve immediately.

Drawbacks:
- Four controls still determine an outcome none names.
- Changes to request timeout alter write availability indirectly.
- The 14-second admission cutoff remains surprising.

Implementation:
- Mixed versions are compatible when effective lease and renewal values agree.
- Visibility improvements do not depend on CAS-343.
- The safety interpretation of the envelope still depends on CAS-343 and the late-write argument.
- Required verification concerns boundary reporting, refusal explanations, configuration errors, and terminal-state precedence.

Operator understanding:
- The settings alone remain insufficient.
- An effective-timing table and mount log can make the configuration understandable after resolution.

Closest precedent:
- Keeper’s separate settings, with an additional request reserve operators must understand.
- A is the correct immediate reporting step, but a weak final interface.

### B. Reserve one envelope {#b-reserve-one-envelope}

Advantages:
- Conditional-write admission moves from 14 to 21 seconds after the confirmed anchor.
- Lease length and observer thresholds stay unchanged.
- Removes a reserve whose settlement-read purpose is not intrinsically a safety requirement.

Drawbacks:
- Does not improve setting names.
- Adds ambiguous outcomes and wedged lanes near the cutoff.
- Reduces incidental protection against an overlong attempt.

Implementation:
- No inter-server timing change if the request bound and late-write argument hold.
- Existing observers need no new threshold.
- CAS-343 must precede the reduction.
- The study establishes a strong ref-ledger argument, but explicitly leaves non-ledger keys and higher callers incomplete.
- Needed verification includes identical give-up outcomes, wedge recovery, delayed landing, checkpoint and blob-metadata paths, takeover, and shutdown.

Operator understanding:
- The hidden arithmetic remains.
- “More outage tolerance” does not mean successful writes during a total storage outage.

Closest precedent:
- Redlock’s usable validity after subtracting necessary work.
- B should be considered an availability optimization, independently of the interface decision.

### C. Put the reserve on the observer {#c-put-the-reserve-on-the-observer}

Advantages:
- The advertised write-stop duration can equal the configured lease number.
- Moves the visible cost toward takeover latency.

Drawbacks:
- Changes the timing protocol.
- Observers must know the maximum consequential duration of another holder’s requests.
- Request budgets currently need not match across the pool.
- It conflicts with the stated constraint if writes start until the holder’s deadline but may finish afterward.

Implementation:
- Either the holder’s authority deadline must be extended, or the admission constraint must explicitly be abandoned.
- Extending authority turns C into something close to D, with extra observer machinery.
- The quoted 43.5 seconds is not a complete specification: which reserve moves, and how drift applies to it, must be defined.
- CAS-343 is necessary.
- Models must include request duration, both clocks, delayed application, observer sampling and mixed configurations.
- Old observers cannot safely coexist with writers relying on a newly extended protected interval.
- Use a stopped-pool transition, or a fully proven observer-first rollout.

Operator understanding:
- One number looks clearer.
- Actual takeover still depends on additional hidden timing and observer knowledge.

Closest precedent:
- Consul’s lock delay.
- Consul’s mitigation-level guarantee is insufficient for CAS’s safety requirement.

I would reject C under the present constraints.

### D. Name the write-stop outcome {#d-name-the-write-stop-outcome}

Advantages:
- The configured value describes a useful operator outcome.
- The reserve remains holder-side.
- The timing protocol can remain unchanged.

Drawbacks:
- Derived lease length depends on request settings.
- Two servers agreeing on the named write-stop value can still derive different lease lengths.
- Changing a supposedly local request timeout can become a stopped-pool change.

Implementation:
- Define one configuration family and reject contradictory settings.
- Compare effective lease and renewal values, not just the new setting.
- CAS-343 is needed for a trustworthy completion promise, but a rename preserving existing arithmetic can precede it.
- Test configuration resolution, legacy equivalence, mixed versions, and request-setting changes.
- No new timing model is needed solely for renaming; altered defaults need the same safety checks as B or E.

Operator understanding:
- Better for predicting the local admission cutoff.
- Worse for understanding why changing an HTTP timeout changes takeover.

Closest precedent:
- Kubernetes’s outcome-oriented duration names.
- Kubernetes keeps its three durations explicit. D hides one of them through derivation.

### E. One tick {#e-one-tick}

Advantages:
- Best matches the owner’s preferred model.
- Fixed multipliers give predictable renewal, admission-stop and expiration times.
- At a five-second tick, the existing 30-second lease and ten-second cadence can remain.
- One pool-wide value replaces independently interacting settings.

Drawbacks:
- Couples request tolerance, renewal traffic, outage allowance and takeover time.
- A request-latency problem can require changing the whole pool’s lease.
- Optional multiplier settings would recreate the current complexity.
- The proposed shorter request limits need evidence beyond one S3 deployment.

Implementation:
- Keep fixed multipliers initially.
- CAS-343 and the delayed-write audit must precede the later admission cutoff.
- With unchanged effective lease and renewal values, old observers and new holders can coexist.
- A different tick changes those effective values and requires a stopped pool.
- Needed verification includes request deadlines, all mutation admission paths, rounding, overflow, observation thresholds, recovery, reporting, configuration compatibility, and storage faults.

Operator understanding:
- Strongest if the setting description states the multipliers.
- “Tick” alone explains nothing; “renew at two, pause writes at four, expire at six” does.
- The setting promises admission eligibility, not successful storage service.

Closest precedent:
- ZooKeeper’s base unit, combined with explicit outcomes.
- CAS would intentionally use a simpler, more rigid profile than ZooKeeper.

## Part 3 {#part-3}

I recommend E with fixed multipliers, explicit paused-write reporting, and two request reservations retained.

First ship the visibility improvements under A. Do not wait for the complete tick implementation to make current behavior understandable.

Do not combine the initial tick release with B. The proposed 20-second cutoff already improves the operator outcome while retaining settlement room. The newer measurements do not establish that saving another request reservation is necessary.

### Proposed operator settings {#proposed-operator-settings}

For the eventual tick profile:

| Setting | Default | Description as it should appear in the settings table |
|---|---:|---|
| `cas_tick_ms` | 5000 | Base duration for CAS mount timing: renew every two ticks, pause new mount mutations four ticks after the confirmed renewal anchor, and end local lease authority after six ticks; use the same value throughout the pool and change it only with every pool member stopped. |

No configurable tick multipliers initially. No additional renew-deadline setting. No new timing fields in stored objects.

During migration, explicit legacy timing settings select the legacy profile. The five-second tick default applies when neither timing family is explicitly configured. This distinction must appear in the documentation and effective-settings log.

The settings describe admission. An outage can make requests fail before the admission cutoff.

### Derived quantities {#derived-quantities}

Let T be `cas_tick_ms`. All quantities below are local durations in milliseconds.

| Quantity | Formula | Default |
|---|---|---:|
| Lease authority duration L | `6T` | 30000 |
| Renewal cadence P | `2T` | 10000 |
| Whole-attempt deadline budget B | `floor(4T/5)` | 4000 |
| Socket send/receive limit S | `floor(3T/5)` | 3000 |
| TCP and TLS phase cap C | `floor(T/10)` | 500 |
| Scheduling margin M | `2T − 2B` | 2000 |
| Mutation reserve R | `2B + M = 2T` | 10000 |
| New-mutation admission cutoff W | `L − R = 4T` | 20000 |
| Time from scheduled renewal to cutoff | `W − P = 2T` | 10000 |
| Restart observation cadence | `max(1, floor(P/2)) = T` | 5000 |
| Restart observation threshold | `L + floor(L/20) + T` | 36500 |
| GC observation threshold | `L + floor(L/20) + P` | 41500 |
| Nominal renewal retry spacing | `floor(T/5)` | 1000 |

The disk’s smaller connect limit may remain effective. A zero disk connect timeout must not disable the tick profile’s cap.

I would use the derived phase limits consistently in tick mode instead of inheriting unrelated adaptive timeout numbers. That is a proposed transport-policy change. It needs comparison against the current adaptive first-attempt behavior, particularly fast failed reads.

Keep existing retry jitter, cancellation, and immediate settlement behavior. Retry spacing is a target between eligible attempts, not an upper bound on request rate or recovery latency.

For confirmed logical renewal anchor a:

- Local authority deadline: `a + L`.
- New-mutation cutoff: `a + W`.
- Admit a mutation only while `remaining lease > R`.
- Reserve any planned pause in addition to the required requests.
- Permit a settlement read only while its own `B + M` reservation fits.
- Recheck authority and generation before acknowledging an outcome.

The anchor is the start of the confirmed logical renewal, including its retries. It is not the response time or the last physical retry’s start.

The whole-attempt timer must start before potentially blocking preparation. It must cover signing, credentials, DNS, scheduling, throttling, connection setup, redirects, request transmission and response consumption. Internal continuation must not restart it.

### Uniform mutation cutoff {#uniform-mutation-cutoff}

The current implementation has different reservations for different operations. To make “writes paused” truthful, tick mode should stop every new authoritative mount mutation at the same four-tick cutoff.

This includes conditional writes and mutation paths currently using one reservation. Settlement reads for an earlier attempt remain separately budgeted. Renewal remains outside the expired mount’s admission gate.

This is a deliberate local admission change. It needs an audit of operation classification. It slightly reduces the late window of cheaper operations in exchange for one understandable state.

### Validation in operator words {#validation-in-operator-words}

The errors should explain these rules:

1. “The tick must be at least 10 milliseconds, so every derived request phase has a positive duration.”
2. “The configured durations must fit the supported timer and integer ranges.”
3. “Choose either `cas_tick_ms` or the legacy timing settings; do not configure both.”
4. “Every server and GC participant in this pool must use the same effective lease duration and renewal cadence.”
5. “Changing either effective duration requires stopping the whole pool.”
6. “The request deadline must cover the entire attempt; this profile cannot be enabled on an implementation that only limits individual socket operations.”

The ten-millisecond minimum is arithmetic validation, not a recommendation for production.

At default values:

`P + R = 20 s < L = 30 s`

The profile therefore leaves ten seconds between a normally scheduled renewal and mutation refusal. This interval is not a guaranteed outage-service SLA.

### States and reporting {#states-and-reporting}

Use the existing lifecycle and reason fields as a presentation of operational readiness. Do not change the internal fence merely to update reporting.

| Operator state | `lifecycle` | `lifecycle_reason` | Meaning |
|---|---|---|---|
| Writable | `live` | empty | New mutations pass the timing admission gate |
| Writes paused | `not_live` | `lease_not_renewed` | Admission reserve no longer fits; local lease authority has not yet expired |
| Lease expired locally | `not_live` | `lease_expired` | The holder’s local authority timer ended; renewal still retries |
| Fenced or superseded | `not_live` | Existing definitive-loss reason | This incarnation cannot resume through ordinary renewal |
| Recovering | Existing recovery presentation | Existing recovery reason | Reclaim and recovery are underway |
| Identity lost or decommissioned | Existing terminal presentation | Existing terminal reason | Preserve terminal-state precedence |

Add effective durations and local timing observations to `system.cas_mounts`:

- Confirmed renewal anchor age.
- Time remaining until mutation cutoff.
- Time remaining until local authority expiration.
- Effective tick or legacy profile.
- Effective request deadline, lease and renewal cadence.
- Last renewal failure.

Display wall-clock instants if useful, but label them diagnostic. Calculate authority and remaining durations from the local monotonic/boottime clock. Do not compare these display timestamps between servers.

A remote stored mount record cannot reveal its holder’s exact local cutoff. Report those fields as unknown unless obtained from that holder’s runtime.

Log the effective timeline at mount. Log each transition once: writes paused, lease expired, writes resumed, or definitive fencing. The pause message should name `cas_tick_ms`, the elapsed anchor age, and the last failed renewal.

A state snapshot must reflect elapsed time even if the renewal thread is blocked. Transition logs may arrive late after a process stall; their recorded transition instant should remain the actual local threshold.

### One outage with the proposed defaults {#one-outage-with-the-proposed-defaults}

Assume the last confirmed renewal has anchor zero. The holder loses access at ten seconds and regains it at 45 seconds. Other participants can still reach the store.

| Holder time | Holder behavior | Local system-table presentation |
|---:|---|---|
| 0 s | Confirmed renewal establishes authority through 30 s | `live` |
| 10 s | Renewal starts; requests fail and retry | `live`, with last renewal failure |
| 20 s | Stop starting new mount mutations | `not_live`, `lease_not_renewed` |
| 20–30 s | Earlier admitted work may settle within its budget; renewal continues | Writes paused |
| 30 s | Local authority expires | `not_live`, `lease_expired` |
| 45 s | Store answers; classify the durable mount record | Resume only with sufficient confirmed budget, or enter definitive-loss recovery |

Takeover times need their own clock origin:

- A restarted claimant may attempt reclaim after its token has remained unchanged for 36.5 seconds on its own clock.
- GC may attempt fencing after its token has remained unchanged for 41.5 seconds on its own clock.
- If first observation was at holder time zero, these correspond approximately to 36.5 and 41.5 seconds with equal clock rates.
- If a restarted claimant first observes at ten seconds, its threshold is approximately 46.5 seconds.
- Poll scheduling, reads and the conditional mutation add execution latency.
- If the observer also cannot reach the store, it cannot complete fencing or takeover at the nominal threshold.

If GC fences at approximately 41.5 seconds, the holder’s response at 45 seconds must lead to definitive loss and recovery. It cannot resume the old incarnation.

If nobody has changed the mount record, the logical renewal started at ten seconds can confirm only a deadline of 40 seconds. Its success at 45 seconds is stale. A fresh successful renewal is needed before writes resume.

A shorter outage ending at 25 seconds could restore admission under the same epoch, because that renewal’s deadline would be 40 seconds and sufficient reserve would remain.

### Compatibility {#compatibility}

Retain the legacy settings and defaults:

| Legacy setting | Default | Preserved meaning |
|---|---:|---|
| `cas_mount_lease_ttl_ms` | 30000 | Local authority duration from the confirmed logical request anchor |
| `cas_mount_renew_period_ms` | 10000 | Renewal cadence and input to observer timing |
| `cas_attempt_timeout_ms` | 5000 | Existing socket-operation limit and input to the legacy reservation |
| `cas_lease_safety_margin_ms` | 2000 | Margin used in runtime admission and startup validation |

Resolution rules:

- Any explicitly configured legacy timing key, without explicit `cas_tick_ms`, selects legacy mode.
- Unspecified legacy values retain their current defaults.
- Explicit `cas_tick_ms` together with an explicit legacy timing key is rejected.
- With neither family explicit, the eventual new release uses the default tick profile.
- Log the selected profile and every effective duration.

CAS-343 can add a whole-attempt deadline to legacy mode equal to its existing envelope. This preserves its configuration arithmetic and admission cutoff, while intentionally ending requests that previously could overrun it. That behavioral correction must be documented.

A five-second tick derives the current lease and renewal values. Switching a default legacy member to this profile therefore needs no stopped-pool timing change. The later admission cutoff still requires the safety gates described below.

Custom legacy configurations do not necessarily map to a tick. Keep them supported. Do not round them into the nearest profile.

Mixed software versions are compatible only where effective lease and renewal values remain equal. Old binaries must not be given an unknown tick setting.

### Configuration disagreement {#configuration-disagreement}

A shorter observer threshold can act while another holder still considers itself authoritative. This is a safety violation, not merely a less efficient configuration.

The frozen object format provides no authoritative timing declaration to validate. Local validation cannot prove agreement with an unavailable peer.

Use deployment configuration validation for all intended pool members. Expose effective values for fleet comparison. Do not treat this as a complete runtime protocol guarantee.

If disagreement is discovered, stop the pool and reconcile it. Do not automatically choose the minimum, maximum, or majority value.

A mount-body timing field would violate the frozen-format constraint. It should not be part of this proposal.

### Order of work {#order-of-work}

1. Ship read-side visibility, effective-timing logs, clearer refusals and corrected setting descriptions. Preserve current timing.
2. Complete CAS-343. Demonstrate one deadline through every blocking path and response consumption.
3. Complete the late-application argument for non-ledger mutations and GC interactions. A client deadline alone is insufficient.
4. Verify tick-mode admission and state projection with injected clocks and request faults. Include equality boundaries, renewal races, terminal precedence, suspension, rounding and overflow.
5. Compare the proposed request limits on S3, GCS and local object stores. Measure complete attempt duration, attempt number, method, connection setup and ambiguous outcomes.
6. Introduce the fixed tick profile, legacy resolution and migration instructions.
7. Consider a default transition only after those gates pass.

The tests above are proposed future work. None were run for this study.

Defer configurable multipliers, observer-side reserves, stored timing declarations, background wedge recovery, and one-envelope admission.

### Risks and reasons to change the recommendation {#risks-and-reasons-to-change-the-recommendation}

The main risk is late remote application after client timeout. If the non-ledger audit cannot establish safety, I would keep current admission timing and ship visibility only.

The second risk is coupling. If deployment evidence shows a need for long request deadlines with short failover, or fast requests with long outage allowance, a single tick becomes inadequate. Then a directly named trio is worth considering: renewal cadence, mutation-stop age and authority duration, with explicit request-reserve validation.

The third risk is compatibility complexity. Supporting custom legacy configurations creates two resolution modes. Explicit mode reporting and rejecting mixed setting families are essential.

The fourth risk is additional ambiguous outcomes under shorter deadlines. I would change the request fraction or defer tick defaults if measurements show materially more wedges, recovery work or failed statements.

The newer S3 sample supports studying the proposed limits. It does not establish their suitability for every supported store.

## Part 4 {#part-4}

The documents are useful, but several statements need correction or qualification.

#### 1. CAS-343 does not by itself prove remote writes finish before authority ends {#item-1-cas-343-does-not-by-itself-prove-remote-writes-finish}

The notes describe the first envelope as ensuring that a `PUT` ends or times out before the deadline. Those are different outcomes.

A client timeout can leave a remotely accepted mutation unresolved. Q2’s “Open ends” explicitly acknowledges later application. Therefore the proposed HTTP deadline proves a client-duration property. It does not alone prove the stronger no-late-mutation property.

The ref ledger’s conditional seals provide a separate argument. The studies explicitly leave other keys incomplete. That gate belongs before any reserve reduction, including E’s reduction from 16 to ten seconds.

Evidence: [studies, Q1 and Q2](2026-10-02-mount-lease-settings-studies.md).

#### 2. The no-reserve example cannot cross both quoted observer thresholds {#item-2-the-no-reserve-example-cannot-cross-both-quoted-obser}

The notes give a request ending around 37 seconds, restart entitlement around 34.8 seconds, and GC entitlement around 39.5 seconds.

That request can cross the restart threshold. It cannot cross the GC threshold at 39.5 seconds.

Under the stated maximum rate ratio:

`36.5 / 1.05 ≈ 34.76 s`

`41.5 / 1.05 ≈ 39.52 s`

A longer request could cross both. The example as written does not.

#### 3. Observer waits are not measured from the holder’s renewal anchor {#item-3-observer-waits-are-not-measured-from-the-holder-s-ren}

`mountObservationThresholdMs` supplies a duration. `TokenWatch::stableFor` measures it from the observer’s first post-read sighting to its pre-confirmation-read sample.

The 36.5- and 41.5-second timeline entries need that origin stated explicitly. Neither is an exact global takeover instant.

Evidence at e1c38ca7478: `CA/Pool/CasServerRoot.cpp`, `TokenWatch::stableFor`, `mountObservationThresholdMs`, and `claimMountAwaitingExpiry`.

#### 4. “Every write is refused from 14 seconds” is too broad {#item-4-every-write-is-refused-from-14-seconds-is-too-broad}

At the pinned commit, `CasOperation::writeLoop` reserves two envelopes. The study also documents mutation paths through `readLoop` using one.

Their default cutoff is 21 seconds, rather than 14, subject to their other checks. The 14-second number accurately describes conditional-write and ref-append admission. It does not describe every backend mutation.

Evidence at e1c38ca7478: `CA/Backend/CasRequests.cpp:927`; study Q1’s caller table.

#### 5. Four seconds is admission slack, not guaranteed successful-write outage tolerance {#item-5-four-seconds-is-admission-slack-not-guaranteed-succes}

`TTL − period − reserve` measures the time between scheduled renewal and conditional-write cutoff, assuming the stated anchor.

Actual behavior depends on outage phase, request duration, ambiguous results, lane state and storage availability. During a complete store outage, admitted writes cannot succeed simply because lease budget remains.

The eight-second-outage example also needs the assumed start at the renewal due time and recovery latency stated.

#### 6. A successful renewal need not immediately resume writes {#item-6-a-successful-renewal-need-not-immediately-resume-writ}

The renewed deadline remains tied to the logical renewal’s original start. A response can arrive with an already expired deadline, or with insufficient mutation reserve.

The background renewal design recognizes stale success and catch-up renewal. The UX sentence “When a renewal lands, writes resume” should say “when a confirmed renewal leaves enough admission budget.”

Evidence at e1c38ca7478: `MountLeaseRenewer::renewOn` derives `confirmed_deadline_boot_ms` from `attempt_start_boot_ms`; `CasMountRuntime::publishRenewedDeadline` handles an expired renewed deadline.

#### 7. E has two different reserve policies in the same note {#item-7-e-has-two-different-reserve-policies-in-the-same-note}

E’s table specifies a two-tick reserve and writes stopping at four ticks. The recommendation later allows a one-tick reserve.

With a six-tick lease, a one-tick reserve gives a five-tick cutoff unless another cutoff is introduced. It cannot silently retain the same four-tick explanation.

Choose one advertised profile. The settlement optimization should be a separate decision.

#### 8. “Lease length in the store” is misleading {#item-8-lease-length-in-the-store-is-misleading}

The lease duration used for authority and observation is configuration. Observers do not obtain authoritative timing from the stored object.

The pinned configuration document does describe a stored diagnostic `expires_at_ms`. That is not an authority source and must not become one.

Use “local authority duration,” rather than “lease length in the store.”

Evidence: `docs/en/antalya/cas/configuration.md` at the pinned commit, inspected through `git show`.

#### 9. The ZooKeeper analogy overstates the fractions and state semantics {#item-9-the-zookeeper-analogy-overstates-the-fractions-and-st}

The Java client has an approximate one-third idle ping target and two-thirds receive timeout. `Disconnected` can occur earlier. Server expiration and client notification are separate events.

The documentation does not make `Disconnected` a universal “no writes” rule for arbitrary application resources.

The useful precedent is named uncertainty before known expiration. [Java source](https://raw.githubusercontent.com/apache/zookeeper/master/zookeeper-server/src/main/java/org/apache/zookeeper/ClientCnxn.java), [programmer’s guide](https://zookeeper.apache.org/doc/current/zookeeperProgrammers.html)

#### 10. The second-envelope verdict is stronger than its stated coverage {#item-10-the-second-envelope-verdict-is-stronger-than-its-sta}

The study’s detailed findings support “the settlement read is not itself the safety mechanism” for the audited paths.

They explicitly qualify non-ledger keys, callers above CAS, and delayed application. The headline “not required for safety” should retain those scope limits.

Likewise, the second envelope is not the only protection against overruns. Other margins exist, and several mutation paths already reserve one envelope. None of those margins supplies a finite bound against an unbounded attempt.

#### 11. The timeout table’s one-second connect cap is a default, not a universal cap {#item-11-the-timeout-table-s-one-second-connect-cap-is-a-defa}

The pinned configuration arithmetic freezes a cap from the disk connect timeout and attempt timeout. `getSingleAttemptClient` enforces the supplied frozen cap.

A disk connect setting above one second can therefore affect the envelope. The table should distinguish the default disk value from the general formula.

Evidence at e1c38ca7478: `CasRequestBudget::attemptEnvelopeMs`, the configuration table, and `S3ObjectStorage::getSingleAttemptClient`.

#### 12. The TLS and “one stalled phase” bounds are conditional {#item-12-the-tls-and-one-stalled-phase-bounds-are-conditional}

Q2’s phase table states a bounded TLS handshake, then its open questions acknowledge that a trickling peer may defeat that interpretation.

Likewise, “one phase stalls completely, at most seven seconds” assumes bounded preceding work, warm DNS and no other unbounded preparation. The broad summary omits those assumptions.

#### 13. Expiry reporting is not necessarily emitted exactly at expiry {#item-13-expiry-reporting-is-not-necessarily-emitted-exactly}

At the pinned commit:

- `warnOnceIfLeaseExpired` runs through request notifications.
- `CASMountLeaseExpired` increments in `publishRenewedDeadline` when an expired interval is restored.
- A blocked attempt can delay the warning.

The counter and warning are useful, but the notes should not imply a timer-driven warning and counter increment exactly at 30 seconds.

Evidence: `CA/Pool/CasMountRuntime.cpp`, `noteRenewRequest`, `warnOnceIfLeaseExpired`, and `publishRenewedDeadline`.

#### 14. Some function references belong to another revision {#item-14-some-function-references-belong-to-another-revision}

The UX note cites `CasMountRuntime::budgetAdmits`. The study explicitly says that function does not exist at e1c38ca7478; the pinned implementation uses `admit`.

The arithmetic is supported. The code reference should identify the later revision or use the pinned name.

#### 15. Percentiles of per-second means do not establish request percentiles {#item-15-percentiles-of-per-second-means-do-not-establish-req}

The study correctly says request percentiles cannot be computed from its aggregate series. Its stronger sentence saying per-request percentiles are higher is not generally justified.

Different bucket sizes and request counts can change weighting in either direction. Treat those aggregate percentiles as a different statistic.

#### 16. The old unresolved counts do not isolate a five-second timeout population {#item-16-the-old-unresolved-counts-do-not-isolate-a-five-seco}

The documents already acknowledge that unresolved outcomes combine several causes. They also document adaptive first-attempt limits below five seconds.

Consequently, the unresolved fraction is not a measured five-second timeout rate, and it cannot decide the effect of changing all attempts to three seconds.

#### 17. The newer measurement resolves part of Q3, with censoring {#item-17-the-newer-measurement-resolves-part-of-q3-with-censo}

The supplied seven-day sample now gives per-request control-plane `PUT` percentiles for one AWS deployment:

| Observation | Approximate frequency |
|---|---:|
| Longer than 0.5 s: 88 of 5.67 million | 15.5 per million |
| Longer than 1 s: 42 | 7.4 per million |
| Around the 3 s timeout boundary: 10 | 1.76 per million |
| 0.5 s connect timeouts: 3 | 0.53 per million |

This materially strengthens the case for examining shorter limits. It supersedes “no per-request tail data” for that deployment and request class.

However, all ten slowest attempts ended at the adaptive timeout. Their successful completion times are unknown. “None exceeded five seconds” is not evidence that they would all have completed within five seconds.

The sample also leaves GCS, other S3 implementations, other request methods, outage correlation and full-attempt preparation costs unresolved.

#### 18. The AWS C++ three-second comparison needs transport and version qualification {#item-18-the-aws-c-three-second-comparison-needs-transport-an}

The current upstream source describes curl’s `requestTimeoutMs` as a low-speed interval and `httpRequestTimeoutMs` as its total transfer timeout. Its current initializer is zero, while the comment still mentions a 3000 ms Windows default.

The bundled version may indeed use three seconds. That does not make it equivalent to ClickHouse’s Poco socket timeout, or establish three seconds as a whole-attempt default. [AWS C++ source](https://raw.githubusercontent.com/aws/aws-sdk-cpp/main/src/aws-cpp-sdk-core/include/aws/core/client/ClientConfiguration.h)

### Ten-line summary {#ten-line-summary}

1. Recommend E: one fixed tick, holder-side reserves, and explicit paused-write reporting.
2. Use `cas_tick_ms = 5000` for the new profile; retain explicit legacy configurations.
3. Renew every 10 seconds; stop new mount mutations after 20 seconds from the confirmed anchor.
4. End local authority after 30 seconds; keep two four-second request reservations plus a two-second margin.
5. Restart and GC thresholds remain 36.5 and 41.5 seconds from each observer’s first unchanged-token sighting.
6. Operators gain one predictable timeline and a visible distinction between paused writes and expired authority.
7. Ship visibility and corrected descriptions first, with today’s admission timing.
8. Complete CAS-343 and the non-ledger late-write safety argument before enabling the later cutoff.
9. The main risk is remote application after client timeout; the S3 sample does not eliminate it.
10. Preserve the frozen format and clock-free protocol; stop the whole pool whenever effective lease or cadence changes.

