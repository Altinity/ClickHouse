"""
Upgrade of a content-addressed pool from the 26.6.4 image to deletion intents, and the token form the writer
records per transport.

The cases of the first part share one pool. Module fixtures build what a case needs, each on top of the one before:
the 26.6.4 image creates the pool at reader generation 1 and leaves condemned rows behind (`condemned_pool`); the
offline tool converts it once every server is stopped (`migrated_pool`); the current image then serves it under the
same server root (`upgraded_new_server`). Any one case can be selected alone. The cases that change the pool run in
file order when several are selected.
"""

import io
import os
import re
import shlex
import time

import pytest

from helpers.cluster import ClickHouseCluster

cluster = ClickHouseCluster(__file__)

OLD_TAG = "26.6.4.20001.altinityantalya"
CONFIG_DIR = os.path.join(os.path.dirname(os.path.realpath(__file__)), "configs")
CONFIG_D = "/etc/clickhouse-server/config.d"

DISK = "disk_cas_shared"
POOL = "migration_pool"
BLOBS_PREFIX = POOL + "/blobs/"
INTENTS_PREFIX = POOL + "/blob-intents/"

TOOL_DISK = "disk_cas_migrate"
TOOL_CONFIG = "tool_disk.xml"

MAX_ROUNDS = 40

# A bounded wait on the background GC loop of the upgraded server, which takes the stopped server's lease over.
GC_TAKEOVER_RETRIES = 120
GC_TAKEOVER_SLEEP = 1.0


@pytest.fixture(scope="module", autouse=True)
def start_cluster():
    cluster.add_instance(
        "old",
        main_configs=["configs/storage_conf_old.xml"],
        image="altinity/clickhouse-server",
        tag=OLD_TAG,
        with_installed_binary=True,
        installed_binary_supports_cas_logs=True,
        with_rustfs=True,
        stay_alive=True,
    )
    cluster.add_instance(
        "new",
        # No CAS disk at start: the current build refuses a pool below generation 4 at every open, so the disk
        # config lands only after the tool has converted the pool.
        main_configs=[],
        with_rustfs=True,
        stay_alive=True,
    )
    cluster.add_instance(
        "tokens",
        main_configs=["configs/storage_conf_tokens.xml"],
        with_rustfs=True,
        stay_alive=True,
    )
    try:
        cluster.start()
        yield cluster
    finally:
        cluster.shutdown()


def list_keys(prefix):
    objects = cluster.rustfs_client.list_objects(
        cluster.rustfs_bucket, prefix, recursive=True
    )
    return {o.object_name for o in objects}


def put_bytes(key, data):
    cluster.rustfs_client.put_object(cluster.rustfs_bucket, key, io.BytesIO(data), len(data))


def body_keys():
    return {k for k in list_keys(BLOBS_PREFIX) if not k.endswith(".meta")}


def body_tokens(keys):
    """The current ETag of each of `keys` that exists."""
    tokens = {}
    for key in keys:
        try:
            tokens[key] = cluster.rustfs_client.stat_object(cluster.rustfs_bucket, key).etag
        except Exception as e:
            if getattr(e, "code", None) != "NoSuchKey":
                raise
    return tokens


def marker_keys():
    return {k for k in list_keys(BLOBS_PREFIX) if k.endswith(".meta")}


def gc_round(node, disk):
    rows = node.query("SYSTEM CAS GC RUN '{}'".format(disk), parse="dict")
    assert len(rows) == 1, rows
    return rows[0]


def run_tool(node, *tool_args):
    """Run `clickhouse-disks cas-migrate-pool` in `node`'s container; returns (exit code, combined output)."""
    query = " ".join(["cas-migrate-pool", "--grace-seconds", "1", *tool_args])
    out = node.exec_in_container(
        [
            "bash",
            "-c",
            "/usr/bin/clickhouse disks -C /etc/clickhouse-server/config.xml --disk {} --save-logs --query {} 2>&1; "
            "echo EXIT=$?".format(TOOL_DISK, shlex.quote(query)),
        ],
        timeout=600,
    )
    code = int(out.rstrip().rsplit("EXIT=", 1)[1])
    return code, out


