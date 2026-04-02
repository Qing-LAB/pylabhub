/**
 * @file native_invoke_types.h
 * @brief C-ABI invoke direction structs for native engine callbacks.
 *
 * Defines plh_rx_t (input), plh_tx_t (output), and plh_inbox_msg_t (inbox).
 * Pure C header — no C++ dependencies. Included by native_engine_api.h
 * and native_engine.hpp.
 *
 * Slot is const on the rx (input) side, writable on the tx (output) side.
 * Flexzone is always writable on both sides (HEP-0002 TABLE 1).
 */

#ifndef PYLABHUB_NATIVE_INVOKE_TYPES_H
#define PYLABHUB_NATIVE_INVOKE_TYPES_H

#include <stddef.h>
#include <stdint.h>

/** Input direction — data received from upstream. */
typedef struct
{
    const void *slot;       /**< Read-only input slot. */
    size_t      slot_size;
    void       *fz;         /**< Mutable flexzone (bidirectional per HEP-0002). */
    size_t      fz_size;
} plh_rx_t;

/** Output direction — data going downstream. */
typedef struct
{
    void  *slot;            /**< Writable output slot. */
    size_t slot_size;
    void  *fz;              /**< Mutable flexzone. */
    size_t fz_size;
} plh_tx_t;

/** Inbox message — one-shot peer-to-peer delivery. */
typedef struct
{
    const void *data;       /**< Typed payload (valid until next recv). */
    size_t      data_size;
    const char *sender_uid; /**< Sender's role UID. */
    uint64_t    seq;        /**< Sender's monotonic sequence number. */
} plh_inbox_msg_t;

#endif /* PYLABHUB_NATIVE_INVOKE_TYPES_H */
