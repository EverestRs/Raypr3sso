#pragma once
// Minimal terminal progress bar. Prints to stderr so it never collides with
// the interactive "> " prompt on stdoutaine.
#include <cstdio>
#include <chrono>
#include <algorithm>

class ProgressBar {
public:
    ProgressBar(long total, int width = 40)
        : total_(total > 0 ? total : 1), width_(width) {}

    // Report current completed count; throttles redraws to ~15 fps.
    void update(long done) {
        if (done >= total_) {
            draw(done);
            std::fputc('\n', stderr);
            std::fflush(stderr);
            last_ = {};
            return;
        }
        auto now = std::chrono::steady_clock::now();
        if (last_.time_since_epoch().count() != 0) {
            auto elapsed = std::chrono::duration_cast<std::chrono::microseconds>(now - last_).count();
            if (elapsed < 66000) return;  // ~66 ms redraw interval
        }
        last_ = now;
        draw(done);
    }

    long total() const { return total_; }

private:
    long total_;
    int width_;
    std::chrono::steady_clock::time_point last_{};

    void draw(long done) {
        double p = double(done) / double(total_);
        int filled = int(p * double(width_));
        std::fprintf(stderr, "\r[");
        for (int i = 0; i < width_; ++i)
            std::fputc(i < filled ? '#' : ' ', stderr);
        std::fprintf(stderr, "] %5.1f%% (%ld/%ld)", p * 100.0, done, total_);
        std::fflush(stderr);
    }
};