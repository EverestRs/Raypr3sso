#!/usr/bin/env bash
# 阶段三 acceptance: chunk-boundary preview export + CLI-identical color
# pipeline.
#   1. Sphere session renders 256 spp in 8-spp chunks; the poller saves a
#      preview PNG per sequence bump; a luminance-gradient noise metric must
#      fall between the first and last captured previews.
#   2. Bistro session renders the CLI raw-baseline view at 4 spp; the served
#      preview PNG must be byte-identical to `RENDER_DENOISE=0` CLI output
#      (same Photo::postProcess pipeline on both sides).
set -euo pipefail
TOKEN="${TOKEN:-devtoken}"
cd "$(dirname "$0")/.."

DUMP=$(mktemp -d)
PORT=${PORT:-9121}
CLI_HASH="3ed9927567d0a047ecc63aaf90c209b46a0c11ce9cd65ad6f2ef2761acc12bc2"
RENDER_TOKEN="$TOKEN" stdbuf -o0 -e0 ./build/render_server 127.0.0.1 "$PORT" > "$DUMP/server.log" 2>&1 &
SRV=$!
cleanup() {
    kill "$SRV" 2>/dev/null || true
    if [ "${fail:-0}" = 0 ]; then rm -rf "$DUMP"; else echo "dumps kept: $DUMP"; fi
}
trap cleanup EXIT
fail=0
for _ in $(seq 50); do
    curl -s -H "Authorization: Bearer $TOKEN" "localhost:$PORT/api/health" > /dev/null 2>&1 && break
    sleep 0.2
done

echo "--- sphere progressive preview (256 spp, 8-spp chunks) ---"
SID=$(curl -s -H "Authorization: Bearer $TOKEN" -X POST localhost:$PORT/api/sessions \
    -d '{"model":"sphere-test","preset":"ball"}' | sed 's/.*"sessionId":"\([^"]*\)".*/\1/')
echo "  session: $SID"
for _ in $(seq 300); do
    ST=$(curl -s -H "Authorization: Bearer $TOKEN" "localhost:$PORT/api/sessions/$SID" | sed 's/.*"state":"\([^"]*\)".*/\1/')
    [ "$ST" = "ready" ] && break
    if [ "$ST" = "failed" ]; then echo "  load FAILED"; grep "scene registry" "$DUMP/server.log"; exit 1; fi
    sleep 0.3
done
[ "$ST" = "ready" ] || { echo "  FAIL: sphere not ready"; exit 1; }
curl -s -H "Authorization: Bearer $TOKEN" -X POST "localhost:$PORT/api/sessions/$SID/render" \
    -d '{"width":320,"height":180,"sppBudget":256,"chunkSpp":8,"maxDepth":4}' > /dev/null

# Save every distinct preview we can catch while the chain runs.
LAST=0
for _ in $(seq 600); do
    META=$(curl -s -H "Authorization: Bearer $TOKEN" "localhost:$PORT/api/sessions/$SID")
    SEQ=$(echo "$META" | python3 -c "import json,sys; print(json.load(sys.stdin)['previewSequence'])")
    if [ "$SEQ" -gt "$LAST" ] 2>/dev/null; then
        curl -s -H "Authorization: Bearer $TOKEN" "localhost:$PORT/api/sessions/$SID/preview" -o "$DUMP/prev_$SEQ.png"
        LAST=$SEQ
        SPP=$(echo "$META" | python3 -c "import json,sys; print(json.load(sys.stdin)['previewSpp'])")
        echo "  caught sequence $SEQ (spp $SPP)"
    fi
    [ "$SEQ" -ge 32 ] && break
    sleep 0.05
done
echo "  final sequence: $LAST (expect 32)"
[ "$LAST" -ge 32 ] || { echo "  FAIL: chain did not publish 32 previews"; fail=1; exit 1; }
N=$(ls "$DUMP"/prev_*.png 2>/dev/null | wc -l)
echo "  previews captured: $N"

python3 - "$DUMP" <<'PYEOF'
import glob, os, sys
from PIL import Image
files = sorted(glob.glob(os.path.join(sys.argv[1], "prev_*.png")),
               key=lambda p: int(p.rsplit("_", 1)[1].split(".")[0]))
assert files, "no previews captured"
metrics = []
for p in files:
    im = Image.open(p).convert("L")
    w, h = im.size
    px = list(im.getdata())
    grad = sum(abs(px[i] - px[i + 1]) for y in range(h)
               for i in range(y * w, y * w + w - 1)) / ((w - 1) * h)
    metrics.append((int(p.rsplit("_", 1)[1].split(".")[0]), im.size, grad))
    print(f"  seq {metrics[-1][0]:>3} {im.size} noise-grad {grad:.4f}")
first, last = metrics[0][2], metrics[-1][2]
print(f"  noise first {first:.4f} -> last {last:.4f} ({'CONVERGING' if last < first else 'NOT CONVERGING'})")
sys.exit(0 if last < first else 1)
PYEOF
echo "  progressive convergence OK"

echo "--- bistro preview vs CLI raw baseline (byte-identical pipeline) ---"
BID=$(curl -s -H "Authorization: Bearer $TOKEN" -X POST localhost:$PORT/api/sessions \
    -d '{"model":"bistro-interior","preset":"interior"}' | sed 's/.*"sessionId":"\([^"]*\)".*/\1/')
for _ in $(seq 900); do
    ST=$(curl -s -H "Authorization: Bearer $TOKEN" "localhost:$PORT/api/sessions/$BID" | sed 's/.*"state":"\([^"]*\)".*/\1/')
    [ "$ST" = "ready" ] && break
    if [ "$ST" = "failed" ]; then echo "  load FAILED"; grep "scene registry" "$DUMP/server.log"; exit 1; fi
    sleep 3
done
[ "$ST" = "ready" ] || { echo "  FAIL: bistro not ready"; exit 1; }
curl -s -H "Authorization: Bearer $TOKEN" -X POST "localhost:$PORT/api/sessions/$BID/camera" \
    -d '{"position":[-0.833,3.099,-1.471],"direction":[0.993846,-0.104828,-0.0358017],"up":[0,1,0],"fov":40.5}' > /dev/null
curl -s -H "Authorization: Bearer $TOKEN" -X POST "localhost:$PORT/api/sessions/$BID/render" \
    -d '{"width":320,"height":180,"sppBudget":4,"maxDepth":4,"exposure":1.0}' > /dev/null
for _ in $(seq 600); do
    SEQ=$(curl -s -H "Authorization: Bearer $TOKEN" "localhost:$PORT/api/sessions/$BID" | python3 -c "import json,sys; print(json.load(sys.stdin)['previewSequence'])")
    [ "$SEQ" -ge 1 ] && break
    sleep 0.5
done
curl -s -H "Authorization: Bearer $TOKEN" "localhost:$PORT/api/sessions/$BID/preview" -o "$DUMP/bistro_preview.png"
GOT=$(sha256sum "$DUMP/bistro_preview.png" | cut -d' ' -f1)
echo "  server preview: $GOT"
echo "  CLI raw (RENDER_DENOISE=0): $CLI_HASH"
if [ "$GOT" = "$CLI_HASH" ]; then
    echo "  BYTE-IDENTICAL: server preview == CLI postProcess output"
else
    echo "  FAIL: preview bytes differ from CLI baseline"; exit 1
fi
echo "ALL PREVIEW CHECKS PASS"
