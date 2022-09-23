#!/usr/bin/env python3
import os
import sys

from testflows.core import *

append_path(sys.path, "..")

from helpers.cluster import Cluster
from helpers.argparser import argparser
from helpers.common import check_clickhouse_version
from map_type.requirements import SRS018_ClickHouse_Map_Data_Type

xfails = {
    "tests/table map with key integer/Int:": [
        (Fail, "https://github.com/ClickHouse/ClickHouse/issues/21032")
    ],
    "tests/table map with value integer/Int:": [
        (Fail, "https://github.com/ClickHouse/ClickHouse/issues/21032")
    ],
    "tests/table map with key integer/UInt256": [
        (Fail, "https://github.com/ClickHouse/ClickHouse/issues/21031")
    ],
    "tests/table map with value integer/UInt256": [
        (Fail, "https://github.com/ClickHouse/ClickHouse/issues/21031")
    ],
    "tests/select map with key integer/Int64": [
        (Fail, "https://github.com/ClickHouse/ClickHouse/issues/21030")
    ],
    "tests/select map with value integer/Int64": [
        (Fail, "https://github.com/ClickHouse/ClickHouse/issues/21030")
    ],
    "tests/cast tuple of two arrays to map/string -> int": [
        (Fail, "https://github.com/ClickHouse/ClickHouse/issues/21029")
    ],
    "tests/mapcontains/null key in map": [
        (Fail, "https://github.com/ClickHouse/ClickHouse/issues/21028")
    ],
    "tests/mapcontains/null key not in map": [
        (Fail, "https://github.com/ClickHouse/ClickHouse/issues/21028")
    ],
    "tests/mapkeys/null key not in map": [
        (Fail, "https://github.com/ClickHouse/ClickHouse/issues/21028")
    ],
    "tests/mapkeys/null key in map": [
        (Fail, "https://github.com/ClickHouse/ClickHouse/issues/21028")
    ],
    "tests/mapcontains/select nullable key": [
        (Fail, "https://github.com/ClickHouse/ClickHouse/issues/21026")
    ],
    "tests/mapkeys/select keys from column": [
        (Fail, "https://github.com/ClickHouse/ClickHouse/issues/21026")
    ],
    "tests/table map select key with value string/LowCardinality:": [
        (Fail, "https://github.com/ClickHouse/ClickHouse/issues/21406")
    ],
    "tests/table map select key with key string/FixedString": [
        (Fail, "https://github.com/ClickHouse/ClickHouse/issues/21406")
    ],
    "tests/table map select key with key string/Nullable": [
        (Fail, "https://github.com/ClickHouse/ClickHouse/issues/21406")
    ],
    "tests/table map select key with key string/Nullable(NULL)": [
        (Fail, "https://github.com/ClickHouse/ClickHouse/issues/21026")
    ],
    "tests/table map select key with key string/LowCardinality:": [
        (Fail, "https://github.com/ClickHouse/ClickHouse/issues/21406")
    ],
    "tests/table map select key with key integer/Int:": [
        (Fail, "https://github.com/ClickHouse/ClickHouse/issues/21032")
    ],
    "tests/table map select key with key integer/UInt256": [
        (Fail, "https://github.com/ClickHouse/ClickHouse/issues/21031")
    ],
    "tests/table map select key with key integer/toNullable": [
        (Fail, "https://github.com/ClickHouse/ClickHouse/issues/21406")
    ],
    "tests/table map select key with key integer/toNullable(NULL)": [
        (Fail, "https://github.com/ClickHouse/ClickHouse/issues/21026")
    ],
    "tests/select map with key integer/Int128": [
        (Fail, "large Int128 as key not supported")
    ],
    "tests/select map with key integer/Int256": [
        (Fail, "large Int256 as key not supported")
    ],
    "tests/select map with key integer/UInt256": [
        (Fail, "large UInt256 as key not supported")
    ],
    "tests/select map with key integer/toNullable": [
        (Fail, "Nullable type as key not supported")
    ],
    "tests/select map with key integer/toNullable(NULL)": [
        (Fail, "Nullable type as key not supported")
    ],
    "tests/select map with key string/Nullable": [
        (Fail, "Nullable type as key not supported")
    ],
    "tests/select map with key string/Nullable(NULL)": [
        (Fail, "Nullable type as key not supported")
    ],
    "tests/table map queries/select map with nullable value": [
        (Fail, "Nullable value not supported")
    ],
    "tests/table map with key integer/toNullable": [
        (Fail, "Nullable type as key not supported")
    ],
    "tests/table map with key integer/toNullable(NULL)": [
        (Fail, "Nullable type as key not supported")
    ],
    "tests/table map with key string/Nullable": [
        (Fail, "Nullable type as key not supported")
    ],
    "tests/table map with key string/Nullable(NULL)": [
        (Fail, "Nullable type as key not supported")
    ],
    "tests/table map with key string/LowCardinality(String)": [
        (Fail, "LowCardinality(String) as key not supported")
    ],
    "tests/table map with key string/LowCardinality(String) cast from String": [
        (Fail, "LowCardinality(String) as key not supported")
    ],
    "tests/table map with key string/LowCardinality(String) for key and value": [
        (Fail, "LowCardinality(String) as key not supported")
    ],
    "tests/table map with key string/LowCardinality(FixedString)": [
        (Fail, "LowCardinality(FixedString) as key not supported")
    ],
    "tests/table map with value string/LowCardinality(String) for key and value": [
        (Fail, "LowCardinality(String) as key not supported")
    ],
    # JSON related
    "tests/table map with duplicated keys/Map(Int64, String))": [
        (Fail, "new bug due to JSON changes")
    ],
    "tests/table map with key integer/UInt64": [(Fail, "new bug due to JSON changes")],
    "tests/table map with value integer/UInt64": [
        (Fail, "new bug due to JSON changes")
    ],
    "tests/:/:": [(Fail, "debug")],
}

xflags = {}

ffails = {
    "/clickhouse/map type/tests/table map unsupported types/nullable map": (
        XFail,
        "type supported in 21.12",
        (lambda test: check_clickhouse_version(">=21.12")(test)),
    ),
    "/clickhouse/map type/tests/table map unsupported types/map with nothing type for key and value": (
        XFail,
        "type supported in 21.12",
        (lambda test: check_clickhouse_version(">=21.12")(test)),
    ),
    "/clickhouse/map type/tests/table map invalid key/integer when key is string": (
        XFail,
        "type supported in 21.12",
        (lambda test: check_clickhouse_version(">=21.12")(test)),
    ),
}


@TestModule
@ArgumentParser(argparser)
@XFails(xfails)
@XFlags(xflags)
@FFails(ffails)
@Name("map type")
@Specifications(SRS018_ClickHouse_Map_Data_Type)
def regression(self, local, clickhouse_binary_path, clickhouse_version, stress=None):
    """Map type regression."""
    nodes = {"clickhouse": ("clickhouse1", "clickhouse2", "clickhouse3")}

    self.context.clickhouse_version = clickhouse_version

    if stress is not None:
        self.context.stress = stress

    from platform import processor as current_cpu

    folder_name = os.path.basename(current_dir())
    if current_cpu() == "aarch64":
        env = f"{folder_name}_env_arm64"
    else:
        env = f"{folder_name}_env"

    with Cluster(
        local,
        clickhouse_binary_path,
        nodes=nodes,
        docker_compose_project_dir=os.path.join(current_dir(), env),
    ) as cluster:
        self.context.cluster = cluster

        Feature(run=load("map_type.tests.feature", "feature"))


if main():
    regression()
