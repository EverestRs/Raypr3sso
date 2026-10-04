#pragma once

#include "../include/types.h"
#include <cstdint>
#ifdef __CUDACC__
#include <cuda_runtime.h>
#endif
#ifndef __CUDACC__
#include <vector>
#include <random>
#include <string>
#endif

// ---- Material forward declaration ----
class Material;

#ifndef __CUDACC__
// ---- Vertex Data (CPU only) ----
struct VertexData {
    float2 uv;
    float3 normal;
};

// ---- Triangle Face (CPU only) ----
struct alignas(32) Face {
    float3 v[3];                // World-space vertex positions
    const VertexData* data[3];  // Vertex attribute pointers
    const Material* material;   // Material pointer
};
#endif

// ---- Hit Record ----
struct HitRecord {
    float t_min;        // Near bound (for BVH traversal)
    float t_max;        // Far bound / closest hit distance
#ifndef __CUDACC__
    const Face* face = nullptr;  // CPU hit face
#endif
    int   faceIdx = -1;          // GPU face index (-1 on CPU)
    float t;            // Hit distance along ray
    float u, v;         // Barycentric coordinates at hit

    __host__ __device__
    HitRecord()
        : t_min(EPS_ZERO), t_max(INFINITY)
#ifndef __CUDACC__
          , face(nullptr)
#endif
          , faceIdx(-1), t(INFINITY), u(0.0f), v(0.0f) {}

    __host__ __device__
    explicit HitRecord(float tmin, float tmax)
        : t_min(tmin), t_max(tmax)
#ifndef __CUDACC__
          , face(nullptr)
#endif
          , faceIdx(-1), t(INFINITY), u(0.0f), v(0.0f) {}

    __host__ __device__
    bool hasHit() const {
#ifndef __CUDACC__
        return face != nullptr || faceIdx >= 0;
#else
        return faceIdx >= 0;
#endif
    }
};

// ---- Light Sample (for NEE, visible on both CPU and GPU) ----
struct LightSample {
    float3 position;
    float3 wi;
    float  pdf;
    float3 emission;
};

#ifndef __CUDACC__
// ---- Random Number Generator (CPU) ----
class Generator {
    std::mt19937 mt;
    std::uniform_real_distribution<float> dist;
public:
    explicit Generator(uint32_t seed);
    float operator()();  // [0, 1)
};

// ---- 1D Distribution Sampling ----
class Distribution1D {
    std::vector<float> cdf;
    float sum;
public:
    void build(const std::vector<float>& f);
    float sample(float u, float& pdf) const;
    float pdfAt(float x) const;
    float getSum() const { return sum; }
    const std::vector<float>& getCDF() const { return cdf; }
};

// ---- Light Object with per-face importance distribution ----
class LightObject {
public:
    float3 center{0.0f};
    float3 color{0.0f};
    float  power = 0.0f;

    std::vector<const Face*> faces;
    Distribution1D faceDistrib;

    void addFace(const Face& face, const float3& emission);
    LightSample sample(Generator& gen, const float3& fromPoint) const;
    float pdf(const float3& fromPoint, const LightSample& sample) const;
};

