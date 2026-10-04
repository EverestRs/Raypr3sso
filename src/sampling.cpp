#include "sampling.h"
#include "geometry.h"
#include <cmath>

// ---- BSDF Regularization ----
// Prevents fireflies from near-specular surfaces at low sample counts.
// Returns the regularized roughness for the given path depth.
static float regularizeRoughness(float roughness, int depth) {
    constexpr float regularizationFactor = 0.01f;
    return fmaxf(roughness, regularizationFactor * float(depth));
}

BSDF::BSDF(const float3& inDir, const HitInfo& info)
    : inDir(inDir), surface(info) {}

float BSDF::GTR1(float NdotH, float a) const {
    if (a >= 1.0f) return INV_PI;
    const float a2 = a * a;
    const float denom = (NdotH * NdotH * (a2 - 1.0f) + 1.0f);
    return (a2 - 1.0f) / (PI * logf(a2) * denom);
}

float BSDF::GTR2(float NdotH, float a) const {
    const float a2 = a * a;
    const float denom = (NdotH * NdotH * (a2 - 1.0f) + 1.0f);
    return a2 / (PI * denom * denom);
}

float BSDF::smithG(float NdotV, float alpha) const {
    const float a = alpha * alpha;
    const float b = NdotV * NdotV;
    return 2.0f / (1.0f + sqrtf(1.0f + a * (1.0f - b) / (b + 1e-8f)));
}

float BSDF::schlickFresnel(float cosTheta) const {
    const float x = fmaxf(0.0f, 1.0f - cosTheta);
    return x * x * x * x * x;
}

// ---- Local frame helpers ----
namespace {

void localFrame(const float3& normal, float3& tangent, float3& bitangent) {
    getTangentSpace(normal, tangent, bitangent);
}

float3 toLocal(const float3& v, const float3& tangent,
               const float3& bitangent, const float3& normal) {
    return float3(glm::dot(v, tangent),
                  glm::dot(v, bitangent),
                  glm::dot(v, normal));
}

float3 toWorld(const float3& v, const float3& tangent,
               const float3& bitangent, const float3& normal) {
    return v.x * tangent + v.y * bitangent + v.z * normal;
}

float3 sampleCosineHemisphere(float u1, float u2, float& pdf) {
    const float r     = sqrtf(u1);
    const float theta = 2.0f * PI * u2;
    const float x     = r * cosf(theta);
    const float y     = r * sinf(theta);
    const float z     = sqrtf(fmaxf(0.0f, 1.0f - x * x - y * y));
    pdf = z * INV_PI;
    return float3(x, y, z);
}

float3 sampleGGXHalf(float u1, float u2, float a, float& pdfH) {
    const float phi = 2.0f * PI * u1;
    const float cosTheta = sqrtf((1.0f - u2) / (1.0f + (a * a - 1.0f) * u2));
    const float sinTheta = sqrtf(fmaxf(0.0f, 1.0f - cosTheta * cosTheta));
    const float a2 = a * a;
    const float denom = (cosTheta * cosTheta * (a2 - 1.0f) + 1.0f);
    pdfH = (a2 / (PI * denom * denom)) * cosTheta;
    return float3(sinTheta * cosf(phi), sinTheta * sinf(phi), cosTheta);
}

float3 sampleGTR1(const float3& Vlocal, float u1, float u2, float a) {
    const float phi = 2.0f * PI * u1;
    // Inverse CDF of D_GTR1: c^2 = (a2^u - 1)/(a2-1) = (1 - a2^(1-u))/(1-a2)
    const float cosTheta = sqrtf((1.0f - powf(a*a, 1.0f - u2)) / (1.0f - a*a));
    const float sinTheta = sqrtf(fmaxf(0.0f, 1.0f - cosTheta * cosTheta));
    float3 Hlocal = float3(sinTheta * cosf(phi), sinTheta * sinf(phi), cosTheta);
    // Reflect
    float VdotH = Vlocal.x*Hlocal.x + Vlocal.y*Hlocal.y + Vlocal.z*Hlocal.z;
    return -Vlocal + 2.0f * VdotH * Hlocal;
}

} // anonymous namespace

// ---- Helper exposed for render.cpp / other files ----
float schlickFresnelPublic(float cosTheta) {
    const float x = fmaxf(0.0f, 1.0f - cosTheta);
    return x * x * x * x * x;
}

