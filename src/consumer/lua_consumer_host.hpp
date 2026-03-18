#pragma once
/**
 * @file lua_consumer_host.hpp
 * @brief LuaConsumerHost — LuaRoleHostBase subclass for the consumer process.
 *
 * Mirrors ConsumerScriptHost but uses Lua/LuaJIT FFI instead of Python/pybind11.
 * The data loop runs on the worker thread that owns lua_State — all Lua callbacks
 * (on_consume, on_inbox, on_init, on_stop) execute on a single thread.
 *
 * See HEP-CORE-0018 for the full consumer binary specification.
 */

#include "consumer_config.hpp"
#include "consumer_schema.hpp"

#include "lua_role_host_base.hpp"

#include "utils/hub_consumer.hpp"
#include "utils/hub_queue.hpp"
#include "utils/hub_shm_queue.hpp"
#include "utils/hub_zmq_queue.hpp"
#include "utils/messenger.hpp"

#include <atomic>
#include <optional>
#include <string>
#include <thread>

namespace pylabhub::consumer
{

class LuaConsumerHost : public scripting::LuaRoleHostBase
{
  public:
    LuaConsumerHost() = default;
    ~LuaConsumerHost() override;

    LuaConsumerHost(const LuaConsumerHost &)            = delete;
    LuaConsumerHost &operator=(const LuaConsumerHost &) = delete;

    void set_config(ConsumerConfig config);

    [[nodiscard]] const ConsumerConfig &config() const noexcept { return config_; }

  protected:
    // -- Virtual hooks --------------------------------------------------------

    const char *role_tag()  const override { return "cons"; }
    const char *role_name() const override { return "consumer"; }
    std::string role_uid()  const override { return config_.consumer_uid; }
    std::string script_base_dir() const override
    {
        if (config_.role_dir.empty()) return config_.script_path;
        const std::filesystem::path sp(config_.script_path);
        return sp.is_absolute() ? config_.script_path
                                : (std::filesystem::path(config_.role_dir) / sp).string();
    }
    std::string script_type_str() const override { return "lua"; }
    std::string required_callback_name() const override { return "on_consume"; }

    void build_role_api_table_(lua_State *L) override;
    void extract_callbacks_() override;
    bool has_required_callback() const override;

    bool build_role_types() override;
    void print_validate_layout() override;

    bool start_role() override;
    void stop_role() override;
    void cleanup_on_start_failure() override;

    void on_script_error() override { core_.script_errors_.fetch_add(1, std::memory_order_relaxed); }
    bool has_connection_for_stop() const override { return in_consumer_.has_value(); }
    hub::Messenger *role_messenger_() override { return &in_messenger_; }

    /** Consumer messages are bare bytes (no sender). */
    void push_messages_table_(std::vector<scripting::IncomingMessage> &msgs) override;

    void run_data_loop_() override;

  private:
    // ── Config accessor overrides (LuaRoleHostBase pure virtuals) ─────────
    std::string config_uid()   const override { return config_.consumer_uid; }
    std::string config_name()  const override { return config_.consumer_name; }
    std::string config_channel() const override { return config_.channel; }
    std::string config_log_level()   const override { return config_.log_level; }
    std::string config_script_path() const override { return config_.script_path; }
    std::string config_role_dir()    const override { return config_.role_dir; }
    bool        stop_on_script_error() const override { return config_.stop_on_script_error; }

    ConsumerConfig config_;

    hub::Messenger              in_messenger_;
    std::optional<hub::Consumer> in_consumer_;

    /// SHM transport: owned QueueReader wrapping DataBlockConsumer.
    /// ZMQ transport: nullptr (queue_reader_ points to Consumer-owned ZmqQueue).
    std::unique_ptr<hub::QueueReader> shm_queue_;
    /// Non-owning pointer to the active QueueReader (shm_queue_.get() or Consumer::queue_reader()).
    hub::QueueReader *queue_reader_{nullptr};

    SchemaSpec slot_spec_;
    size_t     schema_slot_size_{0};

    std::string slot_ffi_type_{"SlotFrame"};
    std::string fz_ffi_type_{"FlexFrame"};

    int ref_on_consume_{LUA_NOREF};

    std::thread              ctrl_thread_;
    std::atomic<uint64_t>    iteration_count_{0};

    // Metrics (role-specific; script_errors_ and critical_error_ are in core_)
    std::atomic<uint64_t> in_slots_received_{0};
    std::atomic<uint64_t> last_cycle_work_us_{0};

    // -- Lua API closures -----------------------------------------------------

    static int lua_api_uid(lua_State *L);
    static int lua_api_name(lua_State *L);
    static int lua_api_channel(lua_State *L);
    static int lua_api_in_received(lua_State *L);
    static int lua_api_set_verify_checksum(lua_State *L);

    nlohmann::json snapshot_metrics_json() const;

    // -- Internal helpers -----------------------------------------------------

    void run_ctrl_thread_();
    void push_flexzone_view_readonly_();

    void call_on_consume_(const void *buf, size_t buf_sz,
                          std::vector<scripting::IncomingMessage> &msgs);
    void call_on_consume_no_slot_(std::vector<scripting::IncomingMessage> &msgs);
};

} // namespace pylabhub::consumer
