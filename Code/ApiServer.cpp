#include "ApiServer.hpp"
#include "Nakamoto.hpp"
#include <arpa/inet.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <unistd.h>
#include <sys/time.h>
#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cerrno>
#include <fstream>
#include <iostream>
#include <mutex>
#include <sstream>
#include <stdexcept>
#include <sys/stat.h>

namespace {
constexpr std::size_t MAX_HEADER = 16 * 1024;
constexpr std::size_t MAX_BODY = 1024 * 1024;
constexpr int MAX_WORKERS = 64;
std::atomic<std::uint64_t> nextTraceId{1};
std::mutex apiLogMutex;

void apiLog(std::uint64_t traceId, const std::string& message) {
    std::lock_guard<std::mutex> lock(apiLogMutex);
    std::clog << "[HTTP #" << traceId << "] " << message << '\n';
    std::clog.flush();
}

std::string trim(std::string value) {
    const auto first = value.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) return {};
    const auto last = value.find_last_not_of(" \t\r\n");
    return value.substr(first, last - first + 1);
}

std::string lower(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return value;
}

bool sendAll(int socket, const std::string& data) {
    std::size_t sent = 0;
    while (sent < data.size()) {
        const auto n = send(socket, data.data() + sent, data.size() - sent, MSG_NOSIGNAL);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) return false;
        sent += static_cast<std::size_t>(n);
    }
    return true;
}

struct HttpRequest { std::string line; std::string headers; std::string body; };

HttpRequest readRequest(int socket) {
    std::string data;
    char buffer[4096];
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    auto checkDeadline = [&]() {
        if (std::chrono::steady_clock::now() >= deadline)
            throw std::runtime_error("HTTP request timed out");
    };
    auto headerEnd = std::string::npos;
    while ((headerEnd = data.find("\r\n\r\n")) == std::string::npos) {
        checkDeadline();
        const auto n = recv(socket, buffer, sizeof(buffer), 0);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) throw std::runtime_error("incomplete HTTP headers");
        data.append(buffer, static_cast<std::size_t>(n));
        if (data.size() > MAX_HEADER) throw std::runtime_error("headers too large");
    }

    HttpRequest request;
    const auto firstLineEnd = data.find("\r\n");
    if (firstLineEnd == std::string::npos || firstLineEnd > headerEnd)
        throw std::runtime_error("invalid request line");
    request.line = data.substr(0, firstLineEnd);
    request.headers = data.substr(firstLineEnd + 2, headerEnd - firstLineEnd - 2);

    std::size_t contentLength = 0;
    std::istringstream lines(request.headers);
    std::string line;
    while (std::getline(lines, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        const auto colon = line.find(':');
        if (colon == std::string::npos) throw std::runtime_error("malformed header");
        const auto name = lower(trim(line.substr(0, colon)));
        const auto value = trim(line.substr(colon + 1));
        if (name == "transfer-encoding") throw std::runtime_error("transfer encoding unsupported");
        if (name == "content-length") {
            std::size_t parsed = 0;
            try { parsed = std::stoull(value); } catch (...) { throw std::runtime_error("invalid content length"); }
            if (parsed > MAX_BODY) throw std::runtime_error("body too large");
            contentLength = parsed;
        }
    }

    const auto bodyStart = headerEnd + 4;
    while (data.size() - bodyStart < contentLength) {
        checkDeadline();
        const auto n = recv(socket, buffer, sizeof(buffer), 0);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) throw std::runtime_error("incomplete HTTP body");
        data.append(buffer, static_cast<std::size_t>(n));
        if (data.size() - bodyStart > MAX_BODY) throw std::runtime_error("body too large");
    }
    request.body = data.substr(bodyStart, contentLength);
    return request;
}

std::string makeResponse(int status, const std::string& reason, const std::string& body,
                         const std::string& origin = {}) {
    std::ostringstream out;
    out << "HTTP/1.1 " << status << ' ' << reason << "\r\n"
        << "Content-Type: application/json\r\n"
        << "Content-Length: " << body.size() << "\r\n"
        << "Connection: close\r\n";
    if (!origin.empty()) out << "Access-Control-Allow-Origin: " << origin << "\r\nVary: Origin\r\n";
    out << "Access-Control-Allow-Methods: GET, POST, OPTIONS\r\n"
        << "Access-Control-Allow-Headers: Content-Type\r\n\r\n" << body;
    return out.str();
}
}

ApiServer::ApiServer() = default;

ApiServer::~ApiServer() { stop(); }

