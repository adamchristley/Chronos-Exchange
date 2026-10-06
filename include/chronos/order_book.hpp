#pragma once

#include "chronos/types.hpp"

#include <functional>
#include <list>
#include <map>
#include <optional>
#include <unordered_map>
#include <variant>
#include <vector>

namespace chronos {

struct RiskLimits {
    std::uint32_t max_order_quantity{100000};
    std::int64_t min_price_ticks{1};
    std::int64_t max_price_ticks{100000000};
};

struct SubmitResult {
    bool accepted{false};
    std::uint32_t reason_code{0};
    std::vector<Trade> trades;
};

class OrderBook {
public:
    explicit OrderBook(RiskLimits limits = {});

    SubmitResult submit(const Order& order, std::uint64_t timestamp_ns = 0);
    bool cancel(std::uint64_t order_id);

    [[nodiscard]] std::optional<std::int64_t> best_bid() const;
    [[nodiscard]] std::optional<std::int64_t> best_ask() const;
    [[nodiscard]] std::size_t resting_orders() const noexcept;
    [[nodiscard]] std::uint64_t trade_sequence() const noexcept {
        return trade_sequence_;
    }

private:
    // std::list gives each resting order a stable iterator. The order-id index
    // stores both that iterator and the owning price-level iterator, so a
    // cancellation can remove an order without scanning the FIFO queue.
    using LevelQueue = std::list<Order>;
    using BidLevels =
        std::map<std::int64_t, LevelQueue, std::greater<std::int64_t>>;
    using AskLevels =
        std::map<std::int64_t, LevelQueue, std::less<std::int64_t>>;
    using BidLevelIterator = BidLevels::iterator;
    using AskLevelIterator = AskLevels::iterator;

    // libstdc++ may give bid/ask map iterators the same concrete type even
    // though the maps use different comparators. Distinct wrappers keep the
    // variant alternatives unambiguous while retaining iterator-based erase.
    struct BidLevelRef {
        BidLevelIterator it;
    };

    struct AskLevelRef {
        AskLevelIterator it;
    };

    struct Locator {
        Side side;
        LevelQueue::iterator order_it;
        std::variant<BidLevelRef, AskLevelRef> level_ref;
    };

    RiskLimits limits_;
    BidLevels bids_;
    AskLevels asks_;
    std::unordered_map<std::uint64_t, Locator> index_;
    std::size_t resting_orders_{0};
    std::uint64_t trade_sequence_{0};

    bool validate(const Order& order, std::uint32_t& reason_code) const;
    void rest(Order order);
};

} // namespace chronos
