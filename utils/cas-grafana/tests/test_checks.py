import os
import unittest

from casgrafana import checks, panels as P

KNOWN = {"S3PutObject", "CASBlobPut", "CASMountLeaseLost", "MemoryTracking", "CASConditionalWriteAttempts"}
ASYNC = {"CASGCIsLeader_", "CASGCWedgedNamespaces_", "CASGCPendingReclaim_", "CASGCLastSuccessAgeSeconds_"}


def good_overview():
    tile = P.stat("Writes safe", "Plain words. What to do: nothing.",
                  [P.target("SELECT if(count() = 0, NULL, sum(ProfileEvent_CASMountLeaseLost)) AS \"Lease lost\" "
                            "FROM clusterAllReplicas('$cluster', system.metric_log) WHERE hostName() IN ($host) AND $timeFilter "
                            + P.SETTINGS, fmt="table")],
                  {"Lease lost": {"steps": [("green", None), ("red", 1)]}}, links=[P.DRILLDOWN_LINK])
    ts = P.metric_ts("PUT", "S3 PUT requests per second.", {"PUT": P.r("S3PutObject")}, unit="reqps")
    return P.dashboard("cas-overview", "CAS / Overview", "d", [tile, ts], tags=["cas"], links=[])


def good_drilldown():
    ts = P.metric_ts("PUT", "S3 PUT per second.", {"PUT": P.r("S3PutObject")})
    counters = P.table("All CAS counters, nonzero in range", "Every CAS counter that moved.",
                       "SELECT 1 FROM clusterAllReplicas('$cluster', system.metric_log) WHERE hostName() IN ($host) AND $timeFilter " + P.SETTINGS,
                       table="metric_log")
    return P.dashboard("cas-drilldown", "CAS / Drilldown", "d",
                       [P.row("GC: rounds", [ts]), P.row(checks.COUNTERS_ROW, [counters])], tags=["cas"], links=[])


class Clean(unittest.TestCase):
    def test_good_boards_pass(self):
        self.assertEqual(checks.check_dashboards([good_overview(), good_drilldown()], KNOWN, ASYNC), [])


