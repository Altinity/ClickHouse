#!/usr/bin/env python3
"""Unit tests for PRs-in-release baseline selection and merge filtering."""

import io
import subprocess
import tempfile
import unittest
from contextlib import redirect_stderr, redirect_stdout
from unittest.mock import MagicMock, patch

import pandas as pd

from create_workflow_report import (
    RELEASE_TAG_REF_PREFIX,
    _checks_latest_test_status_cte,
    _enrich_prs_in_release_merge_prs,
    _fetch_release_tags,
    _fetch_commit,
    _find_release_baseline,
    _get_github_releases,
    _git_log_merge_prs,
    _git_rev_parse,
    _parse_release_ref,
    _release_tag_sha,
    _filter_releases_to_version_line,
    get_prs_in_release_dataframe,
    get_workflow_config,
)

CURRENT_TAG = "v26.6.4.20001.altinityantalya"
PREV_TAG = "v26.6.2.20001.altinityantalya"
CURRENT_SHA = "aa" * 20
PREV_SHA = "bb" * 20
# Linear history: PREV_SHA is an ancestor of CURRENT_SHA (and of itself).
COMMIT_ORDER = {PREV_SHA: 0, CURRENT_SHA: 1}
REF_TO_SHA = {
    CURRENT_TAG: CURRENT_SHA,
    PREV_TAG: PREV_SHA,
    CURRENT_SHA: CURRENT_SHA,
    PREV_SHA: PREV_SHA,
    "antalya-26.6": CURRENT_SHA,
}

RELEASES = [
    {
        "draft": False,
        "tag_name": CURRENT_TAG,
        "published_at": "2026-09-11T14:02:51Z",
    },
    {
        "draft": False,
        "tag_name": PREV_TAG,
        "published_at": "2026-08-26T23:29:39Z",
    },
]


def _rev_parse(ref, cwd):
    return REF_TO_SHA.get(ref.removeprefix(RELEASE_TAG_REF_PREFIX))


def _is_ancestor(ancestor, descendant, cwd):
    a = REF_TO_SHA.get(ancestor, ancestor)
    d = REF_TO_SHA.get(descendant, descendant)
    if a not in COMMIT_ORDER or d not in COMMIT_ORDER:
        return False
    return COMMIT_ORDER[a] <= COMMIT_ORDER[d]


def _releases_response():
    response = MagicMock()
    response.status_code = 200
    response.json.return_value = RELEASES + [
        {"draft": True, "tag_name": "v26.9.1.1.altinityantalya"}
    ]
    return response


class TestGetGithubReleases(unittest.TestCase):
    def test_skips_drafts(self):
        with (
            patch("create_workflow_report.GITHUB_TOKEN", "token"),
            patch(
                "create_workflow_report.requests.get",
                return_value=_releases_response(),
            ),
        ):
            releases = _get_github_releases("Altinity/ClickHouse")
        self.assertEqual(releases, RELEASES)

    def test_no_token_returns_empty(self):
        with (
            patch("create_workflow_report.GITHUB_TOKEN", None),
            patch("builtins.print") as printed,
        ):
            self.assertEqual(_get_github_releases("Altinity/ClickHouse"), [])
        printed.assert_called_once()
        self.assertIn("GITHUB_TOKEN", printed.call_args.args[0])


