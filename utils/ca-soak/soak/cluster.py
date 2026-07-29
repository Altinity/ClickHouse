"""Minimal, dependency-free cluster helper for the CA soak harness.

Queries ClickHouse over its HTTP interface using ONLY the Python stdlib (`urllib.request`) -- the
harness must run WITHOUT `pip install`, so we deliberately do NOT import `clickhouse-connect`.

`Node` is a single replica's HTTP endpoint. `Cluster` exposes the two replicas (ch1 :8123,
ch2 :8124 by default; configurable via constructor or env) plus a `docker_exec` helper.
"""

import os
import socket
import subprocess
import time
import urllib.error
import urllib.parse
import urllib.request

# ClickHouse error code ABORTED (Common/ErrorCodes.cpp). The publish-time resurrect-vs-GC race
# (B137) now throws this as a RETRYABLE transient ("retry the operation") instead of a hard
# FILE_DOESNT_EXIST. No server layer retries it on the async-insert flush path, so the writer
# (this harness) must retry the INSERT. A retried identical INSERT is idempotent thanks to
# ReplicatedMergeTree block-dedup, and the model has already applied the op exactly once.
ABORTED_CODE = 236
# ClickHouse error code NETWORK_ERROR: carries the CAS "mount lease not held" refusal (see
# `is_mount_fenced`). CAS mints its transient-unavailability refusals in this class deliberately --
# it is the class upstream's own classifiers already treat as retry-later, so a lease blip cannot be
# mistaken for damage (see `throwCasTransientUnavailable`).
NETWORK_ERROR_CODE = 210

# ClickHouse error code TABLE_IS_READ_ONLY (Common/ErrorCodes.cpp). A ReplicatedMergeTree replica
# transiently becomes read-only while it re-establishes its ZooKeeper session after a fault (docker
# kill/restart/pause). The window typically lasts ~tens of seconds and the replica RECOVERS
# automatically once the new ZK session is confirmed. Keeper-level admin ops such as
# `SYSTEM SYNC REPLICA` that are issued during/just-after a chaos fault window can hit this transient
# and must RETRY rather than surface as a hard WORKLOAD FAILURE. See B155.
TABLE_IS_READ_ONLY_CODE = 242

# Server-side exception codes that mean "this node is going down / its network is broken" rather than
# "your query is wrong". Under Phase-2 chaos a `docker restart`/`docker stop` shuts a node down
# GRACEFULLY: an in-flight query is then CANCELLED server-side (returns an HTTP 500 body with one of
# these codes) instead of the TCP connection simply dropping. These are the node-down-adjacent twin of
# a raw connection refused/reset -- the same recovery applies (retry with backoff, reroute to the
# other replica on a ReplicatedMergeTree). They are DISTINCT from a logic error (UNKNOWN_TABLE, type
# errors, ...) which must surface immediately, and from the B137 retryable ABORTED (handled by
# `retry_on_aborted`).
#   394 QUERY_WAS_CANCELLED            -- in-flight query cancelled by a graceful shutdown
#   209 SOCKET_TIMEOUT, 210 NETWORK_ERROR -- the node's network went away mid-query
#   735 QUERY_WAS_CANCELLED_BY_CLIENT  -- cancellation surfaced via the client-cancel path
NODE_DOWN_CODES = (394, 209, 210, 735)

# ClickHouse error code KEEPER_EXCEPTION (Common/ErrorCodes.cpp). Under chaos a node that is frozen or
# paused PAST its Keeper session timeout (e.g. a `freeze_long`/`pause` fault longer than the ~tens-of-
# seconds session TTL) has its ZooKeeper/Keeper session EXPIRED or an in-flight Keeper RPC stalls while
# that session is dying/re-establishing. Two shapes have been observed so far:
#   * `Coordination::Exception: Session expired. (KEEPER_EXCEPTION)` -- e.g. `OPTIMIZE TABLE` (which
#     enqueues a merge entry in Keeper) issued during that window. See B190.
#   * `Coordination::Exception: Coordination error: Operation timeout, path ... (KEEPER_EXCEPTION)` --
#     e.g. `ALTER ... DELETE`/`UPDATE` (which creates a mutation entry under .../mutations) racing the
#     same window. `Coordination::Exception::fromPath` (`src/Common/ZooKeeper/KeeperException.h`)
#     formats ALL path-bearing Keeper errors as `"Coordination error: {message}, path {path}"`, so the
#     generic `"Coordination error"` prefix is matched too and covers OTHER not-yet-observed
#     path-bearing Keeper-error variants (session moved, connection loss, ...) the same way.
# The replica re-establishes a fresh session automatically within tens of seconds in both shapes. This
# is the Keeper-coordination twin of the TABLE_IS_READ_ONLY transient (B155); EXPECTED under chaos.
# Originally only the `Session expired` shape was recognized, and only wired into the best-effort
# OPTIMIZE path (no model effect, swallow-and-drop; B190). Diagnosed in
# `.superpowers/sdd/task3v2-chaos-diag-report.md`: a chaos-window `ALTER ... DELETE` hit the
# `Operation timeout` shape on the MUTATION path (`apply_barrier`, run.py), which had NO Keeper-transient
# tolerance at all (not even the narrower `Session expired` case) despite `apply_barrier`'s own
# docstring claiming transport-resilience -- aborting the whole run instead of self-healing in ~70s.
KEEPER_EXCEPTION_CODE = 999

