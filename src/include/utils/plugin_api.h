/**
 * @file plugin_api.h
 * @brief pylabHub Native Plugin API — C-linkage interface for native script engines.
 *
 * Plugin authors include this header and implement the required symbols.
 * All exported symbols use C linkage for ABI stability across compilers.
 *
 * ## Minimal Producer Plugin
 *
 * ```c
 * #include <pylabhub/plugin_api.h>
 *
 * PLH_EXPORT bool plugin_init(const PlhPluginContext *ctx) { return true; }
 * PLH_EXPORT void plugin_finalize(void) {}
 * PLH_EXPORT void on_init(void) {}
 * PLH_EXPORT void on_stop(void) {}
 *
 * PLH_EXPORT bool on_produce(void *out_slot, size_t out_sz,
 *                            void *flexzone, size_t fz_sz) {
 *     float *slot = (float *)out_slot;
 *     slot[0] = 42.0f;
 *     return true;   // true = commit, false = discard
 * }
 * ```
 *
 * ## Schema Validation
 *
 * Use PLH_DECLARE_SCHEMA to declare a slot type with compile-time schema
 * descriptors. The framework verifies the plugin's schema matches the
 * JSON config at load time.
 *
 * See HEP-CORE-0028 for the full specification.
 * See docs/README/README_NativePlugin.md for a developer guide.
 */

#ifndef PYLABHUB_PLUGIN_API_H
#define PYLABHUB_PLUGIN_API_H

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

