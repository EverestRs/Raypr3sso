#!/bin/bash
# Daylight HDRI calibration round 1: real noon sky (qwantani, sun rotated to
# the delta-sun direction) at 480x270@64. One model load, 4 renders varying
# sun intensity / WB / exposure via setenv.
cd "$(dirname "$0")/.."
OUT=output_gpu/daysky_cal
mkdir -p $OUT

# load-time env
export RENDER_SKY_SCALE=1
export RENDER_SKY_FOCUS_DIM=0.999   # enables sunConeKill without dimming the dome
export RENDER_SKY_FOCUS_CONE=12
export RENDER_EMISSION_SCALE=0.008
# shared render-time env
export RENDER_SUN_AZ=-90
export RENDER_SUN_EL=55
export RENDER_SUN_TINT="1 0.95 0.88"
export RENDER_TONEMAP=aces
export RENDER_DENOISE=1

mkargs() { # $1=name $2=exp $3=out
  echo "create args $1"
  echo "480"; echo "270"; echo "64"; echo "12"; echo "$2"
  echo "-17 3.2 0.7"; echo "1 -0.11 -0.09"; echo "50"
  echo "$OUT/$3"
}

{
  echo "create model bext models/Bistro_v5_2 BistroExterior.fbx models/Bistro_v5_2/qwantani_sun55_4k.hdr"
  echo "setenv RENDER_SUN_INT 12"
  echo "setenv RENDER_WB_STRENGTH 0.2"
  mkargs a1 0.85 v1_int12_wb02
  echo "render bext a1 gpu"
  echo "setenv RENDER_SUN_INT 8"
  mkargs a2 0.85 v2_int8_wb02
  echo "render bext a2 gpu"
  echo "setenv RENDER_SUN_INT 12"
  echo "setenv RENDER_WB_STRENGTH 0.4"
  mkargs a3 0.7 v3_int12_wb04
  echo "render bext a3 gpu"
  echo "setenv RENDER_SUN_INT 18"
  echo "setenv RENDER_WB_STRENGTH 0.15"
  mkargs a4 0.7 v4_int18_wb015
  echo "render bext a4 gpu"
  echo "exit"
} | ./build/RenderEngine 2>&1 | grep -E "AOVSTAT|Done in|Output file|Skybox|CUDA ERROR"

echo "=== compare ==="
for d in $OUT/v*/; do
  echo "--- $d"
  python3 scripts/compare_official.py reference/official_bistro_daylight.png "$d"/*.png 2>/dev/null | head -8
done
