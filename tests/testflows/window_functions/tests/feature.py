from testflows.core import *

from window_functions.tests.common import *
from window_functions.requirements import *


@TestOutline(Feature)
@Name("tests")
@Examples(
    "distributed",
    [
        (
            False,
            Name("non distributed"),
            Requirements(
                RQ_SRS_019_ClickHouse_WindowFunctions_NonDistributedTables("1.0")
            ),
        ),
        (
            True,
            Name("distributed"),
            Requirements(
                RQ_SRS_019_ClickHouse_WindowFunctions_DistributedTables("1.0")
            ),
        ),
    ],
)
def feature(self, distributed, node="clickhouse1"):
    """Check window functions behavior using non-distributed or
    distributed tables.
    """
    self.context.distributed = distributed
    self.context.node = self.context.cluster.node(node)

    if check_clickhouse_version("<21.9")(self):
        with Given("I allow experimental window functions"):
            allow_experimental_window_functions()

    with Given("employee salary table"):
        empsalary_table(distributed=distributed)

    with And("tenk1 table"):
        tenk1_table(distributed=distributed)

    with And("numerics table"):
        numerics_table(distributed=distributed)

    with And("datetimes table"):
        datetimes_table(distributed=distributed)

    with And("datetimes2 table populated using data query"):
        datetimes_table_from_data_query(
            data_query=(
                "SELECT number AS id, "
                "toDate('2020-01-01') + number AS f_date, "
                # "toDate32('2020-01-01') + number AS f_date32, "
                "toDateTime('2020-01-01', 'CET') + number AS f_timestamptz, "
                "toDateTime('2020-01-01') + number AS f_timestamp, "
                "toDateTime64('2020-01-01', 9, 'CET') + number AS f_timestamp64tz, "
                "toDateTime64('2020-01-01', 9) + number AS f_timestamp64 "
                "FROM numbers(10000)"
            ),
            name="datetimes2",
        )

    with And("t1 table"):
        t1_table(distributed=distributed)

    Feature(run=load("window_functions.tests.window_spec", "feature"), flags=TE)
    Feature(run=load("window_functions.tests.partition_clause", "feature"), flags=TE)
    Feature(run=load("window_functions.tests.order_clause", "feature"), flags=TE)
    Feature(run=load("window_functions.tests.frame_clause", "feature"), flags=TE)
    Feature(run=load("window_functions.tests.window_clause", "feature"), flags=TE)
    Feature(run=load("window_functions.tests.over_clause", "feature"), flags=TE)
    Feature(run=load("window_functions.tests.funcs", "feature"), flags=TE)
    Feature(run=load("window_functions.tests.aggregate_funcs", "feature"), flags=TE)
    Feature(run=load("window_functions.tests.time_decayed_funcs", "feature"), flags=TE)
    Feature(run=load("window_functions.tests.errors", "feature"), flags=TE)
    Feature(run=load("window_functions.tests.misc", "feature"), flags=TE)
    Feature(run=load("window_functions.tests.datatypes", "feature"), flags=TE)
    Feature(
        run=load("window_functions.tests.non_negative_derivative", "feature"), flags=TE
    )
