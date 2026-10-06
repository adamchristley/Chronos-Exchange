#pragma once

#include "chronos/types.hpp"

#include <array>
#include <cstdint>
#include <cstring>

namespace chronos {

constexpr std::uint32_t kProtocolMagic = 0x4348524f; // CHRO
constexpr std::uint16_t kProtocolVersion = 1;

#pragma pack(push, 1)
struct WireOrder {
    std::uint32_t magic;
    std::uint16_t version;
    std::uint8_t type;
    std::uint8_t side;
    std::uint64_t order_id;
    std::uint32_t client_id;
    char symbol[8];
    std::int64_t price_ticks;
    std::uint32_t quantity;
    std::uint64_t timestamp_ns;
};

struct WireMarketEvent {
    std::uint32_t magic;
    std::uint16_t version;
    std::uint8_t type;
    std::uint8_t reserved;
    std::uint64_t sequence;
    std::uint64_t order_id;
    std::uint64_t contra_order_id;
    char symbol[8];
    std::int64_t price_ticks;
    std::uint32_t quantity;
    std::uint64_t timestamp_ns;
    std::uint32_t reason_code;
};
#pragma pack(pop)

static_assert(sizeof(WireOrder) == 48);
static_assert(sizeof(WireMarketEvent) == 64);

WireOrder encode_order(const Order& order, OrderType type = OrderType::New);
bool decode_order(const WireOrder& wire, Order& order, OrderType& type);
WireMarketEvent encode_event(const EngineEvent& event);
bool decode_event(const WireMarketEvent& wire, EngineEvent& event);

} // namespace chronos
