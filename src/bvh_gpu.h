#pragma once

#include "../include/types.h"
#include "component.h"
#include "geometry.h"

// Opaque typedef for cudaTextureObject_t on CPU (declared in render_gpu.h too)
#ifndef __CUDACC__
typedef unsigned long long cudaTextureObject_t;
#endif

// Upload SkyBox HDR data (float3*) to a CUDA texture object
// Defined in bvh_gpu.cu. Returns 0 on failure.
cudaTextureObject_t uploadSkyBoxToGPU(const float3* data, int w, int h);
void freeSkyBoxTexture(cudaTextureObject_t texture);

// C-linkage wrapper for CPU callers (takes raw float array, 3 channels per pixel)
#ifdef __cplusplus
extern "C" {
#endif
cudaTextureObject_t uploadSkyBoxToGPU_float(float* data, int w, int h);
#ifdef __cplusplus
}
#endif

// ---- GPU Texture Upload ----
// Upload a set of CPU ImageTexture objects to GPU, creating CUDA texture objects.
// The caller provides:
//   textures - array of CPU-side ImageTexture pointers (some may be nullptr = no texture)
//   numTextures - count of textures in the array
//   outTexObjects - [output] will be filled with cudaTextureObject_t handles (GPU memory)
//   outTexWidths / outTexHeights - [output] texture dimensions (GPU memory)
// Returns 0 on success, non-zero on failure.
// The output arrays are allocated with cudaMalloc and must be freed by the caller.
// This function is implemented in bvh_gpu.cu (compiled by nvcc).
// Upload textures to GPU using raw data (avoids CUDA-side ImageTexture dependency).
// Each texture is specified by w, h, channels, and byte data pointer.
// Returns GPU arrays for texture objects and dimensions.
// NOTE: Declared with extern "C" to ensure ABI compatibility between nvcc and g++
#ifdef __cplusplus
extern "C" {
#endif
// texFormats[texId]: 0 = RGBA8 (texData holds decoded texels), 1/2/3 = BC1/BC3/BC5
// (texBlocks[texId] holds the raw mip-0 blocks). texByteOffsets[numTextures+1]
// is the byte layout inside the returned buffer (BC blocks or RGBA8 texels).
// texBlocks/texFormats/texByteOffsets may be null → legacy all-RGBA8 layout.
int uploadTexturesToGPU(const int* texWidths, const int* texHeights,
                        const int* texChannels, const unsigned char* const* texData,
                        const int* texFormats, const unsigned char* const* texBlocks,
                        const long long* texByteOffsets,
                        int numTextures,
                        unsigned long long** outTexObjects,
                        int** outTexWidths, int** outTexHeights,
                        unsigned char** outRawData, // mixed raw data for manual sampling
                        // P0 mipmap chain (optional; null = no mips uploaded).
                        // Levels are RGBA8, stored level-by-level (separate
                        // host allocations) into one flat GPU buffer.
                        const int* mipLevelCounts = nullptr,      // [numTextures]
                        const int* mipLevelStart = nullptr,       // [numTextures+1]
                        const long long* mipTexByteOffsets = nullptr, // [numTextures+1]
                        const unsigned char* const* mipLevelPointers = nullptr, // flat
                        const int* mipLevelWidths = nullptr,      // flat
                        const int* mipLevelHeights = nullptr,     // flat
                        unsigned char** outMipRawData = nullptr,
                        long long** outMipByteOffsets = nullptr,
                        int** outMipLevels = nullptr,
                        int** outMipWidths = nullptr,
                        int** outMipHeights = nullptr,
                        int** outMipStart = nullptr);
void freeGPUTextures(unsigned long long* gpuTexObjs, int numTextures);

// Frees the flat uchar4 texture buffer + width/height arrays (no texObjects).
void freeGPUTextureBuffers(unsigned char* texData, int* texWidths, int* texHeights,
                           unsigned char* mipData = nullptr);
float* uploadSkyCDF(const float* hostCDF, int numEntries);
void freeGPUCDF(float* gpuCDF);
#ifdef __cplusplus
}
#endif

// ---- GPU BVH Node ----
// 8-wide compact node suitable for warp-level traversal
struct alignas(32) LinearBVHNode {
    float3 bboxMin[2];   // Child bounding box mins
    float3 bboxMax[2];   // Child bounding box maxes
    union {
        struct { int leftChild, rightChild; };  // Interior: child indices
        struct { int faceStart, faceCount; };    // Leaf: face range
    };
    int isLeaf;     // 0/1
    int pad;
};

