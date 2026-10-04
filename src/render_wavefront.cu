// =========================================================================
// render_wavefront.cu — Wavefront path tracing pipeline
// (波前架构： 光线重排序 + 按材质分批着色 + 自适应逐像素采样)
//
// This file is #included from render_gpu.cu (same single-translation-unit
// pattern as bvh_gpu.cu — separable compilation is disabled in this project
// because the device linker strips the PTX that sm_70 needs under CUDA 13).
//
// Design
// ------
// The megakernel (renderKernel) traces whole paths per thread. Here each
// (pixel, sample) path is a GPUPathState record in global memory, advanced
// one bounce per kernel launch:
//
//   per sample s:
//     wfGenKernel      camera rays + fresh path states (skips converged px)
//     repeat depth <= maxDepth:
//       [wfRayKeyKernel + cub::DeviceRadixSort]   光线重排序 (optional)
//       wfTraceKernel   BVH intersect all live rays
//       [wfMatKeyKernel + cub::DeviceRadixSort]   按材质分批 (optional)
//       wfShadeKernel   gpu_shadeBounce() — SAME device function the
//                       megakernel runs, so results are byte-identical
//       cub::DeviceSelect::Flagged  stream compaction of dead paths
//     wfAccumKernel    fold the sample into the per-pixel accumulators,
//                      adaptive-convergence check (same formula as the
//                      megakernel's), converged pixels leave the queue
//   wfFinalizeKernel   divide by per-pixel sample count, emit the exact
//                      same output buffers the megakernel emits
//
// Correctness invariants
// ----------------------
// * Each path keeps its own curandState in GPUPathState, initialized with
//   the same useed = baseSeed + s*0xABCD the megakernel uses, and consumes
//   draws in the same order (gpu_shadeBounce is shared source) → identical
//   per-path math.
// * Queue ORDER (gen append, sorting, compaction) never affects results:
//   every accumulation is indexed by pixel id; per-pixel sample folding
//   happens in sample order in wfAccumKernel.
// * No atomics on radiance data (each pixel owned by ≤1 path per sample),
//   so no FP non-associativity: RENDER_WAVEFRONT=0/1 and the sort toggles
//   are byte-identical A/B pairs inside one binary.
// =========================================================================

#include <cub/cub.cuh>
#include <atomic>

// ---- Persistent per-pixel accumulators (across samples) ----
struct WFPixelAccum {
    float3 acc_Dd, acc_Ds, acc_Id, acc_Is, acc_em;
    float3 acc_Dd2, acc_Ds2, acc_Id2, acc_Is2;
    float3 acc_base, acc_nrm, acc_pos, acc_geomNrm;
    float4 acc_mat;      // roughness, metallic, specular, opacity
    float  acc_sun;      // P1: first-hit direct-sun luminance (grade matte)
    int    n;            // samples accumulated so far
    int    converged;    // adaptive: 1 = dropped out of the queue
};

// ---- wfGenKernel: camera rays + path state init for one sample ----
__global__ void wfGenKernel(
    GPURenderArgs        args,
    GPUPathState*        states,
    PerPixelData*        sampleSplit,
    WFPixelAccum*        accum,
    int*                 sampleTag,   // [px] = sample index the px last generated for
    int                  sampleIdx,
    int*                 liveIdx,
    unsigned int*        liveCount)
{
    const int i = blockIdx.x * blockDim.x + threadIdx.x;
    const int pixelCount = args.width * args.height;
    if (i >= pixelCount) return;
    if (accum[i].converged) return;

    const int x = i % args.width;
    const int y = i / args.width;

    // Identical seed/jitter sequence to the megakernel (renderKernel).
    uint32_t baseSeed = args.baseSeed + uint32_t(i) * 2654435761u;
    uint32_t hsh = baseSeed * 0x9E3779B9u;
    hsh = (hsh >> 16) ^ hsh;
    const float pxU = (hsh & 0xFFFFu) * (1.0f / 65536.0f);
    hsh = hsh * 0x9E3779B9u; hsh = (hsh >> 16) ^ hsh;
    const float pxV = (hsh & 0xFFFFu) * (1.0f / 65536.0f);
    constexpr float R2_X = 0.7548776662466927f;
    constexpr float R2_Y = 0.5698402909980532f;

    const int s = sampleIdx;
    uint32_t useed = baseSeed + uint32_t(s) * 0xABCD;
    curandState rng;
    curand_init(useed, 0, 0, &rng);
    float jitU = pxU + float(s) * R2_X; jitU -= floorf(jitU);
    float jitV = pxV + float(s) * R2_Y; jitV -= floorf(jitV);
    float lensU = curand_uniform(&rng) * 2.0f - 1.0f;
    float lensV = curand_uniform(&rng) * 2.0f - 1.0f;
    Ray ray = args.camera.generateRay(
        float(x), float(y), jitU, jitV, lensU, lensV,
        make_float2(float(args.width), float(args.height)));

    gpu_pathInit(states[i], useed, ray);
    sampleSplit[i] = PerPixelData{};
    sampleTag[i] = s;
    const unsigned int pos = atomicAdd(liveCount, 1u);
    liveIdx[pos] = i;
}

