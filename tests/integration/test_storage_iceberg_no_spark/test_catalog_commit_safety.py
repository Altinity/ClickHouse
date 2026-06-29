#!/usr/bin/env python3
"""Tests for the Iceberg REST catalog commit-safety bug.

Bug: on a network failure AFTER the catalog commit succeeds, ClickHouse's HTTP
layer retries the byte-identical commit POST, the catalog returns 409 (its
assert-ref-snapshot-id no longer matches), and -- before the fix --
`RestCatalog::updateMetadata` collapsed that 409 to `return false`, so the caller
ran cleanup() and deleted files the now-live snapshot references.

These tests interpose `catalog_fault_proxy.py` between ClickHouse and the real
REST catalog to make the "commit succeeds, response lost" window deterministic,
then assert the corruption invariant:

    NO catalog-referenced snapshot may point at a manifest list that is missing
    from object storage.

Wiring (already in place):
  * conftest.py merges docker_compose_rest_proxy.yml into the cluster; that runs
    the `rest-proxy` service on the shared network and publishes 8999:8181.
  * ClickHouse reaches the catalog THROUGH the proxy (CREATE DATABASE below uses
    `http://rest-proxy:8181/v1`); pyiceberg keeps talking to the real catalog at
    localhost:8182, so it observes true committed state.
  * proxy_control_url() returns http://localhost:8999, the host-published port.

The fix is re-read-and-classify by snapshot-id. On a failed commit,
`RestCatalog::updateMetadata` re-reads the catalog and returns a typed
`CommitOutcome`:
  * Committed       -- our snapshot-id is the catalog's current-snapshot-id, or is
                       present in snapshots[] (we landed, then were superseded).
                       Callers skip cleanup.
  * RejectedCleanly -- the re-read succeeded and our snapshot-id is provably absent
                       from populated snapshot state. Only here is cleanup safe.
  * Unknown         -- the re-read failed or carried no snapshot state. Callers
                       preserve all files, because a recoverable leak beats
                       unrecoverable corruption.
Making 409 non-retriable at the HTTP layer is an optional efficiency, not the
correctness fix: classification removes the corruption regardless of retry behavior.
"""

import uuid

import boto3
import pyarrow as pa
import pytest
import requests
from helpers.config_cluster import minio_access_key, minio_secret_key
from pyiceberg.catalog import load_catalog
from pyiceberg.partitioning import PartitionSpec
from pyiceberg.schema import Schema, NestedField
from pyiceberg.types import LongType, StringType

# pyiceberg -> REAL catalog (host-published port, as in test_iceberg_truncate.py)
REST_CATALOG_HOST_URL = "http://localhost:8182"
# ClickHouse -> catalog THROUGH the fault proxy (in-network hostname)
CH_CATALOG_URL_VIA_PROXY = "http://rest-proxy:8181/v1"
CATALOG_NAME = "demo"
BUCKET = "warehouse-rest"


# --------------------------------------------------------------------------- #
# Helpers
# --------------------------------------------------------------------------- #
def load_catalog_real(started_cluster):
    return load_catalog(
        CATALOG_NAME,
        **{
            "uri": REST_CATALOG_HOST_URL,
            "type": "rest",
            "s3.endpoint": f"http://{started_cluster.get_instance_ip('minio')}:9000",
            "s3.access-key-id": minio_access_key,
            "s3.secret-access-key": minio_secret_key,
        },
    )


def proxy_control_url(started_cluster):
    # rest-proxy publishes its control+forward port as 8999:8181
    # (docker_compose_rest_proxy.yml), so the host reaches it at localhost:8999.
    return "http://localhost:8999"


def arm_fault(started_cluster, rule):
    r = requests.post(f"{proxy_control_url(started_cluster)}/__fault/arm",
                      json=rule, timeout=10)
    r.raise_for_status()


def disarm_fault(started_cluster):
    requests.post(f"{proxy_control_url(started_cluster)}/__fault/disarm", timeout=10)


def fault_status(started_cluster):
    return requests.get(f"{proxy_control_url(started_cluster)}/__fault/status",
                        timeout=10).json()


def _s3(started_cluster):
    ip = started_cluster.get_instance_ip("minio")
    return boto3.client(
        "s3",
        endpoint_url=f"http://{ip}:9000",
        aws_access_key_id=minio_access_key,
        aws_secret_access_key=minio_secret_key,
    )


def s3_object_exists(started_cluster, s3_uri):
    assert s3_uri and s3_uri.startswith("s3://"), s3_uri
    bucket, _, key = s3_uri[len("s3://"):].partition("/")
    try:
        _s3(started_cluster).head_object(Bucket=bucket, Key=key)
        return True
    except Exception:
        return False


def snapshots_with_missing_manifest_list(started_cluster, table):
    """Catalog-referenced snapshots whose manifest list is gone from storage.
    Empty list == healthy."""
    missing = []
    for snap in (table.metadata.snapshots or []):
        ml = getattr(snap, "manifest_list", None)
        if ml and not s3_object_exists(started_cluster, ml):
            missing.append(snap)
    return missing


