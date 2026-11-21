"""
Statements that copy parts between tables without reading the data, run while GC rounds are running.

Every test re-inserts content that was just dropped (so its blobs carry GC rows), runs one statement
that adds a reference in another table and removes the source, and keeps GC rounds going from a
background thread. The value of a test is its oracle at the end, not a guaranteed interleaving:

* every surviving table reads back the recorded `count()` and `sum(cityHash64(*))`;
* `CHECK TABLE` passes;
* `SYSTEM CAS FSCK` and `clickhouse disks ... cas-fsck` report no dangling reference or other hard finding;
* the server logs, from the start of the test's workload, hold no `LOGICAL_ERROR`, `CORRUPTED_DATA` or missing blob.

The expected value is computed from the generating query alone, without any CAS table, and both the source and
the destination are compared to it.

A statement may legally fail with `NO_REPLICA_HAS_PART` when its source cannot be confirmed. It is retried a
bounded number of times and the refusals are counted; a statement that succeeded must leave every row readable.
That retry path was never taken in any recorded run, so it is not exercised.

`test_fetch_paused_before_confirm_then_source_dropped` is the only test that places an event at a chosen
point: it parks the receiver of a fetch before the confirm, drops the source and runs GC rounds on the source.
"""

import base64
import json
import re
import shlex
import threading
import time

import pytest

from helpers.cluster import ClickHouseCluster

cluster = ClickHouseCluster(__file__)

POLICY = "cas_shared"
DISK = "disk_cas_shared"
RO_DISK = "disk_cas_ro"

ROWS = 20000
MAX_ITERATIONS = 50
LOOP_SECONDS = 60
RETRIES = 5
DRAIN_MAX_ROUNDS = 200
DRAIN_QUIET_ROUNDS = 2

SCHEMA = (
    "(p UInt8, id UInt64, v UInt64, s String) ENGINE = MergeTree PARTITION BY p ORDER BY id "
    "SETTINGS storage_policy = '{}'".format(POLICY)
)

# Fields of the `SYSTEM CAS GC RUN` row that count as work done by a round.
GC_WORK_FIELDS = (
    "candidates_marked",
    "objects_deleted",
    "manifests_deleted",
    "entries_condemned",
    "entries_intent_confirmed",
    "entries_bodies_deleted",
    "pending_candidates",
    "pending_condemned",
)

# The hard findings of `SYSTEM CAS FSCK`: every term of the report's `clean` that the row carries.
FSCK_HARD_FINDINGS = (
    "dangling",
    "stale_edge",
    "corrupted_runs",
    "chain_broken",
    "lifeless_keys",
    "intent_malformed",
    "intent_unread",
    "pending_without_intent",
)

BAD_LOG_PATTERN = r"LOGICAL_ERROR|Logical error|CORRUPTED_DATA|[Bb]lob.* is missing|is missing.*[Bb]lob"

_seed_counter = [1000]
_seed_lock = threading.Lock()


def next_seed():
    with _seed_lock:
        _seed_counter[0] += 1
        return _seed_counter[0]


@pytest.fixture(scope="module", autouse=True)
def start_cluster():
    cluster.add_instance(
        "node1",
        main_configs=["configs/storage_conf.xml", "configs/server_root_id_node1.xml"],
        macros={"replica": "node1"},
        with_rustfs=True,
        with_zookeeper=True,
        stay_alive=True,
    )
    cluster.add_instance(
        "node2",
        main_configs=["configs/storage_conf.xml", "configs/server_root_id_node2.xml"],
        macros={"replica": "node2"},
        with_rustfs=True,
        with_zookeeper=True,
        stay_alive=True,
    )
    cluster.add_instance(
        "node3",
        main_configs=["configs/storage_conf_rebuild.xml"],
        with_rustfs=True,
        stay_alive=True,
    )
    try:
        cluster.start()
        yield cluster
    finally:
        cluster.shutdown()


def content(seed, p=1):
    return (
        "SELECT {p}, number, cityHash64(number, {seed}), toString(cityHash64(number, {seed}, 7)) "
        "FROM numbers({rows})".format(p=p, seed=seed, rows=ROWS)
    )


def expected_digest(seed, p=1):
    """`(count, sum(cityHash64(*)))` of the generated rows, computed without any table."""
    out = cluster.instances["node1"].query(
        "SELECT count(), sum(cityHash64(*)) FROM ({})".format(content(seed, p))
    ).strip()
    count, total = out.split("\t")
    return int(count), int(total)