def stop_new_and_install_tool_disk():
    new = cluster.instances["new"]
    new.stop_clickhouse()
    new.copy_file_to_container(
        os.path.join(CONFIG_DIR, TOOL_CONFIG), "{}/{}".format(CONFIG_D, TOOL_CONFIG)
    )


def query_events(node, query_id, *events):
    node.query("SYSTEM FLUSH LOGS")
    row = node.query(
        "SELECT {} FROM system.query_log "
        "WHERE query_id = '{}' AND type = 'QueryFinish' FORMAT TSV".format(
            ", ".join("ProfileEvents['{}']".format(e) for e in events), query_id
        )
    ).strip()
    assert row, "no QueryFinish row for query {}".format(query_id)
    return dict(zip(events, map(int, row.split("\t"))))


@pytest.fixture(scope="module")
def condemned_pool():
    """The 26.6.4 image fills the pool and leaves condemned rows: `t_a` pending, `t_b` a candidate, `t_keep` live."""
    old = cluster.instances["old"]

    for table, offset in (("t_keep", 0), ("t_a", 1000000), ("t_b", 2000000)):
        old.query(
            "CREATE TABLE {} (a UInt64, s String) ENGINE = MergeTree ORDER BY a "
            "SETTINGS storage_policy = 'cas_shared'".format(table)
        )
        old.query(
            "INSERT INTO {} SELECT number + {}, toString(number) FROM numbers(1000)".format(
                table, offset
            )
        )
    keep_uuid = old.query(
        "SELECT uuid FROM system.tables WHERE database = 'default' AND name = 't_keep'"
    ).strip()

    # The 26.6.4 build writes a marker beside every body and no intent.
    assert marker_keys(), "the 26.6.4 image wrote no marker"
    assert not list_keys(INTENTS_PREFIX)

    # `t_a`: condemned by the first round after the drop and its intent confirmed by the next. `t_b` is dropped between the
    # two, so the second round leaves `t_a` pending (delete_pending) and `t_b` a candidate. No further round runs.
    old.query("DROP TABLE t_a SYNC")
    for _ in range(MAX_ROUNDS):
        if gc_round(old, DISK)["pending_candidates"] > 0:
            break
    else:
        pytest.fail("no round of the 26.6.4 image condemned the blobs of t_a")

    old.query("DROP TABLE t_b SYNC")
    for _ in range(MAX_ROUNDS):
        row = gc_round(old, DISK)
        if row["pending_candidates"] > 0:
            break
    else:
        pytest.fail("no round of the 26.6.4 image condemned the blobs of t_b")
    return {"keep_uuid": keep_uuid, "last_round": row}


def test_old_image_builds_condemned_rows(condemned_pool):
    assert marker_keys()
    assert not list_keys(INTENTS_PREFIX)
    assert condemned_pool["last_round"]["pending_retired"] > 0, condemned_pool


def test_tool_refuses_a_live_older_server(condemned_pool):
    old = cluster.instances["old"]
    stop_new_and_install_tool_disk()

    code, out = run_tool(cluster.instances["new"])
    assert code != 0, out
    assert "stop that server and run the tool again" in out, out
    assert "upgrade_node" in out, out

    # The refusal wrote no intent and removed no marker, and the older server still serves its data.
    assert not list_keys(INTENTS_PREFIX), out
    assert marker_keys()
    assert old.query("SELECT count() FROM t_keep").strip() == "1000"


def test_floor_three_refuses_the_old_image(condemned_pool):
    """The tool's M6 line says a 26.6 build can no longer open the pool once its floor is 3. The 26.6.4 image is
    started over the pool with `_pool_meta` raised to 3 by hand, then the original bytes are put back."""
    old = cluster.instances["old"]
    old.stop_clickhouse()
    key = POOL + "/_pool_meta"
    original = cluster.rustfs_client.get_object(cluster.rustfs_bucket, key).read()
    raised, count = re.subn(rb'("min_reader_generation":\s*"?)1("?)', rb"\g<1>3\g<2>", original)
    assert count == 1, original
    put_bytes(key, raised)
    try:
        old.start_clickhouse(expected_to_fail=True)
        assert old.contains_in_log("pool requires reader generation 3 but this build supports at most 1")
    finally:
        put_bytes(key, original)
    old.start_clickhouse()
    assert old.query("SELECT count() FROM t_keep").strip() == "1000"


