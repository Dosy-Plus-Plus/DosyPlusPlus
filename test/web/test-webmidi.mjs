/* Unit tests for web/webmidi.mjs, the sink that sends a DOS game's music
 * to a synthesiser the visitor already owns.
 *
 *   node test/web/test-webmidi.mjs
 *
 * Driven by a stub in place of navigator.requestMIDIAccess, which is why
 * the module takes it as an argument. What this covers is every decision
 * the module makes -- which device, which messages, what happens when
 * permission is half granted or the device is unplugged mid-tune -- and
 * what it cannot cover is a browser's own Web MIDI implementation. Safari
 * has none; Firefox and Chrome may ask the visitor.
 */
import { WebMidiOut, available } from "../../web/webmidi.mjs";

let failed = 0;

const eq = (what, got, want) => {
    if (got === want) {
        console.log(`  ok    ${what}`);
    } else {
        console.log(`  FAIL  ${what}: got ${JSON.stringify(got)}, wanted `
                    + JSON.stringify(want));
        failed++;
    }
};

/* A MIDIOutput that records, and can be told to misbehave. */
function output(id, name) {
    return {
        id,
        name,
        sent: [],
        throws: false,
        send(data) {
            if (this.throws)
                throw new TypeError("device is not open");
            this.sent.push([...data].map((b) => b.toString(16)
                .padStart(2, "0")).join(" "));
        },
    };
}

/* A stub navigator.requestMIDIAccess. `sysex` says whether the browser
   grants the sysex permission; `fail` makes the whole thing refuse. */
function stub(outs, { sysex = true, fail = false } = {}) {
    const access = {
        outputs: new Map(outs.map((o) => [o.id, o])),
        onstatechange: null,
    };
    const request = async (opts) => {
        if (fail)
            throw new Error("permission denied");
        if (opts && opts.sysex && !sysex)
            throw new Error("sysex not permitted");
        access.sysex = !!(opts && opts.sysex);
        return access;
    };
    return { request, access };
}

const NOTE = new Uint8Array([0x90, 0x3C, 0x64]);
const SYSEX = new Uint8Array([0xF0, 0x41, 0x10, 0xF7]);

console.log("is there any Web MIDI here:");
eq("a navigator without it", available({}), false);
eq("no navigator at all", available(undefined), false);
eq("one with it", available({ requestMIDIAccess: () => {} }), true);

console.log("choosing a device:");
{
    const a = output("out-1", "SC-55");
    const b = output("out-2", "Microsoft GS Wavetable Synth");
    const s = stub([a, b]);
    const m = new WebMidiOut(s.request);

    const list = await m.open();
    eq("both outputs listed", list.map((o) => o.name).join(", "),
       "SC-55, Microsoft GS Wavetable Synth");
    eq("sysex was granted", m.sysexAllowed, true);
    eq("nothing selected yet", m.send(NOTE), false);
    eq("choosing by id returns its name", m.use("out-2"),
       "Microsoft GS Wavetable Synth");
    eq("a name for an id that is not there", m.use("nope"), null);
    eq("and it says so", m.lastError, "no MIDI output with id nope");

    m.use("out-1");
    /* Deselecting out-2 legitimately sent it all-notes-off -- that is the
       behaviour the next block tests on purpose -- so what matters here is
       that the NOTE went only to the device now selected. */
    b.sent = [];
    m.send(NOTE);
    eq("the note went to the chosen device", a.sent.join(""), "90 3c 64");
    eq("and only there", b.sent.length, 0);
}

console.log("switching device mid-tune, which must not leave a note on:");
{
    const a = output("a", "A");
    const b = output("b", "B");
    const m = new WebMidiOut(stub([a, b]).request);
    await m.open();
    m.use("a");
    m.send(NOTE);
    a.sent = [];
    m.use("b");
    eq("all-notes-off went to the device being left", a.sent.length, 32);
    eq("it is the right message", a.sent[0], "b0 7b 00");
    eq("and the sustain-ish one too", a.sent[1], "b0 78 00");
}

console.log("sysex, which a browser may refuse on its own:");
{
    const o = output("o", "MT-32");
    const m = new WebMidiOut(stub([o], { sysex: false }).request);
    await m.open();
    m.use("o");

    eq("access was granted anyway", !!m.access, true);
    eq("but not for sysex", m.sysexAllowed, false);
    eq("a note still goes", m.send(NOTE), true);
    eq("the sysex does not", m.send(SYSEX), false);
    eq("it was counted, not silently dropped", m.sysexDropped, 1);
    eq("and the device never saw it", o.sent.join("|"), "90 3c 64");
    eq("the summary says so",
       m.summary().includes("1 sysex dropped (not permitted)"), true);
}

console.log("with sysex granted:");
{
    const o = output("o", "MT-32");
    const m = new WebMidiOut(stub([o], { sysex: true }).request);
    await m.open();
    m.use("o");
    eq("the patch upload goes through", m.send(SYSEX), true);
    eq("the device got it", o.sent.join("|"), "f0 41 10 f7");
}

console.log("a device that refuses what it is sent:");
{
    const o = output("o", "Broken");
    const m = new WebMidiOut(stub([o]).request);
    await m.open();
    m.use("o");
    o.throws = true;
    eq("send reports failure", m.send(NOTE), false);
    eq("counted", m.failed, 1);
    eq("with the reason kept", m.lastError, "device is not open");
}

console.log("a device unplugged mid-tune:");
{
    const o = output("o", "SC-55");
    const s = stub([o]);
    const m = new WebMidiOut(s.request);
    let notified = null;
    m.onoutputs = (l) => { notified = l.length; };
    await m.open();
    m.use("o");

    s.access.outputs.delete("o");
    s.access.onstatechange();

    eq("the page was told", notified, 0);
    eq("nothing is selected now", m.output, null);
    eq("and it says what happened", m.lastError, "SC-55 went away");
    eq("sending is a no-op rather than a throw", m.send(NOTE), false);
}

console.log("no Web MIDI, and refused permission:");
{
    const m = new WebMidiOut(null);
    let why = "";
    try { await m.open(); } catch (e) { why = e.message; }
    eq("it explains itself", why.includes("no Web MIDI"), true);

    const d = new WebMidiOut(stub([], { fail: true }).request);
    let denied = "";
    try { await d.open(); } catch (e) { denied = e.message; }
    eq("a denial surfaces", denied, "permission denied");
}

console.log();
if (failed) {
    console.log(`${failed} check(s) failed`);
    process.exit(1);
}
console.log("WEBMIDI OK: the right bytes to the right device, sysex "
            + "accounted for,");
console.log("and nothing left holding a note.");
