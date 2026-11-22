#pragma once

// Shared scene presets (design 7.1 "preset"): the single source for the
// camera and render quality defaults of the deployed models. Console's
// `preset` command and the render server's POST /api/sessions both read
// this table, so a calibrated view cannot drift between CLI and server.
//
// Each preset may also carry a calibrated lighting/grade "rig" (the
// RENDER_* values that a delivery render was tuned with). applyPresetRig()
// installs those values as process defaults when the preset is selected:
//   priority: explicit env var  >  preset rig  >  built-in default
// so ad-hoc experiments via the environment still win, and a preset alone
// reproduces the calibrated look (previously those values lived only in
// scripts/ and README prose — running the engine without them silently
// produced an uncalibrated image).
#include "camera.h"

#include <cstdlib>
#include <cstring>
#include <set>
#include <string>
#include <vector>

struct ScenePreset {
    Camera camera;
    int spp = 0, width = 0, height = 0;
    float exposure = 1.0f, warmTint = 1.0f, saturationBoost = 1.0f;
    int maxDepth = 0;                // 0 = engine default (16)
    const char* modelFbx = nullptr;  // recommended FBX (create-model hint)
    const char* skyFile = nullptr;   // recommended HDRI (create-model hint)
    const char* note = nullptr;      // one-line description of the look
    const char* const* rigEnv = nullptr;  // "NAME=VALUE" list, nullptr-terminated
    // Position-aware rig profile (bistro combined scene): when set and the
    // active camera is OUTSIDE the indoor box, the server renders with this
    // profile instead of rigEnv. The profiles ride the task data channel
    // (parsed here, never via setenv — HTTP threads must not mutate the
    // process env, see render_server::run's startup-rig constraint).
    // Load-time knobs (sky tint/scale, FIXTURE_GAIN) stay in rigEnv only:
    // the scene is resident and cannot re-bake per view.
    const char* const* rigEnvOutdoor = nullptr;
    // Indoor auto-exposure tier (optional, browser remote viewing): when the
    // active camera is inside [indoorMin, indoorMax] the display exposure
    // blends up to indoorExposure (smoothstep over indoorMargin metres around
    // the box), so a dim interior and a bright exterior can share one orbit.
    // indoorExposure 0 = tier disabled. The CLIENT computes the blend per
    // render submission and sends `exposure` (display-only, never resets the
    // accumulation); the server only publishes the parameters.
    float indoorExposure = 0.0f;
    float indoorMin[3] = {0.0f, 0.0f, 0.0f};
    float indoorMax[3] = {0.0f, 0.0f, 0.0f};
    float indoorMargin = 1.5f;
};

// Parsed rig profile: the subset of RENDER_* keys that must switch per
// camera position (physical + display-grade knobs that are NOT load-time).
// Keys absent from the list keep their neutral defaults.
struct RigProfile {
    bool valid = false;
    bool hasSun = false;
    float sunAz = 0.0f, sunEl = 45.0f, sunInt = 0.0f;
    float emissionScale = 1.0f, indirectScale = 1.0f;
    float wbStrength = 0.0f, saturationBoost = 1.0f;
    bool splitKeySun = false;
    float splitWarm = 0.0f, splitCool = 0.0f;
    std::string gradeBands;          // raw "l,r,g,b ..." string (parsed at encode)
};

