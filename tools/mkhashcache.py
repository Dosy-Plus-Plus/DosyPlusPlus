#!/usr/bin/env python3
"""Write a DOSYPP.TTH that a DOS build will accept.

The hash cache is keyed on the virtual path, the size and the modification
time -- and the two platforms record that time differently. The host build
stores st_mtime, Unix epoch seconds; the DOS build stores the packed FAT
fields the directory entry carries, (date << 16) | time. A cache written on
one is therefore ignored by the other, and the files get hashed again.

That matters for exactly one job: testing a share too big to hash on the
target. Five gigabytes at 500 KB/s is three hours of emulator time, and the
thing under test is the share total, not the hasher. So the hashes are
computed on the host, where it takes half a minute, and the timestamps are
translated here.

The translation is confirmed against the target rather than assumed: a DOS
run over one file touched to 1994-01-01 00:00 recorded 471924736, which is
what this computes.

    tools/mkhashcache.py --share /tmp/big/DL --from-cache /tmp/big/DOSYPP.TTH \\
                         --out /tmp/dosrun/DOSYPP.TTH
"""
import argparse
import os
import sys
import time


def dos_mtime(path):
    """The packed FAT date and time a DOS directory entry would carry.

    Local time, because that is what the host's filesystem records and what
    DOSBox-X hands to the guest. Seconds have two-second granularity in FAT
    -- the field holds seconds/2 -- which is why a timestamp set to an odd
    second comes back even.
    """
    t = time.localtime(os.path.getmtime(path))

    if t.tm_year < 1980:
        raise ValueError("%s predates FAT (1980)" % path)

    date = ((t.tm_year - 1980) << 9) | (t.tm_mon << 5) | t.tm_mday
    tm = (t.tm_hour << 11) | (t.tm_min << 5) | (t.tm_sec // 2)
    return (date << 16) | tm


def main(argv):
    ap = argparse.ArgumentParser(
        description="translate a host hash cache for a DOS build")
    ap.add_argument("--share", required=True,
                    help="the share root, as the host sees it")
    ap.add_argument("--from-cache", required=True,
                    help="DOSYPP.TTH written by the host build")
    ap.add_argument("--out", required=True)
    args = ap.parse_args(argv[1:])

    root_name = os.path.basename(os.path.normpath(args.share))
    written = 0
    skipped = []

    with open(args.from_cache) as src, open(args.out, "w", newline="\n") as out:
        out.write("; DOSy++ hash cache, translated for DOS by "
                  "tools/mkhashcache.py\n")
        out.write("; <tth> <size> <mtime> <path>\n")

        for line in src:
            if line.startswith(";") or not line.strip():
                continue
            parts = line.split(None, 3)
            if len(parts) != 4:
                continue
            tth, size, _host_mtime, vpath = parts
            vpath = vpath.rstrip("\r\n")

            # The virtual path starts with the share root's own name, and
            # the rest is relative to it.
            if not vpath.startswith(root_name + "/"):
                skipped.append(vpath)
                continue
            rel = vpath[len(root_name) + 1:]
            local = os.path.join(args.share, rel)

            if not os.path.exists(local):
                skipped.append(vpath)
                continue

            out.write("%s %s %d %s\n"
                      % (tth, size, dos_mtime(local), vpath))
            written += 1

    print("wrote %s: %d entries" % (args.out, written))
    if skipped:
        print("skipped %d entries with no file: %s"
              % (len(skipped), ", ".join(skipped[:3])), file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
