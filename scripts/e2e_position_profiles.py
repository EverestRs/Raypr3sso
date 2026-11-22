#!/usr/bin/env python3
"""Acceptance: position-aware rig profiles with NO env overrides at all.
One server process, bistro preset, two cameras. The server must pick the
indoor profile (sun85 low, fixtures on, tier exposure 2.4) for the bar
camera and the street profile (sun18 high, fixtures ~off, bands, exp 0.65)
for the street camera — purely from camera position.
"""
import json, os, subprocess, sys, time, urllib.request

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
BIN = os.path.join(REPO, "build/render_server")
TOKEN = "devtoken"
PORT = 9188
OUTDIR = "/tmp/raypr_compare/out_ACCEPT"
os.makedirs(OUTDIR, exist_ok=True)

CAM_IN = {"position": [-0.833, 3.099, -1.471], "direction": [0.993846, -0.104828, -0.0358017],
          "up": [0.104761, 0.99449, -0.00377383], "fov": 40.5}
CAM_ST = {"position": [-17.0, 3.2, 0.7], "direction": [1.0, -0.11, -0.09],
          "up": [0.0, 1.0, 0.0], "fov": 50.0}

def req(method, path, body=None, binary=False):
    url = f"http://127.0.0.1:{PORT}{path}"
    data = json.dumps(body).encode() if body is not None else None
    r = urllib.request.Request(url, data=data, method=method,
                               headers={"Authorization": f"Bearer {TOKEN}",
                                        "Content-Type": "application/json"})
    with urllib.request.urlopen(r, timeout=180) as resp:
        raw = resp.read()
        return raw if binary else json.loads(raw.decode())

def render(sid, name, cam):
    body = {"width": 960, "height": 540, "sppBudget": 256, "maxDepth": 12,
            "denoise": 1, "progressive": 1, "camera": cam}
    r = req("POST", f"/api/sessions/{sid}/render", body)
    rid = r["renderId"]
    print(f"[accept] {name}: {rid}", flush=True)
    for _ in range(1800):
        st = req("GET", f"/api/renders/{rid}")
        if st.get("state") == "completed": break
        if st.get("state") in ("failed", "cancelled"): raise RuntimeError(st)
        time.sleep(3)
    else:
        raise RuntimeError("timeout")
    png = None
    for _ in range(30):
        try:
            png = req("GET", f"/api/renders/{rid}/image", binary=True); break
        except urllib.error.HTTPError as e:
            if e.code != 404: raise
            time.sleep(2)
    open(os.path.join(OUTDIR, f"{name}.png"), "wb").write(png)
    print(f"[accept] {name} saved", flush=True)

def main():
    log = open(os.path.join(OUTDIR, "server.log"), "wb")
    # NO RENDER_* env — only token + OIDN (a process capability, not a look knob)
    env = dict(os.environ, RENDER_TOKEN=TOKEN, RENDER_OIDN="1")
    srv = subprocess.Popen([BIN, "127.0.0.1", str(PORT)], cwd=REPO, env=env,
                           stdout=log, stderr=subprocess.STDOUT)
    try:
        for _ in range(100):
            try: req("GET", "/api/health"); break
            except Exception: time.sleep(0.3)
        sess = req("POST", "/api/sessions", {"model": "bistro"})
        sid = sess["sessionId"]
        print("[accept] loading...", flush=True)
        for _ in range(1200):
            st = req("GET", f"/api/sessions/{sid}")
            if st.get("state") == "ready": break
            if st.get("state") == "failed": raise RuntimeError(st)
            time.sleep(2)
        render(sid, "indoor", CAM_IN)
        render(sid, "street", CAM_ST)
    finally:
        srv.terminate()
        try: srv.wait(10)
        except Exception: srv.kill()

if __name__ == "__main__":
    main()
    from PIL import Image
    import numpy as np
    for f in sorted(os.listdir(OUTDIR)):
        if not f.endswith(".png"): continue
        a = np.asarray(Image.open(os.path.join(OUTDIR, f)).convert("RGB"), dtype=np.float64)/255.
        a = a.reshape(-1, 3); med = np.median(a, 0)
        lum = a @ [0.2126, 0.7152, 0.0722]
        mx, mn = a.max(1, keepdims=True), a.min(1, keepdims=True)
        sat = np.where(mx > 1e-6, (mx-mn)/(mx+1e-9), 0)
        print(f"STATS {f:14s} medRGB=({med[0]:.3f},{med[1]:.3f},{med[2]:.3f}) R:G={med[0]/med[1]:.2f} "
              f"B:G={med[2]/med[1]:.2f} luma={lum.mean():.3f} medSat={np.median(sat):.3f} "
              f"p95Sat={np.percentile(sat,95):.3f} blown={(a.min(1)>0.95).mean()*100:.1f}%")
    print("EXPECT indoor ~ (0.44,0.28,0.26) luma 0.40; street ~ (0.34,0.35,0.36) R:G~0.97 luma~0.42")
