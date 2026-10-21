#!/usr/bin/env python3
"""Build a share of a given total size, cheaply.

Why this exists: the share total a client advertises is a 64-bit number,
and the code that prints it was 32 bits for a long time -- a 5GB share told
the hub it had 700MB. Testing the fix needs a share bigger than 4GB, and
nobody should have to find 5GB of files, or wait while 5GB is written.

So the files are sparse: a unique 512-byte header, then a hole. On APFS and
on ext4 the hole costs nothing on disk, reads back as zeros at memory
speed, and is a perfectly ordinary file to everything that opens it --
including DOSBox-X's host-directory mount and our own scanner.

The header is what stops this being a test of one hash repeated: files of
equal size made of nothing but zeros have the same TTH, which would exercise
duplicate-hash handling rather than share arithmetic.

The names are 8.3 and the layout is what a 1994 board looked like, because
the share door is a thing people look at.

    tools/mkbigshare.py /tmp/bigshare 5G
    tools/mkbigshare.py /tmp/bigshare 5G --manifest files.txt
    tools/mkbigshare.py /tmp/share 200M --real build/atmos   # real files

With --real, whatever tools/fetchatmos.sh fetched goes in first, with its
own names and its own bytes, and the filler makes up the difference. The
share then has real DOS software at the top of every area -- which is what
somebody looking at the share door sees -- and the bulk costs nothing.
"""
import argparse
import os
import sys

# A warez board's file areas, and the sort of thing that sat in them. The
# sizes are deliberately mixed: a share of forty identical files would not
# exercise the index the way a real one does.
# A warez board's file areas, and plausible-looking contents.
#
# The names are deliberately invented. An earlier version called the filler
# DOOM19S.ZIP, DESCENT1.ZIP and WOLF3D14.ZIP, which was atmospheric right up
# until you noticed that a share door was listing commercial games this
# repository does not have and would not ship -- a sparse file of zeros
# wearing a product's name is worse than no atmosphere at all.
#
# The real names come from the real software, which tools/fetchatmos.sh
# fetches and --real lays in on top of this.
#
# Sizes are mixed on purpose: a share of forty identical files does not
# exercise the index the way a real one does.
AREAS = [
    ("WAREZ/APPS", [
        ("ARCPACK.EXE", 200_000),
        ("DSKTOOLS.EXE", 300_000),
        ("MEMOPT22.ZIP", 120_000),
        ("TERMPRO3.ZIP", 1_200_000),
        ("BBSPACK1.ZIP", 2_400_000),
        ("EDITSUIT.ZIP", 8_600_000),
        ("DTPDEMO1.ZIP", 42_000_000),
    ]),
    ("WAREZ/GAMES", [
        ("MAZERUN2.ZIP", 2_400_000),
        ("SPACEWAR.ZIP", 700_000),
        ("CARDPACK.ZIP", 420_000),
        ("DUNGEON5.ZIP", 18_000_000),
        ("FLIGHTSM.ZIP", 23_000_000),
        ("CITYBLD2.ZIP", 96_000_000),
        ("RACER3DX.ZIP", 128_000_000),
    ]),
    ("WAREZ/ISO", [
        ("CDROM01.ISO", 650_000_000),
        ("CDROM02.ISO", 650_000_000),
        ("CDROM03.ISO", 650_000_000),
        ("CDROM04.ISO", 650_000_000),
    ]),
    ("MODS", [
        ("TUNE0001.MOD", 290_000),
        ("CHIPSET2.MOD", 180_000),
        ("TRACKER.S3M", 640_000),
        ("LONGPLAY.XM", 1_100_000),
    ]),
    ("NFO", [
        ("FILE_ID.DIZ", 400),
        ("BOARD.NFO", 2_300),
        ("UPLOADS.NFO", 1_800),
    ]),
    ("", [
        ("README.TXT", 3_000),
        ("RATIO.TXT", 1_200),
    ]),
]


def parse_size(text):
    """"5G", "500M", "1234" -> bytes."""
    mult = 1
    t = text.upper().strip()
    if t.endswith("K"):
        mult, t = 1024, t[:-1]
    elif t.endswith("M"):
        mult, t = 1024 * 1024, t[:-1]
    elif t.endswith("G"):
        mult, t = 1024 * 1024 * 1024, t[:-1]
    return int(float(t) * mult)


