"""One CAS metadata request gets its own window, not a deadline shared with the previous one.

Pinned values (configs/storage_conf.xml, configs/drop_cooldown.xml):
  attempt envelope 2000 ms, cas_request_window_ms 5000 (more than two envelopes),
  slow_ms 7000 (more than the window), trickle interval 250 ms (shorter than the 500 ms
  connect_timeout_ms, so the socket never idles that long),
  database_catalog_drop_error_cooldown_sec 600 (a failed removal is not requeued while a test
  checks it).

The kill tests run on `node_stop` (configs/storage_conf_stop.xml): attempt timeout 8000 ms, so a
request parked for the kill handshake is not abandoned by its attempt timeout first. A plain `DROP TABLE`
there is removed at once (configs/drop_delay.xml). It runs with Keeper and transactions enabled
(configs/transactions.xml), and uploads a part's blobs one at a time (configs/blob_upload_serial.xml).

The GC test runs on `node_gc` (configs/storage_conf_gc.xml): the same timing and a one-hour GC interval.
A manual round never takes the GC lease from another process, and a restart leaves the lease with the
previous one, so this server is never restarted.

Each test drops the tables it leaves and scopes its fault rules and trace to its own pool, so the module
passes in any order and when repeated.
"""

import json
import os
import re
import subprocess
import threading
import time
import urllib.request

import pytest

from helpers.cluster import ClickHouseCluster

cluster = ClickHouseCluster(__file__)

POLICY = "cas_window"
SLOW_MS = 7000
WINDOW_S = 5
WINDOW_PREFIX = "/test/cas_request_window/"
CATALOG = WINDOW_PREFIX + "cas/ref_catalog"
STOP_POLICY = "cas_stop"
STOP_PREFIX = "/test/cas_request_stop/"
STOP_CATALOG = STOP_PREFIX + "cas/ref_catalog"
# `cas_attempt_timeout_ms` of `configs/storage_conf_stop.xml`: a parked request's socket gives up after it.
STOP_ATTEMPT_TIMEOUT_S = 8
# The store behind the proxy, which the servers can read directly.
STORE_URL = "http://rustfs1:11121"
GC_DISK = "disk_cas_gc"
# Without slashes, so it also matches a listing's URL-encoded `prefix` parameter.
GC_POOL = "cas_request_gc"


def _control(url, path, patch=None):
    data = None if patch is None else json.dumps(patch).encode()
    request = urllib.request.Request(
        url + path, data=data, headers={"Content-Type": "application/json"}, method="GET" if data is None else "POST"
    )
    with urllib.request.urlopen(request, timeout=10) as response:
        return json.loads(response.read().decode())


def _wait_until(probe, timeout):
    end = time.monotonic() + timeout
    while time.monotonic() < end:
        result = probe()
        if result:
            return result
        time.sleep(0.2)
    raise AssertionError("condition did not become true within {}s".format(timeout))


@pytest.fixture(scope="module")
def node():
    cluster.add_instance(
        "node",
        main_configs=["configs/storage_conf.xml", "configs/drop_cooldown.xml"],
        with_rustfs=True,
    )
    # `stay_alive` lets a test restart the server, which drops its cached ref tables.
    cluster.add_instance("node_stop", main_configs=["configs/storage_conf_stop.xml", "configs/drop_delay.xml",
                                                    "configs/transactions.xml", "configs/blob_upload_serial.xml"],
                         with_rustfs=True, with_zookeeper=True, stay_alive=True)
    cluster.add_instance("node_gc", main_configs=["configs/storage_conf_gc.xml"], with_rustfs=True)
    cluster.base_cmd.extend(["--file", os.path.join(os.path.dirname(__file__), "docker_compose_proxy.yml")])
    try:
        cluster.start()
        binding = subprocess.check_output(cluster.base_cmd + ["port", "s3proxy", "8474"], text=True).strip()
        cluster.proxy_url = "http://{}".format(binding)
        _wait_until(lambda: _control(cluster.proxy_url, "/healthz"), timeout=30)
        yield cluster.instances["node"]
    finally:
        cluster.shutdown()


@pytest.fixture(scope="module")
def node_stop(node):
    return cluster.instances["node_stop"]


@pytest.fixture(scope="module")
def node_gc(node):
    return cluster.instances["node_gc"]


def _drop_all_tables(instance):
    for name in instance.query("SELECT table FROM system.detached_tables WHERE database = 'default'").split():
        instance.query("ATTACH TABLE default.`{}`".format(name))
    for name in instance.query("SELECT name FROM system.tables WHERE database = 'default'").split():
        instance.query("DROP TABLE default.`{}` SYNC".format(name))


@pytest.fixture(autouse=True)
def clean_state(node, node_stop, node_gc):
    _control(cluster.proxy_url, "/config", {"reset": True})
    yield
    _control(cluster.proxy_url, "/config", {"reset": True})
    for instance in (node, node_stop, node_gc):
        _drop_all_tables(instance)


def _arm(**config):
    _control(cluster.proxy_url, "/config", dict(rate=1.0, trace=True, **config))


def _stats():
    return _control(cluster.proxy_url, "/stats")


def _create(node, name):
    node.query(
        "CREATE TABLE {} (id UInt64, payload String) ENGINE = MergeTree ORDER BY id "
        "SETTINGS storage_policy = '{}'".format(name, POLICY)
    )


def test_long_body_with_a_lost_response_commits(node):
    _create(node, "long_body")
    _arm(
        modes=["slow_then_drop"], methods=["PUT"], path_substring=[WINDOW_PREFIX, "/blobs/"], remaining_faults=1,
        slow_ms=SLOW_MS, trace_path_substring=[WINDOW_PREFIX, "/blobs/"],
    )
    # A random body, so no earlier run stored it and the `PUT` is not skipped.
    node.query("INSERT INTO long_body SELECT number, randomPrintableASCII(16) FROM numbers(1000)", timeout=60)

    stats = _stats()
    dropped = stats["drop_after_forward"]
    assert len(dropped) == 1, dropped
    key = dropped[0]["path"]
    assert dropped[0]["upstream_status"] == 200, dropped
    trace = stats["trace"]
    puts = [i for i, r in enumerate(trace) if r["method"] == "PUT" and r["path"] == key]
    # The HEAD proves the landed body, so it is never sent again.
    assert len(puts) == 1, trace
    assert any(r["method"] == "HEAD" and r["path"] == key and r["status"] == 200 for r in trace[puts[0] + 1:]), trace
    assert node.query("SELECT count() FROM long_body").strip() == "1000"


def _remount_attempts(node):
    return int(node.query("SELECT sum(value) FROM system.events WHERE event = 'CASRemountAttempts'").strip())


