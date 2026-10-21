/* art.h - the ASCII/ANSI art, compiled in.
 *
 * Stored as plain ASCII with '#' standing for a solid block, translated to
 * CP437 at draw time. Keeping CP437 high bytes out of the source means the
 * files stay valid UTF-8 and survive every editor and diff tool; the one
 * time art was written with literal high bytes it rendered as mojibake.
 *
 * Compiled in rather than loaded from an art directory because the art is part of
 * the program's identity, and a client that cannot draw its own logo
 * because a file is missing is worse than one that cannot be reskinned.
 * Hub-supplied art is a different matter and goes through ansi.c.
 */
#ifndef DOSYPP_ART_H
#define DOSYPP_ART_H

#include "screen.h"
#include "theme.h"

/* The DOSy++ logo: 5 rows, 31 columns. */
#define ART_LOGO_ROWS 5
#define ART_LOGO_COLS 31

void art_logo(screen *s, int x, int y, unsigned char attr);

/* The same word at twice the size, for the 50-row screen: 9 rows, 65
   columns. Not a scaled-up version of the small one -- the strokes are two
   cells thick, which is what makes block letters read at this size.

   Both exist because 80x50 uses an 8x8 font: the cells are half the height,
   so the small logo on a tall screen looks like a caption. */
#define ART_LOGO_BIG_ROWS 9
#define ART_LOGO_BIG_COLS 65

void art_logo_big(screen *s, int x, int y, unsigned char attr);

/* Whichever fits, centred within [x, x+w) -- the big one needs 65 columns,
   so a narrower box gets the small one however tall the screen is.
   `rows` is the display height. Returns the row after the logo, so a
   caller can go on laying out underneath it. */
int art_logo_fit(screen *s, int x, int y, int w, int rows,
                 unsigned char attr);

/* A shaded rule, light to dark, the way a 1994 ANSI separated sections. */
void art_rule(screen *s, int x, int y, int w, unsigned char attr);

/* The greetz line that scrolled across every splash screen. `phase`
   advances by one per frame; the text wraps. */
void art_scroller(screen *s, int x, int y, int w, int phase,
                  unsigned char attr);

/* The line a cracktro always had: who made it, and what is playing. The
   tune title comes from the module itself, so it says "no music" honestly
   when there is none rather than claiming a tune that is not playing. */
void art_credits(screen *s, int x, int y, int w, const char *tune,
                 unsigned char attr, unsigned char dim);

/* A sided marquee: block ends with text between them, the way a 1994
   bulletin headed a section. Draws nothing if `w` is too narrow for the
   text, rather than overflowing the box. */
void art_banner(screen *s, int x, int y, int w, const char *text,
                unsigned char attr, unsigned char edge);

/* The logoff screen: carrier drop, session summary, and a sign-off. */
void art_logoff(screen *s, const theme *th, const char *hub,
                const char *nick, unsigned long secs, unsigned int nodes);

#endif /* DOSYPP_ART_H */
