"""Standalone content-addressed adversarial scenario suite.

Each scenario is an independent, focused run that stresses one hard condition of a
`metadata_type = cas` object-storage disk and produces a detailed report. The suite
reuses the `utils/ca-soak` cluster machinery (docker compose: two ClickHouse replicas + RustFS +
Keeper) but is otherwise a separate driver from the mixed deterministic soak in `soak/`.

Entry point: `python3 -m scenarios.run --scenario <name> --seed <seed> --duration 15m`
(run from the `utils/ca-soak` directory so both `soak` and `scenarios` import as sibling packages).
"""
