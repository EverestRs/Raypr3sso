#!/bin/bash
# Reproduce the bistro GPU render to verify the overexposure + specular fixes.
cd /home/biwenbin/VSCodeProjects/RenderEngine

BIN=./build/RenderEngine
OUTDIR=output_gpu/render_bistro_verify

mkdir -p "$OUTDIR"

# Construct interactive commands. 'create args' prompts one value per line.
{
  echo "create model bistro models/Bistro_v5_2 BistroInterior.fbx null"
  echo "create args bistro_gpu"
  echo "480"           # Width
  echo "270"           # Height
  echo "256"           # Samples per pixel
  echo "16"            # Max depth
  echo "1.1"           # Exposure
  echo "-0.833 3.099 -1.471"   # Camera position
  echo "0.993846 -0.104828 -0.0358017"  # Camera direction
  echo "40.5"          # FOV
  echo "$OUTDIR"       # Output path
  echo "render bistro bistro_gpu gpu"
  echo "exit"
} | "$BIN" 2>&1 | tee /tmp/bistro_verify.log

echo "=== done ==="
ls -la "$OUTDIR"