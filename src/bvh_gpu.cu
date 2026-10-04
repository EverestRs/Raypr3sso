#include "bvh_gpu.h"
#include "component.h"
#include "material.h"
#include <cuda_runtime.h>
#include <cstdio>
#include <unordered_map>
#include <vector>

#define CUDA_SAFE(call) do { cudaError_t e = call; if (e != cudaSuccess) { fprintf(stderr, "CUDA_SAFE(%s) at %s:%d: %s\n", #call, __FILE__, __LINE__, cudaGetErrorString(e)); fflush(stderr); return -1; } } while(0)

static std::unordered_map<cudaTextureObject_t, cudaArray_t> g_skyArrays;

void GPUBVHTraverser::upload(const LinearBVHNode* cpuNodes, int count,
                              LinearBVHNode** gpuNodes) {
    const size_t size = size_t(count) * sizeof(LinearBVHNode);
    cudaMalloc(gpuNodes, size);
    cudaMemcpy(*gpuNodes, cpuNodes, size, cudaMemcpyHostToDevice);
}

// ---- SkyBox GPU upload (host-side; takes raw float3 data) ----
cudaTextureObject_t uploadSkyBoxToGPU(const float3* data, int w, int h) {
    if (!data || w == 0 || h == 0) return 0;

    cudaChannelFormatDesc channelDesc = cudaCreateChannelDesc<float4>();
    cudaArray_t cuArray = nullptr;
    cudaError_t e = cudaMallocArray(&cuArray, &channelDesc, w, h);
    if (e != cudaSuccess) {
        fprintf(stderr, "  Sky upload: cudaMallocArray %dx%d FAILED: %s\n",
                w, h, cudaGetErrorString(e));
        return 0;
    }

    // Convert float3 -> float4 on host heap
    float4* texData = new float4[w * h];
    for (int i = 0; i < w * h; ++i)
        texData[i] = make_float4(data[i].x, data[i].y, data[i].z, 1.0f);

    e = cudaMemcpy2DToArray(cuArray, 0, 0, texData,
                            w * sizeof(float4), w * sizeof(float4), h,
                            cudaMemcpyHostToDevice);
    if (e != cudaSuccess)
        fprintf(stderr, "  Sky upload: cudaMemcpy2DToArray FAILED: %s\n",
                cudaGetErrorString(e));
    delete[] texData;

    cudaResourceDesc resDesc = {};
    resDesc.resType = cudaResourceTypeArray;
    resDesc.res.array.array = cuArray;

    cudaTextureDesc texDesc = {};
    texDesc.addressMode[0] = cudaAddressModeWrap;
    texDesc.addressMode[1] = cudaAddressModeClamp;
    texDesc.filterMode     = cudaFilterModeLinear;
    texDesc.readMode       = cudaReadModeElementType;
    texDesc.normalizedCoords = 1;

    cudaTextureObject_t texObj = 0;
    e = cudaCreateTextureObject(&texObj, &resDesc, &texDesc, nullptr);
    if (e != cudaSuccess) {
        fprintf(stderr, "  Sky upload: cudaCreateTextureObject FAILED: %s\n",
                cudaGetErrorString(e));
        return 0;
    }
    // Verify the object actually reads back non-black (catches silent
    // driver-level failures that used to render the whole sky black).
    fprintf(stderr, "  Sky upload: %dx%d OK (texObj=%llu)\n", w, h,
            (unsigned long long)texObj);
    g_skyArrays[texObj] = cuArray;
    return texObj;
}

void freeSkyBoxTexture(cudaTextureObject_t texture) {
    if (!texture) return;
    cudaDestroyTextureObject(texture);
    auto it = g_skyArrays.find(texture);
    if (it != g_skyArrays.end()) {
        cudaFreeArray(it->second);
        g_skyArrays.erase(it);
    }
}

