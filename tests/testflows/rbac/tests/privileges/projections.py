from rbac.requirements import *
from rbac.helper.common import *
import rbac.helper.errors as errors


@TestSuite
def privilege_granted_directly_or_via_role(self, node=None):
    """Check that user is only able to manipulate projections when they have required privilege, either directly or via role."""
    role_name = f"role_{getuid()}"
    user_name = f"user_{getuid()}"

    if node is None:
        node = self.context.node

    with Suite("user with direct privilege"):
        with user(node, user_name):

            with When(
                f"I run checks that {user_name} is only able to manipulate projections with required privileges"
            ):
                for scenario in loads(current_module(), Scenario):
                    Scenario(test=scenario, name=scenario.name)(
                        grant_target_name=user_name, user_name=user_name
                    )

    with Suite("user with privilege via role"):
        with user(node, user_name), role(node, role_name):

            with When("I grant the role to the user"):
                node.query(f"GRANT {role_name} TO {user_name}")

            with And(
                f"I run checks that {user_name} with {role_name} is only able to manipulate projections with required privileges"
            ):
                for scenario in loads(current_module(), Scenario):
                    Scenario(test=scenario, name=scenario.name)(
                        grant_target_name=role_name, user_name=user_name
                    )


@TestScenario
@Name("ADD PROJECTION")
@Requirements(
    RQ_SRS_006_RBAC_Privileges_AlterProjection_Add("1.0"),
)
def add_projection(self, grant_target_name, user_name, node=None):
    """I check that user is only able to add projections with the ALTER TABLE ADD PROJECTION."""
    exitcode, message = errors.not_enough_privileges(name=f"{user_name}")
    table_name = f"table_{getuid()}"
    proj_name = f"proj_{getuid()}"
    node = node or self.context.node
    privilege = (
        current().context.privilege
        if current().context.privilege != "default"
        else "ADD PROJECTION"
    )

    with table(node, table_name):
        with When("I check that user is unable to add a projection"):
            node.query(
                f"ALTER TABLE {table_name} ADD PROJECTION {proj_name} (SELECT y)",
                settings=[("user", user_name)],
                exitcode=exitcode,
                message=message,
            )

        with Then(f"I grant the {privilege} to the {grant_target_name}"):
            node.query(f"GRANT {privilege} ON *.* TO {grant_target_name}")

        with When("I check the user is able to add a projection"):
            node.query(
                f"ALTER TABLE {table_name} ADD PROJECTION {proj_name} (SELECT y)",
                settings=[("user", user_name)],
            )

        with Then(f"I drop the projection"):
            node.query(f"ALTER TABLE {table_name} DROP PROJECTION {proj_name}")

        with And(f"I revoke the {privilege} from {grant_target_name}"):
            node.query(f"REVOKE {privilege} ON *.* FROM {grant_target_name}")

        with When("I check that user is unable to add a projection"):
            node.query(
                f"ALTER TABLE {table_name} ADD PROJECTION {proj_name} (SELECT y)",
                settings=[("user", user_name)],
                exitcode=exitcode,
                message=message,
            )


@TestScenario
@Name("DROP PROJECTION")
@Requirements(
    RQ_SRS_006_RBAC_Privileges_AlterProjection_Drop("1.0"),
)
def drop_projection(self, grant_target_name, user_name, node=None):
    """I check that user is only able to drop projections with the ALTER TABLE DROP PROJECTION."""
    exitcode, message = errors.not_enough_privileges(name=f"{user_name}")
    table_name = f"table_{getuid()}"
    proj_name = f"proj_{getuid()}"
    node = node or self.context.node
    privilege = (
        current().context.privilege
        if current().context.privilege != "default"
        else "DROP PROJECTION"
    )

    with table(node, table_name):

        with Given("I have a projection"):
            node.query(
                f"ALTER TABLE {table_name} ADD PROJECTION {proj_name} (SELECT y)"
            )

        with When("I check that user is unable to drop a projection"):
            node.query(
                f"ALTER TABLE {table_name} DROP PROJECTION {proj_name}",
                settings=[("user", user_name)],
                exitcode=exitcode,
                message=message,
            )

        with Then(f"I grant the {privilege} to the {grant_target_name}"):
            node.query(f"GRANT {privilege} ON *.* TO {grant_target_name}")

        with When("I check the user is able to drop a projection"):
            node.query(
                f"ALTER TABLE {table_name} DROP PROJECTION {proj_name}",
                settings=[("user", user_name)],
            )

        with Then(f"I add the projection back"):
            node.query(
                f"ALTER TABLE {table_name} ADD PROJECTION {proj_name} (SELECT y)"
            )

        with And(f"I revoke the {privilege} from {grant_target_name}"):
            node.query(f"REVOKE {privilege} ON *.* FROM {grant_target_name}")

        with When("I check that user is unable to drop a projection"):
            node.query(
                f"ALTER TABLE {table_name} DROP PROJECTION {proj_name}",
                settings=[("user", user_name)],
                exitcode=exitcode,
                message=message,
            )


