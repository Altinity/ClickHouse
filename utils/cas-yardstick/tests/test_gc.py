import contextlib
import io
import json
import re
import time
import os
import shutil
import tempfile
import unittest

from yardstick import compare as cmp
from yardstick import gc
from yardstick import report as R

T0, T1 = 1_000_000, 2_000_000


class FakeClient:
    def __init__(self, rows):
        self.rows = rows
        self.queries = []

    def query(self, sql, settings=None, timeout=3600):
        self.queries.append(sql)
        return '\n'.join(json.dumps(r) for r in self.rows) if sql.startswith('SELECT') else ''


def row(round_id, t0, t1, finished):
    return dict(round_id=round_id, t0=t0, t1=t1, outcome='Success', objects_deleted=5, manifests_deleted=2,
                duration_ms=30, finished=finished)


class EmulatingClient(FakeClient):
    """Applies the HAVING clause of `gc._SQL` to its rows, so tests exercise the filter the server would run."""
    def query(self, sql, settings=None, timeout=3600):
        if not sql.startswith('SELECT'):
            return super().query(sql)
        self.queries.append(sql)
        m = re.search(r'HAVING \(\(finished = 0 AND t0 >= (\d+)\) OR t1 >= (\d+)\) AND t0 <= (\d+) ', sql)
        assert m, sql
        process_start, start, end = (int(x) for x in m.groups())
        kept = [r for r in self.rows
                if ((r['finished'] == 0 and r['t0'] >= process_start) or r['t1'] >= start) and r['t0'] <= end]
        return '\n'.join(json.dumps(r) for r in kept)


class CollectTest(unittest.TestCase):
    def test_flags_and_flush_first(self):
        c = FakeClient([row('a', T0 + 10, T1 - 10, 1), row('b', T0 - 10, T1 - 10, 1), row('c', T0 + 10, T1 + 10, 0)])
        rounds = gc.collect(c, T0, T1, 0)
        self.assertEqual(c.queries[0], 'SYSTEM FLUSH LOGS')
        self.assertIn(f't1 >= {T0}) AND t0 <= {T1}', c.queries[1])
        self.assertEqual([(r['round_id'], r['started_in_window'], r['finished_in_window']) for r in rounds],
                         [('a', True, True), ('b', False, True), ('c', True, False)])

    def test_bounds_are_compared_as_integers(self):
        c = FakeClient([])
        gc.collect(c, T0, T1, 0)
        self.assertIn('toUnixTimestamp64Micro(min(event_time_microseconds)) AS t0', c.queries[1])
        self.assertIn('toUnixTimestamp64Micro(max(event_time_microseconds)) AS t1', c.queries[1])

    def test_no_rows(self):
        self.assertEqual(gc.collect(FakeClient([]), T0, T1, 0), [])

    def test_unfinished_round_is_open_until_it_finishes(self):
        rounds = gc.collect(EmulatingClient([row('a', T0 - 20, T0 - 10, 0)]), T0, T1, T0 - 100)
        self.assertEqual([(r['started_in_window'], r['finished_in_window']) for r in rounds], [(False, False)])

    def test_unfinished_round_of_an_earlier_process_is_not_open(self):
        rounds = gc.collect(EmulatingClient([row('a', T0 - 20, T0 - 10, 0)]), T0, T1, T0 - 5)
        self.assertEqual(rounds, [])

    def test_finished_after_window_end_is_not_finished_in_window(self):
        rounds = gc.collect(FakeClient([row('a', T0 + 1, T1 + 1, 1)]), T0, T1, 0)
        self.assertFalse(rounds[0]['finished_in_window'])


class CompareTest(unittest.TestCase):
    def setUp(self):
        self.dir = tempfile.mkdtemp()
        self.addCleanup(shutil.rmtree, self.dir)

    def make_run(self, name, **manifest):
        d = os.path.join(self.dir, name)
        os.makedirs(d)
        fields = {f: 'x' for f in R.COMPAT_FIELDS}
        fields.update(manifest)
        R.write_manifest(os.path.join(d, 'manifest.tsv'), fields)
        rows = [dict(policy='cas', card='cycle', phase='cold', repeat=1, metric='wall', value=2.0, unit_count=1)]
        R.write_summary(os.path.join(d, 'summary.tsv'), R.build_summary(rows))
        return d

    def compare(self, a, b):
        out = io.StringIO()
        with contextlib.redirect_stdout(out):
            code = cmp.compare_runs(a, b)
        return code, out.getvalue()

    def test_incompatible_prints_only_the_field(self):
        a, b = self.make_run('a', scale='1.0'), self.make_run('b', scale='0.1')
        code, out = self.compare(a, b)
        self.assertEqual(out, 'incompatible: scale\n')
        self.assertEqual(code, 1)

    def test_equal_manifests_render_the_comparison(self):
        a, b = self.make_run('a'), self.make_run('b')
        code, out = self.compare(a, b)
        self.assertEqual(code, 0)
        self.assertIn('cas cycle cold wall: cur 2.0', out)


