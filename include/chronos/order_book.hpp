#pragma once

#include "chronos/types.hpp"

#include <deque>
#include <functional>
#include <map>
#include <optional>
#include <unordered_map>
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
    [[nodiscard]] std::uint64_t trade_sequence() const noexcept { return trade_sequence_; }

private:
    using BidLevels = std::map<std::int64_t, std::deque<Order>, std::greater<std::int64_t>>;
    using AskLevels = std::map<std::int64_t, std::deque<Order>, std::less<std::int64_t>>;

    struct Locator { Side side; std::int64_t price_ticks; };

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