def _setup_table_with_one_snapshot(started_cluster, kind):
    """Create namespace+table, point CH at the proxy, seed snapshot N so the next
    commit carries assert-ref-snapshot-id. Returns (catalog, namespace, ch_ident,
    identifier, snap_N, instance)."""
    instance = started_cluster.instances["node1"]
    catalog = load_catalog_real(started_cluster)

    namespace = f"ch_commit_safety_{kind}_{uuid.uuid4().hex}"
    catalog.create_namespace(namespace)
    schema = Schema(
        NestedField(field_id=1, name="id", field_type=LongType(), required=False),
        NestedField(field_id=2, name="val", field_type=StringType(), required=False),
    )
    table_name = "t"
    identifier = f"{namespace}.{table_name}"
    catalog.create_table(
        identifier=identifier,
        schema=schema,
        location=f"s3://{BUCKET}/{identifier}",
        partition_spec=PartitionSpec(),
    )
    ch_ident = f"`{namespace}.{table_name}`"

    instance.query(f"DROP DATABASE IF EXISTS {namespace}")
    instance.query(
        f"""
        CREATE DATABASE {namespace}
            ENGINE = DataLakeCatalog('{CH_CATALOG_URL_VIA_PROXY}', 'minio', '{minio_secret_key}')
        SETTINGS
            catalog_type='rest',
            warehouse='{CATALOG_NAME}',
            storage_endpoint='http://minio:9000/{BUCKET}';
        """,
        settings={"allow_database_iceberg": 1},
    )

    # Seed snapshot N (via pyiceberg/real catalog) so the next CH commit asserts on it.
    catalog.load_table(identifier).append(
        pa.Table.from_pylist([{"id": 1, "val": "A"}])
    )
    snap_N = catalog.load_table(identifier).metadata.current_snapshot_id
    assert int(instance.query(
        f"SELECT count() FROM {namespace}.{ch_ident}").strip()) == 1
    return catalog, namespace, ch_ident, identifier, snap_N, instance


def test_insert_without_fault_is_clean_baseline(started_cluster_iceberg_no_spark):
    cluster = started_cluster_iceberg_no_spark
    catalog, namespace, ch_ident, identifier, snap_N, instance = \
        _setup_table_with_one_snapshot(cluster, "insert_baseline")

    disarm_fault(cluster)  # ensure nothing left armed from a prior test

    insert_error = None
    try:
        instance.query(
            f"INSERT INTO {namespace}.{ch_ident} VALUES (2, 'B')",
            settings={"allow_experimental_insert_into_iceberg": 1},
        )
    except Exception as e:
        insert_error = str(e)

    st = fault_status(cluster)
    assert st["faulted"] == 0, f"no fault should have fired in baseline: {st}"

    after = catalog.load_table(identifier)
    missing = snapshots_with_missing_manifest_list(cluster, after)

    print("BASELINE INSERT raised:", insert_error,
          "| current_snapshot:", after.metadata.current_snapshot_id,
          "| missing:", [s.snapshot_id for s in missing])

    assert insert_error is None, f"baseline INSERT should succeed, got: {insert_error}"
    assert not missing, (
        f"baseline must not corrupt; missing manifest lists: "
        f"{[s.snapshot_id for s in missing]}")
    assert int(instance.query(
        f"SELECT count() FROM {namespace}.{ch_ident}").strip()) == 2


def test_insert_commit_response_loss_is_handled(started_cluster_iceberg_no_spark):
    cluster = started_cluster_iceberg_no_spark
    catalog, namespace, ch_ident, identifier, snap_N, instance = \
        _setup_table_with_one_snapshot(cluster, "insert")

    # Drop the response of the FIRST commit POST for this table.
    arm_fault(cluster, {"method": "POST", "match_path_substr": "/tables/t",
                        "mode": "commit_then_drop", "count": 1})

    insert_error = None
    try:
        instance.query(
            f"INSERT INTO {namespace}.{ch_ident} VALUES (2, 'B')",
            settings={"allow_experimental_insert_into_iceberg": 1},
        )
    except Exception as e:
        insert_error = str(e)

    st = fault_status(cluster)
    assert st["faulted"] == 1, f"fault did not fire (check wiring/precondition): {st}"

    after = catalog.load_table(identifier)
    missing = snapshots_with_missing_manifest_list(cluster, after)

    print("INSERT raised:", insert_error,
          "| current_snapshot:", after.metadata.current_snapshot_id,
          "| missing:", [s.snapshot_id for s in missing])

    assert insert_error is None, (
        f"lost-response INSERT should succeed cleanly, got: {insert_error}")
    assert not missing, (
        "no catalog-referenced snapshot may point at a deleted manifest list; "
        f"corrupted: {[s.snapshot_id for s in missing]}")
    assert int(instance.query(
        f"SELECT count() FROM {namespace}.{ch_ident}").strip()) == 2
