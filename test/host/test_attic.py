#!/usr/bin/env python3
"""Unit tests for the demo hub's serving node (tools/stubhub.py --serve).

What is worth testing here is the share index, not the socket work -- the
sockets are covered end to end by make test-v86-dl, where a DOS client in a
browser engine really downloads a file. What that rig cannot show is the
refusals: a SHARE.TXT that disagrees with the files beside it must stop the
hub rather than make it advertise hashes it cannot honour, because a client
then downloads bytes, computes a different TTH and reports a corrupt
transfer -- blaming the network for a bookkeeping mistake.

    ./test/host/test_attic.py
"""

import os
import sys
import tempfile

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(
    os.path.abspath(__file__))))
sys.path.insert(0, os.path.join(ROOT, "tools"))

import stubhub                                    # noqa: E402

FAILED = 0
TTH_A = "P35QYOY5FCQUOEWBWJSZQX4FCDKNZUUQTWNL6MY"
TTH_B = "MH7UK6IDFNJQ3VBJXFWYDLSHK7ZQZL5XQ6PRPTA"


def check(what, got, want):
    global FAILED
    if got == want:
        print(f"  ok    {what}")
    else:
        print(f"  FAIL  {what}: got {got!r}, wanted {want!r}")
        FAILED += 1


def first(problems):
    """The first complaint, or "" -- so a check that was supposed to produce
    one reports a failure rather than an IndexError three lines later."""
    return problems[0] if problems else ""


def share(tmp, lines, files):
    d = os.path.join(tmp, "attic")
    for name, data in files.items():
        path = os.path.join(d, name)
        os.makedirs(os.path.dirname(path), exist_ok=True)
        with open(path, "wb") as f:
            f.write(data)
    os.makedirs(d, exist_ok=True)
    with open(os.path.join(d, "SHARE.TXT"), "w") as f:
        f.write("# a comment, and a blank line follow\n\n")
        f.write("\n".join(lines) + "\n")
    return d


def main():
    with tempfile.TemporaryDirectory() as tmp:
        print("a share that adds up:")
        d = share(tmp, [f"{TTH_A}  5  warez/games/A.ZIP",
                        f"{TTH_B}  3  mods/B.MOD"],
                  {"warez/games/A.ZIP": b"hello", "mods/B.MOD": b"abc"})
        files, problems = stubhub.load_share(d)
        check("two files loaded", len(files), 2)
        check("no problems", problems, [])
        check("name kept as the virtual path", files[0].name,
              "warez/games/A.ZIP")
        check("size from the index", files[0].size, 5)

        print("a size that disagrees with the file:")
        d = share(tmp + "/x", [f"{TTH_A}  9999  warez/games/A.ZIP"],
                  {"warez/games/A.ZIP": b"hello"})
        files, problems = stubhub.load_share(d)
        check("nothing served", len(files), 0)
        check("one complaint", len(problems), 1)
        check("it says to rehash", "rehash" in first(problems), True)

        print("a file named but not there:")
        d = share(tmp + "/y", [f"{TTH_A}  5  warez/games/GONE.ZIP"], {})
        files, problems = stubhub.load_share(d)
        check("nothing served", len(files), 0)
        check("it names the file", "GONE.ZIP" in first(problems), True)

        print("a hash of the wrong length:")
        d = share(tmp + "/z", [f"SHORT  5  warez/games/A.ZIP"],
                  {"warez/games/A.ZIP": b"hello"})
        files, problems = stubhub.load_share(d)
        check("nothing served", len(files), 0)
        check("it says what a TTH looks like",
              "39-character" in first(problems), True)

        print("no SHARE.TXT at all:")
        d = os.path.join(tmp, "empty")
        os.makedirs(d, exist_ok=True)
        files, problems = stubhub.load_share(d)
        check("nothing served", len(files), 0)
        check("it names the tool that writes one",
              "mkshare" in first(problems), True)

        print("the file list it generates:")
        d = share(tmp + "/w", [f"{TTH_A}  5  warez/games/A.ZIP",
                               f"{TTH_B}  3  mods/B.MOD"],
                  {"warez/games/A.ZIP": b"hello", "mods/B.MOD": b"abc"})
        files, _ = stubhub.load_share(d)
        peer = stubhub.AtticPeer(files, "127.0.0.1", 0, "CID", lambda m: None)
        xml = peer.files_xml("/").decode("utf-8")
        check("declares its base", 'Base="/"' in xml, True)
        check("directories nest", "<Directory Name=\"games\">" in xml, True)
        check("files carry their hash", f'TTH="{TTH_A}"' in xml, True)
        check("sizes are the real ones", 'Size="3"' in xml, True)

        sub = peer.files_xml("/warez/").decode("utf-8")
        check("a partial list is rooted where asked",
              'Base="/warez/"' in sub, True)
        check("and leaves the rest out", "B.MOD" in sub, False)

    print()
    if FAILED:
        print(f"{FAILED} check(s) failed")
        return 1
    print("ATTIC OK: the share index refuses what it cannot serve honestly.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
