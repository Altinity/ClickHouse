import unittest

from casgrafana import overview, checks


def tile(title):
    return next(t for t in overview.tiles() if t["title"] == title)


def sql_of(p, ref="A"):
    return next(t["query"] for t in p["targets"] if t["refId"] == ref)


class Tiles(unittest.TestCase):
    def test_six_tiles_in_order(self):
        self.assertEqual([t["title"] for t in overview.tiles()],
                         ["Storage online", "Writes safe", "GC: last success", "Garbage trend", "Integrity", "S3 throttling"])

    def test_storage_tile_counts_missing_hosts(self):
        sql = sql_of(tile("Storage online"), "A")
        self.assertIn("FROM system.clusters WHERE cluster = '$cluster'", sql)
        self.assertIn("uniqExact(host)", sql)
        self.assertIn("system.cas_mounts", sql)
        self.assertEqual(tile("Storage online")["interval"], "5m")

    def test_storage_tile_metrics_age_is_red_without_rows(self):
        sql = sql_of(tile("Storage online"), "B")
        self.assertIn("dateDiff('second', max(event_time), now()) AS \"Metrics age (s)\"", sql)
        age = next(o for o in tile("Storage online")["fieldConfig"]["overrides"] if o["matcher"]["options"] == "Metrics age (s)")
        steps = next(x for x in age["properties"] if x["id"] == "thresholds")["value"]["steps"]
        self.assertEqual([s["value"] for s in steps], [None, 120, 600])

    def test_gc_tile_counts_deferred_as_success(self):
        sql = sql_of(tile("GC: last success"), "A")
        self.assertIn("outcome IN ('Success', 'Deferred')", sql)
        self.assertIn("/* now */", sql)

    def test_garbage_tile_uses_last_10_success_rounds(self):
        sql = sql_of(tile("Garbage trend"), "A")
        self.assertIn("ORDER BY event_time DESC LIMIT 10", sql)
        self.assertIn("toInt64(entries_condemned) - toInt64(entries_redeleted)", sql)
        self.assertIn("100 * sum(net) / greatest(sum(cond), 1)", sql)

    def test_s3_errors_tile_excludes_benign_conditional_write_answers(self):
        sql = sql_of(tile("S3 throttling"), "B")
        self.assertIn("NOT LIKE '%PreconditionFailed%'", sql)
        self.assertIn("NOT LIKE '%ConditionalRequestConflict%'", sql)
        self.assertIn("/ greatest(($__to - $__from) / 3600000, 1)", sql)
        self.assertIn('AS "S3 errors per hour"', sql)

    def test_wedged_lanes_is_null_without_rows(self):
        sql = sql_of(tile("Integrity"), "D")
        self.assertIn("if(count() = 0, NULL, max(v)) AS \"Wedged lanes\"", sql)

    def test_gc_rounds_bars_color_deferred_separately(self):
        p = next(p for r in overview.rows() for p in r["panels"] if p["title"] == "GC rounds per 30 minutes, by outcome")
        colors = {o["matcher"]["options"]: next(x["value"]["fixedColor"] for x in o["properties"] if x["id"] == "color")
                  for o in p["fieldConfig"]["overrides"] if o["matcher"]["id"] == "byRegexp"}
        self.assertEqual(colors[".*Deferred$"], "blue")
        self.assertEqual(colors[".*Success$"], "green")
        self.assertEqual(colors[".*(Error|Aborted|Stopped)$"], "red")
        self.assertEqual(colors[".*NotALeader$"], "#8e8e8e")

    def test_every_tile_links_to_drilldown_and_passes_checks(self):
        d = overview.build()
        known = {"CASMountLeaseLost", "CASRequestGaveUp", "CASConditionalWriteAttempts", "CASConditionalWriteUnresolved",
                 "CASRefNeedsRecovery", "S3ReadRequestsThrottling", "S3WriteRequestsThrottling"} | overview.METRICS_USED
        self.assertEqual([p for p in checks.check_dashboards([d], known, {"CASGCWedgedNamespaces_", "CASGCIsLeader_"}) if "variables" not in p], [])


class Rows(unittest.TestCase):
    def test_rows_present_and_expanded(self):
        rows = overview.rows()
        self.assertEqual([r["title"] for r in rows], ["Needs attention", "Storage and writes", "Garbage collection", "Objects", "S3 requests"])
        self.assertTrue(all(r["collapsed"] is False for r in rows))

    def test_needs_attention_unions_three_sources(self):
        sql = overview.rows()[0]["panels"][0]["targets"][0]["query"]
        self.assertEqual(sql.count("UNION ALL"), 3)
        for needle in ("system.cas_log", "system.cas_gc_log", "outcome IN ('Error', 'Aborted', 'Stopped')", "gc_fence_out", "ORDER BY \"Time\" DESC LIMIT 100"):
            self.assertIn(needle, sql)

    def test_objects_row_in_plain_words(self):
        row = next(r for r in overview.rows() if r["title"] == "Objects")
        titles = [p["title"] for p in row["panels"]]
        self.assertEqual(titles, ["Tables and write logs", "Data objects: new vs deleted per second",
                                  "Part descriptions: new vs deleted per second", "Write-log entries: written vs cleaned per second",
                                  "Data uploaded per second, by kind"])
        stat = row["panels"][0]
        self.assertEqual(stat["type"], "stat")
        self.assertIn("mapContains(phase_metrics, 'namespaces_seen')", stat["targets"][0]["query"])
        self.assertEqual(len(stat["targets"]), 1)
        uploaded = row["panels"][4]["targets"][0]["query"]
        self.assertIn("'GC state'", uploaded)
        self.assertNotIn("Collector state", uploaded)

    def test_insert_delay_is_wait_per_mutation(self):
        p = next(p for r in overview.rows() for p in r["panels"] if p["title"] == "Insert delay caused by storage (ms per operation)")
        self.assertIn("sum(ProfileEvent_CASRefQueueWaitMicroseconds) / greatest(sum(ProfileEvent_CASRefBatchedMutations), 1) / 1000", p["targets"][0]["query"])
        self.assertEqual(p["interval"], "5m")


if __name__ == "__main__":
    unittest.main()
