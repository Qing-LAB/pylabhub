#pragma once
/**
 * @file plh_datahub.hpp
 * @brief Layer 3 umbrella — full DataHub API (producers, consumers, broker, config).
 *
 * **What is exposed** (all three hub roles):
 *   - Client API: DataBlock SHM, TxQueueOptions/RxQueueOptions (consumed
 *                 by RoleAPIBase::build_tx_queue / build_rx_queue — both
 *                 structs defined in utils/role_api_base.hpp),
 *                 QueueReader/QueueWriter abstract handles
 *   - Server API: BrokerService, JsonConfig, HubConfig, ChannelAccessPolicy
 *   - Schema:     schema_blds (BLDS layout validation)
 *
 * **Include cost**: plh_service.hpp (lifecycle + logger + filelock + crypto).
 * Use individual component headers when you need only a subset:
 *   - DataBlock SHM only:  utils/data_block.hpp
 *   - Queue options:       utils/role_api_base.hpp (TxQueueOptions /
 *     RxQueueOptions live next to the build_tx_queue/build_rx_queue
 *     methods that consume them).  The legacy hub_producer.hpp /
 *     hub_consumer.hpp headers were retired 2026-04-20.
 *   - Server (hub admin):  utils/broker_service.hpp + utils/hub_config.hpp
 */
#include "plh_service.hpp"

#include "utils/schema_blds.hpp"
#include "utils/json_config.hpp"
#include "utils/broker_service.hpp"
#include "utils/data_block.hpp"

#include "utils/hub_config.hpp"
#include "utils/channel_access_policy.hpp"
#include "utils/channel_pattern.hpp"
#include "utils/data_block_mutex.hpp"
