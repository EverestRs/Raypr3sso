#!/bin/bash
# 一键演示：预检 → （按需）构建 → 起渲染服务端 + 静态页 → 打开浏览器。
# Ctrl-C 退出并清理本脚本启动的进程（已运行的实例不受影响）。
set -euo pipefail
cd "$(dirname "$0")"

PORT=${PORT:-9151}
WEB=${WEB:-8080}
TOKEN=${TOKEN:-devtoken}
MODEL=${MODEL:-bistro}          # bistro = 内外景合一 | bistro-interior = 纯内景
LOG=/tmp/raypr3sso_demo_server.log

echo "== Raypr3sso demo =="
echo "服务端端口 $PORT · 静态页端口 $WEB · 模型 $MODEL"

# ---- 1) GPU 预检（演示事故预防：显存被其他进程占用会白屏/失败）----
if ! command -v nvidia-smi >/dev/null 2>&1; then
  echo "错误: 未找到 nvidia-smi —— 本演示需要 NVIDIA GPU（CUDA 12.x）"; exit 1
fi
FREE=$(nvidia-smi --query-gpu=memory.free --format=csv,noheader,nounits | head -1)
if [ "$FREE" -lt 6000 ]; then
  echo "错误: 空闲显存 ${FREE}MiB < 6000MiB —— 请关闭占用显存的进程后重试"; exit 1
fi
echo "空闲显存 ${FREE}MiB ✓"

# ---- 2) 二进制（缺失则现场构建）----
if [ ! -x build/render_server ]; then
  echo "首次运行：构建中（约几分钟）..."
  cmake -S . -B build -DCMAKE_BUILD_TYPE=Release >/dev/null
  cmake --build build -j"$(nproc)" --target render_server | tail -1
fi

cleanup() { [ -n "${SRV:-}" ] && kill "$SRV" 2>/dev/null || true
            [ -n "${WEBPID:-}" ] && kill "$WEBPID" 2>/dev/null || true; }
trap cleanup EXIT

# ---- 3) 渲染服务端（端口已占用则视为已在运行，直接复用）----
SRV=""
if ss -tln 2>/dev/null | grep -q ":$PORT "; then
  echo "端口 $PORT 已有服务在跑，直接复用"
else
  # RENDER_OIDN=1：精修终帧 OIDN 降噪 + 启用自适应采样（见 README）
  RENDER_TOKEN="$TOKEN" RENDER_OIDN=1 ./build/render_server 127.0.0.1 "$PORT" > "$LOG" 2>&1 &
  SRV=$!
fi

# ---- 4) 静态页 ----
WEBPID=""
if ss -tln 2>/dev/null | grep -q ":$WEB "; then
  echo "端口 $WEB 已有静态页在跑，直接复用"
else
  python3 -m http.server "$WEB" >/dev/null 2>&1 &
  WEBPID=$!
fi

# ---- 5) 等健康检查 → 打开浏览器 ----
for _ in $(seq 50); do
  curl -s -H "Authorization: Bearer $TOKEN" "localhost:$PORT/api/health" >/dev/null 2>&1 && break
  sleep 0.2
done
URL="http://127.0.0.1:$WEB/threejs/index.html?remote=http://127.0.0.1:$PORT&model=$MODEL&token=$TOKEN"
echo ""
echo "  演示地址: $URL"
echo "  提示: 场景首次加载约 1 分钟（3.9M 面），页面会显示加载状态；"
echo "        拖动环视 · 滚轮进出建筑 · 停手后自动渐进精修。"
echo "  服务端日志: $LOG"
echo ""
command -v xdg-open >/dev/null 2>&1 && xdg-open "$URL" >/dev/null 2>&1 || true

# ---- 6) 前台保活（Ctrl-C 退出）----
if [ -n "$SRV" ]; then
  wait "$SRV"
else
  echo "（服务端为外部实例——按 Ctrl-C 退出本脚本）"
  sleep infinity
fi
