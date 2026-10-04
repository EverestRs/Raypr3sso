#include "render.h"
#include "model.h"
#include "component.h"
#include "sampling.h"
#include "geometry.h"
#include "progress.h"
#include <thread>
#include <atomic>
#include <numeric>
#include <cmath>

// ---- Medium Implementation ----
void Medium::init() {
    mediums.clear();
    mediums.emplace(-1, MediumData(1.0f, float3(1.0f)));
}

void Medium::insert(int id, float ior, const float3& absorb) {
    mediums.emplace(id, MediumData(ior, absorb));
}

void Medium::erase(int id) {
    mediums.erase(id);
}

float3 Medium::absorb() const {
    float3 abs = float3(1.0f);
    for (const auto& kv : mediums)
        abs *= kv.second.absorb;
    return abs;
}

float Medium::ior() const {
    float ior = 1.0f;
    for (const auto& kv : mediums)
        ior = fmaxf(ior, kv.second.ior);
    return ior;
}

int Medium::size() const {
    return static_cast<int>(mediums.size());
}

// Beer's law absorption
static float3 getAbsorb(const float3& absorb, float dis) {
    static const float omega_absorb = 32.0f;
    float Clum = absorb.x * RGB_WEIGHT[0] + absorb.y * RGB_WEIGHT[1] + absorb.z * RGB_WEIGHT[2];
    if (Clum < 1.0f - 1e-6f) {
        float3 result;
        result.x = powf(absorb.x, omega_absorb * dis);
        result.y = powf(absorb.y, omega_absorb * dis);
        result.z = powf(absorb.z, omega_absorb * dis);
        return result;
    }
    return float3(1.0f);
}

// =========================================================================
// Internal helpers
// =========================================================================
namespace {

float2 interpolateUV(const VertexData* const* data, float u, float v) {
    const float w = 1.0f - u - v;
    return w * data[0]->uv + u * data[1]->uv + v * data[2]->uv;
}

float3 interpolateNormal(const VertexData* const* data, float u, float v) {
    const float w = 1.0f - u - v;
    float3 n = w * data[0]->normal + u * data[1]->normal + v * data[2]->normal;
    const float len = glm::length(n);
    return (len > 1e-8f) ? n / len : n;
}

HitInfo getHitInfo(const Model& model, const Ray& ray, const HitRecord& hit,
                   const RayDifferential* rayDiff = nullptr) {
    (void)model;
    const Face& face = *hit.face;
    const Material& mat = *face.material;

    const float3 pos = ray.origin + ray.direction * hit.t;
    const float2 uv  = interpolateUV(face.data, hit.u, hit.v);

    const float3 e1 = face.v[1] - face.v[0];
    const float3 e2 = face.v[2] - face.v[0];
    float3 geomNormal = glm::normalize(glm::cross(e1, e2));

    float3 shadingNormal = interpolateNormal(face.data, hit.u, hit.v);
    // Save the OUTWARD-pointing shading normal BEFORE any flip. For well-formed
    // geometry (procedural sphere/ground + FBX loaded with aiProcess_FixInfacingNormals)
    // the interpolated vertex normals point outward, so dot(outwardNormal, ray.direction)
    // is negative iff the ray is going INTO the object. Using origGeomNormal here is
    // unreliable because the cross-product geometric normal depends on triangle
    // winding and points INWARD for this codebase's sphere/ground winding, which
    // would invert the entering flag and swap the IOR ratio at every glass surface.
    const float3 outwardShadingNormal = shadingNormal;

    if (glm::dot(geomNormal, ray.direction) > 0.0f)
        geomNormal = -geomNormal;
    // Ensure shading normal faces same hemisphere as geometric normal
    // (consistently oriented across triangle boundaries).
    if (glm::dot(shadingNormal, geomNormal) < 0.0f)
        shadingNormal = -shadingNormal;

    const bool entering = glm::dot(outwardShadingNormal, ray.direction) < 0.0f;

    // Compute mip level from ray differentials (if provided)
    float mipLevel = 0.0f;
    if (rayDiff) {
        float3 hit_dPdx, hit_dPdy;
        calc_dPdxy(ray, hit.t, geomNormal, *rayDiff, hit_dPdx, hit_dPdy);
        float2 uvArr[3] = {face.data[0]->uv, face.data[1]->uv, face.data[2]->uv};
        float2 dUVdx = getDuv(face.v, uvArr, hit_dPdx);
        float2 dUVdy = getDuv(face.v, uvArr, hit_dPdy);
        float duv = 0.5f * (glm::length(dUVdx) + glm::length(dUVdy));
        if (duv > 1e-8f)
            mipLevel = duv;
    }

    HitInfo info;
    info.position      = pos;
    info.shapeNormal   = geomNormal;
    info.surfaceNormal = shadingNormal;
    info.baseColor     = float3(mat.getBaseColor(uv.x, uv.y, mipLevel));
    // Emission = material factor × emissive texture (if present).
    // The Bistro scene's lighting comes from emissive DDS textures
    // (lanterns, light bulbs, etc.), NOT from AI_MATKEY_COLOR_EMISSIVE.
    // Without sampling the texture, every emissive surface is black.
    {
        float3 em = mat.emissionFactor;                 // ×1000 if FBX had a value
        if (mat.emissionMap && !mat.emissionMap->empty()) {
            float4 texEm = mat.emissionMap->sample(uv.x, uv.y, mipLevel);
            float3 tex(texEm);
            if (glm::length(mat.emissionFactor) > 1e-6f)
                em = mat.emissionFactor * tex;          // factor × texture
            else
                em = tex * 1000.0f;                     // texture-only, apply boost
        }
        info.emission = em;
    }
    info.specular      = mat.specularFactor;
    info.roughness     = mat.getRoughness(uv.x, uv.y, mipLevel);
    info.metallic      = mat.getMetallic(uv.x, uv.y, mipLevel);
    info.opacity       = mat.opacity;
    info.eta           = mat.ior;
    info.subsurface    = mat.subsurfaceFactor;
    info.sheen         = mat.sheenFactor;
    info.sheenTint     = mat.sheenTintFactor;
    info.clearcoat     = mat.clearcoatFactor;
    info.clearcoatGloss = mat.clearcoatGlossFactor;
    info.materialID    = 0;
    info.entering      = entering;
    return info;
}

} // anonymous namespace