# ClickHouse error code S3_ERROR (Common/ErrorCodes.cpp). Under chaos the RustFS object-store
# container is PAUSED (docker pause, 5..60s) or RESTARTED — while it is unreachable, any server-side
# S3 request that a client op depends on can fail and surface to the client as an HTTP 500 carrying
# `Code: 499 ... (S3_ERROR)`. The CAS conditional-write path makes this especially sharp: every CAS
# conditional PUT (part manifests, ref-log appends, ...) deliberately runs on a SINGLE-ATTEMPT S3
# client (RFC cas-s3-timeout-retry-control; `ObjectStorageBackend::conditionalWriteSettings`,
# src/.../ContentAddressed/Backend/CasObjectStorageBackend.cpp) whose one attempt gets the ADAPTIVE
# first-attempt timeout (~3s receive for a small PUT, src/IO/ConnectionTimeouts.cpp) — so any S3
# blip longer than ~3s fails a CAS write with zero server-side retries, where upstream non-CAS S3
# traffic would have retried through it (~500 attempts). Diagnosed on the task3 v3 2h soak
# (tmp/task3_soak_2h_v3.log:518): a 19s `rustfs pause` fault made an INSERT's part-manifest PUT
# (`Build::stageManifest`) time out after ONE 3s attempt and the un-retried S3_ERROR aborted the run.
S3_ERROR_CODE = 499

# TRANSPORT flavors of an S3_ERROR body — the store was unreachable / the connection died, saying
# NOTHING about data. These self-heal when the fault window ends (rustfs pause <= 60s, restart takes
# seconds), so they are retryable exactly like node-down:
#   Timeout            -- docker pause: the store freezes mid-request (the observed v3 shape)
#   Connection refused -- rustfs restarting: TCP port not yet listening
#   Connection reset   -- rustfs going down mid-request
#   Broken pipe        -- rustfs closed the socket mid-body (the B187-documented shape)
#   DNS error          -- container-name resolution blips while docker recreates the container
_S3_TRANSIENT_FLAVORS = ("Timeout", "Connection refused", "Connection reset", "Broken pipe", "DNS error")

# CORRECTNESS-signal markers: if any of these appears in the body the error is NEVER retried, even
# when a transport flavor word is also present. Each one is a real bug/damage signal, not weather:
#   NoSuchKey / NO_SUCH_KEY   -- S3 "object absent" SEMANTICS: on the CAS read path a live ref naming
#                                a missing object is INV-NO-DANGLE (durability loss); retrying would
#                                mask data loss (and revival-by-retry violates the CA resurrect
#                                invariant — condemned objects are never read back to life).
#   AccessDenied / ACCESS_DENIED -- auth/config failure: deterministic, a retry can only mask it.
#   PreconditionFailed        -- a conditional-write 412 leaking RAW to a client is a CAS protocol
#                                bug (the CAS layer must resolve conflicts internally), not weather.
#   LOGICAL_ERROR             -- a product invariant broke; must surface immediately.
#   CORRUPTED_DATA            -- decode/integrity failure; retrying re-reads the same bad bytes.
# (Wrong-result classes never reach this classifier at all: they surface as CheckpointFailure from
# the model comparison, not as a QueryError.)
_S3_CORRECTNESS_MARKERS = (
    "NoSuchKey", "NO_SUCH_KEY", "AccessDenied", "ACCESS_DENIED",
    "PreconditionFailed", "LOGICAL_ERROR", "CORRUPTED_DATA",
)


