#include "chronos/order_book.hpp"
#include "chronos/spsc_ring_buffer.hpp"

#include <cassert>
#include <iostream>

using namespace chronos;

static Order make(std::uint64_t id, Side side, std::int64_t px, std::uint32_t qty) {
    return Order{id, 1, symbol_from_string("AAPL"), side, px, qty, 0};
}

int main() {
    {
        OrderBook b;
        assert(b.submit(make(1, Side::Buy, 10000, 10)).accepted);
        assert(b.submit(make(2, Side::Buy, 10100, 10)).accepted);
        auto r = b.submit(make(3, Side::Sell, 9900, 12));
        assert(r.trades.size() == 2);
        assert(r.trades[0].buy_order_id == 2);
        assert(r.trades[0].price_ticks == 10100);
        assert(r.trades[0].quantity == 10);
        assert(r.trades[1].buy_order_id == 1);
        assert(r.trades[1].quantity == 2);
        assert(b.best_bid() && *b.best_bid() == 10000);
    }
    {
        OrderBook b;
        b.submit(make(10, Side::Buy, 10000, 5));
        b.submit(make(11, Side::Buy, 10000, 5));
        auto r = b.submit(make(12, Side::Sell, 10000, 6));
        assert(r.trades.size() == 2);
        assert(r.trades[0].buy_order_id == 10);
        assert(r.trades[1].buy_order_id == 11);
    }
    {
        OrderBook b(RiskLimits{100, 1, 1000000});
        auto r = b.submit(make(20, Side::Buy, 1000, 101));
        assert(!r.accepted && r.reason_code == 2);
    }
    {
        OrderBook b;
        b.submit(make(30, Side::Buy, 1000, 10));
        assert(b.cancel(30));
        assert(!b.best_bid());
    }
    {
        SpscRingBuffer<int, 8> q;
        for (int i = 0; i < 8; ++i) assert(q.try_push(i));
        assert(!q.try_push(9));
        for (int i = 0; i < 8; ++i) { int x=-1; assert(q.try_pop(x)); assert(x == i); }
    }
    std::cout << "all tests passed\n";
}