// =========================================================================
// Ray Differential Functions
// =========================================================================
void calc_dPdxy(const Ray& ray, float hit_t, const float3& normal,
                const RayDifferential& base_diff,
                float3& hit_dPdx, float3& hit_dPdy) {
    float d = -glm::dot(ray.direction, normal);
    float t = hit_t;
    if (fabsf(d) > 1e-8f) {
        float invD = 1.0f / d;
        hit_dPdx = (base_diff.dPdx + base_diff.dDdx * t) +
                   normal * (glm::dot(ray.origin + ray.direction * t, normal) / d) * invD;
        hit_dPdy = (base_diff.dPdy + base_diff.dDdy * t) +
                   normal * (glm::dot(ray.origin + ray.direction * t, normal) / d) * invD;
    } else {
        hit_dPdx = base_diff.dPdx;
        hit_dPdy = base_diff.dPdy;
    }
}

void calc_dDdxy(const Ray& ray, const float3& normal,
                const RayDifferential& base_diff,
                float3& hit_dDdx, float3& hit_dDdy) {
    float ndotw = glm::dot(normal, ray.direction);
    float3 dn = normal;
    hit_dDdx = base_diff.dDdx - 2.0f * (glm::dot(base_diff.dDdx, dn) + glm::dot(ray.direction, dn)) * dn;
    hit_dDdy = base_diff.dDdy - 2.0f * (glm::dot(base_diff.dDdy, dn) + glm::dot(ray.direction, dn)) * dn;
}

// =========================================================================
// RadianceSample — accumulation primitive (Raym0nade-style)
// =========================================================================
struct RadianceSample {
    float3 bsdfPdf;    // BSDF * throughput / pdf_sample
    float3 light;      // Incident radiance
    float  weight;     // 1/nSamples
    float3 direction;  // Sample direction (for MIS: BSDF PDF computation)
    float  lightPdf;   // Light sampling PDF (for MIS weight)
};

// Debug counters

// Raym0nade convention: scale bsdfPdf (the throughput/bounce weight)
static void scaling(std::vector<RadianceSample>& samples, float factor) {
    for (auto& s : samples) s.bsdfPdf = s.bsdfPdf * factor;
}

