#include "chronos/order_book.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <iomanip>
#include <iostream>
#include <random>
#include <vector>

using namespace chronos;
using Clock = std::chrono::steady_clock;

static double percentile(std::vector<std::uint64_t> xs, double p) {
    std::sort(xs.begin(), xs.end());
    const auto idx = static_cast<std::size_t>(std::ceil(p * xs.size())) - 1;
    return static_cast<double>(xs[std::min(idx, xs.size()-1)]);
}

int main(int argc, char** argv) {
    const std::size_t n = argc > 1 ? std::stoull(argv[1]) : 1000000;
    OrderBook book;
    std::vector<std::uint64_t> latencies;
    latencies.reserve(n);
    std::mt19937_64 rng(42);
    std::uniform_int_distribution<int> jitter(-50, 50);
    std::uint64_t trades = 0;

    const auto start = Clock::now();
    for (std::size_t i = 0; i < n; ++i) {
        const bool buy = (i & 1) == 0;
        // Cross frequently so the benchmark exercises both insertion and matching.
        const std::int64_t base = 100000;
        const std::int64_t price = base + jitter(rng) + (buy ? 25 : -25);
        Order o{static_cast<std::uint64_t>(i + 1), 1, symbol_from_string("AAPL"), buy ? Side::Buy : Side::Sell,
                price, static_cast<std::uint32_t>(1 + (i % 20)), 0};
        const auto t0 = Clock::now();
        auto r = book.submit(o);
        const auto t1 = Clock::now();
        trades += r.trades.size();
        latencies.push_back(static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count()));
    }
    const auto end = Clock::now();
    const double sec = std::chrono::duration<double>(end - start).count();
    const double throughput = static_cast<double>(n) / sec;

    std::cout << std::fixed << std::setprecision(2)
              << "orders=" << n << '\n'
              << "trades=" << trades << '\n'
              << "seconds=" << sec << '\n'
              << "throughput_orders_per_sec=" << throughput << '\n'
              << "latency_p50_ns=" << percentile(latencies, 0.50) << '\n'
              << "latency_p95_ns=" << percentile(latencies, 0.95) << '\n'
              << "latency_p99_ns=" << percentile(latencies, 0.99) << '\n'
              << "resting_orders=" << book.resting_orders() << '\n';
}
