---
name: Test discipline and debugging approach
description: Never dismiss test failures as "flaky" or "pre-existing" — always investigate root cause. Never repeat the same test hoping for different results. Read code first.
type: feedback
---

Never dismiss a test failure as "flaky" or "pre-existing" without reading the actual test code and tracing the failure path. The user considers this hand-waving unacceptable.

**Why:** Test failures expose real bugs. The NotifyChannel test failure was caused by our loop redesign (10μs vs 200ms timeout) — not a pre-existing issue. Dismissing it wasted 30+ minutes.

**How to apply:**
1. When a test fails, READ the test code and the failure output FIRST
2. Trace the execution path through the changed code
3. Never run the same test repeatedly hoping for a different result
4. If a test takes longer than expected, that IS the bug — investigate why, don't add timeouts
5. Test scripts must follow the callback contract (§6 of loop_design_unified.md)
6. Tests must be event-driven, not timer/iteration-count based
7. Every test must have a clear success/failure criterion documented in the test
8. A test that passes 3/3 in isolation but fails under -j2 has a real isolation bug — find it, don't dismiss it
