#!/usr/bin/env python3
"""
Read the JSON output from check_for_flaky_fix_backports.py, cherry-pick each
missing commit onto a new branch, push it, and open a single PR.
"""

import argparse
import json
import re
import subprocess
import sys
from datetime import datetime, timezone


UPSTREAM_GIT_URL = "https://github.com/{repo}.git"
UPSTREAM_COMMIT_URL = "https://github.com/{repo}/commit/{sha}"

# Branches like antalya-26.3 or stable-25.8 → family label + version label.
_BRANCH_LABEL_RE = re.compile(r'^([a-z]+)-(\d+\.\d+)$')


def labels_for_branch(branch: str) -> list:
    labels = ["cicd"]
    m = _BRANCH_LABEL_RE.match(branch)
    if m:
        labels.append(m.group(1))   # e.g. "antalya" or "stable"
        labels.append(branch)        # e.g. "antalya-26.3" or "stable-25.8"
    return labels


def existing_labels(repo: str, labels: list) -> list:
    """Return the subset of labels that already exist in the repo."""
    result = subprocess.run(
        ["gh", "label", "list", "--repo", repo, "--json", "name", "--limit", "1000"],
        text=True,
        capture_output=True,
    )
    if result.returncode != 0:
        print(f"Warning: could not fetch labels from {repo}, skipping labels", file=sys.stderr)
        return []
    known = {entry["name"] for entry in json.loads(result.stdout)}
    skipped = [l for l in labels if l not in known]
    if skipped:
        print(f"Skipping non-existent labels: {', '.join(skipped)}", file=sys.stderr)
    return [l for l in labels if l in known]


def run_git(*args, check=True, capture=True) -> subprocess.CompletedProcess:
    result = subprocess.run(
        ["git"] + list(args),
        text=True,
        capture_output=capture,
    )
    if check and result.returncode != 0:
        print(f"git {' '.join(args)} failed:\n{result.stderr}", file=sys.stderr)
        sys.exit(1)
    return result


def git_out(*args) -> str:
    return run_git(*args).stdout.strip()


def fetch_commit(upstream_repo: str, sha: str) -> bool:
    """Fetch a commit and its parent from upstream so cherry-pick can compute the diff.
    Returns True on success, False on failure."""
    url = UPSTREAM_GIT_URL.format(repo=upstream_repo)
    result = run_git("fetch", "--no-tags", "--no-recurse-submodules", url, sha, check=False)
    if result.returncode != 0:
        print(f"Failed to fetch {sha[:12]} from upstream:\n{result.stderr}", file=sys.stderr)
        return False
    # cherry-pick needs the parent's tree objects to compute the diff; fetch it too.
    parent_result = run_git("rev-parse", "FETCH_HEAD^", check=False)
    if parent_result.returncode == 0:
        parent_sha = parent_result.stdout.strip()
        run_git("fetch", "--no-tags", "--no-recurse-submodules", "--depth=1", url, parent_sha, check=False)
    return True


def cherry_pick(sha: str) -> bool:
    """Attempt cherry-pick. Returns True on success, False on conflict."""
    result = run_git("cherry-pick", "-x", sha, check=False)
    if result.returncode == 0:
        return True
    print(f"Cherry-pick of {sha[:12]} failed (conflict), aborting.", file=sys.stderr)
    run_git("cherry-pick", "--abort", check=False)
    return False


def build_pr_body(
    upstream_repo: str,
    applied: list,
    conflicted: list,
    fetch_failed: list,
) -> str:
    lines = []
    lines.append("Automated backport of upstream flaky-fix commits.")
    lines.append("")
    lines.append("- [x] <!---ci_exclude_regression--> Exclude all Regression")
    lines.append("")

    if applied:
        lines.append("### Cherry-picked")
        lines.append("")
        for sha, subject, date in applied:
            url = UPSTREAM_COMMIT_URL.format(repo=upstream_repo, sha=sha)
            lines.append(f"- [`{sha[:12]}`]({url}) {subject}  _(committed {date})_")
        lines.append("")

    if conflicted:
        lines.append("### Skipped (cherry-pick conflict — manual backport needed)")
        lines.append("")
        for sha, subject, date in conflicted:
            url = UPSTREAM_COMMIT_URL.format(repo=upstream_repo, sha=sha)
            lines.append(f"- [`{sha[:12]}`]({url}) {subject}  _(committed {date})_")
        lines.append("")

    if fetch_failed:
        lines.append("### Skipped (fetch failed — commit may not be reachable)")
        lines.append("")
        for sha, subject, date in fetch_failed:
            url = UPSTREAM_COMMIT_URL.format(repo=upstream_repo, sha=sha)
            lines.append(f"- [`{sha[:12]}`]({url}) {subject}  _(committed {date})_")
        lines.append("")

    return "\n".join(lines)


