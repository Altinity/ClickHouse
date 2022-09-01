from testflows.core import *
from testflows.asserts import error

from rbac.requirements import *
from rbac.helper.common import *
import rbac.helper.errors as errors


@TestSuite
def privileges_granted_directly(self, node=None):
    """Check that a user is able to execute `SYSTEM THREAD FUZZER` commands if and only if
    the privilege has been granted directly.
    """
    user_name = f"user_{getuid()}"

    if node is None:
        node = self.context.node

    with user(node, f"{user_name}"):

        Suite(
            run=start_thread_fuzzer,
            examples=Examples(
                "privilege grant_target_name user_name",
                [
                    tuple(list(row) + [user_name, user_name])
                    for row in start_thread_fuzzer.examples
                ],
                args=Args(name="check privilege={privilege}", format_name=True),
            ),
        )

        Suite(
            run=stop_thread_fuzzer,
            examples=Examples(
                "privilege grant_target_name user_name",
                [
                    tuple(list(row) + [user_name, user_name])
                    for row in stop_thread_fuzzer.examples
                ],
                args=Args(name="check privilege={privilege}", format_name=True),
            ),
        )


@TestSuite
def privileges_granted_via_role(self, node=None):
    """Check that a user is able to execute `SYSTEM THREAD FUZZER` commands if and only if
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
            run=start_thread_fuzzer,
            examples=Examples(
                "privilege grant_target_name user_name",
                [
                    tuple(list(row) + [role_name, user_name])
                    for row in start_thread_fuzzer.examples
                ],
                args=Args(name="check privilege={privilege}", format_name=True),
            ),
        )

        Suite(
            run=stop_thread_fuzzer,
            examples=Examples(
                "privilege grant_target_name user_name",
                [
                    tuple(list(row) + [role_name, user_name])
                    for row in stop_thread_fuzzer.examples
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
        ("SYSTEM THREAD FUZZER",),
    ],
)
def start_thread_fuzzer(self, privilege, grant_target_name, user_name, node=None):
    """Check that user is only able to execute `SYSTEM START THREAD FUZZER` when they have privilege."""
    exitcode, message = errors.not_enough_privileges(name=user_name)

    if node is None:
        node = self.context.node

    with Scenario("SYSTEM START THREAD FUZZER without privilege"):

        with When("I grant the user NONE privilege"):
            node.query(f"GRANT NONE TO {grant_target_name}")

        with And("I grant the user USAGE privilege"):
            node.query(f"GRANT USAGE ON *.* TO {grant_target_name}")

        with Then("I check the user can't start thread fuzzer"):
            node.query(
                f"SYSTEM START THREAD FUZZER",
                settings=[("user", f"{user_name}")],
                exitcode=exitcode,
                message=message,
            )

    with Scenario("SYSTEM START THREAD FUZZER with privilege"):

        with When(f"I grant {privilege} on the table"):
            node.query(f"GRANT {privilege} ON *.* TO {grant_target_name}")

        with Then("I check the user can start thread fuzzer"):
            node.query(
                f"SYSTEM START THREAD FUZZER", settings=[("user", f"{user_name}")]
            )

    with Scenario("SYSTEM START THREAD FUZZER with revoked privilege"):

        with When(f"I grant {privilege} on the table"):
            node.query(f"GRANT {privilege} ON *.* TO {grant_target_name}")

        with And(f"I revoke {privilege} on the table"):
            node.query(f"REVOKE {privilege} ON *.* FROM {grant_target_name}")

        with Then("I check the user can't start thread fuzzer"):
            node.query(
                f"SYSTEM START THREAD FUZZER",
                settings=[("user", f"{user_name}")],
                exitcode=exitcode,
                message=message,
            )


@TestOutline(Suite)
@Examples(
    "privilege",
    [
        ("ALL",),
        ("SYSTEM",),
        ("SYSTEM THREAD FUZZER",),
    ],
)
def stop_thread_fuzzer(self, privilege, grant_target_name, user_name, node=None):
    """Check that user is only able to execute `SYSTEM STOP THREAD FUZZER` when they have privilege."""
    exitcode, message = errors.not_enough_privileges(name=user_name)

    if node is None:
        node = self.context.node

    with Scenario("SYSTEM STOP THREAD FUZZER without privilege"):

        with When("I grant the user NONE privilege"):
            node.query(f"GRANT NONE TO {grant_target_name}")

        with And("I grant the user USAGE privilege"):
            node.query(f"GRANT USAGE ON *.* TO {grant_target_name}")

        with Then("I check the user can't stop thread fuzzer"):
            node.query(
                f"SYSTEM STOP THREAD FUZZER",
                settings=[("user", f"{user_name}")],
                exitcode=exitcode,
                message=message,
            )

    with Scenario("SYSTEM STOP THREAD FUZZER with privilege"):

        with When(f"I grant {privilege} on the table"):
            node.query(f"GRANT {privilege} ON *.* TO {grant_target_name}")

        with Then("I check the user can stop thread fuzzer"):
            node.query(
                f"SYSTEM STOP THREAD FUZZER", settings=[("user", f"{user_name}")]
            )

    with Scenario("SYSTEM STOP THREAD FUZZER with revoked privilege"):

        with When(f"I grant {privilege} on the table"):
            node.query(f"GRANT {privilege} ON *.* TO {grant_target_name}")

        with And(f"I revoke {privilege} on the table"):
            node.query(f"REVOKE {privilege} ON *.* FROM {grant_target_name}")

        with Then("I check the user can't stop thread fuzzer"):
            node.query(
                f"SYSTEM STOP THREAD FUZZER",
                settings=[("user", f"{user_name}")],
                exitcode=exitcode,
                message=message,
            )


@TestFeature
@Name("system thread fuzzer")
@Requirements(
    RQ_SRS_006_RBAC_Privileges_System_ThreadFuzzer("1.0"),
    RQ_SRS_006_RBAC_Privileges_All("1.0"),
    RQ_SRS_006_RBAC_Privileges_None("1.0"),
)
def feature(self, node="clickhouse1"):
    """Check the RBAC functionality of SYSTEM THREAD FUZZER."""
    self.context.node = self.context.cluster.node(node)

    Suite(run=privileges_granted_directly, setup=instrument_clickhouse_server_log)
    Suite(run=privileges_granted_via_role, setup=instrument_clickhouse_server_log)
