#!/usr/bin/env python3
"""
pyLabHub Demo Runner.

Spawns the processes declared in a demo's `demo.manifest.json`,
captures their stdout/stderr + role log files, waits for declared
startup markers, runs for a configurable duration, sends a graceful
shutdown signal, and evaluates the captured output against the
manifest's declared criteria.

The same runner drives every demo (single-hub, dual-hub, future
variants).  A demo is described by a single manifest — no code changes
to the runner per new demo.

Usage:
    python3 runner.py <demo_dir>
    python3 runner.py <demo_dir> --duration 30 --report report.json
    python3 runner.py --all                                # every demo with a manifest under share/
    python3 runner.py --list                               # list registered demos and exit

Exit codes:
    0    all evaluators passed
    1    one or more evaluators failed
    2    demo failed to reach steady state / a process crashed early
    3    invalid manifest or runner usage error
"""

from __future__ import annotations

import argparse
import json
import os
import re
import signal
import subprocess
import sys
import time
from dataclasses import dataclass, field
from pathlib import Path
from typing import Any, Callable, Dict, List, Optional

REPO_ROOT = Path(__file__).resolve().parent.parent.parent  # share/demo_framework/runner.py → repo root
SHARE_DIR = REPO_ROOT / "share"
DEFAULT_BIN_DIR = REPO_ROOT / "build" / "stage-debug" / "bin"


# ─────────────────────────────────────────────────────────────────────────────
# Manifest dataclasses
# ─────────────────────────────────────────────────────────────────────────────

@dataclass
class ProcessSpec:
    """One process declared in a demo manifest."""
    name: str
    binary: str
    args: List[str] = field(default_factory=list)
    cwd: Optional[str] = None
    wait_for_marker: Optional[str] = None
    startup_timeout_s: float = 10.0
    depends_on: List[str] = field(default_factory=list)
    role_log_glob: Optional[str] = None  # e.g. "hub/logs/*.log" relative to demo_dir


@dataclass
class EvaluatorSpec:
    """One evaluator block from a manifest."""
    type: str
    params: Dict[str, Any] = field(default_factory=dict)


@dataclass
class SetupCommandSpec:
    """One one-shot setup command run before the demo's long-lived
    processes are spawned.  Used for first-run actions like
    `plh_hub --keygen` to provision the hub's CurveZMQ vault."""
    args: List[str]
    cwd: Optional[str] = None
    env: Dict[str, str] = field(default_factory=dict)
    timeout_s: float = 30.0
    must_succeed: bool = True


@dataclass
class DemoManifest:
    """Parsed demo.manifest.json."""
    name: str
    description: str = ""
    processes: List[ProcessSpec] = field(default_factory=list)
    duration_s: float = 10.0
    shutdown_signal: str = "SIGTERM"
    shutdown_grace_s: float = 5.0
    evaluators: List[EvaluatorSpec] = field(default_factory=list)
    expected_warnings: List[str] = field(default_factory=list)
    expected_errors: List[str] = field(default_factory=list)
    setup_commands: List[SetupCommandSpec] = field(default_factory=list)
    setup_env: Dict[str, str] = field(default_factory=dict)


