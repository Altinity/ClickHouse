import os
import shutil
import tempfile
import unittest

from yardstick import report as R
from yardstick import runner
from yardstick.cards.base import Card

GET_PER_CALL = 7
ALL_COUNTERS = sorted({c for cs in R.AGGREGATES.values() for c in cs} | set(R.CAS_DIAGNOSTICS) | set(R.S3_ERROR_COUNTERS))


class FakeClient:
    """Every events() call advances each counter; DiskS3GetObject by GET_PER_CALL."""
    def __init__(self, log=None):
        self.queries = [] if log is None else log
        self.calls = 0

    def query(self, sql, settings=None, timeout=3600):
        self.queries.append(sql)
        return ''

    def events(self):
        self.calls += 1
        return {c: self.calls * (GET_PER_CALL if c == 'DiskS3GetObject' else 1) for c in ALL_COUNTERS}

    def settle(self, table, timeout=60):
        self.queries.append(f'settle {table}')

    def drop_caches(self):
        self.queries.append('drop caches')


class FakeServer:
    def __init__(self, log):
        self.starts = []
        self.stops = []
        self.log = log

    def start(self, run_dir):
        self.starts.append(run_dir)
        self.log.append('start')

    def stop(self, run_dir):
        self.stops.append(run_dir)
        self.log.append('stop')


class FakeCard(Card):
    unit = 'op'
    stops_merges = False
    COUNTS = {'ops': 4}

    def __init__(self, name, has_hot=True, fail_in=None, supported=(True, ''), cold_reset=False,
                 error=RuntimeError('boom'), log=None):
        self.name = name
        self.has_hot = has_hot
        self.fail_in = fail_in
        self.supported = supported
        self.cold_reset = cold_reset
        self.error = error
        self.calls = [] if log is None else log

    def supports(self, policy):
        return self.supported

    def setup(self, c, policy, counts):
        self.calls.append('setup')

    def cold_reset_needed(self):
        return self.cold_reset

    def measured(self, c, policy, counts, phase):
        self.calls.append(f'measured {phase}')
        if self.fail_in == phase:
            raise self.error
        return {'unit_count': counts['ops'], 'ops': [{'n': i, 'ms': 10.0 * (i + 1)} for i in range(counts['ops'])]}

    def invariants(self, c, counts, phase, when):
        return {'rows': (1, 1)}

    def teardown(self, c):
        self.calls.append('teardown')


