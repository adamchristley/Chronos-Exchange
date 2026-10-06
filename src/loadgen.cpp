#include "chronos/protocol.hpp"

#include <algorithm>
#include <cerrno>
#include <cstddef>
#include <arpa/inet.h>
#include <atomic>
#include <barrier>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <iomanip>
#include <iostream>
#include <memory>
#include <mutex>
#include <netinet/tcp.h>
#include <stdexcept>
#include <string>
#include <sys/socket.h>
#include <thread>
#include <unistd.h>
#include <vector>

using namespace chronos;

namespace {

using Clock = std::chrono::steady_clock;

std::uint64_t now_ns() {
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            Clock::now().time_since_epoch())
            .count());
}

bool send_all(int fd, const void* data, std::size_t size) {
    const auto* bytes = static_cast<const std::byte*>(data);
    std::size_t sent = 0;

    while (sent < size) {
        const ssize_t n =
            ::send(fd, bytes + sent, size - sent, MSG_NOSIGNAL);
        if (n > 0) {
            sent += static_cast<std::size_t>(n);
            continue;
        }
        if (n < 0 && errno == EINTR) {
            continue;
        }
        return false;
    }

    return true;
}

int connect_client(const std::string& host, int port) {
    const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        return -1;
    }

    int no_delay = 1;
    ::setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &no_delay, sizeof(no_delay));

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(static_cast<std::uint16_t>(port));

    if (::inet_pton(AF_INET, host.c_str(), &addr.sin_addr) != 1 ||
        ::connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
        ::close(fd);
        return -1;
    }

    return fd;
}

int bind_market_data_socket(int port) {
    const int fd = ::socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) {
        return -1;
    }

    int receive_buffer = 32 * 1024 * 1024;
    ::setsockopt(
        fd,
        SOL_SOCKET,
        SO_RCVBUF,
        &receive_buffer,
        sizeof(receive_buffer));

    timeval timeout{};
    timeout.tv_sec = 0;
    timeout.tv_usec = 100000;
    ::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons(static_cast<std::uint16_t>(port));

    if (::bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
        ::close(fd);
        return -1;
    }

    return fd;
}

double percentile_us(
    const std::vector<std::uint64_t>& sorted_ns,
    double percentile) {
    if (sorted_ns.empty()) {
        return 0.0;
    }

    const auto index = static_cast<std::size_t>(
        std::ceil(percentile * static_cast<double>(sorted_ns.size()))) - 1;

    const auto clamped = std::min(index, sorted_ns.size() - 1);
    return static_cast<double>(sorted_ns[clamped]) / 1000.0;
}

struct Config {
    std::string host{"127.0.0.1"};
    int tcp_port{9000};
    int udp_port{9100};
    std::size_t clients{8};
    std::size_t orders{100000};
    std::uint64_t timeout_ms{3000};
};

Config parse_args(int argc, char** argv) {
    Config config;

    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];

        if (arg == "--host" && i + 1 < argc) {
            config.host = argv[++i];
        } else if (arg == "--tcp-port" && i + 1 < argc) {
            config.tcp_port = std::stoi(argv[++i]);
        } else if (arg == "--udp-port" && i + 1 < argc) {
            config.udp_port = std::stoi(argv[++i]);
        } else if (arg == "--clients" && i + 1 < argc) {
            config.clients = std::stoull(argv[++i]);
        } else if (arg == "--orders" && i + 1 < argc) {
            config.orders = std::stoull(argv[++i]);
        } else if (arg == "--timeout-ms" && i + 1 < argc) {
            config.timeout_ms = std::stoull(argv[++i]);
        } else if (arg == "--help") {
            std::cout
                << "usage: chronos_loadgen [--host HOST] [--tcp-port PORT] "
                   "[--udp-port PORT] [--clients N] [--orders N] "
                   "[--timeout-ms MS]\n";
            std::exit(0);
        }
    }

    if (config.clients == 0 || config.orders == 0) {
        throw std::runtime_error("clients and orders must both be nonzero");
    }

    return config;
}

} // namespace

