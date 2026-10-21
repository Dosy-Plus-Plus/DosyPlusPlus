/* app.c - the BBS shell. */
#include "app.h"
#include "ansi.h"
#include "art.h"
#include "../hash/base32.h"
#include "../hash/tth.h"
#include "../share/filelist.h"
#include "../share/bz.h"
#include "../share/tthlcache.h"
#include "../snd/mus.h"
#include "cp437.h"
#include "kbd.h"
#include "vga.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Row budget for 25 lines, stated once so the panels cannot overlap:
 *    0        header bar
 *    1        panel top border
 *    2..s-6   chat pane            (CHAT_H rows)
 *    s-5      separator inside the panel
 *    s-4      input line
 *    s-3      panel bottom border
 *    s-2      status line
 *    s-1      hotkey bar
 *
 * At 25 rows that is the layout this program was written against: chat on
 * 2..19, separator 20, input 21, border 22, status 23, hotkeys 24. At 50 it
 * is the same shape with 43 rows of chat instead of 18.
 *
 * The values come from app_layout_for() once, at init, and the macros below
 * read them from the app. Each one needs `a` in scope, which every drawing
 * function has. */
#define PANEL_Y      (a->lay.panel_y)
#define PANEL_H      (a->lay.panel_h)
#define CHAT_TOP     (a->lay.chat_top)
#define SEP_ROW      (a->lay.sep_row)
#define INPUT_ROW    (a->lay.input_row)
#define CHAT_H       (a->lay.sep_row - a->lay.chat_top)
#define STATUS_ROW   (a->lay.status_row)
#define HOTKEY_ROW   (a->lay.hotkey_row)
#define SCREEN_ROWS  (a->lay.rows)

/* Eleven labels in eighty columns, with G-BYE pinned to the right edge by
   draw_hotkeys. It fits to the character: 4-XFER rather than 4-XFERS is
   what buys the last column, and losing it drops the hub list's key off
   the bar. Measured, not estimated -- add a letter here and something
   goes missing silently. */
static const char *const HOTKEYS[] = {
    "1-MSGS", "2-WHO", "3-FILES", "4-XFER", "5-SHARE", "6-MOTD",
    "7-INFO", "8-BROWSE", "9-SETUP", "0-HUBS", "G-BYE"
};

/* ------------------------------------------------------------- helpers */

/* Convert hub text (UTF-8) for display: transliterate to CP437, then strip
   every escape sequence and @X code, then flatten what control bytes
   remain.
 *
 * Order matters. Stripping has to happen after the CP437 conversion,
 * because a multi-byte UTF-8 sequence could otherwise hide an ESC from the
 * scanner. Nothing from the network reaches a cell buffer without passing
 * through here; the welcome door is the single deliberate exception, and it
 * renders into a clamped rectangle. */
static void for_display(const char *src, char *dst, size_t dstsz)
{
    char tmp[MOTD_MAX];

    /* ESC and newlines survive the conversion so ansi_strip can do its job:
       flattening the ESC first would leave "[1;36m" on screen as text and
       merge every line of a multi-line message into one. */
    cp437_from_utf8_ex(tmp, sizeof tmp, src ? src : "",
                       CP437_KEEP_ESC | CP437_KEEP_NL);
    ansi_strip(dst, dstsz, tmp);
    /* Newlines are left in place: sb_add splits on them. */
}

/* Single-line contexts -- the status bar, a nick, a column -- where an
   embedded newline would break the layout. */
static void for_display_line(const char *src, char *dst, size_t dstsz)
{
    for_display(src, dst, dstsz);
    cp437_sanitize(dst);
}

/* Keep the hub's welcome text for the welcome door, codes intact. */
static void motd_append(app *a, const char *utf8)
{
    char conv[MOTD_MAX];
    size_t n;

    cp437_from_utf8_ex(conv, sizeof conv, utf8 ? utf8 : "",
                       CP437_KEEP_ESC | CP437_KEEP_NL);
    n = strlen(conv);

    if (a->motd_len + n + 2 >= MOTD_MAX)
        return;                       /* truncate rather than grow */

    if (a->motd_len > 0)
        a->motd[a->motd_len++] = '\n';
    memcpy(a->motd + a->motd_len, conv, n);
    a->motd_len += n;
    a->motd[a->motd_len] = '\0';

    if (ansi_has_codes(conv))
        a->motd_is_art = 1;
}

static void fmt_size(tiger_u64 bytes, char *out, size_t outsz)
{
    static const char *unit[] = { "B", "K", "M", "G", "T" };
    tiger_u64 v = bytes;
    unsigned int u = 0;
    unsigned long frac = 0;

    while (v >= 1024 && u < 4) {
        frac = (unsigned long)((v % 1024) * 10 / 1024);
        v /= 1024;
        u++;
    }
    if (u == 0)
        snprintf(out, outsz, "%lu%s", (unsigned long)v, unit[u]);
    else
        snprintf(out, outsz, "%lu.%lu%s", (unsigned long)v, frac, unit[u]);
}

/* Share sizes, reusing the search formatter so the two doors agree. */
static void share_fmt(const app *a, char *out, size_t outsz, tth_u64 bytes)
{
    (void)a;
    search_fmt_size(bytes, out, outsz);
}

static unsigned char nick_attr(const app *a, const adc_user *u)
{
    if (!u)
        return a->th->chat_system;
    if (u->sid == a->hub->sid)
        return a->th->nick_self;
    if (u->ct & (ADC_CT_OP | ADC_CT_SUPER | ADC_CT_OWNER))
        return a->th->nick_op;
    if (u->ct & ADC_CT_BOT)
        return a->th->nick_bot;
    return a->th->nick_user;
}

void app_status(app *a, const char *msg)
{
    char tmp[80];

    for_display_line(msg, tmp, sizeof tmp);
    snprintf(a->status, sizeof a->status, "%s", tmp);
    a->status_until = a->now + 8;
    a->need_redraw = 1;
}

void app_say_local(app *a, sb_kind kind, const char *text)
{
    char tmp[SB_LINE_MAX];

    for_display(text, tmp, sizeof tmp);
    sb_add(&a->chat, SCREEN_W - 2, kind, NULL, 0, tmp);
    a->need_redraw = 1;
}

/* High-water mark of the user list, for the logoff screen's summary. The
   live count is no good there: by the time you quit, people have left. */
static void note_nodes(app *a)
{
    if (a->hub->users.count > a->nodes_seen)
        a->nodes_seen = a->hub->users.count;
}

/* ----------------------------------------------------- hub callbacks */

static void cb_state(void *user, hub_state st)
{
    app *a = (app *)user;
    char line[96];

    a->connected = (st == HUB_NORMAL);
    a->input.enabled = a->connected;

    /* The protocol state names are for the log, not the user. A BBS told you
       what the modem was doing, so this does too. */
    switch (st) {
    case HUB_IDENTIFY:
        sb_add(&a->chat, SCREEN_W - 2, SB_SYSTEM, NULL, 0,
               "LOGGING ON...");
        break;
    case HUB_VERIFY:
        sb_add(&a->chat, SCREEN_W - 2, SB_SYSTEM, NULL, 0,
               "VERIFYING PASSWORD...");
        break;
    case HUB_NORMAL:
        a->connected_at = a->now;
        snd_connect();
        sb_add(&a->chat, SCREEN_W - 2, SB_SYSTEM, NULL, 0,
               "CONNECT 14400/ARQ/V32bis/V42bis");
        snprintf(line, sizeof line, "You are node %lu of %u online.",
                 (unsigned long)a->hub->sid, a->hub->users.count);
        sb_add(&a->chat, SCREEN_W - 2, SB_SYSTEM, NULL, 0, line);
        app_status(a, "Logged on. Type to post.");
        break;
    case HUB_FAILED:
        snd_disconnect();
        sb_add(&a->chat, SCREEN_W - 2, SB_ERROR, NULL, 0, "NO CARRIER");
        snprintf(line, sizeof line, "%s", hub_error(a->hub));
        sb_add(&a->chat, SCREEN_W - 2, SB_ERROR, NULL, 0, line);
        app_status(a, hub_error(a->hub));
        break;
    default:
        break;
    }

    a->need_redraw = 1;
}

static void cb_chat(void *user, const adc_user *from, const char *text,
                    int me)
{
    app *a = (app *)user;
    char body[MOTD_MAX];
    char nick[ADC_NICK_MAX];

    (void)me;
    for_display(text, body, sizeof body);

    if (!from) {
        /* A message with no sender is the hub talking: the MOTD arrives
           this way, so keep a copy with its art intact. */
        motd_append(a, text);
        sb_add(&a->chat, SCREEN_W - 2, SB_SYSTEM, NULL, 0, body);
    } else if (strncmp(body, "/me ", 4) == 0) {
        char line[SB_LINE_MAX];
        for_display_line(from->nick, nick, sizeof nick);
        snprintf(line, sizeof line, "* %s %s", nick, body + 4);
        sb_add(&a->chat, SCREEN_W - 2, SB_ACTION, NULL,
               nick_attr(a, from), line);
    } else {
        for_display_line(from->nick, nick, sizeof nick);
        sb_add(&a->chat, SCREEN_W - 2, SB_CHAT, nick,
               nick_attr(a, from), body);
    }
    a->need_redraw = 1;
}

static void cb_private(void *user, const adc_user *from, const char *text)
{
    app *a = (app *)user;
    char body[SB_LINE_MAX], nick[ADC_NICK_MAX], line[SB_LINE_MAX];

    for_display(text, body, sizeof body);
    for_display_line(from ? from->nick : "?", nick, sizeof nick);
    snprintf(line, sizeof line, "[NODE MSG from %s] %s", nick, body);
    sb_add(&a->chat, SCREEN_W - 2, SB_PM, NULL, 0, line);
    /* A node message arriving while you are in another door should not be
       silent -- this is the one notification that earns a beep. */
    snd_message();
    a->need_redraw = 1;
}

static void cb_join(void *user, const adc_user *u)
{
    app *a = (app *)user;
    char nick[ADC_NICK_MAX], line[96];

    /* Silent during the login flood: 5,000 join lines would bury the MOTD. */
    if (a->hub->state != HUB_NORMAL) {
        a->need_redraw = 1;
        return;
    }

    note_nodes(a);
    for_display_line(u->nick, nick, sizeof nick);
    snprintf(line, sizeof line, "%c%c %s logged on",
             CH_ARROW_RIGHT, CH_ARROW_RIGHT, nick);
    sb_add(&a->chat, SCREEN_W - 2, SB_SYSTEM, NULL, 0, line);
    a->need_redraw = 1;
}

static void cb_update(void *user, const adc_user *u)
{
    (void)u;
    ((app *)user)->need_redraw = 1;
}

static void cb_part(void *user, adc_sid sid, const char *nickname,
                    const char *reason)
{
    app *a = (app *)user;
    char nick[ADC_NICK_MAX], why[64], line[128];

    (void)sid;
    for_display_line(nickname, nick, sizeof nick);
    for_display_line(reason, why, sizeof why);

    if (nick[0] == '\0')
        snprintf(nick, sizeof nick, "a node");

    snprintf(line, sizeof line, "%c%c %s logged off%s%s",
             CH_ARROW_LEFT, CH_ARROW_LEFT, nick,
             why[0] ? ": " : "", why);
    sb_add(&a->chat, SCREEN_W - 2, SB_SYSTEM, NULL, 0, line);
    a->need_redraw = 1;
}

static void cb_status(void *user, int severity, int code, const char *desc)
{
    app *a = (app *)user;
    char body[96], line[128];

    for_display(desc, body, sizeof body);
    snprintf(line, sizeof line, "[%s %d] %s",
             severity == 2 ? "FATAL" : severity == 1 ? "WARN" : "INFO",
             code, body);
    sb_add(&a->chat, SCREEN_W - 2,
           severity >= 1 ? SB_ERROR : SB_SYSTEM, NULL, 0, line);
    if (severity >= 2)
        snd_error();
    a->need_redraw = 1;
}

/* A search result arrived. Most go straight into the results table for the
   file areas door, which hubconn does on its own; this exists for the other
   case -- a download restored from the queue file, which knows a hash but
   has no idea who has it any more. SIDs do not survive a restart, so the
   only way back to a source is to search for the hash and watch for an
   answer. */
static void cb_result(void *user, const search_result *r)
{
    app *a = (app *)user;
    xq_request *req;

    if (!r || !r->tth[0] || r->nsource <= 0)
        return;

    req = xq_by_tth(&a->xq, r->tth);
    if (!req)
        return;

    {
        int i;
        int added = 0;

        for (i = 0; i < r->nsource; i++)
            if (xq_add_source(&a->xq, req, r->sources[i].sid))
                added++;

        if (added > 0) {
            char line[SB_LINE_MAX];

            snprintf(line, sizeof line,
                     "found a source for %s -- resuming from %luK",
                     req->name, (unsigned long)(req->resume_from / 1024));
            sb_add(&a->chat, SCREEN_W - 2, SB_ACTION, NULL, 0, line);
            a->need_redraw = 1;
        }
    }
}

void app_set_conf_path(app *a, const char *path)
{
    snprintf(a->conf_path, sizeof a->conf_path, "%s",
             (path && path[0]) ? path : "DOSYPP.INI");
}

void app_attach(app *a, hubconn *h)
{
    a->hub = h;
    h->cb.user = a;
    h->cb.on_state = cb_state;
    h->cb.on_chat = cb_chat;
    h->cb.on_private = cb_private;
    h->cb.on_user_join = cb_join;
    h->cb.on_user_update = cb_update;
    h->cb.on_user_part = cb_part;
    h->cb.on_status = cb_status;
    h->cb.on_result = cb_result;
}

/* Defined with their own code, below the dispatcher. */
static void draw_share_door(app *a);
static void draw_xfer_door(app *a);
static void draw_browse_door(app *a);
static void draw_setup_door(app *a);
static void draw_hubs_door(app *a);
static void draw_favs_view(app *a);
static void redraw(app *a);

/* ------------------------------------------------------------- drawing */

static void draw_frame(app *a)
{
    const theme *th = a->th;
    char left[64], right[48];
    char hub[48];

    for_display_line(a->hub->hubname[0] ? a->hub->hubname : a->conf->hub,
                     hub, sizeof hub);
    snprintf(left, sizeof left, "DOSy++  %c  %s", CH_BULLET, hub);

    if (a->connected)
        snprintf(right, sizeof right, "%u NODE%s ONLINE",
                 a->hub->users.count,
                 a->hub->users.count == 1 ? "" : "S");
    else
        snprintf(right, sizeof right, "%s", hub_state_name(a->hub->state));

    draw_header(&a->scr, th, left, right);
    draw_hotkeys(&a->scr, th, HOTKEYS,
                 (int)(sizeof HOTKEYS / sizeof HOTKEYS[0]),
                 (int)a->door);
}


static void draw_status_row(app *a)
{
    const theme *th = a->th;
    char buf[SCREEN_W + 1];

    if (a->status[0] && a->now < a->status_until) {
        snprintf(buf, sizeof buf, " %s", a->status);
    } else {
        char sid[ADC_SID_CHARS + 1];
        adc_sid_encode(a->hub->sid, sid);
        snprintf(buf, sizeof buf,
                 " NODE %s  %c  %s  %c  F1 HELP  %c  PGUP/PGDN SCROLL",
                 sid, CH_BULLET, a->conf->nick, CH_BULLET, CH_BULLET);
    }
    screen_puts_pad(&a->scr, 0, STATUS_ROW, buf, SCREEN_W, th->text_dim);
}

static void draw_chat_door(app *a)
{
    const theme *th = a->th;

    screen_box(&a->scr, 0, PANEL_Y, SCREEN_W, PANEL_H, BOX_SINGLE,
               th->border);
    screen_box_title(&a->scr, 0, PANEL_Y, SCREEN_W, "MESSAGE BASE",
                     th->border, th->title);

    sb_draw(&a->chat, &a->scr, th, 1, CHAT_TOP, SCREEN_W - 2, CHAT_H);

    /* Input prompt, BBS style. */
    screen_hline(&a->scr, 1, SEP_ROW, SCREEN_W - 2, 0xC4, th->border);
    screen_puts(&a->scr, 1, INPUT_ROW, a->connected ? "> " : "  ",
                th->text_hi);
    input_draw(&a->input, &a->scr, th, 3, INPUT_ROW, SCREEN_W - 4);

    /* An empty prompt is a good place to say what it takes. A board would
       have printed its command list here, and the one thing somebody
       needs and cannot guess is how to leave. It disappears the moment
       anything is typed, so it costs nothing to anybody who knows. */
    if (a->input.len == 0) {
        static const char *const HINT = "type to talk    /help for commands"
                                        "    G to hang up";
        int x = SCREEN_W - 2 - (int)strlen(HINT);

        if (x > 4)
            screen_puts(&a->scr, x, INPUT_ROW, HINT, th->text_dim);
    }

    if (!a->connected) {
        screen_puts(&a->scr, 3, INPUT_ROW, "(not connected)", th->text_dim);
        screen_cursor_hide(&a->scr);
    }
}

/* The who list needs the user records in a stable order, which the hash
   index does not provide. Rebuilt per draw: at 5,000 users this is a few
   thousand pointer writes, far cheaper than keeping a sorted index live. */
static void who_row(void *user, int index, char *out, int outsz)
{
    app *a = (app *)user;
    const adc_user *u = adc_userlist_at(&a->hub->users, (unsigned int)index);
    char nick[ADC_NICK_MAX], tag[ADC_TAG_MAX], share[16];

    if (!u) {
        out[0] = '\0';
        return;
    }

    for_display_line(u->nick, nick, sizeof nick);
    for_display_line(u->tag, tag, sizeof tag);
    fmt_size(u->share, share, sizeof share);

    /* Whether they can be dialled is as useful as their share size: a
       passive node cannot be downloaded from unless we are active. */
    snprintf(out, (size_t)outsz, " %-18s %-6s %-7s %8s %4u  %-16s",
             nick[0] ? nick : "(unknown)",
             u->passive ? "PASV" : "ACTV",
             (u->ct & (ADC_CT_OP | ADC_CT_SUPER | ADC_CT_OWNER)) ? "SYSOP" :
             (u->ct & ADC_CT_BOT) ? "BOT" : "USER",
             share, u->slots, tag);
}

static void draw_who_door(app *a)
{
    const theme *th = a->th;
    int top = 4;
    int h = PANEL_Y + PANEL_H - 2 - top;   /* leave the totals row */
    char totals[64];
    char share[16];

    screen_box(&a->scr, 0, PANEL_Y, SCREEN_W, PANEL_H, BOX_SINGLE,
               th->border);
    screen_box_title(&a->scr, 0, PANEL_Y, SCREEN_W, "WHO'S ONLINE",
                     th->border, th->title);

    screen_puts(&a->scr, 1, 2,
                " HANDLE             MODE   ACCESS   UPLD CR  SLT  TERMINAL",
                th->text_hi);
    screen_hline(&a->scr, 1, 3, SCREEN_W - 2, 0xC4, th->border);

    listbox_set_count(&a->who, (int)a->hub->users.count);
    a->who.focus = 1;
    listbox_draw(&a->who, &a->scr, th, 1, top, SCREEN_W - 2, h,
                 who_row, a);

    fmt_size(adc_userlist_total_share(&a->hub->users), share, sizeof share);
    snprintf(totals, sizeof totals,
             " %u node%s online  %c  %s total upload credit",
             a->hub->users.count, a->hub->users.count == 1 ? "" : "s",
             CH_BULLET, share);
    screen_puts_pad(&a->scr, 1, PANEL_Y + PANEL_H - 2, totals, SCREEN_W - 2,
                    th->text_dim);

    screen_cursor_hide(&a->scr);
}

