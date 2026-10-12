#pragma once
#include <string>
#include <cstdint>
#include <vector>
#include <curl/curl.h>
#include <mutex>        // Required for RPC request synchronization.

class Nakamoto {
private:
    std::string rpcUser;
    std::string rpcPass;
    std::string rpcUrl;         // Prebuilt RPC endpoint URL.
    struct curl_slist* headers;
    CURL* client;
    std::mutex rpcMutex;        // Protects the shared CURL handle.

public:
    Nakamoto(const std::string& user, const std::string& pass,
             const std::string& host, int port);
    ~Nakamoto();
    Nakamoto(const Nakamoto&) = delete;
    Nakamoto& operator=(const Nakamoto&) = delete;
    std::string sendRpc(const std::string& jsonPayload, std::uint64_t traceId = 0);
    bool probe();
    std::vector<std::string> discoverCommands();
    const std::string& endpoint() const { return rpcUrl; }
};
