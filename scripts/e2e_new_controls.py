#!/usr/bin/env python3
"""新控制 E2E 冒烟（B1-B5）：FOV / 景深 / 画面(display-only) / 太阳 / 暂停。

sphere-test 场景（加载 ~1s）。等待一律轮询服务端 session 状态
（accumSamples / previewSequence），不用页面状态文本（会残留上一轮的
"128/128" 造成假就绪）。断言：
  1. 页面无 JS 错误（B1-B5 改动后整页语法/接线完好）
  2. FOV 滑条 → 服务端 session camera.fov 更新 + URL 同步
  3. 景深开关+光圈 → camera.coc/focus 落库
  4. bloom/饱和（display-only）→ session 值更新，累积样本数不重置（仍 128），
     previewSequence 前进（0-sample 再编码）
  5. 太阳方位 → session sun 更新，累积重置后重新收敛到 128
  6. 暂停按钮 → idleFired 计数不再增长；恢复后重新精修
"""
import json, os, subprocess, sys, tempfile, time, urllib.error, urllib.request
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
import e2e_cdp_helper as cdp_check
from e2e_cdp_helper import Cdp
cdp_check.CHROME_PORT = 9227
CHROME_PORT = 9227
RENDER_PORT = 9170
TOKEN = "devtoken"
PAGE = (f"http://127.0.0.1:8080/threejs/index.html"
        f"?remote=http://127.0.0.1:{RENDER_PORT}&model=sphere-test&dragMs=120&idleMs=400")

def http(path):
    r = urllib.request.Request(f"http://127.0.0.1:{RENDER_PORT}{path}",
                               headers={"Authorization": f"Bearer {TOKEN}"})
    with urllib.request.urlopen(r, timeout=30) as resp:
        return json.loads(resp.read())

def sess():
    return http("/api/sessions/s-1")

def wait_settled(seq0, timeout=150):
    """等待：seq 超过 seq0（至少一帧新预览）且累积回到 128。"""
    for _ in range(timeout):
        try:
            s = sess()
        except urllib.error.HTTPError:
            # 页面尚未建立 session（启动竞态，GET s-1 返回 404）→ 继续等
            time.sleep(1)
            continue
        if s["previewSequence"] > seq0 and s["accumSamples"] >= 128:
            return s
        time.sleep(1)
    return sess()

