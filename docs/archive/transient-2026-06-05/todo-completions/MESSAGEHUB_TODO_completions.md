# MESSAGEHUB_TODO completions archive — 2026-06-05

This file preserves verbatim prose for MESSAGEHUB_TODO entries that were
verified shipped in code as of 2026-06-05 but still appeared in the active
TODO as "open" or "must-fix".  Moved here so the active MESSAGEHUB_TODO
reflects code reality.

Source file: `docs/todo/MESSAGEHUB_TODO.md`.

---

## A1 — `ctx_band_leave` semantic bug (CLOSED)

**Original entry (D1 must-fix section):**

> A1 — `ctx_band_leave` semantic bug.  `native_engine.cpp:280` returns
> `1` on broker rejection (e.g. `{status:error,NOT_A_MEMBER}`).  Fix:
> gate on `result->value("status","") == "success"`.  Single-line.

**Code reality (verified 2026-06-05):** the fix is in production code
at `src/utils/service/native_engine.cpp:289-305`.  The function gates
on `status == "success"` (lines 303-304); inline audit comment
preserves the rationale citing the 2026-05-20 discovery:

```cpp
int ctx_band_leave(const PlhNativeContext *ctx, const char *channel)
{
    if (!ctx || !ctx->_api || !channel) return 0;
    // Audit A1 (2026-05-20): pre-fix this returned `has_value() ? 1 : 0`,
    // which treats ANY broker reply as success — including the typed
    // `{status:error, NOT_A_MEMBER}` rejection that broker_proto 5
    // emits for a leave-while-not-a-member (S4 amendment).  Native
    // plugins would see a rejected leave as success.  Gate on
    // status == "success" so the int-bool return matches the
    // Python/Lua surfaces (which forward the full JSON; plugins on
    // those engines can see the error_code directly).  Note that
    // matching the JSON-string-returning ctx_band_join wouldn't help
    // here — the C ABI commits this function to `int` return.
    auto result = static_cast<RoleAPIBase *>(ctx->_api)->band_leave(channel);
    return (result.has_value() &&
            result->value("status", std::string{}) == "success") ? 1 : 0;
}
```

The entry remained in MESSAGEHUB_TODO's "D1 must-fix" list erroneously
after the code shipped.  Removed from active file in 2026-06-05 cleanup.
