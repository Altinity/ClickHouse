"""`assert_no_leftovers` tolerates NO leak class — these pin the boundary from both sides.

After forced GC a reclaimable orphan of any class is a leak. The classifier still separates the
residual into leak / pipeline / bookkeeping, and the risk in that split is the opposite defect from a
missing check: excusing a real leak by filing it under a benign class. So these assert both that a
genuine leak fails whatever prefix it lands on, and that the benign classes still pass plainly.
"""

from scenarios.framework.assertions import assert_no_leftovers
from scenarios.framework.report import ScenarioResult


def _result():
    return ScenarioResult("X", "x", "P0", 1)


def _verdict(detail, dangling=0):
    r = _result()
    v = assert_no_leftovers(
        r,
        {"unreachable": len(detail), "dangling": dangling},
        residual_after_gc=(len(detail) or None),
        fsck_detail_res={"detail": detail},
    )[0]
    return v


def _manifests(n, cls="unreachable"):
    return [{"class": cls, "key": "p/cas/manifests/ns/%d" % i} for i in range(n)]


def _blobs(n, cls="unreachable"):
    return [{"class": cls, "key": "p/blobs/aa/%d" % i} for i in range(n)]


def test_a_manifest_leak_fails_and_is_counted():
    """Manifest bodies are deleted at a gated site; once the gate opens they must drain like anything
    else, so a surviving one is a leak and the count and class both reach the verdict."""
    v = _verdict(_manifests(20))
    assert v.status == "fail"
    assert "20" in str(v.observed)
    assert "unreachable:_manifests" in str(v.observed)


def test_a_blob_leak_still_fails():
    """The class this assertion was written for, and the one that catches GC-CONCURRENT-LEADER-LEAK."""
    v = _verdict(_blobs(3))
    assert v.status == "fail"
    assert "unreachable:blobs" in str(v.observed)


def test_a_mixed_leak_names_every_class_rather_than_a_total():
    """A failing verdict must say WHICH prefixes leaked, or triage starts from a bare number."""
    v = _verdict(_manifests(20) + _blobs(3))
    assert v.status == "fail"
    assert "unreachable:blobs" in str(v.observed)
    assert "unreachable:_manifests" in str(v.observed)


def test_dangling_fails_on_the_manifest_prefix_too():
    """`dangling` is a referenced object that is MISSING — data loss, never retention."""
    v = _verdict(_manifests(2, cls="dangling"))
    assert v.status == "fail"


def test_dangling_in_the_summary_fails_independently_of_the_classifier():
    """Checked separately so a future change to the buckets cannot quietly stop checking it."""
    v = _verdict(_manifests(20), dangling=2)
    assert v.status == "fail"
    assert "dangling=2" in str(v.observed)


def test_unaccounted_manifests_fail_as_well():
    """`unaccounted` means outside GC's view entirely, which no reclamation posture explains."""
    v = _verdict(_manifests(4, cls="unaccounted"))
    assert v.status == "fail"


def test_a_clean_pool_still_passes_plainly():
    r = _result()
    v = assert_no_leftovers(r, {"unreachable": 0, "dangling": 0}, residual_after_gc=0)[0]
    assert v.status == "pass"
