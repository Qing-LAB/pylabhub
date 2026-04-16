/**
 * @file native_engine_api.h
 * @brief pylabHub native engine API — C-linkage interface for native script engines.
 *
 * Native engine authors include this header and implement the required symbols.
 * All exported symbols use C linkage for ABI stability across compilers.
 *
 * The native engine receives a PlhNativeContext from the framework at init time.
 * All framework services (logging, metrics, shutdown) are accessed through
 * function pointers on the context — no host symbol resolution needed.
 *
 * ## Minimal Producer Native engine (C)
 *
 * ```c
 * #include <pylabhub/native_engine_api.h>
 *
 * static const PlhNativeContext *g_ctx;
 *
 * PLH_EXPORT bool native_init(const PlhNativeContext *ctx) { g_ctx = ctx; return true; }
 * PLH_EXPORT void native_finalize(void) { g_ctx = NULL; }
 *
 * PLH_EXPORT bool on_produce(const plh_tx_t *tx) {
 *     float *slot = (float *)tx->slot;
 *     slot[0] = 42.0f;
 *     g_ctx->log(g_ctx, PLH_LOG_INFO, "produced a value");
 *     return true;
 * }
 * ```
 *
 * See HEP-CORE-0028 for the full specification.
 * See docs/README/README_NativePlugin.md for a developer guide.
 */

#ifndef PYLABHUB_NATIVE_ENGINE_API_H
#define PYLABHUB_NATIVE_ENGINE_API_H

#include <stddef.h>
#include <stdint.h>

/* =========================================================================
 * Platform export macro
 * ========================================================================= */

#if defined(_WIN32) || defined(_WIN64)
#   define PLH_EXPORT __declspec(dllexport)
#else
#   define PLH_EXPORT __attribute__((visibility("default")))
#endif

/* =========================================================================
 * Log level constants
 * ========================================================================= */

#define PLH_LOG_DEBUG  0
#define PLH_LOG_INFO   1
#define PLH_LOG_WARN   2
#define PLH_LOG_ERROR  3

/* Invoke direction structs (plh_rx_t, plh_tx_t, plh_inbox_msg_t). */
#include "native_invoke_types.h"