/* A result row: tag marker, name, size, sources, and who has it. */
static void files_row(void *user, int index, char *out, int outsz)
{
    app *a = (app *)user;
    const search_state *ss = &a->hub->search;
    const search_result *r;
    char size[12];
    char nick[ADC_NICK_MAX];

    if (index < 0 || index >= ss->count) {
        out[0] = '\0';
        return;
    }
    r = &ss->results[index];

    if (r->is_dir)
        snprintf(size, sizeof size, "%s", "<DIR>");
    else
        search_fmt_size(r->size, size, sizeof size);

    /* The nick comes from the user list, not from the result: duplicating
       it per source would be stale the moment someone renames. */
    nick[0] = '\0';
    if (r->nsource > 0) {
        adc_user *u = adc_userlist_find(&a->hub->users, r->sources[0].sid);
        if (u)
            for_display_line(u->nick, nick, sizeof nick);
    }

    /* The source count is the most useful number on the row: it is how
       likely the file is to actually arrive. */
    snprintf(out, (size_t)outsz, " %c %-38s %8s %3d  %-14s",
             r->tagged ? CH_ARROW_RIGHT : ' ',
             r->name, size, r->nsource, nick);
}

static void draw_files_door(app *a)
{
    const theme *th = a->th;
    const search_state *ss = &a->hub->search;
    int form_y = PANEL_Y + 1;
    /* form rows at +0..+3, the rule at +4, column headings at +5, list from
       +6. The headings and the rule were both on +4 and the headings won,
       which left the form and the list visually merged. */
    int rule_y = form_y + 4;
    int head_y = form_y + 5;
    int list_y = form_y + 6;
    int list_h = PANEL_Y + PANEL_H - 2 - list_y;
    char buf[96];
    char min_label[24];

    screen_box(&a->scr, 0, PANEL_Y, SCREEN_W, PANEL_H, BOX_SINGLE,
               th->border);
    screen_box_title(&a->scr, 0, PANEL_Y, SCREEN_W, "FILE AREAS",
                     th->border, th->title);

    /* The query form, laid out as a BBS file-scan prompt. */
    screen_puts(&a->scr, 2, form_y, "SCAN FOR :", th->text_dim);
    input_draw(&a->f_terms, &a->scr, th, 13, form_y, 44);
    if (a->sfocus == SF_TERMS)
        screen_set_attr(&a->scr, 13, form_y, 44, th->bar_focus);

    screen_puts(&a->scr, 2, form_y + 1, "NOT      :", th->text_dim);
    input_draw(&a->f_exclude, &a->scr, th, 13, form_y + 1, 28);
    if (a->sfocus == SF_EXCLUDE)
        screen_set_attr(&a->scr, 13, form_y + 1, 28, th->bar_focus);

    screen_puts(&a->scr, 2, form_y + 2, "AREA     :", th->text_dim);
    snprintf(buf, sizeof buf, "%c %-12s %c",
             CH_ARROW_LEFT, search_group_name(a->f_group), CH_ARROW_RIGHT);
    screen_puts_pad(&a->scr, 13, form_y + 2, buf, 18,
                    a->sfocus == SF_GROUP ? th->bar_focus : th->text);

    snprintf(min_label, sizeof min_label, "MIN SIZE : ");
    screen_puts(&a->scr, 2, form_y + 3, min_label, th->text_dim);
    input_draw(&a->f_minsize, &a->scr, th, 13, form_y + 3, 10);
    if (a->sfocus == SF_MINSIZE)
        screen_set_attr(&a->scr, 13, form_y + 3, 10, th->bar_focus);
    screen_puts(&a->scr, 25, form_y + 3, "KB", th->text_dim);

    /* The way out belongs on screen. Somebody who walks into this door
       and finds every digit going into the search box needs to be told
       how to leave, and "press F1" was the only answer the code had. */
    screen_puts(&a->scr, 45, form_y + 1,
                "TAB  next field", th->text_dim);
    screen_puts(&a->scr, 45, form_y + 2,
                "ENTER starts the scan", th->text_dim);
    screen_puts(&a->scr, 45, form_y + 3,
                "ESC  leaves  SPACE tags", th->text_dim);

    screen_hline(&a->scr, 1, rule_y, SCREEN_W - 2, 0xC4, th->border);

    /* Column headings, in the house style. */
    snprintf(buf, sizeof buf, " %c %-38s %8s %3s  %-14s",
             ' ', "FILENAME", "SIZE", "SRC", "FIRST SOURCE");
    screen_puts_pad(&a->scr, 1, head_y, buf, SCREEN_W - 2, th->text_hi);

    if (ss->count == 0) {
        const char *msg;

        if (ss->active && a->now < ss->started + 12)
            msg = "SCANNING THE FILE AREAS ...";
        else if (ss->active || ss->token[0])
            msg = "NOTHING FOUND. TRY FEWER WORDS.";
        else
            msg = "TYPE WHAT YOU ARE AFTER AND PRESS ENTER";

        screen_puts_center(&a->scr, 1, list_y + list_h / 2, SCREEN_W - 2,
                           msg, th->text_dim);
    } else {
        listbox_set_count(&a->files, ss->count);
        a->files.focus = (a->sfocus == SF_RESULTS);
        listbox_draw(&a->files, &a->scr, th, 1, list_y, SCREEN_W - 2,
                     list_h, files_row, a);
    }

    /* Status line for the door: counts, sort order, and the truncation
       notice -- a silently capped list would be misleading. */
    {
        char note[SCREEN_W];
        int tagged = search_tagged_count(ss);

        snprintf(note, sizeof note,
                 " %d result%s  %c  %d tagged  %c  sort: %s%s",
                 ss->count, ss->count == 1 ? "" : "s", CH_BULLET,
                 tagged, CH_BULLET, search_sort_name(a->f_sort),
                 ss->dropped ? "  (list full, some results dropped)" : "");
        screen_puts_pad(&a->scr, 1, PANEL_Y + PANEL_H - 2, note,
                        SCREEN_W - 2, th->text_dim);
    }

    if (a->sfocus == SF_RESULTS || a->sfocus == SF_GROUP)
        screen_cursor_hide(&a->scr);
}

static void draw_welcome_door(app *a)
{
    const theme *th = a->th;
    int inner_y = PANEL_Y + 1;
    int inner_h = PANEL_H - 2;

    screen_box(&a->scr, 0, PANEL_Y, SCREEN_W, PANEL_H, BOX_SINGLE,
               th->border);
    screen_box_title(&a->scr, 0, PANEL_Y, SCREEN_W, "WELCOME",
                     th->border, th->title);

    if (a->motd_len == 0) {
        /* Our own bulletin instead of an empty panel. Plenty of hubs send
           no MOTD at all, and a blank door is the one place where this
           looks like an unfinished program rather than a quiet hub. */
        int y = inner_y + 1;

        art_banner(&a->scr, 2, y, SCREEN_W - 4, " NO BULLETIN FROM THE HUB ",
                   th->title, th->border);
        y = art_logo_fit(&a->scr, 1, y + 2, SCREEN_W - 2, SCREEN_ROWS,
                         th->text_hi) + 1;
        art_rule(&a->scr, 14, y++, SCREEN_W - 28, th->border);
        art_credits(&a->scr, 1, y, SCREEN_W - 2,
                    mus_playing() ? (mus_title()[0] ? mus_title()
                                                    : "a module")
                                  : NULL,
                    th->text_hi, th->text_dim);
        screen_cursor_hide(&a->scr);
        return;
    }

    /* Rendered inside the panel, with cursor movement clamped to it -- so
       even a hostile MOTD cannot reach the header, the hotkey bar, or the
       rest of the screen. */
    a->motd_rows = ansi_draw(&a->scr, 1, inner_y, SCREEN_W - 2, inner_h,
                             th->panel, a->motd, a->motd_scroll);

    if (a->motd_rows > inner_h) {
        char note[40];
        snprintf(note, sizeof note, " %c%c rows %d-%d of %d ",
                 CH_ARROW_UP, CH_ARROW_DOWN,
                 a->motd_scroll + 1,
                 a->motd_scroll + inner_h, a->motd_rows);
        screen_puts(&a->scr, SCREEN_W - 2 - (int)strlen(note),
                    PANEL_Y + PANEL_H - 1, note, th->text_dim);
    }

    if (a->motd_is_art)
        screen_puts(&a->scr, 3, PANEL_Y + PANEL_H - 1, " ANSI ", th->title);

    screen_cursor_hide(&a->scr);
}

/* "1 of 2 free", which is the number other clients see in our INF. Worth
   showing because a user wondering why nobody is downloading from them is
   usually looking at a slot count of zero. */
static const char *slots_line(app *a, char *out, size_t outsz)
{
    int limit = a->conf ? (int)a->conf->slots : 1;
    int free_now = xq_free_upload_slots(&a->xq, limit);

    snprintf(out, outsz, "%d of %d free", free_now, limit);
    return out;
}

static void draw_info_door(app *a)
{
    const theme *th = a->th;
    char sid[ADC_SID_CHARS + 1];
    char buf[96];
    int y = 4;

    screen_box(&a->scr, 0, PANEL_Y, SCREEN_W, PANEL_H, BOX_SINGLE,
               th->border);
    screen_box_title(&a->scr, 0, PANEL_Y, SCREEN_W, "NODE INFO",
                     th->border, th->title);

    adc_sid_encode(a->hub->sid, sid);

#define ROW(label, fmt, val) do {                                          \
        snprintf(buf, sizeof buf, fmt, val);                               \
        screen_puts(&a->scr, 4, y, label, th->text_dim);                   \
        screen_puts(&a->scr, 22, y, buf, th->text);                        \
        y += 1;                                                            \
    } while (0)

    ROW("SYSTEM", "DOSy++ %s for MS-DOS", DOSYPP_VERSION);
    ROW("HUB", "%s", a->hub->hubname[0] ? a->hub->hubname : a->conf->hub);
    ROW("ADDRESS", "%s", a->conf->hub);
    y++;
    ROW("HANDLE", "%s", a->hub->nick);
    ROW("NODE (SID)", "%s", sid);
    ROW("CID", "%s", a->hub->id.cid_b32);
    y++;
    ROW("KEYBOARD", "%s",
        kbd_have_isr() ? "int 9h handler (fast repeat)" : "BIOS int 16h");
    ROW("SPEAKER", "%s", snd_enabled() ? "on" : "off");
    if (mus_playing()) {
        char tune[64];

        for_display_line(mus_title()[0] ? mus_title() : "(untitled)",
                         tune, sizeof tune);
        ROW("MUSIC", "%s", tune);
        ROW("THROUGH", "%s", mus_device());
        if (mus_caveats()[0])
            ROW("NOT PLAYED", "%s", mus_caveats());
    } else {
        ROW("MUSIC", "%s", "off");
    }
    {
        /* Its own buffer: ROW formats into buf, so passing buf as the
           value would have snprintf reading and writing the same object. */
        char slots[32];

        ROW("SLOTS", "%s", slots_line(a, slots, sizeof slots));
    }
    {
        char sh[32];
        share_fmt(a, sh, sizeof sh, a->share.hashed_size);
        snprintf(buf, sizeof buf, "%u files, %s",
                 a->share.hashed_count, sh);
        screen_puts(&a->scr, 4, y, "SHARING", th->text_dim);
        screen_puts(&a->scr, 22, y, buf, th->text);
        y += 1;
    }
    ROW("STATE", "%s", hub_state_name(a->hub->state));
    snprintf(buf, sizeof buf, "%u", a->hub->users.count);
    screen_puts(&a->scr, 4, y, "NODES ONLINE", th->text_dim);
    screen_puts(&a->scr, 22, y, buf, th->text);
    y += 2;

    if (a->hub->err[0]) {
        screen_puts(&a->scr, 4, y, "LAST ERROR", th->text_dim);
        screen_puts(&a->scr, 22, y, a->hub->err, th->error);
        y += 2;
    }

#undef ROW

    /* The command list is not repeated here: there are twenty-five of them
       now and a second copy would be a second thing to forget to update.
       /help prints the table. */
    screen_puts(&a->scr, 4, y,
                "Commands: /help lists all of them. /me, /pm, /whois, "
                "/browse, /search,",
                th->text_dim);
    screen_puts(&a->scr, 4, y + 1,
                "          /stats, /desc, /slots, /connect, /fav, /save, "
                "/raw, /quit.",
                th->text_dim);
    screen_puts(&a->scr, 4, y + 2,
                "Browse: 2 for the node list, B on a node. ENTER opens, "
                "SPACE queues.",
                th->text_dim);
    screen_puts(&a->scr, 4, y + 3,
                "Hubs: 0 for favourites, TAB for the public list, "
                "A adds, P sets a password.",
                th->text_dim);

    screen_cursor_hide(&a->scr);
}

static void redraw(app *a)
{
    screen_fill(&a->scr, 0, 1, SCREEN_W, SCREEN_ROWS - 2,
                a->th->desktop_ch, a->th->desktop);

    switch (a->door) {
    case DOOR_WHO:     draw_who_door(a);     break;
    case DOOR_FILES:   draw_files_door(a);   break;
    case DOOR_XFER:    draw_xfer_door(a);    break;
    case DOOR_SHARE:   draw_share_door(a);   break;
    case DOOR_WELCOME: draw_welcome_door(a); break;
    case DOOR_INFO:    draw_info_door(a);    break;
    case DOOR_BROWSE:  draw_browse_door(a);  break;
    case DOOR_SETUP:   draw_setup_door(a);   break;
    case DOOR_HUBS:    draw_hubs_door(a);    break;
    default:           draw_chat_door(a);    break;
    }

    draw_frame(a);
    draw_status_row(a);

    vga_flush(&a->scr);
    a->need_redraw = 0;
}

/* Defined below; /search reaches it from above. */
static void files_start_search(app *a);

/* ---------------------------------------------------------- the share */

/* How often an unfinished hash run writes its progress to the cache. A
   minute of lost work is an annoyance; half an hour is a reason to stop
   using the program. */
#define HASH_CHECKPOINT_SECS 60

/* What to assume before this machine has hashed anything measurable: the
   benchmarked 486DX2-66 figure from TODO.md. Only ever used to put a number
   on "what would resuming cost", and labelled as an estimate when it is. */
#define HASH_ASSUMED_RATE (517UL * 1024UL)

void app_share_start(app *a)
{
    int i;

    scan_cancel(&a->scan);
    hashq_free(&a->hq);
    a->share_ready = 0;

    share_clear(&a->share);
    a->share.nroot = 0;
    for (i = 0; i < a->conf->nshare; i++)
        share_add_root(&a->share, a->conf->shares[i]);

    if (a->share.nroot == 0) {
        app_status(a, "no share configured -- see NODE INFO");
        a->share_ready = 1;        /* nothing to do, so not "in progress" */
        return;
    }

    if (scan_begin(&a->scan, &a->share) != 0) {
        app_status(a, "could not start the scan");
        return;
    }

    if (hashq_init(&a->hq, &a->share) != 0) {
        app_status(a, "out of memory starting the hasher");
        scan_cancel(&a->scan);
        return;
    }

    a->hash_paused = !a->conf->hash_auto;
    a->hash_run_secs = 0;
    a->hash_last_tick = 0;
    a->hash_saved = a->now;
    app_status(a, "scanning your share...");
    a->need_redraw = 1;
}

/* Everything that has to happen when the hashed set changes in a way other
   nodes should see: the cache so the work survives a restart, the file list
   so a browse is current, and the INF so a hub with a share minimum
   reconsiders. Called at completion and whenever hashing is paused. */
static void share_publish(app *a)
{
    if (!a->hq)
        return;

    hashq_save(a->hq, HASHQ_FILE);

    /* Both copies: the plain XML to look at, the .bz2 because that is what
       a peer asking to browse us receives. A failure here is reported --
       silently serving a stale list is how "nobody can see my files"
       happens. */
    if (filelist_publish(&a->share, FILELIST_FILE, FILELIST_BZ2,
                         a->hub->id.cid_b32) < 0) {
        char msg[96];

        snprintf(msg, sizeof msg, "could not publish the file list: %s",
                 bz_error());
        app_status(a, msg);
        sb_add(&a->chat, SCREEN_W - 2, SB_ERROR, NULL, 0, msg);
    }

    hub_update_share(a->hub);
    a->hash_saved = a->now;
}

int app_hash_toggle(app *a)
{
    if (a->share_ready || !a->hq) {
        app_status(a, "nothing left to hash");
        return a->hash_paused;
    }

    a->hash_paused = !a->hash_paused;

    if (a->hash_paused) {
        /* Checkpoint on the way out. Half an hour of hashing thrown away
           because someone pressed a key would be unforgivable. */
        share_publish(a);
        app_status(a, "hashing paused -- H resumes, and what is already "
                      "hashed stays shared");
    } else {
        a->hash_last_tick = a->now;
        app_status(a, "hashing...");
    }

    a->need_redraw = 1;
    return a->hash_paused;
}

void app_share_tick(app *a)
{
    /* Scanning first: the hasher needs the file list, and enumerating is
       cheap compared with hashing. */
    if (a->scan) {
        int rc = scan_step(&a->scan, 64);

        if (rc < 0) {
            app_status(a, "scan failed");
            scan_cancel(&a->scan);
        } else if (rc == 0) {
            /* Reuse what we hashed on a previous run: at roughly 500 KB/s
               this is the difference between starting in seconds and
               starting in half an hour. */
            int reused = a->hq ? hashq_load(a->hq, HASHQ_FILE) : 0;
            char msg[96];
            unsigned int left = a->share.count - a->share.hashed_count;

            if (left == 0)
                snprintf(msg, sizeof msg,
                         "%u files, all of them already hashed",
                         a->share.count);
            else if (a->hash_paused)
                snprintf(msg, sizeof msg,
                         "%u files, %d from the cache, %u to hash -- "
                         "press H to start",
                         a->share.count, reused, left);
            else
                snprintf(msg, sizeof msg, "%u files, %d already hashed",
                         a->share.count, reused);
            app_status(a, msg);
            a->hash_last_tick = a->now;
            a->scanned_at = a->now;
        }
        a->need_redraw = 1;
        return;
    }

    /* Rescan on a timer, if asked for. Three conditions, each for a
       reason:
     *
     *   - Only once the current pass is finished. Restarting a scan that
     *     is still hashing would lose its place for no gain.
     *   - Never while an upload is running. A rescan clears the index, and
     *     a peer that asked for a file by hash would be told we no longer
     *     have it -- which looks like us lying about our share.
     *   - Never while a download is running either, for the same reason in
     *     reverse: the queue holds pointers into nothing of ours, but the
     *     disk is busy and hashing would fight it for the head. */
    if (a->conf && a->conf->rescan_mins > 0 && a->share_ready &&
        a->scanned_at > 0 &&
        a->now >= a->scanned_at + (unsigned long)a->conf->rescan_mins * 60 &&
        xq_uploads_active(&a->xq) == 0 &&
        xq_downloads_active(&a->xq) == 0) {
        app_status(a, "rescanning the share...");
        app_share_start(a);
        return;
    }

    if (!a->hq || a->share_ready)
        return;

    /* A share that came back fully hashed from the cache is ready whether
       or not hashing was deferred: there is nothing to defer. */
    if (a->hash_paused && a->share.hashed_count < a->share.count) {
        /* Deferred or paused. Deliberately no work at all here, not even a
           reduced budget: the point of the setting is that the disk stays
           quiet. */
        return;
    }

    {
        int rc;

        /* Count only the time actually spent hashing, so a pause does not
           show up as this machine having hashed nothing for ten minutes. */
        if (a->hash_last_tick && a->now > a->hash_last_tick)
            a->hash_run_secs += a->now - a->hash_last_tick;
        a->hash_last_tick = a->now;

        rc = hashq_tick(a->hq, HASHQ_BUDGET);

        if (rc == 0) {
            /* Done. Persist the hashes, write the file list, and tell the
               hub what we actually have -- a hub with a share minimum only
               reconsiders when the INF changes. */
            share_publish(a);
            a->share_ready = 1;
            snd_connect();
            app_status(a, "share ready");
        } else if (rc < 0) {
            /* One unreadable file must not stop the queue. */
            app_status(a, hashq_error(a->hq));
        } else if (a->now - a->hash_saved >= HASH_CHECKPOINT_SECS) {
            /* Checkpoint. Hashing a large share is half an hour of work and
               a DOS machine is not a reliable place to keep half an hour of
               anything in memory: a power cut or a stray TSR should cost a
               minute, not the lot. */
            hashq_save(a->hq, HASHQ_FILE);
            hub_update_share(a->hub);
            a->hash_saved = a->now;
        }
        a->need_redraw = 1;
    }
}

