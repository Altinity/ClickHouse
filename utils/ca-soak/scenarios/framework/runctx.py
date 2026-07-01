"""RunContext — per-run identity, directory layout, config snapshot, and logging.

Every scenario run gets a fresh directory `scenarios/runs/<timestamp>/` holding everything the run
produced: logs, metrics, stdout/stderr, the config snapshot, the resolved parameters, the git sha,
the seed, timings, verdicts, and any failure context. The contract (README §"Common run contract")
requires all of this on disk so a run is fully reconstructable after the fact.
"""

import json
import os
import subprocess
import sys
import time
from dataclasses import dataclass, field
from pathlib import Path

# scenarios/framework/runctx.py -> parents: [0]=framework [1]=scenarios [2]=ca-soak [3]=utils [4]=repo
_THIS = Path(__file__).resolve()
SCENARIOS_DIR = _THIS.parents[1]
CA_SOAK_DIR = _THIS.parents[2]
REPO_ROOT = _THIS.parents[4]
RUNS_DIR = SCENARIOS_DIR / "runs"


def git_info() -> dict:
    """Return {sha, branch, dirty} for the repo the binary was built from. Best-effort: a missing
    git returns empty strings rather than raising (the run must still produce a report)."""
    def _git(*args):
        try:
            p = subprocess.run(["git", "-C", str(REPO_ROOT), *args],
                               capture_output=True, text=True, timeout=15)
            return p.stdout.strip() if p.returncode == 0 else ""
        except Exception:
            return ""
    sha = _git("rev-parse", "HEAD")
    branch = _git("rev-parse", "--abbrev-ref", "HEAD")
    dirty = bool(_git("status", "--porcelain"))
    return {"sha": sha, "branch": branch, "dirty": dirty}


@dataclass
class RunContext:
    """Holds run identity + the run directory, and provides logging and file-writing helpers.

    Construct via `RunContext.create(...)` which stamps the timestamp and makes the run directory.
    The `cluster` and `params` fields are populated by the runner before the scenario executes.
    """

    scenario: str
    seed: int
    duration_s: int
    scale: str
    timestamp: str
    run_dir: Path
    params: dict = field(default_factory=dict)
    git: dict = field(default_factory=dict)
    extra: dict = field(default_factory=dict)
    cluster: object = None
    _log_fh: object = field(default=None, repr=False)
    _t0: float = field(default_factory=time.monotonic, repr=False)

    @classmethod
    def create(cls, scenario: str, seed: int, duration_s: int, scale: str,
               timestamp: str | None = None, runs_dir: Path | None = None) -> "RunContext":
        ts = timestamp or time.strftime("%Y%m%dT%H%M%S", time.gmtime())
        base = runs_dir or RUNS_DIR
        run_dir = base / f"{ts}_{scenario}_seed{seed}"
        run_dir.mkdir(parents=True, exist_ok=True)
        ctx = cls(scenario=scenario, seed=seed, duration_s=duration_s, scale=scale,
                  timestamp=ts, run_dir=run_dir, git=git_info())
        ctx._log_fh = open(run_dir / "run.log", "a", buffering=1)
        return ctx

    # --- logging -----------------------------------------------------------------
    def log(self, msg: str) -> None:
        line = f"[{time.strftime('%H:%M:%S')}] [{self.scenario}] {msg}"
        print(line, flush=True)
        if self._log_fh is not None:
            self._log_fh.write(line + "\n")

    def elapsed_s(self) -> float:
        return time.monotonic() - self._t0

    # --- file helpers ------------------------------------------------------------
    def path(self, *parts) -> Path:
        return self.run_dir.joinpath(*parts)

    def subdir(self, name: str) -> Path:
        d = self.run_dir / name
        d.mkdir(parents=True, exist_ok=True)
        return d

    def write_text(self, name: str, text: str) -> Path:
        p = self.run_dir / name
        p.parent.mkdir(parents=True, exist_ok=True)
        p.write_text(text)
        return p

    def write_json(self, name: str, obj) -> Path:
        return self.write_text(name, json.dumps(obj, indent=2, default=str))

    # --- config snapshot ---------------------------------------------------------
    def snapshot_config(self, compose_variant: str | None = None) -> None:
        """Copy the effective config + compose into the run dir so the run is reproducible, and write
        a config.json with the run-identifying knobs (README §"Common observations")."""
        cfgdir = self.subdir("config")
        files = [
            CA_SOAK_DIR / "docker-compose.yml",
            CA_SOAK_DIR / "configs" / "storage_conf.xml",
            CA_SOAK_DIR / "configs" / "storage_conf_gc_shards2.xml",
            CA_SOAK_DIR / "configs" / "rustfs.env",
            CA_SOAK_DIR / "configs" / "ca_gc_log.xml",
            CA_SOAK_DIR / "configs" / "ca_event_log.xml",
            CA_SOAK_DIR / "docker-compose-gc_shards2.yml",
        ]
        for f in files:
            try:
                if f.exists():
                    (cfgdir / f.name).write_text(f.read_text())
            except Exception as e:
                self.log(f"WARN: could not snapshot {f.name}: {e}")
        meta = {
            "scenario": self.scenario,
            "seed": self.seed,
            "duration_s": self.duration_s,
            "scale": self.scale,
            "timestamp_utc": self.timestamp,
            "git": self.git,
            "params": self.params,
            "compose_variant": compose_variant,
            "python": sys.version.split()[0],
            "pid": os.getpid(),
        }
        self.write_json("config.json", meta)

    def close(self):
        if self._log_fh is not None:
            try:
                self._log_fh.close()
            except Exception:
                pass
            self._log_fh = None