def digest(node, table):
    out = node.query("SELECT count(), sum(cityHash64(*)) FROM {}".format(table)).strip()
    count, total = out.split("\t")
    return int(count), int(total)


def gc_round(node):
    """One synchronous round on the shared pool; returns the result row as a dict."""
    out = node.query("SYSTEM CAS GC RUN {}".format(DISK), settings={"default_format": "JSONEachRow"}).strip()
    return json.loads(out)


def gc_rounds(nodes, count):
    for _ in range(count):
        for node in nodes:
            gc_round(node)


class GcLoop(threading.Thread):
    """Runs rounds on every node until stopped, like a background scheduler with a short period."""

    def __init__(self, nodes):
        super().__init__(daemon=True)
        self.nodes = nodes
        self.stopping = threading.Event()
        self.rounds = 0
        self.errors = []

    def run(self):
        while not self.stopping.is_set():
            for node in self.nodes:
                try:
                    gc_round(node)
                    self.rounds += 1
                except Exception as e:  # a failed round is recorded, the test oracle decides
                    self.errors.append(str(e)[:300])
            self.stopping.wait(0.05)

    def finish(self):
        self.stopping.set()
        self.join(120)
        assert not self.is_alive(), "the GC loop did not stop"


def drain_gc(nodes):
    """Rounds on every node until the leader reports no work twice in a row; returns the rounds run."""
    quiet = 0
    for rounds in range(1, DRAIN_MAX_ROUNDS + 1):
        for node in nodes:
            row = gc_round(node)
            if not row["acquired_lease"] or row["deferred"]:
                continue
            if sum(int(row[f]) for f in GC_WORK_FIELDS) == 0:
                quiet += 1
            else:
                quiet = 0
            if quiet >= DRAIN_QUIET_ROUNDS:
                return rounds
    raise AssertionError("GC did not drain in {} rounds".format(DRAIN_MAX_ROUNDS))


class Stats:
    def __init__(self, label):
        self.label = label
        self.iterations = 0
        self.succeeded = 0
        self.refused = 0
        self.gave_up = 0

    def __str__(self):
        return "{}: iterations={} statements_succeeded={} refused_and_retried={} gave_up_after_retries={}".format(
            self.label, self.iterations, self.succeeded, self.refused, self.gave_up
        )


def run_statement(node, sql, stats, undo=None):
    """Runs a statement, retrying a bounded number of times while it is refused with NO_REPLICA_HAS_PART."""
    for _ in range(RETRIES + 1):
        try:
            node.query(sql)
            stats.succeeded += 1
            return True
        except Exception as e:
            err = str(e)
        if "NO_REPLICA_HAS_PART" not in err and "Code: 234" not in err:
            raise AssertionError("{} failed with an unexpected error: {}".format(sql, err))
        stats.refused += 1
        if undo:
            undo()
    stats.gave_up += 1
    return False


def condemn_content(nodes, seed, rounds):
    """Insert content, drop it and let `rounds` GC rounds see it, so a re-insert meets blobs with GC rows."""
    node = nodes[0]
    node.query("CREATE TABLE w_{} {}".format(seed, SCHEMA))
    node.query("INSERT INTO w_{} {}".format(seed, content(seed)))
    node.query("DROP TABLE w_{} SYNC".format(seed))
    gc_rounds(nodes, rounds)


SERVER_LOG = "/var/log/clickhouse-server/clickhouse-server.log"


def log_offsets(nodes):
    """Current byte size of every node's server log: the start of what a test's workload may be blamed for."""
    return {
        node.name: int(node.exec_in_container(["bash", "-c", "stat -c %s {}".format(SERVER_LOG)]).strip())
        for node in nodes
    }


