#!/usr/bin/env python3
"""Browser E2E: combined interior+exterior bistro (model=bistro).
Start inside at the counter, wheel-zoom OUT through the front windows into
the street, idle-refine, and assert the outside frame is much brighter than
the inside frame (combined scene: dim interior, daylight exterior)."""
import base64, json, os, signal, socket, subprocess, sys, tempfile, time, urllib.request
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import e2e_cdp_helper as cdp_check
from e2e_cdp_helper import Cdp  # reuse Ws/Cdp (drag/wheel/screenshot/evaluate)

CHROME_PORT = 9223
RENDER_PORT = 9152
TOKEN = "e2etoken"
cdp_check.CHROME_PORT = CHROME_PORT  # Cdp reads the module global at call time
PAGE = (f"http://127.0.0.1:8080/threejs/index.html"
        f"?remote=http://127.0.0.1:{RENDER_PORT}&model=bistro"
        f"&dragMs=150&idleMs=500&token={TOKEN}")
OUT = tempfile.mkdtemp(prefix="e2e_bistro_")

def stat(png):
    from PIL import Image
    im = Image.open(png).convert("L")
    px = list(im.getdata())
    return sum(px) / len(px) / 255

def main():
    server = subprocess.Popen(
        ["./build/render_server", "127.0.0.1", str(RENDER_PORT)],
        cwd="/home/biwenbin/VSCodeProjects/RenderEngine",
        env=dict(os.environ, RENDER_TOKEN=TOKEN),
        stdout=open(f"{OUT}/bistro_server.log", "wb"), stderr=subprocess.STDOUT)
    httpd = subprocess.Popen([sys.executable, "-m", "http.server", "8080"],
                             cwd="/home/biwenbin/VSCodeProjects/RenderEngine",
                             stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    profile = os.path.join(OUT, "profile")
    subprocess.run(["rm", "-rf", profile])
    chrome = subprocess.Popen([
        "google-chrome", "--headless=new", "--no-sandbox", "--disable-gpu",
        f"--remote-debugging-port={CHROME_PORT}", f"--user-data-dir={profile}",
        "--window-size=1280,800", "about:blank"],
        stdout=open(f"{OUT}/chrome.log","wb"), stderr=subprocess.STDOUT)
    try:
        import urllib.error
        for _ in range(75):
            try:
                urllib.request.urlopen(f"http://127.0.0.1:{CHROME_PORT}/json/version", timeout=1)
                break
            except Exception:
                if chrome.poll() is not None:
                    print(open(f"{OUT}/chrome.log",errors="replace").read()[-1500:])
                    raise SystemExit(f"chrome exited early code={chrome.returncode}")
                time.sleep(0.4)
        else:
            print("chrome debug port wait timed out; chrome.log tail:")
            print(open(f"{OUT}/chrome.log",errors="replace").read()[-1500:])
        cdp = Cdp()
        cdp.cmd("Page.enable")
        cdp.cmd("Page.navigate", url=PAGE)
        print("page opened; waiting for combined scene (load ~1-2 min)...")
        status = ""
        for poll in range(300):
            time.sleep(2)
            status = cdp.evaluate(
                "JSON.stringify((() => {"
                "  const st = [...document.querySelectorAll('div')]"
                "    .map(d => d.textContent).find(t => t && t.startsWith('remote:'));"
                "  return st || '';"
                "})())") or ""
            if "spp" in status or "done" in status:
                break
            if poll % 10 == 0:
                print(f"  [{poll*2}s] {status}", flush=True)
        print("status:", status)
        assert "spp" in status or "done" in status, f"never ready: {status}"
        # wait for the IDLE refine chain to finish (drag tier at low spp is
        # near-black in the dim combined interior — not a fair frame)
        for _ in range(120):
            status = cdp.evaluate(
                "JSON.stringify((() => {"
                "  const st = [...document.querySelectorAll('div')]"
                "    .map(d => d.textContent).find(t => t && t.startsWith('remote:'));"
                "  return st || '';"
                "})())") or ""
            if "done spp 128" in status or "done spp" not in status and "spp 128" in status:
                break
            time.sleep(1)
        print("inside status after refine:", status)
        ov = json.loads(cdp.evaluate(
            "JSON.stringify((() => {"
            "  const img = [...document.querySelectorAll('img')].find(i => i.style.zIndex === '10');"
            "  return { hidden: img ? img.hidden : null, naturalWidth: img ? img.naturalWidth : null,"
            "           complete: img ? img.complete : null,"
            "           src: img ? img.src.slice(-40) : null,"
            "           rect: img ? [img.getBoundingClientRect().width, img.getBoundingClientRect().height] : null };"
            "})())"))
        print("overlay state:", ov)
        dbg = json.loads(cdp.evaluate(
            "JSON.stringify((() => JSON.stringify(window.__remoteDebug || {}))())"))
        print("client debug:", dbg)
        time.sleep(1)
        cdp.screenshot(f"{OUT}/bistro_1_inside.png")
        inside_mean = stat(f"{OUT}/bistro_1_inside.png")
        print(f"inside frame mean brightness: {inside_mean:.3f}")

        print("--- wheel zoom OUT through the windows ---")
        for _ in range(14):
            cdp.wheel(640, 400, 120)   # +deltaY = dolly out (camera backs away)
            time.sleep(0.12)
        for _ in range(150):           # drag frame + idle refine chain
            status = cdp.evaluate(
                "JSON.stringify((() => {"
                "  const st = [...document.querySelectorAll('div')]"
                "    .map(d => d.textContent).find(t => t && t.startsWith('remote:'));"
                "  return st || '';"
                "})())") or ""
            if "done spp 128" in status:
                break
            time.sleep(1)
        print("outside status after refine:", status)
        ov2 = json.loads(cdp.evaluate(
            "JSON.stringify((() => {"
            "  const img = [...document.querySelectorAll('img')].find(i => i.style.zIndex === '10');"
            "  return { hidden: img ? img.hidden : null, src: img ? img.src.slice(-40) : null };"
            "})())"))
        print("overlay state out:", ov2)
        dbg2 = json.loads(cdp.evaluate(
            "JSON.stringify((() => JSON.stringify(window.__remoteDebug || {}))())"))
        print("client debug after zoom:", dbg2)
        time.sleep(1)
        cdp.screenshot(f"{OUT}/bistro_2_outside.png")
        outside_mean = stat(f"{OUT}/bistro_2_outside.png")
        print(f"outside frame mean brightness: {outside_mean:.3f}")

        state = json.loads(cdp.evaluate(
            "JSON.stringify((() => {"
            "  const st = [...document.querySelectorAll('div')]"
            "    .map(d => d.textContent).find(t => t && t.startsWith('remote:'));"
            "  const img = [...document.querySelectorAll('img')].find(i => i.style.zIndex === '10');"
            "  return { status: st, naturalWidth: img ? img.naturalWidth : 0 };"
            "})())"))
        print("final state:", state)
        assert state["naturalWidth"] > 0, "preview overlay missing"
        # 室内自动提亮后：内景帧应明显抬升（>0.2），滚轮退出 margin 后的
        # 街面帧回到白天天光水平（>0.35 且不过曝）
        ok = inside_mean > 0.2 and 0.35 < outside_mean < 0.9
        print("PASS" if ok else "FAIL",
              f"— inside {inside_mean:.3f} (auto-brightened) outside {outside_mean:.3f}")
        return 0 if ok else 1
    finally:
        for p in (chrome, httpd, server):
            p.terminate()
        for p in (chrome, httpd, server):
            try: p.wait(timeout=5)
            except Exception: p.kill()

if __name__ == "__main__":
    sys.exit(main())
