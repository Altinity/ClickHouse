"""The soak harness must find its containers by the same environment convention the scenario
framework uses (`CA_SOAK_NODE{i}_CONTAINER`, `CA_SOAK_RUSTFS_CONTAINER`, `CA_SOAK_FSCK_CONTAINER`),
resolved at CALL time. Discovered on the live-GCS stand (compose project `ca-live-gcs`, 2026-09-02):
`soak.run` hardcoded `ca-soak-ch1-1` for fsck, so every checkpoint reported `persistent-dangling`
from a `docker exec` into a container that was not running, while `Cluster` itself had already
followed the env override for the workload."""
import pytest

from soak import chaos, pool, run


def test_fsck_container_defaults_to_compose_name(monkeypatch):
    monkeypatch.delenv("CA_SOAK_FSCK_CONTAINER", raising=False)
    assert run.fsck_container() == "ca-soak-ch1-1"


def test_fsck_container_honours_env_at_call_time(monkeypatch):
    monkeypatch.setenv("CA_SOAK_FSCK_CONTAINER", "ca-live-gcs-ch1-1")
    # No reload: the override must be read when the checkpoint runs, not when the module was imported.
    assert run.fsck_container() == "ca-live-gcs-ch1-1"


def test_chaos_targets_honour_node_and_rustfs_env(monkeypatch):
    monkeypatch.setenv("CA_SOAK_NODE1_CONTAINER", "ca-live-gcs-ch1-1")
    monkeypatch.setenv("CA_SOAK_NODE2_CONTAINER", "ca-live-gcs-ch2-1")
    monkeypatch.setenv("CA_SOAK_RUSTFS_CONTAINER", "ca-live-gcs-rustfs1-1")
    assert chaos.container_for(chaos.FaultTarget.CH1) == "ca-live-gcs-ch1-1"
    assert chaos.container_for(chaos.FaultTarget.CH2) == "ca-live-gcs-ch2-1"
    assert chaos.container_for(chaos.FaultTarget.RUSTFS) == "ca-live-gcs-rustfs1-1"


def test_chaos_targets_default_to_compose_names(monkeypatch):
    for k in ("CA_SOAK_NODE1_CONTAINER", "CA_SOAK_NODE2_CONTAINER", "CA_SOAK_RUSTFS_CONTAINER"):
        monkeypatch.delenv(k, raising=False)
    assert chaos.container_for(chaos.FaultTarget.CH1) == "ca-soak-ch1-1"
    assert chaos.container_for(chaos.FaultTarget.CH2) == "ca-soak-ch2-1"
    assert chaos.container_for(chaos.FaultTarget.RUSTFS) == "ca-soak-rustfs1-1"


def test_chaos_both_has_no_single_container():
    with pytest.raises(KeyError):
        chaos.container_for(chaos.FaultTarget.BOTH)


def test_chaos_both_expands_to_both_overridden_nodes(monkeypatch):
    monkeypatch.setenv("CA_SOAK_NODE1_CONTAINER", "ca-live-gcs-ch1-1")
    monkeypatch.setenv("CA_SOAK_NODE2_CONTAINER", "ca-live-gcs-ch2-1")
    assert chaos._containers(chaos.FaultTarget.BOTH) == ["ca-live-gcs-ch1-1", "ca-live-gcs-ch2-1"]


def test_pool_dir_honours_env(monkeypatch):
    monkeypatch.setenv("CA_SOAK_POOL_DIR", "/data/other/pool")
    assert pool.pool_dir() == "/data/other/pool"
    monkeypatch.delenv("CA_SOAK_POOL_DIR")
    assert pool.pool_dir() == "/data/test/soak_pool"


def test_pool_rustfs_container_honours_env(monkeypatch):
    monkeypatch.setenv("CA_SOAK_RUSTFS_CONTAINER", "ca-live-gcs-rustfs1-1")
    assert pool.rustfs_container() == "ca-live-gcs-rustfs1-1"
    monkeypatch.delenv("CA_SOAK_RUSTFS_CONTAINER")
    assert pool.rustfs_container() == "ca-soak-rustfs1-1"
