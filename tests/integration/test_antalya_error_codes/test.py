import re

import pytest
import requests

from helpers.cluster import ClickHouseCluster


cluster = ClickHouseCluster(__file__)
origin = cluster.add_instance("origin", main_configs=["configs/errors.xml"])
relay = cluster.add_instance("relay", main_configs=["configs/errors.xml"])
edge = cluster.add_instance("edge", main_configs=["configs/errors.xml"])

ERRORS = [
    (10001, "CATALOG_NAMESPACE_DISABLED"),
    (10002, "PENDING_MUTATIONS_NOT_ALLOWED"),
    (10003, "EXPORT_PARTITION_ALREADY_EXPORTED"),
    (10004, "PARTITION_EXPORT_FAILED"),
    (10005, "CAS_WRITE_UNATTRIBUTED"),
    (10006, "CAS_DELETE_MARKER"),
]
SETTINGS = {"allow_custom_error_code_in_throwif": 1}


@pytest.fixture(scope="module", autouse=True)
def started_cluster():
    try:
        cluster.start()
        yield
    finally:
        cluster.shutdown()


@pytest.mark.parametrize("code", [-1, 779, 1009, 10000, 10007, 2147483647])
def test_unknown_code_is_not_logged_as_sentinel(code):
    message = f"unknown code {code} must not become sentinel"
    error = origin.query_and_get_error(
        f"SELECT throwIf(1, '{message}', toInt32({code}))", settings=SETTINGS
    )
    assert f"Code: {code}." in error
    origin.query("SYSTEM FLUSH LOGS")
    assert (
        origin.query(
            "SELECT count() FROM system.error_log "
            f"WHERE position(last_error_message, '{message}') > 0"
        )
        == "0\n"
    )
    assert (
        origin.query(
            "SELECT count() FROM system.query_log WHERE type IN "
            "('ExceptionBeforeStart', 'ExceptionWhileProcessing') "
            f"AND exception_code = {code} AND position(exception, '{message}') > 0"
        )
        == "1\n"
    )


@pytest.mark.parametrize("code,name", ERRORS)
def test_local_observability(code, name):
    message = f"Antalya error {code}"
    error = origin.query_and_get_error(
        f"SELECT throwIf(1, '{message}', toInt32({code}))", settings=SETTINGS
    )
    assert f"Code: {code}." in error
    assert name in error
    assert message in error
    assert (
        origin.query(
            f"SELECT count() FROM system.errors WHERE code = {code} "
            f"AND name = '{name}' AND remote = 0 AND value > 0 "
            f"AND position(last_error_message, '{message}') > 0"
        )
        == "1\n"
    )

    origin.query("SYSTEM FLUSH LOGS")
    assert (
        int(
            origin.query(
                f"SELECT count() FROM system.error_log WHERE code = {code} "
                f"AND remote = 0 AND value > 0 AND position(last_error_message, '{message}') > 0"
            )
        )
        > 0
    )

    response = requests.get(f"http://{origin.ip_address}:8001/metrics", timeout=10)
    response.raise_for_status()
    metric = re.search(rf"^ClickHouseErrorMetric_{name} (\d+)$", response.text, re.MULTILINE)
    assert metric is not None
    assert int(metric.group(1)) > 0


@pytest.mark.parametrize("code,name", [ERRORS[0], ERRORS[-1], (10007, "")])
def test_distributed_rethrow(code, name):
    table = f"vendor_error_{code}"
    message = f"Antalya distributed error {code}"
    try:
        origin.query(
            f"CREATE VIEW {table} AS SELECT throwIf(1, '{message}', toInt32({code})) AS value",
            settings=SETTINGS,
        )
        relay.query(
            f"CREATE VIEW {table} AS SELECT * FROM remote('origin', default, {table})"
        )
        error = edge.query_and_get_error(
            f"SELECT * FROM remote('relay', default, {table})", settings=SETTINGS
        )
        assert f"Code: {code}." in error
        assert message in error
        if name:
            assert name in error
            for node in (relay, edge):
                assert (
                    int(
                        node.query(
                            f"SELECT sum(value) FROM system.errors WHERE code = {code} "
                            f"AND name = '{name}' AND remote = 1"
                        )
                    )
                    > 0
                )
                node.query("SYSTEM FLUSH LOGS")
                assert (
                    int(
                        node.query(
                            f"SELECT count() FROM system.error_log WHERE code = {code} AND remote = 1"
                        )
                    )
                    > 0
                )
        else:
            assert (
                edge.query(
                    f"SELECT count() FROM system.errors WHERE code = {code} "
                    "SETTINGS system_events_show_zero_values = 1"
                )
                == "0\n"
            )
    finally:
        relay.query(f"DROP VIEW IF EXISTS {table}")
        origin.query(f"DROP VIEW IF EXISTS {table}")