def pool_check(nodes, offsets):
    """The pool-level oracle: fsck through SQL and through the offline tool, and the server logs since `offsets`."""
    for node in nodes:
        row = json.loads(node.query("SYSTEM CAS FSCK {}".format(DISK), settings={"default_format": "JSONEachRow"}).strip())
        bad = {k: row[k] for k in FSCK_HARD_FINDINGS if int(row[k]) != 0}
        assert not bad, "SYSTEM CAS FSCK on {} has hard findings: {}".format(node.name, bad)

        offline = node.exec_in_container(
            [
                "bash",
                "-c",
                "/usr/bin/clickhouse disks -C /etc/clickhouse-server/config.xml "
                "--disk {} --save-logs --query {}".format(RO_DISK, shlex.quote("cas-fsck")),
            ]
        )
        dangling = re.search(r"dangling=(\d+)", offline)
        assert dangling, "no `dangling=` in the offline cas-fsck output: {}".format(offline)
        assert int(dangling.group(1)) == 0, offline

        size = int(node.exec_in_container(["bash", "-c", "stat -c %s {}".format(SERVER_LOG)]).strip())
        assert size >= offsets[node.name], "the server log of {} was rotated during the test".format(node.name)
        found = node.exec_in_container(
            [
                "bash",
                "-c",
                "tail -c +{} {} | grep -a -E {} | head -20 || true".format(
                    offsets[node.name] + 1, SERVER_LOG, shlex.quote(BAD_LOG_PATTERN)
                ),
            ]
        )
        assert not found.strip(), "server log of {} holds bad lines:\n{}".format(node.name, found)


