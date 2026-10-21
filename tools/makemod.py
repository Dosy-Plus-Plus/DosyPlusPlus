#!/usr/bin/env python3
"""Write a small original ProTracker module.

Two reasons this exists rather than a .MOD sitting in the repo:

  - Licensing. Every MOD worth listening to belongs to whoever tracked it in
    1993, and "it was on a BBS" is not a licence. This one is ours.
  - It is the test fixture. The notes and samples are known exactly, so the
    mixer can be checked against what it should produce rather than against
    a recording of itself.

The tune is deliberately plain: a square-wave bass, a short noise hit, and a
four-note figure that fits a 1994 file area. It uses only the effects
src/snd/mod.c actually implements (set volume, volume slide, set speed,
sample offset), so what plays is what was written -- mod_unsupported()
should report nothing for it.

Usage:
    tools/makemod.py art/dosypp.mod
    tools/makemod.py --test art/test.mod     # one note, for the unit tests
    tools/makemod.py --fx   art/fx.mod       # one row per pitch effect
"""
import struct
import sys


ROWS = 64
CHANNELS = 4

# Amiga periods, finetune 0, octave 2 (the one a MOD normally lives in).
PERIOD = {
    "C-2": 428, "C#2": 404, "D-2": 381, "D#2": 360, "E-2": 339, "F-2": 320,
    "F#2": 302, "G-2": 285, "G#2": 269, "A-2": 254, "A#2": 240, "B-2": 226,
    "C-3": 214, "D-3": 190, "E-3": 170, "F-3": 160, "G-3": 143, "A-3": 127,
    "C-1": 856, "D-1": 762, "E-1": 678, "F-1": 640, "G-1": 570, "A-1": 508,
}