def test_slow_catalog_read_then_drop(node):
    # GC is off for this disk. A remount also reads the catalog, so the test checks that none started.
    _create(node, "slow_catalog")
    node.query("INSERT INTO slow_catalog VALUES (1, 'x')")
    remounts_before = _remount_attempts(node)
    retries_before = node.count_in_log("Will retry later").strip()
    _arm(
        modes=["trickle"], methods=["GET"], path_substring=CATALOG, remaining_faults=1,
        slow_ms=SLOW_MS, trace_path_substring=CATALOG,
    )
    drop_started_ms = time.monotonic() * 1000.0
    drop = node.get_query_request("DROP TABLE slow_catalog SYNC", timeout=90)

    def settled():
        trace = _stats()["trace"]
        return all(r["status"] is not None for r in trace) and all(
            r["outcome"] is not None for r in trace if r["fault"] == "trickle")

    # A failed removal is requeued only after the 600 s cooldown and logs "Will retry later".
    _wait_until(lambda: (drop.process.poll() is not None
                         or node.count_in_log("Will retry later").strip() != retries_before) and settled(), timeout=60)
    trace = sorted(_stats()["trace"], key=lambda r: r["t_mono_ms"])
    assert _remount_attempts(node) == remounts_before

    slowed = trace[0]
    assert slowed["fault"] == "trickle" and slowed["method"] == "GET" and slowed["status"] == 200, trace
    first_put = next((i for i, r in enumerate(trace) if r["method"] == "PUT" and r["status"] == 200), None)
    assert first_put is not None, trace
    assert slowed["outcome"] == "delivered", trace
    assert slowed["t_last_piece_mono_ms"] - slowed["t_mono_ms"] > WINDOW_S * 1000, trace
    assert slowed["t_mono_ms"] >= drop_started_ms, (drop_started_ms, trace)
    # The removal reads the catalog twice before its write; another attempt of the slowed read would
    # add a GET here.
    prefix = trace[:first_put + 1]
    assert len(prefix) == 3, trace
    assert prefix[1]["method"] == "GET" and prefix[1]["status"] == 200, trace
    assert prefix[1]["t_mono_ms"] > slowed["t_last_piece_mono_ms"], trace
    assert node.count_in_log("Will retry later").strip() == retries_before
    drop.get_answer()


def test_failing_catalog_read_gives_up_at_its_window(node):
    """Every catalog request answers 503. The `CREATE TABLE` fails in its first catalog read, which gives up at its
    request window: every attempt of that read is a `GET` sent within the window from the first one, and nothing is
    sent after it. One operation only, so this does not show that a later operation gets a window of its own."""
    _arm(modes=["503"], methods=["GET", "PUT", "HEAD"], path_substring=CATALOG, trace_path_substring=CATALOG)
    error = node.query_and_get_error("CREATE TABLE failing (id UInt64) ENGINE = MergeTree ORDER BY id "
                                     "SETTINGS storage_policy = '{}'".format(POLICY))
    assert re.search(r"read of '[^']*ref_catalog': gave up at the policy deadline after (\d+|zero|one or more) "
                     r"attempt\(s\)", error), error

    # The slack covers scheduling only. Waiting it out on the proxy's clock lets a late attempt show up.
    first_ms = _stats()["trace"][0]["t_mono_ms"]
    window_end_ms = first_ms + WINDOW_S * 1000 + 500
    _wait_until(lambda: _stats()["now_mono_ms"] > window_end_ms, timeout=2 * WINDOW_S)
    trace = _stats()["trace"]
    assert len(trace) > 1 and all(r["method"] == "GET" and r["status"] == 503 for r in trace), trace
    assert not [r for r in trace if r["t_mono_ms"] > window_end_ms], trace


def _rule(method, path_substring, action, count=1, body_substring=None):
    return {"method": method, "path_substring": path_substring, "action": action, "count": count,
            "body_substring": body_substring}


def _arm_rules(rules):
    _control(cluster.proxy_url, "/config",
             {"rules": rules, "trace": True, "trace_path_substring": STOP_PREFIX, "trace_bodies": True})


def _create_sql(name):
    return "CREATE TABLE {} (id UInt64, payload String) ENGINE = MergeTree ORDER BY id " \
           "SETTINGS storage_policy = '{}'".format(name, STOP_POLICY)


def _stop_and_release(node, query_id, path_substring, respond="forward", kill=True):
    """Parked request, stop, `is_cancelled`, release: nothing the stopped query does after the release
    can predate the stop. Returns the release time on the proxy's clock."""
    parked = _wait_until(
        lambda: [p for p in _control(cluster.proxy_url, "/parked")["parked"] if path_substring in p["path"]], timeout=60)
    assert len(parked) == 1, parked
    # The release must reach the parked request before its socket times out, or the stopped query sees a timeout
    # instead of the released answer. Killed and polled over HTTP: a client start per query is slow in a sanitizer
    # build.
    if kill:
        node.http_query("KILL QUERY WHERE query_id = '{}' ASYNC".format(query_id))
    # `ASYNC` returns before the query is marked cancelled.
    _wait_until(lambda: node.http_query(
        "SELECT is_cancelled FROM system.processes WHERE query_id = '{}'".format(query_id)).strip() == "1",
        timeout=STOP_ATTEMPT_TIMEOUT_S - 1)
    _control(cluster.proxy_url, "/release", {"id": parked[0]["id"], "respond": respond})
    released = [r for r in _stats()["trace"] if r["t_release_mono_ms"] is not None]
    assert len(released) == 1, released
    return released[0]["t_release_mono_ms"]


def _catalog_puts(trace):
    return [r for r in trace if r["method"] == "PUT" and r["path"].startswith(STOP_CATALOG)]


def _remaining_counts():
    return [r["count"] for r in _stats()["rules"]]


def _events(node, event):
    return int(node.query("SELECT sum(value) FROM system.events WHERE event = '{}'".format(event)).strip())


def _stop_settings(stop):
    return {"max_execution_time": 3, "timeout_overflow_mode": "throw"} if stop == "timeout" else None


def _catalog_entries(record):
    """The namespace entries of a traced catalog `PUT`, parsed from its body."""
    entries = (json.loads(line) for line in (record["body"] or "").splitlines())
    return [e for e in entries if e.get("kind") == "entry"]


def _namespace_of(trace, life):
    namespaces = {e["ns"] for r in _catalog_puts(trace) for e in _catalog_entries(r) if e["life"] == life}
    assert len(namespaces) == 1, (life, namespaces)
    return namespaces.pop()


def _life_of(trace, uuid):
    """The namespace life that the catalog writes in `trace` give the table with `uuid`. A temporary table's
    namespace carries the UUID URL-encoded."""
    forms = (uuid, uuid.replace("-", "%2D"))
    lives = {e["life"] for r in _catalog_puts(trace) for e in _catalog_entries(r) if any(f in e["ns"] for f in forms)}
    assert len(lives) == 1, (uuid, lives)
    return lives.pop()


