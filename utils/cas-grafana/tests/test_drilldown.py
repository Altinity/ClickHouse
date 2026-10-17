import os
import unittest

from casgrafana import drilldown, checks

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", "..", ".."))


def panel(rows, title):
    return next(p for r in rows for p in r["panels"] if p["title"] == title)


class GcRows(unittest.TestCase):
    def test_row_titles(self):
        self.assertEqual([r["title"] for r in drilldown.gc_rows()],
                         ["GC: rounds", "GC: stage flow", "GC: phases", "GC: round cost", "GC: ref-log cleanup", "GC: anomalies"])
        self.assertTrue(all(r["collapsed"] for r in drilldown.gc_rows()))

    def test_rounds_panel_groups_by_outcome(self):
        sql = panel(drilldown.gc_rows(), "Rounds per interval, by outcome and host")["targets"][0]["query"]
        self.assertIn("concat(hostName(), ' ', toString(outcome))", sql)

    def test_rounds_panel_colors_outcomes(self):
        p = panel(drilldown.gc_rows(), "Rounds per interval, by outcome and host")
        regexps = {o["matcher"]["options"] for o in p["fieldConfig"]["overrides"] if o["matcher"]["id"] == "byRegexp"}
        self.assertIn(".*Deferred$", regexps)

    def test_leader_timeline_uses_async_metric(self):
        sql = panel(drilldown.gc_rows(), "GC leader (1 = this host leads)")["targets"][0]["query"]
        self.assertIn("metric LIKE 'CASGCIsLeader_%'", sql)
        self.assertIn("replaceOne(metric, 'CASGCIsLeader_', '') IN ($disk)", sql)

    def test_phase_table_reads_profile_events_map(self):
        sql = panel(drilldown.gc_rows(), "Phases: duration and S3 requests per phase")["targets"][0]["query"]
        for k in ("'CASRootList'", "'CASGCGet'", "quantile(0.99)(phase_duration_microseconds)"):
            self.assertIn(k, sql)
        self.assertNotIn("ProfileEvents['S3ListObjects']", sql)

    def test_round_cost_uses_cas_semantic_counters(self):
        self.assertEqual(drilldown.cas_verb_sum("List"),
                         "arraySum(x -> ProfileEvents[x], ['CASBlobList', 'CASManifestList', 'CASRootList', 'CASGCList', 'CASOtherList'])")
        sql = panel(drilldown.gc_rows(), "S3 requests per round, by verb")["targets"][0]["query"]
        self.assertIn("'CASGCPut'", sql)
        self.assertIn("ProfileEvents['CASBulkDeleteRequests'] AS \"DELETE requests\"", sql)
        self.assertNotIn("ProfileEvents['S3PutObject']", sql)

    def test_build_passes_checks_with_repo_names(self):
        known, async_prefixes = checks.load_known_names(ROOT)
        probs = [p for p in checks.check_dashboards([drilldown.build()], known, async_prefixes) if "variables" not in p]
        self.assertEqual(probs, [])


class ObjectRows(unittest.TestCase):
    def test_rows_between_gc_and_writes(self):
        titles = [r["title"] for r in drilldown.build()["panels"] if r["type"] == "row"]
        objects = [t for t in titles if t.startswith("Objects:")]
        self.assertEqual(objects, ["Objects: blobs", "Objects: manifests", "Objects: refs", "Objects: namespaces", "Objects: GC state"])
        self.assertLess(titles.index("GC: anomalies"), titles.index("Objects: blobs"))
        self.assertLess(titles.index("Objects: GC state"), titles.index("Writes: mounts now"))

    def test_blobs_row(self):
        titles = [p["title"] for p in next(r for r in drilldown.object_rows() if r["title"] == "Objects: blobs")["panels"]]
        for t in ("Blobs per 5 minutes: new, deduplicated, body put avoided, adopted, deleted", "Blob events per 5 minutes (cas_log)",
                  "Blob bytes per 5 minutes: published vs reused", "Bytes uploaded under blobs/ per 5 minutes", "Net blobs over the period"):
            self.assertIn(t, titles)
        sql = panel(drilldown.object_rows(), "Blob events per 5 minutes (cas_log)")["targets"][0]["query"]
        self.assertIn("object_kind = 'blob'", sql)

    def test_refs_row_has_totals_and_snapshot_bytes(self):
        titles = [p["title"] for p in next(r for r in drilldown.object_rows() if r["title"] == "Objects: refs")["panels"]]
        for t in ("Ref-log keys per round (total)", "Snapshots and checkpoints per round (total)", "Ref lives per round (total)",
                  "Ref objects per 5 minutes: log keys written, snapshots, checkpoints, deleted", "Snapshot bytes per 5 minutes"):
            self.assertIn(t, titles)
        sql = panel(drilldown.object_rows(), "Snapshot bytes per 5 minutes")["targets"][0]["query"]
        self.assertIn("CASRefSnapshotPutBytes", sql)

    def test_namespaces_and_gc_state_rows(self):
        ns = [p["title"] for p in next(r for r in drilldown.object_rows() if r["title"] == "Objects: namespaces")["panels"]]
        self.assertIn("Namespaces per round (total)", ns)
        self.assertIn("Namespaces per round: added, removed, changed", ns)
        gc = [p["title"] for p in next(r for r in drilldown.object_rows() if r["title"] == "Objects: GC state")["panels"]]
        self.assertIn("GC state: seal bytes per round", gc)
        self.assertIn("GC state bytes uploaded per 5 minutes, by sub-prefix", gc)

    def test_latest_round_inventory_reads_phase_metrics(self):
        sql = panel(drilldown.object_rows(), "Inventory, latest round")["targets"][0]["query"]
        for key in ("namespaces_seen", "seal_ref_lives", "ref_log_keys_listed", "ref_keys_listed", "tables_scanned"):
            self.assertIn(f"mapContains(phase_metrics, '{key}')", sql)
        self.assertIn("/* now */", sql)
        self.assertIn('AS "Ref lives"', sql)

    def test_no_system_parts_anywhere(self):
        for p, _ in checks.iter_panels(drilldown.build()):
            for t in p["targets"]:
                self.assertNotIn("system.parts", t["query"], p["title"])

    def test_namespaces_per_round_derives_added(self):
        sql = panel(drilldown.object_rows(), "Namespaces per round: added, removed, changed")["targets"][0]["query"]
        self.assertIn("lagInFrame", sql)
        self.assertIn("namespace_removals", sql)
        self.assertIn("GROUP BY round_id", sql)


