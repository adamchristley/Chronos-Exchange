#include "chronos/protocol.hpp"

#include <arpa/inet.h>
#include <iostream>
#include <sys/socket.h>
#include <unistd.h>

using namespace chronos;

int main(int argc, char** argv) {
    const int port = argc > 1 ? std::stoi(argv[1]) : 9100;
    int fd = ::socket(AF_INET, SOCK_DGRAM, 0);
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons(static_cast<uint16_t>(port));
    if (::bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) { std::perror("bind"); return 1; }
    std::cout << "listening for market data on UDP " << port << "\n";
    while (true) {
        WireMarketEvent wire{};
        const auto n = ::recv(fd, &wire, sizeof(wire), 0);
        if (n != static_cast<ssize_t>(sizeof(wire))) continue;
        EngineEvent e{};
        if (!decode_event(wire, e)) continue;
        std::cout << "seq=" << e.sequence << " type=" << static_cast<int>(e.type)
                  << " order=" << e.order_id << " contra=" << e.contra_order_id
                  << " symbol=" << symbol_to_string(e.symbol)
                  << " price=" << e.price_ticks << " qty=" << e.quantity
                  << " reason=" << e.reason_code << '\n';
    }
}
