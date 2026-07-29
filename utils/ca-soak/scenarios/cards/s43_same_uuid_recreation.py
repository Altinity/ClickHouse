"""S43 (W3) same-uuid recreation over a reused prefix — a survivor's queued write must not be absorbed.

The hazard Stage A task 3's fence test found and task 6 named without reaching end to end (task-6
report, obligation 10). Every other deposition story is closed by the writer epoch: a survivor whose
mount was superseded writes under an epoch its successor has already sealed, so its transaction is
conclusively rejected. This one scenario removes that defence on purpose:

  * the POOL is recreated over the same prefix, so the epoch counter starts from the beginning again;
  * the TABLE is recreated with the SAME uuid, so it lands on the same namespace path;
  * therefore a survivor of the previous life writing at `{1,2}` is NOT writing under a sealed epoch.
    It is writing at the id that, arithmetically, is the second transaction of the new life.

What actually defends here is QUIESCE: dropping the table and unmounting the pool stops the writer
before the prefix is reused, so no such transaction can still be in flight. This card removes that
defence too, by injecting the survivor's transaction directly into the recreated pool with `boto3`,
and then asks what the recreated pool's FIRST recovery of that namespace does with it.

The required answer is that it REFUSES the stream rather than absorbing it — task 6's analysis of
genesis grounding says a recovery that starts above the true first id meets either a non-contiguous
apply or a non-birth op on a never-born table, and both are `CORRUPTED_DATA` rather than a quietly
shorter table. The unacceptable answer is silent absorption: the recreated table exposing, or
pointing at, a previous life's state.

The card asserts the safety property in the form that does not depend on WHICH refusal fires:

  1. the recreated table never returns the previous life's rows;
  2. the always-zero counters (`CasRefApplyPoisoned`, `CasRefRecoveryStreamHole`) stay at zero;
  3. and it RECORDS, as an observation rather than a verdict, whether the touch raised or returned
     empty — because "refused loudly" and "started clean and ignored it" are both safe, and pinning
     one of them as the only acceptable outcome would make this card fail on a correct change.

An injected object that is simply never read is the third safe outcome, and it is why (1) is the
verdict rather than "the query must throw": the walk reads by exact key from the namespace's birth,
so a body at an id the new life has not reached yet is not on any path until the new life allocates
that far.
"""

import time

from soak.cluster import QueryError

from ..framework import cluster_boot, observe, sql
from ..framework.base import Scenario, register
from ..framework.report import Verdict
from . import _common

from .s38_late_put_injection import (
    _POOL_PREFIX,
    _S3_BUCKET,
    _discover_table_namespace,
    _list_keys,
    _render_ref_txn_id,
    _restamp_ref_log_txn,
    _s3_client,
)

_TABLE = "w3_recreated"

# Fixed so the two lives of the table share a namespace path. The whole scenario is that the second
# life reuses the first life's prefix, which a fresh uuid would quietly avoid.
_UUID = "3e1f0a2b-4c5d-4e6f-8a9b-0c1d2e3f4a5b"

# The survivor's queued write: sequence 2 of writer epoch 1. In the RECREATED pool that is exactly
# the id the new life's second transaction would take, which is what makes this the one shape the
# epoch cannot fence.
_SURVIVOR_EPOCH = 1
_SURVIVOR_SEQ = 2

_VIOLATION_EVENTS = ("CasRefApplyPoisoned", "CasRefRecoveryStreamHole")


def _create(node, name: str, table_uuid: str) -> None:
    """`create_ca_table` with an explicit uuid. Written out here rather than threading a uuid through
    the shared helper: this is the only card that needs one, and the reuse IS the scenario."""
    node.command(
        f"CREATE TABLE {name} UUID '{table_uuid}' (id UInt64, payload String) "
        f"ENGINE = MergeTree ORDER BY (id) "
        f"SETTINGS storage_policy='ca', min_bytes_for_wide_part=0, min_rows_for_wide_part=0, "
        f"search_orphaned_parts_disks='local'")