// Raym0nade convention:
//   light *= bsdfPdf   (bake the current accumulated throughput into light)
//   bsdfPdf = new_val  (bsdfPdf now represents "what comes next")
static void passBsdf(std::vector<RadianceSample>& samples, const float3& bsdfPdf) {
    for (auto& s : samples) {
        s.light = s.light * s.bsdfPdf;
        s.bsdfPdf = bsdfPdf;
    }
}

static void decomposeRadiance(const float3& baseColor,
                              const RadianceSample& sample,
                              float3& rd, float3& rs) {
    float3 contrib = sample.bsdfPdf * sample.light * sample.weight;

    // NaN/Inf guard
    if (contrib.x != contrib.x || contrib.y != contrib.y || contrib.z != contrib.z) return;
    if (contrib.x > 1e10f || contrib.y > 1e10f || contrib.z > 1e10f) return;
    if (contrib.x < -1e10f || contrib.y < -1e10f || contrib.z < -1e10f) return;

    if (glm::length(contrib) < 1e-8f) return;

    // Firefly clamp per-sample: clamp BOTH luminance AND individual channels
    // to prevent bright single-channel artifacts from colored light sources.
    constexpr float CLAMP = 10.0f;
    if (contrib.x > CLAMP) contrib.x = CLAMP;
    if (contrib.y > CLAMP) contrib.y = CLAMP;
    if (contrib.z > CLAMP) contrib.z = CLAMP;
    float lum = contrib.x * RGB_WEIGHT[0] + contrib.y * RGB_WEIGHT[1] + contrib.z * RGB_WEIGHT[2];

    float baseLum = baseColor.x * RGB_WEIGHT[0] + baseColor.y * RGB_WEIGHT[1] + baseColor.z * RGB_WEIGHT[2];
    if (baseLum < 1e-4f) {
        rs += contrib;
        return;
    }
    float3 diffusePart = baseColor * (lum / (baseLum + 1e-8f));
    float diffDot = glm::dot(contrib, diffusePart);
    float contribLen2 = glm::dot(contrib, contrib);
    if (contribLen2 > 1e-8f) {
        float diffRatio = glm::clamp(diffDot / contribLen2, 0.0f, 1.0f);
        rd += contrib * diffRatio;
        rs += contrib * (1.0f - diffRatio);
    } else {
        rd += contrib;
    }
}

// Debug counters

// =========================================================================
// Next Event Estimation (NEE)
// =========================================================================
static std::vector<RadianceSample> sampleDirectLight(
    const BSDF& bsdf, const Model& model, Generator& gen, int sampleCnt) {

    std::vector<RadianceSample> result;
    const auto& lights = model.lightObjects();
    if (lights.empty()) {
        static std::atomic<int> warn{0};
        return result;
    }

    Distribution1D lightDistrib;
    {
        std::vector<float> powers(lights.size());
        for (size_t i = 0; i < lights.size(); ++i)
            powers[i] = fmaxf(1e-8f, lights[i].power);
        lightDistrib.build(powers);
    }
    float totalPower = lightDistrib.getSum();
    if (totalPower <= 0.0f) return result;

    const float3& pos = bsdf.surface.position;
    result.reserve(size_t(sampleCnt));

    for (int T = 0; T < sampleCnt; ++T) {
        float lightPdf;
        float sampleVal = lightDistrib.sample(gen(), lightPdf);
        int lightIdx = std::min(static_cast<int>(sampleVal * lights.size()),
                                static_cast<int>(lights.size()) - 1);
        float discLightPdf = lightPdf / totalPower;
        if (discLightPdf <= 0.0f) {
            continue;
        }

        LightSample ls = lights[lightIdx].sample(gen, pos);
        if (ls.pdf <= 0.0f) {
            continue;
        }

        const float3 wi = ls.wi;
        const float NdotL = fmaxf(0.0f, glm::dot(bsdf.surface.surfaceNormal, wi));

        const float dist = glm::length(ls.position - pos);
        if (dist < 1e-6f) {
            continue;
        }
        const Ray shadowRay(pos + wi * EPS_ZERO, wi);

        // Shadow test: check if light is occluded
        if (model.rayOccluded(shadowRay, dist - EPS_ZERO)) continue;

        const float pdfFinal = ls.pdf * discLightPdf;
        if (pdfFinal <= 0.0f) {
            continue;
        }

        const float3 bsdfVal = bsdf.eval(wi);

        // Raym0nade convention:
        //   bsdfPdf = BSDF value (NOT divided by sample PDF)
        //   light   = emitted radiance × NdotL / sampling_PDF  (bakes the geometry
        //             term and the PDF into the light side)
        // After passBsdf (which does light *= bsdfPdf), light becomes
        //   emission × NdotL / pdf × BSDF  = the full NEE contribution.
        float3 lightContrib = ls.emission * NdotL / pdfFinal;
        if (lightContrib.x != lightContrib.x || lightContrib.y != lightContrib.y || lightContrib.z != lightContrib.z) continue;
        if (lightContrib.x > 1e10f || lightContrib.y > 1e10f || lightContrib.z > 1e10f) continue;
        if (lightContrib.x < -1e10f || lightContrib.y < -1e10f || lightContrib.z < -1e10f) continue;

        // Firefly clamp on the light contribution.
        // Glossy/metal surfaces (low roughness) can produce extreme values when
        // the NEE direction aligns with the narrow specular lobe while the
        // uniform light PDF stays low. Match the GPU: tight clamp (5) for
        // low-roughness (rough < 0.3), looser clamp (20) otherwise.
        float lClum = fmaxf(lightContrib.x, fmaxf(lightContrib.y, lightContrib.z));
        const float NEE_CLAMP = (bsdf.surface.roughness < 0.3f) ? 5.0f : 20.0f;
        if (lClum > NEE_CLAMP)
            lightContrib = lightContrib * (NEE_CLAMP / lClum);

        float w = 1.0f / static_cast<float>(sampleCnt);
        result.push_back({bsdfVal, lightContrib, w, wi, pdfFinal});
    }
    return result;
}