def load_manifest(manifest_path: Path) -> DemoManifest:
    raw = json.loads(manifest_path.read_text())
    procs = [
        ProcessSpec(
            name=p["name"],
            binary=p["binary"],
            args=p.get("args", []),
            cwd=p.get("cwd"),
            wait_for_marker=p.get("wait_for_marker"),
            startup_timeout_s=p.get("startup_timeout_s", 10.0),
            depends_on=p.get("depends_on", []) if isinstance(p.get("depends_on", []), list)
                       else [p["depends_on"]],
            role_log_glob=p.get("role_log_glob"),
        )
        for p in raw["processes"]
    ]
    evs = [
        EvaluatorSpec(type=e["type"], params={k: v for k, v in e.items() if k != "type"})
        for e in raw.get("evaluators", [])
    ]
    setup_block = raw.get("setup", {})
    setup_env = dict(setup_block.get("env", {}))
    setup_cmds = [
        SetupCommandSpec(
            args=c["args"],
            cwd=c.get("cwd"),
            env={**setup_env, **c.get("env", {})},
            timeout_s=c.get("timeout_s", 30.0),
            must_succeed=c.get("must_succeed", True),
        )
        for c in setup_block.get("commands", [])
    ]
    return DemoManifest(
        name=raw["name"],
        description=raw.get("description", ""),
        processes=procs,
        duration_s=raw.get("duration_s", 10.0),
        shutdown_signal=raw.get("shutdown_signal", "SIGTERM"),
        shutdown_grace_s=raw.get("shutdown_grace_s", 5.0),
        evaluators=evs,
        expected_warnings=raw.get("expected_warnings", []),
        expected_errors=raw.get("expected_errors", []),
        setup_commands=setup_cmds,
        setup_env=setup_env,
    )


# ─────────────────────────────────────────────────────────────────────────────
# Process orchestration
# ─────────────────────────────────────────────────────────────────────────────

@dataclass
class RunningProcess:
    spec: ProcessSpec
    popen: subprocess.Popen
    stdout_path: Path
    stderr_path: Path
    started_ok: bool = False
    exit_code: Optional[int] = None
    crash_detected_during_startup: bool = False