// C-linkage wrapper for CPU callers (avoids float3 ABI mismatch between CUDA and GLM)
extern "C" cudaTextureObject_t uploadSkyBoxToGPU_float(float* data, int w, int h) {
    // Convert float array to float3 array
    float3* f3 = new float3[w * h];
    for (int i = 0; i < w * h; ++i)
        f3[i] = make_float3(data[i*3], data[i*3+1], data[i*3+2]);
    cudaTextureObject_t result = uploadSkyBoxToGPU(f3, w, h);
    delete[] f3;
    return result;
}

extern "C" {

// ---- GPU Texture Upload (float4 pipeline, matching skybox approach) ----
}  // extern "C"

int uploadTexturesToGPU(const int* texWidths, const int* texHeights,
                        const int* texChannels, const unsigned char* const* texData,
                        const int* texFormats, const unsigned char* const* texBlocks,
                        const long long* texByteOffsets,
                        int numTextures,
                        unsigned long long** outTexObjects,
                        int** outTexWidths, int** outTexHeights,
                        unsigned char** outRawData) {
    *outTexObjects = nullptr;  // deprecated: cudaTextureObjects are not used
                               // by the kernel (manual bilinear on texData)
    if (numTextures == 0) {
        *outTexWidths = nullptr; *outTexHeights = nullptr; *outRawData = nullptr;
        return 0;
    }
    int* gpuWidths = nullptr; int* gpuHeights = nullptr;
    cudaMalloc(&gpuWidths, numTextures * sizeof(int));
    cudaMalloc(&gpuHeights, numTextures * sizeof(int));

    std::vector<int> h_widths(numTextures, 0), h_heights(numTextures, 0);
    int success = 0;
    // One flat buffer holding EITHER raw BC blocks (0.5-1 byte/texel, decoded
    // in-kernel — the Bistro's 3.76 GB RGBA8 set becomes 0.73 GB) OR RGBA8
    // texels for sources that were not BC. texByteOffsets gives each texture's
    // byte position (or a running cursor when the caller passes no offsets).
    size_t totalBytes = 0;
    for (int i = 0; i < numTextures; ++i) {
        int w = texWidths[i], h = texHeights[i];
        if (w <= 0 || h <= 0) continue;
        const bool bc = texFormats && texByteOffsets && texFormats[i] > 0 &&
                        texBlocks && texBlocks[i];
        if (bc) { h_widths[i] = w; h_heights[i] = h;
                  totalBytes += size_t(texByteOffsets[i + 1] - texByteOffsets[i]); }
        else if (texData[i]) { h_widths[i] = w; h_heights[i] = h;
                  totalBytes += size_t(w) * h * 4; }
    }
    unsigned char* gpuRaw = nullptr;
    if (totalBytes > 0) {
        cudaError_t e = cudaMalloc(&gpuRaw, totalBytes);
        if (e != cudaSuccess) {
            fprintf(stderr, "!! TEXTURE UPLOAD FAILED: cudaMalloc(%zu MB) %s\n",
                    totalBytes / (1024 * 1024), cudaGetErrorString(e)); fflush(stderr);
        }
    }
    size_t cursor = 0;   // used when the caller supplies no byte offsets
    size_t bcBytes = 0, rgbaBytes = 0;
    for (int i = 0; i < numTextures; ++i) {
        int w = h_widths[i], h = h_heights[i];
        if (w <= 0 || h <= 0 || !gpuRaw) continue;
        const bool bc = texFormats && texByteOffsets && texFormats[i] > 0 &&
                        texBlocks && texBlocks[i];
        size_t dstOff = (texByteOffsets ? size_t(texByteOffsets[i]) : cursor);
        cudaError_t e;
        if (bc) {
            size_t n = size_t(texByteOffsets[i + 1] - texByteOffsets[i]);
            e = cudaMemcpy(gpuRaw + dstOff, texBlocks[i], n, cudaMemcpyHostToDevice);
            bcBytes += n;
            if (!texByteOffsets) cursor += n;
        } else {
            size_t count = size_t(w) * h;
            const unsigned char* src = texData[i];
            int ch = texChannels[i];
            std::vector<unsigned char> buf(count * 4);
            for (size_t p = 0; p < count; ++p) {
                const uint8_t* s = src + p * ch;
                buf[p * 4 + 0] = s[0];
                buf[p * 4 + 1] = (ch > 1) ? s[1] : s[0];
                buf[p * 4 + 2] = (ch > 2) ? s[2] : s[0];
                buf[p * 4 + 3] = (ch > 3) ? s[3] : 255;
            }
            e = cudaMemcpy(gpuRaw + dstOff, buf.data(), count * 4, cudaMemcpyHostToDevice);
            rgbaBytes += count * 4;
            if (!texByteOffsets) cursor += count * 4;
        }
        if (e != cudaSuccess) {
            fprintf(stderr, "!! texture %d/%d upload FAILED: %s — its materials will render wrong\n",
                    i + 1, numTextures, cudaGetErrorString(e)); fflush(stderr);
            h_widths[i] = 0; h_heights[i] = 0;  // kernel falls back per-texture
        } else {
            ++success;
        }
    }
    fprintf(stderr, "  Texture VRAM: %.2f GB compressed (BC blocks) + %.2f GB RGBA8 = %.2f GB total\n",
            bcBytes / 1e9, rgbaBytes / 1e9, (bcBytes + rgbaBytes) / 1e9);
    if (success < numTextures)
        fprintf(stderr, "!! WARNING: only %d/%d textures uploaded to GPU (GPU out of memory?) — output will be WRONG\n",
                success, numTextures); fflush(stderr);

    if (gpuWidths)  cudaMemcpy(gpuWidths,  h_widths.data(),  numTextures * sizeof(int), cudaMemcpyHostToDevice);
    if (gpuHeights) cudaMemcpy(gpuHeights, h_heights.data(), numTextures * sizeof(int), cudaMemcpyHostToDevice);
    *outTexWidths  = gpuWidths;
    *outTexHeights = gpuHeights;
    *outRawData = gpuRaw;
    return success;
}

