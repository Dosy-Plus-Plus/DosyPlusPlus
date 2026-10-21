#!/usr/bin/env python3
"""A plaintext-to-TLS relay, so a DOS machine can reach an adcs:// hub.

DOSy++ contains no crypto and will not: a TLS stack on a 386 with 16MB is a
different project, and one with a bug in it is worse than none. The
architecture is a relay on the LAN instead -- see docs/STUNNEL.md for the
reasoning and for doing this with stunnel, which is what most people should
use.

This exists for two reasons stunnel does not cover:

  - It needs nothing but Python 3, which is already required by the stub
    hub and the Wisp relay. "Install stunnel first" is a real barrier for
    somebody who just wants to see whether their DOS box can reach a hub.
  - It is testable from the repo. `make test-adcs` runs a TLS hub, puts
    this in front of it and logs in through it, which turns the ADCS story
    from a paragraph in a document into something that either works or
    fails the build.

Certificate verification is ON by default and --insecure is a deliberate
noise-maker: in this architecture the relay is the only thing authenticating
the hub, so turning it off makes the whole TLS leg decoration.

    tools/tlsrelay.py --listen 0.0.0.0:1511 --connect somehub.example:2780
    tools/tlsrelay.py -l 1511 -c hub:2780 --cafile ./hub.pem
    tools/tlsrelay.py -l 1511 -c hub:2780 --insecure   # self-signed hub
"""
import argparse
import selectors
import socket
import ssl
import sys
import threading

BUFSIZE = 8192


def split_addr(text, default_host="0.0.0.0"):
    """"host:port", or just "port"."""
    if ":" in text:
        host, _, port = text.rpartition(":")
        return host or default_host, int(port)
    return default_host, int(text)


def pump(name, a, b, log):
    """Copy a -> b until one end goes away.

    One thread per direction per connection. A select loop would be tidier
    and this is a tool, not the client: threads here cost nothing and the
    error handling stays readable.
    """
    try:
        while True:
            data = a.recv(BUFSIZE)
            if not data:
                break
            b.sendall(data)
    except (OSError, ssl.SSLError) as e:
        log("%s: %s" % (name, e))
    finally:
        # Half-close rather than close: the other direction may still have
        # the hub's goodbye in flight.
        for sock in (a, b):
            try:
                sock.shutdown(socket.SHUT_RDWR)
            except OSError:
                pass


def serve(args):
    host_in, port_in = split_addr(args.listen)
    host_out, port_out = split_addr(args.connect, default_host="")
    if not host_out:
        print("--connect needs a host", file=sys.stderr)
        return 2

    quiet = args.quiet

    def log(msg):
        if not quiet:
            print("tlsrelay: %s" % msg, flush=True)

    ctx = ssl.create_default_context()
    if args.cafile:
        ctx.load_verify_locations(args.cafile)
    if args.insecure:
        # Said once, loudly. Someone reading a log should be able to tell
        # that this run was not authenticating anything.
        ctx.check_hostname = False
        ctx.verify_mode = ssl.CERT_NONE
        log("CERTIFICATES ARE NOT BEING VERIFIED (--insecure). The hub is "
            "not authenticated and neither is anything between you and it.")

    srv = socket.socket()
    srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    try:
        srv.bind((host_in, port_in))
    except OSError as e:
        print("tlsrelay: cannot bind %s:%d: %s" % (host_in, port_in, e),
              file=sys.stderr)
        return 1
    srv.listen(5)

    log("%s:%d (plaintext)  ->  %s:%d (TLS)"
        % (host_in, port_in, host_out, port_out))
    if host_in in ("127.0.0.1", "localhost"):
        log("note: bound to loopback, so another machine cannot reach "
            "this. A DOS box needs the LAN address.")

    conns = 0
    try:
        while True:
            client, who = srv.accept()
            conns += 1
            log("connection %d from %s" % (conns, who[0]))

            try:
                raw = socket.create_connection((host_out, port_out),
                                               timeout=20)
                tls = ctx.wrap_socket(
                    raw, server_hostname=args.servername or host_out)
            except (OSError, ssl.SSLError) as e:
                # The client gets a closed socket, which it reports as the
                # hub not answering; the reason is here.
                log("could not reach the hub: %s" % e)
                client.close()
                continue

            cipher = tls.cipher()
            log("TLS up: %s" % (cipher[0] if cipher else "?"))
            tls.settimeout(None)

            for nm, a, b in (("up", client, tls), ("down", tls, client)):
                t = threading.Thread(target=pump, args=(nm, a, b, log),
                                     daemon=True)
                t.start()

            if args.once:
                # For the test rig: serve one session and stop, so a run
                # cannot leave a listener behind.
                import time
                while True:
                    time.sleep(0.2)
                    if client.fileno() < 0 or tls.fileno() < 0:
                        break
                log("done")
                return 0
    except KeyboardInterrupt:
        log("stopping")
    finally:
        srv.close()
    return 0


def main(argv):
    ap = argparse.ArgumentParser(
        description="plaintext -> TLS relay for reaching adcs:// hubs")
    ap.add_argument("--listen", "-l", default="0.0.0.0:1511",
                    help="where DOSy++ dials; host:port or just port")
    ap.add_argument("--connect", "-c", required=True,
                    help="the real hub: host:port (ADCS is usually 2780)")
    ap.add_argument("--cafile", default=None,
                    help="a PEM file to verify the hub against")
    ap.add_argument("--servername", default=None,
                    help="SNI/verification name, if it differs from the "
                         "connect host")
    ap.add_argument("--insecure", action="store_true",
                    help="do not verify the certificate. For a hub with a "
                         "self-signed one, and nothing else")
    ap.add_argument("--once", action="store_true",
                    help="serve one connection and exit (for test rigs)")
    ap.add_argument("--quiet", "-q", action="store_true")
    args = ap.parse_args(argv[1:])
    return serve(args)


if __name__ == "__main__":
    sys.exit(main(sys.argv))
