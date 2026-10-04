#include "render_server.h"
#include "render_gpu.h"
#include "scene_presets.h"
#include "image.h"

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <fstream>
#include <iostream>
#include <sstream>
#include <thread>
#include <vector>
#include <cmath>

#include <stb_image_write.h>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

namespace {

std::string jsonEscape(const std::string& value) {
    std::string result;
    for (char c : value) {
        if (c == '"' || c == '\\') result += '\\';
        if (c == '\n') result += "\\n";
        else if (c == '\r') result += "\\r";
        else result += c;
    }
    return result;
}

std::string stateName(RenderJobState state) {
    switch (state) {
    case RenderJobState::queued: return "queued";
    case RenderJobState::running: return "running";
    case RenderJobState::canceling: return "canceling";
    case RenderJobState::completed: return "completed";
    case RenderJobState::canceled: return "canceled";
    case RenderJobState::failed: return "failed";
    }
    return "unknown";
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

// No escape handling: only used for our own short ids/preset names.
bool jsonString(const std::string& body, const std::string& key, std::string& value) {
    const size_t keyPosition = body.find("\"" + key + "\"");
    if (keyPosition == std::string::npos) return false;
    const size_t colon = body.find(':', keyPosition);
    if (colon == std::string::npos) return false;
    const size_t quote1 = body.find('"', colon);
    if (quote1 == std::string::npos) return false;
    const size_t quote2 = body.find('"', quote1 + 1);
    if (quote2 == std::string::npos) return false;
    value = body.substr(quote1 + 1, quote2 - quote1 - 1);
    return true;
}

// ---- Model whitelist (design 13) ----
// Clients can only request these ids; folder paths are relative to the
// server's working directory (start it from the repository root).
struct ModelSpec {
    const char* id;
    const char* preset;   // default camera preset; empty = engine default
    bool sphere;          // built-in test sphere instead of a model folder
    const char* folder;
    const char* file;
    const char* file2;    // optional second FBX -> combined load (内外景合一)
    const char* sky;
};

const ModelSpec kModels[] = {
    {"sphere-test",     "ball",     true,  "",                   "",                   "", ""},
    {"bistro-interior", "interior", false, "models/Bistro_v5_2", "BistroInterior.fbx", "", "null"},
    {"bistro",          "bistro",   false, "models/Bistro_v5_2", "BistroInterior.fbx",
     "BistroExterior.fbx", "models/Bistro_v5_2/kloppenheim_02_puresky_4k.hdr"},
};

const ModelSpec* findModelSpec(const std::string& id) {
    for (const auto& spec : kModels)
        if (id == spec.id) return &spec;
    return nullptr;
}

// ---- Event JSON (design 8) ----
std::string eventJson(const std::string& type, const std::string& renderId,
                      const std::string& sessionId, uint64_t generation,
                      int sample, int budget, double avgSpp, uint64_t sequence,
                      int width, int height) {
    std::ostringstream e;
    e << "{\"type\":\"" << type << "\"";
    if (!renderId.empty()) e << ",\"renderId\":\"" << renderId << "\"";
    if (!sessionId.empty()) e << ",\"sessionId\":\"" << sessionId << "\"";
    e << ",\"generation\":" << generation
      << ",\"sample\":" << sample << ",\"sampleBudget\":" << budget
      << ",\"avgSpp\":" << avgSpp;
    if (sequence > 0) e << ",\"sequence\":" << sequence;
    if (width > 0) e << ",\"width\":" << width << ",\"height\":" << height;
    if (!renderId.empty() && sequence > 0)
        e << ",\"previewUrl\":\"/api/renders/" << renderId << "/preview?seq=" << sequence << "\"";
    e << "}";
    return e.str();
}

// Cancel semantics (design 7.5): cancelling a terminal job is a no-op, so
// repeated cancels never corrupt the state machine.
void cancelIfActive(const RenderScheduler::Job& job) {
    if (!job) return;
    const RenderJobState s = job->state();
    if (s == RenderJobState::queued || s == RenderJobState::running ||
        s == RenderJobState::canceling)
        job->requestCancel();
}

bool sendAll(int socket, const std::string& data) {
    size_t sent = 0;
    while (sent < data.size()) {
        ssize_t count = send(socket, data.data() + sent, data.size() - sent, MSG_NOSIGNAL);
        if (count <= 0) return false;
        sent += size_t(count);
    }
    return true;
}

std::string httpResponse(int status, const std::string& type, const std::string& body) {
    const char* text = status == 200 ? "OK" : status == 201 ? "Created" :
                       status == 400 ? "Bad Request" : status == 404 ? "Not Found" :
                       status == 405 ? "Method Not Allowed" : "Internal Server Error";
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

uint32_t rotateLeft(uint32_t value, int bits) {
    return (value << bits) | (value >> (32 - bits));
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

// Temporary progressive-verification aid (design 5.4 A/B): dump the raw
// radiance splits so a chunked accumulation can be byte-compared against a
// one-shot render of the same total spp. Enabled only with
// RENDER_SERVER_DUMP_DIR set; image APIs (阶段四) replace this.
void dumpRadiance(const std::string& dir, const std::string& tag,
                  const std::vector<PerPixelData>& output) {
    struct Channel {
        float3 PerPixelData::*field;
        const char* name;
    };
    static const Channel channels[] = {
        {&PerPixelData::radiance_Dd, "Dd"},
        {&PerPixelData::radiance_Ds, "Ds"},
        {&PerPixelData::radiance_Id, "Id"},
        {&PerPixelData::radiance_Is, "Is"},
        {&PerPixelData::emission,    "Em"},
    };
    for (const auto& channel : channels) {
        std::ofstream file(dir + "/" + tag + "_" + channel.name + ".f32",
                           std::ios::binary | std::ios::trunc);
        if (!file) continue;
        for (const auto& pixel : output)
            file.write(reinterpret_cast<const char*>(&(pixel.*channel.field)),
                       sizeof(float3));
    }
}

} // namespace

RenderServer::RenderServer(std::string bindAddress, uint16_t port, std::string token)
    : bindAddress_(std::move(bindAddress)), port_(port), token_(std::move(token)) {
    if (const char* ttl = std::getenv("RENDER_SESSION_TTL"))
        sessionTtlSeconds_ = std::max(5.0, std::strtod(ttl, nullptr));
}

RenderServer::~RenderServer() {
    stop();
}

void RenderServer::stop() {
    stopping_.store(true, std::memory_order_release);
    if (gcThread_.joinable()) gcThread_.join();
    if (listenSocket_ >= 0) {
        shutdown(listenSocket_, SHUT_RDWR);
        close(listenSocket_);
        listenSocket_ = -1;
    }
    scheduler_.shutdown();
}

std::string RenderServer::createSession(const std::string& body, int& status) {
    std::string modelId = "sphere-test";
    jsonString(body, "model", modelId);
    const ModelSpec* spec = findModelSpec(modelId);
    if (!spec) {
        status = 400;
        return R"({"error":"unknown_model"})";
    }
    std::string presetName = spec->preset;
    jsonString(body, "preset", presetName);
    Camera camera;
    float exposure = 1.0f, warmTint = 1.0f, saturationBoost = 1.0f;
    float indoorExposure = 0.0f, indoorMargin = 1.5f;
    float indoorMin[3] = {0.0f, 0.0f, 0.0f};
    float indoorMax[3] = {0.0f, 0.0f, 0.0f};
    if (!presetName.empty()) {
        ScenePreset preset;
        if (!findScenePreset(presetName, preset)) {
            status = 400;
            return R"({"error":"invalid_preset"})";
        }
        camera = preset.camera;
        // Display defaults follow the preset too (design 10.4); the render
        // request may override exposure per call.
        exposure = preset.exposure;
        warmTint = preset.warmTint;
        saturationBoost = preset.saturationBoost;
        indoorExposure = preset.indoorExposure;
        for (int a = 0; a < 3; ++a) {
            indoorMin[a] = preset.indoorMin[a];
            indoorMax[a] = preset.indoorMax[a];
        }
        indoorMargin = preset.indoorMargin;
        // NOTE: the preset's lighting/grade RIG is intentionally NOT applied
        // here. applyPresetRig mutates the process environment (setenv),
        // which is not thread-safe against the GPU worker / other HTTP
        // threads concurrently reading the same variables. Rigs are installed
        // ONCE in run() before any thread exists; a per-session preset only
        // changes camera/exposure, never the rig.
    }

    std::lock_guard<std::mutex> lock(mutex_);
    // session 数上限（设计 13 显存/任务配额）：满时拒绝创建，等待 GC 回收。
    if (sessions_.size() >= 16) {
        status = 429;
        return R"({"error":"too_many_sessions"})";
    }
    auto entry = scenes_.find(modelId);
    bool needLoad = false;
    if (entry == scenes_.end()) {
        SceneEntry newEntry;
        needLoad = true;
        entry = scenes_.emplace(modelId, std::move(newEntry)).first;
    } else if (entry->second.state == SceneEntry::State::failed) {
        // A failed entry retries on the next request.
        needLoad = true;
    }
    if (needLoad) {
        entry->second.state = SceneEntry::State::loading;
        entry->second.error.clear();
        // Loading runs on the GPU worker (design 7.1/11): the HTTP thread
        // only registers the intent; Bistro takes minutes.
        scheduler_.submit(0, [this, modelId](const RenderScheduler::Job&) {
            loadSceneEntry(modelId);
        });
    }
    const bool ready = entry->second.state == SceneEntry::State::ready;

    std::string id = "s-" + std::to_string(nextSessionId_++);
    SessionInfo session;
    session.id = id;
    session.modelId = modelId;
    session.camera = camera;
    session.exposure = exposure;
    session.warmTint = warmTint;
    session.saturationBoost = saturationBoost;
    session.indoorExposure = indoorExposure;
    session.indoorMargin = indoorMargin;
    for (int a = 0; a < 3; ++a) {
        session.indoorMin[a] = indoorMin[a];
        session.indoorMax[a] = indoorMax[a];
    }
    // No CUDA calls in the constructor: buffers are allocated on first
    // render, on the GPU worker thread.
    session.wavefront = std::make_unique<WavefrontRenderContext>();
    sessions_.emplace(id, std::move(session));

    status = 201;
    return "{\"sessionId\":\"" + id + "\",\"model\":\"" + modelId +
           "\",\"state\":\"" + (ready ? "ready" : "loading") + "\"}";
}

void RenderServer::publishEvent(const std::string& sessionId, const std::string& event) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = sessions_.find(sessionId);
    if (it == sessions_.end()) return;
    auto& channels = it->second.eventChannels;
    channels.erase(std::remove_if(channels.begin(), channels.end(),
                                  [](const std::weak_ptr<EventChannel>& wp) {
                                      return wp.expired();
                                  }),
                   channels.end());
    for (auto& weak : channels)
        if (auto channel = weak.lock()) channel->push(event);
}

void RenderServer::publishEventToModel(const std::string& modelId, const std::string& event) {
    std::lock_guard<std::mutex> lock(mutex_);
    for (auto& [_, session] : sessions_) {
        if (session.modelId != modelId) continue;
        auto& channels = session.eventChannels;
        channels.erase(std::remove_if(channels.begin(), channels.end(),
                                      [](const std::weak_ptr<EventChannel>& wp) {
                                          return wp.expired();
                                      }),
                       channels.end());
        for (auto& weak : channels)
            if (auto channel = weak.lock()) channel->push(event);
    }
}

bool RenderServer::originAllowed(const std::string& origin) const {
    if (origin.empty()) return true;   // 非浏览器客户端：无 Origin 可验，token 仍在把关
    if (origin == "null") return true; // file:// 开发页面
    if (origin.rfind("http://127.0.0.1:", 0) == 0 ||
        origin.rfind("http://localhost:", 0) == 0 ||
        origin.rfind("https://127.0.0.1:", 0) == 0 ||
        origin.rfind("https://localhost:", 0) == 0)
        return true;
    if (const char* extra = std::getenv("RENDER_ORIGINS")) {
        std::string list = extra;
        size_t start = 0;
        while (start <= list.size()) {
            const size_t comma = list.find(',', start);
            std::string item = list.substr(
                start, comma == std::string::npos ? std::string::npos : comma - start);
            while (!item.empty() && (item.front() == ' ' || item.front() == '\t'))
                item.erase(item.begin());
            while (!item.empty() && (item.back() == ' ' || item.back() == '\t'))
                item.pop_back();
            if (!item.empty() && item == origin) return true;
            if (comma == std::string::npos) break;
            start = comma + 1;
        }
    }
    return false;
}

std::string RenderServer::queryParam(const std::string& query, const std::string& key) {
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

double RenderServer::renderTimeoutSeconds() const {
    static const double value = [] {
        if (const char* env = std::getenv("RENDER_RENDER_TIMEOUT"))
            return std::max(5.0, std::strtod(env, nullptr));
        return 600.0;
    }();
    return value;
}

// 设计 13：session 过期回收 + 任务超时监视。标记/cancel 在 GC 线程，
// 释放在 GPU worker 上执行（队列串行保证此刻没有该 session 的 chunk 在跑，
// cudaFree 无竞争）。超时通过 cancel 标志在 chunk 内的 sample 边界生效。
void RenderServer::gcLoop() {
    const auto interval = std::chrono::seconds(5);
    while (!stopping_.load(std::memory_order_acquire)) {
        std::this_thread::sleep_for(interval);
        if (stopping_.load(std::memory_order_acquire)) break;
        const auto now = std::chrono::steady_clock::now();
        {
            std::lock_guard<std::mutex> lock(mutex_);
            for (auto& [id, session] : sessions_) {
                const double idle =
                    std::chrono::duration<double>(now - session.lastActivity).count();
                if (idle > sessionTtlSeconds_) cancelIfActive(session.activeJob);
            }
            // 任务超时：cancel 让在跑 chunk 在 sample 边界快速中止。
            for (auto& [_, record] : renders_) {
                if (record.timedOut) continue;
                const RenderJobState s = record.job->state();
                if (s != RenderJobState::running && s != RenderJobState::queued &&
                    s != RenderJobState::canceling)
                    continue;
                const double elapsed = std::chrono::duration<double>(
                    now - record.startTime).count();
                if (elapsed > renderTimeoutSeconds()) {
                    record.timedOut = true;
                    auto sit = sessions_.find(record.sessionId);
                    if (sit != sessions_.end()) cancelIfActive(sit->second.activeJob);
                    std::cerr << "render " << _ << " timed out after "
                              << int(elapsed) << "s\n";
                }
            }
        }
        std::vector<std::string> expired;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            for (auto& [id, session] : sessions_) {
                const double idle =
                    std::chrono::duration<double>(now - session.lastActivity).count();
                if (idle > sessionTtlSeconds_) expired.push_back(id);
            }
        }
        if (expired.empty()) continue;
        scheduler_.submit(0, [this, expired](const RenderScheduler::Job&) {
            std::lock_guard<std::mutex> lock(mutex_);
            for (const auto& id : expired) {
                if (sessions_.erase(id) > 0)
                    std::cerr << "gc: session " << id << " expired\n";
            }
            for (auto it = renders_.begin(); it != renders_.end();) {
                const RenderJobState s = it->second.job->state();
                const bool terminal = s == RenderJobState::completed ||
                                      s == RenderJobState::canceled ||
                                      s == RenderJobState::failed;
                if (sessions_.count(it->second.sessionId) == 0 && terminal)
                    it = renders_.erase(it);
                else
                    ++it;
            }
        });
    }
}

void RenderServer::loadSceneEntry(const std::string& modelId) {
    const ModelSpec* spec = findModelSpec(modelId);
    std::shared_ptr<Model> model;
    std::shared_ptr<GPUScene> scene;
    std::string error;
    try {
        if (spec->sphere) {
            model = std::make_shared<Model>(Model::createSphereTestScene("diffuse"));
        } else if (spec->file2 && spec->file2[0]) {
            // 内外景合一：双 FBX 进同一 BVH/材质/纹理池，窗户内外互通
            std::cerr << "combined load: begin" << std::endl;
            model = std::make_shared<Model>(Model::createCombined(
                spec->folder, {std::string(spec->file), std::string(spec->file2)},
                spec->sky));
            std::cerr << "combined load: model ready, entering GPUScene::load"
                      << std::endl;
        } else {
            model = std::make_shared<Model>(spec->folder, spec->file, spec->sky);
        }
        if (model->faces().empty())
            error = "model_load_failed";
        else
            scene = GPUScene::load(*model);
    } catch (...) {
        error = "scene_load_failed";
    }
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = scenes_.find(modelId);
    if (it == scenes_.end()) return;
    if (error.empty() && scene && scene->ready()) {
        it->second.model = std::move(model);
        it->second.scene = std::move(scene);
        it->second.state = SceneEntry::State::ready;
        std::cerr << "scene registry: '" << modelId << "' ready ("
                  << it->second.model->faces().size() << " faces)\n";
        // session-ready fan-out to every waiting session (design 8)
        for (auto& [_, session] : sessions_) {
            if (session.modelId != modelId) continue;
            for (auto& weak : session.eventChannels)
                if (auto channel = weak.lock())
                    channel->push("{\"type\":\"session-ready\",\"sessionId\":\"" +
                                  session.id + "\",\"model\":\"" + modelId + "\"}");
        }
    } else {
        it->second.state = SceneEntry::State::failed;
        it->second.error = error.empty() ? "scene_upload_failed" : error;
        std::cerr << "scene registry: '" << modelId << "' failed: "
                  << it->second.error << "\n";
        const std::string event = "{\"type\":\"server-error\",\"model\":\"" + modelId +
                                  "\",\"message\":\"" + it->second.error + "\"}";
        for (auto& [_, session] : sessions_) {
            if (session.modelId != modelId) continue;
            for (auto& weak : session.eventChannels)
                if (auto channel = weak.lock()) channel->push(event);
        }
    }
}

std::string RenderServer::sessionStatus(const std::string& id, int& status) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = sessions_.find(id);
    if (it == sessions_.end()) {
        status = 404;
        return R"({"error":"session_not_found"})";
    }
    const SessionInfo& session = it->second;
    std::string state = "ready";
    auto entry = scenes_.find(session.modelId);
    if (entry != scenes_.end() && entry->second.state != SceneEntry::State::ready) {
        state = entry->second.state == SceneEntry::State::loading ? "loading" : "failed";
    } else if (session.activeJob) {
        state = stateName(session.activeJob->state());
    }
    std::ostringstream body;
    body << "{\"sessionId\":\"" << jsonEscape(session.id)
         << "\",\"model\":\"" << jsonEscape(session.modelId)
         << "\",\"generation\":" << session.generation
         << ",\"camera\":{\"position\":[" << session.camera.position.x << ','
         << session.camera.position.y << ',' << session.camera.position.z
         << "],\"direction\":[" << session.camera.direction.x << ','
         << session.camera.direction.y << ',' << session.camera.direction.z
         << "],\"up\":[" << session.camera.up.x << ',' << session.camera.up.y
         << ',' << session.camera.up.z << "],\"fov\":" << session.camera.fov << "}"
         << ",\"state\":\"" << state << "\""
         << ",\"exposure\":" << session.exposure
         << ",\"accumSamples\":" << session.wavefront->nextSample()
         << ",\"accumGeneration\":" << session.accumGeneration
         << ",\"previewSequence\":" << session.previewSequence
         << ",\"previewSpp\":" << session.previewSpp
         << ",\"previewWidth\":" << session.previewWidth
         << ",\"previewHeight\":" << session.previewHeight;
    // Indoor auto-exposure tier parameters (client-side blend; 0 = off)
    if (session.indoorExposure > 0.0f) {
        body << ",\"indoorExposure\":" << session.indoorExposure
             << ",\"indoorMargin\":" << session.indoorMargin
             << ",\"indoorMin\":[" << session.indoorMin[0] << ','
             << session.indoorMin[1] << ',' << session.indoorMin[2] << "]"
             << ",\"indoorMax\":[" << session.indoorMax[0] << ','
             << session.indoorMax[1] << ',' << session.indoorMax[2] << "]";
    }
    body << "}";
    return body.str();
}

std::string RenderServer::updateCamera(const std::string& id, const std::string& body, int& status) {
    Camera camera;
    if (!jsonVector3(body, "position", camera.position) ||
        !jsonVector3(body, "direction", camera.direction) ||
        !jsonVector3(body, "up", camera.up) ||
        !jsonNumber(body, "fov", camera.fov)) {
        status = 400;
        return R"({"error":"invalid_camera","message":"position, direction, up and fov are required"})";
    }
    jsonNumber(body, "focus", camera.focus);
    jsonNumber(body, "coc", camera.coc);
    const float directionLength = glm::length(camera.direction);
    const float upLength = glm::length(camera.up);
    const float alignment = directionLength > 0.0f && upLength > 0.0f
        ? std::abs(glm::dot(glm::normalize(camera.direction), glm::normalize(camera.up))) : 1.0f;
    if (directionLength < 1e-5f || upLength < 1e-5f || alignment >= 0.999f ||
        camera.fov <= 1.0f || camera.fov >= 150.0f) {
        status = 400;
        return R"({"error":"invalid_camera","message":"camera basis or fov is out of range"})";
    }
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = sessions_.find(id);
    if (it == sessions_.end()) {
        status = 404;
        return R"({"error":"session_not_found"})";
    }
    // 速率限制（设计 13）：更新相机（先限流，不做任何状态变更）。
    if (!rateLimiter_.allow("camera", 30, 1)) {
        status = 429;
        return R"({"error":"rate_limited"})";
    }
    it->second.camera = camera;
    it->second.camera.direction = glm::normalize(camera.direction);
    it->second.camera.up = glm::normalize(camera.up);
    it->second.lastActivity = std::chrono::steady_clock::now();
    ++it->second.generation;
    cancelIfActive(it->second.activeJob);
    return "{\"accepted\":true,\"generation\":" + std::to_string(it->second.generation) + "}";
}

std::string RenderServer::submitRender(const std::string& id, const std::string& body, int& status) {
    int width = 320, height = 180, spp = 4, maxDepth = 4, chunkSpp = 0;
    float value = 0.0f;
    if (jsonNumber(body, "width", value)) width = int(value);
    if (jsonNumber(body, "height", value)) height = int(value);
    if (jsonNumber(body, "sppBudget", value)) spp = int(value);
    if (jsonNumber(body, "maxDepth", value)) maxDepth = int(value);
    if (jsonNumber(body, "chunkSpp", value)) chunkSpp = int(value);
    bool resetAccumulation = false;
    if (jsonNumber(body, "resetAccumulation", value)) resetAccumulation = value != 0.0f;
    uint32_t baseSeed = 0x1234;
    if (jsonNumber(body, "seed", value) && value >= 0.0f) baseSeed = uint32_t(value);
    float exposure = 0.0f;   // 0 = keep the session's display exposure
    if (jsonNumber(body, "exposure", value) && value > 0.0f && std::isfinite(value))
        exposure = value;
    // NEE K（设计 17）：未指定=进程默认；有效范围 1..8（kernel 端同样钳制）。
    int neeK = 0;
    if (jsonNumber(body, "neeK", value) && std::isfinite(value))
        neeK = std::min(8, std::max(1, int(value)));
    // 降噪（设计 9"必要时开启降噪"）：精修链开，拖动档保持原始快速帧。
    bool denoise = false;
    if (jsonNumber(body, "denoise", value)) denoise = value != 0.0f;
    // 渐进链（批1）：几何 chunk 计划（2,4,8,16,...），首帧快、编码次数减半；
    // final chunk 用 OIDN-only 降噪 + 无损 PNG。显式 chunkSpp 优先生效。
    bool progressive = false;
    if (jsonNumber(body, "progressive", value)) progressive = value != 0.0f;
    // 自适应逐像素采样（批2）：budget 花在需要的像素上。sanitize 要求
    // adaptiveMin < 每 chunk spp——几何计划下只有大 chunk 实际启用。
    int adaptiveMin = 0, adaptiveCheck = 8;
    float adaptiveThresh = 0.02f;
    if (jsonNumber(body, "adaptiveMin", value))
        adaptiveMin = std::min(4096, std::max(2, int(value)));
    if (jsonNumber(body, "adaptiveCheck", value))
        adaptiveCheck = std::min(1024, std::max(2, int(value)));
    if (jsonNumber(body, "adaptiveThresh", value))
        adaptiveThresh = std::min(1.0f, std::max(1e-4f, value));
    // Merged camera (批1): an optional "camera" object in the render body
    // replaces the separate POST /camera round trip — one RTT per submit.
    // The JSON helpers are substring-based, so the nested camera fields
    // (position/direction/up/fov — no name collisions with render fields)
    // parse straight out of the body.
    const bool hasCamera = body.find("\"camera\"") != std::string::npos;
    Camera cam;
    if (hasCamera) {
        if (!jsonVector3(body, "position", cam.position) ||
            !jsonVector3(body, "direction", cam.direction) ||
            !jsonVector3(body, "up", cam.up) ||
            !jsonNumber(body, "fov", cam.fov)) {
            status = 400;
            return R"({"error":"invalid_camera","message":"position, direction, up and fov are required"})";
        }
        jsonNumber(body, "focus", cam.focus);
        jsonNumber(body, "coc", cam.coc);
        const float dirLen = glm::length(cam.direction);
        const float upLen = glm::length(cam.up);
        const float alignment = dirLen > 0.0f && upLen > 0.0f
            ? std::abs(glm::dot(glm::normalize(cam.direction), glm::normalize(cam.up))) : 1.0f;
        if (dirLen < 1e-5f || upLen < 1e-5f || alignment >= 0.999f ||
            cam.fov <= 1.0f || cam.fov >= 150.0f) {
            status = 400;
            return R"({"error":"invalid_camera","message":"camera basis or fov is out of range"})";
        }
    }
    if (width < 16 || width > 1920 || height < 16 || height > 1080 ||
        spp < 1 || spp > 4096 || maxDepth < 1 || maxDepth > 32 ||
        (chunkSpp != 0 && (chunkSpp < 1 || chunkSpp > spp))) {
        status = 400;
        return R"({"error":"render_limits"})";
    }

    std::lock_guard<std::mutex> lock(mutex_);
    auto it = sessions_.find(id);
    if (it == sessions_.end()) {
        status = 404;
        return R"({"error":"session_not_found"})";
    }
    SessionInfo& session = it->second;
    session.lastActivity = std::chrono::steady_clock::now();
    auto entry = scenes_.find(session.modelId);
    if (entry == scenes_.end() || entry->second.state != SceneEntry::State::ready) {
        status = 409;
        return R"({"error":"scene_not_ready"})";
    }
    // 速率限制（设计 13）：启动 render。
    if (!rateLimiter_.allow("render", 20, 1)) {
        status = 429;
        return R"({"error":"rate_limited"})";
    }
    if (session.activeJob && (session.activeJob->state() == RenderJobState::running ||
                              session.activeJob->state() == RenderJobState::queued))
        cancelIfActive(session.activeJob);
    // Merged camera (批1): apply before the generation read, exactly like
    // POST /camera does — one RTT per submit instead of two.
    if (hasCamera) {
        session.camera = cam;
        session.camera.direction = glm::normalize(cam.direction);
        session.camera.up = glm::normalize(cam.up);
        ++session.generation;
    }
    // Exposure is display-only (10.4): it never resets the accumulation.
    if (exposure > 0.0f) session.exposure = exposure;

    // Reset accumulation when anything radiance-affecting changed
    // (generation, resolution) or the caller asked for it (design 4.2/5.2).
    // The actual reset runs on the GPU worker (runRenderChunk consumes
    // pendingReset): resetAccumulation issues cudaMemset on the context's
    // buffers AND its nextSample.store(0) would race the in-flight chunk's
    // final store (the stale value would win and the new chain would resume
    // at a shifted sample index).
    WavefrontRenderContext* ctx = session.wavefront.get();
    const uint64_t generation = session.generation;
    // renders_ 记录上限：超出时先淘汰终态记录（释放最终图内存）。
    while (renders_.size() > 64) {
        bool evicted = false;
        for (auto it = renders_.begin(); it != renders_.end(); ++it) {
            const RenderJobState s = it->second.job->state();
            if (s == RenderJobState::completed || s == RenderJobState::canceled ||
                s == RenderJobState::failed) {
                renders_.erase(it);
                evicted = true;
                break;
            }
        }
        if (!evicted) break;
    }
    // NEE K 属于"每 sample 积分器质量参数"（设计 4.2）：变化即重置累积。
    const int effectiveNeeK = neeK > 0 ? neeK : defaultNeeK();
    if (resetAccumulation || generation != session.accumGeneration ||
        width != session.accumWidth || height != session.accumHeight ||
        effectiveNeeK != session.accumNeeK) {
        session.pendingReset = true;
        session.accumGeneration = generation;
        session.accumWidth = width;
        session.accumHeight = height;
        session.accumNeeK = effectiveNeeK;
    }

    // Progressive chain: each job renders one chunk of samples; the worker
    // re-submits the next chunk until sppBudget is reached (design 4.3).
    // progressive (批1): explicit chunkSpp wins; otherwise the chain uses the
    // geometric plan (2,4,8,16,...) computed per continuation.
    ChunkTask task;
    task.sessionId = id;
    task.renderId = "r-" + std::to_string(nextRenderId_++);
    task.generation = generation;
    task.scene = entry->second.scene.get();
    task.ctx = ctx;
    task.camera = session.camera;
    task.width = width;
    task.height = height;
    task.spp = spp;
    task.progressive = progressive && chunkSpp == 0;
    task.chunkSpp = chunkSpp > 0 ? chunkSpp
                  : task.progressive ? 2
                  : spp;
    task.maxDepth = maxDepth;
    task.baseSeed = baseSeed;
    task.neeK = effectiveNeeK;
    task.denoise = denoise;
    task.adaptiveMin = adaptiveMin;
    task.adaptiveCheck = adaptiveCheck;
    task.adaptiveThresh = adaptiveThresh;
    task.exposure = session.exposure;
    task.warmTint = session.warmTint;
    task.saturationBoost = session.saturationBoost;
    auto job = scheduler_.submit(generation, [this, task](const RenderScheduler::Job& job) {
        runRenderChunk(task, job);
    });
    session.activeJob = job;
    RenderRecord record;
    record.sessionId = id;
    record.generation = generation;
    record.width = width;
    record.height = height;
    record.job = job;
    renders_[task.renderId] = std::move(record);
    return "{\"accepted\":true,\"renderId\":\"" + task.renderId +
           "\",\"generation\":" + std::to_string(generation) + "}";
}

void RenderServer::runRenderChunk(const ChunkTask& task, const RenderScheduler::Job& job) {
    WavefrontRenderContext* ctx = task.ctx;
    // Consume a pending accumulation reset HERE, on the GPU worker: the
    // reset's cudaMemset must not run on an HTTP thread (submitRender only
    // arms the flag), and doing it before reading nextSample guarantees the
    // new chain starts at sample 0 even when it cancelled an in-flight
    // chunk whose final store would otherwise overwrite the counter
    // (measured: a mid-loop cancel left nextSample=6, and the next chain
    // resumed at start=6 with zeroed accumulators -> wrong radiance).
    {
        std::lock_guard<std::mutex> lock(mutex_);
        auto sit = sessions_.find(task.sessionId);
        if (sit != sessions_.end() && sit->second.pendingReset) {
            sit->second.pendingReset = false;
            ctx->resetAccumulation();
        }
    }
    const int startSample = ctx->nextSample();
    fprintf(stderr, "chunk %s gen%u: cam=(%.2f,%.2f,%.2f) dir=(%.3f,%.3f,%.3f) "
                    "%dx%d spp=%d neeK=%d exp=%.2f\n",
            task.renderId.c_str(), (unsigned)task.generation,
            task.camera.position.x, task.camera.position.y, task.camera.position.z,
            task.camera.direction.x, task.camera.direction.y, task.camera.direction.z,
            task.width, task.height, task.spp, task.neeK, task.exposure);
    try {
        if (startSample >= task.spp) {
            publishEvent(task.sessionId,
                         eventJson("render-completed", task.renderId, task.sessionId,
                                   task.generation, startSample, task.spp,
                                   double(startSample), 0, task.width, task.height));
            return;
        }
        if (startSample == 0)
            publishEvent(task.sessionId,
                         eventJson("render-started", task.renderId, task.sessionId,
                                   task.generation, 0, task.spp, 0.0, 0, task.width, task.height));

        GPURenderArgs args{};
        args.camera = task.camera;
        args.width = task.width;
        args.height = task.height;
        args.spp = std::min(task.chunkSpp, task.spp - startSample);
        args.sppBudgetTotal = task.spp;
        args.pDirect = 0.5f;
        args.maxDepth = task.maxDepth;
        args.baseSeed = task.baseSeed;
        args.neeK = task.neeK;
        // 自适应逐像素采样（批2）：args.adaptive* 由 sanitize 在 wavefront
        // 入口校验（adaptiveMin < 每 chunk spp 时才真正启用）。
        // 已知组合陷阱：自适应收敛像素会把 NL-means 的 varMed/能量统计拖塌
        // （Sdn 10M、降噪中位数≈0，gain 打满 3.0 也救不回——实测 0.102 vs
        // 0.279）。自适应只在 OIDN final 降噪可用时启用；否则整链关闭。
        args.adaptiveMin = task.adaptiveMin;
        args.adaptiveCheck = task.adaptiveCheck;
        args.adaptiveThresh = task.adaptiveThresh;
        if (task.progressive) {
            const char* o = std::getenv("RENDER_OIDN");
            if (!(o && o[0] == '1')) args.adaptiveMin = 0;
        }
        // 方向太阳与 CLI 同源（RENDER_SUN_* env）——Bistro 内景的主直接光，
        // 缺了它内景整体死黑（Dd 假暗 ~20×）。
        applySunFromEnv(args);
        std::vector<PerPixelData> output(size_t(task.width) * task.height);
        const bool perfLog = [] {
            const char* p = std::getenv("RENDER_PERF");
            return !p || p[0] != '0';
        }();
        const auto perfT0 = std::chrono::steady_clock::now();
        renderGPUWavefrontCancelable(args, task.scene->model(), task.scene->skyTexture(),
                                     output.data(), task.width * task.height, *ctx,
                                     &job->cancelFlag());
        const double renderMs = std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - perfT0).count();
        if (perfLog)
            fprintf(stderr, "PERF chunk %s: render=%.0fms spp=%d %dx%d\n",
                    task.renderId.c_str(), renderMs, args.spp,
                    task.width, task.height);

        const int accumulated = ctx->nextSample();
        job->progress().sample = accumulated;
        job->progress().sampleBudget = task.spp;
        job->progress().averageSpp = double(accumulated);

        // 任务超时（设计 12/13）：链超过上限后不再续提交，状态记为 failed。
        bool timedOut = false;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            auto rit = renders_.find(task.renderId);
            if (rit != renders_.end()) {
                const double elapsed = std::chrono::duration<double>(
                    std::chrono::steady_clock::now() - rit->second.startTime).count();
                if (elapsed > renderTimeoutSeconds()) {
                    rit->second.timedOut = true;
                    timedOut = true;
                }
            }
        }
        if (timedOut) {
            publishEvent(task.sessionId,
                         eventJson("render-failed", task.renderId, task.sessionId,
                                   task.generation, accumulated, task.spp,
                                   double(accumulated), 0, task.width, task.height));
            return;
        }

