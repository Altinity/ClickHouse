"""S38 unclean handover and the late-PUT-loses fence (P0).

**A fenced predecessor's PUT never materializes below coverage its successor has already declared.**
That is the whole card. The injection shape is unchanged from the pre-Stage-A version — kill `ch1`
mid append-storm, restart it, then push a dead-epoch `_log` object straight into the pool with
`boto3`, exactly as a straggling out-of-band PUT from the dead predecessor would look on the wire —
but what is asserted flipped from DETECTION to FENCING.

The old card asserted that a late log was *noticed*: a `materialization grace` wait in the log, an
`unclean_epoch_boundary_seen` flag, a sentinel recovery seal, a `ref_late_log_detected` event from a
LIST-based sweep. Stage A retired all four (tasks 6 and 12) and replaced the whole family with one
in-band object: recovery closes a dead epoch by writing an `EpochSeal` at the exact log id the dying
predecessor's next PUT would take, as a CONDITIONAL CREATE. Nothing waits and nothing is watched for,
because the seal makes the late PUT *lose* rather than merely *visible*.

So the assertions are now about that seal, in three layers:

  1. **It exists.** An unclean restart's recovery mints one per dead epoch it closes
     (`CASRefRecoveryEpochSealed`), and this card reads the object back out of the pool and checks it
     really is an `epoch_seal` transaction sitting at the top of the dead epoch's stream. A CLEAN
     restart mints one too — sealing is decided by arithmetic (every epoch below the live one is
     closed, however its mount died), not by an unclean-boundary flag, and the clean-restart step at
     the end is what pins that. Its predecessor is the old card's "a clean stop/start pays no T_mat
     wait" check, inverted by the same commit that deleted the wait.

  2. **It wins the slot.** The straggler's PUT is replayed with the same primitive the writer uses, a
     conditional create at the id the seal took, and must be REFUSED by the store. This is the fence
     itself, tested at the level where it lives: a plain overwrite would prove nothing, since no
     protocol survives a store that lets one client clobber another's committed object.

  3. **It covers the region above it.** A raw, unconditional PUT then lands at
     `{dead_epoch, huge_seq}` — inside the sealed region, above the seal, where nothing legitimate can
     ever be. This is the out-of-band materialization the invariant is really about, and the assertion
     is that it has NO observable effect: the table's checksum does not move, the replicas still
     agree, a full restart re-recovers the namespace from the durable stream with the object sitting
     there and still returns the same data, and the counters that carry "a durable transaction went
     missing" / "the fold advanced past work" / "the stream stopped being dense"
     (`CASRefNeedsRecovery`, `CASGcUnappliedFoldedTxns`, `CASRefRecoveryStreamHole`) all stay at zero
     across driven GC rounds. The recovery walk reads the dead epoch by exact key, meets the seal and
     advances to the next epoch, so an id above the seal is unreachable by construction — the
     injected object is not "tolerated", it is not on any path.

The one mechanism from the old card that Stage A did NOT retire is the mount-claim OBSERVATION wait,
and step 1 still asserts it: an unclean predecessor's lease is observed rather than assumed dead.

The injected body is built by taking a real ref-log object the server itself wrote in that epoch and
rewriting only its sequence number, then dropping the ops. That keeps the card honest about a wire
format it does not own — everything but `rs` comes from a body the product produced — while an
op-count of zero means the transaction can never poison a real later fold even if some path does GET
and decode it (`manifestEdgesOfTxn` of an empty op list is empty). The body duplicates the id, and
`decodeRefLogTxn` rejects a body whose id does not match the key it was read from, which is why the
rewrite is necessary rather than a verbatim copy.

Dev scale is deliberately small (a couple dozen small inserts) so a developer run finishes in a few
minutes; ci/full scale up the storm.
"""

import json
import threading
import time

from soak.chaos import Fault, FaultTarget, FaultAction, apply_fault

from ..framework import cluster_boot, gc as gc_mod, observe, sql
from ..framework.base import Scenario, register
from ..framework.report import Verdict
from . import _common

_TABLE = "s38_handover"

# S3 endpoint published by docker-compose-s38.yml (rustfs1 18121 -> 11121) so the card can inject a
# raw object directly into the pool from the host process, without an extra throwaway container.
_S3_ENDPOINT = "http://localhost:18121"
_S3_BUCKET = "test"
_POOL_PREFIX = "soak_pool"   # matches observe.POOL_DIR / storage_conf.xml's endpoint sub-path

