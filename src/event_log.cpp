#include "chronos/event_log.hpp"
#include "chronos/order_book.hpp"

#include <cstring>
#include <stdexcept>

namespace chronos {

EventLogWriter::EventLogWriter(const std::string& path)
    : out_(path, std::ios::binary | std::ios::app) {
    if (!out_) throw std::runtime_error("failed to open event log: " + path);
}

void EventLogWriter::append_order(const Order& order, OrderType type, std::uint64_t sequence) {
    LogRecord r{};
    r.record_type = 1;
    r.subtype = static_cast<std::uint8_t>(type);
    r.sequence = sequence;
    r.order_id = order.id;
    r.client_id = order.client_id;
    std::memcpy(r.symbol, order.symbol.data(), order.symbol.size());
    r.side = static_cast<std::uint8_t>(order.side);
    r.price_ticks = order.price_ticks;
    r.quantity = order.quantity;
    r.timestamp_ns = order.timestamp_ns;
    out_.write(reinterpret_cast<const char*>(&r), sizeof(r));
}

void EventLogWriter::append_event(const EngineEvent& event) {
    LogRecord r{};
    r.record_type = 2;
    r.subtype = static_cast<std::uint8_t>(event.type);
    r.sequence = event.sequence;
    r.order_id = event.order_id;
    r.contra_order_id = event.contra_order_id;
    std::memcpy(r.symbol, event.symbol.data(), event.symbol.size());
    r.price_ticks = event.price_ticks;
    r.quantity = event.quantity;
    r.timestamp_ns = event.timestamp_ns;
    r.reason_code = event.reason_code;
    out_.write(reinterpret_cast<const char*>(&r), sizeof(r));
}

void EventLogWriter::flush() { out_.flush(); }

EventLogReader::EventLogReader(const std::string& path) : in_(path, std::ios::binary) {
    if (!in_) throw std::runtime_error("failed to open event log: " + path);
}

bool EventLogReader::next(LogRecord& r) {
    in_.read(reinterpret_cast<char*>(&r), sizeof(r));
    if (!in_) return false;
    if (r.magic != 0x43484c47 || r.version != 1) throw std::runtime_error("invalid event log record");
    return true;
}

} // namespace chronos
