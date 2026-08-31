"""
Regression guards for the Praktika AutoReleases workflow definition.

These pin the contract that ``ci/workflows/auto_releases.py`` driving
``ci/jobs/auto_release_job.py`` must keep:

  * every workflow-dispatch input the job reads is declared by the workflow,
  * the job dispatches the ``CreateRelease`` workflow as patch releases,
  * the legacy ``tests/ci/auto_release.py`` is gone.

The structural checks parse the sources with ``ast`` so they never import the
job (which pulls in praktika/boto3); the behavioural checks import it lazily
behind ``importorskip``. Altinity does not generate this workflow.
"""

import ast
import os
import re
import sys

import pytest

HERE = os.path.dirname(__file__)
REPO_ROOT = os.path.abspath(os.path.join(HERE, "../.."))
sys.path.insert(0, REPO_ROOT)

JOB = os.path.join(REPO_ROOT, "ci/jobs/auto_release_job.py")
WORKFLOW_DEF = os.path.join(REPO_ROOT, "ci/workflows/auto_releases.py")
LEGACY_JOB = os.path.join(REPO_ROOT, "tests/ci/auto_release.py")


def _read(path):
    with open(path, encoding="utf-8") as f:
        return f.read()


def _workflow_input_names():
    """Names declared as ``Workflow.Config.InputConfig(name=...)`` in the def."""
    names = set()
    for node in ast.walk(ast.parse(_read(WORKFLOW_DEF))):
        if isinstance(node, ast.Call):
            for kw in node.keywords:
                if kw.arg == "name" and isinstance(kw.value, ast.Constant):
                    names.add(kw.value.value)
    return names


def _inputs_read_by_job():
    """Input names the job reads via ``Info.get_workflow_input_value``."""
    return set(
        re.findall(r'get_workflow_input_value\(\s*["\']([a-z0-9-]+)["\']', _read(JOB))
    )


def test_workflow_declares_every_input_the_job_reads():
    declared = _workflow_input_names()
    read = _inputs_read_by_job()
    assert read, "auto_release_job.py should read a workflow-dispatch input"
    missing = read - declared
    assert not missing, (
        f"auto_release_job.py reads workflow inputs not declared in the workflow "
        f"definition: {sorted(missing)} (declared: {sorted(declared)})"
    )


def test_job_dispatches_create_release_as_patch():
    text = _read(JOB)
    assert "gh workflow run" in text
    assert "type=patch" in text, "autorelease only ever creates patch releases"


def test_job_references_real_create_release_workflow_file():
    """The dispatched workflow must be named by its actual YAML file.

    `gh workflow run` / `gh run list --workflow` resolve a `.yml` argument as a
    file name, so the workflow *name* "CreateRelease" is rejected with a 404 —
    the file is `create_release.yml`.
    """
    assert 'CREATE_RELEASE_WORKFLOW = "create_release.yml"' in _read(
        JOB
    ), "job must reference the create_release.yml file name (not CreateRelease.yml)"


def test_workflow_points_at_the_job():
    assert "./ci/jobs/auto_release_job.py" in _read(WORKFLOW_DEF)


def test_job_does_not_import_ci_defs():
    """The job runs with ``PYTHONPATH=.`` (see its workflow command), so it must
    not import ``ci.defs`` — that pulls in ``from praktika import ...`` which only
    resolves with ``./ci`` on the path, and would fail at runtime with
    ``ModuleNotFoundError: No module named 'praktika'``. Mirrors release_job.py,
    which keeps its runtime imports to ``ci.praktika.*`` for the same reason.
    """
    imported = set()
    for node in ast.walk(ast.parse(_read(JOB))):
        if isinstance(node, ast.ImportFrom) and node.module:
            imported.add(node.module)
        elif isinstance(node, ast.Import):
            imported.update(a.name for a in node.names)
    offending = {m for m in imported if m == "ci.defs" or m.startswith("ci.defs.")}
    assert (
        not offending
    ), f"job must not import {sorted(offending)} (breaks the PYTHONPATH=. run)"


def test_version_bump_guard_is_scoped_to_title():
    """The version-bump-PR guard must scope its search with ``in:title``.

    An unscoped full-text search matches any PR that merely mentions
    ``Update version_date.tsv`` in its body (the migration PR itself did), which
    would trip the guard and halt every release.
    """
    text = _read(JOB)
    assert (
        "Update version_date.tsv in:title" in text
    ), "guard search must be scoped with in:title to avoid body-only false positives"


def test_legacy_sources_are_gone():
    assert not os.path.exists(LEGACY_JOB), (
        "tests/ci/auto_release.py should be removed; its logic moved to "
        "ci/jobs/auto_release_job.py"
    )