class QueryError(RuntimeError):
    """A ClickHouse HTTP query failed; carries the server-side exception text from the response body
    (ClickHouse returns its full exception message in the body of a non-2xx HTTP response)."""

    def __init__(self, node, code, body, sql):
        self.code = code
        self.body = body
        self.sql = sql
        snippet = sql if len(sql) <= 200 else sql[:200] + "...(%d more chars)" % (len(sql) - 200)
        super().__init__(f"{node} HTTP {code}: {body.strip()} | sql={snippet}")

    @property
    def is_aborted(self) -> bool:
        """True if the server-side exception is the retryable ABORTED transient (code 236).
        Detected by parsing the exception body the server returns in the HTTP response."""
        b = self.body or ""
        return ("Code: %d" % ABORTED_CODE) in b or "ABORTED" in b

    @property
    def is_readonly(self) -> bool:
        """True if the server-side exception is the TABLE_IS_READ_ONLY transient (code 242).
        A ReplicatedMergeTree replica becomes read-only while re-establishing its ZooKeeper session
        after a chaos fault (kill/restart/pause); it recovers automatically within tens of seconds.
        Detected by parsing the ClickHouse exception body in the HTTP response."""
        b = self.body or ""
        return ("Code: %d" % TABLE_IS_READ_ONLY_CODE) in b or "TABLE_IS_READ_ONLY" in b

    @property
    def is_keeper_transient(self) -> bool:
        """True if the server-side exception is a Keeper-coordination transient (code 999
        KEEPER_EXCEPTION): a node frozen/paused past its Keeper session TTL under chaos either lost its
        session (`Session expired`) or stalled an in-flight Keeper RPC while that session was
        dying/re-establishing (`Operation timeout`, or any other path-bearing Keeper error, all of
        which share the generic `Coordination error` prefix -- see the `KEEPER_EXCEPTION_CODE` comment
        above). The replica re-establishes a fresh session and self-heals within tens of seconds either
        way; this must RETRY/reroute rather than surface as a hard WORKLOAD FAILURE. The
        Keeper-coordination twin of `is_readonly` (B155). Originally `is_keeper_session_expired`
        (`Session expired` only, B190); broadened to cover `Operation timeout` per
        `.superpowers/sdd/task3v2-chaos-diag-report.md` (a chaos-window `ALTER ... DELETE` hit this
        exact variant on the mutation path, which the narrower classifier would not have matched
        either). Detected by parsing the ClickHouse exception body in the HTTP response."""
        b = self.body or ""
        if ("Code: %d" % KEEPER_EXCEPTION_CODE) not in b:
            return False
        return "Session expired" in b or "Operation timeout" in b or "Coordination error" in b

    # Back-compat alias: `is_keeper_session_expired` was the original (B190) name, matching only the
    # `Session expired` shape. Kept pointing at the same (now-broadened) property so any call site
    # still spelled the old way keeps working unchanged.
    is_keeper_session_expired = is_keeper_transient

    @property
    def is_s3_transient(self) -> bool:
        """True if the server-side exception is a TRANSPORT-flavored S3_ERROR (code 499): the S3
        backend (RustFS) was unreachable — paused/restarting under chaos — while the server needed it
        for this op. Matched ONLY when a transport flavor (`Timeout`/`Connection refused`/`Connection
        reset`/`Broken pipe`/`DNS error`) is present AND no correctness marker is (`NoSuchKey`,
        `AccessDenied`, raw `PreconditionFailed`, `LOGICAL_ERROR`, `CORRUPTED_DATA` — see
        `_S3_CORRECTNESS_MARKERS` for why each must stay fail-fast). Self-heals when the fault window
        ends (rustfs pause <= 60s), so it gets the same bounded retry + reroute as node-down.
        Diagnosed on the task3 v3 2h soak (tmp/task3_soak_2h_v3.log:518): a 19s `rustfs pause` made an
        INSERT's CAS part-manifest conditional PUT fail after its deliberately-single 3s attempt
        (RFC cas-s3-timeout-retry-control) and the un-retried S3_ERROR aborted the whole run.
        Detected by parsing the ClickHouse exception body in the HTTP response."""
        b = self.body or ""
        if ("Code: %d" % S3_ERROR_CODE) not in b and "S3_ERROR" not in b:
            return False
        if any(marker in b for marker in _S3_CORRECTNESS_MARKERS):
            return False
        return any(flavor in b for flavor in _S3_TRANSIENT_FLAVORS)

    @property
    def is_node_down(self) -> bool:
        """True if the server-side exception is a NODE-DOWN-adjacent transient (a graceful shutdown
        cancelling an in-flight query, or a mid-query network failure) -- one of `NODE_DOWN_CODES`.
        Under chaos this is the body-bearing twin of a dropped connection and is retried/rerouted the
        same way. Excludes the B137 ABORTED (which has its own retry path)."""
        b = self.body or ""
        if self.is_aborted:
            return False
        return any(("Code: %d." % c) in b for c in NODE_DOWN_CODES)

    @property
    def is_mount_fenced(self) -> bool:
        """True if the server-side exception is a CAS mount-fence ABORTED (code 236): this replica's
        mount lease expired / it was GC-fenced (e.g. frozen/paused past the mount-lease TTL), so it
        REFUSES to mutate its ref shard until it self-remounts. Unlike the B137 retryable ABORTED (a
        resurrect-vs-GC race that clears in ms on the SAME node), a fence persists for the WHOLE outage,
        so hammering the same node within the tiny `retry_on_aborted` budget is futile. The correct
        recovery is to REROUTE the write to the healthy peer (which shares the pool and holds its own
        live lease) — the same recovery as node-down. Detected by the fence message in the body."""
        b = self.body or ""
        if self.is_aborted:
            return ("mount lost" in b or "lease expired" in b or "refusing to mutate ref shard" in b)
        # The same condition also arrives as NETWORK_ERROR (210), not ABORTED: the rev.8 disk-lifecycle
        # round made the lease-loss gate THROW a typed not-mounted error instead of aborting, and the
        # transient-classifier round then moved that throw off INVALID_STATE (668) onto the retry-later
        # class so no consumer reads a lease blip as damage. Recognising only 236 here is what turned a
        # routine chaos-window fence into a WORKLOAD FAILURE and killed a 2.5h soak on 2026-07-24: the
        # driver has always known how to handle a fence (reroute to the peer, which holds its own live
        # lease), it just no longer recognised it. Matched on the code AND the message: a genuine socket
        # failure shares the code and must NOT be treated as a fence.
        return ("Code: %d." % NETWORK_ERROR_CODE) in b and "mount lease not held" in b

