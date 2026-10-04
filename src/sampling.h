#pragma once

#include "../include/types.h"

// ---- Hit Surface Info ----
struct HitInfo {
    float3 position;
    float3 shapeNormal;
    float3 surfaceNormal;
    float3 baseColor;
    float3 emission;
    float  specular;
    float  roughness;
    float  metallic;
    float  opacity;
    float  eta;           // Index of refraction (1.0 = air)
    float  subsurface = 0.0f;  // Disney subsurface [0,1]
    float  sheen      = 0.0f;  // Disney sheen [0,1]
    float  sheenTint  = 0.5f;  // Disney sheen tint [0,1]
    float  clearcoat  = 0.0f;  // Disney clearcoat [0,1]
    float  clearcoatGloss = 0.0f; // Disney clearcoat glossiness [0,1]
    int    materialID;
    bool   entering;    // Whether ray is entering from outside
};

// ---- Free function: Schlick Fresnel falloff (shared with render.cpp) ----
float schlickFresnelPublic(float cosTheta);

#ifndef __CUDACC__
// ---- BSDF Evaluation & Sampling ----
class BSDF {
public:
    float3 inDir;        // Incident direction (pointing toward surface)
    HitInfo surface;

    BSDF(const float3& inDir, const HitInfo& info);

    // BRDF / BTDF evaluation
    float3 eval(const float3& outDir) const;
    float  pdf(const float3& outDir) const;

    // Importance sampling
    // Returns BSDF value / PDF, outputs sampled direction and PDF
    float3 sample(float u1, float u2, float3& outDir, float& pdf) const;

    // Glass: Fresnel dielectric + refraction
    // Returns true if the surface is dielectric (glass-like), false otherwise
    bool isDielectric() const;

    // Compute Fresnel reflectance for dielectric (using IOR)
    // Returns reflectance R in [0,1] for the given cosTheta
    float fresnelDielectric(float cosTheta, float eta) const;

    // Compute refraction direction using Snell's law
    // Returns true if refraction is valid (no TIR), fills outDir and fresnelReflectance
    bool refract(float3& outDir, float& fresnelReflectance) const;

    // Evaluate BTDF (transmission BSDF) for a given outgoing direction
    float3 evalTransmission(const float3& outDir) const;
    float  pdfTransmission(const float3& outDir) const;

private:
    float GTR1(float NdotH, float a) const;
    float GTR2(float NdotH, float a) const;
    float smithG(float NdotV, float alpha) const;
    float schlickFresnel(float u) const;
};
#endif
