# Non-doc debris (Phase 0)

Repo-root junk that is OUT of the doc corpus but needs later cleanup. No action taken now.

- `03371_qbit_read_write_test_*.clickhouse` — stray test artifacts left by a stateless test run.
- `__cache__/` — build/tool cache directory.
- `b170_smoke_pool/` — leftover smoke-test pool directory from the `content_addressed_log` (B170) work.
- `disks/` — leftover disk-state directory from local CAS testing.
- `config_file_for_test.xml` — stray test config file.
- `compare_clickhouse_version*` (`compare_clickhouse_version.sh`, `compare_clickhouse_version_25.3.result`, `compare_clickhouse_version_26.3.result`) — version-comparison scratch files.
- `tmp/` — the repo's designated scratch directory (see `CLAUDE.md`); contains raw source/diff dumps from earlier archaeology sessions, not documentation prose.
- `trash/` — quarantine directory for removed/vendored material (e.g. `trash/contrib/rust_vendor/...`); its Rust-crate `CHANGELOG`/`README` files matched the CAS detector only via the unrelated compare-and-swap sense of "CAS".

## Found during Gate M supplementary audit (2026-08-04) {#audit-debris}
- `docs/superpowers/worklogs/2026-07-21-unattended-consistency-f1f11.md` — 0-byte duplicate of the `f1-f11` file.
- Case-collision worklog artifact (`CURrent.md` vs `CURRENT.md`) — skipped by audit, needs removal.
- `docs/superpowers/reports/2026-07-28-ref-rework-reviews/codex_r6_findings.md` — 94% of the file (L168-2474) is an accidentally-embedded `git diff` dump of a deleted tmp symbol-map file.
