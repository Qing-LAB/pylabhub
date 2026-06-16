/**
 * @file pattern4_helpers.cpp
 * @brief Implementation of Pattern 4 test-framework helpers.
 */
#include "pattern4_helpers.h"

#include "utils/logger.hpp"

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include <algorithm>
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

/// Sort a log snapshot by embedded timestamp so cross-process write
/// interleaving on the shared O_APPEND log cannot flip causal order.
///
/// Logger lines look like:
///   `[LOGGER] [INFO  ] [2026-06-15 06:33:01.181971] [PID:N TID:N] ...`
/// The bracketed timestamp is lex-sortable (zero-padded YYYY-MM-DD
/// HH:MM:SS.uuuuuu).  Stable sort keeps lines without a parseable
/// timestamp (e.g. continuation lines from multi-line emit, gtest
/// output) in their original relative order — they sort as if they
/// carried the timestamp of the most recent timestamped line.
std::string sort_log_by_timestamp(const std::string &snapshot)
{
    if (snapshot.empty())
        return snapshot;
    std::vector<std::pair<std::string, std::string>> keyed;  // (sort_key, line)
    keyed.reserve(snapshot.size() / 80 + 1);
    std::string last_ts;  // most recent observed timestamp; inherited by
                          // continuation lines without their own
    std::size_t pos = 0;
    while (pos < snapshot.size())
    {
        const auto eol = snapshot.find('\n', pos);
        const auto end = (eol == std::string::npos) ? snapshot.size() : eol + 1;
        std::string_view line{snapshot.data() + pos, end - pos};
        // Find the THIRD '[' (the timestamp bracket) on a LOGGER line.
        // LOGGER format prefix: `[LOGGER] [LEVEL ] [TIMESTAMP] [PID:...] ...`
        // Cheap structural check: starts with `[LOGGER] `.
        std::string ts;
        if (line.size() > 9 && line.substr(0, 9) == "[LOGGER] ")
        {
            // Skip "[LOGGER] [LEVEL] " — find the 3rd '['.
            std::size_t bracket_count = 0;
            std::size_t ts_start      = std::string::npos;
            for (std::size_t i = 0; i < line.size(); ++i)
            {
                if (line[i] == '[')
                {
                    if (++bracket_count == 3)
                    {
                        ts_start = i + 1;
                        break;
                    }
                }
            }
            if (ts_start != std::string::npos)
            {
                const auto ts_end = line.find(']', ts_start);
                if (ts_end != std::string::npos
                    && ts_end - ts_start >= 19)  // "YYYY-MM-DD HH:MM:SS" minimum
                {
                    ts = std::string{line.substr(ts_start, ts_end - ts_start)};
                    last_ts = ts;
                }
            }
        }
        // Continuation / non-LOGGER lines inherit the most recent
        // timestamp so they sort with their emitting LOGGER call.
        keyed.emplace_back(ts.empty() ? last_ts : ts,
                           std::string{line});
        pos = end;
    }
    std::stable_sort(keyed.begin(), keyed.end(),
        [](const auto &a, const auto &b) { return a.first < b.first; });
    std::string out;
    out.reserve(snapshot.size());
    for (auto &p : keyed)
        out.append(p.second);
    return out;
}

} // anon

void expect_log_sequence(
    const fs::path &shared_log,
    std::initializer_list<std::string_view> markers,
    std::chrono::milliseconds per_step_timeout)
{
    // Re-search from scratch each poll against the sorted snapshot.
    // Carrying a byte-offset cursor across polls is unsafe: a late-
    // arriving line with an earlier timestamp gets sorted before
    // already-matched content, shifting all subsequent bytes, and a
    // stale cursor either skips real matches or, more commonly,
    // produces a misleading "marker not found" when the marker IS in
    // the log but in a causal position that disagrees with the test's
    // expected order.  Re-search is O(N*M) per poll where N=marker
    // count (~8), M=log size (~10KB) — microseconds, negligible.
    //
    // Per-step deadline: each step gets its own per_step_timeout
    // window, measured from when the PREVIOUS step first matched.
    // Tracked by max_matched (highest step count seen across polls);
    // when it advances, the deadline resets.
    std::size_t max_matched = 0;
    auto step_deadline =
        std::chrono::steady_clock::now() + per_step_timeout;
    std::size_t cached_raw_size = 0;
    std::string sorted_snapshot;

    while (std::chrono::steady_clock::now() < step_deadline)
    {
        const std::string raw = read_shared_log_snapshot(shared_log);
        if (raw.size() != cached_raw_size)
        {
            sorted_snapshot = sort_log_by_timestamp(raw);
            cached_raw_size = raw.size();
        }

        std::size_t cursor  = 0;
        std::size_t matched = 0;
        for (auto marker : markers)
        {
            const auto pos = sorted_snapshot.find(marker, cursor);
            if (pos == std::string::npos) break;
            cursor = pos + marker.size();
            ++matched;
        }

        if (matched == markers.size())
            return;  // all markers found in causal order

        if (matched > max_matched)
        {
            max_matched   = matched;
            step_deadline =
                std::chrono::steady_clock::now() + per_step_timeout;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds{25});
    }

    // Failure: the marker at index max_matched is missing or out of
    // causal order.
    auto it = markers.begin();
    std::advance(it, max_matched);
    ADD_FAILURE()
        << "expect_log_sequence: step " << max_matched + 1
        << " of " << markers.size()
        << " — marker '" << *it
        << "' not found within " << per_step_timeout.count()
        << "ms after step " << max_matched << " matched (sorted snapshot "
        << sorted_snapshot.size() << " bytes total).\n"
        << "Note: 'not found' may also mean the marker IS in the log "
           "but with an EARLIER timestamp than a previously-matched "
           "step — i.e., a causal-sequence violation.  Inspect the "
           "tail for both occurrences and their timestamps.\n"
        << "Tail of shared log (last 20 lines):\n"
        << tail_lines(sorted_snapshot, 20);
}

} // namespace pylabhub::tests::pattern4
