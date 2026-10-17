import contextlib
import io
import os
import re
import shutil
import socket
import subprocess
import tempfile
import threading
import unittest
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from unittest import mock

from yardstick import report as R
from yardstick import runner
from tests.test_runner import FakeCard, FakeServer, FakeClient

SERVER_SH = os.path.join(runner.TOOL_DIR, 'server.sh')
RUSTFS_SH = os.path.join(runner.TOOL_DIR, 'rustfs.sh')


class WorkloadHashTest(unittest.TestCase):
    def setUp(self):
        self.dir = os.path.join(tempfile.mkdtemp(), 'tool')
        self.addCleanup(shutil.rmtree, os.path.dirname(self.dir), ignore_errors=True)
        shutil.copytree(runner.TOOL_DIR, self.dir, ignore=shutil.ignore_patterns('__pycache__', 'runs'))

    def touch(self, rel, text='\n# changed\n'):
        path = os.path.join(self.dir, rel)
        os.makedirs(os.path.dirname(path), exist_ok=True)
        with open(path, 'a') as f:
            f.write(text)

    def test_docs_reference_and_tests_leave_the_hash_unchanged(self):
        before = runner.workload_hash(self.dir)
        self.assertEqual(len(before), 64)
        for rel in ('README.md', 'reference/CURRENT', 'reference/2099-01-01-x/summary.tsv', 'tests/test_cli.py'):
            self.touch(rel)
        self.assertEqual(runner.workload_hash(self.dir), before)

    def test_workload_files_change_the_hash(self):
        for rel in ('yardstick.py', 'yardstick/report.py', 'yardstick/cards/cycle.py', 'yardstick/cards/new_card.py',
                    'etc/config.xml', 'server.sh', 'rustfs.sh'):
            with self.subTest(rel=rel):
                before = runner.workload_hash(self.dir)
                self.touch(rel)
                self.assertNotEqual(runner.workload_hash(self.dir), before)


def _free_port_pair():
    for _ in range(100):
        with socket.socket() as a, socket.socket() as b:
            try:
                a.bind(('127.0.0.1', 0))
                port = a.getsockname()[1]
                b.bind(('127.0.0.1', port + 1))
                return port
            except OSError:
                continue
    raise RuntimeError('no free port pair')


# Stands in for `clickhouse server`: records its environment and pid, answers `1` on the HTTP port, exits on POST.
FAKE_SERVER = r'''#!/bin/bash
http_port=$(printf '%s\n' "$@" | sed -n 's/^--http_port=//p')
env > env.txt
echo $$ > real.pid
exec python3 -c '
import sys
from http.server import BaseHTTPRequestHandler, HTTPServer
class H(BaseHTTPRequestHandler):
    def do_GET(self):
        self.send_response(200); self.end_headers(); self.wfile.write(b"1")
    def do_POST(self):
        self.send_response(200); self.end_headers(); self.server.done = True
    def log_message(self, *a):
        pass
s = HTTPServer(("127.0.0.1", int(sys.argv[1])), H)
s.done = False
while not s.done:
    s.handle_request()
' "$http_port"
'''


class ServerScriptTest(unittest.TestCase):
    def setUp(self):
        self.dir = tempfile.mkdtemp()
        self.addCleanup(shutil.rmtree, self.dir, ignore_errors=True)
        os.makedirs(os.path.join(self.dir, 'etc'))
        open(os.path.join(self.dir, 'etc', 'config.xml'), 'w').close()
        self.binary = os.path.join(self.dir, 'fake-clickhouse')
        with open(self.binary, 'w') as f:
            f.write(FAKE_SERVER)
        os.chmod(self.binary, 0o755)
        self.port = _free_port_pair()

    def sh(self, *args):
        return subprocess.run(['bash', SERVER_SH, *args], capture_output=True, text=True, timeout=60)

    def read(self, name):
        with open(os.path.join(self.dir, name)) as f:
            return f.read()

    def test_server_runs_without_the_watchdog_and_stops_cleanly(self):
        started = self.sh('start', self.binary, self.dir, str(self.port))
        self.assertEqual(started.returncode, 0, started.stderr)
        clickhouse_env = [line for line in self.read('env.txt').splitlines() if line.startswith('CLICKHOUSE_')]
        self.assertIn('CLICKHOUSE_WATCHDOG_ENABLE=0', clickhouse_env)
        self.assertEqual(self.read('server.pid').strip(), self.read('real.pid').strip())
        stopped = self.sh('stop', self.dir)
        self.assertEqual((stopped.returncode, stopped.stdout.strip()), (0, 'stopped'), stopped.stderr)

    def test_start_refuses_a_port_that_already_answers(self):
        class One(BaseHTTPRequestHandler):
            def do_GET(self):
                self.send_response(200)
                self.end_headers()
                self.wfile.write(b'1')

            def log_message(self, *a):
                pass

        other = ThreadingHTTPServer(('127.0.0.1', self.port + 1), One)
        threading.Thread(target=other.serve_forever, kwargs={'poll_interval': 0.05}, daemon=True).start()
        self.addCleanup(other.server_close)
        self.addCleanup(other.shutdown)
        started = self.sh('start', self.binary, self.dir, str(self.port))
        self.assertNotEqual(started.returncode, 0)
        self.assertIn('already answers', started.stderr)
        self.assertFalse(os.path.exists(os.path.join(self.dir, 'env.txt')))


