#!/bin/bash
# Final delivery render: Bistro exterior daylight matched to the official
# reference (README 13.6/13.13; delivery output_gpu/final_daylight_v11).
#
# The full calibrated rig (sun, sky scale/tint/focus, ACES, saturation, WB,
# split-tone, indirect scale, AO, OIDN, denoiser detail, LUT curve, LUT band
# grade, adaptive sampling) ships IN the `daylight` preset (scene_presets.h)
# — no env setup needed. `preset daylight rig` installs it as process
# defaults; any RENDER_* env exported before launch still overrides per knob.
#
# Usage: ./scripts/render_bistro_official_match.sh [budget_spp] [wavefront]
cd "$(dirname "$0")/.."
SPP=${1:-2048}
WF=${2:-0}
OUT=output_gpu/official_match
mkdir -p $OUT

if [ "$WF" = "1" ]; then
  export RENDER_WAVEFRONT=1
  export RENDER_WF_SORT=1
  export RENDER_WF_MATBATCH=1
fi

{
  echo "preset daylight rig"     # installs rig env (args 'rig' itself unused)
  echo "create model bext models/Bistro_v5_2 BistroExterior.fbx models/Bistro_v5_2/qwantani_azm55_4k.hdr"
  echo "create args final"
  echo "1920"           # width  (official image is 1920x1061)
  echo "1061"           # height
  echo "$SPP"           # adaptive budget (preset default 2048)
  echo "12"             # max depth
  echo "0.70"           # exposure
  echo "-17 3.2 0.7"    # camera position
  echo "1 -0.11 -0.09"  # camera direction
  echo "50"             # fov
  echo "$OUT"
  echo "render bext final gpu"
  echo "exit"
} | ./build/RenderEngine 2>&1 | tee $OUT/render_log.txt | grep -E "Rig installed|ADAPTSTAT|AOVSTAT|SUNMASK|OIDN|AO:|DENOISE done|Done in|Output file|CUDA ERROR|NOTE:"

echo "=== done ==="
ls -la $OUT/*.png
