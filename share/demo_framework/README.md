# pyLabHub Demo Framework

A single Python runner (`runner.py`) drives every pyLabHub demo via a
declarative `demo.manifest.json`.  Each demo lives at
`share/py-demo-<name>/` and ships its own manifest, role directories,
and Python scripts.  The runner spawns the declared processes, captures
their output, evaluates the run against declared criteria, and emits a
structured report.

The framework is used for two purposes:

1. **Binary-level integration validation.**  L3 integration tests
   exercise dual-hub through in-process objects; the demo runner
   exercises the same code paths via real `plh_hub` and `plh_role`
   binary processes — closer to production.
2. **Deployment-doc audit.**  Each demo's configs and scripts are
   designed by reading `docs/README/README_Deployment.md` + relevant
   HEPs first.  When a config field can't be derived from the doc, it
   is filed as a documentation gap (not silently copied from existing
   files).

## Usage

```bash
# List registered demos
python3 share/demo_framework/runner.py --list

# Run one demo
python3 share/demo_framework/runner.py share/py-demo-single-processor-shm/

# Run for 30 seconds + JSON report
python3 share/demo_framework/runner.py share/py-demo-dual-processor-bridge/ \
    --duration 30 --report /tmp/dual-hub-report.json

# Run every demo with a manifest
python3 share/demo_framework/runner.py --all
```

Default binary directory is `build/stage-debug/bin/`.  Override with
`--bin-dir <path>` for Release / installed-tree runs.

Captured logs land in `<demo>/.runs/<timestamp>/`.

Exit codes:
- `0` — all evaluators passed.
- `1` — one or more evaluators failed.
- `2` — a process didn't reach steady state / crashed during launch.
- `3` — invalid manifest or runner usage error.

## Manifest schema

```json
{
  "name":            "demo-name",
  "description":     "one-liner",
  "duration_s":      10,
  "shutdown_signal": "SIGTERM",
  "shutdown_grace_s": 5,

  "processes": [
    {
      "name":              "hub",
      "binary":            "plh_hub",
      "args":              ["hub/"],
      "cwd":               null,
      "wait_for_marker":   "Broker: listening on",
      "startup_timeout_s": 10,
      "depends_on":        [],
      "role_log_glob":     "hub/logs/*.log"
    },
    {
      "name":             "producer",
      "binary":           "plh_role",
      "args":             ["--role", "producer", "producer/"],
      "wait_for_marker":  "REG_REQ.*success",
      "depends_on":       ["hub"],
      "role_log_glob":    "producer/logs/*.log"
    }
  ],

  "expected_warnings": [],
  "expected_errors":   [],

  "evaluators": [
    {"type": "all_processes_started"},
    {"type": "log_marker_min_count", "process": "producer",
     "marker": "wrote slot", "min": 50},
    {"type": "log_marker_min_count", "process": "consumer",
     "marker": "received", "min": 50},
    {"type": "count_sequence_e2e", "producer": "producer", "consumer": "consumer",
     "producer_pattern": "wrote slot count=(\\d+)",
     "consumer_pattern": "received count=(\\d+)"},
    {"type": "no_unexpected_errors"},
    {"type": "clean_shutdown"}
  ]
}
```

### Field reference

**Top-level:**

| Field | Type | Required | Meaning |
|---|---|---|---|
| `name` | string | yes | Demo identifier; used in report headers |
| `description` | string | no | One-liner about what the demo proves |
| `duration_s` | float | no, default 10 | Seconds to keep the demo running after all processes reach steady state |
| `shutdown_signal` | string | no, default `SIGTERM` | Signal name to send for graceful shutdown |
| `shutdown_grace_s` | float | no, default 5 | Seconds to wait for processes to exit on the shutdown signal before SIGKILL |
| `processes` | array | yes | One entry per process to spawn (see below) |
| `expected_warnings` / `expected_errors` | array of regex strings | no | Whitelist patterns for `no_unexpected_errors` evaluator |
| `evaluators` | array | no | Checks to run against captured output (see below) |

**Per-process:**

| Field | Type | Required | Meaning |
|---|---|---|---|
| `name` | string | yes | Process identifier (used in evaluators) |
| `binary` | string | yes | Binary name; resolved against `--bin-dir` |
| `args` | array | no | CLI args; supports `$REPO_ROOT`, `$DEMO_DIR`, `$BIN_DIR` placeholders |
| `cwd` | string or null | no, default = `demo_dir` | Working directory; relative to `demo_dir` |
| `wait_for_marker` | regex string | no | If set, runner blocks startup until this regex appears in any captured log; otherwise the process is fire-and-launch |
| `startup_timeout_s` | float | no, default 10 | Time budget for `wait_for_marker` |
| `depends_on` | string or array | no | Names of processes that must be fully launched (marker seen) before this one starts |
| `role_log_glob` | string | no | Glob (relative to demo_dir) matching the process's own log files; marker search and evaluator log collection both check stdout/stderr AND these files |

### Built-in evaluators

| `type` | Required params | Pass condition |
|---|---|---|
| `all_processes_started` | — | Every process with a `wait_for_marker` reached steady state |
| `log_marker_present` | `process`, `marker` | Marker regex matches at least once in `process`'s logs |
| `log_marker_min_count` | `process`, `marker`, `min` | Marker regex matches ≥ `min` times in `process`'s logs |
| `count_sequence_e2e` | `producer`, `consumer`, `producer_pattern`, `consumer_pattern` | Both patterns have a single integer capture group; consumer counts are non-decreasing and every consumer count appeared in the producer log |
| `no_unexpected_errors` | — | No `[ERROR …]` or `[FATAL …]` log lines anywhere, except those matching `expected_errors` whitelist |
| `clean_shutdown` | — | Every process exited with rc == 0 OR rc == −shutdown_signal |

Adding a new evaluator: register a function in `evaluators.py` (or
`runner.py` for built-ins) with `@evaluator("type-name")` and reference
it from the manifest.

## Doc-driven manifest authoring

When creating a new demo manifest, the `args` / config fields / log
markers should be derivable from `docs/README/README_Deployment.md`
and the relevant HEPs.  Where the docs are silent, file an entry in
the active demo-doc audit at
`docs/tech_draft/DEMO_DOC_AUDIT_2026-05-20.md` rather than guessing
from existing files.
