// ApiServer.hpp
#pragma once
#include <unordered_map>
#include <memory>
#include <string>
#include <atomic>
#include <nlohmann/json.hpp>

class Nakamoto; // vorwärtsdeklaration

class ApiServer {
    std::atomic<bool> running{false};
    int port = 10666;
    int listen_sock = -1;
    std::unordered_map<std::string, std::unique_ptr<Nakamoto>> nodes;

    void handle_client(int client);
    void run_loop();

public:
    ApiServer();
    ~ApiServer();
    void start();      // blockiert
    void stop();       // kann von außen aufgerufen werden
};
