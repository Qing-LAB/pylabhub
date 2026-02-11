#include "plh_heartbeat_manager.hpp"
#include "utils/data_block.hpp"

namespace pylabhub::hub
{

HeartbeatManager::HeartbeatManager(DataBlockConsumer &consumer) : consumer_(&consumer)
{
    heartbeat_slot_ = consumer_->register_heartbeat();
}

HeartbeatManager::~HeartbeatManager()
{
    if (is_registered())
    {
        consumer_->unregister_heartbeat(heartbeat_slot_);
    }
}

HeartbeatManager::HeartbeatManager(HeartbeatManager &&other) noexcept
    : consumer_(other.consumer_), heartbeat_slot_(other.heartbeat_slot_)
{
    other.heartbeat_slot_ = -1; // Prevent other from unregistering
}

HeartbeatManager &HeartbeatManager::operator=(HeartbeatManager &&other) noexcept
{
    if (this != &other)
    {
        if (is_registered())
        {
            consumer_->unregister_heartbeat(heartbeat_slot_);
        }
        consumer_ = other.consumer_;
        heartbeat_slot_ = other.heartbeat_slot_;
        other.heartbeat_slot_ = -1;
    }
    return *this;
}

void HeartbeatManager::pulse()
{
    if (is_registered())
    {
        consumer_->update_heartbeat(heartbeat_slot_);
    }
}

bool HeartbeatManager::is_registered() const
{
    return heartbeat_slot_ >= 0;
}

} // namespace pylabhub::hub
