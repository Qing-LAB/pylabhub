#pragma once
/**
 * @file engine_factory_workers.h
 * @brief Workers for ScriptEngine factory dispatch tests
 *        (HEP-CORE-0011; Pattern 3).
 */

namespace pylabhub::tests::worker
{
namespace engine_factory
{

int native_returns_non_null();
int native_with_checksum_accepts();
int native_without_checksum_accepts();
int lua_returns_non_null();
int python_returns_non_null();
int python_with_venv_accepts();
int unknown_type_falls_through_to_python();
int empty_type_falls_through_to_python();

} // namespace engine_factory
} // namespace pylabhub::tests::worker
