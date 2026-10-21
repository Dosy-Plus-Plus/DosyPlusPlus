#!/usr/bin/env python3
"""Wisp relay + static file server for the DOSy++ browser host.

v86's WispNetworkAdapter implements ARP, DHCP and a TCP state machine in
JavaScript, and tunnels each guest TCP connection to a Wisp server as a
stream. This is that server. It also serves ./web on the same port, so the
whole browser host is one command.

Architecture:

    DOS guest (v86)            browser JS            this relay        hub
    Watt-32 + RTL8029  --eth-> WispNetworkAdapter --ws--> TCP/TLS --> ADC hub

Because v86 sends the *dotted-quad the guest dialed* as the Wisp CONNECT
hostname, destinations are configured here as IP mappings. The guest dials a
sentinel address, the relay connects to the real hub -- optionally over TLS,
which is how the browser build gets ADCS support without crypto in DOS.

    ./tools/wisprelay.py --map 10.77.0.1=127.0.0.1:1511
    ./tools/wisprelay.py --map 10.77.0.1=adcs.example.org:2780:tls

Then from DOS:  dosypp 10.77.0.1

Wisp v1: https://github.com/MercuryWorkshop/wisp-protocol
Deliberately dependency-free, like tools/stubhub.py.
"""

import argparse
import asyncio
import base64
import hashlib
import mimetypes
import os
import ssl
import struct
import sys

# ---------------------------------------------------------------- Wisp v1

PKT_CONNECT = 0x01
PKT_DATA = 0x02
PKT_CONTINUE = 0x03
PKT_CLOSE = 0x04

STREAM_TCP = 0x01
STREAM_UDP = 0x02

CLOSE_VOLUNTARY = 0x02
CLOSE_NETWORK_ERROR = 0x03
CLOSE_INVALID_INFO = 0x41
CLOSE_UNREACHABLE = 0x42
CLOSE_TIMEOUT = 0x43
CLOSE_REFUSED = 0x44
CLOSE_BLOCKED = 0x48

# How many DATA packets the client may send before it must wait for a
# CONTINUE. Replenished every REFILL packets so the client never stalls.
BUFFER_SIZE = 128
REFILL_AFTER = 32


def wisp_packet(ptype: int, stream_id: int, payload: bytes = b"") -> bytes:
    return struct.pack("<BI", ptype, stream_id) + payload


# ------------------------------------------------------- minimal WebSocket

WS_GUID = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11"

OP_CONT, OP_TEXT, OP_BINARY = 0x0, 0x1, 0x2
OP_CLOSE, OP_PING, OP_PONG = 0x8, 0x9, 0xA


class WebSocket:
    """Server-side WebSocket framing. Enough of RFC 6455 for v86: binary
    frames, fragmentation, ping/pong, close. No extensions, no compression."""

    def __init__(self, reader, writer):
        self.r = reader
        self.w = writer
        self.closed = False

    async def send_binary(self, data: bytes) -> None:
        if self.closed:
            return
        n = len(data)
        if n < 126:
            hdr = struct.pack("!BB", 0x80 | OP_BINARY, n)
        elif n < 65536:
            hdr = struct.pack("!BBH", 0x80 | OP_BINARY, 126, n)
        else:
            hdr = struct.pack("!BBQ", 0x80 | OP_BINARY, 127, n)
        try:
            self.w.write(hdr + data)
            await self.w.drain()
        except (ConnectionError, OSError):
            self.closed = True

    async def _send_control(self, op: int, payload: bytes = b"") -> None:
        if self.closed:
            return
        try:
            self.w.write(struct.pack("!BB", 0x80 | op, len(payload)) + payload)
            await self.w.drain()
        except (ConnectionError, OSError):
            self.closed = True

    async def recv(self):
        """Return the next complete message as bytes, or None at end."""
        chunks = []
        while True:
            hdr = await self.r.readexactly(2)
            fin = bool(hdr[0] & 0x80)
            op = hdr[0] & 0x0F
            masked = bool(hdr[1] & 0x80)
            ln = hdr[1] & 0x7F

            if ln == 126:
                ln = struct.unpack("!H", await self.r.readexactly(2))[0]
            elif ln == 127:
                ln = struct.unpack("!Q", await self.r.readexactly(8))[0]

            # A client frame must be masked; an unmasked one is a protocol
            # violation, and silently accepting it hides broken clients.
            mask = await self.r.readexactly(4) if masked else None
            data = await self.r.readexactly(ln) if ln else b""

            if mask:
                data = bytes(b ^ mask[i % 4] for i, b in enumerate(data))

            if op == OP_CLOSE:
                await self._send_control(OP_CLOSE, data[:2])
                self.closed = True
                return None
            if op == OP_PING:
                await self._send_control(OP_PONG, data)
                continue
            if op == OP_PONG:
                continue

            chunks.append(data)
            if fin:
                return b"".join(chunks)

    async def close(self) -> None:
        await self._send_control(OP_CLOSE, struct.pack("!H", 1000))
        self.closed = True


