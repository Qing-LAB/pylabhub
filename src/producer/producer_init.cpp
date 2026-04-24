/**
 * @file producer_init.cpp
 * @brief Producer role init-directory registration.
 */

#include "producer_init.hpp"
#include "producer_fields.hpp"
#include "producer_role_host.hpp"

#include "utils/role_directory.hpp"
#include "utils/role_registry.hpp"

#include <fmt/core.h>
#include <fmt/ostream.h>
#include <fstream>
#include <memory>
#include <nlohmann/json.hpp>

namespace pylabhub::producer
{

namespace
{

nlohmann::json producer_config_template(const std::string &uid,
                                         const std::string &name)
{
    nlohmann::json j;

    j["producer"]["uid"]       = uid;
    j["producer"]["name"]      = name;
    j["producer"]["log_level"] = "info";
    j["producer"]["auth"]["keyfile"] = "";

    j["out_hub_dir"]          = "<replace with hub directory path, e.g. /var/pylabhub/my_hub>";
    j["out_channel"]          = "lab.my.channel";
    j["loop_timing"]          = "fixed_rate";
    j["target_period_ms"]     = 100;

    j["out_transport"]        = "shm";
    j["out_shm_enabled"]      = true;
    j["out_shm_slot_count"]   = 8;
    // out_shm_secret omitted — 0 is the default ("no secret"); users who need
    // per-channel authentication add it explicitly. Consumer and processor
    // templates follow the same convention (shm_secret is advanced, not
    // boilerplate).

    j["out_slot_schema"]["fields"] = nlohmann::json::array({
        nlohmann::json{{"name", "value"}, {"type", "float32"}}
    });
    j["out_flexzone_schema"]  = nullptr;

    j["checksum"]             = "enforced";
    j["flexzone_checksum"]    = true;
    j["stop_on_script_error"] = false;

    j["script"]["path"] = ".";
    j["script"]["type"] = "python";

    return j;
}

void producer_on_init(const utils::RoleDirectory &rd, const std::string &name)
{
    // Python package layout:
    //   script/python/__init__.py    — package entry, re-exports callbacks
    //   script/python/callbacks.py   — user's callback implementations
    const auto init_py = rd.script_entry(".", "python");
    const auto pkg_dir = init_py.parent_path();
    const auto callbacks_py = pkg_dir / "callbacks.py";

    // __init__.py — stable entry point, should not need editing
    {
        std::ofstream out(init_py);
        if (!out)
            throw std::runtime_error(fmt::format("cannot write '{}'", init_py.string()));
        fmt::print(out,
            "\"\"\"Producer: {} — package entry point.\n"
            "\n"
            "Re-exports callbacks from callbacks.py. Edit callbacks.py to\n"
            "implement your data-production logic.\n"
            "\"\"\"\n"
            "from .callbacks import on_init, on_produce, on_stop\n"
            "\n"
            "__all__ = [\"on_init\", \"on_produce\", \"on_stop\"]\n",
            name);
    }

    // callbacks.py — user edits this file
    {
        std::ofstream out(callbacks_py);
        if (!out)
            throw std::runtime_error(fmt::format("cannot write '{}'", callbacks_py.string()));
        fmt::print(out,
            "\"\"\"Producer callbacks — edit this file.\"\"\"\n"
            "import pylabhub_producer as prod\n"
            "\n"
            "\n"
            "def on_init(api: prod.ProducerAPI) -> None:\n"
            "    \"\"\"Called once before the production loop starts.\"\"\"\n"
            "    api.log('info', f\"on_init: uid={{api.uid()}}\")\n"
            "\n"
            "\n"
            "def on_produce(tx, messages, api: prod.ProducerAPI) -> bool:\n"
            "    \"\"\"\n"
            "    Called on each production interval.\n"
            "\n"
            "    tx.slot:  ctypes/numpy writable view of the output SHM slot.\n"
            "    messages: list of (sender: str, data: bytes) from ZMQ consumers.\n"
            "    api:      ProducerAPI — log, broadcast, stop, metrics, etc.\n"
            "              api.flexzone(ChannelSide.Tx) for output flexzone.\n"
            "\n"
            "    Return True to commit the slot and publish it.\n"
            "    Return False to discard without publishing.\n"
            "    \"\"\"\n"
            "    # TODO: replace with real data production\n"
            "    tx.slot.value = 0.0\n"
            "    return True\n"
            "\n"
            "\n"
            "def on_stop(api: prod.ProducerAPI) -> None:\n"
            "    \"\"\"Called once after the production loop exits.\"\"\"\n"
            "    api.log('info', f\"on_stop: uid={{api.uid()}}\")\n");
    }
}

} // namespace

void register_producer_init()
{
    utils::RoleDirectory::register_role("producer")
        .config_filename("producer.json")
        .uid_prefix("prod")
        .role_label("Producer")
        .config_template(&producer_config_template)
        .on_init(&producer_on_init);
}

// ── Runtime registration (Phase 16 — plh_role dispatch target) ──────────────

namespace
{

// Free factory: constructs a ProducerRoleHost and upcasts to the abstract
// base for uniform plh_role dispatch. Function-pointer (not std::function)
// to satisfy RoleRegistry's ABI-stable signature.
std::unique_ptr<scripting::RoleHostBase> make_producer_host(
    config::RoleConfig config,
    std::unique_ptr<scripting::ScriptEngine> engine,
    std::atomic<bool> *shutdown_flag)
{
    return std::make_unique<ProducerRoleHost>(
        std::move(config), std::move(engine), shutdown_flag);
}

} // namespace

void register_producer_runtime()
{
    utils::RoleRegistry::register_runtime("producer")
        .role_label("Producer")
        .host_factory(&make_producer_host)
        .config_parser(&parse_producer_fields)
        .commit();
}

} // namespace pylabhub::producer
