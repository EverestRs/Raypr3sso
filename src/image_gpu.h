#pragma once

// ============================================================================
// GPU post-processing (RENDER_POST_GPU=1)
// ============================================================================
// CUDA implementation of the post-process pipeline in image.cpp:
// relative clamp → filterVar → firefly clamp → (de)normalization → the
// NL-means multi-scale ladder → AO / detail re-injection → energy gain →
// emission blur → recomposite → tonemap (WB + exposure) → bloom →
// gamma (ACES/tanh + saturation + curve LUT + split-tone + grade bands) →
// despeckle → FXAA.
//
// Semantics: a full mirror of Photo::postProcess. Pixel-level passes are
// kernel-per-pass with the same ping-pong buffer structure as the CPU code,
// and the sampling statistics (1/7, 1/13, 1/4 strided medians) use the SAME
// stride and host-side nth_element, so the statistical behaviour matches.
// Floating-point results differ at ulp level from the CPU build (different
// compilers/FMA), so the acceptance criterion for this path is STATISTICAL
// equivalence, not byte identity — the byte-level A/B regression standard
// still applies to the CPU path, which remains the default.
//
// All env knobs (RENDER_OIDN, RENDER_AO, RENDER_DENOISE_*, RENDER_TONEMAP,
// RENDER_CURVE, RENDER_SPLIT_*, RENDER_GRADE_BANDS, ...) behave identically.
// At the end of the run every mutated host-side buffer (AOVs, emission,
// finalColor, oidnColor, sunliness) is written back, so downstream consumers
// (save/rgbBytes/DUMPAOV/server preview encode) see the same state.

#include "image.h"

// True when RENDER_POST_GPU=1 and a CUDA device is usable. Cheap; call it
// before every postProcess (per-call env read, like the other knobs).
bool gpuPostEnabled();

// Full GPU post-process mirror. Returns false if setup fails mid-way —
// the caller should then fall back to the CPU implementation (the Photo
// state is left untouched unless the run reached the write-back stage).
bool postProcessGPU(Photo& photo, int shadeOptions, int spp, int denoiseQuality);

#ifdef RENDERER_HAS_OIDN
// Runs the OIDN "RT" filter on the given host buffers. Device selection
// honours RENDER_OIDN_DEVICE:
//   cpu (default) — shared-image path, byte-identical to the historical
//                   behaviour;
//   gpu           — a CUDA device with the images staged through OIDN device
//                   buffers (oidnNewBuffer: the CUDA backend cannot
//                   dereference plain malloc'd host memory); ANY failure
//                   (device creation, commit, filter execution) falls back
//                   to the CPU device so the output is never garbage.
// Returns true on success and fills `out` (width*height*3 floats).
bool oidnRunRT(const std::vector<float>& color, const std::vector<float>& albedo,
               const std::vector<float>& normal, std::vector<float>& out,
               int width, int height);
#endif

// Env caches managed by refreshRenderEnv (image.cpp), shared with the GPU
// mirror so both paths see identical tonemap/denoiser knobs.
float getDenoiseNoiseTarget();                     // RENDER_DENOISE_NOISE_TARGET
bool  getUseAcesTonemap();                         // RENDER_TONEMAP=aces
void  getContrastCurveLUT(float* lut, bool& on);   // RENDER_CURVE LUT (1024)
