# ------------------------------------------------------------------------------
# src/CMakeLists.txt
#
# Purpose:
#   Configures all pyLabHub executables and orchestrates the inclusion of
#   core subdirectories. It establishes build properties, links dependencies,
#   and defines installation and staging rules for all application components.
#
# Responsibilities:
#   - Defines the `plh_role` executable (unified data-plane role binary).
#   - Includes the `utils`, `scripting` subdirectories.
#   - Applies common build settings (C++ standard, Clang-Tidy, sanitizers).
#   - Defines installation rules for all executables.
#   - Configures staging for all executables.
#
# Expected Parent Behavior (from top-level CMakeLists.txt):
#   - Should be included via `add_subdirectory(src)`.
#   - Assumes `PYLABHUB_LOGGER_COMPILE_LEVEL` and `PYLABHUB_LOGGER_DEBUG`
#     are defined.
#   - Assumes `pylabhub_enable_clang_tidy_for_target` and
#     `pylabhub_apply_sanitizer_to_target` functions are available.
#
# Data-plane binary:
#   - `plh_role` — unified role host (src/plh_role/) dispatching on --role
#     tag to producer / consumer / processor code.  Role-specific sources
#     under src/{producer,consumer,processor}/ are compiled into
#     pylabhub-utils + pylabhub-scripting (see those subdirs' CMakeLists).
# ------------------------------------------------------------------------------
cmake_minimum_required(VERSION 3.28)

# Add the utils subdirectory, which defines the pylabhub::utils library.
# This must be done before defining any targets that depend on it.
add_subdirectory(utils)

# PythonScriptHost static lib (thin CPython embedding layer; keeps libpython
# out of pure-C++ consumers of pylabhub-utils).
add_subdirectory(scripting)

# Unified role binary (HEP-0024 Phase 19). Dispatches on --role tag via
# a static registration map. The role-specific .cpp files under
# src/{producer,consumer,processor}/ are compiled into pylabhub-utils
# (role_host, init) and pylabhub-scripting (api) — no per-role executables.
add_subdirectory(plh_role)

# The previous `pylabhub-hubshell` executable + `src/hub_python/` legacy
# stack (PythonScriptHost / pylabhub_module / admin_shell / HubScript)
# were deleted with the HEP-CORE-0033 G2 cleanup pass.  The replacement
# `plh_hub` binary lands as part of HEP-CORE-0033 Phase 9 — see
# `docs/HEP/HEP-CORE-0033-Hub-Character.md` §15 (Implementation phases).
