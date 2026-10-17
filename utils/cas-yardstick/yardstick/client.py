"""HTTP client for one yardstick server process."""
import time
import urllib.error
import urllib.parse
import urllib.request

BASE_SETTINGS = {'database_atomic_wait_for_drop_and_detach_synchronously': '1'}
SETTLE_POLL_SEC = 0.5


class Client:
    def __init__(self, http_port: int, host: str = '127.0.0.1'):
        self.url = f'http://{host}:{http_port}/'

    def query(self, sql: str, settings: dict | None = None, timeout: float = 3600) -> str:
        """Run one statement; raise RuntimeError(sql, body[:500]) on an HTTP error."""
        params = dict(BASE_SETTINGS)
        params.update({k: str(v) for k, v in (settings or {}).items()})
        request = urllib.request.Request(self.url + '?' + urllib.parse.urlencode(params), data=sql.encode())
        try:
            with urllib.request.urlopen(request, timeout=timeout) as response:
                return response.read().decode()
        except urllib.error.HTTPError as e:
            raise RuntimeError(sql, e.read().decode(errors='replace')[:500]) from None

    def events(self) -> dict[str, int]:
        text = self.query('SELECT event, value FROM system.events SETTINGS system_events_show_zero_values = 1 '
                          'FORMAT TabSeparated')
        return {name: int(value) for name, value in (line.split('\t') for line in text.splitlines())}

    def settle(self, table: str, timeout: float = 60) -> None:
        """Wait until no merge, unfinished mutation or fetch touches `table` (`db.name` or `db.*`).
        Raises TimeoutError."""
        db, name = table.split('.', 1)
        cond = f"database = '{db}'" + ('' if name == '*' else f" AND table = '{name}'")
        sql = (f'SELECT (SELECT count() FROM system.merges WHERE {cond})'
               f' + (SELECT count() FROM system.mutations WHERE is_done = 0 AND {cond})'
               f' + (SELECT count() FROM system.replicated_fetches WHERE {cond})')
        self.query('SYSTEM FLUSH LOGS')
        deadline = time.monotonic() + timeout
        while True:
            busy = int(self.query(sql))
            if busy == 0:
                return
            if time.monotonic() >= deadline:
                raise TimeoutError(f'{table}: {busy} merges/mutations/fetches still running after {timeout} s')
            time.sleep(SETTLE_POLL_SEC)

    def drop_caches(self) -> None:
        for sql in ('SYSTEM DROP FILESYSTEM CACHE', 'SYSTEM DROP MARK CACHE', 'SYSTEM DROP UNCOMPRESSED CACHE',
                    'SYSTEM CLEAR PRIMARY INDEX CACHE'):
            self.query(sql)