def make_file(path, size, serial):
    """A unique header, then a hole.

    The header is 512 bytes so it lands in the first TTH leaf and nowhere
    else: every file's hash differs, and the rest of the file costs no disk
    and no read time.
    """
    os.makedirs(os.path.dirname(path) or ".", exist_ok=True)
    header = ("DOSYPP FILLER %08d " % serial).encode("ascii")
    header = (header * (512 // len(header) + 1))[:min(512, size)]

    with open(path, "wb") as f:
        f.write(header)
        if size > len(header):
            # The hole. truncate() past the end is what makes it sparse;
            # writing zeros would cost 5GB of disk and an hour of someone's
            # afternoon.
            f.truncate(size)


def copy_real(real_dir, root, manifest):
    """Lay real archives into the share, keeping their areas and names.

    Copied rather than linked: the share is something a client hashes and
    serves, and a symlink into a cache directory is a different thing to
    test. 47MB of copying is not worth being clever about.
    """
    import shutil

    total = 0
    count = 0

    for dirpath, _dirs, names in os.walk(real_dir):
        rel_dir = os.path.relpath(dirpath, real_dir)
        if rel_dir == ".":
            rel_dir = ""
        for name in sorted(names):
            src = os.path.join(dirpath, name)
            size = os.path.getsize(src)
            if size == 0:
                continue
            rel = os.path.join(rel_dir, name) if rel_dir else name
            dst = os.path.join(root, rel)
            os.makedirs(os.path.dirname(dst) or ".", exist_ok=True)
            shutil.copyfile(src, dst)
            manifest.append((size, rel))
            total += size
            count += 1

    return count, total


def main(argv):
    ap = argparse.ArgumentParser(
        description="build a sparse share of a given total size")
    ap.add_argument("root")
    ap.add_argument("size", help='total, e.g. "5G"')
    ap.add_argument("--real", default=None, metavar="DIR",
                    help="real files to include first (tools/fetchatmos.sh)")
    ap.add_argument("--manifest", default=None,
                    help="write <size> <path> per line, for a test to check")
    ap.add_argument("--quiet", action="store_true")
    args = ap.parse_args(argv[1:])

    want = parse_size(args.size)
    total = 0
    count = 0
    serial = 0
    rounds = 0
    manifest = []

    os.makedirs(args.root, exist_ok=True)

    # The real software first, so it is what the share door shows and what
    # a browse opens on. The filler is behind it.
    if args.real and os.path.isdir(args.real):
        n, bytes_in = copy_real(args.real, args.root, manifest)
        count += n
        total += bytes_in
        if not args.quiet:
            print("%d real files, %d bytes from %s" % (n, bytes_in,
                                                       args.real))
    elif args.real:
        print("mkbigshare: %s does not exist -- run tools/fetchatmos.sh"
              % args.real, file=sys.stderr)
        return 1

    # Round after round of the template until the total is reached, with a
    # generation suffix so the names stay unique and 8.3.
    while total < want:
        for area, files in AREAS:
            for name, size in files:
                if total >= want:
                    break
                if rounds > 0:
                    # Unique by the serial, not by the round: taking the
                    # first five characters and appending a round number
                    # turned CDROM01.ISO..CDROM04.ISO into four copies of
                    # one name, each truncating the last. The share then
                    # had three files fewer than the manifest claimed and
                    # the client was blamed for it.
                    stem, dot, ext = name.rpartition(".")
                    stem = (stem or name)[:4] + "%04d" % serial
                    name = stem + dot + ext

                # Never past what the client will touch: a file of 2GB or
                # more cannot be addressed by a 32-bit seek, and the client
                # refuses one. A share made of them would test the refusal,
                # not the total.
                size = min(size, want - total, 2_000_000_000)
                if size <= 0:
                    continue

                rel = os.path.join(area, name) if area else name
                path = os.path.join(args.root, rel)

                # Never over a real file: the README that came with the
                # fetched software explains where it is from, and filler
                # should not replace it.
                if os.path.exists(path):
                    continue

                make_file(path, size, serial)
                manifest.append((size, rel))
                total += size
                count += 1
                serial += 1
        rounds += 1
        if rounds > 200:
            break

    if args.manifest:
        with open(args.manifest, "w") as f:
            for size, rel in manifest:
                f.write("%d %s\n" % (size, rel))

    # What is actually on disk, against what was asked for: a name
    # collision would otherwise be invisible until something counted the
    # files and disagreed.
    on_disk = 0
    for dirpath, _dirs, names in os.walk(args.root):
        on_disk += len(names)
    if on_disk != count:
        print("mkbigshare: wrote %d files but made %d names -- a collision"
              % (on_disk, count), file=sys.stderr)
        return 1

    if not args.quiet:
        gb = total / float(1024 * 1024 * 1024)
        print("%s: %d files, %d bytes (%.2f GB)" % (args.root, count, total,
                                                    gb))
        print("  apparent size on disk: %s" % os.popen(
            "du -sh '%s' 2>/dev/null | cut -f1" % args.root).read().strip())
    print(total)
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
