"""20 tables per pass, each created and filled with the same 1M rows."""
import time

from .base import DB, SINGLE_PART, Card, table_engine

PASSES_BEFORE = {'cold': 0, 'hot': 1}


class DupContent(Card):
    name = 'dup_content'
    unit = 'table'
    stops_merges = False
    COUNTS = {'tables': 20, 'rows': 1000000}

    @property
    def table(self) -> str:
        return f'{DB}.*'

    def cold_reset_needed(self):
        return False

    def setup(self, c, policy, counts):
        c.query(f'CREATE DATABASE IF NOT EXISTS {DB}')

    def measured(self, c, policy, counts, phase):
        ops = []
        for i in range(counts['tables']):
            t = f'{DB}.{self.name}_{phase}_{i}'
            a = time.monotonic()
            c.query(f'CREATE TABLE {t} (k UInt64, s String) {table_engine(policy)}')
            b = time.monotonic()
            c.query(f'INSERT INTO {t} SELECT number, toString(number) FROM numbers({counts["rows"]})', SINGLE_PART)
            d = time.monotonic()
            ops.append(dict(table=i, create_ms=(b - a) * 1e3, insert_ms=(d - b) * 1e3, ms=(d - a) * 1e3))
        return {'unit_count': len(ops), 'ops': ops}

    def invariants(self, c, counts, phase, when):
        tables = counts['tables'] * (PASSES_BEFORE[phase] + (when == 'after'))
        per_table = counts['rows']
        text = c.query(f"SELECT count(), countIf(total_rows = {per_table}), ifNull(sum(total_rows), 0) "
                       f"FROM system.tables WHERE database = '{DB}' AND name LIKE '{self.name}\\\\_%'")
        n, full, rows = (int(x) for x in text.split())
        return {'tables': (tables, n), 'tables_with_all_rows': (tables, full), 'rows': (tables * per_table, rows)}
