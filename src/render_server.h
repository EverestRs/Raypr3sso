#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include "render_job.h"
#include "camera.h"
#include "model.h"
#include "gpu_scene.h"
#include "render_gpu.h"

class RenderServer {
public:
    RenderServer(std::string bindAddress, std::uint16_t port, std::string token);
    ~RenderServer();

    RenderServer(const RenderServer&) = delete;
    RenderServer& operator=(const RenderServer&) = delete;

    int run();
    void stop();

private:
    // Per-connection event queue (design 8): the GPU worker publishes
    // serialized JSON events; the WebSocket thread drains and sends them.
    // Bounded — a slow consumer drops the oldest events instead of ever
    // blocking the worker, and the producer never touches sockets.
    struct EventChannel {
        static constexpr size_t kMaxEvents = 64;
        std::mutex mutex;
        std::deque<std::string> queue;
        bool closed = false;

        void push(std::string event) {
            {
                std::lock_guard<std::mutex> lock(mutex);
                if (closed) return;
                if (queue.size() >= kMaxEvents) queue.pop_front();
                queue.push_back(std::move(event));
            }
        }
        bool tryPop(std::string& out) {
            std::lock_guard<std::mutex> lock(mutex);
            if (queue.empty()) return false;
            out = std::move(queue.front());
            queue.pop_front();
            return true;
        }
        void close() {
            std::lock_guard<std::mutex> lock(mutex);
            closed = true;
        }
    };

    // Resident scene registry (design 4.1): one Model+GPUScene per
    // whitelisted modelId, loaded ONCE by the GPU worker, shared read-only
    // by all sessions. Entries are never evicted in this version, so
    // sessions may hold shared_ptrs / raw pointers into them for the
    // lifetime of a render chain.
    struct SceneEntry {
        std::shared_ptr<Model> model;
        std::shared_ptr<GPUScene> scene;
        enum class State { loading, ready, failed } state = State::loading;
        std::string error;
    };

    // Fixed-window rate limiter (design 13): a counter per key that resets
    // every windowSec. Coarse by design — the single GPU worker is the real
    // bottleneck; this just sheds abusive traffic.
    class RateLimiter {
    public:
        bool allow(const std::string& key, int limit, int windowSec) {
            const auto now = std::chrono::steady_clock::now();
            auto& bucket = buckets_[key];
            const auto window = std::chrono::seconds(windowSec);
            if (now - bucket.second >= window) {
                bucket.first = 0;
                bucket.second = now;
            }
            return ++bucket.first <= limit;
        }

    private:
        std::unordered_map<std::string,
                           std::pair<int, std::chrono::steady_clock::time_point>>
            buckets_;
    };

    struct SessionInfo {
        std::string id;
        std::string modelId;
        uint64_t generation = 0;
        Camera camera;
        // Display parameters (design 10.4): applied only at preview encode
        // time, never part of the accumulated radiance — changing them must
        // not reset accumulation. Defaults come from the session preset.
        float exposure = 1.0f;
        float warmTint = 1.0f;
        float saturationBoost = 1.0f;
        // Indoor auto-exposure tier (from the preset, design 10.4): when the
        // camera is inside [indoorMin, indoorMax] the client blends the
        // per-render exposure up to indoorExposure (smoothstep over
        // indoorMargin metres). Parameters only — the CLIENT computes the
        // blend and sends `exposure`; indoorExposure 0 = tier disabled.
        float indoorExposure = 0.0f;
        float indoorMin[3] = {0.0f, 0.0f, 0.0f};
        float indoorMax[3] = {0.0f, 0.0f, 0.0f};
        float indoorMargin = 1.5f;
        // Wavefront pipeline state owned by the session (design 5.2):
        // buffers and accumulation persist across render chunks; reset
        // when the generation or size changes. Destroying it frees CUDA
        // resources and must eventually be marshaled onto the GPU worker
        // (sessions are not destroyed in the current skeleton).
        std::unique_ptr<WavefrontRenderContext> wavefront;
        uint64_t accumGeneration = 0;  // generation the accumulation belongs to
        int accumWidth = 0, accumHeight = 0;
        int accumNeeK = 0;             // NEE K the accumulation was built with (4.2)
        // Set by submitRender when the accumulation must restart; consumed
        // (and ONLY executed) by runRenderChunk on the GPU worker. The reset
        // itself calls cudaMemset on the context's buffers, so it must never
        // run on an HTTP thread — and running it there would also race the
        // in-flight chunk's final nextSample store (reset lost → the next
        // chain resumes at a stale sample index).
        bool pendingReset = false;
        // Latest progressive preview (design 5.2 staging buffer, 阶段三
        // export point): PNG bytes re-encoded by the GPU worker at every
        // chunk boundary; read by GET /api/sessions/{id}/preview. Guarded
        // by mutex_.
        std::vector<unsigned char> previewPng;
        int previewWidth = 0, previewHeight = 0;
        int previewSpp = 0;             // accumulated samples at encode time
        uint64_t previewGeneration = 0;
        uint64_t previewSequence = 0;   // bumps on every encoded preview
        // Session expiry (design 13): touched by every camera/render/WS
        // request; GC erases idle sessions on the GPU worker.
        std::chrono::steady_clock::time_point lastActivity =
            std::chrono::steady_clock::now();
        // Live event channels (design 8): weak refs so a disconnected
        // browser never keeps anything alive; pruned on every publish.
        std::vector<std::weak_ptr<EventChannel>> eventChannels;
        RenderScheduler::Job activeJob;
    };

