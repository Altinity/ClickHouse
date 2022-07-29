from rbac.requirements import *
from rbac.helper.common import *
import rbac.helper.errors as errors


@TestSuite
def privilege_granted_directly_or_via_role(self, node=None):
    """Check that user is only able to execute DROP FUNCTION when they have required privilege, either directly or via role."""
    role_name = f"role_{getuid()}"
    user_name = f"user_{getuid()}"

    if node is None:
        node = self.context.node

    with Suite("user with direct privilege"):
        with user(node, user_name):

            with When(
                f"I run checks that {user_name} is only able to execute DROP FUNCTION with required privileges"
            ):
                privilege_check(
                    grant_target_name=user_name, user_name=user_name, node=node
                )

    with Suite("user with privilege via role"):
        with user(node, user_name), role(node, role_name):

            with When("I grant the role to the user"):
                node.query(f"GRANT {role_name} TO {user_name}")

            with And(
                f"I run checks that {user_name} with {role_name} is only able to execute DROP FUNCTION with required privileges"
            ):
                privilege_check(
                    grant_target_name=role_name, user_name=user_name, node=node
                )


def privilege_check(grant_target_name, user_name, node=None):
    """Run scenarios to check the user's access with different privileges."""
    exitcode, message = errors.not_enough_privileges(name=f"{user_name}")

    with Scenario("user without privilege"):
        func_name = f"db_{getuid()}"

        try:
            with Given("I have a function"):
                node.query(f"CREATE FUNCTION {func_name} AS (x) -> 2*x;")

            with When("I grant the user NONE privilege"):
                node.query(f"GRANT NONE TO {grant_target_name}")

            with And("I grant the user USAGE privilege"):
                node.query(f"GRANT USAGE ON *.* TO {grant_target_name}")

            with Then("I attempt to drop the function"):
                node.query(
                    f"DROP FUNCTION {func_name}",
                    settings=[("user", user_name)],
                    exitcode=exitcode,
                    message=message,
                )
        finally:
            with Finally("I drop the function"):
                node.query(f"DROP FUNCTION IF EXISTS {func_name}")

    with Scenario("user with privilege"):
        func_name = f"db_{getuid()}"

        try:
            with Given("I have a function"):
                node.query(f"CREATE FUNCTION {func_name} AS (x) -> 2*x;")

            with When(f"I grant {current().context.privilege} privilege"):
                node.query(
                    f"GRANT {current().context.privilege} ON *.* TO {grant_target_name}"
                )

            with Then("I attempt to drop a function"):
                node.query(f"DROP FUNCTION {func_name}", settings=[("user", user_name)])

        finally:
            with Finally("I drop the function"):
                node.query(f"DROP FUNCTION IF EXISTS {func_name}")

    with Scenario("user with revoked privilege"):
        func_name = f"db_{getuid()}"

        try:
            with Given("I have a function"):
                node.query(f"CREATE FUNCTION {func_name} AS (x) -> 2*x;")

            with When(f"I grant the {current().context.privilege} privilege"):
                node.query(
                    f"GRANT {current().context.privilege} ON *.* TO {grant_target_name}"
                )

            with And(f"I revoke the {current().context.privilege} privilege"):
                node.query(
                    f"REVOKE {current().context.privilege} ON *.* FROM {grant_target_name}"
                )

            with Then("I attempt to drop a function"):
                node.query(
                    f"DROP FUNCTION {func_name}",
                    settings=[("user", user_name)],
                    exitcode=exitcode,
                    message=message,
                )

        finally:
            with Finally("I drop the function"):
                node.query(f"DROP FUNCTION IF EXISTS {func_name}")


@TestFeature
@Requirements(
    RQ_SRS_006_RBAC_Privileges_Drop("1.0"),
    RQ_SRS_006_RBAC_Privileges_DropFunction("1.0"),
    RQ_SRS_006_RBAC_Privileges_All("1.0"),
    RQ_SRS_006_RBAC_Privileges_None("1.0"),
)
@Name("drop function")
def feature(self, node="clickhouse1", stress=None, parallel=None):
    """Check the RBAC functionality of DROP FUNCTION."""
    self.context.node = self.context.cluster.node(node)

    if parallel is not None:
        self.context.parallel = parallel
    if stress is not None:
        self.context.stress = stress

    for current().context.privilege in ["ALL", "DROP", "DROP FUNCTION"]:
        with Suite(
            f" {current().context.privilege} privilege",
            test=privilege_granted_directly_or_via_role,
            setup=instrument_clickhouse_server_log,
        ):
            privilege_granted_directly_or_via_role()
