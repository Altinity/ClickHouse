---
description: 'Raw captured evidence that a CAS ref-prefix enumeration omitted two durable objects — the LIST-as-journal release blocker, observed and measured on 2026-07-26'
sidebar_label: 'LIST incompleteness proof (2026-07-26)'
sidebar_position: 99
slug: /superpowers/reports/list-incompleteness-proof-2026-07-26
title: 'Captured proof: a ref-prefix enumeration omitted two durable objects'
doc_type: 'reference'
---

# Captured proof: a ref-prefix enumeration omitted two durable objects {#captured-proof}

The raw rows behind BACKLOG `{#probe-a-proven-by-measurement}`, saved OFF the stand on purpose. The
previous live reproduction of a CAS defect was destroyed by our own smoke runs
(`{#leak-repro-lost}`), and this one sits in a soak cluster that will be torn down.

## What the files are {#files}

| file | what it holds |
|---|---|
| `gc_anomaly_rows.txt` | The two `gc_anomaly` audit rows: hole ids, direction, HEAD verdicts, both enumerations' maxima |
| `blob_storage_log_three_keys.txt` | `system.blob_storage_log` writes and deletes for the two holes and the witness key |
| `append_ordering_measured.txt` | Ref-log upload ordering across 65,263 writes, partitioned by writer epoch |

## The argument in four lines {#argument}

1. Probe A reported two holes at `16:47:38`, both `missing from the pre-fold scan`, both HEAD-verified
   `present` at the moment of the disagreement.
2. `blob_storage_log` shows all three keys uploaded in strict id order 19 seconds earlier —
   `0x1430c` at `16:47:19.211480`, `0x1430d` at `.212340`, `0x1430e` at `.213680`.
3. Walk 1 RETURNED `0x1430e`, so it ran after `.213680`, by which time the other two had been durable for
   1.3–2.2 ms — and for nineteen seconds by the time the disagreement was reported. It did not return them.
4. Upload order is not an assumption: 65,263 ref-log uploads, **zero out of order** once partitioned by
   epoch.

## The concurrent-deleter branch is excluded too, on the SECOND node {#second-node}

Probe A's own message names two explanations: an inconsistent store, or "a deposed leader is deleting ref
objects concurrently". The second is now measurably dead for this occurrence — and it needed the OTHER
node's log to check, which the first pass did not do.

| check | result |
|---|---|
| ch2 deletes of ref objects, all time | **0** |
| ch1 ref deletes during 16:47:00–16:48:00 | **0** |
| ch2 ref deletes during 16:47:00–16:48:00 | **0** |
| ch2 touches in ch1's namespace | 45, all under `cas/manifests` — ordinary pool-wide GC |

So at the moment of the disagreement there was no deleter of ref objects anywhere in the cluster. `ch2`
does operate inside `ch1`'s namespace, which is by design (GC is pool-wide and the lease holder cleans for
everyone), but it has never removed a ref object.

`second_node_no_concurrent_deleter.txt` holds these counts.

## What it does NOT show {#limits}

`blob_storage_log` records object writes and deletes, **not LIST calls**. The enumeration requests
themselves are invisible here, so this evidence bounds the timing but says nothing about how the store
served the listing — whether a page was dropped, a boundary mishandled, or something else. That mechanism
is still unknown.

It is also RustFS, not AWS S3. The design conclusion does not depend on which store did it: GC must not
trust LIST completeness either way.

## The reassuring half {#failclosed}

The same three keys were deleted at `16:53:59`, six minutes later. Folding aborted, the cursor held, a
later complete enumeration folded them and GC reclaimed them normally. **No leak resulted from this
occurrence** — the detector produced exactly the outcome it exists for.
