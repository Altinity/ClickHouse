# Task 2 — `system.cas_log`: `gen`→`generation`, `indeg`→`indegree`, `snap`→`snapshot`

## Enumeration

`git ls-files 'src/*' 'tests/*' 'docs/en/*' 'utils/*' 'programs/*' | xargs grep -n '"gen"\|indeg_zero\|prev_indeg\|"snap"'`
returned 12 hits. Classified:

| hit | verdict |
|---|---|
| `CasFoldSealFormat.cpp` `"gen"` (write + read) | PERSISTED format — left |
| `gtest_cas_fold_seal_format.cpp` `,"gen":"7"` golden | pins the persisted format — left |
| `gtest_cas_gc_round_defer.cpp` `putIfAbsent(snap_key, "snap")` | arbitrary object *body* payload, not a log value — left |
| `Parsers/obfuscateQueries.cpp` `"snap"` | unrelated word list — left |
| `utils/ca-soak/scenarios/BACKLOG.md` `indeg_zero` | dated narrative quoting a past run's trace — left (see Survivors) |
| `CasEvent.cpp` :34 `indeg_zero`, :87 `snap` | RENAMED |
| `gtest_cas_event_log.cpp` :42 assertion, :235 message | RENAMED |
| `ContentAddressedLog.cpp` :24 :29 :32 :39 | RENAMED |
| `docs/.../cas_log.md` :25 :29 :33 :39 | RENAMED |

Consumers re-checked separately: no stateless test, integration test, or `utils/ca-soak` file selects the
`gen` column or matches `object_kind='snap'` / `event_type='indeg_zero'` — every `gen` hit under
`tests/`+`utils/` is either a Python local holding a generator SQL string, the persisted
`gc/gen/<N>/completion_seal` object path (`test_cas_gc_sharded`), or unrelated core-ClickHouse code.

## Deviation: `prev_indeg` has no emitter

The plan says to "rename the `prev_indeg` detail-key literal at its emitter". **There is no emitter.**
`prev_indeg` occurs exactly twice in the tree — in the `detail` column description in
`ContentAddressedLog.cpp` and in the mirrored sentence in `cas_log.md`. Enumerating every `CasEvent`
detail key actually written (both the `detail["k"] = v` and the `e.detail = {{"k", v}, …}` forms) gives:
`branch`, `holder_epoch`, `holder_expires_at_ms`, `holder_hostname`, `holder_pid`, `holder_seq`,
`holder_uuid`, `srid`, `superseded_token`, `condemn_round`, `code`, `site`, `live`, `shards`,
`changed_shards`, `namespaces`, `expected`, `log`, `manifest_ref_instance`, … — no `prev_indeg`, and also
no `dropped_by` and no `cursor`, two more of the four examples that description offered.

Rather than rename a key that does not exist, the example list was replaced with four keys read at their
emitters: `condemn_round` and `superseded_token` (`CasGc::…`, the retire/supersede sites) and `code` /
`site` (`CasManifestReader`, the read-failure sites).

## Verification

- `grep -n 'indeg_zero\|prev_indeg'` tree-wide (minus `docs/superpowers/`, `.superpowers/`, `contrib/`):
  one hit, the ca-soak BACKLOG narrative.
- `ninja -C build clickhouse unit_tests_dbms` → `NINJA_EXIT=0` (`build/obscure_t2_build.log`).
- `unit_tests_dbms --gtest_filter='CAS*Log*:CAS*Event*'` → **56 tests from 36 suites, PASSED**, exit 0
  (`build/obscure_t2_test.log`). `gtest_cas_event_log.cpp` pins `toString(CasEventType::IndegZero) ==
  "indegree_zero"`, so a revert of the `CasEvent.cpp` literal fails this suite.

## Survivors

- `utils/ca-soak/scenarios/BACKLOG.md:273` — a dated "Observed:" paragraph transcribing an event chain
  from a past soak run. Left as written: it records what that run emitted, and the spelling change does
  not retroactively apply to it.
