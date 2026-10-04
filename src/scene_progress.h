#pragma once

// Scene-load progress hook (P2-11): the engine reports coarse load stages
// through a process-global sink. The render server installs a sink that
// fans `scene-progress` events to the sessions waiting on that scene; the
// CLI path leaves it null (one function-pointer check per report — zero
// cost). Stages: parse (assimp) / decode (CPU texture loads) / geometry
// (pack + BVH) / materials / upload (GPU textures + model) / sky.
// total <= 0 means "in-stage count without a known total" (the texture
// decode loop discovers textures lazily).
namespace sceneProgress {

using Sink = void (*)(const char* stage, int done, int total);

inline Sink& sinkRef() {
    static Sink s = nullptr;
    return s;
}
inline void set(Sink s) { sinkRef() = s; }
inline void report(const char* stage, int done, int total) {
    if (Sink s = sinkRef()) s(stage, done, total);
}

}  // namespace sceneProgress
