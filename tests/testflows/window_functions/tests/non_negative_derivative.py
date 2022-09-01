import textwrap

from testflows.core import *

from helpers.common import check_clickhouse_version
from window_functions.requirements import *
from window_functions.tests.common import *


@TestScenario
def null_values(self):
    """Check NULL values in either metric or timestamp column."""
    with Example(f"metric column"):
        execute_query(
            f"SELECT id, metric, ts, nonNegativeDerivative(metric, ts) OVER () AS nnd FROM values('id Int8, metric Nullable(Float64), ts DateTime', (1,1,'2022-12-12 00:00:00'),(1,null,'2022-12-12 00:00:01'))",
            exitcode=36,
            message="Exception: Argument 0 must be a number",
        )

    with Example("timestamp column"):
        execute_query(
            f"SELECT id, metric, ts, nonNegativeDerivative(metric, ts) OVER () AS nnd FROM values('id Int8, metric Float64, ts Nullable(DateTime)', (1,1,'2022-12-12 00:00:00'),(1,1,null))",
            exitcode=36,
            message="Exception: Argument 1 must be DateTime or DateTime64",
        )


@TestScenario
def division_by_zero(self):
    """Check division by zero when two timestamps are the same."""
    execute_query(
        f"SELECT id, metric, ts, nonNegativeDerivative(metric, ts) OVER (ORDER BY id) AS nnd FROM values('id Int8, metric Float64, ts DateTime', (1,1,'2022-12-12 00:00:00'),(2,1,'2022-12-12 00:00:00'))",
    )


@TestScenario
def division_by_tiny(self):
    """Check division by tiny difference between consecutive timestamps."""
    with Example("numerator is non-zero"):
        execute_query(
            f"SELECT id, metric, ts, nonNegativeDerivative(metric, ts) OVER (ORDER BY id) AS nnd FROM values('id Int8, metric Float64, ts DateTime64(9)', (1,1,'2022-12-12 00:00:00'), (2,2,'2022-12-12 00:00:00.000000001'),(3,3,'2022-12-12 00:00:00.000000002'))",
        )

    with Example("numerator is zero"):
        execute_query(
            f"SELECT id, metric, ts, nonNegativeDerivative(metric, ts) OVER (ORDER BY id) AS nnd FROM values('id Int8, metric Float64, ts DateTime64(9)', (1,1,'2022-12-12 00:00:00'), (2,1,'2022-12-12 00:00:00.000000001'),(3,1,'2022-12-12 00:00:00.000000002'))",
        )


@TestScenario
def check_negative_result(self):
    """Check that negative result is converted to 0."""
    expected = convert_output(
        """
        id | metric | ts                            | nnd
        ---+--------+-------------------------------+-----------------------
        1  | 1      | 2022-12-12 00:00:02.000000000 | 0
        2  | 2      | 2022-12-12 00:00:01.000000000 | 0
        3  | 3      | 2022-12-12 00:00:00.000000000 | 0
        """
    )

    execute_query(
        f"SELECT id, metric, ts, nonNegativeDerivative(metric, ts) OVER (ORDER BY id ASC) AS nnd FROM values('id Int8, metric Float64, ts DateTime64(9)', (1,1,'2022-12-12 00:00:02'), (2,2,'2022-12-12 00:00:01'),(3,3,'2022-12-12 00:00:00'))",
        expected=expected,
    )