        if (accumulated > startSample && accumulated >= task.spp) {
            if (const char* dir = std::getenv("RENDER_SERVER_DUMP_DIR"))
                dumpRadiance(dir, task.renderId + "_gen" + std::to_string(task.generation) +
                                  "_spp" + std::to_string(task.spp), output);
        }

        // 阶段三 export point: publish a fresh preview after every productive
        // chunk (also on the final one), so browsers can poll sequence bumps.
        if (accumulated > startSample)
            encodePreview(task, output);

        // Terminal states (design 4.3): budget met -> completed, cancel
        // request -> canceled. Continue the chain otherwise; FIFO queue +
        // one chunk per job gives natural round-robin between sessions.
        // (finalPng was stored by encodePreview's final-chunk branch — the
        // lossless PNG must not be overwritten by the JPEG preview.)
        if (accumulated >= task.spp) {
            publishEvent(task.sessionId,
                         eventJson("render-completed", task.renderId, task.sessionId,
                                   task.generation, accumulated, task.spp,
                                   double(accumulated), 0, task.width, task.height));
            return;
        }
        if (job->cancelRequested()) {
            publishEvent(task.sessionId,
                         eventJson("render-canceled", task.renderId, task.sessionId,
                                   task.generation, accumulated, task.spp,
                                   double(accumulated), 0, task.width, task.height));
            return;
        }
        if (accumulated <= startSample) {
            publishEvent(task.sessionId,
                         eventJson("render-failed", task.renderId, task.sessionId,
                                   task.generation, accumulated, task.spp,
                                   double(accumulated), 0, task.width, task.height));
            return;
        }
        {
            std::lock_guard<std::mutex> lock(mutex_);
            auto it = sessions_.find(task.sessionId);
            if (it != sessions_.end() && it->second.generation == task.generation) {
                // Progressive plan (批1): double the chunk size — cumulative
                // 2,6,14,30,62,126 → first preview lands fast and the encode
                // count drops ~2x vs a fixed 8spp cadence. Sample seeds are
                // absolute (design 5.4), so the plan never changes the image.
                ChunkTask next = task;
                if (next.progressive)
                    next.chunkSpp = std::max(2, std::min(
                        {64, accumulated + 2, next.spp - accumulated}));
                auto next_ = scheduler_.submit(task.generation, [this, next](const RenderScheduler::Job& job) {
                    runRenderChunk(next, job);
                });
                it->second.activeJob = next_;
                auto rit = renders_.find(task.renderId);
                if (rit != renders_.end()) rit->second.job = next_;
                return;
            }
        }
        // stale chain: a newer camera/generation owns the session
        publishEvent(task.sessionId,
                     eventJson("render-canceled", task.renderId, task.sessionId,
                               task.generation, accumulated, task.spp,
                               double(accumulated), 0, task.width, task.height));
    } catch (...) {
        publishEvent(task.sessionId,
                     eventJson("render-failed", task.renderId, task.sessionId,
                               task.generation, task.ctx->nextSample(), task.spp,
                               double(task.ctx->nextSample()), 0, task.width, task.height));
        throw;
    }
}

