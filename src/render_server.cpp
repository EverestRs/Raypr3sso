#include "render_server.h"
#include "scene_progress.h"
#include "render_gpu.h"
#include "scene_presets.h"
#include "image.h"
#include "http_util.h"

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <fstream>
#include <iostream>
#include <sstream>
#include <cstdio>
#include <thread>
#include <vector>
#include <cmath>

#include <stb_image_write.h>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

using http::headerValue;
using http::httpResponse;
using http::jsonEscape;
using http::jsonNumber;
using http::jsonString;
using http::jsonVector3;
using http::queryParam;
using http::sendAll;
using http::sendWebSocketText;

namespace {

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
     "BistroExterior.fbx", "models/Bistro_v5_2/qwantani_azm55_4k.hdr"},
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
    e << "{\"type\":\"" << jsonEscape(type) << "\"";
    if (!renderId.empty()) e << ",\"renderId\":\"" << jsonEscape(renderId) << "\"";
    if (!sessionId.empty()) e << ",\"sessionId\":\"" << jsonEscape(sessionId) << "\"";
    e << ",\"generation\":" << generation
      << ",\"sample\":" << sample << ",\"sampleBudget\":" << budget
      << ",\"avgSpp\":" << avgSpp;
    if (sequence > 0) e << ",\"sequence\":" << sequence;
    if (width > 0) e << ",\"width\":" << width << ",\"height\":" << height;
    if (!renderId.empty() && sequence > 0)
        e << ",\"previewUrl\":\"" << jsonEscape("/api/renders/" + renderId +
                                               "/preview?seq=" + std::to_string(sequence))
          << "\"";
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
    s_self = this;   // singleton in practice; the progress sink publishes through it
    encodeThread_ = std::thread([this] { encodeLoop(); });
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
    // Encode thread last: it depends on nothing above, but drains queued
    // previews (incl. a final PNG) before exiting so a shutdown mid-chain
    // still lands its bytes.
    {
        std::lock_guard<std::mutex> lock(encodeMutex_);
        encodeStopping_ = true;
    }
    encodeCv_.notify_all();
    if (encodeThread_.joinable()) encodeThread_.join();
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
    RigProfile indoorProf, outdoorProf;
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
        // Position-aware rig profiles: PURE DATA parsing of the rig lists
        // (no setenv — safe on the HTTP thread). submitRender picks one by
        // camera position and delivers it through the task channel.
        indoorProf = parseRigProfile(preset.rigEnv);
        outdoorProf = parseRigProfile(preset.rigEnvOutdoor);
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
    session.config.exposure = exposure;
    session.config.warmTint = warmTint;
    session.config.saturationBoost = saturationBoost;
    session.indoorExposure = indoorExposure;
    session.indoorMargin = indoorMargin;
    for (int a = 0; a < 3; ++a) {
        session.indoorMin[a] = indoorMin[a];
        session.indoorMax[a] = indoorMax[a];
    }
    session.indoorProf = std::move(indoorProf);
    session.outdoorProf = std::move(outdoorProf);
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

// ---- scene-load progress sink (P2-11) ----
RenderServer* RenderServer::s_self = nullptr;
std::string RenderServer::s_loadingModelId;
std::string RenderServer::s_lastStage;
std::chrono::steady_clock::time_point RenderServer::s_lastEmit{};

void RenderServer::sceneProgressSink(const char* stage, int done, int total) {
    if (!s_self || s_loadingModelId.empty()) return;
    const auto now = std::chrono::steady_clock::now();
    const std::string st(stage);
    const bool boundary = done == 0 || done == total || st != s_lastStage;
    if (!boundary && now - s_lastEmit < std::chrono::milliseconds(400)) return;
    s_lastStage = st;
    s_lastEmit = now;
    std::ostringstream e;
    e << "{\"type\":\"scene-progress\",\"model\":\"" << jsonEscape(s_loadingModelId)
      << "\",\"stage\":\"" << st << "\",\"done\":" << done
      << ",\"total\":" << total << "}";
    s_self->publishEventToModel(s_loadingModelId, e.str());
}

