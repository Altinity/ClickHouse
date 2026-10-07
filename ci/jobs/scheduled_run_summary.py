#!/usr/bin/env python3
"""Post one Slack message for a nightly CI run that has failures.

Called from the workflow_run job. Exits without posting when the finished run
was not dispatched by the midnight schedule, or when none of the tracked job
groups failed.
"""

import json
import os
import subprocess
import sys
import tempfile
import urllib.parse
import urllib.request
from pathlib import Path

GROUPS = (
    ("Build", "Build ("),
    ("Stateless", "Stateless tests"),
    ("Stateful", "Stateful tests"),
    ("Integration", "Integration tests"),
    ("Regression", "Regression"),
    ("Grype", "GrypeScan"),
)


def gh(path, params=None):
    if params:
        path += "?" + urllib.parse.urlencode(params)
    raw = subprocess.check_output(["gh", "api", path], text=True)
    return json.loads(raw)


def jobs(repo, run_id):
    found = []
    page = 1
    while page <= 20:
        payload = gh(
            f"/repos/{repo}/actions/runs/{run_id}/jobs",
            {"per_page": 100, "page": page, "filter": "latest"},
        )
        chunk = payload.get("jobs") or []
        found.extend(chunk)
        if not chunk or len(found) >= (payload.get("total_count") or 0):
            break
        page += 1
    return found


def failed_by_group(job_list):
    grouped = {label: [] for label, _prefix in GROUPS}
    for job in job_list:
        if job.get("conclusion") != "failure":
            continue
        name = job.get("name") or ""
        for label, prefix in GROUPS:
            if name.startswith(prefix):
                grouped[label].append(name)
                break
    return grouped


def fail_suffix(names, previous_names):
    if previous_names is None:
        return ""
    new = len(set(names) - set(previous_names))
    if new == 0:
        return ", same as previous"
    return f", {new} new"


def format_message(branch, sha, run_id, grouped, previous):
    """previous is None when this branch has no earlier completed run."""
    lines = []
    for label, _prefix in GROUPS:
        names = grouped[label]
        if not names:
            continue
        suffix = fail_suffix(names, None if previous is None else previous.get(label, []))
        lines.append(f"{label}: {len(names)} failed{suffix}")
    if not lines:
        return None
    short = (sha or "")[:7]
    header = f"{branch} · {short}" if short else branch
    report = (
        "https://s3.amazonaws.com/altinity-build-artifacts/"
        f"REFs/{branch}/{sha}/{run_id}/ci_run_report.html"
    )
    return "\n".join([header, *lines, f"Report: {report}"])


def previous_groups(repo, previous_run_id):
    """Failures from the run id saved when this nightly run was dispatched."""
    if not previous_run_id:
        return None
    return failed_by_group(jobs(repo, previous_run_id))


def find_entry(repo, run_id):
    """Load the dispatch record uploaded as batch-entry-<run_id>."""
    name = f"batch-entry-{run_id}"
    payload = gh(f"/repos/{repo}/actions/artifacts", {"name": name, "per_page": 5})
    artifact = next(
        (
            item
            for item in payload.get("artifacts") or []
            if item.get("name") == name and not item.get("expired")
        ),
        None,
    )
    if artifact is None:
        return None
    with tempfile.TemporaryDirectory() as tmp:
        subprocess.run(
            [
                "gh", "run", "download", str(artifact["workflow_run"]["id"]),
                "-n", name,
                "-D", tmp,
            ],
            check=True,
            capture_output=True,
            text=True,
        )
        path = next(Path(tmp).rglob("*.json"), None)
        if path is None:
            raise RuntimeError(f"{name} downloaded but contained no json")
        return json.loads(path.read_text(encoding="utf-8"))


def write_summary(text):
    path = os.environ.get("GITHUB_STEP_SUMMARY")
    if not path:
        return
    with open(path, "a", encoding="utf-8") as handle:
        handle.write(text)
        if not text.endswith("\n"):
            handle.write("\n")