void RenderServer::encodePreview(const ChunkTask& task,
                                 const std::vector<PerPixelData>& output) {
    const int width = task.width, height = task.height;
    // Progressive denoise quality (design 9/批1): intermediate chunks of a
    // progressive chain encode quick (firefly clamp + ONE NL-means pass —
    // the frames are transient and the 7-pass ladder is the single largest
    // CPU item); the final chunk encodes OIDN-only quality 3 (RENDER_OIDN=1;
    // without OIDN linked it degrades to full NL-means). Non-progressive
    // renders keep quality 2 — the calibrated path.
    const bool finalChunk = task.ctx->nextSample() >= task.spp;
    const int denoiseQuality = !task.denoise ? 0
                             : task.progressive ? (finalChunk ? 3 : 1)
                                                : 2;
    Photo photo;
    photo.allocate(width, height);
    photo.exposure = task.exposure;
    photo.warmTint = task.warmTint;
    photo.saturationBoost = task.saturationBoost;

    // 精修链降噪（设计 9）：与 console 完全同源的 radiance/G-buffer 填充，
    // 让预览走同一条 NL-means 降噪路径——否则低 spp 的无降噪预览只剩
    // 高光轮廓（实测反馈）。
    std::vector<RadianceData> radD, radS, radI, radIs;
    std::vector<HitInfo> gbuffer;
    if (task.denoise) {
        const size_t n = output.size();
        radD.resize(n); radS.resize(n); radI.resize(n); radIs.resize(n);
        gbuffer.resize(n);
    }
    // Same composite order as console's Photo fill loop.
    for (size_t i = 0; i < output.size(); ++i) {
        const PerPixelData& ppd = output[i];
        const float3 finalColor = ppd.radiance_Dd + ppd.radiance_Ds
                                + ppd.radiance_Id + ppd.radiance_Is
                                + ppd.emission;
        photo.channels.finalColor[i] = float4(finalColor, 1.0f);
        if (task.denoise) {
            radD[i].radiance = ppd.radiance_Dd; radD[i].Var = ppd.var_Dd;
            radS[i].radiance = ppd.radiance_Ds; radS[i].Var = ppd.var_Ds;
            radI[i].radiance = ppd.radiance_Id; radI[i].Var = ppd.var_Id;
            radIs[i].radiance = ppd.radiance_Is; radIs[i].Var = ppd.var_Is;
            gbuffer[i].surfaceNormal = ppd.normal;
            gbuffer[i].baseColor     = ppd.baseColor;
            gbuffer[i].position      = ppd.position;
            gbuffer[i].shapeNormal   = ppd.shapeNormal;
            gbuffer[i].roughness     = ppd.roughness;
            gbuffer[i].metallic      = ppd.metallic;
            gbuffer[i].specular      = ppd.specular;
            gbuffer[i].opacity       = ppd.opacity;
        }
    }
    if (task.denoise) {
        photo.radiance_Dd = radD.data();
        photo.radiance_Ds = radS.data();
        photo.radiance_Id = radI.data();
        photo.radiance_Is = radIs.data();
        photo.Gbuffer     = gbuffer.data();
    }
    // Bloom follows the console knob (RENDER_BLOOM=0 disables): bright
    // daylight scenes want it off — the Clum>=1 threshold fires on the
    // sunlit street itself and the blur haze washes the frame.
    bool doBloom = true;
    if (const char* bl = std::getenv("RENDER_BLOOM")) doBloom = bl[0] != '0';
    photo.postProcess(Photo::Full |
                      (doBloom ? Photo::DoBloom : 0) |
                      (task.denoise ? Photo::DoDenoise : 0),
                      task.ctx->nextSample(), denoiseQuality);

    const std::vector<uint8_t> rgb = photo.rgbBytes();
    // Progressive previews encode JPEG (quality 90 — 4-5x faster than PNG,
    // the frames are transient); the FINAL frame additionally encodes a
    // lossless PNG for GET /api/renders/{id}/image (delivery stays PNG).
    std::vector<unsigned char> preview, finalPng;
    auto writeBytes = [](void* context, void* data, int size) {
        auto* bytes = static_cast<std::vector<unsigned char>*>(context);
        const unsigned char* p = static_cast<const unsigned char*>(data);
        bytes->insert(bytes->end(), p, p + size);
    };
    stbi_write_jpg_to_func(writeBytes, &preview, width, height, 3,
                           rgb.data(), 90);
    if (finalChunk)
        stbi_write_png_to_func(writeBytes, &finalPng, width, height, 3,
                               rgb.data(), width * 3);
    const bool perfLog = [] {
        const char* p = std::getenv("RENDER_PERF");
        return !p || p[0] != '0';
    }();
    if (perfLog)
        fprintf(stderr, "PERF encode %s: denoiseQ=%d bytes=%zu final=%d\n",
                task.renderId.c_str(), denoiseQuality, preview.size(),
                (int)finalPng.size());

    uint64_t sequence = 0;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = sessions_.find(task.sessionId);
        if (it == sessions_.end()) return;
        SessionInfo& session = it->second;
        session.previewPng = std::move(preview);
        session.previewWidth = width;
        session.previewHeight = height;
        session.previewSpp = task.ctx->nextSample();
        session.previewGeneration = task.generation;
        ++session.previewSequence;
        sequence = session.previewSequence;
        if (session.activeJob)
            session.activeJob->progress().previewSequence = sequence;
        auto rit = renders_.find(task.renderId);
        if (rit != renders_.end()) {
            rit->second.previewPng = session.previewPng;
            rit->second.previewSpp = session.previewSpp;
            rit->second.previewSequence = sequence;
            if (finalChunk) rit->second.finalPng = std::move(finalPng);
        }
    }
    const int accumulated = task.ctx->nextSample();
    publishEvent(task.sessionId,
                 eventJson("progress", task.renderId, task.sessionId,
                           task.generation, accumulated, task.spp,
                           double(accumulated), sequence, width, height));
    publishEvent(task.sessionId,
                 eventJson("preview-ready", task.renderId, task.sessionId,
                           task.generation, accumulated, task.spp,
                           double(accumulated), sequence, width, height));
}