void RenderServer::loadSceneEntry(const std::string& modelId) {
    const ModelSpec* spec = findModelSpec(modelId);
    std::shared_ptr<Model> model;
    std::shared_ptr<GPUScene> scene;
    std::string error;
    s_loadingModelId = modelId;
    s_lastStage.clear();
    s_lastEmit = {};
    sceneProgress::set(&sceneProgressSink);
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
    sceneProgress::set(nullptr);
    s_loadingModelId.clear();
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
         << ',' << session.camera.up.z << "],\"fov\":" << session.camera.fov
         << ",\"focus\":" << session.camera.focus
         << ",\"coc\":" << session.camera.coc << "}"
         << ",\"state\":\"" << state << "\""
         << ",\"exposure\":" << session.config.exposure
         << ",\"warmTint\":" << session.config.warmTint
         << ",\"saturationBoost\":" << session.config.saturationBoost
         << ",\"bloom\":" << session.config.bloom
         << ",\"sun\":{\"has\":" << (session.config.sunHas ? "true" : "false")
         << ",\"az\":" << session.config.sunAz
         << ",\"el\":" << session.config.sunEl
         << ",\"int\":" << session.config.sunInt << "}"
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
    RenderParams p;
    const std::string error = parseRenderParams(body, p, status);
    if (!error.empty()) return error;

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
    // Camera / display / sun merge into the session (10.4, A1/A3).
    bool displayChanged = false, sunChanged = false;
    mergeSessionOverridesLocked(session, p, displayChanged, sunChanged);
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
    // 太阳同理（A3，radiance-affecting）。
    const int effectiveNeeK = p.neeK > 0 ? p.neeK : defaultNeeK();
    const bool resetArmed = armAccumulationResetLocked(session, p, effectiveNeeK,
                                                      sunChanged, generation);
    // A2: display-only change with the accumulation intact — if the chain
    // already reached its budget, the worker runs one 0-sample re-encode
    // instead of restarting (a still-running chain just encodes the next
    // chunk with the new display params).
    const bool reencode = displayChanged && !resetArmed;
    // Progressive chain: each job renders one chunk of samples; the worker
    // re-submits the next chunk until sppBudget is reached (design 4.3).
    ChunkTask task = buildChunkTaskLocked(id, session, entry->second, p,
                                          effectiveNeeK, generation, reencode);
    auto job = scheduler_.submit(generation, [this, task](const RenderScheduler::Job& job) {
        runRenderChunk(task, job);
    });
    session.activeJob = job;
    RenderRecord record;
    record.sessionId = id;
    record.generation = generation;
    record.width = p.width;
    record.height = p.height;
    record.job = job;
    renders_[task.renderId] = std::move(record);
    return "{\"accepted\":true,\"renderId\":\"" + task.renderId +
           "\",\"generation\":" + std::to_string(generation) + "}";
}

// ---- submitRender / runRenderChunk stages. Pure extraction: same order,
// ---- same semantics; the orchestrators only sequence the steps now.

