#!/bin/bash
# Run all single sphere tests on both CPU and GPU backends

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_DIR="$SCRIPT_DIR/.."
BUILD_DIR="$PROJECT_DIR/build"
cd "$BUILD_DIR" || exit 1

SPP=${1:-128}
WIDTH=512
HEIGHT=512
CAM_POS="2.5 1.2 2.0"
CAM_DIR="-0.679 -0.380 -0.543"
FOV="35"

echo "Running ball tests at ${WIDTH}x${HEIGHT}, ${SPP}spp..."
echo "Project dir: ${PROJECT_DIR}"
echo "========================================"

render_one() {
    local model_type=$1
    local backend=$2
    local out_dir="${PROJECT_DIR}/$3"
    local max_depth=$4
    local exposure=$5
    
    local args_id="ball_${model_type}_${backend}"
    local out_path="${out_dir}/ball_${model_type}"
    
    mkdir -p "${out_path}"
    
    echo "--- Rendering ${model_type} ball on ${backend} → ${out_path}/render.png ---"
    
    {
        echo "create model ball ball ${model_type}"
        echo "create args ${args_id}"
        echo "${WIDTH}"
        echo "${HEIGHT}"
        echo "${SPP}"
        echo "${max_depth}"
        echo "${exposure}"
        echo "${CAM_POS}"
        echo "${CAM_DIR}"
        echo "${FOV}"
        echo "${out_path}"
        echo "render ball ${args_id} ${backend}"
        echo "exit"
    } | ./Raypr3sso 2>&1 | grep -E "Done|Rendering|Output|Error|error|failed|Failed"
    
    if [ -f "${out_path}/render.png" ]; then
        echo "  ✓ Output: ${out_path}/render.png"
    else
        echo "  ✗ No output generated!"
    fi
    echo ""
}

# CPU tests (run in sequence)
echo "=== CPU Backend Tests ==="
render_one "diffuse" "cpu" "output_cpu"  "16" "1.0"
render_one "rough"   "cpu" "output_cpu"  "16" "1.0"
render_one "glass"   "cpu" "output_cpu"  "48" "1.5"

# GPU tests
echo "=== GPU Backend Tests ==="
render_one "diffuse" "gpu" "output_gpu"  "16" "1.0"
render_one "rough"   "gpu" "output_gpu"  "16" "1.0"
render_one "glass"   "gpu" "output_gpu"  "48" "1.5"

echo "========================================"
echo "All tests complete. Output:"
echo "CPU outputs:"
ls -la "${PROJECT_DIR}"/output_cpu/ball_*/render.png 2>/dev/null
echo "GPU outputs:"
ls -la "${PROJECT_DIR}"/output_gpu/ball_*/render.png 2>/dev/null