static void draw_share_door(app *a)
{
    const theme *th = a->th;
    int y = PANEL_Y + 2;
    char buf[96];
    char size[16];
    int i;

    screen_box(&a->scr, 0, PANEL_Y, SCREEN_W, PANEL_H, BOX_SINGLE,
               th->border);
    screen_box_title(&a->scr, 0, PANEL_Y, SCREEN_W, "MY SHARE",
                     th->border, th->title);

    if (a->conf->nshare == 0) {
        screen_puts_center(&a->scr, 1, PANEL_Y + 6, SCREEN_W - 2,
                           "YOU ARE NOT SHARING ANYTHING", th->error);
        screen_puts_center(&a->scr, 1, PANEL_Y + 8, SCREEN_W - 2,
                           "Add a line to DOSYPP.INI:", th->text_dim);
        screen_puts_center(&a->scr, 1, PANEL_Y + 9, SCREEN_W - 2,
                           "share = C:\\DL", th->text_hi);
        screen_puts_center(&a->scr, 1, PANEL_Y + 11, SCREEN_W - 2,
                           "then press R to rescan", th->text_dim);
        screen_cursor_hide(&a->scr);
        return;
    }

    screen_puts(&a->scr, 3, y, "SHARING", th->text_dim);
    for (i = 0; i < a->conf->nshare; i++) {
        screen_puts(&a->scr, 14, y, a->conf->shares[i], th->text);
        y++;
    }
    y++;

    share_fmt(a, size, sizeof size, a->share.total_size);
    snprintf(buf, sizeof buf, "%u file%s, %s", a->share.count,
             a->share.count == 1 ? "" : "s", size);
    screen_puts(&a->scr, 3, y, "FOUND", th->text_dim);
    screen_puts(&a->scr, 14, y, buf, th->text);
    y++;

    share_fmt(a, size, sizeof size, a->share.hashed_size);
    snprintf(buf, sizeof buf, "%u file%s, %s", a->share.hashed_count,
             a->share.hashed_count == 1 ? "" : "s", size);
    screen_puts(&a->scr, 3, y, "HASHED", th->text_dim);
    screen_puts(&a->scr, 14, y, buf, th->text);
    y += 2;

    /* Progress. Hashing is slow enough that an honest gauge matters more
       than a tidy one. */
    if (a->scan) {
        screen_puts(&a->scr, 3, y, "SCANNING", th->text_dim);
        screen_puts_pad(&a->scr, 14, y, scan_current(a->scan),
                        SCREEN_W - 16, th->text);
        y += 2;
        snprintf(buf, sizeof buf, "%u directories so far",
                 scan_dirs_done(a->scan));
        screen_puts(&a->scr, 14, y, buf, th->text_dim);
    } else if (a->hq && !a->share_ready) {
        int pct = hashq_percent(a->hq);
        int paused = a->hash_paused;
        tth_u64 left = hashq_total_bytes(a->hq) - hashq_done_bytes(a->hq);
        unsigned long rate = 0;

        /* Measured throughput, from time actually spent hashing. The
           fallback is the benchmarked 486 figure, clearly labelled as an
           estimate -- better than no number at all before the first
           second has elapsed. */
        if (a->hash_run_secs > 0)
            rate = (unsigned long)(hashq_done_bytes(a->hq) /
                                   a->hash_run_secs);

        screen_puts(&a->scr, 3, y, paused ? "PAUSED" : "HASHING",
                    paused ? th->text_hi : th->text_dim);
        screen_puts_pad(&a->scr, 14, y,
                        paused ? "not hashing -- press H to start"
                               : hashq_current(a->hq),
                        SCREEN_W - 16, paused ? th->text_dim : th->text);
        y += 2;
        gauge_draw(&a->scr, th, 14, y, 50, pct);
        y += 2;

        if (paused) {
            /* What resuming would cost, which is the only number that
               helps anyone decide. */
            unsigned long est = rate ? rate : HASH_ASSUMED_RATE;
            unsigned long eta = (unsigned long)(left / est);

            share_fmt(a, size, sizeof size, left);
            if (eta < 60)
                snprintf(buf, sizeof buf,
                         "%lu file%s, %s to go  %c  under a minute at "
                         "%lu KB/s%s",
                         (unsigned long)hashq_files_left(a->hq),
                         hashq_files_left(a->hq) == 1 ? "" : "s",
                         size, CH_BULLET, est / 1024,
                         rate ? "" : " (estimated)");
            else
                snprintf(buf, sizeof buf,
                         "%lu file%s, %s to go  %c  about %lu:%02lu at "
                         "%lu KB/s%s",
                         (unsigned long)hashq_files_left(a->hq),
                         hashq_files_left(a->hq) == 1 ? "" : "s",
                         size, CH_BULLET, eta / 60, eta % 60, est / 1024,
                         rate ? "" : " (estimated)");
        } else if (rate > 0) {
            unsigned long eta = (unsigned long)(left / rate);
            snprintf(buf, sizeof buf,
                     "%lu KB/s  %c  %lu file%s left  %c  about %lu:%02lu left",
                     rate / 1024, CH_BULLET,
                     (unsigned long)hashq_files_left(a->hq),
                     hashq_files_left(a->hq) == 1 ? "" : "s",
                     CH_BULLET, eta / 60, eta % 60);
        } else {
            snprintf(buf, sizeof buf, "%lu file%s left",
                     (unsigned long)hashq_files_left(a->hq),
                     hashq_files_left(a->hq) == 1 ? "" : "s");
        }
        screen_puts_pad(&a->scr, 14, y, buf, SCREEN_W - 16, th->text_dim);

        /* What is hashed is already shared, paused or not -- worth saying,
           because "paused" otherwise reads as "nothing works". */
        if (a->share.hashed_count > 0) {
            y += 2;
            snprintf(buf, sizeof buf,
                     "%u hashed file%s %s findable now",
                     a->share.hashed_count,
                     a->share.hashed_count == 1 ? "" : "s",
                     a->share.hashed_count == 1 ? "is" : "are");
            screen_puts(&a->scr, 14, y, buf, th->chat_action);
        }
    } else if (a->share_ready && a->share.count > 0) {
        screen_puts(&a->scr, 3, y, "STATUS", th->text_dim);
        screen_puts(&a->scr, 14, y, "READY -- other nodes can find these "
                                    "files", th->chat_action);
        y += 2;
        snprintf(buf, sizeof buf, "%lu search repl%s sent this session",
                 a->hub->replies_sent,
                 a->hub->replies_sent == 1 ? "y" : "ies");
        screen_puts(&a->scr, 14, y, buf, th->text_dim);

        /* Leaf sets kept for peers that verify parts of a file. Shown
           because it is disk being used on their behalf, and because it
           explains why the second person to ask gets served instantly. */
        {
            long cached_bytes = 0;
            int cached = tthl_cache_stat(TTHL_CACHE_FILE, &cached_bytes);

            if (cached > 0) {
                y += 1;
                snprintf(buf, sizeof buf,
                         "%d leaf set%s cached (%ldK) for part verification",
                         cached, cached == 1 ? "" : "s",
                         cached_bytes / 1024);
                screen_puts(&a->scr, 14, y, buf, th->text_dim);
            }
        }
    }

    screen_puts(&a->scr, 3, PANEL_Y + PANEL_H - 2,
                a->share_ready
                  ? "R rescans and rehashes  (hashes are cached in DOSYPP.TTH)"
                  : "H pauses or resumes hashing  "
                    "R rescans  (cached in DOSYPP.TTH)",
                th->text_dim);

    screen_cursor_hide(&a->scr);
}


/* Where downloads land. Relative on purpose: on DOS the program's own
   directory is the obvious place, and an absolute path would need the
   config screen Phase 7 has not built yet. */
#define DOWNLOAD_DIR "."


/* ------------------------------------------------------------ the setup
 *
 * The settings, editable, because a client whose only configuration is a
 * text file you have to leave the program to edit is not finished. The
 * fields are described once, as data, and the door is generic over them --
 * which is what stops this becoming nine special cases that drift apart.
 */
typedef enum {
    SF_TEXT = 0,    /* free text */
    SF_NUM,         /* digits */
    SF_BOOL,        /* yes / no, toggled rather than typed */
    SF_CHOICE,      /* one of a list, cycled */
    SF_SECRET       /* text, shown as asterisks */
} setup_kind;

typedef struct {
    const char *label;
    setup_kind  kind;
    const char *help;
    /* Changing this needs a reconnect to take effect, and the door says
       so rather than leaving the user wondering why nothing happened. */
    int         needs_reconnect;
} setup_field;

static const char *const THEME_NAMES[] = { "renegade", "pcboard", "amiga" };

static const setup_field SETUP_FIELDS[] = {
    { "HUB ADDRESS", SF_TEXT,   "hostname or dotted quad",            1 },
    { "HUB PORT",    SF_NUM,    "1511 is the usual ADC port",         1 },
    { "HANDLE",      SF_TEXT,   "what other nodes call you",          1 },
    { "PASSWORD",    SF_SECRET, "for a registered account; stored in "
                                "the clear",                          1 },
    { "DESCRIPTION", SF_TEXT,   "shown beside your handle",           0 },
    { "UPLOAD SLOTS",SF_NUM,    "how many people may take files at "
                                "once",                               0 },
    { "ACTIVE MODE", SF_BOOL,   "needs the port below reachable; "
                                "passive always works",               1 },
    { "LISTEN PORT", SF_NUM,    "only used in active mode",           1 },
    { "HASHING",     SF_CHOICE, "auto hashes after a scan; manual "
                                "waits for H",                        0 },
    { "RESCAN MINS", SF_NUM,    "0 never rescans on its own",         0 },
    { "SOUND",       SF_BOOL,   "the PC speaker",                     0 },
    { "THEME",       SF_CHOICE, "the skin, applied immediately",      0 },
    { "SCREEN ROWS", SF_CHOICE, "50 needs the 8x8 font; takes effect "
                                "on restart",                         0 },
    { "MUSIC",       SF_BOOL,   "the module, started and stopped here", 0 },
    { "MUSIC FILE",  SF_TEXT,   "a ProTracker .MOD beside the "
                                "program",                            0 },
    { "MUSIC RATE",  SF_NUM,    "mixing rate; 11025 suits a 486",     0 }
};

#define SETUP_NFIELD ((int)(sizeof SETUP_FIELDS / sizeof SETUP_FIELDS[0]))

/* The current value of a field, as text. One place, so the door and the
   editor cannot disagree about what is in a field. */
static void setup_value(app *a, int i, char *out, size_t outsz)
{
    cfg *c = a->conf;

    out[0] = '\0';
    if (!c)
        return;

    switch (i) {
    case 0: snprintf(out, outsz, "%s", c->hub); break;
    case 1: snprintf(out, outsz, "%d", c->port); break;
    case 2: snprintf(out, outsz, "%s", c->nick); break;
    case 3: snprintf(out, outsz, "%s", c->password); break;
    case 4: snprintf(out, outsz, "%s", c->desc); break;
    case 5: snprintf(out, outsz, "%u", c->slots); break;
    case 6: snprintf(out, outsz, "%s", c->active ? "yes" : "no"); break;
    case 7: snprintf(out, outsz, "%d", c->listen_port); break;
    case 8: snprintf(out, outsz, "%s", c->hash_auto ? "auto" : "manual");
            break;
    case 9: snprintf(out, outsz, "%d", c->rescan_mins); break;
    case 10: snprintf(out, outsz, "%s", c->sound ? "yes" : "no"); break;
    case 11: snprintf(out, outsz, "%s", c->theme); break;
    case 12: snprintf(out, outsz, "%d", c->rows); break;
    case 13: snprintf(out, outsz, "%s", c->music ? "yes" : "no"); break;
    case 14: snprintf(out, outsz, "%s", c->music_file); break;
    case 15: snprintf(out, outsz, "%d", c->music_rate); break;
    default: break;
    }
}

static void music_begin(app *a);

/* Take typed text, or a toggle, into the config. */
static void setup_set(app *a, int i, const char *text)
{
    cfg *c = a->conf;

    if (!c)
        return;

    switch (i) {
    case 0: snprintf(c->hub, sizeof c->hub, "%s", text); break;
    case 1: c->port = atoi(text); if (c->port <= 0) c->port = 1511; break;
    case 2: snprintf(c->nick, sizeof c->nick, "%s", text); break;
    case 3: snprintf(c->password, sizeof c->password, "%s", text); break;
    case 4: snprintf(c->desc, sizeof c->desc, "%s", text); break;
    case 5: c->slots = (unsigned)atoi(text); break;
    case 6: c->active = !c->active; break;
    case 7: c->listen_port = atoi(text); break;
    case 8: c->hash_auto = !c->hash_auto; break;
    case 9: c->rescan_mins = atoi(text);
            if (c->rescan_mins < 0) c->rescan_mins = 0;
            break;
    case 10:
        c->sound = !c->sound;
        snd_init(c->sound);            /* immediately, so it is testable */
        break;
    case 11: {
        /* Cycle, and apply at once: a theme you cannot see is not a
           choice. */
        int k;

        for (k = 0; k < THEME_COUNT; k++)
            if (strcmp(c->theme, THEME_NAMES[k]) == 0)
                break;
        k = (k + 1) % THEME_COUNT;
        snprintf(c->theme, sizeof c->theme, "%s", THEME_NAMES[k]);
        a->th = theme_by_name(c->theme);
        break;
    }
    case 12:
        /* Cycled rather than typed, because there are two answers. Not
           applied at once: the cell buffer, the layout and the video mode
           would all have to be rebuilt under a running UI, and getting
           that wrong means a screen you cannot read well enough to undo
           it with. The row says so. */
        c->rows = (c->rows >= 50) ? 25 : 50;
        app_status(a, "screen rows change on the next run");
        break;
    case 13:
        /* Immediately, same as the speaker: music you have to restart the
           program to hear is not a setting. */
        c->music = !c->music;
        if (c->music)
            music_begin(a);
        else
            mus_stop();
        break;
    case 14:
        snprintf(c->music_file, sizeof c->music_file, "%s", text);
        if (c->music) { mus_stop(); music_begin(a); }
        break;
    case 15:
        c->music_rate = atoi(text);
        if (c->music_rate < 8000) c->music_rate = 8000;
        if (c->music_rate > 22050) c->music_rate = 22050;
        if (c->music) { mus_stop(); music_begin(a); }
        break;
    default: break;
    }

    a->setup_dirty = 1;
}

/* Start the tune, and say in the chat what happened.
 *
 * A BBS played one; so does this. Failing to is reported where the user
 * will see it and then ignored -- a client that would not run without a
 * sound card would be absurd. Shared by startup and /music so the two
 * cannot disagree about what counts as working. */
static void music_begin(app *a)
{
    cfg *c = a->conf;
    char line[SB_LINE_MAX];

    if (!c || !c->music)
        return;

    if (mus_start(c->music_file, (unsigned long)c->music_rate,
                  c->music_wav[0] ? c->music_wav : NULL) != 0) {
        snprintf(line, sizeof line, "no music: %s", mus_error());
        sb_add(&a->chat, SCREEN_W - 2, SB_SYSTEM, NULL, 0, line);
        return;
    }

    snprintf(line, sizeof line, "%c%c %s %c%c  [%s]",
             CH_NOTE, CH_NOTE,
             mus_title()[0] ? mus_title() : c->music_file,
             CH_NOTE, CH_NOTE, mus_device());
    sb_add(&a->chat, SCREEN_W - 2, SB_ACTION, NULL, 0, line);

    /* Said once, where it will be read, rather than left to be noticed by
       ear: a tune using vibrato plays its notes without it. */
    if (mus_caveats()[0]) {
        snprintf(line, sizeof line,
                 "the tune uses %s, which this player ignores",
                 mus_caveats());
        sb_add(&a->chat, SCREEN_W - 2, SB_SYSTEM, NULL, 0, line);
    }
}

/* Write the settings file. Shared by S in the setup door and /save: two
   copies of this drifted apart once already over which file to write. */
static void setup_save(app *a)
{
    if (cfg_save(a->conf, a->conf_path[0] ? a->conf_path : "DOSYPP.INI")
        == 0) {
        a->setup_dirty = 0;
        app_status(a, "settings written to DOSYPP.INI");
    } else {
        app_status(a, "could not write the settings file");
        snd_error();
    }
}

static void setup_row(void *user, int index, char *out, int outsz)
{
    app *a = (app *)user;
    char value[CFG_STR_MAX];

    if (index < 0 || index >= SETUP_NFIELD) {
        out[0] = '\0';
        return;
    }

    setup_value(a, index, value, sizeof value);

    if (SETUP_FIELDS[index].kind == SF_SECRET && value[0]) {
        size_t n = strlen(value);
        size_t k;

        if (n > 16)
            n = 16;
        for (k = 0; k < n; k++)
            value[k] = '*';
        value[n] = '\0';
    }

    snprintf(out, (size_t)outsz, " %-14s %-28s %s",
             SETUP_FIELDS[index].label, value,
             SETUP_FIELDS[index].needs_reconnect ? "(reconnect)" : "");
}

static void draw_setup_door(app *a)
{
    const theme *th = a->th;
    int top = 3;
    int h = SETUP_NFIELD;
    int sel;

    screen_box(&a->scr, 0, PANEL_Y, SCREEN_W, PANEL_H, BOX_SINGLE,
               th->border);
    screen_box_title(&a->scr, 0, PANEL_Y, SCREEN_W, "NODE SETUP",
                     th->border, th->title);

    screen_puts(&a->scr, 1, 2, " SETTING        VALUE", th->text_hi);

    listbox_set_count(&a->setup, SETUP_NFIELD);
    a->setup.focus = (a->editing < 0);
    listbox_draw(&a->setup, &a->scr, th, 1, top, SCREEN_W - 2, h,
                 setup_row, a);

    sel = a->setup.sel;
    if (sel < 0 || sel >= SETUP_NFIELD)
        sel = 0;

    /* The help line for whatever is selected, and the editor when one is
       open. A settings screen that does not say what a setting does is a
       list of words. */
    if (a->editing >= 0) {
        screen_puts(&a->scr, 3, top + h + 1, "NEW VALUE:", th->text_hi);
        input_draw(&a->setup_input, &a->scr, th, 14, top + h + 1,
                   SCREEN_W - 18);
        screen_puts(&a->scr, 3, top + h + 2,
                    "ENTER accepts  ESC cancels  "
                    "(typing replaces, BACKSPACE edits)", th->text_dim);
    } else {
        screen_puts_pad(&a->scr, 3, top + h + 1,
                        SETUP_FIELDS[sel].help, SCREEN_W - 6, th->text_dim);

        if (a->setup_dirty)
            screen_puts(&a->scr, 3, top + h + 3,
                        "CHANGED -- press S to write DOSYPP.INI",
                        th->error);
        else
            screen_puts(&a->scr, 3, top + h + 3,
                        "ENTER edits or toggles  S saves  "
                        "(settings live in DOSYPP.INI)", th->text_dim);
    }

    screen_cursor_hide(&a->scr);
}

