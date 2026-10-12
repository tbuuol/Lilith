// API server declarations.
#pragma once
#include <unordered_map>
#include <memory>
#include <string>
#include <atomic>
#include <mutex>
#include <thread>
#include <vector>
#include <utility>
#include <nlohmann/json.hpp>

class Nakamoto; // Forward declaration.

class ApiServer {
    std::atomic<bool> running{false};
    int port = 9999;
    int ui_port = 10000;
    std::atomic<int> listen_sock{-1};
    std::atomic<int> ui_listen_sock{-1};
    std::thread ui_thread;
    std::vector<std::pair<std::thread, std::shared_ptr<std::atomic<bool>>>> workers;
    std::mutex workers_mutex;
    std::atomic<int> active_workers{0};
    std::vector<std::string> allowed_origins;
    std::unordered_map<std::string, std::unique_ptr<Nakamoto>> nodes;
    std::unordered_map<std::string, bool> node_available;
    std::unordered_map<std::string, std::vector<std::string>> node_commands;

    void handle_client(int client);
    void run_loop();
    void run_ui_loop();

public:
    ApiServer();
    ~ApiServer();
    bool loadConfig(const std::string& path);
    void start();      // Blocks until the server stops.
    void stop();       // Can be called externally.
};
