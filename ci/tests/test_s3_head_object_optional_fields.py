"""
S3.head_object must tolerate HeadObject JSON that omits optional AWS fields.

aws s3api head-object only emits Expiration when a lifecycle Expire rule
applies. Unpacking the raw JSON into S3.Object then raises TypeError and
Config Workflow leaves submodule_cache_hash empty.

Caused by: https://github.com/Altinity/ClickHouse/actions/runs/33642238083
"""

import json
import os
import sys

sys.path.insert(0, os.path.join(os.path.dirname(__file__), "../.."))

from ci.praktika import s3 as s3_module
from ci.praktika.s3 import S3

# Shape of a successful HeadObject on an untagged cache archive: no Expiration,
# plus keys Object does not declare.
HEAD_WITHOUT_EXPIRATION = {
    "AcceptRanges": "bytes",
    "LastModified": "Wed, 02 Sep 2026 14:33:49 GMT",
    "ContentLength": 123,
    "ETag": '"abc"',
    "ContentType": "application/zstd",
    "Metadata": {},
    "StorageClass": "STANDARD",
}


def test_head_object_without_expiration_is_a_hit(monkeypatch):
    monkeypatch.setattr(
        s3_module.Shell,
        "get_output",
        lambda *a, **kw: json.dumps(HEAD_WITHOUT_EXPIRATION),
    )
    obj = S3.head_object("altinity-build-artifacts/ci_ch_cache/submodules/hash.tar.zst")
    assert obj is not None
    assert obj.ContentLength == 123
    assert obj.Expiration == ""
