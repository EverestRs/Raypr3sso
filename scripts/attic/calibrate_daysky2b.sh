#!/bin/bash
# Daylight calibration round 2b: remaining variants, one model load per
# session (round 2's multi-load session died after the 2nd render; root
# cause not found — fall back to the known-good pattern).
cd "$(dirname "$0")/.."
OUT=output_gpu/daysky_cal2
mkdir -p $OUT
SKY=models/Bistro_v5_2/qwantani_yaw55_4k.hdr

run_variant() { # $1=scale $2=horizon $3=htrans $4=name $5..=int list
  local SCALE=$1 HOR=$2 HTR=$3 NAME=$4; shift 4
  {
    echo "create model m models/Bistro_v5_2 BistroExterior.fbx $SKY"
    local i=0
    for INT in "$@"; do
      i=$((i+1))
      echo "setenv RENDER_SUN_INT $INT"
      echo "create args a$i"
      echo "480"; echo "270"; echo "64"; echo "12"; echo "0.85"
      echo "-17 3.2 0.7"; echo "1 -0.11 -0.09"; echo "50"
      echo "$OUT/${NAME}_int$INT"
      echo "render m a$i gpu"
    done
    echo "exit"
  } | env RENDER_SKY_SCALE=$SCALE RENDER_SKY_HORIZON=$HOR RENDER_SKY_HTRANS=$HTR \
        RENDER_SUN_AZ=-90 RENDER_SUN_EL=55 RENDER_SUN_TINT="1 0.95 0.88" \
        RENDER_TONEMAP=aces RENDER_DENOISE=1 RENDER_EMISSION_SCALE=0.008 \
        RENDER_SKY_FOCUS_DIM=0.999 RENDER_SKY_FOCUS_CONE=12 RENDER_WB_STRENGTH=0.2 \
        ./build/Raypr3sso 2>&1 | grep -E "AOVSTAT|Done in|Output file|CUDA ERROR"
}

run_variant 0.7 0.35 0.6 m2 10 14
run_variant 0.7 0.2 0.5 m3 12
run_variant 0.5 0.25 0.6 m4 12

echo "=== compare ==="
for d in $OUT/m2*/ $OUT/m3*/ $OUT/m4*/; do
  echo "--- $d"
  python3 scripts/compare_official.py reference/official_bistro_daylight.png "$d"/*.png 2>/dev/null | tail -6
done
