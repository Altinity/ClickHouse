from testflows.core import *
from testflows.asserts import error

from rbac.helper.common import *
import rbac.helper.errors as errors
from rbac.requirements import (
    RQ_SRS_006_RBAC_Select_TableFunctions_Cluster,
    RQ_SRS_006_RBAC_Select_TableFunctions_Remote,
)


@TestScenario
@Requirements(RQ_SRS_006_RBAC_Select_TableFunctions_Remote("1.0"))
def remote(self, node=None):
    """Check that user is able to create a table as remote table function
    only if they have REMOTE, SELECT, and CREATE TEMPORARY TABLE priviliges.
    """
    user_name = f"user_{getuid()}"
    table_name = f"table_{getuid()}"
    exitcode, message = errors.not_enough_privileges(name=f"{user_name}")

    if node is None:
        node = self.context.node

    node2 = self.context.cluster.node("clickhouse2")
    node3 = self.context.cluster.node("clickhouse3")

    try:
        with Given("I have a table on a cluster with two nodes on seperate shards"):
            node.query(
                f"CREATE TABLE {table_name} ON CLUSTER sharded_cluster12 (x UInt8) ENGINE=Memory"
            )

        with And(f"I have a user {user_name} on three nodes on seperate shards"):
            node.query(f"CREATE USER {user_name} ON CLUSTER sharded_cluster")

        with And("I have some data in the table on clickhouse1"):
            node.query(f"INSERT INTO {table_name} VALUES (1)")

        with When(
            f"I try to select from the table using remote table function as {user_name}"
        ):
            node.query(
                f"SELECT * FROM remote(sharded_cluster12, default.{table_name})",
                exitcode=exitcode,
                message=message,
                settings=[("user", f"{user_name}")],
            )

        with Then("I grant CREATE TEMPORARY TABLE and REMOTE privileges to a user"):
            node.query(f"GRANT CREATE TEMPORARY TABLE ON *.* TO {user_name}")
            node.query(f"GRANT REMOTE ON *.* TO {user_name}")

        with When(
            f"I try to select from the table using remote table function as {user_name}"
        ):
            node.query(
                f"SELECT * FROM remote(sharded_cluster12, default.{table_name})",
                exitcode=exitcode,
                message=message,
                settings=[("user", f"{user_name}")],
            )

        with Then("I grant SELECT privilege on the source table"):
            node.query(f"GRANT SELECT ON {table_name} TO {user_name}")

        with And(f"I successfully select from the remote table as {user_name}"):
            output = node.query(
                f"SELECT * FROM remote(sharded_cluster12, default.{table_name})",
                settings=[("user", f"{user_name}")],
            ).output
            default = node.query(f"SELECT * FROM {table_name}").output
            assert output == default, error()

        with When(
            "I try to select from the table as the same user, but from clickhouse2"
        ):
            node2.query(
                f"SELECT * FROM remote(sharded_cluster12, default.{table_name})",
                exitcode=exitcode,
                message=message,
                settings=[("user", f"{user_name}")],
            )

        with Then(
            "I grant CREATE TEMPORARY TABLE and REMOTE to the user on clickhouse2"
        ):
            node2.query(f"GRANT CREATE TEMPORARY TABLE ON *.* TO {user_name}")
            node2.query(f"GRANT REMOTE ON *.* TO {user_name}")

        with When(
            "I try to select from the table as the same user, but from clickhouse2"
        ):
            node2.query(
                f"SELECT * FROM remote(sharded_cluster12, default.{table_name})",
                exitcode=exitcode,
                message=message,
                settings=[("user", f"{user_name}")],
            )

        with Then(f"I grant SELECT on {table_name} to the user on clickhouse2"):
            node2.query(f"GRANT SELECT ON {table_name} TO {user_name}")

        with Then(
            f"I successfully select from the remote table as {user_name} from clickhouse2"
        ):
            output = node2.query(
                f"SELECT * FROM remote(sharded_cluster12, default.{table_name})",
                settings=[("user", f"{user_name}")],
            ).output
            assert output == default, error()

        with When(
            "I try to select from the table as the same user, but from clickhouse3"
        ):
            node3.query(
                f"SELECT * FROM remote(sharded_cluster12, default.{table_name})",
                exitcode=exitcode,
                message=message,
                settings=[("user", f"{user_name}")],
            )

        with Then(
            "I grant CREATE TEMPORARY TABLE and REMOTE to the user on clickhouse3"
        ):
            node3.query(f"GRANT CREATE TEMPORARY TABLE ON *.* TO {user_name}")
            node3.query(f"GRANT REMOTE ON *.* TO {user_name}")

        with When(
            "I try to select from the table as the same user, but from clickhouse3"
        ):
            node3.query(
                f"SELECT * FROM remote(sharded_cluster12, default.{table_name})",
                exitcode=exitcode,
                message=message,
                settings=[("user", f"{user_name}")],
            )

        with Then(f"I grant SELECT on {table_name} to the user on clickhouse3"):
            node3.query(f"GRANT SELECT ON {table_name} TO {user_name}")

        with Then(
            f"I successfully select from the remote table as {user_name} from clickhouse3"
        ):
            output = node3.query(
                f"SELECT * FROM remote(sharded_cluster12, default.{table_name})",
                settings=[("user", f"{user_name}")],
            ).output
            assert output == default, error()

    finally:
        with Finally(f"I drop the table {table_name} from the cluster", flags=TE):
            node.query(
                f"DROP TABLE IF EXISTS {table_name} ON CLUSTER sharded_cluster12"
            )

        with And(f"I drop the user from the cluster", flags=TE):
            node.query(f"DROP USER IF EXISTS {user_name} ON CLUSTER sharded_cluster")