// ---- wfTraceKernel: BVH intersect all live rays ----
__global__ void wfTraceKernel(
    const int*           liveIdx,
    int                  count,
    GPUPathState*        states,
    HitRecord*           hits,
    GPUModelData         model)
{
    const int i = blockIdx.x * blockDim.x + threadIdx.x;
    if (i >= count) return;
    const int pid = liveIdx[i];
    hits[pid] = traceRay(states[pid].ray, model);
}

// ---- Sort keys for ray reordering (光线重排序) ----
// Octahedral-encode the ray direction to 2×15 bits, Morton-interleave:
// neighbouring keys = neighbouring directions → coherent BVH traversal.
__device__ __forceinline__ unsigned int wfMorton2(unsigned int x, unsigned int y) {
    x = (x | (x << 8)) & 0x00FF00FFu;
    x = (x | (x << 4)) & 0x0F0F0F0Fu;
    x = (x | (x << 2)) & 0x33333333u;
    x = (x | (x << 1)) & 0x55555555u;
    y = (y | (y << 8)) & 0x00FF00FFu;
    y = (y | (y << 4)) & 0x0F0F0F0Fu;
    y = (y | (y << 2)) & 0x33333333u;
    y = (y | (y << 1)) & 0x55555555u;
    return x | (y << 1);
}

__device__ __forceinline__ unsigned int wfRaySortKey(const float3& d) {
    // Octahedral mapping of the unit direction to [0,1]^2
    const float invL1 = 1.0f / (fabsf(d.x) + fabsf(d.y) + fabsf(d.z) + 1e-20f);
    float ox = d.x * invL1, oy = d.y * invL1;
    if (d.z < 0.0f) {
        const float tx = (1.0f - fabsf(oy)) * (ox >= 0.0f ? 1.0f : -1.0f);
        oy = (1.0f - fabsf(ox)) * (oy >= 0.0f ? 1.0f : -1.0f);
        ox = tx;
    }
    unsigned int qx = min(32767u, (unsigned int)((ox * 0.5f + 0.5f) * 32767.0f));
    unsigned int qy = min(32767u, (unsigned int)((oy * 0.5f + 0.5f) * 32767.0f));
    return wfMorton2(qx, qy);
}

__global__ void wfRayKeyKernel(
    const int*           liveIdx,
    int                  count,
    const GPUPathState*  states,
    unsigned int*        keys)
{
    const int i = blockIdx.x * blockDim.x + threadIdx.x;
    if (i >= count) return;
    keys[i] = wfRaySortKey(states[liveIdx[i]].ray.direction);
}

// ---- Sort keys for material batching (按材质分批) ----
// Hits on the same material shade together: texture fetches and BSDF
// branches stay warp-coherent. Missed rays (sky) sort to the tail.
__global__ void wfMatKeyKernel(
    const int*           liveIdx,
    int                  count,
    const HitRecord*     hits,
    GPUModelData         model,
    unsigned int*        keys)
{
    const int i = blockIdx.x * blockDim.x + threadIdx.x;
    if (i >= count) return;
    const HitRecord& h = hits[liveIdx[i]];
    unsigned int k = 0xFFFFFFFFu;
    if (h.hasHit() && model.materialIds) {
        const int m = model.materialIds[h.faceIdx];
        k = (m < 0) ? 0xFFFFFFFEu : (unsigned int)m;
    }
    keys[i] = k;
}

// ---- wfShadeKernel: one bounce per live path (shared shading body) ----
__global__ void wfShadeKernel(
    const int*           liveIdx,
    int                  count,
    GPUPathState*        states,
    PerPixelData*        sampleSplit,
    const HitRecord*     hits,
    GPUModelData         model,
    cudaTextureObject_t  skyTex,
    GPURenderArgs        args,
    int*                 aliveFlags)   // per queue position
{
    const int i = blockIdx.x * blockDim.x + threadIdx.x;
    if (i >= count) return;
    const int pid = liveIdx[i];
    const bool alive = gpu_shadeBounce(states[pid], sampleSplit[pid],
                                       hits[pid], model, skyTex, args, false);
    aliveFlags[i] = alive ? 1 : 0;
}

