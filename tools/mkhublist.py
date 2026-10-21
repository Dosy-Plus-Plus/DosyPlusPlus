#!/usr/bin/env python3
"""Write a hub list with your hub in it, and nobody else's.

    tools/mkhublist.py OUTDIR [--name NAME] [--address adc://host:port]
                              [--users N] [--description TEXT]

The client's hub-list door (0) fetches a public list by default --
HUBLIST_DEFAULT_URL in src/adc/hublist.h, which is a real service somebody
else pays for. That is right for a copy of DOSy++ on a real machine and
wrong for a demo a few hundred strangers might open: it would point them
all at hubs that never agreed to be part of this.

So the demo ships its own list, served from the same machine as the demo
hub, with one entry: the demo hub. Door 0 then shows something true and
reaches nothing that is not ours.

Writes both forms, because a real list is bzip2 and the client sniffs:
    hublist.xml      plain
    hublist.xml.bz2  compressed
"""
import argparse
import bz2
import os
import sys
from xml.sax.saxutils import quoteattr


def main(argv):
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("outdir")
    ap.add_argument("--name", default="The Attic (demo)")
    ap.add_argument("--address", default="adc://10.77.0.1:1511")
    ap.add_argument("--users", type=int, default=4)
    ap.add_argument("--shared", default="7733058")
    ap.add_argument("--description",
                    default="This demo's own hub. Nothing else is listed.")
    args = ap.parse_args(argv[1:])

    xml = (
        '<?xml version="1.0" encoding="utf-8"?>\n'
        "<Hublist>\n"
        "  <Hubs>\n"
        f"    <Hub Name={quoteattr(args.name)} "
        f"Address={quoteattr(args.address)}\n"
        f"         Description={quoteattr(args.description)} "
        f'Users="{args.users}" Shared="{args.shared}"/>\n'
        "  </Hubs>\n"
        "</Hublist>\n"
    )

    os.makedirs(args.outdir, exist_ok=True)
    plain = os.path.join(args.outdir, "hublist.xml")
    packed = os.path.join(args.outdir, "hublist.xml.bz2")

    with open(plain, "w", encoding="utf-8") as f:
        f.write(xml)
    with open(packed, "wb") as f:
        f.write(bz2.compress(xml.encode("utf-8")))

    print(f"  + {plain}")
    print(f"  + {packed}  ({os.path.getsize(packed)} bytes)")
    print(f"  one entry: {args.name} at {args.address}")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
