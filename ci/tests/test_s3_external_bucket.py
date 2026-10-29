"""
Buckets listed in Settings.S3_EXTERNAL_BUCKETS live on an S3-compatible store other than AWS.
S3.put and S3.head_object must reach them through a boto3 client with their own endpoint and credentials,
and must keep using the AWS CLI for every other bucket.
"""

import datetime
import os
import sys

import pytest

sys.path.insert(0, os.path.join(os.path.dirname(__file__), "../.."))

pytest.importorskip("boto3")
from botocore.exceptions import ClientError

from ci.praktika import s3 as s3_module
from ci.praktika.cache import Cache
from ci.praktika.s3 import S3
from ci.praktika.settings import Settings

BUCKET = "external-bucket"
EXTERNAL = {
    BUCKET: {
        "endpoint_url": "https://s3.example.test",
        "region": "example-1",
        "access_key_env": "EXTERNAL_S3_ACCESS_KEY",
        "secret_key_env": "EXTERNAL_S3_SECRET_KEY",
        "public_read": True,
    }
}


class _FakeClient:
    def __init__(self, error_code=None):
        self.error_code = error_code
        self.put_args = None

    def _maybe_fail(self, operation):
        if self.error_code:
            raise ClientError({"Error": {"Code": self.error_code}}, operation)

    def put_object(self, Body, **kwargs):
        self._maybe_fail("PutObject")
        self.put_args = kwargs

    def head_object(self, Bucket, Key):
        self._maybe_fail("HeadObject")
        return {
            "ETag": '"etag"',
            "ContentLength": 3,
            "LastModified": datetime.datetime(2026, 1, 2, 3, 4, 5),
        }


@pytest.fixture(autouse=True)
def external_bucket(monkeypatch):
    monkeypatch.setattr(Settings, "S3_EXTERNAL_BUCKETS", EXTERNAL, raising=False)
    monkeypatch.setattr(S3, "_external_clients", {})

    def no_cli(*args, **kwargs):
        raise AssertionError("the AWS CLI must not be used for an external bucket")

    monkeypatch.setattr(s3_module.Shell, "get_output", no_cli)
    monkeypatch.setattr(s3_module.Shell, "get_res_stdout_stderr", no_cli)


def _use_client(monkeypatch, client):
    """Serve the same fake for signed and anonymous clients."""
    monkeypatch.setattr(
        S3, "_external_clients", {(BUCKET, False): client, (BUCKET, True): client}
    )


def _put(tmp_path, **kwargs):
    local_file = tmp_path / "archive"
    local_file.write_text("abc")
    return S3.put(f"{BUCKET}/prefix/archive", str(local_file), **kwargs)


def test_put_sends_if_none_match(tmp_path, monkeypatch):
    client = _FakeClient()
    _use_client(monkeypatch, client)
    assert _put(tmp_path, if_none_matched=True) is True
    assert client.put_args == {
        "Bucket": BUCKET,
        "Key": "prefix/archive",
        "IfNoneMatch": "*",
    }


@pytest.mark.parametrize("code", ["PreconditionFailed", "ConditionalRequestConflict"])
def test_put_lost_race_returns_false(tmp_path, monkeypatch, code):
    _use_client(monkeypatch, _FakeClient(code))
    assert _put(tmp_path, if_none_matched=True) is False


def test_put_unexpected_error(tmp_path, monkeypatch):
    _use_client(monkeypatch, _FakeClient("AccessDenied"))
    with pytest.raises(ClientError):
        _put(tmp_path)
    assert _put(tmp_path, no_strict=True) is False


def test_head_object(monkeypatch):
    _use_client(monkeypatch, _FakeClient())
    obj = S3.head_object(f"{BUCKET}/prefix/archive")
    assert obj.ContentLength == 3
    assert obj.LastModified == "2026-01-02 03:04:05"


@pytest.mark.parametrize("code", ["404", "NoSuchKey", "NotFound"])
def test_head_object_missing_is_none(monkeypatch, code):
    _use_client(monkeypatch, _FakeClient(code))
    assert S3.head_object(f"{BUCKET}/prefix/archive") is None


def test_head_object_other_error_raises(monkeypatch):
    _use_client(monkeypatch, _FakeClient("AccessDenied"))
    with pytest.raises(ClientError):
        S3.head_object(f"{BUCKET}/prefix/archive")


@pytest.fixture
def no_credentials(monkeypatch):
    monkeypatch.delenv("EXTERNAL_S3_ACCESS_KEY", raising=False)
    monkeypatch.delenv("EXTERNAL_S3_SECRET_KEY", raising=False)


def test_writes_need_credentials(tmp_path, no_credentials):
    with pytest.raises(AssertionError, match="EXTERNAL_S3_ACCESS_KEY"):
        _put(tmp_path)


def test_reads_of_a_public_bucket_need_no_credentials(no_credentials):
    client = S3._get_boto3_client(BUCKET, read=True)
    assert client.meta.config.signature_version is s3_module.UNSIGNED
    assert client.meta.endpoint_url == "https://s3.example.test"


def test_reads_of_a_private_bucket_need_credentials(monkeypatch, no_credentials):
    private = {BUCKET: {**EXTERNAL[BUCKET], "public_read": False}}
    monkeypatch.setattr(Settings, "S3_EXTERNAL_BUCKETS", private, raising=False)
    with pytest.raises(AssertionError, match="EXTERNAL_S3_ACCESS_KEY"):
        S3.head_object(f"{BUCKET}/prefix/archive")


def test_signed_client_uses_the_configured_endpoint(monkeypatch):
    monkeypatch.setenv("EXTERNAL_S3_ACCESS_KEY", "key")
    monkeypatch.setenv("EXTERNAL_S3_SECRET_KEY", "secret")
    client = S3._get_boto3_client(BUCKET)
    assert client.meta.endpoint_url == "https://s3.example.test"
    assert client.meta.region_name == "example-1"
    assert S3._get_boto3_client(BUCKET) is client
    assert S3._get_boto3_client(BUCKET, read=True) is client


def test_forbidden_is_a_miss_only_for_anonymous_reads(monkeypatch, no_credentials):
    _use_client(monkeypatch, _FakeClient("403"))
    assert S3.head_object(f"{BUCKET}/prefix/archive") is None
    monkeypatch.setenv("EXTERNAL_S3_ACCESS_KEY", "key")
    monkeypatch.setenv("EXTERNAL_S3_SECRET_KEY", "secret")
    with pytest.raises(ClientError):
        S3.head_object(f"{BUCKET}/prefix/archive")


def test_other_buckets_still_use_the_aws_cli(monkeypatch):
    calls = []
    monkeypatch.setattr(
        s3_module.Shell, "get_output", lambda cmd, **kw: calls.append(cmd) or ""
    )
    assert S3.head_object("aws-bucket/key") is None
    assert calls == ["aws s3api head-object --bucket aws-bucket --key key"]


def test_submodule_archive_path(monkeypatch):
    monkeypatch.setattr(Settings, "CACHE_S3_PATH", "bucket/cache")
    monkeypatch.setattr(Settings, "SUBMODULE_CACHE_S3_PATH", "")
    assert Cache.submodule_archive_s3_path("abc") == "bucket/cache/submodules/abc.tar.zst"
    monkeypatch.setattr(Settings, "SUBMODULE_CACHE_S3_PATH", f"{BUCKET}/cache")
    assert Cache.submodule_archive_s3_path("abc") == f"{BUCKET}/cache/submodules/abc.tar.zst"
