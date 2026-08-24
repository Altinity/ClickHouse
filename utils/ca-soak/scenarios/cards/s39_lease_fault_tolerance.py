"""S39: mount-renewal retries and fail-closed recovery against degraded S3.

The short leg applies two exact, mount-key-only transient faults. Each disruption is bounded to one
physical renewal response, so data-plane mutations continue while the renewal controller retries
inside the confirmed lease. The long leg faults that same key continuously for more than one lease
TTL and requires exactly one lease-loss generation followed by whole-chain remount recovery.

The compiled lease TTL and renewal period are currently not XML settings. All time budgets below are
derived from their 30s/10s defaults and are emitted in the scenario observations.
"""

import json as _json
import time
import urllib.request

from ..framework import sql
from ..framework.base import Scenario, register
from ..framework.report import Verdict
from . import _common

_TABLE = "s39_lease"
_DISK = "ca"
_SERVER_ROOT_ID = "ca_soak_ch1"
_MOUNT_PATH = f"/test/soak_pool/gc/server-roots/{_SERVER_ROOT_ID}/mount"
_CTL = "http://localhost:8474"

_MOUNT_LEASE_TTL_S = 30
_MOUNT_RENEW_PERIOD_S = 10
_SHORT_PULSES = 2
_SHORT_RECOVERY_BUDGET_S = _MOUNT_LEASE_TTL_S - _MOUNT_RENEW_PERIOD_S

_EVENTS = (
    "CASMountRenewalAttempts",
    "CASMountRenewalRetries",
    "CASMountRenewalResolved",
    "CASMountRenewalRecovered",
    "CASMountRenewalDeadlineExceeded",
    "CASMountLeaseLost",
    "CASRemountAttempts",
    "CASRemountSucceeded",
    "CASRemountFailed",
)


def _ctl(path, body=None, timeout=10):
    """POST/GET against the request-preserving fault proxy control port."""
    url = f"{_CTL}{path}"
    if body is None:
        return _json.loads(urllib.request.urlopen(url, timeout=timeout).read().decode())
    request = urllib.request.Request(
        url,
        data=_json.dumps(body).encode(),
        headers={"Content-Type": "application/json"},
        method="POST",
    )
    return _json.loads(urllib.request.urlopen(request, timeout=timeout).read().decode())


def _profile_events(node):
    rows = node.query(
        "SELECT event, value FROM system.events WHERE event IN ({}) FORMAT TabSeparated".format(
            ", ".join("'{}'".format(event) for event in _EVENTS)
        )
    )
    values = {event: 0 for event in _EVENTS}
    for row in rows.splitlines():
        event, value = row.split("\t")
        values[event] = int(value)
    return values


def _event_delta(before, after):
    return {event: after[event] - before[event] for event in _EVENTS}


def _mount_lifecycle(node):
    row = node.query(
        f"SELECT lifecycle, state, renewal_sequence FROM system.cas_mounts "
        f"WHERE disk = '{_DISK}' AND server_root_id = '{_SERVER_ROOT_ID}' "
        "LIMIT 1 FORMAT TabSeparated"
    ).strip()
    if not row:
        return "missing", "missing", 0
    lifecycle, state, sequence = row.split("\t")
    return lifecycle, state, int(sequence)


def _renewal_outcomes(node, since):
    try:
        node.command("SYSTEM FLUSH LOGS")
        rows = node.query(
            "SELECT outcome, count() FROM system.cas_log "
            "WHERE event_type = 'watermark_renew' "
            f"AND disk_name = '{_DISK}' "
            f"AND detail['server_root_id'] = '{_SERVER_ROOT_ID}' "
            f"AND event_time_microseconds >= toDateTime64('{since}', 6) "
            "GROUP BY outcome FORMAT TabSeparated"
        )
        return {
            outcome: int(count)
            for outcome, count in (row.split("\t") for row in rows.splitlines())
        }
    except Exception:
        return {}


def _wait_for(probe, timeout):
    deadline = time.monotonic() + timeout
    last = None
    while time.monotonic() < deadline:
        try:
            last = probe()
        except Exception as error:
            last = error
            time.sleep(0.25)
            continue
        if last:
            return last
        time.sleep(0.25)
    return None