static void setup_key(app *a, int key)
{
    int sel = a->setup.sel;

    if (a->editing >= 0) {
        /* Typing into a field. */
        if (key == KEY_ESC) {
            a->editing = -1;
            a->need_redraw = 1;
            return;
        }

        /* The first printable key replaces what was there; anything else
           (backspace, the arrows) edits it. Both behaviours are wanted:
           most settings are replaced outright, and a mistyped hostname is
           corrected. */
        if (a->setup_fresh) {
            a->setup_fresh = 0;
            if (key >= 32 && key < 127) {
                input_init(&a->setup_input);
                a->setup_input.enabled = 1;
            }
        }

        if (input_key(&a->setup_input, key)) {
            setup_set(a, a->editing, a->setup_input.text);
            a->editing = -1;
            a->need_redraw = 1;
        } else {
            a->need_redraw = 1;
        }
        return;
    }

    if (key == 's' || key == 'S') {
        setup_save(a);
        a->need_redraw = 1;
        return;
    }

    if (key == KEY_ENTER || key == '\r' || key == '\n') {
        setup_kind kind;

        if (sel < 0 || sel >= SETUP_NFIELD)
            return;
        kind = SETUP_FIELDS[sel].kind;

        /* A boolean or a choice has nothing to type: pressing ENTER on it
           moves it on. Making the user type "no" would be worse. */
        if (kind == SF_BOOL || kind == SF_CHOICE) {
            setup_set(a, sel, NULL);
            a->need_redraw = 1;
            return;
        }

        input_init(&a->setup_input);
        a->setup_input.enabled = 1;
        {
            char value[CFG_STR_MAX];
            const char *p;

            setup_value(a, sel, value, sizeof value);
            for (p = value; *p; p++)
                input_key(&a->setup_input, (unsigned char)*p);
        }
        a->editing = sel;
        a->setup_fresh = 1;
        a->need_redraw = 1;
        return;
    }

    if (listbox_key(&a->setup, key, SETUP_NFIELD))
        a->need_redraw = 1;
}



/* ------------------------------------------------------- the favourites
 *
 * Saved as they change rather than on a keypress: a password somebody
 * typed and then lost to a power cut would be the kind of small betrayal
 * that makes a program untrustworthy. The settings door asks for S
 * because its fields are mostly harmless; this one does not.
 */
static void favs_store(app *a)
{
    if (favs_save(&a->favs, FAVS_FILE) == 0) {
        a->favs.dirty = 0;
    } else {
        app_status(a, "could not write DOSYPP.FAV");
        snd_error();
    }
}

static void favs_row(void *user, int index, char *out, int outsz)
{
    app *a = (app *)user;
    const fav_entry *e;
    char name[30];
    char addr[34];

    if (index < 0 || index >= a->favs.count) {
        out[0] = '\0';
        return;
    }
    e = &a->favs.list[index];

    for_display_line(e->name, name, sizeof name);
    for_display_line(e->addr, addr, sizeof addr);

    /* Three marks, because all three are things people get wrong and then
       cannot see: which hub starts automatically, which have a password
       stored, and which handle is being used there. */
    snprintf(out, (size_t)outsz, " %c %-24s %-28s %s%s",
             e->autoconnect ? CH_ARROW_RIGHT : ' ',
             name, addr,
             e->pass[0] ? "PW " : "   ",
             e->nick[0] ? e->nick : "");
}

static const char *FAV_FIELD_NAME[] = { "NAME", "HANDLE", "PASSWORD" };

static void draw_favs_view(app *a)
{
    const theme *th = a->th;
    int top = 4;
    int h = PANEL_Y + PANEL_H - 4 - top;
    int sel = a->favs_box.sel;

    screen_puts(&a->scr, 1, 2,
                "   HUB                      ADDRESS                      ",
                th->text_hi);
    screen_hline(&a->scr, 1, 3, SCREEN_W - 2, 0xC4, th->border);

    if (a->favs.count == 0) {
        screen_puts_center(&a->scr, 1, PANEL_Y + 6, SCREEN_W - 2,
                           "NO FAVOURITE HUBS YET", th->text_dim);
        screen_puts_center(&a->scr, 1, PANEL_Y + 8, SCREEN_W - 2,
                           "A adds the hub you are on now", th->text_hi);
        screen_puts_center(&a->scr, 1, PANEL_Y + 9, SCREEN_W - 2,
                           "TAB shows the public list, where A adds any of "
                           "them", th->text_dim);
        screen_cursor_hide(&a->scr);
        return;
    }

    listbox_set_count(&a->favs_box, a->favs.count);
    a->favs_box.focus = (a->fav_editing < 0);
    listbox_draw(&a->favs_box, &a->scr, th, 1, top, SCREEN_W - 2, h,
                 favs_row, a);

    if (a->fav_editing >= 0) {
        char prompt[24];

        snprintf(prompt, sizeof prompt, "%s:",
                 FAV_FIELD_NAME[a->fav_editing]);
        screen_puts(&a->scr, 3, PANEL_Y + PANEL_H - 3, prompt, th->text_hi);
        input_draw(&a->fav_input, &a->scr, th, 14, PANEL_Y + PANEL_H - 3,
                   SCREEN_W - 18);
        screen_puts(&a->scr, 3, PANEL_Y + PANEL_H - 2,
                    "ENTER accepts  ESC cancels  "
                    "(passwords are stored in the clear)", th->text_dim);
    } else if (a->fav_confirm_del && sel >= 0 && sel < a->favs.count) {
        char msg[96];

        snprintf(msg, sizeof msg,
                 " delete %s and its password? press D again",
                 a->favs.list[sel].name);
        screen_puts_pad(&a->scr, 1, PANEL_Y + PANEL_H - 2, msg,
                        SCREEN_W - 2, th->error);
    } else {
        screen_puts_pad(&a->scr, 1, PANEL_Y + PANEL_H - 2,
                        " ENTER connects  A adds this hub  P password  "
                        "N handle  * auto  D deletes  TAB list",
                        SCREEN_W - 2, th->text_dim);
    }

    screen_cursor_hide(&a->scr);
}

/* Which hub we are on, as an address a favourite can hold. */
static void current_hub_addr(app *a, char *out, size_t outsz)
{
    if (a->conf)
        snprintf(out, outsz, "adc://%s:%d", a->conf->hub, a->conf->port);
    else
        out[0] = '\0';
}

static void favs_add_current(app *a)
{
    char addr[FAV_ADDR_MAX];
    const char *name;
    fav_entry *e;
    char msg[96];

    current_hub_addr(a, addr, sizeof addr);
    if (addr[0] == '\0') {
        app_status(a, "there is no hub to add");
        return;
    }

    /* The hub's own name if it told us one, which is better than an
       address and is what the user recognises. */
    name = (a->hub && a->hub->hubname[0]) ? a->hub->hubname : addr;

    e = favs_add(&a->favs, name, addr);
    if (!e) {
        app_status(a, a->favs.err[0] ? a->favs.err
                                     : "the favourites list is full");
        snd_error();
        return;
    }

    /* Carry over the handle and password actually in use here: they are
       what got us in, so they are what should get us back in. */
    if (a->conf) {
        if (e->nick[0] == '\0' && a->conf->nick[0])
            snprintf(e->nick, sizeof e->nick, "%s", a->conf->nick);
        if (e->pass[0] == '\0' && a->conf->password[0])
            snprintf(e->pass, sizeof e->pass, "%s", a->conf->password);
    }

    favs_store(a);
    snprintf(msg, sizeof msg, "%s added to favourites", e->name);
    app_status(a, msg);
    a->need_redraw = 1;
}

static void favs_add_from_list(app *a)
{
    const hublist_entry *h;
    fav_entry *e;
    char msg[96];

    if (a->hubs_box.sel < 0 || a->hubs_box.sel >= a->hubs.count)
        return;

    h = &a->hubs.entries[a->hubs_box.sel];
    e = favs_add(&a->favs, h->name, h->addr);
    if (!e) {
        app_status(a, a->favs.err[0] ? a->favs.err
                                     : "the favourites list is full");
        snd_error();
        return;
    }

    favs_store(a);
    snprintf(msg, sizeof msg, "%s added to favourites", e->name);
    app_status(a, msg);
    a->need_redraw = 1;
}

/* Connect to a favourite, with its own handle and password. */
static void favs_connect_selected(app *a)
{
    const fav_entry *e;
    char host[CFG_STR_MAX];
    int port = 1511;
    char msg[96];

    if (a->favs_box.sel < 0 || a->favs_box.sel >= a->favs.count)
        return;

    e = &a->favs.list[a->favs_box.sel];

    if (favs_split(e->addr, host, sizeof host, &port) != 0) {
        app_status(a, "that favourite has no usable address");
        snd_error();
        return;
    }

    snprintf(a->switch_to, sizeof a->switch_to, "%s", host);
    a->switch_port = port;
    snprintf(a->switch_nick, sizeof a->switch_nick, "%s", e->nick);
    snprintf(a->switch_pass, sizeof a->switch_pass, "%s", e->pass);
    snprintf(a->switch_label, sizeof a->switch_label, "%s", e->name);
    a->want_switch = 1;

    snprintf(msg, sizeof msg, "connecting to %s...", e->name);
    app_status(a, msg);
    sb_add(&a->chat, SCREEN_W - 2, SB_SYSTEM, NULL, 0, msg);
    a->need_redraw = 1;
}

static void hubs_fetch(app *a);

static void favs_key(app *a, int key)
{
    int sel = a->favs_box.sel;

    /* Editing a field. */
    if (a->fav_editing >= 0) {
        if (key == KEY_ESC) {
            a->fav_editing = -1;
            a->need_redraw = 1;
            return;
        }
        if (input_key(&a->fav_input, key)) {
            if (sel >= 0 && sel < a->favs.count) {
                fav_entry *e = &a->favs.list[sel];

                switch (a->fav_editing) {
                case 0: snprintf(e->name, sizeof e->name, "%s",
                                 a->fav_input.text); break;
                case 1: snprintf(e->nick, sizeof e->nick, "%s",
                                 a->fav_input.text); break;
                case 2: snprintf(e->pass, sizeof e->pass, "%s",
                                 a->fav_input.text); break;
                default: break;
                }
                favs_store(a);
            }
            a->fav_editing = -1;
        }
        a->need_redraw = 1;
        return;
    }

    /* A second D confirms; anything else cancels, so a mistyped D does
       not delete a password. */
    if (a->fav_confirm_del) {
        a->fav_confirm_del = 0;
        if (key == 'd' || key == 'D') {
            if (favs_remove(&a->favs, sel)) {
                favs_store(a);
                listbox_set_count(&a->favs_box, a->favs.count);
                app_status(a, "deleted");
            }
            a->need_redraw = 1;
            return;
        }
        a->need_redraw = 1;
        /* fall through: the key still does whatever it does */
    }

    switch (key) {
    case 'f': case 'F':
        /* Fetching from here means "show me the public list", so it
           switches views as well. Pressing F and staying on an unchanged
           screen would look like nothing happened. */
        a->fav_view = 0;
        hubs_fetch(a);
        return;
    case 'a': case 'A':
        favs_add_current(a);
        return;
    case 'd': case 'D':
        if (sel >= 0 && sel < a->favs.count) {
            a->fav_confirm_del = 1;
            a->need_redraw = 1;
        }
        return;
    case '*': case 'u': case 'U':
        if (sel >= 0 && sel < a->favs.count) {
            favs_set_autoconnect(&a->favs, sel);
            favs_store(a);
            app_status(a, a->favs.list[sel].autoconnect
                       ? "this hub will be dialled on startup"
                       : "no hub will be dialled on startup");
            a->need_redraw = 1;
        }
        return;
    case 'p': case 'P':
    case 'n': case 'N':
    case 'r': case 'R': {
        int field = (key == 'p' || key == 'P') ? 2
                  : (key == 'n' || key == 'N') ? 1 : 0;
        const char *start;

        if (sel < 0 || sel >= a->favs.count)
            return;

        input_init(&a->fav_input);
        a->fav_input.enabled = 1;

        start = (field == 2) ? a->favs.list[sel].pass
              : (field == 1) ? a->favs.list[sel].nick
                             : a->favs.list[sel].name;

        /* The password field opens empty rather than pre-filled: showing
           it in the clear in the editor is a different decision from
           storing it in a file, and one of them can be avoided. */
        if (field != 2) {
            const char *q;

            for (q = start; *q; q++)
                input_key(&a->fav_input, (unsigned char)*q);
        }

        a->fav_editing = field;
        a->need_redraw = 1;
        return;
    }
    case KEY_ENTER: case '\n':      /* KEY_ENTER is already '\r' */
        favs_connect_selected(a);
        return;
    default:
        break;
    }

    listbox_set_count(&a->favs_box, a->favs.count);
    if (listbox_key(&a->favs_box, key, PANEL_Y + PANEL_H - 8))
        a->need_redraw = 1;
}

favs *app_favs(app *a)
{
    return &a->favs;
}

/* ------------------------------------------------------- the hub list
 *
 * Fetched over HTTP, decompressed, parsed, and shown busiest first. The
 * fetch blocks: it is one HTTP GET on a machine with one thread, and
 * pretending otherwise would mean an event loop for a thing that happens
 * once. The screen says what is happening before it starts, and the list
 * is small enough that it is seconds rather than minutes.
 */
static void hubs_row(void *user, int index, char *out, int outsz)
{
    app *a = (app *)user;
    const hublist_entry *e;
    char name[32];
    char addr[40];

    if (index < 0 || index >= a->hubs.count) {
        out[0] = '\0';
        return;
    }
    e = &a->hubs.entries[index];

    for_display_line(e->name, name, sizeof name);
    for_display_line(e->addr, addr, sizeof addr);

    snprintf(out, (size_t)outsz, " %-26s %6lu  %-34s",
             name, e->users, addr);
}

static void draw_hubs_door(app *a)
{
    const theme *th = a->th;
    int top = 4;
    int h = PANEL_Y + PANEL_H - 2 - top;
    char foot[96];

    screen_box(&a->scr, 0, PANEL_Y, SCREEN_W, PANEL_H, BOX_SINGLE,
               th->border);
    screen_box_title(&a->scr, 0, PANEL_Y, SCREEN_W,
                     a->fav_view ? "FAVOURITE HUBS" : "PUBLIC HUB LIST",
                     th->border, th->title);

    if (a->fav_view) {
        draw_favs_view(a);
        return;
    }

    if (a->hubs_fetching) {
        screen_puts_center(&a->scr, 1, PANEL_Y + 6, SCREEN_W - 2,
                           "FETCHING THE HUB LIST...", th->text_hi);
        screen_puts_center(&a->scr, 1, PANEL_Y + 8, SCREEN_W - 2,
                           "this takes a moment on a modem", th->text_dim);
        screen_cursor_hide(&a->scr);
        return;
    }

    if (a->hubs.count == 0) {
        int y = PANEL_Y + 5;

        if (a->hubs.err[0]) {
            screen_puts_center(&a->scr, 1, y, SCREEN_W - 2,
                               "THE HUB LIST COULD NOT BE FETCHED",
                               th->error);
            screen_puts_center(&a->scr, 1, y + 2, SCREEN_W - 2,
                               a->hubs.err, th->text_dim);
        } else {
            screen_puts_center(&a->scr, 1, y, SCREEN_W - 2,
                               "NO HUB LIST YET", th->text_dim);
        }
        screen_puts_center(&a->scr, 1, y + 4, SCREEN_W - 2,
                           "press F to fetch one, TAB for your favourites",
                           th->text_hi);
        screen_puts_center(&a->scr, 1, y + 6, SCREEN_W - 2,
                           "(the address is `hublist` in DOSYPP.INI)",
                           th->text_dim);
        screen_cursor_hide(&a->scr);
        return;
    }

    screen_puts(&a->scr, 1, 2,
                " HUB                        USERS  ADDRESS",
                th->text_hi);
    screen_hline(&a->scr, 1, 3, SCREEN_W - 2, 0xC4, th->border);

    listbox_set_count(&a->hubs_box, a->hubs.count);
    a->hubs_box.focus = 1;
    listbox_draw(&a->hubs_box, &a->scr, th, 1, top, SCREEN_W - 2, h,
                 hubs_row, a);

    if (a->hubs.truncated)
        snprintf(foot, sizeof foot,
                 " %d busiest  %c  ENTER connects  A favourites  "
                 "F refetches  TAB favourites",
                 a->hubs.count, CH_BULLET);
    else
        snprintf(foot, sizeof foot,
                 " %d hubs  %c  ENTER connects  A favourites  F refetches  "
                 "TAB favourites", a->hubs.count, CH_BULLET);

    screen_puts_pad(&a->scr, 1, PANEL_Y + PANEL_H - 2, foot, SCREEN_W - 2,
                    th->text_dim);
    screen_cursor_hide(&a->scr);
}

/* Shown while the fetch runs. The screen cannot redraw during it -- one
   thread -- so this is the only sign of life, and it goes to the same
   place as everything else rather than to stdout. */
static int hubs_progress(void *user, unsigned long got, unsigned long total)
{
    app *a = (app *)user;
    char msg[80];

    if (total > 0)
        snprintf(msg, sizeof msg, "hub list: %luK of %luK",
                 got / 1024, total / 1024);
    else
        snprintf(msg, sizeof msg, "hub list: %luK", got / 1024);

    app_status(a, msg);
    return 0;               /* never cancels: nothing can press a key */
}

static void hubs_fetch(app *a)
{
    const char *url = (a->conf && a->conf->hublist[0])
                      ? a->conf->hublist : HUBLIST_DEFAULT_URL;
    int n;

    a->hubs_fetching = 1;
    app_status(a, "fetching the hub list...");
    redraw(a);                  /* say so before blocking */

    n = hublist_fetch(&a->hubs, url, hubs_progress, a);

    a->hubs_fetching = 0;
    listbox_init(&a->hubs_box);

    if (n < 0) {
        char msg[96];

        snprintf(msg, sizeof msg, "hub list: %s", a->hubs.err);
        app_status(a, msg);
        sb_add(&a->chat, SCREEN_W - 2, SB_ERROR, NULL, 0, msg);
        snd_error();
    } else {
        char msg[96];

        a->hubs_fetched_at = a->now;
        snprintf(msg, sizeof msg, "%d hubs%s%s", n,
                 a->hubs.truncated ? " (the busiest; the list is longer)"
                                   : "",
                 a->hubs.skipped ? ", some skipped as not ADC" : "");
        app_status(a, msg);
        snd_connect();
    }

    a->need_redraw = 1;
}

void app_fetch_hublist(app *a)
{
    a->door = DOOR_HUBS;
    a->fav_view = 0;        /* a fetched list is a list, so show it */
    hubs_fetch(a);
}

/* Pick one. The connection belongs to the main loop, so this records the
   choice and main acts on it. */
static void hubs_connect_selected(app *a)
{
    const hublist_entry *e;
    char host[CFG_STR_MAX];
    int port = 1511;
    char msg[96];
    const char *p;
    char *colon;

    if (a->hubs_box.sel < 0 || a->hubs_box.sel >= a->hubs.count)
        return;

    e = &a->hubs.entries[a->hubs_box.sel];

    /* adc://host:port, or a bare host. Anything else was filtered out
       when the list was parsed. */
    p = e->addr;
    if (strncmp(p, "adc://", 6) == 0)
        p += 6;
    snprintf(host, sizeof host, "%s", p);

    /* Trailing path or slash, which some lists carry. */
    {
        char *slash = strchr(host, '/');

        if (slash)
            *slash = '\0';
    }

    colon = strrchr(host, ':');
    if (colon) {
        int v = atoi(colon + 1);

        if (v > 0 && v < 65536) {
            port = v;
            *colon = '\0';
        }
    }

    if (host[0] == '\0') {
        app_status(a, "that entry has no usable address");
        return;
    }

    snprintf(a->switch_to, sizeof a->switch_to, "%s", host);
    a->switch_port = port;

    /* A hub from the public list has no stored credentials: whatever is in
       the config is what gets used. If it turns out to need an account,
       that is what favourites are for. */
    {
        fav_entry *known = favs_find(&a->favs, e->addr);

        snprintf(a->switch_nick, sizeof a->switch_nick, "%s",
                 known ? known->nick : "");
        snprintf(a->switch_pass, sizeof a->switch_pass, "%s",
                 known ? known->pass : "");
    }
    snprintf(a->switch_label, sizeof a->switch_label, "%s", e->name);
    a->want_switch = 1;

    snprintf(msg, sizeof msg, "connecting to %s:%d...", host, port);
    app_status(a, msg);
    sb_add(&a->chat, SCREEN_W - 2, SB_SYSTEM, NULL, 0, msg);
    a->need_redraw = 1;
}

