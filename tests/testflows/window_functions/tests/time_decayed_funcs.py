from testflows.core import *

from window_functions.requirements import *
from window_functions.tests.common import *


@TestOutline
def exponentialTimeDecayedFunc(
    self, funcname, extremely_large_number_of_arguments=1000
):
    """Check exponentialTimeDecayed functions such as
    exponentialTimeDecayedSum, exponentialTimeDecayedMax and exponentialTimeDecayedAvg.
    """
    decay_datatypes = ["toUInt32", "toUInt64", "toFloat32", "toFloat64"]

    value_datatypes = [
        "Int8",
        "Int16",
        "Int32",
        "Int64",
        "UInt8",
        "UInt16",
        "UInt32",
        "UInt64",
        "Float32",
        "Float64",
        "Decimal32(4)",
        "Decimal64(4)",
        "Decimal128(4)",
    ]

    unsupported_value_datatypes = [
        "DateTime",
        "DateTime64",
        "Nullable(Int8)",
        "Nullable(Float32)",
    ]

    time_columns = [
        ("DateTime", "f_timestamp"),
        ("DateTime64", "f_timestamp64"),
        ("DateTimeTZ", "f_timestamptz"),
        ("DateTime64TZ", "f_timestamp64tz"),
    ]

    for time_datatype, time_column in time_columns:
        with Example(f"{time_datatype} time column"):
            execute_query(
                f"SELECT id, {time_column} AS time, {funcname}(1)(id, {time_column}) OVER (ROWS BETWEEN UNBOUNDED PRECEDING AND CURRENT ROW) AS w FROM datetimes2 ORDER BY id LIMIT 10"
            )

    with Example("check decay length with zero"):
        execute_query(
            f"SELECT id, f_timestamp AS time, {funcname}(0)(id, f_timestamp) OVER () AS w FROM datetimes2 ORDER BY id LIMIT 10"
        )

    with Example("check decay length with INTERVAL"):
        for interval_period in interval_periods:
            r = execute_query(
                f"SELECT id, f_timestamp AS time, {funcname}(INTERVAL 5 {interval_period})(id, f_timestamp) OVER () AS w FROM datetimes2 ORDER BY id LIMIT 10",
                no_checks=True,
            )
            assert r.exitcode != 0, error("should return an error")

    with Example("check decay length with negative"):
        execute_query(
            f"SELECT id, f_timestamp AS time, {funcname}(-1000)(id, f_timestamp) OVER () AS w FROM datetimes2 ORDER BY id LIMIT 10"
        )

    with Example("check decay length with invalid number of arguments"):
        execute_query(
            f"SELECT id, f_timestamp AS time, {funcname}(1,2)(id, f_timestamp) OVER () AS w FROM datetimes2 ORDER BY id LIMIT 10",
            exitcode=36,
            message=f"Exception: Function {funcname} takes exactly one parameter",
        )

    with Example("check decay length with extremely large invalid number of arguments"):
        execute_query(
            f"SELECT id, f_timestamp AS time, {funcname}({','.join(['1']*extremely_large_number_of_arguments)})(id, f_timestamp) OVER () AS w FROM datetimes2 ORDER BY id LIMIT 10",
            exitcode=36,
            message=f"Exception: Function {funcname} takes exactly one parameter",
        )

    with Example("check decay length with column argument"):
        execute_query(
            f"SELECT id, f_timestamp AS time, {funcname}(id)(id, f_timestamp) OVER () AS w FROM datetimes2 ORDER BY id LIMIT 10",
            exitcode=47,
            message=f"Exception: ",
        )

    with Example("check input value with 0"):
        execute_query(
            f"SELECT id, f_timestamp AS time, {funcname}(1)(0, f_timestamp) OVER () AS w FROM datetimes2 ORDER BY id LIMIT 10",
        )

    with Example("check input time with 0"):
        execute_query(
            f"SELECT id, f_timestamp AS time, {funcname}(1)(id, 0) OVER () AS w FROM datetimes2 ORDER BY id LIMIT 10",
        )

    with Example("check invalid number of arguments"):
        execute_query(
            f"SELECT id, f_timestamp AS time, {funcname}(1)(id, id, f_timestamp) OVER () AS w FROM datetimes2 ORDER BY id LIMIT 10",
            exitcode=36,
            message=f"DB::Exception: Function {funcname} takes exactly two arguments",
        )

    with Example("check extremely large invalid number of arguments"):
        execute_query(
            f"SELECT id, f_timestamp AS time, {funcname}(1)(id, f_timestamp, {','.join(['id']*extremely_large_number_of_arguments)}) OVER () AS w FROM datetimes2 ORDER BY id LIMIT 10",
            exitcode=36,
            message=f"DB::Exception: Function {funcname} takes exactly two arguments",
        )

    with Example("check decay length overflow"):
        execute_query(
            f"SELECT id, f_timestamp AS time, {funcname}(1000000000000000000000000000000000000000000000000000000000000000)(id, f_timestamp) OVER () AS w FROM datetimes2 ORDER BY id LIMIT 10"
        )

    with Example(f"check using as a non-window aggregate function"):
        execute_query(
            f"SELECT anyLast(id), anyLast(time), {funcname}(10)(id, time) FROM values('id Int8, time DateTime', (1,1),(1,2),(2,3),(3,3),(3,5))",
            exitcode=36,
            message=f"Exception: The function '{funcname}' can only be used as a window function, not as an aggregate function",
        )

    for data_type in [
        "Nullable(DateTime)",
        "Nullable(DateTime64)",
        "Nullable(Int8)",
        "Nullable(Float32)",
    ]:
        with Example(f"check input time with unsupported {data_type} type"):
            execute_query(
                f"SELECT id, time, {funcname}(1)(id, time) OVER () FROM values('id Int8, time Nullable(DateTime)', (1,1),(1,2),(2,3),(3,NULL),(3,5))",
                message="Exception: Argument 1 must be DateTime, DateTime64 or a number",
                exitcode=36,
            )

    for data_type in value_datatypes:
        with Example(f"check input value with {data_type} type"):
            execute_query(
                f"SELECT id, time, {funcname}(2.2)(id, time) OVER () FROM values('id {data_type}, time DateTime', (1,1),(1,2),(2,3),(3,3),(3,5))",
            )

    for data_type in unsupported_value_datatypes:
        with Example(f"check input value with unsupported {data_type} type"):
            execute_query(
                f"SELECT id, time, {funcname}(2.2)(id, time) OVER () FROM values('id {data_type}, time DateTime', (1,1),(1,2),(2,3),(3,4),(3,5))",
                exitcode=36,
                message="Exception: Argument 0 must be a number",
            )

    for decay_datatype in decay_datatypes:
        with Example(f"{decay_datatype} decay length"):
            execute_query(
                f"SELECT id, f_timestamp AS time, {funcname}({decay_datatype}(10.5))(id, f_timestamp) OVER (ROWS BETWEEN UNBOUNDED PRECEDING AND CURRENT ROW) AS w FROM datetimes2 ORDER BY id LIMIT 10",
            )

    with Example("check one row partitions"):
        execute_query(
            f"SELECT id, time, w FROM (SELECT id, f_timestamp AS time, {funcname}(10)(id, f_timestamp) OVER (PARTITION BY id) AS w FROM (SELECT * FROM datetimes2 ORDER BY id LIMIT 10))"
        )

    with Example(f"check -1 time gap"):
        execute_query(
            f"SELECT id, time, {funcname}(1)(id, time) OVER () FROM values('id Int8, time DateTime', (1,'2022-01-01 00:00:00'),(2,'2022-01-01 00:00:00'),(3,'2022-01-01 00:00:00'),(4,'2022-01-01 00:00:01'),(5,'2022-01-01 00:00:00'))",
        )

    with Example(f"check 0 time gap"):
        execute_query(
            f"SELECT id, time, {funcname}(1)(id, time) OVER () FROM values('id Int8, time DateTime', (1,'2022-01-01 00:00:00'),(2,'2022-01-01 00:00:00'),(-3,'2022-01-01 00:00:00'),(-4,'2022-01-01 00:00:00'),(-5,'2022-01-01 00:00:00'))",
        )

    for window in windows(order_by="id"):
        with Check(f"check over({window})"):
            with Example("basic check"):
                execute_query(
                    f"SELECT id, time, w FROM (SELECT id, f_timestamp AS time, {funcname}(10)(id, f_timestamp) OVER ({window}) AS w FROM datetimes2 WHERE id % 3 ORDER BY id LIMIT 10)"
                )
            with Example("check with partition by minute"):
                execute_query(
                    f"SELECT id, time, w FROM (SELECT id, f_timestamp AS time, {funcname}(10)(id, f_timestamp) OVER (PARTITION BY toStartOfMinute(f_timestamp) {window}) AS w FROM datetimes2 WHERE id % 59 ORDER BY id LIMIT 10)"
                )
            with Example("full table"):
                execute_query(
                    f"SELECT anyLast(id), anyLast(time), cityHash64(groupArray(w)) FROM (SELECT id, f_timestamp AS time, {funcname}(1)(id, f_timestamp) OVER ({window}) AS w FROM datetimes2 ORDER BY id)"
                )
            with Example("full table with partition by minute"):
                execute_query(
                    f"SELECT anyLast(id), anyLast(time), cityHash64(groupArray(w)) FROM (SELECT id, f_timestamp AS time, {funcname}(1)(id, f_timestamp) OVER (PARTITION BY toStartOfMinute(f_timestamp) {window}) AS w FROM datetimes2 WHERE id % 59 ORDER BY id)"
                )


