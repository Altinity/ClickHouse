import os
import time
import pytz
import decimal
import pyarrow
from dateutil.tz import tzlocal
from datetime import datetime, timedelta
import dateutil.relativedelta as rd
from testflows.core import *

from datetime64_extended_range.requirements.requirements import *
from datetime64_extended_range.common import *
from datetime64_extended_range.tests.common import *


@TestScenario
@Requirements(RQ_SRS_010_DateTime64_ExtendedRange_ArrowFormat("1.0"))
def arrow_format(self):
    """Check that DateTime64 is properly represented in Arrow format."""
    node = self.context.node
    stress = self.context.stress
    timezones = timezones_range(stress=stress)

    try:
        for year in years_range(stress):
            with Given("I select datetimes in a year"):
                datetimes = select_dates_in_year(year=year, stress=stress)

            for dt in datetimes:
                for tz in timezones:
                    for precision in (0, 3, 6):
                        with Step(f"{dt} {tz}"):
                            with Given("I drop remaining tables"):
                                exec_query(request="DROP TABLE IF EXISTS t_src SYNC")
                                exec_query(request="DROP TABLE IF EXISTS t_dst SYNC")

                            dt_str = f'{dt.strftime("%Y-%m-%d %H:%M:%S")}.123456789'[
                                : (precision - 9)
                            ]
                            if precision == 0:
                                dt_str = dt.strftime("%Y-%m-%d %H:%M:%S")

                            with Check("reading and writing one-line arrow file"):
                                with When("I create .arrow file"):
                                    query = f"SELECT toDateTime64('{dt_str}', {precision}, '{tz}') FORMAT Arrow"
                                    node.cmd(
                                        cmd=f'clickhouse client --query="{query}" > /var/lib/ch-files/a.arrow'
                                    )

                                with When("I check recorded values"):
                                    data_dict = (
                                        pyarrow.ipc.open_file(
                                            os.path.join(
                                                os.path.dirname(
                                                    os.path.dirname(__file__)
                                                ),
                                                "_instances/clickhouse1/files/a.arrow",
                                            )
                                        )
                                        .read_all()
                                        .to_pydict()
                                    )

                                    assert dt_str in list(data_dict.values())[0][
                                        0
                                    ].strftime("%Y-%m-%d %H:%M:%S.%f"), error()

                            with Check("DT64 -> DT conversion on insert works"):
                                with Given(
                                    "I create tables to generate and paste values"
                                ):
                                    exec_query(
                                        request=f"CREATE TABLE t_src "
                                        f"(id Int8, d DateTime64({precision}, '{tz}')) "
                                        f"ENGINE=MergeTree() ORDER BY id"
                                    )
                                    exec_query(
                                        request=f"CREATE TABLE t_dst "
                                        f"(id Int8, d DateTime('{tz}')) "
                                        f"ENGINE=MergeTree() ORDER BY id"
                                    )

                                with When(
                                    "I insert value into source table and dump it"
                                ):
                                    exec_query(
                                        request=f"INSERT INTO t_src VALUES (1, toDateTime64('{dt_str}', {precision}, '{tz}'))"
                                    )
                                    node.cmd(
                                        cmd=f'clickhouse client --query="SELECT * FROM t_src FORMAT Arrow" > /var/lib/ch-files/a.arrow'
                                    )

                                with Then(
                                    "I read file into table with implicit DT conversion"
                                ):
                                    node.cmd(
                                        cmd=f"cat /var/lib/ch-files/a.arrow | "
                                        f'clickhouse client --query="INSERT INTO t_dst FORMAT Arrow"',
                                        exitcode=0,
                                    )

                                if in_normal_range(dt):
                                    with And("I check pasted DateTime is correct"):
                                        assert (
                                            dt.strftime("%Y-%m-%d %H:%M:%S")
                                            in node.query(f"SELECT * FROM t_dst").output
                                        ), error()

    finally:
        exec_query(request="DROP TABLE IF EXISTS t SYNC")
        node.cmd("rm /var/lib/ch-files/a.arrow")


@TestFeature
def format_conversion(self, node="clickhouse1"):
    """Check that DateTime64 is properly represented in different formats."""
    self.context.node = self.context.cluster.node(node)

    for scenario in loads(current_module(), Scenario):
        Scenario(run=scenario, flags=TE)
