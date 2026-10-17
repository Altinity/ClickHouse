"""Create / 3 x insert / select / drop, from concurrent workers."""
import time

from .base import DB, Card, run_workers, table_engine

WORKERS = 8
INSERTS = 3


class Cycle(Card):
    name = 'cycle'
    unit = 'cycle'
    stops_merges = False
    COUNTS = {'iterations': 25, 'rows_per_insert': 1000}

    def __init__(self):
        self._mismatches = {}

    @property
    def table(self) -> str:
        return f'{DB}.*'

    def setup(self, c, policy, counts):
        c.query(f'CREATE DATABASE IF NOT EXISTS {DB}')

    def cold_reset_needed(self):
        return False

    def _iterations(self, c, policy, phase, w, counts, stop):
        rows = counts['rows_per_insert']
        ops = []
        for i in range(counts['iterations']):
            if stop.is_set():
                break
            t = f'{DB}.t_{phase}_{w}_{i}'
            a = time.monotonic()
            c.query(f'CREATE TABLE {t} (k UInt64, s String) {table_engine(policy)}')
            b = time.monotonic()
            for j in range(INSERTS):
                c.query(f'INSERT INTO {t} SELECT number, toString(number) FROM numbers({j * rows}, {rows})')
            d = time.monotonic()
            count, total = (int(x) for x in c.query(f'SELECT count(), sum(k) FROM {t}').split())
            e = time.monotonic()
            c.query(f'DROP TABLE {t} SYNC')
            f = time.monotonic()
            ops.append(dict(worker=w, iteration=i, create_ms=(b - a) * 1e3, insert_ms=(d - b) * 1e3,
                            select_ms=(e - d) * 1e3, drop_ms=(f - e) * 1e3, ms=(f - a) * 1e3,
                            count=count, sum=total))
        return ops

    def measured(self, c, policy, counts, phase):
        per_worker = run_workers(WORKERS, lambda w, stop: self._iterations(c, policy, phase, w, counts, stop))
        ops = [op for worker_ops in per_worker for op in worker_ops]
        n = INSERTS * counts['rows_per_insert']
        expected = (n, n * (n - 1) // 2)
        self._mismatches[phase] = sum((op['count'], op['sum']) != expected for op in ops)
        return {'unit_count': len(ops), 'ops': ops}

    def invariants(self, c, counts, phase, when):
        tables = int(c.query(f"SELECT count() FROM system.tables WHERE database = '{DB}'"))
        out = {'tables': (0, tables)}
        if when == 'after':
            out['select_count_sum_mismatches'] = (0, self._mismatches.pop(phase))
        return out
