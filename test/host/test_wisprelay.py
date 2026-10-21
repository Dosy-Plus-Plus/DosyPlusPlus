#!/usr/bin/env python3
"""End-to-end test for tools/wisprelay.py, without a browser.

Speaks the client half of Wisp v1 over a hand-rolled WebSocket, asks the relay
to connect to the stub hub, and runs an ADC handshake through the tunnel. This
is what lets the relay be developed and trusted before any of the v86 side
exists -- a browser is a bad test harness.

    ./test/host/test_wisprelay.py
"""

import base64
import os
import socket
import struct
import subprocess
import sys
import time

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

RELAY_PORT = 18086
HUB_PORT = 11511
SENTINEL = "10.77.0.1"

PKT_CONNECT, PKT_DATA, PKT_CONTINUE, PKT_CLOSE = 1, 2, 3, 4

fails = []


def check(cond, what):
    print(f"{'ok  ' if cond else 'FAIL'}  {what}", flush=True)
    if not cond:
        fails.append(what)


class WsClient:
    def __init__(self, host, port, path="/"):
        self.s = socket.create_connection((host, port), timeout=10)
        key = base64.b64encode(os.urandom(16)).decode()
        self.s.sendall(
            f"GET {path} HTTP/1.1\r\nHost: {host}:{port}\r\n"
            f"Upgrade: websocket\r\nConnection: Upgrade\r\n"
            f"Sec-WebSocket-Key: {key}\r\nSec-WebSocket-Version: 13\r\n\r\n"
            .encode())
        self.buf = b""
        while b"\r\n\r\n" not in self.buf:
            chunk = self.s.recv(4096)
            if not chunk:
                raise RuntimeError("relay closed during handshake")
            self.buf += chunk
        head, self.buf = self.buf.split(b"\r\n\r\n", 1)
        if b"101" not in head.split(b"\r\n")[0]:
            raise RuntimeError(f"bad handshake: {head.split(chr(13).encode())[0]!r}")

    def send(self, payload):
        mask = os.urandom(4)
        masked = bytes(b ^ mask[i % 4] for i, b in enumerate(payload))
        n = len(payload)
        if n < 126:
            hdr = struct.pack("!BB", 0x82, 0x80 | n)
        elif n < 65536:
            hdr = struct.pack("!BBH", 0x82, 0x80 | 126, n)
        else:
            hdr = struct.pack("!BBQ", 0x82, 0x80 | 127, n)
        self.s.sendall(hdr + mask + masked)

    def _need(self, n):
        while len(self.buf) < n:
            chunk = self.s.recv(65536)
            if not chunk:
                raise RuntimeError("relay closed")
            self.buf += chunk

    def recv(self):
        self._need(2)
        b0, b1 = self.buf[0], self.buf[1]
        ln = b1 & 0x7F
        off = 2
        if ln == 126:
            self._need(4)
            ln = struct.unpack("!H", self.buf[2:4])[0]
            off = 4
        elif ln == 127:
            self._need(10)
            ln = struct.unpack("!Q", self.buf[2:10])[0]
            off = 10
        self._need(off + ln)
        data = self.buf[off:off + ln]
        self.buf = self.buf[off + ln:]
        return b0 & 0x0F, data

    def close(self):
        try:
            self.s.close()
        except OSError:
            pass


def wisp(ptype, sid, payload=b""):
    return struct.pack("<BI", ptype, sid) + payload


def recv_wisp(ws, timeout=10):
    ws.s.settimeout(timeout)
    _op, data = ws.recv()
    return struct.unpack("<BI", data[:5]) + (data[5:],)


def wait_port(port, tries=50):
    for _ in range(tries):
        try:
            socket.create_connection(("127.0.0.1", port), timeout=0.3).close()
            return True
        except OSError:
            time.sleep(0.1)
    return False