// ---- wfAccumKernel: fold one sample into the pixel accumulators ----
// Runs for every pixel every sample (progress accounting); only pixels that
// generated a path for THIS sample accumulate. The adaptive convergence
// test mirrors the megakernel's exactly (same accumulators, same order).
__global__ void wfAccumKernel(
    WFPixelAccum*        accum,
    const PerPixelData*  sampleSplit,
    const int*           sampleTag,
    int                  sampleIdx,
    int                  pixelCount,
    int                  adaptiveMin,
    int                  adaptiveCheck,
    float                adaptiveThresh,
    unsigned int*        progressCounter)
{
    const int i = blockIdx.x * blockDim.x + threadIdx.x;
    if (i >= pixelCount) return;
    if (progressCounter) atomicAdd(progressCounter, 1u);
    if (sampleTag[i] != sampleIdx) return;   // converged earlier: no contribution

    const PerPixelData& split = sampleSplit[i];
    WFPixelAccum& a = accum[i];
    // NaN/Inf guard on accumulators (same clampF as the megakernel)
    auto clampF = [](float x) { return (isnan(x)||isinf(x)||x<-1e10f||x>1e10f) ? 0.0f : x; };
    float3 rDd = make_float3(clampF(split.radiance_Dd.x), clampF(split.radiance_Dd.y), clampF(split.radiance_Dd.z));
    float3 rDs = make_float3(clampF(split.radiance_Ds.x), clampF(split.radiance_Ds.y), clampF(split.radiance_Ds.z));
    float3 rId = make_float3(clampF(split.radiance_Id.x), clampF(split.radiance_Id.y), clampF(split.radiance_Id.z));
    float3 rIs = make_float3(clampF(split.radiance_Is.x), clampF(split.radiance_Is.y), clampF(split.radiance_Is.z));
    float3 rEm = make_float3(clampF(split.emission.x), clampF(split.emission.y), clampF(split.emission.z));
    a.acc_Dd += rDd;
    a.acc_Ds += rDs;
    a.acc_Id += rId;
    a.acc_Is += rIs;
    a.acc_em += rEm;
    a.acc_sun += split.sunMask;
    a.acc_base += split.baseColor;
    a.acc_nrm += split.normal;
    a.acc_pos += split.position;
    a.acc_geomNrm += split.shapeNormal;
    a.acc_mat.x += split.roughness;
    a.acc_mat.y += split.metallic;
    a.acc_mat.z += split.specular;
    a.acc_mat.w += split.opacity;
    a.acc_Dd2 += rDd * rDd;
    a.acc_Ds2 += rDs * rDs;
    a.acc_Id2 += rId * rId;
    a.acc_Is2 += rIs * rIs;
    const int n = ++a.n;

    if (adaptiveMin > 1 && n >= adaptiveMin && (n & (adaptiveCheck - 1)) == 0) {
        const float3 lumW3 = make_float3(0.299f, 0.587f, 0.114f);
        const float inv = 1.0f / float(n);
        auto rseOf = [&](const float3& sum, const float3& sum2) -> float {
            float lm  = dot(sum  * inv, lumW3);
            float m2  = dot(sum2 * inv, lumW3);
            float var = fmaxf(0.0f, m2 - lm * lm);
            return sqrtf(var * inv) / (fabsf(lm) + 1e-3f);
        };
        float r = fmaxf(fmaxf(rseOf(a.acc_Dd, a.acc_Dd2), rseOf(a.acc_Ds, a.acc_Ds2)),
                        fmaxf(rseOf(a.acc_Id, a.acc_Id2), rseOf(a.acc_Is, a.acc_Is2)));
        if (r < adaptiveThresh) a.converged = 1;
    }
}

