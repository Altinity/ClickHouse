"""Unit tests for the tuned-config render path (no cluster, no docker).

Run: cd utils/ca-soak && python3 -m pytest scenarios/tests/test_render_tuned_config.py -q
"""

from scenarios.framework import cluster_boot


def _tuned_xml(node):
    return (cluster_boot.CA_SOAK_DIR / "configs" / f"storage_conf_tuned_{node}.xml").read_text()


def test_render_injects_overrides_into_ca_block():
    cluster_boot.render_tuned_config({"dedup_cache_bytes": "268435456",
                                       "part_folder_validate": "age 5"})
    for node in ("ch1", "ch2"):
        xml = _tuned_xml(node)
        assert "<dedup_cache_bytes>268435456</dedup_cache_bytes>" in xml
        assert "<part_folder_validate>age 5</part_folder_validate>" in xml
        assert "<metadata_type>content_addressed</metadata_type>" in xml  # base block preserved


def test_render_twice_with_different_value_replaces_not_duplicates():
    cluster_boot.render_tuned_config({"dedup_cache_bytes": "1048576"})
    cluster_boot.render_tuned_config({"dedup_cache_bytes": "16777216"})
    for node in ("ch1", "ch2"):
        xml = _tuned_xml(node)
        assert xml.count("<dedup_cache_bytes>") == 1
        assert "<dedup_cache_bytes>16777216</dedup_cache_bytes>" in xml
        assert "1048576" not in xml
