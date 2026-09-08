import pytest

from helpers.export_partition_helpers import SOURCE_ENGINE_IDS, SOURCE_ENGINES


@pytest.fixture(params=SOURCE_ENGINES, ids=SOURCE_ENGINE_IDS)
def source_engine(request):
    """The MergeTree flavour of the export source table.

    A test that requests this fixture runs once per engine; the scenarios that only make sense
    with cross-replica coordination do not request it and stay on `ReplicatedMergeTree`.
    """
    return request.param
