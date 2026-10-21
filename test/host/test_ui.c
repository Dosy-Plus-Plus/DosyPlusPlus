/* test_ui.c - screen primitives, scrollback wrapping, lists, CP437.
 *
 * The UI is mostly arithmetic: wrap points, scroll clamps, paging. All of it
 * is testable without a display, which is the only reason it is safe to
 * build a text UI for a platform whose debugger is "print and reboot".
 */
#include <stdio.h>
#include <string.h>

#include "../../src/ui/screen.h"
#include "../../src/ui/widget.h"
#include "../../src/ui/theme.h"
#include "../../src/ui/cp437.h"
#include "../../src/ui/kbd.h"
#include "../../src/ui/ansi.h"
#include "../../src/ui/art.h"
#include "../../src/ui/app.h"
#include "../../src/snd/pcspkr.h"

static int g_fail;
static int g_run;

#define CHECK(cond, ...) do {                                              \
        g_run++;                                                           \
        if (!(cond)) {                                                     \
            printf("FAIL line %d: ", __LINE__);                            \
            printf(__VA_ARGS__);                                           \
            printf("\n");                                                  \
            g_fail++;                                                      \
        }                                                                  \
    } while (0)

#define CHECK_STR(got, want) do {                                          \
        g_run++;                                                           \
        if (!(got) || strcmp((got), (want)) != 0) {                        \
            printf("FAIL line %d:\n  got  \"%s\"\n  want \"%s\"\n",        \
                   __LINE__, (got) ? (got) : "(null)", (want));            \
            g_fail++;                                                      \
        }                                                                  \
    } while (0)

/* Trailing spaces make row comparisons unreadable. */
static const char *trim_row(const screen *s, int y)
{
    static char buf[SCREEN_W + 1];
    int i;

    screen_row_text(s, y, buf);
    for (i = (int)strlen(buf) - 1; i >= 0 && buf[i] == ' '; i--)
        buf[i] = '\0';
    return buf;
}

/* ------------------------------------------------------------- screen */

static void test_screen_basics(void)
{
    screen s;

    CHECK(screen_init(&s, 80, 25) == 0, "init failed");
    CHECK(s.w == 80 && s.h == 25, "size is %dx%d", s.w, s.h);

    screen_clear(&s, ATTR(C_LGREY, C_BLACK));
    CHECK(screen_at(&s, 0, 0).ch == ' ', "clear did not blank");

    screen_puts(&s, 2, 1, "HELLO", ATTR(C_WHITE, C_BLUE));
    CHECK_STR(trim_row(&s, 1), "  HELLO");
    CHECK(screen_at(&s, 2, 1).attr == ATTR(C_WHITE, C_BLUE), "attr lost");

    /* Out of bounds must clip silently. A UI that aborts because a nick was
       long is worse than one that truncates. */
    screen_putc(&s, -5, 0, 'X', 7);
    screen_putc(&s, 999, 0, 'X', 7);
    screen_putc(&s, 0, -1, 'X', 7);
    screen_putc(&s, 0, 999, 'X', 7);
    screen_puts(&s, 78, 2, "OVERFLOWING", 7);
    CHECK_STR(trim_row(&s, 2), "                                                                              OV");

    /* Padding is what erases a previous, longer value in a list row. */
    screen_puts(&s, 0, 3, "LONGLONGLONG", 7);
    screen_puts_pad(&s, 0, 3, "ab", 12, 7);
    CHECK_STR(trim_row(&s, 3), "ab");

    screen_puts_center(&s, 0, 4, 11, "abc", 7);
    CHECK_STR(trim_row(&s, 4), "    abc");

    screen_free(&s);
}

static void test_screen_box(void)
{
    screen s;
    char row[SCREEN_W + 1];

    screen_init(&s, 20, 6);
    screen_clear(&s, 0);
    screen_box(&s, 0, 0, 10, 4, BOX_SINGLE, 7);

    screen_row_text(&s, 0, row);
    CHECK((unsigned char)row[0] == 0xDA, "TL corner is %02X",
          (unsigned char)row[0]);
    CHECK((unsigned char)row[9] == 0xBF, "TR corner is %02X",
          (unsigned char)row[9]);
    CHECK((unsigned char)row[1] == 0xC4, "top edge is %02X",
          (unsigned char)row[1]);
    screen_row_text(&s, 3, row);
    CHECK((unsigned char)row[0] == 0xC0, "BL corner is %02X",
          (unsigned char)row[0]);
    CHECK((unsigned char)row[9] == 0xD9, "BR corner is %02X",
          (unsigned char)row[9]);
    screen_row_text(&s, 1, row);
    CHECK((unsigned char)row[0] == 0xB3, "left edge is %02X",
          (unsigned char)row[0]);
    CHECK(row[1] == ' ', "interior not cleared");

    /* A box too small to draw must do nothing rather than corrupt memory. */
    screen_box(&s, 0, 5, 1, 1, BOX_SINGLE, 7);

    /* Title sits in the border without eating the corner. */
    screen_clear(&s, 0);
    screen_box(&s, 0, 0, 20, 4, BOX_SINGLE, 7);
    screen_box_title(&s, 0, 0, 20, "HI", 7, 7);
    screen_row_text(&s, 0, row);
    CHECK((unsigned char)row[0] == 0xDA, "title ate the corner");
    CHECK((unsigned char)row[2] == 0xB4, "no left title bracket");
    CHECK(row[4] == 'H' && row[5] == 'I', "title text misplaced");
    CHECK((unsigned char)row[7] == 0xC3, "no right title bracket");

    screen_free(&s);
}

static void test_screen_scroll(void)
{
    screen s;

    screen_init(&s, 10, 5);
    screen_clear(&s, 0);
    screen_puts(&s, 0, 0, "aaa", 7);
    screen_puts(&s, 0, 1, "bbb", 7);
    screen_puts(&s, 0, 2, "ccc", 7);

    screen_scroll_up(&s, 0, 0, 10, 3, 1, 7);
    CHECK_STR(trim_row(&s, 0), "bbb");
    CHECK_STR(trim_row(&s, 1), "ccc");
    CHECK_STR(trim_row(&s, 2), "");

    /* Scrolling by more than the height clears rather than reading out of
       bounds. */
    screen_scroll_up(&s, 0, 0, 10, 3, 99, 7);
    CHECK_STR(trim_row(&s, 0), "");

    screen_free(&s);
}

/* --------------------------------------------------------- scrollback */

