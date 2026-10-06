#pragma once

#include "chronos/types.hpp"

#include <cstdint>
#include <fstream>
#include <functional>
#include <string>

namespace chronos {

#pragma pack(push, 1)
struct LogRecord {
    std::uint32_t magic{0x43484c47}; // CHLG
    std::uint16_t version{1};
    std::uint8_t record_type{0}; // 1 order, 2 event
    std::uint8_t subtype{0};
    std::uint64_t sequence{0};
    std::uint64_t order_id{0};
    std::uint64_t contra_order_id{0};
    std::uint32_t client_id{0};
    char symbol[8]{};
    std::uint8_t side{0};
    std::uint8_t reserved[3]{};
    std::int64_t price_ticks{0};
    std::uint32_t quantity{0};
    std::uint64_t timestamp_ns{0};
    std::uint32_t reason_code{0};
};
#pragma pack(pop)

class EventLogWriter {
public:
    explicit EventLogWriter(const std::string& path);
    void append_order(const Order& order, OrderType type, std::uint64_t sequence);
    void append_event(const EngineEvent& event);
    void flush();
private:
    std::ofstream out_;
};

class EventLogReader {
public:
    explicit EventLogReader(const std::string& path);
    bool next(LogRecord& record);
private:
    std::ifstream in_;
};

} // namespace chronos