void freeGPUTextures(unsigned long long* gpuTexObjs_v, int numTextures) {
    cudaTextureObject_t* gpuTexObjs = reinterpret_cast<cudaTextureObject_t*>(gpuTexObjs_v);
    if (!gpuTexObjs || numTextures <= 0) return;

    // Copy texture object handles from GPU to host
    std::vector<cudaTextureObject_t> h_texObjs(numTextures);
    cudaMemcpy(h_texObjs.data(), gpuTexObjs,
               numTextures * sizeof(cudaTextureObject_t),
               cudaMemcpyDeviceToHost);

    // Destroy each texture object (this also frees the underlying CUDA arrays)
    for (int i = 0; i < numTextures; ++i) {
        if (h_texObjs[i] != 0) {
            cudaDestroyTextureObject(h_texObjs[i]);
        }
    }

    // Free the GPU pointer array
    cudaFree(gpuTexObjs);
}

// Upload skybox importance CDF to GPU (extern C for host callers)
extern "C" float* uploadSkyCDF(const float* hostCDF, int numEntries) {
    if (!hostCDF || numEntries <= 0) return nullptr;
    float* gpuCDF = nullptr;
    cudaMalloc(&gpuCDF, numEntries * sizeof(float));
    cudaMemcpy(gpuCDF, hostCDF, numEntries * sizeof(float), cudaMemcpyHostToDevice);
    return gpuCDF;
}

extern "C" void freeGPUCDF(float* gpuCDF) {
    if (gpuCDF) cudaFree(gpuCDF);
}

void freeGPUTextureBuffers(unsigned char* texData, int* texWidths, int* texHeights) {
    if (texData)    { cudaError_t e = cudaFree(texData);    if (e != cudaSuccess) fprintf(stderr, "  free texData FAILED: %s\n", cudaGetErrorString(e)); }
    if (texWidths)  { cudaFree(texWidths); }
    if (texHeights) { cudaFree(texHeights); }
}