bool ApiServer::loadConfig(const std::string& path) {
    struct stat configStat{};
    if (stat(path.c_str(), &configStat) != 0 || (configStat.st_mode & (S_IRWXG | S_IRWXO)) != 0) {
        std::cerr << "[ApiServer] Configuration file is missing or has insecure permissions; run chmod 600 " << path << '\n';
        return false;
    }
    std::ifstream input(path);
    if (!input) {
        std::cerr << "[ApiServer] Cannot read configuration file: " << path << '\n';
        return false;
    }

    struct NodeConfig { std::string name, host, user, password; int port = 0; };
    std::vector<NodeConfig> configuredNodes;
    std::string section, line;
    NodeConfig current;
    auto finishNode = [&]() {
        if (section.rfind("node:", 0) != 0) return;
        current.name = section.substr(5);
        if (current.name.empty() || current.host.empty() || current.port < 1 || current.port > 65535 || current.user.empty())
            throw std::runtime_error("Incomplete node configuration in [" + section + "]");
        configuredNodes.push_back(current);
        current = NodeConfig{};
    };

    try {
        while (std::getline(input, line)) {
            line = trim(line);
            if (line.empty() || line[0] == '#' || line[0] == ';') continue;
            if (line.front() == '[' && line.back() == ']') {
                finishNode();
                section = trim(line.substr(1, line.size() - 2));
                continue;
            }
            const auto equals = line.find('=');
            if (equals == std::string::npos) throw std::runtime_error("Invalid configuration line");
            const auto key = trim(line.substr(0, equals));
            const auto value = trim(line.substr(equals + 1));
            if (section == "server") {
                if (key == "port") port = std::stoi(value);
                else if (key == "ui_port") ui_port = std::stoi(value);
                else if (key == "allowed_origins") {
                    std::istringstream origins(value); std::string origin;
                    while (std::getline(origins, origin, ',')) if (!(origin = trim(origin)).empty()) allowed_origins.push_back(origin);
                } else throw std::runtime_error("Unknown server option: " + key);
            } else if (section.rfind("node:", 0) == 0) {
                if (key == "host") current.host = value;
                else if (key == "port") current.port = std::stoi(value);
                else if (key == "user") current.user = value;
                else if (key == "password") current.password = value;
                else throw std::runtime_error("Unknown node option: " + key);
            } else throw std::runtime_error("Option outside a configuration section");
        }
        finishNode();
        if (port < 1 || port > 65535) throw std::runtime_error("API port must be between 1 and 65535");
        if (ui_port < 1 || ui_port > 65535 || ui_port == port) throw std::runtime_error("ui_port must be valid and different from the API port");
        if (configuredNodes.empty()) throw std::runtime_error("No [node:NAME] sections are configured");
        for (const auto& node : configuredNodes) {
            if (nodes.count(node.name)) throw std::runtime_error("Duplicate node name: " + node.name);
            nodes.emplace(node.name, std::make_unique<Nakamoto>(node.user, node.password, node.host, node.port));
            node_available.emplace(node.name, false);
            node_commands.emplace(node.name, std::vector<std::string>{});
        }
    } catch (const std::exception& e) {
        std::cerr << "[ApiServer] Configuration error: " << e.what() << '\n';
        nodes.clear();
        return false;
    }

    apiLog(0, "Configuration loaded: API port=" + std::to_string(port) +
           ", UI port=" + std::to_string(ui_port) +
           ", nodes=" + std::to_string(nodes.size()) +
           ", allowed_origins=" + std::to_string(allowed_origins.size()));
    for (const auto& entry : nodes)
        apiLog(0, "Configured node: '" + entry.first + "' -> " + entry.second->endpoint());
    apiLog(0, "Probe plan created: " + std::to_string(configuredNodes.size()) + " nodes; checking in configuration order");
    for (const auto& config : configuredNodes) {
        const auto entry = nodes.find(config.name);
        if (entry == nodes.end()) {
            apiLog(0, "ERROR: Configured node '" + config.name + "' is missing from the node table");
            continue;
        }
        apiLog(0, "Startup probe START node='" + config.name + "' endpoint=" + entry->second->endpoint());
        const bool available = entry->second->probe();
        node_available[config.name] = available;
        apiLog(0, "Startup probe END node='" + config.name + "' status=" + (available ? "reachable" : "unreachable/RPC error"));
        if (available) {
            apiLog(0, "Command discovery START node='" + config.name + "' method=help");
            auto commands = entry->second->discoverCommands();
            const auto count = commands.size();
            node_commands[config.name] = std::move(commands);
            apiLog(0, "Command discovery END node='" + config.name + "' commands=" + std::to_string(count));
        }
    }
    return true;
}

