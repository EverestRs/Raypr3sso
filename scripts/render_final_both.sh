#!/bin/bash
# Final delivery: Bistro exterior daylight matched to the official reference,
# adaptive per-pixel sampling (budget 2048, thresh 0.02), rendered TWICE in
# one session — megakernel and wavefront (ray-reorder + material-batch on) —
# demonstrating all three features on the deliverable itself. The two outputs
# must be byte-identical (shared gpu_shadeBounce, __noinline__).
cd "$(dirname "$0")/.."
OUT=output_gpu/official_match
mkdir -p $OUT

export RENDER_EMISSION_SCALE=0.008
export RENDER_SKY_SCALE=0.05
export RENDER_SKY_TINT="0.75 0.9 1.35"
export RENDER_SUN_AZ=-90
export RENDER_SUN_EL=55
export RENDER_SUN_INT=12
export RENDER_SUN_TINT="1 0.95 0.88"
export RENDER_WB_STRENGTH=0.4
export RENDER_TONEMAP=aces
export RENDER_SKY_FOCUS_DIM=0.05
export RENDER_SKY_FOCUS_CONE=15
export RENDER_DENOISE=1
export RENDER_ADAPTIVE_SPP=1
export RENDER_ADAPTIVE_MIN=48
export RENDER_ADAPTIVE_THRESH=0.02
export RENDER_ADAPTIVE_CHECK=8

{
  echo "create model bext models/Bistro_v5_2 BistroExterior.fbx models/Bistro_v5_2/san_giuseppe_bridge_4k.hdr"
  echo "create args mega"
  echo "1920"; echo "1061"; echo "2048"; echo "12"; echo "0.85"
  echo "-17 3.2 0.7"; echo "1 -0.11 -0.09"; echo "50"
  echo "$OUT/final_mega"
  echo "render bext mega gpu"
  echo "setenv RENDER_WAVEFRONT 1"
  echo "setenv RENDER_WF_SORT 1"
  echo "setenv RENDER_WF_MATBATCH 1"
  echo "create args wf"
  echo "1920"; echo "1061"; echo "2048"; echo "12"; echo "0.85"
  echo "-17 3.2 0.7"; echo "1 -0.11 -0.09"; echo "50"
  echo "$OUT/final_wf"
  echo "render bext wf gpu"
  echo "exit"
} | ./build/Raypr3sso 2>&1 | tee $OUT/render_log.txt | grep -E "ADAPTSTAT|AOVSTAT|Adaptive|Wavefront|DENOISE|Done in|Output file|CUDA ERROR"

echo "=== md5 ==="
md5sum $OUT/final_mega/*.png $OUT/final_wf/*.png
