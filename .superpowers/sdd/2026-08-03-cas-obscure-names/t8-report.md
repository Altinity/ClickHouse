# Task 8 — final sweep

## Grep gate 1 — the decided renames

```
git ls-files | grep -vE '^docs/superpowers/|^\.superpowers/|^contrib/' \
  | xargs grep -nE 'CASGc[A-Z]|CASRefCkpt|FoldedTxns|PutDedup\b|DedupCache|indeg_zero|prev_indeg|
                    phase_duration_us|started_at_ms|expires_at_ms|dedup_cache_bytes|gc_snap_generations|
                    ModeledCostMsPerRequestedMiB|ca-fsck|ca-gc-dryrun|ca-gc-rebuild|ca-inspect|ca-drop-member'
```

Zero hits for every token except `started_at_ms` / `expires_at_ms` and two changelog lines. The
survivors, each deliberate:

| what | where | why it stays |
|---|---|---|
| `ReaderExecutorModeledCostMsPerRequestedMiB` | `CHANGELOG.md`, `docs/_includes/content/changelog.md` | released-history entries for PR #106968; changelogs are not retroactively edited |
| `started_at_ms` / `expires_at_ms` (~70 hits) | `MountLease` members and their reads/writes/comments in `CasServerRootFormats`, `CasPool`, `CasServerRoot`, `CasGc`, `CasMountRuntime`, seven gtests | plain `uint64_t` epoch milliseconds — the suffix is true. The rename was justified only by the *column* being `DateTime64(3)` |
| `.add("started_at_ms", …)` / `.add("expires_at_ms", …)` | `Tools/CasInspect.cpp` | `cas-inspect` JSON keys carrying raw millisecond integers |
| `detail["holder_expires_at_ms"]` | `Pool/CasServerRoot.cpp` | `system.cas_log` detail key, value is a stringified millisecond integer |
| `expires_at_ms={}` in log lines | `CasPool`, `CasServerRoot` | log field naming its own millisecond value |
| `indeg_zero` | `utils/ca-soak/scenarios/BACKLOG.md:273` | a dated "Observed:" transcription of a past soak run's event chain |
| `"gen"` | `Formats/CasFoldSealFormat.cpp` + its golden in `gtest_cas_fold_seal_format.cpp` | persisted fold-seal serialization key |

## Grep gate 2 — whole-word `srid` / `gen` / `snap` in user-facing docs and tests

```
git ls-files 'docs/en/*' 'tests/queries/*' | xargs grep -nw 'srid\|gen\|snap'
```

Reviewed every hit. `srid`: **none** outside iceberg's unrelated spatial-reference-id fixtures.
`gen` / `snap`: all unrelated — Python locals named `gen` in non-CAS tests, the `SNAPSHOT` DDL parser
tests, `FREEZE … WITH NAME 'snap'`, iceberg `snap-*.avro` metadata fixtures, and crawl-data
`.reference` payloads.

One CAS hit deserves naming: `docs/en/sql-reference/statements/system.md` documents that
`SYSTEM CAS GC REBUILD` writes the `gc/gen/*` objects. That is the persisted object-key path in
`CasLayout`, not a renameable user-facing name, so it stays.

## Additional user-facing abbreviations found and NOT actioned

Enumerating every `CasEvent::detail` key (both the `detail["k"] = v` and `e.detail = {{"k", v}}`
forms) surfaced abbreviations outside the decided list — `ns`, `me`, `p`, `ha` appear as short keys in
`CasFsck` / `CasGc` report maps. They are not on the Decisions list and were left; they are worth a
follow-up decision.

## Gates

Both run sequentially (never concurrently — parallel gate runs exhaust tmpfs inodes), each under
`flock "$(git rev-parse --git-common-dir)/unit_tests.lock"`.

| build | generator | result |
|---|---|---|
| release `build` | 278 suites, 4 excluded, **0 unclaimed** | `TOTALS: pass=278 fail=0 abort=0`, `GATE_EXIT=0` |
| ASan `build_asan` | 296 suites, 4 excluded, **0 unclaimed** | `TOTALS: pass=296 fail=0 abort=0`, `GATE_EXIT=0` |

Logs: `build/obscure_t8_gate_release.log` + `build/per_suite_results.txt`,
`build_asan/obscure_t8_gate_asan.log` + `build_asan/per_suite_results.txt`.

**Both arms of the death-test split are proven compiled**, not merely passing. The ASan suite list
minus the release suite list is exactly the 18 `*DeathTest` suites, and it contains
`CASGCHoldGrammarDeathTest` and `CASGCStateFormatDeathTest` — the two suites Task 1 renamed from
`CASGc*` and updated in the generator's `KNOWN_COMPILE_GUARDED` list. Release claims all 278 of its
suites and ASan all 296 of its, so neither arm silently lost a renamed suite to the preprocessor.

## Result

The sweep changed nothing on this pass — no fix-up commit was needed.
