"""CA soak harness — CAS correctness/availability SIGNALS the driver must actually read.

Until 2026-07-26 `grep -n "ProfileEvents" soak/*.py` returned NOTHING: the soak driver read no
counter at all, so every signal the product grew was invisible to it — a green soak proved only that
nobody looked. This module is the reader.

Two families:

* **`system.events` counters** (`CAS_SIGNAL_EVENTS`) — the `Cas*` `ProfileEvents` that carry a
  correctness or availability verdict. Read per metrics tick and, fail-closed, at every checkpoint.
* **The per-phase GC log** (`system.content_addressed_garbage_collection_log`, `event_type='Phase'`,
  landed 2026-07-26 in `d412f85f749`) — eighteen rows per folding round carrying `phase_duration_us`,
  a phase-specific `phase_metrics` map and that phase's `ProfileEvents` delta. Summarised per
  checkpoint into the metrics sqlite so the load study has material that survives the run.

ERROR DISCIPLINE — the point of the exercise, copied from what `scenarios/framework/observe.py` was
fixed to do on 2026-07-25 (BACKLOG `{#gc-observation-vacuous-2026-07-25}`): a node that is DOWN during
a chaos window is legitimately unreadable and folds into a sentinel (`None`), because "no observation"
is the honest answer. A query that the server REJECTS — `UNKNOWN_IDENTIFIER` on a column the schema no
longer has, a syntax error, anything else — is a HARNESS BUG and must surface as an exception. It must
never degrade to zero or to an empty result: reproducing the silent-degradation defect inside the code
written to detect it is the one outcome this module exists to avoid.

FAIL-CLOSED ON AN UNKNOWN COUNTER. `system.events` normally omits a counter that has never been
incremented, which makes "the binary does not have this counter" and "this counter is zero"
indistinguishable — so a renamed or not-yet-existing event would read as a permanent, quiet zero. Every
read here therefore sets `system_events_show_zero_values = 1`, which makes `system.events` enumerate
the binary's WHOLE event registry (`StorageSystemEvents::fillData`), and a requested name still missing
from the answer raises `SignalsUnsupported`. `preflight_signals` runs exactly that read once at
startup, so a typo or a stale name kills the run at second zero instead of producing four blind hours.
"""

import json

from soak.cluster import QueryError, is_node_down

# ---------------------------------------------------------------------------
# The watched counters
# ---------------------------------------------------------------------------

# Each entry is (event, why the soak watches it). The tuple is the contract with the product: when a
# counter here is renamed or removed, `preflight_signals` fails the run rather than silently reading 0.
CAS_SIGNAL_EVENT_NOTES = (
    ("CasGcUnmatchedRemoveDeltas",
     "GC in-degree removal deltas that matched no source edge. Per-key no-op by design, so it cannot "
     "cause a false delete — but a persistent rate means removals reach the reducer without their "
     "activation, which is the retention-leak signal that stayed silent for months (2026-07-25)."),
    ("CasRefAppendPreAttemptRefused",
     "Ref-log append chunks refused BEFORE any request was sent (mount fence / deadline, zero attempts "
     "made, nothing can be durable). EXPECTED to be nonzero under chaos — this is the availability fix "
     "working, and it is counted apart from CasRefAppendWedged so a falling wedge count cannot be "
     "misread as nothing happening."),
    ("CasGcProbeAHolePresent",
     "Probe A holes CONFIRMED to still exist by a HEAD taken at the moment of the disagreement. This is "
     "the direct observation that a ref-prefix enumeration OMITTED a durable object — the "
     "LIST-as-journal release blocker, seen rather than inferred. Cannot be reconstructed afterwards: by "
     "the time a log is read the object has legitimately been deleted either way."),
    ("CasGcProbeAHoleAbsent",
     "Probe A holes that were ABSENT when HEADed at firing time. The OPPOSITE defect to "
     "CasGcProbeAHolePresent and it indicts a different component: the other enumeration returned a key "
     "that is not there, which points at the listing client or iterator rather than at store "
     "completeness. Watched separately precisely so the two are never summed into one number."),
    ("CasRefAppendWedged",
     "Ref-log append lanes that exhausted retries after an UNCERTAIN put. Ref-log progress may be "
     "stalled on that lane."),
    ("CasGcProbeADue",
     "GC rounds on which the sampled ref-prefix store-quality detector was DUE by its deterministic "
     "cadence (`gc_probe_a_period`). Watched together with CasGcProbeAPerformed because a "
     "disagreement count can only be read against how many rounds were actually sampled."),
    ("CasGcProbeAPerformed",
     "GC rounds on which that detector actually compared two independent enumerations, at one extra "
     "full ref-prefix LIST each. Zero here with CasGcProbeADue nonzero means every sample failed."),
    ("CasGcClampSuppressedPasses",
     "GC passes that deferred graduation and deletion because reachability was uncertain — GC held "
     "back. Fail-closed behaviour, but a steady rate means GC is not converging."),
    ("CasGcCondemnMarkerUnconfirmedCarry",
     "Retirements delayed because a durable condemn marker could not be confirmed. Safe (deletion is "
     "postponed) but points at marker write/read failures."),
    ("CasGcRefScanDisagreements",
     "Ref-log ids on which a round's two independent enumerations of the ref prefix disagreed below "
     "the other enumeration's own maximum id. An append cannot produce that shape: it means the store "
     "answered inconsistently about a durable prefix, or a deposed leader deleted ref objects. A "
     "STORE-QUALITY READING ONLY since the sampled detector landed: the round's ref intake reads by "
     "exact key, so a nonzero value no longer aborts the round or suppresses any step. It can only "
     "move on a round the detector sampled — read it against CasGcProbeAPerformed."),
)