static void test_sb_wrap(void)
{
    scrollback sb;

    sb_init(&sb);

    /* Short line: one entry, nick prefixed. */
    sb_add(&sb, 40, SB_CHAT, "bob", 0, "hello");
    CHECK(sb.used == 1, "used is %d, want 1", sb.used);
    CHECK_STR(sb.lines[0].text, "<bob> hello");
    CHECK(sb.lines[0].nick_len == 6, "nick_len is %d, want 6",
          sb.lines[0].nick_len);

    /* Long line wraps at a space, continuation indented so it stays
       visually attached to the nick. */
    sb_init(&sb);
    sb_add(&sb, 30, SB_CHAT, "bob",  0,
           "the quick brown fox jumps over the lazy dog");
    CHECK(sb.used >= 2, "did not wrap (used %d)", sb.used);
    CHECK(strlen(sb.lines[0].text) <= 30, "first line is %d cols",
          (int)strlen(sb.lines[0].text));
    CHECK(sb.lines[1].text[0] == ' ' && sb.lines[1].text[1] == ' ',
          "continuation not indented: \"%s\"", sb.lines[1].text);
    CHECK(sb.lines[1].nick_len == 0,
          "continuation claims a nick prefix");

    /* No line may exceed the width, or it overruns the pane. */
    {
        int i;
        for (i = 0; i < sb.used; i++)
            CHECK(strlen(sb.lines[i].text) <= 30,
                  "line %d is %d cols: \"%s\"", i,
                  (int)strlen(sb.lines[i].text), sb.lines[i].text);
    }

    /* A single unbreakable token (a TTH, a URL) must hard-break rather than
       loop forever or overflow. */
    sb_init(&sb);
    sb_add(&sb, 20, SB_CHAT, NULL, 0,
           "QAHL3II3MKN3H3CBMEPNGTAGLIQKURGL2VM2UIIQAHL3II3MKN3H3CB");
    CHECK(sb.used >= 3, "hard break produced %d lines", sb.used);
    CHECK(strlen(sb.lines[0].text) <= 20, "hard break overflowed");

    /* Empty text must not produce a zero-length infinite loop. */
    sb_init(&sb);
    sb_add(&sb, 20, SB_CHAT, "bob", 0, "");
    CHECK(sb.used == 1, "empty message gave %d lines", sb.used);
}

static void test_sb_ring_and_scroll(void)
{
    scrollback sb;
    int i;

    sb_init(&sb);

    /* Overfill the ring: the oldest lines must fall off without corrupting
       the window. */
    for (i = 0; i < SB_LINES + 50; i++) {
        char msg[32];
        snprintf(msg, sizeof msg, "line%d", i);
        sb_add(&sb, 40, SB_CHAT, NULL, 0, msg);
    }
    CHECK(sb.used == SB_LINES, "used is %d, want %d", sb.used, SB_LINES);
    CHECK(sb.count == SB_LINES + 50, "count is %d", sb.count);

    /* The newest line must be the last one added. */
    CHECK_STR(sb.lines[(sb.head + sb.used - 1) % SB_LINES].text,
              "line449");

    /* Scrolling clamps at both ends. */
    CHECK(sb_at_end(&sb), "not pinned to the end after adding");
    sb_scroll(&sb, 10, 20);
    CHECK(sb.scroll == 10, "scroll is %d", sb.scroll);
    sb_scroll(&sb, -999, 20);
    CHECK(sb.scroll == 0, "scroll did not clamp at 0: %d", sb.scroll);
    sb_scroll(&sb, 99999, 20);
    CHECK(sb.scroll == SB_LINES - 20, "scroll did not clamp at max: %d",
          sb.scroll);

    /* A new message must NOT yank a scrolled-back reader to the bottom --
       the single most irritating thing a chat window can do. */
    sb_add(&sb, 40, SB_CHAT, NULL, 0, "interrupting");
    CHECK(sb.scroll != 0, "a new message forced scroll back to the end");

    /* But if pinned, it follows. */
    sb_scroll_end(&sb);
    sb_add(&sb, 40, SB_CHAT, NULL, 0, "another");
    CHECK(sb.scroll == 0, "pinned view did not follow new output");
}

static void test_sb_draw(void)
{
    screen s;
    scrollback sb;
    const theme *th = theme_get(0);

    screen_init(&s, 40, 6);
    sb_init(&sb);

    sb_add(&sb, 38, SB_CHAT, "bob", ATTR(C_YELLOW, C_BLUE), "first");
    sb_add(&sb, 38, SB_CHAT, "bob", ATTR(C_YELLOW, C_BLUE), "second");

    sb_draw(&sb, &s, th, 0, 0, 38, 4);

    /* An underfull pane fills from the TOP and grows downward, the way a
       scrolling terminal region does -- it does not float two lines at the
       bottom of an empty box. */
    CHECK_STR(trim_row(&s, 0), "<bob> first");
    CHECK_STR(trim_row(&s, 1), "<bob> second");
    CHECK_STR(trim_row(&s, 3), "");

    /* The nick is coloured separately from the body. */
    CHECK(screen_at(&s, 0, 0).attr == ATTR(C_YELLOW, C_BLUE),
          "nick not coloured");
    CHECK(screen_at(&s, 7, 0).attr == th->chat_text,
          "body took the nick colour");

    /* Once full, the newest line is on the last row and the oldest has
       scrolled off the top. */
    {
        int i;
        for (i = 0; i < 10; i++) {
            char msg[16];
            snprintf(msg, sizeof msg, "m%d", i);
            sb_add(&sb, 38, SB_CHAT, NULL, 0, msg);
        }
        sb_draw(&sb, &s, th, 0, 0, 38, 4);
        CHECK_STR(trim_row(&s, 3), "m9");
        CHECK_STR(trim_row(&s, 0), "m6");
    }

    screen_free(&s);
}

/* ------------------------------------------------------------ listbox */