// ---- Compute light sampling PDF for a given direction on a light surface ----
// Used for MIS weighting when BSDF path hits a light source.
static float computeLightPdf(const Model& model, const float3& hitPos,
                              const float3& hitNormal, const float3& dir) {
    float totalPower = 0.0f;
    for (const auto& lo : model.lightObjects())
        totalPower += fmaxf(1e-8f, lo.power);
    if (totalPower <= 0.0f) return 0.0f;

    float pdf = 0.0f;
    for (const auto& lo : model.lightObjects()) {
        float lightProb = fmaxf(1e-8f, lo.power) / totalPower;
        for (const Face* facePtr : lo.faces) {
            const Face& f = *facePtr;
            const float3 e1 = f.v[1] - f.v[0], e2 = f.v[2] - f.v[0];
            float area = 0.5f * glm::length(glm::cross(e1, e2));
            if (area < 1e-12f) continue;
            float geom = fabsf(glm::dot(dir, glm::normalize(glm::cross(e1, e2))));
            float facePdf = lightProb * (1.0f / area);
            float distSq = glm::dot(hitPos - f.v[0], hitPos - f.v[0]);
            pdf += facePdf * distSq / fmaxf(geom, 1e-8f);
        }
    }
    return pdf;
}

// =========================================================================
// Recursive Path Tracing — sampleRay (Raym0nade style)
// =========================================================================
// Returns RadianceSamples with bsdfPdf already multiplied by path throughput.
// prevBsdfPdf: BSDF PDF for the incoming direction at the previous surface
//              (used for MIS weighting when BSDF path hits a light source).
static std::vector<RadianceSample> sampleRay(
    const Model& model, Ray ray,
    uint32_t& seed, int depth, bool excludeDirect,
    Medium& medium,
    float prevBsdfPdf = 0.0f) {

    seed = seed * 1664525u + 1013904223u;
    Generator rng(seed);

    constexpr int MAX_DEPTH = 16;
    HitRecord hit = model.rayHit(ray);

    // ---- Sky / miss ----
    if (!hit.hasHit()) {
        if (!model.skyMap().empty() && !excludeDirect)
            return {{float3(1.0f), model.skyMap().sample(ray.direction), 1.0f, float3(0.0f), 0.0f}};
        return {};
    }

    // ---- First intersection ----
    const HitInfo info = getHitInfo(model, ray, hit);

    // Beer's law: absorption over the distance traveled in the current medium
    const float3 pathAbsorb = getAbsorb(medium.absorb(), hit.t);

    // ---- Emissive surface (with MIS weight) ----
    if (glm::length(info.emission) > 1e-6f) {
        if (excludeDirect) return {};
        // MIS balance heuristic: w_bsdf = pdf_bsdf / (pdf_bsdf + pdf_light)
        // prevBsdfPdf = BSDF PDF that produced the direction hitting this light
        // pdf_light = light sampling PDF for this direction (computed from light geometry)
        float misWeight = 1.0f;
        if (prevBsdfPdf > 1e-8f) {
            float pdfLight = computeLightPdf(model, info.position, info.surfaceNormal, -ray.direction);
            float denom = prevBsdfPdf + pdfLight;
            misWeight = (denom > 1e-8f) ? (prevBsdfPdf / denom) : 1.0f;
        }
        std::vector<RadianceSample> result;
        result.push_back({pathAbsorb * misWeight, info.emission, 1.0f, float3(0.0f), 0.0f});
        return result;
    }

    if (depth >= MAX_DEPTH) return {};

    const float3 inDir = -ray.direction;
    const float3 N = info.surfaceNormal;
    const float3 V = inDir;

    // ---- BSDF & Russian Roulette ----
    BSDF bsdf(inDir, info);

    const float P_RR_Rough = 0.5f, P_RR_Smooth = 1.0f;
    const float P_RR = P_RR_Smooth + (P_RR_Rough - P_RR_Smooth) * sqrtf(info.roughness);
    constexpr float P_RR_THRESH = 0.9f;
    const bool isDielectric = info.opacity < 1e-6f;
    const bool doNEE = !excludeDirect && (P_RR < P_RR_THRESH) && !isDielectric;

    // ---- Handle dielectrics: compute reflection/refraction probability ----
    float P_reflect = 1.0f;
    float Fresnel = 0.0f;

    if (isDielectric) {
        // Compute relative IOR for Fresnel
        // Simple case: air (1.0) ↔ glass (info.eta)
        float relEta;
        if (info.entering) {
            // Ray entering the object (outside → inside)
            relEta = 1.0f / fmaxf(1.0f, info.eta);
        } else {
            // Ray exiting the object (inside → outside)
            relEta = fmaxf(1.0f, info.eta) / 1.0f;
        }
        Fresnel = bsdf.fresnelDielectric(fabsf(glm::dot(-ray.direction, info.surfaceNormal)), relEta);

        // When in air, bias toward reflection for more samples
        if (medium.size() == 1)
            P_reflect = 0.24f + (1.0f - 0.24f) * Fresnel;
        else
            P_reflect = Fresnel;
        P_reflect = fmaxf(P_reflect - 1e-3f, 0.0f);
    }

    // ---- Accumulate contributions from this bounce ----
    std::vector<RadianceSample> result;

    // ---- NEE at EVERY bounce with full MIS ----
    // Balance heuristic: w_nee = pdf_light / (pdf_light + pdf_bsdf)
    // NEE samples light with pdf_light; BSDF would sample same direction with pdf_bsdf.
    if (doNEE) {
        auto neeResult = sampleDirectLight(bsdf, model, rng, 1);
        for (auto& smp : neeResult) {
            // Compute BSDF PDF for the NEE direction
            float bsdfPdfNee = bsdf.pdf(smp.direction);
            float denom = smp.lightPdf + bsdfPdfNee;
            float misWeight = (denom > 1e-8f) ? (smp.lightPdf / denom) : 1.0f;
            // Scale light contribution by MIS weight
            smp.light = smp.light * misWeight;
        }
        result = neeResult;
    }

    // ---- Russian Roulette: continue path? ----
    // With probability P_RR, continue with BSDF sampling.
    // With probability 1-P_RR, terminate (return NEE-only result).
    bool continuePath = true;
    if (doNEE) {
        continuePath = (rng() < P_RR);
    }
    if (depth >= MAX_DEPTH) continuePath = false;

    if (!continuePath) {
        // NEE is unbiased (always computed); no scaling needed.
        return result;
    }

    // ---- BSDF sample to continue path ----
    float regularizedRoughness = fmaxf(info.roughness, 0.01f * float(depth + 1));
    HitInfo regInfo = info;
    regInfo.roughness = regularizedRoughness;
    BSDF regBsdf(inDir, regInfo);

    float3 outDir;
    float  pdf;
    float3 bsdfValDivPdf = regBsdf.sample(rng(), rng(), outDir, pdf);

    if (pdf <= 1e-8f || glm::length(bsdfValDivPdf) <= 1e-8f) return result;

    // For dielectrics: skip NdotL check (refraction goes through the surface)
    // For opaque: check that direction is on the correct side of the normal
    if (!isDielectric) {
        float NdotL = fmaxf(0.0f, glm::dot(N, outDir));
        if (NdotL <= 0.0f) return result;
    }

    // For dielectrics: determine if this is a reflection or refraction event
    if (isDielectric) {
        bool isReflection = (glm::dot(N, outDir) > 0.0f);

        if (isReflection) {
            bsdfValDivPdf = pathAbsorb;
        } else {
            // ---- Refraction ----
            if (info.entering) {
                medium.insert(info.materialID, info.eta, info.baseColor);
            } else {
                medium.erase(info.materialID);
            }
            bsdfValDivPdf = pathAbsorb;
        }
    } else {
        // ---- Opaque: standard throughput ----
        float NdotL = fmaxf(1e-8f, glm::dot(N, outDir));
        float3 childThroughput = bsdfValDivPdf * NdotL / pdf;

        // NaN/Inf guard
        if (childThroughput.x != childThroughput.x ||
            childThroughput.y != childThroughput.y ||
            childThroughput.z != childThroughput.z) return result;
        if (childThroughput.x > 1e10f || childThroughput.y > 1e10f ||
            childThroughput.z > 1e10f) return result;

        // Firefly clamp
        float tLum = childThroughput.x * RGB_WEIGHT[0] + childThroughput.y * RGB_WEIGHT[1] + childThroughput.z * RGB_WEIGHT[2];
        constexpr float THROUGHPUT_CLAMP = 500.0f;
        if (tLum > THROUGHPUT_CLAMP)
            childThroughput = childThroughput * (THROUGHPUT_CLAMP / tLum);

        // Scale by survival probability (1/P_RR) to maintain unbiased estimate.
        // The path continues with probability P_RR, so surviving paths get
        // boosted by 1/P_RR to compensate for those that were terminated.
        if (doNEE && P_RR > 1e-4f)
            childThroughput = childThroughput / P_RR;

        bsdfValDivPdf = childThroughput;
    }

    Ray newRay(info.position + outDir * EPS_ZERO, outDir);
    // Pass BSDF PDF for the current direction as prevBsdfPdf for next bounce's MIS.
    // When the child ray hits a light, it uses this PDF for MIS weight.
    float childBsdfPdf = isDielectric ? 0.0f : pdf;
    std::vector<RadianceSample> children =
        sampleRay(model, newRay, seed, depth + 1, false, medium, childBsdfPdf);
    if (children.empty()) return result;

    passBsdf(children, bsdfValDivPdf);
    for (auto& c : children)
        result.push_back(c);

    return result;
}

