"""S45 (validated live 2026-08-03, written for T8/E4) — decommission a victim member with HIDDEN
`Removing` catalog entries.

T8's soak run (c) ("decommission") requires the victim member to carry catalog entries in `NsState::
Removing` at the moment it is decommissioned -- exactly the shape `gtest_cas_decommission_catalog_
duties.cpp` characterizes at the unit level (`makeRemoving` + the retirement-fence tests), but never
exercised end-to-end against a live cluster. No existing card decommissions a pool member at all: no
scenario invokes the `ca-drop-member` tool (`programs/disks/CommandCaDropMember.cpp`, backed by `Cas::
decommissionPoolMember`).

Mechanics (modelled on `soak/fsck.py`'s `run_fsck`/`run_dryrun`, the only existing wrapper around the
`clickhouse disks` applet in this harness):
  - drop several tables that live on the victim's namespaces WITHOUT waiting for their `Removing` rows
    to clear (kill the victim node immediately after the `DROP TABLE`s return, before its own GC/janitor
    would have retired them) -- this is what makes the entries HIDDEN: still `Removing`, not yet
    condemned, and the member that would have finished retiring them is gone;
  - kill the victim container (`docker kill`) so it is unambiguously DEAD (`ca-drop-member` refuses a
    live member);
  - from a SURVIVING node, run `docker exec <survivor> clickhouse disks --disk ca_ro --query
    "ca-drop-member <victim_srid>"` (the read-only-disk contract `CommandCaDropMember.cpp` requires);
  - assert the tool's own report accounts for the hidden `Removing` rows (`namespaces_removed` covers
    them, not just `Live` ones) and that forced GC afterward completes them (fsck clean, no leaked
    checkpoints).

**Gaps closed at validation time (2026-08-03), for the record:**
  1. The victim `server_root_id` is still hardcoded to `ca_soak_ch2` (the default compose's name for
     it); a different compose variant needs this constant updated.
  2. Fixed: the card now imports `_CLICKHOUSE_DISKS` from `soak.fsck` instead of duplicating (and
     mis-stating) its invocation shape -- the original hardcoded `--config-file
     /etc/clickhouse-server/config.xml` does not carry the `ca_ro` disk definition at all (it lives
     only in the standalone `fsck-only.xml`, kept out of the server's own config.d so table load on
     restart does not see it -- see `configs/fsck_only_ca.xml`); that invocation would have failed
     with `UNKNOWN_DISK` on every run.
  3. Confirmed against `CommandCaDropMember.cpp`: the tool reports only the aggregate
     `namespaces_removed` count, with no separate hidden-`Removing`-specific figure. The card's
     verdict (`namespaces_removed >= len(tables)`) is written to that aggregate, which is what the
     tool actually exposes.
  4. Fixed: a live run showed `ca-drop-member` refusing immediately after `docker kill` with "pool
     member is alive or contended -- mount lease held by ... (expires_at_ms=...)". `docker kill`
     does not shorten the lease the victim already renewed before dying, and
     `Cas::Pool::mountWritable`'s decommission path takes one unbounded-wait-free snapshot compare
     against the lease's `expires_at_ms` with no bounded-wait variant. The card now polls
     `ca-drop-member` for up to `mount_lease_ttl_ms` + one `mount_renew_period` + the `chaos.py`
     FREEZE_LONG fence margin (60s total, 5s interval) before treating a persistent
     "alive or contended" refusal as a real failure.
  5. Fixed: `sql.create_ca_table` only ran on `node` (node1); a `ReplicatedMergeTree` materializes
     per-replica, so the victim never had its own local table at all -- a live run then hit
     `UNKNOWN_TABLE` trying to `SYSTEM SYNC REPLICA` it on the victim. The card now creates each
     table on both replicas.
  6. Fixed: the `DROP TABLE ... SYNC` calls only ran on `node`. A `ReplicatedMergeTree` `DROP` is
     per-replica, not automatically cluster-wide, so the victim's own local replica stayed ATTACHED
     (`Live`, not `Removing`) when it was killed -- a live run then reported `namespaces_removed=0`:
     there was nothing of the victim's own to be hidden. The card now also drops each table on the
     victim, with that drop's own return awaited before the kill.

Live validation run (seed 4, 2026-08-03, after fixes 4-6): `ca-drop-member` reported
`namespaces_removed=3` (matching `victim_tables=3`) and several `CAS orphan sweep: retained ...`
warnings for manifests whose "epoch 1's closing seal is not consumed" -- a real, benign artifact of
decommissioning mid-epoch that the tool correctly leaves for the normal GC path rather than deleting
unconditionally. Final `fsck` showed `dangling=0` (some `unreachable` residue, which is the expected
pre-GC state the warnings describe, not corruption).
"""