#ifdef __cplusplus
extern "C" {
#endif

/* =========================================================================
 * Forward declaration for self-referencing function pointers
 * ========================================================================= */
struct PlhNativeContext;

/* =========================================================================
 * Native engine context — passed to native_init()
 *
 * Contains identity strings (read-only) AND framework API function pointers.
 * The native engine calls framework services through these pointers — no host
 * symbol resolution (dlsym) needed, no -rdynamic required.
 *
 * All function pointers take `const struct PlhNativeContext *ctx` as first
 * argument for context routing. The caller just passes `ctx` through.
 * ========================================================================= */

/** Magic number for PlhNativeContext validation: 'P','L','H','C' = 0x504C4843 */
#define PLH_CONTEXT_MAGIC 0x504C4843u

/** Explicit 8-byte alignment for stable struct layout across compilers. */
typedef struct
#ifdef __cplusplus
    alignas(8)
#elif defined(__STDC_VERSION__) && __STDC_VERSION__ >= 201112L
    _Alignas(8)
#endif
    PlhNativeContext
{
    uint32_t _magic;           /**< Must be PLH_CONTEXT_MAGIC. Validates pointer. */

    /* ── Identity (read-only, valid until native_finalize) ──────────── */
    const char *role_tag;      /**< "prod", "cons", or "proc" */
    const char *uid;           /**< Role UID */
    const char *name;          /**< Role name */
    const char *channel;       /**< Primary channel (in_channel for processor) */
    const char *out_channel;   /**< Output channel (processor only; NULL otherwise) */
    const char *log_level;     /**< Configured log level string */
    const char *role_dir;      /**< Role directory path */

    /* ── Framework API (function pointers filled by host) ──────────── */

    /** Log a message. @param level PLH_LOG_DEBUG/INFO/WARN/ERROR */
    void (*log)(const struct PlhNativeContext *ctx, int level, const char *msg);

    /** Report a custom metric (key-value pair). */
    void (*report_metric)(const struct PlhNativeContext *ctx,
                          const char *key, double value);

    /** Clear all custom metrics. */
    void (*clear_custom_metrics)(const struct PlhNativeContext *ctx);

    /** Request graceful stop (signals main loop to exit after current iteration). */
    void (*request_stop)(const struct PlhNativeContext *ctx);

    /** Signal a critical error (sets critical flag + requests stop). */
    void (*set_critical_error)(const struct PlhNativeContext *ctx);

    /** Check if critical error has been flagged. Returns 1 or 0. */
    int (*is_critical_error)(const struct PlhNativeContext *ctx);

    /** Get stop reason string: "normal", "peer_dead", "hub_dead", "critical_error". */
    const char *(*stop_reason)(const struct PlhNativeContext *ctx);

    /** Query counters (consistent names across all engines). */
    uint64_t (*out_slots_written)(const struct PlhNativeContext *ctx);
    uint64_t (*in_slots_received)(const struct PlhNativeContext *ctx);
    uint64_t (*out_drop_count)(const struct PlhNativeContext *ctx);
    uint64_t (*script_error_count)(const struct PlhNativeContext *ctx);
    uint64_t (*loop_overrun_count)(const struct PlhNativeContext *ctx);
    uint64_t (*last_cycle_work_us)(const struct PlhNativeContext *ctx);

    /** Spinlock — side: PLH_SIDE_TX (0), PLH_SIDE_RX (1), or -1 for auto.
     *  lock returns 1 on success, 0 on timeout. timeout_ms: -1=infinite, 0=try, >0=wait. */
    int      (*spinlock_lock)(const struct PlhNativeContext *ctx, int index, int side, int timeout_ms);
    void     (*spinlock_unlock)(const struct PlhNativeContext *ctx, int index, int side);
    uint32_t (*spinlock_count)(const struct PlhNativeContext *ctx, int side);
    /** Check if spinlock is held by this process. Returns 1 or 0. */
    int      (*spinlock_is_locked)(const struct PlhNativeContext *ctx, int index, int side);

    /** Schema sizes (logical = C struct size). side: PLH_SIDE_TX/RX or -1 for auto. */
    size_t   (*slot_logical_size)(const struct PlhNativeContext *ctx, int side);
    size_t   (*flexzone_logical_size)(const struct PlhNativeContext *ctx, int side);

    /** Role discovery. Returns 1 if found, 0 on timeout. */
    int      (*wait_for_role)(const struct PlhNativeContext *ctx, const char *uid, int timeout_ms);

    /** Band pub/sub (HEP-CORE-0030). JSON strings for body/result. */
    /** band_join: returns JSON string with member list, or NULL on failure. Caller must free(). */
    char    *(*band_join)(const struct PlhNativeContext *ctx, const char *channel);
    /** band_leave: returns 1 on success, 0 on failure. */
    int      (*band_leave)(const struct PlhNativeContext *ctx, const char *channel);
    /** band_broadcast: body_json is a JSON string. Fire-and-forget. */
    void     (*band_broadcast)(const struct PlhNativeContext *ctx, const char *channel, const char *body_json);
    /** band_members: returns JSON string with member list, or NULL. Caller must free(). */
    char    *(*band_members)(const struct PlhNativeContext *ctx, const char *channel);

    /* ── Opaque host data (do not dereference) ────────────────────── */
    void *_core;               /**< Internal — RoleHostCore pointer for API implementations. */
    void *_api;                /**< Internal — RoleAPIBase pointer for spinlock/messaging. */
    const char *_log_label;    /**< Internal — log prefix e.g. "[native libfoo.so]" */

    uint32_t _magic_end;       /**< Must be PLH_CONTEXT_MAGIC. Trailing sentinel. */

} PlhNativeContext;

/* =========================================================================
 * ABI compatibility info — exported by native engine
 * ========================================================================= */

typedef struct PlhAbiInfo
{
    uint32_t struct_size;       /**< sizeof(PlhAbiInfo) — versioning guard */
    uint32_t sizeof_void_ptr;   /**< sizeof(void*) — 4 or 8 */
    uint32_t sizeof_size_t;     /**< sizeof(size_t) */
    uint32_t byte_order;        /**< 1 = little-endian, 2 = big-endian */
    uint32_t api_version;       /**< PLH_NATIVE_API_VERSION */
} PlhAbiInfo;

/** Current native engine API version. Increment on breaking changes. */
#define PLH_NATIVE_API_VERSION 2

/* =========================================================================
 * Channel side constants (for spinlock, schema size queries)
 * ========================================================================= */

#define PLH_SIDE_TX  0   /**< Output / producer / Tx side */
#define PLH_SIDE_RX  1   /**< Input / consumer / Rx side */
#define PLH_SIDE_AUTO (-1) /**< Auto-select (single-side roles only; errors for processor) */

/* =========================================================================
 * Required symbols (implement these)
 * ========================================================================= */

/* bool native_init(const PlhNativeContext *ctx);  -- store ctx, return true */
/* void native_finalize(void);                    -- release resources */
/* const PlhAbiInfo *native_abi_info(void);        -- optional ABI check */

/* =========================================================================
 * Role callback symbols (implement the ones your role needs)
 * ========================================================================= */

/* void on_init(const char *args_json); */
/* void on_stop(const char *args_json); */
/* bool on_produce(const plh_tx_t *tx); */
/* bool on_consume(const plh_rx_t *rx); */
/* bool on_process(const plh_rx_t *rx, const plh_tx_t *tx); */
/* bool on_inbox(const plh_inbox_msg_t *msg); */
/**
 * @brief Heartbeat / generic invoke callback convention.
 *
 * Functions called via invoke() receive args as a JSON string:
 *   void my_func(const char *args_json);
 *     - args_json is NULL when no arguments
 *     - args_json is a JSON object string when arguments are provided
 *     - the string is valid for the duration of the call (stack-owned by host)
 *     - callee must copy if needed beyond return
 */
/* void on_heartbeat(const char *args_json); */

/* =========================================================================
 * Optional metadata symbols
 * ========================================================================= */

/* const char *native_name(void); */
/* const char *native_version(void); */
/* bool native_is_thread_safe(void); */

/* =========================================================================
 * Schema validation macros
 * ========================================================================= */

/**
 * @brief Declare schema descriptor for a slot type.
 * Generates native_schema_<Name>() and native_sizeof_<Name>() symbols.
 * The framework verifies the native engine's schema matches the JSON config.
 *
 * Note: The macro body includes extern "C" for C++ so that the generated
 * symbols have C linkage regardless of where the macro is expanded.
 */
#ifdef __cplusplus
#define PLH_DECLARE_SCHEMA(Name, Schema, Size)                                          \
    extern "C" PLH_EXPORT const char *native_schema_##Name(void) { return (Schema); }   \
    extern "C" PLH_EXPORT size_t native_sizeof_##Name(void) { return (Size); }
#else
#define PLH_DECLARE_SCHEMA(Name, Schema, Size)                              \
    PLH_EXPORT const char *native_schema_##Name(void) { return (Schema); }  \
    PLH_EXPORT size_t native_sizeof_##Name(void) { return (Size); }
#endif

#ifdef __cplusplus
} /* extern "C" */
#endif

