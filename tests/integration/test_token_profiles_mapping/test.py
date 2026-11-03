"""
Tests for `<profiles_mapping>` in the `token` user directory: settings profiles assigned
directly to externally defined users by their IdP groups.

Profiles are assigned to the user itself, so they override profiles carried by roles;
mapped profiles are assigned after `default_profile`, in config order. The profile settings
must reach other nodes of a cluster, which have neither the user's groups nor the profiles.
"""

import jwt
import pytest

from helpers.cluster import ClickHouseCluster

SECRET = "profiles_mapping_test_secret"

cluster = ClickHouseCluster(__file__)

instance1 = cluster.add_instance(
    "instance1",
    main_configs=["configs/token.xml", "configs/remote_servers.xml"],
    user_configs=["configs/users.xml"],
)

instance2 = cluster.add_instance(
    "instance2",
    main_configs=["configs/token.xml", "configs/remote_servers.xml"],
    user_configs=["configs/users.xml"],
)

DEFAULT_MAX_QUERY_SIZE = "262144"


@pytest.fixture(scope="module", autouse=True)
def started_cluster():
    try:
        cluster.start()

        instance1.query("CREATE SETTINGS PROFILE p_default SETTINGS max_block_size = 1001")
        instance1.query("CREATE SETTINGS PROFILE p_role SETTINGS max_insert_block_size = 2002")
        instance1.query(
            "CREATE SETTINGS PROFILE p_analysts SETTINGS max_block_size = 1003, max_insert_block_size = 2003"
        )
        instance1.query("CREATE SETTINGS PROFILE p_fast SETTINGS max_block_size = 1004, max_query_size = 300004")
        instance1.query("CREATE ROLE r_common SETTINGS PROFILE 'p_role'")

        # instance2 has the role pushed by the initiator, but none of the profiles.
        instance2.query("CREATE ROLE r_common")

        for node in (instance1, instance2):
            node.query("GRANT SELECT, REMOTE ON *.* TO r_common")

        yield cluster
    finally:
        cluster.shutdown()


def query_with_token(node, sub, groups, sql):
    token = jwt.encode({"sub": sub, "groups": groups}, SECRET, algorithm="HS256")
    resp = node.http_request(
        "",
        method="POST",
        data=sql,
        headers={"Authorization": f"Bearer {token}"},
    )
    resp.raise_for_status()
    return resp.text


def get_settings(sub, groups):
    return query_with_token(
        instance1,
        sub,
        groups,
        "SELECT getSetting('max_block_size'), getSetting('max_insert_block_size'), getSetting('max_query_size') "
        "FORMAT TabSeparated",
    ).strip().split("\t")


def test_without_mapped_groups():
    assert get_settings("user_no_groups", ["g_unmapped"]) == ["1001", "2002", DEFAULT_MAX_QUERY_SIZE]


def test_mapped_profile_overrides_default_and_role_profiles():
    assert get_settings("user_analyst", ["g_analysts"]) == ["1003", "2003", DEFAULT_MAX_QUERY_SIZE]


def test_later_mapping_entry_wins():
    assert get_settings("user_both", ["g_fast", "g_analysts"]) == ["1004", "2003", "300004"]


def test_profile_removed_with_group():
    assert get_settings("user_leaving", ["g_analysts"]) == ["1003", "2003", DEFAULT_MAX_QUERY_SIZE]
    assert get_settings("user_leaving", []) == ["1001", "2002", DEFAULT_MAX_QUERY_SIZE]


def test_mapped_profile_settings_reach_other_nodes():
    result = query_with_token(
        instance1,
        "user_cluster",
        ["g_fast"],
        "SELECT name, value FROM cluster('test_token_cluster', system.settings) "
        "WHERE name IN ('max_block_size', 'max_query_size') ORDER BY name, value FORMAT TabSeparated",
    )
    assert result.strip().split("\n") == [
        "max_block_size\t1004",
        "max_block_size\t1004",
        "max_query_size\t300004",
        "max_query_size\t300004",
    ]