std::string RenderServer::previewData(const std::string& id, int& status,
                                      std::string& contentType) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = sessions_.find(id);
    if (it == sessions_.end()) {
        status = 404;
        return R"({"error":"session_not_found"})";
    }
    if (it->second.previewPng.empty()) {
        status = 404;
        return R"({"error":"no_preview"})";
    }
    status = 200;
    // Progressive previews encode JPEG (quality 90); the final /image stays
    // lossless PNG — see encodePreview.
    contentType = "image/jpeg";
    return std::string(it->second.previewPng.begin(), it->second.previewPng.end());
}

std::string RenderServer::cancelSession(const std::string& id, int& status) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = sessions_.find(id);
    if (it == sessions_.end()) {
        status = 404;
        return R"({"error":"session_not_found"})";
    }
    if (it->second.activeJob) cancelIfActive(it->second.activeJob);
    return R"({"accepted":true})";
}

std::string RenderServer::route(const std::string& method, const std::string& path,
                                const std::string& body, int& status,
                                std::string& contentType) {
    contentType = "application/json";
    if (method == "OPTIONS") return R"({"ok":true})";
    if (method == "GET" && path == "/api/health")
        return R"({"ok":true,"service":"RenderEngine","gpuWorker":true})";
    // 速率限制（设计 13）：创建 session / 更新相机 / 启动 render。
    if (method == "POST" && path == "/api/sessions" &&
        !rateLimiter_.allow("create-session", 10, 60)) {
        status = 429;
        return R"({"error":"rate_limited"})";
    }
    if (method == "POST" && path == "/api/sessions")
        return createSession(body, status);
    const std::string prefix = "/api/sessions/";
    if (path.rfind(prefix, 0) == 0) {
        const std::string id = path.substr(prefix.size());
        if (method == "GET" && id.size() > 8 && id.rfind("/preview") == id.size() - 8)
            return previewData(id.substr(0, id.size() - 8), status, contentType);
        if (method == "GET") return sessionStatus(id, status);
        if (method == "POST" && id.size() > 7 && id.rfind("/render") == id.size() - 7)
            return submitRender(id.substr(0, id.size() - 7), body, status);
        if (method == "POST" && id.size() > 7 && id.rfind("/camera") == id.size() - 7)
            return updateCamera(id.substr(0, id.size() - 7), body, status);
        if (method == "POST" && id.size() > 8 && id.rfind("/cancel") == id.size() - 7)
            return cancelSession(id.substr(0, id.size() - 7), status);
    }
    const std::string rendersPrefix = "/api/renders/";
    if (path.rfind(rendersPrefix, 0) == 0) {
        const std::string id = path.substr(rendersPrefix.size());
        if (method == "GET" && id.size() > 8 && id.rfind("/preview") == id.size() - 8)
            return renderImage(id.substr(0, id.size() - 8), false, status, contentType);
        if (method == "GET" && id.size() > 6 && id.rfind("/image") == id.size() - 6)
            return renderImage(id.substr(0, id.size() - 6), true, status, contentType);
        if (method == "POST" && id.size() > 7 && id.rfind("/cancel") == id.size() - 7)
            return cancelRender(id.substr(0, id.size() - 7), status);
        if (method == "GET") return renderStatus(id, status);
    }
    status = method == "GET" || method == "POST" ? 404 : 405;
    return R"({"error":"not_found"})";
}

