#!/usr/bin/env python3
"""
Read the JSON output from check_for_flaky_fixes.py and cherry-pick each missing
commit onto a backport branch. Opens a new PR, or amends the open one from a
previous run if it already exists for this base branch.
"""

import argparse
import json
import re
import subprocess
import sys
from datetime import datetime, timezone


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


def prefetch_upstream_objects(shas: list) -> None:
    """Blobless-fetch the target commits and their parents from the 'upstream'
    promisor remote (configured by the workflow) so `git cherry-pick` has the
    commit and its merge base locally and can resolve blobs on demand.
    --depth=2 pulls each commit plus its parent without the full ancestry (a plain
    fetch into a shallow clone drags the entire history), in a single cheap
    negotiation. No-op when no 'upstream' remote exists (e.g. local runs on a full
    clone)."""
    if not shas or "upstream" not in git_out("remote").split():
        print("No 'upstream' remote; skipping prefetch (assuming objects are local).", file=sys.stderr)
        return
    run_git("fetch", "--depth=2", "--no-tags", "upstream", *shas, check=False)


def cherry_pick(sha: str) -> bool:
    """Cherry-pick a commit. Blobs are lazily fetched from the upstream promisor
    remote by full sha via the parent trees pulled by prefetch_upstream_objects.
    Returns True on success, False on conflict or failure (which is aborted)."""
    result = run_git("cherry-pick", "-x", sha, check=False)
    if result.returncode == 0:
        return True
    print(f"  cherry-pick of {sha[:12]} failed, aborting:\n{result.stdout}{result.stderr}", file=sys.stderr)
    run_git("cherry-pick", "--abort", check=False)
    return False


def _commit_line(upstream_repo: str, sha, subject: str, date) -> str:
    """Render one bullet. sha/date may be None for commits carried over from a
    reused PR, where only the subject is known."""
    link = ""
    if sha:
        url = UPSTREAM_COMMIT_URL.format(repo=upstream_repo, sha=sha)
        link = f"[`{sha[:12]}`]({url}) "
    suffix = f"  _(committed {date})_" if date else ""
    return f"- {link}{subject}{suffix}"


def build_pr_body(
    upstream_repo: str,
    applied: list,
    conflicted: list,
) -> str:
    lines = []
    lines.append("Automated backport of upstream flaky-fix commits.")
    lines.append("")
    lines.append("- [x] <!---ci_exclude_regression--> Exclude all Regression")
    lines.append("")

    if applied:
        lines.append("### Applied")
        lines.append("")
        for sha, subject, date in applied:
            lines.append(_commit_line(upstream_repo, sha, subject, date))
        lines.append("")

    if conflicted:
        lines.append("### Skipped (cherry-pick conflict — manual backport needed)")
        lines.append("")
        for sha, subject, date in conflicted:
            lines.append(_commit_line(upstream_repo, sha, subject, date))
        lines.append("")

    return "\n".join(lines)


def find_open_backport_pr(repo: str, base_branch: str):
    """Return (number, head_branch) of an open flaky-fix backport PR for this base
    branch, or (None, None). Lets a weekly run amend last week's PR instead of
    opening a fresh one each time."""
    prefix = f"flaky-fix-backport/{base_branch}/"
    result = subprocess.run(
        ["gh", "pr", "list", "--repo", repo, "--state", "open", "--base", base_branch,
         "--json", "number,headRefName", "--limit", "100"],
        text=True, capture_output=True,
    )
    if result.returncode != 0:
        print(f"Warning: gh pr list failed, will open a new PR:\n{result.stderr}", file=sys.stderr)
        return None, None
    for pr in json.loads(result.stdout):
        if pr["headRefName"].startswith(prefix):
            return pr["number"], pr["headRefName"]
    return None, None


def pr_backport_commits(repo: str, number: int) -> list:
    """Return [(upstream_sha_or_None, subject)] for commits already on the PR,
    oldest-first, by reading the `(cherry picked from commit <sha>)` trailer that
    `cherry-pick -x` records."""
    result = subprocess.run(
        ["gh", "pr", "view", str(number), "--repo", repo, "--json", "commits"],
        text=True, capture_output=True,
    )
    if result.returncode != 0:
        print(f"Warning: could not read commits of PR #{number}:\n{result.stderr}", file=sys.stderr)
        return []
    out = []
    for c in json.loads(result.stdout).get("commits", []):
        body = c.get("messageBody", "") or ""
        m = re.search(r"cherry picked from commit ([0-9a-f]{7,40})", body)
        out.append((m.group(1) if m else None, c.get("messageHeadline", "")))
    return out


