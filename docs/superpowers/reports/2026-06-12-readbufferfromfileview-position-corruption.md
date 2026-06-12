# `ReadBufferFromFileView` reports a corrupted position after the inner buffer discards data on `setReadUntilPosition` (latent in `PackedFilesReader`)

> Bug report, 2026-06-12. Fixed on `cas-mergetree-poc` in commit `440871098a9`; written up
> here in upstream-neutral terms (no content-addressed-storage context required) because both
> `ReadBufferFromFileView` and `PackedFilesReader` are upstream components, suitable for an
> upstream issue/PR. The content-addressed read path is where the bug was actually observed as
> wrong query results; see backlog item B115 in
> [`../deferred_backlog/cas-mergetree-integration.md`](../deferred_backlog/cas-mergetree-integration.md).

## Summary

`ReadBufferFromFileView` exposes a byte sub-range `[left_bound, right_bound)` of an inner
`ReadBufferFromFileBase` as if it were a standalone file. It maintained `file_offset_of_buffer_end`
(the absolute inner-file offset of `working_buffer.end()`) **incrementally**, assuming the inner
buffer only mutates its working buffer in direct response to the view's own `next`/`seek`. That
assumption is violated by inner buffers that **discard their working buffer as a side effect of
`setReadUntilPosition`** — notably `ReadBufferFromS3`. The result is a `getPosition` that silently
jumps forward by the discarded byte count, which can cause a downstream seekable consumer to read
the wrong data.

## Impact

Wrong results (not an exception, not a checksum failure): a consumer that seeks or sets a
read-until bound mid-read, over a packed/viewed file on a buffer-discarding remote backend, can be
served a stale block. At the MergeTree level this manifests as duplicated and missing granules —
every individual block decompresses cleanly, but the wrong block is returned.

## Trigger conditions (all required)

1. A `ReadBufferFromFileView` whose inner buffer **discards its working buffer on
   `setReadUntilPosition`**. `ReadBufferFromS3` does: it rebases `offset` to the consumer
   position, calls `resetWorkingBuffer`, and drops `impl`. Local file buffers no-op
   `setReadUntilPosition`, so they are unaffected.
2. A consumer that calls `setReadUntilPosition` / `setReadUntilEnd` (or relies on the view's
   position across a range change) **while data is still buffered** — e.g. a
   `CompressedReadBufferFromFile` driven by a seeking, mark-range-narrowing reader.

## Why it is currently latent in `PackedFilesReader`

`PackedFilesReader` is the only in-tree user of `ReadBufferFromFileView` besides the experimental
content-addressed read path. Its sole consumer today is **MergeTree column statistics**
(`statistics.packed`, see `IMergeTreeDataPart::loadStatisticsPacked`), which reads each packed
sub-file through `CompressedReadBuffer` — the **sequential, non-seekable** variant. It reads each
sub-file straight through with no mid-read `seek` and no `setReadUntilPosition`, so condition (2)
never holds and the inner buffer is never asked to discard data underneath the view. The bug is
therefore present but **not reachable through the current statistics consumer**.

It becomes a live wrong-results bug for any future consumer that does **ranged / seeking reads of
packed sub-files on a remote disk** (e.g. switching statistics reads to
`CompressedReadBufferFromFile`, or packing seekable column data).

## Root cause

- `nextImpl` advanced the offset with `file_offset_of_buffer_end += available()`. After a prior
  `resizeWorkingBuffer` truncation (right bound narrowed then re-extended), this incremental value
  drifts from the inner buffer's real offset.
- `setReadUntilPosition` / `setReadUntilEnd` forwarded the call to the inner buffer and ran
  `resizeWorkingBuffer` but **never reconciled `file_offset_of_buffer_end`**. When the inner buffer
  discarded its data and the right bound was narrowed, `resizeWorkingBuffer` clamped the now-stale
  `file_offset_of_buffer_end` to the new bound, so
  `getPosition() = (file_offset_of_buffer_end - left_bound) - (working_buffer.end() - pos)`
  returned a position teleported forward by the discarded bytes.

A seekable consumer (`CompressedReadBufferFromFile::seek`) then trusts that position; its "already
at required position" / "seek within working buffer" fast-paths make the wrong decision and
re-serve a stale decompressed block.

## Fix

After **every** operation on the inner buffer, rebase `file_offset_of_buffer_end` from the inner
buffer's own post-op accounting — `impl->getPosition() + impl->available()`, computed inside the
buffer-swap window — instead of incrementing it. This makes the invariant *view buffer-end ==
inner buffer-end* hold by construction across `nextImpl`, `seek`, `setReadUntilPosition`, and
`setReadUntilEnd`, and incidentally fixes the truncate-then-extend drift in `nextImpl`.

## Test coverage

The class had **no tests** prior to this. A parameterized gtest battery
(`src/IO/tests/gtest_read_buffer_from_file_view.cpp`) exercises file-like (no-op
`setReadUntilPosition`) vs remote-like (discard-on-`setReadUntilPosition`, `ReadBufferFromS3`
semantics) inner buffers across several chunk sizes, including the mid-buffer right-mark adjustment
pattern and a randomized sequence checked against a golden model. 14 of 36 cases fail on the
unfixed code; all 36 pass with the fix.

## Provenance

Both `ReadBufferFromFileView` and `PackedFilesReader` were introduced by the "add packed format for
statistics" change (`283c9778d9e`). The defect has existed since that introduction.
