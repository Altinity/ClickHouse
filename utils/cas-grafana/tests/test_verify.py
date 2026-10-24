import unittest

from casgrafana import verify, panels as P


class Expand(unittest.TestCase):
    def test_expand_multi_value(self):
        sql = "SELECT $timeSeries AS t, sum(x) / $interval FROM clusterAllReplicas('$cluster', system.metric_log) WHERE hostName() IN ($host) AND disk_name IN ($disk) AND $timeFilter GROUP BY t"
        out = verify.expand(sql, cluster="otel", hosts=["h1", "h2"], disks=["cas"], seconds=3600, interval=300)
        self.assertIn("clusterAllReplicas('otel', system.metric_log)", out)
        self.assertIn("hostName() IN ('h1','h2')", out)
        self.assertIn("disk_name IN ('cas')", out)
        self.assertIn("SELECT (intDiv(toUInt32(event_time), 300) * 300) * 1000 AS t,", out)
        self.assertIn("sum(x) / 300", out)
        self.assertIn("event_date >= toDate(now() - 3600) AND event_time >= now() - 3600 AND event_time <= now()", out)
        self.assertNotIn("$", out)

    def test_time_series_macro_expands_without_alias_like_the_plugin(self):
        out = verify.expand("SELECT $timeSeries, 1", cluster="c", hosts=["h"], disks=["d"], seconds=1)
        self.assertEqual(out, "SELECT (intDiv(toUInt32(event_time), 300) * 300) * 1000, 1")

    def test_expand_from_to(self):
        out = verify.expand("WHERE t >= $__from AND t <= $__to", cluster="c", hosts=["h"], disks=["d"], seconds=60)
        self.assertIn("toUnixTimestamp(now() - 60) * 1000", out)
        self.assertIn("toUnixTimestamp(now()) * 1000", out)


class ExpandConstants(unittest.TestCase):
    def test_expand_constants(self):
        out = verify.expand("x * $price_put_per_1000", cluster="c", hosts=["h"], disks=["d"], seconds=1, constants={"price_put_per_1000": "0.005"})
        self.assertEqual(out, "x * 0.005")


class FakeClient:
    def __init__(self, answers):
        self.answers = answers
        self.sent = []

    def ds_query(self, sql, fmt="table", seconds=3600):
        self.sent.append((sql, seconds))
        a = self.answers.pop(0)
        if isinstance(a, Exception):
            raise a
        return a


class VerifyDashboard(unittest.TestCase):
    def test_collects_rows_and_errors(self):
        d = P.dashboard("cas-overview", "CAS / Overview", "d",
                        [P.metric_ts("A", "d", {"a": P.r("S3PutObject")}), P.metric_ts("B", "d", {"b": P.r("S3GetObject")})],
                        tags=[], links=[])
        client = FakeClient([[{"t": 1, "a": 2}], RuntimeError("boom"), [], []])
        res = verify.verify_dashboard(client, d, cluster="otel", hosts=["h"], disks=["cas"], seconds=600)
        self.assertEqual([(r.title, r.rows, r.error is None) for r in res[:2]], [("A", 1, True), ("B", 0, False)])
        self.assertNotIn("$", client.sent[0][0])
        self.assertEqual(client.sent[0][1], 600)
        errors, empties = verify.summarize(res, allow_empty_titles=set())
        self.assertEqual(len(errors), 1)
        self.assertEqual(empties, [])

    def test_empty_is_reported_unless_allowed(self):
        d = P.dashboard("cas-overview", "CAS / Overview", "d", [P.metric_ts("Quiet", "d", {"a": P.r("S3PutObject")})], tags=[], links=[])
        res = verify.verify_dashboard(FakeClient([[], [], []]), d, cluster="c", hosts=["h"], disks=["d"], seconds=1)
        self.assertEqual(verify.summarize(res, allow_empty_titles=set())[1], ["cas-overview/Quiet[A]"])
        self.assertEqual(verify.summarize(res, allow_empty_titles={"Quiet"})[1], [])


if __name__ == "__main__":
    unittest.main()
