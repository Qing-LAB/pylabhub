#pragma once
/**
 * @file console_output_buffer.hpp
 * @brief The operator console's single pull output buffer
 *        (HEP-CORE-0033 §11.0.1 layer 6 + §11.0.4).
 *
 * One admin console connects at a time, so the hub keeps ONE buffer. The
 * hub appends output lines — the late result of an accepted command (tagged
 * with the command's `request_id`), a message the hub volunteers, or a line
 * a script emits via `admin_console_print` — and the operator drains them by
 * polling `RESPONSE_QUERY_REQ` (return-and-clear). The hub never pushes.
 *
 * LIFECYCLE. `on_connect()` starts a session clean (empty). `on_disconnect()`
 * flushes any un-drained lines to the log and clears. A healthy poll cadence
 * keeps the buffer near-empty (a poll removes what it returns), so the cap is
 * a SAFETY VALVE for the abnormal path only — a hung/absent console that
 * stops polling, or a script flooding `admin_console_print` faster than polls
 * drain it. The hub cannot assume the client keeps polling, so the cap must
 * exist; it just rarely triggers.
 *
 * THE LOG IS A SPILL/FLUSH SINK, NOT AN AUDIT TRAIL. It is written only when
 * a line is dropped on overflow (oldest-first, to stay under the cap) and
 * when a non-empty buffer is flushed at disconnect. Lines pulled by a normal
 * poll are never logged. Overflow is reported, never silent: every drain
 * carries `dropped_count` accrued since the previous drain.
 *
 * EVERY LINE'S `content` IS A JSON OBJECT (a plain message is
 * `{"message":"…"}`), so the response is structured at every level. A
 * non-object or an over-`max_line_bytes` content is replaced with a
 * `{"truncated":true,…}` marker so the line schema always holds.
 *
 * THREAD-SAFE. Appended by the broker thread (on actuation completion) and
 * the script worker (`admin_console_print`), drained by the admin worker, all
 * under one mutex. The clock and log sink are injectable so the buffer is
 * unit-testable without the real hub clock or logger (mirrors ReplayGuard).
 */

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <iterator>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

#include "utils/json_fwd.hpp"
#include "utils/logger.hpp"

namespace pylabhub::utils
{

class ConsoleOutputBuffer
{
  public:
    /// One buffered output line. `content` is always a JSON object.
    struct Line
    {
        std::uint64_t ts_ms{0};
        std::string request_id;   ///< empty for volunteered / script lines
        nlohmann::json content;   ///< always a JSON object
    };

    /// Caps — the safety-valve bounds. Whichever is hit first triggers a
    /// spill. `max_line_bytes` < `max_bytes`, so a per-line-truncated line
    /// always fits the whole-buffer budget.
    struct Caps
    {
        std::size_t max_lines{1000};
        std::size_t max_bytes{std::size_t{1024} * 1024};       ///< 1 MiB
        std::size_t max_line_bytes{std::size_t{64} * 1024};    ///< 64 KiB
    };

    /// Result of a poll: the buffered lines (oldest-first) plus the count of
    /// lines dropped to the log since the previous drain.
    struct Drain
    {
        std::vector<Line> lines;
        std::uint64_t dropped_count{0};
    };

    using ClockFn = std::function<std::uint64_t()>;                        ///< → wall ms
    using LogSinkFn = std::function<void(const Line &, const char *reason)>;

    ConsoleOutputBuffer() = default;

    explicit ConsoleOutputBuffer(Caps caps, ClockFn clock = {}, LogSinkFn log_sink = {})
        : caps_(caps), clock_(std::move(clock)), log_sink_(std::move(log_sink))
    {
    }

