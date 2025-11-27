#pragma once
#include <string>
#include <curl/curl.h>
#include <mutex>        // <-- wieder aktivieren!

class Nakamoto {
private:
    std::string rpcUser;
    std::string rpcPass;
    std::string rpcUrl;         // jetzt vorgebaut
    struct curl_slist* headers;
    CURL* client;
    std::mutex rpcMutex;        // ← das ist der Lebensretter

public:
    Nakamoto(const std::string& user, const std::string& pass,
             const std::string& host, int port);
    ~Nakamoto();
    std::string sendRpc(const std::string& jsonPayload);
};