void RenderServer::handleClient(int clientSocket) {
    std::string request;
    char buffer[4096];
    while (request.size() < 1024 * 1024) {
        ssize_t count = recv(clientSocket, buffer, sizeof(buffer), 0);
        if (count <= 0) { close(clientSocket); return; }
        request.append(buffer, size_t(count));
        if (request.find("\r\n\r\n") != std::string::npos) break;
    }
    if (request.find("Upgrade: websocket") != std::string::npos) {
        // handleWebSocket owns the socket from here (closes it on every path).
        handleWebSocket(clientSocket, request);
        return;
    }
    size_t lineEnd = request.find("\r\n");
    size_t headerEnd = request.find("\r\n\r\n");
    if (lineEnd == std::string::npos || headerEnd == std::string::npos) {
        sendAll(clientSocket, httpResponse(400, "application/json", R"({"error":"bad_request"})"));
        close(clientSocket); return;
    }
    std::istringstream firstLine(request.substr(0, lineEnd));
    std::string method, path, version;
    firstLine >> method >> path >> version;
    // 拆出 query：此前路由不处理 "?..."，事件里的 previewUrl（带 ?seq=）
    // 一直 404 —— 浏览器 overlay 拉到的全是错误响应。
    std::string query;
    const size_t qmark = path.find('?');
    if (qmark != std::string::npos) {
        query = path.substr(qmark + 1);
        path = path.substr(0, qmark);
    }
    std::string body = request.substr(headerEnd + 4);
    int contentLength = 0;
    std::string length = headerValue(request, "Content-Length");
    if (!length.empty()) contentLength = std::min(1024 * 1024, std::atoi(length.c_str()));
    while (int(body.size()) < contentLength) {
        ssize_t count = recv(clientSocket, buffer, sizeof(buffer), 0);
        if (count <= 0) break;
        body.append(buffer, size_t(count));
    }
    // 访问 token（设计 13）：Authorization: Bearer 或 ?token=（WS/<img> 走 query）。
    // /api/health 保持开放供存活探测。
    if (path != "/api/health") {
        std::string given = headerValue(request, "Authorization");
        if (given.rfind("Bearer ", 0) == 0) given = given.substr(7);
        if (given.empty()) given = queryParam(query, "token");
        if (given != token_) {
            sendAll(clientSocket, httpResponse(401, "application/json",
                                               R"({"error":"unauthorized"})"));
            close(clientSocket);
            return;
        }
    }
    int status = 200;
    std::string type;
    sendAll(clientSocket, httpResponse(status, type, route(method, path, body, status, type)));
    close(clientSocket);
}

