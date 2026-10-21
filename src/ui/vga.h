/* vga.h - getting the cell buffer onto a display.
 *
 * Two backends, chosen at compile time:
 *
 *   DOS   80x25 text mode, cells written straight into video memory at
 *         0xB8000 through a DPMI selector. Only changed cells are written,
 *         batched into runs.
 *   host  the same buffer drawn into a UTF-8 terminal with ANSI colour, so
 *         the entire UI can be developed and looked at without an emulator.
 *
 * The host backend is not a toy: it is how the layout gets built. Iterating
 * on an 80x25 screen through DOSBox is slow enough to change what gets
 * attempted.
 */
#ifndef DOSYPP_VGA_H
#define DOSYPP_VGA_H

#include "screen.h"

/* Enter text mode (DOS) or raw terminal (host) at `rows` rows -- 25 or 50;
   anything else is treated as 25. Returns 0, or -1 with a reason in
   vga_lasterr().

   What you asked for is not necessarily what you get: a terminal may be
   shorter than 50 rows, and a machine without a VGA cannot do 50 at all.
   Call vga_size() afterwards and lay out for what it says. */
int vga_open(int rows);

/* Open a display that draws nowhere. The cell buffer is still filled in
   exactly as normal, so the layout can be rendered and inspected with no
   terminal at all -- which is how the UI gets screenshotted in CI and how
   --screenshot works. */
int vga_open_null(int rows);

/* Write the current buffer as UTF-8 text, one row per line. For
   --screenshot and for tests; CP437 is translated for readability. */
void vga_dump(const screen *s, void *fp);

/* Read video memory back and compare it with the cell buffer, returning the
   number of cells that differ.
 *
 * This is the only way to check the DOS renderer without a pair of eyes: it
 * proves the dirty-run diffing actually puts the right bytes at the right
 * offsets in 0xB8000. Returns -1 where readback is not possible (the host
 * and null backends). */
int vga_verify(const screen *s);

/* Restore whatever was there before: video mode and cursor on DOS, cooked
   terminal and attributes on the host. Safe to call twice. */
void vga_close(void);

/* Write the cells that differ from the last flush, then place the cursor. */
void vga_flush(screen *s);

const char *vga_lasterr(void);

/* The display's actual size: what the mode or the terminal gives, after
   vga_open() has had its say. Not what was asked for. */
void vga_size(int *w, int *h);

#endif /* DOSYPP_VGA_H */
