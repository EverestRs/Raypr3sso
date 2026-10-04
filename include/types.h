#pragma once

#include <cstdint>
#include <cmath>

// Define CUDA host/device macros as no-ops for non-CUDA compilers
#ifndef __CUDACC__
#define __host__
#define __device__
#endif

// ---- Vector Types ----
// CPU side uses glm
#ifdef __CUDACC__
// GPU: use CUDA built-in vector types (no glm in device code)
#include <vector_functions.h>
#include <vector_types.h>

// Arithmetic operators for CUDA float3/float2 (not defined by default)
__host__ __device__ inline float3 operator+(const float3& a, const float3& b) { return make_float3(a.x+b.x, a.y+b.y, a.z+b.z); }
__host__ __device__ inline float3 operator-(const float3& a, const float3& b) { return make_float3(a.x-b.x, a.y-b.y, a.z-b.z); }
__host__ __device__ inline float3 operator*(const float3& a, float s) { return make_float3(a.x*s, a.y*s, a.z*s); }
__host__ __device__ inline float3 operator*(float s, const float3& a) { return make_float3(a.x*s, a.y*s, a.z*s); }
__host__ __device__ inline float3 operator/(const float3& a, float s) { return make_float3(a.x/s, a.y/s, a.z/s); }
__host__ __device__ inline float3 operator*(const float3& a, const float3& b) { return make_float3(a.x*b.x, a.y*b.y, a.z*b.z); }
__host__ __device__ inline float3 operator-(const float3& a) { return make_float3(-a.x, -a.y, -a.z); }
__host__ __device__ inline float3& operator+=(float3& a, const float3& b) { a.x += b.x; a.y += b.y; a.z += b.z; return a; }
__host__ __device__ inline float3& operator-=(float3& a, const float3& b) { a.x -= b.x; a.y -= b.y; a.z -= b.z; return a; }
__host__ __device__ inline float3& operator*=(float3& a, float s) { a.x *= s; a.y *= s; a.z *= s; return a; }
__host__ __device__ inline float3& operator/=(float3& a, float s) { a.x /= s; a.y /= s; a.z /= s; return a; }
__host__ __device__ inline float2 operator+(const float2& a, const float2& b) { return make_float2(a.x+b.x, a.y+b.y); }
__host__ __device__ inline float2 operator-(const float2& a, const float2& b) { return make_float2(a.x-b.x, a.y-b.y); }
#else
// CPU: use glm
#include <glm/glm.hpp>
#include <glm/vec2.hpp>
#include <glm/vec3.hpp>
#include <glm/vec4.hpp>
using float2 = glm::vec2;
using float3 = glm::vec3;
using float4 = glm::vec4;
using uint2  = glm::uvec2;
using uint3  = glm::uvec3;
#endif

// ---- Constants ----
constexpr float EPS_ZERO  = 1e-4f;
constexpr float PI        = 3.14159265358979323846f;
constexpr float INV_PI    = 0.31830988618379067154f;
constexpr float INV_TWO_PI = 0.15915494309189533577f;

constexpr float RGB_WEIGHT[3] = {0.299f, 0.587f, 0.114f};  // luminance weights

// ---- Ray ----
struct Ray {
    float3 origin;
    float3 direction;

    __host__ __device__
    Ray() : origin(ZERO()), direction(ZERO()) {}

    __host__ __device__
    Ray(const float3& o, const float3& d) : origin(o), direction(d) {}

private:
    // Returns a zero float3, works in both CUDA and CPU
    __host__ __device__ static float3 ZERO() {
#ifdef __CUDACC__
        return make_float3(0.0f, 0.0f, 0.0f);
#else
        return float3(0.0f);
#endif
    }
};

// Helper functions for const float3 construction
__host__ __device__ inline float3 INF3() {
#ifdef __CUDACC__
    return make_float3(INFINITY, INFINITY, INFINITY);
#else
    return float3(INFINITY);
#endif
}
__host__ __device__ inline float3 NEG_INF3() {
#ifdef __CUDACC__
    return make_float3(-INFINITY, -INFINITY, -INFINITY);
#else
    return float3(-INFINITY);
#endif
}

// ---- Ray Differential ----
struct RayDifferential {
    float3 dPdx{0.0f}, dPdy{0.0f};
    float3 dDdx{0.0f}, dDdy{0.0f};
};

// ---- Bounding Box ----
struct Box {
    float3 v0;  // min
    float3 v1;  // max

    __host__ __device__
    Box() : v0(INF3()), v1(NEG_INF3()) {}

    __host__ __device__
    Box(const float3& min, const float3& max) : v0(min), v1(max) {}

    __host__ __device__
    Box operator+(const Box& other) const {
        Box b;
        b.v0.x = fminf(v0.x, other.v0.x);
        b.v0.y = fminf(v0.y, other.v0.y);
        b.v0.z = fminf(v0.z, other.v0.z);
        b.v1.x = fmaxf(v1.x, other.v1.x);
        b.v1.y = fmaxf(v1.y, other.v1.y);
        b.v1.z = fmaxf(v1.z, other.v1.z);
        return b;
    }

    __host__ __device__
    float3 centroid() const {
        return (v0 + v1) * 0.5f;
    }

    __host__ __device__
    float surfaceArea() const {
        float3 d = v1 - v0;
        return 2.0f * (d.x * d.y + d.x * d.z + d.y * d.z);
    }
};