@register
class S39(Scenario):
    name = "S39"
    title = "mount-renewal retries and fail-closed recovery under degraded S3"
    priority = "P1"
    compose_variant = "s3faultproxy"
    param_table = {
        "dev": {"long_fault_s": 40, "settle_s": 20, "rows": 2000, "payload_bytes": 512},
        "ci": {"long_fault_s": 50, "settle_s": 40, "rows": 20000, "payload_bytes": 1024},
        "full": {"long_fault_s": 60, "settle_s": 60, "rows": 100000, "payload_bytes": 2048},
    }

    def run(self, ctx, result):
        cl = ctx.cluster
        node = cl.nodes()[0]
        p = ctx.params
        rows = int(p["rows"])
        payload = int(p["payload_bytes"])

        try:
            health = _ctl("/healthz")
        except Exception as error:
            result.add(
                Verdict.inconclusive(
                    "fault proxy reachable", "control :8474 up", f"unreachable: {error}"
                )
            )
            return
        _ctl("/config", {"reset": True})
        result.observations["proxy"] = {"healthz": health}
        result.observations["fault_window_arithmetic"] = {
            "lease_ttl_s": _MOUNT_LEASE_TTL_S,
            "renew_period_s": _MOUNT_RENEW_PERIOD_S,
            "short_pulses": _SHORT_PULSES,
            "faults_per_short_pulse": 1,
            "short_recovery_budget_s": _SHORT_RECOVERY_BUDGET_S,
            "short_formula": "lease_ttl_s - renew_period_s",
            "long_minimum_s": _MOUNT_LEASE_TTL_S + _MOUNT_RENEW_PERIOD_S,
            "long_formula": "lease_ttl_s + renew_period_s",
        }

        for cluster_node in cl.nodes():
            sql.create_ca_table(
                cluster_node,
                _TABLE,
                columns="id UInt64, payload String",
                order_by="id",
                wide=True,
            )
        sql.insert_random(node, _TABLE, rows=rows // 4, payload_bytes=payload, op_id=0)

        # A finite one-fault pulse can wait at most one renewal period to be consumed. Its retry then
        # has the remaining TTL-minus-period budget, and the next pulse starts only after recovery.
        short_since = node.scalar("SELECT toString(now64(6))")
        short_before = _profile_events(node)
        mutation_errors = []
        observed_non_live = False
        pulse_evidence = []
        for pulse in range(_SHORT_PULSES):
            recovered_before = _profile_events(node)["CASMountRenewalRecovered"]
            _ctl(
                "/config",
                {
                    "rate": 1.0,
                    "modes": ["503"],
                    "methods": ["PUT"],
                    "path_substring": _MOUNT_PATH,
                    "remaining_faults": 1,
                    "seed": 3900 + pulse,
                },
            )
            try:
                node.command(
                    f"ALTER TABLE {_TABLE} UPDATE payload = concat(payload, '{pulse}') "
                    f"WHERE id % {_SHORT_PULSES} = {pulse} SETTINGS mutations_sync = 2",
                    timeout=60,
                )
            except Exception as error:
                mutation_errors.append(str(error))

            def pulse_recovered():
                nonlocal observed_non_live
                lifecycle, state, sequence = _mount_lifecycle(node)
                observed_non_live = observed_non_live or lifecycle != "live"
                events = _profile_events(node)
                stats = _ctl("/stats")
                if (
                    stats["faults"] >= pulse + 1
                    and events["CASMountRenewalRecovered"] > recovered_before
                ):
                    return {
                        "pulse": pulse,
                        "faults": stats["faults"],
                        "sequence": sequence,
                        "lifecycle": lifecycle,
                        "state": state,
                    }
                return None

            evidence = _wait_for(
                pulse_recovered,
                _MOUNT_RENEW_PERIOD_S + _SHORT_RECOVERY_BUDGET_S,
            )
            pulse_evidence.append(evidence)
            _ctl("/config", {"rate": 0.0})

        short_after = _profile_events(node)
        short_delta = _event_delta(short_before, short_after)
        short_outcomes = _renewal_outcomes(node, short_since)
        short_stats = _ctl("/stats")
        short_lifecycle = _mount_lifecycle(node)
        result.observations["leg_a"] = {
            "counter_delta": short_delta,
            "aggregate_outcomes": short_outcomes,
            "targeted_proxy_faults": short_stats["faults"],
            "pulse_evidence": pulse_evidence,
            "mutation_errors": mutation_errors,
            "final_mount": short_lifecycle,
            "observed_non_live": observed_non_live,
        }
        result.add(
            Verdict.check(
                "leg A: repeated targeted renewal faults were exercised",
                f"{_SHORT_PULSES} targeted faults and {_SHORT_PULSES} recovered pulses",
                f"faults={short_stats['faults']} pulses={pulse_evidence}",
                short_stats["faults"] == _SHORT_PULSES and all(pulse_evidence),
                "a finite mount-key fault was not consumed and recovered within its sub-TTL budget",
            )
        )
        result.add(
            Verdict.check(
                "leg A: Task 6 retry/recovery counters and aggregate events are positive",
                "attempts > pulses, retries/recovered/retrying events >= pulses",
                f"delta={short_delta} outcomes={short_outcomes}",
                short_delta["CASMountRenewalAttempts"] > _SHORT_PULSES
                and short_delta["CASMountRenewalRetries"] >= _SHORT_PULSES
                and short_delta["CASMountRenewalRecovered"] >= _SHORT_PULSES
                and short_outcomes.get("retrying", 0) >= _SHORT_PULSES
                and short_outcomes.get("recovered", 0) >= _SHORT_PULSES,
                "short faults did not drive the bounded retry/recovery path",
            )
        )
        result.add(
            Verdict.check(
                "leg A: no TransientNotLive generation or remount occurred",
                "lease_lost=0, remount_attempts=0, mount stayed live",
                f"delta={short_delta} observed_non_live={observed_non_live} mount={short_lifecycle}",
                short_delta["CASMountLeaseLost"] == 0
                and short_delta["CASRemountAttempts"] == 0
                and not observed_non_live
                and short_lifecycle[0] == "live",
                "a sub-TTL renewal disruption unexpectedly left Live",
            )
        )
        result.add(
            Verdict.check(
                "leg A: mutations continue while renewal faults recover",
                f"{_SHORT_PULSES} synchronous mutations succeed",
                f"errors={mutation_errors}",
                not mutation_errors,
                "a mutation failed during a targeted mount-renewal disruption",
            )
        )

        # A continuous mount-key outage held past TTL must fail closed exactly once. All unrelated S3
        # paths pass through, isolating the lease-loss/remount operational contract.
        _ctl("/config", {"reset": True})
        long_since = node.scalar("SELECT toString(now64(6))")
        long_before = _profile_events(node)
        long_s = max(int(p["long_fault_s"]), _MOUNT_LEASE_TTL_S + _MOUNT_RENEW_PERIOD_S)
        result.observations["fault_window_arithmetic"]["long_actual_s"] = long_s
        _ctl(
            "/config",
            {
                "rate": 1.0,
                "modes": ["503"],
                "methods": ["PUT"],
                "path_substring": _MOUNT_PATH,
                "remaining_faults": None,
                "seed": 3940,
            },
        )
        long_deadline = time.monotonic() + long_s
        saw_not_live = False
        while time.monotonic() < long_deadline:
            try:
                saw_not_live = saw_not_live or _mount_lifecycle(node)[0] == "not_live"
            except Exception:
                pass
            time.sleep(0.5)
        _ctl("/config", {"rate": 0.0})
        long_fault_stats = _ctl("/stats")

        def remounted():
            events = _profile_events(node)
            lifecycle = _mount_lifecycle(node)
            if (
                events["CASRemountSucceeded"] > long_before["CASRemountSucceeded"]
                and lifecycle[0] == "live"
            ):
                return events, lifecycle
            return None

        recovered_state = _wait_for(remounted, int(p["settle_s"]) + 90)
        long_after = recovered_state[0] if recovered_state else _profile_events(node)
        final_lifecycle = recovered_state[1] if recovered_state else _mount_lifecycle(node)
        long_delta = _event_delta(long_before, long_after)
        long_outcomes = _renewal_outcomes(node, long_since)
        result.observations["leg_b"] = {
            "counter_delta": long_delta,
            "aggregate_outcomes": long_outcomes,
            "targeted_proxy_faults": long_fault_stats["faults"],
            "saw_not_live": saw_not_live,
            "final_mount": final_lifecycle,
        }
        result.add(
            Verdict.check(
                "leg B: sustained disruption fails closed in one recovery generation",
                "lease_lost=1, failed aggregate event>0, targeted faults>0",
                f"delta={long_delta} outcomes={long_outcomes} faults={long_fault_stats['faults']} "
                f"saw_not_live={saw_not_live}",
                long_delta["CASMountLeaseLost"] == 1
                and long_outcomes.get("failed", 0) > 0
                and long_fault_stats["faults"] > 0
                and saw_not_live,
                "the past-TTL renewal outage did not produce exactly one fail-closed generation",
            )
        )
        result.add(
            Verdict.check(
                "leg B: whole-chain remount restores Live",
                "remount_attempts>=1, remount_succeeded>=1, final lifecycle=live",
                f"delta={long_delta} mount={final_lifecycle}",
                long_delta["CASRemountAttempts"] >= 1
                and long_delta["CASRemountSucceeded"] >= 1
                and final_lifecycle[0] == "live",
                "the mount did not recover through the whole-chain remount protocol",
            )
        )

        write_recovered = False
        last_error = ""
        write_deadline = time.monotonic() + 90
        while time.monotonic() < write_deadline:
            try:
                sql.insert_random(
                    node,
                    _TABLE,
                    rows=rows // 4,
                    payload_bytes=payload,
                    op_id=4 * rows,
                    timeout=30,
                )
                write_recovered = True
                break
            except Exception as error:
                last_error = str(error)
                time.sleep(3)
        result.add(
            Verdict.check(
                "leg B: a post-clear write succeeds",
                "write lands within 90s",
                f"recovered={write_recovered}",
                write_recovered,
                f"post-remount write remained unavailable: {last_error[:200]}",
            )
        )

        for cluster_node in cl.nodes():
            try:
                cluster_node.command(f"SYSTEM SYNC REPLICA {_TABLE}", timeout=120)
            except Exception as error:
                ctx.log(f"S39 SYNC REPLICA before agreement check (best-effort): {error}")
        _common.assert_replicas_agree(
            result,
            cl,
            sql.table_checksum_query(_TABLE),
            name="S39 replica agreement",
        )
        _common.standard_end(ctx, result, [_TABLE])
