#include "chronos/event_log.hpp"
#include "chronos/order_book.hpp"

#include <cstring>
#include <iostream>
#include <string>
#include <unordered_map>

using namespace chronos;

int main(int argc, char** argv) {
    if (argc < 2) { std::cerr << "usage: chronos_replay EVENT_LOG [--until SEQUENCE]\n"; return 1; }
    std::uint64_t until = UINT64_MAX;
    for (int i = 2; i < argc; ++i) {
        if (std::string(argv[i]) == "--until" && i + 1 < argc) until = std::stoull(argv[++i]);
    }
    EventLogReader reader(argv[1]);
    std::unordered_map<std::string, OrderBook> books;
    LogRecord r{};
    std::uint64_t orders = 0, recorded_trades = 0;
    while (reader.next(r)) {
        if (r.sequence > until) break;
        if (r.record_type == 1) {
            Order o{};
            o.id = r.order_id; o.client_id = r.client_id;
            std::memcpy(o.symbol.data(), r.symbol, o.symbol.size());
            o.side = static_cast<Side>(r.side); o.price_ticks = r.price_ticks;
            o.quantity = r.quantity; o.timestamp_ns = r.timestamp_ns;
            auto key = symbol_to_string(o.symbol);
            auto& book = books.try_emplace(key).first->second;
            if (static_cast<OrderType>(r.subtype) == OrderType::Cancel) book.cancel(o.id);
            else book.submit(o, o.timestamp_ns);
            ++orders;
        } else if (r.record_type == 2 && static_cast<EventType>(r.subtype) == EventType::Trade) {
            ++recorded_trades;
        }
    }
    std::size_t resting = 0;
    std::uint64_t reconstructed = 0;
    for (const auto& [symbol, book] : books) {
        resting += book.resting_orders();
        reconstructed += book.trade_sequence();
        std::cout << "symbol=" << symbol;
        if (book.best_bid()) std::cout << " best_bid=" << *book.best_bid();
        if (book.best_ask()) std::cout << " best_ask=" << *book.best_ask();
        std::cout << '\n';
    }
    std::cout << "replayed_orders=" << orders
              << " resting_orders=" << resting
              << " reconstructed_trades=" << reconstructed
              << " recorded_trade_events=" << recorded_trades << '\n';
    return reconstructed == recorded_trades ? 0 : 2;
}