@TestScenario
def wrong_number_of_arguments_or_types(self):
    """Check passing wrong number of arguments or types."""
    exitcode = 36
    message = "Exception: Function nonNegativeDerivative takes 2 or 3 arguments"
    table = "values('id Int8, metric Float64, ts DateTime64(9)', (1,1,'2022-12-12 00:00:00'), (2,2,'2022-12-12 00:00:01'),(3,3,'2022-12-12 00:00:02'))"

    with Example("missing timestamp"):
        execute_query(
            f"SELECT id, metric, ts, nonNegativeDerivative(metric) OVER (ORDER BY id ASC) AS nnd FROM {table}",
            exitcode=exitcode,
            message=message,
        )

    with Example("missing metric"):
        execute_query(
            f"SELECT id, metric, ts, nonNegativeDerivative(ts) OVER (ORDER BY id ASC) AS nnd FROM {table}",
            exitcode=exitcode,
            message=message,
        )

    with Example("missing metric with interval"):
        execute_query(
            f"SELECT id, metric, ts, nonNegativeDerivative(INTERVAL 3 SECOND) OVER (ORDER BY id ASC) AS nnd FROM {table}",
            exitcode=exitcode,
            message=message,
        )

    with Example("missing timestamp with interval"):
        execute_query(
            f"SELECT id, metric, ts, nonNegativeDerivative(metric, INTERVAL 3 SECOND) OVER (ORDER BY id ASC) AS nnd FROM {table}",
            exitcode=exitcode,
            message="Exception: Argument 1 must be DateTime or DateTime64",
        )

    with Example("empty"):
        execute_query(
            f"SELECT id, metric, ts, nonNegativeDerivative() OVER (ORDER BY id ASC) AS nnd FROM {table}",
            exitcode=exitcode,
            message=message,
        )

    with Example("invalid interval type"):
        execute_query(
            f"SELECT id, metric, ts, nonNegativeDerivative(metric, ts, 2) OVER (ORDER BY id ASC) AS nnd FROM {table}",
            exitcode=exitcode,
            message="Exception: Argument 2 must be an INTERVAL",
        )

    with Example("invalid metric type"):
        execute_query(
            f"SELECT id, metric, ts, nonNegativeDerivative(ts, ts) OVER (ORDER BY id ASC) AS nnd FROM {table}",
            exitcode=exitcode,
            message="Exception: Argument 0 must be a number",
        )

    with Example("invalid timestamp type"):
        execute_query(
            f"SELECT id, metric, ts, nonNegativeDerivative(metric, metric) OVER (ORDER BY id ASC) AS nnd FROM {table}",
            exitcode=exitcode,
            message="Exception: Argument 1 must be DateTime or DateTime64",
        )


@TestScenario
def zero_interval(self):
    """Check using zero second interval."""
    expected = convert_output(
        """
        id | metric | ts                  | nnd
        ---+--------+---------------------+-----
        1  | 1      | 2022-12-12 00:00:00 | 0
        2  | 2      | 2022-12-12 00:00:01 | 0
        3  | 3      | 2022-12-12 00:00:02 | 0
        """
    )

    execute_query(
        "SELECT id, metric, ts, nonNegativeDerivative(metric, ts, INTERVAL 0 SECOND) OVER (ORDER BY id ASC) AS nnd FROM values('id Int8, metric Float64, ts DateTime64(0)', (1,1,'2022-12-12 00:00:00'), (2,2,'2022-12-12 00:00:01'),(3,3,'2022-12-12 00:00:02'))",
        expected=expected,
    )


@TestScenario
def empty_table(self):
    """Check against empty table."""
    node = self.context.node
    table = f"table_{getuid()}"

    with Given("I create empty table"):
        node.query(
            f"CREATE TABLE {table} (id Int8, metric Float64, ts DateTime64(9)) ENGINE=MergeTree() ORDER BY tuple()"
        )

    try:
        execute_query(
            f"SELECT id, metric, ts, nonNegativeDerivative(metric, ts) OVER (ORDER BY id ASC) AS nnd FROM {table}"
        )
    finally:
        with Finally("drop table"):
            node.query(f"DROP TABLE IF EXISTS {table}")


