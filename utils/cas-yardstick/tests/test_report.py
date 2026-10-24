import unittest, tempfile, os
from yardstick import report as R

class Aggregates(unittest.TestCase):
    def test_delta_and_aggregate(self):
        before = {'DiskS3GetObject': 10, 'DiskS3PutObject': 5, 'DiskS3UploadPart': 0, 'DiskS3CreateMultipartUpload': 0,
                  'DiskS3CompleteMultipartUpload': 0, 'ReadBufferFromS3Bytes': 100}
        after = dict(before, DiskS3GetObject=93, DiskS3PutObject=60, DiskS3UploadPart=4, DiskS3CreateMultipartUpload=2,
                     DiskS3CompleteMultipartUpload=2, ReadBufferFromS3Bytes=600)
        d = R.delta(before, after)
        agg = R.aggregate(d, set(after))
        self.assertEqual(agg['GET'], 83)
        self.assertEqual(agg['PUT'], 55 + 4 + 2 + 2)          # multipart: N parts + create + complete
        self.assertEqual(agg['bytes_read'], 500)
        self.assertEqual(agg['HEAD'], R.UNAVAILABLE)          # DiskS3HeadObject absent from the snapshot
    def test_aggregate_missing_counter(self):
        agg = R.aggregate({'DiskS3GetObject': 1}, {'DiskS3GetObject'})
        self.assertEqual(agg['GET'], 1)
        self.assertEqual(agg['PUT'], R.UNAVAILABLE)

class Stats(unittest.TestCase):
    def test_summarize(self):
        self.assertEqual(R.summarize([3.0, 1.0, 2.0]), (2.0, 1.0, 3.0))
    def test_paired_ratios(self):
        self.assertEqual(R._ratios([83, 84, 83], [12, 12, 12]), (83 / 12, 83 / 12, 84 / 12))
        self.assertEqual(R._ratios([0, 0, 0], [0, 0, 0]), '0 → 0')
        self.assertEqual(R._ratios([5, 5, 5], [0, 0, 0]), '0 → 5')
        self.assertEqual(R._ratios([2392900, 2392900], [0, 0]), '0 → 2392900')
    def test_marks(self):
        self.assertEqual(R.mark_requests([83, 83, 83]), '')
        self.assertEqual(R.mark_requests([83, 84, 83]), '~')
        self.assertEqual(R.mark_spread([1.0, 1.1, 1.0]), '')
        self.assertEqual(R.mark_spread([1.0, 1.3, 1.0]), 'noisy')

    def test_bytes_spread_is_marked_noisy(self):
        rows = rows_for('cas', 'bytes_written', [1264897, 1479089048, 1264897])
        [r] = R.build_summary(rows)
        self.assertEqual(r['mark'], 'noisy')
    def test_per_unit(self):
        self.assertAlmostEqual(R.per_unit(16600, 200), 83.0)

class Files(unittest.TestCase):
    def test_samples_summary_roundtrip_and_table(self):
        fixture = os.path.join(os.path.dirname(__file__), 'fixtures', 'rca_2026_09_28_samples.tsv')
        rows = R.read_samples(fixture)                       # cas GET 83/cycle, PUT 64; s3 GET 12, PUT 31; 800 cycles; 3 repeats
        with tempfile.TemporaryDirectory() as d:
            p = os.path.join(d, 'samples.tsv'); R.write_samples(p, rows)
            self.assertEqual(R.read_samples(p), rows)
            summary = R.build_summary(rows)
            get_cas = [r for r in summary if r['policy'] == 'cas' and r['metric'] == 'GET'][0]
            self.assertEqual((get_cas['median'], get_cas['min'], get_cas['max'], get_cas['repeats']), (66400, 66400, 66400, 3))
            self.assertAlmostEqual(get_cas['per_unit'], 83.0)
            s = os.path.join(d, 'summary.tsv'); R.write_summary(s, summary)
            self.assertEqual(R.read_summary(s), summary)
            table = R.render_table(summary, ['cas', 's3', 's3plain'], [])
            self.assertIn('cycle', table); self.assertIn('cas/s3', table); self.assertIn('6.92', table)   # 83/12
            self.assertIn('median_op_ms', table)              # per-operation latency columns next to wall
    def test_diagnostics(self):
        rows = [dict(policy='cas', card='cycle', phase='cold', repeat=1, metric='CASRootGet', value=5, unit_count=1),
                dict(policy='s3', card='cycle', phase='cold', repeat=1, metric='CASRootGet', value=0, unit_count=1)]
        out = R.render_diagnostics(R.build_summary(rows))
        self.assertIn('CASRootGet', out); self.assertNotIn('\ns3', out)   # CAS policies only
    def test_unsupported_and_failed_cells(self):
        rows = [dict(policy='s3plain', card='mutation', phase='cold', repeat=1, metric='status', value='UNSUPPORTED (write-once)', unit_count=0),
                dict(policy='cas', card='mutation', phase='cold', repeat=1, metric='status', value='FAILED', unit_count=0)]
        table = R.render_table(R.build_summary(rows), ['cas', 's3', 's3plain'], [])
        self.assertIn('UNSUPPORTED (write-once)', table); self.assertIn('FAILED', table)

