import copy
from praktika import Workflow, Artifact

from ci.defs.defs import BASE_BRANCH, DOCKERS, SECRETS, ArtifactConfigs, JobNames
from ci.defs.job_configs import JobConfigs
from ci.jobs.scripts.workflow_hooks.filter_job import should_skip_job

FUNCTIONAL_TESTS_PARALLEL_BLOCKING_JOB_NAMES = [
    job.name
    for job in JobConfigs.functional_tests_jobs
    if any(
        substr in job.name
        for substr in (
            "_debug, parallel",
            "_binary, parallel",
            "_asan, distributed plan, parallel",
        )
    )
]

REGULAR_BUILD_NAMES = [job.name for job in JobConfigs.build_jobs]

workflow = Workflow.Config(
    name="Community PR",
    event=Workflow.Event.PULL_REQUEST,
    base_branches=[BASE_BRANCH, "releases/*", "antalya-*", "stable-*"],
    if_condition="github.repository != github.event.pull_request.head.repo.full_name",
    jobs=[
        JobConfigs.fast_test,
        *[job.set_dependency([JobNames.FAST_TEST]) for job in JobConfigs.build_jobs],
        *JobConfigs.unittest_jobs,
        *[
            j.set_dependency(
                FUNCTIONAL_TESTS_PARALLEL_BLOCKING_JOB_NAMES
                if j.name not in FUNCTIONAL_TESTS_PARALLEL_BLOCKING_JOB_NAMES
                else []
            )
            for j in JobConfigs.functional_tests_jobs
        ],
        *JobConfigs.stateless_tests_flaky_pr_jobs,
        *[
            job.set_dependency(FUNCTIONAL_TESTS_PARALLEL_BLOCKING_JOB_NAMES)
            for job in JobConfigs.integration_test_jobs_required[:]
        ],
        *[
            job.set_dependency(FUNCTIONAL_TESTS_PARALLEL_BLOCKING_JOB_NAMES)
            for job in JobConfigs.integration_test_jobs_non_required
        ],
        JobConfigs.integration_test_asan_flaky_pr_job,
        *[
            job.set_dependency(FUNCTIONAL_TESTS_PARALLEL_BLOCKING_JOB_NAMES)
            for job in JobConfigs.install_check_jobs
        ],
        # *[
        #     job.set_dependency(FUNCTIONAL_TESTS_PARALLEL_BLOCKING_JOB_NAMES)
        #     for job in JobConfigs.stress_test_jobs
        # ], # NOTE (strtgbb): Does not support github artifacts
        # *[
        #     job.set_dependency(FUNCTIONAL_TESTS_PARALLEL_BLOCKING_JOB_NAMES)
        #     for job in JobConfigs.upgrade_test_jobs
        # ], # TODO: customize for our repo
        # *[ # Remove fuzzers for now
        #     job.set_dependency(FUNCTIONAL_TESTS_PARALLEL_BLOCKING_JOB_NAMES)
        #     for job in JobConfigs.ast_fuzzer_jobs
        # ],
        # *[
        #     job.set_dependency(FUNCTIONAL_TESTS_PARALLEL_BLOCKING_JOB_NAMES)
        #     for job in JobConfigs.buzz_fuzzer_jobs
        # ],
    ],
    artifacts=[
        *ArtifactConfigs.unittests_binaries,
        *ArtifactConfigs.clickhouse_binaries,
        *ArtifactConfigs.clickhouse_stripped_binaries,
        *ArtifactConfigs.clickhouse_debians,
        *ArtifactConfigs.clickhouse_rpms,
        *ArtifactConfigs.clickhouse_tgzs,
        ArtifactConfigs.fuzzers,
        ArtifactConfigs.fuzzers_corpus,
    ],
    dockers=DOCKERS,
    disable_dockers_build=True,
    enable_dockers_manifest_merge=False,
    secrets=[],
    enable_job_filtering_by_changes=False,  # TODO: Change this back?
    enable_cache=False,
    enable_report=False,
    enable_cidb=False,
    enable_merge_ready_status=False,
    enable_commit_status_on_failure=False,
    pre_hooks=[
        "python3 ./ci/jobs/scripts/workflow_hooks/store_data.py",
        "python3 ./ci/jobs/scripts/workflow_hooks/version_log.py",
        "python3 ./ci/jobs/scripts/workflow_hooks/parse_ci_tags.py",
    ],
    workflow_filter_hooks=[should_skip_job],
    post_hooks=[],
)

# NOTE (strtgbb): use deepcopy to avoid modifying workflows generated after this one
for i, job in enumerate(workflow.jobs):
    workflow.jobs[i] = copy.deepcopy(job)
    workflow.jobs[i].enable_commit_status = False

for i, artifact in enumerate(workflow.artifacts):
    workflow.artifacts[i] = copy.deepcopy(artifact)
    workflow.artifacts[i].type = Artifact.Type.GH

WORKFLOWS = [
    workflow,
]