def main():
    server = subprocess.Popen(["./build/render_server", "127.0.0.1", str(RENDER_PORT)],
        cwd=REPO,
        env=dict(os.environ, RENDER_TOKEN=TOKEN),
        stdout=open(os.path.join(tempfile.gettempdir(), "nc_server.log"), "wb"),
        stderr=subprocess.STDOUT)
    httpd = subprocess.Popen([sys.executable, "-m", "http.server", "8080"],
        cwd=REPO,
        stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    subprocess.run(["rm", "-rf", "/tmp/wf_e2e/profile_nc"])
    chrome = subprocess.Popen(["google-chrome", "--headless=new", "--no-sandbox", "--disable-gpu",
        f"--remote-debugging-port={CHROME_PORT}", "--user-data-dir=/tmp/wf_e2e/profile_nc",
        "--window-size=1280,800", "about:blank"],
        stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    try:
        for _ in range(75):
            try:
                urllib.request.urlopen(f"http://127.0.0.1:{CHROME_PORT}/json/version", timeout=1); break
            except Exception: time.sleep(0.4)
        cdp = Cdp(); cdp.cmd("Page.enable")
        cdp.cmd("Page.navigate", url=PAGE)
        for _ in range(20):
            r = cdp.evaluate("JSON.stringify(!!document.body)")
            if r and json.loads(r): break
            time.sleep(0.5)
        cdp.evaluate("(window.__errs=[]),window.addEventListener('error',e=>window.__errs.push(e.message)),0")

        print("--- 0) initial refine to 128 ---")
        s = wait_settled(0, 150)
        print(f"  ready: accum={s['accumSamples']} fov={s['camera']['fov']}")
        assert s["accumSamples"] == 128, "initial refine"

        print("--- 1) FOV slider ->70 ---")
        seq0 = s["previewSequence"]
        cdp.evaluate("(function(){const s=document.getElementById('re-in-fov');"
                     "s.value='70';s.dispatchEvent(new Event('input'));"
                     "s.dispatchEvent(new Event('change'));})()")
        s = wait_settled(seq0)
        cam, url = s["camera"], cdp.evaluate("location.search") or ""
        print(f"  fov={cam['fov']} url_fov={'fov=70' in url}")
        assert abs(cam["fov"] - 70) < 1 and "fov=70" in url, "fov"

        print("--- 2) DOF on, coc=3 ---")
        seq0 = s["previewSequence"]
        cdp.evaluate("(function(){const c=document.getElementById('re-in-dof');"
                     "c.checked=true;c.dispatchEvent(new Event('change'));})()")
        cdp.evaluate("(function(){const s=document.getElementById('re-in-coc');"
                     "s.value='3';s.dispatchEvent(new Event('input'));"
                     "s.dispatchEvent(new Event('change'));})()")
        s = wait_settled(seq0)
        cam = s["camera"]
        print(f"  coc={cam['coc']:.1f} focus={cam['focus']:.2f}")
        assert abs(cam["coc"] - 3) < 0.2 and cam["focus"] > 0, "dof"

        print("--- 3) bloom + saturation (display-only: no reset) ---")
        seq0, accum0 = s["previewSequence"], s["accumSamples"]
        cdp.evaluate("(function(){const b=document.getElementById('re-in-bloom');"
                     "b.checked=true;b.dispatchEvent(new Event('change'));})()")
        time.sleep(2.5)  # 250ms debounce + 0-sample 再编码
        cdp.evaluate("(function(){const s=document.getElementById('re-in-sat');"
                     "s.value='1.5';s.dispatchEvent(new Event('input'));})()")
        time.sleep(2.5)
        s = sess()
        print(f"  bloom={s['bloom']} sat={s['saturationBoost']:.2f} "
              f"accum={s['accumSamples']} seq={s['previewSequence']}>(was {seq0})")
        assert s["bloom"] == 1 and abs(s["saturationBoost"] - 1.5) < 1e-3, "display values"
        assert s["accumSamples"] == 128, "display change must not reset accumulation"
        assert s["previewSequence"] > seq0, "re-encode must bump sequence"

        print("--- 4) sun az ->200 (reset + rerender) ---")
        seq0 = s["previewSequence"]
        cdp.evaluate("(function(){const s=document.getElementById('re-in-sunaz');"
                     "s.value='200';s.dispatchEvent(new Event('input'));"
                     "s.dispatchEvent(new Event('change'));})()")
        s = wait_settled(seq0)
        print(f"  sun={s['sun']} accum={s['accumSamples']}")
        assert s["sun"]["has"] and abs(s["sun"]["az"] - 200) < 1, "sun value"
        assert s["accumSamples"] >= 128, "sun rerender to budget"

        print("--- 5) pause blocks idle refine; resume restores ---")
        idle0 = json.loads(cdp.evaluate("JSON.stringify(window.__remoteDebug.idleFired)"))
        cdp.evaluate("document.getElementById('re-btn-pause').click()")
        icon = json.loads(cdp.evaluate(
            "JSON.stringify((()=>{const b=document.getElementById('re-btn-pause');"
            "return b.innerHTML.indexOf('M8 5l11 7-11 7')>=0;})())"))
        cdp.evaluate("(function(){const s=document.getElementById('re-in-fov');"
                     "s.value='75';s.dispatchEvent(new Event('input'));"
                     "s.dispatchEvent(new Event('change'));})()")
        time.sleep(5)
        idle1 = json.loads(cdp.evaluate("JSON.stringify(window.__remoteDebug.idleFired)"))
        print(f"  icon->play={icon} idleFired {idle0}->{idle1} (want equal)")
        assert icon, "pause icon"
        assert idle1 == idle0, "paused must not fire idle refine"
        cdp.evaluate("document.getElementById('re-btn-pause').click()")  # resume
        seq0 = sess()["previewSequence"]
        s = wait_settled(seq0)
        print(f"  resumed accum={s['accumSamples']}")

        errs = cdp.evaluate("JSON.stringify(window.__errs||[])")
        nerr = len(json.loads(errs)) if errs else 0
        print(f"  page errors: {nerr} {errs if nerr else ''}")
        assert nerr == 0, "page js errors"

        print("NEW CONTROLS E2E PASS")
        return 0
    finally:
        for p in (chrome, httpd, server):
            p.terminate()
        for p in (chrome, httpd, server):
            try: p.wait(timeout=5)
            except Exception: p.kill()

if __name__ == "__main__":
    sys.exit(main())
