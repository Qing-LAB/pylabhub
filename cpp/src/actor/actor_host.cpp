/**
 * @file actor_host.cpp
 * @brief ActorHost implementation — top-level orchestrator for all role workers.
 *
 * ActorHost owns the collection of ProducerRoleWorker and ConsumerRoleWorker
 * instances. It manages script loading (importlib module import), role setup,
 * startup sequencing, and graceful shutdown.
 *
 * Worker implementations live in actor_role_workers.cpp.
 * Shared helper utilities live in actor_worker_helpers.hpp.
 */
#include "actor_worker_helpers.hpp"

namespace pylabhub::actor
{


// ============================================================================
// ActorHost
// ============================================================================

ActorHost::ActorHost(const ActorConfig &config)
    : config_(config)
{
}

ActorHost::~ActorHost()
{
    stop();
}

bool ActorHost::load_script(bool verbose)
{
    script_loaded_ = false;
    role_modules_.clear();

    if (config_.roles.empty())
    {
        LOGGER_WARN("[actor] No roles configured — nothing to load");
        return false;
    }

    // Derive a short hex suffix from actor_uid for module aliasing.
    const std::string uid_hex = config_.actor_uid.size() >= 8
        ? config_.actor_uid.substr(config_.actor_uid.size() - 8)
        : config_.actor_uid;

    py::gil_scoped_acquire g;

    // ── Step 1: load actor-level module (fallback for roles without per-role script) ──
    py::module_ actor_module{};
    bool        actor_module_valid = false;

    if (!config_.script_module.empty())
    {
        try
        {
            actor_module       = import_script_module(
                config_.script_module, config_.script_base_dir, uid_hex);
            actor_module_valid = true;
            LOGGER_INFO("[actor] Actor-level script '{}' loaded", config_.script_module);
        }
        catch (py::error_already_set &e)
        {
            LOGGER_ERROR("[actor] Actor-level script '{}' load error: {}",
                         config_.script_module, e.what());
            if (verbose)
                std::cerr << "Script error: " << e.what() << "\n";
        }
        catch (const std::exception &e)
        {
            LOGGER_ERROR("[actor] Actor-level script '{}' load error: {}",
                         config_.script_module, e.what());
            if (verbose)
                std::cerr << "Error: " << e.what() << "\n";
        }
    }

    // ── Step 2: resolve per-role modules (per-role overrides actor-level fallback) ────
    bool any_module = false;

    for (const auto &[role_name, role_cfg] : config_.roles)
    {
        if (!role_cfg.script_module.empty())
        {
            // Per-role script: isolated load via spec_from_file_location.
            try
            {
                auto mod = import_role_script_module(
                    role_name, role_cfg.script_module,
                    role_cfg.script_base_dir, uid_hex);
                role_modules_[role_name] = mod;
                any_module = true;

                py::object on_iter = py::getattr(mod, "on_iteration", py::none());
                LOGGER_INFO("[actor/{}] Per-role script '{}' loaded{}",
                            role_name, role_cfg.script_module,
                            on_iter.is_none() ? " (WARNING: no on_iteration)" : "");
            }
            catch (py::error_already_set &e)
            {
                LOGGER_ERROR("[actor/{}] Script load error: {}", role_name, e.what());
                if (verbose)
                    std::cerr << "Script error [" << role_name << "]: " << e.what() << "\n";
            }
            catch (const std::exception &e)
            {
                LOGGER_ERROR("[actor/{}] Script load error: {}", role_name, e.what());
                if (verbose)
                    std::cerr << "Error [" << role_name << "]: " << e.what() << "\n";
            }
        }
        else if (actor_module_valid)
        {
            // Fallback: roles without a per-role script share the actor-level module.
            role_modules_[role_name] = actor_module;
            any_module = true;
        }
        else
        {
            LOGGER_WARN("[actor/{}] No script configured — role will be skipped", role_name);
        }
    }

    if (!any_module)
    {
        LOGGER_ERROR("[actor] No script modules loaded — "
                     "add \"script\": {{\"module\": \"script\", \"path\": \"./roles/<role>\"}} "
                     "to each role in actor.json");
        if (verbose)
            std::cerr << "Error: No script modules loaded.\n"
                         "Add a \"script\" key to each role (or a top-level \"script\" fallback).\n";
        return false;
    }

    if (verbose)
    {
        std::cout << "\nActor uid: "
                  << (config_.actor_uid.empty() ? "(auto)" : config_.actor_uid) << "\n";
        print_role_summary();
    }

    script_loaded_ = true;
    return true;
}

bool ActorHost::start()
{
    if (!script_loaded_)
        return false;

    bool any_started = false;

    // ── Start producer roles ──────────────────────────────────────────────────
    for (const auto &[role_name, role_cfg] : config_.roles)
    {
        if (role_cfg.kind != RoleConfig::Kind::Producer)
            continue;

        const auto mod_it = role_modules_.find(role_name);
        if (mod_it == role_modules_.end())
        {
            LOGGER_WARN("[actor/{}] No script module — skipping producer role", role_name);
            continue;
        }

        // script_dir shown in api.script_dir(): prefer per-role, fall back to actor-level.
        const std::string script_dir = role_cfg.script_base_dir.empty()
                                       ? config_.script_base_dir
                                       : role_cfg.script_base_dir;

        auto worker = std::make_unique<ProducerRoleWorker>(
            role_name, role_cfg, config_.actor_uid, config_.auth, shutdown_,
            mod_it->second);

        worker->set_env_actor_name(config_.actor_name);
        worker->set_env_log_level(config_.log_level);
        worker->set_env_script_dir(script_dir);

        if (!worker->start())
        {
            LOGGER_ERROR("[actor] Failed to start producer role '{}'", role_name);
            continue;
        }
        producers_[role_name] = std::move(worker);
        any_started = true;
    }

    // ── Start consumer roles ──────────────────────────────────────────────────
    for (const auto &[role_name, role_cfg] : config_.roles)
    {
        if (role_cfg.kind != RoleConfig::Kind::Consumer)
            continue;

        const auto mod_it = role_modules_.find(role_name);
        if (mod_it == role_modules_.end())
        {
            LOGGER_WARN("[actor/{}] No script module — skipping consumer role", role_name);
            continue;
        }

        const std::string script_dir = role_cfg.script_base_dir.empty()
                                       ? config_.script_base_dir
                                       : role_cfg.script_base_dir;

        auto worker = std::make_unique<ConsumerRoleWorker>(
            role_name, role_cfg, config_.actor_uid, config_.auth, shutdown_,
            mod_it->second);

        worker->set_env_actor_name(config_.actor_name);
        worker->set_env_log_level(config_.log_level);
        worker->set_env_script_dir(script_dir);

        if (!worker->start())
        {
            LOGGER_ERROR("[actor] Failed to start consumer role '{}'", role_name);
            continue;
        }
        consumers_[role_name] = std::move(worker);
        any_started = true;
    }

    // Always release the GIL so worker loop_threads can acquire it for Python
    // callbacks.  Also allows the AdminShell thread to run exec() without
    // competing with an idle main thread holding the GIL.
    // GIL ownership is restored by stop() via main_thread_release_.reset().
    main_thread_release_.emplace();

    return any_started;
}

void ActorHost::stop()
{
    // Re-acquire the GIL (was released by start()).
    // main_thread_release_ may be empty if start() was not called or returned
    // before the emplace; in that case this is a no-op.
    main_thread_release_.reset();

    // Release GIL during worker thread joins to prevent deadlock: loop_threads
    // may be mid-on_iteration (inside py::gil_scoped_acquire).  Releasing here
    // allows them to finish and exit.  Each worker's call_on_stop() re-acquires
    // the GIL internally via its own py::gil_scoped_acquire — that works
    // correctly while the GIL is released here in the calling thread.
    {
        py::gil_scoped_release release;
        for (auto &[name, worker] : producers_)
            worker->stop();
        producers_.clear();

        for (auto &[name, worker] : consumers_)
            worker->stop();
        consumers_.clear();
    }
    // GIL re-acquired when release goes out of scope.
}

bool ActorHost::is_running() const noexcept
{
    for (const auto &[_, w] : producers_)
        if (w->is_running()) return true;
    for (const auto &[_, w] : consumers_)
        if (w->is_running()) return true;
    return false;
}

void ActorHost::wait_for_shutdown()
{
    while (!shutdown_.load(std::memory_order_relaxed))
        std::this_thread::sleep_for(
            std::chrono::milliseconds(pylabhub::kAdminPollIntervalMs));
}

void ActorHost::print_role_summary() const
{
    std::cout << "\nConfigured roles:\n";
    for (const auto &[name, cfg] : config_.roles)
    {
        const char *kind_str = (cfg.kind == RoleConfig::Kind::Producer)
            ? "producer" : "consumer";

        const auto mod_it = role_modules_.find(name);
        const bool has_module = (mod_it != role_modules_.end());

        bool has_iteration = false;
        if (has_module && script_loaded_)
        {
            py::gil_scoped_acquire g;
            has_iteration =
                !py::getattr(mod_it->second, "on_iteration", py::none()).is_none();
        }

        const char *status = has_iteration   ? "ACTIVATED"
                           : has_module      ? "no on_iteration"
                           :                   "no script";

        std::cout << "  " << name << "  [" << kind_str << "]"
                  << "  channel=" << cfg.channel
                  << "  " << status << "\n";
    }
}

} // namespace pylabhub::actor