#ifdef __cplusplus
extern "C" {
#endif

/* =========================================================================
 * Plugin context — passed to plugin_init()
 * ========================================================================= */

/**
 * @brief Read-only context provided by the framework at plugin_init().
 *
 * Pointers are valid for the lifetime of the plugin (until plugin_finalize).
 * All strings are null-terminated UTF-8.
 */
typedef struct PlhPluginContext
{
    const char *role_tag;      /**< "prod", "cons", or "proc" */
    const char *uid;           /**< Role UID (e.g. "PROD-MySensor-AABBCCDD") */
    const char *name;          /**< Role name (e.g. "MySensor") */
    const char *channel;       /**< Primary channel (in_channel for processor) */
    const char *out_channel;   /**< Output channel (processor only; NULL otherwise) */
    const char *log_level;     /**< Configured log level string */
    const char *role_dir;      /**< Role directory path */

    /** Opaque pointer to RoleHostCore. Pass to plh_log(), plh_report_metric(), etc. */
    void *core;
} PlhPluginContext;

/* =========================================================================
 * ABI compatibility info — exported by plugin
 * ========================================================================= */

/**
 * @brief ABI descriptor exported by the plugin for compatibility checking.
 *
 * The framework compares these against host values at dlopen time.
 * Mismatch → plugin rejected (hard error).
 */
typedef struct PlhAbiInfo
{
    uint32_t struct_size;       /**< sizeof(PlhAbiInfo) — versioning guard */
    uint32_t sizeof_void_ptr;   /**< sizeof(void*) — 4 or 8 */
    uint32_t sizeof_size_t;     /**< sizeof(size_t) */
    uint32_t byte_order;        /**< 1 = little-endian, 2 = big-endian */
    uint32_t api_version;       /**< PLH_PLUGIN_API_VERSION */
} PlhAbiInfo;

/** Current plugin API version. Increment on breaking changes. */
#define PLH_PLUGIN_API_VERSION 1

/* =========================================================================
 * Required plugin symbols
 * ========================================================================= */

/**
 * @brief Initialize the plugin. Called once after dlopen.
 * @param ctx  Plugin context (valid until plugin_finalize).
 * @return true on success, false to abort role startup.
 */
/* bool plugin_init(const PlhPluginContext *ctx); */

/**
 * @brief Finalize the plugin. Called once before dlclose.
 * Release all resources. Do not call any framework functions after this.
 */
/* void plugin_finalize(void); */

/**
 * @brief Return ABI compatibility info.
 * @return Pointer to static PlhAbiInfo. Must remain valid after return.
 */
/* const PlhAbiInfo *plugin_abi_info(void); */

/* =========================================================================
 * Role callback symbols (implement the ones your role needs)
 * ========================================================================= */

/* void on_init(void); */
/* void on_stop(void); */

/**
 * @brief Producer callback. Write data to out_slot and return true to publish.
 * @param out_slot   Writable buffer (schema-typed). NULL if no slot acquired.
 * @param out_sz     Size of out_slot in bytes.
 * @param flexzone   Writable flexzone buffer (NULL if not configured).
 * @param fz_sz      Size of flexzone in bytes.
 * @return true = commit (publish), false = discard (skip this iteration).
 */
/* bool on_produce(void *out_slot, size_t out_sz, void *flexzone, size_t fz_sz); */

/**
 * @brief Consumer callback. Read data from in_slot.
 * @param in_slot    Read-only buffer (schema-typed). NULL if no data.
 * @param in_sz      Size of in_slot in bytes.
 * @param flexzone   Read-only flexzone buffer (NULL if not configured).
 * @param fz_sz      Size of flexzone in bytes.
 */
/* void on_consume(const void *in_slot, size_t in_sz,
                   const void *flexzone, size_t fz_sz); */

/**
 * @brief Processor callback. Read in_slot, write out_slot.
 * @return true = commit output, false = discard.
 */
/* bool on_process(const void *in_slot, size_t in_sz,
                   void *out_slot, size_t out_sz,
                   void *flexzone, size_t fz_sz); */

/**
 * @brief Inbox callback. Receive a typed message from another role.
 * @param data       Read-only message payload (inbox schema-typed).
 * @param sz         Payload size in bytes.
 * @param sender_uid Null-terminated UID of the sender.
 */
/* void on_inbox(const void *data, size_t sz, const char *sender_uid); */

/**
 * @brief Heartbeat callback. Called periodically from the control thread.
 * Must be thread-safe (called concurrently with data callbacks).
 */
/* void on_heartbeat(void); */

/* =========================================================================
 * Optional plugin metadata symbols
 * ========================================================================= */

/* const char *plugin_name(void);    -- human-readable name for logging */
/* const char *plugin_version(void); -- version string */
/* bool plugin_is_thread_safe(void); -- true if all callbacks are reentrant */

/* =========================================================================
 * Schema validation macros
 * ========================================================================= */

/**
 * @brief Declare a schema descriptor for a slot type.
 *
 * Generates `plugin_schema_<Name>()` and `plugin_sizeof_<Name>()` symbols
 * that the framework uses to verify the plugin's compiled struct matches
 * the JSON config schema at load time.
 *
 * @param Name   Type name (must match register_slot_type type_name:
 *               SlotFrame, InSlotFrame, OutSlotFrame, FlexFrame, InboxFrame)
 * @param Schema Pipe-delimited canonical schema string:
 *               "field_name:type_str:count:length|..."
 *               Example: "ts:float64:1:0|value:float32:1:0"
 * @param Size   sizeof(YourStruct) — must match schema-computed size.
 */
#define PLH_DECLARE_SCHEMA(Name, Schema, Size)                              \
    PLH_EXPORT const char *plugin_schema_##Name(void) { return (Schema); }  \
    PLH_EXPORT size_t plugin_sizeof_##Name(void) { return (Size); }

/* =========================================================================
 * Framework helper functions (implemented by the host, callable by plugin)
 * ========================================================================= */

/**
 * @brief Log a message through the pylabHub logger.
 * @param core   The core pointer from PlhPluginContext.
 * @param level  "debug", "info", "warn", or "error".
 * @param msg    Null-terminated log message.
 */
void plh_log(void *core, const char *level, const char *msg);

/**
 * @brief Report a custom metric (key-value pair).
 * @param core   The core pointer from PlhPluginContext.
 * @param key    Null-terminated metric key.
 * @param value  Metric value.
 */
void plh_report_metric(void *core, const char *key, double value);

/**
 * @brief Request role stop (equivalent to api.stop() in Python/Lua).
 * Signals the main loop to exit gracefully after the current iteration.
 * @param core   The core pointer from PlhPluginContext.
 */
void plh_request_stop(void *core);

/**
 * @brief Signal a critical error (equivalent to api.set_critical_error()).
 * @param core   The core pointer from PlhPluginContext.
 */
void plh_set_critical_error(void *core);

#ifdef __cplusplus
} /* extern "C" */
#endif

/* =========================================================================
 * C++ convenience layer (optional — for C++ plugin authors)
 * ========================================================================= */

#ifdef __cplusplus

#include <cstddef>
#include <cstdint>
#include <type_traits>