int main(int argc, char** argv) {
    Config config;

    try {
        config = parse_args(argc, argv);
    } catch (const std::exception& error) {
        std::cerr << "argument error: " << error.what() << "\n";
        return 1;
    }

    const int market_fd = bind_market_data_socket(config.udp_port);
    if (market_fd < 0) {
        std::perror("bind market-data socket");
        return 1;
    }

    std::vector<int> client_fds;
    client_fds.reserve(config.clients);

    for (std::size_t i = 0; i < config.clients; ++i) {
        const int fd = connect_client(config.host, config.tcp_port);
        if (fd < 0) {
            std::cerr << "failed to connect client " << i
                      << " to " << config.host << ":" << config.tcp_port << "\n";
            for (const int open_fd : client_fds) {
                ::close(open_fd);
            }
            ::close(market_fd);
            return 1;
        }
        client_fds.push_back(fd);
    }

    const std::uint64_t base_order_id = now_ns();
    auto send_times =
        std::make_unique<std::atomic<std::uint64_t>[]>(config.orders);
    for (std::size_t i = 0; i < config.orders; ++i) {
        send_times[i].store(0, std::memory_order_relaxed);
    }

    std::atomic<std::uint64_t> senders_done_ns{0};
    std::atomic<std::size_t> send_failures{0};
    std::vector<std::uint64_t> latencies_ns;
    latencies_ns.reserve(config.orders);
    std::vector<std::uint8_t> seen(config.orders, 0);

    std::size_t acknowledgements = 0;
    std::size_t accepted = 0;
    std::size_t rejected = 0;
    std::uint64_t last_ack_ns = 0;

    std::thread receiver([&] {
        while (true) {
            WireMarketEvent wire{};
            const ssize_t n = ::recv(market_fd, &wire, sizeof(wire), 0);

            if (n == static_cast<ssize_t>(sizeof(wire))) {
                EngineEvent event{};
                if (!decode_event(wire, event)) {
                    continue;
                }

                if (event.type != EventType::Accepted &&
                    event.type != EventType::Rejected) {
                    continue;
                }

                if (event.order_id < base_order_id) {
                    continue;
                }

                const std::uint64_t offset = event.order_id - base_order_id;
                if (offset >= config.orders) {
                    continue;
                }

                const auto index = static_cast<std::size_t>(offset);
                if (seen[index] != 0) {
                    continue;
                }

                const std::uint64_t sent =
                    send_times[index].load(std::memory_order_acquire);
                if (sent == 0) {
                    continue;
                }

                const std::uint64_t received = now_ns();
                if (received >= sent) {
                    latencies_ns.push_back(received - sent);
                }

                seen[index] = 1;
                ++acknowledgements;
                if (event.type == EventType::Accepted) {
                    ++accepted;
                } else {
                    ++rejected;
                }
                last_ack_ns = received;

                if (acknowledgements == config.orders) {
                    break;
                }

                continue;
            }

            const std::uint64_t done =
                senders_done_ns.load(std::memory_order_acquire);
            if (done != 0 &&
                now_ns() - done >= config.timeout_ms * 1000000ULL) {
                break;
            }
        }
    });

    std::barrier<> start_line(
        static_cast<std::ptrdiff_t>(config.clients + 1));

    std::vector<std::thread> senders;
    senders.reserve(config.clients);

    for (std::size_t client_index = 0;
         client_index < config.clients;
         ++client_index) {
        senders.emplace_back([&, client_index] {
            const int fd = client_fds[client_index];
            start_line.arrive_and_wait();

            for (std::size_t index = client_index;
                 index < config.orders;
                 index += config.clients) {
                const Side side =
                    (index & 1U) == 0 ? Side::Buy : Side::Sell;

                Order order{};
                order.id = base_order_id + index;
                order.client_id =
                    static_cast<std::uint32_t>(client_index + 1);
                order.symbol = symbol_from_string("AAPL");
                order.side = side;
                order.price_ticks =
                    side == Side::Buy ? 100025 : 99975;
                order.quantity = 1;
                order.timestamp_ns = now_ns();

                const auto wire = encode_order(order);
                send_times[index].store(
                    now_ns(), std::memory_order_release);

                if (!send_all(fd, &wire, sizeof(wire))) {
                    ++send_failures;
                    break;
                }
            }
        });
    }

    const std::uint64_t start_ns = now_ns();
    start_line.arrive_and_wait();

    for (auto& sender : senders) {
        sender.join();
    }

    const std::uint64_t send_done = now_ns();
    senders_done_ns.store(send_done, std::memory_order_release);

    receiver.join();

    for (const int fd : client_fds) {
        ::close(fd);
    }
    ::close(market_fd);

    std::sort(latencies_ns.begin(), latencies_ns.end());

    const double send_seconds =
        static_cast<double>(send_done - start_ns) / 1e9;
    const double send_rate =
        send_seconds > 0.0
            ? static_cast<double>(config.orders) / send_seconds
            : 0.0;

    const double ack_seconds =
        last_ack_ns > start_ns
            ? static_cast<double>(last_ack_ns - start_ns) / 1e9
            : 0.0;
    const double ack_rate =
        ack_seconds > 0.0
            ? static_cast<double>(acknowledgements) / ack_seconds
            : 0.0;

    const std::size_t missing =
        config.orders > acknowledgements
            ? config.orders - acknowledgements
            : 0;

    const double max_us =
        latencies_ns.empty()
            ? 0.0
            : static_cast<double>(latencies_ns.back()) / 1000.0;

    std::cout << std::fixed << std::setprecision(2)
              << "clients=" << config.clients << '\n'
              << "orders_requested=" << config.orders << '\n'
              << "send_failures=" << send_failures.load() << '\n'
              << "acks_received=" << acknowledgements << '\n'
              << "accepted=" << accepted << '\n'
              << "rejected=" << rejected << '\n'
              << "acks_missing_or_dropped=" << missing << '\n'
              << "send_duration_sec=" << send_seconds << '\n'
              << "send_rate_orders_per_sec=" << send_rate << '\n'
              << "ack_window_sec=" << ack_seconds << '\n'
              << "observed_end_to_end_orders_per_sec=" << ack_rate << '\n'
              << "latency_p50_us="
              << percentile_us(latencies_ns, 0.50) << '\n'
              << "latency_p95_us="
              << percentile_us(latencies_ns, 0.95) << '\n'
              << "latency_p99_us="
              << percentile_us(latencies_ns, 0.99) << '\n'
              << "latency_p99_9_us="
              << percentile_us(latencies_ns, 0.999) << '\n'
              << "latency_max_us=" << max_us << '\n';

    return send_failures.load() == 0 && missing == 0 ? 0 : 2;
}