class DemoRun:
    """One run of one demo.  Owns spawned processes and capture files."""

    def __init__(
        self,
        demo_dir: Path,
        manifest: DemoManifest,
        bin_dir: Path,
        run_dir: Path,
    ) -> None:
        self.demo_dir = demo_dir.resolve()
        self.manifest = manifest
        self.bin_dir = bin_dir.resolve()
        self.run_dir = run_dir.resolve()
        self.run_dir.mkdir(parents=True, exist_ok=True)
        self.processes: Dict[str, RunningProcess] = {}
        self._open_files: List = []  # keep handles alive for the run

    # ── Setup phase ────────────────────────────────────────────────────────

    def run_setup(self) -> None:
        """Run one-shot setup commands declared in the manifest (e.g.
        `plh_hub --keygen` to provision the hub's CurveZMQ vault).  Each
        command runs with the demo_dir as cwd by default and inherits the
        manifest's setup_env merged with the parent process env.  A
        non-zero exit raises SetupFailure unless `must_succeed=false`."""
        if not self.manifest.setup_commands:
            return
        print(f"[runner] running {len(self.manifest.setup_commands)} setup "
              f"command(s)")
        for i, cmd in enumerate(self.manifest.setup_commands):
            resolved_args = [self._resolve_arg(a) for a in cmd.args]
            # If the first arg matches a known binary, resolve against bin_dir.
            if resolved_args and not Path(resolved_args[0]).is_absolute():
                candidate = self.bin_dir / resolved_args[0]
                if candidate.exists():
                    resolved_args[0] = str(candidate)
            cwd = self.demo_dir if cmd.cwd is None else (self.demo_dir / cmd.cwd).resolve()
            env = {**os.environ, **cmd.env}
            print(f"[runner]   setup[{i}]: {' '.join(resolved_args)} (cwd={cwd})")
            try:
                result = subprocess.run(
                    resolved_args, cwd=str(cwd), env=env,
                    capture_output=True, text=True, timeout=cmd.timeout_s)
            except subprocess.TimeoutExpired:
                if cmd.must_succeed:
                    raise SetupFailure(
                        f"setup command {i} ({resolved_args}) timed out "
                        f"after {cmd.timeout_s}s")
                print(f"[runner]   setup[{i}]: TIMEOUT (must_succeed=false; continuing)")
                continue
            if result.returncode != 0:
                stderr_tail = (result.stderr or "").splitlines()[-5:]
                if cmd.must_succeed:
                    raise SetupFailure(
                        f"setup command {i} ({resolved_args}) failed with rc="
                        f"{result.returncode}\nstderr tail:\n  " +
                        "\n  ".join(stderr_tail))
                print(f"[runner]   setup[{i}]: rc={result.returncode} "
                      f"(must_succeed=false; continuing)")
            else:
                print(f"[runner]   setup[{i}]: ok")

    # ── Spawning ───────────────────────────────────────────────────────────

    def launch(self) -> None:
        """Spawn every process in topological order, waiting for markers."""
        ordered = self._topo_sort(self.manifest.processes)
        for spec in ordered:
            self._spawn(spec)
            if spec.wait_for_marker:
                ok = self._wait_for_marker(spec)
                if not ok:
                    raise SteadyStateFailure(
                        f"process '{spec.name}' did not reach steady state "
                        f"(marker '{spec.wait_for_marker}' not seen in "
                        f"{spec.startup_timeout_s}s)"
                    )

    def _topo_sort(self, procs: List[ProcessSpec]) -> List[ProcessSpec]:
        by_name = {p.name: p for p in procs}
        ordered: List[ProcessSpec] = []
        seen: set = set()
        visiting: set = set()

        def visit(name: str) -> None:
            if name in seen:
                return
            if name in visiting:
                raise ValueError(f"manifest has dependency cycle through '{name}'")
            visiting.add(name)
            spec = by_name[name]
            for dep in spec.depends_on:
                if dep not in by_name:
                    raise ValueError(f"process '{name}' depends on unknown '{dep}'")
                visit(dep)
            visiting.discard(name)
            seen.add(name)
            ordered.append(spec)

        for p in procs:
            visit(p.name)
        return ordered

    def _spawn(self, spec: ProcessSpec) -> None:
        bin_path = self.bin_dir / spec.binary
        if not bin_path.exists():
            raise FileNotFoundError(f"binary not found: {bin_path}")
        args = [self._resolve_arg(a) for a in spec.args]
        cmd = [str(bin_path)] + args

        cwd = self.demo_dir if spec.cwd is None else (self.demo_dir / spec.cwd).resolve()
        cwd.mkdir(parents=True, exist_ok=True)

        stdout_path = self.run_dir / f"{spec.name}.stdout.log"
        stderr_path = self.run_dir / f"{spec.name}.stderr.log"
        stdout_f = open(stdout_path, "w")
        stderr_f = open(stderr_path, "w")
        self._open_files += [stdout_f, stderr_f]

        # Inherit the parent env, overlaid with the manifest's setup_env
        # so long-lived processes also see things like PYLABHUB_HUB_PASSWORD
        # (the hub process needs it at run time to unlock the vault).
        env = {**os.environ, **self.manifest.setup_env}

        print(f"[runner] spawning '{spec.name}': {' '.join(cmd)} (cwd={cwd})")
        popen = subprocess.Popen(
            cmd,
            cwd=str(cwd),
            stdout=stdout_f,
            stderr=stderr_f,
            env=env,
            # Forward signals to children; default is fine for SIGTERM at shutdown.
        )
        self.processes[spec.name] = RunningProcess(
            spec=spec,
            popen=popen,
            stdout_path=stdout_path,
            stderr_path=stderr_path,
        )

    def _resolve_arg(self, arg: str) -> str:
        """Expand $REPO_ROOT, $DEMO_DIR, $BIN_DIR placeholders.  Otherwise pass through."""
        return (arg
                .replace("$REPO_ROOT", str(REPO_ROOT))
                .replace("$DEMO_DIR", str(self.demo_dir))
                .replace("$BIN_DIR", str(self.bin_dir)))

    # ── Marker waiting ─────────────────────────────────────────────────────

    def _wait_for_marker(self, spec: ProcessSpec) -> bool:
        """Poll captured logs (stdout/stderr + role log glob) until marker
        regex is seen or startup timeout expires.  Returns False if the
        process died before the marker appeared."""
        assert spec.wait_for_marker is not None
        deadline = time.monotonic() + spec.startup_timeout_s
        marker_re = re.compile(spec.wait_for_marker)
        running = self.processes[spec.name]

        while time.monotonic() < deadline:
            for log_path in self._candidate_logs(spec):
                if log_path.exists():
                    try:
                        text = log_path.read_text(errors="replace")
                    except OSError:
                        continue
                    if marker_re.search(text):
                        running.started_ok = True
                        return True
            rc = running.popen.poll()
            if rc is not None:
                running.crash_detected_during_startup = True
                running.exit_code = rc
                return False
            time.sleep(0.1)
        return False

    def _candidate_logs(self, spec: ProcessSpec) -> List[Path]:
        rp = self.processes.get(spec.name)
        if rp is None:
            return []
        candidates = [rp.stdout_path, rp.stderr_path]
        if spec.role_log_glob:
            candidates += list(self.demo_dir.glob(spec.role_log_glob))
        return candidates

    # ── Lifecycle ──────────────────────────────────────────────────────────

    def run_for(self, duration_s: float) -> None:
        end = time.monotonic() + duration_s
        while time.monotonic() < end:
            # Surface crashes during the run window — don't hide them.
            for rp in self.processes.values():
                rc = rp.popen.poll()
                if rc is not None and rp.exit_code is None:
                    rp.exit_code = rc
                    print(f"[runner] WARNING: process '{rp.spec.name}' exited "
                          f"during run window (rc={rc})")
            time.sleep(0.2)

    def shutdown(self) -> None:
        sig = getattr(signal, self.manifest.shutdown_signal, signal.SIGTERM)
        # Shutdown in reverse-topological order so roles get a chance
        # to deregister before brokers go down.
        ordered = list(reversed(self._topo_sort(self.manifest.processes)))
        for spec in ordered:
            rp = self.processes.get(spec.name)
            if rp and rp.exit_code is None:
                try:
                    rp.popen.send_signal(sig)
                except ProcessLookupError:
                    pass
        deadline = time.monotonic() + self.manifest.shutdown_grace_s
        for spec in ordered:
            rp = self.processes.get(spec.name)
            if rp and rp.exit_code is None:
                remaining = max(0.0, deadline - time.monotonic())
                try:
                    rp.exit_code = rp.popen.wait(timeout=remaining)
                except subprocess.TimeoutExpired:
                    print(f"[runner] WARNING: '{spec.name}' did not exit on "
                          f"{self.manifest.shutdown_signal} within "
                          f"{self.manifest.shutdown_grace_s}s; sending SIGKILL")
                    rp.popen.kill()
                    rp.exit_code = rp.popen.wait()
        for f in self._open_files:
            try:
                f.close()
            except Exception:
                pass


