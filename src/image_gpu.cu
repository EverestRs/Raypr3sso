// ============================================================================
// GPU post-processing pipeline (RENDER_POST_GPU=1) — see image_gpu.h
// ============================================================================
// Full CUDA mirror of Photo::postProcess (image.cpp). Pixel passes are
// kernels with the same ping-pong buffer semantics as the CPU code; the
// strided statistics use the SAME strides and host-side nth_element medians.
// Acceptance criterion: statistical equivalence (ulp-level FP differences
// vs. the CPU build are expected and logged).
//
// NOTE: FILTER_PASSES here must mirror image.cpp (duplicated on purpose —
// image.cpp keeps it file-static for the CPU path).
#include "image_gpu.h"
#include <cuda_runtime.h>
#include <cmath>
#include <cstring>
#include <cstdlib>
#include <cstdio>
#include <algorithm>
#include <vector>
#include <chrono>
#ifdef RENDERER_HAS_OIDN
#include <OpenImageDenoise/oidn.h>
#endif

static constexpr int FILTER_PASSES = 5;  // Steps {1,2,4,8,16} like reference — keep in sync with image.cpp

// ---------------------------------------------------------------------------
// Layout: mirror the CPU-side structures. RadianceData (image.h) is
// {glm::vec3 radiance; float Var} = 16 contiguous bytes — glm::vec3 is a
// plain 3-float struct, so the CUDA mirror below memcpy-maps onto it.
// ---------------------------------------------------------------------------
struct RadG  { float3 r; float Var; };
struct GbufG { float3 position, shapeNormal, surfaceNormal, baseColor;
                float roughness, metallic, specular, opacity; };

static bool layoutOk() {
    return sizeof(RadianceData) == sizeof(RadG) && sizeof(float4) == 16;
}