# The late-PUT-loses invariant, as counters. Two families, and the difference between them is the
# whole point: the first is what the protocol WORKING looks like, the second is what it FAILING looks
# like, and a soak that only watched the second could not tell "the invariant held" from "the code
# path was never reached".
#
# A dying writer epoch can have a PUT in flight for the id its successor's recovery is about to seal.
# Recovery closes the dead epoch by writing an EpochSeal at that exact id as a CONDITIONAL CREATE, so
# the two possible orders both end well: the seal lands first and the straggler's create is refused
# (`CasRefAppendSealRejected` — the late PUT LOSES, and it was never acknowledged), or the straggler
# lands first and recovery adopts it and reseals at the new T+1 (`CasRefRecoveryStragglerAdopted` —
# the write was real, so it is kept). What must NEVER happen is a late transaction materializing
# BELOW coverage the successor has already declared: that is a durable write the fold has walked past.
LATE_PUT_EVIDENCE_NOTES = (
    ("CasRefRecoveryEpochSealed",
     "Epoch seals MINTED by a recovery CAS-walk, one per dead writer epoch closed. This is the "
     "fencing mechanism itself: zero over a whole chaos run means no epoch ever changed hands under "
     "recovery, so the run did not exercise the invariant at all and its silence proves nothing."),
    ("CasRefAppendSealRejected",
     "Ref-log transactions conclusively rejected by a successor's epoch seal occupying the id they "
     "derived. THE late PUT losing, counted: the deposed writer's operation was never acknowledged. "
     "Expected to be nonzero under chaos and never a failure on its own."),
    ("CasRefRecoveryStragglerAdopted",
     "Stragglers a recovery CAS-walk met at the slot it tried to seal, adopted and resealed at the "
     "new T+1. The other legal order of the same race — the write won the slot fairly, so it is kept "
     "rather than fenced. Also never a failure on its own."),
)

# The violation half. Each one is a counter whose OWN registry description says it must be zero, and
# each is a distinct way the invariant could break: a durable transaction missing from a writer's
# view, a fold cursor advanced past work, a stream that is no longer dense.
LATE_PUT_VIOLATION_NOTES = (
    ("CasRefApplyPoisoned",
     "A ref table whose cached state may be MISSING a durable transaction — an install failed while "
     "its ref-log object may already be durable. The LOSS half of the invariant: acknowledged work "
     "that the writer's own view no longer contains."),
    ("CasGcUnappliedFoldedTxns",
     "Ref transactions a round folded and merged but whose blob deltas never reached a shard reducer. "
     "The FOLD half: the round would advance its cursor past a transaction it never applied. The "
     "product already fails such a round closed; the soak must not finish green having seen one."),
    ("CasRefRecoveryStreamHole",
     "A 404 BELOW a durable same-epoch witness — ids are dense 1..T within (namespace, epoch) by "
     "INV-1, so this is a hole and folding what is above it would drop transactions silently. The "
     "DENSITY half."),
)

CAS_SIGNAL_EVENTS = tuple(
    name for name, _ in CAS_SIGNAL_EVENT_NOTES + LATE_PUT_EVIDENCE_NOTES + LATE_PUT_VIOLATION_NOTES)

LATE_PUT_EVIDENCE_EVENTS = tuple(name for name, _ in LATE_PUT_EVIDENCE_NOTES)
LATE_PUT_VIOLATION_EVENTS = tuple(name for name, _ in LATE_PUT_VIOLATION_NOTES)

