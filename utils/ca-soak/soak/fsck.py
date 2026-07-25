"""CA soak harness — fsck / ca-gc-dryrun invocation and output parsing.

`parse_fsck_summary` and `parse_dryrun` are pure functions (no I/O); they are unit-tested
independently of the running cluster.

`run_fsck` / `run_dryrun` invoke `clickhouse disks` (the applet form) via `docker exec`
against the read-only `ca_ro` disk so that the mutating capability probe is skipped.
Both functions return a dict that is safe to assert on in integration tests.
"""
import subprocess


class FsckTimeout(RuntimeError):
    """The fsck/dryrun subprocess exceeded its timeout (an O(pool) scan crawling under load — B146/B154)."""


# ---------------------------------------------------------------------------
# Pure parsers (unit-tested)
# ---------------------------------------------------------------------------

def parse_fsck_summary(line: str) -> dict:
    """Parse the single summary line emitted by `clickhouse-disks ca-fsck`.

    The line has the form:
        reachable=N dangling=N unreachable=N physical_bytes=N
        referenced_logical_bytes=N distinct_blobs=N total_blob_refs=N dedup_ratio=F

    Integer fields are returned as `int`; fields containing a decimal point as `float`.
    Unknown tokens are silently skipped so that future fields do not break the parser.

    On a `--partial` scan, the line ends with `partial=1 reason='<message>'` where `<message>`
    (`FsckReport::partial_reason`, e.g. the `TIMEOUT_EXCEEDED` text) can contain spaces, periods,
    and its own single quotes — a plain whitespace split would mis-tokenize it. `reason='...'` is
    trimmed off (it is always the trailing field) and parsed separately as a string.
    """
    marker = " reason='"
    reason = None
    idx = line.find(marker)
    if idx != -1 and line.rstrip().endswith("'"):
        reason = line.rstrip()[idx + len(marker):-1]
        line = line[:idx]

    out: dict = {}
    for tok in line.strip().split():
        if "=" in tok:
            kk, vv = tok.split("=", 1)
            out[kk] = float(vv) if "." in vv else int(vv)
    if reason is not None:
        out["reason"] = reason
    return out


def parse_dryrun(text: str) -> dict:
    """Parse the full stdout of `clickhouse-disks ca-gc-dryrun`.

    Expected format::

        preview_deletes=N
        <reason>\\t<key>\\t<size>
        ...

    Returns ``{"count": N, "entries": [{"reason": ..., "key": ..., "size": ...}, ...]}``.
    The ``count`` field comes from the ``preview_deletes=`` header line; the entries are
    built from the TSV rows (any row with at least 3 tab-separated fields).
    """
    count = 0
    entries: list[dict] = []
    for raw in text.splitlines():
        line = raw.strip()
        if not line:
            continue
        if line.startswith("preview_deletes="):
            count = int(line.split("=", 1)[1])
        else:
            parts = line.split("\t")
            if len(parts) >= 3:
                entries.append({"reason": parts[0], "key": parts[1], "size": int(parts[2])})
    return {"count": count, "entries": entries}


# ---------------------------------------------------------------------------
# Docker-exec wrappers (integration; unit tests mock these or skip them)
# ---------------------------------------------------------------------------

# The binary in the container is mounted as /usr/bin/clickhouse and provides the
# `clickhouse disks` applet (NOT `clickhouse-disks`).  The disk definitions live in
# /etc/clickhouse-server/config.d/storage_conf.xml which is auto-loaded via the
# standard config directory, so --config-file is not normally required — but we pass
# it explicitly to be defensive about non-standard container entrypoints.
_CLICKHOUSE_DISKS = [
    "clickhouse", "disks",
    # Standalone fsck-only config (the ca_ro read-only disk lives here, NOT in the server's
    # config.d — see configs/fsck_only_ca.xml). A ca_ro disk over the same pool in the SERVER config
    # breaks table load on restart with UNKNOWN_DISK (ROADMAP "Read-only fsck shadow disk breaks table
    # load on restart"; confirmed on the RustFS stand 2026-07-06). Every compose that runs fsck must
    # mount its fsck_only_*.xml at this path.
    "--config-file", "/etc/clickhouse-server/fsck-only.xml",
]


