#pragma once
/**
 * @file native_engine.hpp
 * @brief NativeEngine — ScriptEngine implementation for native C/C++ shared libraries.
 *
 * Loads a shared library (.so/.dll) via dlopen/LoadLibrary, resolves
 * callback symbols, and dispatches invoke calls as direct function pointer
 * calls with zero marshaling overhead.
 *
 * Native engine API: see src/include/utils/native_engine_api.h
 * Specification: see HEP-CORE-0028
 */

#include "pylabhub_utils_export.h"
#include "utils/script_engine.hpp"
#include "utils/native_invoke_types.h"

#include <filesystem>
#include <string>
#include <unordered_map>

namespace pylabhub::scripting
{

class PYLABHUB_UTILS_EXPORT NativeEngine : public ScriptEngine
{
  public:
    NativeEngine();
    ~NativeEngine() override;

    NativeEngine(const NativeEngine &) = delete;
    NativeEngine &operator=(const NativeEngine &) = delete;

    // ── ScriptEngine lifecycle ───────────────────────────────────────────

    bool init_engine_(const std::string &log_tag, RoleHostCore *core) override;
    bool load_script(const std::filesystem::path &script_dir,
                     const std::string &entry_point,
                     const std::string &required_callback) override;
    bool build_api_(RoleAPIBase &api) override;
    void finalize_engine_() override;

    // ── Queries ──────────────────────────────────────────────────────────

    [[nodiscard]] bool has_callback(const std::string &name) const override;

    // ── Schema / type building ───────────────────────────────────────────

    bool register_slot_type(const hub::SchemaSpec &spec,
                            const std::string &type_name,
                            const std::string &packing) override;
    [[nodiscard]] size_t type_sizeof(const std::string &type_name) const override;

    // ── Callback invocation ──────────────────────────────────────────────

    void invoke_on_init() override;
    void invoke_on_stop() override;

    InvokeResult invoke_produce(
        InvokeTx tx,
        std::vector<IncomingMessage> &msgs) override;

    InvokeResult invoke_consume(
        InvokeRx rx,
        std::vector<IncomingMessage> &msgs) override;

    InvokeResult invoke_process(
        InvokeRx rx, InvokeTx tx,
        std::vector<IncomingMessage> &msgs) override;

    InvokeResult invoke_on_inbox(
        InvokeInbox msg) override;

    // ── Generic invoke ───────────────────────────────────────────────────

    bool invoke(const std::string &name) override;
    bool invoke(const std::string &name, const nlohmann::json &args) override;
    InvokeResponse eval(const std::string &code) override;
    /// HEP-CORE-0033 §12.2.2 augmentation hook entry point.  Native
    /// engines have no script-side return-value capture surface today
    /// (compiled C plugins use the typed cycle ops instead) — this
    /// returns NotFound so HubAPI::augment_* leaves the default
    /// response unchanged.  `process_pending` inherits the base
    /// no-op default — there's no cross-thread pending queue here.
    InvokeResponse invoke_returning(const std::string &name,
                                    const nlohmann::json &args,
                                    int64_t timeout_ms = -1) override;

    // ── Error state ──────────────────────────────────────────────────────

    [[nodiscard]] uint64_t script_error_count() const noexcept override;

    // ── Threading ────────────────────────────────────────────────────────

    [[nodiscard]] bool supports_multi_state() const noexcept override;
    void release_thread() override {}

    /// Set the expected BLAKE2b-256 hex checksum for the native .so file.
    /// If set, load_script() verifies the file hash before dlopen.
    void set_expected_checksum(std::string hex_checksum);

  private:
    std::string log_tag_;

    // ── Dynamic library handle ──────────────────────────────────────────
    void *dl_handle_{nullptr};

    // ── Resolved function pointers (C linkage) ──────────────────────────
    using FnNativeInit     = bool (*)(const void *ctx);
    using FnNativeFinalize = void (*)();
    using FnVoid           = void (*)(const char *args_json);
    using FnOnProduce      = bool (*)(const plh_tx_t *);
    using FnOnConsume      = bool (*)(const plh_rx_t *);
    using FnOnProcess      = bool (*)(const plh_rx_t *, const plh_tx_t *);
    using FnOnInbox        = bool (*)(const plh_inbox_msg_t *);
    using FnSchemaDesc     = const char *(*)();
    using FnSizeof         = size_t (*)();
    using FnBool           = bool (*)();
    using FnCstr           = const char *(*)();

    FnNativeInit     fn_init_{nullptr};
    FnNativeFinalize fn_finalize_{nullptr};
    FnVoid           fn_on_init_{nullptr};
    FnVoid           fn_on_stop_{nullptr};
    FnOnProduce      fn_on_produce_{nullptr};
    FnOnConsume      fn_on_consume_{nullptr};
    FnOnProcess      fn_on_process_{nullptr};
    FnOnInbox        fn_on_inbox_{nullptr};
    FnVoid           fn_on_heartbeat_{nullptr};
    FnBool           fn_is_thread_safe_{nullptr};

    // ── Schema tracking ─────────────────────────────────────────────────
    std::unordered_map<std::string, size_t> type_sizes_;

    // ── Native context (kept alive for engine lifetime) ─────────────────
    struct NativeContextStorage;
    std::unique_ptr<NativeContextStorage> native_ctx_;

    // ── File integrity ──────────────────────────────────────────────────────
    std::string expected_checksum_;  ///< BLAKE2b-256 hex, empty = skip check

    // ── Lifecycle ───────────────────────────────────────────────────────────
    std::string lifecycle_module_name_;  ///< Unique name for lifecycle registration
    bool lifecycle_registered_{false};   ///< True if module was registered
    std::filesystem::path lib_path_;

    // ── Internal helpers ────────────────────────────────────────────────
    void *resolve_sym_(const char *name) const;
    bool verify_abi_() const;
    bool verify_file_checksum_() const;
    std::string compute_canonical_schema_(const hub::SchemaSpec &spec) const;
};

} // namespace pylabhub::scripting
