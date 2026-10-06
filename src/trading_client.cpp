#include "chronos/protocol.hpp"

#include <arpa/inet.h>
#include <chrono>
#include <iostream>
#include <string>
#include <sys/socket.h>
#include <unistd.h>

using namespace chronos;

static std::uint64_t now_ns() {
    return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count());
}

int main(int argc, char** argv) {
    if (argc < 7) {
        std::cerr << "usage: chronos_client HOST PORT BUY|SELL SYMBOL PRICE_TICKS QTY [COUNT]\n";
        return 1;
    }
    const std::string host = argv[1];
    const int port = std::stoi(argv[2]);
    const Side side = std::string(argv[3]) == "BUY" ? Side::Buy : Side::Sell;
    const auto symbol = symbol_from_string(argv[4]);
    const auto price = std::stoll(argv[5]);
    const auto qty = static_cast<std::uint32_t>(std::stoul(argv[6]));
    const int count = argc > 7 ? std::stoi(argv[7]) : 1;

    int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(static_cast<uint16_t>(port));
    if (inet_pton(AF_INET, host.c_str(), &addr.sin_addr) != 1 || ::connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
        std::perror("connect"); return 1;
    }
    const auto seed = now_ns();
    for (int i = 0; i < count; ++i) {
        Order o{seed + static_cast<std::uint64_t>(i), 1, symbol, side, price, qty, now_ns()};
        const auto w = encode_order(o);
        if (::send(fd, &w, sizeof(w), MSG_NOSIGNAL) != static_cast<ssize_t>(sizeof(w))) {
            std::perror("send"); return 1;
        }
    }
    std::cout << "sent " << count << " order(s)\n";
    ::close(fd);
}