void ApiServer::start() {
    auto openListener = [](int portNumber) {
        const int listener = socket(AF_INET, SOCK_STREAM, 0);
        if (listener < 0) return -1;
        int opt = 1;
        setsockopt(listener, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_port = htons(static_cast<uint16_t>(portNumber));
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        if (bind(listener, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0 || listen(listener, 16) < 0) {
            close(listener);
            return -1;
        }
        return listener;
    };
    const int apiServer = openListener(port);
    if (apiServer < 0) { perror("API socket/bind/listen"); return; }
    const int uiServer = openListener(ui_port);
    if (uiServer < 0) { perror("UI socket/bind/listen"); close(apiServer); return; }
    listen_sock.store(apiServer);
    ui_listen_sock.store(uiServer);
    running = true;
    std::cout << "[ApiServer] API:  http://127.0.0.1:" << port << '\n'
              << "[ApiServer] UI:   http://127.0.0.1:" << ui_port << '\n';
    apiLog(0, "Both listeners bound successfully; API=127.0.0.1:" + std::to_string(port) +
           ", UI=127.0.0.1:" + std::to_string(ui_port));
    ui_thread = std::thread(&ApiServer::run_ui_loop, this);
    run_loop();
    if (ui_thread.joinable()) ui_thread.join();
}

void ApiServer::stop() {
    running = false;
    const int server = listen_sock.exchange(-1);
    if (server >= 0) { shutdown(server, SHUT_RDWR); close(server); }
    const int uiServer = ui_listen_sock.exchange(-1);
    if (uiServer >= 0) { shutdown(uiServer, SHUT_RDWR); close(uiServer); }
}

void ApiServer::run_ui_loop() {
    while (running) {
        const int listener = ui_listen_sock.load();
        if (listener < 0) break;
        const int client = accept(listener, nullptr, nullptr);
        if (client < 0) {
            if (!running) break;
            if (errno == EINTR) continue;
            continue;
        }
        timeval timeout{5, 0};
        setsockopt(client, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
        setsockopt(client, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));
        try {
            const auto request = readRequest(client);
            apiLog(0, "UI request: " + request.line + "; body_bytes=" + std::to_string(request.body.size()));
            if (request.line.rfind("GET / ", 0) == 0 || request.line.rfind("GET /Genesis.html ", 0) == 0) {
                std::ifstream page("Interface/Genesis.html", std::ios::binary);
                if (!page) {
                    sendAll(client, makeResponse(500, "Internal Server Error", R"({"error":"UI file unavailable"})"));
                } else {
                    const std::string html((std::istreambuf_iterator<char>(page)), std::istreambuf_iterator<char>());
                    std::ostringstream response;
                    response << "HTTP/1.1 200 OK\r\nContent-Type: text/html; charset=utf-8\r\n"
                             << "X-Content-Type-Options: nosniff\r\nContent-Length: " << html.size()
                             << "\r\nConnection: close\r\n\r\n" << html;
                    sendAll(client, response.str());
                }
            } else {
                sendAll(client, makeResponse(404, "Not Found", R"({"error":"not found"})"));
            }
        } catch (const std::exception& e) {
            apiLog(0, std::string("UI request error: ") + e.what());
            sendAll(client, makeResponse(400, "Bad Request", R"({"error":"invalid HTTP request"})"));
        }
        close(client);
    }
}

void ApiServer::run_loop() {
    while (running) {
        const int server = listen_sock.load();
        if (server < 0) break;
        const int client = accept(server, nullptr, nullptr);
        if (client < 0) {
            if (errno == EINTR) continue;
            if (!running) break;
            continue;
        }
        timeval timeout{5, 0};
        setsockopt(client, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
        setsockopt(client, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));
        std::lock_guard<std::mutex> lock(workers_mutex);
        for (auto it = workers.begin(); it != workers.end();) {
            if (it->second->load()) {
                if (it->first.joinable()) it->first.join();
                it = workers.erase(it);
            } else ++it;
        }
        if (active_workers.load() >= MAX_WORKERS) {
            sendAll(client, makeResponse(503, "Service Unavailable", R"({"error":"server busy"})"));
            close(client);
        } else {
            auto done = std::make_shared<std::atomic<bool>>(false);
            active_workers.fetch_add(1);
            workers.emplace_back(std::thread([this, client, done]() {
                handle_client(client);
                active_workers.fetch_sub(1);
                done->store(true);
            }), done);
        }
    }
    std::lock_guard<std::mutex> lock(workers_mutex);
    for (auto& worker : workers) if (worker.first.joinable()) worker.first.join();
    workers.clear();
}

void ApiServer::handle_client(int client) {
    const auto traceId = nextTraceId.fetch_add(1);
    std::string origin;
    sockaddr_in peer{};
    socklen_t peerLen = sizeof(peer);
    char peerIp[INET_ADDRSTRLEN]{};
    const bool hasPeer = getpeername(client, reinterpret_cast<sockaddr*>(&peer), &peerLen) == 0 &&
                         inet_ntop(AF_INET, &peer.sin_addr, peerIp, sizeof(peerIp)) != nullptr;
    const auto peerAddress = hasPeer ? std::string(peerIp) + ":" + std::to_string(ntohs(peer.sin_port))
                                     : std::string("unknown");
    try {
        const HttpRequest request = readRequest(client);
        std::istringstream headers(request.headers);
        std::string line;
        std::string requestedMethod, requestedHeaders;
        while (std::getline(headers, line)) {
            if (!line.empty() && line.back() == '\r') line.pop_back();
            const auto colon = line.find(':');
            if (colon != std::string::npos) {
                const auto name = lower(trim(line.substr(0, colon)));
                const auto value = trim(line.substr(colon + 1));
                if (name == "origin") origin = value;
                else if (name == "access-control-request-method") requestedMethod = value;
                else if (name == "access-control-request-headers") requestedHeaders = value;
            }
        }
        apiLog(traceId, "Client=" + peerAddress + "; request='" + request.line + "'; origin='" +
               (origin.empty() ? "<none>" : origin) + "'; body_bytes=" + std::to_string(request.body.size()));
        if (!origin.empty() && std::find(allowed_origins.begin(), allowed_origins.end(), origin) == allowed_origins.end()) {
            apiLog(traceId, "CORS denied: origin is not in allowed_origins");
            sendAll(client, makeResponse(403, "Forbidden", R"({"error":"origin not allowed"})"));
        } else if (request.line.rfind("OPTIONS ", 0) == 0) {
            apiLog(traceId, "CORS preflight accepted; requested_method='" + requestedMethod +
                   "'; requested_headers='" + requestedHeaders + "'");
            sendAll(client, makeResponse(204, "No Content", "", origin));
        } else if (request.line.rfind("GET /nodes ", 0) == 0) {
            apiLog(traceId, "Node list requested");
            nlohmann::json sources = nlohmann::json::array();
            for (const auto& entry : nodes)
                sources.push_back({{"id", entry.first}, {"available", node_available.at(entry.first)},
                                   {"commands", node_commands.at(entry.first)}});
            sendAll(client, makeResponse(200, "OK", nlohmann::json{{"nodes", sources}}.dump(), origin));
        } else if (request.line.rfind("POST ", 0) != 0) {
            sendAll(client, makeResponse(405, "Method Not Allowed", R"({"error":"POST required"})", origin));
        } else {
            const auto json = nlohmann::json::parse(request.body);
            if (!json.is_object() || !json.contains("id") || !json["id"].is_string()) {
                sendAll(client, makeResponse(400, "Bad Request", R"({"error":"string id required"})", origin));
            } else {
                const auto node = nodes.find(json["id"].get<std::string>());
                if (node == nodes.end()) {
                    apiLog(traceId, "Unknown node ID='" + json["id"].get<std::string>() + "'");
                    sendAll(client, makeResponse(404, "Not Found", R"({"error":"node not found"})", origin));
                }
                else {
                    const auto method = json.contains("method") && json["method"].is_string()
                        ? json["method"].get<std::string>() : std::string("<missing/non-string>");
                    apiLog(traceId, "Forwarding to node='" + json["id"].get<std::string>() +
                           "'; rpc_method=" + nlohmann::json(method).dump() +
                           "; payload_bytes=" + std::to_string(request.body.size()));
                    const auto body = node->second->sendRpc(request.body, traceId);
                    apiLog(traceId, "Upstream request completed; response_bytes=" + std::to_string(body.size()));
                    if (body.empty()) sendAll(client, makeResponse(502, "Bad Gateway", R"({"error":"node request failed"})", origin));
                    else sendAll(client, makeResponse(200, "OK", body, origin));
                }
            }
        }
    } catch (const nlohmann::json::exception& e) {
        apiLog(traceId, std::string("JSON error: ") + e.what());
        sendAll(client, makeResponse(400, "Bad Request", R"({"error":"invalid JSON"})", origin));
    } catch (const std::exception& e) {
        const std::string message = e.what();
        apiLog(traceId, "HTTP request error: " + message);
        const int status = (message.find("too large") != std::string::npos) ? 413 : 400;
        sendAll(client, makeResponse(status, status == 413 ? "Payload Too Large" : "Bad Request",
                                     R"({"error":"invalid HTTP request"})", origin));
    }
    close(client);
}
