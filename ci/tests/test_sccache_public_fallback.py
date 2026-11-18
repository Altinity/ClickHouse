"""Local and community runs with no S3_ARTIFACT_BUCKET read the public sccache bucket.

Fork pull_request jobs do not receive repository variables, and a local run
often has none either. The public bucket is substituted only after that run
has dropped AWS credentials, and the cache is forced read-only so the
fallback cannot upload.
"""

import os
import sys
from types import SimpleNamespace

import pytest

sys.path.insert(0, os.path.join(os.path.dirname(__file__), "../.."))
sys.path.insert(0, os.path.join(os.path.dirname(__file__), ".."))

from ci.jobs.build_clickhouse import setup_build_caches_env
from ci.praktika.settings import Settings

PUBLIC_BUCKET = "altinity-build-artifacts"


@pytest.fixture
def sccache_env(monkeypatch, tmp_path):
    monkeypatch.setattr(
        "ci.jobs.build_clickhouse.build_dir", str(tmp_path / "build")
    )
    monkeypatch.setattr("ci.jobs.build_clickhouse.temp_dir", str(tmp_path))
    monkeypatch.setattr(Settings, "S3_ARTIFACT_PATH", "")
    for name in (
        "SCCACHE_BUCKET",
        "SCCACHE_S3_READ_ONLY",
        "SCCACHE_S3_NO_CREDENTIALS",
        "AWS_ACCESS_KEY_ID",
        "AWS_SECRET_ACCESS_KEY",
        "SCCACHE_ENDPOINT",
    ):
        monkeypatch.delenv(name, raising=False)
    return monkeypatch


def _info(*, local=False, community=False, pr_number=0):
    return SimpleNamespace(
        is_local_run=local,
        is_community_pr=community,
        pr_number=pr_number,
    )


def test_community_empty_bucket_reads_public_cache(sccache_env):
    sccache_env.setenv("AWS_ACCESS_KEY_ID", "key")
    sccache_env.setenv("AWS_SECRET_ACCESS_KEY", "secret")

    setup_build_caches_env(_info(community=True, pr_number=2832))

    assert os.environ["SCCACHE_BUCKET"] == PUBLIC_BUCKET
    assert os.environ["SCCACHE_S3_READ_ONLY"] == "true"
    assert os.environ["SCCACHE_S3_NO_CREDENTIALS"] == "true"
    assert "AWS_ACCESS_KEY_ID" not in os.environ
    assert "AWS_SECRET_ACCESS_KEY" not in os.environ


def test_local_empty_bucket_reads_public_cache(sccache_env):
    setup_build_caches_env(_info(local=True, pr_number=-1))

    assert os.environ["SCCACHE_BUCKET"] == PUBLIC_BUCKET
    assert os.environ["SCCACHE_S3_READ_ONLY"] == "true"
    assert os.environ["SCCACHE_S3_NO_CREDENTIALS"] == "true"


def test_local_with_own_credentials_does_not_fall_back(sccache_env):
    sccache_env.setenv("AWS_ACCESS_KEY_ID", "key")
    sccache_env.setenv("AWS_SECRET_ACCESS_KEY", "secret")
    sccache_env.setattr(Settings, "S3_ARTIFACT_PATH", "my-bucket")

    setup_build_caches_env(_info(local=True, pr_number=-1))

    assert os.environ["SCCACHE_BUCKET"] == "my-bucket"
    assert "SCCACHE_S3_NO_CREDENTIALS" not in os.environ
    assert "SCCACHE_S3_READ_ONLY" not in os.environ


def test_local_credentials_without_a_bucket_do_not_fall_back(sccache_env):
    sccache_env.setenv("AWS_ACCESS_KEY_ID", "key")
    sccache_env.setenv("AWS_SECRET_ACCESS_KEY", "secret")

    setup_build_caches_env(_info(local=True, pr_number=-1))

    assert os.environ["SCCACHE_BUCKET"] == ""
    assert "SCCACHE_S3_NO_CREDENTIALS" not in os.environ


def test_trusted_run_does_not_substitute_the_public_bucket(sccache_env):
    setup_build_caches_env(_info(pr_number=0))

    assert os.environ["SCCACHE_BUCKET"] == ""
    assert "SCCACHE_S3_READ_ONLY" not in os.environ
    assert "SCCACHE_S3_NO_CREDENTIALS" not in os.environ
