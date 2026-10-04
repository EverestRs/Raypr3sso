#!/usr/bin/env python3
"""阶段六 acceptance: auth token, Origin check, rate limits, render timeout,
session expiry GC (design 12/13).

Boots render_server with RENDER_TOKEN / short RENDER_SESSION_TTL /
RENDER_RENDER_TIMEOUT and verifies every item of the 阶段六 acceptance list:
unauthorized requests, over-limit tasks, disconnect, repeated cancel and
expiring sessions.
"""
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
PORT = int(os.environ.get("PORT", "9171"))
TOKEN = "security-check-token"
BASE = f"http://127.0.0.1:{PORT}"


def http(method, path, body=None, token=TOKEN, origin=None):
    headers = {}
    if token is not None:
        headers["Authorization"] = f"Bearer {token}"
    if origin is not None:
        headers["Origin"] = origin
    req = urllib.request.Request(
        BASE + path, method=method,
        data=json.dumps(body).encode() if body is not None else None,
        headers=headers)
    try:
        with urllib.request.urlopen(req, timeout=15) as resp:
            return resp.status, resp.read()
    except urllib.error.HTTPError as err:
        return err.code, err.read()


def ws_handshake(path, token=TOKEN, origin=None):
    """Returns (status_line, sock or None)."""
    sock = socket.create_connection(("127.0.0.1", PORT), timeout=5)
    request = f"GET {path} HTTP/1.1\r\nHost: 127.0.0.1:{PORT}\r\n"
    if token is not None:
        request += f"?token={token}" if False else ""
    # token 走 query（浏览器 WS 无法设头）
    sep = "&" if "?" in path else "?"
    if token is not None:
        request = f"GET {path}{sep}token={token} HTTP/1.1\r\nHost: 127.0.0.1:{PORT}\r\n"
    if origin is not None:
        request += f"Origin: {origin}\r\n"
    request += "Upgrade: websocket\r\nConnection: Upgrade\r\n"
    request += "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\nSec-WebSocket-Version: 13\r\n\r\n"
    sock.sendall(request.encode())
    data = b""
    while b"\r\n\r\n" not in data:
        chunk = sock.recv(4096)
        if not chunk:
            raise RuntimeError("closed during handshake")
        data += chunk
    status_line = data.split(b"\r\n")[0].decode()
    if "101" not in status_line:
        sock.close()
        return status_line, None
    return status_line, sock


def wait_ready(sid):
    for _ in range(300):
        state = json.loads(http("GET", f"/api/sessions/{sid}")[1])["state"]
        if state == "ready":
            return
        assert state != "failed", "sphere load failed"
        time.sleep(0.3)
    raise TimeoutError("session not ready")