def update_pr(repo: str, number: int, title: str, body: str, labels: list, dry_run: bool) -> None:
    if dry_run:
        print(f"DRY RUN: would update PR #{number} in {repo}:", file=sys.stderr)
        print(f"  title:  {title}", file=sys.stderr)
        print(f"  labels: {', '.join(labels)}", file=sys.stderr)
        return

    labels = existing_labels(repo, labels)

    cmd = [
        "gh", "pr", "edit", str(number),
        "--repo", repo,
        "--title", title,
        "--body", body,
    ]
    for label in labels:
        cmd += ["--add-label", label]

    result = subprocess.run(cmd, text=True, capture_output=False)
    if result.returncode != 0:
        print("gh pr edit failed", file=sys.stderr)
        sys.exit(1)


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
        help="JSON output file produced by check_for_flaky_fixes.py",
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

    # Reuse an open PR from a previous run if one exists, so we amend a single
    # rolling PR per base branch instead of opening a new one every week.
    reuse_number, reuse_branch = find_open_backport_pr(args.repo, base_branch)

    prior_applied = []
    if reuse_branch:
        print(f"Reusing open PR #{reuse_number} ({reuse_branch}).", file=sys.stderr)
        prior_commits = pr_backport_commits(args.repo, reuse_number)
        prior_shas = {sha for sha, _ in prior_commits if sha}
        prior_applied = [(sha, subject, None) for sha, subject in prior_commits]
        missing = [c for c in missing if c["sha"] not in prior_shas]
        if not missing:
            print(f"PR #{reuse_number} already contains all missing commits. Nothing to do.", file=sys.stderr)
            return

    prefetch_upstream_objects([c["sha"] for c in missing])

    date_tag = datetime.now(timezone.utc).strftime("%Y-%m-%d")

    if reuse_branch:
        backport_branch = reuse_branch
        run_git("fetch", "--depth=1", "--no-tags", "origin", reuse_branch)
        run_git("checkout", "-B", backport_branch, "FETCH_HEAD")
    else:
        backport_branch = f"flaky-fix-backport/{base_branch}/{date_tag}"
        existing = run_git("branch", "--list", backport_branch).stdout.strip()
        if existing:
            print(f"Branch {backport_branch} already exists. Delete it first.", file=sys.stderr)
            sys.exit(1)
        run_git("checkout", "-b", backport_branch)

    applied = []
    conflicted = []

    for commit in missing:
        sha = commit["sha"]
        subject = commit["subject"]
        date = commit["date"]
        print(f"Cherry-picking {sha[:12]}: {subject}", file=sys.stderr)
        if cherry_pick(sha):
            applied.append((sha, subject, date))
            print(f"  Applied {sha[:12]}", file=sys.stderr)
        else:
            conflicted.append((sha, subject, date))
            print(f"  Skipped {sha[:12]} (conflict)", file=sys.stderr)

    if not reuse_branch and not applied:
        print("No commits could be applied (all conflicted). Cleaning up.", file=sys.stderr)
        run_git("checkout", base_branch)
        run_git("branch", "-D", backport_branch)
        sys.exit(0)

    all_applied = prior_applied + applied
    pr_title = f"{base_branch.title()} - Backport flaky-fix commits from upstream ({date_tag})"
    pr_body = build_pr_body(upstream_repo, all_applied, conflicted)
    pr_labels = labels_for_branch(base_branch)

    if args.dry_run:
        action = f"amend PR #{reuse_number}" if reuse_branch else f"open PR against {base_branch}"
        print(
            f"DRY RUN: would push {backport_branch} and {action}.",
            file=sys.stderr,
        )
        print(f"  Newly applied: {len(applied)}  Carried over: {len(prior_applied)}  Conflicted: {len(conflicted)}", file=sys.stderr)
        print(f"\n--- PR title ---\n{pr_title}\n--- PR body ---\n{pr_body}---")
        if reuse_branch:
            update_pr(repo=args.repo, number=reuse_number, title=pr_title, body=pr_body, labels=pr_labels, dry_run=True)
        else:
            create_pr(repo=args.repo, branch=backport_branch, base=base_branch, title=pr_title, body=pr_body, labels=pr_labels, dry_run=True)
        run_git("checkout", base_branch)
        run_git("branch", "-D", backport_branch)
        return

    # Only push when we added commits; body-only refreshes still update the PR.
    if applied:
        run_git("push", "origin", backport_branch, capture=False)

    if reuse_branch:
        update_pr(repo=args.repo, number=reuse_number, title=pr_title, body=pr_body, labels=pr_labels, dry_run=False)
    else:
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
