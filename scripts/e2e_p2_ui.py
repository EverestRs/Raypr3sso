#!/usr/bin/env python3
"""P2 acceptance: real load progress (scene-progress events), convergence ETA
row, A/B first-frame compare slider with draggable divider."""
import json, os, subprocess, sys, tempfile, time, urllib.request
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
import e2e_cdp_helper as cdp_check
from e2e_cdp_helper import Cdp
cdp_check.CHROME_PORT = 9231
CHROME_PORT = 9231
RENDER_PORT = 9171
TOKEN = "devtoken"
PAGE = (f"http://127.0.0.1:8080/threejs/index.html"
        f"?remote=http://127.0.0.1:{RENDER_PORT}&model=bistro&dragMs=120&idleMs=400")

def status(cdp):
    return cdp.evaluate("JSON.stringify((()=>{const d=[...document.querySelectorAll('div')].map(d=>d.textContent).find(t=>t&&t.startsWith('remote:'));return d||''})())")

def wait(cdp, needle, timeout=120):
    for _ in range(timeout):
        st = status(cdp)
        if needle in st: return st
        time.sleep(1)
    return st

def main():
    server = subprocess.Popen(["./build/render_server","127.0.0.1",str(RENDER_PORT)],
        cwd=REPO,
        env=dict(os.environ, RENDER_TOKEN=TOKEN, RENDER_OIDN="1"),
        stdout=open(os.path.join(tempfile.gettempdir(), "p2_server.log"),"wb"), stderr=subprocess.STDOUT)
    httpd = subprocess.Popen([sys.executable,"-m","http.server","8080"],
        cwd=REPO,
        stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    subprocess.run(["rm","-rf",os.path.join(tempfile.gettempdir(), "profile_p2")])
    chrome = subprocess.Popen(["google-chrome","--headless=new","--no-sandbox","--disable-gpu",
        f"--remote-debugging-port={CHROME_PORT}","--user-data-dir=/tmp/wf_e2e/profile_p2",
        "--window-size=1280,800","about:blank"], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    ok = True
    try:
        for _ in range(75):
            try:
                urllib.request.urlopen(f"http://127.0.0.1:{CHROME_PORT}/json/version", timeout=1); break
            except Exception: time.sleep(0.4)
        cdp = Cdp(); cdp.cmd("Page.enable")
        cdp.cmd("Page.navigate", url=PAGE)
        # ---- 1) 加载进度：轮询 sceneStages 直到 ≥3 个阶段 ----
        stages = []
        for _ in range(180):
            time.sleep(1)
            stages = json.loads(cdp.evaluate("JSON.stringify((window.__remoteDebug||{}).sceneStages||[])"))
            if "upload" in stages: break
        print("scene stages seen:", stages)
        ok &= len(stages) >= 4 and "parse" in stages and "upload" in stages
        # 进度条真实推进（宽度 > 0，非不确定动画）
        barw = cdp.evaluate("JSON.stringify(document.querySelector('.re-status .re-bar i').style.width)")
        print("bar width at upload:", barw)
        ok &= barw not in ('""', "null", None, "0.0%")
        wait(cdp, "spp"); wait(cdp, "128/128"); time.sleep(1)
        cdp.evaluate("window.__errs=[]; window.onerror=(m)=>window.__errs.push(String(m));")

        # ---- 2) ETA 行（完成后显示 完成）----
        cdp.evaluate("document.getElementById('re-btn-stats').click()")
        eta = json.loads(cdp.evaluate("JSON.stringify(document.getElementById('re-stat-eta').textContent)"))
        print("eta row:", eta)
        ok &= eta in ("完成", ) or eta.startswith("~")

        # ---- 3) A/B 对比：开关 + 分割线拖拽 ----
        cdp.evaluate("document.getElementById('re-btn-compare').click()")
        time.sleep(0.5)
        st1 = json.loads(cdp.evaluate("JSON.stringify((()=>{const i=[...document.querySelectorAll('img')];const ab=i.find(x=>x.className==='re-ab');const ov=i.find(x=>x.style.zIndex==='10');return {abHidden:ab?ab.hidden:null, clip:ov?ov.style.clipPath:null};})())"))
        print("ab on:", st1)
        ok &= st1["abHidden"] == False and "50%" in (st1["clip"] or "")
        # 拖拽 50% → 25%
        cdp.cmd("Input.dispatchMouseEvent", type="mousePressed", x=640, y=400, button="left", buttons=1, clickCount=1)
        for x in (560, 480, 400, 320):
            cdp.cmd("Input.dispatchMouseEvent", type="mouseMoved", x=x, y=400, button="left", buttons=1)
            time.sleep(0.05)
        cdp.cmd("Input.dispatchMouseEvent", type="mouseReleased", x=320, y=400, button="left", buttons=0, clickCount=1)
        time.sleep(0.5)
        clip2 = cdp.evaluate("JSON.stringify((()=>{const ov=[...document.querySelectorAll('img')].find(x=>x.style.zIndex==='10');return ov?ov.style.clipPath:null;})())")
        print("clip after drag:", clip2)
        ok &= "25%" in clip2 or "24" in clip2 or "26" in clip2
        # 关闭对比
        cdp.evaluate("document.getElementById('re-btn-compare').click()")
        time.sleep(0.3)
        clip3 = cdp.evaluate("JSON.stringify((()=>{const ov=[...document.querySelectorAll('img')].find(x=>x.style.zIndex==='10');return ov?ov.style.clipPath:'';})())")
        print("clip after off:", repr(clip3))
        ok &= clip3 in ('""', 'null') or clip3 == '""' or clip3 == ""

        errs = cdp.evaluate("JSON.stringify(window.__errs||[])")
        print("js errors:", errs)
        ok &= len(json.loads(errs)) == 0

        print("P2 UI ACCEPTANCE", "PASS" if ok else "FAIL")
        return 0 if ok else 1
    finally:
        for p in (chrome, httpd, server):
            p.terminate()
        for p in (chrome, httpd, server):
            try: p.wait(timeout=5)
            except Exception: p.kill()

if __name__ == "__main__":
    sys.exit(main())
