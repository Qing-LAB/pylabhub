# Test audit — file inventory (companion to REVIEW_TestAudit_2026-05-01.md)

This is the per-file ground truth.  The master plan in
`REVIEW_TestAudit_2026-05-01.md` defines the audit protocol; this
document tracks the 204 test files individually.  Update the row
for a file IN THE SAME COMMIT that fixes the file.

**Status legend**: 🟡 OPEN | 🟢 ACCEPTABLE (no fix needed) |
✅ FIXED (commit-hash) | ⚠ PARTIAL | 🔴 KNOWN BAD (named-issue) | n/a

**Columns A/B/C/D**: see master plan §0 for the four bug classes.

## Layer 0 — platform tests  (6 files)

| # | File | A | B | C | D | Verification / Notes |
|---|------|---|---|---|---|----------------------|
| L0-01 | `test_layer0_platform/test_abi_check.cpp` | 🟡 | 🟡 | 🟡 | 🟡 | not yet audited |
| L0-02 | `test_layer0_platform/test_platform_core.cpp` | 🟡 | 🟡 | 🟡 | 🟡 | not yet audited |
| L0-03 | `test_layer0_platform/test_platform_debug.cpp` | 🟡 | 🟡 | 🟡 | 🟡 | not yet audited |
| L0-04 | `test_layer0_platform/test_platform_sanitizers.cpp` | 🟡 | 🟡 | 🟡 | 🟡 | not yet audited |
| L0-05 | `test_layer0_platform/test_platform_shm.cpp` | 🟡 | 🟡 | 🟡 | 🟡 | not yet audited |
| L0-06 | `test_layer0_platform/test_uuid_and_format.cpp` | 🟡 | 🟡 | 🟡 | 🟡 | not yet audited |

## Layer 1 — base tests  (9 files)

| # | File | A | B | C | D | Verification / Notes |
|---|------|---|---|---|---|----------------------|
| L1-01 | `test_layer1_base/test_backoff_strategy.cpp` | 🟡 | 🟡 | 🟡 | 🟡 | not yet audited |
| L1-02 | `test_layer1_base/test_debug_info.cpp` | 🟡 | 🟡 | 🟡 | 🟡 | not yet audited |
| L1-03 | `test_layer1_base/test_formattable.cpp` | 🟡 | 🟡 | 🟡 | 🟡 | not yet audited |
| L1-04 | `test_layer1_base/test_module_def.cpp` | 🟡 | 🟡 | 🟡 | 🟡 | not yet audited |
| L1-05 | `test_layer1_base/test_portable_atomic_shared_ptr.cpp` | 🟡 | 🟡 | 🟡 | 🟡 | not yet audited |
| L1-06 | `test_layer1_base/test_recursionguard.cpp` | 🟡 | 🟡 | 🟡 | 🟡 | not yet audited |
| L1-07 | `test_layer1_base/test_result.cpp` | 🟡 | 🟡 | 🟡 | 🟡 | not yet audited |
| L1-08 | `test_layer1_base/test_scopeguard.cpp` | 🟡 | 🟡 | 🟡 | 🟡 | not yet audited |
| L1-09 | `test_layer1_base/test_spinlock.cpp` | 🟡 | 🟡 | 🟡 | 🟡 | not yet audited |

## Layer 2 — service tests  (81 files)