# Signals whose benign rate is uncharacterised as of 2026-07-26 and which therefore must NOT gate a
# run yet (Task 21 step 3: "report the counters; do NOT fail on them"). Recorded here so the intent is
# in the code rather than only in a plan: a threshold goes in once several runs agree on a rate.
# The late-PUT families are NOT in it: the evidence counters are expected to move and are never a
# failure, and the violation counters need no characterisation because their benign rate is zero by
# construction, stated in their own `ProfileEvents.cpp` descriptions.
UNCHARACTERISED_SIGNALS = tuple(name for name, _ in CAS_SIGNAL_EVENT_NOTES)

# A name listed in two families would be read twice, validated twice and tracked in one dict key —
# silently halving the accounting rather than failing. Cheap to rule out at import.
if len(set(CAS_SIGNAL_EVENTS)) != len(CAS_SIGNAL_EVENTS):
    _dupes = sorted({e for e in CAS_SIGNAL_EVENTS if CAS_SIGNAL_EVENTS.count(e) > 1})
    raise AssertionError(f"duplicate watched counter(s) across the signal families: {_dupes}")

GC_LOG = "system.content_addressed_garbage_collection_log"

# ClickHouse error code UNKNOWN_TABLE. `SystemLog<>`-backed tables are materialized lazily
# (`SystemLog::prepareTable`), so a pool on which GC has never logged a round legitimately raises this
# on the first probe. That is "nothing has happened yet", the same class as an empty result.
_UNKNOWN_TABLE_CODE = 60


class SignalsUnsupported(RuntimeError):
    """A watched `system.events` counter is absent from the binary's own event registry (read with
    `system_events_show_zero_values = 1`, which lists every event the binary knows). The counter was
    renamed, removed, or never existed — so the soak would record a permanent silent zero for it. Fail
    CLOSED: a signal that cannot be read is not a signal that is quiet."""


def is_missing_table(exc: BaseException) -> bool:
    """True if `exc` is a `QueryError` carrying UNKNOWN_TABLE (60) — a lazily-materialized system log
    that has never had an entry."""
    if not isinstance(exc, QueryError):
        return False
    body = exc.body or ""
    return f"Code: {_UNKNOWN_TABLE_CODE}." in body or "UNKNOWN_TABLE" in body


def is_benign_probe_gap(exc: BaseException, *, allow_missing_table: bool = False) -> bool:
    """True if `exc` is a LEGITIMATE reason an observation came back with nothing.

    Exactly two cases qualify. The node is unreachable or going down (`is_node_down` — a chaos-killed,
    paused or restarting replica, including the graceful-shutdown `QUERY_WAS_CANCELLED` shape), or —
    only when the caller passes `allow_missing_table` because it is probing a `SystemLog` table — the
    log has not been materialized yet.

    Everything else is the query itself being broken (a dropped column, a syntax error, a server-side
    rejection) and every caller below re-raises it. `system.events` is a built-in table that always
    exists, so it is probed WITHOUT `allow_missing_table`: an UNKNOWN_TABLE there would be a genuine
    anomaly and must surface."""
    if is_node_down(exc):
        return True
    return allow_missing_table and is_missing_table(exc)


# ---------------------------------------------------------------------------
# system.events — pure SQL building / parsing
# ---------------------------------------------------------------------------

def _validate_event_names(events) -> tuple:
    """Reject anything that is not a bare identifier before it is interpolated into SQL. The event
    names are compile-time constants of this module, so this can only ever fire on an editing mistake —
    which is precisely when a quote or a comma inside a name would turn a probe into something else."""
    out = tuple(events)
    if not out:
        raise ValueError("no events requested")
    for e in out:
        if not e or not e.replace("_", "").isalnum():
            raise ValueError(f"event name is not a bare identifier: {e!r}")
    return out


def signal_events_sql(events=CAS_SIGNAL_EVENTS) -> str:
    """SQL reading the watched counters from `system.events`.

    `system_events_show_zero_values = 1` is load-bearing, not a nicety: without it `system.events`
    omits every counter that has never been incremented, so a name the binary does not know and a name
    at zero produce the identical empty answer. With it, the query enumerates the binary's whole event
    registry and filters — so a requested name missing from the RESULT proves the binary has no such
    counter, and `parse_signal_events` can fail closed on it."""
    quoted = ", ".join("'" + e + "'" for e in _validate_event_names(events))
    return ("SELECT event, value FROM system.events "
            f"WHERE event IN ({quoted}) "
            "SETTINGS system_events_show_zero_values = 1 "
            "FORMAT TabSeparated")