# Port/container convention for replica i (1-based): ch1=8123, ch2=8124, ..., chN=8122+N;
# container ca-soak-ch{i}-1. Matches docker-compose.yml (2 nodes) and docker-compose-10replicas.yml
# (ch1..ch10 -> 8123..8132). Per-node overrides via env CA_SOAK_NODE{i}_{HOST,PORT,CONTAINER}.
def _node_cfg(i: int, field: str, default):
    env = os.environ.get(f"CA_SOAK_NODE{i}_{field.upper()}")
    if env is not None:
        return type(default)(env) if not isinstance(default, str) else env
    return default


_DEFAULTS = {
    "node1_host": "localhost", "node1_port": 8123, "node1_container": "ca-soak-ch1-1",
    "node2_host": "localhost", "node2_port": 8124, "node2_container": "ca-soak-ch2-1",
    # Background GC tick period, in seconds. MUST mirror gc_interval_sec in
    # configs/storage_conf.xml (currently 2) — the servers' CasGcScheduler makes one reclaim round
    # per tick (only the lease holder progresses), so this is the sole pacing knob the GC-fixpoint
    # poll uses to scale its bound to the backlog. There is NO core retire-grace throttle:
    # content_addressed_gc_grace_sec is inert (not read by the core), so no gc_grace_sec here.
    "gc_interval_s": 2,
}


