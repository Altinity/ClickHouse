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
        assert "<metadata_type>cas</metadata_type>" in xml  # base block preserved


def test_render_twice_with_different_value_replaces_not_duplicates():
    cluster_boot.render_tuned_config({"dedup_cache_bytes": "1048576"})
    cluster_boot.render_tuned_config({"dedup_cache_bytes": "16777216"})
    for node in ("ch1", "ch2"):
        xml = _tuned_xml(node)
        assert xml.count("<dedup_cache_bytes>") == 1
        assert "<dedup_cache_bytes>16777216</dedup_cache_bytes>" in xml
        assert "1048576" not in xml


def test_render_overrides_a_key_already_present_in_the_base_xml():
    # gc_interval_sec is already set (to 10) in the base storage_conf_ch{1,2}.xml — this exercises the
    # "replace a same-named child" branch directly, not just the "re-render from scratch" idempotency
    # covered above.
    cluster_boot.render_tuned_config({"gc_interval_sec": "3"})
    for node in ("ch1", "ch2"):
        xml = _tuned_xml(node)
        assert xml.count("<gc_interval_sec>") == 1
        assert "<gc_interval_sec>3</gc_interval_sec>" in xml
        assert "<gc_interval_sec>10</gc_interval_sec>" not in xml
