// src/utils/channel_handle.cpp
#include "channel_handle_factory.hpp"
#include "utils/channel_handle.hpp"
#include "utils/zmq_context.hpp"

#include "cppzmq/zmq.hpp"
#include "cppzmq/zmq_addon.hpp"

#include <zmq.h>

#include <cassert>
#include <chrono>
#include <vector>

namespace pylabhub::hub
{

namespace
{
// Universal framing type bytes
constexpr char kTypeData    = 'A'; // raw data frame
constexpr char kTypeControl = 'C'; // control frame

/// Convenience: poll a socket for incoming data within @p timeout_ms.
/// Returns true if data is available.
bool poll_readable(zmq::socket_t &socket, int timeout_ms)
{
    std::vector<zmq::pollitem_t> items = {{socket.handle(), 0, ZMQ_POLLIN, 0}};
    zmq::poll(items, std::chrono::milliseconds(timeout_ms));
    return (items[0].revents & ZMQ_POLLIN) != 0;
}
} // namespace

// ============================================================================
// ChannelHandleImpl
// ============================================================================

struct ChannelHandleImpl
{
    std::string    channel;
    ChannelPattern pattern{ChannelPattern::PubSub};
    bool           has_shm{false};
    bool           is_producer{false};
    bool           valid{true};

