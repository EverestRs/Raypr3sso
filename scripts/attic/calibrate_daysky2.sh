#!/bin/bash
# Daylight calibration round 2: yaw-rotated noon sky + horizon-falloff and
# sky-scale matrix. 4 model loads, 7 renders, 480x270@64.
cd "$(dirname "$0")/.."
OUT=output_gpu/daysky_cal2
mkdir -p $OUT

export RENDER_SUN_AZ=-90
export RENDER_SUN_EL=55
export RENDER_SUN_TINT="1 0.95 0.88"
export RENDER_TONEMAP=aces
export RENDER_DENOISE=1
export RENDER_EMISSION_SCALE=0.008
export RENDER_SKY_FOCUS_DIM=0.999
export RENDER_SKY_FOCUS_CONE=12
export RENDER_WB_STRENGTH=0.2
SKY=models/Bistro_v5_2/qwantani_yaw55_4k.hdr

mkargs() { # $1=name $2=exp $3=out
  echo "create args $1"
  echo "480"; echo "270"; echo "64"; echo "12"; echo "$2"
  echo "-17 3.2 0.7"; echo "1 -0.11 -0.09"; echo "50"
  echo "$OUT/$3"
}

{
  echo "setenv RENDER_SKY_SCALE 1"
  echo "setenv RENDER_SKY_HORIZON 0.5"
  echo "setenv RENDER_SKY_HTRANS 0.6"
  echo "create model m1 models/Bistro_v5_2 BistroExterior.fbx $SKY"
  echo "setenv RENDER_SUN_INT 8"
  mkargs a1 0.85 m1_int8
  echo "render m1 a1 gpu"
  echo "setenv RENDER_SUN_INT 12"
  mkargs a2 0.85 m1_int12
  echo "render m1 a2 gpu"
  echo "setenv RENDER_SKY_SCALE 0.7"
  echo "setenv RENDER_SKY_HORIZON 0.35"
  echo "create model m2 models/Bistro_v5_2 BistroExterior.fbx $SKY"
  echo "setenv RENDER_SUN_INT 10"
  mkargs a3 0.85 m2_int10
  echo "render m2 a3 gpu"
  echo "setenv RENDER_SUN_INT 14"
  mkargs a4 0.85 m2_int14
  echo "render m2 a4 gpu"
  echo "setenv RENDER_SKY_HORIZON 0.2"
  echo "setenv RENDER_SKY_HTRANS 0.5"
  echo "create model m3 models/Bistro_v5_2 BistroExterior.fbx $SKY"
  echo "setenv RENDER_SUN_INT 12"
  mkargs a5 0.85 m3_int12
  echo "render m3 a5 gpu"
  echo "setenv RENDER_SKY_SCALE 0.5"
  echo "setenv RENDER_SKY_HORIZON 0.25"
  echo "setenv RENDER_SKY_HTRANS 0.6"
  echo "create model m4 models/Bistro_v5_2 BistroExterior.fbx $SKY"
  mkargs a6 0.85 m4_int12
  echo "render m4 a6 gpu"
  echo "exit"
} | ./build/RenderEngine 2>&1 | grep -E "AOVSTAT|Done in|Output file|CUDA ERROR"

echo "=== compare ==="
for d in $OUT/m*/; do
  echo "--- $d"
  python3 scripts/compare_official.py reference/official_bistro_daylight.png "$d"/*.png 2>/dev/null | tail -6
done