class TestFindReleaseBaseline(unittest.TestCase):
    def test_tag_ref_skips_current_github_release(self):
        with (
            patch(
                "create_workflow_report._git_rev_parse",
                side_effect=_rev_parse,
            ),
            patch(
                "create_workflow_report._git_is_ancestor",
                side_effect=_is_ancestor,
            ),
        ):
            tag_name, tag_sha, published_date = _find_release_baseline(
                CURRENT_TAG, RELEASES, cwd=None, commit_sha=CURRENT_SHA
            )
        self.assertEqual(tag_name, PREV_TAG)
        self.assertEqual(tag_sha, PREV_SHA)
        self.assertEqual(published_date, "2026-08-26")

    def test_branch_ref_keeps_latest_github_release(self):
        with (
            patch(
                "create_workflow_report._git_rev_parse",
                side_effect=_rev_parse,
            ),
            patch(
                "create_workflow_report._git_is_ancestor",
                side_effect=_is_ancestor,
            ),
        ):
            tag_name, tag_sha, published_date = _find_release_baseline(
                "antalya-26.6", RELEASES, cwd=None, commit_sha=CURRENT_SHA
            )
        self.assertEqual(tag_name, CURRENT_TAG)
        self.assertEqual(tag_sha, CURRENT_SHA)
        self.assertEqual(published_date, "2026-09-11")

    def test_ancestry_uses_commit_not_branch_name(self):
        # Local antalya-26.6 may sit on the latest release; the run commit is older.
        with (
            patch(
                "create_workflow_report._git_rev_parse",
                side_effect=_rev_parse,
            ),
            patch(
                "create_workflow_report._git_is_ancestor",
                side_effect=_is_ancestor,
            ),
        ):
            tag_name, tag_sha, published_date = _find_release_baseline(
                "antalya-26.6", RELEASES, cwd=None, commit_sha=PREV_SHA
            )
        self.assertEqual(tag_name, PREV_TAG)
        self.assertEqual(tag_sha, PREV_SHA)
        self.assertEqual(published_date, "2026-08-26")


def _version_file(major, minor):
    return subprocess.CompletedProcess(
        [], 0, stdout=f"SET(VERSION_MAJOR {major})\nSET(VERSION_MINOR {minor})\n"
    )


class TestFetchCommit(unittest.TestCase):
    def test_full_clone_fetches_commit_without_clone_shape_flags(self):
        with (
            patch("create_workflow_report._git_is_shallow", return_value=False),
            patch("create_workflow_report._git_fetch") as fetch,
        ):
            _fetch_commit(CURRENT_SHA, ".")
        fetch.assert_called_once_with(["--no-tags"], [CURRENT_SHA], ".")

    def test_shallow_clone_unshallows_commit(self):
        with (
            patch("create_workflow_report._git_is_shallow", return_value=True),
            patch("create_workflow_report._git_fetch") as fetch,
        ):
            _fetch_commit(PREV_SHA, ".")
        fetch.assert_called_once_with(
            ["--filter=tree:0", "--unshallow"], [PREV_SHA], "."
        )


class TestReleaseTagSha(unittest.TestCase):
    def test_namespaced_only_ignores_local_tag(self):
        with patch(
            "create_workflow_report._git_rev_parse",
            side_effect=lambda ref, cwd: (
                "private" if ref.startswith(RELEASE_TAG_REF_PREFIX) else "local"
            ),
        ):
            self.assertEqual(_release_tag_sha(CURRENT_TAG, None), "private")
        with patch(
            "create_workflow_report._git_rev_parse",
            side_effect=lambda ref, cwd: (
                None if ref.startswith(RELEASE_TAG_REF_PREFIX) else "local"
            ),
        ):
            self.assertIsNone(_release_tag_sha(CURRENT_TAG, None))


class TestReleasesForVersionLine(unittest.TestCase):
    def test_keeps_flavour_and_minor_drops_others(self):
        mixed = RELEASES + [
            {
                "draft": False,
                "tag_name": "v25.8.33.10001.altinitystable",
                "published_at": "2026-09-01T00:00:00Z",
            },
            {
                "draft": False,
                "tag_name": "v26.6.8.10001.altinitystable",
                "published_at": "2026-10-01T00:00:00Z",
            },
        ]
        out = _filter_releases_to_version_line(mixed, "antalya-26.6", CURRENT_SHA, None)
        self.assertEqual([r["tag_name"] for r in out], [CURRENT_TAG, PREV_TAG])

    def test_odd_branch_reads_version_file(self):
        mixed = RELEASES + [
            {
                "draft": False,
                "tag_name": "v25.8.33.10001.altinitystable",
                "published_at": "2026-09-01T00:00:00Z",
            },
        ]
        with patch(
            "create_workflow_report._read_autogenerated_versions",
            return_value=(26, 6),
        ):
            out = _filter_releases_to_version_line(mixed, "odd-branch", CURRENT_SHA, None)
        self.assertEqual([r["tag_name"] for r in out], [CURRENT_TAG, PREV_TAG])


