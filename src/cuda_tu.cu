// Single CUDA translation unit for the renderer core.
//
// bvh_gpu.cu and render_wavefront.cu are implementation-included here because
// separable compilation is disabled: the CUDA 13 device linker strips the PTX
// that sm_70 (V100) needs, and cross-file device calls otherwise fail to link.
//
// Keep this the ONLY file that #includes a .cu implementation. Order matters:
//   1. bvh_gpu.cu       — device traversal helpers used below
//   2. render_gpu.cu    — BSDF sampling, traceRay, gpu_shadeBounce, megakernel
//   3. render_wavefront.cu — wavefront pipeline calling those helpers

#include "bvh_gpu.cu"
#include "render_gpu.cu"
#include "render_wavefront.cu"