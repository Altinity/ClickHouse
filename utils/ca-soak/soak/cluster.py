"""Minimal, dependency-free cluster helper for the CA soak harness.

Queries ClickHouse over its HTTP interface using ONLY the Python stdlib (`urllib.request`) -- the
harness must run WITHOUT `pip install`, so we deliberately do NOT import `clickhouse-connect`.

`Node` is a single replica's HTTP endpoint. `Cluster` exposes the two replicas (ch1 :8123,
ch2 :8124 by default; configurable via constructor or env) plus a `docker_exec` helper.
"""

import os
import subprocess
import time
import urllib.error
import urllib.request

# ClickHouse error code ABORTED (Common/ErrorCodes.cpp). The publish-time resurrect-vs-GC race
# (B137) now throws this as a RETRYABLE transient ("retry the operation") instead of a hard
# FILE_DOESNT_EXIST. No server layer retries it on the async-insert flush path, so the writer
# (this harness) must retry the INSERT. A retried identical INSERT is idempotent thanks to
# ReplicatedMergeTree block-dedup, and the model has already applied the op exactly once.
ABORTED_CODE = 236


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

_DEFAULTS = {
    "node1_host": "localhost", "node1_port": 8123, "node1_container": "ca-soak-ch1-1",
    "node2_host": "localhost", "node2_port": 8124, "node2_container": "ca-soak-ch2-1",
    "gc_interval_s": 30,
    # CA-disk GC retire grace, in seconds. Mirrors content_addressed_gc_grace_sec in
    # configs/storage_conf.xml (currently 5). An unreachable object is not reclaimed until it has
    # spent at least this long retired, so the GC-fixpoint poll must allow grace + several GC
    # intervals before declaring a non-reclaiming leak.
    "gc_grace_sec": 5,
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

    def query(self, sql: str, timeout: float | None = None) -> str:
        """POST `sql` and return the raw response body (TabSeparated text), trailing newline stripped.
        `timeout` overrides the default socket timeout for this call (used for intentionally-blocking
        admin ops such as `SYSTEM SYNC REPLICA`, whose server-side wait can exceed the default)."""
        data = sql.encode("utf-8")
        req = urllib.request.Request(self.url, data=data, method="POST")
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

    def command(self, sql: str, timeout: float | None = None) -> None:
        """Execute a statement expected to return no rows (DDL/DML)."""
        self.query(sql, timeout=timeout)

    def scalar(self, sql: str) -> str:
        """Execute a query expected to return a single value; return it as a string."""
        return self.query(sql).strip()

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
        self.gc_grace_sec = cfg("gc_grace_sec")

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