@TestScenario
def check_intervals(self):
    with Check("valid"):
        valid = ("SECOND", "MINUTE", "HOUR", "DAY", "WEEK")

        for interval in valid:
            with Example(f"{interval}", flags=TE):
                execute_query(
                    f"SELECT id, f_timestamp, nonNegativeDerivative(id, f_timestamp, INTERVAL 1 {interval}) OVER (ROWS BETWEEN CURRENT ROW AND CURRENT ROW) AS nnd FROM (SELECT * FROM datetimes2 ORDER BY f_timestamp LIMIT 10)"
                )

    with Check("invalid"):
        invalid = (
            "MONTH",
            "QUARTER",
            "YEAR",
        )

        for interval in invalid:
            with Example(f"{interval}", flags=TE):
                execute_query(
                    f"SELECT id, f_timestamp, nonNegativeDerivative(id, f_timestamp, INTERVAL 1 {interval}) OVER (ROWS BETWEEN CURRENT ROW AND CURRENT ROW) AS nnd FROM (SELECT * FROM datetimes2 ORDER BY f_timestamp LIMIT 10)",
                    exitcode=43,
                    message="Exception: The INTERVAL must be a week or shorter",
                )


@TestScenario
def check_one_row_per_partition(self):
    """Check when there is only 1 row per partition."""
    execute_query(
        f"SELECT id, f_timestamp, nonNegativeDerivative(id, f_timestamp) OVER (PARTITION BY id) AS nnd FROM datetimes2 ORDER BY id LIMIT 10"
    )


@TestScenario
def check_over_windows(self):
    """Check function over different window frames."""
    for window in windows(order_by="id"):
        with Check(f"{window}", flags=TE):
            execute_query(
                f"SELECT id, f_timestamp, nonNegativeDerivative(id, f_timestamp, INTERVAL 1 SECOND) OVER (PARTITION BY toStartOfHour(f_timestamp) {window}) AS nnd FROM datetimes2 ORDER BY id LIMIT 10"
            )


@TestScenario
def valid_metric_types(self):
    """Check valid numerical metric types."""
    datatypes = (
        "Float32",
        "Float64",
        "UInt8",
        "UInt16",
        "UInt32",
        "UInt64",
        "Int8",
        "Int16",
        "Int32",
        "Int64",
        "Decimal32(4)",
        "Decimal64(4)",
        "Decimal128(4)",
    )

    for datatype in datatypes:
        with Example(f"{datatype}"):
            execute_query(
                f"SELECT id, metric, ts, nonNegativeDerivative(metric, ts) OVER (ORDER BY id ASC) AS nnd FROM values('id Int8, metric {datatype}, ts DateTime64(0)', (1,1,'2022-12-12 00:00:00'), (2,2,'2022-12-12 00:00:01'),(3,3,'2022-12-12 00:00:02'))"
            )


@TestScenario
def valid_timestamp_types(self):
    """Check valid timestamp types."""
    datatypes = ("DateTime", "DateTime64(0)", "DateTime64(3, EST)")

    for datatype in datatypes:
        with Example(f"{datatype}"):
            execute_query(
                f"SELECT id, metric, ts, nonNegativeDerivative(metric, ts) OVER (ORDER BY id ASC) AS nnd FROM values('id Int8, metric UInt8, ts {datatype}', (1,1,'2022-12-12 00:00:00'), (2,2,'2022-12-12 00:00:01'),(3,3,'2022-12-12 00:00:02'))"
            )