@pytest.fixture(scope="module")
def migrated_pool(condemned_pool):
    """The tool's run over the pool with every server stopped, and what the pool held before it."""
    old = cluster.instances["old"]
    new = cluster.instances["new"]
    old.stop_clickhouse()
    stop_new_and_install_tool_disk()

    bodies = body_keys()
    assert bodies and marker_keys()
    old_body_tokens = body_tokens(bodies)

    # The opt-out is ignored below generation 3: every body still gets an intent.
    code, out = run_tool(new, "--trust-clean-markers")
    return {"bodies": bodies, "old_body_tokens": old_body_tokens, "code": code, "out": out}


def test_tool_completes_with_every_server_stopped(migrated_pool):
    bodies = migrated_pool["bodies"]
    code, out = migrated_pool["code"], migrated_pool["out"]
    assert code == 0, out
    assert "migration complete" in out, out
    assert "--trust-clean-markers is ignored" in out, out
    assert re.search(r"M3: [1-9][0-9]* condemned row\(s\)", out), out
    assert "M8: reader floor at 4" in out, out
    assert "servers of this build may start now" in out, out
    assert out.index("servers of this build may start now") < out.index("M9: "), out

    # Every body has an intent of its current incarnation, and no marker is left.
    intents = list_keys(INTENTS_PREFIX)
    for body in bodies:
        blob = body[len(BLOBS_PREFIX) :]
        assert any(
            key.startswith("{}{}/".format(INTENTS_PREFIX, blob)) for key in intents
        ), "no intent for {}\n{}".format(body, out)
    assert not marker_keys()


@pytest.fixture(scope="module")
def upgraded_new_server(condemned_pool, migrated_pool):
    """The current image serves the migrated pool under the old server's identity, with the old table attached."""
    assert migrated_pool["code"] == 0, migrated_pool["out"]
    old = cluster.instances["old"]
    new = cluster.instances["new"]

    new.exec_in_container(["rm", "-f", "{}/{}".format(CONFIG_D, TOOL_CONFIG)])
    # The upgraded server keeps the identity of the old one: its server root is owned by the old server's uuid.
    uuid = old.exec_in_container(["cat", "/var/lib/clickhouse/uuid"]).strip()
    new.exec_in_container(
        ["bash", "-c", "echo -n {} > /var/lib/clickhouse/uuid".format(uuid)]
    )
    new.copy_file_to_container(
        os.path.join(CONFIG_DIR, "storage_conf_new.xml"),
        "{}/storage_conf_new.xml".format(CONFIG_D),
    )
    new.start_clickhouse()
    # The server root of the old image: the table of the old image keeps its namespace.
    new.query(
        "ATTACH TABLE t_keep UUID '{}' (a UInt64, s String) ENGINE = MergeTree ORDER BY a "
        "SETTINGS storage_policy = 'cas_shared'".format(condemned_pool["keep_uuid"])
    )
    return new


def test_new_image_starts_and_old_image_is_refused(upgraded_new_server):
    old = cluster.instances["old"]
    old.start_clickhouse(expected_to_fail=True)
    assert old.contains_in_log(
        "pool requires reader generation 4 but this build supports at most 1"
    )


def test_data_reads_back(upgraded_new_server):
    new = upgraded_new_server
    assert new.query("SELECT count(), sum(a) FROM t_keep").strip() == "1000\t499500"
    assert new.query("SELECT sum(length(s)) FROM t_keep").strip() == "2890"


DUPLICATE_INSERT_EVENTS = (
    "CASBlobUploadFanoutTasks",
    "CASBlobHead",
    "CASBlobHeadMiss",
    "CASBlobIntentGet",
    "CASBlobPut",
    "CASBlobReuseIntentPresent",
    "CASBlobBodyPutAvoided",
)


