from testflows.core import *

from rbac.helper.common import *


@TestFeature
@Name("race condition")
def feature(self):
    """Check RBAC behaves as expected under heavy load with threadfuzzer enabled."""

    self.context.cluster.node("clickhouse1").enable_thread_fuzzer()

    Feature(
        run=load("rbac.tests.privileges.alter.alter_column", "feature"), parallel=True
    )
