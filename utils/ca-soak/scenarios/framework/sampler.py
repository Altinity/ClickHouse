"""Background metrics sampler — periodic memory / pool-size / container samples to metrics.sqlite.

Runs on its own thread during a scenario's workload so the report has a time series for memory and
pool growth (README §"Common observations": per-minute samples of MemoryResident/MemoryTracking,
cgroup memory, scratch-dir bytes, and pool bytes). Best-effort: a failed probe records None for that
field; the sampler never raises into the workload.
"""

import sqlite3
import threading
import time

from . import observe

_COLS = ["ts", "phase", "node", "mem_resident", "mem_tracking", "cont_mem_current",
         "cont_mem_peak_incl_cache", "scratch_bytes", "pool_bytes", "pool_objects"]


def open_db(path) -> sqlite3.Connection:
    conn = sqlite3.connect(str(path), check_same_thread=False)
    cols_ddl = ", ".join("node TEXT" if c == "node" else ("phase TEXT" if c == "phase" else f"{c} INTEGER")
                         for c in _COLS)
    conn.execute(f"CREATE TABLE IF NOT EXISTS samples ({cols_ddl})")
    conn.commit()
    return conn


class MetricsSampler:
    """Periodically samples per-node server memory + per-container cgroup memory/scratch into
    metrics.sqlite. The pool-bytes probe is OPTIONAL per tick (it is O(filesystem) and slower), gated
    by `pool_every` ticks so a large pool is not du'd every interval."""

    def __init__(self, conn, cluster, *, interval_s: float = 30.0, pool_every: int = 4,
                 phase_fn=lambda: "workload", log_fn=print):
        self.conn = conn
        self.cluster = cluster
        self.interval_s = interval_s
        self.pool_every = max(1, pool_every)
        self.phase_fn = phase_fn
        self.log_fn = log_fn
        self._stop = threading.Event()
        self._thread = None
        self._lock = threading.Lock()
        self._tick = 0
        self.peak_mem_resident = {}

    def _record(self, row: dict):
        vals = [row.get(c) for c in _COLS]
        ph = ", ".join("?" for _ in _COLS)
        with self._lock:
            self.conn.execute(
                f"INSERT INTO samples ({', '.join(_COLS)}) VALUES ({ph})", vals)
            self.conn.commit()

    def sample_once(self, phase: str | None = None):
        ts = int(time.time())
        phase = phase or self.phase_fn()
        self._tick += 1
        pool_bytes = pool_objects = None
        if self._tick % self.pool_every == 1:
            try:
                shape = observe.pool_shape(timeout_s=60)
                if shape.get("_ok"):
                    pool_bytes = shape["_total"]["bytes"]
                    pool_objects = shape["_total"]["objects"]
            except Exception:
                pass
        conts = {c["container"]: c for c in observe.container_samples()}
        for node in self.cluster.nodes():
            mem = observe.server_memory(node)
            c = conts.get(node.container, {})
            mr = mem.get("mem_resident")
            if mr is not None:
                self.peak_mem_resident[node.container] = max(
                    self.peak_mem_resident.get(node.container, 0), mr)
            self._record({
                "ts": ts, "phase": phase, "node": node.container,
                "mem_resident": mr, "mem_tracking": mem.get("mem_tracking"),
                "cont_mem_current": c.get("mem_current"),
                # cgroup memory.peak INCLUDES page cache (5-21x above tracked RSS in the campaign) —
                # keep it as cache-inclusive evidence only; verdicts use peak_mem_resident.
                "cont_mem_peak_incl_cache": c.get("mem_peak"),
                "scratch_bytes": c.get("scratch_bytes"),
                "pool_bytes": pool_bytes, "pool_objects": pool_objects,
            })

    def _loop(self):
        while not self._stop.is_set():
            try:
                self.sample_once()
            except Exception as e:
                self.log_fn(f"MetricsSampler tick error: {e}")
            self._stop.wait(self.interval_s)

    def start(self):
        self._thread = threading.Thread(target=self._loop, name="ca-scenario-sampler", daemon=True)
        self._thread.start()

    def stop(self):
        self._stop.set()
        if self._thread is not None:
            self._thread.join(timeout=self.interval_s + 10)

    def peak_scratch_bytes(self):
        with self._lock:
            cur = self.conn.execute("SELECT node, max(scratch_bytes) FROM samples GROUP BY node")
            return {r[0]: r[1] for r in cur.fetchall()}

    def __enter__(self):
        self.start()
        return self

    def __exit__(self, *exc):
        self.stop()
        return False