// ---- Full Disney BSDF Evaluation ----
// Implements: diffuse (subsurface), sheen, specular (GTR2/GGX), clearcoat (GTR1)
// For dielectrics (glass), returns 0 for eval (delta distribution).
float3 BSDF::eval(const float3& outDir) const {
    // Dielectric BSDF is a delta distribution; direct eval is 0
    if (isDielectric()) return float3(0.0f);

    const float3 N = surface.surfaceNormal;
    const float3 V = inDir;
    const float3 L = outDir;

    const float NdotV = fmaxf(1e-6f, glm::dot(N, V));
    const float NdotL = fmaxf(1e-6f, glm::dot(N, L));

    const float3 H = glm::normalize(V + L);
    const float NdotH = fmaxf(1e-6f, glm::dot(N, H));
    const float LdotH = fmaxf(1e-6f, glm::dot(L, H));

    const float roughness = fmaxf(0.001f, surface.roughness);
    const float alpha = roughness * roughness;

    float3 result(0.0f);
    const float metallic = surface.metallic;

    // ---- Diffuse (Disney principled, with subsurface) ----
    if (metallic < 1.0f) {
        const float3 Cdlin = surface.baseColor;
        const float Cdlum = Cdlin.x*RGB_WEIGHT[0] + Cdlin.y*RGB_WEIGHT[1] + Cdlin.z*RGB_WEIGHT[2];
        const float3 Ctint = (Cdlum > 1e-4f) ? Cdlin / Cdlum : float3(1.0f);

        // Diffuse Fresnel — retro-reflection based on roughness
        const float FL = schlickFresnel(NdotL);
        const float FV = schlickFresnel(NdotV);
        const float Fd90 = 0.5f + 2.0f * LdotH * LdotH * roughness;
        const float Fd = glm::mix(1.0f, Fd90, FL) * glm::mix(1.0f, Fd90, FV);

        // Subsurface (Hanrahan-Krueger approximation)
        const float Fss90 = LdotH * LdotH * roughness;
        const float Fss = glm::mix(1.0f, Fss90, FL) * glm::mix(1.0f, Fss90, FV);
        const float ss = 1.25f * (Fss * (1.0f / (NdotL + NdotV + 1e-8f) - 0.5f) + 0.5f);

        const float3 diffuseTerm = INV_PI * glm::mix(Fd, ss, surface.subsurface) * Cdlin;
        result += (1.0f - metallic) * diffuseTerm;

        // Sheen (fabric/velvet)
        if (surface.sheen > 0.0f) {
            const float3 Csheen = glm::mix(float3(1.0f), Ctint, surface.sheenTint);
            result += (1.0f - metallic) * surface.sheen * schlickFresnel(NdotL) * Csheen * INV_PI;
        }
    }

    // ---- Specular (GTR2/GGX) ----
    {
        const float3 Cspec0 = glm::mix(float3(surface.specular), surface.baseColor, metallic);
        const float D  = GTR2(NdotH, alpha);
        const float3 F = Cspec0 + (1.0f - Cspec0) * schlickFresnel(LdotH);
        const float G  = smithG(NdotV, alpha) * smithG(NdotL, alpha);
        result += D * F * G / (4.0f * NdotV * NdotL + 1e-8f);
    }

    // ---- Clearcoat (GTR1) ----
    if (surface.clearcoat > 0.0f) {
        const float alphaCC = glm::mix(0.1f, 0.001f, surface.clearcoatGloss);
        const float Dr = GTR1(NdotH, alphaCC);
        const float Fr = glm::mix(0.04f, 1.0f, schlickFresnel(LdotH));
        const float Gr = smithG(NdotV, 0.25f) * smithG(NdotL, 0.25f);
        result += 0.25f * surface.clearcoat * Gr * Fr * Dr;
    }

    return result;
}

float BSDF::pdf(const float3& outDir) const {
    const float3 N = surface.surfaceNormal;
    const float3 V = inDir;
    const float3 L = outDir;

    const float NdotL = fmaxf(1e-6f, glm::dot(N, L));
    const float3 H = glm::normalize(V + L);
    const float NdotH = fmaxf(1e-6f, glm::dot(N, H));
    const float VdotH = fmaxf(1e-6f, glm::dot(V, H));

    const float alpha = fmaxf(0.001f, surface.roughness * surface.roughness);

    const float pdfDiffuse = NdotL * INV_PI;
    const float pdfHalf = GTR2(NdotH, alpha) * NdotH;
    const float pdfSpecular = pdfHalf / (4.0f * VdotH + 1e-8f);

    // Clearcoat lobe PDF (GTR1) — must be included for MIS weight consistency
    // with sample() which samples clearcoat as a separate lobe.
    float pdfClearcoat = 0.0f;
    if (surface.clearcoat > 0.0f) {
        float alphaCC = glm::mix(0.1f, 0.001f, surface.clearcoatGloss);
        float ccPdfHalf = GTR1(NdotH, alphaCC) * NdotH;
        pdfClearcoat = ccPdfHalf / (4.0f * VdotH + 1e-8f);
    }

    // Blend lobe weights — must match sample() exactly.
    // A glossy non-metal (glass, stone, marble, plastic) must still sample a
    // GGX specular lobe or its highlights vanish. Weight the specular lobe by
    // smoothness (1-roughness) for non-metals, and fully for metals.
    const float nonMetal = 1.0f - surface.metallic;
    const float gloss = fmaxf(0.0f, 1.0f - surface.roughness);
    const float wDiffuse = nonMetal * (1.0f - gloss);
    const float wSpecular = surface.metallic + nonMetal * gloss;
    const float cc = fminf(surface.clearcoat, 1.0f);
    const float wClearcoat = cc * 0.25f;
    const float totalW = wDiffuse + wSpecular + wClearcoat;
    float pdf = 0.0f;
    if (totalW > 1e-8f) {
        pdf = (wDiffuse * pdfDiffuse + wSpecular * pdfSpecular + wClearcoat * pdfClearcoat) / totalW;
    }
    return pdf;
}