    // One submitted render chain (design 7.4): the render-scoped view of a
    // session's progressive render. Terminal records are evicted above 64
    // (submitRender) and reaped with their session by the GC loop.
    struct RenderRecord {
        std::string sessionId;
        uint64_t generation = 0;
        int width = 0, height = 0;
        std::chrono::steady_clock::time_point startTime =
            std::chrono::steady_clock::now();
        bool timedOut = false;   // chain exceeded the render timeout (13)
        RenderScheduler::Job job;
        std::vector<unsigned char> previewPng;  // latest preview of this render
        std::vector<unsigned char> finalPng;    // preview captured at chain end
        int previewSpp = 0;
        uint64_t previewSequence = 0;
    };

    // One chunk of a progressive render chain (design 4.3). Points into
    // registry/session-owned objects; chain continuation re-validates the
    // session and its generation under the server mutex before
    // re-submitting, and registry entries are never evicted.
    struct ChunkTask {
        std::string sessionId;
        std::string renderId;
        uint64_t generation = 0;
        GPUScene* scene = nullptr;
        WavefrontRenderContext* ctx = nullptr;
        Camera camera;
        int width = 0, height = 0;
        int spp = 0, chunkSpp = 0, maxDepth = 0;
        // Progressive chain (批1): geometric chunk plan — each continuation
        // doubles its spp (2,4,8,16,...) so the first preview lands fast and
        // the encode count drops; the final chunk encodes OIDN-only + PNG.
        bool progressive = false;
        // Adaptive per-pixel sampling (批2): the wavefront path supports it;
        // sanitize requires adaptiveMin < the per-chunk spp, so with the
        // geometric plan only the large chunks actually enable it.
        int adaptiveMin = 0, adaptiveCheck = 8;
        float adaptiveThresh = 0.02f;
        uint32_t baseSeed = 0;
        // NEE samples per rough hit (设计 17); resolved (1..8) at submit.
        int neeK = 0;
        // 精修链的预览走 CLI 同源 NL-means 降噪器（拖动档保持原始快速帧）。
        bool denoise = false;
        // Display-only parameters for the preview encode (10.4).
        float exposure = 1.0f;
        float warmTint = 1.0f;
        float saturationBoost = 1.0f;
    };

    void handleClient(int clientSocket);
    std::string route(const std::string& method,
                      const std::string& path,
                      const std::string& body,
                      int& status,
                      std::string& contentType);
    std::string createSession(const std::string& body, int& status);
    std::string sessionStatus(const std::string& id, int& status);
    std::string updateCamera(const std::string& id, const std::string& body, int& status);
    std::string submitRender(const std::string& id, const std::string& body, int& status);
    std::string cancelSession(const std::string& id, int& status);
    std::string previewData(const std::string& id, int& status, std::string& contentType);
    std::string renderStatus(const std::string& renderId, int& status);
    std::string cancelRender(const std::string& renderId, int& status);
    std::string renderImage(const std::string& renderId, bool finalImage,
                            int& status, std::string& contentType);
    bool handleWebSocket(int clientSocket, const std::string& request);
    void runRenderChunk(const ChunkTask& task, const RenderScheduler::Job& job);
    // Fan an event JSON out to every live channel of one session (design 8:
    // the GPU worker is the only producer; HTTP threads only consume).
    void publishEvent(const std::string& sessionId, const std::string& event);
    // Fan an event out to every session bound to a model (session-ready).
    void publishEventToModel(const std::string& modelId, const std::string& event);
    // GPU worker task: builds the Model and uploads the GPUScene for a
    // whitelisted modelId (design 7.1: loading never happens on HTTP
    // threads). Marks the registry entry ready or failed when done.
    void loadSceneEntry(const std::string& modelId);
    // 阶段三 export point: tonemap + encode the current accumulation into
    // the session's preview slot (PNG). Runs on the GPU worker after every
    // productive chunk; reuses the CLI's exact postProcess pipeline (10.4).
    void encodePreview(const ChunkTask& task, const std::vector<PerPixelData>& output);
    // 设计 13 安全项：Origin 校验（回环/null/白名单）、token 比对辅助、
    // session 过期回收（GPU worker 上释放 CUDA 资源）、任务超时上限。
    bool originAllowed(const std::string& origin) const;
    static std::string queryParam(const std::string& query, const std::string& key);
    void gcLoop();
    double renderTimeoutSeconds() const;

    std::string bindAddress_;
    uint16_t port_;
    std::string token_;
    int listenSocket_ = -1;
    std::atomic<bool> stopping_{false};
    std::mutex mutex_;
    uint64_t nextSessionId_ = 1;
    uint64_t nextRenderId_ = 1;
    double sessionTtlSeconds_ = 900.0;
    RateLimiter rateLimiter_;
    std::unordered_map<std::string, SceneEntry> scenes_;
    std::unordered_map<std::string, SessionInfo> sessions_;
    std::unordered_map<std::string, RenderRecord> renders_;
    RenderScheduler scheduler_;
    std::thread gcThread_;
};