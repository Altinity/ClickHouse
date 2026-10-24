"""Run cards on policies: one server process and one fresh bucket prefix per (policy, card, repeat)."""
import datetime
import glob
import hashlib
import os
import platform
import shlex
import shutil
import statistics
import subprocess
import sys
import time
import traceback
from dataclasses import dataclass, field
from typing import Callable

from . import config
from . import gc
from . import report as R
from .cards import CARDS

CARD_UNITS = {name: card.unit for name, card in CARDS.items()}
from .client import Client

TOOL_DIR = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
PHASES = ('cold', 'hot')
SERVER_START_TIMEOUT_SEC = 300
SERVER_STOP_TIMEOUT_SEC = 120
# Above rustfs.sh's own RM_PREFIX_TIMEOUT, so the script's deadline fires first and removes its mc container.
RM_PREFIX_TIMEOUT_SEC = 1800 + 120
# What defines the measurement; docs, tests and accepted references do not.
WORKLOAD_GLOBS = ('yardstick.py', 'yardstick/*.py', 'yardstick/cards/*.py', 'etc/*', 'rustfs.sh', 'server.sh')
GC_ROUNDS_COLUMNS = ['round_id', 'started_in_window', 'finished_in_window', 'outcome', 'objects_deleted',
                     'manifests_deleted', 'duration_ms']
TABLE_METRICS = ['wall', 'median_op_ms', 'p90_op_ms'] + R.REQUEST_METRICS + ['bytes_read', 'bytes_written']


class ServerControl:
    """server.sh wrapper; one process at a time on `port_base`."""
    def __init__(self, binary: str, port_base: int):
        self.binary = os.path.abspath(binary)
        self.port_base = port_base

    def _run(self, run_dir, args, timeout, new_session=False):
        with open(os.path.join(run_dir, 'server_control.log'), 'a') as log:
            subprocess.run(['bash', os.path.join(TOOL_DIR, 'server.sh')] + args, stdout=log, stderr=subprocess.STDOUT,
                           check=True, timeout=timeout, start_new_session=new_session)

    def start(self, run_dir):
        # SYSTEM SHUTDOWN sends SIGTERM to the server's whole process group; keep the runner out of it.
        self._run(run_dir, ['start', self.binary, run_dir, str(self.port_base)], SERVER_START_TIMEOUT_SEC, True)

    def stop(self, run_dir):
        self._run(run_dir, ['stop', run_dir], SERVER_STOP_TIMEOUT_SEC)


@dataclass
class Deps:
    run_id: str
    runs_dir: str
    endpoint: str
    gc: bool
    scale: float
    server: object
    client_for: Callable[[str], object]
    write_config: Callable = config.write_config
    config_hashes: dict = field(default_factory=dict)


def workload_files(tool_dir) -> list[str]:
    return sorted({os.path.relpath(p, tool_dir) for g in WORKLOAD_GLOBS for p in glob.glob(os.path.join(tool_dir, g))
                   if os.path.isfile(p)})


def workload_hash(tool_dir) -> str:
    """SHA-256 over the (path, bytes) of every workload file."""
    digest = hashlib.sha256()
    for rel in workload_files(tool_dir):
        with open(os.path.join(tool_dir, rel), 'rb') as f:
            content = f.read()
        for chunk in (rel.encode(), content):
            digest.update(len(chunk).to_bytes(8, 'little'))
            digest.update(chunk)
    return digest.hexdigest()


def run_dir_for(base, run_id, policy, card, k, phase) -> str:
    """Pass directory for an empty `phase`, otherwise its per-phase subdirectory."""
    parts = [base, run_id, policy, card, f'k{k}'] + ([phase] if phase else [])
    return os.path.join(*parts)


def policy_order(policies, k):
    i = (k - 1) % len(policies)
    return policies[i:] + policies[:i]


def _row(policy, card, phase, k, metric, value, unit_count):
    return dict(policy=policy, card=card, phase=phase, repeat=k, metric=metric, value=value, unit_count=unit_count)


def _status_rows(policy, card, phases, k, status):
    return [_row(policy, card.name, p, k, 'status', status, 0) for p in phases]


def _write_kv(path, header, items):
    with open(path, 'w') as f:
        f.write('\t'.join(header) + '\n')
        for item in items:
            f.write('\t'.join(str(v) for v in item) + '\n')


def _write_ops(path, ops):
    columns = list(ops[0]) if ops else ['ms']
    _write_kv(path, columns, [[op[c] for c in columns] for op in ops])


def _p90(values):
    return statistics.quantiles(values, n=10, method='inclusive')[8] if len(values) > 1 else values[0]


