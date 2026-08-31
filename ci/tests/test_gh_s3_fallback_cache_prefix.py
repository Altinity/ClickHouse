"""
GH→S3 artifact fallback must reuse the cached build SHA prefix.

When Config marks a Build as cache_success, Build is skipped and no per-run
GitHub artifact exists. The fallback downloads the matching S3 artifact, which
lives under the cached SHA (not the current commit). Using env.get_s3_prefix()
looks under the wrong path and leaves tests without a binary.

Caused by: https://github.com/Altinity/ClickHouse/actions/runs/30660314206
"""

import os
import sys
from pathlib import Path
from unittest.mock import MagicMock

import pytest

sys.path.insert(0, os.path.join(os.path.dirname(__file__), "../.."))

from ci.praktika.artifact import Artifact
from ci.praktika.cache import Cache
from ci.praktika.job import Job
from ci.praktika.runner import Runner
from ci.praktika.runtime import RunConfig
from ci.praktika.settings import Settings
from ci.praktika.workflow import Workflow


@pytest.fixture
def input_dir(tmp_path, monkeypatch):
    monkeypatch.setattr(Settings, "INPUT_DIR", str(tmp_path))
    monkeypatch.setattr(Settings, "TEMP_DIR", str(tmp_path))
    monkeypatch.setattr(Settings, "OUTPUT_DIR", str(tmp_path))
    monkeypatch.setattr(Settings, "S3_ARTIFACT_PATH", "altinity-build-artifacts")
    return tmp_path


def test_gh_s3_fallback_uses_cached_sha_prefix(input_dir, monkeypatch):
    cached_sha = "14f4c814a0e5be6b26cc33f8a15e10575e0fbf84"
    current_sha = "6cfd1e62db855762a761c2b3b6efcbba478ffbe4"

    s3_artifact = Artifact.Config(
        name="CH_AMD_DEBUG",
        type=Artifact.Type.S3,
        path="clickhouse",
        _provided_by="Build (amd_debug)",
    )
    gh_artifact = Artifact.Config(
        name="CH_AMD_DEBUG_GH",
        type=Artifact.Type.GH,
        path="clickhouse",
        _provided_by="Build (amd_debug)",
    )
    build_job = Job.Config(
        name="Build (amd_debug)",
        runs_on=[],
        command="",
        provides=["CH_AMD_DEBUG", "CH_AMD_DEBUG_GH"],
    )
    test_job = Job.Config(
        name="Stateless tests (amd_debug, parallel)",
        runs_on=[],
        command="",
        requires=["CH_AMD_DEBUG_GH"],
    )
    workflow = Workflow.Config(
        name="PR",
        event=Workflow.Event.PULL_REQUEST,
        jobs=[build_job, test_job],
        artifacts=[s3_artifact, gh_artifact],
        enable_cache=True,
        enable_report=False,
    )

    env = MagicMock()
    env.JOB_NAME = test_job.name
    env.WORKFLOW_JOB_DATA = None
    env.PR_NUMBER = 2107
    env.BRANCH = "bump/antalya-26.6/26.6.2.81"
    env.SHA = current_sha
    env.get_s3_prefix.return_value = f"PRs/2107/{current_sha}"
    env.get_s3_prefix_static.side_effect = (
        lambda pr, branch, sha, latest=False: f"PRs/{pr}/{sha}"
    )

    cache_record = Cache.CacheRecord(
        type=Cache.CacheRecord.Type.SUCCESS,
        sha=cached_sha,
        pr_number=2107,
        branch="bump/antalya-26.6/26.6.2.81",
        workflow="PR",
    )
    run_config = RunConfig(
        name="PR",
        digest_jobs={},
        digest_dockers={},
        cache_success=["Build (amd_debug)"],
        cache_success_base64=[],
        cache_artifacts={"CH_AMD_DEBUG": cache_record, "CH_AMD_DEBUG_GH": cache_record},
        cache_jobs={"Build (amd_debug)": cache_record},
        filtered_jobs={},
        sha=current_sha,
        submodule_cache_hash="",
        custom_data={},
    )

    downloaded = []

    def fake_copy_from_s3(s3_path, local_path, recursive=False, include_pattern="", **_):
        downloaded.append(s3_path)
        Path(local_path).mkdir(parents=True, exist_ok=True)
        (Path(local_path) / "clickhouse").write_text("binary")
        return True

    monkeypatch.setattr("ci.praktika.runner._Environment.get", lambda: env)
    monkeypatch.setattr("ci.praktika.runner.Shell.get_output", lambda *a, **k: "")
    monkeypatch.setattr("ci.praktika.runner.Result.dump", lambda self: self)
    monkeypatch.setattr(
        "ci.praktika.hook_cache.RunConfig.from_workflow_data", lambda: run_config
    )
    monkeypatch.setattr("ci.praktika.runner.S3.copy_file_from_s3", fake_copy_from_s3)

    assert Runner()._pre_run(workflow, test_job) == 0

    assert downloaded == [
        f"altinity-build-artifacts/PRs/2107/{cached_sha}/build_amd_debug/clickhouse"
    ]
    env.get_s3_prefix.assert_not_called()
