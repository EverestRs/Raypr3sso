#pragma once

#include "../include/types.h"
#include <chrono>
#include <string>
#include <vector>

// ---- Timer ----
class Timer {
    using Clock = std::chrono::high_resolution_clock;
    Clock::time_point m_start;
public:
    Timer();
    void reset();
    double elapsed() const;  // seconds
};

// ---- File I/O ----
std::vector<uint8_t> readFileBinary(const std::string& path);
bool writeFileBinary(const std::string& path, const void* data, size_t size);

// ---- String Utilities ----
std::string toLower(const std::string& str);
std::string toUpper(const std::string& str);
std::vector<std::string> split(const std::string& str, char delimiter);

// ---- Math ----
float luminance(const float3& color);
float3 blend(const float3& a, const float3& b, float t);
float3 powColor(const float3& color, float exponent);
