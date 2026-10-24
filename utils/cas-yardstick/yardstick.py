#!/usr/bin/env python3
"""CAS yardstick: the same workload cards on a CAS disk and on S3 disks, one server process per measurement."""
import argparse
import os
import sys

from yardstick import report as R
from yardstick.compare import compare_runs, reference_dir
from yardstick.cards import CARDS
from yardstick.config import POLICIES
from yardstick.runner import CARD_UNITS, TOOL_DIR, run

DEFAULT_ENDPOINT = 'http://127.0.0.1:19121'
DEFAULT_RUNS_DIR = os.path.join(os.path.dirname(os.path.dirname(TOOL_DIR)), 'tmp', 'cas-yardstick', 'runs')
DEFAULTS = dict(cards=list(CARDS), scale=1.0, repeats=3, policies=list(POLICIES))
SMOKE = dict(cards=list(CARDS), scale=0.01, repeats=1, policies=list(POLICIES))
QUICK = dict(cards=list(CARDS), scale=0.25, repeats=1, policies=[p for p in POLICIES if not p.endswith('_cached')])
PROFILES = {'smoke': SMOKE, 'quick': QUICK}


def _list(choices):
    def parse(text):
        items = [x for x in text.split(',') if x]
        unknown = [x for x in items if x not in choices]
        if unknown or not items:
            raise argparse.ArgumentTypeError(f'expected a comma-separated subset of {",".join(choices)}')
        # A repeated entry would rerun into the same pass directory and bucket prefix.
        if len(set(items)) != len(items):
            raise argparse.ArgumentTypeError(f'duplicate entries in {text}')
        return items
    return parse


def cmd_run(args):
    if args.smoke and args.quick:
        sys.exit('--smoke and --quick exclude each other')
    profile_name = 'smoke' if args.smoke else 'quick' if args.quick else None
    profile = PROFILES.get(profile_name, DEFAULTS)
    conflicts = [name for name in DEFAULTS if profile_name and getattr(args, name) is not None]
    if conflicts:
        sys.exit(f'--{profile_name} fixes --{" --".join(conflicts)}; drop them')
    for name in DEFAULTS:
        if getattr(args, name) is None:
            setattr(args, name, profile[name])
    args.external_endpoint = args.endpoint is not None
    if args.external_endpoint and not args.keep_prefix:
        sys.exit('--endpoint needs --keep-prefix: prefix removal only reaches the RustFS of rustfs.sh')
    args.endpoint = args.endpoint or DEFAULT_ENDPOINT
    code = run(args)
    if args.compare and code in (0, 2):
        try:
            # The run's own exit code wins; the comparison is printed either way.
            compared = compare_runs(args.run_root, _reference(args.reference))
            return code or compared
        except SystemExit as e:
            print(e.code, file=sys.stderr)
            return code or 1
    return code


def _reference(explicit):
    """Reference run directory: `explicit`, else the one `reference/CURRENT` names; exits with a message otherwise."""
    try:
        path = explicit or reference_dir(TOOL_DIR)
        if not os.path.isfile(os.path.join(path, 'manifest.tsv')):
            raise FileNotFoundError(f'{path}: no manifest.tsv')
        return path
    except (OSError, ValueError) as e:
        sys.exit(f'no reference: {e}')


def cmd_compare(args):
    return compare_runs(args.run_dir, _reference(args.reference_dir))


def cmd_report(args):
    summary = R.read_summary(os.path.join(args.run_dir, 'summary.tsv'))
    policies = [p for p in POLICIES if any(r['policy'] == p for r in summary)]
    print(R.render_header(R.read_manifest(os.path.join(args.run_dir, 'manifest.tsv')), summary), end='')
    print(R.render_table(summary, [p for p in policies if not p.endswith('_cached')],
                         [p for p in policies if p.endswith('_cached')], CARD_UNITS))
    if args.diagnostics:
        print(R.render_diagnostics(summary))
    return 0


def list_counters():
    for metric, counters in R.AGGREGATES.items():
        print(f'{metric}\t{" + ".join(counters)}')
    print(f'CAS diagnostics\t{" ".join(R.CAS_DIAGNOSTICS)}')
    return 0


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--list-counters', action='store_true', help='print the counters behind every aggregate')
    sub = parser.add_subparsers(dest='command')

    r = sub.add_parser('run', help='measure the cards')
    r.add_argument('--binary', required=True)
    r.add_argument('--cards', type=_list(list(CARDS)), help='default: all cards')
    r.add_argument('--policies', type=_list(POLICIES), help='default: all six')
    r.add_argument('--repeats', type=int, help=f'default: {DEFAULTS["repeats"]}')
    r.add_argument('--scale', type=float, help=f'default: {DEFAULTS["scale"]}')
    r.add_argument('--gc', choices=['on', 'off'], default='on')
    r.add_argument('--endpoint', help=f'an already running store instead of rustfs.sh on {DEFAULT_ENDPOINT}')
    r.add_argument('--endpoint-version')
    r.add_argument('--port-base', type=int, default=19700)
    r.add_argument('--runs-dir', default=DEFAULT_RUNS_DIR)
    r.add_argument('--keep-prefix', action='store_true', help='keep the run\'s bucket prefix')
    r.add_argument('--compare', action='store_true', help='compare with reference/CURRENT after the run')
    r.add_argument('--reference', help='run directory for --compare (default: the one reference/CURRENT names)')
    r.add_argument('--smoke', action='store_true',
                   help='all cards, all policies, --scale 0.01, --repeats 1; exit 2 on any FAILED or unavailable cell')
    r.add_argument('--quick', action='store_true',
                   help='all cards, uncached policies, --scale 0.25, --repeats 1: the everyday run')
    r.set_defaults(func=cmd_run)

    p = sub.add_parser('report', help='re-print the table of a finished run')
    p.add_argument('run_dir')
    p.add_argument('--diagnostics', action='store_true')
    p.set_defaults(func=cmd_report)

    c = sub.add_parser('compare', help='compare a run with a reference run (default: reference/CURRENT)')
    c.add_argument('run_dir')
    c.add_argument('reference_dir', nargs='?')
    c.set_defaults(func=cmd_compare)

    args = parser.parse_args()
    if args.list_counters:
        return list_counters()
    if args.command is None:
        parser.error('a command is required')
    return args.func(args)


if __name__ == '__main__':
    sys.exit(main())
