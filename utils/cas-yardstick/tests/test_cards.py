import re
import unittest

from yardstick.cards import CARDS
from yardstick.config import POLICIES

EXPECTED_COUNTS = {
    'cycle': ({'iterations': 25, 'rows_per_insert': 1000}, {'iterations': 1, 'rows_per_insert': 10}),
    'wide_insert': ({'inserts': 500, 'rows_per_insert': 20000}, {'inserts': 5, 'rows_per_insert': 200}),
    'attach_200': ({'parts': 200, 'rows_per_part': 10000}, {'parts': 2, 'rows_per_part': 100}),
    'mutation': ({'parts': 100, 'rows_per_part': 10000}, {'parts': 1, 'rows_per_part': 100}),
    'lwd_concurrent': ({'parts': 50, 'rows_per_part': 100000}, {'parts': 1, 'rows_per_part': 1000}),
    'select_subset': ({'parts': 200, 'rows_per_part': 50000}, {'parts': 2, 'rows_per_part': 500}),
    'dup_content': ({'tables': 20, 'rows': 1000000}, {'tables': 1, 'rows': 10000}),
}
FULL = {name: full for name, (full, _) in EXPECTED_COUNTS.items()}
FLAGS = {
    # name: (unit, has_hot, stops_merges, cold_reset_needed)
    'cycle': ('cycle', True, False, False),
    'wide_insert': ('insert', True, True, False),
    'attach_200': ('part', True, True, True),
    'mutation': ('part', True, True, True),
    'lwd_concurrent': ('delete', True, False, True),
    'select_subset': ('query', True, True, True),
    'dup_content': ('table', True, False, False),
}


class FakeClient:
    def __init__(self, reply=lambda sql: ''):
        self.calls = []
        self.reply = reply

    def query(self, sql, settings=None, timeout=3600):
        self.calls.append((sql, settings or {}))
        return self.reply(sql)

    def sqls(self, prefix):
        return [(s, st) for s, st in self.calls if s.startswith(prefix)]


class RegistryTest(unittest.TestCase):
    def test_spec_order(self):
        self.assertEqual(list(CARDS), list(EXPECTED_COUNTS))

    def test_counts(self):
        for name, (full, small) in EXPECTED_COUNTS.items():
            self.assertEqual(CARDS[name].counts(1.0), full, name)
            self.assertEqual(CARDS[name].counts(0.01), small, name)

    def test_flags(self):
        for name, (unit, has_hot, stops_merges, cold_reset) in FLAGS.items():
            card = CARDS[name]
            self.assertEqual((card.unit, card.has_hot, card.stops_merges, card.cold_reset_needed()),
                             (unit, has_hot, stops_merges, cold_reset), name)

    def test_support_matrix(self):
        mutating = {'mutation', 'lwd_concurrent'}
        for card in CARDS.values():
            for policy in POLICIES:
                ok, reason = card.supports(policy)
                immutable = card.name in mutating and policy.startswith('s3plain')
                self.assertEqual(ok, not immutable, (card.name, policy))
                self.assertEqual(bool(reason), immutable, (card.name, policy))