class SteadyStateFailure(RuntimeError):
    pass


class SetupFailure(RuntimeError):
    pass


# ─────────────────────────────────────────────────────────────────────────────
# Evaluation
# ─────────────────────────────────────────────────────────────────────────────

@dataclass
class CheckResult:
    name: str
    passed: bool
    detail: str = ""


def collect_logs_for(run: DemoRun, process_name: str) -> str:
    """Concatenate stdout + stderr + role log files for one process."""
    rp = run.processes.get(process_name)
    if rp is None:
        return ""
    parts: List[str] = []
    for p in [rp.stdout_path, rp.stderr_path]:
        if p.exists():
            parts.append(p.read_text(errors="replace"))
    if rp.spec.role_log_glob:
        for p in run.demo_dir.glob(rp.spec.role_log_glob):
            if p.is_file():
                parts.append(p.read_text(errors="replace"))
    return "\n".join(parts)


# Evaluator registry; each takes (run, params) and returns CheckResult.
EVALUATORS: Dict[str, Callable[["DemoRun", Dict[str, Any]], CheckResult]] = {}


def evaluator(name: str):
    def deco(fn):
        EVALUATORS[name] = fn
        return fn
    return deco


@evaluator("log_marker_min_count")
def _ev_log_marker_min_count(run: DemoRun, params: Dict[str, Any]) -> CheckResult:
    proc = params["process"]
    marker = params["marker"]
    min_count = int(params["min"])
    text = collect_logs_for(run, proc)
    count = len(re.findall(marker, text))
    return CheckResult(
        name=f"log_marker_min_count[{proc}:'{marker}']",
        passed=count >= min_count,
        detail=f"matched {count} times (required ≥{min_count})",
    )


