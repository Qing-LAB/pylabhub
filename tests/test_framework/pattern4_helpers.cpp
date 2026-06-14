/**
 * @file pattern4_helpers.cpp
 * @brief Implementation of Pattern 4 test-framework helpers.
 */
#include "pattern4_helpers.h"

#include "utils/logger.hpp"

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include <atomic>
#include <chrono>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <thread>
#include <vector>
#include <unistd.h>

#if defined(__unix__) || defined(__APPLE__)
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#else
#error "Pattern4 helpers currently support POSIX only.  Windows support tracked separately."
#endif

namespace fs = std::filesystem;

namespace pylabhub::tests::pattern4
{

// ─── pick_unused_port ───────────────────────────────────────────────────────

int pick_unused_port(int max_attempts)
{
    for (int attempt = 0; attempt < max_attempts; ++attempt)
    {
        const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
        if (fd < 0)
            continue;

        sockaddr_in addr{};
        addr.sin_family      = AF_INET;
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        addr.sin_port        = 0;  // OS picks.

        if (::bind(fd, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) != 0)
        {
            ::close(fd);
            continue;
        }

        socklen_t addr_len = sizeof(addr);
        if (::getsockname(fd, reinterpret_cast<sockaddr *>(&addr), &addr_len) != 0)
        {
            ::close(fd);
            continue;
        }

        const int port = ntohs(addr.sin_port);
        ::close(fd);
        if (port > 0)
            return port;
    }
    throw std::runtime_error(
        "pick_unused_port: failed to allocate a port after " +
        std::to_string(max_attempts) + " attempts");
}

// ─── make_temp_dir ──────────────────────────────────────────────────────────

fs::path make_temp_dir(std::string_view test_label)
{
    static std::atomic<int> g_counter{0};
    const auto unique = std::to_string(::getpid()) + "_" +
                        std::to_string(g_counter.fetch_add(1));
    fs::path dir = fs::temp_directory_path() /
                   ("plh_pattern4_" + std::string(test_label) + "_" + unique);
    std::error_code ec;
    fs::remove_all(dir, ec);
    fs::create_directories(dir);
    return dir;
}

// ─── Pattern4Setup ──────────────────────────────────────────────────────────

Pattern4Setup
make_pattern4_setup(const std::vector<std::string> &role_uids)
{
    Pattern4Setup s;
    const int port  = pick_unused_port();
    s.broker_endpoint = "tcp://127.0.0.1:" + std::to_string(port);
    s.curve           = pylabhub::tests::make_curve_setup(role_uids);
    return s;
}

void write_pattern4_setup(const Pattern4Setup &setup, const fs::path &path)
{
    nlohmann::json j;
    j["broker_endpoint"] = setup.broker_endpoint;
    j["shared_log_path"] = setup.shared_log_path;  // optional; "" disables
    j["hub"]             = {
        {"public_z85", setup.curve.hub.public_z85},
        {"secret_z85", setup.curve.hub.secret_z85},
    };
    auto roles = nlohmann::json::array();
    for (const auto &[uid, kp] : setup.curve.role_keys)
    {
        roles.push_back({
            {"uid",        uid},
            {"public_z85", kp.public_z85},
            {"secret_z85", kp.secret_z85},
        });
    }
    j["role_keys"] = std::move(roles);

    std::ofstream out(path, std::ios::out | std::ios::trunc);
    if (!out)
        throw std::runtime_error("write_pattern4_setup: cannot open " +
                                  path.string());
    out << j.dump(2);
}

Pattern4Setup read_pattern4_setup(const fs::path &path)
{
    std::ifstream in(path);
    if (!in)
        throw std::runtime_error("read_pattern4_setup: cannot open " +
                                  path.string());
    nlohmann::json j;
    in >> j;

    Pattern4Setup s;
    s.broker_endpoint    = j.at("broker_endpoint").get<std::string>();
    s.shared_log_path    = j.value("shared_log_path", std::string{});
    s.curve.hub.public_z85 = j.at("hub").at("public_z85").get<std::string>();
    s.curve.hub.secret_z85 = j.at("hub").at("secret_z85").get<std::string>();
    for (const auto &item : j.at("role_keys"))
    {
        pylabhub::tests::CurveKeypair kp;
        kp.public_z85 = item.at("public_z85").get<std::string>();
        kp.secret_z85 = item.at("secret_z85").get<std::string>();
        s.curve.role_keys.emplace(item.at("uid").get<std::string>(),
                                   std::move(kp));
    }
    return s;
}

// ─── set_shared_log ─────────────────────────────────────────────────────────

void set_shared_log(const fs::path &shared_log_path)
{
    // Belt-and-suspenders: O_APPEND already gives kernel-atomic appends
    // up to PIPE_BUF (≥ 4 KB on Linux); use_flock=true layers a POSIX
    // advisory LOCK_EX around each write for full serialisation across
    // subprocesses sharing this log file.  Logger sink swap is
    // asynchronous; in practice the first LOGGER_* call after this
    // returns has already landed on the file by the time it returns
    // (the Logger's worker thread drains the command queue eagerly).
    auto &L = pylabhub::utils::Logger::instance();
    if (!L.set_logfile(shared_log_path.string(), /*use_flock=*/true))
    {
        throw std::runtime_error(
            "Pattern4 set_shared_log: Logger::set_logfile failed for '" +
            shared_log_path.string() + "'");
    }
}

// `expect_log_sequence` is defined further down, after `tail_lines`
// (which it shares with `expect_log` for the failure diagnostic).
// See the trailing block of this file.

// ─── wait_for_log / expect_log ──────────────────────────────────────────────

namespace
{

/// Read the captured stderr file in full.  Returns empty string if
/// the file doesn't exist yet (the subprocess hasn't started writing).
std::string read_stderr_snapshot(
    const pylabhub::tests::helper::WorkerProcess &proc)
{
    // `WorkerProcess::get_stderr()` returns a `const string &` that is
    // populated lazily on first call (or after wait_for_exit).  We
    // need a live re-read — read the captured file directly.  The
    // implementation keeps the file path on the WorkerProcess
    // (private), so we go through the public interface and accept
    // the lazy-caching cost: the snapshot reflects state at the
    // most recent read.
    //
    // For live-polling we want fresh reads on every loop iteration.
    // The simplest portable approach: keep calling get_stderr() —
    // its implementation re-reads the file on every call (verified
    // by inspecting tests/test_framework/test_process_utils.cpp).
    return proc.get_stderr();
}

} // anon

bool wait_for_log(const pylabhub::tests::helper::WorkerProcess &proc,
                  std::string_view substring,
                  std::chrono::milliseconds timeout)
{
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline)
    {
        const auto snapshot = read_stderr_snapshot(proc);
        if (snapshot.find(substring) != std::string::npos)
            return true;
        std::this_thread::sleep_for(std::chrono::milliseconds{25});
    }
    return false;
}