def parse_events_tsv(text: str) -> dict:
    """Parse `event\\tvalue` TabSeparated rows into `{event: int}`. A row whose value is not an integer
    is a malformed answer from a query we wrote ourselves — raise rather than drop it silently."""
    out: dict = {}
    for line in (text or "").splitlines():
        if not line.strip():
            continue
        if "\t" not in line:
            raise ValueError(f"malformed system.events row (no tab): {line!r}")
        k, v = line.split("\t", 1)
        out[k] = int(v)
    return out


def parse_signal_events(text: str, events=CAS_SIGNAL_EVENTS) -> dict:
    """Parse the answer to `signal_events_sql` and FAIL CLOSED on a missing name.

    Because the query ran with `system_events_show_zero_values = 1`, every event the binary knows is a
    candidate row; a requested name still absent means the binary has no such counter. Recording it as
    0 would be the silent-blindness failure this module exists to prevent, so raise instead."""
    requested = _validate_event_names(events)
    got = parse_events_tsv(text)
    missing = [e for e in requested if e not in got]
    if missing:
        raise SignalsUnsupported(
            "system.events does not know these counters even with system_events_show_zero_values=1, "
            f"so this binary cannot report them: {missing}. They were renamed, removed, or never "
            "existed. Recording them as zero would make the run blind — fix the names in "
            "soak/signals.py:CAS_SIGNAL_EVENT_NOTES or run a binary that has them.")
    return {e: got[e] for e in requested}


def read_signal_events(node, events=CAS_SIGNAL_EVENTS, *, timeout: float = 30.0):
    """Read the watched counters from one node.

    Returns `{event: value}`, or `None` when the node is legitimately unreadable (down/paused/
    restarting under chaos) — a visible gap, never a faked zero. Raises `SignalsUnsupported` when the
    binary does not know a counter, and re-raises any other query failure: `system.events` is a
    built-in table, so anything else is a harness bug."""
    try:
        txt = node.query(signal_events_sql(events), timeout=timeout)
    except Exception as e:
        if not is_benign_probe_gap(e):
            raise
        return None
    return parse_signal_events(txt, events)


def preflight_signals(cluster, events=CAS_SIGNAL_EVENTS, *, timeout: float = 30.0) -> dict:
    """Prove, once, before the soak starts, that every watched counter EXISTS in the running binary.

    This is the step that converts "the run was green" into "the run was green and the signals were
    readable". It runs the identical query the ticker will run, so a malformed probe cannot survive to
    the run and quietly disable the whole metrics curve either.

    Every node must answer: a node that is down at bring-up is not a chaos window, it is a broken
    stand, so a probe gap here is fatal too. Returns `{node_repr: {event: value}}` — the starting
    values, useful because the counters are cumulative and a container may have been reused."""
    baseline = {}
    for node in cluster.nodes():
        try:
            txt = node.query(signal_events_sql(events), timeout=timeout)
        except Exception as e:
            raise SignalsUnsupported(
                f"preflight: cannot read the CAS signal counters on {node!r}: {type(e).__name__}: {e}. "
                "The soak refuses to start blind.") from e
        baseline[repr(node)] = parse_signal_events(txt, events)
    return baseline


# ---------------------------------------------------------------------------
# The per-phase GC log
# ---------------------------------------------------------------------------

# The four values the 2026-07-25 detector work made observable, addressed as (phase, metric). They are
# emitted UNCONDITIONALLY on every folding round precisely so that "healthy" has a printed value and
# the one round that is not healthy stands out — which only helps if something reads them, hence this.
#   fold_ref_list.probe_a_holes      — ids missing from one of two independent enumerations of the ref
#                                      prefix, below the other's own maximum id. An append cannot make
#                                      that shape.
#   fold_ref_intake.logs_accounted   — ref-log POSITIONS the round's sealed cut declares covered,
#                                      counted arithmetically per epoch entered (renamed from
#                                      `logs_intended` when the fold stopped deriving the cut from the
#                                      LIST) …
#   fold_ref_intake.logs_applied     — … versus logs that reached the single cursor-advance site.
#                                      Inequality means the cursor advanced over unapplied work.
#   fold_reduce.txns_unapplied       — folded+merged transactions whose blob deltas never reached a
#                                      shard reducer. The fail-closed twin of CasGcUnappliedFoldedTxns.
DETECTOR_METRICS = (
    ("fold_ref_list", "probe_a_holes"),
    ("fold_ref_intake", "logs_accounted"),
    ("fold_ref_intake", "logs_applied"),
    ("fold_reduce", "txns_unapplied"),
    # Not a detector value but the verdict the detectors drive: the round refused to fold.
    ("fold_ref_list", "ref_folding_aborted"),
)

