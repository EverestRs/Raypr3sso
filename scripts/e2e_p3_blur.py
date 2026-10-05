#!/usr/bin/env python3
"""Verify blur-to-sharp interaction: title, blur on move start, drag-tier
denoised progressive frames, blur 0 after refine."""
import json, os, subprocess, sys, time, urllib.request
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import e2e_cdp_helper as cdp_check
from e2e_cdp_helper import Cdp
cdp_check.CHROME_PORT = 9232
CHROME_PORT = 9232
RENDER_PORT = 9151   # 复用已运行的远程服务端（含已加载的 bistro）
TOKEN = "devtoken"
PAGE = f"http://127.0.0.1:8080/threejs/index.html?model=bistro&token={TOKEN}"

def status(cdp):
    return cdp.evaluate("JSON.stringify((()=>{const d=[...document.querySelectorAll('div')].map(d=>d.textContent).find(t=>t&&t.startsWith('remote:'));return d||''})())")

def filter_now(cdp):
    return cdp.evaluate("JSON.stringify(document.querySelector('img[style*=z-index]').style.filter || '(none)')")

def main():
    httpd = subprocess.Popen([sys.executable,"-m","http.server","8080"],
        cwd="/home/biwenbin/VSCodeProjects/RenderEngine",
        stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    subprocess.run(["rm","-rf","/tmp/wf_e2e/profile_p3"])
    chrome = subprocess.Popen(["google-chrome","--headless=new","--no-sandbox","--disable-gpu",
        f"--remote-debugging-port={CHROME_PORT}","--user-data-dir=/tmp/wf_e2e/profile_p3",
        "--window-size=1280,800","about:blank"], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    ok = True
    try:
        for _ in range(75):
            try:
                urllib.request.urlopen(f"http://127.0.0.1:{CHROME_PORT}/json/version", timeout=1); break
            except Exception: time.sleep(0.4)
        cdp = Cdp(); cdp.cmd("Page.enable")
        cdp.evaluate("window.__errs=[]; window.onerror=(m)=>window.__errs.push(String(m));")
        cdp.cmd("Page.navigate", url=PAGE)
        time.sleep(2)
        title = cdp.evaluate("JSON.stringify(document.title)")
        print("title:", title)
        ok &= "RenderEngine" in title
        for _ in range(240):
            time.sleep(1)
            st = status(cdp)
            if "128/128" in st or "done" in st: break
        time.sleep(1)
        f0 = filter_now(cdp)
        print("after refine filter:", f0)
        ok &= f0 in ('"blur(0.0px)"', '"blur(0px)"', '""') and "0.0" not in f0.replace("blur(0.0px)","")

        # 移动开始：立即模糊（旧帧糊化，不黑屏）
        cdp.cmd("Input.dispatchMouseEvent", type="mousePressed", x=640, y=400, button="left", buttons=1, clickCount=1)
        time.sleep(0.25)
        f1 = filter_now(cdp)
        print("filter on move start:", f1)
        ok &= "blur" in f1
        # 持续拖动：松开前保持模糊的快帧
        cdp.cmd("Input.dispatchMouseEvent", type="mouseMoved", x=560, y=380, button="left", buttons=1)
        time.sleep(0.4)
        f2 = filter_now(cdp)
        print("filter while dragging:", f2)
        cdp.cmd("Input.dispatchMouseEvent", type="mouseReleased", x=560, y=380, button="left", buttons=0, clickCount=1)
        # 停手 → 精修 → 清晰
        blurred_seen = "blur" in f2
        for _ in range(90):
            time.sleep(1)
            f3 = filter_now(cdp)
            if "blur(0.0px)" in f3 or f3 == '""': break
        print("filter after refine:", f3)
        ok &= blurred_seen and ("blur(0px)" in f3 or "blur(0.0px)" in f3)

        errs = cdp.evaluate("JSON.stringify(window.__errs||[])")
        print("js errors:", errs)
        ok &= len(json.loads(errs)) == 0
        print("BLUR-TO-SHARP ACCEPTANCE", "PASS" if ok else "FAIL")
        return 0 if ok else 1
    finally:
        for p in (chrome, httpd):
            p.terminate()
        for p in (chrome, httpd):
            try: p.wait(timeout=5)
            except Exception: p.kill()

if __name__ == "__main__":
    sys.exit(main())
