# `clickhouse-disks` exit-code change — blast radius, fix, upstream note

## The change

`DisksApp::main` (`programs/disks/DisksApp.cpp`) returns `last_command_exit_code` as the process exit
code when `--query` was given. `processQueryText` resets that field once at entry and each catch arm
overwrites it, so within one semicolon-separated batch a later success does **not** clear an earlier
failure: the exit code says "something in this batch failed", not "the last command failed".
Interactive runs (`runInteractive`) are unaffected — the guard is `query.has_value()`.

Before the change, every `clickhouse-disks --query <cmd>` exited 0; the error went to stderr and the
log only. Error printing is unchanged, so nothing that diffs output changed behavior.

## Enumeration method and count

`git grep -n -I -E "clickhouse[ -]disks"` over `tests/ ci/ utils/ docs/ programs/ src/ .github/`,
excluding the untracked `ci/tmp/` debris. Filtering to the files that actually *invoke* the tool
(dropping `.md` prose, `.xml` configs, the `.jsonl` consolidation dumps, source-comment mentions, and
`test_disks_app_interactive`, which drives the REPL and never passes `--query`) leaves **22 invoking
files** carrying **77 non-comment invocation-bearing lines** — a line count rather than a call count,
because several of those lines are `disk_cmd_prefix = …` helper definitions that stand for more than
one call. Every invocation reachable from them is classified below.

Do not re-derive the raw grep total from this document: the BACKLOG entry written for Task 3 itself
names the tool, so the tree-wide count moved while this sweep was in progress.

Two structural facts settle most of the table, and both were read out of the code rather than assumed:

- **Stateless `.sh` tests are insulated.** `tests/queries/shell_config.sh` sets neither `-e` nor
  `pipefail`, and `clickhouse-test` runs the script through the pattern `{test} > {stdout} 2> {stderr}`
  — no `bash -e` wrapper. So the script's exit status is its LAST command's. `clickhouse-test` does
  fail a test on a nonzero exit (`FailureReason.EXIT_CODE`), but only the tail command can produce one.
- **Integration tests are the exposed surface.** `Cluster.exec_in_container` defaults to
  `nothrow=False` and raises on a nonzero exit.

Two command semantics that decide individual rows:

- `remove` **throws** on an absent path (`CommandRemove::executeImpl` final `else`).
- `list` does **not** — `DiskWithPath::listAllFilesByPath` returns an empty vector for a
  non-directory, so `ls` on a missing path succeeds with empty output.

## Blast-radius table