| # | File | A | B | C | D | Verification / Notes |
|---|------|---|---|---|---|----------------------|
| L2-01 | `test_layer2_service/test_admin_service.cpp` | ✅ `db9f8f9` | n/a | n/a | ✅ `db9f8f9` | ping mutation + log-capture mutation both produced expected red |
| L2-02 | `test_layer2_service/test_backoff_strategy.cpp` | 🟡 | 🟡 | 🟡 | 🟡 | not yet audited |
| L2-03 | `test_layer2_service/test_configure_logger.cpp` | 🟡 | 🟡 | 🟡 | 🟡 | not yet audited |
| L2-04 | `test_layer2_service/test_crypto_utils.cpp` | 🟡 | 🟡 | 🟡 | 🟡 | not yet audited |
| L2-05 | `test_layer2_service/test_engine_factory.cpp` | 🟡 | 🟡 | 🟡 | 🟡 | not yet audited |
| L2-06 | `test_layer2_service/test_filelock.cpp` | 🟡 | 🟡 | 🟡 | 🟡 | not yet audited |
| L2-07 | `test_layer2_service/test_filelock_singleprocess.cpp` | 🟡 | 🟡 | 🟡 | 🟡 | not yet audited |
| L2-08 | `test_layer2_service/test_framework_selftest.cpp` | 🟡 | 🟡 | 🟡 | 🟡 | not yet audited |
| L2-09 | `test_layer2_service/test_hub_cli.cpp` | 🟡 | 🟡 | 🟡 | 🟡 | not yet audited |
| L2-10 | `test_layer2_service/test_hub_config.cpp` | 🟡 | 🟡 | 🟡 | 🟡 | not yet audited |
| L2-11 | `test_layer2_service/test_hub_directory.cpp` | 🟡 | 🟡 | 🟡 | 🟡 | not yet audited |
| L2-12 | `test_layer2_service/test_hub_host.cpp` | ⚠ PARTIAL `6f54322` | 🟡 | 🟡 | 🟡 | FSM tests + 3 startup tests pinned; rest of file not yet audited |
| L2-13 | `test_layer2_service/test_hub_state.cpp` | 🟡 | 🟡 | 🟡 | 🟡 | not yet audited |
| L2-14 | `test_layer2_service/test_hub_vault.cpp` | 🟡 | 🟡 | 🟡 | 🟡 | not yet audited |
| L2-15 | `test_layer2_service/test_interactive_signal_handler.cpp` | 🟡 | 🟡 | 🟡 | 🟡 | not yet audited |
| L2-16 | `test_layer2_service/test_jsonconfig.cpp` | 🟡 | 🟡 | 🟡 | 🟡 | not yet audited |
| L2-17 | `test_layer2_service/test_lifecycle.cpp` | 🟡 | 🟡 | 🟡 | 🟡 | not yet audited |
| L2-18 | `test_layer2_service/test_lifecycle_dynamic.cpp` | 🟡 | 🟡 | 🟡 | 🟡 | not yet audited |
| L2-19 | `test_layer2_service/test_logger.cpp` | 🟡 | 🟡 | 🟡 | 🟡 | not yet audited |
| L2-20 | `test_layer2_service/test_loop_timing_policy.cpp` | 🟡 | 🟡 | 🟡 | 🟡 | not yet audited |
| L2-21 | `test_layer2_service/test_lua_engine.cpp` | 🟡 | 🟡 | 🟡 | 🟡 | not yet audited |
| L2-22 | `test_layer2_service/test_metrics_api.cpp` | 🟡 | 🟡 | 🟡 | 🟡 | not yet audited |
| L2-23 | `test_layer2_service/test_naming.cpp` | 🟡 | 🟡 | 🟡 | 🟡 | not yet audited |
| L2-24 | `test_layer2_service/test_net_address.cpp` | 🟡 | 🟡 | 🟡 | 🟡 | not yet audited |
| L2-25 | `test_layer2_service/test_plugins/good_producer_plugin.cpp` | 🟡 | 🟡 | 🟡 | 🟡 | not yet audited |
| L2-26 | `test_layer2_service/test_plugins/native_multifield_module.cpp` | 🟡 | 🟡 | 🟡 | 🟡 | not yet audited |
| L2-27 | `test_layer2_service/test_python_engine.cpp` | 🟡 | 🟡 | 🟡 | 🟡 | not yet audited |
| L2-28 | `test_layer2_service/test_role_cli.cpp` | 🟡 | 🟡 | 🟡 | 🟡 | not yet audited |
| L2-29 | `test_layer2_service/test_role_config.cpp` | 🟡 | 🟡 | 🟡 | 🟡 | not yet audited |
| L2-30 | `test_layer2_service/test_role_data_loop.cpp` | ⚠ NOTE | 🟢 OK | 🟢 OK | 🟡 deferred (subprocess) | Audited `<this commit>`.  Class A: `ThreadManagerTest.JoinInReverseOrder` name is misleading — only asserts `order.size() == 2`, NOT actual join order.  Assertion matches the real "drain waits for all threads" contract though, so this is a naming issue not a silent-failure bug; flagged here so a future rename or stronger test can be considered.  Other 7 tests pin specific counts (`EXPECT_GE(count, N)`, `EXPECT_EQ(count, N)`).  Class B: 8 `sleep_for` instances all in the worker file — acceptable per-pattern (see L2-67 row).  Class C: no timeout-bearing returns discarded. |
| L2-31 | `test_layer2_service/test_role_directory.cpp` | 🟡 | 🟡 | 🟡 | 🟡 | not yet audited |
| L2-32 | `test_layer2_service/test_role_host_base.cpp` | 🟢 OK | 🟢 OK | 🟢 OK | 🟡 deferred (subprocess Pattern 3) | Audited `<this commit>`.  Class A: 13 TEST_F all use `expect_panic_abort(..., panic_substring)` which path-pins death tests via message substring; no broad `EXPECT_THROW`.  Class B: worker uses `wait_for(pred, timeout_ms)` condition-based helper (line 204) — correct pattern.  `wait_for_wakeup` test pins an upper-bound timing (`EXPECT_LT(elapsed_ms, 500)`).  No bare `sleep_for` ordering antipattern.  Class C: no timeout-bearing-return discards.  Class D: Pattern 3 subprocess; LogCaptureFixture rollout for subprocess workers is a separate fixture-wide phase. |
| L2-33 | `test_layer2_service/test_role_host_core.cpp` | 🟡 | 🟡 | 🟡 | 🟡 | not yet audited |
| L2-34 | `test_layer2_service/test_role_init_directory.cpp` | 🟡 | 🟡 | 🟡 | 🟡 | not yet audited |
| L2-35 | `test_layer2_service/test_role_logging_roundtrip.cpp` | 🟡 | 🟡 | 🟡 | 🟡 | not yet audited |
| L2-36 | `test_layer2_service/test_role_registry.cpp` | 🟡 | 🟡 | 🟡 | 🟡 | not yet audited |
| L2-37 | `test_layer2_service/test_role_vault.cpp` | 🟡 | 🟡 | 🟡 | 🟡 | not yet audited |
| L2-38 | `test_layer2_service/test_schema_validation.cpp` | 🟡 | 🟡 | 🟡 | 🟡 | not yet audited |
| L2-39 | `test_layer2_service/test_scriptengine_native_dylib.cpp` | 🟡 | 🟡 | 🟡 | 🟡 | not yet audited |
| L2-40 | `test_layer2_service/test_shared_memory_spinlock.cpp` | 🟡 | 🟡 | 🟡 | 🟡 | not yet audited |
| L2-41 | `test_layer2_service/test_slot_rw_coordinator.cpp` | 🟡 | 🟡 | 🟡 | 🟡 | not yet audited |
| L2-42 | `test_layer2_service/test_slot_view_helpers.cpp` | 🟡 | 🟡 | 🟡 | 🟡 | not yet audited |
| L2-43 | `test_layer2_service/test_uid_utils.cpp` | 🟡 | 🟡 | 🟡 | 🟡 | not yet audited |
| L2-44 | `test_layer2_service/test_zmq_context.cpp` | 🟡 | 🟡 | 🟡 | 🟡 | not yet audited |
| L2-45 | `test_layer2_service/workers/crypto_workers.cpp` | 🟡 | 🟡 | 🟡 | 🟡 | not yet audited |
| L2-46 | `test_layer2_service/workers/crypto_workers.h` | n/a | n/a | n/a | n/a | header — audited as part of its companion .cpp |
| L2-47 | `test_layer2_service/workers/filelock_singleprocess_workers.cpp` | 🟡 | 🟡 | 🟡 | 🟡 | not yet audited |
| L2-48 | `test_layer2_service/workers/filelock_singleprocess_workers.h` | n/a | n/a | n/a | n/a | header — audited as part of its companion .cpp |
| L2-49 | `test_layer2_service/workers/filelock_workers.cpp` | 🟡 | 🟡 | 🟡 | 🟡 | not yet audited |
| L2-50 | `test_layer2_service/workers/filelock_workers.h` | n/a | n/a | n/a | n/a | header — audited as part of its companion .cpp |
| L2-51 | `test_layer2_service/workers/hub_config_workers.cpp` | 🟡 | 🟡 | 🟡 | 🟡 | not yet audited |
| L2-52 | `test_layer2_service/workers/hub_config_workers.h` | n/a | n/a | n/a | n/a | header — audited as part of its companion .cpp |
| L2-53 | `test_layer2_service/workers/jsonconfig_workers.cpp` | 🟡 | 🟡 | 🟡 | 🟡 | not yet audited |
| L2-54 | `test_layer2_service/workers/jsonconfig_workers.h` | n/a | n/a | n/a | n/a | header — audited as part of its companion .cpp |
| L2-55 | `test_layer2_service/workers/lifecycle_workers.cpp` | 🟡 | 🟡 | 🟡 | 🟡 | not yet audited |
| L2-56 | `test_layer2_service/workers/lifecycle_workers.h` | n/a | n/a | n/a | n/a | header — audited as part of its companion .cpp |
| L2-57 | `test_layer2_service/workers/logger_workers.cpp` | 🟡 | 🟡 | 🟡 | 🟡 | not yet audited |
| L2-58 | `test_layer2_service/workers/logger_workers.h` | n/a | n/a | n/a | n/a | header — audited as part of its companion .cpp |
| L2-59 | `test_layer2_service/workers/lua_engine_workers.cpp` | 🟡 | 🟡 | 🟡 | 🟡 | not yet audited |
| L2-60 | `test_layer2_service/workers/lua_engine_workers.h` | n/a | n/a | n/a | n/a | header — audited as part of its companion .cpp |
| L2-61 | `test_layer2_service/workers/metrics_api_workers.cpp` | 🟡 | 🟡 | 🟡 | 🟡 | not yet audited |
| L2-62 | `test_layer2_service/workers/metrics_api_workers.h` | n/a | n/a | n/a | n/a | header — audited as part of its companion .cpp |
| L2-63 | `test_layer2_service/workers/python_engine_workers.cpp` | 🟡 | 🟡 | 🟡 | 🟡 | not yet audited |
| L2-64 | `test_layer2_service/workers/python_engine_workers.h` | n/a | n/a | n/a | n/a | header — audited as part of its companion .cpp |
| L2-65 | `test_layer2_service/workers/role_config_workers.cpp` | 🟡 | 🟡 | 🟡 | 🟡 | not yet audited |
| L2-66 | `test_layer2_service/workers/role_config_workers.h` | n/a | n/a | n/a | n/a | header — audited as part of its companion .cpp |
| L2-67 | `test_layer2_service/workers/role_data_loop_workers.cpp` | 🟢 OK | 🟢 OK | 🟢 OK | 🟡 deferred (subprocess) | Audited `<this commit>`.  Class B: 8 `sleep_for` instances categorized — line 103 (SlowOps cycle 2 sleep) is the test's deliberate overrun-trigger; lines 165/220/249/313/341/375 are time-budgets for "let the loop / spawned threads run for N ms" before stopping/draining; line 366 is part of the test's deliberate execution-order setup.  None are sleep-then-assert-state-change ordering antipatterns (assertions check post-stop counts, not state transitions during the sleep).  Acceptable per the existing rule's exception for time-budget patterns. |
| L2-68 | `test_layer2_service/workers/role_data_loop_workers.h` | n/a | n/a | n/a | n/a | header — audited as part of its companion .cpp |
| L2-69 | `test_layer2_service/workers/role_host_base_workers.cpp` | 🟢 OK | 🟢 OK | 🟢 OK | 🟡 deferred (subprocess) | Audited `<this commit>` with L2-32.  Workers use `wait_for(pred, timeout_ms)` condition-based helper (line 204) for state-change waits; `wait_for_incoming(50)` and `wait_for_wakeup(20)` are testing those APIs with bounded-deadline contracts. |
| L2-70 | `test_layer2_service/workers/role_host_base_workers.h` | n/a | n/a | n/a | n/a | header — audited as part of its companion .cpp |
| L2-71 | `test_layer2_service/workers/role_logging_workers.cpp` | 🟡 | 🟡 | 🟡 | 🟡 | not yet audited |
| L2-72 | `test_layer2_service/workers/role_logging_workers.h` | n/a | n/a | n/a | n/a | header — audited as part of its companion .cpp |
| L2-73 | `test_layer2_service/workers/scriptengine_native_dylib_workers.cpp` | 🟡 | 🟡 | 🟡 | 🟡 | not yet audited |
| L2-74 | `test_layer2_service/workers/scriptengine_native_dylib_workers.h` | n/a | n/a | n/a | n/a | header — audited as part of its companion .cpp |
| L2-75 | `test_layer2_service/workers/selftest_workers.cpp` | 🟡 | 🟡 | 🟡 | 🟡 | not yet audited |
| L2-76 | `test_layer2_service/workers/signal_handler_workers.cpp` | 🟡 | 🟡 | 🟡 | 🟡 | not yet audited |
| L2-77 | `test_layer2_service/workers/signal_handler_workers.h` | n/a | n/a | n/a | n/a | header — audited as part of its companion .cpp |
| L2-78 | `test_layer2_service/workers/spinlock_workers.cpp` | 🟡 | 🟡 | 🟡 | 🟡 | not yet audited |
| L2-79 | `test_layer2_service/workers/spinlock_workers.h` | n/a | n/a | n/a | n/a | header — audited as part of its companion .cpp |
| L2-80 | `test_layer2_service/workers/zmq_context_workers.cpp` | 🟡 | 🟡 | 🟡 | 🟡 | not yet audited |
| L2-81 | `test_layer2_service/workers/zmq_context_workers.h` | n/a | n/a | n/a | n/a | header — audited as part of its companion .cpp |