@TestScenario
@Name("MATERIALIZE PROJECTION")
@Requirements(
    RQ_SRS_006_RBAC_Privileges_AlterProjection_Materialize("1.0"),
)
def materialize_projection(self, grant_target_name, user_name, node=None):
    """I check that user is only able to drop projections with the ALTER TABLE MATERIALIZE PROJECTION."""
    exitcode, message = errors.not_enough_privileges(name=f"{user_name}")
    table_name = f"table_{getuid()}"
    proj_name = f"proj_{getuid()}"
    node = node or self.context.node
    privilege = (
        current().context.privilege
        if current().context.privilege != "default"
        else "MATERIALIZE PROJECTION"
    )

    with table(node, table_name, "ReplicatedMergeTree-one_shard_cluster"):

        with Given("I have a projection"):
            node.query(
                f"ALTER TABLE {table_name} ADD PROJECTION {proj_name} (SELECT y)"
            )

        with And("I have some data on the table"):
            node.query(
                f"INSERT INTO {table_name} VALUES ('2019-01-01', 'a', 2, 'zzzz', 8)"
            )

        with When("I check that user is unable to materialize a projection"):
            node.query(
                f"ALTER TABLE {table_name} MATERIALIZE PROJECTION {proj_name} IN PARTITION 8",
                settings=[("user", user_name)],
                exitcode=exitcode,
                message=message,
            )

        with Then(f"I grant the {privilege} to the {grant_target_name}"):
            node.query(f"GRANT {privilege} ON *.* TO {grant_target_name}")

        with When("I check the user is able to materialize a projection"):
            node.query(
                f"ALTER TABLE {table_name} MATERIALIZE PROJECTION {proj_name} IN PARTITION 8",
                settings=[("user", user_name)],
            )

        with And(f"I revoke the {privilege} from {grant_target_name}"):
            node.query(f"REVOKE {privilege} ON *.* FROM {grant_target_name}")

        with When("I check that user is unable to materialize a projection"):
            node.query(
                f"ALTER TABLE {table_name} MATERIALIZE PROJECTION {proj_name} IN PARTITION 8",
                settings=[("user", user_name)],
                exitcode=exitcode,
                message=message,
            )


@TestScenario
@Name("CLEAR PROJECTION")
@Requirements(
    RQ_SRS_006_RBAC_Privileges_AlterProjection_Clear("1.0"),
)
def clear_projection(self, grant_target_name, user_name, node=None):
    """I check that user is only able to drop projections with the ALTER TABLE CLEAR PROJECTION."""
    exitcode, message = errors.not_enough_privileges(name=f"{user_name}")
    table_name = f"table_{getuid()}"
    proj_name = f"proj_{getuid()}"
    node = node or self.context.node
    privilege = (
        current().context.privilege
        if current().context.privilege != "default"
        else "CLEAR PROJECTION"
    )

    with table(node, table_name, "ReplicatedMergeTree-one_shard_cluster"):

        with Given("I have a projection"):
            node.query(
                f"ALTER TABLE {table_name} ADD PROJECTION {proj_name} (SELECT y)"
            )

        with And("I have some data on the table"):
            node.query(
                f"INSERT INTO {table_name} VALUES ('2019-01-01', 'a', 2, 'zzzz', 8)"
            )

        with When("I check that user is unable to clear a projection"):
            node.query(
                f"ALTER TABLE {table_name} CLEAR PROJECTION {proj_name} IN PARTITION 8",
                settings=[("user", user_name)],
                exitcode=exitcode,
                message=message,
            )

        with Then(f"I grant the {privilege} to the {grant_target_name}"):
            node.query(f"GRANT {privilege} ON *.* TO {grant_target_name}")

        with When("I check the user is able to clear a projection"):
            node.query(
                f"ALTER TABLE {table_name} CLEAR PROJECTION {proj_name} IN PARTITION 8",
                settings=[("user", user_name)],
            )

        # with Then(f"I add the projection back"):
        #     node.query(f"ALTER TABLE {table_name} ADD PROJECTION {proj_name} (SELECT y)")

        with And(f"I revoke the {privilege} from {grant_target_name}"):
            node.query(f"REVOKE {privilege} ON *.* FROM {grant_target_name}")

        with When("I check that user is unable to clear a projection"):
            node.query(
                f"ALTER TABLE {table_name} CLEAR PROJECTION {proj_name} IN PARTITION 8",
                settings=[("user", user_name)],
                exitcode=exitcode,
                message=message,
            )


@TestFeature
@Requirements(
    RQ_SRS_006_RBAC_Privileges_AlterProjection("1.0"),
    RQ_SRS_006_RBAC_Privileges_All("1.0"),
)
@Name("projections")
def feature(self, node="clickhouse1", stress=None, parallel=None):
    """Check the RBAC functionality of projections."""
    self.context.node = self.context.cluster.node(node)

    if parallel is not None:
        self.context.parallel = parallel
    if stress is not None:
        self.context.stress = stress

    user_name = f"user_{getuid()}"
    role_name = f"role_{getuid()}"

    for current().context.privilege in ["ALL", "ALTER PROJECTION", "default"]:
        for scenario in loads(current_module(), Scenario):
            with user(self.context.node, user_name):
                Scenario(
                    f" {current().context.privilege} privilege, {scenario.name}, privilege granted to user",
                    test=scenario,
                    setup=instrument_clickhouse_server_log,
                )(grant_target_name=user_name, user_name=user_name)

            with user(self.context.node, user_name), role(self.context.node, role_name):

                with When("I grant the role to the user"):
                    self.context.node.query(f"GRANT {role_name} TO {user_name}")

                Scenario(
                    f" {current().context.privilege} privilege, {scenario.name}, privilege granted to role",
                    test=scenario,
                    setup=instrument_clickhouse_server_log,
                )(grant_target_name=role_name, user_name=user_name)
