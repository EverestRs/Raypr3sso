#pragma once

#include "../include/types.h"
#include "sampling.h"
#include <cstdint>
#include <string>
#include <vector>

// ---- Radiance Data with Variance Tracking ----
struct RadianceData {
    float3 radiance{0.0f};  // Accumulated radiance
    float  Var = 0.0f;       // Variance estimate

    __host__ __device__ RadianceData() = default;
};

// Accumulate a radiance sample into RadianceData (with variance tracking).
// CPU-only inline helper (glm::dot) — unreferenced by the current pipelines;
// hidden from CUDA TUs, where types.h maps float3 to the built-in vector and
// no glm namespace exists.
#ifndef __CUDACC__
inline void accumulateInwardRadiance(RadianceData& rd, float3 inradiance, float weight) {
    rd.radiance += inradiance * weight;
    rd.Var += glm::dot(inradiance, inradiance) * weight;
}
#endif

// ---- Spatial Denoising Functions ----
// Re-read the env knobs cached in image.cpp file-statics (tone map choice,
// contrast curve, denoise noise target). Scene presets install rig env at
// runtime (applyPresetRig), so caches built at program start go stale;
// postProcess calls this before any pixel work.
void refreshRenderEnv();

// Clip exceptionally bright pixels (firefly removal)
void spatialClamp(RadianceData* data, int width, int height, float maxLuma = 100.0f);

// Edge-aware weight for NL-Means filtering (reference-equivalent: normal power,
// position/plane rejection, material diff, variance-aware radiance diff +
// Tier 2 global MAD sigma_l floor + k<-7.5 cutoff + specular roughness gate)
float getDenoiseWeight(const HitInfo& Gp, const HitInfo& Gq,
                       const RadianceData& Lp, const RadianceData& Lq,
                       const float3& LmP, const float3& LmQ,
                       float lumLP, float lumLQ, float varMed,
                       bool specular);

// NL-Means joint bilateral filter on radiance data
// Uses G-buffer (normals + base colors) for edge-aware weighting
// mcVar: per-pixel TRUE MC sample variance (pre-filterVar snapshot) for the
// noise-guided blend — filterVar overwrites Var with SPATIAL variance, which
// on textured regions is high at any spp (texture is signal, not noise).
void filterRadiance(RadianceData* data, const HitInfo* Gbuffer,
                    int width, int height, bool specular, int pass, float varMed,
                    const float* mcVar = nullptr);

class Photo {
public:
    int width = 0, height = 0;
    float exposure = 1.0f;
    // Tonemap controls (neutral defaults; tune for specific scenes)
    float warmTint = 1.0f;           // 1.0 = no tint
    float saturationBoost = 1.0f;    // 1.0 = no boost
    float wbStrength = 0.0f;         // auto white balance strength (0=off, keeps tungsten atmosphere)

    // AOV channels
    struct Channels {
        float4* finalColor = nullptr;         // Final composited image
        float4* directDiffuse = nullptr;
        float4* directSpecular = nullptr;
        float4* indirectDiffuse = nullptr;
        float4* indirectSpecular = nullptr;
        float4* baseColor = nullptr;          // Debug
        float4* emission = nullptr;           // Debug
    } channels;

    // Radiance data arrays for denoising (external, not owned by Photo)
    RadianceData* radiance_Dd = nullptr;
    RadianceData* radiance_Ds = nullptr;
    RadianceData* radiance_Id = nullptr;
    RadianceData* radiance_Is = nullptr;
    HitInfo*      Gbuffer     = nullptr;  // G-buffer for edge-aware filtering
    // P1: first-hit direct-sun luminance per pixel (soft-shadow grade matte);
    // the sun-keyed split-tone reads it via RENDER_SPLIT_KEY=sun.
    const float*  sunMask     = nullptr;
    // P2: normalized sunliness matte computed in postProcess (0=shade..1=sun),
    // consumed by gammaCorrect's split-tone when splitKeySun.
    std::vector<float> sunliness;
    // Split-tone controls (render-time, set by console per render):
    // splitKeySun keys on the P1 sun matte instead of display luma.
    float splitWarm = 0.0f;   // highlight warmth  (>0)
    float splitCool = 0.0f;   // shadow coolness   (>0)
    bool  splitKeySun = false;
    // P4d luma-band grade (RENDER_GRADE_BANDS): groups of 4 floats
    // [luma, gainR, gainG, gainB] sorted by luma; piecewise-linear in luma.
    // The 3D-LUT-style finishing pass fitted by scripts/make_lut_grade.py.
    std::vector<float> gradeBands;
    // P4c: when OIDN ran, the composed denoised image lands here and the
    // final composite uses it instead of summing the (custom-denoised) AOVs.
    std::vector<float> oidnColor;