// =========================================================================
// Per-pixel rendering (two-stage: direct NEE + indirect BSDF recursion)
// =========================================================================
void renderPixel(const Model& model, const RenderArgs& args,
                 int x, int y, uint32_t seed, PerPixelData& out) {

    out = PerPixelData{};

    auto hashSeed = [](uint32_t x, uint32_t y, uint32_t sample, uint32_t base) -> uint32_t {
        uint32_t h = base;
        h ^= x * 0x9e3779b9u;
        h ^= y * 0x85ebca6bu;
        h ^= sample * 0xc2b2ae35u;
        h ^= h >> 16;
        h *= 0x85ebca6bu;
        h ^= h >> 13;
        h *= 0xc2b2ae35u;
        h ^= h >> 16;
        return h;
    };

    const int w = args.width, h = args.height;

    float3 acc_Dd{0}, acc_Ds{0}, acc_Id{0}, acc_Is{0}, acc_em{0};
    float  vDd = 0, vDs = 0, vId = 0, vIs = 0;
    float3 baseColor{0}, normal{0};
    bool   hasBC = false;

    for (int s = 0; s < args.spp; ++s) {
        uint32_t sampleSeed = hashSeed(x, y, s, seed);
        Generator rng(sampleSeed);

        const float jitterU = rng(), jitterV = rng();
        const float lensU   = rng() * 2.0f - 1.0f;
        const float lensV   = rng() * 2.0f - 1.0f;

        Ray ray = args.camera.generateRay(
            float(x), float(y), jitterU, jitterV,
            lensU, lensV, float2(float(w), float(h)));

        // Compute initial ray differential for mipmap level selection
        RayDifferential rayDiff;
        {
            const float aspect = float(w) / float(h);
            const float tanFov = tanf(args.camera.fov * 0.5f * PI / 180.0f);
            const float3 camDir = glm::normalize(args.camera.direction);
            const float3 camUp = glm::normalize(args.camera.up);
            const float3 camRight = glm::normalize(glm::cross(camDir, camUp));

            const float ndcX = (2.0f * (float(x) + jitterU) / float(w) - 1.0f) * aspect * tanFov;
            const float ndcY = (1.0f - 2.0f * (float(y) + jitterV) / float(h)) * tanFov;

            const float3 d = ndcX * camRight + ndcY * camUp + camDir;
            const float3 dddx = (2.0f / float(w)) * aspect * tanFov * camRight;
            const float3 dddy = (2.0f / float(h)) * tanFov * camUp;

            const float d_dot_d = glm::dot(d, d);
            const float d_dot_dddx = glm::dot(d, dddx);
            const float d_dot_dddy = glm::dot(d, dddy);

            rayDiff.dPdx = float3(0.0f);
            rayDiff.dPdy = float3(0.0f);
            rayDiff.dDdx = (d_dot_d * dddx - d * d_dot_dddx) / (sqrtf(d_dot_d) * d_dot_d);
            rayDiff.dDdy = (d_dot_d * dddy - d * d_dot_dddy) / (sqrtf(d_dot_d) * d_dot_d);
        }

        HitRecord hit = model.rayHit(ray);
        if (!hit.hasHit()) {
            if (!model.skyMap().empty())
                acc_em += model.skyMap().sample(ray.direction);
            continue;
        }

        const HitInfo info = getHitInfo(model, ray, hit, &rayDiff);
        if (!hasBC) { baseColor = info.baseColor; normal = info.surfaceNormal; hasBC = true; }

        if (glm::length(info.emission) > 1e-6f) {
            acc_em += info.emission;
            continue;
        }

        const float3 viewDir = -ray.direction;

        // ---- First-hit NEE (direct lighting from the first surface) ----
        // sampleRay starts from the SECOND bounce, so its NEE only covers
        // bounces ≥ 2. We must compute direct light for bounce 1 here.
        {
            Medium emptyMedium;
            emptyMedium.init();
            BSDF firstBsdf(viewDir, info);
            auto directSamples = sampleDirectLight(firstBsdf, model, rng, 1);
            for (auto& smp : directSamples) {
                decomposeRadiance(info.baseColor, smp, acc_Dd, acc_Ds);
            }
        }

        // ---- Path tracing with NEE at every bounce ----

        // Initialize medium stack for this path
        Medium medium;
        medium.init();
        BSDF bsdf(viewDir, info);
        const bool isFirstHitDielectric = (info.opacity < 1e-6f);

        float3 outDir;
        float pdf;
        float3 bsdfVal = bsdf.sample(rng(), rng(), outDir, pdf);

        if (pdf > 1e-8f && glm::length(bsdfVal) > 1e-8f) {
            // For dielectrics, skip NdotL check (refraction goes through surface)
            bool validDir = false;
            if (isFirstHitDielectric) {
                validDir = true;
                // Track medium: if refracting, update stack
                if (glm::dot(info.surfaceNormal, outDir) <= 0.0f) {
                    // Refraction: ray passes through the surface
                    if (info.entering)
                        medium.insert(info.materialID, info.eta, info.baseColor);
                    else
                        medium.erase(info.materialID);
                }
            } else {
                float NdotL = fmaxf(0.0f, glm::dot(info.surfaceNormal, outDir));
                validDir = (NdotL > 0.0f);
            }

            if (validDir) {
                float3 childThru = bsdfVal;
                if (!isFirstHitDielectric) {
                    float NdotL = fmaxf(0.0f, glm::dot(info.surfaceNormal, outDir));
                    childThru = bsdfVal * NdotL / pdf;
                }
                // Firefly clamp on throughput (matching GPU: 500)
                float tLum = childThru.x * RGB_WEIGHT[0] + childThru.y * RGB_WEIGHT[1] + childThru.z * RGB_WEIGHT[2];
                constexpr float THROUGHPUT_CLAMP = 500.0f;
                if (tLum > THROUGHPUT_CLAMP)
                    childThru = childThru * (THROUGHPUT_CLAMP / tLum);
                // NaN/Inf guard
                if (childThru.x != childThru.x || childThru.y != childThru.y || childThru.z != childThru.z) continue;
                if (childThru.x > 1e10f || childThru.y > 1e10f || childThru.z > 1e10f) continue;
                Ray newRay(info.position + outDir * EPS_ZERO, outDir);
                uint32_t childSeed = sampleSeed;
                // KEY FIX: pass excludeDirect=false so sampleRay enables
                // NEE at every subsequent bounce. Previously the entire
                // indirect chain ran without NEE, forcing BSDF random
                // sampling to chase the tiny ceiling light (almost never
                // hit), producing massive variance that manifested as the
                // heavy grain and black blotches on the right tall box.
                // Pass BSDF PDF for MIS weighting when child hits a light.
                float childBsdfPdf = isFirstHitDielectric ? 0.0f : pdf;
                auto children = sampleRay(model, newRay, childSeed, 1, false, medium, childBsdfPdf);
                passBsdf(children, childThru);
                for (auto& smp : children) {
                    decomposeRadiance(info.baseColor, smp, acc_Id, acc_Is);
                    float3 c = smp.bsdfPdf * smp.light * smp.weight;
                    float lu = c.x*RGB_WEIGHT[0] + c.y*RGB_WEIGHT[1] + c.z*RGB_WEIGHT[2];
                    vId += lu * lu;
                }
            }
        }
    }

float invS = 1.0f / float(args.spp), invS2 = invS * invS;
out.radiance_Dd = acc_Dd * invS;    out.radiance_Ds = acc_Ds * invS;
out.radiance_Id = acc_Id * invS;    out.radiance_Is = acc_Is * invS;
out.emission    = acc_em * invS;
out.baseColor   = baseColor;
out.normal      = normal;
out.var_Dd = fmaxf(0.0f, vDd * invS2 - glm::dot(out.radiance_Dd, out.radiance_Dd));
out.var_Ds = fmaxf(0.0f, vDs * invS2 - glm::dot(out.radiance_Ds, out.radiance_Ds));
out.var_Id = fmaxf(0.0f, vId * invS2 - glm::dot(out.radiance_Id, out.radiance_Id));
out.var_Is = fmaxf(0.0f, vIs * invS2 - glm::dot(out.radiance_Is, out.radiance_Is));
}

