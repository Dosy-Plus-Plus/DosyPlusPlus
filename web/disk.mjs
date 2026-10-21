/* disk.mjs - which form of the disk to ask v86 for.
 *
 * There are three, and the page has to pick without knowing where it is
 * being served from:
 *
 *   pieces    what gets published: a few hundred zstd-compressed chunks
 *             plus a manifest. v86 reads these natively (`use_parts`) and
 *             fetches whole small files, so no HTTP Range is involved.
 *   streamed  one big file, read in blocks over Range. What a development
 *             tree has, when you would rather not load 128MB up front.
 *   whole     one big file, loaded entirely. Fastest when it is local.
 *
 * This is a function on its own, with tests, because getting it wrong
 * produced a demo that could not boot at all: the first version only
 * looked for the manifest when a "stream the disk" box was ticked, and
 * that box defaults to off when the page is served from localhost -- so
 * the published bundle, which has no single-file image, fell back to
 * asking for one and failed with "Download failed: images/dosypp.img".
 *
 * The rule: if the pieces are there, they are the disk. Nothing else is a
 * choice worth offering.
 */

/**
 * @param manifest  the parsed images/dosypp.img.json, or null if absent
 * @param streamed  the visitor's preference, which only applies when
 *                  there is a single file
 * @param base      path prefix, "images/" by default
 */
export function chooseDisk(manifest, streamed, base = "images/") {
    if (manifest && manifest.image && manifest.size && manifest.chunk) {
        return {
            url: base + manifest.image,
            async: true,
            use_parts: true,
            size: manifest.size,
            fixed_chunk_size: manifest.chunk,
            /* Not v86's: the page uses these to say what it is doing. */
            form: "pieces",
            describe: `${manifest.parts} compressed pieces, `
                + `${(manifest.bytes / 1048576).toFixed(1)}MB for a `
                + `${(manifest.size / 1048576).toFixed(0)}MB drive`,
        };
    }

    return {
        url: base + "dosypp.img",
        async: !!streamed,
        form: streamed ? "streamed" : "whole",
        describe: streamed
            ? "one file, read in blocks over HTTP Range"
            : "one file, loaded up front",
    };
}

/** Fetch the manifest, or null if this tree does not publish pieces. */
export async function diskManifest(base = "images/") {
    try {
        const r = await fetch(base + "dosypp.img.json");
        if (!r.ok)
            return null;
        const m = await r.json();
        /* A manifest that does not describe a disk is worse than none:
           v86 would be handed a size of undefined and read nothing. */
        return (m && m.image && m.size && m.chunk) ? m : null;
    } catch {
        return null;
    }
}
