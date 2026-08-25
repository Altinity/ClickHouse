"""Verdict / ScenarioResult model + report.md and report.json writers.

A `Verdict` is one assertion outcome: a metric, its expected condition, the observed value, and a
status. A scenario's overall status is the worst of its verdicts (`fail` > `inconclusive` > `pass`),
unless the scenario is a negative test that declares an expected exception.

The README forbids silently converting a missing observation into a pass: an assertion whose data is
unavailable must be recorded as `inconclusive` with a reason, never dropped.
"""

import json
import time
from dataclasses import dataclass, field, asdict

PASS = "pass"
FAIL = "fail"
INCONCLUSIVE = "inconclusive"
SKIPPED = "skipped"

# Ordering for "worst status wins": higher = worse.
_RANK = {PASS: 0, SKIPPED: 1, INCONCLUSIVE: 2, FAIL: 3}


@dataclass
class Verdict:
    name: str
    expected: str
    observed: str
    status: str
    note: str = ""

    @staticmethod
    def check(name: str, expected: str, observed, ok: bool, note: str = "") -> "Verdict":
        return Verdict(name, expected, str(observed), PASS if ok else FAIL, note)

    @staticmethod
    def inconclusive(name: str, expected: str, reason: str) -> "Verdict":
        return Verdict(name, expected, "unavailable", INCONCLUSIVE, reason)

    @staticmethod
    def skipped(name: str, reason: str) -> "Verdict":
        return Verdict(name, "(not run)", "skipped", SKIPPED, reason)

    @staticmethod
    def reported(name: str, expected: str, observed, note: str = "") -> "Verdict":
        """A recorded observation that never gates the run status.

        Use ONLY where the metric is non-gating BY DESIGN (a characterisation number, or a signal
        that is structurally zero in the current build). Never as a way to soften an assertion that
        should fail, and never for data that is UNAVAILABLE -- that stays `inconclusive`, per the
        README rule that a missing observation must never be silently converted into a pass.
        """
        return Verdict(name, expected, str(observed), PASS, note)


def worst_status(verdicts) -> str:
    if not verdicts:
        return INCONCLUSIVE
    return max((v.status for v in verdicts), key=lambda s: _RANK.get(s, 0))


@dataclass
class ScenarioResult:
    scenario: str
    title: str
    priority: str
    seed: int
    params: dict = field(default_factory=dict)
    verdicts: list = field(default_factory=list)
    observations: dict = field(default_factory=dict)
    anomalies: list = field(default_factory=list)
    timings: dict = field(default_factory=dict)
    status: str = INCONCLUSIVE
    started_utc: str = ""
    ended_utc: str = ""
    error: str = ""
    git: dict = field(default_factory=dict)

    def add(self, v: Verdict) -> Verdict:
        self.verdicts.append(v)
        return v

    def note_anomaly(self, text: str) -> None:
        self.anomalies.append(text)

    def finalize(self, explicit_status: str | None = None) -> None:
        self.status = explicit_status or worst_status(self.verdicts)

    # --- serialization -----------------------------------------------------------
    def to_json(self) -> dict:
        d = asdict(self)
        return d

    def to_markdown(self) -> str:
        lines = []
        lines.append(f"# {self.scenario}: {self.title}")
        lines.append("")
        lines.append(f"- **Priority:** {self.priority}")
        lines.append(f"- **Status:** `{self.status.upper()}`")
        lines.append(f"- **Seed:** {self.seed}")
        lines.append(f"- **Started (UTC):** {self.started_utc}")
        lines.append(f"- **Ended (UTC):** {self.ended_utc}")
        if self.git:
            lines.append(f"- **Git:** {self.git.get('branch','?')} @ {self.git.get('sha','?')[:12]}"
                         f"{' (dirty)' if self.git.get('dirty') else ''}")
        lines.append("")
        if self.error:
            lines.append("## Error")
            lines.append("")
            lines.append("```")
            lines.append(self.error.strip())
            lines.append("```")
            lines.append("")

        lines.append("## Parameters")
        lines.append("")
        lines.append("```json")
        lines.append(json.dumps(self.params, indent=2, default=str))
        lines.append("```")
        lines.append("")

        lines.append("## Budget verdict")
        lines.append("")
        lines.append("| metric | expected | observed | verdict |")
        lines.append("|---|---|---|---|")
        for v in self.verdicts:
            note = f" — {v.note}" if v.note else ""
            lines.append(f"| {v.name} | {v.expected} | {v.observed} | "
                         f"{v.status}{note} |")
        lines.append("")

        if self.timings:
            lines.append("## Timings")
            lines.append("")
            for k, val in self.timings.items():
                lines.append(f"- {k}: {val}")
            lines.append("")

        if self.observations:
            lines.append("## Observations")
            lines.append("")
            lines.append("```json")
            lines.append(json.dumps(self.observations, indent=2, default=str))
            lines.append("```")
            lines.append("")

        if self.anomalies:
            lines.append("## Anomalies")
            lines.append("")
            for a in self.anomalies:
                lines.append(f"- {a}")
            lines.append("")

        return "\n".join(lines)


def write_reports(ctx, result: ScenarioResult) -> None:
    """Write report.json and report.md into the run directory."""
    result.git = ctx.git
    ctx.write_json("report.json", result.to_json())
    ctx.write_text("report.md", result.to_markdown())
