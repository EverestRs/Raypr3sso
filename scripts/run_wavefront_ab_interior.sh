#!/bin/bash
# Interior wavefront/adaptive parity check (glass/medium-stack coverage).
# 480x270@16 Bistro interior, official interior calibration env.
cd "$(dirname "$0")/.."
OUT=output_gpu/wfab_interior
mkdir -p $OUT

export RENDER_SUN_AZ=90 RENDER_SUN_EL=12 RENDER_SUN_INT=85
export RENDER_FIXTURE_GAIN=2.0

{
  echo "create model bistro models/Bistro_v5_2 BistroInterior.fbx null"
  echo "create args p1"
  echo "480"; echo "270"; echo "16"; echo "16"; echo "1.75"
  echo "-0.833 3.099 -1.471"; echo "0.993846 -0.104828 -0.0358017"; echo "40.5"
  echo "$OUT/mega"
  echo "render bistro p1 gpu"
  echo "setenv RENDER_WAVEFRONT 1"
  echo "create args p2"
  echo "480"; echo "270"; echo "16"; echo "16"; echo "1.75"
  echo "-0.833 3.099 -1.471"; echo "0.993846 -0.104828 -0.0358017"; echo "40.5"
  echo "$OUT/wf"
  echo "render bistro p2 gpu"
  echo "setenv RENDER_ADAPTIVE_SPP 1"
  echo "setenv RENDER_ADAPTIVE_MIN 8"
  echo "create args p3"
  echo "480"; echo "270"; echo "32"; echo "16"; echo "1.75"
  echo "-0.833 3.099 -1.471"; echo "0.993846 -0.104828 -0.0358017"; echo "40.5"
  echo "$OUT/wf_ad"
  echo "render bistro p3 gpu"
  echo "setenv RENDER_WAVEFRONT 0"
  echo "create args p4"
  echo "480"; echo "270"; echo "32"; echo "16"; echo "1.75"
  echo "-0.833 3.099 -1.471"; echo "0.993846 -0.104828 -0.0358017"; echo "40.5"
  echo "$OUT/mega_ad"
  echo "render bistro p4 gpu"
  echo "exit"
} | ./build/RenderEngine 2>&1 | grep -E "ADAPTSTAT|AOVSTAT|Done in|CUDA ERROR|Output file"

echo "=== md5 ==="
md5sum $OUT/*/*.png