class Problems(unittest.TestCase):
    def one(self, mutate):
        o, d = good_overview(), good_drilldown()
        mutate(o, d)
        return checks.check_dashboards([o, d], KNOWN, ASYNC)

    def test_unknown_metric(self):
        probs = self.one(lambda o, d: o["panels"][1]["targets"][0].__setitem__("query", "SELECT $timeSeries, sum(ProfileEvent_Nope) FROM clusterAllReplicas('$cluster', system.metric_log) WHERE $timeFilter " + P.SETTINGS))
        self.assertTrue(any("unknown metric" in p and "ProfileEvent_Nope" in p for p in probs), probs)

    def test_banned_metric(self):
        probs = self.one(lambda o, d: d["panels"][0]["panels"][0]["targets"][0].__setitem__("query", "SELECT $timeSeries, sum(ProfileEvent_S3ReadRequestsErrors) FROM clusterAllReplicas('$cluster', system.metric_log) WHERE $timeFilter " + P.SETTINGS))
        self.assertTrue(any("banned" in p for p in probs), probs)

    def test_missing_description(self):
        probs = self.one(lambda o, d: o["panels"][1].__setitem__("description", ""))
        self.assertTrue(any("description" in p for p in probs), probs)

    def test_tile_without_drilldown_link(self):
        probs = self.one(lambda o, d: o["panels"][0].pop("links"))
        self.assertTrue(any("link" in p for p in probs), probs)

    def test_missing_time_filter(self):
        probs = self.one(lambda o, d: o["panels"][1]["targets"][0].__setitem__("query", "SELECT $timeSeries, 1 FROM clusterAllReplicas('$cluster', system.metric_log) " + P.SETTINGS))
        self.assertTrue(any("$timeFilter" in p for p in probs), probs)

    def test_now_query_is_exempt_from_time_filter(self):
        probs = self.one(lambda o, d: o["panels"][1]["targets"][0].__setitem__("query", "SELECT 1 /* now */ FROM clusterAllReplicas('$cluster', system.cas_gc_log) " + P.SETTINGS))
        self.assertFalse(any("$timeFilter" in p for p in probs), probs)

    def test_missing_cluster_all_replicas(self):
        probs = self.one(lambda o, d: o["panels"][1]["targets"][0].__setitem__("query", "SELECT $timeSeries, 1 FROM system.metric_log WHERE $timeFilter " + P.SETTINGS))
        self.assertTrue(any("clusterAllReplicas" in p for p in probs), probs)

    def test_missing_settings(self):
        probs = self.one(lambda o, d: o["panels"][1]["targets"][0].__setitem__("query", "SELECT $timeSeries, 1 FROM clusterAllReplicas('$cluster', system.metric_log) WHERE $timeFilter"))
        self.assertTrue(any("skip_unavailable_shards" in p for p in probs), probs)

    def test_row_prefix(self):
        probs = self.one(lambda o, d: d["panels"][0].__setitem__("title", "rounds"))
        self.assertTrue(any("row title" in p for p in probs), probs)

    def test_objects_prefix_allowed(self):
        probs = self.one(lambda o, d: d["panels"][0].__setitem__("title", "Objects: blobs"))
        self.assertFalse(any("row title" in p for p in probs), probs)

    def test_variables_must_match(self):
        probs = self.one(lambda o, d: d["templating"]["list"].pop())
        self.assertTrue(any("variables" in p for p in probs), probs)

    def test_tile_sum_must_be_null_guarded(self):
        probs = self.one(lambda o, d: o["panels"][0]["targets"][0].__setitem__("query", "SELECT sum(ProfileEvent_CASMountLeaseLost) AS \"Lease lost\" FROM clusterAllReplicas('$cluster', system.metric_log) WHERE hostName() IN ($host) AND $timeFilter " + P.SETTINGS))
        self.assertTrue(any("NULL" in p for p in probs), probs)

    def test_overview_internal_words(self):
        probs = self.one(lambda o, d: o["panels"][0].__setitem__("description", "The fold condemned it."))
        self.assertTrue(any("internal term" in p for p in probs), probs)

    def test_table_query_must_not_select_a_datetime_column(self):
        probs = self.one(lambda o, d: d["panels"][1]["panels"][0]["targets"][0].__setitem__("query", "SELECT event_time AS \"Time\", 1 FROM clusterAllReplicas('$cluster', system.metric_log) WHERE hostName() IN ($host) AND $timeFilter " + P.SETTINGS))
        self.assertTrue(any("DateTime" in p for p in probs), probs)

    def test_time_series_macro_needs_its_alias(self):
        probs = self.one(lambda o, d: o["panels"][1]["targets"][0].__setitem__("query", "SELECT $timeSeries, 1 FROM clusterAllReplicas('$cluster', system.metric_log) WHERE hostName() IN ($host) AND $timeFilter GROUP BY t " + P.SETTINGS))
        self.assertTrue(any("$timeSeries AS t" in p for p in probs), probs)

    def test_uid_and_title(self):
        probs = self.one(lambda o, d: o.__setitem__("title", "Overview"))
        self.assertTrue(any("title" in p for p in probs), probs)


class ExtraVariables(unittest.TestCase):
    def test_extra_constant_variables_are_allowed(self):
        o, d = good_overview(), good_drilldown()
        d["templating"]["list"].append({"type": "constant", "name": "price_put_per_1000", "query": "0.005"})
        self.assertEqual(checks.check_dashboards([o, d], KNOWN, ASYNC), [])


class KnownNames(unittest.TestCase):
    def test_loads_from_repo(self):
        root = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", "..", ".."))
        known, async_prefixes = checks.load_known_names(root)
        self.assertIn("CASBlobPut", known)
        self.assertIn("MemoryTracking", known)
        self.assertIn("CASGCIsLeader_", async_prefixes)


if __name__ == "__main__":
    unittest.main()
