#!/usr/bin/env python3
"""Measure what a MIDI run actually sounded like.

    test/dos/midirms.py FILE.WAV --expect-sound
    test/dos/midirms.py FILE.WAV --expect-silence
    test/dos/midirms.py FILE.WAV --expect-music

Nobody can listen to a scripted DOSBox-X run, so this is the listener: it
reads the audio FluidSynth rendered and checks it has the shape the test
program played -- a chord, a gap, a chord -- rather than merely containing
energy. "Some samples were not zero" would pass with a stuck DAC, a startup
beep, or a single click, and the failure this guards against is precisely a
MIDI path that is detected, configured, and silent.

Exit status is the verdict, so a rig can just call it.

--expect-sound is for test/dos/miditest.c's deliberate pattern: two held
chords with a gap, which is a shape no accident produces. --expect-music is
for a real game, where the shape is unknown but the music is continuous:
it asks for sustained, varying energy over most of the run rather than two
tidy blocks.

Thresholds are in 16-bit sample units (-32768..32767). The defaults come
from a measured run: an organ chord through MuseScore General sits near
1,000 RMS and the gaps between notes under 100. They are deliberately far
apart; a run that lands between them is reported rather than guessed at.
"""
import array
import sys
import wave

WINDOW_MS = 250
LOUD = 300          # a chord is ~1000; silence is 0
QUIET = 150         # the release tail of an organ, measured at 32-87
MIN_CHORD_MS = 750  # the program holds each chord for 1500ms
MIN_GAP_MS = 250    # and the gap between them is 1500ms

# --expect-music. Doom's title music measured 1,150-1,400 RMS without a
# break once it started, about 15 seconds into a cold boot.
MUSIC_MIN_MS = 10000    # at least this much audible music
MUSIC_MIN_SHARE = 50    # percent of the run after the first sound
MUSIC_MIN_SPREAD = 40   # loudest minus quietest, over the audible part


def envelope(path):
    w = wave.open(path)
    if w.getsampwidth() != 2:
        raise SystemExit(f"{path}: {w.getsampwidth() * 8}-bit audio; "
                         "this expects 16-bit (FluidSynth's default)")
    ch = w.getnchannels()
    rate = w.getframerate()
    a = array.array("h")
    a.frombytes(w.readframes(w.getnframes()))

    win = max(1, (rate * WINDOW_MS) // 1000) * ch
    out = []
    for i in range(0, len(a) - win + 1, win):
        s = a[i:i + win]
        rms = int((sum(v * v for v in s) / len(s)) ** 0.5)
        out.append((rms, max(abs(v) for v in s)))
    return out, rate, ch


def segments(env):
    """Runs of loud windows, as (start_index, length)."""
    out, start = [], None
    for i, (rms, _) in enumerate(env):
        if rms >= LOUD and start is None:
            start = i
        elif rms < QUIET and start is not None:
            out.append((start, i - start))
            start = None
    if start is not None:
        out.append((start, len(env) - start))
    return out


def main(argv):
    if len(argv) < 3:
        print(__doc__)
        return 2

    path = argv[1]
    want = argv[2]

    try:
        env, rate, ch = envelope(path)
    except FileNotFoundError:
        print(f"FAIL: {path} was never written -- FluidSynth produced no "
              "audio file at all")
        return 1

    if not env:
        print(f"FAIL: {path} holds no audio")
        return 1

    peak = max(p for _, p in env)
    loudest = max(r for r, _ in env)
    print(f"  {path}: {ch}ch {rate}Hz, {len(env) * WINDOW_MS / 1000:.1f}s, "
          f"loudest window {loudest} RMS, peak sample {peak}")

    for i, (rms, _) in enumerate(env):
        if rms:
            print(f"    {i * WINDOW_MS / 1000:5.2f}s  rms {rms:6d}  "
                  f"{'#' * min(40, rms // 40)}")

    segs = [(s, n) for s, n in segments(env)
            if n * WINDOW_MS >= MIN_CHORD_MS]

    if want == "--expect-silence":
        if loudest >= LOUD:
            print(f"FAIL: expected silence, got {loudest} RMS -- so the "
                  "sound in the other run cannot be attributed to the MIDI "
                  "notes")
            return 1
        print(f"  silent, as expected ({loudest} RMS at the loudest)")
        return 0

    if want == "--expect-music":
        audible = [r for r, _ in env if r >= LOUD]
        if not audible:
            print(f"FAIL: nothing audible at all (loudest {loudest} RMS, "
                  f"under {LOUD})")
            return 1

        first = next(i for i, (r, _) in enumerate(env) if r >= LOUD)
        after = len(env) - first
        share = (len(audible) * 100) // max(1, after)
        played = len(audible) * WINDOW_MS
        spread = max(audible) - min(audible)

        print(f"  music starts at {first * WINDOW_MS / 1000:.1f}s, "
              f"{played / 1000:.1f}s audible, {share}% of the run after "
              f"that, {min(audible)}-{max(audible)} RMS")

        if played < MUSIC_MIN_MS:
            print(f"FAIL: {played}ms of audible music, wanted "
                  f"{MUSIC_MIN_MS}ms -- a few notes is not a soundtrack")
            return 1
        if share < MUSIC_MIN_SHARE:
            print(f"FAIL: audible for only {share}% of the run after it "
                  f"started, wanted {MUSIC_MIN_SHARE}% -- that is a "
                  "stutter, not music")
            return 1
        if spread < MUSIC_MIN_SPREAD:
            print(f"FAIL: the level barely moves ({spread} RMS between "
                  "the quietest and loudest) -- a held tone or a stuck "
                  "voice, not a tune")
            return 1

        print("  continuous, varying music: a game's soundtrack")
        return 0

    if want != "--expect-sound":
        print(f"unknown expectation {want!r}")
        return 2

    if loudest < LOUD:
        print(f"FAIL: the loudest 250ms of this run is {loudest} RMS, under "
              f"the {LOUD} a chord measures. The MPU-401 was found and the "
              "notes went nowhere.")
        return 1

    if len(segs) < 2:
        print(f"FAIL: {len(segs)} sustained chord(s) of at least "
              f"{MIN_CHORD_MS}ms; the program plays two")
        return 1

    gap = segs[1][0] - (segs[0][0] + segs[0][1])
    if gap * WINDOW_MS < MIN_GAP_MS:
        print(f"FAIL: the two chords are not separated by a quiet gap "
              f"({gap * WINDOW_MS}ms, wanted {MIN_GAP_MS}ms) -- that is one "
              "continuous noise, not two notes being played and released")
        return 1

    print(f"  two chords of {segs[0][1] * WINDOW_MS}ms and "
          f"{segs[1][1] * WINDOW_MS}ms, {gap * WINDOW_MS}ms of quiet between "
          "them: the pattern the program played")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