std::string RenderServer::renderStatus(const std::string& renderId, int& status) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = renders_.find(renderId);
    if (it == renders_.end()) {
        status = 404;
        return R"({"error":"render_not_found"})";
    }
    const RenderRecord& record = it->second;
    std::ostringstream body;
    body << "{\"renderId\":\"" << renderId
         << "\",\"sessionId\":\"" << record.sessionId
         << "\",\"generation\":" << record.generation
         << ",\"state\":\"" << (record.timedOut ? "failed" : stateName(record.job->state())) << "\""
         << ",\"sample\":" << record.job->progress().sample
         << ",\"sampleBudget\":" << record.job->progress().sampleBudget
         << ",\"avgSpp\":" << record.job->progress().averageSpp
         << ",\"previewSequence\":" << record.previewSequence
         << ",\"previewUrl\":\"/api/renders/" << renderId
         << "/preview?seq=" << record.previewSequence << "\"}";
    return body.str();
}

std::string RenderServer::cancelRender(const std::string& renderId, int& status) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = renders_.find(renderId);
    if (it == renders_.end()) {
        status = 404;
        return R"({"error":"render_not_found"})";
    }
    cancelIfActive(it->second.job);
    return "{\"accepted\":true,\"state\":\"" + stateName(it->second.job->state()) + "\"}";
}

