---
description: 'History of the ReadBufferFromS3 drain-remainder patch for issue #2332 (cas_selects ephemeral-port exhaustion) and the research verdict: S3 GETs are always fixed-length, the HTTP session buffer is only ever filled by the header parser, so the v2 drain recovers at most 7.8 KiB and does not address the connection churn; ranked alternatives.'
sidebar_label: 'S3 drain-remainder research (2026-09-15)'
sidebar_position: 9
slug: /superpowers/cas/s3-drain-remainder-research-2026-09-15
title: 'S3 drain-remainder patch for issue #2332: history and research verdict (2026-09-15)'
doc_type: 'reference'
---

# S3 drain-remainder patch for #2332: history and research verdict {#s3-drain-remainder-research}

## Part 1: `ReadBufferFromS3` drain-remainder patch: history and design notes {#part-1-readbufferfroms3-drain-remainder-patch-history-and-design}

Issue: https://github.com/Altinity/ClickHouse/issues/2332 (cas_selects, ephemeral-port exhaustion).
Mechanism: compact parts + column-subset reads leave an unread HTTP body tail, so every pooled S3
connection is `Reset` instead of reused (`DiskConnectionsReset == DiskConnectionsCreated`); plain
`type=s3` disks behave the same. Spec context: decision 4 of
`docs/superpowers/specs/2026-09-05-cas-connection-churn-design.md` (deferred there until reason
counters justified it; they now do).

### v1 — `readbuffer_s3_drain_remainder.patch` (2026-09-09 21:08, Claude) {#v1-readbuffer-s3-drain-remainder-patch-2026-09-09-21-08-claude}

3 files, +48. `ReadBufferFromS3::drainSmallRemainderBeforeRelease`: remainder =
`read_until_position - offset` (or `file_size - offset`), drained via `impl->tryIgnore` when
<= `remote_read_min_bytes_for_seek`; called before `impl.reset()` in `seek`, `setReadUntilPosition`,
`setReadUntilEnd` and in an explicit destructor. Compiled, no tests.

### Codex review of v1 (gpt-5.6-sol, 2026-09-09 21:45, session rollout-2026-09-09T23-38-32) {#codex-review-of-v1-gpt-5-6-sol-2026-09-09-21-45-session-rollout}

Verdict: not acceptable.
1. **P1, blocking — drain writes into the consumer's external buffer.** With `use_external_buffer`
   the `impl` keeps a pointer to the last read's memory; `AsynchronousBoundedReadBuffer::prefetch_buffer`
   is declared after `impl` (`AsynchronousBoundedReadBuffer.h:65`) so it is destroyed first, and the
   destructor-time drain can write after free. On a range change it can also overwrite memory the
   upper layer is still consuming. Needs its own scratch memory.
2. **P1 — remainder counted from the end of the loaded block but skipped from the start of the
   inner buffer.** `offset` covers the whole loaded block while `impl`'s position lags until the next
   `nextImpl`; e.g. 800-byte response, 512 loaded: `tryIgnore(288)` skips already-loaded bytes,
   reads nothing from HTTP, and still bumps the success counter. Success must be the HTTP response
   actually completing.
3. **P2 — range-change call sites use already-mutated state.** `setReadUntilPosition` /
   `setReadUntilEnd` overwrite `offset` (and clear `read_until_position`) before the helper runs, so
   a small tail can be computed as the whole object and skipped. Snapshot the old response bounds.
4. **P2 — a byte cap does not cap latency.** `tryIgnore` is a blocking socket read with no
   cancellation check and no time budget; many readers destroyed together stack socket timeouts.

### v2 — `readbuffer_s3_drain_remainder_v2.patch` (2026-09-09 22:29, Codex; 8 files) {#v2-readbuffer-s3-drain-remainder-v2-patch-2026-09-09-22-29-codex}

Design change that answers all four findings at once: **drain only a tail that is already entirely
in Poco's session buffer; never touch the socket, never touch the consumer's buffer.**

- `Poco::Net::HTTPFixedLengthStreamBuf::tryDrainBufferedRemainder(max_bytes)`: returns 0 unless
  `remaining <= max_bytes && remaining <= _session.buffered()`; then consumes it through
  `readFromDevice` into a local 1 KiB scratch array. So a tail Poco has not received yet is simply
  not drained (the connection resets as before), and the destructor can never block on the network.
- `ReadBufferFromGetObjectResult::tryDrainBufferedRemainder`: only when `result` is alive and not
  canceled; `dynamic_cast` to `HTTPFixedLengthStreamBuf`, so chunked/unknown-length bodies are
  skipped (their framing might need another socket read); `bytes_read += drained`. The HTTP stream
  owns the real response bounds and counts bytes pulled from the session, independent of the
  consumer cursor, requested range or externally supplied file size — this fixes findings 2 and 3
  without snapshotting anything in `ReadBufferFromS3`.