def _table_uuid(node, name):
    return node.query("SELECT uuid FROM system.tables WHERE database = 'default' AND name = '{}'".format(name)).strip()


def _create_with_life(node, name):
    """Creates `name` on `STOP_POLICY` and returns its namespace life."""
    _arm_rules([])
    seen = len(_stats()["trace"])
    node.query(_create_sql(name))
    return _life_of(_stats()["trace"][seen:], _table_uuid(node, name))


def _stored_lines(node, path):
    """The text lines of the compressed object at `path`, read from the store past the proxy."""
    return node.http_query(
        "SELECT line FROM s3('{}{}', 'clickhouse', 'clickhouse', 'LineAsString', 'line String', 'zstd') "
        "FORMAT TSVRaw".format(STORE_URL, path.split("?")[0])).splitlines()


def _landed_log_records(trace, life):
    return [r for r in trace if r["method"] == "PUT" and r["status"] == 200
            and "cas/ns/stream/{}/_log/".format(life) in r["path"]]


def _log_ops(node, record):
    """The operations of a landed ref-log record."""
    return [json.loads(line) for line in _stored_lines(node, record["path"]) if line.startswith('{"op"')]


def _removal_records(node, trace, life):
    """The landed ref-log records of `life` in `trace` that carry the terminal `remove_namespace` operation and
    follow a landed catalog write that moved `life` to `Removing`."""
    marked = [r["t_mono_ms"] for r in _catalog_puts(trace)
              if r["status"] == 200 and _removing_body(life) in (r["body"] or "")]
    if not marked:
        return []
    return [r for r in _landed_log_records(trace, life) if r["t_mono_ms"] > marked[0]
            and any(op["op"] == "remove_namespace" for op in _log_ops(node, r))]


def _brief(trace):
    """`trace` without request bodies, for an assertion message."""
    return [(r["method"], r["path"], r["status"], r["rule"]) for r in trace]


def _removing_body(life):
    """A substring of exactly the catalog writes that name `life` as `Removing`."""
    return '"state":"removing","life":"{}"'.format(life)


@pytest.mark.parametrize("stop", ["kill", "timeout"])
def test_stop_after_accepted_conflicts_permits_next_put(node_stop, stop):
    """Three `PUT`s of a catalog write meet 412; the read that settles the third is parked while the query stops and
    then succeeds. An accepted conflict is an answer, not a failure, so the fourth `PUT` is sent and lands."""
    rules = []
    for action in ("forward", "forward", "hold"):
        rules += [_rule("PUT", STOP_CATALOG, "412"), _rule("GET", STOP_CATALOG, action)]
    _arm_rules(rules)
    query_id = "accepted_conflicts_" + stop
    request = node_stop.get_query_request(
        _create_sql(query_id), query_id=query_id, settings=_stop_settings(stop), timeout=120)
    released_ms = _stop_and_release(node_stop, query_id, STOP_CATALOG, kill=(stop == "kill"))
    _wait_until(lambda: request.process.poll() is not None, timeout=60)
    trace = _stats()["trace"]
    puts = _catalog_puts(trace)
    assert [r["status"] for r in puts[:4]] == [412, 412, 412, 200], trace
    assert puts[3]["t_mono_ms"] > released_ms, trace
    assert _remaining_counts() == [0] * 6
    # Whether the stopped `CREATE` still reports its stop after the catalog write is outside CAS.
    _, error = request.get_answer_and_error()
    if request.process.returncode != 0:
        assert re.search(r"Code: {}\b".format(394 if stop == "kill" else 159), error), error


@pytest.mark.parametrize("stop", ["kill", "timeout"])
def test_failed_settlement_after_stop_starts_no_further_put(node_stop, stop):
    """A catalog write meets 412 and its settling read is parked while the query stops, then answered with 503. That
    read failed after the stop, so the write ends with its conflict and no further `PUT` is sent."""
    _arm_rules([_rule("PUT", STOP_CATALOG, "412"), _rule("GET", STOP_CATALOG, "hold"),
                _rule("PUT", STOP_CATALOG, "412", count=1000)])
    query_id = "failed_settlement_" + stop
    request = node_stop.get_query_request(
        _create_sql(query_id), query_id=query_id, settings=_stop_settings(stop), timeout=120)
    released_ms = _stop_and_release(node_stop, query_id, STOP_CATALOG, respond="503", kill=(stop == "kill"))
    _wait_until(lambda: request.process.poll() is not None, timeout=60)
    trace = _stats()["trace"]
    assert [r["status"] for r in _catalog_puts(trace)] == [412], trace
    assert not [r for r in _catalog_puts(trace) if r["t_mono_ms"] > released_ms], trace
    error = request.get_error()
    # `ABORTED`: the conflict the settling read could not resolve.
    assert re.search(r"Code: 236\b", error) and "conflict" in error, error
    assert _remaining_counts() == [0, 0, 1000]


def test_kill_after_ambiguous_create_keeps_unresolved_exit(node_stop):
    _arm_rules([_rule("PUT", STOP_CATALOG, "503"), _rule("GET", STOP_CATALOG, "hold")])
    query_id = "ambiguous_create"
    request = node_stop.get_query_request(_create_sql("ambiguous_create"), query_id=query_id, timeout=120)
    _stop_and_release(node_stop, query_id, STOP_CATALOG)
    _wait_until(lambda: request.process.poll() is not None or len(_catalog_puts(_stats()["trace"])) > 1, timeout=60)
    trace = _stats()["trace"]
    assert [r["status"] for r in _catalog_puts(trace)] == [503], trace
    error = request.get_error()
    # The pause before the next decision vetoes it and returns the unresolved send.
    assert re.search(r"Code: 210\b", error) and "the write is unresolved (sent" in error, error
    assert _remaining_counts() == [0, 0]

    # Control: a live owner decides again after each ambiguous attempt until one lands.
    _control(cluster.proxy_url, "/config", {"reset": True})
    _arm_rules([_rule("PUT", STOP_CATALOG, "503", count=8)])
    node_stop.query(_create_sql("ambiguous_create_live"), timeout=120)
    statuses = [r["status"] for r in _catalog_puts(_stats()["trace"])]
    assert statuses[:9] == [503] * 8 + [200], statuses
    assert _remaining_counts() == [0]
    assert node_stop.query("EXISTS TABLE ambiguous_create_live").strip() == "1"


