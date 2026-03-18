#pragma once
/**
 * @file lua_processor_host.hpp
 * @brief LuaProcessorHost — LuaRoleHostBase subclass for the processor process.
 *
 * Mirrors ProcessorScriptHost but uses Lua/LuaJIT FFI instead of Python/pybind11.
 * The data loop runs on the worker thread that owns lua_State — all Lua callbacks
 * (on_process, on_inbox, on_init, on_stop) execute on a single thread.
 *
 * Key differences from LuaProducerHost:
 *  - Dual channels: in_consumer_ (input) + out_producer_ (output)
 *  - Two messengers (potential multi-hub): in_messenger_ + out_messenger_
 *  - on_process(in_slot, out_slot, fz, msgs, api) → bool callback
 *  - Read-only input slot, writable output slot
 *  - Flexzone from OUTPUT queue (write_flexzone)
 *  - Manual data loop (no hub::Processor) for single-threaded Lua access
 *
 * See HEP-CORE-0015 for the full processor binary specification.
 */

#include "processor_config.hpp"
#include "processor_schema.hpp"

#include "lua_role_host_base.hpp"

#include "utils/hub_consumer.hpp"
#include "utils/hub_producer.hpp"
#include "utils/hub_queue.hpp"
#include "utils/hub_shm_queue.hpp"
#include "utils/hub_zmq_queue.hpp"
#include "utils/messenger.hpp"

#include <atomic>
#include <optional>
#include <string>
#include <thread>

namespace pylabhub::processor
{

class LuaProcessorHost : public scripting::LuaRoleHostBase
{
  public:
    LuaProcessorHost() = default;
    ~LuaProcessorHost() override;

    LuaProcessorHost(const LuaProcessorHost &)            = delete;
    LuaProcessorHost &operator=(const LuaProcessorHost &) = delete;

    void set_config(ProcessorConfig config);

    [[nodiscard]] const ProcessorConfig &config() const noexcept { return config_; }

  protected:
    // ── Virtual hooks ────────────────────────────────────────────────────────

    const char *role_tag()  const override { return "proc"; }
    const char *role_name() const override { return "processor"; }
    std::string role_uid()  const override { return config_.processor_uid; }
    std::string script_base_dir() const override
    {
        if (config_.role_dir.empty()) return config_.script_path;
        const std::filesystem::path sp(config_.script_path);
        return sp.is_absolute() ? config_.script_path
                                : (std::filesystem::path(config_.role_dir) / sp).string();
    }
    std::string script_type_str() const override { return "lua"; }
    std::string required_callback_name() const override { return "on_process"; }

    void build_role_api_table_(lua_State *L) override;
    void extract_callbacks_() override;
    bool has_required_callback() const override;

    bool build_role_types() override;
    void print_validate_layout() override;

    bool start_role() override;
    void stop_role() override;
    void cleanup_on_start_failure() override;

    void on_script_error() override { core_.script_errors_.fetch_add(1, std::memory_order_relaxed); }
    bool has_connection_for_stop() const override { return out_producer_.has_value(); }
    void update_fz_checksum_after_init() override;
    hub::Messenger *role_messenger_() override { return &out_messenger_; }

    void run_data_loop_() override;

  private:
    // ── Config accessor overrides (LuaRoleHostBase pure virtuals) ─────────
    std::string config_uid()   const override { return config_.processor_uid; }
    std::string config_name()  const override { return config_.processor_name; }
    std::string config_channel() const override { return config_.out_channel; }
    std::string config_log_level()   const override { return config_.log_level; }
    std::string config_script_path() const override { return config_.script_path; }
    std::string config_role_dir()    const override { return config_.role_dir; }
    bool        stop_on_script_error() const override { return config_.stop_on_script_error; }

    ProcessorConfig config_;

    // ── ZMQ connections ──────────────────────────────────────────────────────
    hub::Messenger                 in_messenger_;
    hub::Messenger                 out_messenger_;
    std::optional<hub::Consumer>   in_consumer_;
    std::optional<hub::Producer>   out_producer_;

    // ── Transport-agnostic queues ────────────────────────────────────────────
    // Owned queues (non-null only for SHM or direct-ZMQ transport).
    std::unique_ptr<hub::QueueReader>  in_queue_;
    std::unique_ptr<hub::QueueWriter>  out_queue_;
    // Raw pointers: point to either owned queue or broker-ZMQ queue from Consumer/Producer.
    hub::QueueReader                  *in_q_{nullptr};
    hub::QueueWriter                  *out_q_{nullptr};

    // ── Schema ───────────────────────────────────────────────────────────────
    SchemaSpec in_slot_spec_;
    SchemaSpec out_slot_spec_;
    size_t     in_schema_slot_size_{0};
    size_t     out_schema_slot_size_{0};

    std::string in_slot_ffi_type_{"InSlotFrame"};
    std::string out_slot_ffi_type_{"OutSlotFrame"};
    std::string fz_ffi_type_{"FlexFrame"};
    int ref_on_process_{LUA_NOREF};

    std::thread              ctrl_thread_;
    std::atomic<uint64_t>    iteration_count_{0};

    // Metrics (role-specific; script_errors_ and critical_error_ are in core_)
    std::atomic<uint64_t> in_slots_received_{0};
    std::atomic<uint64_t> out_slots_written_{0};
    std::atomic<uint64_t> out_drops_{0};
    std::atomic<uint64_t> last_cycle_work_us_{0};

    // ── Lua API closures (role-specific only; common ones in base) ───────────

    static int lua_api_uid(lua_State *L);
    static int lua_api_name(lua_State *L);
    static int lua_api_in_channel(lua_State *L);
    static int lua_api_out_channel(lua_State *L);
    static int lua_api_broadcast(lua_State *L);
    static int lua_api_send(lua_State *L);
    static int lua_api_consumers(lua_State *L);
    static int lua_api_update_flexzone_checksum(lua_State *L);
    static int lua_api_out_written(lua_State *L);
    static int lua_api_in_received(lua_State *L);
    static int lua_api_drops(lua_State *L);
    static int lua_api_set_verify_checksum(lua_State *L);

    nlohmann::json snapshot_metrics_json() const;

    // ── Internal helpers ─────────────────────────────────────────────────────

    void run_ctrl_thread_();

    bool call_on_process_(const void *in_buf, size_t in_sz,
                           void *out_buf, size_t out_sz,
                           std::vector<scripting::IncomingMessage> &msgs);
    bool call_on_process_no_input_(void *out_buf, size_t out_sz,
                                    std::vector<scripting::IncomingMessage> &msgs);
    void push_flexzone_view_();
};

} // namespace pylabhub::processor