# Scalar columns pulled out of `phase_metrics` by name. `map['absent']` is a DEFINED zero for a
# ClickHouse Map, so these columns are exact whatever the aggregate does with absent keys — unlike
# reading them back out of a summed map, whose zero-key behaviour we would be depending on.
_DETECTOR_COLUMNS = ("probe_a_holes", "logs_accounted", "logs_applied", "txns_unapplied",
                     "ref_folding_aborted")


def phase_summary_sql(since_ts: int) -> str:
    """Per-phase aggregate over the GC log rows written since `since_ts` (unix seconds).

    One row per phase, ordered slowest-first, carrying: how many distinct round attempts touched the
    phase (`round_id`, which exists even for a round that never led or never committed — `round` does
    not), the wall-clock the phase spent, its worst single occurrence, the five detector values as
    exact scalars, and the whole summed `phase_metrics` / `ProfileEvents` maps for the load study.

    JSONEachRow rather than TabSeparated because two columns are maps whose values would otherwise
    have to survive TSV escaping."""
    since = int(since_ts)
    detectors = ", ".join(
        f"sum(phase_metrics['{c}']) AS {c}" for c in _DETECTOR_COLUMNS)
    return (
        "SELECT phase, uniqExact(round_id) AS rounds, count() AS calls, "
        "sum(phase_duration_us) AS total_us, max(phase_duration_us) AS max_us, "
        f"{detectors}, "
        "sumMap(CAST(phase_metrics, 'Map(String, UInt64)')) AS metrics, "
        "sumMap(CAST(`ProfileEvents`, 'Map(String, UInt64)')) AS events "
        f"FROM {GC_LOG} "
        f"WHERE event_type = 'Phase' AND event_time >= toDateTime({since}) "
        "GROUP BY phase ORDER BY total_us DESC "
        "FORMAT JSONEachRow")


def _as_int(v) -> int:
    """JSONEachRow quotes 64-bit integers as strings by default
    (`output_format_json_quote_64bit_integers`), so accept both shapes."""
    return int(v)


def _as_int_map(v) -> dict:
    return {str(k): _as_int(x) for k, x in (v or {}).items()}


def parse_phase_summary(text: str) -> list:
    """Parse the JSONEachRow answer to `phase_summary_sql` into normalized dicts.

    A row that cannot be parsed is a mismatch between the query we wrote and the answer we got —
    raise; do not drop it. Rows come back slowest-first and that order is preserved."""
    rows = []
    for line in (text or "").splitlines():
        if not line.strip():
            continue
        r = json.loads(line)
        row = {
            "phase": r["phase"],
            "rounds": _as_int(r["rounds"]),
            "calls": _as_int(r["calls"]),
            "total_us": _as_int(r["total_us"]),
            "max_us": _as_int(r["max_us"]),
            "metrics": _as_int_map(r.get("metrics")),
            "events": _as_int_map(r.get("events")),
        }
        for c in _DETECTOR_COLUMNS:
            row[c] = _as_int(r[c])
        rows.append(row)
    return rows