class Compare(unittest.TestCase):
    def manifest(self, **over):
        m = dict(workload_hash='abc', scale='1', counts='cycle=200', gc='on', repeats='3', config_hash='h1', build_type='Release', store='rustfs/rustfs:1.0.0-rc.3', store_version='unknown')
        m.update(over); return m
    def test_compare_incompatible(self):
        self.assertEqual(R.compatible(self.manifest(), self.manifest(scale='0.5', repeats='1')), ['scale', 'repeats'])
        self.assertEqual(R.compatible(self.manifest(), self.manifest(store_version='2')), ['store_version'])
        self.assertEqual(R.compatible(self.manifest(), self.manifest()), [])
    def test_manifest_roundtrip(self):
        with tempfile.TemporaryDirectory() as d:
            p = os.path.join(d, 'manifest.tsv'); m = self.manifest()
            R.write_manifest(p, m)
            self.assertEqual(R.read_manifest(p), m)
    def test_render_compare_marks(self):
        cur = [dict(policy='cas', card='cycle', phase='cold', metric='wall', median=3.5, min=3.4, max=3.6, repeats=3, per_unit=0.0175, mark=''),
               dict(policy='cas', card='cycle', phase='cold', metric='GET', median=16800, min=16800, max=16800, repeats=3, per_unit=84, mark='')]
        ref = [dict(cur[0], median=3.0, min=2.9, max=3.1), dict(cur[1], median=16600, min=16600, max=16600, per_unit=83)]
        out = R.render_compare(cur, ref)
        self.assertIn('+16.7%', out); self.assertIn('wall', out); self.assertIn('GET', out)
        self.assertRegex(out, r'wall.*!')      # |Δ wall| > 10 % is marked
        self.assertRegex(out, r'GET.*!')       # a request-median change is marked

def rows_for(policy, metric, values, card='cycle', phase='cold', unit_count=200):
    return [dict(policy=policy, card=card, phase=phase, repeat=i + 1, metric=metric, value=v, unit_count=unit_count)
            for i, v in enumerate(values)]