class TestFipsRefs(unittest.TestCase):
    def test_parses_fips_branch_and_tag(self):
        self.assertEqual(_parse_release_ref("fips-25.3"), ("fips", "25.3", None))
        self.assertEqual(
            _parse_release_ref("v25.3.8.30001.altinityfips"),
            ("fips", "25.3", "25.3.8.30001"),
        )

    def test_fips_branch_keeps_only_fips_tags(self):
        fips_tag = "v25.3.8.30001.altinityfips"
        releases = [
            {"draft": False, "tag_name": fips_tag},
            {"draft": False, "tag_name": "v25.3.8.20001.altinityantalya"},
            {"draft": False, "tag_name": "v25.3.8.10001.altinitystable"},
        ]
        out = _filter_releases_to_version_line(releases, "fips-25.3", CURRENT_SHA, None)
        self.assertEqual([r["tag_name"] for r in out], [fips_tag])


class TestFetchReleaseTagsWarning(unittest.TestCase):
    def test_missing_tag_is_reported(self):
        def _fetch(options, refspecs, cwd):
            if CURRENT_TAG in refspecs[0]:
                raise Exception("git fetch failed: not found")

        with (
            patch("create_workflow_report._git_fetch", side_effect=_fetch),
            patch("builtins.print") as printed,
        ):
            _fetch_release_tags(RELEASES, ".")
        message = printed.call_args.args[0]
        self.assertIn(CURRENT_TAG, message)
        self.assertIn("not found", message)


class TestGitRevParse(unittest.TestCase):
    def test_missing_full_sha_is_unresolved(self):
        with tempfile.TemporaryDirectory() as cwd:
            subprocess.run(["git", "init", "-q"], cwd=cwd, check=True)
            subprocess.run(
                [
                    "git",
                    "-c",
                    "user.name=t",
                    "-c",
                    "user.email=t@t",
                    "commit",
                    "-q",
                    "--allow-empty",
                    "-m",
                    "init",
                ],
                cwd=cwd,
                check=True,
            )
            head = _git_rev_parse("HEAD", cwd)
            self.assertRegex(head, r"^[0-9a-f]{40}$")
            self.assertEqual(_git_rev_parse(head, cwd), head)
            self.assertIsNone(_git_rev_parse("deadbeef" * 5, cwd))


class TestGetPrsInReleaseCommitResolve(unittest.TestCase):
    def test_raises_when_commit_cannot_resolve(self):
        with (
            patch("create_workflow_report._fetch_commit"),
            patch("create_workflow_report._git_rev_parse", return_value=None),
        ):
            with self.assertRaises(Exception) as ctx:
                get_prs_in_release_dataframe(
                    "antalya-26.6", cwd=".", commit_sha="deadbeef" * 5
                )
        self.assertIn("Cannot resolve commit", str(ctx.exception))
        self.assertIn("deadbeef", str(ctx.exception))


class TestGitLogMergePrs(unittest.TestCase):
    def test_includes_fork_and_altinity_omits_upstream(self):
        stdout = "\n".join(
            [
                f"{'e' * 40}\tMerge pull request #2226 from strtgbb/community-pr-failure-artifacts",
                f"{'a' * 40}\tMerge pull request #113355 from ClickHouse/backport/26.6/112784",
                f"{'b' * 40}\tMerge pull request #2252 from Altinity/ci/antalya-26.6/container-dns",
            ]
        )
        with patch(
            "create_workflow_report.subprocess.run",
            return_value=subprocess.CompletedProcess([], 0, stdout=stdout),
        ):
            df = _git_log_merge_prs("baseline", "branch", None, "Altinity/ClickHouse")
        self.assertEqual(set(df["pr_number"]), {2226, 2252})


def _pr_response(status_code, title=""):
    response = MagicMock()
    response.status_code = status_code
    response.text = "Not Found" if status_code == 404 else "error"
    response.json.return_value = {"title": title, "labels": []}
    return response