    // ZMQ sockets (live in the calling-thread; not shared with Messenger worker).
    // ctrl_socket: ROUTER (producer) or DEALER (consumer)
    // data_socket: XPUB/PUSH (producer, non-Bidir) or SUB/PULL (consumer, non-Bidir)
    std::optional<zmq::socket_t> ctrl_socket;
    std::optional<zmq::socket_t> data_socket;
};

// ============================================================================
// ChannelHandle — construction / destruction
// ============================================================================

ChannelHandle::ChannelHandle() : pImpl(std::make_unique<ChannelHandleImpl>())
{
    pImpl->valid = false; // default-constructed = empty/invalid
}

ChannelHandle::ChannelHandle(std::unique_ptr<ChannelHandleImpl> impl)
    : pImpl(std::move(impl))
{
}

ChannelHandle::~ChannelHandle() = default;

ChannelHandle::ChannelHandle(ChannelHandle &&) noexcept = default;
ChannelHandle &ChannelHandle::operator=(ChannelHandle &&) noexcept = default;

// ============================================================================
// Introspection
// ============================================================================

ChannelPattern ChannelHandle::pattern() const
{
    return pImpl ? pImpl->pattern : ChannelPattern::PubSub;
}

bool ChannelHandle::has_shm() const
{
    return pImpl && pImpl->has_shm;
}

const std::string &ChannelHandle::channel_name() const
{
    static const std::string kEmpty;
    return pImpl ? pImpl->channel : kEmpty;
}

bool ChannelHandle::is_valid() const
{
    return pImpl && pImpl->valid &&
           (pImpl->ctrl_socket.has_value() || pImpl->data_socket.has_value());
}

void ChannelHandle::invalidate()
{
    if (pImpl)
    {
        pImpl->valid = false;
    }
}

// ============================================================================
// send() — data frame
// ============================================================================

bool ChannelHandle::send(const void *data, size_t size, const std::string &identity)
{
    if (!is_valid())
    {
        return false;
    }

    try
    {
        if (pImpl->pattern == ChannelPattern::Bidir)
        {
            // Both producer and consumer use ctrl_socket for data.
            assert(pImpl->ctrl_socket.has_value());
            zmq::socket_t &sock = *pImpl->ctrl_socket;

            if (pImpl->is_producer)
            {
                // ROUTER: must supply identity
                if (identity.empty())
                {
                    return false;
                }
                sock.send(zmq::message_t(identity.data(), identity.size()),
                          zmq::send_flags::sndmore);
                sock.send(zmq::message_t(&kTypeData, 1), zmq::send_flags::sndmore);
                sock.send(zmq::message_t(data, size), zmq::send_flags::none);
            }
            else
            {
                // DEALER: no identity needed
                sock.send(zmq::message_t(&kTypeData, 1), zmq::send_flags::sndmore);
                sock.send(zmq::message_t(data, size), zmq::send_flags::none);
            }
        }
        else
        {
            // PubSub / Pipeline: producer sends on data_socket; consumer cannot send data.
            if (!pImpl->is_producer || !pImpl->data_socket.has_value())
            {
                return false;
            }
            zmq::socket_t &sock = *pImpl->data_socket;
            sock.send(zmq::message_t(&kTypeData, 1), zmq::send_flags::sndmore);
            sock.send(zmq::message_t(data, size), zmq::send_flags::none);
        }
        return true;
    }
    catch (const zmq::error_t &)
    {
        return false;
    }
}

// ============================================================================
// recv() — data frame (with retry on non-data frames)
// ============================================================================

bool ChannelHandle::recv(std::vector<std::byte> &buf, int timeout_ms,
                         std::string *out_identity)
{
    if (!is_valid())
    {
        return false;
    }

    // Choose socket: consumer non-Bidir uses data_socket; everything else uses ctrl_socket.
    zmq::socket_t *sock = nullptr;
    bool from_router = false; // ROUTER prepends identity frame

    if (pImpl->pattern == ChannelPattern::Bidir)
    {
        if (!pImpl->ctrl_socket.has_value())
        {
            return false;
        }
        sock        = &(*pImpl->ctrl_socket);
        from_router = pImpl->is_producer; // producer's ctrl is ROUTER
    }
    else
    {
        if (pImpl->is_producer)
        {
            return false; // producer doesn't receive data on data socket
        }
        if (!pImpl->data_socket.has_value())
        {
            return false;
        }
        sock        = &(*pImpl->data_socket);
        from_router = false;
    }

    // Poll then recv; discard control frames and retry.
    const auto deadline = std::chrono::steady_clock::now() +
                          std::chrono::milliseconds(timeout_ms);

    while (true)
    {
        auto remaining_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
            deadline - std::chrono::steady_clock::now()).count();
        if (remaining_ms <= 0)
        {
            return false;
        }

        if (!poll_readable(*sock, static_cast<int>(remaining_ms)))
        {
            return false;
        }

        std::vector<zmq::message_t> frames;
        static_cast<void>(zmq::recv_multipart(*sock, std::back_inserter(frames),
                                              zmq::recv_flags::dontwait));

        // Expected for ROUTER: [identity, type_byte, payload]
        // Expected for DEALER/SUB/PULL: [type_byte, payload]
        const size_t min_frames = from_router ? 3u : 2u;
        if (frames.size() < min_frames)
        {
            continue; // malformed, retry
        }

        const size_t type_idx    = from_router ? 1u : 0u;
        const size_t payload_idx = from_router ? 2u : 1u;

        if (frames[type_idx].size() < 1)
        {
            continue;
        }
        const char type_byte = *static_cast<const char *>(frames[type_idx].data());

        if (type_byte != kTypeData)
        {
            // Control frame — not a data message; discard and retry.
            continue;
        }

        if (out_identity && from_router)
        {
            out_identity->assign(static_cast<const char *>(frames[0].data()),
                                 frames[0].size());
        }
        const auto &payload = frames[payload_idx];
        buf.resize(payload.size());
        std::memcpy(buf.data(), payload.data(), payload.size());
        return true;
    }
}

// ============================================================================
// send_ctrl() — control frame
// ============================================================================

bool ChannelHandle::send_ctrl(const void *data, size_t size, const std::string &identity)
{
    if (!is_valid() || !pImpl->ctrl_socket.has_value())
    {
        return false;
    }

    try
    {
        zmq::socket_t &sock = *pImpl->ctrl_socket;
        const std::string ctrl_type = "CTRL";

        if (pImpl->is_producer)
        {
            // ROUTER: must supply identity
            if (identity.empty())
            {
                return false;
            }
            sock.send(zmq::message_t(identity.data(), identity.size()),
                      zmq::send_flags::sndmore);
            sock.send(zmq::message_t(&kTypeControl, 1), zmq::send_flags::sndmore);
            sock.send(zmq::message_t(ctrl_type.data(), ctrl_type.size()),
                      zmq::send_flags::sndmore);
            sock.send(zmq::message_t(data, size), zmq::send_flags::none);
        }
        else
        {
            // DEALER (consumer)
            sock.send(zmq::message_t(&kTypeControl, 1), zmq::send_flags::sndmore);
            sock.send(zmq::message_t(ctrl_type.data(), ctrl_type.size()),
                      zmq::send_flags::sndmore);
            sock.send(zmq::message_t(data, size), zmq::send_flags::none);
        }
        return true;
    }
    catch (const zmq::error_t &)
    {
        return false;
    }
}

