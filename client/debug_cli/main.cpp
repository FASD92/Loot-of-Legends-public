#include "debug_cli/DebugCli.hpp"

#include <iostream>
#include <string>

int main() {
    Client::DebugCli cli;

    std::cout << "Loot of Legends Debug CLI\n";
    std::cout << "type 'help' for commands\n";

    std::string line;
    while (true) {
        std::cout << "> ";
        if (!std::getline(std::cin, line)) {
            break;
        }

        const Client::DebugCliResult result = cli.executeLine(line);
        if (!result.output.empty()) {
            std::cout << result.output << "\n";
        }
        if (result.shouldQuit) {
            break;
        }
    }

    return 0;
}