// ---- wfFinalizeKernel: emit the same output buffers as the megakernel ----
__global__ void wfFinalizeKernel(
    const WFPixelAccum*  accum,
    int                  pixelCount,
    int                  sppBudget,
    float4*              outputColor,
    float4*              outputDirectDiff,
    float4*              outputDirectSpec,
    float4*              outputIndirectDiff,
    float4*              outputIndirectSpec,
    float4*              outputEmission,
    float*               outputVarDd,
    float*               outputVarDs,
    float*               outputVarId,
    float*               outputVarIs,
    float4*              outputBaseColor,
    float4*              outputNormal,
    float4*              outputPosition,
    float4*              outputGeomNormal,
    float4*              outputMaterial,
    unsigned long long*  adaptiveStats,
    float*               outputSunMask)
{
    const int idx = blockIdx.x * blockDim.x + threadIdx.x;
    if (idx >= pixelCount) return;
    const WFPixelAccum& a = accum[idx];
    const int n = max(1, a.n);
    const float3 acc_Dd = a.acc_Dd, acc_Ds = a.acc_Ds, acc_Id = a.acc_Id, acc_Is = a.acc_Is, acc_em = a.acc_em;
    const float3 acc_Dd2 = a.acc_Dd2, acc_Ds2 = a.acc_Ds2, acc_Id2 = a.acc_Id2, acc_Is2 = a.acc_Is2;

    if (adaptiveStats) {
        atomicAdd(&adaptiveStats[0], (unsigned long long)n);
        atomicMax(&adaptiveStats[2], (unsigned long long)n);
        atomicMin(&adaptiveStats[1], (unsigned long long)n);
        if (n < sppBudget) atomicAdd(&adaptiveStats[3], 1ULL);
    }

    const float invSpp = 1.0f / n;
    float3 final = (acc_Dd + acc_Ds + acc_Id + acc_Is + acc_em) * invSpp;

    outputColor[idx] = make_float4(final.x, final.y, final.z, 1.0f);
    if (outputDirectDiff)   outputDirectDiff[idx]   = make_float4(acc_Dd.x*invSpp, acc_Dd.y*invSpp, acc_Dd.z*invSpp, 1.0f);
    if (outputDirectSpec)   outputDirectSpec[idx]   = make_float4(acc_Ds.x*invSpp, acc_Ds.y*invSpp, acc_Ds.z*invSpp, 1.0f);
    if (outputIndirectDiff) outputIndirectDiff[idx] = make_float4(acc_Id.x*invSpp, acc_Id.y*invSpp, acc_Id.z*invSpp, 1.0f);
    if (outputIndirectSpec) outputIndirectSpec[idx] = make_float4(acc_Is.x*invSpp, acc_Is.y*invSpp, acc_Is.z*invSpp, 1.0f);
    if (outputEmission)     outputEmission[idx]     = make_float4(acc_em.x*invSpp, acc_em.y*invSpp, acc_em.z*invSpp, 1.0f);

    if (outputBaseColor)    outputBaseColor[idx]    = make_float4(a.acc_base.x*invSpp, a.acc_base.y*invSpp, a.acc_base.z*invSpp, 1.0f);
    if (outputNormal) {
        float3 nn = a.acc_nrm * invSpp;
        float nl = sqrtf(dot(nn, nn));
        nn = (nl > 1e-6f) ? nn * (1.0f / nl) : make_float3(0.0f, 0.0f, 0.0f);
        outputNormal[idx] = make_float4(nn.x, nn.y, nn.z, 1.0f);
    }
    if (outputPosition) {
        float3 p = a.acc_pos * invSpp;
        outputPosition[idx] = make_float4(p.x, p.y, p.z, 0.0f);
    }
    if (outputGeomNormal) {
        float3 nn = a.acc_geomNrm * invSpp;
        float nl = sqrtf(dot(nn, nn));
        nn = (nl > 1e-6f) ? nn * (1.0f / nl) : make_float3(0.0f, 0.0f, 0.0f);
        outputGeomNormal[idx] = make_float4(nn.x, nn.y, nn.z, 0.0f);
    }
    if (outputMaterial) {
        outputMaterial[idx] = make_float4(a.acc_mat.x*invSpp, a.acc_mat.y*invSpp,
                                          a.acc_mat.z*invSpp, a.acc_mat.w*invSpp);
    }
    if (outputSunMask) {
        float sm = a.acc_sun * invSpp;
        outputSunMask[idx] = (isnan(sm) || isinf(sm) || sm < 0.0f) ? 0.0f : sm;
    }

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
}

// =========================================================================
// Host-side wavefront render
// =========================================================================
struct WavefrontBuffers {
    GPUPathState* states = nullptr;
    PerPixelData* sampleSplit = nullptr;
    WFPixelAccum* accum = nullptr;
    HitRecord* hits = nullptr;
    int* liveIdx[2] = {nullptr, nullptr};
    int* flags = nullptr;
    unsigned int* keys[2] = {nullptr, nullptr};
    unsigned int* liveCount = nullptr;
    int* sampleTag = nullptr;
    int* numSelected = nullptr;
    unsigned long long* adaptStats = nullptr;

    float4* color = nullptr;
    float4* dd = nullptr;
    float4* ds = nullptr;
    float4* id = nullptr;
    float4* is = nullptr;
    float4* em = nullptr;
    float4* base = nullptr;
    float4* nrm = nullptr;
    float4* pos = nullptr;
    float4* geomNrm = nullptr;
    float4* mat = nullptr;
    float* varDd = nullptr;
    float* varDs = nullptr;
    float* varId = nullptr;
    float* varIs = nullptr;
    float* sunMask = nullptr;
    void* cubTemp = nullptr;
    size_t cubTempBytes = 0;
    int capacity = 0;
    // Per-session progress/pinned infrastructure (批1): used to live on the
    // stack of every render call — cudaMalloc + two cudaHostAlloc + a stream
    // create per CHUNK was pure churn on the single GPU worker.
    unsigned int* gpuProgress = nullptr;
    unsigned int* pinnedProgress = nullptr;
    int* pinnedCount = nullptr;
    cudaStream_t progStream = nullptr;

