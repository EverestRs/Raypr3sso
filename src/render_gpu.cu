#include "render_gpu.h"
#include "sampling.h"
#include "bvh_gpu.h"
#include "progress.h"
// Include the implementation to avoid cross-file device function calls
// (we disable separable compilation so the device linker doesn't strip PTX)
#include "bvh_gpu.cu"
#include "../include/math_utils.h"
#include <cstdio>
#include <cuda_runtime.h>
#include <curand_kernel.h>
// isnan/isinf are available from CUDA math headers implicitly
#ifdef __CUDACC__
#include <vector>  // for readback buffers in renderGPU host function
#include <thread>
#include <chrono>
#endif

// =========================================================================
// GPU BSDF helpers (device-only, mirrors sampling.cpp for GPU)
// =========================================================================

__device__ float gpu_GTR1(float NdotH, float a) {
    if (a >= 1.0f) return INV_PI;
    const float a2 = a * a;
    const float denom = (NdotH * NdotH * (a2 - 1.0f) + 1.0f);
    return (a2 - 1.0f) / (PI * logf(a2) * denom);
}

__device__ float gpu_GTR2(float NdotH, float a) {
    const float a2 = a * a;
    const float denom = (NdotH * NdotH * (a2 - 1.0f) + 1.0f);
    return a2 / (PI * denom * denom);
}

__device__ float gpu_smithG(float NdotV, float alpha) {
    const float a = alpha * alpha;
    const float b = NdotV * NdotV;
    return 2.0f / (1.0f + sqrtf(1.0f + a * (1.0f - b) / (b + 1e-8f)));
}

__device__ float gpu_schlickFresnel(float cosTheta) {
    const float x = fmaxf(0.0f, 1.0f - cosTheta);
    return x * x * x * x * x;
}

// ---- Dielectric/glass helpers (GPU) ----
__device__ float gpu_fresnelDielectric(float cosTheta, float eta) {
    float sin2Theta = fmaxf(0.0f, 1.0f - cosTheta * cosTheta);
    float sin2ThetaT = eta * eta * sin2Theta;
    if (sin2ThetaT >= 1.0f) return 1.0f;  // TIR
    float cosThetaT = sqrtf(1.0f - sin2ThetaT);
    float rPara = (eta * cosTheta - cosThetaT) / (eta * cosTheta + cosThetaT);
    float rPerp = (cosTheta - eta * cosThetaT) / (cosTheta + eta * cosThetaT);
    return 0.5f * (rPara * rPara + rPerp * rPerp);
}

__device__ bool gpu_refract(const float3& inDir, const float3& normal,
                             float etaI, float etaT,
                             float3& outDir, float& fresnelR) {
    // Convention: inDir is the VIEW direction (= -ray.direction), pointing
    // OUT of the surface toward the source, same hemisphere as `normal`.
    // Therefore dot(inDir, normal) >= 0 and cosThetaI is positive.
    // The transmitted TRAVEL direction (outDir, used as the new ray direction)
    // is:  wt = -eta * wi + (eta * cosThetaI - cosThetaT) * n
    // (PBRT eq. 8.8), where wi = inDir, n = normal, eta = etaI/etaT.
    const float cosThetaI = dot(inDir, normal);
    const float eta = etaI / etaT;
    fresnelR = gpu_fresnelDielectric(fabsf(cosThetaI), eta);
    const float sin2ThetaI = fmaxf(0.0f, 1.0f - cosThetaI * cosThetaI);
    const float sin2ThetaT = eta * eta * sin2ThetaI;
    if (sin2ThetaT >= 1.0f) { fresnelR = 1.0f; return false; }
    const float cosThetaT = sqrtf(1.0f - sin2ThetaT);
    outDir = -eta * inDir + (eta * cosThetaI - cosThetaT) * normal;
    outDir = normalize(outDir);
    return true;
}

__device__ bool gpu_isDielectric(const HitInfo& surface) {
    if (surface.metallic >= 0.5f) return false;
    return surface.opacity < 0.99f && fabsf(surface.eta - 1.0f) > 0.01f;
}

// Transparent-aware shadow test: dielectric interfaces pass light with a
// Fresnel-based transmittance instead of blocking it entirely (lets wall
// lamps / sky illuminate wine and other liquids inside glassware).
// Returns accumulated tint; blocked=true when an opaque surface intervenes
// or the tint collapses below the visibility threshold.

// Linear interpolation helpers
__device__ inline float lerpf(float a, float b, float t) { return a + (b-a)*t; }
__device__ inline float3 lerp3(const float3& a, const float3& b, float t) {
    return make_float3(a.x+(b.x-a.x)*t, a.y+(b.y-a.y)*t, a.z+(b.z-a.z)*t);
}

// Split a radiance contribution into diffuse (along baseColor) vs specular
// (remainder) — mirrors CPU decomposeRadiance so AOVs separate Dd/Ds/Id/Is
// instead of piling every bounce into Dd.
__device__ void gpu_decomposeLobe(const float3& baseColor, const float3& contrib,
                                  float3& rd, float3& rs) {
    const float c2 = dot(contrib, contrib);
    if (c2 < 1e-8f) return;
    const float lum  = dot(contrib, make_float3(0.299f, 0.587f, 0.114f));
    const float bLum = dot(baseColor,  make_float3(0.299f, 0.587f, 0.114f));
    if (bLum < 1e-4f) { rs += contrib; return; }  // metal / dark tint -> specular
    const float3 diffPart = baseColor * (lum / bLum);
    float ratio = dot(contrib, diffPart) / c2;
    ratio = fminf(1.0f, fmaxf(0.0f, ratio));
    rd += contrib * ratio;
    rs += contrib * (1.0f - ratio);
}

// Per-material emission direction lobe: photons travelling along the axis get
// full radiance, off-axis falls as dot^k, and a zero axis means uniform
// emission. Ceiling-mounted fixtures use a downward axis so the ceiling
// directly above them is not blasted (the official Bistro look).
// NEE samples per rough-surface hit (quality/speed knob, RENDER_NEE_K).
// 4 = default quality; lower (1-2) speeds up previews at some noise cost.
// Host-side default only: per-render overrides go through GPURenderArgs::neeK
// (resolved at render entry), so the device never reads global state and a
// server can vary K per request/session safely.
static int g_defaultNeeK = 4;
void setNeeK(int k) { g_defaultNeeK = k > 0 ? k : 4; }
int defaultNeeK() { return g_defaultNeeK; }

// Directional sun from explicit parameters (render server /render path):
// direction from az/el, radiance = inten × RENDER_SUN_TINT, softness from
// RENDER_SUN_SOFT. Shares the env path's tint/soft knobs so a request-level
// override keeps the same look as the env-driven rig.
bool applySunParams(GPURenderArgs& args, float az, float el, float inten) {
    // AZ: degrees from +x toward +z; EL: degrees above horizon.
    const float azR = az * float(M_PI) / 180.0f, elR = el * float(M_PI) / 180.0f;
    const float3 dirRaw = make_float3(
        std::cos(elR) * std::cos(azR), std::sin(elR), std::cos(elR) * std::sin(azR));
    const float dirLen = std::sqrt(dirRaw.x * dirRaw.x + dirRaw.y * dirRaw.y +
                                   dirRaw.z * dirRaw.z);
    args.sunDir = make_float3(dirRaw.x / dirLen, dirRaw.y / dirLen, dirRaw.z / dirLen);
    // RENDER_SUN_TINT="r g b": official daylight sun is a warm gold.
    float3 tint = make_float3(1.0f, 1.0f, 1.0f);
    if (const char* tn = std::getenv("RENDER_SUN_TINT")) {
        float tr = 1.0f, tg = 1.0f, tb = 1.0f;
        if (std::sscanf(tn, "%f %f %f", &tr, &tg, &tb) == 3)
            tint = make_float3(tr, tg, tb);
    }
    args.sunRadiance = make_float3(inten * tint.x, inten * tint.y, inten * tint.z);
    args.hasSun = 1;
    if (const char* sf = std::getenv("RENDER_SUN_SOFT"))
        args.sunSoftDeg = fmaxf(0.0f, std::strtof(sf, nullptr));
    fprintf(stderr, "Directional sun: az=%g el=%g intensity=%g\n",
            az, el, inten);
    // De-duplicate the sun: the sky-cone focus keeps the HDRI sun region at
    // full radiance for the camera (the visible glow), but with a delta sun
    // present the cone region must not ALSO light the scene.
    if (const char* fd = std::getenv("RENDER_SKY_FOCUS_DIM")) {
        const float dim = std::strtof(fd, nullptr);
        if (dim >= 0.0f && dim < 1.0f) {
            args.sunConeKill = 1;
            if (const char* cc = std::getenv("RENDER_SKY_FOCUS_CONE"))
                args.sunConeHalfDeg = fmaxf(1.0f, std::strtof(cc, nullptr));
            fprintf(stderr, "  Sun-cone lighting kill: on (cone=%gdeg)\n",
                    args.sunConeHalfDeg);
        }
    }
    return true;
}

bool applySunFromEnv(GPURenderArgs& args) {
    // Directional sun (art-directed window light, RENDER_SUN_*).
    const char* se = std::getenv("RENDER_SUN_INT");
    if (!se || std::strtof(se, nullptr) <= 0.0f) return false;
    float az = 0.0f, el = 45.0f, inten = std::strtof(se, nullptr);
    if (const char* a = std::getenv("RENDER_SUN_AZ")) az = std::strtof(a, nullptr);
    if (const char* e = std::getenv("RENDER_SUN_EL")) el = std::strtof(e, nullptr);
    return applySunParams(args, az, el, inten);
}

// Per-render physical env knobs shared by both hosts (console + server):
// the server fills these from its parsed RigProfile instead (task channel),
// so env is only the CLI/startup-rig fallback path.
void applyPhysicalEnvFromEnv(GPURenderArgs& args) {
    if (const char* is = std::getenv("RENDER_INDIRECT_SCALE"))
        args.indirectScale = fmaxf(0.0f, std::strtof(is, nullptr));
    if (const char* es = std::getenv("RENDER_EMISSION_SCALE"))
        args.emissionScale = fmaxf(0.0f, std::strtof(es, nullptr));
}

// Foliage shadow transmission (RENDER_LEAF_SHADOW_TINT, default 1.0):
// alpha-cutout holes pass this fraction of the light. 1.0 = holes fully
// open (original behaviour). Real canopies leak less — sub-texel leaf
// coverage and scattering eat part of the transmitted light; the official
// Bistro terrace dapple sits at ~0.6 of the open-sun brightness.
__device__ float d_leafShadowTint = 1.0f;
void setLeafShadowTint(float t) { cudaMemcpyToSymbol(d_leafShadowTint, &t, sizeof(float)); }

// P4b: exact sRGB EOTF for baseColor textures (RENDER_SRGB_EXACT=1). The
// legacy powf(2.2) is an approximation; the official pipeline uses the true
// piecewise sRGB transfer. Default 0 = pow2.2 (calibrated/zero-regression).
__device__ int d_srgbExact = 0;
void setSrgbExact(int v) { cudaMemcpyToSymbol(d_srgbExact, &v, sizeof(int)); }

__device__ __forceinline__ float gpu_srgbToLinear1(float c) {
    return (c <= 0.04045f) ? c * (1.0f / 12.92f)
                           : powf((c + 0.055f) * (1.0f / 1.055f), 2.4f);
}
__device__ __forceinline__ float3 gpu_toLinear(float3 c) {
    return (d_srgbExact != 0)
        ? make_float3(gpu_srgbToLinear1(c.x), gpu_srgbToLinear1(c.y), gpu_srgbToLinear1(c.z))
        : make_float3(powf(c.x, 2.2f), powf(c.y, 2.2f), powf(c.z, 2.2f));
}

// Average samples per pixel actually traced by the last GPU render
// (megakernel or wavefront); equals spp when adaptive sampling is off.
static double g_lastAvgSpp = 0.0;
double getLastGpuAvgSpp() { return g_lastAvgSpp; }

__device__ float gpu_emissionLobe(const GPUMaterialBuffer& mb, int matId,
                                   float3 photonDir, float3 triNormal) {
    if (!mb.emissionAxis || !mb.emissionLobeK || !mb.emissionLobeU) return 1.0f;
    const float3 axis = mb.emissionAxis[matId];
    const float k = mb.emissionLobeK[matId];
    if (k <= 0.0f) return 1.0f;
    // Mixed model: a uniform floor U keeps the fixture's side glow (the
    // official lanterns glow sideways) while only a (1-U) fraction follows
    // the emission direction. A non-zero axis (ceiling fixtures: down) uses
    // the axis; a zero axis with k>0 (wall sconces) uses the emitter
    // triangle's own normal — sconces light outward, not up their wall.
    const float u = mb.emissionLobeU[matId];
    float lobe;
    if (dot(axis, axis) > 0.5f)
        lobe = powf(fmaxf(0.0f, dot(photonDir, axis)), k);
    else
        lobe = powf(fmaxf(0.0f, dot(photonDir, triNormal)), k);
    return u + (1.0f - u) * lobe;
}

// Fresnel-driven lobe weights shared by gpu_sampleBSDF and gpu_pdfBSDF so the
// two stay consistent (MIS). Specular bounce probability follows Schlick
// Fresnel at the view angle: dielectrics get ~4-10% (enough to catch glossy
// reflections), metals get lum(baseColor). The old `wSpecular = metallic`
// made every metallic=0 surface unable to sample its specular lobe at all,
// killing mirror reflections on polished stone/marble/glass.
__device__ inline void gpu_lobeWeights(const HitInfo& surface, float NdotV,
                                        float& wDiffuse, float& wSpecular,
                                        float& wClearcoat) {
    const float3 F0 = lerp3(make_float3(surface.specular, surface.specular,
                                         surface.specular),
                             surface.baseColor, surface.metallic);
    const float fres = gpu_schlickFresnel(fmaxf(NdotV, 0.0f));
    const float3 F = F0 + (make_float3(1.0f,1.0f,1.0f) - F0) * fres;
    float specProb = dot(F, make_float3(0.299f, 0.587f, 0.114f));
    specProb = fminf(fmaxf(specProb, 0.08f), 0.9f);
    wSpecular = specProb;
    wDiffuse = (1.0f - surface.metallic) * (1.0f - specProb);
    wClearcoat = fminf(surface.clearcoat, 1.0f) * 0.25f;
}

