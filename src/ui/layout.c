/* layout.c - the row arithmetic. See layout.h. */
#include "layout.h"

app_layout app_layout_for(int rows)
{
    app_layout l;

    /* The floor is what the 25-row layout needs: a header, a border, one
       row of chat, a separator, an input line, a border, a status line and
       the hotkey bar. Anything less is not a screen this UI can use, and
       clamping is better than drawing off the top. */
    if (rows < 10)
        rows = 10;

    l.rows       = rows;
    l.hotkey_row = rows - 1;
    l.status_row = rows - 2;
    l.panel_y    = 1;
    l.panel_h    = l.status_row - l.panel_y;    /* border lands on rows-3 */
    l.input_row  = l.panel_y + l.panel_h - 2;
    l.sep_row    = l.input_row - 1;
    l.chat_top   = 2;

    return l;
}
