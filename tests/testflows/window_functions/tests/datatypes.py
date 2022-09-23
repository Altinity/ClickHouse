from testflows.core import *
from testflows.asserts import values, error, snapshot

from window_functions.requirements import *
from window_functions.tests.common import *


@TestScenario
def low_cardinality(self):
    """Check using LowCardinality data type."""
    expected = convert_output(
        """
     ma
    -----
      2
      2
      2
      2
      2
      2
      2
      2
      2
      2
    """
    )

    # note((lambda test: check_clickhouse_version("=21.10.5.3")(test))(self))
    execute_query(
        "SELECT max(a) OVER () AS ma FROM (SELECT toLowCardinality(toString(number % 2 + 1)) AS a FROM numbers(10))",
        expected=expected,
    )


@TestScenario
def datetime64(self):
    """Check using DateTime64 data type."""
    expected = convert_output(
        """
     ma
    ---------------------------
     1972-11-07 19:00:00.00000
     1972-11-07 19:00:00.00000
     1972-11-07 19:00:00.00000
     1972-11-07 19:00:00.00000
     1972-11-07 19:00:00.00000
     1972-11-07 19:00:00.00000
     1972-11-07 19:00:00.00000
     1972-11-07 19:00:00.00000
     1972-11-07 19:00:00.00000
     1972-11-07 19:00:00.00000
     """
    )

    execute_query(
        "SELECT max(a) OVER () AS ma FROM ( SELECT toDateTime64(number*10000000,5) AS a FROM numbers(10))",
        expected=expected,
    )


@TestScenario
def datetime(self):
    """Check using DateTime data type."""
    expected = convert_output(
        """
     ma
    ---------------------
     1972-11-07 19:00:00
     1972-11-07 19:00:00
     1972-11-07 19:00:00
     1972-11-07 19:00:00
     1972-11-07 19:00:00
     1972-11-07 19:00:00
     1972-11-07 19:00:00
     1972-11-07 19:00:00
     1972-11-07 19:00:00
     1972-11-07 19:00:00
     """
    )

    execute_query(
        "SELECT max(a) OVER () AS ma FROM ( SELECT toDateTime(number*10000000) AS a FROM numbers(10))",
        expected=expected,
    )


@TestScenario
def date(self):
    """Check using Date data type."""
    expected = convert_output(
        """
     ma
    ------------
     1972-11-07
     1972-11-07
     1972-11-07
     1972-11-07
     1972-11-07
     1972-11-07
     1972-11-07
     1972-11-07
     1972-11-07
     1972-11-07
     """
    )

    execute_query(
        "SELECT max(a) OVER () AS ma FROM ( SELECT toDate(number*10000000) AS a FROM numbers(10))",
        expected=expected,
    )


@TestScenario
def enum(self):
    """Check using Enum data type."""
    expected = convert_output(
        """
     ma
    ----
     2
     2
     2
     2
     2
     2
     2
     2
     2
     2
    """
    )

    execute_query(
        "SELECT max(a) OVER () AS ma FROM ( SELECT CAST(a, 'Enum(\\'1\\' = 1, \\'2\\' = 2)') AS a FROM (SELECT toString(number % 2 + 1) AS a FROM numbers(10)))",
        expected=expected,
    )


@TestScenario
def decimal32(self):
    """Check using Decimal32 data type."""

    if check_clickhouse_version("<21.9")(self):
        expected = convert_output(
            """
        ma
        ------
        1.80
        1.80
        1.80
        1.80
        1.80
        1.80
        1.80
        1.80
        1.80
        1.80
        """
        )
    else:
        expected = convert_output(
            """
        ma
        ------
        1.8
        1.8
        1.8
        1.8
        1.8
        1.8
        1.8
        1.8
        1.8
        1.8
        """
        )

    execute_query(
        "SELECT max(a) OVER () AS ma FROM ( SELECT toDecimal32(number*0.2,2) AS a FROM numbers(10))",
        expected=expected,
    )


