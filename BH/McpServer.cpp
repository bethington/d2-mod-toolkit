// Must include winsock2 before windows.h to avoid v1/v2 conflict
#include <winsock2.h>
#include <ws2tcpip.h>

#define CPPHTTPLIB_NO_EXCEPTIONS
#include "McpServer.h"

#include <windows.h>
#include <thread>
#include <atomic>
#include <string>
#include <sstream>
#include <mutex>
#include <condition_variable>
#include <queue>
#include <map>
#include <functional>

// cpp-httplib — single header HTTP server
#include "../ThirdParty/httplib.h"

// nlohmann JSON — already in ThirdParty for BH
#include "../ThirdParty/nlohmann/json.hpp"

using json = nlohmann::json;

namespace {
    std::thread g_thread;
    std::atomic<bool> g_running{false};
    std::atomic<int>  g_requestCount{0};
    int g_port = 21337;
    httplib::Server* g_server = nullptr;

    // SSE session management
    // Each SSE client gets a unique session ID. Responses to JSON-RPC requests
    // are pushed back through the SSE stream for that session.
    std::atomic<int> g_nextSessionId{1};

    struct SseSession {
        std::mutex mutex;
        std::condition_variable cv;
        std::queue<std::string> messages; // SSE-formatted messages to send
        bool closed = false;
    };

    std::mutex g_sessionsMutex;
    std::map<int, std::shared_ptr<SseSession>> g_sessions;

    void SendSseMessage(int sessionId, const std::string& event, const std::string& data) {
        std::lock_guard<std::mutex> lock(g_sessionsMutex);
        auto it = g_sessions.find(sessionId);
        if (it != g_sessions.end()) {
            auto& session = it->second;
            std::lock_guard<std::mutex> slock(session->mutex);
            std::string msg = "event: " + event + "\ndata: " + data + "\n\n";
            session->messages.push(msg);
            session->cv.notify_one();
        }
    }

    void CloseSseSession(int sessionId) {
        std::lock_guard<std::mutex> lock(g_sessionsMutex);
        auto it = g_sessions.find(sessionId);
        if (it != g_sessions.end()) {
            auto& session = it->second;
            std::lock_guard<std::mutex> slock(session->mutex);
            session->closed = true;
            session->cv.notify_one();
        }
    }

    // MCP protocol version
    static const char* MCP_VERSION = "2024-11-05";

    // Server info returned in initialize response
    json ServerInfo() {
        return {
            {"name", "d2-mod-toolkit"},
            {"version", "0.1.0"}
        };
    }

    // Build the tools list
    json ToolsList() {
        json tools = json::array();

        tools.push_back({
            {"name", "ping"},
            {"description", "Test connectivity to the game process. Returns basic status."},
            {"inputSchema", {
                {"type", "object"},
                {"properties", json::object()},
                {"required", json::array()}
            }}
        });

        tools.push_back({
            {"name", "get_game_info"},
            {"description", "Get basic information about the running Diablo II process."},
            {"inputSchema", {
                {"type", "object"},
                {"properties", json::object()},
                {"required", json::array()}
            }}
        });

        return tools;
    }

    // Handle tool calls
    json HandleToolCall(const std::string& name, const json& arguments) {
        if (name == "ping") {
            return {
                {"content", {{
                    {"type", "text"},
                    {"text", "pong - d2-mod-toolkit is running inside Diablo II"}
                }}}
            };
        }

        if (name == "get_game_info") {
            // Basic process info for now — will expand in Phase 2
            DWORD pid = GetCurrentProcessId();
            char exePath[MAX_PATH] = {};
            GetModuleFileNameA(nullptr, exePath, MAX_PATH);

            json info = {
                {"process_id", pid},
                {"executable", exePath},
                {"mcp_port", g_port},
                {"request_count", g_requestCount.load()},
                {"status", "running"}
            };

            return {
                {"content", {{
                    {"type", "text"},
                    {"text", info.dump(2)}
                }}}
            };
        }

        return {
            {"content", {{
                {"type", "text"},
                {"text", "Unknown tool: " + name}
            }}},
            {"isError", true}
        };
    }

    // Process a JSON-RPC request and return a response
    json ProcessRequest(const json& request) {
        std::string method = request.value("method", "");
        auto id = request.value("id", json(nullptr));
        json params = request.value("params", json::object());

        json result;

        if (method == "initialize") {
            result = {
                {"protocolVersion", MCP_VERSION},
                {"capabilities", {
                    {"tools", {{"listChanged", false}}}
                }},
                {"serverInfo", ServerInfo()}
            };
        }
        else if (method == "notifications/initialized") {
            // Client notification — no response needed
            return json(nullptr);
        }
        else if (method == "tools/list") {
            result = {
                {"tools", ToolsList()}
            };
        }
        else if (method == "tools/call") {
            std::string toolName = params.value("name", "");
            json arguments = params.value("arguments", json::object());
            result = HandleToolCall(toolName, arguments);
        }
        else {
            // Unknown method
            return {
                {"jsonrpc", "2.0"},
                {"id", id},
                {"error", {
                    {"code", -32601},
                    {"message", "Method not found: " + method}
                }}
            };
        }

        if (result.is_null()) {
            return json(nullptr);
        }

        return {
            {"jsonrpc", "2.0"},
            {"id", id},
            {"result", result}
        };
    }