@evaluator("log_marker_present")
def _ev_log_marker_present(run: DemoRun, params: Dict[str, Any]) -> CheckResult:
    proc = params["process"]
    marker = params["marker"]
    text = collect_logs_for(run, proc)
    found = re.search(marker, text) is not None
    return CheckResult(
        name=f"log_marker_present[{proc}:'{marker}']",
        passed=found,
        detail="found" if found else "NOT found",
    )


@evaluator("no_unexpected_errors")
def _ev_no_unexpected_errors(run: DemoRun, params: Dict[str, Any]) -> CheckResult:
    """Walk every process's logs; any ERROR/FATAL line not in
    `expected_errors` (manifest-level) is an unexpected error."""
    pattern = re.compile(r"\[(ERROR|FATAL)[^\]]*\][^\n]*")
    whitelist = [re.compile(p) for p in run.manifest.expected_errors]
    unexpected: List[str] = []
    for proc_name in run.processes:
        text = collect_logs_for(run, proc_name)
        for line in pattern.finditer(text):
            line_str = line.group(0)
            if any(w.search(line_str) for w in whitelist):
                continue
            unexpected.append(f"[{proc_name}] {line_str}")
    detail = "\n".join(unexpected[:5]) if unexpected else "none"
    return CheckResult(
        name="no_unexpected_errors",
        passed=not unexpected,
        detail=f"{len(unexpected)} unexpected error(s); first 5:\n{detail}",
    )


@evaluator("all_processes_started")
def _ev_all_processes_started(run: DemoRun, params: Dict[str, Any]) -> CheckResult:
    failed = [name for name, rp in run.processes.items() if not rp.started_ok and rp.spec.wait_for_marker]
    return CheckResult(
        name="all_processes_started",
        passed=not failed,
        detail=f"failed to start: {failed}" if failed else "all started ok",
    )


@evaluator("clean_shutdown")
def _ev_clean_shutdown(run: DemoRun, params: Dict[str, Any]) -> CheckResult:
    """Every process exited with rc == 0 or a documented graceful-signal rc."""
    # SIGTERM-terminated subprocess has rc == -signal_number on POSIX.
    sig = getattr(signal, run.manifest.shutdown_signal, signal.SIGTERM)
    acceptable = {0, -sig.value}
    bad = {name: rp.exit_code for name, rp in run.processes.items()
           if rp.exit_code not in acceptable}
    return CheckResult(
        name="clean_shutdown",
        passed=not bad,
        detail=f"unclean exits: {bad}" if bad else "all exited cleanly",
    )