// ---- SkyBox ----
class SkyBox {
    int width = 0, height = 0;
    std::vector<float3> data;
    Distribution1D distrib;

public:
    void load(const std::string& hdrFile);
    // Scale the sky radiance (art-direction knob, RENDER_SKY_SCALE). The
    // importance-sampling distribution is normalized, so probabilities are
    // unchanged and only the radiance magnitude scales.
    void scaleRadiance(float s) { for (auto& d : data) d *= s; }
    // Per-channel sky tint (art knob RENDER_SKY_TINT): the stock dusk HDRI is
    // warm-gray; a blue daytime tint gives vertical facades and shadows the
    // official daytime blue ambient (official shadow R/B ~0.76, green facade
    // reads green-dominant). Applied after scaleRadiance; the sampling CDF is
    // unaffected (it was normalized at load).
    void tintRadiance(const float3& t) { for (auto& d : data) d = d * t; }
    // Elevation-dependent falloff (street-canyon physics): the sky near the
    // horizon is dimmed to `horizonDim` while the zenith keeps full
    // radiance, blending with smoothstep over sin(el) in [0, transSin].
    // Vertical facades see horizon-weighted sky (they darken); horizontal
    // streets see zenith-weighted sky (they stay bright) — the official
    // exterior look. Applied to data after load; the importance CDF is
    // normalized so sampling stays valid (slightly less efficient).
    // Azimuthal darkening: dims the sky texels within halfWidth radians of
    // the direction at azimuth centerAzRad (e.g. the -x hemisphere behind the
    // camera). The facades' broad specular lobe reflects this region; the
    // official rig has a dark environment there.
    void applyAzimuthDarken(float centerAzRad, float halfWidthRad, float dim) {
        if (width <= 0 || height <= 0) return;
        for (int y = 0; y < height; ++y)
            for (int x = 0; x < width; ++x) {
                float u = (x + 0.5f) / width;
                // Azimuth convention shared by applySunConeFocus / sample /
                // the GPU kernel: phi = 2π(u-0.5), dir=(sinT cosφ, cosT,
                // sinT sinφ). The old 2πu mapped the darkened hemisphere
                // 180° away from the intent (in front of the camera).
                float phi = 2.0f * float(M_PI) * (u - 0.5f);
                float dphi = fabsf(atan2f(sinf(phi - centerAzRad),
                                          cosf(phi - centerAzRad)));
                float t = fmaxf(0.0f, fminf(1.0f, dphi / fmaxf(halfWidthRad, 1e-4f)));
                float s = t * t * (3.0f - 2.0f * t);
                float f = dim + (1.0f - dim) * s;
                data[size_t(y) * width + x] *= f;
            }
    }
    // Sun-cone focus (the official "dark environment + strong sun" rig):
    // the whole sky dome dims to `ambientDim` EXCEPT a smooth cone of
    // half-angle `coneHalfRad` around `sunDir`, which keeps full radiance.
    // Vertical facades see mostly the dimmed dome (their ambient fill drops);
    // the street pools stay sun-driven. Sampling CDF unaffected (unbiased).
    void applySunConeFocus(float3 sunDir, float coneHalfRad, float ambientDim) {
        if (width <= 0 || height <= 0) return;
        float L = fmaxf(sqrtf(dot(sunDir, sunDir)), 1e-6f);
        float3 sd = float3(sunDir.x / L, sunDir.y / L, sunDir.z / L);
        for (int y = 0; y < height; ++y) {
            float v = (y + 0.5f) / height;
            float theta = float(M_PI) * v;
            float sinT = sinf(theta), cosT = cosf(theta);
            for (int x = 0; x < width; ++x) {
                float u = (x + 0.5f) / width;
                float phi = 2.0f * float(M_PI) * (u - 0.5f);
                // Equirect convention of geometry.h sphereToDir / the render
                // kernel (atan2(-x,z)): d = (-sinT sinφ, cosT, sinT cosφ).
                // The old (cosφ, ·, sinφ) form placed the cone 90° away in
                // azimuth (found 2026-09-23 during the daylight-HDRI work).
                float3 d = float3(-sinT * sinf(phi), cosT, sinT * cosf(phi));
                float ang = acosf(fmaxf(-1.0f, fminf(1.0f, dot(d, sd))));
                float t = fmaxf(0.0f, fminf(1.0f, ang / fmaxf(coneHalfRad, 1e-4f)));
                float s = t * t * (3.0f - 2.0f * t);
                // 1 at the sun (cone interior keeps full radiance), ramping to
                // ambientDim across/outside the cone (the dark environment).
                float f = 1.0f - (1.0f - ambientDim) * s;
                data[size_t(y) * width + x] *= f;
            }
        }
    }
    void applyElevationFalloff(float horizonDim, float transSin) {
        if (width <= 0 || height <= 0) return;
        for (int y = 0; y < height; ++y) {
            float v = (y + 0.5f) / height;             // 0=zenith, 1=nadir
            float sinEl = fmaxf(-1.0f, fminf(1.0f, cosf(float(M_PI) * v)));
            float t = fmaxf(0.0f, fminf(1.0f, sinEl / fmaxf(transSin, 1e-4f)));
            float s = t * t * (3.0f - 2.0f * t);       // smoothstep
            float f = horizonDim + (1.0f - horizonDim) * s;
            for (int x = 0; x < width; ++x)
                data[size_t(y) * width + x] *= f;
        }
    }
    float3 sample(const float3& dir) const;
    float3 sampleImportance(float u1, float u2, float& pdf) const;
    bool empty() const { return width == 0; }
    int getWidth() const { return width; }
    int getHeight() const { return height; }
    const float3* getData() const { return data.empty() ? nullptr : data.data(); }
    // Importance sampling data access for GPU upload
    const std::vector<float>& getCDF() const { return distrib.getCDF(); }
    float getCDFSum() const { return distrib.getSum(); }
};
#endif
