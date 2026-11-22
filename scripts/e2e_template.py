#!/usr/bin/env python3
"""e2e 模板 —— 复制本文件改造成新测试。合规约定（勿破坏）：
  1. 仓库根从 __file__ 推导，绝不写 /home/<user> 绝对路径；
  2. token 用一次性占位符（仅绑 127.0.0.1，非真实凭据）；
  3. 输出一律进 tempfile 目录或 /tmp/<无用户名前缀>；
  4. server/浏览器进程在 finally 里 terminate + wait + kill 兜底；
  5. 端口取 9180-9199 未占用段，跑前确认没有实例占用。
浏览器交互类测试请参考 e2e_new_controls.py（cdp_helper + Cdp 的用法），
纯 API 验收类（位置感知 rig 那种）照下面的骨架扩展即可。"""
import json, os, subprocess, sys, tempfile, time, urllib.error, urllib.request

# ---- 配置区（每个测试改这里）----
REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))  # 仓库根，勿硬编码绝对路径
BIN = os.path.join(REPO, "build", "render_server")
TOKEN = "devtoken"                       # 本地回环一次性占位 token
PORT = 9181                              # 避开其它 e2e 脚本的端口
MODEL = "sphere-test"                    # 快场景优先；bistro 首帧约 1-2 分钟
OUTDIR = tempfile.mkdtemp(prefix="e2e_template_")

def http(method, path, body=None, binary=False, timeout=180):
    """带 Bearer 的 API 请求；返回 JSON（binary=True 时返回原始字节）。"""
    data = json.dumps(body).encode() if body is not None else None
    req = urllib.request.Request(
        f"http://127.0.0.1:{PORT}{path}", data=data, method=method,
        headers={"Authorization": f"Bearer {TOKEN}",
                 "Content-Type": "application/json"})
    with urllib.request.urlopen(req, timeout=timeout) as resp:
        raw = resp.read()
        return raw if binary else json.loads(raw.decode())

def wait_server(timeout=30):
    for _ in range(int(timeout / 0.3)):
        try:
            http("GET", "/api/health"); return
        except Exception:
            time.sleep(0.3)
    raise RuntimeError("server 启动超时")

def wait_state(path, want, timeout=600, step=2):
    """轮询 /api/renders/<rid> 或 /api/sessions/<sid> 直到 state == want。"""
    for _ in range(int(timeout / step)):
        st = http("GET", path)
        if st.get("state") == want: return st
        if st.get("state") in ("failed", "cancelled"): raise RuntimeError(st)
        time.sleep(step)
    raise RuntimeError(f"等待 {want} 超时: {path}")

def render(sid, name, cam, **opts):
    """提交一次渲染并等待完成，图片落到 OUTDIR/<name>.png。"""
    body = {"width": 960, "height": 540, "sppBudget": 64, "camera": cam, **opts}
    rid = http("POST", f"/api/sessions/{sid}/render", body)["renderId"]
    wait_state(f"/api/renders/{rid}", "completed")
    for _ in range(30):   # completed 后 /image 可能仍在编码队列里，容 404 重试
        try:
            png = http("GET", f"/api/renders/{rid}/image", binary=True); break
        except urllib.error.HTTPError as e:
            if e.code != 404: raise
            time.sleep(1)
    open(os.path.join(OUTDIR, f"{name}.png"), "wb").write(png)
    print(f"[e2e] {name} -> {rid}", flush=True)
    return png

def main():
    ok = True
    log = open(os.path.join(OUTDIR, "server.log"), "wb")
    env = dict(os.environ, RENDER_TOKEN=TOKEN)   # 需要 OIDN 终帧再加 RENDER_OIDN="1"
    srv = subprocess.Popen([BIN, "127.0.0.1", str(PORT)], cwd=REPO, env=env,
                           stdout=log, stderr=subprocess.STDOUT)
    try:
        wait_server()
        sess = http("POST", "/api/sessions", {"model": MODEL})
        sid = sess["sessionId"]
        wait_state(f"/api/sessions/{sid}", "ready", timeout=900)

        # ======== 在这里写测试步骤 ========
        # 注意：POST /api/sessions 的响应没有 camera，须从 ready 后的 GET 取
        cam = http("GET", f"/api/sessions/{sid}")["camera"]
        render(sid, "baseline", cam)
        # TODO: 断言（参考 e2e_position_profiles.py 的 STATS 输出、
        #       e2e_new_controls.py 的 session 字段断言）
        # ================================
    except Exception as e:
        ok = False
        print(f"[e2e] FAIL: {e}", flush=True)
    finally:
        srv.terminate()
        try: srv.wait(10)
        except Exception: srv.kill()
    print("E2E TEMPLATE PASS" if ok else "E2E TEMPLATE FAIL", flush=True)
    print(f"产物目录: {OUTDIR}", flush=True)
    return 0 if ok else 1

if __name__ == "__main__":
    sys.exit(main())
