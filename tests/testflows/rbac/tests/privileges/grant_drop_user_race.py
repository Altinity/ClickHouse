import time

from testflows.core import *
from testflows.asserts import error

from rbac.requirements import *
from rbac.helper.common import *


@TestStep(Then)
def drop_user(self, node, user):
    """Drop the provided user."""
    with Then(f"I drop {user}"):
        node.query(f"DROP USER IF EXISTS {user}")


@TestStep(Then)
def grant_all(self, node, from_user, to_user):
    """Grant all from one user to another."""
    try:
        with Then(f"{from_user} grants all to {to_user}"):
            node.query(f"GRANT ALL ON *.* TO {to_user}", settings=[("user", from_user)])
        self.context.privilege = True

    except:
        self.context.privilege = False


@TestFeature
@Name("grant drop user race")
def feature(self, stress=None, node="clickhouse1"):
    """Check race condition when granting privileges and dropping users."""
    node = self.context.cluster.node(node)

    role_name = f"role_{getuid()}"
    user0_name = f"user0_{getuid()}"
    user1_name = f"user1_{getuid()}"
    table_name = f"table_{getuid()}"

    if stress is not None:
        self.context.stress = stress

    with role(node, role_name):
        with user(node, user0_name):
            with Given("The user has no privileges"):
                node.query(f"REVOKE ALL ON *.* FROM {user0_name}")

            with And(f"I have a table {table_name}"):
                node.query(f"CREATE TABLE {table_name} (n int) engine=Memory")

            with And("It has some values"):
                node.query(f"INSERT INTO {table_name} VALUES (1)")
            start = time.time()

            while time.time() - start < 10:

                with When("I create another user"):
                    node.query(f"CREATE USER IF NOT EXISTS {user1_name} GRANTEES NONE")

                with And(f"I grant all to the user {user1_name} with grant option"):
                    node.query(f"GRANT ALL ON *.* TO {user1_name} WITH GRANT OPTION")

                with Then(f"I drop the user"):
                    Step(test=drop_user, parallel=True)(node=node, user=user1_name)

                with And(f"Grant privilege to the original user in parallel"):
                    Step(test=grant_all, parallel=True)(
                        node=node, to_user=user0_name, from_user=user1_name
                    )

                with Finally(f"I check if {user0_name} succesfully gained privileges."):
                    if self.context.privilege:
                        node.query(
                            f"SELECT * FROM {table_name}",
                            settings=[("user", user0_name)],
                        )
                    else:
                        node.query(
                            f"SELECT * FROM {table_name}",
                            message="DB::Exception: user0_grant_drop_user_race",
                            exitcode=241,
                            settings=[("user", user0_name)],
                        )
