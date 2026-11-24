#!/usr/bin/env bash
# CI entry point: build, warning summary, deterministic smoke renders, and
# (with a GPU) wavefront/megakernel + chunked/one-shot byte A/B.
#
# Usage:
#   scripts/ci.sh                 # auto-detect GPU; CPU smoke always runs
#   scripts/ci.sh --no-gpu        # CI container: build + CPU smoke only
#   scripts/ci.sh --require-gpu   # fail when no GPU is visible
#   scripts/ci.sh --ab            # also run the server chunked==one-shot A/B
#   scripts/ci.sh --skip-build    # reuse BUILD_DIR (no cmake configure/build)
#   BUILD_DIR=build-ci scripts/ci.sh
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT"

BUILD_DIR="${BUILD_DIR:-build-ci}"
WANT_GPU=auto
RUN_AB=0
SKIP_BUILD=0
for arg in "$@"; do
    case "$arg" in
        --no-gpu) WANT_GPU=no ;;
        --require-gpu) WANT_GPU=yes ;;
        --ab) RUN_AB=1 ;;
        --skip-build) SKIP_BUILD=1 ;;
        -h|--help) sed -n '2,13p' "$0"; exit 0 ;;
        *) echo "unknown argument: $arg" >&2; exit 2 ;;
    esac
done

have_gpu() {
    command -v nvidia-smi >/dev/null 2>&1 && nvidia-smi -L >/dev/null 2>&1
}

GPU=0
case "$WANT_GPU" in
    yes)
        have_gpu || { echo "[ci] ERROR: --require-gpu but no GPU is visible" >&2; exit 1; }
        GPU=1
        ;;
    no) GPU=0 ;;
    auto) have_gpu && GPU=1 || GPU=0 ;;
esac

if [ "$SKIP_BUILD" = 0 ]; then
    cmake -S . -B "$BUILD_DIR" -DCMAKE_BUILD_TYPE=Release
    cmake --build "$BUILD_DIR" -j"$(nproc)" 2>&1 | tee "$BUILD_DIR/build.log"
    WARNINGS=$(grep -c 'warning:' "$BUILD_DIR/build.log" || true)
    echo "[ci] compiler warnings: $WARNINGS"
    if [ "$WARNINGS" -gt 0 ]; then
        grep 'warning:' "$BUILD_DIR/build.log" | sed 's/.*warning: //' \
            | sort | uniq -c | sort -rn | head -10
    fi
fi

BIN="$BUILD_DIR/Raypr3sso"
SERVER="$BUILD_DIR/render_server"
for exe in "$BIN" "$SERVER"; do
    [ -x "$exe" ] || { echo "[ci] ERROR: missing $exe (run without --skip-build?)" >&2; exit 1; }
done

SMOKE_DIR="$(mktemp -d -t raypr3sso_ci_XXXXXX)"
trap 'rm -rf "$SMOKE_DIR"' EXIT

# Fixed-seed ball render through the console. Prints the PNG path.
render_ball() {  # <backend> <out-dir> [ENV=VAL ...]
    local backend=$1 out=$2
    shift 2
    mkdir -p "$out"
    local log
    log=$(mktemp "$SMOKE_DIR/render_${backend}_XXXXXX.log")
    {
        printf 'create model ball ball diffuse\ncreate args ci\n'
        printf '160\n90\n16\n4\n1.0\n2.5 1.2 2.0\n-0.679 -0.380 -0.543\n35\n%s\n' "$out"
        printf 'render ball ci %s\nexit\n' "$backend"
    } | env "$@" "$BIN" >"$log" 2>&1
    local png
    png=$(find "$out" -maxdepth 1 -name '*.png' | head -1)
    if [ -z "$png" ]; then
        echo "[ci] ERROR: $backend render produced no PNG; tail of $log:" >&2
        tail -40 "$log" >&2
        exit 1
    fi
    echo "$png"
}

echo "[ci] CPU determinism (two identical runs)"
CPU1=$(render_ball cpu "$SMOKE_DIR/cpu1")
CPU2=$(render_ball cpu "$SMOKE_DIR/cpu2")
cmp "$CPU1" "$CPU2"
echo "[ci] CPU determinism OK"

if [ "$GPU" = 1 ]; then
    echo "[ci] GPU determinism + wavefront==megakernel"
    G0=$(render_ball gpu "$SMOKE_DIR/gpu0" RENDER_WAVEFRONT=0)
    G1=$(render_ball gpu "$SMOKE_DIR/gpu1" RENDER_WAVEFRONT=0)
    G2=$(render_ball gpu "$SMOKE_DIR/gpu2" RENDER_WAVEFRONT=1)
    cmp "$G0" "$G1"
    cmp "$G0" "$G2"
    echo "[ci] GPU determinism / wavefront OK"
else
    echo "[ci] no GPU visible; GPU checks skipped"
fi

if [ "$RUN_AB" = 1 ]; then
    if [ "$GPU" = 0 ]; then
        echo "[ci] ERROR: --ab needs a GPU" >&2
        exit 1
    fi
    echo "[ci] server chunked==one-shot byte A/B"
    BIN="$SERVER" SPP="${AB_SPP:-64}" CHUNK="${AB_CHUNK:-8}" \
        PORT="${AB_PORT:-9101}" scripts/run_server_chunk_ab.sh
fi

echo "[ci] OK"