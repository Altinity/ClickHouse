"""
A Ceph CompleteMultipartUpload retry returns 500 "already in progress" after the
object has been replaced. copy_file_to_s3 accepts that when HeadObject shows a
LastModified within five minutes of now.
"""

import os
import sys
from datetime import datetime, timedelta, timezone

sys.path.insert(0, os.path.join(os.path.dirname(__file__), "../.."))

from ci.praktika import s3 as s3_module
from ci.praktika.s3 import S3


class S3UploadFailedError(Exception):
    pass


ALREADY_IN_PROGRESS = (
    "Failed to upload archive to cache-bucket/key: An error occurred (InternalError) "
    "when calling the CompleteMultipartUpload operation (reached max retries: 4): "
    "This multipart completion is already in progress"
)
S3_PATH = "cache-bucket/ci_ch_cache/thinlto/antalya-26.8/arm_release-g0.tar.zst"


def _mtime(delta):
    return (datetime.now(timezone.utc) + delta).isoformat(sep=" ")


def _obj(mtime):
    return S3.Object(
        AcceptRanges="bytes",
        Expiration="",
        LastModified=mtime,
        ContentLength=1,
        ETag='"abc"',
        ContentType="application/zstd",
        ServerSideEncryption="",
        Metadata={},
    )


class _Client:
    def __init__(self, error):
        self.error = error

    def upload_file(self, *args, **kwargs):
        raise self.error


def _patch(monkeypatch, head, error, external=True):
    config = {"endpoint_url": "https://hel1.your-objectstorage.com"} if external else None
    monkeypatch.setattr(s3_module.time, "sleep", lambda seconds: None)
    monkeypatch.setattr(S3, "_external_bucket_config", classmethod(lambda cls, bucket: config))
    monkeypatch.setattr(S3, "_http_endpoint", classmethod(lambda cls, bucket: "cache-bucket.hel1.your-objectstorage.com"))
    monkeypatch.setattr(S3, "_get_boto3_client", classmethod(lambda cls, bucket=None, read=False: _Client(error)))
    monkeypatch.setattr(S3, "head_object", classmethod(lambda cls, path: head(path)))
    monkeypatch.setattr(s3_module.StorageUsage, "add_uploaded", lambda path: None)


def _archive(tmp_path):
    archive = tmp_path / "arm_release-g0.tar.zst"
    archive.write_bytes(b"cache")
    return archive


def test_in_progress_completion_accepts_a_fresh_object(monkeypatch, tmp_path):
    seen = []

    def head(path):
        seen.append(path)
        return _obj(_mtime(timedelta(seconds=-30)))

    _patch(monkeypatch, head, S3UploadFailedError(ALREADY_IN_PROGRESS))
    link = S3.copy_file_to_s3(
        s3_path=S3_PATH, local_path=str(_archive(tmp_path)), with_rename=True
    )
    assert "cache-bucket.hel1.your-objectstorage.com" in link
    assert seen == [S3_PATH]


def test_in_progress_completion_rejects_a_stale_object(monkeypatch, tmp_path):
    def head(path):
        return _obj(_mtime(timedelta(minutes=-10)))

    _patch(monkeypatch, head, S3UploadFailedError(ALREADY_IN_PROGRESS))
    try:
        S3.copy_file_to_s3(
            s3_path=S3_PATH, local_path=str(_archive(tmp_path)), with_rename=True
        )
    except S3UploadFailedError as e:
        assert "already in progress" in str(e)
    else:
        raise AssertionError("stale cache object was accepted")


def test_other_upload_errors_still_raise(monkeypatch, tmp_path):
    calls = {"n": 0}

    def head(path):
        calls["n"] += 1
        return _obj(_mtime(timedelta(seconds=-30)))

    _patch(monkeypatch, head, RuntimeError("access denied"))
    try:
        S3.copy_file_to_s3(
            s3_path=S3_PATH, local_path=str(_archive(tmp_path)), with_rename=True
        )
    except RuntimeError as e:
        assert str(e) == "access denied"
    else:
        raise AssertionError("upload error was swallowed")
    assert calls["n"] == 0


def test_in_progress_completion_on_aws_still_raises(monkeypatch, tmp_path):
    calls = {"n": 0}

    def head(path):
        calls["n"] += 1
        return _obj(_mtime(timedelta(seconds=-30)))

    _patch(monkeypatch, head, S3UploadFailedError(ALREADY_IN_PROGRESS), external=False)
    try:
        S3.copy_file_to_s3(
            s3_path=S3_PATH, local_path=str(_archive(tmp_path)), with_rename=True
        )
    except S3UploadFailedError as e:
        assert "already in progress" in str(e)
    else:
        raise AssertionError("AWS upload error was accepted")
    assert calls["n"] == 0
