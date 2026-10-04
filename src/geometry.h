#pragma once

#include "../include/types.h"
#include "../include/math_utils.h"

// ---- Triangle Intersection (Möller–Trumbore) ----
// Returns: intersection distance t (INFINITY if no hit)
// Also outputs barycentric coordinates (u, v).
// Inline in header for dual CPU/CUDA compilation.
__host__ __device__ inline
float rayTriangleIntersect(const Ray& ray,
                           const float3& v0, const float3& v1, const float3& v2,
                           float& u, float& v) {
    const float3 e1 = v1 - v0;
    const float3 e2 = v2 - v0;
    const float3 pvec = cross(ray.direction, e2);
    const float det = dot(e1, pvec);
    if (fabsf(det) < 1e-8f) return INFINITY;
    const float invDet = 1.0f / det;
    const float3 tvec = ray.origin - v0;
    u = dot(tvec, pvec) * invDet;
    if (u < 0.0f || u > 1.0f) return INFINITY;
    const float3 qvec = cross(tvec, e1);
    v = dot(ray.direction, qvec) * invDet;
    if (v < 0.0f || u + v > 1.0f) return INFINITY;
    return dot(e2, qvec) * invDet;
}

// ---- Ray-Box Intersection (slab method) ----
// In/Out: tL/tR as current near/far interval, narrowed by the box
__host__ __device__ inline
void rayBoxIntersect(const Ray& ray, const Box& box,
                     float& tL, float& tR) {
    float3 invDir;
    invDir.x = 1.0f / ray.direction.x;
    invDir.y = 1.0f / ray.direction.y;
    invDir.z = 1.0f / ray.direction.z;
    float3 t0 = (box.v0 - ray.origin) * invDir;
    float3 t1 = (box.v1 - ray.origin) * invDir;
    if (ray.direction.x < 0.0f) { float tmp = t0.x; t0.x = t1.x; t1.x = tmp; }
    if (ray.direction.y < 0.0f) { float tmp = t0.y; t0.y = t1.y; t1.y = tmp; }
    if (ray.direction.z < 0.0f) { float tmp = t0.z; t0.z = t1.z; t1.z = tmp; }
    tL = fmaxf(tL, fmaxf(t0.x, fmaxf(t0.y, t0.z)));
    tR = fminf(tR, fminf(t1.x, fminf(t1.y, t1.z)));
}

// ---- Barycentric Coordinates ----
__host__ __device__ inline
float3 barycentric(const float3& v0, const float3& v1,
                   const float3& v2, const float3& P) {
    const float3 e1 = v1 - v0, e2 = v2 - v0, ep = P - v0;
    float d11 = dot(e1, e1), d12 = dot(e1, e2), d22 = dot(e2, e2);
    float dp1 = dot(ep, e1), dp2 = dot(ep, e2);
    float denom = d11 * d22 - d12 * d12;
    if (fabsf(denom) < 1e-8f) return makef3(-1, -1, -1);
    float invD = 1.0f / denom;
    float v = (d22 * dp1 - d12 * dp2) * invD;
    float w = (d11 * dp2 - d12 * dp1) * invD;
    return makef3(1.0f - v - w, v, w);
}
// ---- UV Differentials from Position Differentials ----
// Given a face and a position differential (dPdx or dPdy),
// compute the corresponding UV differential by projecting
// the offset onto the face plane and using barycentric interpolation.
__host__ inline
float2 getDuv(const float3 v[3], const float2 uv[3], const float3& hit_dP) {
    const float3 p = v[0] + hit_dP;
    float3 bc = barycentric(v[0], v[1], v[2], p);
    // Clamp valid barycentric coords
    if (bc.x < 0.0f) { bc.x = 0.0f; float s = 1.0f / (bc.y + bc.z + 1e-8f); bc.y *= s; bc.z *= s; }
    if (bc.y < 0.0f) { bc.y = 0.0f; float s = 1.0f / (bc.x + bc.z + 1e-8f); bc.x *= s; bc.z *= s; }
    if (bc.z < 0.0f) { bc.z = 0.0f; float s = 1.0f / (bc.x + bc.y + 1e-8f); bc.x *= s; bc.y *= s; }
    float2 result;
    result.x = bc.y * uv[1].x + bc.z * uv[2].x + (bc.x - 1.0f) * uv[0].x;
    result.y = bc.y * uv[1].y + bc.z * uv[2].y + (bc.x - 1.0f) * uv[0].y;
    return result;
}
// ---- Tangent Space ----
__host__ __device__ inline
void getTangentSpace(const float3& normal,
                     float3& tangent, float3& bitangent) {
    const float3 ref = (fabsf(normal.x) > 0.9f)
        ? makef3(0, 1, 0) : makef3(1, 0, 0);
    tangent = normalize(cross(normal, ref));
    bitangent = normalize(cross(normal, tangent));
}

// ---- Spherical Coordinates ----
// Equirectangular convention (matches the reference renderer's SkyBox::get):
//   u = atan2(-x, z) / 2PI + 0.5,  v = acos(y) / PI
// The atan2(-x, z) form is the PolyHaven-standard azimuth for these HDRIs.
// The previous atan2(z, x) form rotated the environment 90 degrees in
// azimuth, which threw the HDRI sun onto the wrong side of the scene.
__host__ __device__ inline
float2 dirToSphere(const float3& dir) {
    const float3 d = normalize(dir);
    float theta = acosf(d.y);
    float phi   = atan2f(-d.x, d.z);
#ifdef __CUDACC__
    return make_float2(phi * INV_TWO_PI + 0.5f, theta * INV_PI);
#else
    return float2(phi * INV_TWO_PI + 0.5f, theta * INV_PI);
#endif
}

__host__ __device__ inline
float3 sphereToDir(const float2& uv) {
    float phi   = (uv.x - 0.5f) * 2.0f * PI;
    float theta = uv.y * PI;
    return makef3(-sinf(theta) * sinf(phi), cosf(theta), sinf(theta) * cosf(phi));
}