inline RigProfile parseRigProfile(const char* const* rigEnv) {
    RigProfile p;
    if (!rigEnv) return p;
    p.valid = true;
    for (const char* const* kv = rigEnv; *kv; ++kv) {
        const std::string s(*kv);
        const size_t eq = s.find('=');
        if (eq == std::string::npos || eq == 0) continue;
        const std::string key = s.substr(0, eq);
        const std::string val = s.substr(eq + 1);
        const char* v = val.c_str();
        if      (key == "RENDER_SUN_INT")        { p.sunInt = std::strtof(v, nullptr); p.hasSun = p.sunInt > 0.0f; }
        else if (key == "RENDER_SUN_AZ")         p.sunAz = std::strtof(v, nullptr);
        else if (key == "RENDER_SUN_EL")         p.sunEl = std::strtof(v, nullptr);
        else if (key == "RENDER_EMISSION_SCALE") p.emissionScale = std::strtof(v, nullptr);
        else if (key == "RENDER_INDIRECT_SCALE") p.indirectScale = std::strtof(v, nullptr);
        else if (key == "RENDER_WB_STRENGTH")    p.wbStrength = std::strtof(v, nullptr);
        else if (key == "RENDER_SATURATION")     p.saturationBoost = std::strtof(v, nullptr);
        else if (key == "RENDER_SPLIT_KEY")      p.splitKeySun = (val == "sun");
        else if (key == "RENDER_SPLIT_WARM")     p.splitWarm = std::strtof(v, nullptr);
        else if (key == "RENDER_SPLIT_COOL")     p.splitCool = std::strtof(v, nullptr);
        else if (key == "RENDER_GRADE_BANDS")    p.gradeBands = val;
    }
    return p;
}

// ---- Calibrated rigs (values match the recorded delivery render logs) ----

// Interior delivery (2026-09-14 render_20260914_100729.png): the delta sun
// through the windows is the room's main direct light (without it the
// direct-diffuse AOV drops ~20x) and FIXTURE_GAIN=2.0 rebalances the
// per-triangle textured emission (audit 13.4).
inline const char* const kInteriorRig[] = {
    "RENDER_SUN_AZ=90",
    "RENDER_SUN_EL=12",
    "RENDER_SUN_INT=85",
    "RENDER_FIXTURE_GAIN=2.0",
    nullptr,
};

// Combined interior+exterior bistro rig (web viewer + gallery). The interior
// keeps the calibrated delta sun through the windows + fixture gain; the
// exterior view gets the DAYLIGHT calibration the standalone `daylight`
// preset proved against the official plate (README 13.13): real-daylight
// qwantani sky (cool window counterlight — the physical fix for the warm
// wash), ACES shoulder (the tanh clip deliberately keeps blown highlights
// warm, which turned the whole street frame orange at exposure 2.5), gray-world
// WB, display saturation, sun-keyed split-tone, midtone curve, AO.
// Deliberately NOT inherited from kDaylightRig: RENDER_GRADE_BANDS (a LUT
// fitted to the exterior plate — wrong on interior wood), RENDER_INDIRECT_SCALE
// (0.48 would gut the interior's bounce-dominated lighting),
// RENDER_EMISSION_SCALE=0.008 (would kill the interior fixtures).
inline const char* const kCombinedRig[] = {
    "RENDER_SUN_AZ=90",
    "RENDER_SUN_EL=12",
    "RENDER_SUN_INT=85",
    "RENDER_SUN_TINT=1.03 0.85 0.70",
    "RENDER_FIXTURE_GAIN=2.0",
    "RENDER_SKY_SCALE=2.3",
    // Load-time sky bake is shared by both profiles (resident scene):
    // the street-calibrated tint (grid round 2) — the interior's cool
    // window counterlight barely notices the 0.05/0.10 shift.
    "RENDER_SKY_TINT=0.88 0.95 1.12",
    "RENDER_SKY_HORIZON=0.5",
    "RENDER_SKY_HTRANS=0.6",
    "RENDER_SKY_FOCUS_DIM=0.999",
    "RENDER_SKY_FOCUS_CONE=12",
    "RENDER_TONEMAP=aces",
    "RENDER_WB_STRENGTH=0.5",
    "RENDER_SATURATION=1.8",
    "RENDER_SPLIT_KEY=sun",
    "RENDER_SPLIT_WARM=0.08",
    "RENDER_SPLIT_COOL=0.07",
    "RENDER_CURVE=0,0 0.1,0.06 0.2,0.13 0.3,0.20 0.4,0.27 0.55,0.50 0.66,0.74 0.85,0.88 1,0.96",
    "RENDER_AO=1",
    "RENDER_DENOISE=1",
    "RENDER_OIDN=1",
    "RENDER_DENOISE_DETAIL=1.2",
    "RENDER_DENOISE_NOISE_TARGET=0.12",
    "RENDER_ADAPTIVE_SPP=1",
    "RENDER_ADAPTIVE_MIN=48",
    "RENDER_ADAPTIVE_THRESH=0.02",
    "RENDER_ADAPTIVE_CHECK=8",
    nullptr,
};

