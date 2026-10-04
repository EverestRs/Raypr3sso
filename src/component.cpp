#include "component.h"
#include "material.h"
#include "geometry.h"
#include <algorithm>

// ---- Generator ----
Generator::Generator(uint32_t seed)
    : mt(seed), dist(0.0f, 1.0f) {}

float Generator::operator()() {
    return dist(mt);
}

// ---- Distribution1D ----
void Distribution1D::build(const std::vector<float>& f) {
    const size_t n = f.size();
    cdf.resize(n + 1);
    cdf[0] = 0.0f;
    for (size_t i = 0; i < n; ++i) {
        cdf[i + 1] = cdf[i] + f[i];
    }
    sum = cdf[n];
    if (sum > 0.0f) {
        for (size_t i = 1; i <= n; ++i) {
            cdf[i] /= sum;
        }
        cdf[n] = 1.0f;  // clamp
    }
}

float Distribution1D::sample(float u, float& pdf) const {
    if (cdf.empty() || sum <= 0.0f) {
        pdf = 0.0f;
        return 0.0f;
    }

    // Binary search
    auto it = std::upper_bound(cdf.begin(), cdf.end(), u);
    int idx = static_cast<int>(it - cdf.begin() - 1);
    idx = std::max(0, std::min(idx, static_cast<int>(cdf.size()) - 2));

    pdf = (cdf[idx + 1] - cdf[idx]) * sum;
    return (idx + (u - cdf[idx]) / (cdf[idx + 1] - cdf[idx])) / static_cast<float>(cdf.size() - 1);
}

float Distribution1D::pdfAt(float x) const {
    if (cdf.empty() || sum <= 0.0f) return 0.0f;
    int idx = static_cast<int>(x * (cdf.size() - 1));
    idx = std::max(0, std::min(idx, static_cast<int>(cdf.size()) - 2));
    return (cdf[idx + 1] - cdf[idx]) * sum;
}

// ---- LightObject methods ----
void LightObject::addFace(const Face& face, const float3& emission) {
    faces.push_back(&face);
    // Area-weighted power
    const float3 e1 = face.v[1] - face.v[0];
    const float3 e2 = face.v[2] - face.v[0];
    const float area = 0.5f * glm::length(glm::cross(e1, e2));
    const float lum = emission.x * RGB_WEIGHT[0] + emission.y * RGB_WEIGHT[1] + emission.z * RGB_WEIGHT[2];
    power += lum * area;
    // Rebuild distribution
    std::vector<float> weights;
    weights.reserve(faces.size());
    for (size_t i = 0; i < faces.size(); ++i) {
        const float3 e1 = faces[i]->v[1] - faces[i]->v[0];
        const float3 e2 = faces[i]->v[2] - faces[i]->v[0];
        float a = 0.5f * glm::length(glm::cross(e1, e2));
        weights.push_back(fmaxf(1e-8f, a));
    }
    faceDistrib.build(weights);
}

LightSample LightObject::sample(Generator& gen, const float3& fromPoint) const {
    LightSample ls;
    if (faces.empty() || power <= 0.0f) {
        ls.pdf = 0.0f; return ls;
    }
    // Sample a face from the distribution
    float facePdf;
    float sampleVal = faceDistrib.sample(gen(), facePdf);
    int idx = std::min(static_cast<int>(sampleVal * faces.size()),
                       static_cast<int>(faces.size()) - 1);
    // Convert continuous PDF → discrete probability: divide by sum
    // (Distribution1D::sample returns continuous PDF = p_bin * sum,
    //  dividing by sum gives the discrete probability mass p_bin)
    {
        float sumW = faceDistrib.getSum();
        facePdf = (sumW > 0.0f) ? (facePdf / sumW) : 0.0f;
    }
    // Uniformly sample the triangle
    const Face& f = *faces[idx];
    float u = gen(), v = gen();
    if (u + v > 1.0f) { u = 1.0f - u; v = 1.0f - v; }
    float w = 1.0f - u - v;
    ls.position = w * f.v[0] + u * f.v[1] + v * f.v[2];
    ls.wi = glm::normalize(ls.position - fromPoint);
    float distSq = glm::dot(ls.position - fromPoint, ls.position - fromPoint);
    if (distSq < 1e-12f) { ls.pdf = 0.0f; return ls; }
    // PDF = facePdf * (1/area) * dist² / |dot(wi, N_light)|
    const float3 e1 = f.v[1] - f.v[0], e2 = f.v[2] - f.v[0];
    float area = 0.5f * glm::length(glm::cross(e1, e2));
    if (area < 1e-12f) { ls.pdf = 0.0f; return ls; }
    float geom = fabsf(glm::dot(ls.wi, glm::normalize(glm::cross(e1, e2))));
    ls.pdf = facePdf * (1.0f / area) * distSq / fmaxf(geom, 1e-8f);
    // Emission at sampled point — use emissive texture if available
    ls.emission = float3(0.0f);
    if (f.material) {
        ls.emission = f.material->emissionFactor;
        // Bistro's emission comes from emissive DDS textures, not the flat factor.
        // Sample the texture at the hit UV to get the actual emitted radiance.
        if (f.material->emissionMap && !f.material->emissionMap->empty()) {
            float2 uv = w * f.data[0]->uv + u * f.data[1]->uv + v * f.data[2]->uv;
            float4 texEm = f.material->emissionMap->sample(uv.x, uv.y);
            float3 tex(texEm);
            if (glm::length(f.material->emissionFactor) > 1e-6f)
                ls.emission = f.material->emissionFactor * tex;
            else
                ls.emission = tex * 1000.0f;  // texture-only boost
        }
    }
    return ls;
}