@TestScenario
def acceptance(self):
    """Check function returns the expected result as defined by the original feature request."""
    data = """
        SELECT * FROM values('timestamp DateTime(3), metric_name String, metric_value UInt64',
            ('2020-01-01 00:00:01.000','cpu',10),
            ('2020-01-01 00:00:01.001','cpu',20),
            ('2020-01-01 00:00:01.003','cpu',25),
            ('2020-01-01 00:00:01.004','cpu',25),
            ('2020-01-01 00:00:01.005','cpu',20),
            ('2020-01-01 00:00:01.005','cpu',21),
            ('2020-01-01 00:00:01.015','cpu',30),
            ('2020-01-01 00:00:02.015','cpu',40),
            ('2020-01-01 00:00:05.015','cpu',40),
            ('2020-01-01 00:00:05.016','cpu',40),
            ('2020-01-01 00:05:05.016','cpu',45)
        )""".strip()

    with Check("workaround query"):
        expected = convert_output(
            """
            timestamp               | metric_name | metric_value | rateNonNegative
            -------------------------+-------------+--------------+----------------------
            2020-01-01 00:00:01.000 | cpu         |           10 |  6.337834459352404e-9
            2020-01-01 00:00:01.001 | cpu         |           20 |                 10000
            2020-01-01 00:00:01.003 | cpu         |           25 |                  2500
            2020-01-01 00:00:01.004 | cpu         |           25 |                     0
            2020-01-01 00:00:01.005 | cpu         |           20 |                     0
            2020-01-01 00:00:01.005 | cpu         |           21 |                   inf
            2020-01-01 00:00:01.015 | cpu         |           30 |                   900
            2020-01-01 00:00:02.015 | cpu         |           40 |                    10
            2020-01-01 00:00:05.015 | cpu         |           40 |                     0
            2020-01-01 00:00:05.016 | cpu         |           40 |                     0
            2020-01-01 00:05:05.016 | cpu         |           45 |  0.016666666666666666
            """
        )

        query = f"""
            WITH
                metric_value - prev_metric_value as delta_metric,
                (toUnixTimestamp64Milli(timestamp)  - toUnixTimestamp64Milli(prev_timestamp))/1000 as delta_ts
            SELECT 
                timestamp,
                metric_name,
                metric_value,
                if( delta_metric / delta_ts > 0, delta_metric / delta_ts, 0)  as rateNonNegative
            FROM 
            (
            SELECT
                timestamp,
                metric_name,
                metric_value,
                lagInFrame(metric_value) OVER (PARTITION BY metric_name ORDER BY timestamp ASC ROWS
                            BETWEEN UNBOUNDED PRECEDING AND UNBOUNDED FOLLOWING) AS prev_metric_value,
                lagInFrame(timestamp) OVER (PARTITION BY metric_name ORDER BY timestamp ASC ROWS
                            BETWEEN UNBOUNDED PRECEDING AND UNBOUNDED FOLLOWING) AS prev_timestamp
            FROM (
            {textwrap.indent(data, " "*8)}
            ) t
            )"""

        execute_query(
            query,
            expected=expected,
        )

    with Check("using nonNegativeDerivative"):
        expected = convert_output(
            """
            timestamp               | metric_name | metric_value | rateNonNegative
            -------------------------+-------------+--------------+----------------------
            2020-01-01 00:00:01.000 | cpu         |           10 |                     0
            2020-01-01 00:00:01.001 | cpu         |           20 |                 10000
            2020-01-01 00:00:01.003 | cpu         |           25 |                  2500
            2020-01-01 00:00:01.004 | cpu         |           25 |                     0
            2020-01-01 00:00:01.005 | cpu         |           20 |                     0
            2020-01-01 00:00:01.005 | cpu         |           21 |                     0
            2020-01-01 00:00:01.015 | cpu         |           30 |                   900
            2020-01-01 00:00:02.015 | cpu         |           40 |                    10
            2020-01-01 00:00:05.015 | cpu         |           40 |                     0
            2020-01-01 00:00:05.016 | cpu         |           40 |                     0
            2020-01-01 00:05:05.016 | cpu         |           45 |  0.016666666666666666
            """
        )

        execute_query(
            "SELECT timestamp, metric_name, metric_value, nonNegativeDerivative("
            "metric_value, timestamp, INTERVAL 1 SECOND) OVER "
            "(PARTITION BY metric_name ORDER BY timestamp ASC ROWS BETWEEN UNBOUNDED PRECEDING AND UNBOUNDED FOLLOWING) AS rateNonNegative "
            f"FROM ({data})",
            expected=expected,
        )


@TestFeature
@Name("non-negative derivative func")
@Requirements(
    RQ_SRS_019_ClickHouse_WindowFunctions_Specific_NonNegativeDerivative("1.0")
)
def feature(self):
    """Check non-negative derivative window function."""
    for scenario in loads(current_module(), Scenario):
        Scenario(run=scenario, flags=TE)
