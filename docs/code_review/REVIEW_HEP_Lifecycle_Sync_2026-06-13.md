# HEP Lifecycle Sync — Code Review, 2026-06-13

> **Scope.**  Cross-doc sweep triggered by AUTH-1 (task #103) closure.
> AUTH-1 moved data-queue activation from the role host's S1
> infrastructure phase to S3 (post-broker-registration).  This review
> finds every HEP that describes the role-host lifecycle, queue states,
> or the ctrl thread, and verifies each against current code.
>
> **Trigger commits.** AUTH-1 critical path:
>
> | Commit | What changed |
> |---|---|
> | `72021c54` | AUTH-1 critical path: PUSH `apply_master_approval`, drop `setup_infrastructure_` start calls, wire producer S3 |
> | `a9015f78` | `GET_CHANNEL_AUTH_ACK.allowlist` Z85-string shape |
> | `b2d166f8` | Registration failure aborts role startup |
> | `638443e4` | Retire `RoleAPIBase::start_*_queue` public API |
>
> Plus the older Wave-B M4f closure (commit `0c…` series, 2026-05-15+)
> that retired `start_ctrl_thread` → `start_handler_threads`.
>
> **Status.** OPEN — being addressed in this same commit train.

## Source of truth (do not modify in this review)

- **HEP-CORE-0036** §3.5 + §6.5 + §6.7 — design source of truth for
  the new lifecycle.  Already in sync after the 2026-06-12 lock.
- **Code** — `src/{producer,consumer,processor}/*_role_host.cpp`,
  `src/utils/service/role_api_base.cpp`,
  `src/include/utils/role_api_base.hpp`,
  `src/utils/hub/{hub_zmq_queue,hub_shm_queue}.cpp`.

## Findings

Severity scale: **H** = onboarding-blocker (reader will write wrong
code); **M** = misleading but readers can recover from siblings;
**L** = polish.

### H1 — HEP-CORE-0011 §"Role Host `worker_main_()` Steps" (lines 998-1090)

Describes a sequence that no longer matches code on TWO unrelated axes
at once:

1. **Ctrl thread (Wave-B M4f closure, 4 weeks stale).** Step 6 cites
   `api_->start_ctrl_thread(CtrlThreadConfig)`.  Code uses
   `api_->start_handler_threads(handler)` since commit `0c…` /
   task #45.  The "BRC connects + sends REG_REQ from the ctrl thread"
   description is also stale: REG_REQ is now sent inline from the
   role-host body, not from the ctrl thread.
2. **Queue activation (AUTH-1 closure, 1 day stale).** Step 2b says
   `setup_infrastructure_` "builds queues" without mentioning that the
   queue is left in **Standby** state.  Step 6 doesn't mention
   `apply_consumer_reg_ack(ack)` / `apply_producer_reg_ack(ack)` — the
   only ways to drive Standby → Active.  The "registration failure is
   FATAL" contract from HEP-CORE-0036 §3.5.1 is also absent.

Side findings in the same section:
- Step numbering skips Step 3 (goes 0, 1, 2a, 2b, **(gap)**, 4, 5, 6).
  Code uses 1a + 1b.  Cosmetic but jarring on first read.
- The "Wave-B M9 ... When M9 lands ..." conditional in the ORDERING
  note (lines 1045-1049) refers to work that shipped 2026-05-26.

### H2 — HEP-CORE-0011 line 110-111 (mermaid annotation)

```
-- owns ctrl thread (start_ctrl_thread): heartbeat, broker notifications,
   deregistration sequencing -- see HEP-CORE-0023 §2.5
```

Wrong on the function name (M4f) and on the implication that the ctrl
thread is the only data-path-adjacent thread (handler model has N
per-presence ctrl threads).

### M3 — HEP-CORE-0017 §3.3 — PULL-only coverage

§3.3 correctly describes the new S1-Standby + S3-`apply_master_approval`
flow but only for the PULL (rx queue / consumer) side.  The new design
has an exactly symmetric PUSH path on the producer's tx queue, which
the section doesn't mention.

### M4 — HEP-CORE-0019 line 1122

Table row for `RoleAPIBase` says:

> periodic heartbeat tick installed via `start_ctrl_thread` (Phase 6: per-presence)

Wrong on two counts: the function is now `install_heartbeat` (called
inline after `apply_*_reg_ack`), and `start_ctrl_thread` was retired
4 weeks ago.

### M5 — HEP-CORE-0031 §4.2.4 + §4.3.1 + §4.3.6

- §4.2.4 example block shows `RoleAPIBase::start_ctrl_thread` spawning
  ctrl directly.  Wrong call site post-M4f.
- §4.3.1 table row for the `ctrl` slot says
  `role_api_base.cpp::start_ctrl_thread (legacy)`.  Should be
  `start_handler_threads` with one row per `handler_ctrl_<N>` thread
  (single-hub: `handler_ctrl_0`; dual-hub: `handler_ctrl_0` +
  `handler_ctrl_1`).
- §4.3.6 phase table calls M4c "(pending)".  M4c shipped.  All five
  M4 phases shipped — the table reads as a roadmap when it's now
  history.

### M6 — HEP-CORE-0018 §15.4 step 5 (line 1129)

```
5. Role host: `start_tx_queue()` (idempotent)
```

API deleted by commit `638443e4`.  HEP-0018 is marked SUPERSEDED at
the top with a note "library-level content below remains accurate" —
that claim is now false at this specific line.

### L7 — HEP-CORE-0011 library structure (lines 205-215)

Lists "pylabhub-producer (executable)" / "pylabhub-consumer (executable)"
/ "pylabhub-processor (executable)" as binary targets.  Per
HEP-CORE-0024 (Role Directory Service), these binaries were unified
into a single `plh_role --role <kind>` binary 2026-04-21.  The library
code paths are still correct; only the binary-target wording is stale.

**Disposition for L7.** Out of strict scope for this lifecycle sweep,
but the user asked to "thoroughly detect any obsolete parts" — so it's
in scope.  Will fix as a footnote within the same commit.

## Remediation plan

| # | File | Section / line | Change |
|---|---|---|---|
| H1 | `docs/HEP/HEP-CORE-0011-ScriptHost-Abstraction-Framework.md` | §"Role Host `worker_main_()` Steps" (~998-1090) | Full rewrite of the step list; add Mermaid sequence + queue-state diagram; add inline pseudocode of the canonical register → apply → install_heartbeat sequence; add the "registration failure is FATAL" contract; close step-numbering gap; retire M9 conditional |
| H2 | same file | line 110-111 (text-art annotation) | Update to handler-threads vocabulary |
| M3 | `docs/HEP/HEP-CORE-0017-Pipeline-Architecture.md` | §3.3 framework-integration bullets | Add a PUSH-side mirror paragraph |
| M4 | `docs/HEP/HEP-CORE-0019-Metrics-Plane.md` | line 1122 | Update to `install_heartbeat` post-`apply_*_reg_ack` |
| M5 | `docs/HEP/HEP-CORE-0031-ThreadManager.md` | §4.2.4 + §4.3.1 + §4.3.6 | Code example to handler-threads; table row to per-presence `handler_ctrl_<N>`; phase-table M4 column all → shipped |
| M6 | `docs/HEP/HEP-CORE-0018-Producer-Consumer-Binaries.md` | §15.4 | Add a supersession banner at section head; point readers to the live HEPs |
| L7 | `docs/HEP/HEP-CORE-0011-ScriptHost-Abstraction-Framework.md` | lines 205-215 (library structure) | Annotate executable names with current `plh_role` reality |

## Verification

- **No code changes** in this sweep — doc only.
- Build + L2/L3 sweep run anyway as a safety net (no behavior change
  expected; 1734/1734 from prior turn establishes the baseline).
- Cross-doc references checked: HEP-0019 / HEP-0031 / HEP-0017 cite
  HEP-0011's step numbering by phrase ("Step 6", "Step 5", etc.).
  After the HEP-0011 rewrite, those back-references still parse because
  the step labels for the phases they refer to (`invoke_on_init` at
  Step 5; broker connect + register at Step 6) are preserved by intent
  even though the numbering grid is tightened.

## Status table

| # | Item | Status |
|---|---|---|
| H1 | HEP-0011 §"Role Host `worker_main_()` Steps" rewrite | ✅ FIXED 2026-06-13 |
| H2 | HEP-0011 line 110-111 annotation | ✅ FIXED 2026-06-13 |
| M3 | HEP-0017 §3.3 PUSH mirror | ✅ FIXED 2026-06-13 |
| M4 | HEP-0019 line 1122 | ✅ FIXED 2026-06-13 |
| M5 | HEP-0031 §4.2.4 + §4.3.1 + §4.3.6 | ✅ FIXED 2026-06-13 |
| M6 | HEP-0018 §15.4 banner | ✅ FIXED 2026-06-13 |
| L7 | HEP-0011 lines 205-215 binary-name footnote | ✅ FIXED 2026-06-13 |
| L8 (incidental) | HEP-0033 §19.8 RoleHostFrame CRTP reference (caught during sweep) | ✅ FIXED 2026-06-13 |

All items closed.  Ready for archive at the next sweep per
`docs/DOC_STRUCTURE.md §2.2`.

## Verification

- Build green (cmake --build -j 2; doc-only changes; no targets rebuilt
  modulo timestamps).
- L2 + L3 sweep green: **1734 / 1734 tests passed**.
- Grep for stale strings outside the intended historical-citation
  scope (`start_rx_queue`, `start_tx_queue`, `start_ctrl_thread`):
  zero hits outside HEP-CORE-0018 §15.4 banner (intended) and
  HEP-CORE-0031 §4.3.6 M4f history row (intended).
