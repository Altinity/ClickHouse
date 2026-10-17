import os
import re
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.abspath(os.path.join(HERE, "..", "..", ".."))


class Docs(unittest.TestCase):
    def test_monitoring_doc_names_every_overview_tile(self):
        from casgrafana import overview
        with open(os.path.join(ROOT, "docs/en/antalya/cas/operations/monitoring.md")) as f:
            doc = f.read()
        self.assertIn("## Dashboards {#dashboards}", doc)
        for t in overview.tiles():
            self.assertIn(f"**{t['title']}**", doc, t["title"])
        self.assertIn("utils/cas-grafana/dashboards/cas-overview.json", doc)
        self.assertIn("utils/cas-grafana/dashboards/cas-drilldown.json", doc)

    def test_readme_has_commands(self):
        with open(os.path.join(HERE, "..", "README.md")) as f:
            readme = f.read()
        for cmd in ("python3 build_dashboards.py", "python3 check_dashboards.py", "python3 push_dashboards.py",
                    "python3 verify_on_grafana.py", "python3 export_dashboards.py"):
            self.assertIn(cmd, readme)
        self.assertTrue(re.search(r"GRANT .*REMOTE", readme))


if __name__ == "__main__":
    unittest.main()