/* =========================================================================
 * C++ convenience layer (optional — for C++ native engine authors)
 * ========================================================================= */

#ifdef __cplusplus

#include <cstddef>
#include <cstdint>
#include <string>
#include <type_traits>

// Compile-time layout verification for cross-compiler interop.
static_assert(std::is_standard_layout_v<PlhNativeContext>,
              "PlhNativeContext must be standard-layout for C/C++ interop");
static_assert(alignof(PlhNativeContext) == 8,
              "PlhNativeContext must be 8-byte aligned");
static_assert(offsetof(PlhNativeContext, _magic) == 0,
              "PlhNativeContext: _magic must be at offset 0");

namespace plh
{

/** Log level enum for C++ usage. */
enum class LogLevel : int { Debug = 0, Info = 1, Warn = 2, Error = 3 };

/**
 * @brief C++ wrapper around PlhNativeContext.
 *
 * All methods are inline and call through the context's function pointers.
 * No host symbol resolution needed — everything crosses the C ABI via
 * the function pointers the host filled at init time.
 *
 * Usage:
 * ```cpp
 * static plh::Context *g_ctx;
 *
 * extern "C" bool native_init(const PlhNativeContext *raw) {
 *     static plh::Context ctx(raw);
 *     g_ctx = &ctx;
 *     ctx.log(plh::LogLevel::Info, "native engine initialized");
 *     return true;
 * }
 * ```
 */
class Context
{
  public:
    explicit Context(const PlhNativeContext *c) noexcept : c_(c)
    {
        // Validate both sentinels at construction time.
        if (c && (c->_magic != PLH_CONTEXT_MAGIC || c->_magic_end != PLH_CONTEXT_MAGIC))
            c_ = nullptr; // invalidate — all methods become safe no-ops
    }

