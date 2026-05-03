#include <cstdint>
#include <cstdlib>
#include <iostream>

#include "Core/Server.hpp"

namespace {
bool parsePort(const char* value, uint16_t& outPort) {
    if (!value) {
        return false;
    }

    char* end = nullptr;
    unsigned long port = std::strtoul(value, &end, 10);
    if (end == value || *end != '\0' || port == 0 || port > 65535) {
        return false;
    }

    outPort = static_cast<uint16_t>(port);
    return true;
}
}  // namespace

int main(int argc, char** argv) {
    uint16_t port = 40000;
    if (argc > 1 && !parsePort(argv[1], port)) {
        std::cerr << "Invalid port.\n";
        return 1;
    }

    Core::Server server(port);
    if (!server.start()) {
        std::cerr << "Failed to start server.\n";
        return 1;
    }

    server.run();
    return 0;
}
