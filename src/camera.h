#pragma once

#include "../include/types.h"
#include "../include/math_utils.h"

struct Camera {
    float3 position{0.0f, 0.0f, 3.0f};  // Default: outside box
    float3 direction{0.0f, 0.0f, -1.0f}; // Forward direction
    float3 up{0.0f, 1.0f, 0.0f};         // Up vector
    float3 right;       // Right vector (auto-computed)

    float  fov{60.0f};  // Vertical field of view (degrees)
    float  focus{0.0f}; // Focal distance (0 = no depth of field)
    float  coc{0.0f};   // Maximum circle of confusion radius (pixels)

    // Generate a ray (with AA jitter + depth of field)
    // Inline in header — needed by both host (camera.cpp) and device (render_gpu.cu)
    __host__ __device__
    Ray generateRay(float px, float py,
                    float offsetU, float offsetV,
                    float lensU, float lensV,
                    float2 filmSize) const {
        const float aspect = filmSize.x / filmSize.y;
        const float tanFov = tanf(fov * 0.5f * PI / 180.0f);

        const float ndcX = (2.0f * (px + offsetU) / filmSize.x - 1.0f) * aspect * tanFov;
        const float ndcY = (1.0f - 2.0f * (py + offsetV) / filmSize.y) * tanFov;

        const float3 w = normalize(direction);
        const float3 u = normalize(cross(w, up));
        const float3 v = cross(u, w);

        float3 dir = normalize(ndcX * u + ndcY * v + w);

        if (focus > 0.0f && coc > 0.0f) {
            const float3 lensPos = position + (lensU * coc * u + lensV * coc * v);
            const float3 focalPoint = position + dir * focus;
            dir = normalize(focalPoint - lensPos);
            return Ray(lensPos, dir);
        }

        return Ray(position, dir);
    }
};