@pytest.fixture(scope="module")
def duplicate_inserts(upgraded_new_server, migrated_pool):
    """The rows of `t_keep`, which the old image wrote, inserted twice into a new table."""
    new = upgraded_new_server
    new.query(
        "CREATE TABLE t_dup (a UInt64, s String) ENGINE = MergeTree ORDER BY a "
        "SETTINGS storage_policy = 'cas_shared'"
    )
    insert = "INSERT INTO t_dup SELECT number, toString(number) FROM numbers(1000)"
    new.query(insert, query_id="dup_first")
    first = query_events(new, "dup_first", *DUPLICATE_INSERT_EVENTS)
    old_tokens = migrated_pool["old_body_tokens"]
    republished = {
        key
        for key, token in body_tokens(old_tokens).items()
        if token != old_tokens[key]
    }
    new.query(insert, query_id="dup_second")
    second = query_events(new, "dup_second", *DUPLICATE_INSERT_EVENTS)
    return {"first": first, "second": second, "republished": republished, "old_tokens": old_tokens}


def test_duplicate_insert_reuses_with_two_reads(upgraded_new_server, duplicate_inserts):
    """
    A first insert of the old image's content meets bodies of the old image, which all carry an intent: it
    publishes those again, so their tokens change. The same insert once more finds only bodies without intent and
    reuses every one with `HEAD` and an intent `GET`, and sends no body.
    """
    first, second = duplicate_inserts["first"], duplicate_inserts["second"]
    blobs = first["CASBlobUploadFanoutTasks"]
    assert blobs > 0, first
    assert first["CASBlobHead"] + first["CASBlobHeadMiss"] == blobs, first
    # A body found by the first insert is one of the old image's: it has an intent, so it is published again.
    assert first["CASBlobHead"] > 0, first
    assert first["CASBlobReuseIntentPresent"] == first["CASBlobHead"], first
    assert first["CASBlobIntentGet"] == first["CASBlobHead"], first
    assert first["CASBlobPut"] == blobs, first
    assert first["CASBlobBodyPutAvoided"] == 0, first
    assert len(duplicate_inserts["republished"]) == first["CASBlobHead"], duplicate_inserts
    print("first insert:", first)

    assert second["CASBlobUploadFanoutTasks"] == blobs, second
    assert second["CASBlobHead"] == blobs, second
    assert second["CASBlobHeadMiss"] == 0, second
    assert second["CASBlobIntentGet"] == blobs, second
    assert second["CASBlobPut"] == 0, second
    assert second["CASBlobReuseIntentPresent"] == 0, second
    assert second["CASBlobBodyPutAvoided"] == blobs, second
    print("second insert:", second)
    assert upgraded_new_server.query("SELECT count() FROM t_dup").strip() == "2000"


def test_fsck_reports_no_dangling_ref_and_no_marker(upgraded_new_server, migrated_pool, duplicate_inserts):
    new = upgraded_new_server

    # The bodies of `t_a` and `t_b`: old bodies that no table of the new image published again. The background
    # loop takes the stopped server's lease over and reclaims them through the condemned rows the old image left;
    # poll until they and their intents are gone.
    reclaimed = set(migrated_pool["old_body_tokens"]) - duplicate_inserts["republished"]
    assert reclaimed, migrated_pool

    def leftovers():
        intents = list_keys(INTENTS_PREFIX)
        return {
            key
            for key in reclaimed
            if key in body_keys()
            or any(i.startswith(INTENTS_PREFIX + key[len(BLOBS_PREFIX) :] + "/") for i in intents)
        }

    for _ in range(GC_TAKEOVER_RETRIES):
        gc_round(new, DISK)
        if not leftovers():
            break
        time.sleep(GC_TAKEOVER_SLEEP)
    else:
        pytest.fail("the bodies the old image condemned were not reclaimed: {}".format(sorted(leftovers())))

    # Deleted by the pending rows, the exact-token path, not by any other.
    new.query("SYSTEM FLUSH LOGS")
    bodies_deleted = int(
        new.query(
            "SELECT sum(entries_bodies_deleted) FROM system.cas_gc_log "
            "WHERE disk_name = '{}' AND event_type = 'Finish'".format(DISK)
        ).strip()
    )
    assert bodies_deleted >= len(reclaimed), bodies_deleted

    fsck = new.query("SYSTEM CAS FSCK '{}'".format(DISK), parse="dict")
    assert len(fsck) == 1, fsck
    report = fsck[0]
    # The hard findings of `cas-fsck` (`kFsckHardFindings`), and the markers the tool removed.
    for finding in (
        "dangling",
        "corrupted_runs",
        "stale_edge",
        "chain_broken",
        "lifeless_keys",
        "intent_malformed",
        "intent_unread",
        "pending_without_intent",
        "legacy_marker",
    ):
        assert report[finding] == 0, (finding, report)
    assert report["reachable"] > 0, report
    assert new.query("SELECT count(), sum(a) FROM t_keep").strip() == "1000\t499500"


