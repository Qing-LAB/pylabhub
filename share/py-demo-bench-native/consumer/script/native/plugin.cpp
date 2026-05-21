/* Engine-throughput bench: Native consumer. */
#include "utils/native_engine_api.h"
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <ctime>

PLH_DECLARE_SCHEMA(InSlotFrame,
    "count:int64:1:0|value:float64:1:0|payload:float32:1020:0", 4096)

namespace {
constexpr const char *ENGINE = "native";
constexpr const char *SHUTDOWN_BAND = "!bench.shutdown";
constexpr double WARMUP_S = 5.0;

const PlhNativeContext *g_ctx = nullptr;
int64_t g_received = 0;
double  g_t0 = 0.0;
int64_t g_warmup_received = -1;
double  g_warmup_t = 0.0;
bool    g_band_joined = false;

double now_s()
{
    struct timespec ts{};
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return static_cast<double>(ts.tv_sec) + static_cast<double>(ts.tv_nsec) * 1e-9;
}
}

extern "C" PLH_EXPORT const PlhAbiInfo *native_abi_info(void)
{
    static const PlhAbiInfo info = {
        sizeof(PlhAbiInfo),
        static_cast<uint32_t>(sizeof(void *)),
        static_cast<uint32_t>(sizeof(size_t)),
        1, PLH_NATIVE_API_VERSION,
        0,0,0, 0,0, 0,0, 0,0, 0,0, 0,0, 0,0, {0}
    };
    return &info;
}

extern "C" PLH_EXPORT bool native_init(const PlhNativeContext *ctx)
{
    g_ctx = ctx;
    return true;
}

extern "C" PLH_EXPORT void native_finalize(void) { g_ctx = nullptr; }
extern "C" PLH_EXPORT const char *native_name(void)    { return "bench_native_consumer"; }
extern "C" PLH_EXPORT const char *native_version(void) { return "1.0.0"; }

extern "C" PLH_EXPORT void on_init(const char *)
{
    g_t0 = now_s();
    if (g_ctx && g_ctx->log)
        g_ctx->log(g_ctx, PLH_LOG_INFO, "BenchCons-Native started");
}

extern "C" PLH_EXPORT bool on_consume(const plh_rx_t *rx)
{
    if (!g_band_joined && g_ctx && g_ctx->band_join)
    {
        char *r = g_ctx->band_join(g_ctx, SHUTDOWN_BAND);
        g_band_joined = true;
        if (r) { if (g_ctx->log) g_ctx->log(g_ctx, PLH_LOG_INFO, "BenchCons joined band"); free(r); }
    }
    if (!rx || !rx->slot || rx->slot_size < 16) return true;
    g_received++;
    if (g_warmup_received < 0 && (g_received & 0x3FF) == 0)
    {
        double elapsed = now_s() - g_t0;
        if (elapsed >= WARMUP_S)
        {
            g_warmup_received = g_received;
            g_warmup_t = now_s();
        }
    }
    return true;
}

// Correct signature per native_invoke_types.h:
//   void on_band_message(const plh_band_message_args_t *args)
// Pre-fix this took (band, sender, body_json) as 3 separate args —
// C ABI mismatch with the framework's `fn(&args)` call meant the
// callback received register garbage and silently returned.
extern "C" PLH_EXPORT void on_band_message(const plh_band_message_args_t *args)
{
    if (!args || !args->band || !args->body_json) return;
    if (std::strcmp(args->band, SHUTDOWN_BAND) != 0) return;
    // Simple substring check — body is JSON like {"cmd":"drain"}.
    if (std::strstr(args->body_json, "drain") != nullptr)
    {
        if (g_ctx && g_ctx->log)
            g_ctx->log(g_ctx, PLH_LOG_INFO, "BenchCons received drain — stopping");
        if (g_ctx && g_ctx->request_stop)
            g_ctx->request_stop(g_ctx);
    }
}

extern "C" PLH_EXPORT void on_stop(const char *)
{
    if (!g_ctx || !g_ctx->log) return;
    double t_end = now_s();
    double elapsed = t_end - g_t0; if (elapsed < 1e-9) elapsed = 1e-9;
    int64_t steady_slots = (g_warmup_received >= 0) ? (g_received - g_warmup_received) : 0;
    double  steady_s     = (g_warmup_received >= 0) ? (t_end - g_warmup_t)              : 0.0;
    double  steady_rate  = (steady_slots > 0) ? (steady_slots / (steady_s < 1e-9 ? 1e-9 : steady_s)) : 0.0;
    uint64_t in_received = 0;
    if (g_ctx->in_slots_received) in_received = g_ctx->in_slots_received(g_ctx);
    char buf[512];
    std::snprintf(buf, sizeof(buf),
        "BENCH-CONS engine=%s total=%lld elapsed_s=%.3f avg_rate=%.0f "
        "steady_slots=%lld steady_s=%.3f steady_rate=%.0f "
        "iters=%lld iters_per_s=%lld "
        "in_received=%lld work_us_last=%d",
        ENGINE, (long long)g_received, elapsed, g_received / elapsed,
        (long long)steady_slots, steady_s, steady_rate,
        (long long)g_received, (long long)((double)g_received / elapsed),
        (long long)in_received, 0);
    g_ctx->log(g_ctx, PLH_LOG_INFO, buf);
}