float LightObject::pdf(const float3& fromPoint, const LightSample& sample) const {
    // Not implemented for one-sample MIS; returns 0
    (void)fromPoint; (void)sample;
    return 0.0f;
}

// ---- SkyBox ----
#include <stb_image.h>

void SkyBox::load(const std::string& hdrFile) {
    // Load HDR environment map using stb_image
    int ch;
    float* hdrData = stbi_loadf(hdrFile.c_str(), &width, &height, &ch, 3);
    if (!hdrData) {
        width = height = 0;
        return;
    }

    data.resize(size_t(width) * height);
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            const int idx = (y * width + x) * 3;
            data[y * width + x] = float3(hdrData[idx], hdrData[idx+1], hdrData[idx+2]);
        }
    }
    stbi_image_free(hdrData);

    // Build importance sampling distribution by luminance
    std::vector<float> weights(size_t(width) * height);
    float totalLum = 0.0f;
    for (size_t i = 0; i < data.size(); ++i) {
        const float lum = data[i].x * RGB_WEIGHT[0]
                        + data[i].y * RGB_WEIGHT[1]
                        + data[i].z * RGB_WEIGHT[2];
        weights[i] = fmaxf(1e-8f, lum);
        totalLum += weights[i];
    }
    distrib.build(weights);
}

float3 SkyBox::sample(const float3& dir) const {
    if (data.empty()) return float3(0.0f);

    // Equirectangular mapping — atan2(-x, z) matches the reference renderer
    // (see geometry.h dirToSphere note; atan2(z, x) rotated the sun 90 deg).
    const float3 d = glm::normalize(dir);
    float u = atan2f(-d.x, d.z) * INV_TWO_PI + 0.5f;
    float v = acosf(glm::clamp(d.y, -1.0f, 1.0f)) * INV_PI;

    // Wrap
    u = u - floorf(u);
    v = glm::clamp(v, 0.0f, 1.0f);

    // Map to pixel coordinates with bilinear filtering
    const float fx = u * width  - 0.5f;
    const float fy = v * height - 0.5f;
    int ix = static_cast<int>(floorf(fx));
    int iy = static_cast<int>(floorf(fy));
    float tx = fx - floorf(fx);
    float ty = fy - floorf(fy);

    int x0 = (ix + width)  % width;
    int x1 = (ix + 1 + width) % width;
    int y0 = glm::clamp(iy,       0, height - 1);
    int y1 = glm::clamp(iy + 1,   0, height - 1);

    auto fetch = [&](int x, int y) -> float3 {
        return data[size_t(y) * width + x];
    };

    float3 c00 = fetch(x0, y0);
    float3 c10 = fetch(x1, y0);
    float3 c01 = fetch(x0, y1);
    float3 c11 = fetch(x1, y1);

    float3 result;
    for (int c = 0; c < 3; ++c) {
        result[c] = ((c00[c] * (1 - tx) + c10[c] * tx) * (1 - ty)
                   + (c01[c] * (1 - tx) + c11[c] * tx) * ty);
    }
    return result;
}

float3 SkyBox::sampleImportance(float u1, float u2, float& pdf) const {
    if (data.empty()) {
        pdf = 1.0f;
        return float3(0.0f);
    }

    float lightPdf;
    float sampleVal = distrib.sample(u1, lightPdf);
    int idx = std::min(static_cast<int>(sampleVal * width * height),
                       static_cast<int>(data.size()) - 1);

    int y = idx / width;
    int x = idx % width;

    // Convert pixel to direction
    float u = (float(x) + 0.5f) / width;
    float v = (float(y) + 0.5f) / height;
    float3 dir = sphereToDir(float2(u, v));

    // PDF in solid angle measure: pdf_pixel * (width * height) / (2*PI^2 * sin(theta))
    const float theta = v * PI;
    const float sinTheta = sinf(theta);
    if (sinTheta < 1e-6f) {
        pdf = 0.0f;
        return float3(0.0f);
    }

    pdf = lightPdf * float(width) * height / (2.0f * PI * PI * sinTheta);
    return dir;
}
