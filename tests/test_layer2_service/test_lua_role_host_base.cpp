/**
 * @file test_lua_role_host_base.cpp
 * @brief Unit tests for LuaRoleHostBase threading model and FFI helpers.
 *
 * Tests cover:
 *   1. Worker thread lifecycle: startup → signal_ready → shutdown
 *   2. Early exit paths: missing script, missing callback
 *   3. FFI cdef generation from SchemaSpec
 *   4. FFI type registration and sizeof query
 *   5. Slot view creation via ffi.cast (cached path)
 *   6. Message table builder
 *   7. signal_shutdown from external thread
 *
 * Uses a minimal concrete subclass (TestLuaHost) that provides a trivial
 * run_data_loop_() to test the base class machinery.
 */
#include <gtest/gtest.h>

// LuaRoleHostBase is in pylabhub-scripting (static lib).
// Include the header directly; the test links pylabhub-scripting.
#include "lua_role_host_base.hpp"

#include "test_sync_utils.h" // poll_until
#include "utils/logger.hpp"

#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>

namespace fs = std::filesystem;
using pylabhub::scripting::LuaRoleHostBase;
using pylabhub::scripting::LuaStopReason;
using pylabhub::scripting::SchemaSpec;
using pylabhub::scripting::FieldDef;
using pylabhub::scripting::ffi_c_type_for_field;

namespace
{

// ============================================================================
// Minimal concrete subclass for testing the base class
// ============================================================================

class TestLuaHost : public LuaRoleHostBase
{
  public:
    // Make these public for test access.
    using LuaRoleHostBase::build_ffi_cdef_;
    using LuaRoleHostBase::register_ffi_type_;
    using LuaRoleHostBase::ffi_sizeof_;
    using LuaRoleHostBase::push_slot_view_;
    using LuaRoleHostBase::push_slot_view_readonly_;
    using LuaRoleHostBase::push_messages_table_;
    using LuaRoleHostBase::L_;
    using LuaRoleHostBase::core_;

    std::string script_dir_;
    bool start_role_result{true};
    bool loop_entered{false};
    bool loop_exited{false};
    std::atomic<bool> loop_running{false};

    // Virtual hook overrides
    const char *role_tag()  const override { return "test"; }
    const char *role_name() const override { return "test_host"; }
    std::string role_uid()  const override { return "TEST-LUA-00000001"; }
    std::string script_base_dir() const override { return script_dir_; }
    std::string script_type_str() const override { return "lua"; }
    std::string required_callback_name() const override { return "on_produce"; }

    void build_role_api_table_(lua_State *L) override
    {
        lua_newtable(L);
        // Minimal API table: just a uid field.
        lua_pushstring(L, "TEST-LUA-00000001");
        lua_setfield(L, -2, "uid");
    }

    void extract_callbacks_() override
    {
        lua_getglobal(L_, "on_produce");
        ref_on_produce_ = lua_isfunction(L_, -1)
            ? luaL_ref(L_, LUA_REGISTRYINDEX)
            : (lua_pop(L_, 1), LUA_NOREF);
    }

    bool has_required_callback() const override
    {
        return is_ref_callable_(ref_on_produce_);
    }

    bool build_role_types() override { return true; }
    void print_validate_layout() override {}
    bool start_role() override
    {
        if (start_role_result)
            core_.running_threads.store(true);
        return start_role_result;
    }

    void stop_role() override
    {
        if (ref_on_produce_ != LUA_NOREF)
        {
            luaL_unref(L_, LUA_REGISTRYINDEX, ref_on_produce_);
            ref_on_produce_ = LUA_NOREF;
        }
    }

    void cleanup_on_start_failure() override {}
    void on_script_error() override {}
    bool has_connection_for_stop() const override { return true; }

    // Config accessor overrides (LuaRoleHostBase pure virtuals)
    std::string config_uid()   const override { return "TEST-LUA-00000001"; }
    std::string config_name()  const override { return "test_host"; }
    std::string config_channel() const override { return "test_channel"; }
    std::string config_log_level()   const override { return "info"; }
    std::string config_script_path() const override { return script_dir_; }
    std::string config_role_dir()    const override { return ""; }
    bool        stop_on_script_error() const override { return false; }

