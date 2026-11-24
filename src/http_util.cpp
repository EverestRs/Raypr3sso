#include "http_util.h"

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <sstream>
#include <vector>

#include <sys/socket.h>

namespace http {
namespace {

uint32_t rotateLeft(uint32_t value, int bits) {
    return (value << bits) | (value >> (32 - bits));
}

bool appendUtf8(std::string& out, uint32_t cp) {
    if (cp > 0x10ffff || (cp >= 0xd800 && cp <= 0xdfff)) return false;
    if (cp < 0x80) {
        out += char(cp);
    } else if (cp < 0x800) {
        out += char(0xc0 | (cp >> 6));
        out += char(0x80 | (cp & 0x3f));
    } else if (cp < 0x10000) {
        out += char(0xe0 | (cp >> 12));
        out += char(0x80 | ((cp >> 6) & 0x3f));
        out += char(0x80 | (cp & 0x3f));
    } else {
        out += char(0xf0 | (cp >> 18));
        out += char(0x80 | ((cp >> 12) & 0x3f));
        out += char(0x80 | ((cp >> 6) & 0x3f));
        out += char(0x80 | (cp & 0x3f));
    }
    return true;
}

bool parseHex4(const std::string& s, size_t pos, uint32_t& out) {
    if (pos + 4 > s.size()) return false;
    out = 0;
    for (size_t i = 0; i < 4; ++i) {
        const unsigned char c = static_cast<unsigned char>(s[pos + i]);
        uint32_t digit;
        if (c >= '0' && c <= '9') digit = c - '0';
        else if (c >= 'a' && c <= 'f') digit = c - 'a' + 10;
        else if (c >= 'A' && c <= 'F') digit = c - 'A' + 10;
        else return false;
        out = (out << 4) | digit;
    }
    return true;
}

bool jsonUnescape(const std::string& in, std::string& out) {
    out.clear();
    for (size_t i = 0; i < in.size(); ++i) {
        const char c = in[i];
        if (c != '\\') {
            out += c;
            continue;
        }
        if (++i >= in.size()) return false;
        switch (in[i]) {
        case '"': out += '"'; break;
        case '\\': out += '\\'; break;
        case '/': out += '/'; break;
        case 'b': out += '\b'; break;
        case 'f': out += '\f'; break;
        case 'n': out += '\n'; break;
        case 'r': out += '\r'; break;
        case 't': out += '\t'; break;
        case 'u': {
            uint32_t cp = 0;
            if (!parseHex4(in, i + 1, cp)) return false;
            i += 4;
            if (cp >= 0xd800 && cp <= 0xdbff) {   // surrogate pair
                uint32_t low = 0;
                if (i + 6 > in.size() || in[i + 1] != '\\' || in[i + 2] != 'u' ||
                    !parseHex4(in, i + 3, low) || low < 0xdc00 || low > 0xdfff)
                    return false;
                i += 6;
                cp = 0x10000 + ((cp - 0xd800) << 10) + (low - 0xdc00);
            }
            if (!appendUtf8(out, cp)) return false;
            break;
        }
        default: return false;
        }
    }
    return true;
}

} // namespace

std::string jsonEscape(const std::string& value) {
    std::string result;
    result.reserve(value.size());
    for (unsigned char c : value) {
        switch (c) {
        case '"': result += "\\\""; break;
        case '\\': result += "\\\\"; break;
        case '\b': result += "\\b"; break;
        case '\f': result += "\\f"; break;
        case '\n': result += "\\n"; break;
        case '\r': result += "\\r"; break;
        case '\t': result += "\\t"; break;
        default:
            if (c < 0x20) {
                char escaped[7];
                std::snprintf(escaped, sizeof(escaped), "\\u%04x", unsigned(c));
                result += escaped;
            } else {
                result += char(c);
            }
        }
    }
    return result;
}

bool jsonNumber(const std::string& body, const std::string& key, float& value) {
    const size_t keyPosition = body.find("\"" + key + "\"");
    if (keyPosition == std::string::npos) return false;
    const size_t colon = body.find(':', keyPosition);
    if (colon == std::string::npos) return false;
    char* end = nullptr;
    value = std::strtof(body.c_str() + colon + 1, &end);
    return end != body.c_str() + colon + 1 && std::isfinite(value);
}

bool jsonVector3(const std::string& body, const std::string& key, float3& value) {
    const size_t keyPosition = body.find("\"" + key + "\"");
    if (keyPosition == std::string::npos) return false;
    const size_t open = body.find('[', keyPosition);
    const size_t close = body.find(']', open);
    if (open == std::string::npos || close == std::string::npos) return false;
    std::string values = body.substr(open + 1, close - open - 1);
    std::replace(values.begin(), values.end(), ',', ' ');
    std::istringstream stream(values);
    return bool(stream >> value.x >> value.y >> value.z) &&
           std::isfinite(value.x) && std::isfinite(value.y) && std::isfinite(value.z);
}

bool jsonString(const std::string& body, const std::string& key, std::string& value) {
    const size_t keyPosition = body.find("\"" + key + "\"");
    if (keyPosition == std::string::npos) return false;
    const size_t colon = body.find(':', keyPosition);
    if (colon == std::string::npos) return false;
    size_t pos = colon + 1;
    while (pos < body.size() && std::isspace(static_cast<unsigned char>(body[pos]))) ++pos;
    if (pos >= body.size() || body[pos] != '"') return false;
    const size_t begin = ++pos;
    bool escaped = false;
    for (; pos < body.size(); ++pos) {
        if (escaped) {
            escaped = false;
        } else if (body[pos] == '\\') {
            escaped = true;
        } else if (body[pos] == '"') {
            break;
        }
    }
    if (pos >= body.size()) return false;
    return jsonUnescape(body.substr(begin, pos - begin), value);
}

bool sendAll(int socket, const std::string& data) {
    size_t sent = 0;
    while (sent < data.size()) {
        const ssize_t count = send(socket, data.data() + sent, data.size() - sent,
                                   MSG_NOSIGNAL);
        if (count <= 0) return false;
        sent += size_t(count);
    }
    return true;
}

std::string httpResponse(int status, const std::string& type, const std::string& body) {
    const char* text = "Internal Server Error";
    switch (status) {
    case 200: text = "OK"; break;
    case 201: text = "Created"; break;
    case 400: text = "Bad Request"; break;
    case 401: text = "Unauthorized"; break;
    case 403: text = "Forbidden"; break;
    case 404: text = "Not Found"; break;
    case 405: text = "Method Not Allowed"; break;
    case 409: text = "Conflict"; break;
    case 413: text = "Payload Too Large"; break;
    case 429: text = "Too Many Requests"; break;
    case 503: text = "Service Unavailable"; break;
    }
    std::ostringstream response;
    response << "HTTP/1.1 " << status << ' ' << text << "\r\n"
             << "Content-Type: " << type << "\r\n"
             << "Content-Length: " << body.size() << "\r\n"
             << "Connection: close\r\n"
             << "Access-Control-Allow-Origin: *\r\n"
             << "Access-Control-Allow-Methods: GET, POST, OPTIONS\r\n"
             << "Access-Control-Allow-Headers: Content-Type\r\n\r\n" << body;
    return response.str();
}

std::string headerValue(const std::string& request, const std::string& name) {
    const std::string prefix = name + ": ";
    size_t begin = request.find(prefix);
    if (begin == std::string::npos) return {};
    begin += prefix.size();
    size_t end = request.find("\r\n", begin);
    return request.substr(begin, end == std::string::npos ? std::string::npos : end - begin);
}

std::string queryParam(const std::string& query, const std::string& key) {
    const std::string prefix = key + "=";
    size_t pos = 0;
    while (true) {
        pos = query.find(prefix, pos);
        if (pos == std::string::npos) return {};
        if (pos != 0 && query[pos - 1] != '&') {
            pos += prefix.size();
            continue;
        }
        const size_t end = query.find('&', pos);
        return query.substr(pos + prefix.size(),
                            end == std::string::npos ? std::string::npos
                                                     : end - pos - prefix.size());
    }
}

RequestHead parseRequestHead(const std::string& request) {
    RequestHead head;
    const size_t lineEnd = request.find("\r\n");
    if (lineEnd == std::string::npos) return head;
    std::istringstream firstLine(request.substr(0, lineEnd));
    std::string version;
    if (!(firstLine >> head.method >> head.path >> version)) return head;
    const size_t qmark = head.path.find('?');
    if (qmark != std::string::npos) {
        head.query = head.path.substr(qmark + 1);
        head.path.resize(qmark);
    }
    head.valid = !head.method.empty() && !head.path.empty();
    return head;
}

bool readHttpRequest(int socket, std::string& request, std::string& errorResponse) {
    request.clear();
    errorResponse.clear();
    char buffer[4096];
    while (request.find("\r\n\r\n") == std::string::npos) {
        if (request.size() > kMaxHeaderBytes) {
            errorResponse = httpResponse(413, "application/json",
                                         R"({"error":"headers_too_large"})");
            return false;
        }
        const ssize_t count = recv(socket, buffer, sizeof(buffer), 0);
        if (count <= 0) return false;
        request.append(buffer, size_t(count));
    }
    const std::string length = headerValue(request, "Content-Length");
    const unsigned long long declared =
        length.empty() ? 0 : std::strtoull(length.c_str(), nullptr, 10);
    if (declared > kMaxBodyBytes) {
        errorResponse = httpResponse(413, "application/json",
                                     R"({"error":"body_too_large"})");
        return false;
    }
    const size_t headerEnd = request.find("\r\n\r\n") + 4;
    while (request.size() - headerEnd < declared) {
        const ssize_t count = recv(socket, buffer, sizeof(buffer), 0);
        if (count <= 0) break;
        request.append(buffer, size_t(count));
    }
    return true;
}

std::string base64(const unsigned char* data, size_t size) {
    static const char alphabet[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string result;
    for (size_t i = 0; i < size; i += 3) {
        const uint32_t value = (uint32_t(data[i]) << 16) |
                               (i + 1 < size ? uint32_t(data[i + 1]) << 8 : 0) |
                               (i + 2 < size ? uint32_t(data[i + 2]) : 0);
        result += alphabet[(value >> 18) & 63];
        result += alphabet[(value >> 12) & 63];
        result += i + 1 < size ? alphabet[(value >> 6) & 63] : '=';
        result += i + 2 < size ? alphabet[value & 63] : '=';
    }
    return result;
}

std::string sha1Base64(const std::string& input) {
    std::vector<unsigned char> message(input.begin(), input.end());
    const uint64_t bitLength = uint64_t(message.size()) * 8;
    message.push_back(0x80);
    while ((message.size() % 64) != 56) message.push_back(0);
    for (int shift = 56; shift >= 0; shift -= 8)
        message.push_back(static_cast<unsigned char>((bitLength >> shift) & 0xff));

    uint32_t h0 = 0x67452301, h1 = 0xefcdab89;
    uint32_t h2 = 0x98badcfe, h3 = 0x10325476, h4 = 0xc3d2e1f0;
    for (size_t offset = 0; offset < message.size(); offset += 64) {
        uint32_t words[80]{};
        for (int i = 0; i < 16; ++i) {
            const size_t p = offset + size_t(i) * 4;
            words[i] = (uint32_t(message[p]) << 24) | (uint32_t(message[p + 1]) << 16) |
                       (uint32_t(message[p + 2]) << 8) | uint32_t(message[p + 3]);
        }
        for (int i = 16; i < 80; ++i)
            words[i] = rotateLeft(words[i - 3] ^ words[i - 8] ^ words[i - 14] ^ words[i - 16], 1);
        uint32_t a = h0, b = h1, c = h2, d = h3, e = h4;
        for (int i = 0; i < 80; ++i) {
            uint32_t function = 0, constant = 0;
            if (i < 20) { function = (b & c) | ((~b) & d); constant = 0x5a827999; }
            else if (i < 40) { function = b ^ c ^ d; constant = 0x6ed9eba1; }
            else if (i < 60) { function = (b & c) | (b & d) | (c & d); constant = 0x8f1bbcdc; }
            else { function = b ^ c ^ d; constant = 0xca62c1d6; }
            const uint32_t next = rotateLeft(a, 5) + function + e + constant + words[i];
            e = d; d = c; c = rotateLeft(b, 30); b = a; a = next;
        }
        h0 += a; h1 += b; h2 += c; h3 += d; h4 += e;
    }
    unsigned char digest[20];
    const uint32_t values[] = {h0, h1, h2, h3, h4};
    for (int i = 0; i < 5; ++i)
        for (int j = 0; j < 4; ++j)
            digest[i * 4 + j] = static_cast<unsigned char>((values[i] >> (24 - j * 8)) & 0xff);
    return base64(digest, sizeof(digest));
}

std::string webSocketAccept(const std::string& secWebSocketKey) {
    return sha1Base64(secWebSocketKey + "258EAFA5-E914-47DA-95CA-C5AB0DC85B11");
}

bool sendWebSocketText(int socket, const std::string& text) {
    std::string frame;
    frame.push_back(char(0x81));
    if (text.size() < 126) {
        frame.push_back(char(text.size()));
    } else if (text.size() <= 65535) {
        frame.push_back(126);
        frame.push_back(char((text.size() >> 8) & 0xff));
        frame.push_back(char(text.size() & 0xff));
    } else {
        return false;
    }
    frame += text;
    return sendAll(socket, frame);
}

bool sendWebSocketControl(int socket, unsigned char opcode, const std::string& payload) {
    if (payload.size() > 125) return false;
    std::string frame;
    frame.push_back(char(0x80 | opcode));
    frame.push_back(char(payload.size()));
    frame += payload;
    return sendAll(socket, frame);
}

FrameRead readWebSocketFrame(int socket, WebSocketFrame& frame, size_t maxPayload) {
    // Client frames are always masked; only the bits the server loop needs
    // are decoded. Retry semantics match the original inline reader: the
    // socket has a short SO_RCVTIMEO so the loop can interleave event
    // drains — a timeout BEFORE the header started is idle, a timeout
    // mid-header keeps waiting (the frame is already on its way).
    unsigned char header[2] = {0, 0};
    size_t headerGot = 0;
    while (headerGot < 2) {
        const ssize_t n = recv(socket, header + headerGot, 2 - headerGot, 0);
        if (n <= 0) {
            if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)) {
                if (headerGot == 0) return FrameRead::idle;
                continue;  // partial header: retry the same frame
            }
            return FrameRead::error;
        }
        headerGot += size_t(n);
    }

