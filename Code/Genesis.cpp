#include "ApiServer.hpp"
#include <filesystem>
#include <iostream>
#include <limits.h>
#include <unistd.h>

namespace {
std::filesystem::path executablePath(const char* argv0) {
    char resolved[PATH_MAX];
    const auto length = readlink("/proc/self/exe", resolved, sizeof(resolved) - 1);
    if (length > 0) {
        resolved[length] = '\0';
        return std::filesystem::path(resolved);
    }
    return std::filesystem::absolute(argv0);
}
}

int main(int argc, char** argv) {
    std::error_code pathError;
    const auto configPath = argc > 1
        ? std::filesystem::absolute(std::filesystem::path(argv[1]), pathError)
        : executablePath(argv[0]).parent_path() / "Lilith.conf";
    std::cerr << "[Startup] Config file: "
              << (pathError ? std::filesystem::path(argv[1]).string() : configPath.string()) << '\n'
              << "[Startup] Build: multi-node-rpc-help-ui\n";
    ApiServer API;
    if (!API.loadConfig(configPath.string())) return 1;
    API.start();
    return 0;
}
