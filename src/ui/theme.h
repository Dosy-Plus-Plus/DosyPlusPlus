/* theme.h - colours as data, not scattered constants.
 *
 * Three skins, because the look is the point of the project and a single
 * hardcoded palette would make it one look forever. Switching theme must
 * never require touching drawing code.
 */
#ifndef DOSYPP_THEME_H
#define DOSYPP_THEME_H

#include "screen.h"

typedef struct {
    const char *name;

    unsigned char desktop;       /* the backdrop behind panels */
    unsigned char desktop_ch;    /* and the character it is drawn with */

    unsigned char panel;         /* panel interior */
    unsigned char border;        /* panel border */
    unsigned char title;         /* panel title text */

    unsigned char header;        /* top bar */
    unsigned char header_hi;     /* highlighted text in the top bar */
    unsigned char status;        /* bottom hotkey bar */
    unsigned char status_key;    /* the key letter itself */

    unsigned char text;          /* ordinary body text */
    unsigned char text_dim;      /* secondary detail */
    unsigned char text_hi;       /* emphasis */

    unsigned char bar;           /* lightbar, unfocused list */
    unsigned char bar_focus;     /* lightbar, focused list */

    unsigned char nick_self;     /* your own nick in chat */
    unsigned char nick_op;       /* operators */
    unsigned char nick_bot;      /* bots */
    unsigned char nick_user;     /* everyone else */
    unsigned char chat_text;
    unsigned char chat_action;   /* /me lines */
    unsigned char chat_system;   /* joins, parts, hub notices */
    unsigned char chat_pm;

    unsigned char gauge_full;
    unsigned char gauge_empty;

    unsigned char error;
} theme;

/* Available themes, in order. */
#define THEME_COUNT 3

const theme *theme_get(int index);
const theme *theme_by_name(const char *name);

#endif /* DOSYPP_THEME_H */