class RunnerGcTest(unittest.TestCase):
    def run_card(self, policy, gc_rows):
        from tests.test_runner import FakeCard, FakeServer
        from yardstick import runner
        d = tempfile.mkdtemp()
        self.addCleanup(shutil.rmtree, d, ignore_errors=True)
        c = FakeClient(gc_rows)
        deps = runner.Deps(run_id='R', runs_dir=d, endpoint='e', gc=True, scale=1.0, server=FakeServer([]),
                           client_for=lambda run_dir: _Client(c),
                           write_config=lambda *a: 'h', config_hashes={policy: 'h'})
        rows = runner.run_one(policy, FakeCard('c', has_hot=False), 1, deps)
        return d, c, rows

    def test_cas_window_writes_rounds_and_counts(self):
        d, c, rows = self.run_card('cas', [row('a', 0, 2 ** 62, 1)])
        by = {r['metric']: r['value'] for r in rows}
        self.assertEqual((by['gc_rounds'], by['gc_rounds_overlapping']), (0, 1))
        with open(os.path.join(d, 'R', 'cas', 'c', 'k1', 'cold', 'gc_rounds.tsv')) as f:
            lines = f.read().splitlines()
        self.assertEqual(lines[1].split('\t')[0], 'a')

    def test_plain_s3_never_queries_the_gc_log(self):
        d, c, rows = self.run_card('s3', [row('a', 0, 1, 1)])
        self.assertFalse([q for q in c.queries if 'cas_gc_log' in q])
        self.assertNotIn('gc_rounds', {r['metric'] for r in rows})

    def test_table_cell_shows_overlapping(self):
        rows = [dict(policy='cas', card='c', phase='cold', repeat=1, metric=m, value=v, unit_count=1)
                for m, v in (('gc_rounds', 2), ('gc_rounds_overlapping', 1))]
        table = R.render_table(R.build_summary(rows), ['cas'], [])
        self.assertIn('2, 1 overlapping', table)

    def test_table_cell_shows_the_range_over_repeats(self):
        rows = [dict(policy='cas', card='c', phase='hot', repeat=k, metric=m, value=v, unit_count=1)
                for k, inside, over in ((1, 0, 0), (2, 3, 0), (3, 0, 1))
                for m, v in (('gc_rounds', inside), ('gc_rounds_overlapping', over))]
        table = R.render_table(R.build_summary(rows), ['cas'], [])
        self.assertIn('0 (0..3), 0 (0..1) overlapping', table)


