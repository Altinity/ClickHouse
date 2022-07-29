#!/usr/bin/env python3
import sys
from testflows.core import *

append_path(sys.path, ".")

from helpers.argparser import argparser


@TestModule
@Name("clickhouse")
@ArgumentParser(argparser)
def regression(self, local, clickhouse_binary_path, clickhouse_version, stress=None):
    """ClickHouse regression."""
    args = {
        "local": local,
        "clickhouse_binary_path": clickhouse_binary_path,
        "clickhouse_version": clickhouse_version,
        "stress": stress,
    }

    self.context.stress = stress

    try:
        Feature(test=load("aes_encryption.regression", "regression"), parallel=True)(
            **args
        )

        Feature(
            test=load("datetime64_extended_range.regression", "regression"),
            parallel=True,
        )(**args)

        Feature(test=load("example.regression", "regression"), parallel=True)(**args)

        Feature(
            test=load("extended_precision_data_types.regression", "regression"),
            parallel=True,
        )(**args)

        Feature(
            test=load("ldap.authentication.regression", "regression"), parallel=True
        )(**args)

        join()

        Feature(
            test=load("ldap.external_user_directory.regression", "regression"),
            parallel=True,
        )(**args)

        join()

        Feature(test=load("ldap.role_mapping.regression", "regression"), parallel=True)(
            **args
        )

        Feature(test=load("map_type.regression", "regression"), parallel=True)(**args)

        Feature(test=load("rbac.regression", "regression"), parallel=True)(**args)

        Feature(test=load("window_functions.regression", "regression"), parallel=True)(
            **args
        )
    finally:
        join()


if main():
    regression()