    const bool masked = (header[1] & 0x80) != 0;
    uint64_t payloadLen = header[1] & 0x7f;
    if (payloadLen == 126) {
        unsigned char ext[2] = {0, 0};
        if (recv(socket, ext, 2, 0) != 2) return FrameRead::error;
        payloadLen = (uint64_t(ext[0]) << 8) | ext[1];
    } else if (payloadLen == 127) {
        unsigned char ext[8] = {0};
        if (recv(socket, ext, 8, 0) != 8) return FrameRead::error;
        payloadLen = 0;
        for (int i = 0; i < 8; ++i) payloadLen = (payloadLen << 8) | ext[i];
    }
    if (payloadLen > maxPayload) return FrameRead::error;

    unsigned char mask[4] = {0, 0, 0, 0};
    if (masked && recv(socket, mask, 4, 0) != 4) return FrameRead::error;
    std::vector<unsigned char> payload((size_t(payloadLen)));
    size_t got = 0;
    while (got < payload.size()) {
        const ssize_t n = recv(socket, payload.data() + got,
                               payload.size() - got, 0);
        if (n <= 0) {
            if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR))
                continue;
            return FrameRead::error;
        }
        got += size_t(n);
    }
    if (masked)
        for (size_t i = 0; i < payload.size(); ++i)
            payload[i] ^= mask[i % 4];

    frame.opcode = header[0] & 0x0f;
    frame.payload.assign(payload.begin(), payload.end());
    return FrameRead::ok;
}

} // namespace http