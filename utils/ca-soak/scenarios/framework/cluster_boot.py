"""Cluster bring-up / reset for the scenario suite.

The compose endpoint is a fixed pool prefix (`test/soak_pool/`), so a "fresh pool per run" is realized
by a hard reset: `docker compose down -v` (the RustFS container is ephemeral — no named volume — so
tearing it down wipes the pool) followed by `up -d`. Server logs are host-bind-mounted under
`logs/ch1` / `logs/ch2`, so they survive the reset and are archived per run.

Two compose variants are supported: the default (`gc_shards=1`) and `gc_shards2`. Both target the
same docker-compose project (directory name `ca-soak`), so container names are stable across variants.
"""

import subprocess
import time
from pathlib import Path

from soak.cluster import Cluster

_THIS = Path(__file__).resolve()
CA_SOAK_DIR = _THIS.parents[2]

_VARIANT_FILE = {
    None: None,
    "default": None,
    "gc_shards2": "docker-compose-gc_shards2.yml",
    # S24: 1 MiB dedup cache (vs 64 MiB default) to exercise eviction + remote-HEAD fallback.
    "smalldedupcache": "docker-compose-small_dedup_cache.yml",
    # S12: 10-replica shared-pool compose (harness wiring incomplete — see BACKLOG NEEDS-INFRA-S12).
    "tenreplicas": "docker-compose-10replicas.yml",
}


def compose_cmd(variant, *args):
    base = ["docker", "compose"]
    f = _VARIANT_FILE.get(variant)
    if f:
        base += ["-f", f]
    return base + list(args)


def _run(argv, timeout=600, log_fn=print):
    log_fn(f"$ {' '.join(argv)}")
    p = subprocess.run(argv, cwd=str(CA_SOAK_DIR), capture_output=True, text=True, timeout=timeout)
    if p.returncode != 0:
        log_fn(f"  rc={p.returncode} stderr={p.stderr.strip()[:400]}")
    return p.returncode


def _prep_log_dirs():
    for d in ("logs/ch1", "logs/ch2"):
        p = CA_SOAK_DIR / d
        p.mkdir(parents=True, exist_ok=True)
        try:
            p.chmod(0o777)
        except OSError:
            pass


def wait_healthy(cluster=None, *, timeout_s=240, log_fn=print) -> bool:
    """Poll both replicas' /ping until both answer or timeout. Returns True iff both healthy."""
    cluster = cluster or Cluster()
    deadline = time.time() + timeout_s
    while time.time() < deadline:
        if all(n.ping(timeout=3) for n in cluster.nodes()):
            return True
        time.sleep(3)
    return all(n.ping(timeout=3) for n in cluster.nodes())


def archive_server_logs(tag, log_fn=print):
    """Tar the per-node server logs into logs/ before a reset wipes/overwrites them, so each run's
    server-side logs are preserved (regression-watch false-alarm guard, per the soak convention)."""
    logs = CA_SOAK_DIR / "logs"
    for d in ("ch1", "ch2"):
        src = logs / d
        if src.exists() and any(src.iterdir()):
            dst = logs / f"_archive_{tag}_{d}.tgz"
            try:
                subprocess.run(["tar", "czf", str(dst), "-C", str(logs), d],
                               capture_output=True, timeout=120)
            except Exception as e:
                log_fn(f"archive_server_logs {d}: {e}")


def reset_cluster(variant=None, *, archive_tag=None, log_fn=print, timeout_s=300) -> bool:
    """Hard reset to a fresh pool: down -v (current + variant), then up -d the chosen variant, then
    wait for both replicas healthy. Returns True iff healthy after bring-up."""
    if archive_tag:
        archive_server_logs(archive_tag, log_fn=log_fn)
    # Tear down regardless of which variant is currently up (same project/containers).
    _run(compose_cmd(None, "down", "-v", "--remove-orphans"), timeout=timeout_s, log_fn=log_fn)
    _prep_log_dirs()
    _run(compose_cmd(variant, "up", "-d"), timeout=timeout_s, log_fn=log_fn)
    ok = wait_healthy(timeout_s=timeout_s, log_fn=log_fn)
    if not ok:
        log_fn("reset_cluster: cluster did NOT become healthy within timeout")
    else:
        log_fn(f"reset_cluster: fresh pool up (variant={variant or 'default'})")
    return ok


def ensure_up(variant=None, *, log_fn=print, timeout_s=240) -> bool:
    """Ensure the cluster is up (no pool reset). If not healthy, bring it up. Returns health."""
    if wait_healthy(timeout_s=5, log_fn=log_fn):
        return True
    _prep_log_dirs()
    _run(compose_cmd(variant, "up", "-d"), timeout=timeout_s, log_fn=log_fn)
    return wait_healthy(timeout_s=timeout_s, log_fn=log_fn)
