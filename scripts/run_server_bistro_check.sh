#!/usr/bin/env bash
# Step-2 acceptance: scene registry + GPU-worker async loading + Bistro
# binding. Run from anywhere; the server is started from the repo root
# (whitelist model paths are relative to it).
set -euo pipefail
TOKEN="${TOKEN:-devtoken}"
cd "$(dirname "$0")/.."

DUMP=$(mktemp -d)
PORT=${PORT:-9111}
RENDER_SERVER_DUMP_DIR="$DUMP" RENDER_TOKEN="$TOKEN" stdbuf -o0 -e0 ./build/render_server 127.0.0.1 "$PORT" > "$DUMP/server.log" 2>&1 &
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

echo "--- unknown model rejected ---"
CODE=$(curl -s -H "Authorization: Bearer $TOKEN" -o /dev/null -w '%{http_code}' -X POST localhost:$PORT/api/sessions -d '{"model":"../../etc/passwd"}')
[ "$CODE" = 400 ] && echo "  unknown_model -> $CODE OK" || { echo "  FAIL: got $CODE, want 400"; exit 1; }

echo "--- bistro session (preset interior) ---"
R=$(curl -s -H "Authorization: Bearer $TOKEN" -X POST localhost:$PORT/api/sessions -d '{"model":"bistro-interior","preset":"interior"}')
echo "  $R"
SID=$(echo "$R" | sed 's/.*"sessionId":"\([^"]*\)".*/\1/')

echo "--- second session shares the same load ---"
R2=$(curl -s -H "Authorization: Bearer $TOKEN" -X POST localhost:$PORT/api/sessions -d '{"model":"bistro-interior"}')
echo "  $R2"
SID2=$(echo "$R2" | sed 's/.*"sessionId":"\([^"]*\)".*/\1/')

echo "--- render while loading must be rejected ---"
CODE=$(curl -s -H "Authorization: Bearer $TOKEN" -o /dev/null -w '%{http_code}' -X POST "localhost:$PORT/api/sessions/$SID/render" -d '{"width":320,"height":180,"sppBudget":4}')
[ "$CODE" = 409 ] && echo "  scene_not_ready -> $CODE OK" || { echo "  FAIL: got $CODE, want 409"; exit 1; }

echo "--- waiting for the GPU worker to finish loading (minutes) ---"
for _ in $(seq 900); do
    ST=$(curl -s -H "Authorization: Bearer $TOKEN" "localhost:$PORT/api/sessions/$SID" | sed 's/.*"state":"\([^"]*\)".*/\1/')
    [ "$ST" = "ready" ] && break
    if [ "$ST" = "failed" ]; then
        echo "  load FAILED"; grep "scene registry" "$DUMP/server.log" || tail -5 "$DUMP/server.log"
        exit 1
    fi
    sleep 2
done
ST2=$(curl -s -H "Authorization: Bearer $TOKEN" "localhost:$PORT/api/sessions/$SID2" | sed 's/.*"state":"\([^"]*\)".*/\1/')
echo "  session states: $ST / $ST2"
[ "$ST" = "ready" ] && [ "$ST2" = "ready" ] || { echo "  FAIL: not ready"; exit 1; }
N=$(grep -c "scene registry: 'bistro-interior' ready" "$DUMP/server.log")
echo "  load count: $N (expect 1 — sessions share the resident scene)"
[ "$N" = 1 ] || { echo "  FAIL: loaded more than once"; exit 1; }

echo "--- preset camera applied by default ---"
curl -s -H "Authorization: Bearer $TOKEN" "localhost:$PORT/api/sessions/$SID2" | grep -o '"camera":{[^}]*}'

echo "--- AOVSTAT cross-check vs CLI baseline ---"
curl -s -H "Authorization: Bearer $TOKEN" -X POST "localhost:$PORT/api/sessions/$SID/camera" \
    -d '{"position":[-0.833,3.099,-1.471],"direction":[0.993846,-0.104828,-0.0358017],"up":[0,1,0],"fov":40.5}' > /dev/null
curl -s -H "Authorization: Bearer $TOKEN" -X POST "localhost:$PORT/api/sessions/$SID/render" \
    -d '{"width":320,"height":180,"sppBudget":4,"maxDepth":4}' > /dev/null
wait_dump() {
    for _ in $(seq 600); do
        [ -n "$(compgen -G "$DUMP/$1")" ] && return 0
        sleep 0.3
    done
    echo "  TIMEOUT waiting for $1"; exit 1
}
wait_dump "r-1_gen1_spp4_Em.f32"
echo -n "  server: "; grep AOVSTAT "$DUMP/server.log" | tail -1
echo "  CLI baseline (no sun env): AOVSTAT: Dd=0.0077 Ds=0.0000 Id=0.1497 Is=0.0043 Em=0.0663"

echo "--- chunked vs one-shot on Bistro (8 spp, 4x2 chunks) ---"
curl -s -H "Authorization: Bearer $TOKEN" -X POST "localhost:$PORT/api/sessions/$SID/render" \
    -d '{"width":320,"height":180,"sppBudget":8,"maxDepth":4,"chunkSpp":2,"resetAccumulation":1}' > /dev/null
wait_dump "r-2_gen1_spp8_Em.f32"
curl -s -H "Authorization: Bearer $TOKEN" -X POST "localhost:$PORT/api/sessions/$SID/render" \
    -d '{"width":320,"height":180,"sppBudget":8,"maxDepth":4,"resetAccumulation":1}' > /dev/null
wait_dump "r-3_gen1_spp8_Em.f32"
ok=1
for ch in Dd Ds Id Is Em; do
    if cmp -s "$DUMP/r-2_gen1_spp8_$ch.f32" "$DUMP/r-3_gen1_spp8_$ch.f32"; then
        echo "  $ch: identical"
    else
        echo "  $ch: MISMATCH"; ok=0
    fi
done
[ "$ok" = 1 ] || { echo "  BISTRO A/B FAIL"; exit 1; }
echo "ALL BISTRO SERVER CHECKS PASS"
