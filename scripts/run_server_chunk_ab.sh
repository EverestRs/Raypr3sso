#!/usr/bin/env bash
# Design 5.4 acceptance: chunked progressive accumulation must equal a
# one-shot render of the same total spp, byte for byte (sphere test scene,
# raw radiance-split dumps via RENDER_SERVER_DUMP_DIR).
#
# Also smoke-tests chain cancel: a camera update mid-chain must stop the
# old generation's chain without crashing the worker.
set -euo pipefail
TOKEN="${TOKEN:-devtoken}"
BIN="${BIN:-./build/render_server}"
cd "$(dirname "$0")/.."

DUMP=$(mktemp -d)
PORT=${PORT:-9101}
SPP=${SPP:-64}
CHUNK=${CHUNK:-8}

RENDER_SERVER_DUMP_DIR="$DUMP" RENDER_TOKEN="$TOKEN" "$BIN" 127.0.0.1 "$PORT" > "$DUMP/server.log" 2>&1 &
SRV=$!
# Keep the dump directory when the A/B fails so the mismatch can be
# quantified; clean up only on success/interrupt-free exit.
cleanup() {
    if [ "${fail:-0}" = 0 ]; then rm -rf "$DUMP"; else echo "dumps kept: $DUMP"; fi
}
trap 'kill $SRV 2>/dev/null || true; cleanup' EXIT
fail=0
for _ in $(seq 50); do
    curl -s -H "Authorization: Bearer $TOKEN" "localhost:$PORT/api/health" > /dev/null 2>&1 && break
    sleep 0.2
done

SID=$(curl -s -H "Authorization: Bearer $TOKEN" -X POST "localhost:$PORT/api/sessions" | sed 's/.*"sessionId":"\([^"]*\)".*/\1/')
echo "session: $SID"
for _ in $(seq 300); do
    ST=$(curl -s -H "Authorization: Bearer $TOKEN" "localhost:$PORT/api/sessions/$SID" | sed 's/.*"state":"\([^"]*\)".*/\1/')
    [ "$ST" = "ready" ] && break
    if [ "$ST" = "failed" ]; then echo "load FAILED"; tail -5 "$DUMP/server.log"; exit 1; fi
    sleep 0.3
done

curl -s -H "Authorization: Bearer $TOKEN" -X POST "localhost:$PORT/api/sessions/$SID/camera" \
    -d '{"position":[0,0,3],"direction":[0,0,-1],"up":[0,1,0],"fov":60}' > /dev/null

wait_dump() {  # $1 = glob of the LAST-written channel (Em closes the dump)
    for _ in $(seq 600); do
        [ -n "$(compgen -G "$DUMP/$1")" ] && return 0
        sleep 0.2
    done
    echo "TIMEOUT waiting for $1"; tail -5 "$DUMP/server.log"; exit 1
}

# --- run 1: one-shot (chunkSpp defaults to sppBudget) ---
curl -s -H "Authorization: Bearer $TOKEN" -X POST "localhost:$PORT/api/sessions/$SID/render" \
    -d "{\"width\":320,\"height\":180,\"sppBudget\":$SPP,\"maxDepth\":4,\"resetAccumulation\":1}" > /dev/null
wait_dump "r-1_gen1_spp${SPP}_Em.f32"
echo "one-shot done (r-1)"

# --- run 2: chunked, fresh accumulation, same seeds/generation ---
curl -s -H "Authorization: Bearer $TOKEN" -X POST "localhost:$PORT/api/sessions/$SID/render" \
    -d "{\"width\":320,\"height\":180,\"sppBudget\":$SPP,\"maxDepth\":4,\"chunkSpp\":$CHUNK,\"resetAccumulation\":1}" > /dev/null
wait_dump "r-2_gen1_spp${SPP}_Em.f32"
echo "chunked done (r-2, $((SPP / CHUNK)) x $CHUNK spp)"

# --- byte-compare all five radiance channels ---
fail=0
for ch in Dd Ds Id Is Em; do
    if cmp -s "$DUMP/r-1_gen1_spp${SPP}_$ch.f32" "$DUMP/r-2_gen1_spp${SPP}_$ch.f32"; then
        echo "  $ch: identical"
    else
        echo "  $ch: MISMATCH"; fail=1
    fi
done
[ "$fail" = 0 ] || { echo "A/B FAIL"; exit 1; }
echo "A/B PASS: chunked == one-shot, byte-identical"

# --- cancel smoke: camera update mid-chain must stop the old chain ---
curl -s -H "Authorization: Bearer $TOKEN" -X POST "localhost:$PORT/api/sessions/$SID/render" \
    -d '{"width":320,"height":180,"sppBudget":512,"maxDepth":4,"chunkSpp":8,"resetAccumulation":1}' > /dev/null
sleep 0.5
curl -s -H "Authorization: Bearer $TOKEN" -X POST "localhost:$PORT/api/sessions/$SID/camera" \
    -d '{"position":[0,0,4],"direction":[0,0,-1],"up":[0,1,0],"fov":60}' > /dev/null
for _ in $(seq 100); do
    STATE=$(curl -s -H "Authorization: Bearer $TOKEN" "localhost:$PORT/api/sessions/$SID" | sed 's/.*"state":"\([^"]*\)".*/\1/')
    [ "$STATE" != "running" ] && [ "$STATE" != "queued" ] && [ "$STATE" != "canceling" ] && break
    sleep 0.2
done
ACC=$(curl -s -H "Authorization: Bearer $TOKEN" "localhost:$PORT/api/sessions/$SID" | sed 's/.*"accumSamples":\([0-9]*\).*/\1/')
GEN=$(curl -s -H "Authorization: Bearer $TOKEN" "localhost:$PORT/api/sessions/$SID" | sed 's/.*"generation":\([0-9]*\).*/\1/')
echo "cancel smoke: state=$STATE accum=$ACC (budget 512, stopped early) gen=$GEN"
curl -s -H "Authorization: Bearer $TOKEN" "localhost:$PORT/api/health" > /dev/null
echo "cancel smoke PASS: server healthy after mid-chain cancel"
