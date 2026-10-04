#!/usr/bin/env python3
"""阶段五 E2E: drive the three.js remote-render page in headless Chrome via
CDP (stdlib only) — real pointer drag + wheel events must make the server
preview appear and progress in the overlay."""
import base64
import json
import os
import signal
import socket
import subprocess
import sys
import time
import urllib.request

CHROME_PORT = 9222
RENDER_PORT = 9151
TOKEN = os.environ.get("TOKEN", "devtoken")
PAGE = (f"http://127.0.0.1:8080/threejs/index.html"
        f"?remote=http://127.0.0.1:{RENDER_PORT}&model={os.environ.get('MODEL', 'sphere-test')}"
        f"&dragMs=150&idleMs=500&token={TOKEN}")
OUT = "/tmp/wf_e2e"


class Ws:
    def __init__(self, host, port, path):
        self.sock = socket.create_connection((host, port), timeout=10)
        key = base64.b64encode(os.urandom(16)).decode()
        self.sock.sendall((f"GET {path} HTTP/1.1\r\nHost: {host}:{port}\r\n"
                           "Upgrade: websocket\r\nConnection: Upgrade\r\n"
                           f"Sec-WebSocket-Key: {key}\r\nSec-WebSocket-Version: 13\r\n\r\n").encode())
        self.buf = b""
        while b"\r\n\r\n" not in self.buf:
            self.buf += self._raw(4096)
        head, self.buf = self.buf.split(b"\r\n\r\n", 1)
        assert b"101" in head.split(b"\r\n")[0], head

    def _raw(self, n):
        data = self.sock.recv(n)
        if not data:
            raise ConnectionError("closed")
        return data

    def _recv_more(self, n):
        while len(self.buf) < n:
            self.buf += self._raw(65536)

    def send_text(self, text):
        payload = text.encode()
        mask = os.urandom(4)
        header = bytes([0x81])
        n = len(payload)
        if n < 126:
            header += bytes([0x80 | n])
        elif n <= 0xFFFF:
            header += bytes([0x80 | 126]) + n.to_bytes(2, "big")
        else:
            header += bytes([0x80 | 127]) + n.to_bytes(8, "big")
        masked = bytes(b ^ mask[i % 4] for i, b in enumerate(payload))
        self.sock.sendall(header + mask + masked)

    def recv_message(self):
        self._recv_more(2)
        b0, b1 = self.buf[0], self.buf[1]
        op = b0 & 0x0F
        length, off = b1 & 0x7F, 2
        if length == 126:
            self._recv_more(4)
            length = int.from_bytes(self.buf[2:4], "big")
            off = 4
        elif length == 127:
            self._recv_more(10)
            length = int.from_bytes(self.buf[2:10], "big")
            off = 10
        self._recv_more(off + length)
        payload = self.buf[off:off + length]
        self.buf = self.buf[off + length:]
        return op, payload


class Cdp:
    def __init__(self):
        for _ in range(50):
            try:
                targets = json.load(urllib.request.urlopen(
                    f"http://127.0.0.1:{CHROME_PORT}/json", timeout=2))
                break
            except Exception:
                time.sleep(0.2)
        page = next(t for t in targets if t["type"] == "page")
        self.ws = Ws("127.0.0.1", CHROME_PORT,
                     page["webSocketDebuggerUrl"].split(f"{CHROME_PORT}", 1)[1])
        self.next_id = 1

    def cmd(self, method, **params):
        msg_id = self.next_id
        self.next_id += 1
        self.ws.send_text(json.dumps({"id": msg_id, "method": method, "params": params}))
        deadline = time.time() + 30
        while time.time() < deadline:
            op, payload = self.ws.recv_message()
            msg = json.loads(payload)
            if msg.get("id") == msg_id:
                if "error" in msg:
                    raise RuntimeError(f"{method}: {msg['error']}")
                return msg.get("result", {})
        raise TimeoutError(method)

    def evaluate(self, expression):
        result = self.cmd("Runtime.evaluate", expression=expression, returnByValue=True)
        return result.get("result", {}).get("value")

    def screenshot(self, path):
        data = self.cmd("Page.captureScreenshot", format="png")["data"]
        with open(path, "wb") as f:
            f.write(base64.b64decode(data))
        return path

    def drag(self, x0, y0, x1, y1, steps=12):
        self.cmd("Input.dispatchMouseEvent", type="mousePressed", x=x0, y=y0,
                 button="left", buttons=1, clickCount=1)
        for i in range(1, steps + 1):
            x = x0 + (x1 - x0) * i / steps
            y = y0 + (y1 - y0) * i / steps
            self.cmd("Input.dispatchMouseEvent", type="mouseMoved", x=int(x), y=int(y),
                     button="left", buttons=1)
            time.sleep(0.03)
        self.cmd("Input.dispatchMouseEvent", type="mouseReleased", x=x1, y=y1,
                 button="left", buttons=1)

    def wheel(self, x, y, dy):
        self.cmd("Input.dispatchMouseEvent", type="mouseWheel", x=x, y=y,
                 deltaX=0, deltaY=dy)


