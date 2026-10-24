"""Unit tests for the shared pool-key classifier (per-server-tree layout, 2026-07 relocation)."""

from scenarios.framework.observe import classify_pool_path


def test_blobs_relative_and_prefixed():
    assert classify_pool_path("blobs/ce/ce6dfecc05b818feadd26bcab4a4b4b7") == "blobs"
    assert classify_pool_path("soak_pool/blobs/ce/ce6dfecc05b818feadd26bcab4a4b4b7") == "blobs"
    assert classify_pool_path("./blobs/ce/xhash") == "blobs"


def test_manifests_under_cas_tree():
    # The leak-masking regression: manifests live under cas/manifests/<srid>/... now.
    key = "cas/manifests/ca_soak_ch2/store/aff/aff823b3-cd6a-4444-9999-000000000001/3/3653/000001.proto"
    assert classify_pool_path(key) == "_manifests"
    assert classify_pool_path("soak_pool/" + key) == "_manifests"


def test_refs_under_cas_tree():
    assert classify_pool_path("cas/refs/ca_soak_ch1/7") == "refs"
    assert classify_pool_path("soak_pool/cas/refs/ca_soak_ch1/12") == "refs"


def test_gc_and_server_roots_not_confused_with_roots():
    # 'gc/server-roots/...' must classify as gc, not roots ('server-roots' is one segment).
    assert classify_pool_path("soak_pool/gc/server-roots/ca_soak_ch1/mount") == "gc"
    assert classify_pool_path("gc/state") == "gc"


def test_roots_tree():
    assert classify_pool_path("roots/ca_soak_ch1/store/uuid@cas@/3") == "roots"
    assert classify_pool_path("soak_pool/roots/ca_soak_ch1/_watermark") == "roots"


def test_files_segment_wins():
    assert classify_pool_path("roots/ns/store/uuid@cas@/_files/data.bin") == "_files"


def test_pool_meta_and_other():
    assert classify_pool_path("_pool_meta") == "_pool_meta"
    assert classify_pool_path("soak_pool/_pool_meta") == "_pool_meta"
    assert classify_pool_path("something/unknown") == "other"


def test_cas_segment_without_manifests_or_refs_is_not_anchored():
    # A stray 'cas' path segment with an unknown child must not classify as manifests/refs.
    assert classify_pool_path("cas/unknown/zzz") == "other"


def test_unreachable_manifest_is_reclaimable_not_bookkeeping():
    """Regression: an unreachable part-manifest must land in the '_manifests' (RECLAIMABLE) bucket."""
    from scenarios.framework.assertions import classify_unreachable

    detail = {"detail": [
        {"class": "unreachable", "key": "soak_pool/cas/manifests/ca_soak_ch1/store/aa/uuid/1/2/000001.proto"},
        {"class": "unreachable", "key": "soak_pool/blobs/ab/abcdef0123"},
        {"class": "unreachable", "key": "soak_pool/gc/gen/5/attempt/2/run"},
        {"class": "reachable",   "key": "soak_pool/blobs/cd/cdef"},
    ]}
    buckets = classify_unreachable(detail)
    assert buckets == {"_manifests": 1, "blobs": 1, "gc": 1}


def test_identity_from_key_post_relocation():
    from scenarios.framework.observe import _identity_from_key

    blob = _identity_from_key("soak_pool/blobs/ab/abcdef0123456789")
    assert blob["object_hash"] == "abcdef0123456789"
    man = _identity_from_key(
        "soak_pool/cas/manifests/ca_soak_ch1/store/aa/uuid-1/3/12/000001.proto")
    assert man["object_hash"] is None          # ordinal stem is NOT an id — must not be misused
    assert man["token"] is None
    assert man["namespace_hint"] == "ca_soak_ch1"


def test_s3_error_rate_computation():
    from scenarios.framework.observe import _rates_from_counters

    r = _rates_from_counters({"S3ReadRequestsErrors": 19, "S3ReadRequestsCount": 100,
                              "S3WriteRequestsErrors": 0, "S3WriteRequestsCount": 50})
    assert r["read_error_rate"] == 0.19
    assert r["write_error_rate"] == 0.0
    # Missing counters yield None, never 0 (a gap must be visible, not faked).
    r2 = _rates_from_counters({})
    assert r2["read_error_rate"] is None and r2["write_error_rate"] is None
