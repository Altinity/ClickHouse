"""A fake GCE metadata server, serving only the service-account bearer-token endpoint.

`PocoHTTPClientGCPOAuth` fetches a bearer token from the metadata service before every request whose
cached token has expired, so a `gcp_oauth` disk cannot perform a single real request without one.
Without this server the disk fails to resolve `metadata.google.internal` and the failure looks like a
GCS problem rather than a missing fixture, which is why this lives in its own module and its own
container: the disk config points `metadata_service` at this host instead.

It answers a long expiry so that the token is fetched once and no test depends on refresh timing.

Usage: ``python3 auth.py <port>``. Started by ``helpers.mock_servers.start_mock_servers``, which
probes ``GET /`` and expects the body ``OK``.
"""

import http.server
import json
import sys

ACCESS_TOKEN = "fake-gce-metadata-bearer-token"


class Handler(http.server.BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"

    def log_message(self, fmt, *args):
        sys.stderr.write("%s - %s\n" % (self.address_string(), fmt % args))

    def _reply(self):
        if self.path == "/":
            return 200, b"OK", "text/plain"
        if self.path.rstrip("/").endswith("/token"):
            payload = json.dumps(
                {
                    "access_token": ACCESS_TOKEN,
                    "expires_in": 86400,
                    "token_type": "Bearer",
                }
            ).encode()
            return 200, payload, "application/json"
        return 404, b"not a metadata endpoint: " + self.path.encode(), "text/plain"

    def do_GET(self):
        status, body, content_type = self._reply()
        self.send_response(status)
        self.send_header("Content-Type", content_type)
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def do_HEAD(self):
        status, body, content_type = self._reply()
        self.send_response(status)
        self.send_header("Content-Type", content_type)
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()


def main():
    server = http.server.ThreadingHTTPServer(("0.0.0.0", int(sys.argv[1])), Handler)
    server.daemon_threads = True
    server.serve_forever()


if __name__ == "__main__":
    main()
