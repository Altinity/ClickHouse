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