def square(length, period_bytes, amplitude=100):
    """A square wave, one period every `period_bytes` bytes."""
    out = bytearray()
    half = max(1, period_bytes // 2)
    for i in range(length):
        out.append(amplitude if (i // half) % 2 == 0 else (256 - amplitude))
    return bytes(out)


def noise(length, seed=1994):
    """A short burst, for a hit. A deterministic LCG, so the file is
    reproducible and the tests can rely on it."""
    out = bytearray()
    state = seed
    for _ in range(length):
        state = (state * 1103515245 + 12345) & 0x7FFFFFFF
        v = (state >> 16) & 0xFF
        out.append(v if v < 128 else v - 256 & 0xFF)
    return bytes(out)


def ramp(length):
    """A sawtooth: a recognisable pitch with a brighter tone than square."""
    out = bytearray()
    for i in range(length):
        v = (i * 255 // max(1, length - 1)) - 128
        out.append(v & 0xFF)
    return bytes(out)


class Sample:
    def __init__(self, name, data, volume=64, loop_start=0, loop_len=0,
                 finetune=0):
        # Sample lengths are stored in words, so an odd byte is not
        # representable: pad rather than truncate, which would shorten a
        # loop by a byte and click.
        if len(data) % 2:
            data += b"\x00"
        self.name = name
        self.data = data
        self.volume = volume
        self.loop_start = loop_start
        self.loop_len = loop_len
        self.finetune = finetune

    def header(self):
        name = self.name.encode("ascii", "replace")[:22]
        name += b"\x00" * (22 - len(name))
        return (name
                + struct.pack(">H", len(self.data) // 2)
                + bytes([self.finetune & 0x0F, self.volume])
                + struct.pack(">H", self.loop_start // 2)
                + struct.pack(">H", self.loop_len // 2))


def note(sample=0, name=None, effect=0, param=0):
    """One cell of pattern data, in the format's four bytes."""
    period = PERIOD[name] if name else 0
    b0 = ((sample & 0xF0) | ((period >> 8) & 0x0F))
    b1 = period & 0xFF
    b2 = ((sample & 0x0F) << 4) | (effect & 0x0F)
    b3 = param & 0xFF
    return bytes([b0, b1, b2, b3])


EMPTY = note()


def pattern(rows):
    """rows is a list of up to 64 lists of up to 4 cells."""
    out = bytearray()
    for r in range(ROWS):
        cells = rows[r] if r < len(rows) else []
        for c in range(CHANNELS):
            out += cells[c] if c < len(cells) else EMPTY
    return bytes(out)


def build(name, samples, orders, patterns):
    out = bytearray()
    title = name.encode("ascii", "replace")[:20]
    out += title + b"\x00" * (20 - len(title))

    for i in range(31):
        if i < len(samples):
            out += samples[i].header()
        else:
            out += b"\x00" * 22 + struct.pack(">H", 0) + bytes([0, 0]) \
                   + struct.pack(">H", 0) + struct.pack(">H", 0)

    out += bytes([len(orders), 127])
    order_bytes = bytes(orders) + b"\x00" * (128 - len(orders))
    out += order_bytes
    out += b"M.K."

    for p in patterns:
        out += p

    for s in samples:
        out += s.data

    return bytes(out)


def tune():
    """The one that ships: a bass line, a hit and a lead figure."""
    bass = Sample("bass (square)", square(256, 64), volume=56,
                  loop_start=0, loop_len=256)
    lead = Sample("lead (saw)", ramp(128), volume=48,
                  loop_start=0, loop_len=128)
    hit = Sample("hit (noise)", noise(600), volume=64)

    samples = [bass, lead, hit]

    # Pattern 0: the figure. Bass on channel 1, lead on 2, hits on 4.
    rows = []
    bassline = ["C-1", None, "C-1", None, "G-1", None, "C-1", None,
                "F-1", None, "F-1", None, "C-1", None, "G-1", None]
    leadline = ["C-2", "E-2", "G-2", "E-2", "C-2", None, "G-2", None,
                "F-2", "A-2", "C-3", "A-2", "F-2", None, "C-2", None]

    for r in range(ROWS):
        cells = [EMPTY, EMPTY, EMPTY, EMPTY]
        step = r % 16

        if r == 0:
            # Set the tempo once, at the top: 6 ticks a row is the default
            # but saying so means the file does not depend on it.
            cells[0] = note(sample=1, name=bassline[0], effect=0xF, param=6)
        elif bassline[step] and r % 2 == 0:
            cells[0] = note(sample=1, name=bassline[step])

        if leadline[step] and r % 2 == 0:
            cells[1] = note(sample=2, name=leadline[step])

        # A hit on the half-bar, and a fade on the bar before it repeats.
        if step in (4, 12):
            cells[3] = note(sample=3, name="C-2")
        if r == ROWS - 2:
            cells[1] = note(effect=0xA, param=0x02)      # volume slide down

        rows.append(cells)

    pat0 = pattern(rows)

    # Pattern 1: the same figure a fourth up, so it goes somewhere.
    rows2 = []
    for r in range(ROWS):
        cells = [EMPTY, EMPTY, EMPTY, EMPTY]
        step = r % 16
        if bassline[step] and r % 2 == 0:
            up = {"C-1": "F-1", "G-1": "C-2", "F-1": "A#2"}
            cells[0] = note(sample=1, name=up.get(bassline[step],
                                                  bassline[step]))
        if leadline[step] and r % 2 == 0:
            up = {"C-2": "F-2", "E-2": "A-2", "G-2": "C-3",
                  "F-2": "A#2", "A-2": "D-3", "C-3": "F-3"}
            cells[1] = note(sample=2, name=up.get(leadline[step],
                                                  leadline[step]))
        if step in (0, 8):
            cells[3] = note(sample=3, name="C-2")
        rows2.append(cells)

    pat1 = pattern(rows2)

    return build("DOSy++ attic", samples, [0, 1, 0, 1], [pat0, pat1])


def test_module():
    """One channel, one sample, one note, then silence.

    For the unit tests: a square wave of a known period held for a known
    number of rows is something the mixer's output can be measured against
    -- count the zero crossings and you have the pitch it actually played.
    """
    # A 64-byte square wave: at period 428 the sample is clocked at
    # 3546895/428 = 8286 Hz, so one wave is 64/8286 s and the tone is about
    # 129 Hz. The test does that arithmetic itself.
    s = Sample("square 64", square(64, 64, amplitude=120), volume=64,
               loop_start=0, loop_len=64)

    rows = []
    for r in range(ROWS):
        cells = [EMPTY, EMPTY, EMPTY, EMPTY]
        if r == 0:
            cells[0] = note(sample=1, name="C-2", effect=0xF, param=6)
        elif r == 16:
            cells[0] = note(effect=0xC, param=0)         # volume to zero
        rows.append(cells)

    return build("test tone", [s], [0], [pattern(rows)])


def fx_module():
    """One row per pitch effect, each on the same note and sample.

    For the effect tests. Every row holds the note for its whole length and
    does one thing to it, so the mixer's output for that row can be
    measured against what the effect is supposed to do -- count the zero
    crossings in a window and you have the pitch it actually played. The
    arithmetic is in the test, not here.

    Rows (speed 6, so each row is six ticks):

      0  C-2, no effect          the baseline pitch
      1  C-2, 1 10              portamento up: period 428 -> 348
      2  C-2, 2 10              portamento down: 428 -> 508
      3  C-2, 0 47              arpeggio: the note, +4, +7 semitones
      4  C-2, 4 88              vibrato: speed 8, depth 8
      5  C-1 (period 856)       a low note, to slide away from
      6  C-2, 3 20              tone portamento towards 428 at 32/tick
      7  C-2, 7 84              tremolo: speed 8, depth 4
      8  C-2, no effect          back to the baseline, unbent
    """
    # A long square wave so the pitch is the note's, not the sample's: 64
    # bytes at period 428 is 129 Hz, and the test counts crossings.
    s = Sample("square 64", square(64, 64, amplitude=120), volume=64,
               loop_start=0, loop_len=64)

    rows = []
    for r in range(ROWS):
        cells = [EMPTY, EMPTY, EMPTY, EMPTY]
        if r == 0:
            cells[0] = note(sample=1, name="C-2", effect=0xF, param=6)
        elif r == 1:
            cells[0] = note(sample=1, name="C-2", effect=0x1, param=0x10)
        elif r == 2:
            cells[0] = note(sample=1, name="C-2", effect=0x2, param=0x10)
        elif r == 3:
            cells[0] = note(sample=1, name="C-2", effect=0x0, param=0x47)
        elif r == 4:
            cells[0] = note(sample=1, name="C-2", effect=0x4, param=0x88)
        elif r == 5:
            cells[0] = note(sample=1, name="C-1")
        elif r == 6:
            # A note with effect 3 is a destination, not a new note: the
            # channel slides to it from C-1 without restarting the sample.
            cells[0] = note(sample=1, name="C-2", effect=0x3, param=0x20)
        elif r == 7:
            cells[0] = note(sample=1, name="C-2", effect=0x7, param=0x84)
        elif r == 8:
            cells[0] = note(sample=1, name="C-2")
        elif r == 9:
            cells[0] = note(effect=0xC, param=0)        # silence
        rows.append(cells)

    return build("effect tests", [s], [0], [pattern(rows)])


def main(argv):
    want_test = "--test" in argv
    want_fx = "--fx" in argv
    args = [a for a in argv[1:] if not a.startswith("--")]

    if not args:
        print(__doc__)
        return 2

    if want_fx:
        data = fx_module()
    elif want_test:
        data = test_module()
    else:
        data = tune()
    with open(args[0], "wb") as f:
        f.write(data)

    print("wrote %s (%d bytes)" % (args[0], len(data)))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
