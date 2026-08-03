# CI review fixes — Altinity PR 2073 (strtgbb)

https://github.com/Altinity/ClickHouse/pull/2073

## Recommendation 1 — trim the CAS ParamSet comments

The three CAS comment blocks in `ci/defs/job_configs.py` were removed together with the
ParamSets themselves (see recommendation 2) and re-written at the new location, each ≤3 lines,
keeping only the load-bearing constraint:

- CAS-over-S3: RustFS rather than MinIO OSS, because the incarnation pool needs enforced
  conditional deletes.
- Sanitizer lanes: sharded because an unsharded lane exceeds the 6h GitHub job timeout and is
  killed before uploading results.
- Local-storage lane: one line.

Dropped: which sanitizer died at which hour, the kill-lands-after-the-test-loop detail, the
per-lane coverage inventory, and the config-file names (they are visible in
`ci/jobs/functional_tests.py`).

## Recommendation 2 — move the ParamSets to `AltinityJobConfigs`

`common_ft_job_config` is module-level in `ci/defs/job_configs.py`, so it is importable as the
reviewer sketched. `ci/defs/job_configs.py` does not import `ci/defs/altinity_jobs.py`, so the new
`from ci.defs.job_configs import common_ft_job_config` introduces no cycle.

All 10 CAS ParamSets moved from `JobConfigs.functional_tests_jobs` to
`AltinityJobConfigs.cas_functional_tests_jobs`:

| parameter | count |
| --- | --- |
| `amd_binary, cas s3 storage, parallel` | 1 |
| `amd_asan_ubsan, cas s3 storage, parallel, {b}/2` | 2 |
| `amd_tsan, cas s3 storage, parallel, {b}/2` | 2 |
| `amd_msan, cas s3 storage, parallel, {b}/3` | 3 |
| `arm_binary, cas s3 storage, parallel` | 1 |
| `amd_binary, cas storage, parallel` | 1 |

The count was derived by grepping `cas s3 storage|cas storage` under `ci/defs`, `ci/workflows`,
`ci/jobs` before the edit, and re-derived after the move as
`len(AltinityJobConfigs.cas_functional_tests_jobs) == 10`.

### Consumers

Every consumer of `JobConfigs.functional_tests_jobs` picked the CAS jobs up implicitly, some
through substring filters that are easy to miss:

- `ci/workflows/pull_request.py` — all four uses (`ALL_FUNCTIONAL_TESTS`, the blocking-name list,
  the plain-job pick, the job list).
- `ci/workflows/pull_request_community.py` — three uses.
- `ci/workflows/master.py` — the non-coverage job list.
- `ci/workflows/release_builds.py` — filter `any(t in job.name for t in ("release", "binary"))`,
  which matches the three CAS `*_binary` lanes.
- `ci/workflows/release_branches.py` — filter `"asan" in job.name`, which matches the two CAS
  `amd_asan_ubsan` shards.
- `ci/workflows/backport_branches.py` — filter `"amd_asan_ubsan" in job.name`, same two shards.

Each file now defines a module-level

```py
FUNCTIONAL_TESTS_JOBS = [
    *JobConfigs.functional_tests_jobs,
    *AltinityJobConfigs.cas_functional_tests_jobs,
]
```

and every reference to `JobConfigs.functional_tests_jobs` (word-boundary match, so
`functional_tests_jobs_coverage` and `_azure` are untouched) uses it. Because the CAS ParamSets
were the last entries of `functional_tests_jobs`, the concatenation reproduces the previous list
element-for-element in the same order — which is what makes this a provably pure relocation.

`ci/jobs/functional_tests.py` selects on the `--options` parameter string, not on job objects, so
it needed no change.

## Verification

1. Baseline: tree clean of workflow diffs, `PYTHONPATH=ci python3 -m praktika yaml`, copy of
   `.github/workflows` taken before any edit.
2. After the move and the comment edits, regenerated and diffed:

```
$ diff -r /tmp/wf_before .github/workflows && echo "WORKFLOW DIFF EMPTY"
WORKFLOW DIFF EMPTY
$ git status --porcelain -- .github/workflows
(empty)
```

3. Only four workflows are generated (Community PR, Release Builds, MasterCI, PR), so the empty
   YAML diff does not cover `release_branches` and `backport_branches` — the two workflows that
   pick CAS jobs up through the `asan` / `amd_asan_ubsan` substring filters. Those were covered
   separately by importing every `ci/workflows/*.py` module and dumping `workflow.jobs` names,
   at `HEAD` (extracted with `git archive`) and in the working tree:

```
$ diff /tmp/wf_jobs_old.txt /tmp/wf_jobs_new.txt && echo "ALL WORKFLOW JOB LISTS IDENTICAL"
ALL WORKFLOW JOB LISTS IDENTICAL
```

488 job entries total, 37 of them CAS: backport_branches 2, master 10, pull_request 10,
pull_request_community 10, release_branches 2, release_builds 3.

4. Imports:

```
$ PYTHONPATH=.:ci python3 -c "import ci.defs.job_configs, ci.defs.altinity_jobs as a; print('import ok', len(a.AltinityJobConfigs.cas_functional_tests_jobs))"
import ok 10
```

5. `grep -n "cas" ci/defs/job_configs.py` returns only two `ParallelReplicas` lines — no CAS
   ParamSet remains.

## Recommendation 3 — `ci/jobs/scripts/clickhouse_proc.py` comments

- Read-only CAS scrape (in `dump_system_tables`): 21 comment lines to 6, keeping the three
  constraints — writable open claims server-root ownership and fails against the real server's
  persisted owner, the substitution is keyed on the `<metadata_type>cas</metadata_type>` marker
  rather than on disk names, and `grep -R` plus `sed --follow-symlinks` are required because
  `tests/config/install.sh` symlinks the configs into `config.d`. The fail-loud WARNING check is
  unchanged; its comment is now two lines.
- `start_rustfs`: the RustFS rationale went from 6 lines to 4 (dropping the internal `M-W D-W8`
  reference), and the scanner/heal-manager plus open-files block from 17 lines to 7, keeping why
  each is disabled or raised and the deprecated-env-name note.

No other CAS-related comment in the file exceeded the standard.

## Not done / notes

- `black` is not installed in this environment, so the edited Python was not machine-formatted;
  the inserted code follows the surrounding style (magic-trailing-comma list literals) and all
  four edited-module trees parse.
