/* inpage.mjs - the whole demo with no server at all.
 *
 *   node test/v86/inpage.mjs <boot.img> <hda.img|manifest> [seconds]
 *
 * Boots the browser host with the relay, the hub and the serving peer all
 * inside the process: web/wisp.mjs stands in for the WebSocket, and
 * web/adchub.mjs answers as the hub and as the node with the files. No
 * TCP, no relay, nothing listening on any port.
 *
 * It is the same code the page runs, which is the point of writing it in
 * JavaScript: this rig is not a model of the in-page demo, it is the
 * in-page demo with Node where the browser would be.
 *
 * The verdict is the client's own: a search that finds the game, a
 * download that completes, and -- because the client only reports a
 * download complete after the TTH matches -- a hash verified against the
 * one the hub advertised.
 */
import { readFileSync, existsSync } from "node:fs";
import { dirname, resolve, join } from "node:path";
import { fileURLToPath } from "node:url";
import { V86 } from "../../web/vendor/libv86.mjs";
import { WispServer, installInPageSocket } from "../../web/wisp.mjs";
import { AdcHub, parseShare } from "../../web/adchub.mjs";
import { installWorkerShim, partsConfig } from "./workershim.mjs";

const here = dirname(fileURLToPath(import.meta.url));
const root = resolve(here, "../..");
const vendor = resolve(root, "web/vendor");

const bootImg = process.argv[2] || resolve(root, "web/images/boot.img");
const hdaArg = process.argv[3] || resolve(root, "web/images/dosypp.img");
const seconds = Number(process.argv[4] || 240);
const shareDir = process.env.WAREZ_DIR || resolve(root, "build/warez");

const buffer = (p) => {
    const b = readFileSync(p);
    return b.buffer.slice(b.byteOffset, b.byteOffset + b.byteLength);
};

/* ----------------------------------------------------------- the hub */

const shareText = readFileSync(join(shareDir, "SHARE.TXT"), "utf8");
const { files, problems } = parseShare(shareText);

if (problems.length) {
    console.log("SHARE.TXT problems:");
    for (const p of problems) console.log(`  ${p}`);
}
if (!files.length) {
    console.log("FAIL: nothing to share -- run: make warez");
    process.exit(1);
}

console.log(`[hub] ${files.length} file(s) in the share, `
            + `${(files.reduce((n, f) => n + f.size, 0) / 1048576).toFixed(1)}MB`);

const hub = new AdcHub({
    share: files,
    /* In the page this is a ranged fetch; here it is a read. Same shape,
       which is why it is injected. */
    fetchFile: async (file, start, count) => {
        const path = join(shareDir, file.name);
        const bytes = readFileSync(path);
        return bytes.subarray(start, start + count);
    },
});

const log = [];
const logins = [];
hub.onLog = (m) => log.push(m);
hub.onEvent = (kind, detail) => {
    if (kind === "login")
        logins.push(detail);
    console.log(`[hub] ${kind}: ${JSON.stringify(detail)}`);
};

const server = new WispServer(hub.listeners());
server.onLog = (m) => console.log(`[wisp] ${m}`);
installInPageSocket(server);

/* ------------------------------------------------------- the machine */

const manifest = hdaArg.endsWith(".json")
    ? JSON.parse(readFileSync(hdaArg, "utf8")) : null;
if (manifest)
    installWorkerShim();

console.log(`[harness] no relay, no hub process, no open ports`);

const emulator = new V86({
    wasm_path: resolve(vendor, "v86.wasm"),
    bios: { buffer: buffer(resolve(vendor, "seabios.bin")) },
    vga_bios: { buffer: buffer(resolve(vendor, "vgabios.bin")) },
    fda: { buffer: buffer(bootImg) },
    hda: manifest
        ? partsConfig(manifest, dirname(hdaArg))
        : { buffer: buffer(hdaArg) },
    boot_order: 0x321,
    memory_size: 16 * 1024 * 1024,
    vga_memory_size: 2 * 1024 * 1024,
    uart1: true,
    /* The URL the shim answers for. Nothing opens a socket. */
    net_device: {
        type: "ne2k",
        relay_url: "wisp://in-page/",
        router_ip: "192.168.86.1",
        vm_ip: "192.168.86.100",
    },
    autostart: true,
    disable_speaker: true,
});

