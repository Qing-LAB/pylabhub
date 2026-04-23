/**
 * @file native_multifield_module.cpp
 * @brief Test native module — multi-field schema with mixed types, arrays, padding.
 *
 * Schema: float64 + uint8 + int32 + float32[3] + bytes[8] = 40 bytes aligned.
 * Exercises all three roles (producer, consumer, processor).
 */
#include "utils/native_engine_api.h"
#include "pylabhub_version.h"  // PYLABHUB_VERSION_* for HEP-0032 axes

#include <cstring>

// ── Slot struct (aligned packing) ───────────────────────────────────────
struct Slot
{
    double   ts;        // offset 0
    uint8_t  flag;      // offset 8
    // 3 bytes padding
    int32_t  count;     // offset 12
    float    values[3]; // offset 16
    uint8_t  tag[8];    // offset 28
    // 4 bytes tail padding (max_align=8)
};
static_assert(sizeof(Slot) == 40, "Slot must be 40 bytes (aligned)");

// ── Schema declarations — standard directional names ────────────────────
#define SLOT_SCHEMA "ts:float64:1:0|flag:uint8:1:0|count:int32:1:0|values:float32:3:0|tag:bytes:1:8"
PLH_DECLARE_SCHEMA(SlotFrame,    SLOT_SCHEMA, 40)
PLH_DECLARE_SCHEMA(OutSlotFrame, SLOT_SCHEMA, 40)
PLH_DECLARE_SCHEMA(InSlotFrame,  SLOT_SCHEMA, 40)

// ── Module state ────────────────────────────────────────────────────────
static const PlhNativeContext *g_ctx = nullptr;

// ── Required symbols ────────────────────────────────────────────────────

extern "C" PLH_EXPORT const PlhAbiInfo *native_abi_info(void)
{
    // See good_producer_plugin.cpp for notes on the HEP-0032 fields.
    static const PlhAbiInfo info = {
        sizeof(PlhAbiInfo),
        static_cast<uint32_t>(sizeof(void *)),
        static_cast<uint32_t>(sizeof(size_t)),
        1,
        PLH_NATIVE_API_VERSION,
        static_cast<uint16_t>(PYLABHUB_VERSION_MAJOR),
        static_cast<uint16_t>(PYLABHUB_VERSION_MINOR),
        static_cast<uint16_t>(PYLABHUB_VERSION_ROLLING),
        PLH_COMPONENT_SHM_MAJOR,           PLH_COMPONENT_SHM_MINOR,
        PLH_COMPONENT_BROKER_PROTO_MAJOR,  PLH_COMPONENT_BROKER_PROTO_MINOR,
        PLH_COMPONENT_ZMQ_FRAME_MAJOR,     PLH_COMPONENT_ZMQ_FRAME_MINOR,
        PLH_COMPONENT_SCRIPT_API_MAJOR,    PLH_COMPONENT_SCRIPT_API_MINOR,
        PLH_COMPONENT_SCRIPT_ENGINE_MAJOR, PLH_COMPONENT_SCRIPT_ENGINE_MINOR,
        PLH_COMPONENT_CONFIG_MAJOR,        PLH_COMPONENT_CONFIG_MINOR,
        {0}
    };
    return &info;
}

extern "C" PLH_EXPORT bool native_init(const PlhNativeContext *ctx)
{
    g_ctx = ctx;
    return true;
}

extern "C" PLH_EXPORT void native_finalize(void) { g_ctx = nullptr; }
extern "C" PLH_EXPORT const char *native_name(void) { return "multifield"; }
extern "C" PLH_EXPORT const char *native_version(void) { return "1.0.0"; }

// ── Callbacks ───────────────────────────────────────────────────────────

extern "C" PLH_EXPORT void on_init(const char *) {}
extern "C" PLH_EXPORT void on_stop(const char *) {}

extern "C" PLH_EXPORT bool on_produce(const plh_tx_t *tx)
{
    if (!tx || !tx->slot || tx->slot_size < sizeof(Slot))
        return false;

    auto *s = static_cast<Slot *>(tx->slot);
    s->ts        = 1.23456789;
    s->flag      = 0xAB;
    s->count     = -42;
    s->values[0] = 1.0f;
    s->values[1] = 2.5f;
    s->values[2] = -3.75f;
    std::memcpy(s->tag, "DEADBEEF", 8);
    return true;
}

extern "C" PLH_EXPORT bool on_consume(const plh_rx_t *rx)
{
    if (!rx || !rx->slot || rx->slot_size < sizeof(Slot))
        return false;

    auto *s = static_cast<const Slot *>(rx->slot);
    (void)s->ts;
    (void)s->flag;
    (void)s->count;
    (void)s->values[0];
    (void)s->tag[0];
    return true;
}

extern "C" PLH_EXPORT bool on_process(const plh_rx_t *rx, const plh_tx_t *tx)
{
    if (!rx || !rx->slot || rx->slot_size < sizeof(Slot))
        return false;
    if (!tx || !tx->slot || tx->slot_size < sizeof(Slot))
        return false;

    std::memcpy(tx->slot, rx->slot, sizeof(Slot));
    static_cast<Slot *>(tx->slot)->count *= 2;
    return true;
}