def post_slack(text):
    body = json.dumps({
        "channel": os.environ["SLACK_CHANNEL_ID"],
        "text": text,
    }).encode("utf-8")
    request = urllib.request.Request(
        "https://slack.com/api/chat.postMessage",
        data=body,
        headers={
            "Authorization": f"Bearer {os.environ['SLACK_BOT_TOKEN']}",
            "Content-Type": "application/json; charset=utf-8",
        },
    )
    with urllib.request.urlopen(request, timeout=30) as response:
        payload = json.load(response)
    if not payload.get("ok"):
        raise RuntimeError(f"Slack chat.postMessage failed: {payload.get('error')}")


def report():
    run_id = int(os.environ["TRIGGERING_RUN_ID"])
    repo = os.environ["GITHUB_REPOSITORY"]
    entry = find_entry(repo, run_id)
    if entry is None:
        note = f"run {run_id} was not dispatched by the nightly schedule"
        print(note)
        write_summary(note)
        return
    grouped = failed_by_group(jobs(repo, run_id))
    if not any(grouped.values()):
        note = f"run {run_id}: no failures"
        print(note)
        write_summary(note)
        return
    previous = None
    if entry.get("previous_run_id"):
        try:
            previous = previous_groups(repo, entry["previous_run_id"])
        except (subprocess.CalledProcessError, RuntimeError) as exc:
            print(f"previous run lookup failed: {exc}")
    text = format_message(os.environ["HEAD_BRANCH"], os.environ["HEAD_SHA"], run_id, grouped, previous)
    print(text)
    write_summary(text)
    if not os.environ.get("SLACK_BOT_TOKEN") or not os.environ.get("SLACK_CHANNEL_ID"):
        print("Slack secrets are not set, skipped posting")
        return
    post_slack(text)
    print(f"posted run {run_id}")


def self_check():
    grouped = failed_by_group([
        {"name": "Integration tests (amd_asan, 1/4)", "conclusion": "failure"},
        {"name": "Integration tests (amd_asan, 2/4)", "conclusion": "failure"},
        {"name": "Stateless tests (amd_debug, parallel)", "conclusion": "success"},
        {"name": "Build (amd_release)", "conclusion": "skipped"},
        {"name": "Stress test (amd_tsan)", "conclusion": "failure"},
    ])
    text = format_message(
        "antalya-26.6",
        "abc1234ffff",
        7,
        grouped,
        {"Integration": {"Integration tests (amd_asan, 1/4)"}},
    )
    assert text.startswith("antalya-26.6 · abc1234")
    assert "Integration: 2 failed, 1 new" in text
    assert "Stateless" not in text
    assert "Build" not in text
    assert "Stress" not in text
    assert "/7/ci_run_report.html" in text

    same = format_message(
        "antalya-26.6",
        "abc1234ffff",
        8,
        failed_by_group([{"name": "GrypeScanServer", "conclusion": "failure"}]),
        {"Grype": {"GrypeScanServer"}},
    )
    assert "Grype: 1 failed, same as previous" in same

    first = format_message(
        "antalya-26.6",
        "abc1234ffff",
        8,
        failed_by_group([{"name": "RegressionTestsRelease", "conclusion": "failure"}]),
        None,
    )
    assert "Regression: 1 failed\n" in first
    assert "new" not in first

    stateful = format_message(
        "antalya-26.6",
        "abc1234ffff",
        9,
        failed_by_group([{"name": "Stateful tests (amd_debug)", "conclusion": "failure"}]),
        None,
    )
    assert "Stateful: 1 failed\n" in stateful

    assert format_message("antalya-26.3", "abcdef0", 5, failed_by_group([]), None) is None
    print("self-check ok")


if __name__ == "__main__":
    try:
        if len(sys.argv) > 1 and sys.argv[1] == "self-check":
            self_check()
        else:
            report()
    except Exception:
        import traceback
        traceback.print_exc()
        sys.exit(1)