# The token a body is published with, per transport. The intent key of a condemned body carries the store's
# `HEAD` token (S3: the ETag without its quotes, hex-encoded), so the key shows which form the writer recorded.
LARGE_ROWS = 50000  # about 50 MB of incompressible payload, over the 32 MiB multipart threshold
TOKEN_CASES = [
    # (id, policy, rows, payload bytes per row, the ETag must be a multipart ETag)
    ("streamed_single_part", "cas_local_stage", 1000, 100, False),
    ("streamed_multipart", "cas_local_stage", LARGE_ROWS, 1024, True),
    ("copied_single_part", "cas_s3_stage", 1000, 100, False),
    ("copied_multipart", "cas_s3_stage", LARGE_ROWS, 1024, True),
]
TOKEN_POOLS = {
    "cas_local_stage": ("token_pool_local", "disk_cas_local_stage"),
    "cas_s3_stage": ("token_pool_s3", "disk_cas_s3_stage"),
}


def drain_pool(node, disk, pool):
    """Rounds until the pool holds no body and no intent: the tables of earlier cases are dropped, and a body
    pending delete would make the condemning round of the next case delete it."""
    for _ in range(MAX_ROUNDS):
        if not list_keys(pool + "/blobs/") and not list_keys(pool + "/blob-intents/"):
            return
        gc_round(node, disk)
    pytest.fail(
        "{} still holds bodies or intents after {} rounds: {}".format(
            pool, MAX_ROUNDS, sorted(list_keys(pool + "/blobs/") | list_keys(pool + "/blob-intents/"))
        )
    )


@pytest.mark.parametrize(
    "case_id, policy, rows, payload_bytes, multipart",
    TOKEN_CASES,
    ids=[c[0] for c in TOKEN_CASES],
)
def test_head_token_form_per_store(case_id, policy, rows, payload_bytes, multipart):
    node = cluster.instances["tokens"]
    pool, disk = TOKEN_POOLS[policy]
    blobs_prefix = pool + "/blobs/"
    intents_prefix = pool + "/blob-intents/"
    table = "t_token_" + case_id

    drain_pool(node, disk, pool)
    before = list_keys(blobs_prefix)
    node.query(
        "CREATE TABLE {} (a UInt64, p String) ENGINE = MergeTree ORDER BY a "
        "SETTINGS storage_policy = '{}'".format(table, policy)
    )
    node.query(
        "INSERT INTO {} SELECT number, randomString({}) FROM numbers({})".format(
            table, payload_bytes, rows
        )
    )
    created = list_keys(blobs_prefix) - before
    assert created, "the insert created no body"

    stats = {
        key: cluster.rustfs_client.stat_object(cluster.rustfs_bucket, key)
        for key in created
    }
    if multipart:
        assert any("-" in s.etag for s in stats.values()), {k: s.etag for k, s in stats.items()}

    # Condemn: the first round that creates intents leaves the bodies in place.
    node.query("DROP TABLE {} SYNC".format(table))
    for _ in range(MAX_ROUNDS):
        row = gc_round(node, disk)
        if row["entries_intent_confirmed"] > 0:
            break
    else:
        pytest.fail("no round condemned the bodies of {}".format(table))
    assert row["entries_bodies_deleted"] == 0, row

    intents = list_keys(intents_prefix)
    for key, stat in stats.items():
        blob = key[len(blobs_prefix) :]
        tag_hex = stat.etag.strip('"').encode().hex()
        expected = "{}{}/etag/{}".format(intents_prefix, blob, tag_hex)
        assert expected in intents, "no intent {} for body {} with ETag {}; intents: {}".format(
            expected, key, stat.etag, sorted(k for k in intents if blob in k)
        )