| Site | Class | Note |
|---|---|---|
| `tests/integration/test_replicated_database/test.py` `test_replicated_table_structure_alter` | **(b) AT RISK — confirmed victim, FIXED** | `remove {metadata_path}` with `metadata_path` empty because the SELECT ran after `DETACH DATABASE` on the same node. See below. |
| `tests/integration/test_replicated_database/test.py` `test_table_metadata_corruption` | (a) | SELECT of `metadata_path` runs before `stop_clickhouse`; database attached. |
| `tests/integration/helpers/database_disk.py` `replace_text_in_metadata` / `write_to_file` / `move_file` | (a) via callers | Path is a caller parameter; all three callers (below) supply a path read while the object was visible. |
| `tests/integration/test_attach_table_normalizer/test.py` (detached-table case) | (a) | Reads `metadata_path` from `system.detached_tables` — the table is detached *on purpose*, and that view has the row. |
| `tests/integration/test_attach_table_normalizer/test.py` `test_attach_substr_restart` | (a) | SELECT before the restart. |
| `tests/integration/test_storage_mongodb/test.py` | (a) | SELECT before `stop_clickhouse`. |
| `tests/integration/test_replicated_database_recover_digest_mismatch/test.py` `ways_to_corrupt_metadata` | (a)/(c) | `db_data_path` and `db_disk_path` read while the server is up. The `remove -r .../store/` entry already carries `\|\| true`, which was dead code before and is live now. Each `remove` target is restored by the recovery the loop asserts on before the next iteration. |
| `tests/integration/test_database_disk_setting/test.py` (`directory_exists`, `replace_text_in_metadata`, `read_file`, `write_to_file`, `remove_file`) | (a) | `directory_exists` looked like the same shape as the victim — it asserts `not directory_exists(...)` on a disk where the parent may not exist — but it uses `ls`, which does not throw on a missing path. Verified against `listAllFilesByPath`. |
| `tests/integration/test_tmp_policy/test.py` | (a) | Literal paths; `write` then `ls`. |
| `tests/integration/test_azure_blob_storage_native_copy/test.py` | (a) | Literal `im_a_file.txt` / `another_file.txt`. |
| `tests/integration/test_disks_app_other_disk_types/test.py` | (a) | Literal. |
| `tests/integration/test_disks_app_func/test.py` (`write`/`touch`/`mkdir`/`ls`/`remove`/`remove_recurive` and the inline `link`/`move` calls) | (a) | Every `remove` target is proved present by the `ls` assertion immediately before it; the two `remove -r .` calls target the disk root, which always exists. |
| `tests/integration/test_cas_drop_pool_member/test.py`, `test_cas_ref_snaplog/test.py` (`cas-fsck`, `cas-gc-dryrun`) | (c) | These *want* the exit code. `cas-fsck` throws only on `dangling`, `chain_broken`, `corrupted_runs`, `lifeless_keys` — all hard corruption the tests already assert against. `janitor_pending`, which the drop-pool-member test polls down to zero, is a printed note and does **not** exit nonzero, so the poll loop cannot spuriously raise. |
| `ci/jobs/scripts/clickhouse_proc.py` `save_system_metadata_files_from_remote_database_disk` | (c) | Uses `Shell.get_output` and validates the result (`is_valid_uuid`) instead of trusting the exit code. |
| `utils/ca-soak/soak/fsck.py` `run_fsck` / `run_dryrun` | (c) | Captures `p.returncode` into `exit_code` explicitly. This harness is the consumer the change exists for. |
| `tests/queries/0_stateless/02802_clickhouse_disks_s3_copy.sh` | (a) | Tail command is `remove -r $CLICKHOUSE_DATABASE/test.copy`, created by the `copy` two lines above. |
| `02980_s3_plain_DROP_TABLE_{MergeTree,ReplicatedMergeTree}.sh` | (a) | disks calls are `list`; tail is a `CLICKHOUSE_CLIENT` drop. |
| `03001_matview_columns_after_modify_query.sh` | (a) | Same shape as the victim but **already correct**: `mv_metadata_path` is captured before `DETACH TABLE mv`. |
| `03566_write_disks_with_append_mode.sh` | (a), intentional-failure note | The three `write --mode append` calls are *expected* to fail on plain/plain-rewritable disks (hence `2>/dev/null`) and now exit nonzero. They are mid-script with no `-e`, so they cannot affect the result; tail is `remove $file_name`. Not edited — an `\|\| true` would change nothing observable. |
| `03578_copy_file_in_object_storage.sh` | (a), same intentional-failure note | Tail is `remove $file_name_non_existing_target`, created by the `cp` above it. |
| `03600_disk_app_plain_rewritable_disks_list_cmd.sh` | (a) | Two deliberately-failing `cd` batches, both piped into `grep`, so the pipeline's status is grep's. Tail is `rm -r /${prefix}_hello/`. |
| `04326_disks_app_read_checksums.sh`, `04327_clickhouse_disks_sed.sh`, `04400_clickhouse_disks_read_bitmap.sh` | (a) | Deliberate failures are piped into `grep`; tails are `CLICKHOUSE_CLIENT`, `remove $file`, and a grep pipeline respectively. |
| `docs/en/operations/utilities/clickhouse-disks.md` and the CAS doc set | n/a | Prose examples; not executed by CI. |

**Conclusion: exactly one at-risk site, and it was the already-confirmed one.** No second victim.

## The victim fix

`tests/integration/test_replicated_database/test.py::test_replicated_table_structure_alter`. The
SELECT of `metadata_path` for `table_structure.mem` moved from after `competing_node.query("DETACH
DATABASE table_structure")` to before it, with `assert metadata_path` added. `db_disk_name` stays where
it was (it reads a config file, not the server). Intent unchanged: the metadata file is still removed
to simulate the corruption the test then recovers from. Validated with `ast.parse`.

Not verified: the test has not been *run*. Confirming the fix makes the three sanitizer lanes green
needs an integration run, which this session was directed not to perform.

## Upstream note

`docs/superpowers/cas/BACKLOG.md`, section `[disks-exit-code-upstream]` (anchor
`{#disks-exit-code-upstream}`). BACKLOG.md rather than `upstream.md`: BACKLOG.md is the live pending
list with a stable anchor scheme and a frontmatter'd doc identity, while `upstream.md` is an unstructured
scratch dump of design notes with no entry format to join.

A stale `fsck`/`cas-fsck` name in the `DisksApp` exit-code comment was batched into
`docs/superpowers/cas/deferred-docs-fixes.md` as D50 rather than opened as a code round.

## Commits

- `11b5c32a5f2` — ca: tests -- read metadata_path before the detach that hides it
- `0a17684b9d2` — docs: BACKLOG/upstream -- the clickhouse-disks exit-code change is upstream-affecting
