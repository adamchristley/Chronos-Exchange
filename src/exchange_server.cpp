#include "chronos/cpu_affinity.hpp"
#include "chronos/event_log.hpp"
#include "chronos/order_book.hpp"
#include "chronos/protocol.hpp"
#include "chronos/spsc_ring_buffer.hpp"

#include <arpa/inet.h>
#include <array>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstddef>
#include <csignal>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <iostream>
#include <memory>
#include <netinet/tcp.h>
#include <string>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <thread>
#include <unordered_map>
#include <unistd.h>

using namespace chronos;

namespace {

std::atomic<bool> running{true};

void handle_signal(int) {
    running.store(false);
}

std::uint64_t now_ns() {
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now().time_since_epoch())
            .count());
}

struct Inbound {
    Order order;
    OrderType type{OrderType::New};
};

struct ClientState {
    std::array<std::byte, sizeof(WireOrder)> receive_buffer{};
    std::size_t bytes_used{0};
};

using OrderQueue = SpscRingBuffer<Inbound, 1u << 16>;
using MarketQueue = SpscRingBuffer<EngineEvent, 1u << 16>;

bool make_nonblocking(int fd) {
    const int flags = ::fcntl(fd, F_GETFL, 0);
    return flags >= 0 && ::fcntl(fd, F_SETFL, flags | O_NONBLOCK) == 0;
}

void close_client(int epoll_fd, int client_fd, std::unordered_map<int, ClientState>& clients) {
    ::epoll_ctl(epoll_fd, EPOLL_CTL_DEL, client_fd, nullptr);
    ::close(client_fd);
    clients.erase(client_fd);
}

void enqueue_order(OrderQueue& queue, Inbound inbound) {
    while (running.load(std::memory_order_relaxed) && !queue.try_push(inbound)) {
        std::this_thread::yield();
    }
}

void network_loop(int port, OrderQueue& queue, int pin_core) {
    if (pin_core >= 0) {
        pin_current_thread(pin_core);
    }

    const int listen_fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (listen_fd < 0) {
        std::perror("socket");
        std::exit(1);
    }

    int yes = 1;
    ::setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));

    if (!make_nonblocking(listen_fd)) {
        std::perror("fcntl");
        ::close(listen_fd);
        std::exit(1);
    }

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons(static_cast<std::uint16_t>(port));

    if (::bind(listen_fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0 ||
        ::listen(listen_fd, 128) < 0) {
        std::perror("listen/bind");
        ::close(listen_fd);
        std::exit(1);
    }

    const int epoll_fd = ::epoll_create1(EPOLL_CLOEXEC);
    if (epoll_fd < 0) {
        std::perror("epoll_create1");
        ::close(listen_fd);
        std::exit(1);
    }

    epoll_event listen_event{};
    listen_event.events = EPOLLIN;
    listen_event.data.fd = listen_fd;
    if (::epoll_ctl(epoll_fd, EPOLL_CTL_ADD, listen_fd, &listen_event) < 0) {
        std::perror("epoll_ctl listen");
        ::close(epoll_fd);
        ::close(listen_fd);
        std::exit(1);
    }

    std::unordered_map<int, ClientState> clients;
    std::array<epoll_event, 128> ready{};

    std::cout << "epoll TCP order gateway listening on port " << port << "\n";

    while (running.load(std::memory_order_relaxed)) {
        const int count =
            ::epoll_wait(epoll_fd, ready.data(), static_cast<int>(ready.size()), 100);

        if (count < 0) {
            if (errno == EINTR) {
                continue;
            }
            std::perror("epoll_wait");
            break;
        }

        for (int i = 0; i < count; ++i) {
            const int fd = ready[static_cast<std::size_t>(i)].data.fd;
            const std::uint32_t events = ready[static_cast<std::size_t>(i)].events;

            if (fd == listen_fd) {
                while (true) {
                    const int client_fd = ::accept(listen_fd, nullptr, nullptr);
                    if (client_fd < 0) {
                        if (errno == EAGAIN || errno == EWOULDBLOCK) {
                            break;
                        }
                        if (errno == EINTR) {
                            continue;
                        }
                        std::perror("accept");
                        break;
                    }

                    if (!make_nonblocking(client_fd)) {
                        ::close(client_fd);
                        continue;
                    }

                    int no_delay = 1;
                    ::setsockopt(
                        client_fd, IPPROTO_TCP, TCP_NODELAY, &no_delay, sizeof(no_delay));

                    epoll_event client_event{};
                    client_event.events = EPOLLIN | EPOLLRDHUP | EPOLLHUP | EPOLLERR;
                    client_event.data.fd = client_fd;

                    if (::epoll_ctl(
                            epoll_fd, EPOLL_CTL_ADD, client_fd, &client_event) < 0) {
                        ::close(client_fd);
                        continue;
                    }

                    clients.emplace(client_fd, ClientState{});
                }
                continue;
            }

            auto client = clients.find(fd);
            if (client == clients.end()) {
                continue;
            }

            bool should_close = (events & (EPOLLRDHUP | EPOLLHUP | EPOLLERR)) != 0;

            if ((events & EPOLLIN) != 0) {
                while (true) {
                    auto& state = client->second;
                    const std::size_t remaining =
                        sizeof(WireOrder) - state.bytes_used;

                    const ssize_t received = ::recv(
                        fd,
                        state.receive_buffer.data() + state.bytes_used,
                        remaining,
                        0);

                    if (received > 0) {
                        state.bytes_used += static_cast<std::size_t>(received);

                        if (state.bytes_used == sizeof(WireOrder)) {
                            WireOrder wire{};
                            std::memcpy(
                                &wire,
                                state.receive_buffer.data(),
                                sizeof(wire));
                            state.bytes_used = 0;

                            Inbound inbound{};
                            if (decode_order(wire, inbound.order, inbound.type)) {
                                if (inbound.order.timestamp_ns == 0) {
                                    inbound.order.timestamp_ns = now_ns();
                                }
                                enqueue_order(queue, inbound);
                            }
                        }
                        continue;
                    }

                    if (received == 0) {
                        should_close = true;
                        break;
                    }

                    if (errno == EINTR) {
                        continue;
                    }
                    if (errno == EAGAIN || errno == EWOULDBLOCK) {
                        break;
                    }

                    should_close = true;
                    break;
                }
            }

            if (should_close) {
                close_client(epoll_fd, fd, clients);
            }
        }
    }

    for (const auto& [fd, state] : clients) {
        (void)state;
        ::close(fd);
    }
    ::close(epoll_fd);
    ::close(listen_fd);
}

