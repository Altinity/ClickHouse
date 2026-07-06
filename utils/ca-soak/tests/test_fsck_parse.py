from soak.fsck import parse_fsck_summary, parse_dryrun


def test_parse_fsck_summary():
    line = ("reachable=18432 dangling=0 unreachable=211 physical_bytes=5500000000 "
            "referenced_logical_bytes=8200000000 distinct_blobs=12000 total_blob_refs=18000 "
            "dedup_ratio=1.5")
    r = parse_fsck_summary(line)
    assert r["dangling"] == 0 and r["unreachable"] == 211 and r["reachable"] == 18432
    assert r["distinct_blobs"] == 12000


def test_parse_fsck_summary_partial():
    line = ("reachable=100 dangling=0 unreachable=0 pending_gc=0 awaiting_gc=0 unaccounted=0 "
            "physical_bytes=0 referenced_logical_bytes=0 distinct_blobs=0 total_blob_refs=0 "
            "dedup_ratio=0 partial=1 reason='fsck: exceeded the deadline during "
            "'walking refs' — run against a QUIESCED pool or raise --timeout.'")
    r = parse_fsck_summary(line)
    assert r["partial"] == 1
    assert r["reachable"] == 100
    assert "exceeded the deadline" in r["reason"]


def test_parse_dryrun():
    out = "preview_deletes=2\nunreachable\tpool/blobs/ab/abcd\t100\nunreachable\tpool/trees/cd/cdef\t40\n"
    r = parse_dryrun(out)
    assert r["count"] == 2
    assert {e["key"] for e in r["entries"]} == {"pool/blobs/ab/abcd", "pool/trees/cd/cdef"}