float3 BSDF::sample(float u1, float u2, float3& outDir, float& pdf) const {
    const float3 N = surface.surfaceNormal;
    const float3 V = inDir;

    float3 tangent, bitangent;
    localFrame(N, tangent, bitangent);

    const float3 Vlocal = toLocal(V, tangent, bitangent, N);
    const float alpha = fmaxf(0.001f, surface.roughness * surface.roughness);

    // ---- Handle dielectrics (glass/transparent) ----
    if (isDielectric()) {
        // Convention: V is the view direction (toward source, same hemisphere
        // as N). The new ray's TRAVEL direction is what we return as outDir.
        // For the specular reflection, the travel direction is the reflection
        // of -V about N (= 2*dot(V,N)*N - V). The old formula
        // `glm::reflect(V, N)` reflected V (the view direction) and pointed
        // the new ray INTO the surface, producing a thick black ring with
        // "black eye" artifacts on the CPU render.
        const float cosThetaI = glm::dot(V, N);  // positive: V and N same hemisphere
        const float etaI = surface.entering ? 1.0f : surface.eta;
        const float etaT = surface.entering ? surface.eta : 1.0f;
        const float relEta = etaI / etaT;
        const float F = fresnelDielectric(cosThetaI, relEta);

        if (u1 < F) {
            // ---- Reflect ----
            // outDir = reflect(-V, N) = 2*dot(V,N)*N - V  (TRAVEL direction)
            outDir = 2.0f * cosThetaI * N - V;
            outDir = glm::normalize(outDir);

            // PDF = Fresnel (the reflection event probability)
            pdf = F;

            // BSDF value = Fresnel * delta / cosTheta
            // For Monte Carlo: Fresnel / pdf = 1.0
            return float3(1.0f);
        } else {
            // ---- Refract ----
            float fresnelR;
            if (!refract(outDir, fresnelR)) {
                // TIR — all light reflected, use the (correct) reflection path
                outDir = 2.0f * cosThetaI * N - V;
                outDir = glm::normalize(outDir);
                pdf = 1.0f;
                return float3(1.0f);
            }

            // PDF = 1 - Fresnel (the transmission event probability)
            pdf = 1.0f - F;

            // BTDF = (1-F) * |cosThetaT / cosThetaI| * transmittingColor / pdf
            // For Monte Carlo: BTDF / pdf = 1.0 * transmittingColor
            // The |cosThetaT / cosThetaI| factor is handled by the path throughput
            // (since we're in the exact delta direction)
            if (surface.entering) {
                // Entering glass: apply Beer's law absorption
                // transmittance = exp(-absorption * distance)
                // For simplicity, use the transmitting color as the per-unit absorption
                return float3(1.0f);  // Absorption handled by medium tracking
            } else {
                // Exiting glass: no additional absorption
                return float3(1.0f);
            }
        }
    }

// ---- Opaque materials: choose lobe ----
    // Weight the GGX specular lobe by smoothness (1-roughness) for non-metals so
    // glossy glass/stone/marble still sample a GGX highlight (matches pdf()).
    const float gloss = fmaxf(0.0f, 1.0f - surface.roughness);
    const float wDiffuse = (1.0f - surface.metallic) * (1.0f - gloss);
    const float wSpecular = surface.metallic + (1.0f - surface.metallic) * gloss;
    if (u1 < wDiffuse) {
        // ---- Diffuse lobe ----
        const float u1m = u1 / fmaxf(wDiffuse, 1e-8f);
        float diffPdf;
        float3 Llocal = sampleCosineHemisphere(u1m, u2, diffPdf);
        if (Llocal.z < 0.0f) Llocal.z = -Llocal.z;
        outDir = toWorld(Llocal, tangent, bitangent, N);
        pdf = wDiffuse * diffPdf;
        // Return FULL BSDF (all lobes) for the sampled direction.
        // Critical for MIS consistency: NEE path evaluates the full BSDF,
        // so BSDF-sampled path must also return the full BSDF to keep the
        // two strategies estimating the same quantity.
        return eval(outDir);
    } else {
        // ---- Specular lobe ----
        const float u1m = (u1 - wDiffuse) / fmaxf(wSpecular, 1e-8f);
        float pdfH;
        float3 Hlocal = sampleGGXHalf(u1m, u2, alpha, pdfH);
        if (Hlocal.z < 0.0f) Hlocal.z = -Hlocal.z;

        const float VdotH = Vlocal.x*Hlocal.x + Vlocal.y*Hlocal.y + Vlocal.z*Hlocal.z;
        float3 Llocal = -Vlocal + 2.0f * VdotH * Hlocal;

        if (Llocal.z <= 0.0f) {
            pdf = 0.0f;
            return float3(0.0f);
        }
        outDir = toWorld(Llocal, tangent, bitangent, N);

        const float specPdf = pdfH / (4.0f * VdotH + 1e-8f);
        pdf = wSpecular * specPdf;
        // Return FULL BSDF (see diffuse lobe comment for rationale).
        return eval(outDir);
    }
}

