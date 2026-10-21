#!/usr/bin/env python3
"""A static file server that honours Range requests.

python3 -m http.server does not: it answers a Range request with the whole
file and a 200, which is exactly the thing this rig needs to tell apart. v86
reads the 64MB disk image in blocks over HTTP Range, so a host that ignores
Range makes "stream the disk image" quietly download all of it instead.

This is a fixture, not a product: it exists so the bundle can be read the
way a real host serves it. Whether the host you publish to supports Range is
a question about that host -- for GitHub Pages, check the live site:

    curl -s -D- -r 0-15 -o /dev/null https://you.github.io/dosypp/images/dosypp.img

A 206 with a Content-Range header is the answer you want.

    test/web/rangeserver.py DIR [PORT]
"""
import os
import sys
from http.server import HTTPServer, SimpleHTTPRequestHandler


class RangeHandler(SimpleHTTPRequestHandler):
    def send_head(self):
        rng = self.headers.get("Range")
        if not rng:
            return super().send_head()

        path = self.translate_path(self.path)
        if os.path.isdir(path) or not os.path.isfile(path):
            return super().send_head()

        try:
            unit, _, spec = rng.partition("=")
            if unit.strip() != "bytes":
                raise ValueError(rng)
            first, _, last = spec.partition("-")
            size = os.path.getsize(path)

            if first == "":                      # bytes=-N, the last N
                length = int(last)
                start = max(0, size - length)
                end = size - 1
            else:
                start = int(first)
                end = int(last) if last else size - 1
                end = min(end, size - 1)

            if start > end or start >= size:
                self.send_error(416, "Requested range not satisfiable")
                return None
        except ValueError:
            self.send_error(400, f"cannot parse Range: {rng!r}")
            return None

        f = open(path, "rb")
        f.seek(start)

        self.send_response(206)
        self.send_header("Content-Type", self.guess_type(path))
        self.send_header("Content-Length", str(end - start + 1))
        self.send_header("Content-Range", f"bytes {start}-{end}/{size}")
        self.send_header("Accept-Ranges", "bytes")
        self.end_headers()

        # copyfile() would send to the end of the file; hand it just the
        # window by reading it here.
        self.wfile.write(f.read(end - start + 1))
        f.close()
        return None

    def end_headers(self):
        self.send_header("Accept-Ranges", "bytes")
        super().end_headers()

    def log_message(self, fmt, *args):
        sys.stderr.write("%s - %s\n" % (self.address_string(), fmt % args))


def main(argv):
    if len(argv) < 2:
        print(__doc__)
        return 2
    directory = argv[1]
    port = int(argv[2]) if len(argv) > 2 else 8000

    handler = lambda *a, **k: RangeHandler(*a, directory=directory, **k)
    srv = HTTPServer(("127.0.0.1", port), handler)
    print(f"serving {directory} on http://127.0.0.1:{port}/ with Range",
          flush=True)
    srv.serve_forever()
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