@TestScenario
def string(self):
    """Check using String data type."""
    expected = convert_output(
        """
     ma
    ------
     2
     2
     2
     2
     2
     2
     2
     2
     2
     2
    """
    )

    execute_query(
        "SELECT max(a) OVER () AS ma FROM ( SELECT toString(number % 2 + 1) AS a FROM numbers(10))",
        expected=expected,
    )


@TestScenario
def fixed_string(self):
    """Check using FixedString data type."""
    expected = convert_output(
        """
     ma
    ------
     2\\0\\0
     2\\0\\0
     2\\0\\0
     2\\0\\0
     2\\0\\0
     2\\0\\0
     2\\0\\0
     2\\0\\0
     2\\0\\0
     2\\0\\0
    """
    )

    execute_query(
        "SELECT max(a) OVER () AS ma FROM ( SELECT toFixedString(toString(number % 2 + 1), 3) AS a FROM numbers(10))",
        expected=expected,
    )


@TestScenario
def uuid(self):
    """Check using UUID data type."""
    expected = convert_output(
        """
     ma
    ------
     61f0c404-5cb3-11e7-907b-a6006ad3dba0
     61f0c404-5cb3-11e7-907b-a6006ad3dba0
     61f0c404-5cb3-11e7-907b-a6006ad3dba0
     61f0c404-5cb3-11e7-907b-a6006ad3dba0
     61f0c404-5cb3-11e7-907b-a6006ad3dba0
     61f0c404-5cb3-11e7-907b-a6006ad3dba0
     61f0c404-5cb3-11e7-907b-a6006ad3dba0
     61f0c404-5cb3-11e7-907b-a6006ad3dba0
     61f0c404-5cb3-11e7-907b-a6006ad3dba0
     61f0c404-5cb3-11e7-907b-a6006ad3dba0
    """
    )

    execute_query(
        "SELECT max(a) OVER () AS ma FROM ( SELECT toUUID('61f0c404-5cb3-11e7-907b-a6006ad3dba0') AS a FROM numbers(10))",
        expected=expected,
    )


@TestScenario
def ipv4(self):
    """Check using IPv4 data type."""
    expected = convert_output(
        """
     ma
    ------
     171.225.130.45
     171.225.130.45
     171.225.130.45
     171.225.130.45
     171.225.130.45
     171.225.130.45
     171.225.130.45
     171.225.130.45
     171.225.130.45
     171.225.130.45
    """
    )

    execute_query(
        "SELECT max(a) OVER () AS ma FROM ( SELECT toIPv4('171.225.130.45') AS a FROM numbers(10))",
        expected=expected,
    )


@TestScenario
def ipv6(self):
    """Check using IPv6 data type."""
    expected = convert_output(
        """
     ma
    ------
     2001:438:ffff::407d:1bc1
     2001:438:ffff::407d:1bc1
     2001:438:ffff::407d:1bc1
     2001:438:ffff::407d:1bc1
     2001:438:ffff::407d:1bc1
     2001:438:ffff::407d:1bc1
     2001:438:ffff::407d:1bc1
     2001:438:ffff::407d:1bc1
     2001:438:ffff::407d:1bc1
     2001:438:ffff::407d:1bc1
    """
    )

    execute_query(
        "SELECT max(a) OVER () AS ma FROM ( SELECT toIPv6('2001:438:ffff::407d:1bc1') AS a FROM numbers(10))",
        expected=expected,
    )


@TestScenario
def map(self):
    """Check using Map data type."""
    expected = convert_output(
        """
     ma
    ------
     {'hello2':2}
     {'hello2':2}
    """
    )

    execute_query(
        "SELECT max(a) OVER () AS ma FROM (SELECT m AS a FROM values('m Map(String, UInt64)', map('hello',1), map('hello2',2)))",
        expected=expected,
    )


@TestFeature
@Name("datatypes")
@Requirements()
def feature(self):
    """Check using aggregate function over windows with different data types."""

    for scenario in loads(current_module(), Scenario):
        Scenario(run=scenario, flags=TE)
