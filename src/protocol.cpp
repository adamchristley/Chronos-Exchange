#include "chronos/protocol.hpp"

#include <arpa/inet.h>
#include <endian.h>

namespace chronos {
namespace {
std::uint64_t hton64(std::uint64_t v) { return htobe64(v); }
std::uint64_t ntoh64(std::uint64_t v) { return be64toh(v); }
std::int64_t hton_i64(std::int64_t v) { return static_cast<std::int64_t>(htobe64(static_cast<std::uint64_t>(v))); }
std::int64_t ntoh_i64(std::int64_t v) { return static_cast<std::int64_t>(be64toh(static_cast<std::uint64_t>(v))); }
}

WireOrder encode_order(const Order& order, OrderType type) {
    WireOrder w{};
    w.magic = htonl(kProtocolMagic);
    w.version = htons(kProtocolVersion);
    w.type = static_cast<std::uint8_t>(type);
    w.side = static_cast<std::uint8_t>(order.side);
    w.order_id = hton64(order.id);
    w.client_id = htonl(order.client_id);
    std::memcpy(w.symbol, order.symbol.data(), order.symbol.size());
    w.price_ticks = hton_i64(order.price_ticks);
    w.quantity = htonl(order.quantity);
    w.timestamp_ns = hton64(order.timestamp_ns);
    return w;
}

bool decode_order(const WireOrder& w, Order& order, OrderType& type) {
    if (ntohl(w.magic) != kProtocolMagic || ntohs(w.version) != kProtocolVersion) return false;
    if (w.type != static_cast<std::uint8_t>(OrderType::New) && w.type != static_cast<std::uint8_t>(OrderType::Cancel)) return false;
    if (w.side != static_cast<std::uint8_t>(Side::Buy) && w.side != static_cast<std::uint8_t>(Side::Sell)) return false;
    type = static_cast<OrderType>(w.type);
    order.id = ntoh64(w.order_id);
    order.client_id = ntohl(w.client_id);
    std::memcpy(order.symbol.data(), w.symbol, order.symbol.size());
    order.side = static_cast<Side>(w.side);
    order.price_ticks = ntoh_i64(w.price_ticks);
    order.quantity = ntohl(w.quantity);
    order.timestamp_ns = ntoh64(w.timestamp_ns);
    return true;
}

WireMarketEvent encode_event(const EngineEvent& event) {
    WireMarketEvent w{};
    w.magic = htonl(kProtocolMagic);
    w.version = htons(kProtocolVersion);
    w.type = static_cast<std::uint8_t>(event.type);
    w.sequence = hton64(event.sequence);
    w.order_id = hton64(event.order_id);
    w.contra_order_id = hton64(event.contra_order_id);
    std::memcpy(w.symbol, event.symbol.data(), event.symbol.size());
    w.price_ticks = hton_i64(event.price_ticks);
    w.quantity = htonl(event.quantity);
    w.timestamp_ns = hton64(event.timestamp_ns);
    w.reason_code = htonl(event.reason_code);
    return w;
}

bool decode_event(const WireMarketEvent& w, EngineEvent& event) {
    if (ntohl(w.magic) != kProtocolMagic || ntohs(w.version) != kProtocolVersion) return false;
    event.type = static_cast<EventType>(w.type);
    event.sequence = ntoh64(w.sequence);
    event.order_id = ntoh64(w.order_id);
    event.contra_order_id = ntoh64(w.contra_order_id);
    std::memcpy(event.symbol.data(), w.symbol, event.symbol.size());
    event.price_ticks = ntoh_i64(w.price_ticks);
    event.quantity = ntohl(w.quantity);
    event.timestamp_ns = ntoh64(w.timestamp_ns);
    event.reason_code = ntohl(w.reason_code);
    return true;
}

} // namespace chronos