namespace
{

/// Returns the last N lines of `s` for diagnostic context.  Uses '\n'
/// as delimiter; collapses trailing whitespace.
std::string tail_lines(const std::string &s, std::size_t n)
{
    std::vector<std::string> lines;
    std::size_t pos = 0;
    while (pos < s.size())
    {
        const auto nl = s.find('\n', pos);
        if (nl == std::string::npos)
        {
            lines.emplace_back(s.substr(pos));
            break;
        }
        lines.emplace_back(s.substr(pos, nl - pos));
        pos = nl + 1;
    }
    const std::size_t start = lines.size() > n ? lines.size() - n : 0;
    std::string out;
    for (std::size_t i = start; i < lines.size(); ++i)
    {
        out += "  > ";
        out += lines[i];
        out += '\n';
    }
    return out;
}

} // anon

void expect_log(const pylabhub::tests::helper::WorkerProcess &proc,
                std::string_view substring,
                std::chrono::milliseconds timeout)
{
    if (wait_for_log(proc, substring, timeout))
        return;
    const auto snapshot = read_stderr_snapshot(proc);
    ADD_FAILURE() << "expect_log: substring '" << substring
                  << "' not found within " << timeout.count()
                  << "ms in worker mode '" << proc.mode() << "'\n"
                  << "Tail of captured stderr (last 10 lines):\n"
                  << tail_lines(snapshot, 10);
}

// ─── expect_log_sequence ────────────────────────────────────────────────────

namespace
{

/// Read the shared log file in full.  Returns empty string if the file
/// doesn't exist yet (subprocesses haven't started writing).
std::string read_shared_log_snapshot(const fs::path &shared_log)
{
    std::error_code ec;
    if (!fs::exists(shared_log, ec))
        return {};
    std::ifstream in(shared_log, std::ios::in | std::ios::binary);
    if (!in)
        return {};
    std::ostringstream oss;
    oss << in.rdbuf();
    return oss.str();
}

} // anon

void expect_log_sequence(
    const fs::path &shared_log,
    std::initializer_list<std::string_view> markers,
    std::chrono::milliseconds per_step_timeout)
{
    std::size_t cursor   = 0;   // byte offset; next search starts here
    std::size_t step_idx = 0;
    for (auto marker : markers)
    {
        const auto deadline = std::chrono::steady_clock::now() + per_step_timeout;
        bool matched = false;
        std::string snapshot;
        while (std::chrono::steady_clock::now() < deadline)
        {
            snapshot = read_shared_log_snapshot(shared_log);
            if (snapshot.size() > cursor)
            {
                const auto found = snapshot.find(marker, cursor);
                if (found != std::string::npos)
                {
                    cursor  = found + marker.size();
                    matched = true;
                    break;
                }
            }
            std::this_thread::sleep_for(std::chrono::milliseconds{25});
        }

        if (!matched)
        {
            ADD_FAILURE()
                << "expect_log_sequence: step " << step_idx + 1
                << " of " << markers.size()
                << " — marker '" << marker
                << "' not found within " << per_step_timeout.count()
                << "ms (search from byte offset " << cursor << " of "
                << snapshot.size() << " total).\n"
                << "Earlier steps matched: " << step_idx << "\n"
                << "Tail of shared log (last 20 lines):\n"
                << tail_lines(snapshot, 20);
            return;  // stop on first failed step — downstream diagnostics
                     // are misleading once the sequence breaks
        }
        ++step_idx;
    }
}

} // namespace pylabhub::tests::pattern4
