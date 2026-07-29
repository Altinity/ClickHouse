"""Cluster bring-up / reset for the scenario suite.

The compose endpoint is a fixed pool prefix (`test/soak_pool/`), so a "fresh pool per run" is realized
by a hard reset: `docker compose down -v` (the RustFS container is ephemeral — no named volume — so
tearing it down wipes the pool) followed by `up -d`. Server logs are host-bind-mounted under
`logs/ch1` / `logs/ch2`, so they survive the reset and are archived per run.

Two compose variants are supported: the default (`gc_shards=1`) and `gc_shards2`. Both target the
same docker-compose project (directory name `ca-soak`), so container names are stable across variants.
"""

import shutil
import subprocess
import time
import xml.etree.ElementTree as ET
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
    # S12: 10-replica shared-pool compose (ch1..ch10 over one CA pool).
    "tenreplicas": "docker-compose-10replicas.yml",
    # S15: gc_shards=8 variant (8-way sharded fold) for the target-shard comparison.
    "gc_shards8": "docker-compose-gc_shards8.yml",
    # S22: fault-injecting S3 proxy (503/429/slow/reset) between ClickHouse and RustFS.
    "s3faultproxy": "docker-compose-s3faultproxy.yml",
    # S27: same proxy, LIST-anomaly mode (duplicate keys / dropped continuation token).
    "s3listproxy": "docker-compose-s3faultproxy.yml",
    # S38: a published RustFS port (18121) for direct object-store injection (the late-PUT test).
    # It also used to raise materialization_grace_ms (T_mat); that setting is retired (Stage A task
    # 12, `ff9f36a056f`) and the card now asserts the epoch-seal fence instead of a wait.
    "s38": "docker-compose-s38.yml",
    # S36/S37: local+CA (and local+local+CA) multi-disk storage policies (ca_local / ca_local3)
    # alongside the same shared CA disk, for MOVE PART/PARTITION and policy-driven tiering tests.
    "multidisk": "docker-compose-multidisk.yml",
    # Soak-matrix config sweeps (opt §2 dedup_cache_bytes, §3 part_folder_validate): render_tuned_config
    # writes configs/storage_conf_tuned_ch{1,2}.xml on demand; this compose mounts those instead of the
    # fixed per-variant XML the other entries above use.
    "tuned": "docker-compose-tuned.yml",
    # S41: single-node write-path baseline in an ISOLATED docker-compose project (`name: ca-s41`) so
    # it never disturbs a concurrently-running `ca-soak` stack. Point the framework at it with env
    # CA_SOAK_NODE_COUNT=1, CA_SOAK_NODE1_PORT=18123, CA_SOAK_NODE1_CONTAINER=ca-s41-ch1-1,
    # CA_SOAK_RUSTFS_CONTAINER=ca-s41-rustfs1-1, CA_SOAK_CH_CONTAINERS=ca-s41-ch1-1,
    # CA_SOAK_FSCK_CONTAINER=ca-s41-ch1-1. On a host where `ca-soak` is running, this variant MUST be
    # run with `--no-reset` against a pre-brought-up stack: reset_cluster's mandatory pre-`down` is
    # scoped to the `ca-soak` project and would tear that stack down.
    "s41": "docker-compose-s41.yml",
}

# Replica count per compose variant — drives the N-node Cluster + health wait + log-dir prep.
_VARIANT_NODES = {
    None: 2, "default": 2, "gc_shards2": 2, "smalldedupcache": 2,
    "tenreplicas": 10, "gc_shards8": 2, "s3faultproxy": 2, "s3listproxy": 2, "s38": 2,
    "tuned": 2, "multidisk": 2, "s41": 1,
}


def render_tuned_config(overrides: dict) -> None:
    """Render configs/storage_conf_tuned_ch{1,2}.xml from the base storage_conf_ch{1,2}.xml, injecting
    one child element per override inside the <ca> disk block (replacing a same-named child if present).

    Each call re-parses the base XML from scratch, so repeated calls are idempotent by construction —
    there is no way to accumulate stale children across runs. Soak-matrix sweeps (opt §2 dedup_cache_bytes,
    §3 part_folder_validate) feed one variable per run through here instead of hand-authoring a
    storage_conf_<variant>.xml + docker-compose-<variant>.yml pair per value (the S24 smalldedupcache
    variant is the pattern this replaces for ad hoc single-knob sweeps).

    Note: xml.etree.ElementTree does not round-trip comments — the rendered tuned file loses the
    explanatory comments present in the base storage_conf_ch{1,2}.xml. That's acceptable here because
    the tuned files are generated run artifacts (gitignored, not committed), not the source of truth.
    """
    for node in ("ch1", "ch2"):
        base = CA_SOAK_DIR / "configs" / f"storage_conf_{node}.xml"
        tree = ET.parse(base)
        ca = tree.getroot().find("./storage_configuration/disks/ca")
        if ca is None:
            raise RuntimeError(f"no <ca> disk block in {base}")
        for key, value in overrides.items():
            existing = ca.find(key)
            if existing is not None:
                ca.remove(existing)
            child = ET.SubElement(ca, key)
            child.text = str(value)
        tree.write(CA_SOAK_DIR / "configs" / f"storage_conf_tuned_{node}.xml",
                   encoding="unicode", xml_declaration=False)


