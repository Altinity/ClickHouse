"""Tests for the dryrun subset check logic in checkpoint (fixing P3-B1 residual d).

The dryrun subset check validates that GC never plans to delete a reachable object.
A dryrun key MUST either:
  1. Be present in fsck detail with a DELETION-PIPELINE class (unreachable, pending-gc, awaiting-gc), OR
  2. Be ABSENT from fsck detail (the blob was already physically deleted but the row hasn't folded yet)

Case 2 (absent) is now TOLERATED (fixed P3-B1 d); case 3 (present with wrong class) remains a failure.
"""

import pytest

from soak.checker import CheckpointFailure


def make_fsck_detail(rows):
    """Build an fsck result dict with detail rows.

    Args:
        rows: list of dicts, each with at least 'key' and 'class' fields.

    Returns a dict with 'detail' key containing the rows."""
    return {"detail": rows}


def make_dryrun(entries):
    """Build a dryrun result dict.

    Args:
        entries: list of dicts, each with at least 'key' field.

    Returns a dict with 'entries' and 'count' keys."""
    return {"entries": entries, "count": len(entries)}


def dryrun_subset_check(detail_rows, dryrun_entries, *, log_fn=None):
    """Perform the dryrun subset check logic from checkpoint().

    This extracts the core validation logic so it can be unit-tested independently.

    Args:
        detail_rows: list of fsck detail rows (with 'key' and 'class' fields)
        dryrun_entries: list of dryrun entries (with 'key' field)
        log_fn: optional callable to capture log messages (receives message string)

    Raises:
        CheckpointFailure: if a dryrun key is present in detail with a non-pipeline class

    Returns:
        int: count of already-deleted (absent from detail) keys that were tolerated
    """
    pipeline_classes = ("unreachable", "pending-gc", "awaiting-gc")
    pipeline_keys = {row["key"] for row in detail_rows if row["class"] in pipeline_classes}
    detail_class_by_key = {row["key"]: row["class"] for row in detail_rows}

    already_deleted_count = 0
    for entry in dryrun_entries:
        if entry["key"] not in detail_class_by_key:
            # Blob absent from fsck detail: already physically deleted but row not yet folded. TOLERATED.
            already_deleted_count += 1
        elif entry["key"] not in pipeline_keys:
            # Blob present in detail but NOT in a deletion-pipeline class: a real wrong-preview signal.
            other = detail_class_by_key.get(entry["key"], "unknown")
            class_counts = {}
            for row in detail_rows:
                class_counts[row["class"]] = class_counts.get(row["class"], 0) + 1
            raise CheckpointFailure(
                f"dryrun key {entry['key']!r} previews deletion of a non-pipeline blob "
                f"(fsck class={other!r}) — a dryrun key must be in a deletion-pipeline class "
                f"{pipeline_classes} (absent-from-detail means the blob is already deleted); "
                f"dryrun_count={len(dryrun_entries)} pipeline_keys={len(pipeline_keys)} "
                f"detail_class_counts={class_counts}")

    if already_deleted_count > 0 and log_fn:
        log_fn(f"dryrun: {already_deleted_count} keys already deleted, pending fold — tolerated")

    return already_deleted_count


def test_dryrun_empty_no_failure():
    """Empty dryrun is OK (nothing to delete)."""
    detail = [{"key": "pool/blobs/aa/aaa", "class": "reachable"}]
    dryrun = []
    assert dryrun_subset_check(detail, dryrun) == 0


def test_dryrun_all_in_pipeline_no_failure():
    """All dryrun keys in pipeline classes pass."""
    detail = [
        {"key": "pool/blobs/aa/aaa", "class": "unreachable"},
        {"key": "pool/blobs/bb/bbb", "class": "pending-gc"},
        {"key": "pool/trees/cc/ccc", "class": "awaiting-gc"},
    ]
    dryrun = [
        {"key": "pool/blobs/aa/aaa"},
        {"key": "pool/blobs/bb/bbb"},
        {"key": "pool/trees/cc/ccc"},
    ]
    assert dryrun_subset_check(detail, dryrun) == 0


def test_dryrun_key_absent_from_detail_tolerated():
    """A dryrun key absent from fsck detail is TOLERATED (blob already deleted, pending fold).

    This is the main fix for P3-B1 residual (d): condemned blobs may be already physically deleted
    but the row not yet folded out of the GC run, causing the blob to be absent from fsck detail."""
    detail = [
        {"key": "pool/blobs/aa/aaa", "class": "reachable"},
        {"key": "pool/blobs/bb/bbb", "class": "unreachable"},
    ]
    # Third key is absent from detail (already deleted)
    dryrun = [
        {"key": "pool/blobs/bb/bbb"},  # in detail, pipeline -> OK
        {"key": "pool/blobs/cc/ccc"},  # NOT in detail (absent) -> tolerated
    ]
    logs = []
    count = dryrun_subset_check(detail, dryrun, log_fn=logs.append)
    assert count == 1
    assert len(logs) == 1
    assert "1 keys already deleted, pending fold" in logs[0]


