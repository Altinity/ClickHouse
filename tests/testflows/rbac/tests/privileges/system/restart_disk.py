from testflows.core import *
from testflows.asserts import error

from rbac.requirements import *
from rbac.helper.common import *
import rbac.helper.errors as errors


@TestSuite
def privileges_granted_directly(self, node=None):
    """Check that a user is able to execute `SYSTEM RESTART DISK` commands if and only if
    the privilege has been granted directly.
    """
    user_name = f"user_{getuid()}"

    if node is None:
        node = self.context.node

    with user(node, f"{user_name}"):

        Suite(
            run=restart_disk,
            examples=Examples(
                "privilege grant_target_name user_name",
                [
                    tuple(list(row) + [user_name, user_name])
                    for row in restart_disk.examples
                ],
                args=Args(name="check privilege={privilege}", format_name=True),
            ),
        )


@TestSuite
def privileges_granted_via_role(self, node=None):
    """Check that a user is able to execute `SYSTEM RESTART DISK` commands if and only if
    the privilege has been granted via role.
    """
    user_name = f"user_{getuid()}"
    role_name = f"role_{getuid()}"

    if node is None:
        node = self.context.node

    with user(node, f"{user_name}"), role(node, f"{role_name}"):

        with When("I grant the role to the user"):
            node.query(f"GRANT {role_name} TO {user_name}")

        Suite(
            run=restart_disk,
            examples=Examples(
                "privilege grant_target_name user_name",
                [
                    tuple(list(row) + [role_name, user_name])
                    for row in restart_disk.examples
                ],
                args=Args(name="check privilege={privilege}", format_name=True),
            ),
        )


@TestOutline(Suite)
@Examples(
    "privilege",
    [
        ("ALL",),
        ("SYSTEM",),
        ("SYSTEM RESTART DISK",),
    ],
)
def restart_disk(self, privilege, grant_target_name, user_name, node=None):
    """Check that user is only able to execute `SYSTEM RESTART DISK` when they have privilege."""
    exitcode, message = errors.not_enough_privileges(name=user_name)

    if node is None:
        node = self.context.node

    with Scenario("SYSTEM RESTART DISK without privilege"):

        with When("I grant the user NONE privilege"):
            node.query(f"GRANT NONE TO {grant_target_name}")

        with And("I grant the user USAGE privilege"):
            node.query(f"GRANT USAGE ON *.* TO {grant_target_name}")

        with Then("I check the user can't restart disk"):
            node.query(
                f"SYSTEM RESTART DISK some_disk",
                settings=[("user", f"{user_name}")],
                exitcode=exitcode,
                message=message,
            )

    with Scenario("SYSTEM RESTART DISK with privilege"):

        with When(f"I grant {privilege} on the table"):
            node.query(f"GRANT {privilege} ON *.* TO {grant_target_name}")

        with Then("I check the user can restart disk"):
            node.query(
                f"SYSTEM RESTART DISK some_disk",
                settings=[("user", f"{user_name}")],
                exitcode=223,
                message="DB::Exception: Unknown disk",
            )

    with Scenario("SYSTEM RESTART DISK with revoked privilege"):

        with When(f"I grant {privilege} on the table"):
            node.query(f"GRANT {privilege} ON *.* TO {grant_target_name}")

        with And(f"I revoke {privilege} on the table"):
            node.query(f"REVOKE {privilege} ON *.* FROM {grant_target_name}")

        with Then("I check the user can't restart disk"):
            node.query(
                f"SYSTEM RESTART DISK some_disk",
                settings=[("user", f"{user_name}")],
                exitcode=exitcode,
                message=message,
            )


@TestFeature
@Name("system restart disk")
@Requirements(
    RQ_SRS_006_RBAC_Privileges_System_RestartDisk("1.0"),
    RQ_SRS_006_RBAC_Privileges_All("1.0"),
    RQ_SRS_006_RBAC_Privileges_None("1.0"),
)
def feature(self, node="clickhouse1"):
    """Check the RBAC functionality of SYSTEM RESTART DISK."""
    self.context.node = self.context.cluster.node(node)

    Suite(run=privileges_granted_directly, setup=instrument_clickhouse_server_log)
    Suite(run=privileges_granted_via_role, setup=instrument_clickhouse_server_log)
