#!/usr/bin/env python3
"""Transparent reverse proxy for an Iceberg REST catalog with controllable fault injection.

Forwards every request unchanged by default. A test arms a one-shot fault via the
/__fault control API; once armed, the proxy forwards the matching commit POST upstream
(so the catalog commits), consumes the upstream response, then RSTs the client connection
without returning that response. ClickHouse then retries the identical POST into a now
stale-assert state, reproducing the "commit succeeded, response lost" window.

Request bodies (including Transfer-Encoding: chunked) are fully decoded before being
forwarded, and the upstream status plus forwarded body length of every /tables/ POST are
recorded and exposed via /__fault/status for the test to assert on.
"""

import http.client
import json
import os
import socket
import struct
import sys
import threading
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

UPSTREAM = os.environ.get("UPSTREAM", "http://rest:8181")
LISTEN = os.environ.get("LISTEN", "0.0.0.0:8181")

_HOP_BY_HOP = {
    "connection", "keep-alive", "proxy-authenticate", "proxy-authorization",
    "te", "trailers", "transfer-encoding", "upgrade",
}


def _parse_upstream(u):
    assert u.startswith("http://"), "only http upstream supported"
    hostport = u[len("http://"):].rstrip("/")
    if ":" in hostport:
        host, port = hostport.split(":", 1)
        return host, int(port)
    return hostport, 80


UP_HOST, UP_PORT = _parse_upstream(UPSTREAM)


class _Fault:
    def __init__(self):
        self.lock = threading.Lock()
        self.armed = None
        self.budget = 0
        self.seen = 0
        self.faulted = 0
        self.fault_upstream_status = None
        self.commit_posts = []

    def arm(self, rule):
        with self.lock:
            self.armed = rule
            self.budget = int(rule.get("count", 1))
            self.seen = 0
            self.faulted = 0
            self.fault_upstream_status = None
            self.commit_posts = []

    def disarm(self):
        with self.lock:
            self.armed = None
            self.budget = 0

    def should_fault(self, method, path):
        with self.lock:
            if not self.armed or self.budget <= 0:
                return False
            if self.armed.get("method", "POST").upper() != method.upper():
                return False
            if self.armed.get("match_path_substr", "/tables/") not in path:
                return False
            self.seen += 1
            self.budget -= 1
            self.faulted += 1
            return True

    def record_commit(self, status, faulted, body_len):
        with self.lock:
            self.commit_posts.append(
                {"status": status, "faulted": faulted, "body_len": body_len})
            if faulted:
                self.fault_upstream_status = status

    def status(self):
        with self.lock:
            return {
                "armed": self.armed, "budget": self.budget,
                "seen": self.seen, "faulted": self.faulted,
                "fault_upstream_status": self.fault_upstream_status,
                "commit_posts": list(self.commit_posts),
            }


FAULT = _Fault()


class Handler(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"

    def _read_body(self):
        te = (self.headers.get("Transfer-Encoding") or "").lower()
        if "chunked" in te:
            return self._read_chunked()
        n = int(self.headers.get("Content-Length", 0) or 0)
        return self.rfile.read(n) if n else b""

    def _read_chunked(self):
        chunks = []
        while True:
            line = self.rfile.readline()
            if not line:
                break
            line = line.strip()
            if not line:
                continue
            try:
                size = int(line.split(b";", 1)[0], 16)
            except ValueError:
                break
            if size == 0:
                while True:
                    t = self.rfile.readline()
                    if t in (b"\r\n", b"\n", b""):
                        break
                break
            chunks.append(self.rfile.read(size))
            self.rfile.read(2)
        return b"".join(chunks)

    def _json(self, status, obj):
        body = json.dumps(obj).encode()
        self.send_response(status)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def _control(self):
        if self.command == "POST" and self.path.startswith("/__fault/arm"):
            rule = json.loads(self._read_body() or b"{}")
            FAULT.arm(rule)
            self.log_message("ARM %s", rule)
            return self._json(200, {"ok": True, "armed": rule})
        if self.command == "POST" and self.path.startswith("/__fault/disarm"):
            self._read_body()
            FAULT.disarm()
            return self._json(200, {"ok": True})
        if self.command == "GET" and self.path.startswith("/__fault/status"):
            return self._json(200, FAULT.status())
        return self._json(404, {"error": "unknown control path"})

    def _upstream_headers(self):
        h = {}
        for k, v in self.headers.items():
            kl = k.lower()
            if kl in _HOP_BY_HOP or kl in ("host", "content-length"):
                continue
            h[k] = v
        h["Host"] = f"{UP_HOST}:{UP_PORT}"
        return h

    def _forward(self, body):
        conn = http.client.HTTPConnection(UP_HOST, UP_PORT, timeout=60)
        conn.request(self.command, self.path, body=body, headers=self._upstream_headers())
        resp = conn.getresponse()
        data = resp.read()
        headers = [(k, v) for (k, v) in resp.getheaders() if k.lower() not in _HOP_BY_HOP]
        status = resp.status
        conn.close()
        return status, headers, data

    def _reset_connection(self):
        try:
            self.connection.setsockopt(
                socket.SOL_SOCKET, socket.SO_LINGER, struct.pack("ii", 1, 0))
        except OSError:
            pass
        self.close_connection = True
        try:
            self.connection.close()
        except OSError:
            pass

    def _relay(self, status, headers, data):
        self.send_response(status)
        sent_len = False
        for k, v in headers:
            if k.lower() == "content-length":
                sent_len = True
            self.send_header(k, v)
        if not sent_len:
            self.send_header("Content-Length", str(len(data)))
        self.end_headers()
        if self.command != "HEAD":
            self.wfile.write(data)

    def _handle(self):
        if self.path.startswith("/__fault"):
            return self._control()
        body = self._read_body()
        fault = FAULT.should_fault(self.command, self.path)
        try:
            status, headers, data = self._forward(body)
        except Exception as e:
            self.log_message("upstream forward FAILED for %s %s: %s", self.command, self.path, e)
            return self._json(502, {"error": "upstream forward failed"})

        is_commit = self.command == "POST" and "/tables/" in self.path
        if is_commit:
            FAULT.record_commit(status, fault, len(body))
            self.log_message("commit POST -> upstream %s (faulted=%s, fwd_body=%dB) %s",
                             status, fault, len(body), self.path)

        if fault:
            self.log_message("FAULT: upstream returned %s; dropping response to client (RST)", status)
            return self._reset_connection()
        return self._relay(status, headers, data)

    do_GET = _handle
    do_POST = _handle
    do_PUT = _handle
    do_DELETE = _handle
    do_HEAD = _handle

    def log_message(self, fmt, *args):
        sys.stderr.write("[proxy] " + (fmt % args) + "\n")
        sys.stderr.flush()


def main():
    host, port = LISTEN.split(":")
    httpd = ThreadingHTTPServer((host, int(port)), Handler)
    print(f"catalog_fault_proxy: listening {LISTEN}, upstream {UPSTREAM}", flush=True)
    httpd.serve_forever()


if __name__ == "__main__":
    main()