class RemovePrefixTest(unittest.TestCase):
    def test_runner_deadline_is_above_the_scripts(self):
        with open(RUSTFS_SH) as f:
            script = int(re.search(r'^RM_PREFIX_TIMEOUT=(\d+)$', f.read(), re.M).group(1))
        self.assertGreaterEqual(runner.RM_PREFIX_TIMEOUT_SEC, script + 60)

    def test_remove_prefix_passes_the_long_deadline(self):
        seen = {}

        def fake_run(cmd, **kw):
            seen.update(cmd=cmd, timeout=kw['timeout'])
            return subprocess.CompletedProcess(cmd, 0, '', '')

        with mock.patch.object(runner.subprocess, 'run', fake_run):
            self.assertTrue(runner.remove_prefix('R1'))
        self.assertEqual(seen['cmd'][-2:], ['rm-prefix', 'test/R1'])
        self.assertEqual(seen['timeout'], runner.RM_PREFIX_TIMEOUT_SEC)


class FinishRunTest(unittest.TestCase):
    def setUp(self):
        self.dir = tempfile.mkdtemp()
        self.addCleanup(shutil.rmtree, self.dir, ignore_errors=True)
        self.run_root = os.path.join(self.dir, 'R1')
        os.makedirs(self.run_root)
        R.write_manifest(os.path.join(self.run_root, 'manifest.tsv'), dict(scale='1.0', repeats='2', gc='on'))
        self.deps = runner.Deps(run_id='R1', runs_dir=self.dir, endpoint='e', gc=True, scale=1.0,
                                server=FakeServer([]), client_for=lambda run_dir: FakeClient(),
                                write_config=lambda run_dir, policy, *a: 'h-' + policy,
                                config_hashes={'cas': 'h-cas', 's3': 'h-s3'})

    def finish(self, rows, interrupted, smoke=False, cards=()):
        out, err = io.StringIO(), io.StringIO()
        with contextlib.redirect_stdout(out), contextlib.redirect_stderr(err):
            code = runner.finish_run(self.run_root, rows, [], interrupted, True, ['cas', 's3'], list(cards), smoke)
        return code, out.getvalue(), err.getvalue()

    def test_interrupt_in_a_later_repeat_still_writes_the_summary(self):
        # Repeat 2 runs s3 first; the interrupt hits cas, so GET has repeats {1, 2} on s3 and {1} on cas.
        card = FakeCard('c')
        original = card.measured
        cas_cold_passes = []

        def measured(c, policy, counts, phase):
            if (policy, phase) == ('cas', 'cold'):
                cas_cold_passes.append(1)
                if len(cas_cold_passes) == 2:
                    raise KeyboardInterrupt
            return original(c, policy, counts, phase)

        card.measured = measured
        rows, interrupted, _ = runner.run_all(['cas', 's3'], [card], 2, self.deps, cleanup=lambda: True)
        self.assertTrue(interrupted)
        self.assertEqual({(r['policy'], r['repeat']) for r in rows}, {('cas', 1), ('s3', 1), ('s3', 2)})
        code, out, _ = self.finish(rows, interrupted=True)
        self.assertEqual(code, 130)
        summary = R.read_summary(os.path.join(self.run_root, 'summary.tsv'))
        self.assertEqual([r for r in summary if r['metric'].startswith('ratio:')], [])
        self.assertEqual(len(R.read_samples(os.path.join(self.run_root, 'samples.tsv'))), len(rows))
        self.assertIn('c/cold', out)

    def test_interrupt_wins_over_a_smoke_failure(self):
        rows = [dict(policy='cas', card='c', phase='cold', repeat=1, metric='status', value='FAILED', unit_count=0)]
        code, _, err = self.finish(rows, interrupted=True, smoke=True, cards=[FakeCard('c')])
        self.assertEqual(code, 130)
        self.assertIn('smoke: cas c cold: FAILED', err)


if __name__ == '__main__':
    unittest.main()
