/* widget.h - the handful of controls a BBS door needs.
 *
 * All pure logic over a cell buffer: no I/O, no DOS, no globals. That keeps
 * the awkward parts -- word wrap, scroll clamping, lightbar paging -- under
 * unit test, which is where they belong. Getting scroll arithmetic wrong is
 * the classic text-UI bug and it is invisible until someone resizes or
 * reaches the end of a list.
 */
#ifndef DOSYPP_WIDGET_H
#define DOSYPP_WIDGET_H

#include "screen.h"
#include "theme.h"

/* ------------------------------------------------------------ scrollback */

#define SB_LINE_MAX  120
#define SB_LINES     400     /* ~48KB: generous on a 16MB machine, and a
                                BBS without scrollback is a teletype */

typedef enum {
    SB_CHAT,
    SB_ACTION,       /* /me */
    SB_SYSTEM,       /* joins, parts, hub notices */
    SB_PM,
    SB_ERROR
} sb_kind;

typedef struct {
    char    text[SB_LINE_MAX];
    unsigned char kind;
    unsigned char nick_len;    /* colour the nick separately from the body */
    unsigned char nick_attr;   /* op yellow, bot cyan, you white, ... */
} sb_line;

typedef struct {
    sb_line  lines[SB_LINES];
    int      count;      /* total ever added, for numbering */
    int      head;       /* ring index of the oldest retained line */
    int      used;       /* lines currently retained */
    int      scroll;     /* 0 = pinned to the newest line */
} scrollback;

void sb_init(scrollback *sb);

/* Add a line, wrapping to `width` columns at word boundaries. `nick` may be
   NULL; when given it is prefixed as "<nick> " and coloured separately.
   Text must already be CP437. */
void sb_add(scrollback *sb, int width, sb_kind kind, const char *nick,
            unsigned char nick_attr, const char *text);

/* Scrolling. Positive moves towards older lines. Clamps. */
void sb_scroll(scrollback *sb, int delta, int page_h);
void sb_scroll_end(scrollback *sb);
int  sb_at_end(const scrollback *sb);

/* Draw into the rectangle. An underfull pane fills from the top and grows
   downward, like a scrolling terminal region; once full, the newest line
   sits on the last row. */
void sb_draw(const scrollback *sb, screen *s, const theme *th,
             int x, int y, int w, int h);

/* ------------------------------------------------------------ input line */

#define INPUT_MAX 256

typedef struct {
    char text[INPUT_MAX];
    int  len;
    int  cursor;
    int  scroll;        /* first visible column, for text wider than the box */
    int  enabled;
} inputline;

void input_init(inputline *in);
/* Returns 1 when Enter was pressed and text is ready. */
int  input_key(inputline *in, int key);
void input_clear(inputline *in);
void input_draw(const inputline *in, screen *s, const theme *th,
                int x, int y, int w);

/* ------------------------------------------------------------- list box */

typedef void (*list_row_fn)(void *user, int index, char *out, int outsz);

typedef struct {
    int  count;
    int  sel;
    int  top;
    int  focus;
} listbox;

void listbox_init(listbox *lb);
/* Keeps sel and top consistent after the row count changes. */
void listbox_set_count(listbox *lb, int count);
int  listbox_key(listbox *lb, int key, int page_h);
void listbox_draw(const listbox *lb, screen *s, const theme *th,
                  int x, int y, int w, int h,
                  list_row_fn row, void *user);

/* ---------------------------------------------------------------- gauge */

/* [▓▓▓▓░░░░] with a percentage. pct is clamped to 0..100. No floating
   point: the whole program is built -msoft-float. */
void gauge_draw(screen *s, const theme *th, int x, int y, int w, int pct);

/* ------------------------------------------------------------ furniture */

/* Top bar: hub name on the left, status on the right. */
void draw_header(screen *s, const theme *th, const char *left,
                 const char *right);

/* Bottom bar of hotkeys, "1-CHAT 2-WHO ..." with the keys highlighted.
   `active` is the index to show as selected, or -1. */
void draw_hotkeys(screen *s, const theme *th, const char *const *labels,
                  int count, int active);

#endif /* DOSYPP_WIDGET_H */