__device__ float3 gpu_evalBSDF(const float3& inDir, const float3& outDir,
                                const HitInfo& surface) {
    if (gpu_isDielectric(surface)) return make_float3(0.0f, 0.0f, 0.0f);

    const float3 N = surface.surfaceNormal;
    const float3 V = inDir;
    const float3 L = outDir;

    const float NdotV = fmaxf(1e-6f, dot(N, V));
    const float NdotL = fmaxf(1e-6f, dot(N, L));
    const float3 H = normalize(V + L);
    const float NdotH = fmaxf(1e-6f, dot(N, H));
    const float LdotH = fmaxf(1e-6f, dot(L, H));

    const float rough = fmaxf(0.001f, surface.roughness);
    const float alpha = rough * rough;
    float3 result = make_float3(0.0f, 0.0f, 0.0f);
    const float metallic = surface.metallic;
    const float3 base = surface.baseColor;

    // ---- Diffuse (Disney with subsurface) ----
    // MATCH CPU: Fd = mix(1, Fd90, FL) * mix(1, Fd90, FV)
    if (metallic < 1.0f) {
        float FL = gpu_schlickFresnel(NdotL);
        float FV = gpu_schlickFresnel(NdotV);
        float Fd90 = 0.5f + 2.0f * LdotH * LdotH * rough;
        float Fd = (1.0f + FL * (Fd90 - 1.0f)) * (1.0f + FV * (Fd90 - 1.0f));
        float3 Fd3 = base * INV_PI * Fd;
        float Fss90 = LdotH * LdotH * rough;
        float Fss = (1.0f + FL * (Fss90 - 1.0f)) * (1.0f + FV * (Fss90 - 1.0f));
        float ss = 1.25f * (Fss * (1.0f / (NdotL + NdotV + 1e-8f) - 0.5f) + 0.5f);
        float3 diff = (1.0f - metallic) * lerp3(Fd3, base * INV_PI * ss, fminf(surface.subsurface, 1.0f));
        result.x += diff.x; result.y += diff.y; result.z += diff.z;
    }

    // ---- Sheen (fabric reflection) ----
    if (metallic < 1.0f && surface.sheen > 0.0f) {
        float sheenW = fminf(surface.sheen, 1.0f);
        float3 sheenCol = lerp3(make_float3(1,1,1), base, fminf(surface.sheenTint, 1.0f));
        float sTerm = gpu_schlickFresnel(LdotH);
        float3 s = (1.0f - metallic) * sheenW * sheenCol * sTerm;
        result.x += s.x; result.y += s.y; result.z += s.z;
    }

    // ---- Specular (GTR2/GGX) ----
    {
        float3 F0 = lerp3(make_float3(surface.specular,surface.specular,surface.specular), base, metallic);
        float sf = gpu_schlickFresnel(LdotH);
        float3 F = F0 + (make_float3(1,1,1) - F0) * sf;
        float D = gpu_GTR2(NdotH, alpha);
        float G = gpu_smithG(NdotV, alpha) * gpu_smithG(NdotL, alpha);
        float3 spec = D * F * G / (4.0f * NdotV * NdotL + 1e-8f);
        result.x += spec.x; result.y += spec.y; result.z += spec.z;
    }

    // ---- Clearcoat (GTR1, IOR=1.5) ----
    if (surface.clearcoat > 0.0f) {
        float ccGloss = fminf(surface.clearcoatGloss, 1.0f);
        float alphaCC = lerpf(0.1f, 0.001f, ccGloss);
        float Dr = gpu_GTR1(NdotH, alphaCC);
        float Fr = lerpf(0.04f, 1.0f, gpu_schlickFresnel(LdotH));
        float Gr = gpu_smithG(NdotV, 0.25f) * gpu_smithG(NdotL, 0.25f);
        float ccW = fminf(surface.clearcoat, 1.0f);
        float ccv = Dr * Fr * Gr / 4.0f * ccW;
        float3 cc = make_float3(ccv, ccv, ccv);
        result.x += cc.x; result.y += cc.y; result.z += cc.z;
    }

    return result;
}

// ---- Lobe-specific BSDF evaluation (matches CPU sampling return values) ----
// Returns only the diffuse lobe component (Disney diffuse + sheen)
__device__ float3 gpu_evalDiffuseOnly(const float3& inDir, const float3& outDir,
                                       const HitInfo& surface) {
    if (gpu_isDielectric(surface)) return make_float3(0.0f, 0.0f, 0.0f);

    const float3 N = surface.surfaceNormal;
    const float3 V = inDir;
    const float3 L = outDir;

    const float NdotV = fmaxf(1e-6f, dot(N, V));
    const float NdotL = fmaxf(1e-6f, dot(N, L));
    const float3 H = normalize(V + L);
    const float NdotH = fmaxf(1e-6f, dot(N, H));
    const float LdotH = fmaxf(1e-6f, dot(L, H));

    const float rough = fmaxf(0.001f, surface.roughness);
    const float metallic = surface.metallic;
    const float3 base = surface.baseColor;
    float3 result = make_float3(0.0f, 0.0f, 0.0f);

    if (metallic < 1.0f) {
        // MATCH CPU: Fd = mix(1, Fd90, FL) * mix(1, Fd90, FV)
        float FL = gpu_schlickFresnel(NdotL);
        float FV = gpu_schlickFresnel(NdotV);
        float Fd90 = 0.5f + 2.0f * LdotH * LdotH * rough;
        float Fd = (1.0f + FL * (Fd90 - 1.0f)) * (1.0f + FV * (Fd90 - 1.0f));
        float3 Fd3 = base * INV_PI * Fd;
        float Fss90 = LdotH * LdotH * rough;
        float Fss = (1.0f + FL * (Fss90 - 1.0f)) * (1.0f + FV * (Fss90 - 1.0f));
        float ss = 1.25f * (Fss * (1.0f / (NdotL + NdotV + 1e-8f) - 0.5f) + 0.5f);
        float3 diff = (1.0f - metallic) * lerp3(Fd3, base * INV_PI * ss, fminf(surface.subsurface, 1.0f));
        result.x += diff.x; result.y += diff.y; result.z += diff.z;

        if (surface.sheen > 0.0f) {
            float sheenW = fminf(surface.sheen, 1.0f);
            float3 sheenCol = lerp3(make_float3(1,1,1), base, fminf(surface.sheenTint, 1.0f));
            float sTerm = gpu_schlickFresnel(NdotL);
            float3 s = (1.0f - metallic) * sheenW * sheenCol * sTerm;
            result.x += s.x; result.y += s.y; result.z += s.z;
        }
    }
    return result;
}

// Returns only the specular lobe component (GTR2/GGX only).
// Clearcoat is handled as a separate lobe in gpu_sampleBSDF and
// gpu_evalBSDF, so it must NOT be included here to avoid double-counting
// when used as the specular lobe's return value in gpu_sampleBSDF.
__device__ float3 gpu_evalSpecularOnly(const float3& inDir, const float3& outDir,
                                        const HitInfo& surface) {
    if (gpu_isDielectric(surface)) return make_float3(0.0f, 0.0f, 0.0f);

    const float3 N = surface.surfaceNormal;
    const float3 V = inDir;
    const float3 L = outDir;

    const float NdotV = fmaxf(1e-6f, dot(N, V));
    const float NdotL = fmaxf(1e-6f, dot(N, L));
    const float3 H = normalize(V + L);
    const float NdotH = fmaxf(1e-6f, dot(N, H));
    const float LdotH = fmaxf(1e-6f, dot(L, H));

    const float rough = fmaxf(0.001f, surface.roughness);
    const float alpha = rough * rough;
    const float metallic = surface.metallic;
    const float3 base = surface.baseColor;
    float3 result = make_float3(0.0f, 0.0f, 0.0f);

    // ---- Specular (GTR2/GGX) ----
    {
        float3 F0 = lerp3(make_float3(surface.specular,surface.specular,surface.specular), base, metallic);
        float sf = gpu_schlickFresnel(LdotH);
        float3 F = F0 + (make_float3(1,1,1) - F0) * sf;
        float D = gpu_GTR2(NdotH, alpha);
        float G = gpu_smithG(NdotV, alpha) * gpu_smithG(NdotL, alpha);
        float3 spec = D * F * G / (4.0f * NdotV * NdotL + 1e-8f);
        result.x += spec.x; result.y += spec.y; result.z += spec.z;
    }

    return result;
}

__device__ float gpu_pdfBSDF(const float3& inDir, const float3& outDir,
                              const HitInfo& surface) {
    // Dielectric BSDF is a delta distribution
    if (gpu_isDielectric(surface)) return 0.0f;

    const float3 N = surface.surfaceNormal;
    const float3 V = inDir;
    const float3 L = outDir;

    const float NdotL = fmaxf(1e-6f, dot(N, L));
    const float3 H = normalize(V + L);
    const float NdotH = fmaxf(1e-6f, dot(N, H));
    const float VdotH = fmaxf(1e-6f, dot(V, H));
    const float alpha = fmaxf(0.001f, surface.roughness * surface.roughness);

    const float pdfDiffuse = NdotL * INV_PI;
    const float pdfHalf = gpu_GTR2(NdotH, alpha) * NdotH;
    const float pdfSpecular = pdfHalf / (4.0f * VdotH + 1e-8f);

    // Clearcoat lobe PDF (GTR1) — must be included for MIS weight consistency
    // with gpu_sampleBSDF which samples clearcoat as a separate lobe.
    float pdfClearcoat = 0.0f;
    if (surface.clearcoat > 0.0f) {
        float ccGloss = fminf(surface.clearcoatGloss, 1.0f);
        float alphaCC = lerpf(0.1f, 0.001f, ccGloss);
        float ccPdfHalf = gpu_GTR1(NdotH, alphaCC) * NdotH;
        pdfClearcoat = ccPdfHalf / (4.0f * VdotH + 1e-8f);
    }

    // Blend lobe weights — must match gpu_sampleBSDF exactly (shared helper).
    // Fresnel-driven: glossy non-metals (stone, marble, glass) keep a real
    // chance of sampling the GGX specular lobe so highlights and reflections
    // don't vanish.
    float wDiffuse, wSpecular, wClearcoat;
    gpu_lobeWeights(surface, fmaxf(1e-6f, dot(N, V)), wDiffuse, wSpecular, wClearcoat);
    const float totalW = wDiffuse + wSpecular + wClearcoat;
    float pdf = 0.0f;
    if (totalW > 1e-8f) {
        pdf = (wDiffuse * pdfDiffuse + wSpecular * pdfSpecular + wClearcoat * pdfClearcoat) / totalW;
    }
    return pdf;
}

// Cosine-weighted hemisphere sampling (returns local direction, z-up)
__device__ float3 gpu_sampleCosine(float u1, float u2, float& pdf) {
    const float r     = sqrtf(u1);
    const float theta = 2.0f * PI * u2;
    const float x     = r * cosf(theta);
    const float y     = r * sinf(theta);
    const float z     = sqrtf(fmaxf(0.0f, 1.0f - x*x - y*y));
    pdf = z * INV_PI;
    return make_float3(x, y, z);
}

// GGX half-vector sampling
__device__ float3 gpu_sampleGGXHalf(float u1, float u2, float a, float& pdfH) {
    const float phi = 2.0f * PI * u1;
    const float cosTheta = sqrtf((1.0f - u2) / (1.0f + (a*a - 1.0f) * u2));
    const float sinTheta = sqrtf(fmaxf(0.0f, 1.0f - cosTheta * cosTheta));
    const float a2 = a * a;
    const float denom = (cosTheta * cosTheta * (a2 - 1.0f) + 1.0f);
    pdfH = (a2 / (PI * denom * denom)) * cosTheta;
    return make_float3(sinTheta * cosf(phi), sinTheta * sinf(phi), cosTheta);
}

