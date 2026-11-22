#pragma once

#include "../include/types.h"
#include "camera.h"
#include "render.h"       // PerPixelData
#include "model.h"        // GPUModelData
#include <atomic>

// ---- GPU Render Arguments ----
struct GPURenderArgs {
    Camera      camera;
    int         width, height;
    int         spp;
    float       pDirect;
    int         maxDepth;
    uint32_t    baseSeed;
    // HDR skybox importance sampling data
    float*      skyCDF;        // GPU CDF for importance sampling (nullptr = uniform)
    float       skyCDFSum;     // total luminance sum
    int         skyWidth, skyHeight;

    // Directional sun (delta light, NEE-only). hasSun=0 disables.
    float3 sunDir;       // toward the sun (set by the caller when hasSun=1)
    float3 sunRadiance;  // per-unit radiance
    int    hasSun    = 0;
    float  sunSoftDeg = 1.0f;  // sun cone half-angle (soft shadows)
    // Sun-cone lighting kill (rig de-duplication): when a delta sun is the
    // scene's sun AND cone focus kept the HDRI cone bright, mask that cone
    // out of LIGHTING (sky NEE + escaping rays at depth>0). The cone stays
    // fully visible to primary camera rays (the sun-in-frame glow).
    int    sunConeKill = 0;
    float  sunConeHalfDeg = 25.0f;   // matches RENDER_SKY_FOCUS_CONE

    // ---- Adaptive per-pixel sampling (自适应逐像素) ----
    // adaptiveMin <= 1 disables the feature (byte-identical fixed-spp path).
    // With it on, `spp` becomes the MAX sample budget: a pixel stops early
    // once every AOV channel's relative standard error drops below
    // adaptiveThresh (checked every adaptiveCheck samples, adaptiveCheck
    // rounded to a power of two by the host).
    int   adaptiveMin = 0;
    int   adaptiveCheck = 8;
    float adaptiveThresh = 0.02f;
    // Device counters [total, min, max, earlyTerminatedPixels]; nullptr = off.
    unsigned long long* adaptiveStats = nullptr;
    // Total sample budget of the accumulation this call contributes to
    // (progressive chunked rendering; the wavefront host only feeds it to
    // the adaptive earlyStop statistics). 0 = single-call render, same as spp.
    int   sppBudgetTotal = 0;

    // ---- Wavefront pipeline knobs (波前: 光线重排序 + 按材质分批) ----
    // Read only by renderGPUWavefront (RENDER_WAVEFRONT=1 in console).
    int wfSortRays = 0;   // 1 = reorder live rays by direction before tracing
    int wfMatBatch = 0;   // 1 = batch live hits by material before shading

    // ---- Indirect bounce energy scale (RENDER_INDIRECT_SCALE) ----
    // Multiplies every radiance contribution contributed at path depth > 0
    // (NEE at bounce vertices, sky escapes, emissive hits, delta sun) —
    // i.e. the whole GI bounce term, direct lighting untouched. 1.0 =
    // unbiased default. The official Bistro exterior rig carries far less
    // interreflection than our calibrated interior look, so shade areas
    // read warm-gray instead of blue; dial this down (with SKY_SCALE up)
    // for the weak-bounce/blue-fill look.
    float indirectScale = 1.0f;

    // Emissive energy scale (RENDER_EMISSION_SCALE / position-aware rig
    // profiles): multiplies surface emission factors and light-object
    // radiance at shading time. Moved OFF the load-time material bake so a
    // resident scene can render daytime (fixtures ~off) and evening
    // (fixtures on) views without a reload. 1.0 = unbiased default; applied
    // in gpu_shadeBounce at the two emission consumption points.
    float emissionScale = 1.0f;

    // NEE samples per rough-surface hit (设计 17 的 NEE K). 0 = resolve to
    // the process default (RENDER_NEE_K via setNeeK, else 4) at render
    // entry; kernels only ever read the resolved args value, so a server
    // can vary K per request without touching global device state.
    int neeK = 0;
};

// ---- GPU Model Data (forward decl) ----
struct GPUModelData;

// ---- Tile size ----
constexpr int TILE_W = 16;
constexpr int TILE_H = 16;

// ---- CUDA-specific declarations (kernel + device functions) ----
#ifdef __CUDACC__
#include <curand_kernel.h>

// ---- Medium Stack for nested glass (IOR + absorption) ----
struct MediumRecord {
    float eta; float3 absorb;
};
struct MediumStack {
    MediumRecord stack[8];
    int size;
    __device__ void init() { size = 0; }
    __device__ void push(float eta, float3 absorb) {
        if (size < 8) { stack[size].eta = eta; stack[size].absorb = absorb; size++; }
    }
    __device__ void pop() { if (size > 0) size--; }
    __device__ float topEta() const { return size > 0 ? stack[size-1].eta : 1.0f; }
    __device__ float3 topAbsorb() const { return size > 0 ? stack[size-1].absorb : make_float3(1,1,1); }
    __device__ int count() const { return size; }
};