def create_pr(repo: str, branch: str, base: str, title: str, body: str, labels: list, dry_run: bool) -> None:
    if dry_run:
        print(f"DRY RUN: would open PR in {repo}:", file=sys.stderr)
        print(f"  title:  {title}", file=sys.stderr)
        print(f"  base:   {base}", file=sys.stderr)
        print(f"  head:   {branch}", file=sys.stderr)
        print(f"  labels: {', '.join(labels)}", file=sys.stderr)
        return

    labels = existing_labels(repo, labels)

    cmd = [
        "gh", "pr", "create",
        "--repo", repo,
        "--base", base,
        "--head", branch,
        "--title", title,
        "--body", body,
    ]
    for label in labels:
        cmd += ["--label", label]

    result = subprocess.run(cmd, text=True, capture_output=False)
    if result.returncode != 0:
        print("gh pr create failed", file=sys.stderr)
        sys.exit(1)


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Cherry-pick missing flaky-fix commits and open a backport PR.",
        formatter_class=argparse.ArgumentDefaultsHelpFormatter,
    )
    parser.add_argument(
        "--json-file",
        required=True,
        metavar="PATH",
        help="JSON output file produced by check_flaky_fix_backports.py",
    )
    parser.add_argument(
        "--repo",
        default="Altinity/ClickHouse",
        metavar="OWNER/REPO",
        help="target GitHub repository where the PR will be opened",
    )
    parser.add_argument(
        "--dry-run",
        action="store_true",
        help="fetch and cherry-pick locally but do not push or open a PR",
    )
    return parser.parse_args()


def main() -> None:
    args = parse_args()

    with open(args.json_file, encoding="utf-8") as f:
        data = json.load(f)

    missing = data.get("missing", [])
    if not missing:
        print("No missing commits in JSON file. Nothing to do.", file=sys.stderr)
        return

    upstream_repo = data["upstream_repo"]
    base_branch = data["branch"]

    # Sort oldest-first so cherry-picks apply in chronological order.
    missing.sort(key=lambda c: c["date"])

    date_tag = datetime.now(timezone.utc).strftime("%Y-%m-%d")
    backport_branch = f"flaky-fix-backport/{base_branch}/{date_tag}"

    existing = run_git("branch", "--list", backport_branch).stdout.strip()
    if existing:
        print(f"Branch {backport_branch} already exists. Delete it first.", file=sys.stderr)
        sys.exit(1)

    run_git("checkout", "-b", backport_branch)

    applied = []
    conflicted = []
    fetch_failed = []

    for commit in missing:
        sha = commit["sha"]
        subject = commit["subject"]
        date = commit["date"]
        print(f"Fetching {sha[:12]}: {subject}", file=sys.stderr)
        if not fetch_commit(upstream_repo, sha):
            fetch_failed.append((sha, subject, date))
            print(f"  Skipped {sha[:12]} (fetch failed)", file=sys.stderr)
            continue
        if cherry_pick(sha):
            applied.append((sha, subject, date))
            print(f"  Applied {sha[:12]}", file=sys.stderr)
        else:
            conflicted.append((sha, subject, date))
            print(f"  Skipped {sha[:12]} (conflict)", file=sys.stderr)

    if not applied:
        print("No commits could be applied (all conflicted or failed to fetch). Cleaning up.", file=sys.stderr)
        run_git("checkout", base_branch)
        run_git("branch", "-D", backport_branch)
        sys.exit(0)

    pr_title = f"{base_branch.title()} - Backport flaky-fix commits from upstream ({date_tag})"
    pr_body = build_pr_body(upstream_repo, applied, conflicted, fetch_failed)
    pr_labels = labels_for_branch(base_branch)

    if args.dry_run:
        print(
            f"DRY RUN: would push {backport_branch} and open PR against {base_branch}.",
            file=sys.stderr,
        )
        print(f"  Applied: {len(applied)}  Conflicted: {len(conflicted)}  Fetch failed: {len(fetch_failed)}", file=sys.stderr)
        print(f"\n--- PR title ---\n{pr_title}\n--- PR body ---\n{pr_body}---")
        create_pr(repo=args.repo, branch=backport_branch, base=base_branch, title=pr_title, body=pr_body, labels=pr_labels, dry_run=True)
        run_git("checkout", base_branch)
        run_git("branch", "-D", backport_branch)
        return

    run_git("push", "origin", backport_branch, capture=False)

    create_pr(
        repo=args.repo,
        branch=backport_branch,
        base=base_branch,
        title=pr_title,
        body=pr_body,
        labels=pr_labels,
        dry_run=False,
    )


if __name__ == "__main__":
    main()