void matching_loop(
    OrderQueue& in,
    MarketQueue& out,
    const std::string& log_path,
    int pin_core) {
    if (pin_core >= 0) {
        pin_current_thread(pin_core);
    }

    std::unordered_map<std::string, OrderBook> books;
    EventLogWriter log(log_path);
    std::uint64_t event_seq = 0;
    Inbound msg{};

    while (running.load() || in.approximate_size() > 0) {
        if (!in.try_pop(msg)) {
            std::this_thread::yield();
            continue;
        }

        log.append_order(msg.order, msg.type, ++event_seq);

        if (msg.type == OrderType::Cancel) {
            auto key = symbol_to_string(msg.order.symbol);
            auto bit = books.find(key);
            const bool ok =
                bit != books.end() && bit->second.cancel(msg.order.id);

            EngineEvent event{
                ok ? EventType::Cancelled : EventType::Rejected,
                ++event_seq,
                msg.order.id,
                0,
                msg.order.symbol,
                msg.order.price_ticks,
                msg.order.quantity,
                now_ns(),
                ok ? 0u : 5u};

            log.append_event(event);
            while (running.load() && !out.try_push(event)) {
                std::this_thread::yield();
            }
            continue;
        }

        auto key = symbol_to_string(msg.order.symbol);
        const auto result =
            books.try_emplace(key).first->second.submit(msg.order, now_ns());

        EngineEvent accepted{
            result.accepted ? EventType::Accepted : EventType::Rejected,
            ++event_seq,
            msg.order.id,
            0,
            msg.order.symbol,
            msg.order.price_ticks,
            msg.order.quantity,
            now_ns(),
            result.reason_code};

        log.append_event(accepted);
        while (running.load() && !out.try_push(accepted)) {
            std::this_thread::yield();
        }

        for (const auto& trade : result.trades) {
            EngineEvent event{
                EventType::Trade,
                ++event_seq,
                trade.buy_order_id,
                trade.sell_order_id,
                trade.symbol,
                trade.price_ticks,
                trade.quantity,
                trade.timestamp_ns,
                0};

            log.append_event(event);
            while (running.load() && !out.try_push(event)) {
                std::this_thread::yield();
            }
        }

        if ((event_seq & 0x3ff) == 0) {
            log.flush();
        }
    }

    log.flush();
}

void market_data_loop(
    MarketQueue& queue,
    const std::string& host,
    int port,
    int pin_core) {
    if (pin_core >= 0) {
        pin_current_thread(pin_core);
    }

    const int fd = ::socket(AF_INET, SOCK_DGRAM, 0);
    sockaddr_in dest{};
    dest.sin_family = AF_INET;
    dest.sin_port = htons(static_cast<std::uint16_t>(port));
    ::inet_pton(AF_INET, host.c_str(), &dest.sin_addr);

    EngineEvent event{};
    while (running.load() || queue.approximate_size() > 0) {
        if (!queue.try_pop(event)) {
            std::this_thread::yield();
            continue;
        }

        const auto wire = encode_event(event);
        ::sendto(
            fd,
            &wire,
            sizeof(wire),
            0,
            reinterpret_cast<sockaddr*>(&dest),
            sizeof(dest));
    }

    ::close(fd);
}

} // namespace

int main(int argc, char** argv) {
    int tcp_port = 9000;
    int udp_port = 9100;
    std::string udp_host = "127.0.0.1";
    std::string log_path = "chronos.events";
    bool pin = false;

    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--tcp-port" && i + 1 < argc) {
            tcp_port = std::stoi(argv[++i]);
        } else if (arg == "--udp-port" && i + 1 < argc) {
            udp_port = std::stoi(argv[++i]);
        } else if (arg == "--udp-host" && i + 1 < argc) {
            udp_host = argv[++i];
        } else if (arg == "--log" && i + 1 < argc) {
            log_path = argv[++i];
        } else if (arg == "--pin") {
            pin = true;
        }
    }

    std::signal(SIGINT, handle_signal);
    std::signal(SIGTERM, handle_signal);

    auto inbound = std::make_unique<OrderQueue>();
    auto market = std::make_unique<MarketQueue>();

    std::thread network(
        network_loop, tcp_port, std::ref(*inbound), pin ? 1 : -1);
    std::thread matcher(
        matching_loop,
        std::ref(*inbound),
        std::ref(*market),
        log_path,
        pin ? 2 : -1);
    std::thread publisher(
        market_data_loop,
        std::ref(*market),
        udp_host,
        udp_port,
        pin ? 3 : -1);

    network.join();
    matcher.join();
    publisher.join();
}