# -------------------------------------------------------------- the relay


class Stream:
    """One guest TCP connection, proxied to a real destination."""

    def __init__(self, sid, ws, reader, writer, log):
        self.sid = sid
        self.ws = ws
        self.r = reader
        self.w = writer
        self.log = log
        self.since_refill = 0
        self.task = None

    async def pump(self):
        """Upstream -> guest."""
        try:
            while True:
                data = await self.r.read(8192)
                if not data:
                    break
                await self.ws.send_binary(
                    wisp_packet(PKT_DATA, self.sid, data))
        except (ConnectionError, OSError, asyncio.IncompleteReadError):
            pass
        finally:
            self.log(f"[{self.sid}] upstream closed")
            await self.ws.send_binary(
                wisp_packet(PKT_CLOSE, self.sid,
                            bytes([CLOSE_VOLUNTARY])))
            self.shutdown()

    def shutdown(self):
        try:
            self.w.close()
        except OSError:
            pass


class Relay:
    def __init__(self, mappings, allow_direct, webroot, verbose):
        self.mappings = mappings          # "a.b.c.d" -> (host, port, tls)
        self.allow_direct = allow_direct
        self.webroot = webroot
        self.verbose = verbose

    def log(self, msg):
        print(msg, flush=True)

    def vlog(self, msg):
        if self.verbose:
            print(msg, flush=True)

    # ---------------------------------------------------------- dispatch

    async def handle(self, reader, writer):
        peer = writer.get_extra_info("peername")
        try:
            request = await self.read_http_head(reader)
        except (asyncio.IncompleteReadError, ConnectionError, OSError):
            writer.close()
            return

        if request is None:
            writer.close()
            return

        method, path, headers = request

        if headers.get("upgrade", "").lower() == "websocket":
            self.log(f"wisp: websocket from {peer[0]}:{peer[1]} ({path})")
            await self.serve_wisp(reader, writer, headers)
        else:
            await self.serve_file(writer, method, path)

    async def read_http_head(self, reader):
        head = await reader.readuntil(b"\r\n\r\n")
        lines = head.decode("latin-1").split("\r\n")
        if not lines or len(lines[0].split()) < 2:
            return None
        parts = lines[0].split()
        method, path = parts[0], parts[1]
        headers = {}
        for line in lines[1:]:
            if ":" in line:
                k, v = line.split(":", 1)
                headers[k.strip().lower()] = v.strip()
        return method, path, headers

    # ------------------------------------------------------- static files

    async def serve_file(self, writer, method, path):
        def respond(status, body, ctype="text/plain; charset=utf-8"):
            hdr = (f"HTTP/1.1 {status}\r\n"
                   f"Content-Type: {ctype}\r\n"
                   f"Content-Length: {len(body)}\r\n"
                   # v86 needs SharedArrayBuffer for its threaded build, and
                   # these two headers are what unlock it.
                   "Cross-Origin-Opener-Policy: same-origin\r\n"
                   "Cross-Origin-Embedder-Policy: require-corp\r\n"
                   "Cache-Control: no-store\r\n"
                   "Connection: close\r\n\r\n").encode()
            writer.write(hdr + body)

        if not self.webroot:
            respond("404 Not Found", b"no webroot configured\n")
        elif method not in ("GET", "HEAD"):
            respond("405 Method Not Allowed", b"GET only\n")
        else:
            rel = path.split("?", 1)[0].lstrip("/") or "index.html"
            full = os.path.realpath(os.path.join(self.webroot, rel))
            root = os.path.realpath(self.webroot)

            # Containment check: a relay that serves the whole filesystem
            # because of "../.." is a worse bug than anything else here.
            if not (full == root or full.startswith(root + os.sep)):
                respond("403 Forbidden", b"outside webroot\n")
            elif not os.path.isfile(full):
                respond("404 Not Found", f"no {rel}\n".encode())
            else:
                ctype = mimetypes.guess_type(full)[0] or "application/octet-stream"
                with open(full, "rb") as f:
                    body = f.read()
                self.vlog(f"http: {rel} ({len(body)} bytes)")
                respond("200 OK", b"" if method == "HEAD" else body, ctype)

        try:
            await writer.drain()
        except (ConnectionError, OSError):
            pass
        writer.close()

    # -------------------------------------------------------------- wisp

    async def serve_wisp(self, reader, writer, headers):
        key = headers.get("sec-websocket-key")
        if not key:
            writer.write(b"HTTP/1.1 400 Bad Request\r\n\r\n")
            writer.close()
            return

        accept = base64.b64encode(
            hashlib.sha1((key + WS_GUID).encode()).digest()).decode()
        writer.write(
            ("HTTP/1.1 101 Switching Protocols\r\n"
             "Upgrade: websocket\r\n"
             "Connection: Upgrade\r\n"
             f"Sec-WebSocket-Accept: {accept}\r\n\r\n").encode())
        await writer.drain()

        ws = WebSocket(reader, writer)
        streams = {}

        # The client waits for this before sending anything.
        await ws.send_binary(
            wisp_packet(PKT_CONTINUE, 0, struct.pack("<I", BUFFER_SIZE)))

        try:
            while True:
                msg = await ws.recv()
                if msg is None:
                    break
                if len(msg) < 5:
                    self.vlog(f"wisp: runt packet ({len(msg)} bytes)")
                    continue

                ptype, sid = struct.unpack("<BI", msg[:5])
                payload = msg[5:]

                if ptype == PKT_CONNECT:
                    await self.on_connect(ws, streams, sid, payload)
                elif ptype == PKT_DATA:
                    await self.on_data(ws, streams, sid, payload)
                elif ptype == PKT_CLOSE:
                    self.on_close(streams, sid, payload)
                elif ptype == PKT_CONTINUE:
                    pass  # client-bound only
                else:
                    self.vlog(f"wisp: unknown packet type {ptype:#x}")
        except (asyncio.IncompleteReadError, ConnectionError, OSError):
            pass
        finally:
            for st in list(streams.values()):
                if st.task:
                    st.task.cancel()
                st.shutdown()
            self.log("wisp: websocket closed")
            try:
                writer.close()
            except OSError:
                pass

    def resolve(self, host, port):
        """Map a guest-dialled address to a real destination.
        Returns (host, port, tls) or None if not permitted."""
        if host in self.mappings:
            return self.mappings[host]
        if self.allow_direct:
            return (host, port, False)
        return None

    async def on_connect(self, ws, streams, sid, payload):
        if len(payload) < 3:
            await ws.send_binary(wisp_packet(PKT_CLOSE, sid,
                                             bytes([CLOSE_INVALID_INFO])))
            return

        stype = payload[0]
        port = struct.unpack("<H", payload[1:3])[0]
        host = payload[3:].decode("utf-8", "replace")

        if stype != STREAM_TCP:
            # UDP would be needed for ADC search results over UDP; the v86
            # client does not forward UDP yet, so this should not arrive.
            self.log(f"[{sid}] UDP to {host}:{port} refused (not supported)")
            await ws.send_binary(wisp_packet(PKT_CLOSE, sid,
                                             bytes([CLOSE_BLOCKED])))
            return

        dest = self.resolve(host, port)
        if dest is None:
            self.log(f"[{sid}] BLOCKED {host}:{port} (no mapping)")
            await ws.send_binary(wisp_packet(PKT_CLOSE, sid,
                                             bytes([CLOSE_BLOCKED])))
            return

        rhost, rport, use_tls = dest
        label = f"{rhost}:{rport}" + (" (TLS)" if use_tls else "")
        self.log(f"[{sid}] CONNECT {host}:{port} -> {label}")

        try:
            ctx = None
            if use_tls:
                ctx = ssl.create_default_context()
                # Verification stays on: the relay is the only thing
                # authenticating the hub in this architecture.
            r, w = await asyncio.wait_for(
                asyncio.open_connection(rhost, rport, ssl=ctx,
                                        server_hostname=rhost if ctx else None),
                timeout=20)
        except asyncio.TimeoutError:
            self.log(f"[{sid}] timeout connecting to {label}")
            await ws.send_binary(wisp_packet(PKT_CLOSE, sid,
                                             bytes([CLOSE_TIMEOUT])))
            return
        except (ConnectionRefusedError, OSError, ssl.SSLError) as e:
            self.log(f"[{sid}] cannot reach {label}: {e}")
            reason = (CLOSE_REFUSED if isinstance(e, ConnectionRefusedError)
                      else CLOSE_UNREACHABLE)
            await ws.send_binary(wisp_packet(PKT_CLOSE, sid, bytes([reason])))
            return

        st = Stream(sid, ws, r, w, self.log)
        streams[sid] = st
        st.task = asyncio.create_task(st.pump())

        await ws.send_binary(
            wisp_packet(PKT_CONTINUE, sid, struct.pack("<I", BUFFER_SIZE)))

    async def on_data(self, ws, streams, sid, payload):
        st = streams.get(sid)
        if st is None:
            self.vlog(f"[{sid}] DATA for unknown stream")
            return
        try:
            st.w.write(payload)
            await st.w.drain()
        except (ConnectionError, OSError):
            self.log(f"[{sid}] write failed, closing")
            await ws.send_binary(wisp_packet(PKT_CLOSE, sid,
                                             bytes([CLOSE_NETWORK_ERROR])))
            st.shutdown()
            streams.pop(sid, None)
            return

        st.since_refill += 1
        if st.since_refill >= REFILL_AFTER:
            st.since_refill = 0
            await ws.send_binary(
                wisp_packet(PKT_CONTINUE, sid, struct.pack("<I", BUFFER_SIZE)))

    def on_close(self, streams, sid, payload):
        reason = payload[0] if payload else 0
        st = streams.pop(sid, None)
        if st is None:
            # A duplicate CLOSE for a stream already gone is normal: the
            # guest and the upstream can both decide to close at once.
            self.vlog(f"[{sid}] CLOSE for an already-closed stream")
            return
        self.log(f"[{sid}] guest closed (reason {reason:#x})")
        if st.task:
            st.task.cancel()
        st.shutdown()

    # --------------------------------------------------------------- run

    async def run(self, host, port):
        server = await asyncio.start_server(self.handle, host, port)
        self.log(f"wisprelay: http://{host}:{port}/   (also the wisp endpoint)")
        self.log(f"wisprelay: serving {self.webroot or '(nothing)'}")
        for k, (h, p, tls) in self.mappings.items():
            self.log(f"wisprelay: {k}:* -> {h}:{p}" + (" TLS" if tls else ""))
        if self.allow_direct:
            self.log("wisprelay: --allow-direct is ON (any destination)")
        async with server:
            await server.serve_forever()


