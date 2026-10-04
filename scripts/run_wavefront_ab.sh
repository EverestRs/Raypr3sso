#!/bin/bash
# Wavefront + adaptive-sampling A/B validation, ONE model load, 5 renders.
# 480x270@64 Bistro exterior daylight config (README 13.6).
# Checks: mega==wf_plain==wf_sort (byte-identical), mega_ad==wf_ad (byte-identical).
cd "$(dirname "$0")/.."
OUT=output_gpu/wfab
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

mkargs() { # $1=name $2=spp $3=outdir
  echo "create args $1"
  echo "480"; echo "270"; echo "$2"; echo "12"; echo "0.85"
  echo "-17 3.2 0.7"; echo "1 -0.11 -0.09"; echo "50"
  echo "$OUT/$3"
}

{
  echo "create model bext models/Bistro_v5_2 BistroExterior.fbx models/Bistro_v5_2/san_giuseppe_bridge_4k.hdr"
  # 1) megakernel fixed 64spp
  mkargs p1 64 mega_fixed
  echo "render bext p1 gpu"
  # 2) wavefront, sorts OFF
  echo "setenv RENDER_WAVEFRONT 1"
  echo "setenv RENDER_WF_SORT 0"
  echo "setenv RENDER_WF_MATBATCH 0"
  mkargs p2 64 wf_plain
  echo "render bext p2 gpu"
  # 3) wavefront, sorts ON
  echo "setenv RENDER_WF_SORT 1"
  echo "setenv RENDER_WF_MATBATCH 1"
  mkargs p3 64 wf_sort
  echo "render bext p3 gpu"
  # 4) wavefront + adaptive (min16 thresh0.05 budget64)
  echo "setenv RENDER_ADAPTIVE_SPP 1"
  echo "setenv RENDER_ADAPTIVE_MIN 16"
  echo "setenv RENDER_ADAPTIVE_THRESH 0.05"
  mkargs p4 64 wf_adapt
  echo "render bext p4 gpu"
  # 5) megakernel + adaptive, same params
  echo "setenv RENDER_WAVEFRONT 0"
  mkargs p5 64 mega_adapt
  echo "render bext p5 gpu"
  echo "exit"
} | ./build/RenderEngine 2>&1 | tee $OUT/ab_log.txt | grep -E "ADAPTSTAT|AOVSTAT|Wavefront|Adaptive|Done in|Output file|renderGPU|CUDA ERROR|error"

echo "=== md5 ==="
md5sum $OUT/*/*.png