std::string RenderServer::parseRenderParams(const std::string& body, RenderParams& p,
                                            int& status) {
    float value = 0.0f;
    if (jsonNumber(body, "width", value)) p.width = int(value);
    if (jsonNumber(body, "height", value)) p.height = int(value);
    if (jsonNumber(body, "sppBudget", value)) p.spp = int(value);
    if (jsonNumber(body, "maxDepth", value)) p.maxDepth = int(value);
    if (jsonNumber(body, "chunkSpp", value)) p.chunkSpp = int(value);
    if (jsonNumber(body, "resetAccumulation", value)) p.resetAccumulation = value != 0.0f;
    if (jsonNumber(body, "seed", value) && value >= 0.0f) p.baseSeed = uint32_t(value);
    // 0 = keep the session's display exposure
    if (jsonNumber(body, "exposure", value) && value > 0.0f && std::isfinite(value))
        p.exposure = value;
    // NEE K（设计 17）：未指定=进程默认；有效范围 1..8（kernel 端同样钳制）。
    if (jsonNumber(body, "neeK", value) && std::isfinite(value))
        p.neeK = std::min(8, std::max(1, int(value)));
    // 降噪（设计 9"必要时开启降噪"）：精修链开，拖动档保持原始快速帧。
    if (jsonNumber(body, "denoise", value)) p.denoise = value != 0.0f;
    // 渐进链（批1）：几何 chunk 计划（2,4,8,16,...），首帧快、编码次数减半；
    // final chunk 用 OIDN-only 降噪 + 无损 PNG。显式 chunkSpp 优先生效。
    if (jsonNumber(body, "progressive", value)) p.progressive = value != 0.0f;
    // 自适应逐像素采样（批2）：budget 花在需要的像素上。sanitize 要求
    // adaptiveMin < 每 chunk spp——几何计划下只有大 chunk 实际启用。
    if (jsonNumber(body, "adaptiveMin", value))
        p.adaptiveMin = std::min(4096, std::max(2, int(value)));
    if (jsonNumber(body, "adaptiveCheck", value))
        p.adaptiveCheck = std::min(1024, std::max(2, int(value)));
    if (jsonNumber(body, "adaptiveThresh", value))
        p.adaptiveThresh = std::min(1.0f, std::max(1e-4f, value));
    // Display-only params (A1, 10.4): absent/0 = keep the session's value.
    // Changing any of these NEVER resets the accumulation — they are applied
    // at preview-encode time only.
    if (jsonNumber(body, "warmTint", value) && value > 0.0f && std::isfinite(value))
        p.warmTint = std::min(4.0f, std::max(0.25f, value));
    if (jsonNumber(body, "saturationBoost", value) && value >= 0.0f && std::isfinite(value))
        p.saturationBoost = std::min(4.0f, value);
    if (jsonNumber(body, "bloom", value)) p.bloom = value != 0.0f ? 1 : 0;
    // Directional sun override (A3): any of the three keys arms the session
    // override (each key merges over the previous values). sunInt=0 = sun
    // off. Radiance-affecting: a change resets the accumulation (4.2).
    if (jsonNumber(body, "sunAz", value) && std::isfinite(value)) {
        p.sunAz = value; p.sunAzSet = true;
    }
    if (jsonNumber(body, "sunEl", value) && std::isfinite(value)) {
        p.sunEl = value; p.sunElSet = true;
    }
    if (jsonNumber(body, "sunInt", value) && std::isfinite(value)) {
        p.sunInt = value; p.sunIntSet = true;
    }
    // Merged camera (批1): an optional "camera" object in the render body
    // replaces the separate POST /camera round trip — one RTT per submit.
    // The JSON helpers are substring-based, so the nested camera fields
    // (position/direction/up/fov — no name collisions with render fields)
    // parse straight out of the body.
    p.hasCamera = body.find("\"camera\"") != std::string::npos;
    Camera& cam = p.camera;
    if (p.hasCamera) {
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
    if (p.width < 16 || p.width > 1920 || p.height < 16 || p.height > 1080 ||
        p.spp < 1 || p.spp > 4096 || p.maxDepth < 1 || p.maxDepth > 32 ||
        (p.chunkSpp != 0 && (p.chunkSpp < 1 || p.chunkSpp > p.spp))) {
        status = 400;
        return R"({"error":"render_limits"})";
    }
    return "";
}

void RenderServer::mergeSessionOverridesLocked(SessionInfo& session, const RenderParams& p,
                                               bool& displayChanged, bool& sunChanged) {
    // Merged camera (批1): apply before the generation read, exactly like
    // POST /camera does — one RTT per submit instead of two. An IDENTICAL
    // camera (position/basis/fov/focus/coc within epsilon) does NOT bump
    // the generation: display-only re-submits (exposure/bloom/… tweaks)
    // must keep the accumulation (10.4) — previously any /render carrying a
    // camera restarted from sample 0 and threw away the refinement.
    if (p.hasCamera) {
        const Camera& cam = p.camera;
        const glm::vec3 nd = glm::normalize(cam.direction);
        const glm::vec3 nu = glm::normalize(cam.up);
        const Camera& cur = session.camera;
        const bool cameraMoved =
            glm::length(cam.position - cur.position) > 1e-4f ||
            glm::length(nd - cur.direction) > 1e-4f ||
            glm::length(nu - cur.up) > 1e-4f ||
            std::abs(cam.fov - cur.fov) > 1e-3f ||
            std::abs(cam.focus - cur.focus) > 1e-3f ||
            std::abs(cam.coc - cur.coc) > 1e-3f;
        if (cameraMoved) {
            session.camera = cam;
            session.camera.direction = nd;
            session.camera.up = nu;
            ++session.generation;
        }
    }
    // Display-only params (10.4/A1): applied at encode time, never reset.
    displayChanged =
        (p.exposure > 0.0f && std::abs(p.exposure - session.config.exposure) > 1e-4f) ||
        (p.warmTint > 0.0f && std::abs(p.warmTint - session.config.warmTint) > 1e-4f) ||
        (p.saturationBoost > 0.0f &&
         std::abs(p.saturationBoost - session.config.saturationBoost) > 1e-4f) ||
        (p.bloom >= 0 && p.bloom != session.config.bloom);
    if (p.exposure > 0.0f) session.config.exposure = p.exposure;
    if (p.warmTint > 0.0f) session.config.warmTint = p.warmTint;
    if (p.saturationBoost > 0.0f) session.config.saturationBoost = p.saturationBoost;
    if (p.bloom >= 0) session.config.bloom = p.bloom;
    // Sun override merge (A3): compare before storing to detect changes.
    sunChanged = false;
    if (p.sunAzSet || p.sunElSet || p.sunIntSet) {
        const bool hadSun = session.config.sunHas;
        const float oAz = session.config.sunAz, oEl = session.config.sunEl,
                    oInt = session.config.sunInt;
        if (p.sunAzSet)
            session.config.sunAz = std::fmod(std::fmod(p.sunAz, 360.0f) + 360.0f, 360.0f);
        if (p.sunElSet)
            session.config.sunEl = std::min(90.0f, std::max(-90.0f, p.sunEl));
        if (p.sunIntSet)
            session.config.sunInt = std::min(200.0f, std::max(0.0f, p.sunInt));
        sunChanged = !hadSun ||
                     std::abs(session.config.sunAz - oAz) > 1e-3f ||
                     std::abs(session.config.sunEl - oEl) > 1e-3f ||
                     std::abs(session.config.sunInt - oInt) > 1e-3f;
        session.config.sunHas = true;
    }
}

// Reset accumulation when anything radiance-affecting changed
// (generation, resolution) or the caller asked for it (design 4.2/5.2).
// The actual reset runs on the GPU worker (runRenderChunk consumes
// pendingReset): resetAccumulation issues cudaMemset on the context's
// buffers AND its nextSample.store(0) would race the in-flight chunk's
// final store (the stale value would win and the new chain would resume
// at a shifted sample index).
bool RenderServer::armAccumulationResetLocked(SessionInfo& session, const RenderParams& p,
                                              int effectiveNeeK, bool sunChanged,
                                              uint64_t generation) {
    // NEE K 属于"每 sample 积分器质量参数"（设计 4.2）：变化即重置累积。
    // 太阳同理（A3，radiance-affecting）。
    const bool sunDiffers = session.config.sunHas != session.accumSunHas ||
        (session.config.sunHas &&
         (std::abs(session.config.sunAz - session.accumSunAz) > 1e-3f ||
          std::abs(session.config.sunEl - session.accumSunEl) > 1e-3f ||
          std::abs(session.config.sunInt - session.accumSunInt) > 1e-3f));
    bool resetArmed = false;
    if (p.resetAccumulation || generation != session.accumGeneration ||
        p.width != session.accumWidth || p.height != session.accumHeight ||
        effectiveNeeK != session.accumNeeK || sunChanged || sunDiffers) {
        session.pendingReset = true;
        session.accumGeneration = generation;
        session.accumWidth = p.width;
        session.accumHeight = p.height;
        session.accumNeeK = effectiveNeeK;
        session.accumSunHas = session.config.sunHas;
        session.accumSunAz = session.config.sunAz;
        session.accumSunEl = session.config.sunEl;
        session.accumSunInt = session.config.sunInt;
        resetArmed = true;
    }
    return resetArmed;
}

RenderServer::ChunkTask RenderServer::buildChunkTaskLocked(const std::string& id, SessionInfo& session,
                                             SceneEntry& entry, const RenderParams& p,
                                             int effectiveNeeK, uint64_t generation,
                                             bool reencode) {
    // Position-aware rig profile: pick by the EFFECTIVE camera position.
    // indoor = inside the interior box grown by indoorMargin — the same box
    // the client's exposure tier blends on. Pure data on the task channel;
    // no setenv anywhere near this (HTTP thread).
    const RigProfile* prof = nullptr;
    if (session.outdoorProf.valid) {
        const float3& pos = session.camera.position;
        const float m = session.indoorMargin;
        const bool indoor =
            pos.x >= session.indoorMin[0] - m && pos.x <= session.indoorMax[0] + m &&
            pos.y >= session.indoorMin[1] - m && pos.y <= session.indoorMax[1] + m &&
            pos.z >= session.indoorMin[2] - m && pos.z <= session.indoorMax[2] + m;
        prof = indoor ? &session.indoorProf : &session.outdoorProf;
        session.activeOutdoor = !indoor;  // observability; crossing the
        // boundary is a camera change, which already resets the accumulation
    }
    ChunkTask task;
    task.sessionId = id;
    task.renderId = "r-" + std::to_string(nextRenderId_++);
    task.generation = generation;
    task.scene = entry.scene.get();
    task.ctx = session.wavefront.get();
    task.camera = session.camera;
    task.width = p.width;
    task.height = p.height;
    task.spp = p.spp;
    task.progressive = p.progressive && p.chunkSpp == 0;
    task.chunkSpp = p.chunkSpp > 0 ? p.chunkSpp
                  : task.progressive ? 2
                  : p.spp;
    task.maxDepth = p.maxDepth;
    task.baseSeed = p.baseSeed;
    task.neeK = effectiveNeeK;
    task.denoise = p.denoise;
    task.adaptiveMin = p.adaptiveMin;
    task.adaptiveCheck = p.adaptiveCheck;
    task.adaptiveThresh = p.adaptiveThresh;
    task.config = session.config;
    task.reencode = reencode;
    if (prof) {
        task.hasProfile = true;
        const RenderConfig& pc = prof->config;
        task.config.emissionScale = pc.emissionScale;
        task.config.indirectScale = pc.indirectScale;
        task.config.wbStrength = pc.wbStrength;
        task.config.splitKeySun = pc.splitKeySun;
        task.config.splitWarm = pc.splitWarm;
        task.config.splitCool = pc.splitCool;
        task.config.gradeBands = pc.gradeBands;
        task.config.useMipmaps = pc.useMipmaps;
        task.config.anisoMax = pc.anisoMax;
        task.config.lodScale = pc.lodScale;
        // Client-pinned saturation (slider) wins over the profile default.
        if (session.config.saturationBoost != 1.0f)
            task.config.saturationBoost = session.config.saturationBoost;
        else
            task.config.saturationBoost = pc.saturationBoost;
        // Sun: the client's explicit override (A3) wins; otherwise the
        // profile arms it.
        if (!session.config.sunHas && pc.sunHas) {
            task.config.sunHas = true;
            task.config.sunAz = pc.sunAz;
            task.config.sunEl = pc.sunEl;
            task.config.sunInt = pc.sunInt;
        }
        // Exposure default when the body sent none: the indoor tier value
        // for the indoor profile, the outdoor base otherwise. (The browser
        // client always sends its blended tier exposure, which wins here.)
        if (p.exposure <= 0.0f)
            task.config.exposure = (prof == &session.indoorProf && session.indoorExposure > 0.0f)
                          ? session.indoorExposure : session.config.exposure;
    }
    return task;
}


void RenderServer::runRenderChunk(const ChunkTask& task, const RenderScheduler::Job& job) {
    WavefrontRenderContext* ctx = task.ctx;
    consumePendingResetLocked(task.sessionId, ctx);
    const int startSample = ctx->nextSample();
    fprintf(stderr, "chunk %s gen%u: cam=(%.2f,%.2f,%.2f) dir=(%.3f,%.3f,%.3f) "
                    "%dx%d spp=%d neeK=%d exp=%.2f\n",
            task.renderId.c_str(), (unsigned)task.generation,
            task.camera.position.x, task.camera.position.y, task.camera.position.z,
            task.camera.direction.x, task.camera.direction.y, task.camera.direction.z,
            task.width, task.height, task.spp, task.neeK, task.config.exposure);
    try {
        if (startSample >= task.spp && !task.reencode) {
            // No encode follows this path — mark the record landed so REST
            // doesn't gate on the encode that never comes (A4).
            {
                std::lock_guard<std::mutex> lock(mutex_);
                auto rit = renders_.find(task.renderId);
                if (rit != renders_.end()) rit->second.encoded = true;
            }
            publishEvent(task.sessionId,
                         eventJson("render-completed", task.renderId, task.sessionId,
                                   task.generation, startSample, task.spp,
                                   double(startSample), 0, task.width, task.height));
            return;
        }
        // task.reencode && startSample >= spp falls through as a 0-sample
        // chunk (A2): the sample loop no-ops, finalize + readback + encode
        // re-publish the existing accumulation with new display params.
        if (startSample == 0)
            publishEvent(task.sessionId,
                         eventJson("render-started", task.renderId, task.sessionId,
                                   task.generation, 0, task.spp, 0.0, 0, task.width, task.height));

        GPURenderArgs args = makeRenderArgs(task, startSample);
        std::vector<PerPixelData> output(size_t(task.width) * task.height);
        const bool perfLog = [] {
            const char* pe = std::getenv("RENDER_PERF");
            return !pe || pe[0] != '0';
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
        if (chunkTimedOutLocked(task.renderId)) {
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

        // 阶段三 export point (A4 pipelined): hand the chunk output to the
        // encode thread and move on — the GPU worker starts the next chunk
        // while postProcess/JPEG run on the CPU. render-completed for the
        // final chunk is published by the encode path AFTER finalBytes are
        // stored (emitCompleted), so GET /image never races the event.
        // task.reencode (A2) enqueues with accumulated == startSample —
        // that 0-sample chunk is exactly one encode of the old accumulation.
        const bool productive = accumulated > startSample || task.reencode;
        if (productive)
            enqueueEncode(makeEncodeJob(task, accumulated, std::move(output)));

        advanceChunkChain(task, job, accumulated, startSample);
    } catch (...) {
        publishEvent(task.sessionId,
                     eventJson("render-failed", task.renderId, task.sessionId,
                               task.generation, task.ctx->nextSample(), task.spp,
                               double(task.ctx->nextSample()), 0, task.width, task.height));
        throw;
    }
}

// Consume a pending accumulation reset HERE, on the GPU worker: the
// reset's cudaMemset must not run on an HTTP thread (submitRender only
// arms the flag), and doing it before reading nextSample guarantees the
// new chain starts at sample 0 even when it cancelled an in-flight
// chunk whose final store would otherwise overwrite the counter
// (measured: a mid-loop cancel left nextSample=6, and the next chain
// resumed at start=6 with zeroed accumulators -> wrong radiance).
void RenderServer::consumePendingResetLocked(const std::string& sessionId,
                                            WavefrontRenderContext* ctx) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto sit = sessions_.find(sessionId);
    if (sit != sessions_.end() && sit->second.pendingReset) {
        sit->second.pendingReset = false;
        ctx->resetAccumulation();
    }
}

GPURenderArgs RenderServer::makeRenderArgs(const ChunkTask& task, int startSample) {
    GPURenderArgs args{};
    args.camera = task.camera;
    args.width = task.width;
    args.height = task.height;
    args.spp = std::max(0, std::min(task.chunkSpp, task.spp - startSample));
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
    // 方向太阳与 CLI 同源：session override（/render 的 sunAz/El/Int，
    // A3）优先，否则 RENDER_SUN_* env rig——Bistro 内景的主直接光，
    // 缺了它内景整体死黑（Dd 假暗 ~20×）。Physical knobs (emission/
    // indirect/mip) come from the profile when one drove this render;
    // startup-env fallback otherwise (CLI parity).
    applyRenderConfig(args, task.config,
                      /*sunEnvFallback=*/!task.config.sunHas,
                      /*physicalEnvFallback=*/!task.hasProfile);
    return args;
}

// 任务超时（设计 12/13）：链超过上限后不再续提交，状态记为 failed。
bool RenderServer::chunkTimedOutLocked(const std::string& renderId) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto rit = renders_.find(renderId);
    if (rit == renders_.end()) return false;
    const double elapsed = std::chrono::duration<double>(
        std::chrono::steady_clock::now() - rit->second.startTime).count();
    if (elapsed <= renderTimeoutSeconds()) return false;
    rit->second.timedOut = true;
    return true;
}

RenderServer::EncodeJob RenderServer::makeEncodeJob(const ChunkTask& task, int accumulated,
                                      std::vector<PerPixelData>&& output) {
    EncodeJob job;
    job.sessionId = task.sessionId;
    job.renderId = task.renderId;
    job.generation = task.generation;
    job.width = task.width;
    job.height = task.height;
    job.spp = task.spp;
    job.accumulatedSpp = accumulated;
    job.denoise = task.denoise;
    job.progressive = task.progressive;
    job.emitCompleted = accumulated >= task.spp || task.reencode;
    job.config = task.config;
    job.hasProfile = task.hasProfile;
    job.output = std::move(output);
    return job;
}

// Terminal states (design 4.3): budget met -> completed (the event
// comes from the encode path), cancel request -> canceled. Continue
// the chain otherwise; FIFO queue + one chunk per job gives natural
// round-robin between sessions. (finalBytes was stored by the
// encode path's final-chunk branch — the lossless PNG must not be
// overwritten by the JPEG preview.)
void RenderServer::advanceChunkChain(const ChunkTask& task, const RenderScheduler::Job& job,
                                     int accumulated, int startSample) {
    if (accumulated >= task.spp || task.reencode)
        return;
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
}

void RenderServer::encodePreview(const EncodeJob& job) {
    const int width = job.width, height = job.height;
    // Progressive denoise quality (design 9/批1): intermediate chunks of a
    // progressive chain encode quick (firefly clamp + ONE NL-means pass —
    // the frames are transient and the 7-pass ladder is the single largest
    // CPU item); the final chunk encodes OIDN-only quality 3 (RENDER_OIDN=1;
    // without OIDN linked it degrades to full NL-means). Non-progressive
    // renders keep quality 2 — the calibrated path.
    const bool finalChunk = job.accumulatedSpp >= job.spp;
    const int denoiseQuality = !job.denoise ? 0
                             : job.progressive ? (finalChunk ? 3 : 1)
                                                : 2;
    Photo photo;
    photo.allocate(width, height);
    photo.exposure = job.config.exposure;
    photo.warmTint = job.config.warmTint;
    photo.saturationBoost = job.config.saturationBoost;
    // Display grade: position-aware profiles deliver WB / split-tone /
    // grade bands / saturation as task data (parsed at session creation —
    // no setenv on this thread either); sessions without a profile keep
    // the shared env path (CLI parity).
    if (job.hasProfile) {
        photo.wbStrength = job.config.wbStrength;
        photo.splitKeySun = job.config.splitKeySun;
        photo.splitWarm = job.config.splitWarm;
        photo.splitCool = job.config.splitCool;
        photo.gradeBands.clear();
        {
            std::istringstream gbs(job.config.gradeBands);
            std::string tok;
            while (gbs >> tok) {
                float l, r, g, b;
                if (std::sscanf(tok.c_str(), "%f,%f,%f,%f", &l, &r, &g, &b) == 4) {
                    photo.gradeBands.push_back(l);
                    photo.gradeBands.push_back(r);
                    photo.gradeBands.push_back(g);
                    photo.gradeBands.push_back(b);
                }
            }
        }
        // photo.saturationBoost was set from job.config above —
        // the profile/client precedence was already resolved at submit.
    } else {
        // Shared env grade assembly (WB / split-tone / grade bands /
        // RENDER_SATURATION) — identical to the console path. The
        // saturation env only wins when the session still carries the
        // neutral 1.0 (no client slider pinned it).
        applyRenderGradeEnv(photo, job.config.saturationBoost == 1.0f);
    }

    // 精修链降噪（设计 9）：与 console 完全同源的 radiance/G-buffer 填充，
    // 让预览走同一条 NL-means 降噪路径——否则低 spp 的无降噪预览只剩
    // 高光轮廓（实测反馈）。
    std::vector<RadianceData> radD, radS, radI, radIs;
    std::vector<HitInfo> gbuffer;
    std::vector<float> sunMask(job.output.size());  // P1 grade matte (split-tone key)
    if (job.denoise) {
        const size_t n = job.output.size();
        radD.resize(n); radS.resize(n); radI.resize(n); radIs.resize(n);
        gbuffer.resize(n);
    }
    // Same composite order as console's Photo fill loop.
    for (size_t i = 0; i < job.output.size(); ++i) {
        const PerPixelData& ppd = job.output[i];
        const float3 finalColor = ppd.radiance_Dd + ppd.radiance_Ds
                                + ppd.radiance_Id + ppd.radiance_Is
                                + ppd.emission;
        photo.channels.finalColor[i] = float4(finalColor, 1.0f);
        sunMask[i] = ppd.sunMask;
        if (job.denoise) {
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
    if (job.denoise) {
        photo.radiance_Dd = radD.data();
        photo.radiance_Ds = radS.data();
        photo.radiance_Id = radI.data();
        photo.radiance_Is = radIs.data();
        photo.Gbuffer     = gbuffer.data();
    }
    photo.sunMask = sunMask.data();
    // Bloom follows the console knob (RENDER_BLOOM=0 disables) unless the
    // request pinned it (display-only A1): bright daylight scenes want it
    // off — the Clum>=1 threshold fires on the sunlit street itself and the
    // blur haze washes the frame.
    bool doBloom = true;
    if (job.config.bloom >= 0) doBloom = job.config.bloom != 0;
    else if (const char* bl = std::getenv("RENDER_BLOOM")) doBloom = bl[0] != '0';
    photo.postProcess(Photo::Full |
                      (doBloom ? Photo::DoBloom : 0) |
                      (job.denoise ? Photo::DoDenoise : 0),
                      job.accumulatedSpp, denoiseQuality);

    const std::vector<uint8_t> rgb = photo.rgbBytes();
    std::vector<unsigned char> preview, finalBytes;
    auto writeBytes = [](void* context, void* data, int size) {
        auto* bytes = static_cast<std::vector<unsigned char>*>(context);
        const unsigned char* p = static_cast<const unsigned char*>(data);
        bytes->insert(bytes->end(), p, p + size);
    };
    // Progressive previews encode JPEG — transient intermediate frames at
    // quality 80 (smaller + faster to encode/download; the frames are
    // replaced within a second anyway), the chain-final preview at 90 (it
    // stays on screen). The FINAL frame additionally encodes a lossless PNG
    // for GET /api/renders/{id}/image (delivery stays PNG).
    const int jpegQ = finalChunk ? 90 : 80;
    stbi_write_jpg_to_func(writeBytes, &preview, width, height, 3,
                           rgb.data(), jpegQ);
    if (finalChunk)
        stbi_write_png_to_func(writeBytes, &finalBytes, width, height, 3,
                               rgb.data(), width * 3);
    const bool perfLog = [] {
        const char* p = std::getenv("RENDER_PERF");
        return !p || p[0] != '0';
    }();
    if (perfLog)
        fprintf(stderr, "PERF encode %s: denoiseQ=%d bytes=%zu final=%d\n",
                job.renderId.c_str(), denoiseQuality, preview.size(),
                (int)finalBytes.size());

    uint64_t sequence = 0;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = sessions_.find(job.sessionId);
        if (it == sessions_.end()) return;
        SessionInfo& session = it->second;
        session.previewBytes = std::move(preview);
        session.previewWidth = width;
        session.previewHeight = height;
        session.previewSpp = job.accumulatedSpp;
        session.previewGeneration = job.generation;
        ++session.previewSequence;
        sequence = session.previewSequence;
        if (session.activeJob)
            session.activeJob->progress().previewSequence = sequence;
        auto rit = renders_.find(job.renderId);
        if (rit != renders_.end()) {
            rit->second.previewBytes = session.previewBytes;
            rit->second.previewSpp = session.previewSpp;
            rit->second.previewSequence = sequence;
            if (finalChunk) rit->second.finalBytes = std::move(finalBytes);
            rit->second.encoded = true;
        }
    }
    const int accumulated = job.accumulatedSpp;
    publishEvent(job.sessionId,
                 eventJson("progress", job.renderId, job.sessionId,
                           job.generation, accumulated, job.spp,
                           double(accumulated), sequence, width, height));
    publishEvent(job.sessionId,
                 eventJson("preview-ready", job.renderId, job.sessionId,
                           job.generation, accumulated, job.spp,
                           double(accumulated), sequence, width, height));
    // Final chunk (A4): completion is announced here — after finalBytes are
    // stored — instead of from the GPU worker, so a browser reacting to
    // render-completed can immediately GET /image.
    if (job.emitCompleted)
        publishEvent(job.sessionId,
                     eventJson("render-completed", job.renderId, job.sessionId,
                               job.generation, accumulated, job.spp,
                               double(accumulated), 0, width, height));
}

void RenderServer::enqueueEncode(EncodeJob&& job) {
    bool queued = false;
    {
        std::lock_guard<std::mutex> lock(encodeMutex_);
        if (!encodeStopping_ && encodeQueue_.size() < kMaxEncodeQueue) {
            encodeQueue_.push_back(std::move(job));
            queued = true;
        }
    }
    if (queued) {
        encodeCv_.notify_one();
        return;
    }
    // Queue full or shutting down: backpressure — encode on the caller
    // (GPU worker) thread, i.e. the pre-pipeline serialized behavior.
    encodePreview(job);
}

void RenderServer::encodeLoop() {
    for (;;) {
        EncodeJob job;
        {
            std::unique_lock<std::mutex> lock(encodeMutex_);
            encodeCv_.wait(lock, [this] { return encodeStopping_ || !encodeQueue_.empty(); });
            if (encodeQueue_.empty()) {
                if (encodeStopping_) return;
                continue;
            }
            job = std::move(encodeQueue_.front());
            encodeQueue_.pop_front();
        }
        try {
            encodePreview(job);
        } catch (...) {
            // The worker's own chunk try/catch published render-failed for
            // render exceptions; an encode-side failure must not kill the
            // encode thread, so report and keep draining.
            publishEvent(job.sessionId,
                         eventJson("render-failed", job.renderId, job.sessionId,
                                   job.generation, job.accumulatedSpp, job.spp,
                                   double(job.accumulatedSpp), 0, job.width,
                                   job.height));
        }
    }
}

std::string RenderServer::previewData(const std::string& id, int& status,
                                      std::string& contentType) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = sessions_.find(id);
    if (it == sessions_.end()) {
        status = 404;
        return R"({"error":"session_not_found"})";
    }
    if (it->second.previewBytes.empty()) {
        status = 404;
        return R"({"error":"no_preview"})";
    }
    status = 200;
    // Progressive previews encode JPEG (quality 90); the final /image stays
    // lossless PNG — see encodePreview.
    contentType = "image/jpeg";
    return std::string(it->second.previewBytes.begin(), it->second.previewBytes.end());
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
        return R"({"ok":true,"service":"Raypr3sso","gpuWorker":true})";
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
    std::string request, errorResponse;
    if (!http::readHttpRequest(clientSocket, request, errorResponse)) {
        if (!errorResponse.empty()) sendAll(clientSocket, errorResponse);
        close(clientSocket);
        return;
    }
    if (request.find("Upgrade: websocket") != std::string::npos) {
        // handleWebSocket owns the socket from here (closes it on every path).
        handleWebSocket(clientSocket, request);
        return;
    }
    // 拆出 query：此前路由不处理 "?..."，事件里的 previewUrl（带 ?seq=）
    // 一直 404 —— 浏览器 overlay 拉到的全是错误响应。
    const http::RequestHead head = http::parseRequestHead(request);
    if (!head.valid) {
        sendAll(clientSocket, httpResponse(400, "application/json", R"({"error":"bad_request"})"));
        close(clientSocket);
        return;
    }
    const std::string body = request.substr(request.find("\r\n\r\n") + 4);
    // 访问 token（设计 13）：Authorization: Bearer 或 ?token=（WS/<img> 走 query）。
    // /api/health 保持开放供存活探测。
    if (head.path != "/api/health") {
        std::string given = headerValue(request, "Authorization");
        if (given.rfind("Bearer ", 0) == 0) given = given.substr(7);
        if (given.empty()) given = queryParam(head.query, "token");
        if (given != token_) {
            sendAll(clientSocket, httpResponse(401, "application/json",
                                               R"({"error":"unauthorized"})"));
            close(clientSocket);
            return;
        }
    }
    int status = 200;
    std::string type;
    sendAll(clientSocket,
            httpResponse(status, type, route(head.method, head.path, body, status, type)));
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
    // A4 encode pipeline: the scheduler job finishes before the encode
    // thread lands the bytes — hold "completed" back until encoded so REST
    // pollers never race GET /image (WS render-completed already waits).
    std::string state = record.timedOut ? "failed"
                                      : stateName(record.job->state());
    if (state == "completed" && !record.encoded) state = "running";
    std::ostringstream body;
    body << "{\"renderId\":\"" << jsonEscape(renderId)
         << "\",\"sessionId\":\"" << jsonEscape(record.sessionId)
         << "\",\"generation\":" << record.generation
         << ",\"state\":\"" << state << "\""
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
        finalImage ? it->second.finalBytes : it->second.previewBytes;
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
    const http::RequestHead head = http::parseRequestHead(request);
    const std::string& path = head.path;
    const std::string& query = head.query;
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
    const std::string accept = http::webSocketAccept(key);
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
        http::WebSocketFrame frame;
        const http::FrameRead rc =
            http::readWebSocketFrame(clientSocket, frame, kMaxClientPayload);
        if (rc == http::FrameRead::idle) continue;
        if (rc == http::FrameRead::error) break;

        if (frame.opcode == 0x8) break;   // Close
        if (frame.opcode == 0x9) {        // Ping -> Pong (echo payload)
            if (!http::sendWebSocketControl(clientSocket, 0x0A, frame.payload))
                alive = false;
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