// Nakamoto.cpp - JSON-RPC client for a node.
#include "Nakamoto.hpp"
#include <nlohmann/json.hpp>
#include <algorithm>
#include <iostream>                     // Used for startup errors.
#include <mutex>
#include <set>

// Keep implementation details local to this file.
namespace {
std::mutex logMutex;

struct CurlTraceContext {
    std::uint64_t traceId = 0;
    unsigned requestCount = 0;
};

void debugLog(std::uint64_t traceId, const std::string& message) {
    std::lock_guard<std::mutex> lock(logMutex);
    std::clog << "[RPC" << (traceId ? " #" + std::to_string(traceId) : "") << "] " << message << '\n';
    std::clog.flush();
}

// libcurl invokes this callback as response data arrives in chunks.
// contents points to the bytes, size and nmemb describe the chunk, and userp
// is the response string provided through CURLOPT_WRITEDATA.
size_t writeCallback(void* contents, size_t size, size_t nmemb, void* userp) {
    auto* output = static_cast<std::string*>(userp);
    output->append(static_cast<char*>(contents), size * nmemb);
    // Return the number of bytes consumed.
    return size * nmemb;
}

// Log selected HTTP metadata only. Never log the Authorization header or its
// Base64 value.
int curlDebugCallback(CURL*, curl_infotype type, char* data, size_t size, void* userp) {
    auto* trace = static_cast<CurlTraceContext*>(userp);
    if (!trace || !data) return 0;

    const std::string block(data, size);
    size_t offset = 0;
    while (offset < block.size()) {
        const size_t newline = block.find('\n', offset);
        std::string line = block.substr(offset, newline == std::string::npos ? std::string::npos : newline - offset);
        if (!line.empty() && line.back() == '\r') line.pop_back();
        offset = newline == std::string::npos ? block.size() : newline + 1;

        if (type == CURLINFO_HEADER_OUT) {
            if (line.rfind("POST ", 0) == 0) {
                ++trace->requestCount;
                debugLog(trace->traceId, "HTTP request #" + std::to_string(trace->requestCount) + " sent");
            } else if (line.rfind("Authorization:", 0) == 0 || line.rfind("authorization:", 0) == 0) {
                debugLog(trace->traceId, "Authorization header sent (value redacted)");
            }
        } else if (type == CURLINFO_HEADER_IN) {
            if (line.rfind("HTTP/", 0) == 0) {
                const size_t firstSpace = line.find(' ');
                if (firstSpace != std::string::npos) {
                    const size_t end = line.find_first_of(" \t", firstSpace + 1);
                    debugLog(trace->traceId, "HTTP response status=" + line.substr(firstSpace + 1,
                        end == std::string::npos ? std::string::npos : end - firstSpace - 1));
                }
            } else if (line.rfind("WWW-Authenticate:", 0) == 0 || line.rfind("www-authenticate:", 0) == 0) {
                const auto colon = line.find(':');
                const auto value = line.find_first_not_of(" \t", colon + 1);
                const auto end = line.find_first_of(" \t,", value);
                debugLog(trace->traceId, "WWW-Authenticate scheme=" +
                    (value == std::string::npos ? std::string("unknown") : line.substr(value, end - value)));
            }
        }
    }
    return 0;
}

} // namespace