class Pairing(unittest.TestCase):
    def rows(self):
        return (rows_for('cas', 'GET', [83, 84, 83]) + rows_for('s3', 'GET', [12, 12, 12]) +
                rows_for('cas', 'wall', [3.0, 3.1, 3.2]) + rows_for('s3', 'wall', [1.0, 1.0, 1.0]) +
                rows_for('cas', 'median_op_ms', [12.5, 12.5, 12.5]) + rows_for('s3', 'median_op_ms', [4.0, 4.0, 4.0]) +
                rows_for('cas', 'bytes_read', [3 << 20] * 3) + rows_for('s3', 'bytes_read', [1 << 19] * 3) +
                rows_for('cas', 'bytes_written', [1 << 21] * 3) + rows_for('s3', 'bytes_written', [1 << 20] * 3))
    def test_ratio_rows_are_per_repeat(self):
        summary = R.build_summary(self.rows())
        r = [x for x in summary if x['metric'] == 'ratio:cas/s3:GET'][0]
        self.assertEqual((r['policy'], r['min'], r['max']), ('cas/s3', 83 / 12, 84 / 12))
        self.assertAlmostEqual(r['median'], 83 / 12)
        self.assertEqual([x for x in summary if x['metric'] == 'ratio:cas/s3plain:GET'], [])   # s3plain has no rows
    def test_table_shows_ranges_ratios_values(self):
        table = R.render_table(R.build_summary(self.rows()), ['cas', 's3', 's3plain'], [])
        self.assertIn('6.92 (6.92..7.00)', table)            # ratio with range
        self.assertIn('83 (83..84)~', table)                  # cell range plus the request-variation mark
        self.assertIn('12.50', table)                         # median_op_ms rendered with a value
        self.assertIn('3.0', table)                           # MiB_r cell of cas; cannot match a wall cell
        self.assertIn('per unit', table); self.assertIn('GET=0.415', table)

    def test_per_unit_line_names_the_unit(self):
        table = R.render_table(R.build_summary(self.rows()), ['cas', 's3'], [], {'cycle': 'cycle'})
        self.assertIn('per cycle cycle/cold cas: ', table)
    def test_zero_denominator_and_roundtrip(self):
        rows = rows_for('cas', 'PUT', [5, 5, 5]) + rows_for('s3', 'PUT', [0, 0, 0])
        summary = R.build_summary(rows)
        self.assertEqual([x for x in summary if x['metric'] == 'ratio:cas/s3:PUT'][0]['median'], '0 → 5')
        self.assertIn('0 → 5', R.render_table(summary, ['cas', 's3'], []))
        with tempfile.TemporaryDirectory() as d:
            p = os.path.join(d, 's.tsv'); R.write_summary(p, summary)
            self.assertEqual(R.read_summary(p), summary)
    def test_unavailable_suppresses_ratio(self):
        rows = (rows_for('cas', 'GET', [83, R.UNAVAILABLE, 83]) + rows_for('s3', 'GET', [6, 6, 6]) +
                rows_for('cas', 'PUT', [64, 64, 64]) + rows_for('s3', 'PUT', [31, 31, 31]))
        summary = R.build_summary(rows)
        self.assertEqual([x['metric'] for x in summary if x['metric'].startswith('ratio')], ['ratio:cas/s3:PUT'])
        table = R.render_table(summary, ['cas', 's3'], [])
        self.assertIn(R.UNAVAILABLE, table)
        with tempfile.TemporaryDirectory() as d:
            p = os.path.join(d, 's.tsv'); R.write_summary(p, summary)
            self.assertEqual(R.read_summary(p), summary)
    def test_repeat_sets_must_match(self):
        rows = rows_for('cas', 'GET', [1, 2, 3]) + rows_for('s3', 'GET', [1, 2])
        with self.assertRaises(ValueError):
            R.build_summary(rows)
    def test_cached_block_skips_cards_without_rows(self):
        table = R.render_table(R.build_summary(self.rows()), ['cas', 's3'], ['cas_cached'])
        self.assertIn('\nuncached\n', '\n' + table)
        self.assertNotIn('\ncached\n', '\n' + table)
        rows = self.rows() + rows_for('cas_cached', 'GET', [5, 5, 5], card='warm')
        table = R.render_table(R.build_summary(rows), ['cas', 's3'], ['cas_cached'])
        block = ('\n' + table).split('\ncached\n')[1]
        self.assertIn('warm/cold', block)
        self.assertNotIn('cycle/cold', block)
    def test_scientific_notation_never_printed(self):
        rows = rows_for('cas', 'GET', [16600, 16601], unit_count=1) + rows_for('s3', 'GET', [1, 1], unit_count=1)
        table = R.render_table(R.build_summary(rows), ['cas', 's3'], [])
        self.assertIn('16600.50', table); self.assertNotIn('e+', table)
    def test_cached_pairs_get_ratios(self):
        rows = []
        for policy, get in (('cas', 83), ('s3', 6), ('s3plain', 6), ('cas_cached', 40), ('s3_cached', 4),
                            ('s3plain_cached', 5)):
            rows += rows_for(policy, 'GET', [get] * 3)
        metrics = {x['metric'] for x in R.build_summary(rows) if x['metric'].startswith('ratio')}
        self.assertEqual(metrics, {'ratio:cas/s3:GET', 'ratio:cas/s3plain:GET', 'ratio:cas_cached/s3_cached:GET',
                                   'ratio:cas_cached/s3plain_cached:GET'})
        table = R.render_table(R.build_summary(rows), ['cas', 's3', 's3plain'], ['cas_cached', 's3_cached'])
        self.assertIn('cas_cached/s3_cached', table.split('\ncached\n')[1])
        self.assertIn('10.00 (10.00..10.00)', table)

    def test_failed_repeat_leaves_no_numbers(self):
        rows = (rows_for('cas', 'GET', [500, 500]) + rows_for('cas', 'wall', [1.0, 1.0]) +
                [dict(policy='cas', card='cycle', phase='cold', repeat=3, metric='status', value='FAILED', unit_count=0)] +
                rows_for('s3', 'GET', [6, 6, 6]))
        summary = R.build_summary(rows)
        cas = [(x['metric'], x['median']) for x in summary if x['policy'] == 'cas']
        self.assertEqual(cas, [('status', 'FAILED (2 of 3 repeats ok)')])
        self.assertEqual([x['repeats'] for x in summary if x['policy'] == 'cas'], [3])
        self.assertEqual([x for x in summary if x['metric'].startswith('ratio')], [])
        table = R.render_table(summary, ['cas', 's3'], [], {'cycle': 'cycle'})
        self.assertIn('FAILED (2 of 3 repeats ok)', table)
        self.assertNotIn('cycle/cold cas:', table)

    def test_interrupted_run_drops_unpaired_ratios(self):
        rows = rows_for('cas', 'GET', [1, 2]) + rows_for('s3', 'GET', [1, 2, 3]) + \
            rows_for('cas', 'PUT', [4, 4, 4]) + rows_for('s3', 'PUT', [2, 2, 2])
        metrics = [x['metric'] for x in R.build_summary(rows, strict=False) if x['metric'].startswith('ratio')]
        self.assertEqual(metrics, ['ratio:cas/s3:PUT'])

    def test_header_states_unavailable_and_gc(self):
        rows = rows_for('cas', 'HEAD', [R.UNAVAILABLE]) + rows_for('cas', 'GET', [1])
        header = R.render_header(dict(scale='1.0', repeats='3', gc='off'), R.build_summary(rows))
        self.assertIn('GC-disabled process window', header)
        self.assertIn('unavailable: cas cycle cold HEAD', header)
        header = R.render_header(dict(scale='1.0', repeats='3', gc='on'), R.build_summary(rows[1:]))
        self.assertNotIn('GC-disabled', header)
        self.assertIn('unavailable: none', header)

    def test_diagnostics_s3_errors_and_empty(self):
        rows = rows_for('cas', 'S3ReadRequestsErrors', [2, 2, 2], unit_count=1)
        self.assertIn('S3ReadRequestsErrors', R.render_diagnostics(R.build_summary(rows)))
        self.assertEqual(R.render_diagnostics([]).strip(), '')

