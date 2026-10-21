/* widget.c - scrollback, input line, lightbar list, gauge, furniture. */
#include "widget.h"
#include "kbd.h"

#include <stdio.h>
#include <string.h>

/* ---------------------------------------------------------- scrollback */

void sb_init(scrollback *sb)
{
    memset(sb, 0, sizeof *sb);
}

static sb_line *sb_slot(scrollback *sb)
{
    sb_line *l;

    if (sb->used < SB_LINES) {
        l = &sb->lines[(sb->head + sb->used) % SB_LINES];
        sb->used++;
    } else {
        /* Full: overwrite the oldest and advance the window. */
        l = &sb->lines[sb->head];
        sb->head = (sb->head + 1) % SB_LINES;
    }
    sb->count++;
    return l;
}

static void sb_push(scrollback *sb, sb_kind kind, const char *s,
                    int nick_len, unsigned char nick_attr)
{
    sb_line *l = sb_slot(sb);
    size_t n = strlen(s);

    if (n >= SB_LINE_MAX)
        n = SB_LINE_MAX - 1;
    memcpy(l->text, s, n);
    l->text[n] = '\0';
    l->kind = (unsigned char)kind;
    l->nick_len = (unsigned char)(nick_len > 255 ? 255 : nick_len);
    l->nick_attr = nick_attr;
}

/* One logical message, which may still be wider than the pane. */
static void sb_add_one(scrollback *sb, int width, sb_kind kind,
                       const char *nick, unsigned char nick_attr,
                       const char *text)
{
    char line[SB_LINE_MAX];
    int prefix_len = 0;

    if (width < 16)
        width = 16;
    if (width > SB_LINE_MAX - 1)
        width = SB_LINE_MAX - 1;
    if (!text)
        text = "";

    line[0] = '\0';
    if (nick && nick[0]) {
        snprintf(line, sizeof line, "<%s> ", nick);
        prefix_len = (int)strlen(line);
    }

    /* Wrap at word boundaries, continuing onto indented lines so a long
       message stays visually attached to its nick. */
    {
        const char *p = text;
        int first = 1;
        int indent = prefix_len > 0 ? 2 : 0;

        for (;;) {
            int avail = width - (first ? prefix_len : indent);
            int take, brk;

            if (avail < 8)
                avail = 8;

            if ((int)strlen(p) <= avail) {
                take = (int)strlen(p);
                brk = take;
            } else {
                /* Prefer the last space within reach; fall back to a hard
                   break so a long URL or TTH still renders. */
                brk = -1;
                for (take = avail; take > 0; take--) {
                    if (p[take] == ' ') { brk = take; break; }
                }
                if (brk <= 0) {
                    brk = avail;
                    take = avail;
                } else {
                    take = brk;
                }
            }

            {
                char out[SB_LINE_MAX];
                int o = 0;
                int i;

                if (first && prefix_len > 0) {
                    memcpy(out, line, (size_t)prefix_len);
                    o = prefix_len;
                } else {
                    for (i = 0; i < indent; i++)
                        out[o++] = ' ';
                }
                if (o + take >= SB_LINE_MAX)
                    take = SB_LINE_MAX - 1 - o;
                memcpy(out + o, p, (size_t)take);
                out[o + take] = '\0';

                sb_push(sb, kind, out, first ? prefix_len : 0,
                        nick_attr);
            }

            p += brk;
            while (*p == ' ')
                p++;
            if (!*p)
                break;
            first = 0;
        }
    }

}

