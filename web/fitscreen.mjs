/* fitscreen.mjs - how big to draw the emulated screen.
 *
 * v86 sizes its own canvas: in graphics mode it sets the element's width
 * and height in pixels, inline, and doubles them for small modes. CSS
 * cannot win an argument with that -- a `width: 100%` rule is simply
 * ignored, which is why Doom's 320x200 arrived at its own size, anchored
 * to one corner of a bezel several times wider, and clipped.
 *
 * So the page does not argue. It measures whatever v86 produced and
 * scales the whole screen container with a transform, which works for the
 * canvas and for the text-mode element alike and needs to know nothing
 * about either.
 *
 * The arithmetic is here, and tested, because it is the part that can be
 * wrong in a way nobody notices until a game starts.
 */

/** Never blow a 320x200 mode up past this: it is already soft. */
export const MAX_SCALE = 4;

/**
 * @param natW  the screen's natural width in CSS pixels (0 if unknown)
 * @param natH  its natural height
 * @param boxW  how much width there is to fill
 * @param maxH  how much height may be used
 * @returns the scale factor to apply, 1 when there is nothing to measure
 */
export function fitScale(natW, natH, boxW, maxH) {
    if (!natW || !natH || !boxW || !maxH)
        return 1;

    /* Fill the width, unless that would be taller than the room
       available -- a 4:3 game in a wide bezel is limited by height, and a
       25-row text screen by width. */
    const scale = Math.min(boxW / natW, maxH / natH);

    if (!isFinite(scale) || scale <= 0)
        return 1;
    return Math.min(scale, MAX_SCALE);
}

/**
 * The size the page should reserve for a screen scaled that way, so that
 * a mode change does not move everything below it.
 */
export function fitBox(natW, natH, scale) {
    return {
        width: Math.round(natW * scale),
        height: Math.round(natH * scale),
    };
}
