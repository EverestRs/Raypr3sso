#pragma once

#include "../include/types.h"
#include "camera.h"
#ifndef __CUDACC__
#include <string>
#include <vector>
#include <map>
#endif

#ifndef __CUDACC__
class Model;

// ---- Render Parameters ----
struct RenderArgs {
    Camera  camera;
    int     width = 1920, height = 1080;
    int     spp = 1024;
    float   pDirect = 0.5f;
    int     maxDepth = 16;
    int     numThreads = 0;
    float   exposure = 1.0f;          // Tonemap exposure
    float   warmTint = 1.0f;          // Warm tint (1.0 = none)
    float   saturationBoost = 1.0f;   // Saturation boost (1.0 = none)
    std::string outputPath = "output";
};

// ---- Medium Tracking (IOR stack + Beer's law absorption) ----
struct MediumData {
    float ior;
    float3 absorb;  // per-unit transmission color
    MediumData(float ior_, const float3& absorb_) : ior(ior_), absorb(absorb_) {}
};

class Medium {
private:
    std::multimap<int, MediumData> mediums;
public:
    void init();
    void insert(int id, float ior, const float3& absorb);
    void erase(int id);
    float3 absorb() const;
    float ior() const;
    int size() const;
};
#endif

// ---- Per-Pixel Data (AOV channels + variance) ----
struct PerPixelData {
    float3 radiance_Dd;  // Direct Diffuse
    float3 radiance_Ds;  // Direct Specular
    float3 radiance_Id;  // Indirect Diffuse
    float3 radiance_Is;  // Indirect Specular
    float  var_Dd = 0;   // Variance of direct diffuse
    float  var_Ds = 0;   // Variance of direct specular
    float  var_Id = 0;   // Variance of indirect diffuse
    float  var_Is = 0;   // Variance of indirect specular
    float3 baseColor;    // Hit point base color
    float3 normal;       // Hit point shading normal (perturbed)
    float3 emission;     // Emission / sky contribution
    // G-buffer extensions for denoiser (first-hit, non-perturbed)
    float3 position;      // Hit point world position
    float3 shapeNormal;   // Geometric (unperturbed) normal
    float  roughness = 0; // Material roughness
    float  metallic = 0;  // Material metallic
    float  specular = 0;  // Material specular (F0)
    float  opacity = 1;   // Material opacity
    // First-hit DIRECT-SUN luminance (grade matte, P1). Carries the soft
    // shadow profile (cone jitter averages over samples): 0 = shade, high =
    // sunlit. The split-tone keys warm/cool zones on this instead of luma.
    float  sunMask = 0;
};

// ---- Ray Differential Functions ----
// Compute hit point position differentials from ray differential and hit point
void calc_dPdxy(const Ray& ray, float hit_t, const float3& normal,
                const RayDifferential& base_diff,
                float3& hit_dPdx, float3& hit_dPdy);

// Compute ray direction differentials at hit point
void calc_dDdxy(const Ray& ray, const float3& normal,
                const RayDifferential& base_diff,
                float3& hit_dDdx, float3& hit_dDdy);

#ifndef __CUDACC__
// ---- CPU Multi-threaded Render Entry ----
void renderCPU(const Model& model, const RenderArgs& args,
               std::vector<PerPixelData>& output);

// ---- Single Pixel Render ----
void renderPixel(const Model& model, const RenderArgs& args,
                 int x, int y, uint32_t seed, PerPixelData& out);

// ---- Recursive Path Tracing ----
void samplePath(const Model& model, Ray ray,
                uint32_t& seed, int depth, bool excludeDirect,
                float3& radiance, float3& throughput,
                PerPixelData& split);
#endif