def _check_invariants(card, c, counts, phase, when, checked, path):
    for name, (expected, actual) in card.invariants(c, counts, phase, when).items():
        checked.append((when, name, expected, actual, 'ok' if expected == actual else 'VIOLATED'))
    _write_kv(path, ['when', 'name', 'expected', 'actual', 'result'], checked)
    violated = [row for row in checked if row[4] != 'ok']
    if violated:
        raise AssertionError(f'invariants violated: {violated}')


def _window(card, c, policy, counts, phase, k, pass_dir, process_start_us):
    """One measured pass; returns its sample rows."""
    phase_dir = os.path.join(pass_dir, phase)
    os.makedirs(phase_dir, exist_ok=True)
    card.before_window(c, phase)
    c.settle(card.table)
    checked = []
    inv_path = os.path.join(phase_dir, 'invariants.tsv')
    _check_invariants(card, c, counts, phase, 'before', checked, inv_path)
    before = c.events()
    t_start_us = time.time_ns() // 1000
    t0 = time.monotonic()
    result = card.measured(c, policy, counts, phase)
    wall = time.monotonic() - t0
    t_end_us = time.time_ns() // 1000
    after = c.events()
    delta = R.delta(before, after)
    _write_kv(os.path.join(phase_dir, 'events_before.tsv'), ['event', 'value'], sorted(before.items()))
    _write_kv(os.path.join(phase_dir, 'events_after.tsv'), ['event', 'value'], sorted(after.items()))
    _write_kv(os.path.join(phase_dir, 'events_delta.tsv'), ['event', 'value'], sorted(delta.items()))
    ops = result['ops']
    _write_ops(os.path.join(phase_dir, 'card.tsv'), ops)
    is_cas = policy.startswith('cas')
    rounds = gc.collect(c, t_start_us, t_end_us, process_start_us) if is_cas else []
    _write_kv(os.path.join(phase_dir, 'gc_rounds.tsv'), GC_ROUNDS_COLUMNS,
              [[r[column] for column in GC_ROUNDS_COLUMNS] for r in rounds])
    _check_invariants(card, c, counts, phase, 'after', checked, inv_path)

    units = result['unit_count']
    rows = [_row(policy, card.name, phase, k, 'wall', wall, units)]
    if ops:
        ms = [op['ms'] for op in ops]
        rows.append(_row(policy, card.name, phase, k, 'median_op_ms', statistics.median(ms), units))
        rows.append(_row(policy, card.name, phase, k, 'p90_op_ms', _p90(ms), units))
    present = set(before) & set(after)
    rows += [_row(policy, card.name, phase, k, m, v, units) for m, v in R.aggregate(delta, present).items()]
    if is_cas:
        inside = sum(r['started_in_window'] and r['finished_in_window'] for r in rounds)
        rows.append(_row(policy, card.name, phase, k, 'gc_rounds', inside, units))
        rows.append(_row(policy, card.name, phase, k, 'gc_rounds_overlapping', len(rounds) - inside, units))
    diagnostics = R.S3_ERROR_COUNTERS + (R.CAS_DIAGNOSTICS if is_cas else [])
    rows += [_row(policy, card.name, phase, k, m, delta[m] if m in present else R.UNAVAILABLE, units)
             for m in diagnostics]
    return rows


def _log_error(pass_dir, what):
    os.makedirs(pass_dir, exist_ok=True)
    with open(os.path.join(pass_dir, 'error.txt'), 'a') as f:
        f.write(f'--- {what}\n{traceback.format_exc()}\n')


