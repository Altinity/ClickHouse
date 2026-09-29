from __future__ import annotations

import importlib.util
import unittest
from pathlib import Path


TOOL_DIR = Path(__file__).resolve().parents[1]


def load(name: str):
    spec = importlib.util.spec_from_file_location(name, TOOL_DIR / f"{name}.py")
    module = importlib.util.module_from_spec(spec)
    assert spec.loader is not None
    spec.loader.exec_module(module)
    return module


collector = load("collect_release_evidence")
generator = load("generate_final_sbom")


class BuildEvidenceTests(unittest.TestCase):
    def test_parses_only_compilation_or_link_lines(self):
        log = """
checking out contrib/not-shipped/README.md
[21/99] Building CXX object contrib/fmtlib/CMakeFiles/fmt.dir/src/format.cc.o
clang++ -I/work/contrib/arrow/cpp/src -c /work/contrib/arrow/cpp/src/arrow.cc
[22/99] Linking CXX static library contrib/zstd/libzstd.a
"""
        self.assertEqual(
            collector.parse_build_components(log),
            {"fmtlib", "arrow", "zstd"},
        )

    def test_ignores_source_checkout_noise(self):
        self.assertEqual(
            collector.parse_build_components("Submodule path 'contrib/googletest': checked out 'abc'"),
            set(),
        )


class GeneratorTests(unittest.TestCase):
    def test_license_normalization(self):
        self.assertEqual(generator.combine_licenses(["Apache"]), "Apache-2.0")
        self.assertEqual(
            generator.combine_licenses(["MIT", "Apache-2.0"]),
            "(Apache-2.0) AND (MIT)",
        )

    def test_spdx_identifier_is_safe(self):
        self.assertEqual(generator.spdx_id("Package-cargo:foo@1.2.3"), "SPDXRef-Package-cargo-foo-1.2.3")


if __name__ == "__main__":
    unittest.main()