// Construct and configure a JSON-RPC client.
Nakamoto::Nakamoto(const std::string& user, const std::string& pass,
                   const std::string& host, int port)
    : rpcUser(user),
      rpcPass(pass),
      rpcUrl("http://" + host + ":" + std::to_string(port)),
      headers(nullptr),
      client(nullptr)
{
    // Global initialization is idempotent and is also performed here as a safeguard.
    static const CURLcode curl_init_result = curl_global_init(CURL_GLOBAL_DEFAULT);
    if (curl_init_result != CURLE_OK) {
        std::cerr << "[Nakamoto] curl_global_init() failed.\n";
        return;
    }

    // Create the CURL handle.
    client = curl_easy_init();
    if (!client) {
        std::cerr << "[Nakamoto] curl_easy_init() failed.\n";
        return; // The client remains uninitialized.
    }

    // Set options that remain fixed for every request.
    curl_easy_setopt(client, CURLOPT_URL, rpcUrl.c_str());

    // Enable HTTP Basic authentication.
    curl_easy_setopt(client, CURLOPT_HTTPAUTH, CURLAUTH_BASIC);
    // Credentials use the "user:pass" format.
    curl_easy_setopt(client, CURLOPT_USERPWD, (rpcUser + ":" + rpcPass).c_str());
    debugLog(0, "Client ready; endpoint=" + rpcUrl + "; Basic authentication configured (credentials redacted)");

    // Register the response callback.
    curl_easy_setopt(client, CURLOPT_WRITEFUNCTION, writeCallback);

    // Prevent libcurl from raising SIGPIPE when a connection is interrupted.
    curl_easy_setopt(client, CURLOPT_NOSIGNAL, 1L);

    // Maximum duration for the entire request.
    curl_easy_setopt(client, CURLOPT_TIMEOUT, 15L);
    // Maximum duration for connecting.
    curl_easy_setopt(client, CURLOPT_CONNECTTIMEOUT, 5L);

    // Send JSON requests.
    headers = curl_slist_append(headers, "Content-Type: application/json");
    // Pass the header list to CURL.
    curl_easy_setopt(client, CURLOPT_HTTPHEADER, headers);

    // Restrict debug output to safe HTTP header metadata.
    curl_easy_setopt(client, CURLOPT_DEBUGFUNCTION, curlDebugCallback);
    curl_easy_setopt(client, CURLOPT_VERBOSE, 1L);

    // Keep connections alive for subsequent requests.
    curl_easy_setopt(client, CURLOPT_TCP_KEEPALIVE, 1L);
}

// Release resources owned by this client.
Nakamoto::~Nakamoto() {
    // Free the HTTP header list.
    if (headers) {
        curl_slist_free_all(headers);
        headers = nullptr;
    }

    // Clean up the CURL handle.
    if (client) {
        curl_easy_cleanup(client);
        client = nullptr;
    }
}

// Send a JSON-RPC request to the node.
std::string Nakamoto::sendRpc(const std::string& jsonPayload, std::uint64_t traceId) {
    // Do not use the shared CURL handle from multiple threads at once.
    std::lock_guard<std::mutex> lock(rpcMutex);

    std::string response;
    if (!client) {
        debugLog(traceId, "Aborted: CURL client is not initialized");
        return response;
    }

    debugLog(traceId, "Sending POST to " + rpcUrl + "; payload_bytes=" + std::to_string(jsonPayload.size()));

    // Update only per-request settings. curl_easy_reset() would clear the URL,
    // authentication, timeout, and other persistent options.
    response.clear();

    // Set the JSON request body and its explicit byte length.
    curl_easy_setopt(client, CURLOPT_POSTFIELDS, jsonPayload.c_str());
    curl_easy_setopt(client, CURLOPT_POSTFIELDSIZE, jsonPayload.size());
    // Store the response in our string.
    curl_easy_setopt(client, CURLOPT_WRITEDATA, &response);

    // Perform the request.
    CurlTraceContext curlTrace{traceId, 0};
    curl_easy_setopt(client, CURLOPT_DEBUGDATA, &curlTrace);
    CURLcode res = curl_easy_perform(client);

    long http_code = 0;
    long auth_available = 0;
    char* primary_ip = nullptr;
    char* content_type = nullptr;
    double total_seconds = 0;
    curl_easy_getinfo(client, CURLINFO_RESPONSE_CODE, &http_code);
    curl_easy_getinfo(client, CURLINFO_HTTPAUTH_AVAIL, &auth_available);
    curl_easy_getinfo(client, CURLINFO_PRIMARY_IP, &primary_ip);
    curl_easy_getinfo(client, CURLINFO_CONTENT_TYPE, &content_type);
    curl_easy_getinfo(client, CURLINFO_TOTAL_TIME, &total_seconds);

    // Handle transport errors.
    if (res != CURLE_OK) {
        debugLog(traceId, "CURL_ERROR=" + std::string(curl_easy_strerror(res)) +
                 "; http=" + std::to_string(http_code) +
                 "; auth_challenge_bits=" + std::to_string(auth_available) +
                 "; basic_challenge=" + ((auth_available & CURLAUTH_BASIC) ? "yes" : "no") +
                 "; peer=" + (primary_ip ? primary_ip : "unknown") +
                 "; duration_s=" + std::to_string(total_seconds) +
                 "; response_bytes=" + std::to_string(response.size()));
        return response;
    }

    // Log the HTTP status (for example, 401 indicates rejected credentials).
    debugLog(traceId, "Response from " + rpcUrl + "; http=" + std::to_string(http_code) +
             "; auth_challenge_bits=" + std::to_string(auth_available) +
             "; basic_challenge=" + ((auth_available & CURLAUTH_BASIC) ? "yes" : "no") +
             "; peer=" + (primary_ip ? primary_ip : "unknown") +
             "; content_type=" + (content_type ? content_type : "unknown") +
             "; duration_s=" + std::to_string(total_seconds) +
             "; response_bytes=" + std::to_string(response.size()));
    if (http_code != 200) {
        debugLog(traceId, "UPSTREAM_HTTP_STATUS=" + std::to_string(http_code));
        // Keep the body; it may contain a useful JSON-RPC error.
    }

    return response;
}

