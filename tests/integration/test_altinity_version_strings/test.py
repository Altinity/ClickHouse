#! /usr/bin/env python3

import argparse
import subprocess
import re
import signal
import time

import pytest

from helpers.cluster import ClickHouseCluster, ClickHouseInstance

VERSION_REGEX_QUERY = re.compile(r"(\d+\.\d+\.\d+.\d+.altinity[a-z]*)")
VERSION_REGEX_CLI = re.compile(
    r" (\d+\.\d+\.\d+.\d+.altinity[a-z]* \(altinity build\))"
)


@pytest.fixture(scope="module")
def started_node():
    cluster = ClickHouseCluster(__file__)
    try:
        node = cluster.add_instance("node", stay_alive=True)

        cluster.start()
        yield node
    finally:
        cluster.shutdown()


def send_signal(started_node: ClickHouseInstance, signal: int):
    started_node.exec_in_container(
        ["bash", "-c", f"pkill -{signal} clickhouse"], user="root"
    )


def test_stacktrace(started_node: ClickHouseInstance):
    query = "SELECT throwIf(1, 'throw')"
    result = started_node.exec_in_container(
        ["bash", "-c", f'clickhouse local --stacktrace -q "{query}" 2>&1'],
        nothrow=True,
    )
    assert "FunctionThrowIf::executeImpl" in result, "Stacktrace is not enabled"


def test_version(started_node: ClickHouseInstance):
    query = "SELECT version()"
    result = started_node.query(query)
    assert VERSION_REGEX_QUERY.search(
        result
    ), f"Version on query not formatted correctly, expected match for: '{VERSION_REGEX_QUERY.pattern}' but got: '{result}'"

    result = started_node.exec_in_container(["clickhouse", "--version"])
    assert VERSION_REGEX_CLI.search(
        result
    ), f"Version on cli not formatted correctly, expected match for: '{VERSION_REGEX_CLI.pattern}' but got: '{result}'"


def test_error_message(started_node: ClickHouseInstance):
    # Give it a moment to start up
    time.sleep(1)

    # Send error signal
    send_signal(started_node, signal.SIGSEGV)

    if started_node.contains_in_log("ClickHouse/issues"):
        assert started_node.contains_in_log(
            "github.com/Altinity/ClickHouse/issues"
        ), "ClickHouse/issues link is not correct"

    unexpected_messages = [
        "github.com/ClickHouse/ClickHouse",
        "not official",
        "(official build)",
    ]

    for message in unexpected_messages:
        match = started_node.grep_in_log(message)
        assert not match, f"Unexpected message '{message}' found in log: {match}"

    expected_messages = [
        "(altinity build)",
    ]

    for message in expected_messages:
        match = started_node.grep_in_log(message)
        assert match, f"Expected message '{message}' not found in log"

    version_messages = started_node.grep_in_log("(version ").splitlines()
    assert len(version_messages) > 0, "No version messages found in log"

    for version_message in version_messages:
        assert VERSION_REGEX_CLI.search(
            version_message
        ), f"Version is not formatted correctly, expected match for: '{VERSION_REGEX_CLI.pattern}' but got: {version_message}"


def test_issues_link(started_node: ClickHouseInstance):
    result = started_node.exec_in_container(
        ["bash", "-c", "strings $(which clickhouse) | grep -i clickhouse/issues"]
    )

    assert (
        "github.com/ClickHouse/ClickHouse/issues\n" not in result
    ), f"ClickHouse/issues link is not correct, expected to not find upstream link but got: '{result}'"

    assert (
        "github.com/Altinity/ClickHouse/issues\n" in result
    ), f"ClickHouse/issues link is not correct, expected to find Altinity link but got: '{result}'"