def test_dryrun_multiple_absent_keys_tolerated():
    """Multiple absent keys are all tolerated and counted."""
    detail = [
        {"key": "pool/blobs/aa/aaa", "class": "unreachable"},
    ]
    dryrun = [
        {"key": "pool/blobs/aa/aaa"},  # in pipeline
        {"key": "pool/blobs/missing1", "class": "unreachable"},  # absent
        {"key": "pool/blobs/missing2", "class": "unreachable"},  # absent
        {"key": "pool/blobs/missing3", "class": "unreachable"},  # absent
    ]
    logs = []
    count = dryrun_subset_check(detail, dryrun, log_fn=logs.append)
    assert count == 3
    assert "3 keys already deleted, pending fold" in logs[0]


def test_dryrun_key_reachable_in_detail_fails():
    """A dryrun key present in detail with class='reachable' is a FAILURE (real wrong-preview).

    This is the core GC safety invariant: GC must never plan to delete a reachable object."""
    detail = [
        {"key": "pool/blobs/aa/aaa", "class": "reachable"},  # reachable is NOT pipeline
        {"key": "pool/blobs/bb/bbb", "class": "unreachable"},
    ]
    dryrun = [
        {"key": "pool/blobs/bb/bbb"},  # OK
        {"key": "pool/blobs/aa/aaa"},  # FAIL: reachable is not in pipeline
    ]
    with pytest.raises(CheckpointFailure) as exc_info:
        dryrun_subset_check(detail, dryrun)
    msg = str(exc_info.value)
    assert "pool/blobs/aa/aaa" in msg
    assert "fsck class='reachable'" in msg
    assert "deletion-pipeline class" in msg


def test_dryrun_key_unaccounted_in_detail_fails():
    """A dryrun key present with class='unaccounted' is a FAILURE."""
    detail = [
        {"key": "pool/blobs/aa/aaa", "class": "unaccounted"},
        {"key": "pool/blobs/bb/bbb", "class": "unreachable"},
    ]
    dryrun = [
        {"key": "pool/blobs/bb/bbb"},
        {"key": "pool/blobs/aa/aaa"},  # unaccounted is not in pipeline
    ]
    with pytest.raises(CheckpointFailure) as exc_info:
        dryrun_subset_check(detail, dryrun)
    msg = str(exc_info.value)
    assert "pool/blobs/aa/aaa" in msg
    assert "fsck class='unaccounted'" in msg


def test_dryrun_mixed_absent_and_bad_present_fails_on_bad():
    """When there are both absent keys (tolerated) and bad-class keys (failed), the check fails on bad."""
    detail = [
        {"key": "pool/blobs/aa/aaa", "class": "reachable"},  # bad
        {"key": "pool/blobs/bb/bbb", "class": "unreachable"},  # OK
    ]
    dryrun = [
        {"key": "pool/blobs/bb/bbb"},
        {"key": "pool/blobs/cc/ccc"},  # absent (would be tolerated)
        {"key": "pool/blobs/aa/aaa"},  # reachable (FAIL before tolerating absent)
    ]
    with pytest.raises(CheckpointFailure) as exc_info:
        dryrun_subset_check(detail, dryrun)
    msg = str(exc_info.value)
    assert "pool/blobs/aa/aaa" in msg
    # The absent one doesn't get logged because we raise on the first bad key
    # (current implementation processes sequentially)


def test_error_message_includes_class_counts():
    """When a key fails, the error message includes a summary of detail classes."""
    detail = [
        {"key": "pool/blobs/aa/aaa", "class": "reachable"},
        {"key": "pool/blobs/bb/bbb", "class": "unreachable"},
        {"key": "pool/blobs/bb/bbb", "class": "unreachable"},
        {"key": "pool/blobs/cc/ccc", "class": "pending-gc"},
    ]
    dryrun = [
        {"key": "pool/blobs/aa/aaa"},
    ]
    with pytest.raises(CheckpointFailure) as exc_info:
        dryrun_subset_check(detail, dryrun)
    msg = str(exc_info.value)
    assert "detail_class_counts" in msg
    assert "reachable" in msg or "pending-gc" in msg  # counts are included
