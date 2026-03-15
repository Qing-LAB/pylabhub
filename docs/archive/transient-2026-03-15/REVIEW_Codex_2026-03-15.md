
**Findings**

- High: role-level Lua support is documented and parsed, but the actual role runtime is still Python-only. [`ProducerScriptHost`](/home/qqing/Work/pylabhub/src/producer/producer_script_host.hpp#L42), [`ConsumerScriptHost`](/home/qqing/Work/pylabhub/src/consumer/consumer_script_host.hpp#L40), and [`ProcessorScriptHost`](/home/qqing/Work/pylabhub/src/processor/processor_script_host.hpp#L39) all inherit [`PythonRoleHostBase`](/home/qqing/Work/pylabhub/src/scripting/python_role_host_base.hpp), while the shared import helper hardcodes `__init__.py` regardless of `script_type` in [`script_host_helpers.hpp:398`](/home/qqing/Work/pylabhub/src/include/utils/script_host_helpers.hpp#L398). At the same time, config accepts `"lua"` in all three role configs ([`producer_config.hpp`](/home/qqing/Work/pylabhub/src/producer/producer_config.hpp#L204), [`consumer_config.hpp`](/home/qqing/Work/pylabhub/src/consumer/consumer_config.hpp#L172), [`processor_config.hpp`](/home/qqing/Work/pylabhub/src/processor/processor_config.hpp#L252)), and `RoleDirectory::create()` only creates `script/python` in [`role_directory.cpp:50`](/home/qqing/Work/pylabhub/src/utils/config/role_directory.cpp#L50). This is a real cross-module correctness and documentation inconsistency, not just an unfinished feature.

- High: the project claims broader platform support than it currently validates. CI is Linux-only in [`ci.yml`](/home/qqing/Work/pylabhub/.github/workflows/ci.yml#L19), wheel publishing is Linux-only in [`publish-pypi.yml`](/home/qqing/Work/pylabhub/.github/workflows/publish-pypi.yml#L39), but [`pyproject.toml`](/home/qqing/Work/pylabhub/pyproject.toml#L38) advertises macOS and Windows, and [`README_testing.md`](/home/qqing/Work/pylabhub/docs/README/README_testing.md#L596) says all tests must run on Windows, Linux, macOS, and FreeBSD. [`README_utils.md`](/home/qqing/Work/pylabhub/docs/README/README_utils.md#L406) also still says DataBlock-on-Windows is incomplete. The current validation story does not match the stated support surface.

- Medium: two lock RAII guards still have throwing destructors, which can turn ordinary error paths or racey shutdown into `std::terminate()`. See [`DataBlockLockGuard::~DataBlockLockGuard()`](/home/qqing/Work/pylabhub/src/utils/shm/data_block_mutex.cpp#L461) and [`SharedSpinLockGuard::~SharedSpinLockGuard()`](/home/qqing/Work/pylabhub/src/utils/shm/shared_memory_spinlock.cpp#L176). This is especially risky in code that already deals with crashed peers, abandoned locks, and cross-process recovery.

- Medium: role config parsing is heavily duplicated and already drifting in style and helper usage. The startup-wait parsing blocks are near-copies in [`producer_config.cpp:205`](/home/qqing/Work/pylabhub/src/producer/producer_config.cpp#L205), [`consumer_config.cpp:168`](/home/qqing/Work/pylabhub/src/consumer/consumer_config.cpp#L168), and [`processor_config.cpp:307`](/home/qqing/Work/pylabhub/src/processor/processor_config.cpp#L307). Transport/overflow parsing is also split, with processor carrying its own helper in [`processor_config.cpp:54`](/home/qqing/Work/pylabhub/src/processor/processor_config.cpp#L54) while other modules validate inline. This is a design/maintenance risk more than an immediate bug, but it is exactly how role behavior drifts over time.

- Medium: public API still exposes intentionally non-working operations and placeholder state in production headers. The remap APIs in [`data_block.hpp:705`](/home/qqing/Work/pylabhub/src/include/utils/data_block.hpp#L705), [`data_block.hpp:722`](/home/qqing/Work/pylabhub/src/include/utils/data_block.hpp#L722), [`data_block.hpp:995`](/home/qqing/Work/pylabhub/src/include/utils/data_block.hpp#L995), and [`data_block.hpp:1012`](/home/qqing/Work/pylabhub/src/include/utils/data_block.hpp#L1012) are public and always throw; [`recovery_api.hpp:58`](/home/qqing/Work/pylabhub/src/include/utils/recovery_api.hpp#L58) documents a field that is always zero. That is workable during development, but it is obsolete/redundant public surface if the goal is a stable runtime API.

- Low/Medium: documentation and status tracking are materially stale. [`TODO_MASTER.md`](/home/qqing/Work/pylabhub/docs/TODO_MASTER.md#L209) still says “884/884 passing”, while `ctest -N` now reports 1,166 tests. [`README_testing.md`](/home/qqing/Work/pylabhub/docs/README/README_testing.md#L105) and nearby sections still use older executable-level examples like `./test_layer2_filelock`, which no longer reflects the full current test layout as documented elsewhere. This weakens trust in the docs as an operational guide.

- Low: there is still dead or placeholder code that should either be wired up or removed. [`lua_role_host_base.hpp`](/home/qqing/Work/pylabhub/src/scripting/lua_role_host_base.hpp#L4) is an admitted placeholder with no implementation, and [`g_hub_config_initialized`](/home/qqing/Work/pylabhub/src/utils/config/hub_config.cpp#L42) is written in startup/shutdown but not consumed anywhere else in the codebase.

**Module Evaluation**

- Build/CMake/CI: strong structure overall. The top-level CMake is explicit, staged-install oriented, and avoids source globbing well; the custom CMake modules are organized and documented. The main weakness is that the support matrix described by CMake/docs/packaging is much wider than what CI and wheel publishing actually exercise.

- `src/utils/core`, `config`, `service`: generally solid and thoughtfully layered. Platform abstraction, lifecycle ordering, and vault/config separation are good foundations. Weak spots are stale internal state, a few public placeholders, and some lingering comment/code drift.

- `src/utils/shm`, `ipc`, `hub`: this is the most sophisticated and best-tested part of the codebase. Design intent is clear, and there is serious attention to race conditions and recovery. The biggest remaining risk is failure-mode behavior around locks and guard destructors, where exceptional paths can still become fatal.

- `src/{producer,consumer,processor}`: functional but repetitive. The three role stacks are close enough that shared parsing/validation helpers would pay off. Right now correctness is acceptable, but duplication is high and the scripting/runtime seams are inconsistent.

- `src/scripting` and script hosts: Python support looks substantially more mature than Lua. The abstraction direction is good, but the implementation reality is uneven: Lua exists in hub/utilities, while role-level Lua support is still partial and misleadingly exposed through config and directory conventions.

- Tests: broad and unusually deep on Linux runtime behavior, especially in shared memory, broker, and lifecycle areas. The biggest gaps are platform-matrix validation, wheel/install smoke coverage, and explicit end-to-end Lua role coverage.

- Docs: there is a lot of design material and many internal notes, which is valuable, but the repo has crossed the point where doc volume is creating drift. The canonical story is no longer obvious unless you already know which document to trust.

**Holistic Evaluation**

The project’s strongest area is the core runtime architecture: shared-memory/datahub, broker integration, lifecycle handling, and test depth all show deliberate engineering. The weakest area is consistency at the edges: platform claims versus actual validation, Python-versus-Lua scripting support, and duplicated role code/documentation that now evolves at different speeds.

Read-only review only; I did not edit anything. I excluded external-project code under `third_party/` and only considered the project-owned wrappers and integration points. I also did not run full cross-platform builds, so Windows/macOS/FreeBSD findings are based on source, packaging, and CI inspection rather than live execution.

**Findings**

- High: role-level Lua support is still not implemented, but several docs describe it as part of the completed role system. `HEP-CORE-0011` is careful that `LuaRoleHostBase` is still a stub, but `HEP-CORE-0018` and the role-directory docs describe Lua role layouts and role binaries as if they support Lua today. In code, all three role hosts are still Python-only, the import path is hardcoded to `__init__.py`, and the only Lua-related test I found is a path-extension unit test.
  [`HEP-CORE-0018-Producer-Consumer-Binaries.md#L107`](/home/qqing/Work/pylabhub/docs/HEP/HEP-CORE-0018-Producer-Consumer-Binaries.md#L107)
  [`HEP-CORE-0011-ScriptHost-Abstraction-Framework.md#L83`](/home/qqing/Work/pylabhub/docs/HEP/HEP-CORE-0011-ScriptHost-Abstraction-Framework.md#L83)
  [`lua_role_host_base.hpp#L1`](/home/qqing/Work/pylabhub/src/scripting/lua_role_host_base.hpp#L1)
  [`script_host_helpers.hpp#L386`](/home/qqing/Work/pylabhub/src/include/utils/script_host_helpers.hpp#L386)
  [`test_role_directory.cpp#L153`](/home/qqing/Work/pylabhub/tests/test_layer2_service/test_role_directory.cpp#L153)

- High: the role `--init` flow is documented incorrectly in multiple places. The docs say `--init` creates a vault and prompts for a password, but the actual binaries only create the directory, JSON template, and Python stub; they leave `auth.keyfile` empty and require a separate `--keygen` step later. That is a factual behavior mismatch, not a wording issue.
  [`README_DirectoryLayout.md#L319`](/home/qqing/Work/pylabhub/docs/README/README_DirectoryLayout.md#L319)
  [`HEP-CORE-0018-Producer-Consumer-Binaries.md#L663`](/home/qqing/Work/pylabhub/docs/HEP/HEP-CORE-0018-Producer-Consumer-Binaries.md#L663)
  [`producer_main.cpp#L75`](/home/qqing/Work/pylabhub/src/producer/producer_main.cpp#L75)
  [`consumer_main.cpp#L76`](/home/qqing/Work/pylabhub/src/consumer/consumer_main.cpp#L76)
  [`processor_main.cpp#L93`](/home/qqing/Work/pylabhub/src/processor/processor_main.cpp#L93)

- High: `README_DirectoryLayout` says role configs are self-contained and have no runtime `hub_dir` reference, but the runtime still resolves `hub_dir` from the role directory at startup and overwrites broker settings from the hub files. So the document currently describes a deployment model the code does not use.
  [`README_DirectoryLayout.md#L351`](/home/qqing/Work/pylabhub/docs/README/README_DirectoryLayout.md#L351)
  [`producer_config.cpp#L266`](/home/qqing/Work/pylabhub/src/producer/producer_config.cpp#L266)
  [`consumer_config.cpp#L222`](/home/qqing/Work/pylabhub/src/consumer/consumer_config.cpp#L222)
  [`processor_config.cpp#L369`](/home/qqing/Work/pylabhub/src/processor/processor_config.cpp#L369)

- Medium: `HEP-CORE-0024` does not match the shipped `RoleDirectory` API. The HEP documents `create(base, config_filename)`, says `open()` throws if the base is not an existing directory, and describes generic extension derivation; the actual public header exposes `create(base)` only, `open()` just weakly-canonicalizes, and `script_entry()` only distinguishes `"python"` vs everything else.
  [`HEP-CORE-0024-Role-Directory-Service.md#L155`](/home/qqing/Work/pylabhub/docs/HEP/HEP-CORE-0024-Role-Directory-Service.md#L155)
  [`role_directory.hpp#L50`](/home/qqing/Work/pylabhub/src/include/utils/role_directory.hpp#L50)
  [`role_directory.cpp#L33`](/home/qqing/Work/pylabhub/src/utils/config/role_directory.cpp#L33)
  [`role_directory.cpp#L69`](/home/qqing/Work/pylabhub/src/utils/config/role_directory.cpp#L69)

- Medium: `script.type` is exposed as a role feature, but the implementation is only partially wired and not validated. The config parsers accept any string, `RoleDirectory::script_entry()` maps any non-`python` type to `.lua`, and the Python role import helper still always loads `__init__.py`. That is both a correctness risk and evidence that the documented scripting abstraction is ahead of the implementation.
  [`producer_config.cpp#L239`](/home/qqing/Work/pylabhub/src/producer/producer_config.cpp#L239)
  [`consumer_config.cpp#L202`](/home/qqing/Work/pylabhub/src/consumer/consumer_config.cpp#L202)
  [`processor_config.cpp#L341`](/home/qqing/Work/pylabhub/src/processor/processor_config.cpp#L341)
  [`role_directory.cpp#L69`](/home/qqing/Work/pylabhub/src/utils/config/role_directory.cpp#L69)
  [`script_host_helpers.hpp#L398`](/home/qqing/Work/pylabhub/src/include/utils/script_host_helpers.hpp#L398)

- Medium: `HEP-CORE-0018` documents role CLI flags that do not exist in the shared parser, including `--dev` and `--version`. The actual parser only supports `--init`, `--name`, `--config`, `--validate`, `--keygen`, `--log-file`, `--run`, and `--help`.
  [`HEP-CORE-0018-Producer-Consumer-Binaries.md#L663`](/home/qqing/Work/pylabhub/docs/HEP/HEP-CORE-0018-Producer-Consumer-Binaries.md#L663)
  [`role_cli.hpp#L207`](/home/qqing/Work/pylabhub/src/include/utils/role_cli.hpp#L207)

- Low/Medium: the testing docs are stale relative to both the suite and CI. `README_testing` still says broker/message-plane Phase C is “To be implemented” and says all supported platforms must run the suite, but the repo already has broker/message tests and CI currently runs Linux only.
  [`README_testing.md#L596`](/home/qqing/Work/pylabhub/docs/README/README_testing.md#L596)
  [`ci.yml#L1`](/home/qqing/Work/pylabhub/.github/workflows/ci.yml#L1)
  [`test_datahub_broker.cpp`](/home/qqing/Work/pylabhub/tests/test_layer3_datahub/test_datahub_broker.cpp)
  [`test_datahub_messagehub.cpp`](/home/qqing/Work/pylabhub/tests/test_layer3_datahub/test_datahub_messagehub.cpp)

**Conclusion**

The docs under `docs/README` and `docs/HEP` are not fully consistent with the current codebase, and not all documented implementations are complete. The biggest gaps are around role Lua support, role-directory API/behavior, and the actual role bootstrap flow. I kept this review read-only and did not edit anything.