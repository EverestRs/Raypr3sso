#!/usr/bin/env python3
# Mid-render camera-move A/B (nextSample reset race detector).
#
# A render submitted while ANOTHER chunk's sample loop is mid-flight must
# produce byte-identical output to the same render submitted on a quiet
# server (rendering is deterministic: fixed baseSeed).
#
# Timing: the progress counter only updates at chunk boundaries, and between
# chunks the worker runs encodePreview (tens of ms) before queueing the next
# chunk — polling progress therefore tends to fire inside that harmless gap.
# Instead we watch the server log for "renderGPUWavefront: entered" (printed
# BEFORE the sample loop, fflush'ed), sleep into the loop, then fire.
#
# Usage: test_midmove_ab.py [port] [server_binary]
import glob
import json
import os
import subprocess
import sys
import tempfile
import time
import urllib.request

TOKEN = "testtoken"
PORT = int(sys.argv[1]) if len(sys.argv) > 1 else 9137
BIN = sys.argv[2] if len(sys.argv) > 2 else "./build/render_server"
W, H, SPP, CHUNK = 1600, 900, 1024, 256


def req(method, path, body=None, timeout=30):
    r = urllib.request.Request(
        f"http://127.0.0.1:{PORT}{path}", method=method,
        data=json.dumps(body).encode() if body is not None else None,
        headers={"Authorization": f"Bearer {TOKEN}",
                 "Content-Type": "application/json"})
    with urllib.request.urlopen(r, timeout=timeout) as resp:
        return json.loads(resp.read().decode())


def main():
    dump = tempfile.mkdtemp(prefix="midmove_")
    env = {k: v for k, v in os.environ.items()
           if not k.startswith("RENDER_")}
    env["RENDER_TOKEN"] = TOKEN
    env["RENDER_SERVER_DUMP_DIR"] = dump
    log_path = os.path.join(dump, "server.log")
    log = open(log_path, "wb")
    srv = subprocess.Popen([BIN, "127.0.0.1", str(PORT)], env=env,
                           stdout=log, stderr=log)
    try:
        for _ in range(100):
            try:
                req("GET", "/api/health")
                break
            except Exception:
                time.sleep(0.2)
        else:
            raise SystemExit("server did not come up")

        sid = req("POST", "/api/sessions", {"model": "sphere-test"})["sessionId"]
        for _ in range(200):
            if req("GET", f"/api/sessions/{sid}")["state"] == "ready":
                break
            time.sleep(0.2)
        else:
            raise SystemExit("sphere scene failed to load")

        c1 = {"position": [2.5, 1.2, 2.0], "direction": [-0.7, -0.35, -0.61],
              "up": [0, 1, 0], "fov": 35}
        c2 = {"position": [2.0, 1.0, 2.5], "direction": [-0.6, -0.3, -0.74],
              "up": [0, 1, 0], "fov": 35}
        body = {"width": W, "height": H, "sppBudget": SPP, "maxDepth": 4}

        # 1) chunked chain at C1; wait until its FIRST chunk's sample loop is
        #    running ("entered" line), then sleep well into the loop.
        req("POST", f"/api/sessions/{sid}/camera", c1)
        r1 = req("POST", f"/api/sessions/{sid}/render",
                 dict(body, chunkSpp=CHUNK))
        for _ in range(2000):
            with open(log_path, "rb") as f:
                if f.read().count(b"renderGPUWavefront: entered") >= 1:
                    break
            time.sleep(0.005)
        else:
            raise SystemExit("chunk 1 never entered the sample loop")
        time.sleep(0.5)  # now provably inside chunk 1's sample loop

        # 2) move the camera + submit mid-loop
        req("POST", f"/api/sessions/{sid}/camera", c2)
        r2 = req("POST", f"/api/sessions/{sid}/render", body)
        wait_done(r2["renderId"])

        # 3) same render again on a quiet server (fresh generation)
        req("POST", f"/api/sessions/{sid}/camera", c2)
        r3 = req("POST", f"/api/sessions/{sid}/render", body)
        wait_done(r3["renderId"])

        # 4) the direct race signature: r2 must have entered at start=0.
        #    start>0 means the cancelled chunk's final nextSample store
        #    overwrote the reset and the chain resumed at a stale index.
        entered = []
        with open(log_path, "rb") as f:
            for line in f.read().decode(errors="replace").splitlines():
                if "renderGPUWavefront: entered" in line:
                    entered.append(line)
        print(f"render calls: {len(entered)}")
        for line in entered:
            print(f"  {line.split('renderGPUWavefront: ')[1]}")

        # r2/r3 are the 2nd and 3rd render calls (r1's chunks are spp=CHUNK;
        # a raced r2 shows a shifted start AND a shrunk spp like 1018)
        def entered_start(line):
            return int(line.split(" start=")[1].split(" ")[0])
        r2_start = entered_start(entered[1]) if len(entered) >= 3 else -1
        start_ok = r2_start == 0
        print(f"r2 start sample: {r2_start} "
              f"({'OK' if start_ok else 'RACE: stale start'})")

        # 5) byte-compare the raw radiance dumps
        ok = start_ok
        for ch in ("Dd", "Ds", "Id", "Is", "Em"):
            a = glob.glob(f"{dump}/{r2['renderId']}_gen*_spp{SPP}_{ch}.f32")
            b = glob.glob(f"{dump}/{r3['renderId']}_gen*_spp{SPP}_{ch}.f32")
            if not a or not b:
                print(f"  {ch}: dump missing (a={a} b={b})")
                ok = False
                continue
            same = open(a[0], "rb").read() == open(b[0], "rb").read()
            print(f"  {ch}: {'identical' if same else 'MISMATCH'}")
            ok = ok and same
        print("MID-MOVE A/B PASS" if ok else "MID-MOVE A/B FAIL")
        return 0 if ok else 1
    finally:
        srv.terminate()
        try:
            srv.wait(timeout=5)
        except Exception:
            srv.kill()
        print(f"artifacts kept: {dump}")


def wait_done(render_id):
    for _ in range(6000):
        st = req("GET", f"/api/renders/{render_id}")
        if st["state"] in ("completed", "failed", "canceled"):
            if st["state"] != "completed":
                raise SystemExit(f"{render_id} ended {st['state']}")
            return
        time.sleep(0.05)
    raise SystemExit(f"{render_id} timed out")


if __name__ == "__main__":
    sys.exit(main())