// ============================================================================
// recv_ctrl() — control frame
// ============================================================================

bool ChannelHandle::recv_ctrl(std::vector<std::byte> &buf, int timeout_ms,
                               std::string *out_type, std::string *out_identity)
{
    if (!is_valid() || !pImpl->ctrl_socket.has_value())
    {
        return false;
    }

    zmq::socket_t &sock  = *pImpl->ctrl_socket;
    const bool from_router = pImpl->is_producer; // producer's ctrl is ROUTER

    const auto deadline = std::chrono::steady_clock::now() +
                          std::chrono::milliseconds(timeout_ms);

    while (true)
    {
        auto remaining_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
            deadline - std::chrono::steady_clock::now()).count();
        if (remaining_ms <= 0)
        {
            return false;
        }

        if (!poll_readable(sock, static_cast<int>(remaining_ms)))
        {
            return false;
        }

        std::vector<zmq::message_t> frames;
        static_cast<void>(zmq::recv_multipart(sock, std::back_inserter(frames),
                                              zmq::recv_flags::dontwait));

        // ROUTER: [identity, 'C', type_str, body]  min 4 frames
        // DEALER: ['C', type_str, body]             min 3 frames
        const size_t min_frames = from_router ? 4u : 3u;
        if (frames.size() < min_frames)
        {
            continue;
        }

        const size_t type_byte_idx = from_router ? 1u : 0u;
        const size_t type_str_idx  = from_router ? 2u : 1u;
        const size_t body_idx      = from_router ? 3u : 2u;

        if (frames[type_byte_idx].size() < 1)
        {
            continue;
        }
        const char tb = *static_cast<const char *>(frames[type_byte_idx].data());
        if (tb != kTypeControl)
        {
            // Data frame — not a control message; discard and retry.
            continue;
        }

        if (out_identity && from_router)
        {
            out_identity->assign(static_cast<const char *>(frames[0].data()),
                                 frames[0].size());
        }
        if (out_type)
        {
            out_type->assign(static_cast<const char *>(frames[type_str_idx].data()),
                             frames[type_str_idx].size());
        }
        const auto &body = frames[body_idx];
        buf.resize(body.size());
        std::memcpy(buf.data(), body.data(), body.size());
        return true;
    }
}

// ============================================================================
// ChannelHandleImpl factory helpers (used by Messenger internals)
// ============================================================================

ChannelHandle make_producer_handle(const std::string &channel,
                                   ChannelPattern     pattern,
                                   bool               has_shm,
                                   zmq::socket_t    &&ctrl_sock,
                                   zmq::socket_t    &&data_sock_or_dummy,
                                   bool               has_data_sock)
{
    auto impl = std::make_unique<ChannelHandleImpl>();
    impl->channel     = channel;
    impl->pattern     = pattern;
    impl->has_shm     = has_shm;
    impl->is_producer = true;
    impl->valid       = true;
    impl->ctrl_socket.emplace(std::move(ctrl_sock));
    if (has_data_sock)
    {
        impl->data_socket.emplace(std::move(data_sock_or_dummy));
    }
    return ChannelHandle(std::move(impl));
}

ChannelHandle make_consumer_handle(const std::string &channel,
                                   ChannelPattern     pattern,
                                   bool               has_shm,
                                   zmq::socket_t    &&ctrl_sock,
                                   zmq::socket_t    &&data_sock_or_dummy,
                                   bool               has_data_sock)
{
    auto impl = std::make_unique<ChannelHandleImpl>();
    impl->channel     = channel;
    impl->pattern     = pattern;
    impl->has_shm     = has_shm;
    impl->is_producer = false;
    impl->valid       = true;
    impl->ctrl_socket.emplace(std::move(ctrl_sock));
    if (has_data_sock)
    {
        impl->data_socket.emplace(std::move(data_sock_or_dummy));
    }
    return ChannelHandle(std::move(impl));
}

} // namespace pylabhub::hub