import subprocess
import time

from soak.fsck import _CLICKHOUSE_DISKS

from ..framework import gc as gc_mod
from ..framework.assertions import assert_fsck_clean
from ..framework.base import Scenario, register
from ..framework.report import Verdict
from . import _common

_VICTIM_CONTAINER = "ca-soak-ch2-1"
_VICTIM_SRID = "ca_soak_ch2"
_SURVIVOR_CONTAINER = "ca-soak-ch1-1"
_TABLE_PREFIX = "s45_victim"


def _run_drop_member(container: str, srid: str, timeout_s: float = 300.0) -> dict:
    """Invoke `ca-drop-member` against `srid` from `container`. Reuses `soak/fsck.py`'s own
    `_CLICKHOUSE_DISKS` invocation prefix (the `ca_ro` disk is only defined in the standalone
    fsck-only config, not the server's own config.d -- `--config-file
    /etc/clickhouse-server/config.xml` would hit UNKNOWN_DISK)."""
    cmd = [
        "docker", "exec", container,
        *_CLICKHOUSE_DISKS,
        "--disk", "ca_ro",
        "--query", f"ca-drop-member {srid}",
    ]
    p = subprocess.run(cmd, capture_output=True, text=True, timeout=timeout_s)
    out = {"exit_code": p.returncode, "stdout": p.stdout, "stderr": p.stderr}
    for line in p.stdout.splitlines():
        if "=" in line:
            k, _, v = line.partition("=")
            try:
                out[k.strip()] = int(v.strip())
            except ValueError:
                out[k.strip()] = v.strip()
    return out


# `openForDecommission` -> `mountWritable` (CasPool.cpp) takes one unbounded-wait-free snapshot
# compare of now() against the victim's mount lease `expires_at_ms` and refuses outright if it
# still looks live -- there is no bounded-wait variant on this path (that only exists on the
# crash-recovery mount path). `docker kill` does not shorten the lease the victim already renewed
# before dying, so the poll bound below must cover the full lease lifetime: `mount_lease_ttl_ms`
# (30s default) plus one full `mount_renew_period` (10s) for the worst case where the victim
# renewed an instant before being killed, plus the `chaos.py` FREEZE_LONG fence margin (ttl/2) the
# GC leader itself relies on to call a lease conclusively lapsed.
_LEASE_WAIT_BOUND_S = 60.0
_LEASE_POLL_INTERVAL_S = 5.0


def _run_drop_member_after_lease_lapses(container: str, srid: str) -> dict:
    """Poll `ca-drop-member` until the victim's mount lease has provably lapsed (ABORTED /
    "alive or contended") or `_LEASE_WAIT_BOUND_S` is exceeded. Any other failure is returned
    immediately without retrying -- retrying would mask a real defect as a timing flake."""
    deadline = time.monotonic() + _LEASE_WAIT_BOUND_S
    last = _run_drop_member(container, srid)
    while last.get("exit_code") != 0 and "alive or contended" in last.get("stderr", "") \
            and time.monotonic() < deadline:
        time.sleep(_LEASE_POLL_INTERVAL_S)
        last = _run_drop_member(container, srid)
    return last


