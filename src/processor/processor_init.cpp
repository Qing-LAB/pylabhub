/**
 * @file processor_init.cpp
 * @brief Processor role init-directory registration.
 */

#include "processor_init.hpp"
#include "processor_fields.hpp"
#include "processor_role_host.hpp"

#include "utils/role_directory.hpp"
#include "utils/role_registry.hpp"

#include <fmt/core.h>
#include <fmt/ostream.h>
#include <fstream>
#include <memory>
#include <nlohmann/json.hpp>

namespace pylabhub::processor
{

namespace
{

nlohmann::json processor_config_template(const std::string &uid,
                                           const std::string &name)
{
    nlohmann::json j;

    j["processor"]["uid"]       = uid;
    j["processor"]["name"]      = name;
    j["processor"]["log_level"] = "info";
    j["processor"]["auth"]["keyfile"] = "";

    j["loop_timing"]  = "max_rate";
    j["in_hub_dir"]   = "<replace with input hub directory path>";
    j["out_hub_dir"]  = "<replace with output hub directory path>";
    j["in_channel"]   = "lab.source.channel";
    j["out_channel"]  = "lab.output.channel";

    j["in_transport"]       = "shm";
    j["out_transport"]      = "shm";
    j["in_shm_enabled"]     = true;
    j["out_shm_enabled"]    = true;
    j["out_shm_slot_count"] = 4;

    j["in_slot_schema"]["fields"] = nlohmann::json::array({
        nlohmann::json{{"name", "value"}, {"type", "float32"}}
    });
    j["out_slot_schema"]["fields"] = nlohmann::json::array({
        nlohmann::json{{"name", "value"}, {"type", "float32"}}
    });
    j["out_flexzone_schema"] = nullptr;

    j["checksum"]             = "enforced";
    j["flexzone_checksum"]    = true;
    j["stop_on_script_error"] = false;

    j["script"]["path"] = ".";
    j["script"]["type"] = "python";

    return j;
}

void processor_on_init(const utils::RoleDirectory &rd, const std::string &name)
{
    // Python package layout:
    //   script/python/__init__.py    — package entry, re-exports callbacks
    //   script/python/callbacks.py   — user's callback implementations
    const auto init_py = rd.script_entry(".", "python");
    const auto pkg_dir = init_py.parent_path();
    const auto callbacks_py = pkg_dir / "callbacks.py";

    // __init__.py
    {
        std::ofstream out(init_py);
        if (!out)
            throw std::runtime_error(fmt::format("cannot write '{}'", init_py.string()));
        fmt::print(out,
            "\"\"\"Processor: {} — package entry point.\n"
            "\n"
            "Re-exports callbacks from callbacks.py. Edit callbacks.py to\n"
            "implement your data-transformation logic.\n"
            "\"\"\"\n"
            "from .callbacks import on_init, on_process, on_stop\n"
            "\n"
            "__all__ = [\"on_init\", \"on_process\", \"on_stop\"]\n",
            name);
    }

    // callbacks.py
    {
        std::ofstream out(callbacks_py);
        if (!out)
            throw std::runtime_error(fmt::format("cannot write '{}'", callbacks_py.string()));
        fmt::print(out,
            "\"\"\"Processor callbacks — edit this file.\"\"\"\n"
            "import pylabhub_processor as proc\n"
            "\n"
            "\n"
            "def on_init(api: proc.ProcessorAPI) -> None:\n"
            "    \"\"\"Called once before the processing loop starts.\"\"\"\n"
            "    api.log('info', f\"on_init: uid={{api.uid()}}\")\n"
            "\n"
            "\n"
            "def on_process(rx, tx, messages, api: proc.ProcessorAPI) -> bool:\n"
            "    \"\"\"\n"
            "    Called for each input slot.\n"
            "\n"
            "    rx.slot:  ctypes/numpy read-only view of the input SHM slot.\n"
            "    tx.slot:  ctypes/numpy writable view of the output SHM slot.\n"
            "    messages: list of (sender: str, data: bytes) from ZMQ.\n"
            "    api:      ProcessorAPI — log, broadcast, stop, metrics, etc.\n"
            "              api.flexzone(side) for input/output flexzone.\n"
            "\n"
            "    Return True to commit the output slot.\n"
            "    Return False to discard without publishing.\n"
            "    \"\"\"\n"
            "    # TODO: replace with real transformation\n"
            "    tx.slot.value = rx.slot.value\n"
            "    return True\n"
            "\n"
            "\n"
            "def on_stop(api: proc.ProcessorAPI) -> None:\n"
            "    \"\"\"Called once after the processing loop exits.\"\"\"\n"
            "    api.log('info', f\"on_stop: uid={{api.uid()}}\")\n");
    }
}

} // namespace

void register_processor_init()
{
    utils::RoleDirectory::register_role("processor")
        .config_filename("processor.json")
        .uid_prefix("proc")
        .role_label("Processor")
        .config_template(&processor_config_template)
        .on_init(&processor_on_init);
}

// ── Runtime registration (Phase 16 — plh_role dispatch target) ──────────────

namespace
{

std::unique_ptr<scripting::RoleHostBase> make_processor_host(
    config::RoleConfig config,
    std::unique_ptr<scripting::ScriptEngine> engine,
    std::atomic<bool> *shutdown_flag)
{
    return std::make_unique<ProcessorRoleHost>(
        std::move(config), std::move(engine), shutdown_flag);
}

} // namespace

void register_processor_runtime()
{
    utils::RoleRegistry::register_runtime("processor")
        .role_label("Processor")
        .host_factory(&make_processor_host)
        .config_parser(&parse_processor_fields)
        .commit();
}

} // namespace pylabhub::processor