# Far above anything a dev-scale storm could allocate as a real `ref_sequence`, so the injected object
# is unambiguously INSIDE the region the seal closed and above every real transaction in it. One below
# the u64 maximum, which several code paths use as an open-ended sentinel.
_HUGE_SEQ = 0xFFFFFFFFFFFFFFFE

# Counters that carry the violation half of the invariant. Each is documented in `ProfileEvents.cpp`
# as always-zero; the soak driver gates on the same three (`soak/signals.py`
# LATE_PUT_VIOLATION_NOTES), and this card asserts them around the injection specifically.
_VIOLATION_EVENTS = ("CASRefNeedsRecovery", "CASGcUnappliedFoldedTxns", "CASRefRecoveryStreamHole")

def _render_ref_txn_id(writer_epoch: int, ref_sequence: int) -> str:
    """Mirrors `renderRefTxnId` (CasRefIds.h): two 16-digit lowercase hex fields joined by '-'."""
    return f"{writer_epoch:016x}-{ref_sequence:016x}"


# `Layout::refLogKey` is `<life stream prefix>_log/<render>` + `storedSuffix(FormatId::RefLog)`, and RefLog is
# `CompressionPolicy::Always` (`CasFormat.cpp:110`), so every ref-log object's key ends `.zst` and its
# BODY is zstd. Both facts bit the first run of this card: the listing parsed to zero ids because the
# suffix broke the id parse, and the body would not have decoded as text either.
_REF_LOG_SUFFIX = ".zst"


def _parse_ref_txn_id(leaf: str):
    """Inverse of `_render_ref_txn_id`. Returns `(writer_epoch, ref_sequence)` or None. Tolerates the
    stored suffix, and anything nested deeper than one level under `_log/` simply fails to parse."""
    if leaf.endswith(_REF_LOG_SUFFIX):
        leaf = leaf[: -len(_REF_LOG_SUFFIX)]
    parts = leaf.split("-")
    if len(parts) != 2 or not all(len(p) == 16 for p in parts):
        return None
    try:
        return int(parts[0], 16), int(parts[1], 16)
    except ValueError:
        return None


def _restamp_ref_log_txn(body: bytes, ref_sequence: int, writer_epoch: int | None = None,
                         keep_ops: bool = False) -> bytes:
    """Rewrite a real ref-log object's id and strip its ops.

    The format is line-oriented JSON (`CasTextFormat.cpp`): a header line, a meta line carrying
    `ns`/`we`/`rs` (u64 as decimal STRINGS, `readU64String`), then one line per op, then a trailer
    `{"n": <op count>}`. Only the id fields and the op list change here, so the header (with its
    compatibility version) and the namespace come from a body the server wrote — this card does not
    encode the format, it edits two fields of it. `writer_epoch` defaults to the donor's own.
    """
    lines = _zstd_decompress(body).decode().splitlines()
    if len(lines) < 3:
        raise ValueError(f"ref-log body has {len(lines)} lines, expected at least 3 (header/meta/trailer)")
    meta = json.loads(lines[1])
    if "rs" not in meta or "ns" not in meta or "we" not in meta:
        raise ValueError(f"ref-log meta line lacks ns/we/rs: {lines[1][:200]}")
    meta["rs"] = str(ref_sequence)
    if writer_epoch is not None:
        meta["we"] = str(writer_epoch)
    # `!pse` is a chain link, legal only at sequence 1; the restamped id is not sequence 1, so a
    # copied link would be rejected by the seal grammar for a reason unrelated to this card.
    meta.pop("!pse", None)
    meta.pop("!pss", None)
    # `keep_ops=False` is the S38 shape: an op-count of zero can never poison a real later fold, which
    # is what you want when the point is that the object is never READ. S43 needs the opposite — an
    # object whose absorption would be OBSERVABLE — so it keeps the donor's ops and the trailer that
    # counts them (the op lines sit between the meta line and the trailer).
    if keep_ops:
        ops = lines[2:-1]
        out = [lines[0], json.dumps(meta, separators=(",", ":"))] + ops + ['{"n":%d}' % len(ops)]
    else:
        out = [lines[0], json.dumps(meta, separators=(",", ":")), '{"n":0}']
    return _zstd_compress(("\n".join(out) + "\n").encode())