- `ReadBufferFromS3::drainBufferedRemainderBeforeRelease() noexcept`: skipped if `!impl`,
  `isCanceled()`, `std::uncaught_exceptions()`, or the current query is canceled; byte cap =
  `remote_fs_settings.min_bytes_for_seek`; counts `ReadBufferFromS3Bytes` and the new
  `ReadBufferFromS3DrainedBeforeRelease`; exceptions only logged at debug. Called from the explicit
  destructor and before each `impl.reset()` in `seek`, `setReadUntilPosition`, `setReadUntilEnd`.
- Why it makes the pool reuse the connection: it advances the same completion predicate the pool
  checks (`HTTPConnectionPool.cpp:553`) and empties the session buffer the pooling check requires
  (`:879`).
- Tests: `src/IO/tests/gtest_s3_drain_remainder.cpp`, 12 tests with a real Poco fixed-length parser
  over a transport stub that fails on any second socket read (`PacketSocket`): drain from HTTP
  position with unknown file size; external buffer not overwritten; external buffer freed before the
  reader; range change / open-ended range change use the old response; seek drains the discarded
  response; incomplete tail is NOT drained; etc. On unpatched code 6 fail / 6 pass, patched 12/12.

### Validation (Codex, 2026-09-10 00:21, harness `drain_remainder_v2/validate.py`) {#validation-codex-2026-09-10-00-21-harness-drain-remainder-v2}

Base commit 94bc2762e87. 29/29 S3 gtests incl. the 12 new; 12/12 under the ASan build without
diagnostics; all changed TUs compile; `git apply --check` passes. Logs (may be gone):
`build/test_drain_v2_before.log`, `build/test_drain_v2_after_existing.log`,
`build_asan/test_drain_v2_after.log`.

### Second Codex review of v2 (2026-09-10 00:28) {#second-codex-review-of-v2-2026-09-10-00-28}

"No concrete correctness issues found in v2." Cancellation, unwinding, released responses, byte
limits and consumer-cursor independence handled; lifecycle tests cover the v1 defects.

### Still open before it can become a PR {#still-open-before-it-can-become-a-pr}

- Not exercised end to end: the A/B on the local cas_selects repro (229 compact parts, `SELECT sum(id)`)
  should show `DiskConnectionsReset` near zero and `DrainedBeforeRelease` near the part count.
- Effectiveness depends on Poco having already buffered the tail; how often that holds for the
  compact-part read pattern (tail = rest of the granule, up to `min_bytes_for_seek`) is unmeasured.
  The v1 idea of a bounded blocking read is the fallback if the buffered-only drain proves too weak.
- `base/poco` change: a fork patch to a vendored library; upstream-worthy, needs the minimal/portable
  framing (memory: upstream code changes minimal, motivated, portable).
- Not applied to any branch; not in antalya-26.6.

### Research 2026-09-15 (Part 2 below): v2 almost never fires {#research-2026-09-15-research-drain-research-md-v2-almost-never}

- Framing: `ReadBufferFromS3` always sends a `Range`, S3/MinIO/RustFS answer 200/206 with `Content-Length`
  (RustFS verified live: `206 content-length: 100`, no `Transfer-Encoding`). Fixed-length-only costs nothing.
- Blocking: `SocketImpl::receiveBytes` = poll + one `recv` without `MSG_WAITALL` (`SocketImpl.cpp:342-352`),
  returns k < N at once; blocks only at 0 available (up to `http_receive_timeout`, 30 s).
