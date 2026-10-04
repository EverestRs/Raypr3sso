#pragma once

#include <atomic>
#include <cstdint>
#include <condition_variable>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <thread>
#include <unordered_map>

enum class RenderJobState {
    queued,
    running,
    canceling,
    completed,
    canceled,
    failed
};

struct RenderJobProgress {
    int sample = 0;
    int sampleBudget = 0;
    double averageSpp = 0.0;
    uint64_t previewSequence = 0;
};

class RenderJob {
public:
    explicit RenderJob(uint64_t generation = 0) : generation_(generation) {}

    uint64_t generation() const { return generation_; }
    RenderJobState state() const { return state_.load(std::memory_order_acquire); }
    void start() { state_.store(RenderJobState::running, std::memory_order_release); }
    void requestCancel() {
        cancelRequested_.store(true, std::memory_order_release);
        state_.store(RenderJobState::canceling, std::memory_order_release);
    }
    bool cancelRequested() const {
        return cancelRequested_.load(std::memory_order_acquire);
    }
    const std::atomic<bool>& cancelFlag() const { return cancelRequested_; }
    void complete() {
        state_.store(cancelRequested() ? RenderJobState::canceled
                                       : RenderJobState::completed,
                     std::memory_order_release);
    }
    void fail() { state_.store(RenderJobState::failed, std::memory_order_release); }

    RenderJobProgress& progress() { return progress_; }
    const RenderJobProgress& progress() const { return progress_; }

private:
    uint64_t generation_ = 0;
    std::atomic<RenderJobState> state_{RenderJobState::queued};
    std::atomic<bool> cancelRequested_{false};
    RenderJobProgress progress_;
};

// Serializes all GPU work onto one worker thread. Network/request threads may
// submit or cancel jobs, but never touch CUDA resources directly.
class RenderScheduler {
public:
    using Job = std::shared_ptr<RenderJob>;
    using Work = std::function<void(const Job&)>;

    RenderScheduler();
    ~RenderScheduler();

    RenderScheduler(const RenderScheduler&) = delete;
    RenderScheduler& operator=(const RenderScheduler&) = delete;

    Job submit(uint64_t generation, Work work);
    bool cancel(const Job& job);
    void shutdown();

private:
    struct QueueItem {
        Job job;
        Work work;
    };

    void run();

    std::mutex mutex_;
    std::condition_variable condition_;
    std::deque<QueueItem> queue_;
    std::thread worker_;
    bool stopping_ = false;
};