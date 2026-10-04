#!/usr/bin/env python3
"""阶段四 acceptance: /api/renders/* routes + WebSocket event pathway (design 8).

Boots render_server, then with a stdlib-only WebSocket client verifies:
  * upgrade rejected on wrong path / unknown session (404, no 101)
  * session-ready snapshot on connect (and again on reconnect)
  * full event chain: render-started -> progress/preview-ready per chunk
    (monotonic sample + sequence) -> render-completed
  * GET /api/renders/{id} status shape, /preview and /image PNG bytes
  * cancel is idempotent (completed render cancel x2 keeps state completed)
  * cancel mid-chain stops the chain and emits render-canceled
"""
import base64
import json
import os
import socket
import subprocess
import sys
import tempfile
import time
import urllib.error
import urllib.request

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
PORT = int(os.environ.get("PORT", "9141"))
BASE = f"http://127.0.0.1:{PORT}"
TOKEN = os.environ.get("TOKEN", "devtoken")
PNG_MAGIC = b"\x89PNG\r\n\x1a\n"


def http(method, path, body=None):
    headers = {"Content-Type": "application/json"} if body is not None else {}
    headers["Authorization"] = f"Bearer {TOKEN}"
    req = urllib.request.Request(
        BASE + path, method=method,
        data=json.dumps(body).encode() if body is not None else None,
        headers=headers)
    try:
        with urllib.request.urlopen(req, timeout=15) as resp:
            return resp.status, resp.read()
    except urllib.error.HTTPError as err:
        return err.code, err.read()


class WsClient:
    def __init__(self, port, path):
        self.sock = socket.create_connection(("127.0.0.1", port), timeout=5)
        key = base64.b64encode(os.urandom(16)).decode()
        sep = "&" if "?" in path else "?"
        request = (f"GET {path}{sep}token={TOKEN} HTTP/1.1\r\nHost: 127.0.0.1:{port}\r\n"
                   "Upgrade: websocket\r\nConnection: Upgrade\r\n"
                   f"Sec-WebSocket-Key: {key}\r\nSec-WebSocket-Version: 13\r\n\r\n")
        self.sock.sendall(request.encode())
        self.buf = b""
        while b"\r\n\r\n" not in self.buf:
            chunk = self.sock.recv(4096)
            if not chunk:
                raise RuntimeError("closed during handshake")
            self.buf += chunk
        head, self.buf = self.buf.split(b"\r\n\r\n", 1)
        self.status_line = head.split(b"\r\n")[0].decode()

    def _recv_more(self, n, timeout):
        self.sock.settimeout(timeout)
        while len(self.buf) < n:
            chunk = self.sock.recv(65536)
            if not chunk:
                raise ConnectionError("closed")
            self.buf += chunk

    def recv_frame(self, timeout=15):
        self._recv_more(2, timeout)
        b0, b1 = self.buf[0], self.buf[1]
        length, off = b1 & 0x7F, 2
        if length == 126:
            self._recv_more(4, timeout)
            length = int.from_bytes(self.buf[2:4], "big")
            off = 4
        elif length == 127:
            self._recv_more(10, timeout)
            length = int.from_bytes(self.buf[2:10], "big")
            off = 10
        self._recv_more(off + length, timeout)
        payload = self.buf[off:off + length]
        self.buf = self.buf[off + length:]
        return b0 & 0x0F, payload

    def recv_json(self, timeout=15):
        opcode, payload = self.recv_frame(timeout)
        return opcode, json.loads(payload.decode())

    def close(self):
        try:
            self.sock.sendall(bytes([0x88, 0x80]) + os.urandom(4))
        except OSError:
            pass
        self.sock.close()


