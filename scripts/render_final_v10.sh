#!/bin/bash
# FINAL v10: P1-P4 全功能交付 —
#   P1 日光分离 matte → P2 sun-keyed split-tone · P3 自动标定参数
#   P4a SSAO · P4c OIDN（单次，albedo+normal 引导）· P4d LUT 亮度分带等级
cd "$(dirname "$0")/.."
OUT=output_gpu/final_daylight_v10
mkdir -p $OUT
export RENDER_SKY_SCALE=2.3 RENDER_SKY_TINT="0.80 0.92 1.22"
export RENDER_SKY_HORIZON=0.5 RENDER_SKY_HTRANS=0.6
export RENDER_SKY_FOCUS_DIM=0.999 RENDER_SKY_FOCUS_CONE=12
export RENDER_EMISSION_SCALE=0.008
export RENDER_SUN_AZ=-50 RENDER_SUN_EL=55 RENDER_SUN_INT=14
export RENDER_SUN_TINT="1.03 0.85 0.70"
export RENDER_TONEMAP=aces RENDER_DENOISE=1 RENDER_SATURATION=1.8
export RENDER_WB_STRENGTH=0.5
export RENDER_SPLIT_KEY=sun RENDER_SPLIT_WARM=0.08 RENDER_SPLIT_COOL=0.07
export RENDER_INDIRECT_SCALE=0.48
export RENDER_AO=1
export RENDER_OIDN=1
export RENDER_DENOISE_DETAIL=1.2 RENDER_DENOISE_NOISE_TARGET=0.12
export RENDER_CURVE="0,0 0.1,0.06 0.2,0.13 0.3,0.20 0.4,0.27 0.55,0.50 0.66,0.74 0.85,0.88 1,0.96"
export RENDER_GRADE_BANDS="0.1004,1.6538,1.7200,2.0000 0.1611,1.1273,1.1915,1.2694 0.1998,0.9589,1.0041,0.9506 0.2270,1.1610,1.1594,1.0324 0.2675,1.2468,1.2331,1.0830 0.3698,1.0257,1.0243,0.9112 0.6783,0.8466,0.8409,0.7814 0.8386,0.8918,0.8736,0.8105 0.8941,0.9692,0.9430,0.8632"
export RENDER_ADAPTIVE_SPP=1 RENDER_ADAPTIVE_MIN=48
export RENDER_ADAPTIVE_THRESH=0.02 RENDER_ADAPTIVE_CHECK=8

{ echo "create model bext models/Bistro_v5_2 BistroExterior.fbx models/Bistro_v5_2/qwantani_azm55_4k.hdr"
  echo "create args final"
  echo "1920"; echo "1061"; echo "2048"; echo "12"; echo "0.70"
  echo "-17 3.2 0.7"; echo "1 -0.11 -0.09"; echo "50"
  echo "$OUT"; echo "render bext final gpu"; echo "exit"
} | ./build/Raypr3sso 2>&1 | tee $OUT/render_log.txt | grep -E "ADAPTSTAT|AOVSTAT|SUNMASK|OIDN|AO:|DENOISE done|Done in|Output file|CUDA ERROR"
FINAL_PNG=$(ls -t $OUT/render_*.png | head -1)
python3 scripts/compare_official.py reference/official_bistro_daylight.png "$FINAL_PNG" $OUT/side_by_side.png
