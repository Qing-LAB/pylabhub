/* Engine-throughput bench: Native producer.  Mirror of bench-python/lua. */
#include "utils/native_engine_api.h"
#include <cstdint>
#include <cstdio>
#include <ctime>
#include <cstring>
#include <cstdlib>

PLH_DECLARE_SCHEMA(OutSlotFrame,
    "count:int64:1:0|value:float64:1:0|payload:float32:1020:0", 4096)

namespace {
constexpr const char *ENGINE = "native";
constexpr const char *SHUTDOWN_BAND = "!bench.shutdown";
constexpr double WARMUP_S = 5.0;
constexpr double MEASUREMENT_S = 40.0;

const PlhNativeContext *g_ctx = nullptr;
int64_t g_count = 0;
double  g_t0 = 0.0;
int64_t g_warmup_count = -1;
double  g_warmup_t = 0.0;
bool    g_band_joined = false;
bool    g_drain_sent  = false;

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
extern "C" PLH_EXPORT const char *native_name(void)    { return "bench_native_producer"; }
extern "C" PLH_EXPORT const char *native_version(void) { return "1.0.0"; }

extern "C" PLH_EXPORT void on_init(const char *)
{
    g_t0 = now_s();
    if (g_ctx && g_ctx->log)
        g_ctx->log(g_ctx, PLH_LOG_INFO, "BenchProd-Native started");
}

extern "C" PLH_EXPORT bool on_produce(const plh_tx_t *tx)
{
    if (!g_band_joined && g_ctx && g_ctx->band_join)
    {
        // API v6: band_join returns int (1=joined, 0=rejected, -1=error).
        const int r = g_ctx->band_join(g_ctx, SHUTDOWN_BAND);
        g_band_joined = true;
        if (r == 1 && g_ctx->log)
            g_ctx->log(g_ctx, PLH_LOG_INFO, "BenchProd joined band");
    }
    if (!tx || !tx->slot || tx->slot_size < 16) return false;

    auto *count_ptr = static_cast<int64_t *>(tx->slot);
    auto *value_ptr = reinterpret_cast<double *>(static_cast<char *>(tx->slot) + 8);

    g_count++;
    *count_ptr = g_count;
    *value_ptr = static_cast<double>(g_count) * 0.5;

    // Fresh random fill of the 4 KB payload — 1020 float32 = 4080 B.
    auto *payload_ptr = reinterpret_cast<float *>(static_cast<char *>(tx->slot) + 16);
    constexpr float inv_rand_max = 1.0f / static_cast<float>(RAND_MAX);
    for (int i = 0; i < 1020; ++i)
        payload_ptr[i] = static_cast<float>(std::rand()) * inv_rand_max;

    if ((g_count & 0x3FF) == 0)
    {
        double elapsed = now_s() - g_t0;
        if (g_warmup_count < 0 && elapsed >= WARMUP_S)
        {
            g_warmup_count = g_count;
            g_warmup_t = now_s();
        }
        if (!g_drain_sent && elapsed >= MEASUREMENT_S)
        {
            g_drain_sent = true;
            if (g_ctx && g_ctx->log)
                g_ctx->log(g_ctx, PLH_LOG_INFO, "BenchProd reached MEASUREMENT_S — broadcasting drain");
            if (g_ctx && g_ctx->band_broadcast)
                g_ctx->band_broadcast(g_ctx, SHUTDOWN_BAND, "{\"cmd\":\"drain\"}");
            if (g_ctx && g_ctx->request_stop)
                g_ctx->request_stop(g_ctx);
        }
    }
    return true;
}

extern "C" PLH_EXPORT void on_stop(const char *)
{
    if (!g_ctx || !g_ctx->log) return;
    double t_end = now_s();
    double elapsed = (t_end - g_t0); if (elapsed < 1e-9) elapsed = 1e-9;
    int64_t steady_slots = (g_warmup_count >= 0) ? (g_count - g_warmup_count) : 0;
    double  steady_s     = (g_warmup_count >= 0) ? (t_end - g_warmup_t)        : 0.0;
    double  steady_rate  = (steady_slots > 0)    ? (steady_slots / (steady_s < 1e-9 ? 1e-9 : steady_s)) : 0.0;
    uint64_t written = 0, drops = 0;
    if (g_ctx->out_slots_written) written = g_ctx->out_slots_written(g_ctx);
    if (g_ctx->out_drop_count)    drops   = g_ctx->out_drop_count(g_ctx);
    char buf[512];
    std::snprintf(buf, sizeof(buf),
        "BENCH-PROD engine=%s total=%lld elapsed_s=%.3f avg_rate=%.0f "
        "steady_slots=%lld steady_s=%.3f steady_rate=%.0f "
        "iters=%lld iters_per_s=%lld "
        "written=%lld drops=%lld work_us_last=%d",
        ENGINE, (long long)g_count, elapsed, g_count / elapsed,
        (long long)steady_slots, steady_s, steady_rate,
        (long long)g_count, (long long)((double)g_count / elapsed),
        (long long)written, (long long)drops, 0);
    g_ctx->log(g_ctx, PLH_LOG_INFO, buf);
}
