#include <cstdint>
#include <cstdlib>
#include <chrono>
#include <iostream>
#include <limits>
#include <memory>
#include <string>
#include <string_view>
#include <utility>

#include "Core/LocalDevMetaSessionClaimClient.hpp"
#include "Core/MetaHttpSessionClaimClient.hpp"
#include "Core/MetaSessionClaimClient.hpp"
#include "Core/Server.hpp"

namespace {
constexpr std::string_view kLocalDevAuthEnv = "LOL_RELEASE0_DEV_AUTH";
constexpr std::string_view kMetaBaseUrlEnv = "META_BASE_URL";
constexpr std::string_view kMetaInternalTokenEnv = "META_INTERNAL_TOKEN";

struct ServerRuntimeOptions {
    uint16_t port{40000};
    std::string metricsTextfilePath;
    std::chrono::milliseconds metricsInterval{1000};
};

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

bool parsePositiveMilliseconds(
    const char* value,
    std::chrono::milliseconds& outInterval) {
    if (!value) {
        return false;
    }

    char* end = nullptr;
    unsigned long milliseconds = std::strtoul(value, &end, 10);
    if (end == value || *end != '\0' || milliseconds == 0) {
        return false;
    }
    if (milliseconds >
        static_cast<unsigned long>(
            std::numeric_limits<std::chrono::milliseconds::rep>::max())) {
        return false;
    }

    outInterval = std::chrono::milliseconds(
        static_cast<std::chrono::milliseconds::rep>(milliseconds));
    return true;
}

bool localDevAuthEnabled() {
    const char* value = std::getenv(kLocalDevAuthEnv.data());
    return value != nullptr && std::string_view(value) == "1";
}

bool hasText(const char* value) {
    return value != nullptr && std::string_view(value).find_first_not_of(" \t\r\n") !=
        std::string_view::npos;
}

bool isFlag(std::string_view value) {
    return value.rfind("--", 0) == 0;
}

bool parseCommandLine(
    int argc,
    char** argv,
    ServerRuntimeOptions& options,
    std::string& errorMessage) {
    bool portSpecified = false;
    bool metricsIntervalSpecified = false;

    for (int index = 1; index < argc; ++index) {
        const std::string_view arg(argv[index] != nullptr ? argv[index] : "");
        if (arg == "--metrics-textfile") {
            if (index + 1 >= argc || !hasText(argv[index + 1]) ||
                isFlag(argv[index + 1])) {
                errorMessage = "Invalid --metrics-textfile path.";
                return false;
            }
            options.metricsTextfilePath = argv[++index];
            continue;
        }

        if (arg == "--metrics-interval-ms") {
            if (index + 1 >= argc ||
                !parsePositiveMilliseconds(argv[index + 1], options.metricsInterval)) {
                errorMessage = "Invalid --metrics-interval-ms value.";
                return false;
            }
            ++index;
            metricsIntervalSpecified = true;
            continue;
        }

        if (isFlag(arg)) {
            errorMessage = "Unknown option: " + std::string(arg);
            return false;
        }

        if (portSpecified || !parsePort(argv[index], options.port)) {
            errorMessage = "Invalid port.";
            return false;
        }
        portSpecified = true;
    }

    if (metricsIntervalSpecified && options.metricsTextfilePath.empty()) {
        errorMessage = "--metrics-interval-ms requires --metrics-textfile.";
        return false;
    }

    return true;
}

int runServer(
    const ServerRuntimeOptions& options,
    Core::IMetaSessionClaimClient* claimClient) {
    Core::Server server(options.port, claimClient);
    if (!options.metricsTextfilePath.empty() &&
        !server.configureRelease1MetricsTextfile(
            options.metricsTextfilePath,
            options.metricsInterval)) {
        std::cerr << "Invalid metrics textfile options.\n";
        return 1;
    }
    if (!server.start()) {
        std::cerr << "Failed to start server.\n";
        return 1;
    }

    server.run();
    return 0;
}
}  // namespace

int main(int argc, char** argv) {
    ServerRuntimeOptions options;
    std::string commandLineError;
    if (!parseCommandLine(argc, argv, options, commandLineError)) {
        std::cerr << commandLineError << '\n';
        return 1;
    }

    std::unique_ptr<Core::LocalDevMetaSessionClaimClient> localDevClaimClient;
    std::unique_ptr<Core::MetaHttpSessionClaimClient> metaHttpClaimClient;
    Core::IMetaSessionClaimClient* claimClient = nullptr;
    if (localDevAuthEnabled()) {
        localDevClaimClient = std::make_unique<Core::LocalDevMetaSessionClaimClient>();
        claimClient = localDevClaimClient.get();
        std::cerr << "Local dev game-session auth enabled.\n";
    } else if (hasText(std::getenv(kMetaBaseUrlEnv.data())) &&
               hasText(std::getenv(kMetaInternalTokenEnv.data()))) {
        Core::MetaHttpEndpoint endpoint;
        if (!Core::parseMetaHttpEndpoint(
                std::getenv(kMetaBaseUrlEnv.data()),
                endpoint)) {
            std::cerr << "Invalid META_BASE_URL.\n";
            return 1;
        }
        metaHttpClaimClient = std::make_unique<Core::MetaHttpSessionClaimClient>(
            std::move(endpoint),
            std::getenv(kMetaInternalTokenEnv.data()));
        claimClient = metaHttpClaimClient.get();
        std::cerr << "Meta HTTP game-session auth enabled.\n";
    }

    return runServer(options, claimClient);
}
