#!/usr/bin/env python3
# Refine-chain latency benchmark: first-preview latency, per-chunk cadence,
# total refine duration, and payload size. The before/after yardstick for the
# browser real-time speed work.
#
# Usage: bench_refine.py [port] [server_binary] [--model=sphere-test|bistro] [--spp=128]
import base64
import json
import os
import socket
import statistics
import subprocess
import sys
import tempfile
import time
import urllib.request

TOKEN = "benchtoken"
PORT = int(sys.argv[1]) if len(sys.argv) > 1 else 9155
BIN = sys.argv[2] if len(sys.argv) > 2 else "./build/render_server"
MODEL = "sphere-test"
SPP = 128
for a in sys.argv[3:]:
    if a.startswith("--model="):
        MODEL = a.split("=", 1)[1]
    elif a.startswith("--spp="):
        SPP = int(a.split("=", 1)[1])


class Ws:
    """Minimal RFC6455 client — the server only sends unmasked text frames."""

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

    def recv_text(self, timeout=300):
        self.sock.settimeout(timeout)
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
        return payload.decode(errors="replace")


def req(method, path, body=None, timeout=60):
    r = urllib.request.Request(
        f"http://127.0.0.1:{PORT}{path}", method=method,
        data=json.dumps(body).encode() if body is not None else None,
        headers={"Authorization": f"Bearer {TOKEN}",
                 "Content-Type": "application/json"})
    with urllib.request.urlopen(r, timeout=timeout) as resp:
        return json.loads(resp.read())


def main():
    dump = tempfile.mkdtemp(prefix="bench_")
    env = {k: v for k, v in os.environ.items() if not k.startswith("RENDER_")}
    env["RENDER_TOKEN"] = TOKEN
    env["RENDER_PERF"] = "1"
    log = open(os.path.join(dump, "server.log"), "wb")
    srv = subprocess.Popen([BIN, "127.0.0.1", str(PORT)], env=env,
                           stdout=log, stderr=log)
    try:
        for _ in range(100):
            try:
                req("GET", "/api/health")
                break
            except Exception:
                time.sleep(0.2)
        t0 = time.time()
        sid = req("POST", "/api/sessions", {"model": MODEL})["sessionId"]
        for _ in range(1800):
            if req("GET", f"/api/sessions/{sid}")["state"] == "ready":
                break
            time.sleep(1)
        else:
            raise SystemExit("scene load timeout")
        print(f"scene load: {time.time()-t0:.0f}s")

        cam = {"position": [2.5, 1.2, 2.0], "direction": [-0.7, -0.35, -0.61],
               "up": [0, 1, 0], "fov": 35}
        if MODEL.startswith("bistro"):
            cam = {"position": [-0.833, 3.099, -1.471],
                   "direction": [0.993846, -0.104828, -0.0358017],
                   "up": [0.104761, 0.99449, -0.00377383], "fov": 40.5}
        req("POST", f"/api/sessions/{sid}/camera", cam)

        ws = Ws("127.0.0.1", PORT,
                f"/api/sessions/{sid}/events?token={TOKEN}")
        base = {"width": 640, "height": 360, "sppBudget": SPP,
                "maxDepth": 6, "neeK": 2, "denoise": 1}
        # 两种模式对比：old = 固定 8spp chunk + 每 chunk 完整降噪（旧行为，
        # 预览格式已 JPEG 化）；new = progressive 几何 chunk + quick/OIDN 分档。
        runs = [("old(fixed8,full)", dict(base, chunkSpp=8)),
                ("new(progressive)", dict(base, progressive=1,
                                          adaptiveMin=16, adaptiveCheck=8,
                                          adaptiveThresh=0.02))]
        for label, body in runs:
            req("POST", f"/api/sessions/{sid}/camera", cam)
            t0 = time.time()
            r = req("POST", f"/api/sessions/{sid}/render", body)
            previews = []
            total = None
            while True:
                msg = json.loads(ws.recv_text())
                if msg.get("renderId") != r["renderId"]:
                    continue
                t = time.time() - t0
                if msg["type"] == "preview-ready":
                    previews.append((t, msg["sequence"], msg["sample"]))
                if msg["type"] in ("render-completed", "render-failed",
                                   "render-canceled"):
                    total = t
                    break
            rq = urllib.request.Request(
                f"http://127.0.0.1:{PORT}/api/renders/{r['renderId']}/image",
                headers={"Authorization": f"Bearer {TOKEN}"})
            with urllib.request.urlopen(rq, timeout=60) as resp:
                final_bytes = len(resp.read())
                final_type = resp.headers.get("Content-Type", "?")

            first = previews[0][0] if previews else float("nan")
            intervals = [previews[i][0] - previews[i - 1][0]
                         for i in range(1, len(previews))]
            med = statistics.median(intervals) if intervals else float("nan")
            print(f"--- {label} ---")
            print(f"first preview : {first*1000:7.0f} ms")
            print(f"total refine  : {total*1000:7.0f} ms  ({len(previews)} previews)")
            if intervals:
                print(f"chunk cadence : median {med*1000:7.0f} ms  "
                      f"min {min(intervals)*1000:.0f} max {max(intervals)*1000:.0f}")
            print(f"final image   : {final_bytes} bytes ({final_type})")
            print(f"preview times : {[f'{t:.2f}s@{s}' for t, _, s in previews]}")
        return 0
    finally:
        srv.terminate()
        try:
            srv.wait(timeout=5)
        except Exception:
            srv.kill()
        print(f"log: {dump}/server.log")


if __name__ == "__main__":
    sys.exit(main())