class RunOneTest(unittest.TestCase):
    def setUp(self):
        self.dir = tempfile.mkdtemp()
        self.addCleanup(shutil.rmtree, self.dir, ignore_errors=True)
        self.log = []
        self.server = FakeServer(self.log)
        self.prefixes = []

        def write_config(run_dir, policy, endpoint, prefix, gc_enabled):
            self.prefixes.append(prefix)
            return 'hash-' + policy

        self.deps = runner.Deps(run_id='R1', runs_dir=self.dir, endpoint='http://127.0.0.1:1', gc=True, scale=1.0,
                                server=self.server, client_for=lambda run_dir: FakeClient(self.log),
                                write_config=write_config, config_hashes={'cas': 'hash-cas', 's3': 'hash-s3'})

    def rows(self, rows, **match):
        return [r for r in rows if all(r[k] == v for k, v in match.items())]

    def test_failed_measured_still_tears_down_and_next_card_runs(self):
        bad = FakeCard('bad', fail_in='cold')
        good = FakeCard('good')
        rows = runner.run_matrix(['cas'], [bad, good], 1, self.deps, [])
        for phase in ('cold', 'hot'):
            self.assertEqual([r['value'] for r in self.rows(rows, card='bad', phase=phase)], ['FAILED'])
        self.assertIn('teardown', bad.calls)
        self.assertEqual(len(self.server.stops), 2)
        self.assertEqual(self.server.starts, self.server.stops)
        self.assertEqual(good.calls, ['setup', 'measured cold', 'measured hot', 'teardown'])
        self.assertTrue(self.rows(rows, card='good', phase='hot', metric='wall'))
        bad_dir = runner.run_dir_for(self.dir, 'R1', 'cas', 'bad', 1, '')
        with open(os.path.join(bad_dir, 'error.txt')) as f:
            self.assertIn('boom', f.read())

    def test_timings_hold_one_entry_per_pass_in_run_order(self):
        timings = []
        runner.run_matrix(['cas', 's3'], [FakeCard('a'), FakeCard('b')], 1, self.deps, [], timings=timings)
        self.assertEqual([(p, c, k) for p, c, k, _ in timings],
                         [('cas', 'a', 1), ('cas', 'b', 1), ('s3', 'a', 1), ('s3', 'b', 1)])
        self.assertTrue(all(isinstance(s, float) and s >= 0 for *_, s in timings))

    def test_pass_seconds_file_has_header_and_one_decimal(self):
        path = os.path.join(self.dir, 'pass_seconds.tsv')
        runner.write_pass_seconds(path, [('cas', 'cycle', 1, 4.349), ('s3', 'cycle', 2, 10.0)])
        with open(path, encoding='utf-8') as f:
            self.assertEqual(f.read(), 'policy\tcard\trepeat\tseconds\ncas\tcycle\t1\t4.3\ns3\tcycle\t2\t10.0\n')

    def test_no_hot_rows_without_hot_pass(self):
        rows = runner.run_one('cas', FakeCard('cold_only', has_hot=False), 1, self.deps)
        self.assertTrue(self.rows(rows, phase='cold', metric='GET'))
        self.assertEqual(self.rows(rows, phase='hot'), [])

    def test_unsupported_rows_and_no_server(self):
        rows = runner.run_one('cas', FakeCard('m', supported=(False, 'write-once')), 1, self.deps)
        self.assertEqual(sorted((r['phase'], r['metric'], r['value']) for r in rows),
                         [('cold', 'status', 'UNSUPPORTED (write-once)'), ('hot', 'status', 'UNSUPPORTED (write-once)')])
        self.assertEqual(self.server.starts, [])

    def test_get_sample_is_the_events_delta(self):
        rows = runner.run_one('s3', FakeCard('c'), 1, self.deps)
        for phase in ('cold', 'hot'):
            [get] = self.rows(rows, phase=phase, metric='GET')
            self.assertEqual((get['value'], get['unit_count'], get['repeat']), (GET_PER_CALL, 4, 1))
        [median] = self.rows(rows, phase='cold', metric='median_op_ms')
        self.assertEqual(median['value'], 25.0)
        self.assertEqual(self.rows(rows, metric='CASRootGet'), [])  # CAS diagnostics on CAS policies only

    def test_prefix_is_fresh_per_repeat(self):
        runner.run_one('cas', FakeCard('c'), 1, self.deps)
        runner.run_one('cas', FakeCard('c'), 2, self.deps)
        self.assertEqual(self.prefixes, ['test/R1/cas/c/k1', 'test/R1/cas/c/k2'])

    def test_config_hash_mismatch_fails_the_pass(self):
        self.deps.config_hashes['cas'] = 'other'
        rows = runner.run_one('cas', FakeCard('c'), 1, self.deps)
        self.assertEqual({r['value'] for r in rows}, {'FAILED'})

    def test_policy_rotation(self):
        self.assertEqual(runner.policy_order(['a', 'b', 'c'], 1), ['a', 'b', 'c'])
        self.assertEqual(runner.policy_order(['a', 'b', 'c'], 2), ['b', 'c', 'a'])
        self.assertEqual(runner.policy_order(['a', 'b', 'c'], 4), ['a', 'b', 'c'])

    def test_hot_failure_keeps_the_cold_rows(self):
        rows = runner.run_one('cas', FakeCard('c', fail_in='hot'), 1, self.deps)
        self.assertTrue(self.rows(rows, phase='cold', metric='GET'))
        self.assertEqual(self.rows(rows, phase='cold', metric='status'), [])
        self.assertEqual([(r['metric'], r['value']) for r in self.rows(rows, phase='hot')], [('status', 'FAILED')])

    def test_teardown_failure_fails_every_phase(self):
        card = FakeCard('c')
        card.teardown = lambda c: (_ for _ in ()).throw(RuntimeError('teardown boom'))
        rows = runner.run_one('cas', card, 1, self.deps)
        self.assertEqual(sorted((r['phase'], r['value']) for r in rows), [('cold', 'FAILED'), ('hot', 'FAILED')])
        self.assertEqual(len(self.server.stops), 1)

    def test_cold_reset_order(self):
        card = FakeCard('c', cold_reset=True, log=self.log)
        rows = runner.run_one('cas', card, 1, self.deps)
        self.assertEqual(self.rows(rows, metric='status'), [])
        self.assertEqual(self.log[:6], ['start', 'setup', 'stop', 'start', 'drop caches', 'settle yardstick.c'])
        pass_dir = runner.run_dir_for(self.dir, 'R1', 'cas', 'c', 1, '')
        self.assertTrue(os.path.isdir(os.path.join(pass_dir, 'caches')))
        self.assertTrue(os.path.isfile(os.path.join(pass_dir, 'cold', 'gc_rounds.tsv')))

    def test_before_window_runs_before_settle_and_before_the_events_snapshot(self):
        class LoggingClient(FakeClient):
            def events(self):
                self.queries.append('events')
                return super().events()

        class PreparingCard(FakeCard):
            def before_window(self, c, phase):
                self.calls.append(f'before_window {phase}')

        card = PreparingCard('c', log=self.log)
        self.deps.client_for = lambda run_dir: LoggingClient(self.log)
        runner.run_one('cas', card, 1, self.deps)
        hot = self.log[self.log.index('before_window hot'):]
        self.assertEqual(hot[:4], ['before_window hot', 'settle yardstick.c', 'events', 'measured hot'])
        cold = self.log[self.log.index('before_window cold'):]
        self.assertEqual(cold[:3], ['before_window cold', 'settle yardstick.c', 'events'])

    def test_interrupt_still_tears_down_stops_and_cleans_up(self):
        cleanups = []
        interrupted = FakeCard('ctrl_c', fail_in='cold', error=KeyboardInterrupt())
        rows, was_interrupted, cleaned = runner.run_all(
            ['cas'], [FakeCard('good'), interrupted, FakeCard('never')], 1, self.deps,
            cleanup=lambda: cleanups.append('rm-prefix') or True)
        self.assertTrue(was_interrupted)
        self.assertTrue(cleaned)
        self.assertEqual(cleanups, ['rm-prefix'])
        self.assertIn('teardown', interrupted.calls)
        self.assertEqual(self.server.starts, self.server.stops)
        self.assertEqual({r['card'] for r in rows}, {'good'})


    def test_unsupported_card_without_hot_pass_has_only_cold_rows(self):
        rows = runner.run_one('cas', FakeCard('m', has_hot=False, supported=(False, 'x')), 1, self.deps)
        self.assertEqual([(r['phase'], r['value']) for r in rows], [('cold', 'UNSUPPORTED (x)')])

    def test_interrupted_teardown_still_stops_the_server(self):
        card = FakeCard('c')
        card.teardown = lambda c: (_ for _ in ()).throw(KeyboardInterrupt())
        with self.assertRaises(KeyboardInterrupt):
            runner.run_one('cas', card, 1, self.deps)
        self.assertEqual(len(self.server.stops), 1)

