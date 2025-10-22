from ci.praktika.runtime import RunConfig
from ci.praktika.mangle import _get_workflows
from ci.praktika._environment import _Environment


def get_cache_entry():
    return {
        "type": "success",
        "sha": "410f2f383a3efc6c525d353ce88b9353da0c275b",
        "pr_number": 0,
        "branch": "antalya-25.8",
        "workflow": "MasterCI",
    }


if __name__ == "__main__":

    workflow = _get_workflows(name=_Environment.get().WORKFLOW_NAME)[0]
    workflow_config = RunConfig.from_fs(workflow.name)

    cache_entry = get_cache_entry()

    for job in workflow.jobs:
        if not job.name.startswith("Build"):
            continue

        if job.name in workflow_config.filtered_jobs:
            continue

        workflow_config.set_job_as_filtered(
            job.name, "Skipped, result supplied by user"
        )
        workflow_config.cache_jobs[job.name] = cache_entry
        workflow_config.cache_artifacts[job.name] = cache_entry
        for artifact in job.provides:
            workflow_config.cache_artifacts[artifact] = cache_entry

    workflow_config.dump()