class WritesRows(unittest.TestCase):
    def test_row_titles(self):
        self.assertEqual([r["title"] for r in drilldown.writes_rows()],
                         ["Writes: mounts now", "Writes: lease", "Writes: conditional writes", "Writes: ref lane",
                          "Writes: blobs and dedup", "Writes: caches", "Writes: replication"])

    def test_mounts_now_refreshes_slowly_and_has_no_time_filter(self):
        p = panel(drilldown.writes_rows(), "Mounts now")
        self.assertEqual(p["interval"], "5m")
        sql = p["targets"][0]["query"]
        self.assertIn("system.cas_mounts", sql)
        self.assertNotIn("$timeFilter", sql)
        self.assertIn("dateDiff('second', now(), expires_at)", sql)

    def test_dedup_ratio(self):
        sql = panel(drilldown.writes_rows(), "Dedup ratio (%)")["targets"][0]["query"]
        self.assertIn("100 * sum(ProfileEvent_CASBlobPutDeduplicated) / greatest(sum(ProfileEvent_CASBlobPutDeduplicated) + sum(ProfileEvent_CASBlobHeadMiss), 1)", sql)

    def test_decode_cache_vs_cap(self):
        sql = panel(drilldown.writes_rows(), "Manifest decode cache bytes vs 128 MiB default cap")["targets"][0]["query"]
        self.assertIn("max(CurrentMetric_CASManifestDecodeCacheBytes)", sql)
        self.assertIn("134217728", sql)


class S3Rows(unittest.TestCase):
    def test_gc_state_share_panel_is_named_for_what_it_measures(self):
        p = panel(drilldown.s3_rows(), "GC-state objects' share of CAS requests (%)")
        self.assertIn("object class", p["description"])

    def test_row_titles(self):
        self.assertEqual([r["title"] for r in drilldown.s3_rows()],
                         ["S3: by verb", "S3: who makes requests", "S3: cost estimate", "S3: errors and throttling", "S3: cost per unit", "S3: latency"])

    def test_cost_uses_price_variables(self):
        sql = panel(drilldown.s3_rows(), "Estimated cost per day (USD)")["targets"][0]["query"]
        self.assertIn("$price_put_per_1000", sql)
        self.assertIn("$price_get_per_1000", sql)

    def test_write_errors_panel_carries_the_412_caveat(self):
        p = panel(drilldown.s3_rows(), "S3 write request errors per second (see description)")
        self.assertIn("412", p["description"])
        self.assertIn("409", p["description"])

    def test_counters_row_is_last_and_dynamic(self):
        d = drilldown.build()
        last = d["panels"][-1]
        self.assertEqual(last["title"], "All CAS counters, nonzero in range")
        sql = last["panels"][0]["targets"][0]["query"]
        self.assertIn("system.columns", sql)
        self.assertIn("name LIKE 'ProfileEvent_CAS%'", sql)

    def test_price_variables_declared_in_drilldown_only(self):
        d = drilldown.build()
        names = [v["name"] for v in d["templating"]["list"]]
        self.assertEqual(names[:4], ["datasource", "cluster", "host", "disk"])
        self.assertIn("price_put_per_1000", names)


if __name__ == "__main__":
    unittest.main()