// Importance-sample the BSDF (returns world-space outDir)
__device__ float3 gpu_sampleBSDF(float u1, float u2,
                                  const float3& inDir,
                                  const HitInfo& surface,
                                  float3& outDir, float& pdf,
                                  float mediumEta = 1.0f) {
    const float3 N = surface.surfaceNormal;

    // ---- Handle dielectrics (glass/transparent) ----
    if (gpu_isDielectric(surface)) {
        const float3 faceN = N;  // Always points toward the incident side
        // (gpu_getHitInfo flips geomNormal to face against ray.direction, then
        // aligns shadingNormal to that hemisphere). For both entering and
        // exiting, faceN faces where the ray came from. The refraction formula
        // in gpu_refract and the reflection formula below use this convention
        // together with inDir = -ray.direction (view direction, same hemisphere
        // as faceN) to produce the correct transmitted/reflected TRAVEL dir.
        const float cosThetaI = -dot(inDir, faceN);  // <0; only used as fabsf() below
        // IOR of the medium the ray is currently in (from medium stack)
        // and the medium it's transmitting into (the surface material or air).
        // For entering (air→glass): etaI = 1.0 (air), etaT = surface.eta
        // For exiting (glass→air):  etaI = surface.eta,   etaT = 1.0 (air)
        const float etaI = surface.entering ? mediumEta : fmaxf(surface.eta, 1.0f);
        const float etaT = surface.entering ? fmaxf(surface.eta, 1.0f) : 1.0f;

        // If IORs are equal (thin transparent surface), just pass through
        if (fabsf(etaI - etaT) < 1e-6f) {
            outDir = -inDir;  // straight through
            pdf = 1.0f;
            return make_float3(1.0f, 1.0f, 1.0f);
        }

        const float relEta = etaI / etaT;
        const float F = gpu_fresnelDielectric(fabsf(cosThetaI), relEta);

        if (u1 < F) {
            // Reflect: raw BSDF = Fresnel F, pdf = F
            // tracePath will compute: F / F = 1.0 (energy conserved)
            // outDir is the new ray TRAVEL direction. inDir is the VIEW
            // direction (= -ray.direction), so the reflected travel dir is
            // -inDir reflected about faceN:  -inDir + 2*dot(inDir,faceN)*faceN.
            // (The old formula `inDir - 2*dot(inDir,faceN)*faceN` reflected
            //  the view dir and pointed the new ray INTO the surface.)
            outDir = 2.0f * dot(inDir, faceN) * faceN - inDir;
            outDir = normalize(outDir);
            pdf = F;
            return make_float3(F, F, F);
        } else {
            // Refract
            float fresnelR;
            if (!gpu_refract(inDir, faceN, etaI, etaT, outDir, fresnelR)) {
                // TIR — all reflected: raw BSDF = 1.0, pdf = 1.0
                outDir = 2.0f * dot(inDir, faceN) * faceN - inDir;
                outDir = normalize(outDir);
                pdf = 1.0f;
                return make_float3(1.0f, 1.0f, 1.0f);
            }
            pdf = 1.0f - F;
            // Refraction: raw BTDF = (1-F) * (etaI/etaT)^2, pdf = 1-F
            // tracePath will compute: (1-F)*et² / (1-F) = et²
            float et = etaI / etaT;
            float et2 = et * et;
            return make_float3((1.0f - F) * et2, (1.0f - F) * et2, (1.0f - F) * et2);
        }
    }

    // ---- Opaque materials ----
    // Build local frame
    float3 tangent, bitangent;
    {
        const float3 ref = (fabsf(N.x) > 0.9f)
            ? make_float3(0,1,0) : make_float3(1,0,0);
        tangent = normalize(cross(N, ref));
        bitangent = normalize(cross(N, tangent));
    }

    // Transform V to local
    const float3 Vlocal = make_float3(dot(inDir, tangent),
                                       dot(inDir, bitangent),
                                       dot(inDir, N));

    const float alpha = fmaxf(0.001f, surface.roughness * surface.roughness);

    // ---- Lobe weights (match gpu_pdfBSDF exactly — shared helper) ----
    float wDiffuse, wSpecular, wClearcoat;
    gpu_lobeWeights(surface, fmaxf(1e-6f, dot(inDir, N)), wDiffuse, wSpecular, wClearcoat);
    float totalW = wDiffuse + wSpecular + wClearcoat;
    if (totalW < 1e-8f) {
        outDir = Vlocal.x*tangent + Vlocal.y*bitangent + Vlocal.z*N;
        pdf = 1.0f;
        return make_float3(0, 0, 0);
    }

    float uLobe = u1 * totalW;

    if (uLobe < wDiffuse) {
        // ---- Diffuse lobe ----
        const float uAdj = uLobe / fmaxf(wDiffuse, 1e-8f);
        float diffPdf;
        float3 Llocal = gpu_sampleCosine(uAdj, u2, diffPdf);
        if (Llocal.z < 0.0f) Llocal.z = -Llocal.z;
        outDir = Llocal.x * tangent + Llocal.y * bitangent + Llocal.z * N;
        // PDF: lobe-weighted diffuse PDF
        pdf = (wDiffuse / totalW) * diffPdf;
        // Return FULL BSDF (all lobes) for the sampled direction.
        // This is critical for MIS consistency: the NEE path evaluates
        // the full BSDF, so the BSDF-sampled path must also return the
        // full BSDF to keep the two strategies estimating the same
        // quantity. Using only the lobe-specific BSDF caused the two
        // estimators to disagree, breaking MIS weight balancing.
        return gpu_evalBSDF(inDir, outDir, surface);
    } else if (uLobe < wDiffuse + wSpecular) {
        // ---- Specular lobe ----
        float uAdj = (uLobe - wDiffuse) / fmaxf(wSpecular, 1e-8f);
        float pdfH;
        float3 Hlocal = gpu_sampleGGXHalf(uAdj, u2, alpha, pdfH);
        if (Hlocal.z < 0.0f) Hlocal.z = -Hlocal.z;
        const float VdotH = Vlocal.x*Hlocal.x + Vlocal.y*Hlocal.y + Vlocal.z*Hlocal.z;
        float3 Llocal = -Vlocal + 2.0f * VdotH * Hlocal;
        if (Llocal.z <= 0.0f) {
            pdf = 0.0f;
            return make_float3(0.0f, 0.0f, 0.0f);
        }
        outDir = Llocal.x*tangent + Llocal.y*bitangent + Llocal.z*N;
        // PDF: lobe-weighted GGX specular PDF
        float specPdf = pdfH / (4.0f * VdotH + 1e-8f);
        pdf = (wSpecular / totalW) * specPdf;
        // Return FULL BSDF (see diffuse lobe comment for rationale).
        return gpu_evalBSDF(inDir, outDir, surface);
    } else {
        // ---- Clearcoat lobe (GTR1) ----
        float alphaCC = lerpf(0.1f, 0.001f, fminf(surface.clearcoatGloss, 1.0f));
        float uAdj = (uLobe - wDiffuse - wSpecular) / fmaxf(wClearcoat, 1e-8f);
        float phi = 2.0f * PI * u2;
        // Inverse CDF of D_GTR1: CDF(c) = ln(1+(a2-1)c^2)/ln(a2)
        // => c^2 = (a2^u - 1)/(a2-1) = (1 - a2^(1-u))/(1-a2)
        float cosTheta = sqrtf((1.0f - powf(alphaCC*alphaCC, 1.0f - uAdj)) / (1.0f - alphaCC*alphaCC));
        if (cosTheta != cosTheta) cosTheta = 0.0f;
        float sinTheta = sqrtf(fmaxf(0.0f, 1.0f - cosTheta*cosTheta));
        float3 Hlocal = make_float3(sinTheta*cosf(phi), sinTheta*sinf(phi), cosTheta);
        if (Hlocal.z < 0.0f) Hlocal.z = -Hlocal.z;
        const float VdotH = Vlocal.x*Hlocal.x + Vlocal.y*Hlocal.y + Vlocal.z*Hlocal.z;
        float3 Llocal = -Vlocal + 2.0f * VdotH * Hlocal;
        if (Llocal.z <= 0.0f) {
            pdf = 0.0f;
            return make_float3(0.0f, 0.0f, 0.0f);
        }
        outDir = Llocal.x*tangent + Llocal.y*bitangent + Llocal.z*N;
        float3 H = normalize(Vlocal + Llocal);
        float ccPdf = gpu_GTR1(H.z, alphaCC) * H.z / (4.0f * VdotH + 1e-8f);
        pdf = (wClearcoat / totalW) * ccPdf;
        // Return FULL BSDF (see diffuse lobe comment for rationale).
        return gpu_evalBSDF(inDir, outDir, surface);
    }
}

// =========================================================================
// GPU Texture sampling helpers
// =========================================================================

// ---- BC (DXT) texel decoders ----
// These replicate the CPU decoders in material.cpp BIT-EXACTLY (all integer
// rounding: expand565 = (v*255)/31 or /63, blends = (2a+b+1)/3 and (a+b+1)/2,
// BC4/BC5 tables with the same +3/+2 rounding, BC5 Z reconstruction with the
// same truncating cast). Keeping a texture compressed in VRAM therefore leaves
// every sampled value identical — the RGBA8 upload and the BC path produce the
// same uchar4, and the bilinear below is untouched.
__device__ __forceinline__ unsigned bcExpR(unsigned c) { return ((c >> 11) & 0x1Fu) * 255u / 31u; }
__device__ __forceinline__ unsigned bcExpG(unsigned c) { return ((c >> 5) & 0x3Fu) * 255u / 63u; }
__device__ __forceinline__ unsigned bcExpB(unsigned c) { return (c & 0x1Fu) * 255u / 31u; }

// Value of ONE entry of the BC4-style 8-entry table (used by the alpha half of
// BC3 and both halves of BC5). Scalar-only on purpose: building the whole table
// in a local array pushed the kernel's per-thread stack past the CUDA default
// (1 KB) and produced stack-overflow faults on every launch.
__device__ __forceinline__ unsigned bcAlphaValue(const uint8_t* blk, int idx) {
    unsigned a0 = blk[0], a1 = blk[1];
    if (idx == 0) return a0;
    if (idx == 1) return a1;
    if (a0 > a1) return ((8 - idx) * a0 + (idx - 1) * a1 + 3) / 7;
    if (idx == 6) return 0u;
    if (idx == 7) return 255u;
    return ((6 - idx) * a0 + (idx - 1) * a1 + 2) / 5;
}
__device__ __forceinline__ int bcAlphaIndex(const uint8_t* idxBits, int texel) {
    int bitOff = 3 * texel, byteOff = bitOff / 8, bitShift = bitOff % 8;
    int idx = (idxBits[byteOff] >> bitShift) & 7;
    if (byteOff + 1 < 6 && bitShift > 5) idx |= (idxBits[byteOff + 1] << (8 - bitShift)) & 7;
    return idx & 7;
}

// fmt: 1 = BC1 (8 B/block), 2 = BC3 (16 B), 3 = BC5/ATI2 (16 B).
// texel = (y&3)*4 + (x&3) inside the block.
__device__ __forceinline__ uchar4 bcDecodeTexel(int fmt, const uint8_t* blk, int texel) {
    const int x = texel & 3, y = texel >> 2;
    if (fmt == 1) {
        unsigned c0 = (unsigned)blk[0] | ((unsigned)blk[1] << 8);
        unsigned c1 = (unsigned)blk[2] | ((unsigned)blk[3] << 8);
        int idx = (blk[4 + y] >> (2 * x)) & 3;
        int r0 = bcExpR(c0), g0 = bcExpG(c0), b0 = bcExpB(c0);
        int r1 = bcExpR(c1), g1 = bcExpG(c1), b1 = bcExpB(c1);
        int r, g, b, a;
        if (c0 > c1) {
            if (idx == 0)      { r = r0; g = g0; b = b0; }
            else if (idx == 1) { r = r1; g = g1; b = b1; }
            else if (idx == 2) { r = (2*r0+r1+1)/3; g = (2*g0+g1+1)/3; b = (2*b0+b1+1)/3; }
            else               { r = (r0+2*r1+1)/3; g = (g0+2*g1+1)/3; b = (b0+2*b1+1)/3; }
            a = 255;
        } else {
            if (idx == 0)      { r = r0; g = g0; b = b0; a = 255; }
            else if (idx == 1) { r = r1; g = g1; b = b1; a = 255; }
            else if (idx == 2) { r = (r0+r1+1)/2; g = (g0+g1+1)/2; b = (b0+b1+1)/2; a = 255; }
            else               { r = 0; g = 0; b = 0; a = 0; }
        }
        return make_uchar4((unsigned char)r, (unsigned char)g, (unsigned char)b, (unsigned char)a);
    }
    unsigned ax = bcAlphaValue(blk, bcAlphaIndex(blk + 2, texel));
    if (fmt == 2) {
        // BC3: NOTE the CPU decoder (material.cpp decompressDXT5) writes the
        // interpolated alpha first and then calls decompressDXT1 on the color
        // half, which OVERWRITES the alpha channel. The effective alpha of a
        // DXT5 texture in this pipeline is therefore the BC1 1-bit alpha, not
        // the smooth 8-bit one. Replicating that exactly (instead of decoding
        // the alpha block) is what keeps the two paths byte-identical.
        return bcDecodeTexel(1, blk + 8, texel);
    }
    // BC5: R from the first BC4 half, G from the second, then Z reconstruction
    // identical to the CPU. The arithmetic uses explicit non-contracted ops
    // (__fmul_rn/__fsub_rn/__fadd_rn) because nvcc contracts by default while
    // the host build (SSE2 baseline, no FMA) does not — matching the host's
    // separate roundings keeps the truncating cast below bit-identical.
    unsigned bx = ax;
    unsigned by = bcAlphaValue(blk + 8, bcAlphaIndex(blk + 10, texel));
    float rf = bx / 255.0f * 2.0f - 1.0f;
    float gf = by / 255.0f * 2.0f - 1.0f;
    float t1 = __fmul_rn(rf, rf);
    float t2 = __fsub_rn(1.0f, t1);
    float t3 = __fmul_rn(gf, gf);
    float t4 = __fsub_rn(t2, t3);
    float bf = sqrtf(fmaxf(0.0f, t4));
    float u1 = __fmul_rn(bf, 0.5f);
    float u2 = __fadd_rn(u1, 0.5f);
    float u3 = __fmul_rn(u2, 255.0f);
    unsigned bz = (unsigned)u3;
    return make_uchar4((unsigned char)bx, (unsigned char)by, (unsigned char)bz, 255);
}

// Simple bilinear texture lookup (taps come from the RGBA8 buffer or from
// in-kernel BC decoding, per mb.texFormats[texId]).
__device__ inline float4 gpu_sampleTexture(int texId, float u, float v,
                                            const GPUMaterialBuffer& mb) {
    if (texId < 0 || texId >= mb.numTextures) return make_float4(1,1,1,1);
    if (!mb.texData || !mb.texDataWidths || !mb.texDataHeights) return make_float4(1,1,1,1);
    int w = mb.texDataWidths[texId], h = mb.texDataHeights[texId];
    if (w <= 0 || h <= 0) return make_float4(1,1,1,1);
    u = u - floorf(u); v = v - floorf(v);
    float fx = u * w - 0.5f;
    float fy = v * h - 0.5f;
    int ix = int(floorf(fx));
    int iy = int(floorf(fy));
    float tx = fx - floorf(fx);
    float ty = fy - floorf(fy);
    int x0 = (ix + w) % w; int x1 = (ix + 1 + w) % w;
    int y0 = (iy + h) % h; int y1 = (iy + 1 + h) % h;
    float4 c00, c10, c01, c11;
    const int fmt = mb.texFormats ? mb.texFormats[texId] : 0;
    if (fmt > 0 && mb.texByteOffsets) {
        const uint8_t* base = mb.texData + mb.texByteOffsets[texId];
        const int blkBytes = (fmt == 1) ? 8 : 16;
        const int bw = (w + 3) / 4;
        auto tap = [&](int x, int y) -> float4 {
            uchar4 c = bcDecodeTexel(fmt, base + ((y >> 2) * bw + (x >> 2)) * blkBytes,
                                     ((y & 3) << 2) + (x & 3));
            return make_float4(c.x, c.y, c.z, c.w);
        };
        c00 = tap(x0, y0); c10 = tap(x1, y0); c01 = tap(x0, y1); c11 = tap(x1, y1);
    } else {
        const long long base = mb.texByteOffsets ? mb.texByteOffsets[texId]
                                                 : (long long)mb.texDataOffsets[texId] * 4;
        const uchar4* td = reinterpret_cast<const uchar4*>(mb.texData + base);
        c00 = make_float4(td[y0 * w + x0].x, td[y0 * w + x0].y, td[y0 * w + x0].z, td[y0 * w + x0].w);
        c10 = make_float4(td[y0 * w + x1].x, td[y0 * w + x1].y, td[y0 * w + x1].z, td[y0 * w + x1].w);
        c01 = make_float4(td[y1 * w + x0].x, td[y1 * w + x0].y, td[y1 * w + x0].z, td[y1 * w + x0].w);
        c11 = make_float4(td[y1 * w + x1].x, td[y1 * w + x1].y, td[y1 * w + x1].z, td[y1 * w + x1].w);
    }
    float4 result;
    result.x = ((c00.x*(1-tx) + c10.x*tx)*(1-ty) + (c01.x*(1-tx) + c11.x*tx)*ty) / 255.0f;
    result.y = ((c00.y*(1-tx) + c10.y*tx)*(1-ty) + (c01.y*(1-tx) + c11.y*tx)*ty) / 255.0f;
    result.z = ((c00.z*(1-tx) + c10.z*tx)*(1-ty) + (c01.z*(1-tx) + c11.z*tx)*ty) / 255.0f;
    result.w = ((c00.w*(1-tx) + c10.w*tx)*(1-ty) + (c01.w*(1-tx) + c11.w*tx)*ty) / 255.0f;
    return result;
}

// Sample a normal map and transform from tangent space to world space.
__device__ inline float3 gpu_sampleNormalMap(int texId, float u, float v,
                                              const float3& shadingNormal,
                                              const GPUModelData& model,
                                              int triBase) {
    if (texId < 0) return shadingNormal;

    const GPUMaterialBuffer& mb = model.materials;
    float4 nmap = gpu_sampleTexture(texId, u, v, mb);
    // Decode normal from [0,1] to [-1,1]
    float3 tangentNormal = make_float3(
        nmap.x * 2.0f - 1.0f,
        nmap.y * 2.0f - 1.0f,
        nmap.z * 2.0f - 1.0f);
    float len = dot(tangentNormal, tangentNormal);
    if (len < 1e-8f) return shadingNormal;
    tangentNormal = tangentNormal / sqrtf(len);

    // Compute tangent and bitangent
    const float3 v0 = model.vertPositions[triBase + 0];
    const float3 v1 = model.vertPositions[triBase + 1];
    const float3 v2 = model.vertPositions[triBase + 2];

    const float2 uv0 = model.vertUVs[triBase + 0];
    const float2 uv1 = model.vertUVs[triBase + 1];
    const float2 uv2 = model.vertUVs[triBase + 2];

    float3 e1 = v1 - v0, e2 = v2 - v0;
    float2 duv1 = uv1 - uv0, duv2 = uv2 - uv0;
    float det = duv1.x * duv2.y - duv1.y * duv2.x;
    if (fabsf(det) < 1e-8f) return shadingNormal;
    float invDet = 1.0f / det;
    float3 tangent = normalize((e1 * duv2.y - e2 * duv1.y) * invDet);
    float3 bitangent = normalize(cross(tangent, shadingNormal));
    tangent = normalize(cross(bitangent, shadingNormal));

    // Transform normal from tangent to world space
    return normalize(
        tangent * tangentNormal.x +
        bitangent * tangentNormal.y +
        shadingNormal * tangentNormal.z);
}

