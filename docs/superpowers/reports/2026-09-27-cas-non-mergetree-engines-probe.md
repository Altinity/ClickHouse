---
description: 'Probe of non-MergeTree engines (Log, TinyLog, StripeLog, Join, Set) on a CAS disk with the 26.6.2 antalya binary and a local object-storage backend: the Log family works through the verbatim namespace-file mechanism; persistent Join and Set fail because their tmp/<n>.bin path is parsed as a part file.'
sidebar_label: 'CAS non-MergeTree engines probe'
sidebar_position: 40
slug: /superpowers/reports/cas-non-mergetree-engines-probe
title: 'Non-MergeTree engines on a CAS disk: probe 2026-09-27'
doc_type: 'report'
---

# Non-MergeTree engines on a CAS disk: probe 2026-09-27 {#cas-non-mergetree-engines-probe}

Why: the design study `2026-09-26-cas-table-files-as-refs-design.md` replaces the verbatim
namespace-file mechanism (`cas/ns/state/<life_id>/_files/<name>`). Non-MergeTree engines that store
files directly under the table directory depend on that mechanism, and the branch has no test or
document saying which of them work. This probe establishes the baseline.

Setup: `build/programs/clickhouse` 26.6.2.20000.altinityantalya, standalone server, one CAS disk
over `object_storage_type=local` (`tmp/cas95/nonmt/config.xml`), tables created with
`SETTINGS disk = 'cas'`, 1,500 rows per Log table, restart of the server between the two reads,
then appends, `TRUNCATE`, `DROP ... SYNC`. Scripts and logs: `tmp/cas95/nonmt/`.

## Results {#results}

| engine | create, insert | read | survives restart | append after restart | truncate, drop | objects written |
|---|---|---|---|---|---|---|
| `Log` | ok | ok | ok | ok | ok | `_files/k.bin`, `s.bin`, `__marks.mrk`, `sizes.json` |
| `TinyLog` | ok | ok | ok | not tested | drop ok | `_files/k.bin`, `s.bin`, `sizes.json` |
| `StripeLog` | ok | ok | ok | ok | not tested | `_files/data.bin`, `index.mrk`, `sizes.json` |
| `Join`, `persistent = 1` | insert fails | in-memory rows only | lost | | drop ok | none |
| `Set`, `persistent = 1` | insert fails | in-memory rows only | lost | | | none |
| `Join`, `Set`, `persistent = 0` | ok | ok | memory only by design | | | none |
| `MergeTree` (control) | ok | ok | ok | | | refs |

The failing insert:

```
Code: 48. DB::Exception: Autocommit writes are not supported for content part files on a
content-addressed disk. (NOT_IMPLEMENTED)
```

Cause: `StorageSetOrJoinBase` writes `<table>/tmp/<n>.bin` and then `replaceFile`s it to
`<table>/<n>.bin` (`src/Storages/StorageSet.cpp:114`, `:131`, `:140`). On an Atomic path the CAS
path parser takes the first component after the table uuid as the part component, so `tmp/1.bin`
is a part file `1.bin` of part `tmp`, and the autocommit part-file write is refused
(`ContentAddressedTransaction.cpp:792-795`). The Log family writes flat names and never hits this.

## What this means {#meaning}

- The verbatim-file mechanism is what makes the Log family work on CAS today. Any replacement
  (file refs) must keep arbitrary flat file names at table level, `WriteMode::Append`
  (`Log` appends to `k.bin` on every insert), `replaceFile`, `TRUNCATE` (rewrite of every file) and
  `DROP`.
- `Log` on CAS rewrites every column file on each insert (the disk cannot append). Through file refs
  each append would publish a new manifest and, above the inline size, a new whole-file blob, and
  leave the previous one to GC. Today it is one HEAD plus one PUT per file with no garbage.
- Persistent `Join` and `Set` are broken today by the nested `tmp/` path, independently of this
  design. The same parser rule would break any engine that keeps a subdirectory under the table
  directory.
