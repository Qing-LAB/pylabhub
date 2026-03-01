#pragma once
/**
 * @file plh_datahub.hpp
 * @brief Layer 3 umbrella — full DataHub API (producers, consumers, broker, config).
 *
 * **What is exposed** (all three hub roles):
 *   - Client API: DataBlock SHM, Messenger, hub::Producer, hub::Consumer, ChannelHandle
 *   - Server API: BrokerService, JsonConfig, HubConfig, ChannelAccessPolicy
 *   - Schema:     schema_blds (BLDS layout validation)
 *
 * **Include cost**: nlohmann/json.hpp, plh_service.hpp (lifecycle + logger + filelock + crypto).
 * Use individual component headers when you need only a subset:
 *   - DataBlock SHM only:  utils/data_block.hpp
 *   - Client (actor/script): utils/hub_producer.hpp or utils/hub_consumer.hpp
 *   - Server (hub admin):  utils/broker_service.hpp + utils/hub_config.hpp
 *
 * @note `nlohmann/json.hpp` is not included here directly — it arrives transitively
 *       via messenger.hpp. Add your own #include <nlohmann/json.hpp> if you need it
 *       explicitly in a TU that does not include any Messenger/hub headers.
 */
#include "plh_service.hpp"

#include "utils/schema_blds.hpp"
#include "utils/json_config.hpp"
#include "utils/broker_service.hpp"
#include "utils/messenger.hpp"
#include "utils/data_block.hpp"
#include "utils/hub_producer.hpp"
#include "utils/hub_consumer.hpp"
#include "utils/hub_config.hpp"
