from datetime import datetime

from praktika.info import Info
from praktika.runtime import RunConfig
from praktika.utils import Shell

from ci.jobs.scripts.cidb_cluster import CIDBCluster
from ci.jobs.scripts.clickhouse_version import CHVersion


def _build_version(info):
    """The build version recorded for this run.

    In a PR an unflavoured version's tweak is pinned to 1. `HEAD` is the
    ephemeral merge commit, whose first-parent commit count diverges across
    close/reopen and re-runs of the same PR as `master` advances. Artifacts are
    keyed by the head SHA, so an unpinned tweak would store diverging version
    strings under one artifact prefix. Flavoured Altinity versions keep the
    explicit tweak committed in the version file."""
    version = CHVersion.get_current_version(no_strict=True)
    if info.pr_number != 0 and not version.flavour:
        version = version.with_tweak(1)
    return version


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
