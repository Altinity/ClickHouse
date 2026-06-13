"""Minimal, dependency-free cluster helper for the CA soak harness.

Queries ClickHouse over its HTTP interface using ONLY the Python stdlib (`urllib.request`) -- the
harness must run WITHOUT `pip install`, so we deliberately do NOT import `clickhouse-connect`.

`Node` is a single replica's HTTP endpoint. `Cluster` exposes the two replicas (ch1 :8123,
ch2 :8124 by default; configurable via constructor or env) plus a `docker_exec` helper.
"""

import os
import subprocess
import urllib.error
import urllib.request


class QueryError(RuntimeError):
    """A ClickHouse HTTP query failed; carries the server-side exception text from the response body
    (ClickHouse returns its full exception message in the body of a non-2xx HTTP response)."""

    def __init__(self, node, code, body, sql):
        self.code = code
        self.body = body
        self.sql = sql
        snippet = sql if len(sql) <= 200 else sql[:200] + "...(%d more chars)" % (len(sql) - 200)
        super().__init__(f"{node} HTTP {code}: {body.strip()} | sql={snippet}")

_DEFAULTS = {
    "node1_host": "localhost", "node1_port": 8123, "node1_container": "ca-soak-ch1-1",
    "node2_host": "localhost", "node2_port": 8124, "node2_container": "ca-soak-ch2-1",
    "gc_interval_s": 30,
}


class Node:
    def __init__(self, host: str, port: int, container: str | None = None, timeout: float = 60.0):
        self.host = host
        self.port = port
        self.container = container
        self.timeout = timeout

    @property
    def url(self) -> str:
        return f"http://{self.host}:{self.port}/"

    def query(self, sql: str) -> str:
        """POST `sql` and return the raw response body (TabSeparated text), trailing newline stripped."""
        data = sql.encode("utf-8")
        req = urllib.request.Request(self.url, data=data, method="POST")
        try:
            with urllib.request.urlopen(req, timeout=self.timeout) as resp:
                return resp.read().decode("utf-8").rstrip("\n")
        except urllib.error.HTTPError as e:
            body = ""
            try:
                body = e.read().decode("utf-8", "replace")
            except Exception:
                pass
            raise QueryError(self, e.code, body, sql) from e

    def command(self, sql: str) -> None:
        """Execute a statement expected to return no rows (DDL/DML)."""
        self.query(sql)

    def scalar(self, sql: str) -> str:
        """Execute a query expected to return a single value; return it as a string."""
        return self.query(sql).strip()

    def __repr__(self) -> str:
        return f"Node({self.host}:{self.port})"


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