let serial = "";
let settled = false;

/* A machine that does not boot is the hardest thing to debug here, so
   anything the emulator complains about is printed rather than lost. */
process.on("uncaughtException", (e) => {
    console.log(`[harness] uncaught: ${e && e.stack}`);
});
emulator.add_listener("emulator-ready", () =>
    console.log("[harness] emulator ready"));
emulator.add_listener("download-error", (e) =>
    console.log(`[harness] download error: ${e && e.file_name}`));

/* The text screen, kept so that "nothing arrived on COM1" can say why.
 *
 * Everything these rigs read comes over a serial port, and the things that
 * stop a run before the serial port is involved -- a disk that will not
 * mount, a batch file that is not there -- are printed on the screen by
 * DOS and were invisible. This cost twenty minutes of looking in the wrong
 * place: a half-written disk image said "Bad command or filename" on a
 * screen nobody was reading. */
const screen = [];
emulator.add_listener("screen-put-char", ([row, col, ch]) => {
    screen[row] = screen[row] || [];
    screen[row][col] = String.fromCharCode(ch || 32);
});

function screenText() {
    return screen
        .map((r) => (r || []).join("").replace(/\s+$/, ""))
        .filter(Boolean)
        .join("\n");
}

emulator.add_listener("serial0-output-byte", (byte) => {
    serial += String.fromCharCode(byte);
    if (serial.includes("AUTODL-DONE"))
        finish();
});

const ticker = setInterval(() => {
    if (!settled)
        console.log(`[harness] ${server.streams.size} stream(s), `
                    + `${hub.sent} hub lines, ${hub.served} bytes served, `
                    + `${serial.length} from COM1`);
}, 10000);

function finish() {
    if (settled) return;
    settled = true;
    clearInterval(ticker);

    console.log("\n--- COM1 ---");
    console.log(serial.replace(/\r/g, "").trim() || "(nothing)");

    if (!serial.trim()) {
        /* Whatever stopped it happened before the batch file could
           redirect anything. The screen has the reason. */
        console.log("\n--- the guest's screen, since COM1 was silent ---");
        console.log(screenText() || "(blank: it never got to DOS)");
    }

    console.log("\n--- what the guest dialled ---");
    for (const a of [...new Set(server.connects)])
        console.log(`  ${a}`);

    try { emulator.destroy(); } catch { /* already gone */ }

    let bad = 0;
    const fail = (why) => { console.log(`FAIL: ${why}`); bad++; };

    /* Login is judged from the hub's side: the client's "connected" goes
       to its own screen, and what matters here is that the hub completed
       the handshake and saw this node in NORMAL state. */
    if (!logins.length)
        fail("the in-page hub never completed a login");
    if (!/download done: BLKDROP\.ZIP/.test(serial))
        fail("the client did not download the game from the in-page attic");
    if (!/BLKDROP  ZIP/.test(serial))
        fail("DIR does not show the file on the guest's disk");
    if (hub.served === 0)
        fail("the attic served no bytes");

    const want = files.find((f) => f.name.endsWith("BLKDROP.ZIP"));
    if (want && !serial.includes(String(want.size)))
        fail(`the guest's DIR does not show ${want.size} bytes`);

    console.log(`\nhub lines: ${hub.sent}, bytes served: ${hub.served}`);
    if (process.env.INPAGE_TRACE)
        console.log(log.join("\n"));

    if (bad === 0) {
        console.log("\nV86 IN-PAGE OK: the DOS client logged in, searched, "
                    + "downloaded a game and");
        console.log("verified its hash -- against a hub, a serving peer and "
                    + "a relay that are all");
        console.log("JavaScript in this process. Nothing listened on a port.");
        process.exit(0);
    }
    console.log("\nV86 IN-PAGE FAILED");
    process.exit(1);
}

setTimeout(() => {
    console.log(`[harness] TIMEOUT after ${seconds}s`);
    finish();
}, seconds * 1000);