- TLS: `SecureSocketImpl::available` = `SSL_pending` (decrypted plaintext only); a plaintext read can block
  on an incomplete record, unobservable through Poco. v2 (Poco's decrypted buffer only) is sound under TLS
  by construction; any FIONREAD extension must be gated to plain sockets and is bounded by `SO_RCVBUF`.
- **Load-bearing:** the `HTTPSession` buffer is 8 KiB and is refilled ONLY by the byte-at-a-time header
  parser (`HTTPSession.cpp:142,154,219-228`); `HTTPSession::read` hands out the leftover once and then
  receives straight into the caller's 1 MiB buffer. So after the first body read `buffered()` is 0 for the
  rest of the response; v2 can drain at most ~7.8 KiB and only when the body was never read. The #2332
  remainder is up to 4 MiB after >= 1 MiB was read: expected conversion of resets into reuse ≈ 0.
- Ranking: (d) fix the read range (`MergeTreeReaderStream::adjustRightMark`) > (c) bounded blocking drain
  with a real cancellation-aware deadline > (a) v2 as is (sound but ineffective for #2332) > (b) FIONREAD.
- Branch `fix/antalya-26.6/s3-drain-buffered-remainder` (3e0dd5e279d, pushed to filimonov) should NOT be
  presented as the #2332 fix without the A/B counters (`ReadBufferFromS3DrainedBeforeRelease` vs
  `DiskConnectionsReset` on the repro).


## Part 2: Drain-remainder research (Altinity #2332) {#part-2-drain-remainder-research-altinity-2332}

Tree: `/home/mfilimonov/workspace/ClickHouse/lane-g`, base `94bc2762e87`. Read-only.

### 1. Framing of S3 GET responses {#framing-of-s3-get-responses}

Selection in `base/poco/Net/src/HTTPClientSession.cpp:388-395`, in order:
no body expected / status < 200 / 204 / 304 -> `HTTPFixedLengthInputStream(*this, 0)`;
`Transfer-Encoding: chunked` -> `HTTPChunkedInputStream`; `Content-Length` present ->
`HTTPFixedLengthInputStream(*this, getContentLength64())`; neither -> `HTTPInputStream`
(read to EOF). The pool classifies the same three cases at `src/Common/HTTPConnectionPool.cpp:547-560`.

`ReadBufferFromS3::sendRequest` always sets a Range header, either `bytes=a-b`
(`src/IO/ReadBufferFromS3.cpp:601`) or open-ended `bytes=a-` (`:608`), so every object read is a
200/206 with `Content-Length`. AWS S3 returns `Content-Length` for `GetObject` with and without a
Range and does not chunk `GetObject` responses; MinIO does the same. RustFS verified on the live rig
(bucket `test`, key `cas_s3/blobs/ch128/00/000...000`, 256 bytes):

    Range: bytes=0-99  -> HTTP/1.1 206 Partial Content, content-length: 100, content-range: bytes 0-99/256
    no Range           -> HTTP/1.1 200 OK, content-length: 256

No `Transfer-Encoding` in either case. So the fixed-length branch is the case for ClickHouse S3
reads, and restricting the drain to `HTTPFixedLengthStreamBuf` costs nothing in practice.

### 2. Blocking semantics on plain TCP {#blocking-semantics-on-plain-tcp}

`SocketImpl::receiveBytes` (`base/poco/Net/src/SocketImpl.cpp:330-375`): while blocking it first
polls for readability with the receive timeout (`:342-346`, `pollImpl` at `:522`), then issues a
single `::recv(_sockfd, buffer, length, flags)` at `:352`. `MSG_WAITALL` is never passed. So with
0 < k < N bytes in the kernel queue it returns k immediately; it blocks only while zero bytes are
available, bounded by the receive timeout (S3 sessions: `http_receive_timeout`, default 30 s,
`src/Core/Defines.h:53`).

Therefore a drain guarded by `StreamSocket::available()` (FIONREAD, `SocketImpl.cpp:485-490`)
>= remaining is non-blocking for plain TCP, provided the loop re-checks and never asks for more than
was reported. Pitfalls: (a) the remainder can be split between Poco's session buffer and the kernel,
so the guard must be `session.buffered() + available() >= remaining`; (b) on a peer-closed socket
FIONREAD reports only the bytes still queued, and a read past them returns 0, which
`HTTPFixedLengthStreamBuf::readFromDevice` turns into `MessageException("Unexpected EOF")`
(`HTTPFixedLengthStream.cpp:59`) - it must be caught; (c) `SO_RCVBUF` caps what can ever be
simultaneously available, so a multi-MiB remainder essentially never satisfies the guard; the
extension only widens the reach from "already buffered" to "already buffered plus one socket
window", still far below 4 MiB.

### 3. TLS {#tls}

`SecureStreamSocketImpl::available` (`base/poco/NetSSL_OpenSSL/src/SecureStreamSocketImpl.cpp:174`)
forwards to `SecureSocketImpl::available` (`SecureSocketImpl.cpp:406-411`), which returns
`SSL_pending(_pSSL)`: decrypted plaintext already extracted from processed records, not the TCP
FIONREAD ciphertext count. A plaintext read of `remaining` bytes can therefore block even when the
TCP queue looks full: `SSL_read` (`SecureSocketImpl.cpp:381-387`) loops through `mustRetry` until a
record completes, and the record carrying the final plaintext bytes may still be partially in
flight; a renegotiation or a post-handshake message can also force more socket traffic. Whether a
partial record is outstanding is not observable through Poco's API.

The v2 design is correct under TLS by construction: it only consumes bytes already sitting in
`HTTPSession`'s plaintext buffer (`_pCurrent.._pEnd`) and never calls into the socket, so the TLS
layer is not involved at all. ClickHouse builds https S3 sessions from
`EndpointConnectionPool<Poco::Net::HTTPSClientSession>` (`src/Common/HTTPConnectionPool.cpp:978`,
reached through `makeHTTPSession`, `src/IO/HTTPCommon.cpp:52-61`), i.e. a `SecureStreamSocket`.
Any FIONREAD-based extension is unsound there and must be restricted to non-secure sockets;
`SSL_pending` alone is not a sufficient substitute because it says nothing about the incomplete
record that may hold the tail.

### 4. How often can v2 fire? (the decisive finding) {#how-often-can-v2-fire-the-decisive-finding}

Sizes. `HTTP_DEFAULT_BUFFER_SIZE` is 8 KiB (`base/poco/Net/include/Poco/Net/HTTPBasicStreamBuf.h:29`).
`HTTPSession::refill` (`HTTPSession.cpp:219-228`) is the *only* writer of the session buffer, and it
is called from exactly two places, `HTTPSession::get` (`:142`) and `peek` (`:154`). Those are used
only by `HTTPHeaderStreamBuf::readFromDevice`, which parses the response head byte by byte
(`HTTPHeaderStream.cpp:41-62`). `HTTPSession::read` (`:163-174`) returns the buffered leftover if
any and otherwise calls `receive` straight into the caller's memory - it never refills.

Consequence: the session buffer is filled once per response, by the header parse, with the result of
one `recv` of at most 8192 bytes. After the headers (about 330 bytes for the RustFS 206 above) the
leftover is B0 <= ~7.8 KiB. ClickHouse then reads the body through
`ReadBufferFromIStream::nextImpl` (`src/IO/ReadBufferFromIStream.cpp:24-35`), which bypasses the
stream buffer and calls `readFromDevice` in a loop until its whole buffer is full - 1 MiB by default
(`remote_fs_settings.buffer_size = DBMS_DEFAULT_BUFFER_SIZE = 1 MiB`, `src/IO/ReadSettings.h:29`,
`src/Core/Defines.h:22`; shrunk to the file size by `ReadSettings::adjustBufferSize`,
`src/IO/ReadSettings.cpp:40-49`). The first such loop consumes B0 and then reads from the socket, so
from the first body read onward `_session.buffered()` is 0 for the rest of the response.

`tryDrainBufferedRemainder` requires `remaining <= _session.buffered()`. It can therefore fire only
while the body has not been read at all (or, marginally, when the consumer buffer is smaller than
B0), and only when the entire body fits in the header `recv`: **the reachable ceiling is about
7.8 KiB, and 0 bytes once any body read has happened**. The #2332 pattern is the opposite case: a
compact-part column read stops mid-granule with up to `remote_read_min_bytes_for_seek` = 4 * 1 MiB =
4 MiB left (`src/Core/Settings.cpp:6524`) after at least one 1 MiB body read, where `buffered()` is
0 by construction. Predicted conversion of #2332 resets into reuse: **near zero**, with
`ReadBufferFromS3DrainedBeforeRelease` staying near zero on the repro. The cheap decisive experiment
is exactly that counter next to `DiskConnectionsReset` on the 229-part repro; it should be run
before any further work on this patch.

Reaching the rest needs one of: a socket-touching drain (2), a much larger session buffer (a
per-response 4 MiB buffer is not acceptable memory-wise), or not creating the remainder in the first
place - ending the HTTP range at the granule boundary via `MergeTreeReaderStream::adjustRightMark`
(`src/Storages/MergeTree/MergeTreeReaderStream.cpp:231`) and its
`setReadUntilPosition` (`:255`) feeding `ReadBufferFromS3::setReadUntilPosition`
(`src/IO/ReadBufferFromS3.cpp:500`).

### 5. Ranking {#ranking}

1. **(d) fix the read range.** Removes the remainder instead of paying to discard it, so it is the
   only option whose benefit does not depend on TLS, buffer sizes or timing; sound everywhere;
   upstream-portable and touches no vendored Poco. Cost: the hardest to get right, and it needs the
   measurement of where the over-long ranges come from.
2. **(c) bounded blocking drain with a deadline and a byte cap.** The only option that actually
   reaches the multi-MiB remainders. Sound under TLS (it goes through `SSL_read` normally) but it
   spends wall time on a hot path and needs a real cancellation-aware deadline; Codex finding 4 on
   v1 applies and must be answered with a deadline, not only a byte cap.
3. **(a) v2 as is.** Sound, allocation-free, cannot block, correct under TLS by construction, and
   small enough to be upstreamable. But by the analysis in 4 its expected effect on #2332 is
   negligible, so it should not be merged as "the fix" for #2332 without the counter evidence.
4. **(b) v2 + FIONREAD for plain TCP only.** Adds complexity and a TLS-asymmetric code path for a
   reach still bounded by `SO_RCVBUF`, well under 4 MiB; unsound if ever allowed on
   `SecureStreamSocket`. Worst value per risk.