// Outdoor (street) profile for the combined bistro preset — parsed into a
// RigProfile and delivered per render via the task data channel (never via
// setenv). Only the keys that DIFFER from the indoor rig or must be
// explicitly present are listed; load-time knobs (SKY_*, FIXTURE_GAIN) and
// the shared process knobs (TONEMAP/CURVE/AO/OIDN/ADAPTIVE) stay in
// kCombinedRig. Calibrated 2026-11-21 (grid rounds 1-3): the high warm sun
// replaces the interior's low window raker, fixtures go nearly off for
// daytime, bounce is suppressed, and the v11-fitted band grade pulls the
// street plate toward the official (medRGB 0.337/0.349/0.361 vs official
// 0.353/0.349/0.361, R:G 0.97 vs 1.01, B:G 1.03 = exact).
inline const char* const kCombinedStreetRig[] = {
    "RENDER_SUN_AZ=-50",
    "RENDER_SUN_EL=55",
    "RENDER_SUN_INT=18",
    "RENDER_EMISSION_SCALE=0.008",
    // NB: no RENDER_INDIRECT_SCALE here. The street calibration (grid
    // rounds 1-3, street_final) ran while the server silently ignored the
    // knob — its EFFECTIVE value was 1.0. Applying the recorded 0.35 for
    // real over-cools the frame (B:G 1.55). Keep the calibrated 1.0.
    "RENDER_WB_STRENGTH=0.55",
    "RENDER_SATURATION=2.0",
    "RENDER_SPLIT_KEY=sun",
    "RENDER_SPLIT_WARM=0.08",
    "RENDER_SPLIT_COOL=0.07",
    "RENDER_GRADE_BANDS=0.1004,1.6538,1.7200,2.0000 0.1611,1.1273,1.1915,1.2694 0.1998,0.9589,1.0041,0.9506 0.2270,1.1610,1.1594,1.0324 0.2675,1.2468,1.2331,1.0830 0.3698,1.0257,1.0243,0.9112 0.6783,0.8466,0.8409,0.7814 0.8386,0.8918,0.8736,0.8105 0.8941,0.9692,0.9430,0.8632",
    nullptr,
};