static void test_listbox(void)
{
    listbox lb;

    listbox_init(&lb);
    listbox_set_count(&lb, 10);
    CHECK(lb.sel == 0 && lb.top == 0, "init sel/top %d/%d", lb.sel, lb.top);

    listbox_key(&lb, KEY_DOWN, 5);
    CHECK(lb.sel == 1, "sel is %d", lb.sel);

    /* Moving past the window scrolls by the minimum needed. */
    listbox_key(&lb, KEY_END, 5);
    CHECK(lb.sel == 9, "END gave sel %d", lb.sel);
    CHECK(lb.top == 5, "top is %d, want 5", lb.top);

    listbox_key(&lb, KEY_DOWN, 5);
    CHECK(lb.sel == 9, "DOWN past the end moved to %d", lb.sel);

    listbox_key(&lb, KEY_HOME, 5);
    CHECK(lb.sel == 0 && lb.top == 0, "HOME gave %d/%d", lb.sel, lb.top);

    listbox_key(&lb, KEY_UP, 5);
    CHECK(lb.sel == 0, "UP past the start moved to %d", lb.sel);

    listbox_key(&lb, KEY_PGDN, 5);
    CHECK(lb.sel == 5, "PGDN gave %d", lb.sel);
    listbox_key(&lb, KEY_PGUP, 5);
    CHECK(lb.sel == 0, "PGUP gave %d", lb.sel);

    /* The count shrinking under the selection is the real hazard: users
       leave the hub while you are looking at the list. */
    listbox_key(&lb, KEY_END, 5);
    listbox_set_count(&lb, 3);
    CHECK(lb.sel == 2, "sel is %d after shrink, want 2", lb.sel);
    CHECK(lb.top <= lb.sel, "top %d is past sel %d", lb.top, lb.sel);

    listbox_set_count(&lb, 0);
    CHECK(lb.sel == 0 && lb.top == 0, "empty list gave %d/%d",
          lb.sel, lb.top);
    CHECK(listbox_key(&lb, KEY_DOWN, 5) == 0 || lb.sel == 0,
          "moved in an empty list");

    /* An unhandled key must be reported as unhandled so the screen can act
       on it instead. */
    listbox_set_count(&lb, 5);
    CHECK(listbox_key(&lb, 'x', 5) == 0, "claimed an ordinary key");
}

static void test_gauge(void)
{
    screen s;
    const theme *th = theme_get(0);
    char row[SCREEN_W + 1];

    screen_init(&s, 40, 2);
    screen_clear(&s, 0);

    gauge_draw(&s, th, 0, 0, 20, 50);
    screen_row_text(&s, 0, row);
    CHECK(row[0] == '[', "no opening bracket");
    CHECK((unsigned char)row[1] == CH_SHADE_DARK, "filled cell is %02X",
          (unsigned char)row[1]);
    CHECK((unsigned char)row[14] == CH_SHADE_LIGHT, "empty cell is %02X",
          (unsigned char)row[14]);
    CHECK(strstr(row, "50%") != NULL, "no percentage: \"%s\"", row);

    /* Clamping, not wrapping or overflowing. */
    gauge_draw(&s, th, 0, 1, 20, 999);
    screen_row_text(&s, 1, row);
    CHECK(strstr(row, "100%") != NULL, "999%% did not clamp: \"%s\"", row);
    gauge_draw(&s, th, 0, 1, 20, -5);
    screen_row_text(&s, 1, row);
    CHECK(strstr(row, "  0%") != NULL, "-5%% did not clamp: \"%s\"", row);

    screen_free(&s);
}

/* ------------------------------------------------------------- CP437 */

static void test_cp437(void)
{
    char out[64];

    /* ASCII passes through. */
    cp437_from_utf8(out, sizeof out, "Hello");
    CHECK_STR(out, "Hello");

    /* Latin-1 that CP437 has: e-acute is 0x82. */
    cp437_from_utf8(out, sizeof out, "caf\xc3\xa9");
    CHECK((unsigned char)out[3] == 0x82, "e-acute became %02X",
          (unsigned char)out[3]);

    /* Latin that CP437 lacks: transliterate to the base letter rather than
       '?', because "Bjork" is more use than "Bj?rk". U+0159 is r-caron. */
    /* Split literal: "\x99a" would be read as one long hex escape. */
    cp437_from_utf8(out, sizeof out, "Dvo\xc5\x99" "ak");
    CHECK_STR(out, "Dvorak");

    /* Genuinely unmappable becomes '?'. */
    cp437_from_utf8(out, sizeof out, "a\xe6\x97\xa5" "b");
    CHECK_STR(out, "a?b");

    /* Smart quotes and dashes, which a modern client will happily send. */
    cp437_from_utf8(out, sizeof out, "\xe2\x80\x9cquoted\xe2\x80\x9d");
    CHECK_STR(out, "\"quoted\"");
    cp437_from_utf8(out, sizeof out, "a\xe2\x80\x94" "b");
    CHECK_STR(out, "a-b");

    /* Control characters never reach the screen. */
    cp437_from_utf8(out, sizeof out, "a\x01\x1b" "b");
    CHECK_STR(out, "a  b");

    /* Zero-width space is dropped, not turned into a question mark. */
    cp437_from_utf8(out, sizeof out, "a\xe2\x80\x8b" "b");
    CHECK_STR(out, "ab");

    /* Malformed UTF-8 must terminate, not loop or read past the end. */
    cp437_from_utf8(out, sizeof out, "a\xff\xfe" "b");
    CHECK(strlen(out) >= 1, "malformed input produced nothing");
    cp437_from_utf8(out, sizeof out, "a\xc3");
    CHECK(strlen(out) >= 1, "truncated sequence produced nothing");

    /* Truncation rather than overflow. */
    CHECK(cp437_from_utf8(out, 4, "abcdefgh") == 3, "did not truncate to 3");
    CHECK_STR(out, "abc");

    /* Round trip for display: a box-drawing byte becomes the right glyph. */
    {
        char utf[4];
        cp437_to_utf8(0xDA, utf);
        CHECK_STR(utf, "\xe2\x94\x8c");          /* U+250C */
        CHECK(cp437_to_ucs(0xB2) == 0x2593, "shade maps to U+%04X",
              cp437_to_ucs(0xB2));
        /* The low range displays as glyphs in DOS, not control codes. */
        CHECK(cp437_to_ucs(0x10) == 0x25BA, "0x10 maps to U+%04X",
              cp437_to_ucs(0x10));
    }

    /* Sanitiser. */
    {
        char buf[16];
        strcpy(buf, "a\x1b[2Jb");
        cp437_sanitize(buf);
        CHECK_STR(buf, "a [2Jb");
    }
}