static void hubs_key(app *a, int key)
{
    /* One door, two lists. TAB between them, as a client with tabs would
       have -- there is no hotkey left on the bar and these two belong
       together anyway. */
    if (key == KEY_TAB) {
        a->fav_view = !a->fav_view;
        a->need_redraw = 1;
        return;
    }

    if (a->fav_view) {
        favs_key(a, key);
        return;
    }

    if (key == 'f' || key == 'F') {
        hubs_fetch(a);
        return;
    }

    if (key == 'a' || key == 'A') {
        favs_add_from_list(a);
        return;
    }

    if (key == KEY_ENTER || key == '\r' || key == '\n') {
        hubs_connect_selected(a);
        return;
    }

    listbox_set_count(&a->hubs_box, a->hubs.count);
    if (listbox_key(&a->hubs_box, key, PANEL_Y + PANEL_H - 6))
        a->need_redraw = 1;
}

void app_hub_failed(app *a, const char *why)
{
    char line[SB_LINE_MAX];

    snprintf(line, sizeof line, "could not connect: %s", why);
    sb_add(&a->chat, SCREEN_W - 2, SB_ERROR, NULL, 0, line);
    app_status(a, line);
    snd_error();
    a->door = DOOR_CHAT;
    a->need_redraw = 1;
    redraw(a);              /* the loop is about to exit; show it first */
}

void app_note(app *a, const char *text)
{
    sb_add(&a->chat, SCREEN_W - 2, SB_SYSTEM, NULL, 0, text);
    app_status(a, text);
    a->need_redraw = 1;
}

int app_take_hub_switch(app *a, app_hub_target *to)
{
    if (!a->want_switch || !to)
        return 0;

    a->want_switch = 0;
    snprintf(to->host, sizeof to->host, "%s", a->switch_to);
    to->port = a->switch_port;
    snprintf(to->nick, sizeof to->nick, "%s", a->switch_nick);
    snprintf(to->pass, sizeof to->pass, "%s", a->switch_pass);
    snprintf(to->label, sizeof to->label, "%s", a->switch_label);
    return 1;
}

/* ------------------------------------------------- somebody else's share */

/* The visible slice of the current directory, recomputed when it changes
   rather than per row: browse_listing walks the whole table, and doing that
   for every one of 18 rows on every redraw is noticeable on a 486. */
#define BROWSE_PAGE 256
static long g_browse_rows[BROWSE_PAGE];
static long g_browse_nrow;

static void browse_refresh(app *a)
{
    g_browse_nrow = browse_listing(&a->browse, g_browse_rows, BROWSE_PAGE);
    listbox_set_count(&a->browse_box, (int)g_browse_nrow);
}

static void browse_row(void *user, int index, char *out, int outsz)
{
    app *a = (app *)user;
    long e;
    char name[64];
    char size[16];

    if (index < 0 || index >= (int)g_browse_nrow) {
        out[0] = '\0';
        return;
    }
    e = g_browse_rows[index];

    for_display_line(browse_name(&a->browse, e), name, sizeof name);

    if (a->browse.ents[e].is_dir) {
        snprintf(out, (size_t)outsz, " %c %-44s %10s",
                 CH_ARROW_RIGHT, name, "<DIR>");
        return;
    }

    fmt_size(a->browse.ents[e].size, size, sizeof size);
    snprintf(out, (size_t)outsz, "   %-44s %10s", name, size);
}

static void draw_browse_door(app *a)
{
    const theme *th = a->th;
    int top = 4;
    int h = PANEL_Y + PANEL_H - 2 - top;
    char path[96];
    char title[64];
    char foot[96];

    screen_box(&a->scr, 0, PANEL_Y, SCREEN_W, PANEL_H, BOX_SINGLE,
               th->border);

    if (a->browse.nick[0])
        snprintf(title, sizeof title, "BROWSING %s", a->browse.nick);
    else
        snprintf(title, sizeof title, "%s", "BROWSE");
    screen_box_title(&a->scr, 0, PANEL_Y, SCREEN_W, title,
                     th->border, th->title);

    if (!a->browse_ready) {
        int y = PANEL_Y + 5;

        if (a->browse.err[0]) {
            screen_puts_center(&a->scr, 1, y, SCREEN_W - 2,
                               "THAT FILE LIST COULD NOT BE READ",
                               th->error);
            screen_puts_center(&a->scr, 1, y + 2, SCREEN_W - 2,
                               a->browse.err, th->text_dim);
        } else if (xq_browse_pending(&a->xq)) {
            screen_puts_center(&a->scr, 1, y, SCREEN_W - 2,
                               "FETCHING A FILE LIST...", th->text_hi);
            screen_puts_center(&a->scr, 1, y + 2, SCREEN_W - 2,
                               "the transfer door shows the progress",
                               th->text_dim);
        } else {
            screen_puts_center(&a->scr, 1, y, SCREEN_W - 2,
                               "NOBODY'S FILES ARE OPEN", th->text_dim);
            screen_puts_center(&a->scr, 1, y + 2, SCREEN_W - 2,
                               "press 2 for WHO'S ONLINE, pick a node, "
                               "then B", th->text_dim);
        }
        screen_cursor_hide(&a->scr);
        return;
    }

    browse_path(&a->browse, path, sizeof path);
    {
        char shown[72];
        for_display_line(path, shown, sizeof shown);
        screen_puts_pad(&a->scr, 1, 2, shown, SCREEN_W - 2, th->text_hi);
    }
    screen_hline(&a->scr, 1, 3, SCREEN_W - 2, 0xC4, th->border);

    a->browse_box.focus = 1;
    listbox_draw(&a->browse_box, &a->scr, th, 1, top, SCREEN_W - 2, h,
                 browse_row, a);

    if (a->browse.truncated)
        snprintf(foot, sizeof foot,
                 " %ld entries -- INCOMPLETE, the list was too big",
                 a->browse.count);
    else
        snprintf(foot, sizeof foot,
                 " %ld entries  %c  ENTER opens, BACKSPACE goes up, "
                 "SPACE queues", a->browse.count, CH_BULLET);

    screen_puts_pad(&a->scr, 1, PANEL_Y + PANEL_H - 2, foot, SCREEN_W - 2,
                    a->browse.truncated ? th->error : th->text_dim);

    screen_cursor_hide(&a->scr);
}

static void browse_step_towards_goal(app *a);

/* A fetched list has landed. Parsed here rather than in the transfer
   callback's caller, because a 20MB list takes real time on a 486 and this
   is the one place that knows the door can be told about it. */
static void browse_arrived(app *a, const char *local, const char *base)
{
    char msg[96];
    const char *nick = "that node";
    adc_user *u = adc_userlist_find(&a->hub->users, a->browse_sid);
    long n;

    if (u && u->nick[0])
        nick = u->nick;

    /* A listing of one directory: merged into the tree under the entry it
       was asked for, and then entered. This is the ordinary case once a
       peer does partial lists -- the whole-list path below is the fallback
       for peers that do not. */
    if (base && base[0] && a->browse_fetching >= 0) {
        long into = a->browse_fetching;

        a->browse_fetching = -1;
        n = browse_merge_bz2(&a->browse, local, into, base);
        remove(local);

        if (n < 0) {
            snprintf(msg, sizeof msg, "could not read that directory");
            app_status(a, msg);
            sb_add(&a->chat, SCREEN_W - 2, SB_ERROR, NULL, 0,
                   browse_error(&a->browse));
        } else {
            browse_enter(&a->browse, into);
            listbox_init(&a->browse_box);
            browse_refresh(a);
            snprintf(msg, sizeof msg, "%ld entries in %s", n,
                     browse_name(&a->browse, into));
            app_status(a, msg);
            browse_step_towards_goal(a);
        }

        a->door = DOOR_BROWSE;
        a->need_redraw = 1;
        return;
    }

    /* Parsing a big list takes real time on a 486 and nothing can redraw
       while it happens, so the screen is brought up to date first. A
       client that simply stops for twenty seconds looks hung. */
    app_status(a, "reading the file list...");
    redraw(a);

    n = browse_load_bz2(&a->browse, local, nick);
    a->browse_ready = (n > 0);
    a->browse_fetching = -1;

    listbox_init(&a->browse_box);
    browse_refresh(a);

    /* The list is a temporary: it is parsed into memory and the compressed
       copy is of no further use. Leaving it would also mean the next browse
       could silently show the last one if the fetch failed. */
    remove(local);

    if (n < 0) {
        snprintf(msg, sizeof msg, "could not read %s's file list", nick);
        app_status(a, msg);
        sb_add(&a->chat, SCREEN_W - 2, SB_ERROR, NULL, 0,
               browse_error(&a->browse));
    } else if (a->browse.truncated) {
        snprintf(msg, sizeof msg,
                 "%ld entries from %s -- the list is incomplete",
                 a->browse.count, nick);
        app_status(a, msg);
    } else {
        snprintf(msg, sizeof msg, "%ld entries from %s%s",
                 a->browse.count, nick,
                 a->browse.whole ? "" : " (one directory at a time)");
        app_status(a, msg);
        snd_connect();

        /* --browse-dir: a root listing is where a walk starts. This was
           first written into the merge branch only, so the walk never took
           its first step -- the root arrives through the load path, not
           the merge one. */
        browse_step_towards_goal(a);
    }

    a->door = DOOR_BROWSE;
    a->need_redraw = 1;
}

void app_browse_goto(app *a, const char *path)
{
    snprintf(a->browse_goto, sizeof a->browse_goto, "%s",
             (path && path[0]) ? path : "");
}

/* Your own share, which is right here.
 *
 * Browsing yourself used to ask the hub to put you in touch with
 * yourself: the queue sent an RCM addressed to our own SID, the hub
 * routed it back, and a passive client cannot answer it -- so the door
 * sat on "NOBODY'S FILES ARE OPEN" with no error and no timeout. It is
 * also the one file list this program never has to ask anybody for.
 *
 * Writes the same list a peer would be sent and hands it to the same
 * reader, so what you see of your own share is what somebody else would
 * see of it, including anything missing from it.
 *
 * Returns 1 when the list was produced, 0 when there is nothing to show.
 */
static int browse_self(app *a, const char *base)
{
    /* 8.3, in the working directory, like every other file this program
       writes. browse_arrived removes it. */
    static const char *const SELF_LIST = "DOSYPP.SLF";
    const char *want = (base && base[0]) ? base : "/";
    int n;

    n = filelist_write_partial(&a->share, want, 0, SELF_LIST,
                               a->hub->id.cid_b32);

    if (n < 0) {
        char msg[96];

        /* A share that has not finished being scanned has nothing to
           list, and that is not the same as an empty one. Asking again
           when it is ready is what somebody pressing B expects -- it took
           a 2-file share to find this, because the scan is asynchronous
           and the browse request beat it by about a second. */
        if (!a->share_ready) {
            a->browse_self_pending = 1;
            app_status(a, "your share is still being scanned -- "
                          "opening it when it is ready");
            a->browse_fetching = -1;
            a->need_redraw = 1;
            return 0;
        }

        snprintf(msg, sizeof msg,
                 a->share.count ? "%s is not in your share"
                                : "nothing shared yet -- press 5",
                 want);
        app_status(a, msg);
        a->browse_fetching = -1;
        a->need_redraw = 1;
        return 0;
    }

    a->browse_self_pending = 0;

    browse_arrived(a, SELF_LIST, base);
    return 1;
}

/* Ask for one directory. Returns 0 if the request went out. */
static int browse_fetch_dir(app *a, long index)
{
    char base[128];
    char msg[128];
    adc_user *u;
    const char *nick = "that node";

    if (a->browse_fetching >= 0) {
        app_status(a, "still waiting for the last directory");
        return -1;
    }

    browse_entry_path(&a->browse, index, base, sizeof base);
    u = adc_userlist_find(&a->hub->users, a->browse_sid);
    if (u && u->nick[0])
        nick = u->nick;

    /* Our own share needs no round trip. */
    if (a->browse_sid == a->hub->sid) {
        a->browse_fetching = index;
        return browse_self(a, base) ? 0 : -1;
    }

    if (!xq_add_list(&a->xq, a->browse_sid, nick, FILELIST_PEER_BZ2,
                     base)) {
        app_status(a, "the transfer queue is full");
        return -1;
    }

    a->browse_fetching = index;
    snprintf(msg, sizeof msg, "asking %s for %s...", nick, base);
    app_status(a, msg);
    a->need_redraw = 1;
    return 0;
}

/* One step towards browse_goto, if there is one to take.
 *
 * Called whenever a listing lands. Each step either enters a directory
 * already in the tree or asks for it, so a path three deep costs three
 * round trips -- which is exactly what browsing it by hand would cost. */
static void browse_step_towards_goal(app *a)
{
    char here[160];
    const char *want;
    long rows[BROWSE_PAGE];
    long n, i;
    size_t hn;

    if (!a->browse_goto[0] || a->browse_fetching >= 0)
        return;

    browse_path(&a->browse, here, sizeof here);

    /* Under --peer-trace, each step: the wire shows the requests, this
       shows the intent, and the two together are what made a walk that
       never started obvious. */
    if (peer_trace_enabled())
        printf("browse: at %s, heading for %s\n", here, a->browse_goto);

    /* The goal, relative to where we are. Both are "/a/b" with an
       optional trailing slash, so the comparison is a prefix test with
       the slashes normalised away. */
    hn = strlen(here);
    while (hn > 0 && here[hn - 1] == '/')
        here[--hn] = '\0';

    want = a->browse_goto;
    if (hn > 0) {
        if (strncmp(want, here, hn) != 0) {
            /* Somewhere else entirely: give up rather than wander. */
            a->browse_goto[0] = '\0';
            return;
        }
        want += hn;
    }
    while (*want == '/')
        want++;

    if (*want == '\0') {
        a->browse_goto[0] = '\0';      /* arrived */
        app_status(a, "opened");
        return;
    }

    /* The next component, and the child that matches it. */
    {
        char comp[64];
        size_t k = 0;

        while (want[k] && want[k] != '/' && k + 1 < sizeof comp) {
            comp[k] = want[k];
            k++;
        }
        comp[k] = '\0';

        n = browse_listing(&a->browse, rows, BROWSE_PAGE);
        for (i = 0; i < n; i++) {
            if (!a->browse.ents[rows[i]].is_dir)
                continue;
            if (strcmp(browse_name(&a->browse, rows[i]), comp) != 0)
                continue;

            if (browse_needs_fetch(&a->browse, rows[i])) {
                browse_fetch_dir(a, rows[i]);
            } else {
                browse_enter(&a->browse, rows[i]);
                listbox_init(&a->browse_box);
                browse_refresh(a);
                a->need_redraw = 1;
                browse_step_towards_goal(a);   /* keep going */
            }
            return;
        }

        /* Not there. Say so rather than silently sitting at the top. */
        {
            char msg[128];

            snprintf(msg, sizeof msg, "no directory called %s here", comp);
            app_status(a, msg);
        }
        a->browse_goto[0] = '\0';
    }
}

void app_browse_start(app *a, adc_sid sid, const char *nick)
{
    char msg[96];

    if (a->hub->state != HUB_NORMAL) {
        app_status(a, "not logged in yet");
        return;
    }

    /* The root, one level deep. A share with 200,000 files produces a list
       this machine cannot hold; asking for the top level and then one
       directory at a time is what makes browsing such a share possible at
       all. A peer that will not do it gets asked for the whole thing
       instead, once, and is remembered. */
    /* Under --peer-trace, say which kind of list was asked for. The wire
       says it too, but only if the request reaches the wire: an inbound
       connection that forgot the base asked for the whole share while this
       line said "partial", which is how that bug was found. */
    if (peer_trace_enabled())
        printf("browse: asking %s for %s\n",
               (nick && nick[0]) ? nick : "?",
               a->browse_no_partial ? "the whole list" : "/ (partial)");

    /* Ourselves: no hub, no peer, no waiting. */
    if (sid == a->hub->sid) {
        a->browse_sid = sid;
        a->browse_ready = 0;
        a->browse_fetching = -1;
        a->browse.err[0] = '\0';
        a->door = DOOR_BROWSE;

        if (browse_self(a, "/"))
            app_status(a, "your own share");
        a->need_redraw = 1;
        return;
    }

    if (!xq_add_list(&a->xq, sid, nick, FILELIST_PEER_BZ2,
                     a->browse_no_partial ? NULL : "/")) {
        app_status(a, "the transfer queue is full");
        return;
    }

    a->browse_sid = sid;
    a->browse_ready = 0;
    a->browse_fetching = -1;
    a->browse.err[0] = '\0';
    a->door = DOOR_BROWSE;

    snprintf(msg, sizeof msg, "asking %s for a file list...",
             (nick && nick[0]) ? nick : "that node");
    app_status(a, msg);
    a->need_redraw = 1;
}

/* Queue the highlighted file. A directory cannot be queued as a unit:
   ADC downloads are per file and addressed by hash, so "get this folder"
   would mean queueing every file inside it -- which is what SPACE on each
   of them does, visibly. */
static void browse_queue_selected(app *a)
{
    search_result r;
    long e;
    char msg[96];

    if (!a->browse_ready || a->browse_box.sel < 0 ||
        a->browse_box.sel >= (int)g_browse_nrow)
        return;

    e = g_browse_rows[a->browse_box.sel];

    if (a->browse.ents[e].is_dir) {
        app_status(a, "that is a directory -- ENTER opens it");
        return;
    }

    memset(&r, 0, sizeof r);
    snprintf(r.name, sizeof r.name, "%s", browse_name(&a->browse, e));
    if (browse_tth(&a->browse, e, r.tth, sizeof r.tth) != 0) {
        app_status(a, "that entry has no hash, so it cannot be fetched");
        return;
    }
    r.size = a->browse.ents[e].size;

    /* The only source we know is whoever's list this is. More may turn up
       later if the file is searched for. */
    r.nsource = 1;
    r.sources[0].sid = a->browse_sid;
    r.sources[0].slots = 1;

    if (xq_add(&a->xq, &r, DOWNLOAD_DIR)) {
        snprintf(msg, sizeof msg, "queued %s", r.name);
        app_status(a, msg);
        xq_save(&a->xq, XQ_QUEUE_FILE);
    } else if (r.size > (tth_u64)DOSYPP_MAX_FILE) {
        /* Said plainly, because the reason is not the user's fault and
           not fixable by trying again: DOS cannot seek past 2GB. */
        app_status(a, "that file is larger than 2GB, which this machine "
                      "cannot address");
        snd_error();
    } else {
        app_status(a, "already queued, or the queue is full");
    }
    a->need_redraw = 1;
}

static void browse_key(app *a, int key)
{
    int h = PANEL_Y + PANEL_H - 6;

    if (!a->browse_ready)
        return;

    listbox_set_count(&a->browse_box, (int)g_browse_nrow);

    if (key == KEY_ENTER || key == '\r' || key == '\n') {
        if (a->browse_box.sel >= 0 &&
            a->browse_box.sel < (int)g_browse_nrow) {
            long e = g_browse_rows[a->browse_box.sel];

            if (browse_needs_fetch(&a->browse, e)) {
                /* A directory from a partial list: it has to be asked for
                   before it can be opened. The door shows it as pending
                   and the reply files itself under this entry. */
                browse_fetch_dir(a, e);
            } else if (a->browse.ents[e].is_dir) {
                browse_enter(&a->browse, e);
                listbox_init(&a->browse_box);
                browse_refresh(a);
                a->need_redraw = 1;
            } else {
                browse_queue_selected(a);
            }
        }
        return;
    }

    if (key == KEY_BACKSPACE || key == KEY_LEFT) {
        browse_up(&a->browse);
        listbox_init(&a->browse_box);
        browse_refresh(a);
        a->need_redraw = 1;
        return;
    }

    if (key == ' ') {
        browse_queue_selected(a);
        return;
    }

    if (listbox_key(&a->browse_box, key, h))
        a->need_redraw = 1;
}

/* ------------------------------------------------------- transfers */

static void browse_arrived(app *a, const char *local, const char *base);

