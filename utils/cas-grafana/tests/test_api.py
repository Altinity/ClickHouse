import os
import tempfile
import unittest

from casgrafana import api


class OwnUidGuard(unittest.TestCase):
    def test_own_uids_pass(self):
        for uid in ("cas-overview", "cas-drilldown"):
            api.assert_owned_uid(uid)

    def test_foreign_uid_raises(self):
        with self.assertRaises(ValueError):
            api.assert_owned_uid("adu5ghhwk0rnka")

    def test_post_dashboard_refuses_foreign_uid_before_any_http(self):
        client = api.GrafanaClient(api.Config(grafana_url="http://127.0.0.1:9", ds_uid="x", cluster="c"))
        with self.assertRaises(ValueError):
            client.post_dashboard({"uid": "alerts-dashboard", "title": "x"}, None, "m")


class LoadConfig(unittest.TestCase):
    def test_reads_env_file_and_defaults(self):
        with tempfile.NamedTemporaryFile("w", suffix=".env", delete=False) as f:
            f.write("GRAFANA_DS_UID=P1\nGRAFANA_CLUSTER=otel\n# comment\n\nGRAFANA_TOKEN=secret\n")
        try:
            cfg = api.load_config(f.name)
        finally:
            os.unlink(f.name)
        self.assertEqual(cfg.grafana_url, "http://localhost:3000")
        self.assertEqual(cfg.ds_uid, "P1")
        self.assertEqual(cfg.cluster, "otel")
        self.assertEqual(cfg.token, "secret")

    def test_missing_file_gives_defaults_and_no_ds(self):
        cfg = api.load_config("/nonexistent/local.env")
        self.assertEqual(cfg.grafana_url, "http://localhost:3000")
        self.assertIsNone(cfg.ds_uid)


class QueryBody(unittest.TestCase):
    def test_range_covers_the_verification_window(self):
        body = api.ds_query_body("SELECT 1", "table", "P1", seconds=7 * 86400)
        self.assertEqual(body["from"], "now-604800s")
        self.assertEqual(body["to"], "now")
        self.assertEqual(body["queries"][0]["query"], "SELECT 1 FORMAT JSON")


class RowsFromFrames(unittest.TestCase):
    def test_frames_to_rows(self):
        frames = [
            {"schema": {"fields": [{"name": "h", "type": "string"}]}, "data": {"values": [["a", "b"]]}},
            {"schema": {"fields": [{"name": "n", "type": "number"}]}, "data": {"values": [[1, 2]]}},
        ]
        self.assertEqual(api.rows_from_frames(frames), [{"h": "a", "n": 1}, {"h": "b", "n": 2}])

    def test_multi_field_frame(self):
        frames = [{"schema": {"fields": [{"name": "h"}, {"name": "n"}]}, "data": {"values": [["a"], [1]]}}]
        self.assertEqual(api.rows_from_frames(frames), [{"h": "a", "n": 1}])


if __name__ == "__main__":
    unittest.main()
