#include "chronos/cpu_affinity.hpp"
#include "chronos/event_log.hpp"
#include "chronos/order_book.hpp"
#include "chronos/protocol.hpp"
#include "chronos/spsc_ring_buffer.hpp"

#include <arpa/inet.h>
#include <atomic>
#include <chrono>
#include <csignal>
#include <cstring>
#include <iostream>
#include <memory>
#include <poll.h>
#include <string>
#include <sys/socket.h>
#include <thread>
#include <unordered_map>
#include <unistd.h>
#include <vector>

using namespace chronos;
namespace {
std::atomic<bool> running{true};
void handle_signal(int) { running.store(false); }

std::uint64_t now_ns() {
    return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count());
}

struct Inbound { Order order; OrderType type{OrderType::New}; };
using OrderQueue = SpscRingBuffer<Inbound, 1u << 16>;
using MarketQueue = SpscRingBuffer<EngineEvent, 1u << 16>;

void network_loop(int port, OrderQueue& queue, int pin_core) {
    if (pin_core >= 0) pin_current_thread(pin_core);
    int listen_fd = ::socket(AF_INET, SOCK_STREAM, 0);
    int yes = 1;
    setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons(static_cast<uint16_t>(port));
    if (::bind(listen_fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0 || ::listen(listen_fd, 128) < 0) {
        std::perror("listen/bind"); std::exit(1);
    }
    std::vector<pollfd> fds{{listen_fd, POLLIN, 0}};
    std::cout << "TCP order gateway listening on port " << port << "\n";

    while (running.load()) {
        const int rc = ::poll(fds.data(), fds.size(), 100);
        if (rc <= 0) continue;
        for (std::size_t i = 0; i < fds.size(); ++i) {
            if (!(fds[i].revents & POLLIN)) continue;
            if (fds[i].fd == listen_fd) {
                int client = ::accept(listen_fd, nullptr, nullptr);
                if (client >= 0) fds.push_back({client, POLLIN, 0});
                continue;
            }
            WireOrder wire{};
            const auto n = ::recv(fds[i].fd, &wire, sizeof(wire), MSG_WAITALL);
            if (n != static_cast<ssize_t>(sizeof(wire))) {
                ::close(fds[i].fd);
                fds.erase(fds.begin() + static_cast<long>(i));
                --i;
                continue;
            }
            Inbound in{};
            if (!decode_order(wire, in.order, in.type)) continue;
            if (in.order.timestamp_ns == 0) in.order.timestamp_ns = now_ns();
            while (running.load() && !queue.try_push(in)) std::this_thread::yield();
        }
    }
    for (auto& fd : fds) ::close(fd.fd);
}

void matching_loop(OrderQueue& in, MarketQueue& out, const std::string& log_path, int pin_core) {
    if (pin_core >= 0) pin_current_thread(pin_core);
    std::unordered_map<std::string, OrderBook> books;
    EventLogWriter log(log_path);
    std::uint64_t event_seq = 0;
    Inbound msg{};
    while (running.load() || in.approximate_size() > 0) {
        if (!in.try_pop(msg)) { std::this_thread::yield(); continue; }
        log.append_order(msg.order, msg.type, ++event_seq);
        if (msg.type == OrderType::Cancel) {
            auto key = symbol_to_string(msg.order.symbol);
            auto bit = books.find(key);
            const bool ok = bit != books.end() && bit->second.cancel(msg.order.id);
            EngineEvent e{ok ? EventType::Cancelled : EventType::Rejected, ++event_seq, msg.order.id, 0,
                          msg.order.symbol, msg.order.price_ticks, msg.order.quantity, now_ns(), ok ? 0u : 5u};
            log.append_event(e);
            while (running.load() && !out.try_push(e)) std::this_thread::yield();
            continue;
        }
        auto key = symbol_to_string(msg.order.symbol);
        const auto r = books.try_emplace(key).first->second.submit(msg.order, now_ns());
        EngineEvent accepted{r.accepted ? EventType::Accepted : EventType::Rejected, ++event_seq, msg.order.id, 0,
                             msg.order.symbol, msg.order.price_ticks, msg.order.quantity, now_ns(), r.reason_code};
        log.append_event(accepted);
        while (running.load() && !out.try_push(accepted)) std::this_thread::yield();
        for (const auto& t : r.trades) {
            EngineEvent e{EventType::Trade, ++event_seq, t.buy_order_id, t.sell_order_id, t.symbol,
                          t.price_ticks, t.quantity, t.timestamp_ns, 0};
            log.append_event(e);
            while (running.load() && !out.try_push(e)) std::this_thread::yield();
        }
        if ((event_seq & 0x3ff) == 0) log.flush();
    }
    log.flush();
}

void market_data_loop(MarketQueue& queue, const std::string& host, int port, int pin_core) {
    if (pin_core >= 0) pin_current_thread(pin_core);
    int fd = ::socket(AF_INET, SOCK_DGRAM, 0);
    sockaddr_in dest{};
    dest.sin_family = AF_INET;
    dest.sin_port = htons(static_cast<uint16_t>(port));
    inet_pton(AF_INET, host.c_str(), &dest.sin_addr);
    EngineEvent event{};
    while (running.load() || queue.approximate_size() > 0) {
        if (!queue.try_pop(event)) { std::this_thread::yield(); continue; }
        const auto wire = encode_event(event);
        ::sendto(fd, &wire, sizeof(wire), 0, reinterpret_cast<sockaddr*>(&dest), sizeof(dest));
    }
    ::close(fd);
}
}

int main(int argc, char** argv) {
    int tcp_port = 9000;
    int udp_port = 9100;
    std::string udp_host = "127.0.0.1";
    std::string log_path = "chronos.events";
    bool pin = false;
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        if (a == "--tcp-port" && i + 1 < argc) tcp_port = std::stoi(argv[++i]);
        else if (a == "--udp-port" && i + 1 < argc) udp_port = std::stoi(argv[++i]);
        else if (a == "--udp-host" && i + 1 < argc) udp_host = argv[++i];
        else if (a == "--log" && i + 1 < argc) log_path = argv[++i];
        else if (a == "--pin") pin = true;
    }
    std::signal(SIGINT, handle_signal);
    std::signal(SIGTERM, handle_signal);

    auto inbound = std::make_unique<OrderQueue>();
    auto market = std::make_unique<MarketQueue>();
    std::thread network(network_loop, tcp_port, std::ref(*inbound), pin ? 1 : -1);
    std::thread matcher(matching_loop, std::ref(*inbound), std::ref(*market), log_path, pin ? 2 : -1);
    std::thread publisher(market_data_loop, std::ref(*market), udp_host, udp_port, pin ? 3 : -1);

    network.join();
    matcher.join();
    publisher.join();
}