class Node:
    # Default socket timeout is deliberately generous: an INSERT's async-insert flush
    # (`WaitForAsyncInsert`) can block well beyond a minute while the publish path retries through the
    # resurrect-vs-GC race (B137), and OPTIMIZE under merge churn is similarly slow. A tight timeout
    # turns a slow-but-progressing op into a spurious socket TimeoutError. The overall run is still
    # bounded by the `timeout` wrapping `run_phase1.sh`, so this is transient tolerance, not a hang mask.
    def __init__(self, host: str, port: int, container: str | None = None, timeout: float = 300.0,
                 database: str = "default"):
        self.host = host
        self.port = port
        self.container = container
        self.timeout = timeout
        self.database = database

    @property
    def url(self) -> str:
        return f"http://{self.host}:{self.port}/"

    def query(self, sql: str, timeout: float | None = None, settings: dict | None = None) -> str:
        """POST `sql` and return the raw response body (TabSeparated text), trailing newline stripped.
        `timeout` overrides the default socket timeout for this call (used for intentionally-blocking
        admin ops such as `SYSTEM SYNC REPLICA`, whose server-side wait can exceed the default).
        `settings` are passed as URL query params (ClickHouse reads per-query settings from the URL),
        used to align the SERVER-side bound (`receive_timeout`/`max_execution_time`) with the client
        socket timeout for blocking admin ops so a slow-but-progressing large-pool op is not tripped by
        a server-side HTTP-408 `TIMEOUT_EXCEEDED`. `self.database` is always sent as the default
        database so bare table names resolve there (the dedicated CAS soak database)."""
        params = {"database": self.database}
        if settings:
            params.update(settings)
        url = self.url + "?" + urllib.parse.urlencode(params)
        data = sql.encode("utf-8")
        req = urllib.request.Request(url, data=data, method="POST")
        try:
            with urllib.request.urlopen(req, timeout=timeout or self.timeout) as resp:
                return resp.read().decode("utf-8").rstrip("\n")
        except urllib.error.HTTPError as e:
            body = ""
            try:
                body = e.read().decode("utf-8", "replace")
            except Exception:
                pass
            raise QueryError(self, e.code, body, sql) from e

    def command(self, sql: str, timeout: float | None = None, settings: dict | None = None) -> None:
        """Execute a statement expected to return no rows (DDL/DML)."""
        self.query(sql, timeout=timeout, settings=settings)

    def scalar(self, sql: str) -> str:
        """Execute a query expected to return a single value; return it as a string."""
        return self.query(sql).strip()

    def ping(self, timeout: float = 2.0) -> bool:
        """Return True iff the node answers `/ping` with HTTP 200 ("Ok.\\n"). Used by the Phase-2
        recovery wait to confirm a killed/restarted node is HTTP-healthy again before checkpointing.
        Any transport error or non-2xx means not-yet-healthy -> False (the caller polls with a bound;
        a node that never returns is failed loudly there, not swallowed here)."""
        req = urllib.request.Request(f"http://{self.host}:{self.port}/ping", method="GET")
        try:
            with urllib.request.urlopen(req, timeout=timeout) as resp:
                return resp.status == 200
        except Exception:
            return False

    def __repr__(self) -> str:
        return f"Node({self.host}:{self.port})"


def retry_on_aborted(fn, *, attempts: int = 6, backoff_s: float = 0.05, on_retry=None):
    """Call `fn` (a no-arg callable performing one INSERT) and retry it on a retryable ABORTED
    (code 236) QueryError, up to `attempts` total tries with a tiny linear backoff. A persistent
    ABORTED after exhausting the budget is re-raised as a real failure. Any non-ABORTED QueryError
    (or other exception) is raised immediately without retry.

    Scope: INSERTs only. The retried INSERT is idempotent (ReplicatedMergeTree block-dedup), so a
    transient resurrect-vs-GC race converges without double-applying rows."""
    last = None
    for attempt in range(1, attempts + 1):
        try:
            return fn()
        except QueryError as e:
            if not e.is_aborted:
                raise
            # A mount-fence ABORTED (lease expired / GC-fenced replica) will NOT clear on this node
            # within the tiny same-node budget — the replica stays fenced for the whole outage. Re-raise
            # it immediately so the outer transport-retry REROUTES the write to the healthy peer instead
            # of wasting the budget hammering the fenced node (which caused the FREEZE_LONG WORKLOAD
            # FAILURE). The B137 transient ABORTED (resurrect-vs-GC race) still retries on the same node.
            if e.is_mount_fenced:
                raise
            last = e
            if attempt < attempts:
                if on_retry is not None:
                    on_retry(attempt, e)
                time.sleep(backoff_s * attempt)
    raise last