def _zstd_decompress(data: bytes) -> bytes:
    import zstandard
    # `stream_reader`, not `decompress`: ClickHouse's frames need not carry the decompressed size, and
    # the one-shot API refuses those.
    import io
    with zstandard.ZstdDecompressor().stream_reader(io.BytesIO(data)) as r:
        return r.read()


def _zstd_compress(data: bytes) -> bytes:
    import zstandard
    return zstandard.ZstdCompressor().compress(data)


def _s3_client():
    import boto3
    from botocore.config import Config
    return boto3.client(
        "s3", endpoint_url=_S3_ENDPOINT, aws_access_key_id="clickhouse",
        aws_secret_access_key="clickhouse", region_name="us-east-1",
        config=Config(s3={"addressing_style": "path"}, retries={"max_attempts": 5}))


def _list_common_prefixes(s3, prefix: str) -> list:
    resp = s3.list_objects_v2(Bucket=_S3_BUCKET, Prefix=prefix, Delimiter="/")
    return [p["Prefix"] for p in resp.get("CommonPrefixes", [])]


def _list_keys(s3, prefix: str) -> list:
    keys = []
    token = None
    while True:
        kw = {"Bucket": _S3_BUCKET, "Prefix": prefix}
        if token:
            kw["ContinuationToken"] = token
        resp = s3.list_objects_v2(**kw)
        keys += [o["Key"] for o in resp.get("Contents", [])]
        if not resp.get("IsTruncated"):
            return keys
        token = resp.get("NextContinuationToken")


# S38 and S43 deliberately create one table in an otherwise empty pool. The current layout therefore
# exposes exactly one DIRECT child under `cas/ns/stream/`: its opaque, canonical 32-hex life id. Do not
# reconstruct a logical namespace from storage keys; the catalog is the only such mapping. Refuse zero,
# multiple, malformed, uppercase, or nested children rather than guessing which life the card should
# inject into.
def _discover_single_life_id(s3):
    prefix = f"{_POOL_PREFIX}/cas/ns/stream/"
    children = _list_common_prefixes(s3, prefix)
    if len(children) != 1:
        return None
    child = children[0]
    if not child.startswith(prefix) or not child.endswith("/"):
        return None
    life_id = child[len(prefix):-1]
    if len(life_id) != 32 or any(c not in "0123456789abcdef" for c in life_id):
        return None
    return life_id


def _injected_log_observation(life_id: str, key: str, txn_id: str, body: bytes) -> dict:
    """Stable report shape for the real post-injection path; storage keys expose a life id, not `ns`."""
    return {
        "life_id": life_id,
        "key": key,
        "txn_id": txn_id,
        "body": body.decode(errors="replace"),
    }


def _text_log_count(node, since: str, needle: str) -> int:
    """Count `system.text_log` rows containing `needle` (case-insensitive) at/after `since`. Flushes
    logs first (system log tables buffer in memory) — mirrors observe.gc_log_rows's flush pattern."""
    try:
        node.command("SYSTEM FLUSH LOGS")
        v = node.scalar(
            f"SELECT count() FROM system.text_log WHERE event_time >= '{since}' "
            f"AND message ILIKE '%{needle}%'")
        return int(v or 0)
    except Exception:
        return -1   # probe failure is distinct from a genuine 0 — caller treats <0 as inconclusive


def _violation_counters(cluster, events):
    """Peak value of each always-zero counter across the cluster.

    FAIL-AWARE, deliberately. The first version caught every exception and SKIPPED the node, so a probe
    that failed on both nodes returned a dict of clean zeros and the invariant "nothing moved" passed
    while nothing had been read — the same laundering the soak's own `SignalTracker` was written to
    prevent. A read failure on a required node is a CARD FAILURE, raised here, because a counter that
    cannot be read is not a counter that is quiet."""
    peak = {e: 0 for e in events}
    names = ", ".join(f"'{e}'" for e in events)
    for node in cluster.nodes():
        # `system_events_show_zero_values = 1` is what makes the missing-counter check meaningful.
        # Without it `system.events` OMITS every counter that has never incremented, so "the binary
        # does not have this counter" and "this counter is zero" are the same observation — and a probe
        # that treats absence as a hard error then fails on a perfectly healthy fresh cluster, which is
        # exactly what happened on this card's first run. With it, the binary enumerates its whole
        # registry, so a name still missing really is missing. Same technique as
        # `soak/signals.py:read_signal_events`.
        try:
            txt = node.query(
                f"SELECT event, value FROM system.events WHERE event IN ({names}) "
                f"FORMAT TabSeparated SETTINGS system_events_show_zero_values = 1")
        except Exception as exc:
            raise RuntimeError(
                f"counter probe FAILED on {node!r} ({type(exc).__name__}: {str(exc)[:160]}) — refusing "
                f"to report the always-zero invariant as held on an unread node") from exc
        ev = {}
        for line in txt.splitlines():
            if "\t" in line:
                k, v = line.split("\t", 1)
                try:
                    ev[k] = int(v)
                except ValueError:
                    pass
        missing = [e for e in events if e not in ev]
        if missing:
            raise RuntimeError(
                f"counter probe on {node!r} did not return {missing} even with "
                f"system_events_show_zero_values=1 — this binary does not have those counters; "
                f"refusing to treat absence as zero")
        for e in events:
            peak[e] = max(peak[e], int(ev[e]))
    return peak


