#pragma once
/**
 * @file lua_producer_host.hpp
 * @brief LuaProducerHost — LuaRoleHostBase subclass for the producer process.
 *
 * Mirrors ProducerScriptHost but uses Lua/LuaJIT FFI instead of Python/pybind11.
 * The data loop runs on the worker thread that owns lua_State — all Lua callbacks
 * (on_produce, on_inbox, on_init, on_stop) execute on a single thread.
 *
 * See HEP-CORE-0018 for the full producer binary specification.
 */

#include "producer_config.hpp"
#include "producer_schema.hpp"

#include "lua_role_host_base.hpp"

#include "utils/hub_producer.hpp"
#include "utils/hub_queue.hpp"
#include "utils/hub_shm_queue.hpp"
#include "utils/hub_zmq_queue.hpp"
#include "utils/messenger.hpp"

#include <atomic>
#include <optional>
#include <string>
#include <thread>

namespace pylabhub::producer
{

class LuaProducerHost : public scripting::LuaRoleHostBase
{
  public:
    LuaProducerHost() = default;
    ~LuaProducerHost() override;

    LuaProducerHost(const LuaProducerHost &)            = delete;
    LuaProducerHost &operator=(const LuaProducerHost &) = delete;

    void set_config(ProducerConfig config);

    [[nodiscard]] const ProducerConfig &config() const noexcept { return config_; }

  protected:
    // ── Virtual hooks ────────────────────────────────────────────────────────

    const char *role_tag()  const override { return "prod"; }
    const char *role_name() const override { return "producer"; }
    std::string role_uid()  const override { return config_.producer_uid; }
    std::string script_base_dir() const override
    {
        if (config_.role_dir.empty()) return config_.script_path;
        const std::filesystem::path sp(config_.script_path);
        return sp.is_absolute() ? config_.script_path
                                : (std::filesystem::path(config_.role_dir) / sp).string();
    }
    std::string script_type_str() const override { return "lua"; }
    std::string required_callback_name() const override { return "on_produce"; }

    void build_role_api_table_(lua_State *L) override;
    void extract_callbacks_() override;
    bool has_required_callback() const override;

    bool build_role_types() override;
    void print_validate_layout() override;

    bool start_role() override;
    void stop_role() override;
    void cleanup_on_start_failure() override;

    void on_script_error() override { ++script_errors_; }
    bool has_connection_for_stop() const override { return out_producer_.has_value(); }
    void update_fz_checksum_after_init() override;

    void run_data_loop_() override;

  private:
    // ── Config accessor overrides (LuaRoleHostBase pure virtuals) ─────────
    std::string config_uid()   const override { return config_.producer_uid; }
    std::string config_name()  const override { return config_.producer_name; }
    std::string config_channel() const override { return config_.channel; }
    std::string config_log_level()   const override { return config_.log_level; }
    std::string config_script_path() const override { return config_.script_path; }
    std::string config_role_dir()    const override { return config_.role_dir; }
    bool        stop_on_script_error() const override { return config_.stop_on_script_error; }

    ProducerConfig config_;

    hub::Messenger               out_messenger_;
    std::optional<hub::Producer> out_producer_;

    /// Transport-agnostic output queue.
    std::unique_ptr<hub::QueueWriter> queue_;

    SchemaSpec slot_spec_;
    size_t     schema_slot_size_{0};

    std::string slot_ffi_type_{"SlotFrame"};
    std::string fz_ffi_type_{"FlexFrame"};

    int ref_on_produce_{LUA_NOREF};

    std::thread              ctrl_thread_;
    std::atomic<uint64_t>    iteration_count_{0};

    // Metrics (role-specific; script_errors_ and critical_error_ are in base)
    std::atomic<uint64_t> out_slots_written_{0};
    std::atomic<uint64_t> out_drops_{0};
    std::atomic<uint64_t> last_cycle_work_us_{0};

    // ── Lua API closures ─────────────────────────────────────────────────────
    // Registered as C closures with `this` as lightuserdata upvalue.

    static int lua_api_uid(lua_State *L);
    static int lua_api_name(lua_State *L);
    static int lua_api_channel(lua_State *L);
    static int lua_api_broadcast(lua_State *L);
    static int lua_api_send(lua_State *L);
    static int lua_api_consumers(lua_State *L);
    static int lua_api_update_flexzone_checksum(lua_State *L);
    static int lua_api_out_written(lua_State *L);
    static int lua_api_drops(lua_State *L);

    nlohmann::json snapshot_metrics_json() const;

    // ── Internal helpers ─────────────────────────────────────────────────────

    void run_ctrl_thread_();

    bool call_on_produce_(void *buf, size_t buf_sz, std::vector<scripting::IncomingMessage> &msgs);
    bool call_on_produce_no_slot_(std::vector<scripting::IncomingMessage> &msgs);
    void push_flexzone_view_();
};

} // namespace pylabhub::producer