class SetupTest(unittest.TestCase):
    def test_table_settings_and_merge_stop(self):
        for name in ('wide_insert', 'attach_200', 'mutation', 'lwd_concurrent', 'select_subset'):
            card = CARDS[name]
            c = FakeClient()
            card.setup(c, 's3', card.counts(0.01))
            create = c.sqls('CREATE TABLE')[0][0]
            self.assertIn("storage_policy = 's3'", create, name)
            for setting in ('primary_key_lazy_load = 0', 'prewarm_mark_cache = 0', 'prewarm_primary_key_cache = 0'):
                self.assertIn(setting, create, name)
            self.assertEqual('max_parts_to_merge_at_once = 1' in create, card.stops_merges, name)
            self.assertEqual(c.sqls('SYSTEM STOP MERGES'), [], name)

    def test_before_window_only_prepares_wide_insert_hot(self):
        for name, card in CARDS.items():
            for phase in ('cold', 'hot'):
                c = FakeClient()
                card.before_window(c, phase)
                expected = ['TRUNCATE TABLE yardstick.wide_insert'] if (name, phase) == ('wide_insert', 'hot') else []
                self.assertEqual([s for s, _ in c.calls], expected, (name, phase))

    def test_per_card_table_settings(self):
        def create(name):
            c = FakeClient()
            CARDS[name].setup(c, 'cas', CARDS[name].counts(0.01))
            return c.sqls('CREATE TABLE')[0][0]
        self.assertIn('enable_block_number_column = 1, enable_block_offset_column = 1', create('lwd_concurrent'))
        self.assertIn('min_bytes_for_wide_part = 1073741824', create('select_subset'))

    def test_wide_table_has_thirty_data_columns(self):
        c = FakeClient()
        CARDS['wide_insert'].setup(c, 'cas', CARDS['wide_insert'].counts(0.01))
        create = c.sqls('CREATE TABLE')[0][0]
        self.assertEqual(len(re.findall(r'\bu\d+ UInt64', create)), 15)
        self.assertEqual(len(re.findall(r'\bs\d+ String', create)), 15)

    def test_setup_parts_are_single_stream_inserts(self):
        for name in ('attach_200', 'mutation', 'lwd_concurrent', 'select_subset'):
            c = FakeClient()
            CARDS[name].setup(c, 'cas', {'parts': 2, 'rows_per_part': 160})
            inserts = c.sqls('INSERT')
            self.assertEqual(len(inserts), 2, name)
            self.assertIn('FROM numbers(160, 160)', inserts[1][0], name)
            self.assertTrue(all(st.get('max_insert_threads') == 1 for _, st in inserts), name)


class WideInsertTest(unittest.TestCase):
    def test_measured_issues_one_insert_per_part(self):
        card = CARDS['wide_insert']
        c = FakeClient()
        result = card.measured(c, 'cas', FULL['wide_insert'], 'cold')
        inserts = c.sqls('INSERT')
        self.assertEqual(len(inserts), 500)
        self.assertEqual(result['unit_count'], 500)
        self.assertEqual(len(result['ops']), 500)
        self.assertIn('FROM numbers(9980000, 20000)', inserts[-1][0])
        self.assertTrue(all(st == {'max_insert_threads': 1} for _, st in inserts))

    def test_columns_follow_the_column_index(self):
        c = FakeClient()
        CARDS['wide_insert'].measured(c, 'cas', {'inserts': 2, 'rows_per_insert': 10}, 'cold')
        sql = c.sqls('INSERT')[1][0]
        self.assertIn('SELECT number + 0, number * 0 + 0, number * 1 + 0,', sql)
        self.assertIn('number * 14 + 0, toString(number * 0 + 0)', sql)
        self.assertIn('toString(number * 14 + 0) FROM numbers(10, 10)', sql)

    def test_hot_pass_inserts_different_values(self):
        cold, hot = FakeClient(), FakeClient()
        CARDS['wide_insert'].measured(cold, 'cas', {'inserts': 3, 'rows_per_insert': 10}, 'cold')
        CARDS['wide_insert'].measured(hot, 'cas', {'inserts': 3, 'rows_per_insert': 10}, 'hot')
        for (a, _), (b, _) in zip(cold.sqls('INSERT'), hot.sqls('INSERT')):
            self.assertNotEqual(a, b)
            self.assertEqual(a.replace(' + 0', ' + 1'), b)

    def test_truncate_runs_before_the_hot_window_not_in_it(self):
        card = CARDS['wide_insert']
        c = FakeClient()
        card.before_window(c, 'cold')
        self.assertEqual(c.sqls('TRUNCATE'), [])
        card.before_window(c, 'hot')
        self.assertEqual(len(c.sqls('TRUNCATE TABLE yardstick.wide_insert')), 1)
        card.measured(c, 'cas', {'inserts': 3, 'rows_per_insert': 10}, 'hot')
        self.assertEqual(len(c.sqls('TRUNCATE')), 1)

    def test_invariants(self):
        card = CARDS['wide_insert']
        c = FakeClient(lambda sql: '500\t10000000')
        self.assertEqual(card.invariants(c, FULL['wide_insert'], 'cold', 'after'),
                         {'active_parts': (500, 500), 'rows': (10_000_000, 10_000_000)})
        self.assertEqual(card.invariants(FakeClient(lambda s: '0\t0'), FULL['wide_insert'], 'hot', 'before'),
                         {'active_parts': (0, 0), 'rows': (0, 0)})