@TestScenario
def exponentialTimeDecayedSum(self):
    """Check exponentialTimeDecayedSum."""
    exponentialTimeDecayedFunc(funcname="exponentialTimeDecayedSum")


@TestScenario
def exponentialTimeDecayedMax(self):
    """Check exponentialTimeDecayedMax."""
    exponentialTimeDecayedFunc(funcname="exponentialTimeDecayedMax")


@TestScenario
def exponentialTimeDecayedAvg(self):
    """Check exponentialTimeDecayedAvg."""
    exponentialTimeDecayedFunc(funcname="exponentialTimeDecayedAvg")


@TestScenario
def exponentialTimeDecayedCount(self, extremely_large_number_of_arguments=1000):
    """Check exponentialTimeDecayedCount function."""
    funcname = "exponentialTimeDecayedCount"

    decay_datatypes = ["toUInt32", "toUInt64", "toFloat32", "toFloat64"]

    value_datatypes = [
        "Int8",
        "Int16",
        "Int32",
        "Int64",
        "UInt8",
        "UInt16",
        "UInt32",
        "UInt64",
        "Float32",
        "Float64",
        "Decimal32(4)",
        "Decimal64(4)",
        "Decimal128(4)",
    ]

    unsupported_value_datatypes = [
        "DateTime",
        "DateTime64",
        "Nullable(Int8)",
        "Nullable(Float32)",
    ]

    time_columns = [
        ("DateTime", "f_timestamp"),
        ("DateTime64", "f_timestamp64"),
        ("DateTimeTZ", "f_timestamptz"),
        ("DateTime64TZ", "f_timestamp64tz"),
    ]

    for time_datatype, time_column in time_columns:
        with Example(f"{time_datatype} time column"):
            execute_query(
                f"SELECT id, {time_column} AS time, {funcname}(1)({time_column}) OVER (ROWS BETWEEN UNBOUNDED PRECEDING AND CURRENT ROW) AS w FROM datetimes2 ORDER BY id LIMIT 10"
            )

    with Example("check decay length with zero"):
        execute_query(
            f"SELECT id, f_timestamp AS time, {funcname}(0)(f_timestamp) OVER () AS w FROM datetimes2 ORDER BY id LIMIT 10"
        )

    with Example("check decay length with INTERVAL"):
        for interval_period in interval_periods:
            r = execute_query(
                f"SELECT id, f_timestamp AS time, {funcname}(INTERVAL 5 {interval_period})(f_timestamp) OVER () AS w FROM datetimes2 ORDER BY id LIMIT 10",
                no_checks=True,
            )
            assert r.exitcode != 0, error("should return an error")

    with Example("check decay length with negative"):
        execute_query(
            f"SELECT id, f_timestamp AS time, {funcname}(-1000)(f_timestamp) OVER () AS w FROM datetimes2 ORDER BY id LIMIT 10"
        )

    with Example("check decay length with invalid number of arguments"):
        execute_query(
            f"SELECT id, f_timestamp AS time, {funcname}(1,2)(f_timestamp) OVER () AS w FROM datetimes2 ORDER BY id LIMIT 10",
            exitcode=36,
            message=f"Exception: Function {funcname} takes exactly one parameter",
        )

    with Example("check decay length with extremely large invalid number of arguments"):
        execute_query(
            f"SELECT id, f_timestamp AS time, {funcname}({','.join(['1']*extremely_large_number_of_arguments)})(f_timestamp) OVER () AS w FROM datetimes2 ORDER BY id LIMIT 10",
            exitcode=36,
            message=f"Exception: Function {funcname} takes exactly one parameter",
        )

    with Example("check invalid number of arguments"):
        execute_query(
            f"SELECT id, f_timestamp AS time, {funcname}(1)(id, f_timestamp) OVER () AS w FROM datetimes2 ORDER BY id LIMIT 10",
            exitcode=36,
            message=f"DB::Exception: Function {funcname} takes exactly one argument",
        )

    with Example("check extremely large invalid number of arguments"):
        execute_query(
            f"SELECT id, f_timestamp AS time, {funcname}(1)(f_timestamp, {','.join(['f_timestamp']*extremely_large_number_of_arguments)}) OVER () AS w FROM datetimes2 ORDER BY id LIMIT 10",
            exitcode=36,
            message=f"DB::Exception: Function {funcname} takes exactly one argument",
        )

    with Example("check decay length overflow"):
        execute_query(
            f"SELECT id, f_timestamp AS time, {funcname}(1000000000000000000000000000000000000000000000000000000000000000)(f_timestamp) OVER () AS w FROM datetimes2 ORDER BY id LIMIT 10"
        )

    with Example("check decay length with column argument"):
        execute_query(
            f"SELECT id, f_timestamp AS time, {funcname}(id)(f_timestamp) OVER () AS w FROM datetimes2 ORDER BY id LIMIT 10",
            exitcode=47,
            message=f"Exception: ",
        )

    with Example("check input time with 0"):
        execute_query(
            f"SELECT id, f_timestamp AS time, {funcname}(1)(0) OVER () AS w FROM datetimes2 ORDER BY id LIMIT 10",
        )

    with Example(f"check using as a non-window aggregate function"):
        execute_query(
            f"SELECT anyLast(id), anyLast(time), {funcname}(10)(time) FROM values('id Int8, time DateTime', (1,1),(1,2),(2,3),(3,3),(3,5))",
            exitcode=36,
            message=f"Exception: The function '{funcname}' can only be used as a window function, not as an aggregate function",
        )

    for data_type in [
        "Nullable(DateTime)",
        "Nullable(DateTime64)",
        "Nullable(Int8)",
        "Nullable(Float32)",
    ]:
        with Example(f"check input time with unsupported {data_type} type"):
            execute_query(
                f"SELECT id, time, {funcname}(1)(time) OVER () FROM values('id Int8, time Nullable(DateTime)', (1,1),(2,2),(2,3),(3,NULL),(3,5))",
                message="Exception: Argument 0 must be DateTime, DateTime64 or a number",
                exitcode=36,
            )

    for decay_datatype in decay_datatypes:
        with Example(f"{decay_datatype} decay length"):
            execute_query(
                f"SELECT id, f_timestamp AS time, {funcname}({decay_datatype}(10.5))(f_timestamp) OVER (ROWS BETWEEN UNBOUNDED PRECEDING AND CURRENT ROW) AS w FROM datetimes2 ORDER BY id LIMIT 10",
            )

    with Example("check one row partitions"):
        execute_query(
            f"SELECT id, time, w FROM (SELECT id, f_timestamp AS time, {funcname}(10)(f_timestamp) OVER (PARTITION BY id) AS w FROM (SELECT * FROM datetimes2 ORDER BY id LIMIT 10))"
        )

    with Example(f"check -1 time gap"):
        execute_query(
            f"SELECT id, time, {funcname}(1)(time) OVER () FROM values('id Int8, time DateTime', (1,'2022-01-01 00:00:00'),(2,'2022-01-01 00:00:00'),(3,'2022-01-01 00:00:00'),(4,'2022-01-01 00:00:01'),(5,'2022-01-01 00:00:00'))",
        )

    with Example(f"check 0 time gap"):
        execute_query(
            f"SELECT id, time, {funcname}(1)(time) OVER () FROM values('id Int8, time DateTime', (1,'2022-01-01 00:00:00'),(2,'2022-01-01 00:00:00'),(-3,'2022-01-01 00:00:00'),(-4,'2022-01-01 00:00:00'),(-5,'2022-01-01 00:00:00'))",
        )

    for window in windows(order_by="id"):
        with Check(f"check over({window})"):
            with Example("basic check"):
                execute_query(
                    f"SELECT id, time, w FROM (SELECT id, f_timestamp AS time, {funcname}(10)(f_timestamp) OVER ({window}) AS w FROM datetimes2 WHERE id % 3 ORDER BY id LIMIT 10)"
                )
            with Example("check with partition by minute"):
                execute_query(
                    f"SELECT id, time, w FROM (SELECT id, f_timestamp AS time, {funcname}(10)(f_timestamp) OVER (PARTITION BY toStartOfMinute(f_timestamp) {window}) AS w FROM datetimes2 WHERE id % 59 ORDER BY id LIMIT 10)"
                )
            with Example("full table"):
                execute_query(
                    f"SELECT anyLast(id), anyLast(time), cityHash64(groupArray(w)) FROM (SELECT id, f_timestamp AS time, {funcname}(1)(f_timestamp) OVER ({window}) AS w FROM datetimes2 ORDER BY id)"
                )
            with Example("full table with partition by minute"):
                execute_query(
                    f"SELECT anyLast(id), anyLast(time), cityHash64(groupArray(w)) FROM (SELECT id, f_timestamp AS time, {funcname}(1)(f_timestamp) OVER (PARTITION BY toStartOfMinute(f_timestamp) {window}) AS w FROM datetimes2 WHERE id % 59 ORDER BY id)"
                )


@TestFeature
@Name("time decayed funcs")
@Requirements(RQ_SRS_019_ClickHouse_WindowFunctions_ExponentialTimeDecayed("1.0"))
@Skipped("only for >=22.3", check_clickhouse_version("<22.3.10"))
def feature(self):
    """Check time decayed window functions."""
    for scenario in loads(current_module(), Scenario):
        Scenario(run=scenario, flags=TE)
