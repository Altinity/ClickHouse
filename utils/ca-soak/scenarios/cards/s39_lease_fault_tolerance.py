"""S39: mount-renewal retries and fail-closed recovery against degraded S3.

The short leg spends its allocated campaign window applying isolated, mount-key-only transient
faults. Each disruption is bounded to one physical renewal response, cleared, and verified before
the next begins. The long and post-clear legs consume the remaining requested time while requiring
exactly one lease-loss generation followed by whole-chain remount recovery.

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
_MIN_SHORT_PULSES = 2
_SHORT_RECOVERY_BUDGET_S = _MOUNT_LEASE_TTL_S - _MOUNT_RENEW_PERIOD_S
_SHORT_MUTATION_BUDGET_S = 60
_SHORT_CONTROL_SLACK_S = 10
_SHORT_PULSE_BUDGET_S = (
    _MOUNT_RENEW_PERIOD_S
    + _SHORT_RECOVERY_BUDGET_S
    + _SHORT_MUTATION_BUDGET_S
    + _SHORT_CONTROL_SLACK_S
)
_REMOUNT_EXTRA_BUDGET_S = 90
_POST_CLEAR_WRITE_BUDGET_S = 90
_FINAL_CHECKS_CLEANUP_BUDGET_S = 60
_ELAPSED_TOLERANCE_S = 15
_POST_CLEAR_PROBE_INTERVAL_S = 15

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


def _duration_budget(requested_s, configured_long_fault_s, settle_s):
    long_fault_s = max(
        int(configured_long_fault_s),
        _MOUNT_LEASE_TTL_S + _MOUNT_RENEW_PERIOD_S,
    )
    remount_recovery_budget_s = int(settle_s) + _REMOUNT_EXTRA_BUDGET_S
    mandatory_short_budget_s = _MIN_SHORT_PULSES * _SHORT_PULSE_BUDGET_S
    long_and_final_reserve_s = (
        long_fault_s
        + remount_recovery_budget_s
        + _POST_CLEAR_WRITE_BUDGET_S
        + _FINAL_CHECKS_CLEANUP_BUDGET_S
    )
    minimum_duration_s = mandatory_short_budget_s + long_and_final_reserve_s
    requested_s = int(requested_s)
    if requested_s < minimum_duration_s:
        raise ValueError(
            f"S39 requires at least {minimum_duration_s}s; requested {requested_s}s. "
            "The minimum covers two complete short pulses, the past-TTL long fault, bounded "
            "remount/post-clear write recovery, and final fsck/cleanup."
        )
    short_window_s = requested_s - long_and_final_reserve_s
    return {
        "requested_s": requested_s,
        "acceptable_min_elapsed_s": requested_s - _ELAPSED_TOLERANCE_S,
        "long_fault_s": long_fault_s,
        "short_pulse_budget_s": _SHORT_PULSE_BUDGET_S,
        "mandatory_short_pulses": _MIN_SHORT_PULSES,
        "mandatory_short_budget_s": mandatory_short_budget_s,
        "remount_recovery_budget_s": remount_recovery_budget_s,
        "post_clear_write_budget_s": _POST_CLEAR_WRITE_BUDGET_S,
        "final_checks_cleanup_budget_s": _FINAL_CHECKS_CLEANUP_BUDGET_S,
        "long_and_final_reserve_s": long_and_final_reserve_s,
        "minimum_duration_s": minimum_duration_s,
        "short_window_s": short_window_s,
        "additional_short_window_s": short_window_s - mandatory_short_budget_s,
    }


def _complete_short_pulse_fits(now, deadline):
    return now + _SHORT_PULSE_BUDGET_S <= deadline


def _failure_note(ok, failure):
    return "" if ok else failure


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
        budget = _duration_budget(ctx.duration_s, p["long_fault_s"], p["settle_s"])
        scenario_started = time.monotonic() - ctx.elapsed_s()
        requested_deadline = scenario_started + budget["requested_s"]
        short_deadline = requested_deadline - budget["long_and_final_reserve_s"]

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
        result.observations["duration_budget"] = dict(budget)
        result.observations["fault_window_arithmetic"] = {
            "lease_ttl_s": _MOUNT_LEASE_TTL_S,
            "renew_period_s": _MOUNT_RENEW_PERIOD_S,
            "minimum_short_pulses": _MIN_SHORT_PULSES,
            "faults_per_short_pulse": 1,
            "short_recovery_budget_s": _SHORT_RECOVERY_BUDGET_S,
            "short_formula": "lease_ttl_s - renew_period_s",
            "long_minimum_s": _MOUNT_LEASE_TTL_S + _MOUNT_RENEW_PERIOD_S,
            "long_formula": "lease_ttl_s + renew_period_s",
            "elapsed_tolerance_s": _ELAPSED_TOLERANCE_S,
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
        # has the remaining TTL-minus-period budget. A conservative complete-pulse budget also
        # includes the synchronous mutation and control-plane slack; no new pulse starts unless all
        # of that work fits before the long/final reserve.
        short_since = node.scalar("SELECT toString(now64(6))")
        short_before = _profile_events(node)
        mutation_errors = []
        observed_non_live = False
        pulse_evidence = []
        targeted_faults = 0
        short_started = time.monotonic()
        while (
            len(pulse_evidence) < _MIN_SHORT_PULSES
            or _complete_short_pulse_fits(time.monotonic(), short_deadline)
        ):
            pulse = len(pulse_evidence)
            if not _complete_short_pulse_fits(time.monotonic(), short_deadline):
                raise RuntimeError(
                    "S39 duration budget was exhausted before the two mandatory short pulses; "
                    f"requested={budget['requested_s']}s elapsed={ctx.elapsed_s():.1f}s"
                )

            _ctl("/config", {"reset": True})
            recovered_before = _profile_events(node)["CASMountRenewalRecovered"]
            mount_before = _mount_lifecycle(node)
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

            def pulse_recovered():
                nonlocal observed_non_live
                lifecycle, state, sequence = _mount_lifecycle(node)
                observed_non_live = observed_non_live or lifecycle != "live"
                events = _profile_events(node)
                stats = _ctl("/stats")
                if (
                    stats["faults"] == 1
                    and events["CASMountRenewalRecovered"] > recovered_before
                    and sequence > mount_before[2]
                    and lifecycle == "live"
                ):
                    return {
                        "pulse": pulse,
                        "faults": stats["faults"],
                        "sequence": sequence,
                        "lifecycle": lifecycle,
                        "state": state,
                    }
                return None

            try:
                evidence = _wait_for(
                    pulse_recovered,
                    _MOUNT_RENEW_PERIOD_S + _SHORT_RECOVERY_BUDGET_S,
                )
            finally:
                _ctl("/config", {"rate": 0.0})

            stats = _ctl("/stats")
            targeted_faults += int(stats["faults"])
            mutation_ok = False
            mutation_error = ""
            if evidence is not None and stats["faults"] == 1:
                suffix = f"|s39-short-{pulse}|"
                try:
                    before_length = int(
                        node.scalar(f"SELECT length(payload) FROM {_TABLE} WHERE id = 0")
                    )
                    node.command(
                        f"ALTER TABLE {_TABLE} UPDATE payload = concat(payload, '{suffix}') "
                        "WHERE id = 0 SETTINGS mutations_sync = 2",
                        timeout=_SHORT_MUTATION_BUDGET_S,
                    )
                    after_length = int(
                        node.scalar(f"SELECT length(payload) FROM {_TABLE} WHERE id = 0")
                    )
                    mutation_ok = after_length == before_length + len(suffix)
                    if not mutation_ok:
                        mutation_error = (
                            f"pulse {pulse}: payload length {before_length} -> {after_length}, "
                            f"expected +{len(suffix)}"
                        )
                except Exception as error:
                    mutation_error = f"pulse {pulse}: {error}"
            else:
                mutation_error = f"pulse {pulse}: renewal recovery evidence was unavailable"
            if mutation_error:
                mutation_errors.append(mutation_error)

            final_mount = _mount_lifecycle(node)
            observed_non_live = observed_non_live or final_mount[0] != "live"
            pulse_ok = (
                evidence is not None
                and stats["faults"] == 1
                and stats["by_mode"].get("503") == 1
                and final_mount[0] == "live"
                and final_mount[1] == "live"
                and mutation_ok
            )
            pulse_evidence.append(
                {
                    **(evidence or {"pulse": pulse}),
                    "stats": stats,
                    "final_mount": final_mount,
                    "mutation_ok": mutation_ok,
                    "ok": pulse_ok,
                }
            )
            if not pulse_ok:
                break

        # The residual is necessarily smaller than one conservative complete pulse. Keep checking
        # liveness through it instead of silently idling, then start the single long fault leg.
        residual_started = time.monotonic()
        if all(pulse["ok"] for pulse in pulse_evidence):
            while time.monotonic() < short_deadline:
                lifecycle = _mount_lifecycle(node)
                observed_non_live = observed_non_live or lifecycle[0] != "live"
                time.sleep(min(0.25, max(0.0, short_deadline - time.monotonic())))
        residual_wait_s = max(0.0, time.monotonic() - residual_started)

        short_after = _profile_events(node)
        short_delta = _event_delta(short_before, short_after)
        short_outcomes = _renewal_outcomes(node, short_since)
        short_lifecycle = _mount_lifecycle(node)
        pulse_count = len(pulse_evidence)
        result.observations["leg_a"] = {
            "counter_delta": short_delta,
            "aggregate_outcomes": short_outcomes,
            "targeted_proxy_faults": targeted_faults,
            "pulse_evidence": pulse_evidence,
            "mutation_errors": mutation_errors,
            "final_mount": short_lifecycle,
            "observed_non_live": observed_non_live,
            "short_leg_elapsed_s": round(time.monotonic() - short_started, 3),
            "residual_liveness_wait_s": round(residual_wait_s, 3),
        }
        exercised_ok = (
            pulse_count >= _MIN_SHORT_PULSES
            and targeted_faults == pulse_count
            and all(pulse["ok"] for pulse in pulse_evidence)
        )
        result.add(
            Verdict.check(
                "leg A: repeated targeted renewal faults were exercised",
                f">= {_MIN_SHORT_PULSES} exact targeted faults and recovered pulses",
                f"faults={targeted_faults} pulse_count={pulse_count}",
                exercised_ok,
                _failure_note(
                    exercised_ok,
                    "a finite mount-key fault was not consumed, cleared, mutated, and recovered "
                    "within its complete-pulse budget",
                ),
            )
        )
        counters_ok = (
            short_delta["CASMountRenewalAttempts"] > pulse_count
            and short_delta["CASMountRenewalRetries"] >= pulse_count
            and short_delta["CASMountRenewalRecovered"] >= pulse_count
            and short_outcomes.get("retrying", 0) >= pulse_count
            and short_outcomes.get("recovered", 0) >= pulse_count
        )
        result.add(
            Verdict.check(
                "leg A: Task 6 retry/recovery counters and aggregate events are positive",
                "attempts > pulses, retries/recovered/retrying events >= pulses",
                f"delta={short_delta} outcomes={short_outcomes}",
                counters_ok,
                _failure_note(
                    counters_ok,
                    "short faults did not drive the bounded retry/recovery path",
                ),
            )
        )
        stayed_live_ok = (
            short_delta["CASMountLeaseLost"] == 0
            and short_delta["CASMountRenewalDeadlineExceeded"] == 0
            and short_delta["CASRemountAttempts"] == 0
            and short_delta["CASRemountSucceeded"] == 0
            and short_delta["CASRemountFailed"] == 0
            and not observed_non_live
            and short_lifecycle[0] == "live"
            and short_lifecycle[1] == "live"
        )
        result.add(
            Verdict.check(
                "leg A: no TransientNotLive generation or remount occurred",
                "deadline/loss/remount counters=0 and mount stayed live",
                f"delta={short_delta} observed_non_live={observed_non_live} mount={short_lifecycle}",
                stayed_live_ok,
                _failure_note(
                    stayed_live_ok,
                    "a sub-TTL renewal disruption unexpectedly left Live",
                ),
            )
        )
        mutations_ok = not mutation_errors and pulse_count >= _MIN_SHORT_PULSES
        result.add(
            Verdict.check(
                "leg A: mutations continue while renewal faults recover",
                "one verified synchronous mutation per recovered pulse",
                f"errors={mutation_errors}",
                mutations_ok,
                _failure_note(
                    mutations_ok,
                    "a mutation failed after a targeted mount-renewal disruption recovered",
                ),
            )
        )
        residual_ok = residual_wait_s < _SHORT_PULSE_BUDGET_S + 1.0
        result.add(
            Verdict.check(
                "leg A: scheduler leaves only a sub-pulse residual",
                f"residual liveness wait < {_SHORT_PULSE_BUDGET_S + 1:.0f}s",
                f"residual_wait_s={residual_wait_s:.3f}",
                residual_ok,
                _failure_note(
                    residual_ok,
                    "the short scheduler stopped while another conservative complete pulse fit",
                ),
            )
        )

        # A continuous mount-key outage held past TTL must fail closed exactly once. All unrelated S3
        # paths pass through, isolating the lease-loss/remount operational contract.
        _ctl("/config", {"reset": True})
        long_since = node.scalar("SELECT toString(now64(6))")
        long_before = _profile_events(node)
        long_s = budget["long_fault_s"]
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

        recovered_state = _wait_for(remounted, budget["remount_recovery_budget_s"])

        write_recovered = False
        last_error = ""
        write_deadline = time.monotonic() + budget["post_clear_write_budget_s"]
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

        # If the bounded long/recovery work completed before the requested campaign floor, use the
        # reserved post-clear window for continued Live checks and verified mutations. This keeps a
        # 15-minute request a real 15-minute campaign without converting unused recovery allowance
        # into a long idle sleep.
        post_clear_probe_count = 0
        post_clear_probe_errors = []
        post_clear_observed_non_live = False
        next_probe = time.monotonic()
        active_until = requested_deadline - _ELAPSED_TOLERANCE_S
        while write_recovered and time.monotonic() < active_until:
            lifecycle = _mount_lifecycle(node)
            if lifecycle[0] != "live" or lifecycle[1] != "live":
                post_clear_observed_non_live = True
                post_clear_probe_errors.append(
                    f"mount left Live during post-clear soak: {lifecycle}"
                )
                break
            now = time.monotonic()
            if now >= next_probe:
                suffix = f"|s39-post-{post_clear_probe_count}|"
                try:
                    before_length = int(
                        node.scalar(f"SELECT length(payload) FROM {_TABLE} WHERE id = 0")
                    )
                    node.command(
                        f"ALTER TABLE {_TABLE} UPDATE payload = concat(payload, '{suffix}') "
                        "WHERE id = 0 SETTINGS mutations_sync = 2",
                        timeout=_SHORT_MUTATION_BUDGET_S,
                    )
                    after_length = int(
                        node.scalar(f"SELECT length(payload) FROM {_TABLE} WHERE id = 0")
                    )
                    if after_length != before_length + len(suffix):
                        post_clear_probe_errors.append(
                            f"post-clear probe {post_clear_probe_count}: payload length "
                            f"{before_length} -> {after_length}, expected +{len(suffix)}"
                        )
                        break
                    post_clear_probe_count += 1
                except Exception as error:
                    post_clear_probe_errors.append(str(error))
                    break
                next_probe = time.monotonic() + _POST_CLEAR_PROBE_INTERVAL_S
            time.sleep(min(0.5, max(0.0, active_until - time.monotonic())))

        long_after = _profile_events(node)
        final_lifecycle = _mount_lifecycle(node)
        long_delta = _event_delta(long_before, long_after)
        long_outcomes = _renewal_outcomes(node, long_since)
        result.observations["leg_b"] = {
            "counter_delta": long_delta,
            "aggregate_outcomes": long_outcomes,
            "targeted_proxy_faults": long_fault_stats["faults"],
            "saw_not_live": saw_not_live,
            "final_mount": final_lifecycle,
            "post_clear_probe_count": post_clear_probe_count,
            "post_clear_probe_errors": post_clear_probe_errors,
            "post_clear_observed_non_live": post_clear_observed_non_live,
        }
        loss_ok = (
            long_delta["CASMountLeaseLost"] == 1
            and long_outcomes.get("failed", 0) == 1
            and long_fault_stats["faults"] > 0
            and saw_not_live
        )
        result.add(
            Verdict.check(
                "leg B: sustained disruption fails closed in one recovery generation",
                "lease_lost=1, failed aggregate event=1, targeted faults>0",
                f"delta={long_delta} outcomes={long_outcomes} faults={long_fault_stats['faults']} "
                f"saw_not_live={saw_not_live}",
                loss_ok,
                _failure_note(
                    loss_ok,
                    "the past-TTL renewal outage did not produce exactly one fail-closed "
                    "generation",
                ),
            )
        )
        remount_ok = (
            recovered_state is not None
            and long_delta["CASRemountAttempts"] >= 1
            and long_delta["CASRemountSucceeded"] >= 1
            and final_lifecycle[0] == "live"
            and final_lifecycle[1] == "live"
        )
        result.add(
            Verdict.check(
                "leg B: whole-chain remount restores Live",
                "remount_attempts>=1, remount_succeeded>=1, final lifecycle=live",
                f"delta={long_delta} mount={final_lifecycle}",
                remount_ok,
                _failure_note(
                    remount_ok,
                    "the mount did not recover through the whole-chain remount protocol",
                ),
            )
        )
        post_clear_ok = (
            write_recovered
            and not post_clear_probe_errors
            and not post_clear_observed_non_live
            and final_lifecycle[0] == "live"
        )
        result.add(
            Verdict.check(
                "leg B: post-clear writes and liveness remain available",
                "initial write lands within its budget and active probes stay Live",
                f"recovered={write_recovered} probes={post_clear_probe_count} "
                f"errors={post_clear_probe_errors}",
                post_clear_ok,
                _failure_note(
                    post_clear_ok,
                    f"post-remount write/liveness remained unavailable: "
                    f"{last_error[:200]} {post_clear_probe_errors[:2]}",
                ),
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

        actual_elapsed_s = ctx.elapsed_s()
        result.observations["duration_budget"].update(
            {
                "actual_elapsed_s": round(actual_elapsed_s, 3),
                "actual_short_pulses": pulse_count,
                "short_deadline_elapsed_s": budget["short_window_s"],
            }
        )
        elapsed_ok = actual_elapsed_s >= budget["acceptable_min_elapsed_s"]
        result.add(
            Verdict.check(
                "requested campaign duration was consumed",
                f"elapsed >= {budget['acceptable_min_elapsed_s']}s "
                f"(requested {budget['requested_s']}s minus {_ELAPSED_TOLERANCE_S}s tolerance)",
                f"elapsed={actual_elapsed_s:.3f}s pulses={pulse_count}",
                elapsed_ok,
                _failure_note(
                    elapsed_ok,
                    "S39 terminated materially before the requested campaign duration",
                ),
            )
        )
