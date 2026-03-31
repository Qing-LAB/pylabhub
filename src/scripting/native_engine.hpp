#pragma once
/**
 * @file native_engine.hpp
 * @brief NativeEngine — ScriptEngine implementation for native C/C++ plugins.
 *
 * Loads a shared library (.so/.dll) via dlopen/LoadLibrary, resolves
 * callback symbols, and dispatches invoke calls as direct function pointer
 * calls with zero marshaling overhead.
 *
 * Plugin API: see src/include/utils/plugin_api.h
 * Specification: see HEP-CORE-0028
 */

#include "utils/script_engine.hpp"

#include <filesystem>
#include <string>
#include <unordered_map>

namespace pylabhub::scripting
{

class NativeEngine : public ScriptEngine
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
    bool build_api_(const RoleContext &ctx) override;
    void finalize_engine_() override;

    // ── Queries ──────────────────────────────────────────────────────────

    [[nodiscard]] bool has_callback(const std::string &name) const override;

    // ── Schema / type building ───────────────────────────────────────────

    bool register_slot_type(const SchemaSpec &spec,
                            const std::string &type_name,
                            const std::string &packing) override;
    [[nodiscard]] size_t type_sizeof(const std::string &type_name) const override;

    // ── Callback invocation ──────────────────────────────────────────────

    void invoke_on_init() override;
    void invoke_on_stop() override;

    InvokeResult invoke_produce(
        void *out_slot, size_t out_sz,
        void *flexzone, size_t fz_sz, const char *fz_type,
        std::vector<IncomingMessage> &msgs) override;

    void invoke_consume(
        const void *in_slot, size_t in_sz,
        const void *flexzone, size_t fz_sz, const char *fz_type,
        std::vector<IncomingMessage> &msgs) override;

    InvokeResult invoke_process(
        const void *in_slot, size_t in_sz,
        void *out_slot, size_t out_sz,
        void *flexzone, size_t fz_sz, const char *fz_type,
        std::vector<IncomingMessage> &msgs) override;

    void invoke_on_inbox(
        const void *data, size_t sz,
        const std::string &sender) override;

    // ── Generic invoke ───────────────────────────────────────────────────

    bool invoke(const std::string &name) override;
    bool invoke(const std::string &name, const nlohmann::json &args) override;
    InvokeResponse eval(const std::string &code) override;

    // ── Error state ──────────────────────────────────────────────────────

    [[nodiscard]] uint64_t script_error_count() const noexcept override;

    // ── Threading ────────────────────────────────────────────────────────

    [[nodiscard]] bool supports_multi_state() const noexcept override;
    void release_thread() override {}

    /// Set the expected BLAKE2b-256 hex checksum for the plugin .so file.
    /// If set, load_script() verifies the file hash before dlopen.
    void set_expected_checksum(std::string hex_checksum);

  private:
    std::string log_tag_;

    // ── Dynamic library handle ──────────────────────────────────────────
    void *dl_handle_{nullptr};

    // ── Resolved function pointers (C linkage) ──────────────────────────
    using FnPluginInit     = bool (*)(const void *ctx);
    using FnPluginFinalize = void (*)();
    using FnVoid           = void (*)();
    using FnOnProduce      = bool (*)(void *, size_t, void *, size_t);
    using FnOnConsume      = void (*)(const void *, size_t, const void *, size_t);
    using FnOnProcess      = bool (*)(const void *, size_t, void *, size_t, void *, size_t);
    using FnOnInbox        = void (*)(const void *, size_t, const char *);
    using FnSchemaDesc     = const char *(*)();
    using FnSizeof         = size_t (*)();
    using FnBool           = bool (*)();
    using FnCstr           = const char *(*)();

    FnPluginInit     fn_init_{nullptr};
    FnPluginFinalize fn_finalize_{nullptr};
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

    // ── Plugin context (kept alive for plugin lifetime) ─────────────────
    struct PluginContextStorage;
    std::unique_ptr<PluginContextStorage> plugin_ctx_;

    // ── File integrity ──────────────────────────────────────────────────
    std::string expected_checksum_;  ///< BLAKE2b-256 hex, empty = skip check
    std::filesystem::path lib_path_;

    // ── Internal helpers ────────────────────────────────────────────────
    void *resolve_sym_(const char *name) const;
    bool verify_abi_() const;
    bool verify_file_checksum_() const;
    std::string compute_canonical_schema_(const SchemaSpec &spec) const;
};

} // namespace pylabhub::scripting
