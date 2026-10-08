# LIST requests per logical page (GC global ref walk), RustFS

Runs: 20k = 20,000 `_log` keys (reference figure, one run, 328 s); 7k = 7,000 keys (the committed test; re-run on the final tree ac48289f453: DiskS3ListObjects 28, CASRefGlobalListPages 8, ratio 3.50, 41 s).
Raw output: `build/list_pages_attribution_20k.md`, `build/list_pages_attribution_7k.md`.
M = measured (counter or trace line), I = inferred.

## Ratios (M)

| run | DiskS3ListObjects | CASRefGlobalListPages | round ratio | ref stream only (trace) |
|---|---|---|---|---|
| 20k | 67 | 21 | 3.19 | (3 + 57) / 21 = 2.86 |
| 7k | 28 | 8 | 3.50 | (3 + 18) / 8 = 2.63 |

The trace sum equals the round delta in both runs (20k: 67 = 67; 7k: 28 = 28), so no LIST is unaccounted for.

## Per-call cost (M, 20k trace)

| call | calls | requests each |
|---|---|---|
| first ref-stream page (`limit + 1`) | 1 | 3 |
| resumed full page | 18 | 3 |
| resumed full page whose successor batch is the last | 1 | 2 |
| last resumed page (72 keys) | 1 | 1 |
| `cas/manifests`, `cas/ns` (first page, other callers) | 2 | 3 each |
| `gc/server-roots` | 1 | 1 |

## Mechanism (I, from the code)

- A full page costs A + B + C:
  - A: the batch holding the page's keys.
  - B: the next batch, read for the sentinel key (the store's page is 1000 = `limit`, so key 1001 is on the next batch).
  - C: the prefetch scheduled when B arrives with `has_next`. `nextBatch` schedules the next batch unconditionally (`ObjectStorageIteratorAsync.cpp:71-73`); `~S3IteratorAsync` waits for it (`S3ObjectStorage.cpp:201-206`, `ObjectStorageIteratorAsync.cpp:30-33`). `DiskS3ListObjects` is incremented when the job starts (`S3ObjectStorage.cpp:213`).
- The page before the last costs 2: B is the last batch (`has_next` false), so there is no C.
- The last page costs 1: A is the last batch.
- The first page costs 3, not 2 (the earlier report read 2 because the prefetch had not started when the trace was written). With `max-keys` 1001 a store that honoured it would answer in A and cost 2, so RustFS returns 1000 keys per request (I; the counters cannot show the key count of a batch).

## Levers (I: arithmetic from the mechanism, not measured; 20k: 21 pages, ref stream 60 requests)

| lever | saves per full page | ref stream | dominant? |
|---|---|---|---|
| Do not schedule the next batch when the consumer stops (removes C) | 1 | 60 -> about 40 (2.0) | yes: store-independent, no change to the page contract |
| Sentinel on the same request (removes B) | 1 | 60 -> about 40 (2.0) | no: needs `max-keys` = `limit + 1`, and S3 caps `max-keys` at 1000 (I), so at `limit` 1000 it saves nothing on real S3 unless the page size drops to 999 |
| Both | 2 | 60 -> about 21 (1.0) | |

The earlier "about 22 requests for 21 pages" from bounding resumed pages to `limit + 1` was wrong: the first page is already bounded and costs 3, because C is scheduled regardless of the bound.

## Retries

- M: 21 `ns/stream` calls equal `CASRefGlobalListPages` = 21. A CAS-level retry of a page would give more calls than pages. Cursors are not in the trace, so their distinctness is not checked.
- M: trace sum = round delta, so every counted LIST was issued by a `listUnder` iterator.
- Not observable: retries inside the AWS SDK. `DiskS3ListObjects` counts iterator jobs, not HTTP attempts.

## otel.demo

This run cannot explain otel.demo's 9.3 requests per page. RustFS shows 2.86 with the ref stream alone. A run on that stand with the `listUnder` trace enabled would show per-call `s3_list_requests` and `keys`, which separates paging cost from other callers and from SDK retries (compare against the S3 server's request log).

## Per-phase counters

`system.cas_gc_log` phase rows read 0 for `S3ListObjects` and `DiskS3ListObjects`, so per-phase attribution is not available (M).