std::string RenderServer::renderImage(const std::string& renderId, bool finalImage,
                                      int& status, std::string& contentType) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = renders_.find(renderId);
    if (it == renders_.end()) {
        status = 404;
        return R"({"error":"render_not_found"})";
    }
    const std::vector<unsigned char>& png =
        finalImage ? it->second.finalPng : it->second.previewPng;
    if (png.empty()) {
        status = 404;
        return R"({"error":"no_preview"})";
    }
    status = 200;
    // /image = the final lossless PNG; per-chunk previews are JPEG.
    contentType = finalImage ? "image/png" : "image/jpeg";
    return std::string(png.begin(), png.end());
}

bool RenderServer::handleWebSocket(int clientSocket, const std::string& request) {
    // Path validation (design 8): only /api/sessions/{id}/events upgrades.
    std::string path, query;
    {
        const size_t lineEnd = request.find("\r\n");
        std::istringstream firstLine(request.substr(0, lineEnd));
        std::string method, version;
        firstLine >> method >> path >> version;
        const size_t qmark = path.find('?');
        if (qmark != std::string::npos) {
            query = path.substr(qmark + 1);
            path = path.substr(0, qmark);
        }
    }
    const std::string eventsPrefix = "/api/sessions/";
    const std::string eventsSuffix = "/events";
    std::string sessionId;
    if (path.rfind(eventsPrefix, 0) == 0 &&
        path.size() > eventsPrefix.size() + eventsSuffix.size() &&
        path.rfind(eventsSuffix) == path.size() - eventsSuffix.size())
        sessionId = path.substr(eventsPrefix.size(),
                                path.size() - eventsPrefix.size() - eventsSuffix.size());
    // Origin 校验（设计 13）：回环来源、file://（Origin: null）与
    // RENDER_ORIGINS 白名单之外的一律拒绝。
    if (!originAllowed(headerValue(request, "Origin"))) {
        sendAll(clientSocket, httpResponse(403, "application/json",
                                           R"({"error":"origin_rejected"})"));
        close(clientSocket);
        return false;
    }
    // token：query（浏览器 WS 无法设头）或 Authorization 头。
    {
        std::string given = queryParam(query, "token");
        if (given.empty()) {
            given = headerValue(request, "Authorization");
            if (given.rfind("Bearer ", 0) == 0) given = given.substr(7);
        }
        if (given != token_) {
            sendAll(clientSocket, httpResponse(401, "application/json",
                                               R"({"error":"unauthorized"})"));
            close(clientSocket);
            return false;
        }
    }
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (sessionId.empty() || !sessions_.count(sessionId)) {
            sendAll(clientSocket, httpResponse(404, "application/json",
                                               R"({"error":"session_not_found"})"));
            close(clientSocket);
            return false;
        }
    }

    const std::string key = headerValue(request, "Sec-WebSocket-Key");
    if (key.empty()) {
        close(clientSocket);
        return false;
    }
    const std::string accept = sha1Base64(key + "258EAFA5-E914-47DA-95CA-C5AB0DC85B11");
    const std::string response =
        "HTTP/1.1 101 Switching Protocols\r\n"
        "Upgrade: websocket\r\nConnection: Upgrade\r\n"
        "Sec-WebSocket-Accept: " + accept + "\r\n\r\n";
    if (!sendAll(clientSocket, response)) {
        close(clientSocket);
        return false;
    }

    // Register this connection's channel; send a session-ready snapshot if
    // the scene is already resident. Loading sessions receive session-ready
    // through the channel when the GPU worker finishes the load.
    auto channel = std::make_shared<EventChannel>();
    {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = sessions_.find(sessionId);
        if (it == sessions_.end()) {
            close(clientSocket);
            return false;
        }
        auto& channels = it->second.eventChannels;
        channels.erase(std::remove_if(channels.begin(), channels.end(),
                                      [](const std::weak_ptr<EventChannel>& wp) {
                                          return wp.expired();
                                      }),
                       channels.end());
        channels.push_back(channel);
        auto entry = scenes_.find(it->second.modelId);
        if (entry != scenes_.end() && entry->second.state == SceneEntry::State::ready)
            channel->push("{\"type\":\"session-ready\",\"sessionId\":\"" + it->second.id +
                          "\",\"model\":\"" + it->second.modelId + "\"}");
    }

    // Idle receive timeout: lets the loop drain the event queue while the
    // browser sends nothing.
    timeval tv{};
    tv.tv_sec = 0;
    tv.tv_usec = 200 * 1000;
    setsockopt(clientSocket, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    constexpr size_t kMaxClientPayload = 1 << 20;
    bool alive = true;
    while (alive && !stopping_.load(std::memory_order_acquire)) {
        // drain outbound events first
        std::string event;
        while (channel->tryPop(event))
            if (!sendWebSocketText(clientSocket, event)) { alive = false; break; }
        if (!alive) break;

        // read one client frame (masked; only Ping/Close are interpreted)
        unsigned char header[2] = {0, 0};
        size_t headerGot = 0;
        bool idle = false;
        while (headerGot < 2) {
            const ssize_t n = recv(clientSocket, header + headerGot, 2 - headerGot, 0);
            if (n <= 0) {
                if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)) {
                    if (headerGot == 0) { idle = true; break; }
                    continue;   // partial header: retry the same frame
                }
                headerGot = 99;
                break;
            }
            headerGot += size_t(n);
        }
        if (idle) continue;
        if (headerGot != 2) break;

        const unsigned char opcode = header[0] & 0x0f;
        const bool masked = (header[1] & 0x80) != 0;
        uint64_t payloadLen = header[1] & 0x7f;
        if (payloadLen == 126) {
            unsigned char ext[2] = {0, 0};
            if (recv(clientSocket, ext, 2, 0) != 2) break;
            payloadLen = (uint64_t(ext[0]) << 8) | ext[1];
        } else if (payloadLen == 127) {
            unsigned char ext[8] = {0};
            if (recv(clientSocket, ext, 8, 0) != 8) break;
            payloadLen = 0;
            for (int i = 0; i < 8; ++i) payloadLen = (payloadLen << 8) | ext[i];
        }
        if (payloadLen > kMaxClientPayload) break;

        unsigned char mask[4] = {0, 0, 0, 0};
        if (masked && recv(clientSocket, mask, 4, 0) != 4) break;
        std::vector<unsigned char> payload((size_t(payloadLen)));
        size_t got = 0;
        while (got < payload.size()) {
            const ssize_t n = recv(clientSocket, payload.data() + got,
                                   payload.size() - got, 0);
            if (n <= 0) {
                if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR))
                    continue;
                got = payload.size() + 1;
                break;
            }
            got += size_t(n);
        }
        if (got != payload.size()) break;
        if (masked)
            for (size_t i = 0; i < payload.size(); ++i)
                payload[i] ^= mask[i % 4];

        if (opcode == 0x8) break;   // Close
        if (opcode == 0x9) {        // Ping -> Pong (echo payload)
            std::string pong;
            pong.push_back(char(0x8a));
            if (payload.size() < 126) {
                pong.push_back(char(payload.size()));
            } else if (payload.size() <= 0xffff) {
                pong.push_back(126);
                pong.push_back(char((payload.size() >> 8) & 0xff));
                pong.push_back(char(payload.size() & 0xff));
            } else {
                break;
            }
            pong += std::string(payload.begin(), payload.end());
            if (!sendAll(clientSocket, pong)) alive = false;
        }
        // client text/binary frames are not interpreted in this version
    }
    channel->close();
    close(clientSocket);
    return true;
}