// =========================================================================
// GPU HitInfo builder (with texture support)
// =========================================================================
__device__ HitInfo gpu_getHitInfo(const Ray& ray, const HitRecord& hit,
                                   const GPUModelData& model,
                                   float emissionScale) {
    const int faceIdx = hit.faceIdx;
    const int triBase = faceIdx * 3;

    const float3 v0 = model.vertPositions[triBase + 0];
    const float3 v1 = model.vertPositions[triBase + 1];
    const float3 v2 = model.vertPositions[triBase + 2];

    const float3 n0 = model.vertNormals[triBase + 0];
    const float3 n1 = model.vertNormals[triBase + 1];
    const float3 n2 = model.vertNormals[triBase + 2];

    const float2 uv0 = model.vertUVs[triBase + 0];
    const float2 uv1 = model.vertUVs[triBase + 1];
    const float2 uv2 = model.vertUVs[triBase + 2];

    const float w = 1.0f - hit.u - hit.v;
    const float3 pos = ray.origin + ray.direction * hit.t;
    float3 shadingNormal = normalize(w * n0 + hit.u * n1 + hit.v * n2);
    const float3 e1 = v1 - v0, e2 = v2 - v0;
    float3 geomNormal = normalize(cross(e1, e2));
    if (dot(geomNormal, ray.direction) > 0.0f) geomNormal = -geomNormal;

    // Determine entering/exiting using the ORIGINAL (un-flipped) shading
    // normal. For well-formed geometry (procedural scenes + models loaded
    // with aiProcess_FixInfacingNormals) the interpolated vertex normals
    // point outward, so dot(outwardNormal, ray.direction) < 0 means the ray
    // is going INTO the object. We must NOT use origGeomNormal here: the
    // geometric normal from cross(e1,e2) depends on triangle winding and
    // points INWARD for this codebase's sphere/ground winding, which would
    // invert the entering flag and swap the IOR ratio at every glass surface.
    const bool entering = dot(shadingNormal, ray.direction) < 0.0f;

    // Ensure shading normal faces same hemisphere as geometric normal.
    // Using geomNormal (consistently oriented across triangle boundaries)
    // instead of -ray.direction prevents visible edge discontinuities.
    if (dot(shadingNormal, geomNormal) < 0.0f)
        shadingNormal = -shadingNormal;

    const int matId = model.materialIds[faceIdx];
    const GPUMaterialBuffer& mb = model.materials;

    // Interpolate UV coordinates
    float2 uv = make_float2(
        w * uv0.x + hit.u * uv1.x + hit.v * uv2.x,
        w * uv0.y + hit.u * uv1.y + hit.v * uv2.y);
    // Per-material UV mirror (RENDER_UVFLIP_U / MenuSign). Mirror about the
    // FACE's own UV-bbox center: the MenuSign boards sample only the MENU
    // half of the atlas (u~[0.02,0.48]), and a global u'=1-u would jump
    // them to the scooter half (wrong content). Mode 1 = u mirror (generic
    // knob). Mode 2 = V mirror — close-up forensics: the MenuSign face UVs
    // are authored V-flipped for this viewing side (u-only render read the
    // text rotated 180, u+v render read it horizontally mirrored; v-only is
    // the identity that yields upright readable "MENU"). Faces that span the
    // full [0,1] reduce to the old global mirror. NOTE: the flip also
    // mirrors the normal map's x (mode 1) or y (mode 2) — fine for flat
    // signage.
    if (mb.uvFlipU && matId < mb.numMaterials && mb.uvFlipU[matId]) {
        if (mb.uvFlipU[matId] == 2) {
            const float vMin = fminf(uv0.y, fminf(uv1.y, uv2.y));
            const float vMax = fmaxf(uv0.y, fmaxf(uv1.y, uv2.y));
            uv.y = (vMin + vMax) - uv.y;
        } else {
            const float uMin = fminf(uv0.x, fminf(uv1.x, uv2.x));
            const float uMax = fmaxf(uv0.x, fmaxf(uv1.x, uv2.x));
            uv.x = (uMin + uMax) - uv.x;
        }
    }

    // Sample textures
    float4 bcTex = gpu_sampleTexture(mb.baseColorTexIds[matId], uv.x, uv.y, mb);
    const int specFlag = mb.packedSpecFlags ? mb.packedSpecFlags[matId] : 0;
    bool packedSpec = (specFlag == 1);   // G channel = roughness
    bool glossSpec  = (specFlag == 2);   // G channel = glossiness (rough = 1-G)
    float roughnessVal = mb.roughnessFactors[matId];
    if (mb.roughnessTexIds[matId] >= 0) {
        float4 rt = gpu_sampleTexture(mb.roughnessTexIds[matId], uv.x, uv.y, mb);
        // Packed SPECULAR format: roughness in G channel
        roughnessVal = packedSpec ? rt.y : (glossSpec ? 1.0f - rt.y : rt.x);
    }
    float metallicVal = mb.metallicFactors[matId];
    if (mb.metallicTexIds[matId] >= 0) {
        float4 mt = gpu_sampleTexture(mb.metallicTexIds[matId], uv.x, uv.y, mb);
        metallicVal = mt.x;  // Metallic in R channel (or G for some maps)
        if (metallicVal < 0.01f && mt.y > 0.01f) metallicVal = mt.y;  // Fallback
    } else if (packedSpec && mb.roughnessTexIds[matId] >= 0) {
        // Packed SPECULAR format: metallic in B channel
        float4 rt = gpu_sampleTexture(mb.roughnessTexIds[matId], uv.x, uv.y, mb);
        metallicVal = fminf(rt.z, 0.99f);
    }

    // Normal map sampling
    float3 finalNormal = shadingNormal;
    if (mb.normalTexIds[matId] >= 0) {
        finalNormal = gpu_sampleNormalMap(mb.normalTexIds[matId], uv.x, uv.y,
                                           shadingNormal, model, triBase);
    }

    HitInfo info;
    info.position      = pos;
    info.shapeNormal   = geomNormal;
    info.surfaceNormal = finalNormal;
    // Base color: sampled texture color × material factor.
    // BaseColor DDS is sRGB-encoded — convert to linear (pow 2.2) before any
    // lighting math, matching the reference gammaPow. Skipping this compresses
    // texture contrast by ~2.2x on display and was why chair/wood grain was
    // invisible.
    float3 texColor = gpu_toLinear(make_float3(bcTex.x, bcTex.y, bcTex.z));
    float3 bcf = mb.baseColorFactors[matId];
    if (dot(bcf, bcf) < 1e-8f) bcf = make_float3(0.5f, 0.4f, 0.3f);
    info.baseColor = texColor * bcf;
    if (dot(info.baseColor, info.baseColor) < 1e-8f)
        info.baseColor = bcf;
    // Emissive texture sampling (GPU path): if an emissive texture exists, use it
    // to modulate the emission factor, giving lamps their patterned appearance
    // instead of uniform white blobs. Emission maps are sRGB too.
    // Emissive energy scale moved off the load-time material bake (per-
    // render / per-profile): multiply the raw factor here, BEFORE the
    // texture modulation, so the product chain matches the historical
    // (factor x scale) x emCol order bit-for-bit at scale=1.
    info.emission = mb.emissionFactors[matId] * emissionScale;
    if (mb.emissionTexIds[matId] >= 0) {
        float4 emTex = gpu_sampleTexture(mb.emissionTexIds[matId], uv.x, uv.y, mb);
        // Emissive contrast gamma 5 — matches the bakeAverageTexel domain and
        // the flux-compensated emissionFactor (see model.cpp). Keeps bulb
        // pixels white while map mid-grays (frames/stems) stay dark.
        float emG = (model.materials.emissionGamma && matId < model.materials.numMaterials)
                  ? model.materials.emissionGamma[matId] : 5.0f;
        float3 emCol = make_float3(powf(emTex.x, emG),
                                   powf(emTex.y, emG),
                                   powf(emTex.z, emG));
        info.emission = info.emission * emCol;
    }
    info.specular      = mb.specularFactors[matId];
    info.roughness     = fmaxf(0.001f, roughnessVal);
    info.metallic      = metallicVal;
    info.opacity       = mb.opacities[matId] * bcTex.w;  // Alpha from baseColor
    info.eta           = mb.iors[matId];
    info.materialID    = matId;
    // entering was computed above from the original outward shading normal
    // (see comment there for why origGeomNormal cannot be used).
    info.entering      = entering;
    return info;
}

// =========================================================================
// traceRay — BVH traversal + triangle intersection on GPU
// =========================================================================
__device__ HitRecord traceRay(const Ray& ray, const GPUModelData& model) {
    return GPUBVHTraverser::traverse(
        ray, model.bvhNodes, model.vertPositions,
        model.materialIds, model.numBVHNodes);
}

// MediumRecord/MediumStack live in render_gpu.h (shared with the wavefront
// pipeline, which persists them inside GPUPathState between stage kernels).
__device__ float3 gpu_applyAbsorption(const MediumStack& ms, float distance) {
    float3 a = ms.topAbsorb();
    return make_float3(expf(-a.x*distance), expf(-a.y*distance), expf(-a.z*distance));
}

// Sky lighting pdf at an arbitrary direction under the same CDF the NEE
// sampler uses: texel t carries probability mass cdf[t+1]-cdf[t].
__device__ float gpu_skyEnvPdfAt(float3 d, const float* skyCDF, int skyW, int skyH) {
    if (!skyCDF || skyW <= 0 || skyH <= 0) return 0.0f;
    float theta = acosf(fmaxf(-1.0f, fminf(1.0f, d.y)));
    float sinTheta = sinf(theta);
    if (sinTheta < 1e-6f) return 0.0f;
    float az = atan2f(-d.x, d.z);   // reference equirect convention (see geometry.h)
    float u = az * INV_TWO_PI + 0.5f;
    float v = theta * INV_PI;
    int px = min(max((int)(u * skyW), 0), skyW - 1);
    int py = min(max((int)(v * skyH), 0), skyH - 1);
    int t = py * skyW + px;
    float mass = skyCDF[t + 1] - skyCDF[t];
    int n = skyW * skyH;
    return mass * (float)n / (2.0f * PI * PI * sinTheta);
}

// 0 at the sun direction, 1 at/after the cone edge (same cone as the
// applySunConeFocus dome transform). See GPURenderArgs::sunConeKill.
__device__ float gpu_sunConeMask(float3 d, float3 sunDir, float halfRad) {
    float c = fmaxf(-1.0f, fminf(1.0f,
        dot(normalize(d), normalize(sunDir))));
    float ang = acosf(c);
    float t = fminf(ang / fmaxf(halfRad, 1e-4f), 1.0f);
    return t * t * (3.0f - 2.0f * t);
}

// =========================================================================
// tracePath — iterative path tracing on GPU
// =========================================================================
// Skybox importance sampling: skyCDF is the luminance CDF on GPU (length = skyW*skyH+1)
__device__ float3 gpu_transparentShadow(float3 org, float3 dir, float maxDist,
                                        const GPUModelData& model,
                                        bool& blocked) {
    float3 T = make_float3(1.0f, 1.0f, 1.0f);
    blocked = false;
    float remaining = maxDist;
    for (int pass = 0; pass < 8; ++pass) {
        HitRecord h = traceRay(Ray(org, dir), model);
        if (!h.hasHit() || h.t <= 0.0f || h.t >= remaining - 1e-4f)
            return T;                                   // clear to the light
        // Shadow fast path: materials classified on the host as provably-opaque
        // blockers (!alphaCutout && opacityFactor*minAlpha >= 0.99) can never
        // trigger the cutout pass-through (needs opacity < 0.5) nor the
        // dielectric Fresnel tint (needs opacity < 0.99), so ANY hit on them
        // blocks the ray — bit-identically to the code below, without paying
        // for the full hit-info evaluation (baseColor/roughness/metallic/
        // emission texture fetches + the powf gamma chains).
        {
            const int mid = model.materialIds[h.faceIdx];
            if (model.materials.shadowOpaque && mid >= 0 &&
                mid < model.materials.numMaterials &&
                model.materials.shadowOpaque[mid]) {
                blocked = true;
                return T;
            }
        }
        // Shadow helper: emission is not consumed on this path (opacity/
        // normal/eta only) — the scale arg is inert here.
        HitInfo hs = gpu_getHitInfo(Ray(org, dir), h, model, 1.0f);
        // Alpha-cutout foliage: texels with alpha < 0.5 are holes — the shadow
        // ray passes through, matching the camera-path cutout branch (otherwise
        // canopies would punch holes for camera rays but cast solid shadows).
        // RENDER_LEAF_SHADOW_TINT < 1 attenuates each pass: real canopies
        // leak less light through their gaps (sub-texel leaf coverage), so
        // the terrace dapple matches the official's dimmer sun patches.
        if (model.materials.alphaCutout && hs.materialID >= 0 &&
            hs.materialID < model.materials.numMaterials &&
            model.materials.alphaCutout[hs.materialID] && hs.opacity < 0.5f) {
            T = T * d_leafShadowTint;
            org = org + dir * (h.t + EPS_ZERO * 4.0f);
            remaining -= h.t + EPS_ZERO * 4.0f;
            if (remaining <= 0.0f) return T;
            if (T.x < 0.05f && T.y < 0.05f && T.z < 0.05f) { blocked = true; return T; }
            continue;
        }
        if (!gpu_isDielectric(hs)) { blocked = true; return T; }
        // Schlick reflectance of this interface (approximate eta vs air)
        float cosI = fabsf(dot(dir, hs.surfaceNormal));
        float e = fmaxf(hs.eta, 1.01f);
        float f0 = (e - 1.0f) / (e + 1.0f); f0 *= f0;
        float Fr = f0 + (1.0f - f0) * powf(fmaxf(0.0f, 1.0f - cosI), 5.0f);
        T = T * (1.0f - Fr);
        org = org + dir * (h.t + EPS_ZERO * 4.0f);
        remaining -= h.t + EPS_ZERO * 4.0f;
        if (remaining <= 0.0f) return T;
        if (T.x < 0.05f && T.y < 0.05f && T.z < 0.05f) { blocked = true; return T; }
    }
    blocked = true;
    return T;
}

