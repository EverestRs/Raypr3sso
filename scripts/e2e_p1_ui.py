#!/usr/bin/env python3
"""P1 UI acceptance: stats panel, keyboard (1-4/H), scene switch, URL sync,
error card on dead server."""
import json, os, subprocess, sys, tempfile, time, urllib.request
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import e2e_cdp_helper as cdp_check
from e2e_cdp_helper import Cdp
cdp_check.CHROME_PORT = 9230
CHROME_PORT = 9230
RENDER_PORT = 9170
TOKEN = "devtoken"
PAGE = (f"http://127.0.0.1:8080/threejs/index.html"
        f"?remote=http://127.0.0.1:{RENDER_PORT}&model=bistro&dragMs=120&idleMs=400")
DEAD_PAGE = (f"http://127.0.0.1:8080/threejs/index.html"
             f"?remote=http://127.0.0.1:9199&model=bistro")

def http(path):
    r = urllib.request.Request(f"http://127.0.0.1:{RENDER_PORT}{path}",
                               headers={"Authorization": f"Bearer {TOKEN}"})
    with urllib.request.urlopen(r, timeout=30) as resp:
        return json.loads(resp.read())

def status(cdp):
    return cdp.evaluate("JSON.stringify((()=>{const d=[...document.querySelectorAll('div')].map(d=>d.textContent).find(t=>t&&t.startsWith('remote:'));return d||''})())")

def wait_state(cdp, needle, timeout=120):
    for _ in range(timeout):
        st = status(cdp)
        if needle in st: return st
        time.sleep(1)
    return st

def overlay_w(cdp):
    return json.loads(cdp.evaluate("JSON.stringify((()=>{const i=[...document.querySelectorAll('img')].find(i=>i.style.zIndex==='10');return i?i.naturalWidth:0})())"))

def key(cdp, k):
    cdp.evaluate(f"window.dispatchEvent(new KeyboardEvent('keydown', {{key:'{k}'}}))")

def main():
    server = subprocess.Popen(["./build/render_server","127.0.0.1",str(RENDER_PORT)],
        cwd="/home/biwenbin/VSCodeProjects/RenderEngine",
        env=dict(os.environ, RENDER_TOKEN=TOKEN, RENDER_OIDN="1"),
        stdout=open(os.path.join(tempfile.gettempdir(), "p1_server.log"),"wb"), stderr=subprocess.STDOUT)
    httpd = subprocess.Popen([sys.executable,"-m","http.server","8080"],
        cwd="/home/biwenbin/VSCodeProjects/RenderEngine",
        stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    subprocess.run(["rm","-rf",os.path.join(tempfile.gettempdir(), "profile_p1")])
    chrome = subprocess.Popen(["google-chrome","--headless=new","--no-sandbox","--disable-gpu",
        f"--remote-debugging-port={CHROME_PORT}","--user-data-dir=/tmp/wf_e2e/profile_p1",
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
            st = status(cdp)
            if "spp" in st: break
        wait_state(cdp, "128/128"); time.sleep(1)
        cdp.evaluate("window.__errs=[]; window.onerror=(m)=>window.__errs.push(String(m))")
        sid = "s-1"
        preset = http(f"/api/sessions/{sid}")["camera"]["position"]

        # ---- 1) 统计面板 ----
        cdp.evaluate("document.getElementById('re-btn-stats').click()")
        vis = cdp.evaluate("JSON.stringify(!document.querySelector('.re-panel:nth-of-type(1)')) ")
        sp = json.loads(cdp.evaluate("JSON.stringify((()=>{const g=id=>{const e=document.getElementById(id);return e?e.textContent:null};"
                          "return {model:g('re-stat-model'),res:g('re-stat-res'),spp:g('re-stat-spp'),"
                          "cad:g('re-stat-cad'),exp:g('re-stat-exp')};})())"))
        print("stats:", sp)
        ok &= sp["model"] == "bistro" and "960" in sp["res"] and "128" in sp["spp"]

        # ---- 2) 键盘 1 → 640；2 → 960；h 隐藏 UI ----
        key(cdp, "1")
        w = 0
        for _ in range(60):
            time.sleep(1); w = overlay_w(cdp)
            if w == 640: break
        print("key 1: overlayW =", w)
        ok &= w == 640
        key(cdp, "2")
        for _ in range(60):
            time.sleep(1); w = overlay_w(cdp)
            if w == 960: break
        print("key 2: overlayW =", w)
        ok &= w == 960
        key(cdp, "h")
        hid = json.loads(cdp.evaluate("JSON.stringify(getComputedStyle(document.querySelector('.re-title')).display)"))
        key(cdp, "h")
        vis2 = json.loads(cdp.evaluate("JSON.stringify(getComputedStyle(document.querySelector('.re-title')).display)"))
        print(f"h key: hidden display={hid} -> restored {vis2}")
        ok &= hid == "none" and vis2 != "none"

        # ---- 3) URL 同步（画质档已在 URL；曝光滑条 → exposure=）----
        url1 = cdp.evaluate("JSON.stringify(location.search)")
        ok &= "idleRes=960x540" in url1
        cdp.evaluate("(function(){const s=document.querySelector('.re-panel input[type=range]');s.value='4.0';s.dispatchEvent(new Event('input'));})()")
        time.sleep(1.5)
        url2 = cdp.evaluate("JSON.stringify(location.search)")
        print("url:", url2[:120])
        ok &= "exposure=4" in url2

        # ---- 4) 场景切换 → sphere-test（1s 加载）----
        cdp.evaluate("document.getElementById('re-btn-settings').click()")
        cdp.evaluate("(function(){const b=[...document.querySelectorAll('#re-scene button')].find(b=>b.dataset.model==='sphere-test');b.click();})()")
        wait_state(cdp, "sphere-test", 120)
        time.sleep(2)
        url3 = cdp.evaluate("JSON.stringify(location.search)")
        smodel = http("/api/sessions/s-2")["model"] if "s-2" else "?"
        print("scene switch: url model=", ("model=sphere-test" in url3), "server session model=", smodel)
        ok &= "model=sphere-test" in url3 and smodel == "sphere-test"
        wait_state(cdp, "done", 120)
        # R 回位在球场景也工作
        cdp.evaluate("(function(){const b=[...document.querySelectorAll('button.re-btn')].find(b=>b.id==='re-btn-home');b.click();})()")
        time.sleep(3)
        p2 = http("/api/sessions/s-2")["camera"]["position"]
        print("sphere home cam:", [round(v,2) for v in p2])
        ok &= abs(p2[0] - 2.5) < 0.05 and abs(p2[1] - 1.2) < 0.05

        # ---- 5) 错误卡片（死端口页面）----
        cdp.cmd("Page.navigate", url=DEAD_PAGE)
        time.sleep(5)
        card = cdp.evaluate("JSON.stringify((()=>{const c=document.querySelector('.re-err');return {hidden:c?c.hidden:null, txt:c?(c.querySelector('.card b')||{}).textContent:null};})())")
        print("error card (dead port):", card)
        ok &= '"hidden":false' in card and "无法连接" in card

        errs = cdp.evaluate("JSON.stringify(window.__errs||[])")
        print("js errors:", errs)
        ok &= len(json.loads(errs)) == 0

        print("P1 UI ACCEPTANCE", "PASS" if ok else "FAIL")
        return 0 if ok else 1
    finally:
        for p in (chrome, httpd, server):
            p.terminate()
        for p in (chrome, httpd, server):
            try: p.wait(timeout=5)
            except Exception: p.kill()

if __name__ == "__main__":
    sys.exit(main())
