-- Engine-throughput bench: Lua producer.  Mirror of the Python bench.
-- Band-coordinated shutdown over `!bench.shutdown`.

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
local MEASUREMENT_S = 40.0

local count = 0
local t0 = 0.0
local warmup_count = -1
local warmup_t = 0.0
local band_joined = false
local drain_sent = false

function on_init(api)
    t0 = now_s()
    api.log("info", "BenchProd-Lua started warmup_s=" .. WARMUP_S .. " measurement_s=" .. MEASUREMENT_S)
end

function on_produce(tx, msgs, api)
    if not band_joined then
        local res = api.band_join(SHUTDOWN_BAND)
        band_joined = true
        if res and res.status == "success" then
            api.log("info", "BenchProd joined band " .. SHUTDOWN_BAND)
        end
    end
    if tx.slot == nil then return false end
    count = count + 1
    tx.slot.count = count
    tx.slot.value = count * 0.5
    -- Fresh random numbers each slot (1020 float32 = 4 KB payload).
    -- LuaJIT FFI direct indexed assign — JIT-compiled tight loop.
    local payload = tx.slot.payload
    for i = 0, 1019 do
        payload[i] = math.random()
    end
    if (count % 1024) == 0 then
        local elapsed = now_s() - t0
        if warmup_count < 0 and elapsed >= WARMUP_S then
            warmup_count = count
            warmup_t = now_s()
        end
        if (not drain_sent) and elapsed >= MEASUREMENT_S then
            drain_sent = true
            api.log("info", "BenchProd reached MEASUREMENT_S — broadcasting drain")
            api.band_broadcast(SHUTDOWN_BAND, {cmd = "drain"})
            api.stop()
        end
    end
    return true
end

function on_stop(api)
    local t_end = now_s()
    local elapsed = math.max(t_end - t0, 1e-9)
    local m = api.metrics()
    local loop = (m and m.loop) or {}
    local role = (m and m.role) or {}
    local iters = loop.iteration_count or 0
    local steady_slots = (warmup_count >= 0) and (count - warmup_count) or 0
    local steady_s     = (warmup_count >= 0) and (t_end - warmup_t)     or 0.0
    local steady_rate  = (steady_slots > 0) and (steady_slots / math.max(steady_s, 1e-9)) or 0.0
    api.log("info", string.format(
        "BENCH-PROD engine=%s total=%d elapsed_s=%.3f avg_rate=%.0f "
        .. "steady_slots=%d steady_s=%.3f steady_rate=%.0f "
        .. "iters=%d iters_per_s=%.0f "
        .. "written=%d drops=%d work_us_last=%d",
        ENGINE, count, elapsed, count / elapsed,
        steady_slots, steady_s, steady_rate,
        iters, iters / elapsed,
        role.out_slots_written or 0, role.out_drop_count or 0,
        loop.last_cycle_work_us or 0))
end