def node_count_for(variant) -> int:
    return _VARIANT_NODES.get(variant, 2)


def compose_cmd(variant, *args):
    base = ["docker", "compose"]
    f = _VARIANT_FILE.get(variant)
    if f:
        base += ["-f", f]
    return base + list(args)


def compose_run(variant, *args, timeout=600, log_fn=print) -> int:
    """Build AND run a `docker compose` command for `variant`, returning its exit code.

    `compose_cmd` only builds the argv; a caller that forgets to run it gets a silently discarded
    list, so cards that drive the cluster directly (S43's stop/start around a pool wipe) use this."""
    return _run(compose_cmd(variant, *args), timeout=timeout, log_fn=log_fn)


def _run(argv, timeout=600, log_fn=print):
    log_fn(f"$ {' '.join(argv)}")
    p = subprocess.run(argv, cwd=str(CA_SOAK_DIR), capture_output=True, text=True, timeout=timeout)
    if p.returncode != 0:
        log_fn(f"  rc={p.returncode} stderr={p.stderr.strip()[:400]}")
    return p.returncode


def _prep_log_dirs(node_count=2):
    """Ensure fresh, world-writable per-node log dirs before a `docker compose up`.

    A stale host-side log dir can carry files from an earlier (non-containerized, or differently-
    owned) run — root-owned or mode 640 — that the container's uid-101 `clickhouse-server` cannot
    open for append. `p.chmod(0o777)` only fixed the DIRECTORY's own mode, not files already inside
    it, so a stale `clickhouse-server.log` still blocked first boot with a
    `Poco::FileAccessDeniedException` (S01 attempt 1, 2026-07-13 task-4 campaign). Archive-then-clear
    (mirrors `run_24h.sh`'s archive-before-restart pattern): if a dir already has content, move the
    WHOLE dir aside into `logs/prev_<ts>/ch<i>` first — never delete data outright — then recreate a
    fresh, empty, world-writable directory.
    """
    logs_dir = CA_SOAK_DIR / "logs"
    ts = None
    for i in range(1, node_count + 1):
        p = logs_dir / f"ch{i}"
        if p.exists() and any(p.iterdir()):
            if ts is None:
                ts = time.strftime("%Y%m%dT%H%M%S")
            prev_dir = logs_dir / f"prev_{ts}"
            prev_dir.mkdir(parents=True, exist_ok=True)
            try:
                shutil.move(str(p), str(prev_dir / f"ch{i}"))
            except OSError:
                pass  # best-effort: fall through and (re)create/chmod whatever remains
        p.mkdir(parents=True, exist_ok=True)
        try:
            p.chmod(0o777)
        except OSError:
            pass


def wait_healthy(cluster=None, *, variant=None, timeout_s=240, log_fn=print) -> bool:
    """Poll every replica's /ping until all answer or timeout. Returns True iff all healthy.
    Builds an N-node Cluster sized for `variant` (ch1..chN) when no cluster is passed."""
    cluster = cluster or Cluster(node_count=node_count_for(variant))
    n_total = len(cluster.nodes())
    deadline = time.time() + timeout_s
    while time.time() < deadline:
        up = sum(1 for n in cluster.nodes() if n.ping(timeout=3))
        if up == n_total:
            return True
        time.sleep(3)
    up = sum(1 for n in cluster.nodes() if n.ping(timeout=3))
    if up != n_total:
        log_fn(f"wait_healthy: only {up}/{n_total} replicas healthy at timeout")
    return up == n_total


def archive_server_logs(tag, node_count=2, log_fn=print):
    """Tar the per-node server logs into logs/ before a reset wipes/overwrites them, so each run's
    server-side logs are preserved (regression-watch false-alarm guard, per the soak convention)."""
    logs = CA_SOAK_DIR / "logs"
    for i in range(1, node_count + 1):
        d = f"ch{i}"
        src = logs / d
        if src.exists() and any(src.iterdir()):
            dst = logs / f"_archive_{tag}_{d}.tgz"
            try:
                subprocess.run(["tar", "czf", str(dst), "-C", str(logs), d],
                               capture_output=True, timeout=120)
            except Exception as e:
                log_fn(f"archive_server_logs {d}: {e}")


