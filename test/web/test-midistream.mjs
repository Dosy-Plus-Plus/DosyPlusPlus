/* Unit tests for web/midi.mjs's stream reassembly.
 *
 *   node test/web/test-midistream.mjs
 *
 * The byte stream a DOS game writes to an MPU-401 is not a list of tidy
 * three-byte messages, and each way it is untidy loses something real if
 * mishandled: running status is most of the notes in a piece of music,
 * realtime bytes arrive in the middle of other messages, and sysex is how
 * a game uploads the MT-32 patches that made it sound like 1994.
 */
import { MidiStream } from "../../web/midi.mjs";

let failed = 0;

const hex = (a) => [...a].map((b) => b.toString(16).padStart(2, "0")).join(" ");

function run(name, bytes, wantMessages, check) {
    const s = new MidiStream();
    const got = [];
    for (const b of bytes)
        got.push(...s.feed(b));

    const g = got.map(hex).join(" | ");
    const w = wantMessages.map((m) => hex(new Uint8Array(m))).join(" | ");

    if (g === w) {
        console.log(`  ok    ${name}`);
    } else {
        console.log(`  FAIL  ${name}\n          got  ${g}\n          want ${w}`);
        failed++;
    }
    if (check) check(s, name);
}

function eq(name, got, want) {
    if (got === want) {
        console.log(`  ok    ${name}`);
    } else {
        console.log(`  FAIL  ${name}: got ${got}, wanted ${want}`);
        failed++;
    }
}

console.log("the ordinary cases:");
run("a note on", [0x90, 0x3C, 0x64], [[0x90, 0x3C, 0x64]]);
run("a program change is two bytes, not three", [0xC0, 0x13], [[0xC0, 0x13]]);
run("a controller", [0xB0, 0x07, 0x7F], [[0xB0, 0x07, 0x7F]]);
run("nothing until the message is complete", [0x90, 0x3C], []);

console.log("running status, which is most of the notes in a piece of music:");
run("three chords' worth of bare data pairs",
    [0x90, 0x3C, 0x64, 0x40, 0x64, 0x43, 0x64],
    [[0x90, 0x3C, 0x64], [0x90, 0x40, 0x64], [0x90, 0x43, 0x64]]);
/* A new channel message replaces the running status, and every data byte
   after it starts another message OF THAT KIND -- so two bytes after a
   program change are two more program changes, not one. Written down
   because the first version of this test expected two messages and the
   parser was right: this is what "running status" means, and a game
   switching patches in a hurry really does produce it. */
run("a new channel message becomes the running status",
    [0x90, 0x3C, 0x64, 0xC0, 0x13, 0x3C, 0x00],
    [[0x90, 0x3C, 0x64], [0xC0, 0x13], [0xC0, 0x3C], [0xC0, 0x00]]);

console.log("realtime bytes, which interrupt anything:");
run("a clock in the middle of a note on",
    [0x90, 0x3C, 0xF8, 0x64],
    [[0xF8], [0x90, 0x3C, 0x64]]);
run("active sensing between notes",
    [0x90, 0x3C, 0x64, 0xFE, 0x40, 0x64],
    [[0x90, 0x3C, 0x64], [0xFE], [0x90, 0x40, 0x64]]);

console.log("sysex, which is how an MT-32 gets its patches:");
run("a complete sysex",
    [0xF0, 0x41, 0x10, 0x16, 0x12, 0xF7],
    [[0xF0, 0x41, 0x10, 0x16, 0x12, 0xF7]]);
run("a sysex abandoned by a status byte",
    [0xF0, 0x41, 0x10, 0x90, 0x3C, 0x64],
    [[0x90, 0x3C, 0x64]]);
run("sysex cancels running status",
    [0x90, 0x3C, 0x64, 0xF0, 0x41, 0xF7, 0x40, 0x64],
    [[0x90, 0x3C, 0x64], [0xF0, 0x41, 0xF7]]);

console.log("system common, which cancels running status:");
run("song select, then stray data",
    [0x90, 0x3C, 0x64, 0xF3, 0x02, 0x40, 0x64],
    [[0x90, 0x3C, 0x64], [0xF3, 0x02]]);
run("tune request has no data bytes at all",
    [0xF6], [[0xF6]]);

console.log("rubbish, which must not be mistaken for music:");
run("data with no status to belong to", [0x3C, 0x64], []);

console.log("what the page shows:");
{
    const s = new MidiStream();
    for (const b of [0xC0, 0x13, 0x90, 0x3C, 0x64, 0x40, 0x64, 0x43, 0x00,
                     0xF0, 0x41, 0xF7])
        s.feed(b);
    eq("notes counted", s.notes, 2);          /* 0x43 has velocity 0 */
    eq("a zero-velocity note on is not a note", s.notes, 2);
    eq("sysex counted", s.sysexes, 1);
    eq("messages counted", s.messages, 5);
    eq("nothing stray", s.dropped, 0);
    console.log(`  summary: ${s.summary()}`);
}

console.log();
if (failed) {
    console.log(`${failed} check(s) failed`);
    process.exit(1);
}
console.log("MIDI STREAM OK: running status, realtime and sysex all survive.");
