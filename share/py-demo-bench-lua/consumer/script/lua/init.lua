-- Engine-throughput bench: Lua consumer.

local ffi = require("ffi")
ffi.cdef[[
    typedef long time_t;
    typedef struct timespec { time_t tv_sec; long tv_nsec; } timespec;
    int clock_gettime(int clk_id, struct timespec *tp);
]]
local CLOCK_MONOTONIC = 1
local function now_s()
    local ts = ffi.new("timespec")
    ffi.C.clock_gettime(CLOCK_MONOTONIC, ts)
    return tonumber(ts.tv_sec) + tonumber(ts.tv_nsec) * 1e-9
end

local ENGINE        = "lua"
local SHUTDOWN_BAND = "!bench.shutdown"
local WARMUP_S      = 5.0

local received = 0
local t0 = 0.0
local warmup_received = -1
local warmup_t = 0.0
local band_joined = false

function on_init(api)
    t0 = now_s()
    api.log("info", "BenchCons-Lua started")
end

function on_consume(rx, msgs, api)
    if not band_joined then
        local res = api.band_join(SHUTDOWN_BAND)
        band_joined = true
        if res and res.status == "success" then
            api.log("info", "BenchCons joined band " .. SHUTDOWN_BAND)
        end
    end
    if rx.slot == nil then return true end
    received = received + 1
    if warmup_received < 0 and (received % 1024) == 0 then
        local elapsed = now_s() - t0
        if elapsed >= WARMUP_S then
            warmup_received = received
            warmup_t = now_s()
        end
    end
    return true
end

function on_band_message(band, sender, body, api)
    if band == SHUTDOWN_BAND and type(body) == "table" and body.cmd == "drain" then
        api.log("info", "BenchCons received drain from " .. tostring(sender) .. " — stopping")
        api.stop()
    end
end

function on_stop(api)
    local t_end = now_s()
    local elapsed = math.max(t_end - t0, 1e-9)
    local m = api.metrics()
    local loop = (m and m.loop) or {}
    local role = (m and m.role) or {}
    local iters = loop.iteration_count or 0
    local steady_slots = (warmup_received >= 0) and (received - warmup_received) or 0
    local steady_s     = (warmup_received >= 0) and (t_end - warmup_t)            or 0.0
    local steady_rate  = (steady_slots > 0) and (steady_slots / math.max(steady_s, 1e-9)) or 0.0
    api.log("info", string.format(
        "BENCH-CONS engine=%s total=%d elapsed_s=%.3f avg_rate=%.0f "
        .. "steady_slots=%d steady_s=%.3f steady_rate=%.0f "
        .. "iters=%d iters_per_s=%.0f "
        .. "in_received=%d work_us_last=%d",
        ENGINE, received, elapsed, received / elapsed,
        steady_slots, steady_s, steady_rate,
        iters, iters / elapsed,
        role.in_slots_received or 0,
        loop.last_cycle_work_us or 0))
end