def test_killed_leader_with_chunk_in_flight_leaves_a_usable_lane(node_stop):
    """A killed ref-log leader whose chunk `PUT` fails settles it by a read that meets a 404. The S3 client
    reports that 404 as the leader's stop, so the lane may need one recovery; a concurrent insert into the
    table still commits and later inserts work. Whether the concurrent insert shares the leader's chunk is
    not observable here."""
    life = _create_with_life(node_stop, "ref_batch")
    wedged_before = _events(node_stop, "CASRefAppendWedged")
    _arm_rules([_rule("PUT", [STOP_PREFIX, "cas/ns/stream/{}/_log/".format(life)], "hold")])
    # A synchronous insert appends from the query's own thread, so the kill reaches the chunk's leader.
    sync = {"async_insert": 0}
    leader = node_stop.get_query_request("INSERT INTO ref_batch VALUES (1, 'leader')", query_id="ref_batch_leader",
                                         settings=sync, timeout=120)
    _wait_until(lambda: _control(cluster.proxy_url, "/parked")["parked"], timeout=60)
    neighbor = node_stop.get_query_request("INSERT INTO ref_batch VALUES (2, 'neighbor')", settings=sync, timeout=120)
    # The release answers 503 without forwarding, so the chunk's settling read meets a 404.
    released_ms = _stop_and_release(node_stop, "ref_batch_leader", "/_log/", respond="503")
    neighbor.get_answer()
    _wait_until(lambda: leader.process.poll() is not None, timeout=60)
    trace = _stats()["trace"]
    held = next(r for r in trace if r["rule"] == "hold")
    assert held["status"] == 503, trace
    assert [r for r in trace if r["method"] == "GET" and r["path"] == held["path"] and r["status"] == 404
            and r["t_mono_ms"] > released_ms], trace
    assert _events(node_stop, "CASRefAppendWedged") <= wedged_before + 1, trace
    assert _remaining_counts() == [0]
    assert node_stop.query("SELECT count() FROM ref_batch WHERE id = 2").strip() == "1"
    node_stop.query("INSERT INTO ref_batch VALUES (3, 'after')", settings=sync)
    assert node_stop.query("SELECT count() FROM ref_batch WHERE id = 3").strip() == "1"


def test_atomic_sync_drop_kill_releases_only_waiter(node_stop):
    """Killing `DROP TABLE ... SYNC` in an `Atomic` database stops only the waiting query. The background
    removal keeps its parked catalog request and completes in the same attempt."""
    life = _create_with_life(node_stop, "atomic_sync_drop")
    node_stop.query("INSERT INTO atomic_sync_drop VALUES (1, 'x')")
    uuid = _table_uuid(node_stop, "atomic_sync_drop")
    retries_before = node_stop.count_in_log("Will retry later").strip()
    _arm_rules([_rule("PUT", STOP_CATALOG, "hold", body_substring=_removing_body(life))])
    seen = len(_stats()["trace"])
    query_id = "atomic_sync_drop"
    request = node_stop.get_query_request("DROP TABLE atomic_sync_drop SYNC", query_id=query_id, timeout=120)
    parked = _wait_until(lambda: _control(cluster.proxy_url, "/parked")["parked"], timeout=60)
    assert len(parked) == 1, parked
    # Over HTTP: the parked write must still be parked when the kill returns.
    node_stop.http_query("KILL QUERY WHERE query_id = '{}' SYNC".format(query_id))
    _wait_until(lambda: request.process.poll() is not None, timeout=30)
    error = request.get_error()
    assert re.search(r"Code: 394\b", error), error
    assert _control(cluster.proxy_url, "/parked")["parked"] == parked

    _control(cluster.proxy_url, "/release", {"id": parked[0]["id"], "respond": "forward"})
    _wait_until(lambda: node_stop.query(
        "SELECT count() FROM system.dropped_tables WHERE uuid = '{}'".format(uuid)).strip() == "0", timeout=60)
    _wait_until(lambda: _removal_records(node_stop, _stats()["trace"][seen:], life), timeout=60)
    trace = _stats()["trace"][seen:]
    assert [r["status"] for r in _catalog_puts(trace)] == [200], _brief(trace)
    assert _remaining_counts() == [0]
    assert node_stop.count_in_log("Will retry later").strip() == retries_before


def _active_parts(node, name):
    return node.query("SELECT name FROM system.parts WHERE table = '{}' AND active ORDER BY name".format(name))


def _detach_cold(node, name):
    """Creates a one-part table of two rows, detaches it permanently and restarts the server so its ref table is not
    cached. Returns the table's namespace life and its active parts before the detach."""
    life = _create_with_life(node, name)
    node.query("INSERT INTO {} VALUES (1, 'a'), (2, 'b')".format(name))
    parts = _active_parts(node, name)
    node.query("DETACH TABLE {} PERMANENTLY".format(name))
    node.restart_clickhouse()
    _control(cluster.proxy_url, "/config", {"reset": True})
    return life, parts


def _assert_reattached(node, name, parts):
    """Every part of `_detach_cold`'s table is active again, and none was detached as broken."""
    assert _active_parts(node, name) == parts
    assert node.query("SELECT count() FROM {}".format(name)).strip() == "2"
    assert _broken_on_start(node, name) == "0"


def _cold_attach_with_parked_walk(node, name, respond, settings=None):
    """Runs `ATTACH TABLE` on `_detach_cold`'s table with the first `_log/` GET of its namespace life parked, stops
    the query and releases the GET with `respond`. Returns the request, the life's id, the release time and the
    active parts before the detach."""
    life, parts = _detach_cold(node, name)
    _arm_rules([_rule("GET", [STOP_PREFIX, "cas/ns/stream/{}/_log/".format(life)], "hold")])
    request = node.get_query_request("ATTACH TABLE {}".format(name), query_id=name, settings=settings, timeout=120)
    released_ms = _stop_and_release(node, name, "/_log/", respond=respond, kill=settings is None)
    _wait_until(lambda: request.process.poll() is not None, timeout=60)
    return request, life, released_ms, parts


def _broken_on_start(node, name):
    return node.query(
        "SELECT count() FROM system.detached_parts WHERE table = '{}' AND reason = 'broken-on-start'".format(name)).strip()


def test_killed_cold_attach_keeps_parts_active(node_stop):
    """A stopped `ATTACH TABLE` whose cold ref walk reads a healthy store, then a second `ATTACH` if the
    stop ended the first one: every part is active. When the first `ATTACH` fails, it fails in the ref walk
    before any part loads, so the `broken-on-start` check then covers only the second `ATTACH`."""
    request, _, _, parts = _cold_attach_with_parked_walk(node_stop, "cold_attach", respond="forward")
    _, error = request.get_answer_and_error()
    if request.process.returncode != 0:
        assert re.search(r"Code: 394\b", error), error
        node_stop.query("ATTACH TABLE cold_attach")
    _assert_reattached(node_stop, "cold_attach", parts)
    assert _remaining_counts() == [0]


