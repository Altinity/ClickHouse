"""`assert_no_leftovers` is NARROWED for Stage A, not disabled — these pin exactly how far.

Stage A suppresses every destructive site (`UniversePolicy::kDefault = StageA_Suppressed`), and
manifest bodies are deleted at such a site without being condemned first, so they can only accumulate
as `unreachable`. Before this narrowing that read as a leak and failed every card that drops a table.

The risk of the narrowing is the opposite defect: excusing a real leak because it happens to land on
the manifest prefix. So these tests assert the boundary from BOTH sides — the permitted family passes
AND is reported, and everything adjacent to it still fails.
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


def test_the_permitted_family_passes_and_is_counted():
    v = _verdict(_manifests(20))
    assert v.status == "pass"
    # Counted and reported, never silent: the number and the class must both be in the verdict.
    assert "20" in str(v.observed)
    assert "unreachable:_manifests" in str(v.observed)
    assert "STAGE-A" in v.note and "7b" in v.note


def test_a_blob_leak_still_fails():
    """The class this assertion was written for, and the one that catches GC-CONCURRENT-LEADER-LEAK."""
    v = _verdict(_blobs(3))
    assert v.status == "fail"
    assert "unreachable:blobs" in str(v.observed)


def test_a_blob_leak_is_not_excused_by_accompanying_manifests():
    """The narrowing must not become a hiding place: a real leak alongside the permitted family still
    fails, and the message names the hard part rather than burying it in a total."""
    v = _verdict(_manifests(20) + _blobs(3))
    assert v.status == "fail"
    assert "unreachable:blobs" in str(v.observed)


def test_dangling_fails_even_on_the_permitted_prefix():
    """`dangling` is a referenced object that is MISSING — data loss, never retention."""
    v = _verdict(_manifests(2, cls="dangling"))
    assert v.status == "fail"


def test_dangling_in_the_summary_fails_independently_of_the_classifier():
    """Checked separately so a future change to the buckets cannot quietly stop checking it."""
    v = _verdict(_manifests(20), dangling=2)
    assert v.status == "fail"
    assert "dangling=2" in str(v.observed)


def test_unaccounted_manifests_are_not_the_permitted_family():
    """Only `unreachable` is the gated-delete shape; `unaccounted` means outside GC's view entirely."""
    v = _verdict(_manifests(4, cls="unaccounted"))
    assert v.status == "fail"


def test_a_clean_pool_still_passes_plainly():
    r = _result()
    v = assert_no_leftovers(r, {"unreachable": 0, "dangling": 0}, residual_after_gc=0)[0]
    assert v.status == "pass"