#define CUDA_CHECK(call)                                                       \
    do { cudaError_t e_ = (call);                                              \
         if (e_ != cudaSuccess) {                                              \
             fprintf(stderr, "POSTGPU CUDA error %s at %s:%d (%s)\n",          \
                     cudaGetErrorString(e_), __FILE__, __LINE__, #call);       \
             fflush(stderr); return false; } } while (0)

// Re-allocating buffer: frees a previous allocation first (buffers are reused
// across pipeline stages with different widths, e.g. float3 bloom vs float4
// despeckle scratch).
struct DevBuf {
    void* p = nullptr; size_t bytes = 0;
    bool alloc(size_t b) {
        if (p && bytes >= b) return true;
        if (p) { cudaFree(p); p = nullptr; }
        bytes = b;
        return b == 0 || cudaMalloc(&p, b) == cudaSuccess;
    }
    ~DevBuf() { if (p) cudaFree(p); }
};

static inline unsigned ceilDiv(size_t a, size_t b) {
    return (unsigned)((a + b - 1) / b);
}
static const int TPB = 256;

__device__ __forceinline__ int imin(int a, int b) { return a < b ? a : b; }
__device__ __forceinline__ int imax(int a, int b) { return a > b ? a : b; }

static bool envIs(const char* name, const char* val) {
    const char* e = std::getenv(name);
    return e && std::strcmp(e, val) == 0;
}
static bool envFlag1(const char* name) {
    const char* e = std::getenv(name);
    return e && e[0] == '1';
}

__device__ __forceinline__ float devLum(const float3& c) {
    return c.x * 0.299f + c.y * 0.587f + c.z * 0.114f;
}
__device__ __forceinline__ float3 devLum3(const float4& c) {
    return make_float3(c.x, c.y, c.z);
}

// ---------------------------------------------------------------------------
// OIDN: unified RT-filter run (RENDER_OIDN_DEVICE=cpu|gpu, see image_gpu.h)
// ---------------------------------------------------------------------------
#ifdef RENDERER_HAS_OIDN
bool oidnRunRT(const std::vector<float>& color, const std::vector<float>& albedo,
               const std::vector<float>& normal, std::vector<float>& out,
               int width, int height) {
    const size_t bytes = size_t(width) * height * 3 * sizeof(float);
    const bool wantGpu = envIs("RENDER_OIDN_DEVICE", "gpu") ||
                         envIs("RENDER_OIDN_DEVICE", "cuda");

    if (wantGpu) {
        OIDNDevice dev = oidnNewDevice(OIDN_DEVICE_TYPE_CUDA);
        if (dev) {
            oidnCommitDevice(dev);
            const char* err = nullptr;
            if (oidnGetDeviceError(dev, &err) == OIDN_ERROR_NONE) {
                // The CUDA backend cannot dereference plain malloc'd host
                // memory ("image data not accessible by the device") — stage
                // through OIDN device buffers and cudaMemcpy in/out. UVA makes
                // the device pointers addressable from the runtime context.
                OIDNFilter flt = oidnNewFilter(dev, "RT");
                OIDNBuffer bc = oidnNewBuffer(dev, bytes);
                OIDNBuffer ba = oidnNewBuffer(dev, bytes);
                OIDNBuffer bn = oidnNewBuffer(dev, bytes);
                OIDNBuffer bo = oidnNewBuffer(dev, bytes);
                if (flt && bc && ba && bn && bo) {
                    if (cudaMemcpy(oidnGetBufferData(bc), color.data(), bytes,
                                   cudaMemcpyHostToDevice) == cudaSuccess &&
                        cudaMemcpy(oidnGetBufferData(ba), albedo.data(), bytes,
                                   cudaMemcpyHostToDevice) == cudaSuccess &&
                        cudaMemcpy(oidnGetBufferData(bn), normal.data(), bytes,
                                   cudaMemcpyHostToDevice) == cudaSuccess) {
                        oidnSetFilterImage(flt, "color",  bc, OIDN_FORMAT_FLOAT3, width, height, 0, 0, 0);
                        oidnSetFilterImage(flt, "albedo", ba, OIDN_FORMAT_FLOAT3, width, height, 0, 0, 0);
                        oidnSetFilterImage(flt, "normal", bn, OIDN_FORMAT_FLOAT3, width, height, 0, 0, 0);
                        oidnSetFilterImage(flt, "output", bo, OIDN_FORMAT_FLOAT3, width, height, 0, 0, 0);
                        oidnSetFilterBool(flt, "hdr", true);
                        oidnCommitFilter(flt);
                        oidnExecuteFilter(flt);
                        err = nullptr;
                        if (oidnGetDeviceError(dev, &err) == OIDN_ERROR_NONE) {
                            cudaDeviceSynchronize();
                            if (cudaMemcpy(out.data(), oidnGetBufferData(bo), bytes,
                                           cudaMemcpyDeviceToHost) == cudaSuccess) {
                                fprintf(stderr, "OIDN device: CUDA (device-buffer staging)\n");
                                fflush(stderr);
                                oidnReleaseFilter(flt);
                                oidnReleaseBuffer(bc); oidnReleaseBuffer(ba);
                                oidnReleaseBuffer(bn); oidnReleaseBuffer(bo);
                                oidnReleaseDevice(dev);
                                return true;
                            }
                            fprintf(stderr, "OIDN: CUDA result copy failed — falling back to CPU\n");
                        } else {
                            fprintf(stderr, "OIDN: CUDA filter failed (%s) — falling back to CPU\n",
                                    err ? err : "?");
                        }
                    } else {
                        fprintf(stderr, "OIDN: CUDA staging failed — falling back to CPU\n");
                    }
                } else {
                    fprintf(stderr, "OIDN: CUDA buffer/filter allocation failed — falling back to CPU\n");
                }
                if (flt) oidnReleaseFilter(flt);
                if (bc) oidnReleaseBuffer(bc);
                if (ba) oidnReleaseBuffer(ba);
                if (bn) oidnReleaseBuffer(bn);
                if (bo) oidnReleaseBuffer(bo);
            } else {
                fprintf(stderr, "OIDN: CUDA device commit failed (%s) — falling back to CPU\n",
                        err ? err : "?");
            }
            oidnReleaseDevice(dev);
        } else {
            const char* err = nullptr;
            oidnGetDeviceError(nullptr, &err);
            fprintf(stderr, "OIDN: CUDA device unavailable (%s) — falling back to CPU\n",
                    err ? err : "?");
        }
        fflush(stderr);
    }

    // CPU device — the historical shared-image path (byte-identical default).
    OIDNDevice dev = oidnNewDevice(OIDN_DEVICE_TYPE_CPU);
    if (!dev) return false;
    oidnCommitDevice(dev);
    fprintf(stderr, "OIDN device: CPU\n"); fflush(stderr);
    OIDNFilter flt = oidnNewFilter(dev, "RT");
    oidnSetSharedFilterImage(flt, "color",  (void*)color.data(),  OIDN_FORMAT_FLOAT3, width, height, 0, 3*sizeof(float), 0);
    oidnSetSharedFilterImage(flt, "albedo", (void*)albedo.data(), OIDN_FORMAT_FLOAT3, width, height, 0, 3*sizeof(float), 0);
    oidnSetSharedFilterImage(flt, "normal", (void*)normal.data(), OIDN_FORMAT_FLOAT3, width, height, 0, 3*sizeof(float), 0);
    oidnSetSharedFilterImage(flt, "output", (void*)out.data(),    OIDN_FORMAT_FLOAT3, width, height, 0, 3*sizeof(float), 0);
    oidnSetFilterBool(flt, "hdr", true);
    oidnCommitFilter(flt);
    oidnExecuteFilter(flt);
    const char* err = nullptr;
    const bool ok = oidnGetDeviceError(dev, &err) == OIDN_ERROR_NONE;
    if (!ok) fprintf(stderr, "OIDN error: %s\n", err ? err : "?");
    oidnReleaseFilter(flt);
    oidnReleaseDevice(dev);
    return ok;
}
#endif // RENDERER_HAS_OIDN

// ---------------------------------------------------------------------------
// Kernels — statistics
// ---------------------------------------------------------------------------
__global__ void kCompositeLumSample(const RadG* a, const RadG* b, const RadG* c,
                                    const RadG* d, int n, int stride, float* out) {
    const int i = blockIdx.x * blockDim.x + threadIdx.x;
    const size_t px = size_t(i) * stride;
    if (px >= size_t(n)) return;
    out[i] = devLum(a[px].r) + devLum(b[px].r) + devLum(c[px].r) + devLum(d[px].r);
}

__global__ void kLumReduce(const RadG* a, const RadG* b, const RadG* c, const RadG* d,
                           int n, float3* partial) {
    __shared__ float3 sh[TPB];
    const int t = threadIdx.x;
    const size_t idx = size_t(blockIdx.x) * blockDim.x + t;
    sh[t] = (idx < size_t(n))
        ? make_float3(devLum(a[idx].r) + devLum(b[idx].r) +
                      devLum(c[idx].r) + devLum(d[idx].r), 0.0f, 0.0f)
        : make_float3(0.0f, 0.0f, 0.0f);
    __syncthreads();
    for (int s = blockDim.x / 2; s > 0; s >>= 1) {
        if (t < s) sh[t] = make_float3(sh[t].x + sh[t + s].x, 0.0f, 0.0f);
        __syncthreads();
    }
    if (t == 0) partial[blockIdx.x] = sh[0];
}

__global__ void kSampleVar(const RadG* a, int n, int stride, float* out) {
    const int i = blockIdx.x * blockDim.x + threadIdx.x;
    const size_t px = size_t(i) * stride;
    if (px >= size_t(n)) return;
    out[i] = a[px].Var;
}

__global__ void kSampleLum1(const RadG* a, int n, int stride, float* out) {
    const int i = blockIdx.x * blockDim.x + threadIdx.x;
    const size_t px = size_t(i) * stride;
    if (px >= size_t(n)) return;
    out[i] = devLum(a[px].r);
}

// ---------------------------------------------------------------------------
// Kernels — clamp / scale / variance
// ---------------------------------------------------------------------------
__global__ void kSpatialClamp(RadG* a, int n, float maxLuma) {
    const size_t i = size_t(blockIdx.x) * blockDim.x + threadIdx.x;
    if (i >= size_t(n)) return;
    const float lum = devLum(a[i].r);
    if (lum > maxLuma) {
        const float s = maxLuma / lum;
        a[i].r = a[i].r * s;
        a[i].Var *= s * s;
    }
}

__global__ void kSnapshotMcVar(const RadG* a, float* mcVar, int n) {
    const size_t i = size_t(blockIdx.x) * blockDim.x + threadIdx.x;
    if (i >= size_t(n)) return;
    mcVar[i] = a[i].Var;
}

__global__ void kFilterVar(const RadG* a, int width, int height, float* varEst) {
    const size_t i = size_t(blockIdx.x) * blockDim.x + threadIdx.x;
    const int n = width * height;
    if (i >= size_t(n)) return;
    const int x = int(i) % width, y = int(i) / width;
    const float h[3] = {0.25f, 0.5f, 0.25f};
    float3 E = make_float3(0.0f, 0.0f, 0.0f);
    float E2 = 0.0f, Var = 0.0f;
    for (int dy = -1; dy <= 1; ++dy)
        for (int dx = -1; dx <= 1; ++dx) {
            const int nx = x + dx, ny = y + dy;
            if (nx < 0 || nx >= width || ny < 0 || ny >= height) continue;
            const RadG& Lq = a[size_t(ny) * width + nx];
            const float w = h[dx + 1] * h[dy + 1];
            E = E + Lq.r * w;
            E2 += (Lq.r.x * Lq.r.x + Lq.r.y * Lq.r.y + Lq.r.z * Lq.r.z) * w;
            Var += Lq.Var * w;
        }
    varEst[i] = Var + E2 - (E.x * E.x + E.y * E.y + E.z * E.z);
}

__global__ void kAssignVar(RadG* a, const float* varEst, int n) {
    const size_t i = size_t(blockIdx.x) * blockDim.x + threadIdx.x;
    if (i >= size_t(n)) return;
    a[i].Var = varEst[i];
}

__global__ void kLumField(const RadG* a, int n, float* lum) {
    const size_t i = size_t(blockIdx.x) * blockDim.x + threadIdx.x;
    if (i >= size_t(n)) return;
    lum[i] = devLum(a[i].r);
}

__global__ void kFireflyClamp(RadG* a, const float* lum, int width, int height,
                              float gate) {
    const size_t i = size_t(blockIdx.x) * blockDim.x + threadIdx.x;
    const int n = width * height;
    if (i >= size_t(n)) return;
    if (lum[i] <= 0.0f) return;
    const int x = int(i) % width, y = int(i) / width;
    float nb[9]; float sum = 0.0f; int cnt = 0;
    for (int dy = -1; dy <= 1; ++dy)
        for (int dx = -1; dx <= 1; ++dx) {
            const int sx = imin(width - 1, imax(0, x + dx));
            const int sy = imin(height - 1, imax(0, y + dy));
            const float l = lum[size_t(sy) * width + sx];
            nb[cnt++] = l; sum += l;
        }
    // 9-element insertion sort (same as the CPU med9)
    for (int k = 1; k < 9; ++k) {
        const float key = nb[k]; int j = k - 1;
        while (j >= 0 && nb[j] > key) { nb[j + 1] = nb[j]; --j; }
        nb[j + 1] = key;
    }
    float cap = gate * fmaxf(nb[4], 1e-3f);
    cap = fmaxf(cap, gate * sum / 9.0f);
    if (lum[i] > cap) {
        const float s = cap / lum[i];
        a[i].r = a[i].r * s;
        a[i].Var *= s * s;
    }
}

__global__ void kScaleAov(RadG* a, int n, float s) {
    const size_t i = size_t(blockIdx.x) * blockDim.x + threadIdx.x;
    if (i >= size_t(n)) return;
    a[i].r = a[i].r * s;
    a[i].Var *= s * s;
}

__global__ void kScaleMcVar(float* mcVar, int n, float s2) {
    const size_t i = size_t(blockIdx.x) * blockDim.x + threadIdx.x;
    if (i >= size_t(n)) return;
    mcVar[i] *= s2;
}

// ---------------------------------------------------------------------------
// Kernels — NL-means filterRadiance
// ---------------------------------------------------------------------------
__global__ void kLogTemp(const RadG* a, int n, RadG* temp) {
    const size_t i = size_t(blockIdx.x) * blockDim.x + threadIdx.x;
    if (i >= size_t(n)) return;
    const float3 r = a[i].r;
    temp[i].r = make_float3(logf(1.0f + r.x), logf(1.0f + r.y), logf(1.0f + r.z));
    temp[i].Var = a[i].Var;
}

__global__ void kLm(const RadG* temp, int width, int height, float3* Lm) {
    const size_t i = size_t(blockIdx.x) * blockDim.x + threadIdx.x;
    const int n = width * height;
    if (i >= size_t(n)) return;
    const int x = int(i) % width, y = int(i) / width;
    float3 acc = make_float3(0.0f, 0.0f, 0.0f); int cnt = 0;
    for (int dy = -1; dy <= 1; ++dy)
        for (int dx = -1; dx <= 1; ++dx) {
            const int sx = imin(width - 1, imax(0, x + dx));
            const int sy = imin(height - 1, imax(0, y + dy));
            acc = acc + temp[size_t(sy) * width + sx].r;
            ++cnt;
        }
    Lm[i] = acc / float(cnt);
}

__global__ void kLumLin(const RadG* a, int n, float* lumLin) {
    const size_t i = size_t(blockIdx.x) * blockDim.x + threadIdx.x;
    if (i >= size_t(n)) return;
    lumLin[i] = devLum(a[i].r);
}

// Edge-aware weight — exact mirror of getDenoiseWeight (image.cpp).
__device__ float devDenoiseWeight(const GbufG& Gp, const GbufG& Gq,
                                  const RadG& Lp, const RadG& Lq,
                                  const float3& LmP, const float3& LmQ,
                                  float lumLP, float lumLQ, float varMed,
                                  bool specular) {
    const float sigma_z = 1.0f, sigma_n = 1024.0f, sigma_l = 1.0f, sigma_m = 1.0f;
    const float eps = 1e-2f, eps_r = 4e-2f, eps_zero = 1e-6f;
    (void)lumLQ;   // CPU signature parity

    if (!isfinite(Gq.position.x)) return 0.0f;

    const float3 Np = specular ? Gp.surfaceNormal : Gp.shapeNormal;
    const float3 Nq = specular ? Gq.surfaceNormal : Gq.shapeNormal;
    const float dotN = fmaxf(Np.x * Nq.x + Np.y * Nq.y + Np.z * Nq.z, 0.0f);
    float weight = powf(dotN, sigma_n);
    if (weight < 1e-6f) return 0.0f;

    float k = 0.0f;

    float3 dir = Gq.position - Gp.position;
    const float dirLen = sqrtf(dir.x * dir.x + dir.y * dir.y + dir.z * dir.z);
    if (dirLen > 1e-8f) {
        dir = dir / dirLen;
        const float sinTheta = fabsf(Gp.shapeNormal.x * dir.x +
                                     Gp.shapeNormal.y * dir.y +
                                     Gp.shapeNormal.z * dir.z);
        const float cosTheta = sqrtf(fmaxf(1.0f - sinTheta * sinTheta, 0.0f));
        const float tanTheta = sinTheta / (cosTheta + eps_zero);
        k += -tanTheta / sigma_z;
    }

    const float varCapped = fminf(fmaxf(Lp.Var, 0.0f), 512.0f * varMed);
    const float sigmaL = fmaxf(sigma_l * sqrtf(varCapped) / 3.0f,
                       fmaxf(0.05f * lumLP, 0.10f * sqrtf(varMed))) + eps;
    const float3 dLm = LmP - LmQ;
    k += -sqrtf(dLm.x * dLm.x + dLm.y * dLm.y + dLm.z * dLm.z) / sigmaL;

    const float3 dM = make_float3(Gp.metallic - Gq.metallic,
                                  Gp.specular - Gq.specular,
                                  Gp.opacity  - Gq.opacity);
    k += -sqrtf(dM.x * dM.x + dM.y * dM.y + dM.z * dM.z) / sigma_m;

    const float3 dA = Gp.baseColor - Gq.baseColor;
    const float aLenP = sqrtf(Gp.baseColor.x * Gp.baseColor.x +
                              Gp.baseColor.y * Gp.baseColor.y +
                              Gp.baseColor.z * Gp.baseColor.z);
    const float albedoDiff =
        sqrtf(dA.x * dA.x + dA.y * dA.y + dA.z * dA.z) / (aLenP + 0.02f);
    k += -albedoDiff * albedoDiff / (2.0f * 0.15f * 0.15f);

    if (k < -7.5f) return 0.0f;
    weight *= expf(k);

    if (specular) weight *= fmaxf(Gp.roughness, eps_r);
    if (!isfinite(weight)) return 0.0f;
    return weight;
}

// One filterRadiance pass for one AOV: main 5x5 loop with adaptive step
// (kernel mirror of filterRadiance's parallelForRows body).
__global__ void kFilterMain(const RadG* temp, const GbufG* G, const float3* Lm,
                            const float* lumLin, int width, int height,
                            bool specular, int pass, float varMed,
                            float3* radBuf, float* varBuf) {
    const size_t i = size_t(blockIdx.x) * blockDim.x + threadIdx.x;
    const int n = width * height;
    if (i >= size_t(n)) return;
    const int x = int(i) % width, y = int(i) / width;
    constexpr int R = 2;
    const float h[5] = {0.0625f, 0.25f, 0.375f, 0.25f, 0.0625f};
    const GbufG& Gp = G[i];
    const RadG& Lp = temp[i];
    if (!isfinite(Gp.position.x)) { radBuf[i] = Lp.r; varBuf[i] = Lp.Var; return; }

    int step = 1 << pass;
    if (Lp.Var > 12.0f * varMed)     step = imin(step * 4, 16);
    else if (Lp.Var > 3.0f * varMed) step = imin(step * 2, 16);

    float3 sumRad = make_float3(0.0f, 0.0f, 0.0f);
    float sumW = 0.0f, sumVar = 0.0f;
    for (int dy = -R; dy <= R; ++dy)
        for (int dx = -R; dx <= R; ++dx) {
            const int nx = x + dx * step, ny = y + dy * step;
            if (nx < 0 || nx >= width || ny < 0 || ny >= height) continue;
            const size_t nidx = size_t(ny) * width + nx;
            const RadG& Lq = temp[nidx];
            float w = h[dx + R] * h[dy + R];
            if (dx != 0 || dy != 0)
                w *= devDenoiseWeight(Gp, G[nidx], Lp, Lq,
                                      Lm[i], Lm[nidx],
                                      lumLin[i], lumLin[nidx], varMed, specular);
            sumRad = sumRad + Lq.r * w;
            sumVar += Lq.Var * w * w;
            sumW += w;
        }
    if (sumW > 1e-8f) {
        radBuf[i] = sumRad / sumW;
        varBuf[i] = sumVar / (sumW * sumW);
    } else {
        radBuf[i] = Lp.r;
        varBuf[i] = Lp.Var;
    }
}

// Write-back: exp inverse transform + mcVar noise-guided blend.
__global__ void kWriteBack(RadG* a, const RadG* origIn, const RadG* temp,
                           const float3* radBuf, const float* varBuf,
                           const float* mcVar, const float* lumLin,
                           int n, float noiseTarget) {
    const size_t i = size_t(blockIdx.x) * blockDim.x + threadIdx.x;
    if (i >= size_t(n)) return;
    float3 filtered = make_float3(expf(radBuf[i].x) - 1.0f,
                                  expf(radBuf[i].y) - 1.0f,
                                  expf(radBuf[i].z) - 1.0f);
    const float3 o = origIn[i].r;
    if (noiseTarget > 0.0f) {
        const float vMC = mcVar ? mcVar[i] : temp[i].Var;
        const float noiseFrac = sqrtf(fmaxf(vMC, 0.0f)) / (lumLin[i] + 1e-4f);
        float blend = (noiseFrac - 0.4f * noiseTarget) / (1.2f * noiseTarget);
        blend = fminf(fmaxf(blend, 0.0f), 1.0f);
        filtered = o + (filtered - o) * blend;
    }
    a[i].r = filtered;
    a[i].Var = varBuf[i];
}

// ---------------------------------------------------------------------------
// Kernels — AO / detail / energy gain
// ---------------------------------------------------------------------------
__global__ void kAO(const GbufG* G, int width, int height,
                    float radius, float strength, int taps, float* ao) {
    const size_t i = size_t(blockIdx.x) * blockDim.x + threadIdx.x;
    const int n = width * height;
    if (i >= size_t(n)) return;
    const int x = int(i) % width, y = int(i) / width;
    const float3 pp = G[i].position;
    if (!isfinite(pp.x) ||
        (pp.x * pp.x + pp.y * pp.y + pp.z * pp.z) < 1e-12f) { ao[i] = 1.0f; return; }
    const float3 N = G[i].shapeNormal;
    const uint32_t hsh = uint32_t(x * 73856093) ^ uint32_t(y * 19349663);
    const float rot = (hsh & 1023u) * (2.0f * 3.14159265358979323846f / 1024.0f);
    float occ = 0.0f, wsum = 0.0f;
    for (int t = 0; t < taps; ++t) {
        const float ang = rot + t * (2.0f * 3.14159265358979323846f / taps) * 2.4f;
        const float rr = radius * sqrtf((t + 0.5f) / taps);
        const int sx = x + int(rr * cosf(ang));
        const int sy = y + int(rr * sinf(ang));
        if (sx < 0 || sx >= width || sy < 0 || sy >= height) continue;
        const float3 pq = G[size_t(sy) * width + sx].position;
        if (!isfinite(pq.x) ||
            (pq.x * pq.x + pq.y * pq.y + pq.z * pq.z) < 1e-12f) continue;
        const float3 diff = pq - pp;
        const float dist = sqrtf(diff.x * diff.x + diff.y * diff.y + diff.z * diff.z);
        if (dist > radius * 2.0f || dist < 1e-5f) continue;
        const float proj = (diff.x * N.x + diff.y * N.y + diff.z * N.z) / dist;
        float w = fmaxf(0.0f, proj - 0.15f) / 0.85f;
        w *= 1.0f - fminf(1.0f, dist / (radius * 2.0f));
        occ += w; wsum += 1.0f;
    }
    if (wsum > 0.0f) occ /= wsum;
    ao[i] = 1.0f - strength * fminf(1.0f, occ * 2.0f);
}

__global__ void kMulAOIndirect(RadG* aId, RadG* aIs, const float* ao, int n) {
    const size_t i = size_t(blockIdx.x) * blockDim.x + threadIdx.x;
    if (i >= size_t(n)) return;
    aId[i].r = aId[i].r * ao[i];
    aIs[i].r = aIs[i].r * ao[i];
}

__global__ void kMulAOOidn(float* oidn, const float* ao,
                           const RadG* aDd, const RadG* aDs,
                           const RadG* aId, const RadG* aIs,
                           const float4* emission, int n) {
    const size_t i = size_t(blockIdx.x) * blockDim.x + threadIdx.x;
    if (i >= size_t(n)) return;
    const float idl = devLum(aId[i].r) + devLum(aIs[i].r);
    const float emL = emission ? devLum(devLum3(emission[i])) : 0.0f;
    const float tot = idl + devLum(aDd[i].r) + devLum(aDs[i].r) + emL;
    const float frac = (tot > 1e-6f) ? fminf(1.0f, idl / tot) : 0.0f;
    const float m = 1.0f - (1.0f - ao[i]) * frac;
    oidn[i*3+0] *= m; oidn[i*3+1] *= m; oidn[i*3+2] *= m;
}

__global__ void kAlbBlur(const GbufG* G, int width, int height, float3* albBlur) {
    const size_t i = size_t(blockIdx.x) * blockDim.x + threadIdx.x;
    const int n = width * height;
    if (i >= size_t(n)) return;
    const int x = int(i) % width, y = int(i) / width;
    float3 acc = make_float3(0.0f, 0.0f, 0.0f); int cnt = 0;
    for (int dy = -1; dy <= 1; ++dy)
        for (int dx = -1; dx <= 1; ++dx) {
            const int sx = imin(width - 1, imax(0, x + dx));
            const int sy = imin(height - 1, imax(0, y + dy));
            acc = acc + G[size_t(sy) * width + sx].baseColor;
            ++cnt;
        }
    albBlur[i] = acc / float(cnt);
}

__global__ void kDetailInject(RadG* aDd, RadG* aDs, RadG* aId, RadG* aIs,
                              const GbufG* G, const float3* albBlur,
                              int n, float gain) {
    const size_t i = size_t(blockIdx.x) * blockDim.x + threadIdx.x;
    if (i >= size_t(n)) return;
    const float3 alb = G[i].baseColor;
    const float3 hf = alb - albBlur[i];
    const float3 fac = make_float3(fmaxf(1.0f + gain * hf.x, 0.0f),
                                   fmaxf(1.0f + gain * hf.y, 0.0f),
                                   fmaxf(1.0f + gain * hf.z, 0.0f));
    aDd[i].r = aDd[i].r * fac;
    aDs[i].r = aDs[i].r * fac;
    aId[i].r = aId[i].r * fac;
    aIs[i].r = aIs[i].r * fac;
}

__global__ void kDNLumReduceOidn(const float* oidn, int n, float3* partial) {
    __shared__ float3 sh[TPB];
    const int t = threadIdx.x;
    const size_t idx = size_t(blockIdx.x) * blockDim.x + t;
    sh[t] = (idx < size_t(n))
        ? make_float3(oidn[idx*3+0] * 0.299f + oidn[idx*3+1] * 0.587f +
                      oidn[idx*3+2] * 0.114f, 0.0f, 0.0f)
        : make_float3(0.0f, 0.0f, 0.0f);
    __syncthreads();
    for (int s = blockDim.x / 2; s > 0; s >>= 1) {
        if (t < s) sh[t] = make_float3(sh[t].x + sh[t + s].x, 0.0f, 0.0f);
        __syncthreads();
    }
    if (t == 0) partial[blockIdx.x] = sh[0];
}

__global__ void kDNLumSampleOidn(const float* oidn, int n, int stride, float* out) {
    const int i = blockIdx.x * blockDim.x + threadIdx.x;
    const size_t px = size_t(i) * stride;
    if (px >= size_t(n)) return;
    out[i] = oidn[px*3+0] * 0.299f + oidn[px*3+1] * 0.587f + oidn[px*3+2] * 0.114f;
}

__global__ void kGainAov(RadG* a, int n, float g) {
    const size_t i = size_t(blockIdx.x) * blockDim.x + threadIdx.x;
    if (i >= size_t(n)) return;
    a[i].r = a[i].r * g;
    a[i].Var *= g * g;
}

__global__ void kGainOidn(float* oidn, int n, float g) {
    const size_t i = size_t(blockIdx.x) * blockDim.x + threadIdx.x;
    if (i >= size_t(n)) return;
    oidn[i*3+0] *= g; oidn[i*3+1] *= g; oidn[i*3+2] *= g;
}

// ---------------------------------------------------------------------------
// Kernels — OIDN input packing / sunliness / emission / composite
// ---------------------------------------------------------------------------
__global__ void kOIDNPack(const RadG* aDd, const RadG* aDs, const RadG* aId,
                          const RadG* aIs, const float4* emission,
                          const GbufG* G, int n,
                          float* color, float* albedo, float* normal) {
    const size_t i = size_t(blockIdx.x) * blockDim.x + threadIdx.x;
    if (i >= size_t(n)) return;
    float3 c = aDd[i].r + aDs[i].r + aId[i].r + aIs[i].r;
    if (emission) c = c + devLum3(emission[i]);
    color[i*3+0] = c.x; color[i*3+1] = c.y; color[i*3+2] = c.z;
    albedo[i*3+0] = G[i].baseColor.x;
    albedo[i*3+1] = G[i].baseColor.y;
    albedo[i*3+2] = G[i].baseColor.z;
    normal[i*3+0] = G[i].shapeNormal.x;
    normal[i*3+1] = G[i].shapeNormal.y;
    normal[i*3+2] = G[i].shapeNormal.z;
}

__global__ void kSunlinessBlur(const float* sunMask, int width, int height,
                               float* mb) {
    const size_t i = size_t(blockIdx.x) * blockDim.x + threadIdx.x;
    const int n = width * height;
    if (i >= size_t(n)) return;
    const int x = int(i) % width, y = int(i) / width;
    float acc = 0.0f; int cnt = 0;
    for (int dy = -1; dy <= 1; ++dy)
        for (int dx = -1; dx <= 1; ++dx) {
            const int sx = imin(width - 1, imax(0, x + dx));
            const int sy = imin(height - 1, imax(0, y + dy));
            acc += sunMask[size_t(sy) * width + sx];
            ++cnt;
        }
    mb[i] = acc / float(cnt);
}

__global__ void kSunlinessNorm(const float* mb, const RadG* aDd, const RadG* aDs,
                               const RadG* aId, const RadG* aIs,
                               const float4* emission, int n, float* sunliness) {
    const size_t i = size_t(blockIdx.x) * blockDim.x + threadIdx.x;
    if (i >= size_t(n)) return;
    const float tot = devLum(aDd[i].r) + devLum(aDs[i].r) + devLum(aId[i].r) +
                      devLum(aIs[i].r) +
                      (emission ? devLum(devLum3(emission[i])) : 0.0f);
    sunliness[i] = fminf(1.0f, mb[i] / (tot + 1e-4f));
}

__global__ void kEmissionDenoise(const float4* src, const GbufG* G,
                                 int width, int height, float4* dst) {
    const size_t i = size_t(blockIdx.x) * blockDim.x + threadIdx.x;
    const int n = width * height;
    if (i >= size_t(n)) return;
    const int x = int(i) % width, y = int(i) / width;
    const GbufG& Gp = G[i];
    float4 sum = make_float4(0.0f, 0.0f, 0.0f, 0.0f);
    float totalW = 0.0f;
    for (int dy = -2; dy <= 2; ++dy)
        for (int dx = -2; dx <= 2; ++dx) {
            const int sx = x + dx, sy = y + dy;
            if (sx < 0 || sx >= width || sy < 0 || sy >= height) continue;
            const size_t sidx = size_t(sy) * width + sx;
            const GbufG& Gq = G[sidx];
            const float dn = 1.0f - (Gp.surfaceNormal.x * Gq.surfaceNormal.x +
                                     Gp.surfaceNormal.y * Gq.surfaceNormal.y +
                                     Gp.surfaceNormal.z * Gq.surfaceNormal.z);
            const float w = expf(-dn * dn / 0.3f) *
                            expf(-float(dx * dx + dy * dy) / 8.0f);
            const float4& s = src[sidx];
            sum.x += s.x * w; sum.y += s.y * w; sum.z += s.z * w; sum.w += s.w * w;
            totalW += w;
        }
    if (totalW > 1e-8f) {
        dst[i] = make_float4(sum.x / totalW, sum.y / totalW, sum.z / totalW,
                             sum.w / totalW);
    }
}

__global__ void kComposite(const RadG* aDd, const RadG* aDs, const RadG* aId,
                           const RadG* aIs, const float4* emission,
                           const float* oidn, int n, float4* out) {
    const size_t i = size_t(blockIdx.x) * blockDim.x + threadIdx.x;
    if (i >= size_t(n)) return;
    float3 c;
    if (oidn) {
        c = make_float3(oidn[i*3+0], oidn[i*3+1], oidn[i*3+2]);
    } else {
        c = aDd[i].r + aDs[i].r + aId[i].r + aIs[i].r;
        if (emission) c = c + devLum3(emission[i]);
    }
    out[i] = make_float4(c.x, c.y, c.z, 1.0f);
}

// ---------------------------------------------------------------------------
// Kernels — display domain (tonemap / bloom / gamma / despeckle / FXAA)
// ---------------------------------------------------------------------------
__global__ void kWBReduce(const float4* img, int n, float3* partial) {
    __shared__ float3 sh[TPB];
    const int t = threadIdx.x;
    const size_t idx = size_t(blockIdx.x) * blockDim.x + t;
    sh[t] = (idx < size_t(n)) ? devLum3(img[idx]) : make_float3(0.0f, 0.0f, 0.0f);
    __syncthreads();
    for (int s = blockDim.x / 2; s > 0; s >>= 1) {
        if (t < s) sh[t] = sh[t] + sh[t + s];
        __syncthreads();
    }
    if (t == 0) partial[blockIdx.x] = sh[0];
}

__global__ void kTonemap(float4* img, int n, float exposure,
                         float gR, float gG, float gB) {
    const size_t i = size_t(blockIdx.x) * blockDim.x + threadIdx.x;
    if (i >= size_t(n)) return;
    float3 c = devLum3(img[i]);
    c.x *= gR; c.y *= gG; c.z *= gB;
    c = c * exposure;
    img[i] = make_float4(c.x, c.y, c.z, 1.0f);
}

__global__ void kBloomExtract(const float4* img, int n, float3* bloomColor) {
    const size_t i = size_t(blockIdx.x) * blockDim.x + threadIdx.x;
    if (i >= size_t(n)) return;
    const float3 c = devLum3(img[i]);
    const float Clum = c.x * 0.299f + c.y * 0.587f + c.z * 0.114f;
    if (Clum < 1.0f) { bloomColor[i] = make_float3(0.0f, 0.0f, 0.0f); return; }
    const float3 b = c / powf(Clum, 0.65f);
    bloomColor[i] = make_float3(fmaxf(b.x - 1.0f, 0.0f),
                                fmaxf(b.y - 1.0f, 0.0f),
                                fmaxf(b.z - 1.0f, 0.0f));
}

__global__ void kBloomBlur(const float3* src, int width, int height, int step,
                           float3* dst) {
    const size_t i = size_t(blockIdx.x) * blockDim.x + threadIdx.x;
    const int n = width * height;
    if (i >= size_t(n)) return;
    const int x = int(i) % width, y = int(i) / width;
    const float h[5] = {0.0625f, 0.25f, 0.375f, 0.25f, 0.0625f};
    float3 col = make_float3(0.0f, 0.0f, 0.0f);
    for (int ky = -2; ky <= 2; ++ky)
        for (int kx = -2; kx <= 2; ++kx) {
            const int nx = x + kx * step, ny = y + ky * step;
            if (nx < 0 || nx >= width || ny < 0 || ny >= height) continue;
            const float3 t = src[size_t(ny) * width + nx];
            col = col + t * (h[ky + 2] * h[kx + 2]);
        }
    dst[i] = col;
}

__global__ void kBloomAdd(float4* img, const float3* bloomColor, int n) {
    const size_t i = size_t(blockIdx.x) * blockDim.x + threadIdx.x;
    if (i >= size_t(n)) return;
    const float3 c = devLum3(img[i]) + bloomColor[i] / 6.0f;
    img[i] = make_float4(c.x, c.y, c.z, 1.0f);
}

struct GammaParams {
    bool  aces;
    float invGamma;
    float sat;
    bool  curveOn;
    float splitWarm, splitCool;
    bool  splitKeySun;
    bool  gradeOn;
    int   nBands;
};

__global__ void kGamma(float4* img, const float* sunliness, const float* curveLut,
                       const float* gradeBands, int n, GammaParams p) {
    const size_t i = size_t(blockIdx.x) * blockDim.x + threadIdx.x;
    if (i >= size_t(n)) return;
    float3 c = devLum3(img[i]);
    if (p.aces) {
        // CUDA float3 has no mixed vec+scalar operator (types.h defines only
        // vec*vec / vec*scalar) — expand the ACES fit per component.
        float3 v = make_float3(
            (c.x * (2.51f * c.x + 0.03f)) / (c.x * (2.43f * c.x + 0.59f) + 0.14f),
            (c.y * (2.51f * c.y + 0.03f)) / (c.y * (2.43f * c.y + 0.59f) + 0.14f),
            (c.z * (2.51f * c.z + 0.03f)) / (c.z * (2.43f * c.z + 0.59f) + 0.14f));
        c = make_float3(fminf(fmaxf(v.x, 0.0f), 1.0f),
                        fminf(fmaxf(v.y, 0.0f), 1.0f),
                        fminf(fmaxf(v.z, 0.0f), 1.0f));
        c = make_float3(powf(c.x, p.invGamma), powf(c.y, p.invGamma),
                        powf(c.z, p.invGamma));
    } else {
        const float Clum = c.x * 0.299f + c.y * 0.587f + c.z * 0.114f;
        if (Clum > 0.75f) {
            const float lumBound = tanhf(3.0f * (Clum - 0.75f)) / 3.0f + 0.75f;
            c = c / Clum * lumBound;
        }
        c = make_float3(fminf(fmaxf(c.x, 0.0f), 1.0f),
                        fminf(fmaxf(c.y, 0.0f), 1.0f),
                        fminf(fmaxf(c.z, 0.0f), 1.0f));
        c = make_float3(powf(c.x, p.invGamma), powf(c.y, p.invGamma),
                        powf(c.z, p.invGamma));
    }
    if (p.sat != 1.0f) {
        const float l = c.x * 0.299f + c.y * 0.587f + c.z * 0.114f;
        c = make_float3(l + (c.x - l) * p.sat, l + (c.y - l) * p.sat,
                        l + (c.z - l) * p.sat);
        c = make_float3(fminf(fmaxf(c.x, 0.0f), 1.0f),
                        fminf(fmaxf(c.y, 0.0f), 1.0f),
                        fminf(fmaxf(c.z, 0.0f), 1.0f));
    }
    if (p.curveOn) {
        const float l = c.x * 0.299f + c.y * 0.587f + c.z * 0.114f;
        const float lc = curveLut[int(fminf(fmaxf(l, 0.0f), 1.0f) * 1023.0f + 0.5f)];
        const float s = lc / fmaxf(l, 1e-6f);
        c = c * s;
        c = make_float3(fminf(fmaxf(c.x, 0.0f), 1.0f),
                        fminf(fmaxf(c.y, 0.0f), 1.0f),
                        fminf(fmaxf(c.z, 0.0f), 1.0f));
    }
    float sw;
    if (p.splitKeySun && sunliness) {
        float sf = fminf(fmaxf(sunliness[i], 0.0f), 1.0f);
        sw = sf * sf * (3.0f - 2.0f * sf);
    } else {
        const float sl = c.x * 0.299f + c.y * 0.587f + c.z * 0.114f;
        sw = fminf(fmaxf((sl - 0.15f) / 0.60f, 0.0f), 1.0f);
        sw = sw * sw * (3.0f - 2.0f * sw);
    }
    if (p.splitWarm != 0.0f) {
        c.x *= 1.0f + p.splitWarm * sw;
        c.z *= 1.0f - p.splitWarm * 0.6f * sw;
    }
    if (p.splitCool != 0.0f) {
        const float shadow = 1.0f - sw;
        c.z *= 1.0f + p.splitCool * shadow;
        c.x *= 1.0f - p.splitCool * 0.6f * shadow;
    }
    if (p.gradeOn) {
        const float l = c.x * 0.299f + c.y * 0.587f + c.z * 0.114f;
        int hi = 1;
        while (hi < p.nBands - 1 && gradeBands[hi*4] < l) ++hi;
        const int lo = hi - 1;
        const float l0 = gradeBands[lo*4], l1 = gradeBands[hi*4];
        const float t = (l1 > l0)
            ? fminf(fmaxf((l - l0) / (l1 - l0), 0.0f), 1.0f) : 0.0f;
        c.x *= gradeBands[lo*4+1] + (gradeBands[hi*4+1] - gradeBands[lo*4+1]) * t;
        c.y *= gradeBands[lo*4+2] + (gradeBands[hi*4+2] - gradeBands[lo*4+2]) * t;
        c.z *= gradeBands[lo*4+3] + (gradeBands[hi*4+3] - gradeBands[lo*4+3]) * t;
    }
    c = make_float3(fminf(fmaxf(c.x, 0.0f), 1.0f),
                    fminf(fmaxf(c.y, 0.0f), 1.0f),
                    fminf(fmaxf(c.z, 0.0f), 1.0f));
    img[i] = make_float4(c.x, c.y, c.z, 1.0f);
}

__global__ void kDespeckle(const float4* src, int width, int height,
                           float thresh, float4* dst) {
    const size_t i = size_t(blockIdx.x) * blockDim.x + threadIdx.x;
    const int n = width * height;
    if (i >= size_t(n)) return;
    const int x = int(i) % width, y = int(i) / width;
    const float4 c0 = src[i];
    const float vals[3] = {c0.x, c0.y, c0.z};
    float ch[3];
    for (int c = 0; c < 3; ++c) {
        float nb[9]; int k = 0;
        for (int dy = -1; dy <= 1; ++dy)
            for (int dx = -1; dx <= 1; ++dx) {
                const int sx = imin(width - 1, imax(0, x + dx));
                const int sy = imin(height - 1, imax(0, y + dy));
                const float4& s = src[size_t(sy) * width + sx];
                nb[k++] = (c == 0) ? s.x : (c == 1) ? s.y : s.z;
            }
        for (int q = 1; q < 9; ++q) {
            const float key = nb[q]; int j = q - 1;
            while (j >= 0 && nb[j] > key) { nb[j + 1] = nb[j]; --j; }
            nb[j + 1] = key;
        }
        ch[c] = nb[4];
    }
    const float dev = fmaxf(fmaxf(fabsf(vals[0] - ch[0]), fabsf(vals[1] - ch[1])),
                            fabsf(vals[2] - ch[2]));
    if (dev > thresh)
        dst[i] = make_float4(ch[0], ch[1], ch[2], 1.0f);
}

__global__ void kFXAA(const float4* src, int width, int height, float4* dst) {
    const size_t i = size_t(blockIdx.x) * blockDim.x + threadIdx.x;
    const int n = width * height;
    if (i >= size_t(n)) return;
    const int x = int(i) % width, y = int(i) / width;
    dst[i] = src[i];
    if (x < 2 || x >= width - 2 || y < 2 || y >= height - 2) return;
    const float edgeThreshold = 0.1f;
    const size_t W = size_t(width);
    auto lumAt = [&](int px, int py) -> float {
        const float4& c = src[size_t(py) * W + size_t(px)];
        return c.x * 0.299f + c.y * 0.587f + c.z * 0.114f;
    };
    const float lC = lumAt(x, y);
    const float lN = lumAt(x, y - 1), lS = lumAt(x, y + 1);
    const float lW = lumAt(x - 1, y), lE = lumAt(x + 1, y);
    const float hContrast = fabsf(lW + lE - 2.0f * lC);
    const float vContrast = fabsf(lN + lS - 2.0f * lC);
    if (hContrast < edgeThreshold && vContrast < edgeThreshold) return;
    const float kernel[3] = {0.25f, 0.50f, 0.25f};
    float4 avg = make_float4(0.0f, 0.0f, 0.0f, 0.0f);
    float total = 0.0f;
    if (hContrast > vContrast) {
        for (int k = -2; k <= 2; ++k) {
            const float4& s = src[size_t(y) * W + size_t(x + k)];
            const float w = kernel[abs(k)];
            avg = make_float4(avg.x + s.x * w, avg.y + s.y * w,
                              avg.z + s.z * w, avg.w + s.w * w);
            total += w;
        }
    } else {
        for (int k = -2; k <= 2; ++k) {
            const float4& s = src[size_t(y + k) * W + size_t(x)];
            const float w = kernel[abs(k)];
            avg = make_float4(avg.x + s.x * w, avg.y + s.y * w,
                              avg.z + s.z * w, avg.w + s.w * w);
            total += w;
        }
    }
    if (total > 0.0f)
        dst[i] = make_float4(avg.x / total, avg.y / total, avg.z / total,
                             avg.w / total);
}

// ---------------------------------------------------------------------------
// Host pipeline
// ---------------------------------------------------------------------------
bool gpuPostEnabled() {
    if (!envFlag1("RENDER_POST_GPU")) return false;
    int ndev = 0;
    return cudaGetDeviceCount(&ndev) == cudaSuccess && ndev > 0;
}

bool postProcessGPU(Photo& photo, int shadeOptions, int spp, int denoiseQuality) {
    if (!layoutOk()) {
        fprintf(stderr, "POSTGPU: host/device layout mismatch — CPU fallback\n");
        return false;
    }
    if (!photo.channels.finalColor || photo.width <= 0 || photo.height <= 0 ||
        !photo.radiance_Dd || !photo.radiance_Ds || !photo.radiance_Id ||
        !photo.radiance_Is || !photo.Gbuffer)
        return false;

    const int width = photo.width, height = photo.height;
    const size_t n = size_t(width) * height;
    const auto t0 = std::chrono::steady_clock::now();
    auto msOf = [](const std::chrono::steady_clock::time_point& a,
                   const std::chrono::steady_clock::time_point& b) {
        return std::chrono::duration<float, std::milli>(b - a).count();
    };

    cudaDeviceProp prop{};
    int devId = 0;
    cudaGetDevice(&devId);
    cudaGetDeviceProperties(&prop, devId);
    fprintf(stderr, "POSTGPU: %dx%d on %s (statistics-equivalent path, NOT byte-identical)\n",
            width, height, prop.name);
    fflush(stderr);

    // RENDER_SPP backdoor (same as the CPU path).
    int sppEff = spp;
    if (const char* e = std::getenv("RENDER_SPP")) {
        const long v = std::strtol(e, nullptr, 10);
        if (v > 0) sppEff = (int)v;
    }

    // ---- device buffers ----
    DevBuf aDd, aDs, aId, aIs, gbuf, emission, sunMaskD, finalG;
    DevBuf temp, Lm, lumLin, radBuf, varBuf, varEst, lumF;
    DevBuf oidnG, colorG, albedoG, normalG, sunlinessG;
    DevBuf aoG, albBlurG, bloomColor, bloomTemp, scratch4;
    DevBuf wbPartial, lumReducePartial;
    if (!aDd.alloc(n * 16) || !aDs.alloc(n * 16) || !aId.alloc(n * 16) ||
        !aIs.alloc(n * 16) || !gbuf.alloc(n * sizeof(GbufG)) ||
        !finalG.alloc(n * 16))
        return false;

    // ---- pack + upload (H2D) ----
    const auto tUp0 = std::chrono::steady_clock::now();
    CUDA_CHECK(cudaMemcpy(aDd.p, photo.radiance_Dd, n * 16, cudaMemcpyHostToDevice));
    CUDA_CHECK(cudaMemcpy(aDs.p, photo.radiance_Ds, n * 16, cudaMemcpyHostToDevice));
    CUDA_CHECK(cudaMemcpy(aId.p, photo.radiance_Id, n * 16, cudaMemcpyHostToDevice));
    CUDA_CHECK(cudaMemcpy(aIs.p, photo.radiance_Is, n * 16, cudaMemcpyHostToDevice));
    {
        std::vector<GbufG> g(n);
        const HitInfo* GB = photo.Gbuffer;
        for (size_t i = 0; i < n; ++i) {
            g[i].position      = make_float3(GB[i].position.x, GB[i].position.y, GB[i].position.z);
            g[i].shapeNormal   = make_float3(GB[i].shapeNormal.x, GB[i].shapeNormal.y, GB[i].shapeNormal.z);
            g[i].surfaceNormal = make_float3(GB[i].surfaceNormal.x, GB[i].surfaceNormal.y, GB[i].surfaceNormal.z);
            g[i].baseColor     = make_float3(GB[i].baseColor.x, GB[i].baseColor.y, GB[i].baseColor.z);
            g[i].roughness = GB[i].roughness; g[i].metallic = GB[i].metallic;
            g[i].specular  = GB[i].specular;  g[i].opacity  = GB[i].opacity;
        }
        CUDA_CHECK(cudaMemcpy(gbuf.p, g.data(), n * sizeof(GbufG), cudaMemcpyHostToDevice));
    }
    const bool hasEmission = photo.channels.emission != nullptr;
    if (hasEmission) {
        if (!emission.alloc(n * 16)) return false;
        CUDA_CHECK(cudaMemcpy(emission.p, photo.channels.emission, n * 16,
                              cudaMemcpyHostToDevice));
    }
    const bool hasSunMask = photo.splitKeySun && photo.sunMask != nullptr;
    if (hasSunMask) {
        if (!sunMaskD.alloc(n * 4) || !sunlinessG.alloc(n * 4)) return false;
        CUDA_CHECK(cudaMemcpy(sunMaskD.p, photo.sunMask, n * 4, cudaMemcpyHostToDevice));
    }
    CUDA_CHECK(cudaMemcpy(finalG.p, photo.channels.finalColor, n * 16,
                          cudaMemcpyHostToDevice));
    CUDA_CHECK(cudaDeviceSynchronize());
    const double upMs = msOf(tUp0, std::chrono::steady_clock::now());

    RadG* A[4] = { (RadG*)aDd.p, (RadG*)aDs.p, (RadG*)aId.p, (RadG*)aIs.p };
    const GbufG* G = (const GbufG*)gbuf.p;
    float4* EM = (float4*)(hasEmission ? emission.p : nullptr);
    // sunlinessG is allocated with the upload stage but only WRITTEN inside
    // the denoise block (same scope as the CPU path). kGamma must key the
    // split-tone on this flag, not on pointer non-nullness — with DoDenoise
    // off the buffer holds cudaMalloc garbage and the CPU path falls back to
    // the luma key (photo.sunliness empty), so a null-check alone would read
    // uninitialized device memory and diverge.
    bool sunlinessReady = false;

    bool denoised = false;
    float lumScale = 1.0f, rawLumMean = 0.0f;
    const auto tDn0 = std::chrono::steady_clock::now();

    // ======================= denoise block =======================
    if (shadeOptions & Photo::DoDenoise) {
        const unsigned nS7  = ceilDiv(n, 7);
        const unsigned nS13 = ceilDiv(n, 13);
        const unsigned nS4  = ceilDiv(n, 4);

        // ---- robust statistics: lumScale (1/7 median) + rawLumMean ----
        {
            if (!lumReducePartial.alloc(ceilDiv(n, TPB) * sizeof(float3))) return false;
            kLumReduce<<<ceilDiv(n, TPB), TPB>>>(A[0], A[1], A[2], A[3],
                                                 (int)n, (float3*)lumReducePartial.p);
            std::vector<float3> parts(ceilDiv(n, TPB));
            CUDA_CHECK(cudaMemcpy(parts.data(), lumReducePartial.p,
                                  parts.size() * sizeof(float3), cudaMemcpyDeviceToHost));
            double s = 0.0;
            for (const float3& p : parts) s += double(p.x);
            rawLumMean = float(s / double(n));
        }
        {
            DevBuf samp;
            if (!samp.alloc(size_t(nS7) * 4)) return false;
            kCompositeLumSample<<<ceilDiv(nS7, TPB), TPB>>>(
                A[0], A[1], A[2], A[3], (int)n, 7, (float*)samp.p);
            std::vector<float> s(nS7);
            CUDA_CHECK(cudaMemcpy(s.data(), samp.p, size_t(nS7) * 4,
                                  cudaMemcpyDeviceToHost));
            std::nth_element(s.begin(), s.begin() + s.size() / 2, s.end());
            lumScale = fmaxf(s[s.size() / 2], 1e-6f);
        }
        if (lumScale <= rawLumMean / 16.0f)
            lumScale = rawLumMean / 8.0f;

        // ---- relative firefly clamp (per AOV, 1/13 p99.9) ----
        for (int c = 0; c < 4; ++c) {
            DevBuf samp;
            if (!samp.alloc(size_t(nS13) * 4)) return false;
            kSampleLum1<<<ceilDiv(nS13, TPB), TPB>>>(A[c], (int)n, 13,
                                                     (float*)samp.p);
            std::vector<float> s(nS13);
            CUDA_CHECK(cudaMemcpy(s.data(), samp.p, size_t(nS13) * 4,
                                  cudaMemcpyDeviceToHost));
            std::nth_element(s.begin(),
                             s.begin() + ptrdiff_t(s.size() * 999 / 1000),
                             s.end());
            const float p999 = s[size_t(s.size() * 999 / 1000)];
            kSpatialClamp<<<ceilDiv(n, TPB), TPB>>>(A[c], (int)n,
                                                    fmaxf(20.0f, 8.0f * p999));
        }

        // ---- SPP-adaptive tolerance ----
        static constexpr float SPP_REF = 320.0f;
        float kSpp = std::sqrt(SPP_REF / fmaxf(float(sppEff), 1.0f));
        kSpp = std::min(8.0f, std::max(1.0f, kSpp));
        const float ffGate = 6.0f * kSpp;

        // ---- sun matte (sun-keyed split-tone) ----
        if (hasSunMask) {
            DevBuf mb;
            if (!mb.alloc(n * 4)) return false;
            kSunlinessBlur<<<ceilDiv(n, TPB), TPB>>>((const float*)sunMaskD.p,
                                                     width, height, (float*)mb.p);
            kSunlinessNorm<<<ceilDiv(n, TPB), TPB>>>(
                (const float*)mb.p, A[0], A[1], A[2], A[3], EM, (int)n,
                (float*)sunlinessG.p);
            sunlinessReady = true;
        }

        // ---- OIDN (RENDER_OIDN=1, quality >= 2) ----
        bool oidnHostOk = false;
#ifdef RENDERER_HAS_OIDN
        {
            const bool oidnOn = envFlag1("RENDER_OIDN");
            if (oidnOn && denoiseQuality >= 2) {
                if (!colorG.alloc(n * 12) || !albedoG.alloc(n * 12) ||
                    !normalG.alloc(n * 12))
                    return false;
                kOIDNPack<<<ceilDiv(n, TPB), TPB>>>(
                    A[0], A[1], A[2], A[3], EM, G, (int)n,
                    (float*)colorG.p, (float*)albedoG.p, (float*)normalG.p);
                std::vector<float> color(n * 3), albedo(n * 3), normal(n * 3), out(n * 3);
                CUDA_CHECK(cudaMemcpy(color.data(),  colorG.p,  n * 12, cudaMemcpyDeviceToHost));
                CUDA_CHECK(cudaMemcpy(albedo.data(), albedoG.p, n * 12, cudaMemcpyDeviceToHost));
                CUDA_CHECK(cudaMemcpy(normal.data(), normalG.p, n * 12, cudaMemcpyDeviceToHost));
                oidnHostOk = oidnRunRT(color, albedo, normal, out, width, height);
                if (oidnHostOk) {
                    photo.oidnColor = out;
                    if (!oidnG.alloc(n * 12)) return false;
                    CUDA_CHECK(cudaMemcpy(oidnG.p, out.data(), n * 12,
                                          cudaMemcpyHostToDevice));
                    fprintf(stderr, "OIDN: denoised %dx%d (RT filter, albedo+normal guides)\n",
                            width, height);
                }
                fflush(stderr);
            }
        }
#endif
        const bool oidnUsed = denoiseQuality >= 3 && oidnHostOk &&
                              photo.oidnColor.size() == n * 3;
        const bool nlMeans = !oidnUsed;
        const int nlPasses = !nlMeans ? 0
                           : (denoiseQuality == 1 ? 1 : FILTER_PASSES + 2);
        const float noiseTarget = getDenoiseNoiseTarget();

        if (nlMeans) {
            // Tier 0: TRUE MC variance snapshot (before filterVar replaces it)
            DevBuf mc[4];
            if (!mc[0].alloc(n * 4) || !mc[1].alloc(n * 4) ||
                !mc[2].alloc(n * 4) || !mc[3].alloc(n * 4)) return false;
            for (int c = 0; c < 4; ++c)
                kSnapshotMcVar<<<ceilDiv(n, TPB), TPB>>>(A[c], (float*)mc[c].p,
                                                         (int)n);

            // Tier 1: filterVar + pre-firefly clamp
            if (!varEst.alloc(n * 4)) return false;
            fprintf(stderr, "DENOISE start\n"); fflush(stderr);
            for (int c = 0; c < 4; ++c) {
                kFilterVar<<<ceilDiv(n, TPB), TPB>>>(A[c], width, height,
                                                     (float*)varEst.p);
                kAssignVar<<<ceilDiv(n, TPB), TPB>>>(A[c], (const float*)varEst.p,
                                                     (int)n);
            }
            if (!lumF.alloc(n * 4)) return false;
            for (int c = 0; c < 4; ++c) {
                kLumField<<<ceilDiv(n, TPB), TPB>>>(A[c], (int)n, (float*)lumF.p);
                kFireflyClamp<<<ceilDiv(n, TPB), TPB>>>(A[c], (const float*)lumF.p,
                                                        width, height, ffGate);
            }

            // Normalize to median composite luminance 1.0
            const float invScale = 1.0f / lumScale;
            for (int c = 0; c < 4; ++c)
                kScaleAov<<<ceilDiv(n, TPB), TPB>>>(A[c], (int)n, invScale);
            for (int c = 0; c < 4; ++c)
                kScaleMcVar<<<ceilDiv(n, TPB), TPB>>>((float*)mc[c].p, (int)n,
                                                      invScale * invScale);

            // Per-AOV median Var (1/7 samples, host median — same stride)
            float varMed[4] = {1.0f, 1.0f, 1.0f, 1.0f};
            for (int c = 0; c < 4; ++c) {
                DevBuf samp;
                if (!samp.alloc(size_t(nS7) * 4)) return false;
                kSampleVar<<<ceilDiv(nS7, TPB), TPB>>>(A[c], (int)n, 7,
                                                       (float*)samp.p);
                std::vector<float> s(nS7);
                CUDA_CHECK(cudaMemcpy(s.data(), samp.p, size_t(nS7) * 4,
                                      cudaMemcpyDeviceToHost));
                std::nth_element(s.begin(), s.begin() + s.size() / 2, s.end());
                varMed[c] = fmaxf(s[s.size() / 2], 1e-12f);
            }

            // Multi-scale ladder — the same 5x5 log-domain filter as the CPU.
            if (!temp.alloc(n * 16) || !Lm.alloc(n * 12) || !lumLin.alloc(n * 4) ||
                !radBuf.alloc(n * 12) || !varBuf.alloc(n * 4))
                return false;
            const bool spec[4] = {false, true, false, true};
            for (int pass = 0; pass < nlPasses; ++pass) {
                for (int c = 0; c < 4; ++c) {
                    kLogTemp<<<ceilDiv(n, TPB), TPB>>>(A[c], (int)n,
                                                       (RadG*)temp.p);
                    kLm<<<ceilDiv(n, TPB), TPB>>>((const RadG*)temp.p, width,
                                                  height, (float3*)Lm.p);
                    kLumLin<<<ceilDiv(n, TPB), TPB>>>(A[c], (int)n,
                                                      (float*)lumLin.p);
                    kFilterMain<<<ceilDiv(n, TPB), TPB>>>(
                        (const RadG*)temp.p, G, (const float3*)Lm.p,
                        (const float*)lumLin.p, width, height, spec[c], pass,
                        varMed[c], (float3*)radBuf.p, (float*)varBuf.p);
                    kWriteBack<<<ceilDiv(n, TPB), TPB>>>(
                        A[c], A[c], (const RadG*)temp.p,
                        (const float3*)radBuf.p, (const float*)varBuf.p,
                        (const float*)mc[c].p, (const float*)lumLin.p, (int)n,
                        noiseTarget);
                }
            }

            // Denormalize back to scene units
            for (int c = 0; c < 4; ++c)
                kScaleAov<<<ceilDiv(n, TPB), TPB>>>(A[c], (int)n, lumScale);

            // Post-filter firefly cleanup (quality >= 2)
            if (denoiseQuality >= 2) {
                for (int c = 0; c < 4; ++c) {
                    kLumField<<<ceilDiv(n, TPB), TPB>>>(A[c], (int)n,
                                                        (float*)lumF.p);
                    kFireflyClamp<<<ceilDiv(n, TPB), TPB>>>(
                        A[c], (const float*)lumF.p, width, height, ffGate);
                }
            }
        }
        denoised = true;   // nlMeans or OIDN — recomposite has a source either way

        // ---- AO (RENDER_AO=1) ----
        {
            const bool aoOn = envFlag1("RENDER_AO");
            if (aoOn) {
                float aoStrength = 0.6f, aoRadius = 12.0f;
                int aoTaps = 12;
                if (const char* e = std::getenv("RENDER_AO_STRENGTH"))
                    aoStrength = std::strtof(e, nullptr);
                if (const char* e = std::getenv("RENDER_AO_RADIUS"))
                    aoRadius = std::strtof(e, nullptr);
                if (const char* e = std::getenv("RENDER_AO_TAPS"))
                    aoTaps = std::max(4, std::atoi(e));
                if (!aoG.alloc(n * 4)) return false;
                kAO<<<ceilDiv(n, TPB), TPB>>>(G, width, height, aoRadius,
                                              aoStrength, aoTaps, (float*)aoG.p);
                kMulAOIndirect<<<ceilDiv(n, TPB), TPB>>>((RadG*)aId.p, (RadG*)aIs.p,
                                                         (const float*)aoG.p,
                                                         (int)n);
                if (oidnG.p)
                    kMulAOOidn<<<ceilDiv(n, TPB), TPB>>>(
                        (float*)oidnG.p, (const float*)aoG.p, A[0], A[1], A[2],
                        A[3], EM, (int)n);
                fprintf(stderr, "AO: position-based (radius=%.0fpx strength=%.2f taps=%d)\n",
                        aoRadius, aoStrength, aoTaps);
                fflush(stderr);
            }
        }

        // ---- albedo detail re-injection ----
        {
            float detailGain = -1.0f;
            if (const char* e = std::getenv("RENDER_DENOISE_DETAIL"))
                detailGain = fmaxf(0.0f, std::strtof(e, nullptr));
            if (detailGain > 0.0f) {
                if (!albBlurG.alloc(n * 12)) return false;
                kAlbBlur<<<ceilDiv(n, TPB), TPB>>>(G, width, height,
                                                   (float3*)albBlurG.p);
                kDetailInject<<<ceilDiv(n, TPB), TPB>>>(
                    (RadG*)aDd.p, (RadG*)aDs.p, (RadG*)aId.p, (RadG*)aIs.p,
                    G, (const float3*)albBlurG.p, (int)n, detailGain);
            }
        }

        // ---- SPP-adaptive energy gain ----
        {
            double dnLumSum = 0.0;
            std::vector<float> dnLum;
            if (!wbPartial.alloc(ceilDiv(n, TPB) * sizeof(float3))) return false;
            if (nlMeans) {
                kLumReduce<<<ceilDiv(n, TPB), TPB>>>(A[0], A[1], A[2], A[3],
                                                     (int)n,
                                                     (float3*)wbPartial.p);
                std::vector<float3> parts(ceilDiv(n, TPB));
                CUDA_CHECK(cudaMemcpy(parts.data(), wbPartial.p,
                                      parts.size() * sizeof(float3), cudaMemcpyDeviceToHost));
                for (const float3& p : parts) dnLumSum += double(p.x);
                DevBuf samp;
                if (!samp.alloc(size_t(nS4) * 4)) return false;
                kCompositeLumSample<<<ceilDiv(nS4, TPB), TPB>>>(
                    A[0], A[1], A[2], A[3], (int)n, 4, (float*)samp.p);
                dnLum.resize(nS4);
                CUDA_CHECK(cudaMemcpy(dnLum.data(), samp.p, size_t(nS4) * 4,
                                      cudaMemcpyDeviceToHost));
            } else if (oidnG.p) {
                kDNLumReduceOidn<<<ceilDiv(n, TPB), TPB>>>(
                    (const float*)oidnG.p, (int)n, (float3*)wbPartial.p);
                std::vector<float3> parts(ceilDiv(n, TPB));
                CUDA_CHECK(cudaMemcpy(parts.data(), wbPartial.p,
                                      parts.size() * sizeof(float3), cudaMemcpyDeviceToHost));
                for (const float3& p : parts) dnLumSum += double(p.x);
                DevBuf samp;
                if (!samp.alloc(size_t(nS4) * 4)) return false;
                kDNLumSampleOidn<<<ceilDiv(nS4, TPB), TPB>>>(
                    (const float*)oidnG.p, (int)n, 4, (float*)samp.p);
                dnLum.resize(nS4);
                CUDA_CHECK(cudaMemcpy(dnLum.data(), samp.p, size_t(nS4) * 4,
                                      cudaMemcpyDeviceToHost));
            }
            float energyGain = 1.0f, refGain = 1.0f, r320 = 0.80f, Sdn = 0.0f;
            if (dnLumSum > 1e-12 && !dnLum.empty()) {
                std::nth_element(dnLum.begin(), dnLum.begin() + dnLum.size() / 2,
                                 dnLum.end());
                const double dnMean = dnLumSum / double(n);
                const double dnMedian = dnLum[dnLum.size() / 2];
                Sdn = float(dnMean / fmax(dnMedian, 1e-9));
                if (const char* e = std::getenv("RENDER_DENOISE_RET320")) {
                    const double v = std::strtod(e, nullptr);
                    if (v > 0.0 && v < 1.0) r320 = float(v);
                }
                refGain = float((r320 * double(rawLumMean) * double(n)) / dnLumSum);
                refGain = std::min(3.0f, std::max(0.5f, refGain));
                if (sppEff < int(SPP_REF)) energyGain = refGain;
            }
            if (nlMeans && (energyGain > 1.0001f || energyGain < 0.9999f)) {
                for (int c = 0; c < 4; ++c)
                    kGainAov<<<ceilDiv(n, TPB), TPB>>>(A[c], (int)n, energyGain);
            }
            if (!nlMeans && oidnG.p &&
                (energyGain > 1.0001f || energyGain < 0.9999f))
                kGainOidn<<<ceilDiv(n, TPB), TPB>>>((float*)oidnG.p, (int)n,
                                                    energyGain);
            fprintf(stderr, "DENOISE done (lumScale=%.4f spp=%d k=%.3f Sdn=%.2f r320=%.3f refGain=%.3f energyGain=%.3f)\n",
                    lumScale, sppEff, kSpp, Sdn, r320, refGain, energyGain);
            fflush(stderr);
        }
    }
    CUDA_CHECK(cudaDeviceSynchronize());
    const double dnMs = msOf(tDn0, std::chrono::steady_clock::now());

    // ======================= emission denoise =======================
    const auto tDis0 = std::chrono::steady_clock::now();
    if ((shadeOptions & Photo::DoDenoise) && hasEmission) {
        if (!scratch4.alloc(n * 16)) return false;
        CUDA_CHECK(cudaMemcpy(scratch4.p, EM, n * 16, cudaMemcpyDeviceToDevice));
        kEmissionDenoise<<<ceilDiv(n, TPB), TPB>>>((const float4*)scratch4.p, G,
                                                   width, height, EM);
    }

    // ======================= recomposite =======================
    if (denoised)
        kComposite<<<ceilDiv(n, TPB), TPB>>>(
            A[0], A[1], A[2], A[3], EM,
            (const float*)(oidnG.p ? oidnG.p : nullptr), (int)n,
            (float4*)finalG.p);

    // ======================= tonemap (WB + exposure) =======================
    {
        float gR = 1.0f, gG = 1.0f, gB = 1.0f;
        if (photo.wbStrength > 0.0f) {
            if (!wbPartial.alloc(ceilDiv(n, TPB) * sizeof(float3))) return false;
            kWBReduce<<<ceilDiv(n, TPB), TPB>>>((const float4*)finalG.p, (int)n,
                                                (float3*)wbPartial.p);
            std::vector<float3> parts(ceilDiv(n, TPB));
            CUDA_CHECK(cudaMemcpy(parts.data(), wbPartial.p, parts.size() * sizeof(float3),
                                  cudaMemcpyDeviceToHost));
            double mr = 0, mg = 0, mb = 0;
            for (const float3& p : parts) { mr += p.x; mg += p.y; mb += p.z; }
            mr /= double(n); mg /= double(n); mb /= double(n);
            const float mLum = float(0.299 * mr + 0.587 * mg + 0.114 * mb);
            auto wbGain = [&](double m) -> float {
                if (m < 1e-8) return 1.0f;
                return 1.0f + photo.wbStrength * (float(mLum / m) - 1.0f);
            };
            gR = wbGain(mr); gG = wbGain(mg); gB = wbGain(mb);
            fprintf(stderr, "WB: mean=(%.2f,%.2f,%.2f) gains=(%.3f,%.3f,%.3f)\n",
                    mr, mg, mb, gR, gG, gB);
        }
        kTonemap<<<ceilDiv(n, TPB), TPB>>>((float4*)finalG.p, (int)n,
                                           fmaxf(photo.exposure, 1e-6f),
                                           gR, gG, gB);
    }

    // ======================= bloom =======================
    if (shadeOptions & Photo::DoBloom) {
        if (width >= 4 && height >= 4) {
            if (!bloomColor.alloc(n * 12) || !bloomTemp.alloc(n * 12))
                return false;
            kBloomExtract<<<ceilDiv(n, TPB), TPB>>>(
                (const float4*)finalG.p, (int)n, (float3*)bloomColor.p);
            for (int step = 1; step <= 8; step *= 2) {
                CUDA_CHECK(cudaMemcpy(bloomTemp.p, bloomColor.p, n * 12,
                                      cudaMemcpyDeviceToDevice));
                kBloomBlur<<<ceilDiv(n, TPB), TPB>>>(
                    (const float3*)bloomTemp.p, width, height, step,
                    (float3*)bloomColor.p);
                kBloomAdd<<<ceilDiv(n, TPB), TPB>>>(
                    (float4*)finalG.p, (const float3*)bloomColor.p, (int)n);
            }
        }
    }

    // ======================= gamma =======================
    {
        float lutHost[1024]; bool curveOn = false;
        getContrastCurveLUT(lutHost, curveOn);
        DevBuf curveG, gradeG;
        if (curveOn) {
            if (!curveG.alloc(1024 * 4)) return false;
            CUDA_CHECK(cudaMemcpy(curveG.p, lutHost, 1024 * 4,
                                  cudaMemcpyHostToDevice));
        }
        const bool gradeOn = photo.gradeBands.size() >= 8;
        if (gradeOn) {
            if (!gradeG.alloc(photo.gradeBands.size() * 4)) return false;
            CUDA_CHECK(cudaMemcpy(gradeG.p, photo.gradeBands.data(),
                                  photo.gradeBands.size() * 4,
                                  cudaMemcpyHostToDevice));
        }
        GammaParams gp;
        gp.aces = getUseAcesTonemap();
        gp.invGamma = 1.0f / 2.2f;
        gp.sat = photo.saturationBoost;
        gp.curveOn = curveOn;
        gp.splitWarm = photo.splitWarm;
        gp.splitCool = photo.splitCool;
        gp.splitKeySun = photo.splitKeySun && sunlinessReady;
        gp.gradeOn = gradeOn;
        gp.nBands = int(photo.gradeBands.size() / 4);
        kGamma<<<ceilDiv(n, TPB), TPB>>>(
            (float4*)finalG.p,
            (const float*)(sunlinessG.p ? sunlinessG.p : nullptr),
            (const float*)(curveG.p ? curveG.p : nullptr),
            (const float*)(gradeG.p ? gradeG.p : nullptr), (int)n, gp);
    }

    // ======================= despeckle =======================
    {
        float dsp = 0.20f;
        if (const char* e = std::getenv("RENDER_DESPECKLE"))
            dsp = std::strtof(e, nullptr);
        if (dsp > 0.0f && width > 0 && height > 0) {
            if (!scratch4.alloc(n * 16)) return false;   // src copy (reuse)
            CUDA_CHECK(cudaMemcpy(scratch4.p, finalG.p, n * 16,
                                  cudaMemcpyDeviceToDevice));
            kDespeckle<<<ceilDiv(n, TPB), TPB>>>((const float4*)scratch4.p,
                                                 width, height, dsp,
                                                 (float4*)finalG.p);
        }
    }

    // ======================= FXAA =======================
    if (shadeOptions & Photo::DoFXAA) {
        if (width >= 4 && height >= 4) {
            if (!scratch4.alloc(n * 16)) return false;
            CUDA_CHECK(cudaMemcpy(scratch4.p, finalG.p, n * 16,
                                  cudaMemcpyDeviceToDevice));
            kFXAA<<<ceilDiv(n, TPB), TPB>>>((const float4*)scratch4.p, width,
                                            height, (float4*)finalG.p);
        }
    }
    CUDA_CHECK(cudaDeviceSynchronize());
    const double disMs = msOf(tDis0, std::chrono::steady_clock::now());

    // ======================= download (D2H write-back) =======================
    // NOTE: the copies below run in sequence; a failure mid-way (extremely
    // unlikely — 20+ successful steps precede this) leaves the host Photo in
    // a mixed state and the CPU fallback would re-denoise it. Accepted: the
    // fallback path is a last-resort safety net, not a hot path.
    const auto tDl0 = std::chrono::steady_clock::now();
    CUDA_CHECK(cudaMemcpy(photo.channels.finalColor, finalG.p, n * 16,
                          cudaMemcpyDeviceToHost));
    CUDA_CHECK(cudaMemcpy(photo.radiance_Dd, aDd.p, n * 16, cudaMemcpyDeviceToHost));
    CUDA_CHECK(cudaMemcpy(photo.radiance_Ds, aDs.p, n * 16, cudaMemcpyDeviceToHost));
    CUDA_CHECK(cudaMemcpy(photo.radiance_Id, aId.p, n * 16, cudaMemcpyDeviceToHost));
    CUDA_CHECK(cudaMemcpy(photo.radiance_Is, aIs.p, n * 16, cudaMemcpyDeviceToHost));
    if (hasEmission)
        CUDA_CHECK(cudaMemcpy(photo.channels.emission, EM, n * 16,
                              cudaMemcpyDeviceToHost));
    if (sunlinessG.p && sunlinessReady) {
        photo.sunliness.assign(n, 0.0f);
        CUDA_CHECK(cudaMemcpy(photo.sunliness.data(), sunlinessG.p, n * 4,
                              cudaMemcpyDeviceToHost));
    }
    if (oidnG.p) {
        // Post-AO / post-energy-gain state — matches the CPU path, which
        // mutates photo.oidnColor in place through the same passes.
        photo.oidnColor.assign(n * 3, 0.0f);
        CUDA_CHECK(cudaMemcpy(photo.oidnColor.data(), oidnG.p, n * 12,
                              cudaMemcpyDeviceToHost));
    }
    CUDA_CHECK(cudaDeviceSynchronize());
    const double dlMs = msOf(tDl0, std::chrono::steady_clock::now());

    fprintf(stderr, "POSTGPU timings: upload=%.0fms denoise=%.0fms display=%.0fms download=%.0fms total=%.0fms\n",
            upMs, dnMs, disMs, dlMs,
            msOf(t0, std::chrono::steady_clock::now()));
    fflush(stderr);
    return true;
}