def test_kill_during_cold_ref_walk(node_stop):
    """An `ATTACH TABLE` whose query timed out while its cold ref walk failed ends without another walk; a
    second query recovers the data. The timeout's 159 is a transient code, so only the owner check keeps
    recovery from walking again."""
    request, life, released_ms, parts = _cold_attach_with_parked_walk(
        node_stop, "cold_walk", respond="503", settings={"max_execution_time": 3, "timeout_overflow_mode": "throw"})
    error = request.get_error()
    assert re.search(r"Code: 159\b", error), error
    trace = _stats()["trace"]
    assert not [r for r in trace if life in r["path"] and r["t_mono_ms"] > released_ms], trace
    node_stop.query("ATTACH TABLE cold_walk")
    _assert_reattached(node_stop, "cold_walk", parts)


def test_kill_in_recovery_backoff_keeps_the_store_error(node_stop):
    """A cold `ATTACH TABLE` whose ref walk failed on store errors while the query was live, killed in the backoff
    before the next walk: no other walk starts, and the query ends with the walk's own retryable `NETWORK_ERROR`, not
    with its stop. A second `ATTACH` loads every part, and none is detached as broken."""
    name = "backoff_kill"
    life, parts = _detach_cold(node_stop, name)
    # Every read of the life's log fails, so the walk fails once its request window passes.
    _arm_rules([_rule("GET", [STOP_PREFIX, "cas/ns/stream/{}/_log/".format(life)], "503", count=1000)])

    def recovery_retries():
        return int(node_stop.http_query(
            "SELECT sum(value) FROM system.events WHERE event = 'CASRefRecoveryRetries'").strip() or 0)

    retries_before = recovery_retries()
    request = node_stop.get_query_request("ATTACH TABLE {}".format(name), query_id=name, timeout=120)
    # Counted right before the backoff, which lasts a second before the next walk. Polled and killed over HTTP:
    # starting a client per poll or for the kill can take most of that second in a sanitizer build.
    end = time.monotonic() + 60
    while recovery_retries() == retries_before:
        assert time.monotonic() < end and request.process.poll() is None, "the walk did not fail"
        time.sleep(0.02)
    life_gets_at_kill = len([r for r in _stats()["trace"] if life in r["path"]])
    node_stop.http_query("KILL QUERY WHERE query_id = '{}' ASYNC".format(name))
    _wait_until(lambda: request.process.poll() is not None, timeout=60)
    error = request.get_error()
    # The walk's own give-up on the life's log, not a retry-later of another request.
    assert re.search(r"Code: 210\b", error) and re.search(r"_log/.*gave up at the \w+ deadline", error, re.DOTALL), error
    assert len([r for r in _stats()["trace"] if life in r["path"]]) == life_gets_at_kill, "another walk started"

    _control(cluster.proxy_url, "/config", {"reset": True})
    node_stop.query("ATTACH TABLE {}".format(name))
    _assert_reattached(node_stop, name, parts)


def _blob_puts_after(trace, released_ms):
    return [r for r in trace if r["method"] == "PUT" and "/blobs/" in r["path"] and r["t_mono_ms"] > released_ms]


def test_kill_while_blob_head_is_parked_publishes_nothing(node_stop):
    """A query killed while the `HEAD` of its part's only blob, a new body, is parked: the `HEAD` is released and
    answers 404, which the S3 client reports as the query's stop, so the upload ends with 394 before any publication
    of that blob. No blob publication starts after the release, no part is committed, and the next insert succeeds.
    An outcome test: the check before a publication is not reached here; the unit tests cover it."""
    node_stop.query("CREATE TABLE blob_head_kill {}".format(ONE_BLOB_TABLE))
    _arm_rules([_rule("HEAD", [STOP_PREFIX, "/blobs/"], "hold")])
    request, query_id = _blob_fixture_insert(node_stop, "blob_head_kill", ONE_BLOB_INSERT)
    released_ms = _stop_and_release(node_stop, query_id, "/blobs/")
    _wait_until(lambda: request.process.poll() is not None, timeout=60)
    trace = _stats()["trace"]
    held = next(r for r in trace if r["rule"] == "hold")
    assert held["status"] == 404, trace
    assert not [r for r in trace if r["method"] == "PUT" and r["path"] == held["path"]], trace
    assert not _blob_puts_after(trace, released_ms), trace
    error = request.get_error()
    assert re.search(r"Code: 394\b", error), error
    assert _remaining_counts() == [0]
    assert node_stop.query("SELECT count() FROM blob_head_kill").strip() == "0"

    node_stop.query("INSERT INTO blob_head_kill VALUES (1, 'next')")
    assert node_stop.query("SELECT count() FROM blob_head_kill").strip() == "1"


def _blob_requests(trace, method):
    return [r for r in trace if r["method"] == method and "/blobs/" in r["path"]]


# One blob per part: a packed part keeps every file in `data.packed`.
ONE_BLOB_TABLE = "(id UInt64, payload String) ENGINE = MergeTree ORDER BY id " \
                 "SETTINGS storage_policy = '{}', min_bytes_for_full_part_storage = '1G'".format(STOP_POLICY)
ONE_BLOB_INSERT = "INSERT INTO {} SELECT number, randomPrintableASCII(100) FROM numbers(10000)"
# Two new blobs per part: the `Wide` column bodies are random. Uncompressed fixed-length values make the marks and
# the index the same in every part, so after the first part they are reused and only answer a `HEAD`. The second
# argument of `randomPrintableASCII` keeps the two calls distinct: one call would give both columns one body.
TWO_BLOB_TABLE = "(a String CODEC(NONE), b String CODEC(NONE)) ENGINE = MergeTree ORDER BY tuple() " \
                 "SETTINGS storage_policy = '{}', min_bytes_for_wide_part = 0, min_rows_for_wide_part = 0, " \
                 "min_bytes_for_full_part_storage = 0".format(STOP_POLICY)
TWO_BLOB_INSERT = "INSERT INTO {} SELECT randomPrintableASCII(100, 1), randomPrintableASCII(100, 2) FROM numbers(10000)"


def _checked_blob_fixture(node, name, table, insert, new_blobs):
    """Creates `name`, after checking on two other tables of the same shape that an insert of `insert` publishes
    exactly `new_blobs` blobs and reuses every other one. The first of them stores the reusable bodies."""
    for suffix in ("_warm", "_control"):
        node.query("CREATE TABLE {}{} {}".format(name, suffix, table))
    _arm_rules([])
    node.query(insert.format(name + "_warm"), settings={"async_insert": 0})
    seen = len(_stats()["trace"])
    node.query(insert.format(name + "_control"), settings={"async_insert": 0})
    trace = _stats()["trace"][seen:]
    puts = sorted({r["path"] for r in _blob_requests(trace, "PUT")})
    missing = sorted({r["path"] for r in _blob_requests(trace, "HEAD") if r["status"] == 404})
    assert len(puts) == new_blobs and puts == missing, \
        "the fixture of {} publishes {} blob(s), not {}: PUT {}, HEAD 404 {}".format(name, len(puts), new_blobs, puts, missing)
    node.query("CREATE TABLE {} {}".format(name, table))
    _control(cluster.proxy_url, "/config", {"reset": True})