class Edges(unittest.TestCase):
    def test_paired_ratio_length_mismatch(self):
        with self.assertRaises(ValueError):
            R._ratios([1, 2], [1])
        with self.assertRaises(ValueError):
            R._ratios([], [])
    def test_mark_spread_zero_min(self):
        self.assertEqual(R.mark_spread([0.0, 1.0]), 'noisy')
    def test_compatible_missing_field(self):
        m = dict(workload_hash='a')
        self.assertIn('scale', R.compatible(m, m))
    def test_tsv_rejects_tab_and_newline(self):
        with tempfile.TemporaryDirectory() as d:
            for bad in ('a\tb', 'a\nb'):
                with self.assertRaises(ValueError):
                    R.write_manifest(os.path.join(d, 'm.tsv'), dict(gc=bad))
    def test_compare_negative_and_missing(self):
        def row(metric, median):
            return dict(policy='cas', card='cycle', phase='cold', metric=metric, median=median, min=median, max=median,
                        repeats=3, per_unit='', mark='')
        cur = [row('wall', 3.1), row('GET', 100), row('PUT', 7)]
        ref = [row('wall', 3.0), row('GET', 100), row('HEAD', 1)]
        lines = {l.split(': ')[0]: l for l in R.render_compare(cur, ref).splitlines()}
        self.assertNotIn('!', lines['cas cycle cold wall'])
        self.assertNotIn('!', lines['cas cycle cold GET'])
        self.assertRegex(lines['cas cycle cold PUT'], r'no reference.*!')
        self.assertRegex(lines['cas cycle cold HEAD'], r'no current.*!')
    def test_compare_text_vs_number_flagged(self):
        a = dict(policy='cas', card='m', phase='cold', metric='status', median='FAILED', min='FAILED', max='FAILED',
                 repeats=1, per_unit='', mark='')
        b = dict(a, median=1, min=1, max=1)
        self.assertIn('!', R.render_compare([a], [b]))

if __name__ == '__main__':
    unittest.main()