// =========================================================================
// GPUPathState + gpu_shadeBounce — the per-bounce shading body, shared by
// the megakernel (tracePath keeps it in registers) and the wavefront
// pipeline (persisted in global memory between stage kernels). Every
// expression, every RNG draw and every accumulation keeps the exact order
// of the original monolithic tracePath, so both pipelines produce
// byte-identical images for the same per-path seed stream.
// =========================================================================
__device__ void gpu_pathInit(GPUPathState& st, uint32_t seed, const Ray& inRay) {
    st.ray = inRay;
    st.throughput = make_float3(1.0f, 1.0f, 1.0f);
    // Path roughness regularization (reference render.cpp): propagate the max
    // roughness along the path so indirect glossy chains widen their GGX
    // lobes — the main firefly suppressor for floor/glass reflections
    st.pathRough = 0.0f;
    st.radiance = make_float3(0.0f, 0.0f, 0.0f);
    curand_init(seed, 0, 0, &st.rng);
    st.medStack.init();
    // MIS records for the previous vertex's (multiple) NEE samples.
    // Matching is at MATERIAL level: any implicit hit on the same emitter
    // material (any of its thousands of lamp-shade triangles) counts as
    // covered, not just the exact triangle a NEE sample landed on.
    st.misSampledCnt = 0;
    // Sky MIS carriers: the balance weight for an escaping ray needs the
    // BSDF pdf of THIS ray (sampled at the previous vertex) and the env
    // pdf at the escape direction (evaluated at escape time).
    st.prevBsdfPdf = 0.0f;
    st.prevBsdfDielectric = false;
    // baseColor of the previous vertex — needed to split implicit emitter
    // hits / sky escapes into Indirect Diffuse vs Indirect Specular
    st.prevBaseColor = make_float3(1.0f, 1.0f, 1.0f);
    st.depth = 0;
    st.alive = 1;
}