@TestScenario
@Requirements(RQ_SRS_006_RBAC_Select_TableFunctions_Cluster("1.0"))
def cluster(self, node=None):
    """Check that user is able to create a table as cluster table function
    only if they have REMOTE, SELECT, and CREATE TEMPORARY TABLE priviliges.
    """
    user_name = f"user_{getuid()}"
    table_name = f"table_{getuid()}"
    exitcode, message = errors.not_enough_privileges(name=f"{user_name}")

    if node is None:
        node = self.context.node

    node2 = self.context.cluster.node("clickhouse2")
    node3 = self.context.cluster.node("clickhouse3")

    try:
        with Given("I have a table on a cluster with two nodes on seperate shards"):
            node.query(
                f"CREATE TABLE {table_name} ON CLUSTER sharded_cluster12 (x UInt8) ENGINE=Memory"
            )

        with And(f"I have a user {user_name} on three nodes on seperate shards"):
            node.query(f"CREATE USER {user_name} ON CLUSTER sharded_cluster")

        with And("I have some data in the table on clickhouse1"):
            node.query(f"INSERT INTO {table_name} VALUES (1)")

        with When(
            f"I try to select from the table using cluster table function as {user_name}"
        ):
            node.query(
                f"SELECT * FROM cluster(sharded_cluster12, default.{table_name})",
                exitcode=exitcode,
                message=message,
                settings=[("user", f"{user_name}")],
            )

        with Then("I grant CREATE TEMPORARY TABLE and cluster privileges to a user"):
            node.query(f"GRANT CREATE TEMPORARY TABLE ON *.* TO {user_name}")
            node.query(f"GRANT REMOTE ON *.* TO {user_name}")

        with When(
            f"I try to select from the table using cluster table function as {user_name}"
        ):
            node.query(
                f"SELECT * FROM cluster(sharded_cluster12, default.{table_name})",
                exitcode=exitcode,
                message=message,
                settings=[("user", f"{user_name}")],
            )

        with Then("I grant SELECT privilege on the source table"):
            node.query(f"GRANT SELECT ON {table_name} TO {user_name}")

        with And(f"I successfully select from the cluster table as {user_name}"):
            output = node.query(
                f"SELECT * FROM cluster(sharded_cluster12, default.{table_name})",
                settings=[("user", f"{user_name}")],
            ).output
            default = node.query(f"SELECT * FROM {table_name}").output
            assert output == default, error()

        with When(
            "I try to select from the table as the same user, but from clickhouse2"
        ):
            node2.query(
                f"SELECT * FROM cluster(sharded_cluster12, default.{table_name})",
                exitcode=exitcode,
                message=message,
                settings=[("user", f"{user_name}")],
            )

        with Then(
            "I grant CREATE TEMPORARY TABLE and cluster to the user on clickhouse2"
        ):
            node2.query(f"GRANT CREATE TEMPORARY TABLE ON *.* TO {user_name}")
            node2.query(f"GRANT REMOTE ON *.* TO {user_name}")

        with When(
            "I try to select from the table as the same user, but from clickhouse2"
        ):
            node2.query(
                f"SELECT * FROM cluster(sharded_cluster12, default.{table_name})",
                exitcode=exitcode,
                message=message,
                settings=[("user", f"{user_name}")],
            )

        with Then(f"I grant SELECT on {table_name} to the user on clickhouse2"):
            node2.query(f"GRANT SELECT ON {table_name} TO {user_name}")

        with Then(
            f"I successfully select from the cluster table as {user_name} from clickhouse2"
        ):
            output = node2.query(
                f"SELECT * FROM cluster(sharded_cluster12, default.{table_name})",
                settings=[("user", f"{user_name}")],
            ).output
            assert output == default, error()

        with When(
            "I try to select from the table as the same user, but from clickhouse3"
        ):
            node3.query(
                f"SELECT * FROM cluster(sharded_cluster12, default.{table_name})",
                exitcode=exitcode,
                message=message,
                settings=[("user", f"{user_name}")],
            )

        with Then(
            "I grant CREATE TEMPORARY TABLE and cluster to the user on clickhouse3"
        ):
            node3.query(f"GRANT CREATE TEMPORARY TABLE ON *.* TO {user_name}")
            node3.query(f"GRANT REMOTE ON *.* TO {user_name}")

        with When(
            "I try to select from the table as the same user, but from clickhouse3"
        ):
            node3.query(
                f"SELECT * FROM cluster(sharded_cluster12, default.{table_name})",
                exitcode=exitcode,
                message=message,
                settings=[("user", f"{user_name}")],
            )

        with Then(f"I grant SELECT on {table_name} to the user on clickhouse3"):
            node3.query(f"GRANT SELECT ON {table_name} TO {user_name}")

        with Then(
            f"I successfully select from the cluster table as {user_name} from clickhouse3"
        ):
            output = node3.query(
                f"SELECT * FROM cluster(sharded_cluster12, default.{table_name})",
                settings=[("user", f"{user_name}")],
            ).output
            assert output == default, error()

    finally:
        with Finally(f"I drop the table {table_name} from the cluster", flags=TE):
            node.query(
                f"DROP TABLE IF EXISTS {table_name} ON CLUSTER sharded_cluster12"
            )

        with And(f"I drop the user from the cluster", flags=TE):
            node.query(f"DROP USER IF EXISTS {user_name} ON CLUSTER sharded_cluster")


@TestOutline(Feature)
@Name("table functions")
def feature(self, stress=None, node="clickhouse1"):
    """Check the RBAC functionality of table functions."""
    self.context.node = self.context.cluster.node(node)

    if stress is not None:
        self.context.stress = stress

    Scenario(run=remote)
    Scenario(run=cluster)