    void ensure(int pixelCount) {
        if (capacity >= pixelCount) return;
        release();
        cudaMalloc(&states, sizeof(GPUPathState) * pixelCount);
        cudaMalloc(&sampleSplit, sizeof(PerPixelData) * pixelCount);
        cudaMalloc(&accum, sizeof(WFPixelAccum) * pixelCount);
        cudaMalloc(&hits, sizeof(HitRecord) * pixelCount);
        cudaMalloc(&liveIdx[0], sizeof(int) * pixelCount);
        cudaMalloc(&liveIdx[1], sizeof(int) * pixelCount);
        cudaMalloc(&flags, sizeof(int) * pixelCount);
        cudaMalloc(&keys[0], sizeof(unsigned int) * pixelCount);
        cudaMalloc(&keys[1], sizeof(unsigned int) * pixelCount);
        cudaMalloc(&liveCount, sizeof(unsigned int));
        cudaMalloc(&sampleTag, sizeof(int) * pixelCount);
        cudaMalloc(&numSelected, sizeof(int));
        cudaMalloc(&adaptStats, 4 * sizeof(unsigned long long));

        cudaMalloc(&color, sizeof(float4) * pixelCount);
        cudaMalloc(&dd, sizeof(float4) * pixelCount);
        cudaMalloc(&ds, sizeof(float4) * pixelCount);
        cudaMalloc(&id, sizeof(float4) * pixelCount);
        cudaMalloc(&is, sizeof(float4) * pixelCount);
        cudaMalloc(&em, sizeof(float4) * pixelCount);
        cudaMalloc(&varDd, sizeof(float) * pixelCount);
        cudaMalloc(&varDs, sizeof(float) * pixelCount);
        cudaMalloc(&varId, sizeof(float) * pixelCount);
        cudaMalloc(&varIs, sizeof(float) * pixelCount);
        cudaMalloc(&base, sizeof(float4) * pixelCount);
        cudaMalloc(&nrm, sizeof(float4) * pixelCount);
        cudaMalloc(&pos, sizeof(float4) * pixelCount);
        cudaMalloc(&geomNrm, sizeof(float4) * pixelCount);
        cudaMalloc(&mat, sizeof(float4) * pixelCount);
        cudaMalloc(&sunMask, sizeof(float) * pixelCount);

        size_t selectBytes = 0, sortBytes = 0;
        cub::DeviceSelect::Flagged(nullptr, selectBytes, liveIdx[0], flags,
                                   liveIdx[1], numSelected, pixelCount);
        cub::DeviceRadixSort::SortPairs(nullptr, sortBytes, keys[0], keys[1],
                                        liveIdx[0], liveIdx[1], pixelCount);
        cubTempBytes = (selectBytes > sortBytes) ? selectBytes : sortBytes;
        cudaMalloc(&cubTemp, cubTempBytes);
        cudaMalloc(&gpuProgress, sizeof(unsigned int));
        cudaHostAlloc(&pinnedProgress, sizeof(unsigned int), cudaHostAllocMapped);
        cudaHostAlloc(&pinnedCount, sizeof(int), cudaHostAllocMapped);
        cudaStreamCreateWithFlags(&progStream, cudaStreamNonBlocking);
        cudaDeviceSetLimit(cudaLimitStackSize, 4096);
        capacity = pixelCount;
        // cudaMalloc does not zero: a fresh allocation starts from a clean
        // accumulation, so the first render on a context is well-defined
        // even without an explicit resetAccumulation().
        reset(pixelCount);
    }

    void reset(int pixelCount) {
        cudaMemset(accum, 0, sizeof(WFPixelAccum) * pixelCount);
        cudaMemset(sampleTag, 0xFF, sizeof(int) * pixelCount);
        cudaMemset(adaptStats, 0, 4 * sizeof(unsigned long long));
        const unsigned long long ullMax = ~0ULL;
        cudaMemcpy(adaptStats + 1, &ullMax, sizeof(ullMax), cudaMemcpyHostToDevice);
    }

    void release() {
        cudaFree(states); cudaFree(sampleSplit); cudaFree(accum); cudaFree(hits);
        cudaFree(liveIdx[0]); cudaFree(liveIdx[1]); cudaFree(flags);
        cudaFree(keys[0]); cudaFree(keys[1]); cudaFree(liveCount);
        cudaFree(sampleTag); cudaFree(numSelected); cudaFree(adaptStats);
        cudaFree(color); cudaFree(dd); cudaFree(ds); cudaFree(id);
        cudaFree(is); cudaFree(em); cudaFree(base); cudaFree(nrm);
        cudaFree(pos); cudaFree(geomNrm); cudaFree(mat);
        cudaFree(varDd); cudaFree(varDs); cudaFree(varId); cudaFree(varIs);
        cudaFree(sunMask); cudaFree(cubTemp);
        if (progStream) cudaStreamDestroy(progStream);
        cudaFree(gpuProgress);
        if (pinnedProgress) cudaFreeHost(pinnedProgress);
        if (pinnedCount) cudaFreeHost(pinnedCount);
        states = nullptr; sampleSplit = nullptr; accum = nullptr; hits = nullptr;
        liveIdx[0] = nullptr; liveIdx[1] = nullptr; flags = nullptr;
        keys[0] = nullptr; keys[1] = nullptr; liveCount = nullptr;
        sampleTag = nullptr; numSelected = nullptr; adaptStats = nullptr;
        color = nullptr; dd = nullptr; ds = nullptr; id = nullptr;
        is = nullptr; em = nullptr; base = nullptr; nrm = nullptr;
        pos = nullptr; geomNrm = nullptr; mat = nullptr;
        varDd = nullptr; varDs = nullptr; varId = nullptr; varIs = nullptr;
        sunMask = nullptr; cubTemp = nullptr; cubTempBytes = 0; capacity = 0;
        gpuProgress = nullptr; pinnedProgress = nullptr; pinnedCount = nullptr;
        progStream = nullptr;
    }
};