static void xfer_complete(void *user, peerconn *p, const char *local)
{
    app *a = (app *)user;
    char line[SB_LINE_MAX];
    const char *real;

    /* A file list is not a download anybody wants announced as a file: it
       is a door opening. */
    if (p->want == PEER_WANT_LIST) {
        browse_arrived(a, local, p->list_base);
        return;
    }

    /* Leaf hashes: build the plan and let the connection carry on with the
       first segment. Nothing is announced -- from the user's point of view
       the download simply started. */
    if (p->want == PEER_WANT_TTHL) {
        xq_request *r = xq_by_token(&a->xq, p->token);
        long nseg = r ? xq_plan_load(&a->xq, r, local) : 0;

        remove(local);

        if (r && nseg > 0) {
            char msg[96];

            snprintf(msg, sizeof msg, "%s: %ld parts", r->name, nseg);
            app_status(a, msg);

            /* Straight on with the first segment on this connection. */
            p->want = PEER_WANT_NOTHING;
            p->state = PEER_IDLE;
            if (!xq_next_segment(&a->xq, r, p)) {
                /* Everything was already on disk: the resume case where a
                   previous run got all of it but never finished. */
                xq_segment_done(&a->xq, r, 0);
                r->status = XQ_DONE;
            }
        } else if (r) {
            /* No usable leaves. Fall back to fetching the whole file,
               which is what every version before this one did. */
            r->tthl_refused = 1;
            r->status = XQ_WAITING;
            p->state = PEER_DONE;
        }

        a->need_redraw = 1;
        return;
    }

    /* Show the real name, not the 8.3 one the filesystem has. */
    real = a->names ? namemap_long(a->names, local) : NULL;
    if (!real)
        real = p->name;

    if (p->uploading)
        snprintf(line, sizeof line, "%c%c sent %s", CH_ARROW_RIGHT,
                 CH_ARROW_RIGHT, real);
    else
        snprintf(line, sizeof line, "%c%c got %s (hash verified)",
                 CH_ARROW_LEFT, CH_ARROW_LEFT, real);

    sb_add(&a->chat, SCREEN_W - 2, SB_ACTION, NULL, 0, line);
    snd_connect();

    /* Under --peer-trace, on stdout as well as on screen. The wire shows a
       transfer starting; nothing showed one finishing, which a headless rig
       watching a serial port has no other way to know. The v86 gate killed
       a working upload mid-flight for want of this line. */
    if (peer_trace_enabled()) {
        if (p->uploading)
            printf("upload done: %s %s\n",
                   p->sending_type[0] ? p->sending_type : "file", real);
        else
            printf("download done: %s\n", real);
        fflush(stdout);
    }

    /* The queue file is rewritten on every change rather than at exit:
       what interrupts a download is rarely something that gets to run
       cleanup code. */
    xq_save(&a->xq, XQ_QUEUE_FILE);

    a->need_redraw = 1;
}

static void xfer_failed(void *user, peerconn *p)
{
    app *a = (app *)user;
    char line[SB_LINE_MAX];

    /* A segment that failed goes back in the pool rather than taking the
       download with it: one bad source costs one part. */
    if (p->want == PEER_WANT_SEGMENT) {
        xq_request *r = xq_by_token(&a->xq, p->token);

        if (r)
            xq_segment_failed(&a->xq, r, p->seg_index);
    }

    if (p->want == PEER_WANT_LIST) {
        /* A peer that will not do partial lists: ask it for the whole
           share instead, once, and remember so the rest of the session
           does not keep trying. Plenty of clients predate partial lists
           and some hub-side proxies refuse them. */
        if (peer_list_refused(p) && !a->browse_no_partial) {
            adc_user *u = adc_userlist_find(&a->hub->users, a->browse_sid);
            const char *nick = (u && u->nick[0]) ? u->nick : "that node";
            char msg[128];

            a->browse_no_partial = 1;
            a->browse_fetching = -1;

            snprintf(msg, sizeof msg,
                     "%s does not do partial lists -- asking for the whole "
                     "share", nick);
            sb_add(&a->chat, SCREEN_W - 2, SB_SYSTEM, NULL, 0, msg);

            if (xq_add_list(&a->xq, a->browse_sid, nick, FILELIST_PEER_BZ2,
                            NULL)) {
                app_status(a, "asking for the whole file list...");
                a->need_redraw = 1;
                return;
            }
        }

        /* A failed file list belongs on the browse door, not just in the
           chat: somebody who presses 8 afterwards should see why it is
           empty rather than "nobody's files are open". */
        snprintf(a->browse.err, sizeof a->browse.err, "%s", peer_error(p));
        a->browse_ready = 0;
        a->browse_fetching = -1;
    }

    snprintf(line, sizeof line, "transfer of %s failed: %s",
             p->name, peer_error(p));
    sb_add(&a->chat, SCREEN_W - 2, SB_ERROR, NULL, 0, line);
    snd_error();

    /* A failed download usually leaves a partial file, and that partial is
       the whole point of keeping a queue file: the next attempt, in this
       session or the next, starts where this one stopped. */
    xq_save(&a->xq, XQ_QUEUE_FILE);

    a->need_redraw = 1;
}

static void xfer_progress(void *user, peerconn *p)
{
    app *a = (app *)user;

    (void)p;
    a->need_redraw = 1;
}

/* Upload slots. The policy lives here because the number comes from the
   config and the count from the queue; peerconn only needs a yes or no.
 *
 * A slot is counted as busy from the moment a connection is serving, so a
 * peer that connects and sits idle does not hold one. */
static int xfer_may_upload(void *user, peerconn *p)
{
    app *a = (app *)user;
    unsigned int limit = a->conf ? a->conf->slots : 1;
    int busy = xq_uploads_sending(&a->xq);

    (void)p;

    if (limit == 0)
        return 0;                   /* sharing without serving: their call */

    return busy < (int)limit;
}

/* A peer dialled us quoting a token. The queue decides whether it is one of
   ours and, if so, what this connection is now for. */
static int xfer_on_token(void *user, peerconn *p, const char *token)
{
    app *a = (app *)user;

    return xq_claim_token(&a->xq, p, token);
}

/* One segment verified. Hand the same connection the next one: reusing it
   is what makes this cheaper than a connection per part, and it is what
   DC++ does. */
static void xfer_segment(void *user, peerconn *p, long index)
{
    app *a = (app *)user;
    xq_request *r = xq_by_token(&a->xq, p->token);

    if (!r)
        return;

    if (xq_segment_done(&a->xq, r, index)) {
        /* The last part. The file is verified by construction: every
           segment matched its leaf and the leaves matched the root, so
           there is nothing left to check and no second pass over the
           file. */
        char line[SB_LINE_MAX];
        const char *real = a->names ? namemap_long(a->names, r->local)
                                    : NULL;

        r->status = XQ_DONE;
        a->xq.completed++;

        snprintf(line, sizeof line, "%c%c got %s (every part hash verified)",
                 CH_ARROW_LEFT, CH_ARROW_LEFT, real ? real : r->name);
        sb_add(&a->chat, SCREEN_W - 2, SB_ACTION, NULL, 0, line);
        snd_connect();
        xq_save(&a->xq, XQ_QUEUE_FILE);

        p->state = PEER_DONE;
        a->need_redraw = 1;
        return;
    }

    if (!xq_next_segment(&a->xq, r, p)) {
        /* Nothing left that this connection can take -- the remaining
           parts are with other sources. Let it close. */
        p->state = PEER_DONE;
    }
    a->need_redraw = 1;
}

static void xfer_callbacks(app *a, peer_callbacks *cb)
{
    memset(cb, 0, sizeof *cb);
    cb->user = a;
    cb->on_complete = xfer_complete;
    cb->on_failed = xfer_failed;
    cb->on_progress = xfer_progress;
    cb->may_upload = xfer_may_upload;
    cb->on_token = xfer_on_token;
    cb->on_segment = xfer_segment;
}

void app_on_ctm(void *user, const char *host, int port, const char *token)
{
    app *a = (app *)user;
    peer_callbacks cb;
    transport t;
    char msg[96];

    if (!a)
        return;

    xfer_callbacks(a, &cb);

    /* Brief timeout: a peer that advertised an address and does not answer
       is usually behind a NAT it does not know about, and there are other
       sources to try. */
    if (tp_plain_connect(&t, host, port, 10) != 0) {
        snprintf(msg, sizeof msg, "could not reach %s:%d", host, port);
        app_status(a, msg);
        return;
    }

    /* We dialled, in answer to their CTM, so this connection carries the
       token they gave us. */
    if (xq_adopt(&a->xq, &t, token, 1, &a->hub->id, &a->share, &cb,
                 a->now) != 0) {
        tp_close(&t);
        app_status(a, "no free transfer slot");
    }
    a->need_redraw = 1;
}

void app_queue_selected(app *a)
{
    search_state *ss = &a->hub->search;
    int queued = 0;
    int i;
    char msg[96];

    /* Tagged files first; if nothing is tagged, the highlighted row. That
       is how a BBS file-scan behaved and it saves a keystroke. */
    /* Queued requests are written out below, once, rather than per file. */
    if (search_tagged_count(ss) > 0) {
        for (i = 0; i < ss->count; i++)
            if (ss->results[i].tagged &&
                xq_add(&a->xq, &ss->results[i], DOWNLOAD_DIR))
                queued++;
        search_tag_all(ss, 0);
    } else if (a->files.sel >= 0 && a->files.sel < ss->count) {
        if (xq_add(&a->xq, &ss->results[a->files.sel], DOWNLOAD_DIR))
            queued++;
    }

    if (queued == 0) {
        /* Name the reason when it is the one nobody would guess. A 2GB
           file is not a mistake the user made and not one retrying
           fixes. */
        int oversize = 0;

        for (i = 0; i < ss->count; i++)
            if ((ss->results[i].tagged || i == a->files.sel) &&
                ss->results[i].size > (tth_u64)DOSYPP_MAX_FILE)
                oversize = 1;

        if (oversize)
            app_status(a, "larger than 2GB, which this machine cannot "
                          "address");
        else
            app_status(a, "nothing queued (already queued, or a directory)");
        snd_keypress_bad();
        return;
    }

    snprintf(msg, sizeof msg, "%d file%s queued", queued,
             queued == 1 ? "" : "s");
    app_status(a, msg);

    /* So a power cut between now and the transfer does not lose the queue.
       Written once for the whole batch rather than per file. */
    xq_save(&a->xq, XQ_QUEUE_FILE);

    a->door = DOOR_XFER;
    a->need_redraw = 1;
}

/* A download restored from DOSYPP.QUE knows its hash and how much of it is
   already on disk, but not who has it: a SID means nothing in a new
   session. So the hash is searched for, one request at a time.
 *
 * Kept out of the user's way rather than abandoned: a search replaces the
 * results table, so this waits until the file areas door is not the one
 * being looked at and the user's own last search is a minute cold. A user
 * who searches constantly delays their resumes, which is the right way
 * round -- the alternative is wiping their results while they read them. */
static void app_resume_tick(app *a)
{
    search_query q;
    xq_request *r = NULL;
    int i;

    if (a->hub->state != HUB_NORMAL)
        return;
    if (a->now < a->resume_next_at)
        return;
    if (a->door == DOOR_FILES)
        return;
    if (a->last_user_search && a->now < a->last_user_search + 60)
        return;

    for (i = 0; i < a->xq.nreq; i++)
        if (!a->xq.reqs[i].wants_list &&
            a->xq.reqs[i].status == XQ_WAITING &&
            a->xq.reqs[i].nsource == 0) {
            r = &a->xq.reqs[i];
            break;
        }

    if (!r)
        return;

    /* Spaced out: a hub takes a dim view of a client that searches in a
       tight loop, and some will disconnect for it. */
    a->resume_next_at = a->now + 25;

    memset(&q, 0, sizeof q);
    q.group = SEARCH_TTH;
    snprintf(q.tth, sizeof q.tth, "%s", r->tth);

    if (hub_search(a->hub, &q, a->now) == 0) {
        char line[SB_LINE_MAX];

        snprintf(line, sizeof line, "looking for a source for %s", r->name);
        sb_add(&a->chat, SCREEN_W - 2, SB_SYSTEM, NULL, 0, line);
        a->need_redraw = 1;
    }
}

void app_xfer_tick(app *a)
{
    peer_callbacks cb;
    unsigned long now = a->now;

    xfer_callbacks(a, &cb);

    /* Accept anyone dialling us. Only possible when active, which is why
       being active is worth configuring. */
    if (a->listening) {
        transport t;
        int guard;

        for (guard = 0; guard < 4; guard++) {
            int rc = tp_accept(&a->listener, &t);

            if (rc <= 0)
                break;

            /* The token is not known until they send CINF, so the queue
               decides then whether this is a download we asked for or
               someone wanting something from us. */
            /* They dialled us, so the token is not known until their
               CINF arrives: the queue decides then (see on_token). */
            if (xq_adopt(&a->xq, &t, NULL, 0, &a->hub->id, &a->share, &cb,
                         now) != 0)
                tp_close(&t);
        }
    }

    /* Ask for a connection for whatever is waiting. A segmented download
       asks each of its sources, which is what makes it parallel; a plain
       one asks the source its cursor is on. One connection per peer either
       way -- see busy_with(). */
    {
        adc_sid src = ADC_SID_NONE;
        xq_request *r = xq_next_to_ask(&a->xq, &src);

        if (r && src != ADC_SID_NONE && a->hub->state == HUB_NORMAL) {
            char token[XQ_TOKEN_MAX];

            xq_mint_token(&a->xq, token, sizeof token);

            if (xq_note_asked(&a->xq, r, src, token, now) != 0) {
                /* No room in the connection table: as parallel as we get. */
            } else if (hub_request_connection(a->hub, src, token) == 0) {
                if (r->status != XQ_ACTIVE) {
                    r->status = XQ_ASKING;
                    r->asked_at = now;
                }
            } else {
                /* Usually a passive-to-passive pair. Move on to another
                   source rather than retrying something that cannot work. */
                char line[SB_LINE_MAX];

                xq_forget(&a->xq, token);
                snprintf(r->err, sizeof r->err, "%s", hub_error(a->hub));

                /* Also into the message base: the transfer door truncates
                   this to a column, and the reason a download will never
                   start is worth reading in full. */
                snprintf(line, sizeof line, "cannot get %s: %s",
                         r->name, hub_error(a->hub));
                sb_add(&a->chat, SCREEN_W - 2, SB_ERROR, NULL, 0, line);

                r->try_source++;
                if (r->try_source >= r->nsource && r->status != XQ_ACTIVE)
                    r->status = XQ_FAILED;
            }
            a->need_redraw = 1;
        }
    }

    /* Attempts nobody answered. Thirty seconds: a hub round trip plus a
       peer deciding to dial us, which on a modem is slow but not minutes. */
    if (xq_expire_asks(&a->xq, now, 30))
        a->need_redraw = 1;

    /* Keep the hub's idea of our free slots current. Other clients read
       SL before asking, so a stale number either turns people away or
       invites requests we will refuse. Only on a change: an INF per
       transferred byte would be flooding. */
    {
        int free_now = xq_free_upload_slots(&a->xq,
                                            a->conf ? (int)a->conf->slots : 1);

        if (free_now != a->slots_advertised &&
            a->hub->state == HUB_NORMAL) {
            if (hub_update_slots(a->hub, (unsigned int)free_now) == 0)
                a->slots_advertised = free_now;
        }
    }

    /* Hand idle connections their next part. A segmented download keeps a
       connection and feeds it segment after segment, so this is where the
       work actually gets distributed. */
    if (xq_assign_segments(&a->xq))
        a->need_redraw = 1;

    /* Look for a source for something restored from the queue file. */
    app_resume_tick(a);

    if (xq_poll(&a->xq, now))
        a->need_redraw = 1;
}

/* How far along a connection's download is. For a segmented one that is
   the whole file's progress, not this segment's: a gauge that runs 0-100%
   per 64KB part and starts again tells the user nothing about the hour
   they are waiting. */
static int xfer_percent(app *a, peerconn *p)
{
    xq_request *r;
    xq_plan *pl;

    if (p->want != PEER_WANT_SEGMENT && p->want != PEER_WANT_NOTHING)
        return peer_percent(p);

    r = xq_by_token(&a->xq, p->token);
    pl = r ? xq_plan_of(&a->xq, r) : NULL;

    if (!pl || r->size == 0)
        return peer_percent(p);

    /* Verified parts, plus whatever the current one has moved. */
    {
        tth_u64 done = xq_plan_done_bytes(&a->xq, r);

        if (p->want == PEER_WANT_SEGMENT && p->moved > p->offset)
            done += p->moved - p->offset;

        if (done > r->size)
            done = r->size;
        return (int)((done * 100) / r->size);
    }
}

/* A file list that arrived is not a transfer anybody queued, and leaving
   it on the door as "DONE" is clutter that outlives its usefulness by a
   whole session. A failed one stays: that is a thing the user asked for
   and did not get. */
static int xfer_row_hidden(const xq_request *r)
{
    return r->wants_list && r->status == XQ_DONE;
}

static void xfer_row(void *user, int index, char *out, int outsz)
{
    app *a = (app *)user;
    char size[12];
    int i, seen = 0;

    /* Live connections first, then the queue: what is moving matters more
       than what is waiting. */
    for (i = 0; i < XQ_MAX_DOWNLOADS; i++) {
        if (!a->xq.down_used[i])
            continue;
        if (seen++ == index) {
            peerconn *p = &a->xq.down[i];
            xq_request *r = xq_by_token(&a->xq, p->token);
            xq_plan *pl = r ? xq_plan_of(&a->xq, r) : NULL;

            search_fmt_size(peer_rate(p, a->now), size, sizeof size);

            /* A segmented download says which part is moving: with two
               sources there are two of these rows for one file, and
               without the part number they look like a bug. */
            if (pl && p->want == PEER_WANT_SEGMENT)
                snprintf(out, (size_t)outsz,
                         " %c %-22s p%ld/%ld %3d%% %8s/s  %s",
                         CH_ARROW_DOWN, p->name, p->seg_index + 1,
                         pl->nseg, xfer_percent(a, p), size,
                         peer_state_name(p->state));
            else
                snprintf(out, (size_t)outsz, " %c %-30s %3d%% %8s/s  %s",
                         CH_ARROW_DOWN, p->name, xfer_percent(a, p), size,
                         peer_state_name(p->state));
            return;
        }
    }
    for (i = 0; i < XQ_MAX_UPLOADS; i++) {
        if (!a->xq.up_used[i])
            continue;
        if (seen++ == index) {
            peerconn *p = &a->xq.up[i];
            search_fmt_size(peer_rate(p, a->now), size, sizeof size);
            snprintf(out, (size_t)outsz, " %c %-30s %3d%% %8s/s  %s",
                     CH_ARROW_UP, p->name, peer_percent(p), size,
                     peer_state_name(p->state));
            return;
        }
    }
    for (i = 0; i < a->xq.nreq; i++) {
        xq_request *r = &a->xq.reqs[i];

        if (r->status == XQ_ACTIVE)
            continue;              /* already shown above */
        if (xfer_row_hidden(r))
            continue;
        if (seen++ == index) {
            /* Must match the width draw_xfer_door hands listbox_draw, or
               the reason is cut mid-word at the border -- which is how
               "we are passive a" ended up on screen. */
            const int row_w = SCREEN_W - 2;
            int used;

            search_fmt_size(r->size, size, sizeof size);
            used = snprintf(out, (size_t)outsz, "   %-30s %12s  %-11s",
                            r->name, size, xq_status_name(r->status));

            if (r->err[0] && used > 0 && used + 6 < row_w) {
                int room = row_w - used - 3;    /* after " - " */
                char why[SCREEN_W];

                snprintf(why, sizeof why, "%s", r->err);
                if ((int)strlen(why) > room) {
                    /* End on "..", not on half a word. */
                    why[room > 2 ? room - 2 : 0] = '\0';
                    strncat(why, "..", sizeof why - strlen(why) - 1);
                }
                snprintf(out + used, (size_t)(outsz - used), " - %s", why);
            }
            return;
        }
    }

    out[0] = '\0';
}