    /// Check if the context is valid (both magic sentinels match).
    bool valid() const noexcept
    {
        return c_ != nullptr
            && c_->_magic     == PLH_CONTEXT_MAGIC
            && c_->_magic_end == PLH_CONTEXT_MAGIC;
    }

    // ── Identity ────────────────────────────────────────────────────
    const char *uid()         const noexcept { return c_->uid; }
    const char *name()        const noexcept { return c_->name; }
    const char *channel()     const noexcept { return c_->channel; }
    const char *out_channel() const noexcept { return c_->out_channel; }
    const char *log_level_str() const noexcept { return c_->log_level; }
    const char *role_dir()    const noexcept { return c_->role_dir; }
    const char *role_tag()    const noexcept { return c_->role_tag; }

    // ── Logging ─────────────────────────────────────────────────────
    void log(LogLevel level, const char *msg) const
    {
        if (c_->log) c_->log(c_, static_cast<int>(level), msg);
    }
    void log(LogLevel level, const std::string &msg) const
    {
        log(level, msg.c_str());
    }

    // ── Custom metrics ──────────────────────────────────────────────
    void report_metric(const char *key, double value) const
    {
        if (c_->report_metric) c_->report_metric(c_, key, value);
    }
    void clear_custom_metrics() const
    {
        if (c_->clear_custom_metrics) c_->clear_custom_metrics(c_);
    }

    // ── Lifecycle ───────────────────────────────────────────────────
    void request_stop() const
    {
        if (c_->request_stop) c_->request_stop(c_);
    }
    void set_critical_error() const
    {
        if (c_->set_critical_error) c_->set_critical_error(c_);
    }
    bool is_critical_error() const
    {
        return c_->is_critical_error ? c_->is_critical_error(c_) != 0 : false;
    }
    const char *stop_reason() const
    {
        return c_->stop_reason ? c_->stop_reason(c_) : "normal";
    }

    // ── Counters ────────────────────────────────────────────────────
    uint64_t out_slots_written() const { return c_->out_slots_written ? c_->out_slots_written(c_) : 0; }
    uint64_t in_slots_received() const { return c_->in_slots_received ? c_->in_slots_received(c_) : 0; }
    uint64_t out_drop_count()    const { return c_->out_drop_count ? c_->out_drop_count(c_) : 0; }
    uint64_t script_error_count() const { return c_->script_error_count ? c_->script_error_count(c_) : 0; }
    uint64_t loop_overrun_count() const { return c_->loop_overrun_count ? c_->loop_overrun_count(c_) : 0; }
    uint64_t last_cycle_work_us() const { return c_->last_cycle_work_us ? c_->last_cycle_work_us(c_) : 0; }

    // ── Spinlock ───────────────────────────────────────────────────
    bool spinlock_lock(int index, int side = PLH_SIDE_AUTO, int timeout_ms = -1) const
    {
        return c_->spinlock_lock ? c_->spinlock_lock(c_, index, side, timeout_ms) != 0 : false;
    }
    void spinlock_unlock(int index, int side = PLH_SIDE_AUTO) const
    {
        if (c_->spinlock_unlock) c_->spinlock_unlock(c_, index, side);
    }
    uint32_t spinlock_count(int side = PLH_SIDE_AUTO) const
    {
        return c_->spinlock_count ? c_->spinlock_count(c_, side) : 0;
    }
    bool spinlock_is_locked(int index, int side = PLH_SIDE_AUTO) const
    {
        return c_->spinlock_is_locked ? c_->spinlock_is_locked(c_, index, side) != 0 : false;
    }