# --- behavioural checks (import the job lazily) ------------------------------


def _job_module():
    pytest.importorskip("praktika")
    import ci.jobs.auto_release_job as m  # noqa: E402

    return m


def test_latest_release_tag_orders_by_version(tmp_path, monkeypatch):
    """``_latest_release_tag`` must pick v...10 over v...9 (numeric, not lexical).

    The legacy PyGithub lookup sorted refs lexically, which ranks ``v...10``
    below ``v...9``; the git ``--sort=v:refname`` port fixes that.
    """
    m = _job_module()
    repo = tmp_path / "repo"
    repo.mkdir()

    def git(cmd):
        import subprocess

        subprocess.run(
            [
                "git",
                "-c",
                "user.email=t@t",
                "-c",
                "user.name=t",
                "-c",
                "commit.gpgsign=false",
                *cmd,
            ],
            cwd=repo,
            check=True,
            capture_output=True,
        )

    git(["init", "-q"])
    git(["commit", "--allow-empty", "-qm", "c"])
    for tag in ("v24.3.9.1-lts", "v24.3.10.1-lts", "v24.3.2.1-lts"):
        git(["tag", tag])

    monkeypatch.chdir(repo)
    assert m._latest_release_tag("24.3") == "v24.3.10.1-lts"


def test_failed_statuses_keeps_newest_per_context(monkeypatch):
    """Only the latest status row per context decides pass/fail."""
    m = _job_module()
    rows = "\n".join(
        [
            "Build\tfailure\t2024-01-01T00:00:00Z",
            "Build\tsuccess\t2024-01-02T00:00:00Z",  # newer: Build now green
            "Tests\tsuccess\t2024-01-01T00:00:00Z",
            "Tests\tfailure\t2024-01-03T00:00:00Z",  # newer: Tests now red
        ]
    )
    monkeypatch.setattr(
        m.GH, "get_output_with_retries", staticmethod(lambda *a, **k: rows)
    )
    assert m._failed_statuses("deadbeef") == ["Tests"]


def test_status_reads_are_strict():
    """The `/statuses` and run-id reads must fail early, not return ``""``."""
    text = _read(JOB)
    for fn in ("_failed_statuses", "_latest_create_release_run_id"):
        body = text.split(f"def {fn}(", 1)[1].split("\ndef ", 1)[0]
        assert "get_output_with_retries" in body, f"{fn} should read via GH"
        assert "strict=True" in body, f"{fn} must read with strict=True (fail-close)"


def test_fetch_history_does_not_swallow_unshallow_failures(monkeypatch):
    """``--unshallow`` must fail early, and run only on a shallow clone."""
    m = _job_module()
    calls = []
    monkeypatch.setattr(
        m.Shell, "get_output_or_raise", staticmethod(lambda *a, **k: "true")
    )
    monkeypatch.setattr(
        m.Shell,
        "check",
        staticmethod(lambda cmd, **kwargs: calls.append((cmd, kwargs)) or True),
    )
    m._fetch_history()

    unshallow = [(cmd, kw) for cmd, kw in calls if "--unshallow" in cmd]
    assert len(unshallow) == 1, "shallow clone must be unshallowed exactly once"
    cmd, kwargs = unshallow[0]
    assert "||:" not in cmd, "a failing --unshallow must not be swallowed"
    assert kwargs.get("strict"), "--unshallow must run with strict=True"

    # A complete repository: `--unshallow` errors there, so it must be skipped.
    calls.clear()
    monkeypatch.setattr(
        m.Shell, "get_output_or_raise", staticmethod(lambda *a, **k: "false")
    )
    m._fetch_history()
    assert not [cmd for cmd, _ in calls if "--unshallow" in cmd]


def test_find_release_candidate_skips_new_branch(monkeypatch):
    """A branch whose latest tag ends with ``new`` is not auto-released."""
    m = _job_module()
    monkeypatch.setattr(m, "_latest_release_tag", lambda branch: "v25.8.1.1-new")
    sha, reason, status = m._find_release_candidate("25.8")
    assert sha == "" and "new release branch" in reason
    assert status == m.Result.Status.SKIPPED


def test_find_release_candidate_errors_without_release_tag(monkeypatch):
    """A missing ``v<branch>.*`` tag means broken metadata, not ``SKIPPED``."""
    m = _job_module()
    monkeypatch.setattr(m, "_latest_release_tag", lambda branch: None)
    sha, reason, status = m._find_release_candidate("25.8")
    assert sha == "" and "no release tag" in reason
    assert status == m.Result.Status.ERROR