// =========================================================================
// Multi-threaded CPU rendering
// =========================================================================
void renderCPU(const Model& model, const RenderArgs& args,
           std::vector<PerPixelData>& output) {
const int w = args.width, h = args.height;
output.resize(size_t(w) * h);

const int numThreads = (args.numThreads > 0)
    ? args.numThreads
    : static_cast<int>(std::thread::hardware_concurrency());

std::vector<std::thread> threads;
std::atomic<int> nextTileRow(0);
constexpr int TILE_ROWS = 16;
const long numRows = (long(h) + TILE_ROWS - 1) / TILE_ROWS;
std::atomic<long> completed{0};
std::atomic<bool> stopReport{false};
ProgressBar bar(numRows);
std::thread reporter([&]() {
    while (!stopReport.load()) {
        bar.update(completed.load());
        std::this_thread::sleep_for(std::chrono::milliseconds(66));
    }
});

for (int t = 0; t < numThreads; ++t) {
    threads.emplace_back([&]() {
        while (true) {
            const int rowStart = nextTileRow.fetch_add(TILE_ROWS);
            if (rowStart >= h) break;
            const int rowEnd = glm::min(rowStart + TILE_ROWS, h);
            for (int y = rowStart; y < rowEnd; ++y)
                for (int x = 0; x < w; ++x)
                    renderPixel(model, args, x, y, 0xABCDu, output[size_t(y) * w + x]);
            completed.fetch_add(1);
        }
    });
}
for (auto& th : threads) th.join();
stopReport.store(true);
reporter.join();
bar.update(numRows);
}