def _wipe_pool(s3, log_fn) -> int:
    """Delete every object under the pool prefix — the pool, recreated over a reused prefix."""
    keys = _list_keys(s3, f"{_POOL_PREFIX}/")
    for i in range(0, len(keys), 1000):
        s3.delete_objects(Bucket=_S3_BUCKET,
                          Delete={"Objects": [{"Key": k} for k in keys[i:i + 1000]]})
    log_fn(f"S43: wiped {len(keys)} objects under {_POOL_PREFIX}/ — the pool is recreated over the "
           f"same prefix, so its writer-epoch counter starts from the beginning")
    return len(keys)


def _violation_counters(cluster) -> dict:
    peak = {e: 0 for e in _VIOLATION_EVENTS}
    for node in cluster.nodes():
        try:
            ev = observe.events_snapshot(node)
        except Exception:
            continue
        for e in _VIOLATION_EVENTS:
            peak[e] = max(peak[e], int(ev.get(e, 0) or 0))
    return peak


@register
class S43(Scenario):
    name = "S43"
    title = "same-uuid recreation over a reused prefix: a survivor's write is not absorbed"
    priority = "P0"
    compose_variant = "s38"   # for the published RustFS port; the injection needs direct pool access
    param_table = {
        "dev": {"rows": 200, "payload_bytes": 256, "heal_timeout_s": 240},
        "ci": {"rows": 2000, "payload_bytes": 512, "heal_timeout_s": 300},
        "full": {"rows": 20000, "payload_bytes": 1024, "heal_timeout_s": 360},
    }

    def run(self, ctx, result):
        cl = ctx.cluster
        p = ctx.params
        rows = int(p["rows"])
        payload = int(p["payload_bytes"])
        heal_timeout_s = int(p["heal_timeout_s"])

        # =====================================================================================
        # Life 1: a table on the CA disk with a pinned uuid, with real content in its ref stream.
        # =====================================================================================
        cl.node1.command(f"DROP TABLE IF EXISTS {_TABLE} SYNC")
        _create(cl.node1, _TABLE, _UUID)
        sql.insert_random(cl.node1, _TABLE, rows=rows, payload_bytes=payload, op_id=0)
        life1_rows = int(cl.node1.scalar(f"SELECT count() FROM {_TABLE}") or 0)
        life1_checksum = cl.node1.query(sql.table_checksum_query(_TABLE)).strip()
        result.observations["life1"] = {"uuid": _UUID, "rows": life1_rows}
        result.add(Verdict.check(
            "life 1 has content to be absorbed", f"{rows} rows written and readable",
            life1_rows, life1_rows == rows,
            "" if life1_rows == rows else "the first life never got its rows, so a later absorption "
                                          "would have nothing recognisable to absorb"))

        s3 = _s3_client()
        ns = _discover_table_namespace(s3, "ca_soak_ch1")
        result.observations["namespace"] = ns
        if ns is None or _UUID not in ns:
            result.add(Verdict.inconclusive(
                "the pinned uuid appears in the namespace path",
                f"a namespace under cas/refs/ca_soak_ch1/ containing {_UUID}",
                f"discovered ns={ns!r} — cannot demonstrate prefix reuse without it"))
            _common.standard_end(ctx, result, [_TABLE])
            return

        log_prefix = f"{_POOL_PREFIX}/cas/refs/{ns}/_log/"
        life1_keys = _list_keys(s3, log_prefix)
        if not life1_keys:
            result.add(Verdict.inconclusive(
                "life 1 wrote a ref-log stream", ">0 objects under the namespace's _log/",
                f"none under {log_prefix}"))
            _common.standard_end(ctx, result, [_TABLE])
            return
        # A body the server itself wrote, kept for restamping — the survivor's queued write is a real
        # transaction of the previous life, not something this card invented.
        donor_key = sorted(life1_keys)[-1]
        donor_body = s3.get_object(Bucket=_S3_BUCKET, Key=donor_key)["Body"].read()
        result.observations["donor"] = {"key": donor_key, "bytes": len(donor_body)}

        # =====================================================================================
        # Quiesce, then remove it: drop the table (the defence), stop the servers, recreate the pool.
        # =====================================================================================
        cl.node1.command(f"DROP TABLE {_TABLE} SYNC")
        ctx.log("S43: table dropped (quiesce — the defence this card is about to remove)")
        stop_rc = cluster_boot.compose_run(self.compose_variant, "stop", "ch1", "ch2",
                                          log_fn=ctx.log)
        result.observations["compose_stop_rc"] = stop_rc
        time.sleep(3)
        wiped = _wipe_pool(s3, ctx.log)
        result.observations["pool_wipe"] = {"objects_deleted": wiped}

        survivor_id = _render_ref_txn_id(_SURVIVOR_EPOCH, _SURVIVOR_SEQ)
        survivor_key = f"{log_prefix}{survivor_id}"
        survivor_body = _restamp_ref_log_txn(donor_body, _SURVIVOR_SEQ, writer_epoch=_SURVIVOR_EPOCH)
        s3.put_object(Bucket=_S3_BUCKET, Key=survivor_key, Body=survivor_body)
        ctx.log(f"S43: injected the survivor's queued write at {survivor_id} into the recreated pool")
        result.observations["survivor"] = {
            "key": survivor_key, "txn_id": survivor_id,
            "body": survivor_body.decode(errors="replace")}
        planted = s3.get_object(Bucket=_S3_BUCKET, Key=survivor_key)["Body"].read()
        result.add(Verdict.check(
            "the survivor's write is present in the recreated pool before life 2 starts",
            "a GET of the injected id returns the injected body",
            f"{len(planted)} bytes", planted == survivor_body, ""))

        start_rc = cluster_boot.compose_run(self.compose_variant, "start", "ch1", "ch2",
                                           log_fn=ctx.log)
        result.observations["compose_start_rc"] = start_rc
        healthy = cluster_boot.wait_healthy(cl, timeout_s=heal_timeout_s, log_fn=ctx.log)
        result.add(Verdict.check(
            "the servers come back on the recreated pool", "healthy within heal_timeout_s",
            f"healthy={healthy}", healthy, ""))
        if not healthy:
            _common.standard_end(ctx, result, [_TABLE])
            return

        # =====================================================================================
        # Life 2: same uuid, same namespace path, and a foreign transaction already sitting at {1,2}.
        # =====================================================================================
        before = _violation_counters(cl)
        create_error = None
        try:
            _create(cl.node1, _TABLE, _UUID)
        except QueryError as e:
            create_error = str(e)[:400]

        touch_error = None
        life2_rows = None
        life2_checksum = None
        if create_error is None:
            try:
                life2_rows = int(cl.node1.scalar(f"SELECT count() FROM {_TABLE}") or 0)
                life2_checksum = cl.node1.query(sql.table_checksum_query(_TABLE)).strip()
            except QueryError as e:
                touch_error = str(e)[:400]

        after = _violation_counters(cl)
        result.observations["life2"] = {
            "create_error": create_error, "touch_error": touch_error, "rows": life2_rows,
            "checksum": life2_checksum, "life1_checksum": life1_checksum,
            "violation_counters": {"before": before, "after": after},
            "outcome": ("create refused" if create_error else
                        "touch refused" if touch_error else
                        f"started clean ({life2_rows} rows)")}

        # THE assertion. Refusing loudly and starting clean are both safe; returning the previous
        # life's data is the one outcome that is not.
        absorbed = life2_rows is not None and life2_rows > 0
        result.add(Verdict.check(
            "the recreated table does not absorb the previous life's state",
            "life 2 exposes 0 rows (or refuses outright) — never life 1's rows",
            result.observations["life2"]["outcome"], not absorbed,
            "" if not absorbed else
            f"the recreated table returned {life2_rows} row(s) over a reused prefix. A survivor's "
            f"queued transaction was absorbed into a new life: the epoch could not fence it (the "
            f"pool's counter was reset) and quiesce was the only defence"))
        if life2_checksum is not None:
            result.add(Verdict.check(
                "life 2's checksum is not life 1's",
                "the two lives do not agree — they share only a prefix, not a state",
                f"life1={life1_checksum!r} life2={life2_checksum!r}",
                life2_checksum != life1_checksum or life1_rows == 0, ""))

        moved = {e: after[e] - before[e] for e in _VIOLATION_EVENTS if after[e] > before[e]}
        result.add(Verdict.check(
            "no always-zero counter moved across the recreation",
            f"all of {', '.join(_VIOLATION_EVENTS)} unchanged", moved or "unchanged", not moved,
            "" if not moved else f"counters moved: {moved}"))

        _common.standard_end(ctx, result, [_TABLE])