static void test_input(void)
{
    inputline in;

    input_init(&in);
    CHECK(input_key(&in, 'h') == 0, "typing returned ready");
    input_key(&in, 'i');
    CHECK_STR(in.text, "hi");
    CHECK(in.cursor == 2, "cursor is %d", in.cursor);

    CHECK(input_key(&in, KEY_ENTER) == 1, "Enter did not report ready");

    /* Editing in the middle. */
    input_key(&in, KEY_LEFT);
    input_key(&in, 'X');
    CHECK_STR(in.text, "hXi");

    input_key(&in, KEY_BACKSPACE);
    CHECK_STR(in.text, "hi");

    input_key(&in, KEY_HOME);
    CHECK(in.cursor == 0, "HOME gave %d", in.cursor);
    input_key(&in, KEY_DEL);
    CHECK_STR(in.text, "i");

    /* Backspace at the start must do nothing, not underflow. */
    input_key(&in, KEY_HOME);
    input_key(&in, KEY_BACKSPACE);
    CHECK_STR(in.text, "i");

    /* Enter on an empty line is not a submission. */
    input_clear(&in);
    CHECK(input_key(&in, KEY_ENTER) == 0, "submitted an empty line");

    /* Escape clears. */
    input_key(&in, 'a');
    input_key(&in, KEY_ESC);
    CHECK_STR(in.text, "");

    /* Overflow must be refused, not overrun. */
    {
        int i;
        for (i = 0; i < INPUT_MAX + 50; i++)
            input_key(&in, 'z');
        CHECK(in.len < INPUT_MAX, "len is %d, cap %d", in.len, INPUT_MAX);
    }

    /* A disabled line ignores everything -- that is how the UI stops you
       typing into a hub you are not logged into yet. */
    input_init(&in);
    in.enabled = 0;
    input_key(&in, 'a');
    CHECK_STR(in.text, "");
}

static void test_themes(void)
{
    int i;

    for (i = 0; i < THEME_COUNT; i++) {
        const theme *t = theme_get(i);
        CHECK(t != NULL && t->name != NULL, "theme %d is unusable", i);
        /* Text invisible against its own background is the one palette bug
           that makes a theme useless. */
        CHECK(ATTR_FG(t->text) != ATTR_BG(t->text),
              "theme %s: body text is invisible", t->name);
        CHECK(ATTR_FG(t->chat_text) != ATTR_BG(t->chat_text),
              "theme %s: chat text is invisible", t->name);
        CHECK(ATTR_FG(t->bar_focus) != ATTR_BG(t->bar_focus),
              "theme %s: focused lightbar is invisible", t->name);
        CHECK(ATTR_FG(t->status) != ATTR_BG(t->status),
              "theme %s: status bar is invisible", t->name);
    }

    CHECK(theme_get(-1) == theme_get(0), "negative index not clamped");
    CHECK(theme_get(999) == theme_get(0), "large index not clamped");
    CHECK(theme_by_name("amiga") != theme_get(0), "named lookup failed");
    CHECK(theme_by_name("nonexistent") == theme_get(0),
          "unknown name did not fall back");
    CHECK(theme_by_name(NULL) == theme_get(0), "NULL name crashed or wrong");
}

/* ---------------------------------------------------- scancode decode */

static void test_scancodes(void)
{
    /* Letters and digits, unshifted and shifted. Scancode set 1. */
    CHECK(kbd_decode(0x1E, 0, 0) == 'a', "0x1E gave %d", kbd_decode(0x1E, 0, 0));
    CHECK(kbd_decode(0x1E, 0, KBD_SHIFT) == 'A', "shift+0x1E wrong");
    CHECK(kbd_decode(0x02, 0, 0) == '1', "0x02 gave %d", kbd_decode(0x02, 0, 0));
    CHECK(kbd_decode(0x02, 0, KBD_SHIFT) == '!', "shift+1 is not !");
    CHECK(kbd_decode(0x39, 0, 0) == ' ', "space wrong");
    CHECK(kbd_decode(0x1C, 0, 0) == KEY_ENTER, "enter wrong");
    CHECK(kbd_decode(0x0E, 0, 0) == KEY_BACKSPACE, "backspace wrong");
    CHECK(kbd_decode(0x0F, 0, 0) == KEY_TAB, "tab wrong");
    CHECK(kbd_decode(0x01, 0, 0) == KEY_ESC, "escape wrong");

    /* The full QWERTY row, since a single transposed table entry is exactly
       the bug nobody notices by hand. */
    {
        static const unsigned char row[] = {
            0x10, 0x11, 0x12, 0x13, 0x14, 0x15, 0x16, 0x17, 0x18, 0x19
        };
        const char *want = "qwertyuiop";
        int i;
        for (i = 0; i < 10; i++)
            CHECK(kbd_decode(row[i], 0, 0) == want[i],
                  "scancode %02X gave '%c', want '%c'",
                  row[i], kbd_decode(row[i], 0, 0), want[i]);
    }
    {
        static const unsigned char row[] = {
            0x1E, 0x1F, 0x20, 0x21, 0x22, 0x23, 0x24, 0x25, 0x26
        };
        const char *want = "asdfghjkl";
        int i;
        for (i = 0; i < 9; i++)
            CHECK(kbd_decode(row[i], 0, 0) == want[i],
                  "scancode %02X gave '%c', want '%c'",
                  row[i], kbd_decode(row[i], 0, 0), want[i]);
    }
    {
        static const unsigned char row[] = {
            0x2C, 0x2D, 0x2E, 0x2F, 0x30, 0x31, 0x32
        };
        const char *want = "zxcvbnm";
        int i;
        for (i = 0; i < 7; i++)
            CHECK(kbd_decode(row[i], 0, 0) == want[i],
                  "scancode %02X gave '%c', want '%c'",
                  row[i], kbd_decode(row[i], 0, 0), want[i]);
    }

    /* Cursor keys, with and without the 0xE0 prefix: the grey keys send the
       prefix, the numeric keypad does not, and both must work. */
    CHECK(kbd_decode(0x48, 1, 0) == KEY_UP, "E0 up wrong");
    CHECK(kbd_decode(0x48, 0, 0) == KEY_UP, "keypad up wrong");
    CHECK(kbd_decode(0x50, 1, 0) == KEY_DOWN, "down wrong");
    CHECK(kbd_decode(0x4B, 1, 0) == KEY_LEFT, "left wrong");
    CHECK(kbd_decode(0x4D, 1, 0) == KEY_RIGHT, "right wrong");
    CHECK(kbd_decode(0x49, 1, 0) == KEY_PGUP, "pgup wrong");
    CHECK(kbd_decode(0x51, 1, 0) == KEY_PGDN, "pgdn wrong");
    CHECK(kbd_decode(0x53, 1, 0) == KEY_DEL, "del wrong");
    CHECK(kbd_decode(0x3B, 0, 0) == KEY_F1, "F1 wrong");
    CHECK(kbd_decode(0x44, 0, 0) == KEY_F10, "F10 wrong");

    /* Keypad Enter and slash are prefixed but are NOT cursor keys -- they
       must fall through to the character tables, not become Home or '/'. */
    CHECK(kbd_decode(0x1C, 1, 0) == KEY_ENTER, "keypad enter wrong");
    CHECK(kbd_decode(0x35, 1, 0) == '/', "keypad slash wrong");

    /* A release is not a keypress. Without this the ISR would emit every
       key twice. */
    CHECK(kbd_decode(0x1E | 0x80, 0, 0) == KEY_NONE, "release produced a key");
    CHECK(kbd_decode(0x48 | 0x80, 1, 0) == KEY_NONE, "arrow release emitted");

    /* Ctrl+letter becomes a control code, matching what the host terminal
       delivers so the UI needs only one code path. */
    CHECK(kbd_decode(0x1E, 0, KBD_CTRL) == 1, "ctrl+A is not 0x01");
    CHECK(kbd_decode(0x2E, 0, KBD_CTRL) == 3, "ctrl+C is not 0x03");

    /* Modifiers themselves produce no character. */
    CHECK(kbd_decode(0x2A, 0, 0) == KEY_NONE, "shift emitted a key");
    CHECK(kbd_decode(0x1D, 0, 0) == KEY_NONE, "ctrl emitted a key");
    CHECK(kbd_decode(0x38, 0, 0) == KEY_NONE, "alt emitted a key");
    CHECK(kbd_decode(0x3A, 0, 0) == KEY_NONE, "capslock emitted a key");

    /* Unmapped codes are silent rather than producing junk. */
    CHECK(kbd_decode(0x7E, 0, 0) == KEY_NONE, "unmapped code emitted a key");

    /* No scancode may decode to a value that collides with the KEY_*
       range, or an arrow key would be indistinguishable from a character. */
    {
        int sc;
        for (sc = 0; sc < 0x80; sc++) {
            int k = kbd_decode((unsigned char)sc, 0, 0);
            CHECK(k == KEY_NONE || k < 0x100 || (k >= KEY_UP && k <= KEY_F10),
                  "scancode %02X decoded to %d, outside both ranges", sc, k);
        }
    }
}

