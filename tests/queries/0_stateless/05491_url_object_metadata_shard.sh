#!/usr/bin/env bash
# Tags: no-fasttest
# Tag no-fasttest: serves two local HTTP shards so url() can list the same path on each.

# Two url() shards expose the same relative path with different sizes. Metadata for each listed
# file must be read from that file's shard. Losing the shard index HEADs the first shard for both
# and uses its size as the read bound, so the larger file is truncated.

CUR_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
# shellcheck source=../shell_config.sh
. "$CUR_DIR"/../shell_config.sh

fail()
{
    echo "FAIL: $*" >&2
    exit 1
}

free_port()
{
    python3 -c "
import socket
s = socket.socket()
s.bind(('127.0.0.1', 0))
print(s.getsockname()[1])
s.close()
"
}

PORT_A=$(free_port)
PORT_B=$(free_port)
[[ -n $PORT_A && -n $PORT_B && $PORT_A != "$PORT_B" ]] || fail "could not reserve two ports"

python3 -c "
from http.server import HTTPServer, BaseHTTPRequestHandler
import threading

SMALL = b'1\n'
LARGE = b'10\n20\n30\n'
INDEX = b'<a href=\"file.tsv\">file.tsv</a>\n'

def make_handler(body):
    class Handler(BaseHTTPRequestHandler):
        def log_message(self, *args):
            pass

        def respond(self, with_body):
            path = self.path.split('?')[0]
            if path == '/data/':
                data, content_type = INDEX, 'text/html'
            elif path == '/data/file.tsv':
                data, content_type = body, 'text/plain'
            else:
                self.send_response(404)
                self.end_headers()
                return
            self.send_response(200)
            self.send_header('Content-Type', content_type)
            self.send_header('Content-Length', str(len(data)))
            self.end_headers()
            if with_body:
                self.wfile.write(data)

        def do_HEAD(self):
            self.respond(False)

        def do_GET(self):
            self.respond(True)
    return Handler

def serve(port, body):
    HTTPServer(('127.0.0.1', port), make_handler(body)).serve_forever()

threading.Thread(target=serve, args=(${PORT_A}, SMALL), daemon=True).start()
serve(${PORT_B}, LARGE)
" &
HTTP_PID=$!
trap 'kill $HTTP_PID 2>/dev/null; wait $HTTP_PID 2>/dev/null' EXIT

for _ in $(seq 1 50); do
    curl -sf "http://127.0.0.1:${PORT_A}/data/" >/dev/null && curl -sf "http://127.0.0.1:${PORT_B}/data/" >/dev/null && break
    sleep 0.1
done
curl -sf "http://127.0.0.1:${PORT_A}/data/" >/dev/null || fail "shard A did not start"
curl -sf "http://127.0.0.1:${PORT_B}/data/" >/dev/null || fail "shard B did not start"

URL="http://127.0.0.1:{${PORT_A},${PORT_B}}/data/*.tsv"
data=$(${CLICKHOUSE_CLIENT} -q "
    SET allow_experimental_url_wildcard_from_index_pages = 1;
    SELECT n
    FROM url('${URL}', 'TSV', 'n UInt64')
    ORDER BY n
    SETTINGS max_threads = 1, http_max_tries = 1
    FORMAT TabSeparated") || fail "select failed"

expected=$'1\n10\n20\n30'
[[ $data == "$expected" ]] || fail "data '${data}', expected '${expected}'"

echo OK
