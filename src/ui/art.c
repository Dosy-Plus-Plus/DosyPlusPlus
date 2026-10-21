/* art.c - logo, rules, scroller, logoff screen. */
#include "art.h"

#include <stdio.h>
#include <string.h>

/* '#' is a solid block. See the header for why it is not stored as CP437. */
static const char *const LOGO[ART_LOGO_ROWS] = {
    "###  #### #### #  #           ",
    "#  # #  # #    #  #   #    #  ",
    "#  # #  # #### #### ##### #####",
    "#  # #  #    #   #    #    #  ",
    "###  #### ####   #            "
};

/* The big one, with two-cell strokes. Written out rather than scaled at
   draw time: doubling the small logo gives letters with holes in them,
   because its strokes are one cell wide with a gap of one between them. */
static const char *const LOGO_BIG[ART_LOGO_BIG_ROWS] = {
    "########    ########   #########",
    "########## ########## ##########",
    "##      ## ##      ## ##         ##      ##    ####       ####",
    "##      ## ##      ## ##         ##      ##    ####       ####",
    "##      ## ##      ##  ########  ##      ## ########## ##########",
    "##      ## ##      ##         ##  ########  ########## ##########",
    "##      ## ##      ##         ##         ##    ####       ####",
    "########## ########## ########## ##########    ####       ####",
    "########    ########  #########  ########"
};

static void draw_blocks(screen *s, int x, int y, const char *const *art,
                        int rows, unsigned char attr)
{
    int row, col;

    for (row = 0; row < rows; row++) {
        const char *line = art[row];

        for (col = 0; line[col]; col++)
            if (line[col] == '#')
                screen_putc(s, x + col, y + row, CH_BLOCK_FULL, attr);
    }
}

void art_logo(screen *s, int x, int y, unsigned char attr)
{
    draw_blocks(s, x, y, LOGO, ART_LOGO_ROWS, attr);
}

void art_logo_big(screen *s, int x, int y, unsigned char attr)
{
    draw_blocks(s, x, y, LOGO_BIG, ART_LOGO_BIG_ROWS, attr);
}

int art_logo_fit(screen *s, int x, int y, int w, int rows,
                 unsigned char attr)
{
    /* Forty is the threshold rather than fifty: it is the point at which
       the big logo and the text under it still leave room, and it means an
       odd terminal height does not fall back to the small logo for the
       sake of one row. The width is checked as well, because the big logo
       is 65 columns and a panel inside a border is not. */
    if (rows >= 40 && w >= ART_LOGO_BIG_COLS) {
        art_logo_big(s, x + (w - ART_LOGO_BIG_COLS) / 2, y, attr);
        return y + ART_LOGO_BIG_ROWS;
    }

    art_logo(s, x + (w - ART_LOGO_COLS) / 2, y, attr);
    return y + ART_LOGO_ROWS;
}

void art_rule(screen *s, int x, int y, int w, unsigned char attr)
{
    int i;

    for (i = 0; i < w; i++) {
        unsigned char ch;
        int third = w / 3;

        if (third < 1)
            third = 1;
        if (i < third)
            ch = CH_SHADE_LIGHT;
        else if (i < third * 2)
            ch = CH_SHADE_MED;
        else
            ch = CH_SHADE_DARK;

        screen_putc(s, x + i, y, ch, attr);
    }
}

void art_scroller(screen *s, int x, int y, int w, int phase,
                  unsigned char attr)
{
    /* The obligatory greetz. Padded with spaces so it reads as a loop
       rather than a wrap. */
    static const char TEXT[] =
        "GREETZ TO ALL THE SYSOPS STILL RUNNING A BOARD IN "
        "THE YEAR TWENTY-TWENTY-SIX   ...   NO LEECHING   ...   "
        "RATIO IS 1:3 AND THAT IS FINAL   ...   ";
    int len = (int)(sizeof TEXT - 1);
    int i;

    if (w <= 0 || len <= 0)
        return;

    for (i = 0; i < w; i++) {
        int src = (phase + i) % len;
        if (src < 0)
            src += len;
        screen_putc(s, x + i, y, (unsigned char)TEXT[src], attr);
    }
}

