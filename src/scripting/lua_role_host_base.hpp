#pragma once
/**
 * @file lua_role_host_base.hpp
 * @brief LuaRoleHostBase — placeholder for future Lua scripting engine support.
 *
 * When implemented, this class will compose RoleHostCore (the same engine-agnostic
 * infrastructure used by PythonRoleHostBase) and provide Lua-specific equivalents
 * for:
 *  - lua_State management and GIL equivalent (Lua has no GIL, but coroutine mgmt)
 *  - Lua slot bindings (ctypes equivalent via ffi or lightuserdata)
 *  - Lua role subclasses (LuaProcessorHost, LuaProducerHost, LuaConsumerHost)
 *
 * The virtual hook contract matches PythonRoleHostBase:
 *  - role_tag(), role_name(), role_uid()
 *  - build_role_types(), start_role(), stop_role()
 *  - etc.
 *
 * See HEP-CORE-0011 for the ScriptHost abstraction framework.
 */

#include "role_host_core.hpp"

namespace pylabhub::scripting
{

// Placeholder — no implementation yet.
// When Lua support is added:
//   1. Create LuaScriptHost : ScriptHost (equivalent of PythonScriptHost)
//   2. Create LuaRoleHostBase : LuaScriptHost (equivalent of PythonRoleHostBase)
//   3. LuaRoleHostBase composes RoleHostCore (same as Python — zero duplication)
//   4. Role subclasses override the same virtual hooks

} // namespace pylabhub::scripting