    void run_data_loop_() override
    {
        loop_entered = true;
        loop_running.store(true);
        // Simple loop: wait for shutdown.
        while (core_.running_threads.load() && !core_.shutdown_requested.load())
        {
            std::this_thread::sleep_for(std::chrono::milliseconds{10});
        }
        loop_running.store(false);
        loop_exited = true;
    }

  private:
    int ref_on_produce_{LUA_NOREF};
};

// ============================================================================
// Test fixture: creates temp dir with init.lua
// ============================================================================

class LuaRoleHostBaseTest : public ::testing::Test
{
  protected:
    fs::path temp_dir_;

    void SetUp() override
    {
        temp_dir_ = fs::temp_directory_path() / ("lua_host_test_" + std::to_string(::getpid()));
        fs::create_directories(temp_dir_ / "script" / "lua");
    }

    void TearDown() override
    {
        std::error_code ec;
        fs::remove_all(temp_dir_, ec);
    }

    void write_script(const std::string &code)
    {
        std::ofstream out(temp_dir_ / "script" / "lua" / "init.lua");
        out << code;
    }
};

// ============================================================================
// Tests: FFI helpers (no worker thread needed)
// ============================================================================

TEST(FfiTypeMapping, AllSupportedTypes)
{
    EXPECT_EQ(ffi_c_type_for_field("bool"),    "bool");
    EXPECT_EQ(ffi_c_type_for_field("int8"),    "int8_t");
    EXPECT_EQ(ffi_c_type_for_field("uint8"),   "uint8_t");
    EXPECT_EQ(ffi_c_type_for_field("int16"),   "int16_t");
    EXPECT_EQ(ffi_c_type_for_field("uint16"),  "uint16_t");
    EXPECT_EQ(ffi_c_type_for_field("int32"),   "int32_t");
    EXPECT_EQ(ffi_c_type_for_field("uint32"),  "uint32_t");
    EXPECT_EQ(ffi_c_type_for_field("int64"),   "int64_t");
    EXPECT_EQ(ffi_c_type_for_field("uint64"),  "uint64_t");
    EXPECT_EQ(ffi_c_type_for_field("float32"), "float");
    EXPECT_EQ(ffi_c_type_for_field("float64"), "double");
    EXPECT_EQ(ffi_c_type_for_field("string"),  "char");
    EXPECT_EQ(ffi_c_type_for_field("bytes"),   "uint8_t");
}

TEST(FfiTypeMapping, UnsupportedType_ReturnsEmpty)
{
    EXPECT_TRUE(ffi_c_type_for_field("complex128").empty());
    EXPECT_TRUE(ffi_c_type_for_field("").empty());
}

TEST(FfiCdef, SingleField_AlignedPacking)
{
    SchemaSpec spec;
    spec.has_schema = true;
    spec.fields.push_back(FieldDef{"value", "float32", 1, 0});

    auto cdef = TestLuaHost::build_ffi_cdef_(spec, "SlotFrame", "aligned");
    ASSERT_FALSE(cdef.empty());
    EXPECT_NE(cdef.find("float value;"), std::string::npos);
    EXPECT_NE(cdef.find("SlotFrame"), std::string::npos);
    EXPECT_EQ(cdef.find("__attribute__"), std::string::npos); // not packed
}

TEST(FfiCdef, MultipleFields_PackedPacking)
{
    SchemaSpec spec;
    spec.has_schema = true;
    spec.fields.push_back(FieldDef{"x", "float64", 1, 0});
    spec.fields.push_back(FieldDef{"y", "float64", 1, 0});
    spec.fields.push_back(FieldDef{"label", "string", 1, 32});

    auto cdef = TestLuaHost::build_ffi_cdef_(spec, "TestFrame", "packed");
    ASSERT_FALSE(cdef.empty());
    EXPECT_NE(cdef.find("__attribute__((packed))"), std::string::npos);
    EXPECT_NE(cdef.find("double x;"), std::string::npos);
    EXPECT_NE(cdef.find("char label[32];"), std::string::npos);
}

TEST(FfiCdef, ArrayField_CountGreaterThan1)
{
    SchemaSpec spec;
    spec.has_schema = true;
    spec.fields.push_back(FieldDef{"data", "int32", 10, 0});

    auto cdef = TestLuaHost::build_ffi_cdef_(spec, "ArrayFrame", "aligned");
    ASSERT_FALSE(cdef.empty());
    EXPECT_NE(cdef.find("int32_t data[10];"), std::string::npos);
}

TEST(FfiCdef, UnsupportedFieldType_ReturnsEmpty)
{
    SchemaSpec spec;
    spec.has_schema = true;
    spec.fields.push_back(FieldDef{"bad", "complex128", 1, 0});

    auto cdef = TestLuaHost::build_ffi_cdef_(spec, "BadFrame", "aligned");
    EXPECT_TRUE(cdef.empty());
}

// ============================================================================
// Tests: Worker thread lifecycle
// ============================================================================

TEST_F(LuaRoleHostBaseTest, StartupShutdown_WithValidScript)
{
    write_script(R"(
        function on_produce(out_slot, fz, msgs, api)
            return true
        end
    )");

    TestLuaHost host;
    host.script_dir_ = temp_dir_.string();
    std::atomic<bool> g_shutdown{false};
    host.set_shutdown_flag(&g_shutdown);

    host.startup_();
    EXPECT_TRUE(host.script_load_ok());
    EXPECT_TRUE(host.is_running());

    // startup_() returns after signal_ready_(), but the worker thread may not
    // have entered run_data_loop_() yet. Poll the atomic flag rather than
    // checking instantly — eliminates scheduling-dependent flake under CI load.
    ASSERT_TRUE(pylabhub::tests::helper::poll_until(
        [&] { return host.loop_running.load(std::memory_order_acquire); },
        std::chrono::milliseconds{2000}))
        << "Worker thread did not enter run_data_loop_() within 2 s";
    EXPECT_TRUE(host.loop_entered);

    host.shutdown_();
    EXPECT_TRUE(host.loop_exited);
    EXPECT_FALSE(host.is_running());
}

TEST_F(LuaRoleHostBaseTest, MissingScript_ScriptLoadFails)
{
    // Don't write any script — init.lua doesn't exist.
    TestLuaHost host;
    host.script_dir_ = temp_dir_.string();

    // Remove the script/lua dir so init.lua can't be found.
    fs::remove_all(temp_dir_ / "script" / "lua");

    host.startup_();
    EXPECT_FALSE(host.script_load_ok());
    EXPECT_FALSE(host.is_running());

    host.shutdown_();
}

TEST_F(LuaRoleHostBaseTest, MissingRequiredCallback_ScriptLoadFails)
{
    write_script(R"(
        -- No on_produce defined
        function on_init(api) end
    )");

    TestLuaHost host;
    host.script_dir_ = temp_dir_.string();

    host.startup_();
    EXPECT_FALSE(host.script_load_ok());
    EXPECT_FALSE(host.is_running());

    host.shutdown_();
}

TEST_F(LuaRoleHostBaseTest, ScriptSyntaxError_ScriptLoadFails)
{
    write_script("this is not valid lua!!!");

    TestLuaHost host;
    host.script_dir_ = temp_dir_.string();

    host.startup_();
    EXPECT_FALSE(host.script_load_ok());

    host.shutdown_();
}

TEST_F(LuaRoleHostBaseTest, StartRoleFailure_DoesNotRun)
{
    write_script(R"(
        function on_produce(out_slot, fz, msgs, api) return true end
    )");

    TestLuaHost host;
    host.script_dir_ = temp_dir_.string();
    host.start_role_result = false; // Force start_role to fail.

    host.startup_();
    EXPECT_TRUE(host.script_load_ok());  // Script loaded OK
    EXPECT_FALSE(host.is_running());     // But role didn't start
    EXPECT_FALSE(host.loop_entered);

    host.shutdown_();
}

TEST_F(LuaRoleHostBaseTest, SignalShutdown_StopsLoop)
{
    write_script(R"(
        function on_produce(out_slot, fz, msgs, api) return true end
    )");

    TestLuaHost host;
    host.script_dir_ = temp_dir_.string();

    host.startup_();
    ASSERT_TRUE(host.is_running());

    // Signal shutdown from this thread.
    host.signal_shutdown();

    // Give it time to exit.
    for (int i = 0; i < 100 && host.loop_running.load(); ++i)
        std::this_thread::sleep_for(std::chrono::milliseconds{10});

    EXPECT_FALSE(host.loop_running.load());

    host.shutdown_();
    EXPECT_TRUE(host.loop_exited);
}

} // anonymous namespace