def run_one(policy, card, k, deps: Deps) -> list[dict]:
    """Measure one (policy, card, repeat) on its own server process.
    A failed window marks it and every later phase FAILED; a setup, teardown or stop error fails every phase."""
    phases = PHASES if card.has_hot else PHASES[:1]
    ok, reason = card.supports(policy)
    if not ok:
        return _status_rows(policy, card, phases, k, f'UNSUPPORTED ({reason})')

    pass_dir = os.path.abspath(run_dir_for(deps.runs_dir, deps.run_id, policy, card.name, k, ''))
    os.makedirs(pass_dir, exist_ok=True)
    prefix = f'test/{deps.run_id}/{policy}/{card.name}/k{k}'
    counts = card.counts(deps.scale)
    rows = []
    failed = []
    started = False
    c = None
    try:
        try:
            config_hash = deps.write_config(pass_dir, policy, deps.endpoint, prefix, deps.gc)
            if config_hash != deps.config_hashes.get(policy):
                raise RuntimeError(f'config hash {config_hash} differs from the run manifest\'s '
                                   f'{deps.config_hashes.get(policy)} for {policy}')
            started = True
            process_start_us = time.time_ns() // 1000
            deps.server.start(pass_dir)
            c = deps.client_for(pass_dir)
            card.setup(c, policy, counts)
            if card.cold_reset_needed():
                deps.server.stop(pass_dir)
                caches = os.path.join(pass_dir, 'caches')
                if os.path.isdir(caches):
                    shutil.rmtree(caches)
                os.makedirs(caches)
                process_start_us = time.time_ns() // 1000
                deps.server.start(pass_dir)
                c.drop_caches()
        except Exception:
            _log_error(pass_dir, 'setup')
            failed = list(phases)
        else:
            for i, phase in enumerate(phases):
                try:
                    rows += _window(card, c, policy, counts, phase, k, pass_dir, process_start_us)
                except Exception:
                    _log_error(pass_dir, f'{phase} window')
                    failed = list(phases[i:])
                    break
    finally:
        cleaned = _cleanup(card, c, deps.server if started else None, pass_dir)
    if not cleaned:
        rows, failed = [], list(phases)
    return rows + _status_rows(policy, card, failed, k, 'FAILED')


def _cleanup(card, c, server, pass_dir):
    """Teardown, then stop the server even if teardown raised or was interrupted; True when both succeeded."""
    ok = True
    try:
        if c is not None:
            card.teardown(c)
    except Exception:
        ok = False
        _log_error(pass_dir, 'teardown')
    finally:
        if server is not None:
            try:
                server.stop(pass_dir)
            except Exception:
                ok = False
                _log_error(pass_dir, 'server stop')
    return ok


def run_matrix(policies, cards, repeats, deps: Deps, rows, after_pass=None, timings=None) -> list[dict]:
    """Append every pass's rows to `rows`, so an interrupted run keeps the finished passes.
    `timings` collects (policy, card, repeat, seconds) of each whole pass: server starts, setup, both windows, teardown."""
    for k in range(1, repeats + 1):
        for policy in policy_order(policies, k):
            for card in cards:
                t0 = time.monotonic()
                rows += run_one(policy, card, k, deps)
                if timings is not None:
                    timings.append((policy, card.name, k, time.monotonic() - t0))
                if after_pass:
                    after_pass(rows)
    return rows


def write_pass_seconds(path, timings):
    with open(path, 'w', encoding='utf-8') as f:
        f.write('policy\tcard\trepeat\tseconds\n')
        for policy, card_name, k, seconds in timings:
            f.write(f'{policy}\t{card_name}\t{k}\t{seconds:.1f}\n')


def run_all(policies, cards, repeats, deps: Deps, cleanup, after_pass=None, timings=None):
    """run_matrix, then `cleanup()` even on Ctrl-C; returns (rows, interrupted, cleanup result)."""
    rows = []
    interrupted = False
    try:
        run_matrix(policies, cards, repeats, deps, rows, after_pass, timings)
    except KeyboardInterrupt:
        interrupted = True
    finally:
        cleaned = cleanup()
    return rows, interrupted, cleaned


def _out(cmd, timeout=120, **kw):
    return subprocess.run(cmd, check=True, capture_output=True, text=True, timeout=timeout, **kw).stdout.strip()


def remove_prefix(run_id) -> bool:
    try:
        _out(['bash', os.path.join(TOOL_DIR, 'rustfs.sh'), 'rm-prefix', f'test/{run_id}'], timeout=RM_PREFIX_TIMEOUT_SEC)
        return True
    except subprocess.SubprocessError as e:
        print(f'removing prefix test/{run_id} failed: {e}', file=sys.stderr)
        return False


def _probe(binary):
    text = _out([binary, 'local', '--query',
                 "SELECT version(), (SELECT value FROM system.build_options WHERE name = 'BUILD_TYPE') "
                 'FORMAT TabSeparated'])
    version, build_type = text.split('\t')
    return version, build_type


def _format_counts(cards, scale):
    return ';'.join(f'{card.name}=' + ','.join(f'{k}:{v}' for k, v in card.counts(scale).items()) for card in cards)


def _smoke_failures(summary, policies, cards):
    idx = {(r['policy'], r['card'], r['phase'], r['metric']): r for r in summary}
    failures = []
    for policy in policies:
        for card in cards:
            for phase in PHASES:
                status = idx.get((policy, card.name, phase, 'status'))
                if status is not None:
                    if status['median'].startswith('FAILED'):
                        failures.append(f'{policy} {card.name} {phase}: FAILED')
                    continue
                failures += [f'{policy} {card.name} {phase} {m}: {R.UNAVAILABLE}' for m in TABLE_METRICS
                             if (policy, card.name, phase, m) in idx
                             and idx[(policy, card.name, phase, m)]['median'] == R.UNAVAILABLE]
    return failures


