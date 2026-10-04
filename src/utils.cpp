#include "utils.h"
#include <chrono>
#include <fstream>
#include <algorithm>
#include <sstream>

namespace {

using Clock = std::chrono::high_resolution_clock;

inline Clock::time_point now() {
    return Clock::now();
}

} // anonymous namespace

// ---- Timer ----
Timer::Timer() : m_start(now()) {}

void Timer::reset() { m_start = now(); }

double Timer::elapsed() const {
    auto end = now();
    return std::chrono::duration<double>(end - m_start).count();
}

// ---- File I/O ----
std::vector<uint8_t> readFileBinary(const std::string& path) {
    std::ifstream file(path, std::ios::binary | std::ios::ate);
    if (!file) return {};

    std::streamsize size = file.tellg();
    file.seekg(0, std::ios::beg);

    std::vector<uint8_t> buffer(static_cast<size_t>(size));
    file.read(reinterpret_cast<char*>(buffer.data()), size);
    return buffer;
}

bool writeFileBinary(const std::string& path, const void* data, size_t size) {
    std::ofstream file(path, std::ios::binary);
    if (!file) return false;
    file.write(static_cast<const char*>(data), size);
    return file.good();
}

// ---- String Utilities ----
std::string toLower(const std::string& str) {
    std::string result = str;
    std::transform(result.begin(), result.end(), result.begin(), ::tolower);
    return result;
}

std::string toUpper(const std::string& str) {
    std::string result = str;
    std::transform(result.begin(), result.end(), result.begin(), ::toupper);
    return result;
}

std::vector<std::string> split(const std::string& str, char delimiter) {
    std::vector<std::string> tokens;
    std::istringstream stream(str);
    std::string token;
    while (std::getline(stream, token, delimiter)) {
        if (!token.empty()) tokens.push_back(token);
    }
    return tokens;
}

// ---- Math ----
float luminance(const float3& color) {
    return color.x * RGB_WEIGHT[0] + color.y * RGB_WEIGHT[1] + color.z * RGB_WEIGHT[2];
}

float3 blend(const float3& a, const float3& b, float t) {
    return a + (b - a) * t;
}

float3 powColor(const float3& color, float exponent) {
    return float3(powf(color.x, exponent),
                  powf(color.y, exponent),
                  powf(color.z, exponent));
}