def main():
    profile = os.path.join(OUT, "profile")
    chrome = subprocess.Popen([
        "google-chrome", "--headless=new", "--no-sandbox", "--disable-gpu",
        f"--remote-debugging-port={CHROME_PORT}", f"--user-data-dir={profile}",
        "--window-size=1280,800", "about:blank"],
        stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    try:
        cdp = Cdp()
        cdp.cmd("Page.enable")
        cdp.cmd("Page.navigate", url=PAGE)
        print("page opened, waiting for session-ready (model load)...")
        readystate = None
        for poll in range(240):
            time.sleep(2)
            if poll % 5 == 0:
                print(f"  [wait {poll*2}s]", flush=True)
            readystate = json.loads(cdp.evaluate(
                "JSON.stringify((() => {"
                "  const st = [...document.querySelectorAll('div')]"
                "    .map(d => d.textContent).find(t => t && t.startsWith('remote:'));"
                "  return st || '';"
                "})())"))
            if readystate and ("ready" in readystate or "spp" in readystate) \
                    and "loading" not in readystate and "connecting" not in readystate:
                break
        print("status:", readystate)
        time.sleep(2)

        state = cdp.evaluate(
            "JSON.stringify((() => {"
            "  const divs = [...document.querySelectorAll('div')];"
            "  const st = divs.map(d => d.textContent).find(t => t && t.startsWith('remote:'));"
            "  const img = [...document.querySelectorAll('img')].find(i => i.style.zIndex === '10');"
            "  return { status: st, overlayHidden: img ? img.hidden : null,"
            "           overlaySrc: img ? img.src : null,"
            "           naturalWidth: img ? img.naturalWidth : null };"
            "})())")
        print("state after load:", state)
        state = json.loads(state)
        assert state["status"], "status element missing"
        cdp.screenshot(f"{OUT}/e2e_1_loaded.png")

        print("--- drag across the canvas ---")
        cdp.drag(640, 400, 900, 300)
        # 等待 overlay 图片真正加载（src 随每个 preview 事件更新，加载有延迟）
        state1 = {}
        for _ in range(60):
            state1 = json.loads(cdp.evaluate(
                "JSON.stringify((() => {"
                "  const divs = [...document.querySelectorAll('div')];"
                "  const st = divs.map(d => d.textContent).find(t => t && t.startsWith('remote:'));"
                "  const img = [...document.querySelectorAll('img')].find(i => i.style.zIndex === '10');"
                "  return { status: st, overlayHidden: img ? img.hidden : null,"
                "           overlaySrc: img ? img.src : null,"
                "           naturalWidth: img ? img.naturalWidth : null };"
                "})())"))
            if (state1.get("overlayHidden") is False
                    and state1.get("naturalWidth") and state1["naturalWidth"] > 0):
                break
            time.sleep(0.3)
        print("state after drag:", state1)
        assert state1.get("overlayHidden") is False, "overlay never shown after drag"
        assert state1.get("naturalWidth") and state1["naturalWidth"] > 0, (
            "overlay image not actually loaded (broken src)", state1)
        assert "/api/renders/" in state1["overlaySrc"], state1["overlaySrc"]
        cdp.screenshot(f"{OUT}/e2e_2_drag.png")

        print("--- wheel zoom, then idle progressive chain ---")
        for _ in range(3):
            cdp.wheel(640, 400, -120)
            time.sleep(0.15)
        time.sleep(6)
        state2 = json.loads(cdp.evaluate(
            "JSON.stringify((() => {"
            "  const divs = [...document.querySelectorAll('div')];"
            "  const st = divs.map(d => d.textContent).find(t => t && t.startsWith('remote:'));"
            "  return { status: st };"
            "})())"))
        print("state after idle:", state2)
        assert "spp" in state2["status"], state2
        cdp.screenshot(f"{OUT}/e2e_3_idle.png")

        req = urllib.request.Request(
            f"http://127.0.0.1:{RENDER_PORT}/api/sessions/s-1",
            headers={"Authorization": f"Bearer {TOKEN}"})
        meta = json.load(urllib.request.urlopen(req, timeout=5))
        print("server session:", json.dumps(meta))
        assert meta["previewSequence"] >= 3, meta["previewSequence"]
        assert meta["accumSamples"] > 0
        img1 = open(f"{OUT}/e2e_2_drag.png", "rb").read()
        img2 = open(f"{OUT}/e2e_3_idle.png", "rb").read()
        assert img1 != img2, "screenshots identical — screen did not update"
        print("ALL PHASE-5 E2E CHECKS PASS")
        return 0
    finally:
        chrome.send_signal(signal.SIGTERM)
        try:
            chrome.wait(timeout=5)
        except subprocess.TimeoutExpired:
            chrome.kill()


if __name__ == "__main__":
    sys.exit(main())
