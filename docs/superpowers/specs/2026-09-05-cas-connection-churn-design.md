---
description: 'Outcome record for the connection churn behind ephemeral-port exhaustion on CAS-over-S3 disks (issue #2243): the reason-breakdown instrumentation was written, used for one A/B spike, and removed; the spike ran but its baseline did not reproduce port pressure; the keep-alive mitigation shipped as an operator config recommendation, not a code-level client-profile default. The read-path drain (decision 4) is a separate, still-open investigation superseded by `2026-09-15-s3-drain-remainder-research.md`.'
sidebar_label: 'CAS connection churn (outcome)'
sidebar_position: 10
slug: /superpowers/specs/cas-connection-churn-design
title: 'CAS: connection churn and ephemeral-port exhaustion — outcome record'
doc_type: 'design'
---

# CAS: connection churn and ephemeral-port exhaustion — outcome record {#cas-connection-churn-design}

**Status:** OUTCOME RECORD (groomed 2026-09-25 from DRAFT rev.4, 2026-09-05). Spec 3 of the R2 series;
the measured part of https://github.com/Altinity/ClickHouse/issues/2243. Kept as a design/history
record; the original step-by-step decision text is replaced below by what each step actually did.
codex review history (`codex_spec3.final.md`, `codex_spec3r2.final.md`) is unchanged from rev.2.

## What was measured, and what it does and does not say {#measured}

CI, `Stateless tests (amd_asan_ubsan, cas s3 storage, parallel, 2/2)`, `metric_log` per 10 minutes:
`DiskConnectionsReused` 250–300k, `DiskConnectionsCreated` 5–25k, `Expired` 2–4k, `Reset` up to 22k.
rustfs logged 1 604 `Connection reset by peer`. The adaptive first-attempt fuse (spec 4, landed) accounted
for ~570 trips in two hours. These are lifecycle counters, not cohorts of one window.

`cas_selects` (regression, three nodes, no system tables collected) hit 970 `EADDRNOTAVAIL` in one
minute at the start of the concurrent-selects phase. The backlog's #2243 reading (reporter: ~430 GET/s
× 60 s ≈ 26k of 28 232 ports) fits a TIME_WAIT rate equal to the request rate.

## Outcome, by decision {#outcome}