## Layer 3 — datahub tests  (102 files)

| # | File | A | B | C | D | Verification / Notes |
|---|------|---|---|---|---|----------------------|
| L3-01 | `test_layer3_datahub/test_datahub_broker_admin.cpp` | 🟡 | 🟡 | 🟡 | 🟡 | not yet audited |
| L3-02 | `test_layer3_datahub/test_datahub_broker_consumer.cpp` | 🟡 | 🟡 | 🟡 | 🟡 | not yet audited |
| L3-03 | `test_layer3_datahub/test_datahub_broker.cpp` | 🟡 | 🟡 | 🟡 | 🟡 | not yet audited |
| L3-04 | `test_layer3_datahub/test_datahub_broker_health.cpp` | 🟡 | 🟡 | 🟡 | 🟡 | not yet audited |
| L3-05 | `test_layer3_datahub/test_datahub_broker_protocol.cpp` | 🟡 | 🟡 | 🟡 | 🟡 | not yet audited |
| L3-06 | `test_layer3_datahub/test_datahub_broker_request_comm.cpp` | 🟡 | 🟡 | 🟡 | 🟡 | not yet audited |
| L3-07 | `test_layer3_datahub/test_datahub_broker_schema.cpp` | 🟡 | 🟡 | 🟡 | 🟡 | not yet audited |
| L3-08 | `test_layer3_datahub/test_datahub_broker_shutdown.cpp` | 🟡 | 🟡 | 🟡 | 🟡 | not yet audited |
| L3-09 | `test_layer3_datahub/test_datahub_c_api_checksum.cpp` | 🟡 | 🟡 | 🟡 | 🟡 | not yet audited |
| L3-10 | `test_layer3_datahub/test_datahub_c_api_recovery.cpp` | 🟡 | 🟡 | 🟡 | 🟡 | not yet audited |
| L3-11 | `test_layer3_datahub/test_datahub_c_api_slot_protocol.cpp` | 🟡 | 🟡 | 🟡 | 🟡 | not yet audited |
| L3-12 | `test_layer3_datahub/test_datahub_c_api_validation.cpp` | 🟡 | 🟡 | 🟡 | 🟡 | not yet audited |
| L3-13 | `test_layer3_datahub/test_datahub_channel_access_policy.cpp` | 🟡 | 🟡 | 🟡 | 🟡 | not yet audited |
| L3-14 | `test_layer3_datahub/test_datahub_channel_group.cpp` | 🟡 | 🟡 | 🟡 | 🟡 | not yet audited |
| L3-15 | `test_layer3_datahub/test_datahub_config_validation.cpp` | 🟡 | 🟡 | 🟡 | 🟡 | not yet audited |
| L3-16 | `test_layer3_datahub/test_datahub_e2e.cpp` | 🟡 | 🟡 | 🟡 | 🟡 | not yet audited |
| L3-17 | `test_layer3_datahub/test_datahub_exception_safety.cpp` | 🟡 | 🟡 | 🟡 | 🟡 | not yet audited |
| L3-18 | `test_layer3_datahub/test_datahub_handle_semantics.cpp` | 🟡 | 🟡 | 🟡 | 🟡 | not yet audited |
| L3-19 | `test_layer3_datahub/test_datahub_header_structure.cpp` | 🟡 | 🟡 | 🟡 | 🟡 | not yet audited |
| L3-20 | `test_layer3_datahub/test_datahub_hub_federation.cpp` | 🟡 | 🟡 | 🟡 | 🟡 | not yet audited |
| L3-21 | `test_layer3_datahub/test_datahub_hub_host_integration.cpp` | 🟡 | 🟡 | 🟡 | 🟡 | not yet audited |
| L3-22 | `test_layer3_datahub/test_datahub_hub_inbox_queue.cpp` | 🟡 | 🟢 | ✅ FIXED `<this commit>` | 🟡 deferred | Class C: all 6 previously-discarded `c->send(1500ms)` returns now captured + asserted.  Happy-path 3 sites (lines 229, 462, 531) assert ack=0 — mutation-verified individually: omitting `q->send_ack(0)` in each recv lambda → matching test fails at "send timed out or got non-zero ack=255" assertion.  Reject-path 3 sites (lines 390, 424, 498) assert ack=255 + add `if (item) q->send_ack(0)` to recv lambda so the assert IS sensitive to "server failed to drop"; production-code mutation (schema-tag drop disabled in `hub_inbox_queue.cpp:350`) showed the existing `EXPECT_GT(error_count, 0)` catches the regression but the new ack assert was not independently triggered (other drop paths — frame size check — still active).  Reverted production mutation, 16/16 green at 3.15 s.  The new ack asserts add path-discrimination value but are not independently mutation-sensitive on reject paths due to multiple drop paths converging.  Class B: 10 sleeps reviewed — all are pre-traffic establishment timing per existing rule.  Class A: no broad EXPECT_THROW.  Class D LogCaptureFixture rollout deferred to fixture-wide phase. |
| L3-23 | `test_layer3_datahub/test_datahub_hub_monitored_queue.cpp` | 🟡 | 🟡 | 🟡 | 🟡 | not yet audited |
| L3-24 | `test_layer3_datahub/test_datahub_hub_queue.cpp` | 🟡 | 🟡 | 🟡 | 🟡 | not yet audited |
| L3-25 | `test_layer3_datahub/test_datahub_hub_zmq_queue.cpp` | 🟢 | 🟢 ACCEPTABLE | 🟢 | 🟡 deferred | Class B: 33 sleep_for confirmed all post-`q->start()` / `push->start()` / `pull->start()` ZMQ-TCP-establishment per the documented-acceptable rule (`feedback_test_correctness_principle.md`).  No Class B ordering antipattern found.  Class A: 4 `EXPECT_NO_THROW` calls all test "operation does not throw" as the actual contract (idempotent stop, null-acquire commit/discard) — valid usage.  Class C: file does not use timeout-bearing operations.  Class D LogCaptureFixture rollout deferred to fixture-wide phase.  Audit `<this commit>`. |
| L3-26 | `test_layer3_datahub/test_datahub_integrity_repair.cpp` | 🟡 | 🟡 | 🟡 | 🟡 | not yet audited |
| L3-27 | `test_layer3_datahub/test_datahub_metrics.cpp` | 🟡 | 🟡 | 🟡 | 🟡 | not yet audited |
| L3-28 | `test_layer3_datahub/test_datahub_mutex.cpp` | 🟡 | 🟡 | 🟡 | 🟡 | not yet audited |
| L3-29 | `test_layer3_datahub/test_datahub_policy_enforcement.cpp` | 🟡 | 🟡 | 🟡 | 🟡 | not yet audited |
| L3-30 | `test_layer3_datahub/test_datahub_producer_consumer.cpp` | 🟡 | 🟡 | 🟡 | 🟡 | not yet audited |
| L3-31 | `test_layer3_datahub/test_datahub_recovery_scenarios.cpp` | 🟡 | 🟡 | 🟡 | 🟡 | not yet audited |
| L3-32 | `test_layer3_datahub/test_datahub_role_state_machine.cpp` | 🟡 | 🟡 | 🟡 | 🟡 | not yet audited |
| L3-33 | `test_layer3_datahub/test_datahub_schema_blds.cpp` | 🟡 | 🟡 | 🟡 | 🟡 | not yet audited |
| L3-34 | `test_layer3_datahub/test_datahub_schema_loader.cpp` | 🟡 | 🟡 | 🟡 | 🟡 | not yet audited |
| L3-35 | `test_layer3_datahub/test_datahub_schema_validation.cpp` | 🟡 | 🟡 | 🟡 | 🟡 | not yet audited |
| L3-36 | `test_layer3_datahub/test_datahub_stress_raii.cpp` | 🟡 | 🟡 | 🟡 | 🟡 | not yet audited |
| L3-37 | `test_layer3_datahub/test_datahub_transaction_api.cpp` | 🟡 | 🟡 | 🟡 | 🟡 | not yet audited |
| L3-38 | `test_layer3_datahub/test_datahub_write_attach.cpp` | 🟡 | 🟡 | 🟡 | 🟡 | not yet audited |
| L3-39 | `test_layer3_datahub/test_datahub_zmq_endpoint_registry.cpp` | 🟡 | 🟡 | 🟡 | 🟡 | not yet audited |
| L3-40 | `test_layer3_datahub/test_datahub_zmq_poll_loop.cpp` | 🟢 | ✅ FIXED `<this commit>` | n/a | 🟡 deferred | Class B: 4 ordering sites converted to `poll_until` with named predicates (DispatchesOnPollin, SignalSocketWakesLoop, MultipleSocketsDispatchCorrectly, PeriodicTasksFireDuringLoop).  Mutation: each converted site's callback side-effect commented out → exactly that test failed at the `poll_until` ASSERT_TRUE with the path-discriminating predicate message; restored → 7/7 pass at 0.72 s.  Lines 81/154 are PeriodicTask interval-timing tests (acceptable per existing rule); lines 208/280 are pre-shutdown loop-letting-run sleeps (acceptable, no state assertion follows).  Class A: no broad EXPECT_THROW.  Class C: no timeout-bearing returns discarded (PeriodicTask & loop test all-local).  Class D LogCaptureFixture rollout deferred to fixture-wide phase. |
| L3-41 | `test_layer3_datahub/test_role_api_flexzone.cpp` | 🟡 | 🟡 | 🟡 | 🟡 | not yet audited |
| L3-42 | `test_layer3_datahub/test_role_api_loop_policy.cpp` | 🟡 | 🟡 | 🟡 | 🟡 | not yet audited |
| L3-43 | `test_layer3_datahub/test_role_api_raii.cpp` | 🟡 | 🟡 | 🟡 | 🟡 | not yet audited |
| L3-44 | `test_layer3_datahub/workers/datahub_broker_consumer_workers.cpp` | 🟡 | 🟡 | 🟡 | 🟡 | not yet audited |
| L3-45 | `test_layer3_datahub/workers/datahub_broker_consumer_workers.h` | n/a | n/a | n/a | n/a | header — audited as part of its companion .cpp |
| L3-46 | `test_layer3_datahub/workers/datahub_broker_health_workers.cpp` | 🟡 | 🟡 | 🟡 | 🟡 | not yet audited |
| L3-47 | `test_layer3_datahub/workers/datahub_broker_health_workers.h` | n/a | n/a | n/a | n/a | header — audited as part of its companion .cpp |
| L3-48 | `test_layer3_datahub/workers/datahub_broker_request_comm_workers.cpp` | 🟡 | 🟡 | 🟡 | 🟡 | not yet audited |
| L3-49 | `test_layer3_datahub/workers/datahub_broker_workers.cpp` | 🟡 | 🟡 | 🟡 | 🟡 | not yet audited |
| L3-50 | `test_layer3_datahub/workers/datahub_broker_workers.h` | n/a | n/a | n/a | n/a | header — audited as part of its companion .cpp |
| L3-51 | `test_layer3_datahub/workers/datahub_c_api_checksum_workers.cpp` | 🟡 | 🟡 | 🟡 | 🟡 | not yet audited |
| L3-52 | `test_layer3_datahub/workers/datahub_c_api_checksum_workers.h` | n/a | n/a | n/a | n/a | header — audited as part of its companion .cpp |
| L3-53 | `test_layer3_datahub/workers/datahub_c_api_draining_workers.cpp` | 🟡 | 🟡 | 🟡 | 🟡 | not yet audited |
| L3-54 | `test_layer3_datahub/workers/datahub_c_api_draining_workers.h` | n/a | n/a | n/a | n/a | header — audited as part of its companion .cpp |
| L3-55 | `test_layer3_datahub/workers/datahub_c_api_recovery_workers.cpp` | 🟡 | 🟡 | 🟡 | 🟡 | not yet audited |
| L3-56 | `test_layer3_datahub/workers/datahub_c_api_recovery_workers.h` | n/a | n/a | n/a | n/a | header — audited as part of its companion .cpp |
| L3-57 | `test_layer3_datahub/workers/datahub_c_api_slot_protocol_workers.cpp` | 🟡 | 🟡 | 🟡 | 🟡 | not yet audited |
| L3-58 | `test_layer3_datahub/workers/datahub_c_api_slot_protocol_workers.h` | n/a | n/a | n/a | n/a | header — audited as part of its companion .cpp |
| L3-59 | `test_layer3_datahub/workers/datahub_c_api_validation_workers.cpp` | 🟡 | 🟡 | 🟡 | 🟡 | not yet audited |
| L3-60 | `test_layer3_datahub/workers/datahub_c_api_validation_workers.h` | n/a | n/a | n/a | n/a | header — audited as part of its companion .cpp |
| L3-61 | `test_layer3_datahub/workers/datahub_channel_group_workers.cpp` | 🟡 | 🟡 | 🟡 | 🟡 | not yet audited |
| L3-62 | `test_layer3_datahub/workers/datahub_config_validation_workers.cpp` | 🟡 | 🟡 | 🟡 | 🟡 | not yet audited |
| L3-63 | `test_layer3_datahub/workers/datahub_config_validation_workers.h` | n/a | n/a | n/a | n/a | header — audited as part of its companion .cpp |
| L3-64 | `test_layer3_datahub/workers/datahub_e2e_workers.cpp` | 🟡 | 🟡 | 🟡 | 🟡 | not yet audited |
| L3-65 | `test_layer3_datahub/workers/datahub_e2e_workers.h` | n/a | n/a | n/a | n/a | header — audited as part of its companion .cpp |
| L3-66 | `test_layer3_datahub/workers/datahub_exception_safety_workers.cpp` | 🟡 | 🟡 | 🟡 | 🟡 | not yet audited |
| L3-67 | `test_layer3_datahub/workers/datahub_exception_safety_workers.h` | n/a | n/a | n/a | n/a | header — audited as part of its companion .cpp |
| L3-68 | `test_layer3_datahub/workers/datahub_handle_semantics_workers.cpp` | 🟡 | 🟡 | 🟡 | 🟡 | not yet audited |
| L3-69 | `test_layer3_datahub/workers/datahub_handle_semantics_workers.h` | n/a | n/a | n/a | n/a | header — audited as part of its companion .cpp |
| L3-70 | `test_layer3_datahub/workers/datahub_header_structure_workers.cpp` | 🟡 | 🟡 | 🟡 | 🟡 | not yet audited |
| L3-71 | `test_layer3_datahub/workers/datahub_header_structure_workers.h` | n/a | n/a | n/a | n/a | header — audited as part of its companion .cpp |
| L3-72 | `test_layer3_datahub/workers/datahub_hub_queue_workers.cpp` | 🟡 | 🟡 | 🟡 | 🟡 | not yet audited |
| L3-73 | `test_layer3_datahub/workers/datahub_hub_queue_workers.h` | n/a | n/a | n/a | n/a | header — audited as part of its companion .cpp |
| L3-74 | `test_layer3_datahub/workers/datahub_integrity_repair_workers.cpp` | 🟡 | 🟡 | 🟡 | 🟡 | not yet audited |
| L3-75 | `test_layer3_datahub/workers/datahub_integrity_repair_workers.h` | n/a | n/a | n/a | n/a | header — audited as part of its companion .cpp |
| L3-76 | `test_layer3_datahub/workers/datahub_mutex_workers.cpp` | 🟡 | 🟡 | 🟡 | 🟡 | not yet audited |
| L3-77 | `test_layer3_datahub/workers/datahub_mutex_workers.h` | n/a | n/a | n/a | n/a | header — audited as part of its companion .cpp |
| L3-78 | `test_layer3_datahub/workers/datahub_policy_enforcement_workers.cpp` | 🟡 | 🟡 | 🟡 | 🟡 | not yet audited |
| L3-79 | `test_layer3_datahub/workers/datahub_policy_enforcement_workers.h` | n/a | n/a | n/a | n/a | header — audited as part of its companion .cpp |
| L3-80 | `test_layer3_datahub/workers/datahub_producer_consumer_workers.cpp` | 🟡 | 🟡 | 🟡 | 🟡 | not yet audited |
| L3-81 | `test_layer3_datahub/workers/datahub_producer_consumer_workers.h` | n/a | n/a | n/a | n/a | header — audited as part of its companion .cpp |
| L3-82 | `test_layer3_datahub/workers/datahub_recovery_scenario_workers.cpp` | 🟡 | 🟡 | 🟡 | 🟡 | not yet audited |
| L3-83 | `test_layer3_datahub/workers/datahub_recovery_scenario_workers.h` | n/a | n/a | n/a | n/a | header — audited as part of its companion .cpp |
| L3-84 | `test_layer3_datahub/workers/datahub_role_state_workers.cpp` | 🟡 | 🟡 | 🟡 | 🟡 | not yet audited |
| L3-85 | `test_layer3_datahub/workers/datahub_schema_blds_workers.cpp` | 🟡 | 🟡 | 🟡 | 🟡 | not yet audited |
| L3-86 | `test_layer3_datahub/workers/datahub_schema_blds_workers.h` | n/a | n/a | n/a | n/a | header — audited as part of its companion .cpp |
| L3-87 | `test_layer3_datahub/workers/datahub_schema_validation_workers.cpp` | 🟡 | 🟡 | 🟡 | 🟡 | not yet audited |
| L3-88 | `test_layer3_datahub/workers/datahub_schema_validation_workers.h` | n/a | n/a | n/a | n/a | header — audited as part of its companion .cpp |
| L3-89 | `test_layer3_datahub/workers/datahub_slot_allocation_workers.cpp` | 🟡 | 🟡 | 🟡 | 🟡 | not yet audited |
| L3-90 | `test_layer3_datahub/workers/datahub_slot_allocation_workers.h` | n/a | n/a | n/a | n/a | header — audited as part of its companion .cpp |
| L3-91 | `test_layer3_datahub/workers/datahub_stress_raii_workers.cpp` | 🟡 | 🟡 | 🟡 | 🟡 | not yet audited |
| L3-92 | `test_layer3_datahub/workers/datahub_stress_raii_workers.h` | n/a | n/a | n/a | n/a | header — audited as part of its companion .cpp |
| L3-93 | `test_layer3_datahub/workers/datahub_transaction_api_workers.cpp` | 🟡 | 🟡 | 🟡 | 🟡 | not yet audited |
| L3-94 | `test_layer3_datahub/workers/datahub_transaction_api_workers.h` | n/a | n/a | n/a | n/a | header — audited as part of its companion .cpp |
| L3-95 | `test_layer3_datahub/workers/datahub_write_attach_workers.cpp` | 🟡 | 🟡 | 🟡 | 🟡 | not yet audited |
| L3-96 | `test_layer3_datahub/workers/datahub_write_attach_workers.h` | n/a | n/a | n/a | n/a | header — audited as part of its companion .cpp |
| L3-97 | `test_layer3_datahub/workers/role_api_flexzone_workers.cpp` | 🟡 | 🟡 | 🟡 | 🟡 | not yet audited |
| L3-98 | `test_layer3_datahub/workers/role_api_flexzone_workers.h` | n/a | n/a | n/a | n/a | header — audited as part of its companion .cpp |
| L3-99 | `test_layer3_datahub/workers/role_api_loop_policy_workers.cpp` | 🟡 | 🟡 | 🟡 | 🟡 | not yet audited |
| L3-100 | `test_layer3_datahub/workers/role_api_loop_policy_workers.h` | n/a | n/a | n/a | n/a | header — audited as part of its companion .cpp |
| L3-101 | `test_layer3_datahub/workers/role_api_raii_workers.cpp` | 🟡 | 🟡 | 🟡 | 🟡 | not yet audited |
| L3-102 | `test_layer3_datahub/workers/role_api_raii_workers.h` | n/a | n/a | n/a | n/a | header — audited as part of its companion .cpp |