def summarize_phases(rows, *, top_n: int = 5) -> dict:
    """Shape the per-phase rows into the per-checkpoint summary that gets logged and stored.

    * `rounds` — the largest per-phase distinct-`round_id` count, i.e. how many round attempts the
      window saw at all. Taken as a max rather than a sum because every phase of one round shares the
      id, and as a max rather than off one named phase because a deferred or non-leader round emits
      only the phases it reached.
    * `slowest` — the `top_n` phases by total time, each with its worst single occurrence. This is the
      thing that was unanswerable before `d412f85f749`: "where did round 33 spend 39 minutes".
    * `detector` — the four detector values plus `ref_folding_aborted`, summed over the window, keyed
      `<phase>.<metric>`. Absent (not zero) when the owning phase never ran in the window: a round
      that never led emits no `fold_*` phase at all, and calling that "0 holes" would be a claim the
      data does not support.
    * `intake_mismatch` — `logs_accounted - logs_applied`, the one derived value worth naming, because
      the identity it checks (every position the sealed cut covers reached the single cursor-advance
      site) is the whole reason the pair is emitted.

    Raises `SignalsUnsupported` when a phase that RAN did not carry one of its own detector metrics —
    see the presence check below.

    Pure function: `rows` is whatever `parse_phase_summary` produced."""
    by_phase = {r["phase"]: r for r in rows}
    slowest = sorted(rows, key=lambda r: r["total_us"], reverse=True)[:top_n]
    detector = {}
    for phase, metric in DETECTOR_METRICS:
        r = by_phase.get(phase)
        if r is None:
            continue
        # FAIL-CLOSED ON A RENAMED PHASE METRIC — the `system.events` discipline at the top of this
        # module, applied to the other half of the reader. `sum(phase_metrics['x'])` is a DEFINED zero
        # for an absent key, so a metric that gets renamed in the server does not raise here: it
        # reports 0 forever, and `intake_mismatch` silently becomes `-logs_applied` on every healthy
        # round. That is exactly the silent-degradation failure this module exists to avoid, and it
        # already happened once (`logs_intended` -> `logs_accounted`). The summed `metrics` map carries
        # only keys some row actually had, so it is the presence oracle the scalar column cannot be:
        # a phase that RAN emits its own detector metrics unconditionally, so an absent key means the
        # name moved, not that the value was zero.
        if metric not in r["metrics"]:
            raise SignalsUnsupported(
                f"the GC phase {phase!r} ran ({r['calls']} rows) but its `phase_metrics` map carries "
                f"no {metric!r} key. The metric was renamed or removed in the server; the detector "
                f"column would read a permanent silent zero. Known keys on this phase: "
                f"{sorted(r['metrics'])}")
        detector[f"{phase}.{metric}"] = r[metric]
    intake = by_phase.get("fold_ref_intake")
    mismatch = None
    if intake is not None:
        mismatch = intake["logs_accounted"] - intake["logs_applied"]
    return {
        "phases": len(rows),
        "rounds": max((r["rounds"] for r in rows), default=0),
        "total_us": sum(r["total_us"] for r in rows),
        "slowest": [
            {"phase": r["phase"], "total_us": r["total_us"], "max_us": r["max_us"],
             "rounds": r["rounds"], "calls": r["calls"]}
            for r in slowest
        ],
        "detector": detector,
        "intake_mismatch": mismatch,
    }


def format_phase_summary(summary: dict) -> str:
    """One log line per checkpoint: the slowest phases and every detector value that was observed."""
    slow = " ".join(
        f"{s['phase']}={s['total_us'] / 1000.0:.1f}ms(max {s['max_us'] / 1000.0:.1f}ms,"
        f"r={s['rounds']})"
        for s in summary["slowest"])
    det = " ".join(f"{k}={v}" for k, v in sorted(summary["detector"].items())) or "(no fold phase ran)"
    mm = summary["intake_mismatch"]
    mm_s = "n/a" if mm is None else str(mm)
    return (f"GC PHASES rounds={summary['rounds']} phases={summary['phases']} "
            f"total={summary['total_us'] / 1000.0:.1f}ms | slowest: {slow} | detector: {det} "
            f"| logs_accounted-logs_applied={mm_s}")


def read_phase_summary(node, since_ts: int, *, timeout: float = 120.0, flush: bool = True):
    """Read the per-phase GC-log summary for the window since `since_ts` from one node.

    Returns the parsed rows, or `None` on a legitimate gap: the node is unreachable (chaos) or the GC
    log has never been materialized on it (`SystemLog` creates its table lazily, and a replica that
    has never held the GC lease genuinely has no rows). Any other failure RAISES — in particular
    `UNKNOWN_IDENTIFIER` on `phase`/`round_id`/`phase_metrics`, which means the server predates
    `d412f85f749` and cannot produce this signal at all. Silently reporting an empty summary for that
    is exactly the vacuous-observation bug of 2026-07-25.

    `flush` issues `SYSTEM FLUSH LOGS` first: the GC log is buffered, and a checkpoint that reads it
    without flushing systematically misses the rounds closest to the moment of interest."""
    try:
        if flush:
            node.command("SYSTEM FLUSH LOGS", timeout=timeout)
        txt = node.query(phase_summary_sql(since_ts), timeout=timeout)
    except Exception as e:
        if not is_benign_probe_gap(e, allow_missing_table=True):
            raise
        return None
    return parse_phase_summary(txt)


# ---------------------------------------------------------------------------
# Run-level accounting
# ---------------------------------------------------------------------------

