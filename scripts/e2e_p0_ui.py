#!/usr/bin/env python3
"""P0 UI acceptance v4 — stable button ids, server camera as ground truth."""
import json, os, subprocess, sys, tempfile, time, urllib.request
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import e2e_cdp_helper as cdp_check
from e2e_cdp_helper import Cdp
cdp_check.CHROME_PORT = 9227
CHROME_PORT = 9227
RENDER_PORT = 9168
TOKEN = "devtoken"
PAGE = (f"http://127.0.0.1:8080/threejs/index.html"
        f"?remote=http://127.0.0.1:{RENDER_PORT}&model=bistro&dragMs=120&idleMs=400")

def http(path):
    r = urllib.request.Request(f"http://127.0.0.1:{RENDER_PORT}{path}",
                               headers={"Authorization": f"Bearer {TOKEN}"})
    with urllib.request.urlopen(r, timeout=30) as resp:
        return json.loads(resp.read())

def wait(cdp, needle, timeout=90):
    for _ in range(timeout):
        st = cdp.evaluate("JSON.stringify((()=>{const d=[...document.querySelectorAll('div')].map(d=>d.textContent).find(t=>t&&t.startsWith('remote:'));return d||''})())")
        if needle in st: return st
        time.sleep(1)
    return st

def overlay_w(cdp):
    return json.loads(cdp.evaluate("JSON.stringify((()=>{const i=[...document.querySelectorAll('img')].find(i=>i.style.zIndex==='10');return i?i.naturalWidth:0})())"))

def dist(a, b):
    return sum((x-y)**2 for x, y in zip(a, b))**0.5

def main():
    server = subprocess.Popen(["./build/render_server","127.0.0.1",str(RENDER_PORT)],
        cwd="/home/biwenbin/VSCodeProjects/RenderEngine",
        env=dict(os.environ, RENDER_TOKEN=TOKEN, RENDER_OIDN="1"),
        stdout=open(os.path.join(tempfile.gettempdir(), "p0_server.log"),"wb"), stderr=subprocess.STDOUT)
    httpd = subprocess.Popen([sys.executable,"-m","http.server","8080"],
        cwd="/home/biwenbin/VSCodeProjects/RenderEngine",
        stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    subprocess.run(["rm","-rf",os.path.join(tempfile.gettempdir(), "profile_p0")])
    chrome = subprocess.Popen(["google-chrome","--headless=new","--no-sandbox","--disable-gpu",
        f"--remote-debugging-port={CHROME_PORT}","--user-data-dir=/tmp/wf_e2e/profile_p0",
        "--window-size=1280,800","about:blank"], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    ok = True
    try:
        for _ in range(75):
            try:
                urllib.request.urlopen(f"http://127.0.0.1:{CHROME_PORT}/json/version", timeout=1); break
            except Exception: time.sleep(0.4)
        cdp = Cdp(); cdp.cmd("Page.enable")
        cdp.cmd("Page.navigate", url=PAGE)
        for _ in range(240):
            time.sleep(1)
            st = cdp.evaluate("JSON.stringify((()=>{const d=[...document.querySelectorAll('div')].map(d=>d.textContent).find(t=>t&&t.startsWith('remote:'));return d||''})())")
            if "spp" in st: break
        wait(cdp, "128/128"); time.sleep(1)
        sid = "s-1"
        preset = http(f"/api/sessions/{sid}")["camera"]["position"]
        print("preset cam:", [round(v,2) for v in preset])
        cdp.evaluate("window.__errs=[]; window.onerror=(m)=>window.__errs.push(String(m))")

        # 1) wheel 移走 → 点击回位(id) → server camera 回 preset
        for _ in range(8):
            cdp.wheel(640,400,240); time.sleep(0.25)
        time.sleep(3)
        moved = http(f"/api/sessions/{sid}")["camera"]["position"]
        dmove = dist(preset, moved)
        cdp.evaluate("document.getElementById('re-btn-home').click()")
        back = 99
        for _ in range(30):
            time.sleep(1)
            back = dist(preset, http(f"/api/sessions/{sid}")["camera"]["position"])
            if back < 0.2: break
        print(f"home: moved {dmove:.2f}m -> back to preset {back:.3f}  [{dmove>0.5 and back<0.2}] type(back)={type(back).__name__}")
        ok &= bool(dmove > 0.5 and back < 0.2)

        # 2) 点击 高清(by id 面板内 textContent) → overlay naturalWidth == 1280
        cdp.evaluate("(function(){const b=[...document.querySelectorAll('.re-q button')].find(b=>b.textContent==='高清');b.click();})()")
        w = 0
        for _ in range(90):
            time.sleep(1); w = overlay_w(cdp)
            if w == 1280: break
        print(f"after 高清: overlay naturalWidth = {w} [{w==1280}] type(w)={type(w).__name__}")
        ok &= bool(w == 1280)

        # 3) 曝光滑条 → 4.0
        cdp.evaluate("(function(){const s=document.querySelector('.re-panel input[type=range]');s.value='4.0';s.dispatchEvent(new Event('input'));})()")
        exp = 0
        for _ in range(60):
            time.sleep(1)
            exp = json.loads(cdp.evaluate("JSON.stringify(window.__remoteDebug.lastExposure)"))
            if abs(float(exp) - 4.0) < 0.01: break
        print(f"after slider: lastExposure = {exp} [{abs(float(exp)-4.0)<0.01}]")
        ok &= bool(abs(float(exp) - 4.0) < 0.01)

        # 4) 保存按钮点击无异常
        cdp.evaluate("document.getElementById('re-btn-save').click()")
        time.sleep(1.5)
        errs = cdp.evaluate("JSON.stringify(window.__errs||[])")
        print("save click errs:", errs)
        ok &= bool(len(json.loads(errs)) == 0)

        print("P0 UI ACCEPTANCE", "PASS" if ok else "FAIL")
        return 0 if ok else 1
    finally:
        for p in (chrome, httpd, server):
            p.terminate()
        for p in (chrome, httpd, server):
            try: p.wait(timeout=5)
            except Exception: p.kill()

if __name__ == "__main__":
    sys.exit(main())
