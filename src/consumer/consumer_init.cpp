/**
 * @file consumer_init.cpp
 * @brief Consumer role init-directory registration.
 */

#include "consumer_init.hpp"

#include "utils/role_directory.hpp"

#include <fmt/core.h>
#include <fmt/ostream.h>
#include <fstream>
#include <nlohmann/json.hpp>

namespace pylabhub::consumer
{

namespace
{

nlohmann::json consumer_config_template(const std::string &uid,
                                         const std::string &name)
{
    nlohmann::json j;

    j["consumer"]["uid"]       = uid;
    j["consumer"]["name"]      = name;
    j["consumer"]["log_level"] = "info";
    j["consumer"]["auth"]["keyfile"] = "";

    j["loop_timing"]         = "max_rate";
    j["in_hub_dir"]          = "<replace with hub directory path, e.g. /var/pylabhub/my_hub>";
    j["in_channel"]          = "lab.my.channel";
    j["in_transport"]        = "shm";
    j["in_shm_enabled"]      = true;
    j["checksum"]            = "enforced";

    j["stop_on_script_error"] = false;

    j["script"]["path"] = ".";
    j["script"]["type"] = "python";

    return j;
}

void consumer_on_init(const utils::RoleDirectory &rd, const std::string &name)
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
            "\"\"\"Consumer: {} — package entry point.\n"
            "\n"
            "Re-exports callbacks from callbacks.py. Edit callbacks.py to\n"
            "implement your data-consumption logic.\n"
            "\"\"\"\n"
            "from .callbacks import on_init, on_consume, on_stop\n"
            "\n"
            "__all__ = [\"on_init\", \"on_consume\", \"on_stop\"]\n",
            name);
    }

    // callbacks.py
    {
        std::ofstream out(callbacks_py);
        if (!out)
            throw std::runtime_error(fmt::format("cannot write '{}'", callbacks_py.string()));
        fmt::print(out,
            "\"\"\"Consumer callbacks — edit this file.\"\"\"\n"
            "import pylabhub_consumer as cons\n"
            "\n"
            "\n"
            "def on_init(api: cons.ConsumerAPI) -> None:\n"
            "    \"\"\"Called once before the consumption loop starts.\"\"\"\n"
            "    api.log('info', f\"on_init: uid={{api.uid()}}\")\n"
            "\n"
            "\n"
            "def on_consume(rx, messages, api: cons.ConsumerAPI) -> bool:\n"
            "    \"\"\"\n"
            "    Called on each incoming slot.\n"
            "\n"
            "    rx.slot:  ctypes/numpy read-only view of the input SHM slot.\n"
            "    messages: list of bytes from ZMQ data channel.\n"
            "    api:      ConsumerAPI — log, stop, metrics, etc.\n"
            "              api.flexzone(ChannelSide.Rx) for input flexzone.\n"
            "\n"
            "    Return True to acknowledge the slot.\n"
            "    \"\"\"\n"
            "    # TODO: replace with real data-consumption logic\n"
            "    api.log('debug', f\"received slot #{{api.in_slots_received()}}\")\n"
            "    return True\n"
            "\n"
            "\n"
            "def on_stop(api: cons.ConsumerAPI) -> None:\n"
            "    \"\"\"Called once after the consumption loop exits.\"\"\"\n"
            "    api.log('info', f\"on_stop: uid={{api.uid()}}\")\n");
    }
}

} // namespace

void register_consumer_init()
{
    utils::RoleDirectory::register_role("consumer")
        .config_filename("consumer.json")
        .uid_prefix("CONS")
        .role_label("Consumer")
        .config_template(&consumer_config_template)
        .on_init(&consumer_on_init);
}

} // namespace pylabhub::consumer
