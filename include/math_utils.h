#pragma once

#include "types.h"

// ---- CUDA-compatible vector math ----
// Works with both glm::vec3 (CPU) and CUDA float3 (GPU)

// Helper: construct float3 from 3 components, works on both CPU and CUDA
__host__ __device__ inline float3 makef3(float x, float y, float z) {
#ifdef __CUDACC__
    return make_float3(x, y, z);
#else
    return float3(x, y, z);
#endif
}

__host__ __device__ inline float   dot(const float3& a, const float3& b) { return a.x*b.x + a.y*b.y + a.z*b.z; }
__host__ __device__ inline float3 cross(const float3& a, const float3& b) { return makef3(a.y*b.z - a.z*b.y, a.z*b.x - a.x*b.z, a.x*b.y - a.y*b.x); }
__host__ __device__ inline float  length(const float3& a) { return sqrtf(dot(a, a)); }
__host__ __device__ inline float3 normalize(const float3& a) { float l = length(a); return l < 1e-8f ? makef3(0,0,0) : makef3(a.x/l, a.y/l, a.z/l); }


