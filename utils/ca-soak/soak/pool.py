"""CA pool physical-size probe (S3 LIST over the pool prefix) for the Phase-3 metrics curve.

`metrics.snapshot_cluster` fills `pool_objects`/`pool_bytes` from system tables as None — those are
the PHYSICAL footprint of the shared content-addressed pool on the object store, which the per-node
`system.parts` view cannot see (it reports the LOGICAL/referenced bytes). This module LISTs the pool
prefix on RustFS to get the true physical object count + bytes, so the metrics plot can show
REFERENCED (logical, from system.parts + fsck) vs PHYSICAL (this LIST) divergence — the core CA
dedup/GC curve.

It is BEST-EFFORT: per the task spec the curve is still meaningful from system.parts + fsck alone, so
any probe failure returns (None, None) and the tick proceeds. We never block the soak on the probe.

Mechanism: a short-lived `minio/mc` container on the compose network runs `mc ls --recursive` against
the RustFS endpoint. `mc` is dependency-free for the harness (the same image the compose
`createbucket` service uses) and works even though that one-shot service has already exited.
"""

import json
import os
import subprocess
import uuid

# Endpoint/credentials mirror configs/storage_conf.xml (the `ca` disk) and configs/rustfs.env. The
# pool prefix is the path component AFTER the bucket in the disk endpoint
# (http://rustfs1:11121/test/soak_pool/ -> bucket "test", prefix "soak_pool/").
_RUSTFS_ENDPOINT = "http://rustfs1:11121"
_BUCKET = "test"
_PREFIX = "soak_pool"
_ACCESS_KEY = "clickhouse"
_SECRET_KEY = "clickhouse"
_MC_IMAGE = "minio/mc:latest"

# Compose project default network. Overridable for non-default `docker compose -p` projects.
_NETWORK = os.environ.get("CA_SOAK_NETWORK", "ca-soak_default")


def _mc_ls_json(timeout_s: float) -> str:
    """Run `mc ls --recursive --json` over the pool prefix in a throwaway container on the compose
    network. Returns the raw stdout (one JSON object per line). Raises on any docker/mc failure.

    The container is NAMED and force-removed on timeout: `subprocess.run(timeout=...)` kills only the
    `docker run` CLIENT, leaving the LIST container running (and the metrics thread blocked for far
    longer than `timeout_s` on a large pool). On a `TimeoutExpired` we therefore `docker rm -f` the
    named container so the next tick starts clean and the thread cannot wedge."""
    name = f"ca-soak-poolls-{uuid.uuid4().hex[:12]}"
    alias_and_ls = (
        f"mc alias set p {_RUSTFS_ENDPOINT} {_ACCESS_KEY} {_SECRET_KEY} >/dev/null 2>&1 && "
        f"mc ls --recursive --json p/{_BUCKET}/{_PREFIX}/"
    )
    cmd = [
        "docker", "run", "--rm", "--name", name, "--network", _NETWORK,
        "--entrypoint", "/bin/sh", _MC_IMAGE, "-c", alias_and_ls,
    ]
    try:
        p = subprocess.run(cmd, capture_output=True, text=True, timeout=timeout_s)
    except subprocess.TimeoutExpired:
        subprocess.run(["docker", "rm", "-f", name], capture_output=True)
        raise
    if p.returncode != 0:
        raise RuntimeError(f"mc ls failed rc={p.returncode}: {p.stderr.strip()[:300]}")
    return p.stdout


def parse_mc_ls(stdout: str) -> tuple:
    """Pure parser: sum object count + bytes from `mc ls --recursive --json` line-delimited output.

    Each line is a JSON object; a file entry has `type=="file"` (or no type but a numeric `size`).
    Directory/prefix entries (`type=="folder"`) are skipped. Unknown/blank lines are ignored so a
    future mc field cannot break the parser. Returns (objects, bytes)."""
    objects = 0
    total = 0
    for raw in stdout.splitlines():
        line = raw.strip()
        if not line:
            continue
        try:
            obj = json.loads(line)
        except ValueError:
            continue
        if obj.get("type") == "folder":
            continue
        size = obj.get("size")
        if size is None:
            continue
        objects += 1
        total += int(size)
    return objects, total


def pool_size(timeout_s: float = 60.0) -> tuple:
    """Return (pool_objects, pool_bytes) for the physical CA pool, or (None, None) on ANY failure.

    Best-effort by contract (task spec §2): the metrics curve degrades gracefully to referenced-only
    when the physical probe is unavailable, so we never raise into the soak — a probe failure logs and
    yields None."""
    try:
        out = _mc_ls_json(timeout_s)
        return parse_mc_ls(out)
    except Exception:
        return (None, None)