class SignalTracker:
    """Accumulates what the run actually SAW, so the end-of-run report can say whether each signal was
    observed at all. Task 21's own stability criterion is that a green run in which a counter was never
    read is not stable, it is blind — which needs this bookkeeping to be checkable rather than assumed.

    Pure state machine (no I/O): `observe` takes what a reader returned, including `None` for a
    legitimate gap."""

    def __init__(self, events=CAS_SIGNAL_EVENTS):
        self.events = tuple(events)
        self.reads = 0
        self.gaps = 0
        self.latest = {}                             # node -> {event: value}
        self.peak = {e: 0 for e in self.events}      # max value ever seen on any node
        self.nonzero_reads = {e: 0 for e in self.events}

    def observe(self, node_name: str, values) -> None:
        if values is None:
            self.gaps += 1
            return
        self.reads += 1
        self.latest[node_name] = dict(values)
        for e in self.events:
            v = values.get(e)
            if v is None:
                continue
            if v > self.peak[e]:
                self.peak[e] = v
            if v > 0:
                self.nonzero_reads[e] += 1

    def format_latest(self) -> str:
        """One compact line of the most recent per-node values, for a checkpoint log."""
        parts = []
        for node_name in sorted(self.latest):
            vals = self.latest[node_name]
            body = " ".join(f"{e}={vals.get(e)}" for e in self.events)
            parts.append(f"{node_name}: {body}")
        return " | ".join(parts) if parts else "(no reading yet)"

    def report_lines(self) -> list:
        """End-of-run block. Names the counters explicitly as REPORTED-NOT-GATED (Task 21 step 3:
        their benign rates are uncharacterised, and a threshold goes in only once several runs agree),
        and states plainly whether the run read them at all."""
        lines = [f"CAS SIGNALS: {self.reads} successful reads, {self.gaps} probe gaps "
                 f"(node down under chaos — not an error)"]
        if self.reads == 0:
            lines.append("CAS SIGNALS: WARNING — not a single successful read. This run is BLIND to "
                         "every counter below; treat a green result as unproven.")
        for e in self.events:
            gated = "reported-not-gated" if e in UNCHARACTERISED_SIGNALS else "gated"
            lines.append(f"CAS SIGNALS:   {e} peak={self.peak[e]} "
                         f"nonzero_in={self.nonzero_reads[e]}/{self.reads} reads ({gated})")
        return lines


class LatePutFencingViolation(RuntimeError):
    """A counter that carries the late-PUT-loses invariant moved off zero.

    Raised where the soak can still say WHERE it happened. Not an availability complaint: every counter
    behind it is documented as always-zero, so a nonzero reading is a durable transaction that went
    missing, a fold cursor that advanced past work, or a stream that stopped being dense."""


def check_late_put_fencing(values, *, baseline=None) -> list:
    """Evaluate the violation half of the late-PUT-loses invariant against ONE node's reading.

    Pure. Returns a list of human-readable violation strings (empty when the invariant holds). `values`
    is what `read_signal_events` returned, or `None` for a legitimate probe gap — a gap yields no
    violations, because a node that could not be read has said nothing, which is not the same as
    saying zero.

    `baseline` is the node's preflight reading, subtracted when supplied. The counters are cumulative
    per process, and a soak stand can legitimately reuse a container that already carried a nonzero
    value from an earlier run; charging that to this run would be an inherited red. What this run did
    is the DELTA."""
    if values is None:
        return []
    violations = []
    for event, why in LATE_PUT_VIOLATION_NOTES:
        now = values.get(event)
        if now is None:
            continue
        was = (baseline or {}).get(event, 0)
        delta = now - was
        if delta > 0:
            violations.append(f"{event}={delta} (cumulative {now}, baseline {was}): {why}")
    return violations