def is_transport_error(exc: BaseException) -> bool:
    """Classify an exception raised while talking to a node as a TRANSPORT-level failure -- the node
    was unreachable (down/paused/restarting), as opposed to a `QueryError` (the server WAS reachable
    and returned an HTTP error body, e.g. the B137 retryable ABORTED).

    Phase-2 chaos KILLs/PAUSEs/RESTARTs a node mid-op; the in-flight HTTP call then fails with a
    connection refused/reset/timeout BEFORE any HTTP response is produced. `urllib` surfaces these as
    a `urllib.error.URLError` whose `.reason` is the underlying `OSError` (`ConnectionRefusedError`,
    `ConnectionResetError`, `socket.timeout`, ...), or directly as an `OSError`/`socket.timeout` on a
    socket-level timeout. A `urllib.error.HTTPError` is NOT a transport error (the server responded);
    that is wrapped into a `QueryError` and handled separately.

    Pure function (no I/O) so the classification is unit-testable without docker."""
    # HTTPError is a subclass of URLError but means the server responded -> not transport.
    if isinstance(exc, urllib.error.HTTPError):
        return False
    if isinstance(exc, QueryError):
        return False
    if isinstance(exc, urllib.error.URLError):
        reason = getattr(exc, "reason", None)
        if isinstance(reason, BaseException):
            return is_transport_error(reason)
        return True   # a URLError without an HTTP response is a connection-level failure
    if isinstance(exc, (ConnectionError, socket.timeout, TimeoutError)):
        return True
    if isinstance(exc, OSError):
        return True
    return False


def is_node_down(exc: BaseException) -> bool:
    """A node-down failure under chaos comes in TWO shapes: a connection-level transport error (the
    socket dropped -- `is_transport_error`), OR a server-side `QueryError` carrying a node-down code
    (`QueryError.is_node_down` -- a graceful shutdown cancelled the in-flight query, e.g.
    `QUERY_WAS_CANCELLED`/`NETWORK_ERROR`). Both get the same recovery: bounded retry + reroute to the
    other replica. A logic `QueryError` (UNKNOWN_TABLE, ...) and the B137 ABORTED are NOT node-down."""
    if is_transport_error(exc):
        return True
    if isinstance(exc, QueryError):
        return exc.is_node_down
    return False


def is_readonly(exc: BaseException) -> bool:
    """A ReplicatedMergeTree replica is transiently read-only (TABLE_IS_READ_ONLY, code 242) while it
    re-establishes its ZooKeeper session after a chaos fault -- especially a `both pause` that drops
    BOTH replicas' Keeper sessions, so neither reroute target is writable until they reconnect. This
    is a TRANSIENT, not a logic error, and gets the same recovery as node-down (bounded retry +
    reroute + backoff). The retry is safe: a readonly-rejected INSERT never committed (RMT block-dedup
    keeps the rerouted retry idempotent), and OPTIMIZE has no model effect."""
    return isinstance(exc, QueryError) and exc.is_readonly


def is_mount_fenced(exc: BaseException) -> bool:
    """A CAS mount-fence ABORTED (`QueryError.is_mount_fenced`): the target replica's mount lease
    expired / it was GC-fenced (frozen past the mount-lease TTL) and refuses to mutate its ref shard
    until it self-remounts. This persists for the whole outage, so — like node-down — the recovery is
    to REROUTE to the other replica (shared pool, own live lease) with bounded backoff, NOT to hammer
    the fenced node. Safe: the rejected INSERT never committed (RMT block-dedup keeps the rerouted
    retry idempotent), and a fenced OPTIMIZE has no model effect."""
    return isinstance(exc, QueryError) and exc.is_mount_fenced


def is_keeper_transient(exc: BaseException) -> bool:
    """A Keeper-coordination transient (`QueryError.is_keeper_transient`, code 999 KEEPER_EXCEPTION):
    the target replica's Keeper session expired (`Session expired`) or an in-flight Keeper RPC stalled
    while that session was dying/re-establishing (`Operation timeout` and other path-bearing Keeper
    errors, all sharing the generic `Coordination error` prefix). Self-heals within tens of seconds
    once the node re-establishes its session, so it gets the same bounded retry + reroute recovery as
    node-down/readonly/mount-fenced. Previously only recognized (as `is_keeper_session_expired`) for
    the `Session expired` shape and wired only into the best-effort OPTIMIZE path (B190); broadened and
    added HERE (to `retry_on_transport`, and therefore `apply_barrier`'s mutation path) per
    `.superpowers/sdd/task3v2-chaos-diag-report.md`, whose diagnosed failure was exactly an
    `Operation timeout` KEEPER_EXCEPTION on `ALTER ... DELETE` going unretried and aborting the run."""
    return isinstance(exc, QueryError) and exc.is_keeper_transient