@register
class S38(Scenario):
    name = "S38"
    title = "unclean handover: the epoch seal makes a late predecessor PUT lose"
    priority = "P0"
    compose_variant = "s38"
    param_table = {
        "dev": {"storm_inserts": 20, "rows_per_insert": 50, "payload_bytes": 512,
                "kill_delay_s": 1.5, "kill_down_s": 3, "heal_timeout_s": 180},
        "ci": {"storm_inserts": 60, "rows_per_insert": 300, "payload_bytes": 1024,
               "kill_delay_s": 2.0, "kill_down_s": 4, "heal_timeout_s": 240},
        "full": {"storm_inserts": 150, "rows_per_insert": 1000, "payload_bytes": 2048,
                 "kill_delay_s": 3.0, "kill_down_s": 5, "heal_timeout_s": 300},
    }

    def run(self, ctx, result):
        cl = ctx.cluster
        p = ctx.params
        storm_inserts = int(p["storm_inserts"])
        rows = int(p["rows_per_insert"])
        payload = int(p["payload_bytes"])
        kill_delay_s = float(p["kill_delay_s"])
        kill_down_s = int(p["kill_down_s"])
        heal_timeout_s = int(p["heal_timeout_s"])
        result.observations["scale"] = {
            "storm_inserts": storm_inserts, "rows_per_insert": rows, "payload_bytes": payload,
            "note": "DEV-scale: a couple dozen small inserts before the kill; ci/full scale up the "
                    "storm. Nothing in this card waits on a timer any more — the fence is an object, "
                    "not a grace period — so scale only changes how much real stream the dead epoch "
                    "has under its seal."}

        sql.create_ca_table(cl.node1, _TABLE, columns="id UInt64, payload String", order_by="id",
                            wide=True)
        sql.create_ca_table(cl.node2, _TABLE, columns="id UInt64, payload String", order_by="id",
                            wide=True)

        # =====================================================================================
        # Step 1: kill -9 ch1 mid append-storm, restart. The dying epoch leaves a stream that ends
        # wherever the kill landed — which is what gives the successor a dead epoch to close.
        # =====================================================================================
        since_kill = cl.node1.scalar("SELECT toString(now())")
        stop = threading.Event()
        wl_stats = {"inserts_ok": 0, "inserts_failed": 0}

        def _storm():
            i = 0
            while not stop.is_set() and i < storm_inserts:
                try:
                    sql.insert_random(cl.node1, _TABLE, rows=rows, payload_bytes=payload, op_id=i * rows)
                    wl_stats["inserts_ok"] += 1
                except Exception as e:
                    wl_stats["inserts_failed"] += 1
                    ctx.log(f"S38 storm insert failed (adversarial, continuing): {str(e)[:160]}")
                i += 1

        wl_thread = threading.Thread(target=_storm, daemon=True)
        wl_thread.start()
        time.sleep(kill_delay_s)
        ctx.log(f"S38: KILL ch1 mid append-storm (down {kill_down_s}s)")
        apply_fault(Fault(t_offset=0, target=FaultTarget.CH1, action=FaultAction.KILL,
                          duration_s=kill_down_s))
        stop.set()
        wl_thread.join(timeout=120)
        result.observations["storm_stats"] = wl_stats

        healthy = cluster_boot.wait_healthy(cl, timeout_s=heal_timeout_s, log_fn=ctx.log)
        result.add(Verdict.check(
            "ch1 recovers after kill -9", "healthy within heal_timeout_s",
            f"healthy={healthy}", healthy,
            "" if healthy else "ch1 did not answer /ping within the heal timeout"))
        if not healthy:
            _common.standard_end(ctx, result, [_TABLE])
            return

        # The one mechanism of the pre-Stage-A card that was NOT retired: the successor OBSERVES the
        # stale-looking lease's write-token rather than assuming its holder is dead.
        obs_n = _text_log_count(cl.node1, since_kill, "stale-looking mount lease")
        sparing_n = _text_log_count(cl.node1, since_kill, "delete_pending retired entry recovered in-degree")
        result.observations["unclean_restart_log_counts"] = {
            "observation_wait": obs_n, "in_degree_sparing_warning": sparing_n}
        result.add(Verdict.check(
            "observation wait line appears (unclean restart)",
            ">0 'stale-looking mount lease' rows in system.text_log since kill",
            obs_n, obs_n > 0,
            "" if obs_n > 0 else "ch1's restart did not log the mount-claim observation wait — "
                                 "either the predecessor's death looked clean, or the log is missing"))
        result.add(Verdict.check(
            "no in-degree sparing warning (delete_pending retired entry recovered)",
            "0 'delete_pending retired entry recovered in-degree' rows",
            sparing_n, sparing_n == 0,
            "" if sparing_n == 0 else "the unclean-handover recovery triggered a "
                                      "delete_pending/in-degree sparing warning — unexpected on a "
                                      "clean append-storm kill with no prior GC condemnation"))

        # =====================================================================================
        # Step 2: the seal exists. Touching the table drives the lazy per-namespace recovery, whose
        # CAS-walk closes every epoch below the live one.
        # =====================================================================================
        pre_inject_checksum = cl.node1.query(sql.table_checksum_query(_TABLE)).strip()
        sealed = observe.events_snapshot(cl.node1).get("CASRefRecoveryEpochSealed", 0)
        result.observations["recovery_seal"] = {"CASRefRecoveryEpochSealed": sealed}
        result.add(Verdict.check(
            "the unclean restart's recovery sealed the dead epoch",
            "CASRefRecoveryEpochSealed > 0 (system.events, fresh since this process's start)",
            sealed, sealed > 0,
            "" if sealed > 0 else "no epoch seal was minted since ch1 restarted — the CAS-walk closes "
                                  "every epoch below the live one by arithmetic, so a zero here means "
                                  "the walk never ran or the namespace has no dead epoch at all"))
        if not sealed:
            _common.standard_end(ctx, result, [_TABLE])
            return

        s3 = _s3_client()
        life_id = _discover_single_life_id(s3)
        result.observations["discovered_life_id"] = life_id
        if life_id is None:
            result.add(Verdict.inconclusive(
                "opaque life id discovered for injection",
                "exactly one canonical 32-hex child under cas/ns/stream/",
                "the single-table pool did not expose one unambiguous life-id child"))
            _common.standard_end(ctx, result, [_TABLE])
            return

        log_prefix = f"{_POOL_PREFIX}/cas/ns/stream/{life_id}/_log/"
        ids = [i for i in (_parse_ref_txn_id(k[len(log_prefix):]) for k in _list_keys(s3, log_prefix))
               if i is not None]
        epochs = sorted({e for e, _ in ids})
        result.observations["ref_log_epochs"] = epochs

        # The dead epoch is the one whose TOP object is a seal, not "the lowest of at least two".
        # Requiring two epochs in the listing was wrong: a seal closing epoch N is written AT
        # `{N, T+1}`, i.e. inside epoch N, and the live epoch N+1 need not have written anything yet —
        # so the expected shape right after an unclean restart is a SINGLE listed epoch that ends in a
        # seal. (Found by running the card: `ref_log_epochs = [1]` with a seal already minted.)
        dead_epoch = seal_seq = seal_key = seal_body = None
        for candidate in epochs:
            top = max(sq for e, sq in ids if e == candidate)
            key = f"{log_prefix}{_render_ref_txn_id(candidate, top)}{_REF_LOG_SUFFIX}"
            body = s3.get_object(Bucket=_S3_BUCKET, Key=key)["Body"].read()
            if b'"epoch_seal"' in _zstd_decompress(body):
                dead_epoch, seal_seq, seal_key, seal_body = candidate, top, key, body
                break
        if dead_epoch is None:
            result.add(Verdict.inconclusive(
                "a dead epoch exists to be sealed",
                "one listed writer epoch whose highest ref-log id carries an `epoch_seal` op",
                f"epochs present: {epochs}, none of them ends in a seal — the storm did not span the "
                f"restart, or the seal has not been written yet"))
            _common.standard_end(ctx, result, [_TABLE])
            return

        seal_id = _render_ref_txn_id(dead_epoch, seal_seq)
        is_seal = True
        result.observations["seal"] = {
            "dead_epoch": dead_epoch, "seal_txn_id": seal_id, "key": seal_key,
            "body": seal_body.decode(errors="replace")[:400]}
        result.add(Verdict.check(
            "the top of the dead epoch's stream IS an epoch seal",
            "the object at the dead epoch's highest ref-log id carries an `epoch_seal` op",
            f"{seal_id} -> {seal_body[:120]!r}", is_seal,
            "" if is_seal else "the highest id of the dead epoch is an ordinary transaction, not a "
                               "seal — the epoch was never closed, so nothing fences a straggler"))

        # =====================================================================================
        # Step 3: the seal WINS THE SLOT. Replay the straggler's PUT with the writer's own primitive.
        # =====================================================================================
        straggler_body = _restamp_ref_log_txn(seal_body, seal_seq)
        # The refusal must be the STORE's, at the protocol's own status. Accepting any exception would
        # let a client-side mistake — a botocore that does not know `IfNoneMatch`, a bad endpoint —
        # pass as "the fence held", which is the one way this assertion could lie in the safe direction.
        outcome = {"raised": None, "code": None, "http_status": None}
        try:
            s3.put_object(Bucket=_S3_BUCKET, Key=seal_key, Body=straggler_body, IfNoneMatch="*")
        except Exception as e:
            resp = getattr(e, "response", None) or {}
            outcome["raised"] = type(e).__name__
            outcome["code"] = resp.get("Error", {}).get("Code")
            outcome["http_status"] = resp.get("ResponseMetadata", {}).get("HTTPStatusCode")
        result.observations["conditional_create_outcome"] = outcome
        refused_by_store = outcome["http_status"] in (409, 412)
        result.add(Verdict.check(
            "a straggler's conditional create at the sealed id is REFUSED by the store",
            "HTTP 412 (or 409) from a create at an id the seal already occupies",
            outcome, refused_by_store,
            "" if refused_by_store else
            ("the conditional create SUCCEEDED at the sealed id — either it overwrote the seal or the "
             "store is not honouring the precondition; the whole fence rests on this being impossible"
             if outcome["raised"] is None else
             f"the create failed with {outcome['raised']} rather than a precondition refusal — that is "
             f"a CARD defect (or an unreachable endpoint), not evidence about the fence")))
        after_refusal = s3.get_object(Bucket=_S3_BUCKET, Key=seal_key)["Body"].read()
        result.add(Verdict.check(
            "the seal object is byte-for-byte unchanged by the refused create",
            "GET of the sealed id returns exactly the seal that was there before",
            f"{len(after_refusal)} bytes, identical={after_refusal == seal_body}",
            after_refusal == seal_body,
            "" if after_refusal == seal_body else "the seal's bytes CHANGED — the late PUT won"))

        # =====================================================================================
        # Step 4: the seal COVERS THE REGION ABOVE IT. A raw PUT lands above the seal, inside the
        # closed epoch, and must be inert: the walk meets the seal and advances, so this id is on no
        # path at all.
        # =====================================================================================
        injected_id = _render_ref_txn_id(dead_epoch, _HUGE_SEQ)
        injected_key = f"{log_prefix}{injected_id}{_REF_LOG_SUFFIX}"
        injected_body = _restamp_ref_log_txn(seal_body, _HUGE_SEQ)
        ctx.log(f"S38: injecting a late dead-epoch log ABOVE the seal at "
                f"s3://{_S3_BUCKET}/{injected_key} ({len(injected_body)} bytes)")
        s3.put_object(Bucket=_S3_BUCKET, Key=injected_key, Body=injected_body)
        result.observations["injected_log"] = _injected_log_observation(
            life_id, injected_key, injected_id, injected_body)

        since_inject = cl.node1.scalar("SELECT toString(now())")
        violations_before = _violation_counters(cl, _VIOLATION_EVENTS)
        for _ in range(3):
            for idx in range(len(cl.nodes())):
                gc_mod.gc_drive_round(cl, log_fn=ctx.log, node_index=idx)
        gc_summary = observe.gc_log_all(cl, since_inject).get("summary", {})
        violations_after = _violation_counters(cl, _VIOLATION_EVENTS)
        # An EMPTY delta map is only meaningful if both readings actually covered every counter; the
        # fail-aware probe above guarantees that, so `measured` can never be silently partial.
        measured = sorted(set(violations_before) & set(violations_after))
        moved = {e: violations_after[e] - violations_before[e]
                 for e in measured if violations_after[e] > violations_before[e]}
        unmeasured = [e for e in _VIOLATION_EVENTS if e not in measured]
        result.observations["violation_counters"] = {
            "before": violations_before, "after": violations_after, "measured": measured,
            "unmeasured": unmeasured, "gc_rounds": gc_summary}
        result.add(Verdict.check(
            "no always-zero counter moved across the injection and driven GC",
            f"all of {', '.join(_VIOLATION_EVENTS)} read on every node and unchanged",
            {"moved": moved, "unmeasured": unmeasured} if (moved or unmeasured) else "unchanged",
            not moved and not unmeasured,
            "" if not moved else f"counters moved: {moved} — a durable transaction went missing, the "
                                 "fold advanced past work, or the stream stopped being dense"))

        post_inject_checksum = cl.node1.query(sql.table_checksum_query(_TABLE)).strip()
        unaffected = post_inject_checksum == pre_inject_checksum
        result.observations["checksums"] = {
            "pre_inject": pre_inject_checksum, "post_inject": post_inject_checksum}
        result.add(Verdict.check(
            "queries return only sealed truth (the injection has no observable effect)",
            "table checksum unchanged by the injected dead-epoch log",
            f"pre={pre_inject_checksum!r} post={post_inject_checksum!r}", unaffected,
            "" if unaffected else "the table's queryable state CHANGED after injecting a log above "
                                  "the seal — it must never be applied (resurrect invariant)"))
        _common.assert_replicas_agree(result, cl, sql.table_checksum_query(_TABLE),
                                      name="S38 replica agreement after injection")

        # =====================================================================================
        # Step 5: a CLEAN restart seals too, and re-recovers from the durable stream with the injected
        # object present. Sealing is arithmetic — every epoch below the live one is closed, however
        # its mount died — so this step is the inverted successor of the old card's "a clean stop/start
        # pays no T_mat wait", which died with the wait itself.
        # =====================================================================================
        ctx.log("S38: graceful `docker restart` ch1 — re-recovers with the injected object in place")
        apply_fault(Fault(t_offset=0, target=FaultTarget.CH1, action=FaultAction.RESTART, duration_s=0))
        healthy_clean = cluster_boot.wait_healthy(cl, timeout_s=heal_timeout_s, log_fn=ctx.log)
        result.add(Verdict.check(
            "ch1 recovers after a clean restart", "healthy within heal_timeout_s",
            f"healthy={healthy_clean}", healthy_clean, ""))
        if not healthy_clean:
            _common.standard_end(ctx, result, [_TABLE])
            return

        rerecovered_checksum = cl.node1.query(sql.table_checksum_query(_TABLE)).strip()
        still_unaffected = rerecovered_checksum == pre_inject_checksum
        clean_sealed = observe.events_snapshot(cl.node1).get("CASRefRecoveryEpochSealed", 0)
        result.observations["clean_restart"] = {
            "healthy": healthy_clean, "CASRefRecoveryEpochSealed": clean_sealed,
            "checksum": rerecovered_checksum}
        result.add(Verdict.check(
            "re-recovery from the durable stream still ignores the injected log",
            "checksum after a full restart equals the pre-injection checksum",
            f"pre={pre_inject_checksum!r} re-recovered={rerecovered_checksum!r}", still_unaffected,
            "" if still_unaffected else "a from-scratch recovery ABSORBED the injected log — the "
                                        "seal did not cover the region above it"))
        result.add(Verdict.check(
            "a CLEAN restart seals its predecessor's epoch too",
            "CASRefRecoveryEpochSealed > 0 on the cleanly-restarted process",
            clean_sealed, clean_sealed > 0,
            "" if clean_sealed > 0 else "the cleanly-restarted process minted no seal — sealing must "
                                        "be decided by arithmetic (every epoch below the live one is "
                                        "closed), not by an unclean-boundary flag"))

        _common.standard_end(ctx, result, [_TABLE])