void sb_add(scrollback *sb, int width, sb_kind kind, const char *nick,
            unsigned char nick_attr, const char *text)
{
    int pinned = sb_at_end(sb);

    /* Split on newlines first. A hub welcome arrives as one message with
       embedded line breaks; treating it as a single line would run the whole
       thing together and lose the shape of the art. */
    if (text && strchr(text, '\n')) {
        const char *p = text;
        int first = 1;

        while (*p) {
            const char *nl = strchr(p, '\n');
            char part[SB_LINE_MAX];
            size_t n = nl ? (size_t)(nl - p) : strlen(p);

            if (n >= sizeof part)
                n = sizeof part - 1;
            memcpy(part, p, n);
            part[n] = '\0';

            sb_add_one(sb, width, kind, first ? nick : NULL, nick_attr,
                       part);
            first = 0;

            if (!nl)
                break;
            p = nl + 1;
        }
    } else {
        sb_add_one(sb, width, kind, nick, nick_attr, text);
    }

    /* Only follow the newest line if the user had not scrolled back. Yanking
       someone away from what they were reading because a bot spoke is the
       single most irritating thing a chat window can do. */
    if (pinned)
        sb->scroll = 0;
}

int sb_at_end(const scrollback *sb)
{
    return sb->scroll == 0;
}

void sb_scroll(scrollback *sb, int delta, int page_h)
{
    int max = sb->used - page_h;

    if (max < 0)
        max = 0;

    sb->scroll += delta;
    if (sb->scroll < 0)
        sb->scroll = 0;
    if (sb->scroll > max)
        sb->scroll = max;
}

void sb_scroll_end(scrollback *sb)
{
    sb->scroll = 0;
}

void sb_draw(const scrollback *sb, screen *s, const theme *th,
             int x, int y, int w, int h)
{
    int row;
    int first;      /* index into the retained window */

    screen_fill(s, x, y, w, h, ' ', th->panel);

    /* Once there are more lines than rows, the bottom row shows the newest
       unless scrolled back. Below that, `first` clamps to 0 and the pane
       fills downward from the top. */
    first = sb->used - h - sb->scroll;
    if (first < 0)
        first = 0;

    for (row = 0; row < h; row++) {
        int idx = first + row;
        const sb_line *l;
        unsigned char attr;
        unsigned char nick_attr;

        if (idx < 0 || idx >= sb->used)
            continue;

        l = &sb->lines[(sb->head + idx) % SB_LINES];

        switch (l->kind) {
        case SB_ACTION: attr = th->chat_action; break;
        case SB_SYSTEM: attr = th->chat_system; break;
        case SB_PM:     attr = th->chat_pm;     break;
        case SB_ERROR:  attr = th->error;       break;
        default:        attr = th->chat_text;   break;
        }
        /* A zero attribute means the caller did not care; fall back rather
           than painting black-on-black. */
        nick_attr = l->nick_attr ? l->nick_attr : th->nick_user;

        screen_puts_pad(s, x, y + row, l->text, w, attr);

        /* Re-colour just the "<nick>" part. Cheaper and simpler than
           splitting the string at draw time. */
        if (l->nick_len > 0)
            screen_set_attr(s, x, y + row, l->nick_len, nick_attr);
    }

    /* A scrollback indicator, so it is obvious why new messages are not
       appearing at the bottom. */
    if (sb->scroll > 0) {
        char note[32];
        snprintf(note, sizeof note, " %c %d more ", CH_ARROW_DOWN, sb->scroll);
        screen_puts(s, x + w - (int)strlen(note) - 1, y + h - 1, note,
                    th->text_hi);
    }
}

/* ---------------------------------------------------------- input line */

void input_init(inputline *in)
{
    memset(in, 0, sizeof *in);
    in->enabled = 1;
}

void input_clear(inputline *in)
{
    in->text[0] = '\0';
    in->len = 0;
    in->cursor = 0;
    in->scroll = 0;
}