def is_s3_transient(exc: BaseException) -> bool:
    """A transport-flavored S3_ERROR transient (`QueryError.is_s3_transient`, code 499): the S3
    backend was unreachable (rustfs pause/restart chaos fault) while the server needed it — most
    sharply on the CAS conditional-write path, whose single-attempt client (RFC
    cas-s3-timeout-retry-control) turns any >~3s S3 blip into an immediate client-visible S3_ERROR
    with zero server-side retries. The store comes back within the fault window (pause <= 60s), so
    this gets the same bounded retry + reroute recovery as node-down/readonly/mount-fenced/
    keeper-transient. S3 SEMANTIC errors (NoSuchKey, AccessDenied), raw conditional-write conflicts
    (PreconditionFailed) and integrity failures (LOGICAL_ERROR, CORRUPTED_DATA) are explicitly NOT
    matched — those are correctness signals and stay fail-fast (see `_S3_CORRECTNESS_MARKERS`).

    Retry safety on the INSERT path: the outcome of a timed-out conditional PUT is AMBIGUOUS (the
    write may have landed). The retried INSERT re-sends the byte-identical statement (the sql string
    is built once per op from (seed, op_id) and captured by `_insert_with_retry`), and the soak runs
    SYNC inserts with `deduplicate_blocks=true` (B138): if the first attempt actually committed
    server-side, the retry dedups against its block hash (window `replicated_deduplication_window` =
    10000 blocks, >= ~15 min of headroom at the observed ~11 ops/s — far beyond the retry budget);
    if it never committed (the observed v3 case: the failed INSERT's transaction was undone,
    tmp/soak_v3_evidence/ch2/clickhouse-server.log:2021560 `Undoing transaction`), the retry truly
    re-inserts. Either way the model applied the op exactly once. Added per the task3 v3 soak failure
    (tmp/task3_soak_2h_v3.log:518, 19s `rustfs pause` -> un-retried S3_ERROR Timeout on an INSERT)."""
    return isinstance(exc, QueryError) and exc.is_s3_transient


def is_aborted(exc: BaseException) -> bool:
    """A retryable ABORTED (code 236) that PERSISTED past `retry_on_aborted`'s tiny same-node budget
    (6 tries x 0.05s -- sized for the sub-second B137 resurrect-vs-GC race). Post-stagefix
    (c3d9aa9d8d6) an exhausted manifest-PUT controller budget also surfaces as ABORTED, and under a
    COMPOUND chaos window (20m-i3: ch1 freeze 76s -> rustfs pause -> rustfs restart -> ch2 freeze,
    back-to-back) it persists for the whole outage -- far beyond the inner budget, well inside the
    transport envelope. Two-tier design: the inner loop absorbs the fast race; when it exhausts and
    re-raises, THIS predicate lets `retry_on_transport` absorb the long tail with reroute + the
    shared ~287s capped-exponential budget. A genuinely wedged ABORTED still fails after that bound.
    Retry safety: identical to `is_s3_transient` above (byte-identical INSERT, RMT block-dedup)."""
    return isinstance(exc, QueryError) and exc.is_aborted


