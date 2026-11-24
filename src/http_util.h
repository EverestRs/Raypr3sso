#pragma once

// Stateless HTTP / JSON / WebSocket helpers shared by the render server.
// Everything here is free of render-scene state so it can be unit-tested and
// reused without pulling in the scheduler or CUDA.
#include "../include/types.h"

#include <cstddef>
#include <cstdint>
#include <string>

namespace http {

constexpr size_t kMaxHeaderBytes = 1024 * 1024;
constexpr size_t kMaxBodyBytes = 1024 * 1024;

// ---- JSON (substring-based: request bodies are flat objects) ----
std::string jsonEscape(const std::string& value);
bool jsonNumber(const std::string& body, const std::string& key, float& value);
bool jsonVector3(const std::string& body, const std::string& key, float3& value);
bool jsonString(const std::string& body, const std::string& key, std::string& value);

// ---- HTTP ----
bool sendAll(int socket, const std::string& data);
std::string httpResponse(int status, const std::string& type, const std::string& body);
std::string headerValue(const std::string& request, const std::string& name);
std::string queryParam(const std::string& query, const std::string& key);

struct RequestHead {
    std::string method;
    std::string path;    // without the query string
    std::string query;   // without '?', empty when absent
    bool valid = false;
};
RequestHead parseRequestHead(const std::string& request);

// Reads one request (headers + declared body) with both sizes bounded. On
// failure returns false and, when a reply is owed, fills `errorResponse`
// with a complete response (413 for oversized input). A closed connection
// leaves `errorResponse` empty: the caller just closes.
bool readHttpRequest(int socket, std::string& request, std::string& errorResponse);

// ---- WebSocket ----
std::string base64(const unsigned char* data, size_t size);
std::string sha1Base64(const std::string& input);
std::string webSocketAccept(const std::string& secWebSocketKey);
bool sendWebSocketText(int socket, const std::string& text);
// Sends a control frame (FIN set). opcode: 0x8 Close, 0x9 Ping, 0xA Pong.
bool sendWebSocketControl(int socket, unsigned char opcode, const std::string& payload);

struct WebSocketFrame {
    unsigned char opcode = 0;  // low nibble of the first byte
    std::string payload;       // unmasked
};
// ok: one frame decoded. idle: the socket timeout fired before the header
// started — the caller should retry (drain/send cycle). error: dead or
// malformed connection — the caller must close. Partial headers are retried
// inside; oversized payloads (> maxPayload) are an error.
enum class FrameRead { ok, idle, error };
FrameRead readWebSocketFrame(int socket, WebSocketFrame& frame, size_t maxPayload);

} // namespace http