def main():
    dump = tempfile.mkdtemp(prefix="wf_sec_")
    log = open(os.path.join(dump, "server.log"), "w")
    env = dict(os.environ, RENDER_TOKEN=TOKEN, RENDER_RENDER_TIMEOUT="6")
    server = subprocess.Popen([os.path.join(ROOT, "build", "render_server"),
                               "127.0.0.1", str(PORT)],
                              stdout=log, stderr=log, cwd=ROOT, env=env)
    try:
        for _ in range(50):
            try:
                if http("GET", "/api/health", token=None)[0] == 200:
                    break
            except (urllib.error.URLError, socket.timeout):
                pass
            time.sleep(0.2)

        print("--- unauthorized requests rejected ---")
        assert http("GET", "/api/sessions", token=None)[0] == 401
        assert http("GET", "/api/sessions", token="wrong")[0] == 401
        assert http("POST", "/api/sessions", {"model": "sphere-test"},
                    token=None)[0] == 401
        print("  no/wrong token -> 401; /api/health stays open OK")

        print("--- WebSocket auth + Origin ---")
        line, _ = ws_handshake("/api/sessions/s-999/events", token=None)
        assert "401" in line, line
        line, _ = ws_handshake("/api/sessions/s-999/events", token="wrong")
        assert "401" in line, line
        line, _ = ws_handshake("/api/sessions/s-1/events", token=TOKEN,
                               origin="http://evil.example.com")
        assert "403" in line, line
        _, body = http("POST", "/api/sessions", {"model": "sphere-test"})
        sid = json.loads(body)["sessionId"]
        wait_ready(sid)
        line, sock = ws_handshake(f"/api/sessions/{sid}/events", token=TOKEN,
                                  origin="http://127.0.0.1:8080")
        assert "101" in line, line
        sock.close()
        line, _ = ws_handshake(f"/api/sessions/{sid}/events", token=TOKEN,
                               origin="null")
        assert "101" in line, line
        print("  401 no/wrong token, 403 evil origin, 101 loopback + null OK")

        print("--- rate limits ---")
        codes = [http("POST", f"/api/sessions/{sid}/camera",
                      {"position": [0, 0, 3], "direction": [0, 0, -1],
                       "up": [0, 1, 0], "fov": 60})[0] for _ in range(40)]
        assert 429 in codes and codes[0] == 200, codes[:5]
        print(f"  camera burst: {codes.count(429)}/40 limited OK")
        session_codes = [http("POST", "/api/sessions",
                              {"model": "sphere-test"})[0] for _ in range(14)]
        assert 429 in session_codes, session_codes
        print(f"  create burst: {session_codes.count(429)}/14 limited OK")

        print("--- render timeout (RENDER_RENDER_TIMEOUT=6) ---")
        _, body = http("POST", f"/api/sessions/{sid}/render",
                       {"width": 640, "height": 360, "sppBudget": 4096,
                        "maxDepth": 8})
        rid = json.loads(body)["renderId"]
        deadline = time.time() + 20
        state = None
        while time.time() < deadline:
            info = json.loads(http("GET", f"/api/renders/{rid}")[1])
            state = info["state"]
            if state == "failed":
                break
            time.sleep(1)
        assert state == "failed", state
        assert json.loads(http("GET", f"/api/renders/{rid}")[1])["sample"] < 4096
        print(f"  4096-spp chain -> failed by timeout, sample={info['sample']} OK")

        print("--- repeated cancel stays idempotent ---")
        assert http("POST", f"/api/renders/{rid}/cancel")[0] == 200
        assert http("POST", f"/api/renders/{rid}/cancel")[0] == 200
        print("  cancel x2 on failed render OK")

        print("--- session expiry GC (separate server, TTL=6) ---")
        port2 = PORT + 1
        dump2 = tempfile.mkdtemp(prefix="wf_sec_gc_")
        log2 = open(os.path.join(dump2, "server.log"), "w")
        env2 = dict(os.environ, RENDER_TOKEN=TOKEN, RENDER_SESSION_TTL="6")
        server2 = subprocess.Popen(
            [os.path.join(ROOT, "build", "render_server"), "127.0.0.1", str(port2)],
            stdout=log2, stderr=log2, cwd=ROOT, env=env2)
        try:
            base2 = f"http://127.0.0.1:{port2}"
            for _ in range(50):
                try:
                    urllib.request.urlopen(base2 + "/api/health", timeout=3)
                    break
                except Exception:
                    time.sleep(0.2)
            req = urllib.request.Request(base2 + "/api/sessions", method="POST",
                                         data=b'{"model":"sphere-test"}',
                                         headers={"Authorization": f"Bearer {TOKEN}"})
            sid2 = json.loads(urllib.request.urlopen(req, timeout=15).read())["sessionId"]
            for _ in range(300):
                req = urllib.request.Request(base2 + f"/api/sessions/{sid2}",
                                             headers={"Authorization": f"Bearer {TOKEN}"})
                if json.loads(urllib.request.urlopen(req, timeout=5).read())["state"] == "ready":
                    break
                time.sleep(0.3)
            time.sleep(16)   # ttl 6 + gc interval 5 + slack
            req = urllib.request.Request(base2 + f"/api/sessions/{sid2}",
                                         headers={"Authorization": f"Bearer {TOKEN}"})
            try:
                urllib.request.urlopen(req, timeout=5)
                raise AssertionError("expired session still visible")
            except urllib.error.HTTPError as err:
                assert err.code == 404, err.code
            gc_lines = [l for l in open(os.path.join(dump2, "server.log"))
                        if l.startswith("gc:")]
            assert gc_lines, "gc did not log"
            # 过期后可以新建 session（配额被回收）
            req = urllib.request.Request(base2 + "/api/sessions", method="POST",
                                         data=b'{"model":"sphere-test"}',
                                         headers={"Authorization": f"Bearer {TOKEN}"})
            assert urllib.request.urlopen(req, timeout=15).status == 201
            print(f"  expired session 404, {gc_lines[0].strip()} , new session OK")
        finally:
            server2.terminate()
            try:
                server2.wait(timeout=5)
            except subprocess.TimeoutExpired:
                server2.kill()
            log2.close()
            import shutil
            shutil.rmtree(dump2, ignore_errors=True)

        print("ALL PHASE-6 SECURITY CHECKS PASS")
        return 0
    finally:
        server.terminate()
        try:
            server.wait(timeout=10)
        except subprocess.TimeoutExpired:
            server.kill()
        log.close()
        import shutil
        shutil.rmtree(dump, ignore_errors=True)


if __name__ == "__main__":
    sys.exit(main())