def retry_on_transport(fn, *, attempts: int, backoff_s: float = 0.5, max_backoff_s: float = 8.0,
                       on_retry=None, sleep_fn=time.sleep):
    """Call `fn` and retry it on a NODE-DOWN failure (`is_node_down`: a connection-level transport
    error OR a graceful-shutdown cancellation/network `QueryError`), a transient `is_readonly`/
    `is_mount_fenced`/`is_keeper_transient`/`is_s3_transient` `QueryError`, with bounded,
    capped-exponential backoff, up to `attempts` total tries. A persistent failure after the budget is
    exhausted is re-raised -- per the task spec, a node that never comes back within a generous bound
    IS a failure (the feature must survive crash+restart). Other exceptions (a logic `QueryError`,
    ...) propagate IMMEDIATELY so the caller's own handling sees them unmasked. ABORTED (236) is
    two-tier: `retry_on_aborted` absorbs the sub-second B137 race on the same node; only when that
    inner budget exhausts does the re-raised ABORTED land here for the long-tail treatment
    (see `is_aborted`).

    Budget note: with the caller's default `attempts=TRANSPORT_ATTEMPTS` (40, `run.py`) and this
    function's default `backoff_s=0.5`/`max_backoff_s=8.0`, the capped-exponential backoff sums to
    0.5+1+2+4 + 35*8 = ~287s (~4.8 minutes) of PURE SLEEP across the retry loop -- and each failed
    attempt's own call time (connect/receive timeouts while a node or the store is unreachable) adds
    on top, so the real wall-clock envelope is strictly larger. That covers the worst self-healing
    window the chaos module can create (`generate_chaos_schedule`, chaos.py): kill/restart/pause
    faults last 5..60s and freeze_long 60..90s, plus the worst OBSERVED recoveries — ~120s
    kill-to-serving boot (task3 v2 fault #8) and ~66-90s Keeper-session re-establishment (v2 diag) —
    stacking to ~270s < 287s even before per-attempt call time. The rustfs-fault classes are milder
    still: a rustfs pause (<= 60s) + mount-lease expiry + self-remount completes well inside the
    budget. Every retryable class therefore shares this ONE bounded budget.

    `sleep_fn` is injectable so the loop is pure-testable without real sleeps."""
    last = None
    for attempt in range(1, attempts + 1):
        try:
            return fn()
        except Exception as e:
            if not (is_node_down(e) or is_readonly(e) or is_mount_fenced(e) or is_keeper_transient(e)
                    or is_s3_transient(e) or is_aborted(e)):
                raise
            last = e
            if attempt < attempts:
                if on_retry is not None:
                    on_retry(attempt, e)
                sleep_fn(min(max_backoff_s, backoff_s * (2 ** (attempt - 1))))
    raise last


class Cluster:
    """N-replica cluster over the shared CA pool. Default 2 nodes (ch1/ch2) for the standard soak +
    the 2-node scenarios; `node_count` (or env CA_SOAK_NODE_COUNT) selects N for multi-replica
    scenarios such as S12 (10 replicas, docker-compose-10replicas.yml). `node1`/`node2` stay valid
    for all the 2-node callers; `nodes()` returns the full tuple so N-aware code addresses ch1..chN."""

    def __init__(self, node_count: int | None = None, database: str = "default", **kw):
        def cfg(name):
            env = os.environ.get("CA_SOAK_" + name.upper())
            if env is not None:
                d = _DEFAULTS[name]
                return type(d)(env) if not isinstance(d, str) else env
            return kw.get(name, _DEFAULTS[name])

        if node_count is None:
            node_count = int(os.environ.get("CA_SOAK_NODE_COUNT", "2"))
        if node_count < 1:
            raise ValueError(f"node_count must be >= 1, got {node_count}")
        self.node_count = node_count
        self._nodes = [
            Node(_node_cfg(i, "host", kw.get(f"node{i}_host", "localhost")),
                 _node_cfg(i, "port", kw.get(f"node{i}_port", 8122 + i)),
                 _node_cfg(i, "container", kw.get(f"node{i}_container", f"ca-soak-ch{i}-1")),
                 database=database)
            for i in range(1, node_count + 1)
        ]
        self.gc_interval_s = cfg("gc_interval_s")

    def nodes(self):
        return tuple(self._nodes)

    @property
    def node1(self) -> Node:
        return self._nodes[0]

    @property
    def node2(self) -> Node:
        # For a 1-node cluster this aliases node1 (no second replica exists).
        return self._nodes[1] if len(self._nodes) > 1 else self._nodes[0]

    def docker_exec(self, container: str, args: list[str]):
        """Run `docker exec <container> <args...>`; return (rc, stdout, stderr)."""
        p = subprocess.run(
            ["docker", "exec", container, *args],
            capture_output=True, text=True)
        return p.returncode, p.stdout, p.stderr


def classify_retry_error(exc: BaseException) -> str:
    """Availability-accounting label for a driver-retried error (see run.py AVAILABILITY report).
    Order matters: most-specific first; `node_down` covers plain transport-level failures."""
    if isinstance(exc, QueryError):
        if is_mount_fenced(exc):
            return "mount_fenced"
        if is_keeper_transient(exc):
            return "keeper_transient"
        if is_s3_transient(exc):
            return "s3_transient"
        if is_aborted(exc):
            return "aborted_persistent"
        if is_readonly(exc):
            return "readonly"
    if is_node_down(exc):
        return "node_down"
    return "other"