class AttachTest(unittest.TestCase):
    def test_setup_leaves_the_table_detached_for_the_restart(self):
        c = FakeClient()
        CARDS['attach_200'].setup(c, 'cas', FULL['attach_200'])
        self.assertEqual(c.calls[-1][0], 'DETACH TABLE yardstick.attach_200 PERMANENTLY')

    def test_cold_attaches_hot_detaches_then_attaches(self):
        for phase, expected in (('cold', ['ATTACH TABLE yardstick.attach_200']),
                                ('hot', ['DETACH TABLE yardstick.attach_200', 'ATTACH TABLE yardstick.attach_200'])):
            c = FakeClient()
            result = CARDS['attach_200'].measured(c, 'cas', FULL['attach_200'], phase)
            self.assertEqual([s for s, _ in c.calls], expected, phase)
            self.assertEqual(result['unit_count'], 200)
            self.assertEqual(len(result['ops']), 1)

    def test_invariants(self):
        card = CARDS['attach_200']
        c = FakeClient(lambda sql: '200\t2000000')
        for phase, when in (('cold', 'after'), ('hot', 'before'), ('hot', 'after')):
            self.assertEqual(card.invariants(c, FULL['attach_200'], phase, when),
                             {'active_parts': (200, 200), 'rows': (2_000_000, 2_000_000)}, (phase, when))

    def test_cold_before_expects_the_table_detached(self):
        c = FakeClient(lambda sql: '0')
        self.assertEqual(CARDS['attach_200'].invariants(c, FULL['attach_200'], 'cold', 'before'),
                         {'attached_tables': (0, 0)})
        self.assertIn('system.tables', c.calls[0][0])


class MutationTest(unittest.TestCase):
    def test_measured_pass_number_follows_phase(self):
        for phase, n in (('cold', 1), ('hot', 2)):
            c = FakeClient()
            result = CARDS['mutation'].measured(c, 'cas', FULL['mutation'], phase)
            (sql, settings), = c.calls
            self.assertIn(f'UPDATE s = toString(k + {n}) WHERE 1', sql)
            self.assertEqual(settings, {'mutations_sync': 2})
            self.assertEqual(result['unit_count'], 100)

    def test_before_checks_metadata_only_after_checks_values(self):
        card = CARDS['mutation']
        c = FakeClient(lambda sql: '100\t1000000' if 'system.parts' in sql else '0')
        before = card.invariants(c, FULL['mutation'], 'hot', 'before')
        self.assertEqual(set(before), {'active_parts', 'rows'})
        self.assertFalse([s for s, _ in c.calls if 's !=' in s])
        after = card.invariants(c, FULL['mutation'], 'hot', 'after')
        self.assertEqual(after['wrong_values'], (0, 0))
        self.assertTrue([s for s, _ in c.calls if 's != toString(k + 2)' in s])


