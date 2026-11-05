import os
from datetime import datetime

from praktika.info import Info
from praktika.runtime import RunConfig
from praktika.utils import Shell

from ci.jobs.scripts.cidb_cluster import CIDBCluster
from ci.jobs.scripts.clickhouse_version import (
    CHVersion,
    next_release_tweak,
    release_tags,
)


def _track_from_branch(branch):
    """Track and pre-release flavour from a release-branch name, or None.

    PRs use the base branch; pushes use the branch. Prefixes `fips-`,
    `antalya-`, and `stable-` select the track, in that order. Anything else
    keeps the file values."""
    if not branch:
        return None
    if branch.startswith("fips-"):
        return 3, "altinityfipstest"
    if branch.startswith("antalya-"):
        return 2, "antalyatest"
    if branch.startswith("stable-"):
        return 1, "altinitytest"
    return None


def _build_version(info):
    """The build version recorded for this run.

    Unflavoured PR builds pin the tweak to 1. `HEAD` is the merge commit, and
    its first-parent count changes as `master` moves, so an unpinned tweak
    would store different version strings under one artifact prefix.

    Flavoured builds take flavour and tweak from the tag on a tag push, or
    from the branch and existing release tags when the branch names a track.
    Otherwise the committed file values are used."""
    version = CHVersion.get_current_version(no_strict=True)
    if info.pr_number != 0 and not version.flavour:
        version = version.with_tweak(1)
    if not version.flavour:
        return version
    # NOTE (strtgbb): tag pushes take tweak and flavour from the tag.
    # --check-tag validates the counter. Other runs use the branch.
    if os.getenv("GITHUB_REF_TYPE") == "tag":
        *_, tweak, flavour = os.getenv("GITHUB_REF_NAME", "").split(".")
        version.flavour = flavour
        return version.with_tweak(int(tweak)).with_description(flavour)
    branch = info.base_branch if info.pr_number != 0 else info.git_branch
    derived = _track_from_branch(branch)
    if not derived:
        return version
    track, flavour = derived
    tags = release_tags(version)
    tweak = next_release_tweak(tags, version, track, info.sha)
    version.flavour = flavour
    return version.with_tweak(tweak).with_description(flavour)


def _add_build_to_version_history():
    info = Info()
    Shell.check(
        f"git rev-parse --is-shallow-repository | grep -q true && git fetch --unshallow --prune --no-recurse-submodules --filter=tree:0 origin {info.git_branch} ||:"
    )
    commit_parents = Shell.get_output("git log --format=%P -n 1").split(" ")
    version = _build_version(info)
    data = {
        "check_start_time": datetime.now().strftime("%Y-%m-%d %H:%M:%S"),
        "pull_request_number": info.pr_number,
        "pull_request_url": info.pr_url,
        "commit_sha": info.sha,
        "commit_url": info.commit_url,
        "parent_commits_sha": commit_parents,
        "version": version.string,
        "git_ref": info.git_branch,
    }
    print(f"Update version log: [{data}]")
    CIDBCluster().insert_json(table="version_history", json_str=data)
    # stores actual version data in pipline storage, to be used by jobs that need it
    version.store_version_data_in_ci_pipeline()
    workflow_config = RunConfig.from_fs(info.workflow_name)
    workflow_config.custom_data["version"] = version.to_dict()
    workflow_config.dump()


if __name__ == "__main__":
    _add_build_to_version_history()
