import threading
import unittest
import urllib.parse
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

from yardstick.client import Client


class Handler(BaseHTTPRequestHandler):
    def do_POST(self):
        sql = self.rfile.read(int(self.headers['Content-Length'])).decode()
        self.server.requests.append((sql, dict(urllib.parse.parse_qsl(urllib.parse.urlparse(self.path).query))))
        status, body = self.server.answer(sql)
        self.send_response(status)
        self.send_header('Content-Length', str(len(body)))
        self.end_headers()
        self.wfile.write(body.encode())

    def log_message(self, *args):
        pass


class ClientTest(unittest.TestCase):
    def serve(self, answer):
        server = ThreadingHTTPServer(('127.0.0.1', 0), Handler)
        server.requests = []
        server.answer = answer
        threading.Thread(target=server.serve_forever, kwargs={'poll_interval': 0.05}, daemon=True).start()
        self.addCleanup(server.server_close)
        self.addCleanup(server.shutdown)
        return server, Client(server.server_address[1])

    def test_every_query_carries_the_drop_setting(self):
        server, c = self.serve(lambda sql: (200, '1\n'))
        self.assertEqual(c.query('SELECT 1', settings={'max_threads': 1}), '1\n')
        [(sql, params)] = server.requests
        self.assertEqual(params, {'database_atomic_wait_for_drop_and_detach_synchronously': '1', 'max_threads': '1'})

    def test_http_error_raises_with_sql_and_body(self):
        _, c = self.serve(lambda sql: (500, 'Code: 60. DB::Exception: Table x does not exist' + 'x' * 1000))
        with self.assertRaises(RuntimeError) as e:
            c.query('SELECT * FROM x')
        sql, body = e.exception.args
        self.assertEqual(sql, 'SELECT * FROM x')
        self.assertTrue(body.startswith('Code: 60.'))
        self.assertEqual(len(body), 500)

    def test_events(self):
        _, c = self.serve(lambda sql: (200, 'DiskS3GetObject\t83\nCASRootGet\t0\n'))
        self.assertEqual(c.events(), {'DiskS3GetObject': 83, 'CASRootGet': 0})

    def test_settle_times_out_while_busy(self):
        server, c = self.serve(lambda sql: (200, '' if sql == 'SYSTEM FLUSH LOGS' else '1\n'))
        with self.assertRaises(TimeoutError):
            c.settle('yardstick.*', timeout=0.6)
        self.assertEqual(server.requests[0][0], 'SYSTEM FLUSH LOGS')
        self.assertIn("database = 'yardstick'", server.requests[1][0])
        self.assertNotIn('table =', server.requests[1][0])

    def test_settle_returns_when_idle(self):
        server, c = self.serve(lambda sql: (200, '0\n'))
        c.settle('yardstick.t')
        self.assertIn("table = 't'", server.requests[1][0])

    def test_drop_caches_stops_at_the_first_failure(self):
        server, c = self.serve(lambda sql: (500, 'unknown') if sql == 'SYSTEM DROP MARK CACHE' else (200, ''))
        with self.assertRaises(RuntimeError):
            c.drop_caches()
        self.assertEqual([sql for sql, _ in server.requests], ['SYSTEM DROP FILESYSTEM CACHE', 'SYSTEM DROP MARK CACHE'])

    def test_drop_caches_statements(self):
        server, c = self.serve(lambda sql: (200, ''))
        c.drop_caches()
        self.assertEqual([sql for sql, _ in server.requests],
                         ['SYSTEM DROP FILESYSTEM CACHE', 'SYSTEM DROP MARK CACHE', 'SYSTEM DROP UNCOMPRESSED CACHE',
                          'SYSTEM CLEAR PRIMARY INDEX CACHE'])


if __name__ == '__main__':
    unittest.main()