1. **Reset/Expired reason counters — implemented, then reverted; not in `antalya-26.6`.** Ten DISK-only
   `ProfileEvents` landed on `cas-gc-rebuild` (`054c2b9407a` "cas: count disk connection resets and
   expiries by reason", with `gtest_connection_pool.cpp` coverage) and were deliberately dropped one
   commit range later (`2f03984e027`/`5a53310ee82` "cas: drop the connection-pool reason counters"):
   the maintainer's stated reason is that this diagnostic touched `src/Common/HTTPConnectionPool.*`
   outside CAS-owned code for a one-time attribution need the shipped fix does not depend on. Neither
   the add nor the revert reached `altinity/antalya-26.6`. **Consequence:** decision 4 below can no
   longer be "justified by the reason counters" as originally planned — that dependency is broken; a
   future read-path investigation needs its own instrumentation or must re-add these events.
2. **The A/B spike — run, recorded below, baseline invalid.** Ten runs on a real
   rustfs `1.0.0-rc.3` stand are recorded verbatim in [Implementation record](#implementation-record)
   below. Verdict reached: `ExpiredMaxRequests` (the 100-request
   keep-alive rotation) is the dominant, mechanistic cause of the churn, not `Reset`; raising
   `http_keep_alive_max_requests` to 10000 removes it. But the spec's own strict rule ("baseline is
   valid only if it reproduces port pressure") was **not met**: zero `EADDRNOTAVAIL`, TIME_WAIT peak
   ≤ 11.9% of the range, likely because rustfs sat on loopback with `net.ipv4.tcp_tw_reuse=2` masking
   TIME_WAIT pressure a real network hop would not mask, and because the stand's request rate was one
   to two orders of magnitude below the reporter's incident. **This is a genuine open point**: nobody
   has since redone the spike over a non-loopback path at the reporter's rate, and no BACKLOG entry
   tracks it (see below).
3. **The keep-alive mitigation shipped — as an operator config recommendation, not as code.** The
   plan's mechanism (a `S3ClientProfile`/`ObjectStorageCreateHints` default-injection seam in
   `ObjectStorageFactory`/`RegisterDiskObjectStorage`/`S3ObjectStorage`) was **never implemented**; grep
   for `S3ClientProfile`, `applyClientProfileDefaults`, `ObjectStorageCreateHints`,
   `casClientProfileHintFor` in `src/` on both `cas-gc-rebuild` and `altinity/antalya-26.6` returns
   nothing. Instead, `http_keep_alive_timeout=30` / `http_keep_alive_max_requests=10000` are recommended
   directly on every CAS S3 disk's own XML (`05e7fc2bdee` "cas: recommend the keep-alive settings on
   every CAS S3 disk config, in docs and tests", `3517ab72bfd` follow-up), rewriting
   `docs/en/antalya/cas/configuration.md` §"Recommended keep-alive settings" and every `test_cas_*`
   config, the stateless lane's `cas_s3_storage_policy_for_merge_tree_by_default.xml`, and
   `test_cas_mount_renewal_retry`'s `unsafe_remount.xml`. **This has not reached `altinity/antalya-26.6`
   yet** — only `cas-gc-rebuild` carries it (verified: `05e7fc2bdee` is not an ancestor of
   `altinity/antalya-26.6`).
4. **Read path (draining a buffered remainder) — still open, superseded by dedicated research.** The
   original plan deferred this "until the reason counters justify it"; a candidate patch exists on
   unmerged branch `fix/antalya-26.6/s3-drain-buffered-remainder` (`3e0dd5e279d`, pushed to filimonov,
   citing issue #2332), but `docs/superpowers/cas/2026-09-15-s3-drain-remainder-research.md` (Parts 1–2,
   already in the tree) found the patch as written recovers at most ~7.8 KiB and is expected to convert
   near-zero of #2332's multi-MiB remainders, because Poco's session buffer is only ever refilled by the
   header parser and is empty after the first body read. That research ranks "fix the read range"
   (`MergeTreeReaderStream::adjustRightMark`/`setReadUntilPosition`) above the buffered-only drain. This
   whole line of work is tracked only by that dated research note and a generic umbrella-roadmap mention
   of issue #2332 — no `BACKLOG.md`/`BACKLOG/*.md` bracket-id item names it as a live task.
5. **Not a fix, but not nothing — unchanged, still true.** Lowering `disk_connections_soft_limit`/
   `store_limit` does not help (they gate keeping, not creating); the hard limit stops creation but
   turns port pressure into `HTTP_CONNECTION_LIMIT_REACHED`, insufficient for lease safety.

## Out of scope {#out-of-scope}

Renewal isolation (a dedicated connection with a keep-alive ping was rejected), spec 1, spec 2, spec 4
(all landed — see their own outcome).

## Implementation record {#implementation-record}

**Stand.** rustfs `1.0.0-rc.3` (the cached `ci/tmp/rustfs` was `1.0.0-beta.9`; downloaded the pinned
release to a versioned path and asserted `--version` before use), one `clickhouse server` built from
HEAD (`666e4b0db4a`, confirmed to contain the D1 reason counters), the lane's `cas_s3` disk
(`tests/config/config.d/cas_s3_storage_policy_for_merge_tree_by_default.xml`) against rustfs, every
system log pinned to a local `default`-policy disk so background log flushes cannot confound the
measurement. Ports moved to `18123`/`19000` because another live investigation on the same host
already held `8123`/`9000`. Ephemeral range `32768-60999` (28 232 ports), `net.ipv4.tcp_tw_reuse = 2`
(loopback-only reuse — material to the verdict, see below). Data: `spike`/`spike_join`
(`ReplacingMergeTree`, `storage_policy='cas_s3'`, merges stopped), 200 parts x 10 000 rows each,
loaded as 200 disjoint-key `INSERT`s. Load: 20 queries (10 point, 10 range, each a `JOIN` between the
two tables), fixed seed.

**rustfs keep-alive probe.** A `HEAD` response (both raw and SigV4-signed) carries no `Keep-Alive`
header and no `Connection: close` — rustfs never advertises a cap. A Python `http.client` connection
held idle and reused after 10 s, 20 s, 40 s, and 75 s was accepted every time (same TCP socket, same
`403` response) — rustfs's own HTTP/1 header-read timeout is empirically >= 75 s on rc.3, matching the
spec's assumed figure.

**Ramp.** `clickhouse-benchmark --concurrency N -i 0 --timelimit 20` at `N=4` already produced
`DiskS3GetObject` at ~696/s (target ~400/s) with zero rustfs 5xx; `N=4` is the found value (doubling
further would only overshoot the target).

**Arms.** 5 arms (`http_keep_alive_timeout`/`http_keep_alive_max_requests` in the `cas_s3` disk
block, or `disk_connections_soft_limit` at top level), `concurrency=4`, `--timelimit 300`, server
restarted between every run (fresh client pool), TIME_WAIT-to-rustfs drained below 500 first, run
order ABBA (forward baseline/30s-100/5s-10000/30s-10000/soft-limit-100, then reversed) so every arm
gets one early and one late repetition:

| run | arm | completed queries | `DiskS3GetObject` | `DiskConnectionsCreated` | GetObject/Created | `ExpiredMaxRequests` | TIME_WAIT peak | peak % of range |
|---|---|---|---|---|---|---|---|---|
| 1 | baseline (5s/100) | 60 208 | 362 670 | 3 600 | 100.7 | 3 651 | 769 | 2.7% |
| 10 | baseline (5s/100) | 271 051 | 1 628 125 | 16 288 | 100.0 | 16 276 | 3 239 | 11.5% |
| 2 | 30s/100 | 269 842 | 1 625 597 | 16 286 | 99.8 | 16 305 | 3 311 | 11.7% |
| 9 | 30s/100 | 272 145 | 1 636 086 | 16 363 | 100.0 | 16 358 | 3 347 | 11.9% |
| 3 | 5s/10000 | 263 697 | 1 583 075 | 167 | 9 479.5 | 155 | 324 | 1.2% |
| 8 | 5s/10000 | 270 340 | 1 624 932 | 163 | 9 968.9 | 157 | 53 | 0.2% |
| 4 | 30s/10000 | 274 092 | 1 646 432 | 163 | 10 100.8 | 158 | 49 | 0.2% |
| 7 | 30s/10000 | 268 472 | 1 614 779 | 154 | 10 485.6 | 159 | 325 | 1.2% |
| 5 | soft-limit 100 | 271 024 | 1 628 284 | 16 286 | 100.0 | 16 282 | 3 256 | 11.5% |
| 6 | soft-limit 100 | 262 432 | 1 577 031 | 15 781 | 99.9 | 15 769 | 3 139 | 11.1% |

Every one of the 10 runs recorded zero `EADDRNOTAVAIL`, zero `CASMountLeaseLost`/
`CASMountRenewalDeadlineExceeded`, and zero of every `DiskConnectionsReset*` reason (the run's whole
`DiskConnectionsExpired` total is `ExpiredMaxRequests`, plus one stray `ExpiredAge` in two runs).
`CASMountRenewalAttempts` was ~30-34 per run regardless of arm.

Run 1 (the very first restart after the data load) completed ~4.5x fewer queries than run 10 (the
same arm, run last) at the same concurrency — a one-time cold-mount cost on the first post-load
restart, not an arm effect: the GetObject-per-completed-query ratio is identical between the two
(6.02 vs 6.01), so every downstream counter scales with it. Reported here rather than discarded so
the low run-1 numbers are not misread as a `5s/100` regression.

**What the reason counters show.** `ExpiredMaxRequests` accounts for essentially all
`DiskConnectionsExpired`, and `DiskConnectionsCreated` tracks `completed_queries / 100` almost
exactly in every arm still running the default `http_keep_alive_max_requests=100` (baseline,
`30s/100`, soft-limit-100) — the churn is **entirely** the request cap, not the keep-alive timeout:
raising only the timeout (`30s/100` vs baseline) moved nothing (ratio 99.8-100.0 either way,
TIME_WAIT peak 11.1-11.9% either way). Raising only `http_keep_alive_max_requests` to 10 000
(`5s/10000`) cut `DiskConnectionsCreated` by ~100x and TIME_WAIT peak by ~10-65x, with completed
queries and error rates unchanged; adding the 30 s timeout on top (`30s/10000`) added a further
+6% on the GetObject/Created ratio (10 288 vs 9 721 summed) — a small but consistent extra
benefit. The `disk_connections_soft_limit=100` arm is indistinguishable from baseline (ratio 100.0,
TIME_WAIT peak 11.1-11.5%): confirms decision 5 -- the soft limit gates keeping, not creating, and
does not reduce port churn.

An earlier informal ramp probe at `N=4`/20 s (before the arm battery, using `SYSTEM DROP MARK CACHE`
between concurrency steps to force a cold read) showed the opposite reason profile --
`DiskConnectionsResetIncompleteRequestOrResponse` at 100% of `Reset` and zero `Expired*`. None of
the 10 sustained 300 s runs (each starting from an equally cold, freshly-restarted server)
reproduced any `Reset` of any reason. The likely explanation: dropping the mark/uncompressed cache
mid-flight, synchronized with concurrent in-flight reads, cancels reads that would otherwise
complete -- an artifact of that specific probe, not a property of the workload or of a cold
server start. Recorded so it is not read as a second churn mechanism competing with
`ExpiredMaxRequests`.

**Verdict.** Baseline is **not valid** by the letter of the spec's rule: zero `EADDRNOTAVAIL` and a
TIME_WAIT peak of at most 11.9% of the range, both runs, well under the 50% threshold for "baseline
reproduces port pressure". Two likely reasons, left as-is per "do not tune the stand until it
fails": rustfs sits on loopback and `net.ipv4.tcp_tw_reuse=2` enables fast TIME_WAIT-socket reuse
specifically for loopback addresses (masking pressure that a real network hop would not mask), and
`N=4` over 300 s creates one to two orders of magnitude fewer connections (16-32k) than the
backlog's cited CI incident (~26k ports in one *minute* at ~430 GET/s). This is the valid
"baseline does not reproduce" outcome the spec allows for.

Because baseline is invalid, no arm can "pass" against it in the spec's strict sense. The reason
counters nonetheless give a decisive, mechanistic answer to the question decision 3 asked: the
default `http_keep_alive_max_requests=100` is not a headroom margin here, it is the connection's
entire lifetime under this workload's request rate (every arm still at 100 recycles a connection
~100 requests in, matching `ExpiredMaxRequests` one-for-one with `DiskConnectionsCreated`). Raising
it to 10 000 removes essentially all of the churn with no downside observed (same completed-query
counts, same error rates, zero `EADDRNOTAVAIL`/lease-loss both before and after).

**Chosen values.** `http_keep_alive_timeout=30` and `http_keep_alive_max_requests=10000` -- both
from the best-performing tested arm (`30s/10000`, GetObject/Created 10 288 summed, its two runs'
TIME_WAIT peaks at 49 and 325, i.e. 0.2%-1.2% of the range -- in the same low range as `5s/10000`'s
53 and 324, not distinguishable from it on this metric). `http_keep_alive_max_requests` is shipped,
not left at its default, because the spike attributes the dominant reason (`ExpiredMaxRequests`) to it directly, satisfying
the spec's own condition for changing that setting. 30 s (not the backlog's 60 s) because it is the
value actually tested here and sits with room to spare under rustfs's verified >= 75 s idle
tolerance.

## Open points not tracked elsewhere {#open-points}

- A real (non-loopback), reporter-rate redo of the A/B spike, to get a *valid* baseline — no BACKLOG
  item names this.
- Porting the shipped keep-alive recommendation (`05e7fc2bdee`, `3517ab72bfd`) from `cas-gc-rebuild` to
  `altinity/antalya-26.6`.
- The read-path drain question, now reduced (by the 2026-09-15 research) to "fix the read range" —
  needs a BACKLOG item of its own; today it lives only in a dated research doc and issue #2332.
