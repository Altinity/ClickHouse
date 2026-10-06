import logging

import pytest

from helpers.cluster import ClickHouseCluster
from helpers.export_partition_helpers import SOURCE_ENGINE_IDS, SOURCE_ENGINES

# `TTL ... EXPORT TO TABLE`, split across several modules so the harness can spread them over
# xdist workers (`--dist=loadfile` assigns a whole module to one worker).
#
# Each module declares the instances it needs in `CLUSTER_INSTANCES` and gets a cluster with only
# those, so modules that never touch a second replica do not start one.

REPLICA = dict(
    main_configs=[
        "configs/named_collections.xml",
        "configs/allow_experimental_export_partition.xml",
        "configs/config.d/metadata_log.xml",
    ],
    user_configs=["configs/users.d/profile.xml"],
    with_minio=True,
    stay_alive=True,
    with_zookeeper=True,
    keeper_required_feature_flags=["multi_read"],
)

INSTANCES = {"replica1": REPLICA, "replica2": REPLICA}


@pytest.fixture(scope="module")
def cluster(request):
    instance_names = getattr(request.module, "CLUSTER_INSTANCES", list(INSTANCES))
    try:
        cluster = ClickHouseCluster(__file__)
        for name in instance_names:
            cluster.add_instance(name, **INSTANCES[name])
        logging.info("Starting cluster with instances %s...", instance_names)
        cluster.start()
        yield cluster
    finally:
        cluster.shutdown()


@pytest.fixture(autouse=True)
def drop_tables_after_test(cluster):
    """Drop every table of the default database after each test, so the TTL schedulers of finished
    tests stop running and do not disturb the next ones."""
    yield
    for instance_name, instance in cluster.instances.items():
        try:
            instance.query("SYSTEM DISABLE FAILPOINT export_part_pause_before_schema_validation")
            tables = instance.query(
                "SELECT name FROM system.tables WHERE database = 'default' FORMAT TabSeparated"
            ).split()
            if tables:
                instance.query("".join(f"DROP TABLE IF EXISTS default.`{table}` SYNC;" for table in tables))
        except Exception as e:
            logging.warning(f"drop_tables_after_test: cleanup failed on {instance_name}: {e}")


@pytest.fixture(params=SOURCE_ENGINES, ids=SOURCE_ENGINE_IDS)
def source_engine(request):
    """The MergeTree flavour of the source table. A test that requests this fixture runs once per
    engine; scenarios that need several replicas do not request it."""
    return request.param