class LwdTest(unittest.TestCase):
    def deletes(self, phase):
        c = FakeClient()
        result = CARDS['lwd_concurrent'].measured(c, 'cas', FULL['lwd_concurrent'], phase)
        return c, result

    def test_eighty_deletes_cover_half_of_the_residues(self):
        for phase, residues in (('cold', range(0, 10)), ('hot', range(10, 20))):
            c, result = self.deletes(phase)
            deletes = c.sqls('DELETE FROM')
            self.assertEqual(len(deletes), 80)
            self.assertEqual(result['unit_count'], 80)
            seen = {(int(re.search(r'k % 8 = (\d+)', s).group(1)),
                     int(re.search(r'intDiv\(k, 8\) % 20 = (\d+)', s).group(1))) for s, _ in deletes}
            self.assertEqual(seen, {(w, r) for w in range(8) for r in residues})
            self.assertTrue(all(st == {'lightweight_delete_mode': 'lightweight_update_force'}
                                for _, st in deletes))

    def test_row_expectations_by_phase(self):
        card = CARDS['lwd_concurrent']
        for counts, halves in ((FULL['lwd_concurrent'], (5_000_000, 2_500_000)),
                               # 1000 rows: k % 160 < 80 holds for 6 x 80 + 40 of them, so the cold pass deletes 520.
                               ({'parts': 1, 'rows_per_part': 1000}, (1000, 480))):
            for phase, when, expected in (('cold', 'before', halves[0]), ('cold', 'after', halves[1]),
                                          ('hot', 'before', halves[1]), ('hot', 'after', 0)):
                c = FakeClient(lambda sql: "50\t7" if "system.parts" in sql else "7")
                self.assertEqual(card.invariants(c, counts, phase, when)['rows'][0], expected, (counts, phase, when))

    def test_cold_before_reads_no_data(self):
        c = FakeClient(lambda sql: '50\t5000000')
        CARDS['lwd_concurrent'].invariants(c, FULL['lwd_concurrent'], 'cold', 'before')
        self.assertTrue(all('system.parts' in s for s, _ in c.calls))


class SelectSubsetTest(unittest.TestCase):
    def test_measured_and_sum_invariant(self):
        card = CARDS['select_subset']
        counts = FULL['select_subset']
        total = 3 * sum(range(10_000_000))
        c = FakeClient(lambda sql: str(total))
        result = card.measured(c, 'cas', counts, 'cold')
        self.assertEqual([s for s, _ in c.calls], ['SELECT sum(c3) FROM yardstick.select_subset'])
        self.assertEqual(result['unit_count'], 1)
        self.assertEqual(card.invariants(FakeClient(lambda s: '200\t10000000'), counts, 'cold', 'after')['sum_c3'],
                         (total, total))

    def test_wrong_sum_is_reported(self):
        card = CARDS['select_subset']
        card.measured(FakeClient(lambda sql: '1'), 'cas', FULL['select_subset'], 'cold')
        got = card.invariants(FakeClient(lambda s: '200\t10000000'), FULL['select_subset'], 'cold', 'after')['sum_c3']
        self.assertNotEqual(got[0], got[1])


class DupContentTest(unittest.TestCase):
    def test_measured_creates_one_table_per_op(self):
        c = FakeClient()
        result = CARDS['dup_content'].measured(c, 'cas', FULL['dup_content'], 'hot')
        creates = c.sqls('CREATE TABLE')
        self.assertEqual(len(creates), 20)
        self.assertEqual(len(c.sqls('INSERT')), 20)
        self.assertIn('yardstick.dup_content_hot_19 ', creates[-1][0])
        self.assertIn('FROM numbers(1000000)', c.sqls('INSERT')[0][0])
        self.assertEqual(result['unit_count'], 20)

    def test_invariants_count_tables_and_rows_of_all_passes(self):
        card = CARDS['dup_content']
        c = FakeClient(lambda sql: '40\t40\t40000000')
        got = card.invariants(c, FULL['dup_content'], 'hot', 'after')
        self.assertEqual(got, {'tables': (40, 40), 'tables_with_all_rows': (40, 40), 'rows': (40_000_000, 40_000_000)})
        self.assertEqual(set(card.invariants(FakeClient(lambda s: '0\t0\t0'), FULL['dup_content'], 'cold', 'before')),
                         {'tables', 'tables_with_all_rows', 'rows'})

    def test_scale_reduces_the_rows(self):
        c = FakeClient()
        CARDS['dup_content'].measured(c, 'cas', CARDS['dup_content'].counts(0.01), 'cold')
        self.assertEqual([s for s, _ in c.sqls('INSERT')][0].split(' FROM ')[-1], 'numbers(10000)')
        c = FakeClient(lambda s: '1\t1\t10000')
        got = CARDS['dup_content'].invariants(c, CARDS['dup_content'].counts(0.01), 'cold', 'after')
        self.assertEqual(got['rows'], (10000, 10000))
        self.assertIn('total_rows = 10000', c.calls[0][0])


if __name__ == '__main__':
    unittest.main()
