/**
 * @file test_module_def.cpp
 * @brief Tests for ModuleDef builder API (module_def.hpp).
 *
 * These tests exercise the public builder API: construction, setters, validation.
 * Internal state is only accessible via LifecycleManager (friend), so we only
 * test the API surface — no peeking at pImpl internals.
 */
#include <plh_base.hpp>

#include <gtest/gtest.h>

#include <string>

using namespace pylabhub::utils;
using namespace std::chrono_literals;

// ============================================================================
// Constants
// ============================================================================

TEST(ModuleDefTest, Constants_Values)
{
    EXPECT_EQ(ModuleDef::MAX_MODULE_NAME_LEN, 256u);
    EXPECT_EQ(ModuleDef::MAX_CALLBACK_PARAM_STRLEN, 1024u);
}

// ============================================================================
// Construction
// ============================================================================

TEST(ModuleDefTest, Constructor_ValidName)
{
    EXPECT_NO_THROW(ModuleDef("TestModule"));
}

TEST(ModuleDefTest, Constructor_EmptyName_Throws)
{
    EXPECT_THROW({ ModuleDef m(""); }, std::invalid_argument);
}

TEST(ModuleDefTest, Constructor_MaxLengthName)
{
    std::string name(ModuleDef::MAX_MODULE_NAME_LEN, 'A');
    EXPECT_NO_THROW({ ModuleDef m(name); });
}

TEST(ModuleDefTest, Constructor_TooLongName_Throws)
{
    std::string name(ModuleDef::MAX_MODULE_NAME_LEN + 1, 'A');
    EXPECT_THROW({ ModuleDef m(name); }, std::length_error);
}

// ============================================================================
// Move semantics
// ============================================================================

TEST(ModuleDefTest, MoveConstructor_TransfersOwnership)
{
    ModuleDef m1("MoveSource");
    ModuleDef m2(std::move(m1));
    // m2 should be valid (no crash on destruction)
    // m1 is in a valid but moved-from state
}

TEST(ModuleDefTest, MoveAssignment_TransfersOwnership)
{
    ModuleDef m1("Source");
    ModuleDef m2("Target");
    m2 = std::move(m1);
    // m2 now owns Source's state; m1 is moved-from
}

// ============================================================================
// add_dependency
// ============================================================================

TEST(ModuleDefTest, AddDependency_Valid)
{
    ModuleDef m("TestModule");
    EXPECT_NO_THROW(m.add_dependency("Logger"));
}

TEST(ModuleDefTest, AddDependency_Empty_Ignored)
{
    ModuleDef m("TestModule");
    // Empty dependency is silently ignored (no throw)
    EXPECT_NO_THROW(m.add_dependency(""));
}

TEST(ModuleDefTest, AddDependency_TooLong_Throws)
{
    ModuleDef m("TestModule");
    std::string dep(ModuleDef::MAX_MODULE_NAME_LEN + 1, 'D');
    EXPECT_THROW(m.add_dependency(dep), std::length_error);
}

// ============================================================================
// set_startup
// ============================================================================

static void dummy_callback(const char *) {}

TEST(ModuleDefTest, SetStartup_NoArg)
{
    ModuleDef m("TestModule");
    EXPECT_NO_THROW(m.set_startup(dummy_callback));
}

TEST(ModuleDefTest, SetStartup_WithArg)
{
    ModuleDef m("TestModule");
    EXPECT_NO_THROW(m.set_startup(dummy_callback, "config_path"));
}

TEST(ModuleDefTest, SetStartup_ArgTooLong_Throws)
{
    ModuleDef m("TestModule");
    std::string arg(ModuleDef::MAX_CALLBACK_PARAM_STRLEN + 1, 'X');
    EXPECT_THROW(m.set_startup(dummy_callback, arg), std::length_error);
}

// ============================================================================
// set_shutdown
// ============================================================================

TEST(ModuleDefTest, SetShutdown_NoArg)
{
    ModuleDef m("TestModule");
    EXPECT_NO_THROW(m.set_shutdown(dummy_callback, 1000ms));
}

TEST(ModuleDefTest, SetShutdown_ArgTooLong_Throws)
{
    ModuleDef m("TestModule");
    std::string arg(ModuleDef::MAX_CALLBACK_PARAM_STRLEN + 1, 'Y');
    EXPECT_THROW(m.set_shutdown(dummy_callback, 1000ms, arg), std::length_error);
}

// ============================================================================
// set_as_persistent
// ============================================================================

TEST(ModuleDefTest, SetAsPersistent)
{
    ModuleDef m("TestModule");
    EXPECT_NO_THROW(m.set_as_persistent(true));
    EXPECT_NO_THROW(m.set_as_persistent(false));
}
