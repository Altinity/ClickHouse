"""`stale_edge` is a HARD checkpoint assert, and its whole point is that it fails closed.

These tests pin the three ways it must NOT pass: a missing key (a binary that predates the class), a
positive count, and a summary-mode zero that is structural rather than evidence. Each of them was a
plausible way to write the check such that it could never go red.
"""

from soak.fsck import parse_fsck_summary, stale_edge_verdict


SUMMARY_LINE = (
    "reachable=120 dangling=0 unreachable=7 pending_gc=3 awaiting_gc=4 unaccounted=0 "
    "stale_edge=0 physical_bytes=4096 "
    "referenced_logical_bytes=8192 distinct_blobs=120 total_blob_refs=456 dedup_ratio=3.8"
)


def test_summary_line_carries_stale_edge():
    """The parser must surface the field at all — the assert reads it off this dict."""
    parsed = parse_fsck_summary(SUMMARY_LINE)
    assert parsed["stale_edge"] == 0
    assert parsed["unreachable"] == 7


def test_missing_key_fails_closed():
    """A binary that predates the StaleEdge class prints no `stale_edge=`. That is the ABSENCE of an
    answer, not a zero, and reading it as zero is how a check comes to pass while looking at nothing."""
    verdict, why = stale_edge_verdict({"reachable": 1, "unreachable": 0, "dangling": 0}, detail=True)
    assert verdict == "absent"
    assert "predates" in why


def test_missing_key_fails_closed_even_on_a_clean_summary():
    """Absence beats every other consideration: a clean `unreachable == 0` must NOT rescue a result
    that never reported the field, because we cannot know the scan looked for it."""
    verdict, _ = stale_edge_verdict({"unreachable": 0, "dangling": 0}, detail=False)
    assert verdict == "absent"


def test_nonzero_on_a_detail_scan_is_a_hard_finding():
    parsed = parse_fsck_summary(SUMMARY_LINE.replace("stale_edge=0", "stale_edge=5"))
    verdict, why = stale_edge_verdict(parsed, detail=True)
    assert verdict == "found"
    assert "5" in why


def test_zero_on_a_detail_scan_is_clean():
    verdict, _ = stale_edge_verdict(parse_fsck_summary(SUMMARY_LINE), detail=True)
    assert verdict == "clean"


def test_summary_zero_with_unreferenced_blobs_is_unchecked_not_clean():
    """The cross-check is `--detail`-gated in `runFsck`, so a summary line's `stale_edge=0` is
    structural. Reporting it as clean would be a green that cannot go red."""
    verdict, why = stale_edge_verdict(parse_fsck_summary(SUMMARY_LINE), detail=False)
    assert verdict == "unchecked"
    assert "structural" in why


def test_summary_zero_with_no_unreferenced_blobs_is_genuinely_clean():
    """`runFsck` increments `unreachable` for EVERY present-but-unreferenced blob before classifying
    it, and StaleEdge is one of those classifications — so `unreachable == 0` implies
    `stale_edge == 0` without any cross-check. This is the case that keeps a clean checkpoint cheap."""
    line = SUMMARY_LINE.replace("unreachable=7", "unreachable=0")
    verdict, _ = stale_edge_verdict(parse_fsck_summary(line), detail=False)
    assert verdict == "clean"