class TwoProcessesTest(unittest.TestCase):
    def test_round_cut_by_the_cold_reset_stop_counts_in_no_window(self):
        from tests.test_runner import FakeCard, FakeServer
        from yardstick import runner

        class TimedServer(FakeServer):
            def start(self, run_dir):
                super().start(run_dir)
                self.started_us = getattr(self, 'started_us', []) + [time.time_ns() // 1000]

        server = TimedServer([])

        class Rounds(EmulatingClient):
            # One unfinished round per process lifetime, each starting 1 us after its process.
            @property
            def rows(self):
                return [row(f'p{i}', t + 1, t + 2, 0) for i, t in enumerate(server.started_us)]

            @rows.setter
            def rows(self, value):
                pass

        d = tempfile.mkdtemp()
        self.addCleanup(shutil.rmtree, d, ignore_errors=True)
        c = Rounds([])
        deps = runner.Deps(run_id='R', runs_dir=d, endpoint='e', gc=True, scale=1.0, server=server,
                           client_for=lambda run_dir: _Client(c), write_config=lambda *a: 'h',
                           config_hashes={'cas': 'h'})
        rows = runner.run_one('cas', FakeCard('c', cold_reset=True), 1, deps)
        self.assertEqual(len(server.started_us), 2)
        for phase in ('cold', 'hot'):
            with open(os.path.join(d, 'R', 'cas', 'c', 'k1', phase, 'gc_rounds.tsv')) as f:
                self.assertEqual([line.split('\t')[0] for line in f.read().splitlines()[1:]], ['p1'], phase)
        self.assertEqual({r['value'] for r in rows if r['metric'] == 'gc_rounds_overlapping'}, {1})


class FailingLogClient(FakeClient):
    def query(self, sql, settings=None, timeout=3600):
        if 'cas_gc_log' in sql:
            raise RuntimeError(sql, 'UNKNOWN_TABLE')
        return super().query(sql)


class GcFailureTest(unittest.TestCase):
    def test_failing_gc_log_query_fails_the_pass(self):
        from tests.test_runner import FakeCard, FakeServer
        from yardstick import runner
        d = tempfile.mkdtemp()
        self.addCleanup(shutil.rmtree, d, ignore_errors=True)
        c = FailingLogClient([])
        deps = runner.Deps(run_id='R', runs_dir=d, endpoint='e', gc=True, scale=1.0, server=FakeServer([]),
                           client_for=lambda run_dir: _Client(c), write_config=lambda *a: 'h',
                           config_hashes={'cas': 'h'})
        rows = runner.run_one('cas', FakeCard('c', has_hot=False), 1, deps)
        self.assertEqual([(r['phase'], r['metric'], r['value']) for r in rows], [('cold', 'status', 'FAILED')])


class TableColumnsTest(unittest.TestCase):
    def test_gc_column_only_for_cas_policies(self):
        rows = [dict(policy=p, card='c', phase='cold', repeat=1, metric='wall', value=1.0, unit_count=1)
                for p in ('cas', 's3')]
        header = R.render_table(R.build_summary(rows), ['cas', 's3'], []).splitlines()[2]
        self.assertEqual(header.count('gc_rounds'), 1)


class CliTest(unittest.TestCase):
    def setUp(self):
        import importlib.util
        tool = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
        spec = importlib.util.spec_from_file_location('yardstick_cli', os.path.join(tool, 'yardstick.py'))
        self.cli = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(self.cli)
        self.dir = tempfile.mkdtemp()
        self.addCleanup(shutil.rmtree, self.dir, ignore_errors=True)
        os.makedirs(os.path.join(self.dir, 'reference'))
        self.cli.TOOL_DIR = self.dir
        self.run_dir = self.make_run('run')
        self.ran = []

        def stub_run(args):
            self.ran.append(args)
            args.run_root = self.run_dir
            return 0
        self.cli.run = stub_run

    def make_run(self, name, **manifest):
        d = os.path.join(self.dir, name)
        os.makedirs(d)
        R.write_manifest(os.path.join(d, 'manifest.tsv'), {f: 'x' for f in R.COMPAT_FIELDS} | manifest)
        rows = [dict(policy='cas', card='cycle', phase='cold', repeat=1, metric='wall', value=2.0, unit_count=1)]
        R.write_summary(os.path.join(d, 'summary.tsv'), R.build_summary(rows))
        return d

    def set_current(self, text):
        with open(os.path.join(self.dir, 'reference', 'CURRENT'), 'w') as f:
            f.write(text)

    def main(self, *argv):
        import unittest.mock
        out, err = io.StringIO(), io.StringIO()
        with unittest.mock.patch('sys.argv', ['yardstick.py', *argv]), contextlib.redirect_stdout(out), \
                contextlib.redirect_stderr(err):
            try:
                code = self.cli.main()
            except SystemExit as e:
                code = e.code
        return code, out.getvalue(), err.getvalue()

    def test_run_compare_uses_current(self):
        self.make_run('ref')
        self.set_current('../ref\n')
        code, out, _ = self.main('run', '--binary', 'b', '--compare')
        self.assertEqual(code, 0)
        self.assertIn('cas cycle cold wall: cur 2.0', out)

    def test_run_compare_with_empty_current_fails_after_the_run(self):
        self.set_current('')
        code, out, err = self.main('run', '--binary', 'b', '--compare')
        self.assertEqual(len(self.ran), 1)
        self.assertNotEqual(code, 0)
        self.assertIn('no reference', err)

    def test_run_compare_with_explicit_reference(self):
        code, out, _ = self.main('run', '--binary', 'b', '--compare', '--reference', self.make_run('ref2'))
        self.assertEqual(code, 0)
        self.assertIn('cas cycle cold wall', out)

    def test_compare_with_explicit_dir(self):
        code, out, _ = self.main('compare', self.run_dir, self.make_run('ref3', scale='2'))
        self.assertEqual((code, out), (1, 'incompatible: scale\n'))

    def test_compare_with_empty_current_exits_with_message(self):
        self.set_current('')
        code, _, _ = self.main('compare', self.run_dir)
        self.assertIn('no reference', str(code))

    def test_missing_reference_dir_exits_with_message(self):
        self.set_current('nowhere')
        code, _, _ = self.main('compare', self.run_dir)
        self.assertIn('no reference', str(code))


class _Client:
    def __init__(self, inner):
        self.inner = inner

    def query(self, sql, settings=None, timeout=3600):
        return self.inner.query(sql)

    def events(self):
        return {}

    def settle(self, table, timeout=60):
        pass

    def drop_caches(self):
        pass


if __name__ == '__main__':
    unittest.main()