// Official daylight exterior match (README 13.6/13.13; delivery
// output_gpu/final_daylight_v11, one-click repro scripts/render_final_v10.sh).
// Real-daylight yaw-rotated qwantani HDRI + warm delta sun + ACES +
// sun-keyed split-tone + LUT band grade + adaptive sampling.
inline const char* const kDaylightRig[] = {
    "RENDER_SKY_SCALE=2.3",
    "RENDER_SKY_TINT=0.80 0.92 1.22",
    "RENDER_SKY_HORIZON=0.5",
    "RENDER_SKY_HTRANS=0.6",
    "RENDER_SKY_FOCUS_DIM=0.999",
    "RENDER_SKY_FOCUS_CONE=12",
    "RENDER_EMISSION_SCALE=0.008",
    "RENDER_SUN_AZ=-50",
    "RENDER_SUN_EL=55",
    "RENDER_SUN_INT=14",
    "RENDER_SUN_TINT=1.03 0.85 0.70",
    "RENDER_TONEMAP=aces",
    "RENDER_DENOISE=1",
    "RENDER_SATURATION=1.8",
    "RENDER_WB_STRENGTH=0.5",
    "RENDER_SPLIT_KEY=sun",
    "RENDER_SPLIT_WARM=0.08",
    "RENDER_SPLIT_COOL=0.07",
    "RENDER_INDIRECT_SCALE=0.48",
    "RENDER_AO=1",
    "RENDER_OIDN=1",
    "RENDER_DENOISE_DETAIL=1.2",
    "RENDER_DENOISE_NOISE_TARGET=0.12",
    "RENDER_CURVE=0,0 0.1,0.06 0.2,0.13 0.3,0.20 0.4,0.27 0.55,0.50 0.66,0.74 0.85,0.88 1,0.96",
    "RENDER_GRADE_BANDS=0.1004,1.6538,1.7200,2.0000 0.1611,1.1273,1.1915,1.2694 0.1998,0.9589,1.0041,0.9506 0.2270,1.1610,1.1594,1.0324 0.2675,1.2468,1.2331,1.0830 0.3698,1.0257,1.0243,0.9112 0.6783,0.8466,0.8409,0.7814 0.8386,0.8918,0.8736,0.8105 0.8941,0.9692,0.9430,0.8632",
    "RENDER_ADAPTIVE_SPP=1",
    "RENDER_ADAPTIVE_MIN=48",
    "RENDER_ADAPTIVE_THRESH=0.02",
    "RENDER_ADAPTIVE_CHECK=8",
    nullptr,
};

// Dusk exterior delivery (2026-09-14 render_20260914_100716.png): warm low
// sun + dusk HDRI, string lights on, ACES.
inline const char* const kDuskRig[] = {
    "RENDER_SUN_AZ=-45",
    "RENDER_SUN_EL=45",
    "RENDER_SUN_INT=12",
    "RENDER_SUN_TINT=1 0.85 0.72",
    "RENDER_SKY_SCALE=2",
    "RENDER_WB_STRENGTH=0.6",
    "RENDER_TONEMAP=aces",
    "RENDER_SKY_FOCUS_DIM=0.05",
    "RENDER_SKY_FOCUS_CONE=15",
    nullptr,
};

// Install a preset's rig as process-default environment values. A variable
// the user set explicitly before launch is never overwritten; values
// installed by an earlier preset application are refreshed, so switching
// presets inside one session works. Returns the list installed (optional).
inline void applyPresetRig(const ScenePreset& preset,
                           std::vector<std::string>* installed = nullptr) {
    // Env keys previously installed by a preset rig (owned = refreshable).
    static std::set<std::string> rigOwned;
    if (!preset.rigEnv) return;
    for (const char* const* kv = preset.rigEnv; *kv; ++kv) {
        const char* eq = std::strchr(*kv, '=');
        if (!eq || eq == *kv) continue;
        const std::string key(*kv, size_t(eq - *kv));
        if (!rigOwned.count(key) && std::getenv(key.c_str()) != nullptr)
            continue;  // explicit user env wins over the preset
        setenv(key.c_str(), eq + 1, 1);
        rigOwned.insert(key);
        if (installed) installed->push_back(*kv);
    }
}

