"""ATTACH of a table with 200 small parts that no process has loaded yet, then DETACH + ATTACH."""
import time

from .base import DB, NO_MERGES, SINGLE_PART, Card, parts_and_rows, table_engine


class Attach200(Card):
    name = 'attach_200'
    unit = 'part'
    stops_merges = True
    COUNTS = {'parts': 200, 'rows_per_part': 10000}

    def cold_reset_needed(self):
        return True

    def setup(self, c, policy, counts):
        c.query(f'CREATE DATABASE IF NOT EXISTS {DB}')
        c.query(f'CREATE TABLE {self.table} (k UInt64, s String) {table_engine(policy, NO_MERGES)}')
        rows = counts['rows_per_part']
        for i in range(counts['parts']):
            c.query(f'INSERT INTO {self.table} SELECT number, toString(number) '
                    f'FROM numbers({i * rows}, {rows})', SINGLE_PART)
        # A permanently detached table is not loaded by the restart, so the cold ATTACH is the first load.
        c.query(f'DETACH TABLE {self.table} PERMANENTLY')

    def measured(self, c, policy, counts, phase):
        a = time.monotonic()
        if phase == 'hot':
            c.query(f'DETACH TABLE {self.table}')
        b = time.monotonic()
        c.query(f'ATTACH TABLE {self.table}')
        d = time.monotonic()
        op = dict(detach_ms=(b - a) * 1e3, attach_ms=(d - b) * 1e3, ms=(d - a) * 1e3)
        return {'unit_count': counts['parts'], 'ops': [op]}

    def invariants(self, c, counts, phase, when):
        db, name = self.table.split('.', 1)
        if (phase, when) == ('cold', 'before'):
            attached = int(c.query(f"SELECT count() FROM system.tables WHERE database = '{db}' AND name = '{name}'"))
            return {'attached_tables': (0, attached)}
        parts, rows = parts_and_rows(c, self.table)
        return {'active_parts': (counts['parts'], parts), 'rows': (counts['parts'] * counts['rows_per_part'], rows)}
