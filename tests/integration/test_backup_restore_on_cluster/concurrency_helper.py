from pathlib import Path
from typing import List

from helpers.cluster import ClickHouseCluster, ClickHouseInstance


def generate_cluster_def(file: str, num_nodes: int) -> str:
    # For multiple workers, it has race and sometimes errors out,
    # so we generate it once and reuse
    path = (
        Path(__file__).parent / f"_gen/cluster_{Path(file).stem}_{num_nodes}_nodes.xml"
    )
    replicas = "\n".join(
        f"""                <replica>
                    <host>node{i}</host>
                    <port>9000</port>
                </replica>"""
        for i in range(num_nodes)
    )
    config = f"""<clickhouse>
    <remote_servers>
        <cluster>
            <shard>
{replicas}
            </shard>
        </cluster>
    </remote_servers>
</clickhouse>"""
    if path.is_file():
        existing = path.read_text(encoding="utf-8")
        if existing == config:
            return str(path.absolute())
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(
        encoding="utf-8",
        data=config,
    )
    return str(path.absolute())


def generate_cas_server_root_def(file: str, node_index: int) -> str:
    path = (
        Path(__file__).parent
        / f"_gen/cas_server_root_{Path(file).stem}_node{node_index}.xml"
    )
    config = f"""<clickhouse>
    <storage_configuration>
        <disks>
            <cas>
                <cas_server_root_id>itest-{Path(file).stem}-node{node_index}</cas_server_root_id>
            </cas>
        </disks>
    </storage_configuration>
</clickhouse>"""
    if path.is_file() and path.read_text(encoding="utf-8") == config:
        return str(path.absolute())
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(encoding="utf-8", data=config)
    return str(path.absolute())


def add_nodes_to_cluster(
    cluster: ClickHouseCluster,
    num_nodes: int,
    main_configs: List[str],
    user_configs: List[str],
    cas_file: str = None,
    **kwargs
) -> List[ClickHouseInstance]:
    def node_main_configs(i):
        if cas_file is None:
            return main_configs
        return main_configs + [
            "configs/cas_storage.xml",
            generate_cas_server_root_def(cas_file, i),
        ]

    if cas_file is not None:
        kwargs["with_rustfs"] = True

    nodes = [
        cluster.add_instance(
            f"node{i}",
            main_configs=node_main_configs(i),
            user_configs=user_configs,
            external_dirs=["/backups/"],
            macros={"replica": f"node{i}", "shard": "shard1"},
            with_zookeeper=True,
            **kwargs
        )
        for i in range(num_nodes)
    ]
    return nodes


def create_test_table(node: ClickHouseInstance, storage_policy: str = None) -> None:
    settings = f" SETTINGS storage_policy = '{storage_policy}'" if storage_policy else ""
    node.query(
        """CREATE TABLE tbl ON CLUSTER 'cluster' ( x UInt64 )
ENGINE=ReplicatedMergeTree('/clickhouse/tables/tbl/', '{replica}')
ORDER BY tuple()"""
        + settings
    )
