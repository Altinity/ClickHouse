"""80 lightweight deletes from 8 concurrent workers over 50 parts of 100k rows."""
import time

from .base import DB, SINGLE_PART, Card, parts_and_rows, run_workers, supports_mutations, table_engine

WORKERS = 8
DELETES_PER_WORKER = 10
RESIDUES = 20
# Worker w, residue r deletes k % 8 = w AND intDiv(k, 8) % 20 = r; the cold residues 0..9 are k % 160 < 80.
PERIOD = WORKERS * RESIDUES
TABLE_SETTINGS = 'enable_block_number_column = 1, enable_block_offset_column = 1'
# The default mode registers concurrent deletes as mutations out of order and loses some (ClickHouse#122083).
DELETE_SETTINGS = {'lightweight_delete_mode': 'lightweight_update_force'}


class LwdConcurrent(Card):
    name = 'lwd_concurrent'
    unit = 'delete'
    stops_merges = False
    COUNTS = {'parts': 50, 'rows_per_part': 100000}

    def supports(self, policy):
        return supports_mutations(policy)

    def cold_reset_needed(self):
        return True

    def setup(self, c, policy, counts):
        c.query(f'CREATE DATABASE IF NOT EXISTS {DB}')
        c.query(f'CREATE TABLE {self.table} (k UInt64, s String) {table_engine(policy, TABLE_SETTINGS)}')
        rows = counts['rows_per_part']
        for i in range(counts['parts']):
            c.query(f'INSERT INTO {self.table} SELECT number, toString(number) '
                    f'FROM numbers({i * rows}, {rows})', SINGLE_PART)

    def _deletes(self, c, w, offset, stop):
        ops = []
        for i in range(DELETES_PER_WORKER):
            if stop.is_set():
                break
            a = time.monotonic()
            c.query(f'DELETE FROM {self.table} WHERE k % {WORKERS} = {w} '
                    f'AND intDiv(k, {WORKERS}) % {RESIDUES} = {i + offset}', DELETE_SETTINGS)
            ops.append(dict(worker=w, iteration=i, ms=(time.monotonic() - a) * 1e3))
        return ops

    def measured(self, c, policy, counts, phase):
        offset = 0 if phase == 'cold' else DELETES_PER_WORKER
        per_worker = run_workers(WORKERS, lambda w, stop: self._deletes(c, w, offset, stop))
        ops = [op for worker_ops in per_worker for op in worker_ops]
        return {'unit_count': len(ops), 'ops': ops}

    def invariants(self, c, counts, phase, when):
        total = counts['parts'] * counts['rows_per_part']
        cold_deleted = total // PERIOD * (PERIOD // 2) + min(total % PERIOD, PERIOD // 2)
        left = total - cold_deleted
        expected = {('cold', 'before'): total, ('cold', 'after'): left,
                    ('hot', 'before'): left, ('hot', 'after'): 0}[(phase, when)]
        if (phase, when) == ('cold', 'before'):
            # Nothing is deleted yet, so part metadata is exact and no data is read before the cold window.
            _, rows = parts_and_rows(c, self.table)
        else:
            rows = int(c.query(f'SELECT count() FROM {self.table}'))
        return {'rows': (expected, rows)}