class LatePutFencing:
    """Run-level accounting for the late-PUT-loses invariant: a fenced predecessor's PUT never
    materializes below coverage its successor has already declared.

    Two halves, and the report states both, because either one alone is misleading:

    * **evidence** — did the run reach the fencing path at all (`LATE_PUT_EVIDENCE_EVENTS`)? A chaos
      soak in which no epoch was ever sealed did not test this invariant, and saying so is the
      difference between "held" and "never asked". Reported, never a failure.
    * **violations** — did any always-zero counter move (`LATE_PUT_VIOLATION_EVENTS`)? GATED: the
      caller turns a nonzero delta into a checkpoint failure. This is the assertion.

    Same shape as `SignalTracker` on purpose (`observe` per node per read, `report_lines` at the end),
    and just as pure — the caller does the I/O and decides what a violation costs."""

    def __init__(self, baseline=None):
        # {node_repr: {event: value}} from `preflight_signals`, so an inherited nonzero counter on a
        # reused container is not charged to this run.
        self.baseline = dict(baseline or {})
        self.reads = 0
        self.gaps = 0
        self.evidence_peak = {e: 0 for e in LATE_PUT_EVIDENCE_EVENTS}
        self.violation_peak = {e: 0 for e in LATE_PUT_VIOLATION_EVENTS}
        self.violations = []          # [(node_repr, label, violation string)] — every one ever seen

    def observe(self, node_name: str, values, *, label: str = "") -> list:
        """Fold one node's reading in and return the violations it carries (empty when clean)."""
        if values is None:
            self.gaps += 1
            return []
        self.reads += 1
        base = self.baseline.get(node_name, {})
        for e in LATE_PUT_EVIDENCE_EVENTS:
            v = values.get(e)
            if v is not None:
                delta = v - base.get(e, 0)
                if delta > self.evidence_peak[e]:
                    self.evidence_peak[e] = delta
        for e in LATE_PUT_VIOLATION_EVENTS:
            v = values.get(e)
            if v is not None:
                delta = v - base.get(e, 0)
                if delta > self.violation_peak[e]:
                    self.violation_peak[e] = delta
        found = check_late_put_fencing(values, baseline=base)
        for v in found:
            self.violations.append((node_name, label, v))
        return found

    @property
    def exercised(self) -> bool:
        """True when the run actually reached the fencing path — at least one epoch seal was minted."""
        return self.evidence_peak.get("CasRefRecoveryEpochSealed", 0) > 0

    def report_lines(self) -> list:
        lines = [f"LATE-PUT FENCING: {self.reads} readings, {self.gaps} probe gaps; "
                 f"{len(self.violations)} violation readings"]
        lines.append("LATE-PUT FENCING:   evidence (deltas over the run, never a failure): "
                     + " ".join(f"{e}={self.evidence_peak[e]}" for e in LATE_PUT_EVIDENCE_EVENTS))
        lines.append("LATE-PUT FENCING:   violations (gated, must all be 0): "
                     + " ".join(f"{e}={self.violation_peak[e]}" for e in LATE_PUT_VIOLATION_EVENTS))
        if not self.exercised:
            lines.append("LATE-PUT FENCING: WARNING — no epoch seal was minted in this run "
                         "(CasRefRecoveryEpochSealed stayed 0), so the fencing path was never reached. "
                         "The zero violations below are the absence of a test, not a passing one.")
        for node_name, label, v in self.violations:
            lines.append(f"LATE-PUT FENCING:   VIOLATION [{node_name}]{' ' + label if label else ''}: {v}")
        return lines


class PhaseCoverage:
    """Tracks whether the per-phase GC summary was ever actually captured, and the worst values seen.

    Same reason as `SignalTracker`: a soak that never once read the phase rows has not exercised the
    signal, and the report must say so rather than print nothing."""

    def __init__(self):
        self.attempts = 0
        self.captured = 0
        self.gaps = 0
        self.empty = 0
        self.rounds = 0
        self.worst_phase_us = {}     # phase -> worst single occurrence over the run
        self.detector_peak = {}      # "<phase>.<metric>" -> max over the run

    def observe(self, summary) -> None:
        self.attempts += 1
        if summary is None:
            self.gaps += 1
            return
        if summary["phases"] == 0:
            self.empty += 1
            return
        self.captured += 1
        self.rounds += summary["rounds"]
        for s in summary["slowest"]:
            prev = self.worst_phase_us.get(s["phase"], 0)
            if s["max_us"] > prev:
                self.worst_phase_us[s["phase"]] = s["max_us"]
        for k, v in summary["detector"].items():
            if v > self.detector_peak.get(k, 0):
                self.detector_peak[k] = v

    def report_lines(self) -> list:
        lines = [f"GC PHASES: captured at {self.captured}/{self.attempts} checkpoints "
                 f"({self.gaps} probe gaps, {self.empty} empty windows), "
                 f"{self.rounds} round attempts observed"]
        if self.captured == 0:
            lines.append("GC PHASES: WARNING — the per-phase GC log was NEVER captured in this run. "
                         "Nothing below is evidence; find out why before reading the run as green.")
        worst = sorted(self.worst_phase_us.items(), key=lambda kv: kv[1], reverse=True)[:8]
        if worst:
            lines.append("GC PHASES:   worst single occurrence per phase: "
                         + " ".join(f"{p}={us / 1000.0:.1f}ms" for p, us in worst))
        if self.detector_peak:
            lines.append("GC PHASES:   detector peaks: "
                         + " ".join(f"{k}={v}" for k, v in sorted(self.detector_peak.items())))
        return lines
