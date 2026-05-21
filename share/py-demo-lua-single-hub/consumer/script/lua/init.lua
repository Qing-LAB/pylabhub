-- Lua demo consumer.  Reads (count, value, doubled) from the processor;
-- logs every 50 slots.  No explicit shutdown — default on_channel_closing
-- stops the role when hub-b notifies the processor's output channel closed.

local received = 0

function on_init(api)
    api.log("info", string.format(
        "LuaCons started uid=%s channel=%s",
        api.uid(), api.channel()))
end

function on_consume(rx, msgs, api)
    if rx.slot == nil then
        return true
    end
    received = received + 1
    if received % 50 == 0 then
        api.log("info", string.format(
            "LuaCons received count=%d value=%.4f doubled=%.4f total=%d",
            rx.slot.count, rx.slot.value, rx.slot.doubled, received))
    end
    return true
end

function on_stop(api)
    api.log("info", string.format(
        "LuaCons STOPPED total_received=%d (close-cascade)", received))
end