static int xfer_rows(const app *a)
{
    int n = xq_downloads_active(&a->xq) + xq_uploads_active(&a->xq);
    int i;

    /* The same rule as xfer_row, or the list has rows the row function
       will not fill in. */
    for (i = 0; i < a->xq.nreq; i++)
        if (a->xq.reqs[i].status != XQ_ACTIVE
            && !xfer_row_hidden(&a->xq.reqs[i]))
            n++;
    return n;
}

static void draw_xfer_door(app *a)
{
    const theme *th = a->th;
    int list_y = PANEL_Y + 3;
    int list_h = PANEL_Y + PANEL_H - 2 - list_y;
    char buf[SCREEN_W];
    char down[12], up[12];
    int rows;

    screen_box(&a->scr, 0, PANEL_Y, SCREEN_W, PANEL_H, BOX_SINGLE,
               th->border);
    screen_box_title(&a->scr, 0, PANEL_Y, SCREEN_W, "TRANSFERS",
                     th->border, th->title);

    snprintf(buf, sizeof buf, "   %-30s %12s  %s",
             "FILE", "SIZE / RATE", "STATE");
    screen_puts_pad(&a->scr, 1, PANEL_Y + 1, buf, SCREEN_W - 2,
                    th->text_hi);

    /* The mode belongs up here with the headings: it is a property of this
       node, it is short, and the footer had no room for it. Drawn after
       the headings, because the headings pad the whole row. */
    {
        /* One word: the headings end at about column 59 and the full
           explanation ("cannot be dialled") did not fit, so it was
           silently suppressed -- which is the same class of bug as the
           overflow this is fixing. The explanation lives in the info
           door, where there is room for it. */
        const char *mode = a->hub->active ? "ACTIVE" : "PASSIVE";
        int mx = SCREEN_W - 2 - (int)strlen(mode);

        if (mx > 60)
            screen_puts(&a->scr, mx, PANEL_Y + 1, mode, th->text_dim);
    }
    screen_hline(&a->scr, 1, PANEL_Y + 2, SCREEN_W - 2, 0xC4, th->border);

    rows = xfer_rows(a);
    if (rows == 0) {
        screen_puts_center(&a->scr, 1, list_y + list_h / 2 - 1,
                           SCREEN_W - 2,
                           "NOTHING IN THE QUEUE", th->text_dim);
        screen_puts_center(&a->scr, 1, list_y + list_h / 2 + 1,
                           SCREEN_W - 2,
                           "Tag files in the FILE AREAS door and press ENTER",
                           th->text_dim);
    } else {
        listbox_set_count(&a->xfers, rows);
        a->xfers.focus = 1;
        listbox_draw(&a->xfers, &a->scr, th, 1, list_y, SCREEN_W - 2,
                     list_h, xfer_row, a);
    }

    /* The gauge for whatever is selected, so one transfer can be watched
       properly rather than squinting at a column. */
    {
        int sel = a->xfers.sel;
        int i, seen = 0;
        peerconn *show = NULL;

        for (i = 0; i < XQ_MAX_DOWNLOADS && !show; i++)
            if (a->xq.down_used[i] && seen++ == sel)
                show = &a->xq.down[i];
        for (i = 0; i < XQ_MAX_UPLOADS && !show; i++)
            if (a->xq.up_used[i] && seen++ == sel)
                show = &a->xq.up[i];

        if (show)
            gauge_draw(&a->scr, th, 2, PANEL_Y + PANEL_H - 2, 40,
                       peer_percent(show));
    }

    /* Counters only, and padded to exactly the room there is: this line
       used to run past the panel and overwrite its right border, which
       was reported as "0 done, 0 faile" with no border after it. */
    search_fmt_size(a->xq.bytes_down, down, sizeof down);
    search_fmt_size(a->xq.bytes_up, up, sizeof up);
    snprintf(buf, sizeof buf, "%s in / %s out  %u done %u failed",
             down, up, a->xq.completed, a->xq.failed);
    screen_puts_pad(&a->scr, 44, PANEL_Y + PANEL_H - 2, buf,
                    SCREEN_W - 2 - 44, th->text_dim);

    screen_cursor_hide(&a->scr);
}

/* ------------------------------------------------------- the commands
 *
 * The command set is described once, as data, for the same reason the
 * settings are: /help, the info door and the dispatcher cannot then
 * disagree about what exists. A client whose only documentation is its
 * source is a client with undiscoverable features.
 */
typedef struct {
    const char *name;       /* without the slash */
    const char *args;       /* "" when it takes none */
    const char *help;
} command_doc;

static const command_doc COMMANDS[] = {
    { "help",    "",            "this list" },
    { "me",      "<action>",    "speak in the third person" },
    { "pm",      "<nick> <text>", "a private message" },
    { "who",     "",            "the node list" },
    { "whois",   "<nick>",      "what the hub knows about somebody" },
    { "browse",  "<nick>",      "open their share" },
    { "search",  "<words>",     "search the hub" },
    { "xfers",   "",            "the transfer door" },
    { "motd",    "",            "the hub's welcome screen again" },
    { "stats",   "",            "this session, in numbers" },
    { "desc",    "<text>",      "change your description, now" },
    { "slots",   "<n>",         "how many uploads at once" },
    { "rescan",  "",            "look at the share again" },
    { "hash",    "",            "start or pause hashing" },
    { "connect", "<host[:port]>", "move to another hub" },
    { "fav",     "",            "keep this hub in the favourites" },
    { "save",    "",            "write DOSYPP.INI now" },
    { "theme",   "<name>",      "renegade, pcboard or amiga" },
    { "rows",    "<25|50>",     "screen height, on the next run" },
    { "music",   "",            "the tune, on or off" },
    { "sound",   "",            "the PC speaker, on or off" },
    { "clear",   "",            "empty the message pane" },
    { "version", "",            "what this is" },
    { "raw",     "<line>",      "send an ADC line yourself" },
    { "quit",    "",            "sign off (also /q)" }
};

#define COMMAND_COUNT ((int)(sizeof COMMANDS / sizeof COMMANDS[0]))

/* The list.
 *
 * Names only, wrapped to the pane: twenty-five commands one to a line
 * would scroll an 18-row chat pane past its own beginning, and the half
 * that survived would be the half nobody needed. `/help <name>` explains
 * one, which is the other half of the same idea -- discover from the list,
 * then ask.
 */
static void command_help(app *a)
{
    char line[SB_LINE_MAX];
    size_t used = 0;
    int i;

    app_say_local(a, SB_SYSTEM,
                  "COMMANDS -- /help <name> explains one:");

    line[0] = '\0';
    for (i = 0; i < COMMAND_COUNT; i++) {
        char one[24];
        size_t n;

        snprintf(one, sizeof one, "/%s ", COMMANDS[i].name);
        n = strlen(one);

        /* Four columns of ten, which fits 80 wide with the pane borders
           and reads as a table rather than a paragraph. */
        if (n < 10) {
            memset(one + n, ' ', 10 - n);
            one[10] = '\0';
            n = 10;
        }

        if (used + n >= 72) {
            app_say_local(a, SB_SYSTEM, line);
            line[0] = '\0';
            used = 0;
        }

        snprintf(line + used, sizeof line - used, "%s", one);
        used = strlen(line);
    }

    if (used > 0)
        app_say_local(a, SB_SYSTEM, line);
}

/* One command, explained. Takes the name with or without its slash,
   because both are what people type. */
static void command_help_one(app *a, const char *name)
{
    char line[SB_LINE_MAX];
    int i;

    while (*name == '/' || *name == ' ')
        name++;

    for (i = 0; i < COMMAND_COUNT; i++) {
        if (strcmp(COMMANDS[i].name, name) != 0)
            continue;

        snprintf(line, sizeof line, "/%s %s -- %s", COMMANDS[i].name,
                 COMMANDS[i].args, COMMANDS[i].help);
        app_say_local(a, SB_SYSTEM, line);
        return;
    }

    snprintf(line, sizeof line, "there is no /%s. /help lists them.", name);
    app_say_local(a, SB_SYSTEM, line);
    snd_keypress_bad();
}

/* What the hub knows about somebody, which is what /whois is for: the
   fields are already in the user list, and digging them out of the who
   door one row at a time is not the same thing. */
static void command_whois(app *a, const char *nick)
{
    adc_user *u;
    char line[SB_LINE_MAX];
    char size[24];
    char sid_text[ADC_SID_CHARS + 1];

    if (!a->hub) {
        app_status(a, "not connected");
        return;
    }

    u = adc_userlist_find_nick(&a->hub->users, nick);
    if (!u) {
        snprintf(line, sizeof line, "nobody here is called %s", nick);
        app_say_local(a, SB_SYSTEM, line);
        snd_keypress_bad();
        return;
    }

    adc_sid_encode(u->sid, sid_text);
    search_fmt_size((unsigned long)u->share, size, sizeof size);

    snprintf(line, sizeof line, "%s -- node %s, %s, %lu files, %s",
             u->nick, sid_text, size, u->files,
             u->passive ? "passive" : "active");
    app_say_local(a, SB_SYSTEM, line);

    if (u->desc[0]) {
        char desc[64];

        for_display_line(u->desc, desc, sizeof desc);
        snprintf(line, sizeof line, "  \"%s\"", desc);
        app_say_local(a, SB_SYSTEM, line);
    }

    snprintf(line, sizeof line, "  client %s%s%s  slots %u  hubs %u/%u/%u",
             u->tag[0] ? u->tag : "unknown",
             u->ip[0] ? "  at " : "", u->ip,
             u->slots, u->hubs_normal, u->hubs_reg, u->hubs_op);
    app_say_local(a, SB_SYSTEM, line);

    if (u->ct) {
        snprintf(line, sizeof line, "  %s%s%s%s",
                 (u->ct & ADC_CT_OP) ? "operator " : "",
                 (u->ct & ADC_CT_REGISTERED) ? "registered " : "",
                 (u->ct & ADC_CT_BOT) ? "bot " : "",
                 (u->ct & ADC_CT_HUB) ? "hub " : "");
        app_say_local(a, SB_SYSTEM, line);
    }

    snprintf(line, sizeof line, "  CID %s", u->cid);
    app_say_local(a, SB_SYSTEM, line);
}

/* The session, in numbers. Everything here is already on some door; the
   point is having it in one place without changing door. */
static void command_stats(app *a)
{
    char line[SB_LINE_MAX];
    char down[24], up[24], shared[24];
    unsigned long secs = (a->connected_at && a->now > a->connected_at)
                         ? a->now - a->connected_at : 0;

    search_fmt_size(a->xq.bytes_down, down, sizeof down);
    search_fmt_size(a->xq.bytes_up, up, sizeof up);
    search_fmt_size((unsigned long)a->share.total_size, shared,
                    sizeof shared);

    snprintf(line, sizeof line, "online %lu:%02lu:%02lu  %s in  %s out  "
             "%u done  %u failed", secs / 3600UL, (secs / 60UL) % 60UL,
             secs % 60UL, down, up, a->xq.completed, a->xq.failed);
    app_say_local(a, SB_SYSTEM, line);

    snprintf(line, sizeof line, "sharing %s in %u files (%u hashed)  "
             "%s  %s", shared, a->share.count, a->share.hashed_count,
             a->hub && a->hub->active ? "active" : "passive",
             mus_playing() ? "music on" : "music off");
    app_say_local(a, SB_SYSTEM, line);
}

/* --------------------------------------------------------------- input */

static void do_command(app *a, char *line)
{
    if (strcmp(line, "/quit") == 0 || strcmp(line, "/q") == 0) {
        a->running = 0;
        return;
    }
    if (strcmp(line, "/help") == 0 || strcmp(line, "/?") == 0 ||
        strcmp(line, "/h") == 0) {
        command_help(a);
        return;
    }
    if (strncmp(line, "/help ", 6) == 0) {
        command_help_one(a, line + 6);
        return;
    }
    if (strcmp(line, "/version") == 0) {
        char buf[SB_LINE_MAX];

        snprintf(buf, sizeof buf,
                 "DOSy++ %s -- an ADC client for MS-DOS. %d columns, "
                 "%d rows, %s.",
                 DOSYPP_VERSION, SCREEN_W, SCREEN_ROWS,
                 mus_playing() ? mus_device() : "no music");
        app_say_local(a, SB_SYSTEM, buf);
        return;
    }
    if (strcmp(line, "/stats") == 0) {
        command_stats(a);
        return;
    }
    if (strncmp(line, "/whois ", 7) == 0) {
        command_whois(a, line + 7);
        return;
    }
    if (strcmp(line, "/xfers") == 0 || strcmp(line, "/queue") == 0) {
        a->door = DOOR_XFER;
        a->need_redraw = 1;
        return;
    }
    if (strncmp(line, "/browse ", 8) == 0) {
        adc_user *u = a->hub ? adc_userlist_find_nick(&a->hub->users,
                                                      line + 8) : NULL;

        if (!u) {
            app_status(a, "nobody here by that name");
            snd_keypress_bad();
            return;
        }
        app_browse_start(a, u->sid, u->nick);
        return;
    }
    if (strncmp(line, "/desc ", 6) == 0 || strcmp(line, "/desc") == 0) {
        const char *text = (line[5] == ' ') ? line + 6 : "";

        if (a->conf)
            snprintf(a->conf->desc, sizeof a->conf->desc, "%s", text);
        if (a->hub && hub_update_desc(a->hub, text) != 0) {
            app_status(a, "not connected, so the hub has not been told");
        } else {
            a->setup_dirty = 1;     /* S in the setup door saves it */
            app_status(a, text[0] ? text : "description cleared");
        }
        return;
    }
    if (strncmp(line, "/slots ", 7) == 0) {
        int n = atoi(line + 7);

        if (n < 1 || n > 99) {
            app_status(a, "slots take a number from 1 to 99");
            snd_keypress_bad();
            return;
        }
        if (a->conf)
            a->conf->slots = (unsigned)n;
        if (a->hub) {
            a->hub->slots = (unsigned)n;
            /* Tell the hub at once: a slot count other clients cannot see
               is not a slot count. */
            hub_update_slots(a->hub,
                             (unsigned)xq_free_upload_slots(&a->xq, n));
        }
        a->setup_dirty = 1;
        {
            char buf[48];

            snprintf(buf, sizeof buf, "%d upload slot%s", n,
                     n == 1 ? "" : "s");
            app_status(a, buf);
        }
        return;
    }
    if (strncmp(line, "/connect ", 9) == 0) {
        /* The connection belongs to the main loop, so this records the
           choice the same way the hubs door does and lets the loop act on
           it. Two owners of one socket is how that goes wrong. */
        char host[CFG_STR_MAX];
        int port = 0;

        if (favs_split(line + 9, host, sizeof host, &port) != 0) {
            app_status(a, "that is not a hub address");
            snd_keypress_bad();
            return;
        }

        snprintf(a->switch_to, sizeof a->switch_to, "%s", host);
        a->switch_port = port ? port : 1511;
        /* No per-hub credentials here: this is "go to this address", and
           the handle and password come from the config. A favourite is how
           you keep credentials for a hub. */
        a->switch_nick[0] = '\0';
        a->switch_pass[0] = '\0';
        snprintf(a->switch_label, sizeof a->switch_label, "%s", host);
        a->want_switch = 1;
        {
            char buf[96];

            snprintf(buf, sizeof buf, "moving to %s:%d", a->switch_to,
                     a->switch_port);
            app_say_local(a, SB_SYSTEM, buf);
        }
        return;
    }
    if (strcmp(line, "/fav") == 0) {
        favs_add_current(a);
        return;
    }
    if (strcmp(line, "/save") == 0) {
        setup_save(a);
        return;
    }
    if (strncmp(line, "/rows ", 6) == 0) {
        int n = atoi(line + 6);

        if (n != 25 && n != 50) {
            app_status(a, "rows are 25 or 50");
            snd_keypress_bad();
            return;
        }
        if (a->conf)
            a->conf->rows = n;
        a->setup_dirty = 1;
        app_status(a, "screen rows change on the next run");
        return;
    }
    if (strcmp(line, "/clear") == 0) {
        sb_init(&a->chat);
        a->need_redraw = 1;
        return;
    }
    if (strcmp(line, "/who") == 0) {
        a->door = DOOR_WHO;
        a->need_redraw = 1;
        return;
    }
    if (strncmp(line, "/search ", 8) == 0) {
        app_search(a, line + 8);
        return;
    }
    if (strcmp(line, "/rescan") == 0) {
        a->door = DOOR_SHARE;
        app_share_start(a);
        return;
    }
    if (strcmp(line, "/hash") == 0) {
        a->door = DOOR_SHARE;
        app_hash_toggle(a);
        return;
    }
    if (strcmp(line, "/motd") == 0) {
        a->door = DOOR_WELCOME;
        a->motd_scroll = 0;
        a->need_redraw = 1;
        return;
    }
    if (strcmp(line, "/sound") == 0) {
        snd_init(!snd_enabled());
        app_status(a, snd_enabled() ? "speaker on" : "speaker off");
        if (snd_enabled())
            snd_message();
        return;
    }
    if (strcmp(line, "/music") == 0) {
        if (mus_playing()) {
            mus_stop();
            if (a->conf)
                a->conf->music = 0;
            app_status(a, "music off");
        } else {
            if (a->conf)
                a->conf->music = 1;
            music_begin(a);
            app_status(a, mus_playing() ? mus_device() : mus_error());
        }
        return;
    }
    if (strcmp(line, "/theme") == 0) {
        char buf[SB_LINE_MAX];
        int k;

        buf[0] = '\0';
        for (k = 0; k < THEME_COUNT; k++) {
            char one[32];

            snprintf(one, sizeof one, "%s%s", k ? ", " : "",
                     theme_get(k)->name);
            strncat(buf, one, sizeof buf - strlen(buf) - 1);
        }
        app_say_local(a, SB_SYSTEM, buf);
        return;
    }
    if (strncmp(line, "/theme ", 7) == 0) {
        a->th = theme_by_name(line + 7);
        screen_invalidate(&a->scr);
        app_status(a, a->th->name);
        return;
    }
    if (strncmp(line, "/raw ", 5) == 0) {
        if (hub_raw(a->hub, line + 5) != 0)
            app_status(a, hub_error(a->hub));
        return;
    }
    if (strncmp(line, "/msg ", 5) == 0) {
        /* The DC++ name for it. Rewritten rather than duplicated. */
        memmove(line + 1, "pm", 2);
        memmove(line + 3, line + 4, strlen(line + 4) + 1);
    }
    if (strncmp(line, "/pm ", 4) == 0) {
        char *nick = line + 4;
        char *text = strchr(nick, ' ');
        adc_user *target;

        if (!text) {
            app_status(a, "usage: /pm <handle> <message>");
            return;
        }
        *text++ = '\0';
        target = adc_userlist_find_nick(&a->hub->users, nick);
        if (!target) {
            char msg[64];
            snprintf(msg, sizeof msg, "no such node: %s", nick);
            app_status(a, msg);
            return;
        }
        if (hub_pm(a->hub, target->sid, text) != 0) {
            app_status(a, hub_error(a->hub));
        } else {
            char echo[SB_LINE_MAX];
            snprintf(echo, sizeof echo, "[NODE MSG to %s] %s",
                     target->nick, text);
            app_say_local(a, SB_PM, echo);
        }
        return;
    }

    if (line[0] == '/' && strncmp(line, "/me ", 4) != 0) {
        app_status(a, "unknown command - see NODE INFO");
        snd_keypress_bad();
        return;
    }

    if (hub_say(a->hub, line) != 0)
        app_status(a, hub_error(a->hub));
}