def run(args) -> int:
    """Run every (repeat, policy, card) and print the table.
    Exit code: 1 on a FAILED pass or a failed prefix removal, 2 on a failed smoke, 130 on Ctrl-C (which wins)."""
    cards = [CARDS[name] for name in args.cards]
    policies = args.policies
    run_id = datetime.datetime.now(datetime.timezone.utc).strftime('%Y%m%dT%H%M%SZ')
    run_root = os.path.abspath(os.path.join(args.runs_dir, run_id))
    os.makedirs(run_root)
    args.run_root = run_root
    gc_on = args.gc == 'on'

    config_dir = os.path.join(run_root, 'config')
    hashes = {p: config.write_config(os.path.join(config_dir, p), p, args.endpoint, f'test/{run_id}/{p}/config', gc_on)
              for p in policies}
    version, build_type = _probe(args.binary)
    if args.external_endpoint:
        store = 'external'
    else:
        store = _out(['bash', os.path.join(TOOL_DIR, 'rustfs.sh'), 'status']).removeprefix('image=')
    manifest = dict(
        binary_version=version,
        git_rev=_out(['git', 'rev-parse', '--short', 'HEAD'], cwd=TOOL_DIR),
        build_type=build_type,
        workload_hash=workload_hash(TOOL_DIR),
        dirty='yes' if _out(['git', 'status', '--porcelain', '--'] + workload_files(TOOL_DIR), cwd=TOOL_DIR) else 'no',
        scale=args.scale,
        counts=_format_counts(cards, args.scale),
        gc=args.gc,
        repeats=args.repeats,
        policy_order=';'.join(f'k{k}=' + ','.join(policy_order(policies, k)) for k in range(1, args.repeats + 1)),
        config_hash=';'.join(f'{p}={h}' for p, h in hashes.items()),
        config_dir=config_dir,
        store=store,
        store_version=args.endpoint_version or 'unknown',
        host=platform.node(),
        kernel=platform.release(),
        date=datetime.datetime.now(datetime.timezone.utc).isoformat(timespec='seconds'),
        flags=shlex.join(sys.argv),
    )
    R.write_manifest(os.path.join(run_root, 'manifest.tsv'), manifest)
    print(f'run {run_id} in {run_root}', flush=True)

    server = ServerControl(args.binary, args.port_base)
    deps = Deps(run_id=run_id, runs_dir=os.path.abspath(args.runs_dir), endpoint=args.endpoint, gc=gc_on,
                scale=args.scale, server=server, client_for=lambda run_dir: Client(args.port_base + 1),
                config_hashes=hashes)
    samples_path = os.path.join(run_root, 'samples.tsv')

    timings = []
    rows, interrupted, cleaned = run_all(policies, cards, args.repeats, deps,
                                         lambda: args.keep_prefix or remove_prefix(run_id),
                                         after_pass=lambda rows: R.write_samples(samples_path, rows),
                                         timings=timings)
    return finish_run(run_root, rows, timings, interrupted, cleaned, policies, cards, args.smoke)


def finish_run(run_root, rows, timings, interrupted, cleaned, policies, cards, smoke) -> int:
    """Write the run's files and print its table; returns the exit code of `run`."""
    write_pass_seconds(os.path.join(run_root, 'pass_seconds.tsv'), timings)
    R.write_samples(os.path.join(run_root, 'samples.tsv'), rows)
    # An interrupted repeat leaves policies with different repeat sets; their ratios are dropped, not paired.
    summary = R.build_summary(rows, strict=not interrupted)
    R.write_summary(os.path.join(run_root, 'summary.tsv'), summary)
    manifest = R.read_manifest(os.path.join(run_root, 'manifest.tsv'))
    print(R.render_header(manifest, summary), end='')
    print(R.render_table(summary, [p for p in policies if not p.endswith('_cached')],
                         [p for p in policies if p.endswith('_cached')], CARD_UNITS))
    exit_code = 0
    if not cleaned or any(r['metric'] == 'status' and r['value'] == 'FAILED' for r in rows):
        exit_code = 1
    if smoke:
        failures = _smoke_failures(summary, policies, cards)
        for f in failures:
            print(f'smoke: {f}', file=sys.stderr)
        if failures:
            exit_code = 2
    if interrupted:
        print('interrupted: the table covers the finished passes only', file=sys.stderr)
        exit_code = 130
    return exit_code
