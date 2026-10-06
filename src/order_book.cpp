#include "chronos/order_book.hpp"

#include <algorithm>
#include <chrono>
#include <iterator>

namespace chronos {
namespace {

std::uint64_t now_ns() {
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now().time_since_epoch())
            .count());
}

} // namespace

OrderBook::OrderBook(RiskLimits limits) : limits_(limits) {}

bool OrderBook::validate(
    const Order& order,
    std::uint32_t& reason_code) const {
    if (order.id == 0) {
        reason_code = 1;
        return false;
    }
    if (order.quantity == 0 ||
        order.quantity > limits_.max_order_quantity) {
        reason_code = 2;
        return false;
    }
    if (order.price_ticks < limits_.min_price_ticks ||
        order.price_ticks > limits_.max_price_ticks) {
        reason_code = 3;
        return false;
    }
    if (index_.contains(order.id)) {
        reason_code = 4;
        return false;
    }
    return true;
}

SubmitResult OrderBook::submit(
    const Order& incoming,
    std::uint64_t timestamp_ns) {
    SubmitResult result;

    if (!validate(incoming, result.reason_code)) {
        return result;
    }

    result.accepted = true;
    Order order = incoming;

    if (timestamp_ns == 0) {
        timestamp_ns = now_ns();
    }

    if (order.side == Side::Buy) {
        while (order.quantity > 0 && !asks_.empty()) {
            auto level = asks_.begin();

            if (level->first > order.price_ticks) {
                break;
            }

            auto& queue = level->second;

            while (order.quantity > 0 && !queue.empty()) {
                auto resting_it = queue.begin();
                auto& resting = *resting_it;
                const auto quantity =
                    std::min(order.quantity, resting.quantity);

                result.trades.push_back(
                    Trade{
                        ++trade_sequence_,
                        order.id,
                        resting.id,
                        order.symbol,
                        resting.price_ticks,
                        quantity,
                        timestamp_ns});

                order.quantity -= quantity;
                resting.quantity -= quantity;

                if (resting.quantity == 0) {
                    index_.erase(resting.id);
                    queue.erase(resting_it);
                    --resting_orders_;
                }
            }

            if (queue.empty()) {
                asks_.erase(level);
            }
        }
    } else {
        while (order.quantity > 0 && !bids_.empty()) {
            auto level = bids_.begin();

            if (level->first < order.price_ticks) {
                break;
            }

            auto& queue = level->second;

            while (order.quantity > 0 && !queue.empty()) {
                auto resting_it = queue.begin();
                auto& resting = *resting_it;
                const auto quantity =
                    std::min(order.quantity, resting.quantity);

                result.trades.push_back(
                    Trade{
                        ++trade_sequence_,
                        resting.id,
                        order.id,
                        order.symbol,
                        resting.price_ticks,
                        quantity,
                        timestamp_ns});

                order.quantity -= quantity;
                resting.quantity -= quantity;

                if (resting.quantity == 0) {
                    index_.erase(resting.id);
                    queue.erase(resting_it);
                    --resting_orders_;
                }
            }

            if (queue.empty()) {
                bids_.erase(level);
            }
        }
    }

    if (order.quantity > 0) {
        rest(order);
    }

    return result;
}

void OrderBook::rest(Order order) {
    if (order.side == Side::Buy) {
        auto [level_it, inserted] =
            bids_.try_emplace(order.price_ticks);
        (void)inserted;

        level_it->second.push_back(order);
        auto order_it = std::prev(level_it->second.end());

        index_.emplace(
            order.id,
            Locator{
                order.side,
                order_it,
                BidLevelRef{level_it}});
    } else {
        auto [level_it, inserted] =
            asks_.try_emplace(order.price_ticks);
        (void)inserted;

        level_it->second.push_back(order);
        auto order_it = std::prev(level_it->second.end());

        index_.emplace(
            order.id,
            Locator{
                order.side,
                order_it,
                AskLevelRef{level_it}});
    }

    ++resting_orders_;
}

bool OrderBook::cancel(std::uint64_t order_id) {
    auto indexed = index_.find(order_id);

    if (indexed == index_.end()) {
        return false;
    }

    auto& locator = indexed->second;

    if (locator.side == Side::Buy) {
        auto level =
            std::get<BidLevelRef>(locator.level_ref).it;

        level->second.erase(locator.order_it);

        if (level->second.empty()) {
            bids_.erase(level);
        }
    } else {
        auto level =
            std::get<AskLevelRef>(locator.level_ref).it;

        level->second.erase(locator.order_it);

        if (level->second.empty()) {
            asks_.erase(level);
        }
    }

    index_.erase(indexed);
    --resting_orders_;
    return true;
}

std::optional<std::int64_t> OrderBook::best_bid() const {
    if (bids_.empty()) {
        return std::nullopt;
    }
    return bids_.begin()->first;
}

std::optional<std::int64_t> OrderBook::best_ask() const {
    if (asks_.empty()) {
        return std::nullopt;
    }
    return asks_.begin()->first;
}

std::size_t OrderBook::resting_orders() const noexcept {
    return resting_orders_;
}

} // namespace chronos