int RenderServer::run() {
    // Install the whitelisted scenes' calibrated rigs ONCE, before any
    // thread exists (the scheduler worker starts in the constructor but is
    // idle until a session submits; GC and HTTP threads start below).
    // applyPresetRig calls setenv, which is not thread-safe against
    // concurrent getenv — so it must never run from createSession on an
    // HTTP thread. Pre-launch env still overrides (rig never overwrites a
    // variable the operator set explicitly).
    for (const auto& spec : kModels) {
        ScenePreset preset;
        if (findScenePreset(spec.preset, preset) && preset.rigEnv) {
            std::vector<std::string> installed;
            applyPresetRig(preset, &installed);
            for (const auto& kv : installed)
                std::cerr << "[startup rig:" << spec.preset << "] " << kv << "\n";
        }
    }
    listenSocket_ = socket(AF_INET, SOCK_STREAM, 0);
    if (listenSocket_ < 0) return 1;
    int reuse = 1;
    setsockopt(listenSocket_, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = htons(port_);
    if (inet_pton(AF_INET, bindAddress_.c_str(), &address.sin_addr) != 1 ||
        bind(listenSocket_, reinterpret_cast<sockaddr*>(&address), sizeof(address)) < 0 ||
        listen(listenSocket_, 16) < 0) {
        stop();
        return 1;
    }
    gcThread_ = std::thread(&RenderServer::gcLoop, this);
    while (!stopping_.load(std::memory_order_acquire)) {
        int client = accept(listenSocket_, nullptr, nullptr);
        if (client < 0) {
            if (stopping_.load(std::memory_order_acquire)) break;
            if (errno == EINTR) continue;
            continue;
        }
        std::thread(&RenderServer::handleClient, this, client).detach();
    }
    return 0;
}