def _blob_fixture_insert(node, name, insert):
    query_id = name + "_insert"
    request = node.get_query_request(insert.format(name), query_id=query_id, settings={"async_insert": 0}, timeout=120)
    return request, query_id


def test_kill_during_last_blob_upload_commits_the_part(node_stop):
    """The insert's part has one blob. A query killed while its `PUT` is parked: the `PUT` is released and lands, and
    adoption, promotion and the part rename are no safe points and meet no failure, so the part is committed. No blob
    publication starts after the release."""
    name = "blob_last_put_kill"
    _checked_blob_fixture(node_stop, name, ONE_BLOB_TABLE, ONE_BLOB_INSERT, new_blobs=1)
    _arm_rules([_rule("PUT", [STOP_PREFIX, "/blobs/"], "hold")])
    request, query_id = _blob_fixture_insert(node_stop, name, ONE_BLOB_INSERT)
    released_ms = _stop_and_release(node_stop, query_id, "/blobs/")
    _wait_until(lambda: request.process.poll() is not None, timeout=60)
    trace = _stats()["trace"]
    puts = _blob_requests(trace, "PUT")
    assert [(r["rule"], r["status"]) for r in puts] == [("hold", 200)], trace
    assert not _blob_puts_after(trace, released_ms), trace
    _, error = request.get_answer_and_error()
    # The query may still report its kill once the part is committed.
    if request.process.returncode != 0:
        assert re.search(r"Code: 394\b", error), error
    assert node_stop.query("SELECT count() FROM {}".format(name)).strip() == "10000"
    assert _remaining_counts() == [0]

    node_stop.query("INSERT INTO {} VALUES (100000, 'next')".format(name))
    assert node_stop.query("SELECT count() FROM {} WHERE id = 100000".format(name)).strip() == "1"


def test_kill_during_first_blob_upload_publishes_no_later_blob(node_stop):
    """The insert's part has two new blobs, published one after another. A query killed while the first `PUT` is
    parked: the `PUT` is released and lands, then the second blob's `HEAD` answers 404, which the S3 client reports as
    the query's stop. The second blob is not published, the insert ends with 394 and commits nothing."""
    name = "blob_first_put_kill"
    _checked_blob_fixture(node_stop, name, TWO_BLOB_TABLE, TWO_BLOB_INSERT, new_blobs=2)
    _arm_rules([_rule("PUT", [STOP_PREFIX, "/blobs/"], "hold")])
    request, query_id = _blob_fixture_insert(node_stop, name, TWO_BLOB_INSERT)
    released_ms = _stop_and_release(node_stop, query_id, "/blobs/")
    _wait_until(lambda: request.process.poll() is not None, timeout=60)
    trace = _stats()["trace"]
    held = next(r for r in trace if r["rule"] == "hold")
    assert held["status"] == 200, trace
    later_misses = [r for r in _blob_requests(trace, "HEAD")
                    if r["t_mono_ms"] > released_ms and r["path"] != held["path"] and r["status"] == 404]
    assert len(later_misses) == 1, trace
    assert not [r for r in _blob_requests(trace, "PUT") if r["path"] == later_misses[0]["path"]], trace
    assert not _blob_puts_after(trace, released_ms), trace
    error = request.get_error()
    assert re.search(r"Code: 394\b", error), error
    assert node_stop.query("SELECT count() FROM {}".format(name)).strip() == "0"
    assert _remaining_counts() == [0]

    node_stop.query("INSERT INTO {} VALUES ('next', 'next')".format(name))
    assert node_stop.query("SELECT count() FROM {}".format(name)).strip() == "1"


LIFE_LOG = re.compile(r"cas/ns/stream/([0-9a-f]+)/_log/")


def _landed_log_puts(trace):
    """The lives of the ref-log `PUT`s in `trace` that landed."""
    return [m.group(1) for r in trace if r["method"] == "PUT" and r["status"] == 200
            for m in [LIFE_LOG.search(r["path"])] if m]


def _killed_after_first_ref_append(node, query_id, sql, settings):
    """Runs `sql`, which creates a table and writes one part per second into it, and kills it once a ref-log append
    has landed, so the query owns committed CAS work when it stops. Merges are stopped meanwhile. Returns the request
    after the query ended, the life of the first append, which the caller attributes, and the length of the proxy
    trace when the kill was sent."""
    node.query("SYSTEM STOP MERGES")
    try:
        _arm_rules([])
        seen = len(_stats()["trace"])
        request = node.get_query_request(sql, query_id=query_id, settings=settings, timeout=120)
        appended = _wait_until(lambda: _landed_log_puts(_stats()["trace"][seen:]), timeout=60)
        killed_at = len(_stats()["trace"])
        node.query("KILL QUERY WHERE query_id = '{}' SYNC".format(query_id))
        _wait_until(lambda: request.process.poll() is not None, timeout=60)
    finally:
        node.query("SYSTEM START MERGES")
    return request, appended[0], killed_at


ONE_PART_PER_SECOND = {"max_block_size": 1, "max_insert_block_size": 1, "min_insert_block_size_rows": 0,
                       "min_insert_block_size_bytes": 0, "async_insert": 0}


ROLLED_BACK_CSN = 2**64 - 1


def _rolled_back_installs(node, trace, life, part):
    """The landed ref-log records of `life` in `trace` that install `part` with a manifest, also written in
    `trace`, whose `txn_version.txt` carries the rolled-back creation CSN."""
    namespace = _namespace_of(_stats()["trace"], life)
    rolled_back = set()
    for r in trace:
        if r["method"] == "PUT" and r["status"] == 200 and "/cas/manifests/" in r["path"]:
            lines = _stored_lines(node, r["path"])
            header = json.loads(lines[1])
            if header["namespace"] == namespace and "creation_csn: {}".format(ROLLED_BACK_CSN) in lines:
                rolled_back.add((header["epoch"], header["build"]))
    return [r for r in _landed_log_records(trace, life) for op in _log_ops(node, r)
            if op.get("new_ref") == part and (op["new_epoch"], op["new_build"]) in rolled_back]


