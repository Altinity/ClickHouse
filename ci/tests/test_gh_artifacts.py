"""GitHub (`_GH`) ClickHouse binaries must exist as named artifacts with S3 fallbacks.

Community CI downloads binaries as GitHub Actions artifacts. The runner strips a
trailing `_GH` to find the matching S3 object when the GitHub artifact is missing
(cache hit, expired retention). Names and Artifact.Config entries have to exist
or workflow import and `praktika yaml` fail.
"""

from ci.praktika.artifact import Artifact
from ci.praktika.job import Job


def _jobs_from(container):
    jobs = []
    for value in vars(container).values():
        if isinstance(value, Job.Config):
            jobs.append(value)
        elif isinstance(value, list):
            jobs.extend(item for item in value if isinstance(item, Job.Config))
    return jobs


def _artifact_configs_from(container):
    configs = []
    for value in vars(container).values():
        if isinstance(value, Artifact.Config):
            configs.append(value)
        elif isinstance(value, list):
            configs.extend(
                item for item in value if isinstance(item, Artifact.Config)
            )
    return configs


def test_job_configs_import_with_gh_artifact_names():
    from ci.defs.altinity_jobs import AltinityJobConfigs
    from ci.defs.job_configs import JobConfigs

    assert JobConfigs.build_jobs
    assert AltinityJobConfigs.sign_release_jobs


def test_every_job_artifact_has_a_config():
    from ci.defs.altinity_jobs import AltinityArtifactConfigs, AltinityJobConfigs
    from ci.defs.defs import ArtifactConfigs
    from ci.defs.job_configs import JobConfigs

    configured = {cfg.name for cfg in _artifact_configs_from(ArtifactConfigs)}
    configured.update(
        cfg.name for cfg in _artifact_configs_from(AltinityArtifactConfigs)
    )

    missing = []
    for job in _jobs_from(JobConfigs) + _jobs_from(AltinityJobConfigs):
        for name in (job.provides or []) + (job.requires or []):
            if name not in configured:
                missing.append((job.name, name))

    assert missing == []


def test_gh_binaries_are_github_artifacts_with_s3_fallback():
    from ci.defs.defs import ArtifactConfigs, ArtifactNames

    gh_by_name = {cfg.name: cfg for cfg in ArtifactConfigs.clickhouse_binaries_gh}
    s3_names = {cfg.name for cfg in ArtifactConfigs.clickhouse_binaries}

    assert ArtifactNames.CH_ARM_BINARY_GH == ArtifactNames.CH_ARM_BINARY + "_GH"

    for name in (
        ArtifactNames.CH_AMD_DEBUG_GH,
        ArtifactNames.CH_AMD_BINARY_GH,
        ArtifactNames.CH_ARM_BINARY_GH,
        ArtifactNames.CH_AMD_TSAN_GH,
        ArtifactNames.CH_AMD_MSAN_GH,
        ArtifactNames.CH_AMD_ASAN_UBSAN_GH,
        ArtifactNames.CH_ARM_ASAN_UBSAN_GH,
        ArtifactNames.CH_ARM_MSAN_GH,
        ArtifactNames.CH_ARM_DEBUG_GH,
        ArtifactNames.CH_ARM_TSAN_GH,
    ):
        cfg = gh_by_name[name]
        assert cfg.type == Artifact.Type.GH
        fallback = name[: -len("_GH")]
        assert fallback in s3_names


def test_arm_ubsan_and_debug_debian_are_not_restored():
    from ci.defs.defs import ArtifactConfigs, ArtifactNames, BuildTypes
    from ci.defs.job_configs import JobConfigs

    assert not hasattr(BuildTypes, "ARM_UBSAN")
    assert not hasattr(ArtifactNames, "CH_ARM_UBSAN")
    assert not hasattr(ArtifactNames, "CH_ARM_UBSAN_GH")
    assert not hasattr(ArtifactNames, "DEB_AMD_DEBUG")
    assert "CH_ARM_UBSAN" not in {cfg.name for cfg in ArtifactConfigs.clickhouse_binaries}
    assert "DEB_AMD_DEBUG" not in {cfg.name for cfg in ArtifactConfigs.clickhouse_debians}
    assert all("arm_ubsan" not in job.name for job in JobConfigs.build_jobs)
    amd_debug = next(job for job in JobConfigs.build_jobs if "amd_debug" in job.name)
    assert "DEB_AMD_DEBUG" not in (amd_debug.provides or [])


def test_stripped_release_binaries_are_configured_and_provided():
    from ci.defs.defs import ArtifactConfigs, ArtifactNames, BINARIES_WITH_LONG_RETENTION
    from ci.defs.job_configs import JobConfigs

    configs = {cfg.name: cfg for cfg in ArtifactConfigs.clickhouse_stripped_binaries}
    for name in (
        ArtifactNames.CH_AMD_RELEASE_STRIPPED,
        ArtifactNames.CH_ARM_RELEASE_STRIPPED,
    ):
        cfg = configs[name]
        assert cfg.type == Artifact.Type.S3
        assert cfg.path.endswith("clickhouse-stripped")
        assert name in BINARIES_WITH_LONG_RETENTION

    amd_release = next(
        job for job in JobConfigs.release_build_jobs if "amd_release" in job.name
    )
    arm_release = next(
        job for job in JobConfigs.release_build_jobs if "arm_release" in job.name
    )
    assert ArtifactNames.CH_AMD_RELEASE_STRIPPED in amd_release.provides
    assert ArtifactNames.CH_ARM_RELEASE_STRIPPED in arm_release.provides


def test_enabled_workflows_using_stripped_binaries_import():
    import ci.workflows.pull_request
    import ci.workflows.fast_builds

    assert ci.workflows.pull_request.workflow
    assert ci.workflows.fast_builds.workflow