@evaluator("count_sequence_e2e")
def _ev_count_sequence_e2e(run: DemoRun, params: Dict[str, Any]) -> CheckResult:
    """Extract integer counts from producer + consumer logs via two
    regexes (`producer_pattern` and `consumer_pattern`, each with a
    single integer capture group); verify the consumer counts form a
    non-empty subsequence of the producer counts with no out-of-order
    gaps beyond `max_gap`."""
    prod_text = collect_logs_for(run, params["producer"])
    cons_text = collect_logs_for(run, params["consumer"])
    prod_pat = re.compile(params["producer_pattern"])
    cons_pat = re.compile(params["consumer_pattern"])
    prod_counts = [int(m.group(1)) for m in prod_pat.finditer(prod_text)]
    cons_counts = [int(m.group(1)) for m in cons_pat.finditer(cons_text)]
    if not cons_counts:
        return CheckResult(name="count_sequence_e2e", passed=False,
                            detail="consumer log has no count entries")
    if not prod_counts:
        return CheckResult(name="count_sequence_e2e", passed=False,
                            detail="producer log has no count entries")
    # Consumer counts must be monotone non-decreasing (drops allowed).
    for prev, cur in zip(cons_counts, cons_counts[1:]):
        if cur < prev:
            return CheckResult(name="count_sequence_e2e", passed=False,
                                detail=f"consumer count went backwards: {prev} → {cur}")
    # Every consumer count must have appeared in the producer log.
    prod_set = set(prod_counts)
    missing = [c for c in cons_counts if c not in prod_set]
    if missing:
        return CheckResult(name="count_sequence_e2e", passed=False,
                            detail=f"{len(missing)} consumer count(s) never appeared in producer log "
                                   f"(first 5: {missing[:5]})")
    return CheckResult(
        name="count_sequence_e2e",
        passed=True,
        detail=f"producer wrote {len(prod_counts)} entries; consumer saw "
               f"{len(cons_counts)} entries; first prod={prod_counts[0]}, last cons={cons_counts[-1]}",
    )


def run_evaluators(run: DemoRun) -> List[CheckResult]:
    results: List[CheckResult] = []
    for ev in run.manifest.evaluators:
        fn = EVALUATORS.get(ev.type)
        if fn is None:
            results.append(CheckResult(
                name=f"<unknown evaluator '{ev.type}'>",
                passed=False,
                detail="no such evaluator registered",
            ))
            continue
        try:
            results.append(fn(run, ev.params))
        except Exception as exc:
            results.append(CheckResult(
                name=f"{ev.type}",
                passed=False,
                detail=f"evaluator raised: {exc!r}",
            ))
    return results


# ─────────────────────────────────────────────────────────────────────────────
# Reporting
# ─────────────────────────────────────────────────────────────────────────────

def emit_report(run: DemoRun, results: List[CheckResult], report_path: Optional[Path]) -> bool:
    overall = all(r.passed for r in results) and bool(results)
    print()
    print(f"=== Demo report — {run.manifest.name} ===")
    print(f"Demo dir: {run.demo_dir}")
    print(f"Run dir:  {run.run_dir}")
    print(f"Duration: {run.manifest.duration_s}s")
    print()
    print("Processes:")
    for name, rp in run.processes.items():
        print(f"  - {name}: started_ok={rp.started_ok}  exit_code={rp.exit_code}")
    print()
    print("Evaluators:")
    for r in results:
        status = "PASS" if r.passed else "FAIL"
        print(f"  [{status}] {r.name}")
        if r.detail and (not r.passed or len(r.detail) < 200):
            for line in r.detail.splitlines():
                print(f"           {line}")
    print()
    print(f"Overall: {'PASS' if overall else 'FAIL'}")
    if report_path:
        report_path.parent.mkdir(parents=True, exist_ok=True)
        report_path.write_text(json.dumps({
            "demo": run.manifest.name,
            "demo_dir": str(run.demo_dir),
            "run_dir": str(run.run_dir),
            "duration_s": run.manifest.duration_s,
            "processes": {
                name: {
                    "started_ok": rp.started_ok,
                    "exit_code": rp.exit_code,
                    "stdout_log": str(rp.stdout_path),
                    "stderr_log": str(rp.stderr_path),
                }
                for name, rp in run.processes.items()
            },
            "evaluators": [
                {"name": r.name, "passed": r.passed, "detail": r.detail}
                for r in results
            ],
            "overall": overall,
        }, indent=2))
        print(f"JSON report: {report_path}")
    return overall