// Returns false for unknown names (caller decides how to report it).
inline bool findScenePreset(const std::string& name, ScenePreset& out) {
    out = ScenePreset{};
    if (name == "interior") {
        // Bistro interior — camera derived from reference project's
        // renderArguments.txt; exposure/rig from the 2026-09-14 interior
        // delivery (README 13.4/13.5). NOTE: this engine's emission scaling
        // (x1000 emissive factor) is not the reference engine's, so the
        // reference file's exposure (0.42) is wrong here — 1.75 is the
        // calibrated value that hits the official wall/median targets.
        out.camera.position  = float3(-0.833f, 3.099f, -1.471f);
        out.camera.direction = float3(0.993846f, -0.104828f, -0.0358017f);
        out.camera.up        = float3(0.104761f, 0.99449f, -0.00377383f);
        out.camera.fov       = 40.5f;
        out.spp = 4096; out.width = 1920; out.height = 1080;
        out.exposure = 1.75f;
        out.modelFbx = "BistroInterior.fbx";
        out.skyFile  = "null";
        out.note     = "interior delivery rig (sun through windows + fixture gain)";
        out.rigEnv   = kInteriorRig;
        return true;
    }
    if (name == "interior_quick") {
        // Verification preset: 1080p @ 320 spp, same delivery look as
        // `interior` (exposure 1.75, sun + fixture rig).
        out.camera.position  = float3(-0.833f, 3.099f, -1.471f);
        out.camera.direction = float3(0.993846f, -0.104828f, -0.0358017f);
        out.camera.up        = float3(0.104761f, 0.99449f, -0.00377383f);
        out.camera.fov       = 40.5f;
        out.spp = 320; out.width = 1920; out.height = 1080;
        out.exposure = 1.75f;
        out.modelFbx = "BistroInterior.fbx";
        out.skyFile  = "null";
        out.note     = "interior delivery look @ 320 spp";
        out.rigEnv   = kInteriorRig;
        return true;
    }
    if (name == "daylight") {
        // Official Bistro daylight exterior match
        // (reference/official_bistro_daylight.png). Camera from the v11
        // delivery (street-corner eye height); qwantani_azm55 is the
        // yaw-rotated real-daylight HDRI. The rig below reproduces
        // output_gpu/final_daylight_v11 / scripts/render_final_v10.sh
        // (README 13.13) with no external env setup.
        out.camera.position  = float3(-17.0f, 3.2f, 0.7f);
        out.camera.direction = float3(1.0f, -0.11f, -0.09f);
        out.camera.up        = float3(0.0f, 1.0f, 0.0f);
        out.camera.fov       = 50.0f;
        out.spp = 2048; out.width = 1920; out.height = 1061;  // official canvas
        out.exposure = 0.70f;
        out.maxDepth = 12;
        out.modelFbx = "BistroExterior.fbx";
        out.skyFile  = "models/Bistro_v5_2/qwantani_azm55_4k.hdr";
        out.note     = "official daylight exterior match (v11 delivery rig)";
        out.rigEnv   = kDaylightRig;
        return true;
    }
    if (name == "exterior") {
        // Dusk exterior delivery (2026-09-14 render_20260914_100716.png):
        // bevy-port camera framing the bistro corner, warm low sun, string
        // lights on, dusk HDRI (san_giuseppe).
        out.camera.position  = float3(-10.5f, 1.7f, -1.0f);
        out.camera.direction = glm::normalize(float3(10.5f, 1.8f, 1.0f));  // look_at(0,3.5,0)
        out.camera.up        = float3(0.0f, 1.0f, 0.0f);
        out.camera.fov       = 45.0f;
        out.spp = 320; out.width = 1920; out.height = 1080;
        out.exposure = 0.6f;
        out.modelFbx = "BistroExterior.fbx";
        out.skyFile  = "models/Bistro_v5_2/san_giuseppe_bridge_4k.hdr";
        out.note     = "dusk exterior delivery (warm sun, lights on)";
        out.rigEnv   = kDuskRig;
        return true;
    }
    if (name == "bistro") {
        // Combined interior+exterior scene (Model::createCombined — both FBX
        // in one BVH, windows see-through both ways). Colour-calibrated to
        // the official daylight plate via kCombinedRig (qwantani real-daylight
        // HDRI + ACES + WB + grade): the old kloppenheim sunset sky + tanh
        // shoulder at exposure 2.5/6.0 pushed the street frame into a full
        // orange cast (median R:G 1.53 vs official 1.01). Outdoor exposure
        // matches the standalone `daylight` preset (0.70); the indoor tier
        // is calibrated against the interior delivery median.
        out.camera.position  = float3(-0.833f, 3.099f, -1.471f);
        out.camera.direction = float3(0.993846f, -0.104828f, -0.0358017f);
        out.camera.up        = float3(0.104761f, 0.99449f, -0.00377383f);
        out.camera.fov       = 40.5f;
        out.spp = 320; out.width = 1920; out.height = 1080;
        // exposure = the OUTDOOR (street) delivery value from the 2026-11-21
        // grid calibration; the indoor view uses the indoorExposure tier.
        // The server picks per camera position when no explicit exposure is
        // sent (the browser client always sends its blended tier value).
        out.exposure = 0.65f;
        out.modelFbx = "BistroInterior.fbx + BistroExterior.fbx (combined)";
        out.skyFile  = "models/Bistro_v5_2/qwantani_azm55_4k.hdr";
        out.note     = "combined in+out bistro (position-aware daylight rigs)";
        out.rigEnv   = kCombinedRig;
        out.rigEnvOutdoor = kCombinedStreetRig;
        // Indoor auto-exposure tier: the exterior shell blocks the calibrated
        // low sun, so the room reads darker than the pure interior. When the
        // camera is inside the interior FBX's bounds (measured at load:
        // min=(-2.06,0.23,-13.94) max=(13.56,7.51,5.47)) the client blends
        // the display exposure up to the tier value with a 1.5m smoothstep
        // through the windows; outside stays at the outdoor exposure.
        // Calibrated 2026-11-21 with the daylight rig: indoor tier 2.4 hits
        // median luma 0.433 (official interior ~0.45); outdoor 0.30 keeps the
        // street at median luma ~0.30 without clipping (the rig's low interior
        // sun rakes the street — street DELIVERY renders should use the
        // daylight sun/emission/indirect treatment instead, see README).
        out.indoorExposure = 2.4f;
        out.indoorMin[0] = -2.06f; out.indoorMin[1] = 0.23f; out.indoorMin[2] = -13.94f;
        out.indoorMax[0] = 13.56f; out.indoorMax[1] = 7.51f; out.indoorMax[2] = 5.47f;
        out.indoorMargin = 1.5f;
        return true;
    }
    if (name == "counter") {
        // Counter close-up
        out.camera.position  = float3(0.0f, 1.0f, -2.0f);
        out.camera.direction = float3(0.5f, 0.0f, 0.9f);
        out.camera.up        = float3(0.0f, 1.0f, 0.0f);
        out.camera.fov       = 50.0f;
        out.spp = 256; out.width = 640; out.height = 480;
        return true;
    }
    if (name == "wide") {
        // Wide overview from above
        out.camera.position  = float3(0.0f, 30.0f, -30.0f);
        out.camera.direction = float3(0.5f, -0.3f, 0.8f);
        out.camera.up        = float3(0.0f, 1.0f, 0.0f);
        out.camera.fov       = 90.0f;
        out.spp = 128; out.width = 640; out.height = 480;
        return true;
    }
    if (name == "front") {
        // Front view from outside
        out.camera.position  = float3(-30.0f, 1.5f, -20.0f);
        out.camera.direction = float3(0.6f, 0.0f, 0.8f);
        out.camera.up        = float3(0.0f, 1.0f, 0.0f);
        out.camera.fov       = 60.0f;
        out.spp = 128; out.width = 640; out.height = 480;
        return true;
    }
    if (name == "ball") {
        // Single sphere test camera
        out.camera.position  = float3(2.5f, 1.2f, 2.0f);
        out.camera.direction = glm::normalize(float3(-2.5f, -1.4f, -2.0f));
        out.camera.up        = float3(0.0f, 1.0f, 0.0f);
        out.camera.fov       = 35.0f;
        out.spp = 512; out.width = 512; out.height = 512;
        out.exposure = 1.2f;           // Ball scene has brighter direct lighting
        return true;
    }
    return false;
}