class TestEnrichPrsInReleaseMergePrs(unittest.TestCase):
    def test_skips_404_keeps_ours(self):
        # Upstream PR merged from a personal fork (head_owner != ClickHouse)
        # is not in Altinity/ClickHouse; 404 means skip, not fail.
        df = pd.DataFrame(
            {
                "pr_number": [114814, 2287],
                "merge_commit_sha": ["a" * 40, "b" * 40],
                "merge_subject": [
                    "Merge pull request #114814 from groeneai/groeneai/backport-112327-25.8",
                    "Merge pull request #2287 from Altinity/bump/antalya-25.8/25.8.33",
                ],
            }
        )

        def _get(url, headers=None):
            if url.endswith("/114814"):
                return _pr_response(404)
            return _pr_response(200, title="bump 25.8.33")

        with (
            patch("create_workflow_report.GITHUB_TOKEN", "token"),
            patch("create_workflow_report.requests.get", side_effect=_get),
        ):
            out = _enrich_prs_in_release_merge_prs(df, "Altinity/ClickHouse")
        self.assertEqual(list(out["pr_number"]), [2287])
        self.assertEqual(list(out["pr_name"]), ["bump 25.8.33"])

    def test_all_404s_returns_empty(self):
        df = pd.DataFrame(
            {
                "pr_number": [114814],
                "merge_commit_sha": ["a" * 40],
                "merge_subject": [
                    "Merge pull request #114814 from groeneai/groeneai/backport-112327-25.8",
                ],
            }
        )
        with (
            patch("create_workflow_report.GITHUB_TOKEN", "token"),
            patch(
                "create_workflow_report.requests.get",
                return_value=_pr_response(404),
            ),
        ):
            out = _enrich_prs_in_release_merge_prs(df, "Altinity/ClickHouse")
        self.assertEqual(len(out), 0)
        self.assertEqual(list(out.columns), ["pr_number", "pr_name", "pr_labels"])

    def test_non_404_error_still_raises(self):
        df = pd.DataFrame(
            {
                "pr_number": [2287],
                "merge_commit_sha": ["b" * 40],
                "merge_subject": [
                    "Merge pull request #2287 from Altinity/bump/antalya-25.8/25.8.33",
                ],
            }
        )
        with (
            patch("create_workflow_report.GITHUB_TOKEN", "token"),
            patch(
                "create_workflow_report.requests.get",
                return_value=_pr_response(500),
            ),
        ):
            with self.assertRaises(Exception) as ctx:
                _enrich_prs_in_release_merge_prs(df, "Altinity/ClickHouse")
        self.assertIn("500", str(ctx.exception))


class TestChecksLatestTestStatusCte(unittest.TestCase):
    def test_anchor_excludes_hooks_and_empty_test_name(self):
        sql = _checks_latest_test_status_cte("deadbeef", "antalya-25.8")
        self.assertIn("test_name != ''", sql)
        self.assertIn("NOT IN ('Pre Hooks', 'Post Hooks')", sql)


class TestGetWorkflowConfigStdout(unittest.TestCase):
    def tearDown(self):
        get_workflow_config.cache_clear()

    def test_s3_404_warning_goes_to_stderr(self):
        # action.yml takes the script's stdout as the report URL. A WARNING on
        # stdout makes grep '^https?://' fail even after a successful upload.
        mock_resp = MagicMock()
        mock_resp.status_code = 404
        stdout = io.StringIO()
        stderr = io.StringIO()
        with (
            patch(
                "create_workflow_report._workflow_config_from_local",
                return_value=None,
            ),
            patch("create_workflow_report.requests.get", return_value=mock_resp),
            redirect_stdout(stdout),
            redirect_stderr(stderr),
        ):
            cfg = get_workflow_config(2719, "abc", "antalya-25.8")
        self.assertEqual(cfg, {"cache_jobs": {}})
        self.assertEqual(stdout.getvalue(), "")
        self.assertIn("WARNING", stderr.getvalue())


if __name__ == "__main__":
    unittest.main()