/* ---------------------------------------------------------------- ANSI */

static void test_ansi_strip(void)
{
    char out[256];

    /* Plain text is untouched. */
    ansi_strip(out, sizeof out, "hello");
    CHECK_STR(out, "hello");

    /* SGR colour codes vanish, text survives. */
    ansi_strip(out, sizeof out, "\033[1;32mGREEN\033[0m done");
    CHECK_STR(out, "GREEN done");

    /* The dangerous ones: cursor positioning and screen clear must not
       reach a text-mode screen as escape bytes. */
    ansi_strip(out, sizeof out, "a\033[2J\033[10;1Hb");
    CHECK_STR(out, "ab");

    /* OSC can carry arbitrary text and, on a real terminal, set the
       answerback string -- consumed whole, including its payload. */
    ansi_strip(out, sizeof out, "a\033]0;window title\007b");
    CHECK_STR(out, "ab");
    ansi_strip(out, sizeof out, "a\033]0;title\033\\b");
    CHECK_STR(out, "ab");

    /* An unterminated sequence must consume to the end rather than run off
       the buffer. */
    ansi_strip(out, sizeof out, "a\033[");
    CHECK_STR(out, "a");
    ansi_strip(out, sizeof out, "a\033");
    CHECK_STR(out, "a");
    ansi_strip(out, sizeof out, "a\033]unterminated");
    CHECK_STR(out, "a");

    /* PCBoard @X codes. @@ is a literal at-sign. */
    ansi_strip(out, sizeof out, "@X0Fbright@X07normal");
    CHECK_STR(out, "brightnormal");
    ansi_strip(out, sizeof out, "mail@@example");
    CHECK_STR(out, "mail@example");
    /* Something that only looks like a code is left alone. */
    ansi_strip(out, sizeof out, "@Xzz plain");
    CHECK_STR(out, "@Xzz plain");

    /* Newlines survive; other control bytes become spaces. */
    ansi_strip(out, sizeof out, "a\nb\tc\007d");
    CHECK_STR(out, "a\nb c d");

    /* Truncation, not overflow. */
    CHECK(ansi_strip(out, 4, "abcdefg") == 3, "did not truncate");
    CHECK_STR(out, "abc");

    CHECK(ansi_has_codes("plain") == 0, "false positive");
    CHECK(ansi_has_codes("\033[0m") == 1, "missed an escape");
    CHECK(ansi_has_codes("@X0F") == 1, "missed an @X code");
}

