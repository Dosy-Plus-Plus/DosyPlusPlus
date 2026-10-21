/* theme.c - the palettes. */
#include "theme.h"

#include <string.h>

static const theme THEMES[THEME_COUNT] = {
    {
        /* "renegade": the default. Blue desktop, cyan furniture, the look of
           a mid-90s Renegade or Telegard board. */
        "renegade",
        ATTR(C_BLUE, C_BLACK), CH_SHADE_LIGHT,
        ATTR(C_LGREY, C_BLUE), ATTR(C_LCYAN, C_BLUE), ATTR(C_WHITE, C_BLUE),
        ATTR(C_BLACK, C_LGREY), ATTR(C_RED, C_LGREY),
        ATTR(C_BLACK, C_CYAN), ATTR(C_WHITE, C_CYAN),
        ATTR(C_LGREY, C_BLUE), ATTR(C_DGREY, C_BLUE), ATTR(C_WHITE, C_BLUE),
        ATTR(C_BLACK, C_CYAN), ATTR(C_WHITE, C_CYAN),
        ATTR(C_WHITE, C_BLUE), ATTR(C_YELLOW, C_BLUE),
        ATTR(C_LCYAN, C_BLUE), ATTR(C_LGREEN, C_BLUE),
        ATTR(C_LGREY, C_BLUE), ATTR(C_LMAGENTA, C_BLUE),
        ATTR(C_DGREY, C_BLUE), ATTR(C_LCYAN, C_BLUE),
        ATTR(C_LGREEN, C_BLUE), ATTR(C_DGREY, C_BLUE),
        ATTR(C_LRED, C_BLUE)
    },
    {
        /* "pcboard": black desktop, grey panels, high-contrast. Closer to a
           PCBoard install with the colour set turned down. */
        "pcboard",
        ATTR(C_DGREY, C_BLACK), CH_SHADE_LIGHT,
        ATTR(C_LGREY, C_BLACK), ATTR(C_LGREY, C_BLACK), ATTR(C_YELLOW, C_BLACK),
        ATTR(C_BLACK, C_LGREY), ATTR(C_BLUE, C_LGREY),
        ATTR(C_LGREY, C_BLACK), ATTR(C_YELLOW, C_BLACK),
        ATTR(C_LGREY, C_BLACK), ATTR(C_DGREY, C_BLACK), ATTR(C_WHITE, C_BLACK),
        ATTR(C_BLACK, C_LGREY), ATTR(C_BLACK, C_WHITE),
        ATTR(C_WHITE, C_BLACK), ATTR(C_YELLOW, C_BLACK),
        ATTR(C_LCYAN, C_BLACK), ATTR(C_LGREEN, C_BLACK),
        ATTR(C_LGREY, C_BLACK), ATTR(C_LMAGENTA, C_BLACK),
        ATTR(C_DGREY, C_BLACK), ATTR(C_LCYAN, C_BLACK),
        ATTR(C_LGREEN, C_BLACK), ATTR(C_DGREY, C_BLACK),
        ATTR(C_LRED, C_BLACK)
    },
    {
        /* "amiga": the scene look. Black and orange, no blue anywhere,
           deliberately flat. */
        "amiga",
        ATTR(C_BLACK, C_BLACK), ' ',
        ATTR(C_LGREY, C_BLACK), ATTR(C_BROWN, C_BLACK), ATTR(C_YELLOW, C_BLACK),
        ATTR(C_BLACK, C_BROWN), ATTR(C_WHITE, C_BROWN),
        ATTR(C_BROWN, C_BLACK), ATTR(C_YELLOW, C_BLACK),
        ATTR(C_LGREY, C_BLACK), ATTR(C_DGREY, C_BLACK), ATTR(C_YELLOW, C_BLACK),
        ATTR(C_BLACK, C_BROWN), ATTR(C_BLACK, C_YELLOW),
        ATTR(C_YELLOW, C_BLACK), ATTR(C_LRED, C_BLACK),
        ATTR(C_BROWN, C_BLACK), ATTR(C_LGREY, C_BLACK),
        ATTR(C_LGREY, C_BLACK), ATTR(C_LCYAN, C_BLACK),
        ATTR(C_DGREY, C_BLACK), ATTR(C_BROWN, C_BLACK),
        ATTR(C_YELLOW, C_BLACK), ATTR(C_DGREY, C_BLACK),
        ATTR(C_LRED, C_BLACK)
    }
};

const theme *theme_get(int index)
{
    if (index < 0 || index >= THEME_COUNT)
        index = 0;
    return &THEMES[index];
}

const theme *theme_by_name(const char *name)
{
    int i;

    if (name)
        for (i = 0; i < THEME_COUNT; i++)
            if (strcmp(THEMES[i].name, name) == 0)
                return &THEMES[i];
    return &THEMES[0];
}