def check_host_headroom(log_fn=print, *, min_disk_gb=80, min_free_ram_gb=8) -> list:
    """Report host resources a scenario is about to consume, and RETURN the concerns rather than raising.

    Written after S42 at `--scale full` had to be killed 12 minutes in: it consumed ~21 GB/minute and was
    on course to need ~380 GB against 218 GB free. Nothing warned, because the scenario runner has no
    pool-size cap of its own (`--max-pool-gb` belongs to the soak driver, not here).

    Memory is checked for a DIFFERENT reason than disk, and the difference matters. Low disk breaks the
    machine. Low free RAM breaks the EXPERIMENT: an allocation-fault scenario (S42) cannot distinguish a
    real host OOM kill from its own injected fault, so a run started without memory headroom produces a
    verdict that means nothing whichever colour it comes out.

    Returns a list of human-readable concerns (empty when clear). Deliberately non-fatal: the operator may
    have reason to proceed, and a hard stop in shared tooling is how people learn to bypass it.
    """
    import shutil
    concerns = []
    free_gb = shutil.disk_usage("/").free / (1 << 30)
    log_fn(f"host headroom: disk {free_gb:.0f} GB free")
    if free_gb < min_disk_gb:
        concerns.append(f"disk {free_gb:.0f} GB free is under the {min_disk_gb} GB floor")

    try:
        with open("/proc/meminfo") as fh:
            meminfo = {k.strip(): v for k, v in (l.split(":", 1) for l in fh)}
        avail_gb = int(meminfo["MemAvailable"].split()[0]) / (1 << 20)
        log_fn(f"host headroom: RAM {avail_gb:.0f} GB available")
        if avail_gb < min_free_ram_gb:
            concerns.append(
                f"RAM {avail_gb:.0f} GB available is under the {min_free_ram_gb} GB floor — for an "
                f"allocation-fault scenario this invalidates the run rather than merely risking it")
    except (OSError, KeyError, ValueError) as e:
        concerns.append(f"could not read /proc/meminfo ({e}) — memory headroom UNKNOWN, not assumed fine")

    for c in concerns:
        log_fn(f"host headroom WARNING: {c}")
    return concerns


def reset_cluster(variant=None, *, archive_tag=None, log_fn=print, timeout_s=300, overrides=None) -> bool:
    """Hard reset to a fresh pool: down -v (current + variant), then up -d the chosen variant, then
    wait for ALL replicas healthy. Returns True iff healthy after bring-up. The 10-replica variant
    serializes its startup (ch2 waits ch1, ..., ch10 waits ch9) so bring-up takes longer — the
    caller passes a larger timeout for it.

    `overrides`: for variant="tuned" only — a dict of <ca> disk config overrides rendered via
    `render_tuned_config` before `up` (see that function for the soak-matrix use case)."""
    n = node_count_for(variant)
    # The 10-replica compose serializes startup (ch2 waits ch1, ..., ch10 waits ch9) to avoid the CA
    # capability-probe race on the shared pool, so bring-up scales with node count — widen the bound.
    boot_timeout = max(timeout_s, 90 + 45 * n)
    if archive_tag:
        archive_server_logs(archive_tag, node_count=n, log_fn=log_fn)
    # Tear down regardless of which variant is currently up (same project/containers). Pass the
    # tenreplicas file too so ch3..ch10 (defined only there) are torn down when switching away.
    _run(compose_cmd("tenreplicas", "down", "-v", "--remove-orphans"), timeout=boot_timeout, log_fn=log_fn)
    _prep_log_dirs(node_count=n)
    if variant == "tuned" and overrides:
        render_tuned_config(overrides)
    _run(compose_cmd(variant, "up", "-d"), timeout=boot_timeout, log_fn=log_fn)
    ok = wait_healthy(variant=variant, timeout_s=boot_timeout, log_fn=log_fn)
    if not ok:
        log_fn("reset_cluster: cluster did NOT become healthy within timeout")
    else:
        log_fn(f"reset_cluster: fresh pool up (variant={variant or 'default'}, {n} replicas)")
    return ok


def ensure_up(variant=None, *, log_fn=print, timeout_s=240) -> bool:
    """Ensure the cluster is up (no pool reset). If not healthy, bring it up. Returns health."""
    if wait_healthy(variant=variant, timeout_s=5, log_fn=log_fn):
        return True
    _prep_log_dirs(node_count=node_count_for(variant))
    _run(compose_cmd(variant, "up", "-d"), timeout=timeout_s, log_fn=log_fn)
    return wait_healthy(variant=variant, timeout_s=timeout_s, log_fn=log_fn)