    enum ShadeOption {
        BaseColor        = 1 << 0,
        Emission         = 1 << 1,
        DirectLight      = 1 << 2,
        IndirectLight    = 1 << 3,
        Diffuse          = 1 << 4,
        Specular         = 1 << 5,
        Full             = DirectLight | IndirectLight | Diffuse | Specular | BaseColor | Emission,
        DoBloom          = 1 << 6,
        DoFXAA           = 1 << 7,
        DoDoF            = 1 << 8,
        DoDenoise        = 1 << 9,
    };

    Photo() = default;
    ~Photo();

    // Non-copyable
    Photo(const Photo&) = delete;
    Photo& operator=(const Photo&) = delete;

    void allocate(int w, int h);
    // spp: samples per pixel of the input frame — drives the SPP-adaptive
    // denoiser gates (firefly tolerance widens as spp drops).
    // denoiseQuality (with DoDenoise): 2 = full (calibrated FILTER_PASSES+2
    // NL-means ladder — byte-identical CLI delivery path), 3 = OIDN-only
    // (RENDER_OIDN=1; skips the NL-means chain, recomposite uses oidnColor),
    // 1 = quick (firefly clamp + ONE NL-means pass — transient progressive
    // preview chunks), 0 = statistics only.
    void postProcess(int shadeOptions, int spp = 320, int denoiseQuality = 2);
    void save(const std::string& filename);
    // Post-gamma display bytes (RGB8, same clamp as save) — lets the render
    // server encode previews with the exact pipeline save() uses.
    std::vector<uint8_t> rgbBytes() const;

private:
    // postProcess stages. Pure extraction: same order, same numerics.
    bool denoiseRadiance(int sppEff, int denoiseQuality);  // true when filtered
    void denoiseEmission();
    void recompositeFinal();
    void finishDisplay(int shadeOptions);
    // denoiseRadiance stages. Same extraction rule: n / aovs rebuilt per
    // stage, conditions and numerics moved verbatim.
    void denoiseStatistics(double& rawLumSum, float& lumScale);
    void buildSunliness();                      // P1/P2 sun-key matte
#ifdef RENDERER_HAS_OIDN
    void runOidnPass(int denoiseQuality);       // P4c OIDN alternative
#endif
    void nlMeansFilter(size_t n, float lumScale, float ffGate, bool nlMeans,
                       int nlPasses, int denoiseQuality);
    void applyPositionAO(size_t n);             // P4a contact shadows
    void albedoDetailTransfer(size_t n);        // multi-scale detail pyramid
    void applyEnergyGain(size_t n, double rawLumSum, float lumScale,
                         float kSpp, bool nlMeans, int sppEff);
    void tonemap();        // Reinhard / ACES
    void gammaCorrect();   // sRGB
    void bloom();
    void fxaa();
    void depthOfField(const float* depthBuffer, float focus, float coc);
};

// Assemble the env-driven display grade (RENDER_WB_STRENGTH, RENDER_SPLIT_*,
// RENDER_GRADE_BANDS, RENDER_SATURATION) onto a Photo. Shared by the console
// and the render server so a calibrated rig cannot drift between the CLI and
// the deployed viewer (the server silently dropped WB/split/grade/saturation
// before this existed). saturationEnvWins: console always honors the env; the
// server only when no client value was pinned (session == neutral 1.0).
void applyRenderGradeEnv(Photo& photo, bool saturationEnvWins);