def finish(nodes, survivors, stats, gc_loop, offsets):
    """The oracle after the workload: drain GC, re-read every survivor, check the pool, drop everything."""
    gc_loop.finish()
    rounds = drain_gc(nodes)
    wrong = []
    for node, table, expected in survivors:
        if digest(node, table) != expected:
            wrong.append((table, expected, digest(node, table)))
        check = node.query("CHECK TABLE {}".format(table), settings={"check_query_single_value_result": 1}).strip()
        if check != "1":
            wrong.append((table, "CHECK TABLE", check))
    assert not wrong, "tables that read back differently after the workload: {}".format(wrong)
    pool_check(nodes, offsets)
    for node, table, _ in survivors:
        node.query("DROP TABLE IF EXISTS {} SYNC".format(table))
    drain_gc(nodes)
    pool_check(nodes, offsets)
    print(
        "{} survivors_checked={} background_rounds={} background_round_errors={} drain_rounds={}".format(
            stats, len(survivors), gc_loop.rounds, len(gc_loop.errors), rounds
        )
    )
    assert stats.iterations > 0
    # Half of the iterations the test reached: refusals are legal but must stay the exception.
    minimum = max(1, stats.iterations // 2)
    assert stats.succeeded >= minimum, "only {} statements succeeded in {} iterations".format(
        stats.succeeded, stats.iterations
    )
    assert not gc_loop.errors, gc_loop.errors[:3]


def run_workload(label, iteration):
    """Calls `iteration(i, stats, survivors)` for up to MAX_ITERATIONS or LOOP_SECONDS, under a GC loop."""
    nodes = [cluster.instances["node1"], cluster.instances["node2"]]
    stats = Stats(label)
    survivors = []
    offsets = log_offsets(nodes)
    gc_loop = GcLoop(nodes)
    gc_loop.start()
    deadline = time.time() + LOOP_SECONDS
    try:
        for i in range(MAX_ITERATIONS):
            if time.time() > deadline:
                break
            iteration(nodes, i, stats, survivors)
            stats.iterations += 1
    except BaseException:
        gc_loop.stopping.set()
        raise
    finish(nodes, survivors, stats, gc_loop, offsets)


def offset_rounds(i):
    """Different offsets between the re-insert and the statement: 0 to 5 synchronous rounds."""
    return i % 6


def new_pair(node, seed, dst_rows_seed=None):
    src, dst = "src_{}".format(seed), "dst_{}".format(seed)
    node.query("CREATE TABLE {} {}".format(src, SCHEMA))
    node.query("CREATE TABLE {} {}".format(dst, SCHEMA))
    node.query("INSERT INTO {} {}".format(src, content(seed)))
    if dst_rows_seed is not None:
        node.query("INSERT INTO {} {}".format(dst, content(dst_rows_seed)))
    return src, dst


def copy_iteration(statement, preload_destination=False):
    def iteration(nodes, i, stats, survivors):
        node = nodes[0]
        seed = next_seed()
        condemn_content(nodes, seed, offset_rounds(i))
        dst_seed = next_seed() if preload_destination else None
        src, dst = new_pair(node, seed, dst_seed)
        expected = expected_digest(seed)
        assert digest(node, src) == expected, "the insert into the source wrote different rows"
        gc_rounds(nodes, offset_rounds(i + 1))
        done = run_statement(node, statement.format(src=src, dst=dst), stats)
        if done:
            node.query("DROP TABLE {} SYNC".format(src))
            assert digest(node, dst) == expected, "{} lost data".format(statement.format(src=src, dst=dst))
            survivors.append((node, dst, expected))
        else:
            assert digest(node, src) == expected, "the source lost data after a refused statement"
            node.query("DROP TABLE {} SYNC".format(src))
            node.query("DROP TABLE {} SYNC".format(dst))

    return iteration


def test_move_partition_to_table():
    run_workload("MOVE PARTITION TO TABLE", copy_iteration("ALTER TABLE {src} MOVE PARTITION 1 TO TABLE {dst}"))


def test_attach_partition_from_then_drop_source():
    run_workload("ATTACH PARTITION FROM + DROP source", copy_iteration("ALTER TABLE {dst} ATTACH PARTITION 1 FROM {src}"))


def test_replace_partition_from_then_drop_source():
    run_workload(
        "REPLACE PARTITION FROM + DROP source",
        copy_iteration("ALTER TABLE {dst} REPLACE PARTITION 1 FROM {src}", preload_destination=True),
    )


def test_clone_as_then_drop_source():
    def iteration(nodes, i, stats, survivors):
        node = nodes[0]
        seed = next_seed()
        condemn_content(nodes, seed, offset_rounds(i))
        src = "src_{}".format(seed)
        dst = "dst_{}".format(seed)
        node.query("CREATE TABLE {} {}".format(src, SCHEMA))
        node.query("INSERT INTO {} {}".format(src, content(seed)))
        expected = expected_digest(seed)
        assert digest(node, src) == expected, "the insert into the source wrote different rows"
        gc_rounds(nodes, offset_rounds(i + 1))
        done = run_statement(
            node,
            "CREATE TABLE {} CLONE AS {}".format(dst, src),
            stats,
            undo=lambda: node.query("DROP TABLE IF EXISTS {} SYNC".format(dst)),
        )
        if done:
            node.query("DROP TABLE {} SYNC".format(src))
            assert digest(node, dst) == expected, "CLONE AS then DROP of the source lost data"
            survivors.append((node, dst, expected))
        else:
            assert digest(node, src) == expected, "the source lost data after a refused CLONE AS"
            node.query("DROP TABLE {} SYNC".format(src))

    run_workload("CREATE TABLE ... CLONE AS + DROP source", iteration)


def test_rename_table_changing_the_namespace():
    node1 = cluster.instances["node1"]
    node1.query(
        "CREATE DATABASE IF NOT EXISTS ord ENGINE = Ordinary", settings={"allow_deprecated_database_ordinary": 1}
    )

    def iteration(nodes, i, stats, survivors):
        node = nodes[0]
        seed = next_seed()
        condemn_content(nodes, seed, offset_rounds(i))
        src, dst = "ord.src_{}".format(seed), "ord.dst_{}".format(seed)
        node.query("CREATE TABLE {} {}".format(src, SCHEMA))
        node.query("INSERT INTO {} {}".format(src, content(seed)))
        expected = expected_digest(seed)
        assert digest(node, src) == expected, "the insert into the source wrote different rows"
        gc_rounds(nodes, offset_rounds(i + 1))
        # A refused rename is retried as the same statement: the move is re-drivable.
        done = run_statement(node, "RENAME TABLE {} TO {}".format(src, dst), stats)
        if done:
            assert digest(node, dst) == expected, "RENAME TABLE lost data"
            survivors.append((node, dst, expected))
        else:
            node.query("DROP TABLE IF EXISTS {} SYNC".format(src))

    try:
        run_workload("RENAME TABLE (Ordinary database, namespace changes)", iteration)
    finally:
        node1.query("DROP DATABASE IF EXISTS ord SYNC")


def active_parts(node, table):
    return node.query(
        "SELECT name FROM system.parts WHERE database = currentDatabase() AND table = '{}' AND active "
        "ORDER BY name".format(table)
    ).split()


def create_replicated_pair(name):
    for node in (cluster.instances["node1"], cluster.instances["node2"]):
        node.query(
            "CREATE TABLE {} (p UInt8, id UInt64, v UInt64, s String) "
            "ENGINE = ReplicatedMergeTree('/clickhouse/tables/xcopy/{}', '{{replica}}') "
            "PARTITION BY p ORDER BY id SETTINGS storage_policy = '{}'".format(name, name, POLICY)
        )


def poll_until(predicate, what, timeout=120):
    """Bounded polling: the deadline only turns a hang into a failure, it orders nothing."""
    deadline = time.time() + timeout
    while not predicate():
        assert time.time() < deadline, "timed out waiting for {}".format(what)
        time.sleep(0.5)


def test_fetch_paused_before_confirm_then_source_dropped():
    """The fetch by relink is parked before its confirm; the source table is dropped and GC runs on the source.

    The source can no longer confirm, so the fetch may be refused or succeed. A refused fetch commits no rows;
    a fetch that commits reads back in full.
    """
    node1 = cluster.instances["node1"]
    node2 = cluster.instances["node2"]
    nodes = [node1, node2]
    offsets = log_offsets(nodes)
    seed = next_seed()
    table = "paused_{}".format(seed)
    expected = expected_digest(seed)
    create_replicated_pair(table)
    node2.query("SYSTEM STOP FETCHES {}".format(table))
    node1.query("INSERT INTO {} {}".format(table, content(seed)))
    assert digest(node1, table) == expected
    part = active_parts(node1, table)
    assert part

    node2.query("SYSTEM ENABLE FAILPOINT cas_relink_receiver_pause_before_confirm")
    released = False
    try:
        node2.query("SYSTEM START FETCHES {}".format(table))
        node2.query("SYSTEM WAIT FAILPOINT cas_relink_receiver_pause_before_confirm PAUSE", timeout=120)

        node1.query("DROP TABLE {} SYNC".format(table))
        # Rounds for longer than the condemn grace of the pool, so an unprotected body would be deleted.
        deadline = time.time() + 6
        while time.time() < deadline:
            gc_round(node1)
        node2.query("SYSTEM DISABLE FAILPOINT cas_relink_receiver_pause_before_confirm")
        released = True
    finally:
        if not released:
            node2.query("SYSTEM DISABLE FAILPOINT cas_relink_receiver_pause_before_confirm")

    refusal = re.compile(r"did not prove it still holds the manifest")

    def refused():
        log = node2.exec_in_container(["bash", "-c", "tail -c +{} {} || true".format(offsets["node2"] + 1, SERVER_LOG)])
        return refusal.search(log) is not None

    def active_rows():
        return int(
            node2.query(
                "SELECT sum(rows) FROM system.parts WHERE database = currentDatabase() AND table = '{}' AND active".format(
                    table
                )
            ).strip()
        )

    # The fetch ends in one of two ways: it commits the part with its rows, or the source refuses to confirm.
    poll_until(lambda: refused() or active_rows() > 0, "the paused fetch to commit or be refused")
    was_refused = refused()
    print("paused fetch: refused by the source = {}".format(was_refused))
    if was_refused:
        # The part-check thread may then replace the lost part with an empty part of the same name.
        assert active_rows() == 0, "a refused fetch committed rows: {}".format(active_parts(node2, table))
        assert int(node2.query("SELECT count() FROM {}".format(table)).strip()) == 0
        node2.query("SYSTEM FLUSH LOGS")
        downloaded = node2.query(
            "SELECT count() FROM system.part_log WHERE database = currentDatabase() AND table = '{}' "
            "AND event_type = 'DownloadPart' AND rows > 0".format(table)
        ).strip()
        assert downloaded == "0", "a refused fetch logged a downloaded part with rows"
    else:
        assert digest(node2, table) == expected, "the fetched part is committed but does not read back in full"
        check = node2.query("CHECK TABLE {}".format(table), settings={"check_query_single_value_result": 1}).strip()
        assert check == "1", check
    node2.query("DROP TABLE {} SYNC".format(table))
    drain_gc(nodes)
    pool_check(nodes, offsets)


def test_fetch_by_relink_then_drop_on_the_first_replica():
    node1 = cluster.instances["node1"]
    node2 = cluster.instances["node2"]

    def create_both(name):
        for node in (node1, node2):
            node.query(
                "CREATE TABLE {} (p UInt8, id UInt64, v UInt64, s String) "
                "ENGINE = ReplicatedMergeTree('/clickhouse/tables/xcopy/{}', '{{replica}}') "
                "PARTITION BY p ORDER BY id SETTINGS storage_policy = '{}'".format(name, name, POLICY)
            )

    def iteration(nodes, i, stats, survivors):
        seed = next_seed()
        # Content that is dropped on both replicas, so its blobs carry GC rows.
        first = "ra_{}".format(seed)
        create_both(first)
        node1.query("INSERT INTO {} {}".format(first, content(seed)))
        node2.query("SYSTEM SYNC REPLICA {}".format(first))
        node1.query("DROP TABLE {} SYNC".format(first))
        node2.query("DROP TABLE {} SYNC".format(first))
        gc_rounds(nodes, offset_rounds(i))

        # The re-insert on node1 is fetched by node2 by relink, then node1's table goes away.
        second = "rb_{}".format(seed)
        create_both(second)
        node2.query("SYSTEM STOP FETCHES {}".format(second))
        node1.query("INSERT INTO {} {}".format(second, content(seed)))
        expected = expected_digest(seed)
        assert digest(node1, second) == expected, "the insert on the first replica wrote different rows"
        part = active_parts(node1, second)
        assert part, "the insert on the first replica made no part"
        gc_rounds(nodes, offset_rounds(i + 1))
        node2.query("SYSTEM START FETCHES {}".format(second))
        node2.query("SYSTEM SYNC REPLICA {}".format(second))
        assert active_parts(node2, second) == part, "the second replica does not hold the part of the first"
        node1.query("DROP TABLE {} SYNC".format(second))
        assert digest(node2, second) == expected, "the fetched part lost data after the drop on the first replica"
        survivors.append((node2, second, expected))
        stats.succeeded += 1

    run_workload("fetch by relink + DROP on the first replica", iteration)


RO_WINDOW_CONFIG = """<clickhouse>
    <storage_configuration>
        <disks>
            <disk_cas_rebuild_ro>
                <type>object_storage</type>
                <object_storage_type>s3</object_storage_type>
                <metadata_type>cas</metadata_type>
                <cas_server_root_id>node3</cas_server_root_id>
                <endpoint>http://rustfs1:11121/test/xcopy_rebuild_pool/</endpoint>
                <access_key_id>clickhouse</access_key_id>
                <secret_access_key>clickhouse</secret_access_key>
                <readonly>true</readonly>
                <cas_gc_enabled>0</cas_gc_enabled>
            </disk_cas_rebuild_ro>
        </disks>
    </storage_configuration>
</clickhouse>
"""


def test_gc_rebuild_force_on_a_pool_with_gc_history():
    node = cluster.instances["node3"]
    rebuild_disk = "disk_cas_rebuild"
    rebuild_ro_disk = "disk_cas_rebuild_ro"

    node.query(
        "CREATE TABLE h (id UInt64, s String) ENGINE = MergeTree ORDER BY id "
        "SETTINGS storage_policy = 'cas_rebuild'"
    )
    node.query("INSERT INTO h SELECT number, toString(number) FROM numbers(20000)")
    before = node.query("SELECT count(), sum(cityHash64(*)) FROM h").strip()

    # GC history: at least one round of the background loop has finished on this pool.
    deadline = time.time() + 60
    while True:
        node.query("SYSTEM FLUSH LOGS")
        finished = int(
            node.query(
                "SELECT count() FROM system.cas_gc_log WHERE event_type = 'Finish' AND outcome = 'Success'"
            ).strip()
        )
        if finished > 0:
            break
        assert time.time() < deadline, "no GC round finished on the rebuild pool"
        time.sleep(1)

    # Observed behaviour, not a goal: the command takes a new GC identity and never takes over a lease.
    sql_error = node.query_and_get_error("SYSTEM CAS GC REBUILD FORCE {}".format(rebuild_disk))
    print("SYSTEM CAS GC REBUILD FORCE, server running, observed: {!r}".format(sql_error))
    assert "another GC leader holds the lease" in sql_error, sql_error

    node.stop_clickhouse()
    # The read-only window lives in a config of its own: declared in the server config it would be loaded
    # by the server as a second disk holding the table's parts.
    encoded = base64.b64encode(RO_WINDOW_CONFIG.encode()).decode()
    node.exec_in_container(["bash", "-c", "echo {} | base64 -d > /etc/clickhouse-server/ro_window.xml".format(encoded)])
    offline = node.exec_in_container(
        [
            "bash",
            "-c",
            "/usr/bin/clickhouse disks -C /etc/clickhouse-server/ro_window.xml --disk {} --save-logs "
            "--query {} 2>&1; echo EXIT=$?".format(rebuild_ro_disk, shlex.quote("cas-gc-rebuild --force")),
        ],
        nothrow=True,
    )
    print("clickhouse-disks cas-gc-rebuild --force, server stopped, observed: {!r}".format(offline))
    assert "another GC leader holds the lease" in offline, offline
    assert "EXIT=0" not in offline, offline

    node.start_clickhouse(start_wait_sec=150)
    assert node.query("SELECT count(), sum(cityHash64(*)) FROM h").strip() == before
    node.query("DROP TABLE h SYNC")
