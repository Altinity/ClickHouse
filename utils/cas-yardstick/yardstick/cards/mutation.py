"""One ALTER ... UPDATE rewriting a column in each of 100 parts."""
import time

from .base import DB, NO_MERGES, SINGLE_PART, Card, parts_and_rows, supports_mutations, table_engine

PASS_NO = {'cold': 1, 'hot': 2}


class Mutation(Card):
    name = 'mutation'
    unit = 'part'
    stops_merges = True
    COUNTS = {'parts': 100, 'rows_per_part': 10000}

    def supports(self, policy):
        return supports_mutations(policy)

    def cold_reset_needed(self):
        return True

    def setup(self, c, policy, counts):
        c.query(f'CREATE DATABASE IF NOT EXISTS {DB}')
        c.query(f'CREATE TABLE {self.table} (k UInt64, s String) {table_engine(policy, NO_MERGES)}')
        rows = counts['rows_per_part']
        for i in range(counts['parts']):
            c.query(f'INSERT INTO {self.table} SELECT number, toString(number) '
                    f'FROM numbers({i * rows}, {rows})', SINGLE_PART)

    def measured(self, c, policy, counts, phase):
        a = time.monotonic()
        c.query(f'ALTER TABLE {self.table} UPDATE s = toString(k + {PASS_NO[phase]}) WHERE 1',
                {'mutations_sync': 2})
        return {'unit_count': counts['parts'], 'ops': [dict(ms=(time.monotonic() - a) * 1e3)]}

    def invariants(self, c, counts, phase, when):
        parts, rows = parts_and_rows(c, self.table)
        out = {'active_parts': (counts['parts'], parts), 'rows': (counts['parts'] * counts['rows_per_part'], rows)}
        if when == 'after':
            # Reads column s, so it runs only after the window.
            wrong = int(c.query(f'SELECT count() FROM {self.table} WHERE s != toString(k + {PASS_NO[phase]})'))
            out['wrong_values'] = (0, wrong)
        return out