__device__ __noinline__ bool gpu_shadeBounce(
    GPUPathState&    st,
    PerPixelData&    split,
    const HitRecord& hit,
    const GPUModelData& model,
    cudaTextureObject_t skyTex,
    const GPURenderArgs& args,
    bool             excludeDirect)
{
    const float sunConeHalfRad = args.sunConeHalfDeg * 0.01745329252f;
    const int depth = st.depth;
    // GI bounce energy scale (RENDER_INDIRECT_SCALE): every contribution
    // contributed at depth > 0 scales uniformly — direct (depth-0) lighting
    // untouched, the MIS pairings among depth>0 estimators stay balanced.
    const float indS = (depth > 0) ? args.indirectScale : 1.0f;
    Ray ray = st.ray;
    float3 throughput = st.throughput;
    float3 radiance = st.radiance;

    if (!hit.hasHit()) {
        // Sample HDR sky texture (equirectangular mapping)
        float3 skyRad = make_float3(0.0f, 0.0f, 0.0f);
        const float3 escDir = normalize(ray.direction);
        if (skyTex) {
            float u = atan2f(-escDir.x, escDir.z) * INV_TWO_PI + 0.5f;  // reference convention
            float v = acosf(fmaxf(-1.0f, fminf(1.0f, escDir.y))) * INV_PI;
            float4 tex = tex2D<float4>(skyTex, u, v);
            skyRad = make_float3(tex.x, tex.y, tex.z);
        } else {
            // No sky texture: return black.
            // The Cornell box is a closed scene (no sky visible), and the
            // glass sphere test uses a checkerboard background. Without a
            // sky texture, we should not fabricate a sky — doing so would
            // leak an unnatural blue glow through the Cornell box opening.
            skyRad = make_float3(0.0f, 0.0f, 0.0f);
        }
        if (depth == 0) {
            split.emission += throughput * skyRad;
        } else {
            // Sky MIS (proper balance heuristic): the escape estimator
            // carries its OWN weight at the ESCAPE direction,
            // pb²/(pb²+pn²) — the old code reused 1-w_nee evaluated at
            // the NEE sample's direction, a different direction, which
            // left the escape estimator with positive-bias leftover
            // energy whenever the NEE draw missed the bright region the
            // escape ray happened to hit. Dielectric interfaces carry
            // delta-sampling pdfs not comparable to an env-map density
            // — keep them at full weight (their NEE sky side evaluates
            // to ~0, so no double count can occur).
            float skyW = 1.0f;
            if (!st.prevBsdfDielectric && st.prevBsdfPdf > 0.0f &&
                args.skyCDF && args.skyWidth > 0) {
                float pn = gpu_skyEnvPdfAt(escDir, args.skyCDF, args.skyWidth, args.skyHeight);
                float pb2 = st.prevBsdfPdf * st.prevBsdfPdf;
                float pn2 = pn * pn;
                skyW = pb2 / (pb2 + pn2 + 1e-16f);
            }
            if (args.sunConeKill) skyW *= gpu_sunConeMask(escDir, args.sunDir, sunConeHalfRad);
            float3 skyContrib = throughput * skyRad * skyW * indS;
            gpu_decomposeLobe(st.prevBaseColor, skyContrib,
                              split.radiance_Id, split.radiance_Is);
            radiance += skyContrib;
        }
        if (depth == 0) radiance += throughput * skyRad;
        st.radiance = radiance;
        return false;
    }

    // Build surface info using texture-aware hit info builder
    HitInfo surface = gpu_getHitInfo(ray, hit, model, args.emissionScale);

    // Roughness regularization: widen this and all subsequent lobes to the
    // max roughness seen so far on the path (matches reference)
    st.pathRough = fmaxf(st.pathRough, surface.roughness);
    surface.roughness = fmaxf(surface.roughness, st.pathRough);

    // NaN/Inf guard on surface data
    if (isnan(surface.baseColor.x) || isnan(surface.emission.x) ||
        isinf(surface.baseColor.x) || isinf(surface.emission.x)) {
        return false;
    }

    // Alpha-cutout foliage (leaf/flower cards): texels with alpha < 0.5
    // are holes — skip shading and pass the ray through. depth++ bounds
    // the iteration (dense foliage consumes bounces, like transparency).
    if (model.materials.alphaCutout && model.materials.alphaCutout[surface.materialID] &&
        surface.opacity < 0.5f) {
        st.ray = Ray(surface.position + ray.direction * (EPS_ZERO * 4.0f), ray.direction);
        st.depth = depth + 1;
        return true;
    }

    // ---- Beer's law absorption ----
    // Apply absorption for the distance traveled in the CURRENT medium
    // (the one the ray was just traveling in, BEFORE any push/pop).
    // The medium stack is updated AFTER BSDF sampling (see below), so
    // at this point it still reflects the medium the ray arrived in.
    if (st.medStack.count() > 0 && depth > 0 && hit.t > 0) {
        float3 absorb = gpu_applyAbsorption(st.medStack, hit.t);
        throughput = throughput * absorb;
    }

    // First hit AOV
    if (depth == 0) {
        split.baseColor  = surface.baseColor;
        split.normal     = surface.surfaceNormal;
        split.position   = surface.position;
        split.shapeNormal = surface.shapeNormal;
        split.roughness  = surface.roughness;
        split.metallic   = surface.metallic;
        split.specular   = surface.specular;
        split.opacity    = surface.opacity;
    }
    // NaN/Inf guard on emission
    float3 emContrib = surface.emission;
    if (isnan(emContrib.x) || isnan(emContrib.y) || isnan(emContrib.z) ||
        isinf(emContrib.x) || isinf(emContrib.y) || isinf(emContrib.z)) {
        emContrib = make_float3(0,0,0);
    }
    // Firefly clamp on emission
    float emLum = dot(emContrib, make_float3(0.299f, 0.587f, 0.114f));
    if (emLum > 1000.0f) emContrib = emContrib * (1000.0f / emLum);

    // Terminate path on emissive surface (implicit light connection).
    // MIS: full weight unless the BSDF hit the SAME light triangle the
    // previous NEE sampled (that emitter is then already covered by NEE).
    if (emLum > 1e-6f) {
        float misW = 1.0f;
        if (depth > 0 && st.misSampledCnt > 0) {
            // Material-level match: NEE covered this EMITTER (any triangle)
            float wsum = 0.0f; int matched = 0;
            for (int j = 0; j < st.misSampledCnt; ++j)
                if (surface.materialID == st.misSampledMat[j]) { wsum += st.misSampledW[j]; matched++; }
            if (matched > 0) misW = fmaxf(0.0f, 1.0f - wsum / float(matched));
        }
        float3 misEm = emContrib * misW * indS
                     * gpu_emissionLobe(model.materials, surface.materialID, -ray.direction,
                                        surface.shapeNormal);
        if (depth > 0) {
            // Firefly clamp on the FINAL contribution (depth > 0)
            float3 finalEm = throughput * misEm;
            float emLum2 = dot(finalEm, make_float3(0.299f, 0.587f, 0.114f));
            // Indirect emitter hits: tight clamp (these are the firefly
            // source). Direct lamp view at depth 0 keeps the 1000 clamp
            // so HDR bloom halos still form.
            if (emLum2 > 200.0f) finalEm = finalEm * (200.0f / emLum2);
            gpu_decomposeLobe(st.prevBaseColor, finalEm,
                              split.radiance_Id, split.radiance_Is);
            radiance += finalEm;
        } else {
            split.emission += throughput * misEm;
            radiance += throughput * misEm;
        }
        st.radiance = radiance;
        return false;
    }

    // Ambient light removed — shadows emerge naturally from NEE shadow
    // rays blocking direct light, and indirect bounces being dimmer in
    // occluded regions. The old ambient term (baseColor * 0.05f) was
    // added at every bounce without shadow testing, washing out all
    // shadows and making furniture cast no visible shadows on the floor.

    // ---- Russian roulette (matching CPU render.cpp logic) ----
    // The CPU applies RR at EVERY bounce (not just deep bounces), scaling
    // the surviving throughput by 1/(1-P_RR). This accumulated scaling
    // at each bounce is what makes the CPU's indirect light brighter.
    // The GPU previously only applied RR at depth >= 3, so early bounces
    // (depth 0-2) were never scaled — making the GPU consistently dimmer.
    //
    // CPU's doNEE = P_RR < 0.9 && !isDielectric:
    //   - Smooth surfaces (P_RR >= 0.9) skip RR to preserve reflections
    //   - Dielectrics (glass) skip RR (no NEE needed)
    if (!gpu_isDielectric(surface)) {
        const float P_RR_Rough = 0.5f, P_RR_Smooth = 1.0f;
        const float P_RR = P_RR_Smooth + (P_RR_Rough - P_RR_Smooth) * sqrtf(surface.roughness);
        const float P_RR_THRESH = 0.9f;
        if (P_RR < P_RR_THRESH) {
            if (curand_uniform(&st.rng) > P_RR) { st.throughput = throughput; st.radiance = radiance; return false; }
            // Divide by the SURVIVAL probability P_RR (not 1-P_RR). The path
            // continues with probability P_RR, so surviving paths must be
            // boosted by 1/P_RR to remain unbiased. Dividing by 1-P_RR (the
            // termination probability) over-scaled indirect light by 1.8-5.3x
            // per bounce, blowing out white walls/floor/ceiling.
            throughput = throughput / fmaxf(P_RR, 1e-4f);
        }
    }
    // Throughput clamp after RR scaling (matches CPU THROUGHPUT_CLAMP=500).
    float tLum = dot(throughput, make_float3(0.299f, 0.587f, 0.114f));
    if (tLum > 500.0f) throughput = throughput * (500.0f / tLum);

    // Reset MIS records for this bounce. Entries are only added when an
    // NEE sample actually SEES its emitter (occluded samples must leave
    // their emitter fully covered by BSDF sampling instead).
    st.misSampledCnt = 0;

    // Direct light sampling (NEE): sample area lights + skybox
    if (!excludeDirect) {
        // ---- Two-level, distance-weighted, multi-sample NEE ----
        // Object selection weight is evaluated PER SHADING POINT:
        //   w_i = power_i / (dist²_i + area_i)  ∝ irradiance potential
        // so tiny-but-near sources (hanging bulbs) get a fair sampling
        // rate instead of their tiny global power share. K samples are
        // averaged; variance of the direct estimate drops ~K×.
        if (model.numLightObjects > 0 && model.lightObjPower &&
            model.lightObjCentroid &&
            model.numLightTriangles > 0 && model.lightTriangles) {
            // K=4 NEE samples: keep render time at the ~470s baseline; heavier denoising
            // happens CPU-side (free) to make up for the extra variance
            const int K = (surface.roughness < 0.3f) ? 1 : min(8, max(1, args.neeK));
            float3 sumContrib = make_float3(0.0f, 0.0f, 0.0f);
            for (int k = 0; k < K; ++k) {
                float totalW = 0.0f;
                for (int o = 0; o < model.numLightObjects; ++o) {
                    float3 dc = surface.position - model.lightObjCentroid[o];
                    totalW += model.lightObjPower[o] /
                              (dot(dc, dc) + fmaxf(model.lightObjArea[o], 1e-4f));
                }
                if (!(totalW > 0.0f)) break;
                float uo = curand_uniform(&st.rng) * totalW;
                float acc = 0.0f;
                int obj = model.numLightObjects - 1;
                float pPick = 1e-6f;
                for (int o = 0; o < model.numLightObjects; ++o) {
                    float3 dc = surface.position - model.lightObjCentroid[o];
                    float wi = model.lightObjPower[o] /
                               (dot(dc, dc) + fmaxf(model.lightObjArea[o], 1e-4f));
                    acc += wi;
                    if (acc >= uo) { obj = o; pPick = wi / totalW; break; }
                }
                pPick = fmaxf(pPick, 1e-6f);
                float objArea = fmaxf(model.lightObjArea[obj], 1e-8f);
                // pdf of the sampled POINT on the object = P(obj) / Area(obj)
                float ptPdf = pPick / objArea;

                int first = model.lightObjFirst[obj];
                int last  = model.lightObjFirst[obj + 1];
                // In-object area prefix: P[k]-base = cumulative area up to k
                float base = model.lightTriPrefix[first] - model.lightTriangles[first].area;
                float a = curand_uniform(&st.rng) * objArea;
                int tlo = first, thi = last - 1;
                while (tlo < thi) {
                    int mid = (tlo + thi) / 2;
                    if (model.lightTriPrefix[mid] - base < a) tlo = mid + 1;
                    else thi = mid;
                }
                int li = tlo;
                const GPULightTri& lt = model.lightTriangles[li];

                float ru = curand_uniform(&st.rng), rv = curand_uniform(&st.rng);
                if (ru + rv > 1.0f) { ru = 1.0f - ru; rv = 1.0f - rv; }
                float rw = 1.0f - ru - rv;
                float3 lightPos = rw * lt.v0 + ru * lt.v1 + rv * lt.v2;

                float3 toLight = lightPos - surface.position;
                float distSq = dot(toLight, toLight);
                float dist = sqrtf(distSq);
                float3 wi = toLight / dist;

                if (dist <= 1e-6f) continue;
                float NdotL = fmaxf(0.0f, dot(surface.surfaceNormal, wi));
                if (NdotL <= 0.0f) continue;
                bool occluded;
                float3 shadowTint = gpu_transparentShadow(
                    surface.position + wi * EPS_ZERO, wi, dist - 1e-4f, model, occluded);
                if (occluded) continue;
                // Light emits from both sides — use |cos(θ)| for the PDF
                float3 e1l = lt.v1 - lt.v0, e2l = lt.v2 - lt.v0;
                float3 ln = normalize(cross(e1l, e2l));
                float cosLight = fabsf(dot(wi, ln));
                float3 bsdfVal = gpu_evalBSDF(-ray.direction, wi, surface);
                if (isnan(bsdfVal.x) || isinf(bsdfVal.x)) bsdfVal = make_float3(0,0,0);
                float pdfSA = ptPdf * distSq / max(cosLight, 1e-8f);
                if (!(pdfSA > 0.0f) || dot(bsdfVal, bsdfVal) <= 1e-16f) continue;
                float bsdfPdf = gpu_pdfBSDF(-ray.direction, wi, surface);
                float w_nee = 1.0f;
                if (bsdfPdf > 1e-8f) {
                    float pn2 = pdfSA * pdfSA;
                    float pb2 = bsdfPdf * bsdfPdf;
                    w_nee = pn2 / (pn2 + pb2 + 1e-16f);
                }
                // This sample covers lt's EMITTER MATERIAL for the MIS rule
                if (st.misSampledCnt < 8) {
                    st.misSampledMat[st.misSampledCnt] = lt.matId;
                    st.misSampledW[st.misSampledCnt] = w_nee;
                    st.misSampledCnt++;
                }
                // Photon travels from the emitter toward the shading point:
                // that is -wi (wi points at the light). The lobe must weight
                // the photon's travel direction. Light radiance carries the
                // per-render emission scale (the load-time bake stays raw).
                const float3 ltEm = lt.emission * args.emissionScale;
                float3 contrib = ltEm * bsdfVal * NdotL / pdfSA * w_nee
                               * gpu_emissionLobe(model.materials, lt.matId, -wi, ln)
                               * shadowTint;
                sumContrib += throughput * contrib / float(K);
            }
            // Clamp the SUMMED contribution once (per-sample clamping would
            // under-count bright sources K times as often)
            float lum = dot(sumContrib, make_float3(0.299f, 0.587f, 0.114f));
            const float NEE_CLAMP = (surface.roughness < 0.3f) ? 5.0f : 15.0f;
            if (lum > NEE_CLAMP) {
                // Soft knee: keep 35% of the excess so lit-side peaks near
                // geometry survive
                float nl = NEE_CLAMP + (lum - NEE_CLAMP) * 0.35f;
                sumContrib = sumContrib * (nl / lum);
            }
            sumContrib = sumContrib * indS;
            radiance += sumContrib;
            if (depth == 0)
                gpu_decomposeLobe(surface.baseColor, sumContrib,
                                  split.radiance_Dd, split.radiance_Ds);
            else
                gpu_decomposeLobe(surface.baseColor, sumContrib,
                                  split.radiance_Id, split.radiance_Is);
        }

        // ---- Sample skybox as environment light (importance-sampled) ----
        if (skyTex) {
            float3 skyDir;
            float pdf = 0.0f;
            bool sampled = false;

            if (args.skyCDF && args.skyWidth > 0 && args.skyHeight > 0 && args.skyCDFSum > 0.0f) {
                // ---- Importance sampling via CDF ----
                float u = curand_uniform(&st.rng);
                // CDF is an exclusive prefix over texels (cdf[0]=0,
                // cdf[j] = P(texel < j)): texel t is drawn when
                // u ∈ [cdf[t], cdf[t+1]). Search [1, n] for the first
                // entry above u; the sampled texel is that index minus 1
                // (searching [0, n) instead shifted the distribution by
                // one texel and made the last texel unreachable).
                int lo = 1, hi = args.skyWidth * args.skyHeight;
                while (lo < hi) {
                    int mid = (lo + hi) / 2;
                    if (args.skyCDF[mid] <= u) lo = mid + 1;
                    else hi = mid;
                }
                int idx = lo;
                int px = (idx - 1) % args.skyWidth;
                int py = (idx - 1) / args.skyWidth;

                // Convert pixel to direction (equirectangular)
                float u_ = (px + 0.5f) / args.skyWidth;
                float v_ = (py + 0.5f) / args.skyHeight;
                skyDir = sphereToDir(make_float2(u_, v_));

                // Compute PDF: solid angle conversion
                int nPixels = args.skyWidth * args.skyHeight;
                float cdfLo = (idx > 0) ? args.skyCDF[idx - 1] : 0.0f;
                float cdfHi = args.skyCDF[idx];
                float pixelPdf = (cdfHi - cdfLo);  // probability mass for this pixel

                float theta = v_ * PI;
                float sinTheta = sinf(theta);
                if (sinTheta > 1e-6f) {
                    // Convert pixel PDF to solid angle PDF
                    // pdf = pixelPdf * nPixels / (2 * PI² * sin(theta))
                    pdf = pixelPdf * nPixels / (2.0f * PI * PI * sinTheta);
                    sampled = true;
                }
            } else {
                // ---- Fallback: uniform cosine-weighted hemisphere ----
                float u1 = curand_uniform(&st.rng);
                float u2 = curand_uniform(&st.rng);
                float r = sqrtf(u1);
                float phi = 2.0f * PI * u2;
                float3 skyLocal = make_float3(r*cosf(phi), r*sinf(phi), sqrtf(fmaxf(0.0f,1.0f-u1)));
                float3 N = surface.surfaceNormal;
                float3 ref = (fabsf(N.x) > 0.9f) ? make_float3(0,1,0) : make_float3(1,0,0);
                float3 T = normalize(cross(N, ref));
                float3 B = normalize(cross(N, T));
                skyDir = normalize(skyLocal.x*T + skyLocal.y*B + skyLocal.z*N);
                pdf = skyLocal.z * INV_PI;
                sampled = true;
            }

            if (sampled && pdf > 1e-8f) {
                float NdotL = fmaxf(0.0f, dot(surface.surfaceNormal, skyDir));
                if (NdotL > 0.0f) {
                    bool skyBlocked;
                    float3 skyTint = gpu_transparentShadow(
                        surface.position + skyDir * EPS_ZERO, skyDir,
                        1e30f, model, skyBlocked);
                    if (!skyBlocked) {
                        float theta = acosf(fmaxf(-1.0f, fminf(1.0f, skyDir.y)));
                        float az = atan2f(-skyDir.x, skyDir.z);  // reference convention
                        float u_ = az * INV_TWO_PI + 0.5f;
                        float v_ = theta * INV_PI;
                        float4 tex = tex2D<float4>(skyTex, u_, v_);
                        float3 skyRad = make_float3(tex.x, tex.y, tex.z);
                        float3 bsdfVal = gpu_evalBSDF(-ray.direction, skyDir, surface);
                        if (dot(bsdfVal, bsdfVal) > 1e-16f) {
                            // Balance weight for THIS NEE sample at its
                            // own direction; the escape estimator at the
                            // next vertex carries the complementary
                            // weight (see the sky-escape block).
                            float bsdfPdfSky = gpu_pdfBSDF(-ray.direction, skyDir, surface);
                            float w_nee = 1.0f;
                            if (bsdfPdfSky > 1e-8f) {
                                float pn2 = pdf * pdf;
                                float pb2 = bsdfPdfSky * bsdfPdfSky;
                                w_nee = pn2 / (pn2 + pb2 + 1e-16f);
                            }
                            float3 contrib = skyRad * bsdfVal * NdotL / pdf
                                           * w_nee * skyTint;
                            // Sun-cone de-duplication: with a delta sun in
                            // the scene, the HDRI cone is a visual glow
                            // only, not a second light source.
                            if (args.sunConeKill)
                                contrib = contrib * gpu_sunConeMask(skyDir, args.sunDir, sunConeHalfRad);
                            // Clamp FINAL contribution (throughput * contrib)
                            float3 finalSky = throughput * contrib;
                            float lum = dot(finalSky, make_float3(0.299f,0.587f,0.114f));
                            if (lum > 50.0f) finalSky = finalSky * (50.0f/lum);
                            radiance += finalSky;
                            if (depth == 0)
                                gpu_decomposeLobe(surface.baseColor, finalSky,
                                                  split.radiance_Dd, split.radiance_Ds);
                            else
                                gpu_decomposeLobe(surface.baseColor, finalSky,
                                                  split.radiance_Id, split.radiance_Is);
                        }
                    }
                }
            }
        }
    }

    // ---- Directional sun (delta light, NEE-only) ----
    // A delta light cannot be sampled by the BSDF, so the estimate is
    // deterministic (no MIS, no PDF): radiance * bsdf * NdotL * T.
    if (args.hasSun && !excludeDirect) {
        // Cone jitter around sunDir (RENDER_SUN_SOFT, half-angle degrees,
        // default 1.0): physically the sun's disc, artistically a
        // soft-shadow knob. Radiance constant over the cone = unbiased.
        float softDeg = args.sunSoftDeg;
        float3 sw = args.sunDir;
        float3 su = normalize(cross(fabsf(sw.y) < 0.99f ? make_float3(0,1,0) : make_float3(1,0,0), sw));
        float3 sv = cross(sw, su);
        float3 wi = sw;
        if (softDeg > 0.0f) {
            float u1 = curand_uniform(&st.rng), u2 = curand_uniform(&st.rng);
            float thetaMax = softDeg * (float(M_PI) / 180.0f);
            float cosT = 1.0f - u1 * (1.0f - cosf(thetaMax));
            float sinT = sqrtf(fmaxf(0.0f, 1.0f - cosT * cosT));
            float phi = 2.0f * float(M_PI) * u2;
            wi = normalize(cosT * sw + sinT * (cosf(phi) * su + sinf(phi) * sv));
        }
        float NdotL = dot(surface.surfaceNormal, wi);
        if (NdotL > 0.0f) {
            bool blocked;
            float3 tint = gpu_transparentShadow(
                surface.position + wi * EPS_ZERO, wi, 1e30f, model, blocked);
            if (!blocked) {
                float3 bsdfVal = gpu_evalBSDF(-ray.direction, wi, surface);
                if (dot(bsdfVal, bsdfVal) > 1e-16f) {
                    float3 contrib = args.sunRadiance * bsdfVal * NdotL * tint * indS;
                    if (depth == 0) {
                        gpu_decomposeLobe(surface.baseColor, contrib,
                                          split.radiance_Dd, split.radiance_Ds);
                        // P1 sun matte: first-hit direct-sun luminance. The
                        // cone-jittered occlusion averages over samples, so
                        // soft shadows show up as a smooth 0..1 mask.
                        split.sunMask += dot(throughput * contrib,
                                             make_float3(0.299f, 0.587f, 0.114f));
                    } else {
                        gpu_decomposeLobe(st.prevBaseColor, contrib,
                                          split.radiance_Id, split.radiance_Is);
                    }
                    radiance += throughput * contrib;
                }
            }
        }
    }

    // BSDF sampling for next bounce (with medium IOR for nested glass)
    float3 outDir;
    float bsdfPdf;
    float medEta = st.medStack.topEta();

    // ---- Path regularization (matching CPU render.cpp) ----
    // Gradually increase effective roughness for deep bounces to keep
    // paths alive on highly specular surfaces (e.g. metals). The CPU
    // uses: regRoughness = max(roughness, 0.01 * (depth + 1)) and samples
    // from that regularized distribution while still evaluating the true
    // BSDF for throughput. We copy the surface and adjust roughness
    // ONLY for the sampling call (eval uses the true surface below).
    HitInfo sampleSurface = surface;
    if (!gpu_isDielectric(surface)) {
        float regRough = fmaxf(surface.roughness, 0.01f * float(depth + 1));
        sampleSurface.roughness = regRough;
    }

    float3 bsdfVal = gpu_sampleBSDF(
        curand_uniform(&st.rng), curand_uniform(&st.rng),
        -ray.direction, sampleSurface, outDir, bsdfPdf, medEta);

    if (bsdfPdf > 1e-8f && dot(bsdfVal, bsdfVal) > 1e-16f) {
        // The next bounce's contributions are classified against THIS
        // vertex's material (mirrors CPU decomposeRadiance)
        st.prevBaseColor = surface.baseColor;
        if (gpu_isDielectric(surface)) {
            float3 newThroughput = throughput * bsdfVal / bsdfPdf;
            float tLum = newThroughput.x * 0.299f + newThroughput.y * 0.587f + newThroughput.z * 0.114f;
            if (tLum > 80.0f) {
                // Soft knee keeps 35% of the excess
                float nl = 80.0f + (tLum - 80.0f) * 0.35f;
                newThroughput = newThroughput * (nl / tLum);
            }
            if (!isnan(newThroughput.x) && !isinf(newThroughput.x) && newThroughput.x > -1e10f && newThroughput.x < 1e10f) {
                throughput = newThroughput;
                // Update medium stack ONLY if the ray transmitted (refracted)
                // through the surface. If it reflected (Fresnel/TIR), the
                // ray stays in the same medium — no push/pop needed.
                // faceN (= surfaceNormal) faces the incident side; a
                // transmitted ray goes to the opposite side, so
                // dot(outDir, faceN) < 0 means transmitted.
                if (fabsf(surface.eta - 1.0f) > 0.01f &&
                    dot(outDir, surface.surfaceNormal) < 0.0f) {
                    if (surface.entering) {
                        // Absorption coefficient from the authored color:
                        // sigma_c = −ln(color_c) × scale (per meter), so a
                        // dark red like (0.07,0.003,0.003) drinks green/blue
                        // and glows ruby over centimetres. Pure-black
                        // factors carry no hue info — treat as clear water
                        // instead of an opaque void.
                        float3 c = surface.baseColor;
                        float lumC = dot(c, make_float3(0.299f,0.587f,0.114f));
                        float3 sig;
                        if (lumC < 0.004f) {
                            sig = make_float3(0.05f, 0.05f, 0.05f);
                        } else {
                            const float ABS_SCALE = 6.0f;
                            sig = make_float3(
                                -logf(fmaxf(c.x, 1e-3f)) * ABS_SCALE,
                                -logf(fmaxf(c.y, 1e-3f)) * ABS_SCALE,
                                -logf(fmaxf(c.z, 1e-3f)) * ABS_SCALE);
                            sig.x = fminf(sig.x, 60.0f);
                            sig.y = fminf(sig.y, 60.0f);
                            sig.z = fminf(sig.z, 60.0f);
                        }
                        st.medStack.push(surface.eta, sig);
                    } else {
                        st.medStack.pop();
                    }
                }
                st.prevBsdfPdf = bsdfPdf;
                st.prevBsdfDielectric = true;
                st.throughput = throughput;
                st.radiance = radiance;
                st.ray = Ray(surface.position + outDir * EPS_ZERO, outDir);
                st.depth = depth + 1;
                return true;
            }
        } else {
            float NdotL = fmaxf(0.0f, dot(surface.surfaceNormal, outDir));
            if (NdotL > 0.0f && dot(outDir, outDir) > 1e-16f) {
                // MIS balance heuristic weight for the BSDF bounce:
                // w_bsdf = bsdfPdf² / (bsdfPdf² + pdfSA²)
                // When the BSDF direction happens to hit the light next bounce,
                // this weight reduces the implicit contribution to balance NEE.
                // For the non-light-hit case (most bounces), the weight is ~1.0
                // since the NEE PDF for a random BSDF direction is near-zero.
                // For simplicity, we approximate: the NEE PDF for a random direction
                // is the average light sampling PDF (~pdfSA_avg), but since we don't
                // have it here, we just use NdotL weight.
                throughput = throughput * bsdfVal * NdotL / bsdfPdf;
                if (isnan(throughput.x) || isnan(throughput.y) || isnan(throughput.z) ||
                    isinf(throughput.x) || isinf(throughput.y) || isinf(throughput.z) ||
                    throughput.x < -1e10f || throughput.y < -1e10f || throughput.z < -1e10f ||
                    throughput.x > 1e10f || throughput.y > 1e10f || throughput.z > 1e10f) {
                    st.throughput = throughput; st.radiance = radiance;
                    return false;
                }
                // Throughput clamp: match CPU (500) to avoid suppressing indirect light.
                float tLum2 = throughput.x * 0.299f + throughput.y * 0.587f + throughput.z * 0.114f;
                if (tLum2 > 500.0f) throughput = throughput * (500.0f / tLum2);
                st.prevBsdfPdf = bsdfPdf;
                st.prevBsdfDielectric = false;
                st.throughput = throughput;
                st.radiance = radiance;
                st.ray = Ray(surface.position + outDir * EPS_ZERO, outDir);
                st.depth = depth + 1;
                return true;
            }
        }
    }
    st.throughput = throughput; st.radiance = radiance;
    return false;  // path ends (absorption or invalid bounce)
}

