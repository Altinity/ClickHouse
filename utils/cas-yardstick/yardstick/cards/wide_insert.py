"""500 inserts of 20000 rows into a 30-column table, merges stopped."""
import time

from .base import DB, NO_MERGES, SINGLE_PART, Card, parts_and_rows, table_engine

COLUMNS = 15
# The hot pass shifts every value, so it writes new content instead of repeating the cold pass's blobs.
VALUE_OFFSET = {'cold': 0, 'hot': 1}


class WideInsert(Card):
    name = 'wide_insert'
    unit = 'insert'
    stops_merges = True
    COUNTS = {'inserts': 500, 'rows_per_insert': 20000}

    def cold_reset_needed(self):
        return False

    def setup(self, c, policy, counts):
        c.query(f'CREATE DATABASE IF NOT EXISTS {DB}')
        columns = ', '.join([f'u{i} UInt64' for i in range(COLUMNS)] + [f's{i} String' for i in range(COLUMNS)])
        c.query(f'CREATE TABLE {self.table} (k UInt64, {columns}) {table_engine(policy, NO_MERGES)}')

    def before_window(self, c, phase):
        if phase == 'hot':
            c.query(f'TRUNCATE TABLE {self.table}')

    def measured(self, c, policy, counts, phase):
        rows = counts['rows_per_insert']
        o = VALUE_OFFSET[phase]
        numeric = ', '.join(f'number * {j} + {o}' for j in range(COLUMNS))
        strings = ', '.join(f'toString(number * {j} + {o})' for j in range(COLUMNS))
        ops = []
        for i in range(counts['inserts']):
            a = time.monotonic()
            c.query(f'INSERT INTO {self.table} SELECT number + {o}, {numeric}, {strings} '
                    f'FROM numbers({i * rows}, {rows})', SINGLE_PART)
            ops.append(dict(insert=i, ms=(time.monotonic() - a) * 1e3))
        return {'unit_count': len(ops), 'ops': ops}

    def invariants(self, c, counts, phase, when):
        parts, rows = parts_and_rows(c, self.table)
        inserts = counts['inserts'] if when == 'after' else 0
        return {'active_parts': (inserts, parts), 'rows': (inserts * counts['rows_per_insert'], rows)}
