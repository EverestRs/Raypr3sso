#!/bin/bash
# Bistro 外景白天 —— 与官方参考图对齐的光照配置（README 13.6 标定值）
#
# 组成:
#   * 天空环境光: san_giuseppe_bridge_4k.hdr (黄昏 HDRI, 染蓝+压暗当白天天光)
#     RENDER_SKY_SCALE=0.05 RENDER_SKY_TINT="0.75 0.9 1.35"
#     RENDER_SKY_FOCUS_DIM=0.05 RENDER_SKY_FOCUS_CONE=15 (暗环境+太阳锥去重)
#   * 直射阳光: delta 太阳 az=-90 el=55 int=12, 暖白 "1 0.95 0.88"
#   * 灯具白天近全灭: RENDER_EMISSION_SCALE=0.008
#   * 相机: pos(-17 3.2 0.7) dir(1 -0.11 -0.09) fov 50 exposure 0.85
#     (官方参考图 1920x1061 同构图: Vespa 居中, 路缘弧线占下 1/4)
#
# 2026-09-23: 接入自适应逐像素采样 (RENDER_ADAPTIVE_*) —— spp 变为预算上限,
# 像素按 AOV 相对标准误差提前收敛, 高预算下噪点收敛至不可见。
# 旧配置 (az=30/el=48/int=8/sky=3, 相机 -15.594) 已被 13.6 标定取代。
#
# 用法: ./scripts/render_bistro_exterior_daylight.sh [width height budget_spp]
cd "$(dirname "$0")/.."
W=${1:-1920}; H=${2:-1061}; SPP=${3:-2048}

export RENDER_DENOISE=1
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
# 自适应逐像素: 预算 SPP, RSE<2% 即停 (降噪器收尾; 要更干净改 0.01)
export RENDER_ADAPTIVE_SPP=${RENDER_ADAPTIVE_SPP:-1}
export RENDER_ADAPTIVE_MIN=${RENDER_ADAPTIVE_MIN:-48}
export RENDER_ADAPTIVE_THRESH=${RENDER_ADAPTIVE_THRESH:-0.02}
export RENDER_ADAPTIVE_CHECK=${RENDER_ADAPTIVE_CHECK:-8}

{
  echo "create model bext models/Bistro_v5_2 BistroExterior.fbx models/Bistro_v5_2/san_giuseppe_bridge_4k.hdr"
  echo "create args daylight"
  echo "$W"
  echo "$H"
  echo "$SPP"
  echo "12"
  echo "0.85"
  echo "-17 3.2 0.7"
  echo "1 -0.11 -0.09"
  echo "50"
  echo "output_gpu/render_bistro_exterior_daylight"
  echo "render bext daylight gpu"
  echo "exit"
} | ./build/RenderEngine
