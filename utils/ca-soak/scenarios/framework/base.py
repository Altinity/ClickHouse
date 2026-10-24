"""Scenario base class + registry.

A scenario declares its identity and run-shape via class attributes and implements `run`. The
framework constructs the `RunContext` and `ScenarioResult`, snapshots config, runs the scenario,
finalizes the verdict, and writes the reports — the scenario body focuses on the workload and its
scenario-specific assertions, calling the shared framework helpers for everything common.

Scale presets: `dev` (fast developer-scale default), `ci` (medium), `full` (spec-scale). A scenario
maps a scale name to concrete parameters via `param_table`; CLI `--param k=v` overrides win last.
"""

from .report import ScenarioResult, Verdict, INCONCLUSIVE

_REGISTRY: dict = {}

SCALES = ("dev", "ci", "full")


def register(cls):
    """Class decorator: register a Scenario subclass under its `name` (case-insensitive)."""
    key = cls.name.upper()
    if key in _REGISTRY:
        raise ValueError(f"duplicate scenario name {cls.name}")
    _REGISTRY[key] = cls
    return cls


def get(name: str):
    return _REGISTRY.get(name.upper())


def all_scenarios() -> dict:
    return dict(_REGISTRY)


def select(spec: str) -> list:
    """Resolve a selection spec into an ordered list of scenario classes.

    Accepts: "all"; a priority ("P0"/"P1"/"P2"); a comma-separated list of names; or one name.
    """
    items = sorted(_REGISTRY.values(), key=lambda c: c.name)
    s = spec.strip()
    if s.lower() == "all":
        return items
    if s.upper() in ("P0", "P1", "P2"):
        return [c for c in items if c.priority.upper() == s.upper()]
    names = [x.strip().upper() for x in s.split(",") if x.strip()]
    out = []
    for n in names:
        c = _REGISTRY.get(n)
        if c is not None:
            out.append(c)
    return out


def _coerce(default, raw: str):
    """Coerce a CLI --param string to the type of the scenario default it overrides."""
    if isinstance(default, bool):
        return raw.lower() in ("1", "true", "yes", "on")
    if isinstance(default, int):
        return int(raw)
    if isinstance(default, float):
        return float(raw)
    return raw


class Scenario:
    name: str = "S00"
    title: str = "base"
    priority: str = "P0"

    # Behavioral flags.
    abandons: bool = False             # deliberately leaves unreachable objects (relaxes leftover check)
    expect_exception: bool = False     # negative test: an `exception` CA-log row is allowed
    requires_stack_attribution: bool = False   # enable trace_log etc. (advisory; config is static here)
    compose_variant = None             # None | "gc_shards2"
    needs_infra: str | None = None     # non-None => scenario cannot run with current infra (inconclusive)

    # scale -> {param: value}. The "dev" row is the default fast preset.
    param_table: dict = {"dev": {}, "ci": {}, "full": {}}

    def resolve_params(self, scale: str, overrides: dict | None = None) -> dict:
        base = dict(self.param_table.get("dev", {}))
        if scale != "dev":
            base.update(self.param_table.get(scale, {}))
        if overrides:
            for k, v in overrides.items():
                if k in base:
                    base[k] = _coerce(base[k], v) if isinstance(v, str) else v
                else:
                    base[k] = v
        return base

    def run(self, ctx, result: ScenarioResult) -> None:
        """Override in subclasses. Populate `result` (verdicts, observations, anomalies). The
        framework finalizes the status and writes reports. `ctx.cluster` is the live cluster."""
        raise NotImplementedError

    # Convenience for needs-infra scenarios: a one-line inconclusive body.
    def run_inconclusive(self, ctx, result: ScenarioResult) -> None:
        reason = self.needs_infra or "infrastructure unavailable"
        result.add(Verdict.inconclusive(self.name, "runnable", reason))
        result.note_anomaly(f"NOT RUN — {reason}")
        result.finalize(INCONCLUSIVE)
