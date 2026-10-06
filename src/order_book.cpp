#include "chronos/order_book.hpp"

#include <algorithm>
#include <chrono>

namespace chronos {
namespace {
std::uint64_t now_ns() {
    return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count());
}
}

OrderBook::OrderBook(RiskLimits limits) : limits_(limits) {}

bool OrderBook::validate(const Order& order, std::uint32_t& reason_code) const {
    if (order.id == 0) { reason_code = 1; return false; }
    if (order.quantity == 0 || order.quantity > limits_.max_order_quantity) { reason_code = 2; return false; }
    if (order.price_ticks < limits_.min_price_ticks || order.price_ticks > limits_.max_price_ticks) { reason_code = 3; return false; }
    if (index_.contains(order.id)) { reason_code = 4; return false; }
    return true;
}

SubmitResult OrderBook::submit(const Order& incoming, std::uint64_t timestamp_ns) {
    SubmitResult result;
    if (!validate(incoming, result.reason_code)) return result;
    result.accepted = true;

    Order order = incoming;
    if (timestamp_ns == 0) timestamp_ns = now_ns();

    if (order.side == Side::Buy) {
        while (order.quantity > 0 && !asks_.empty()) {
            auto level = asks_.begin();
            if (level->first > order.price_ticks) break;
            auto& queue = level->second;
            while (order.quantity > 0 && !queue.empty()) {
                auto& resting = queue.front();
                const auto qty = std::min(order.quantity, resting.quantity);
                result.trades.push_back(Trade{++trade_sequence_, order.id, resting.id, order.symbol,
                                              resting.price_ticks, qty, timestamp_ns});
                order.quantity -= qty;
                resting.quantity -= qty;
                if (resting.quantity == 0) {
                    index_.erase(resting.id);
                    queue.pop_front();
                    --resting_orders_;
                }
            }
            if (queue.empty()) asks_.erase(level);
        }
    } else {
        while (order.quantity > 0 && !bids_.empty()) {
            auto level = bids_.begin();
            if (level->first < order.price_ticks) break;
            auto& queue = level->second;
            while (order.quantity > 0 && !queue.empty()) {
                auto& resting = queue.front();
                const auto qty = std::min(order.quantity, resting.quantity);
                result.trades.push_back(Trade{++trade_sequence_, resting.id, order.id, order.symbol,
                                              resting.price_ticks, qty, timestamp_ns});
                order.quantity -= qty;
                resting.quantity -= qty;
                if (resting.quantity == 0) {
                    index_.erase(resting.id);
                    queue.pop_front();
                    --resting_orders_;
                }
            }
            if (queue.empty()) bids_.erase(level);
        }
    }

    if (order.quantity > 0) rest(order);
    return result;
}

void OrderBook::rest(Order order) {
    if (order.side == Side::Buy) bids_[order.price_ticks].push_back(order);
    else asks_[order.price_ticks].push_back(order);
    index_[order.id] = Locator{order.side, order.price_ticks};
    ++resting_orders_;
}

bool OrderBook::cancel(std::uint64_t order_id) {
    auto it = index_.find(order_id);
    if (it == index_.end()) return false;
    const auto loc = it->second;

    auto erase_from = [&](auto& levels) {
        auto level = levels.find(loc.price_ticks);
        if (level == levels.end()) return false;
        auto& q = level->second;
        auto order_it = std::find_if(q.begin(), q.end(), [&](const Order& o){ return o.id == order_id; });
        if (order_it == q.end()) return false;
        q.erase(order_it);
        if (q.empty()) levels.erase(level);
        return true;
    };

    const bool removed = loc.side == Side::Buy ? erase_from(bids_) : erase_from(asks_);
    if (removed) {
        index_.erase(it);
        --resting_orders_;
    }
    return removed;
}

std::optional<std::int64_t> OrderBook::best_bid() const {
    if (bids_.empty()) return std::nullopt;
    return bids_.begin()->first;
}

std::optional<std::int64_t> OrderBook::best_ask() const {
    if (asks_.empty()) return std::nullopt;
    return asks_.begin()->first;
}

std::size_t OrderBook::resting_orders() const noexcept { return resting_orders_; }

} // namespace chronos