// ---- WavefrontRenderContext: caller-owned pipeline state ----
// Defined here (render_gpu.cu translation unit) so the opaque State can
// hold the CUDA-pointer WavefrontBuffers without leaking device types into
// render_gpu.h. nextSample is atomic so the HTTP/session threads can poll
// progress while the GPU worker renders.
struct WavefrontRenderContext::State {
    WavefrontBuffers buffers;
    std::atomic<int> nextSample{0};
};

WavefrontRenderContext::WavefrontRenderContext() : state_(new State()) {}
WavefrontRenderContext::~WavefrontRenderContext() { delete state_; }

void WavefrontRenderContext::resetAccumulation() {
    state_->nextSample.store(0, std::memory_order_release);
    if (state_->buffers.capacity > 0)
        state_->buffers.reset(state_->buffers.capacity);
}

int WavefrontRenderContext::nextSample() const {
    return state_->nextSample.load(std::memory_order_acquire);
}

void renderGPUWavefront(GPURenderArgs args, GPUModelData model,
                        cudaTextureObject_t skyTex,
                        PerPixelData* output, int pixelCount,
                        WavefrontRenderContext& ctx) {
    renderGPUWavefrontCancelable(args, model, skyTex, output, pixelCount, ctx, nullptr);
}

void renderGPUWavefrontCancelable(GPURenderArgs args, GPUModelData model,
                                  cudaTextureObject_t skyTex,
                                  PerPixelData* output, int pixelCount,
                                  WavefrontRenderContext& ctx,
                                  const std::atomic<bool>* cancelRequested) {
    if (args.neeK <= 0) args.neeK = defaultNeeK();
    sanitizeAdaptiveArgs(args);
    const bool adaptive = args.adaptiveMin > 1;

    // Progressive accumulation: this call folds absolute samples
    // [startSample, startSample + args.spp) into the caller-owned
    // accumulators. Seeds and jitter sequences are pure functions of the
    // absolute sample index, so chunked accumulation is byte-identical to
    // a one-shot render of the same total spp (design 5.4).
    WavefrontBuffers& buffers = ctx.state_->buffers;
    const int startSample = ctx.state_->nextSample.load(std::memory_order_acquire);
    const int endSample = startSample + args.spp;
    const int sppBudgetTotal = args.sppBudgetTotal > 0 ? args.sppBudgetTotal : args.spp;

    fprintf(stderr, "renderGPUWavefront: entered, pixelCount=%d start=%d spp=%d sort=%d matbatch=%d\n",
            pixelCount, startSample, args.spp, args.wfSortRays, args.wfMatBatch); fflush(stderr);

    if (adaptive)
        fprintf(stderr, "  adaptive sampling: min=%d check=%d thresh=%.4f budget=%d\n",
                args.adaptiveMin, args.adaptiveCheck, args.adaptiveThresh, sppBudgetTotal);

    // Allocate/reuse the persistent buffers; a fresh allocation starts from
    // a clean accumulation (see ensure()).
    buffers.ensure(pixelCount);

    GPUPathState*& d_states = buffers.states;
    PerPixelData*& d_sampleSplit = buffers.sampleSplit;
    WFPixelAccum*& d_accum = buffers.accum;
    HitRecord*& d_hits = buffers.hits;
    int* (&d_liveIdx)[2] = buffers.liveIdx;
    int*& d_flags = buffers.flags;
    unsigned int* (&d_keys)[2] = buffers.keys;
    unsigned int*& d_liveCount = buffers.liveCount;
    int*& d_sampleTag = buffers.sampleTag;
    int*& d_numSelected = buffers.numSelected;
    unsigned long long*& d_adaptStats = buffers.adaptStats;
    if (adaptive)
        args.adaptiveStats = d_adaptStats;

    // ---- Output buffers (same set as the megakernel host) ----
    float4*& gpuColor = buffers.color; float4*& gpuDD = buffers.dd;
    float4*& gpuDS = buffers.ds; float4*& gpuID = buffers.id;
    float4*& gpuIS = buffers.is; float4*& gpuEm = buffers.em;
    float4*& gpuBase = buffers.base; float4*& gpuNrm = buffers.nrm;
    float4*& gpuPos = buffers.pos; float4*& gpuGeomNrm = buffers.geomNrm;
    float4*& gpuMat = buffers.mat;
    float*& gpuVarDd = buffers.varDd; float*& gpuVarDs = buffers.varDs;
    float*& gpuVarId = buffers.varId; float*& gpuVarIs = buffers.varIs;
    float*& gpuSunMask = buffers.sunMask;
    void*& d_cubTemp = buffers.cubTemp;
    size_t& cubTempBytes = buffers.cubTempBytes;

    // ---- Progress reporting (persistent per-session buffers, 批1: the
    // cudaMalloc/cudaHostAlloc/stream used to be recreated every call) ----
    unsigned int*& gpuProgress = buffers.gpuProgress;
    cudaMemset(gpuProgress, 0, sizeof(unsigned int));
    unsigned int*& pinnedProgress = buffers.pinnedProgress;
    cudaStream_t& progStream = buffers.progStream;
    *pinnedProgress = 0;
    int*& pinnedCount = buffers.pinnedCount;

    cudaGetLastError();
    const int threads = 256;
    const int pxBlocks = (pixelCount + threads - 1) / threads;
    const long long barTotal = (long long)pixelCount * args.spp;
    // stderr progress bar: stderr spam + a D2H sync every 8th sample —
    // RENDER_VERBOSE only.
    const bool verbose = [] {
        const char* v = std::getenv("RENDER_VERBOSE");
        return v && v[0] == '1';
    }();
    ProgressBar bar(verbose ? barTotal : 0);

    // =============== sample loop ===============
    int completed = 0;
    for (int s = startSample; s < endSample; ++s) {
        if (cancelRequested && cancelRequested->load(std::memory_order_acquire))
            break;
        // gen
        cudaMemsetAsync(d_liveCount, 0, sizeof(unsigned int));
        wfGenKernel<<<pxBlocks, threads>>>(args, d_states, d_sampleSplit, d_accum,
                                           d_sampleTag, s, d_liveIdx[0], d_liveCount);
        cudaMemcpyAsync(pinnedCount, d_liveCount, sizeof(unsigned int),
                        cudaMemcpyDeviceToHost);
        cudaStreamSynchronize(0);
        int count = *pinnedCount;
        if (count == 0) break;   // every pixel converged

        // =============== bounce loop ===============
        bool aborted = false;
        int cur = 0;
        for (int depth = 0; depth < args.maxDepth && count > 0; ++depth) {
            if (cancelRequested && cancelRequested->load(std::memory_order_acquire)) {
                count = 0;
                aborted = true;
                break;
            }
            const int blocks = (count + threads - 1) / threads;

            // 光线重排序: sort live rays by direction before tracing
            if (args.wfSortRays) {
                wfRayKeyKernel<<<blocks, threads>>>(d_liveIdx[cur], count, d_states, d_keys[0]);
                cub::DeviceRadixSort::SortPairs(d_cubTemp, cubTempBytes,
                                                d_keys[0], d_keys[1],
                                                d_liveIdx[cur], d_liveIdx[cur ^ 1], count);
                cur ^= 1;
            }
            // intersect
            wfTraceKernel<<<blocks, threads>>>(d_liveIdx[cur], count, d_states, d_hits, model);
            // 按材质分批: group hits by material before shading
            if (args.wfMatBatch) {
                wfMatKeyKernel<<<blocks, threads>>>(d_liveIdx[cur], count, d_hits, model, d_keys[0]);
                cub::DeviceRadixSort::SortPairs(d_cubTemp, cubTempBytes,
                                                d_keys[0], d_keys[1],
                                                d_liveIdx[cur], d_liveIdx[cur ^ 1], count);
                cur ^= 1;
            }
            // shade one bounce
            wfShadeKernel<<<blocks, threads>>>(d_liveIdx[cur], count, d_states, d_sampleSplit,
                                               d_hits, model, skyTex, args, d_flags);
            // compact: keep survivors
            cub::DeviceSelect::Flagged(d_cubTemp, cubTempBytes,
                                       d_liveIdx[cur], d_flags, d_liveIdx[cur ^ 1],
                                       d_numSelected, count);
            cudaMemcpyAsync(pinnedCount, d_numSelected, sizeof(int), cudaMemcpyDeviceToHost);
            cudaStreamSynchronize(0);
            count = *pinnedCount;
            cur ^= 1;
        }

        // A sample cancelled mid-bounce is discarded, not folded: the
        // accumulation only ever extends by complete samples (design 5.4),
        // and the continuation re-runs this absolute sample index.
        if (aborted) break;

        // fold the sample into the per-pixel accumulators + adaptive check
        wfAccumKernel<<<pxBlocks, threads>>>(d_accum, d_sampleSplit, d_sampleTag, s,
                                             pixelCount,
                                             adaptive ? args.adaptiveMin : 0,
                                             args.adaptiveCheck, args.adaptiveThresh,
                                             gpuProgress);
        ++completed;
        // progress bar (non-blocking poll) — RENDER_VERBOSE only: the poll
        // costs a D2H copy + a stream sync every 8th sample.
        if (verbose && ((s & 7) == 0 || s == endSample - 1)) {
            cudaMemcpyAsync(pinnedProgress, gpuProgress, sizeof(unsigned int),
                            cudaMemcpyDeviceToHost, progStream);
            cudaStreamSynchronize(progStream);
            bar.update(long(*pinnedProgress));
        }
    }
    ctx.state_->nextSample.store(startSample + completed, std::memory_order_release);

    // =============== finalize ===============
    wfFinalizeKernel<<<pxBlocks, threads>>>(d_accum, pixelCount, sppBudgetTotal,
                                            gpuColor, gpuDD, gpuDS, gpuID, gpuIS, gpuEm,
                                            gpuVarDd, gpuVarDs, gpuVarId, gpuVarIs,
                                            gpuBase, gpuNrm, gpuPos, gpuGeomNrm, gpuMat,
                                            d_adaptStats, gpuSunMask);
    cudaStreamSynchronize(0);
    if (verbose) bar.update(barTotal);

    cudaError_t err = cudaGetLastError();
    if (err != cudaSuccess) {
        fprintf(stderr, "  CUDA ERROR after wavefront: %s\n", cudaGetErrorString(err)); fflush(stderr);
    }

    // Adaptive sampling statistics
    g_lastAvgSpp = double(args.spp);
    if (d_adaptStats) {
        unsigned long long st[4] = {0, 0, 0, 0};
        cudaMemcpy(st, d_adaptStats, sizeof(st), cudaMemcpyDeviceToHost);
        const double avg = pixelCount > 0 ? double(st[0]) / pixelCount : 0.0;
        g_lastAvgSpp = avg;
        fprintf(stderr, "ADAPTSTAT: avg=%.1f min=%llu max=%llu budget=%d earlyStop=%llu/%d (%.1f%%)\n",
                avg, st[1] == ~0ULL ? 0 : st[1], st[2], sppBudgetTotal,
                st[3], pixelCount, 100.0 * st[3] / (pixelCount > 0 ? pixelCount : 1));
        fflush(stderr);
    }

    // ---- Read back results (identical layout to the megakernel host) ----
    std::vector<float4> colorBuf(pixelCount);
    std::vector<float4> ddBuf(pixelCount), dsBuf(pixelCount);
    std::vector<float4> idBuf(pixelCount), isBuf(pixelCount);
    std::vector<float4> emBuf(pixelCount);
    std::vector<float> varDdBuf(pixelCount), varDsBuf(pixelCount);
    std::vector<float> varIdBuf(pixelCount), varIsBuf(pixelCount);

    cudaMemcpy(colorBuf.data(), gpuColor, pixelCount * sizeof(float4), cudaMemcpyDeviceToHost);
    cudaMemcpy(ddBuf.data(),    gpuDD,    pixelCount * sizeof(float4), cudaMemcpyDeviceToHost);
    cudaMemcpy(dsBuf.data(),    gpuDS,    pixelCount * sizeof(float4), cudaMemcpyDeviceToHost);
    cudaMemcpy(idBuf.data(),    gpuID,    pixelCount * sizeof(float4), cudaMemcpyDeviceToHost);
    cudaMemcpy(isBuf.data(),    gpuIS,    pixelCount * sizeof(float4), cudaMemcpyDeviceToHost);
    cudaMemcpy(emBuf.data(),    gpuEm,    pixelCount * sizeof(float4), cudaMemcpyDeviceToHost);
    std::vector<float4> baseBuf(pixelCount), nrmBuf(pixelCount);
    std::vector<float4> posBuf(pixelCount), geomNrmBuf(pixelCount), matBuf(pixelCount);
    cudaMemcpy(baseBuf.data(),  gpuBase,  pixelCount * sizeof(float4), cudaMemcpyDeviceToHost);
    cudaMemcpy(nrmBuf.data(),   gpuNrm,   pixelCount * sizeof(float4), cudaMemcpyDeviceToHost);
    cudaMemcpy(posBuf.data(),   gpuPos,   pixelCount * sizeof(float4), cudaMemcpyDeviceToHost);
    cudaMemcpy(geomNrmBuf.data(), gpuGeomNrm, pixelCount * sizeof(float4), cudaMemcpyDeviceToHost);
    cudaMemcpy(matBuf.data(),   gpuMat,   pixelCount * sizeof(float4), cudaMemcpyDeviceToHost);
    std::vector<float> sunMaskBuf(pixelCount);
    cudaMemcpy(sunMaskBuf.data(), gpuSunMask, pixelCount * sizeof(float), cudaMemcpyDeviceToHost);
    cudaMemcpy(varDdBuf.data(), gpuVarDd, pixelCount * sizeof(float), cudaMemcpyDeviceToHost);
    cudaMemcpy(varDsBuf.data(), gpuVarDs, pixelCount * sizeof(float), cudaMemcpyDeviceToHost);
    cudaMemcpy(varIdBuf.data(), gpuVarId, pixelCount * sizeof(float), cudaMemcpyDeviceToHost);
    cudaMemcpy(varIsBuf.data(), gpuVarIs, pixelCount * sizeof(float), cudaMemcpyDeviceToHost);

    // Convert to PerPixelData. The AOVSTAT scan is O(5n) per chunk —
    // RENDER_VERBOSE only (the delivery-signature check uses the CLI path).
    if (verbose) {
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
        output[i].var_Dd = varDdBuf[i];
        output[i].var_Ds = varDsBuf[i];
        output[i].var_Id = varIdBuf[i];
        output[i].var_Is = varIsBuf[i];
    }

    // Persistent device buffers remain owned by WavefrontBuffers and are
    // reused by the next render at the same or smaller resolution (批1: the
    // progress/pinned/stream infrastructure too — no per-call teardown).
    fprintf(stderr, "renderGPUWavefront: done\n"); fflush(stderr);
}
