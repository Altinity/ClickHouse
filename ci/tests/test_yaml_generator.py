from ci.praktika import Job, Secret, Workflow
from ci.praktika.parser import WorkflowConfigParser
from ci.praktika.yaml_additional_templates import AltinityWorkflowTemplates
from ci.praktika.yaml_generator import PullRequestPushYamlGen
from ci.settings.altinity_overrides import DISABLED_WORKFLOWS


def _generate(workflow: Workflow.Config) -> str:
    parser = WorkflowConfigParser(workflow).parse()
    return PullRequestPushYamlGen(parser).generate()


def _job(name="Hello", secrets=None):
    kwargs = dict(name=name, runs_on=["self-hosted"], command="true")
    if secrets is not None:
        kwargs["secrets"] = secrets
    return Job.Config(**kwargs)


def test_input_config_uses_upstream_boolean_flag():
    input_item = Workflow.Config.InputConfig(
        name="no_cache",
        description="Run without cache",
        is_required=False,
        default_value="false",
        is_boolean=True,
    )

    assert not hasattr(input_item, "input_type")
    assert input_item.is_boolean is True


def test_dispatch_with_tags_renders_boolean_input_and_tag_trigger():
    yaml_text = _generate(
        Workflow.Config(
            name="MasterCI",
            event=Workflow.Event.DISPATCH,
            tags=["*"],
            inputs=[
                Workflow.Config.InputConfig(
                    name="no_cache",
                    description="Run without cache",
                    is_required=False,
                    default_value="false",
                    is_boolean=True,
                )
            ],
            jobs=[_job()],
        )
    )

    assert "type: boolean" in yaml_text
    assert "push:" in yaml_text
    assert "tags: ['*']" in yaml_text


def test_per_job_secret_is_exported_into_setup_script():
    yaml_text = _generate(
        Workflow.Config(
            name="CreateRelease",
            event=Workflow.Event.DISPATCH,
            jobs=[
                _job(
                    name="CreateRelease",
                    secrets=[
                        Secret.Config(
                            name="ROBOT_CLICKHOUSE_COMMIT_TOKEN",
                            type=Secret.Type.GH_SECRET,
                        )
                    ],
                )
            ],
        )
    )

    assert "export ROBOT_CLICKHOUSE_COMMIT_TOKEN=$(cat<<'EOF'" in yaml_text
    assert "${{ secrets.ROBOT_CLICKHOUSE_COMMIT_TOKEN }}" in yaml_text


def test_skip_condition_is_emitted_when_cache_is_disabled():
    yaml_text = _generate(
        Workflow.Config(
            name="PR",
            event=Workflow.Event.PULL_REQUEST,
            base_branches=["antalya"],
            enable_cache=False,
            jobs=[_job("Config Workflow"), _job("Fast test")],
        )
    )

    assert "cache_success_base64" in yaml_text


def test_altinity_injected_jobs_skip_undefined_pipeline_status():
    for name in ("GrypeScan", "Regression", "RegressionPR"):
        assert (
            "!contains(needs.*.outputs.pipeline_status, 'undefined')"
            in AltinityWorkflowTemplates.ALTINITY_JOBS[name]
        )


def test_upstream_release_workflows_are_disabled():
    assert "auto_releases.py" in DISABLED_WORKFLOWS
    assert "create_release.py" in DISABLED_WORKFLOWS
