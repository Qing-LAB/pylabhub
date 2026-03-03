#pragma once
/**
 * @file plh_datahub_client.hpp
 * @brief Lightweight DataHub client API — for actors, scripts, and DataBlock-only consumers.
 *
 * Provides everything needed to read and write shared-memory channels:
 *   - DataBlock SHM layer (data_block.hpp)
 *   - Hub producer service (hub_producer.hpp)
 *   - Hub consumer service (hub_consumer.hpp)
 *   - Application framework (plh_service.hpp: lifecycle, logger, filelock, crypto)
 *
 * **Excluded vs plh_datahub.hpp** (server/admin infrastructure):
 *   - BrokerService  — run the channel registry hub (hubshell only)
 *   - JsonConfig     — JSON-based config loading
 *   - HubConfig      — hub configuration singleton
 *   - schema_blds    — raw BLDS schema helpers (only needed by SHM creators)
 *
 * **Include cost**: nlohmann/json.hpp arrives transitively via messenger.hpp
 * (required by hub_producer.hpp and hub_consumer.hpp for channel options structs).
 * There is no way to avoid it without restructuring the Messenger public API.
 *
 * Use plh_datahub.hpp when you need BrokerService, JsonConfig, or HubConfig.
 */
#include "plh_service.hpp"

#include "utils/data_block.hpp"
#include "utils/hub_producer.hpp"
#include "utils/hub_consumer.hpp"
#include "utils/hub_queue.hpp"
#include "utils/hub_shm_queue.hpp"
#include "utils/hub_zmq_queue.hpp"
#include "utils/hub_processor.hpp"
#include "utils/schema_def.hpp"
#include "utils/schema_library.hpp"
#include "utils/schema_registry.hpp"