__device__ float3 tracePath(
    uint32_t&      seed,
    const Ray&     inRay,
    const GPUModelData& model,
    cudaTextureObject_t skyTex,
    const GPURenderArgs& args,
    bool           excludeDirect,
    PerPixelData&  split)
{
    GPUPathState st;
    gpu_pathInit(st, seed, inRay);
    split = PerPixelData{};

    while (st.depth < args.maxDepth) {
        // Trace ray
        HitRecord hit = traceRay(st.ray, model);
        if (!gpu_shadeBounce(st, split, hit, model, skyTex, args, excludeDirect))
            break;
    }

    // Final per-sample radiance clamp (firefly suppression)
    // Match CPU: no aggressive clamp; use 100 as safety net.
    float finalLum = st.radiance.x * 0.299f + st.radiance.y * 0.587f + st.radiance.z * 0.114f;
    if (finalLum > 100.0f)
        st.radiance = st.radiance * (100.0f / finalLum);

    return st.radiance;
}

// =========================================================================
// Main Render Kernel (tile-based, full path tracing)
// =========================================================================
// launch_bounds: the kernel naturally wants ~210 registers, which allows only
// ONE 256-thread block per SM (12.5% occupancy on a 2048-thread V100) — and
// this kernel is memory-latency bound (BVH traversal + 3.8 GB texture array),
// so more resident warps hide far better. Asking for 2 blocks/SM caps registers
// at 128; ptxas then spills ~1 KB to local, but measured on the exterior
// (480x270@256spp) it is a net 1.19x speedup with BYTE-IDENTICAL output.
// TILE_W*TILE_H keeps the bound honest if the tile size ever changes.
__global__ void __launch_bounds__(TILE_W * TILE_H, 2) renderKernel(
    GPURenderArgs      args,
    GPUModelData       model,
    cudaTextureObject_t skyTex,
    float4*            outputColor,
    float4*            outputDirectDiff,
    float4*            outputDirectSpec,
    float4*            outputIndirectDiff,
    float4*            outputIndirectSpec,
    float4*            outputEmission,
    float*             outputVarDd,
    float*             outputVarDs,
    float*             outputVarId,
    float*             outputVarIs,
    float4*            outputBaseColor,
    float4*            outputNormal,
    float4*            outputPosition,
    float4*            outputGeomNormal,
    float4*            outputMaterial,
    unsigned int*      progressCounter,
    float*             outputSunMask)
{
    const int x = blockIdx.x * blockDim.x + threadIdx.x;
    const int y = blockIdx.y * blockDim.y + threadIdx.y;
    if (x >= args.width || y >= args.height) return;
    const size_t idx = size_t(y) * args.width + x;

    uint32_t baseSeed = args.baseSeed + idx * 2654435761u;

    // C: per-pixel Cranley-Patterson rotation for low-discrepancy primary-ray
    // jitter — spreads the SPP samples evenly per pixel while keeping pixels
    // decorrelated, cutting low-SPP visual noise with zero extra cost.
    uint32_t hsh = baseSeed * 0x9E3779B9u;
    hsh = (hsh >> 16) ^ hsh;
    const float pxU = (hsh & 0xFFFFu) * (1.0f / 65536.0f);
    hsh = hsh * 0x9E3779B9u; hsh = (hsh >> 16) ^ hsh;
    const float pxV = (hsh & 0xFFFFu) * (1.0f / 65536.0f);
    // Irrational-per-dimension increments (R2 sequence) — uniform 2D spread
    constexpr float R2_X = 0.7548776662466927f;
    constexpr float R2_Y = 0.5698402909980532f;

    float3 acc_Dd = {0,0,0}, acc_Ds = {0,0,0}, acc_Id = {0,0,0}, acc_Is = {0,0,0}, acc_em = {0,0,0};
    float  acc_sun = 0.0f;   // P1: first-hit direct-sun luminance (grade matte)
    // Welford's online variance: track sum of squares for each channel
    float3 acc_Dd2 = {0,0,0}, acc_Ds2 = {0,0,0}, acc_Id2 = {0,0,0}, acc_Is2 = {0,0,0};
    // First-hit G-buffer averaged over AA-jittered samples (denoiser guidance)
    float3 acc_base = {0,0,0}, acc_nrm = {0,0,0};
    float3 acc_pos = {0,0,0}, acc_geomNrm = {0,0,0};
    float4 acc_mat = {0,0,0,0}; // roughness, metallic, specular, opacity

    // Adaptive per-pixel sampling (自适应逐像素): with args.adaptiveStats
    // set, `spp` is only the budget — a pixel stops early once every AOV
    // channel's relative standard error drops below args.adaptiveThresh.
    // Sky/flat pixels converge at the min; shadow/GM pixels keep sampling.
    const bool adaptive = (args.adaptiveStats != nullptr) &&
                          (args.adaptiveMin > 1) && (args.spp > args.adaptiveMin);
    const int adaptMin = adaptive ? args.adaptiveMin : args.spp;
    const int adaptCheck = adaptive ? max(2, args.adaptiveCheck) : args.spp;
    const float3 lumW3 = make_float3(0.299f, 0.587f, 0.114f);
    int n = 0;

    for (int s = 0; s < args.spp; ++s) {
        uint32_t useed = baseSeed + s * 0xABCD;
        curandState rng;
        curand_init(useed, 0, 0, &rng);
        // Low-discrepancy jitter: R2 sequence + per-pixel rotation
        float jitU = pxU + float(s) * R2_X; jitU -= floorf(jitU);
        float jitV = pxV + float(s) * R2_Y; jitV -= floorf(jitV);
        float lensU = curand_uniform(&rng) * 2.0f - 1.0f;
        float lensV = curand_uniform(&rng) * 2.0f - 1.0f;
        Ray ray = args.camera.generateRay(
            float(x), float(y), jitU, jitV, lensU, lensV,
            make_float2(float(args.width), float(args.height)));
        PerPixelData split;
        float3 rad = tracePath(useed, ray, model, skyTex, args, false, split);
        // NaN/Inf guard on accumulators
        auto clampF = [](float x) { return (isnan(x)||isinf(x)||x<-1e10f||x>1e10f) ? 0.0f : x; };
        split.radiance_Dd.x = clampF(split.radiance_Dd.x); split.radiance_Dd.y = clampF(split.radiance_Dd.y); split.radiance_Dd.z = clampF(split.radiance_Dd.z);
        split.radiance_Ds.x = clampF(split.radiance_Ds.x); split.radiance_Ds.y = clampF(split.radiance_Ds.y); split.radiance_Ds.z = clampF(split.radiance_Ds.z);
        split.radiance_Id.x = clampF(split.radiance_Id.x); split.radiance_Id.y = clampF(split.radiance_Id.y); split.radiance_Id.z = clampF(split.radiance_Id.z);
        split.radiance_Is.x = clampF(split.radiance_Is.x); split.radiance_Is.y = clampF(split.radiance_Is.y); split.radiance_Is.z = clampF(split.radiance_Is.z);
        split.emission.x = clampF(split.emission.x); split.emission.y = clampF(split.emission.y); split.emission.z = clampF(split.emission.z);
        acc_Dd += split.radiance_Dd;
        acc_Ds += split.radiance_Ds;
        acc_Id += split.radiance_Id;
        acc_Is += split.radiance_Is;
        acc_em += split.emission;
        acc_sun += split.sunMask;
        acc_base += split.baseColor;
        acc_nrm += split.normal;
        acc_pos += split.position;
        acc_geomNrm += split.shapeNormal;
        acc_mat.x += split.roughness;
        acc_mat.y += split.metallic;
        acc_mat.z += split.specular;
        acc_mat.w += split.opacity;
        // Accumulate squares for variance estimation (host-side)
        acc_Dd2 += split.radiance_Dd * split.radiance_Dd;
        acc_Ds2 += split.radiance_Ds * split.radiance_Ds;
        acc_Id2 += split.radiance_Id * split.radiance_Id;
        acc_Is2 += split.radiance_Is * split.radiance_Is;
        ++n;
        // Convergence check: relative standard error of the running mean per
        // AOV luminance. The 1e-3 absolute floor lets truly-black pixels
        // converge instead of grinding to the budget. Identical formula in
        // the wavefront reduce kernel (wfAccumKernel) — keep in sync.
        if (adaptive && n >= adaptMin && (n & (adaptCheck - 1)) == 0 && n < args.spp) {
            const float inv = 1.0f / float(n);
            auto rseOf = [&](const float3& sum, const float3& sum2) -> float {
                float lm  = dot(sum  * inv, lumW3);
                float m2  = dot(sum2 * inv, lumW3);
                float var = fmaxf(0.0f, m2 - lm * lm);
                return sqrtf(var * inv) / (fabsf(lm) + 1e-3f);
            };
            float r = fmaxf(fmaxf(rseOf(acc_Dd, acc_Dd2), rseOf(acc_Ds, acc_Ds2)),
                            fmaxf(rseOf(acc_Id, acc_Id2), rseOf(acc_Is, acc_Is2)));
            if (r < args.adaptiveThresh) break;
        }
    }
    if (args.adaptiveStats) {
        atomicAdd(&args.adaptiveStats[0], (unsigned long long)n);
        atomicMax(&args.adaptiveStats[2], (unsigned long long)n);
        atomicMin(&args.adaptiveStats[1], (unsigned long long)n);
        if (n < args.spp) atomicAdd(&args.adaptiveStats[3], 1ULL);
    }
    float invSpp = 1.0f / n;
    float3 final = (acc_Dd + acc_Ds + acc_Id + acc_Is + acc_em) * invSpp;

    outputColor[idx] = make_float4(final.x, final.y, final.z, 1.0f);
    if (outputDirectDiff)   outputDirectDiff[idx]   = make_float4(acc_Dd.x*invSpp, acc_Dd.y*invSpp, acc_Dd.z*invSpp, 1.0f);
    if (outputDirectSpec)   outputDirectSpec[idx]   = make_float4(acc_Ds.x*invSpp, acc_Ds.y*invSpp, acc_Ds.z*invSpp, 1.0f);
    if (outputIndirectDiff) outputIndirectDiff[idx] = make_float4(acc_Id.x*invSpp, acc_Id.y*invSpp, acc_Id.z*invSpp, 1.0f);
    if (outputIndirectSpec) outputIndirectSpec[idx] = make_float4(acc_Is.x*invSpp, acc_Is.y*invSpp, acc_Is.z*invSpp, 1.0f);
    if (outputEmission)     outputEmission[idx]     = make_float4(acc_em.x*invSpp, acc_em.y*invSpp, acc_em.z*invSpp, 1.0f);

    if (outputBaseColor)    outputBaseColor[idx]    = make_float4(acc_base.x*invSpp, acc_base.y*invSpp, acc_base.z*invSpp, 1.0f);
    if (outputNormal) {
        float3 n = acc_nrm * invSpp;
        float nl = sqrtf(dot(n, n));
        n = (nl > 1e-6f) ? n * (1.0f / nl) : make_float3(0.0f, 0.0f, 0.0f);
        outputNormal[idx] = make_float4(n.x, n.y, n.z, 1.0f);
    }
    if (outputPosition) {
        float3 p = acc_pos * invSpp;
        outputPosition[idx] = make_float4(p.x, p.y, p.z, 0.0f);
    }
    if (outputGeomNormal) {
        float3 n = acc_geomNrm * invSpp;
        float nl = sqrtf(dot(n, n));
        n = (nl > 1e-6f) ? n * (1.0f / nl) : make_float3(0.0f, 0.0f, 0.0f);
        outputGeomNormal[idx] = make_float4(n.x, n.y, n.z, 0.0f);
    }
    if (outputSunMask) {
        float sm = acc_sun * invSpp;
        outputSunMask[idx] = (isnan(sm) || isinf(sm) || sm < 0.0f) ? 0.0f : sm;
    }
    if (outputMaterial) {
        outputMaterial[idx] = make_float4(acc_mat.x*invSpp, acc_mat.y*invSpp,
                                          acc_mat.z*invSpp, acc_mat.w*invSpp);
    }

    // Output variance: Var = E[X^2] - E[X]^2 (scalar per channel, averaged over RGB)
    if (outputVarDd) {
        float3 mean_Dd = acc_Dd * invSpp;
        float3 var_Dd = acc_Dd2 * invSpp - mean_Dd * mean_Dd;
        outputVarDd[idx] = fmaxf(0.0f, (var_Dd.x + var_Dd.y + var_Dd.z) / 3.0f);
    }
    if (outputVarDs) {
        float3 mean_Ds = acc_Ds * invSpp;
        float3 var_Ds = acc_Ds2 * invSpp - mean_Ds * mean_Ds;
        outputVarDs[idx] = fmaxf(0.0f, (var_Ds.x + var_Ds.y + var_Ds.z) / 3.0f);
    }
    if (outputVarId) {
        float3 mean_Id = acc_Id * invSpp;
        float3 var_Id = acc_Id2 * invSpp - mean_Id * mean_Id;
        outputVarId[idx] = fmaxf(0.0f, (var_Id.x + var_Id.y + var_Id.z) / 3.0f);
    }
    if (outputVarIs) {
        float3 mean_Is = acc_Is * invSpp;
        float3 var_Is = acc_Is2 * invSpp - mean_Is * mean_Is;
        outputVarIs[idx] = fmaxf(0.0f, (var_Is.x + var_Is.y + var_Is.z) / 3.0f);
    }

    if (progressCounter)
        atomicAdd(progressCounter, 1u);
}