void art_credits(screen *s, int x, int y, int w, const char *tune,
                 unsigned char attr, unsigned char dim)
{
    char buf[80];

    if (w <= 0)
        return;

    screen_puts_center(s, x, y, w, "CODE AND ART BY THE AUTHOR", dim);

    if (tune && tune[0])
        snprintf(buf, sizeof buf, "%c%c MUSIC: %s %c%c",
                 CH_NOTE, CH_NOTE, tune, CH_NOTE, CH_NOTE);
    else
        snprintf(buf, sizeof buf, "NO MUSIC -- SEE SETUP");

    screen_puts_center(s, x, y + 1, w, buf, attr);
}

void art_banner(screen *s, int x, int y, int w, const char *text,
                unsigned char attr, unsigned char edge)
{
    int len = text ? (int)strlen(text) : 0;
    int pad, i, tx;

    /* Two spaces either side of the text, and at least three blocks each
       end for it to read as a banner at all. */
    if (w < len + 10)
        return;

    pad = (w - (len + 4)) / 2;
    tx = x + pad + 2;

    for (i = 0; i < pad; i++) {
        unsigned char ch = (i < pad / 3)        ? CH_SHADE_LIGHT
                         : (i < (pad * 2) / 3)  ? CH_SHADE_MED
                                                : CH_SHADE_DARK;

        /* Mirrored, so the shading runs light-to-dark into the text from
           both sides. */
        screen_putc(s, x + i, y, ch, edge);
        screen_putc(s, x + w - 1 - i, y, ch, edge);
    }

    screen_puts(s, tx, y, text, attr);
}

void art_logoff(screen *s, const theme *th, const char *hub,
                const char *nick, unsigned long secs, unsigned int nodes)
{
    char buf[80];
    int box_x, box_w, box_y, box_h;
    int y;

    screen_clear(s, th->desktop);
    screen_fill(s, 0, 0, s->w, s->h, th->desktop_ch, th->desktop);

    /* Centred on whatever height there is, and wide enough for the big
       logo when the screen is tall enough to use it. The sign-off used to
       land on the bottom border, which is why the rows are counted from
       the box rather than written in. */
    box_h = (s->h >= 40) ? 21 : 17;
    box_x = (s->h >= 40) ? 3 : 10;
    box_w = s->w - 2 * box_x;
    box_y = (s->h - box_h) / 2;
    if (box_y < 1)
        box_y = 1;

    screen_box(s, box_x, box_y, box_w, box_h, BOX_DOUBLE, th->border);

    y = art_logo_fit(s, box_x + 1, box_y + 2, box_w - 2, s->h,
                     th->text_hi) + 1;

    art_rule(s, box_x + 4, y++, box_w - 8, th->border);

    snprintf(buf, sizeof buf, "%s, you were on %s", nick,
             hub[0] ? hub : "the hub");
    screen_puts_center(s, box_x + 1, y++, box_w - 2, buf, th->text);

    /* Integer only: the whole program is built -msoft-float. */
    snprintf(buf, sizeof buf, "CONNECT TIME  %lu:%02lu:%02lu   NODES SEEN  %u",
             secs / 3600UL, (secs / 60UL) % 60UL, secs % 60UL, nodes);
    screen_puts_center(s, box_x + 1, y++, box_w - 2, buf, th->text_dim);

    y++;
    screen_puts_center(s, box_x + 1, y++, box_w - 2,
                       "THANK YOU FOR CALLING  -  CALL BACK SOON",
                       th->title);

    /* The sign-off goes at the bottom of the box rather than after the
       text: "NO CARRIER" is the last thing on the screen, and it reads as
       the modem hanging up only if it is where the modem would have put
       it. */
    screen_puts_center(s, box_x + 1, box_y + box_h - 3, box_w - 2, "+++",
                       th->text_dim);
    screen_puts_center(s, box_x + 1, box_y + box_h - 2, box_w - 2,
                       "NO CARRIER", th->error);

    screen_cursor_hide(s);
}