def test_killed_implicit_transaction_insert_rolls_back(node_stop):
    """An `INSERT` in an implicit transaction killed after the transaction registered a part it created: the
    transaction rolls back and persists that part's rolled-back version. A ref-log record that lands after the kill
    and before the query ends installs the registered part with a manifest whose `txn_version.txt` carries the
    rolled-back creation CSN. Only the row inserted before remains, the server keeps running, and the next
    transactional insert succeeds."""
    name = "txn_kill"
    life = _create_with_life(node_stop, name)
    try:
        node_stop.query("SYSTEM STOP MERGES")
        node_stop.query("INSERT INTO {} VALUES (1000, 'first')".format(name))
        fatal_before = int(node_stop.count_in_log("<Fatal>").strip())
        query_id = "txn_kill_insert"
        request = node_stop.get_query_request(
            "INSERT INTO {} SELECT number, 'x' FROM numbers(30) WHERE sleepEachRow(1) = 0".format(name),
            query_id=query_id, settings=dict(ONE_PART_PER_SECOND, implicit_transaction=1), timeout=120)
        # A part of an uncommitted transaction is active with an unknown CSN once the transaction registered it.
        registered = _wait_until(lambda: node_stop.query(
            "SELECT name FROM system.parts WHERE table = '{}' AND active AND creation_csn = 0 LIMIT 1".format(name)).strip(),
            timeout=60)
        killed_at = len(_stats()["trace"])
        node_stop.query("KILL QUERY WHERE query_id = '{}' SYNC".format(query_id))
        _wait_until(lambda: request.process.poll() is not None, timeout=60)
        ended_at = len(_stats()["trace"])
        # A merge of transactional parts is not supported on a CAS disk.
        node_stop.query("SYSTEM STOP MERGES {}".format(name))
        error = request.get_error()
        assert re.search(r"Code: 394\b", error), error
        window = _stats()["trace"][killed_at:ended_at]
        assert _rolled_back_installs(node_stop, window, life, registered), (registered, window)
        # The cleanup may already have removed the rolled-back part.
        csn = node_stop.query(
            "SELECT creation_csn FROM system.parts WHERE table = '{}' AND name = '{}'".format(name, registered)).strip()
        assert csn in ("", str(ROLLED_BACK_CSN)), (registered, csn)
        assert node_stop.query("SELECT count() FROM {}".format(name)).strip() == "1"
        assert int(node_stop.count_in_log("<Fatal>").strip()) == fatal_before

        node_stop.query("INSERT INTO {} VALUES (100, 'next')".format(name),
                        settings={"implicit_transaction": 1, "async_insert": 0})
        assert node_stop.query("SELECT count() FROM {}".format(name)).strip() == "2"
    finally:
        # Its transactional parts must not meet a merge in a later test, and a server-wide `START MERGES` lifts
        # the table's own stop.
        try:
            node_stop.query("DROP TABLE IF EXISTS {} SYNC".format(name))
        finally:
            node_stop.query("SYSTEM START MERGES")


def _holder_errors(node):
    return int(node.count_in_log("<Error> TemporaryTableHolder").strip())


def _temporary_table_dropped(node, uuid):
    """The storage of the temporary table with `uuid` finished dropping its data."""
    return node.count_in_log("({}): dropAllData: done.".format(uuid)).strip() == "1"


def test_killed_temporary_table_query_is_dropped_at_disconnect(node_stop):
    """A `CREATE TEMPORARY TABLE ... AS SELECT` on a CAS disk killed after it committed a part fails with 394 and
    leaves the table in its session. The table is dropped when the client disconnects: its namespace moves to
    `Removing` in the catalog, its ref log ends with the terminal record, its storage finishes dropping, and the
    server keeps running."""
    fatal_before = int(node_stop.count_in_log("<Fatal>").strip())
    holder_errors_before = _holder_errors(node_stop)
    request, life, killed_at = _killed_after_first_ref_append(
        node_stop, "temporary_kill",
        "CREATE TEMPORARY TABLE temporary_kill (id UInt64, payload String) ENGINE = MergeTree ORDER BY id "
        "SETTINGS storage_policy = '{}' AS SELECT number, 'x' FROM numbers(30) WHERE sleepEachRow(1) = 0".format(STOP_POLICY),
        ONE_PART_PER_SECOND)
    error = request.get_error()
    assert re.search(r"Code: 394\b", error), error
    namespace = _namespace_of(_stats()["trace"], life)
    temporary = re.search(r"/_temporary_and_external_tables/_tmp_([0-9a-f%D]+)@cas@$", namespace)
    assert temporary, namespace
    uuid = temporary.group(1).replace("%2D", "-")
    _wait_until(lambda: _removal_records(node_stop, _stats()["trace"][killed_at:], life), timeout=60)
    _wait_until(lambda: _temporary_table_dropped(node_stop, uuid), timeout=60)
    # A failed drop is logged by the holder and leaves the table attached.
    assert _holder_errors(node_stop) == holder_errors_before
    assert int(node_stop.count_in_log("<Fatal>").strip()) == fatal_before
    assert node_stop.query("SELECT 1").strip() == "1"


def test_killed_replace_of_a_temporary_table_drops_the_replaced_table(node_stop):
    """`REPLACE TEMPORARY TABLE` on a CAS disk killed while it fills the new table, after it committed a part. The
    query ends with 394 and the session keeps the new table. The replaced table is dropped: its namespace moves to
    `Removing` in the catalog, its ref log ends with the terminal record, its storage finishes dropping, and the
    server keeps running. GC erases the catalog row later; it is off on this disk."""
    session = {"session_id": "replace_temporary_kill"}
    table = "(id UInt64, payload String) ENGINE = MergeTree ORDER BY id SETTINGS storage_policy = '{}'".format(STOP_POLICY)

    def session_query(sql, **params):
        return node_stop.http_query(sql, params=dict(session, **params), method="POST")

    def table_uuid():
        return session_query("SELECT uuid FROM system.tables WHERE is_temporary AND name = 'replace_kill'").strip()

    try:
        _arm_rules([])
        session_query("CREATE TEMPORARY TABLE replace_kill {}".format(table))
        node_stop.query("SYSTEM STOP MERGES")
        session_query("INSERT INTO replace_kill VALUES (1, 'x')")
        replaced_uuid = table_uuid()
        replaced_life = _life_of(_stats()["trace"], replaced_uuid)
        fatal_before = int(node_stop.count_in_log("<Fatal>").strip())
        holder_errors_before = _holder_errors(node_stop)

        query_id = "replace_temporary_kill"
        seen = len(_stats()["trace"])
        outcome = {}

        def replace():
            outcome["answer"], outcome["error"] = node_stop.http_query_and_get_answer_with_error(
                "REPLACE TEMPORARY TABLE replace_kill {} AS SELECT number, 'y' FROM numbers(30) WHERE sleepEachRow(1) = 0"
                .format(table), params=dict(session, query_id=query_id, wait_end_of_query=1, **ONE_PART_PER_SECOND),
                method="POST", timeout=120)

        thread = threading.Thread(target=replace)
        thread.start()
        try:
            # The fill starts after the session swapped in the new table, so a part of another life is the fill's.
            _wait_until(lambda: [life for life in _landed_log_puts(_stats()["trace"][seen:]) if life != replaced_life],
                        timeout=60)
            killed_at = len(_stats()["trace"])
            node_stop.query("KILL QUERY WHERE query_id = '{}' SYNC".format(query_id))
        finally:
            thread.join(timeout=120)
        assert not thread.is_alive(), "the killed REPLACE did not return"
        assert re.search(r"Code: 394\b", outcome["error"] or ""), outcome

        _wait_until(lambda: _removal_records(node_stop, _stats()["trace"][killed_at:], replaced_life), timeout=60)
        _wait_until(lambda: _temporary_table_dropped(node_stop, replaced_uuid), timeout=60)
        assert _holder_errors(node_stop) == holder_errors_before
        new_uuid = table_uuid()
        assert new_uuid and new_uuid != replaced_uuid, (replaced_uuid, new_uuid)
        assert int(node_stop.count_in_log("<Fatal>").strip()) == fatal_before
        assert node_stop.query("SELECT 1").strip() == "1"
    finally:
        node_stop.query("SYSTEM START MERGES")
        # Dropped now rather than when the session expires or the server stops, during a later test.
        session_query("DROP TEMPORARY TABLE IF EXISTS replace_kill")