# ─────────────────────────────────────────────────────────────────────────────
# CLI
# ─────────────────────────────────────────────────────────────────────────────

def find_manifests() -> List[Path]:
    return sorted(SHARE_DIR.glob("py-demo-*/demo.manifest.json"))


def main(argv: List[str]) -> int:
    ap = argparse.ArgumentParser(
        description=(__doc__ or "").splitlines()[1] if __doc__ else "")
    g = ap.add_mutually_exclusive_group(required=True)
    g.add_argument("demo_dir", nargs="?", help="Demo directory containing demo.manifest.json")
    g.add_argument("--all", action="store_true",
                   help="Run every demo with a manifest under share/")
    g.add_argument("--list", action="store_true",
                   help="List registered demos and exit")
    ap.add_argument("--bin-dir", default=str(DEFAULT_BIN_DIR),
                     help=f"Directory containing plh_hub/plh_role binaries (default: {DEFAULT_BIN_DIR})")
    ap.add_argument("--duration", type=float, default=None,
                     help="Override manifest's duration_s")
    ap.add_argument("--report", default=None,
                     help="Write JSON report to this path")
    ap.add_argument("--run-dir", default=None,
                     help="Directory for captured stdout/stderr (default: <demo>/.runs/<timestamp>)")
    args = ap.parse_args(argv)

    if args.list:
        for m in find_manifests():
            print(m.parent.relative_to(REPO_ROOT))
        return 0

    if args.all:
        manifests = find_manifests()
        if not manifests:
            print("No demo manifests found under share/py-demo-*/", file=sys.stderr)
            return 3
        any_fail = False
        for m in manifests:
            print(f"\n────── Running {m.parent.name} ──────\n")
            rc = run_one(m.parent, args)
            any_fail = any_fail or (rc != 0)
        return 1 if any_fail else 0

    demo_dir = Path(args.demo_dir).resolve()
    manifest_path = demo_dir / "demo.manifest.json"
    if not manifest_path.exists():
        print(f"Manifest not found: {manifest_path}", file=sys.stderr)
        return 3
    return run_one(demo_dir, args)


def run_one(demo_dir: Path, args: argparse.Namespace) -> int:
    manifest_path = demo_dir / "demo.manifest.json"
    try:
        manifest = load_manifest(manifest_path)
    except (json.JSONDecodeError, KeyError) as exc:
        print(f"Invalid manifest at {manifest_path}: {exc}", file=sys.stderr)
        return 3
    if args.duration is not None:
        manifest.duration_s = float(args.duration)

    ts = time.strftime("%Y%m%d-%H%M%S")
    run_dir = (Path(args.run_dir) if args.run_dir
               else demo_dir / ".runs" / ts).resolve()

    bin_dir = Path(args.bin_dir).resolve()
    if not bin_dir.exists():
        print(f"Bin dir does not exist: {bin_dir}", file=sys.stderr)
        return 3

    run = DemoRun(demo_dir, manifest, bin_dir, run_dir)
    try:
        run.run_setup()
    except SetupFailure as exc:
        print(f"[runner] setup failure: {exc}", file=sys.stderr)
        return 2
    except FileNotFoundError as exc:
        print(f"[runner] {exc}", file=sys.stderr)
        return 3
    try:
        run.launch()
    except SteadyStateFailure as exc:
        print(f"[runner] steady-state failure: {exc}", file=sys.stderr)
        run.shutdown()
        results = run_evaluators(run)
        emit_report(run, results,
                    Path(args.report) if args.report else None)
        return 2
    except FileNotFoundError as exc:
        print(f"[runner] {exc}", file=sys.stderr)
        return 3

    run.run_for(manifest.duration_s)
    run.shutdown()
    results = run_evaluators(run)
    overall = emit_report(run, results,
                          Path(args.report) if args.report else None)
    return 0 if overall else 1


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
