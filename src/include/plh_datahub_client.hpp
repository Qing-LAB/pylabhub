#pragma once
/**
 * @file plh_datahub_client.hpp
 * @brief Lightweight DataHub client API — for roles, scripts, and DataBlock-only consumers.
 *
 * Provides everything needed to read and write shared-memory channels:
 *   - DataBlock SHM layer (data_block.hpp)
 *   - Queue options (TxQueueOptions / RxQueueOptions):
 *     utils/role_api_base.hpp (post-2026-04-20; previously lived in
 *     the retired hub_producer.hpp / hub_consumer.hpp)
 *   - Application framework (plh_service.hpp: lifecycle, logger, filelock, crypto)
 *
 * **Excluded vs plh_datahub.hpp** (server/admin infrastructure):
 *   - BrokerService  — run the channel registry hub (hubshell only)
 *   - JsonConfig     — JSON-based config loading
 *   - HubConfig      — hub configuration singleton
 *   - schema_blds    — raw BLDS schema helpers (only needed by SHM creators)
 *
 * Use plh_datahub.hpp when you need BrokerService, JsonConfig, or HubConfig.
 */
#include "plh_service.hpp"

#include "utils/data_block.hpp"


#include "utils/hub_queue.hpp"
#include "utils/hub_shm_queue.hpp"
#include "utils/hub_zmq_queue.hpp"
#include "utils/schema_def.hpp"
#include "utils/schema_library.hpp"
#include "utils/schema_registry.hpp"
#include "utils/heartbeat_manager.hpp"
#include "utils/recovery_api.hpp"
#include "utils/slot_diagnostics.hpp"
#include "utils/slot_recovery.hpp"
#include "utils/integrity_validator.hpp"