// =========================================================================
// Host-side: launch GPU render and read back results
// =========================================================================
void renderGPU(GPURenderArgs args, GPUModelData model,
               cudaTextureObject_t skyTex,
               PerPixelData* output, int pixelCount) {
    if (args.neeK <= 0) args.neeK = defaultNeeK();
    fprintf(stderr, "renderGPU: entered, pixelCount=%d\n", pixelCount); fflush(stderr);
    fprintf(stderr, "renderGPU: model.numFaces=%d numBVH=%d\n",
            model.numFaces, model.numBVHNodes); fflush(stderr);

    // Allocate GPU output buffers
    float4 *gpuColor = nullptr, *gpuDD = nullptr, *gpuDS = nullptr, *gpuID = nullptr, *gpuIS = nullptr, *gpuEm = nullptr;
    float4 *gpuBase = nullptr, *gpuNrm = nullptr;
    float4 *gpuPos = nullptr, *gpuGeomNrm = nullptr, *gpuMat = nullptr;
    float *gpuVarDd = nullptr, *gpuVarDs = nullptr, *gpuVarId = nullptr, *gpuVarIs = nullptr;
    auto cu = [](cudaError_t e) { return e; };

    fprintf(stderr, "renderGPU: cudaMalloc outputs...\n"); fflush(stderr);
    cudaError_t e;
    e = cudaMalloc(&gpuColor, pixelCount * sizeof(float4));
    fprintf(stderr, "  gpuColor: %s\n", cudaGetErrorString(e)); fflush(stderr);
    e = cudaMalloc(&gpuDD, pixelCount * sizeof(float4));
    fprintf(stderr, "  gpuDD: %s\n", cudaGetErrorString(e)); fflush(stderr);
    e = cudaMalloc(&gpuDS, pixelCount * sizeof(float4));
    fprintf(stderr, "  gpuDS: %s\n", cudaGetErrorString(e)); fflush(stderr);
    e = cudaMalloc(&gpuID, pixelCount * sizeof(float4));
    fprintf(stderr, "  gpuID: %s\n", cudaGetErrorString(e)); fflush(stderr);
    e = cudaMalloc(&gpuIS, pixelCount * sizeof(float4));
    fprintf(stderr, "  gpuIS: %s\n", cudaGetErrorString(e)); fflush(stderr);
    e = cudaMalloc(&gpuEm, pixelCount * sizeof(float4));
    fprintf(stderr, "  gpuEm: %s\n", cudaGetErrorString(e)); fflush(stderr);
    // Variance buffers (for denoiser)
    e = cudaMalloc(&gpuVarDd, pixelCount * sizeof(float));
    e = cudaMalloc(&gpuVarDs, pixelCount * sizeof(float));
    e = cudaMalloc(&gpuVarId, pixelCount * sizeof(float));
    e = cudaMalloc(&gpuVarIs, pixelCount * sizeof(float));
    // First-hit G-buffer for edge-aware denoising
    e = cudaMalloc(&gpuBase, pixelCount * sizeof(float4));
    e = cudaMalloc(&gpuNrm, pixelCount * sizeof(float4));
    e = cudaMalloc(&gpuPos, pixelCount * sizeof(float4));
    e = cudaMalloc(&gpuGeomNrm, pixelCount * sizeof(float4));
    e = cudaMalloc(&gpuMat, pixelCount * sizeof(float4));
    // P1: first-hit direct-sun grade matte
    float* gpuSunMask = nullptr;
    e = cudaMalloc(&gpuSunMask, pixelCount * sizeof(float));

    fprintf(stderr, "renderGPU: launching kernel...\n"); fflush(stderr);
    // Per-thread stack headroom: the path-tracing frame (locals + inlined
    // texture decoders) is close to the 1 KB default, and exceeding it faults
    // as "Stack overflow" inside unrelated inlined code. 4 KB costs nothing
    // measurable and removes the cliff.
    cudaDeviceSetLimit(cudaLimitStackSize, 4096);

    // Adaptive per-pixel sampling counters ([total, min, max, earlyStopPx]).
    sanitizeAdaptiveArgs(args);
    unsigned long long* gpuAdaptStats = nullptr;
    if (args.adaptiveMin > 1) {
        cudaMalloc(&gpuAdaptStats, 4 * sizeof(unsigned long long));
        cudaMemset(gpuAdaptStats, 0, 4 * sizeof(unsigned long long));
        const unsigned long long ullMax = ~0ULL;
        cudaMemcpy(gpuAdaptStats + 1, &ullMax, sizeof(ullMax), cudaMemcpyHostToDevice);
        args.adaptiveStats = gpuAdaptStats;
        fprintf(stderr, "  adaptive sampling: min=%d check=%d thresh=%.4f budget=%d\n",
                args.adaptiveMin, args.adaptiveCheck, args.adaptiveThresh, args.spp);
        fflush(stderr);
    }

    // Launch kernel
    dim3 blockDim(TILE_W, TILE_H);
    dim3 gridDim((args.width + TILE_W - 1) / TILE_W,
                 (args.height + TILE_H - 1) / TILE_H);
    fprintf(stderr, "  grid=%dx%d block=%dx%d\n", gridDim.x, gridDim.y, blockDim.x, blockDim.y); fflush(stderr);
    // Clear any prior CUDA errors, then launch
    cudaGetLastError();

    // Device-side progress counter (one increment per completed pixel).
    // The host polls it on a side stream while the kernel runs.
    unsigned int* gpuProgress = nullptr;
    cudaMalloc(&gpuProgress, sizeof(unsigned int));
    cudaMemset(gpuProgress, 0, sizeof(unsigned int));
    unsigned int* pinnedProgress = nullptr;
    cudaHostAlloc(&pinnedProgress, sizeof(unsigned int), cudaHostAllocMapped);
    cudaStream_t progStream;
    cudaStreamCreateWithFlags(&progStream, cudaStreamNonBlocking);
    *pinnedProgress = 0;

    renderKernel<<<gridDim, blockDim>>>(args, model, skyTex,
                                         gpuColor, gpuDD, gpuDS, gpuID, gpuIS, gpuEm,
                                         gpuVarDd, gpuVarDs, gpuVarId, gpuVarIs,
                                         gpuBase, gpuNrm,
                                         gpuPos, gpuGeomNrm, gpuMat,
                                         gpuProgress, gpuSunMask);

    // Poll for kernel completion while drawing a progress bar, instead of a
    // blocking cudaDeviceSynchronize. Reads run on a non-blocking stream so
    // they never serialize with the kernel on the default stream.
    ProgressBar bar(long(args.width) * args.height);
    while (cudaStreamQuery(0) == cudaErrorNotReady) {
        cudaMemcpyAsync(pinnedProgress, gpuProgress, sizeof(unsigned int),
                        cudaMemcpyDeviceToHost, progStream);
        cudaStreamSynchronize(progStream);
        bar.update(long(*pinnedProgress));
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    cudaMemcpyAsync(pinnedProgress, gpuProgress, sizeof(unsigned int),
                    cudaMemcpyDeviceToHost, progStream);
    cudaStreamSynchronize(progStream);
    bar.update(long(args.width) * args.height);

    cudaStreamDestroy(progStream);
    cudaFreeHost(pinnedProgress);
    cudaFree(gpuProgress);

    fprintf(stderr, "  sync done\n"); fflush(stderr);
    cudaError_t err = cudaGetLastError();
    if (err != cudaSuccess) {
        fprintf(stderr, "  CUDA ERROR after kernel: %s\n", cudaGetErrorString(err)); fflush(stderr);
    }

    // Adaptive sampling statistics (avg/min/max samples + early-stop rate)
    g_lastAvgSpp = double(args.spp);
    if (gpuAdaptStats) {
        unsigned long long st[4] = {0, 0, 0, 0};
        cudaMemcpy(st, gpuAdaptStats, sizeof(st), cudaMemcpyDeviceToHost);
        const double avg = pixelCount > 0 ? double(st[0]) / pixelCount : 0.0;
        g_lastAvgSpp = avg;
        fprintf(stderr, "ADAPTSTAT: avg=%.1f min=%llu max=%llu budget=%d earlyStop=%llu/%d (%.1f%%)\n",
                avg, st[1] == ~0ULL ? 0 : st[1], st[2], args.spp,
                st[3], pixelCount, 100.0 * st[3] / (pixelCount > 0 ? pixelCount : 1));
        fflush(stderr);
        cudaFree(gpuAdaptStats);
    }

    // Read back results
    std::vector<float4> colorBuf(pixelCount);
    std::vector<float4> ddBuf(pixelCount), dsBuf(pixelCount);
    std::vector<float4> idBuf(pixelCount), isBuf(pixelCount);
    std::vector<float4> emBuf(pixelCount);
    std::vector<float> varDdBuf(pixelCount), varDsBuf(pixelCount);
    std::vector<float> varIdBuf(pixelCount), varIsBuf(pixelCount);

    cu(cudaMemcpy(colorBuf.data(), gpuColor, pixelCount * sizeof(float4), cudaMemcpyDeviceToHost));
    cu(cudaMemcpy(ddBuf.data(),    gpuDD,    pixelCount * sizeof(float4), cudaMemcpyDeviceToHost));
    cu(cudaMemcpy(dsBuf.data(),    gpuDS,    pixelCount * sizeof(float4), cudaMemcpyDeviceToHost));
    cu(cudaMemcpy(idBuf.data(),    gpuID,    pixelCount * sizeof(float4), cudaMemcpyDeviceToHost));
    cu(cudaMemcpy(isBuf.data(),    gpuIS,    pixelCount * sizeof(float4), cudaMemcpyDeviceToHost));
    cu(cudaMemcpy(emBuf.data(),    gpuEm,    pixelCount * sizeof(float4), cudaMemcpyDeviceToHost));
    std::vector<float4> baseBuf(pixelCount), nrmBuf(pixelCount);
    std::vector<float4> posBuf(pixelCount), geomNrmBuf(pixelCount), matBuf(pixelCount);
    cu(cudaMemcpy(baseBuf.data(),  gpuBase,  pixelCount * sizeof(float4), cudaMemcpyDeviceToHost));
    cu(cudaMemcpy(nrmBuf.data(),   gpuNrm,   pixelCount * sizeof(float4), cudaMemcpyDeviceToHost));
    cu(cudaMemcpy(posBuf.data(),   gpuPos,   pixelCount * sizeof(float4), cudaMemcpyDeviceToHost));
    cu(cudaMemcpy(geomNrmBuf.data(), gpuGeomNrm, pixelCount * sizeof(float4), cudaMemcpyDeviceToHost));
    cu(cudaMemcpy(matBuf.data(),   gpuMat,   pixelCount * sizeof(float4), cudaMemcpyDeviceToHost));
    std::vector<float> sunMaskBuf(pixelCount);
    cu(cudaMemcpy(sunMaskBuf.data(), gpuSunMask, pixelCount * sizeof(float), cudaMemcpyDeviceToHost));
    // Read back variance buffers (for denoiser)
    cu(cudaMemcpy(varDdBuf.data(), gpuVarDd, pixelCount * sizeof(float), cudaMemcpyDeviceToHost));
    cu(cudaMemcpy(varDsBuf.data(), gpuVarDs, pixelCount * sizeof(float), cudaMemcpyDeviceToHost));
    cu(cudaMemcpy(varIdBuf.data(), gpuVarId, pixelCount * sizeof(float), cudaMemcpyDeviceToHost));
    cu(cudaMemcpy(varIsBuf.data(), gpuVarIs, pixelCount * sizeof(float), cudaMemcpyDeviceToHost));

    // Convert to PerPixelData
    {
        double sDd=0,sDs=0,sId=0,sIs=0,sEm=0;
        for (int i = 0; i < pixelCount; ++i) {
            sDd+=ddBuf[i].x+ddBuf[i].y+ddBuf[i].z;
            sDs+=dsBuf[i].x+dsBuf[i].y+dsBuf[i].z;
            sId+=idBuf[i].x+idBuf[i].y+idBuf[i].z;
            sIs+=isBuf[i].x+isBuf[i].y+isBuf[i].z;
            sEm+=emBuf[i].x+emBuf[i].y+emBuf[i].z;
        }
        fprintf(stderr,"AOVSTAT: Dd=%.4f Ds=%.4f Id=%.4f Is=%.4f Em=%.4f\n",
                sDd/pixelCount,sDs/pixelCount,sId/pixelCount,sIs/pixelCount,sEm/pixelCount);
    }
    for (int i = 0; i < pixelCount; ++i) {
        output[i].radiance_Dd = make_float3(ddBuf[i].x, ddBuf[i].y, ddBuf[i].z);
        output[i].radiance_Ds = make_float3(dsBuf[i].x, dsBuf[i].y, dsBuf[i].z);
        output[i].radiance_Id = make_float3(idBuf[i].x, idBuf[i].y, idBuf[i].z);
        output[i].radiance_Is = make_float3(isBuf[i].x, isBuf[i].y, isBuf[i].z);
        output[i].emission    = make_float3(emBuf[i].x, emBuf[i].y, emBuf[i].z);
        output[i].baseColor   = make_float3(baseBuf[i].x, baseBuf[i].y, baseBuf[i].z);
        output[i].normal      = make_float3(nrmBuf[i].x, nrmBuf[i].y, nrmBuf[i].z);
        output[i].position    = make_float3(posBuf[i].x, posBuf[i].y, posBuf[i].z);
        output[i].shapeNormal = make_float3(geomNrmBuf[i].x, geomNrmBuf[i].y, geomNrmBuf[i].z);
        output[i].roughness   = matBuf[i].x;
        output[i].metallic    = matBuf[i].y;
        output[i].specular    = matBuf[i].z;
        output[i].opacity     = matBuf[i].w;
        output[i].sunMask     = sunMaskBuf[i];
        // Store variance for denoiser
        output[i].var_Dd = varDdBuf[i];
        output[i].var_Ds = varDsBuf[i];
        output[i].var_Id = varIdBuf[i];
        output[i].var_Is = varIsBuf[i];
    }

    // Free GPU buffers
    cu(cudaFree(gpuColor)); cu(cudaFree(gpuDD)); cu(cudaFree(gpuDS));
    cu(cudaFree(gpuID)); cu(cudaFree(gpuIS)); cu(cudaFree(gpuEm));
    cu(cudaFree(gpuBase)); cu(cudaFree(gpuNrm));
    cu(cudaFree(gpuPos)); cu(cudaFree(gpuGeomNrm)); cu(cudaFree(gpuMat));
    cu(cudaFree(gpuVarDd)); cu(cudaFree(gpuVarDs));
    cu(cudaFree(gpuVarId)); cu(cudaFree(gpuVarIs));
    cu(cudaFree(gpuSunMask));
}

// Wavefront pipeline (光线重排序 + 按材质分批 + 自适应逐像素) — included
// into this translation unit (same pattern as bvh_gpu.cu above): separable
// compilation is disabled, and the shade kernel calls device functions
// (gpu_shadeBounce/traceRay) defined here.
#include "render_wavefront.cu"

