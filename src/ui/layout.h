/* layout.h - where the rows go, worked out from the display's height.
 *
 * This was a block of #defines for 80x25, which was fine until the screen
 * could be 50 rows as well. The arithmetic lives in one place so that a
 * door cannot disagree with the hotkey bar about which row is the last
 * one -- the comment that used to warn against deriving these by
 * arithmetic was warning against doing it at nine separate use sites, not
 * against doing it at all.
 *
 * Pure arithmetic, in its own unit, so the tests can check a layout
 * without linking the UI or opening a display.
 */
#ifndef DOSYPP_LAYOUT_H
#define DOSYPP_LAYOUT_H

typedef struct {
    int rows;           /* the display height this was computed for */
    int panel_y;        /* top border of the big panel */
    int panel_h;
    int chat_top;
    int sep_row;        /* the rule above the input line */
    int input_row;
    int status_row;
    int hotkey_row;
} app_layout;

app_layout app_layout_for(int rows);

#endif /* DOSYPP_LAYOUT_H */