@register
class S45(Scenario):
    name = "S45"
    title = "decommission a victim member with hidden Removing catalog entries"
    priority = "P1"
    needs_infra = None  # built for T8; requires ca-drop-member wired into the compose image (gap #2/#3 above)

    param_table = {
        "dev": {"victim_tables": 3, "rows_per_table": 100},
        "ci": {"victim_tables": 6, "rows_per_table": 500},
        "full": {"victim_tables": 12, "rows_per_table": 2000},
    }

    def run(self, ctx, result):
        p = ctx.params
        node = ctx.cluster.node1
        victim = ctx.cluster.node2
        from ..framework import sql

        tables = [f"{_TABLE_PREFIX}_{i}" for i in range(int(p["victim_tables"]))]
        for t in tables:
            # A ReplicatedMergeTree materializes per-replica: each replica runs its own CREATE
            # against the shared zk path. Creating it only on node1 leaves the victim (node2) without
            # the table at all, so it never creates its OWN local namespace under its own srid --
            # there would be nothing of the victim's left to be Removing. A live run that created the
            # table on node1 only, then tried `SYSTEM SYNC REPLICA` on node2, hit `UNKNOWN_TABLE`
            # there outright.
            for n in (node, victim):
                sql.create_ca_table(n, t, columns="id UInt64, payload String", order_by="id", wide=True)
            sql.insert_random(node, t, rows=int(p["rows_per_table"]), payload_bytes=64)
            victim.command(f"SYSTEM SYNC REPLICA {t}", timeout=60)

        # Drop every victim table on BOTH replicas, then immediately kill the victim node -- before
        # its own background GC/janitor has a chance to condemn+retire these `Removing` rows.
        # Dropping only on node1 would leave the victim's own local replica still ATTACHED (a
        # ReplicatedMergeTree DROP is per-replica, not automatically cluster-wide), so the victim's
        # own namespace would stay Live, not Removing -- there would be nothing "hidden" to find. The
        # victim's own `DROP ... SYNC` must return before the kill so its namespace is provably
        # Removing, not merely mid-drop.
        for t in tables:
            node.query(f"DROP TABLE IF EXISTS {t} SYNC", timeout=120)
            victim.query(f"DROP TABLE IF EXISTS {t} SYNC", timeout=120)

        ctx.log(f"S45: killing victim {_VICTIM_CONTAINER} immediately after drop (before its own GC settles)")
        subprocess.run(["docker", "kill", _VICTIM_CONTAINER], capture_output=True, check=True)

        report = _run_drop_member_after_lease_lapses(_SURVIVOR_CONTAINER, _VICTIM_SRID)
        ctx.write_json("s45_drop_member_report.json", report)

        result.add(Verdict.check(
            "ca-drop-member exits cleanly", "exit_code == 0",
            f"exit_code={report.get('exit_code')}", report.get("exit_code") == 0,
            f"stderr: {report.get('stderr', '')[:500]}"))

        namespaces_removed = report.get("namespaces_removed")
        result.add(Verdict.check(
            "hidden Removing rows are accounted for", f">= {len(tables)}",
            f"namespaces_removed={namespaces_removed}",
            namespaces_removed is not None and namespaces_removed >= len(tables),
            "the victim's Removing rows (dropped but never condemned before the kill) must be swept "
            "by the tool, not left as permanent catalog debris"))

        gc_mod.forced_gc_to_fixpoint(ctx.cluster, lambda: 0)
        try:
            from soak import fsck as fsck_mod
            fsck = fsck_mod.run_fsck(_SURVIVOR_CONTAINER, disk="ca_ro", detail=False)
            assert_fsck_clean(result, fsck)
        except Exception as e:
            ctx.log(f"S45: final fsck raised: {e}")
            result.add(Verdict.inconclusive("fsck dangling", "0", f"final fsck raised: {e}"))

        ctx.log("S45: leaving the victim container down; the compose teardown at run end recreates it")
