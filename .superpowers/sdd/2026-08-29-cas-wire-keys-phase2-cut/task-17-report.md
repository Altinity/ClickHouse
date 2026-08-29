# Task 17 report: `cas_ref_catalog` and external wire sweep

## Catalog inventory and expectations

Inventory was completed before edits. The codec, raw-row helpers, battery golden, and missing-namespace fixture used `k:"ent"`, `st`, `inc`, `rsr`, `csr`, `cwe`, and `cfg`; the two external readers were `t8_s44_stuck_removing_discrimination.py` and `05023_cas_dropns_leaked_namespace.sh`.

| Old spelling | Updated expectation |
|---|---|
| `k:"ent"` | `kind:"entry"` |
| `ns` | `ns` (kept) |
| `st` | `state` |
| `inc` | `life` |
| `rsr` | `remove_round` |
| `csr` | `creator` |
| `cwe` | `creator_epoch` |
| `cfg` | `creator_fence` |

`creating`, `live`, and `removing` remain their existing words. The creator/state and removal/state predicates are unchanged. `DecodeRejectsUnknownEntryKey` pins the strict reader's explicit `unknown entry key` `CORRUPTED_DATA` exception; `build/src/unit_tests_dbms --gtest_list_tests` listed that new test.

`05023` now treats a present row without a readable `state` field as an error, rather than returning the absent-row sentinel. This makes a parser-vocabulary mismatch loud.

The format registry now documents the `cas/ref_catalog` row and its live keys.

## External sweep

Before editing, each changed-plan spelling was searched in all three roots in raw (`"key":`), escaped (`\"key\":`), and bare-token (`"key"`) forms. One- and two-character spellings were classified only in CAS files or at CAS-wire context; non-CAS hits are recorded below. The known already-fixed parsers were rechecked: `test_cas_gc_sharded`, `test_cas_gcs`, and card `s38` use their live vocabulary.

The table reports post-edit line-hit counts as `raw / escaped / bare` for `tests/integration` (I), `utils/ca-soak` (S), and `tests/queries/0_stateless/*cas*` (Q). Every nonzero row is classified; all remaining listed spellings had `0 / 0 / 0` in every root.

| Spelling | I | S | Q | Classification of every hit |
|---|---:|---:|---:|---|
| `ns` | 0/0/0 | 2/0/2 | 0/1/0 | CAS catalog parser uses the deliberately retained catalog `ns`; escaped `rn` documentation is unrelated prose. |
| `k` | 0/0/5 | 1/0/7 | 0/0/0 | All are unrelated command/data dictionary keys; the soak raw hit is the workload row key, not a CAS wire object. |
| `b` | 31/0/173 | 0/0/3 | 133/0/159 | Unrelated JSON/SQL fixture data, chiefly `json_cast` files selected by the literal `*cas*` glob; no CAS marker or sibling key. |
| `s` | 5/0/67 | 2/0/2 | 0/0/0 | Unrelated shell/process and duration-unit data. |
| `m` | 0/0/23 | 2/0/2 | 0/0/0 | Unrelated process/duration-unit data. |
| `mc` | 0/0/4 | 0/0/0 | 0/0/0 | Unrelated MinIO client command token. |
| `p` | 1/0/3 | 0/0/0 | 0/0/0 | Unrelated JSON fixture/process data. |
| `alg` | 4/0/4 | 0/0/0 | 0/0/0 | JWT JWK algorithm fields, not CAS wire data. |
| `ls` | 0/0/44 | 0/0/0 | 0/0/0 | Shell `ls` command tokens. |
| `le` | 35/0/35 | 0/0/0 | 0/0/0 | Prometheus histogram-label fields. |
| `h` | 2/0/7 | 2/0/2 | 0/0/0 | Unrelated duration/data tokens. |
| `tt` | 0/0/4 | 0/0/0 | 0/0/0 | Unrelated test table names. |
| `g` | 2/0/9 | 0/0/0 | 15/0/31 | Unrelated JSON fixtures, including `json_cast` reference data. |
| `pid` | 0/0/0 | 1/0/1 | 0/0/0 | Soak process-id telemetry, not pool metadata. |
| `ts` | 0/0/0 | 9/0/29 | 0/0/0 | Soak metrics/run-log timestamps, not blob-envelope time fields. |
| `rn` | 0/0/0 | 0/1/0 | 0/0/0 | Documentation example only; not a parser or assertion. |
| `me` | 0/0/0 | 0/0/1 | 0/0/0 | Backlog prose about a historical allocation; not wire data. |

Zero-hit spelling roster (each root and form): `oc`, `pend`, `sz`, `cr`, `pm`, `il`, `hln`, `gcs`, `mrg`, `rnd`, `sg`, `spt`, `sa`, `msc`, `lo`, `su`, `rt`, `nwe`, `we`, `hn`, `sat`, `eat`, `ma`, `fen`, `cte`, `cts`, `cse`, `css`, `lse`, `lss`, `rs`, `!pse`, `!pss`, `lc`, `ha`, `tv`, `mb`, `mo`, `ome`, `omb`, `omo`, `nme`, `nmb`, `nmo`, `obk`, `orn`, `nbk`, `nrn`, `pg`, `rfl`, `btr`, `cnd`, `ck`, `gen`, `lfe`, `lfs`, `hr`, `hpe`, `hps`, `hrc`, `hnr`, `rte`, `rts`, `ct`, `pt`, `ocr`, `cls`, `cur`, `bld`, `by`, `ch`, `st`, `inc`, `rsr`, `csr`, `cwe`, `cfg`, `ent`, and row-tag `c`. `seq` is contextual: it remains live in `cas_mount_lease`; its old heartbeat use was verified absent in CAS-wire context. (Duplicated spellings across plan tasks are listed once.)

CAS-wire fixes made by this task:

| Root | File | Result |
|---|---|---|
| `utils/ca-soak` | `scripts/t8_s44_stuck_removing_discrimination.py` | Catalog regex now matches `kind:"entry"`, `state`, and `life`. |
| `tests/queries/0_stateless/*cas*` | `05023_cas_dropns_leaked_namespace.sh` | Catalog parser now reads `state` and errors on a present malformed row. |
| `tests/integration` | — | No remaining old CAS-wire assertion; the known GCS and sharded parsers already use live names. |

## Verification

`ninja -C build unit_tests_dbms > build/build_wirekeys_p2_task17.log 2>&1` completed successfully. `build/src/unit_tests_dbms --gtest_filter='CAS*' > build/test_cas_p2_task17.log 2>&1` completed successfully. Required summary (the log contains binary diagnostic bytes, so `grep -aE` was required):

```text
[==========] 2220 tests from 284 test suites ran. (162821 ms total)
[  PASSED  ] 2220 tests.
```

## Deviations

None. The normal `grep -E` reports the CAS test log as binary because a passing test emits binary diagnostic data; `grep -aE` produced the required summary without changing the test command or log.
