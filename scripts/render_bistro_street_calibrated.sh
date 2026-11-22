#!/bin/bash
# Bistro 街景交付渲染 —— 白天标定完整处理（2026-11-21 定版）
#
# 场景：独立外景 FBX + qwantani 真白天 HDRI（与 v11 交付同场景同机位，
# 即 daylight 预设）；合并场景（内外合一）的街景变体走 render_server API，
# 用同一组 env（见 REPORT.md 复现节）。
#
# 标定历程（640x360@96 网格，见 output_review/bistro-color-fix/REPORT.md）：
#   SKY_TINT 回调 1.22→1.12   修复调色生效后 B:G 过冷（bands 阴影蓝带叠加过冲）
#   SUN_INT 14→18             暖化 R:G（0.92→~0.97）
#   exp 0.65 / SAT 2.0        luma 命中官方 0.40 区间
#   WB 0.55 + v11 GRADE_BANDS 拟合 LUT（街景板专用，勿用于内景）
#   NB: 标定期间 server 静默忽略 RENDER_INDIRECT_SCALE（与调色字段同源的
#   丢弃 bug），street_final 的有效 indirect=1.0 —— 本脚本同样不设该键，
#   以保持与标定交付一致。位置感知 profile（kCombinedStreetRig）同理省略。
#
# 前置：RENDER_RENDER_TIMEOUT 必须调大（默认 600s，渐进链 ~12 分钟）。
cd "$(dirname "$0")/.."
OUT=${1:-output_gpu/bistro_street_calibrated}
mkdir -p "$OUT"

GRADE_BANDS="0.1004,1.6538,1.7200,2.0000 0.1611,1.1273,1.1915,1.2694 0.1998,0.9589,1.0041,0.9506 0.2270,1.1610,1.1594,1.0324 0.2675,1.2468,1.2331,1.0830 0.3698,1.0257,1.0243,0.9112 0.6783,0.8466,0.8409,0.7814 0.8386,0.8918,0.8736,0.8105 0.8941,0.9692,0.9430,0.8632"

export RENDER_RENDER_TIMEOUT=2400
export RENDER_OIDN=1
export RENDER_SUN_AZ=-50
export RENDER_SUN_EL=55
export RENDER_SUN_INT=18
export RENDER_EMISSION_SCALE=0.008
export RENDER_SKY_TINT="0.88 0.95 1.12"
export RENDER_WB_STRENGTH=0.55
export RENDER_SATURATION=2.0
export RENDER_GRADE_BANDS="$GRADE_BANDS"
export RENDER_ADAPTIVE_SPP=1
export RENDER_ADAPTIVE_MIN=48
export RENDER_ADAPTIVE_THRESH=0.02
export RENDER_ADAPTIVE_CHECK=8

{
  echo "create model bext models/Bistro_v5_2 BistroExterior.fbx models/Bistro_v5_2/qwantani_azm55_4k.hdr"
  echo "create args daylight"
  echo "1920"
  echo "1061"
  echo "2048"
  echo "12"
  echo "0.65"
  echo "-17 3.2 0.7"
  echo "1 -0.11 -0.09"
  echo "50"
  echo "$OUT"
  echo "render bext daylight gpu"
  echo "exit"
} | ./build/Raypr3sso 2>&1 | tee "$OUT/render_log.txt" | grep -E "ADAPTSTAT|DENOISE|WB:|Done in|Output file|CUDA ERROR"