// ---- GPU BVH Traverser ----
class GPUBVHTraverser {
public:
    // Host: copy BVH to GPU (implemented in bvh_gpu.cu)
    static void upload(const LinearBVHNode* cpuNodes, int count,
                       LinearBVHNode** gpuNodes);

    // Device: traversal kernel (inline in header to avoid cross-TU device linking issues)
    __device__ static HitRecord traverse(const Ray& ray,
                                          const LinearBVHNode* nodes,
                                          const float3* vertPositions,
                                          const int* materialIds,
                                          int numNodes);

private:
    __device__ static bool rayBoxTest(const Ray& ray,
                                       const float3& bmin,
                                       const float3& bmax,
                                       float tMin, float tMax);
};

// ---- Inline device implementation ----
__device__ inline bool GPUBVHTraverser::rayBoxTest(const Ray& ray,
                                                    const float3& bmin,
                                                    const float3& bmax,
                                                    float tMin, float tMax) {
    float tL = tMin, tR = tMax;

    float invDirX = 1.0f / ray.direction.x;
    float t0x = (bmin.x - ray.origin.x) * invDirX;
    float t1x = (bmax.x - ray.origin.x) * invDirX;
    if (invDirX < 0.0f) { float tmp = t0x; t0x = t1x; t1x = tmp; }
    tL = fmaxf(tL, t0x); tR = fminf(tR, t1x);
    if (tL > tR) return false;

    float invDirY = 1.0f / ray.direction.y;
    float t0y = (bmin.y - ray.origin.y) * invDirY;
    float t1y = (bmax.y - ray.origin.y) * invDirY;
    if (invDirY < 0.0f) { float tmp = t0y; t0y = t1y; t1y = tmp; }
    tL = fmaxf(tL, t0y); tR = fminf(tR, t1y);
    if (tL > tR) return false;

    float invDirZ = 1.0f / ray.direction.z;
    float t0z = (bmin.z - ray.origin.z) * invDirZ;
    float t1z = (bmax.z - ray.origin.z) * invDirZ;
    if (invDirZ < 0.0f) { float tmp = t0z; t0z = t1z; t1z = tmp; }
    tL = fmaxf(tL, t0z); tR = fminf(tR, t1z);
    if (tL > tR) return false;

    return tR >= tMin;
}

__device__ inline HitRecord GPUBVHTraverser::traverse(const Ray& ray,
                                                       const LinearBVHNode* nodes,
                                                       const float3* vertPositions,
                                                       const int* materialIds,
                                                       int numNodes) {
    HitRecord hit;
    constexpr int STACK_SIZE = 64;
    int stack[STACK_SIZE];
    int sp = 0;
    stack[sp++] = 0;

    while (sp > 0) {
        const int idx = stack[--sp];
        const LinearBVHNode& node = nodes[idx];

        // Test ray against the union of both children's bounding boxes
        float3 bmin, bmax;
        bmin.x = fminf(node.bboxMin[0].x, node.bboxMin[1].x);
        bmin.y = fminf(node.bboxMin[0].y, node.bboxMin[1].y);
        bmin.z = fminf(node.bboxMin[0].z, node.bboxMin[1].z);
        bmax.x = fmaxf(node.bboxMax[0].x, node.bboxMax[1].x);
        bmax.y = fmaxf(node.bboxMax[0].y, node.bboxMax[1].y);
        bmax.z = fmaxf(node.bboxMax[0].z, node.bboxMax[1].z);
        if (!rayBoxTest(ray, bmin, bmax, hit.t_min, hit.t_max))
            continue;

        if (node.isLeaf) {
            for (int i = 0; i < node.faceCount; ++i) {
                const int faceIdx = node.faceStart + i;
                const float3* v = vertPositions + faceIdx * 3;
                float u, vt;
                float t = rayTriangleIntersect(ray, v[0], v[1], v[2], u, vt);
                if (t < hit.t_max && t >= hit.t_min) {
                    hit.t_max  = t;
                    hit.t      = t;
                    hit.u      = u;
                    hit.v      = vt;
                    hit.faceIdx = faceIdx;
                }
            }
        } else {
            const float3 sign = ray.direction;
            const int nearChild = (sign.x + sign.y + sign.z >= 0.0f)
                ? node.leftChild : node.rightChild;
            const int farChild = (nearChild == node.leftChild)
                ? node.rightChild : node.leftChild;

            if (sp < STACK_SIZE - 2) {
                stack[sp++] = farChild;
                stack[sp++] = nearChild;
            }
        }
    }
    return hit;
}
