/* workershim.mjs - enough of a Web Worker for v86's zstd decompressor.
 *
 * The published disk image is split into zstd-compressed pieces (see
 * tools/splitimg.mjs): 128MB of mostly-empty FAT16 becomes 8.9MB, and the
 * browser fetches whole small files instead of byte ranges, so the demo
 * stops depending on the host honouring Range requests.
 *
 * v86 decompresses those pieces in a Web Worker, which Node does not have.
 * Without this shim the rigs could only boot the uncompressed image -- and
 * then nothing would ever have booted the form that actually gets
 * published, which is the status this project tries not to ship.
 *
 * So: a Worker backed by node:worker_threads, with the three things v86's
 * decompressor uses -- postMessage, onmessage, and addEventListener. The
 * worker body arrives as a blob URL holding a stringified function that
 * assigns to a global `onmessage`, so the prelude below defines that
 * global and bridges it to parentPort.
 *
 * What this does NOT establish is that the browser's own Worker behaves
 * the same way. It establishes everything on our side of it: that the
 * pieces are named as v86 expects, that they decompress to the right
 * bytes, and that DOS boots off them.
 */
import { Worker as NodeWorker } from "node:worker_threads";
import { resolveObjectURL } from "node:buffer";

export function installWorkerShim() {
    if (globalThis.Worker)
        return false;               /* a real one, or already installed */

    globalThis.Worker = class WorkerShim {
        constructor(url) {
            this.onmessage = null;
            this._listeners = [];
            this._worker = null;
            /* Messages posted before the body has been read, which for
               v86 is the wasm source -- posted immediately. */
            this._queue = [];

            const blob = resolveObjectURL(url);
            if (!blob)
                throw new Error(`WorkerShim: cannot resolve ${url}`);

            blob.text().then((code) => {
                const prelude = `
                    const { parentPort } = require("node:worker_threads");
                    let _onmessage = null;
                    Object.defineProperty(globalThis, "onmessage", {
                        get: () => _onmessage,
                        set: (f) => { _onmessage = f; },
                    });
                    globalThis.postMessage = (d) => parentPort.postMessage(d);
                    parentPort.on("message", (m) => {
                        if (_onmessage) _onmessage({ data: m });
                    });
                `;
                this._worker = new NodeWorker(prelude + code, { eval: true });
                this._worker.on("message", (data) => {
                    const ev = { data };
                    if (this.onmessage)
                        this.onmessage(ev);
                    /* A copy: v86 removes its listener from inside it. */
                    for (const fn of [...this._listeners])
                        fn(ev);
                });
                /* Do not keep the process alive on its own account. */
                this._worker.unref();

                for (const m of this._queue)
                    this._worker.postMessage(m);
                this._queue = [];
            });
        }

        addEventListener(type, fn) {
            if (type === "message")
                this._listeners.push(fn);
        }

        removeEventListener(type, fn) {
            this._listeners = this._listeners.filter((f) => f !== fn);
        }

        postMessage(data) {
            if (this._worker)
                this._worker.postMessage(data);
            else
                this._queue.push(data);
        }

        terminate() {
            if (this._worker)
                this._worker.terminate();
        }
    };

    return true;
}

/**
 * The hda/hdb configuration for a split image, from its manifest.
 *
 * `use_parts` is v86's own option for this and the naming is v86's too:
 * basename + "-" + start + "-" + end + extension. Getting it wrong 404s
 * every piece with no explanation, so the manifest is the single place the
 * numbers come from.
 */
export function partsConfig(manifest, urlOrDir) {
    const base = urlOrDir.replace(/\/+$/, "");

    return {
        url: `${base}/${manifest.image}`,
        async: true,
        use_parts: true,
        size: manifest.size,
        fixed_chunk_size: manifest.chunk,
    };
}
