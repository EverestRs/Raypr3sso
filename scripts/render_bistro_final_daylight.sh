#!/bin/bash
# FINAL daylight exterior render — v3 lighting rig + grade (2026-09-23).
#
# Sun rig reverse-engineered from the official's shadow geometry: the sun
# sits BEHIND-RIGHT of the bistro (az=-50, not 13.6's az=-90), so the
# building shades the terrace/bushes while the foreground street stays
# sunlit and shadows fall right-down toward the camera, like the official.
# Sun tint kept near-neutral (gold washes the Vespa's blue paint to gray);
# the sky tint is pushed blue (official's blue-lit shade panels).
#
# Sky:  qwantani_noon_puresky_4k (CC0), sun yaw-rotated to az=-55
#       (scripts/rotate_sky_hdr.py); SKY_SCALE=0.6 + blue tint + falloff.
# Sun:  az=-50 el=55 int=14, near-neutral warm.
# Post: ACES, WB 0.15, sat 1.5, exp 0.78, luma contrast curve (moderate
#       shadow crush + highlight shoulder).
# Sampling: adaptive per-pixel, budget 2048, RSE<2% + denoiser.
#
# Usage: ./scripts/render_bistro_final_daylight.sh [budget_spp]
cd "$(dirname "$0")/.."
SPP=${1:-2048}
OUT=output_gpu/final_daylight_v9
mkdir -p $OUT

# load-time env (sky processing happens at model load)
export RENDER_SKY_SCALE=2.3
export RENDER_SKY_HORIZON=0.5
export RENDER_SKY_HTRANS=0.6
export RENDER_SKY_TINT="0.80 0.92 1.22"
export RENDER_SKY_FOCUS_DIM=0.999   # sun-cone lighting kill, dome untouched
export RENDER_SKY_FOCUS_CONE=12
export RENDER_EMISSION_SCALE=0.008
# render-time env
export RENDER_SUN_AZ=-50
export RENDER_SUN_EL=55
export RENDER_SUN_INT=14
export RENDER_SUN_TINT="1 0.85 0.70"
export RENDER_TONEMAP=aces
export RENDER_WB_STRENGTH=0.3
export RENDER_SATURATION=1.8
export RENDER_INDIRECT_SCALE=0.6
export RENDER_DENOISE_DETAIL=1.2
export RENDER_CURVE="0,0 0.1,0.06 0.2,0.13 0.3,0.20 0.4,0.27 0.55,0.50 0.66,0.74 0.85,0.88 1,0.96"
export RENDER_DENOISE=1
export RENDER_ADAPTIVE_SPP=1
export RENDER_ADAPTIVE_MIN=48
export RENDER_ADAPTIVE_THRESH=0.02
export RENDER_ADAPTIVE_CHECK=8

{
  echo "create model bext models/Bistro_v5_2 BistroExterior.fbx models/Bistro_v5_2/qwantani_azm55_4k.hdr"
  echo "create args final"
  echo "1920"           # width (official 1920x1061)
  echo "1061"           # height
  echo "$SPP"           # adaptive budget
  echo "12"             # max depth
  echo "0.70"           # exposure
  echo "-17 3.2 0.7"    # camera position
  echo "1 -0.11 -0.09"  # camera direction
  echo "50"             # fov
  echo "$OUT"
  echo "render bext final gpu"
  echo "exit"
} | ./build/RenderEngine 2>&1 | tee $OUT/render_log.txt | grep -E "ADAPTSTAT|AOVSTAT|Adaptive|DENOISE|Contrast|Done in|Output file|CUDA ERROR"

echo "=== compare with official ==="
FINAL_PNG=$(ls -t $OUT/render_*.png | head -1)
echo "final: $FINAL_PNG"
python3 scripts/compare_official.py reference/official_bistro_daylight.png \
  "$FINAL_PNG" $OUT/side_by_side.png
