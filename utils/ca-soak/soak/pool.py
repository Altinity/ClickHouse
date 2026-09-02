"""CA pool physical-size probe for the Phase-3 metrics curve.

`metrics.snapshot_cluster` fills `pool_objects`/`pool_bytes` from system tables as None — those are
the PHYSICAL footprint of the shared content-addressed pool on the object store, which the per-node
`system.parts` view cannot see (it reports the LOGICAL/referenced bytes). This module probes the
RustFS container directly to get the true physical byte count, so the metrics plot can show
REFERENCED (logical, from system.parts + fsck) vs PHYSICAL (this probe) divergence — the core CA
dedup/GC curve.

It is BEST-EFFORT: per the task spec the curve is still meaningful from system.parts + fsck alone, so
any probe failure returns (None, None) and the tick proceeds. We never block the soak on the probe.

Mechanism: `docker exec` into the running RustFS container and run `du -sb` (bytes, filesystem-level)
over the pool data directory. This is O(filesystem inodes), not O(pool objects), and does NOT issue
any HTTP object-LIST requests — so it scales to a large pool without timing out and without wedging
the metrics thread. Object count is returned as None (the byte total is what the throttle needs).

Previous mechanism (removed, B204): a throwaway `minio/mc` container running `mc ls --recursive`
issued one HTTP-LIST request per ~1000 objects, making the probe O(pool) and liable to time out on
a large pool. On timeout `subprocess.run` killed only the docker-run CLIENT, leaving the LIST
container running and the metrics thread wedged for far longer than `timeout_s`. This turned into a
fail-open throttle: `compute_throttle(None, ...)` kept the CURRENT (zero) sleep, allowing unchecked
growth that filled the host disk to 100% in ~1.7h (B204).
"""

import os
import subprocess

# Pool data directory inside the RustFS container. Mirror of configs/storage_conf.xml:
# the `ca` disk endpoint is http://rustfs1:11121/test/soak_pool/ -> bucket "test",
# prefix "soak_pool/". RustFS stores bucket data under /data/<bucket>/<prefix>.
_POOL_DIR = "/data/test/soak_pool"


def rustfs_container() -> str:
    """Overridable through `CA_SOAK_RUSTFS_CONTAINER`, like every other container the harness names."""
    return os.environ.get("CA_SOAK_RUSTFS_CONTAINER", "ca-soak-rustfs1-1")


def pool_dir() -> str:
    """Overridable through `CA_SOAK_POOL_DIR`, so a stand that relocates the pool is measured, not
    silently reported as unmeasurable."""
    return os.environ.get("CA_SOAK_POOL_DIR", _POOL_DIR)


def pool_size(timeout_s: float = 30.0) -> tuple:
    """Return (pool_objects, pool_bytes) for the physical CA pool, or (None, None) on ANY failure.

    Best-effort by contract (task spec §2): the metrics curve degrades gracefully to referenced-only
    when the physical probe is unavailable, so we never raise into the soak — a probe failure logs and
    yields None.

    pool_objects is always None (not measured; the byte total is sufficient for throttle decisions).
    pool_bytes is the raw filesystem byte count from `du -sb` — fast and O(filesystem), not O(objects).
    The `timeout` wrapper applied to the docker exec ensures the metrics thread cannot wedge on a
    slow/unavailable container."""
    try:
        cmd = [
            "docker", "exec", rustfs_container(),
            "sh", "-c", f"timeout {int(timeout_s)} du -sb {pool_dir()}",
        ]
        p = subprocess.run(cmd, capture_output=True, text=True, timeout=timeout_s + 5)
        # `du` exits nonzero (with "cannot access ... No such file or directory" on stderr) whenever
        # it lists a file that a concurrent writer/GC then removes before `du` can stat it -- an
        # expected, benign TOCTOU race against the live, actively-mutating pool, NOT a probe failure.
        # GNU `du -sb` still prints a valid (near-exact; the missing file's bytes are simply excluded)
        # recursive total to stdout in this case, so we do NOT gate on returncode here -- only an
        # unparseable/empty stdout (a genuinely failed probe, e.g. the container being unreachable)
        # falls back to (None, None).
        first_line = p.stdout.strip().splitlines()[0] if p.stdout.strip() else ""
        parts = first_line.split()
        if not parts:
            return (None, None)
        pool_bytes = int(parts[0])
        return (None, pool_bytes)
    except Exception:
        return (None, None)
