#pragma once

// Per-render configuration shared by every stage that carries it: a preset
// rig profile (parsed from "NAME=VALUE" lists), a session's client overrides,
// one chunk's task payload, and the encode job. One structural definition so
// the stage copies cannot drift apart.
//
// Defaults are neutral: a field a rig does not mention keeps the engine's
// historical behavior.
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <string>

struct RenderConfig {
    // Directional sun (delta light, NEE-only). sunHas=false = env rig / off.
    bool sunHas = false;
    float sunAz = 0.0f, sunEl = 45.0f, sunInt = 0.0f;
    // Physical knobs applied to GPURenderArgs on the GPU worker.
    float emissionScale = 1.0f, indirectScale = 1.0f;
    int useMipmaps = 0;
    float anisoMax = 2.0f, lodScale = 1.0f;
    // Display / grade knobs applied at encode time only (changing them never
    // resets the accumulation).
    float exposure = 1.0f, warmTint = 1.0f, saturationBoost = 1.0f;
    int bloom = -1;   // -1 = follow the RENDER_BLOOM env default
    float wbStrength = 0.0f;
    bool splitKeySun = false;
    float splitWarm = 0.0f, splitCool = 0.0f;
    std::string gradeBands;   // raw "l,r,g,b ..." (parsed at encode)
};

// Parse one rig "NAME=VALUE" entry into cfg. Returns false for malformed
// entries or keys the per-view profile does not own (load-time/env-only
// knobs) — callers may simply ignore those.
inline bool applyRigEntry(RenderConfig& cfg, const char* entry) {
    if (!entry) return false;
    const char* eq = std::strchr(entry, '=');
    if (!eq || eq == entry) return false;
    const std::string key(entry, size_t(eq - entry));
    const char* v = eq + 1;
    if      (key == "RENDER_SUN_INT")        { cfg.sunInt = std::strtof(v, nullptr); cfg.sunHas = cfg.sunInt > 0.0f; }
    else if (key == "RENDER_SUN_AZ")         cfg.sunAz = std::strtof(v, nullptr);
    else if (key == "RENDER_SUN_EL")         cfg.sunEl = std::strtof(v, nullptr);
    else if (key == "RENDER_EMISSION_SCALE") cfg.emissionScale = std::strtof(v, nullptr);
    else if (key == "RENDER_INDIRECT_SCALE") cfg.indirectScale = std::strtof(v, nullptr);
    else if (key == "RENDER_WB_STRENGTH")    cfg.wbStrength = std::strtof(v, nullptr);
    else if (key == "RENDER_SATURATION")     cfg.saturationBoost = std::strtof(v, nullptr);
    else if (key == "RENDER_SPLIT_KEY")      cfg.splitKeySun = std::string(v) == "sun";
    else if (key == "RENDER_SPLIT_WARM")     cfg.splitWarm = std::strtof(v, nullptr);
    else if (key == "RENDER_SPLIT_COOL")     cfg.splitCool = std::strtof(v, nullptr);
    else if (key == "RENDER_GRADE_BANDS")    cfg.gradeBands = v;
    else if (key == "RENDER_MIPMAP")         cfg.useMipmaps = std::atoi(v) != 0;
    else if (key == "RENDER_ANISO")          cfg.anisoMax = fmaxf(1.0f, std::strtof(v, nullptr));
    else if (key == "RENDER_LOD_SCALE")      cfg.lodScale = fmaxf(0.0f, std::strtof(v, nullptr));
    else return false;
    return true;
}

// Env fallbacks for hosts without a profile (CLI path and server startup rig).
inline bool sunFromEnv(RenderConfig& cfg) {
    const char* se = std::getenv("RENDER_SUN_INT");
    if (!se || std::strtof(se, nullptr) <= 0.0f) return false;
    cfg.sunHas = true;
    cfg.sunAz = 0.0f;
    cfg.sunEl = 45.0f;
    cfg.sunInt = std::strtof(se, nullptr);
    if (const char* a = std::getenv("RENDER_SUN_AZ")) cfg.sunAz = std::strtof(a, nullptr);
    if (const char* e = std::getenv("RENDER_SUN_EL")) cfg.sunEl = std::strtof(e, nullptr);
    return true;
}

inline void physicalFromEnv(RenderConfig& cfg) {
    if (const char* is = std::getenv("RENDER_INDIRECT_SCALE"))
        cfg.indirectScale = fmaxf(0.0f, std::strtof(is, nullptr));
    if (const char* es = std::getenv("RENDER_EMISSION_SCALE"))
        cfg.emissionScale = fmaxf(0.0f, std::strtof(es, nullptr));
    // P0 mipmap chain (default off = byte-identical bilinear path).
    if (const char* mm = std::getenv("RENDER_MIPMAP"))
        cfg.useMipmaps = std::atoi(mm) != 0;
    if (const char* an = std::getenv("RENDER_ANISO"))
        cfg.anisoMax = fmaxf(1.0f, std::strtof(an, nullptr));
    if (const char* ls = std::getenv("RENDER_LOD_SCALE"))
        cfg.lodScale = fmaxf(0.0f, std::strtof(ls, nullptr));
}