__global__ void renderKernel(
    GPURenderArgs      args,
    GPUModelData       model,
    cudaTextureObject_t skyTex,
    float4*            outputColor,
    float4*            outputDirectDiff,
    float4*            outputDirectSpec,
    float4*            outputIndirectDiff,
    float4*            outputIndirectSpec,
    float4*            outputEmission
);

// GPUPathState carries everything the per-bounce shading body needs across
// bounces — the megakernel keeps it in registers, the wavefront pipeline
// round-trips it through global memory between stage kernels.
struct GPUPathState {
    Ray      ray;
    float3   throughput;
    float    pathRough;
    float3   radiance;
    curandState rng;
    MediumStack medStack;
    int      misSampledMat[8];
    float    misSampledW[8];
    int      misSampledCnt;
    float    prevBsdfPdf;
    int      prevBsdfDielectric;
    float3   prevBaseColor;
    int      depth;
    int      alive;
};

__device__ void gpu_pathInit(GPUPathState& st, uint32_t seed, const Ray& ray);

// One path-tracing bounce against an already-traced hit. Returns true when
// the path continues (st.ray/st.depth updated); false = terminated.
// Shared source for the megakernel (tracePath) and the wavefront shade
// kernel, so both execute bit-identical math per path.
__device__ __noinline__ bool gpu_shadeBounce(
    GPUPathState&    st,
    PerPixelData&    split,
    const HitRecord& hit,
    const GPUModelData& model,
    cudaTextureObject_t skyTex,
    const GPURenderArgs& args,
    bool             excludeDirect
);

__device__ float3 tracePath(
    uint32_t&      seed,
    const Ray&     ray,
    const GPUModelData& model,
    cudaTextureObject_t skyTex,
    const GPURenderArgs& args,
    bool           excludeDirect,
    PerPixelData&  split
);

__device__ HitRecord traceRay(
    const Ray&     ray,
    const GPUModelData& model
);
#endif

// ---- WavefrontRenderContext: caller-owned wavefront pipeline state ----
// Owns the wavefront pipeline's persistent device buffers (path states,
// per-pixel accumulators, AOV outputs, CUB scratch) plus the absolute
// sample position of the accumulation. Held by the caller (one per console
// model / render session) and passed to every wavefront render: buffers
// are allocated on first use, reused while the resolution fits, and the
// accumulation persists across calls until resetAccumulation(). Sample
// seeds/jitter are pure functions of the absolute sample index, so
// chunked accumulation equals a one-shot render of the same total spp.
// Construction performs no CUDA calls; use it from the thread that owns
// GPU work (the GPU worker / console thread).
class WavefrontRenderContext {
public:
    WavefrontRenderContext();
    ~WavefrontRenderContext();
    WavefrontRenderContext(const WavefrontRenderContext&) = delete;
    WavefrontRenderContext& operator=(const WavefrontRenderContext&) = delete;

    // Zero the per-pixel accumulators and adaptive state; the next render
    // starts from absolute sample 0. Required on any radiance-affecting
    // change (camera, sun/sky/emitters, materials, quality knobs, size).
    void resetAccumulation();

    // Absolute sample index the next render call starts at (= samples
    // already folded into the accumulation).
    int nextSample() const;

private:
    struct State;
    State* state_;

    // The wavefront host functions drive State directly (buffer ensure,
    // absolute sample position); public API stays minimal by design.
    friend void renderGPUWavefront(GPURenderArgs args, GPUModelData model,
                                   cudaTextureObject_t skyTex,
                                   PerPixelData* output, int pixelCount,
                                   WavefrontRenderContext& ctx);
    friend void renderGPUWavefrontCancelable(GPURenderArgs args, GPUModelData model,
                                             cudaTextureObject_t skyTex,
                                             PerPixelData* output, int pixelCount,
                                             WavefrontRenderContext& ctx,
                                             const std::atomic<bool>* cancelRequested);
};

// ---- Host-side GPU render helpers ----
// Implemented in render_gpu.cu.  Declared separately for CPU vs CUDA.

#ifndef __CUDACC__
// CPU-side declarations — uses raw pointers to avoid CUDA std::vector issues
__host__ GPUModelData uploadModelToGPU(const float3* verts, int numVerts,
                               const float3* norms, const float2* uvs,
                               const int* matIds, int numFaces,
                               const LinearBVHNode* bvhNodes, int numBVHNodes,
                               const GPUMaterialBuffer& matBuf,
                               const GPULightTri* lightTris = nullptr,
                               int numLightTris = 0,
                               const float* triPrefix = nullptr,
                               const int* objFirst = nullptr,
                               const float* objArea = nullptr,
                               const float* objPower = nullptr,
                               const float3* objCentroid = nullptr,
                               int numObjs = 0);