static void test_ansi_draw(void)
{
    screen s;
    const theme *th = theme_get(0);
    int rows;

    screen_init(&s, 40, 10);

    /* Colour is applied, text lands at the origin. */
    screen_clear(&s, 0);
    ansi_draw(&s, 0, 0, 20, 5, th->panel, "\033[1;32mHI", 0);
    CHECK_STR(trim_row(&s, 0), "HI");
    CHECK(ATTR_FG(screen_at(&s, 0, 0).attr) == C_LGREEN,
          "bright green became fg %d", ATTR_FG(screen_at(&s, 0, 0).attr));

    /* ANSI colour order is not VGA's: ANSI 1 is red, VGA 1 is blue. */
    screen_clear(&s, 0);
    ansi_draw(&s, 0, 0, 20, 5, th->panel, "\033[31mR", 0);
    CHECK(ATTR_FG(screen_at(&s, 0, 0).attr) == C_RED,
          "ANSI 31 became fg %d, want red (%d)",
          ATTR_FG(screen_at(&s, 0, 0).attr), C_RED);
    screen_clear(&s, 0);
    ansi_draw(&s, 0, 0, 20, 5, th->panel, "\033[34mB", 0);
    CHECK(ATTR_FG(screen_at(&s, 0, 0).attr) == C_BLUE,
          "ANSI 34 became fg %d, want blue (%d)",
          ATTR_FG(screen_at(&s, 0, 0).attr), C_BLUE);

    /* Cursor positioning is honoured inside the box. */
    screen_clear(&s, 0);
    ansi_draw(&s, 0, 0, 20, 5, th->panel, "\033[3;5HX", 0);
    CHECK(screen_at(&s, 4, 2).ch == 'X', "H positioning put it elsewhere");

    /* THE important one: a hostile MOTD must not escape its rectangle.
       Draw into a 10x3 box at (5,2) and try to write far outside it. */
    screen_clear(&s, ATTR(C_LGREY, C_BLACK));
    screen_puts(&s, 0, 9, "SAFE", 7);
    ansi_draw(&s, 5, 2, 10, 3, th->panel,
              "\033[20;1HESCAPED\033[1;1HALSO", 0);
    CHECK_STR(trim_row(&s, 9), "SAFE");
    {
        int x, y;
        int outside = 0;
        for (y = 0; y < 10; y++)
            for (x = 0; x < 40; x++) {
                int inside = (x >= 5 && x < 15 && y >= 2 && y < 5);
                if (!inside && screen_at(&s, x, y).ch != ' ' && y != 9)
                    outside++;
            }
        CHECK(outside == 0, "%d cells written outside the rectangle",
              outside);
    }

    /* A clear-screen clears only our rectangle. */
    screen_clear(&s, ATTR(C_LGREY, C_BLACK));
    screen_puts(&s, 0, 0, "HEADER", 7);
    ansi_draw(&s, 0, 5, 20, 3, th->panel, "\033[2Jx", 0);
    CHECK_STR(trim_row(&s, 0), "HEADER");

    /* Art taller than the box reports its true height so the caller can
       offer scrolling, and scrolling shows the later rows. */
    screen_clear(&s, 0);
    rows = ansi_draw(&s, 0, 0, 20, 3, th->panel,
                     "r0\nr1\nr2\nr3\nr4\nr5", 0);
    CHECK(rows == 6, "reported %d rows, want 6", rows);
    CHECK_STR(trim_row(&s, 0), "r0");
    screen_clear(&s, 0);
    ansi_draw(&s, 0, 0, 20, 3, th->panel,
              "r0\nr1\nr2\nr3\nr4\nr5", 3);
    CHECK_STR(trim_row(&s, 0), "r3");

    /* Lines wider than the box wrap rather than overwrite the border. */
    screen_clear(&s, 0);
    ansi_draw(&s, 0, 0, 5, 4, th->panel, "abcdefghij", 0);
    CHECK_STR(trim_row(&s, 0), "abcde");
    CHECK_STR(trim_row(&s, 1), "fghij");

    /* PCBoard @X: background 1 (blue), foreground F (bright white). */
    screen_clear(&s, 0);
    ansi_draw(&s, 0, 0, 20, 3, th->panel, "@X1FZ", 0);
    CHECK(screen_at(&s, 0, 0).ch == 'Z', "@X consumed the text");
    CHECK(ATTR_FG(screen_at(&s, 0, 0).attr) == C_WHITE &&
          ATTR_BG(screen_at(&s, 0, 0).attr) == C_BLUE,
          "@X1F gave fg %d bg %d", ATTR_FG(screen_at(&s, 0, 0).attr),
          ATTR_BG(screen_at(&s, 0, 0).attr));

    /* A device query must be dropped, never answered. */
    screen_clear(&s, 0);
    ansi_draw(&s, 0, 0, 20, 3, th->panel, "\033[6nok", 0);
    CHECK_STR(trim_row(&s, 0), "ok");

    screen_free(&s);
}

/* ----------------------------------------------------------------- art */

static void test_art(void)
{
    screen s;
    const theme *th = theme_get(0);
    int x, y;

    screen_init(&s, 80, 25);

    /* The logo draws solid blocks and nothing else -- an earlier version
       stored CP437 high bytes as string literals and rendered as mojibake. */
    screen_clear(&s, 0);
    art_logo(&s, 0, 0, th->text_hi);
    {
        int blocks = 0, other = 0;
        for (y = 0; y < ART_LOGO_ROWS; y++)
            for (x = 0; x < ART_LOGO_COLS; x++) {
                unsigned char ch = screen_at(&s, x, y).ch;
                if (ch == CH_BLOCK_FULL) blocks++;
                else if (ch != ' ') other++;
            }
        CHECK(blocks > 40, "logo drew only %d blocks", blocks);
        CHECK(other == 0, "logo drew %d non-block glyphs", other);
    }

    /* And stays inside its stated size, so callers can lay out around it. */
    CHECK(screen_at(&s, ART_LOGO_COLS, 0).ch == ' ',
          "logo wrote past ART_LOGO_COLS");
    CHECK(screen_at(&s, 0, ART_LOGO_ROWS).ch == ' ',
          "logo wrote past ART_LOGO_ROWS");

    /* The rule shades light to dark across its width. */
    screen_clear(&s, 0);
    art_rule(&s, 0, 0, 30, th->border);
    CHECK(screen_at(&s, 0, 0).ch == CH_SHADE_LIGHT, "rule starts wrong");
    CHECK(screen_at(&s, 29, 0).ch == CH_SHADE_DARK, "rule ends wrong");
    CHECK(screen_at(&s, 30, 0).ch == ' ', "rule overran its width");
    art_rule(&s, 0, 1, 1, th->border);          /* must not divide by zero */
    art_rule(&s, 0, 2, 0, th->border);          /* nor crash on zero width */

    /* The scroller fills exactly its width and wraps rather than running
       off the end of the text. */
    screen_clear(&s, 0);
    art_scroller(&s, 0, 0, 40, 0, th->text);
    CHECK(screen_at(&s, 39, 0).ch != ' ' || 1, "scroller drew nothing");
    CHECK(screen_at(&s, 40, 0).ch == ' ', "scroller overran its width");
    {
        char a[SCREEN_W + 1], b[SCREEN_W + 1];
        screen_row_text(&s, 0, a);
        screen_clear(&s, 0);
        art_scroller(&s, 0, 0, 40, 1, th->text);
        screen_row_text(&s, 0, b);
        CHECK(strcmp(a, b) != 0, "scroller did not move between phases");
        /* A huge phase must still be in range -- it is incremented forever
           while the splash is up. */
        screen_clear(&s, 0);
        art_scroller(&s, 0, 0, 40, 1000000, th->text);
        screen_row_text(&s, 0, b);
        CHECK(strlen(b) > 0, "scroller broke at a large phase");
    }

    /* The banner centres its text between shaded ends, and refuses a width
       it would not fit in rather than writing past it -- it is drawn inside
       a box, and overflowing one is how a door ends up with a hole in its
       border. */
    screen_clear(&s, 0);
    art_banner(&s, 0, 0, 40, " NEWS ", th->title, th->border);
    {
        char row[SCREEN_W + 1];

        screen_row_text(&s, 0, row);
        CHECK(strstr(row, "NEWS") != NULL, "banner lost its text: %s", row);
        CHECK(screen_at(&s, 0, 0).ch == CH_SHADE_LIGHT,
              "banner does not start shaded");
        CHECK(screen_at(&s, 39, 0).ch == CH_SHADE_LIGHT,
              "banner is not mirrored at its right end");
        CHECK(screen_at(&s, 40, 0).ch == ' ', "banner overran its width");
    }

    screen_clear(&s, 0);
    art_banner(&s, 0, 0, 8, "FAR TOO LONG FOR THIS", th->title, th->border);
    {
        char row[SCREEN_W + 1];

        screen_row_text(&s, 0, row);
        CHECK(strspn(row, " ") == strlen(row),
              "banner drew into a width it does not fit: %s", row);
    }

    /* The credits are honest about the music: no tune means it says so,
       rather than naming one that is not playing. */
    screen_clear(&s, 0);
    art_credits(&s, 0, 0, 80, "DOSy++ attic", th->text_hi, th->text_dim);
    {
        char row[SCREEN_W + 1];

        screen_row_text(&s, 1, row);
        CHECK(strstr(row, "DOSy++ attic") != NULL,
              "credits did not name the tune: %s", row);
        CHECK(strchr(row, (char)CH_NOTE) != NULL,
              "credits did not mark the tune with a note glyph");
    }

    screen_clear(&s, 0);
    art_credits(&s, 0, 0, 80, NULL, th->text_hi, th->text_dim);
    {
        char row[SCREEN_W + 1];

        screen_row_text(&s, 1, row);
        CHECK(strstr(row, "NO MUSIC") != NULL,
              "credits claimed music with none playing: %s", row);
    }

    /* The logoff screen says the thing it exists to say, and keeps its box
       intact -- the sign-off used to land on the bottom border. */
    screen_clear(&s, 0);
    art_logoff(&s, th, "The Attic", "Warlord", 3725, 42);
    {
        int found_carrier = 0, found_time = 0;
        char row[SCREEN_W + 1];
        for (y = 0; y < 25; y++) {
            screen_row_text(&s, y, row);
            if (strstr(row, "NO CARRIER")) found_carrier = 1;
            if (strstr(row, "1:02:05")) found_time = 1;
        }
        CHECK(found_carrier, "logoff screen has no NO CARRIER");
        CHECK(found_time, "logoff screen formatted the time wrong");

        /* Checking only the corners was not enough: centred text misses
           them, so "NO CARRIER" sat on the bottom border and the test
           still passed. Assert instead that the row below the sign-off is
           nothing but border glyphs. */
        {
            int carrier_row = -1;
            int bad = 0;
            for (y = 0; y < 25; y++) {
                screen_row_text(&s, y, row);
                if (strstr(row, "NO CARRIER")) {
                    carrier_row = y;
                    break;
                }
            }
            CHECK(carrier_row > 0, "could not find the sign-off row");
            if (carrier_row > 0) {
                for (x = 11; x < 69; x++) {
                    unsigned char ch =
                        screen_at(&s, x, carrier_row + 1).ch;
                    if (ch != 0xCD)           /* the double horizontal */
                        bad++;
                }
                CHECK(bad == 0,
                      "%d cells of the bottom border were overwritten -- "
                      "the sign-off is sitting on it", bad);
                CHECK((unsigned char)screen_at(&s, 10, carrier_row + 1).ch
                          == 0xC8, "bottom-left corner lost");
                CHECK((unsigned char)screen_at(&s, 69, carrier_row + 1).ch
                          == 0xBC, "bottom-right corner lost");
            }
        }
    }

    /* Zero session time must not produce nonsense. */
    screen_clear(&s, 0);
    art_logoff(&s, th, "", "", 0, 0);
    CHECK(1, "logoff survived empty inputs");

    screen_free(&s);
}