    void ServerThread() {
        g_server = new httplib::Server();

        // CORS headers for browser/tool access
        g_server->set_default_headers({
            {"Access-Control-Allow-Origin", "*"},
            {"Access-Control-Allow-Methods", "GET, POST, OPTIONS"},
            {"Access-Control-Allow-Headers", "Content-Type"}
        });

        // OPTIONS preflight
        g_server->Options("/mcp", [](const httplib::Request&, httplib::Response& res) {
            res.status = 204;
        });

        // POST /mcp — JSON-RPC endpoint
        // Accepts an optional ?sessionId= query param to route response through SSE
        g_server->Post("/mcp", [](const httplib::Request& req, httplib::Response& res) {
            g_requestCount++;

            // Check for session ID to route response through SSE
            int sessionId = 0;
            if (req.has_param("sessionId")) {
                sessionId = std::atoi(req.get_param_value("sessionId").c_str());
            }

            try {
                json request = json::parse(req.body);
                json response = ProcessRequest(request);

                if (response.is_null()) {
                    // Notification — no response body
                    res.status = 202;
                    return;
                }

                if (sessionId > 0) {
                    // Route response through the SSE stream
                    SendSseMessage(sessionId, "message", response.dump());
                    res.status = 202;
                    res.set_content("", "text/plain");
                } else {
                    // Direct HTTP response (for non-SSE clients like curl)
                    res.set_content(response.dump(), "application/json");
                }
            }
            catch (const json::parse_error& e) {
                json error = {
                    {"jsonrpc", "2.0"},
                    {"id", nullptr},
                    {"error", {
                        {"code", -32700},
                        {"message", std::string("Parse error: ") + e.what()}
                    }}
                };
                if (sessionId > 0) {
                    SendSseMessage(sessionId, "message", error.dump());
                    res.status = 202;
                } else {
                    res.set_content(error.dump(), "application/json");
                }
            }
            catch (...) {
                json error = {
                    {"jsonrpc", "2.0"},
                    {"id", nullptr},
                    {"error", {
                        {"code", -32603},
                        {"message", "Internal error"}
                    }}
                };
                if (sessionId > 0) {
                    SendSseMessage(sessionId, "message", error.dump());
                    res.status = 202;
                } else {
                    res.set_content(error.dump(), "application/json");
                }
            }
        });

        // GET /mcp/sse — Server-Sent Events endpoint
        // Keeps the connection open. Sends an initial 'endpoint' event with
        // the POST URL (including sessionId), then streams responses back.
        g_server->Get("/mcp/sse", [](const httplib::Request&, httplib::Response& res) {
            int sessionId = g_nextSessionId++;

            // Create session
            auto session = std::make_shared<SseSession>();
            {
                std::lock_guard<std::mutex> lock(g_sessionsMutex);
                g_sessions[sessionId] = session;
            }

            // Build the endpoint URL with session ID
            std::string postUrl = "/mcp?sessionId=" + std::to_string(sessionId);

            res.set_header("Cache-Control", "no-cache");
            res.set_header("Connection", "keep-alive");
            res.set_header("X-Accel-Buffering", "no");

            // Track whether we've sent the endpoint event for this connection
            auto sentEndpoint = std::make_shared<bool>(false);

            // Use chunked content provider to stream SSE events
            res.set_chunked_content_provider(
                "text/event-stream",
                [sessionId, session, postUrl, sentEndpoint](size_t /*offset*/, httplib::DataSink& sink) -> bool {
                    // Send the initial endpoint event
                    if (!*sentEndpoint) {
                        std::string endpointEvent = "event: endpoint\ndata: " + postUrl + "\n\n";
                        sink.write(endpointEvent.c_str(), endpointEvent.size());
                        *sentEndpoint = true;
                    }

                    // Wait for messages or shutdown
                    std::unique_lock<std::mutex> lock(session->mutex);
                    session->cv.wait_for(lock, std::chrono::seconds(15), [&session]() {
                        return !session->messages.empty() || session->closed;
                    });

                    if (session->closed) {
                        return false; // Close the stream
                    }

                    // Send all queued messages
                    while (!session->messages.empty()) {
                        std::string msg = session->messages.front();
                        session->messages.pop();
                        if (!sink.write(msg.c_str(), msg.size())) {
                            return false;
                        }
                    }

                    // If no messages, send a keep-alive comment
                    if (session->messages.empty()) {
                        std::string keepalive = ": keepalive\n\n";
                        sink.write(keepalive.c_str(), keepalive.size());
                    }

                    return true; // Keep connection open
                },
                [sessionId](bool /*success*/) {
                    // Cleanup session on disconnect
                    std::lock_guard<std::mutex> lock(g_sessionsMutex);
                    g_sessions.erase(sessionId);
                }
            );
        });

        // GET /health — simple health check
        g_server->Get("/health", [](const httplib::Request&, httplib::Response& res) {
            json health = {
                {"status", "ok"},
                {"server", "d2-mod-toolkit"},
                {"port", g_port},
                {"requests", g_requestCount.load()}
            };
            res.set_content(health.dump(), "application/json");
        });

        g_running = true;
        g_server->listen("127.0.0.1", g_port);
        g_running = false;
    }
}

namespace McpServer {
    void Init(int port) {
        if (g_running) return;
        g_port = port;
        g_requestCount = 0;
        g_thread = std::thread(ServerThread);
        g_thread.detach();
    }

    void Shutdown() {
        if (g_server) {
            g_server->stop();
        }
        // Wait for thread to exit
        for (int i = 0; i < 30 && g_running; ++i) {
            Sleep(100);
        }
        if (g_server) {
            delete g_server;
            g_server = nullptr;
        }
    }

    bool IsRunning() {
        return g_running;
    }

    int GetPort() {
        return g_port;
    }

    int GetRequestCount() {
        return g_requestCount;
    }
}