def parse_map(spec):
    """IP=host:port[:tls]"""
    if "=" not in spec:
        raise argparse.ArgumentTypeError("expected IP=host:port[:tls]")
    key, dest = spec.split("=", 1)
    bits = dest.split(":")
    if len(bits) == 2:
        host, port, tls = bits[0], bits[1], False
    elif len(bits) == 3 and bits[2].lower() in ("tls", "ssl"):
        host, port, tls = bits[0], bits[1], True
    else:
        raise argparse.ArgumentTypeError(f"cannot parse destination {dest!r}")
    return key.strip(), (host, int(port), tls)


def main():
    ap = argparse.ArgumentParser(
        description=__doc__,
        formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--host", default="127.0.0.1")
    ap.add_argument("--port", type=int, default=8086)
    ap.add_argument("--map", type=parse_map, action="append", default=[],
                    metavar="IP=HOST:PORT[:tls]",
                    help="map an address the DOS guest dials to a real "
                         "destination; repeatable")
    ap.add_argument("--allow-direct", action="store_true",
                    help="allow any destination (development only -- this is "
                         "an open TCP proxy)")
    ap.add_argument("--web", default=None,
                    help="directory to serve (default: ./web next to this "
                         "script's repo root)")
    ap.add_argument("-v", "--verbose", action="store_true")
    args = ap.parse_args()

    webroot = args.web
    if webroot is None:
        here = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
        candidate = os.path.join(here, "web")
        webroot = candidate if os.path.isdir(candidate) else None

    mappings = dict(args.map)
    if not mappings and not args.allow_direct:
        # Default to the stub hub, so the common case needs no flags.
        mappings = {"10.77.0.1": ("127.0.0.1", 1511, False)}
        print("wisprelay: no --map given, defaulting "
              "10.77.0.1 -> 127.0.0.1:1511", flush=True)

    relay = Relay(mappings, args.allow_direct, webroot, args.verbose)
    try:
        asyncio.run(relay.run(args.host, args.port))
    except KeyboardInterrupt:
        print("\nwisprelay: bye", flush=True)
    return 0


if __name__ == "__main__":
    sys.exit(main())