int input_key(inputline *in, int key)
{
    if (!in->enabled)
        return 0;

    switch (key) {
    case KEY_ENTER:
        return in->len > 0 ? 1 : 0;

    case KEY_BACKSPACE:
        if (in->cursor > 0) {
            memmove(in->text + in->cursor - 1, in->text + in->cursor,
                    (size_t)(in->len - in->cursor) + 1);
            in->cursor--;
            in->len--;
        }
        return 0;

    case KEY_DEL:
        if (in->cursor < in->len) {
            memmove(in->text + in->cursor, in->text + in->cursor + 1,
                    (size_t)(in->len - in->cursor));
            in->len--;
        }
        return 0;

    case KEY_LEFT:  if (in->cursor > 0) in->cursor--; return 0;
    case KEY_RIGHT: if (in->cursor < in->len) in->cursor++; return 0;
    case KEY_HOME:  in->cursor = 0; return 0;
    case KEY_END:   in->cursor = in->len; return 0;

    case KEY_ESC:
        input_clear(in);
        return 0;

    default:
        if (key >= 32 && key < 127 && in->len + 1 < INPUT_MAX) {
            memmove(in->text + in->cursor + 1, in->text + in->cursor,
                    (size_t)(in->len - in->cursor) + 1);
            in->text[in->cursor] = (char)key;
            in->cursor++;
            in->len++;
        }
        return 0;
    }
}

void input_draw(const inputline *in, screen *s, const theme *th,
                int x, int y, int w)
{
    int vis = w - 1;
    int scroll = in->scroll;
    unsigned char attr = in->enabled ? th->text : th->text_dim;

    if (vis < 1)
        return;

    /* Keep the cursor in view. Recomputed at draw time rather than stored,
       so it cannot drift out of step with the text. */
    if (in->cursor < scroll)
        scroll = in->cursor;
    if (in->cursor >= scroll + vis)
        scroll = in->cursor - vis + 1;
    if (scroll < 0)
        scroll = 0;

    screen_puts_pad(s, x, y, in->text + scroll, w, attr);

    if (in->enabled)
        screen_cursor(s, x + (in->cursor - scroll), y);
}

/* ------------------------------------------------------------- list box */

void listbox_init(listbox *lb)
{
    memset(lb, 0, sizeof *lb);
}

void listbox_set_count(listbox *lb, int count)
{
    lb->count = count < 0 ? 0 : count;

    if (lb->sel >= lb->count)
        lb->sel = lb->count - 1;
    if (lb->sel < 0)
        lb->sel = 0;
    if (lb->top > lb->sel)
        lb->top = lb->sel;
    if (lb->top < 0)
        lb->top = 0;
}

int listbox_key(listbox *lb, int key, int page_h)
{
    int moved = 0;

    if (page_h < 1)
        page_h = 1;

    switch (key) {
    case KEY_UP:   if (lb->sel > 0) { lb->sel--; moved = 1; } break;
    case KEY_DOWN: if (lb->sel + 1 < lb->count) { lb->sel++; moved = 1; } break;
    case KEY_PGUP: lb->sel -= page_h; moved = 1; break;
    case KEY_PGDN: lb->sel += page_h; moved = 1; break;
    case KEY_HOME: lb->sel = 0; moved = 1; break;
    case KEY_END:  lb->sel = lb->count - 1; moved = 1; break;
    default: return 0;
    }

    if (lb->sel < 0)
        lb->sel = 0;
    if (lb->sel >= lb->count)
        lb->sel = lb->count - 1;
    if (lb->sel < 0)
        lb->sel = 0;

    /* Scroll only as far as needed to bring the selection into view, so
       paging feels like a page rather than a jump. */
    if (lb->sel < lb->top)
        lb->top = lb->sel;
    if (lb->sel >= lb->top + page_h)
        lb->top = lb->sel - page_h + 1;
    if (lb->top < 0)
        lb->top = 0;

    return moved;
}