bool Nakamoto::probe() {
    const std::string response = sendRpc(R"({"jsonrpc":"1.0","id":"lilith-probe","method":"getblockcount","params":[]})");
    if (response.empty()) return false;
    try {
        const auto json = nlohmann::json::parse(response);
        return json.is_object() && json.contains("result") && !json["result"].is_null();
    } catch (const nlohmann::json::parse_error&) {
        return false;
    }
}

std::vector<std::string> Nakamoto::discoverCommands() {
    const std::string response = sendRpc(
        R"({"jsonrpc":"1.0","id":"lilith-help","method":"help","params":[]})");
    std::vector<std::string> commands;
    if (response.empty()) return commands;

    try {
        const auto json = nlohmann::json::parse(response);
        if (!json.is_object() || !json.contains("result") || !json["result"].is_string() ||
            (json.contains("error") && !json["error"].is_null())) {
            return commands;
        }

        const std::string helpText = json["result"].get<std::string>();
        std::set<std::string> seen;
        size_t offset = 0;
        while (offset <= helpText.size()) {
            const size_t newline = helpText.find('\n', offset);
            std::string line = helpText.substr(offset,
                newline == std::string::npos ? std::string::npos : newline - offset);
            if (!line.empty() && line.back() == '\r') line.pop_back();
            offset = newline == std::string::npos ? helpText.size() + 1 : newline + 1;

            // Bitcoin-derived help output may show either a bare command name
            // or its synopsis (for example: getblock "hash" ( verbosity )).
            // Keep only unindented lowercase RPC-name tokens; prose and section
            // headings are not command names.
            if (line.empty() || line.front() == ' ' || line.front() == '\t' || line.rfind("==", 0) == 0)
                continue;
            const size_t nameEnd = line.find_first_of(" \t(");
            const std::string name = line.substr(0, nameEnd);
            const bool validName = std::all_of(name.begin(), name.end(), [](unsigned char c) {
                return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_';
            });
            bool looksLikeSynopsis = nameEnd == std::string::npos;
            if (!looksLikeSynopsis && line[nameEnd] == '(') looksLikeSynopsis = true;
            if (!looksLikeSynopsis && (line[nameEnd] == ' ' || line[nameEnd] == '\t')) {
                const size_t argumentStart = line.find_first_not_of(" \t", nameEnd);
                if (argumentStart != std::string::npos) {
                    const char firstArgument = line[argumentStart];
                    looksLikeSynopsis = firstArgument == '"' || firstArgument == '(' ||
                        firstArgument == '<' || firstArgument == '[';
                    // Some RPC help implementations omit quotes/brackets in
                    // signatures, e.g. "getblockhash height". Accept a bare
                    // parameter token while excluding common prose starters.
                    if (!looksLikeSynopsis && firstArgument >= 'a' && firstArgument <= 'z') {
                        const size_t argEnd = line.find_first_of(" \t(),", argumentStart);
                        const std::string firstWord = line.substr(argumentStart,
                            argEnd == std::string::npos ? std::string::npos : argEnd - argumentStart);
                        static const std::set<std::string> proseStarters = {
                            "a", "also", "an", "and", "arguments", "deprecated", "does", "examples",
                            "for", "if", "note", "otherwise", "returns", "return", "result", "the",
                            "this", "to", "use", "used", "when", "which", "will"
                        };
                        looksLikeSynopsis = !firstWord.empty() && proseStarters.count(firstWord) == 0;
                    }
                }
            }
            if (validName && !name.empty() && name.front() >= 'a' && name.front() <= 'z' &&
                looksLikeSynopsis && seen.insert(name).second)
                commands.push_back(name);
        }
    } catch (const nlohmann::json::exception&) {
        return {};
    }
    return commands;
}
