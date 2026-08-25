import concurrent.futures
import hashlib
import http.client
import http.server
import importlib.util
import json
from pathlib import Path
import sys
import threading

import pytest


PROXY_PATH = Path(__file__).parents[1] / "proxy" / "s3_fault_proxy.py"
SPEC = importlib.util.spec_from_file_location("s3_fault_proxy_under_test", PROXY_PATH)
proxy = importlib.util.module_from_spec(SPEC)
assert SPEC.loader is not None
SPEC.loader.exec_module(proxy)

CA_SOAK_PATH = Path(__file__).parents[1]
if str(CA_SOAK_PATH) not in sys.path:
    sys.path.insert(0, str(CA_SOAK_PATH))
s39 = importlib.import_module("scenarios.cards.s39_lease_fault_tolerance")


class UpstreamHandler(http.server.BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"

    def log_message(self, *_args):
        pass

    def do_PUT(self):
        body = self.rfile.read(int(self.headers.get("Content-Length", "0")))
        with self.server.requests_lock:
            self.server.requests.append((self.command, self.path, body))
        etag = hashlib.sha256(body).hexdigest()[:24]
        response = b"landed"
        self.send_response(201)
        self.send_header("ETag", f'"{etag}"')
        self.send_header("Content-Length", str(len(response)))
        self.end_headers()
        self.wfile.write(response)


@pytest.fixture
def servers():
    upstream = proxy.ThreadingHTTPServer(("127.0.0.1", 0), UpstreamHandler)
    upstream.requests = []
    upstream.requests_lock = threading.Lock()
    proxy.UPSTREAM = "{}:{}".format(*upstream.server_address)
    s3 = proxy.ThreadingHTTPServer(("127.0.0.1", 0), proxy.Handler)
    control = proxy.ThreadingHTTPServer(("127.0.0.1", 0), proxy.CtlHandler)
    threads = [
        threading.Thread(target=upstream.serve_forever, daemon=True),
        threading.Thread(target=s3.serve_forever, daemon=True),
        threading.Thread(target=control.serve_forever, daemon=True),
    ]
    for thread in threads:
        thread.start()

    def configure(patch):
        connection = http.client.HTTPConnection(*control.server_address, timeout=5)
        body = json.dumps(patch).encode()
        connection.request("POST", "/config", body=body, headers={"Content-Type": "application/json"})
        response = connection.getresponse()
        payload = json.loads(response.read())
        connection.close()
        assert response.status == 200, payload
        return payload

    def stats():
        connection = http.client.HTTPConnection(*control.server_address, timeout=5)
        connection.request("GET", "/stats")
        response = connection.getresponse()
        payload = json.loads(response.read())
        connection.close()
        assert response.status == 200, payload
        return payload

    def put(path, body=b"payload"):
        connection = http.client.HTTPConnection(*s3.server_address, timeout=5)
        try:
            connection.request("PUT", path, body=body, headers={"Content-Length": str(len(body))})
            response = connection.getresponse()
            return response.status, response.read()
        except (http.client.RemoteDisconnected, ConnectionResetError, TimeoutError):
            return None, b""
        finally:
            connection.close()

    configure({"reset": True})
    try:
        yield configure, stats, put, upstream
    finally:
        configure({"reset": True})
        for server in (control, s3, upstream):
            server.shutdown()
            server.server_close()
        for thread in threads:
            thread.join(timeout=5)


def test_scoped_fault_passes_path_mismatch_and_consumes_exactly_once(servers):
    configure, stats, put, upstream = servers
    configure(
        {
            "rate": 1.0,
            "modes": ["503"],
            "methods": ["PUT"],
            "path_substring": "/gc/server-roots/node-a/mount",
            "remaining_faults": 1,
        }
    )

    assert put("/bucket/data/object") == (201, b"landed")
    assert put("/bucket/gc/server-roots/node-a/mount")[0] == 503
    assert put("/bucket/gc/server-roots/node-a/mount") == (201, b"landed")
    assert stats()["faults"] == 1
    assert [request[1] for request in upstream.requests] == [
        "/bucket/data/object",
        "/bucket/gc/server-roots/node-a/mount",
    ]


def test_remaining_faults_is_atomic_under_concurrent_requests(servers):
    configure, stats, put, upstream = servers
    configure(
        {
            "rate": 1.0,
            "modes": ["503"],
            "methods": ["PUT"],
            "path_substring": "/mount",
            "remaining_faults": 3,
        }
    )

    with concurrent.futures.ThreadPoolExecutor(max_workers=12) as executor:
        statuses = list(executor.map(lambda i: put(f"/mount?request={i}")[0], range(12)))

    assert statuses.count(503) == 3
    assert statuses.count(201) == 9
    assert stats()["faults"] == 3
    assert len(upstream.requests) == 9


def test_reset_restores_disarmed_defaults_and_clears_statistics(servers):
    configure, stats, put, upstream = servers
    configure(
        {
            "rate": 1.0,
            "modes": ["503"],
            "methods": ["PUT"],
            "path_substring": "/mount",
            "remaining_faults": 2,
        }
    )
    assert put("/mount")[0] == 503

    reset = configure({"reset": True})

    assert reset["config"]["rate"] == 0.0
    assert reset["config"]["remaining_faults"] is None
    assert put("/mount") == (201, b"landed")
    assert stats()["faults"] == 0
    assert len(upstream.requests) == 1


def test_drop_after_forward_loses_response_but_retains_landing_evidence(servers):
    configure, stats, put, upstream = servers
    body = b"immutable mount renewal body\nwrite_attempt_id=0011223344556677\n"
    configure(
        {
            "rate": 1.0,
            "modes": ["drop_after_forward"],
            "methods": ["PUT"],
            "path_substring": "/gc/server-roots/node-a/mount",
            "remaining_faults": 1,
        }
    )

    assert put("/bucket/gc/server-roots/node-a/mount", body)[0] is None
    assert upstream.requests == [
        ("PUT", "/bucket/gc/server-roots/node-a/mount", body),
    ]
    record = stats()["drop_after_forward"][-1]
    assert record == {
        "method": "PUT",
        "path": "/bucket/gc/server-roots/node-a/mount",
        "request_body_sha256": hashlib.sha256(body).hexdigest(),
        "upstream_status": 201,
        "upstream_etag": f'"{hashlib.sha256(body).hexdigest()[:24]}"',
    }


def test_unscoped_configuration_keeps_legacy_seeded_decisions(servers):
    configure, stats, put, upstream = servers
    configure(
        {
            "rate": 0.5,
            "modes": ["503", "429"],
            "methods": ["PUT"],
            "seed": 17,
        }
    )

    statuses = [put("/legacy/path/{}".format(index))[0] for index in range(1, 9)]

    # This is the original seed/request-index sequence. Omitting both scoped fields must preserve it.
    assert statuses == [429, 429, 201, 429, 201, 201, 201, 503]
    assert stats()["faults"] == 4
    assert len(upstream.requests) == 4


def test_s39_duration_budget_reserves_long_recovery_write_and_cleanup():
    helper = getattr(s39, "_duration_budget", None)
    assert helper is not None, "S39 must derive its schedule from ctx.duration_s"

    assert helper(900, 40, 20) == {
        "requested_s": 900,
        "acceptable_min_elapsed_s": 885,
        "long_fault_s": 40,
        "short_pulse_budget_s": 100,
        "mandatory_short_pulses": 2,
        "mandatory_short_budget_s": 200,
        "remount_recovery_budget_s": 110,
        "post_clear_write_budget_s": 90,
        "final_checks_cleanup_budget_s": 60,
        "long_and_final_reserve_s": 300,
        "minimum_duration_s": 500,
        "short_window_s": 600,
        "additional_short_window_s": 400,
    }


def test_s39_duration_budget_rejects_unsafe_short_campaign():
    helper = getattr(s39, "_duration_budget", None)
    assert helper is not None, "S39 must validate the requested duration"

    with pytest.raises(ValueError, match="requires at least 500s; requested 499s"):
        helper(499, 40, 20)


def test_s39_short_scheduler_waits_only_for_a_sub_pulse_residual():
    helper = getattr(s39, "_complete_short_pulse_fits", None)
    assert helper is not None, "S39 must reserve a complete pulse before starting it"

    assert helper(500.0, 600.0)
    assert not helper(500.001, 600.0)


def test_s39_pass_verdict_note_is_empty():
    helper = getattr(s39, "_failure_note", None)
    assert helper is not None, "S39 must not display failure explanations on passing verdicts"

    assert helper(True, "failure explanation") == ""
    assert helper(False, "failure explanation") == "failure explanation"