/* --------------------------------------------------------------- sound */

/* The row arithmetic, at both heights.
 *
 * This is the thing the old block of #defines could not get wrong and the
 * new function can: every row has to be inside the screen, in order, with
 * nothing sharing a row with anything else. The 25-row answers are written
 * out because they are the layout the whole UI was drawn against -- if they
 * ever change, every door moves. */
static void test_layout(void)
{
    app_layout l = app_layout_for(25);

    CHECK(l.rows == 25, "rows is %d", l.rows);
    CHECK(l.panel_y == 1, "panel_y is %d, was 1", l.panel_y);
    CHECK(l.panel_h == 22, "panel_h is %d, was 22", l.panel_h);
    CHECK(l.chat_top == 2, "chat_top is %d, was 2", l.chat_top);
    CHECK(l.sep_row == 20, "sep_row is %d, was 20", l.sep_row);
    CHECK(l.input_row == 21, "input_row is %d, was 21", l.input_row);
    CHECK(l.status_row == 23, "status_row is %d, was 23", l.status_row);
    CHECK(l.hotkey_row == 24, "hotkey_row is %d, was 24", l.hotkey_row);

    /* The panel's bottom border sits one row above the status line: the
       input row used to land on top of it. */
    CHECK(l.panel_y + l.panel_h - 1 == l.status_row - 1,
          "the panel border is at %d and the status line at %d",
          l.panel_y + l.panel_h - 1, l.status_row);

    {
        int heights[] = { 25, 50, 43, 10, 2 };
        size_t k;

        for (k = 0; k < sizeof heights / sizeof heights[0]; k++) {
            int h = heights[k];

            l = app_layout_for(h);

            /* A screen smaller than the layout needs is clamped rather
               than drawn off the top. */
            if (h < 10) {
                CHECK(l.rows == 10, "%d rows was not clamped: %d",
                      h, l.rows);
                continue;
            }

            CHECK(l.rows == h, "%d rows became %d", h, l.rows);
            CHECK(l.hotkey_row == h - 1,
                  "at %d rows the hotkey bar is at %d", h, l.hotkey_row);
            CHECK(l.status_row < l.hotkey_row,
                  "at %d rows the status line is not above the hotkeys", h);
            CHECK(l.panel_y + l.panel_h - 1 < l.status_row,
                  "at %d rows the panel overlaps the status line", h);
            CHECK(l.chat_top < l.sep_row,
                  "at %d rows there is no room for chat", h);
            CHECK(l.sep_row < l.input_row,
                  "at %d rows the separator is not above the input", h);
            CHECK(l.input_row < l.panel_y + l.panel_h - 1,
                  "at %d rows the input line is on the panel border", h);
        }
    }

    /* Fifty rows gives the chat pane the extra rows, which is the whole
       point of the mode. */
    {
        app_layout tall = app_layout_for(50);
        app_layout short_ = app_layout_for(25);

        CHECK((tall.sep_row - tall.chat_top) ==
              (short_.sep_row - short_.chat_top) + 25,
              "50 rows gave the chat pane %d rows, 25 gave it %d",
              tall.sep_row - tall.chat_top,
              short_.sep_row - short_.chat_top);
    }
}

