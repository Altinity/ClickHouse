import unittest

from casgrafana import panels as P


class SqlHelpers(unittest.TestCase):
    def test_rate_count_gauge(self):
        self.assertEqual(P.r("CASBlobPut"), "sum(ProfileEvent_CASBlobPut) / $interval")
        self.assertEqual(P.c("CASBlobPut"), "sum(ProfileEvent_CASBlobPut)")
        self.assertEqual(P.g("MemoryTracking"), "max(CurrentMetric_MemoryTracking)")
        self.assertEqual(P.guarded("sum(x)"), "if(count() = 0, NULL, sum(x))")


class MetricTs(unittest.TestCase):
    def test_query_shape(self):
        p = P.metric_ts("PUT rate", "desc", {"PUT": P.r("S3PutObject")}, unit="reqps")
        sql = p["targets"][0]["query"]
        self.assertIn("SELECT $timeSeries AS t, sum(ProfileEvent_S3PutObject) / $interval AS \"PUT\"", sql)
        self.assertIn("FROM clusterAllReplicas('$cluster', system.metric_log)", sql)
        self.assertIn("WHERE hostName() IN ($host) AND $timeFilter", sql)
        self.assertIn("GROUP BY t ORDER BY t", sql)
        self.assertTrue(sql.rstrip().endswith(P.SETTINGS))
        self.assertEqual(p["interval"], "5m")
        self.assertEqual(p["fieldConfig"]["defaults"]["unit"], "reqps")
        self.assertEqual(p["targets"][0]["dateTimeColDataType"], "event_time")

    def test_by_host_needs_single_series(self):
        with self.assertRaises(ValueError):
            P.metric_ts("x", "d", {"a": "1", "b": "2"}, by_host=True)
        p = P.metric_ts("x", "d", {"a": P.r("S3ListObjects")}, by_host=True)
        self.assertIn("hostName() AS host", p["targets"][0]["query"])
        self.assertIn("GROUP BY t, host ORDER BY t", p["targets"][0]["query"])

    def test_stacking(self):
        p = P.metric_ts("x", "d", {"a": "1"}, stack=True)
        self.assertEqual(p["fieldConfig"]["defaults"]["custom"]["stacking"]["mode"], "normal")


class Stat(unittest.TestCase):
    def test_fields_thresholds_and_null_mapping(self):
        p = P.stat("Storage online", "d", [P.target("SELECT 1 AS \"Disks not live\"", fmt="table")],
                   {"Disks not live": {"steps": [("green", None), ("red", 1)]},
                    "Metrics age (s)": {"steps": [("green", None), ("yellow", 120), ("red", 600)], "unit": "s"}},
                   links=[P.DRILLDOWN_LINK])
        d = p["fieldConfig"]["defaults"]
        self.assertEqual(d["noValue"], "no data")
        self.assertEqual(d["mappings"][0]["options"]["match"], "null")
        names = {o["matcher"]["options"] for o in p["fieldConfig"]["overrides"]}
        self.assertEqual(names, {"Disks not live", "Metrics age (s)"})
        age = next(o for o in p["fieldConfig"]["overrides"] if o["matcher"]["options"] == "Metrics age (s)")
        props = {x["id"]: x["value"] for x in age["properties"]}
        self.assertEqual(props["unit"], "s")
        self.assertEqual(props["thresholds"]["steps"][2], {"color": "red", "value": 600})
        self.assertEqual(p["links"][0]["url"], P.DRILLDOWN_LINK["url"])
        self.assertEqual(p["options"]["textMode"], "value_and_name")


class Table(unittest.TestCase):
    def test_time_column_rendered_as_date(self):
        p = P.table("t", "d", "SELECT toUInt64(event_time) * 1000 AS \"Time\" FROM x")
        self.assertEqual(p["targets"][0]["format"], "table")
        override = next(o for o in p["fieldConfig"]["overrides"] if o["matcher"]["options"] == "Time")
        self.assertEqual(override["properties"][0], {"id": "unit", "value": "dateTimeAsIso"})


class Layout(unittest.TestCase):
    def test_flow_layout_wraps_and_rows_reset(self):
        a = P.metric_ts("a", "d", {"a": "1"}, w=12)
        b = P.metric_ts("b", "d", {"a": "1"}, w=12)
        c = P.metric_ts("c", "d", {"a": "1"}, w=8)
        d = P.dashboard("cas-overview", "CAS / Overview", "desc", [a, b, P.row("GC: rounds", [c])], tags=["cas"], links=[])
        pos = [p["gridPos"] for p in d["panels"]]
        self.assertEqual((pos[0]["x"], pos[0]["y"]), (0, 0))
        self.assertEqual((pos[1]["x"], pos[1]["y"]), (12, 0))
        self.assertEqual(d["panels"][2]["type"], "row")
        self.assertEqual(pos[2]["y"], 8)
        inner = d["panels"][2]["panels"][0]["gridPos"]
        self.assertEqual((inner["x"], inner["y"]), (0, 9))
        ids = [p["id"] for p in d["panels"]] + [q["id"] for p in d["panels"] for q in p.get("panels", [])]
        self.assertEqual(len(ids), len(set(ids)))

    def test_expanded_row_panels_follow_the_row_at_top_level(self):
        a = P.metric_ts("a", "d", {"a": "1"}, w=12)
        row = P.row("Needs attention", [a], collapsed=False)
        d = P.dashboard("cas-overview", "CAS / Overview", "desc", [row], tags=["cas"], links=[])
        self.assertEqual([p["type"] for p in d["panels"]], ["row", "timeseries"])
        self.assertEqual(d["panels"][0]["panels"], [])
        self.assertEqual(d["panels"][1]["gridPos"]["y"], 1)
        self.assertEqual(len(row["panels"]), 1)  # the input row is not mutated

    def test_cluster_variable_skips_operator_clusters(self):
        q = P._templating()["list"][1]["query"]
        self.assertIn("cluster NOT IN ('default')", q)
        self.assertIn("cluster NOT LIKE 'all-%'", q)

    def test_stat_layout_is_readable(self):
        p = P.stat("t", "d", [P.target("SELECT 1", fmt="table")], {})
        self.assertEqual(p["options"]["orientation"], "horizontal")
        self.assertEqual(p["options"]["text"], {"titleSize": 12, "valueSize": 20})

    def test_dashboard_skeleton(self):
        d = P.dashboard("cas-drilldown", "CAS / Drilldown", "desc", [], tags=["cas"], links=[])
        self.assertEqual(d["uid"], "cas-drilldown")
        self.assertNotIn("id", d)
        names = [v["name"] for v in d["templating"]["list"]]
        self.assertEqual(names, ["datasource", "cluster", "host", "disk"])
        host = d["templating"]["list"][2]
        self.assertTrue(host["multi"] and host["includeAll"])
        self.assertNotIn("allValue", host)
        self.assertEqual(host["current"], {"text": ["All"], "value": ["$__all"], "selected": True})
        self.assertIn("system.cas_gc_log", d["templating"]["list"][3]["query"])
        self.assertEqual([a["name"] for a in d["annotations"]["list"]], ["Restart", "Lease lost"])
        self.assertEqual(d["refresh"], "1m")
        self.assertEqual(d["time"]["from"], "now-6h")


if __name__ == "__main__":
    unittest.main()