    /// RAII spinlock guard — releases on scope exit.
    class SpinLockGuard
    {
      public:
        SpinLockGuard(const Context &api, int index, int side = PLH_SIDE_AUTO,
                      int timeout_ms = -1)
            : api_(&api), index_(index), side_(side)
            , locked_(api.spinlock_lock(index, side, timeout_ms))
        {}
        ~SpinLockGuard() { if (locked_) api_->spinlock_unlock(index_, side_); }

        SpinLockGuard(const SpinLockGuard &) = delete;
        SpinLockGuard &operator=(const SpinLockGuard &) = delete;

        bool owns_lock() const noexcept { return locked_; }
        explicit operator bool() const noexcept { return locked_; }
        int index() const noexcept { return index_; }
        int side() const noexcept { return side_; }

      private:
        const Context *api_;
        int index_;
        int side_;
        bool locked_;
    };

    /// Execute fn while holding spinlock acquired by index+side.
    /// Returns false if lock acquisition fails.
    /// Exception-safe: RAII guard releases the lock during stack unwinding
    /// if fn throws — the exception propagates to the caller after release.
    /// NOT noexcept — fn is free to throw.
    template <typename Fn>
    bool with_spinlock(int index, int side, int timeout_ms, Fn &&fn) const
    {
        SpinLockGuard guard(*this, index, side, timeout_ms);
        if (!guard) return false;
        fn();
        return true;
    }

    /// Execute fn while holding an additional recursive lock on an existing guard.
    /// Safe even if guard already holds the lock (recursive locking increments
    /// the recursion counter; the inner guard's destructor decrements it).
    /// Returns false if lock acquisition fails.
    template <typename Fn>
    bool with_spinlock(SpinLockGuard &existing, int timeout_ms, Fn &&fn) const
    {
        // Re-lock the same index+side — recursive, safe on same thread.
        SpinLockGuard inner(*this, existing.index(), existing.side(), timeout_ms);
        if (!inner) return false;
        fn();
        return true;
    }

    // ── Schema sizes ───────────────────────────────────────────────
    size_t slot_logical_size(int side = PLH_SIDE_AUTO) const
    {
        return c_->slot_logical_size ? c_->slot_logical_size(c_, side) : 0;
    }
    size_t flexzone_logical_size(int side = PLH_SIDE_AUTO) const
    {
        return c_->flexzone_logical_size ? c_->flexzone_logical_size(c_, side) : 0;
    }

    // Flexzone: C/C++ plugins access fz directly via plh_tx_t.fz / plh_rx_t.fz
    // in the invoke callback — zero-cost, no function pointer dispatch needed.
    // No separate flexzone() accessor on Context; the invoke struct IS the access
    // path for C/C++. Python/Lua use api.flexzone(side) because reconstructing
    // typed views per invoke is expensive in those languages.

    bool wait_for_role(const char *uid, int timeout_ms = 5000) const
    {
        return c_->wait_for_role ? c_->wait_for_role(c_, uid, timeout_ms) != 0 : false;
    }

    // ── Band pub/sub (HEP-CORE-0030) ─────────────────────────────
    /** Returns JSON string with member list. Caller must free(). NULL on failure. */
    char *band_join(const char *channel) const
    {
        return c_->band_join ? c_->band_join(c_, channel) : nullptr;
    }
    bool band_leave(const char *channel) const
    {
        return c_->band_leave ? c_->band_leave(c_, channel) != 0 : false;
    }
    void band_broadcast(const char *channel, const char *body_json) const
    {
        if (c_->band_broadcast) c_->band_broadcast(c_, channel, body_json);
    }
    /** Returns JSON string with member list. Caller must free(). NULL on failure. */
    char *band_members(const char *channel) const
    {
        return c_->band_members ? c_->band_members(c_, channel) : nullptr;
    }