namespace plh
{

/**
 * @brief Typed slot reference — zero-cost wrapper around raw pointer.
 *
 * Provides type-safe access to slot data with compile-time standard-layout check.
 * Usage: `plh::SlotRef<MySlotFrame> slot(raw_ptr, raw_sz);`
 */
template <typename T>
class SlotRef
{
    static_assert(std::is_standard_layout_v<T>,
                  "SlotRef<T>: T must be a standard-layout type");
  public:
    SlotRef(void *ptr, size_t sz) noexcept
        : ptr_(static_cast<T *>(ptr)), valid_(ptr && sz >= sizeof(T))
    {}
    SlotRef(const void *ptr, size_t sz) noexcept
        : ptr_(const_cast<T *>(static_cast<const T *>(ptr))), valid_(ptr && sz >= sizeof(T))
    {}

    explicit operator bool() const noexcept { return valid_; }
    T *operator->() noexcept { return ptr_; }
    const T *operator->() const noexcept { return ptr_; }
    T &operator*() noexcept { return *ptr_; }
    const T &operator*() const noexcept { return *ptr_; }
    T *get() noexcept { return ptr_; }
    const T *get() const noexcept { return ptr_; }

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
        : ptr_(static_cast<const T *>(ptr)), valid_(ptr && sz >= sizeof(T))
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

/**
 * @brief Export a C++ producer function with typed slot access.
 *
 * Usage:
 * ```cpp
 * static bool my_produce(plh::SlotRef<SlotFrame> slot, plh::SlotRef<FlexFrame> fz) {
 *     slot->value = 42.0f;
 *     return true;
 * }
 * PLH_EXPORT_PRODUCE(SlotFrame, FlexFrame, my_produce)
 * ```
 */
#define PLH_EXPORT_PRODUCE(SlotType, FlexType, func)                        \
    extern "C" PLH_EXPORT bool on_produce(                                  \
        void *out, size_t out_sz, void *fz, size_t fz_sz)                   \
    {                                                                       \
        return func(plh::SlotRef<SlotType>(out, out_sz),                    \
                    plh::SlotRef<FlexType>(fz, fz_sz));                     \
    }

/** Variant without flexzone. */
#define PLH_EXPORT_PRODUCE_NOFZ(SlotType, func)                             \
    extern "C" PLH_EXPORT bool on_produce(                                  \
        void *out, size_t out_sz, void * /*fz*/, size_t /*fz_sz*/)          \
    {                                                                       \
        return func(plh::SlotRef<SlotType>(out, out_sz));                   \
    }

#define PLH_EXPORT_CONSUME(SlotType, FlexType, func)                        \
    extern "C" PLH_EXPORT void on_consume(                                  \
        const void *in, size_t in_sz, const void *fz, size_t fz_sz)        \
    {                                                                       \
        func(plh::ConstSlotRef<SlotType>(in, in_sz),                        \
             plh::ConstSlotRef<FlexType>(fz, fz_sz));                       \
    }

#define PLH_EXPORT_CONSUME_NOFZ(SlotType, func)                             \
    extern "C" PLH_EXPORT void on_consume(                                  \
        const void *in, size_t in_sz, const void * /*fz*/, size_t /*fz_sz*/)\
    {                                                                       \
        func(plh::ConstSlotRef<SlotType>(in, in_sz));                       \
    }

#define PLH_EXPORT_PROCESS(InType, OutType, FlexType, func)                 \
    extern "C" PLH_EXPORT bool on_process(                                  \
        const void *in, size_t in_sz, void *out, size_t out_sz,             \
        void *fz, size_t fz_sz)                                             \
    {                                                                       \
        return func(plh::ConstSlotRef<InType>(in, in_sz),                   \
                    plh::SlotRef<OutType>(out, out_sz),                      \
                    plh::SlotRef<FlexType>(fz, fz_sz));                     \
    }

#define PLH_EXPORT_PROCESS_NOFZ(InType, OutType, func)                      \
    extern "C" PLH_EXPORT bool on_process(                                  \
        const void *in, size_t in_sz, void *out, size_t out_sz,             \
        void * /*fz*/, size_t /*fz_sz*/)                                    \
    {                                                                       \
        return func(plh::ConstSlotRef<InType>(in, in_sz),                   \
                    plh::SlotRef<OutType>(out, out_sz));                     \
    }

#endif /* __cplusplus */

#endif /* PYLABHUB_PLUGIN_API_H */