/* Build the query from the form and send it. */
static void files_start_search(app *a)
{
    search_query q;
    unsigned long kb;

    memset(&q, 0, sizeof q);

    /* The form holds display text; the protocol wants the raw bytes. These
       are ASCII-only inputs, so no conversion is needed -- but they do need
       length-clamping into the query's fields. */
    snprintf(q.terms, sizeof q.terms, "%s", a->f_terms.text);
    snprintf(q.exclude, sizeof q.exclude, "%s", a->f_exclude.text);
    q.group = a->f_group;

    /* A TTH pasted into the terms box is almost certainly meant as an exact
       search: 39 base32 characters and nothing else. Guessing here saves
       the user finding a separate field they did not know existed. */
    if (strlen(q.terms) == BASE32_TIGER_LEN &&
        strchr(q.terms, ' ') == NULL) {
        unsigned char probe[TTH_HASH_LEN + 4];
        if (base32_decode(probe, sizeof probe, q.terms) == TTH_HASH_LEN) {
            snprintf(q.tth, sizeof q.tth, "%s", q.terms);
            q.group = SEARCH_TTH;
        }
    }

    kb = strtoul(a->f_minsize.text, NULL, 10);
    if (kb)
        q.min_size = (tiger_u64)kb * 1024;

    /* Noted so the background resume searches keep out of the way: they
       replace the results table, and wiping what somebody is reading would
       be rude. */
    a->last_user_search = a->now;

    if (hub_search(a->hub, &q, a->now) != 0) {
        app_status(a, hub_error(a->hub));
        snd_keypress_bad();
        return;
    }

    listbox_init(&a->files);
    a->sfocus = SF_RESULTS;
    app_status(a, "scanning...");
    a->need_redraw = 1;
}

/* Returns non-zero if the key was consumed. */
/* Is the focused search field empty?
 *
 * The group selector and the results list have no text, so they count as
 * empty: a digit pressed there is a door and not a character. */
static int files_field_empty(const app *a)
{
    switch (a->sfocus) {
    case SF_TERMS:   return a->f_terms.len == 0;
    case SF_EXCLUDE: return a->f_exclude.len == 0;
    case SF_MINSIZE: return a->f_minsize.len == 0;
    default:         return 1;
    }
}

static int files_key(app *a, int key)
{
    /* ESC is not this door's to keep. It falls through to handle_key's
       ladder -- results, then query, then out of the door -- and if it
       were handled here it would reach input_key instead and only clear a
       field, which is how this door ended up with no way out of it. */
    if (key == KEY_ESC)
        return 0;

    /* Must match draw_files_door's list geometry, or paging moves by the
       wrong number of rows. */
    int list_h = PANEL_Y + PANEL_H - 2 - (PANEL_Y + 7);
    inputline *field = NULL;

    switch (a->sfocus) {
    case SF_TERMS:   field = &a->f_terms;   break;
    case SF_EXCLUDE: field = &a->f_exclude; break;
    case SF_MINSIZE: field = &a->f_minsize; break;
    default: break;
    }

    if (key == KEY_TAB) {
        a->sfocus = (search_focus)((a->sfocus + 1) % SF_COUNT);
        a->need_redraw = 1;
        return 1;
    }

    if (key == KEY_ENTER) {
        if (a->sfocus == SF_RESULTS) {
            app_queue_selected(a);
            return 1;
        }
        files_start_search(a);
        return 1;
    }

    if (a->sfocus == SF_GROUP) {
        if (key == KEY_LEFT) {
            a->f_group = (search_group)((a->f_group + SEARCH_GROUP_COUNT - 1)
                                        % SEARCH_GROUP_COUNT);
            a->need_redraw = 1;
            return 1;
        }
        if (key == KEY_RIGHT) {
            a->f_group = (search_group)((a->f_group + 1)
                                        % SEARCH_GROUP_COUNT);
            a->need_redraw = 1;
            return 1;
        }
    }

    if (a->sfocus == SF_RESULTS) {
        search_state *ss = &a->hub->search;

        if (key == ' ') {
            search_tag_toggle(ss, a->files.sel);
            /* Advance after tagging: tagging a run of files should not need
               two keys per file. */
            listbox_key(&a->files, KEY_DOWN, list_h);
            a->need_redraw = 1;
            return 1;
        }
        if (key == 's' || key == 'S') {
            a->f_sort = (search_sort)((a->f_sort + 1) % SORT_COUNT);
            search_sort_by(ss, a->f_sort);
            listbox_init(&a->files);
            app_status(a, search_sort_name(a->f_sort));
            a->need_redraw = 1;
            return 1;
        }
        if (key == 'a' || key == 'A') {
            search_tag_all(ss, search_tagged_count(ss) != ss->count);
            a->need_redraw = 1;
            return 1;
        }
        if (listbox_key(&a->files, key, list_h)) {
            a->need_redraw = 1;
            return 1;
        }
        return 0;
    }

    if (field) {
        input_key(field, key);
        a->need_redraw = 1;
        return 1;
    }

    return 0;
}

static void handle_key(app *a, int key)
{
    int chat_h = CHAT_H;

    /* The file areas door owns most keys while a form field has focus,
       otherwise typing a search term would switch doors.
     *
     * "Most" and not "all", and the difference is the whole door: with a
     * field focused and nothing typed into it yet, the digits still pick
     * doors. Without that, walking into door 3 was a trap -- every digit
     * went into the search box, TAB moved between the form's fields
     * instead of to the next door, and the only way out was F1, which
     * nothing on screen mentions. The same rule the chat door already
     * uses: a key is text once there is text.
     */
    if (a->door == DOOR_FILES && a->sfocus != SF_RESULTS &&
        key != KEY_F1 && key != KEY_ESC &&
        !(files_field_empty(a) && key >= '0' && key <= '9')) {
        if (files_key(a, key))
            return;
    }

    /* Door selection and global keys first, but only when they cannot be
       part of a message being typed. */
    if ((a->door != DOOR_CHAT || a->input.len == 0) &&
        !(a->door == DOOR_FILES && a->sfocus != SF_RESULTS
          && !files_field_empty(a))) {
        switch (key) {
        case '1': a->door = DOOR_CHAT;    a->need_redraw = 1; return;
        case '2': a->door = DOOR_WHO;     a->need_redraw = 1; return;
        case '3': a->door = DOOR_FILES;   a->need_redraw = 1; return;
        case '4': a->door = DOOR_XFER;    a->need_redraw = 1; return;
        case '5': a->door = DOOR_SHARE;   a->need_redraw = 1; return;
        case '6': a->door = DOOR_WELCOME; a->need_redraw = 1; return;
        case '7': a->door = DOOR_INFO;    a->need_redraw = 1; return;
        case '8': a->door = DOOR_BROWSE;  a->need_redraw = 1; return;
        case '9': a->door = DOOR_SETUP;   a->need_redraw = 1; return;
        case '0': a->door = DOOR_HUBS;    a->need_redraw = 1; return;
        case 'g': case 'G':
        case 'q': case 'Q':
            /* The bar says G-BYE on every door, so it had better work on
               every door. It used to be refused on the message door --
               the one you land on, and the one ESC now returns you to --
               because a G could be part of something being typed. This
               block is already only reached when the input line is empty,
               so that cannot be the case here.
               Q as well as G: the bar says one and half the boards of the
               period took the other. */
            a->running = 0;
            return;
        default: break;
        }
    }

    switch (key) {
    case KEY_F1:
        a->door = DOOR_INFO;
        a->need_redraw = 1;
        return;
    case KEY_TAB:
        a->door = (door_id)(((int)a->door + 1) % DOOR_COUNT);
        a->need_redraw = 1;
        return;
    case KEY_PGUP:
        if (a->door == DOOR_CHAT) {
            sb_scroll(&a->chat, chat_h - 1, chat_h);
            a->need_redraw = 1;
            return;
        }
        if (a->door == DOOR_WELCOME) {
            a->motd_scroll -= PANEL_H - 2;
            if (a->motd_scroll < 0)
                a->motd_scroll = 0;
            a->need_redraw = 1;
            return;
        }
        break;
    case KEY_PGDN:
        if (a->door == DOOR_CHAT) {
            sb_scroll(&a->chat, -(chat_h - 1), chat_h);
            a->need_redraw = 1;
            return;
        }
        if (a->door == DOOR_WELCOME) {
            int page = PANEL_H - 2;
            if (a->motd_scroll + page < a->motd_rows)
                a->motd_scroll += page;
            a->need_redraw = 1;
            return;
        }
        break;
    case KEY_UP:
        if (a->door == DOOR_WELCOME && a->motd_scroll > 0) {
            a->motd_scroll--;
            a->need_redraw = 1;
            return;
        }
        break;
    case KEY_DOWN:
        if (a->door == DOOR_WELCOME &&
            a->motd_scroll + PANEL_H - 2 < a->motd_rows) {
            a->motd_scroll++;
            a->need_redraw = 1;
            return;
        }
        break;
    default:
        break;
    }

    if (a->door == DOOR_FILES) {
        if (files_key(a, key))
            return;

        /* ESC, as a ladder that always ends somewhere useful: out of the
           results and back to the query, then clearing what was typed,
           then out of the door altogether. Pressing it twice therefore
           always leaves, which is what somebody who cannot find the way
           out will try. It used to put focus back on the query and stop
           there -- for ever. */
        if (key == KEY_ESC) {
            if (a->sfocus != SF_TERMS) {
                a->sfocus = SF_TERMS;
            } else if (a->f_terms.len || a->f_exclude.len
                       || a->f_minsize.len) {
                input_clear(&a->f_terms);
                input_clear(&a->f_exclude);
                input_clear(&a->f_minsize);
                app_status(a, "search cleared -- ESC again to leave");
            } else {
                a->door = DOOR_CHAT;
            }
            a->need_redraw = 1;
        }
        return;
    }

    if (a->door == DOOR_XFER) {
        int h = PANEL_Y + PANEL_H - 6;

        listbox_set_count(&a->xfers, xfer_rows(a));
        if (listbox_key(&a->xfers, key, h))
            a->need_redraw = 1;
        return;
    }

    if (a->door == DOOR_BROWSE) {
        browse_key(a, key);
        return;
    }

    if (a->door == DOOR_SETUP) {
        setup_key(a, key);
        return;
    }

    if (a->door == DOOR_HUBS) {
        hubs_key(a, key);
        return;
    }

    if (a->door == DOOR_SHARE) {
        if (key == 'r' || key == 'R') {
            app_share_start(a);
            return;
        }
        if (key == 'h' || key == 'H') {
            app_hash_toggle(a);
            return;
        }
        return;
    }

    if (a->door == DOOR_WHO) {
        int h = PANEL_Y + PANEL_H - 6;

        if (key == 'b' || key == 'B') {
            adc_user *u = adc_userlist_at(&a->hub->users,
                                          (unsigned int)a->who.sel);

            if (u)
                app_browse_start(a, u->sid, u->nick);
            else
                app_status(a, "nobody is selected");
            return;
        }

        /* Sync the row count here, not only while drawing. A key that
           arrives before the first draw of this door would otherwise act on
           a count of zero and move nothing -- which is exactly what the
           keyboard test caught. */
        listbox_set_count(&a->who, (int)a->hub->users.count);

        if (listbox_key(&a->who, key, h))
            a->need_redraw = 1;
        return;
    }

    if (a->door == DOOR_CHAT) {
        if (input_key(&a->input, key)) {
            char line[INPUT_MAX];
            snprintf(line, sizeof line, "%s", a->input.text);
            input_clear(&a->input);
            sb_scroll_end(&a->chat);
            do_command(a, line);
        }
        a->need_redraw = 1;
    }
}

/* ---------------------------------------------------------------- life */

int app_init(app *a, hubconn *h, cfg *conf, const char *theme_name)
{
    memset(a, 0, sizeof *a);

    a->hub = h;
    a->conf = conf;
    a->th = theme_by_name(theme_name);
    a->door = DOOR_CHAT;
    a->running = 1;
    a->need_redraw = 1;

    /* The display opens before the cell buffer, because the buffer has to
       be the size the display actually gave us: asking for 50 rows does
       not mean getting them, on a terminal window or on a BIOS that will
       not load the 8x8 font. */
    if (vga_open(conf ? conf->rows : SCREEN_H) != 0)
        return -1;

    {
        int w = SCREEN_W, h = SCREEN_H;

        vga_size(&w, &h);
        a->lay = app_layout_for(h);

        if (screen_init(&a->scr, SCREEN_W, h) != 0) {
            vga_close();
            return -1;
        }
    }

    if (share_init(&a->share) != 0)
        return -1;

    if (namemap_init(&a->names) != 0) {
        share_free(&a->share);
        return -1;
    }
    namemap_load(a->names, NAMEMAP_FILE);
    xq_init(&a->xq, a->names);
    listbox_init(&a->xfers);
    listbox_init(&a->setup);
    input_init(&a->setup_input);
    a->editing = -1;
    hublist_init(&a->hubs);
    listbox_init(&a->hubs_box);
    favs_init(&a->favs);
    listbox_init(&a->favs_box);
    input_init(&a->fav_input);
    a->fav_editing = -1;

    /* Favourites open on the favourites, not the public list: the point of
       having them is not having to look through three thousand hubs. */
    a->fav_view = 1;
    favs_load(&a->favs, FAVS_FILE);
    browse_init(&a->browse);
    listbox_init(&a->browse_box);
    a->listener.fd = -1;

    /* Anything a previous run left unfinished. Loaded here rather than at
       login so the transfer door shows it immediately: a user who restarts
       mid-download wants to see that it survived, not wonder. The sources
       come later, from a search for the hash. */
    {
        int restored = xq_load(&a->xq, XQ_QUEUE_FILE);

        if (restored > 0) {
            char line[SB_LINE_MAX];

            snprintf(line, sizeof line,
                     "%d unfinished download%s restored from the queue",
                     restored, restored == 1 ? "" : "s");
            sb_add(&a->chat, SCREEN_W - 2, SB_SYSTEM, NULL, 0, line);
        }
    }

    sb_init(&a->chat);
    input_init(&a->input);
    listbox_init(&a->who);
    listbox_init(&a->files);
    input_init(&a->f_terms);
    input_init(&a->f_exclude);
    input_init(&a->f_minsize);
    a->sfocus = SF_TERMS;
    a->input.enabled = 0;

    kbd_open();
    snd_init(conf ? conf->sound : 1);

    music_begin(a);

    return 0;
}

void app_free(app *a)
{
    /* One last write: a clean exit is the case where the queue file is
       most likely to be read next, and a request abandoned at quit is
       still worth resuming. */
    xq_save(&a->xq, XQ_QUEUE_FILE);

    mus_stop();
    browse_free(&a->browse);
    xq_free(&a->xq);
    if (a->listening)
        tp_listen_close(&a->listener);
    namemap_free(&a->names);
    scan_cancel(&a->scan);
    hashq_free(&a->hq);
    share_free(&a->share);
    snd_shutdown();
    kbd_close();
    vga_close();
    screen_free(&a->scr);
}

int app_tick(app *a, unsigned long now_secs)
{
    int key;
    int guard;

    a->now = now_secs;

    /* The sound queue advances here rather than on a timer hook: a tone
       that outlives the program is worse than one that stutters. */
    snd_tick(snd_millis());

    /* Somebody pressed B on their own node while the share was still
       being scanned. Now that there is something to list, open it. */
    if (a->browse_self_pending && a->share_ready && a->share.count > 0) {
        a->browse_self_pending = 0;
        a->browse_fetching = -1;
        if (browse_self(a, "/"))
            app_status(a, "your own share");
    }

    /* And the music, which is the same argument at a higher data rate:
       mixing a slice here costs less than a hub poll and keeps the card's
       ring ahead of it. */
    mus_tick();

    /* Drain the keyboard, but bounded: holding a key down must not starve
       the network poll. */
    for (guard = 0; guard < 16; guard++) {
        key = kbd_get();
        if (key == KEY_NONE)
            break;
        handle_key(a, key);
        if (!a->running)
            return 1;
    }

    /* The status line expires, so a redraw is needed even with no input. */
    if (a->status[0] && a->now >= a->status_until) {
        a->status[0] = '\0';
        a->need_redraw = 1;
    }

    if (a->need_redraw)
        redraw(a);

    return a->running ? 0 : 1;
}

void app_search(app *a, const char *terms)
{
    const char *p = terms;

    input_clear(&a->f_terms);
    while (p && *p)
        input_key(&a->f_terms, (unsigned char)*p++);

    a->door = DOOR_FILES;
    files_start_search(a);
}

void app_audio_tick(app *a)
{
    (void)a;
    snd_tick(snd_millis());
    mus_tick();
}

void app_splash_tick(app *a, unsigned long now_ms)
{
    /* Twelve frames a second, which is enough for the scroller to read as
       moving and few enough that the splash is not what the machine is
       doing while it logs in. */
    if (now_ms - a->splash_drawn >= 80) {
        a->splash_drawn = now_ms;
        app_splash_draw(a);
    }

    app_audio_tick(a);
}

void app_splash_draw(app *a)
{
    const theme *th = a->th;
    int rows = SCREEN_ROWS;
    int box_x, box_w, box_y, box_h, y;

    screen_clear(&a->scr, th->desktop);
    screen_fill(&a->scr, 0, 0, SCREEN_W, rows, th->desktop_ch,
                th->desktop);

    /* The box is centred on whatever height there is: nineteen rows of
       content at 25, twenty-seven at 50 -- the splash is the one screen
       where filling the display matters more than a fixed layout. */
    /* Wider as well as taller at 50 rows: the big logo is 65 columns and
       the 66-wide box that suits 25 rows would have it writing over both
       borders. */
    box_h = (rows >= 40) ? 24 : 19;
    box_x = (rows >= 40) ? 3 : 7;
    box_w = SCREEN_W - 2 * box_x;
    box_y = (rows - box_h) / 2;
    if (box_y < 1)
        box_y = 1;

    screen_box(&a->scr, box_x, box_y, box_w, box_h, BOX_DOUBLE, th->border);

    y = box_y + 2;
    y = art_logo_fit(&a->scr, box_x + 1, y, box_w - 2, rows,
                     th->text_hi) + 1;

    screen_puts_center(&a->scr, box_x + 1, y++, box_w - 2,
                       "A D C   C L I E N T   F O R   M S - D O S",
                       th->title);
    y++;
    art_rule(&a->scr, box_x + (box_w - 40) / 2, y++, 40, th->border);
    y++;

    screen_puts_center(&a->scr, box_x + 1, y++, box_w - 2,
                       "NODE 1  -  14.4k DUAL STANDARD  -  32-BIT PROTECTED MODE",
                       th->text_dim);

    /* The credits, with whatever is playing: the splash is where a cracktro
       put them, and it is also the first place a user looks to see whether
       the music came up. */
    /* One more boast where there is room for it. At 25 rows the credits
       need that line. */
    if (rows >= 40)
        screen_puts_center(&a->scr, box_x + 1, y++, box_w - 2,
                           "TIGER TREE HASHING  -  NO FLOATING POINT "
                           "REQUIRED", th->text_dim);

    art_credits(&a->scr, box_x + 1, y, box_w - 2,
                mus_playing() ? (mus_title()[0] ? mus_title()
                                                : "a module")
                              : NULL,
                th->text_hi, th->text_dim);
    y += 3;

    /* The greetz line, scrolling. Advanced per draw rather than per second
       so it moves while the hub connection is still being set up. */
    art_scroller(&a->scr, box_x + 1, box_y + box_h - 3, box_w - 2,
                 a->splash_phase++, th->text_hi);

    screen_puts_center(&a->scr, box_x + 1, box_y + box_h - 2, box_w - 2,
                       "CONNECTING  -  PRESS ANY KEY", th->text_hi);

    screen_cursor_hide(&a->scr);
    vga_flush(&a->scr);
}

void app_logoff_draw(app *a)
{
    unsigned long secs = 0;

    if (a->connected_at && a->now > a->connected_at)
        secs = a->now - a->connected_at;

    art_logoff(&a->scr, a->th,
               a->hub->hubname[0] ? a->hub->hubname : a->conf->hub,
               a->hub->nick[0] ? a->hub->nick : a->conf->nick,
               secs, a->nodes_seen);
    vga_flush(&a->scr);

    snd_disconnect();
}

void app_splash_done(app *a)
{
    screen_invalidate(&a->scr);
    a->need_redraw = 1;
}
