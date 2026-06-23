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
    def is_node_down(self) -> bool:
        """True if the server-side exception is a NODE-DOWN-adjacent transient (a graceful shutdown
        cancelling an in-flight query, or a mid-query network failure) -- one of `NODE_DOWN_CODES`.
        Under chaos this is the body-bearing twin of a dropped connection and is retried/rerouted the
        same way. Excludes the B137 ABORTED (which has its own retry path)."""
        b = self.body or ""
        if self.is_aborted:
            return False
        return any(("Code: %d." % c) in b for c in NODE_DOWN_CODES)

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
    def __init__(self, host: str, port: int, container: str | None = None, timeout: float = 300.0):
        self.host = host
        self.port = port
        self.container = container
        self.timeout = timeout

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
        a server-side HTTP-408 `TIMEOUT_EXCEEDED`."""
        url = self.url
        if settings:
            url = url + "?" + urllib.parse.urlencode(settings)
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


def retry_on_transport(fn, *, attempts: int, backoff_s: float = 0.5, max_backoff_s: float = 8.0,
                       on_retry=None, sleep_fn=time.sleep):
    """Call `fn` and retry it on a NODE-DOWN failure (`is_node_down`: a connection-level transport
    error OR a graceful-shutdown cancellation/network `QueryError`) with bounded, capped-exponential
    backoff, up to `attempts` total tries. A persistent node-down after the budget is exhausted is
    re-raised -- per the task spec, a node that never comes back within a generous bound IS a failure
    (the feature must survive crash+restart). Other exceptions (a logic `QueryError`, the B137 ABORTED
    transient that has its own retry, ...) propagate IMMEDIATELY so the caller's own handling sees them
    unmasked.

    `sleep_fn` is injectable so the loop is pure-testable without real sleeps."""
    last = None
    for attempt in range(1, attempts + 1):
        try:
            return fn()
        except Exception as e:
            if not (is_node_down(e) or is_readonly(e)):
                raise
            last = e
            if attempt < attempts:
                if on_retry is not None:
                    on_retry(attempt, e)
                sleep_fn(min(max_backoff_s, backoff_s * (2 ** (attempt - 1))))
    raise last


class Cluster:
    def __init__(self, **kw):
        def cfg(name):
            env = os.environ.get("CA_SOAK_" + name.upper())
            if env is not None:
                d = _DEFAULTS[name]
                return type(d)(env) if not isinstance(d, str) else env
            return kw.get(name, _DEFAULTS[name])

        self._node1 = Node(cfg("node1_host"), cfg("node1_port"), cfg("node1_container"))
        self._node2 = Node(cfg("node2_host"), cfg("node2_port"), cfg("node2_container"))
        self.gc_interval_s = cfg("gc_interval_s")

    def nodes(self):
        return (self._node1, self._node2)

    @property
    def node1(self) -> Node:
        return self._node1

    @property
    def node2(self) -> Node:
        return self._node2

    def docker_exec(self, container: str, args: list[str]):
        """Run `docker exec <container> <args...>`; return (rc, stdout, stderr)."""
        p = subprocess.run(
            ["docker", "exec", container, *args],
            capture_output=True, text=True)
        return p.returncode, p.stdout, p.stderr