def main():
    dump = tempfile.mkdtemp(prefix="wf_events_")
    log = open(os.path.join(dump, "server.log"), "w")
    server = subprocess.Popen([os.path.join(ROOT, "build", "render_server"),
                               "127.0.0.1", str(PORT)],
                              stdout=log, stderr=log, cwd=ROOT,
                              env=dict(os.environ, RENDER_TOKEN=TOKEN))
    kept = {"dir": dump, "live": True}
    try:
        for _ in range(50):
            try:
                if http("GET", "/api/health")[0] == 200:
                    break
            except (urllib.error.URLError, socket.timeout):
                pass
            time.sleep(0.2)

        print("--- upgrade rejected: unknown session ---")
        ws = WsClient(PORT, "/api/sessions/s-999/events")
        assert "404" in ws.status_line, ws.status_line
        ws.sock.close()
        print(f"  {ws.status_line.split(' ', 1)[1]} OK")

        print("--- upgrade rejected: wrong suffix path ---")
        _, body = http("POST", "/api/sessions", {"model": "sphere-test", "preset": "ball"})
        sid = json.loads(body)["sessionId"]
        for _ in range(300):
            state = json.loads(http("GET", f"/api/sessions/{sid}")[1])["state"]
            if state == "ready":
                break
            assert state != "failed", "sphere load failed"
            time.sleep(0.3)
        ws = WsClient(PORT, f"/api/sessions/{sid}/camera")   # not /events
        assert "404" in ws.status_line, ws.status_line
        ws.sock.close()
        print("  404 on non-/events path OK")

        print("--- session-ready snapshot on connect ---")
        ws = WsClient(PORT, f"/api/sessions/{sid}/events")
        assert "101" in ws.status_line, ws.status_line
        _, event = ws.recv_json()
        assert event["type"] == "session-ready" and event["sessionId"] == sid, event
        print(f"  {event} OK")

        print("--- event chain over a 64-spp / 8-spp chunk render ---")
        _, body = http("POST", f"/api/sessions/{sid}/render",
                       {"width": 320, "height": 180, "sppBudget": 64,
                        "chunkSpp": 8, "maxDepth": 4})
        result = json.loads(body)
        assert result["accepted"] is True
        rid = result["renderId"]
        events = []
        while True:
            _, event = ws.recv_json(timeout=20)
            events.append(event)
            if event["type"] in ("render-completed", "render-canceled", "render-failed"):
                break
        assert events[0]["type"] == "render-started" and events[0]["renderId"] == rid, events[0]
        progress = [e for e in events if e["type"] == "progress"]
        assert [e["sample"] for e in progress] == [8, 16, 24, 32, 40, 48, 56, 64], \
            [e["sample"] for e in progress]
        sequences = [e["sequence"] for e in progress]
        assert sequences == sorted(set(sequences)), sequences
        previews = [e for e in events if e["type"] == "preview-ready"]
        assert len(previews) == 8 and all("previewUrl" in e for e in previews)
        assert events[-1]["type"] == "render-completed" and events[-1]["sample"] == 64
        assert events[-1]["generation"] == result["generation"]
        print(f"  events: {len(events)} (started + 8 progress + 8 preview-ready + completed) OK")

        print("--- render status / preview / image ---")
        status, body = http("GET", f"/api/renders/{rid}")
        info = json.loads(body)
        assert info["state"] == "completed" and info["sample"] == 64, info
        assert info["sampleBudget"] == 64 and info["sessionId"] == sid
        assert info["previewUrl"].startswith(f"/api/renders/{rid}/preview?seq=")
        status, png = http("GET", f"/api/renders/{rid}/preview")
        assert status == 200 and png[:8] == PNG_MAGIC and len(png) > 1000
        status, img = http("GET", f"/api/renders/{rid}/image")
        assert status == 200 and img[:8] == PNG_MAGIC and img == png
        print(f"  status completed, preview {len(png)}B == image {len(img)}B OK")

        print("--- cancel is idempotent on a completed render ---")
        status1, _ = http("POST", f"/api/renders/{rid}/cancel")
        status2, _ = http("POST", f"/api/renders/{rid}/cancel")
        assert status1 == 200 and status2 == 200
        state = json.loads(http("GET", f"/api/renders/{rid}")[1])["state"]
        assert state == "completed", state
        print("  cancel x2 on completed -> still completed OK")

        print("--- cancel mid-chain stops the chain ---")
        _, body = http("POST", f"/api/sessions/{sid}/render",
                       {"width": 320, "height": 180, "sppBudget": 4096,
                        "chunkSpp": 8, "maxDepth": 4})
        rid2 = json.loads(body)["renderId"]
        time.sleep(1.0)   # let a few chunks run
        assert http("POST", f"/api/renders/{rid2}/cancel")[0] == 200
        assert http("POST", f"/api/renders/{rid2}/cancel")[0] == 200
        while True:
            _, event = ws.recv_json(timeout=20)
            if event.get("renderId") == rid2 and event["type"] == "render-canceled":
                break
        state = json.loads(http("GET", f"/api/renders/{rid2}")[1])["state"]
        assert state == "canceled", state
        sample = json.loads(http("GET", f"/api/renders/{rid2}")[1])["sample"]
        assert sample < 4096, sample
        print(f"  canceled at sample {sample} (< 4096) OK")

        print("--- reconnect delivers a fresh session-ready snapshot ---")
        ws.close()
        ws = WsClient(PORT, f"/api/sessions/{sid}/events")
        assert "101" in ws.status_line, ws.status_line
        _, event = ws.recv_json()
        assert event["type"] == "session-ready", event
        ws.close()
        print("  reconnect OK")

        print("ALL PHASE-4 CHECKS PASS")
        kept["live"] = False
    finally:
        server.terminate()
        try:
            server.wait(timeout=10)
        except subprocess.TimeoutExpired:
            server.kill()
        log.close()
        import shutil
        shutil.rmtree(dump, ignore_errors=not kept["live"])
    return 0


if __name__ == "__main__":
    sys.exit(main())