/* The big logo, and the choice between the two. */
static void test_logo_sizes(void)
{
    screen s;
    const theme *th = theme_get(0);
    int x, y, blocks;

    CHECK(screen_init(&s, 80, 50) == 0, "could not make a 50-row screen");

    screen_clear(&s, 0);
    art_logo_big(&s, 0, 0, th->text_hi);

    blocks = 0;
    for (y = 0; y < ART_LOGO_BIG_ROWS; y++)
        for (x = 0; x < ART_LOGO_BIG_COLS; x++)
            if (screen_at(&s, x, y).ch == CH_BLOCK_FULL)
                blocks++;

    /* Two-cell strokes over nine rows: a lot more than the small logo's
       forty-odd, and the number is a guard against the table losing a row
       to an editor. */
    CHECK(blocks > 200, "the big logo drew only %d blocks", blocks);
    CHECK(screen_at(&s, ART_LOGO_BIG_COLS, 0).ch == ' ',
          "the big logo wrote past its stated width");
    CHECK(screen_at(&s, 0, ART_LOGO_BIG_ROWS).ch == ' ',
          "the big logo wrote past its stated height");

    /* The fitting helper picks by height AND width: a tall screen with a
       narrow panel still gets the small logo, which is what stopped it
       writing over a box border. */
    screen_clear(&s, 0);
    CHECK(art_logo_fit(&s, 0, 0, 80, 50, th->text_hi) == ART_LOGO_BIG_ROWS,
          "a 50-row screen did not get the big logo");

    screen_clear(&s, 0);
    CHECK(art_logo_fit(&s, 0, 0, 40, 50, th->text_hi) == ART_LOGO_ROWS,
          "a 40-column panel got the big logo anyway");

    screen_clear(&s, 0);
    CHECK(art_logo_fit(&s, 0, 0, 80, 25, th->text_hi) == ART_LOGO_ROWS,
          "a 25-row screen got the big logo");

    /* And the logoff screen fills a tall display rather than leaving the
       bottom half of it blank, which it did when the fill was 25 rows. */
    screen_clear(&s, 0);
    art_logoff(&s, th, "The Attic", "Warlord", 60, 2);
    {
        char row[81];
        int blank = 0;

        screen_row_text(&s, 48, row);
        for (x = 0; row[x]; x++)
            if (row[x] == ' ')
                blank++;
        CHECK(blank < 80,
              "the logoff screen left row 48 of a 50-row display empty");

        /* NO CARRIER has to be on screen, wherever the box ended up. */
        blank = 0;
        for (y = 0; y < 50; y++) {
            screen_row_text(&s, y, row);
            if (strstr(row, "NO CARRIER"))
                blank = 1;
        }
        CHECK(blank, "the logoff screen lost NO CARRIER at 50 rows");
    }

    screen_free(&s);
}

static void test_sound(void)
{
    /* Disabled means silent: nothing queued, nothing to tick. */
    snd_init(0);
    CHECK(snd_enabled() == 0, "disabled sound reports enabled");
    snd_connect();
    snd_tick(0);
    snd_tick(10000);
    CHECK(1, "ticking a disabled queue is safe");

    snd_init(1);
    CHECK(snd_enabled() == 1, "enabled sound reports disabled");

    /* A sound must finish rather than repeat forever -- a tone left on
       outlives the program. Advance well past any sequence length. */
    snd_connect();
    {
        unsigned long t;
        for (t = 0; t < 5000; t += 10)
            snd_tick(t);
    }
    snd_tick(100000);
    CHECK(1, "queue drained without hanging");

    /* Starting a new sound while one plays replaces it, so a burst of
       messages cannot back the queue up. */
    snd_message();
    snd_message();
    snd_error();
    snd_tick(1);
    snd_tick(2000);
    CHECK(1, "overlapping sounds are safe");

    snd_shutdown();

    /* The clock has to advance, or nothing ever finishes. */
    {
        unsigned long a = snd_millis();
        unsigned long b;
        volatile int spin;
        for (spin = 0; spin < 2000000; spin++)
            ;
        b = snd_millis();
        CHECK(b >= a, "millisecond clock went backwards (%lu -> %lu)", a, b);
    }
}

int main(void)
{
    printf("DOSy++ UI tests (screen / widgets / CP437 / keys / ANSI / art)\n");

    test_screen_basics();
    test_screen_box();
    test_screen_scroll();
    test_sb_wrap();
    test_sb_ring_and_scroll();
    test_sb_draw();
    test_listbox();
    test_gauge();
    test_cp437();
    test_input();
    test_themes();
    test_scancodes();
    test_ansi_strip();
    test_ansi_draw();
    test_art();
    test_layout();
    test_logo_sizes();
    test_sound();

    /* The hotkey bar, and specifically its last label.
     *
     * Eleven labels want every one of eighty columns. They used to be
     * drawn left to right until one did not fit, and the one that did not
     * fit was G-BYE -- so the only advertised way to leave the program
     * was dropped off the end of its own menu bar, on every door, for
     * every user. It is pinned to the right edge now, and these checks
     * are here because the failure was invisible: the bar looked fine. */
    {
        static const char *const BAR[] = {
            "1-MSGS", "2-WHO", "3-FILES", "4-XFER", "5-SHARE", "6-MOTD",
            "7-INFO", "8-BROWSE", "9-SETUP", "0-HUBS", "G-BYE"
        };
        const int n = (int)(sizeof BAR / sizeof BAR[0]);
        screen sc;
        const theme *th = theme_get(0);
        char row[128];
        int i;

        if (screen_init(&sc, 80, 25) == 0) {
            draw_hotkeys(&sc, th, BAR, n, 0);

            for (i = 0; i < 80; i++)
                row[i] = (char)sc.buf[(sc.h - 1) * sc.w + i].ch;
            row[80] = '\0';

            CHECK(strstr(row, "G-BYE") != NULL,
                  "the quit key is missing from the hotkey bar");
            CHECK(strstr(row, "0-HUBS") != NULL,
                  "the hub list key was pushed off the bar");
            CHECK(strcmp(row + 75, "G-BYE") == 0,
                  "the quit key is not pinned to the right edge");
            CHECK(row[74] == ' ',
                  "no gap between the last door and the quit key");

            /* A narrower screen keeps the quit key and loses middle
               labels, which is the right way round. */
            screen_free(&sc);
            if (screen_init(&sc, 40, 25) == 0) {
                draw_hotkeys(&sc, th, BAR, n, 0);
                for (i = 0; i < 40; i++)
                    row[i] = (char)sc.buf[(sc.h - 1) * sc.w + i].ch;
                row[40] = '\0';
                CHECK(strstr(row, "G-BYE") != NULL,
                      "a narrow bar dropped the quit key instead of a door");
                screen_free(&sc);
            }
        }
    }

    printf("%s: %d checks, %d failed\n", g_fail ? "FAILED" : "ok",
           g_run, g_fail);
    return g_fail ? 1 : 0;
}