def test_manual_gc_kill_preserves_owner_code(node_gc):
    """A killed `SYSTEM CAS GC RUN` whose drain's first catalog `GET` is parked returns 394 and sends no
    request of that round after the release. Mount renewals and `gc/hb` pulses continue and are not counted."""
    node_gc.query("CREATE TABLE gc_kill (id UInt64) ENGINE = MergeTree ORDER BY id SETTINGS storage_policy = 'cas_gc'")
    node_gc.query("INSERT INTO gc_kill VALUES (1)")
    # The first round adopts a generation; only then does a round's drain read the catalog.
    first = json.loads(node_gc.http_query("SYSTEM CAS GC RUN {}".format(GC_DISK),
                                          params={"default_format": "JSONEachRow"}, method="POST"))
    assert first["acquired_lease"] in (1, True), first
    catalog = GC_POOL + "/cas/ref_catalog"
    _control(cluster.proxy_url, "/config",
             {"rules": [_rule("GET", catalog, "hold")], "trace": True, "trace_path_substring": GC_POOL})
    query_id = "manual_gc_kill"
    request = node_gc.get_query_request("SYSTEM CAS GC RUN {}".format(GC_DISK), query_id=query_id, timeout=120)
    released_ms = _stop_and_release(node_gc, query_id, catalog)
    _wait_until(lambda: request.process.poll() is not None, timeout=60)
    error = request.get_error()
    assert re.search(r"Code: 394\b", error), error
    late = [r for r in _stats()["trace"] if r["t_mono_ms"] > released_ms
            and not r["path"].endswith("/mount") and "/gc/hb" not in r["path"]]
    assert not late, late
    assert _remaining_counts() == [0]


def test_shutdown_hands_atomic_drop_to_restart(node_stop):
    """Server shutdown stops the background removal of a dropped table after its `Live -> Removing` write and
    before the terminal ref-log record, so the dropped metadata survives the exit and the removal finishes
    after restart. One SIGTERM to the exact server PID: a second one, or SIGKILL, would end the process
    without the orderly shutdown under test."""
    name = "shutdown_drop"
    life = _create_with_life(node_stop, name)
    node_stop.query("INSERT INTO {} VALUES (1, 'x')".format(name))
    life_log = "cas/ns/stream/{}/_log/".format(life)
    uuid = _table_uuid(node_stop, name)
    # The database disk may be remote, so the dropped metadata is followed through the log, not the local filesystem.
    metadata_removed = "Removing metadata .*{}.sql of dropped table".format(uuid)
    removed_before = int(node_stop.count_in_log(metadata_removed).strip())
    stopped_before = int(node_stop.count_in_log("removal stopped by server shutdown").strip())
    shutting_down_before = int(node_stop.count_in_log("Waiting for current connections to close").strip())
    fatal_before = int(node_stop.count_in_log("<Fatal>").strip())
    # Taken before the hold, so only the kill and the log poll run before the parked `PUT`'s attempt timeout.
    pid = node_stop.get_process_pid("clickhouse")
    assert pid is not None

    _control(cluster.proxy_url, "/config", {"reset": True})
    # Only this table's `Live -> Removing` write names its life as `Removing`.
    _arm_rules([_rule("PUT", STOP_CATALOG, "hold", body_substring=_removing_body(life))])
    node_stop.query("DROP TABLE {}".format(name))
    parked = _wait_until(lambda: _control(cluster.proxy_url, "/parked")["parked"], timeout=60)
    assert len(parked) == 1, parked
    node_stop.exec_in_container(["bash", "-c", "kill -15 {}".format(pid)], user="root")
    # Logged right after `IsServerShuttingDown` is set, so the removal resumes under shutdown.
    _wait_until(lambda: int(node_stop.count_in_log("Waiting for current connections to close").strip())
                > shutting_down_before, timeout=30)
    _control(cluster.proxy_url, "/release", {"id": parked[0]["id"], "respond": "forward"})
    released_ms = [r for r in _stats()["trace"] if r["t_release_mono_ms"] is not None][0]["t_release_mono_ms"]
    _wait_until(lambda: node_stop.get_process_pid("clickhouse") is None, timeout=60)

    trace = _stats()["trace"]
    held = next(r for r in trace if r["rule"] == "hold")
    assert held["path"].startswith(STOP_CATALOG) and held["status"] == 200, trace
    assert _removing_body(life) in held["body"], held
    assert [r for r in trace if r["method"] == "PUT" and life_log in r["path"] and r["status"] == 200
            and r["t_mono_ms"] < held["t_mono_ms"]], trace
    assert not [r for r in trace if r["method"] == "PUT" and life_log in r["path"] and r["t_mono_ms"] > released_ms], trace
    assert int(node_stop.count_in_log(metadata_removed).strip()) == removed_before
    assert int(node_stop.count_in_log("removal stopped by server shutdown").strip()) > stopped_before
    # A crash during shutdown also ends the process; an orderly exit logs nothing fatal.
    assert int(node_stop.count_in_log("<Fatal>").strip()) == fatal_before

    restarted_ms = _stats()["trace"][-1]["t_mono_ms"]
    node_stop.start_clickhouse()
    # Only metadata that survived the exit can be found and removed by the restarted server.
    _wait_until(lambda: int(node_stop.count_in_log(metadata_removed).strip()) > removed_before, timeout=60)
    assert node_stop.query("SELECT count() FROM system.dropped_tables WHERE uuid = '{}'".format(uuid)).strip() == "0"

    def appended_through_terminal():
        appended = [r for r in _stats()["trace"]
                    if r["t_mono_ms"] > restarted_ms and r["method"] == "PUT" and life_log in r["path"]]
        terminal = [r for r in appended if r["status"] == 200 and _log_ops(node_stop, r)[-1]["op"] == "remove_namespace"]
        return appended if terminal else None

    # The retried removal may seal the previous writer epoch first.
    appended = _wait_until(appended_through_terminal, timeout=60)
    assert all(r["status"] == 200 for r in appended), _brief(appended)