## Layer 4 — integration tests  (1 files)

| # | File | A | B | C | D | Verification / Notes |
|---|------|---|---|---|---|----------------------|
| L4i-01 | `test_layer4_integration/test_admin_shell.cpp` | 🟡 | 🟡 | 🟡 | 🟡 | not yet audited |

## Layer 4 — plh_role tests  (5 files)

| # | File | A | B | C | D | Verification / Notes |
|---|------|---|---|---|---|----------------------|
| L4r-01 | `test_layer4_plh_role/plh_role_fixture.h` | n/a | n/a | n/a | n/a | header — audited as part of its companion .cpp |
| L4r-02 | `test_layer4_plh_role/test_plh_role_errors.cpp` | 🟡 | 🟡 | 🟡 | 🟡 | not yet audited |
| L4r-03 | `test_layer4_plh_role/test_plh_role_init.cpp` | 🟡 | 🟡 | 🟡 | 🟡 | not yet audited |
| L4r-04 | `test_layer4_plh_role/test_plh_role_keygen.cpp` | 🟡 | 🟡 | 🟡 | 🟡 | not yet audited |
| L4r-05 | `test_layer4_plh_role/test_plh_role_validate.cpp` | 🟡 | 🟡 | 🟡 | 🟡 | not yet audited |