    /// Append one line. `request_id` is empty for hub/script-volunteered
    /// lines. A non-object or oversize `content` is replaced with a
    /// truncation marker. Overflow drops the oldest lines to the log sink and
    /// bumps the running `dropped_count`.
    void append(std::string request_id, nlohmann::json content)
    {
        Line line;
        line.request_id = std::move(request_id);
        line.content = std::move(content);

        std::lock_guard<std::mutex> lk(mu_);
        line.ts_ms = now_ms_();

        // Measure the content once; re-measure only on the rare truncation
        // paths (the marker is tiny), never twice on the common path.
        std::size_t content_sz = content_bytes_(line.content);
        if (!line.content.is_object())
        {
            line.content = nlohmann::json{{"truncated", true}, {"reason", "not_an_object"}};
            content_sz = content_bytes_(line.content);
        }
        else if (content_sz > caps_.max_line_bytes)
        {
            line.content = nlohmann::json{
                {"truncated", true}, {"reason", "line_too_large"}, {"bytes", content_sz}};
            content_sz = content_bytes_(line.content);
        }

        bytes_ += content_sz + line.request_id.size() + sizeof(std::uint64_t);
        lines_.push_back(std::move(line));
        enforce_caps_("overflow");
    }

    /// Return all buffered lines + the dropped count since the last drain,
    /// then clear (return-and-clear). Resets the running dropped count.
    Drain drain()
    {
        std::lock_guard<std::mutex> lk(mu_);
        Drain out;
        out.dropped_count = dropped_since_drain_;
        out.lines.assign(std::make_move_iterator(lines_.begin()),
                         std::make_move_iterator(lines_.end()));
        lines_.clear();
        bytes_ = 0;
        dropped_since_drain_ = 0;
        return out;
    }

    /// Console connect: reset to empty — a fresh session starts clean.
    void on_connect()
    {
        std::lock_guard<std::mutex> lk(mu_);
        lines_.clear();
        bytes_ = 0;
        dropped_since_drain_ = 0;
    }

    /// Console disconnect: flush any un-drained lines to the log, then clear.
    void on_disconnect()
    {
        std::lock_guard<std::mutex> lk(mu_);
        for (const auto &l : lines_)
            spill_to_log_(l, "disconnect_flush");
        lines_.clear();
        bytes_ = 0;
        dropped_since_drain_ = 0;
    }

    /// Current line count — for tests and diagnostics.
    [[nodiscard]] std::size_t size() const
    {
        std::lock_guard<std::mutex> lk(mu_);
        return lines_.size();
    }

  private:
    // caller holds mu_
    void enforce_caps_(const char *reason)
    {
        while (lines_.size() > caps_.max_lines || bytes_ > caps_.max_bytes)
        {
            if (lines_.empty())
                break;
            const Line &oldest = lines_.front();
            spill_to_log_(oldest, reason);
            bytes_ -= line_bytes_(oldest);
            lines_.pop_front();
            ++dropped_since_drain_;
        }
    }

    void spill_to_log_(const Line &l, const char *reason) const
    {
        if (log_sink_)
        {
            log_sink_(l, reason);
            return;
        }
        // Default sink: the hub log. The buffer is a spill/flush sink, so a
        // dropped line is not lost — it lands here. Tests inject their own sink.
        LOGGER_WARN("[admin_console] output line spilled ({}): request_id='{}' content={}", reason,
                    l.request_id, l.content.dump());
    }

    std::uint64_t now_ms_() const
    {
        if (clock_)
            return clock_();
        return static_cast<std::uint64_t>(
            std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::system_clock::now().time_since_epoch())
                .count());
    }

    static std::size_t content_bytes_(const nlohmann::json &content)
    {
        return content.dump().size();
    }

    static std::size_t line_bytes_(const Line &l)
    {
        return content_bytes_(l.content) + l.request_id.size() + sizeof(std::uint64_t);
    }

    mutable std::mutex mu_;
    std::deque<Line> lines_;
    std::size_t bytes_{0};
    std::uint64_t dropped_since_drain_{0};
    Caps caps_;
    ClockFn clock_;
    LogSinkFn log_sink_;
};

}  // namespace pylabhub::utils