class SmokeTest(unittest.TestCase):
    def test_failed_and_unavailable_cells(self):
        def row(policy, phase, metric, value):
            return dict(policy=policy, card='cycle', phase=phase, repeat=1, metric=metric, value=value, unit_count=16)
        rows = [row('cas', 'cold', 'status', 'FAILED'), row('cas', 'hot', 'status', 'FAILED'),
                row('s3', 'cold', 'GET', R.UNAVAILABLE), row('s3', 'cold', 'PUT', 31), row('s3', 'hot', 'GET', 6),
                row('s3plain', 'cold', 'status', 'UNSUPPORTED (write-once)'), row('s3', 'cold', 'CASRootGet', R.UNAVAILABLE)]
        failures = runner._smoke_failures(R.build_summary(rows), ['cas', 's3', 's3plain'], [FakeCard('cycle')])
        self.assertEqual(failures, ['cas cycle cold: FAILED', 'cas cycle hot: FAILED', f's3 cycle cold GET: {R.UNAVAILABLE}'])


class CardCountsTest(unittest.TestCase):
    def test_counts_scale_with_floor_of_one(self):
        card = FakeCard('c')
        self.assertEqual(card.counts(1.0), {'ops': 4})
        self.assertEqual(card.counts(0.01), {'ops': 1})


if __name__ == '__main__':
    unittest.main()
