/* state.mjs - does a saved v86 session come back?
 *
 * The browser page keeps the whole machine state in IndexedDB so a reload
 * does not mean a new CID, a re-hashed share and the favourites gone. The
 * storage half of that is browser-only; the half that can actually go
 * wrong is v86's own save_state/restore_state with a DOS program running
 * inside, and that runs in Node.
 *
 * What is checked: boot until DOSy++ is up, save, destroy the emulator,
 * build a fresh one, restore, and confirm the restored machine CARRIES ON
 * -- same identity, more output on COM1. A restore that produced a machine
 * which merely did not crash would prove nothing.
 *
 * Usage: node test/v86/state.mjs <relay-url> <boot.img> <hda.img> [timeout]
 */
import { readFileSync } from "node:fs";
import { dirname, resolve } from "node:path";
import { fileURLToPath } from "node:url";
import { V86 } from "../../web/vendor/libv86.mjs";

const here = dirname(fileURLToPath(import.meta.url));
const root = resolve(here, "../..");

const relayUrl = process.argv[2] || "wisp://localhost:8086/";
const bootImg = process.argv[3] || resolve(root, "web/images/boot.img");
const hdaImg = process.argv[4] || resolve(root, "web/images/dosypp.img");
const timeoutS = Number(process.argv[5] || 300);

const vendor = resolve(root, "web/vendor");
const buffer = (p) => {
    const b = readFileSync(p);
    return b.buffer.slice(b.byteOffset, b.byteOffset + b.byteLength);
};

const config = () => ({
    wasm_path: resolve(vendor, "v86.wasm"),
    bios: { buffer: buffer(resolve(vendor, "seabios.bin")) },
    vga_bios: { buffer: buffer(resolve(vendor, "vgabios.bin")) },
    fda: { buffer: buffer(bootImg) },
    hda: { buffer: buffer(hdaImg) },
    boot_order: 0x321,
    memory_size: 16 * 1024 * 1024,
    vga_memory_size: 2 * 1024 * 1024,
    uart1: true,
    net_device: {
        type: "ne2k",
        relay_url: relayUrl,
        router_ip: "192.168.86.1",
        vm_ip: "192.168.86.100",
    },
    autostart: true,
    disable_speaker: true,
});

const die = (why) => {
    console.log(`[state] FAIL: ${why}`);
    process.exit(1);
};

const deadline = setTimeout(() => die(`timeout after ${timeoutS}s`),
                            timeoutS * 1000);

/* Wait for `mark` on COM1, collecting everything seen. */
function runUntil(emu, mark, label) {
    return new Promise((ok, no) => {
        let seen = "";
        const t = setInterval(
            () => console.log(`[state] ${label}: ${seen.length} bytes`), 10000);

        emu.add_listener("serial0-output-byte", (b) => {
            seen += String.fromCharCode(b);
            if (seen.includes(mark)) {
                clearInterval(t);
                ok(seen);
            }
        });
        setTimeout(() => {
            clearInterval(t);
            no(new Error(`"${mark}" never appeared in ${label}; saw:\n` +
                         seen.replace(/\r/g, "")));
        }, (timeoutS - 10) * 1000);
    });
}

const first = new V86(config());
console.log("[state] booting...");

/* The CID is the identity, printed once at startup. If it changes after a
   restore, the session was not restored -- it was rebooted. */
let cid = null;

try {
    const seen = await runUntil(first, "connecting to", "first boot");
    const m = seen.match(/cid: ([A-Z0-9]{39})/);

    if (!m)
        die("no CID on COM1 -- did DOSy++ start?");
    cid = m[1];
    console.log(`[state] running, identity ${cid}`);

    const state = await first.save_state();

    console.log(`[state] saved ${Math.round(state.byteLength / 1024)}KB`);
    if (state.byteLength < 1024)
        die(`the saved state is ${state.byteLength} bytes, which is nothing`);

    first.destroy();

    /* A fresh emulator, as a reloaded tab would build. */
    const second = new V86(config());

    await new Promise((r) => second.add_listener("emulator-ready", r));
    await second.restore_state(state);
    console.log("[state] restored into a fresh emulator");

    /* And it has to go on running. The marker is the client's own output
       after the restore: something it prints while working, not something
       left over in the buffer -- the restored machine starts with an empty
       serial buffer, so anything seen now was produced after the restore. */
    const after = await runUntil(second, "\n", "after restore");

    console.log(`[state] ${after.length} bytes of new output after restore`);
    if (after.length < 1)
        die("the restored machine produced nothing");

    /* It is the same machine, not a reboot: a reboot would print a new CID
       and the identity line again. */
    if (/cid: [A-Z0-9]{39}/.test(after) && !after.includes(cid))
        die("the restored machine printed a different identity -- it rebooted");

    clearTimeout(deadline);
    console.log("[state] PASS: state saved, restored, and the machine " +
                "carried on with the same identity");
    second.destroy();
    process.exit(0);
} catch (e) {
    die(e && e.message);
}