    /// Access the raw C context.
    const PlhNativeContext *raw() const noexcept { return c_; }

  private:
    const PlhNativeContext *c_;
};

/**
 * @brief Typed slot reference — zero-cost wrapper around raw pointer.
 */
template <typename T>
class SlotRef
{
    static_assert(std::is_standard_layout_v<T>,
                  "SlotRef<T>: T must be a standard-layout type");
  public:
    SlotRef(void *ptr, size_t sz) noexcept
        : ptr_(static_cast<T *>(ptr)), valid_(ptr != nullptr && sz >= sizeof(T))
    {}

    explicit operator bool() const noexcept { return valid_; }
    T *operator->() noexcept { return ptr_; }
    const T *operator->() const noexcept { return ptr_; }
    T &operator*() noexcept { return *ptr_; }
    const T &operator*() const noexcept { return *ptr_; }
    T *get() noexcept { return ptr_; }

  private:
    T   *ptr_;
    bool valid_;
};

/// Read-only variant.
template <typename T>
class ConstSlotRef
{
    static_assert(std::is_standard_layout_v<T>,
                  "ConstSlotRef<T>: T must be a standard-layout type");
  public:
    ConstSlotRef(const void *ptr, size_t sz) noexcept
        : ptr_(static_cast<const T *>(ptr)), valid_(ptr != nullptr && sz >= sizeof(T))
    {}

    explicit operator bool() const noexcept { return valid_; }
    const T *operator->() const noexcept { return ptr_; }
    const T &operator*() const noexcept { return *ptr_; }
    const T *get() const noexcept { return ptr_; }

  private:
    const T *ptr_;
    bool     valid_;
};

} // namespace plh

/* ── Export macros for C++ typed callbacks ──────────────────────────────── */

#define PLH_EXPORT_PRODUCE(SlotType, FlexType, func)                        \
    extern "C" PLH_EXPORT bool on_produce(const plh_tx_t *tx)               \
    { return func(plh::SlotRef<SlotType>(tx->slot, tx->slot_size),          \
                  plh::SlotRef<FlexType>(tx->fz, tx->fz_size)); }

#define PLH_EXPORT_PRODUCE_NOFZ(SlotType, func)                             \
    extern "C" PLH_EXPORT bool on_produce(const plh_tx_t *tx)               \
    { return func(plh::SlotRef<SlotType>(tx->slot, tx->slot_size)); }

#define PLH_EXPORT_CONSUME(SlotType, FlexType, func)                        \
    extern "C" PLH_EXPORT bool on_consume(const plh_rx_t *rx)               \
    { return func(plh::ConstSlotRef<SlotType>(rx->slot, rx->slot_size),     \
                  plh::SlotRef<FlexType>(rx->fz, rx->fz_size)); }

#define PLH_EXPORT_CONSUME_NOFZ(SlotType, func)                             \
    extern "C" PLH_EXPORT bool on_consume(const plh_rx_t *rx)               \
    { return func(plh::ConstSlotRef<SlotType>(rx->slot, rx->slot_size)); }

#define PLH_EXPORT_PROCESS(InType, OutType, FlexType, func)                 \
    extern "C" PLH_EXPORT bool on_process(                                  \
        const plh_rx_t *rx, const plh_tx_t *tx)                             \
    { return func(plh::ConstSlotRef<InType>(rx->slot, rx->slot_size),       \
                  plh::SlotRef<OutType>(tx->slot, tx->slot_size),           \
                  plh::SlotRef<FlexType>(tx->fz, tx->fz_size)); }

#define PLH_EXPORT_PROCESS_NOFZ(InType, OutType, func)                      \
    extern "C" PLH_EXPORT bool on_process(                                  \
        const plh_rx_t *rx, const plh_tx_t *tx)                             \
    { return func(plh::ConstSlotRef<InType>(rx->slot, rx->slot_size),       \
                  plh::SlotRef<OutType>(tx->slot, tx->slot_size)); }

#endif /* __cplusplus */

#endif /* PYLABHUB_NATIVE_ENGINE_API_H */
