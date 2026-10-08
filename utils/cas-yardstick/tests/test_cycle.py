import _thread
import re
import threading
import time
import unittest

from yardstick.cards.cycle import Cycle, WORKERS

WORK_SEC = 0.02


class FailingWorkerClient:
    """Worker 0's CREATE fails; the other workers' queries each take WORK_SEC."""
    def __init__(self):
        self.creates = {}
        self.lock = threading.Lock()

    def query(self, sql, settings=None, timeout=3600):
        worker = int(re.search(r't_\w+?_(\d+)_\d+', sql).group(1))
        if sql.startswith('CREATE'):
            with self.lock:
                self.creates[worker] = self.creates.get(worker, 0) + 1
            if worker == 0:
                raise RuntimeError('create failed')
        time.sleep(WORK_SEC)
        return '3000\t4498500\n' if sql.startswith('SELECT') else ''


class InterruptingClient:
    """The first query raises KeyboardInterrupt in the main thread; every query then blocks until `release`."""
    def __init__(self):
        self.queries = []
        self.lock = threading.Lock()
        self.release = threading.Event()

    def query(self, sql, settings=None, timeout=3600):
        with self.lock:
            self.queries.append(sql)
            first = len(self.queries) == 1
        if first:
            _thread.interrupt_main()
        self.release.wait(10)
        return '30\t435\n' if sql.startswith('SELECT') else ''


class CycleTest(unittest.TestCase):
    def test_counts(self):
        self.assertEqual(Cycle().counts(1.0), {'iterations': 25, 'rows_per_insert': 1000})
        self.assertEqual(Cycle().counts(0.01), {'iterations': 1, 'rows_per_insert': 10})

    def test_ctrl_c_stops_the_workers_without_waiting_for_them(self):
        baseline = threading.active_count()
        c = InterruptingClient()
        t0 = time.monotonic()
        with self.assertRaises(KeyboardInterrupt):
            Cycle().measured(c, 'cas', {'iterations': 25, 'rows_per_insert': 10}, 'cold')
        self.assertLess(time.monotonic() - t0, 5)
        c.release.set()
        deadline = time.monotonic() + 10
        while threading.active_count() > baseline and time.monotonic() < deadline:
            time.sleep(0.01)
        self.assertEqual(threading.active_count(), baseline)
        # At most the iteration each worker was in: CREATE, 3 INSERT, SELECT, DROP.
        self.assertLessEqual(len(c.queries), WORKERS * 6)

    def test_rows_follow_the_scale(self):
        class Recording:
            def __init__(self):
                self.sqls = []

            def query(self, sql, settings=None, timeout=3600):
                self.sqls.append(sql)
                return '30\t435\n' if sql.startswith('SELECT') else ''

        c = Recording()
        card = Cycle()
        card.measured(c, 'cas', {'iterations': 1, 'rows_per_insert': 10}, 'cold')
        self.assertIn('FROM numbers(20, 10)', '\n'.join(c.sqls))
        self.assertEqual(card.invariants(type('C', (), {'query': lambda self, s: '0'})(), {'iterations': 1, 'rows_per_insert': 10},
                                         'cold', 'after')['select_count_sum_mismatches'], (0, 0))

    def test_failed_worker_stops_the_others(self):
        c = FailingWorkerClient()
        with self.assertRaisesRegex(RuntimeError, 'create failed'):
            Cycle().measured(c, 'cas', {'iterations': 25, 'rows_per_insert': 1000}, 'cold')
        others = sum(n for w, n in c.creates.items() if w != 0)
        self.assertLess(others, (WORKERS - 1) * 25)


if __name__ == '__main__':
    unittest.main()