def main():
    hub = subprocess.Popen(
        [sys.executable, os.path.join(ROOT, "tools", "stubhub.py"),
         "--port", str(HUB_PORT)],
        stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    relay = subprocess.Popen(
        [sys.executable, os.path.join(ROOT, "tools", "wisprelay.py"),
         "--port", str(RELAY_PORT),
         "--map", f"{SENTINEL}=127.0.0.1:{HUB_PORT}"],
        stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)

    try:
        check(wait_port(HUB_PORT), "stub hub listening")
        check(wait_port(RELAY_PORT), "relay listening")

        # --- the relay must serve the browser host page too
        s = socket.create_connection(("127.0.0.1", RELAY_PORT), timeout=5)
        s.sendall(b"GET /index.html HTTP/1.1\r\nHost: x\r\n\r\n")
        page = b""
        s.settimeout(5)
        try:
            while True:
                chunk = s.recv(65536)
                if not chunk:
                    break
                page += chunk
        except socket.timeout:
            pass
        s.close()
        check(b"200 OK" in page, "serves index.html")
        check(b"Cross-Origin-Embedder-Policy: require-corp" in page,
              "sends COEP header (v86 needs SharedArrayBuffer)")

        # --- path containment
        s = socket.create_connection(("127.0.0.1", RELAY_PORT), timeout=5)
        s.sendall(b"GET /../../../../etc/passwd HTTP/1.1\r\nHost: x\r\n\r\n")
        s.settimeout(5)
        resp = s.recv(4096)
        s.close()
        check(b"200 OK" not in resp, "refuses path traversal")

        # --- wisp handshake
        ws = WsClient("127.0.0.1", RELAY_PORT)
        ptype, sid, payload = recv_wisp(ws)
        check(ptype == PKT_CONTINUE and sid == 0,
              "sends initial CONTINUE on stream 0")
        check(len(payload) == 4 and struct.unpack("<I", payload)[0] > 0,
              "initial CONTINUE carries a buffer size")

        # --- blocked destination
        ws.send(wisp(PKT_CONNECT, 7,
                     bytes([1]) + struct.pack("<H", 80) + b"203.0.113.9"))
        ptype, sid, payload = recv_wisp(ws)
        check(ptype == PKT_CLOSE and sid == 7 and payload[:1] == b"\x48",
              "CLOSE 0x48 (blocked) for an unmapped destination")

        # --- the real thing: ADC handshake through the tunnel
        ws.send(wisp(PKT_CONNECT, 1,
                     bytes([1]) + struct.pack("<H", 1511) + SENTINEL.encode()))
        ptype, sid, payload = recv_wisp(ws)
        check(ptype == PKT_CONTINUE and sid == 1,
              "CONTINUE after a successful CONNECT")

        ws.send(wisp(PKT_DATA, 1, b"HSUP ADBASE ADTIGR\n"))

        got = b""
        deadline = time.time() + 10
        while b"ISID" not in got and time.time() < deadline:
            ptype, sid, payload = recv_wisp(ws, timeout=5)
            if ptype == PKT_DATA and sid == 1:
                got += payload
            elif ptype == PKT_CLOSE:
                break

        check(b"ISUP" in got, "ISUP arrived through the tunnel")
        check(b"ISID" in got, "ISID arrived through the tunnel")

        # --- UDP is refused rather than silently dropped
        ws.send(wisp(PKT_CONNECT, 2,
                     bytes([2]) + struct.pack("<H", 1511) + SENTINEL.encode()))

        # Drain until the CLOSE for stream 2, rather than taking the next
        # packet and hoping: stream 1 is still live and the hub's own
        # chatter (an IMSG, a keepalive) can arrive first. This failed
        # intermittently for exactly that reason, which is worse than
        # failing always -- it reads as a bug somewhere else.
        closed = False
        deadline = time.time() + 10
        while not closed and time.time() < deadline:
            try:
                ptype, sid, _ = recv_wisp(ws, timeout=5)
            except (socket.timeout, OSError):
                break
            if ptype == PKT_CLOSE and sid == 2:
                closed = True
        check(closed, "refuses UDP streams")

        # --- guest-initiated close
        ws.send(wisp(PKT_CLOSE, 1, b"\x02"))
        time.sleep(0.3)
        check(relay.poll() is None, "relay survives a stream close")

        ws.close()
        time.sleep(0.3)
        check(relay.poll() is None, "relay survives a websocket drop")

    finally:
        for p in (relay, hub):
            p.terminate()
            try:
                p.wait(timeout=5)
            except subprocess.TimeoutExpired:
                p.kill()

    print()
    if fails:
        print(f"FAILED: {len(fails)} check(s): " + "; ".join(fails))
        return 1
    print("ok: all relay checks passed")
    return 0


if __name__ == "__main__":
    sys.exit(main())