def run_fsck(container: str, disk: str = "ca_ro", detail: bool = True,
             timeout_s: float = 600.0) -> dict:
    """Run `clickhouse disks --disk <disk> --query "ca-fsck [--detail]"` in the container.

    Uses the read-only disk (``ca_ro`` by default) so the mutating capability probe is
    skipped.  ``ca-fsck`` exits nonzero when ``dangling > 0`` (invariant INV-NO-LOSS).

    Returns a dict with:
    - all fields from the summary line (``reachable``, ``dangling``, ``unreachable``, …)
    - ``exit_code``  — raw process return code
    - ``stdout`` / ``stderr``  — raw strings for diagnostic logging
    - ``detail`` (when ``detail=True``) — list of per-object dicts
      ``{"class": …, "key": …, "size": …}``

    Raises ``FsckTimeout`` if the subprocess does not complete within ``timeout_s`` seconds
    (an O(pool) scan can take 40+ minutes under load — B146/B154).  ``subprocess.run``
    kills the child process automatically on ``TimeoutExpired`` in Python 3.x.
    """
    query = "ca-fsck --detail" if detail else "ca-fsck"
    cmd = [
        "docker", "exec", container,
        *_CLICKHOUSE_DISKS,
        "--disk", disk,
        "--query", query,
    ]
    try:
        p = subprocess.run(cmd, capture_output=True, text=True, timeout=timeout_s)
    except subprocess.TimeoutExpired:
        raise FsckTimeout(f"ca-fsck (detail={detail}) exceeded {timeout_s}s on {container}")

    # The summary line starts with "reachable="; find the first such line.
    summary_line = next(
        (ln for ln in p.stdout.splitlines() if ln.startswith("reachable=")), ""
    )
    res = parse_fsck_summary(summary_line) if summary_line else {}
    res["exit_code"] = p.returncode
    res["stdout"] = p.stdout
    res["stderr"] = p.stderr

    if detail:
        # Detail rows: TSV with the object class in column 0. pending-gc / awaiting-gc are the
        # ack-floor deletion pipeline mid-flight (expected); unaccounted = outside the GC view;
        # stale-edge = every source edge on the blob names a manifest that no longer exists, so its
        # in-degree can never reach zero and the incremental GC can never reclaim it (a hard finding,
        # NOT pipeline backlog).
        #
        # This whitelist is a KNOWN HAZARD, not a nicety: a class the product emits but this list omits
        # is dropped SILENTLY, so the harness under-reports exactly the objects a new fsck class was
        # added to surface. That is how `awaiting-gc` objects sat mislabelled as B140 M-F debris for
        # months. Whenever `FsckClass` gains a member, extend this tuple in the same change — and note
        # the parser below now warns rather than discarding an unknown class outright.
        known_classes = (
            "reachable", "dangling", "unreachable", "pending-gc", "awaiting-gc", "unaccounted",
            "stale-edge",
        )
        detail_rows: list[dict] = []
        unknown_classes: set[str] = set()
        for ln in p.stdout.splitlines():
            parts = ln.split("\t")
            if len(parts) >= 3 and parts[0] not in known_classes and parts[0].islower() \
                    and " " not in parts[0] and parts[2].isdigit():
                unknown_classes.add(parts[0])
            if len(parts) >= 3 and parts[0] in known_classes:
                detail_rows.append(
                    {"class": parts[0], "key": parts[1], "size": int(parts[2])}
                )
        res["detail"] = detail_rows
        # Never silently drop a class the product knows about and this parser does not.
        if unknown_classes:
            res["unknown_detail_classes"] = sorted(unknown_classes)

    return res


def run_dryrun(container: str, disk: str = "ca_ro", timeout_s: float = 600.0) -> dict:
    """Run `clickhouse disks --disk <disk> --query ca-gc-dryrun` in the container.

    Returns the parsed output of `parse_dryrun`.

    Raises ``FsckTimeout`` if the subprocess does not complete within ``timeout_s`` seconds
    (an O(pool) scan can take 40+ minutes under load — B146/B154).
    """
    cmd = [
        "docker", "exec", container,
        *_CLICKHOUSE_DISKS,
        "--disk", disk,
        "--query", "ca-gc-dryrun",
    ]
    try:
        p = subprocess.run(cmd, capture_output=True, text=True, timeout=timeout_s)
    except subprocess.TimeoutExpired:
        raise FsckTimeout(f"ca-gc-dryrun exceeded {timeout_s}s on {container}")
    result = parse_dryrun(p.stdout)
    result["exit_code"] = p.returncode
    result["stdout"] = p.stdout
    result["stderr"] = p.stderr
    return result