// ---- Dielectric/glass helpers ----
bool BSDF::isDielectric() const {
    // A material is dielectric (glass-like) only if it is actually transparent
    // (opacity < 0.99).  Removing the emission-length fallback: a non-emissive
    // opaque material must NEVER be treated as dielectric just because it has
    // an IOR value set — that would make BSDF::eval() return 0, starving NEE.
    return surface.metallic < 0.5f &&
           surface.opacity < 0.99f &&
           (fabsf(surface.eta - 1.0f) > 0.01f);
}

float BSDF::fresnelDielectric(float cosTheta, float eta) const {
    // Full Fresnel equations for dielectrics
    // Handles both entering (eta = 1/ior) and exiting (eta = ior/1) cases
    float sin2Theta = fmaxf(0.0f, 1.0f - cosTheta * cosTheta);
    float sin2ThetaT = eta * eta * sin2Theta;
    if (sin2ThetaT >= 1.0f) return 1.0f;  // TIR

    float cosThetaT = sqrtf(1.0f - sin2ThetaT);
    float rParallel = (eta * cosTheta - cosThetaT) / (eta * cosTheta + cosThetaT);
    float rPerp = (cosTheta - eta * cosThetaT) / (cosTheta + eta * cosThetaT);
    return 0.5f * (rParallel * rParallel + rPerp * rPerp);
}

bool BSDF::refract(float3& outDir, float& fresnelReflectance) const {
    // Convention (matches PBRT and the GPU gpu_refract):
    //   - inDir is the VIEW direction (toward source, same hemisphere as N)
    //   - N is the surface normal pointing toward the incident side
    //   - cosThetaI = dot(inDir, N) is POSITIVE
    //   - Snell's formula:  wt = -eta * inDir + (eta*cosThetaI - cosThetaT) * N
    //     where wt is the transmitted TRAVEL direction
    // The previous implementation used -dot(inDir, N) for cosThetaI and
    // +eta*inDir in the formula, which produced a refracted ray on the wrong
    // side of the surface — making the glass sphere appear as an opaque
    // white/gray block on the CPU render.
    const float3 N = surface.surfaceNormal;
    const float cosThetaI = glm::dot(inDir, N);
    const float etaI = surface.entering ? 1.0f : surface.eta;
    const float etaT = surface.entering ? surface.eta : 1.0f;
    const float eta = etaI / etaT;  // relative IOR

    // Compute Fresnel reflectance (cosThetaI is positive by convention)
    fresnelReflectance = fresnelDielectric(cosThetaI, eta);

    // Snell's law
    const float sin2ThetaI = fmaxf(0.0f, 1.0f - cosThetaI * cosThetaI);
    const float sin2ThetaT = eta * eta * sin2ThetaI;
    if (sin2ThetaT >= 1.0f) {
        // Total Internal Reflection
        fresnelReflectance = 1.0f;
        return false;
    }

    const float cosThetaT = sqrtf(1.0f - sin2ThetaT);
    outDir = -eta * inDir + (eta * cosThetaI - cosThetaT) * N;
    outDir = glm::normalize(outDir);
    return true;
}

float3 BSDF::evalTransmission(const float3& outDir) const {
    (void)outDir;
    // For a perfect (delta) BTDF with Fresnel weighting:
    // BTDF = (1 - Fresnel) * |cosThetaT / cosThetaI| * transmissionColor
    // Since we evaluate at the sampled direction (which is the exact refraction direction),
    // the BTDF value is a delta distribution scaled by (1-F) / pdf.
    // For Monte Carlo, we return the BSDF value at the sample point.
    // Return 0 here; the actual contribution is computed in sample().
    return float3(0.0f);
}

float BSDF::pdfTransmission(const float3& outDir) const {
    (void)outDir;
    // Delta distribution: the PDF is 0 everywhere except at the exact refraction direction.
    return 0.0f;
}
