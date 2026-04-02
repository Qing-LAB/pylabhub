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

    /** Query counters. */
    uint64_t (*out_written)(const struct PlhNativeContext *ctx);
    uint64_t (*in_received)(const struct PlhNativeContext *ctx);
    uint64_t (*drops)(const struct PlhNativeContext *ctx);
    uint64_t (*script_errors)(const struct PlhNativeContext *ctx);
    uint64_t (*loop_overrun_count)(const struct PlhNativeContext *ctx);
    uint64_t (*last_cycle_work_us)(const struct PlhNativeContext *ctx);

    /* ── Opaque host data (do not dereference) ────────────────────── */
    void *_core;               /**< Internal — RoleHostCore pointer for API implementations. */
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
#define PLH_NATIVE_API_VERSION 1

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
    uint64_t out_written()       const { return c_->out_written ? c_->out_written(c_) : 0; }
    uint64_t in_received()       const { return c_->in_received ? c_->in_received(c_) : 0; }
    uint64_t drops()             const { return c_->drops ? c_->drops(c_) : 0; }
    uint64_t script_errors()     const { return c_->script_errors ? c_->script_errors(c_) : 0; }
    uint64_t loop_overrun_count() const { return c_->loop_overrun_count ? c_->loop_overrun_count(c_) : 0; }
    uint64_t last_cycle_work_us() const { return c_->last_cycle_work_us ? c_->last_cycle_work_us(c_) : 0; }

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