void listbox_draw(const listbox *lb, screen *s, const theme *th,
                  int x, int y, int w, int h,
                  list_row_fn row, void *user)
{
    int i;

    screen_fill(s, x, y, w, h, ' ', th->panel);

    for (i = 0; i < h; i++) {
        int idx = lb->top + i;
        char text[SCREEN_W + 1];
        unsigned char attr = th->text;

        if (idx >= lb->count)
            break;

        text[0] = '\0';
        row(user, idx, text, sizeof text);

        if (idx == lb->sel)
            attr = lb->focus ? th->bar_focus : th->bar;

        screen_puts_pad(s, x, y + i, text, w, attr);
    }

    /* Position indicator: a scrollbar would cost a column we do not have. */
    if (lb->count > h) {
        char note[24];
        snprintf(note, sizeof note, " %d/%d ", lb->sel + 1, lb->count);
        screen_puts(s, x + w - (int)strlen(note), y + h - 1, note,
                    th->text_dim);
    }
}

/* ---------------------------------------------------------------- gauge */

void gauge_draw(screen *s, const theme *th, int x, int y, int w, int pct)
{
    int bar_w, full, i;
    char label[8];

    if (pct < 0) pct = 0;
    if (pct > 100) pct = 100;
    if (w < 8) return;

    bar_w = w - 6;                   /* room for "[" "]" and " 99%" */
    full = bar_w * pct / 100;        /* integer: no floating point anywhere */

    screen_putc(s, x, y, '[', th->text_dim);
    for (i = 0; i < bar_w; i++)
        screen_putc(s, x + 1 + i, y,
                    i < full ? CH_SHADE_DARK : CH_SHADE_LIGHT,
                    i < full ? th->gauge_full : th->gauge_empty);
    screen_putc(s, x + 1 + bar_w, y, ']', th->text_dim);

    snprintf(label, sizeof label, "%3d%%", pct);
    screen_puts(s, x + 2 + bar_w, y, label, th->text);
}

/* ------------------------------------------------------------ furniture */

void draw_header(screen *s, const theme *th, const char *left,
                 const char *right)
{
    int rlen = right ? (int)strlen(right) : 0;

    screen_fill(s, 0, 0, s->w, 1, ' ', th->header);
    if (left)
        screen_puts(s, 1, 0, left, th->header);
    if (right && rlen + 2 < s->w)
        screen_puts(s, s->w - rlen - 1, 0, right, th->header_hi);
}

void draw_hotkeys(screen *s, const theme *th, const char *const *labels,
                  int count, int active)
{
    int x = 1;
    int i;
    int last = count - 1;
    int tail_len = (count > 0) ? (int)strlen(labels[last]) : 0;
    int tail_x = s->w - tail_len;

    screen_fill(s, 0, s->h - 1, s->w, 1, ' ', th->status);

    /* The last label is pinned to the right edge, and the others stop
       before it.
     *
     * They used to be drawn left to right until one did not fit, and the
     * one that did not fit was G-BYE: eleven labels want 80 columns and
     * the screen has 80, so the only advertised way to leave the program
     * was silently dropped off the end of its own menu bar. Pinning it
     * means it is there whatever the others are called and however wide
     * the screen is -- and a board would have put GOODBYE on the right
     * anyway. */
    for (i = 0; i < last; i++) {
        const char *l = labels[i];
        int len = (int)strlen(l);
        unsigned char attr = (i == active) ? th->bar_focus : th->status;

        if (x + len + 1 > tail_x)
            break;

        screen_puts(s, x, s->h - 1, l, attr);
        /* The leading key character is what the user is looking for. */
        if (i != active)
            screen_set_attr(s, x, s->h - 1, 1, th->status_key);
        x += len + 1;
    }

    /* >= and not >: with a full bar the last label ends exactly where the
       pinned one begins, one blank column between them, and requiring a
       strict gap dropped the pinned label instead -- the same silent
       disappearance this change exists to fix. */
    if (count > 0 && tail_x >= x) {
        unsigned char attr = (last == active) ? th->bar_focus : th->status;

        screen_puts(s, tail_x, s->h - 1, labels[last], attr);
        if (last != active)
            screen_set_attr(s, tail_x, s->h - 1, 1, th->status_key);
    }
}