void freeGPUModel(GPUModelData& data);
#endif

// GPU scene resource helpers implemented in bvh_gpu.cu.
void freeSkyBoxTexture(cudaTextureObject_t texture);
void freeGPUCDF(float* gpuCDF);

// Forward declaration for cudaTextureObject_t (opaque handle)
// Used by renderGPU.  On CUDA this is the real type; on CPU it's an opaque integer.
#ifndef __CUDACC__
typedef unsigned long long cudaTextureObject_t;
#endif

#if defined(__CUDACC__) && defined(RENDERER_HAS_CUDA)
// CUDA-side declarations (only in .cu files)
void renderGPU(GPURenderArgs args, GPUModelData model,
               cudaTextureObject_t skyTex,
               PerPixelData* output, int pixelCount);
void renderGPUWavefront(GPURenderArgs args, GPUModelData model,
                        cudaTextureObject_t skyTex,
                        PerPixelData* output, int pixelCount,
                        WavefrontRenderContext& ctx);
void renderGPUWavefrontCancelable(GPURenderArgs args, GPUModelData model,
                                  cudaTextureObject_t skyTex,
                                  PerPixelData* output, int pixelCount,
                                  WavefrontRenderContext& ctx,
                                  const std::atomic<bool>* cancelRequested);
#elif defined(RENDERER_HAS_CUDA) && !defined(__CUDACC__)
// Host-side declaration for CPU callers (console.cpp)
void renderGPU(GPURenderArgs args, GPUModelData model,
               cudaTextureObject_t skyTex,
               PerPixelData* output, int pixelCount);
void renderGPUWavefront(GPURenderArgs args, GPUModelData model,
                        cudaTextureObject_t skyTex,
                        PerPixelData* output, int pixelCount,
                        WavefrontRenderContext& ctx);
void renderGPUWavefrontCancelable(GPURenderArgs args, GPUModelData model,
                                  cudaTextureObject_t skyTex,
                                  PerPixelData* output, int pixelCount,
                                  WavefrontRenderContext& ctx,
                                  const std::atomic<bool>* cancelRequested);
#endif

// NEE samples-per-rough-hit knob (RENDER_NEE_K). Sets the process default;
// per-render overrides go through GPURenderArgs::neeK.
void setNeeK(int k);
// Process default NEE K (what args.neeK == 0 resolves to).
int defaultNeeK();

// 从 RENDER_SUN_INT/AZ/EL/TINT/SOFT 与 RENDER_SKY_FOCUS_DIM/CONE 环境变量
// 组装方向太阳（console 与 render server 共用同一组装路径，保证两边观感
// 一致——服务端此前从未设置 hasSun，Bistro 内景缺主直接光导致整体死黑）。
// 返回是否启用了太阳。
bool applySunFromEnv(GPURenderArgs& args);
// Per-render physical env knobs (RENDER_INDIRECT_SCALE / RENDER_EMISSION_SCALE)
// assembled from the process env — the CLI/startup-rig path. The server's
// position-aware profiles deliver these via the task instead.
void applyPhysicalEnvFromEnv(GPURenderArgs& args);
// 显式参数版（render server /render 的 sunAz/sunEl/sunInt）：方向来自
// az/el，radiance = inten × RENDER_SUN_TINT，soft/cone-kill 仍跟随 env——
// 与 env rig 同观感。inten<=0 时直接返回 false（关太阳）。
bool applySunParams(GPURenderArgs& args, float az, float el, float inten);
// Foliage shadow transmission knob (RENDER_LEAF_SHADOW_TINT, default 1.0)
void setLeafShadowTint(float t);
// Exact sRGB EOTF for baseColor textures (RENDER_SRGB_EXACT=1; default pow2.2)
void setSrgbExact(int v);
// Average samples actually traced per pixel by the last GPU render
// (equals args.spp when adaptive sampling is off). Drives the denoiser's
// SPP-adaptive gates with the effective sample count.
double getLastGpuAvgSpp();

// Normalize adaptive-sampling knobs: disabled combos collapse to
// adaptiveMin=0 (byte-identical fixed-spp path), the check interval is
// rounded up to a power of two (the kernels test `n & (check-1) == 0`).
// Both render hosts call this before launch so the megakernel and the
// wavefront reduce kernel make identical per-pixel convergence decisions.
inline void sanitizeAdaptiveArgs(GPURenderArgs& a) {
    if (a.adaptiveMin > 1 && a.spp > a.adaptiveMin) {
        if (a.adaptiveCheck < 2) a.adaptiveCheck = 2;
        int p = 1; while (p < a.adaptiveCheck) p <<= 1;
        a.adaptiveCheck = p;
    } else {
        a.adaptiveMin = 0;
    }
}
