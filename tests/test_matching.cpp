#include "chronos/order_book.hpp"
#include "chronos/spsc_ring_buffer.hpp"

#include <cassert>
#include <cstdint>
#include <iostream>

using namespace chronos;

static Order make(
    std::uint64_t id,
    Side side,
    std::int64_t price,
    std::uint32_t quantity) {
    return Order{
        id,
        1,
        symbol_from_string("AAPL"),
        side,
        price,
        quantity,
        0};
}

int main() {
    {
        OrderBook book;
        assert(book.submit(make(1, Side::Buy, 10000, 10)).accepted);
        assert(book.submit(make(2, Side::Buy, 10100, 10)).accepted);

        auto result = book.submit(make(3, Side::Sell, 9900, 12));

        assert(result.trades.size() == 2);
        assert(result.trades[0].buy_order_id == 2);
        assert(result.trades[0].price_ticks == 10100);
        assert(result.trades[0].quantity == 10);
        assert(result.trades[1].buy_order_id == 1);
        assert(result.trades[1].quantity == 2);
        assert(book.best_bid() && *book.best_bid() == 10000);
    }

    {
        OrderBook book;
        book.submit(make(10, Side::Buy, 10000, 5));
        book.submit(make(11, Side::Buy, 10000, 5));

        auto result =
            book.submit(make(12, Side::Sell, 10000, 6));

        assert(result.trades.size() == 2);
        assert(result.trades[0].buy_order_id == 10);
        assert(result.trades[1].buy_order_id == 11);
    }

    {
        OrderBook book(RiskLimits{100, 1, 1000000});

        auto result =
            book.submit(make(20, Side::Buy, 1000, 101));

        assert(!result.accepted);
        assert(result.reason_code == 2);
    }

    {
        OrderBook book;
        book.submit(make(30, Side::Buy, 1000, 10));

        assert(book.cancel(30));
        assert(!book.best_bid());
        assert(!book.cancel(30));
    }

    {
        // Exercise cancellation in the middle of a large FIFO price level.
        // The implementation should remove the indexed list node directly,
        // rather than scanning all orders at that price.
        OrderBook book;

        for (std::uint64_t id = 1000; id < 1100; ++id) {
            assert(book.submit(make(id, Side::Buy, 10000, 5)).accepted);
        }

        assert(book.resting_orders() == 100);
        assert(book.cancel(1050));
        assert(book.resting_orders() == 99);

        auto result =
            book.submit(make(2000, Side::Sell, 10000, 495));

        assert(result.trades.size() == 99);
        for (const auto& trade : result.trades) {
            assert(trade.buy_order_id != 1050);
        }
        assert(book.resting_orders() == 0);
        assert(!book.best_bid());
    }

    {
        // Removing the final order at a price must also remove that level,
        // while preserving other levels and their iterators.
        OrderBook book;
        book.submit(make(3000, Side::Buy, 10100, 1));
        book.submit(make(3001, Side::Buy, 10000, 1));
        book.submit(make(3002, Side::Buy, 9900, 1));

        assert(book.cancel(3000));
        assert(book.best_bid() && *book.best_bid() == 10000);

        assert(book.cancel(3001));
        assert(book.best_bid() && *book.best_bid() == 9900);

        assert(book.cancel(3002));
        assert(!book.best_bid());
    }

    {
        SpscRingBuffer<int, 8> queue;

        for (int i = 0; i < 8; ++i) {
            assert(queue.try_push(i));
        }

        assert(!queue.try_push(9));

        for (int i = 0; i < 8; ++i) {
            int value = -1;
            assert(queue.try_pop(value));
            assert(value == i);
        }
    }

    std::cout << "all tests passed\n";
}
